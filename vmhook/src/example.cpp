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

#include <chrono>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

/*
    The only wrapper this driver needs: main_class mirrors vmhook/Main and
    drives the stop-JVM handshake.  Every per-feature wrapper now lives in its
    own self-registering module under tests/jvm/modules/ (each paired with a
    Java fixture in example/vmhook/fixtures/), so this file is a thin launcher
    around vmhook_test::run_all — not a test surface itself.

    main_class uses the portable `static_field` alias rather than the C++23
    deducing-this get_field("name") overload because GCC includes that overload
    as a candidate in static contexts and errors out; `static_field` /
    `static_method` resolve identically on MSVC, Clang, and GCC.
*/
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
    /*
        The GitHub Actions workflow reads test_results.txt after injection.
        These are the result sink the modular harness writes through: each
        module records [PASS]/[FAIL] lines via the check/write_result helpers
        below, which the driver hands to vmhook_test::run_all as a context.
    */
    std::ofstream test_log{};
    std::size_t passed_checks{};
    std::size_t failed_checks{};


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

    template <typename value_type>
    auto check_equal(const std::string& name, const value_type& actual, const value_type& expected)
        -> void
    {
        check(name, actual == expected);
    }

    auto check_equal(const std::string& name, const std::byte actual, const std::byte expected)
        -> void
    {
        check(name, std::to_integer<int>(actual) == std::to_integer<int>(expected));
    }

    auto check_float(const std::string& name, const float actual, const float expected)
        -> void
    {
        check(name, std::fabs(actual - expected) < 0.0001F);
    }

    auto check_double(const std::string& name, const double actual, const double expected)
        -> void
    {
        check(name, std::fabs(actual - expected) < 0.0001);
    }

    template <typename value_type>
    auto check_vector(const std::string& name, const std::vector<value_type>& actual, const std::vector<value_type>& expected)
        -> void
    {
        check(name, actual == expected);
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
    std::this_thread::sleep_for(std::chrono::seconds{ 2 });

    test_log.open("test_results.txt", std::ios::out | std::ios::trunc);

    // The only class the driver itself needs: main_class drives the stop-JVM
    // handshake (set_stop_jvm) that tears the JVM down once every module has
    // run.  Every feature's own classes are registered inside its module's
    // VMHOOK_JVM_MODULE body, so the driver registers nothing else.
    vmhook::register_class<main_class>("vmhook/Main");

    // ── Modular per-feature JVM test modules ─────────────────────────────
    // Every tests/jvm/modules/*.cpp self-registers and runs here against the
    // live JVM, recording into the same test_results.txt the CI greps for
    // [FAIL].  This is the single authoritative, JVM-based unit-test surface.
    // Gated so the legacy MSBuild build (no modules) still links example.cpp.
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
        const std::size_t modules_ran{ vmhook_test::run_all(ctx) };
        write_result("[INFO] ran " + std::to_string(modules_ran) + " modular JVM test module(s).");
        // Closes the verification gap: a green CI must mean the modular
        // registry actually ran (not vacuously empty), so every module's
        // own [FAIL]s are meaningful.
        check("modular_registry_ran_at_least_one_module", modules_ran >= 1);
    }
#endif // VMHOOK_MODULAR_HARNESS

    // ── vmhook vs pure JNI microbench ────────────────────────────────────
    // Lives in a separate translation unit (speedtest.cpp) so the jni.h
    // include never bleeds into vmhook.hpp itself.  Gated on
    // VMHOOK_BENCH_USE_JNI so build systems that don't include speedtest.cpp
    // (e.g. legacy MSBuild) skip the call entirely.
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
        static std::once_flag launched{};
        std::call_once(launched, []
        {
            std::thread{ run_test_suite }.detach();
        });
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
