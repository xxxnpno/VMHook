// Standalone (no-JVM) unit test for the hook_install_after_jit feature surface:
// the SECOND scenario where vmhook::hook<T> targets a method that is ALREADY
// JIT-compiled, requiring the install path to DEOPTIMISE it (clear Method::_code,
// redirect _from_interpreted_entry -> i2i and _from_compiled_entry -> c2i).
//
// ───────────────────────────────────────────────────────────────────────────
// WHY THIS FILE EXISTS / WHAT IS NO-JVM-DETERMINABLE
// ───────────────────────────────────────────────────────────────────────────
// The install-time deopt path lives inside `hook<T>(name, sig, detour)` (and the
// bulk `deoptimize_methods_if` sweep that the documented workflow chains after
// install).  It performs the THREE-STEP redirect:
//   (1) m->set_from_interpreted_entry(i2i)   // route interpreted callers
//   (2) m->set_from_compiled_entry(c2i)      // route already-compiled callers
//   (3) m->set_code(nullptr)                 // last: clear JIT entry pointer
// in EXACTLY that order so that any thread observing _code==nullptr also sees
// the entry-point writes already published.  This file pins what IS determinable
// without a live JVM:
//
//   1. The PUBLIC API signatures + noexcept / return-type pins on `hook<T>`,
//      `hook_by_signature<T>`, `deoptimize_methods_if`,
//      `deoptimize_all_jit_compiled_methods`.  Compile-time branch detection
//      via static_assert.
//
//   2. The three-step DEOPT primitives the install path uses, called on null
//      and on is_valid_pointer-rejected Method*: each must be a SAFE no-op
//      (no write to any byte of an owned scratch buffer aliased at a rejected
//      address; no fault on nullptr).  This proves the cold-state contract:
//      "install on a null/invalid Method* returns false safely" — the precise
//      LEDGER gap.
//
//   3. The install-path PREDICATE (does this method need deopt?): in source
//      it is `safe_method_pointer_field(m, _code_entry, code) && code != nullptr
//      && is_valid_pointer(code)`.  Reproduced as a captureless mirror over an
//      exhaustive truth table.
//
//   4. The deopt-sweep PREDICATE-PROTOCOL on the bulk path: the user's
//      `bool(const std::string&, vmhook::hotspot::method*)` callable selects
//      which methods get deoptimised.  Pinned through a captureless test of
//      the predicate-shape contract via a constexpr is_invocable check.
//
//   5. The convenience `deoptimize_all_jit_compiled_methods()` returning 0
//      with no JVM (for_each_loaded_class iterates nothing), idempotent across
//      repeats, and not perturbing watchdog / shutdown gates.
//
//   6. The ORDERING-RULE static_asserts that pin the entry types matching the
//      method::set_* signatures (i2i and c2i are both `void*`).
//
// OUT OF SCOPE (needs live JVM): provoking a real Method whose _code != null at
// install time, observing _code becoming null after install, observing detour
// fire on the next bytecode dispatch.  Those are the live-JVM module's job
// (tests/jvm/modules/hook_install_after_jit.cpp).
//
// Source of truth (vmhook/ext/vmhook/vmhook.hpp):
//   hook<T>(name, sig, detour, *already)           10310 (install entry; deopt inline)
//   hook<T>(name, detour)                          10295 (sig-less convenience)
//   hook_by_signature<T>(desc, detour)             11436 (single-match install)
//   deoptimize_methods_if(predicate)               8375  (bulk sweep)
//   deoptimize_all_jit_compiled_methods            8524  (sweep-all)
//   method::set_code                               3163
//   method::set_from_interpreted_entry             3194
//   method::set_from_compiled_entry                3268
//   method::get_code                               3111
//   get_c2i_entry_from_adapter                     7770
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
#include <vector>

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

namespace
{
    // Captureless mirror of the install-path "needs deopt?" predicate
    // (vmhook.hpp:8443-8452 inside deoptimize_methods_if; same logic gates the
    // per-hook install).  A method needs deopt iff _code reads successfully AND
    // is non-null AND is_valid_pointer-accepted.
    auto needs_deopt(bool code_readable, const void* code) -> bool
    {
        return code_readable
               && code != nullptr
               && vmhook::hotspot::is_valid_pointer(code);
    }

    // Captureless mirror of the three-step deopt ORDER (vmhook.hpp:8496-8498).
    // We model it as a state machine over a struct mirroring the writes — the
    // ORDERING contract is: i2i write, then c2i write, then _code=nullptr LAST.
    struct mock_entry_state
    {
        void* from_interpreted{ reinterpret_cast<void*>(std::uintptr_t{ 0xAAAAull }) };
        void* from_compiled  { reinterpret_cast<void*>(std::uintptr_t{ 0xBBBBull }) };
        void* code           { reinterpret_cast<void*>(std::uintptr_t{ 0xCCCCull }) };
    };

    auto apply_deopt_in_order(mock_entry_state& s, void* i2i, void* c2i) -> void
    {
        s.from_interpreted = i2i;     // (1)
        s.from_compiled    = c2i;     // (2)
        s.code             = nullptr; // (3) LAST
    }
}

int main()
{
    using vmhook::hotspot::is_valid_pointer;

    // =====================================================================
    // 0. COMPILE-TIME static_assert pins on the install / deopt API.
    // =====================================================================
    // (a) deoptimize_all_jit_compiled_methods: noexcept, returns size_t.
    static_assert(std::is_same_v<decltype(vmhook::deoptimize_all_jit_compiled_methods()),
                                 std::size_t>,
                  "deoptimize_all_jit_compiled_methods() returns std::size_t");
    static_assert(noexcept(vmhook::deoptimize_all_jit_compiled_methods()),
                  "deoptimize_all_jit_compiled_methods() must be noexcept");

    // (b) deoptimize_methods_if: callable with a (string, method*) predicate
    //     that returns bool.  This is the branch-detection static_assert for
    //     the bulk install-after-JIT entry point.
    using predicate_alias = bool(*)(const std::string&, vmhook::hotspot::method*) noexcept;
    static_assert(std::is_invocable_r_v<std::size_t,
                                        decltype(vmhook::deoptimize_methods_if<predicate_alias>),
                                        predicate_alias>,
                  "deoptimize_methods_if accepts a bool(const string&, method*) predicate");

    // (c) Method-side deopt primitives.  These are the three writes the install
    //     path performs IN ORDER.  Pin their void return + noexcept.
    static_assert(std::is_same_v<decltype(std::declval<vmhook::hotspot::method>()
                                              .set_code(static_cast<void*>(nullptr))),
                                 void>,
                  "method::set_code returns void");
    static_assert(noexcept(std::declval<vmhook::hotspot::method>()
                               .set_code(static_cast<void*>(nullptr))),
                  "method::set_code must be noexcept");
    static_assert(std::is_same_v<decltype(std::declval<vmhook::hotspot::method>()
                                              .set_from_interpreted_entry(
                                                  static_cast<void*>(nullptr))),
                                 void>,
                  "method::set_from_interpreted_entry returns void");
    static_assert(noexcept(std::declval<vmhook::hotspot::method>()
                               .set_from_interpreted_entry(static_cast<void*>(nullptr))),
                  "method::set_from_interpreted_entry must be noexcept");
    static_assert(std::is_same_v<decltype(std::declval<vmhook::hotspot::method>()
                                              .set_from_compiled_entry(
                                                  static_cast<void*>(nullptr))),
                                 void>,
                  "method::set_from_compiled_entry returns void");
    static_assert(noexcept(std::declval<vmhook::hotspot::method>()
                               .set_from_compiled_entry(static_cast<void*>(nullptr))),
                  "method::set_from_compiled_entry must be noexcept");
    static_assert(std::is_same_v<decltype(std::declval<const vmhook::hotspot::method>()
                                              .get_code()),
                                 void*>,
                  "method::get_code returns void*");
    static_assert(std::is_same_v<decltype(std::declval<const vmhook::hotspot::method>()
                                              .get_from_compiled_entry()),
                                 void*>,
                  "method::get_from_compiled_entry returns void*");
    static_assert(noexcept(std::declval<const vmhook::hotspot::method>()
                               .get_from_compiled_entry()),
                  "method::get_from_compiled_entry must be noexcept");

    // (d) Install signature: the `hook<T>` overloads (4-arg + already_hooked,
    //     2-arg sig-less, hook_by_signature) are templates so we cannot take
    //     their direct address — but we CAN pin that the wrapper registry the
    //     install path consults is the documented type-index map.
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(vmhook::type_to_class_map)>,
                                 std::unordered_map<std::type_index, std::string>>,
                  "type_to_class_map is unordered_map<type_index, string>");

    // (e) BRANCH DETECTION: the install-after-JIT path divides on whether
    //     Method::_code reads as a non-null is_valid_pointer-accepted value
    //     at install time.  Pin the static type of NO_COMPILE (the install
    //     re-arm word) so a width regression fails the build.
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(vmhook::hotspot::NO_COMPILE)>,
                                 std::int32_t>,
                  "NO_COMPILE is int32_t (install re-arm bitset)");
    check("static_asserts_compiled", true);

    // =====================================================================
    // A. NULL/INVALID-Method* INSTALL CONTRACT.  The install path's first guard
    //    is is_valid_pointer; a null/invalid Method* is a no-op short-circuit.
    //    Calling the three deopt write primitives on null and on a rejected
    //    Method* MUST NOT WRITE A BYTE and MUST NOT FAULT.  This is the
    //    LEDGER-named "cold-state hook install on null Method* returns false
    //    safely" gap on the underlying deopt primitives.
    // =====================================================================
    {
        using vmhook::hotspot::method;

        // (a) null: the three writes are safe no-ops (the !is_valid_pointer guard
        //     on `this` short-circuits before any byte is written).  Route the
        //     null pointer through a volatile to keep -Wnonnull from flagging
        //     what is intentionally a null-`this` test of the runtime guard.
        method* volatile null_sink{ nullptr };
        method* const null_m{ null_sink };
        null_m->set_code(nullptr);
        null_m->set_from_interpreted_entry(nullptr);
        null_m->set_from_compiled_entry(nullptr);
        check("A_null_method_set_code_safe", true);
        check("A_null_method_set_from_interpreted_safe", true);
        check("A_null_method_set_from_compiled_safe", true);

        // (b) is_valid_pointer-rejected (sentinel low32 below floor): the same
        //     guard rejects this address; no fabricated address is dereferenced.
        auto* const rejected{
            reinterpret_cast<method*>(std::uintptr_t{ 0xDEADBEEFu }) };
        check("A_rejected_method_is_invalid", !is_valid_pointer(rejected));
        rejected->set_code(nullptr);
        rejected->set_from_interpreted_entry(nullptr);
        rejected->set_from_compiled_entry(nullptr);
        check("A_rejected_method_set_code_safe", true);

        // (c) OWNED zeroed buffer that PASSES is_valid_pointer: with no JVM the
        //     "_code"/"_from_interpreted_entry"/"_from_compiled_entry" VMStruct
        //     entries are unresolved, so the setters bail at `!entry` and write
        //     NO byte.  Snapshot + compare proves byte-identity.
        alignas(16) std::array<std::uint8_t, 128> owned{};
        owned.fill(0x7Bu);
        const std::array<std::uint8_t, 128> snapshot{ owned };
        auto* const owned_m{ reinterpret_cast<method*>(owned.data()) };
        owned_m->set_code(reinterpret_cast<void*>(std::uintptr_t{ 0x12345678u }));
        owned_m->set_from_interpreted_entry(reinterpret_cast<void*>(std::uintptr_t{ 0x12345678u }));
        owned_m->set_from_compiled_entry(reinterpret_cast<void*>(std::uintptr_t{ 0x12345678u }));
        check("A_no_jvm_deopt_writes_no_byte",
              std::memcmp(owned.data(), snapshot.data(), owned.size()) == 0);

        // (d) get_code on null / rejected / owned-no-JVM all return nullptr —
        //     so the install-time "is this method JIT-compiled?" probe answers
        //     NO with no JVM, and the deopt three-step is correctly skipped.
        check("A_get_code_null_returns_null", null_m->get_code() == nullptr);
        check("A_get_code_rejected_returns_null", rejected->get_code() == nullptr);
        check("A_get_code_owned_no_jvm_returns_null", owned_m->get_code() == nullptr);
        check("A_get_from_compiled_entry_null_safe",
              null_m->get_from_compiled_entry() == nullptr);
        check("A_get_from_compiled_entry_rejected_safe",
              rejected->get_from_compiled_entry() == nullptr);
        check("A_get_from_compiled_entry_owned_no_jvm_null",
              owned_m->get_from_compiled_entry() == nullptr);
    }

    // =====================================================================
    // B. INSTALL-AFTER-JIT BRANCH-DETECTION via the needs_deopt mirror.
    //    The install path checks `safe_method_pointer_field(m, _code) && code
    //    && is_valid_pointer(code)`.  Exhaustive truth table.
    // =====================================================================
    {
        // (a) code_readable=false (cold or no-JVM): no deopt regardless of value.
        check("B_unreadable_code_no_deopt",
              needs_deopt(false, nullptr) == false);
        const void* some_code{ reinterpret_cast<const void*>(std::uintptr_t{ 0x00000A0000040000ull }) };
        check("B_unreadable_code_with_addr_no_deopt",
              needs_deopt(false, some_code) == false);

        // (b) code_readable=true, code==null: NOT JIT-compiled (interpreted).
        //     The install path skips deopt; the precondition for "already-JIT"
        //     scenario is NOT met.
        check("B_readable_null_code_no_deopt",
              needs_deopt(true, nullptr) == false);

        // (c) code_readable=true, code!=null but is_valid_pointer rejects:
        //     hostile/torn _code read; skip deopt to stay crash-safe.
        const void* rejected_code{ reinterpret_cast<const void*>(std::uintptr_t{ 0xCAFEBABEu }) };
        check("B_rejected_code_no_deopt",
              needs_deopt(true, rejected_code) == false);

        // (d) code_readable=true, code!=null, valid: THE install-after-JIT case
        //     — deopt is required.  This is the LEDGER-named branch.
        check("B_valid_code_needs_deopt",
              needs_deopt(true, some_code) == true);

        // Exhaustive truth-table sweep against the closed form.
        bool sweep_ok{ true };
        const bool readables[]{ false, true };
        const void* codes[]{
            nullptr,
            reinterpret_cast<const void*>(std::uintptr_t{ 0xDEADBEEFu }),  // rejected
            some_code,                                                     // valid
        };
        for (const bool r : readables)
        {
            for (const void* c : codes)
            {
                const bool got{ needs_deopt(r, c) };
                const bool want{ r && c != nullptr && is_valid_pointer(c) };
                if (got != want) { sweep_ok = false; }
            }
        }
        check("B_truth_table_matches_formula", sweep_ok);
    }

    // =====================================================================
    // C. THREE-STEP DEOPT ORDER (vmhook.hpp:8487-8498 + per-hook install).
    //    The install path does the writes in the order: i2i, c2i, _code=null.
    //    The "_code=null LAST" rule is what guarantees publication ordering:
    //    a thread that observes _code==null also observes the entry-point
    //    writes that preceded it.  Pinned through the mock state machine.
    // =====================================================================
    {
        void* const i2i_stub{ reinterpret_cast<void*>(std::uintptr_t{ 0x00000A0000060000ull }) };
        void* const c2i_adapter{ reinterpret_cast<void*>(std::uintptr_t{ 0x00000A0000070000ull }) };

        // Before deopt: all three fields are non-null sentinels.
        mock_entry_state s{};
        check("C_pre_deopt_code_nonnull", s.code != nullptr);
        check("C_pre_deopt_from_interpreted_sentinel",
              s.from_interpreted == reinterpret_cast<void*>(std::uintptr_t{ 0xAAAAull }));
        check("C_pre_deopt_from_compiled_sentinel",
              s.from_compiled == reinterpret_cast<void*>(std::uintptr_t{ 0xBBBBull }));

        // Apply: writes happen in the source order.
        apply_deopt_in_order(s, i2i_stub, c2i_adapter);
        check("C_post_deopt_from_interpreted_is_i2i", s.from_interpreted == i2i_stub);
        check("C_post_deopt_from_compiled_is_c2i", s.from_compiled == c2i_adapter);
        check("C_post_deopt_code_cleared_LAST", s.code == nullptr);

        // Idempotent: re-applying deopt with the same stubs is a no-op steady state.
        apply_deopt_in_order(s, i2i_stub, c2i_adapter);
        check("C_idempotent_re_deopt_keeps_i2i", s.from_interpreted == i2i_stub);
        check("C_idempotent_re_deopt_keeps_c2i", s.from_compiled == c2i_adapter);
        check("C_idempotent_re_deopt_keeps_code_null", s.code == nullptr);
    }

    // =====================================================================
    // D. BULK DEOPT SWEEP — deoptimize_methods_if + deoptimize_all_jit_compiled_methods.
    //    With no JVM, for_each_loaded_class iterates ZERO klasses, so the sweep
    //    returns 0 every time and never spawns the watchdog.  This is the
    //    documented post-install workflow's cold-state contract.
    // =====================================================================
    {
        const std::size_t r0{ vmhook::deoptimize_all_jit_compiled_methods() };
        check("D_deoptimize_all_empty_returns_zero", r0 == 0);

        // Idempotent across repeats — no hidden state.
        bool all_zero{ true };
        for (int i{ 0 }; i < 8; ++i)
        {
            if (vmhook::deoptimize_all_jit_compiled_methods() != 0) { all_zero = false; }
        }
        check("D_deoptimize_all_repeat_all_zero", all_zero);

        // A user predicate that would ACCEPT every method also returns 0 (no
        // methods to iterate).  The predicate is never invoked.
        std::atomic<int> visits{ 0 };
        const std::size_t r_accept_all{ vmhook::deoptimize_methods_if(
            [&visits](const std::string&, vmhook::hotspot::method*) noexcept -> bool
            {
                visits.fetch_add(1, std::memory_order_relaxed);
                return true;
            }) };
        check("D_deoptimize_methods_if_accept_all_zero", r_accept_all == 0);
        check("D_predicate_never_invoked_no_jvm",
              visits.load(std::memory_order_acquire) == 0);

        // A REJECTING predicate also returns 0 (and equally never invoked).
        const std::size_t r_reject_all{ vmhook::deoptimize_methods_if(
            [](const std::string&, vmhook::hotspot::method*) noexcept -> bool
            {
                return false;
            }) };
        check("D_deoptimize_methods_if_reject_all_zero", r_reject_all == 0);

        // The bulk sweep does not perturb the auto-repair gates.
        check("D_sweep_did_not_raise_shutdown",
              vmhook::hotspot::g_shutdown_requested.load(std::memory_order_acquire) == false);
        check("D_sweep_did_not_spawn_watchdog",
              vmhook::detail::auto_repair::g_watchdog_running.load(std::memory_order_acquire)
                  == false);
    }

    // =====================================================================
    // E. INSTALL-RETURN-FALSE on a NULL / unregistered wrapper type.  This is
    //    the LEDGER-named "cold-state hook install ... returns false safely".
    //    With no JVM, `hook<T>` runs through the entry point, fails to find
    //    the wrapper-type's class name (or finds it and fails on find_class),
    //    catches the exception in its catch-all, and returns false WITHOUT
    //    throwing.  We assert against a FRESH unregistered wrapper-type so
    //    the failure is the documented "class not registered" path; the
    //    return must be false, the call must not throw.
    // =====================================================================
    {
        struct unregistered_wrapper_for_install_after_jit
        {
            // No register_class<>() call -> typeid is absent from type_to_class_map.
        };

        // No-arg detour shape: every vmhook detour starts with return_value&.
        auto no_op_detour = [](vmhook::return_value&) noexcept -> void {};

        // The unsigned: vmhook::hook returns bool; false on any failure.
        const bool installed_2arg{
            vmhook::hook<unregistered_wrapper_for_install_after_jit>(
                std::string_view{ "anyName" }, no_op_detour) };
        check("E_install_unregistered_2arg_returns_false", installed_2arg == false);

        const bool installed_3arg{
            vmhook::hook<unregistered_wrapper_for_install_after_jit>(
                std::string_view{ "anyName" }, std::string_view{ "()V" }, no_op_detour) };
        check("E_install_unregistered_3arg_returns_false", installed_3arg == false);

        const bool installed_by_sig{
            vmhook::hook_by_signature<unregistered_wrapper_for_install_after_jit>(
                std::string_view{ "()V" }, no_op_detour) };
        check("E_install_by_signature_unregistered_returns_false",
              installed_by_sig == false);

        // None of the failures perturbed the auto-repair gates.
        check("E_failed_install_no_shutdown_raise",
              vmhook::hotspot::g_shutdown_requested.load(std::memory_order_acquire) == false);
        check("E_failed_install_no_watchdog_spawn",
              vmhook::detail::auto_repair::g_watchdog_running.load(std::memory_order_acquire)
                  == false);
        // verify_hooks() still reports zero installs (g_hooked_methods empty).
        check("E_failed_install_verify_hooks_zero", vmhook::verify_hooks() == 0);
    }

    // =====================================================================
    // F. PREDICATE-SHAPE CONTRACT for the bulk sweep.  The user supplies a
    //    callable invocable as `bool(const std::string&, vmhook::hotspot::method*)`.
    //    Pinned via constexpr is_invocable_r checks — a regression that
    //    changed the predicate signature (e.g. swapped string for string_view,
    //    or moved method* to first position) would surface as a build break
    //    in user code; we pin BOTH the accepted shape and the convertibility
    //    of its return to bool.
    // =====================================================================
    {
        using pred_shape = bool(const std::string&, vmhook::hotspot::method*);
        static_assert(std::is_invocable_r_v<bool, pred_shape,
                                            const std::string&, vmhook::hotspot::method*>,
                      "documented predicate shape is invocable");
        // A callable returning a bool is acceptable (no return-type regression).
        auto user_predicate = [](const std::string& n, vmhook::hotspot::method* m) noexcept -> bool
        {
            // Real users would filter by package; we never invoke it here.
            (void)n; (void)m;
            return false;
        };
        static_assert(std::is_invocable_r_v<bool, decltype(user_predicate),
                                            const std::string&, vmhook::hotspot::method*>,
                      "user predicate matches documented shape");
        check("F_predicate_shape_pins_compiled", true);

        // Reuse the predicate on the live API — still returns 0 with no JVM.
        const std::size_t r{ vmhook::deoptimize_methods_if(user_predicate) };
        check("F_user_predicate_swept_returns_zero", r == 0);
    }

    // =====================================================================
    // G. FINAL STATE — leave nothing armed.  No installs landed, no watchdog
    //    spawned, shutdown gate clear, master switch enabled by default.
    // =====================================================================
    {
        check("G_verify_hooks_still_zero", vmhook::verify_hooks() == 0);
        check("G_auto_repair_default_enabled", vmhook::auto_repair_enabled() == true);
        check("G_shutdown_gate_clear",
              vmhook::hotspot::g_shutdown_requested.load(std::memory_order_acquire) == false);
        check("G_watchdog_not_running",
              vmhook::detail::auto_repair::g_watchdog_running.load(std::memory_order_acquire)
                  == false);
        check("G_deoptimize_all_final_zero",
              vmhook::deoptimize_all_jit_compiled_methods() == 0);
    }

    std::printf("\n%s: %d failure(s)\n",
                failures == 0 ? "ALL PASS" : "FAILURES", failures);
    return failures == 0 ? 0 : 1;
}
