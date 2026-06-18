---
slug: os_signal_handler
title: Os Signal Handler
category: os
status: seeded
risk: critical
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/critical, category/os, tag/os, tag/signals, tag/safety, tag/seh, tag/x86_64, tag/arm64]
---

# Os Signal Handler

> **Category:** [[categories/os|OS abstraction (memory / signals / breakpoints)]]  ·  **Status:** `seeded`  ·  **Risk:** `critical`  ·  **Specialist:** `.claude/agents/os_signal_handler-specialist.md`

## Description

The OS-level fault-isolation machinery that lets vmhook touch live, possibly-bad
JVM memory without taking the process down.  Two halves: (1) os::detail_signal —
a POSIX SIGSEGV/SIGBUS sigaction handler plus thread-local sigjmp_buf
sigsetjmp/siglongjmp recovery (installed once, SA_SIGINFO|SA_NODEFER) that backs
safe_read's probed-memcpy fallback on Linux/Android; (2) hotspot::seh_invoke_detour
— the SEH __try/__except (cl.exe) / try-catch(...) (everyone else) wrapper that
fences a single user detour so an access violation chasing a stale OOP becomes a
clean "skip this call" instead of a JVM crash.  Critical caveat: clang-cl and
clang-on-windows define _MSC_VER but their __try does NOT reliably trap a hardware
AV, so on those no-SEH toolchains the firewall degrades and faults must be avoided
at the source.  This is the fault firewall every other pointer-dereferencing
feature relies on.

## Related

- [[features/os_safe_read|os_safe_read]]
- [[features/seh_invoke_detour|seh_invoke_detour]]

## Depended on by

- [[features/os_safe_read|os_safe_read]]
- [[features/seh_invoke_detour|seh_invoke_detour]]

## Implementation anchors

- `vmhook::os::detail_signal` — `vmhook/ext/vmhook/vmhook.hpp:900-943` — POSIX SIGSEGV/SIGBUS handler + thread-local sigsetjmp recovery
- `vmhook::hotspot::seh_invoke_detour` — `vmhook/ext/vmhook/vmhook.hpp:7085-7112` — cl.exe-only __try/__except detour fence; no-SEH toolchains uncovered

## Tests

- `tests/test_os_safe_read.cpp`
- `tests/test_os_protect_interaction.cpp`

## Known bugs

- **[high]** clang-cl / clang-on-windows define _MSC_VER but their __try does not reliably trap a hardware AV; the SEH firewall degrades to no-protection on those toolchains, so faults must be removed at the source there.

## Notes

Backs os_safe_read's POSIX fallback path.  The SEH half (seh_invoke_detour) is
the hooking-machinery side; see the seh_invoke_detour feature for the detour-fence
contract.  No dedicated test module — exercised indirectly via safe_read across
no-access pages.
