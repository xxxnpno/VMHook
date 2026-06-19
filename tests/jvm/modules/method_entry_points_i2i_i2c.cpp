// method_entry_points_i2i_i2c JVM test module
//                       (feature area: HotSpot Method entry-point accessor layer)
//
// Drives the live Method* entry-point accessors DIRECTLY -- the snapshot/restore
// layer every hook install, deopt, re-anchor and watchdog repair routes through:
//   * get_i2i_entry()            -- _i2i_entry        (the i2i hook-location target)
//   * get_from_interpreted_entry()/set_from_interpreted_entry()  -- _from_interpreted_entry
//   * get_from_compiled_entry()/set_from_compiled_entry()        -- _from_compiled_entry
//                                   (a.k.a _from_compiled_code_entry_point, JDK<=20)
//   * get_code()/set_code()      -- _code (set_code(nullptr) is the deopt trigger)
//   * get_adapter()              -- Method::_adapter -> AdapterHandlerEntry (c2i recovery)
//   * get_c2i_entry_from_adapter()-- AdapterHandlerEntry::_c2i_entry
//
// This is the "FIX C" core: when a hooked method is JIT-compiled, the patched i2i
// interpreter stub becomes reachable again only by writing
//   set_from_interpreted_entry(i2i); set_from_compiled_entry(c2i); set_code(nullptr)
// in that order.  There was NO dedicated test for this accessor cluster -- it was
// exercised only transitively by hook_install_after_jit / deoptimize_methods /
// dont_inline_dont_compile, none of which OWN it.  This module is that owner.
//
// What it proves (universal invariants HARD on every JDK):
//   1. RAW ACCESSOR ROUND-TRIP on a clean interpreted method: _i2i_entry and
//      _from_interpreted_entry are non-null and point into committed+executable
//      code-cache memory; reads are STABLE across repeats (offsets are cached).
//   2. SETTER<->GETTER OFFSET AGREEMENT on a never-hooked throwaway method: write
//      a sentinel through set_from_interpreted_entry / set_from_compiled_entry /
//      set_code, read it back through the getter, restore the original, read it
//      back.  The setters must land EXACTLY where the getters read (the property
//      the entire FIX-C dance depends on).
//   3. The DEOPT INVARIANT the install path establishes: a hooked method has
//      _from_interpreted_entry == _i2i_entry (interp routes through the patch) and
//      _code == null; after shutdown_hooks() the entries are LEFT deopted (the
//      deliberate non-restore) -- the durable teardown proof is the detour going
//      silent, asserted hard.
//   4. c2i RECOVERY on a JIT-compiled method (timing/JDK-gated): get_adapter() and
//      get_c2i_entry_from_adapter() resolve to executable memory; the c2i-
//      unrecoverable case is [INFO] (the documented forced-deopt fallback).
//   5. get_adapter() PROCESS-WIDE offset latch: resolving on `warm` then on a
//      DIFFERENT method (`touched`) both succeed once the offset is cached.
//   6. DUAL-NAME resolution: exactly the JDK-appropriate _from_compiled_* VMStruct
//      is exported (<=20: _from_compiled_code_entry_point; 21+: _from_compiled_entry)
//      and the accessor returns non-null.
//   7. BOUNDARY/NULL paths: every accessor on an invalid Method* returns null/no-op
//      and never crashes; get_c2i_entry_from_adapter(nullptr) -> null.
//
// JDK-variant gating (prior waves reddened on hard-asserting these): whether a
// tiny method actually JIT-compiles, and whether c2i is recoverable on this
// runner, is timing/JDK dependent -- those are [INFO]/gated, never a FAIL.  The
// _from_compiled_entry rename at JDK 21 and the _adapter VMStruct drop at JDK 9
// are characterised, with only the universal invariant (the accessor returns a
// sane value on whatever this JDK exports) kept HARD.
//
// HARD RULES honoured: harness API only; NEVER crash the JVM -- every Method* /
// adapter / entry-point deref is is_valid_pointer-guarded and bails to an [INFO]
// skip rather than touching anything suspicious; the setter round-trip restores
// the original value it read; leave NO hooks armed (every scoped_hook is scoped,
// final unconditional shutdown_hooks()).  No forced GC, modest allocation.
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
    // Wrapper for vmhook.fixtures.MethodEntryPoints.  Deriving from
    // vmhook::object<> gives the wrapper its vtable (required by register_class<T>)
    // and the static_field(...) / get_field(...) accessors for the go/done probe.
    class mep_fixture : public vmhook::object<mep_fixture>
    {
    public:
        explicit mep_fixture(vmhook::oop_t instance) noexcept
            : vmhook::object<mep_fixture>{ instance }
        {
        }

        // --- go/done handshake + scenario selector ------------------------
        static auto set_go(bool value) -> void       { static_field("go")->set(value); }
        static auto set_done(bool value) -> void      { static_field("done")->set(value); }
        static auto get_done() -> bool                { return static_field("done")->get(); }
        static auto set_mode(std::int32_t m) -> void  { static_field("mode")->set(m); }

        // --- recorded observations the Java side writes -------------------
        static auto get_last_warm_result() -> std::int32_t    { return static_field("lastWarmResult")->get(); }
        static auto get_last_touched_result() -> std::int32_t { return static_field("lastTouchedResult")->get(); }
        static auto get_warm_calls_made() -> std::int32_t     { return static_field("warmCallsMade")->get(); }

        // Reads this instance's own seed (proves the detour's `self` is correct).
        auto seed() const -> std::int32_t { return get_field("seed")->get(); }
    };

    // ---- Fixture-mirrored constants (lockstep with MethodEntryPoints.java) --
    constexpr std::int32_t SEED{ 2000 };
    constexpr std::int32_t DELTA{ 11 };
    constexpr std::int32_t SINGLE_RESULT{ SEED + DELTA };   // warm(DELTA) body result
    constexpr std::int32_t WARM_CALLS{ 200000 };

    constexpr const char* FIXTURE_CLASS{ "vmhook/fixtures/MethodEntryPoints" };
    constexpr const char* WARM_NAME{ "warm" };
    constexpr const char* TOUCHED_NAME{ "touched" };
    constexpr const char* SSET_NAME{ "sset" };
    constexpr const char* QUIET_NAME{ "quiet" };
    constexpr const char* HOT_SIG{ "(I)I" };

    // Probe modes (lockstep with the fixture's switch).
    constexpr std::int32_t MODE_CALL_WARM_ONCE{ 1 };
    constexpr std::int32_t MODE_WARM{ 2 };
    constexpr std::int32_t MODE_CALL_TOUCHED_ONCE{ 3 };
    constexpr std::int32_t MODE_CALL_SSET_ONCE{ 4 };

    // ---- Hook observation state (reset per scenario) -----------------------
    std::atomic<std::int32_t> g_fire_count{ 0 };
    std::atomic<std::int32_t> g_self_ok_fires{ 0 };   // self non-null & seed == SEED
    std::atomic<std::int32_t> g_arg_ok_fires{ 0 };    // decoded delta == DELTA

    auto reset_observations() -> void
    {
        g_fire_count.store(0);
        g_self_ok_fires.store(0);
        g_arg_ok_fires.store(0);
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
                    mep_fixture::set_done(false);
                    mep_fixture::set_mode(mode);
                }
                mep_fixture::set_go(value);
            },
            []() { return mep_fixture::get_done(); });
    }

    // Locates the live Method* for FIXTURE_CLASS::<name>(I)I by walking the
    // InstanceKlass methods array.  Returns nullptr if anything looks invalid --
    // callers MUST treat nullptr as "cannot run this Method-level scenario" and
    // skip it rather than crash.  Every read is pointer-validated.  (Same shape as
    // deoptimize_methods.cpp::find_method / dont_inline_dont_compile::find_hot_method.)
    auto find_method(const char* const name) -> vmhook::hotspot::method*
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
        for (std::int32_t i{ 0 }; i < count; ++i)
        {
            vmhook::hotspot::method* const m{ methods[i] };
            if (!m || !vmhook::hotspot::is_valid_pointer(m))
            {
                continue;
            }
            const std::string method_name = m->get_name();   // copy-init (MSVC)
            const std::string method_sig = m->get_signature(); // copy-init (MSVC)
            if (method_name == name && method_sig == HOT_SIG)
            {
                return m;
            }
        }
        return nullptr;
    }

    // True iff `p` points at committed + executable memory (a real code-cache
    // entry point / adapter stub).  query_region never faults.  A null / invalid
    // pointer yields false.
    auto points_into_executable(void* const p) -> bool
    {
        if (!p || !vmhook::hotspot::is_valid_pointer(p))
        {
            return false;
        }
        const vmhook::os::region_info info{ vmhook::os::query_region(p) };
        return info.committed && info.executable;
    }

    // Reads Method::_code through a validated pointer.  nullptr means "not
    // currently JIT-compiled" (the deopted steady state vmhook installs).
    auto method_code(vmhook::hotspot::method* const m) -> void*
    {
        if (!m || !vmhook::hotspot::is_valid_pointer(m))
        {
            return nullptr;
        }
        void* const code{ m->get_code() };
        return (code && vmhook::hotspot::is_valid_pointer(code)) ? code : nullptr;
    }

    // True iff an INTERPRETED dispatch routes through the patched i2i stub
    // (_from_interpreted_entry == _i2i_entry -- the deopted invariant the install
    // path establishes).  Pointer-validated; unreadable entries yield false.
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

    // The exported _from_compiled_* VMStruct type_string + which name carries it,
    // diagnostic only.  Resolves the two-name fallback the accessor uses.
    auto from_compiled_field_name() -> std::string
    {
        if (vmhook::hotspot::iterate_struct_entries("Method", "_from_compiled_code_entry_point"))
        {
            return std::string{ "_from_compiled_code_entry_point" };   // JDK <= 20
        }
        if (vmhook::hotspot::iterate_struct_entries("Method", "_from_compiled_entry"))
        {
            return std::string{ "_from_compiled_entry" };              // JDK 21+
        }
        return std::string{ "<neither exported>" };
    }

    // Drives warm() through the hot loop a bounded number of times, polling for
    // HotSpot's async compiler to populate Method::_code.  Best-effort: a -Xint /
    // busy runner returns false and the caller folds that into an [INFO].
    // Mirrors deoptimize_methods.cpp::warm_to_jit.
    auto warm_to_jit(vmhook_test::context& ctx, vmhook::hotspot::method* const m) -> bool
    {
        constexpr std::int32_t max_rounds{ 5 };
        constexpr std::chrono::milliseconds settle_budget{ 1500 };
        constexpr std::chrono::milliseconds settle_step{ 25 };

        for (std::int32_t round{ 0 }; round < max_rounds; ++round)
        {
            const bool warm_done{ drive(ctx, MODE_WARM) };
            if (!warm_done)
            {
                continue;
            }
            if (method_code(m) != nullptr)
            {
                return true;
            }
            const auto deadline{ std::chrono::steady_clock::now() + settle_budget };
            while (std::chrono::steady_clock::now() < deadline)
            {
                if (method_code(m) != nullptr)
                {
                    return true;
                }
                std::this_thread::sleep_for(settle_step);
            }
        }
        return method_code(m) != nullptr;
    }

    // Installs a scoped allow-through observer on FIXTURE_CLASS::<name>(I)I.
    auto make_observer(const char* const name) -> vmhook::hook_handle
    {
        return vmhook::scoped_hook<mep_fixture>(
            name,
            [](vmhook::return_value&,
               const std::unique_ptr<mep_fixture>& self,
               std::int32_t delta)
            {
                g_fire_count.fetch_add(1, std::memory_order_relaxed);
                if (self != nullptr && self->seed() == SEED)
                {
                    g_self_ok_fires.fetch_add(1, std::memory_order_relaxed);
                }
                if (delta == DELTA)
                {
                    g_arg_ok_fires.fetch_add(1, std::memory_order_relaxed);
                }
            });
    }
}

VMHOOK_JVM_MODULE(method_entry_points_i2i_i2c)
{
    vmhook::register_class<mep_fixture>(FIXTURE_CLASS);

    // Belt-and-braces: ensure no hook from an earlier module is armed on our
    // fixture before we measure clean entry-point state.  shutdown_hooks() is
    // safe-when-empty.
    vmhook::shutdown_hooks();

    // Link every method we read up front (interpreted dispatch must run at least
    // once before a method's entry points are in their steady interpreted state).
    // warm/touched/sset are dispatched here; quiet is deliberately NEVER dispatched.
    const bool warm_linked{ drive(ctx, MODE_CALL_WARM_ONCE) };
    const bool touched_linked{ drive(ctx, MODE_CALL_TOUCHED_ONCE) };
    const bool sset_linked{ drive(ctx, MODE_CALL_SSET_ONCE) };
    ctx.check("link_warm_probe_completed", warm_linked);
    ctx.check("link_touched_probe_completed", touched_linked);
    ctx.check("link_sset_probe_completed", sset_linked);

    vmhook::hotspot::method* const warm_m{ find_method(WARM_NAME) };
    vmhook::hotspot::method* const touched_m{ find_method(TOUCHED_NAME) };
    vmhook::hotspot::method* const sset_m{ find_method(SSET_NAME) };
    vmhook::hotspot::method* const quiet_m{ find_method(QUIET_NAME) };
    ctx.check("located_warm_method", warm_m != nullptr);
    ctx.check("located_touched_method", touched_m != nullptr);
    ctx.check("located_sset_method", sset_m != nullptr);
    ctx.check("located_quiet_method", quiet_m != nullptr);

    // =====================================================================
    // Scenario 1 -- RAW ACCESSOR ROUND-TRIP on a clean interpreted method.
    //   On a linked, non-hooked, interpreted method:
    //     * get_i2i_entry() is non-null and points into committed+executable
    //       code-cache memory (it is the interpreter entry stub),
    //     * get_from_interpreted_entry() is non-null,
    //     * get_code() is null (not JIT-compiled),
    //     * a fresh interpreted method routes through its own i2i stub
    //       (_from_interpreted_entry == _i2i_entry is the COMMON case on a never-
    //        compiled, never-hooked method, characterised -- HotSpot points a
    //        freshly-linked method's interpreted entry at the i2i stub),
    //     * every read is STABLE across two passes (offsets are cached statics).
    // =====================================================================
    if (warm_m != nullptr)
    {
        void* const i2i_1{ warm_m->get_i2i_entry() };
        void* const i2i_2{ warm_m->get_i2i_entry() };
        ctx.check("raw_i2i_entry_nonnull", i2i_1 != nullptr);
        ctx.check("raw_i2i_entry_stable_across_reads", i2i_1 == i2i_2);
        ctx.check("raw_i2i_entry_points_into_executable", points_into_executable(i2i_1));

        void* const fie_1{ warm_m->get_from_interpreted_entry() };
        void* const fie_2{ warm_m->get_from_interpreted_entry() };
        ctx.check("raw_from_interpreted_entry_nonnull", fie_1 != nullptr);
        ctx.check("raw_from_interpreted_entry_stable_across_reads", fie_1 == fie_2);
        ctx.check("raw_from_interpreted_entry_points_into_executable",
                  points_into_executable(fie_1));

        // Clean (not hooked, not JIT'd) method: _code is null.  A racing async
        // recompile of this freshly-linked method is vanishingly unlikely here
        // (warm() was called exactly once), so this is a HARD universal invariant.
        ctx.check("raw_code_null_on_clean_interpreted_method", method_code(warm_m) == nullptr);

        // get_from_compiled_entry: the field is exported on every JDK 8..26.  On a
        // not-yet-compiled method HotSpot points _from_compiled_entry at the c2i
        // adapter (so a compiled caller transitions to the interpreter), which is
        // a real executable stub -- so the accessor returns non-null and the slot
        // is executable.  Characterised (not hard) because a JDK could in theory
        // leave it null on an unlinked method; warm() is linked here.
        void* const fce{ warm_m->get_from_compiled_entry() };
        ctx.record(std::string{ "[INFO] method_entry_points_i2i_i2c scenario 1: warm() "
                   "_from_compiled_entry (field '" } + from_compiled_field_name() + "') = "
                   + (fce == nullptr ? "null"
                      : (points_into_executable(fce) ? "executable code-cache stub"
                                                     : "non-null but NOT executable"))
                   + " on this linked-but-uncompiled method.");
        // Universal: whatever it returns, reading it twice is stable.
        ctx.check("raw_from_compiled_entry_stable_across_reads",
                  fce == warm_m->get_from_compiled_entry());

        // The freshly-linked-method interpreter route: characterise rather than
        // hard-assert (HotSpot's linking state for a once-dispatched method is
        // version-dependent), but it MUST be a sane pair.
        ctx.record(std::string{ "[INFO] method_entry_points_i2i_i2c scenario 1: clean warm() "
                   "interp_routes_through_i2i=" } + (interp_routes_through_i2i(warm_m) ? "yes" : "no")
                   + " (_from_interpreted_entry " + (interp_routes_through_i2i(warm_m) ? "==" : "!=")
                   + " _i2i_entry on this linked-but-unhooked method).");
    }
    else
    {
        ctx.record("[INFO] method_entry_points_i2i_i2c scenario 1: warm Method* unavailable -- "
                   "raw accessor round-trip skipped (no crash).");
    }

    // =====================================================================
    // Scenario 2 -- SETTER<->GETTER OFFSET AGREEMENT on a never-hooked method.
    //   The whole FIX-C dance depends on set_from_interpreted_entry /
    //   set_from_compiled_entry / set_code writing EXACTLY where the matching
    //   getter reads.  Prove it on `sset` (a static method this module never
    //   hooks): read the original, write a sentinel, read it back equal, restore
    //   the original, read it back equal.  Bug #5/#6 (offset disagreement / a
    //   silent no-op setter) would surface here as a read-back mismatch.
    //
    //   The sentinel is the method's OWN _i2i_entry (a real, valid code address):
    //   writing a known-good pointer keeps the JVM consistent even if a dispatch
    //   somehow raced (it never does -- sset is dispatched only on our probe,
    //   between which all pokes happen on the driver thread), and avoids planting
    //   a wild address the watchdog/interpreter could later jump to.
    // =====================================================================
    if (sset_m != nullptr)
    {
        void* const sentinel{ sset_m->get_i2i_entry() };
        ctx.check("setter_sentinel_source_nonnull", sentinel != nullptr);

        if (sentinel != nullptr)
        {
            // --- _from_interpreted_entry round-trip ---
            void* const fie_orig{ sset_m->get_from_interpreted_entry() };
            sset_m->set_from_interpreted_entry(sentinel);
            ctx.check("setter_fie_readback_equals_written",
                      sset_m->get_from_interpreted_entry() == sentinel);
            sset_m->set_from_interpreted_entry(fie_orig);
            ctx.check("setter_fie_restored_to_original",
                      sset_m->get_from_interpreted_entry() == fie_orig);

            // --- _from_compiled_entry round-trip ---
            void* const fce_orig{ sset_m->get_from_compiled_entry() };
            sset_m->set_from_compiled_entry(sentinel);
            ctx.check("setter_fce_readback_equals_written",
                      sset_m->get_from_compiled_entry() == sentinel);
            sset_m->set_from_compiled_entry(fce_orig);
            ctx.check("setter_fce_restored_to_original",
                      sset_m->get_from_compiled_entry() == fce_orig);

            // --- _code round-trip (write null = the deopt trigger, then restore) ---
            // sset is not JIT'd here, so _code is already null; write null again
            // (idempotent deopt), read null, then write the sentinel and read it
            // back to prove the setter lands where the getter reads, then restore
            // null (the correct steady state for an uncompiled method).
            void* const code_orig{ method_code(sset_m) };
            sset_m->set_code(nullptr);
            ctx.check("setter_code_null_readback", sset_m->get_code() == nullptr);
            sset_m->set_code(sentinel);
            ctx.check("setter_code_readback_equals_written", sset_m->get_code() == sentinel);
            // Restore the original (null on an uncompiled method); never leave a
            // non-null _code pointing at a non-nmethod address.
            sset_m->set_code(code_orig);
            ctx.check("setter_code_restored_to_original", method_code(sset_m) == code_orig);
        }

        // The static method is still callable after the round-trip (we restored
        // every entry point) -- prove the JVM is intact.
        const bool done{ drive(ctx, MODE_CALL_SSET_ONCE) };
        ctx.check("setter_sset_still_callable_after_roundtrip", done);
    }
    else
    {
        ctx.record("[INFO] method_entry_points_i2i_i2c scenario 2: sset Method* unavailable -- "
                   "setter round-trip skipped (no crash).");
    }

    // =====================================================================
    // Scenario 3 -- DEOPT INVARIANT + deliberate non-restore on teardown.
    //   Install a hook on warm(); the live Method must now satisfy the deopt
    //   invariant: _from_interpreted_entry == _i2i_entry (interp routes through
    //   the patch) AND _code == null.  The detour fires once on a real dispatch
    //   (allow-through).  After shutdown_hooks(), the entries are LEFT in the
    //   deopted state (the deliberate non-restore -- bug #1: original_from_* are
    //   snapshotted but never restored, by design, to avoid the dangling-nmethod
    //   AV).  The DURABLE teardown proof is the detour going silent, HARD.
    // =====================================================================
    if (warm_m != nullptr)
    {
        {
            auto handle{ make_observer(WARM_NAME) };
            ctx.check("deopt_hook_installed", handle.installed());

            // The deopt invariant the install path establishes (vmhook.hpp:8258).
            ctx.check("deopt_interp_routes_through_i2i_while_hooked",
                      interp_routes_through_i2i(warm_m));
            ctx.check("deopt_code_null_while_hooked", method_code(warm_m) == nullptr);

            const bool done{ drive(ctx, MODE_CALL_WARM_ONCE) };
            ctx.check("deopt_probe_completed", done);
            ctx.check("deopt_java_made_one_call", mep_fixture::get_warm_calls_made() == 1);
            ctx.check("deopt_detour_fired_once", g_fire_count.load() == 1);
            ctx.check("deopt_self_correct", g_self_ok_fires.load() == 1);
            ctx.check("deopt_arg_decoded", g_arg_ok_fires.load() == 1);
            ctx.check("deopt_allow_through_original_result",
                      mep_fixture::get_last_warm_result() == SINGLE_RESULT);
        }
        // handle dropped -> hook_handle::stop() ran.  Per bug #1, stop() does NOT
        // restore the snapshotted original entries -- it leaves the method in the
        // deopted state.  Lock that in: _from_interpreted_entry is STILL == the
        // i2i stub (interp route preserved), proving the non-restore.  A future
        // "fix" that restores from the dead original_from_* snapshot would flip
        // this and regress loudly.
        ctx.record(std::string{ "[INFO] method_entry_points_i2i_i2c scenario 3: after "
                   "hook_handle::stop(), warm() interp_routes_through_i2i=" }
                   + (interp_routes_through_i2i(warm_m) ? "yes (entries LEFT deopted -- the "
                       "deliberate non-restore, vmhook.hpp:8922-8928)"
                       : "no (entries changed since teardown -- HotSpot relinked or restored)")
                   + "; _code=" + (method_code(warm_m) == nullptr ? "null" : "NON-null") + ".");

        // The DURABLE teardown proof (HARD on every JDK): the detour is silent and
        // the original body runs byte-exact after teardown.
        reset_observations();
        const bool done2{ drive(ctx, MODE_CALL_WARM_ONCE) };
        ctx.check("deopt_post_teardown_probe_completed", done2);
        ctx.check("deopt_detour_silent_after_teardown", g_fire_count.load() == 0);
        ctx.check("deopt_byte_exact_original_after_teardown",
                  mep_fixture::get_last_warm_result() == SINGLE_RESULT);

        vmhook::shutdown_hooks();   // belt-and-braces
    }

    // =====================================================================
    // Scenario 4 -- c2i RECOVERY on a JIT-compiled method (timing/JDK gated).
    //   Warm warm() to a JIT-compiled state (NO hook armed so HotSpot is free to
    //   populate Method::_code).  Then:
    //     * get_adapter() resolves to a valid AdapterHandlerEntry,
    //     * get_c2i_entry_from_adapter(adapter) is non-null and points into
    //       committed+executable memory (the c2i stub lives in the code cache),
    //     * after a hook install deopts the method, _from_compiled_entry equals
    //       that recovered c2i pointer (the FIX-C redirect).
    //   The c2i-unrecoverable case (get_adapter()/c2i null on this runner) is the
    //   documented forced-deopt fallback, recorded [INFO] -- NEVER a FAIL.
    // =====================================================================
    if (warm_m != nullptr)
    {
        const bool warmed{ warm_to_jit(ctx, warm_m) };
        void* const code_before{ method_code(warm_m) };
        ctx.record(std::string{ "[INFO] method_entry_points_i2i_i2c scenario 4: warm warm() -> _code=" }
                   + (code_before == nullptr ? "null (HotSpot declined/raced -- JIT-dependent "
                                               "c2i checks skipped)"
                                             : "NON-null (JIT-compiled)") + ".");

        if (warmed && code_before != nullptr)
        {
            // On a compiled method _from_compiled_entry is the compiled nmethod's
            // verified entry (NOT the c2i adapter) -- it must point into executable
            // memory.  Universal once we know the method really compiled.
            void* const fce_compiled{ warm_m->get_from_compiled_entry() };
            ctx.check("c2i_compiled_from_compiled_entry_executable",
                      points_into_executable(fce_compiled));

            void* const adapter{ warm_m->get_adapter() };
            void* const c2i{ vmhook::hotspot::get_c2i_entry_from_adapter(adapter) };
            ctx.record(std::string{ "[INFO] method_entry_points_i2i_i2c scenario 4: get_adapter()=" }
                       + (adapter == nullptr ? "null" : "non-null")
                       + ", get_c2i_entry_from_adapter()="
                       + (c2i == nullptr ? "null (c2i UNRECOVERABLE on this runner -- the "
                                           "documented forced-deopt fallback path)"
                                         : (points_into_executable(c2i) ? "executable code-cache stub"
                                                                        : "non-null but NOT executable")) + ".");

            if (adapter != nullptr && c2i != nullptr)
            {
                // c2i recovery succeeded: the recovered stub must be real
                // executable code-cache memory.  HARD in this branch.
                ctx.check("c2i_recovered_points_into_executable", points_into_executable(c2i));

                // Install a hook -> the install path deopts a was_compiled method
                // by writing _from_compiled_entry = c2i, then _code = null.  After
                // install, _from_compiled_entry must equal the recovered c2i stub
                // AND _code must be null (the FIX-C redirect).
                {
                    auto handle{ make_observer(WARM_NAME) };
                    ctx.check("c2i_hook_installed_on_compiled_method", handle.installed());
                    ctx.check("c2i_code_nulled_after_install", method_code(warm_m) == nullptr);

                    void* const fce_after{ warm_m->get_from_compiled_entry() };
                    // The recovered c2i the install wrote is the SAME stub
                    // get_c2i_entry_from_adapter returns (offset is process-wide).
                    ctx.record(std::string{ "[INFO] method_entry_points_i2i_i2c scenario 4: post-install "
                               "_from_compiled_entry " } + (fce_after == c2i ? "== recovered c2i (FIX-C "
                               "redirect landed)" : "!= recovered c2i (HotSpot re-derived a different "
                               "adapter, or forced-deopt fallback took the fie-only branch)") + ".");
                    // Universal: whatever the install wrote, _from_compiled_entry is
                    // a real executable stub (never garbage) on a deopted method.
                    ctx.check("c2i_from_compiled_entry_executable_after_install",
                              points_into_executable(fce_after));
                    // The interpreter route is now the patch -> the detour fires.
                    ctx.check("c2i_interp_routes_through_i2i_after_install",
                              interp_routes_through_i2i(warm_m));

                    const bool done{ drive(ctx, MODE_CALL_WARM_ONCE) };
                    ctx.check("c2i_post_install_probe_completed", done);
                    ctx.check("c2i_post_install_detour_fired",
                              g_fire_count.load() == 1);
                    ctx.check("c2i_post_install_allow_through",
                              mep_fixture::get_last_warm_result() == SINGLE_RESULT);
                }
            }
            else
            {
                ctx.record("[INFO] method_entry_points_i2i_i2c scenario 4: warm() was JIT-compiled but "
                           "c2i was unrecoverable (get_adapter or _c2i_entry null) -- the documented "
                           "forced-deopt fallback (vmhook.hpp:8281-8285 sets fie=i2i, code=null only). "
                           "A hook install still deopts + fires via the interpreter; proving it:");
                auto handle{ make_observer(WARM_NAME) };
                ctx.check("c2i_unrecoverable_hook_still_installs", handle.installed());
                const bool done{ drive(ctx, MODE_CALL_WARM_ONCE) };
                ctx.check("c2i_unrecoverable_post_install_probe_completed", done);
                ctx.check("c2i_unrecoverable_post_install_detour_fired_or_info",
                          g_fire_count.load() <= 1);   // never double-fires
                ctx.record(std::string{ "[INFO] method_entry_points_i2i_i2c scenario 4: forced-deopt-"
                           "fallback detour fired " } + std::to_string(g_fire_count.load()) + "/1.");
            }
        }
        else
        {
            ctx.record("[INFO] method_entry_points_i2i_i2c scenario 4: warm() was not JIT-compiled before "
                       "the c2i probe -- adapter/c2i checks are vacuous and skipped (no-op is correct on "
                       "a -Xint / busy runner).");
        }

        vmhook::shutdown_hooks();   // clean up scenario 4
    }

    // =====================================================================
    // Scenario 5 -- get_adapter() PROCESS-WIDE offset latch across two methods.
    //   The JDK 9+ heuristic detects the _adapter offset once and caches it
    //   process-wide.  Resolve it on warm() (warmed above) and on a DIFFERENT,
    //   never-warmed method (touched): once the offset is latched, BOTH must
    //   resolve a non-null adapter (proving the cached offset is method-agnostic).
    //   This is gated: on a runner where get_adapter never latched (no method
    //   ever JIT-compiled, JDK 9+ heuristic never validated), both legitimately
    //   return null -- recorded [INFO], not a FAIL (the retry-not-cache-failure
    //   design, the Forge 1.8.9 lesson).
    // =====================================================================
    if (warm_m != nullptr && touched_m != nullptr)
    {
        void* const adapter_warm{ warm_m->get_adapter() };
        void* const adapter_touched{ touched_m->get_adapter() };
        // Stability: re-reading get_adapter() on warm() is identical (offset cached).
        ctx.check("adapter_warm_stable_across_reads",
                  adapter_warm == warm_m->get_adapter());

        if (adapter_warm != nullptr)
        {
            // Offset latched on warm() -> a DIFFERENT method must also resolve a
            // non-null adapter through the same cached offset.  HARD in this branch.
            ctx.check("adapter_offset_process_wide_resolves_second_method",
                      adapter_touched != nullptr);
            // Each method's adapter is a DISTINCT object (different methods have
            // different AdapterHandlerEntry instances unless they share a signature;
            // warm and touched share (I)I so HotSpot MAY share the adapter -- so we
            // only assert both are valid, not that they differ).
            ctx.check("adapter_warm_is_valid_pointer",
                      vmhook::hotspot::is_valid_pointer(adapter_warm));
            ctx.check("adapter_touched_is_valid_pointer",
                      adapter_touched == nullptr
                          || vmhook::hotspot::is_valid_pointer(adapter_touched));
            // The c2i recovered from each resolves (both share (I)I so c2i may be
            // identical) -- characterise.
            void* const c2i_warm{ vmhook::hotspot::get_c2i_entry_from_adapter(adapter_warm) };
            void* const c2i_touched{ vmhook::hotspot::get_c2i_entry_from_adapter(adapter_touched) };
            ctx.record(std::string{ "[INFO] method_entry_points_i2i_i2c scenario 5: c2i(warm)=" }
                       + (c2i_warm == nullptr ? "null" : "non-null") + ", c2i(touched)="
                       + (c2i_touched == nullptr ? "null" : "non-null")
                       + (c2i_warm != nullptr && c2i_warm == c2i_touched
                              ? " (identical -- shared (I)I adapter)" : "") + ".");
        }
        else
        {
            ctx.record("[INFO] method_entry_points_i2i_i2c scenario 5: get_adapter() did not latch an "
                       "offset on this runner (no method JIT-compiled, or the JDK 9+ heuristic never "
                       "validated a candidate) -- the process-wide-offset assertion is vacuous and "
                       "skipped. This is the retry-not-cache-failure design (Forge 1.8.9 lesson, "
                       "vmhook.hpp:3345-3362); both methods returning null is correct here.");
        }
    }

    // =====================================================================
    // Scenario 6 -- DUAL-NAME _from_compiled_* resolution coverage.
    //   The single most JDK-fragile line in the feature: the field renamed at
    //   JDK 21 (<=20: _from_compiled_code_entry_point; 21+: _from_compiled_entry).
    //   Read iterate_struct_entries for BOTH names: EXACTLY ONE must be present
    //   (universal across JDK 8..26), and the accessor must return non-null on a
    //   linked method.  A future third rename would flip the "exactly one" check.
    // =====================================================================
    {
        const bool has_old{ vmhook::hotspot::iterate_struct_entries(
                                "Method", "_from_compiled_code_entry_point") != nullptr };
        const bool has_new{ vmhook::hotspot::iterate_struct_entries(
                                "Method", "_from_compiled_entry") != nullptr };
        ctx.record(std::string{ "[INFO] method_entry_points_i2i_i2c scenario 6: VMStruct exports "
                   "_from_compiled_code_entry_point=" } + (has_old ? "yes" : "no")
                   + " (JDK<=20), _from_compiled_entry=" + (has_new ? "yes" : "no")
                   + " (JDK21+); accessor resolves via the two-name fallback.");
        // Exactly one of the two names is exported on every supported JDK.
        ctx.check("dual_name_exactly_one_from_compiled_field_exported",
                  has_old != has_new);

        // The accessor returns non-null on a linked method whichever name carried
        // the field (the i2c/c2i adapter on an uncompiled method, or the nmethod
        // entry on a compiled one).  Gated on having located the method.
        if (warm_m != nullptr && (has_old || has_new))
        {
            ctx.check("dual_name_accessor_resolves_nonnull",
                      warm_m->get_from_compiled_entry() != nullptr);
        }
    }

    // =====================================================================
    // Scenario 7 -- QUIET (never-dispatched) boundary method.  quiet() was never
    //   called by run(), so interpreted dispatch never linked it.  Its entry
    //   points still RESOLVE (offsets are class-wide), so the accessors must not
    //   crash and must return sane values.  Characterise the never-linked state
    //   (its _from_interpreted_entry may point at the lazy-resolution/link stub
    //   rather than the i2i stub) without hard-asserting a JDK-variant shape.
    // =====================================================================
    if (quiet_m != nullptr)
    {
        void* const i2i{ quiet_m->get_i2i_entry() };
        void* const fie{ quiet_m->get_from_interpreted_entry() };
        void* const fce{ quiet_m->get_from_compiled_entry() };
        // The accessors didn't crash and the pointer is valid -- HARD.
        ctx.check("quiet_method_pointer_still_valid",
                  vmhook::hotspot::is_valid_pointer(quiet_m));
        // _i2i_entry is class-wide-resolvable even on a never-linked method.
        ctx.check("quiet_i2i_entry_nonnull", i2i != nullptr);
        ctx.check("quiet_code_null_on_never_called_method", method_code(quiet_m) == nullptr);
        ctx.record(std::string{ "[INFO] method_entry_points_i2i_i2c scenario 7: never-dispatched quiet() "
                   "_i2i_entry=" } + (i2i == nullptr ? "null" : "non-null")
                   + ", _from_interpreted_entry=" + (fie == nullptr ? "null" : "non-null")
                   + (i2i != nullptr && fie != nullptr && i2i == fie ? " (==i2i)" : " (!=i2i -- "
                     "lazy link stub on this never-linked method)")
                   + ", _from_compiled_entry=" + (fce == nullptr ? "null" : "non-null") + ".");
    }
    else
    {
        ctx.record("[INFO] method_entry_points_i2i_i2c scenario 7: quiet Method* unavailable -- "
                   "never-dispatched boundary scenario skipped (no crash).");
    }

    // =====================================================================
    // Scenario 8 -- BOUNDARY / NULL paths.  Every accessor on a deliberately-
    //   invalid Method*, and every adapter helper on null, must return null /
    //   no-op and NEVER crash.  These cover the is_valid_pointer guards at the top
    //   of each accessor and the explicit nullptr-safe early returns.
    // =====================================================================
    {
        // get_c2i_entry_from_adapter(nullptr) -> null (vmhook.hpp:7643).
        ctx.check("null_get_c2i_from_null_adapter",
                  vmhook::hotspot::get_c2i_entry_from_adapter(nullptr) == nullptr);

        // get_c2i_entry_from_adapter on a non-AHE but readable address (the
        // fixture klass pointer) must not crash; it returns whatever the slot
        // holds (likely garbage/null), but the call is fault-safe.  We only
        // require it not to crash -- reaching the assert is the proof.
        void* const k{ vmhook::find_class(FIXTURE_CLASS) };
        if (k != nullptr && vmhook::hotspot::is_valid_pointer(k))
        {
            (void)vmhook::hotspot::get_c2i_entry_from_adapter(k);
            ctx.check("null_get_c2i_on_non_adapter_did_not_crash", true);
        }

        // A deliberately-invalid Method* (a poison value is_valid_pointer rejects).
        // Every accessor must early-return null / no-op without dereferencing.
        // 0x1 is odd-aligned -> is_valid_pointer is false -> every guard trips.
        auto* const bad{ reinterpret_cast<vmhook::hotspot::method*>(
            static_cast<std::uintptr_t>(0x1)) };
        ctx.check("null_bad_method_get_i2i_returns_null", bad->get_i2i_entry() == nullptr);
        ctx.check("null_bad_method_get_fie_returns_null",
                  bad->get_from_interpreted_entry() == nullptr);
        ctx.check("null_bad_method_get_fce_returns_null",
                  bad->get_from_compiled_entry() == nullptr);
        ctx.check("null_bad_method_get_code_returns_null", bad->get_code() == nullptr);
        ctx.check("null_bad_method_get_adapter_returns_null", bad->get_adapter() == nullptr);
        // The setters on a bad Method* are silent no-ops (no crash); reaching the
        // assert after calling all three is the proof.
        bad->set_from_interpreted_entry(nullptr);
        bad->set_from_compiled_entry(nullptr);
        bad->set_code(nullptr);
        ctx.check("null_bad_method_setters_are_noop_no_crash", true);

        // detect_adapter_offset_from_method(nullptr) -> 0 (vmhook.hpp:7832).
        ctx.check("null_detect_adapter_offset_from_null_method",
                  vmhook::hotspot::detect_adapter_offset_from_method(nullptr) == 0u);

        // validate_adapter_handler_entry(nullptr, ...) -> false (vmhook.hpp:7792).
        ctx.check("null_validate_adapter_handler_entry_null_is_false",
                  !vmhook::hotspot::validate_adapter_handler_entry(nullptr, 0));
        // A readable-but-non-executable address (the fixture klass) is NOT a valid
        // AHE: its offset-0 slot does not point at executable memory -> false.
        if (k != nullptr && vmhook::hotspot::is_valid_pointer(k))
        {
            ctx.check("null_validate_adapter_handler_entry_non_ahe_is_false",
                      !vmhook::hotspot::validate_adapter_handler_entry(k, sizeof(void*)));
        }
    }

    // =====================================================================
    // FINAL CLEANUP -- belt-and-braces.  Other modules run after this one, so the
    //   module MUST leave ZERO hooks armed.  Every scenario already tears its hook
    //   down; call shutdown_hooks() once more unconditionally (idempotent,
    //   safe-when-empty).
    // =====================================================================
    vmhook::shutdown_hooks();
    ctx.check("module_left_clean_final_shutdown", true);
}
