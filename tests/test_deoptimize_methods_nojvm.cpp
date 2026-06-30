// Standalone (no-JVM) unit test for vmhook::deoptimize_methods_if and
// vmhook::deoptimize_all_jit_compiled_methods cold-state contracts.
//
// WAVE-33 LEDGER GAPS this file closes:
//   * cold-state deoptimize_all_jit_compiled_methods() is a safe no-op:
//     returns 0, does not throw, does not crash.
//   * cold-state deoptimize_methods_if(<any-predicate>) returns 0 — the
//     predicate must NEVER be invoked because for_each_loaded_class visits
//     zero klasses cold.
//   * idempotent: two/three back-to-back sweeps cold all return 0.
//   * static_asserts on the public signature:
//       - return type is std::size_t
//       - noexcept-qualified
//       - deoptimize_all_jit_compiled_methods takes no args
//       - deoptimize_methods_if accepts both lambda and free-function
//         predicates (the documented predicate_type universal form).
//   * an always-true predicate, always-false predicate, and a name-matching
//     predicate all return 0 cold and are all NEVER called.
//
// OUT OF SCOPE (live-JVM only — covered by tests/jvm/modules/
// deoptimize_methods.cpp against the DeoptimizeMethods fixture):
//   * actual _code clearing after JIT warm-up.
//   * predicate selectivity on real klass graph.
//   * post-deopt hook fire on next dispatch.

#include <vmhook/vmhook.hpp>

#include <cstdio>
#include <cstddef>
#include <string>
#include <type_traits>

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

// ---------------------------------------------------------------------------
// SECTION 1 - compile-time signature / type locks.
// ---------------------------------------------------------------------------

// deoptimize_all_jit_compiled_methods: () noexcept -> std::size_t
static_assert(std::is_same_v<decltype(vmhook::deoptimize_all_jit_compiled_methods()),
                             std::size_t>,
              "deoptimize_all_jit_compiled_methods must return std::size_t");
static_assert(noexcept(vmhook::deoptimize_all_jit_compiled_methods()),
              "deoptimize_all_jit_compiled_methods must be noexcept");
static_assert(std::is_invocable_r_v<std::size_t,
                                    decltype(&vmhook::deoptimize_all_jit_compiled_methods)>,
              "deoptimize_all_jit_compiled_methods must take no arguments");

// deoptimize_methods_if<predicate>: returns std::size_t, noexcept.
namespace
{
    constexpr auto always_true_pred =
        [](const std::string&, vmhook::hotspot::method*) noexcept { return true; };
    [[maybe_unused]] constexpr auto always_false_pred =
        [](const std::string&, vmhook::hotspot::method*) noexcept { return false; };
}

static_assert(std::is_same_v<decltype(vmhook::deoptimize_methods_if(always_true_pred)),
                             std::size_t>,
              "deoptimize_methods_if must return std::size_t");
static_assert(noexcept(vmhook::deoptimize_methods_if(always_true_pred)),
              "deoptimize_methods_if must be noexcept");

// Free function form must also compile through the universal-reference
// template parameter.
static auto free_pred(const std::string&, vmhook::hotspot::method*) noexcept -> bool
{
    return true;
}
static_assert(std::is_same_v<decltype(vmhook::deoptimize_methods_if(&free_pred)),
                             std::size_t>,
              "deoptimize_methods_if must accept a free-function predicate");

auto main() -> int
{
    std::printf("[deoptimize_methods_nojvm] start\n");

    // -----------------------------------------------------------------------
    // Case 1: cold all-sweep returns 0 and does not crash.
    // -----------------------------------------------------------------------
    {
        const std::size_t n{ vmhook::deoptimize_all_jit_compiled_methods() };
        check("cold deoptimize_all_jit_compiled_methods returns 0", n == 0);
    }

    // -----------------------------------------------------------------------
    // Case 2: predicate is NEVER invoked cold (for_each_loaded_class visits
    // zero klasses without a JVM).
    // -----------------------------------------------------------------------
    {
        int calls{ 0 };
        const std::size_t n{ vmhook::deoptimize_methods_if(
            [&](const std::string&, vmhook::hotspot::method*) noexcept
            {
                ++calls;
                return true;
            }) };
        check("cold deoptimize_methods_if(always-true) returns 0", n == 0);
        check("cold predicate never invoked", calls == 0);
    }

    // -----------------------------------------------------------------------
    // Case 3: always-false predicate cold also returns 0 (and not invoked).
    // -----------------------------------------------------------------------
    {
        int calls{ 0 };
        const std::size_t n{ vmhook::deoptimize_methods_if(
            [&](const std::string&, vmhook::hotspot::method*) noexcept
            {
                ++calls;
                return false;
            }) };
        check("cold deoptimize_methods_if(always-false) returns 0", n == 0);
        check("cold always-false predicate never invoked", calls == 0);
    }

    // -----------------------------------------------------------------------
    // Case 4: name-matching predicate cold returns 0 — the documented usage
    // pattern (e.g. starts_with("net/minecraft/")) must compile and run safely
    // cold even though no klasses exist.
    // -----------------------------------------------------------------------
    {
        int calls{ 0 };
        const std::size_t n{ vmhook::deoptimize_methods_if(
            [&](const std::string& name, vmhook::hotspot::method*) noexcept
            {
                ++calls;
                return name.starts_with("net/minecraft/");
            }) };
        check("cold deoptimize_methods_if(name-match) returns 0", n == 0);
        check("cold name-match predicate never invoked", calls == 0);
    }

    // -----------------------------------------------------------------------
    // Case 5: IDEMPOTENT — three back-to-back sweeps all return 0 and do not
    // crash; pins that there is no latent state between cold sweeps.
    // -----------------------------------------------------------------------
    {
        const std::size_t a{ vmhook::deoptimize_all_jit_compiled_methods() };
        const std::size_t b{ vmhook::deoptimize_all_jit_compiled_methods() };
        const std::size_t c{ vmhook::deoptimize_all_jit_compiled_methods() };
        check("idempotent sweep #1 == 0", a == 0);
        check("idempotent sweep #2 == 0", b == 0);
        check("idempotent sweep #3 == 0", c == 0);
    }

    // -----------------------------------------------------------------------
    // Case 6: returns-normally fence — set a flag right after the call to
    // prove control returned (no abort/terminate/unwind).
    // -----------------------------------------------------------------------
    {
        bool returned{ false };
        (void)vmhook::deoptimize_all_jit_compiled_methods();
        returned = true;
        check("deoptimize_all_jit_compiled_methods returns control", returned);
    }

    // -----------------------------------------------------------------------
    // Case 7: 32 back-to-back sweeps all return 0 — stress idempotence.
    // -----------------------------------------------------------------------
    {
        bool all_zero{ true };
        for (int i = 0; i < 32; ++i)
        {
            if (vmhook::deoptimize_all_jit_compiled_methods() != 0)
            {
                all_zero = false;
                break;
            }
        }
        check("32 consecutive cold sweeps all return 0", all_zero);
    }

    std::printf("[deoptimize_methods_nojvm] done failures=%d\n", failures);
    return failures == 0 ? 0 : 1;
}
