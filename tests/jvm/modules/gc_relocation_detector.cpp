// gc_relocation_detector JVM test module -- area: gc / jni.global_ref.
//
// FEATURE: the GC RELOCATION DETECTOR.
//
//   * vmhook::vm_capabilities()  -- Layer 0.  Collector, barrier shape,
//     UseCompressedOops, UseCompactObjectHeaders, and the single `supported`
//     gate the detector consults, all derived from a gHotSpotVMStructs /
//     gHotSpotVMTypes + JVM-flag-table walk done entirely through fault-safe
//     reads.
//   * vmhook::gc_epoch() / gc_epoch_changed()  -- Layer 1.  A PAIRED sample of
//     CollectedHeap::_total_collections and the gc-active flag (_is_gc_active up
//     to JDK 20, _is_stw_gc_active from 21; both probed, no version sniffed).
//   * vmhook::jni::global_ref  -- records that epoch at construction, so oop()
//     returns nullptr once a collection has happened instead of handing back an
//     address a relocating collector may have invalidated.  is_stale() reports
//     it; raw_unsafe() still yields the captured address for diagnostics.
//
// WHAT MAKES THIS TEST WORTH ANYTHING.  global_ref is explicitly NOT a GC root
// (see its class doc) -- it does not keep the object alive.  So the object under
// test is kept alive from JAVA, in GcRelocationProbe.subject.  That static field
// IS a real root, which is what makes this test provable rather than assumed:
// the collector UPDATES the field when it moves the object, so re-reading its
// oop natively after the forced collection is an INDEPENDENT witness of the new
// address.  Comparing it against the address captured before the collection
// establishes "the object physically moved"; only then is
// "the detector caught it" asserted HARD.  Without that witness a green run
// would prove nothing (docs/research/build_and_validation_manual.md section 5.3).
//
// Assertion policy, in line with the repo's gating discipline:
//
//   HARD, always:
//     * the fail-closed contract -- a default/empty/never-sampled epoch is
//       ALWAYS treated as stale, and a default global_ref is stale with a null
//       oop().  These need no collector and no JVM state at all.
//     * Layer 0 identifies the collector, and `supported` is exactly the
//       documented function of the collector + epoch readability.
//     * compressed_oops matches an INDEPENDENT measurement of the VM (the
//       distance between two adjacent oop instance fields: 4 compressed, 8 not).
//     * a freshly built ref is NOT stale and hands back the captured address.
//     * the collection counter advances across the forced collection.
//     * once the epoch HAS advanced, the ref reports stale, oop()/handle() are
//       null, operator bool is false, and raw_unsafe() still reports the
//       ORIGINAL capture.  (This needs a collection, not a relocation.)
//     * when the object is PROVEN to have moved: the detector caught it, and the
//       relocated object is still intact at its new address (sentinel re-read).
//     * a ref taken AFTER the collection is fresh again (not one-shot poisoned).
//
//   [INFO], degraded, never a failure:
//     * no relocation observed after the bounded retry -- a forced System.gc()
//       is only a hint, and CI runs the target JVM with -Xmx4g -Xmn3g, whose
//       3 GB young generation makes a natural evacuation unlikely.  GC
//       relocation variance must NEVER hard-fail (and must never crash the JVM).
//     * a natural collection racing the "no spurious staleness" window.
//     * an unsupported collector (ZGC / Shenandoah) -- the CI matrix runs
//       Serial / Parallel / G1 only, so that arm is recorded, not exercised.
//
// SAFETY.  This module needs NO hook and NO detour: every step is a read.  The
// subject's address is re-derived from the Java static field each time, and the
// only dereference -- the post-relocation sentinel re-read -- is gated behind
// is_valid_pointer AND a fault-safe os::safe_read probe of the object header, so
// a stale/unmapped address records an [INFO] instead of taking the JVM down on
// the no-SEH toolchains (MinGW, clang-on-Windows).
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>

namespace
{
    // ---------------------------------------------------------------------
    //  Compile-time contract of the newly added API surface.
    // ---------------------------------------------------------------------
    using gref = vmhook::jni::global_ref;

    static_assert(!std::is_copy_constructible_v<gref>,
                  "global_ref must NOT be copy-constructible.");
    static_assert(!std::is_copy_assignable_v<gref>,
                  "global_ref must NOT be copy-assignable.");
    static_assert(std::is_nothrow_move_constructible_v<gref>,
                  "global_ref move ctor must be noexcept.");
    static_assert(std::is_nothrow_move_assignable_v<gref>,
                  "global_ref move-assign must be noexcept.");
    static_assert(std::is_nothrow_default_constructible_v<gref>,
                  "an empty holder must be a noexcept no-op.");
    static_assert(std::is_final_v<gref>,
                  "global_ref must be final.");
    static_assert(noexcept(std::declval<const gref&>().is_stale()),
                  "global_ref::is_stale() must be a noexcept observer.");
    static_assert(noexcept(std::declval<const gref&>().raw_unsafe()),
                  "global_ref::raw_unsafe() must be a noexcept observer.");
    static_assert(noexcept(std::declval<const gref&>().oop()),
                  "global_ref::oop() must be a noexcept observer.");
    static_assert(std::is_same_v<decltype(std::declval<const gref&>().is_stale()), bool>,
                  "global_ref::is_stale() must yield bool.");
    static_assert(std::is_same_v<decltype(std::declval<const gref&>().raw_unsafe()), vmhook::oop_t>,
                  "global_ref::raw_unsafe() must yield a vmhook::oop_t.");
    static_assert(noexcept(vmhook::vm_capabilities()),
                  "vm_capabilities() must be noexcept (it is read-only).");
    static_assert(noexcept(vmhook::gc_epoch()),
                  "gc_epoch() must be noexcept (it is read-only).");
    static_assert(noexcept(vmhook::gc_epoch_changed(std::declval<const vmhook::gc_epoch_t&>())),
                  "gc_epoch_changed() must be noexcept.");
    static_assert(std::is_same_v<decltype(vmhook::vm_capabilities()),
                                 const vmhook::vm_capabilities_t&>,
                  "vm_capabilities() must return a reference to cached, immutable state.");

    // FAIL-CLOSED defaults, pinned at compile time: a sample that was never
    // taken is invalid AND reports a pause in progress, so it can only ever be
    // read as "stale".  An unreadable epoch must never degrade to "assume the
    // object did not move".
    static_assert(!vmhook::gc_epoch_t{}.valid,
                  "a default gc_epoch_t must be INVALID (fail closed).");
    static_assert(vmhook::gc_epoch_t{}.gc_active,
                  "a default gc_epoch_t must read as gc-active (fail closed).");
    static_assert(!vmhook::vm_capabilities_t{}.supported,
                  "default capabilities must be unsupported (fail closed).");
    static_assert(vmhook::vm_capabilities_t{}.collector == vmhook::gc_collector::unknown,
                  "default capabilities must not name a collector.");

    // ---------------------------------------------------------------------
    //  Fixture wrapper
    // ---------------------------------------------------------------------
    constexpr const char* k_fixture{ "vmhook/fixtures/GcRelocationProbe" };
    constexpr std::int32_t k_sentinel{ 0x5A5A };

    // Probe modes (lockstep with GcRelocationProbe.java).
    constexpr std::int32_t k_mode_arm{ 1 };      // publish a fresh subject
    constexpr std::int32_t k_mode_collect{ 2 };  // churn + System.gc() x2

    class gc_probe_fixture : public vmhook::object<gc_probe_fixture>
    {
    public:
        explicit gc_probe_fixture(vmhook::oop_t instance) noexcept
            : vmhook::object<gc_probe_fixture>{ instance }
        {
        }

        static auto set_go(bool value) -> void       { static_field("go")->set(value); }
        static auto set_done(bool value) -> void      { static_field("done")->set(value); }
        static auto get_done() -> bool                { return static_field("done")->get(); }
        static auto set_mode(std::int32_t m) -> void  { static_field("mode")->set(m); }
        static auto gc_rounds() -> std::int32_t       { return static_field("gcRounds")->get(); }
        static auto arm_rounds() -> std::int32_t      { return static_field("armRounds")->get(); }

        // The CURRENT address of the object held by the static `subject` root.
        // Re-derived from the field on every call, so after a collection this
        // reports the object's NEW address -- the independent movement witness.
        static auto subject_oop() -> void*
        {
            const auto proxy{ static_field("subject") };
            if (!proxy.has_value())
            {
                return nullptr;
            }
            void* const oop{ vmhook::field_oop(*proxy) };
            return (oop && vmhook::hotspot::is_valid_pointer(oop)) ? oop : nullptr;
        }

        auto sentinel() -> std::int32_t { return get_field("sentinel")->get(); }
    };

    // ---------------------------------------------------------------------
    //  Safety helpers
    // ---------------------------------------------------------------------

    // is_valid_pointer is only a range + alignment heuristic.  Before touching a
    // possibly-relocated oop, additionally probe its header through
    // os::safe_read (ReadProcessMemory / process_vm_readv), which returns false
    // rather than faulting when the page is unmapped.  Model copied from
    // tests/jvm/modules/field_introspection.cpp.
    constexpr std::size_t k_oop_header_probe_bytes{ 16 };

    auto oop_header_safely_readable(void* const decoded) noexcept -> bool
    {
        if (!decoded || !vmhook::hotspot::is_valid_pointer(decoded))
        {
            return false;
        }
        std::uint8_t scratch[k_oop_header_probe_bytes]{};
        return vmhook::os::safe_read(scratch, decoded, sizeof(scratch));
    }

    auto drive(vmhook_test::context& ctx, const std::int32_t mode) -> bool
    {
        if (!ctx.run_probe)
        {
            return false;
        }
        return ctx.run_probe(
            [mode](bool value)
            {
                if (value)
                {
                    gc_probe_fixture::set_done(false);
                    gc_probe_fixture::set_mode(mode);
                }
                gc_probe_fixture::set_go(value);
            },
            []() { return gc_probe_fixture::get_done(); });
    }

    auto as_hex(const void* const pointer) -> std::string
    {
        std::ostringstream oss{};
        oss << "0x" << std::hex << reinterpret_cast<std::uintptr_t>(pointer);
        return oss.str();
    }

    // In-heap oop slot width, measured from the VM itself: two ADJACENT
    // reference instance fields on the fixture sit 4 bytes apart with
    // compressed oops and 8 without.  Returns 0 when the two offsets are
    // neither (an unexpected layout), which the caller degrades to [INFO]
    // rather than guessing.
    auto measured_oop_slot_width(vmhook::hotspot::klass* const fixture_klass) noexcept
        -> std::size_t
    {
        if (!fixture_klass)
        {
            return 0;
        }
        const auto a{ fixture_klass->find_field("refA") };
        const auto b{ fixture_klass->find_field("refB") };
        if (!a.has_value() || !b.has_value())
        {
            return 0;
        }
        const std::uint32_t lo{ a->offset < b->offset ? a->offset : b->offset };
        const std::uint32_t hi{ a->offset < b->offset ? b->offset : a->offset };
        const std::uint32_t delta{ hi - lo };
        return (delta == 4u || delta == 8u) ? static_cast<std::size_t>(delta) : 0u;
    }
}

VMHOOK_JVM_MODULE(gc_relocation_detector)
{
    // =====================================================================
    //  SECTION 0 -- the FAIL-CLOSED contract.  No JVM state, no collector,
    //  no allocation: these must hold on every cell of the matrix.
    // =====================================================================
    ctx.check("gc_default_epoch_is_treated_as_stale",
              vmhook::gc_epoch_changed(vmhook::gc_epoch_t{}));
    {
        // A sample that claims to be valid but was taken DURING a pause is also
        // untrustworthy (the counter may already have been bumped), so it too
        // must read as changed.
        const vmhook::gc_epoch_t during_pause{ 7u, true, true };
        ctx.check("gc_epoch_sampled_during_a_pause_is_treated_as_stale",
                  vmhook::gc_epoch_changed(during_pause));
    }
    {
        const vmhook::jni::global_ref empty{};
        ctx.check("gc_default_global_ref_is_stale", empty.is_stale());
        ctx.check("gc_default_global_ref_oop_is_null", empty.oop() == nullptr);
        ctx.check("gc_default_global_ref_handle_is_null", empty.handle() == nullptr);
        ctx.check("gc_default_global_ref_is_falsy", !static_cast<bool>(empty));
        ctx.check("gc_default_global_ref_raw_unsafe_is_null", empty.raw_unsafe() == nullptr);
    }
    {
        vmhook::jni::global_ref null_ref{ vmhook::pin(vmhook::oop_t{ nullptr }) };
        ctx.check("gc_pin_nullptr_is_falsy", !static_cast<bool>(null_ref));
        ctx.check("gc_pin_nullptr_oop_is_null", null_ref.oop() == nullptr);
        ctx.check("gc_pin_nullptr_raw_unsafe_is_null", null_ref.raw_unsafe() == nullptr);
        null_ref.reset();
        ctx.check("gc_reset_is_safe_on_an_empty_holder",
                  null_ref.oop() == nullptr && null_ref.raw_unsafe() == nullptr);
    }

    // =====================================================================
    //  SECTION A -- Layer 0 capabilities.
    // =====================================================================
    const vmhook::vm_capabilities_t& caps{ vmhook::vm_capabilities() };
    {
        std::ostringstream oss{};
        oss << "[INFO] gc_relocation_detector: vm_capabilities collector="
            << vmhook::gc_collector_name(caps.collector)
            << " supported=" << (caps.supported ? 1 : 0)
            << " flags_resolved=" << (caps.flags_resolved ? 1 : 0)
            << " epoch_readable=" << (caps.epoch_readable ? 1 : 0)
            << " compressed_oops=" << (caps.compressed_oops ? 1 : 0)
            << " compact_object_headers=" << (caps.compact_object_headers ? 1 : 0)
            << " barrier=" << static_cast<int>(caps.barrier);
        ctx.record(oss.str());
    }

    // The JVM flag table (Flag on 8, JVMFlag on 11+) is what identifies the
    // collector and reads UseCompressedOops; the walk describes its own shape
    // through gHotSpotVMTypes, so it must resolve on every JDK in the matrix.
    ctx.check("gc_capabilities_flag_table_resolved", caps.flags_resolved);
    ctx.check("gc_capabilities_collector_identified",
              caps.collector != vmhook::gc_collector::unknown);

    const bool relocating_stw{ caps.collector == vmhook::gc_collector::serial
                            || caps.collector == vmhook::gc_collector::parallel
                            || caps.collector == vmhook::gc_collector::g1 };
    const bool concurrent_relocator{ caps.collector == vmhook::gc_collector::z
                                  || caps.collector == vmhook::gc_collector::shenandoah };

    // `supported` is exactly the documented function of the collector and the
    // epoch coordinates -- never a guess.  This invariant holds whatever the VM
    // happens to be running, so it is HARD on every cell.
    {
        const bool expected_supported{
            relocating_stw ? caps.epoch_readable
                           : (caps.collector == vmhook::gc_collector::epsilon) };
        ctx.check("gc_capabilities_supported_is_the_documented_function_of_the_collector",
                  caps.supported == expected_supported);
    }
    ctx.check("gc_capabilities_unsupported_collectors_fail_closed",
              !concurrent_relocator || !caps.supported);

    // The CI matrix (GitHub + .localci) never selects a collector flag, so the
    // VM ergonomically picks Serial, Parallel or G1 -- every one of which the
    // detector supports.  A cell that lands anywhere else is a finding.  (A
    // concurrent relocator is handled by its own arm below; asserting
    // `supported` there would be asserting the opposite of the contract.)
    if (!concurrent_relocator)
    {
        ctx.check("gc_capabilities_collector_is_one_the_detector_supports", caps.supported);
    }
    if (relocating_stw)
    {
        ctx.check("gc_capabilities_epoch_coordinates_resolved", caps.epoch_readable);
    }

    // Independent cross-check of compressed_oops: measure the in-heap oop slot
    // width from the VM's own field layout rather than re-reading the flag.
    vmhook::hotspot::klass* const fixture_klass{ vmhook::find_class(k_fixture) };
    {
        const std::size_t width{ measured_oop_slot_width(fixture_klass) };
        if (width != 0)
        {
            ctx.check("gc_capabilities_compressed_oops_matches_measured_slot_width",
                      caps.compressed_oops == (width == 4));
            std::ostringstream oss{};
            oss << "[INFO] gc_relocation_detector: measured in-heap oop slot width = " << width
                << " byte(s) from two adjacent reference fields; vm_capabilities reports "
                   "compressed_oops=" << (caps.compressed_oops ? 1 : 0);
            ctx.record(oss.str());
        }
        else
        {
            ctx.record("[INFO] gc_relocation_detector: could not measure the oop slot width "
                       "(GcRelocationProbe.refA/refB offsets absent or not 4/8 apart), so the "
                       "independent compressed_oops cross-check is not asserted this run.");
        }
    }

    if (concurrent_relocator)
    {
        ctx.record("[INFO] gc_relocation_detector: this VM runs a CONCURRENT relocator (ZGC / "
                   "Shenandoah).  Their counters tick at cycle start and objects move behind "
                   "load barriers, so an unchanged counter proves nothing and the detector "
                   "correctly refuses to vouch for any address -- every ref is born stale.  The "
                   "CI matrix never selects such a collector, so the live relocation drive below "
                   "is skipped rather than exercised.");
        return;
    }

    // =====================================================================
    //  SECTION B -- Layer 1 epoch sampling.
    // =====================================================================
    const vmhook::gc_epoch_t boot_epoch{ vmhook::gc_epoch() };
    {
        std::ostringstream oss{};
        oss << "[INFO] gc_relocation_detector: gc_epoch at module entry valid="
            << (boot_epoch.valid ? 1 : 0) << " gc_active=" << (boot_epoch.gc_active ? 1 : 0)
            << " collections=" << boot_epoch.collections;
        ctx.record(oss.str());
    }
    ctx.check("gc_epoch_validity_tracks_the_capability_gate",
              boot_epoch.valid == caps.supported);

    if (caps.collector == vmhook::gc_collector::epsilon)
    {
        ctx.record("[INFO] gc_relocation_detector: Epsilon never moves or reclaims anything, so "
                   "its epoch is a constant and no relocation exists to detect; the live "
                   "relocation drive is skipped.");
        return;
    }

    if (fixture_klass == nullptr)
    {
        ctx.record("[INFO] gc_relocation_detector: the GcRelocationProbe fixture is not loaded on "
                   "this run; the live relocation drive is skipped (no crash, nothing armed).");
        return;
    }
    vmhook::register_class<gc_probe_fixture>(k_fixture);

    // =====================================================================
    //  SECTION C -- the central test: allocate, capture, force collections
    //  until the object PROVABLY moves, and check the detector caught it.
    // =====================================================================
    constexpr int k_max_attempts{ 4 };

    bool          any_arm_ok{ false };
    bool          any_collect_ok{ false };
    bool          fresh_asserted{ false };
    bool          epoch_advanced{ false };
    bool          moved{ false };
    bool          detector_checked_after_collection{ false };
    void*         addr_before{ nullptr };
    void*         addr_after{ nullptr };
    std::uint32_t collections_before{ 0 };
    std::uint32_t collections_after{ 0 };
    int           attempts_run{ 0 };

    // Observations taken from the SAME holder, on the SAME attempt, in which the
    // object was proven to have moved -- so "it moved and we detected it" is one
    // coherent statement rather than two unrelated ones.
    bool moved_ref_reported_stale{ false };
    bool moved_ref_oop_was_null{ false };
    bool moved_ref_handle_was_null{ false };
    bool moved_ref_was_falsy{ false };
    bool moved_ref_raw_unsafe_kept_capture{ false };

    for (int attempt{ 0 }; attempt < k_max_attempts && !moved; ++attempt)
    {
        ++attempts_run;

        // -- arm: publish a fresh subject behind a band of dead garbage -----
        const bool armed{ drive(ctx, k_mode_arm) };
        any_arm_ok = any_arm_ok || armed;
        if (!armed)
        {
            continue;
        }

        void* const before{ gc_probe_fixture::subject_oop() };
        if (!before)
        {
            continue;
        }

        const vmhook::gc_epoch_t pre{ vmhook::gc_epoch() };
        const vmhook::jni::global_ref ref{ vmhook::pin(vmhook::oop_t{ before }) };

        // A freshly built ref, before any collection, must be usable.  If a
        // NATURAL collection slipped in between the pin and this read the ref is
        // legitimately stale; that is not a failure, so only assert on an
        // attempt where the epoch provably did not move.
        const bool quiet{ pre.valid && !pre.gc_active
                       && !vmhook::gc_epoch_changed(pre) };
        if (quiet && !fresh_asserted)
        {
            ctx.check("gc_fresh_ref_is_not_stale", !ref.is_stale());
            ctx.check("gc_fresh_ref_oop_is_the_captured_address", ref.oop() == before);
            ctx.check("gc_fresh_ref_handle_is_the_captured_address", ref.handle() == before);
            ctx.check("gc_fresh_ref_is_truthy", static_cast<bool>(ref));
            ctx.check("gc_fresh_ref_raw_unsafe_agrees", ref.raw_unsafe() == before);

            // No spurious staleness: repeated reads with nothing collecting must
            // stay fresh, or the detector would be useless.  Cross-checked
            // against the counter so a natural collection inside the window
            // degrades to [INFO] instead of failing.
            constexpr int k_idle_reads{ 4096 };
            bool stable{ true };
            for (int i{ 0 }; i < k_idle_reads && stable; ++i)
            {
                stable = !ref.is_stale();
            }
            const vmhook::gc_epoch_t post_window{ vmhook::gc_epoch() };
            if (post_window.valid && !post_window.gc_active
                && post_window.collections == pre.collections)
            {
                ctx.check("gc_no_spurious_staleness_over_4096_idle_reads", stable);
            }
            else
            {
                ctx.record("[INFO] gc_relocation_detector: a collection occurred during the "
                           "idle-read window, so the no-spurious-staleness check is not asserted "
                           "this run (the ref went stale for a real reason).");
            }
            fresh_asserted = true;
        }

        // -- collect: churn + two explicit full collections ----------------
        const bool collected{ drive(ctx, k_mode_collect) };
        any_collect_ok = any_collect_ok || collected;

        const vmhook::gc_epoch_t post{ vmhook::gc_epoch() };
        void* const after{ gc_probe_fixture::subject_oop() };

        if (pre.valid && post.valid && post.collections != pre.collections)
        {
            epoch_advanced = true;
            collections_before = pre.collections;
            collections_after  = post.collections;

            // A COLLECTION happened.  Independently of whether this particular
            // object moved, the detector's contract is that the ref now refuses
            // to hand its address back.  Nothing is dereferenced here, so this
            // is safe and HARD on every supported collector.
            if (!detector_checked_after_collection)
            {
                ctx.check("gc_ref_reports_stale_after_a_collection", ref.is_stale());
                ctx.check("gc_ref_oop_is_null_after_a_collection", ref.oop() == nullptr);
                ctx.check("gc_ref_handle_is_null_after_a_collection", ref.handle() == nullptr);
                ctx.check("gc_ref_is_falsy_after_a_collection", !static_cast<bool>(ref));
                ctx.check("gc_ref_raw_unsafe_still_reports_the_original_capture",
                          ref.raw_unsafe() == before);
                ctx.check("gc_ref_never_hands_back_the_captured_address_after_a_collection",
                          ref.oop() != before);
                detector_checked_after_collection = true;
            }
        }

        if (after != nullptr && after != before)
        {
            moved       = true;
            addr_before = before;
            addr_after  = after;

            // Same holder, same attempt, movement proven by the Java root.
            moved_ref_reported_stale          = ref.is_stale();
            moved_ref_oop_was_null            = ref.oop() == nullptr;
            moved_ref_handle_was_null         = ref.handle() == nullptr;
            moved_ref_was_falsy               = !static_cast<bool>(ref);
            moved_ref_raw_unsafe_kept_capture = ref.raw_unsafe() == before;
        }
        else if (attempt == k_max_attempts - 1)
        {
            addr_before = before;
            addr_after  = after;
        }
    }

    ctx.check("gc_arm_probe_completed", any_arm_ok);
    ctx.check("gc_collect_probe_completed", any_collect_ok);
    ctx.check("gc_java_side_forced_at_least_one_collection", gc_probe_fixture::gc_rounds() >= 1);
    ctx.check("gc_java_side_published_at_least_one_subject", gc_probe_fixture::arm_rounds() >= 1);

    if (!fresh_asserted)
    {
        ctx.record("[INFO] gc_relocation_detector: never observed a quiet window in which to "
                   "assert the freshly-built ref (a collection raced every attempt), so the "
                   "fresh-ref checks are not asserted this run.");
    }

    // The counter MUST advance across explicitly forced full collections on a
    // supported stop-the-world collector -- that counter is the entire basis of
    // the detector, so this is HARD rather than gated.
    ctx.check("gc_epoch_advanced_across_the_forced_collection", epoch_advanced);
    if (epoch_advanced)
    {
        std::ostringstream oss{};
        oss << "[INFO] gc_relocation_detector: CollectedHeap::_total_collections "
            << collections_before << " -> " << collections_after << " across "
            << attempts_run << " forced-collection attempt(s)";
        ctx.record(oss.str());
    }

    // -- the movement witness -------------------------------------------------
    {
        std::ostringstream oss{};
        oss << "[INFO] gc_relocation_detector: subject oop " << as_hex(addr_before) << " -> "
            << as_hex(addr_after) << (moved ? "  (MOVED)" : "  (in place)")
            << " after " << attempts_run << " attempt(s)";
        ctx.record(oss.str());
    }

    if (moved)
    {
        // The object PROVABLY relocated: the Java static root -- which the
        // collector itself updates -- now names a different address.  These are
        // the observations taken from the holder built on the OLD address, in
        // the very attempt that moved it, so this is the real "the object moved
        // and the detector caught it" assertion the whole module exists for.
        ctx.check("gc_relocation_changed_the_address", addr_after != addr_before);
        ctx.check("gc_moved_ref_reports_stale", moved_ref_reported_stale);
        ctx.check("gc_moved_ref_oop_is_null", moved_ref_oop_was_null);
        ctx.check("gc_moved_ref_handle_is_null", moved_ref_handle_was_null);
        ctx.check("gc_moved_ref_is_falsy", moved_ref_was_falsy);
        ctx.check("gc_moved_ref_raw_unsafe_still_reports_the_moved_from_address",
                  moved_ref_raw_unsafe_kept_capture);
        ctx.check("gc_epoch_advanced_when_the_object_moved", epoch_advanced);

        // Address validity is not liveness: re-read the sentinel THROUGH the new
        // address and prove it is still our object, intact.  Guarded by a
        // fault-safe header probe first -- never a raw deref.
        if (oop_header_safely_readable(addr_after))
        {
            gc_probe_fixture relocated{ addr_after };
            const std::int32_t sentinel{ relocated.sentinel() };
            ctx.check("gc_relocated_object_is_intact_at_its_new_address",
                      sentinel == k_sentinel);
            std::ostringstream oss{};
            oss << "[INFO] gc_relocation_detector: sentinel re-read at the NEW address = 0x"
                << std::hex << sentinel;
            ctx.record(oss.str());
        }
        else
        {
            ctx.record("[INFO] gc_relocation_detector: the relocated object's header was not "
                       "safely readable through os::safe_read, so the sentinel re-read is not "
                       "asserted this run (nothing was dereferenced).");
        }
    }
    else
    {
        ctx.record("[INFO] gc_relocation_detector: the subject did NOT move across "
                   + std::to_string(attempts_run)
                   + " forced-collection attempt(s), so the 'it moved and we detected it' case "
                     "is not asserted on this cell.  A forced System.gc() is only a hint, and CI "
                     "runs the target JVM with -Xmx4g -Xmn3g -- a 3 GB young generation makes a "
                     "real evacuation of any single object correspondingly less likely.  GC "
                     "relocation variance is never hard-failed here; the detector's "
                     "after-a-collection contract above was still asserted whenever the "
                     "collection counter advanced.");
    }

    if (!detector_checked_after_collection)
    {
        ctx.record("[INFO] gc_relocation_detector: the collection counter never advanced across "
                   "the forced collections on this run, so the after-a-collection staleness "
                   "checks had no collection to observe and were not asserted.");
    }

    // -- a ref taken AFTER the collection is fresh again ----------------------
    // The detector is not one-shot poisoned: rebuilding from the post-collection
    // address must yield a usable holder.  Gated on a quiet window, so a
    // collection racing this read degrades to [INFO].
    {
        void* const current{ gc_probe_fixture::subject_oop() };
        const vmhook::gc_epoch_t sample{ vmhook::gc_epoch() };
        if (current != nullptr && sample.valid && !sample.gc_active)
        {
            const vmhook::jni::global_ref rearmed{ vmhook::pin(vmhook::oop_t{ current }) };
            if (!vmhook::gc_epoch_changed(sample))
            {
                ctx.check("gc_ref_taken_after_the_collection_is_fresh_again", !rearmed.is_stale());
                ctx.check("gc_ref_taken_after_the_collection_resolves_to_the_new_address",
                          rearmed.oop() == current);
                ctx.check("gc_ref_taken_after_the_collection_is_truthy",
                          static_cast<bool>(rearmed));
            }
            else
            {
                ctx.record("[INFO] gc_relocation_detector: a collection raced the re-arm window, "
                           "so the 'fresh again after a collection' checks are not asserted this "
                           "run.");
            }
        }
        else
        {
            ctx.record("[INFO] gc_relocation_detector: the subject root or the epoch sample was "
                       "unavailable when re-arming, so the 'fresh again after a collection' "
                       "checks are not asserted this run.");
        }
    }

    // -- move semantics carry the epoch, not just the address -----------------
    // A move must transfer BOTH the address and its recorded epoch and empty the
    // source; a move that dropped the epoch would silently hand back a moved-from
    // address.  No collection is involved, so this is HARD.
    {
        void* const current{ gc_probe_fixture::subject_oop() };
        const vmhook::gc_epoch_t sample{ vmhook::gc_epoch() };
        if (current != nullptr && sample.valid && !sample.gc_active)
        {
            vmhook::jni::global_ref source{ vmhook::pin(vmhook::oop_t{ current }) };
            vmhook::jni::global_ref moved_to{ std::move(source) };
            const bool quiet{ !vmhook::gc_epoch_changed(sample) };
            if (quiet)
            {
                ctx.check("gc_move_construct_transfers_the_address_and_epoch",
                          moved_to.raw_unsafe() == current && moved_to.oop() == current
                              && !moved_to.is_stale());
                ctx.check("gc_move_construct_empties_the_source",
                          source.raw_unsafe() == nullptr && source.oop() == nullptr
                              && source.is_stale());

                vmhook::jni::global_ref target{};
                target = std::move(moved_to);
                ctx.check("gc_move_assign_transfers_the_address_and_epoch",
                          target.raw_unsafe() == current && target.oop() == current
                              && !target.is_stale());
                ctx.check("gc_move_assign_empties_the_source",
                          moved_to.raw_unsafe() == nullptr && moved_to.oop() == nullptr);

                target.reset();
                ctx.check("gc_reset_clears_both_the_address_and_the_epoch",
                          target.raw_unsafe() == nullptr && target.oop() == nullptr
                              && target.is_stale());
            }
            else
            {
                ctx.record("[INFO] gc_relocation_detector: a collection raced the move-semantics "
                           "window, so those checks are not asserted this run.");
            }
        }
        else
        {
            ctx.record("[INFO] gc_relocation_detector: no usable subject/epoch for the "
                       "move-semantics checks this run.");
        }
    }
}
