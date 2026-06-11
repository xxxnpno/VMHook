// Standalone (no-JVM) test for vmhook's OPT-IN FILE LOGGING path —
// the `#ifdef VMHOOK_LOG_FILE` branch of detail::emit_log_line
// (vmhook.hpp ~358-380) that the in-process std::cout test
// (test_logging_format.cpp) structurally cannot reach.
//
// That branch is selected at COMPILE TIME by defining VMHOOK_LOG_FILE to a
// path string before including the header, so it needs its own translation
// unit built with -DVMHOOK_LOG_FILE="<writable path>".  tests/CMakeLists.txt
// wires that define (and a twin VMHOOK_TEST_LOG_PATH the test reads back from)
// to a per-build path under ${CMAKE_BINARY_DIR}.
//
// What this TU proves about the file sink (all pure host C++, no HotSpot):
//   * lazy open — the file does not exist before the first emit and DOES after,
//   * line fidelity / ordering — N emitted payloads land as N '\n'-terminated
//     lines in emission order, bytes preserved,
//   * APPEND, not truncate — content pre-seeded into the file BEFORE the first
//     emit survives (the static ofstream is opened std::ios::out|std::ios::app,
//     vmhook.hpp:373); a truncating open would have erased the sentinel,
//   * the file sink SUPPRESSES the std::cout fallback — while the file is open,
//     emit writes nothing to std::cout (rdbuf-captured, asserted empty),
//   * embedded NUL / interior-newline payloads keep their byte length,
//   * concurrency — K threads × M lines serialise under the function-local
//     mutex into exactly K*M intact lines with no torn writes,
//   * VMHOOK_LOG end-to-end routes through the file in an active
//     (VMHOOK_DEBUG_LOGS) build and through the unevaluated no-op otherwise.
//
// Readback strategy: the sink flush()es after every write, so an independent
// std::ifstream opened in the SAME process sees the bytes even though the
// sink's static std::ofstream is still open (verified portable on the MinGW
// and POSIX legs).  Because that static ofstream opens ONCE and appends for
// the whole process, every line this TU ever emits accumulates in the file;
// assertions therefore look for presence/relative-order of expected lines
// inside the full readback rather than pinning an absolute total, except where
// the running tally is tracked explicitly.
//
// Line-ending portability: the file is opened by the library in TEXT mode, so
// on Windows the lone '\n' the sink appends becomes CRLF on disk.  We never do
// a byte-exact whole-file compare; readback is line-oriented (std::getline)
// and we strip a trailing '\r' so the same assertions hold on win32 and Linux.

#include <vmhook/vmhook.hpp>

#include <cstdio>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <fstream>
#include <sstream>
#include <streambuf>
#include <iostream>
#include <algorithm>
#include <thread>
#include <type_traits>

#ifndef VMHOOK_LOG_FILE
#  error "test_logging_logfile.cpp must be compiled with -DVMHOOK_LOG_FILE=<path>"
#endif
#ifndef VMHOOK_TEST_LOG_PATH
#  error "test_logging_logfile.cpp must be compiled with -DVMHOOK_TEST_LOG_PATH=<same path>"
#endif

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

namespace
{
    // The on-disk path the library logs to (== VMHOOK_LOG_FILE, supplied twice
    // by the build so the test can read back what the sink writes).
    constexpr const char* log_path{ VMHOOK_TEST_LOG_PATH };

    // Read the whole log file back as a vector of lines, stripping a trailing
    // '\r' from each so Windows text-mode CRLF and POSIX LF compare identically.
    // Returns false (via `ok`) only if the file could not be opened at all.
    auto read_lines(bool& ok) -> std::vector<std::string>
    {
        std::vector<std::string> lines;
        std::ifstream in{ log_path, std::ios::in | std::ios::binary };
        ok = in.is_open();
        if (!ok)
        {
            return lines;
        }
        std::string line;
        while (std::getline(in, line))
        {
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }
            lines.push_back(line);
        }
        return lines;
    }

    auto file_exists(const char* path) -> bool
    {
        std::ifstream in{ path, std::ios::in | std::ios::binary };
        return in.is_open();
    }

    auto contains_line(const std::vector<std::string>& lines, std::string_view want) -> bool
    {
        return std::find(lines.begin(), lines.end(), std::string{ want }) != lines.end();
    }

    // Index of the first line exactly equal to `want`, or -1 if absent.
    auto index_of(const std::vector<std::string>& lines, std::string_view want) -> int
    {
        for (std::size_t i{ 0 }; i < lines.size(); ++i)
        {
            if (lines[i] == want)
            {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    // std::cout capture (RAII) — used to prove the file sink does NOT also write
    // to std::cout while the file is open.
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

    // A sentinel written into the log file BEFORE any emit_log_line call, so the
    // first (lazy) open of the library's static ofstream is appending to an
    // already-non-empty file.  If the library opened with trunc instead of app,
    // this line would vanish — its survival is the append proof.
    constexpr const char* preseed_line{ "PRESEED-sentinel-line-0" };
}

int main()
{
    // ---------------------------------------------------------------------
    // The path baked into the header (VMHOOK_LOG_FILE) and the one the test
    // reads back (VMHOOK_TEST_LOG_PATH) must be the same string, or the whole
    // TU is meaningless — assert the build wired them consistently.
    // ---------------------------------------------------------------------
    check("logfile_defines_agree",
        std::string_view{ VMHOOK_LOG_FILE } == std::string_view{ VMHOOK_TEST_LOG_PATH });

    // ---------------------------------------------------------------------
    // Start from a clean slate, then PRE-SEED the file with a sentinel line
    // before the library has opened it.  This must happen before the very first
    // emit_log_line call in this process (which is what triggers the lazy open).
    // ---------------------------------------------------------------------
    std::remove(log_path);
    check("logfile_absent_before_any_write", !file_exists(log_path));

    {
        std::ofstream seed{ log_path, std::ios::out | std::ios::trunc };
        check("preseed_stream_open", seed.is_open());
        seed << preseed_line << '\n';
        seed.flush();
    }
    check("logfile_present_after_preseed", file_exists(log_path));

    // ---------------------------------------------------------------------
    // First emit — triggers the library's lazy open (append) and writes a line.
    // The file must now contain BOTH the pre-seed sentinel (append proof) AND
    // the freshly emitted line, in that order.
    // ---------------------------------------------------------------------
    constexpr const char* first_line{ "FILE-line-1" };
    vmhook::detail::emit_log_line(std::string{ first_line });
    {
        bool ok{ false };
        const std::vector<std::string> lines{ read_lines(ok) };
        check("logfile_readable_after_first_emit", ok);
        check("logfile_append_preserved_preseed", contains_line(lines, preseed_line));
        check("logfile_has_first_emitted_line", contains_line(lines, first_line));
        // Append ordering: the sentinel that was already in the file must come
        // BEFORE the first appended line.
        const int seed_idx{ index_of(lines, preseed_line) };
        const int first_idx{ index_of(lines, first_line) };
        check("logfile_append_order_seed_before_first",
            seed_idx >= 0 && first_idx >= 0 && seed_idx < first_idx);
        // Exactly two lines so far (sentinel + one emit): proves no truncation
        // AND no spurious extra lines from a single emit.
        check("logfile_two_lines_after_first_emit", lines.size() == 2u);
    }

    // ---------------------------------------------------------------------
    // The file sink SUPPRESSES the std::cout fallback: with the file open, an
    // emit must write to the file and NOT to std::cout.  Capture cout across an
    // emit and assert it stayed empty, while the line did reach the file.
    // ---------------------------------------------------------------------
    constexpr const char* cout_probe_line{ "FILE-not-on-cout" };
    {
        std::string captured;
        {
            cout_capture cap;
            vmhook::detail::emit_log_line(std::string{ cout_probe_line });
            captured = cap.str();
        }
        check("logfile_emit_does_not_touch_cout", captured.empty());
        bool ok{ false };
        const std::vector<std::string> lines{ read_lines(ok) };
        check("logfile_cout_probe_line_in_file",
            ok && contains_line(lines, cout_probe_line));
    }

    // ---------------------------------------------------------------------
    // Ordering / fidelity of a batch: emit a known sequence and assert every
    // line is present, in the emitted order, byte-for-byte.
    // ---------------------------------------------------------------------
    {
        std::vector<std::string> batch;
        batch.reserve(10);
        for (int i{ 0 }; i < 10; ++i)
        {
            batch.push_back("BATCH-" + std::to_string(i));
        }
        for (const std::string& s : batch)
        {
            vmhook::detail::emit_log_line(s);
        }
        bool ok{ false };
        const std::vector<std::string> lines{ read_lines(ok) };
        check("logfile_batch_readback_ok", ok);

        bool all_present{ true };
        bool in_order{ true };
        int prev_idx{ -1 };
        for (const std::string& s : batch)
        {
            const int idx{ index_of(lines, s) };
            if (idx < 0) { all_present = false; }
            else if (idx <= prev_idx) { in_order = false; }
            if (idx >= 0) { prev_idx = idx; }
        }
        check("logfile_batch_all_lines_present", all_present);
        check("logfile_batch_lines_in_order", in_order);
    }

    // ---------------------------------------------------------------------
    // Empty payload -> a single empty line (just the appended '\n').  Assert the
    // file line count grows by exactly one across the emit.
    // ---------------------------------------------------------------------
    {
        bool ok0{ false };
        const std::size_t before{ read_lines(ok0).size() };
        vmhook::detail::emit_log_line(std::string{});
        bool ok1{ false };
        const std::vector<std::string> after{ read_lines(ok1) };
        check("logfile_empty_payload_adds_one_line",
            ok0 && ok1 && after.size() == before + 1u);
        check("logfile_empty_payload_is_empty_last_line",
            !after.empty() && after.back().empty());
    }

    // ---------------------------------------------------------------------
    // Embedded NUL / interior newline payload: the sink writes the std::string
    // by length, so an interior NUL is preserved on disk and an interior '\n'
    // splits the logical message into multiple physical lines (the sink only
    // APPENDS one).  We assert the file grew and that a NUL byte survived.
    // ---------------------------------------------------------------------
    {
        std::string payload{ "NUL-before" };
        payload.push_back('\0');
        payload += "NUL-after";

        bool ok0{ false };
        const std::size_t before{ read_lines(ok0).size() };
        vmhook::detail::emit_log_line(payload);

        // Read raw bytes (binary) and confirm a NUL is present somewhere — the
        // length-preserving write means the embedded '\0' was not dropped.
        std::ifstream raw{ log_path, std::ios::in | std::ios::binary };
        const std::string all{ std::istreambuf_iterator<char>{ raw },
                               std::istreambuf_iterator<char>{} };
        check("logfile_embedded_nul_byte_present",
            all.find('\0') != std::string::npos);
        bool ok1{ false };
        const std::size_t after{ read_lines(ok1).size() };
        // The payload contains no interior '\n', so it adds exactly one line.
        check("logfile_nul_payload_adds_one_line",
            ok0 && ok1 && after == before + 1u);
    }

    // ---------------------------------------------------------------------
    // A multi-line single message: one emit whose payload has an interior '\n'
    // produces TWO physical lines (interior newline preserved + one appended).
    // ---------------------------------------------------------------------
    {
        bool ok0{ false };
        const std::size_t before{ read_lines(ok0).size() };
        vmhook::detail::emit_log_line(std::string{ "MULTI-a\nMULTI-b" });
        bool ok1{ false };
        const std::vector<std::string> after{ read_lines(ok1) };
        check("logfile_interior_newline_adds_two_lines",
            ok0 && ok1 && after.size() == before + 2u);
        check("logfile_interior_newline_both_parts_present",
            contains_line(after, "MULTI-a") && contains_line(after, "MULTI-b"));
    }

    // ---------------------------------------------------------------------
    // VMHOOK_LOG end-to-end through the file sink.  In an active build
    // (VMHOOK_DEBUG_LOGS truthy) the formatted line is appended; in a no-op
    // build nothing is written.  Either way it must compile and not throw.
    // ---------------------------------------------------------------------
    {
        bool ok0{ false };
        const std::size_t before{ read_lines(ok0).size() };
        bool threw{ false };
        try
        {
            VMHOOK_LOG("{} file-macro probe={}", vmhook::info_tag, 7);
        }
        catch (...) { threw = true; }
        check("logfile_vmhook_log_macro_no_throw", !threw);

        bool ok1{ false };
        const std::vector<std::string> after{ read_lines(ok1) };
#if VMHOOK_DEBUG_LOGS
    #if VMHOOK_HAS_STD_FORMAT
        check("logfile_vmhook_log_active_appended",
            ok0 && ok1 && after.size() == before + 1u
            && contains_line(after, "[VMHook INFO] file-macro probe=7"));
    #else
        check("logfile_vmhook_log_active_appended",
            ok0 && ok1 && after.size() == before + 1u
            && contains_line(after, "{} file-macro probe={}"));
    #endif
#else
        // No-op build: the macro is an unevaluated sizeof, so the file is
        // UNCHANGED by the VMHOOK_LOG call.
        check("logfile_vmhook_log_noop_no_append",
            ok0 && ok1 && after.size() == before);
#endif
    }

    // ---------------------------------------------------------------------
    // Concurrency: K threads each emit M distinct, newline-free lines to the
    // file.  The function-local mutex must serialise them into exactly K*M
    // intact lines with no torn writes.  We snapshot the line count before and
    // after and validate the delta and that every expected line is intact.
    // ---------------------------------------------------------------------
    {
        constexpr int K{ 6 };
        constexpr int M{ 40 };

        bool ok0{ false };
        const std::size_t before{ read_lines(ok0).size() };

        std::vector<std::thread> threads;
        threads.reserve(K);
        for (int t{ 0 }; t < K; ++t)
        {
            threads.emplace_back([t]()
            {
                for (int i{ 0 }; i < M; ++i)
                {
                    vmhook::detail::emit_log_line(
                        "C" + std::to_string(t) + "_" + std::to_string(i));
                }
            });
        }
        for (auto& th : threads) { th.join(); }

        bool ok1{ false };
        const std::vector<std::string> after{ read_lines(ok1) };
        check("logfile_concurrent_added_exactly_K_times_M",
            ok0 && ok1 && after.size() == before + static_cast<std::size_t>(K * M));

        // Every one of the K*M lines must be present exactly and intact (no torn
        // interleaving would have produced a malformed "C<t>_<i>" token).
        bool all_present{ true };
        for (int t{ 0 }; t < K && all_present; ++t)
        {
            for (int i{ 0 }; i < M; ++i)
            {
                if (!contains_line(after, "C" + std::to_string(t) + "_" + std::to_string(i)))
                {
                    all_present = false;
                    break;
                }
            }
        }
        check("logfile_concurrent_all_lines_intact", all_present);
        // No line in the file is malformed (every line is non-empty unless it is
        // one of the deliberately empty ones already emitted; the concurrent
        // batch contributes only "C<t>_<i>" tokens which all contain '_').
        bool concurrent_lines_wellformed{ true };
        for (const std::string& s : after)
        {
            if (!s.empty() && s.front() == 'C' && s.find('_') == std::string::npos)
            {
                concurrent_lines_wellformed = false;
                break;
            }
        }
        check("logfile_concurrent_no_torn_writes", concurrent_lines_wellformed);
    }

    // emit_log_line is noexcept regardless of which sink (file vs cout) the
    // header compiled in — the contract holds on the VMHOOK_LOG_FILE path too.
    check("logfile_emit_is_noexcept",
        noexcept(vmhook::detail::emit_log_line(std::string{})));

    std::printf("\n%d checks failed\n", failures);
    return failures == 0 ? 0 : 1;
}
