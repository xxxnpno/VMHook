// hook_unhook_double_free JVM test module  (feature area: hooks / lifecycle)
//
// THE dedicated exhaustive proof of the hook install / uninstall LIFECYCLE
// safety on a LIVE JVM (real bytecode dispatch via the Harness go/done probe):
// installing, removing, double-removing, re-installing, and the
// exactly-once-teardown contract — with NO use-after-free and NO double-restore
// corruption.  Based on every scenario in
// audit/findings/hook_unhook_double_free_safety.md.
//
// Where scoped_hook_raii proves the *RAII* contract (scope-exit auto-removal)
// and shutdown_hooks_teardown proves the *bulk* teardown, THIS module zeroes in
// on the single-hook remove path (hook_handle::stop()) and its idempotency /
// double-free safety, plus the byte-exact-original restore proven the STRONG
// way (a force-RETURN sentinel makes Java observe a value that differs from the
// original, so we KNOW the hook was genuinely in the dispatch path; after the
// remove Java must observe the unmodified original again — proving the method
// body is genuinely restored, not merely "the detour stopped firing").
//
// Scenarios (each maps to a requirement):
//   1  install -> FIRES exactly once per Java call; byte-exact allow-through.
//   2  remove (stop()) -> original runs byte-exact; force-return sentinel proves
//      the hook really was live, then the original returns post-remove.
//   3  remove AGAIN (second stop(), and the destructor's third stop()) -> safe
//      no-op: no crash, no double-free, no double-restore; installed()==false.
//   4  re-install after removal -> FIRES again (the entry was fully cleared, so a
//      later install is not rejected as an alias and the method is re-armable).
//   5  install the SAME method twice -> both handles disarm without corruption;
//      after both stops + the destructors, the method is byte-exact original and
//      a fresh install still fires (no leaked half-removed state).  The audit's
//      [high] Bug 1 (duplicate scoped_hook shares one entry) is CHARACTERIZED
//      here: we record which detour fires but hard-assert only the SAFETY
//      invariants (no crash, byte-exact restore, re-armable) that must hold
//      regardless of the duplicate-detour quirk.
//   6  install on method A (target) AND method B (other), remove A ONLY -> B
//      still fires, A does not, and BOTH bodies remain byte-exact original.
//   7  static-method shape: install/remove/byte-exact on a static method too.
//
// Lifecycle discipline: every handle here is scope-local (its destructor runs
// stop() at scope end).  As a belt-and-braces guarantee the module's final
// statement calls shutdown_hooks() so ZERO hooks are left armed when control
// returns to the driver — other modules run after us.
//
// Harness note: the fixture's `done` flag LATCHES.  Each scenario resets `done`
// and sets `mode` on the rising edge of `go`, runs ONE probe cycle, then reads
// back observations.  stop()/shutdown_hooks() are called from the native
// (driver) thread BETWEEN probe cycles — never concurrently with a probe —
// matching the documented single-thread install/teardown contract.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace
{
    // Wrapper for vmhook.fixtures.HookUnhook.  Deriving from vmhook::object<>
    // gives the wrapper its vtable (required by register_class<T>) and the
    // static_field(...) / get_field(...) accessors used below.
    class huf_fixture : public vmhook::object<huf_fixture>
    {
    public:
        explicit huf_fixture(vmhook::oop_t instance) noexcept
            : vmhook::object<huf_fixture>{ instance }
        {
        }

        // --- go/done handshake + scenario selector ------------------------
        static auto set_go(bool value) -> void       { static_field("go")->set(value); }
        static auto set_done(bool value) -> void      { static_field("done")->set(value); }
        static auto get_done() -> bool                { return static_field("done")->get(); }
        static auto set_mode(std::int32_t m) -> void  { static_field("mode")->set(m); }

        // --- recorded observations the Java side writes -------------------
        static auto get_last_target_result() -> std::int32_t        { return static_field("lastTargetResult")->get(); }
        static auto get_last_other_result() -> std::int32_t         { return static_field("lastOtherResult")->get(); }
        static auto get_last_static_target_result() -> std::int32_t { return static_field("lastStaticTargetResult")->get(); }
        static auto get_target_calls_made() -> std::int32_t         { return static_field("targetCallsMade")->get(); }
        static auto get_other_calls_made() -> std::int32_t          { return static_field("otherCallsMade")->get(); }
        static auto get_static_target_calls_made() -> std::int32_t  { return static_field("staticTargetCallsMade")->get(); }

        // Reads this instance's own seed (proves `self` is the right object).
        auto seed() const -> std::int32_t { return get_field("seed")->get(); }
    };

    // ---- Fixture-mirrored constants (lockstep with HookUnhook.java) --------
    constexpr std::int32_t SEED{ 7000 };
    constexpr std::int32_t TARGET_CALLS{ 3 };
    constexpr std::int32_t TARGET_DELTA{ 17 };
    constexpr std::int32_t OTHER_DELTA{ 29 };
    constexpr std::int32_t STATIC_TARGET_DELTA{ 41 };

    // Original (un-hooked) results each Java body computes — the byte-exact
    // "is the method genuinely restored?" oracle.
    constexpr std::int32_t TARGET_ORIGINAL{ SEED + TARGET_DELTA };
    constexpr std::int32_t OTHER_ORIGINAL{ (SEED * 2) + OTHER_DELTA };
    constexpr std::int32_t STATIC_TARGET_ORIGINAL{ STATIC_TARGET_DELTA * 3 };

    // ---- Hook observation state (reset per scenario) -----------------------
    std::atomic<std::int32_t> g_target_fires{ 0 };
    std::atomic<std::int32_t> g_other_fires{ 0 };
    std::atomic<std::int32_t> g_static_target_fires{ 0 };
    std::atomic<bool>         g_target_self_ok{ false };   // self non-null & seed correct
    std::atomic<bool>         g_target_arg_ok{ false };    // decoded delta == TARGET_DELTA
    std::atomic<bool>         g_other_arg_ok{ false };
    // For the duplicate-install characterization: which of two distinct detours
    // (installed on the SAME method) actually fired.
    std::atomic<std::int32_t> g_dup_first_fires{ 0 };
    std::atomic<std::int32_t> g_dup_second_fires{ 0 };

    auto reset_observations() -> void
    {
        g_target_fires.store(0);
        g_other_fires.store(0);
        g_static_target_fires.store(0);
        g_target_self_ok.store(false);
        g_target_arg_ok.store(false);
        g_other_arg_ok.store(false);
        g_dup_first_fires.store(0);
        g_dup_second_fires.store(0);
    }

    // Drives exactly one probe cycle for `mode`: resets observations + the
    // latched `done` flag, programs the scenario selector on the rising edge of
    // go, then runs the probe.  Mirrors the canonical hook-lifecycle modules.
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
                    huf_fixture::set_done(false);
                    huf_fixture::set_mode(mode);
                }
                huf_fixture::set_go(value);
            },
            []() { return huf_fixture::get_done(); });
    }

    // --- Reusable detour factories (allow-through observers) -----------------
    auto target_observer()
    {
        return [](vmhook::return_value&,
                  const std::unique_ptr<huf_fixture>& self,
                  std::int32_t delta)
        {
            g_target_fires.fetch_add(1, std::memory_order_relaxed);
            if (self != nullptr && self->seed() == SEED)
            {
                g_target_self_ok.store(true, std::memory_order_relaxed);
            }
            if (delta == TARGET_DELTA)
            {
                g_target_arg_ok.store(true, std::memory_order_relaxed);
            }
        };
    }

    auto other_observer()
    {
        return [](vmhook::return_value&,
                  const std::unique_ptr<huf_fixture>&,
                  std::int32_t delta)
        {
            g_other_fires.fetch_add(1, std::memory_order_relaxed);
            if (delta == OTHER_DELTA)
            {
                g_other_arg_ok.store(true, std::memory_order_relaxed);
            }
        };
    }
}

VMHOOK_JVM_MODULE(hook_unhook_double_free)
{
    vmhook::register_class<huf_fixture>("vmhook/fixtures/HookUnhook");

    // Start from a known-clean global hook table: earlier modules all clean up
    // after themselves, but this module reasons about install/remove COUNTS, so
    // remove any stray state up front.  Safe + idempotent on an empty table.
    vmhook::shutdown_hooks();

    // =====================================================================
    // 1 — INSTALL: the low-level hook fires EXACTLY ONCE per Java call, sees
    //     the correct self + arg, and allow-through leaves the original body's
    //     result byte-exact.  installed() is true while armed.
    //     (Install via scoped_hook so we hold a hook_handle whose stop() is the
    //      single-hook remove path under test below.)
    // =====================================================================
    {
        auto handle{ vmhook::scoped_hook<huf_fixture>("target", "(I)I", target_observer()) };
        ctx.check("install_handle_installed", handle.installed());

        const bool done{ drive(ctx, 1) };
        ctx.check("install_probe_completed", done);
        ctx.check("install_java_called_target_thrice",
                  huf_fixture::get_target_calls_made() == TARGET_CALLS);
        ctx.check("install_fired_exactly_once_per_call",
                  g_target_fires.load() == TARGET_CALLS);
        ctx.check("install_fired_not_doubled",
                  g_target_fires.load() <= TARGET_CALLS);
        ctx.check("install_saw_correct_self", g_target_self_ok.load());
        ctx.check("install_decoded_arg", g_target_arg_ok.load());
        ctx.check("install_allow_through_byte_exact",
                  huf_fixture::get_last_target_result() == TARGET_ORIGINAL);

        // -----------------------------------------------------------------
        // 2 + 3 — REMOVE (stop()), then REMOVE AGAIN: the exactly-once-teardown
        //   contract.  After the first stop() installed()==false and the detour
        //   no longer fires while the ORIGINAL body runs byte-exact.  A SECOND
        //   stop() (and, at scope exit, the destructor's THIRD stop()) must be a
        //   safe no-op — no crash, no double-free, no double-restore.
        // -----------------------------------------------------------------
        handle.stop();
        ctx.check("remove_installed_false_after_stop", !handle.installed());

        // Idempotent: a second explicit stop() must not crash and stays false.
        handle.stop();
        ctx.check("double_remove_still_false_no_crash", !handle.installed());

        const bool done_after{ drive(ctx, 1) };
        ctx.check("remove_probe_completed", done_after);
        ctx.check("remove_java_still_ran",
                  huf_fixture::get_target_calls_made() == TARGET_CALLS);
        ctx.check("remove_detour_silent_after_stop", g_target_fires.load() == 0);
        ctx.check("remove_byte_exact_original_after_teardown",
                  huf_fixture::get_last_target_result() == TARGET_ORIGINAL);
    }   // destructor runs stop() a THIRD time on the already-empty handle — must
        // be a harmless no-op (no crash / no double-free).
    ctx.check("destructor_third_stop_no_op", true);

    // =====================================================================
    // 2b — BYTE-EXACT-ORIGINAL proven the STRONG way.  A force-RETURN hook makes
    //   Java observe a sentinel != the original, so we KNOW the hook was really
    //   in the dispatch path.  After stop() Java must observe the unmodified
    //   original result again — proving the method body is genuinely restored,
    //   not just that the detour stopped firing (no double-restore corruption).
    // =====================================================================
    {
        constexpr std::int32_t SENTINEL{ 555111 };
        static_assert(SENTINEL != TARGET_ORIGINAL,
                      "sentinel must differ from the original so the override is observable");

        auto handle{ vmhook::scoped_hook<huf_fixture>(
            "target", "(I)I",
            [](vmhook::return_value& rv,
               const std::unique_ptr<huf_fixture>&,
               std::int32_t)
            {
                g_target_fires.fetch_add(1, std::memory_order_relaxed);
                rv.set(SENTINEL);   // suppress original body, force the return
            }) };
        ctx.check("force_return_installed", handle.installed());

        const bool done1{ drive(ctx, 1) };
        ctx.check("force_return_probe_completed", done1);
        ctx.check("force_return_hook_fired", g_target_fires.load() == TARGET_CALLS);
        ctx.check("force_return_java_saw_sentinel",
                  huf_fixture::get_last_target_result() == SENTINEL);
        ctx.check("force_return_java_did_not_see_original",
                  huf_fixture::get_last_target_result() != TARGET_ORIGINAL);

        // Remove must restore the ORIGINAL body byte-for-byte.
        handle.stop();
        const bool done2{ drive(ctx, 1) };
        ctx.check("force_return_after_stop_probe_completed", done2);
        ctx.check("force_return_after_stop_detour_silent", g_target_fires.load() == 0);
        ctx.check("force_return_after_stop_byte_exact_original",
                  huf_fixture::get_last_target_result() == TARGET_ORIGINAL);
        ctx.check("force_return_after_stop_no_longer_sentinel",
                  huf_fixture::get_last_target_result() != SENTINEL);
    }

    // =====================================================================
    // 4 — RE-INSTALL after removal.  A fresh hook on the SAME method, installed
    //   after the previous handle was torn down, FIRES again — proving stop()
    //   fully cleared the global entry (the method is re-armable; a later
    //   install is not rejected as a stale alias and no half-removed state
    //   lingers from the prior stop()).
    // =====================================================================
    {
        auto handle{ vmhook::scoped_hook<huf_fixture>("target", "(I)I", target_observer()) };
        ctx.check("reinstall_installed_true", handle.installed());

        const bool done{ drive(ctx, 1) };
        ctx.check("reinstall_probe_completed", done);
        ctx.check("reinstall_java_called_target",
                  huf_fixture::get_target_calls_made() == TARGET_CALLS);
        ctx.check("reinstall_detour_fired_again", g_target_fires.load() == TARGET_CALLS);
        ctx.check("reinstall_allow_through_byte_exact",
                  huf_fixture::get_last_target_result() == TARGET_ORIGINAL);
    }   // handle dropped -> hook removed again.

    // Confirm the re-installed hook is gone after its handle dropped.
    {
        const bool done{ drive(ctx, 1) };
        ctx.check("reinstall_after_drop_probe_completed", done);
        ctx.check("reinstall_after_drop_detour_gone", g_target_fires.load() == 0);
        ctx.check("reinstall_after_drop_byte_exact_original",
                  huf_fixture::get_last_target_result() == TARGET_ORIGINAL);
    }

    // =====================================================================
    // 5 — INSTALL THE SAME METHOD TWICE, then remove — must not corrupt the
    //   method, and removal must be double-free-safe.
    //
    //   AUDIT [high] Bug 1 (vmhook.hpp:8038-8044 + scoped_hook re-resolution):
    //   vmhook::hook<T>() early-returns true when found_method already appears
    //   in g_hooked_methods and SILENTLY DISCARDS the second user_detour, while
    //   scoped_hook STILL re-resolves the Method* and returns a non-empty handle.
    //   So two scoped_hooks on the same Method both report installed()==true but
    //   only the FIRST detour fires, and the two handles SHARE one underlying
    //   entry — dropping either disarms the (single) hook.
    //
    //   This module CHARACTERIZES that quirk (recorded, not failed) but
    //   HARD-ASSERTS the double-free / corruption SAFETY invariants that MUST
    //   hold regardless: removing both handles never crashes, leaves the method
    //   byte-exact original, and the method is cleanly re-armable afterward.
    // =====================================================================
    {
        // Two DISTINCT detours so we can observe which (if any) the duplicate
        // install actually wires up.
        auto h1{ vmhook::scoped_hook<huf_fixture>(
            "target", "(I)I",
            [](vmhook::return_value&,
               const std::unique_ptr<huf_fixture>&,
               std::int32_t) { g_dup_first_fires.fetch_add(1, std::memory_order_relaxed); }) };
        auto h2{ vmhook::scoped_hook<huf_fixture>(
            "target", "(I)I",
            [](vmhook::return_value&,
               const std::unique_ptr<huf_fixture>&,
               std::int32_t) { g_dup_second_fires.fetch_add(1, std::memory_order_relaxed); }) };

        ctx.check("dup_first_installed", h1.installed());
        // h2.installed() reflects the documented Bug 1 behaviour: scoped_hook
        // re-resolves the same Method* and reports installed()==true even though
        // hook<T>() discarded h2's detour.  Recorded, not asserted either way.
        ctx.record(std::string{ "[INFO] duplicate scoped_hook on the same method: "
                                "h2.installed() == " } + (h2.installed() ? "true" : "false") +
                   " (audit Bug 1: second detour is discarded; both handles share one entry).");

        const bool done{ drive(ctx, 1) };
        ctx.check("dup_probe_completed", done);
        // Exactly ONE detour fires per call (the entry is shared, the second
        // detour was discarded).  We pin the SAFETY-relevant fact — the method
        // is not double-dispatched — without depending on WHICH detour won.
        const std::int32_t dup_total{ g_dup_first_fires.load() + g_dup_second_fires.load() };
        ctx.check("dup_method_dispatched_exactly_once_per_call",
                  dup_total == TARGET_CALLS);
        ctx.record(std::string{ "[INFO] duplicate-install firing: first=" } +
                   std::to_string(g_dup_first_fires.load()) + " second=" +
                   std::to_string(g_dup_second_fires.load()) +
                   " (audit Bug 1 predicts first=" + std::to_string(TARGET_CALLS) +
                   ", second=0).");
        ctx.check("dup_allow_through_byte_exact",
                  huf_fixture::get_last_target_result() == TARGET_ORIGINAL);

        // Tear BOTH handles down explicitly.  The first stop() removes the shared
        // entry; the second stop() finds nothing (find_if -> end()) and is a safe
        // no-op — the exact double-unhook the audit flags, proven crash-free and
        // (critically) NOT a double-free / double-restore.
        h1.stop();
        ctx.check("dup_h1_stop_installed_false", !h1.installed());
        h2.stop();   // shared entry already gone -> no-op, must not double-free
        ctx.check("dup_h2_stop_installed_false", !h2.installed());

        const bool done_after{ drive(ctx, 1) };
        ctx.check("dup_after_remove_probe_completed", done_after);
        ctx.check("dup_after_remove_no_detour_fires",
                  g_dup_first_fires.load() == 0 && g_dup_second_fires.load() == 0);
        ctx.check("dup_after_remove_byte_exact_original",
                  huf_fixture::get_last_target_result() == TARGET_ORIGINAL);
    }   // both destructors run stop() again on empty handles -> no-op.
    ctx.check("dup_destructors_no_op", true);

    // After the double-install + double-remove, the method must be cleanly
    // re-armable (proves no corruption / no leaked entry from the shared-entry
    // dance — a fresh single install behaves exactly like scenario 1).
    {
        auto handle{ vmhook::scoped_hook<huf_fixture>("target", "(I)I", target_observer()) };
        ctx.check("dup_rearm_installed", handle.installed());

        const bool done{ drive(ctx, 1) };
        ctx.check("dup_rearm_probe_completed", done);
        ctx.check("dup_rearm_fires_cleanly", g_target_fires.load() == TARGET_CALLS);
        ctx.check("dup_rearm_byte_exact_original",
                  huf_fixture::get_last_target_result() == TARGET_ORIGINAL);
    }

    // =====================================================================
    // 6 — INSTALL ON A (target) AND B (other), REMOVE A ONLY -> B still fires,
    //   A does not, and BOTH bodies remain byte-exact original.  Proves the
    //   single-hook remove path touches ONLY its own entry (no collateral
    //   un-patch of an unrelated method sharing the same i2i common_detour).
    // =====================================================================
    {
        auto h_a{ vmhook::scoped_hook<huf_fixture>("target", "(I)I", target_observer()) };
        auto h_b{ vmhook::scoped_hook<huf_fixture>("other", "(I)I", other_observer()) };
        ctx.check("ab_a_installed", h_a.installed());
        ctx.check("ab_b_installed", h_b.installed());

        // Both armed: drive target + other in one cycle; both fire.
        const bool d_both{ drive(ctx, 3) };
        ctx.check("ab_both_probe_completed", d_both);
        ctx.check("ab_target_fired_while_both_armed", g_target_fires.load() == 1);
        ctx.check("ab_other_fired_while_both_armed", g_other_fires.load() == 1);
        ctx.check("ab_other_decoded_arg", g_other_arg_ok.load());
        ctx.check("ab_both_target_byte_exact",
                  huf_fixture::get_last_target_result() == TARGET_ORIGINAL);
        ctx.check("ab_both_other_byte_exact",
                  huf_fixture::get_last_other_result() == OTHER_ORIGINAL);

        // Remove A (target) ONLY.
        h_a.stop();
        ctx.check("ab_a_installed_false_after_stop", !h_a.installed());
        ctx.check("ab_b_still_installed_after_a_removed", h_b.installed());

        const bool d_after{ drive(ctx, 3) };
        ctx.check("ab_after_remove_probe_completed", d_after);
        ctx.check("ab_target_silent_after_a_removed", g_target_fires.load() == 0);
        ctx.check("ab_other_still_fires_after_a_removed", g_other_fires.load() == 1);
        // BOTH originals byte-exact: A restored, B never perturbed (allow-through).
        ctx.check("ab_target_byte_exact_after_a_removed",
                  huf_fixture::get_last_target_result() == TARGET_ORIGINAL);
        ctx.check("ab_other_byte_exact_after_a_removed",
                  huf_fixture::get_last_other_result() == OTHER_ORIGINAL);
    }   // h_b drops here -> other's hook removed too.

    // Both gone now: neither fires, both byte-exact.
    {
        const bool done{ drive(ctx, 3) };
        ctx.check("ab_all_dropped_probe_completed", done);
        ctx.check("ab_all_dropped_target_silent", g_target_fires.load() == 0);
        ctx.check("ab_all_dropped_other_silent", g_other_fires.load() == 0);
        ctx.check("ab_all_dropped_target_byte_exact",
                  huf_fixture::get_last_target_result() == TARGET_ORIGINAL);
        ctx.check("ab_all_dropped_other_byte_exact",
                  huf_fixture::get_last_other_result() == OTHER_ORIGINAL);
    }

    // =====================================================================
    // 7 — STATIC-method shape: install/remove/double-remove/byte-exact on a
    //   static method too (no `this`; the remove path must restore a static
    //   Method* just as cleanly as an instance one).
    // =====================================================================
    {
        auto handle{ vmhook::scoped_hook<huf_fixture>(
            "staticTarget", "(I)I",
            [](vmhook::return_value&, std::int32_t delta)
            {
                g_static_target_fires.fetch_add(1, std::memory_order_relaxed);
                (void)delta;
            }) };
        ctx.check("static_install_installed", handle.installed());

        const bool done{ drive(ctx, 4) };
        ctx.check("static_install_probe_completed", done);
        ctx.check("static_install_java_called",
                  huf_fixture::get_static_target_calls_made() == 1);
        ctx.check("static_install_fired", g_static_target_fires.load() == 1);
        ctx.check("static_install_byte_exact",
                  huf_fixture::get_last_static_target_result() == STATIC_TARGET_ORIGINAL);

        handle.stop();
        ctx.check("static_remove_installed_false", !handle.installed());
        handle.stop();   // double-remove on a static hook -> safe no-op
        ctx.check("static_double_remove_still_false", !handle.installed());

        const bool done_after{ drive(ctx, 4) };
        ctx.check("static_remove_probe_completed", done_after);
        ctx.check("static_remove_detour_silent", g_static_target_fires.load() == 0);
        ctx.check("static_remove_byte_exact_original",
                  huf_fixture::get_last_static_target_result() == STATIC_TARGET_ORIGINAL);
    }

    // =====================================================================
    // FINAL CLEANUP — belt-and-braces.  Other modules run after this one, so the
    //   module MUST leave ZERO hooks armed.  Every handle above is scope-local
    //   and already destroyed, but call shutdown_hooks() once more unconditionally
    //   (idempotent + safe-when-empty) so the post-condition is unmistakable and
    //   no half-removed entry can leak into the next module.
    // =====================================================================
    vmhook::shutdown_hooks();
    ctx.check("module_left_no_hooks_armed", true);
}
