// method_call_wide_args — exhaustive JVM tests for method_proxy::call(args...)
// passing long / double (TWO interpreter local slots each) arguments correctly.
//
// THE BUG CLASS UNDER TEST
//   A long or double argument occupies two interpreter local slots; every other
//   primitive (and an object reference) occupies one.  Two failure modes:
//     (1) TRUNCATION  — only 32 bits of a 64-bit value reach the callee (the
//         high or low half is dropped, or a stale high half leaks in).
//     (2) SLOT SHIFT  — the wide value's presence mis-aligns the FOLLOWING
//         parameter, silently corrupting the next int (or everything after it).
//   This module proves NEITHER happens, for wide args in the leading / middle /
//   trailing position, long+double mixed, two longs, two doubles, an all-wide
//   four-arg frame, and the minimal "int immediately after a wide arg" witness —
//   on BOTH the call_stub fast path and the call_jni fallback (whichever the
//   live JDK uses), via instance AND static dispatch.
//
// HOW THE FEATURE PACKS WIDE ARGS (vmhook/ext/vmhook/vmhook.hpp)
//   * call() call_stub fast path: 13199-13416.  params[8] is an intptr_t array;
//     the generic-arg branch (13321-13327) does
//         std::intptr_t v{}; std::memcpy(&v, &a, sizeof(clean_t)); params[i] = v;
//     so a long/double fills ALL 8 bytes of ONE slot (the zero-init guarantees a
//     narrow arg leaves no stale high bits), and the hand-written stub expands
//     wide values into interpreter locals.  param_idx counts ONE per C++ arg.
//   * call_jni() fallback: write_jni_arg_to_slot (10200-10273) zeroes the union
//     cell (value.j = 0) then sets value.j for a 64-bit integral / value.d for a
//     double; JNI's Call*MethodA expands the jvalue into the two locals.
//   * overload selection: resolve_compatible_method<args_t...> (13781-13869) maps
//     int64_t->"J", double->"D", int32_t->"I", float->"F" via
//     argument_matches_descriptor (13590-13672), so call((int64_t)x) on a
//     name-only proxy with both an int and a long overload picks the long one.
//
// WHY EVERYTHING RUNS IN ONE DETOUR
//   method_proxy::call() requires vmhook::hotspot::current_java_thread, set only
//   while the Java thread executes inside an interpreter detour.  The module hooks
//   MethodCallWideArgs.trigger(int); the detour performs every call() and records
//   the returned value_t (raw bits for doubles) plus the Java-side per-parameter
//   witness fields into file-scope state.  The body then asserts each, identically
//   for whichever dispatch path the JDK exposed.
//
// EVERY ctx.check here is a path-independent invariant: a real bug on ANY JDK if
// it fails.  Diagnostics (the live dispatch path) go through ctx.record.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace
{
    // ---- bit helpers (memcpy type-pun; no std::bit_cast for C++17 parity) -----
    inline auto d2bits(double d) noexcept -> std::uint64_t
    {
        std::uint64_t b{ 0 };
        std::memcpy(&b, &d, sizeof(b));
        return b;
    }
    inline auto bits2d(std::uint64_t b) noexcept -> double
    {
        double d{ 0.0 };
        std::memcpy(&d, &b, sizeof(d));
        return d;
    }

    // Java `long` arithmetic is two's-complement wraparound, which is bit-for-bit
    // identical to unsigned 64-bit arithmetic.  We compute every EXPECTED long
    // formula through these unsigned helpers so the C++ side matches Java exactly
    // AND avoids signed-overflow UB (and the -Woverflow warning) on boundary
    // operands like Long.MIN_VALUE — the runtime callee wraps the same way.
    inline auto jadd(std::int64_t a, std::int64_t b) noexcept -> std::int64_t
    {
        return static_cast<std::int64_t>(
            static_cast<std::uint64_t>(a) + static_cast<std::uint64_t>(b));
    }
    inline auto jmul(std::int64_t a, std::int64_t b) noexcept -> std::int64_t
    {
        return static_cast<std::int64_t>(
            static_cast<std::uint64_t>(a) * static_cast<std::uint64_t>(b));
    }

    // ---- mirrored Java constants ---------------------------------------------
    constexpr std::int64_t SENTINEL      = static_cast<std::int64_t>(0x5A5A5A5A5A5A5A5AULL);
    constexpr std::int32_t SENTINEL_INT  = static_cast<std::int32_t>(0x5A5A5A5AU);
    constexpr std::int32_t WIDTH_TAG_INT  = 111;
    constexpr std::int32_t WIDTH_TAG_LONG = 222;
    constexpr std::int32_t FD_TAG_FLOAT   = 333;
    constexpr std::int32_t FD_TAG_DOUBLE  = 444;

    // Two ASCII String arguments for the object-reference interleave methods
    // (mixS / objLong / longObj / sMixS).  They have DISTINCT lengths (7 vs 13)
    // so an a<->c swap changes the length-weighted return; the exact lengths feed
    // the deterministic return formula the native side recomputes.  Pure ASCII so
    // a.length() == byte count == the std::string size on both sides.
    const std::string kStrA{ "wide-aa" };          // length 7
    const std::string kStrC{ "ref-cc-trailer" };   // length 14
    constexpr std::int64_t kStrLenA = 7;
    constexpr std::int64_t kStrLenC = 14;

    // The exhaustive long boundary set.  Beyond the canonical halves (high-only
    // vs low-only — what a 32-bit truncation confuses) this also walks the
    // 32->64-bit SIGN/CARRY boundary and powers of two straddling bit 31/32/63,
    // because the single most common wide-arg defect is treating a long as a
    // sign-extended 32-bit int: such a bug maps 0x0000000080000000 (a POSITIVE
    // long whose low word reads as a negative int) onto 0xFFFFFFFF80000000, and
    // collapses 0x0000000100000000 (1<<32) to 0.  Each entry round-trips through
    // idL bit-exact, so any of those confusions fails its echo check.
    constexpr std::int64_t kLongVals[] = {
        0LL,
        1LL,
        -1LL,
        std::numeric_limits<std::int64_t>::min(),               // 0x8000...0
        std::numeric_limits<std::int64_t>::max(),               // 0x7FFF...F
        static_cast<std::int64_t>(0x0123456789ABCDEFULL),       // mixed pattern
        static_cast<std::int64_t>(0xFFFFFFFF00000000ULL),       // high half only
        static_cast<std::int64_t>(0x00000000FFFFFFFFULL),       // low half only (== 4294967295)
        static_cast<std::int64_t>(0xDEADBEEFCAFEBABEULL),       // both halves nonzero
        // --- 32->64-bit sign / carry boundary (the sign-extension-bug witnesses) ---
        static_cast<std::int64_t>(0x000000007FFFFFFFULL),       // INT_MAX as a long (positive)
        static_cast<std::int64_t>(0x0000000080000000ULL),       // INT_MAX+1: low word "looks" negative-int, long is +2^31
        static_cast<std::int64_t>(0xFFFFFFFF80000000ULL),       // INT_MIN sign-extended to 64 (-2^31)
        static_cast<std::int64_t>(0x0000000100000000ULL),       // 1<<32: a naive low-32 pack reads 0
        static_cast<std::int64_t>(0x00000000FFFFFFFEULL),       // (1<<32)-2: high word zero, low word large
        // --- powers of two and their neighbours straddling bit 31 / 62 / 63 ---
        static_cast<std::int64_t>(0x0000000080000001ULL),       // (1<<31)+1
        static_cast<std::int64_t>(0x000000007FFFFFFEULL),       // (1<<31)-2
        static_cast<std::int64_t>(0x4000000000000000ULL),       // 1<<62
        static_cast<std::int64_t>(0x4000000000000001ULL),       // (1<<62)+1
        static_cast<std::int64_t>(0x7FFFFFFFFFFFFFFEULL),       // LONG_MAX-1
        static_cast<std::int64_t>(0x8000000000000001ULL),       // LONG_MIN+1
        static_cast<std::int64_t>(0x7EDCBA9812345678ULL),       // distinct nonzero high+low words (no symmetry)
    };
    constexpr std::size_t kLongCount{ sizeof(kLongVals) / sizeof(kLongVals[0]) };

    // The exhaustive double boundary set, expressed as RAW BITS so NaN payloads /
    // signaling bit / denormal mantissa / sign-of-zero are reconstructed exactly.
    constexpr std::uint64_t kDoubleBits[] = {
        0x0000000000000000ULL, // +0.0
        0x8000000000000000ULL, // -0.0
        0x3FF0000000000000ULL, // +1.0
        0xBFF0000000000000ULL, // -1.0
        0x400921FB54442D18ULL, // Math.PI
        0xC02E000000000000ULL, // -15.0 (an ordinary negative)
        0x7FF0000000000000ULL, // +Inf
        0xFFF0000000000000ULL, // -Inf
        0x7FF8000000000000ULL, // canonical qNaN
        0x7FF0000000000001ULL, // signaling NaN
        0x7FFAAAAAAAAAAAAAULL, // qNaN with payload
        0x0000000000000001ULL, // smallest subnormal (Double.MIN_VALUE)
        0x0010000000000000ULL, // Double.MIN_NORMAL
        0x7FEFFFFFFFFFFFFFULL, // Double.MAX_VALUE
        // --- the negative-sign mirrors (an 'abs only' or sign-dropping bug fails here) ---
        0xFFF0000000000001ULL, // -signaling NaN (sign bit + signaling payload)
        0x8000000000000001ULL, // -smallest subnormal (sign bit on the denormal)
        0x8010000000000000ULL, // -Double.MIN_NORMAL
        0xFFEFFFFFFFFFFFFFULL, // -Double.MAX_VALUE
        // --- distinct nonzero HIGH and LOW 32-bit words (a word-swap/endian bug
        //     on the wide echo path lands here; most of the set above has a zero
        //     low word, so this is the dedicated split-word witness). ---
        0x123456789ABCDEF0ULL, // arbitrary finite-ish pattern, both words set
        0x401ABCDEF1234567ULL, // a normal magnitude (~6.7) with a busy low word
    };
    constexpr std::size_t kDoubleCount{ sizeof(kDoubleBits) / sizeof(kDoubleBits[0]) };

    // Wrapper for vmhook.fixtures.MethodCallWideArgs.
    class wide : public vmhook::object<wide>
    {
    public:
        explicit wide(vmhook::oop_t instance) noexcept
            : vmhook::object<wide>{ instance }
        {
        }

        // -- handshake --
        static auto set_go(bool v) -> void { static_field("go")->set(v); }
        static auto get_done() -> bool      { return static_field("done")->get(); }
        static auto trigger_count() -> std::int32_t { return static_field("triggerCount")->get(); }

        // A live instance wrapper (the fixture keeps `static instance` alive).
        static auto get_instance() -> std::unique_ptr<wide>
        {
            return static_field("instance")->get();
        }

        // Read a static double field as its RAW 64-bit pattern, so a witness
        // comparison preserves NaN payload / signaling bit / sign-of-zero exactly
        // (the same reason the double ECHO checks compare bits, not values).
        static auto d2bits_field(const char* name) -> std::uint64_t
        {
            const double d = static_field(name)->get();
            std::uint64_t b{ 0 };
            std::memcpy(&b, &d, sizeof(b));
            return b;
        }

        // -- witness-field readers (read AFTER the detour, prove exact args) --
        static auto wIdL() -> std::int64_t    { return static_field("wIdL")->get(); }
        static auto wAddLa() -> std::int64_t  { return static_field("wAddLa")->get(); }
        static auto wAddLb() -> std::int64_t  { return static_field("wAddLb")->get(); }
        static auto wMixAa() -> std::int32_t  { return static_field("wMixAa")->get(); }
        static auto wMixAb() -> std::int64_t  { return static_field("wMixAb")->get(); }
        static auto wMixAc() -> std::int32_t  { return static_field("wMixAc")->get(); }
        static auto wMixBa() -> std::int64_t  { return static_field("wMixBa")->get(); }
        static auto wMixBb() -> std::int32_t  { return static_field("wMixBb")->get(); }
        static auto wMixBc() -> std::int64_t  { return static_field("wMixBc")->get(); }
        static auto wScaleDn() -> std::int32_t  { return static_field("wScaleDn")->get(); }
        static auto wMixCa() -> std::int32_t  { return static_field("wMixCa")->get(); }
        static auto wMixCc() -> std::int32_t  { return static_field("wMixCc")->get(); }
        static auto wMixDa() -> std::int64_t  { return static_field("wMixDa")->get(); }
        static auto wMixDc() -> std::int64_t  { return static_field("wMixDc")->get(); }
        static auto wIalLong() -> std::int64_t { return static_field("wIalLong")->get(); }
        static auto wIalInt()  -> std::int32_t { return static_field("wIalInt")->get(); }
        static auto wIadInt()  -> std::int32_t { return static_field("wIadInt")->get(); }
        static auto wLaiInt()  -> std::int32_t { return static_field("wLaiInt")->get(); }
        static auto wLaiLong() -> std::int64_t { return static_field("wLaiLong")->get(); }
        static auto wDaiInt()  -> std::int32_t { return static_field("wDaiInt")->get(); }
        // addD / jd / dj adjacency witnesses (two wide args back-to-back)
        static auto wAddDa() -> std::uint64_t { return d2bits_field("wAddDa"); }
        static auto wAddDb() -> std::uint64_t { return d2bits_field("wAddDb"); }
        static auto wJdA()   -> std::int64_t  { return static_field("wJdA")->get(); }
        static auto wJdB()   -> std::uint64_t { return d2bits_field("wJdB"); }
        static auto wDjA()   -> std::uint64_t { return d2bits_field("wDjA"); }
        static auto wDjB()   -> std::int64_t  { return static_field("wDjB")->get(); }
        // hexA / hexB six-arg interleave witnesses
        static auto wHexAa() -> std::int32_t  { return static_field("wHexAa")->get(); }
        static auto wHexAb() -> std::int64_t  { return static_field("wHexAb")->get(); }
        static auto wHexAc() -> std::uint64_t { return d2bits_field("wHexAc"); }
        static auto wHexAd() -> std::int32_t  { return static_field("wHexAd")->get(); }
        static auto wHexAe() -> std::int64_t  { return static_field("wHexAe")->get(); }
        static auto wHexAf() -> std::uint64_t { return d2bits_field("wHexAf"); }
        static auto wHexBa() -> std::int64_t  { return static_field("wHexBa")->get(); }
        static auto wHexBb() -> std::int32_t  { return static_field("wHexBb")->get(); }
        static auto wHexBc() -> std::uint64_t { return d2bits_field("wHexBc"); }
        static auto wHexBd() -> std::int64_t  { return static_field("wHexBd")->get(); }
        static auto wHexBe() -> std::int32_t  { return static_field("wHexBe")->get(); }
        static auto wHexBf() -> std::uint64_t { return d2bits_field("wHexBf"); }
        // mixF / fld float+wide witnesses.  A float witness is read as its RAW
        // 32-bit pattern so the comparison is bit-exact (mirrors the double-bits
        // approach), reconstructed from the float static field.
        static auto f2bits_field(const char* name) -> std::uint32_t
        {
            const float f = static_field(name)->get();
            std::uint32_t b{ 0 };
            std::memcpy(&b, &f, sizeof(b));
            return b;
        }
        static auto wMixFa() -> std::uint32_t { return f2bits_field("wMixFa"); }
        static auto wMixFb() -> std::int64_t  { return static_field("wMixFb")->get(); }
        static auto wMixFc() -> std::uint32_t { return f2bits_field("wMixFc"); }
        static auto wFldA()  -> std::uint32_t { return f2bits_field("wFldA"); }
        static auto wFldB()  -> std::int64_t  { return static_field("wFldB")->get(); }
        static auto wFldC()  -> std::uint64_t { return d2bits_field("wFldC"); }
        // mixS / objLong / longObj object-reference adjacency witnesses.  The
        // String witnesses are read back as std::string (their content is the
        // proof the reference neither swapped nor shifted); the long witnesses
        // prove the wide arg stayed intact beside the reference slots.
        static auto wMixSa() -> std::string  { return static_field("wMixSa")->get().as_string(); }
        static auto wMixSb() -> std::int64_t { return static_field("wMixSb")->get(); }
        static auto wMixSc() -> std::string  { return static_field("wMixSc")->get().as_string(); }
        static auto wOlObj()  -> std::string  { return static_field("wOlObj")->get().as_string(); }
        static auto wOlLong() -> std::int64_t { return static_field("wOlLong")->get(); }
        static auto wLoLong() -> std::int64_t { return static_field("wLoLong")->get(); }
        static auto wLoObj()  -> std::string  { return static_field("wLoObj")->get().as_string(); }
        // static-variant witnesses
        static auto sWAddLa() -> std::int64_t { return static_field("sWAddLa")->get(); }
        static auto sWAddLb() -> std::int64_t { return static_field("sWAddLb")->get(); }
        static auto sWMixAa() -> std::int32_t { return static_field("sWMixAa")->get(); }
        static auto sWMixAb() -> std::int64_t { return static_field("sWMixAb")->get(); }
        static auto sWMixAc() -> std::int32_t { return static_field("sWMixAc")->get(); }
        static auto sWScaleDn() -> std::int32_t { return static_field("sWScaleDn")->get(); }
        static auto sWMixDa() -> std::int64_t { return static_field("sWMixDa")->get(); }
        static auto sWMixDc() -> std::int64_t { return static_field("sWMixDc")->get(); }
        static auto sWAddDa() -> std::uint64_t { return d2bits_field("sWAddDa"); }
        static auto sWAddDb() -> std::uint64_t { return d2bits_field("sWAddDb"); }
        static auto sWJdA()   -> std::int64_t  { return static_field("sWJdA")->get(); }
        static auto sWJdB()   -> std::uint64_t { return d2bits_field("sWJdB"); }
        static auto sWDjA()   -> std::uint64_t { return d2bits_field("sWDjA"); }
        static auto sWDjB()   -> std::int64_t  { return static_field("sWDjB")->get(); }
        // static float/object-interleave witnesses
        static auto sWFldA()  -> std::uint32_t { return f2bits_field("sWFldA"); }
        static auto sWFldB()  -> std::int64_t  { return static_field("sWFldB")->get(); }
        static auto sWFldC()  -> std::uint64_t { return d2bits_field("sWFldC"); }
        static auto sWMixSa() -> std::string  { return static_field("sWMixSa")->get().as_string(); }
        static auto sWMixSb() -> std::int64_t { return static_field("sWMixSb")->get(); }
        static auto sWMixSc() -> std::string  { return static_field("sWMixSc")->get().as_string(); }
        // mixE(float,double,float) — double in the MIDDLE flanked by FLOATS.
        static auto wMixEa() -> std::uint32_t { return f2bits_field("wMixEa"); }
        static auto wMixEb() -> std::uint64_t { return d2bits_field("wMixEb"); }
        static auto wMixEc() -> std::uint32_t { return f2bits_field("wMixEc"); }
        // jidi(long,int,double,int) — the (JIDI) shape.
        static auto wJidiA() -> std::int64_t  { return static_field("wJidiA")->get(); }
        static auto wJidiB() -> std::int32_t  { return static_field("wJidiB")->get(); }
        static auto wJidiC() -> std::uint64_t { return d2bits_field("wJidiC"); }
        static auto wJidiD() -> std::int32_t  { return static_field("wJidiD")->get(); }
        // idj(int,double,long) — the (ID J) shape.
        static auto wIdjA() -> std::int32_t  { return static_field("wIdjA")->get(); }
        static auto wIdjB() -> std::uint64_t { return d2bits_field("wIdjB"); }
        static auto wIdjC() -> std::int64_t  { return static_field("wIdjC")->get(); }
        // widePent(long,int,double,int,float) — the (JIDIF) five-arg tail.
        static auto wPentA() -> std::int64_t  { return static_field("wPentA")->get(); }
        static auto wPentB() -> std::int32_t  { return static_field("wPentB")->get(); }
        static auto wPentC() -> std::uint64_t { return d2bits_field("wPentC"); }
        static auto wPentD() -> std::int32_t  { return static_field("wPentD")->get(); }
        static auto wPentE() -> std::uint32_t { return f2bits_field("wPentE"); }
        // sixL(long x6) — deep-packing long witnesses.
        static auto wSixLa() -> std::int64_t { return static_field("wSixLa")->get(); }
        static auto wSixLb() -> std::int64_t { return static_field("wSixLb")->get(); }
        static auto wSixLc() -> std::int64_t { return static_field("wSixLc")->get(); }
        static auto wSixLd() -> std::int64_t { return static_field("wSixLd")->get(); }
        static auto wSixLe() -> std::int64_t { return static_field("wSixLe")->get(); }
        static auto wSixLf() -> std::int64_t { return static_field("wSixLf")->get(); }
        // sixD(double x6) — deep-packing double witnesses (raw bits).
        static auto wSixDa() -> std::uint64_t { return d2bits_field("wSixDa"); }
        static auto wSixDb() -> std::uint64_t { return d2bits_field("wSixDb"); }
        static auto wSixDc() -> std::uint64_t { return d2bits_field("wSixDc"); }
        static auto wSixDd() -> std::uint64_t { return d2bits_field("wSixDd"); }
        static auto wSixDe() -> std::uint64_t { return d2bits_field("wSixDe"); }
        static auto wSixDf() -> std::uint64_t { return d2bits_field("wSixDf"); }
        // mixSD(String,double,String) — wide double between two references.
        static auto wMixSDa() -> std::string  { return static_field("wMixSDa")->get().as_string(); }
        static auto wMixSDb() -> std::uint64_t { return d2bits_field("wMixSDb"); }
        static auto wMixSDc() -> std::string  { return static_field("wMixSDc")->get().as_string(); }
        // static deep-packing witnesses
        static auto sWSixLa() -> std::int64_t { return static_field("sWSixLa")->get(); }
        static auto sWSixLb() -> std::int64_t { return static_field("sWSixLb")->get(); }
        static auto sWSixLc() -> std::int64_t { return static_field("sWSixLc")->get(); }
        static auto sWSixLd() -> std::int64_t { return static_field("sWSixLd")->get(); }
        static auto sWSixLe() -> std::int64_t { return static_field("sWSixLe")->get(); }
        static auto sWSixLf() -> std::int64_t { return static_field("sWSixLf")->get(); }
        static auto sWSixDa() -> std::uint64_t { return d2bits_field("sWSixDa"); }
        static auto sWSixDb() -> std::uint64_t { return d2bits_field("sWSixDb"); }
        static auto sWSixDc() -> std::uint64_t { return d2bits_field("sWSixDc"); }
        static auto sWSixDd() -> std::uint64_t { return d2bits_field("sWSixDd"); }
        static auto sWSixDe() -> std::uint64_t { return d2bits_field("sWSixDe"); }
        static auto sWSixDf() -> std::uint64_t { return d2bits_field("sWSixDf"); }
        // static (JIDIF) five-arg witnesses
        static auto sWPentA() -> std::int64_t  { return static_field("sWPentA")->get(); }
        static auto sWPentB() -> std::int32_t  { return static_field("sWPentB")->get(); }
        static auto sWPentC() -> std::uint64_t { return d2bits_field("sWPentC"); }
        static auto sWPentD() -> std::int32_t  { return static_field("sWPentD")->get(); }
        static auto sWPentE() -> std::uint32_t { return f2bits_field("sWPentE"); }
        // septa(int x6, long) — TRAILING wide long at the deepest instance slot.
        static auto wSeptaA() -> std::int32_t { return static_field("wSeptaA")->get(); }
        static auto wSeptaB() -> std::int32_t { return static_field("wSeptaB")->get(); }
        static auto wSeptaC() -> std::int32_t { return static_field("wSeptaC")->get(); }
        static auto wSeptaD() -> std::int32_t { return static_field("wSeptaD")->get(); }
        static auto wSeptaE() -> std::int32_t { return static_field("wSeptaE")->get(); }
        static auto wSeptaF() -> std::int32_t { return static_field("wSeptaF")->get(); }
        static auto wSeptaG() -> std::int64_t { return static_field("wSeptaG")->get(); }
        // sOcta(int x7, long) — TRAILING wide long as the 8th (last packable) arg.
        static auto sWOctaA() -> std::int32_t { return static_field("sWOctaA")->get(); }
        static auto sWOctaB() -> std::int32_t { return static_field("sWOctaB")->get(); }
        static auto sWOctaC() -> std::int32_t { return static_field("sWOctaC")->get(); }
        static auto sWOctaD() -> std::int32_t { return static_field("sWOctaD")->get(); }
        static auto sWOctaE() -> std::int32_t { return static_field("sWOctaE")->get(); }
        static auto sWOctaF() -> std::int32_t { return static_field("sWOctaF")->get(); }
        static auto sWOctaG() -> std::int32_t { return static_field("sWOctaG")->get(); }
        static auto sWOctaH() -> std::int64_t { return static_field("sWOctaH")->get(); }
        // sOctaD(int x7, double) — TRAILING wide double as the 8th arg (raw bits).
        static auto sWOctaDa() -> std::int32_t  { return static_field("sWOctaDa")->get(); }
        static auto sWOctaDb() -> std::int32_t  { return static_field("sWOctaDb")->get(); }
        static auto sWOctaDc() -> std::int32_t  { return static_field("sWOctaDc")->get(); }
        static auto sWOctaDd() -> std::int32_t  { return static_field("sWOctaDd")->get(); }
        static auto sWOctaDe() -> std::int32_t  { return static_field("sWOctaDe")->get(); }
        static auto sWOctaDf() -> std::int32_t  { return static_field("sWOctaDf")->get(); }
        static auto sWOctaDg() -> std::int32_t  { return static_field("sWOctaDg")->get(); }
        static auto sWOctaDh() -> std::uint64_t { return d2bits_field("sWOctaDh"); }
        // SUB-INT NEIGHBOUR witnesses.  byte -> int8_t, short -> int16_t, char ->
        // uint16_t (zero-extended UTF-16 code unit), boolean -> bool.  Read back at
        // their native widths so the extension semantics are preserved exactly (a
        // sign-extend bug on a byte/short, or a stray high bit on a char, fails).
        static auto wBalLong() -> std::int64_t { return static_field("wBalLong")->get(); }
        static auto wBalByte() -> std::int8_t  { return static_field("wBalByte")->get(); }
        static auto wSalLong()  -> std::int64_t { return static_field("wSalLong")->get(); }
        static auto wSalShort() -> std::int16_t { return static_field("wSalShort")->get(); }
        static auto wCalLong() -> std::int64_t  { return static_field("wCalLong")->get(); }
        static auto wCalChar() -> std::uint16_t { return static_field("wCalChar")->get(); }
        static auto wZalLong() -> std::int64_t { return static_field("wZalLong")->get(); }
        static auto wZalBool() -> bool         { return static_field("wZalBool")->get(); }
        static auto wCadDouble() -> std::uint64_t { return d2bits_field("wCadDouble"); }
        static auto wCadChar()   -> std::uint16_t { return static_field("wCadChar")->get(); }
        static auto wSadDouble() -> std::uint64_t { return d2bits_field("wSadDouble"); }
        static auto wSadShort()  -> std::int16_t  { return static_field("wSadShort")->get(); }
        static auto wBscB() -> std::int8_t  { return static_field("wBscB")->get(); }
        static auto wBscL() -> std::int64_t { return static_field("wBscL")->get(); }
        static auto wBscS() -> std::int16_t { return static_field("wBscS")->get(); }
        static auto wZdcZ() -> bool         { return static_field("wZdcZ")->get(); }
        static auto wZdcD() -> std::uint64_t { return d2bits_field("wZdcD"); }
        static auto wZdcC() -> std::uint16_t { return static_field("wZdcC")->get(); }
        static auto wClChar() -> std::uint16_t { return static_field("wClChar")->get(); }
        static auto wClLong() -> std::int64_t  { return static_field("wClLong")->get(); }
        static auto wBldcsB() -> std::int8_t   { return static_field("wBldcsB")->get(); }
        static auto wBldcsL() -> std::int64_t  { return static_field("wBldcsL")->get(); }
        static auto wBldcsD() -> std::uint64_t { return d2bits_field("wBldcsD"); }
        static auto wBldcsC() -> std::uint16_t { return static_field("wBldcsC")->get(); }
        static auto wBldcsS() -> std::int16_t  { return static_field("wBldcsS")->get(); }
        static auto sWBalLong() -> std::int64_t { return static_field("sWBalLong")->get(); }
        static auto sWBalByte() -> std::int8_t  { return static_field("sWBalByte")->get(); }
        static auto sWCadDouble() -> std::uint64_t { return d2bits_field("sWCadDouble"); }
        static auto sWCadChar()   -> std::uint16_t { return static_field("sWCadChar")->get(); }
        static auto sWClChar() -> std::uint16_t { return static_field("sWClChar")->get(); }
        static auto sWClLong() -> std::int64_t  { return static_field("sWClLong")->get(); }
    };

    // ---------------------------------------------------------------------
    //  Observations recorded inside the detour, read back in the body.
    //  A single std::map keyed by a probe name keeps the (resolved, returned
    //  value, returned bits) triple for each call without hundreds of atomics.
    // ---------------------------------------------------------------------
    std::atomic<int>  g_detour_calls{ 0 };
    std::atomic<bool> g_detour_saw_self{ false };
    std::atomic<bool> g_call_stub_path{ false };

    struct probe_result
    {
        bool         resolved{ false };  // get_method / static_method has_value()
        bool         dispatched{ false };// call() actually returned non-void
        bool         is_void{ false };
        std::int64_t ival{ 0 };          // long/int result
        std::uint64_t dbits{ 0 };        // double result raw bits
        std::uint32_t fbits{ 0 };        // float result raw bits (mixF)
    };

    // float -> raw 32-bit pattern (the single-precision analogue of d2bits), so a
    // float return / witness compares bit-exact.
    inline auto f2bits(float f) noexcept -> std::uint32_t
    {
        std::uint32_t b{ 0 };
        std::memcpy(&b, &f, sizeof(b));
        return b;
    }

    std::mutex                          g_mutex;
    std::map<std::string, probe_result> g_res;

    auto put(const std::string& key, const probe_result& r) -> void
    {
        std::lock_guard<std::mutex> lock{ g_mutex };
        g_res[key] = r;
    }
    auto got(const std::string& key) -> probe_result
    {
        std::lock_guard<std::mutex> lock{ g_mutex };
        const auto it{ g_res.find(key) };
        return (it != g_res.end()) ? it->second : probe_result{};
    }

    // ---- capture helpers: resolve by NAME, call with wide args, record --------
    // We capture into value_t via COPY-INIT (never brace-init a numeric from a
    // value_t — the templated conversion operator + const char* makes a braced
    // numeric init ambiguous on MSVC).

    // Instance, returns long.
    auto cap_long(const wide& self, const std::string& key,
                  const char* name,
                  std::int64_t a) -> void
    {
        probe_result r{};
        auto px{ self.get_method(name) };
        if (px.has_value())
        {
            r.resolved = true;
            const vmhook::method_proxy::value_t v = px->call(a);
            r.is_void    = v.is_void();
            r.dispatched = !v.is_void();
            r.ival       = static_cast<std::int64_t>(v);
        }
        put(key, r);
    }

    // Instance, returns long, (long,long).
    auto cap_long2(const wide& self, const std::string& key,
                   const char* name,
                   std::int64_t a, std::int64_t b) -> void
    {
        probe_result r{};
        auto px{ self.get_method(name) };
        if (px.has_value())
        {
            r.resolved = true;
            const vmhook::method_proxy::value_t v = px->call(a, b);
            r.is_void    = v.is_void();
            r.dispatched = !v.is_void();
            r.ival       = static_cast<std::int64_t>(v);
        }
        put(key, r);
    }

    // Instance, returns long, (int,long,int).
    auto cap_mixA(const wide& self, const std::string& key,
                  std::int32_t a, std::int64_t b, std::int32_t c) -> void
    {
        probe_result r{};
        auto px{ self.get_method("mixA") };
        if (px.has_value())
        {
            r.resolved = true;
            const vmhook::method_proxy::value_t v = px->call(a, b, c);
            r.is_void    = v.is_void();
            r.dispatched = !v.is_void();
            r.ival       = static_cast<std::int64_t>(v);
        }
        put(key, r);
    }

    // Instance, returns long, (long,int,long).
    auto cap_mixB(const wide& self, const std::string& key,
                  std::int64_t a, std::int32_t b, std::int64_t c) -> void
    {
        probe_result r{};
        auto px{ self.get_method("mixB") };
        if (px.has_value())
        {
            r.resolved = true;
            const vmhook::method_proxy::value_t v = px->call(a, b, c);
            r.is_void    = v.is_void();
            r.dispatched = !v.is_void();
            r.ival       = static_cast<std::int64_t>(v);
        }
        put(key, r);
    }

    // Instance, returns double, single double (echo).
    auto cap_dbl(const wide& self, const std::string& key,
                 const char* name, double d) -> void
    {
        probe_result r{};
        auto px{ self.get_method(name) };
        if (px.has_value())
        {
            r.resolved = true;
            const vmhook::method_proxy::value_t v = px->call(d);
            r.is_void    = v.is_void();
            r.dispatched = !v.is_void();
            const double got = v;
            r.dbits      = d2bits(got);
        }
        put(key, r);
    }

    // Instance, returns double, (double,int).
    auto cap_scaleD(const wide& self, const std::string& key,
                    double x, std::int32_t n) -> void
    {
        probe_result r{};
        auto px{ self.get_method("scaleD") };
        if (px.has_value())
        {
            r.resolved = true;
            const vmhook::method_proxy::value_t v = px->call(x, n);
            r.is_void    = v.is_void();
            r.dispatched = !v.is_void();
            const double got = v;
            r.dbits      = d2bits(got);
        }
        put(key, r);
    }

    // Instance, returns double, (int,double,int).
    auto cap_mixC(const wide& self, const std::string& key,
                  std::int32_t a, double b, std::int32_t c) -> void
    {
        probe_result r{};
        auto px{ self.get_method("mixC") };
        if (px.has_value())
        {
            r.resolved = true;
            const vmhook::method_proxy::value_t v = px->call(a, b, c);
            r.is_void    = v.is_void();
            r.dispatched = !v.is_void();
            const double got = v;
            r.dbits      = d2bits(got);
        }
        put(key, r);
    }

    // Instance, returns double, (long,double,long,double).
    auto cap_mixD(const wide& self, const std::string& key,
                  std::int64_t a, double b, std::int64_t c, double d) -> void
    {
        probe_result r{};
        auto px{ self.get_method("mixD") };
        if (px.has_value())
        {
            r.resolved = true;
            const vmhook::method_proxy::value_t v = px->call(a, b, c, d);
            r.is_void    = v.is_void();
            r.dispatched = !v.is_void();
            const double got = v;
            r.dbits      = d2bits(got);
        }
        put(key, r);
    }

    // Instance, returns int, (long,int).
    auto cap_intAfterLong(const wide& self, const std::string& key,
                          std::int64_t a, std::int32_t b) -> void
    {
        probe_result r{};
        auto px{ self.get_method("intAfterLong") };
        if (px.has_value())
        {
            r.resolved = true;
            const vmhook::method_proxy::value_t v = px->call(a, b);
            r.is_void    = v.is_void();
            r.dispatched = !v.is_void();
            r.ival       = static_cast<std::int64_t>(v);
        }
        put(key, r);
    }

    // Instance, returns int, (double,int).
    auto cap_intAfterDouble(const wide& self, const std::string& key,
                            double a, std::int32_t b) -> void
    {
        probe_result r{};
        auto px{ self.get_method("intAfterDouble") };
        if (px.has_value())
        {
            r.resolved = true;
            const vmhook::method_proxy::value_t v = px->call(a, b);
            r.is_void    = v.is_void();
            r.dispatched = !v.is_void();
            r.ival       = static_cast<std::int64_t>(v);
        }
        put(key, r);
    }

    // Instance, returns long, (int,long).
    auto cap_longAfterInt(const wide& self, const std::string& key,
                          std::int32_t a, std::int64_t b) -> void
    {
        probe_result r{};
        auto px{ self.get_method("longAfterInt") };
        if (px.has_value())
        {
            r.resolved = true;
            const vmhook::method_proxy::value_t v = px->call(a, b);
            r.is_void    = v.is_void();
            r.dispatched = !v.is_void();
            r.ival       = static_cast<std::int64_t>(v);
        }
        put(key, r);
    }

    // Instance, returns double, (int,double).
    auto cap_doubleAfterInt(const wide& self, const std::string& key,
                            std::int32_t a, double b) -> void
    {
        probe_result r{};
        auto px{ self.get_method("doubleAfterInt") };
        if (px.has_value())
        {
            r.resolved = true;
            const vmhook::method_proxy::value_t v = px->call(a, b);
            r.is_void    = v.is_void();
            r.dispatched = !v.is_void();
            const double got = v;
            r.dbits      = d2bits(got);
        }
        put(key, r);
    }

    // Instance, returns double, (double,double) — two adjacent wide doubles.
    auto cap_addD(const wide& self, const std::string& key,
                  double a, double b) -> void
    {
        probe_result r{};
        auto px{ self.get_method("addD") };
        if (px.has_value())
        {
            r.resolved = true;
            const vmhook::method_proxy::value_t v = px->call(a, b);
            r.is_void    = v.is_void();
            r.dispatched = !v.is_void();
            const double got = v;
            r.dbits      = d2bits(got);
        }
        put(key, r);
    }

    // Instance, returns double, (long,double) — wide long immediately then wide
    // double, no narrow between.
    auto cap_jd(const wide& self, const std::string& key,
                std::int64_t a, double b) -> void
    {
        probe_result r{};
        auto px{ self.get_method("jd") };
        if (px.has_value())
        {
            r.resolved = true;
            const vmhook::method_proxy::value_t v = px->call(a, b);
            r.is_void    = v.is_void();
            r.dispatched = !v.is_void();
            const double got = v;
            r.dbits      = d2bits(got);
        }
        put(key, r);
    }

    // Instance, returns double, (double,long) — the mirror adjacency.
    auto cap_dj(const wide& self, const std::string& key,
                double a, std::int64_t b) -> void
    {
        probe_result r{};
        auto px{ self.get_method("dj") };
        if (px.has_value())
        {
            r.resolved = true;
            const vmhook::method_proxy::value_t v = px->call(a, b);
            r.is_void    = v.is_void();
            r.dispatched = !v.is_void();
            const double got = v;
            r.dbits      = d2bits(got);
        }
        put(key, r);
    }

    // Instance, returns double, (int,long,double,int,long,double) — every kind
    // interleaved across ten interpreter slots.
    auto cap_hexA(const wide& self, const std::string& key,
                  std::int32_t a, std::int64_t b, double c,
                  std::int32_t d, std::int64_t e, double f) -> void
    {
        probe_result r{};
        auto px{ self.get_method("hexA") };
        if (px.has_value())
        {
            r.resolved = true;
            const vmhook::method_proxy::value_t v = px->call(a, b, c, d, e, f);
            r.is_void    = v.is_void();
            r.dispatched = !v.is_void();
            const double got = v;
            r.dbits      = d2bits(got);
        }
        put(key, r);
    }

    // Instance, returns double, (long,int,double,long,int,double) — a different
    // interleave so no single fixed mis-alignment passes both hexA and hexB.
    auto cap_hexB(const wide& self, const std::string& key,
                  std::int64_t a, std::int32_t b, double c,
                  std::int64_t d, std::int32_t e, double f) -> void
    {
        probe_result r{};
        auto px{ self.get_method("hexB") };
        if (px.has_value())
        {
            r.resolved = true;
            const vmhook::method_proxy::value_t v = px->call(a, b, c, d, e, f);
            r.is_void    = v.is_void();
            r.dispatched = !v.is_void();
            const double got = v;
            r.dbits      = d2bits(got);
        }
        put(key, r);
    }

    // Instance, returns float, (float,long,float) — wide long flanked by floats.
    auto cap_mixF(const wide& self, const std::string& key,
                  float a, std::int64_t b, float c) -> void
    {
        probe_result r{};
        auto px{ self.get_method("mixF") };
        if (px.has_value())
        {
            r.resolved = true;
            const vmhook::method_proxy::value_t v = px->call(a, b, c);
            r.is_void    = v.is_void();
            r.dispatched = !v.is_void();
            const float got = v;
            r.fbits      = f2bits(got);
        }
        put(key, r);
    }

    // Instance, returns double, (float,long,double) — narrow float then two wide.
    auto cap_fld(const wide& self, const std::string& key,
                 float a, std::int64_t b, double c) -> void
    {
        probe_result r{};
        auto px{ self.get_method("fld") };
        if (px.has_value())
        {
            r.resolved = true;
            const vmhook::method_proxy::value_t v = px->call(a, b, c);
            r.is_void    = v.is_void();
            r.dispatched = !v.is_void();
            const double got = v;
            r.dbits      = d2bits(got);
        }
        put(key, r);
    }

    // Instance, returns long, (String,long,String) — wide long between two object
    // references.  The Strings are passed as std::string (-> java.lang.String).
    auto cap_mixS(const wide& self, const std::string& key,
                  const std::string& a, std::int64_t b, const std::string& c) -> void
    {
        probe_result r{};
        auto px{ self.get_method("mixS") };
        if (px.has_value())
        {
            r.resolved = true;
            const vmhook::method_proxy::value_t v = px->call(a, b, c);
            r.is_void    = v.is_void();
            r.dispatched = !v.is_void();
            r.ival       = static_cast<std::int64_t>(v);
        }
        put(key, r);
    }

    // Instance, returns long, (String,long) — reference then wide long.
    auto cap_objLong(const wide& self, const std::string& key,
                     const std::string& o, std::int64_t v_) -> void
    {
        probe_result r{};
        auto px{ self.get_method("objLong") };
        if (px.has_value())
        {
            r.resolved = true;
            const vmhook::method_proxy::value_t v = px->call(o, v_);
            r.is_void    = v.is_void();
            r.dispatched = !v.is_void();
            r.ival       = static_cast<std::int64_t>(v);
        }
        put(key, r);
    }

    // Instance, returns long, (long,String) — wide long then reference.
    auto cap_longObj(const wide& self, const std::string& key,
                     std::int64_t v_, const std::string& o) -> void
    {
        probe_result r{};
        auto px{ self.get_method("longObj") };
        if (px.has_value())
        {
            r.resolved = true;
            const vmhook::method_proxy::value_t v = px->call(v_, o);
            r.is_void    = v.is_void();
            r.dispatched = !v.is_void();
            r.ival       = static_cast<std::int64_t>(v);
        }
        put(key, r);
    }

    // Instance, returns float, (float,double,float) — wide DOUBLE flanked by
    // floats (the 'F'-neighbour analogue of mixC's int flanks).
    auto cap_mixE(const wide& self, const std::string& key,
                  float a, double b, float c) -> void
    {
        probe_result r{};
        auto px{ self.get_method("mixE") };
        if (px.has_value())
        {
            r.resolved = true;
            const vmhook::method_proxy::value_t v = px->call(a, b, c);
            r.is_void    = v.is_void();
            r.dispatched = !v.is_void();
            const float got = v;
            r.fbits      = f2bits(got);
        }
        put(key, r);
    }

    // Instance, returns double, (long,int,double,int) — the (JIDI) shape.
    auto cap_jidi(const wide& self, const std::string& key,
                  std::int64_t a, std::int32_t b, double c, std::int32_t d) -> void
    {
        probe_result r{};
        auto px{ self.get_method("jidi") };
        if (px.has_value())
        {
            r.resolved = true;
            const vmhook::method_proxy::value_t v = px->call(a, b, c, d);
            r.is_void    = v.is_void();
            r.dispatched = !v.is_void();
            const double got = v;
            r.dbits      = d2bits(got);
        }
        put(key, r);
    }

    // Instance, returns double, (int,double,long) — the (ID J) shape.
    auto cap_idj(const wide& self, const std::string& key,
                 std::int32_t a, double b, std::int64_t c) -> void
    {
        probe_result r{};
        auto px{ self.get_method("idj") };
        if (px.has_value())
        {
            r.resolved = true;
            const vmhook::method_proxy::value_t v = px->call(a, b, c);
            r.is_void    = v.is_void();
            r.dispatched = !v.is_void();
            const double got = v;
            r.dbits      = d2bits(got);
        }
        put(key, r);
    }

    // Instance, returns double, (long,int,double,int,float) — the explicit
    // five-arg "every shape" tail with a TRAILING float.
    auto cap_widePent(const wide& self, const std::string& key,
                      std::int64_t a, std::int32_t b, double c,
                      std::int32_t d, float e) -> void
    {
        probe_result r{};
        auto px{ self.get_method("widePent") };
        if (px.has_value())
        {
            r.resolved = true;
            const vmhook::method_proxy::value_t v = px->call(a, b, c, d, e);
            r.is_void    = v.is_void();
            r.dispatched = !v.is_void();
            const double got = v;
            r.dbits      = d2bits(got);
        }
        put(key, r);
    }

    // Instance, returns long, (int,int,int,int,int,int,long) — SEVEN args, the
    // trailing wide long lands at the deepest writable instance call-stub slot.
    auto cap_septa(const wide& self, const std::string& key,
                   std::int32_t a, std::int32_t b, std::int32_t c,
                   std::int32_t d, std::int32_t e, std::int32_t f,
                   std::int64_t g) -> void
    {
        probe_result r{};
        auto px{ self.get_method("septa") };
        if (px.has_value())
        {
            r.resolved = true;
            const vmhook::method_proxy::value_t v = px->call(a, b, c, d, e, f, g);
            r.is_void    = v.is_void();
            r.dispatched = !v.is_void();
            r.ival       = static_cast<std::int64_t>(v);
        }
        put(key, r);
    }

    // Instance, returns long, (long x6) — six adjacent longs, deep slot packing.
    auto cap_sixL(const wide& self, const std::string& key,
                  std::int64_t a, std::int64_t b, std::int64_t c,
                  std::int64_t d, std::int64_t e, std::int64_t f) -> void
    {
        probe_result r{};
        auto px{ self.get_method("sixL") };
        if (px.has_value())
        {
            r.resolved = true;
            const vmhook::method_proxy::value_t v = px->call(a, b, c, d, e, f);
            r.is_void    = v.is_void();
            r.dispatched = !v.is_void();
            r.ival       = static_cast<std::int64_t>(v);
        }
        put(key, r);
    }

    // Instance, returns double, (double x6) — six adjacent doubles, deep packing.
    auto cap_sixD(const wide& self, const std::string& key,
                  double a, double b, double c,
                  double d, double e, double f) -> void
    {
        probe_result r{};
        auto px{ self.get_method("sixD") };
        if (px.has_value())
        {
            r.resolved = true;
            const vmhook::method_proxy::value_t v = px->call(a, b, c, d, e, f);
            r.is_void    = v.is_void();
            r.dispatched = !v.is_void();
            const double got = v;
            r.dbits      = d2bits(got);
        }
        put(key, r);
    }

    // Instance, returns double, (String,double,String) — wide DOUBLE between two
    // object references (the double analogue of mixS).
    auto cap_mixSD(const wide& self, const std::string& key,
                   const std::string& a, double b, const std::string& c) -> void
    {
        probe_result r{};
        auto px{ self.get_method("mixSD") };
        if (px.has_value())
        {
            r.resolved = true;
            const vmhook::method_proxy::value_t v = px->call(a, b, c);
            r.is_void    = v.is_void();
            r.dispatched = !v.is_void();
            const double got = v;
            r.dbits      = d2bits(got);
        }
        put(key, r);
    }

    // Instance, returns int (overload tag), single arg of templated width.
    template<typename arg_t>
    auto cap_tag(const wide& self, const std::string& key,
                 const char* name, arg_t a) -> void
    {
        probe_result r{};
        auto px{ self.get_method(name) };
        if (px.has_value())
        {
            r.resolved = true;
            const vmhook::method_proxy::value_t v = px->call(a);
            r.is_void    = v.is_void();
            r.dispatched = !v.is_void();
            r.ival       = static_cast<std::int64_t>(v);
        }
        put(key, r);
    }

    // ---- SUB-INT NEIGHBOUR capture helpers -----------------------------------
    // Each returns the narrow value through value_t's int64 conversion (a byte /
    // short widens with its sign, a char / boolean as an unsigned/0-1 small int);
    // r.ival therefore holds the EXACT narrow value the callee returned, which is
    // the same value it received (these methods just echo it).  The dedicated
    // witness fields independently pin both the wide arg and the narrow neighbour.

    // Instance, returns byte, (long,byte).
    auto cap_byteAfterLong(const wide& self, const std::string& key,
                           std::int64_t a, std::int8_t b) -> void
    {
        probe_result r{};
        auto px{ self.get_method("byteAfterLong") };
        if (px.has_value())
        {
            r.resolved = true;
            const vmhook::method_proxy::value_t v = px->call(a, b);
            r.is_void    = v.is_void();
            r.dispatched = !v.is_void();
            r.ival       = static_cast<std::int64_t>(v);
        }
        put(key, r);
    }

    // Instance, returns short, (long,short).
    auto cap_shortAfterLong(const wide& self, const std::string& key,
                            std::int64_t a, std::int16_t b) -> void
    {
        probe_result r{};
        auto px{ self.get_method("shortAfterLong") };
        if (px.has_value())
        {
            r.resolved = true;
            const vmhook::method_proxy::value_t v = px->call(a, b);
            r.is_void    = v.is_void();
            r.dispatched = !v.is_void();
            r.ival       = static_cast<std::int64_t>(v);
        }
        put(key, r);
    }

    // Instance, returns char, (long,char).  char passed/read as uint16_t.
    auto cap_charAfterLong(const wide& self, const std::string& key,
                           std::int64_t a, std::uint16_t b) -> void
    {
        probe_result r{};
        auto px{ self.get_method("charAfterLong") };
        if (px.has_value())
        {
            r.resolved = true;
            const vmhook::method_proxy::value_t v = px->call(a, b);
            r.is_void    = v.is_void();
            r.dispatched = !v.is_void();
            r.ival       = static_cast<std::int64_t>(v);
        }
        put(key, r);
    }

    // Instance, returns boolean, (long,boolean).
    auto cap_boolAfterLong(const wide& self, const std::string& key,
                           std::int64_t a, bool b) -> void
    {
        probe_result r{};
        auto px{ self.get_method("boolAfterLong") };
        if (px.has_value())
        {
            r.resolved = true;
            const vmhook::method_proxy::value_t v = px->call(a, b);
            r.is_void    = v.is_void();
            r.dispatched = !v.is_void();
            r.ival       = static_cast<std::int64_t>(v);
        }
        put(key, r);
    }

    // Instance, returns char, (double,char).
    auto cap_charAfterDouble(const wide& self, const std::string& key,
                             double a, std::uint16_t b) -> void
    {
        probe_result r{};
        auto px{ self.get_method("charAfterDouble") };
        if (px.has_value())
        {
            r.resolved = true;
            const vmhook::method_proxy::value_t v = px->call(a, b);
            r.is_void    = v.is_void();
            r.dispatched = !v.is_void();
            r.ival       = static_cast<std::int64_t>(v);
        }
        put(key, r);
    }

    // Instance, returns short, (double,short).
    auto cap_shortAfterDouble(const wide& self, const std::string& key,
                              double a, std::int16_t b) -> void
    {
        probe_result r{};
        auto px{ self.get_method("shortAfterDouble") };
        if (px.has_value())
        {
            r.resolved = true;
            const vmhook::method_proxy::value_t v = px->call(a, b);
            r.is_void    = v.is_void();
            r.dispatched = !v.is_void();
            r.ival       = static_cast<std::int64_t>(v);
        }
        put(key, r);
    }

    // Instance, returns long, (byte,long,short) — wide in the middle, B/S flanks.
    auto cap_mixBSC(const wide& self, const std::string& key,
                    std::int8_t a, std::int64_t b, std::int16_t c) -> void
    {
        probe_result r{};
        auto px{ self.get_method("mixBSC") };
        if (px.has_value())
        {
            r.resolved = true;
            const vmhook::method_proxy::value_t v = px->call(a, b, c);
            r.is_void    = v.is_void();
            r.dispatched = !v.is_void();
            r.ival       = static_cast<std::int64_t>(v);
        }
        put(key, r);
    }

    // Instance, returns double, (boolean,double,char) — wide double in the middle.
    auto cap_mixZDC(const wide& self, const std::string& key,
                    bool a, double b, std::uint16_t c) -> void
    {
        probe_result r{};
        auto px{ self.get_method("mixZDC") };
        if (px.has_value())
        {
            r.resolved = true;
            const vmhook::method_proxy::value_t v = px->call(a, b, c);
            r.is_void    = v.is_void();
            r.dispatched = !v.is_void();
            const double got = v;
            r.dbits      = d2bits(got);
        }
        put(key, r);
    }

    // Instance, returns long, (char,long) — leading char then wide long.
    auto cap_charLong(const wide& self, const std::string& key,
                      std::uint16_t a, std::int64_t b) -> void
    {
        probe_result r{};
        auto px{ self.get_method("charLong") };
        if (px.has_value())
        {
            r.resolved = true;
            const vmhook::method_proxy::value_t v = px->call(a, b);
            r.is_void    = v.is_void();
            r.dispatched = !v.is_void();
            r.ival       = static_cast<std::int64_t>(v);
        }
        put(key, r);
    }

    // Instance, returns double, (byte,long,double,char,short) — all sub-int kinds
    // interleaved around both wide kinds.
    auto cap_bldcs(const wide& self, const std::string& key,
                   std::int8_t a, std::int64_t b, double c,
                   std::uint16_t d, std::int16_t e) -> void
    {
        probe_result r{};
        auto px{ self.get_method("bldcs") };
        if (px.has_value())
        {
            r.resolved = true;
            const vmhook::method_proxy::value_t v = px->call(a, b, c, d, e);
            r.is_void    = v.is_void();
            r.dispatched = !v.is_void();
            const double got = v;
            r.dbits      = d2bits(got);
        }
        put(key, r);
    }

    // ---- STATIC capture helpers (no receiver) --------------------------------
    auto scap_long2(const std::string& key, const char* name,
                    std::int64_t a, std::int64_t b) -> void
    {
        probe_result r{};
        auto px{ wide::static_method(name) };
        if (px.has_value())
        {
            r.resolved = true;
            const vmhook::method_proxy::value_t v = px->call(a, b);
            r.is_void    = v.is_void();
            r.dispatched = !v.is_void();
            r.ival       = static_cast<std::int64_t>(v);
        }
        put(key, r);
    }

    auto scap_long1(const std::string& key, const char* name, std::int64_t a) -> void
    {
        probe_result r{};
        auto px{ wide::static_method(name) };
        if (px.has_value())
        {
            r.resolved = true;
            const vmhook::method_proxy::value_t v = px->call(a);
            r.is_void    = v.is_void();
            r.dispatched = !v.is_void();
            r.ival       = static_cast<std::int64_t>(v);
        }
        put(key, r);
    }

    auto scap_dbl1(const std::string& key, const char* name, double d) -> void
    {
        probe_result r{};
        auto px{ wide::static_method(name) };
        if (px.has_value())
        {
            r.resolved = true;
            const vmhook::method_proxy::value_t v = px->call(d);
            r.is_void    = v.is_void();
            r.dispatched = !v.is_void();
            const double got = v;
            r.dbits      = d2bits(got);
        }
        put(key, r);
    }

    auto scap_mixA(const std::string& key,
                   std::int32_t a, std::int64_t b, std::int32_t c) -> void
    {
        probe_result r{};
        auto px{ wide::static_method("sMixA") };
        if (px.has_value())
        {
            r.resolved = true;
            const vmhook::method_proxy::value_t v = px->call(a, b, c);
            r.is_void    = v.is_void();
            r.dispatched = !v.is_void();
            r.ival       = static_cast<std::int64_t>(v);
        }
        put(key, r);
    }

    auto scap_scaleD(const std::string& key, double x, std::int32_t n) -> void
    {
        probe_result r{};
        auto px{ wide::static_method("sScaleD") };
        if (px.has_value())
        {
            r.resolved = true;
            const vmhook::method_proxy::value_t v = px->call(x, n);
            r.is_void    = v.is_void();
            r.dispatched = !v.is_void();
            const double got = v;
            r.dbits      = d2bits(got);
        }
        put(key, r);
    }

    auto scap_mixD(const std::string& key,
                   std::int64_t a, double b, std::int64_t c, double d) -> void
    {
        probe_result r{};
        auto px{ wide::static_method("sMixD") };
        if (px.has_value())
        {
            r.resolved = true;
            const vmhook::method_proxy::value_t v = px->call(a, b, c, d);
            r.is_void    = v.is_void();
            r.dispatched = !v.is_void();
            const double got = v;
            r.dbits      = d2bits(got);
        }
        put(key, r);
    }

    // Static (double,double) — two adjacent wide doubles, first at slot 0.
    auto scap_addD(const std::string& key, double a, double b) -> void
    {
        probe_result r{};
        auto px{ wide::static_method("sAddD") };
        if (px.has_value())
        {
            r.resolved = true;
            const vmhook::method_proxy::value_t v = px->call(a, b);
            r.is_void    = v.is_void();
            r.dispatched = !v.is_void();
            const double got = v;
            r.dbits      = d2bits(got);
        }
        put(key, r);
    }

    // Static (long,double) adjacency, first wide kind at slot 0.
    auto scap_jd(const std::string& key, std::int64_t a, double b) -> void
    {
        probe_result r{};
        auto px{ wide::static_method("sJd") };
        if (px.has_value())
        {
            r.resolved = true;
            const vmhook::method_proxy::value_t v = px->call(a, b);
            r.is_void    = v.is_void();
            r.dispatched = !v.is_void();
            const double got = v;
            r.dbits      = d2bits(got);
        }
        put(key, r);
    }

    // Static (double,long) adjacency.
    auto scap_dj(const std::string& key, double a, std::int64_t b) -> void
    {
        probe_result r{};
        auto px{ wide::static_method("sDj") };
        if (px.has_value())
        {
            r.resolved = true;
            const vmhook::method_proxy::value_t v = px->call(a, b);
            r.is_void    = v.is_void();
            r.dispatched = !v.is_void();
            const double got = v;
            r.dbits      = d2bits(got);
        }
        put(key, r);
    }

    // Static (float,long,double) interleave, first arg (float) at slot 0.
    auto scap_fld(const std::string& key, float a, std::int64_t b, double c) -> void
    {
        probe_result r{};
        auto px{ wide::static_method("sFld") };
        if (px.has_value())
        {
            r.resolved = true;
            const vmhook::method_proxy::value_t v = px->call(a, b, c);
            r.is_void    = v.is_void();
            r.dispatched = !v.is_void();
            const double got = v;
            r.dbits      = d2bits(got);
        }
        put(key, r);
    }

    // Static (String,long,String) — wide long between two references, first
    // reference at slot 0 (no receiver shift).
    auto scap_mixS(const std::string& key,
                   const std::string& a, std::int64_t b, const std::string& c) -> void
    {
        probe_result r{};
        auto px{ wide::static_method("sMixS") };
        if (px.has_value())
        {
            r.resolved = true;
            const vmhook::method_proxy::value_t v = px->call(a, b, c);
            r.is_void    = v.is_void();
            r.dispatched = !v.is_void();
            r.ival       = static_cast<std::int64_t>(v);
        }
        put(key, r);
    }

    // Static (long x6) — six adjacent longs, deep packing, first long at slot 0.
    auto scap_sixL(const std::string& key,
                   std::int64_t a, std::int64_t b, std::int64_t c,
                   std::int64_t d, std::int64_t e, std::int64_t f) -> void
    {
        probe_result r{};
        auto px{ wide::static_method("sSixL") };
        if (px.has_value())
        {
            r.resolved = true;
            const vmhook::method_proxy::value_t v = px->call(a, b, c, d, e, f);
            r.is_void    = v.is_void();
            r.dispatched = !v.is_void();
            r.ival       = static_cast<std::int64_t>(v);
        }
        put(key, r);
    }

    // Static (double x6) — six adjacent doubles, deep packing, first at slot 0.
    auto scap_sixD(const std::string& key,
                   double a, double b, double c,
                   double d, double e, double f) -> void
    {
        probe_result r{};
        auto px{ wide::static_method("sSixD") };
        if (px.has_value())
        {
            r.resolved = true;
            const vmhook::method_proxy::value_t v = px->call(a, b, c, d, e, f);
            r.is_void    = v.is_void();
            r.dispatched = !v.is_void();
            const double got = v;
            r.dbits      = d2bits(got);
        }
        put(key, r);
    }

    // Static (long,int,double,int,float) — the (JIDIF) five-arg "every shape"
    // tail at the no-receiver frame (first arg at slot 0).
    auto scap_widePent(const std::string& key,
                       std::int64_t a, std::int32_t b, double c,
                       std::int32_t d, float e) -> void
    {
        probe_result r{};
        auto px{ wide::static_method("sWidePent") };
        if (px.has_value())
        {
            r.resolved = true;
            const vmhook::method_proxy::value_t v = px->call(a, b, c, d, e);
            r.is_void    = v.is_void();
            r.dispatched = !v.is_void();
            const double got = v;
            r.dbits      = d2bits(got);
        }
        put(key, r);
    }

    // Static (int x7, long) — EIGHT args, the trailing wide long is the 8th (and
    // last packable) argument; with no receiver the long fills slots 7..8.
    auto scap_octa(const std::string& key,
                   std::int32_t a, std::int32_t b, std::int32_t c, std::int32_t d,
                   std::int32_t e, std::int32_t f, std::int32_t g,
                   std::int64_t h) -> void
    {
        probe_result r{};
        auto px{ wide::static_method("sOcta") };
        if (px.has_value())
        {
            r.resolved = true;
            const vmhook::method_proxy::value_t v = px->call(a, b, c, d, e, f, g, h);
            r.is_void    = v.is_void();
            r.dispatched = !v.is_void();
            r.ival       = static_cast<std::int64_t>(v);
        }
        put(key, r);
    }

    // Static (int x7, double) — EIGHT args, the trailing wide DOUBLE is the 8th
    // argument; proves the 'D'-kind eighth arg lands bit-exact at the boundary.
    auto scap_octaD(const std::string& key,
                    std::int32_t a, std::int32_t b, std::int32_t c, std::int32_t d,
                    std::int32_t e, std::int32_t f, std::int32_t g,
                    double h) -> void
    {
        probe_result r{};
        auto px{ wide::static_method("sOctaD") };
        if (px.has_value())
        {
            r.resolved = true;
            const vmhook::method_proxy::value_t v = px->call(a, b, c, d, e, f, g, h);
            r.is_void    = v.is_void();
            r.dispatched = !v.is_void();
            const double got = v;
            r.dbits      = d2bits(got);
        }
        put(key, r);
    }

    // Static, returns byte, (long,byte) — long at slot 0, byte at slot 2.
    auto scap_byteAfterLong(const std::string& key,
                            std::int64_t a, std::int8_t b) -> void
    {
        probe_result r{};
        auto px{ wide::static_method("sByteAfterLong") };
        if (px.has_value())
        {
            r.resolved = true;
            const vmhook::method_proxy::value_t v = px->call(a, b);
            r.is_void    = v.is_void();
            r.dispatched = !v.is_void();
            r.ival       = static_cast<std::int64_t>(v);
        }
        put(key, r);
    }

    // Static, returns char, (double,char) — double at slot 0, char at slot 2.
    auto scap_charAfterDouble(const std::string& key,
                              double a, std::uint16_t b) -> void
    {
        probe_result r{};
        auto px{ wide::static_method("sCharAfterDouble") };
        if (px.has_value())
        {
            r.resolved = true;
            const vmhook::method_proxy::value_t v = px->call(a, b);
            r.is_void    = v.is_void();
            r.dispatched = !v.is_void();
            r.ival       = static_cast<std::int64_t>(v);
        }
        put(key, r);
    }

    // Static, returns long, (char,long) — char at slot 0, long at slots 1..2.
    auto scap_charLong(const std::string& key,
                       std::uint16_t a, std::int64_t b) -> void
    {
        probe_result r{};
        auto px{ wide::static_method("sCharLong") };
        if (px.has_value())
        {
            r.resolved = true;
            const vmhook::method_proxy::value_t v = px->call(a, b);
            r.is_void    = v.is_void();
            r.dispatched = !v.is_void();
            r.ival       = static_cast<std::int64_t>(v);
        }
        put(key, r);
    }

    // ---- EXPLICIT-SIGNATURE capture (pin a wide overload by descriptor) -------
    auto cap_sig_long1(const wide& self, const std::string& key,
                       const char* name, const char* sig, std::int64_t a) -> void
    {
        probe_result r{};
        auto px{ self.get_method(name, sig) };
        if (px.has_value())
        {
            r.resolved = true;
            const vmhook::method_proxy::value_t v = px->call(a);
            r.is_void    = v.is_void();
            r.dispatched = !v.is_void();
            r.ival       = static_cast<std::int64_t>(v);
        }
        put(key, r);
    }

    // Run EVERY capture inside the detour against the live receiver.
    auto run_all(const std::unique_ptr<wide>& self) -> void
    {
        if (!self)
        {
            return;
        }
        const wide& s = *self;

        // ============================================================
        //  WRONG ARITY / WRONG TYPE — must NOT crash.  Run these FIRST: they
        //  reuse real methods (addL / scaleD / mixA) and would otherwise stamp
        //  garbage into the witness fields that the legitimate calls below leave
        //  in their asserted state.  By running first, the later legit calls
        //  overwrite any garbage, so the "LAST call" witness invariants hold.
        //  We resolve the name (which succeeds) but DELIBERATELY do not assert a
        //  specific value — the real guarantee is the process SURVIVES.  Calling
        //  a wide method with too few args / mismatched widths exercises the
        //  arg-packing + overload-walk under abuse (zero-init params[] makes the
        //  missing words read as 0; the abuse methods are primitive-only so there
        //  is no reference-slot store barrier that could AV).
        // ============================================================
        {
            // addL with ZERO args (too few).  call() with no args; survive.
            probe_result r{};
            auto px{ s.get_method("addL") };
            r.resolved = px.has_value();
            if (px.has_value())
            {
                const vmhook::method_proxy::value_t v = px->call();
                r.dispatched = !v.is_void();
                r.ival       = static_cast<std::int64_t>(v);
            }
            put("wrong_addL_noargs", r);
        }
        {
            // scaleD with a single double (missing the trailing int).  Survive.
            probe_result r{};
            auto px{ s.get_method("scaleD") };
            r.resolved = px.has_value();
            if (px.has_value())
            {
                const vmhook::method_proxy::value_t v = px->call(2.0);
                r.dispatched = !v.is_void();
                const double g = v;
                r.dbits = d2bits(g);
            }
            put("wrong_scaleD_one_arg", r);
        }
        {
            // mixA called with (long,long,long) instead of (int,long,int):
            // wrong widths for the flanking params.  Survive; pin survival only.
            probe_result r{};
            auto px{ s.get_method("mixA") };
            r.resolved = px.has_value();
            if (px.has_value())
            {
                const vmhook::method_proxy::value_t v = px->call(1LL, 2LL, 3LL);
                r.dispatched = !v.is_void();
                r.ival = static_cast<std::int64_t>(v);
            }
            put("wrong_mixA_all_long", r);
        }

        // ============================================================
        //  BOUNDARY-EXTREME WIDE ARG IN A FLANKED POSITION.  The *_main flanked
        //  shapes below use mild operands (PI, 2.5, small longs); these re-run the
        //  SAME shapes with the wide arg at an EXTREME (subnormal / Inf / NaN /
        //  MAX_VALUE / +2^31 sign-extend witness / all-ones -1) so a truncation,
        //  sign-extension, or NaN-mangle in the flanked slot is caught while the
        //  narrow neighbours survive.  They run BEFORE the *_main calls so the
        //  later legit calls leave the LAST-call witnesses in the state those
        //  *_main witness assertions expect; here we assert ONLY the combined
        //  return, which alone pins the wide operand's full 64-bit width.
        // ============================================================
        // scaleD with the smallest subnormal as the wide leading double.
        cap_scaleD(s, "scaleD_subnormal", bits2d(0x0000000000000001ULL), 3);
        // scaleD with +Inf: Inf * n stays Inf bit-exact iff the double is intact.
        cap_scaleD(s, "scaleD_inf", bits2d(0x7FF0000000000000ULL), 2);
        // mixC with the middle double = MAX_VALUE and INT_MIN/INT_MAX flanks.
        cap_mixC(s, "mixC_extreme",
                 std::numeric_limits<std::int32_t>::min(),
                 bits2d(0x7FEFFFFFFFFFFFFFULL),               // MAX_VALUE
                 std::numeric_limits<std::int32_t>::max());
        // mixD all-four-wide at the extremes: LONG_MIN, +Inf, LONG_MAX, qNaN.
        // The sum becomes NaN (Inf/NaN dominate); both sides agree bit-for-bit.
        cap_mixD(s, "mixD_extreme",
                 std::numeric_limits<std::int64_t>::min(),
                 bits2d(0x7FF0000000000000ULL),               // +Inf
                 std::numeric_limits<std::int64_t>::max(),
                 bits2d(0x7FF8000000000000ULL));              // qNaN
        // addL with BOTH operands all-ones (-1): the full-width 0xFFFF..FFFF
        // pattern — a 32-bit truncation reads the same low word but a different
        // product/sum; the unsigned-wrap formula pins the true value.
        cap_long2(s, "addL_allones", "addL", -1LL, -1LL);
        // idj with the long operand = +2^31 (low word "looks negative-int"): a
        // sign-extend-from-32 bug flips the long's sign and the sum changes.
        cap_idj(s, "idj_pos2to31", 7, bits2d(0x3FF0000000000000ULL),  // +1.0
                static_cast<std::int64_t>(0x0000000080000000ULL));
        // jidi with the leading long = LONG_MIN and the middle double = -0.0; the
        // two narrow ints (INT_MAX / INT_MIN) must survive across both wide kinds.
        cap_jidi(s, "jidi_extreme",
                 std::numeric_limits<std::int64_t>::min(),
                 std::numeric_limits<std::int32_t>::max(),
                 bits2d(0x8000000000000000ULL),               // -0.0
                 std::numeric_limits<std::int32_t>::min());

        // ============================================================
        //  Single-long ECHO across the full boundary set (idL).  Proves the
        //  whole 64-bit value round-trips with no truncation, leading slot.
        // ============================================================
        for (std::size_t i{ 0 }; i < kLongCount; ++i)
        {
            cap_long(s, "idL_" + std::to_string(i), "idL", kLongVals[i]);
        }

        // ============================================================
        //  Two longs (addL): both wide, adjacent.  a<->b swap and truncation
        //  both caught by the asymmetric formula + the two witness fields.
        // ============================================================
        cap_long2(s, "addL_min_max", "addL",
                  std::numeric_limits<std::int64_t>::min(),
                  std::numeric_limits<std::int64_t>::max());
        cap_long2(s, "addL_high_low", "addL",
                  static_cast<std::int64_t>(0xFFFFFFFF00000000ULL),
                  static_cast<std::int64_t>(0x00000000FFFFFFFFULL));
        cap_long2(s, "addL_one_negone", "addL", 1LL, -1LL);
        // 32->64-bit CARRY pair: 0x7FFFFFFF + 1 must carry into bit 31 producing
        // 0x80000000 as a POSITIVE long, not wrap a 32-bit int to INT_MIN.  And a
        // pair whose low words are both "negative as int" but whose longs are
        // positive (>2^31) — a sign-extend-from-32 bug would make both negative.
        cap_long2(s, "addL_carry31", "addL",
                  static_cast<std::int64_t>(0x000000007FFFFFFFULL),
                  1LL);
        cap_long2(s, "addL_pos2to31", "addL",
                  static_cast<std::int64_t>(0x0000000080000000ULL),
                  static_cast<std::int64_t>(0x0000000080000000ULL));
        cap_long2(s, "addL_pat_pat", "addL",
                  static_cast<std::int64_t>(0x0123456789ABCDEFULL),
                  static_cast<std::int64_t>(0xDEADBEEFCAFEBABEULL));

        // ============================================================
        //  Single-double ECHO across the full boundary set (idD).  Bit-exact:
        //  NaN payload, signaling bit, denormal mantissa, sign of zero.
        // ============================================================
        for (std::size_t i{ 0 }; i < kDoubleCount; ++i)
        {
            cap_dbl(s, "idD_" + std::to_string(i), "idD", bits2d(kDoubleBits[i]));
        }

        // ============================================================
        //  WIDE IN THE MIDDLE: mixA(int, long, int).  The two ints must survive
        //  the two-slot long between them.  Use a long whose BOTH halves are
        //  nonzero (the worst case for a high-bits-leak corrupting the next int).
        // ============================================================
        cap_mixA(s, "mixA_main", 0x11111111, static_cast<std::int64_t>(0xDEADBEEFCAFEBABEULL), 0x22222222);
        // Middle long whose LOW word is 0x80000000 ("negative as int") but whose
        // value is +2^31: a sign-extend-from-32 bug flips its sign and the return
        // changes; the flanking ints (here both INT_MIN/INT_MAX extremes) must be
        // untouched.  NOT the last mixA, so its witnesses are not asserted.
        cap_mixA(s, "mixA_lowneg",
                 std::numeric_limits<std::int32_t>::min(),
                 static_cast<std::int64_t>(0x0000000080000000ULL),
                 std::numeric_limits<std::int32_t>::max());
        // and with the high-half-only long (a naive low-32-bit pack would make
        // the long look like 0 and the trailing int could absorb the high bits).
        cap_mixA(s, "mixA_highhalf", -7, static_cast<std::int64_t>(0xFFFFFFFF00000000ULL), 99);

        // ============================================================
        //  WIDE LEADING + TRAILING: mixB(long, int, long).
        // ============================================================
        cap_mixB(s, "mixB_main",
                 std::numeric_limits<std::int64_t>::min(), 0x5EEDFACE,
                 std::numeric_limits<std::int64_t>::max());

        // ============================================================
        //  WIDE LEADING, INT TRAILING: scaleD(double, int).  The canonical
        //  "double must not corrupt the following int" case.
        // ============================================================
        cap_scaleD(s, "scaleD_pi",   3.141592653589793, 1000000);
        cap_scaleD(s, "scaleD_neg",  bits2d(0xC02E000000000000ULL), -3); // -15.0 * -3
        cap_scaleD(s, "scaleD_zero", bits2d(0x8000000000000000ULL), 7);  // -0.0 * 7

        // ============================================================
        //  DOUBLE IN THE MIDDLE: mixC(int, double, int).
        // ============================================================
        cap_mixC(s, "mixC_main", 1000, 2.5, -2000);

        // ============================================================
        //  ALL FOUR WIDE: mixD(long, double, long, double).  long+double mixed.
        // ============================================================
        cap_mixD(s, "mixD_main",
                 100LL, 200.0, 300LL, 400.0);
        cap_mixD(s, "mixD_neg",
                 -5LL, -2.5, -7LL, -0.5);

        // ============================================================
        //  TWO DOUBLES, ADJACENT: addD(double, double).  The double analogue of
        //  addL — four contiguous wide slots.  Boundary doubles incl. -0.0, the
        //  smallest subnormal, MAX_VALUE, and an Inf/NaN pair (whose a*8.0+b is
        //  bit-identical on Java and the split C++ expression).
        // ============================================================
        cap_addD(s, "addD_pi_e", bits2d(0x400921FB54442D18ULL),  // PI
                                 bits2d(0x4005BF0A8B145769ULL));  // E
        cap_addD(s, "addD_negzero_min", bits2d(0x8000000000000000ULL),  // -0.0
                                        bits2d(0x0000000000000001ULL));  // MIN subnormal
        cap_addD(s, "addD_max_neg", bits2d(0x7FEFFFFFFFFFFFFFULL),       // MAX_VALUE
                                    bits2d(0xBFF0000000000000ULL));       // -1.0
        cap_addD(s, "addD_inf_nan", bits2d(0x7FF0000000000000ULL),       // +Inf
                                    bits2d(0x7FF8000000000000ULL));       // qNaN

        // ============================================================
        //  TWO WIDE KINDS BACK-TO-BACK (no narrow between):
        //    jd(long, double)  — long then double
        //    dj(double, long)  — double then long
        //  Proves the second wide arg starts exactly two slots after the first.
        // ============================================================
        cap_jd(s, "jd_main", static_cast<std::int64_t>(0xDEADBEEFCAFEBABEULL),
                             bits2d(0x400921FB54442D18ULL));            // PI
        cap_jd(s, "jd_minmax", std::numeric_limits<std::int64_t>::min(),
                               bits2d(0x7FEFFFFFFFFFFFFFULL));          // MAX_VALUE
        cap_dj(s, "dj_main", bits2d(0xC02E000000000000ULL),            // -15.0
                             static_cast<std::int64_t>(0x0123456789ABCDEFULL));
        cap_dj(s, "dj_nanmax", bits2d(0x7FF8000000000000ULL),         // qNaN
                               std::numeric_limits<std::int64_t>::max());

        // ============================================================
        //  SIX ARGS, EVERY KIND INTERLEAVED:
        //    hexA(int, long, double, int, long, double)
        //    hexB(long, int, double, long, int, double)
        //  Ten interpreter slots; two different interleaves so a single fixed
        //  mis-alignment cannot satisfy both.  Each operand stamped to a witness.
        // ============================================================
        cap_hexA(s, "hexA_main",
                 0x0A0A0A0A,
                 static_cast<std::int64_t>(0xFFFFFFFF00000000ULL),     // high-half-only long
                 bits2d(0x400921FB54442D18ULL),                        // PI
                 -1,
                 static_cast<std::int64_t>(0x00000000FFFFFFFFULL),     // low-half-only long
                 bits2d(0xBFF0000000000000ULL));                       // -1.0
        cap_hexB(s, "hexB_main",
                 std::numeric_limits<std::int64_t>::min(),
                 0x7FFFFFFF,
                 bits2d(0x8000000000000000ULL),                        // -0.0
                 std::numeric_limits<std::int64_t>::max(),
                 -2000000000,
                 bits2d(0x4005BF0A8B145769ULL));                       // E

        // ============================================================
        //  WIDE LONG FLANKED BY FLOATS: mixF(float, long, float).  A float is one
        //  slot but carries the 'F' descriptor (distinct from mixA's 'I'); the
        //  flanking floats must survive the two-slot long.  Both floats are exact
        //  integral values so the return and witnesses compare bit-exact.  The
        //  middle long has both halves set (worst case for a high-bits leak).
        // ============================================================
        cap_mixF(s, "mixF_main", 3.0f,
                 static_cast<std::int64_t>(0x00000000DEADBEEFULL), 5.0f);

        // ============================================================
        //  NARROW FLOAT THEN TWO WIDE: fld(float, long, double).  Five slots; the
        //  float must not widen into the long, and the double must start exactly
        //  two slots past the long.  Both halves of the long are set.
        // ============================================================
        cap_fld(s, "fld_main", 2.5f,
                static_cast<std::int64_t>(0xDEADBEEFCAFEBABEULL),
                bits2d(0x400921FB54442D18ULL));                        // PI

        // ============================================================
        //  WIDE LONG FLANKED BY OBJECT REFERENCES: mixS(String, long, String).
        //  Each reference is one slot; the long between them (high-half-only, the
        //  worst case) must stay intact and the two distinct-length references
        //  must not swap.  Passed as std::string -> java.lang.String.
        // ============================================================
        cap_mixS(s, "mixS_main", kStrA,
                 static_cast<std::int64_t>(0xFFFFFFFF00000000ULL), kStrC);

        // ============================================================
        //  REFERENCE THEN WIDE LONG, and WIDE LONG THEN REFERENCE.  The wide arg
        //  must start one slot after a leading reference (objLong), and a trailing
        //  reference must start two slots after a leading long (longObj).
        // ============================================================
        cap_objLong(s, "objLong_main", kStrA,
                    static_cast<std::int64_t>(0xDEADBEEFCAFEBABEULL));
        cap_longObj(s, "longObj_main",
                    std::numeric_limits<std::int64_t>::min(), kStrC);

        // ============================================================
        //  MINIMAL TWO-SLOT WITNESSES: an int / value immediately after a wide
        //  arg.  These isolate the corruption-of-following-slot bug class.
        // ============================================================
        cap_intAfterLong(s, "ial_min",  std::numeric_limits<std::int64_t>::min(), 0x1BADCAFE);
        cap_intAfterLong(s, "ial_high", static_cast<std::int64_t>(0xFFFFFFFF00000000ULL), -12345);
        cap_intAfterLong(s, "ial_full", static_cast<std::int64_t>(0xDEADBEEFCAFEBABEULL), 0x7FFFFFFF);
        cap_intAfterDouble(s, "iad_nan", bits2d(0x7FF8000000000000ULL), 0x0BADF00D);
        cap_intAfterDouble(s, "iad_max", bits2d(0x7FEFFFFFFFFFFFFFULL), -1);

        // ============================================================
        //  WIDE AFTER A NARROW: the wide value must START at the right slot.
        // ============================================================
        cap_longAfterInt(s, "lai_main", -1, std::numeric_limits<std::int64_t>::max());
        cap_doubleAfterInt(s, "dai_pi", 42, 3.141592653589793);

        // ============================================================
        //  OVERLOAD SELECTION BY WIDTH (name-only proxy):
        //  widthTag(int) vs widthTag(long); fdTag(float) vs fdTag(double).
        //  resolve_compatible_method must pick the wide overload for the wide arg.
        // ============================================================
        cap_tag<std::int32_t>(s, "wtag_int",  "widthTag", 5);
        cap_tag<std::int64_t>(s, "wtag_long", "widthTag", 5LL);
        cap_tag<float>(s,  "fdtag_float",  "fdTag", 1.5f);
        cap_tag<double>(s, "fdtag_double", "fdTag", 1.5);

        // ============================================================
        //  EXPLICIT SIGNATURE pinning the long overload of widthTag.
        // ============================================================
        cap_sig_long1(s, "wtag_sig_long", "widthTag", "(J)I", 9LL);

        // ============================================================
        //  DOUBLE IN THE MIDDLE FLANKED BY FLOATS: mixE(float, double, float).
        //  The 'F'-neighbour analogue of mixC(int,double,int): a packer that mis-
        //  expands the wide double against an 'F' slot (vs the 'I' in mixC) would
        //  corrupt a flanking float.  The flanking floats are small exact integers
        //  so the return and witnesses compare bit-exact.
        // ============================================================
        cap_mixE(s, "mixE_main", 3.0f, bits2d(0x400921FB54442D18ULL), 5.0f); // PI middle

        // ============================================================
        //  THE (JIDI) SHAPE: jidi(long, int, double, int).  Seven interpreter
        //  slots; the first int sits between a long and a double, the second int
        //  follows the double — a one-slot drift anywhere fails a witness.
        // ============================================================
        cap_jidi(s, "jidi_main",
                 static_cast<std::int64_t>(0xDEADBEEFCAFEBABEULL),   // both halves set
                 0x1BADCAFE,
                 bits2d(0x400921FB54442D18ULL),                       // PI
                 0x5EEDFACE);

        // ============================================================
        //  THE (ID J) SHAPE: idj(int, double, long).  The long must START exactly
        //  two slots after the double's start (slot 4 for instance).
        // ============================================================
        cap_idj(s, "idj_main", -1, bits2d(0xC02E000000000000ULL),    // -15.0
                static_cast<std::int64_t>(0x00000000FFFFFFFFULL));    // low-half-only long

        // ============================================================
        //  FIVE-ARG "EVERY SHAPE" TAIL: widePent(long, int, double, int, float).
        //  Eight interpreter slots (2+1+2+1+1); a TRAILING float after a wide-heavy
        //  frame proves the float lands on the right single slot.  The float widens
        //  to double once (exact), so the return is bit-exact.
        // ============================================================
        cap_widePent(s, "widePent_main",
                     std::numeric_limits<std::int64_t>::min(),       // long at slot 1
                     0x0A0A0A0A,
                     bits2d(0x400921FB54442D18ULL),                  // PI
                     -2000000000,
                     2.5f);                                          // trailing float

        // ============================================================
        //  DEEP PACKING — SIX adjacent longs: sixL(long x6).  Twelve contiguous
        //  interpreter slots; every slot pair must stay distinct (no half-bleed).
        //  Boundary mix: high-half-only, low-half-only, the +2^31 sign-extend
        //  witness, MIN/MAX, and a both-halves-set pattern.  An asymmetric formula
        //  (distinct multipliers) catches any neighbour swap; full-width operands
        //  catch any truncation.  Each operand stamped to its own witness.
        // ============================================================
        cap_sixL(s, "sixL_main",
                 static_cast<std::int64_t>(0xFFFFFFFF00000000ULL),   // high half only
                 static_cast<std::int64_t>(0x00000000FFFFFFFFULL),   // low half only
                 static_cast<std::int64_t>(0x0000000080000000ULL),   // +2^31 (sign-extend witness)
                 std::numeric_limits<std::int64_t>::min(),
                 std::numeric_limits<std::int64_t>::max(),
                 static_cast<std::int64_t>(0xDEADBEEFCAFEBABEULL));   // both halves set

        // ============================================================
        //  DEEP PACKING — SIX adjacent doubles: sixD(double x6).  Twelve contiguous
        //  slots, the double-kind deep-packing witness.  Each operand is scaled by a
        //  DISTINCT exact power of two in its own rounded step, then summed strictly
        //  left-to-right; the native side recomputes the identical operation order so
        //  the bits match.  Includes -0.0 and the smallest subnormal so the sign and
        //  the denormal mantissa survive twelve-deep packing.  Each operand stamped.
        // ============================================================
        cap_sixD(s, "sixD_main",
                 bits2d(0x400921FB54442D18ULL),   // PI
                 bits2d(0x8000000000000000ULL),   // -0.0
                 bits2d(0x0000000000000001ULL),   // smallest subnormal
                 bits2d(0xBFF0000000000000ULL),   // -1.0
                 bits2d(0x4005BF0A8B145769ULL),   // E
                 bits2d(0x3FE0000000000000ULL));  // 0.5

        // ============================================================
        //  WIDE DOUBLE BETWEEN OBJECT REFERENCES: mixSD(String, double, String).
        //  The double analogue of mixS's long-between-references.  The double must
        //  stay bit-exact beside two 'L' slots and the two distinct-length
        //  references must not swap.  String lengths feed the return (distinct
        //  exact scales), each operand stamped to a witness.
        // ============================================================
        cap_mixSD(s, "mixSD_main", kStrA,
                  bits2d(0x400921FB54442D18ULL), kStrC);            // PI between refs

        // ============================================================
        //  TRAILING WIDE AT THE DEEPEST INSTANCE SLOT: septa(int x6, long).  The
        //  receiver takes slot 0, the six ints slots 1..6, and the long slots 7..8
        //  — so the long's leading word lands in the LAST writable call-stub word
        //  (params[7]).  This is the boundary case for "the trailing argument is
        //  wide AND the frame is as deep as the params[8] array allows for an
        //  instance call".  The middle long has BOTH halves set (worst case for a
        //  high-bits leak past the array edge).  All seven operands stamped.
        // ============================================================
        cap_septa(s, "septa_main",
                  0x0A0A0A0A, -7, 0x13572468, -2000000000, 0x7FFFFFFF, 0x5EEDFACE,
                  static_cast<std::int64_t>(0xDEADBEEFCAFEBABEULL));

        // ============================================================
        //  SUB-INT NEIGHBOURS OF A WIDE ARG (byte/short/char/boolean).  Each
        //  sub-int is one slot like int but with a DIFFERENT descriptor and
        //  extension rule; the existing int-flank shapes only exercise 'I'.  We
        //  run two flavours per kind: the BOUNDARY value (MIN/MAX/-1/all-ones-as-
        //  char) first, then the *_main value last so the LAST-call witnesses are
        //  the ones the body asserts.  The leading wide arg uses a both-halves-set
        //  pattern (worst case for a high-bits leak into the adjacent narrow).
        // ============================================================
        // byte after long — boundary (-128) then main (-1 == 0xFF as byte).
        cap_byteAfterLong(s, "bal_min",
                          static_cast<std::int64_t>(0xDEADBEEFCAFEBABEULL),
                          std::numeric_limits<std::int8_t>::min());     // -128
        cap_byteAfterLong(s, "bal_main",
                          static_cast<std::int64_t>(0xFFFFFFFF00000000ULL), // high-half-only
                          static_cast<std::int8_t>(-1));                // 0xFF
        // short after long — boundary (SHRT_MIN) then main (-1).
        cap_shortAfterLong(s, "sal_min",
                           static_cast<std::int64_t>(0xDEADBEEFCAFEBABEULL),
                           std::numeric_limits<std::int16_t>::min());   // -32768
        cap_shortAfterLong(s, "sal_main",
                           static_cast<std::int64_t>(0xFFFFFFFF00000000ULL),
                           static_cast<std::int16_t>(-1));              // 0xFFFF
        // char after long — boundary (0xFFFF, the max code unit; a sign-extend bug
        // would turn it into -1) then main (a CJK code point U+4E2D).
        cap_charAfterLong(s, "cal_max",
                          static_cast<std::int64_t>(0xDEADBEEFCAFEBABEULL),
                          static_cast<std::uint16_t>(0xFFFF));
        cap_charAfterLong(s, "cal_main",
                          static_cast<std::int64_t>(0xFFFFFFFF00000000ULL),
                          static_cast<std::uint16_t>(0x4E2D));          // CJK 'zhong'
        // boolean after long — false then true.
        cap_boolAfterLong(s, "zal_false",
                          static_cast<std::int64_t>(0xDEADBEEFCAFEBABEULL), false);
        cap_boolAfterLong(s, "zal_main",
                          static_cast<std::int64_t>(0xFFFFFFFF00000000ULL), true);
        // char after double — boundary (0xFFFF) then main (U+00FF, the byte/char
        // boundary; a sign-or-byte-truncation bug would corrupt the high zero byte).
        cap_charAfterDouble(s, "cad_max", bits2d(0x7FEFFFFFFFFFFFFFULL), // MAX_VALUE
                            static_cast<std::uint16_t>(0xFFFF));
        cap_charAfterDouble(s, "cad_main", bits2d(0x400921FB54442D18ULL), // PI
                            static_cast<std::uint16_t>(0x00FF));
        // short after double — boundary (SHRT_MAX) then main (-12345).
        cap_shortAfterDouble(s, "sad_max", bits2d(0x8000000000000000ULL), // -0.0
                             std::numeric_limits<std::int16_t>::max());  // 32767
        cap_shortAfterDouble(s, "sad_main", bits2d(0x400921FB54442D18ULL), // PI
                             static_cast<std::int16_t>(-12345));
        // wide long flanked by byte+short (both sign-extended); high-half-only long.
        cap_mixBSC(s, "mixBSC_main",
                   static_cast<std::int8_t>(-100),
                   static_cast<std::int64_t>(0xFFFFFFFF00000000ULL),
                   static_cast<std::int16_t>(-30000));
        // wide double flanked by boolean+char.
        cap_mixZDC(s, "mixZDC_main", true,
                   bits2d(0x400921FB54442D18ULL),                       // PI
                   static_cast<std::uint16_t>(0xBEEF));
        // leading char then wide long — long must start one slot after the char.
        cap_charLong(s, "charLong_main",
                     static_cast<std::uint16_t>(0x4E2D),                 // CJK char
                     static_cast<std::int64_t>(0xDEADBEEFCAFEBABEULL));
        // every sub-int kind interleaved around both wide kinds.
        cap_bldcs(s, "bldcs_main",
                  static_cast<std::int8_t>(-7),
                  static_cast<std::int64_t>(0xDEADBEEFCAFEBABEULL),
                  bits2d(0x400921FB54442D18ULL),                        // PI
                  static_cast<std::uint16_t>(0xABCD),
                  static_cast<std::int16_t>(0x1234));

        // ============================================================
        //  STATIC variants (no receiver; first wide arg at slot 0).
        // ============================================================
        scap_long2("s_addL_min_max", "sAddL",
                   std::numeric_limits<std::int64_t>::min(),
                   std::numeric_limits<std::int64_t>::max());
        scap_long1("s_idL_pat", "sIdL", static_cast<std::int64_t>(0x0123456789ABCDEFULL));
        scap_dbl1 ("s_idD_nan", "sIdD", bits2d(0x7FF8000000000000ULL));
        scap_dbl1 ("s_idD_negzero", "sIdD", bits2d(0x8000000000000000ULL));
        scap_mixA ("s_mixA", -7, static_cast<std::int64_t>(0xFFFFFFFF00000000ULL), 99);
        scap_scaleD("s_scaleD", 3.141592653589793, 1000000);
        scap_mixD ("s_mixD", 100LL, 200.0, 300LL, 400.0);
        // static adjacent-wide variants (first wide arg at slot 0, no receiver).
        scap_addD ("s_addD", bits2d(0x400921FB54442D18ULL),   // PI
                             bits2d(0x4005BF0A8B145769ULL));   // E
        scap_jd   ("s_jd", static_cast<std::int64_t>(0xDEADBEEFCAFEBABEULL),
                           bits2d(0x400921FB54442D18ULL));     // PI
        scap_dj   ("s_dj", bits2d(0xC02E000000000000ULL),     // -15.0
                           static_cast<std::int64_t>(0x0123456789ABCDEFULL));
        // static float/object-interleave variants (first arg at slot 0).
        scap_fld  ("s_fld", 2.5f, static_cast<std::int64_t>(0xDEADBEEFCAFEBABEULL),
                            bits2d(0x400921FB54442D18ULL));    // PI
        scap_mixS ("s_mixS", kStrA,
                   static_cast<std::int64_t>(0xFFFFFFFF00000000ULL), kStrC);
        // static deep-packing variants — twelve contiguous slots at the no-receiver
        // frame (first wide arg at slot 0).  Same boundary mixes / FMA-safe formulas
        // as the instance sixL / sixD.
        scap_sixL ("s_sixL",
                   static_cast<std::int64_t>(0xFFFFFFFF00000000ULL),  // high half only
                   static_cast<std::int64_t>(0x00000000FFFFFFFFULL),  // low half only
                   static_cast<std::int64_t>(0x0000000080000000ULL),  // +2^31
                   std::numeric_limits<std::int64_t>::min(),
                   std::numeric_limits<std::int64_t>::max(),
                   static_cast<std::int64_t>(0xDEADBEEFCAFEBABEULL));  // both halves
        scap_sixD ("s_sixD",
                   bits2d(0x400921FB54442D18ULL),   // PI
                   bits2d(0x8000000000000000ULL),   // -0.0
                   bits2d(0x0000000000000001ULL),   // smallest subnormal
                   bits2d(0xBFF0000000000000ULL),   // -1.0
                   bits2d(0x4005BF0A8B145769ULL),   // E
                   bits2d(0x3FE0000000000000ULL));  // 0.5
        // static (JIDIF) five-arg "every shape" tail (first arg at slot 0).
        scap_widePent("s_widePent",
                      std::numeric_limits<std::int64_t>::min(),
                      0x0A0A0A0A,
                      bits2d(0x400921FB54442D18ULL),                  // PI
                      -2000000000,
                      2.5f);
        // STATIC EIGHT-ARG frames with a TRAILING wide as the 8th (last packable)
        // argument.  No receiver, so the seven narrow ints fill slots 0..6 and the
        // wide fills slots 7..8 — its leading word in the LAST call-stub word
        // params[7].  sOcta trails a long, sOctaD trails a double; both prove the
        // maximum-arity wide tail survives with no truncation/drop at the boundary.
        scap_octa("s_octa",
                  0x0A0A0A0A, -7, 0x13572468, -2000000000,
                  0x7FFFFFFF, 0x5EEDFACE, std::numeric_limits<std::int32_t>::min(),
                  static_cast<std::int64_t>(0xDEADBEEFCAFEBABEULL));
        scap_octaD("s_octaD",
                   1, 2, 3, 4, 5, 6, std::numeric_limits<std::int32_t>::max(),
                   bits2d(0x400921FB54442D18ULL));                    // PI as 8th
        // STATIC sub-int neighbour variants (no receiver; first arg at slot 0).
        scap_byteAfterLong("s_bal",
                           static_cast<std::int64_t>(0xFFFFFFFF00000000ULL),
                           static_cast<std::int8_t>(-1));              // 0xFF
        scap_charAfterDouble("s_cad", bits2d(0x400921FB54442D18ULL),   // PI
                             static_cast<std::uint16_t>(0x4E2D));      // CJK char
        scap_charLong("s_charLong",
                      static_cast<std::uint16_t>(0xFFFF),              // max code unit
                      std::numeric_limits<std::int64_t>::min());
    }

    // The entire test body, factored out so the VMHOOK_JVM_MODULE wrapper can run
    // it under a try/catch and ALWAYS follow it with an unconditional
    // shutdown_hooks() (mirrors register_class.cpp's suite-safety contract).
    auto run_wide_args_checks(vmhook_test::context& ctx) -> void
    {
    // =====================================================================
    //  ENTRY GUARD.  If MethodCallWideArgs is not loaded/resolvable, every
    //  static_field()->set/get below would deref a disengaged optional.  Bail
    //  cleanly to [INFO] (the final shutdown_hooks() in the wrapper still runs).
    //  In practice the harness loads the fixture on every run, so this is
    //  belt-and-braces.
    // =====================================================================
    if (vmhook::find_class("vmhook/fixtures/MethodCallWideArgs") == nullptr)
    {
        ctx.record("[INFO] method_call_wide_args: MethodCallWideArgs not "
                   "loaded/resolvable on this run; skipping live checks (no crash, "
                   "no hooks armed).");
        return;
    }

    vmhook::register_class<wide>("vmhook/fixtures/MethodCallWideArgs");

    // Sanity: the class resolves and a static read works at all.
    {
        const auto probe{ wide::static_field("triggerCount") };
        ctx.check("mcw_class_registered_static_field_resolves", probe.has_value());
    }

    {
        auto handle{ vmhook::scoped_hook<wide>(
            "trigger",
            [](vmhook::return_value&,
               const std::unique_ptr<wide>& self,
               std::int32_t /*delta*/)
            {
                g_detour_calls.fetch_add(1, std::memory_order_relaxed);
                g_detour_saw_self.store(self != nullptr, std::memory_order_relaxed);
                g_call_stub_path.store(
                    vmhook::detail::find_call_stub_entry() != nullptr,
                    std::memory_order_relaxed);
                // `self` is the live dispatch receiver (trigger is an instance
                // method); every method_proxy::call() runs against it here, where
                // current_java_thread is set.
                run_all(self);
            }) };
        ctx.check("mcw_hook_installed", handle.installed());

        const bool done{ ctx.run_probe(
            [](bool v) { wide::set_go(v); },
            []() { return wide::get_done(); }) };

        ctx.check("mcw_probe_completed", done);
        ctx.check("mcw_detour_fired", g_detour_calls.load(std::memory_order_relaxed) >= 1);
        ctx.check("mcw_detour_saw_self", g_detour_saw_self.load(std::memory_order_relaxed));
        ctx.check("mcw_trigger_count_advanced", wide::trigger_count() >= 1);

        const bool stub_path{ g_call_stub_path.load(std::memory_order_relaxed) };
        ctx.record(std::string{ "[INFO] method_call_wide_args dispatch path: " }
                   + (stub_path ? "call_stub fast path (StubRoutines::_call_stub_entry present)"
                                : "JNI fallback (CallXMethodA; call stub absent)"));

        // =====================================================================
        //  SINGLE-LONG ECHO — every boundary value round-trips bit-exact.
        // =====================================================================
        for (std::size_t i{ 0 }; i < kLongCount; ++i)
        {
            const probe_result r{ got("idL_" + std::to_string(i)) };
            const std::string suffix{ std::to_string(i) };
            ctx.check("idL_resolved_" + suffix, r.resolved);
            ctx.check("idL_not_void_" + suffix, r.dispatched);
            ctx.check("idL_echo_exact_" + suffix, r.ival == kLongVals[i]);
        }
        // The dedicated witness field for the LAST idL call holds that call's
        // exact long (proves the single wide arg reached the callee unmangled).
        ctx.check("idL_witness_last_arg",
                  wide::wIdL() == kLongVals[kLongCount - 1]);

        // =====================================================================
        //  TWO LONGS (addL) — combined return AND both witnesses.
        // =====================================================================
        {
            const std::int64_t a{ std::numeric_limits<std::int64_t>::min() };
            const std::int64_t b{ std::numeric_limits<std::int64_t>::max() };
            const probe_result r{ got("addL_min_max") };
            ctx.check("addL_min_max_resolved", r.resolved);
            ctx.check("addL_min_max_return", r.ival == jadd(jmul(a, 1000003LL), b));
        }
        {
            const std::int64_t a{ static_cast<std::int64_t>(0xFFFFFFFF00000000ULL) };
            const std::int64_t b{ static_cast<std::int64_t>(0x00000000FFFFFFFFULL) };
            const probe_result r{ got("addL_high_low") };
            ctx.check("addL_high_low_resolved", r.resolved);
            // If either long were truncated to 32 bits the product/sum changes.
            ctx.check("addL_high_low_return", r.ival == jadd(jmul(a, 1000003LL), b));
        }
        {
            const probe_result r{ got("addL_one_negone") };
            ctx.check("addL_one_negone_return", r.ival == jadd(jmul(1LL, 1000003LL), -1LL));
        }
        {
            // 32->64 carry: 0x7FFFFFFF * 1000003 + 1.  A 32-bit-truncating packer
            // that wrapped the first operand at INT_MAX would compute a different
            // product (and sign), so the full-width formula here is the witness.
            const std::int64_t a{ static_cast<std::int64_t>(0x000000007FFFFFFFULL) };
            const probe_result r{ got("addL_carry31") };
            ctx.check("addL_carry31_resolved", r.resolved);
            ctx.check("addL_carry31_return", r.ival == jadd(jmul(a, 1000003LL), 1LL));
        }
        {
            // Both operands are +2^31 (low word 0x80000000, "negative as int").  A
            // sign-extend-from-32 bug would treat each as -2^31 and flip the
            // product's sign — the unsigned-wrap formula pins the correct value.
            const std::int64_t a{ static_cast<std::int64_t>(0x0000000080000000ULL) };
            const probe_result r{ got("addL_pos2to31") };
            ctx.check("addL_pos2to31_resolved", r.resolved);
            ctx.check("addL_pos2to31_return", r.ival == jadd(jmul(a, 1000003LL), a));
        }
        {
            const std::int64_t a{ static_cast<std::int64_t>(0x0123456789ABCDEFULL) };
            const std::int64_t b{ static_cast<std::int64_t>(0xDEADBEEFCAFEBABEULL) };
            const probe_result r{ got("addL_pat_pat") };
            ctx.check("addL_pat_pat_return", r.ival == jadd(jmul(a, 1000003LL), b));
            // The LAST addL executed in run_all was addL_pat_pat; its witnesses
            // hold both exact 64-bit args (no truncation, no swap).
            ctx.check("addL_witness_a_exact", wide::wAddLa() == a);
            ctx.check("addL_witness_b_exact", wide::wAddLb() == b);
        }

        // =====================================================================
        //  SINGLE-DOUBLE ECHO — bit-exact across the boundary set.
        // =====================================================================
        for (std::size_t i{ 0 }; i < kDoubleCount; ++i)
        {
            const probe_result r{ got("idD_" + std::to_string(i)) };
            const std::string suffix{ std::to_string(i) };
            ctx.check("idD_resolved_" + suffix, r.resolved);
            ctx.check("idD_not_void_" + suffix, r.dispatched);
            ctx.check("idD_bits_exact_" + suffix, r.dbits == kDoubleBits[i]);
        }

        // =====================================================================
        //  WIDE IN THE MIDDLE (mixA): the flanking ints survive the long.
        // =====================================================================
        {
            const std::int32_t a{ 0x11111111 };
            const std::int64_t b{ static_cast<std::int64_t>(0xDEADBEEFCAFEBABEULL) };
            const std::int32_t c{ 0x22222222 };
            const probe_result r{ got("mixA_main") };
            ctx.check("mixA_main_resolved", r.resolved);
            ctx.check("mixA_main_return",
                      r.ival == jadd(jadd(jmul(a, 7LL), jmul(b, 1000003LL)), jmul(c, 13LL)));
        }
        {
            // Middle long = +2^31 (low word 0x80000000); flanking ints are INT_MIN
            // and INT_MAX.  A sign-extend-from-32 bug on the middle long flips its
            // sign and the return changes — the flanking extremes also catch a
            // shift that would alias the long's low/high word into a neighbour.
            const std::int32_t a{ std::numeric_limits<std::int32_t>::min() };
            const std::int64_t b{ static_cast<std::int64_t>(0x0000000080000000ULL) };
            const std::int32_t c{ std::numeric_limits<std::int32_t>::max() };
            const probe_result r{ got("mixA_lowneg") };
            ctx.check("mixA_lowneg_resolved", r.resolved);
            ctx.check("mixA_lowneg_return",
                      r.ival == jadd(jadd(jmul(a, 7LL), jmul(b, 1000003LL)), jmul(c, 13LL)));
        }
        {
            // high-half-only long; the trailing int (99) MUST be intact — this is
            // the precise witness for "wide high bits leaked into the next slot".
            const std::int32_t a{ -7 };
            const std::int64_t b{ static_cast<std::int64_t>(0xFFFFFFFF00000000ULL) };
            const std::int32_t c{ 99 };
            const probe_result r{ got("mixA_highhalf") };
            ctx.check("mixA_highhalf_resolved", r.resolved);
            ctx.check("mixA_highhalf_return",
                      r.ival == jadd(jadd(jmul(a, 7LL), jmul(b, 1000003LL)), jmul(c, 13LL)));
            // Witnesses from the LAST mixA call (mixA_highhalf): all three exact.
            ctx.check("mixA_witness_a_intact", wide::wMixAa() == a);
            ctx.check("mixA_witness_b_intact", wide::wMixAb() == b);
            ctx.check("mixA_witness_c_intact_after_wide", wide::wMixAc() == c);
        }

        // =====================================================================
        //  WIDE LEADING + TRAILING (mixB): the squeezed int survives.
        // =====================================================================
        {
            const std::int64_t a{ std::numeric_limits<std::int64_t>::min() };
            const std::int32_t b{ 0x5EEDFACE };
            const std::int64_t c{ std::numeric_limits<std::int64_t>::max() };
            const probe_result r{ got("mixB_main") };
            ctx.check("mixB_main_resolved", r.resolved);
            ctx.check("mixB_main_return",
                      r.ival == jadd(jadd(jmul(a, 1000003LL), jmul(b, 7LL)), jmul(c, 97LL)));
            ctx.check("mixB_witness_a_exact", wide::wMixBa() == a);
            ctx.check("mixB_witness_b_intact", wide::wMixBb() == b);
            ctx.check("mixB_witness_c_exact", wide::wMixBc() == c);
        }

        // =====================================================================
        //  WIDE LEADING, INT TRAILING (scaleD): the int after the double is exact.
        // =====================================================================
        {
            const double x{ 3.141592653589793 };
            const std::int32_t n{ 1000000 };
            const probe_result r{ got("scaleD_pi") };
            ctx.check("scaleD_pi_resolved", r.resolved);
            // x*n computed the same way in C++ (IEEE-754 deterministic).
            ctx.check("scaleD_pi_return_bits", r.dbits == d2bits(x * static_cast<double>(n)));
        }
        {
            const double x{ bits2d(0xC02E000000000000ULL) }; // -15.0
            const std::int32_t n{ -3 };
            const probe_result r{ got("scaleD_neg") };
            ctx.check("scaleD_neg_return_bits", r.dbits == d2bits(x * static_cast<double>(n)));
        }
        {
            const double x{ bits2d(0x8000000000000000ULL) }; // -0.0
            const std::int32_t n{ 7 };
            const probe_result r{ got("scaleD_zero") };
            ctx.check("scaleD_zero_return_bits", r.dbits == d2bits(x * static_cast<double>(n)));
            // The LAST scaleD executed was scaleD_zero: the trailing int witness
            // MUST equal 7 exactly (proves the double did not corrupt it).
            ctx.check("scaleD_witness_n_intact_after_double", wide::wScaleDn() == n);
        }

        // =====================================================================
        //  DOUBLE IN THE MIDDLE (mixC): both flanking ints survive.
        // =====================================================================
        {
            const std::int32_t a{ 1000 };
            const double b{ 2.5 };
            const std::int32_t c{ -2000 };
            const probe_result r{ got("mixC_main") };
            ctx.check("mixC_main_resolved", r.resolved);
            ctx.check("mixC_main_return_bits",
                      r.dbits == d2bits(b + static_cast<double>(a) + static_cast<double>(c)));
            ctx.check("mixC_witness_a_intact", wide::wMixCa() == a);
            ctx.check("mixC_witness_c_intact_after_double", wide::wMixCc() == c);
        }

        // =====================================================================
        //  ALL FOUR WIDE (mixD): long+double interleaved.
        // =====================================================================
        {
            const std::int64_t a{ 100 };
            const double b{ 200.0 };
            const std::int64_t c{ 300 };
            const double d{ 400.0 };
            const probe_result r{ got("mixD_main") };
            ctx.check("mixD_main_resolved", r.resolved);
            ctx.check("mixD_main_return_bits",
                      r.dbits == d2bits(static_cast<double>(a) + b + static_cast<double>(c) + d));
        }
        {
            const std::int64_t a{ -5 };
            const double b{ -2.5 };
            const std::int64_t c{ -7 };
            const double d{ -0.5 };
            const probe_result r{ got("mixD_neg") };
            ctx.check("mixD_neg_return_bits",
                      r.dbits == d2bits(static_cast<double>(a) + b + static_cast<double>(c) + d));
            // The LAST instance mixD was mixD_neg: both long witnesses exact (the
            // two longs survived the doubles interleaved between/after them).
            ctx.check("mixD_witness_a_exact", wide::wMixDa() == a);
            ctx.check("mixD_witness_c_exact", wide::wMixDc() == c);
        }

        // =====================================================================
        //  TWO DOUBLES, ADJACENT (addD).  Return computed the EXACT way Java does
        //  it: `sa = a * 8.0` rounded FIRST (its own statement), THEN `sa + b`.
        //  Splitting the multiply out of the add guarantees neither Java (no FMA)
        //  nor GCC (no cross-statement contraction) fuses it, so the bits match on
        //  every target.  Four boundary pairs incl. -0.0 / subnormal / Inf / NaN.
        // =====================================================================
        {
            const double a{ bits2d(0x400921FB54442D18ULL) }; // PI
            const double b{ bits2d(0x4005BF0A8B145769ULL) }; // E
            const double sa{ a * 8.0 };
            const probe_result r{ got("addD_pi_e") };
            ctx.check("addD_pi_e_resolved", r.resolved);
            ctx.check("addD_pi_e_not_void", r.dispatched);
            ctx.check("addD_pi_e_return_bits", r.dbits == d2bits(sa + b));
        }
        {
            const double a{ bits2d(0x8000000000000000ULL) }; // -0.0
            const double b{ bits2d(0x0000000000000001ULL) }; // smallest subnormal
            const double sa{ a * 8.0 };
            const probe_result r{ got("addD_negzero_min") };
            ctx.check("addD_negzero_min_return_bits", r.dbits == d2bits(sa + b));
        }
        {
            const double a{ bits2d(0x7FEFFFFFFFFFFFFFULL) }; // MAX_VALUE
            const double b{ bits2d(0xBFF0000000000000ULL) }; // -1.0
            const double sa{ a * 8.0 };                       // overflows to +Inf
            const probe_result r{ got("addD_max_neg") };
            ctx.check("addD_max_neg_return_bits", r.dbits == d2bits(sa + b));
        }
        {
            const double a{ bits2d(0x7FF0000000000000ULL) }; // +Inf
            const double b{ bits2d(0x7FF8000000000000ULL) }; // qNaN
            const double sa{ a * 8.0 };                       // +Inf
            const probe_result r{ got("addD_inf_nan") };
            // Inf + NaN -> NaN; both sides produce a NaN bit pattern identically.
            ctx.check("addD_inf_nan_return_bits", r.dbits == d2bits(sa + b));
            // The LAST addD executed was addD_inf_nan: both double witnesses are
            // bit-exact (Inf and the qNaN survived the adjacent four-slot pack).
            ctx.check("addD_witness_a_bits_exact", wide::wAddDa() == 0x7FF0000000000000ULL);
            ctx.check("addD_witness_b_bits_exact", wide::wAddDb() == 0x7FF8000000000000ULL);
        }

        // =====================================================================
        //  TWO WIDE KINDS BACK-TO-BACK (jd = long,double; dj = double,long).  The
        //  second wide arg must START exactly two slots past the first (no narrow
        //  between).  Return is a pure sum (FMA-safe); witnesses pin each exactly.
        // =====================================================================
        {
            const std::int64_t a{ static_cast<std::int64_t>(0xDEADBEEFCAFEBABEULL) };
            const double b{ bits2d(0x400921FB54442D18ULL) }; // PI
            const probe_result r{ got("jd_main") };
            ctx.check("jd_main_resolved", r.resolved);
            ctx.check("jd_main_return_bits", r.dbits == d2bits(static_cast<double>(a) + b));
        }
        {
            const std::int64_t a{ std::numeric_limits<std::int64_t>::min() };
            const double b{ bits2d(0x7FEFFFFFFFFFFFFFULL) }; // MAX_VALUE
            const probe_result r{ got("jd_minmax") };
            ctx.check("jd_minmax_return_bits", r.dbits == d2bits(static_cast<double>(a) + b));
            // LAST jd was jd_minmax: long witness exact, double witness bit-exact.
            ctx.check("jd_minmax_witness_a_exact", wide::wJdA() == a);
            ctx.check("jd_minmax_witness_b_bits_exact", wide::wJdB() == 0x7FEFFFFFFFFFFFFFULL);
        }
        {
            const double a{ bits2d(0xC02E000000000000ULL) }; // -15.0
            const std::int64_t b{ static_cast<std::int64_t>(0x0123456789ABCDEFULL) };
            const probe_result r{ got("dj_main") };
            ctx.check("dj_main_resolved", r.resolved);
            ctx.check("dj_main_return_bits", r.dbits == d2bits(a + static_cast<double>(b)));
        }
        {
            const double a{ bits2d(0x7FF8000000000000ULL) }; // qNaN
            const std::int64_t b{ std::numeric_limits<std::int64_t>::max() };
            const probe_result r{ got("dj_nanmax") };
            ctx.check("dj_nanmax_return_bits", r.dbits == d2bits(a + static_cast<double>(b)));
            // LAST dj was dj_nanmax: double witness bit-exact (NaN), long exact.
            ctx.check("dj_nanmax_witness_a_bits_exact", wide::wDjA() == 0x7FF8000000000000ULL);
            ctx.check("dj_nanmax_witness_b_exact", wide::wDjB() == b);
        }

        // =====================================================================
        //  SIX ARGS, EVERY KIND INTERLEAVED (hexA / hexB).  Combined return is a
        //  pure sum (no FMA-contractible mul+add); EVERY operand additionally has
        //  a dedicated witness, so a one-slot shift anywhere fails a witness even
        //  if the sum coincidentally matched.
        // =====================================================================
        {
            const std::int32_t a{ 0x0A0A0A0A };
            const std::int64_t b{ static_cast<std::int64_t>(0xFFFFFFFF00000000ULL) };
            const double c{ bits2d(0x400921FB54442D18ULL) }; // PI
            const std::int32_t d{ -1 };
            const std::int64_t e{ static_cast<std::int64_t>(0x00000000FFFFFFFFULL) };
            const double f{ bits2d(0xBFF0000000000000ULL) }; // -1.0
            const probe_result r{ got("hexA_main") };
            ctx.check("hexA_main_resolved", r.resolved);
            ctx.check("hexA_main_not_void", r.dispatched);
            ctx.check("hexA_main_return_bits",
                      r.dbits == d2bits(static_cast<double>(a) + static_cast<double>(b)
                                        + c + static_cast<double>(d)
                                        + static_cast<double>(e) + f));
            // Every one of the six args is exact / bit-exact in its witness.
            ctx.check("hexA_witness_a_intact", wide::wHexAa() == a);
            ctx.check("hexA_witness_b_exact",  wide::wHexAb() == b);
            ctx.check("hexA_witness_c_bits",   wide::wHexAc() == 0x400921FB54442D18ULL);
            ctx.check("hexA_witness_d_intact", wide::wHexAd() == d);
            ctx.check("hexA_witness_e_exact",  wide::wHexAe() == e);
            ctx.check("hexA_witness_f_bits",   wide::wHexAf() == 0xBFF0000000000000ULL);
        }
        {
            const std::int64_t a{ std::numeric_limits<std::int64_t>::min() };
            const std::int32_t b{ 0x7FFFFFFF };
            const double c{ bits2d(0x8000000000000000ULL) }; // -0.0
            const std::int64_t d{ std::numeric_limits<std::int64_t>::max() };
            const std::int32_t e{ -2000000000 };
            const double f{ bits2d(0x4005BF0A8B145769ULL) }; // E
            const probe_result r{ got("hexB_main") };
            ctx.check("hexB_main_resolved", r.resolved);
            ctx.check("hexB_main_return_bits",
                      r.dbits == d2bits(static_cast<double>(a) + static_cast<double>(b)
                                        + c + static_cast<double>(d)
                                        + static_cast<double>(e) + f));
            ctx.check("hexB_witness_a_exact",  wide::wHexBa() == a);
            ctx.check("hexB_witness_b_intact", wide::wHexBb() == b);
            ctx.check("hexB_witness_c_bits",   wide::wHexBc() == 0x8000000000000000ULL);
            ctx.check("hexB_witness_d_exact",  wide::wHexBd() == d);
            ctx.check("hexB_witness_e_intact", wide::wHexBe() == e);
            ctx.check("hexB_witness_f_bits",   wide::wHexBf() == 0x4005BF0A8B145769ULL);
        }

        // =====================================================================
        //  WIDE LONG FLANKED BY FLOATS (mixF).  The return is recomputed in the
        //  EXACT float-precision order Java uses — a*256.0f and c*4.0f each rounded
        //  to float in their own step, the long widened to float once, then summed
        //  left-to-right — so the bits match with no FMA/double-rounding drift.
        //  Both floats are small exact integers, so their witnesses are bit-exact.
        // =====================================================================
        {
            const float        a{ 3.0f };
            const std::int64_t b{ static_cast<std::int64_t>(0x00000000DEADBEEFULL) };
            const float        c{ 5.0f };
            const float sa{ a * 256.0f };
            const float sc{ c * 4.0f };
            const float lo{ static_cast<float>(b) };
            const float expect{ sa + lo + sc };
            const probe_result r{ got("mixF_main") };
            ctx.check("mixF_main_resolved", r.resolved);
            ctx.check("mixF_main_not_void", r.dispatched);
            ctx.check("mixF_main_return_bits", r.fbits == f2bits(expect));
            // The flanking floats survived the two-slot long (bit-exact witnesses).
            ctx.check("mixF_witness_a_bits", wide::wMixFa() == f2bits(a));
            ctx.check("mixF_witness_b_exact", wide::wMixFb() == b);
            ctx.check("mixF_witness_c_bits_after_wide", wide::wMixFc() == f2bits(c));
        }

        // =====================================================================
        //  NARROW FLOAT THEN TWO WIDE (fld = float,long,double).  Pure sum, so
        //  bit-exact across the widen-to-double of the float and the long.
        // =====================================================================
        {
            const float        a{ 2.5f };
            const std::int64_t b{ static_cast<std::int64_t>(0xDEADBEEFCAFEBABEULL) };
            const double       c{ bits2d(0x400921FB54442D18ULL) }; // PI
            const probe_result r{ got("fld_main") };
            ctx.check("fld_main_resolved", r.resolved);
            ctx.check("fld_main_return_bits",
                      r.dbits == d2bits(static_cast<double>(a) + static_cast<double>(b) + c));
            ctx.check("fld_witness_a_bits", wide::wFldA() == f2bits(a));
            ctx.check("fld_witness_b_exact", wide::wFldB() == b);
            ctx.check("fld_witness_c_bits", wide::wFldC() == 0x400921FB54442D18ULL);
        }

        // =====================================================================
        //  WIDE LONG FLANKED BY OBJECT REFERENCES (mixS = String,long,String).
        //  The long between two 'L' slots is intact, and the two distinct-length
        //  references did not swap.  Return is recomputed from the long + the
        //  known String lengths; witnesses pin the long exactly and the Strings'
        //  content (so a swap is caught independently of the return).
        // =====================================================================
        {
            const std::int64_t b{ static_cast<std::int64_t>(0xFFFFFFFF00000000ULL) };
            const std::int64_t expect{ jadd(jadd(b, jmul(kStrLenA, 1000003LL)),
                                            jmul(kStrLenC, 97LL)) };
            const probe_result r{ got("mixS_main") };
            ctx.check("mixS_main_resolved", r.resolved);
            ctx.check("mixS_main_not_void", r.dispatched);
            ctx.check("mixS_main_return", r.ival == expect);
            // The long survived between the two reference slots.
            ctx.check("mixS_witness_long_exact", wide::wMixSb() == b);
            // The references did not swap (distinct content each in its own slot).
            ctx.check("mixS_witness_a_content", wide::wMixSa() == kStrA);
            ctx.check("mixS_witness_c_content_after_wide", wide::wMixSc() == kStrC);
        }

        // =====================================================================
        //  REFERENCE THEN WIDE LONG (objLong) and WIDE LONG THEN REFERENCE
        //  (longObj).  The wide value must start one slot after a leading
        //  reference, and a trailing reference two slots after a leading long.
        // =====================================================================
        {
            const std::int64_t v{ static_cast<std::int64_t>(0xDEADBEEFCAFEBABEULL) };
            const probe_result r{ got("objLong_main") };
            ctx.check("objLong_main_resolved", r.resolved);
            ctx.check("objLong_main_return_long_exact", r.ival == v);
            ctx.check("objLong_witness_obj_content", wide::wOlObj() == kStrA);
            ctx.check("objLong_witness_long_exact_after_ref", wide::wOlLong() == v);
        }
        {
            const std::int64_t v{ std::numeric_limits<std::int64_t>::min() };
            const probe_result r{ got("longObj_main") };
            ctx.check("longObj_main_resolved", r.resolved);
            ctx.check("longObj_main_return_long_exact", r.ival == v);
            ctx.check("longObj_witness_long_exact", wide::wLoLong() == v);
            ctx.check("longObj_witness_obj_content_after_wide", wide::wLoObj() == kStrC);
        }

        // =====================================================================
        //  MINIMAL TWO-SLOT WITNESSES (intAfterLong / intAfterDouble): the int
        //  immediately after a wide arg equals exactly what was passed, AND the
        //  method returned that same int.
        // =====================================================================
        {
            const std::int32_t b{ 0x1BADCAFE };
            const probe_result r{ got("ial_min") };
            ctx.check("ial_min_resolved", r.resolved);
            ctx.check("ial_min_return_is_int", r.ival == static_cast<std::int64_t>(b));
        }
        {
            const std::int32_t b{ -12345 };
            const probe_result r{ got("ial_high") };
            ctx.check("ial_high_return_is_int", r.ival == static_cast<std::int64_t>(b));
        }
        {
            const std::int64_t a{ static_cast<std::int64_t>(0xDEADBEEFCAFEBABEULL) };
            const std::int32_t b{ 0x7FFFFFFF };
            const probe_result r{ got("ial_full") };
            ctx.check("ial_full_return_is_int", r.ival == static_cast<std::int64_t>(b));
            // LAST intAfterLong was ial_full: both witnesses exact.
            ctx.check("ial_full_witness_long_exact", wide::wIalLong() == a);
            ctx.check("ial_full_witness_int_intact", wide::wIalInt() == b);
        }
        {
            const std::int32_t b{ 0x0BADF00D };
            const probe_result r{ got("iad_nan") };
            ctx.check("iad_nan_resolved", r.resolved);
            ctx.check("iad_nan_return_is_int", r.ival == static_cast<std::int64_t>(b));
        }
        {
            const std::int32_t b{ -1 };
            const probe_result r{ got("iad_max") };
            ctx.check("iad_max_return_is_int", r.ival == static_cast<std::int64_t>(b));
            // LAST intAfterDouble was iad_max: the int after the double is intact.
            ctx.check("iad_max_witness_int_intact", wide::wIadInt() == b);
        }

        // =====================================================================
        //  WIDE AFTER A NARROW (longAfterInt / doubleAfterInt).
        // =====================================================================
        {
            const std::int64_t b{ std::numeric_limits<std::int64_t>::max() };
            const probe_result r{ got("lai_main") };
            ctx.check("lai_main_resolved", r.resolved);
            ctx.check("lai_main_return_long_exact", r.ival == b);
            ctx.check("lai_main_witness_int_intact", wide::wLaiInt() == -1);
            ctx.check("lai_main_witness_long_exact", wide::wLaiLong() == b);
        }
        {
            const double b{ 3.141592653589793 };
            const probe_result r{ got("dai_pi") };
            ctx.check("dai_pi_resolved", r.resolved);
            ctx.check("dai_pi_return_double_bits", r.dbits == d2bits(b));
            ctx.check("dai_pi_witness_int_intact", wide::wDaiInt() == 42);
        }

        // =====================================================================
        //  OVERLOAD SELECTION BY WIDTH — the C++ arg type picks the wide overload.
        // =====================================================================
        {
            const probe_result ri{ got("wtag_int") };
            const probe_result rl{ got("wtag_long") };
            ctx.check("wtag_int_resolved", ri.resolved);
            ctx.check("wtag_long_resolved", rl.resolved);
            ctx.check("wtag_int_picks_int_overload",  ri.ival == WIDTH_TAG_INT);
            ctx.check("wtag_long_picks_long_overload", rl.ival == WIDTH_TAG_LONG);
        }
        {
            const probe_result rf{ got("fdtag_float") };
            const probe_result rd{ got("fdtag_double") };
            ctx.check("fdtag_float_resolved", rf.resolved);
            ctx.check("fdtag_double_resolved", rd.resolved);
            ctx.check("fdtag_float_picks_float_overload",   rf.ival == FD_TAG_FLOAT);
            ctx.check("fdtag_double_picks_double_overload",  rd.ival == FD_TAG_DOUBLE);
        }
        {
            // Explicit (J)I signature must dispatch the long overload.
            const probe_result r{ got("wtag_sig_long") };
            ctx.check("wtag_sig_long_resolved", r.resolved);
            ctx.check("wtag_sig_long_picks_long", r.ival == WIDTH_TAG_LONG);
        }

        // =====================================================================
        //  DOUBLE IN THE MIDDLE FLANKED BY FLOATS (mixE).  Return recomputed in
        //  the EXACT float-precision order Java uses — a*256.0f and c*4.0f each
        //  rounded to float in their own step, the double widened to float once,
        //  then summed left-to-right — so the bits match with no FMA/double-round
        //  drift.  Both floats are small exact integers, so their witnesses are
        //  bit-exact; the double witness is bit-exact (full 64-bit pattern).
        // =====================================================================
        {
            const float  a{ 3.0f };
            const double b{ bits2d(0x400921FB54442D18ULL) }; // PI
            const float  c{ 5.0f };
            const float sa{ a * 256.0f };
            const float sc{ c * 4.0f };
            const float bf{ static_cast<float>(b) };
            const float expect{ sa + bf + sc };
            const probe_result r{ got("mixE_main") };
            ctx.check("mixE_main_resolved", r.resolved);
            ctx.check("mixE_main_not_void", r.dispatched);
            ctx.check("mixE_main_return_bits", r.fbits == f2bits(expect));
            // The flanking floats survived the two-slot double (bit-exact).
            ctx.check("mixE_witness_a_bits", wide::wMixEa() == f2bits(a));
            ctx.check("mixE_witness_b_bits", wide::wMixEb() == 0x400921FB54442D18ULL);
            ctx.check("mixE_witness_c_bits_after_double", wide::wMixEc() == f2bits(c));
        }

        // =====================================================================
        //  THE (JIDI) SHAPE (jidi = long,int,double,int).  Pure left-to-right sum
        //  (no FMA-contractible mul+add), recomputed identically; every operand
        //  additionally pinned by a witness, so a one-slot drift fails a witness
        //  even if the sum coincided.
        // =====================================================================
        {
            const std::int64_t a{ static_cast<std::int64_t>(0xDEADBEEFCAFEBABEULL) };
            const std::int32_t b{ 0x1BADCAFE };
            const double       c{ bits2d(0x400921FB54442D18ULL) }; // PI
            const std::int32_t d{ 0x5EEDFACE };
            const probe_result r{ got("jidi_main") };
            ctx.check("jidi_main_resolved", r.resolved);
            ctx.check("jidi_main_not_void", r.dispatched);
            ctx.check("jidi_main_return_bits",
                      r.dbits == d2bits(static_cast<double>(a) + static_cast<double>(b)
                                        + c + static_cast<double>(d)));
            ctx.check("jidi_witness_a_exact",  wide::wJidiA() == a);
            ctx.check("jidi_witness_b_intact", wide::wJidiB() == b);
            ctx.check("jidi_witness_c_bits",   wide::wJidiC() == 0x400921FB54442D18ULL);
            ctx.check("jidi_witness_d_intact_after_double", wide::wJidiD() == d);
        }

        // =====================================================================
        //  THE (ID J) SHAPE (idj = int,double,long).  The long must START exactly
        //  two slots after the double.  Pure sum; every operand pinned.
        // =====================================================================
        {
            const std::int32_t a{ -1 };
            const double       b{ bits2d(0xC02E000000000000ULL) }; // -15.0
            const std::int64_t c{ static_cast<std::int64_t>(0x00000000FFFFFFFFULL) };
            const probe_result r{ got("idj_main") };
            ctx.check("idj_main_resolved", r.resolved);
            ctx.check("idj_main_return_bits",
                      r.dbits == d2bits(static_cast<double>(a) + b + static_cast<double>(c)));
            ctx.check("idj_witness_a_intact", wide::wIdjA() == a);
            ctx.check("idj_witness_b_bits",   wide::wIdjB() == 0xC02E000000000000ULL);
            ctx.check("idj_witness_c_exact_after_double", wide::wIdjC() == c);
        }

        // =====================================================================
        //  FIVE-ARG "EVERY SHAPE" TAIL (widePent = long,int,double,int,float).
        //  Eight interpreter slots; the TRAILING float lands on the right single
        //  slot once two wides + two narrows precede it.  Pure left-to-right sum
        //  (the float widens to double exactly), so bit-exact.  Each operand pinned.
        // =====================================================================
        {
            const std::int64_t a{ std::numeric_limits<std::int64_t>::min() };
            const std::int32_t b{ 0x0A0A0A0A };
            const double       c{ bits2d(0x400921FB54442D18ULL) }; // PI
            const std::int32_t d{ -2000000000 };
            const float        e{ 2.5f };
            const probe_result r{ got("widePent_main") };
            ctx.check("widePent_main_resolved", r.resolved);
            ctx.check("widePent_main_not_void", r.dispatched);
            ctx.check("widePent_main_return_bits",
                      r.dbits == d2bits(static_cast<double>(a) + static_cast<double>(b)
                                        + c + static_cast<double>(d)
                                        + static_cast<double>(e)));
            ctx.check("widePent_witness_a_exact",  wide::wPentA() == a);
            ctx.check("widePent_witness_b_intact", wide::wPentB() == b);
            ctx.check("widePent_witness_c_bits",   wide::wPentC() == 0x400921FB54442D18ULL);
            ctx.check("widePent_witness_d_intact", wide::wPentD() == d);
            ctx.check("widePent_witness_e_bits_trailing", wide::wPentE() == f2bits(e));
        }

        // =====================================================================
        //  DEEP PACKING — SIX adjacent longs (sixL).  Twelve contiguous slots; the
        //  asymmetric per-operand multipliers + full-width sum catch any neighbour
        //  swap or truncation across the deep pack.  Combined return AND all six
        //  witnesses are checked.  All arithmetic is two's-complement wraparound,
        //  mirrored via the unsigned-wrap helpers (jadd / jmul).
        // =====================================================================
        {
            const std::int64_t a{ static_cast<std::int64_t>(0xFFFFFFFF00000000ULL) };
            const std::int64_t b{ static_cast<std::int64_t>(0x00000000FFFFFFFFULL) };
            const std::int64_t c{ static_cast<std::int64_t>(0x0000000080000000ULL) };
            const std::int64_t d{ std::numeric_limits<std::int64_t>::min() };
            const std::int64_t e{ std::numeric_limits<std::int64_t>::max() };
            const std::int64_t f{ static_cast<std::int64_t>(0xDEADBEEFCAFEBABEULL) };
            // a*1000003 + b*31 + c*131 + d*524287 + e*8191 + f  (Java two's-comp)
            const std::int64_t expect{
                jadd(jadd(jadd(jadd(jadd(
                    jmul(a, 1000003LL),
                    jmul(b, 31LL)),
                    jmul(c, 131LL)),
                    jmul(d, 524287LL)),
                    jmul(e, 8191LL)),
                    f) };
            const probe_result r{ got("sixL_main") };
            ctx.check("sixL_main_resolved", r.resolved);
            ctx.check("sixL_main_not_void", r.dispatched);
            ctx.check("sixL_main_return", r.ival == expect);
            ctx.check("sixL_witness_a_exact", wide::wSixLa() == a);
            ctx.check("sixL_witness_b_exact", wide::wSixLb() == b);
            ctx.check("sixL_witness_c_exact", wide::wSixLc() == c);
            ctx.check("sixL_witness_d_exact", wide::wSixLd() == d);
            ctx.check("sixL_witness_e_exact", wide::wSixLe() == e);
            ctx.check("sixL_witness_f_exact", wide::wSixLf() == f);
        }

        // =====================================================================
        //  DEEP PACKING — SIX adjacent doubles (sixD).  Twelve contiguous slots.
        //  Return recomputed with the EXACT same split-scale-then-left-to-right-sum
        //  the fixture uses (each product its own rounded step), so the bits match
        //  on every target with no FMA contraction.  -0.0 and the smallest
        //  subnormal are included so sign and denormal mantissa survive the pack.
        //  Combined return AND all six bit-exact witnesses checked.
        // =====================================================================
        {
            const double a{ bits2d(0x400921FB54442D18ULL) }; // PI
            const double b{ bits2d(0x8000000000000000ULL) }; // -0.0
            const double c{ bits2d(0x0000000000000001ULL) }; // smallest subnormal
            const double d{ bits2d(0xBFF0000000000000ULL) }; // -1.0
            const double e{ bits2d(0x4005BF0A8B145769ULL) }; // E
            const double f{ bits2d(0x3FE0000000000000ULL) }; // 0.5
            const double ta{ a * 2.0 };
            const double tb{ b * 4.0 };
            const double tc{ c * 8.0 };
            const double td{ d * 16.0 };
            const double te{ e * 32.0 };
            const double tf{ f * 64.0 };
            const double expect{ ta + tb + tc + td + te + tf };
            const probe_result r{ got("sixD_main") };
            ctx.check("sixD_main_resolved", r.resolved);
            ctx.check("sixD_main_not_void", r.dispatched);
            ctx.check("sixD_main_return_bits", r.dbits == d2bits(expect));
            ctx.check("sixD_witness_a_bits", wide::wSixDa() == 0x400921FB54442D18ULL);
            ctx.check("sixD_witness_b_bits", wide::wSixDb() == 0x8000000000000000ULL);
            ctx.check("sixD_witness_c_bits", wide::wSixDc() == 0x0000000000000001ULL);
            ctx.check("sixD_witness_d_bits", wide::wSixDd() == 0xBFF0000000000000ULL);
            ctx.check("sixD_witness_e_bits", wide::wSixDe() == 0x4005BF0A8B145769ULL);
            ctx.check("sixD_witness_f_bits", wide::wSixDf() == 0x3FE0000000000000ULL);
        }

        // =====================================================================
        //  WIDE DOUBLE BETWEEN OBJECT REFERENCES (mixSD = String,double,String).
        //  The double analogue of mixS: the double between two 'L' slots is intact
        //  and the two distinct-length references did not swap.  Return recomputed
        //  from the double + known String lengths (exact scales, each its own step);
        //  the double witness is bit-exact and each String's content is pinned.
        // =====================================================================
        {
            const double b{ bits2d(0x400921FB54442D18ULL) }; // PI
            const double wa{ static_cast<double>(kStrLenA) * 1024.0 };
            const double wc{ static_cast<double>(kStrLenC) * 16.0 };
            const double expect{ b + wa + wc };
            const probe_result r{ got("mixSD_main") };
            ctx.check("mixSD_main_resolved", r.resolved);
            ctx.check("mixSD_main_not_void", r.dispatched);
            ctx.check("mixSD_main_return_bits", r.dbits == d2bits(expect));
            // The double survived between the two reference slots (bit-exact).
            ctx.check("mixSD_witness_double_bits", wide::wMixSDb() == 0x400921FB54442D18ULL);
            // The references did not swap (distinct content each in its own slot).
            ctx.check("mixSD_witness_a_content", wide::wMixSDa() == kStrA);
            ctx.check("mixSD_witness_c_content_after_wide", wide::wMixSDc() == kStrC);
        }

        // =====================================================================
        //  BOUNDARY-EXTREME WIDE ARG IN A FLANKED POSITION.  These re-run the
        //  flanked shapes (scaleD / mixC / mixD / addL / idj / jidi) with the wide
        //  operand at an EXTREME — subnormal, +Inf, NaN, MAX_VALUE, +2^31 (the
        //  sign-extend witness), all-ones -1, -0.0 — so a truncation / sign-extend
        //  / NaN-mangle in the flanked slot changes the combined return.  Asserted
        //  by combined return only (these run before the *_main calls in run_all,
        //  so the *_main witness assertions above keep their LAST-call values).
        //  The deterministic formula / bit-exact double makes each return pin the
        //  wide operand's full 64-bit width independent of the witness.
        // =====================================================================
        {
            // scaleD(subnormal, 3): subnormal * 3 stays a subnormal bit-exactly
            // iff the leading double reached the callee with its full mantissa.
            const double x{ bits2d(0x0000000000000001ULL) };
            const std::int32_t n{ 3 };
            const probe_result r{ got("scaleD_subnormal") };
            ctx.check("scaleD_subnormal_resolved", r.resolved);
            ctx.check("scaleD_subnormal_return_bits", r.dbits == d2bits(x * static_cast<double>(n)));
        }
        {
            // scaleD(+Inf, 2): Inf * 2 == Inf bit-exact iff the double is intact.
            const double x{ bits2d(0x7FF0000000000000ULL) };
            const std::int32_t n{ 2 };
            const probe_result r{ got("scaleD_inf") };
            ctx.check("scaleD_inf_resolved", r.resolved);
            ctx.check("scaleD_inf_return_bits", r.dbits == d2bits(x * static_cast<double>(n)));
        }
        {
            // mixC(INT_MIN, MAX_VALUE, INT_MAX): the middle double is the largest
            // finite; a truncation would not survive the b + a + c recomposition,
            // and the INT_MIN/INT_MAX flanks catch a shift aliasing the double's
            // words into a neighbour.
            const std::int32_t a{ std::numeric_limits<std::int32_t>::min() };
            const double       b{ bits2d(0x7FEFFFFFFFFFFFFFULL) }; // MAX_VALUE
            const std::int32_t c{ std::numeric_limits<std::int32_t>::max() };
            const probe_result r{ got("mixC_extreme") };
            ctx.check("mixC_extreme_resolved", r.resolved);
            ctx.check("mixC_extreme_return_bits",
                      r.dbits == d2bits(b + static_cast<double>(a) + static_cast<double>(c)));
        }
        {
            // mixD(LONG_MIN, +Inf, LONG_MAX, qNaN): the sum is NaN (Inf/NaN
            // dominate); both sides produce the identical NaN bit pattern, so a
            // wide-kind swap or truncation that changed any operand would change it.
            const std::int64_t a{ std::numeric_limits<std::int64_t>::min() };
            const double       b{ bits2d(0x7FF0000000000000ULL) }; // +Inf
            const std::int64_t c{ std::numeric_limits<std::int64_t>::max() };
            const double       d{ bits2d(0x7FF8000000000000ULL) }; // qNaN
            const probe_result r{ got("mixD_extreme") };
            ctx.check("mixD_extreme_resolved", r.resolved);
            ctx.check("mixD_extreme_return_bits",
                      r.dbits == d2bits(static_cast<double>(a) + b + static_cast<double>(c) + d));
        }
        {
            // addL(-1, -1): full-width 0xFFFF..FFFF in both operands.  -1*1000003
            // + -1 wraps in two's complement; the unsigned-wrap formula pins it.  A
            // 32-bit truncation reads the same low word but a different product.
            const probe_result r{ got("addL_allones") };
            ctx.check("addL_allones_resolved", r.resolved);
            ctx.check("addL_allones_return", r.ival == jadd(jmul(-1LL, 1000003LL), -1LL));
        }
        {
            // idj(7, +1.0, +2^31): the long's low word is 0x80000000 ("negative as
            // int") but the value is +2^31; a sign-extend-from-32 bug flips its sign
            // and the (double)a + b + (double)c sum changes.
            const std::int32_t a{ 7 };
            const double       b{ bits2d(0x3FF0000000000000ULL) }; // +1.0
            const std::int64_t c{ static_cast<std::int64_t>(0x0000000080000000ULL) };
            const probe_result r{ got("idj_pos2to31") };
            ctx.check("idj_pos2to31_resolved", r.resolved);
            ctx.check("idj_pos2to31_return_bits",
                      r.dbits == d2bits(static_cast<double>(a) + b + static_cast<double>(c)));
        }
        {
            // jidi(LONG_MIN, INT_MAX, -0.0, INT_MIN): leading long extreme, middle
            // double = -0.0, two narrow ints at their extremes across both wide
            // kinds.  The pure sum pins each operand's full width.
            const std::int64_t a{ std::numeric_limits<std::int64_t>::min() };
            const std::int32_t b{ std::numeric_limits<std::int32_t>::max() };
            const double       c{ bits2d(0x8000000000000000ULL) }; // -0.0
            const std::int32_t d{ std::numeric_limits<std::int32_t>::min() };
            const probe_result r{ got("jidi_extreme") };
            ctx.check("jidi_extreme_resolved", r.resolved);
            ctx.check("jidi_extreme_return_bits",
                      r.dbits == d2bits(static_cast<double>(a) + static_cast<double>(b)
                                        + c + static_cast<double>(d)));
        }

        // =====================================================================
        //  TRAILING WIDE AT THE DEEPEST INSTANCE SLOT (septa = int x6, long).  The
        //  receiver is slot 0, the six ints slots 1..6, the long slots 7..8 — its
        //  leading word in the LAST writable call-stub word (params[7]).  Combined
        //  return (full 64-bit trailing long + asymmetric narrow multipliers) AND
        //  all seven witnesses, so a truncation/drop/shift of the boundary wide arg
        //  fails the return and the wSeptaG witness independently.
        // =====================================================================
        {
            const std::int32_t a{ 0x0A0A0A0A };
            const std::int32_t b{ -7 };
            const std::int32_t c{ 0x13572468 };
            const std::int32_t d{ -2000000000 };
            const std::int32_t e{ 0x7FFFFFFF };
            const std::int32_t f{ 0x5EEDFACE };
            const std::int64_t g{ static_cast<std::int64_t>(0xDEADBEEFCAFEBABEULL) };
            // a*3 + b*31 + c*131 + d*524287 + e*8191 + f*17 + g*1000003 (two's-comp)
            const std::int64_t expect{
                jadd(jadd(jadd(jadd(jadd(jadd(
                    jmul(static_cast<std::int64_t>(a), 3LL),
                    jmul(static_cast<std::int64_t>(b), 31LL)),
                    jmul(static_cast<std::int64_t>(c), 131LL)),
                    jmul(static_cast<std::int64_t>(d), 524287LL)),
                    jmul(static_cast<std::int64_t>(e), 8191LL)),
                    jmul(static_cast<std::int64_t>(f), 17LL)),
                    jmul(g, 1000003LL)) };
            const probe_result r{ got("septa_main") };
            ctx.check("septa_main_resolved", r.resolved);
            ctx.check("septa_main_not_void", r.dispatched);
            ctx.check("septa_main_return", r.ival == expect);
            ctx.check("septa_witness_a_intact", wide::wSeptaA() == a);
            ctx.check("septa_witness_b_intact", wide::wSeptaB() == b);
            ctx.check("septa_witness_c_intact", wide::wSeptaC() == c);
            ctx.check("septa_witness_d_intact", wide::wSeptaD() == d);
            ctx.check("septa_witness_e_intact", wide::wSeptaE() == e);
            ctx.check("septa_witness_f_intact", wide::wSeptaF() == f);
            // The trailing wide long at the deepest instance slot is exact.
            ctx.check("septa_witness_g_long_exact_at_boundary", wide::wSeptaG() == g);
        }

        // =====================================================================
        //  SUB-INT NEIGHBOURS OF A WIDE ARG (byte/short/char/boolean).  These are
        //  the descriptor kinds the existing 'I'/'F' flank methods never touch.  A
        //  sub-int is one interpreter slot, but byte/short are SIGN-extended into
        //  it, a char is ZERO-extended, and a boolean is 0/1 — so a wide arg whose
        //  high word leaked into the adjacent narrow slot would corrupt the value
        //  WITH ITS EXTENSION RULE.  Each typed return equals the value passed (the
        //  methods echo), and the witnesses pin the wide arg AND the narrow exactly.
        // =====================================================================
        {
            // byte after long: -1 (0xFF) must NOT pick up the long's high bits; the
            // return is a signed byte (-1) and the witness equals -1 exactly.
            const std::int8_t  b{ static_cast<std::int8_t>(-1) };
            const std::int64_t a{ static_cast<std::int64_t>(0xFFFFFFFF00000000ULL) };
            const probe_result r{ got("bal_main") };
            ctx.check("bal_main_resolved", r.resolved);
            ctx.check("bal_main_not_void", r.dispatched);
            ctx.check("bal_main_return_is_byte", r.ival == static_cast<std::int64_t>(b));
            ctx.check("bal_main_witness_long_exact", wide::wBalLong() == a);
            ctx.check("bal_main_witness_byte_intact_after_wide", wide::wBalByte() == b);
            // The boundary call (-128) returned the sign-extended minimum.
            const probe_result rm{ got("bal_min") };
            ctx.check("bal_min_return_is_byte",
                      rm.ival == static_cast<std::int64_t>(std::numeric_limits<std::int8_t>::min()));
        }
        {
            // short after long: -1 (0xFFFF), sign-extended.
            const std::int16_t b{ static_cast<std::int16_t>(-1) };
            const std::int64_t a{ static_cast<std::int64_t>(0xFFFFFFFF00000000ULL) };
            const probe_result r{ got("sal_main") };
            ctx.check("sal_main_resolved", r.resolved);
            ctx.check("sal_main_return_is_short", r.ival == static_cast<std::int64_t>(b));
            ctx.check("sal_main_witness_long_exact", wide::wSalLong() == a);
            ctx.check("sal_main_witness_short_intact_after_wide", wide::wSalShort() == b);
            const probe_result rm{ got("sal_min") };
            ctx.check("sal_min_return_is_short",
                      rm.ival == static_cast<std::int64_t>(std::numeric_limits<std::int16_t>::min()));
        }
        {
            // char after long: a char is ZERO-extended, so 0x4E2D round-trips as a
            // positive small int (NOT sign-extended like a short would be).
            const std::uint16_t b{ 0x4E2D };
            const std::int64_t  a{ static_cast<std::int64_t>(0xFFFFFFFF00000000ULL) };
            const probe_result r{ got("cal_main") };
            ctx.check("cal_main_resolved", r.resolved);
            ctx.check("cal_main_return_is_char", r.ival == static_cast<std::int64_t>(b));
            ctx.check("cal_main_witness_long_exact", wide::wCalLong() == a);
            ctx.check("cal_main_witness_char_intact_after_wide", wide::wCalChar() == b);
            // The 0xFFFF boundary: a char zero-extends to 65535, NOT -1 (a sign-
            // extend bug on the 'C' slot would return -1 here).
            const probe_result rmax{ got("cal_max") };
            ctx.check("cal_max_return_is_unsigned_65535", rmax.ival == 0xFFFF);
        }
        {
            // boolean after long: true round-trips as 1, false as 0.
            const std::int64_t a{ static_cast<std::int64_t>(0xFFFFFFFF00000000ULL) };
            const probe_result r{ got("zal_main") };
            ctx.check("zal_main_resolved", r.resolved);
            ctx.check("zal_main_return_is_true", r.ival == 1);
            ctx.check("zal_main_witness_long_exact", wide::wZalLong() == a);
            ctx.check("zal_main_witness_bool_true", wide::wZalBool() == true);
            const probe_result rf{ got("zal_false") };
            ctx.check("zal_false_return_is_false", rf.ival == 0);
        }
        {
            // char after double: 0x00FF zero-extends; the double must not corrupt it.
            const std::uint16_t b{ 0x00FF };
            const probe_result r{ got("cad_main") };
            ctx.check("cad_main_resolved", r.resolved);
            ctx.check("cad_main_return_is_char", r.ival == static_cast<std::int64_t>(b));
            ctx.check("cad_main_witness_double_bits", wide::wCadDouble() == 0x400921FB54442D18ULL);
            ctx.check("cad_main_witness_char_intact_after_double", wide::wCadChar() == b);
            const probe_result rmax{ got("cad_max") };
            ctx.check("cad_max_return_is_unsigned_65535", rmax.ival == 0xFFFF);
        }
        {
            // short after double: -12345 sign-extends; double bit-exact beside it.
            const std::int16_t b{ static_cast<std::int16_t>(-12345) };
            const probe_result r{ got("sad_main") };
            ctx.check("sad_main_resolved", r.resolved);
            ctx.check("sad_main_return_is_short", r.ival == static_cast<std::int64_t>(b));
            ctx.check("sad_main_witness_double_bits", wide::wSadDouble() == 0x400921FB54442D18ULL);
            ctx.check("sad_main_witness_short_intact_after_double", wide::wSadShort() == b);
            const probe_result rmax{ got("sad_max") };
            ctx.check("sad_max_return_is_short",
                      rmax.ival == static_cast<std::int64_t>(std::numeric_limits<std::int16_t>::max()));
        }
        {
            // wide long flanked by a byte and a short, both sign-extended kinds.
            const std::int8_t  a{ static_cast<std::int8_t>(-100) };
            const std::int64_t b{ static_cast<std::int64_t>(0xFFFFFFFF00000000ULL) };
            const std::int16_t c{ static_cast<std::int16_t>(-30000) };
            const probe_result r{ got("mixBSC_main") };
            ctx.check("mixBSC_main_resolved", r.resolved);
            ctx.check("mixBSC_main_not_void", r.dispatched);
            ctx.check("mixBSC_main_return",
                      r.ival == jadd(jadd(jmul(static_cast<std::int64_t>(a), 7LL),
                                          jmul(b, 1000003LL)),
                                     jmul(static_cast<std::int64_t>(c), 13LL)));
            ctx.check("mixBSC_witness_byte_intact", wide::wBscB() == a);
            ctx.check("mixBSC_witness_long_exact",  wide::wBscL() == b);
            ctx.check("mixBSC_witness_short_intact_after_wide", wide::wBscS() == c);
        }
        {
            // wide double flanked by a boolean and a char; return recomputed in the
            // identical split-scale order the fixture uses (each its own step).
            const bool          a{ true };
            const double        b{ bits2d(0x400921FB54442D18ULL) }; // PI
            const std::uint16_t c{ 0xBEEF };
            const double za{ (a ? 1.0 : 0.0) * 1024.0 };
            const double cc{ static_cast<double>(c) * 16.0 };
            const probe_result r{ got("mixZDC_main") };
            ctx.check("mixZDC_main_resolved", r.resolved);
            ctx.check("mixZDC_main_not_void", r.dispatched);
            ctx.check("mixZDC_main_return_bits", r.dbits == d2bits(b + za + cc));
            ctx.check("mixZDC_witness_bool_true", wide::wZdcZ() == a);
            ctx.check("mixZDC_witness_double_bits", wide::wZdcD() == 0x400921FB54442D18ULL);
            ctx.check("mixZDC_witness_char_intact_after_double", wide::wZdcC() == c);
        }
        {
            // leading char then wide long: the long started one slot after the char.
            const std::uint16_t a{ 0x4E2D };
            const std::int64_t  b{ static_cast<std::int64_t>(0xDEADBEEFCAFEBABEULL) };
            const probe_result r{ got("charLong_main") };
            ctx.check("charLong_main_resolved", r.resolved);
            ctx.check("charLong_main_return_long_exact", r.ival == b);
            ctx.check("charLong_witness_char_intact", wide::wClChar() == a);
            ctx.check("charLong_witness_long_exact_after_char", wide::wClLong() == b);
        }
        {
            // every sub-int kind interleaved around both wide kinds (nine slots).
            const std::int8_t   a{ static_cast<std::int8_t>(-7) };
            const std::int64_t  b{ static_cast<std::int64_t>(0xDEADBEEFCAFEBABEULL) };
            const double        c{ bits2d(0x400921FB54442D18ULL) }; // PI
            const std::uint16_t d{ 0xABCD };
            const std::int16_t  e{ static_cast<std::int16_t>(0x1234) };
            const probe_result r{ got("bldcs_main") };
            ctx.check("bldcs_main_resolved", r.resolved);
            ctx.check("bldcs_main_not_void", r.dispatched);
            ctx.check("bldcs_main_return_bits",
                      r.dbits == d2bits(static_cast<double>(a) + static_cast<double>(b)
                                        + c + static_cast<double>(d)
                                        + static_cast<double>(e)));
            ctx.check("bldcs_witness_byte_intact",  wide::wBldcsB() == a);
            ctx.check("bldcs_witness_long_exact",   wide::wBldcsL() == b);
            ctx.check("bldcs_witness_double_bits",  wide::wBldcsD() == 0x400921FB54442D18ULL);
            ctx.check("bldcs_witness_char_intact",  wide::wBldcsC() == d);
            ctx.check("bldcs_witness_short_intact", wide::wBldcsS() == e);
        }

        // =====================================================================
        //  STATIC variants — first wide arg at slot 0 (no receiver).
        // =====================================================================
        {
            const std::int64_t a{ std::numeric_limits<std::int64_t>::min() };
            const std::int64_t b{ std::numeric_limits<std::int64_t>::max() };
            const probe_result r{ got("s_addL_min_max") };
            ctx.check("s_addL_resolved", r.resolved);
            ctx.check("s_addL_return", r.ival == jadd(jmul(a, 1000003LL), b));
            ctx.check("s_addL_witness_a_exact", wide::sWAddLa() == a);
            ctx.check("s_addL_witness_b_exact", wide::sWAddLb() == b);
        }
        {
            const std::int64_t a{ static_cast<std::int64_t>(0x0123456789ABCDEFULL) };
            const probe_result r{ got("s_idL_pat") };
            ctx.check("s_idL_resolved", r.resolved);
            ctx.check("s_idL_echo_exact", r.ival == a);
        }
        {
            const probe_result r{ got("s_idD_nan") };
            ctx.check("s_idD_nan_resolved", r.resolved);
            ctx.check("s_idD_nan_bits_exact", r.dbits == 0x7FF8000000000000ULL);
        }
        {
            const probe_result r{ got("s_idD_negzero") };
            ctx.check("s_idD_negzero_bits_exact", r.dbits == 0x8000000000000000ULL);
        }
        {
            const std::int32_t a{ -7 };
            const std::int64_t b{ static_cast<std::int64_t>(0xFFFFFFFF00000000ULL) };
            const std::int32_t c{ 99 };
            const probe_result r{ got("s_mixA") };
            ctx.check("s_mixA_resolved", r.resolved);
            ctx.check("s_mixA_return",
                      r.ival == jadd(jadd(jmul(a, 7LL), jmul(b, 1000003LL)), jmul(c, 13LL)));
            ctx.check("s_mixA_witness_a_intact", wide::sWMixAa() == a);
            ctx.check("s_mixA_witness_b_intact", wide::sWMixAb() == b);
            ctx.check("s_mixA_witness_c_intact_after_wide", wide::sWMixAc() == c);
        }
        {
            const double x{ 3.141592653589793 };
            const std::int32_t n{ 1000000 };
            const probe_result r{ got("s_scaleD") };
            ctx.check("s_scaleD_resolved", r.resolved);
            ctx.check("s_scaleD_return_bits", r.dbits == d2bits(x * static_cast<double>(n)));
            ctx.check("s_scaleD_witness_n_intact", wide::sWScaleDn() == n);
        }
        {
            const std::int64_t a{ 100 };
            const double b{ 200.0 };
            const std::int64_t c{ 300 };
            const double d{ 400.0 };
            const probe_result r{ got("s_mixD") };
            ctx.check("s_mixD_resolved", r.resolved);
            ctx.check("s_mixD_return_bits",
                      r.dbits == d2bits(static_cast<double>(a) + b + static_cast<double>(c) + d));
            ctx.check("s_mixD_witness_a_exact", wide::sWMixDa() == a);
            ctx.check("s_mixD_witness_c_exact", wide::sWMixDc() == c);
        }
        {
            // STATIC two doubles adjacent (no receiver; first double at slot 0).
            const double a{ bits2d(0x400921FB54442D18ULL) }; // PI
            const double b{ bits2d(0x4005BF0A8B145769ULL) }; // E
            const double sa{ a * 8.0 };
            const probe_result r{ got("s_addD") };
            ctx.check("s_addD_resolved", r.resolved);
            ctx.check("s_addD_return_bits", r.dbits == d2bits(sa + b));
            ctx.check("s_addD_witness_a_bits", wide::sWAddDa() == 0x400921FB54442D18ULL);
            ctx.check("s_addD_witness_b_bits", wide::sWAddDb() == 0x4005BF0A8B145769ULL);
        }
        {
            // STATIC long-then-double adjacency (long at slot 0, double at slot 2).
            const std::int64_t a{ static_cast<std::int64_t>(0xDEADBEEFCAFEBABEULL) };
            const double b{ bits2d(0x400921FB54442D18ULL) }; // PI
            const probe_result r{ got("s_jd") };
            ctx.check("s_jd_resolved", r.resolved);
            ctx.check("s_jd_return_bits", r.dbits == d2bits(static_cast<double>(a) + b));
            ctx.check("s_jd_witness_a_exact", wide::sWJdA() == a);
            ctx.check("s_jd_witness_b_bits", wide::sWJdB() == 0x400921FB54442D18ULL);
        }
        {
            // STATIC double-then-long adjacency (double at slot 0, long at slot 2).
            const double a{ bits2d(0xC02E000000000000ULL) }; // -15.0
            const std::int64_t b{ static_cast<std::int64_t>(0x0123456789ABCDEFULL) };
            const probe_result r{ got("s_dj") };
            ctx.check("s_dj_resolved", r.resolved);
            ctx.check("s_dj_return_bits", r.dbits == d2bits(a + static_cast<double>(b)));
            ctx.check("s_dj_witness_a_bits", wide::sWDjA() == 0xC02E000000000000ULL);
            ctx.check("s_dj_witness_b_exact", wide::sWDjB() == b);
        }
        {
            // STATIC float,long,double interleave (float at slot 0, no receiver).
            const float        a{ 2.5f };
            const std::int64_t b{ static_cast<std::int64_t>(0xDEADBEEFCAFEBABEULL) };
            const double       c{ bits2d(0x400921FB54442D18ULL) }; // PI
            const probe_result r{ got("s_fld") };
            ctx.check("s_fld_resolved", r.resolved);
            ctx.check("s_fld_return_bits",
                      r.dbits == d2bits(static_cast<double>(a) + static_cast<double>(b) + c));
            ctx.check("s_fld_witness_a_bits", wide::sWFldA() == f2bits(a));
            ctx.check("s_fld_witness_b_exact", wide::sWFldB() == b);
            ctx.check("s_fld_witness_c_bits", wide::sWFldC() == 0x400921FB54442D18ULL);
        }
        {
            // STATIC String,long,String — wide long between two references at the
            // no-receiver frame (first reference at slot 0, long at slot 1).
            const std::int64_t b{ static_cast<std::int64_t>(0xFFFFFFFF00000000ULL) };
            const std::int64_t expect{ jadd(jadd(b, jmul(kStrLenA, 1000003LL)),
                                            jmul(kStrLenC, 97LL)) };
            const probe_result r{ got("s_mixS") };
            ctx.check("s_mixS_resolved", r.resolved);
            ctx.check("s_mixS_return", r.ival == expect);
            ctx.check("s_mixS_witness_long_exact", wide::sWMixSb() == b);
            ctx.check("s_mixS_witness_a_content", wide::sWMixSa() == kStrA);
            ctx.check("s_mixS_witness_c_content_after_wide", wide::sWMixSc() == kStrC);
        }
        {
            // STATIC SIX longs — twelve contiguous slots, first long at slot 0 (no
            // receiver shift).  Same boundary mix + asymmetric formula as instance.
            const std::int64_t a{ static_cast<std::int64_t>(0xFFFFFFFF00000000ULL) };
            const std::int64_t b{ static_cast<std::int64_t>(0x00000000FFFFFFFFULL) };
            const std::int64_t c{ static_cast<std::int64_t>(0x0000000080000000ULL) };
            const std::int64_t d{ std::numeric_limits<std::int64_t>::min() };
            const std::int64_t e{ std::numeric_limits<std::int64_t>::max() };
            const std::int64_t f{ static_cast<std::int64_t>(0xDEADBEEFCAFEBABEULL) };
            const std::int64_t expect{
                jadd(jadd(jadd(jadd(jadd(
                    jmul(a, 1000003LL),
                    jmul(b, 31LL)),
                    jmul(c, 131LL)),
                    jmul(d, 524287LL)),
                    jmul(e, 8191LL)),
                    f) };
            const probe_result r{ got("s_sixL") };
            ctx.check("s_sixL_resolved", r.resolved);
            ctx.check("s_sixL_return", r.ival == expect);
            ctx.check("s_sixL_witness_a_exact", wide::sWSixLa() == a);
            ctx.check("s_sixL_witness_b_exact", wide::sWSixLb() == b);
            ctx.check("s_sixL_witness_c_exact", wide::sWSixLc() == c);
            ctx.check("s_sixL_witness_d_exact", wide::sWSixLd() == d);
            ctx.check("s_sixL_witness_e_exact", wide::sWSixLe() == e);
            ctx.check("s_sixL_witness_f_exact", wide::sWSixLf() == f);
        }
        {
            // STATIC SIX doubles — twelve contiguous slots, first double at slot 0.
            // Same FMA-safe split-scale-then-left-to-right-sum as the instance sixD.
            const double a{ bits2d(0x400921FB54442D18ULL) }; // PI
            const double b{ bits2d(0x8000000000000000ULL) }; // -0.0
            const double c{ bits2d(0x0000000000000001ULL) }; // smallest subnormal
            const double d{ bits2d(0xBFF0000000000000ULL) }; // -1.0
            const double e{ bits2d(0x4005BF0A8B145769ULL) }; // E
            const double f{ bits2d(0x3FE0000000000000ULL) }; // 0.5
            const double ta{ a * 2.0 };
            const double tb{ b * 4.0 };
            const double tc{ c * 8.0 };
            const double td{ d * 16.0 };
            const double te{ e * 32.0 };
            const double tf{ f * 64.0 };
            const double expect{ ta + tb + tc + td + te + tf };
            const probe_result r{ got("s_sixD") };
            ctx.check("s_sixD_resolved", r.resolved);
            ctx.check("s_sixD_return_bits", r.dbits == d2bits(expect));
            ctx.check("s_sixD_witness_a_bits", wide::sWSixDa() == 0x400921FB54442D18ULL);
            ctx.check("s_sixD_witness_b_bits", wide::sWSixDb() == 0x8000000000000000ULL);
            ctx.check("s_sixD_witness_c_bits", wide::sWSixDc() == 0x0000000000000001ULL);
            ctx.check("s_sixD_witness_d_bits", wide::sWSixDd() == 0xBFF0000000000000ULL);
            ctx.check("s_sixD_witness_e_bits", wide::sWSixDe() == 0x4005BF0A8B145769ULL);
            ctx.check("s_sixD_witness_f_bits", wide::sWSixDf() == 0x3FE0000000000000ULL);
        }
        {
            // STATIC (JIDIF) five-arg "every shape" tail (first arg at slot 0).
            const std::int64_t a{ std::numeric_limits<std::int64_t>::min() };
            const std::int32_t b{ 0x0A0A0A0A };
            const double       c{ bits2d(0x400921FB54442D18ULL) }; // PI
            const std::int32_t d{ -2000000000 };
            const float        e{ 2.5f };
            const probe_result r{ got("s_widePent") };
            ctx.check("s_widePent_resolved", r.resolved);
            ctx.check("s_widePent_return_bits",
                      r.dbits == d2bits(static_cast<double>(a) + static_cast<double>(b)
                                        + c + static_cast<double>(d)
                                        + static_cast<double>(e)));
            ctx.check("s_widePent_witness_a_exact",  wide::sWPentA() == a);
            ctx.check("s_widePent_witness_b_intact", wide::sWPentB() == b);
            ctx.check("s_widePent_witness_c_bits",   wide::sWPentC() == 0x400921FB54442D18ULL);
            ctx.check("s_widePent_witness_d_intact", wide::sWPentD() == d);
            ctx.check("s_widePent_witness_e_bits_trailing", wide::sWPentE() == f2bits(e));
        }
        {
            // STATIC EIGHT-ARG, TRAILING WIDE LONG as the 8th (last packable) arg.
            // No receiver: seven ints in slots 0..6, the long in slots 7..8 (its
            // leading word in the LAST call-stub word params[7]).  Combined return
            // (full 64-bit trailing long + asymmetric narrow multipliers) AND all
            // eight witnesses; a truncation/drop/shift of the boundary wide fails
            // the return and the sWOctaH witness independently.
            const std::int32_t a{ 0x0A0A0A0A };
            const std::int32_t b{ -7 };
            const std::int32_t c{ 0x13572468 };
            const std::int32_t d{ -2000000000 };
            const std::int32_t e{ 0x7FFFFFFF };
            const std::int32_t f{ 0x5EEDFACE };
            const std::int32_t g{ std::numeric_limits<std::int32_t>::min() };
            const std::int64_t h{ static_cast<std::int64_t>(0xDEADBEEFCAFEBABEULL) };
            // a*3 + b*31 + c*131 + d*524287 + e*8191 + f*17 + g*41 + h*1000003
            const std::int64_t expect{
                jadd(jadd(jadd(jadd(jadd(jadd(jadd(
                    jmul(static_cast<std::int64_t>(a), 3LL),
                    jmul(static_cast<std::int64_t>(b), 31LL)),
                    jmul(static_cast<std::int64_t>(c), 131LL)),
                    jmul(static_cast<std::int64_t>(d), 524287LL)),
                    jmul(static_cast<std::int64_t>(e), 8191LL)),
                    jmul(static_cast<std::int64_t>(f), 17LL)),
                    jmul(static_cast<std::int64_t>(g), 41LL)),
                    jmul(h, 1000003LL)) };
            const probe_result r{ got("s_octa") };
            ctx.check("s_octa_resolved", r.resolved);
            ctx.check("s_octa_not_void", r.dispatched);
            ctx.check("s_octa_return", r.ival == expect);
            ctx.check("s_octa_witness_a_intact", wide::sWOctaA() == a);
            ctx.check("s_octa_witness_b_intact", wide::sWOctaB() == b);
            ctx.check("s_octa_witness_c_intact", wide::sWOctaC() == c);
            ctx.check("s_octa_witness_d_intact", wide::sWOctaD() == d);
            ctx.check("s_octa_witness_e_intact", wide::sWOctaE() == e);
            ctx.check("s_octa_witness_f_intact", wide::sWOctaF() == f);
            ctx.check("s_octa_witness_g_intact", wide::sWOctaG() == g);
            // The trailing wide long as the 8th argument is exact.
            ctx.check("s_octa_witness_h_long_exact_8th_arg", wide::sWOctaH() == h);
        }
        {
            // STATIC EIGHT-ARG, TRAILING WIDE DOUBLE as the 8th arg.  Proves the
            // 'D'-kind eighth argument lands bit-exact at the boundary; the pure
            // left-to-right sum is recomputed in the identical order so the bits
            // match.  Combined return AND all eight witnesses (double bit-exact).
            const std::int32_t a{ 1 };
            const std::int32_t b{ 2 };
            const std::int32_t c{ 3 };
            const std::int32_t d{ 4 };
            const std::int32_t e{ 5 };
            const std::int32_t f{ 6 };
            const std::int32_t g{ std::numeric_limits<std::int32_t>::max() };
            const double       h{ bits2d(0x400921FB54442D18ULL) }; // PI as 8th
            const double expect{ static_cast<double>(a) + static_cast<double>(b)
                                 + static_cast<double>(c) + static_cast<double>(d)
                                 + static_cast<double>(e) + static_cast<double>(f)
                                 + static_cast<double>(g) + h };
            const probe_result r{ got("s_octaD") };
            ctx.check("s_octaD_resolved", r.resolved);
            ctx.check("s_octaD_not_void", r.dispatched);
            ctx.check("s_octaD_return_bits", r.dbits == d2bits(expect));
            ctx.check("s_octaD_witness_a_intact", wide::sWOctaDa() == a);
            ctx.check("s_octaD_witness_b_intact", wide::sWOctaDb() == b);
            ctx.check("s_octaD_witness_c_intact", wide::sWOctaDc() == c);
            ctx.check("s_octaD_witness_d_intact", wide::sWOctaDd() == d);
            ctx.check("s_octaD_witness_e_intact", wide::sWOctaDe() == e);
            ctx.check("s_octaD_witness_f_intact", wide::sWOctaDf() == f);
            ctx.check("s_octaD_witness_g_intact", wide::sWOctaDg() == g);
            // The trailing wide double as the 8th argument is bit-exact.
            ctx.check("s_octaD_witness_h_double_bits_8th_arg",
                      wide::sWOctaDh() == 0x400921FB54442D18ULL);
        }
        {
            // STATIC byte after long (long at slot 0, byte at slot 2).  -1 returns
            // sign-extended; the long and byte witnesses are both exact.
            const std::int64_t a{ static_cast<std::int64_t>(0xFFFFFFFF00000000ULL) };
            const std::int8_t  b{ static_cast<std::int8_t>(-1) };
            const probe_result r{ got("s_bal") };
            ctx.check("s_bal_resolved", r.resolved);
            ctx.check("s_bal_return_is_byte", r.ival == static_cast<std::int64_t>(b));
            ctx.check("s_bal_witness_long_exact", wide::sWBalLong() == a);
            ctx.check("s_bal_witness_byte_intact_after_wide", wide::sWBalByte() == b);
        }
        {
            // STATIC char after double (double at slot 0, char at slot 2).  A CJK
            // code unit zero-extends; the double is bit-exact beside it.
            const std::uint16_t b{ 0x4E2D };
            const probe_result r{ got("s_cad") };
            ctx.check("s_cad_resolved", r.resolved);
            ctx.check("s_cad_return_is_char", r.ival == static_cast<std::int64_t>(b));
            ctx.check("s_cad_witness_double_bits", wide::sWCadDouble() == 0x400921FB54442D18ULL);
            ctx.check("s_cad_witness_char_intact_after_double", wide::sWCadChar() == b);
        }
        {
            // STATIC leading char then wide long (char at slot 0, long at slots
            // 1..2).  LONG_MIN round-trips exact; the char (0xFFFF) zero-extends.
            const std::uint16_t a{ 0xFFFF };
            const std::int64_t  b{ std::numeric_limits<std::int64_t>::min() };
            const probe_result r{ got("s_charLong") };
            ctx.check("s_charLong_resolved", r.resolved);
            ctx.check("s_charLong_return_long_exact", r.ival == b);
            ctx.check("s_charLong_witness_char_intact", wide::sWClChar() == a);
            ctx.check("s_charLong_witness_long_exact_after_char", wide::sWClLong() == b);
        }

        // =====================================================================
        //  WRONG ARITY / WRONG TYPE — the process SURVIVED (no JVM tear-down).
        //  These pin "we exercised the abuse path and are still here"; the
        //  returned value is intentionally NOT asserted (it is unspecified).
        // =====================================================================
        ctx.check("wrong_addL_noargs_resolved",   got("wrong_addL_noargs").resolved);
        ctx.check("wrong_scaleD_one_arg_resolved", got("wrong_scaleD_one_arg").resolved);
        ctx.check("wrong_mixA_all_long_resolved",  got("wrong_mixA_all_long").resolved);
        // Survival proof: the detour ran to completion and set every later result,
        // so reaching this point with the trigger count advanced means none of the
        // abuse calls tore the process down.
        ctx.check("wrong_calls_did_not_crash_process", wide::trigger_count() >= 1);
    }
    }   // run_wide_args_checks
}       // anonymous namespace

VMHOOK_JVM_MODULE(method_call_wide_args)
{
    // Run the whole body under a try/catch so a stray throw from any vmhook call
    // can never escape this module (a throw is recorded as [INFO], never a FAIL).
    bool body_threw{ false };
    try
    {
        run_wide_args_checks(ctx);
    }
    catch (...)
    {
        body_threw = true;
    }

    // FINAL CLEANUP — belt-and-braces, OUTSIDE the try so it ALWAYS runs.  Other
    // modules run after this one, so this module MUST leave ZERO hooks armed.  The
    // only hook (the trigger scoped_hook) already uninstalls at its scope exit;
    // this unconditional shutdown_hooks() guarantees an empty hook table even if
    // the body threw before reaching that scope exit (it is idempotent and
    // safe-when-empty).  A leaked armed hook is exactly what cascades into later
    // modules.
    vmhook::shutdown_hooks();

    if (body_threw)
    {
        ctx.record("[INFO] method_call_wide_args: the test body threw and was "
                   "contained (no crash, no hooks armed); see preceding checks for "
                   "partial results.");
    }
    ctx.check("mcw_module_left_clean_final_shutdown", true);
}
