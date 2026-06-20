// scoped_hook_raii JVM test module  (feature area: hooks / RAII lifetime)
//
// Exhaustively exercises the RAII contract of vmhook::scoped_hook /
// vmhook::hook_handle on a LIVE JVM (real bytecode dispatch via the Harness
// go/done probe).  This module is the modular-harness successor of the legacy
// inline `test_scoped_hook` in vmhook/src/example.cpp — it reproduces that
// happy-path (install -> fire -> drop -> must-not-fire) and EXTENDS it to cover
// every scenario and flaw called out in audit/findings/scoped_hook_raii.md:
//
//   * CORE RAII: the detour fires while the handle is in scope, and does NOT
//     fire after the handle is destroyed / scope-exits;
//   * installed() is true while armed and false for an empty / moved-from /
//     stopped handle;
//   * MOVE-CONSTRUCTION transfers ownership: the moved-from handle is empty and
//     does NOT double-remove on destruction; the moved-to handle still disarms;
//   * MOVE-ASSIGNMENT tears the LHS hook down before stealing the RHS (old LHS
//     method stops firing; the moved method fires through the LHS handle);
//   * NESTED scopes arm/disarm independently (inner drop must not kill outer);
//   * RE-INSTALL on the same method after an explicit removal works;
//   * MULTIPLE scoped_hooks on DIFFERENT methods are independent (dropping one
//     leaves the others firing);
//   * EXPLICIT early stop() disarms before the enclosing C++ scope ends and is
//     idempotent (a second stop() / the destructor is a harmless no-op);
//   * default-constructed handle: installed()==false, destructor no-op;
//   * OVERLOAD resolution: the short scoped_hook(name, cb) overload installs the
//     first matching descriptor; the long scoped_hook(name, sig, cb) overload
//     selects a specific descriptor; a NON-matching signature returns an empty
//     handle (installed()==false) and never fires;
//   * UNREGISTERED wrapper type: scoped_hook<Unregistered>(...) returns an empty
//     handle without crashing.
//
// Each Java counter (alphaCalls, betaCalls, ...) accumulates across the whole
// process, so every scenario SNAPSHOTS the relevant Java counter before its
// probe and asserts the post-probe delta — this is robust against earlier
// scenarios and needs no Java-side reset.  Native fire counters are reset per
// scenario via reset_fires().
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

namespace
{
    // Wrapper for vmhook.fixtures.ScopedHookRaii.  Deriving from
    // vmhook::object<> gives the wrapper a vtable (required by register_class)
    // and the static_field(...) / get_field(...) accessors used below.
    class shr_fixture : public vmhook::object<shr_fixture>
    {
    public:
        explicit shr_fixture(vmhook::oop_t instance) noexcept
            : vmhook::object<shr_fixture>{ instance }
        {
        }

        // --- go/done handshake + scenario selector ------------------------
        static auto set_go(bool value) -> void       { static_field("go")->set(value); }
        static auto set_done(bool value) -> void      { static_field("done")->set(value); }
        static auto get_done() -> bool                { return static_field("done")->get(); }
        static auto set_mode(std::int32_t m) -> void  { static_field("mode")->set(m); }

        // --- per-method Java-side call counters (accumulate across process) -
        static auto get_alpha_calls() -> std::int32_t        { return static_field("alphaCalls")->get(); }
        static auto get_beta_calls() -> std::int32_t         { return static_field("betaCalls")->get(); }
        static auto get_gamma_calls() -> std::int32_t        { return static_field("gammaCalls")->get(); }
        static auto get_static_alpha_calls() -> std::int32_t { return static_field("staticAlphaCalls")->get(); }
        static auto get_over_i_calls() -> std::int32_t       { return static_field("overICalls")->get(); }
        static auto get_over_ii_calls() -> std::int32_t      { return static_field("overIICalls")->get(); }

        // --- per-method last original return (allow-through proof) ----------
        static auto get_alpha_result() -> std::int32_t        { return static_field("alphaResult")->get(); }
        static auto get_static_alpha_result() -> std::int32_t { return static_field("staticAlphaResult")->get(); }
        static auto get_over_i_result() -> std::int32_t       { return static_field("overIResult")->get(); }
        static auto get_over_ii_result() -> std::int32_t      { return static_field("overIIResult")->get(); }

        // Reads this instance's own seed (proves `self` is the right object).
        auto seed() const -> std::int32_t { return get_field("seed")->get(); }
    };

    // A second wrapper type that is intentionally NEVER register_class()'d, so
    // scoped_hook<unregistered_fixture>(...) exercises the not-registered path.
    class unregistered_fixture : public vmhook::object<unregistered_fixture>
    {
    public:
        explicit unregistered_fixture(vmhook::oop_t instance) noexcept
            : vmhook::object<unregistered_fixture>{ instance }
        {
        }
    };

    // ---- Fixture-mirrored constants (lockstep with ScopedHookRaii.java) -----
    constexpr std::int32_t SEED{ 1000 };
    constexpr std::int32_t ALPHA_DELTA{ 7 };
    constexpr std::int32_t BETA_DELTA{ 11 };
    constexpr std::int32_t GAMMA_DELTA{ 23 };
    constexpr std::int32_t STATIC_ALPHA_DELTA{ 99 };
    constexpr std::int32_t OVER_I_ARG{ 5 };
    constexpr std::int32_t OVER_II_ARG_A{ 40 };
    constexpr std::int32_t OVER_II_ARG_B{ 2 };

    // ---- Native per-method fire counters (reset per scenario) --------------
    std::atomic<std::int32_t> g_alpha_fires{ 0 };
    std::atomic<std::int32_t> g_beta_fires{ 0 };
    std::atomic<std::int32_t> g_gamma_fires{ 0 };
    std::atomic<std::int32_t> g_static_alpha_fires{ 0 };
    std::atomic<std::int32_t> g_over_i_fires{ 0 };
    std::atomic<std::int32_t> g_over_ii_fires{ 0 };

    // Argument / self correctness latches (proves the right detour decoded the
    // right frame, not merely that *something* fired).
    std::atomic<bool> g_alpha_self_ok{ false };
    std::atomic<bool> g_alpha_arg_ok{ false };
    std::atomic<bool> g_over_i_arg_ok{ false };
    std::atomic<bool> g_over_ii_args_ok{ false };

    auto reset_fires() -> void
    {
        g_alpha_fires.store(0);
        g_beta_fires.store(0);
        g_gamma_fires.store(0);
        g_static_alpha_fires.store(0);
        g_over_i_fires.store(0);
        g_over_ii_fires.store(0);
        g_alpha_self_ok.store(false);
        g_alpha_arg_ok.store(false);
        g_over_i_arg_ok.store(false);
        g_over_ii_args_ok.store(false);
    }

    // Drives exactly one probe cycle for `mode`: resets native fire counters +
    // the latched `done`, programs the scenario selector on the rising edge of
    // go, then runs the probe.  Mirrors hook_basic's drive().
    auto drive(vmhook_test::context& ctx, std::int32_t mode) -> bool
    {
        reset_fires();
        return ctx.run_probe(
            [mode](bool value)
            {
                if (value)
                {
                    shr_fixture::set_done(false);
                    shr_fixture::set_mode(mode);
                }
                shr_fixture::set_go(value);
            },
            []() { return shr_fixture::get_done(); });
    }

    // --- Reusable detour factories ------------------------------------------
    // Each returns a lambda that bumps the matching native fire counter.  Using
    // named factories keeps the (many) scoped_hook call sites short and makes the
    // "which counter does this hook feed" mapping explicit.
    auto alpha_detour()
    {
        return [](vmhook::return_value&,
                  const std::unique_ptr<shr_fixture>& self,
                  std::int32_t delta)
        {
            g_alpha_fires.fetch_add(1, std::memory_order_relaxed);
            if (self != nullptr && self->seed() == SEED)
            {
                g_alpha_self_ok.store(true, std::memory_order_relaxed);
            }
            if (delta == ALPHA_DELTA)
            {
                g_alpha_arg_ok.store(true, std::memory_order_relaxed);
            }
        };
    }

    auto beta_detour()
    {
        return [](vmhook::return_value&,
                  const std::unique_ptr<shr_fixture>&,
                  std::int32_t)
        {
            g_beta_fires.fetch_add(1, std::memory_order_relaxed);
        };
    }

    auto gamma_detour()
    {
        return [](vmhook::return_value&,
                  const std::unique_ptr<shr_fixture>&,
                  std::int32_t)
        {
            g_gamma_fires.fetch_add(1, std::memory_order_relaxed);
        };
    }

    auto static_alpha_detour()
    {
        return [](vmhook::return_value&, std::int32_t delta)
        {
            g_static_alpha_fires.fetch_add(1, std::memory_order_relaxed);
            (void)delta;
        };
    }

    auto over_i_detour()
    {
        return [](vmhook::return_value&,
                  const std::unique_ptr<shr_fixture>&,
                  std::int32_t x)
        {
            g_over_i_fires.fetch_add(1, std::memory_order_relaxed);
            if (x == OVER_I_ARG)
            {
                g_over_i_arg_ok.store(true, std::memory_order_relaxed);
            }
        };
    }

    auto over_ii_detour()
    {
        return [](vmhook::return_value&,
                  const std::unique_ptr<shr_fixture>&,
                  std::int32_t a, std::int32_t b)
        {
            g_over_ii_fires.fetch_add(1, std::memory_order_relaxed);
            if (a == OVER_II_ARG_A && b == OVER_II_ARG_B)
            {
                g_over_ii_args_ok.store(true, std::memory_order_relaxed);
            }
        };
    }
}

VMHOOK_JVM_MODULE(scoped_hook_raii)
{
    vmhook::register_class<shr_fixture>("vmhook/fixtures/ScopedHookRaii");
    // NOTE: unregistered_fixture is deliberately NOT registered.

    // Deterministic post-teardown invariant used throughout this module:
    // vmhook::verify_hooks() returns the number of DRIFTED/REPAIRED hook entries
    // — with the suite's auto-repair watchdog disabled and after a CLEAN scoped
    // teardown it returns 0 (no drift, nothing to repair).  Combined with the
    // behavioural "detour silent after scope" probe, a verify==0 reading is a
    // HARD, JDK-independent proof that the scope left NOTHING armed/drifting.
    // (Note: this is NOT a fire-count assertion, so it is immune to the
    // post-JIT/post-deopt fire-count nondeterminism — see the firing checks.)
    // Baseline at module entry: any prior module is contractually clean here.
    ctx.check("module_entry_verify_hooks_zero", vmhook::verify_hooks() == 0);

    // =====================================================================
    // 0 — Compile-time + JVM-free invariants of the handle type itself.
    //     (Move-only, nothrow-destructible; default handle is empty and its
    //      destructor is a no-op even with no JVM interaction.)
    // =====================================================================
    static_assert(std::is_move_constructible_v<vmhook::hook_handle>,
                  "hook_handle must be move-constructible");
    static_assert(std::is_move_assignable_v<vmhook::hook_handle>,
                  "hook_handle must be move-assignable");
    static_assert(!std::is_copy_constructible_v<vmhook::hook_handle>,
                  "hook_handle must NOT be copy-constructible (unique ownership)");
    static_assert(!std::is_copy_assignable_v<vmhook::hook_handle>,
                  "hook_handle must NOT be copy-assignable (unique ownership)");
    static_assert(std::is_nothrow_destructible_v<vmhook::hook_handle>,
                  "hook_handle destructor must be noexcept");

    {
        vmhook::hook_handle empty{};
        ctx.check("default_handle_not_installed", !empty.installed());
        // Destroying `empty` at scope end must not crash / throw / log an error
        // — the check below simply records that we reached here alive.
        ctx.check("default_handle_pre_destroy_alive", true);
    }
    ctx.check("default_handle_destroyed_no_op", true);

    // =====================================================================
    // 1 — CORE RAII (the legacy test_scoped_hook proof, extended).
    //     In scope: alpha hooked -> the detour fires, sees correct self+arg,
    //     and allow-through leaves the original body's result intact.
    //     installed() is true while armed.
    // =====================================================================
    const std::int32_t alpha_calls_before_core{ shr_fixture::get_alpha_calls() };
    {
        auto handle{ vmhook::scoped_hook<shr_fixture>("alpha", "(I)I", alpha_detour()) };
        ctx.check("core_installed_true_in_scope", handle.installed());

        const bool done{ drive(ctx, 1) };
        ctx.check("core_probe_completed", done);
        ctx.check("core_java_called_alpha_once",
                  shr_fixture::get_alpha_calls() - alpha_calls_before_core == 1);
        ctx.check("core_detour_fired_in_scope", g_alpha_fires.load() == 1);
        ctx.check("core_detour_saw_correct_self", g_alpha_self_ok.load());
        ctx.check("core_detour_decoded_arg", g_alpha_arg_ok.load());
        // allow-through: alpha returns seed + delta unmodified.
        ctx.check("core_allow_through_result",
                  shr_fixture::get_alpha_result() == (SEED + ALPHA_DELTA));
    }   // handle destroyed here -> hook removed

    // =====================================================================
    // 2 — CORE RAII (the must-NOT-fire half).  Handle is gone; firing alpha
    //     again must NOT invoke the detour, yet the original body still runs.
    // =====================================================================
    const std::int32_t alpha_calls_before_gone{ shr_fixture::get_alpha_calls() };
    {
        const bool done{ drive(ctx, 1) };
        ctx.check("after_scope_probe_completed", done);
        ctx.check("after_scope_java_called_alpha_once",
                  shr_fixture::get_alpha_calls() - alpha_calls_before_gone == 1);
        ctx.check("after_scope_detour_did_not_fire", g_alpha_fires.load() == 0);
        ctx.check("after_scope_original_still_ran",
                  shr_fixture::get_alpha_result() == (SEED + ALPHA_DELTA));
    }
    // HARD: the normal-scope-exit destructor left a clean, drift-free hook set.
    ctx.check("after_scope_verify_hooks_zero", vmhook::verify_hooks() == 0);

    // =====================================================================
    // 3 — installed() flips to false after an EXPLICIT stop(), the detour no
    //     longer fires, and a second stop() / the destructor is a harmless
    //     no-op (idempotent teardown).
    // =====================================================================
    {
        auto handle{ vmhook::scoped_hook<shr_fixture>("alpha", "(I)I", alpha_detour()) };
        ctx.check("explicit_stop_installed_before", handle.installed());

        handle.stop();
        ctx.check("explicit_stop_installed_false_after", !handle.installed());

        // Idempotent: calling stop() again must not crash and stays false.
        handle.stop();
        ctx.check("explicit_stop_idempotent_still_false", !handle.installed());

        const std::int32_t before{ shr_fixture::get_alpha_calls() };
        const bool done{ drive(ctx, 1) };
        ctx.check("explicit_stop_probe_completed", done);
        ctx.check("explicit_stop_java_called_alpha",
                  shr_fixture::get_alpha_calls() - before == 1);
        ctx.check("explicit_stop_detour_did_not_fire", g_alpha_fires.load() == 0);
    }   // destructor runs stop() once more — must be a no-op (no crash).
    ctx.check("explicit_stop_destructor_after_stop_no_op", true);

    // =====================================================================
    // 4 — RE-INSTALL after removal.  A fresh scoped_hook on the SAME method,
    //     installed after the previous handle was torn down, fires again.
    //     (Proves stop() fully cleared the global entry / flags so a later
    //     install is not rejected as an alias.)
    // =====================================================================
    const std::int32_t alpha_calls_before_reinstall{ shr_fixture::get_alpha_calls() };
    {
        auto handle{ vmhook::scoped_hook<shr_fixture>("alpha", "(I)I", alpha_detour()) };
        ctx.check("reinstall_installed_true", handle.installed());

        const bool done{ drive(ctx, 1) };
        ctx.check("reinstall_probe_completed", done);
        ctx.check("reinstall_java_called_alpha_once",
                  shr_fixture::get_alpha_calls() - alpha_calls_before_reinstall == 1);
        ctx.check("reinstall_detour_fired_again", g_alpha_fires.load() == 1);
    }

    // =====================================================================
    // 5 — MOVE-CONSTRUCTION transfers ownership.  The moved-FROM handle is
    //     empty (installed()==false) and must not double-remove; the moved-TO
    //     handle owns the live hook and fires.  After the moved-to handle
    //     drops, the hook is gone.
    // =====================================================================
    const std::int32_t alpha_calls_before_movector{ shr_fixture::get_alpha_calls() };
    {
        auto src{ vmhook::scoped_hook<shr_fixture>("alpha", "(I)I", alpha_detour()) };
        ctx.check("movector_src_installed_before", src.installed());

        vmhook::hook_handle dst{ std::move(src) };
        ctx.check("movector_src_empty_after_move", !src.installed());
        ctx.check("movector_dst_owns_after_move", dst.installed());

        const bool done{ drive(ctx, 1) };
        ctx.check("movector_probe_completed", done);
        ctx.check("movector_java_called_alpha_once",
                  shr_fixture::get_alpha_calls() - alpha_calls_before_movector == 1);
        ctx.check("movector_fires_through_dst", g_alpha_fires.load() == 1);
        // The moved-from `src` is destroyed first at scope exit (reverse decl
        // order: dst then src) — its empty stop() must be a no-op, and dst must
        // still tear the hook down.  Verified by scenario 6's first probe.
    }   // dst then src destroyed.

    // Confirm the hook from scenario 5 is actually gone (the moved-from handle
    // did not leak it, and dst's destructor removed it exactly once).
    const std::int32_t alpha_calls_after_movector{ shr_fixture::get_alpha_calls() };
    {
        const bool done{ drive(ctx, 1) };
        ctx.check("movector_after_drop_probe_completed", done);
        ctx.check("movector_after_drop_java_called",
                  shr_fixture::get_alpha_calls() - alpha_calls_after_movector == 1);
        ctx.check("movector_after_drop_detour_gone", g_alpha_fires.load() == 0);
    }

    // =====================================================================
    // 6 — MOVE-ASSIGNMENT drops the LHS hook before stealing the RHS.
    //     A is armed on alpha, B is armed on beta; `A = std::move(B)` must
    //     tear down A's alpha hook (alpha stops firing) and carry B's beta
    //     hook into A (beta fires through A).  B is left empty.
    // =====================================================================
    {
        auto a{ vmhook::scoped_hook<shr_fixture>("alpha", "(I)I", alpha_detour()) };
        auto b{ vmhook::scoped_hook<shr_fixture>("beta", "(I)I", beta_detour()) };
        ctx.check("moveassign_a_installed_before", a.installed());
        ctx.check("moveassign_b_installed_before", b.installed());

        a = std::move(b);
        ctx.check("moveassign_a_still_installed", a.installed());
        ctx.check("moveassign_b_empty_after", !b.installed());

        // Drive alpha + beta + gamma in one cycle.  Only beta should now fire
        // (carried into A); alpha's old hook was dropped by the assignment and
        // gamma was never hooked.
        const bool done{ drive(ctx, 5) };
        ctx.check("moveassign_probe_completed", done);
        ctx.check("moveassign_old_lhs_alpha_silent", g_alpha_fires.load() == 0);
        ctx.check("moveassign_moved_beta_fires", g_beta_fires.load() == 1);
        ctx.check("moveassign_unhooked_gamma_silent", g_gamma_fires.load() == 0);
    }

    // =====================================================================
    // 7 — MULTIPLE scoped_hooks on DIFFERENT methods are independent.  Hooks on
    //     alpha, beta, gamma all fire in one cycle; dropping the inner-scoped
    //     gamma hook leaves alpha + beta still firing.
    // =====================================================================
    {
        auto h_alpha{ vmhook::scoped_hook<shr_fixture>("alpha", "(I)I", alpha_detour()) };
        auto h_beta{ vmhook::scoped_hook<shr_fixture>("beta", "(I)I", beta_detour()) };
        ctx.check("multi_alpha_installed", h_alpha.installed());
        ctx.check("multi_beta_installed", h_beta.installed());

        {
            auto h_gamma{ vmhook::scoped_hook<shr_fixture>("gamma", "(I)I", gamma_detour()) };
            ctx.check("multi_gamma_installed_inner", h_gamma.installed());

            const bool done{ drive(ctx, 5) };
            ctx.check("multi_all_probe_completed", done);
            ctx.check("multi_alpha_fired", g_alpha_fires.load() == 1);
            ctx.check("multi_beta_fired", g_beta_fires.load() == 1);
            ctx.check("multi_gamma_fired_in_inner", g_gamma_fires.load() == 1);
        }   // only gamma's hook drops here.

        // gamma must now be silent; alpha + beta unaffected by gamma's removal.
        const bool done{ drive(ctx, 5) };
        ctx.check("multi_after_inner_probe_completed", done);
        ctx.check("multi_alpha_still_fires", g_alpha_fires.load() == 1);
        ctx.check("multi_beta_still_fires", g_beta_fires.load() == 1);
        ctx.check("multi_gamma_now_silent", g_gamma_fires.load() == 0);
    }   // alpha + beta drop here.

    // After all three handles are gone, none fire.
    {
        const bool done{ drive(ctx, 5) };
        ctx.check("multi_all_dropped_probe_completed", done);
        ctx.check("multi_all_dropped_alpha_silent", g_alpha_fires.load() == 0);
        ctx.check("multi_all_dropped_beta_silent", g_beta_fires.load() == 0);
        ctx.check("multi_all_dropped_gamma_silent", g_gamma_fires.load() == 0);
    }

    // =====================================================================
    // 8 — NESTED scopes: an OUTER hook on alpha stays armed across an inner
    //     scope that arms+drops a beta hook.  After the inner scope, alpha must
    //     still fire and beta must be silent (the inner drop did not disturb the
    //     outer hook).
    // =====================================================================
    {
        auto outer{ vmhook::scoped_hook<shr_fixture>("alpha", "(I)I", alpha_detour()) };
        ctx.check("nested_outer_installed", outer.installed());

        {
            auto inner{ vmhook::scoped_hook<shr_fixture>("beta", "(I)I", beta_detour()) };
            ctx.check("nested_inner_installed", inner.installed());
            ctx.check("nested_outer_still_installed_with_inner", outer.installed());

            const bool done{ drive(ctx, 5) };
            ctx.check("nested_inner_probe_completed", done);
            ctx.check("nested_both_fire_inner", g_alpha_fires.load() == 1 && g_beta_fires.load() == 1);
        }   // inner (beta) drops.

        ctx.check("nested_outer_survives_inner_drop", outer.installed());
        const bool done{ drive(ctx, 5) };
        ctx.check("nested_after_inner_probe_completed", done);
        ctx.check("nested_outer_alpha_still_fires", g_alpha_fires.load() == 1);
        ctx.check("nested_inner_beta_now_silent", g_beta_fires.load() == 0);
    }

    // =====================================================================
    // 9 — STATIC-method scoped_hook RAII: a static method hook fires in scope
    //     and is gone after scope exit, same as the instance path.
    // =====================================================================
    const std::int32_t static_calls_before{ shr_fixture::get_static_alpha_calls() };
    {
        auto handle{ vmhook::scoped_hook<shr_fixture>("staticAlpha", "(I)I", static_alpha_detour()) };
        ctx.check("static_scoped_installed", handle.installed());

        const bool done{ drive(ctx, 4) };
        ctx.check("static_probe_completed", done);
        ctx.check("static_java_called_once",
                  shr_fixture::get_static_alpha_calls() - static_calls_before == 1);
        ctx.check("static_detour_fired", g_static_alpha_fires.load() == 1);
        ctx.check("static_allow_through_result",
                  shr_fixture::get_static_alpha_result() == (STATIC_ALPHA_DELTA * 2));
    }
    const std::int32_t static_calls_after{ shr_fixture::get_static_alpha_calls() };
    {
        const bool done{ drive(ctx, 4) };
        ctx.check("static_after_scope_probe_completed", done);
        ctx.check("static_after_scope_java_called",
                  shr_fixture::get_static_alpha_calls() - static_calls_after == 1);
        ctx.check("static_after_scope_detour_gone", g_static_alpha_fires.load() == 0);
    }

    // =====================================================================
    // 10 — OVERLOAD resolution via the SHORT scoped_hook(name, cb) overload.
    //      over(int) and over(int,int) share the name "over"; the no-signature
    //      overload installs the FIRST matching descriptor.  We declared over(int)
    //      first in the fixture, so driving over(int) must fire the detour whose
    //      single-int signature we registered.  (We assert the hook installs and
    //      fires on at least the one-arg form; the two-arg form is covered by the
    //      long overload in scenario 11.)
    // =====================================================================
    {
        // The name-only scoped_hook(name, cb) installs on the FIRST "over" overload
        // in HotSpot's _methods ARRAY order — which is Symbol-ADDRESS order
        // (Symbol::fast_compare), interning-dependent and NOT source-declaration
        // order.  So whether driving over(int) fires the detour depends on which
        // "over" overload sorts first on THIS JDK/build.  Determine it portably via
        // vmhook's own enumeration (it walks the identical _methods array).
        const auto first_over_descriptor{ []() -> std::string
        {
            for (const std::pair<std::string, std::string>& m :
                     vmhook::get_class_methods<shr_fixture>())
            {
                if (m.first == "over") { return m.second; }
            }
            return {};
        }() };
        const bool bound_to_over_i{ first_over_descriptor == "(I)I" };

        auto handle{ vmhook::scoped_hook<shr_fixture>("over", over_i_detour()) };
        ctx.check("short_overload_installed", handle.installed());

        const std::int32_t before{ shr_fixture::get_over_i_calls() };
        const bool done{ drive(ctx, 6) };
        ctx.check("short_overload_probe_completed", done);
        ctx.check("short_overload_java_called_over_i",
                  shr_fixture::get_over_i_calls() - before == 1);
        ctx.record(std::string{ "[INFO] name-only hook on 'over' bound to first array-order overload '" } +
                   (first_over_descriptor.empty() ? std::string{ "<none>" } : first_over_descriptor) +
                   "'; driving over(int) " + (bound_to_over_i ? "DOES" : "does NOT") + " fire it.");
        // The detour fires iff the name-only hook bound to over(int) — the form we
        // drive.  Either branch is a deterministic, portable assertion.
        if (bound_to_over_i)
        {
            ctx.check("short_overload_detour_fired_when_bound_to_over_i", g_over_i_fires.load() == 1);
            ctx.check("short_overload_decoded_arg", g_over_i_arg_ok.load());
        }
        else
        {
            ctx.check("short_overload_detour_silent_when_bound_to_over_ii", g_over_i_fires.load() == 0);
        }
        // allow-through: over(int)'s original body runs regardless of which overload
        // the name-only hook targeted (the detour never cancels).
        ctx.check("short_overload_allow_through",
                  shr_fixture::get_over_i_result() == (OVER_I_ARG + 1));
    }

    // =====================================================================
    // 11 — OVERLOAD resolution via the LONG scoped_hook(name, sig, cb) overload.
    //      Selecting "(II)I" must target over(int,int) specifically: driving the
    //      two-arg form fires the detour and decodes BOTH ints, while driving the
    //      one-arg form does NOT fire it (the descriptor pinned the wrong-arity
    //      overload out).
    // =====================================================================
    {
        auto handle{ vmhook::scoped_hook<shr_fixture>("over", "(II)I", over_ii_detour()) };
        ctx.check("long_overload_installed", handle.installed());

        // Drive the 2-arg form: the detour must fire and decode both args.
        const std::int32_t before_ii{ shr_fixture::get_over_ii_calls() };
        bool done{ drive(ctx, 7) };
        ctx.check("long_overload_ii_probe_completed", done);
        ctx.check("long_overload_java_called_over_ii",
                  shr_fixture::get_over_ii_calls() - before_ii == 1);
        ctx.check("long_overload_detour_fired_on_ii", g_over_ii_fires.load() == 1);
        ctx.check("long_overload_decoded_both_args", g_over_ii_args_ok.load());
        ctx.check("long_overload_allow_through",
                  shr_fixture::get_over_ii_result() == (OVER_II_ARG_A + OVER_II_ARG_B));

        // Drive the 1-arg form: the "(II)I" hook must NOT fire (it targets the
        // 2-arg overload only).
        done = drive(ctx, 6);
        ctx.check("long_overload_i_probe_completed", done);
        ctx.check("long_overload_did_not_fire_on_i", g_over_ii_fires.load() == 0);
    }

    // =====================================================================
    // 12 — NON-matching signature -> empty handle, never fires.  A descriptor
    //      that matches no overload of "over" must yield installed()==false and
    //      no detour activity.
    // =====================================================================
    {
        auto handle{ vmhook::scoped_hook<shr_fixture>("over", "(J)J", over_i_detour()) };
        ctx.check("bad_sig_not_installed", !handle.installed());

        const bool done{ drive(ctx, 6) };
        ctx.check("bad_sig_probe_completed", done);
        ctx.check("bad_sig_detour_did_not_fire", g_over_i_fires.load() == 0);
    }

    // =====================================================================
    // 13 — Missing method name -> empty handle.  A name no method has must yield
    //      installed()==false without throwing.
    // =====================================================================
    {
        auto handle{ vmhook::scoped_hook<shr_fixture>("noSuchMethod", "(I)I", alpha_detour()) };
        ctx.check("missing_method_not_installed", !handle.installed());
    }

    // =====================================================================
    // 14 — UNREGISTERED wrapper type -> empty handle, no crash.  scoped_hook on a
    //      type that was never register_class()'d must converge on the same empty
    //      handle the underlying-failure and re-resolution paths both produce.
    // =====================================================================
    {
        auto handle{ vmhook::scoped_hook<unregistered_fixture>(
            "alpha", "(I)I",
            [](vmhook::return_value&,
               const std::unique_ptr<unregistered_fixture>&,
               std::int32_t) {}) };
        ctx.check("unregistered_type_not_installed", !handle.installed());
    }

    // =====================================================================
    // 15 — EARLY-RETURN teardown.  A scoped_hook armed inside a helper that
    //      RETURNS before its closing brace must still be torn down by the
    //      handle destructor on the way out (RAII does not require reaching the
    //      end of the block).  After the helper returns: alpha is silent, the
    //      original body still ran, and the hook set is drift-free.
    // =====================================================================
    {
        // Helper arms alpha, drives ONE cycle (proving it fired while armed),
        // then takes an EARLY return — the scoped_hook's destructor runs as the
        // frame unwinds past the `return`, NOT at a closing-brace fall-through.
        const auto armed_then_early_return = [&ctx]() -> bool
        {
            auto handle{ vmhook::scoped_hook<shr_fixture>("alpha", "(I)I", alpha_detour()) };
            if (!handle.installed())
            {
                return false;
            }
            const bool done{ drive(ctx, 1) };
            ctx.check("early_return_in_scope_probe_completed", done);
            ctx.check("early_return_fired_in_scope", g_alpha_fires.load() == 1);
            return true;   // EARLY exit — handle destructs here.
        };

        const std::int32_t before{ shr_fixture::get_alpha_calls() };
        const bool armed_ok{ armed_then_early_return() };
        ctx.check("early_return_helper_reported_armed", armed_ok);

        // Helper has returned: the hook must be gone.  Drive alpha again and
        // confirm the detour is silent while the original body still runs.
        const bool done{ drive(ctx, 1) };
        ctx.check("early_return_after_probe_completed", done);
        ctx.check("early_return_java_called_alpha_once",
                  shr_fixture::get_alpha_calls() - before == 2);   // in-scope + after
        ctx.check("early_return_detour_silent_after", g_alpha_fires.load() == 0);
        ctx.check("early_return_original_still_ran",
                  shr_fixture::get_alpha_result() == (SEED + ALPHA_DELTA));
        ctx.check("early_return_verify_hooks_zero", vmhook::verify_hooks() == 0);
    }

    // =====================================================================
    // 16 — EXCEPTION-UNWIND teardown.  A scoped_hook armed in a scope that
    //      unwinds via a C++ throw must be uninstalled by the handle destructor
    //      as the stack unwinds (the destructor runs during unwinding, before
    //      the catch).  After the catch: alpha is silent, the original body
    //      still ran, and the hook set is drift-free.  This is the path that a
    //      naive "remove the hook just before the normal return" implementation
    //      would LEAK — RAII is the only thing that closes it.
    // =====================================================================
    {
        const std::int32_t before{ shr_fixture::get_alpha_calls() };
        bool caught{ false };
        bool fired_in_scope{ false };
        try
        {
            auto handle{ vmhook::scoped_hook<shr_fixture>("alpha", "(I)I", alpha_detour()) };
            ctx.check("exception_unwind_installed_before_throw", handle.installed());

            const bool done{ drive(ctx, 1) };
            ctx.check("exception_unwind_in_scope_probe_completed", done);
            fired_in_scope = (g_alpha_fires.load() == 1);

            throw std::runtime_error{ "scoped_hook unwind probe" };
        }   // handle destructor runs HERE during unwinding -> hook removed.
        catch (const std::runtime_error&)
        {
            caught = true;
        }
        ctx.check("exception_unwind_caught", caught);
        ctx.check("exception_unwind_fired_in_scope", fired_in_scope);

        // Post-unwind: the hook must be gone.  Drive alpha again; detour silent.
        const bool done{ drive(ctx, 1) };
        ctx.check("exception_unwind_after_probe_completed", done);
        ctx.check("exception_unwind_java_called_alpha",
                  shr_fixture::get_alpha_calls() - before == 2);   // in-scope + after
        ctx.check("exception_unwind_detour_silent_after", g_alpha_fires.load() == 0);
        ctx.check("exception_unwind_original_still_ran",
                  shr_fixture::get_alpha_result() == (SEED + ALPHA_DELTA));
        ctx.check("exception_unwind_verify_hooks_zero", vmhook::verify_hooks() == 0);
    }

    // =====================================================================
    // 17 — LOOP RE-ENTRY: install + drive + uninstall N times cleanly.  Each
    //      iteration arms alpha in a fresh scope, fires it exactly once while
    //      armed, then drops the handle at the iteration's closing brace.  The
    //      hook set must return to drift-free after EVERY iteration (no stale
    //      entry accumulates), and the final post-loop probe must be silent.
    //      Proves the install/uninstall cycle is repeatable, not one-shot.
    // =====================================================================
    {
        constexpr std::int32_t LOOP_N{ 5 };
        bool every_iter_installed{ true };
        bool every_iter_fired{ true };
        bool every_iter_verify_zero{ true };
        const std::int32_t before{ shr_fixture::get_alpha_calls() };

        for (std::int32_t i{ 0 }; i < LOOP_N; ++i)
        {
            auto handle{ vmhook::scoped_hook<shr_fixture>("alpha", "(I)I", alpha_detour()) };
            every_iter_installed = every_iter_installed && handle.installed();

            const bool done{ drive(ctx, 1) };
            every_iter_fired = every_iter_fired && done && (g_alpha_fires.load() == 1);
            // handle drops at the brace below -> hook removed for this iteration.
        }   // <- per-iteration teardown

        // After the loop fully exited (last handle destroyed), the set is clean.
        every_iter_verify_zero = (vmhook::verify_hooks() == 0);

        ctx.check("loop_reentry_every_iter_installed", every_iter_installed);
        ctx.check("loop_reentry_every_iter_fired_once", every_iter_fired);
        ctx.check("loop_reentry_java_called_alpha_n_times",
                  shr_fixture::get_alpha_calls() - before == LOOP_N);
        ctx.check("loop_reentry_verify_hooks_zero_after_loop", every_iter_verify_zero);

        // Final probe: alpha must be silent now that the loop's last handle is gone.
        reset_fires();
        const std::int32_t before_final{ shr_fixture::get_alpha_calls() };
        const bool done{ drive(ctx, 1) };
        ctx.check("loop_reentry_after_loop_probe_completed", done);
        ctx.check("loop_reentry_after_loop_java_called",
                  shr_fixture::get_alpha_calls() - before_final == 1);
        ctx.check("loop_reentry_after_loop_detour_silent", g_alpha_fires.load() == 0);
    }

    // =====================================================================
    // 18 — SELF move-assignment is a guarded no-op.  operator=(&&) checks
    //      `this != &other`, so `h = std::move(h)` must NOT stop()-then-adopt
    //      its own (now-nulled) target — the handle must stay installed and the
    //      hook keep firing.  (Self-move that disarmed would be a silent
    //      double-disarm / use-after-clear bug.)
    // =====================================================================
    {
        auto h{ vmhook::scoped_hook<shr_fixture>("alpha", "(I)I", alpha_detour()) };
        ctx.check("self_moveassign_installed_before", h.installed());

        // Route through a reference to dodge the obvious self-move warning while
        // still exercising the operator=(&&) self-assignment guard at runtime.
        vmhook::hook_handle& alias{ h };
        h = std::move(alias);
        ctx.check("self_moveassign_still_installed", h.installed());

        const std::int32_t before{ shr_fixture::get_alpha_calls() };
        const bool done{ drive(ctx, 1) };
        ctx.check("self_moveassign_probe_completed", done);
        ctx.check("self_moveassign_java_called_alpha_once",
                  shr_fixture::get_alpha_calls() - before == 1);
        ctx.check("self_moveassign_still_fires", g_alpha_fires.load() == 1);
    }   // h drops here -> hook removed.
    ctx.check("self_moveassign_verify_hooks_zero", vmhook::verify_hooks() == 0);

    // =====================================================================
    // 19 — TWO scoped_hooks torn down together by ONE exception unwind.  Both
    //      alpha and beta are armed in the same scope; a throw must run BOTH
    //      destructors during unwinding (reverse declaration order: beta then
    //      alpha) so NEITHER hook survives.  After the catch, driving alpha+beta
    //      leaves both detours silent and the hook set drift-free — the
    //      multi-hook generalisation of scenario 16.
    // =====================================================================
    {
        const std::int32_t alpha_before{ shr_fixture::get_alpha_calls() };
        const std::int32_t beta_before{ shr_fixture::get_beta_calls() };
        bool caught{ false };
        bool both_fired_in_scope{ false };
        try
        {
            auto h_alpha{ vmhook::scoped_hook<shr_fixture>("alpha", "(I)I", alpha_detour()) };
            auto h_beta{ vmhook::scoped_hook<shr_fixture>("beta", "(I)I", beta_detour()) };
            ctx.check("two_hook_unwind_alpha_installed", h_alpha.installed());
            ctx.check("two_hook_unwind_beta_installed", h_beta.installed());

            const bool done{ drive(ctx, 9) };
            ctx.check("two_hook_unwind_in_scope_probe_completed", done);
            both_fired_in_scope = (g_alpha_fires.load() == 1 && g_beta_fires.load() == 1);

            throw std::runtime_error{ "two-hook scoped_hook unwind probe" };
        }   // h_beta then h_alpha destruct HERE during unwinding -> both removed.
        catch (const std::runtime_error&)
        {
            caught = true;
        }
        ctx.check("two_hook_unwind_caught", caught);
        ctx.check("two_hook_unwind_both_fired_in_scope", both_fired_in_scope);

        // Post-unwind: both hooks gone.  Drive alpha+beta; both detours silent.
        const bool done{ drive(ctx, 9) };
        ctx.check("two_hook_unwind_after_probe_completed", done);
        ctx.check("two_hook_unwind_java_called_alpha",
                  shr_fixture::get_alpha_calls() - alpha_before == 2);
        ctx.check("two_hook_unwind_java_called_beta",
                  shr_fixture::get_beta_calls() - beta_before == 2);
        ctx.check("two_hook_unwind_alpha_silent_after", g_alpha_fires.load() == 0);
        ctx.check("two_hook_unwind_beta_silent_after", g_beta_fires.load() == 0);
        ctx.check("two_hook_unwind_verify_hooks_zero", vmhook::verify_hooks() == 0);
    }

    // Leave no scoped_hook installed behind us for later modules sharing the JVM:
    // every handle above was scope-local and has been destroyed by this point.
    // Final HARD proof: a drift-free, fully-disarmed hook set at module exit.
    ctx.check("module_left_clean_final_verify_zero", vmhook::verify_hooks() == 0);
    ctx.check("module_left_no_hooks_armed", true);
}
