// Standalone (no-JVM) test for vmhook's file-logging FALLBACK path: when
// VMHOOK_LOG_FILE points at a path that cannot be opened for writing, the sink
// (detail::emit_log_line, vmhook.hpp:358-382) must NOT crash and must fall back
// to std::cout silently.  This is the `is_open() == false` arm of the
// `#ifdef VMHOOK_LOG_FILE` branch — unreachable from the writable-file TU.
//
// The build (tests/CMakeLists.txt) compiles this TU with VMHOOK_LOG_FILE set to
// a path inside a directory that does not exist under ${CMAKE_BINARY_DIR}, so
// the library's lazy std::ofstream open fails (is_open() == false on every
// supported STL — verified on the MinGW and POSIX legs), exercising the
// fallback.  The twin VMHOOK_TEST_LOG_PATH lets the test confirm that bogus
// file was never created.
//
// Contract asserted here:
//   * no crash / no throw when the configured log file cannot be opened,
//   * emit_log_line and the active VMHOOK_LOG fall back to std::cout
//     (rdbuf-captured) — the exact payload + a single '\n' appears there,
//   * the unwritable path is NOT created as a side effect,
//   * emit_log_line is still noexcept on this compile.
//
// Everything is pure host C++ (no HotSpot), deterministic, flake-free.

#include <vmhook/vmhook.hpp>

#include <cstdio>
#include <string>
#include <string_view>
#include <fstream>
#include <sstream>
#include <streambuf>
#include <iostream>

#ifndef VMHOOK_LOG_FILE
#  error "test_logging_logfile_unwritable.cpp must be compiled with -DVMHOOK_LOG_FILE=<bad path>"
#endif
#ifndef VMHOOK_TEST_LOG_PATH
#  error "test_logging_logfile_unwritable.cpp must be compiled with -DVMHOOK_TEST_LOG_PATH=<same bad path>"
#endif

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

namespace
{
    constexpr const char* bad_path{ VMHOOK_TEST_LOG_PATH };

    auto file_exists(const char* path) -> bool
    {
        std::ifstream in{ path, std::ios::in | std::ios::binary };
        return in.is_open();
    }

    class cout_capture
    {
    public:
        cout_capture()
            : old_{ std::cout.rdbuf(buffer_.rdbuf()) }
        {
        }
        ~cout_capture()
        {
            std::cout.rdbuf(old_);
        }
        cout_capture(const cout_capture&)            = delete;
        cout_capture& operator=(const cout_capture&) = delete;
        auto str() const -> std::string { return buffer_.str(); }

    private:
        std::ostringstream  buffer_{};
        std::streambuf*     old_{ nullptr };
    };
}

int main()
{
    check("unwritable_defines_agree",
        std::string_view{ VMHOOK_LOG_FILE } == std::string_view{ VMHOOK_TEST_LOG_PATH });

    // The configured log path lives in a non-existent directory; it must not
    // exist at start (nothing has tried to create it, and the parent dir is
    // absent so it cannot be created by the failed open either).
    check("unwritable_path_absent_at_start", !file_exists(bad_path));

    // ---------------------------------------------------------------------
    // First emit: the library's lazy ofstream open FAILS (parent dir missing),
    // so the sink falls back to std::cout.  The exact payload + one '\n' must
    // appear on the captured cout, and the call must not throw.
    // ---------------------------------------------------------------------
    {
        std::string captured;
        bool threw{ false };
        {
            cout_capture cap;
            try
            {
                vmhook::detail::emit_log_line(std::string{ "fallback-line-1" });
            }
            catch (...) { threw = true; }
            captured = cap.str();
        }
        check("unwritable_emit_does_not_throw", !threw);
        check("unwritable_emit_falls_back_to_cout",
            captured == "fallback-line-1\n");
    }

    // A second emit also falls back (the failed-open state is sticky for the
    // process, but the fallback path is taken every time).
    {
        std::string captured;
        {
            cout_capture cap;
            vmhook::detail::emit_log_line(std::string{ "fallback-line-2" });
            captured = cap.str();
        }
        check("unwritable_second_emit_also_on_cout",
            captured == "fallback-line-2\n");
    }

    // Empty payload still yields just the newline on the cout fallback.
    {
        std::string captured;
        {
            cout_capture cap;
            vmhook::detail::emit_log_line(std::string{});
            captured = cap.str();
        }
        check("unwritable_empty_payload_is_newline", captured == "\n");
    }

    // ---------------------------------------------------------------------
    // VMHOOK_LOG end-to-end with an unopenable file: active build writes the
    // formatted line to the cout fallback; no-op build writes nothing.  No throw
    // either way.
    // ---------------------------------------------------------------------
    {
        std::string captured;
        bool threw{ false };
        {
            cout_capture cap;
            try
            {
                VMHOOK_LOG("{} unwritable probe={}", vmhook::warning_tag, 9);
            }
            catch (...) { threw = true; }
            captured = cap.str();
        }
        check("unwritable_vmhook_log_no_throw", !threw);
#if VMHOOK_DEBUG_LOGS
    #if VMHOOK_HAS_STD_FORMAT
        check("unwritable_vmhook_log_active_on_cout",
            captured == "[VMHook WARNING] unwritable probe=9\n");
    #else
        check("unwritable_vmhook_log_active_on_cout",
            captured == "{} unwritable probe={}\n");
    #endif
#else
        check("unwritable_vmhook_log_noop_silent", captured.empty());
#endif
    }

    // ---------------------------------------------------------------------
    // The failed open must NOT have created the bogus file (its parent dir does
    // not exist, so neither the file nor anything else was written there).
    // ---------------------------------------------------------------------
    check("unwritable_path_not_created", !file_exists(bad_path));

    // noexcept contract holds on this compile too.
    check("unwritable_emit_is_noexcept",
        noexcept(vmhook::detail::emit_log_line(std::string{})));

    std::printf("\n%d checks failed\n", failures);
    return failures == 0 ? 0 : 1;
}
