// return_value_cancel JVM test module — exhaustively exercises
// vmhook::return_value::cancel() (the force-CANCEL path) on a LIVE JVM.
//
// Feature under test: vmhook/ext/vmhook/vmhook.hpp:1205-1209.
//   cancel() is a 3-line setter that flips return_slot::cancel = true WITHOUT
//   touching return_slot::retval.  When the trampoline takes the cancel path it
//   unconditionally loads the (zero-initialised) retval cell at [rsp+8] into BOTH
//   rax (integer return) and xmm0 (float/double return) — regardless of the Java
//   method's return descriptor.  The real cancel epilogue (mov rax,[rsp+8] ;
//   movq xmm0,rax) is at vmhook.hpp:5433-5434 (Win64) / 5530-5531 (SysV), with
//   the slot-zeroing pushes at 5414-5415 / 5512-5513.  (An earlier revision of
//   this header cited 5371-5372 / 5468-5469; those are unrelated code — the
//   current_chain_resume ternary and the SysV banner — corrected here.)
//   Consequences this module locks in on a real JVM:
//     * void method            -> original body is SKIPPED (no side effect),
//     * int / long returner     -> Java caller receives 0 / 0L,
//     * double returner         -> Java caller receives +0.0 (NOT NaN, sign +),
//     * boolean returner        -> Java caller receives false,
//     * char returner           -> Java caller receives U+0000,
//     * reference returner       -> Java caller receives null.
//   These are exactly the "silent footgun on non-void methods" + the zero-fill /
//   xmm0 / reference-null scenarios the audit finding raises
//   (audit/findings/return_value_cancel.md, the [jvm_integration] [new] cases).
//
// Strategy mirrors return_set_primitives.cpp / pilot.cpp: the fixture is a dumb
// actor (ReturnValueCancel.java) that, per probe, calls each orig* method and
// records what the Java caller OBSERVED.  Each orig* returns a fixed non-zero /
// non-null value the native side NEVER forces, so an observed 0 / null / +0.0
// can only mean the cancel path delivered it.  Every round installs fresh
// scoped_hooks in a nested block (never shutdown_hooks): they disarm at block
// end so the next round installs clean, which also lets us re-prove the cancel
// path is stable across repeated arm/disarm cycles.
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
    // Wrapper for vmhook.fixtures.ReturnValueCancel.
    class rvc_fixture : public vmhook::object<rvc_fixture>
    {
    public:
        explicit rvc_fixture(vmhook::oop_t instance) noexcept
            : vmhook::object<rvc_fixture>{ instance }
        {
        }

        // ── go/done handshake + mode selector ──────────────────────────────
        static auto set_go(bool value)   -> void { static_field("go")->set(value); }
        static auto get_done()           -> bool { return static_field("done")->get(); }
        static auto set_done(bool value) -> void { static_field("done")->set(value); }
        static auto set_mode(std::int32_t m) -> void { static_field("mode")->set(m); }

        // ── observed return values (instance dispatch) ─────────────────────
        static auto obs_int()    -> std::int32_t { return static_field("obsInt")->get(); }
        static auto obs_long()   -> std::int64_t { return static_field("obsLong")->get(); }
        static auto obs_double() -> double        { return static_field("obsDouble")->get(); }
        static auto obs_bool()   -> bool          { return static_field("obsBool")->get(); }
        static auto obs_char()   -> std::uint16_t { return static_field("obsChar")->get(); }
        static auto obs_byte()   -> std::int8_t   { return static_field("obsByte")->get(); }
        static auto obs_short()  -> std::int16_t  { return static_field("obsShort")->get(); }
        static auto obs_float()  -> float         { return static_field("obsFloat")->get(); }
        static auto obs_ref_is_null()  -> bool          { return static_field("obsRefIsNull")->get(); }
        static auto obs_ref_identity() -> std::int32_t  { return static_field("obsRefIdentity")->get(); }
        static auto obs_str_is_null()  -> bool          { return static_field("obsStrIsNull")->get(); }
        static auto obs_str_len()      -> std::int32_t  { return static_field("obsStrLen")->get(); }
        static auto obs_static_str_is_null() -> bool         { return static_field("obsStaticStrIsNull")->get(); }
        static auto obs_static_str_len()     -> std::int32_t { return static_field("obsStaticStrLen")->get(); }
        static auto obs_arg_return()        -> std::int32_t { return static_field("obsArgReturn")->get(); }
        static auto obs_static_arg_return() -> std::int32_t { return static_field("obsStaticArgReturn")->get(); }
        static auto arg_echo()         -> std::int32_t { return static_field("argEcho")->get(); }
        static auto static_arg_echo()  -> std::int32_t { return static_field("staticArgEcho")->get(); }
        static auto obs_double_was_nan()      -> bool { return static_field("obsDoubleWasNaN")->get(); }
        static auto obs_double_was_neg_zero() -> bool { return static_field("obsDoubleWasNegZero")->get(); }
        static auto obs_float_was_nan()       -> bool { return static_field("obsFloatWasNaN")->get(); }
        static auto obs_float_was_neg_zero()  -> bool { return static_field("obsFloatWasNegZero")->get(); }

        // ── observed return values (static dispatch) ───────────────────────
        static auto obs_static_int()    -> std::int32_t { return static_field("obsStaticInt")->get(); }
        static auto obs_static_double() -> double        { return static_field("obsStaticDouble")->get(); }
        static auto obs_static_long()   -> std::int64_t { return static_field("obsStaticLong")->get(); }
        static auto obs_static_bool()   -> bool          { return static_field("obsStaticBool")->get(); }
        static auto obs_static_char()   -> std::uint16_t { return static_field("obsStaticChar")->get(); }
        static auto obs_static_byte()   -> std::int8_t   { return static_field("obsStaticByte")->get(); }
        static auto obs_static_short()  -> std::int16_t  { return static_field("obsStaticShort")->get(); }
        static auto obs_static_float()  -> float         { return static_field("obsStaticFloat")->get(); }
        static auto obs_static_float_was_neg_zero() -> bool { return static_field("obsStaticFloatWasNegZero")->get(); }

        // ── void side-effect witnesses ─────────────────────────────────────
        static auto side_effect()        -> std::int32_t { return static_field("sideEffect")->get(); }
        static auto static_side_effect() -> std::int32_t { return static_field("staticSideEffect")->get(); }
        static auto side_effect_after_call1() -> std::int32_t { return static_field("sideEffectAfterCall1")->get(); }
        static auto side_effect_after_call2() -> std::int32_t { return static_field("sideEffectAfterCall2")->get(); }
        static auto side_effect_after_call3() -> std::int32_t { return static_field("sideEffectAfterCall3")->get(); }

        // ── control ────────────────────────────────────────────────────────
        static auto saw_exception() -> bool         { return static_field("sawException")->get(); }
        static auto round_count()   -> std::int32_t { return static_field("roundCount")->get(); }
    };

    // Probe-mode selectors — MUST match the MODE_* constants in
    // ReturnValueCancel.java.  The native side writes `mode` before raising
    // `go`; the fixture's action branches on it.
    struct mode_consts
    {
        static constexpr std::int32_t observe_all{ 0 };
        static constexpr std::int32_t void_twice{ 1 };
        static constexpr std::int32_t void_thrice{ 2 };
    };

    // +0.0 is delivered as all-zero bits; -0.0 has the sign bit set.  Bit-exact
    // so we can prove the cancel path produced a TRUE positive zero (not -0.0,
    // not a NaN with a zero-ish payload).
    auto is_positive_zero(double value) -> bool
    {
        std::uint64_t bits{};
        std::memcpy(&bits, &value, sizeof(bits));
        return bits == 0u;
    }

    // 32-bit twin of the above: +0.0f is all-zero bits, -0.0f sets bit 31.
    // The float cancel path rides the SAME movq xmm0 epilogue but the JVM's
    // freturn reads only the low 32 bits of the SSE register, so a true +0.0f
    // proves the low dword of the zero-filled slot landed in xmm0 intact.
    auto is_positive_zero(float value) -> bool
    {
        std::uint32_t bits{};
        std::memcpy(&bits, &value, sizeof(bits));
        return bits == 0u;
    }

    // Bit-exact float/double equality so a forced -0.0 / NaN / boundary value
    // that SURVIVES a later cancel() is distinguished from a zero-fill.
    auto same_bits(float a, float b) -> bool
    {
        std::uint32_t ua{};
        std::uint32_t ub{};
        std::memcpy(&ua, &a, sizeof(ua));
        std::memcpy(&ub, &b, sizeof(ub));
        return ua == ub;
    }
    auto same_bits(double a, double b) -> bool
    {
        std::uint64_t ua{};
        std::uint64_t ub{};
        std::memcpy(&ua, &a, sizeof(ua));
        std::memcpy(&ub, &b, sizeof(ub));
        return ua == ub;
    }

    // ── per-round detour bookkeeping (reset before each probe) ──────────────
    std::atomic<int>  g_inst_fires{ 0 };
    std::atomic<int>  g_stat_fires{ 0 };
    std::atomic<bool> g_inst_all_saw_self{ true };

    auto reset_counters() -> void
    {
        g_inst_fires.store(0, std::memory_order_relaxed);
        g_stat_fires.store(0, std::memory_order_relaxed);
        g_inst_all_saw_self.store(true, std::memory_order_relaxed);
    }

    auto note_inst(const std::unique_ptr<rvc_fixture>& self) -> void
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

    // Drive ONE MODE_OBSERVE_ALL probe and return whether it completed.
    auto run_observe_probe(vmhook_test::context& ctx, const std::string& tag) -> bool
    {
        rvc_fixture::set_mode(mode_consts::observe_all);
        rvc_fixture::set_done(false);
        const bool done{ ctx.run_probe(
            [](bool value) { rvc_fixture::set_go(value); },
            []() { return rvc_fixture::get_done(); }) };
        ctx.check(tag + "_probe_completed", done);
        return done;
    }
}

VMHOOK_JVM_MODULE(return_value_cancel)
{
    vmhook::register_class<rvc_fixture>("vmhook/fixtures/ReturnValueCancel");

    // ===================================================================
    // ROUND 0 — BASELINE: no hooks installed at all.  Every orig* body
    // runs and its non-zero / non-null value flows to the Java caller.
    // This is the control: it proves the later "observed 0/null/+0.0"
    // results are caused by cancel(), not by some ambient effect, and
    // that the fixture's own plumbing reports the original values.
    // ===================================================================
    {
        reset_counters();
        rvc_fixture::set_mode(mode_consts::observe_all);
        rvc_fixture::set_done(false);
        const bool done{ ctx.run_probe(
            [](bool value) { rvc_fixture::set_go(value); },
            []() { return rvc_fixture::get_done(); }) };
        ctx.check("baseline_probe_completed", done);
        if (done)
        {
            ctx.check("baseline_no_hook_fired",
                      g_inst_fires.load() == 0 && g_stat_fires.load() == 0);
            ctx.check("baseline_no_java_exception", !rvc_fixture::saw_exception());
            // Original values flow unchanged.
            ctx.check("baseline_int_is_1111",    rvc_fixture::obs_int()  == 1111);
            ctx.check("baseline_long_is_orig",   rvc_fixture::obs_long() == static_cast<std::int64_t>(0x7FFFFFFF00000001LL));
            ctx.check("baseline_double_is_11_25",rvc_fixture::obs_double() == 11.25);
            ctx.check("baseline_bool_is_true",   rvc_fixture::obs_bool() == true);
            ctx.check("baseline_char_is_A",      rvc_fixture::obs_char() == static_cast<std::uint16_t>('A'));
            ctx.check("baseline_byte_is_88",     rvc_fixture::obs_byte()  == static_cast<std::int8_t>(88));
            ctx.check("baseline_short_is_7777",  rvc_fixture::obs_short() == static_cast<std::int16_t>(7777));
            ctx.check("baseline_float_is_33_5",  rvc_fixture::obs_float() == 33.5f);
            ctx.check("baseline_ref_not_null",   !rvc_fixture::obs_ref_is_null());
            ctx.check("baseline_ref_identity_nonzero", rvc_fixture::obs_ref_identity() != 0);
            ctx.check("baseline_static_int_2222",rvc_fixture::obs_static_int() == 2222);
            ctx.check("baseline_static_double_22_25", rvc_fixture::obs_static_double() == 22.25);
            ctx.check("baseline_static_long_is_orig",
                      rvc_fixture::obs_static_long() == static_cast<std::int64_t>(0x7FFFFFFF00000002LL));
            ctx.check("baseline_static_bool_is_true",  rvc_fixture::obs_static_bool() == true);
            ctx.check("baseline_static_char_is_Z",     rvc_fixture::obs_static_char() == static_cast<std::uint16_t>('Z'));
            ctx.check("baseline_static_byte_is_66",    rvc_fixture::obs_static_byte()  == static_cast<std::int8_t>(66));
            ctx.check("baseline_static_short_is_6666", rvc_fixture::obs_static_short() == static_cast<std::int16_t>(6666));
            ctx.check("baseline_static_float_is_44_5", rvc_fixture::obs_static_float() == 44.5f);
            // String returns flow non-null with their original length.
            ctx.check("baseline_str_not_null",        !rvc_fixture::obs_str_is_null());
            ctx.check("baseline_str_len_is_orig",     rvc_fixture::obs_str_len() == 10);
            ctx.check("baseline_static_str_not_null", !rvc_fixture::obs_static_str_is_null());
            ctx.check("baseline_static_str_len_is_orig", rvc_fixture::obs_static_str_len() == 10);
            // Arg-taking bodies ran with the caller's argument: echo + return.
            ctx.check("baseline_arg_return_is_303",        rvc_fixture::obs_arg_return() == 303);
            ctx.check("baseline_arg_echo_is_303",          rvc_fixture::arg_echo() == 303);
            ctx.check("baseline_static_arg_return_is_404", rvc_fixture::obs_static_arg_return() == 404);
            ctx.check("baseline_static_arg_echo_is_404",   rvc_fixture::static_arg_echo() == 404);
            // Void bodies ran: their side effects advanced.
            ctx.check("baseline_void_side_effect_ran",        rvc_fixture::side_effect() == 7);
            ctx.check("baseline_static_void_side_effect_ran", rvc_fixture::static_side_effect() == 13);
        }
    }

    // Snapshots of the void side-effect counters AFTER the baseline ran, so the
    // cancel rounds can assert "did NOT advance" relative to this point.
    const std::int32_t side_effect_after_baseline{ rvc_fixture::side_effect() };
    const std::int32_t static_side_effect_after_baseline{ rvc_fixture::static_side_effect() };

    // ===================================================================
    // ROUND 1 — CANCEL-WITHOUT-SET on every method.  Each detour calls
    // ONLY retval.cancel() (never set()).  This is the heart of the
    // feature: the trampoline must skip every original body and deliver
    // the zero-filled slot as 0 / 0L / +0.0 / false / U+0000 / null.
    // ===================================================================
    {
        reset_counters();

        // INSTANCE hooks — cancel only.
        auto h_void  { vmhook::scoped_hook<rvc_fixture>("origVoid",
            [](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            { note_inst(self); r.cancel(); }) };
        auto h_int   { vmhook::scoped_hook<rvc_fixture>("origInt",
            [](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            { note_inst(self); r.cancel(); }) };
        auto h_long  { vmhook::scoped_hook<rvc_fixture>("origLong",
            [](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            { note_inst(self); r.cancel(); }) };
        auto h_double{ vmhook::scoped_hook<rvc_fixture>("origDouble",
            [](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            { note_inst(self); r.cancel(); }) };
        auto h_bool  { vmhook::scoped_hook<rvc_fixture>("origBool",
            [](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            { note_inst(self); r.cancel(); }) };
        auto h_char  { vmhook::scoped_hook<rvc_fixture>("origChar",
            [](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            { note_inst(self); r.cancel(); }) };
        auto h_ref   { vmhook::scoped_hook<rvc_fixture>("origRef",
            [](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            { note_inst(self); r.cancel(); }) };
        auto h_byte  { vmhook::scoped_hook<rvc_fixture>("origByte",
            [](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            { note_inst(self); r.cancel(); }) };
        auto h_short { vmhook::scoped_hook<rvc_fixture>("origShort",
            [](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            { note_inst(self); r.cancel(); }) };
        auto h_float { vmhook::scoped_hook<rvc_fixture>("origFloat",
            [](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            { note_inst(self); r.cancel(); }) };
        auto h_str   { vmhook::scoped_hook<rvc_fixture>("origStr",
            [](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            { note_inst(self); r.cancel(); }) };
        auto h_arg   { vmhook::scoped_hook<rvc_fixture>("origIntFromArg",
            [](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            { note_inst(self); r.cancel(); }) };

        // STATIC hooks — cancel only (no 'this').  Every static return descriptor
        // is exercised so the static dispatch path's zero-fill is proven for the
        // full primitive set, not just int/double.
        auto hs_void  { vmhook::scoped_hook<rvc_fixture>("origStaticVoid",
            [](vmhook::return_value& r) { note_static(); r.cancel(); }) };
        auto hs_int   { vmhook::scoped_hook<rvc_fixture>("origStaticInt",
            [](vmhook::return_value& r) { note_static(); r.cancel(); }) };
        auto hs_double{ vmhook::scoped_hook<rvc_fixture>("origStaticDouble",
            [](vmhook::return_value& r) { note_static(); r.cancel(); }) };
        auto hs_long  { vmhook::scoped_hook<rvc_fixture>("origStaticLong",
            [](vmhook::return_value& r) { note_static(); r.cancel(); }) };
        auto hs_bool  { vmhook::scoped_hook<rvc_fixture>("origStaticBool",
            [](vmhook::return_value& r) { note_static(); r.cancel(); }) };
        auto hs_char  { vmhook::scoped_hook<rvc_fixture>("origStaticChar",
            [](vmhook::return_value& r) { note_static(); r.cancel(); }) };
        auto hs_byte  { vmhook::scoped_hook<rvc_fixture>("origStaticByte",
            [](vmhook::return_value& r) { note_static(); r.cancel(); }) };
        auto hs_short { vmhook::scoped_hook<rvc_fixture>("origStaticShort",
            [](vmhook::return_value& r) { note_static(); r.cancel(); }) };
        auto hs_float { vmhook::scoped_hook<rvc_fixture>("origStaticFloat",
            [](vmhook::return_value& r) { note_static(); r.cancel(); }) };
        auto hs_str   { vmhook::scoped_hook<rvc_fixture>("origStaticStr",
            [](vmhook::return_value& r) { note_static(); r.cancel(); }) };
        auto hs_arg   { vmhook::scoped_hook<rvc_fixture>("origStaticIntFromArg",
            [](vmhook::return_value& r) { note_static(); r.cancel(); }) };

        const bool all_installed{
            h_void.installed()  && h_int.installed()  && h_long.installed()  &&
            h_double.installed()&& h_bool.installed() && h_char.installed()  &&
            h_ref.installed()   && h_byte.installed() && h_short.installed() &&
            h_float.installed() && h_str.installed()  && h_arg.installed()   &&
            hs_void.installed() && hs_int.installed() && hs_double.installed() &&
            hs_long.installed() && hs_bool.installed()&& hs_char.installed()  &&
            hs_byte.installed() && hs_short.installed()&& hs_float.installed()&&
            hs_str.installed()  && hs_arg.installed() };
        ctx.check("cancel_all_23_hooks_installed", all_installed);

        // Snapshot the arg-echo side-effect witnesses so the cancel round can
        // assert the arg-taking bodies NEVER ran (echo unchanged).
        const std::int32_t arg_echo_before{ rvc_fixture::arg_echo() };
        const std::int32_t static_arg_echo_before{ rvc_fixture::static_arg_echo() };

        if (run_observe_probe(ctx, "cancel"))
        {
            // Every detour fired exactly once: 12 instance + 11 static.
            ctx.check("cancel_instance_hooks_fired_12", g_inst_fires.load() == 12);
            ctx.check("cancel_static_hooks_fired_11",   g_stat_fires.load() == 11);
            ctx.check("cancel_instance_hooks_saw_self", g_inst_all_saw_self.load());
            ctx.check("cancel_no_java_exception",       !rvc_fixture::saw_exception());

            // VOID: original body SKIPPED — side effect did NOT advance past
            // the baseline snapshot.
            ctx.check("cancel_void_body_skipped",
                      rvc_fixture::side_effect() == side_effect_after_baseline);
            ctx.check("cancel_static_void_body_skipped",
                      rvc_fixture::static_side_effect() == static_side_effect_after_baseline);

            // NON-VOID instance: the documented zero-fill fallback.
            ctx.check("cancel_int_returns_zero",     rvc_fixture::obs_int()  == 0);
            ctx.check("cancel_long_returns_zero",    rvc_fixture::obs_long() == 0);
            ctx.check("cancel_bool_returns_false",   rvc_fixture::obs_bool() == false);
            ctx.check("cancel_char_returns_nul",     rvc_fixture::obs_char() == 0);
            ctx.check("cancel_byte_returns_zero",    rvc_fixture::obs_byte()  == 0);
            ctx.check("cancel_short_returns_zero",   rvc_fixture::obs_short() == 0);
            // double: +0.0 via the movq xmm0 epilogue — NOT NaN, sign bit clear.
            ctx.check("cancel_double_returns_zero",        rvc_fixture::obs_double() == 0.0);
            ctx.check("cancel_double_is_positive_zero",    is_positive_zero(rvc_fixture::obs_double()));
            ctx.check("cancel_double_not_nan",             !rvc_fixture::obs_double_was_nan());
            ctx.check("cancel_double_not_neg_zero",        !rvc_fixture::obs_double_was_neg_zero());
            // float: +0.0f via the SAME xmm0 epilogue read at 32-bit width —
            // NOT NaN, sign bit clear, and bit-exactly 0x00000000.
            ctx.check("cancel_float_returns_zero",         rvc_fixture::obs_float() == 0.0f);
            ctx.check("cancel_float_is_positive_zero",     is_positive_zero(rvc_fixture::obs_float()));
            ctx.check("cancel_float_not_nan",              !rvc_fixture::obs_float_was_nan());
            ctx.check("cancel_float_not_neg_zero",         !rvc_fixture::obs_float_was_neg_zero());
            // reference: null.
            ctx.check("cancel_ref_returns_null",     rvc_fixture::obs_ref_is_null());
            ctx.check("cancel_ref_identity_zero",    rvc_fixture::obs_ref_identity() == 0);
            // String returner: cancel yields null (same zero-oop path as Object),
            // and the breadcrumb length is the null marker (-1).
            ctx.check("cancel_str_returns_null",     rvc_fixture::obs_str_is_null());
            ctx.check("cancel_str_len_is_null_marker", rvc_fixture::obs_str_len() == -1);
            // Arg-taking instance method: cancel suppresses the body, so the
            // return is the zero-fill (0) AND the body's argEcho side effect did
            // NOT happen (still the pre-round sentinel) — the headline invariant.
            ctx.check("cancel_arg_return_zero",      rvc_fixture::obs_arg_return() == 0);
            ctx.check("cancel_arg_body_skipped",     rvc_fixture::arg_echo() == arg_echo_before);

            // NON-VOID static: same zero-fill on the static dispatch path, now
            // proven for every static return descriptor.
            ctx.check("cancel_static_int_returns_zero",    rvc_fixture::obs_static_int() == 0);
            ctx.check("cancel_static_double_returns_zero", rvc_fixture::obs_static_double() == 0.0);
            ctx.check("cancel_static_double_is_positive_zero",
                      is_positive_zero(rvc_fixture::obs_static_double()));
            ctx.check("cancel_static_long_returns_zero",   rvc_fixture::obs_static_long() == 0);
            ctx.check("cancel_static_bool_returns_false",  rvc_fixture::obs_static_bool() == false);
            ctx.check("cancel_static_char_returns_nul",    rvc_fixture::obs_static_char() == 0);
            ctx.check("cancel_static_byte_returns_zero",   rvc_fixture::obs_static_byte()  == 0);
            ctx.check("cancel_static_short_returns_zero",  rvc_fixture::obs_static_short() == 0);
            ctx.check("cancel_static_float_returns_zero",  rvc_fixture::obs_static_float() == 0.0f);
            ctx.check("cancel_static_float_is_positive_zero",
                      is_positive_zero(rvc_fixture::obs_static_float()));
            ctx.check("cancel_static_float_not_neg_zero",
                      !rvc_fixture::obs_static_float_was_neg_zero());
            // static String returner -> null; static arg method -> body skipped,
            // return is the zero-fill, staticArgEcho unchanged.
            ctx.check("cancel_static_str_returns_null", rvc_fixture::obs_static_str_is_null());
            ctx.check("cancel_static_str_len_is_null_marker",
                      rvc_fixture::obs_static_str_len() == -1);
            ctx.check("cancel_static_arg_return_zero", rvc_fixture::obs_static_arg_return() == 0);
            ctx.check("cancel_static_arg_body_skipped",
                      rvc_fixture::static_arg_echo() == static_arg_echo_before);
        }
    }

    // ===================================================================
    // ROUND 2 — ALLOW-THROUGH: hooks installed on the SAME methods but the
    // detour does NOT call cancel() (or set()).  The original body must run
    // and its value flows unchanged.  This isolates cancel() as the cause of
    // round 1's suppression: same hooks, only the cancel() call removed.
    // ===================================================================
    {
        reset_counters();
        const std::int32_t side_effect_before{ rvc_fixture::side_effect() };
        const std::int32_t static_side_effect_before{ rvc_fixture::static_side_effect() };

        auto h_void  { vmhook::scoped_hook<rvc_fixture>("origVoid",
            [](vmhook::return_value&, const std::unique_ptr<rvc_fixture>& self) { note_inst(self); }) };
        auto h_int   { vmhook::scoped_hook<rvc_fixture>("origInt",
            [](vmhook::return_value&, const std::unique_ptr<rvc_fixture>& self) { note_inst(self); }) };
        auto h_double{ vmhook::scoped_hook<rvc_fixture>("origDouble",
            [](vmhook::return_value&, const std::unique_ptr<rvc_fixture>& self) { note_inst(self); }) };
        auto h_ref   { vmhook::scoped_hook<rvc_fixture>("origRef",
            [](vmhook::return_value&, const std::unique_ptr<rvc_fixture>& self) { note_inst(self); }) };
        auto h_str   { vmhook::scoped_hook<rvc_fixture>("origStr",
            [](vmhook::return_value&, const std::unique_ptr<rvc_fixture>& self) { note_inst(self); }) };
        auto h_arg   { vmhook::scoped_hook<rvc_fixture>("origIntFromArg",
            [](vmhook::return_value&, const std::unique_ptr<rvc_fixture>& self) { note_inst(self); }) };
        auto hs_int  { vmhook::scoped_hook<rvc_fixture>("origStaticInt",
            [](vmhook::return_value&) { note_static(); }) };

        const bool all_installed{
            h_void.installed() && h_int.installed() && h_double.installed() &&
            h_ref.installed()  && h_str.installed() && h_arg.installed()    &&
            hs_int.installed() };
        ctx.check("allow_hooks_installed", all_installed);

        if (run_observe_probe(ctx, "allow"))
        {
            ctx.check("allow_instance_hooks_fired", g_inst_fires.load() >= 6);
            ctx.check("allow_static_hook_fired",    g_stat_fires.load() >= 1);
            ctx.check("allow_no_java_exception",    !rvc_fixture::saw_exception());

            // Original bodies ran despite the hook being present.
            ctx.check("allow_void_body_ran",        rvc_fixture::side_effect() == side_effect_before + 7);
            ctx.check("allow_static_void_body_ran", rvc_fixture::static_side_effect() == static_side_effect_before + 13);
            ctx.check("allow_int_is_original",      rvc_fixture::obs_int()  == 1111);
            ctx.check("allow_double_is_original",   rvc_fixture::obs_double() == 11.25);
            ctx.check("allow_ref_not_null",         !rvc_fixture::obs_ref_is_null());
            ctx.check("allow_static_int_is_original", rvc_fixture::obs_static_int() == 2222);
            // String returner flows non-null; arg method echoes + returns the
            // caller's argument (the body ran, no set_arg, no cancel).
            ctx.check("allow_str_not_null",         !rvc_fixture::obs_str_is_null());
            ctx.check("allow_str_len_is_original",  rvc_fixture::obs_str_len() == 10);
            ctx.check("allow_arg_return_is_303",    rvc_fixture::obs_arg_return() == 303);
            ctx.check("allow_arg_echo_is_303",      rvc_fixture::arg_echo() == 303);
        }
    }

    // ===================================================================
    // ROUND 3 — CANCEL + SET, "cancel THEN set" order.  cancel() flips the
    // flag; set() then writes a real value over the (still-cancelled) slot.
    // Documents the "last write wins" contract: the Java caller sees the
    // SET value, not the zero-fill.  (audit standalone case
    // return_value_cancel_then_set_overrides, proven here end-to-end.)
    // ===================================================================
    {
        reset_counters();
        auto h_int   { vmhook::scoped_hook<rvc_fixture>("origInt",
            [](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            { note_inst(self); r.cancel(); r.set(static_cast<std::int32_t>(42)); }) };
        auto h_double{ vmhook::scoped_hook<rvc_fixture>("origDouble",
            [](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            { note_inst(self); r.cancel(); r.set(2.5); }) };
        auto hs_int  { vmhook::scoped_hook<rvc_fixture>("origStaticInt",
            [](vmhook::return_value& r) { note_static(); r.cancel(); r.set(static_cast<std::int32_t>(4242)); }) };

        ctx.check("cancel_then_set_hooks_installed",
                  h_int.installed() && h_double.installed() && hs_int.installed());

        if (run_observe_probe(ctx, "cancel_then_set"))
        {
            ctx.check("cancel_then_set_no_java_exception", !rvc_fixture::saw_exception());
            // set() AFTER cancel() wins: the forced value is delivered.
            ctx.check("cancel_then_set_int_is_42",        rvc_fixture::obs_int()  == 42);
            ctx.check("cancel_then_set_double_is_2_5",    rvc_fixture::obs_double() == 2.5);
            ctx.check("cancel_then_set_static_int_is_4242", rvc_fixture::obs_static_int() == 4242);
        }
    }

    // ===================================================================
    // ROUND 3b — CANCEL + SET with BOUNDARY / SIGN-SENSITIVE values.  The
    // prior round used small positive constants, which a buggy "cancel poisons
    // the slot" path could still pass.  Here set() after cancel() must deliver
    // negative / sign-extended / IEEE-sign / full-64-bit / unsigned-char
    // values intact — proving cancel()'s flag write left retval pristine for
    // the subsequent set() across the WHOLE descriptor range (the
    // sign-extension branch at vmhook.hpp:1169 and the memcpy branch at 1173).
    // ===================================================================
    {
        reset_counters();
        constexpr std::int32_t int_min{ std::numeric_limits<std::int32_t>::min() };
        constexpr std::int64_t long_full{ static_cast<std::int64_t>(0xFEDCBA9876543210ULL) };
        const     double       neg_zero_d{ -0.0 };
        auto h_int   { vmhook::scoped_hook<rvc_fixture>("origInt",
            [int_min](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            { note_inst(self); r.cancel(); r.set(int_min); }) };
        auto h_long  { vmhook::scoped_hook<rvc_fixture>("origLong",
            [long_full](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            { note_inst(self); r.cancel(); r.set(long_full); }) };
        auto h_double{ vmhook::scoped_hook<rvc_fixture>("origDouble",
            [neg_zero_d](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            { note_inst(self); r.cancel(); r.set(neg_zero_d); }) };
        auto h_byte  { vmhook::scoped_hook<rvc_fixture>("origByte",
            [](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            { note_inst(self); r.cancel(); r.set(static_cast<std::int8_t>(-1)); }) };
        auto h_char  { vmhook::scoped_hook<rvc_fixture>("origChar",
            [](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            { note_inst(self); r.cancel(); r.set(static_cast<char16_t>(0xFFFF)); }) };
        auto h_float { vmhook::scoped_hook<rvc_fixture>("origFloat",
            [](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            { note_inst(self); r.cancel(); r.set(-7.5f); }) };

        ctx.check("cancel_then_set_boundary_hooks_installed",
                  h_int.installed() && h_long.installed() && h_double.installed() &&
                  h_byte.installed() && h_char.installed() && h_float.installed());

        if (run_observe_probe(ctx, "cancel_then_set_boundary"))
        {
            ctx.check("cancel_then_set_boundary_no_java_exception", !rvc_fixture::saw_exception());
            // INT_MIN survives: cancel() did NOT pre-zero a cell set() then
            // sign-extends — a positive (zero-extended) result would mean the
            // slot was corrupted between the two writes.
            ctx.check("cancel_then_set_int_is_INT_MIN", rvc_fixture::obs_int() == int_min);
            // Full 64-bit pattern with both dwords non-zero survives the memcpy.
            ctx.check("cancel_then_set_long_is_full64",
                      rvc_fixture::obs_long() == long_full);
            // -0.0 must arrive WITH its sign bit (not the +0.0 zero-fill cancel
            // alone would have produced) — proves set() overwrote the slot.
            ctx.check("cancel_then_set_double_is_neg_zero",
                      same_bits(rvc_fixture::obs_double(), neg_zero_d));
            ctx.check("cancel_then_set_double_neg_zero_flag",
                      rvc_fixture::obs_double_was_neg_zero());
            // byte -1 sign-extends to -1 (not +255).
            ctx.check("cancel_then_set_byte_is_minus_one",
                      rvc_fixture::obs_byte() == static_cast<std::int8_t>(-1));
            // jchar 0xFFFF is unsigned: 65535, not -1.
            ctx.check("cancel_then_set_char_is_FFFF",
                      rvc_fixture::obs_char() == static_cast<std::uint16_t>(0xFFFF));
            // float -7.5 survives at 32-bit width.
            ctx.check("cancel_then_set_float_is_neg_7_5",
                      same_bits(rvc_fixture::obs_float(), -7.5f));
        }
    }

    // ===================================================================
    // ROUND 4 — SET + CANCEL, "set THEN cancel" order.  set() writes the
    // value AND raises cancel; the subsequent cancel() must NOT zero the
    // retval that set() stored.  This is the subtle case: a user who set()s
    // early then conditionally cancel()s expects the value to SURVIVE.
    // (audit standalone case return_value_set_then_cancel_preserves_value.)
    // ===================================================================
    {
        reset_counters();
        auto h_int   { vmhook::scoped_hook<rvc_fixture>("origInt",
            [](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            { note_inst(self); r.set(static_cast<std::int32_t>(77)); r.cancel(); }) };
        auto h_double{ vmhook::scoped_hook<rvc_fixture>("origDouble",
            [](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            { note_inst(self); r.set(-3.5); r.cancel(); }) };
        auto h_long  { vmhook::scoped_hook<rvc_fixture>("origLong",
            [](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            { note_inst(self); r.set(static_cast<std::int64_t>(0x0123456789ABCDEFLL)); r.cancel(); }) };

        ctx.check("set_then_cancel_hooks_installed",
                  h_int.installed() && h_double.installed() && h_long.installed());

        if (run_observe_probe(ctx, "set_then_cancel"))
        {
            ctx.check("set_then_cancel_no_java_exception", !rvc_fixture::saw_exception());
            // cancel() after set() preserves the stored value (does not zero it).
            ctx.check("set_then_cancel_int_preserved",   rvc_fixture::obs_int()  == 77);
            ctx.check("set_then_cancel_double_preserved",rvc_fixture::obs_double() == -3.5);
            ctx.check("set_then_cancel_long_preserved",
                      rvc_fixture::obs_long() == static_cast<std::int64_t>(0x0123456789ABCDEFLL));
        }
    }

    // ===================================================================
    // ROUND 4b — SET + CANCEL with SIGN / IEEE-special survivors.  The
    // strongest form of the "cancel() must not zero what set() stored" rule:
    // every value here is one a "cancel zeroes retval" regression would
    // visibly corrupt — a sign-extended negative byte/short, INT/LONG max,
    // an unsigned jchar 0xFFFF, a -0.0 (sign bit), a +Inf double, and a
    // quiet-NaN-payload float.  Each must arrive bit-for-bit as set() left it,
    // because cancel() only writes the flag, never retval.
    // ===================================================================
    {
        reset_counters();
        constexpr std::int32_t int_max{ std::numeric_limits<std::int32_t>::max() };
        constexpr std::int64_t long_max{ std::numeric_limits<std::int64_t>::max() };
        const     double       pos_inf{ std::numeric_limits<double>::infinity() };
        const     double       neg_zero_d{ -0.0 };
        float nan_payload{};
        {
            const std::uint32_t bits{ 0x7FABCDEFu };
            std::memcpy(&nan_payload, &bits, sizeof(nan_payload));
        }
        auto h_byte  { vmhook::scoped_hook<rvc_fixture>("origByte",
            [](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            { note_inst(self); r.set(static_cast<std::int8_t>(-128)); r.cancel(); }) };
        auto h_short { vmhook::scoped_hook<rvc_fixture>("origShort",
            [](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            { note_inst(self); r.set(static_cast<std::int16_t>(-32768)); r.cancel(); }) };
        auto h_int   { vmhook::scoped_hook<rvc_fixture>("origInt",
            [int_max](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            { note_inst(self); r.set(int_max); r.cancel(); }) };
        auto h_long  { vmhook::scoped_hook<rvc_fixture>("origLong",
            [long_max](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            { note_inst(self); r.set(long_max); r.cancel(); }) };
        auto h_char  { vmhook::scoped_hook<rvc_fixture>("origChar",
            [](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            { note_inst(self); r.set(static_cast<char16_t>(0xFFFF)); r.cancel(); }) };
        auto h_double{ vmhook::scoped_hook<rvc_fixture>("origDouble",
            [neg_zero_d](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            { note_inst(self); r.set(neg_zero_d); r.cancel(); }) };
        auto h_float { vmhook::scoped_hook<rvc_fixture>("origFloat",
            [nan_payload](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            { note_inst(self); r.set(nan_payload); r.cancel(); }) };
        auto hs_double{ vmhook::scoped_hook<rvc_fixture>("origStaticDouble",
            [pos_inf](vmhook::return_value& r) { note_static(); r.set(pos_inf); r.cancel(); }) };

        ctx.check("set_then_cancel_special_hooks_installed",
                  h_byte.installed() && h_short.installed() && h_int.installed() &&
                  h_long.installed() && h_char.installed() && h_double.installed() &&
                  h_float.installed() && hs_double.installed());

        if (run_observe_probe(ctx, "set_then_cancel_special"))
        {
            ctx.check("set_then_cancel_special_no_java_exception", !rvc_fixture::saw_exception());
            ctx.check("set_then_cancel_byte_is_minus_128",
                      rvc_fixture::obs_byte() == static_cast<std::int8_t>(-128));
            ctx.check("set_then_cancel_short_is_INT16_MIN",
                      rvc_fixture::obs_short() == static_cast<std::int16_t>(-32768));
            ctx.check("set_then_cancel_int_is_INT_MAX", rvc_fixture::obs_int() == int_max);
            ctx.check("set_then_cancel_long_is_LONG_MAX", rvc_fixture::obs_long() == long_max);
            ctx.check("set_then_cancel_char_is_FFFF",
                      rvc_fixture::obs_char() == static_cast<std::uint16_t>(0xFFFF));
            // -0.0 survives WITH its sign bit (cancel did not flip it to +0.0).
            ctx.check("set_then_cancel_double_is_neg_zero",
                      same_bits(rvc_fixture::obs_double(), neg_zero_d));
            ctx.check("set_then_cancel_double_neg_zero_flag",
                      rvc_fixture::obs_double_was_neg_zero());
            // float NaN payload survives bit-exact (not zeroed by cancel).
            ctx.check("set_then_cancel_float_nan_payload",
                      same_bits(rvc_fixture::obs_float(), nan_payload));
            ctx.check("set_then_cancel_float_was_nan", rvc_fixture::obs_float_was_nan());
            // static +Inf survives.
            ctx.check("set_then_cancel_static_double_is_pos_inf",
                      same_bits(rvc_fixture::obs_static_double(), pos_inf));
        }
    }

    // ===================================================================
    // ROUND 5 — DOUBLE cancel() in the same detour (idempotent).  Two
    // back-to-back cancel() calls must behave exactly like one: the body is
    // skipped and the caller still sees +0.0 (no accumulated side effect).
    // (audit standalone case return_value_cancel_idempotent, on a live JVM.)
    // ===================================================================
    {
        reset_counters();
        const std::int32_t side_effect_before{ rvc_fixture::side_effect() };
        auto h_void  { vmhook::scoped_hook<rvc_fixture>("origVoid",
            [](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            { note_inst(self); r.cancel(); r.cancel(); }) };
        auto h_double{ vmhook::scoped_hook<rvc_fixture>("origDouble",
            [](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            { note_inst(self); r.cancel(); r.cancel(); }) };

        ctx.check("double_cancel_hooks_installed", h_void.installed() && h_double.installed());

        if (run_observe_probe(ctx, "double_cancel"))
        {
            ctx.check("double_cancel_no_java_exception", !rvc_fixture::saw_exception());
            ctx.check("double_cancel_void_body_skipped", rvc_fixture::side_effect() == side_effect_before);
            ctx.check("double_cancel_double_returns_zero",     rvc_fixture::obs_double() == 0.0);
            ctx.check("double_cancel_double_is_positive_zero", is_positive_zero(rvc_fixture::obs_double()));
        }
    }

    // ===================================================================
    // ROUND 5b — N-times cancel() (idempotent for N>2).  Five back-to-back
    // cancel() calls must behave exactly like one — the flag is a plain bool,
    // so re-setting it true any number of times is a no-op.  Guards against a
    // hypothetical "toggle" or "counter" mis-implementation that double-cancel
    // (N==2) alone could mask (an even count would un-cancel).
    // ===================================================================
    {
        reset_counters();
        const std::int32_t side_effect_before{ rvc_fixture::side_effect() };
        auto h_void  { vmhook::scoped_hook<rvc_fixture>("origVoid",
            [](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            { note_inst(self); r.cancel(); r.cancel(); r.cancel(); r.cancel(); r.cancel(); }) };
        auto h_int   { vmhook::scoped_hook<rvc_fixture>("origInt",
            [](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            { note_inst(self); r.cancel(); r.cancel(); r.cancel(); r.cancel(); r.cancel(); }) };

        ctx.check("ntimes_cancel_hooks_installed", h_void.installed() && h_int.installed());

        if (run_observe_probe(ctx, "ntimes_cancel"))
        {
            ctx.check("ntimes_cancel_no_java_exception", !rvc_fixture::saw_exception());
            ctx.check("ntimes_cancel_void_body_skipped", rvc_fixture::side_effect() == side_effect_before);
            ctx.check("ntimes_cancel_int_returns_zero",  rvc_fixture::obs_int() == 0);
        }
    }

    // ===================================================================
    // ROUND 5c — CANCEL, SET, CANCEL (triple interleave).  set() in the middle
    // stores a real value; a trailing cancel() must NOT re-zero it, because
    // cancel() only writes the flag (vmhook.hpp:1205-1209), never retval.  This
    // is the union of cancel-then-set (set wins) and set-then-cancel (value
    // survives): the value set() stored between two cancels must STILL be the
    // observed result.
    // ===================================================================
    {
        reset_counters();
        auto h_int   { vmhook::scoped_hook<rvc_fixture>("origInt",
            [](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            { note_inst(self); r.cancel(); r.set(static_cast<std::int32_t>(909)); r.cancel(); }) };
        auto h_double{ vmhook::scoped_hook<rvc_fixture>("origDouble",
            [](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            { note_inst(self); r.cancel(); r.set(6.25); r.cancel(); }) };

        ctx.check("cancel_set_cancel_hooks_installed",
                  h_int.installed() && h_double.installed());

        if (run_observe_probe(ctx, "cancel_set_cancel"))
        {
            ctx.check("cancel_set_cancel_no_java_exception", !rvc_fixture::saw_exception());
            ctx.check("cancel_set_cancel_int_is_909",     rvc_fixture::obs_int() == 909);
            ctx.check("cancel_set_cancel_double_is_6_25", rvc_fixture::obs_double() == 6.25);
        }
    }

    // ===================================================================
    // ROUND 5d — MIXED FATE in ONE probe: different methods take different
    // paths simultaneously (some cancel, some set, some allow-through).  Each
    // dispatch gets its OWN trampoline-allocated return_slot (vmhook.hpp:8126),
    // so the decisions must not leak between methods within a single probe:
    //   origInt    -> cancel()         => 0
    //   origLong   -> set(0x00000000ABCDEF01) => preserved (no cross-talk)
    //   origDouble -> allow-through     => original 11.25
    //   origVoid   -> cancel()          => side effect skipped
    //   origStaticVoid -> allow-through => static side effect advances
    // ===================================================================
    {
        reset_counters();
        constexpr std::int64_t long_set{ static_cast<std::int64_t>(0x00000000ABCDEF01LL) };
        const std::int32_t side_effect_before{ rvc_fixture::side_effect() };
        const std::int32_t static_side_effect_before{ rvc_fixture::static_side_effect() };

        auto h_int   { vmhook::scoped_hook<rvc_fixture>("origInt",
            [](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            { note_inst(self); r.cancel(); }) };
        auto h_long  { vmhook::scoped_hook<rvc_fixture>("origLong",
            [long_set](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            { note_inst(self); r.set(long_set); }) };
        auto h_double{ vmhook::scoped_hook<rvc_fixture>("origDouble",
            [](vmhook::return_value&, const std::unique_ptr<rvc_fixture>& self)
            { note_inst(self); /* allow-through */ }) };
        auto h_void  { vmhook::scoped_hook<rvc_fixture>("origVoid",
            [](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            { note_inst(self); r.cancel(); }) };
        auto hs_void { vmhook::scoped_hook<rvc_fixture>("origStaticVoid",
            [](vmhook::return_value&) { note_static(); /* allow-through */ }) };

        ctx.check("mixed_fate_hooks_installed",
                  h_int.installed() && h_long.installed() && h_double.installed() &&
                  h_void.installed() && hs_void.installed());

        if (run_observe_probe(ctx, "mixed_fate"))
        {
            ctx.check("mixed_fate_no_java_exception", !rvc_fixture::saw_exception());
            // Cancelled int -> 0.
            ctx.check("mixed_fate_int_cancelled",  rvc_fixture::obs_int() == 0);
            // set() long survives untouched by the neighbouring cancels.
            ctx.check("mixed_fate_long_set",       rvc_fixture::obs_long() == long_set);
            // Allow-through double -> original 11.25 (NOT zeroed by the int cancel).
            ctx.check("mixed_fate_double_original", rvc_fixture::obs_double() == 11.25);
            // Cancelled void -> side effect did NOT advance.
            ctx.check("mixed_fate_void_skipped",
                      rvc_fixture::side_effect() == side_effect_before);
            // Allow-through static void -> side effect DID advance.
            ctx.check("mixed_fate_static_void_ran",
                      rvc_fixture::static_side_effect() == static_side_effect_before + 13);
        }
    }

    // ===================================================================
    // ROUND 5e — TYPED-NULL overload as a cancel path on a reference returner.
    // set<wrapper_type>(nullptr) (vmhook.hpp:1402-1409, the object_base-only
    // overload) raises cancel AND zeroes retval — a documentation-bearing twin
    // of plain cancel() for the reference case.  The Java caller must observe
    // null exactly as a bare cancel() would, proving the requires-constrained
    // overload routes through the same zero-fill, not the primitive set() path.
    // ===================================================================
    {
        reset_counters();
        auto h_ref{ vmhook::scoped_hook<rvc_fixture>("origRef",
            [](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            { note_inst(self); r.set<rvc_fixture>(nullptr); }) };

        ctx.check("typed_null_hook_installed", h_ref.installed());

        if (run_observe_probe(ctx, "typed_null"))
        {
            ctx.check("typed_null_no_java_exception", !rvc_fixture::saw_exception());
            ctx.check("typed_null_ref_is_null",       rvc_fixture::obs_ref_is_null());
            ctx.check("typed_null_ref_identity_zero", rvc_fixture::obs_ref_identity() == 0);
        }
    }

    // ===================================================================
    // ROUND 5f — SET_ARG + CANCEL (orthogonal mutations).  The detour mutates
    // the method's incoming int argument via set_arg(1, ...) AND cancels.  cancel
    // suppresses the body, so the body NEVER reads the mutated arg: argEcho stays
    // at its pre-round sentinel and the Java caller receives the zero-fill return
    // (0), NOT the injected argument.  This proves set_arg writes the locals array
    // while cancel independently skips the body that would consume it — the two
    // operations touch different state (interpreter locals vs the return slot's
    // cancel flag) and do not interfere.
    // ===================================================================
    {
        reset_counters();
        const std::int32_t arg_echo_before{ rvc_fixture::arg_echo() };
        const std::int32_t static_arg_echo_before{ rvc_fixture::static_arg_echo() };
        std::atomic<bool> inst_set_arg_ok{ false };
        std::atomic<bool> stat_set_arg_ok{ false };

        // instance: slot 0 = this, slot 1 = first arg.
        auto h_arg{ vmhook::scoped_hook<rvc_fixture>("origIntFromArg",
            [&inst_set_arg_ok](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            {
                note_inst(self);
                inst_set_arg_ok.store(r.set_arg(1, static_cast<std::int32_t>(555)),
                                      std::memory_order_relaxed);
                r.cancel();
            }) };
        // static: slot 0 = first arg (no this).
        auto hs_arg{ vmhook::scoped_hook<rvc_fixture>("origStaticIntFromArg",
            [&stat_set_arg_ok](vmhook::return_value& r)
            {
                note_static();
                stat_set_arg_ok.store(r.set_arg(0, static_cast<std::int32_t>(666)),
                                      std::memory_order_relaxed);
                r.cancel();
            }) };

        ctx.check("set_arg_cancel_hooks_installed", h_arg.installed() && hs_arg.installed());

        if (run_observe_probe(ctx, "set_arg_cancel"))
        {
            ctx.check("set_arg_cancel_no_java_exception", !rvc_fixture::saw_exception());
            // set_arg accepted the in-range slot write.
            ctx.check("set_arg_cancel_inst_set_arg_accepted", inst_set_arg_ok.load());
            ctx.check("set_arg_cancel_stat_set_arg_accepted", stat_set_arg_ok.load());
            // Body suppressed: caller gets the zero-fill, NOT 555 / 666.
            ctx.check("set_arg_cancel_inst_return_zero",  rvc_fixture::obs_arg_return() == 0);
            ctx.check("set_arg_cancel_stat_return_zero",  rvc_fixture::obs_static_arg_return() == 0);
            // Body never ran: argEcho unchanged (proves cancel skipped the body
            // that would have consumed the mutated argument).
            ctx.check("set_arg_cancel_inst_body_skipped",
                      rvc_fixture::arg_echo() == arg_echo_before);
            ctx.check("set_arg_cancel_stat_body_skipped",
                      rvc_fixture::static_arg_echo() == static_arg_echo_before);
        }
    }

    // ===================================================================
    // ROUND 5g — SET_ARG-ONLY control (allow-through) on the SAME arg methods.
    // No cancel: the body RUNS and observes the set_arg mutation, so argEcho and
    // the returned value both become the INJECTED argument (not the caller's 303 /
    // 404).  This is the indispensable control for ROUND 5f: it proves set_arg
    // genuinely reaches the interpreter locals on these methods, so 5f's "return
    // is zero, echo unchanged" is caused by cancel suppressing the body — not by
    // set_arg silently failing.
    // ===================================================================
    {
        reset_counters();
        std::atomic<bool> inst_set_arg_ok{ false };
        std::atomic<bool> stat_set_arg_ok{ false };

        auto h_arg{ vmhook::scoped_hook<rvc_fixture>("origIntFromArg",
            [&inst_set_arg_ok](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            {
                note_inst(self);
                inst_set_arg_ok.store(r.set_arg(1, static_cast<std::int32_t>(555)),
                                      std::memory_order_relaxed);
                // no cancel -> body runs and consumes the replacement
            }) };
        auto hs_arg{ vmhook::scoped_hook<rvc_fixture>("origStaticIntFromArg",
            [&stat_set_arg_ok](vmhook::return_value& r)
            {
                note_static();
                stat_set_arg_ok.store(r.set_arg(0, static_cast<std::int32_t>(666)),
                                      std::memory_order_relaxed);
            }) };

        ctx.check("set_arg_only_hooks_installed", h_arg.installed() && hs_arg.installed());

        if (run_observe_probe(ctx, "set_arg_only"))
        {
            ctx.check("set_arg_only_no_java_exception", !rvc_fixture::saw_exception());
            ctx.check("set_arg_only_inst_accepted", inst_set_arg_ok.load());
            ctx.check("set_arg_only_stat_accepted", stat_set_arg_ok.load());
            // Body ran on the REPLACED argument: echo + return are the injected
            // value, not the caller's 303 / 404.
            ctx.check("set_arg_only_inst_echo_is_555",   rvc_fixture::arg_echo() == 555);
            ctx.check("set_arg_only_inst_return_is_555", rvc_fixture::obs_arg_return() == 555);
            ctx.check("set_arg_only_stat_echo_is_666",   rvc_fixture::static_arg_echo() == 666);
            ctx.check("set_arg_only_stat_return_is_666", rvc_fixture::obs_static_arg_return() == 666);
        }
    }

    // ===================================================================
    // ROUND 5h — SET_ARG + SET (force a DIFFERENT return than the arg).  The
    // detour mutates the argument AND forces a return value with set() (no
    // cancel).  Because set() delivers its own value, the caller sees the FORCED
    // return (7000), independent of the mutated arg — but the body still runs, so
    // argEcho proves the set_arg mutation landed (555).  Separates the two return
    // overrides on a method whose natural return is its argument: set_arg feeds
    // the body, set() overrides what the caller ultimately observes.
    // ===================================================================
    {
        reset_counters();
        std::atomic<bool> inst_set_arg_ok{ false };

        auto h_arg{ vmhook::scoped_hook<rvc_fixture>("origIntFromArg",
            [&inst_set_arg_ok](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            {
                note_inst(self);
                inst_set_arg_ok.store(r.set_arg(1, static_cast<std::int32_t>(555)),
                                      std::memory_order_relaxed);
                r.set(static_cast<std::int32_t>(7000));
            }) };

        ctx.check("set_arg_and_set_hook_installed", h_arg.installed());

        if (run_observe_probe(ctx, "set_arg_and_set"))
        {
            ctx.check("set_arg_and_set_no_java_exception", !rvc_fixture::saw_exception());
            ctx.check("set_arg_and_set_accepted", inst_set_arg_ok.load());
            // Body ran on the replacement arg (echo proves set_arg landed)...
            ctx.check("set_arg_and_set_echo_is_555", rvc_fixture::arg_echo() == 555);
            // ...but set() overrides the OBSERVED return to 7000, not 555.
            ctx.check("set_arg_and_set_return_is_7000", rvc_fixture::obs_arg_return() == 7000);
        }
    }

    // ===================================================================
    // ROUND 6 — PER-INVOCATION cancel state.  A single hook on origVoid()
    // cancels ONLY the first call of each probe and lets the second through.
    // The probe (MODE_VOID_TWICE) calls origVoid() twice and snapshots the
    // side-effect counter after each.  This proves the cancel flag lives in
    // the per-call trampoline-allocated return_slot on the native stack and
    // does NOT stick across invocations.  (audit case
    // test_cancel_then_original_runs_on_next_call.)
    // ===================================================================
    {
        reset_counters();
        std::atomic<int> call_index{ 0 };
        const std::int32_t side_effect_before{ rvc_fixture::side_effect() };

        auto h_void{ vmhook::scoped_hook<rvc_fixture>("origVoid",
            [&call_index](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            {
                note_inst(self);
                const int idx{ call_index.fetch_add(1, std::memory_order_relaxed) };
                if (idx == 0)
                {
                    r.cancel();   // suppress the FIRST call only
                }
                // second call: do nothing -> original body runs
            }) };
        ctx.check("per_invocation_hook_installed", h_void.installed());

        rvc_fixture::set_mode(mode_consts::void_twice);
        rvc_fixture::set_done(false);
        const bool done{ ctx.run_probe(
            [](bool value) { rvc_fixture::set_go(value); },
            []() { return rvc_fixture::get_done(); }) };
        ctx.check("per_invocation_probe_completed", done);

        if (done)
        {
            ctx.check("per_invocation_no_java_exception", !rvc_fixture::saw_exception());
            ctx.check("per_invocation_hook_fired_twice",  g_inst_fires.load() == 2);
            // 1st call cancelled -> counter unchanged after call 1.
            ctx.check("per_invocation_call1_cancelled",
                      rvc_fixture::side_effect_after_call1() == side_effect_before);
            // 2nd call allowed -> counter advanced by 7 after call 2.
            ctx.check("per_invocation_call2_ran",
                      rvc_fixture::side_effect_after_call2() == side_effect_before + 7);
            // Net effect: exactly ONE body executed across the two calls.
            ctx.check("per_invocation_net_single_body",
                      rvc_fixture::side_effect() == side_effect_before + 7);
        }
    }

    // ===================================================================
    // ROUND 6b — PER-INVOCATION cancel state, INVERTED.  The mirror of ROUND 6:
    // allow the FIRST call through and cancel the SECOND.  Together the two
    // rounds prove the per-call slot resets in BOTH directions — a hook that
    // "latched" cancel on after the first use would pass ROUND 6 (cancel-first)
    // yet fail here (the first body must RUN, the second must be SKIPPED).
    // ===================================================================
    {
        reset_counters();
        std::atomic<int> call_index{ 0 };
        const std::int32_t side_effect_before{ rvc_fixture::side_effect() };

        auto h_void{ vmhook::scoped_hook<rvc_fixture>("origVoid",
            [&call_index](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            {
                note_inst(self);
                const int idx{ call_index.fetch_add(1, std::memory_order_relaxed) };
                if (idx == 1)
                {
                    r.cancel();   // suppress the SECOND call only
                }
                // first call: do nothing -> original body runs
            }) };
        ctx.check("per_invocation_inv_hook_installed", h_void.installed());

        rvc_fixture::set_mode(mode_consts::void_twice);
        rvc_fixture::set_done(false);
        const bool done{ ctx.run_probe(
            [](bool value) { rvc_fixture::set_go(value); },
            []() { return rvc_fixture::get_done(); }) };
        ctx.check("per_invocation_inv_probe_completed", done);

        if (done)
        {
            ctx.check("per_invocation_inv_no_java_exception", !rvc_fixture::saw_exception());
            ctx.check("per_invocation_inv_hook_fired_twice",  g_inst_fires.load() == 2);
            // 1st call allowed -> counter advanced by 7 after call 1.
            ctx.check("per_invocation_inv_call1_ran",
                      rvc_fixture::side_effect_after_call1() == side_effect_before + 7);
            // 2nd call cancelled -> counter UNCHANGED between call 1 and call 2.
            ctx.check("per_invocation_inv_call2_cancelled",
                      rvc_fixture::side_effect_after_call2() == side_effect_before + 7);
            // Net effect: exactly ONE body executed (the first).
            ctx.check("per_invocation_inv_net_single_body",
                      rvc_fixture::side_effect() == side_effect_before + 7);
        }
    }

    // ===================================================================
    // ROUND 6c — PER-INVOCATION across THREE calls (MODE_VOID_THRICE).  Cancel
    // the MIDDLE call only (idx==1) and let calls 0 and 2 run.  Snapshots after
    // each of the three dispatches must show: run, skip, run — i.e. the counter
    // advances after call 0, holds after call 1, advances again after call 2,
    // for a net of TWO bodies.  Extends the per-call-slot proof beyond two
    // invocations and pins the suppression to exactly the middle one.
    // ===================================================================
    {
        reset_counters();
        std::atomic<int> call_index{ 0 };
        const std::int32_t side_effect_before{ rvc_fixture::side_effect() };

        auto h_void{ vmhook::scoped_hook<rvc_fixture>("origVoid",
            [&call_index](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            {
                note_inst(self);
                const int idx{ call_index.fetch_add(1, std::memory_order_relaxed) };
                if (idx == 1)
                {
                    r.cancel();   // suppress ONLY the middle call
                }
            }) };
        ctx.check("per_invocation_thrice_hook_installed", h_void.installed());

        rvc_fixture::set_mode(mode_consts::void_thrice);
        rvc_fixture::set_done(false);
        const bool done{ ctx.run_probe(
            [](bool value) { rvc_fixture::set_go(value); },
            []() { return rvc_fixture::get_done(); }) };
        ctx.check("per_invocation_thrice_probe_completed", done);

        if (done)
        {
            ctx.check("per_invocation_thrice_no_java_exception", !rvc_fixture::saw_exception());
            ctx.check("per_invocation_thrice_hook_fired_3", g_inst_fires.load() == 3);
            // call 0 ran -> +7.
            ctx.check("per_invocation_thrice_call1_ran",
                      rvc_fixture::side_effect_after_call1() == side_effect_before + 7);
            // call 1 cancelled -> holds at +7.
            ctx.check("per_invocation_thrice_call2_cancelled",
                      rvc_fixture::side_effect_after_call2() == side_effect_before + 7);
            // call 2 ran -> +14.
            ctx.check("per_invocation_thrice_call3_ran",
                      rvc_fixture::side_effect_after_call3() == side_effect_before + 14);
            // Net: exactly TWO bodies executed.
            ctx.check("per_invocation_thrice_net_two_bodies",
                      rvc_fixture::side_effect() == side_effect_before + 14);
        }
    }

    // ===================================================================
    // ROUND 6d — CANCEL re-fire on the SAME installed hook across MULTIPLE
    // probes (no teardown between them).  The other rounds re-install fresh
    // scoped_hooks each round; this one installs ONE cancel hook and drives
    // THREE consecutive OBSERVE_ALL probes through it, asserting every probe
    // delivers the cancel result.  Proves the cancel path is stable when the
    // SAME detour fires repeatedly without being re-armed (the slot is fresh
    // per dispatch, the detour pointer is reused).
    // ===================================================================
    {
        reset_counters();
        auto h_int{ vmhook::scoped_hook<rvc_fixture>("origInt",
            [](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            { note_inst(self); r.cancel(); }) };
        auto h_void{ vmhook::scoped_hook<rvc_fixture>("origVoid",
            [](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            { note_inst(self); r.cancel(); }) };
        ctx.check("refire_hooks_installed", h_int.installed() && h_void.installed());

        bool all_probes_cancelled{ true };
        bool all_probes_completed{ true };
        for (int probe{ 0 }; probe < 3; ++probe)
        {
            const std::int32_t side_effect_before{ rvc_fixture::side_effect() };
            rvc_fixture::set_mode(mode_consts::observe_all);
            rvc_fixture::set_done(false);
            const bool done{ ctx.run_probe(
                [](bool value) { rvc_fixture::set_go(value); },
                []() { return rvc_fixture::get_done(); }) };
            if (!done)
            {
                all_probes_completed = false;
                break;
            }
            if (rvc_fixture::obs_int() != 0
                || rvc_fixture::side_effect() != side_effect_before
                || rvc_fixture::saw_exception())
            {
                all_probes_cancelled = false;
            }
        }
        ctx.check("refire_all_probes_completed", all_probes_completed);
        ctx.check("refire_all_probes_cancelled", all_probes_cancelled);
        // The int detour fired once per probe (3) + the void detour once per
        // probe (3) == 6 total instance fires across the three probes.
        ctx.check("refire_total_instance_fires_6", g_inst_fires.load() == 6);
    }

    // ===================================================================
    // ROUND 7 — STABILITY: re-run the canonical cancel-only round a SECOND
    // time at the very end.  Each round installs fresh scoped_hooks and tears
    // them down at block exit; this guards against state left behind by a
    // previous round's arm/disarm cycle, proving cancel() is repeatable.
    // ===================================================================
    {
        reset_counters();
        const std::int32_t side_effect_before{ rvc_fixture::side_effect() };
        auto h_void  { vmhook::scoped_hook<rvc_fixture>("origVoid",
            [](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            { note_inst(self); r.cancel(); }) };
        auto h_int   { vmhook::scoped_hook<rvc_fixture>("origInt",
            [](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            { note_inst(self); r.cancel(); }) };
        auto h_ref   { vmhook::scoped_hook<rvc_fixture>("origRef",
            [](vmhook::return_value& r, const std::unique_ptr<rvc_fixture>& self)
            { note_inst(self); r.cancel(); }) };

        ctx.check("repeat_cancel_hooks_installed",
                  h_void.installed() && h_int.installed() && h_ref.installed());

        if (run_observe_probe(ctx, "repeat_cancel"))
        {
            ctx.check("repeat_cancel_no_java_exception", !rvc_fixture::saw_exception());
            ctx.check("repeat_cancel_void_body_skipped", rvc_fixture::side_effect() == side_effect_before);
            ctx.check("repeat_cancel_int_returns_zero",  rvc_fixture::obs_int() == 0);
            ctx.check("repeat_cancel_ref_returns_null",  rvc_fixture::obs_ref_is_null());
        }
    }

    // ===================================================================
    // ROUND 8 — FINAL ALLOW-THROUGH: with all hooks now disarmed (every
    // scoped_hook above went out of scope), one more bare probe proves the
    // teardown was clean — original values flow again, exactly like the
    // baseline.  Closes the arm/cancel/disarm lifecycle.
    // ===================================================================
    {
        reset_counters();
        rvc_fixture::set_mode(mode_consts::observe_all);
        rvc_fixture::set_done(false);
        const bool done{ ctx.run_probe(
            [](bool value) { rvc_fixture::set_go(value); },
            []() { return rvc_fixture::get_done(); }) };
        ctx.check("final_baseline_probe_completed", done);
        if (done)
        {
            ctx.check("final_baseline_no_hook_fired",
                      g_inst_fires.load() == 0 && g_stat_fires.load() == 0);
            ctx.check("final_baseline_int_is_1111",  rvc_fixture::obs_int()  == 1111);
            ctx.check("final_baseline_double_is_11_25", rvc_fixture::obs_double() == 11.25);
            ctx.check("final_baseline_ref_not_null", !rvc_fixture::obs_ref_is_null());
            // New return descriptors also flow on clean teardown.
            ctx.check("final_baseline_byte_is_88",   rvc_fixture::obs_byte()  == static_cast<std::int8_t>(88));
            ctx.check("final_baseline_short_is_7777",rvc_fixture::obs_short() == static_cast<std::int16_t>(7777));
            ctx.check("final_baseline_float_is_33_5",rvc_fixture::obs_float() == 33.5f);
            ctx.check("final_baseline_static_long_is_orig",
                      rvc_fixture::obs_static_long() == static_cast<std::int64_t>(0x7FFFFFFF00000002LL));
            ctx.check("final_baseline_static_float_is_44_5",
                      rvc_fixture::obs_static_float() == 44.5f);
            // String + arg-taking methods also flow original on clean teardown.
            ctx.check("final_baseline_str_not_null", !rvc_fixture::obs_str_is_null());
            ctx.check("final_baseline_str_len_is_orig", rvc_fixture::obs_str_len() == 10);
            ctx.check("final_baseline_arg_return_is_303", rvc_fixture::obs_arg_return() == 303);
            ctx.check("final_baseline_arg_echo_is_303",   rvc_fixture::arg_echo() == 303);
            ctx.check("final_baseline_static_arg_return_is_404",
                      rvc_fixture::obs_static_arg_return() == 404);
        }
    }

    ctx.record("[INFO] return_value_cancel: proved cancel() skips the original "
               "body on void (instance+static) and forces 0/0L/+0.0/+0.0f/false/U+0000/null "
               "on int/long/double/float/bool/char/byte/short/reference returns (instance+static, "
               "every primitive descriptor); float cancel proven +0.0f (not NaN/-0.0f) via the "
               "32-bit xmm0 read alongside the double +0.0; verified allow-through vs cancel, "
               "cancel+set both orders (canonical AND boundary: INT_MIN/INT_MAX/LONG full-64/-0.0/"
               "NaN-payload/byte-1/char-0xFFFF survive set across cancel), cancel-set-cancel triple "
               "interleave, mixed-fate (cancel/set/allow in one probe — no cross-slot leak), the "
               "String returner cancel -> null (instance+static), cancel+set_arg (arg mutated but "
               "body suppressed -> zero-fill return, echo unchanged) vs set_arg-only control (body "
               "runs on the injected arg) vs set_arg+set (set() overrides the observed return), "
               "typed-null set<wrapper>(nullptr) overload, N-times idempotent cancel, per-invocation "
               "cancel state across 2 calls in BOTH directions and across 3 calls (suppress middle), "
               "cancel re-fire across 3 probes on one installed hook, and a clean arm/disarm lifecycle "
               "bracketed by no-hook baselines.");

    // [INFO] cancel() itself is interpreter-only: the force-CANCEL epilogue
    // lives ONLY in the i2i trampoline (vmhook.hpp:5414-5461 Win64 / 5512-5562
    // SysV) and is reached only when the hooked method dispatches through the
    // interpreter.  The module's low call counts keep every target interpreted
    // (NO_COMPILE holds), so it does not — and structurally cannot from native
    // code — exercise a tier-up-then-cancel race; a JIT'd/inlined caller would
    // bypass the i2i patch and make cancel() a silent no-op.  This is a
    // documented scope limit, not a tested behaviour.
    ctx.record("[INFO] return_value_cancel: the cancel epilogue is interpreter-only "
               "(i2i trampoline); these checks keep every target interpreted, so a "
               "tier-up-then-cancel race is out of scope by construction.");
}
