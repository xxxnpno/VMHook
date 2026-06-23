// Standalone (no-JVM) unit test for vmhook::hotspot::seh_invoke_detour — the
// single crash-safety boundary that wraps every user detour invocation so a
// fault or throw inside a hook callback becomes a recoverable false-return
// instead of tearing the JVM down (vmhook.hpp:7215-7242, the library's one and
// only __try/__except).  This is an ADDITIVE deepening pass: there is no
// dedicated test for this feature today (only doc-comment references in the JVM
// suite), so this file is new and registered in tests/CMakeLists.txt.
//
// SCOPE — what is genuinely no-JVM-determinable and tested here:
//   * The C++-THROW arm of seh_invoke_detour is exercised on the REAL function:
//     a detour lambda that touches ONLY our own local return_slot (and ignores
//     the frame/thread pointers entirely — they are passed as nullptr and never
//     dereferenced) lets us drive the actual __try/__except (MSVC) and the
//     try/catch(...) (everything else) arms with no live HotSpot memory:
//       - a clean detour returns true and its slot side effect IS observed
//         (negative control: the guard does not suppress healthy detours);
//       - a detour that throws std::runtime_error returns false;
//       - a detour that throws a non-std type (throw 42) ALSO returns false
//         (no std::exception&-only narrowing on either arm);
//       - the false-return on a throw is symmetric on every toolchain (both the
//         MSVC SEH-code-0xE06D7363 capture and the C++ catch(...) return false).
//   * The set()-then-throw / cancel()-then-throw torn-slot behaviour (flaw #4):
//     a swallowed throw leaves whatever the detour wrote in the slot — we pin
//     down today's behaviour (the slot is NOT reset by the guard) so a future
//     fix is a deliberate, test-visible change.
//   * The empty-std::function contract (flaw #6 hazard): a default-constructed
//     detour std::function is falsy; calling it would throw bad_function_call.
//   * return_slot is a standard-layout, trivially-copyable POD with the exact
//     field defaults / sizes the trampoline contract relies on (vmhook.hpp:
//     1313-1317): cancel{false}, retval{0}.
//   * java_thread_state::_thread_in_Java == 8 — the state common_detour
//     force-sets after the detour (faulted or not), vmhook.hpp:7337 / 4716.
//   * g_shutdown_requested defaults to false (vmhook.hpp:7197).
//
// OUT OF SCOPE (needs a live JVM / would require a fabricated-address read,
// which SEGV-aborts the suite on POSIX — see the project's HARD RULES): the
// hardware-AV-containment scenarios (null-receiver / stale-OOP / wild-pointer /
// extract_frame_arg faults) that the MSVC __except arm is meant to trap.  Those
// are the JVM integration suite's job and only pass on real cl.exe anyway.  We
// NEVER fabricate a frame*/java_thread*/oop and deref it here.
//
// No external framework: a plain int main(), a failures counter, and a check()
// helper printing [PASS]/[FAIL], matching the sibling tests' idiom.  Every value
// is derived from the header source (inline vmhook.hpp:<line> references).
#include <vmhook/vmhook.hpp>

#include <cstdint>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <type_traits>

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

// The exact detour signature seh_invoke_detour takes (vmhook.hpp:7215, 6020).
// We never construct a real frame/java_thread; the detours below ignore both
// pointer arguments and operate ONLY on the real return_slot we own.
using detour_fn_t = std::function<void(vmhook::hotspot::frame*,
                                       vmhook::hotspot::java_thread*,
                                       vmhook::hotspot::return_slot*)>;

int main()
{
    // =====================================================================
    // SECTION A — return_slot is the POD the trampoline contract depends on
    // (vmhook.hpp:1313-1317).  Standard-layout + trivially-copyable so the
    // trampoline can allocate it on the native stack and the callback can write
    // it without any C++ unwinding hazard; field defaults are cancel=false,
    // retval=0.
    // =====================================================================
    {
        using slot_t = vmhook::hotspot::return_slot;
        check("return_slot_is_standard_layout", std::is_standard_layout_v<slot_t>);
        check("return_slot_is_trivially_copyable", std::is_trivially_copyable_v<slot_t>);
        check("return_slot_is_trivially_destructible", std::is_trivially_destructible_v<slot_t>);

        // Field types exactly as declared: bool cancel, int64 retval.
        check("return_slot_cancel_is_bool",
              std::is_same_v<decltype(slot_t{}.cancel), bool>);
        check("return_slot_retval_is_int64",
              std::is_same_v<decltype(slot_t{}.retval), std::int64_t>);

        // Default member initialisers (vmhook.hpp:1315-1316).
        const slot_t fresh{};
        check("return_slot_default_cancel_false", fresh.cancel == false);
        check("return_slot_default_retval_zero", fresh.retval == 0);

        // retval must be wide enough for the 8-byte raw return cell.
        check("return_slot_retval_is_8_bytes", sizeof(fresh.retval) == 8u);
    }

    // =====================================================================
    // SECTION B — the detour std::function type and the empty-function hazard
    // (flaw #6).  A default-constructed detour function is falsy; the guard
    // does NOT pre-check it today, so invoking it would throw
    // std::bad_function_call — which the C++-throw arm of seh_invoke_detour
    // would swallow as false.  We assert the falsy contract directly (we do NOT
    // call the empty function ourselves).
    // =====================================================================
    {
        detour_fn_t empty{};
        check("empty_detour_function_is_falsy", static_cast<bool>(empty) == false);

        detour_fn_t filled{ [](vmhook::hotspot::frame*,
                               vmhook::hotspot::java_thread*,
                               vmhook::hotspot::return_slot*) noexcept {} };
        check("filled_detour_function_is_truthy", static_cast<bool>(filled) == true);
    }

    // =====================================================================
    // SECTION C — seh_invoke_detour CLEAN path returns true and the detour's
    // side effect IS observed (negative control: the guard must not suppress a
    // healthy detour).  The detour writes only the real local slot; frame/thread
    // are nullptr and never dereferenced.  (vmhook.hpp:7222-7235.)
    // =====================================================================
    {
        vmhook::hotspot::return_slot slot{};
        detour_fn_t clean{ [](vmhook::hotspot::frame*,
                              vmhook::hotspot::java_thread*,
                              vmhook::hotspot::return_slot* const s) noexcept
        {
            s->cancel = true;
            s->retval = 99;
        } };

        const bool ok{ vmhook::hotspot::seh_invoke_detour(clean, nullptr, nullptr, &slot) };
        check("clean_detour_returns_true", ok == true);
        check("clean_detour_side_effect_cancel_observed", slot.cancel == true);
        check("clean_detour_side_effect_retval_observed", slot.retval == 99);
    }

    // A clean detour that does NOTHING still returns true and leaves the slot
    // at its defaults (proves true is "completed", not "wrote something").
    {
        vmhook::hotspot::return_slot slot{};
        detour_fn_t noop{ [](vmhook::hotspot::frame*,
                             vmhook::hotspot::java_thread*,
                             vmhook::hotspot::return_slot*) noexcept {} };
        const bool ok{ vmhook::hotspot::seh_invoke_detour(noop, nullptr, nullptr, &slot) };
        check("clean_noop_detour_returns_true", ok == true);
        check("clean_noop_detour_leaves_slot_default",
              slot.cancel == false && slot.retval == 0);
    }

    // =====================================================================
    // SECTION D — C++ throw of a std::exception subtype is SWALLOWED and
    // seh_invoke_detour returns FALSE.  On MSVC the throw is SEH code
    // 0xE06D7363 which __except(EXCEPTION_EXECUTE_HANDLER) captures; on
    // MinGW/GCC/Clang the catch(...) captures it.  Both return false
    // (vmhook.hpp:7227-7240).  This path needs NO live JVM — the detour throws
    // before touching any frame/thread/oop.
    // =====================================================================
    {
        vmhook::hotspot::return_slot slot{};
        detour_fn_t thrower{ [](vmhook::hotspot::frame*,
                                vmhook::hotspot::java_thread*,
                                vmhook::hotspot::return_slot*)
        {
            throw std::runtime_error{ "detour boom" };
        } };

        const bool ok{ vmhook::hotspot::seh_invoke_detour(thrower, nullptr, nullptr, &slot) };
        check("std_exception_throw_swallowed_returns_false", ok == false);
        // noexcept contract held: control reached here, no std::terminate.
        check("std_exception_throw_did_not_terminate", true);
    }

    // =====================================================================
    // SECTION E — throw of a NON-std type (throw 42) ALSO returns false: there
    // is no `catch (const std::exception&)`-only narrowing on either arm; the
    // non-MSVC arm is catch(...) and the MSVC arm is the all-capturing
    // __except.  (vmhook.hpp:7227 / 7237.)
    // =====================================================================
    {
        vmhook::hotspot::return_slot slot{};
        detour_fn_t int_thrower{ [](vmhook::hotspot::frame*,
                                    vmhook::hotspot::java_thread*,
                                    vmhook::hotspot::return_slot*)
        {
            throw 42;
        } };

        const bool ok{ vmhook::hotspot::seh_invoke_detour(int_thrower, nullptr, nullptr, &slot) };
        check("non_std_throw_swallowed_returns_false", ok == false);
    }

    // =====================================================================
    // SECTION F — torn-slot behaviour (flaw #4).  A detour that calls set()
    // (cancel=true, retval=X) and THEN throws is swallowed (false), but the
    // guard does NOT reset the slot: whatever the detour wrote SURVIVES.  We
    // pin down today's exact behaviour so a future fix (zero the slot on the
    // false path) is a deliberate, test-visible change.
    // =====================================================================
    {
        // set()-then-throw: slot carries the stale value after the false return.
        vmhook::hotspot::return_slot slot{};
        detour_fn_t set_then_throw{ [](vmhook::hotspot::frame*,
                                       vmhook::hotspot::java_thread*,
                                       vmhook::hotspot::return_slot* const s)
        {
            s->cancel = true;
            s->retval = 1234;
            throw std::runtime_error{ "after set" };
        } };

        const bool ok{ vmhook::hotspot::seh_invoke_detour(set_then_throw, nullptr, nullptr, &slot) };
        check("set_then_throw_returns_false", ok == false);
        check("set_then_throw_slot_retval_survives_torn", slot.retval == 1234);
        check("set_then_throw_slot_cancel_survives_torn", slot.cancel == true);
    }
    {
        // cancel()-then-throw (void-method style): cancel flag survives.
        vmhook::hotspot::return_slot slot{};
        detour_fn_t cancel_then_throw{ [](vmhook::hotspot::frame*,
                                          vmhook::hotspot::java_thread*,
                                          vmhook::hotspot::return_slot* const s)
        {
            s->cancel = true;
            throw 7;
        } };

        const bool ok{ vmhook::hotspot::seh_invoke_detour(cancel_then_throw, nullptr, nullptr, &slot) };
        check("cancel_then_throw_returns_false", ok == false);
        check("cancel_then_throw_slot_cancel_survives_torn", slot.cancel == true);
        check("cancel_then_throw_slot_retval_untouched", slot.retval == 0);
    }

    // =====================================================================
    // SECTION G — per-invocation, not latched.  A throwing detour (false) does
    // not poison the guard: a subsequent CLEAN detour through the same
    // seh_invoke_detour still returns true and its side effect is observed
    // (mirrors common_detour's "fault on call N, clean call N+1 still works").
    // =====================================================================
    {
        vmhook::hotspot::return_slot slot{};
        detour_fn_t thrower{ [](vmhook::hotspot::frame*,
                                vmhook::hotspot::java_thread*,
                                vmhook::hotspot::return_slot*)
        {
            throw std::runtime_error{ "first" };
        } };
        detour_fn_t clean{ [](vmhook::hotspot::frame*,
                              vmhook::hotspot::java_thread*,
                              vmhook::hotspot::return_slot* const s) noexcept
        {
            s->retval = 55;
        } };

        const bool first{ vmhook::hotspot::seh_invoke_detour(thrower, nullptr, nullptr, &slot) };
        const bool second{ vmhook::hotspot::seh_invoke_detour(clean, nullptr, nullptr, &slot) };
        check("fault_then_clean_first_returns_false", first == false);
        check("fault_then_clean_second_returns_true", second == true);
        check("fault_then_clean_second_side_effect_observed", slot.retval == 55);
    }

    // =====================================================================
    // SECTION H — repeated throws (stress): 1000 throwing invocations in a loop
    // all return false, none abort/terminate, and the guard has no cumulative
    // state.  Exercises flaw #4's leak surface at the unit level.
    // =====================================================================
    {
        vmhook::hotspot::return_slot slot{};
        detour_fn_t thrower{ [](vmhook::hotspot::frame*,
                                vmhook::hotspot::java_thread*,
                                vmhook::hotspot::return_slot*)
        {
            throw std::runtime_error{ "loop" };
        } };

        bool all_false{ true };
        int  count{ 0 };
        for (int i{ 0 }; i < 1000; ++i)
        {
            if (vmhook::hotspot::seh_invoke_detour(thrower, nullptr, nullptr, &slot))
            {
                all_false = false;
            }
            ++count;
        }
        check("repeated_throws_all_return_false", all_false);
        check("repeated_throws_completed_full_loop", count == 1000);
    }

    // =====================================================================
    // SECTION I — seh_invoke_detour's declared return type and noexcept.  The
    // noexcept (vmhook.hpp:7218) is the load-bearing promise (flaw #5): any
    // escape would become std::terminate.  Assert the signature contract that
    // keeps it honest.
    // =====================================================================
    {
        check("seh_invoke_detour_returns_bool",
              std::is_same_v<decltype(vmhook::hotspot::seh_invoke_detour(
                                 std::declval<const detour_fn_t&>(),
                                 std::declval<vmhook::hotspot::frame*>(),
                                 std::declval<vmhook::hotspot::java_thread*>(),
                                 std::declval<vmhook::hotspot::return_slot*>())),
                            bool>);
        check("seh_invoke_detour_is_noexcept",
              noexcept(vmhook::hotspot::seh_invoke_detour(
                  std::declval<const detour_fn_t&>(),
                  std::declval<vmhook::hotspot::frame*>(),
                  std::declval<vmhook::hotspot::java_thread*>(),
                  std::declval<vmhook::hotspot::return_slot*>())));
    }

    // =====================================================================
    // SECTION J — the constants common_detour pins around the guard.  After the
    // detour (faulted or not) common_detour force-sets the thread state to
    // _thread_in_Java (vmhook.hpp:7337); that enumerator's underlying value is
    // 8 (vmhook.hpp:4716) and the enum's underlying type is int8_t (4703).
    // =====================================================================
    {
        using state_t = vmhook::hotspot::java_thread_state;
        check("thread_state_underlying_is_int8",
              std::is_same_v<std::underlying_type_t<state_t>, std::int8_t>);
        check("thread_state_in_java_is_8",
              static_cast<int>(state_t::_thread_in_Java) == 8);
        // The surrounding enumerators the forced-state fix-up sits between
        // (vmhook.hpp:4708-4720) — derived verbatim from source.
        check("thread_state_in_native_is_4",
              static_cast<int>(state_t::_thread_in_native) == 4);
        check("thread_state_in_vm_is_6",
              static_cast<int>(state_t::_thread_in_vm) == 6);
        check("thread_state_in_java_trans_is_9",
              static_cast<int>(state_t::_thread_in_Java_trans) == 9);
        check("thread_state_max_is_12",
              static_cast<int>(state_t::_thread_max_state) == 12);
    }

    // =====================================================================
    // SECTION K — the shutdown gate common_detour checks BEFORE reaching the
    // guard (vmhook.hpp:7275, 7197) defaults to false in a freshly loaded
    // process with no JVM and no shutdown in flight, so the detour path is
    // reachable.  (Atomic<bool>, lock-free on every supported target.)
    // =====================================================================
    {
        check("g_shutdown_requested_defaults_false",
              vmhook::hotspot::g_shutdown_requested.load(std::memory_order_acquire) == false);
        check("g_shutdown_requested_is_lock_free",
              vmhook::hotspot::g_shutdown_requested.is_lock_free());
    }

    std::printf("\n%s: %d failure(s)\n", failures == 0 ? "OK" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
