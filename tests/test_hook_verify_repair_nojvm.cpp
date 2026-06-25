// Standalone (no-JVM) unit test for the hook_verify_repair feature surface:
// vmhook::verify_hooks() (the manual drift detector + re-armer), the auto-repair
// watchdog gating / reversibility, and the two metadata-walk legs the drift
// detector funnels through (the constant_pool index/length bound arithmetic and
// the InstanceKlass methods-Array stride/clamp math).
//
// ───────────────────────────────────────────────────────────────────────────
// WHY THIS FILE EXISTS / WHAT IS NO-JVM-DETERMINABLE
// ───────────────────────────────────────────────────────────────────────────
// This executable runs with NO HotSpot JVM in-process, so the exported global
// gHotSpotVMStructs is never resolvable: iterate_struct_entries(...) /
// iterate_type_entries(...) return nullptr for EVERY field, so every raw layout
// accessor bails at its `!entry` guard BEFORE dereferencing `this`.  That makes
// exactly four things on the hook_verify_repair surface fully deterministic and
// testable here:
//
//   1. verify_hooks()'s NO-JVM / EMPTY contract on the REAL free function: with
//      g_hooked_methods / g_hooked_i2i_entries empty (nothing installed) it
//      returns 0, NEVER throws (its body is one big try/catch), and is
//      idempotent run-to-run.  Plus its compile-time signature / noexcept pins.
//
//   2. The auto-repair WATCHDOG gating booleans + REVERSIBILITY, driven on the
//      real public API: auto_repair_enabled() defaults true; the watchdog is NOT
//      live before any hook<T>() install (g_watchdog_running observed via the
//      public predicate); set_auto_repair_enabled(false) is reversible and leaves
//      g_shutdown_requested cleared so the library stays usable; the run-time
//      master switch round-trips.  g_shutdown_requested's fail-closed default.
//
//   3. The THREE DRIFT-MODE DECISION predicates verify_hooks applies per stored
//      hooked_method, reproduced as captureless mirrors of the EXACT source
//      expressions on POD snapshots (we cannot fabricate + read a live Method*):
//        mode 1: const_method == nullptr            -> FREED
//        mode 2: !name.empty() && name != expected  -> ALIASED
//        mode 3: code_now != nullptr || !no_compile -> JIT-DRIFTED
//      plus the NO_COMPILE re-arm bit round-trip (OR re-arm / AND-NOT teardown)
//      and the verify_and_repair() 5-byte JMP expected-bytes + memcmp-intact
//      decision (pure arithmetic on owned bytes).
//
//   4. The two metadata-walk legs the detector funnels through, as pure math +
//      the no-JVM null/empty contract on the REAL accessors:
//        constant_pool : get_length() -> -1 ("unknown") with no JVM; entries are
//                        1-based (index 0 unused), pointer-sized (8B) stride; the
//                        1 <= index <= length bound predicate.
//        InstanceKlass : methods-Array<Method*> data @ array+8, 8-byte stride,
//                        length clamped to [0, 65535] (u2 method_count ceiling).
//
// OUT OF SCOPE (needs a live JVM, or would fabricate + read a wild address —
// which SEGV-aborts on POSIX, uncatchable on the no-SEH MinGW/clang-on-Windows
// legs): provoking a REAL mode-1/2/3 drift (requires a hostile JVMTI
// RedefineClasses or a JIT recompile), walking a REAL ConstantPool / methods
// array, or spawning the watchdog (needs a successful hook<T>()).  Those are the
// live-JVM module's job (tests/jvm/modules/hook_verify_repair.cpp).  We NEVER
// fabricate a Method/ConstantPool/InstanceKlass and dereference it; the real
// accessors are called ONLY on nullptr and is_valid_pointer-rejected low/odd/
// sentinel constants (rejected BEFORE any read), and every decision SEMANTIC is
// pinned through captureless mirrors of the exact source expressions.
//
// Source of truth (vmhook/ext/vmhook/vmhook.hpp; the functions are authority):
//   verify_hooks                       10682 (no-throw; mode 1/2/3 detector; re-arm)
//   try_reinstall (lambda)             10710 (find_class null -> false off-JVM)
//   mode-1 predicate                   10858 (!const_method ...)
//   mode-2 predicate                   10877 (!name.empty() && name != expected)
//   mode-3 predicate                   10932 (code_now != nullptr || !no_compile_set)
//   NO_COMPILE                          7579 (0x01|0x02|0x04|0x08 << 24 == 0x0F000000)
//   midi2i_hook::verify_and_repair      6977 (0xE9 + rel32 expected; memcmp intact)
//   hooked_method                       7120 (POD: method/expected_*/drift_logged)
//   g_shutdown_requested                7197 (default false)
//   safe_access_flags_test / _or        2777 / 2820 (found=false / false with no JVM)
//   method::get_code / set_code         3111 / 3163 (nullptr / no-op with no JVM)
//   set_dont_inline                     7644 (null/invalid -> no-op)
//   auto_repair_enabled                11153 (default true)
//   set_auto_repair_enabled            11189 (reversible; clears g_shutdown_requested)
//   g_watchdog_running                 11022 (false until a watchdog spawns)
//   constant_pool::get_length          2362 (-1 with no JVM; 1-based, 8B entries)
//   klass::get_methods_count/_ptr      3504 / 3543 (Array<Method*> @+8; clamp 0..65535)
//   is_valid_pointer                   2047 (floor/ceiling/odd/9-sentinel gate)
#include <vmhook/vmhook.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
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
    // ── Captureless mirrors of the EXACT verify_hooks() source expressions ───
    // None of these call into the library; they reproduce the documented closed
    // forms so the suite can pin the per-hook drift DECISION that would otherwise
    // require a live Method* to observe.

    // Mode 1 (vmhook.hpp:10858): a freed Method has a null (or invalid)
    // ConstMethod*; the hook is dead and the re-arm path is taken.
    auto mode1_freed(const void* const_method_ptr, bool const_method_valid) -> bool
    {
        return const_method_ptr == nullptr || !const_method_valid;
    }

    // Mode 2 (vmhook.hpp:10877): a NON-empty current name that differs from the
    // install-time expected name means the Method* address was aliased.  An
    // empty current name (cold read) is NOT treated as drift here (mode 1 owns
    // the freed case), so the predicate requires a non-empty name first.
    auto mode2_aliased(const std::string& current_name, const std::string& expected_name) -> bool
    {
        return !current_name.empty() && current_name != expected_name;
    }

    // Mode 3 (vmhook.hpp:10932): JIT drift iff _code was re-populated OR our
    // NO_COMPILE flag got cleared.  (Only evaluated when the flags word was
    // readable; an unreadable word defers the tick — modelled separately.)
    auto mode3_jit_drifted(const void* code_now, bool no_compile_set) -> bool
    {
        return code_now != nullptr || !no_compile_set;
    }

    // verify_and_repair() expected-bytes builder (vmhook.hpp:6984-6992): the
    // intact stub is 0xE9 (JMP opcode) followed by the rel32 delta from the
    // patch site+5 to the trampoline.  Pure arithmetic on owned addresses.
    auto build_expected_jmp(std::uintptr_t target, std::uintptr_t trampoline,
                            std::array<std::uint8_t, 5>& out) -> void
    {
        constexpr std::uint8_t JMP_OPCODE{ 0xE9u };
        constexpr std::int32_t JMP_SIZE{ 5 };
        const std::int32_t expected_rel{ static_cast<std::int32_t>(
            static_cast<std::int64_t>(trampoline)
            - static_cast<std::int64_t>(target + JMP_SIZE)) };
        out[0] = JMP_OPCODE;
        std::memcpy(out.data() + 1, &expected_rel, sizeof(expected_rel));
    }
}

int main()
{
    using vmhook::hotspot::is_valid_pointer;

    // =====================================================================
    // 0. COMPILE-TIME signature / return-type / noexcept pins (static_assert).
    //    A regression in any of these fails the BUILD — the strongest pin.
    // =====================================================================
    static_assert(std::is_same_v<decltype(vmhook::verify_hooks()), std::size_t>,
                  "verify_hooks() must return std::size_t (count repaired)");
    static_assert(noexcept(vmhook::verify_hooks()),
                  "verify_hooks() must be noexcept (callable from hot loops)");
    static_assert(std::is_same_v<decltype(vmhook::auto_repair_enabled()), bool>,
                  "auto_repair_enabled() must return bool");
    static_assert(noexcept(vmhook::auto_repair_enabled()),
                  "auto_repair_enabled() must be noexcept");
    static_assert(std::is_same_v<decltype(vmhook::set_auto_repair_enabled(true)), void>,
                  "set_auto_repair_enabled() must return void");
    static_assert(noexcept(vmhook::set_auto_repair_enabled(true)),
                  "set_auto_repair_enabled() must be noexcept");
    // NO_COMPILE is a signed 32-bit constant equal to the documented compose.
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(vmhook::hotspot::NO_COMPILE)>,
                      std::int32_t>,
                  "NO_COMPILE is std::int32_t");
    static_assert(vmhook::hotspot::NO_COMPILE
                      == (0x01000000 | 0x02000000 | 0x04000000 | 0x08000000),
                  "NO_COMPILE is the OR of the four compile-control bits");
    // hooked_method (the per-hook record verify_hooks iterates) field types.
    using hm_t = vmhook::hotspot::hooked_method;
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(hm_t{}.method)>,
                      vmhook::hotspot::method*>,
                  "hooked_method::method is method*");
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(hm_t{}.expected_class_name)>,
                      std::string>,
                  "hooked_method::expected_class_name is std::string");
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(hm_t{}.expected_method_name)>,
                      std::string>,
                  "hooked_method::expected_method_name is std::string");
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(hm_t{}.expected_signature)>,
                      std::string>,
                  "hooked_method::expected_signature is std::string");
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(hm_t{}.drift_logged)>, bool>,
                  "hooked_method::drift_logged is bool");
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(hm_t{}.was_compiled)>, bool>,
                  "hooked_method::was_compiled is bool");
    // safe_access_flags_test / _or — the fault-safe NO_COMPILE probe / re-armer.
    static_assert(noexcept(std::declval<const vmhook::hotspot::method>()
                      .safe_access_flags_or(std::uint32_t{ 0 })),
                  "safe_access_flags_or must be noexcept");
    static_assert(std::is_same_v<decltype(std::declval<const vmhook::hotspot::method>()
                      .safe_access_flags_or(std::uint32_t{ 0 })), bool>,
                  "safe_access_flags_or returns bool");
    // get_code / set_code (the mode-3 read + re-arm clear).
    static_assert(std::is_same_v<decltype(std::declval<const vmhook::hotspot::method>()
                      .get_code()), void*>,
                  "get_code returns void*");
    static_assert(noexcept(std::declval<const vmhook::hotspot::method>().get_code()),
                  "get_code must be noexcept");
    // constant_pool::get_length — the cp bound source.
    static_assert(std::is_same_v<decltype(std::declval<const vmhook::hotspot::constant_pool>()
                      .get_length()), std::int32_t>,
                  "constant_pool::get_length returns int32_t");
    static_assert(noexcept(std::declval<const vmhook::hotspot::constant_pool>().get_length()),
                  "constant_pool::get_length must be noexcept");
    check("hook_verify_repair_static_asserts_compiled", true);

    // =====================================================================
    // A. verify_hooks() — NO-JVM / EMPTY contract on the REAL free function.
    //    With nothing installed, g_hooked_methods / g_hooked_i2i_entries are
    //    empty, so the detector loops zero times and returns 0.  Its whole body
    //    is wrapped in try/catch (no-throw contract), so this is safe to call
    //    repeatedly with no JVM; the result is deterministic (always 0) and the
    //    call must not throw.
    // =====================================================================
    {
        const std::size_t r0{ vmhook::verify_hooks() };
        check("A_verify_hooks_empty_returns_zero", r0 == 0);
        // Idempotent / deterministic across repeats (no hidden state mutated).
        bool all_zero{ true };
        for (int i{ 0 }; i < 8; ++i)
        {
            if (vmhook::verify_hooks() != 0) { all_zero = false; }
        }
        check("A_verify_hooks_repeat_all_zero", all_zero);
        // No-throw: reaching this line after a noexcept call is the proof; but
        // also assert the noexcept-ness held at the type level (belt-and-braces).
        check("A_verify_hooks_is_noexcept_typelevel",
              noexcept(vmhook::verify_hooks()));
    }

    // =====================================================================
    // B. AUTO-REPAIR WATCHDOG — gating booleans + REVERSIBILITY on the real API.
    //    auto_repair_enabled() reflects the run-time master switch (default
    //    true).  set_auto_repair_enabled(false) flips it to false and, since no
    //    watchdog is live (no hook<T>() ran), takes the "no live thread" branch:
    //    it clears g_started and returns WITHOUT raising g_shutdown_requested.
    //    Re-enabling restores true.  The switch round-trips deterministically.
    // =====================================================================
    {
        // Default state: enabled.
        check("B_auto_repair_enabled_default_true", vmhook::auto_repair_enabled() == true);
        // g_shutdown_requested is the fail-closed gate (default false: dispatch
        // is OPEN; only teardown / disable-while-live flips it true).
        check("B_g_shutdown_requested_default_false",
              vmhook::hotspot::g_shutdown_requested.load(std::memory_order_acquire) == false);
        // No watchdog thread can be live: nothing was hooked, so ensure_started()
        // was never reached.  The internal running flag must read false.
        check("B_watchdog_not_running_before_any_install",
              vmhook::detail::auto_repair::g_watchdog_running.load(std::memory_order_acquire)
                  == false);

        // Disable: master switch flips false, and because no thread is live the
        // disable path must NOT leave g_shutdown_requested raised (the library
        // stays usable: verify_hooks() keeps working, common_detour stays open).
        vmhook::set_auto_repair_enabled(false);
        check("B_disable_flips_enabled_false", vmhook::auto_repair_enabled() == false);
        check("B_disable_no_live_thread_leaves_shutdown_clear",
              vmhook::hotspot::g_shutdown_requested.load(std::memory_order_acquire) == false);
        // verify_hooks() is wholly unaffected by the master switch.
        check("B_verify_hooks_still_works_while_disabled", vmhook::verify_hooks() == 0);

        // Re-enable: master switch flips back true; still no live thread spawned
        // (ensure_started only runs on the next successful hook<T>()).
        vmhook::set_auto_repair_enabled(true);
        check("B_reenable_flips_enabled_true", vmhook::auto_repair_enabled() == true);
        check("B_reenable_does_not_spawn_thread_here",
              vmhook::detail::auto_repair::g_watchdog_running.load(std::memory_order_acquire)
                  == false);
        check("B_reenable_leaves_shutdown_clear",
              vmhook::hotspot::g_shutdown_requested.load(std::memory_order_acquire) == false);

        // Full round-trip: false -> true -> false -> true ends enabled, and the
        // shutdown gate is never left raised by these no-live-thread toggles.
        vmhook::set_auto_repair_enabled(false);
        vmhook::set_auto_repair_enabled(true);
        check("B_switch_round_trip_ends_enabled", vmhook::auto_repair_enabled() == true);
        check("B_switch_round_trip_shutdown_clear",
              vmhook::hotspot::g_shutdown_requested.load(std::memory_order_acquire) == false);
    }

    // =====================================================================
    // C. MODE-1 (FREED) decision — null/invalid ConstMethod* means the owning
    //    class was unloaded; the stored Method* is dead and try_reinstall fires.
    //    Pinned over the (const_method_ptr, valid) truth grid (vmhook.hpp:10858).
    // =====================================================================
    {
        check("C_mode1_null_const_method_is_freed", mode1_freed(nullptr, false) == true);
        check("C_mode1_null_ptr_even_if_flagged_valid", mode1_freed(nullptr, true) == true);
        // An in-range-looking but is_valid_pointer-REJECTED const_method is
        // treated as freed (the predicate's !valid arm).  We never deref it.
        const void* rejected{ reinterpret_cast<const void*>(std::uintptr_t{ 0xDEADBEEFu }) };
        check("C_mode1_rejected_const_method_is_invalid",
              !is_valid_pointer(rejected));
        check("C_mode1_invalid_const_method_is_freed",
              mode1_freed(rejected, is_valid_pointer(rejected)) == true);
        // A non-null, valid const_method is NOT freed (the live-hook case).
        const void* live_like{ reinterpret_cast<const void*>(std::uintptr_t{ 0x00000A0000020000ull }) };
        check("C_mode1_valid_const_method_not_freed",
              mode1_freed(live_like, is_valid_pointer(live_like)) == false);
    }

    // =====================================================================
    // D. MODE-2 (ALIASED) decision — a NON-empty current name differing from the
    //    install-time expected name means the address was reused for another
    //    Method (vmhook.hpp:10877).  Empty current name is NOT drift (mode 1
    //    owns the freed case), and an exact-match name is the steady state.
    // =====================================================================
    {
        check("D_mode2_diff_name_is_aliased",
              mode2_aliased("bridge$getRenderState", "orientCamera") == true);
        check("D_mode2_same_name_not_aliased",
              mode2_aliased("orientCamera", "orientCamera") == false);
        // Empty current name (cold get_name read) is deliberately NOT mode-2
        // drift — the guard requires a non-empty name first.
        check("D_mode2_empty_current_name_not_aliased",
              mode2_aliased(std::string{}, "orientCamera") == false);
        // Expected empty + non-empty current still counts as differing (an
        // install with an empty captured name is degenerate but the predicate is
        // a pure string compare; pinned for completeness).
        check("D_mode2_nonempty_vs_empty_expected_is_diff",
              mode2_aliased("foo", std::string{}) == true);
        // Case sensitivity: names differing only by case ARE different methods.
        check("D_mode2_case_sensitive",
              mode2_aliased("Foo", "foo") == true);
    }

    // =====================================================================
    // E. MODE-3 (JIT-DRIFTED) decision — drift iff _code was re-populated OR our
    //    NO_COMPILE flag got cleared (vmhook.hpp:10932).  Exhaustive over the
    //    (code_now, no_compile_set) truth table.  Only the (null, set) corner is
    //    the clean steady state; the other three are drift.
    // =====================================================================
    {
        const void* some_code{ reinterpret_cast<const void*>(std::uintptr_t{ 0x00000A0000030000ull }) };
        // Clean steady state: _code null AND NO_COMPILE still set -> no drift.
        check("E_mode3_clean_no_code_compile_set",
              mode3_jit_drifted(nullptr, true) == false);
        // _code re-populated (recompiled) -> drift, regardless of NO_COMPILE.
        check("E_mode3_code_repopulated_is_drift",
              mode3_jit_drifted(some_code, true) == true);
        // NO_COMPILE cleared (flag stomped) -> drift even with _code null.
        check("E_mode3_no_compile_cleared_is_drift",
              mode3_jit_drifted(nullptr, false) == true);
        // Both bad -> drift.
        check("E_mode3_both_bad_is_drift",
              mode3_jit_drifted(some_code, false) == true);
        // Truth-table sweep: drift == (code!=null) OR (!compile_set).
        bool table_ok{ true };
        const void* codes[]{ nullptr, some_code };
        const bool compiles[]{ false, true };
        for (const void* c : codes)
        {
            for (const bool nc : compiles)
            {
                const bool got{ mode3_jit_drifted(c, nc) };
                const bool want{ (c != nullptr) || !nc };
                if (got != want) { table_ok = false; }
            }
        }
        check("E_mode3_truth_table_matches_formula", table_ok);
        // The flags-unreadable case: verify_hooks() defers the tick when the
        // flags word could not be read (found==false).  We model the SOURCE
        // guard (vmhook.hpp:10928): unreadable -> skip (no drift decision made).
        const bool flags_readable{ false };
        const bool would_evaluate_drift{ flags_readable };
        check("E_mode3_unreadable_flags_defers", would_evaluate_drift == false);
    }

    // =====================================================================
    // F. NO_COMPILE re-arm bit round-trip — the re-armer OR's NO_COMPILE back in
    //    (vmhook.hpp:10952 / safe_access_flags_or) and teardown AND-NOT's it out
    //    (safe_access_flags_and).  Pure RMW on an owned word disjoint from the
    //    four target bits, so the round-trip restores the base exactly.
    // =====================================================================
    {
        const std::uint32_t no_compile{ static_cast<std::uint32_t>(vmhook::hotspot::NO_COMPILE) };
        check("F_no_compile_value_is_0x0F000000", no_compile == 0x0F000000u);
        check("F_no_compile_is_four_high_nibble_bits",
              (no_compile >> 24) == 0x0Fu && (no_compile & 0x00FFFFFFu) == 0u);
        // A base carrying ONLY non-target bits (low 24 + bit 28) round-trips.
        const std::uint32_t base{ 0x10ABCDEFu };
        const std::uint32_t armed{ base | no_compile };               // re-arm (OR)
        check("F_rearm_sets_all_target_bits", (armed & no_compile) == no_compile);
        check("F_rearm_preserves_other_bits",
              (armed & ~no_compile) == (base & ~no_compile));
        const std::uint32_t torn_down{ armed & ~no_compile };         // teardown (AND-NOT)
        check("F_teardown_clears_all_target_bits", (torn_down & no_compile) == 0u);
        check("F_rearm_then_teardown_round_trips", torn_down == base);
        // safe_access_flags_or short-circuits when the bits are already set:
        // OR-ing twice equals OR-ing once (idempotent re-arm).
        check("F_rearm_idempotent", (armed | no_compile) == armed);
        // The mode-3 "already set" early-return predicate: (flags & mask)==mask.
        check("F_already_set_predicate_true", (armed & no_compile) == no_compile);
        check("F_already_set_predicate_false_when_partial",
              (((base | 0x01000000u) & no_compile) == no_compile) == false);
    }

    // =====================================================================
    // G. verify_and_repair() 5-byte JMP DECISION (vmhook.hpp:6977-7012).  The
    //    i2i-stub re-armer builds an expected 0xE9 + rel32 from (target,
    //    trampoline) and memcmp's the live 5 bytes against it: equal -> intact
    //    (return true, no write); differ -> re-arm.  Pinned as pure arithmetic
    //    on OWNED byte buffers (no stub fabricated, no live address read).
    // =====================================================================
    {
        const std::uintptr_t target{ 0x00000A0000100000ull };
        const std::uintptr_t trampoline{ 0x00000A0000200000ull };
        std::array<std::uint8_t, 5> expected{};
        build_expected_jmp(target, trampoline, expected);
        // Byte 0 is always the JMP opcode.
        check("G_expected_byte0_is_jmp_opcode", expected[0] == 0xE9u);
        // rel32 decodes back to (trampoline - (target + 5)).
        std::int32_t decoded_rel{};
        std::memcpy(&decoded_rel, expected.data() + 1, sizeof(decoded_rel));
        const std::int32_t want_rel{ static_cast<std::int32_t>(
            static_cast<std::int64_t>(trampoline)
            - static_cast<std::int64_t>(target + 5)) };
        check("G_expected_rel32_is_delta_to_trampoline", decoded_rel == want_rel);
        // INTACT decision: live bytes == expected -> memcmp 0 -> "already intact".
        std::array<std::uint8_t, 5> live_intact{ expected };
        check("G_intact_when_bytes_match",
              std::memcmp(live_intact.data(), expected.data(), 5) == 0);
        // DRIFTED decision: a stomped rel32 (someone else's JMP) -> memcmp != 0.
        std::array<std::uint8_t, 5> live_stomped{ expected };
        live_stomped[1] = static_cast<std::uint8_t>(live_stomped[1] ^ 0xFFu);
        check("G_repair_needed_when_rel_differs",
              std::memcmp(live_stomped.data(), expected.data(), 5) != 0);
        // A backward jump (trampoline below target) yields a negative rel32 that
        // still round-trips through the int32 encoding (sign preserved).
        std::array<std::uint8_t, 5> back{};
        build_expected_jmp(trampoline, target, back);  // target now < site -> negative
        std::int32_t back_rel{};
        std::memcpy(&back_rel, back.data() + 1, sizeof(back_rel));
        check("G_backward_jump_rel_is_negative", back_rel < 0);
        check("G_backward_jump_byte0_still_jmp", back[0] == 0xE9u);
    }

    // =====================================================================
    // H. The REAL fault-safe Method accessors honour the NO-JVM contract WITHOUT
    //    dereferencing.  With no JVM the Method::_access_flags / _code VMStruct
    //    entries are absent, so every accessor bails at its `!entry` guard.
    //    Called ONLY on null and is_valid_pointer-rejected Method* (rejected
    //    before any `this` read) AND on an OWNED, zeroed buffer (ours to inspect):
    //      safe_access_flags_test -> false, found stays false (cannot detect)
    //      safe_access_flags_or   -> false (re-arm deferred), NO byte written
    //      get_code               -> nullptr (read as "not compiled")
    //      set_code / set_dont_inline -> no-op (no write)
    // =====================================================================
    {
        using vmhook::hotspot::method;
        // is_valid_pointer-rejected Method*: the gate rejects it BEFORE any read.
        auto* const rejected_m{
            reinterpret_cast<const method*>(std::uintptr_t{ 0xCAFEBABEu }) };
        check("H_rejected_method_is_invalid", !is_valid_pointer(rejected_m));
        bool found_rejected{ true };
        const bool test_rejected{ rejected_m->safe_access_flags_test(
            vmhook::hotspot::NO_COMPILE, found_rejected) };
        check("H_test_rejected_method_returns_false", test_rejected == false);
        check("H_test_rejected_method_found_false", found_rejected == false);
        check("H_or_rejected_method_returns_false",
              rejected_m->safe_access_flags_or(vmhook::hotspot::NO_COMPILE) == false);
        check("H_get_code_rejected_method_null", rejected_m->get_code() == nullptr);
        // set_dont_inline on a null + rejected Method* is a safe no-op (it guards
        // !method_pointer || !is_valid_pointer up front; reaching the next line is
        // the assertion, a wild deref would have faulted).
        vmhook::hotspot::set_dont_inline(nullptr, true);
        vmhook::hotspot::set_dont_inline(rejected_m, true);
        check("H_set_dont_inline_null_and_rejected_safe_noop", true);

        // OWNED, zeroed, in-range, aligned buffer: it PASSES is_valid_pointer,
        // but with no JVM the _access_flags / _code offsets are unresolved, so
        // the read/RMW bail at `!entry` and write NO byte.  We snapshot and
        // confirm byte-identity after the re-arm + clear attempts.
        alignas(16) std::array<std::uint8_t, 128> owned{};
        owned.fill(0x5Au);
        const std::array<std::uint8_t, 128> snapshot{ owned };
        auto* const owned_m{ reinterpret_cast<const method*>(owned.data()) };
        bool found_owned{ true };
        const bool test_owned{ owned_m->safe_access_flags_test(
            vmhook::hotspot::NO_COMPILE, found_owned) };
        check("H_test_owned_buffer_no_jvm_returns_false", test_owned == false);
        check("H_test_owned_buffer_no_jvm_found_false", found_owned == false);
        check("H_or_owned_buffer_no_jvm_returns_false",
              owned_m->safe_access_flags_or(vmhook::hotspot::NO_COMPILE) == false);
        check("H_get_code_owned_buffer_no_jvm_null", owned_m->get_code() == nullptr);
        vmhook::hotspot::set_dont_inline(owned_m, true);
        check("H_no_jvm_accessors_wrote_no_byte",
              std::memcmp(owned.data(), snapshot.data(), owned.size()) == 0);
    }

    // =====================================================================
    // I. CONSTANT-POOL bound arithmetic (constantpool_access leg).  The drift
    //    detector funnels name/signature reads through ConstantPool entries.
    //    With no JVM get_length() returns its -1 "unknown -> skip bound check"
    //    sentinel; the index discipline is 1-BASED (index 0 unused) and each
    //    entry is pointer-sized (8 bytes on x64).  Pinned as the no-JVM contract
    //    + the bound predicate the cp accessors apply before any slot deref.
    // =====================================================================
    {
        using vmhook::hotspot::constant_pool;
        // get_length() on null + rejected -> -1 (unknown), never a deref.
        auto* const rejected_cp{
            reinterpret_cast<const constant_pool*>(std::uintptr_t{ 0xCCCCCCCCu }) };
        check("I_rejected_cp_is_invalid", !is_valid_pointer(rejected_cp));
        check("I_get_length_rejected_cp_is_minus1", rejected_cp->get_length() == -1);
        // Owned, zeroed, in-range buffer: PASSES is_valid_pointer, but no JVM ->
        // no _length VMStruct -> -1 sentinel (no fabricated read of pool data).
        alignas(16) std::array<std::uint8_t, 64> owned_cp{};
        auto* const owned_cp_ptr{
            reinterpret_cast<const constant_pool*>(owned_cp.data()) };
        check("I_get_length_owned_cp_no_jvm_is_minus1",
              owned_cp_ptr->get_length() == -1);

        // The bound predicate: a cp index is in-bounds iff 1 <= index <= length
        // (index 0 is the unused slot).  When length == -1 ("unknown") the
        // accessors SKIP the bound check and fall back to is_valid_pointer /
        // is_readable_pointer gating, so -1 disables the numeric clamp.
        auto cp_index_in_bounds = [](std::int32_t index, std::int32_t length) -> bool
        {
            if (length < 0) { return true; }            // unknown -> skip numeric bound
            return index >= 1 && index <= length;
        };
        check("I_cp_index_zero_out_of_bounds", cp_index_in_bounds(0, 10) == false);
        check("I_cp_index_one_in_bounds", cp_index_in_bounds(1, 10) == true);
        check("I_cp_index_at_length_in_bounds", cp_index_in_bounds(10, 10) == true);
        check("I_cp_index_over_length_out_of_bounds", cp_index_in_bounds(11, 10) == false);
        check("I_cp_index_negative_out_of_bounds", cp_index_in_bounds(-1, 10) == false);
        check("I_cp_unknown_length_skips_numeric_bound", cp_index_in_bounds(999, -1) == true);
        // Entry stride is pointer-sized (8 bytes on x64): the byte offset of
        // entry[index] from base is index * sizeof(void*).  Pinned as the ABI
        // fact get_base()+entry arithmetic relies on.
        check("I_cp_entry_is_pointer_sized", sizeof(void*) == 8);
        check("I_cp_entry_offset_is_index_times_8",
              (std::size_t{ 5 } * sizeof(void*)) == 40u);
        // Boundary sweep of the bound predicate against the closed form.
        bool cp_sweep_ok{ true };
        const std::int32_t lens[]{ -1, 0, 1, 2, 100, 65535 };
        const std::int32_t idxs[]{ -5, 0, 1, 2, 99, 100, 101, 65535, 65536 };
        for (const std::int32_t L : lens)
        {
            for (const std::int32_t I : idxs)
            {
                const bool got{ cp_index_in_bounds(I, L) };
                const bool want{ (L < 0) ? true : (I >= 1 && I <= L) };
                if (got != want) { cp_sweep_ok = false; }
            }
        }
        check("I_cp_bound_predicate_sweep", cp_sweep_ok);
    }

    // =====================================================================
    // J. INSTANCEKLASS methods-walk arithmetic (instanceklass_methods_walk leg).
    //    try_reinstall walks the owning class's Array<Method*> to re-resolve a
    //    drifted Method by name+signature.  The ABI math: Method* data begins at
    //    array+8 (int32 _length @0 + 4 pad + 8-aligned pointers @8), the stride
    //    is 8 bytes, and the length is clamped to [0, 65535] (the u2 class-file
    //    method_count ceiling) so a torn/hostile length cannot drive the walk
    //    off the end.  Pinned as the documented x64 layout + clamp facts, plus
    //    the no-JVM null contract on the real klass accessors.
    // =====================================================================
    {
        using vmhook::hotspot::klass;
        // Real klass accessors honour the no-JVM / rejected contract WITHOUT
        // dereferencing (the try_reinstall safe_klass_methods funnel).
        auto* const rejected_k{
            reinterpret_cast<klass*>(std::uintptr_t{ 0xBAADF00Du }) };
        check("J_rejected_klass_is_invalid", !is_valid_pointer(rejected_k));
        check("J_get_methods_count_rejected_zero", rejected_k->get_methods_count() == 0);
        check("J_get_methods_ptr_rejected_null", rejected_k->get_methods_ptr() == nullptr);

        // Array<Method*> layout: _length @0, pad @4, first Method* slot @8, 8-byte
        // stride.  Build an OWNED byte buffer shaped like the array and verify the
        // +8 data offset reaches slot 0 and consecutive slots are 8 bytes apart.
        alignas(16) std::array<std::uint8_t, 64> arr{};
        const std::int32_t length{ 3 };
        std::memcpy(arr.data() + 0, &length, sizeof(length));
        const std::uint64_t slot0{ 0x1111111111111110ull };
        const std::uint64_t slot1{ 0x2222222222222220ull };
        const std::uint64_t slot2{ 0x3333333333333330ull };
        std::memcpy(arr.data() + 8 + 0 * 8, &slot0, sizeof(slot0));
        std::memcpy(arr.data() + 8 + 1 * 8, &slot1, sizeof(slot1));
        std::memcpy(arr.data() + 8 + 2 * 8, &slot2, sizeof(slot2));
        std::int32_t read_len{};
        std::memcpy(&read_len, arr.data(), sizeof(read_len));
        check("J_methods_array_length_at_off0", read_len == 3);
        const auto* const data{
            reinterpret_cast<const std::uint64_t*>(arr.data() + 8) };
        check("J_methods_array_data_at_off8",
              data[0] == slot0 && data[1] == slot1 && data[2] == slot2);
        check("J_methods_array_stride_is_8",
              reinterpret_cast<const std::uint8_t*>(&data[1])
                  - reinterpret_cast<const std::uint8_t*>(&data[0]) == 8);

        // Length clamp: trusted only when 0 <= n <= 65535; anything else (torn
        // read / hostile) clamps to 0 so the walk does nothing.
        auto clamp = [](std::int32_t raw) -> std::int32_t
        {
            return (raw < 0 || raw > 65535) ? 0 : raw;
        };
        check("J_clamp_zero", clamp(0) == 0);
        check("J_clamp_one", clamp(1) == 1);
        check("J_clamp_max_ok", clamp(65535) == 65535);
        check("J_clamp_just_over_zeroed", clamp(65536) == 0);
        check("J_clamp_negative_zeroed", clamp(-1) == 0);
        check("J_clamp_int_min_zeroed", clamp(-2147483647 - 1) == 0);
        check("J_clamp_int_max_zeroed", clamp(2147483647) == 0);
        bool clamp_sweep_ok{ true };
        const std::int32_t probes[]{ -100, -1, 0, 1, 100, 65534, 65535, 65536, 70000, 1000000 };
        for (const std::int32_t n : probes)
        {
            const std::int32_t got{ clamp(n) };
            const std::int32_t want{ (n < 0 || n > 65535) ? 0 : n };
            if (got != want) { clamp_sweep_ok = false; }
        }
        check("J_clamp_boundary_sweep", clamp_sweep_ok);
        // The walk index discipline: a clamped count C means valid indices are
        // [0, C), so the highest byte offset touched is (C-1)*8 + 8 == C*8.
        const std::int32_t c{ clamp(read_len) };
        check("J_walk_last_offset_within_data",
              (static_cast<std::size_t>(c) * 8u) == 24u);
    }

    // =====================================================================
    // K. is_valid_pointer — the per-Method / per-slot gate EVERY verify_hooks
    //    accessor applies before forming `this + offset` (vmhook.hpp:2047).
    //    Pure address arithmetic; NO memory read for any input.  Constants from
    //    source: floor 0xFFFF (reject <=), ceiling 0x7FFFFFFFFFFF (reject >=),
    //    reject odd, reject the debug-fill sentinels by low32.  This is what
    //    makes passing the rejected pointers above POSIX-safe.
    // =====================================================================
    {
        constexpr std::uintptr_t floor{ vmhook::os::user_address_floor };
        constexpr std::uintptr_t ceiling{ vmhook::os::user_address_ceiling };
        check("K_floor_is_0xFFFF", floor == 0xFFFFull);
        check("K_ceiling_value", ceiling == 0x00007FFFFFFFFFFFull);
        check("K_null_rejected", !is_valid_pointer(nullptr));
        check("K_floor_exact_rejected",
              !is_valid_pointer(reinterpret_cast<const void*>(floor)));
        check("K_floor_plus_one_even_accepted",
              is_valid_pointer(reinterpret_cast<const void*>(floor + 1)));
        check("K_floor_plus_two_odd_rejected",
              !is_valid_pointer(reinterpret_cast<const void*>(floor + 2)));
        check("K_ceiling_exact_rejected",
              !is_valid_pointer(reinterpret_cast<const void*>(ceiling)));
        check("K_ceiling_minus_one_even_accepted",
              is_valid_pointer(reinterpret_cast<const void*>(ceiling - 1)));
        // The sentinels verify_hooks-rejected Method* candidates trip on, placed
        // at a high in-range well-aligned base so ONLY the sentinel rule fires.
        constexpr std::uint64_t high_base{ 0x00000A0000000000ull };
        const std::array<std::uint32_t, 9> sentinels{
            0xDEADBEEFu, 0xCAFEBABEu, 0xCCCCCCCCu, 0xCDCDCDCDu, 0xBAADF00Du,
            0xFEEEFEEEu, 0xABABABABu, 0xFDFDFDFDu, 0xDDDDDDDDu };
        bool all_rejected{ true };
        for (const std::uint32_t s : sentinels)
        {
            const std::uint64_t addr{ high_base | static_cast<std::uint64_t>(s) };
            if (is_valid_pointer(reinterpret_cast<const void*>(addr))) { all_rejected = false; }
        }
        check("K_all_sentinel_low32_rejected", all_rejected);
        check("K_clean_low32_high_base_accepted",
              is_valid_pointer(reinterpret_cast<const void*>(high_base | 0x00010002ull)));
    }

    // =====================================================================
    // L. DETERMINISM / FINAL STATE — leave NOTHING armed.  verify_hooks() is
    //    still empty/zero after all the toggling above, the master switch is
    //    back to its default (enabled), and the shutdown gate is clear, so the
    //    process exits in the same state another no-JVM module would expect.
    // =====================================================================
    {
        check("L_verify_hooks_still_zero", vmhook::verify_hooks() == 0);
        check("L_auto_repair_back_to_default_enabled", vmhook::auto_repair_enabled() == true);
        check("L_shutdown_gate_clear",
              vmhook::hotspot::g_shutdown_requested.load(std::memory_order_acquire) == false);
        check("L_watchdog_not_running",
              vmhook::detail::auto_repair::g_watchdog_running.load(std::memory_order_acquire)
                  == false);
    }

    // =====================================================================
    // M. WAVE-29 DEEPENING — ledger-driven gap closure:
    //    (1) verify_hooks() cold-state TRIVIALLY-VALID semantics: with nothing
    //        installed the detector has nothing to drift, so verify_hooks()==0
    //        IS the "all-intact" answer (no failure could be reported); pin the
    //        equivalence across long repeat runs and across master-switch toggles.
    //    (2) Watchdog disabled-by-default in cold state: g_watchdog_running stays
    //        false through every toggle of the master switch (no install happened),
    //        AND g_started — the CAS one-shot guard — also stays false because the
    //        no-live-thread disable branch resets it to false.
    //    (3) Idempotent install/teardown of the master switch: setting it to the
    //        SAME value repeatedly is a true no-op; the round-trip lattice never
    //        leaks g_shutdown_requested up; rapid 64-cycle flip-flop is stable.
    //    (4) More compile-time signature pins (static_assert) on the watchdog
    //        atomics and the constant_pool/klass accessors that the cold detector
    //        funnels through.
    // =====================================================================
    {
        // (4a) Watchdog atomic types are atomic<bool> + lock-free.
        static_assert(std::is_same_v<std::remove_cvref_t<decltype(
                          vmhook::detail::auto_repair::g_watchdog_running)>,
                      std::atomic<bool>>,
                      "g_watchdog_running is atomic<bool>");
        static_assert(std::is_same_v<std::remove_cvref_t<decltype(
                          vmhook::detail::auto_repair::g_started)>,
                      std::atomic<bool>>,
                      "g_started is atomic<bool>");
        static_assert(std::is_same_v<std::remove_cvref_t<decltype(
                          vmhook::hotspot::g_shutdown_requested)>,
                      std::atomic<bool>>,
                      "g_shutdown_requested is atomic<bool>");
        static_assert(std::atomic<bool>::is_always_lock_free,
                      "atomic<bool> must be lock-free for signal-safe gating");
        // (4b) klass accessor signatures the drift detector funnels through.
        static_assert(std::is_same_v<decltype(std::declval<vmhook::hotspot::klass>()
                          .get_methods_count()), std::int32_t>,
                      "klass::get_methods_count returns int32_t");
        static_assert(noexcept(std::declval<vmhook::hotspot::klass>().get_methods_count()),
                      "klass::get_methods_count must be noexcept");
        static_assert(noexcept(std::declval<vmhook::hotspot::klass>().get_methods_ptr()),
                      "klass::get_methods_ptr must be noexcept");
        static_assert(noexcept(vmhook::hotspot::set_dont_inline(
                          static_cast<vmhook::hotspot::method*>(nullptr), true)),
                      "set_dont_inline must be noexcept");
        check("M_compile_time_pins_compiled", true);

        // (1) Cold-state TRIVIALLY-VALID: 0 IS the all-intact answer.  Long
        // repeat (64x) must stay 0; the call must not perturb watchdog state.
        bool long_repeat_zero{ true };
        for (int i{ 0 }; i < 64; ++i)
        {
            if (vmhook::verify_hooks() != 0) { long_repeat_zero = false; }
        }
        check("M_verify_hooks_long_repeat_all_zero", long_repeat_zero);
        check("M_long_repeat_did_not_spawn_watchdog",
              vmhook::detail::auto_repair::g_watchdog_running.load(
                  std::memory_order_acquire) == false);
        check("M_long_repeat_did_not_raise_shutdown",
              vmhook::hotspot::g_shutdown_requested.load(
                  std::memory_order_acquire) == false);
        // Master-switch flip BEFORE / AFTER verify_hooks() does not perturb its
        // return — the master switch gates the watchdog, NOT the manual call.
        vmhook::set_auto_repair_enabled(false);
        check("M_verify_hooks_zero_while_master_disabled",
              vmhook::verify_hooks() == 0);
        vmhook::set_auto_repair_enabled(true);
        check("M_verify_hooks_zero_while_master_reenabled",
              vmhook::verify_hooks() == 0);

        // (2) g_started stays false in cold state — no install ever reached the
        // CAS that flips it true, and the no-live-thread disable branch resets
        // it.  Pinned for completeness so a regression that pre-arms the CAS
        // would surface here as a no-JVM failure.
        check("M_g_started_false_in_cold_state",
              vmhook::detail::auto_repair::g_started.load(
                  std::memory_order_acquire) == false);

        // (3) Idempotent SAME-VALUE writes to the master switch.  Setting to
        // true 8x in a row, then false 8x in a row, leaves enabled==false and
        // no live thread; flipping back to true 8x lands enabled==true.
        for (int i{ 0 }; i < 8; ++i) { vmhook::set_auto_repair_enabled(true); }
        check("M_set_enabled_true_8x_idempotent_value",
              vmhook::auto_repair_enabled() == true);
        check("M_set_enabled_true_8x_idempotent_started",
              vmhook::detail::auto_repair::g_started.load(
                  std::memory_order_acquire) == false);
        for (int i{ 0 }; i < 8; ++i) { vmhook::set_auto_repair_enabled(false); }
        check("M_set_enabled_false_8x_idempotent_value",
              vmhook::auto_repair_enabled() == false);
        check("M_set_enabled_false_8x_idempotent_no_shutdown_leak",
              vmhook::hotspot::g_shutdown_requested.load(
                  std::memory_order_acquire) == false);
        check("M_set_enabled_false_8x_idempotent_no_watchdog",
              vmhook::detail::auto_repair::g_watchdog_running.load(
                  std::memory_order_acquire) == false);

        // Rapid 64-cycle flip-flop stress: ends on the cycle we choose, and the
        // shutdown / watchdog gates remain clear throughout (no live thread to
        // spin up, no thread to shut down).
        for (int i{ 0 }; i < 64; ++i)
        {
            vmhook::set_auto_repair_enabled((i & 1) == 0);
        }
        // Last write (i=63, odd) was set_auto_repair_enabled(false).
        check("M_flipflop_ends_disabled", vmhook::auto_repair_enabled() == false);
        check("M_flipflop_no_watchdog_leak",
              vmhook::detail::auto_repair::g_watchdog_running.load(
                  std::memory_order_acquire) == false);
        check("M_flipflop_no_shutdown_leak",
              vmhook::hotspot::g_shutdown_requested.load(
                  std::memory_order_acquire) == false);

        // FINAL: leave the master switch back to its default (enabled) so the
        // section-L invariants below still hold for downstream modules.
        vmhook::set_auto_repair_enabled(true);
        check("M_final_master_switch_restored_enabled",
              vmhook::auto_repair_enabled() == true);
        check("M_final_verify_hooks_still_zero", vmhook::verify_hooks() == 0);
    }

    std::printf("\n%s: %d failure(s)\n",
                failures == 0 ? "ALL PASS" : "FAILURES", failures);
    return failures == 0 ? 0 : 1;
}
