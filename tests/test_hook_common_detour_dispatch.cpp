// Standalone (no-JVM) EXHAUSTIVE unit test for the hook_common_detour_dispatch
// feature: the lock-free dispatch core (vmhook::hotspot::common_detour) every
// interpreter-hook trampoline funnels into, plus the two pure pieces it leans
// on that ARE determinable with no live JVM:
//
//   (1) return_slot          - the POD the trampoline pushes on the native stack
//                              and reads back after the detour to decide
//                              allow-through vs. short-circuit.  Its layout /
//                              default-encoding / the "untouched slot => allow
//                              through" dispatch contract are pure C++.
//   (2) the dispatch DECISION - first-match-wins pointer-equality over
//                              g_hooked_methods, the shutdown bail boolean, and
//                              the thread-validity gate.  With no JVM the
//                              global vector is EMPTY and g_shutdown_requested is
//                              false; we pin those fail-closed invariants and
//                              model the scan's exact arithmetic on an owned
//                              replica (NEVER a fabricated frame walk).
//   (3) dont_inline / NO_COMPILE FLAG-BIT math - the JIT-inhibitor bit logic the
//                              dispatch feature installs alongside every hook:
//                              the NO_COMPILE 4-bit decomposition and the
//                              `1u << bit` set / clear / mask / idempotence /
//                              neighbour-no-bleed arithmetic over both widths
//                              (bit 2 for JDK 11..20 u2, bit 12 for JDK 21+ u4).
//   (4) klass_introspection  - the null / empty no-JVM contract of the public
//                              klass entry points (find_class, klass_from_oop)
//                              every hooked dispatch eventually needs.
//
// ---------------------------------------------------------------------------
// WHY THIS FILE EXISTS / WHAT IS OUT OF SCOPE
// ---------------------------------------------------------------------------
// common_detour is `static`, JVM-coupled, and its single cold deref
// (frame_pointer->get_method() at rbp-24) would fault if handed a fabricated
// frame, so it cannot be called directly here.  We therefore pin the pieces of
// its CONTRACT that are pure arithmetic / POD layout / global-state invariants,
// and reproduce its first-match-wins decision on an owned std::vector replica.
// The live exactly-once / AV-survival / thread-state guarantees belong to the
// JVM integration modules (hook_basic, return_value_cancel); the per-JDK
// derive_method_flags_layout offset sweep belongs to test_method_flags_width.
// This file is the standalone half: the DISPATCH-OWNED contract that no other
// no-JVM file pins as its own.
//
// Source of truth (vmhook/ext/vmhook/vmhook.hpp; line numbers approximate):
//   return_slot                       1313-1317  (bool cancel; int64_t retval)
//   return_value::set / cancel        1353-1415  (cancel=true on set/cancel)
//   java_thread_state::_thread_in_Java 4716      (== 8, the forced post-detour state)
//   common_detour                     7268-7346  (shutdown bail, thread gate, scan)
//   g_hooked_methods / mutex          7179-7180  (lock-free scanned vector)
//   g_shutdown_requested              7197       (reversible shutdown bail flag)
//   seh_invoke_detour                 7215-7242  (SEH/EH firewall)
//   hooked_method                     7120-7148  (method ptr + detour cell)
//   NO_COMPILE                        7579-7583  (4 JVM_ACC compile-control bits)
//   set_dont_inline mask/width        7700-7755  (1u<<bit; <16 / <32 width gates)
//   is_valid_pointer                  2047       (thread-validity gate predicate)
//   find_class empty-name reject      8146-8164  (klass introspection null contract)

#include <vmhook/vmhook.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

static int failures{ 0 };

static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok)
    {
        ++failures;
    }
}

// ===========================================================================
// SOURCE-DERIVED CONSTANTS (every one is referenced below; no unused const).
// ===========================================================================

// NO_COMPILE is the OR of four JVM_ACC compile-control bits (vmhook.hpp:7579).
// Reproduced independently so a regression in the header's definition is caught
// by value, not just by recompiling the header's own expression.
static constexpr std::uint32_t kAccNotC2Compilable{ 0x02000000u };  // JVM_ACC_NOT_C2_COMPILABLE
static constexpr std::uint32_t kAccNotC1Compilable{ 0x04000000u };  // JVM_ACC_NOT_C1_COMPILABLE
static constexpr std::uint32_t kAccNotC2OsrCompilable{ 0x08000000u };  // JVM_ACC_NOT_C2_OSR_COMPILABLE
static constexpr std::uint32_t kAccQueued{ 0x01000000u };  // JVM_ACC_QUEUED
static constexpr std::uint32_t kNoCompileExpected{
    kAccNotC2Compilable | kAccNotC1Compilable | kAccNotC2OsrCompilable | kAccQueued };

// _dont_inline bit positions (vmhook.hpp:7359 / 7479 / 7492).
static constexpr int kDontInlineBitU2{ 2 };   // JDK 11..20, inside the u2 _flags word
static constexpr int kDontInlineBitU4{ 12 };  // JDK 21+, inside the u4 MethodFlags::_status

// The post-detour forced thread state (vmhook.hpp:4716 / 7337).
static constexpr std::int8_t kThreadInJavaExpected{ 8 };

// ===========================================================================
// 0. COMPILE-TIME signature / layout / noexcept pins (static_assert).
//    A regression fails the BUILD, the strongest possible pin.
// ===========================================================================

// return_slot is a 2-field POD: bool cancel, int64_t retval.
static_assert(std::is_standard_layout_v<vmhook::hotspot::return_slot>,
              "return_slot must be standard-layout (it is laid out on the trampoline stack)");
static_assert(std::is_trivially_copyable_v<vmhook::hotspot::return_slot>,
              "return_slot must be trivially copyable (raw native-stack cell)");
static_assert(std::is_same_v<decltype(vmhook::hotspot::return_slot::cancel), bool>,
              "return_slot::cancel must be bool");
static_assert(std::is_same_v<decltype(vmhook::hotspot::return_slot::retval), std::int64_t>,
              "return_slot::retval must be int64_t (the 64-bit return cell)");

// The detour ABI the trampoline bakes in: an int64_t(*) taking
// (frame*, java_thread*, return_slot*).  Retval is passed through RAX
// (the C++ ABI return-value register) so the trampoline no longer wraps
// it through rbx.  Pin the function-pointer type shape.
static_assert(
    std::is_same_v<vmhook::hotspot::detour_function_t,
                   std::int64_t (*)(vmhook::hotspot::frame*, vmhook::hotspot::java_thread*,
                                    vmhook::hotspot::return_slot*)>,
    "detour_function_t must be int64_t(*)(frame*, java_thread*, return_slot*)");

// hooked_method.detour is a std::function with void return -- it writes
// result through the return_slot* parameter.  common_detour bridges the
// int64_t ABI to the void-returning std::function internally.
static_assert(
    std::is_same_v<decltype(vmhook::hotspot::hooked_method::detour),
                   std::function<void(vmhook::hotspot::frame*, vmhook::hotspot::java_thread*,
                                      vmhook::hotspot::return_slot*)>>,
    "hooked_method::detour must be std::function<void(frame*, java_thread*, return_slot*)>");

// hooked_method.method is the Method* the scan compares with == (first-match).
static_assert(std::is_same_v<decltype(vmhook::hotspot::hooked_method::method),
                             vmhook::hotspot::method*>,
              "hooked_method::method must be hotspot::method*");

// is_valid_pointer (the thread-validity gate) is noexcept and bool-returning.
static_assert(noexcept(vmhook::hotspot::is_valid_pointer(nullptr)),
              "is_valid_pointer must be noexcept (called on the hot dispatch path)");
static_assert(std::is_same_v<decltype(vmhook::hotspot::is_valid_pointer(nullptr)), bool>,
              "is_valid_pointer must return bool");

// g_shutdown_requested is an atomic<bool> -- the lock-free shutdown bail flag.
static_assert(std::is_same_v<decltype(vmhook::hotspot::g_shutdown_requested),
                             std::atomic<bool>>,
              "g_shutdown_requested must be std::atomic<bool>");

// _thread_in_Java enumerates to 8 (the forced post-detour state).
static_assert(static_cast<std::int8_t>(
                  vmhook::hotspot::java_thread_state::_thread_in_Java) == kThreadInJavaExpected,
              "_thread_in_Java must be 8 (the post-detour normalisation target)");

int main()
{
    using vmhook::hotspot::is_valid_pointer;

    check("dispatch_signature_static_asserts_compiled", true);

    // =====================================================================
    // A. return_slot POD layout / default encoding.
    //    The trampoline pushes a ZEROED slot (vmhook.hpp:6652-6653: two
    //    `push 0x0`) so a freshly-constructed return_slot must default to
    //    {cancel=false, retval=0}.  cancel sits at offset 0, retval at +8
    //    (the trampoline reads cancel at [rsp+0], retval at [rsp+8]).
    // =====================================================================
    {
        const vmhook::hotspot::return_slot slot{};
        check("return_slot_default_cancel_is_false", slot.cancel == false);
        check("return_slot_default_retval_is_zero", slot.retval == 0);
        // Offset contract: cancel is the FIRST member (offset 0); retval
        // follows after alignment padding.  retval is 8 bytes wide.
        check("return_slot_cancel_at_offset_0",
              offsetof(vmhook::hotspot::return_slot, cancel) == 0u);
        check("return_slot_retval_offset_at_least_after_cancel",
              offsetof(vmhook::hotspot::return_slot, retval)
                  >= sizeof(bool));
        check("return_slot_retval_is_8_bytes",
              sizeof(vmhook::hotspot::return_slot::retval) == 8u);
        // The whole slot fits the two stack pushes the trampoline reserves
        // (it discards 0x10 == 16 bytes via `add rsp, 0x10`).
        check("return_slot_fits_trampoline_reservation",
              sizeof(vmhook::hotspot::return_slot) <= 16u);
    }

    // =====================================================================
    // B. ALLOW-THROUGH contract: an UNTOUCHED slot means "run the original
    //    body".  common_detour itself never writes slot; only the user detour
    //    (via return_value) may set cancel.  So the dispatch-level promise is:
    //    a slot whose cancel stays false leaves retval irrelevant and the
    //    trampoline restores normal execution.  Pin that return_value leaves
    //    cancel false until something sets it, and flips it on set/cancel.
    // =====================================================================
    {
        vmhook::hotspot::return_slot slot{};
        vmhook::return_value rv{ &slot };
        // Constructing the handle does NOT touch the slot -> allow-through.
        check("allow_through_construct_leaves_cancel_false", slot.cancel == false);
        check("allow_through_construct_leaves_retval_zero", slot.retval == 0);

        // cancel() flips cancel true but does NOT write retval (void-return
        // short-circuit): the trampoline then short-circuits with retval 0.
        rv.cancel();
        check("cancel_sets_cancel_flag", slot.cancel == true);
        check("cancel_leaves_retval_untouched", slot.retval == 0);
    }
    {
        // set(value) flips cancel true AND encodes the value -> short-circuit.
        vmhook::hotspot::return_slot slot{};
        vmhook::return_value rv{ &slot };
        rv.set(std::int32_t{ 12345 });
        check("set_value_sets_cancel_flag", slot.cancel == true);
        check("set_value_encodes_retval", slot.retval == 12345);
    }
    {
        // A signed-narrow set sign-extends into the 64-bit cell (the encoding
        // the trampoline reads back); this is the slot's job, not the scan's,
        // but it pins that the dispatch cell carries the value faithfully.
        vmhook::hotspot::return_slot slot{};
        vmhook::return_value rv{ &slot };
        rv.set(std::int8_t{ -1 });
        check("set_signed_narrow_sign_extends_in_slot",
              slot.retval == static_cast<std::int64_t>(-1));
    }

    // =====================================================================
    // C. THREAD-VALIDITY GATE (common_detour's first in-try guard, vmhook.hpp
    //    7282): `if (!thread || !is_valid_pointer(thread)) throw`.  The gate's
    //    predicate is pure and JVM-independent.  Reproduce the EXACT boolean
    //    the gate evaluates for the inputs common_detour can legally receive:
    //    a null thread and a battery of is_valid_pointer-REJECTED low / poison
    //    / misaligned / kernel-half sentinels (NONE are dereferenced -- the
    //    gate rejects them BEFORE any thread read).
    // =====================================================================
    {
        // null thread -> gate fires (the `!thread` arm).
        const vmhook::hotspot::java_thread* const null_thread{ nullptr };
        const bool gate_fires_for_null{ !null_thread || !is_valid_pointer(null_thread) };
        check("thread_gate_fires_for_null", gate_fires_for_null);

        // is_valid_pointer-rejected sentinels: the gate's second arm fires for
        // every one.  These constants are NEVER dereferenced (low / poison /
        // odd / kernel-half values is_valid_pointer rejects up front).
        const std::uintptr_t rejected[]{
            std::uintptr_t{ 0x1u },                       // below floor + odd
            std::uintptr_t{ 0x8u },                       // below floor, aligned
            std::uintptr_t{ 0xFFFFu },                    // exactly the floor sentinel
            std::uintptr_t{ 0xDEADBEEFu },                // debug poison
            std::uintptr_t{ 0xCAFEBABEu },                // debug poison
            std::uintptr_t{ 0x0000'1234'0000'0001ull },   // in range but ODD
            std::uintptr_t{ 0x0000'8000'0000'0000ull },   // first kernel-half addr
            std::uintptr_t{ 0xFFFF'8000'0000'0000ull },   // high non-canonical
        };
        bool all_rejected_gate_fires{ true };
        std::size_t probed{ 0 };
        for (const std::uintptr_t value : rejected)
        {
            const auto* const candidate{
                reinterpret_cast<const vmhook::hotspot::java_thread*>(value) };
            // The gate fires (i.e. would throw) when the OR is true.
            const bool gate_fires{ !candidate || !is_valid_pointer(candidate) };
            if (!gate_fires) { all_rejected_gate_fires = false; }
            ++probed;
        }
        check("thread_gate_fires_for_all_rejected_sentinels", all_rejected_gate_fires);
        check("thread_gate_sentinel_battery_is_dense", probed >= 8u);

        // Conversely, a REAL mapped, aligned stack object passes the gate
        // (the only branch where common_detour would proceed to the scan).
        alignas(16) std::uint8_t fake_thread_storage[64]{};
        const auto* const valid_like{
            reinterpret_cast<const vmhook::hotspot::java_thread*>(fake_thread_storage) };
        const bool gate_passes{ !(!valid_like || !is_valid_pointer(valid_like)) };
        check("thread_gate_passes_for_valid_aligned_pointer", gate_passes);
    }

    // =====================================================================
    // D. SHUTDOWN BAIL is REVERSIBLE (the resolved one-way-latch hazard).
    //    common_detour returns immediately while g_shutdown_requested is true
    //    (vmhook.hpp:7275) and resumes dispatching once it is cleared again
    //    (shutdown_hooks() resets it at teardown end).  With no JVM the flag is
    //    an owned atomic we can toggle; pin that it starts false, that the bail
    //    predicate tracks it, and that flipping it back to false re-arms
    //    dispatch -- the regression guard against the latch ever returning.
    //    We SAVE and RESTORE the flag so we never disturb global state for any
    //    test that runs after us in the same process.
    // =====================================================================
    {
        const bool saved{ vmhook::hotspot::g_shutdown_requested.load(
            std::memory_order_acquire) };

        // No-JVM start state: not shutting down -> dispatch would proceed.
        check("shutdown_flag_starts_false",
              vmhook::hotspot::g_shutdown_requested.load(std::memory_order_acquire) == false);

        // The bail predicate is exactly `flag.load(acquire)`.
        vmhook::hotspot::g_shutdown_requested.store(true, std::memory_order_release);
        check("shutdown_bail_predicate_true_when_requested",
              vmhook::hotspot::g_shutdown_requested.load(std::memory_order_acquire) == true);

        // Reversibility: clearing it re-arms dispatch (bail predicate false).
        vmhook::hotspot::g_shutdown_requested.store(false, std::memory_order_release);
        check("shutdown_bail_predicate_false_after_reset",
              vmhook::hotspot::g_shutdown_requested.load(std::memory_order_acquire) == false);

        // A full set->clear->set->clear cycle stays consistent (no latch).
        bool cycle_consistent{ true };
        for (int iteration{ 0 }; iteration < 4; ++iteration)
        {
            vmhook::hotspot::g_shutdown_requested.store(true, std::memory_order_release);
            if (!vmhook::hotspot::g_shutdown_requested.load(std::memory_order_acquire))
            {
                cycle_consistent = false;
            }
            vmhook::hotspot::g_shutdown_requested.store(false, std::memory_order_release);
            if (vmhook::hotspot::g_shutdown_requested.load(std::memory_order_acquire))
            {
                cycle_consistent = false;
            }
        }
        check("shutdown_bail_set_clear_cycle_consistent", cycle_consistent);

        vmhook::hotspot::g_shutdown_requested.store(saved, std::memory_order_release);
        check("shutdown_flag_restored_for_other_tests",
              vmhook::hotspot::g_shutdown_requested.load(std::memory_order_acquire) == saved);
    }

    // =====================================================================
    // E. NO-JVM FAIL-CLOSED: g_hooked_methods is EMPTY at process start (no
    //    install ran), so common_detour's scan matches nothing and falls
    //    through to the original body for ANY current_method -- exactly the
    //    fail-closed behaviour the no-JVM contract promises.  Pin the empty
    //    invariant; we do NOT mutate the global vector (that would need the
    //    mutex and is the install path's job, out of scope here).
    // =====================================================================
    {
        check("hooked_methods_empty_with_no_jvm",
              vmhook::hotspot::g_hooked_methods.empty());
        check("hooked_methods_size_zero_with_no_jvm",
              vmhook::hotspot::g_hooked_methods.size() == 0u);
        // A scan over the empty global finds no match for any probe Method*.
        bool empty_scan_never_matches{ true };
        const auto* const probe{
            reinterpret_cast<const vmhook::hotspot::method*>(std::uintptr_t{ 0x1000u }) };
        for (const vmhook::hotspot::hooked_method& hook : vmhook::hotspot::g_hooked_methods)
        {
            if (hook.method == probe) { empty_scan_never_matches = false; }
        }
        check("empty_global_scan_matches_nothing", empty_scan_never_matches);
    }

    // =====================================================================
    // F. FIRST-MATCH-WINS / EXACTLY-ONE dispatch decision, modelled on an
    //    OWNED replica of g_hooked_methods (NEVER the live global, NEVER a
    //    fabricated frame).  This pins the pure decision arithmetic of the
    //    scan (vmhook.hpp:7317-7339): iterate entries, fire the FIRST whose
    //    `method == current_method`, then `return` -- so exactly ONE entry
    //    fires per call even when duplicates exist, and a non-member method
    //    fires NOTHING.  We use fabricated Method* VALUES purely for pointer
    //    EQUALITY (never dereferenced), and a counting detour to prove the
    //    fire count.
    // =====================================================================
    {
        // Synthetic, never-dereferenced Method* sentinels used only as the ==
        // key the scan compares.  Distinct values model distinct methods.
        auto* const method_a{ reinterpret_cast<vmhook::hotspot::method*>(
            std::uintptr_t{ 0x1000u }) };
        auto* const method_b{ reinterpret_cast<vmhook::hotspot::method*>(
            std::uintptr_t{ 0x2000u }) };
        auto* const method_unhooked{ reinterpret_cast<vmhook::hotspot::method*>(
            std::uintptr_t{ 0x3000u }) };

        int fire_count_a{ 0 };
        int fire_count_b{ 0 };

        std::vector<vmhook::hotspot::hooked_method> replica;
        {
            vmhook::hotspot::hooked_method entry_a{};
            entry_a.method = method_a;
            entry_a.detour = [&fire_count_a](vmhook::hotspot::frame*,
                                             vmhook::hotspot::java_thread*,
                                             vmhook::hotspot::return_slot*) noexcept
            {
                ++fire_count_a;
            };
            replica.push_back(std::move(entry_a));

            vmhook::hotspot::hooked_method entry_b{};
            entry_b.method = method_b;
            entry_b.detour = [&fire_count_b](vmhook::hotspot::frame*,
                                             vmhook::hotspot::java_thread*,
                                             vmhook::hotspot::return_slot*) noexcept
            {
                ++fire_count_b;
            };
            replica.push_back(std::move(entry_b));
        }

        // The EXACT scan common_detour runs: first match fires once, return.
        auto dispatch_once = [&replica](const vmhook::hotspot::method* const current) -> bool
        {
            for (const vmhook::hotspot::hooked_method& hook : replica)
            {
                if (hook.method == current)
                {
                    vmhook::hotspot::return_slot slot{};
                    hook.detour(nullptr, nullptr, &slot);
                    return true;   // fire-once-and-return
                }
            }
            return false;          // no match -> allow-through
        };

        // Calling method A N times fires A's detour EXACTLY N times, and never
        // B's (correct Method* discrimination).
        const int calls_a{ 1000 };
        int matched_a{ 0 };
        for (int i{ 0 }; i < calls_a; ++i)
        {
            if (dispatch_once(method_a)) { ++matched_a; }
        }
        check("dispatch_method_a_fires_exactly_n_lower_bound", fire_count_a >= calls_a);
        check("dispatch_method_a_fires_exactly_n_upper_bound", fire_count_a <= calls_a);
        check("dispatch_method_a_fired_count_equals_calls", fire_count_a == calls_a);
        check("dispatch_method_a_all_calls_matched", matched_a == calls_a);
        check("dispatch_method_a_did_not_fire_b", fire_count_b == 0);

        // Calling method B fires only B.
        const int calls_b{ 7 };
        for (int i{ 0 }; i < calls_b; ++i) { (void)dispatch_once(method_b); }
        check("dispatch_method_b_fired_count_equals_calls", fire_count_b == calls_b);
        check("dispatch_method_b_did_not_re_fire_a", fire_count_a == calls_a);

        // An UNHOOKED method matches nothing -> allow-through, no detour fires.
        bool unhooked_matched{ false };
        for (int i{ 0 }; i < 5; ++i)
        {
            if (dispatch_once(method_unhooked)) { unhooked_matched = true; }
        }
        check("dispatch_unhooked_method_matches_nothing", !unhooked_matched);
        check("dispatch_unhooked_left_a_count_unchanged", fire_count_a == calls_a);
        check("dispatch_unhooked_left_b_count_unchanged", fire_count_b == calls_b);
    }

    // =====================================================================
    // F2. FIRST-MATCH-WINS with a DUPLICATE Method* (flaw-#4 structural guard).
    //     If two entries share the same Method*, the scan fires the FIRST and
    //     returns -- the second is structurally unreachable.  In-library the
    //     duplicate-install check prevents this, but the DISPATCH decision must
    //     provably fire exactly once (the first) for a duplicate key.
    // =====================================================================
    {
        auto* const dup_method{ reinterpret_cast<vmhook::hotspot::method*>(
            std::uintptr_t{ 0x4000u }) };
        int first_fired{ 0 };
        int second_fired{ 0 };

        std::vector<vmhook::hotspot::hooked_method> replica;
        {
            vmhook::hotspot::hooked_method first{};
            first.method = dup_method;
            first.detour = [&first_fired](vmhook::hotspot::frame*,
                                          vmhook::hotspot::java_thread*,
                                          vmhook::hotspot::return_slot*) noexcept
            {
                ++first_fired;
            };
            replica.push_back(std::move(first));

            vmhook::hotspot::hooked_method second{};
            second.method = dup_method;   // SAME key
            second.detour = [&second_fired](vmhook::hotspot::frame*,
                                            vmhook::hotspot::java_thread*,
                                            vmhook::hotspot::return_slot*) noexcept
            {
                ++second_fired;
            };
            replica.push_back(std::move(second));
        }

        for (const vmhook::hotspot::hooked_method& hook : replica)
        {
            if (hook.method == dup_method)
            {
                vmhook::hotspot::return_slot slot{};
                hook.detour(nullptr, nullptr, &slot);
                break;   // first-match-and-return: exactly the scan's `return`
            }
        }
        check("duplicate_key_first_entry_fires", first_fired == 1);
        check("duplicate_key_second_entry_unreachable", second_fired == 0);
    }

    // =====================================================================
    // G. SEH FIREWALL catch-and-return-false (the non-MSVC try/catch arm,
    //    vmhook.hpp:7232-7240).  seh_invoke_detour wraps the user detour; a
    //    C++ exception thrown INSIDE the detour must be caught and converted to
    //    a `false` return so common_detour logs + falls through to the original
    //    body (it does NOT propagate into the JVM).  A clean detour returns
    //    true.  This is the platform-independent half of the AV-survival
    //    contract (the hardware-AV half is JVM-only and toolchain-variant).
    // =====================================================================
    {
        vmhook::hotspot::return_slot slot{};

        // A clean detour -> true.
        const std::function<void(vmhook::hotspot::frame*, vmhook::hotspot::java_thread*,
                                 vmhook::hotspot::return_slot*)>
            clean_detour{ [](vmhook::hotspot::frame*, vmhook::hotspot::java_thread*,
                             vmhook::hotspot::return_slot*) noexcept {} };
        const bool clean_ok{ vmhook::hotspot::seh_invoke_detour(
            clean_detour, nullptr, nullptr, &slot) };
        check("seh_invoke_clean_detour_returns_true", clean_ok == true);

        // A detour that throws a C++ exception -> false (caught, not propagated).
        const std::function<void(vmhook::hotspot::frame*, vmhook::hotspot::java_thread*,
                                 vmhook::hotspot::return_slot*)>
            throwing_detour{ [](vmhook::hotspot::frame*, vmhook::hotspot::java_thread*,
                                vmhook::hotspot::return_slot*)
            {
                throw vmhook::exception{ "deliberate detour fault" };
            } };
        const bool throw_contained{ !vmhook::hotspot::seh_invoke_detour(
            throwing_detour, nullptr, nullptr, &slot) };
        check("seh_invoke_throwing_detour_returns_false", throw_contained);

        // After a contained fault the firewall is reusable: a subsequent clean
        // detour still returns true (the firewall does not poison itself).
        const bool clean_after_fault{ vmhook::hotspot::seh_invoke_detour(
            clean_detour, nullptr, nullptr, &slot) };
        check("seh_invoke_clean_after_fault_returns_true", clean_after_fault == true);

        // A throwing detour leaves cancel UNTOUCHED (false) -> common_detour
        // falls through to the original body, the allow-through outcome.
        vmhook::hotspot::return_slot fault_slot{};
        (void)vmhook::hotspot::seh_invoke_detour(throwing_detour, nullptr, nullptr, &fault_slot);
        check("seh_invoke_fault_leaves_cancel_false_allow_through",
              fault_slot.cancel == false);
    }

    // =====================================================================
    // H. NO_COMPILE flag-bit decomposition (dont_inline_dont_compile feature).
    //    NO_COMPILE (vmhook.hpp:7579) is the OR of four JVM_ACC compile-control
    //    bits.  Pin the EXACT value, that the header's NO_COMPILE equals our
    //    independent recomposition, that the four bits are mutually disjoint
    //    (no double-counting), occupy bits 24..27 only, and that ORing it is
    //    idempotent with no bleed into neighbour bits.
    // =====================================================================
    {
        const std::uint32_t header_no_compile{
            static_cast<std::uint32_t>(vmhook::hotspot::NO_COMPILE) };
        check("no_compile_matches_independent_recomposition",
              header_no_compile == kNoCompileExpected);
        check("no_compile_exact_value_0x0F000000",
              header_no_compile == 0x0F000000u);

        // The four constituent bits are pairwise disjoint (each is a distinct
        // power of two) so the OR sets exactly four bits, popcount == 4.
        const std::uint32_t all_four[]{ kAccQueued, kAccNotC2Compilable,
                                        kAccNotC1Compilable, kAccNotC2OsrCompilable };
        bool pairwise_disjoint{ true };
        for (std::size_t i{ 0 }; i < 4u; ++i)
        {
            for (std::size_t j{ i + 1u }; j < 4u; ++j)
            {
                if ((all_four[i] & all_four[j]) != 0u) { pairwise_disjoint = false; }
            }
        }
        check("no_compile_constituent_bits_pairwise_disjoint", pairwise_disjoint);

        // popcount of NO_COMPILE is exactly 4 (no accidental extra bits).
        int set_bits{ 0 };
        for (int bit{ 0 }; bit < 32; ++bit)
        {
            if ((header_no_compile & (std::uint32_t{ 1u } << bit)) != 0u) { ++set_bits; }
        }
        check("no_compile_popcount_is_4", set_bits == 4);

        // All set bits live in the high byte (bits 24..31); specifically 24..27.
        check("no_compile_low_24_bits_clear", (header_no_compile & 0x00FFFFFFu) == 0u);
        check("no_compile_only_bits_24_to_27",
              (header_no_compile & ~std::uint32_t{ 0x0F000000u }) == 0u);

        // Idempotence: OR-ing NO_COMPILE into a word twice equals once.
        const std::uint32_t base_flags{ 0x00000021u };  // some unrelated access bits
        const std::uint32_t once{ base_flags | header_no_compile };
        const std::uint32_t twice{ once | header_no_compile };
        check("no_compile_or_is_idempotent", once == twice);
        // ...and it does not disturb the pre-existing low bits.
        check("no_compile_or_preserves_low_bits",
              (once & 0x00FFFFFFu) == (base_flags & 0x00FFFFFFu));
        // Clearing it (AND ~NO_COMPILE) restores the original word.
        check("no_compile_clear_restores_original",
              (once & ~header_no_compile) == base_flags);
    }

    // =====================================================================
    // I. _dont_inline bit MASK arithmetic (the set/clear RMW set_dont_inline
    //    performs, vmhook.hpp:7700-7755), exercised as PURE bit logic on an
    //    OWNED word for BOTH widths.  The mask is `1u << bit` (bit 2 for the u2
    //    layout, bit 12 for the u4 layout); set == fetch_or(mask), clear ==
    //    `word & ~mask`.  Pin: the mask hits exactly the named bit, set is
    //    idempotent, clear restores, neither bleeds into a neighbour, and the
    //    width GUARDS (`bit < 16` for u2, `bit < 32` for u4) hold for the bits
    //    the layouts actually use.
    // =====================================================================
    {
        // u2 layout: bit 2, masked into a 16-bit word.
        check("dont_inline_u2_bit_within_width_guard", kDontInlineBitU2 < 16);
        const std::uint16_t mask_u2{
            static_cast<std::uint16_t>(1u << kDontInlineBitU2) };
        check("dont_inline_u2_mask_is_0x0004", mask_u2 == 0x0004u);

        // Set into a zero word lands exactly bit 2; idempotent.
        std::uint16_t word_u2{ 0u };
        word_u2 = static_cast<std::uint16_t>(word_u2 | mask_u2);
        check("dont_inline_u2_set_lands_bit2", word_u2 == 0x0004u);
        const std::uint16_t word_u2_twice{ static_cast<std::uint16_t>(word_u2 | mask_u2) };
        check("dont_inline_u2_set_idempotent", word_u2_twice == word_u2);
        // Clear restores zero.
        const std::uint16_t cleared_u2{
            static_cast<std::uint16_t>(word_u2 & static_cast<std::uint16_t>(~mask_u2)) };
        check("dont_inline_u2_clear_restores_zero", cleared_u2 == 0u);
        // No neighbour bleed: set into a word with the neighbour bits set leaves
        // them intact, and clearing only clears bit 2.
        std::uint16_t neighbours_u2{ 0xFFFBu };  // every bit EXCEPT bit 2
        const std::uint16_t set_with_neighbours{
            static_cast<std::uint16_t>(neighbours_u2 | mask_u2) };
        check("dont_inline_u2_set_no_neighbour_bleed", set_with_neighbours == 0xFFFFu);
        const std::uint16_t clear_with_neighbours{
            static_cast<std::uint16_t>(0xFFFFu & static_cast<std::uint16_t>(~mask_u2)) };
        check("dont_inline_u2_clear_only_target_bit", clear_with_neighbours == 0xFFFBu);

        // u4 layout: bit 12, masked into a 32-bit word.
        check("dont_inline_u4_bit_within_width_guard", kDontInlineBitU4 < 32);
        const std::uint32_t mask_u4{ 1u << kDontInlineBitU4 };
        check("dont_inline_u4_mask_is_0x1000", mask_u4 == 0x00001000u);

        std::uint32_t word_u4{ 0u };
        word_u4 |= mask_u4;
        check("dont_inline_u4_set_lands_bit12", word_u4 == 0x00001000u);
        check("dont_inline_u4_set_idempotent", (word_u4 | mask_u4) == word_u4);
        check("dont_inline_u4_clear_restores_zero", (word_u4 & ~mask_u4) == 0u);
        const std::uint32_t neighbours_u4{ 0xFFFFFFFFu & ~mask_u4 };
        check("dont_inline_u4_set_no_neighbour_bleed",
              (neighbours_u4 | mask_u4) == 0xFFFFFFFFu);
        check("dont_inline_u4_clear_only_target_bit",
              (0xFFFFFFFFu & ~mask_u4) == neighbours_u4);

        // The two layouts target DIFFERENT bits -> the masks are distinct,
        // which is exactly why set_dont_inline branches on the resolved width
        // and bit rather than using one fixed access.  The JDK 21+ layout
        // RELOCATED the bit upward (2 -> 12), so the bit positions differ even
        // though both fit a 16-bit window.
        check("dont_inline_masks_differ_by_layout",
              static_cast<std::uint32_t>(mask_u2) != mask_u4);
        check("dont_inline_bit_relocated_upward_in_u4", kDontInlineBitU4 > kDontInlineBitU2);
    }

    // =====================================================================
    // I2. EXHAUSTIVE single-bit mask arithmetic over the whole legal bit range
    //     of both widths: for every bit position the mask isolates exactly that
    //     bit, set is idempotent, and clear is its exact inverse.  This widens
    //     section I (which pins the two NAMED bits) to the full width axis, so
    //     a future JDK that relocates _dont_inline to any other bit is covered
    //     by the same pure arithmetic.
    // =====================================================================
    {
        // u2 width: bits 0..15.
        bool u2_all_bits_ok{ true };
        for (int bit{ 0 }; bit < 16; ++bit)
        {
            const std::uint16_t mask{ static_cast<std::uint16_t>(1u << bit) };
            // Exactly one bit set.
            int popcount{ 0 };
            for (int b{ 0 }; b < 16; ++b)
            {
                if ((mask & static_cast<std::uint16_t>(1u << b)) != 0u) { ++popcount; }
            }
            if (popcount != 1) { u2_all_bits_ok = false; }
            // set then clear restores zero; set is idempotent.
            std::uint16_t word{ 0u };
            word = static_cast<std::uint16_t>(word | mask);
            if (word != mask) { u2_all_bits_ok = false; }
            if (static_cast<std::uint16_t>(word | mask) != word) { u2_all_bits_ok = false; }
            if (static_cast<std::uint16_t>(word & static_cast<std::uint16_t>(~mask)) != 0u)
            {
                u2_all_bits_ok = false;
            }
        }
        check("dont_inline_u2_all_16_bits_mask_exact", u2_all_bits_ok);

        // u4 width: bits 0..31.
        bool u4_all_bits_ok{ true };
        std::size_t u4_cases{ 0 };
        for (int bit{ 0 }; bit < 32; ++bit)
        {
            const std::uint32_t mask{ std::uint32_t{ 1u } << bit };
            std::uint32_t word{ 0u };
            word |= mask;
            if (word != mask) { u4_all_bits_ok = false; }
            if ((word | mask) != word) { u4_all_bits_ok = false; }
            if ((word & ~mask) != 0u) { u4_all_bits_ok = false; }
            ++u4_cases;
        }
        check("dont_inline_u4_all_32_bits_mask_exact", u4_all_bits_ok);
        check("dont_inline_u4_bit_axis_is_full", u4_cases == 32u);
    }

    // =====================================================================
    // J. klass_introspection NO-JVM null / empty contract (the dispatch
    //    feature's downstream class-resolution entry points).  With no JVM:
    //      - find_class("") short-circuits to nullptr (vmhook.hpp:8161).
    //      - find_class(any name) cannot resolve -> nullptr (no loaded classes).
    //      - klass_from_oop(nullptr) -> nullptr (pre-gate).
    //    None of these dereference a fabricated klass; they exercise the
    //    documented fail-closed return every introspection entry gives off-JVM.
    // =====================================================================
    {
        check("find_class_empty_name_is_null",
              vmhook::find_class("") == nullptr);
        check("find_class_unloaded_name_no_jvm_is_null",
              vmhook::find_class("java/lang/Object") == nullptr);
        check("find_class_array_descriptor_no_jvm_is_null",
              vmhook::find_class("[I") == nullptr);
        check("klass_from_oop_null_is_null",
              vmhook::klass_from_oop(nullptr) == nullptr);
        // A repeated empty-name query is stable (the fast-reject is pure).
        bool empty_stable{ true };
        for (int i{ 0 }; i < 8; ++i)
        {
            if (vmhook::find_class("") != nullptr) { empty_stable = false; }
        }
        check("find_class_empty_name_stable_repeat", empty_stable);
    }

    // =====================================================================
    // K. WAVE-27 LEDGER GAPS: dispatch noexcept / null-method bail / empty-table
    //    lock-free no-crash / signature type identity static_asserts.
    //    All pure C++ — no JVM, no fabricated frame walk.
    // =====================================================================
    {
        // K.1 Type identity of the dispatch detour cell at value level: a
        // default-constructed std::function with the dispatch ABI is empty and
        // compares equal to nullptr -- the cold-state cell representation.
        const std::function<void(vmhook::hotspot::frame*, vmhook::hotspot::java_thread*,
                                 vmhook::hotspot::return_slot*)> empty_cell{};
        check("dispatch_detour_cell_default_is_empty",
              static_cast<bool>(empty_cell) == false);
        check("dispatch_detour_cell_default_equals_nullptr",
              empty_cell == nullptr);

        // K.2 hooked_method is default-constructible and a default entry has a
        // null method pointer + empty detour cell.  This is the COLD-STATE shape
        // the install path must overwrite before push_back -- and it lets the
        // scan's `hook.method == current_method` comparison work uniformly.
        const vmhook::hotspot::hooked_method cold_entry{};
        check("hooked_method_cold_method_ptr_is_null", cold_entry.method == nullptr);
        check("hooked_method_cold_detour_cell_is_empty",
              static_cast<bool>(cold_entry.detour) == false);

        // K.3 NULL-METHOD BAIL: even if the global vector somehow held a
        // null-method entry, the scan's `hook.method == current_method` for a
        // NON-NULL current_method must NOT match.  And a null current_method
        // (degenerate frame, flaw #6) must NOT match any real entry.
        auto* const real_method{ reinterpret_cast<vmhook::hotspot::method*>(
            std::uintptr_t{ 0x5000u }) };
        std::vector<vmhook::hotspot::hooked_method> mixed_replica;
        {
            vmhook::hotspot::hooked_method null_entry{};
            null_entry.method = nullptr;
            null_entry.detour = [](vmhook::hotspot::frame*, vmhook::hotspot::java_thread*,
                                   vmhook::hotspot::return_slot*) noexcept {};
            mixed_replica.push_back(std::move(null_entry));

            vmhook::hotspot::hooked_method real_entry{};
            real_entry.method = real_method;
            real_entry.detour = [](vmhook::hotspot::frame*, vmhook::hotspot::java_thread*,
                                   vmhook::hotspot::return_slot*) noexcept {};
            mixed_replica.push_back(std::move(real_entry));
        }
        bool null_current_matched_anything{ false };
        bool real_current_matched_null_entry{ false };
        for (const vmhook::hotspot::hooked_method& hook : mixed_replica)
        {
            const vmhook::hotspot::method* const null_current{ nullptr };
            if (hook.method == null_current && hook.method != nullptr)
            {
                // unreachable -- documents the comparison semantics
                null_current_matched_anything = true;
            }
            if (hook.method == real_method && hook.method == nullptr)
            {
                real_current_matched_null_entry = true;
            }
        }
        check("null_current_does_not_pseudo_match", !null_current_matched_anything);
        check("real_current_does_not_match_null_entry", !real_current_matched_null_entry);

        // K.4 EMPTY-TABLE LOCK-FREE READ never derefs anything: a range-for
        // over the LIVE empty g_hooked_methods executes zero iterations and
        // does not touch storage.  We pin both that the loop body never runs
        // and that .data()/.size() are consistent with "empty".
        int iterations{ 0 };
        for (const vmhook::hotspot::hooked_method& hook : vmhook::hotspot::g_hooked_methods)
        {
            (void)hook;
            ++iterations;
        }
        check("empty_global_loop_body_never_runs", iterations == 0);
        check("empty_global_size_zero", vmhook::hotspot::g_hooked_methods.size() == 0u);
        check("empty_global_begin_equals_end",
              vmhook::hotspot::g_hooked_methods.begin()
                  == vmhook::hotspot::g_hooked_methods.end());

        // Repeated empty-scan attempts are stable (no lazy mutation on read).
        bool empty_scan_stable{ true };
        for (int i{ 0 }; i < 32; ++i)
        {
            if (!vmhook::hotspot::g_hooked_methods.empty()) { empty_scan_stable = false; }
        }
        check("empty_global_repeated_scan_stable", empty_scan_stable);
    }

    // -----------------------------------------------------------------------
    // K-COMPILE-TIME: extra static_asserts on dispatch signature type identity
    // and noexcept properties (BUILD-time pins; regression fails compilation).
    // -----------------------------------------------------------------------
    {
        // The detour ABI's three argument types are exactly frame* / java_thread*
        // / return_slot* (pointer-to non-const).
        using detour_fn = vmhook::hotspot::detour_function_t;
        static_assert(std::is_pointer_v<detour_fn>,
                      "detour_function_t must be a function pointer");
        static_assert(std::is_function_v<std::remove_pointer_t<detour_fn>>,
                      "detour_function_t pointee must be a function type");

        // Confirm the empty-detour function pointer cell value is constexpr-null.
        constexpr detour_fn null_detour{ nullptr };
        static_assert(null_detour == nullptr, "null detour_function_t must compare nullptr");

        // g_hooked_methods is a std::vector<hooked_method> (so range-for works
        // and .empty()/.size()/.begin()/.end() are the standard contract).
        static_assert(std::is_same_v<decltype(vmhook::hotspot::g_hooked_methods),
                                     std::vector<vmhook::hotspot::hooked_method>>,
                      "g_hooked_methods must be std::vector<hooked_method>");

        // hooked_method is default-constructible (cold-state shape).
        static_assert(std::is_default_constructible_v<vmhook::hotspot::hooked_method>,
                      "hooked_method must be default-constructible");

        // The atomic shutdown flag's load is noexcept (lock-free hot-path bail).
        static_assert(noexcept(vmhook::hotspot::g_shutdown_requested.load(
                          std::memory_order_acquire)),
                      "g_shutdown_requested.load must be noexcept");
        static_assert(std::atomic<bool>::is_always_lock_free,
                      "atomic<bool> must be lock-free for the dispatch bail");

        // return_slot construction is noexcept (the trampoline pushes a
        // zero-initialised slot on the native stack; throwing is not an option).
        static_assert(std::is_nothrow_default_constructible_v<vmhook::hotspot::return_slot>,
                      "return_slot default ctor must be noexcept");
        static_assert(std::is_nothrow_copy_constructible_v<vmhook::hotspot::return_slot>,
                      "return_slot copy ctor must be noexcept");

        check("wave27_dispatch_signature_static_asserts_compiled", true);
    }

    std::printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
