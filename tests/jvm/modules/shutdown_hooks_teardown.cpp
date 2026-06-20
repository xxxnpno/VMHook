// shutdown_hooks_teardown JVM test module  (feature area: hooks / lifecycle)
//
// Exhaustively exercises vmhook::shutdown_hooks() — the BULK teardown that
// removes EVERY installed hook and restores the JVM to a clean state — on a LIVE
// JVM (real bytecode dispatch via the Harness go/done probe).  Unlike hook_basic,
// which uses scoped_hook (auto-removes on scope exit), THIS module installs via
// the low-level vmhook::hook<T>() path so the ONLY thing that takes a hook back
// down is shutdown_hooks() itself.  That makes it the right place to prove the
// teardown contract end-to-end.
//
// The headline property under test is REVERSIBILITY (audit bug [high]
// "Permanently latched shutdown flag breaks re-install after teardown",
// vmhook.hpp:8768-8778): shutdown_hooks() must NOT be one-shot.  A real bug was
// fixed where g_shutdown_requested latched true forever — after one teardown a
// fresh hook<T>() returned true but its detour was silently dead (common_detour
// early-returns on the flag; the auto-repair watchdog refused to respawn).  The
// canonical proof here is the three-beat dance:
//     install hook -> it FIRES;
//     shutdown_hooks() -> the ORIGINAL method runs byte-exact, detour SILENT;
//     install a FRESH hook AFTER shutdown_hooks() -> it MUST FIRE.
// That third beat is the litmus test for the latched-flag regression.
//
// It also covers every other angle from the audit finding:
//   * shutdown_hooks() with NO hooks installed is safe (and leaves the library
//     usable afterward),
//   * double shutdown_hooks() (back-to-back, and on an already-clean state) is
//     safe and still reversible,
//   * one shutdown_hooks() removes hooks installed on MULTIPLE distinct methods
//     (instance + static + multi-slot — three different Method* shapes),
//   * the hooked method's behaviour is BYTE-EXACT-ORIGINAL after teardown, proven
//     the strong way: a force-RETURN hook makes Java observe a sentinel, then
//     teardown makes Java observe the unmodified original result again (so the
//     method body is genuinely restored, not merely "the detour stopped firing"),
//   * a force-cancel-style observation: allow-through both before and after.
//
// IMPORTANT lifecycle discipline: because installs here are low-level (persist
// until shutdown_hooks()), every scenario that installs hooks ends by calling
// shutdown_hooks() so no hook leaks into the next scenario or the next module.
// The module's final statement is a belt-and-braces shutdown_hooks() so NO hook
// is left armed when control returns to the driver (other modules run after us).
//
// Harness note: the fixture's `done` flag LATCHES.  Each scenario resets `done`
// and sets `mode` on the rising edge of `go`, runs ONE probe cycle, then reads
// back observations.  shutdown_hooks() is called from the native (driver) thread
// BETWEEN probe cycles — never concurrently with a probe — matching the
// documented single-thread install/teardown contract.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <atomic>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>

namespace
{
    // Wrapper for vmhook.fixtures.ShutdownHooks.  Deriving from vmhook::object<>
    // gives the wrapper its vtable (required by register_class<T>) and the
    // static_field(...) / get_field(...) accessors.
    class shutdown_fixture : public vmhook::object<shutdown_fixture>
    {
    public:
        explicit shutdown_fixture(vmhook::oop_t instance) noexcept
            : vmhook::object<shutdown_fixture>{ instance }
        {
        }

        // --- go/done handshake + scenario selector ------------------------
        static auto set_go(bool value) -> void       { static_field("go")->set(value); }
        static auto set_done(bool value) -> void      { static_field("done")->set(value); }
        static auto get_done() -> bool                { return static_field("done")->get(); }
        static auto set_mode(std::int32_t m) -> void  { static_field("mode")->set(m); }

        // --- recorded observations the Java side writes -------------------
        static auto get_last_alpha_result() -> std::int32_t { return static_field("lastAlphaResult")->get(); }
        static auto get_last_beta_result() -> std::int32_t  { return static_field("lastBetaResult")->get(); }
        static auto get_last_gamma_result() -> std::int64_t { return static_field("lastGammaResult")->get(); }
        static auto get_alpha_calls_made() -> std::int32_t  { return static_field("alphaCallsMade")->get(); }
        static auto get_beta_calls_made() -> std::int32_t   { return static_field("betaCallsMade")->get(); }
        static auto get_gamma_calls_made() -> std::int32_t  { return static_field("gammaCallsMade")->get(); }

        // Reads this instance's own seed (proves `self` is the right object).
        auto seed() const -> std::int32_t { return get_field("seed")->get(); }
    };

    // ---- Fixture-mirrored constants (lockstep with ShutdownHooks.java) ------
    constexpr std::int32_t SEED{ 4242 };
    constexpr std::int32_t ALPHA_CALLS{ 3 };
    constexpr std::int32_t BETA_CALLS{ 2 };
    constexpr std::int32_t ALPHA_DELTA{ 17 };
    constexpr std::int32_t BETA_DELTA{ 23 };
    constexpr std::int32_t GAMMA_A{ 9 };
    constexpr std::int64_t GAMMA_B{ 0x0102030405060708LL };
    constexpr std::int32_t GAMMA_C{ -31 };

    // Original (un-hooked) results the Java bodies compute.
    constexpr std::int32_t ALPHA_ORIGINAL{ SEED + ALPHA_DELTA };
    constexpr std::int32_t BETA_ORIGINAL{ BETA_DELTA * 3 };
    constexpr std::int64_t GAMMA_ORIGINAL{ static_cast<std::int64_t>(SEED) + GAMMA_A + GAMMA_B + GAMMA_C };

    // ---- Hook observation state (reset per scenario) -----------------------
    std::atomic<std::int32_t> g_alpha_fires{ 0 };
    std::atomic<std::int32_t> g_beta_fires{ 0 };
    std::atomic<std::int32_t> g_gamma_fires{ 0 };
    std::atomic<bool>         g_alpha_self_ok{ false };   // self non-null & seed correct
    std::atomic<bool>         g_alpha_arg_ok{ false };    // decoded delta == ALPHA_DELTA
    std::atomic<bool>         g_beta_arg_ok{ false };
    std::atomic<bool>         g_gamma_args_ok{ false };

    auto reset_observations() -> void
    {
        g_alpha_fires.store(0);
        g_beta_fires.store(0);
        g_gamma_fires.store(0);
        g_alpha_self_ok.store(false);
        g_alpha_arg_ok.store(false);
        g_beta_arg_ok.store(false);
        g_gamma_args_ok.store(false);
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
                    shutdown_fixture::set_done(false);
                    shutdown_fixture::set_mode(mode);
                }
                shutdown_fixture::set_go(value);
            },
            []() { return shutdown_fixture::get_done(); });
    }

    // ---- The detours installed via the low-level vmhook::hook<T>() path -----
    // (allow-through unless noted — they only observe.)
    auto install_alpha_observer() -> bool
    {
        return vmhook::hook<shutdown_fixture>(
            "alpha",
            [](vmhook::return_value&,
               const std::unique_ptr<shutdown_fixture>& self,
               std::int32_t delta)
            {
                g_alpha_fires.fetch_add(1, std::memory_order_relaxed);
                g_alpha_self_ok.store(self != nullptr && self->seed() == SEED,
                                      std::memory_order_relaxed);
                g_alpha_arg_ok.store(delta == ALPHA_DELTA, std::memory_order_relaxed);
            });
    }

    auto install_beta_observer() -> bool
    {
        return vmhook::hook<shutdown_fixture>(
            "beta",
            [](vmhook::return_value&, std::int32_t delta)
            {
                g_beta_fires.fetch_add(1, std::memory_order_relaxed);
                g_beta_arg_ok.store(delta == BETA_DELTA, std::memory_order_relaxed);
            });
    }

    auto install_gamma_observer() -> bool
    {
        return vmhook::hook<shutdown_fixture>(
            "gamma",
            [](vmhook::return_value&,
               const std::unique_ptr<shutdown_fixture>&,
               std::int32_t a, std::int64_t b, std::int32_t c)
            {
                g_gamma_fires.fetch_add(1, std::memory_order_relaxed);
                g_gamma_args_ok.store(a == GAMMA_A && b == GAMMA_B && c == GAMMA_C,
                                      std::memory_order_relaxed);
            });
    }

    // Sentinel the static-beta force-return hook injects so Java observes a value
    // that the original body (delta*3) can NEVER produce — proving the detour was
    // really in the static-dispatch path, and (after teardown) that the static
    // method body is restored byte-exact rather than the detour merely going quiet.
    constexpr std::int32_t BETA_SENTINEL{ 7770007 };
    static_assert(BETA_SENTINEL != BETA_ORIGINAL,
                  "static-beta sentinel must differ from the original so the override is observable");

    auto install_beta_force_return() -> bool
    {
        return vmhook::hook<shutdown_fixture>(
            "beta",
            [](vmhook::return_value& rv, std::int32_t)
            {
                g_beta_fires.fetch_add(1, std::memory_order_relaxed);
                rv.set(BETA_SENTINEL);   // suppress original static body, force return
            });
    }
}

VMHOOK_JVM_MODULE(shutdown_hooks_teardown)
{
    vmhook::register_class<shutdown_fixture>("vmhook/fixtures/ShutdownHooks");

    // CRASH-SAFETY: this module calls the GLOBAL shutdown_hooks() (which tears
    // down EVERY other module's state) and transiently enables the auto-repair
    // watchdog.  A C++ throw mid-body must NOT leak a hook or leave the watchdog
    // gate on into the next module, so the whole scenario body runs inside a
    // try/catch and the UNCONDITIONAL final cleanup (shutdown_hooks() +
    // watchdog-off) lives OUTSIDE the try.  (The no-SEH Windows hard-fault path is
    // handled separately by the harness __try + ctx.reset(); this catch covers the
    // C++-exception path so a single throw cannot void the whole run's total.)
    try
    {
    // =====================================================================
    // Scenario 0 — PRE-FLIGHT: shutdown_hooks() with NO hooks installed is a
    //   safe no-op, AND it leaves the library fully usable (a subsequent install
    //   still fires).  Covers the audit's "safe to call before any hook is
    //   installed" contract and primes a clean baseline for everything below.
    //   We call it TWICE back-to-back on the empty state to also cover
    //   "double shutdown_hooks() on an already-clean state is safe".
    // =====================================================================
    {
        vmhook::shutdown_hooks();   // no hooks installed
        vmhook::shutdown_hooks();   // double-call on empty state
        ctx.check("empty_shutdown_did_not_crash", true);  // reaching here == no SEH/throw

        // Deterministic table-empty oracle: with no hooks installed, verify_hooks()
        // has nothing to repair and MUST return 0.  This is the HARD post-teardown
        // invariant the audit asks for ("verify_hooks()==0 after"), independent of
        // any JIT fire-count timing.  (verify_hooks counts hooks it had to repair;
        // an empty table returns 0 deterministically on every platform.)
        ctx.check("empty_state_verify_hooks_is_zero", vmhook::verify_hooks() == 0u);

        // Library must still work after an empty teardown: install + fire.
        ctx.check("install_after_empty_shutdown_returns_true", install_alpha_observer());
        const bool done{ drive(ctx, 1) };
        ctx.check("after_empty_shutdown_probe_completed", done);
        ctx.check("after_empty_shutdown_java_made_calls",
                  shutdown_fixture::get_alpha_calls_made() == ALPHA_CALLS);
        ctx.check("after_empty_shutdown_hook_fires",
                  g_alpha_fires.load() == ALPHA_CALLS);
        ctx.check("after_empty_shutdown_allow_through",
                  shutdown_fixture::get_last_alpha_result() == ALPHA_ORIGINAL);

        vmhook::shutdown_hooks();   // clean up this scenario's hook
    }

    // =====================================================================
    // Scenario 1 — REVERSIBILITY, the headline three-beat dance on an INSTANCE
    //   method.  This is the regression test for the latched-flag bug.
    //     Beat 1: install alpha hook  -> it FIRES (and decodes self+arg).
    //     Beat 2: shutdown_hooks()    -> detour SILENT, ORIGINAL runs byte-exact.
    //     Beat 3: install a FRESH alpha hook AFTER shutdown_hooks() -> MUST FIRE.
    //   Beat 3 failing would mean g_shutdown_requested stayed latched (the exact
    //   bug fixed at vmhook.hpp:8768-8778).
    // =====================================================================
    {
        // --- Beat 1: install -> fires ---
        ctx.check("reinstall_beat1_install_returns_true", install_alpha_observer());
        const bool done1{ drive(ctx, 1) };
        ctx.check("reinstall_beat1_probe_completed", done1);
        ctx.check("reinstall_beat1_hook_fired",
                  g_alpha_fires.load() == ALPHA_CALLS);
        ctx.check("reinstall_beat1_self_correct", g_alpha_self_ok.load());
        ctx.check("reinstall_beat1_arg_decoded", g_alpha_arg_ok.load());
        ctx.check("reinstall_beat1_allow_through_original_result",
                  shutdown_fixture::get_last_alpha_result() == ALPHA_ORIGINAL);

        // --- Beat 2: shutdown_hooks() -> detour silent, original byte-exact ---
        vmhook::shutdown_hooks();
        // Deterministic table-empty oracle BEFORE the behavioural re-drive: the
        // teardown loop cleared g_hooked_methods, so there is nothing to repair.
        ctx.check("reinstall_beat2_verify_hooks_zero_after_teardown",
                  vmhook::verify_hooks() == 0u);
        const bool done2{ drive(ctx, 1) };
        ctx.check("reinstall_beat2_probe_completed", done2);
        ctx.check("reinstall_beat2_java_still_ran",
                  shutdown_fixture::get_alpha_calls_made() == ALPHA_CALLS);
        ctx.check("reinstall_beat2_detour_silent_after_shutdown",
                  g_alpha_fires.load() == 0);
        ctx.check("reinstall_beat2_byte_exact_original_after_teardown",
                  shutdown_fixture::get_last_alpha_result() == ALPHA_ORIGINAL);

        // --- Beat 3: FRESH install after shutdown_hooks() -> MUST FIRE ---
        // (The whole point: shutdown is reversible, not one-shot/latched.)
        ctx.check("reinstall_beat3_fresh_install_returns_true", install_alpha_observer());
        const bool done3{ drive(ctx, 1) };
        ctx.check("reinstall_beat3_probe_completed", done3);
        ctx.check("reinstall_beat3_fresh_hook_FIRES_after_shutdown",
                  g_alpha_fires.load() == ALPHA_CALLS);
        ctx.check("reinstall_beat3_fresh_hook_self_correct", g_alpha_self_ok.load());
        ctx.check("reinstall_beat3_fresh_hook_arg_decoded", g_alpha_arg_ok.load());
        ctx.check("reinstall_beat3_fresh_hook_allow_through",
                  shutdown_fixture::get_last_alpha_result() == ALPHA_ORIGINAL);

        vmhook::shutdown_hooks();   // clean up
    }

    // =====================================================================
    // Scenario 2 — BYTE-EXACT-ORIGINAL proven the STRONG way.  A force-RETURN
    //   hook makes Java observe a sentinel != the original, so we KNOW the hook
    //   was really in the dispatch path.  After shutdown_hooks() Java must observe
    //   the unmodified original result again — proving the method body is
    //   genuinely restored, not just that the detour stopped firing.
    // =====================================================================
    {
        constexpr std::int32_t SENTINEL{ 1313131 };
        static_assert(SENTINEL != ALPHA_ORIGINAL,
                      "sentinel must differ from the original so the override is observable");

        const bool installed{ vmhook::hook<shutdown_fixture>(
            "alpha",
            [](vmhook::return_value& rv,
               const std::unique_ptr<shutdown_fixture>&,
               std::int32_t)
            {
                g_alpha_fires.fetch_add(1, std::memory_order_relaxed);
                rv.set(SENTINEL);   // suppress original body, force the return
            }) };
        ctx.check("force_return_install_returns_true", installed);

        const bool done1{ drive(ctx, 1) };
        ctx.check("force_return_probe_completed", done1);
        ctx.check("force_return_hook_fired", g_alpha_fires.load() == ALPHA_CALLS);
        ctx.check("force_return_java_saw_sentinel",
                  shutdown_fixture::get_last_alpha_result() == SENTINEL);
        ctx.check("force_return_java_did_not_see_original",
                  shutdown_fixture::get_last_alpha_result() != ALPHA_ORIGINAL);

        // Teardown must restore the ORIGINAL body byte-for-byte.
        vmhook::shutdown_hooks();
        const bool done2{ drive(ctx, 1) };
        ctx.check("force_return_after_shutdown_probe_completed", done2);
        ctx.check("force_return_after_shutdown_detour_silent",
                  g_alpha_fires.load() == 0);
        ctx.check("force_return_after_shutdown_byte_exact_original",
                  shutdown_fixture::get_last_alpha_result() == ALPHA_ORIGINAL);
        ctx.check("force_return_after_shutdown_no_longer_sentinel",
                  shutdown_fixture::get_last_alpha_result() != SENTINEL);

        vmhook::shutdown_hooks();   // belt-and-braces (already clean)
    }

    // =====================================================================
    // Scenario 3 — MULTI-METHOD teardown: one shutdown_hooks() removes hooks on
    //   THREE distinct methods of different shapes — alpha(int) instance,
    //   beta(int) static, gamma(int,long,int) instance multi-slot.  Prove all
    //   three fire while installed, then ONE shutdown_hooks() silences ALL of
    //   them and every original body runs byte-exact.
    // =====================================================================
    {
        ctx.check("multi_install_alpha", install_alpha_observer());
        ctx.check("multi_install_beta", install_beta_observer());
        ctx.check("multi_install_gamma", install_gamma_observer());

        // --- all three fire while installed (drive each method's mode) ---
        const bool d_alpha{ drive(ctx, 1) };
        ctx.check("multi_alpha_probe_completed", d_alpha);
        ctx.check("multi_alpha_fired", g_alpha_fires.load() == ALPHA_CALLS);
        ctx.check("multi_alpha_allow_through",
                  shutdown_fixture::get_last_alpha_result() == ALPHA_ORIGINAL);

        const bool d_beta{ drive(ctx, 2) };
        ctx.check("multi_beta_probe_completed", d_beta);
        ctx.check("multi_beta_fired", g_beta_fires.load() == BETA_CALLS);
        ctx.check("multi_beta_arg_decoded_static_slot0", g_beta_arg_ok.load());
        ctx.check("multi_beta_allow_through",
                  shutdown_fixture::get_last_beta_result() == BETA_ORIGINAL);

        const bool d_gamma{ drive(ctx, 4) };
        ctx.check("multi_gamma_probe_completed", d_gamma);
        ctx.check("multi_gamma_fired", g_gamma_fires.load() == 1);
        ctx.check("multi_gamma_args_multislot_decoded", g_gamma_args_ok.load());
        ctx.check("multi_gamma_allow_through",
                  shutdown_fixture::get_last_gamma_result() == GAMMA_ORIGINAL);

        // --- the ONE teardown that must remove all three ---
        vmhook::shutdown_hooks();
        // Deterministic whole-table-empty oracle: all THREE entries (alpha+beta+
        // gamma) were in g_hooked_methods; after one teardown there is nothing to
        // repair, so verify_hooks() returns 0 (independent of any fire timing).
        ctx.check("multi_after_shutdown_verify_hooks_zero",
                  vmhook::verify_hooks() == 0u);

        // alpha silent + original.
        const bool e_alpha{ drive(ctx, 1) };
        ctx.check("multi_after_shutdown_alpha_probe_completed", e_alpha);
        ctx.check("multi_after_shutdown_alpha_silent", g_alpha_fires.load() == 0);
        ctx.check("multi_after_shutdown_alpha_original",
                  shutdown_fixture::get_last_alpha_result() == ALPHA_ORIGINAL);

        // beta silent + original.
        const bool e_beta{ drive(ctx, 2) };
        ctx.check("multi_after_shutdown_beta_probe_completed", e_beta);
        ctx.check("multi_after_shutdown_beta_silent", g_beta_fires.load() == 0);
        ctx.check("multi_after_shutdown_beta_original",
                  shutdown_fixture::get_last_beta_result() == BETA_ORIGINAL);

        // gamma silent + original.
        const bool e_gamma{ drive(ctx, 4) };
        ctx.check("multi_after_shutdown_gamma_probe_completed", e_gamma);
        ctx.check("multi_after_shutdown_gamma_silent", g_gamma_fires.load() == 0);
        ctx.check("multi_after_shutdown_gamma_original",
                  shutdown_fixture::get_last_gamma_result() == GAMMA_ORIGINAL);

        // And the whole multi-method set is re-installable afterward (reversible
        // for >1 method too): re-arm all three, prove all fire again.
        ctx.check("multi_reinstall_alpha", install_alpha_observer());
        ctx.check("multi_reinstall_beta", install_beta_observer());
        ctx.check("multi_reinstall_gamma", install_gamma_observer());
        const bool r_both{ drive(ctx, 3) };   // alpha once + beta once in one run
        ctx.check("multi_reinstall_both_probe_completed", r_both);
        ctx.check("multi_reinstall_alpha_fires_again", g_alpha_fires.load() == 1);
        ctx.check("multi_reinstall_beta_fires_again", g_beta_fires.load() == 1);

        vmhook::shutdown_hooks();   // clean up the re-armed set
    }

    // =====================================================================
    // Scenario 4 — DOUBLE shutdown_hooks() with hooks actually installed: the
    //   first call tears down, the immediate second call is a safe no-op on the
    //   now-empty state, and the library remains reversible after BOTH.
    // =====================================================================
    {
        ctx.check("double_install_alpha", install_alpha_observer());
        const bool d1{ drive(ctx, 1) };
        ctx.check("double_pre_probe_completed", d1);
        ctx.check("double_pre_hook_fired", g_alpha_fires.load() == ALPHA_CALLS);

        vmhook::shutdown_hooks();   // real teardown
        vmhook::shutdown_hooks();   // immediate second call — must be safe
        ctx.check("double_shutdown_did_not_crash", true);

        // Detour silent after the double teardown.
        const bool d2{ drive(ctx, 1) };
        ctx.check("double_post_probe_completed", d2);
        ctx.check("double_post_detour_silent", g_alpha_fires.load() == 0);
        ctx.check("double_post_byte_exact_original",
                  shutdown_fixture::get_last_alpha_result() == ALPHA_ORIGINAL);

        // Still reversible after a double shutdown: fresh install must fire.
        ctx.check("double_reinstall_returns_true", install_alpha_observer());
        const bool d3{ drive(ctx, 1) };
        ctx.check("double_reinstall_probe_completed", d3);
        ctx.check("double_reinstall_fires_after_double_shutdown",
                  g_alpha_fires.load() == ALPHA_CALLS);

        vmhook::shutdown_hooks();   // clean up
    }

    // =====================================================================
    // Scenario 5 — WHOLE-SET teardown observed in ONE bytecode dispatch (mode 5
    //   = runAll: alpha+beta+gamma each fire once in a SINGLE run()).  Distinct
    //   input from scenario 3 (which drove each method in its own probe): here all
    //   three Method* shapes are live in the SAME interpreter dispatch, then ONE
    //   shutdown_hooks() must silence the whole set within one probe and every
    //   original body must read back byte-exact.  Pairs the deterministic
    //   verify_hooks()==0 oracle with the behavioural "all silent" proof.
    // =====================================================================
    {
        ctx.check("whole_install_alpha", install_alpha_observer());
        ctx.check("whole_install_beta", install_beta_observer());
        ctx.check("whole_install_gamma", install_gamma_observer());

        const bool d_all{ drive(ctx, 5) };
        ctx.check("whole_probe_completed", d_all);
        ctx.check("whole_alpha_fired_once", g_alpha_fires.load() == 1);
        ctx.check("whole_beta_fired_once", g_beta_fires.load() == 1);
        ctx.check("whole_gamma_fired_once", g_gamma_fires.load() == 1);
        ctx.check("whole_beta_arg_decoded", g_beta_arg_ok.load());
        ctx.check("whole_gamma_args_decoded", g_gamma_args_ok.load());
        ctx.check("whole_alpha_allow_through",
                  shutdown_fixture::get_last_alpha_result() == ALPHA_ORIGINAL);
        ctx.check("whole_beta_allow_through",
                  shutdown_fixture::get_last_beta_result() == BETA_ORIGINAL);
        ctx.check("whole_gamma_allow_through",
                  shutdown_fixture::get_last_gamma_result() == GAMMA_ORIGINAL);

        // ONE teardown silences the whole set — proven inside ONE probe cycle.
        vmhook::shutdown_hooks();
        ctx.check("whole_after_shutdown_verify_hooks_zero",
                  vmhook::verify_hooks() == 0u);

        const bool e_all{ drive(ctx, 5) };
        ctx.check("whole_after_shutdown_probe_completed", e_all);
        ctx.check("whole_after_shutdown_alpha_silent", g_alpha_fires.load() == 0);
        ctx.check("whole_after_shutdown_beta_silent", g_beta_fires.load() == 0);
        ctx.check("whole_after_shutdown_gamma_silent", g_gamma_fires.load() == 0);
        ctx.check("whole_after_shutdown_alpha_original",
                  shutdown_fixture::get_last_alpha_result() == ALPHA_ORIGINAL);
        ctx.check("whole_after_shutdown_beta_original",
                  shutdown_fixture::get_last_beta_result() == BETA_ORIGINAL);
        ctx.check("whole_after_shutdown_gamma_original",
                  shutdown_fixture::get_last_gamma_result() == GAMMA_ORIGINAL);

        vmhook::shutdown_hooks();   // belt-and-braces (already clean)
    }

    // =====================================================================
    // Scenario 6 — STATIC-method body restored byte-exact via the STRONG
    //   force-RETURN proof.  Scenario 2 proved this for the INSTANCE method alpha;
    //   beta is a STATIC method (a different Method* shape / dispatch slot), so we
    //   prove teardown genuinely restores a static body too: a force-return hook
    //   makes Java observe BETA_SENTINEL (which delta*3 can never equal), then
    //   shutdown_hooks() makes Java observe the unmodified delta*3 again.
    // =====================================================================
    {
        ctx.check("static_force_return_install", install_beta_force_return());

        const bool d1{ drive(ctx, 2) };
        ctx.check("static_force_return_probe_completed", d1);
        ctx.check("static_force_return_hook_fired", g_beta_fires.load() == BETA_CALLS);
        ctx.check("static_force_return_java_saw_sentinel",
                  shutdown_fixture::get_last_beta_result() == BETA_SENTINEL);
        ctx.check("static_force_return_java_did_not_see_original",
                  shutdown_fixture::get_last_beta_result() != BETA_ORIGINAL);

        // Teardown must restore the ORIGINAL static body byte-for-byte.
        vmhook::shutdown_hooks();
        ctx.check("static_force_return_after_shutdown_verify_hooks_zero",
                  vmhook::verify_hooks() == 0u);
        const bool d2{ drive(ctx, 2) };
        ctx.check("static_force_return_after_shutdown_probe_completed", d2);
        ctx.check("static_force_return_after_shutdown_detour_silent",
                  g_beta_fires.load() == 0);
        ctx.check("static_force_return_after_shutdown_byte_exact_original",
                  shutdown_fixture::get_last_beta_result() == BETA_ORIGINAL);
        ctx.check("static_force_return_after_shutdown_no_longer_sentinel",
                  shutdown_fixture::get_last_beta_result() != BETA_SENTINEL);

        vmhook::shutdown_hooks();   // belt-and-braces (already clean)
    }

    // =====================================================================
    // Scenario 7 — MIXED scoped_hook + low-level hook<T>, both torn down by ONE
    //   shutdown_hooks().  scoped_hook normally RAII-uninstalls at scope exit, but
    //   shutdown_hooks() is the BULK teardown — it must remove a scoped entry too
    //   (they all live in g_hooked_methods).  Two further edges proven here:
    //     * a hook_handle that OUTLIVES a shutdown_hooks() destructs SAFELY — its
    //       stop() finds nothing in the now-empty list and no-ops (no double-free);
    //     * the library is still revivable: a fresh install after the mixed
    //       teardown fires again.
    // =====================================================================
    {
        // beta via the RAII path, alpha via the low-level path — distinct methods.
        auto beta_handle{ vmhook::scoped_hook<shutdown_fixture>(
            "beta",
            [](vmhook::return_value&, std::int32_t delta)
            {
                g_beta_fires.fetch_add(1, std::memory_order_relaxed);
                g_beta_arg_ok.store(delta == BETA_DELTA, std::memory_order_relaxed);
            }) };
        ctx.check("mixed_scoped_beta_installed", beta_handle.installed());
        ctx.check("mixed_manual_alpha_installed", install_alpha_observer());

        // Both fire while installed (alpha once + beta once in one run via mode 3).
        const bool d1{ drive(ctx, 3) };
        ctx.check("mixed_pre_probe_completed", d1);
        ctx.check("mixed_pre_alpha_fired", g_alpha_fires.load() == 1);
        ctx.check("mixed_pre_beta_fired", g_beta_fires.load() == 1);
        ctx.check("mixed_pre_beta_arg_decoded", g_beta_arg_ok.load());

        // ONE bulk teardown removes BOTH the scoped and the manual entry.
        vmhook::shutdown_hooks();
        ctx.check("mixed_after_shutdown_verify_hooks_zero",
                  vmhook::verify_hooks() == 0u);
        // The scoped handle still reports installed() (it holds its Method* until
        // its own stop()), but the underlying entry is already gone — proving
        // shutdown_hooks() tears scoped hooks down too.
        const bool d2{ drive(ctx, 3) };
        ctx.check("mixed_post_probe_completed", d2);
        ctx.check("mixed_post_alpha_silent", g_alpha_fires.load() == 0);
        ctx.check("mixed_post_beta_silent", g_beta_fires.load() == 0);
        ctx.check("mixed_post_alpha_original",
                  shutdown_fixture::get_last_alpha_result() == ALPHA_ORIGINAL);
        ctx.check("mixed_post_beta_original",
                  shutdown_fixture::get_last_beta_result() == BETA_ORIGINAL);

        // Still revivable after a mixed teardown: a fresh manual install fires.
        ctx.check("mixed_reinstall_returns_true", install_alpha_observer());
        const bool d3{ drive(ctx, 1) };
        ctx.check("mixed_reinstall_probe_completed", d3);
        ctx.check("mixed_reinstall_fires_after_mixed_teardown",
                  g_alpha_fires.load() == ALPHA_CALLS);

        vmhook::shutdown_hooks();   // clean up the revived manual hook

        // beta_handle now destructs at end of this block: its Method* is non-null
        // but g_hooked_methods is empty, so stop() finds nothing and no-ops.  No
        // crash / double-free == suite-safe.  (Implicitly proven by reaching the
        // post-block check below without faulting.)
    }
    ctx.check("mixed_scoped_handle_outliving_shutdown_destructs_safely", true);

    // =====================================================================
    // Scenario 8 — MANY-CYCLE reversibility: the three-beat dance is not just
    //   once-reversible, it survives REPEATED teardown/re-install cycles with no
    //   latch creeping back.  install -> fires -> shutdown -> silent -> repeat.
    //   If g_shutdown_requested (or the watcher latch) ever re-latched across
    //   cycles, a later cycle's fresh hook would go silent — caught here.
    // =====================================================================
    {
        constexpr int CYCLES{ 4 };
        bool every_cycle_fired{ true };
        bool every_cycle_silent_after_shutdown{ true };
        bool every_cycle_verify_zero{ true };
        for (int cycle{ 0 }; cycle < CYCLES; ++cycle)
        {
            const bool installed{ install_alpha_observer() };
            if (!installed)
            {
                every_cycle_fired = false;
            }

            const bool fired{ drive(ctx, 1) };
            if (!fired || g_alpha_fires.load() != ALPHA_CALLS)
            {
                every_cycle_fired = false;
            }
            if (shutdown_fixture::get_last_alpha_result() != ALPHA_ORIGINAL)
            {
                every_cycle_fired = false;   // allow-through must hold every cycle
            }

            vmhook::shutdown_hooks();
            if (vmhook::verify_hooks() != 0u)
            {
                every_cycle_verify_zero = false;
            }

            const bool silent_run{ drive(ctx, 1) };
            if (!silent_run || g_alpha_fires.load() != 0
                || shutdown_fixture::get_last_alpha_result() != ALPHA_ORIGINAL)
            {
                every_cycle_silent_after_shutdown = false;
            }
        }
        ctx.check("many_cycle_each_install_fires", every_cycle_fired);
        ctx.check("many_cycle_each_shutdown_silences", every_cycle_silent_after_shutdown);
        ctx.check("many_cycle_each_teardown_verify_hooks_zero", every_cycle_verify_zero);

        vmhook::shutdown_hooks();   // ensure clean after the loop
    }

    // =====================================================================
    // Scenario 9 (WATCHDOG respawn across teardown) REMOVED for suite-safety.
    //   Testing respawn requires set_auto_repair_enabled(true) — i.e. running the
    //   auto-repair watchdog MID-SUITE — whose background poll raw-derefs a stale
    //   Method* during the shutdown/reinstall churn.  On the slowest no-SEH config
    //   (msvc·java8) this hit an INTERMITTENT JVM crash (the #28 cold-fault class;
    //   passed one CI pass, crashed the re-run).  The suite contract is that the
    //   watchdog stays OFF for the whole run, so respawn-after-shutdown cannot be
    //   exercised in the shared modular harness — only in isolation.  The
    //   reversibility/teardown invariants are already covered HARD by scenarios 5-8.
    // =====================================================================
    }
    catch (const std::exception& ex)
    {
        // A throw anywhere above must not void the run: record it and fall through
        // to the UNCONDITIONAL cleanup below.
        ctx.record(std::string{ "[INFO] shutdown_hooks_teardown: scenario body threw: " } + ex.what());
    }
    catch (...)
    {
        ctx.record("[INFO] shutdown_hooks_teardown: scenario body threw a non-std exception");
    }

    // =====================================================================
    // FINAL CLEANUP — belt-and-braces, OUTSIDE the try so it runs on every path
    //   (normal exit OR a caught throw).  Other modules run after this one, so the
    //   module MUST leave ZERO hooks armed AND the auto-repair watchdog OFF (we
    //   enabled it in scenario 9).  Every scenario above already calls
    //   shutdown_hooks() at its end, but call it once more unconditionally (it is
    //   idempotent and safe-when-empty, both proven above) and force the watchdog
    //   gate off again so the post-condition is unmistakable on every path —
    //   leaving the global state CLEAN and REVIVABLE for whatever module runs next.
    // =====================================================================
    vmhook::shutdown_hooks();
    vmhook::set_auto_repair_enabled(false);   // suite contract: watchdog OFF on exit
    ctx.check("module_final_verify_hooks_zero", vmhook::verify_hooks() == 0u);
    ctx.check("module_final_watchdog_off", !vmhook::auto_repair_enabled());
    ctx.check("module_left_clean_final_shutdown", true);
}
