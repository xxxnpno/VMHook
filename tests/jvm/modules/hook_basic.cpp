// hook_basic JVM test module  (feature area: hooks)
//
// Exhaustively exercises vmhook::hook<T> / scoped_hook installed on an INSTANCE
// method and a STATIC method, proving on a live JVM (real bytecode dispatch via
// the Harness go/done probe) that:
//   * the detour fires EXACTLY ONCE per Java call (N Java dispatches -> N fires),
//   * it sees the correct receiver `self` (verified by reading the instance's
//     own `seed`, and by hooking two DIFFERENT instances with distinct seeds),
//   * it decodes every argument correctly across primitive widths and across the
//     J/D two-slot boundary (int, long, double, boolean, String),
//   * the ORIGINAL method body still runs after a non-cancelling detour
//     (allow-through): Java observes the unmodified results,
//   * scoped_hook installs (handle.installed()) and uninstalls on scope exit
//     (after the handle drops, the detour no longer fires).
//
// Harness note: the fixture's `done` flag LATCHES (run_java_probe never clears
// it).  So each scenario resets `done` to false and sets `mode` on the rising
// edge of `go`, runs ONE probe cycle, then reads back the recorded observations.
//
// ---------------------------------------------------------------------------
// JIT-RELIABILITY HARDENING (why this module deopts before each asserting drive)
// ---------------------------------------------------------------------------
// hook_basic installs an i2i INTERPRETER detour on a method and then asserts the
// detour fired on a real dispatch.  The detour only fires when the method is
// dispatched through the INTERPRETER (so the patched i2i stub is reached); a
// JIT-compiled (i2c/nmethod) dispatch BYPASSES the i2i patch and the detour
// never fires.  On the fast tiered JIT of JDK 24/25/26 — and now that the modular
// suite is ~40% larger (waves 1-3), so HookBasic.touch can already be JIT-warm
// from cumulative suite activity BEFORE this module even runs — the hooked method
// may be compiled at install time (or get asynchronously recompiled in the
// microsecond window between install and the asserting drive).  That intermittently
// fails the instance detour's HARD firing checks on linux/gcc/java24/25.
//
// The established fix (the same one hook_install_after_jit.cpp + hook_verify_repair.cpp
// rely on) is to DEOPTIMIZE the target method after install so execution returns
// to the interpreter and the i2i detour is taken.  We do this with a BOUNDED
// settle loop (mirroring code_settles_null / verify_settles_zero): deoptimize the
// fixture's methods + verify_hooks() until the live Method's interpreter entry is
// observed routing through the i2i stub (_from_interpreted_entry == _i2i_entry),
// which is the reliable indicator that the next interpreted dispatch reaches the
// patch.  vmhook holds NO_COMPILE on the hooked Method (set at install), so once
// the route is established it stays put for the single Java probe that follows.
//
// The asserting drives go through drive_until_fires(): it re-settles the route and
// re-drives the probe up to a small budget so a recompile that races between the
// settle and the START of run() cannot slip a call past the detour, then the
// per-scenario checks remain HARD on the final observations.  The retry only
// EXISTS to achieve the firing deterministically; a genuine failure to fire after
// the whole budget still fails the HARD assertion (a real regression).  The core
// firing checks are NOT downgraded to [INFO] — hook_basic is the foundational
// hook test and its firing guarantees stay hard on every JDK 8-26.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

namespace
{
    // Wrapper for vmhook.fixtures.HookBasic.  Deriving from vmhook::object<>
    // gives the wrapper a vtable (required by register_class<T>) and the
    // static_field(...) / get_field(...) accessors used below.
    class hook_basic_fixture : public vmhook::object<hook_basic_fixture>
    {
    public:
        explicit hook_basic_fixture(vmhook::oop_t instance) noexcept
            : vmhook::object<hook_basic_fixture>{ instance }
        {
        }

        // --- go/done handshake + scenario selector ------------------------
        static auto set_go(bool value) -> void   { static_field("go")->set(value); }
        static auto set_done(bool value) -> void  { static_field("done")->set(value); }
        static auto get_done() -> bool            { return static_field("done")->get(); }
        static auto set_mode(std::int32_t m) -> void { static_field("mode")->set(m); }

        // --- recorded observations the Java side writes -------------------
        static auto get_last_touch_result() -> std::int32_t  { return static_field("lastTouchResult")->get(); }
        static auto get_touch_result_sum() -> std::int64_t   { return static_field("touchResultSum")->get(); }
        static auto get_instance_calls_made() -> std::int32_t{ return static_field("instanceCallsMade")->get(); }
        static auto get_static_calls_made() -> std::int32_t  { return static_field("staticCallsMade")->get(); }
        static auto get_combine_result() -> std::int64_t     { return static_field("combineResult")->get(); }
        static auto get_static_combine_result() -> std::int64_t { return static_field("staticCombineResult")->get(); }
        static auto get_wide_result() -> double              { return static_field("wideResult")->get(); }
        static auto get_instance_a_seed() -> std::int32_t    { return static_field("instanceASeed")->get(); }
        static auto get_instance_b_seed() -> std::int32_t    { return static_field("instanceBSeed")->get(); }
        static auto get_two_instance_result_a() -> std::int32_t { return static_field("twoInstanceResultA")->get(); }
        static auto get_two_instance_result_b() -> std::int32_t { return static_field("twoInstanceResultB")->get(); }

        // --- return-value interception observations (modes 8-20) ----------
        static auto get_ret_int_observed() -> std::int32_t     { return static_field("retIntObserved")->get(); }
        static auto get_ret_long_observed() -> std::int64_t    { return static_field("retLongObserved")->get(); }
        static auto get_ret_double_observed() -> double        { return static_field("retDoubleObserved")->get(); }
        static auto get_ret_float_observed() -> float          { return static_field("retFloatObserved")->get(); }
        static auto get_ret_boolean_observed() -> bool         { return static_field("retBooleanObserved")->get(); }
        static auto get_ret_byte_observed() -> std::int8_t     { return static_field("retByteObserved")->get(); }
        static auto get_ret_short_observed() -> std::int16_t   { return static_field("retShortObserved")->get(); }
        static auto get_ret_char_observed() -> std::uint16_t   { return static_field("retCharObserved")->get(); }
        static auto get_name_was_null() -> bool                { return static_field("nameWasNull")->get(); }
        static auto get_mutated_touch_result() -> std::int32_t { return static_field("mutatedTouchResult")->get(); }
        static auto get_reinstall_touch_result() -> std::int32_t { return static_field("reinstallTouchResult")->get(); }

        // --- deepening (batch-16) observations ----------------------------
        static auto get_many_args_result() -> std::int64_t        { return static_field("manyArgsResult")->get(); }
        static auto get_static_many_args_result() -> std::int64_t { return static_field("staticManyArgsResult")->get(); }
        static auto get_boundary_combine_result() -> std::int64_t { return static_field("boundaryCombineResult")->get(); }
        static auto get_float_arg_result() -> float               { return static_field("floatArgResult")->get(); }
        static auto get_null_ref_args_both_null() -> bool         { return static_field("nullRefArgsBothNull")->get(); }
        static auto get_obj_arg_result() -> std::int32_t          { return static_field("objArgResult")->get(); }
        static auto get_coexist_instance_result() -> std::int32_t { return static_field("coexistInstanceResult")->get(); }
        static auto get_coexist_static_result() -> std::int32_t   { return static_field("coexistStaticResult")->get(); }

        // Reads an instance's own seed (proves `self` is the right object).
        auto seed() const -> std::int32_t { return get_field("seed")->get(); }
    };

    // ---- Fixture-mirrored constants (kept in lockstep with HookBasic.java) --
    constexpr std::int32_t INSTANCE_CALLS{ 3 };
    constexpr std::int32_t STATIC_CALLS{ 4 };
    constexpr std::int32_t TOUCH_DELTA_0{ 7 };
    constexpr std::int32_t TOUCH_DELTA_1{ 11 };
    constexpr std::int32_t TOUCH_DELTA_2{ 42 };
    constexpr std::int32_t STATIC_DELTA{ 99 };
    constexpr std::int32_t COMBINE_A{ 5 };
    constexpr std::int64_t COMBINE_B{ 0x1122334455667788LL };
    constexpr std::int32_t COMBINE_C{ -13 };
    constexpr std::int32_t SEED_A{ 2000 };
    constexpr std::int32_t SEED_B{ 30000 };
    constexpr std::int32_t DELTA_A{ 3 };
    constexpr std::int32_t DELTA_B{ 4 };
    constexpr double       WIDE_D{ 2.5 };
    constexpr std::int32_t WIDE_I{ 77 };
    constexpr std::int32_t PRIMARY_SEED{ 1000 };

    // ---- Deepening (batch-16) constants (lockstep with HookBasic.java) -----
    // manyArgs(int,long,int,double,int,String,long,int): slot alignment past 8 args.
    constexpr std::int32_t MANY_A{ 11 };
    constexpr std::int64_t MANY_B{ 0x0A0B0C0D0E0F1011LL };
    constexpr std::int32_t MANY_C{ 22 };
    constexpr double       MANY_D{ 6.5 };
    constexpr std::int32_t MANY_E{ 33 };
    constexpr std::int32_t MANY_F_LEN{ 4 };                       // "many".length()
    constexpr std::int64_t MANY_G{ -0x7FEEDDCCBBAA9988LL };
    constexpr std::int32_t MANY_H{ 44 };
    // staticManyArgs(long,int,double,String,int,long): wide-first.
    constexpr std::int64_t SMANY_A{ 0x1213141516171819LL };
    constexpr std::int32_t SMANY_B{ 55 };
    constexpr double       SMANY_C{ -12.75 };
    constexpr std::int32_t SMANY_D_LEN{ 5 };                      // "swide".length()
    constexpr std::int32_t SMANY_E{ 66 };
    constexpr std::int64_t SMANY_F{ 0x2122232425262728LL };
    // floatArg(float,int).
    constexpr float        FLOAT_ARG{ -3.25f };
    constexpr std::int32_t FLOAT_TRAILING_I{ 88 };
    // objArg(HookBasic,int).
    constexpr std::int32_t OBJ_ARG_SEED{ 7777 };
    constexpr std::int32_t OBJ_ARG_DELTA{ 9 };
    // coexist.
    constexpr std::int32_t COEXIST_INSTANCE_DELTA{ 13 };
    constexpr std::int32_t COEXIST_STATIC_DELTA{ 21 };
    // boundaryCombine extreme/all-ones argument sets.
    constexpr std::int32_t INT_MIN_J{ -2147483647 - 1 };         // Integer.MIN_VALUE
    constexpr std::int32_t INT_MAX_J{ 2147483647 };              // Integer.MAX_VALUE
    constexpr std::int64_t LONG_MIN_J{ -9223372036854775807LL - 1 }; // Long.MIN_VALUE

    // ---- Return-value interception constants (modes 8-20) ------------------
    // The Java fixture calls each ret*(x) with x==RET_INT_X and naturally
    // returns the "natural" value; the hook OVERRIDES it with the "*_OVERRIDE"
    // value, and we assert Java observed the override (not the natural value).
    constexpr std::int32_t RET_INT_NATURAL{ 1 };            // retInt(1) -> 2 natural
    constexpr std::int32_t RET_INT_NATURAL_RESULT{ 2 };     // x + 1
    constexpr std::int32_t RET_INT_OVERRIDE{ 0x5EED1234 };  // distinct from natural
    constexpr std::int64_t RET_LONG_OVERRIDE{ -0x0123456789ABCDEFLL };
    constexpr double       RET_DOUBLE_OVERRIDE{ -987654.3125 };  // exact in IEEE-754
    constexpr float        RET_FLOAT_OVERRIDE{ -123.5f };        // exact in IEEE-754
    constexpr std::int8_t  RET_BYTE_OVERRIDE{ static_cast<std::int8_t>(-7) };
    constexpr std::int16_t RET_SHORT_OVERRIDE{ static_cast<std::int16_t>(-31000) };
    constexpr std::uint16_t RET_CHAR_OVERRIDE{ static_cast<std::uint16_t>(0xBEEF) };
    // mode 18: hook rewrites touch()'s delta arg to this value.
    constexpr std::int32_t MUTATED_DELTA{ 555 };

    // The fully-qualified (JVM-internal, slash-form) class name of the fixture.
    // Used both to locate the live Method* (for the interpreter-route settle)
    // and as the predicate filter for deoptimize_methods_if so the deopt is
    // scoped to this fixture's methods only.
    constexpr const char* FIXTURE_CLASS{ "vmhook/fixtures/HookBasic" };

    // ---- Hook observation state (reset per scenario) -----------------------
    std::atomic<std::int32_t> g_fire_count{ 0 };

    // Instance touch() observations.
    std::atomic<std::int32_t> g_self_nonnull_fires{ 0 };
    std::atomic<std::int32_t> g_self_seed_ok_fires{ 0 };
    std::atomic<std::int64_t> g_arg_xor{ 0 };       // XOR of every decoded delta
    std::atomic<std::int64_t> g_arg_sum{ 0 };       // SUM of every decoded delta
    std::atomic<std::int32_t> g_last_delta{ -1 };

    // Static touch() observations.
    std::atomic<std::int32_t> g_static_arg_ok_fires{ 0 };

    // combine() observations.
    std::atomic<bool>         g_combine_self_ok{ false };
    std::atomic<bool>         g_combine_a_ok{ false };
    std::atomic<bool>         g_combine_b_ok{ false };
    std::atomic<bool>         g_combine_c_ok{ false };

    // static combine() observations.
    std::atomic<bool>         g_scombine_a_ok{ false };
    std::atomic<bool>         g_scombine_b_ok{ false };
    std::atomic<bool>         g_scombine_c_ok{ false };

    // two-instance observations: the seed each fire saw, in call order.
    std::atomic<std::int32_t> g_two_seed_first{ -1 };
    std::atomic<std::int32_t> g_two_seed_second{ -1 };
    std::atomic<std::int32_t> g_two_delta_first{ -1 };
    std::atomic<std::int32_t> g_two_delta_second{ -1 };

    // wideArgs() observations.
    std::atomic<bool>         g_wide_self_ok{ false };
    std::atomic<bool>         g_wide_flag_ok{ false };
    std::atomic<bool>         g_wide_d_ok{ false };
    std::atomic<bool>         g_wide_s_ok{ false };
    std::atomic<bool>         g_wide_i_ok{ false };

    // Return-value interception observations (modes 8-20): the decoded incoming
    // arg the detour saw, so we can prove the detour ran on the SAME call whose
    // return Java later observed as overridden.
    std::atomic<std::int32_t> g_ret_arg_seen{ -1 };
    std::atomic<bool>         g_setarg_ok{ false };     // set_arg returned true

    // ---- Deepening (batch-16) observations --------------------------------
    // manyArgs / staticManyArgs: each decoded arg matched its expected value.
    std::atomic<bool> g_many_self_ok{ false };
    std::atomic<bool> g_many_a_ok{ false };
    std::atomic<bool> g_many_b_ok{ false };
    std::atomic<bool> g_many_c_ok{ false };
    std::atomic<bool> g_many_d_ok{ false };
    std::atomic<bool> g_many_e_ok{ false };
    std::atomic<bool> g_many_f_ok{ false };
    std::atomic<bool> g_many_g_ok{ false };
    std::atomic<bool> g_many_h_ok{ false };
    std::atomic<bool> g_smany_a_ok{ false };
    std::atomic<bool> g_smany_b_ok{ false };
    std::atomic<bool> g_smany_c_ok{ false };
    std::atomic<bool> g_smany_d_ok{ false };
    std::atomic<bool> g_smany_e_ok{ false };
    std::atomic<bool> g_smany_f_ok{ false };
    // boundaryCombine: the exact decoded args (so we can assert the boundary value).
    std::atomic<std::int32_t> g_bound_a{ 0 };
    std::atomic<std::int64_t> g_bound_b{ 0 };
    std::atomic<std::int32_t> g_bound_c{ 0 };
    // floatArg.
    std::atomic<bool> g_float_self_ok{ false };
    std::atomic<bool> g_float_f_ok{ false };
    std::atomic<bool> g_float_i_ok{ false };
    // nullRefArgs: string-arg decoded empty, object-arg decoded null.
    std::atomic<bool> g_nullref_string_empty{ false };
    std::atomic<bool> g_nullref_obj_null{ false };
    // objArg: object-arg (not the receiver) decoded to the right seed.
    std::atomic<bool> g_objarg_other_nonnull{ false };
    std::atomic<std::int32_t> g_objarg_other_seed{ -1 };
    std::atomic<std::int32_t> g_objarg_delta{ -1 };
    // coexist: separate fire counters + decoded args for the two methods.
    std::atomic<std::int32_t> g_coexist_instance_fires{ 0 };
    std::atomic<std::int32_t> g_coexist_static_fires{ 0 };
    std::atomic<std::int32_t> g_coexist_instance_delta{ -1 };
    std::atomic<std::int32_t> g_coexist_static_delta{ -1 };

    auto reset_observations() -> void
    {
        g_fire_count.store(0);
        g_self_nonnull_fires.store(0);
        g_self_seed_ok_fires.store(0);
        g_arg_xor.store(0);
        g_arg_sum.store(0);
        g_last_delta.store(-1);
        g_static_arg_ok_fires.store(0);
        g_combine_self_ok.store(false);
        g_combine_a_ok.store(false);
        g_combine_b_ok.store(false);
        g_combine_c_ok.store(false);
        g_scombine_a_ok.store(false);
        g_scombine_b_ok.store(false);
        g_scombine_c_ok.store(false);
        g_two_seed_first.store(-1);
        g_two_seed_second.store(-1);
        g_two_delta_first.store(-1);
        g_two_delta_second.store(-1);
        g_wide_self_ok.store(false);
        g_wide_flag_ok.store(false);
        g_wide_d_ok.store(false);
        g_wide_s_ok.store(false);
        g_wide_i_ok.store(false);
        g_ret_arg_seen.store(-1);
        g_setarg_ok.store(false);
        g_many_self_ok.store(false);
        g_many_a_ok.store(false);
        g_many_b_ok.store(false);
        g_many_c_ok.store(false);
        g_many_d_ok.store(false);
        g_many_e_ok.store(false);
        g_many_f_ok.store(false);
        g_many_g_ok.store(false);
        g_many_h_ok.store(false);
        g_smany_a_ok.store(false);
        g_smany_b_ok.store(false);
        g_smany_c_ok.store(false);
        g_smany_d_ok.store(false);
        g_smany_e_ok.store(false);
        g_smany_f_ok.store(false);
        g_bound_a.store(0);
        g_bound_b.store(0);
        g_bound_c.store(0);
        g_float_self_ok.store(false);
        g_float_f_ok.store(false);
        g_float_i_ok.store(false);
        g_nullref_string_empty.store(false);
        g_nullref_obj_null.store(false);
        g_objarg_other_nonnull.store(false);
        g_objarg_other_seed.store(-1);
        g_objarg_delta.store(-1);
        g_coexist_instance_fires.store(0);
        g_coexist_static_fires.store(0);
        g_coexist_instance_delta.store(-1);
        g_coexist_static_delta.store(-1);
    }

    // Drives exactly one probe cycle for `mode`: resets observations + the
    // latched `done` flag, programs the scenario selector, then runs the probe.
    auto drive(vmhook_test::context& ctx, std::int32_t mode) -> bool
    {
        reset_observations();
        return ctx.run_probe(
            [mode](bool value)
            {
                if (value)
                {
                    // Rising edge: program the scenario and clear the latch
                    // BEFORE the fixture's pending() observes go.
                    hook_basic_fixture::set_done(false);
                    hook_basic_fixture::set_mode(mode);
                }
                hook_basic_fixture::set_go(value);
            },
            []() { return hook_basic_fixture::get_done(); });
    }

    // ---- JIT-reliability helpers -------------------------------------------

    // Locates the live Method* for FIXTURE_CLASS::name(signature) by walking the
    // InstanceKlass methods array.  Returns nullptr if anything looks invalid —
    // callers treat nullptr as "cannot settle the interpreter route for this
    // method" and fall back to driving without the settle (never a crash).  All
    // reads are pointer-validated.  (Same shape as hook_verify_repair.cpp's
    // find_hot_method, generalised to name+signature.)
    auto find_method(const char* const name, const char* const signature)
        -> vmhook::hotspot::method*
    {
        vmhook::hotspot::klass* const k{ vmhook::find_class(FIXTURE_CLASS) };
        if (!k || !vmhook::hotspot::is_valid_pointer(k))
        {
            return nullptr;
        }
        const std::int32_t count{ k->get_methods_count() };
        vmhook::hotspot::method** const methods{ k->get_methods_ptr() };
        if (!methods || count <= 0)
        {
            return nullptr;
        }
        const std::string want_name{ name };
        const std::string want_sig{ signature };
        for (std::int32_t i{ 0 }; i < count; ++i)
        {
            vmhook::hotspot::method* const m{ methods[i] };
            if (!m || !vmhook::hotspot::is_valid_pointer(m))
            {
                continue;
            }
            const std::string m_name = m->get_name();        // copy-init (MSVC)
            const std::string m_sig = m->get_signature();    // copy-init (MSVC)
            if (m_name == want_name && m_sig == want_sig)
            {
                return m;
            }
        }
        return nullptr;
    }

    // Reads Method::_code through a validated pointer.  nullptr means "not
    // currently JIT-compiled" (the deopted state in which interpreted dispatch
    // reaches our i2i patch).
    auto method_code(vmhook::hotspot::method* const m) -> void*
    {
        if (!m || !vmhook::hotspot::is_valid_pointer(m))
        {
            return nullptr;
        }
        void* const code{ m->get_code() };
        return (code && vmhook::hotspot::is_valid_pointer(code)) ? code : nullptr;
    }

    // True iff an INTERPRETED dispatch of this method will route through the
    // patched i2i stub (so the detour can fire).  That holds exactly when
    // _from_interpreted_entry == _i2i_entry — the "deopted" invariant the install
    // path (vmhook.hpp:8531) and verify_hooks()/deoptimize_methods_if re-establish.
    // Once HotSpot re-JITs the method, _from_interpreted_entry is repointed at the
    // i2c adapter and this returns false (the interpreter would bypass the patch).
    // Pointer-validated; unreadable entries yield false ("cannot guarantee the
    // i2i route").  (Mirrors hook_verify_repair.cpp's interp_routes_through_i2i.)
    auto interp_routes_through_i2i(vmhook::hotspot::method* const m) -> bool
    {
        if (!m || !vmhook::hotspot::is_valid_pointer(m))
        {
            return false;
        }
        void* const i2i{ m->get_i2i_entry() };
        void* const fie{ m->get_from_interpreted_entry() };
        return i2i != nullptr && fie != nullptr && i2i == fie;
    }

    // Drives the hooked Method back to the interpreter so the next dispatch
    // reaches the i2i patch, with a bounded tolerance for HotSpot's ASYNCHRONOUS
    // tiered JIT (which can compile/recompile the method at any instant, including
    // the window right after install).  Returns true once the interpreter route
    // is observed established (interp_routes_through_i2i), or when there is no live
    // Method* to settle (m == nullptr — the caller then just drives, which is the
    // pre-hardening behaviour and still correct on a cold/interpreted method).
    //
    // Each attempt:
    //   1. deoptimize_methods_if(<our fixture class>) — for any CURRENTLY-compiled
    //      fixture method this repoints _from_interpreted_entry -> i2i,
    //      _from_compiled_entry -> c2i and nulls _code (vmhook.hpp:6799-6801), i.e.
    //      the exact deopt the install path performs for an already-JIT'd method.
    //   2. verify_hooks() — re-arms NO_COMPILE / re-applies the deopt for the hook
    //      (and, when _code != null, also re-points the interpreter entry to i2i,
    //      vmhook.hpp:8881-8885), so an in-flight recompile that just landed is
    //      absorbed.  No-op on an already-clean, interpreted hook.
    //   3. Re-check the route; a short settle lets a queued nmethod land + be
    //      re-nulled before the next read.
    //
    // Non-vacuous: a transient async recompile is ABSORBED (a later attempt sees
    // the route restored), but if the method genuinely cannot be driven to the
    // interpreter route within the budget this returns false and the caller's
    // drive_until_fires falls through to its HARD firing assertion (a real
    // regression then fails the suite — the firing checks are NOT softened).
    auto settle_interpreter_route(vmhook::hotspot::method* const m, int attempts) -> bool
    {
        if (m == nullptr)
        {
            // No live Method* to inspect.  Best-effort global re-arm so a
            // freshly-installed hook is in its deopted state, then let the caller
            // drive (the cold/interpreted path needs no settle).
            (void)vmhook::verify_hooks();
            return false;
        }
        if (interp_routes_through_i2i(m) && method_code(m) == nullptr)
        {
            return true;   // already routed through the interpreter i2i patch
        }
        for (int attempt{ 0 }; attempt < attempts; ++attempt)
        {
            // Deopt any currently-compiled fixture method back to the interpreter.
            (void)vmhook::deoptimize_methods_if(
                [](const std::string& class_name, vmhook::hotspot::method*) -> bool
                {
                    return class_name == FIXTURE_CLASS;
                });
            // Re-arm / re-apply the hook's deopt (and re-point the interpreter
            // entry when _code != null).  No-op on a clean hook.
            (void)vmhook::verify_hooks();

            if (interp_routes_through_i2i(m) && method_code(m) == nullptr)
            {
                return true;
            }
            // Let any in-flight compile / safepoint settle before re-reading.
            // 40 ms matches the proven code_settles_null/verify_settles_zero
            // cadence used for the same java24-26 async-recompile race elsewhere.
            std::this_thread::sleep_for(std::chrono::milliseconds{ 40 });
        }
        return interp_routes_through_i2i(m) && method_code(m) == nullptr;
    }

    // Drives `mode` and guarantees the detour fires `expected_fires` times by
    // re-settling the interpreter route and re-driving the probe up to `attempts`
    // times.  `done_out` receives the probe-completed status of the FINAL drive
    // (an infra signal the caller can assert hard regardless of the fire count).
    //
    // Rationale: a single drive can still race an async recompile that lands
    // between settle_interpreter_route() returning and the START of the Java
    // run() (HotSpot's compiler threads run concurrently).  Because `done`
    // latches and drive() resets observations each cycle, re-driving is clean; we
    // re-deopt the method before each retry so every attempt starts from the
    // interpreter route.  The caller's firing checks then stay HARD on the final
    // observations — this wrapper only EXISTS to make the firing deterministic, it
    // does not weaken any assertion.
    auto drive_until_fires(vmhook_test::context& ctx,
                           std::int32_t mode,
                           vmhook::hotspot::method* const m,
                           std::int32_t expected_fires,
                           int attempts,
                           bool& done_out) -> void
    {
        done_out = false;
        for (int attempt{ 0 }; attempt < attempts; ++attempt)
        {
            (void)settle_interpreter_route(m, 12);
            done_out = drive(ctx, mode);
            if (done_out && g_fire_count.load() == expected_fires)
            {
                return;   // achieved the expected firing deterministically
            }
            // Brief pause before re-settling so a recompile triggered by this
            // dispatch can be observed + undone on the next settle pass.
            std::this_thread::sleep_for(std::chrono::milliseconds{ 25 });
        }
    }
}

VMHOOK_JVM_MODULE(hook_basic)
{
    vmhook::register_class<hook_basic_fixture>("vmhook/fixtures/HookBasic");

    // All Method-poking + hook install/teardown is wrapped so a mid-run throw on
    // a hostile JDK is recorded as [INFO] rather than crashing the suite (other
    // modules run after us); the unconditional shutdown_hooks() below is OUTSIDE
    // the try so NO hook is ever left armed.
    try
    {
    // =====================================================================
    // Scenario 1 — INSTANCE method touch(int): exactly-once, self, arg decode,
    //              allow-through, then uninstall-on-scope-exit.
    // =====================================================================
    {
        auto handle{ vmhook::scoped_hook<hook_basic_fixture>(
            "touch",
            [](vmhook::return_value&,
               const std::unique_ptr<hook_basic_fixture>& self,
               std::int32_t delta)
            {
                g_fire_count.fetch_add(1, std::memory_order_relaxed);
                if (self != nullptr)
                {
                    g_self_nonnull_fires.fetch_add(1, std::memory_order_relaxed);
                    if (self->seed() == PRIMARY_SEED)
                    {
                        g_self_seed_ok_fires.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                g_arg_xor.fetch_xor(delta, std::memory_order_relaxed);
                g_arg_sum.fetch_add(delta, std::memory_order_relaxed);
                g_last_delta.store(delta, std::memory_order_relaxed);
            }) };

        ctx.check("instance_scoped_hook_installed", handle.installed());

        // Deopt touch() back to the interpreter so the i2i detour is taken even
        // if HotSpot JIT-compiled it before install (the fast-JIT JDK24-26 race).
        vmhook::hotspot::method* const m{ find_method("touch", "(I)I") };
        bool done{ false };
        drive_until_fires(ctx, 1, m, INSTANCE_CALLS, 6, done);
        ctx.check("instance_probe_completed", done);

        // --- exactly-once-per-call -----------------------------------------
        ctx.check("instance_calls_made_is_3",
                  hook_basic_fixture::get_instance_calls_made() == INSTANCE_CALLS);
        ctx.check("instance_fired_exactly_once_per_call",
                  g_fire_count.load() == INSTANCE_CALLS);
        ctx.check("instance_fired_not_zero", g_fire_count.load() != 0);
        ctx.check("instance_fired_not_doubled",
                  g_fire_count.load() <= INSTANCE_CALLS);

        // --- correct self on every fire ------------------------------------
        ctx.check("instance_self_nonnull_every_fire",
                  g_self_nonnull_fires.load() == INSTANCE_CALLS);
        ctx.check("instance_self_is_correct_object_every_fire",
                  g_self_seed_ok_fires.load() == INSTANCE_CALLS);

        // --- correct decoded args ------------------------------------------
        ctx.check("instance_arg_sum_matches",
                  g_arg_sum.load() == (TOUCH_DELTA_0 + TOUCH_DELTA_1 + TOUCH_DELTA_2));
        ctx.check("instance_arg_xor_matches",
                  g_arg_xor.load() == (TOUCH_DELTA_0 ^ TOUCH_DELTA_1 ^ TOUCH_DELTA_2));
        ctx.check("instance_last_arg_is_42",
                  g_last_delta.load() == TOUCH_DELTA_2);

        // --- allow-through: original body ran, unmodified ------------------
        ctx.check("instance_allow_through_last_result",
                  hook_basic_fixture::get_last_touch_result() == (PRIMARY_SEED + TOUCH_DELTA_2));
        ctx.check("instance_allow_through_result_sum",
                  hook_basic_fixture::get_touch_result_sum()
                      == static_cast<std::int64_t>(PRIMARY_SEED + TOUCH_DELTA_0)
                       + (PRIMARY_SEED + TOUCH_DELTA_1)
                       + (PRIMARY_SEED + TOUCH_DELTA_2));
    }
    // handle is now out of scope -> hook uninstalled.

    // =====================================================================
    // Scenario 7 — after the instance handle dropped: detour must NOT fire,
    //              original body still runs (proves scoped_hook teardown).
    // =====================================================================
    {
        const bool done{ drive(ctx, 7) };
        ctx.check("uninstall_probe_completed", done);
        ctx.check("uninstall_java_call_happened",
                  hook_basic_fixture::get_instance_calls_made() == 1);
        ctx.check("uninstall_detour_did_not_fire", g_fire_count.load() == 0);
        ctx.check("uninstall_original_still_ran",
                  hook_basic_fixture::get_last_touch_result() == (500 + 1));
    }

    // =====================================================================
    // Scenario 2 — STATIC method staticTouch(int): exactly-once, NO self,
    //              arg decode at slot 0, allow-through.
    // =====================================================================
    {
        auto handle{ vmhook::scoped_hook<hook_basic_fixture>(
            "staticTouch",
            [](vmhook::return_value&, std::int32_t delta)
            {
                g_fire_count.fetch_add(1, std::memory_order_relaxed);
                if (delta == STATIC_DELTA)
                {
                    g_static_arg_ok_fires.fetch_add(1, std::memory_order_relaxed);
                }
                g_last_delta.store(delta, std::memory_order_relaxed);
            }) };

        ctx.check("static_scoped_hook_installed", handle.installed());

        // Deopt staticTouch() back to the interpreter (same fast-JIT hardening).
        vmhook::hotspot::method* const m{ find_method("staticTouch", "(I)I") };
        bool done{ false };
        drive_until_fires(ctx, 2, m, STATIC_CALLS, 6, done);
        ctx.check("static_probe_completed", done);

        ctx.check("static_calls_made_is_4",
                  hook_basic_fixture::get_static_calls_made() == STATIC_CALLS);
        ctx.check("static_fired_exactly_once_per_call",
                  g_fire_count.load() == STATIC_CALLS);
        ctx.check("static_arg_ok_every_fire",
                  g_static_arg_ok_fires.load() == STATIC_CALLS);
        ctx.check("static_arg_slot0_decoded", g_last_delta.load() == STATIC_DELTA);
        // allow-through: staticTouch returns delta*2.
        ctx.check("static_allow_through_result",
                  hook_basic_fixture::get_last_touch_result() == (STATIC_DELTA * 2));
    }

    // =====================================================================
    // Scenario 3 — INSTANCE combine(int,long,int): self + multi-slot decode
    //              (long widens across two interpreter slots; trailing int must
    //              still be read from the correct slot).
    // =====================================================================
    {
        auto handle{ vmhook::scoped_hook<hook_basic_fixture>(
            "combine",
            [](vmhook::return_value&,
               const std::unique_ptr<hook_basic_fixture>& self,
               std::int32_t a, std::int64_t b, std::int32_t c)
            {
                g_fire_count.fetch_add(1, std::memory_order_relaxed);
                g_combine_self_ok.store(self != nullptr && self->seed() == PRIMARY_SEED,
                                        std::memory_order_relaxed);
                g_combine_a_ok.store(a == COMBINE_A, std::memory_order_relaxed);
                g_combine_b_ok.store(b == COMBINE_B, std::memory_order_relaxed);
                g_combine_c_ok.store(c == COMBINE_C, std::memory_order_relaxed);
            }) };

        ctx.check("combine_scoped_hook_installed", handle.installed());

        // Deopt combine() back to the interpreter (same fast-JIT hardening).
        vmhook::hotspot::method* const m{ find_method("combine", "(IJI)J") };
        bool done{ false };
        drive_until_fires(ctx, 3, m, 1, 6, done);
        ctx.check("combine_probe_completed", done);

        ctx.check("combine_fired_exactly_once", g_fire_count.load() == 1);
        ctx.check("combine_self_correct", g_combine_self_ok.load());
        ctx.check("combine_arg_a_int_decoded", g_combine_a_ok.load());
        ctx.check("combine_arg_b_long_decoded", g_combine_b_ok.load());
        ctx.check("combine_arg_c_int_after_long_decoded", g_combine_c_ok.load());
        // allow-through: original returns seed + a + b + c.
        ctx.check("combine_allow_through_result",
                  hook_basic_fixture::get_combine_result()
                      == static_cast<std::int64_t>(PRIMARY_SEED) + COMBINE_A + COMBINE_B + COMBINE_C);
    }

    // =====================================================================
    // Scenario 4 — STATIC staticCombine(int,long,int): multi-slot decode with
    //              NO self (first int at slot 0, long at slot 1-2, int at slot 3).
    // =====================================================================
    {
        auto handle{ vmhook::scoped_hook<hook_basic_fixture>(
            "staticCombine",
            [](vmhook::return_value&,
               std::int32_t a, std::int64_t b, std::int32_t c)
            {
                g_fire_count.fetch_add(1, std::memory_order_relaxed);
                g_scombine_a_ok.store(a == COMBINE_A, std::memory_order_relaxed);
                g_scombine_b_ok.store(b == COMBINE_B, std::memory_order_relaxed);
                g_scombine_c_ok.store(c == COMBINE_C, std::memory_order_relaxed);
            }) };

        ctx.check("static_combine_scoped_hook_installed", handle.installed());

        // Deopt staticCombine() back to the interpreter (same fast-JIT hardening).
        vmhook::hotspot::method* const m{ find_method("staticCombine", "(IJI)J") };
        bool done{ false };
        drive_until_fires(ctx, 4, m, 1, 6, done);
        ctx.check("static_combine_probe_completed", done);

        ctx.check("static_combine_fired_exactly_once", g_fire_count.load() == 1);
        ctx.check("static_combine_arg_a_slot0", g_scombine_a_ok.load());
        ctx.check("static_combine_arg_b_long_slot1", g_scombine_b_ok.load());
        ctx.check("static_combine_arg_c_after_long", g_scombine_c_ok.load());
        ctx.check("static_combine_allow_through_result",
                  hook_basic_fixture::get_static_combine_result()
                      == static_cast<std::int64_t>(COMBINE_A) + COMBINE_B + COMBINE_C);
    }

    // =====================================================================
    // Scenario 5 — TWO DIFFERENT instances of touch(): the detour must see the
    //              CORRECT receiver each time (not merely non-null).
    // =====================================================================
    {
        auto handle{ vmhook::scoped_hook<hook_basic_fixture>(
            "touch",
            [](vmhook::return_value&,
               const std::unique_ptr<hook_basic_fixture>& self,
               std::int32_t delta)
            {
                const std::int32_t order{ g_fire_count.fetch_add(1, std::memory_order_relaxed) };
                const std::int32_t seen_seed{ self != nullptr ? self->seed() : -1 };
                if (order == 0)
                {
                    g_two_seed_first.store(seen_seed, std::memory_order_relaxed);
                    g_two_delta_first.store(delta, std::memory_order_relaxed);
                }
                else if (order == 1)
                {
                    g_two_seed_second.store(seen_seed, std::memory_order_relaxed);
                    g_two_delta_second.store(delta, std::memory_order_relaxed);
                }
            }) };

        ctx.check("two_instance_scoped_hook_installed", handle.installed());

        // Deopt touch() back to the interpreter (same fast-JIT hardening).
        vmhook::hotspot::method* const m{ find_method("touch", "(I)I") };
        bool done{ false };
        drive_until_fires(ctx, 5, m, 2, 6, done);
        ctx.check("two_instance_probe_completed", done);

        ctx.check("two_instance_fired_exactly_twice", g_fire_count.load() == 2);
        // First call was on instance A (seed SEED_A), second on B (seed SEED_B).
        ctx.check("two_instance_first_self_is_A",
                  g_two_seed_first.load() == SEED_A);
        ctx.check("two_instance_second_self_is_B",
                  g_two_seed_second.load() == SEED_B);
        ctx.check("two_instance_selves_differ",
                  g_two_seed_first.load() != g_two_seed_second.load());
        ctx.check("two_instance_first_arg", g_two_delta_first.load() == DELTA_A);
        ctx.check("two_instance_second_arg", g_two_delta_second.load() == DELTA_B);
        // Java confirms the two instances really had the seeds we cross-checked.
        ctx.check("two_instance_java_seed_a", hook_basic_fixture::get_instance_a_seed() == SEED_A);
        ctx.check("two_instance_java_seed_b", hook_basic_fixture::get_instance_b_seed() == SEED_B);
        // allow-through on both instances.
        ctx.check("two_instance_allow_through_a",
                  hook_basic_fixture::get_two_instance_result_a() == (SEED_A + DELTA_A));
        ctx.check("two_instance_allow_through_b",
                  hook_basic_fixture::get_two_instance_result_b() == (SEED_B + DELTA_B));
    }

    // =====================================================================
    // Scenario 6 — INSTANCE wideArgs(boolean,double,String,int): exercise
    //              boolean + double (2 slots) + String (reference) + trailing
    //              int decode together, with a correct self.
    // =====================================================================
    {
        auto handle{ vmhook::scoped_hook<hook_basic_fixture>(
            "wideArgs",
            [](vmhook::return_value&,
               const std::unique_ptr<hook_basic_fixture>& self,
               bool flag, double d, const std::string& s, std::int32_t i)
            {
                g_fire_count.fetch_add(1, std::memory_order_relaxed);
                g_wide_self_ok.store(self != nullptr && self->seed() == 0,
                                     std::memory_order_relaxed);
                g_wide_flag_ok.store(flag == true, std::memory_order_relaxed);
                g_wide_d_ok.store(d == WIDE_D, std::memory_order_relaxed);
                g_wide_s_ok.store(s == "vmhook", std::memory_order_relaxed);
                g_wide_i_ok.store(i == WIDE_I, std::memory_order_relaxed);
            }) };

        ctx.check("wide_scoped_hook_installed", handle.installed());

        // Deopt wideArgs() back to the interpreter (same fast-JIT hardening).
        vmhook::hotspot::method* const m{
            find_method("wideArgs", "(ZDLjava/lang/String;I)D") };
        bool done{ false };
        drive_until_fires(ctx, 6, m, 1, 6, done);
        ctx.check("wide_probe_completed", done);

        ctx.check("wide_fired_exactly_once", g_fire_count.load() == 1);
        ctx.check("wide_self_correct", g_wide_self_ok.load());
        ctx.check("wide_arg_boolean_decoded", g_wide_flag_ok.load());
        ctx.check("wide_arg_double_decoded", g_wide_d_ok.load());
        ctx.check("wide_arg_string_decoded", g_wide_s_ok.load());
        ctx.check("wide_arg_trailing_int_after_double_and_ref", g_wide_i_ok.load());
        // allow-through: wideArgs returns 1.0 + 2.5 + len("vmhook")(=6) + 77 = 86.5
        ctx.check("wide_allow_through_result",
                  hook_basic_fixture::get_wide_result() == (1.0 + WIDE_D + 6.0 + WIDE_I));
    }

    // =====================================================================
    // Scenario 8 — RETURN-VALUE INTERCEPTION (set<int>): the detour overrides
    //              the int return, so Java observes the OVERRIDE, not seed-natural.
    //              Proves the cancel/retval short-circuit path (rax) for a 32-bit
    //              int return and that the override differs from the natural value.
    // =====================================================================
    {
        auto handle{ vmhook::scoped_hook<hook_basic_fixture>(
            "retInt", "(I)I",
            [](vmhook::return_value& ret,
               const std::unique_ptr<hook_basic_fixture>&,
               std::int32_t x)
            {
                g_fire_count.fetch_add(1, std::memory_order_relaxed);
                g_ret_arg_seen.store(x, std::memory_order_relaxed);
                ret.set(RET_INT_OVERRIDE);   // suppress body, force this return
            }) };

        ctx.check("retint_scoped_hook_installed", handle.installed());

        vmhook::hotspot::method* const m{ find_method("retInt", "(I)I") };
        bool done{ false };
        drive_until_fires(ctx, 8, m, 1, 6, done);
        ctx.check("retint_probe_completed", done);

        ctx.check("retint_fired_exactly_once", g_fire_count.load() == 1);
        ctx.check("retint_detour_saw_arg", g_ret_arg_seen.load() == RET_INT_NATURAL);
        // The natural (un-hooked) return is RET_INT_NATURAL_RESULT; the override
        // must be observed instead, and must DIFFER from natural so a no-op hook
        // (body ran) cannot accidentally pass.
        ctx.check("retint_override_observed",
                  hook_basic_fixture::get_ret_int_observed() == RET_INT_OVERRIDE);
        ctx.check("retint_override_not_natural",
                  hook_basic_fixture::get_ret_int_observed() != RET_INT_NATURAL_RESULT);
    }

    // =====================================================================
    // Scenario 9 — set<long>: override a 64-bit long return (full-width rax).
    // =====================================================================
    {
        auto handle{ vmhook::scoped_hook<hook_basic_fixture>(
            "retLong", "(I)J",
            [](vmhook::return_value& ret,
               const std::unique_ptr<hook_basic_fixture>&,
               std::int32_t)
            {
                g_fire_count.fetch_add(1, std::memory_order_relaxed);
                ret.set(RET_LONG_OVERRIDE);
            }) };

        ctx.check("retlong_scoped_hook_installed", handle.installed());

        vmhook::hotspot::method* const m{ find_method("retLong", "(I)J") };
        bool done{ false };
        drive_until_fires(ctx, 9, m, 1, 6, done);
        ctx.check("retlong_probe_completed", done);

        ctx.check("retlong_fired_exactly_once", g_fire_count.load() == 1);
        ctx.check("retlong_override_observed",
                  hook_basic_fixture::get_ret_long_observed() == RET_LONG_OVERRIDE);
        // Natural would be 11; the high 32 bits of the override are non-zero, so
        // this also proves the FULL 64 bits survived (not just the low word).
        ctx.check("retlong_override_high_bits_survived",
                  (hook_basic_fixture::get_ret_long_observed() >> 32) != 0);
    }

    // =====================================================================
    // Scenario 10 — set<double>: override a double return (lands in xmm0 via the
    //               trampoline's `movq xmm0, rax`).  Value chosen exact in IEEE-754.
    // =====================================================================
    {
        auto handle{ vmhook::scoped_hook<hook_basic_fixture>(
            "retDouble", "(I)D",
            [](vmhook::return_value& ret,
               const std::unique_ptr<hook_basic_fixture>&,
               std::int32_t)
            {
                g_fire_count.fetch_add(1, std::memory_order_relaxed);
                ret.set(RET_DOUBLE_OVERRIDE);
            }) };

        ctx.check("retdouble_scoped_hook_installed", handle.installed());

        vmhook::hotspot::method* const m{ find_method("retDouble", "(I)D") };
        bool done{ false };
        drive_until_fires(ctx, 10, m, 1, 6, done);
        ctx.check("retdouble_probe_completed", done);

        ctx.check("retdouble_fired_exactly_once", g_fire_count.load() == 1);
        ctx.check("retdouble_override_observed",
                  hook_basic_fixture::get_ret_double_observed() == RET_DOUBLE_OVERRIDE);
    }

    // =====================================================================
    // Scenario 11 — set<float>: override a float return (xmm0, 32-bit lane).
    //               Distinct from the double path (single-precision encoding).
    // =====================================================================
    {
        auto handle{ vmhook::scoped_hook<hook_basic_fixture>(
            "retFloat", "(I)F",
            [](vmhook::return_value& ret,
               const std::unique_ptr<hook_basic_fixture>&,
               std::int32_t)
            {
                g_fire_count.fetch_add(1, std::memory_order_relaxed);
                ret.set(RET_FLOAT_OVERRIDE);
            }) };

        ctx.check("retfloat_scoped_hook_installed", handle.installed());

        vmhook::hotspot::method* const m{ find_method("retFloat", "(I)F") };
        bool done{ false };
        drive_until_fires(ctx, 11, m, 1, 6, done);
        ctx.check("retfloat_probe_completed", done);

        ctx.check("retfloat_fired_exactly_once", g_fire_count.load() == 1);
        ctx.check("retfloat_override_observed",
                  hook_basic_fixture::get_ret_float_observed() == RET_FLOAT_OVERRIDE);
    }

    // =====================================================================
    // Scenario 12 — set<bool>: override a boolean return.  Natural retBoolean(3)
    //               is false (3 is odd); the hook forces true, proving the boolean
    //               return path and that the override flipped the natural answer.
    // =====================================================================
    {
        auto handle{ vmhook::scoped_hook<hook_basic_fixture>(
            "retBoolean", "(I)Z",
            [](vmhook::return_value& ret,
               const std::unique_ptr<hook_basic_fixture>&,
               std::int32_t)
            {
                g_fire_count.fetch_add(1, std::memory_order_relaxed);
                ret.set(true);
            }) };

        ctx.check("retboolean_scoped_hook_installed", handle.installed());

        vmhook::hotspot::method* const m{ find_method("retBoolean", "(I)Z") };
        bool done{ false };
        drive_until_fires(ctx, 12, m, 1, 6, done);
        ctx.check("retboolean_probe_completed", done);

        ctx.check("retboolean_fired_exactly_once", g_fire_count.load() == 1);
        // Natural retBoolean(3) == false; the override forces true.
        ctx.check("retboolean_override_observed",
                  hook_basic_fixture::get_ret_boolean_observed() == true);
    }

    // =====================================================================
    // Scenario 13 — set<int8_t> of a NEGATIVE byte: proves return_value::set()
    //               SIGN-EXTENDS sub-int signed returns (the -1 -> 0x..FF bug the
    //               header documents).  Java reads it back as a signed byte.
    // =====================================================================
    {
        auto handle{ vmhook::scoped_hook<hook_basic_fixture>(
            "retByte", "(I)B",
            [](vmhook::return_value& ret,
               const std::unique_ptr<hook_basic_fixture>&,
               std::int32_t)
            {
                g_fire_count.fetch_add(1, std::memory_order_relaxed);
                ret.set(RET_BYTE_OVERRIDE);
            }) };

        ctx.check("retbyte_scoped_hook_installed", handle.installed());

        vmhook::hotspot::method* const m{ find_method("retByte", "(I)B") };
        bool done{ false };
        drive_until_fires(ctx, 13, m, 1, 6, done);
        ctx.check("retbyte_probe_completed", done);

        ctx.check("retbyte_fired_exactly_once", g_fire_count.load() == 1);
        ctx.check("retbyte_negative_override_observed",
                  hook_basic_fixture::get_ret_byte_observed() == RET_BYTE_OVERRIDE);
        ctx.check("retbyte_override_is_negative",
                  hook_basic_fixture::get_ret_byte_observed() < 0);
    }

    // =====================================================================
    // Scenario 14 — set<int16_t> of a NEGATIVE short: same sign-extension proof
    //               at 16-bit width.
    // =====================================================================
    {
        auto handle{ vmhook::scoped_hook<hook_basic_fixture>(
            "retShort", "(I)S",
            [](vmhook::return_value& ret,
               const std::unique_ptr<hook_basic_fixture>&,
               std::int32_t)
            {
                g_fire_count.fetch_add(1, std::memory_order_relaxed);
                ret.set(RET_SHORT_OVERRIDE);
            }) };

        ctx.check("retshort_scoped_hook_installed", handle.installed());

        vmhook::hotspot::method* const m{ find_method("retShort", "(I)S") };
        bool done{ false };
        drive_until_fires(ctx, 14, m, 1, 6, done);
        ctx.check("retshort_probe_completed", done);

        ctx.check("retshort_fired_exactly_once", g_fire_count.load() == 1);
        ctx.check("retshort_negative_override_observed",
                  hook_basic_fixture::get_ret_short_observed() == RET_SHORT_OVERRIDE);
        ctx.check("retshort_override_is_negative",
                  hook_basic_fixture::get_ret_short_observed() < 0);
    }

    // =====================================================================
    // Scenario 15 — set<char>: override a char return (16-bit UNSIGNED, so a
    //               high bit must ZERO-extend, not sign-extend).  Java reads the
    //               char back; we compare against the unsigned override.
    // =====================================================================
    {
        auto handle{ vmhook::scoped_hook<hook_basic_fixture>(
            "retChar", "(I)C",
            [](vmhook::return_value& ret,
               const std::unique_ptr<hook_basic_fixture>&,
               std::int32_t)
            {
                g_fire_count.fetch_add(1, std::memory_order_relaxed);
                ret.set(static_cast<char16_t>(RET_CHAR_OVERRIDE));
            }) };

        ctx.check("retchar_scoped_hook_installed", handle.installed());

        vmhook::hotspot::method* const m{ find_method("retChar", "(I)C") };
        bool done{ false };
        drive_until_fires(ctx, 15, m, 1, 6, done);
        ctx.check("retchar_probe_completed", done);

        ctx.check("retchar_fired_exactly_once", g_fire_count.load() == 1);
        ctx.check("retchar_override_observed",
                  hook_basic_fixture::get_ret_char_observed() == RET_CHAR_OVERRIDE);
        // 0xBEEF has the high bit set; a char must NOT sign-extend, so the
        // observed value stays in [0, 0xFFFF] and equals the unsigned override.
        ctx.check("retchar_zero_extended_high_bit",
                  hook_basic_fixture::get_ret_char_observed() >= RET_CHAR_OVERRIDE);
    }

    // =====================================================================
    // Scenario 16 — REFERENCE-NULL override: set<wrapper>(nullptr) on a method
    //               returning String forces a null reference (writes 0 into the
    //               retval slot).  Java observes getName() == null even though the
    //               natural body returns a non-null String.
    // =====================================================================
    {
        auto handle{ vmhook::scoped_hook<hook_basic_fixture>(
            "getName", "()Ljava/lang/String;",
            [](vmhook::return_value& ret,
               const std::unique_ptr<hook_basic_fixture>&)
            {
                g_fire_count.fetch_add(1, std::memory_order_relaxed);
                ret.set<hook_basic_fixture>(nullptr);   // return null String
            }) };

        ctx.check("getname_scoped_hook_installed", handle.installed());

        vmhook::hotspot::method* const m{
            find_method("getName", "()Ljava/lang/String;") };
        bool done{ false };
        drive_until_fires(ctx, 16, m, 1, 6, done);
        ctx.check("getname_probe_completed", done);

        ctx.check("getname_fired_exactly_once", g_fire_count.load() == 1);
        ctx.check("getname_null_reference_override_observed",
                  hook_basic_fixture::get_name_was_null());
    }

    // =====================================================================
    // Scenario 17 — cancel() WITHOUT set(): the retval slot stays 0, so Java
    //               observes 0 for an int method even though the body would have
    //               returned RET_INT_NATURAL_RESULT.  Proves cancel suppresses the
    //               body and the zero-initialised retval is what the caller sees.
    // =====================================================================
    {
        auto handle{ vmhook::scoped_hook<hook_basic_fixture>(
            "retInt", "(I)I",
            [](vmhook::return_value& ret,
               const std::unique_ptr<hook_basic_fixture>&,
               std::int32_t)
            {
                g_fire_count.fetch_add(1, std::memory_order_relaxed);
                ret.cancel();   // suppress body, no explicit value -> retval == 0
            }) };

        ctx.check("cancel_scoped_hook_installed", handle.installed());

        vmhook::hotspot::method* const m{ find_method("retInt", "(I)I") };
        bool done{ false };
        drive_until_fires(ctx, 17, m, 1, 6, done);
        ctx.check("cancel_probe_completed", done);

        ctx.check("cancel_fired_exactly_once", g_fire_count.load() == 1);
        ctx.check("cancel_body_suppressed_returns_zero",
                  hook_basic_fixture::get_ret_int_observed() == 0);
        ctx.check("cancel_not_natural_result",
                  hook_basic_fixture::get_ret_int_observed() != RET_INT_NATURAL_RESULT);
    }

    // =====================================================================
    // Scenario 18 — set_arg(): the detour MUTATES touch()'s delta argument
    //               in-place on the interpreter stack, then allows the body
    //               through.  The original body now computes seed + MUTATED_DELTA,
    //               proving argument injection reaches the running method body.
    //               (touch(int): slot 0 == this, slot 1 == delta.)
    // =====================================================================
    {
        auto handle{ vmhook::scoped_hook<hook_basic_fixture>(
            "touch", "(I)I",
            [](vmhook::return_value& ret,
               const std::unique_ptr<hook_basic_fixture>&,
               std::int32_t delta)
            {
                g_fire_count.fetch_add(1, std::memory_order_relaxed);
                g_ret_arg_seen.store(delta, std::memory_order_relaxed);
                g_setarg_ok.store(ret.set_arg(1, std::int32_t{ MUTATED_DELTA }),
                                  std::memory_order_relaxed);
                // No set/cancel -> body runs with the rewritten delta.
            }) };

        ctx.check("setarg_scoped_hook_installed", handle.installed());

        vmhook::hotspot::method* const m{ find_method("touch", "(I)I") };
        bool done{ false };
        drive_until_fires(ctx, 18, m, 1, 6, done);
        ctx.check("setarg_probe_completed", done);

        ctx.check("setarg_fired_exactly_once", g_fire_count.load() == 1);
        ctx.check("setarg_detour_saw_original_delta_1", g_ret_arg_seen.load() == 1);
        ctx.check("setarg_write_succeeded", g_setarg_ok.load());
        // Body computed seed(1000) + MUTATED_DELTA, NOT 1000 + 1.
        ctx.check("setarg_body_saw_mutated_delta",
                  hook_basic_fixture::get_mutated_touch_result()
                      == (PRIMARY_SEED + MUTATED_DELTA));
        ctx.check("setarg_body_not_original_delta",
                  hook_basic_fixture::get_mutated_touch_result()
                      != (PRIMARY_SEED + 1));
    }

    // =====================================================================
    // Scenario 19 — INSTALL / TEARDOWN CYCLE: hook touch(), drop the handle,
    //               then re-install a DIFFERENT detour on the SAME method and
    //               prove the new detour fires.  Guards against a half-installed
    //               method poisoning re-install (specialist flaw #1) on a clean
    //               install path: a fresh scoped_hook on a previously-hooked-then-
    //               released method must install and fire again.
    // =====================================================================
    {
        // First cycle: install + fire + drop.
        std::int32_t first_cycle_fires{ -1 };
        {
            auto h1{ vmhook::scoped_hook<hook_basic_fixture>(
                "touch", "(I)I",
                [](vmhook::return_value&,
                   const std::unique_ptr<hook_basic_fixture>&,
                   std::int32_t)
                {
                    g_fire_count.fetch_add(1, std::memory_order_relaxed);
                }) };
            ctx.check("reinstall_first_cycle_installed", h1.installed());

            vmhook::hotspot::method* const m{ find_method("touch", "(I)I") };
            bool done{ false };
            drive_until_fires(ctx, 19, m, 1, 6, done);
            ctx.check("reinstall_first_cycle_probe_completed", done);
            first_cycle_fires = g_fire_count.load();
            ctx.check("reinstall_first_cycle_fired", first_cycle_fires == 1);
        }
        // h1 dropped here -> hook uninstalled.

        // Second cycle: a fresh hook on the same Method* must install AND fire.
        {
            auto h2{ vmhook::scoped_hook<hook_basic_fixture>(
                "touch", "(I)I",
                [](vmhook::return_value&,
                   const std::unique_ptr<hook_basic_fixture>&,
                   std::int32_t delta)
                {
                    g_fire_count.fetch_add(1, std::memory_order_relaxed);
                    g_ret_arg_seen.store(delta, std::memory_order_relaxed);
                }) };
            ctx.check("reinstall_second_cycle_installed", h2.installed());

            vmhook::hotspot::method* const m{ find_method("touch", "(I)I") };
            bool done{ false };
            drive_until_fires(ctx, 19, m, 1, 6, done);
            ctx.check("reinstall_second_cycle_probe_completed", done);
            ctx.check("reinstall_second_cycle_fired", g_fire_count.load() == 1);
            ctx.check("reinstall_second_cycle_decoded_arg",
                      g_ret_arg_seen.load() == TOUCH_DELTA_0);
            // allow-through on the re-installed hook (body still ran).
            ctx.check("reinstall_second_cycle_allow_through",
                      hook_basic_fixture::get_reinstall_touch_result()
                          == (PRIMARY_SEED + TOUCH_DELTA_0));
        }
    }

    // =====================================================================
    // Scenario 20 — PLAIN (non-scoped) hook<T> + explicit shutdown_hooks():
    //               install via vmhook::hook<T> (no RAII handle), prove it fires,
    //               then tear it down with shutdown_hooks() and prove a subsequent
    //               call does NOT fire while the body still runs.  Exercises the
    //               non-RAII install/teardown contract distinct from scoped_hook.
    // =====================================================================
    {
        const bool installed{ vmhook::hook<hook_basic_fixture>(
            "retInt", "(I)I",
            [](vmhook::return_value& ret,
               const std::unique_ptr<hook_basic_fixture>&,
               std::int32_t x)
            {
                g_fire_count.fetch_add(1, std::memory_order_relaxed);
                g_ret_arg_seen.store(x, std::memory_order_relaxed);
                ret.set(RET_INT_OVERRIDE);
            }) };
        ctx.check("plain_hook_installed", installed);

        vmhook::hotspot::method* const m{ find_method("retInt", "(I)I") };
        bool done{ false };
        drive_until_fires(ctx, 20, m, 1, 6, done);
        ctx.check("plain_hook_probe_completed", done);
        ctx.check("plain_hook_fired_exactly_once", g_fire_count.load() == 1);
        ctx.check("plain_hook_decoded_arg", g_ret_arg_seen.load() == RET_INT_NATURAL);
        ctx.check("plain_hook_override_observed",
                  hook_basic_fixture::get_ret_int_observed() == RET_INT_OVERRIDE);

        // Tear it down explicitly (no RAII handle owns it).
        vmhook::shutdown_hooks();

        // After shutdown the detour must NOT fire and the natural body runs.
        reset_observations();
        const bool done2{ drive(ctx, 20) };
        ctx.check("plain_hook_teardown_probe_completed", done2);
        ctx.check("plain_hook_teardown_did_not_fire", g_fire_count.load() == 0);
        ctx.check("plain_hook_teardown_natural_return",
                  hook_basic_fixture::get_ret_int_observed() == RET_INT_NATURAL_RESULT);
    }

    // =====================================================================
    // Scenario 21 — INSTANCE manyArgs(int,long,int,double,int,String,long,int):
    //               EIGHT args with TWO longs + a double + a String interleaved.
    //               Proves java_slot_offsets keeps every trailing arg aligned PAST
    //               the 8-arg / register boundary (MANY_H sits at slot 11 after
    //               this + two J's + one D + a ref).  Self correct; allow-through.
    // =====================================================================
    {
        auto handle{ vmhook::scoped_hook<hook_basic_fixture>(
            "manyArgs", "(IJIDILjava/lang/String;JI)J",
            [](vmhook::return_value&,
               const std::unique_ptr<hook_basic_fixture>& self,
               std::int32_t a, std::int64_t b, std::int32_t c, double d,
               std::int32_t e, const std::string& f, std::int64_t g, std::int32_t h)
            {
                g_fire_count.fetch_add(1, std::memory_order_relaxed);
                g_many_self_ok.store(self != nullptr && self->seed() == PRIMARY_SEED,
                                     std::memory_order_relaxed);
                g_many_a_ok.store(a == MANY_A, std::memory_order_relaxed);
                g_many_b_ok.store(b == MANY_B, std::memory_order_relaxed);
                g_many_c_ok.store(c == MANY_C, std::memory_order_relaxed);
                g_many_d_ok.store(d == MANY_D, std::memory_order_relaxed);
                g_many_e_ok.store(e == MANY_E, std::memory_order_relaxed);
                g_many_f_ok.store(f == "many", std::memory_order_relaxed);
                g_many_g_ok.store(g == MANY_G, std::memory_order_relaxed);
                g_many_h_ok.store(h == MANY_H, std::memory_order_relaxed);
            }) };

        ctx.check("many_scoped_hook_installed", handle.installed());

        vmhook::hotspot::method* const m{
            find_method("manyArgs", "(IJIDILjava/lang/String;JI)J") };
        bool done{ false };
        drive_until_fires(ctx, 21, m, 1, 6, done);
        ctx.check("many_probe_completed", done);

        ctx.check("many_fired_exactly_once", g_fire_count.load() == 1);
        ctx.check("many_self_correct", g_many_self_ok.load());
        ctx.check("many_arg_a_int_slot1", g_many_a_ok.load());
        ctx.check("many_arg_b_long_slot2", g_many_b_ok.load());
        ctx.check("many_arg_c_int_after_long", g_many_c_ok.load());
        ctx.check("many_arg_d_double_after_int", g_many_d_ok.load());
        ctx.check("many_arg_e_int_after_double", g_many_e_ok.load());
        ctx.check("many_arg_f_string_after_double", g_many_f_ok.load());
        ctx.check("many_arg_g_long_after_ref", g_many_g_ok.load());
        ctx.check("many_arg_h_trailing_int_past_8_boundary", g_many_h_ok.load());
        // allow-through: body summed seed + a + b + c + (long)d + e + len("many") + g + h.
        ctx.check("many_allow_through_result",
                  hook_basic_fixture::get_many_args_result()
                      == static_cast<std::int64_t>(PRIMARY_SEED) + MANY_A + MANY_B
                         + MANY_C + static_cast<std::int64_t>(MANY_D) + MANY_E
                         + MANY_F_LEN + MANY_G + MANY_H);
    }

    // =====================================================================
    // Scenario 22 — STATIC staticManyArgs(long,int,double,String,int,long):
    //               WIDE-FIRST (long at slot 0-1, NO this), so the offset table
    //               starts with a 2-slot type at index 0; the trailing long lands
    //               at slot 7-8.  No self; allow-through.
    // =====================================================================
    {
        auto handle{ vmhook::scoped_hook<hook_basic_fixture>(
            "staticManyArgs", "(JIDLjava/lang/String;IJ)J",
            [](vmhook::return_value&,
               std::int64_t a, std::int32_t b, double c, const std::string& d,
               std::int32_t e, std::int64_t f)
            {
                g_fire_count.fetch_add(1, std::memory_order_relaxed);
                g_smany_a_ok.store(a == SMANY_A, std::memory_order_relaxed);
                g_smany_b_ok.store(b == SMANY_B, std::memory_order_relaxed);
                g_smany_c_ok.store(c == SMANY_C, std::memory_order_relaxed);
                g_smany_d_ok.store(d == "swide", std::memory_order_relaxed);
                g_smany_e_ok.store(e == SMANY_E, std::memory_order_relaxed);
                g_smany_f_ok.store(f == SMANY_F, std::memory_order_relaxed);
            }) };

        ctx.check("static_many_scoped_hook_installed", handle.installed());

        vmhook::hotspot::method* const m{
            find_method("staticManyArgs", "(JIDLjava/lang/String;IJ)J") };
        bool done{ false };
        drive_until_fires(ctx, 22, m, 1, 6, done);
        ctx.check("static_many_probe_completed", done);

        ctx.check("static_many_fired_exactly_once", g_fire_count.load() == 1);
        ctx.check("static_many_arg_a_long_slot0_wide_first", g_smany_a_ok.load());
        ctx.check("static_many_arg_b_int_after_long", g_smany_b_ok.load());
        ctx.check("static_many_arg_c_double_after_int", g_smany_c_ok.load());
        ctx.check("static_many_arg_d_string_after_double", g_smany_d_ok.load());
        ctx.check("static_many_arg_e_int_after_ref", g_smany_e_ok.load());
        ctx.check("static_many_arg_f_trailing_long", g_smany_f_ok.load());
        ctx.check("static_many_allow_through_result",
                  hook_basic_fixture::get_static_many_args_result()
                      == SMANY_A + SMANY_B + static_cast<std::int64_t>(SMANY_C)
                         + SMANY_D_LEN + SMANY_E + SMANY_F);
    }

    // =====================================================================
    // Scenario 23 — boundaryCombine(int,long,int) with EXTREME primitive values
    //               (Integer.MIN_VALUE, Long.MIN_VALUE, Integer.MAX_VALUE): the
    //               decoder must reproduce the exact two's-complement bit patterns
    //               at the int and long edges.  Self-less body; allow-through.
    // =====================================================================
    {
        auto handle{ vmhook::scoped_hook<hook_basic_fixture>(
            "boundaryCombine", "(IJI)J",
            [](vmhook::return_value&,
               const std::unique_ptr<hook_basic_fixture>&,
               std::int32_t a, std::int64_t b, std::int32_t c)
            {
                g_fire_count.fetch_add(1, std::memory_order_relaxed);
                g_bound_a.store(a, std::memory_order_relaxed);
                g_bound_b.store(b, std::memory_order_relaxed);
                g_bound_c.store(c, std::memory_order_relaxed);
            }) };

        ctx.check("boundary_extreme_scoped_hook_installed", handle.installed());

        vmhook::hotspot::method* const m{ find_method("boundaryCombine", "(IJI)J") };
        bool done{ false };
        drive_until_fires(ctx, 23, m, 1, 6, done);
        ctx.check("boundary_extreme_probe_completed", done);

        ctx.check("boundary_extreme_fired_exactly_once", g_fire_count.load() == 1);
        ctx.check("boundary_extreme_int_min_decoded", g_bound_a.load() == INT_MIN_J);
        ctx.check("boundary_extreme_long_min_decoded", g_bound_b.load() == LONG_MIN_J);
        ctx.check("boundary_extreme_int_max_after_long_decoded",
                  g_bound_c.load() == INT_MAX_J);
        // Compute the wrapping sum in DEFINED unsigned arithmetic (two's-complement
        // wrap), matching Java's defined long overflow — a signed int64 sum here
        // overflows (INT_MIN + LONG_MIN underflows) which is C++ UB and folds
        // compiler-dependently, diverging from the Java side.
        const std::uint64_t boundary_expected{
            static_cast<std::uint64_t>(static_cast<std::int64_t>(INT_MIN_J))
          + static_cast<std::uint64_t>(LONG_MIN_J)
          + static_cast<std::uint64_t>(static_cast<std::int64_t>(INT_MAX_J)) };
        ctx.check("boundary_extreme_allow_through_result",
                  hook_basic_fixture::get_boundary_combine_result()
                      == static_cast<std::int64_t>(boundary_expected));
    }

    // =====================================================================
    // Scenario 24 — boundaryCombine again with an ALL-ONES long (-1L) between
    //               Integer.MAX_VALUE and Integer.MIN_VALUE: -1L is 0xFFFF...FF,
    //               so every bit of the wide slot pair is set — a high-word decode
    //               bug (reading only the low 32 bits) would surface here.
    // =====================================================================
    {
        auto handle{ vmhook::scoped_hook<hook_basic_fixture>(
            "boundaryCombine", "(IJI)J",
            [](vmhook::return_value&,
               const std::unique_ptr<hook_basic_fixture>&,
               std::int32_t a, std::int64_t b, std::int32_t c)
            {
                g_fire_count.fetch_add(1, std::memory_order_relaxed);
                g_bound_a.store(a, std::memory_order_relaxed);
                g_bound_b.store(b, std::memory_order_relaxed);
                g_bound_c.store(c, std::memory_order_relaxed);
            }) };

        ctx.check("boundary_allones_scoped_hook_installed", handle.installed());

        vmhook::hotspot::method* const m{ find_method("boundaryCombine", "(IJI)J") };
        bool done{ false };
        drive_until_fires(ctx, 24, m, 1, 6, done);
        ctx.check("boundary_allones_probe_completed", done);

        ctx.check("boundary_allones_fired_exactly_once", g_fire_count.load() == 1);
        ctx.check("boundary_allones_int_max_decoded", g_bound_a.load() == INT_MAX_J);
        ctx.check("boundary_allones_minus1_long_decoded", g_bound_b.load() == -1LL);
        ctx.check("boundary_allones_high_word_all_set",
                  (g_bound_b.load() >> 32) == -1LL);
        ctx.check("boundary_allones_int_min_after_long_decoded",
                  g_bound_c.load() == INT_MIN_J);
        ctx.check("boundary_allones_allow_through_result",
                  hook_basic_fixture::get_boundary_combine_result()
                      == static_cast<std::int64_t>(INT_MAX_J) + (-1LL) + INT_MIN_J);
    }

    // =====================================================================
    // Scenario 25 — INSTANCE floatArg(float,int): a FLOAT argument (1-slot, a
    //               distinct decode lane from int — the low 32 bits of the slot
    //               hold the IEEE-754 single) followed by a trailing int.  Value
    //               chosen exact in IEEE-754 so the equality is bit-exact.
    // =====================================================================
    {
        auto handle{ vmhook::scoped_hook<hook_basic_fixture>(
            "floatArg", "(FI)D",
            [](vmhook::return_value&,
               const std::unique_ptr<hook_basic_fixture>& self,
               float f, std::int32_t i)
            {
                g_fire_count.fetch_add(1, std::memory_order_relaxed);
                g_float_self_ok.store(self != nullptr && self->seed() == 0,
                                      std::memory_order_relaxed);
                g_float_f_ok.store(f == FLOAT_ARG, std::memory_order_relaxed);
                g_float_i_ok.store(i == FLOAT_TRAILING_I, std::memory_order_relaxed);
            }) };

        ctx.check("float_scoped_hook_installed", handle.installed());

        vmhook::hotspot::method* const m{ find_method("floatArg", "(FI)D") };
        bool done{ false };
        drive_until_fires(ctx, 25, m, 1, 6, done);
        ctx.check("float_probe_completed", done);

        ctx.check("float_fired_exactly_once", g_fire_count.load() == 1);
        ctx.check("float_self_correct", g_float_self_ok.load());
        ctx.check("float_arg_decoded", g_float_f_ok.load());
        ctx.check("float_trailing_int_after_float_decoded", g_float_i_ok.load());
        // allow-through: body returns (double)f + i.
        ctx.check("float_allow_through_result",
                  hook_basic_fixture::get_float_arg_result()
                      == static_cast<float>(static_cast<double>(FLOAT_ARG) + FLOAT_TRAILING_I));
    }

    // =====================================================================
    // Scenario 26 — INSTANCE nullRefArgs(String,HookBasic): BOTH reference args
    //               passed null.  A null String arg decodes through read_java_string
    //               to an EMPTY std::string (not a crash); a null object arg decodes
    //               to a null unique_ptr.  Body confirms both were null (allow-through).
    // =====================================================================
    {
        auto handle{ vmhook::scoped_hook<hook_basic_fixture>(
            "nullRefArgs", "(Ljava/lang/String;Lvmhook/fixtures/HookBasic;)Z",
            [](vmhook::return_value&,
               const std::unique_ptr<hook_basic_fixture>&,
               const std::string& s, const std::unique_ptr<hook_basic_fixture>& obj)
            {
                g_fire_count.fetch_add(1, std::memory_order_relaxed);
                g_nullref_string_empty.store(s.empty(), std::memory_order_relaxed);
                g_nullref_obj_null.store(vmhook_test::no_object(obj), std::memory_order_relaxed);
            }) };

        ctx.check("nullref_scoped_hook_installed", handle.installed());

        vmhook::hotspot::method* const m{
            find_method("nullRefArgs", "(Ljava/lang/String;Lvmhook/fixtures/HookBasic;)Z") };
        bool done{ false };
        drive_until_fires(ctx, 26, m, 1, 6, done);
        ctx.check("nullref_probe_completed", done);

        ctx.check("nullref_fired_exactly_once", g_fire_count.load() == 1);
        ctx.check("nullref_null_string_decodes_empty", g_nullref_string_empty.load());
        ctx.check("nullref_null_object_decodes_null", g_nullref_obj_null.load());
        // allow-through: body saw both null too.
        ctx.check("nullref_allow_through_both_null",
                  hook_basic_fixture::get_null_ref_args_both_null());
    }

    // =====================================================================
    // Scenario 27 — INSTANCE objArg(HookBasic,int): a REAL object-wrapper arg
    //               distinct from `this`.  The detour decodes `other` and reads
    //               ITS seed — proving an object ARGUMENT (not the receiver) is
    //               decoded to the correct instance.  Allow-through.
    // =====================================================================
    {
        auto handle{ vmhook::scoped_hook<hook_basic_fixture>(
            "objArg", "(Lvmhook/fixtures/HookBasic;I)I",
            [](vmhook::return_value&,
               const std::unique_ptr<hook_basic_fixture>& self,
               const std::unique_ptr<hook_basic_fixture>& other, std::int32_t delta)
            {
                g_fire_count.fetch_add(1, std::memory_order_relaxed);
                // self is the receiver (seed 0); other is the passed-in object.
                g_objarg_other_nonnull.store(
                    self != nullptr && self->seed() == 0 && other != nullptr,
                    std::memory_order_relaxed);
                g_objarg_other_seed.store(other != nullptr ? other->seed() : -1,
                                          std::memory_order_relaxed);
                g_objarg_delta.store(delta, std::memory_order_relaxed);
            }) };

        ctx.check("objarg_scoped_hook_installed", handle.installed());

        vmhook::hotspot::method* const m{
            find_method("objArg", "(Lvmhook/fixtures/HookBasic;I)I") };
        bool done{ false };
        drive_until_fires(ctx, 27, m, 1, 6, done);
        ctx.check("objarg_probe_completed", done);

        ctx.check("objarg_fired_exactly_once", g_fire_count.load() == 1);
        ctx.check("objarg_other_nonnull_and_self_is_receiver",
                  g_objarg_other_nonnull.load());
        ctx.check("objarg_other_seed_is_correct_object",
                  g_objarg_other_seed.load() == OBJ_ARG_SEED);
        ctx.check("objarg_delta_after_object_decoded",
                  g_objarg_delta.load() == OBJ_ARG_DELTA);
        // allow-through: body returns other.seed + delta.
        ctx.check("objarg_allow_through_result",
                  hook_basic_fixture::get_obj_arg_result() == (OBJ_ARG_SEED + OBJ_ARG_DELTA));
    }

    // =====================================================================
    // Scenario 28 — TWO DISTINCT hooks coexisting: touch() (instance) AND
    //               staticTouch() (static) are both installed, then ONE dispatch
    //               calls each exactly once.  Both detours must fire in the SAME
    //               probe cycle, each decoding ITS OWN arg from ITS OWN frame —
    //               proving multiple hooks on different methods (sharing the one
    //               patched i2i stub) each fire for their method only.
    // =====================================================================
    {
        auto h_instance{ vmhook::scoped_hook<hook_basic_fixture>(
            "touch", "(I)I",
            [](vmhook::return_value&,
               const std::unique_ptr<hook_basic_fixture>& self,
               std::int32_t delta)
            {
                g_coexist_instance_fires.fetch_add(1, std::memory_order_relaxed);
                g_fire_count.fetch_add(1, std::memory_order_relaxed);
                if (self != nullptr && self->seed() == PRIMARY_SEED)
                {
                    g_coexist_instance_delta.store(delta, std::memory_order_relaxed);
                }
            }) };
        auto h_static{ vmhook::scoped_hook<hook_basic_fixture>(
            "staticTouch", "(I)I",
            [](vmhook::return_value&, std::int32_t delta)
            {
                g_coexist_static_fires.fetch_add(1, std::memory_order_relaxed);
                g_fire_count.fetch_add(1, std::memory_order_relaxed);
                g_coexist_static_delta.store(delta, std::memory_order_relaxed);
            }) };

        ctx.check("coexist_instance_hook_installed", h_instance.installed());
        ctx.check("coexist_static_hook_installed", h_static.installed());

        // Settle BOTH methods to the interpreter route before driving.
        vmhook::hotspot::method* const m_instance{ find_method("touch", "(I)I") };
        vmhook::hotspot::method* const m_static{ find_method("staticTouch", "(I)I") };
        bool done{ false };
        for (int attempt{ 0 }; attempt < 6; ++attempt)
        {
            (void)settle_interpreter_route(m_instance, 12);
            (void)settle_interpreter_route(m_static, 12);
            done = drive(ctx, 28);
            if (done && g_coexist_instance_fires.load() == 1
                     && g_coexist_static_fires.load() == 1)
            {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{ 25 });
        }
        ctx.check("coexist_probe_completed", done);

        // Both methods are COLD single-dispatch in this probe -> each fires once.
        ctx.check("coexist_instance_fired", g_coexist_instance_fires.load() >= 1);
        ctx.check("coexist_instance_not_doubled",
                  g_coexist_instance_fires.load() <= 1);
        ctx.check("coexist_static_fired", g_coexist_static_fires.load() >= 1);
        ctx.check("coexist_static_not_doubled",
                  g_coexist_static_fires.load() <= 1);
        // Each detour decoded ITS OWN method's arg (not the other's).
        ctx.check("coexist_instance_decoded_own_arg",
                  g_coexist_instance_delta.load() == COEXIST_INSTANCE_DELTA);
        ctx.check("coexist_static_decoded_own_arg",
                  g_coexist_static_delta.load() == COEXIST_STATIC_DELTA);
        ctx.check("coexist_args_are_distinct",
                  g_coexist_instance_delta.load() != g_coexist_static_delta.load());
        // allow-through on both methods.
        ctx.check("coexist_instance_allow_through",
                  hook_basic_fixture::get_coexist_instance_result()
                      == (PRIMARY_SEED + COEXIST_INSTANCE_DELTA));
        ctx.check("coexist_static_allow_through",
                  hook_basic_fixture::get_coexist_static_result()
                      == (COEXIST_STATIC_DELTA * 2));
    }
    }
    catch (const std::exception& ex)
    {
        ctx.record(std::string{ "[INFO] hook_basic: scenario body threw - " }
                   + ex.what() + " (recorded, not a crash).");
    }
    catch (...)
    {
        ctx.record("[INFO] hook_basic: scenario body threw a non-std exception "
                   "(recorded, not a crash).");
    }

    // Belt-and-braces: every scoped_hook above already tears its hook down on
    // scope exit, but a mid-scenario throw could leave one armed.  This
    // unconditional teardown (OUTSIDE the try) guarantees NO hook is left armed
    // for the modules that run after us.
    vmhook::shutdown_hooks();
}
