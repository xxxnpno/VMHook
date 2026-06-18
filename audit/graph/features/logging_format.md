---
slug: logging_format
title: Logging Format
category: infra
status: seeded
risk: low
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/low, category/infra, tag/infra, tag/logging, tag/std-format, tag/no-jvm, tag/host-cpp, tag/never-throw, tag/thread-safe]
---

# Logging Format

> **Category:** [[categories/infra|Infrastructure (wrappers, traits, macros, logging)]]  ·  **Status:** `seeded`  ·  **Risk:** `low`  ·  **Specialist:** `.claude/agents/logging_format-specialist.md`

## Description

The entire diagnostic-logging layer of vmhook — `detail::format_log` (the
std::format / verbatim-fallback formatter), `detail::emit_log_line` (the
never-throw, mutex-serialized sink), the `VMHOOK_LOG(...)` macro (active vs
no-op), the `VMHOOK_HAS_STD_FORMAT` / `VMHOOK_DEBUG_LOGS` / `VMHOOK_LOG_FILE`
configuration knobs, and the `error_tag`/`warning_tag`/`info_tag` literals.
`format_log` returns the formatted string (or, on a malformed/argument-mismatch
that makes std::vformat throw, the raw format string — the never-throw
guarantee); on a pre-std::format toolchain the fallback drops the args and
returns the format string verbatim. `emit_log_line` is `noexcept`, locks a
function-local static mutex, writes to a lazily-opened `VMHOOK_LOG_FILE`
ofstream (append) or falls back to `std::cout`. This is the one piece of vmhook
that is PURE host C++ — no JVM, no JNI, no HotSpot layout — so it is covered by
standalone host tests, not a live-JVM module.

## Implementation anchors

- `detail::format_log (std::format path + fallback)` — `vmhook/ext/vmhook/vmhook.hpp:338-360` — std::format path (340, try/catch -> raw fmt on throw) and the VMHOOK_HAS_STD_FORMAT==0 fallback (354, drops args, returns fmt verbatim)
- `detail::emit_log_line` — `vmhook/ext/vmhook/vmhook.hpp:361-390` — noexcept sink: function-local static mutex + lazily-opened VMHOOK_LOG_FILE ofstream(out|app), else std::cout; whole body in try/catch
- `VMHOOK_LOG macro (active vs no-op)` — `vmhook/ext/vmhook/vmhook.hpp:392-398` — active emit_log_line(format_log(...)) (392) vs unevaluated ((void)sizeof(format_log(...))) no-op (398) keyed on VMHOOK_DEBUG_LOGS
- `error_tag / warning_tag / info_tag` — `vmhook/ext/vmhook/vmhook.hpp:408-410` — inline constexpr std::string_view [VMHook ERROR]/[VMHook WARNING]/[VMHook INFO]
- `VMHOOK_HAS_STD_FORMAT / VMHOOK_DEBUG_LOGS / VMHOOK_LOG_FILE knobs` — `vmhook/ext/vmhook/vmhook.hpp:217-316` — VMHOOK_HAS_STD_FORMAT __has_include(<format>) (217); VMHOOK_DEBUG_LOGS 0-under-NDEBUG-else-1 (293); VMHOOK_LOG_FILE opt-in file logging doc (301)

## Tests

- `tests/test_logging_format.cpp`
- `tests/test_logging_logfile.cpp`
- `tests/test_logging_logfile_unwritable.cpp`

## Known bugs

- **[medium]** No per-line tag/timestamp normalization: a single VMHOOK_LOG message that itself contains embedded '\n' (several call sites build multi-line messages) yields several physical lines only the first of which carries the [VMHook ...] tag, so log-filtering by tag silently drops the continuation lines. (One emit_log_line call IS atomic under the lock — this is a formatting, not an interleaving, defect.)
- **[medium]** format_log fallback drops ALL arguments on a pre-std::format toolchain (GCC <13 / Clang <14 / MSVC <19.29): an error like '{} hook() for {}: {}' is emitted literally with braces and no method/class/exception text. By-design but makes fallback logs useless for triage. The doc comment claiming 'otherwise stream-format' is stale — there is no stream fallback; the code returns fmt untouched.
- **[low]** VMHOOK_LOG_FILE fallback-to-cout is silent and one-shot: the ofstream is a function-local static opened once; an unwritable path never opens and every later call silently uses cout, and a path that becomes writable later is never retried. An open stream that enters a fail state (disk full) is not detected (is_open() stays true, << no-ops) so lines are dropped with no diagnostics.
- **[low]** make_format_args(args...) binds the named lvalue parameters and is safe ONLY because the store is consumed by std::vformat on the same line; the args_t&&... forwarding is misleading — a refactor that captures the result into a variable (dangling UAF) or forwards rvalues into make_format_args (hard compile error) breaks it. A const args_t&... signature would be clearer. Loaded gun, not a live bug.
- **[low]** Mutex/static-init ordering vs very-early/late logging: log_mutex and log_file are Meyers function-local statics destroyed at program exit; a VMHOOK_LOG from a static destructor / atexit handler running after they are torn down would touch a destroyed stream (UB the outer catch does not protect against). Latent — vmhook's own teardown logging runs while the process is live.

## Notes

This feature is PURE host C++ — it never reads JVM memory, calls JNI, or
depends on HotSpot layout — so it has NO Java-8 vs 9+ vs 21+ vs 26 behavioral
variance. Its only version axis is the host C++ TOOLCHAIN:
VMHOOK_HAS_STD_FORMAT splits the whole formatter in two (std::format requires
GCC 13+ / Clang 14+ / MSVC 19.29+), so the MinGW/MSYS GCC + older Clang legs of
the matrix run the FALLBACK path — any formatted-output assertion must be
#if VMHOOK_HAS_STD_FORMAT-gated, and BOTH paths must be green (a local -Werror
MinGW build may be exercising the fallback, not std::format). VMHOOK_DEBUG_LOGS
keys off NDEBUG: Release CI compiles every VMHOOK_LOG to the no-op sizeof form,
Debug to the active emit — the build TYPE is the variable, both tested.
VMHOOK_HAS_STD_PRINT is detected but UNUSED by logging (the sink is always raw
cout/ofstream). The LOG_FILE path is covered by the two dedicated logfile TUs
(writable + unwritable); test_logging_format.cpp covers tags, format_log both
paths, never-throw, emit_log_line noexcept, and the macro in both modes.
