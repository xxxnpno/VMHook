// Standalone (no-JVM) unit test for vmhook::for_each_loaded_class — the
// cold-state contract of the class-enumeration entry point.
//
// vmhook::for_each_loaded_class wraps a try { graph.for_each_klass(visit) }
// catch (...) block.  With no JVM attached, the class_loader_data_graph
// constructor / for_each_klass either throws (caught) or finds nothing to
// walk.  Either way the OBSERVABLE no-JVM contract is:
//   1. The call RETURNS (does not crash, does not propagate).
//   2. The visitor is NOT invoked even ONCE — count must be 0.
//   3. The function's return type is void.
//   4. The visitor signature is invocable with
//      (const std::string&, vmhook::hotspot::klass*).
//   5. Repeated cold calls (32 in a row) each produce 0 invocations — no
//      latent state leaks between snapshots.
//
// This file PINS those properties at compile time (static_assert on the
// signature contract + return type) and at run time (HARD asserts on the
// invocation count).
//
// Out of scope (live-JVM only — covered by tests/jvm/modules/for_each_loaded_class):
// bootstrap-class presence, application-loaded fixture reach, klass* validity,
// name well-formedness, walk termination under real graph load.

#include "vmhook/vmhook.hpp"

#include <cassert>
#include <cstdio>
#include <string>
#include <type_traits>
#include <utility>

namespace
{
    // === Compile-time contract pins ==========================================

    using klass_t = vmhook::hotspot::klass;

    // Visitor signature contract: must accept (const std::string&, klass*).
    static_assert(std::is_invocable_v<
                      void(*)(const std::string&, klass_t*),
                      const std::string&, klass_t*>,
                  "visitor signature must accept (const std::string&, klass*)");

    // A lambda matching the documented signature must satisfy is_invocable
    // with the documented argument types.
    static auto noop_visitor = [](const std::string&, klass_t*) noexcept {};
    static_assert(std::is_invocable_v<decltype(noop_visitor),
                                      const std::string&, klass_t*>,
                  "documented visitor lambda must be invocable");

    // Return type pin: for_each_loaded_class returns void.
    using ret_t = decltype(vmhook::for_each_loaded_class(noop_visitor));
    static_assert(std::is_same_v<ret_t, void>,
                  "for_each_loaded_class must return void");

    // The library wrapper itself is NOT noexcept — it allocates a string in
    // its catch-log path, and the underlying graph constructor may throw.
    // Pin that observable shape so a future silent flip to noexcept becomes a
    // build break (a noexcept-flip would change exception-propagation
    // semantics for callers that already wrap this in their own try/catch).
    static_assert(!noexcept(vmhook::for_each_loaded_class(noop_visitor)),
                  "for_each_loaded_class is not declared noexcept "
                  "(catch-block VMHOOK_LOG allocates)");

    // Visitor types we want to be acceptable (template parameter inference):
    static_assert(std::is_invocable_v<
                      decltype([](const std::string&, klass_t*){}),
                      const std::string&, klass_t*>,
                  "stateless capture-less lambda is invocable");

    struct counting_visitor
    {
        int* counter;
        void operator()(const std::string&, klass_t*) const noexcept
        {
            ++*counter;
        }
    };
    static_assert(std::is_invocable_v<counting_visitor,
                                      const std::string&, klass_t*>,
                  "functor visitor is invocable");

    // === Runtime cases =======================================================

    // Case 1: a single cold call MUST NOT invoke the visitor.
    auto case_cold_count_zero() -> void
    {
        int count = 0;
        vmhook::for_each_loaded_class(
            [&](const std::string&, klass_t*) { ++count; });
        assert(count == 0 && "cold-state visitor must be invoked 0 times");
    }

    // Case 2: the call returns control to the caller (no crash, no
    // propagated exception).  A flag set immediately after the call proves
    // we did not abort, terminate, or unwind through the call.
    auto case_returns_normally() -> void
    {
        bool returned = false;
        vmhook::for_each_loaded_class(
            [](const std::string&, klass_t*) {});
        returned = true;
        assert(returned && "for_each_loaded_class must return normally on cold state");
    }

    // Case 3: 32 back-to-back cold calls all yield count == 0.  Pins that
    // there is no latent state between snapshots — every call independently
    // observes an empty graph in the no-JVM environment.
    auto case_32_consecutive_calls_all_zero() -> void
    {
        for (int i = 0; i < 32; ++i)
        {
            int local = 0;
            vmhook::for_each_loaded_class(
                [&](const std::string&, klass_t*) { ++local; });
            assert(local == 0 && "every cold call must visit 0 classes");
        }
    }

    // Case 4: a stateful functor (not a lambda) works as a visitor and is
    // also not invoked.  Documents that the template parameter is forwarded
    // by value or reference correctly.
    auto case_functor_visitor() -> void
    {
        int count = 0;
        counting_visitor v{ &count };
        vmhook::for_each_loaded_class(v);
        assert(count == 0 && "functor visitor cold call must be 0");
    }

    // Case 5: rvalue visitor (the documented call style with a temporary
    // lambda) compiles and runs without invoking the body.
    auto case_rvalue_visitor() -> void
    {
        int count = 0;
        vmhook::for_each_loaded_class(
            counting_visitor{ &count });
        assert(count == 0 && "rvalue visitor cold call must be 0");
    }
} // namespace

int main()
{
    case_cold_count_zero();
    case_returns_normally();
    case_32_consecutive_calls_all_zero();
    case_functor_visitor();
    case_rvalue_visitor();
    std::printf("test_for_each_loaded_class_nojvm: OK\n");
    return 0;
}
