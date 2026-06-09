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

    // The exhaustive long boundary set.  The last two are the halves a 32-bit
    // truncation bug would confuse (high-only vs low-only).
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
    };

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
