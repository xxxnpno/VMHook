#include <vmhook/vmhook.hpp>

// Forward declaration for the optional JNI-side microbench.  Defined in
// vmhook/src/speedtest.cpp, which is only compiled when CMake's
// find_package(JNI) succeeds.  Gated on the same macro so build systems
// that don't include speedtest.cpp (e.g. legacy MSBuild) don't end up
// with an unresolved external.
#if defined(VMHOOK_BENCH_USE_JNI)
extern "C" auto run_vmhook_vs_jni_speedtest() -> void;
#endif

// Modular JVM test harness: per-feature test modules under tests/jvm/modules/
// self-register and are run by run_test_suite() once the JVM is live.  Gated on
// VMHOOK_MODULAR_HARNESS (defined by the CMake example target) so the legacy
// MSBuild vcxproj — which has a hardcoded source list and can't glob the
// modules — still builds example.cpp on its own.
#if defined(VMHOOK_MODULAR_HARNESS)
#include "../../tests/jvm/harness.hpp"
#endif

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// ── Post-Rework-D thin modular driver ────────────────────────────────────────
//
// The legacy inline test_*() battery + its top-level fixture wrappers (Example,
// A, B, Color, Dog, Animal, NestedHost, CallerProbe, TickerProbe, LateClass) were
// retired here: every one had a superset module under tests/jvm/modules/, so the
// duplication was pure cost.  What remains is the thin driver — main_class (to
// flip vmhook/Main.stopJVM at the end), a small result sink shared with the
// modular harness, and run_test_suite(), which just runs vmhook_test::run_all().
// The aaa_warmup module (vmhook_test::priority::first) provides the JIT / class-
// load / GC warm-up the JIT-sensitive modules used to inherit from the battery.

class main_class : public vmhook::object<main_class>
{
public:
    explicit main_class(vmhook::oop_t instance)
        : vmhook::object<main_class>{ instance }
    {
    }


    static auto get_stop_jvm()
        -> bool
    {
        return static_field("stopJVM")->get();
    }

    static auto set_stop_jvm(bool value)
        -> void
    {
        static_field("stopJVM")->set(value);
    }
};

namespace
{
    // Test-result sink + pass/fail tally.  Shared with the modular harness via
    // the vmhook_test::context callbacks wired up in run_test_suite().
    std::ofstream test_log{};
    std::size_t   passed_checks{};
    std::size_t   failed_checks{};

    auto write_result(const std::string& line)
        -> void
    {
        if (test_log.is_open())
        {
            // Flush every line: a JVM-crashing test (a wild OOP deref, a bad
            // hook) takes the whole process down, and a buffered ofstream loses
            // everything written-but-not-flushed — leaving an EMPTY results
            // file with no clue WHERE it died.  Flushing per line means the
            // file always reflects progress up to the crash, so the last line
            // names the module/check that was running.
            test_log << line << '\n';
            test_log.flush();
        }
    }

    auto check(const std::string& name, const bool condition)
        -> void
    {
        if (condition)
        {
            ++passed_checks;
            write_result("[PASS] " + name);
            return;
        }

        ++failed_checks;
        write_result("[FAIL] " + name);
    }

    auto write_summary()
        -> void
    {
        std::ostringstream line{};
        line << "TOTAL: " << passed_checks << "/" << (passed_checks + failed_checks) << " PASSED";
        write_result(line.str());
    }

    template<typename request_function, typename done_function>
    auto run_java_probe(request_function&& set_requested, done_function&& is_done)
        -> bool
    {
        set_requested(true);

        constexpr std::int32_t max_wait_iterations{ 5000 };
        for (std::int32_t wait_iteration{ 0 }; wait_iteration < max_wait_iterations; ++wait_iteration)
        {
            if (is_done())
            {
                break;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds{ 1 });
        }

        set_requested(false);
        return is_done();
    }
} // namespace (anonymous)

static auto run_test_suite() -> void
{
    // JVM readiness gate: bounded poll on the live vmhook/Main class (the
    // launcher entry, always loaded before the DLL injects).  Succeed as soon
    // as find_class resolves it; cap at 30s so a truly failed-to-load JVM still
    // terminates instead of hanging the runner.  (The legacy gate polled
    // vmhook/Example, which Rework D removed.)
    constexpr auto poll_step{ std::chrono::milliseconds{ 50 } };
    constexpr auto poll_cap { std::chrono::seconds{ 30 } };
    const auto deadline{ std::chrono::steady_clock::now() + poll_cap };
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (vmhook::find_class("vmhook/Main"))
        {
            break;
        }
        std::this_thread::sleep_for(poll_step);
    }

    test_log.open("test_results.txt", std::ios::out | std::ios::trunc);

    // vmhook/Main is registered so main_class::set_stop_jvm() can flip stopJVM
    // at the end; every per-feature module resolves its own classes/fixtures.
    vmhook::register_class<main_class>("vmhook/Main");

    // ── Modular per-feature JVM test modules ─────────────────────────────────
    // Every tests/jvm/modules/*.cpp self-registers and runs here against the
    // live JVM, recording into test_results.txt.  Since Rework D this is the
    // ONLY test surface (the legacy inline battery was retired).  aaa_warmup
    // (priority::first) runs before every ordinary module and provides the JIT
    // / class-load / GC warm-up the JIT-sensitive modules used to inherit from
    // the legacy battery.  Gated so the legacy MSBuild build (no modules) still
    // links (it simply runs no tests).
#if defined(VMHOOK_MODULAR_HARNESS)
    {
        vmhook_test::context ctx{};
        ctx.check  = [](const std::string& name, bool ok) { check(name, ok); };
        ctx.record = [](const std::string& line) { write_result(line); };
        ctx.run_probe = [](std::function<void(bool)> set_go,
                           std::function<bool()>     get_done) -> bool
        {
            return run_java_probe(
                [&](bool value) { set_go(value); },
                [&]() { return get_done(); });
        };
        // Post-contained-crash cleanup for the harness's no-SEH Windows AV
        // container (MinGW / clang-on-Windows): after it longjmps out of a
        // faulting module — skipping that module's C++ destructors — any
        // scoped_hook it left armed must be torn down before the next module
        // runs.  shutdown_hooks() is REVERSIBLE, so the next module installs a
        // fully-live hook again.
        ctx.reset = [] { vmhook::shutdown_hooks(); };
        // Run-time master switch for vmhook's background auto-repair watchdog.
        // run_all() flips this OFF before the module loop so the detached
        // watchdog thread cannot race a GC-time code-cache sweep; the
        // watchdog-exercising modules toggle it back on for their own test.
        ctx.set_auto_repair = [](bool enabled) { vmhook::set_auto_repair_enabled(enabled); };
        const std::size_t modules_ran{ vmhook_test::run_all(ctx) };
        write_result("[INFO] ran " + std::to_string(modules_ran) + " modular JVM test module(s).");
        // A green CI must mean the modular registry actually ran (not vacuously
        // empty), so every module's own [FAIL]s are meaningful.
        check("modular_registry_ran_at_least_one_module", modules_ran >= 1);
    }
#endif // VMHOOK_MODULAR_HARNESS

    // ── vmhook vs pure JNI microbench ────────────────────────────────────────
    // Lives in a separate translation unit (speedtest.cpp) so jni.h never bleeds
    // into vmhook.hpp.  Gated on VMHOOK_BENCH_USE_JNI.
#if defined(VMHOOK_BENCH_USE_JNI)
    run_vmhook_vs_jni_speedtest();
#endif

    write_summary();

    main_class::set_stop_jvm(true);

    if (test_log.is_open())
    {
        test_log.close();
    }
}
namespace
{
    inline auto launch_worker_once() -> void
    {
        // Resettable launch flag (instead of std::once_flag): a process-lifetime
        // once_flag means a FreeLibrary+LoadLibrary cycle never re-spawns the
        // worker, which silently defeats vmhook::shutdown_hooks()'s now-reversible
        // teardown+re-init contract (the mod-loader unload/reload pattern).  An
        // atomic bool is reset by an explicit teardown if/when needed; today no
        // caller resets it, so semantics are byte-identical to call_once on the
        // single-load case but the path is no longer one-shot by construction.
        static std::atomic<bool> launched{ false };
        bool expected{ false };
        if (launched.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        {
            std::thread{ run_test_suite }.detach();
        }
    }
}

// ── Platform entry points ────────────────────────────────────────────────────
//
// The harness is invoked the same way on every platform: a single C entry
// point spawns the test worker on a detached std::thread.  The platform-
// specific glue around it just exposes that entry under the names the host
// expects:
//   - Windows : DllMain on DLL_PROCESS_ATTACH (LoadLibrary or remote injection).
//   - POSIX   : a shared-library constructor; also exported as JNI_OnLoad so
//               Java's System.loadLibrary can trigger it.

#if VMHOOK_OS_WINDOWS

extern "C" BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);
        launch_worker_once();
    }
    return TRUE;
}

#else

__attribute__((constructor))
static auto vmhook_so_init() -> void
{
    launch_worker_once();
}

extern "C" int JNI_OnLoad(void* /*vm*/, void* /*reserved*/)
{
    launch_worker_once();
    return 0x00010008; // JNI_VERSION_1_8
}

#endif
