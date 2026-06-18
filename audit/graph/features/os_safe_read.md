---
slug: os_safe_read
title: Os Safe Read
category: os
status: seeded
risk: critical
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/critical, category/os, tag/os, tag/memory, tag/safety, tag/x86_64, tag/arm64]
---

# Os Safe Read

> **Category:** [[categories/os|OS abstraction (memory / signals / breakpoints)]]  ·  **Status:** `seeded`  ·  **Risk:** `critical`  ·  **Specialist:** `.claude/agents/os_safe_read-specialist.md`

## Description

vmhook::os::safe_read(dst, src, size) — the fault-tolerant memory read that lets
every HotSpot introspection helper dereference an untrusted JVM pointer (an OOP
candidate, a Klass*, a saved-rbp frame slot) and get back false instead of a
SIGSEGV / access violation when the pointer is garbage, freed, or on a no-access
page.  All-or-nothing: true iff every byte copied.  Backends: Windows
ReadProcessMemory (kernel-validated), Linux/Android process_vm_readv with a
SIGSEGV/SIGBUS sigsetjmp-guarded probe fallback, macOS mach_vm_read_overwrite,
iOS a best-effort faulting memcpy (no user-callable fault-safe API).  Guards a
null dst/src, size==0, and an address-space-wrapping src+size, all returning
false.  safe_read_fast adds a cl.exe-only SEH __try fast path that delegates to
safe_read on every other (no-SEH) toolchain.  If this primitive ever lets a
fault escape, the library's crash-proof promise collapses and the JVM dies.

## Depends on

- [[features/os_signal_handler|os_signal_handler]]

## Related

- [[features/os_protect|os_protect]]
- [[features/os_query_region|os_query_region]]
- [[features/os_page_size_granularity|os_page_size_granularity]]

## Depended on by

- [[features/decode_oop_and_pointers|decode_oop_and_pointers]]
- [[features/hook_basic|hook_basic]]
- [[features/hook_verify_repair|hook_verify_repair]]
- [[features/interpreter_frame_walk|interpreter_frame_walk]]
- [[features/read_java_string|read_java_string]]
- [[features/return_caller|return_caller]]
- [[features/return_set_arg|return_set_arg]]
- [[features/return_stack_trace_depth|return_stack_trace_depth]]
- [[features/vmstructs_offset_resolution|vmstructs_offset_resolution]]

## Implementation anchors

- `vmhook::os::safe_read` — `vmhook/ext/vmhook/vmhook.hpp:955-1018` — RPM / process_vm_readv+sigsetjmp / mach_vm; null+overflow guards
- `vmhook::os::safe_read_fast` — `vmhook/ext/vmhook/vmhook.hpp:1138-1158` — cl.exe SEH fast path; delegates to safe_read elsewhere
- `vmhook::os::detail_signal` — `vmhook/ext/vmhook/vmhook.hpp:900-943` — POSIX SIGSEGV/SIGBUS sigaction + thread-local sigsetjmp recovery

## Tests

- `tests/test_os_safe_read.cpp`
- `tests/test_os_protect_interaction.cpp`

## Notes

Canonical stress: read across a PROT_NONE / PAGE_NOACCESS page and observe a
clean false with no fault (test_os_protect_interaction.cpp).  Pure OS-layer; no
JVM module.
