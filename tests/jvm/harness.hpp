// Modular JVM test harness — shared API for per-feature test modules.
//
// Every feature gets ONE self-registering test module in tests/jvm/modules/.
// A module is a plain function that receives a `context` (its result sink + the
// Java-coordination probe) and runs as many JVM checks as it can imagine for
// its feature.  Modules self-register at DLL load via the VMHOOK_JVM_MODULE
// macro, so adding a feature is "drop a .cpp in tests/jvm/modules/" — no shared
// file to edit, no merge conflicts between parallel authors.
//
// The driver (example.cpp's run_test_suite) calls vmhook_test::run_all(ctx)
// once the JVM is live and the baseline wrappers are registered; results land in
// the same test_results.txt the CI greps for [FAIL].
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace vmhook_test
{
    // The result sink + Java coordination handed to every module.  Implemented
    // by the driver (example.cpp) so modules never touch the ofstream directly.
    struct context
    {
        // Record a [PASS]/[FAIL] line and bump the pass/fail counters.
        std::function<void(const std::string& name, bool ok)> check;
        // Record a raw line (e.g. "[INFO] ...") without touching counters.
        std::function<void(const std::string& line)> record;

        // Java coordination.  set_go(true) raises the fixture's request flag;
        // poll get_done() until the Java loop has run the fixture's action.
        // Returns true if the action completed within the timeout.  This is the
        // ONLY way to make a hooked Java method actually run from native code:
        // the interpreter hook fires only on real Java bytecode dispatch, which
        // happens on the Java thread inside the fixture's registered action.
        std::function<bool(std::function<void(bool)> set_go,
                           std::function<bool()>     get_done)> run_probe;

        // Tear down ALL installed hooks and clear vmhook's global hook state.
        // Implemented by the driver (example.cpp) as vmhook::shutdown_hooks(),
        // which is REVERSIBLE — a subsequent module's hook<T>() is fully live
        // again.  run_all() calls this after a CONTAINED module crash on the
        // no-SEH Windows path: that recovery longjmps out of the faulting
        // module, skipping its C++ destructors, so any scoped_hook it armed
        // would otherwise stay installed and corrupt the NEXT module.  Resetting
        // here restores a clean slate.  Optional — run_all() no-ops if unset
        // (e.g. the MSVC __try path doesn't need it; __except unwinds the C++
        // frames normally on return, but calling reset() there is harmless).
        std::function<void()> reset;

        // Enable/disable vmhook's background auto-repair watchdog.  Implemented
        // by the driver (example.cpp) as vmhook::set_auto_repair_enabled(bool).
        // run_all() calls set_auto_repair(false) ONCE at the very start so NO
        // detached watchdog thread runs during the functional modules: the
        // watchdog's asynchronous verify_hooks() (os::protect + i2i rewrite)
        // racing a GC-time code-cache sweep/relocation is the uncontainable,
        // off-suite-thread crash class the per-module __try cannot catch.  The
        // functional modules don't need continuous auto-repair — ctx.reset()
        // already clears hook state at each module boundary, and any module
        // that specifically EXERCISES the watchdog re-enables it locally for the
        // duration of its own (GC-quiet) test and disables it again at the end.
        // harness.cpp deliberately does not include vmhook.hpp, so this crosses
        // the boundary as a callback, exactly like reset.  Optional — run_all()
        // no-ops if unset.
        std::function<void(bool enabled)> set_auto_repair;
    };

    using module_fn = void (*)(context&);

    // Run-ordering priority for a module.  run_all() executes modules in
    // ASCENDING priority (lower runs first); modules with equal priority keep
    // their registration order.  The default is `normal`, so an ordinary module
    // never has to think about this.  A module that must run BEFORE every
    // ordinary module (e.g. the process-global JIT/class-load/GC warm-up) uses
    // `first`.
    //
    // WHY an explicit key instead of a filename trick: run_all() iterates the
    // registry in REGISTRATION order, and registration happens in C++ static-
    // initializer order across translation units — which the standard leaves
    // UNSPECIFIED and which real toolchains disagree on.  GNU ld (MinGW + Linux
    // gcc) runs a TU's static initializers in REVERSE link-line order, so a
    // CMake file(GLOB) that sorts "aaa_warmup.cpp" to the front of the link line
    // makes it initialize (and thus register, and thus run) LAST, not first —
    // the exact opposite of what an "aaa_"-prefix is meant to buy.  Sorting the
    // registry by an explicit priority in run_all() makes run-order independent
    // of link order and of the compiler, so "runs first" is guaranteed on every
    // toolchain in CI (MSVC, clang-cl, MinGW, gcc, clang).
    enum class priority : int
    {
        first  = -100,   // warm-up / setup: runs before every normal module
        normal = 0,      // default for ordinary feature modules
        last   = 100,    // teardown / wind-down: runs after every normal module
    };

    // Register a module (called from the VMHOOK_JVM_MODULE static initializer).
    // `prio` controls run order (see `priority`); ordinary modules use the
    // defaulted `normal` so existing callers are unaffected.
    auto register_module(const char* name, module_fn fn,
                         priority prio = priority::normal) -> void;

    // Run every registered module, ordered by ascending priority (modules of
    // equal priority keep their registration order — a STABLE sort).  Each
    // module's failures are isolated (a throwing module is caught and logged,
    // the rest still run).  Returns the number of modules executed.
    auto run_all(context& ctx) -> std::size_t;
}

// Define a self-registering feature test module.
//
//   #include <vmhook/vmhook.hpp>
//   #include "../harness.hpp"
//   VMHOOK_JVM_MODULE(field_primitives)
//   {
//       // ctx.check("...", cond); ctx.run_probe(...); ...
//   }
#define VMHOOK_JVM_MODULE(modname)                                            \
    static void modname##_body(vmhook_test::context& ctx);                    \
    namespace                                                                 \
    {                                                                         \
        struct modname##_registrar                                            \
        {                                                                     \
            modname##_registrar() noexcept                                    \
            {                                                                 \
                vmhook_test::register_module(#modname, &modname##_body);      \
            }                                                                 \
        } modname##_registrar_instance;                                       \
    }                                                                         \
    static void modname##_body(vmhook_test::context& ctx)

// Like VMHOOK_JVM_MODULE but pins the module's run order via a
// vmhook_test::priority value (first / normal / last).  run_all() sorts by it,
// so this is the RELIABLE way to make a module run before (or after) the
// ordinary feature modules on every toolchain — see the `priority` enum for why
// a filename trick is not.  Used by the standalone warm-up module.
#define VMHOOK_JVM_MODULE_PRIORITY(modname, prio)                             \
    static void modname##_body(vmhook_test::context& ctx);                    \
    namespace                                                                 \
    {                                                                         \
        struct modname##_registrar                                            \
        {                                                                     \
            modname##_registrar() noexcept                                    \
            {                                                                 \
                vmhook_test::register_module(#modname, &modname##_body,       \
                                             (prio));                         \
            }                                                                 \
        } modname##_registrar_instance;                                       \
    }                                                                         \
    static void modname##_body(vmhook_test::context& ctx)
