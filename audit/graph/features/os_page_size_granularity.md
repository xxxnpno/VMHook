---
slug: os_page_size_granularity
title: Os Page Size Granularity
category: os
status: seeded
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/medium, category/os, tag/os, tag/memory, tag/x86_64, tag/arm64]
---

# Os Page Size Granularity

> **Category:** [[categories/os|OS abstraction (memory / signals / breakpoints)]]  ·  **Status:** `seeded`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/os_page_size_granularity-specialist.md`

## Description

vmhook::os::page_size / allocation_granularity — the host VM geometry queries
every memory operation in the library rounds to.  page_size returns the CPU
page size (GetSystemInfo dwPageSize on Windows; sysconf(_SC_PAGESIZE) on POSIX,
clamped to a 4096 floor).  allocation_granularity returns the VM reservation
granularity (GetSystemInfo dwAllocationGranularity on Windows, where it is the
64 KiB stride mmap-style hint placement must align to; identical to page_size
on POSIX).  Both are process-invariant.  The platform-invariant contract the
trampoline allocator relies on: each is a non-zero power of two >= 4096, and
granularity is a whole multiple of page_size (granularity % page == 0).  The
module also pins the align_up / align_down arithmetic built on these values.

## Related

- [[features/os_allocate_release|os_allocate_release]]
- [[features/os_protect|os_protect]]
- [[features/midi2i_trampoline_alloc|midi2i_trampoline_alloc]]

## Depended on by

- [[features/midi2i_trampoline_alloc|midi2i_trampoline_alloc]]
- [[features/os_allocate_release|os_allocate_release]]
- [[features/os_protect|os_protect]]

## Implementation anchors

- `vmhook::os::page_size` — `vmhook/ext/vmhook/vmhook.hpp:486-496` — GetSystemInfo / sysconf(_SC_PAGESIZE) with 4096 floor
- `vmhook::os::allocation_granularity` — `vmhook/ext/vmhook/vmhook.hpp:501-510` — dwAllocationGranularity on Windows; == page_size on POSIX

## Tests

- `tests/test_os_page_size_granularity.cpp`
- `tests/test_os_layer.cpp`

## Notes

test_os_layer.cpp carries the exhaustive page/granularity + align_up/align_down
invariant sweep (power-of-two, >= 4096, granularity % page == 0, stability
across threads); test_os_page_size_granularity.cpp is the dedicated file.
