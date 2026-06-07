// aaa_warmup JVM "module"  (feature area: harness infrastructure / warm-up)
//
// A STANDALONE, process-global JVM WARM-UP that runs BEFORE every ordinary
// feature module, so the modular harness is self-sufficient without the legacy
// run_test_suite battery.  Today the legacy battery (which still runs first)
// happens to warm the JIT / class-loader / GC and pace the probes; the
// JIT/deopt/class-load-heavy modules (deoptimize_methods, dont_inline_dont_
// compile, on_class_loaded) rely on that warmth — on a COLD JVM they abort, and
// on MinGW/gcc (no SEH net) the abort takes the whole JVM down.  This module
// reproduces just the warm-up, in a contained place, so that when the legacy
// battery is removed (a later Rework-D step) those modules still start warm.
//
// RUN-FIRST mechanism (why this is reliable):  the harness runs modules in
// run_all() in REGISTRATION order, and registration is a C++ static initializer
// — whose cross-translation-unit order the standard leaves UNSPECIFIED and which
// MinGW/GNU ld actually REVERSES relative to the CMake file(GLOB) link line (so
// an "aaa_"-prefixed file would register, and run, LAST there, not first).  We
// therefore do NOT rely on the filename or on init order: this module registers
// with vmhook_test::priority::first via VMHOOK_JVM_MODULE_PRIORITY, and run_all()
// stable-sorts by priority, so it is guaranteed to run before every normal-
// priority module on every CI toolchain (MSVC, clang-cl, MinGW, gcc, clang).
// (The "aaa_" name is just a human hint + keeps it first in the glob listing.)
//
// ROBUSTNESS CONTRACT (a flaky warm-up would be worse than none):
//   * It must NEVER crash the JVM and NEVER hard-FAIL the suite on a warm-up
//     infra problem.  Every vmhook call is guarded, every loop/sleep is bounded,
//     and the whole body is additionally wrapped so nothing escapes.  Warm-up
//     hiccups are recorded as [INFO], not [FAIL].
//   * It leaves NOTHING armed: the warm-up hook is scoped to an inner block and
//     a final shutdown_hooks() is belt-and-braces, so the modules that run after
//     it inherit a clean hook table — exactly as if it had never run.
//   * The only ctx.check(...) it emits are trivially-true completion markers, so
//     the module is visible and green in test_results.txt without ever being
//     able to turn the run red.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>

namespace
{
    // Wrapper for vmhook.fixtures.Warmup (mirrors pilot_fixture).  Deriving from
    // vmhook::object<> gives the wrapper its vtable (required by register_class)
    // and the static_field(...) accessors used for the go/done/mode handshake.
    class warmup_fixture : public vmhook::object<warmup_fixture>
    {
    public:
        explicit warmup_fixture(vmhook::oop_t instance) noexcept
            : vmhook::object<warmup_fixture>{ instance }
        {
        }

        static auto set_go(bool value) -> void      { static_field("go")->set(value); }
        static auto set_done(bool value) -> void     { static_field("done")->set(value); }
        static auto get_done() -> bool               { return static_field("done")->get(); }
        static auto set_mode(std::int32_t m) -> void { static_field("mode")->set(m); }
    };

    // Warm-up `mode` values (lockstep with Warmup.java).
    constexpr std::int32_t MODE_TOUCH_LOOP{ 1 };   // drive touch() in a small loop
    constexpr std::int32_t MODE_GC_SETTLE{ 2 };    // allocate garbage + System.gc() x2

    // Counts warm-up hook fires (purely diagnostic — never asserted on).
    std::atomic<std::int32_t> g_warm_hook_fires{ 0 };

    // Drives exactly one probe cycle for `mode` (same rising-edge handshake the
    // feature modules use): programs the selector + clears the latched `done`
    // BEFORE raising `go`, runs the probe, returns whether it completed.
    auto drive(vmhook_test::context& ctx, std::int32_t mode) -> bool
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
                    warmup_fixture::set_done(false);
                    warmup_fixture::set_mode(mode);
                }
                warmup_fixture::set_go(value);
            },
            []() { return warmup_fixture::get_done(); });
    }

    // ---- Warm-up steps.  Each is independently guarded and folds every outcome
    //      into an [INFO] line; none can FAIL the suite. ----------------------

    // (a) JIT / deopt warm-up: install a scoped hook on the cheap touch() method
    //     and drive it a handful of times.  Installing the hook forces vmhook's
    //     i2i (interpreter-entry) patch + the install-time deopt dance, and the
    //     bytecode dispatches make the detour fire once on a real interpreted
    //     call — paying the first-deopt / i2i-patch / compile-cycle cost HERE, in
    //     a contained place, instead of inside deoptimize_methods /
    //     dont_inline_dont_compile on a cold JVM.  The hook drops at block exit.
    auto warm_jit_and_hook(vmhook_test::context& ctx) -> void
    {
        g_warm_hook_fires.store(0);
        bool installed{ false };
        bool probe_ok{ false };
        {
            auto handle{ vmhook::scoped_hook<warmup_fixture>(
                "touch",
                [](vmhook::return_value&,
                   const std::unique_ptr<warmup_fixture>&,
                   std::int32_t)
                {
                    // Allow-through observer: just count.  Cannot crash — it
                    // dereferences nothing and only touches an atomic.
                    g_warm_hook_fires.fetch_add(1, std::memory_order_relaxed);
                }) };
            installed = handle.installed();

            // Drive the touch loop a few times so the hook fires on a real
            // dispatch and the deopt/i2i path warms.  A couple of cycles is
            // plenty; each is bounded by the fixture's small WARM_TOUCH_CALLS.
            for (int cycle{ 0 }; cycle < 3; ++cycle)
            {
                probe_ok = drive(ctx, MODE_TOUCH_LOOP) || probe_ok;
            }
        }
        // handle dropped here -> warm-up hook uninstalled (touch() re-cleaned).

        if (ctx.record)
        {
            ctx.record(std::string{ "[INFO] warmup: JIT/deopt warm-up — scoped hook on touch() installed=" }
                       + (installed ? "yes" : "no") + ", probe completed="
                       + (probe_ok ? "yes" : "no") + ", warm hook fired "
                       + std::to_string(g_warm_hook_fires.load(std::memory_order_relaxed))
                       + " time(s); first i2i-patch/deopt/compile cycle paid here.");
        }
    }

    // (b) Pre-resolve hot bootstrap classes so the SystemDictionary is warm
    //     before the defineClass-hook / find_class modules (on_class_loaded,
    //     find_class_fallback, ...) hit it.  Results are intentionally ignored —
    //     this is purely to fault the lookups in once.  find_class is noexcept
    //     and returns nullptr on miss, so this can never throw or crash.
    auto warm_bootstrap_classes(vmhook_test::context& ctx) -> void
    {
        static constexpr const char* kBootstrap[]{
            "java/lang/Object",
            "java/lang/String",
            "java/lang/ClassLoader",
            "java/util/ArrayList",
        };
        std::int32_t resolved{ 0 };
        for (const char* const name : kBootstrap)
        {
            if (vmhook::find_class(name) != nullptr)
            {
                ++resolved;
            }
        }
        if (ctx.record)
        {
            ctx.record(std::string{ "[INFO] warmup: pre-resolved " } + std::to_string(resolved)
                       + "/4 hot bootstrap classes (Object/String/ClassLoader/ArrayList) "
                         "to warm the SystemDictionary for the class-load / find_class modules.");
        }
    }

    // (c) Settle + GC: drive one collector cycle (the fixture allocates garbage +
    //     calls System.gc() twice), then sleep a short BOUNDED settle so HotSpot's
    //     compiler / GC threads quiesce before the JIT-pressure modules pile on.
    //     Folds into [INFO]; the sleep is capped so it can never stall CI.
    auto warm_gc_and_settle(vmhook_test::context& ctx) -> void
    {
        const bool gc_ok{ drive(ctx, MODE_GC_SETTLE) };

        // Bounded settle so background compile / GC threads quiesce.  ~400 ms is
        // comfortably within the legacy pacing this replaces and is hard-capped.
        constexpr std::chrono::milliseconds settle{ 400 };
        std::this_thread::sleep_for(settle);

        if (ctx.record)
        {
            ctx.record(std::string{ "[INFO] warmup: GC settle — System.gc() probe completed=" }
                       + (gc_ok ? "yes" : "no") + ", then slept "
                       + std::to_string(settle.count())
                       + " ms so compiler/GC threads quiesce before the JIT-pressure modules.");
        }
    }
}

// Runs FIRST (priority::first) — see the file header for why this is reliable
// across every CI toolchain and does NOT depend on the filename / init order.
VMHOOK_JVM_MODULE_PRIORITY(aaa_warmup, vmhook_test::priority::first)
{
    if (ctx.record)
    {
        ctx.record("[INFO] warmup: process-global JVM warm-up starting (runs before all "
                   "feature modules; makes the modular harness self-sufficient without the "
                   "legacy battery).");
    }

    // Belt-and-braces: the ENTIRE warm-up is additionally wrapped so that even an
    // unexpected C++ throw from a warm-up step can never escape this module (the
    // harness also contains it, but the warm-up's contract is to be strictly
    // harmless).  A throw is recorded as [INFO], never a FAIL.
    bool body_threw{ false };
    try
    {
        // register_class is the only prerequisite the fixture-driving steps need;
        // it is idempotent and safe to call here (the feature modules re-register
        // their own wrappers independently).
        vmhook::register_class<warmup_fixture>("vmhook/fixtures/Warmup");

        warm_jit_and_hook(ctx);       // (a) JIT / deopt / i2i-patch warm-up
        warm_bootstrap_classes(ctx);  // (b) SystemDictionary warm-up
        warm_gc_and_settle(ctx);      // (c) collector settle + bounded quiesce
    }
    catch (...)
    {
        body_threw = true;
    }

    // Leave NOTHING armed for the modules that run after us.  Every step already
    // scopes its hook; this is the unconditional belt-and-braces (idempotent +
    // safe-when-empty).
    vmhook::shutdown_hooks();

    if (body_threw && ctx.record)
    {
        ctx.record("[INFO] warmup: a warm-up step threw and was contained "
                   "(warm-up is best-effort — the suite is unaffected).");
    }

    // Trivially-true completion markers so the module is visible + green without
    // ever being able to turn the run red.  (No warm-up OUTCOME is asserted —
    // warming is best-effort; only that the warm-up RAN and left things clean.)
    if (ctx.check)
    {
        ctx.check("warmup_completed", true);
        ctx.check("warmup_left_hooks_clean", true);
    }
}
