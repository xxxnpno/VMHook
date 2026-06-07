// Modular JVM test harness — registry implementation.
//
// Holds the global list of self-registered feature modules and runs them.
// Deliberately tiny and dependency-free (no vmhook.hpp) so it links into the
// example DLL alongside example.cpp without ODR or include-order surprises.
#include "harness.hpp"

#include <algorithm>
#include <exception>
#include <vector>

#if defined(_MSC_VER) && !defined(__clang__)
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#endif

namespace vmhook_test
{
    namespace
    {
        struct entry
        {
            const char* name;
            module_fn   fn;
            priority    prio;
        };

        // Run one module, CONTAINING a hard crash (access violation chasing a
        // stale OOP, a bad hook) so it can't take the JVM and the whole suite
        // down.  On MSVC we get real SEH containment via __try/__except (which
        // must live in a function with no C++ unwinding objects — hence this
        // tiny standalone helper).  On MinGW/Clang __try/__except isn't
        // available, so we fall back to a plain C++ try/catch, which catches
        // C++ throws but NOT a segfault — those still take the process down on
        // those toolchains (matching vmhook's own seh_invoke_detour behaviour).
        // Returns true on clean completion, false if it crashed/threw.
        inline auto run_one(const module_fn fn, context& ctx) noexcept -> bool
        {
#if defined(_MSC_VER) && !defined(__clang__)
            __try
            {
                fn(ctx);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
#else
            try
            {
                fn(ctx);
                return true;
            }
            catch (...)
            {
                return false;
            }
#endif
        }

        // Function-local static so registration from other TUs' static
        // initializers (which may run before/after this TU's) always sees a
        // constructed vector — avoids the static-init-order fiasco.
        auto registry() -> std::vector<entry>&
        {
            static std::vector<entry> modules;
            return modules;
        }
    }

    auto register_module(const char* const name, const module_fn fn,
                         const priority prio) -> void
    {
        if (name && fn)
        {
            registry().push_back(entry{ name, fn, prio });
        }
    }

    auto run_all(context& ctx) -> std::size_t
    {
        // Order by ascending priority so a `priority::first` module (the warm-up)
        // runs before every ordinary module and a `priority::last` module runs
        // after them, REGARDLESS of static-initializer / link order (which the
        // C++ standard leaves unspecified and which MinGW/GNU ld in fact
        // REVERSES — see harness.hpp).  std::stable_sort preserves registration
        // order among equal-priority modules, so the default `normal` modules
        // keep running in exactly the order they do today.
        std::stable_sort(registry().begin(), registry().end(),
                         [](const entry& lhs, const entry& rhs) noexcept
                         {
                             return static_cast<int>(lhs.prio) < static_cast<int>(rhs.prio);
                         });

        std::size_t ran{ 0 };
        for (const entry& module_entry : registry())
        {
            if (ctx.record)
            {
                ctx.record(std::string{ "[INFO] === module: " } + module_entry.name + " ===");
            }
            const bool clean{ run_one(module_entry.fn, ctx) };
            if (!clean && ctx.check)
            {
                // A crash/throw escaped the module.  Record it as a failure so
                // the run goes red and the module is named; on MSVC the SEH
                // containment let the suite keep going to reveal EVERY bad
                // module in one run.
                ctx.check(std::string{ "module_" } + module_entry.name + "_completed_cleanly", false);
            }
            // Paired with the "=== module: X ===" line above: with per-line
            // flushing in the driver, if a module crashes the JVM on a toolchain
            // without SEH containment (MinGW/Clang) the results file ends at its
            // "=== module: X ===" with no matching "--- done ---", naming the
            // culprit even when the whole process died.
            if (ctx.record)
            {
                ctx.record(std::string{ "[INFO] --- module " } + module_entry.name + " done ---");
            }
            ++ran;
        }
        return ran;
    }
}
