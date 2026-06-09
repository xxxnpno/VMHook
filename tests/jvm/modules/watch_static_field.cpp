// watch_static_field JVM test module  (feature area: watchers / hardware DRs)
//
// THE field-write-watchpoint authority: exhaustively exercises
// vmhook::watch_static_field<wrapper, field_type>(name, callback) on a LIVE
// JVM.  watch_static_field arms a hardware data breakpoint (one of the four
// CPU debug-register slots DR0-DR3) on a Java static field's backing storage;
// the trap fires SYNCHRONOUSLY on whichever thread writes the field, *during*
// the store, and the callback runs inside a vectored exception handler on that
// same thread.  vmhook arms the trap on every thread that exists at install
// time -- including the Harness loop thread -- so a putstatic the fixture's
// run() executes traps immediately and the callback has already fired by the
// time the go/done probe returns (mirrors the synchronous-trap reasoning in
// the legacy example.cpp test_field_watcher this module migrates).
//
// What this module proves on a live JVM (Windows x86_64; the only platform
// where VMHOOK_HAS_HW_DATA_BREAKPOINTS is 1 -- elsewhere it asserts the
// documented empty-handle fallback instead of silently polling):
//   * a watch on a static int observes a Java-driven write: the callback fires
//     once per putstatic (N writes -> N fires) and its `new` argument carries
//     the field's NEW value, ending at the precise final value;
//   * the four hardware slots can be filled simultaneously (four independent
//     watches all running()), a FIFTH watch is cleanly REFUSED with an empty
//     handle (running()==false) -- characterising the DR0-DR3 capacity limit
//     instead of crashing or silently no-op'ing;
//   * the four armed watches are INDEPENDENT: driving one field fires ONLY
//     that field's callback, never a sibling's (correct slot<->address
//     binding);
//   * dropping a watch FREES its slot -- a subsequent install that would have
//     been the fifth now succeeds;
//   * after a watch is stopped, further writes to its field DO NOT fire it
//     (the trap is disarmed on every thread).
//
// SAFETY (hardware debug registers are delicate -- a stray armed DR or a
// callback that re-enters the JVM can wedge or crash the whole process):
//   * EVERY watch_handle is RAII-scoped or explicitly stop()'d before the
//     module returns, so NO watchpoint is left armed to fire spuriously inside
//     a later module sharing this JVM;
//   * the callbacks only touch std::atomic<> counters -- no allocation, no
//     JVM re-entry, exactly as the contract requires of a VEH-context
//     callback;
//   * every assertion that needs a real trap is gated on
//     VMHOOK_HAS_HW_DATA_BREAKPOINTS; the unsupported-platform branch asserts
//     the empty-handle contract and drives the Java writes anyway to prove the
//     fixture itself works.
//
// Harness shape mirrors field_static / hook_basic: register_class, a `mode`
// selector with `done` reset on the rising edge of go, a dense ctx.check()
// battery.  MSVC-safe value extraction (copy-init, never brace-init).
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace
{
    // Wrapper for vmhook.fixtures.WatchStaticField.  Deriving from
    // vmhook::object<> gives the wrapper a vtable (required by register_class)
    // and the static_field(...) accessors used for the handshake + readback.
    class wsf : public vmhook::object<wsf>
    {
    public:
        explicit wsf(vmhook::oop_t instance) noexcept
            : vmhook::object<wsf>{ instance }
        {
        }

        // ---- go / done / mode handshake (all via static_field) ----
        static auto set_go(bool value) -> void       { static_field("go")->set(value); }
        static auto set_done(bool value) -> void      { static_field("done")->set(value); }
        static auto get_done() -> bool                { return static_field("done")->get(); }
        static auto set_mode(std::int32_t m) -> void  { static_field("mode")->set(m); }

        static auto resolves(const char* name) -> bool { return static_field(name).has_value(); }

        // ---- read a watched counter's current value (Java's own view) ----
        static auto counter(const char* name) -> std::int32_t { return static_field(name)->get(); }

        static auto get_writes_made() -> std::int32_t
        {
            const std::int32_t v = static_field("writesMade")->get();
            return v;
        }
    };

    // ---- Fixture-mirrored constants (kept in lockstep with WatchStaticField.java) --
    constexpr std::int32_t WRITE_COUNT{ 12 };
    constexpr std::int32_t FINAL_VALUE{ 12 };

    // ---- Per-watch callback observation state -----------------------------
    // One independent (fire-count, last-value) pair per watched field so the
    // module can attribute a fired callback to a specific watch and assert the
    // observed NEW value.  Reset between scenarios via reset_observations().
    struct watch_obs
    {
        std::atomic<std::int32_t> fires{ 0 };
        std::atomic<std::int32_t> last_new{ -1 };
        std::atomic<std::int32_t> max_new{ -1 };
        std::atomic<bool>         prev_was_zero{ true }; // every `old` arg is the zero placeholder
        std::atomic<bool>         monotonic{ true };     // `new` never decreases within a scenario
    };

    watch_obs g_obs_a{};
    watch_obs g_obs_b{};
    watch_obs g_obs_c{};
    watch_obs g_obs_d{};
    watch_obs g_obs_e{};

    auto reset_one(watch_obs& o) -> void
    {
        o.fires.store(0, std::memory_order_relaxed);
        o.last_new.store(-1, std::memory_order_relaxed);
        o.max_new.store(-1, std::memory_order_relaxed);
        o.prev_was_zero.store(true, std::memory_order_relaxed);
        o.monotonic.store(true, std::memory_order_relaxed);
    }

    auto reset_observations() -> void
    {
        reset_one(g_obs_a);
        reset_one(g_obs_b);
        reset_one(g_obs_c);
        reset_one(g_obs_d);
        reset_one(g_obs_e);
    }

    // Records one trap callback into `o`.  Only touches atomics -- safe to run
    // in the VEH context on the writing Java thread (no allocation, no JVM
    // re-entry).  `prev` is the zero-initialised placeholder the trap path
    // always passes (the CPU cannot recover the pre-write value); `next` is the
    // value read at trap time, which must be the new, post-increment value.
    auto record_trap(watch_obs& o, std::int32_t prev, std::int32_t next) -> void
    {
        if (prev != 0)
        {
            o.prev_was_zero.store(false, std::memory_order_relaxed);
        }
        const std::int32_t before{ o.last_new.exchange(next, std::memory_order_relaxed) };
        if (before >= 0 && next < before)
        {
            o.monotonic.store(false, std::memory_order_relaxed);
        }
        std::int32_t observed_max{ o.max_new.load(std::memory_order_relaxed) };
        while (next > observed_max
               && !o.max_new.compare_exchange_weak(observed_max, next, std::memory_order_relaxed))
        {
            // retry until our max wins or a larger one is already stored
        }
        o.fires.fetch_add(1, std::memory_order_relaxed);
    }

    // Drives exactly one probe cycle for `mode`: resets observations + the
    // latched `done`, programs the selector on the rising edge of go, then
    // waits for done.  (Observations are reset here so each scenario's fire
    // accounting starts clean -- the watches themselves are installed by the
    // caller and remain armed across this cycle.)
    auto drive(vmhook_test::context& ctx, std::int32_t mode) -> bool
    {
        reset_observations();
        return ctx.run_probe(
            [mode](bool value)
            {
                if (value)
                {
                    wsf::set_done(false);
                    wsf::set_mode(mode);
                }
                wsf::set_go(value);
            },
            []() { return wsf::get_done(); });
    }

    // Drives a probe WITHOUT clearing observations (used to reset the Java-side
    // counters via mode 0 while keeping the native fire accounting from the
    // previous cycle intact -- the "writes after removal don't fire" proof).
    auto drive_keep_obs(vmhook_test::context& ctx, std::int32_t mode) -> bool
    {
        return ctx.run_probe(
            [mode](bool value)
            {
                if (value)
                {
                    wsf::set_done(false);
                    wsf::set_mode(mode);
                }
                wsf::set_go(value);
            },
            []() { return wsf::get_done(); });
    }

    // Installs a watch on the named int field whose callback records into `o`.
    // field_type is std::int32_t (4-byte field -> DR LEN = four_bytes).
    auto install_watch(const char* field, watch_obs& o) -> vmhook::watch_handle
    {
        return vmhook::watch_static_field<wsf, std::int32_t>(
            field,
            [&o](std::int32_t prev, std::int32_t next)
            {
                record_trap(o, prev, next);
            });
    }
}

VMHOOK_JVM_MODULE(watch_static_field)
{
    vmhook::register_class<wsf>("vmhook/fixtures/WatchStaticField");

    // =====================================================================
    //  0. Sanity: the class + every watched field + the handshake resolve.
    // =====================================================================
    ctx.check("wsf_class_registered_counterA_resolves", wsf::resolves("counterA"));
    ctx.check("wsf_counterB_resolves", wsf::resolves("counterB"));
    ctx.check("wsf_counterC_resolves", wsf::resolves("counterC"));
    ctx.check("wsf_counterD_resolves", wsf::resolves("counterD"));
    ctx.check("wsf_counterE_resolves", wsf::resolves("counterE"));
    ctx.check("wsf_handshake_go_resolves", wsf::resolves("go"));
    ctx.check("wsf_ready_flag_true", wsf::counter("ready") != 0);

    // Compile-time capability advertisement, surfaced for log readers.
#if VMHOOK_HAS_HW_DATA_BREAKPOINTS
    ctx.record("[INFO] watch_static_field: hardware data breakpoints SUPPORTED on "
               "this platform (Windows x86_64) -- exercising the live DR-trap path.");
#else
    ctx.record("[INFO] watch_static_field: hardware data breakpoints UNSUPPORTED on "
               "this platform -- asserting the empty-handle fallback contract only.");
#endif

    // =====================================================================
    //  1. HEADLINE: watch a static int, drive a Java write, assert the
    //     callback fired with the NEW value.  This is the migrated
    //     test_field_watcher contract, sharpened: exact fire count, exact
    //     final value, and the `old` placeholder is the documented zero.
    // =====================================================================
    {
        // Clean the Java-side counters first (mode 0) so counterA starts at 0.
        const bool reset_done{ drive(ctx, 0) };
        ctx.check("headline_reset_probe_completed", reset_done);
        ctx.check("headline_counterA_starts_zero", wsf::counter("counterA") == 0);

        auto watch{ install_watch("counterA", g_obs_a) };

#if VMHOOK_HAS_HW_DATA_BREAKPOINTS
        ctx.check("headline_watch_running", watch.running());

        const bool done{ drive(ctx, 1) };   // writes counterA WRITE_COUNT times
        ctx.check("headline_probe_completed", done);

        // Java really performed the writes (independent of the trap firing).
        ctx.check("headline_java_writes_made", wsf::get_writes_made() == WRITE_COUNT);
        ctx.check("headline_counterA_final_value", wsf::counter("counterA") == FINAL_VALUE);

        // The trap fired -- and (synchronous, one-per-store) fired for EVERY
        // write.  We assert >= 1 strictly and == WRITE_COUNT as the precise
        // contract; both hold deterministically because the writing thread is
        // armed at install time and the trap is taken in-line on each store.
        ctx.check("headline_callback_fired", g_obs_a.fires.load() > 0);
        ctx.check("headline_callback_fired_once_per_write",
                  g_obs_a.fires.load() == WRITE_COUNT);

        // The callback observed the field's NEW value: the maximum value it
        // ever saw is the final value, and the LAST value it saw is the final
        // value (the trap reads memory at store time, so it sees the freshly
        // written content).
        ctx.check("headline_callback_saw_new_value_max",
                  g_obs_a.max_new.load() == FINAL_VALUE);
        ctx.check("headline_callback_last_value_is_final",
                  g_obs_a.last_new.load() == FINAL_VALUE);
        ctx.check("headline_callback_values_monotonic", g_obs_a.monotonic.load());

        // The `old`/`prev` argument is always the zero placeholder (documented
        // limitation: the CPU cannot reconstruct the pre-write value).
        ctx.check("headline_prev_arg_is_zero_placeholder", g_obs_a.prev_was_zero.load());
#else
        // Unsupported platform: the watch must be an inert empty handle (NOT a
        // silent poller), and the Java writes still run so the fixture is sound.
        ctx.check("headline_watch_empty_on_unsupported_platform", !watch.running());
        const bool done{ drive(ctx, 1) };
        ctx.check("headline_probe_completed", done);
        ctx.check("headline_java_writes_made", wsf::get_writes_made() == WRITE_COUNT);
        ctx.check("headline_counterA_final_value", wsf::counter("counterA") == FINAL_VALUE);
        ctx.check("headline_callback_did_not_fire_unsupported", g_obs_a.fires.load() == 0);
#endif
    }
    // watch dropped here -> slot freed, trap disarmed on every thread.

#if VMHOOK_HAS_HW_DATA_BREAKPOINTS
    // =====================================================================
    //  2. DR-SLOT CAPACITY: there are exactly four hardware slots (DR0-DR3).
    //     Fill all four with independent watches (all running()), then prove a
    //     FIFTH watch is REFUSED with an empty handle.  This characterises the
    //     capacity limit deterministically -- find_free_slot returns -1 once
    //     all four in_use flags are set, no Java write required.
    // =====================================================================
    {
        auto wa{ install_watch("counterA", g_obs_a) };
        auto wb{ install_watch("counterB", g_obs_b) };
        auto wc{ install_watch("counterC", g_obs_c) };
        auto wd{ install_watch("counterD", g_obs_d) };

        ctx.check("capacity_slot0_running", wa.running());
        ctx.check("capacity_slot1_running", wb.running());
        ctx.check("capacity_slot2_running", wc.running());
        ctx.check("capacity_slot3_running", wd.running());
        ctx.check("capacity_four_watches_all_armed",
                  wa.running() && wb.running() && wc.running() && wd.running());

        // The fifth watch must fail: all four DR slots are occupied.
        {
            auto w5{ install_watch("counterE", g_obs_e) };
            ctx.check("capacity_fifth_watch_refused_empty_handle", !w5.running());
            // w5 (empty) dropped here -- dropping an empty handle is a safe no-op
            // and must NOT release a slot belonging to wa..wd.
        }

        // The four originals are still armed after the refused fifth attempt.
        ctx.check("capacity_originals_survive_refused_fifth",
                  wa.running() && wb.running() && wc.running() && wd.running());
        ctx.record("[INFO] watch_static_field: DR-slot capacity characterised -- exactly "
                   "4 simultaneous watches (CPU DR0-DR3); the 5th install returns an "
                   "empty watch_handle (running()==false) rather than crashing.");

        // -----------------------------------------------------------------
        //  2b. INDEPENDENCE: with all four armed, driving ONE field fires ONLY
        //      that field's callback (correct slot<->address binding).
        // -----------------------------------------------------------------
        {
            // mode 2 writes counterB only -> only watch-B fires.
            const bool done{ drive(ctx, 2) };
            ctx.check("independence_modeB_probe_completed", done);
            ctx.check("independence_only_B_fired_count", g_obs_b.fires.load() == WRITE_COUNT);
            ctx.check("independence_B_saw_final_value", g_obs_b.max_new.load() == FINAL_VALUE);
            ctx.check("independence_A_did_not_fire", g_obs_a.fires.load() == 0);
            ctx.check("independence_C_did_not_fire", g_obs_c.fires.load() == 0);
            ctx.check("independence_D_did_not_fire", g_obs_d.fires.load() == 0);
        }
        {
            // mode 4 writes counterD only -> only watch-D fires.
            const bool done{ drive(ctx, 4) };
            ctx.check("independence_modeD_probe_completed", done);
            ctx.check("independence_only_D_fired_count", g_obs_d.fires.load() == WRITE_COUNT);
            ctx.check("independence_D_saw_final_value", g_obs_d.max_new.load() == FINAL_VALUE);
            ctx.check("independence_A_still_silent", g_obs_a.fires.load() == 0);
            ctx.check("independence_B_silent_on_D_write", g_obs_b.fires.load() == 0);
            ctx.check("independence_C_silent_on_D_write", g_obs_c.fires.load() == 0);
        }

        // -----------------------------------------------------------------
        //  3. REMOVAL FREES A SLOT: drop ONE of the four held watches; a fresh
        //     install (which a moment ago was refused as the fifth) now
        //     succeeds, landing in the freed slot.
        // -----------------------------------------------------------------
        wc.stop();                                  // explicit release of slot 2
        ctx.check("removal_wc_stopped_not_running", !wc.running());
        {
            auto w_new{ install_watch("counterE", g_obs_e) };
            ctx.check("removal_freed_slot_allows_new_watch", w_new.running());

            // And the newly-installed watch actually works: drive counterE.
            const bool done{ drive(ctx, 5) };
            ctx.check("removal_newwatch_probe_completed", done);
            ctx.check("removal_newwatch_E_fired", g_obs_e.fires.load() == WRITE_COUNT);
            ctx.check("removal_newwatch_E_saw_final", g_obs_e.max_new.load() == FINAL_VALUE);
            // The still-held A/B/D did not fire on a counterE write.
            ctx.check("removal_A_silent_on_E_write", g_obs_a.fires.load() == 0);
            ctx.check("removal_B_silent_on_E_write", g_obs_b.fires.load() == 0);
            ctx.check("removal_D_silent_on_E_write", g_obs_d.fires.load() == 0);
            // w_new dropped here -> its slot released again.
        }
        // wa, wb, wd dropped here -> all remaining slots released.
    }

    // =====================================================================
    //  4. WRITES AFTER REMOVAL DON'T FIRE: install a watch, prove it fires,
    //     stop it, then drive more writes to the SAME field and assert NO
    //     further callback fires (the trap is disarmed on every thread).
    // =====================================================================
    {
        // Reset counterA to 0 first so the post-removal writes are unambiguous.
        const bool reset_done{ drive(ctx, 0) };
        ctx.check("afterremove_reset_probe_completed", reset_done);

        auto watch{ install_watch("counterA", g_obs_a) };
        ctx.check("afterremove_watch_running", watch.running());

        // Drive once with the watch armed -> it fires.
        {
            const bool done{ drive(ctx, 1) };
            ctx.check("afterremove_armed_probe_completed", done);
            ctx.check("afterremove_armed_fired", g_obs_a.fires.load() == WRITE_COUNT);
        }

        // Stop the watch (frees the slot, disarms the trap on every thread).
        watch.stop();
        ctx.check("afterremove_watch_stopped_not_running", !watch.running());

        // Reset the Java counter to 0 (mode 0) and drive more writes -- WITHOUT
        // clearing the native fire accounting -- then assert the stopped watch
        // recorded ZERO additional fires.
        {
            const bool reset2{ drive_keep_obs(ctx, 0) };
            ctx.check("afterremove_second_reset_completed", reset2);
            const std::int32_t fires_before{ g_obs_a.fires.load() };

            const bool done{ drive_keep_obs(ctx, 1) }; // writes counterA again
            ctx.check("afterremove_post_stop_probe_completed", done);
            ctx.check("afterremove_java_actually_wrote_again",
                      wsf::counter("counterA") == FINAL_VALUE);
            ctx.check("afterremove_no_fire_after_stop",
                      g_obs_a.fires.load() == fires_before);
        }
    }
    // watch already stopped; nothing armed.

    // =====================================================================
    //  5. Idempotent stop + double-drop safety: a watch that is stop()'d
    //     twice (and then destroyed) must not misbehave; running() stays
    //     false.  This guards the slot-release path against double-free.
    // =====================================================================
    {
        auto watch{ install_watch("counterA", g_obs_a) };
        ctx.check("idem_watch_running", watch.running());
        watch.stop();
        ctx.check("idem_after_first_stop_not_running", !watch.running());
        watch.stop();   // second stop -- must be a safe no-op
        ctx.check("idem_after_second_stop_not_running", !watch.running());
        // destructor runs here on an already-stopped handle -> safe.
    }

    // After all watches are released, four fresh slots must be available again:
    // installing four more must all succeed (proves the capacity recovered and
    // no slot leaked across the scenarios above).
    {
        auto wa{ install_watch("counterA", g_obs_a) };
        auto wb{ install_watch("counterB", g_obs_b) };
        auto wc{ install_watch("counterC", g_obs_c) };
        auto wd{ install_watch("counterD", g_obs_d) };
        ctx.check("recovery_all_four_slots_reusable",
                  wa.running() && wb.running() && wc.running() && wd.running());
        // All four dropped here -> JVM left with NO armed watchpoints, as
        // required so no trap fires inside a later module.
    }
#endif // VMHOOK_HAS_HW_DATA_BREAKPOINTS

    // =====================================================================
    //  6. Final safety net: leave the Java fixture's counters reset so a
    //     later module reading these fields sees a clean slate, and (belt and
    //     braces) confirm no watch is reported running by re-driving a write
    //     with NO watch installed -- nothing should fire.
    // =====================================================================
    {
        reset_observations();
        const bool reset_done{ drive_keep_obs(ctx, 0) };
        ctx.check("final_reset_probe_completed", reset_done);
        ctx.check("final_counterA_reset", wsf::counter("counterA") == 0);

        const bool done{ drive_keep_obs(ctx, 1) }; // write with NOTHING armed
        ctx.check("final_unwatched_probe_completed", done);
        ctx.check("final_no_callback_when_unwatched",
                  g_obs_a.fires.load() == 0 && g_obs_b.fires.load() == 0
                      && g_obs_c.fires.load() == 0 && g_obs_d.fires.load() == 0
                      && g_obs_e.fires.load() == 0);

        // Leave counters clean for the next module.
        const bool final_reset{ drive_keep_obs(ctx, 0) };
        ctx.check("final_leave_clean_state", final_reset && wsf::counter("counterA") == 0);
    }
}
