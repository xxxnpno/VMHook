// return_set_primitives JVM test module — exhaustively exercises
// vmhook::return_value::set(value) (the force-return path) for every primitive
// return type on a LIVE JVM.
//
// Feature under test: vmhook/ext/vmhook/vmhook.hpp:1147-1176.
//   return_value::set<T>(value) sets return_slot::cancel = true and writes the
//   user value into the 64-bit return_slot::retval cell.  Signed integrals < 8
//   bytes are sign-extended via static_cast<int64_t>; everything else takes a
//   zero-fill + memcpy path.  The trampoline epilogue
//   (vmhook.hpp:5314-5315 Win64 / 5411-5412 SysV) loads that cell into BOTH rax
//   (integer return) and xmm0 (float/double return), so the same slot bits
//   serve every Java return descriptor.
//
// Strategy: the fixture is a dumb actor that calls each orig* method once and
// stores what the Java caller OBSERVED into a per-type field.  Each orig*
// method returns a fixed value the native side NEVER forces, so a passing
// check proves the original body was skipped and the forced slot was delivered.
// This module re-arms its 16 hooks (8 instance + 8 static) with a fresh value
// vector each "round", runs ONE run_probe handshake, then asserts every
// observed field equals what it forced — covering canonical, boundary, and
// IEEE-754 special values across many rounds.
//
// Mirrors pilot.cpp's register_class + scoped_hook + run_probe shape.  Uses
// scoped_hook only (never shutdown_hooks): each round's handles live in a
// nested block and disarm when the block ends, so the next round installs
// clean.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>

namespace
{
    // Wrapper for vmhook.fixtures.ReturnSetPrimitives.
    class rsp_fixture : public vmhook::object<rsp_fixture>
    {
    public:
        explicit rsp_fixture(vmhook::oop_t instance) noexcept
            : vmhook::object<rsp_fixture>{ instance }
        {
        }

        static auto set_go(bool value) -> void { static_field("go")->set(value); }
        static auto get_done() -> bool          { return static_field("done")->get(); }
        static auto set_done(bool value) -> void { static_field("done")->set(value); }

        // Observed-value readers (instance dispatch path).
        static auto obs_bool()   -> bool          { return static_field("obsBool")->get(); }
        static auto obs_byte()   -> std::int8_t   { return static_field("obsByte")->get(); }
        static auto obs_short()  -> std::int16_t  { return static_field("obsShort")->get(); }
        static auto obs_int()    -> std::int32_t  { return static_field("obsInt")->get(); }
        static auto obs_int_readback() -> std::int32_t { return static_field("obsIntReadback")->get(); }
        static auto obs_long()   -> std::int64_t  { return static_field("obsLong")->get(); }
        static auto obs_float()  -> float         { return static_field("obsFloat")->get(); }
        static auto obs_double() -> double         { return static_field("obsDouble")->get(); }
        static auto obs_char()   -> std::uint16_t { return static_field("obsChar")->get(); }

        // Observed-value readers (static dispatch path).
        static auto obs_s_bool()   -> bool          { return static_field("obsStaticBool")->get(); }
        static auto obs_s_byte()   -> std::int8_t   { return static_field("obsStaticByte")->get(); }
        static auto obs_s_short()  -> std::int16_t  { return static_field("obsStaticShort")->get(); }
        static auto obs_s_int()    -> std::int32_t  { return static_field("obsStaticInt")->get(); }
        static auto obs_s_long()   -> std::int64_t  { return static_field("obsStaticLong")->get(); }
        static auto obs_s_float()  -> float         { return static_field("obsStaticFloat")->get(); }
        static auto obs_s_double() -> double         { return static_field("obsStaticDouble")->get(); }
        static auto obs_s_char()   -> std::uint16_t { return static_field("obsStaticChar")->get(); }

        static auto saw_exception() -> bool        { return static_field("sawException")->get(); }
        static auto round_count()   -> std::int32_t { return static_field("roundCount")->get(); }
    };

    // ---- One value vector forced in a given round -------------------------
    struct forced_values
    {
        bool          b;
        std::int8_t   by;
        std::int16_t  s;
        std::int32_t  i;
        std::int64_t  l;
        float         f;
        double        d;
        std::uint16_t c; // jchar is unsigned 16-bit; we force a char16_t value.
    };

    // Per-hook fire counters + self-observation, reset before each round.
    std::atomic<int>  g_inst_fires{ 0 };
    std::atomic<int>  g_stat_fires{ 0 };
    std::atomic<bool> g_inst_all_saw_self{ true };

    auto reset_round_counters() -> void
    {
        g_inst_fires.store(0, std::memory_order_relaxed);
        g_stat_fires.store(0, std::memory_order_relaxed);
        g_inst_all_saw_self.store(true, std::memory_order_relaxed);
    }

    auto note_self(const std::unique_ptr<rsp_fixture>& self) -> void
    {
        g_inst_fires.fetch_add(1, std::memory_order_relaxed);
        if (self == nullptr)
        {
            g_inst_all_saw_self.store(false, std::memory_order_relaxed);
        }
    }

    auto note_static() -> void
    {
        g_stat_fires.fetch_add(1, std::memory_order_relaxed);
    }

    // Bit-exact float/double comparison (so NaN==NaN and -0.0 != +0.0 are
    // detected): the JVM caller must receive the EXACT bit pattern we forced.
    auto same_bits(float a, float b) -> bool
    {
        std::uint32_t ua{}, ub{};
        std::memcpy(&ua, &a, sizeof(ua));
        std::memcpy(&ub, &b, sizeof(ub));
        return ua == ub;
    }
    auto same_bits(double a, double b) -> bool
    {
        std::uint64_t ua{}, ub{};
        std::memcpy(&ua, &a, sizeof(ua));
        std::memcpy(&ub, &b, sizeof(ub));
        return ua == ub;
    }

    // Reinterpret a raw 32/64-bit pattern AS a float/double.  Used to forge an
    // exact IEEE-754 bit pattern (e.g. a NaN with a SPECIFIC mantissa payload, a
    // chosen subnormal) without relying on a literal that a compiler might fold.
    // The value forced into the slot and the value the JVM caller must observe
    // are then compared bit-for-bit by same_bits().
    auto bits_to_float(std::uint32_t bits) -> float
    {
        float out{};
        std::memcpy(&out, &bits, sizeof(out));
        return out;
    }
    auto bits_to_double(std::uint64_t bits) -> double
    {
        double out{};
        std::memcpy(&out, &bits, sizeof(out));
        return out;
    }

    // Arm all 16 hooks for one value vector, run ONE probe, leave the observed
    // fields populated for the caller to assert on.  Returns whether the probe
    // completed.  All handles are local, so they disarm when this function
    // returns — the next round re-installs cleanly without shutdown_hooks().
    auto run_round(vmhook_test::context& ctx,
                   const std::string&    tag,
                   const forced_values&  v) -> bool
    {
        reset_round_counters();
        rsp_fixture::set_done(false);

        // INSTANCE hooks: signature is (return_value&, const unique_ptr<T>&).
        auto h_bool  { vmhook::scoped_hook<rsp_fixture>("origBool",
            [v](vmhook::return_value& r, const std::unique_ptr<rsp_fixture>& self)
            { note_self(self); r.set(v.b); }) };
        auto h_byte  { vmhook::scoped_hook<rsp_fixture>("origByte",
            [v](vmhook::return_value& r, const std::unique_ptr<rsp_fixture>& self)
            { note_self(self); r.set(v.by); }) };
        auto h_short { vmhook::scoped_hook<rsp_fixture>("origShort",
            [v](vmhook::return_value& r, const std::unique_ptr<rsp_fixture>& self)
            { note_self(self); r.set(v.s); }) };
        auto h_int   { vmhook::scoped_hook<rsp_fixture>("origInt",
            [v](vmhook::return_value& r, const std::unique_ptr<rsp_fixture>& self)
            { note_self(self); r.set(v.i); }) };
        auto h_long  { vmhook::scoped_hook<rsp_fixture>("origLong",
            [v](vmhook::return_value& r, const std::unique_ptr<rsp_fixture>& self)
            { note_self(self); r.set(v.l); }) };
        auto h_float { vmhook::scoped_hook<rsp_fixture>("origFloat",
            [v](vmhook::return_value& r, const std::unique_ptr<rsp_fixture>& self)
            { note_self(self); r.set(v.f); }) };
        auto h_double{ vmhook::scoped_hook<rsp_fixture>("origDouble",
            [v](vmhook::return_value& r, const std::unique_ptr<rsp_fixture>& self)
            { note_self(self); r.set(v.d); }) };
        // jchar is unsigned 16-bit: force a char16_t (NOT plain 'char', whose
        // signedness is implementation-defined — see audit Bug #1).
        auto h_char  { vmhook::scoped_hook<rsp_fixture>("origChar",
            [v](vmhook::return_value& r, const std::unique_ptr<rsp_fixture>& self)
            { note_self(self); r.set(static_cast<char16_t>(v.c)); }) };

        // STATIC hooks: signature is (return_value&) — no 'this'.
        auto hs_bool  { vmhook::scoped_hook<rsp_fixture>("origStaticBool",
            [v](vmhook::return_value& r) { note_static(); r.set(v.b); }) };
        auto hs_byte  { vmhook::scoped_hook<rsp_fixture>("origStaticByte",
            [v](vmhook::return_value& r) { note_static(); r.set(v.by); }) };
        auto hs_short { vmhook::scoped_hook<rsp_fixture>("origStaticShort",
            [v](vmhook::return_value& r) { note_static(); r.set(v.s); }) };
        auto hs_int   { vmhook::scoped_hook<rsp_fixture>("origStaticInt",
            [v](vmhook::return_value& r) { note_static(); r.set(v.i); }) };
        auto hs_long  { vmhook::scoped_hook<rsp_fixture>("origStaticLong",
            [v](vmhook::return_value& r) { note_static(); r.set(v.l); }) };
        auto hs_float { vmhook::scoped_hook<rsp_fixture>("origStaticFloat",
            [v](vmhook::return_value& r) { note_static(); r.set(v.f); }) };
        auto hs_double{ vmhook::scoped_hook<rsp_fixture>("origStaticDouble",
            [v](vmhook::return_value& r) { note_static(); r.set(v.d); }) };
        auto hs_char  { vmhook::scoped_hook<rsp_fixture>("origStaticChar",
            [v](vmhook::return_value& r) { note_static(); r.set(static_cast<char16_t>(v.c)); }) };

        // Each round must (re)install all 16 hooks; if any failed to install
        // the round's value checks are meaningless, so surface it once.
        const bool all_installed{
            h_bool.installed()  && h_byte.installed()  && h_short.installed() &&
            h_int.installed()   && h_long.installed()  && h_float.installed() &&
            h_double.installed()&& h_char.installed()  &&
            hs_bool.installed() && hs_byte.installed() && hs_short.installed() &&
            hs_int.installed()  && hs_long.installed() && hs_float.installed() &&
            hs_double.installed()&& hs_char.installed() };
        ctx.check(tag + "_all_16_hooks_installed", all_installed);

        const bool done{ ctx.run_probe(
            [](bool value) { rsp_fixture::set_go(value); },
            []() { return rsp_fixture::get_done(); }) };
        ctx.check(tag + "_probe_completed", done);
        return done;
    }

    // Assert every observed field == the forced value for this round.
    auto check_round(vmhook_test::context& ctx,
                     const std::string&    tag,
                     const forced_values&  v) -> void
    {
        // --- Each instance hook fires once per Java call.  The fixture invokes
        // origInt() TWICE (obsInt + obsIntReadback stability readback) and the
        // other 7 origs once, so the instance path yields 9 fires; the static
        // path calls each of its 8 origs exactly once → 8 fires.
        ctx.check(tag + "_instance_hooks_fired", g_inst_fires.load() == 9);
        ctx.check(tag + "_static_hooks_fired",   g_stat_fires.load() == 8);
        ctx.check(tag + "_instance_hooks_saw_self", g_inst_all_saw_self.load());
        ctx.check(tag + "_no_java_exception",       !rsp_fixture::saw_exception());

        // --- INSTANCE: forced value observed by the Java caller.
        ctx.check(tag + "_inst_bool",   rsp_fixture::obs_bool()  == v.b);
        ctx.check(tag + "_inst_byte",   rsp_fixture::obs_byte()  == v.by);
        ctx.check(tag + "_inst_short",  rsp_fixture::obs_short() == v.s);
        ctx.check(tag + "_inst_int",    rsp_fixture::obs_int()   == v.i);
        ctx.check(tag + "_inst_long",   rsp_fixture::obs_long()  == v.l);
        ctx.check(tag + "_inst_float",  same_bits(rsp_fixture::obs_float(),  v.f));
        ctx.check(tag + "_inst_double", same_bits(rsp_fixture::obs_double(), v.d));
        ctx.check(tag + "_inst_char",   rsp_fixture::obs_char()  == v.c);
        // Stability: a second read of the forced int yields the same value.
        ctx.check(tag + "_inst_int_stable", rsp_fixture::obs_int_readback() == v.i);

        // --- STATIC: forced value observed by the Java caller.
        ctx.check(tag + "_stat_bool",   rsp_fixture::obs_s_bool()  == v.b);
        ctx.check(tag + "_stat_byte",   rsp_fixture::obs_s_byte()  == v.by);
        ctx.check(tag + "_stat_short",  rsp_fixture::obs_s_short() == v.s);
        ctx.check(tag + "_stat_int",    rsp_fixture::obs_s_int()   == v.i);
        ctx.check(tag + "_stat_long",   rsp_fixture::obs_s_long()  == v.l);
        ctx.check(tag + "_stat_float",  same_bits(rsp_fixture::obs_s_float(),  v.f));
        ctx.check(tag + "_stat_double", same_bits(rsp_fixture::obs_s_double(), v.d));
        ctx.check(tag + "_stat_char",   rsp_fixture::obs_s_char()  == v.c);
    }

    auto run_and_check(vmhook_test::context& ctx,
                       const std::string&    tag,
                       const forced_values&  v) -> void
    {
        if (run_round(ctx, tag, v))
        {
            check_round(ctx, tag, v);
        }
    }
}

VMHOOK_JVM_MODULE(return_set_primitives)
{
    vmhook::register_class<rsp_fixture>("vmhook/fixtures/ReturnSetPrimitives");

    // Constants for readability.
    constexpr float  f_pos_inf{  std::numeric_limits<float>::infinity() };
    constexpr float  f_neg_inf{ -std::numeric_limits<float>::infinity() };
    constexpr double d_pos_inf{  std::numeric_limits<double>::infinity() };
    constexpr double d_neg_inf{ -std::numeric_limits<double>::infinity() };
    const     float  f_qnan{ std::numeric_limits<float>::quiet_NaN() };
    const     double d_qnan{ std::numeric_limits<double>::quiet_NaN() };

    float  f_neg_zero{ -0.0f };
    double d_neg_zero{ -0.0 };

    // Largest/smallest finite magnitudes and the smallest positive SUBNORMAL.
    // max()      = largest finite (sign clear).
    // lowest()   = most-negative finite (== -max(); sign bit set).
    // min()      = smallest POSITIVE NORMAL (exponent field == 1, mantissa 0).
    // denorm_min = smallest POSITIVE SUBNORMAL (exponent field 0, mantissa == 1)
    //              — a flush-to-zero FPU path would corrupt it, so a bit-exact
    //              round-trip proves the slot/movq-xmm0 epilogue copies, never
    //              arithmetises, the value.
    constexpr float  f_max{ std::numeric_limits<float>::max() };
    constexpr double d_max{ std::numeric_limits<double>::max() };
    constexpr float  f_lowest{ std::numeric_limits<float>::lowest() };
    constexpr double d_lowest{ std::numeric_limits<double>::lowest() };
    constexpr float  f_min_normal{ std::numeric_limits<float>::min() };
    constexpr double d_min_normal{ std::numeric_limits<double>::min() };
    const     float  f_subnormal{ std::numeric_limits<float>::denorm_min() };
    const     double d_subnormal{ std::numeric_limits<double>::denorm_min() };

    // NaNs carrying a SPECIFIC, non-canonical mantissa payload (distinct from
    // the library/std quiet-NaN used in the "qnan" round).  The exponent field
    // is all-ones and a hand-chosen payload sits in the mantissa; same_bits()
    // then demands the EXACT payload survive the movq xmm0 epilogue, proving the
    // slot is not "normalised" to a canonical NaN.  Bit 22 (float) / 51 (double)
    // is set so the value is a quiet NaN regardless of platform sNaN handling.
    const     float  f_nan_payload{ bits_to_float(0x7FAB1234u) };
    const     double d_nan_payload{ bits_to_double(0x7FF8ABCDEF012345ULL) };

    // ROUND 1 — canonical "obviously not the original" values, all distinct
    // from every orig* return.  The bedrock that the force-return path works
    // at all for each primitive, on both instance and static dispatch.
    run_and_check(ctx, "canonical", forced_values{
        /*b */ true,
        /*by*/ static_cast<std::int8_t>(99),
        /*s */ static_cast<std::int16_t>(31000),
        /*i */ 0x12345678,
        /*l */ static_cast<std::int64_t>(0x0123456789ABCDEFLL),
        /*f */ 3.5f,
        /*d */ 2.5,
        /*c */ static_cast<std::uint16_t>(0x263A) }); // ☺

    // ROUND 2 — forced bool FALSE over an original that also returns false is
    // weak; instead force bool=false here while the canonical round forced
    // true.  This proves set(false) takes the cancel path (forces the return)
    // rather than "doing nothing": the original origBool() returns false too,
    // but origByte..origChar prove the slot was delivered, and the static
    // bool=false path is asserted via obsStaticBool.  Also: minimum/zero
    // boundaries for every integral.
    run_and_check(ctx, "min_zero", forced_values{
        /*b */ false,
        /*by*/ static_cast<std::int8_t>(0),
        /*s */ static_cast<std::int16_t>(0),
        /*i */ 0,
        /*l */ static_cast<std::int64_t>(0),
        /*f */ 0.0f,
        /*d */ 0.0,
        /*c */ static_cast<std::uint16_t>(0) }); // U+0000

    // ROUND 3 — signed MINIMUMS.  This is the load-bearing sign-extension
    // angle (vmhook.hpp:1165-1169): static_cast<int64_t>(INT8_MIN/…/INT32_MIN)
    // must land 0xFFFFFFFF80000000-style bits so the interpreter's ireturn
    // pops the negative value, not the zero-extended positive.  Long takes the
    // memcpy path (sizeof==8), so INT64_MIN proves the full 8 bytes land.
    run_and_check(ctx, "signed_min", forced_values{
        /*b */ true,
        /*by*/ std::numeric_limits<std::int8_t>::min(),    // -128
        /*s */ std::numeric_limits<std::int16_t>::min(),   // -32768
        /*i */ std::numeric_limits<std::int32_t>::min(),   // INT_MIN
        /*l */ std::numeric_limits<std::int64_t>::min(),   // LONG_MIN
        /*f */ -1.0f,
        /*d */ -1.0,
        /*c */ static_cast<std::uint16_t>(0x0001) });

    // ROUND 4 — signed MAXIMUMS for every integral; char MAX (0xFFFF, unsigned)
    // proves jchar is NOT sign-extended (the upper 48 bits must stay zero).
    run_and_check(ctx, "signed_max", forced_values{
        /*b */ true,
        /*by*/ std::numeric_limits<std::int8_t>::max(),    // 127
        /*s */ std::numeric_limits<std::int16_t>::max(),   // 32767
        /*i */ std::numeric_limits<std::int32_t>::max(),   // INT_MAX
        /*l */ std::numeric_limits<std::int64_t>::max(),   // LONG_MAX
        /*f */ 1.0f,
        /*d */ 1.0,
        /*c */ static_cast<std::uint16_t>(0xFFFF) });      // jchar max

    // ROUND 5 — "minus one" everywhere: the classic sign-extension trap.  A
    // byte/short/int of -1 that is zero-extended would surface as +255/+65535/
    // +4294967295 on the Java side; this round fails loudly if the
    // sign-extension branch ever regresses.  char 0xFFFF is the unsigned twin.
    run_and_check(ctx, "minus_one", forced_values{
        /*b */ true,
        /*by*/ static_cast<std::int8_t>(-1),
        /*s */ static_cast<std::int16_t>(-1),
        /*i */ -1,
        /*l */ static_cast<std::int64_t>(-1),
        /*f */ -123.0f,
        /*d */ -456.0,
        /*c */ static_cast<std::uint16_t>(0xFFFF) });

    // ROUND 6 — high-bit-set sub-max values that are NOT min/max but exercise
    // the boundary between sign- and zero-extension precisely:
    //   byte  0x80 = -128, short 0x8000 = -32768, int 0x80000000 = INT_MIN,
    //   char  0x8000 (unsigned 32768).  These overlap min by design but use the
    //   raw hex form a user is most likely to type, catching off-by-one in the
    //   predicate.
    run_and_check(ctx, "high_bit", forced_values{
        /*b */ false,
        /*by*/ static_cast<std::int8_t>(0x80),
        /*s */ static_cast<std::int16_t>(0x8000),
        /*i */ static_cast<std::int32_t>(0x80000000),
        /*l */ static_cast<std::int64_t>(0x8000000000000000ULL),
        /*f */ 7.0f,
        /*d */ 7.0,
        /*c */ static_cast<std::uint16_t>(0x8000) });

    // ROUND 7 — IEEE-754 NEGATIVE ZERO.  -0.0 must round-trip bit-exactly:
    //   float  0x80000000, double 0x8000000000000000.  same_bits() distinguishes
    // it from +0.0, proving the memcpy path preserved the sign bit through the
    // movq xmm0 epilogue.  (Integrals carry small sentinels so their checks
    // stay meaningful.)
    run_and_check(ctx, "neg_zero", forced_values{
        /*b */ true,
        /*by*/ static_cast<std::int8_t>(7),
        /*s */ static_cast<std::int16_t>(7),
        /*i */ 7,
        /*l */ static_cast<std::int64_t>(7),
        /*f */ f_neg_zero,
        /*d */ d_neg_zero,
        /*c */ static_cast<std::uint16_t>(7) });

    // ROUND 8 — IEEE-754 POSITIVE INFINITY for float and double.
    run_and_check(ctx, "pos_inf", forced_values{
        /*b */ true,
        /*by*/ static_cast<std::int8_t>(8),
        /*s */ static_cast<std::int16_t>(8),
        /*i */ 8,
        /*l */ static_cast<std::int64_t>(8),
        /*f */ f_pos_inf,
        /*d */ d_pos_inf,
        /*c */ static_cast<std::uint16_t>(8) });

    // ROUND 9 — IEEE-754 NEGATIVE INFINITY for float and double.
    run_and_check(ctx, "neg_inf", forced_values{
        /*b */ true,
        /*by*/ static_cast<std::int8_t>(9),
        /*s */ static_cast<std::int16_t>(9),
        /*i */ 9,
        /*l */ static_cast<std::int64_t>(9),
        /*f */ f_neg_inf,
        /*d */ d_neg_inf,
        /*c */ static_cast<std::uint16_t>(9) });

    // ROUND 10 — IEEE-754 quiet NaN.  NaN must NOT be normalized or rendered
    // as 0; same_bits() requires the exact qNaN payload to survive.  This is
    // the angle most likely to break if anyone "cleans up" the retval value.
    run_and_check(ctx, "qnan", forced_values{
        /*b */ true,
        /*by*/ static_cast<std::int8_t>(10),
        /*s */ static_cast<std::int16_t>(10),
        /*i */ 10,
        /*l */ static_cast<std::int64_t>(10),
        /*f */ f_qnan,
        /*d */ d_qnan,
        /*c */ static_cast<std::uint16_t>(10) });

    // ROUND 11 — float/double precision witnesses: values that are NOT exactly
    // representable as the OTHER width, proving the slot carries the true
    // 32-bit / 64-bit pattern and the JVM reads the correct register width.
    //   float  Float.intBitsToFloat(0x3FC00000) == 1.5f (exact),
    //   double Math.PI (not representable in float).
    run_and_check(ctx, "precision", forced_values{
        /*b */ false,
        /*by*/ static_cast<std::int8_t>(11),
        /*s */ static_cast<std::int16_t>(11),
        /*i */ 11,
        /*l */ static_cast<std::int64_t>(11),
        /*f */ 0.1f,                       // 0.1f != (float)0.1d round-trip trap
        /*d */ 3.141592653589793,          // Math.PI
        /*c */ static_cast<std::uint16_t>(11) });

    // ROUND 12 — long boundary that spills past 32 bits: a value whose low 32
    // bits are zero but whose high bits are non-zero, proving the FULL 64-bit
    // long is delivered (not just the low dword).  char ASCII boundary 0x7F.
    run_and_check(ctx, "long_high_dword", forced_values{
        /*b */ true,
        /*by*/ static_cast<std::int8_t>(0x7F),  // byte max via hex
        /*s */ static_cast<std::int16_t>(0x7FFF),
        /*i */ 0x7FFFFFFF,
        /*l */ static_cast<std::int64_t>(0x7FFFFFFF00000000LL),
        /*f */ 12.0f,
        /*d */ 12.0,
        /*c */ static_cast<std::uint16_t>(0x007F) }); // DEL

    // ROUND 13 — char-specific coverage: a surrogate-range code unit (0xD83D)
    // and an astral-plane low surrogate are perfectly valid jchar values (a
    // jchar is just an unsigned 16-bit code unit, not a validated codepoint).
    // Forcing 0xD83D proves no surrogate filtering / no sign extension.
    run_and_check(ctx, "char_surrogate", forced_values{
        /*b */ false,
        /*by*/ static_cast<std::int8_t>(13),
        /*s */ static_cast<std::int16_t>(13),
        /*i */ 13,
        /*l */ static_cast<std::int64_t>(13),
        /*f */ 13.0f,
        /*d */ 13.0,
        /*c */ static_cast<std::uint16_t>(0xD83D) }); // high surrogate

    // ROUND 14 — signed-integral MIN+1: the off-by-one neighbour just ABOVE the
    // minimum.  A sign-extension predicate that is wrong only at exactly MIN
    // would still pass signed_min by luck; -127 / INT16_MIN+1 / INT32_MIN+1 /
    // INT64_MIN+1 are negative non-extreme values that MUST keep their sign
    // through static_cast<int64_t> (byte/short/int) and the memcpy path (long).
    run_and_check(ctx, "boundary_plus_one", forced_values{
        /*b */ true,
        /*by*/ static_cast<std::int8_t>(std::numeric_limits<std::int8_t>::min() + 1),    // -127
        /*s */ static_cast<std::int16_t>(std::numeric_limits<std::int16_t>::min() + 1),  // -32767
        /*i */ std::numeric_limits<std::int32_t>::min() + 1,                             // INT_MIN+1
        /*l */ std::numeric_limits<std::int64_t>::min() + 1,                             // LONG_MIN+1
        /*f */ 14.5f,
        /*d */ 14.25,
        /*c */ static_cast<std::uint16_t>(0x0002) });

    // ROUND 15 — signed-integral MAX-1: the off-by-one neighbour just BELOW the
    // maximum (positive, non-extreme).  char 0xFFFE is jchar-max minus one and
    // must keep the upper 48 bits CLEAR (no accidental sign extension next to
    // the top of the unsigned range).
    run_and_check(ctx, "boundary_minus_one", forced_values{
        /*b */ false,
        /*by*/ static_cast<std::int8_t>(std::numeric_limits<std::int8_t>::max() - 1),    // 126
        /*s */ static_cast<std::int16_t>(std::numeric_limits<std::int16_t>::max() - 1),  // 32766
        /*i */ std::numeric_limits<std::int32_t>::max() - 1,                             // INT_MAX-1
        /*l */ std::numeric_limits<std::int64_t>::max() - 1,                             // LONG_MAX-1
        /*f */ 15.5f,
        /*d */ 15.25,
        /*c */ static_cast<std::uint16_t>(0xFFFE) });

    // ROUND 16 — LONG low-dword saturated, high-dword zero (0x00000000FFFFFFFF).
    // This is the 32->64 boundary the OTHER way from long_high_dword: a 64-bit
    // long whose low 32 bits are all ones must NOT be sign-extended (long takes
    // the memcpy path, sizeof==8), so the high dword stays 0 — i.e. the Java
    // caller sees +4294967295L, never -1L.  byte/short/int carry their own
    // low-bits-set sentinels; char 0x0100 is the first 2-byte UTF-8 boundary.
    run_and_check(ctx, "long_low_dword_max", forced_values{
        /*b */ true,
        /*by*/ static_cast<std::int8_t>(16),
        /*s */ static_cast<std::int16_t>(16),
        /*i */ 16,
        /*l */ static_cast<std::int64_t>(0x00000000FFFFFFFFLL), // 4294967295
        /*f */ 16.5f,
        /*d */ 16.25,
        /*c */ static_cast<std::uint16_t>(0x0100) });

    // ROUND 17 — LONG exact 32->64 carry point (0x0000000100000000 == 2^32):
    // bit 32 set, every lower bit clear.  Proves the single bit that straddles
    // the two 32-bit halves is delivered, so a truncating "low dword only" path
    // would surface 0 here and fail loudly.  char 0x0200 keeps the char slot
    // distinct.
    run_and_check(ctx, "long_carry_2pow32", forced_values{
        /*b */ false,
        /*by*/ static_cast<std::int8_t>(17),
        /*s */ static_cast<std::int16_t>(17),
        /*i */ 17,
        /*l */ static_cast<std::int64_t>(0x0000000100000000LL), // 2^32
        /*f */ 17.5f,
        /*d */ 17.25,
        /*c */ static_cast<std::uint16_t>(0x0200) });

    // ROUND 18 — float/double TYPE MAX (largest finite magnitude), bit-exact.
    //   float  0x7F7FFFFF, double 0x7FEFFFFFFFFFFFFF.  Distinct from +Inf: the
    // exponent is all-ones-minus-one with a full mantissa, so a mishandled
    // overflow-to-Inf would change the bits and same_bits() would catch it.
    run_and_check(ctx, "float_double_type_max", forced_values{
        /*b */ true,
        /*by*/ static_cast<std::int8_t>(18),
        /*s */ static_cast<std::int16_t>(18),
        /*i */ 18,
        /*l */ static_cast<std::int64_t>(18),
        /*f */ f_max,
        /*d */ d_max,
        /*c */ static_cast<std::uint16_t>(0x4D41) }); // 'MA'

    // ROUND 19 — float/double smallest POSITIVE NORMAL (min()), bit-exact.
    //   float  0x00800000, double 0x0010000000000000.  Exponent field == 1,
    // mantissa 0 — the very edge above the subnormal range.
    run_and_check(ctx, "float_double_min_normal", forced_values{
        /*b */ false,
        /*by*/ static_cast<std::int8_t>(19),
        /*s */ static_cast<std::int16_t>(19),
        /*i */ 19,
        /*l */ static_cast<std::int64_t>(19),
        /*f */ f_min_normal,
        /*d */ d_min_normal,
        /*c */ static_cast<std::uint16_t>(0x4D49) }); // 'MI'

    // ROUND 20 — float/double LOWEST finite (== -max(); sign bit set),
    // bit-exact.  The most-negative representable finite value, distinct from
    // type_max only by the sign bit, proving the sign rides through the slot for
    // the maximum-magnitude operand.
    run_and_check(ctx, "float_double_lowest", forced_values{
        /*b */ true,
        /*by*/ static_cast<std::int8_t>(20),
        /*s */ static_cast<std::int16_t>(20),
        /*i */ 20,
        /*l */ static_cast<std::int64_t>(20),
        /*f */ f_lowest,
        /*d */ d_lowest,
        /*c */ static_cast<std::uint16_t>(0x4C4F) }); // 'LO'

    // ROUND 21 — float/double smallest POSITIVE SUBNORMAL (denorm_min()),
    // bit-exact.  float 0x00000001, double 0x0000000000000001: exponent field 0,
    // mantissa == 1.  An FPU path with flush-to-zero / denormals-are-zero would
    // turn this into +0.0; same_bits() proves the slot is copied verbatim and
    // never passed through an arithmetic unit.
    run_and_check(ctx, "float_double_subnormal", forced_values{
        /*b */ false,
        /*by*/ static_cast<std::int8_t>(21),
        /*s */ static_cast<std::int16_t>(21),
        /*i */ 21,
        /*l */ static_cast<std::int64_t>(21),
        /*f */ f_subnormal,
        /*d */ d_subnormal,
        /*c */ static_cast<std::uint16_t>(0x5355) }); // 'SU'

    // ROUND 22 — NaN with a SPECIFIC mantissa payload (not the canonical quiet
    // NaN of the qnan round).  float 0x7FAB1234, double 0x7FF8ABCDEF012345 must
    // arrive with their payload intact: same_bits() fails if the value were
    // canonicalised to 0x7FC00000 / 0x7FF8000000000000.  This is the strongest
    // guard that the slot delivers ARBITRARY bit patterns, not just "a NaN".
    run_and_check(ctx, "float_double_nan_payload", forced_values{
        /*b */ true,
        /*by*/ static_cast<std::int8_t>(22),
        /*s */ static_cast<std::int16_t>(22),
        /*i */ 22,
        /*l */ static_cast<std::int64_t>(22),
        /*f */ f_nan_payload,
        /*d */ d_nan_payload,
        /*c */ static_cast<std::uint16_t>(0x4E50) }); // 'NP'

    // ROUND 23 — positive sign-bit-neighbour integrals: MAX-with-low-bit-clear
    //   byte 0x7E, short 0x7FFE, int 0x7FFFFFFE, long 0x7FFFFFFFFFFFFFFE.  The
    // largest-even positive in each width, sitting just below the sign flip,
    // confirming the top positive region is delivered without tipping into the
    // negative (extension) branch.  char 0x7FFE is the BMP value just below the
    // surrogate-adjacent range.
    run_and_check(ctx, "int_sign_neighbors", forced_values{
        /*b */ false,
        /*by*/ static_cast<std::int8_t>(0x7E),
        /*s */ static_cast<std::int16_t>(0x7FFE),
        /*i */ 0x7FFFFFFE,
        /*l */ static_cast<std::int64_t>(0x7FFFFFFFFFFFFFFELL),
        /*f */ 23.5f,
        /*d */ 23.25,
        /*c */ static_cast<std::uint16_t>(0x7FFE) });

    // ROUND 24 — char LOW surrogate (0xDC00), the twin of the high surrogate
    // (0xD83D) covered earlier, plus the 2-byte/3-byte UTF-8 BMP boundaries on
    // the integral slots (short 0x07FF, int 0x0800).  A lone low surrogate is a
    // valid 16-bit jchar code unit; forcing it proves the char path applies no
    // surrogate validation and no sign extension across the full unsigned range.
    run_and_check(ctx, "char_low_surrogate", forced_values{
        /*b */ true,
        /*by*/ static_cast<std::int8_t>(24),
        /*s */ static_cast<std::int16_t>(0x07FF), // last 2-byte UTF-8 codepoint
        /*i */ 0x0800,                            // first 3-byte UTF-8 codepoint
        /*l */ static_cast<std::int64_t>(24),
        /*f */ 24.5f,
        /*d */ 24.25,
        /*c */ static_cast<std::uint16_t>(0xDC00) }); // lone low surrogate

    // ROUND 25 — re-run the canonical vector a SECOND time at the very end to
    // prove the force-return path is stable across repeated arm/disarm cycles
    // (each round installs fresh scoped_hooks; this guards against state left
    // behind by a previous round's teardown).
    run_and_check(ctx, "canonical_repeat", forced_values{
        /*b */ true,
        /*by*/ static_cast<std::int8_t>(99),
        /*s */ static_cast<std::int16_t>(31000),
        /*i */ 0x12345678,
        /*l */ static_cast<std::int64_t>(0x0123456789ABCDEFLL),
        /*f */ 3.5f,
        /*d */ 2.5,
        /*c */ static_cast<std::uint16_t>(0x263A) });

    // ---- Lifecycle sanity: 25 rounds ran, so roundCount must be 25. -------
    ctx.check("ran_25_rounds", rsp_fixture::round_count() == 25);

    // ---- Control angle: with NO hooks installed, the original values flow
    // through unchanged (proves the force-return is what changed the result,
    // not some ambient effect).  We run the probe once more with zero hooks;
    // every observed field must equal the ORIGINAL orig* return value.
    {
        reset_round_counters();
        rsp_fixture::set_done(false);
        const bool done{ ctx.run_probe(
            [](bool value) { rsp_fixture::set_go(value); },
            []() { return rsp_fixture::get_done(); }) };
        ctx.check("baseline_probe_completed", done);
        if (done)
        {
            ctx.check("baseline_no_hook_fired", g_inst_fires.load() == 0 && g_stat_fires.load() == 0);
            // Instance originals.
            ctx.check("baseline_inst_bool_false",  rsp_fixture::obs_bool()  == false);
            ctx.check("baseline_inst_byte_11",     rsp_fixture::obs_byte()  == static_cast<std::int8_t>(11));
            ctx.check("baseline_inst_short_111",   rsp_fixture::obs_short() == static_cast<std::int16_t>(111));
            ctx.check("baseline_inst_int_1111",    rsp_fixture::obs_int()   == 1111);
            ctx.check("baseline_inst_long_1111",   rsp_fixture::obs_long()  == static_cast<std::int64_t>(1111));
            ctx.check("baseline_inst_float_11_5",  same_bits(rsp_fixture::obs_float(),  11.5f));
            ctx.check("baseline_inst_double_11_25",same_bits(rsp_fixture::obs_double(), 11.25));
            ctx.check("baseline_inst_char_A",      rsp_fixture::obs_char()  == static_cast<std::uint16_t>('A'));
            // Static originals.
            ctx.check("baseline_stat_byte_22",     rsp_fixture::obs_s_byte()  == static_cast<std::int8_t>(22));
            ctx.check("baseline_stat_int_2222",    rsp_fixture::obs_s_int()   == 2222);
            ctx.check("baseline_stat_long_2222",   rsp_fixture::obs_s_long()  == static_cast<std::int64_t>(2222));
            ctx.check("baseline_stat_double_22_25",same_bits(rsp_fixture::obs_s_double(), 22.25));
            ctx.check("baseline_stat_char_B",      rsp_fixture::obs_s_char()  == static_cast<std::uint16_t>('B'));
        }
    }

    ctx.record("[INFO] return_set_primitives: forced bool/byte/short/int/long/float/double/char "
               "on instance+static dispatch across 25 value rounds (canonical, min/zero, signed "
               "min/max, minus-one, high-bit, -0.0, +Inf, -Inf, qNaN, precision, long-high-dword, "
               "surrogate char, MIN+1, MAX-1, long-low-dword-max, long-2^32-carry, float/double "
               "type-max, min-normal, lowest, subnormal, custom-NaN-payload, int-sign-neighbors, "
               "low-surrogate char, canonical-repeat) plus a no-hook baseline.");

    // [INFO] Boolean force-return is intentionally bounded to {true,false}: the
    // return_value::set(value) API takes its argument BY TYPE, so a boolean slot
    // can only be driven through a C++ `bool`, whose object representation is
    // {0,1}.  A "raw non-canonical byte" (e.g. 0x02) cannot be expressed through
    // this typed path WITHOUT constructing a `bool` with an invalid value, which
    // is undefined behaviour in C++ — so it is deliberately NOT exercised here.
    // The byte-width force-return is covered exhaustively by the origByte rounds
    // (0, 1, -1, 0x80, MIN/MAX, +/-1 neighbours) instead; the {true,false} edges
    // of the boolean path are both asserted (canonical=true, min_zero=false,
    // high_bit=false) on instance AND static dispatch.
    ctx.record("[INFO] return_set_primitives: bool force-return is type-bounded to {true,false} by "
               "set(value); a raw non-canonical boolean byte is not expressible without UB, so the "
               "raw-byte boolean case is documented rather than tested (byte-width raw patterns are "
               "covered by the origByte rounds).");
}
