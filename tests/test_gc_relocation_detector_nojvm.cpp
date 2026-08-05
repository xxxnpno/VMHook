// Standalone (no-JVM) unit test for the Layer 0 capability gate
// (vmhook::vm_capabilities) and the Layer 1 GC relocation detector
// (vmhook::gc_epoch / vmhook::gc_epoch_changed, and oop_pin's use of them).
//
// ===========================================================================
// WHAT THIS FILE CAN AND CANNOT PROVE
// ===========================================================================
// No jvm.dll / libjvm.so is loaded into this binary, so gHotSpotVMStructs and
// gHotSpotVMTypes never resolve.  Every lookup the two layers make returns
// null, which makes this the GRACEFUL-DEGRADATION test and nothing more:
//
//   * vm_capabilities() must report collector `unknown`, barrier `unknown`,
//     supported == false, and both oop-shape booleans false-because-unknown
//     (flags_resolved == false says so explicitly);
//   * gc_epoch() must not crash and must return an INVALID sample;
//   * gc_epoch_changed() must answer TRUE for every input, including a sample
//     hand-built to look pristine -- fail closed, never "assume it did not
//     move";
//   * a oop_pin built from a FABRICATED address must report stale and hand
//     back nullptr from oop() / handle() / operator bool, while raw_unsafe()
//     still shows what was captured;
//   * move semantics must carry the address AND the epoch, and empty the source.
//
// It CANNOT prove that the detector fires on a real relocation, that the flag
// walk finds UseG1GC, or that the counter offsets are right -- all of that needs
// a live VM and belongs in tests/jvm/modules/ (or the out-of-tree live harness).
// Nothing below asserts any of it.
//
// SAFETY: the fabricated addresses here are never dereferenced by the test, and
// the header never dereferences them either -- oop_pin only ever stores and
// compares them.  The two layers themselves read only through os::safe_read /
// hotspot::safe_read_pointer, which are kernel-validated and cannot fault.
// ===========================================================================
#include <vmhook/vmhook.hpp>

#include <cstdint>
#include <cstdio>
#include <type_traits>
#include <utility>
#include <vector>

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

namespace
{
    auto oop_of(const std::uintptr_t bits) noexcept -> vmhook::oop_t
    {
        return reinterpret_cast<vmhook::oop_t>(bits);
    }
}

// ===========================================================================
// COMPILE-TIME PINS on the new public types.
// ===========================================================================

// -- gc_collector: a scoped, byte-wide enum whose `unknown` enumerator is the
//    zero value, so a value-initialised capability struct degrades to "we do
//    not know" rather than to a named collector.
static_assert(std::is_enum_v<vmhook::gc_collector>,
              "gc_collector must be an enum");
static_assert(!std::is_convertible_v<vmhook::gc_collector, int>,
              "gc_collector must be SCOPED (no implicit conversion to int)");
static_assert(std::is_same_v<std::underlying_type_t<vmhook::gc_collector>, std::uint8_t>,
              "gc_collector must be uint8_t-backed (it is stored in a cached struct)");
static_assert(static_cast<std::uint8_t>(vmhook::gc_collector::unknown) == 0u,
              "gc_collector::unknown must be the zero value so defaults fail closed");

static_assert(std::is_enum_v<vmhook::gc_barrier_shape>,
              "gc_barrier_shape must be an enum");
static_assert(!std::is_convertible_v<vmhook::gc_barrier_shape, int>,
              "gc_barrier_shape must be SCOPED");
static_assert(static_cast<std::uint8_t>(vmhook::gc_barrier_shape::unknown) == 0u,
              "gc_barrier_shape::unknown must be the zero value");

// -- vm_capabilities_t: a trivially-copyable, standard-layout value with NSDMIs
//    that spell out the safe default.  It is cached in a function-local static
//    and handed out by const reference, so it must not need a destructor.
static_assert(std::is_standard_layout_v<vmhook::vm_capabilities_t>,
              "vm_capabilities_t must be standard-layout");
static_assert(std::is_trivially_copyable_v<vmhook::vm_capabilities_t>,
              "vm_capabilities_t must be trivially copyable");
static_assert(std::is_trivially_destructible_v<vmhook::vm_capabilities_t>,
              "vm_capabilities_t must be trivially destructible (it lives in a static)");
static_assert(!std::is_trivially_default_constructible_v<vmhook::vm_capabilities_t>,
              "vm_capabilities_t must value-initialise via NSDMIs -> not trivially default-constructible");

// -- gc_epoch_t: same, and cheap enough to return by value on every dereference.
static_assert(std::is_standard_layout_v<vmhook::gc_epoch_t>,
              "gc_epoch_t must be standard-layout");
static_assert(std::is_trivially_copyable_v<vmhook::gc_epoch_t>,
              "gc_epoch_t must be trivially copyable (copied on every move)");
static_assert(!std::is_trivially_default_constructible_v<vmhook::gc_epoch_t>,
              "gc_epoch_t must value-initialise via NSDMIs -> not trivially default-constructible");
static_assert(std::is_same_v<decltype(vmhook::gc_epoch_t{}.collections), std::uint32_t>,
              "the counter mirrors HotSpot's `unsigned int _total_collections`");

// -- Accessor signatures.  vm_capabilities() MUST return a reference (it is a
//    cached singleton, not a fresh computation per call); gc_epoch() MUST
//    return by value (it is a fresh sample every call).  A drift either way is
//    a behaviour change, not a style change.
static_assert(std::is_same_v<decltype(vmhook::vm_capabilities()),
                             const vmhook::vm_capabilities_t&>,
              "vm_capabilities() must return a cached const reference");
static_assert(std::is_same_v<decltype(vmhook::gc_epoch()), vmhook::gc_epoch_t>,
              "gc_epoch() must return a fresh sample BY VALUE");
static_assert(std::is_same_v<decltype(vmhook::gc_epoch_changed(std::declval<const vmhook::gc_epoch_t&>())),
                             bool>,
              "gc_epoch_changed() must return bool");
static_assert(noexcept(vmhook::vm_capabilities()),
              "vm_capabilities() must be noexcept (it runs inside detours)");
static_assert(noexcept(vmhook::gc_epoch()),
              "gc_epoch() must be noexcept (it runs inside detours)");
static_assert(noexcept(vmhook::gc_epoch_changed(std::declval<const vmhook::gc_epoch_t&>())),
              "gc_epoch_changed() must be noexcept");
static_assert(noexcept(std::declval<const vmhook::oop_pin&>().is_stale()),
              "oop_pin::is_stale() must be noexcept");
static_assert(noexcept(std::declval<const vmhook::oop_pin&>().raw_unsafe()),
              "oop_pin::raw_unsafe() must be noexcept");
static_assert(std::is_same_v<decltype(std::declval<const vmhook::oop_pin&>().raw_unsafe()),
                             vmhook::oop_t>,
              "raw_unsafe() must yield oop_t");

int main()
{
    using vmhook::oop_pin;

    // =======================================================================
    // SECTION 1 -- Layer 0: with no JVM the capability gate must degrade to
    //   "unknown / unsupported" on EVERY axis, and must do so without faulting.
    // =======================================================================
    {
        const vmhook::vm_capabilities_t& caps{ vmhook::vm_capabilities() };

        check("caps_collector_is_unknown_without_a_jvm",
              caps.collector == vmhook::gc_collector::unknown);
        check("caps_barrier_shape_is_unknown_without_a_jvm",
              caps.barrier == vmhook::gc_barrier_shape::unknown);
        check("caps_not_supported_without_a_jvm", !caps.supported);
        check("caps_flag_walk_reports_failure", !caps.flags_resolved);
        // The two oop-shape booleans are meaningless when flags_resolved is
        // false; assert they kept their safe defaults rather than inventing a
        // value the flag table never supplied.
        check("caps_oop_shape_booleans_keep_safe_defaults",
              !caps.compressed_oops && !caps.compact_object_headers);
        check("caps_epoch_not_readable_without_a_jvm", !caps.epoch_readable);

        // "unknown collector" must be UNSUPPORTED, not "probably fine".  This is
        // the same code path ZGC / Shenandoah take, and it is the whole reason
        // the detector cannot be silently wrong on them.
        check("caps_unknown_collector_is_treated_as_unsupported",
              caps.collector == vmhook::gc_collector::unknown && !caps.supported);
    }

    // The result is CACHED: repeated calls must hand back the very same object,
    // not recompute (the flag walk is O(numFlags) and runs inside detours).
    {
        const vmhook::vm_capabilities_t& first{ vmhook::vm_capabilities() };
        const vmhook::vm_capabilities_t& second{ vmhook::vm_capabilities() };
        check("caps_are_cached_same_object", &first == &second);
        check("caps_are_stable_across_calls",
              first.collector == second.collector
                  && first.supported == second.supported
                  && first.barrier == second.barrier);
    }

    // Every enumerator has a name, and unknown is not silently spelled as a
    // real collector (this string ends up in user-facing logs).
    {
        check("collector_name_unknown", vmhook::gc_collector_name(vmhook::gc_collector::unknown) == "unknown");
        check("collector_name_serial", vmhook::gc_collector_name(vmhook::gc_collector::serial) == "Serial");
        check("collector_name_parallel", vmhook::gc_collector_name(vmhook::gc_collector::parallel) == "Parallel");
        check("collector_name_g1", vmhook::gc_collector_name(vmhook::gc_collector::g1) == "G1");
        check("collector_name_z", vmhook::gc_collector_name(vmhook::gc_collector::z) == "ZGC");
        check("collector_name_shenandoah",
              vmhook::gc_collector_name(vmhook::gc_collector::shenandoah) == "Shenandoah");
        check("collector_name_epsilon", vmhook::gc_collector_name(vmhook::gc_collector::epsilon) == "Epsilon");
    }

    // =======================================================================
    // SECTION 2 -- Layer 1: gc_epoch() must not crash and must fail closed.
    // =======================================================================
    {
        const vmhook::gc_epoch_t sample{ vmhook::gc_epoch() };
        check("gc_epoch_does_not_crash_without_a_jvm", true);
        check("gc_epoch_is_invalid_without_a_jvm", !sample.valid);

        // A default-constructed epoch is the canonical "cannot vouch" value, and
        // an unreadable sample must be indistinguishable from it.
        const vmhook::gc_epoch_t defaulted{};
        check("default_epoch_is_invalid", !defaulted.valid);
        check("default_epoch_assumes_a_pause_is_running", defaulted.gc_active);
        check("unreadable_epoch_matches_the_default",
              sample.valid == defaulted.valid && sample.collections == defaulted.collections);
    }

    // Calling it repeatedly is stable and still never faults (this is the call
    // pattern oop_pin::oop() produces -- once per dereference).
    {
        bool all_invalid{ true };
        for (int i{ 0 }; i < 1000; ++i)
        {
            all_invalid = all_invalid && !vmhook::gc_epoch().valid;
        }
        check("gc_epoch_is_stable_over_many_calls", all_invalid);
    }

    // =======================================================================
    // SECTION 3 -- gc_epoch_changed(): fail closed for EVERY input.
    // =======================================================================
    {
        check("changed_true_for_default_epoch",
              vmhook::gc_epoch_changed(vmhook::gc_epoch_t{}));
        check("changed_true_for_a_freshly_sampled_epoch",
              vmhook::gc_epoch_changed(vmhook::gc_epoch()));

        // The important one: a hand-built sample that LOOKS pristine (valid,
        // no pause, counter 0) must still report changed, because the CURRENT
        // sample cannot be read.  An unreadable present must never be reported
        // as "nothing happened".
        const vmhook::gc_epoch_t forged{ 0u, false, true };
        check("changed_true_when_the_current_sample_is_unreadable",
              vmhook::gc_epoch_changed(forged));

        // A recorded sample taken during a pause is unusable by construction,
        // regardless of anything else.
        const vmhook::gc_epoch_t during_pause{ 7u, true, true };
        check("changed_true_when_recorded_during_a_pause",
              vmhook::gc_epoch_changed(during_pause));

        // ... and an invalid recorded sample short-circuits without ever
        // consulting the VM.
        const vmhook::gc_epoch_t invalid_record{ 7u, false, false };
        check("changed_true_when_the_recorded_sample_was_invalid",
              vmhook::gc_epoch_changed(invalid_record));
    }

    // =======================================================================
    // SECTION 4 -- oop_pin is wired to the detector: a fabricated address is
    //   CAPTURED but never handed back.  This is the bug the whole exercise
    //   exists to fix -- the old stub returned it verbatim, forever.
    // =======================================================================
    {
        auto* const fake_oop{ oop_of(0x1234'5678u) };
        const oop_pin ref{ fake_oop };

        check("ref_reports_stale_without_a_readable_epoch", ref.is_stale());
        check("ref_oop_is_null_when_stale", ref.oop() == nullptr);
        check("ref_handle_is_null_when_stale", ref.handle() == nullptr);
        check("ref_bool_is_false_when_stale", !static_cast<bool>(ref));
        check("ref_never_hands_back_the_stale_address",
              ref.oop() != fake_oop && ref.handle() != fake_oop);
        check("ref_raw_unsafe_still_shows_what_was_captured",
              ref.raw_unsafe() == fake_oop);
    }

    // Sweep a spread of bit patterns: neither the capture nor the refusal may
    // depend on the value.  (None of these is dereferenced.)
    {
        const std::uintptr_t fakes[]{
            0x1u, 0x8u, 0x1000u, 0xDEADBEEFu, 0xFFFFFFFFu,
            static_cast<std::uintptr_t>(~static_cast<std::uintptr_t>(0)),
        };
        bool all_captured{ true };
        bool all_refused{ true };
        for (const std::uintptr_t bits : fakes)
        {
            auto* const fake_oop{ oop_of(bits) };
            const oop_pin ref{ fake_oop };
            all_captured = all_captured && ref.raw_unsafe() == fake_oop;
            all_refused  = all_refused
                        && ref.is_stale()
                        && ref.oop() == nullptr
                        && !static_cast<bool>(ref);
        }
        check("every_bit_pattern_is_captured_verbatim", all_captured);
        check("every_bit_pattern_is_refused", all_refused);
    }

    // A default-constructed / reset holder is stale too -- there is no address
    // to vouch for, so the answer is the same "do not use this".
    {
        oop_pin empty{};
        check("default_constructed_ref_is_stale", empty.is_stale());
        check("default_constructed_ref_has_no_captured_address",
              empty.raw_unsafe() == nullptr);

        oop_pin armed{ oop_of(0x4000u) };
        armed.reset();
        check("reset_clears_the_captured_address", armed.raw_unsafe() == nullptr);
        check("reset_holder_is_stale", armed.is_stale());
        check("reset_holder_reads_null", armed.oop() == nullptr && armed.handle() == nullptr);
    }

    // =======================================================================
    // SECTION 5 -- Move semantics still work, and now carry the EPOCH too.
    //   A move that copied the address but left the destination's epoch
    //   default-initialised would still be "safe" (everything reads stale), so
    //   these checks watch the captured address specifically -- that is the
    //   part a broken move would silently duplicate or drop.
    // =======================================================================
    {
        auto* const fake_oop{ oop_of(0x5100u) };
        oop_pin source{ fake_oop };
        oop_pin destination{ std::move(source) };

        check("move_ctor_transfers_the_captured_address",
              destination.raw_unsafe() == fake_oop);
        check("move_ctor_empties_the_source",
              source.raw_unsafe() == nullptr);            // NOLINT(bugprone-use-after-move)
        check("move_ctor_source_is_stale",
              source.is_stale());                          // NOLINT(bugprone-use-after-move)
        check("move_ctor_destination_still_fails_closed",
              destination.is_stale() && destination.oop() == nullptr);
    }
    {
        auto* const fake_a{ oop_of(0x5200u) };
        auto* const fake_b{ oop_of(0x5300u) };
        oop_pin a{ fake_a };
        oop_pin b{ fake_b };
        b = std::move(a);

        check("move_assign_transfers_the_captured_address", b.raw_unsafe() == fake_a);
        check("move_assign_drops_the_old_address", b.raw_unsafe() != fake_b);
        check("move_assign_empties_the_source",
              a.raw_unsafe() == nullptr);                  // NOLINT(bugprone-use-after-move)
        check("move_assign_destination_still_fails_closed",
              b.is_stale() && b.oop() == nullptr);
    }
    {
        // Re-arming a moved-from holder must work (it is an ordinary empty
        // holder afterwards, not a poisoned one).
        oop_pin a{ oop_of(0x5400u) };
        const oop_pin b{ std::move(a) };
        a = oop_pin{ oop_of(0x5500u) };                  // NOLINT(bugprone-use-after-move)
        check("moved_from_holder_can_be_rearmed", a.raw_unsafe() == oop_of(0x5500u));
        check("rearm_does_not_disturb_the_move_destination", b.raw_unsafe() == oop_of(0x5400u));
    }
    {
        // Container round-trip: the addresses survive a vector reallocation,
        // and every element still fails closed afterwards.
        std::vector<oop_pin> refs;
        for (std::uintptr_t bits{ 0x6000u }; bits < 0x6000u + 64u * 0x10u; bits += 0x10u)
        {
            refs.emplace_back(oop_of(bits));
        }
        bool ok{ true };
        std::uintptr_t expected{ 0x6000u };
        for (const oop_pin& ref : refs)
        {
            ok = ok && ref.raw_unsafe() == oop_of(expected) && ref.is_stale() && ref.oop() == nullptr;
            expected += 0x10u;
        }
        check("vector_growth_preserves_captured_addresses_and_staleness", ok);
    }

    // =======================================================================
    // SECTION 6 -- vmhook::pin() free helpers go through the same path.
    // =======================================================================
    {
        auto* const fake_oop{ oop_of(0x7100u) };
        const vmhook::oop_pin pinned{ vmhook::pin(fake_oop) };
        check("pin_captures_the_address", pinned.raw_unsafe() == fake_oop);
        check("pin_result_is_stale_without_a_readable_epoch", pinned.is_stale());
        check("pin_result_refuses_to_hand_the_address_back", pinned.oop() == nullptr);

        const vmhook::oop_pin pinned_null{ vmhook::pin(vmhook::oop_t{ nullptr }) };
        check("pin_null_is_empty_and_stale",
              pinned_null.raw_unsafe() == nullptr && pinned_null.is_stale());
    }

    return failures == 0 ? 0 : 1;
}
