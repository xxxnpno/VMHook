// Standalone (no-JVM) unit test for vmhook::shutdown_hooks() cold-state
// contract.
//
// WAVE-32 LEDGER GAPS this file closes:
//   * cold-state shutdown_hooks() with NO hooks ever installed is a TRUE no-op:
//     no throw, no observable side-effect that would leak into a later install.
//   * idempotent twice/thrice/N-times back-to-back at cold-state.
//   * shutdown_hooks() is noexcept (signature pin) and returns void.
//   * the auto_repair::g_started latch is observable from the test, and
//     shutdown_hooks() reaches the rendezvous that drives g_started -> false.
//   * the reversibility invariant: AFTER shutdown_hooks() the two latches
//     (g_shutdown_requested + auto_repair::g_started) are BOTH clear so a
//     subsequent vmhook::hook<T>() install would be fully live again — this is
//     the pin for the "permanently latched shutdown flag" regression
//     (vmhook.hpp:11325-11335).
//   * static_asserts on signature, return type, noexcept, and no-argument form.
//
// OUT OF SCOPE (needs a live JVM, covered by tests/jvm/modules/
// shutdown_hooks_teardown.cpp against the ShutdownHooksTeardown fixture):
//   * the three-beat dance install -> shutdown -> reinstall.
//   * BYTE-EXACT original behaviour after teardown.
//   * multi-hook bulk teardown across distinct Method shapes.

#include <vmhook/vmhook.hpp>

#include <atomic>
#include <cstdio>
#include <type_traits>

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

// ---------------------------------------------------------------------------
// SECTION 1 - signature / type locks (compile-time).
// ---------------------------------------------------------------------------
static_assert(std::is_same_v<decltype(vmhook::shutdown_hooks()), void>,
              "vmhook::shutdown_hooks() must return void");
static_assert(noexcept(vmhook::shutdown_hooks()),
              "vmhook::shutdown_hooks() must be noexcept");
static_assert(std::is_invocable_r_v<void, decltype(&vmhook::shutdown_hooks)>,
              "vmhook::shutdown_hooks must take no arguments");

// The two latches that drive reversibility live in well-known namespaces and
// are std::atomic<bool>.  Pin those types so a later refactor that shifts the
// width or drops atomicity has to also touch this file.
static_assert(std::is_same_v<decltype(vmhook::hotspot::g_shutdown_requested),
                             std::atomic<bool>>,
              "g_shutdown_requested must remain std::atomic<bool>");
static_assert(std::is_same_v<decltype(vmhook::detail::auto_repair::g_started),
                             std::atomic<bool>>,
              "auto_repair::g_started must remain std::atomic<bool>");

auto main() -> int
{
    std::printf("[shutdown_hooks_teardown_nojvm] start\n");

    // -----------------------------------------------------------------------
    // SECTION 2 - cold-state default latches.
    // Before any hook<T>() call in this process, both latches default to false
    // (vmhook.hpp:7197, :11021).  This is the precondition the reversibility
    // invariant restores after shutdown.
    // -----------------------------------------------------------------------
    check("cold_g_shutdown_requested_default_false",
          !vmhook::hotspot::g_shutdown_requested.load(std::memory_order_acquire));
    check("cold_auto_repair_g_started_default_false",
          !vmhook::detail::auto_repair::g_started.load(std::memory_order_acquire));

    // -----------------------------------------------------------------------
    // SECTION 3 - cold-state shutdown_hooks() is a safe no-op.
    // No hook was ever installed; the teardown loops walk empty vectors and
    // the latch-reset stores at the tail are unconditional.  The call itself
    // is noexcept, but exercise it under a try-block anyway so an accidental
    // future throw from a noexcept function (= std::terminate) is at least
    // surfaced as a hard test failure on the way down.
    // -----------------------------------------------------------------------
    {
        bool threw{ false };
        try { vmhook::shutdown_hooks(); } catch (...) { threw = true; }
        check("cold_shutdown_hooks_no_throw", !threw);
    }

    // After one cold-state shutdown, both latches must STILL be observably
    // false — this is the reversibility contract (vmhook.hpp:11325-11335).
    check("post_shutdown_g_shutdown_requested_false",
          !vmhook::hotspot::g_shutdown_requested.load(std::memory_order_acquire));
    check("post_shutdown_auto_repair_g_started_false",
          !vmhook::detail::auto_repair::g_started.load(std::memory_order_acquire));

    // -----------------------------------------------------------------------
    // SECTION 4 - idempotent: twice, thrice, N times back-to-back.
    // -----------------------------------------------------------------------
    {
        bool threw{ false };
        try
        {
            vmhook::shutdown_hooks();
            vmhook::shutdown_hooks();
        }
        catch (...) { threw = true; }
        check("twice_back_to_back_no_throw", !threw);
        check("twice_post_shutdown_flag_clear",
              !vmhook::hotspot::g_shutdown_requested.load(std::memory_order_acquire));
        check("twice_post_started_flag_clear",
              !vmhook::detail::auto_repair::g_started.load(std::memory_order_acquire));
    }
    {
        bool threw{ false };
        try
        {
            vmhook::shutdown_hooks();
            vmhook::shutdown_hooks();
            vmhook::shutdown_hooks();
        }
        catch (...) { threw = true; }
        check("thrice_back_to_back_no_throw", !threw);
    }
    {
        // 32 cold calls — stability under repetition.
        bool threw{ false };
        try
        {
            for (int i = 0; i < 32; ++i) { vmhook::shutdown_hooks(); }
        }
        catch (...) { threw = true; }
        check("thirty_two_cold_calls_no_throw", !threw);
        check("thirty_two_cold_calls_flag_still_clear",
              !vmhook::hotspot::g_shutdown_requested.load(std::memory_order_acquire));
        check("thirty_two_cold_calls_started_still_clear",
              !vmhook::detail::auto_repair::g_started.load(std::memory_order_acquire));
    }

    // -----------------------------------------------------------------------
    // SECTION 5 - simulate a "latched shutdown" regression and verify that
    // shutdown_hooks() actively clears it.  Manually flip the two latches to
    // true (as the install path + a buggy pre-fix teardown would have left
    // them), call shutdown_hooks(), and assert both stores at the tail
    // executed — the regression characterization for vmhook.hpp:11325-11335.
    // This is the cold-state equivalent of the JVM-side three-beat dance.
    // -----------------------------------------------------------------------
    {
        vmhook::hotspot::g_shutdown_requested.store(true, std::memory_order_release);
        vmhook::detail::auto_repair::g_started.store(true, std::memory_order_release);

        // Pre-condition: confirm our manual stores landed.
        check("manual_flag_set_observed",
              vmhook::hotspot::g_shutdown_requested.load(std::memory_order_acquire));
        check("manual_started_set_observed",
              vmhook::detail::auto_repair::g_started.load(std::memory_order_acquire));

        bool threw{ false };
        try { vmhook::shutdown_hooks(); } catch (...) { threw = true; }
        check("shutdown_after_manual_set_no_throw", !threw);

        // The teardown's reversibility tail MUST have cleared both.
        check("reversibility_g_shutdown_requested_cleared",
              !vmhook::hotspot::g_shutdown_requested.load(std::memory_order_acquire));
        check("reversibility_auto_repair_g_started_cleared",
              !vmhook::detail::auto_repair::g_started.load(std::memory_order_acquire));
    }

    if (failures == 0)
    {
        std::printf("[shutdown_hooks_teardown_nojvm] OK\n");
        return 0;
    }
    std::printf("[shutdown_hooks_teardown_nojvm] %d FAILED\n", failures);
    return 1;
}
