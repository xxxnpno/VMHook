---
slug: os_protect
title: Os Protect
category: os
status: seeded
risk: high
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/high, category/os, tag/os, tag/memory, tag/x86_64, tag/arm64, tag/safety]
---

# Os Protect

> **Category:** [[categories/os|OS abstraction (memory / signals / breakpoints)]]  ·  **Status:** `seeded`  ·  **Risk:** `high`  ·  **Specialist:** `.claude/agents/os_protect-specialist.md`

## Description

vmhook::os::protect — changes the protection of a memory region in place,
expressed in portable memory_protection terms (no_access, read, read_write,
execute_read, execute_rw).  On Windows it forwards to VirtualProtect and writes
the previous native flags into the optional old_prot out-param; on POSIX it
page-aligns base down and rounds the length up to whole pages (mprotect requires
page-aligned base+length) and sets old_prot to 0.  Guards: a null address or
size==0 returns false and leaves old_prot untouched, and a [base, base+size)
span that would wrap the address space is rejected as false rather than handing
mprotect a garbage range.  This is the page-permission primitive the trampoline
installer uses to flip target code RW->RX and to set up no-access probe pages.

## Depends on

- [[features/os_page_size_granularity|os_page_size_granularity]]

## Related

- [[features/os_allocate_release|os_allocate_release]]
- [[features/os_query_region|os_query_region]]
- [[features/os_safe_read|os_safe_read]]

## Depended on by

- [[features/hook_basic|hook_basic]]
- [[features/hook_verify_repair|hook_verify_repair]]
- [[features/midi2i_trampoline_alloc|midi2i_trampoline_alloc]]

## Implementation anchors

- `vmhook::os::protect` — `vmhook/ext/vmhook/vmhook.hpp:648-694` — null/zero/overflow guards; POSIX page-rounding; old_prot out-param
- `vmhook::os::to_native_protect` — `vmhook/ext/vmhook/vmhook.hpp:617-641` — memory_protection -> VirtualProtect DWORD / mprotect PROT_* int

## Tests

- `tests/test_os_protect_interaction.cpp`
- `tests/test_os_release_and_protect_edges.cpp`
- `tests/test_os_layer.cpp`

## Notes

No dedicated JVM module — pure OS-layer.  protect_interaction covers the
protect/query_region/safe_read round-trip and each portable protection level;
release_and_protect_edges covers the null/zero early-return leaving old_prot
untouched and the non-page-aligned interior address case.
