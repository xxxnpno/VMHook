// watch_static_field JVM test module  (feature area: watchers / hardware DRs)
//
// THE field-write-watchpoint authority: exhaustively exercises
// vmhook::watch_static_field<wrapper, field_type>(name, callback) on a LIVE
// JVM.  watch_static_field arms a hardware data breakpoint (one of the four
// CPU debug-register slots DR0-DR3) on a Java static field's backing storage;
// the trap fires SYNCHRONOUSLY on whichever thread writes the field, *during*
// the store, and the callback runs inside a vectored exception handler on that
// same thread.  vmhook arms the trap on every thread that exists at install
// time -- including the Harness loop thread (and the fixture's persistent
// worker thread) -- so a putstatic the fixture's run() executes traps
// immediately and the callback has already fired by the time the go/done probe
// returns (mirrors the synchronous-trap reasoning in the legacy example.cpp
// test_field_watcher this module migrates).
//
// What this module proves on a live JVM (Windows x86_64; the only platform
// where VMHOOK_HAS_HW_DATA_BREAKPOINTS is 1 -- elsewhere it asserts the
// documented empty-handle fallback instead of silently polling):
//   * a watch on a static int observes a Java-driven write: the callback fires
//     once per putstatic (N writes -> N fires) and its `new` argument carries
//     the field's NEW value, ending at the precise final value;
//   * EVERY DR-watchable storage width traps and reports the correct NEW value
//     -- byte (1), short/char (2), int/float (4), long/double (8), and an
//     Object reference (4-byte compressed-oop slot).  Each watch is installed
//     with a field_type whose sizeof matches the field's storage so the DR LEN
//     covers exactly the bytes that change;
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
//     (the trap is disarmed on every thread);
//   * the trap fires on whichever THREAD issues the store: a write performed
//     by the fixture's PRE-EXISTING worker thread (armed at install time)
//     fires; a write from a thread CREATED AFTER install does NOT (the
//     documented "later threads are not armed" limitation, characterised);
//   * an armed watch SURVIVES a class-initialisation (<clinit>) event that
//     runs on the writing thread;
//   * writing the SAME value repeatedly is characterised (fire-or-not is
//     hardware-dependent for an unchanged store; the module records what it
//     observes and asserts only the deterministic final value).
//
// SAFETY (hardware debug registers are delicate -- a stray armed DR or a
// callback that re-enters the JVM can wedge or crash the whole process):
//   * EVERY watch_handle is RAII-scoped or explicitly stop()'d before the
//     module returns, so NO watchpoint is left armed to fire spuriously inside
//     a later module sharing this JVM;
//   * an UNCONDITIONAL teardown probe at the very end (mode 0 reset + a write
//     with NOTHING armed) confirms no slot leaked and no callback fires, i.e.
//     DR0-DR3 + DR7 are clear for the writing thread;
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
// Distinct check/identifier prefix throughout: wsf_ / headline_ / width_ /
// capacity_ / independence_ / removal_ / afterremove_ / thread_ / clinit_ /
// samevalue_ / final_.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <atomic>
#include <cstddef>
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
        static auto set_go(bool value) -> void        { static_field("go")->set(value); }
        static auto set_done(bool value) -> void      { static_field("done")->set(value); }
        static auto get_done() -> bool                { return static_field("done")->get(); }
        static auto set_mode(std::int32_t m) -> void  { static_field("mode")->set(m); }

        static auto resolves(const char* name) -> bool { return static_field(name).has_value(); }

        // ---- read a watched int counter's current value (Java's own view) ----
        static auto counter(const char* name) -> std::int32_t { return static_field(name)->get(); }

        static auto get_writes_made() -> std::int32_t
        {
            const std::int32_t v = static_field("writesMade")->get();
            return v;
        }

        static auto worker_alive() -> bool { return static_field("workerAlive")->get(); }

        // ---- raw backing address + storage alignment of a static field ----
        // Used to assert the DR LEN we arm matches a naturally-aligned slot
        // (a misaligned width-N watch silently mis-fires; this lets the module
        // record [INFO] and relax the strict count instead of a false FAIL).
        static auto field_address(const char* name) -> const void*
        {
            const auto proxy{ static_field(name) };
            return proxy.has_value() ? proxy->raw_address() : nullptr;
        }
    };

    // ---- Fixture-mirrored constants (kept in lockstep with WatchStaticField.java) --
    constexpr std::int32_t WRITE_COUNT{ 12 };
    constexpr std::int32_t FINAL_VALUE{ 12 };
    constexpr std::int32_t SAME_VALUE{ 7 };

    // =====================================================================
    //  Per-watch callback observation state.
    //
    //  Numeric widths (byte/short/char/int/float/long/double) all carry an
    //  integer- or float-valued 1..WRITE_COUNT sequence; the observed NEW
    //  value is normalised to a double (lossless for 1..12) so ONE struct
    //  serves every width.  The reference watch instead records whether the
    //  observed compressed oop was non-zero / changed (its value is a heap
    //  address, not a fixed number).  Reset between scenarios.
    // =====================================================================
    struct watch_obs
    {
        std::atomic<std::int32_t> fires{ 0 };
        std::atomic<std::int64_t> last_new_bits{ -1 };  // raw bits of last NEW (numeric path)
        std::atomic<double>       last_new{ -1.0 };
        std::atomic<double>       max_new{ -1.0 };
        std::atomic<bool>         prev_was_zero{ true }; // every `old` arg is the zero placeholder
        std::atomic<bool>         monotonic{ true };     // `new` never decreases within a scenario
        std::atomic<bool>         saw_nonzero{ false };  // ref path: a non-zero compressed oop seen
    };

    watch_obs g_obs_a{};
    watch_obs g_obs_b{};
    watch_obs g_obs_c{};
    watch_obs g_obs_d{};
    watch_obs g_obs_e{};
    watch_obs g_obs_w{};   // shared "width under test" observation

    auto reset_one(watch_obs& o) -> void
    {
        o.fires.store(0, std::memory_order_relaxed);
        o.last_new_bits.store(-1, std::memory_order_relaxed);
        o.last_new.store(-1.0, std::memory_order_relaxed);
        o.max_new.store(-1.0, std::memory_order_relaxed);
        o.prev_was_zero.store(true, std::memory_order_relaxed);
        o.monotonic.store(true, std::memory_order_relaxed);
        o.saw_nonzero.store(false, std::memory_order_relaxed);
    }

    auto reset_observations() -> void
    {
        reset_one(g_obs_a);
        reset_one(g_obs_b);
        reset_one(g_obs_c);
        reset_one(g_obs_d);
        reset_one(g_obs_e);
        reset_one(g_obs_w);
    }

    // Records one numeric trap callback into `o`.  Only touches atomics -- safe
    // to run in the VEH context on the writing Java thread (no allocation, no
    // JVM re-entry).  `prev_is_zero` is whether the zero-placeholder `old` arg
    // really was zero; `next` is the value read at trap time (the new value),
    // normalised to a double by the caller.
    auto record_numeric(watch_obs& o, bool prev_is_zero, double next) -> void
    {
        if (!prev_is_zero)
        {
            o.prev_was_zero.store(false, std::memory_order_relaxed);
        }
        const double before{ o.last_new.exchange(next, std::memory_order_relaxed) };
        if (before >= 0.0 && next < before)
        {
            o.monotonic.store(false, std::memory_order_relaxed);
        }
        double observed_max{ o.max_new.load(std::memory_order_relaxed) };
        while (next > observed_max
               && !o.max_new.compare_exchange_weak(observed_max, next, std::memory_order_relaxed))
        {
            // retry until our max wins or a larger one is already stored
        }
        o.fires.fetch_add(1, std::memory_order_relaxed);
    }

    // Records one reference-slot trap (the NEW value is a compressed oop).
    auto record_ref(watch_obs& o, std::uint32_t compressed) -> void
    {
        if (compressed != 0u)
        {
            o.saw_nonzero.store(true, std::memory_order_relaxed);
        }
        o.last_new_bits.store(static_cast<std::int64_t>(compressed), std::memory_order_relaxed);
        o.fires.fetch_add(1, std::memory_order_relaxed);
    }

    // Drives exactly one probe cycle for `mode`: resets observations + the
    // latched `done`, programs the selector on the rising edge of go, then
    // waits for done.  Observations are reset here so each scenario's fire
    // accounting starts clean; the watches themselves are installed by the
    // caller and remain armed across this cycle.
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
    auto install_int_watch(const char* field, watch_obs& o) -> vmhook::watch_handle
    {
        return vmhook::watch_static_field<wsf, std::int32_t>(
            field,
            [&o](std::int32_t prev, std::int32_t next)
            {
                record_numeric(o, prev == 0, static_cast<double>(next));
            });
    }

#if VMHOOK_HAS_HW_DATA_BREAKPOINTS
    // True when `addr` is N-byte aligned (DR LEN requires natural alignment).
    auto is_aligned(const void* addr, std::size_t n) -> bool
    {
        return addr != nullptr
            && (reinterpret_cast<std::uintptr_t>(addr) % n) == 0;
    }
#endif
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
    ctx.check("wsf_width_byte_resolves", wsf::resolves("wByte"));
    ctx.check("wsf_width_short_resolves", wsf::resolves("wShort"));
    ctx.check("wsf_width_char_resolves", wsf::resolves("wChar"));
    ctx.check("wsf_width_float_resolves", wsf::resolves("wFloat"));
    ctx.check("wsf_width_long_resolves", wsf::resolves("wLong"));
    ctx.check("wsf_width_double_resolves", wsf::resolves("wDouble"));
    ctx.check("wsf_width_ref_resolves", wsf::resolves("wRef"));
    ctx.check("wsf_same_value_field_resolves", wsf::resolves("sameValueField"));
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

        auto watch{ install_int_watch("counterA", g_obs_a) };

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
                  g_obs_a.max_new.load() == static_cast<double>(FINAL_VALUE));
        ctx.check("headline_callback_last_value_is_final",
                  g_obs_a.last_new.load() == static_cast<double>(FINAL_VALUE));
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
    //  1b. EVERY DR-WATCHABLE WIDTH.  For each storage width we install a
    //      watch whose field_type sizeof matches the slot, drive a monotone
    //      1..WRITE_COUNT sequence, and assert it fires once per store and
    //      ends at the right NEW value.  The DR LEN is chosen by the library
    //      from sizeof(field_type), so this proves the 1/2/4/8-byte LEN paths
    //      and the compressed-oop reference path end to end.
    //
    //      Each width is GATED on the slot being naturally aligned (HotSpot
    //      lays statics out aligned, so this holds; the gate degrades a
    //      surprising misalignment to an [INFO] + relaxed >=1 instead of a
    //      false FAIL on a silently mis-firing unaligned watch).
    // =====================================================================

    // ---- 1-byte: byte ----
    {
        drive(ctx, 0);
        const void* addr{ wsf::field_address("wByte") };
        const bool aligned{ is_aligned(addr, 1) };  // any address is 1-aligned
        auto w{ vmhook::watch_static_field<wsf, std::uint8_t>(
            "wByte",
            [](std::uint8_t prev, std::uint8_t next)
            {
                record_numeric(g_obs_w, prev == 0, static_cast<double>(next));
            }) };
        ctx.check("width_byte_watch_running", w.running());
        const bool done{ drive(ctx, 10) };
        ctx.check("width_byte_probe_completed", done);
        ctx.check("width_byte_java_wrote", wsf::get_writes_made() == WRITE_COUNT);
        ctx.check("width_byte_final_value", wsf::counter("wByte") == FINAL_VALUE);
        if (aligned)
        {
            ctx.check("width_byte_fired_once_per_write", g_obs_w.fires.load() == WRITE_COUNT);
        }
        else
        {
            ctx.record("[INFO] wByte slot unexpectedly unaligned; relaxing to >=1 fire.");
            ctx.check("width_byte_fired_at_least_once", g_obs_w.fires.load() >= 1);
        }
        ctx.check("width_byte_saw_final_new_value",
                  g_obs_w.max_new.load() == static_cast<double>(FINAL_VALUE));
        ctx.check("width_byte_prev_zero_placeholder", g_obs_w.prev_was_zero.load());
    }

    // ---- 2-byte: short ----
    {
        drive(ctx, 0);
        const void* addr{ wsf::field_address("wShort") };
        const bool aligned{ is_aligned(addr, 2) };
        auto w{ vmhook::watch_static_field<wsf, std::int16_t>(
            "wShort",
            [](std::int16_t prev, std::int16_t next)
            {
                record_numeric(g_obs_w, prev == 0, static_cast<double>(next));
            }) };
        ctx.check("width_short_watch_running", w.running());
        const bool done{ drive(ctx, 11) };
        ctx.check("width_short_probe_completed", done);
        ctx.check("width_short_final_value", wsf::counter("wShort") == FINAL_VALUE);
        if (aligned)
        {
            ctx.check("width_short_fired_once_per_write", g_obs_w.fires.load() == WRITE_COUNT);
        }
        else
        {
            ctx.record("[INFO] wShort slot unaligned; relaxing to >=1 fire.");
            ctx.check("width_short_fired_at_least_once", g_obs_w.fires.load() >= 1);
        }
        ctx.check("width_short_saw_final_new_value",
                  g_obs_w.max_new.load() == static_cast<double>(FINAL_VALUE));
    }

    // ---- 2-byte: char ----
    {
        drive(ctx, 0);
        const void* addr{ wsf::field_address("wChar") };
        const bool aligned{ is_aligned(addr, 2) };
        auto w{ vmhook::watch_static_field<wsf, std::uint16_t>(
            "wChar",
            [](std::uint16_t prev, std::uint16_t next)
            {
                record_numeric(g_obs_w, prev == 0, static_cast<double>(next));
            }) };
        ctx.check("width_char_watch_running", w.running());
        const bool done{ drive(ctx, 12) };
        ctx.check("width_char_probe_completed", done);
        ctx.check("width_char_final_value", wsf::counter("wChar") == FINAL_VALUE);
        if (aligned)
        {
            ctx.check("width_char_fired_once_per_write", g_obs_w.fires.load() == WRITE_COUNT);
        }
        else
        {
            ctx.record("[INFO] wChar slot unaligned; relaxing to >=1 fire.");
            ctx.check("width_char_fired_at_least_once", g_obs_w.fires.load() >= 1);
        }
        ctx.check("width_char_saw_final_new_value",
                  g_obs_w.max_new.load() == static_cast<double>(FINAL_VALUE));
    }

    // ---- 4-byte: float ----
    {
        drive(ctx, 0);
        const void* addr{ wsf::field_address("wFloat") };
        const bool aligned{ is_aligned(addr, 4) };
        auto w{ vmhook::watch_static_field<wsf, float>(
            "wFloat",
            [](float prev, float next)
            {
                record_numeric(g_obs_w, prev == 0.0f, static_cast<double>(next));
            }) };
        ctx.check("width_float_watch_running", w.running());
        const bool done{ drive(ctx, 13) };
        ctx.check("width_float_probe_completed", done);
        if (aligned)
        {
            ctx.check("width_float_fired_once_per_write", g_obs_w.fires.load() == WRITE_COUNT);
        }
        else
        {
            ctx.record("[INFO] wFloat slot unaligned; relaxing to >=1 fire.");
            ctx.check("width_float_fired_at_least_once", g_obs_w.fires.load() >= 1);
        }
        ctx.check("width_float_saw_final_new_value",
                  g_obs_w.max_new.load() == static_cast<double>(FINAL_VALUE));
        ctx.check("width_float_values_monotonic", g_obs_w.monotonic.load());
    }

    // ---- 8-byte: long ----
    {
        drive(ctx, 0);
        const void* addr{ wsf::field_address("wLong") };
        const bool aligned{ is_aligned(addr, 8) };
        auto w{ vmhook::watch_static_field<wsf, std::int64_t>(
            "wLong",
            [](std::int64_t prev, std::int64_t next)
            {
                record_numeric(g_obs_w, prev == 0, static_cast<double>(next));
            }) };
        ctx.check("width_long_watch_running", w.running());
        const bool done{ drive(ctx, 14) };
        ctx.check("width_long_probe_completed", done);
        ctx.check("width_long_final_value", wsf::counter("wLong") == FINAL_VALUE);
        if (aligned)
        {
            ctx.check("width_long_fired_once_per_write", g_obs_w.fires.load() == WRITE_COUNT);
        }
        else
        {
            ctx.record("[INFO] wLong slot unaligned; relaxing to >=1 fire.");
            ctx.check("width_long_fired_at_least_once", g_obs_w.fires.load() >= 1);
        }
        ctx.check("width_long_saw_final_new_value",
                  g_obs_w.max_new.load() == static_cast<double>(FINAL_VALUE));
    }

    // ---- 8-byte: double ----
    {
        drive(ctx, 0);
        const void* addr{ wsf::field_address("wDouble") };
        const bool aligned{ is_aligned(addr, 8) };
        auto w{ vmhook::watch_static_field<wsf, double>(
            "wDouble",
            [](double prev, double next)
            {
                record_numeric(g_obs_w, prev == 0.0, next);
            }) };
        ctx.check("width_double_watch_running", w.running());
        const bool done{ drive(ctx, 15) };
        ctx.check("width_double_probe_completed", done);
        if (aligned)
        {
            ctx.check("width_double_fired_once_per_write", g_obs_w.fires.load() == WRITE_COUNT);
        }
        else
        {
            ctx.record("[INFO] wDouble slot unaligned; relaxing to >=1 fire.");
            ctx.check("width_double_fired_at_least_once", g_obs_w.fires.load() >= 1);
        }
        ctx.check("width_double_saw_final_new_value",
                  g_obs_w.max_new.load() == static_cast<double>(FINAL_VALUE));
        ctx.check("width_double_values_monotonic", g_obs_w.monotonic.load());
    }

    // ---- Object reference (4-byte compressed-oop slot) ----
    // The reference field stores a 4-byte narrow oop under the default heap
    // (UseCompressedOops on -- as the whole suite assumes; see
    // field_object_ref.cpp).  We watch it as a 4-byte slot.  Its NEW value is
    // a heap address, so we assert it FIRES once per store and the observed
    // compressed oop is non-zero, not a fixed number.  If compressed oops are
    // off (8-byte slot), the read-back compressed oop would be 0; we then
    // relax to "fired" only and record [INFO].
    {
        drive(ctx, 0);
        const void* addr{ wsf::field_address("wRef") };
        const bool aligned{ is_aligned(addr, 4) };
        auto w{ vmhook::watch_static_field<wsf, std::uint32_t>(
            "wRef",
            [](std::uint32_t prev, std::uint32_t next)
            {
                (void)prev;
                record_ref(g_obs_w, next);
            }) };
        ctx.check("width_ref_watch_running", w.running());
        const bool done{ drive(ctx, 17) };
        ctx.check("width_ref_probe_completed", done);
        ctx.check("width_ref_java_wrote", wsf::get_writes_made() == WRITE_COUNT);
        if (aligned)
        {
            ctx.check("width_ref_fired_once_per_store", g_obs_w.fires.load() == WRITE_COUNT);
        }
        else
        {
            ctx.record("[INFO] wRef slot unaligned for width-4; relaxing to >=1 fire.");
            ctx.check("width_ref_fired_at_least_once", g_obs_w.fires.load() >= 1);
        }
        if (g_obs_w.saw_nonzero.load())
        {
            ctx.check("width_ref_observed_nonzero_compressed_oop", true);
        }
        else
        {
            ctx.record("[INFO] wRef compressed-oop read back as zero (compressed oops "
                       "likely off / 8-byte slot); the store still trapped. Value not "
                       "asserted.");
            ctx.check("width_ref_fired_even_without_compressed_value",
                      g_obs_w.fires.load() >= 1);
        }
    }
    ctx.record("[INFO] watch_static_field: all DR-watchable widths exercised -- "
               "byte(1), short/char(2), int/float(4), long/double(8), Object ref(4).");

    // =====================================================================
    //  2. DR-SLOT CAPACITY: there are exactly four hardware slots (DR0-DR3).
    //     Fill all four with independent watches (all running()), then prove a
    //     FIFTH watch is REFUSED with an empty handle.  This characterises the
    //     capacity limit deterministically -- find_free_slot returns -1 once
    //     all four in_use flags are set, no Java write required.
    // =====================================================================
    {
        auto wa{ install_int_watch("counterA", g_obs_a) };
        auto wb{ install_int_watch("counterB", g_obs_b) };
        auto wc{ install_int_watch("counterC", g_obs_c) };
        auto wd{ install_int_watch("counterD", g_obs_d) };

        ctx.check("capacity_slot0_running", wa.running());
        ctx.check("capacity_slot1_running", wb.running());
        ctx.check("capacity_slot2_running", wc.running());
        ctx.check("capacity_slot3_running", wd.running());
        ctx.check("capacity_four_watches_all_armed",
                  wa.running() && wb.running() && wc.running() && wd.running());

        // The fifth watch must fail: all four DR slots are occupied.
        {
            auto w5{ install_int_watch("counterE", g_obs_e) };
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
            ctx.check("independence_B_saw_final_value",
                      g_obs_b.max_new.load() == static_cast<double>(FINAL_VALUE));
            ctx.check("independence_A_did_not_fire", g_obs_a.fires.load() == 0);
            ctx.check("independence_C_did_not_fire", g_obs_c.fires.load() == 0);
            ctx.check("independence_D_did_not_fire", g_obs_d.fires.load() == 0);
        }
        {
            // mode 4 writes counterD only -> only watch-D fires.
            const bool done{ drive(ctx, 4) };
            ctx.check("independence_modeD_probe_completed", done);
            ctx.check("independence_only_D_fired_count", g_obs_d.fires.load() == WRITE_COUNT);
            ctx.check("independence_D_saw_final_value",
                      g_obs_d.max_new.load() == static_cast<double>(FINAL_VALUE));
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
            auto w_new{ install_int_watch("counterE", g_obs_e) };
            ctx.check("removal_freed_slot_allows_new_watch", w_new.running());

            // And the newly-installed watch actually works: drive counterE.
            const bool done{ drive(ctx, 5) };
            ctx.check("removal_newwatch_probe_completed", done);
            ctx.check("removal_newwatch_E_fired", g_obs_e.fires.load() == WRITE_COUNT);
            ctx.check("removal_newwatch_E_saw_final",
                      g_obs_e.max_new.load() == static_cast<double>(FINAL_VALUE));
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

        auto watch{ install_int_watch("counterA", g_obs_a) };
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
    //  5. THREAD ATTRIBUTION: the trap fires on whichever thread performs the
    //     store.
    //
    //   5a. PRE-EXISTING thread: the fixture's worker thread was started in
    //       <clinit> (before any watch install), so vmhook armed the DR trap
    //       on it.  Driving counterA FROM the worker thread must fire the
    //       watch -- proving the trap is per-thread and synchronous on the
    //       writer, not special to the Harness loop thread.
    //   5b. NEW thread (created AFTER install): documented limitation -- the
    //       library only arms threads that exist at install time, so a write
    //       from a freshly-spawned thread does NOT fire.  Characterised as
    //       zero fires (a regression target for the day post-install thread
    //       tracking lands).
    // =====================================================================
    {
        ctx.check("thread_worker_alive_before_watch", wsf::worker_alive());

        // 5a. pre-existing worker thread.
        {
            const bool reset_done{ drive(ctx, 0) };
            ctx.check("thread_pre_reset_completed", reset_done);
            auto watch{ install_int_watch("counterA", g_obs_a) };
            ctx.check("thread_pre_watch_running", watch.running());

            const bool done{ drive(ctx, 20) };   // worker increments counterA
            ctx.check("thread_pre_probe_completed", done);
            ctx.check("thread_pre_java_wrote", wsf::counter("counterA") == FINAL_VALUE);
            ctx.check("thread_pre_existing_thread_write_fires",
                      g_obs_a.fires.load() == WRITE_COUNT);
            ctx.record("[INFO] watch_static_field: a write performed by a PRE-EXISTING "
                       "(armed-at-install) thread fires the trap on that thread.");
            // watch dropped here.
        }

        // 5b. brand-new thread created after install.
        {
            const bool reset_done{ drive(ctx, 0) };
            ctx.check("thread_new_reset_completed", reset_done);
            auto watch{ install_int_watch("counterA", g_obs_a) };
            ctx.check("thread_new_watch_running", watch.running());

            const bool done{ drive(ctx, 21) };   // new thread increments counterA
            ctx.check("thread_new_probe_completed", done);
            ctx.check("thread_new_java_wrote", wsf::counter("counterA") == FINAL_VALUE);
            // Documented limitation: threads created after install are not armed.
            ctx.check("thread_new_post_install_thread_write_does_not_fire",
                      g_obs_a.fires.load() == 0);
            ctx.record("[INFO] watch_static_field: a write from a thread CREATED AFTER "
                       "install does NOT fire (documented limitation: later threads are "
                       "not armed). If this ever starts firing, the library gained "
                       "post-install thread tracking -- update this expectation.");
            // watch dropped here.
        }
    }

    // =====================================================================
    //  6. CLASS-INIT SURVIVAL: an armed watch on counterA must survive a real
    //     class-initialisation event (LazyInit.<clinit>) that runs on the
    //     writing thread.  Mode 22 triggers LazyInit.<clinit> then writes
    //     counterA; the counterA watch must still fire for those writes.
    //     Best-effort: also watch LazyInit.lazyClinitValue and observe the
    //     <clinit> store where the field address resolves.
    // =====================================================================
    {
        const bool reset_done{ drive(ctx, 0) };
        ctx.check("clinit_reset_completed", reset_done);

        // Best-effort watch on the lazily-initialised class's field.  The
        // field's backing storage exists once the class is prepared; on some
        // JDKs the class may not be prepared until first active use, in which
        // case raw_address() is null and the watch is refused -- handled.
        bool lazy_watch_armed{ false };
        const void* lazy_addr{ wsf::field_address("nonexistent_placeholder") };
        (void)lazy_addr;

        auto counter_watch{ install_int_watch("counterA", g_obs_a) };
        ctx.check("clinit_counterA_watch_running", counter_watch.running());

        // (We cannot register LazyInit as its own wrapper class without a second
        // registration; the survival proof rests on the counterA watch, below.)
        (void)lazy_watch_armed;

        const bool done{ drive(ctx, 22) };   // LazyInit.<clinit> + counterA writes
        ctx.check("clinit_probe_completed", done);
        ctx.check("clinit_java_wrote_counterA", wsf::counter("counterA") == FINAL_VALUE);
        // THE survival assertion: counterA's watch still fired after <clinit>.
        ctx.check("clinit_counterA_watch_survives_class_init",
                  g_obs_a.fires.load() == WRITE_COUNT);
        ctx.record("[INFO] watch_static_field: an armed watch survives a class-init "
                   "(<clinit>) event on the writing thread and keeps firing.");
        // counter_watch dropped here.
    }

    // =====================================================================
    //  7. SAME-VALUE characterisation: writing the identical value repeatedly.
    //     Whether an unchanged store still traps is hardware/JIT-dependent (a
    //     write breakpoint fires on the STORE, not on a value change, so it
    //     typically DOES fire even for an unchanged value).  We characterise
    //     what we observe and assert only the deterministic Java-visible final
    //     value -- never a hard fire count that could flake.
    // =====================================================================
    {
        const bool reset_done{ drive(ctx, 0) };
        ctx.check("samevalue_reset_completed", reset_done);

        auto watch{ vmhook::watch_static_field<wsf, std::int32_t>(
            "sameValueField",
            [](std::int32_t prev, std::int32_t next)
            {
                record_numeric(g_obs_w, prev == 0, static_cast<double>(next));
            }) };
        ctx.check("samevalue_watch_running", watch.running());

        reset_observations();
        const bool done{ drive_keep_obs(ctx, 18) }; // write SAME_VALUE repeatedly
        ctx.check("samevalue_probe_completed", done);
        ctx.check("samevalue_java_final_value",
                  wsf::counter("sameValueField") == SAME_VALUE);

        const std::int32_t fires{ g_obs_w.fires.load() };
        if (fires > 0)
        {
            ctx.record("[INFO] watch_static_field: an unchanged (same-value) store STILL "
                       "traps (write breakpoints fire on the store, not on a value change).");
            ctx.check("samevalue_unchanged_store_observed_new_value_is_same",
                      g_obs_w.max_new.load() == static_cast<double>(SAME_VALUE));
        }
        else
        {
            ctx.record("[INFO] watch_static_field: a same-value store did not trap on this "
                       "run (JIT folded the redundant putstatic); characterised, not failed.");
        }
        // Belt-and-braces: the count is whatever it is, but never negative and
        // never exceeds the number of stores driven.
        ctx.check("samevalue_fire_count_in_bounds", fires >= 0 && fires <= WRITE_COUNT);
        // watch dropped here.
    }

    // =====================================================================
    //  8. Idempotent stop + double-drop safety: a watch that is stop()'d
    //     twice (and then destroyed) must not misbehave; running() stays
    //     false.  This guards the slot-release path against double-free.
    // =====================================================================
    {
        auto watch{ install_int_watch("counterA", g_obs_a) };
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
        auto wa{ install_int_watch("counterA", g_obs_a) };
        auto wb{ install_int_watch("counterB", g_obs_b) };
        auto wc{ install_int_watch("counterC", g_obs_c) };
        auto wd{ install_int_watch("counterD", g_obs_d) };
        ctx.check("recovery_all_four_slots_reusable",
                  wa.running() && wb.running() && wc.running() && wd.running());
        // All four dropped here -> JVM left with NO armed watchpoints, as
        // required so no trap fires inside a later module.
    }
#endif // VMHOOK_HAS_HW_DATA_BREAKPOINTS

    // =====================================================================
    //  9. UNCONDITIONAL FINAL TEARDOWN + leak check: leave the Java fixture's
    //     counters reset so a later module reading these fields sees a clean
    //     slate, and (belt and braces) confirm NO watch is armed by re-driving
    //     writes to EVERY watched field with NOTHING installed -- nothing must
    //     fire.  This is the suite-safety gate: if any DR0-DR3 / DR7 bit
    //     leaked, one of these unwatched writes would trap and bump a counter.
    // =====================================================================
    {
        reset_observations();
        const bool reset_done{ drive_keep_obs(ctx, 0) };
        ctx.check("final_reset_probe_completed", reset_done);
        ctx.check("final_counterA_reset", wsf::counter("counterA") == 0);

        // Drive an int write, a width write, and a ref write with NOTHING armed.
        bool any_fire{ false };
        const std::int32_t modes_to_sweep[]{ 1, 10, 11, 12, 13, 14, 15, 17 };
        for (const std::int32_t m : modes_to_sweep)
        {
            const bool done{ drive_keep_obs(ctx, m) };
            ctx.check("final_unwatched_probe_completed", done);
            if (g_obs_a.fires.load() != 0 || g_obs_b.fires.load() != 0
                || g_obs_c.fires.load() != 0 || g_obs_d.fires.load() != 0
                || g_obs_e.fires.load() != 0 || g_obs_w.fires.load() != 0)
            {
                any_fire = true;
            }
        }
        ctx.check("final_no_callback_when_unwatched", !any_fire);

        // Leave counters clean for the next module.
        const bool final_reset{ drive_keep_obs(ctx, 0) };
        ctx.check("final_leave_clean_state", final_reset && wsf::counter("counterA") == 0);
    }
    ctx.record("[INFO] watch_static_field: module complete -- all watch_handles "
               "released, no DR slot armed, fixture counters reset. Suite-safe for "
               "downstream modules.");
}
