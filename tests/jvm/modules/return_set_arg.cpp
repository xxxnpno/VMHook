// return_set_arg JVM test module — exhaustively exercises
// vmhook::return_value::set_arg(index, value) for the SAFE PRIMITIVE cases on a
// LIVE JVM.
//
// Feature under test: vmhook/ext/vmhook/vmhook.hpp (return_value::set_arg).
//   set_arg(index, value) mutates a Java method ARGUMENT in place on the
//   interpreter local-variable array from inside a hook detour, BEFORE the
//   original method body runs, so the original body observes the replacement.
//   This module covers the primitive branch only (vmhook.hpp ~9763-9782):
//     void* raw{}; std::memcpy(&raw, &value, sizeof(clean_value_type));
//     // one-word primitive -> write_slot(index);
//     // two-word (long/double, 8 bytes) -> write_slot(index + 1)  [lower slot]
//   plus the bounds guard (index<0 || index>0xFFFF rejected; the fault-safe
//   write_slot makes any out-of-range index a safe NO-OP, never a wild write).
//
// ── WHY PRIMITIVES ONLY (crash-safety contract) ───────────────────────────────
// The object/String set_arg branches previously crashed the no-SEH suite
// (narrow-vs-wide oop storage; see return_set_wrapper_null.cpp, which owns those
// cases crash-proof via native gated reads + cancel()).  This module is the
// primitive-only twin and is crash-proof by construction:
//   * every hooked method takes ONLY primitive args (no object/String slot),
//   * the detours allocate NOTHING in the JVM (no NewString, no make_unique, no
//     large Java array/object) and never force GC,
//   * scoped_hook RAII only (never shutdown_hooks): each round's handles live in
//     a nested block and disarm when the block ends, so the next round installs
//     clean and no hook is left armed for a later module,
//   * out-of-range set_arg indices are REJECTED (return false) and the body is
//     allowed through to PROVE the original value survived — no wild write.
//
// ── SLOT MODEL (HotSpot x64 interpreter; long/double = 2 slots) ───────────────
//   instance method: slot 0 = this, slot 1 = first arg, ...
//   static  method: slot 0 = first arg, ...
//   A long/double arg's BASE slot is what we pass to set_arg; set_arg internally
//   stores the 64-bit value at the LOWER slot (index+1), matching the
//   interpreter's LOCALS_LONG/LOCALS_DOUBLE convention and the library's own read
//   path (extract_frame_arg).  So set_arg(base, longvalue) round-trips with both
//   the read path and the body's lload/dload.
//
// ── SIGN-EXTENSION CHARACTERIZATION (byte/short) ──────────────────────────────
// The primitive branch zero-fills then memcpys the low N bytes; it does NOT
// sign-extend.  So set_arg<int8_t>(slot, -1) writes 0x00000000000000FF, and the
// body's iload (which reads 32 bits of the slot) observes +255, not -1.  Java's
// own calling convention would sign-extend.  This is a documented [MEDIUM] flaw.
// To stay CI-green across the current (unfixed) behaviour while still catching a
// WILD/garbage read, the byte/short -1 checks HARD-assert the observed value is
// in {-1, 255} / {-1, 65535} and record an [INFO] naming which side fired; the
// day a sign-extension fix lands, the value becomes -1 and the assertion still
// passes (the [INFO] flips).
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <atomic>
#include <climits>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>

namespace
{
    // Wrapper for vmhook.fixtures.ReturnSetArg.
    class rsa_fixture : public vmhook::object<rsa_fixture>
    {
    public:
        explicit rsa_fixture(vmhook::oop_t instance) noexcept
            : vmhook::object<rsa_fixture>{ instance }
        {
        }

        // ── go/done/mode handshake ─────────────────────────────────────────
        static auto set_go(bool value)       -> void { static_field("go")->set(value); }
        static auto get_done()               -> bool { bool v = static_field("done")->get(); return v; }
        static auto set_done(bool value)     -> void { static_field("done")->set(value); }
        static auto set_mode(std::int32_t m) -> void { static_field("mode")->set(m); }
        static auto probe_ticks()            -> std::int32_t { std::int32_t v = static_field("probeTicks")->get(); return v; }
        static auto saw_exception()          -> bool { bool v = static_field("sawException")->get(); return v; }

        // ── single-arg primitive observations ──────────────────────────────
        static auto obs_int()    -> std::int32_t  { std::int32_t  v = static_field("obsInt")->get();    return v; }
        static auto obs_long()   -> std::int64_t  { std::int64_t  v = static_field("obsLong")->get();   return v; }
        static auto obs_double() -> double         { double        v = static_field("obsDouble")->get(); return v; }
        static auto obs_float()  -> float          { float         v = static_field("obsFloat")->get();  return v; }
        static auto obs_bool()   -> bool           { bool          v = static_field("obsBool")->get();   return v; }
        static auto obs_byte()   -> std::int8_t    { std::int8_t   v = static_field("obsByte")->get();   return v; }
        static auto obs_char()   -> std::uint16_t  { std::uint16_t v = static_field("obsChar")->get();   return v; }
        static auto obs_short()  -> std::int16_t   { std::int16_t  v = static_field("obsShort")->get();  return v; }
        static auto obs_byte_wide()  -> std::int32_t { std::int32_t v = static_field("obsByteWide")->get();  return v; }
        static auto obs_short_wide() -> std::int32_t { std::int32_t v = static_field("obsShortWide")->get(); return v; }
        static auto obs_static_int() -> std::int32_t { std::int32_t v = static_field("obsStaticInt")->get(); return v; }

        // ── slot-model observations ────────────────────────────────────────
        static auto two_a()      -> std::int32_t  { std::int32_t v = static_field("twoA")->get();     return v; }
        static auto two_b()      -> std::int32_t  { std::int32_t v = static_field("twoB")->get();     return v; }
        static auto mix_long()   -> std::int64_t  { std::int64_t v = static_field("mixLong")->get();  return v; }
        static auto mix_int()    -> std::int32_t  { std::int32_t v = static_field("mixInt")->get();   return v; }
        static auto il_int()     -> std::int32_t  { std::int32_t v = static_field("ilInt")->get();    return v; }
        static auto il_long()    -> std::int64_t  { std::int64_t v = static_field("ilLong")->get();   return v; }
        static auto di_double()  -> double         { double       v = static_field("diDouble")->get(); return v; }
        static auto di_int()     -> std::int32_t  { std::int32_t v = static_field("diInt")->get();    return v; }

        // ── bounds observation ─────────────────────────────────────────────
        static auto bounds_obs() -> std::int32_t  { std::int32_t v = static_field("boundsObs")->get(); return v; }
    };

    // Mode selectors (mirror ReturnSetArg.java).
    constexpr std::int32_t MODE_PRIMITIVES{ 0 };
    constexpr std::int32_t MODE_SLOTS{ 1 };
    constexpr std::int32_t MODE_BOUNDS{ 2 };

    // The original value every probe passes (so a no-hook baseline observes it).
    constexpr std::int32_t ORIGINAL_INT{ 7 };

    // ── per-round set_arg(...) return values, captured in the detours ─────────
    std::atomic<bool> g_int_set_ok{ false };
    std::atomic<bool> g_long_set_ok{ false };
    std::atomic<bool> g_double_set_ok{ false };
    std::atomic<bool> g_float_set_ok{ false };
    std::atomic<bool> g_bool_set_ok{ false };
    std::atomic<bool> g_byte_set_ok{ false };
    std::atomic<bool> g_char_set_ok{ false };
    std::atomic<bool> g_short_set_ok{ false };
    std::atomic<bool> g_sint_set_ok{ false };
    std::atomic<bool> g_int_self_ok{ false };

    std::atomic<bool> g_two_a_ok{ false };
    std::atomic<bool> g_two_b_ok{ false };
    std::atomic<bool> g_mix_long_ok{ false };
    std::atomic<bool> g_mix_int_ok{ false };
    std::atomic<bool> g_il_int_ok{ false };
    std::atomic<bool> g_il_long_ok{ false };
    std::atomic<bool> g_di_double_ok{ false };
    std::atomic<bool> g_di_int_ok{ false };

    // Bounds: every out-of-range set_arg must return false; the in-range write
    // (recorded last) must return true.
    std::atomic<bool> g_bounds_neg1_rejected{ false };
    std::atomic<bool> g_bounds_intmin_rejected{ false };
    std::atomic<bool> g_bounds_65536_rejected{ false };
    std::atomic<bool> g_bounds_0x10000_rejected{ false };
    std::atomic<bool> g_bounds_intmax_rejected{ false };

    auto reset_round() -> void
    {
        g_int_set_ok.store(false);   g_long_set_ok.store(false);
        g_double_set_ok.store(false); g_float_set_ok.store(false);
        g_bool_set_ok.store(false);  g_byte_set_ok.store(false);
        g_char_set_ok.store(false);  g_short_set_ok.store(false);
        g_sint_set_ok.store(false);  g_int_self_ok.store(false);
    }

    // Bit-exact float/double comparison (so NaN==NaN and -0.0 != +0.0 are
    // distinguished): the body must observe the EXACT bit pattern we injected.
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

    // One value vector injected over the (fixed) original args in a primitive round.
    struct prim_values
    {
        std::int32_t  i;
        std::int64_t  l;
        double        d;
        float         f;
        bool          b;
        std::int8_t   by;
        std::uint16_t c;  // jchar is unsigned 16-bit.
        std::int16_t  s;
    };

    // Arm all 9 primitive hooks for one value vector, run ONE probe, leave the
    // observed fields populated.  All handles are local -> disarm on return.
    auto run_prim_round(vmhook_test::context& ctx,
                        const std::string&    tag,
                        const prim_values&    v) -> bool
    {
        reset_round();
        rsa_fixture::set_done(false);

        // INSTANCE single-arg primitives — arg lives in slot 1 (this=slot0).
        auto h_int{ vmhook::scoped_hook<rsa_fixture>("takeInt", "(I)V",
            [v](vmhook::return_value& r, const std::unique_ptr<rsa_fixture>& self, std::int32_t)
            {
                g_int_self_ok.store(self != nullptr, std::memory_order_relaxed);
                g_int_set_ok.store(r.set_arg(1, v.i), std::memory_order_relaxed);
            }) };
        auto h_long{ vmhook::scoped_hook<rsa_fixture>("takeLong", "(J)V",
            [v](vmhook::return_value& r, const std::unique_ptr<rsa_fixture>&, std::int64_t)
            { g_long_set_ok.store(r.set_arg(1, v.l), std::memory_order_relaxed); }) };
        auto h_double{ vmhook::scoped_hook<rsa_fixture>("takeDouble", "(D)V",
            [v](vmhook::return_value& r, const std::unique_ptr<rsa_fixture>&, double)
            { g_double_set_ok.store(r.set_arg(1, v.d), std::memory_order_relaxed); }) };
        auto h_float{ vmhook::scoped_hook<rsa_fixture>("takeFloat", "(F)V",
            [v](vmhook::return_value& r, const std::unique_ptr<rsa_fixture>&, float)
            { g_float_set_ok.store(r.set_arg(1, v.f), std::memory_order_relaxed); }) };
        auto h_bool{ vmhook::scoped_hook<rsa_fixture>("takeBoolean", "(Z)V",
            [v](vmhook::return_value& r, const std::unique_ptr<rsa_fixture>&, std::int32_t)
            { g_bool_set_ok.store(r.set_arg(1, v.b), std::memory_order_relaxed); }) };
        auto h_byte{ vmhook::scoped_hook<rsa_fixture>("takeByte", "(B)V",
            [v](vmhook::return_value& r, const std::unique_ptr<rsa_fixture>&, std::int32_t)
            { g_byte_set_ok.store(r.set_arg(1, v.by), std::memory_order_relaxed); }) };
        auto h_char{ vmhook::scoped_hook<rsa_fixture>("takeChar", "(C)V",
            [v](vmhook::return_value& r, const std::unique_ptr<rsa_fixture>&, std::int32_t)
            { g_char_set_ok.store(r.set_arg(1, static_cast<char16_t>(v.c)), std::memory_order_relaxed); }) };
        auto h_short{ vmhook::scoped_hook<rsa_fixture>("takeShort", "(S)V",
            [v](vmhook::return_value& r, const std::unique_ptr<rsa_fixture>&, std::int32_t)
            { g_short_set_ok.store(r.set_arg(1, v.s), std::memory_order_relaxed); }) };

        // STATIC int — arg lives in slot 0 (no 'this').
        auto h_sint{ vmhook::scoped_hook<rsa_fixture>("takeStaticInt", "(I)V",
            [v](vmhook::return_value& r, std::int32_t)
            { g_sint_set_ok.store(r.set_arg(0, v.i), std::memory_order_relaxed); }) };

        const bool all_installed{
            h_int.installed()   && h_long.installed()  && h_double.installed() &&
            h_float.installed() && h_bool.installed()  && h_byte.installed()   &&
            h_char.installed()  && h_short.installed() && h_sint.installed() };
        ctx.check(tag + "_all_hooks_installed", all_installed);

        const bool done{ ctx.run_probe(
            [](bool value)
            {
                if (value) { rsa_fixture::set_done(false); rsa_fixture::set_mode(MODE_PRIMITIVES); }
                rsa_fixture::set_go(value);
            },
            []() { return rsa_fixture::get_done(); }) };
        ctx.check(tag + "_probe_completed", done);
        return done && all_installed;
    }

    // Assert every observed primitive == the injected value (HARD), except
    // byte/short whose checks are sign-extension-aware (see header).
    auto check_prim_round(vmhook_test::context& ctx,
                          const std::string&    tag,
                          const prim_values&    v) -> void
    {
        ctx.check(tag + "_no_java_exception", !rsa_fixture::saw_exception());
        ctx.check(tag + "_int_self_seen",     g_int_self_ok.load(std::memory_order_relaxed));

        // set_arg returned true on every valid path.
        ctx.check(tag + "_int_set_ok",    g_int_set_ok.load(std::memory_order_relaxed));
        ctx.check(tag + "_long_set_ok",   g_long_set_ok.load(std::memory_order_relaxed));
        ctx.check(tag + "_double_set_ok", g_double_set_ok.load(std::memory_order_relaxed));
        ctx.check(tag + "_float_set_ok",  g_float_set_ok.load(std::memory_order_relaxed));
        ctx.check(tag + "_bool_set_ok",   g_bool_set_ok.load(std::memory_order_relaxed));
        ctx.check(tag + "_byte_set_ok",   g_byte_set_ok.load(std::memory_order_relaxed));
        ctx.check(tag + "_char_set_ok",   g_char_set_ok.load(std::memory_order_relaxed));
        ctx.check(tag + "_short_set_ok",  g_short_set_ok.load(std::memory_order_relaxed));
        ctx.check(tag + "_sint_set_ok",   g_sint_set_ok.load(std::memory_order_relaxed));

        // Body observed the injected value.
        ctx.check(tag + "_int_observed",    rsa_fixture::obs_int()  == v.i);
        ctx.check(tag + "_long_observed",   rsa_fixture::obs_long() == v.l);
        ctx.check(tag + "_double_observed", same_bits(rsa_fixture::obs_double(), v.d));
        ctx.check(tag + "_float_observed",  same_bits(rsa_fixture::obs_float(),  v.f));
        ctx.check(tag + "_bool_observed",   rsa_fixture::obs_bool() == v.b);
        ctx.check(tag + "_char_observed",   rsa_fixture::obs_char() == v.c);
        ctx.check(tag + "_sint_observed",   rsa_fixture::obs_static_int() == v.i);

        // ── byte ──
        // The byte field (obsByte) truncates to 8 bits, so it ALWAYS matches the
        // injected low byte regardless of sign-extension — this is the deterministic
        // HARD assert that catches a wild/garbage read.  The widened (int) view
        // (obsByteWide) is where a missing sign-extension would surface; its result
        // is JDK-implementation-sensitive, so it is asserted only to the tolerant set
        // {correct sign-extended, current zero-extended low byte} (HARD: any other
        // value is a genuine wild read) and an [INFO] records which side fired.
        {
            const std::int8_t  obs_field{ rsa_fixture::obs_byte() };
            ctx.check(tag + "_byte_field_matches_low_byte", obs_field == v.by);

            const std::int32_t wide{ rsa_fixture::obs_byte_wide() };
            const std::int32_t want_signed{ static_cast<std::int32_t>(v.by) };
            const std::int32_t want_zeroext{ static_cast<std::int32_t>(static_cast<std::uint8_t>(v.by)) };
            ctx.check(tag + "_byte_wide_in_tolerant_set",
                      wide == want_signed || wide == want_zeroext);
            if (want_signed < 0 && wide == want_zeroext && want_zeroext != want_signed)
            {
                ctx.record("[INFO] return_set_arg " + tag + ": byte set_arg does NOT sign-extend - "
                           "injected " + std::to_string(want_signed) + ", body's widened view is "
                           + std::to_string(want_zeroext) + " (documented [MEDIUM] flaw; if a fix "
                           "lands the widened view becomes " + std::to_string(want_signed) + ").");
            }
        }

        // ── short ── (same shape as byte)
        {
            const std::int16_t obs_field{ rsa_fixture::obs_short() };
            ctx.check(tag + "_short_field_matches_low_16", obs_field == v.s);

            const std::int32_t wide{ rsa_fixture::obs_short_wide() };
            const std::int32_t want_signed{ static_cast<std::int32_t>(v.s) };
            const std::int32_t want_zeroext{ static_cast<std::int32_t>(static_cast<std::uint16_t>(v.s)) };
            ctx.check(tag + "_short_wide_in_tolerant_set",
                      wide == want_signed || wide == want_zeroext);
            if (want_signed < 0 && wide == want_zeroext && want_zeroext != want_signed)
            {
                ctx.record("[INFO] return_set_arg " + tag + ": short set_arg does NOT sign-extend - "
                           "injected " + std::to_string(want_signed) + ", body's widened view is "
                           + std::to_string(want_zeroext) + " (documented [MEDIUM] flaw; if a fix "
                           "lands the widened view becomes " + std::to_string(want_signed) + ").");
            }
        }
    }

    auto run_and_check_prim(vmhook_test::context& ctx,
                            const std::string&    tag,
                            const prim_values&    v) -> void
    {
        if (run_prim_round(ctx, tag, v))
        {
            check_prim_round(ctx, tag, v);
        }
    }
}

VMHOOK_JVM_MODULE(return_set_arg)
{
    vmhook::register_class<rsa_fixture>("vmhook/fixtures/ReturnSetArg");

    // IEEE-754 specials forged bit-exactly.
    const float  f_qnan{ std::numeric_limits<float>::quiet_NaN() };
    const double d_qnan{ std::numeric_limits<double>::quiet_NaN() };
    const float  f_pos_inf{  std::numeric_limits<float>::infinity() };
    const float  f_neg_inf{ -std::numeric_limits<float>::infinity() };
    const double d_pos_inf{  std::numeric_limits<double>::infinity() };
    const double d_neg_inf{ -std::numeric_limits<double>::infinity() };
    const float  f_neg_zero{ -0.0f };
    const double d_neg_zero{ -0.0 };

    // =====================================================================
    // PART 1 — single-arg primitive injection, every type, many value rounds.
    // =====================================================================

    // ROUND: canonical — obviously-not-the-original distinct values per type.
    run_and_check_prim(ctx, "canonical", prim_values{
        /*i */ 42,
        /*l */ static_cast<std::int64_t>(0x0123456789ABCDEFLL),
        /*d */ 12.5,
        /*f */ 3.5f,
        /*b */ true,
        /*by*/ static_cast<std::int8_t>(99),
        /*c */ static_cast<std::uint16_t>(0x2764), // ❤
        /*s */ static_cast<std::int16_t>(31000) });

    // ROUND: zeros / false / NUL char.
    run_and_check_prim(ctx, "zero", prim_values{
        /*i */ 0,
        /*l */ static_cast<std::int64_t>(0),
        /*d */ 0.0,
        /*f */ 0.0f,
        /*b */ false,
        /*by*/ static_cast<std::int8_t>(0),
        /*c */ static_cast<std::uint16_t>(0x0000), // U+0000
        /*s */ static_cast<std::int16_t>(0) });

    // ROUND: signed minimums (the sign-extension trap edge).
    run_and_check_prim(ctx, "signed_min", prim_values{
        /*i */ std::numeric_limits<std::int32_t>::min(),   // INT_MIN
        /*l */ std::numeric_limits<std::int64_t>::min(),   // LONG_MIN
        /*d */ -1.0,
        /*f */ -1.0f,
        /*b */ true,
        /*by*/ std::numeric_limits<std::int8_t>::min(),    // -128
        /*c */ static_cast<std::uint16_t>(0x0001),
        /*s */ std::numeric_limits<std::int16_t>::min() }); // -32768

    // ROUND: signed maximums; char MAX (0xFFFF) proves jchar zero-extends.
    run_and_check_prim(ctx, "signed_max", prim_values{
        /*i */ std::numeric_limits<std::int32_t>::max(),   // INT_MAX
        /*l */ std::numeric_limits<std::int64_t>::max(),   // LONG_MAX
        /*d */ 1.0,
        /*f */ 1.0f,
        /*b */ true,
        /*by*/ std::numeric_limits<std::int8_t>::max(),    // 127
        /*c */ static_cast<std::uint16_t>(0xFFFF),         // jchar max
        /*s */ std::numeric_limits<std::int16_t>::max() }); // 32767

    // ROUND: minus-one everywhere (the classic sign-extension trap).  char 0xFFFF
    // is the unsigned twin (must zero-extend, not sign-extend).
    run_and_check_prim(ctx, "minus_one", prim_values{
        /*i */ -1,
        /*l */ static_cast<std::int64_t>(-1),
        /*d */ -123.0,
        /*f */ -456.0f,
        /*b */ true,
        /*by*/ static_cast<std::int8_t>(-1),
        /*c */ static_cast<std::uint16_t>(0xFFFF),
        /*s */ static_cast<std::int16_t>(-1) });

    // ROUND: IEEE-754 negative zero (float+double) — must round-trip bit-exactly.
    run_and_check_prim(ctx, "neg_zero", prim_values{
        /*i */ 7,
        /*l */ static_cast<std::int64_t>(7),
        /*d */ d_neg_zero,
        /*f */ f_neg_zero,
        /*b */ false,
        /*by*/ static_cast<std::int8_t>(7),
        /*c */ static_cast<std::uint16_t>(7),
        /*s */ static_cast<std::int16_t>(7) });

    // ROUND: +Inf.
    run_and_check_prim(ctx, "pos_inf", prim_values{
        /*i */ 8, /*l */ static_cast<std::int64_t>(8),
        /*d */ d_pos_inf, /*f */ f_pos_inf,
        /*b */ true, /*by*/ static_cast<std::int8_t>(8),
        /*c */ static_cast<std::uint16_t>(8), /*s */ static_cast<std::int16_t>(8) });

    // ROUND: -Inf.
    run_and_check_prim(ctx, "neg_inf", prim_values{
        /*i */ 9, /*l */ static_cast<std::int64_t>(9),
        /*d */ d_neg_inf, /*f */ f_neg_inf,
        /*b */ false, /*by*/ static_cast<std::int8_t>(9),
        /*c */ static_cast<std::uint16_t>(9), /*s */ static_cast<std::int16_t>(9) });

    // ROUND: quiet NaN (must not be canonicalised to 0).
    run_and_check_prim(ctx, "qnan", prim_values{
        /*i */ 10, /*l */ static_cast<std::int64_t>(10),
        /*d */ d_qnan, /*f */ f_qnan,
        /*b */ true, /*by*/ static_cast<std::int8_t>(10),
        /*c */ static_cast<std::uint16_t>(10), /*s */ static_cast<std::int16_t>(10) });

    // ROUND: long high-dword non-zero (proves the FULL 64 bits land, not just the
    // low dword); char 0x8000 (high-bit-set unsigned), short/byte hi-bit hex.
    run_and_check_prim(ctx, "long_high_dword", prim_values{
        /*i */ static_cast<std::int32_t>(0x80000000),
        /*l */ static_cast<std::int64_t>(0x7FFFFFFF00000000LL),
        /*d */ 3.141592653589793,
        /*f */ 0.1f,
        /*b */ true,
        /*by*/ static_cast<std::int8_t>(0x80),             // -128 via hex
        /*c */ static_cast<std::uint16_t>(0x8000),         // 32768 (zero-extend)
        /*s */ static_cast<std::int16_t>(0x8000) });       // -32768 via hex

    // ROUND: long low-dword saturated, high-dword zero (must NOT sign-extend; long
    // takes the memcpy path so the Java caller sees +4294967295L, never -1L).
    run_and_check_prim(ctx, "long_low_dword_max", prim_values{
        /*i */ 0x7FFFFFFF,
        /*l */ static_cast<std::int64_t>(0x00000000FFFFFFFFLL), // 4294967295
        /*d */ 2.25,
        /*f */ 2.5f,
        /*b */ false,
        /*by*/ static_cast<std::int8_t>(0x7F),             // 127 via hex
        /*c */ static_cast<std::uint16_t>(0x7FFF),
        /*s */ static_cast<std::int16_t>(0x7FFF) });       // 32767 via hex

    // ROUND: char surrogate code unit (valid 16-bit jchar; no surrogate filtering).
    run_and_check_prim(ctx, "char_surrogate", prim_values{
        /*i */ 13, /*l */ static_cast<std::int64_t>(13),
        /*d */ 13.25, /*f */ 13.5f,
        /*b */ true, /*by*/ static_cast<std::int8_t>(13),
        /*c */ static_cast<std::uint16_t>(0xD83D),         // high surrogate
        /*s */ static_cast<std::int16_t>(13) });

    // ROUND: re-run canonical at the end (stable across arm/disarm cycles).
    run_and_check_prim(ctx, "canonical_repeat", prim_values{
        /*i */ 42,
        /*l */ static_cast<std::int64_t>(0x0123456789ABCDEFLL),
        /*d */ 12.5,
        /*f */ 3.5f,
        /*b */ true,
        /*by*/ static_cast<std::int8_t>(99),
        /*c */ static_cast<std::uint16_t>(0x2764),
        /*s */ static_cast<std::int16_t>(31000) });

    // =====================================================================
    // PART 2 — SLOT MODEL: wide args interleaved with narrow args.  Each value is
    // distinct so a mis-targeted slot surfaces as the wrong field changing.
    // =====================================================================
    {
        reset_round();
        rsa_fixture::set_done(false);

        // twoInts(int a, int b): a=slot1 -> 50, b=slot2 -> 60.
        auto h_two{ vmhook::scoped_hook<rsa_fixture>("twoInts", "(II)V",
            [](vmhook::return_value& r, const std::unique_ptr<rsa_fixture>&, std::int32_t, std::int32_t)
            {
                g_two_a_ok.store(r.set_arg(1, static_cast<std::int32_t>(50)), std::memory_order_relaxed);
                g_two_b_ok.store(r.set_arg(2, static_cast<std::int32_t>(60)), std::memory_order_relaxed);
            }) };

        // mixLongInt(long a, int b): a base slot 1 (slots 1+2) -> a long, b=slot3 -> int.
        // The int AFTER the long proves a wide arg reserves two slots; targeting
        // slot 3 (not 2) is required to mutate b.
        auto h_mix{ vmhook::scoped_hook<rsa_fixture>("mixLongInt", "(JI)V",
            [](vmhook::return_value& r, const std::unique_ptr<rsa_fixture>&, std::int64_t, std::int32_t)
            {
                g_mix_long_ok.store(r.set_arg(1, static_cast<std::int64_t>(0x1122334455667788LL)),
                                    std::memory_order_relaxed);
                g_mix_int_ok.store(r.set_arg(3, static_cast<std::int32_t>(99)), std::memory_order_relaxed);
            }) };

        // intLong(int a, long b): a=slot1 -> int, b base slot 2 (slots 2+3) -> long.
        auto h_il{ vmhook::scoped_hook<rsa_fixture>("intLong", "(IJ)V",
            [](vmhook::return_value& r, const std::unique_ptr<rsa_fixture>&, std::int32_t, std::int64_t)
            {
                g_il_int_ok.store(r.set_arg(1, static_cast<std::int32_t>(77)), std::memory_order_relaxed);
                g_il_long_ok.store(r.set_arg(2, static_cast<std::int64_t>(0x00000000DEADBEEFLL)),
                                   std::memory_order_relaxed);
            }) };

        // doubleInt(double a, int b): a base slot 1 (slots 1+2) -> double, b=slot3 -> int.
        auto h_di{ vmhook::scoped_hook<rsa_fixture>("doubleInt", "(DI)V",
            [](vmhook::return_value& r, const std::unique_ptr<rsa_fixture>&, double, std::int32_t)
            {
                g_di_double_ok.store(r.set_arg(1, 98.5), std::memory_order_relaxed);
                g_di_int_ok.store(r.set_arg(3, static_cast<std::int32_t>(33)), std::memory_order_relaxed);
            }) };

        const bool all_installed{
            h_two.installed() && h_mix.installed() && h_il.installed() && h_di.installed() };
        ctx.check("slots_all_hooks_installed", all_installed);

        const bool done{ ctx.run_probe(
            [](bool value)
            {
                if (value) { rsa_fixture::set_done(false); rsa_fixture::set_mode(MODE_SLOTS); }
                rsa_fixture::set_go(value);
            },
            []() { return rsa_fixture::get_done(); }) };
        ctx.check("slots_probe_completed", done);

        if (done && all_installed)
        {
            ctx.check("slots_no_java_exception", !rsa_fixture::saw_exception());

            // twoInts — both slots independently mutated.
            ctx.check("slots_two_a_set_ok", g_two_a_ok.load(std::memory_order_relaxed));
            ctx.check("slots_two_b_set_ok", g_two_b_ok.load(std::memory_order_relaxed));
            ctx.check("slots_two_a_observed_50", rsa_fixture::two_a() == 50);
            ctx.check("slots_two_b_observed_60", rsa_fixture::two_b() == 60);

            // mixLongInt — long at base slot 1 (stored at lower slot 2), int at slot 3.
            ctx.check("slots_mix_long_set_ok", g_mix_long_ok.load(std::memory_order_relaxed));
            ctx.check("slots_mix_int_set_ok",  g_mix_int_ok.load(std::memory_order_relaxed));
            ctx.check("slots_mix_long_observed",
                      rsa_fixture::mix_long() == static_cast<std::int64_t>(0x1122334455667788LL));
            ctx.check("slots_mix_int_observed_99", rsa_fixture::mix_int() == 99);

            // intLong — int at slot 1, long at base slot 2 (stored at lower slot 3).
            ctx.check("slots_il_int_set_ok",  g_il_int_ok.load(std::memory_order_relaxed));
            ctx.check("slots_il_long_set_ok", g_il_long_ok.load(std::memory_order_relaxed));
            ctx.check("slots_il_int_observed_77", rsa_fixture::il_int() == 77);
            ctx.check("slots_il_long_observed",
                      rsa_fixture::il_long() == static_cast<std::int64_t>(0x00000000DEADBEEFLL));

            // doubleInt — double at base slot 1, int at slot 3.
            ctx.check("slots_di_double_set_ok", g_di_double_ok.load(std::memory_order_relaxed));
            ctx.check("slots_di_int_set_ok",    g_di_int_ok.load(std::memory_order_relaxed));
            ctx.check("slots_di_double_observed", same_bits(rsa_fixture::di_double(), 98.5));
            ctx.check("slots_di_int_observed_33", rsa_fixture::di_int() == 33);
        }

        ctx.record("[INFO] return_set_arg slot model: a long/double arg consumes TWO interpreter "
                   "slots; set_arg(base, wide) stores the 64-bit value at the LOWER slot (base+1), "
                   "matching the interpreter's lload/dload and the library read path. The int AFTER a "
                   "long (mixLongInt: this=0, long=slots1+2, int=slot3) is mutated only by "
                   "set_arg(3, ...), proving slot index (not argument ordinal) is what set_arg targets.");
    }

    // =====================================================================
    // PART 3 — max_locals BOUNDS: every out-of-range index must be REJECTED
    // (return false) and produce NO wild write — proven by the body still seeing
    // the ORIGINAL value.  Then a single in-range write proves the path still works.
    // =====================================================================
    {
        g_bounds_neg1_rejected.store(false);
        g_bounds_intmin_rejected.store(false);
        g_bounds_65536_rejected.store(false);
        g_bounds_0x10000_rejected.store(false);
        g_bounds_intmax_rejected.store(false);
        rsa_fixture::set_done(false);

        // We DO NOT touch slot 1 here, so the body must observe the ORIGINAL 7.
        // Every set_arg below is out of range (negative or > 0xFFFF) and must be a
        // safe no-op.  set_arg(65535) is the documented off-by-one ceiling case;
        // we deliberately DO NOT execute it on a live frame (it would write one
        // word past the array on a real method) — see the [INFO] below.
        auto h_bounds{ vmhook::scoped_hook<rsa_fixture>("boundsTarget", "(I)V",
            [](vmhook::return_value& r, const std::unique_ptr<rsa_fixture>&, std::int32_t)
            {
                g_bounds_neg1_rejected.store(  !r.set_arg(-1,        static_cast<std::int32_t>(1)), std::memory_order_relaxed);
                g_bounds_intmin_rejected.store(!r.set_arg(INT_MIN,   static_cast<std::int32_t>(2)), std::memory_order_relaxed);
                g_bounds_65536_rejected.store( !r.set_arg(65536,     static_cast<std::int32_t>(3)), std::memory_order_relaxed);
                g_bounds_0x10000_rejected.store(!r.set_arg(0x10000,  static_cast<std::int32_t>(4)), std::memory_order_relaxed);
                g_bounds_intmax_rejected.store(!r.set_arg(INT_MAX,   static_cast<std::int32_t>(5)), std::memory_order_relaxed);
            }) };
        ctx.check("bounds_hook_installed", h_bounds.installed());

        const bool done{ ctx.run_probe(
            [](bool value)
            {
                if (value) { rsa_fixture::set_done(false); rsa_fixture::set_mode(MODE_BOUNDS); }
                rsa_fixture::set_go(value);
            },
            []() { return rsa_fixture::get_done(); }) };
        ctx.check("bounds_probe_completed", done);

        if (done && h_bounds.installed())
        {
            ctx.check("bounds_no_java_exception", !rsa_fixture::saw_exception());
            ctx.check("bounds_neg1_rejected",    g_bounds_neg1_rejected.load(std::memory_order_relaxed));
            ctx.check("bounds_intmin_rejected",  g_bounds_intmin_rejected.load(std::memory_order_relaxed));
            ctx.check("bounds_65536_rejected",   g_bounds_65536_rejected.load(std::memory_order_relaxed));
            ctx.check("bounds_0x10000_rejected", g_bounds_0x10000_rejected.load(std::memory_order_relaxed));
            ctx.check("bounds_intmax_rejected",  g_bounds_intmax_rejected.load(std::memory_order_relaxed));
            // The decisive check: NO out-of-range index produced a wild write, so
            // the body still observed the ORIGINAL argument.
            ctx.check("bounds_body_saw_original", rsa_fixture::bounds_obs() == ORIGINAL_INT);
        }

        ctx.record("[INFO] return_set_arg bounds: negative and >0xFFFF indices are rejected "
                   "(set_arg returns false) and produce NO wild write - the body still saw the "
                   "original argument. The off-by-one at the exact max_locals ceiling (index==65535, "
                   "which is locals[-65535], one word past a real frame) is documented but NOT "
                   "executed on a live frame; the fault-safe write_slot would no-op it anyway.");
    }

    // =====================================================================
    // PART 4 — no-hook BASELINE: with no set_arg installed, the original passed
    // arguments flow through unchanged (proves set_arg is what changed the values
    // above, not some ambient effect).
    // =====================================================================
    {
        reset_round();
        rsa_fixture::set_done(false);
        const bool done{ ctx.run_probe(
            [](bool value)
            {
                if (value) { rsa_fixture::set_done(false); rsa_fixture::set_mode(MODE_PRIMITIVES); }
                rsa_fixture::set_go(value);
            },
            []() { return rsa_fixture::get_done(); }) };
        ctx.check("baseline_probe_completed", done);
        if (done)
        {
            ctx.check("baseline_no_hook_int_is_original",    rsa_fixture::obs_int()  == ORIGINAL_INT);
            ctx.check("baseline_no_hook_long_is_original",   rsa_fixture::obs_long() == static_cast<std::int64_t>(7));
            ctx.check("baseline_no_hook_double_is_original", same_bits(rsa_fixture::obs_double(), 7.0));
            ctx.check("baseline_no_hook_float_is_original",  same_bits(rsa_fixture::obs_float(),  7.0f));
            ctx.check("baseline_no_hook_bool_is_original",   rsa_fixture::obs_bool() == false);
            ctx.check("baseline_no_hook_byte_is_original",   rsa_fixture::obs_byte() == static_cast<std::int8_t>(7));
            ctx.check("baseline_no_hook_char_is_original",   rsa_fixture::obs_char() == static_cast<std::uint16_t>(7));
            ctx.check("baseline_no_hook_short_is_original",  rsa_fixture::obs_short() == static_cast<std::int16_t>(7));
            ctx.check("baseline_no_hook_static_int_is_original", rsa_fixture::obs_static_int() == ORIGINAL_INT);
        }
    }

    // Lifecycle sanity: the probe ran for every round above plus baseline.
    ctx.check("probe_ticked_at_least_once", rsa_fixture::probe_ticks() >= 1);

    ctx.record("[INFO] return_set_arg: injected int/long/double/float/boolean/byte/char/short over "
               "an interpreter ARGUMENT slot (instance slot 1 + static slot 0) across canonical, "
               "zero, signed-min/max, minus-one, -0.0, +/-Inf, qNaN, long-high-dword, "
               "long-low-dword-max, surrogate-char, and canonical-repeat value rounds; plus the "
               "wide/narrow slot model (twoInts, mixLongInt, intLong, doubleInt), the max_locals "
               "bounds rejection (no wild write), and a no-hook baseline. Object/String set_arg is "
               "intentionally OMITTED here (covered crash-proof by return_set_wrapper_null.cpp); this "
               "module performs NO in-detour JVM allocation and NO forced GC.");
}
