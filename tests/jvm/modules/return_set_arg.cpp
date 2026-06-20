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

        // ── wide-slot model observations ───────────────────────────────────
        static auto ll_a()        -> std::int64_t { std::int64_t v = static_field("llA")->get();        return v; }
        static auto ll_b()        -> std::int64_t { std::int64_t v = static_field("llB")->get();        return v; }
        static auto dd_a()        -> double        { double       v = static_field("ddA")->get();        return v; }
        static auto dd_b()        -> double        { double       v = static_field("ddB")->get();        return v; }
        static auto ld_long()     -> std::int64_t { std::int64_t v = static_field("ldLong")->get();     return v; }
        static auto ld_double()   -> double        { double       v = static_field("ldDouble")->get();   return v; }
        static auto dl_double()   -> double        { double       v = static_field("dlDouble")->get();   return v; }
        static auto dl_long()     -> std::int64_t { std::int64_t v = static_field("dlLong")->get();     return v; }
        static auto tri_a()       -> std::int32_t { std::int32_t v = static_field("triA")->get();       return v; }
        static auto tri_b()       -> std::int32_t { std::int32_t v = static_field("triB")->get();       return v; }
        static auto tri_c()       -> std::int32_t { std::int32_t v = static_field("triC")->get();       return v; }
        static auto fl_float()    -> float         { float        v = static_field("flFloat")->get();    return v; }
        static auto fl_long()     -> std::int64_t { std::int64_t v = static_field("flLong")->get();     return v; }
        static auto wide_probe_long() -> std::int64_t { std::int64_t v = static_field("wideProbeLong")->get(); return v; }
        static auto wide_probe_int()  -> std::int32_t { std::int32_t v = static_field("wideProbeInt")->get();  return v; }

        // ── static slot-model observations ─────────────────────────────────
        static auto s_two_a()     -> std::int32_t { std::int32_t v = static_field("sTwoA")->get();     return v; }
        static auto s_two_b()     -> std::int32_t { std::int32_t v = static_field("sTwoB")->get();     return v; }
        static auto s_mix_long()  -> std::int64_t { std::int64_t v = static_field("sMixLong")->get();  return v; }
        static auto s_mix_int()   -> std::int32_t { std::int32_t v = static_field("sMixInt")->get();   return v; }

        // ── receiver-swap / bool-polarity / over-wide / reserved-slot ──────
        static auto ignore_this_v()    -> std::int32_t { std::int32_t v = static_field("ignoreThisV")->get();    return v; }
        static auto ignore_this_ran()  -> bool         { bool v = static_field("ignoreThisRan")->get();          return v; }
        static auto set_ignore_this_ran(bool value)    -> void { static_field("ignoreThisRan")->set(value); }
        static auto bool_poly()        -> bool         { bool v = static_field("boolPoly")->get();                return v; }
        static auto ow_byte()          -> std::int8_t  { std::int8_t  v = static_field("owByte")->get();          return v; }
        static auto ow_byte_wide()     -> std::int32_t { std::int32_t v = static_field("owByteWide")->get();      return v; }
        static auto ow_char()          -> std::uint16_t{ std::uint16_t v = static_field("owChar")->get();         return v; }
        static auto ow_char_wide()     -> std::int32_t { std::int32_t v = static_field("owCharWide")->get();      return v; }
        static auto ow_short()         -> std::int16_t { std::int16_t v = static_field("owShort")->get();         return v; }
        static auto ow_short_wide()    -> std::int32_t { std::int32_t v = static_field("owShortWide")->get();     return v; }
        static auto resv_long()        -> std::int64_t { std::int64_t v = static_field("resvLong")->get();        return v; }
        static auto resv_int()         -> std::int32_t { std::int32_t v = static_field("resvInt")->get();         return v; }
    };

    // Mode selectors (mirror ReturnSetArg.java).
    constexpr std::int32_t MODE_PRIMITIVES{ 0 };
    constexpr std::int32_t MODE_SLOTS{ 1 };
    constexpr std::int32_t MODE_BOUNDS{ 2 };
    constexpr std::int32_t MODE_WIDESLOTS{ 3 };
    constexpr std::int32_t MODE_STATICSLOTS{ 4 };
    constexpr std::int32_t MODE_EXTRA{ 5 };

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

    // Wide-slot model (back-to-back / interleaved long+double).
    std::atomic<bool> g_ll_a_ok{ false };
    std::atomic<bool> g_ll_b_ok{ false };
    std::atomic<bool> g_dd_a_ok{ false };
    std::atomic<bool> g_dd_b_ok{ false };
    std::atomic<bool> g_ld_long_ok{ false };
    std::atomic<bool> g_ld_double_ok{ false };
    std::atomic<bool> g_dl_double_ok{ false };
    std::atomic<bool> g_dl_long_ok{ false };
    std::atomic<bool> g_tri_a_ok{ false };
    std::atomic<bool> g_tri_b_ok{ false };
    std::atomic<bool> g_tri_c_ok{ false };
    std::atomic<bool> g_fl_float_ok{ false };
    std::atomic<bool> g_fl_long_ok{ false };
    std::atomic<bool> g_wide_probe_base_ok{ false };
    std::atomic<bool> g_wide_probe_high_ok{ false };

    // Static slot model.
    std::atomic<bool> g_s_two_a_ok{ false };
    std::atomic<bool> g_s_two_b_ok{ false };
    std::atomic<bool> g_s_mix_long_ok{ false };
    std::atomic<bool> g_s_mix_int_ok{ false };

    // EXTRA round: receiver-swap / bool-polarity / over-wide / reserved-slot.
    std::atomic<bool> g_recv0_ok{ false };       // set_arg(0) on instance returned true
    std::atomic<bool> g_recv1_ok{ false };       // set_arg(1) on the same detour returned true
    std::atomic<bool> g_bool_poly_ok{ false };   // boolPolyTake injection returned true
    std::atomic<bool> g_ow_byte_ok{ false };     // over-wide int into byte slot returned true
    std::atomic<bool> g_ow_char_ok{ false };
    std::atomic<bool> g_ow_short_ok{ false };
    std::atomic<bool> g_resv_long_ok{ false };   // set_arg(2) on resvLongInt returned true

    // Unsigned-source-type primitive injection (set_arg<uintN_t>(...)).
    std::atomic<bool> g_u8_ok{ false };
    std::atomic<bool> g_u16_ok{ false };
    std::atomic<bool> g_u32_ok{ false };
    std::atomic<bool> g_u64_ok{ false };

    // Idempotent / double-write semantics.
    std::atomic<bool> g_idem_ok{ false };
    std::atomic<bool> g_dbl_first_ok{ false };
    std::atomic<bool> g_dbl_second_ok{ false };

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

    // ROUND: plus-one / smallest-magnitude integers; char 0x0001; bool false.
    run_and_check_prim(ctx, "plus_one", prim_values{
        /*i */ 1,
        /*l */ static_cast<std::int64_t>(1),
        /*d */ 1.0,
        /*f */ 1.0f,
        /*b */ false,
        /*by*/ static_cast<std::int8_t>(1),
        /*c */ static_cast<std::uint16_t>(0x0001),
        /*s */ static_cast<std::int16_t>(1) });

    // ROUND: IEEE-754 smallest positive SUBNORMAL float/double — must round-trip
    // bit-exactly (the memcpy path makes no float canonicalisation).
    run_and_check_prim(ctx, "subnormal_min", prim_values{
        /*i */ 11, /*l */ static_cast<std::int64_t>(11),
        /*d */ std::numeric_limits<double>::denorm_min(),
        /*f */ std::numeric_limits<float>::denorm_min(),
        /*b */ true, /*by*/ static_cast<std::int8_t>(11),
        /*c */ static_cast<std::uint16_t>(11), /*s */ static_cast<std::int16_t>(11) });

    // ROUND: largest finite float/double + smallest-normal — full-magnitude finite
    // values that are NOT specials.
    run_and_check_prim(ctx, "finite_extremes", prim_values{
        /*i */ 12, /*l */ static_cast<std::int64_t>(12),
        /*d */ std::numeric_limits<double>::max(),
        /*f */ std::numeric_limits<float>::lowest(),   // -FLT_MAX (negative, finite)
        /*b */ false, /*by*/ static_cast<std::int8_t>(12),
        /*c */ static_cast<std::uint16_t>(12), /*s */ static_cast<std::int16_t>(12) });

    // ROUND: signaling-NaN bit pattern (distinct from the quiet-NaN round) — the
    // memcpy path must preserve the EXACT mantissa bits, not canonicalise to qNaN.
    {
        std::uint32_t snan_bits{ 0x7FA00000u }; // a signaling-NaN float bit pattern
        std::uint64_t snan_bits_d{ 0x7FF4000000000000ull };
        float  f_snan{};  std::memcpy(&f_snan, &snan_bits,   sizeof(f_snan));
        double d_snan{};  std::memcpy(&d_snan, &snan_bits_d, sizeof(d_snan));
        run_and_check_prim(ctx, "snan", prim_values{
            /*i */ 14, /*l */ static_cast<std::int64_t>(14),
            /*d */ d_snan, /*f */ f_snan,
            /*b */ true, /*by*/ static_cast<std::int8_t>(14),
            /*c */ static_cast<std::uint16_t>(14), /*s */ static_cast<std::int16_t>(14) });
    }

    // ROUND: byte/short PLUS-bit-boundary (0x7F / 0x7FFF — the largest positive)
    // and char low-surrogate 0xDFFF (high-bit-set, must zero-extend).
    run_and_check_prim(ctx, "subint_pos_boundary", prim_values{
        /*i */ 0x40000000,
        /*l */ static_cast<std::int64_t>(0x4000000000000000LL),
        /*d */ 6.25, /*f */ 6.5f,
        /*b */ true,
        /*by*/ static_cast<std::int8_t>(0x7F),       // 127
        /*c */ static_cast<std::uint16_t>(0xDFFF),   // low surrogate, zero-extend
        /*s */ static_cast<std::int16_t>(0x7FFF) }); // 32767

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
    // PART 1b — UNSIGNED C++ SOURCE TYPES: set_arg<uintN_t>(...).  The primitive
    // branch is generic on the C++ value_type; an unsigned source takes the same
    // zero-fill+memcpy path.  The Java method signature is unchanged (it is still
    // takeInt/Long/Char/Short); only the C++ value passed to set_arg is unsigned.
    // We inject high-bit-set unsigned values and assert the body's DECLARED-WIDTH
    // field observes the exact low-N-bit truncation — deterministic regardless of
    // sign-extension, so this is a clean HARD assert on every JDK.
    // =====================================================================
    {
        reset_round();
        rsa_fixture::set_done(false);

        // uint8 -> takeByte slot 1.  0xC8 = 200; the byte field truncates to
        // (byte)0xC8 = -56 (deterministic low-8-bit view).
        const std::uint8_t  u8{ static_cast<std::uint8_t>(0xC8) };
        // uint16 -> takeChar slot 1.  0xBEEF; char zero-extends, so the char field
        // observes 0xBEEF exactly.
        const std::uint16_t u16{ static_cast<std::uint16_t>(0xBEEF) };
        // uint32 -> takeInt slot 1.  0xDEADBEEF; the int field is the low 32 bits.
        const std::uint32_t u32{ 0xDEADBEEFu };
        // uint64 -> takeLong slot 1.  Full 64-bit unsigned value round-trips.
        const std::uint64_t u64{ 0xFEEDFACECAFEBABEull };

        auto hu_byte{ vmhook::scoped_hook<rsa_fixture>("takeByte", "(B)V",
            [u8](vmhook::return_value& r, const std::unique_ptr<rsa_fixture>&, std::int32_t)
            { g_u8_ok.store(r.set_arg(1, u8), std::memory_order_relaxed); }) };
        auto hu_char{ vmhook::scoped_hook<rsa_fixture>("takeChar", "(C)V",
            [u16](vmhook::return_value& r, const std::unique_ptr<rsa_fixture>&, std::int32_t)
            { g_u16_ok.store(r.set_arg(1, u16), std::memory_order_relaxed); }) };
        auto hu_int{ vmhook::scoped_hook<rsa_fixture>("takeInt", "(I)V",
            [u32](vmhook::return_value& r, const std::unique_ptr<rsa_fixture>&, std::int32_t)
            { g_u32_ok.store(r.set_arg(1, u32), std::memory_order_relaxed); }) };
        auto hu_long{ vmhook::scoped_hook<rsa_fixture>("takeLong", "(J)V",
            [u64](vmhook::return_value& r, const std::unique_ptr<rsa_fixture>&, std::int64_t)
            { g_u64_ok.store(r.set_arg(1, u64), std::memory_order_relaxed); }) };

        const bool all_installed{
            hu_byte.installed() && hu_char.installed() && hu_int.installed() && hu_long.installed() };
        ctx.check("unsigned_all_hooks_installed", all_installed);

        const bool done{ ctx.run_probe(
            [](bool value)
            {
                if (value) { rsa_fixture::set_done(false); rsa_fixture::set_mode(MODE_PRIMITIVES); }
                rsa_fixture::set_go(value);
            },
            []() { return rsa_fixture::get_done(); }) };
        ctx.check("unsigned_probe_completed", done);

        if (done && all_installed)
        {
            ctx.check("unsigned_no_java_exception", !rsa_fixture::saw_exception());

            ctx.check("unsigned_u8_set_ok",  g_u8_ok.load(std::memory_order_relaxed));
            ctx.check("unsigned_u16_set_ok", g_u16_ok.load(std::memory_order_relaxed));
            ctx.check("unsigned_u32_set_ok", g_u32_ok.load(std::memory_order_relaxed));
            ctx.check("unsigned_u64_set_ok", g_u64_ok.load(std::memory_order_relaxed));

            // byte field == low-8-bit truncation (sign-agnostic).
            ctx.check("unsigned_u8_byte_truncates",
                      rsa_fixture::obs_byte() == static_cast<std::int8_t>(u8));
            // char field zero-extends => exact 16-bit value.
            ctx.check("unsigned_u16_char_exact",
                      rsa_fixture::obs_char() == u16);
            // int field == full low 32 bits.
            ctx.check("unsigned_u32_int_exact",
                      rsa_fixture::obs_int() == static_cast<std::int32_t>(u32));
            // long field == full 64 bits.
            ctx.check("unsigned_u64_long_exact",
                      rsa_fixture::obs_long() == static_cast<std::int64_t>(u64));
        }

        ctx.record("[INFO] return_set_arg unsigned source types: set_arg<uint8/16/32/64_t> take the "
                   "same zero-fill+memcpy primitive path as their signed twins; the body's "
                   "declared-width field observes the exact low-N-bit value (byte 0xC8->-56, char "
                   "0xBEEF exact, int 0xDEADBEEF exact, long 0xFEEDFACECAFEBABE exact).");
    }

    // =====================================================================
    // PART 1c — IDEMPOTENT overwrite + DOUBLE-WRITE last-wins semantics.
    //   * Inject the SAME value the original passes (7): set_arg must succeed and
    //     the body still sees 7 (a no-op overwrite is still a real write).
    //   * Call set_arg twice on the SAME slot in one detour: the SECOND value wins
    //     (the slot holds whatever was written last before the body runs).
    // =====================================================================
    {
        reset_round();
        rsa_fixture::set_done(false);

        // takeStaticInt: overwrite slot 0 with the ORIGINAL value 7 (idempotent).
        auto h_idem{ vmhook::scoped_hook<rsa_fixture>("takeStaticInt", "(I)V",
            [](vmhook::return_value& r, std::int32_t)
            { g_idem_ok.store(r.set_arg(0, ORIGINAL_INT), std::memory_order_relaxed); }) };

        // takeInt: write slot 1 TWICE; the body must observe the second value.
        auto h_dbl{ vmhook::scoped_hook<rsa_fixture>("takeInt", "(I)V",
            [](vmhook::return_value& r, const std::unique_ptr<rsa_fixture>&, std::int32_t)
            {
                g_dbl_first_ok.store(r.set_arg(1, static_cast<std::int32_t>(1111)), std::memory_order_relaxed);
                g_dbl_second_ok.store(r.set_arg(1, static_cast<std::int32_t>(2222)), std::memory_order_relaxed);
            }) };

        const bool all_installed{ h_idem.installed() && h_dbl.installed() };
        ctx.check("idem_all_hooks_installed", all_installed);

        const bool done{ ctx.run_probe(
            [](bool value)
            {
                if (value) { rsa_fixture::set_done(false); rsa_fixture::set_mode(MODE_PRIMITIVES); }
                rsa_fixture::set_go(value);
            },
            []() { return rsa_fixture::get_done(); }) };
        ctx.check("idem_probe_completed", done);

        if (done && all_installed)
        {
            ctx.check("idem_no_java_exception", !rsa_fixture::saw_exception());
            ctx.check("idem_set_ok",            g_idem_ok.load(std::memory_order_relaxed));
            ctx.check("idem_body_saw_original", rsa_fixture::obs_static_int() == ORIGINAL_INT);

            ctx.check("dbl_first_set_ok",  g_dbl_first_ok.load(std::memory_order_relaxed));
            ctx.check("dbl_second_set_ok", g_dbl_second_ok.load(std::memory_order_relaxed));
            // Last write wins: the body sees the SECOND value, not the first.
            ctx.check("dbl_last_write_wins", rsa_fixture::obs_int() == 2222);
        }

        ctx.record("[INFO] return_set_arg idempotent/double-write: overwriting a slot with its own "
                   "original value succeeds and the body sees the original; two set_arg calls on the "
                   "same slot in one detour leave the SECOND value (last write wins) for the body.");
    }

    // =====================================================================
    // PART 1d — BOOLEAN POLARITY MATRIX: the boolean slot is read by the body as
    // an int (iload) and narrowed to a single bit.  Prove BOTH polarities round-
    // trip and that only bit 0 of the injected slot matters (a high-bit-set int
    // source still narrows to its low bit).  The probe's original is false, so the
    // false->true direction is the visible change; the true->true / false->false
    // identity directions are covered by the canonical/zero rounds above.
    // =====================================================================
    {
        struct bool_case { const char* tag; std::int32_t inject; bool expect; };
        const bool_case cases[]{
            { "true",        1,          true  },  // canonical true
            { "false",       0,          false },  // canonical false (== original)
            { "high_bit_0",  0x7FFFFFFE, false },  // bit0==0 -> false despite high bits
            { "high_bit_1",  0x7FFFFFFF, true  },  // bit0==1 -> true  despite high bits
            { "minus_one",  -1,          true  },  // 0xFFFFFFFF -> bit0==1 -> true
            { "two",         2,          false },  // bit0==0 -> false (even value)
        };
        for (const bool_case& c : cases)
        {
            g_bool_poly_ok.store(false, std::memory_order_relaxed);
            rsa_fixture::set_done(false);

            const std::int32_t inject{ c.inject };
            auto h_bp{ vmhook::scoped_hook<rsa_fixture>("boolPolyTake", "(Z)V",
                [inject](vmhook::return_value& r, const std::unique_ptr<rsa_fixture>&, std::int32_t)
                { g_bool_poly_ok.store(r.set_arg(1, inject), std::memory_order_relaxed); }) };

            const std::string tag{ std::string{ "boolpoly_" } + c.tag };
            ctx.check(tag + "_hook_installed", h_bp.installed());

            const bool done{ ctx.run_probe(
                [](bool value)
                {
                    if (value) { rsa_fixture::set_done(false); rsa_fixture::set_mode(MODE_EXTRA); }
                    rsa_fixture::set_go(value);
                },
                []() { return rsa_fixture::get_done(); }) };
            ctx.check(tag + "_probe_completed", done);

            if (done && h_bp.installed())
            {
                ctx.check(tag + "_no_java_exception", !rsa_fixture::saw_exception());
                ctx.check(tag + "_set_ok",  g_bool_poly_ok.load(std::memory_order_relaxed));
                ctx.check(tag + "_observed", rsa_fixture::bool_poly() == c.expect);
            }
        }

        ctx.record("[INFO] return_set_arg boolean polarity: the boolean slot is narrowed to bit 0 by "
                   "the body, so injecting an int with any high bits set still resolves to true/false "
                   "by its low bit (0x7FFFFFFE->false, 0x7FFFFFFF->true, -1->true, 2->false); both "
                   "canonical polarities round-trip.");
    }

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
    // PART 2b — WIDE SLOT MODEL: back-to-back and interleaved long/double args.
    // Each wide arg reserves TWO slots, so the next arg's base slot index is
    // shifted by two.  Distinct injected values per arg make a mis-targeted slot
    // surface as the WRONG field changing.  set_arg(base, wide) stores the 64-bit
    // value at the lower slot (base+1) internally, so we pass the BASE slot index
    // (1, 3, ...) and read the result back from the body.
    // =====================================================================
    {
        reset_round();
        rsa_fixture::set_done(false);

        // longLong(long a, long b): this=0, a=base slot 1, b=base slot 3.
        auto h_ll{ vmhook::scoped_hook<rsa_fixture>("longLong", "(JJ)V",
            [](vmhook::return_value& r, const std::unique_ptr<rsa_fixture>&, std::int64_t, std::int64_t)
            {
                g_ll_a_ok.store(r.set_arg(1, static_cast<std::int64_t>(0x1111111122222222LL)),
                                std::memory_order_relaxed);
                g_ll_b_ok.store(r.set_arg(3, static_cast<std::int64_t>(0x3333333344444444LL)),
                                std::memory_order_relaxed);
            }) };

        // doubleDouble(double a, double b): this=0, a=base slot 1, b=base slot 3.
        auto h_dd{ vmhook::scoped_hook<rsa_fixture>("doubleDouble", "(DD)V",
            [](vmhook::return_value& r, const std::unique_ptr<rsa_fixture>&, double, double)
            {
                g_dd_a_ok.store(r.set_arg(1, 111.25), std::memory_order_relaxed);
                g_dd_b_ok.store(r.set_arg(3, 222.75), std::memory_order_relaxed);
            }) };

        // longDouble(long a, double b): this=0, a=base slot 1, b=base slot 3.
        auto h_ld{ vmhook::scoped_hook<rsa_fixture>("longDouble", "(JD)V",
            [](vmhook::return_value& r, const std::unique_ptr<rsa_fixture>&, std::int64_t, double)
            {
                g_ld_long_ok.store(r.set_arg(1, static_cast<std::int64_t>(0x55667788AABBCCDDLL)),
                                   std::memory_order_relaxed);
                g_ld_double_ok.store(r.set_arg(3, 333.5), std::memory_order_relaxed);
            }) };

        // doubleLong(double a, long b): this=0, a=base slot 1, b=base slot 3.
        auto h_dl{ vmhook::scoped_hook<rsa_fixture>("doubleLong", "(DJ)V",
            [](vmhook::return_value& r, const std::unique_ptr<rsa_fixture>&, double, std::int64_t)
            {
                g_dl_double_ok.store(r.set_arg(1, 444.125), std::memory_order_relaxed);
                g_dl_long_ok.store(r.set_arg(3, static_cast<std::int64_t>(0x00000000CAFEBABELL)),
                                   std::memory_order_relaxed);
            }) };

        // intIntInt(int a, int b, int c): three narrow args in slots 1, 2, 3.
        auto h_tri{ vmhook::scoped_hook<rsa_fixture>("intIntInt", "(III)V",
            [](vmhook::return_value& r, const std::unique_ptr<rsa_fixture>&, std::int32_t, std::int32_t, std::int32_t)
            {
                g_tri_a_ok.store(r.set_arg(1, static_cast<std::int32_t>(100)), std::memory_order_relaxed);
                g_tri_b_ok.store(r.set_arg(2, static_cast<std::int32_t>(200)), std::memory_order_relaxed);
                g_tri_c_ok.store(r.set_arg(3, static_cast<std::int32_t>(300)), std::memory_order_relaxed);
            }) };

        // floatLong(float a, long b): float is ONE slot (slot1), long base slot 2.
        // Proves a one-slot primitive followed by a wide arg: the long base index
        // is 2 (not 3), because the float consumed exactly one slot.
        auto h_fl{ vmhook::scoped_hook<rsa_fixture>("floatLong", "(FJ)V",
            [](vmhook::return_value& r, const std::unique_ptr<rsa_fixture>&, float, std::int64_t)
            {
                g_fl_float_ok.store(r.set_arg(1, 55.5f), std::memory_order_relaxed);
                g_fl_long_ok.store(r.set_arg(2, static_cast<std::int64_t>(0x000000007EADBEEFLL)),
                                   std::memory_order_relaxed);
            }) };

        // wideProbe(long a, int b): this=0, a=base slot 1, b=slot 3.  Mutate ONLY
        // the long (base slot 1) and the trailing int (slot 3); the int at slot 3
        // is reachable only because the long reserved slots 1+2.
        auto h_wp{ vmhook::scoped_hook<rsa_fixture>("wideProbe", "(JI)V",
            [](vmhook::return_value& r, const std::unique_ptr<rsa_fixture>&, std::int64_t, std::int32_t)
            {
                g_wide_probe_base_ok.store(r.set_arg(1, static_cast<std::int64_t>(0x0102030405060708LL)),
                                           std::memory_order_relaxed);
                g_wide_probe_high_ok.store(r.set_arg(3, static_cast<std::int32_t>(4242)),
                                           std::memory_order_relaxed);
            }) };

        const bool all_installed{
            h_ll.installed() && h_dd.installed() && h_ld.installed() && h_dl.installed() &&
            h_tri.installed() && h_fl.installed() && h_wp.installed() };
        ctx.check("wideslots_all_hooks_installed", all_installed);

        const bool done{ ctx.run_probe(
            [](bool value)
            {
                if (value) { rsa_fixture::set_done(false); rsa_fixture::set_mode(MODE_WIDESLOTS); }
                rsa_fixture::set_go(value);
            },
            []() { return rsa_fixture::get_done(); }) };
        ctx.check("wideslots_probe_completed", done);

        if (done && all_installed)
        {
            ctx.check("wideslots_no_java_exception", !rsa_fixture::saw_exception());

            // longLong — two wide args back to back at base slots 1 and 3.
            ctx.check("wideslots_ll_a_set_ok", g_ll_a_ok.load(std::memory_order_relaxed));
            ctx.check("wideslots_ll_b_set_ok", g_ll_b_ok.load(std::memory_order_relaxed));
            ctx.check("wideslots_ll_a_observed",
                      rsa_fixture::ll_a() == static_cast<std::int64_t>(0x1111111122222222LL));
            ctx.check("wideslots_ll_b_observed",
                      rsa_fixture::ll_b() == static_cast<std::int64_t>(0x3333333344444444LL));

            // doubleDouble — two wide doubles back to back.
            ctx.check("wideslots_dd_a_set_ok", g_dd_a_ok.load(std::memory_order_relaxed));
            ctx.check("wideslots_dd_b_set_ok", g_dd_b_ok.load(std::memory_order_relaxed));
            ctx.check("wideslots_dd_a_observed", same_bits(rsa_fixture::dd_a(), 111.25));
            ctx.check("wideslots_dd_b_observed", same_bits(rsa_fixture::dd_b(), 222.75));

            // longDouble — wide long then wide double.
            ctx.check("wideslots_ld_long_set_ok",   g_ld_long_ok.load(std::memory_order_relaxed));
            ctx.check("wideslots_ld_double_set_ok", g_ld_double_ok.load(std::memory_order_relaxed));
            ctx.check("wideslots_ld_long_observed",
                      rsa_fixture::ld_long() == static_cast<std::int64_t>(0x55667788AABBCCDDLL));
            ctx.check("wideslots_ld_double_observed", same_bits(rsa_fixture::ld_double(), 333.5));

            // doubleLong — wide double then wide long.
            ctx.check("wideslots_dl_double_set_ok", g_dl_double_ok.load(std::memory_order_relaxed));
            ctx.check("wideslots_dl_long_set_ok",   g_dl_long_ok.load(std::memory_order_relaxed));
            ctx.check("wideslots_dl_double_observed", same_bits(rsa_fixture::dl_double(), 444.125));
            ctx.check("wideslots_dl_long_observed",
                      rsa_fixture::dl_long() == static_cast<std::int64_t>(0x00000000CAFEBABELL));

            // intIntInt — three independent narrow slots.
            ctx.check("wideslots_tri_a_set_ok", g_tri_a_ok.load(std::memory_order_relaxed));
            ctx.check("wideslots_tri_b_set_ok", g_tri_b_ok.load(std::memory_order_relaxed));
            ctx.check("wideslots_tri_c_set_ok", g_tri_c_ok.load(std::memory_order_relaxed));
            ctx.check("wideslots_tri_a_observed_100", rsa_fixture::tri_a() == 100);
            ctx.check("wideslots_tri_b_observed_200", rsa_fixture::tri_b() == 200);
            ctx.check("wideslots_tri_c_observed_300", rsa_fixture::tri_c() == 300);

            // floatLong — one-slot float then wide long at base slot 2.
            ctx.check("wideslots_fl_float_set_ok", g_fl_float_ok.load(std::memory_order_relaxed));
            ctx.check("wideslots_fl_long_set_ok",  g_fl_long_ok.load(std::memory_order_relaxed));
            ctx.check("wideslots_fl_float_observed", same_bits(rsa_fixture::fl_float(), 55.5f));
            ctx.check("wideslots_fl_long_observed",
                      rsa_fixture::fl_long() == static_cast<std::int64_t>(0x000000007EADBEEFLL));

            // wideProbe — long base 1 + trailing int slot 3 both land.
            ctx.check("wideslots_wp_base_set_ok", g_wide_probe_base_ok.load(std::memory_order_relaxed));
            ctx.check("wideslots_wp_high_set_ok", g_wide_probe_high_ok.load(std::memory_order_relaxed));
            ctx.check("wideslots_wp_long_observed",
                      rsa_fixture::wide_probe_long() == static_cast<std::int64_t>(0x0102030405060708LL));
            ctx.check("wideslots_wp_int_observed_4242", rsa_fixture::wide_probe_int() == 4242);
        }

        ctx.record("[INFO] return_set_arg wide-slot model: back-to-back wide args (longLong, "
                   "doubleDouble, longDouble, doubleLong) each occupy a 2-slot pair, so the second "
                   "wide arg's BASE slot index is 3 (this=0, first-wide=1+2, second-wide=3+4). A "
                   "one-slot float before a long (floatLong) shifts the long's base to slot 2, not 3. "
                   "All round-trip through the body's lload/dload, proving set_arg's wide write lands "
                   "on the interpreter's lower slot for every interleaving.");
    }

    // =====================================================================
    // PART 2c — STATIC slot model: a static method has NO 'this', so the first
    // argument begins at slot 0 (not slot 1).  This complements the static-int
    // single-arg case in PART 1 with multi-arg + wide-arg static shapes.
    // =====================================================================
    {
        reset_round();
        rsa_fixture::set_done(false);

        // staticTwoInts(int a, int b): a=slot0, b=slot1.
        auto h_s_two{ vmhook::scoped_hook<rsa_fixture>("staticTwoInts", "(II)V",
            [](vmhook::return_value& r, std::int32_t, std::int32_t)
            {
                g_s_two_a_ok.store(r.set_arg(0, static_cast<std::int32_t>(501)), std::memory_order_relaxed);
                g_s_two_b_ok.store(r.set_arg(1, static_cast<std::int32_t>(601)), std::memory_order_relaxed);
            }) };

        // staticLongInt(long a, int b): a=base slot 0 (slots 0+1), b=slot2.
        // A wide arg with NO 'this' offset: the trailing int is at slot 2, proving
        // the two-slot reservation holds from slot 0 on static methods too.
        auto h_s_mix{ vmhook::scoped_hook<rsa_fixture>("staticLongInt", "(JI)V",
            [](vmhook::return_value& r, std::int64_t, std::int32_t)
            {
                g_s_mix_long_ok.store(r.set_arg(0, static_cast<std::int64_t>(0x7766554433221100LL)),
                                      std::memory_order_relaxed);
                g_s_mix_int_ok.store(r.set_arg(2, static_cast<std::int32_t>(909)), std::memory_order_relaxed);
            }) };

        const bool all_installed{ h_s_two.installed() && h_s_mix.installed() };
        ctx.check("staticslots_all_hooks_installed", all_installed);

        const bool done{ ctx.run_probe(
            [](bool value)
            {
                if (value) { rsa_fixture::set_done(false); rsa_fixture::set_mode(MODE_STATICSLOTS); }
                rsa_fixture::set_go(value);
            },
            []() { return rsa_fixture::get_done(); }) };
        ctx.check("staticslots_probe_completed", done);

        if (done && all_installed)
        {
            ctx.check("staticslots_no_java_exception", !rsa_fixture::saw_exception());

            ctx.check("staticslots_s_two_a_set_ok", g_s_two_a_ok.load(std::memory_order_relaxed));
            ctx.check("staticslots_s_two_b_set_ok", g_s_two_b_ok.load(std::memory_order_relaxed));
            ctx.check("staticslots_s_two_a_observed_501", rsa_fixture::s_two_a() == 501);
            ctx.check("staticslots_s_two_b_observed_601", rsa_fixture::s_two_b() == 601);

            ctx.check("staticslots_s_mix_long_set_ok", g_s_mix_long_ok.load(std::memory_order_relaxed));
            ctx.check("staticslots_s_mix_int_set_ok",  g_s_mix_int_ok.load(std::memory_order_relaxed));
            ctx.check("staticslots_s_mix_long_observed",
                      rsa_fixture::s_mix_long() == static_cast<std::int64_t>(0x7766554433221100LL));
            ctx.check("staticslots_s_mix_int_observed_909", rsa_fixture::s_mix_int() == 909);
        }

        ctx.record("[INFO] return_set_arg static slot model: static methods have no 'this', so the "
                   "first arg is slot 0 (staticTwoInts: a=0,b=1) and a wide first arg reserves slots "
                   "0+1, putting the trailing int at slot 2 (staticLongInt) - identical two-slot "
                   "accounting to instance methods, just without the slot-0 receiver.");
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
    // PART 5 — RECEIVER SWAP + OVER-WIDE-INTO-NARROW + RESERVED-SLOT distinctness.
    // All exercised in ONE MODE_EXTRA probe so each fires exactly once.
    //
    //  * ignoreThis(int v): set_arg(0, ...) overwrites the 'this' receiver slot.
    //    The body never dereferences 'this', so this is crash-safe; we prove the
    //    write to slot 0 RETURNS TRUE (the receiver slot is writable) and that the
    //    independent set_arg(1, ...) mutates the slot-1 arg — i.e. slot 0 and slot
    //    1 are distinct, and the body still ran to completion (no crash).
    //
    //  * owByteTake/owCharTake/owShortTake: inject an int value WIDER than the
    //    declared byte/char/short.  The native-width masked readback is HARD
    //    (stable across the matrix); the int-WIDENED readback is platform-variant
    //    (windows vs linux) and is recorded [INFO], never asserted.
    //
    //  * resvLongInt(long a, int b): the long's value occupies slot 2, b is at
    //    slot 3.  A caller who thinks set_arg's index is an ARGUMENT ordinal calls
    //    set_arg(2, ...) to change "the second argument" b — but slot 2 is part of
    //    the long, and b is untouched.  Prove set_arg(2,...) does NOT change b at
    //    slot 3: set_arg targets a raw SLOT index, not an argument ordinal (the
    //    documented Flaw #1 behaviour, pinned).
    // =====================================================================
    {
        g_recv0_ok.store(false, std::memory_order_relaxed);
        g_recv1_ok.store(false, std::memory_order_relaxed);
        g_ow_byte_ok.store(false, std::memory_order_relaxed);
        g_ow_char_ok.store(false, std::memory_order_relaxed);
        g_ow_short_ok.store(false, std::memory_order_relaxed);
        g_resv_long_ok.store(false, std::memory_order_relaxed);
        rsa_fixture::set_ignore_this_ran(false);
        rsa_fixture::set_done(false);

        // RECEIVER SWAP: overwrite slot 0 ('this') AND slot 1 (the int arg).  The
        // receiver value we write is an arbitrary primitive (the body ignores it);
        // the slot-1 arg becomes 4321 and must be observed by the body.
        auto h_recv{ vmhook::scoped_hook<rsa_fixture>("ignoreThis", "(I)V",
            [](vmhook::return_value& r, const std::unique_ptr<rsa_fixture>&, std::int32_t)
            {
                g_recv0_ok.store(r.set_arg(0, static_cast<std::int32_t>(0x6B6B6B6B)), std::memory_order_relaxed);
                g_recv1_ok.store(r.set_arg(1, static_cast<std::int32_t>(4321)),       std::memory_order_relaxed);
            }) };

        // OVER-WIDE into narrow slots: an int wider than the declared sub-int type.
        const std::int32_t OW_INJECT{ 0x12345678 };
        auto h_owb{ vmhook::scoped_hook<rsa_fixture>("owByteTake", "(B)V",
            [OW_INJECT](vmhook::return_value& r, const std::unique_ptr<rsa_fixture>&, std::int32_t)
            { g_ow_byte_ok.store(r.set_arg(1, OW_INJECT), std::memory_order_relaxed); }) };
        auto h_owc{ vmhook::scoped_hook<rsa_fixture>("owCharTake", "(C)V",
            [OW_INJECT](vmhook::return_value& r, const std::unique_ptr<rsa_fixture>&, std::int32_t)
            { g_ow_char_ok.store(r.set_arg(1, OW_INJECT), std::memory_order_relaxed); }) };
        auto h_ows{ vmhook::scoped_hook<rsa_fixture>("owShortTake", "(S)V",
            [OW_INJECT](vmhook::return_value& r, const std::unique_ptr<rsa_fixture>&, std::int32_t)
            { g_ow_short_ok.store(r.set_arg(1, OW_INJECT), std::memory_order_relaxed); }) };

        // RESERVED SLOT (Flaw #1 pin): resvLongInt(long a, int b): this=0,
        // a=base slot 1 (its 64-bit value lives at the lower slot 2), b=slot 3.
        // A user who treats set_arg's index as an ARGUMENT ordinal would call
        // set_arg(2, ...) to change "the second argument" b — but slot 2 is the
        // long's value slot, and b is at slot 3.  So set_arg(2, int) does NOT
        // change b; the trailing int must remain the ORIGINAL 8.  We do NOT write
        // the long's base slot here (that would itself land on slot 2 and the int
        // clobber would corrupt it), keeping the demonstration clean: ONE int
        // write at slot 2, then prove b@slot3 is untouched.
        auto h_resv{ vmhook::scoped_hook<rsa_fixture>("resvLongInt", "(JI)V",
            [](vmhook::return_value& r, const std::unique_ptr<rsa_fixture>&, std::int64_t, std::int32_t)
            {
                g_resv_long_ok.store(r.set_arg(2, static_cast<std::int32_t>(0x7777)),
                                     std::memory_order_relaxed);
            }) };

        const bool all_installed{
            h_recv.installed() && h_owb.installed() && h_owc.installed() &&
            h_ows.installed()  && h_resv.installed() };
        ctx.check("extra_all_hooks_installed", all_installed);

        const bool done{ ctx.run_probe(
            [](bool value)
            {
                if (value) { rsa_fixture::set_done(false); rsa_fixture::set_mode(MODE_EXTRA); }
                rsa_fixture::set_go(value);
            },
            []() { return rsa_fixture::get_done(); }) };
        ctx.check("extra_probe_completed", done);

        if (done && all_installed)
        {
            ctx.check("extra_no_java_exception", !rsa_fixture::saw_exception());

            // ── receiver swap ──
            ctx.check("extra_recv0_set_ok",       g_recv0_ok.load(std::memory_order_relaxed));
            ctx.check("extra_recv1_set_ok",       g_recv1_ok.load(std::memory_order_relaxed));
            ctx.check("extra_recv_body_ran",      rsa_fixture::ignore_this_ran());
            ctx.check("extra_recv_arg_observed",  rsa_fixture::ignore_this_v() == 4321);

            // ── over-wide into narrow: native-width masked view is HARD ──
            const std::int8_t   want_byte { static_cast<std::int8_t>(OW_INJECT) };
            const std::uint16_t want_char { static_cast<std::uint16_t>(OW_INJECT) };
            const std::int16_t  want_short{ static_cast<std::int16_t>(OW_INJECT) };
            ctx.check("extra_ow_byte_set_ok",  g_ow_byte_ok.load(std::memory_order_relaxed));
            ctx.check("extra_ow_char_set_ok",  g_ow_char_ok.load(std::memory_order_relaxed));
            ctx.check("extra_ow_short_set_ok", g_ow_short_ok.load(std::memory_order_relaxed));
            ctx.check("extra_ow_byte_masked",  rsa_fixture::ow_byte()  == want_byte);
            ctx.check("extra_ow_char_masked",  rsa_fixture::ow_char()  == want_char);
            ctx.check("extra_ow_short_masked", rsa_fixture::ow_short() == want_short);

            // The int-WIDENED readback of an over-wide injection is platform-variant
            // (windows vs linux mask/widen the over-wide slot differently); record
            // it [INFO], never assert.
            ctx.record("[INFO] return_set_arg over-wide-into-narrow: injected int 0x12345678 over a "
                       "byte/char/short slot. Native-width masked views (HARD): byte=0x78, char=0x5678, "
                       "short=0x5678. Observed int-WIDENED views (platform-variant, [INFO] only): byte_wide="
                       + std::to_string(rsa_fixture::ow_byte_wide())
                       + ", char_wide=" + std::to_string(rsa_fixture::ow_char_wide())
                       + ", short_wide=" + std::to_string(rsa_fixture::ow_short_wide()) + ".");

            // ── reserved-slot distinctness (Flaw #1, pinned) ──
            // set_arg(2, int) (the long's value slot) returns true (a valid in-range
            // write) but does NOT touch b at slot 3: b must remain the ORIGINAL 8.
            ctx.check("extra_resv_set_ok", g_resv_long_ok.load(std::memory_order_relaxed));
            ctx.check("extra_resv_int_untouched", rsa_fixture::resv_int() == 8);
        }

        ctx.record("[INFO] return_set_arg receiver-swap / reserved-slot: set_arg(0, ...) overwrites the "
                   "'this' receiver slot of an instance method (returns true; crash-safe only because "
                   "the body never dereferences 'this'), and is distinct from set_arg(1, ...) which "
                   "mutates the first arg. Writing a long's RESERVED high slot (set_arg(2,...) on "
                   "resvLongInt) does NOT change the trailing int at slot 3 - set_arg targets a raw "
                   "SLOT index, never an argument ordinal (documented Flaw #1).");
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
               "zero, signed-min/max, plus-one, minus-one, -0.0, +/-Inf, qNaN, sNaN, subnormal-min, "
               "finite-extremes, long-high-dword, long-low-dword-max, surrogate-char, subint-pos-"
               "boundary, and canonical-repeat value rounds; unsigned C++ source types "
               "(uint8/16/32/64); the boolean polarity matrix (only bit 0 of the slot matters); "
               "idempotent overwrite + double-write last-wins; the wide/narrow slot model (twoInts, "
               "mixLongInt, intLong, doubleInt) plus back-to-back and interleaved wide args (longLong, "
               "doubleDouble, longDouble, doubleLong, floatLong, intIntInt, wideProbe); the STATIC "
               "multi-arg + wide-arg slot model (staticTwoInts, staticLongInt); the receiver swap "
               "(set_arg(0) on an instance method) + reserved-slot distinctness (writing a long's high "
               "slot does not change the trailing arg); the over-wide-into-narrow masking case "
               "(native-width HARD, int-widened [INFO]); the max_locals bounds rejection (no wild "
               "write); and a no-hook baseline. Object/String set_arg is intentionally OMITTED here "
               "(covered crash-proof by return_set_wrapper_null.cpp); this module performs NO in-detour "
               "JVM allocation and NO forced GC.");
}
