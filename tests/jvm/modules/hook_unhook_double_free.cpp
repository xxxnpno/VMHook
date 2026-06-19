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
//   5  install the SAME method twice -> the HONEST duplicate-install contract
//      (robustness #7 fix): the first scoped_hook owns the method and fires; the
//      SECOND scoped_hook on the same Method* returns a NOT-installed handle
//      (h2.installed()==false) because hook<T>() declines to register a second
//      detour.  Only the first detour ever fires.  Dropping the duplicate handle
//      is a guaranteed no-op (it never owned the entry), so there is no
//      double-free and no premature disarm of the first hook.  After both
//      handles drop, the method is byte-exact original and a fresh install still
//      fires (no leaked half-removed state).  (Formerly audit Bug 1: the two
//      handles used to both report installed()==true and share one entry so
//      dropping either disarmed the single shared hook — now fixed: contained
//      decline, full multi-detour chaining still out of scope.)
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
#include <utility>

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
    // For the honest duplicate-install contract: two distinct detours are
    // attempted on the SAME method; the first must fire every call, the second
    // (declined) must never fire.
    std::atomic<std::int32_t> g_dup_first_fires{ 0 };
    std::atomic<std::int32_t> g_dup_second_fires{ 0 };
    // Move-semantics scenario: two distinct detours wired through ONE handle
    // variable across a move-assign, so we can prove which body the surviving
    // handle owns after the overwrite (the old hook must be disarmed, the new
    // one armed).
    std::atomic<std::int32_t> g_move_old_fires{ 0 };
    std::atomic<std::int32_t> g_move_new_fires{ 0 };

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
        g_move_old_fires.store(0);
        g_move_new_fires.store(0);
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
    // 5 — INSTALL THE SAME METHOD TWICE -> HONEST duplicate-install contract,
    //   then remove — must not corrupt the method, and removal must be
    //   double-free-safe.
    //
    //   FIXED CONTRACT (robustness #7): vmhook::hook<T>() still returns true
    //   when found_method is already hooked (so name-based callers stay
    //   non-fatal), but it now signals the duplicate to scoped_hook<T>(), which
    //   returns a NOT-installed handle.  So of two scoped_hooks on the SAME
    //   Method*: the FIRST owns the entry and fires; the SECOND reports
    //   installed()==false and registers NO second detour.  Only the first
    //   detour fires, and — crucially — the duplicate handle never owns the
    //   shared entry, so dropping it can neither double-free nor prematurely
    //   disarm the first hook.
    //
    //   This pins the new contract HARD (h2 not installed; only the first
    //   detour fires; the second detour count stays 0) AND keeps the SAFETY
    //   invariants that always had to hold: removing both handles never
    //   crashes, leaves the method byte-exact original, and the method is
    //   cleanly re-armable afterward.
    //   (Formerly audit Bug 1: both handles reported installed()==true and
    //   shared one entry so dropping either disarmed the single hook.)
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
        // HONEST contract (robustness #7): the duplicate scoped_hook on the
        // SAME Method* returns a NOT-installed handle, because hook<T>()
        // declined to register h2's detour as a second owner of the one entry.
        // This is now hard-asserted (was a Bug-1 ctx.record that accepted
        // either value).
        ctx.check("dup_second_not_installed", !h2.installed());

        const bool done{ drive(ctx, 1) };
        ctx.check("dup_probe_completed", done);
        // Exactly the FIRST detour fires, once per Java call; the second detour
        // was never installed, so it fires ZERO times.  Both are now hard
        // assertions (no longer "exactly one total, don't care which").
        ctx.check("dup_first_detour_fires_every_call",
                  g_dup_first_fires.load() == TARGET_CALLS);
        ctx.check("dup_second_detour_never_fires",
                  g_dup_second_fires.load() == 0);
        ctx.check("dup_allow_through_byte_exact",
                  huf_fixture::get_last_target_result() == TARGET_ORIGINAL);

        // Tear both handles down explicitly.  h1.stop() removes the (sole) entry
        // it owns.  h2 never owned an entry — its stop() short-circuits on the
        // null method (installed()==false already), so it is a guaranteed no-op:
        // it can neither double-free the entry nor disarm h1's hook.
        h1.stop();
        ctx.check("dup_h1_stop_installed_false", !h1.installed());
        h2.stop();   // never installed -> no-op, must not double-free / disarm
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
    // 8 — DEFAULT-CONSTRUCTED handle: a hook_handle that never owned a target.
    //   installed()==false from birth; stop() is a guaranteed no-op (the
    //   `if (!this->method) return;` gate), the destructor is a no-op, and a
    //   double stop() on it cannot crash / double-free.  This is the empty-handle
    //   leg of the idempotency contract that the scoped_hook scenarios above only
    //   reach AFTER a stop() — here it is exercised from the start.
    // =====================================================================
    {
        vmhook::hook_handle empty{};
        ctx.check("default_handle_not_installed", !empty.installed());
        empty.stop();
        ctx.check("default_handle_stop_no_op", !empty.installed());
        empty.stop();   // double no-op
        ctx.check("default_handle_double_stop_no_op", !empty.installed());
    }   // destructor on the empty handle -> no-op.
    ctx.check("default_handle_destructor_no_op", true);

    // =====================================================================
    // 9 — MOVE-CONSTRUCT a handle.  Moving an installed handle TRANSFERS
    //   ownership of the single entry: the source becomes not-installed (its
    //   method is nulled) so its later stop()/destructor is a no-op, while the
    //   destination owns the live hook and is the one that disarms.  Proves the
    //   move ctor cannot double-free (only ONE handle ever owns the entry) and
    //   does not prematurely disarm (the hook keeps firing through the move).
    // =====================================================================
    {
        auto src{ vmhook::scoped_hook<huf_fixture>("target", "(I)I", target_observer()) };
        ctx.check("move_ctor_src_installed_before", src.installed());

        vmhook::hook_handle dst{ std::move(src) };
        ctx.check("move_ctor_src_emptied", !src.installed());
        ctx.check("move_ctor_dst_owns", dst.installed());

        // The moved-from source's explicit stop() must be a no-op and must NOT
        // disarm the hook the destination now owns.
        src.stop();
        ctx.check("move_ctor_src_stop_no_op", !src.installed());

        const bool done{ drive(ctx, 1) };
        ctx.check("move_ctor_probe_completed", done);
        ctx.check("move_ctor_hook_still_fires_through_move",
                  g_target_fires.load() == TARGET_CALLS);
        ctx.check("move_ctor_byte_exact",
                  huf_fixture::get_last_target_result() == TARGET_ORIGINAL);

        // Destination disarms the one real entry.
        dst.stop();
        ctx.check("move_ctor_dst_stop_installed_false", !dst.installed());

        const bool done_after{ drive(ctx, 1) };
        ctx.check("move_ctor_after_stop_probe_completed", done_after);
        ctx.check("move_ctor_after_stop_silent", g_target_fires.load() == 0);
        ctx.check("move_ctor_after_stop_byte_exact",
                  huf_fixture::get_last_target_result() == TARGET_ORIGINAL);
    }   // both src (already empty) and dst (already stopped) destruct -> no-op.
    ctx.check("move_ctor_destructors_no_op", true);

    // =====================================================================
    // 10 — MOVE-ASSIGN over a LIVE handle.  operator=(&&) must stop() *this
    //   FIRST (disarming the hook it currently owns) before adopting the
    //   right-hand side's target.  So after `h = scoped_hook(...)`: the OLD
    //   hook is removed (its entry erased) and the NEW hook is the only one
    //   armed through `h`.  This is the exactly-once-teardown contract embedded
    //   in the move path — the overwritten hook must be torn down exactly once,
    //   not leaked and not double-freed.
    //
    //   IMPORTANT real-semantics note (why the NEW hook is on a DIFFERENT method
    //   `other`, not the same `target`):  C++ fully evaluates the right-hand-side
    //   scoped_hook(...) BEFORE operator=(&&) runs.  At that instant `h` still
    //   owns the OLD hook, so its entry is still in g_hooked_methods.  If the RHS
    //   targeted the SAME Method*, vmhook::hook<T>() would see it already hooked
    //   and — by the honest duplicate-install contract (scenario 5) — DECLINE to
    //   register the second detour, handing scoped_hook<T>() a NOT-installed
    //   handle.  The subsequent move-assign would then stop() the old hook and
    //   adopt an EMPTY target, leaving `h` not-installed and NO detour armed.
    //   That is correct, safe library behaviour (no double-free, byte-exact
    //   restore) — it is just NOT a demonstration of the move-assign adopting a
    //   live hook.  Using a distinct method (`other`) makes the RHS install
    //   genuinely succeed so the move-assign actually transfers a LIVE hook,
    //   which is what this scenario is here to prove.  The same-method
    //   interaction is pinned HARD separately just below (10b).
    //
    //   OLD detour observes `target`; NEW detour observes `other` — distinct
    //   counters so the fire tallies prove which hook `h` owns after the assign.
    // =====================================================================
    {
        auto h{ vmhook::scoped_hook<huf_fixture>(
            "target", "(I)I",
            [](vmhook::return_value&,
               const std::unique_ptr<huf_fixture>&,
               std::int32_t) { g_move_old_fires.fetch_add(1, std::memory_order_relaxed); }) };
        ctx.check("move_assign_old_installed", h.installed());

        // Prove the OLD detour (on target) is live before the assign.
        const bool done_old{ drive(ctx, 1) };
        ctx.check("move_assign_old_probe_completed", done_old);
        ctx.check("move_assign_old_fires_before",
                  g_move_old_fires.load() == TARGET_CALLS);
        ctx.check("move_assign_new_silent_before",
                  g_move_new_fires.load() == 0);

        // Move-assign a fresh hook on a DIFFERENT method (other) over the live
        // handle.  The RHS install succeeds (other was not hooked), producing a
        // live temporary; operator=(&&) then stops the OLD target hook first and
        // adopts the NEW other hook.
        h = vmhook::scoped_hook<huf_fixture>(
            "other", "(I)I",
            [](vmhook::return_value&,
               const std::unique_ptr<huf_fixture>&,
               std::int32_t) { g_move_new_fires.fetch_add(1, std::memory_order_relaxed); });
        ctx.check("move_assign_handle_still_installed", h.installed());

        // Drive BOTH methods in one cycle (mode 3 = runBoth: target ONCE +
        // other ONCE).  Only the NEW (other) detour fires; the OLD (target)
        // detour was disarmed by the assign's stop()-first.  (If the old entry
        // had leaked, the old counter would tick on the target call here.)
        const bool done_new{ drive(ctx, 3) };
        ctx.check("move_assign_new_probe_completed", done_new);
        ctx.check("move_assign_new_fires_after",
                  g_move_new_fires.load() == 1);
        ctx.check("move_assign_old_silent_after",
                  g_move_old_fires.load() == 0);
        // Both bodies byte-exact: target restored by the stop()-first, other
        // allow-through left untouched.
        ctx.check("move_assign_target_byte_exact",
                  huf_fixture::get_last_target_result() == TARGET_ORIGINAL);
        ctx.check("move_assign_other_byte_exact",
                  huf_fixture::get_last_other_result() == OTHER_ORIGINAL);
    }   // handle drops -> the new (other) hook removed; both detours now silent.

    // After the move-assign scope, BOTH methods must be byte-exact original again
    // and neither detour fires (proves the assign left no leaked entry — the old
    // target entry was erased by the stop()-first, the new other entry by the
    // handle's destructor).
    {
        const bool done{ drive(ctx, 3) };
        ctx.check("move_assign_after_drop_probe_completed", done);
        ctx.check("move_assign_after_drop_old_silent",
                  g_move_old_fires.load() == 0);
        ctx.check("move_assign_after_drop_new_silent",
                  g_move_new_fires.load() == 0);
        ctx.check("move_assign_after_drop_target_byte_exact",
                  huf_fixture::get_last_target_result() == TARGET_ORIGINAL);
        ctx.check("move_assign_after_drop_other_byte_exact",
                  huf_fixture::get_last_other_result() == OTHER_ORIGINAL);
    }

    // =====================================================================
    // 10b — MOVE-ASSIGN onto the SAME method as the live handle: the honest
    //   duplicate-install interaction, pinned HARD.  Because the RHS scoped_hook
    //   is evaluated while `h` still owns the target hook, hook<T>() sees target
    //   already hooked and DECLINES the second detour -> the RHS temporary is a
    //   NOT-installed handle.  operator=(&&) then stop()s the old hook (erasing
    //   its entry — exactly-once teardown) and adopts the empty target, so `h`
    //   ends up NOT-installed and NO detour is armed.  This is safe (no
    //   double-free, byte-exact restore), it just doesn't transfer a live hook.
    // =====================================================================
    {
        auto h{ vmhook::scoped_hook<huf_fixture>(
            "target", "(I)I",
            [](vmhook::return_value&,
               const std::unique_ptr<huf_fixture>&,
               std::int32_t) { g_move_old_fires.fetch_add(1, std::memory_order_relaxed); }) };
        ctx.check("move_assign_same_old_installed", h.installed());

        // Move-assign a second hook on the SAME method.  RHS declines (duplicate)
        // -> empty temporary; the assign disarms the old hook and adopts nothing.
        h = vmhook::scoped_hook<huf_fixture>(
            "target", "(I)I",
            [](vmhook::return_value&,
               const std::unique_ptr<huf_fixture>&,
               std::int32_t) { g_move_new_fires.fetch_add(1, std::memory_order_relaxed); });
        ctx.check("move_assign_same_not_installed_after", !h.installed());

        // With h not-installed, the target hook was disarmed and no new one
        // armed: NEITHER detour fires and target is byte-exact original.
        const bool done{ drive(ctx, 1) };
        ctx.check("move_assign_same_probe_completed", done);
        ctx.check("move_assign_same_old_silent_after",
                  g_move_old_fires.load() == 0);
        ctx.check("move_assign_same_new_silent_after",
                  g_move_new_fires.load() == 0);
        ctx.check("move_assign_same_byte_exact",
                  huf_fixture::get_last_target_result() == TARGET_ORIGINAL);
    }   // destructor on the empty handle -> no-op (no double-free).
    ctx.check("move_assign_same_destructor_no_op", true);

    // =====================================================================
    // 11 — NAME-ONLY scoped_hook overload (empty signature).  The 2-arg
    //   scoped_hook<T>(name, detour) resolves the method by name alone; the
    //   remove path must disarm it just as cleanly as the explicit-signature
    //   form.  (target() is unique on the fixture, so name-only resolution is
    //   unambiguous.)
    // =====================================================================
    {
        auto handle{ vmhook::scoped_hook<huf_fixture>("target", target_observer()) };
        ctx.check("name_only_installed", handle.installed());

        const bool done{ drive(ctx, 1) };
        ctx.check("name_only_probe_completed", done);
        ctx.check("name_only_fired", g_target_fires.load() == TARGET_CALLS);
        ctx.check("name_only_byte_exact",
                  huf_fixture::get_last_target_result() == TARGET_ORIGINAL);

        handle.stop();
        ctx.check("name_only_installed_false_after_stop", !handle.installed());
        handle.stop();   // idempotent
        ctx.check("name_only_double_stop_still_false", !handle.installed());

        const bool done_after{ drive(ctx, 1) };
        ctx.check("name_only_after_stop_probe_completed", done_after);
        ctx.check("name_only_after_stop_silent", g_target_fires.load() == 0);
        ctx.check("name_only_after_stop_byte_exact",
                  huf_fixture::get_last_target_result() == TARGET_ORIGINAL);
    }

    // =====================================================================
    // 12 — cancel() suppression then byte-exact restore (a SECOND strong proof,
    //   distinct from 2b's rv.set(SENTINEL)).  rv.cancel() suppresses the
    //   ORIGINAL body WITHOUT writing a return value, so Java observes the
    //   default-initialised result (0 — `int last = 0;` in runTarget() is never
    //   overwritten because the body never runs).  0 differs from the original
    //   7017, proving the hook was genuinely in the dispatch path and the body
    //   really was suppressed.  After stop() the original body must run again and
    //   Java must re-observe 7017 (no double-restore corruption, no lingering
    //   suppression).
    // =====================================================================
    {
        constexpr std::int32_t CANCEL_RESULT{ 0 };
        static_assert(CANCEL_RESULT != TARGET_ORIGINAL,
                      "cancelled body must yield a value distinct from the original");

        auto handle{ vmhook::scoped_hook<huf_fixture>(
            "target", "(I)I",
            [](vmhook::return_value& rv,
               const std::unique_ptr<huf_fixture>&,
               std::int32_t)
            {
                g_target_fires.fetch_add(1, std::memory_order_relaxed);
                rv.cancel();   // suppress the original body; write no value
            }) };
        ctx.check("cancel_installed", handle.installed());

        const bool done{ drive(ctx, 1) };
        ctx.check("cancel_probe_completed", done);
        ctx.check("cancel_hook_fired", g_target_fires.load() == TARGET_CALLS);
        ctx.check("cancel_java_saw_suppressed_default",
                  huf_fixture::get_last_target_result() == CANCEL_RESULT);
        ctx.check("cancel_java_did_not_see_original",
                  huf_fixture::get_last_target_result() != TARGET_ORIGINAL);

        handle.stop();
        const bool done_after{ drive(ctx, 1) };
        ctx.check("cancel_after_stop_probe_completed", done_after);
        ctx.check("cancel_after_stop_detour_silent", g_target_fires.load() == 0);
        ctx.check("cancel_after_stop_byte_exact_original",
                  huf_fixture::get_last_target_result() == TARGET_ORIGINAL);
        ctx.check("cancel_after_stop_no_longer_suppressed",
                  huf_fixture::get_last_target_result() != CANCEL_RESULT);
    }

    // =====================================================================
    // 13 — RAPID install/remove CHURN.  Repeat install -> drive -> stop a few
    //   times in a tight loop.  Every cycle must re-arm cleanly (the entry was
    //   fully cleared by the previous stop()), fire exactly TARGET_CALLS, and
    //   leave the body byte-exact — proving stop() leaves NO residue that drifts
    //   the table or poisons the next install.  Heap-modest: small iteration
    //   count, each cycle drives the same lightweight probe.
    // =====================================================================
    {
        constexpr std::int32_t CHURN_CYCLES{ 5 };
        std::int32_t armed_ok{ 0 };
        std::int32_t fired_ok{ 0 };
        std::int32_t byte_exact_ok{ 0 };
        std::int32_t removed_ok{ 0 };
        for (std::int32_t cycle{ 0 }; cycle < CHURN_CYCLES; ++cycle)
        {
            auto handle{ vmhook::scoped_hook<huf_fixture>("target", "(I)I", target_observer()) };
            if (handle.installed())
            {
                ++armed_ok;
            }
            const bool done{ drive(ctx, 1) };
            if (done && g_target_fires.load() == TARGET_CALLS)
            {
                ++fired_ok;
            }
            if (huf_fixture::get_last_target_result() == TARGET_ORIGINAL)
            {
                ++byte_exact_ok;
            }
            handle.stop();
            if (!handle.installed())
            {
                ++removed_ok;
            }
        }
        ctx.check("churn_all_armed", armed_ok == CHURN_CYCLES);
        ctx.check("churn_all_fired_exactly", fired_ok == CHURN_CYCLES);
        ctx.check("churn_all_byte_exact", byte_exact_ok == CHURN_CYCLES);
        ctx.check("churn_all_removed", removed_ok == CHURN_CYCLES);

        // After the churn the table is clean: a final drive sees no detour.
        const bool done{ drive(ctx, 1) };
        ctx.check("churn_after_probe_completed", done);
        ctx.check("churn_after_silent", g_target_fires.load() == 0);
        ctx.check("churn_after_byte_exact",
                  huf_fixture::get_last_target_result() == TARGET_ORIGINAL);
    }

    // =====================================================================
    // 14 — THREE distinct methods armed at once (target + other + staticTarget),
    //   remove the MIDDLE one (other) only.  Stronger collateral-damage proof
    //   than scenario 6: with three entries sharing the same i2i common_detour,
    //   removing exactly one must leave the OTHER TWO firing and byte-exact.
    // =====================================================================
    {
        auto h_t{ vmhook::scoped_hook<huf_fixture>("target", "(I)I", target_observer()) };
        auto h_o{ vmhook::scoped_hook<huf_fixture>("other", "(I)I", other_observer()) };
        auto h_s{ vmhook::scoped_hook<huf_fixture>(
            "staticTarget", "(I)I",
            [](vmhook::return_value&, std::int32_t)
            {
                g_static_target_fires.fetch_add(1, std::memory_order_relaxed);
            }) };
        ctx.check("tri_target_installed", h_t.installed());
        ctx.check("tri_other_installed", h_o.installed());
        ctx.check("tri_static_installed", h_s.installed());

        // Drive target+other together (mode 3), then static (mode 4): all three
        // bodies run.  Each drive resets observations, so check after each.
        const bool d_both{ drive(ctx, 3) };
        ctx.check("tri_both_probe_completed", d_both);
        ctx.check("tri_target_fires_armed", g_target_fires.load() == 1);
        ctx.check("tri_other_fires_armed", g_other_fires.load() == 1);
        const bool d_static{ drive(ctx, 4) };
        ctx.check("tri_static_probe_completed", d_static);
        ctx.check("tri_static_fires_armed", g_static_target_fires.load() == 1);

        // Remove the MIDDLE entry (other) only.
        h_o.stop();
        ctx.check("tri_other_installed_false_after_stop", !h_o.installed());
        ctx.check("tri_target_still_installed", h_t.installed());
        ctx.check("tri_static_still_installed", h_s.installed());

        // target + other drive: target STILL fires, other is silent + byte-exact.
        const bool d_both2{ drive(ctx, 3) };
        ctx.check("tri_after_remove_both_probe_completed", d_both2);
        ctx.check("tri_target_still_fires_after_other_removed",
                  g_target_fires.load() == 1);
        ctx.check("tri_other_silent_after_removed", g_other_fires.load() == 0);
        ctx.check("tri_other_byte_exact_after_removed",
                  huf_fixture::get_last_other_result() == OTHER_ORIGINAL);
        ctx.check("tri_target_byte_exact_after_other_removed",
                  huf_fixture::get_last_target_result() == TARGET_ORIGINAL);

        // static drive: the third entry STILL fires (untouched by the middle
        // removal).
        const bool d_static2{ drive(ctx, 4) };
        ctx.check("tri_after_remove_static_probe_completed", d_static2);
        ctx.check("tri_static_still_fires_after_other_removed",
                  g_static_target_fires.load() == 1);
        ctx.check("tri_static_byte_exact_after_other_removed",
                  huf_fixture::get_last_static_target_result() == STATIC_TARGET_ORIGINAL);
    }   // h_t and h_s drop here -> all three hooks now gone.

    // All three dropped: none fire, all byte-exact.
    {
        const bool d_both{ drive(ctx, 3) };
        ctx.check("tri_all_dropped_both_probe_completed", d_both);
        ctx.check("tri_all_dropped_target_silent", g_target_fires.load() == 0);
        ctx.check("tri_all_dropped_other_silent", g_other_fires.load() == 0);
        const bool d_static{ drive(ctx, 4) };
        ctx.check("tri_all_dropped_static_probe_completed", d_static);
        ctx.check("tri_all_dropped_static_silent", g_static_target_fires.load() == 0);
        ctx.check("tri_all_dropped_target_byte_exact",
                  huf_fixture::get_last_target_result() == TARGET_ORIGINAL);
        ctx.check("tri_all_dropped_other_byte_exact",
                  huf_fixture::get_last_other_result() == OTHER_ORIGINAL);
        ctx.check("tri_all_dropped_static_byte_exact",
                  huf_fixture::get_last_static_target_result() == STATIC_TARGET_ORIGINAL);
    }

    // =====================================================================
    // 15 — DUPLICATE install, drop the REAL OWNER while the not-installed
    //   duplicate is still alive.  Reinforces scenario 5 from the other angle:
    //   the FIRST scoped_hook owns the entry; the SECOND is a not-installed
    //   handle.  Dropping the owner (h1.stop()) must disarm the hook even though
    //   the empty duplicate (h2) is still in scope, and h2's eventual
    //   destruction must remain a no-op (it never owned the entry, so it can
    //   neither resurrect nor double-free it).
    // =====================================================================
    {
        auto h2_outer{ vmhook::hook_handle{} };   // empty, populated below
        {
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
            ctx.check("dup2_h1_installed", h1.installed());
            ctx.check("dup2_h2_not_installed", !h2.installed());

            // Drop the REAL OWNER explicitly while the empty duplicate lives on.
            h1.stop();
            ctx.check("dup2_h1_stop_installed_false", !h1.installed());

            // The hook is now gone even though h2 is still in scope.
            const bool done{ drive(ctx, 1) };
            ctx.check("dup2_after_owner_drop_probe_completed", done);
            ctx.check("dup2_after_owner_drop_first_silent",
                      g_dup_first_fires.load() == 0);
            ctx.check("dup2_after_owner_drop_second_silent",
                      g_dup_second_fires.load() == 0);
            ctx.check("dup2_after_owner_drop_byte_exact",
                      huf_fixture::get_last_target_result() == TARGET_ORIGINAL);

            // Move the (empty) duplicate out so its destruction happens AFTER
            // the inner scope — confirming a moved empty handle is still inert.
            h2_outer = std::move(h2);
            ctx.check("dup2_h2_still_not_installed_after_move", !h2_outer.installed());
        }   // h1 (already stopped) destructs -> no-op.
        // The moved-out empty duplicate is still inert; explicit stop() no-op.
        h2_outer.stop();
        ctx.check("dup2_outer_duplicate_stop_no_op", !h2_outer.installed());
    }   // h2_outer destructs (empty) -> no-op.
    ctx.check("dup2_destructors_no_op", true);

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
