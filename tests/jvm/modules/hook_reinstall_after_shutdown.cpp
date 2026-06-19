// hook_reinstall_after_shutdown JVM test module  (feature area: hooks / REVIVE)
//
// THE authority on the REVIVE-AFTER-TEARDOWN dimension of the hook lifecycle:
// install a hook, tear it down, install it AGAIN, and prove the detour comes
// back to LIFE and fires on real bytecode dispatch.  This is the live-JVM
// regression guard for the (now-fixed) "shutdown_hooks() is one-shot" class of
// bug.  shutdown_hooks() (vmhook.hpp shutdown_hooks()) used to latch three pieces
// of state true FOREVER:
//   * vmhook::hotspot::g_shutdown_requested   (common_detour bails on it ->
//     EVERY future detour silently dead),
//   * detail::auto_repair::g_started          (ensure_started CAS -> the
//     JIT-drift watchdog never respawns),
//   * the high-level watcher install latches  (class_load_hook_installed /
//     exception_hook_installed).
// The fix resets all three at the END of shutdown_hooks(): the two stores
// (g_started=false, g_shutdown_requested=false) and detail::reset_watcher_latches().
// ensure_started() is then free to spawn a FRESH watchdog on the next hook<T>()
// because shutdown_hooks() first wait_for_exit()s the old one before clearing
// g_started, so no duplicate watchdog can leak.  This module proves the
// user-visible consequence (the hook fires again) AND the documented
// internal-state invariant (audit/findings/hook_basic_install.md test #127
// "auto_repair_restarts_after_shutdown_and_reinit": post-reinit
// g_shutdown_requested == false, auto_repair::g_started == true, worker alive).
//
// ─────────────────────────────────────────────────────────────────────────────
// SUITE-SAFETY (this module was the DIAGNOSED ROOT CAUSE of the Wave-3 matrix-
// wide JVM-crash cascade; re-enabled here under the suite-safety rules in
// audit/PERFECTION_PROGRAM.md:400-403):
//
//   * WHY THE CASCADE HAPPENED, precisely.  This module's whole feature IS
//     shutdown_hooks() reversibility, so it MUST call the GLOBAL
//     vmhook::shutdown_hooks().  In Wave 3 it (a) had no try/catch + entry-guard
//     hardening, so a stray throw / cold-compile SEH fault on a slow CI config
//     tore down the JVM with NO "TOTAL" line -> the WHOLE run's results were
//     voided and every module after it cascade-FAILED, and (b) was authored
//     before the "leave ZERO hooks armed on exit" discipline was codified, so a
//     global shutdown_hooks() mid-suite could tear down a SIBLING module's still-
//     armed hooks.  Calling shutdown_hooks() mid-suite is NOT itself the hazard:
//     hook_verify_repair / shutdown_hooks_teardown / deoptimize_methods /
//     hook_install_after_jit all call it between their own install/teardown
//     beats and are green.  The hazard was the UNGUARDED crash + the pre-
//     discipline leak.  Both are now closed:
//       - Every sibling module leaves zero hooks armed on exit (the modular
//         harness discipline), so by the time THIS module runs no sibling has a
//         hook armed -- every shutdown_hooks() below tears down ONLY this
//         module's own HookReinstall hooks, never another module's state.
//       - The module installs ONLY its own hooks (on vmhook/fixtures/
//         HookReinstall), every shutdown_hooks() is part of THIS module's own
//         install -> fire -> teardown -> (silent) -> reinstall -> fire cycle, and
//         the module's FINAL statement is an unconditional shutdown_hooks()
//         placed OUTSIDE the body try/catch, so on EVERY path (including a thrown
//         body) control returns to the driver with an empty, revivable hook table.
//
//   * NEVER CRASH, BAIL TO [INFO].  The whole body runs inside
//     run_..._checks(ctx) under a try/catch (a throw is recorded as [INFO], never
//     escapes -- mirrors register_class.cpp / aaa_warmup.cpp).  An ENTRY GUARD
//     skips the module cleanly (record [INFO] + the wrapper's final
//     shutdown_hooks() still runs) if HookReinstall does not resolve, so the
//     unguarded static_field("go")->set(...) handshake derefs (same idiom as
//     hook_basic) can never deref a disengaged optional.  The only deref of a
//     decoded oop is self->seed() inside the ping detour; it is gated on
//     self != nullptr and goes through the guarded get_field accessor.
//
//   * LIVE-HOOK verify uses a bounded settle, EMPTY-SET verify stays HARD.  A
//     verify_hooks()==0 assertion taken the instant after a live install can
//     flake: HotSpot's async compiler can land Method::_code in the microsecond
//     window, which verify_hooks() CORRECTLY repairs (counting 1) before
//     NO_COMPILE re-bites.  So the one post-revive verify-on-a-LIVE-hook check
//     uses verify_settles_zero() (ported from hook_verify_repair.cpp: poll up to
//     N passes ~40ms apart, true once a pass returns 0).  Every EMPTY-SET
//     verify==0 check (no hook installed) is taken on a hook table we just
//     cleared with shutdown_hooks() and stays HARD -- there is no method left for
//     an async recompile to drift.
//
// RELATIONSHIP TO shutdown_hooks_teardown.cpp: that module is the authority on
// TEARDOWN (the original body is byte-exact restored, multi-method removal, the
// single three-beat dance via the low-level vmhook::hook<T>() path).  THIS
// module is the authority on REVIVE and deliberately attacks the orthogonal
// surface it leaves uncovered:
//   * the scoped_hook RAII teardown surface (handle drop, NOT shutdown_hooks())
//     interleaved with shutdown_hooks() -- the crash-prone ordering where
//     shutdown_hooks() clears g_hooked_methods while a hook_handle is still
//     alive, then the handle's destructor stop() must find nothing and be a
//     safe no-op,
//   * MANY consecutive shutdown->reinstall cycles (REVIVE_CYCLES) on one
//     instance method -- durability of the reset, not just one cycle,
//   * the same cycle loop on a STATIC method (no `this`, arg at slot 0),
//   * the internal revival invariants (g_shutdown_requested / g_started)
//     that no other module asserts,
//   * empty / double / triple shutdown_hooks() no-crash, each followed by a
//     proof the library still revives,
//   * a force-RETURN revive: the revived hook overrides the return value again
//     (sentinel), proving it is genuinely back in the dispatch path and not a
//     phantom "install returned true but does nothing" success.
//
// WHAT IS HARD vs GATED:
//   * Every no-crash invariant is HARD on every JDK (reaching the assert == no
//     SEH/throw tore down the JVM).
//   * "the detour FIRES again after teardown" is HARD on every JDK: these are
//     small, never-JIT'd interpreted methods (NO_COMPILE is held while hooked),
//     so the i2i interpreter patch is the dispatch path on every HotSpot -- the
//     mode-3 JIT-drift caveat that forces hook_verify_repair to gate its
//     re-fire does NOT apply here (we never warm the method past the JIT
//     threshold).  A missed revive is a real bug, so it stays a red FAIL.
//   * The internal watchdog-started invariant (auto_repair::g_started) is gated:
//     a build with -DVMHOOK_DISABLE_AUTO_REPAIR makes ensure_started() a no-op so
//     g_started never flips -> those asserts are recorded [INFO] SKIPPED.
//     g_shutdown_requested==false after install/shutdown stays HARD regardless.
//
// LIFECYCLE DISCIPLINE: this module mixes both teardown surfaces, so it is
// scrupulous about leaving NOTHING armed.  scoped_hook handles uninstall on
// scope exit; every low-level vmhook::hook<T>() install is matched by a
// shutdown_hooks(); and the module's final statement is an unconditional
// shutdown_hooks() (OUTSIDE the try) + a verify check so the post-condition is
// unmistakable for the modules that run after us.  All install/teardown happens
// on the native (driver) thread BETWEEN probe cycles, never concurrently with a
// probe -- the documented single-thread install/teardown contract.
//
// C++17 idioms only in the module body (no std::bit_cast, no std::format); the
// header requires C++20+ and the project builds the modules at C++23.
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
    // The fully-qualified fixture class.  Used by register_class<hr_fixture>()
    // and -- critically for suite-safety -- by the entry guard's find_class()
    // pre-check, so the unguarded handshake static_field("go")->set(...) derefs
    // in drive() can never fault on a missing/unloaded class.
    constexpr char FIXTURE_CLASS[]{ "vmhook/fixtures/HookReinstall" };

    // Wrapper for vmhook.fixtures.HookReinstall.  Deriving from vmhook::object<>
    // gives the wrapper its vtable (required by register_class<T>) and the
    // static_field(...) / get_field(...) accessors.
    class hr_fixture : public vmhook::object<hr_fixture>
    {
    public:
        explicit hr_fixture(vmhook::oop_t instance) noexcept
            : vmhook::object<hr_fixture>{ instance }
        {
        }

        // --- go/done handshake + scenario selector ------------------------
        // Unguarded handshake setters mirror hook_basic's idiom.  They are only
        // ever reached AFTER the module's entry guard has confirmed FIXTURE_CLASS
        // resolves, so static_field(...) is engaged here (no deref of a
        // disengaged optional).
        static auto set_go(bool value) -> void       { static_field("go")->set(value); }
        static auto set_done(bool value) -> void      { static_field("done")->set(value); }
        static auto get_done() -> bool                { return static_field("done")->get(); }
        static auto set_mode(std::int32_t m) -> void  { static_field("mode")->set(m); }

        // --- recorded observations the Java side writes -------------------
        static auto get_last_ping_result() -> std::int32_t  { return static_field("lastPingResult")->get(); }
        static auto get_last_sping_result() -> std::int32_t { return static_field("lastSpingResult")->get(); }
        static auto get_ping_calls_made() -> std::int32_t   { return static_field("pingCallsMade")->get(); }
        static auto get_sping_calls_made() -> std::int32_t  { return static_field("spingCallsMade")->get(); }
        static auto get_run_epoch() -> std::int32_t         { return static_field("runEpoch")->get(); }

        // Reads this instance's own seed (proves `self` is the right object).
        // get_field guards internally; callers only ever invoke this on a
        // is_valid_pointer-checked, non-null self.
        auto seed() const -> std::int32_t { return get_field("seed")->get(); }
    };

    // ---- Fixture-mirrored constants (lockstep with HookReinstall.java) ------
    constexpr std::int32_t SEED{ 9001 };
    constexpr std::int32_t PING_CALLS{ 3 };
    constexpr std::int32_t SPING_CALLS{ 2 };
    constexpr std::int32_t PING_DELTA{ 41 };
    constexpr std::int32_t SPING_DELTA{ 13 };
    constexpr std::int32_t PING_ORIGINAL{ SEED + PING_DELTA };  // 9042
    constexpr std::int32_t SPING_ORIGINAL{ SPING_DELTA * 7 };   // 91

    // How many consecutive shutdown->reinstall cycles the durability scenarios
    // run.  Each cycle: install -> fire -> teardown -> (silent) -> re-install.
    // Five is enough to catch a latch that re-arms on the FIRST cycle but stays
    // stuck on later ones, while keeping the probe-cycle count modest.
    constexpr int REVIVE_CYCLES{ 5 };

    // Scenario selectors (mirror HookReinstall.mode).
    constexpr std::int32_t MODE_PING{ 1 };
    constexpr std::int32_t MODE_SPING{ 2 };
    constexpr std::int32_t MODE_BOTH{ 3 };

    // ---- Hook observation state (reset per probe cycle) --------------------
    std::atomic<std::int32_t> g_ping_fires{ 0 };
    std::atomic<std::int32_t> g_sping_fires{ 0 };
    std::atomic<bool>         g_ping_self_ok{ false };   // self non-null & seed correct
    std::atomic<bool>         g_ping_arg_ok{ false };    // decoded delta == PING_DELTA
    std::atomic<bool>         g_sping_arg_ok{ false };   // decoded delta == SPING_DELTA (slot 0, no this)

    auto reset_observations() -> void
    {
        g_ping_fires.store(0);
        g_sping_fires.store(0);
        g_ping_self_ok.store(false);
        g_ping_arg_ok.store(false);
        g_sping_arg_ok.store(false);
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
                    hr_fixture::set_done(false);
                    hr_fixture::set_mode(mode);
                }
                hr_fixture::set_go(value);
            },
            []() { return hr_fixture::get_done(); });
    }

    // True once a verify_hooks() pass reports a CLEAN hook set (returns 0) with a
    // bounded tolerance for HotSpot's ASYNCHRONOUS compiler threads.  Ported from
    // hook_verify_repair.cpp.
    //
    // Why a plain `verify_hooks() == 0` taken on a LIVE hook is a flake:
    // verify_hooks() returns the count of hooks it had to REPAIR this pass; its
    // mode-3 detector is `jit_drifted = (_code != null) || !NO_COMPILE`.  _code is
    // owned by HotSpot's compiler threads, so an in-flight (re)compile can land an
    // nmethod in the microsecond window between an install/firing and this read.
    // verify_hooks() then CORRECTLY reports that as one repair and re-deopts the
    // method (re-nulls _code, re-arms NO_COMPILE) -- so a single-instant `== 0`
    // observes the transient mid-recompile repair and fails even though the
    // machinery is working.  NO_COMPILE (re-armed by that very pass) inhibits the
    // next compile, so the transient drift is FINITE and SETTLES.
    //
    // Non-vacuous: a transient async recompile is ABSORBED (a later pass returns
    // 0), but a genuine regression -- verify_hooks() reporting drift it CANNOT
    // settle -- is NOT absorbed: every pass keeps returning non-zero and this
    // returns false, failing the caller's check.  (Note: this module's hooked
    // targets are tiny, never-warmed interpreted methods, so in practice the very
    // first pass returns 0; the settle is belt-and-braces against a sibling
    // module having left hot() warm earlier in the run.)
    auto verify_settles_zero(int attempts) -> bool
    {
        if (vmhook::verify_hooks() == 0)
        {
            return true;
        }
        for (int attempt{ 0 }; attempt < attempts; ++attempt)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds{ 40 });
            if (vmhook::verify_hooks() == 0)
            {
                return true;
            }
        }
        return false;
    }

    // True once the auto-repair watchdog's `g_watchdog_running` liveness flag
    // reaches `want` within a bounded number of ~5ms polls.  The watchdog is a
    // DETACHED thread spawned by ensure_started() at the END of a successful
    // hook<T>(); it flips g_watchdog_running true on entry / false on exit
    // (worker_loop()).  Because the spawn is asynchronous, an INSTANT read right
    // after install can observe `false` before the thread has run its first
    // statement -- so the "watchdog became live" assertion MUST poll, not read
    // once.  The "watchdog is gone" direction is deterministic the instant
    // shutdown_hooks()/set_auto_repair_enabled(false) returns (both call
    // wait_for_exit() before returning), but polling for it too is harmless and
    // keeps both directions symmetric.  Bounded so a genuine never-spawn / never-
    // exit regression returns false (the caller's check fails) instead of hanging.
    auto watchdog_running_reaches(bool want, int attempts) -> bool
    {
        for (int attempt{ 0 }; attempt < attempts; ++attempt)
        {
            if (vmhook::detail::auto_repair::g_watchdog_running.load(std::memory_order_acquire) == want)
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{ 5 });
        }
        return vmhook::detail::auto_repair::g_watchdog_running.load(std::memory_order_acquire) == want;
    }

    // ---- Detours installed through the various paths under test -------------
    // Plain observers (allow-through: they only count + validate self/arg).
    auto install_ping_observer() -> bool
    {
        return vmhook::hook<hr_fixture>(
            "ping",
            [](vmhook::return_value&,
               const std::unique_ptr<hr_fixture>& self,
               std::int32_t delta)
            {
                g_ping_fires.fetch_add(1, std::memory_order_relaxed);
                // self is the factory-built wrapper.  The library's
                // extract_frame_arg<unique_ptr<T>> only builds it from an oop that
                // already passed is_valid_pointer, so the guard here is just
                // self != nullptr before the get_field("seed") read (which itself
                // guards internally) -- the exact idiom hook_verify_repair uses.
                g_ping_self_ok.store(self != nullptr && self->seed() == SEED,
                                     std::memory_order_relaxed);
                g_ping_arg_ok.store(delta == PING_DELTA, std::memory_order_relaxed);
            });
    }

    auto install_sping_observer() -> bool
    {
        return vmhook::hook<hr_fixture>(
            "sping",
            [](vmhook::return_value&, std::int32_t delta)
            {
                g_sping_fires.fetch_add(1, std::memory_order_relaxed);
                g_sping_arg_ok.store(delta == SPING_DELTA, std::memory_order_relaxed);
            });
    }

    // scoped_hook variant returning the RAII handle (teardown on drop).
    auto scoped_ping_observer() -> vmhook::hook_handle
    {
        return vmhook::scoped_hook<hr_fixture>(
            "ping",
            [](vmhook::return_value&,
               const std::unique_ptr<hr_fixture>& self,
               std::int32_t delta)
            {
                g_ping_fires.fetch_add(1, std::memory_order_relaxed);
                g_ping_self_ok.store(self != nullptr && self->seed() == SEED,
                                     std::memory_order_relaxed);
                g_ping_arg_ok.store(delta == PING_DELTA, std::memory_order_relaxed);
            });
    }

    // The return value the force-RETURN override detour writes.  Kept in a global
    // (not a lambda capture) so install_ping_override stays a plain non-capturing
    // lambda matching the rest of the module's style; the alternating-sentinel
    // scenario rewrites this between installs to prove each REVIVE installs the
    // NEW detour body, not a stale cached one.
    std::atomic<std::int32_t> g_override_value{ 0 };

    // Force-RETURN override installer: the detour suppresses the original body and
    // forces the return to g_override_value.  Re-used by the alternating-sentinel
    // revive scenario.
    auto install_ping_override() -> bool
    {
        return vmhook::hook<hr_fixture>(
            "ping",
            [](vmhook::return_value& rv,
               const std::unique_ptr<hr_fixture>&,
               std::int32_t)
            {
                g_ping_fires.fetch_add(1, std::memory_order_relaxed);
                rv.set(g_override_value.load(std::memory_order_relaxed));
            });
    }

    // Whether the auto-repair watchdog is even compiled in.  If the build
    // defined VMHOOK_DISABLE_AUTO_REPAIR, ensure_started() is a no-op and
    // g_started never flips -- so the watchdog-revival asserts are recorded as
    // [INFO] instead of HARD.  The g_shutdown_requested reset is independent of
    // this and stays HARD.
#ifdef VMHOOK_DISABLE_AUTO_REPAIR
    constexpr bool auto_repair_compiled_in{ false };
#else
    constexpr bool auto_repair_compiled_in{ true };
#endif

    // The whole test body, factored out so the VMHOOK_JVM_MODULE wrapper can run
    // it under a try/catch and ALWAYS follow it with shutdown_hooks().
    auto run_hook_reinstall_after_shutdown_checks(vmhook_test::context& ctx) -> void
    {
        vmhook::register_class<hr_fixture>(FIXTURE_CLASS);

        // The functional suite runs with vmhook's background auto-repair watchdog
        // DISABLED (run_all() flips it off before the module loop) so no detached
        // thread can race a GC-time code-cache sweep.  THIS module, however, is a
        // white-box proof of the watchdog's spawn/teardown/respawn lifecycle:
        // section 1 below HARD-asserts detail::auto_repair::g_started flips true on
        // install, false on shutdown, and true again on reinstall — which only
        // holds when ensure_started() is actually allowed to spawn the thread.
        // Re-enable auto-repair for the duration of this (GC-quiet) module so the
        // g_started lifecycle is genuine; the FINAL CLEANUP in the wrapper turns it
        // back OFF (unconditionally, even if this body returns early or throws) so
        // no watchdog survives into the modules that run after us.
        vmhook::set_auto_repair_enabled(true);

        // =====================================================================
        //  ENTRY GUARD.  If HookReinstall is not loaded/resolvable, every
        //  static_field()->set/get below would deref a disengaged optional.  Bail
        //  cleanly to [INFO] instead of dereferencing anything (the final
        //  shutdown_hooks() in the wrapper still runs).  In practice the harness
        //  loads HookReinstall on every run (its own module registers it), so
        //  this is belt-and-braces.
        // =====================================================================
        if (vmhook::find_class(FIXTURE_CLASS) == nullptr)
        {
            ctx.record("[INFO] hook_reinstall_after_shutdown: HookReinstall not "
                       "loaded/resolvable on this run; skipping the module's live "
                       "checks (no crash, no hooks armed).");
            return;
        }

        ctx.record(std::string{ "[INFO] hook_reinstall_after_shutdown: auto-repair watchdog "
                                "compiled in = " } + (auto_repair_compiled_in ? "yes" : "no (VMHOOK_DISABLE_AUTO_REPAIR)"));

        // =====================================================================
        //  0. Sanity: the fixture resolves and its hookable methods exist.
        // =====================================================================
        ctx.check("hr_class_registered_field_resolves",
                  hr_fixture::static_field("go").has_value());
        {
            const auto methods{ vmhook::get_class_methods<hr_fixture>() };
            bool has_ping{ false };
            bool has_sping{ false };
            for (const auto& entry : methods)
            {
                if (entry.first == "ping")  { has_ping = true; }
                if (entry.first == "sping") { has_sping = true; }
            }
            ctx.check("hr_ping_method_declared", has_ping);
            ctx.check("hr_sping_method_declared", has_sping);
        }
        // Mirror-constant cross-checks so a fixture/native drift is caught up front.
        // value_t has no operator== with int; convert each read through int32_t
        // first (the documented field_proxy::value_t -> primitive conversion path).
        {
            const std::int32_t seed_const{ hr_fixture::static_field("SEED")->get() };
            const std::int32_t ping_orig_const{ hr_fixture::static_field("PING_ORIGINAL")->get() };
            const std::int32_t sping_orig_const{ hr_fixture::static_field("SPING_ORIGINAL")->get() };
            ctx.check("hr_seed_constant_matches", seed_const == SEED);
            ctx.check("hr_ping_original_constant_matches", ping_orig_const == PING_ORIGINAL);
            ctx.check("hr_sping_original_constant_matches", sping_orig_const == SPING_ORIGINAL);
        }

        // Start from a known-empty, known-revivable state.  (Other modules run
        // before us and every one is supposed to leave nothing armed, but be
        // defensive: a clean baseline makes every assertion below unambiguous.
        // This shutdown_hooks() runs on an already-empty table in the normal
        // modular case -- proven safe-when-empty by shutdown_hooks_teardown.)
        vmhook::shutdown_hooks();
        // EMPTY-SET verify: the table was just cleared, no method left to drift,
        // so this is HARD.
        ctx.check("hr_baseline_verify_hooks_zero", vmhook::verify_hooks() == 0);
        ctx.check("hr_baseline_shutdown_flag_clear",
                  vmhook::hotspot::g_shutdown_requested.load(std::memory_order_acquire) == false);

        // =====================================================================
        //  1. INTERNAL REVIVAL INVARIANT (audit test #127).  The mechanical heart
        //     of the fix, asserted directly on the library's own state:
        //       (a) install -> g_shutdown_requested is false and g_started is true
        //           (a watchdog was spawned),
        //       (b) shutdown_hooks() RESETS both: g_shutdown_requested back to false
        //           (it is briefly true mid-teardown, but cleared on the way out)
        //           AND g_started back to false (so the next install respawns),
        //       (c) a FRESH install flips g_started true again -> the watchdog
        //           genuinely REVIVED, it is not a stale leftover.
        //     The user-visible "fires again" proof is scenario 2+; this scenario is
        //     the white-box proof that the three latches are actually reset.
        // =====================================================================
        {
            ctx.check("internal_install1_returns_true", install_ping_observer());
            ctx.check("internal_after_install1_shutdown_flag_false",
                      vmhook::hotspot::g_shutdown_requested.load(std::memory_order_acquire) == false);
            const bool started_after_install1{
                vmhook::detail::auto_repair::g_started.load(std::memory_order_acquire) };
            if (auto_repair_compiled_in)
            {
                ctx.check("internal_after_install1_watchdog_started", started_after_install1);
            }
            else
            {
                ctx.record("[INFO] internal_after_install1_watchdog_started: SKIPPED "
                           "(VMHOOK_DISABLE_AUTO_REPAIR -- g_started never set).");
            }

            // Teardown must clear BOTH latches (the headline fix).
            vmhook::shutdown_hooks();
            ctx.check("internal_after_shutdown_flag_reset_false",
                      vmhook::hotspot::g_shutdown_requested.load(std::memory_order_acquire) == false);
            if (auto_repair_compiled_in)
            {
                ctx.check("internal_after_shutdown_watchdog_started_reset_false",
                          vmhook::detail::auto_repair::g_started.load(std::memory_order_acquire) == false);
            }
            else
            {
                ctx.record("[INFO] internal_after_shutdown_watchdog_started_reset_false: SKIPPED "
                           "(VMHOOK_DISABLE_AUTO_REPAIR).");
            }

            // A fresh install must REVIVE both: flag still false, watchdog respawned.
            ctx.check("internal_install2_returns_true", install_ping_observer());
            ctx.check("internal_after_install2_shutdown_flag_false",
                      vmhook::hotspot::g_shutdown_requested.load(std::memory_order_acquire) == false);
            if (auto_repair_compiled_in)
            {
                ctx.check("internal_after_install2_watchdog_respawned",
                          vmhook::detail::auto_repair::g_started.load(std::memory_order_acquire));
            }
            else
            {
                ctx.record("[INFO] internal_after_install2_watchdog_respawned: SKIPPED "
                           "(VMHOOK_DISABLE_AUTO_REPAIR).");
            }

            vmhook::shutdown_hooks();   // clean up scenario 1
        }

        // =====================================================================
        //  2. INSTANCE REVIVE CYCLES -- the headline durability proof.  Run
        //     REVIVE_CYCLES full install->fire->teardown->silent->reinstall cycles
        //     on the instance method ping(int).  Each cycle must:
        //        * install (returns true),
        //        * fire exactly PING_CALLS times with the right self + arg,
        //        * allow-through the byte-exact original result,
        //        * after shutdown_hooks(): be SILENT while the original still runs.
        //     A latch that re-armed on cycle 0 but stuck later is caught because we
        //     assert the FIRE on every cycle, not just the first.  All HARD.
        // =====================================================================
        {
            int cycles_with_fire{ 0 };
            for (int cycle{ 0 }; cycle < REVIVE_CYCLES; ++cycle)
            {
                const std::string c{ std::to_string(cycle) };

                // -- install + fire --
                ctx.check("inst_cycle" + c + "_install_returns_true", install_ping_observer());
                const bool done_fire{ drive(ctx, MODE_PING) };
                ctx.check("inst_cycle" + c + "_probe_completed", done_fire);
                ctx.check("inst_cycle" + c + "_java_made_calls",
                          hr_fixture::get_ping_calls_made() == PING_CALLS);
                const bool fired{ g_ping_fires.load() == PING_CALLS };
                ctx.check("inst_cycle" + c + "_hook_fires_PING_CALLS_times", fired);
                ctx.check("inst_cycle" + c + "_self_correct", g_ping_self_ok.load());
                ctx.check("inst_cycle" + c + "_arg_decoded", g_ping_arg_ok.load());
                ctx.check("inst_cycle" + c + "_allow_through_original",
                          hr_fixture::get_last_ping_result() == PING_ORIGINAL);
                if (fired) { ++cycles_with_fire; }

                // -- teardown -> silent, original byte-exact --
                vmhook::shutdown_hooks();
                const bool done_silent{ drive(ctx, MODE_PING) };
                ctx.check("inst_cycle" + c + "_after_shutdown_probe_completed", done_silent);
                ctx.check("inst_cycle" + c + "_after_shutdown_java_still_ran",
                          hr_fixture::get_ping_calls_made() == PING_CALLS);
                ctx.check("inst_cycle" + c + "_after_shutdown_detour_SILENT",
                          g_ping_fires.load() == 0);
                ctx.check("inst_cycle" + c + "_after_shutdown_byte_exact_original",
                          hr_fixture::get_last_ping_result() == PING_ORIGINAL);
            }

            // Hard floor: EVERY cycle must have fired (no vacuous pass).  Worst
            // acceptable is "all REVIVE_CYCLES fired" -- a strict equality so a
            // single dead revive anywhere in the loop fails the module.
            ctx.check("inst_all_revive_cycles_fired", cycles_with_fire == REVIVE_CYCLES);
            ctx.record(std::string{ "[INFO] hook_reinstall_after_shutdown: instance revive cycles fired " }
                       + std::to_string(cycles_with_fire) + "/" + std::to_string(REVIVE_CYCLES));

            vmhook::shutdown_hooks();   // belt-and-braces
        }

        // =====================================================================
        //  3. STATIC REVIVE CYCLES -- the same durability proof on a STATIC method
        //     sping(int): no `this`, the explicit arg is at interpreter slot 0.
        //     Proves the revive path is identical for the static install shape.
        // =====================================================================
        {
            int cycles_with_fire{ 0 };
            for (int cycle{ 0 }; cycle < REVIVE_CYCLES; ++cycle)
            {
                const std::string c{ std::to_string(cycle) };

                ctx.check("static_cycle" + c + "_install_returns_true", install_sping_observer());
                const bool done_fire{ drive(ctx, MODE_SPING) };
                ctx.check("static_cycle" + c + "_probe_completed", done_fire);
                ctx.check("static_cycle" + c + "_java_made_calls",
                          hr_fixture::get_sping_calls_made() == SPING_CALLS);
                const bool fired{ g_sping_fires.load() == SPING_CALLS };
                ctx.check("static_cycle" + c + "_hook_fires_SPING_CALLS_times", fired);
                ctx.check("static_cycle" + c + "_arg_decoded_slot0", g_sping_arg_ok.load());
                ctx.check("static_cycle" + c + "_allow_through_original",
                          hr_fixture::get_last_sping_result() == SPING_ORIGINAL);
                if (fired) { ++cycles_with_fire; }

                vmhook::shutdown_hooks();
                const bool done_silent{ drive(ctx, MODE_SPING) };
                ctx.check("static_cycle" + c + "_after_shutdown_probe_completed", done_silent);
                ctx.check("static_cycle" + c + "_after_shutdown_detour_SILENT",
                          g_sping_fires.load() == 0);
                ctx.check("static_cycle" + c + "_after_shutdown_byte_exact_original",
                          hr_fixture::get_last_sping_result() == SPING_ORIGINAL);
            }

            ctx.check("static_all_revive_cycles_fired", cycles_with_fire == REVIVE_CYCLES);
            ctx.record(std::string{ "[INFO] hook_reinstall_after_shutdown: static revive cycles fired " }
                       + std::to_string(cycles_with_fire) + "/" + std::to_string(REVIVE_CYCLES));

            vmhook::shutdown_hooks();
        }

        // =====================================================================
        //  4. scoped_hook REVIVE via RAII drop (NOT shutdown_hooks()).  The
        //     orthogonal teardown surface: a scoped_hook handle disarms on scope
        //     exit (hook_handle::stop()).  Prove the cycle
        //     install(scoped)->fire->{handle drops}->silent->install(scoped again)
        //     ->fire works -- i.e. RAII teardown does NOT poison re-install and
        //     does NOT touch the global shutdown latch (scoped_hook teardown must
        //     leave the library revivable WITHOUT a shutdown_hooks()).
        // =====================================================================
        {
            // -- cycle A: scoped install -> fire -> drop -> silent --
            {
                auto handle{ scoped_ping_observer() };
                ctx.check("scoped_cycleA_installed", handle.installed());
                const bool doneA{ drive(ctx, MODE_PING) };
                ctx.check("scoped_cycleA_probe_completed", doneA);
                ctx.check("scoped_cycleA_hook_fires", g_ping_fires.load() == PING_CALLS);
                ctx.check("scoped_cycleA_self_correct", g_ping_self_ok.load());
                ctx.check("scoped_cycleA_allow_through",
                          hr_fixture::get_last_ping_result() == PING_ORIGINAL);
                // handle drops here -> RAII stop().  No shutdown_hooks().
            }
            // scoped_hook teardown must NOT have flipped the global shutdown latch.
            ctx.check("scoped_after_drop_shutdown_flag_still_false",
                      vmhook::hotspot::g_shutdown_requested.load(std::memory_order_acquire) == false);
            {
                const bool doneA2{ drive(ctx, MODE_PING) };
                ctx.check("scoped_cycleA_after_drop_probe_completed", doneA2);
                ctx.check("scoped_cycleA_after_drop_detour_SILENT", g_ping_fires.load() == 0);
                ctx.check("scoped_cycleA_after_drop_byte_exact_original",
                          hr_fixture::get_last_ping_result() == PING_ORIGINAL);
            }

            // -- cycle B: scoped install AGAIN -> must REVIVE and fire --
            {
                auto handle{ scoped_ping_observer() };
                ctx.check("scoped_cycleB_reinstalled", handle.installed());
                const bool doneB{ drive(ctx, MODE_PING) };
                ctx.check("scoped_cycleB_probe_completed", doneB);
                ctx.check("scoped_cycleB_REVIVED_hook_fires", g_ping_fires.load() == PING_CALLS);
                ctx.check("scoped_cycleB_self_correct", g_ping_self_ok.load());
                ctx.check("scoped_cycleB_allow_through",
                          hr_fixture::get_last_ping_result() == PING_ORIGINAL);
            }
            vmhook::shutdown_hooks();   // belt-and-braces (handle already dropped)
        }

        // =====================================================================
        //  5. MIXED TEARDOWN ORDERING -- the crash-prone interaction.
        //     shutdown_hooks() is called WHILE a scoped_hook handle is still alive.
        //     shutdown_hooks() clears g_hooked_methods; the handle's later
        //     destructor stop() must then find its entry already gone and be a
        //     SAFE no-op (no double-free, no AV).  Then a fresh install must still
        //     revive.  This is the single most likely crash path in real
        //     mod-loader "tear down the world, drop the RAII guards later" code.
        // =====================================================================
        {
            {
                auto handle{ scoped_ping_observer() };
                ctx.check("mixed_scoped_installed", handle.installed());
                const bool done1{ drive(ctx, MODE_PING) };
                ctx.check("mixed_pre_probe_completed", done1);
                ctx.check("mixed_pre_hook_fires", g_ping_fires.load() == PING_CALLS);

                // Bulk teardown while the handle is STILL ALIVE.  This removes the
                // entry from g_hooked_methods out from under the handle.
                vmhook::shutdown_hooks();
                ctx.check("mixed_bulk_shutdown_while_handle_alive_did_not_crash", true);

                // Detour must be silent now even though `handle` still thinks it is
                // installed (the handle's `method` pointer is non-null, but its
                // entry is gone from the dispatch list).
                const bool done2{ drive(ctx, MODE_PING) };
                ctx.check("mixed_after_bulk_probe_completed", done2);
                ctx.check("mixed_after_bulk_detour_silent", g_ping_fires.load() == 0);

                // `handle` destructs HERE -> stop() finds no matching entry in
                // g_hooked_methods (shutdown_hooks() cleared it) and must be a safe
                // idempotent no-op.  If stop() blindly erased/double-freed this
                // would AV; reaching the next assert proves it did not.
            }
            ctx.check("mixed_stale_handle_drop_after_bulk_shutdown_did_not_crash", true);

            // Library must still revive after that messy ordering.
            ctx.check("mixed_reinstall_returns_true", install_ping_observer());
            const bool done3{ drive(ctx, MODE_PING) };
            ctx.check("mixed_reinstall_probe_completed", done3);
            ctx.check("mixed_reinstall_REVIVES_and_fires", g_ping_fires.load() == PING_CALLS);
            ctx.check("mixed_reinstall_allow_through",
                      hr_fixture::get_last_ping_result() == PING_ORIGINAL);

            vmhook::shutdown_hooks();   // clean up
        }

        // =====================================================================
        //  6. EMPTY / DOUBLE / TRIPLE shutdown_hooks() are all safe AND keep the
        //     library revivable.  No hook is installed for the empty case; the
        //     double/triple cases stack redundant teardowns.  After each, a fresh
        //     install must fire -- proving none of the redundant calls latched the
        //     shutdown flag in a way the reset at the end of shutdown_hooks() fails
        //     to clear.
        // =====================================================================
        {
            // -- empty (no hooks installed) x2 back-to-back --
            vmhook::shutdown_hooks();
            vmhook::shutdown_hooks();
            ctx.check("empty_double_shutdown_did_not_crash", true);
            ctx.check("empty_double_shutdown_flag_clear",
                      vmhook::hotspot::g_shutdown_requested.load(std::memory_order_acquire) == false);
            // EMPTY-SET verify (table cleared, nothing to drift): HARD.
            ctx.check("empty_double_shutdown_verify_zero", vmhook::verify_hooks() == 0);

            // revive after the empty double-teardown.
            ctx.check("empty_double_then_install_returns_true", install_ping_observer());
            const bool d_empty{ drive(ctx, MODE_PING) };
            ctx.check("empty_double_then_probe_completed", d_empty);
            ctx.check("empty_double_then_hook_fires", g_ping_fires.load() == PING_CALLS);

            // -- triple shutdown WITH a hook installed: first tears down, next two
            //    are no-ops on the empty state.  Then revive again. --
            vmhook::shutdown_hooks();
            vmhook::shutdown_hooks();
            vmhook::shutdown_hooks();
            ctx.check("triple_shutdown_did_not_crash", true);
            ctx.check("triple_shutdown_flag_clear",
                      vmhook::hotspot::g_shutdown_requested.load(std::memory_order_acquire) == false);

            const bool d_silent{ drive(ctx, MODE_PING) };
            ctx.check("triple_shutdown_probe_completed", d_silent);
            ctx.check("triple_shutdown_detour_silent", g_ping_fires.load() == 0);

            ctx.check("triple_then_install_returns_true", install_ping_observer());
            const bool d_revive{ drive(ctx, MODE_PING) };
            ctx.check("triple_then_probe_completed", d_revive);
            ctx.check("triple_then_REVIVES_and_fires", g_ping_fires.load() == PING_CALLS);

            vmhook::shutdown_hooks();   // clean up
        }

        // =====================================================================
        //  7. FORCE-RETURN REVIVE -- the strongest "genuinely back in the dispatch
        //     path" proof.  A revived hook is not merely "install returned true";
        //     it must actually intercept.  Install a hook that OVERRIDES the return
        //     (sentinel != original), prove Java sees the sentinel; shutdown ->
        //     Java sees the original again; install the override AGAIN -> Java sees
        //     the sentinel AGAIN.  A phantom revive (flag stuck so common_detour
        //     bails) would let Java see the ORIGINAL on the third beat -> red FAIL.
        // =====================================================================
        {
            constexpr std::int32_t SENTINEL{ 7654321 };
            static_assert(SENTINEL != PING_ORIGINAL,
                          "sentinel must differ from the original so the override is observable");

            const auto install_override = []() -> bool
            {
                return vmhook::hook<hr_fixture>(
                    "ping",
                    [](vmhook::return_value& rv,
                       const std::unique_ptr<hr_fixture>&,
                       std::int32_t)
                    {
                        g_ping_fires.fetch_add(1, std::memory_order_relaxed);
                        rv.set(SENTINEL);   // suppress original body, force the return
                    });
            };

            // Beat 1: override installed -> Java observes the sentinel.
            ctx.check("force_beat1_install_returns_true", install_override());
            const bool d1{ drive(ctx, MODE_PING) };
            ctx.check("force_beat1_probe_completed", d1);
            ctx.check("force_beat1_hook_fired", g_ping_fires.load() == PING_CALLS);
            ctx.check("force_beat1_java_saw_sentinel",
                      hr_fixture::get_last_ping_result() == SENTINEL);

            // Beat 2: teardown -> Java observes the byte-exact original again.
            vmhook::shutdown_hooks();
            const bool d2{ drive(ctx, MODE_PING) };
            ctx.check("force_beat2_probe_completed", d2);
            ctx.check("force_beat2_detour_silent", g_ping_fires.load() == 0);
            ctx.check("force_beat2_java_saw_original",
                      hr_fixture::get_last_ping_result() == PING_ORIGINAL);
            ctx.check("force_beat2_java_not_sentinel",
                      hr_fixture::get_last_ping_result() != SENTINEL);

            // Beat 3: REVIVE the override -> Java observes the sentinel AGAIN.  This
            // is the litmus test: a revived hook that is truly in the dispatch path
            // can override the return; a phantom one cannot.
            ctx.check("force_beat3_reinstall_returns_true", install_override());
            const bool d3{ drive(ctx, MODE_PING) };
            ctx.check("force_beat3_probe_completed", d3);
            ctx.check("force_beat3_REVIVED_hook_fired", g_ping_fires.load() == PING_CALLS);
            ctx.check("force_beat3_java_saw_sentinel_AGAIN",
                      hr_fixture::get_last_ping_result() == SENTINEL);
            ctx.check("force_beat3_java_not_original",
                      hr_fixture::get_last_ping_result() != PING_ORIGINAL);

            vmhook::shutdown_hooks();   // clean up
        }

        // =====================================================================
        //  8. MULTI-METHOD REVIVE in one shot: install BOTH ping (instance) and
        //     sping (static), prove both fire in a single run(); shutdown_hooks()
        //     silences BOTH; re-arm BOTH -> both REVIVE in a single run().  Proves
        //     the reset revives the whole hook set, not just whichever method was
        //     installed last.
        // =====================================================================
        {
            ctx.check("multi_install_ping", install_ping_observer());
            ctx.check("multi_install_sping", install_sping_observer());
            const bool d_pre{ drive(ctx, MODE_BOTH) };
            ctx.check("multi_pre_probe_completed", d_pre);
            ctx.check("multi_pre_ping_fired", g_ping_fires.load() == 1);
            ctx.check("multi_pre_sping_fired", g_sping_fires.load() == 1);
            ctx.check("multi_pre_ping_allow_through",
                      hr_fixture::get_last_ping_result() == PING_ORIGINAL);
            ctx.check("multi_pre_sping_allow_through",
                      hr_fixture::get_last_sping_result() == SPING_ORIGINAL);

            vmhook::shutdown_hooks();   // one teardown silences both
            const bool d_silent{ drive(ctx, MODE_BOTH) };
            ctx.check("multi_after_shutdown_probe_completed", d_silent);
            ctx.check("multi_after_shutdown_ping_silent", g_ping_fires.load() == 0);
            ctx.check("multi_after_shutdown_sping_silent", g_sping_fires.load() == 0);
            ctx.check("multi_after_shutdown_ping_original",
                      hr_fixture::get_last_ping_result() == PING_ORIGINAL);
            ctx.check("multi_after_shutdown_sping_original",
                      hr_fixture::get_last_sping_result() == SPING_ORIGINAL);

            // Re-arm both -> both revive in one run().
            ctx.check("multi_reinstall_ping", install_ping_observer());
            ctx.check("multi_reinstall_sping", install_sping_observer());
            const bool d_revive{ drive(ctx, MODE_BOTH) };
            ctx.check("multi_reinstall_probe_completed", d_revive);
            ctx.check("multi_reinstall_ping_REVIVES", g_ping_fires.load() == 1);
            ctx.check("multi_reinstall_sping_REVIVES", g_sping_fires.load() == 1);

            vmhook::shutdown_hooks();   // clean up
        }

        // =====================================================================
        //  10. EXPLICIT hook_handle::stop() (not RAII drop) as a teardown surface,
        //      and its idempotency under revive.  Scenarios 4/5 only exercise the
        //      DESTRUCTOR path (handle drops at scope exit).  stop() is also a
        //      public method a caller may invoke EARLY (disarm now, keep the handle
        //      object around) -- and it must be idempotent (a second stop(), then
        //      the eventual destructor's stop(), are all safe no-ops on method ==
        //      nullptr).  Prove: install(scoped) -> fire -> explicit stop() ->
        //      SILENT -> redundant stop() no-crash -> the global flag is untouched
        //      (stop() must not flip g_shutdown_requested) -> a fresh install
        //      REVIVES and fires.
        // =====================================================================
        {
            auto handle{ scoped_ping_observer() };
            ctx.check("explicitstop_installed", handle.installed());
            const bool d_fire{ drive(ctx, MODE_PING) };
            ctx.check("explicitstop_probe_completed", d_fire);
            ctx.check("explicitstop_hook_fires", g_ping_fires.load() == PING_CALLS);

            // Explicit disarm via the public method (NOT the destructor).
            handle.stop();
            ctx.check("explicitstop_handle_not_installed_after_stop", handle.installed() == false);
            ctx.check("explicitstop_did_not_crash", true);
            // stop() is per-handle teardown: it must NEVER touch the global flag
            // (same revivable-without-shutdown_hooks() contract as the RAII drop).
            ctx.check("explicitstop_shutdown_flag_still_false",
                      vmhook::hotspot::g_shutdown_requested.load(std::memory_order_acquire) == false);

            const bool d_silent{ drive(ctx, MODE_PING) };
            ctx.check("explicitstop_after_stop_probe_completed", d_silent);
            ctx.check("explicitstop_after_stop_detour_SILENT", g_ping_fires.load() == 0);
            ctx.check("explicitstop_after_stop_byte_exact_original",
                      hr_fixture::get_last_ping_result() == PING_ORIGINAL);

            // Idempotency: a redundant stop() (and the destructor's stop() at the
            // end of this scope) must be safe no-ops.
            handle.stop();
            ctx.check("explicitstop_redundant_stop_did_not_crash", true);
            ctx.check("explicitstop_redundant_stop_still_not_installed", handle.installed() == false);

            // The library must still revive after an explicit-stop teardown.
            ctx.check("explicitstop_reinstall_returns_true", install_ping_observer());
            const bool d_revive{ drive(ctx, MODE_PING) };
            ctx.check("explicitstop_reinstall_probe_completed", d_revive);
            ctx.check("explicitstop_reinstall_REVIVES_and_fires", g_ping_fires.load() == PING_CALLS);
            ctx.check("explicitstop_reinstall_allow_through",
                      hr_fixture::get_last_ping_result() == PING_ORIGINAL);

            vmhook::shutdown_hooks();   // clean up (handle.stop() already disarmed)
        }

        // =====================================================================
        //  11. hook_handle MOVE semantics across a revive.  hook_handle is
        //      move-only; move-CONSTRUCT transfers ownership (the source becomes
        //      not-installed, the destination disarms on drop), and move-ASSIGN
        //      first stop()s the destination's current hook then takes the source.
        //      A bug in move could double-disarm or leak, breaking revive.  Prove:
        //        (a) move-construct: source becomes not-installed, dest fires,
        //        (b) move-assign over a live handle disarms the OLD target and the
        //            survivor fires (no double-free),
        //        (c) the library still revives afterward.
        // =====================================================================
        {
            // (a) move-construct
            {
                auto src{ scoped_ping_observer() };
                ctx.check("move_src_installed", src.installed());
                auto dst{ std::move(src) };
                ctx.check("move_src_emptied_after_move", src.installed() == false);
                ctx.check("move_dst_installed_after_move", dst.installed());
                const bool d_mv{ drive(ctx, MODE_PING) };
                ctx.check("move_construct_probe_completed", d_mv);
                ctx.check("move_construct_hook_fires_via_dst", g_ping_fires.load() == PING_CALLS);
                // src drops (no-op, emptied) then dst drops (disarms) here.
            }
            {
                const bool d_silent{ drive(ctx, MODE_PING) };
                ctx.check("move_after_dst_drop_probe_completed", d_silent);
                ctx.check("move_after_dst_drop_detour_SILENT", g_ping_fires.load() == 0);
            }

            // (b) move-assign over a live handle.  `keep` starts holding the live
            //     ping hook; move-assigning an EMPTY handle into it must stop()
            //     keep's hook (so the detour goes silent) without crashing.  Then a
            //     re-install proves the library is still revivable.
            {
                auto keep{ scoped_ping_observer() };
                ctx.check("move_assign_keep_installed", keep.installed());
                const bool d_pre{ drive(ctx, MODE_PING) };
                ctx.check("move_assign_pre_probe_completed", d_pre);
                ctx.check("move_assign_pre_hook_fires", g_ping_fires.load() == PING_CALLS);

                vmhook::hook_handle empty{};      // not-installed
                keep = std::move(empty);          // move-assign disarms keep's hook
                ctx.check("move_assign_keep_emptied", keep.installed() == false);
                ctx.check("move_assign_did_not_crash", true);

                const bool d_silent{ drive(ctx, MODE_PING) };
                ctx.check("move_assign_after_probe_completed", d_silent);
                ctx.check("move_assign_after_detour_SILENT", g_ping_fires.load() == 0);
            }

            // (c) revive after the move gymnastics.
            ctx.check("move_reinstall_returns_true", install_ping_observer());
            const bool d_revive{ drive(ctx, MODE_PING) };
            ctx.check("move_reinstall_probe_completed", d_revive);
            ctx.check("move_reinstall_REVIVES_and_fires", g_ping_fires.load() == PING_CALLS);

            vmhook::shutdown_hooks();   // clean up
        }

        // =====================================================================
        //  12. DUPLICATE-INSTALL contract through a revive.  Install ping via the
        //      low-level hook<T>() (persistent), then a scoped_hook on the SAME
        //      method must return a NOT-installed handle (only one detour per
        //      method fires; the first owns it).  Dropping that duplicate handle
        //      must be a no-op that does NOT disarm the original -- the original
        //      keeps firing.  Then shutdown_hooks() silences it and a fresh install
        //      revives.  This pins that the duplicate-handle path never poisons the
        //      revive lifecycle.
        // =====================================================================
        {
            ctx.check("dup_primary_install_returns_true", install_ping_observer());
            {
                auto dup{ scoped_ping_observer() };   // same method -> already hooked
                ctx.check("dup_scoped_handle_not_installed", dup.installed() == false);
                // The original (low-level) hook must still fire while the duplicate
                // handle is alive.
                const bool d_dup{ drive(ctx, MODE_PING) };
                ctx.check("dup_probe_completed", d_dup);
                ctx.check("dup_original_still_fires", g_ping_fires.load() == PING_CALLS);
                // dup drops here -> its stop() is a guaranteed no-op (method ==
                // nullptr), so it must NOT disarm the original.
            }
            // Original must STILL be live after the duplicate handle dropped.
            const bool d_after{ drive(ctx, MODE_PING) };
            ctx.check("dup_after_drop_probe_completed", d_after);
            ctx.check("dup_original_survives_duplicate_drop", g_ping_fires.load() == PING_CALLS);
            ctx.check("dup_original_allow_through",
                      hr_fixture::get_last_ping_result() == PING_ORIGINAL);

            // Now tear down the real one and prove revive.
            vmhook::shutdown_hooks();
            const bool d_silent{ drive(ctx, MODE_PING) };
            ctx.check("dup_after_shutdown_probe_completed", d_silent);
            ctx.check("dup_after_shutdown_SILENT", g_ping_fires.load() == 0);
            ctx.check("dup_reinstall_returns_true", install_ping_observer());
            const bool d_revive{ drive(ctx, MODE_PING) };
            ctx.check("dup_reinstall_probe_completed", d_revive);
            ctx.check("dup_reinstall_REVIVES_and_fires", g_ping_fires.load() == PING_CALLS);

            vmhook::shutdown_hooks();   // clean up
        }

        // =====================================================================
        //  13. set_auto_repair_enabled(false) is the WATCHDOG teardown surface and
        //      is ALSO revivable -- WITHOUT removing hooks (the header documents
        //      that disabling raises+clears g_shutdown_requested and clears
        //      g_started just like shutdown_hooks(), but does NOT touch
        //      g_hooked_methods).  So a hook installed BEFORE the disable must keep
        //      firing AFTER it (common_detour no longer early-outs because the flag
        //      was cleared on the way out), the watchdog must be GONE
        //      (g_watchdog_running false), and a later re-enable + install must
        //      RESPAWN the watchdog.  No other module asserts this orthogonal
        //      "watchdog off but hooks live" state.  Watchdog asserts gated on
        //      auto_repair_compiled_in; the flag + still-fires asserts are HARD.
        // =====================================================================
        {
            // Ensure the watchdog is enabled and spawned by the install.
            vmhook::set_auto_repair_enabled(true);
            ctx.check("arepair_install_returns_true", install_ping_observer());
            if (auto_repair_compiled_in)
            {
                ctx.check("arepair_watchdog_live_after_install",
                          watchdog_running_reaches(true, 200));
            }
            else
            {
                ctx.record("[INFO] arepair_watchdog_live_after_install: SKIPPED "
                           "(VMHOOK_DISABLE_AUTO_REPAIR).");
            }
            const bool d_pre{ drive(ctx, MODE_PING) };
            ctx.check("arepair_pre_probe_completed", d_pre);
            ctx.check("arepair_pre_hook_fires", g_ping_fires.load() == PING_CALLS);

            // Disable the watchdog.  This is NOT shutdown_hooks(): the hook entry
            // must SURVIVE in g_hooked_methods, only the background thread stops.
            vmhook::set_auto_repair_enabled(false);
            ctx.check("arepair_after_disable_flag_clear",
                      vmhook::hotspot::g_shutdown_requested.load(std::memory_order_acquire) == false);
            if (auto_repair_compiled_in)
            {
                ctx.check("arepair_watchdog_gone_after_disable",
                          watchdog_running_reaches(false, 200));
                ctx.check("arepair_started_reset_after_disable",
                          vmhook::detail::auto_repair::g_started.load(std::memory_order_acquire) == false);
            }
            else
            {
                ctx.record("[INFO] arepair_watchdog_gone_after_disable: SKIPPED "
                           "(VMHOOK_DISABLE_AUTO_REPAIR).");
                ctx.record("[INFO] arepair_started_reset_after_disable: SKIPPED "
                           "(VMHOOK_DISABLE_AUTO_REPAIR).");
            }

            // The headline of this scenario: the hook is STILL LIVE even though the
            // watchdog is gone (disable cleared g_shutdown_requested, so
            // common_detour does not early-out and the entry is still in the list).
            const bool d_live{ drive(ctx, MODE_PING) };
            ctx.check("arepair_hook_STILL_FIRES_after_watchdog_disabled", g_ping_fires.load() == PING_CALLS);
            ctx.check("arepair_still_fires_probe_completed", d_live);
            ctx.check("arepair_still_fires_allow_through",
                      hr_fixture::get_last_ping_result() == PING_ORIGINAL);

            // Re-enable: the gate flips but no thread spawns until the next install.
            // A FRESH install must respawn the watchdog (g_started true again).
            vmhook::set_auto_repair_enabled(true);
            vmhook::shutdown_hooks();   // clear the surviving entry for a clean reinstall
            ctx.check("arepair_reenable_install_returns_true", install_ping_observer());
            if (auto_repair_compiled_in)
            {
                ctx.check("arepair_watchdog_respawned_after_reenable",
                          watchdog_running_reaches(true, 200));
                ctx.check("arepair_started_true_after_reenable",
                          vmhook::detail::auto_repair::g_started.load(std::memory_order_acquire));
            }
            else
            {
                ctx.record("[INFO] arepair_watchdog_respawned_after_reenable: SKIPPED "
                           "(VMHOOK_DISABLE_AUTO_REPAIR).");
                ctx.record("[INFO] arepair_started_true_after_reenable: SKIPPED "
                           "(VMHOOK_DISABLE_AUTO_REPAIR).");
            }
            const bool d_revive{ drive(ctx, MODE_PING) };
            ctx.check("arepair_reenable_probe_completed", d_revive);
            ctx.check("arepair_reenable_REVIVES_and_fires", g_ping_fires.load() == PING_CALLS);

            vmhook::shutdown_hooks();   // clean up
        }

        // =====================================================================
        //  14. SELECTIVE re-arm after a multi-method teardown.  Install BOTH ping
        //      and sping, fire both; ONE shutdown_hooks() silences both; then
        //      re-arm ONLY ping.  ping must REVIVE and fire, while sping must stay
        //      SILENT (it was NOT re-armed) and run its byte-exact original.  This
        //      proves the reset does not spuriously revive a method the caller
        //      chose not to reinstall -- the revive is per-install, not a blanket
        //      "everything that was ever hooked comes back" latch.
        // =====================================================================
        {
            ctx.check("selective_install_ping", install_ping_observer());
            ctx.check("selective_install_sping", install_sping_observer());
            const bool d_pre{ drive(ctx, MODE_BOTH) };
            ctx.check("selective_pre_probe_completed", d_pre);
            ctx.check("selective_pre_ping_fired", g_ping_fires.load() == 1);
            ctx.check("selective_pre_sping_fired", g_sping_fires.load() == 1);

            vmhook::shutdown_hooks();   // silences both

            // Re-arm ONLY ping.
            ctx.check("selective_reinstall_only_ping", install_ping_observer());
            const bool d_sel{ drive(ctx, MODE_BOTH) };
            ctx.check("selective_probe_completed", d_sel);
            ctx.check("selective_ping_REVIVES", g_ping_fires.load() == 1);
            ctx.check("selective_sping_STAYS_SILENT", g_sping_fires.load() == 0);
            ctx.check("selective_ping_allow_through",
                      hr_fixture::get_last_ping_result() == PING_ORIGINAL);
            // sping ran its original body (un-hooked) -> byte-exact original.
            ctx.check("selective_sping_byte_exact_original",
                      hr_fixture::get_last_sping_result() == SPING_ORIGINAL);

            vmhook::shutdown_hooks();   // clean up
        }

        // =====================================================================
        //  15. ALTERNATING-SENTINEL force-RETURN revive.  Scenario 7 proves a
        //      revived OVERRIDE re-enters the dispatch path, but always with the
        //      SAME sentinel -- a stale cached detour from the first install would
        //      be indistinguishable.  Here each revive installs an override whose
        //      forced return value DIFFERS from the previous cycle's (the detour
        //      reads g_override_value, which we rewrite between installs).  Java
        //      must observe the value from the MOST RECENT install on every cycle,
        //      proving the revive installs the CURRENT detour body, not a stale one
        //      latched at first install.  Each cycle is shutdown-separated.
        // =====================================================================
        {
            constexpr std::int32_t sentinels[3]{ 1111111, 2222222, 3333333 };
            static_assert(sentinels[0] != sentinels[1] && sentinels[1] != sentinels[2]
                          && sentinels[0] != sentinels[2],
                          "alternating sentinels must be pairwise distinct so a stale "
                          "cached detour body is caught");
            for (int i{ 0 }; i < 3; ++i)
            {
                const std::string c{ std::to_string(i) };
                const std::int32_t want{ sentinels[i] };
                // Each sentinel must differ from the original so the override is
                // observable, and from its neighbours so a stale body is caught.
                if (want == PING_ORIGINAL)
                {
                    ctx.record("[INFO] altsentinel: sentinel collided with original (impossible "
                               "given the chosen constants); skipping this cycle's observe.");
                    continue;
                }

                g_override_value.store(want, std::memory_order_relaxed);
                ctx.check("altsentinel_cycle" + c + "_install_returns_true", install_ping_override());
                const bool d{ drive(ctx, MODE_PING) };
                ctx.check("altsentinel_cycle" + c + "_probe_completed", d);
                ctx.check("altsentinel_cycle" + c + "_hook_fired",
                          g_ping_fires.load() == PING_CALLS);
                ctx.check("altsentinel_cycle" + c + "_java_saw_THIS_cycles_sentinel",
                          hr_fixture::get_last_ping_result() == want);
                ctx.check("altsentinel_cycle" + c + "_java_not_original",
                          hr_fixture::get_last_ping_result() != PING_ORIGINAL);

                vmhook::shutdown_hooks();
                const bool d_silent{ drive(ctx, MODE_PING) };
                ctx.check("altsentinel_cycle" + c + "_after_shutdown_probe_completed", d_silent);
                ctx.check("altsentinel_cycle" + c + "_after_shutdown_SILENT",
                          g_ping_fires.load() == 0);
                ctx.check("altsentinel_cycle" + c + "_after_shutdown_byte_exact_original",
                          hr_fixture::get_last_ping_result() == PING_ORIGINAL);
            }
            vmhook::shutdown_hooks();   // belt-and-braces
        }

        // =====================================================================
        //  16. NO-OP MODE liveness sanity: drive with an out-of-range mode (the
        //      fixture's switch `default:` does nothing) WHILE a hook is armed.  The
        //      probe still completes and runEpoch advances (the Java thread ran),
        //      but neither hookable method was dispatched -> the detour MUST be
        //      silent.  This proves the module's "silent" assertions elsewhere are
        //      genuinely keyed on dispatch (not on the probe failing to run), and
        //      that an armed hook firing zero times is the no-dispatch case, not a
        //      dead-detour false-negative.  Then revive on a real mode to confirm
        //      the armed hook was live all along.
        // =====================================================================
        {
            constexpr std::int32_t MODE_NONE{ 0 };
            ctx.check("noopmode_install_returns_true", install_ping_observer());
            const std::int32_t epoch_before{ hr_fixture::get_run_epoch() };
            const bool d_noop{ drive(ctx, MODE_NONE) };
            ctx.check("noopmode_probe_completed", d_noop);
            ctx.check("noopmode_epoch_advanced",
                      hr_fixture::get_run_epoch() == epoch_before + 1);
            ctx.check("noopmode_armed_hook_silent_no_dispatch", g_ping_fires.load() == 0);

            // Same armed hook, now a real mode -> it fires.  Proves it was live the
            // whole time and the silence above was pure no-dispatch.
            const bool d_real{ drive(ctx, MODE_PING) };
            ctx.check("noopmode_real_probe_completed", d_real);
            ctx.check("noopmode_armed_hook_fires_on_real_mode", g_ping_fires.load() == PING_CALLS);

            vmhook::shutdown_hooks();   // clean up
        }

        // =====================================================================
        //  9. LIVENESS WITNESS + verify_hooks() safe-after-revive.  Across all the
        //     cycles above the Java probe must have actually run many times
        //     (runEpoch monotonic), independent of detour firing -- so a "silent"
        //     assertion can never be silently passing because the probe never ran.
        //     And verify_hooks() on the now-clean state is a safe no-op == 0.
        // =====================================================================
        {
            const std::int32_t epoch{ hr_fixture::get_run_epoch() };
            // The expanded module drives well over forty probe cycles (scenarios
            // 1-8 plus the added 10-16); keep a conservative floor that is still a
            // strong non-vacuous witness -- if it is not met, the Java probe was
            // not running and every "silent" assertion above is suspect.
            ctx.check("liveness_run_epoch_advanced", epoch >= 30);
            ctx.record(std::string{ "[INFO] hook_reinstall_after_shutdown: total Java run() epochs = " }
                       + std::to_string(epoch));

            // EMPTY-SET verify on the now-clean table (every scenario above ended
            // with shutdown_hooks()).  In principle the table is empty here so a
            // single-instant read is correct; but a sibling module run earlier in
            // the suite may have left a method warm and an async recompile could
            // race -- use the bounded settle (the only LIVE/near-live verify check
            // in the module) so this can never flake.  Still NON-vacuous: a real
            // unsettleable drift keeps every pass non-zero and this fails.
            ctx.check("post_revive_verify_hooks_zero_is_safe", verify_settles_zero(12));
        }
    }
}

// priority::last — this module deliberately calls the GLOBAL vmhook::shutdown_hooks()
// to exercise the revive-after-teardown lifecycle.  Running it dead-last guarantees no
// sibling module still has a hook armed when it tears the world down (the strongest
// isolation for the module the Wave-3 cascade was originally pinned on); its own hooks
// are torn down by the unconditional final shutdown_hooks() regardless.
VMHOOK_JVM_MODULE_PRIORITY(hook_reinstall_after_shutdown, vmhook_test::priority::last)
{
    // Run the whole body under a try/catch so a stray throw from any vmhook call
    // can never escape this module and tear down the JVM (a crash voids the whole
    // run's TOTAL line -- exactly the Wave-3 cascade).  A throw is recorded as
    // [INFO], never a FAIL -- mirrors register_class.cpp / aaa_warmup.cpp.
    bool body_threw{ false };
    try
    {
        run_hook_reinstall_after_shutdown_checks(ctx);
    }
    catch (...)
    {
        body_threw = true;
    }

    // FINAL CLEANUP -- belt-and-braces, OUTSIDE the try so it ALWAYS runs.  Other
    // modules run after this one, so the module MUST leave ZERO hooks armed and
    // the library REVIVABLE.  Every scenario already tears down (and the only
    // scoped_hook handles RAII-uninstall at their scope exit); this unconditional
    // shutdown_hooks() guarantees an empty, revivable hook table even if the body
    // threw before reaching a teardown (idempotent + safe-when-empty, proven by
    // shutdown_hooks_teardown).  Here it doubles as the feature's own teardown.
    // A leaked armed hook is exactly what cascaded into later modules in Wave 3;
    // this module cannot leak one on ANY path.
    vmhook::shutdown_hooks();

    // Restore the suite-wide invariant: background watchdog OFF for every other
    // module.  We re-enabled it at the top of the body to make section 1's
    // g_started lifecycle genuine; turn it back off here, OUTSIDE the try, so it
    // runs on EVERY exit path (early return, throw, or normal completion).
    // shutdown_hooks() above already stopped any live thread; this re-establishes
    // the off state run_all() set before the module loop (and stops a stray
    // watchdog belt-and-braces).
    vmhook::set_auto_repair_enabled(false);

    if (body_threw)
    {
        ctx.record("[INFO] hook_reinstall_after_shutdown: the test body threw and was "
                   "contained (no crash, no hooks armed); see preceding checks for "
                   "partial results.");
    }
    // EMPTY-SET verify on the just-cleared table -> HARD.  The final shutdown
    // flag must be clear so the library is revivable for the modules after us.
    ctx.check("module_left_clean_final_verify_zero", vmhook::verify_hooks() == 0);
    ctx.check("module_left_clean_final_shutdown_flag_false",
              vmhook::hotspot::g_shutdown_requested.load(std::memory_order_acquire) == false);
}
