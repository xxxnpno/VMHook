---
slug: os_allocate_release
title: Os Allocate Release
category: os
status: seeded
risk: high
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/high, category/os, tag/os, tag/memory, tag/x86_64, tag/arm64, tag/safety]
---

# Os Allocate Release

> **Category:** [[categories/os|OS abstraction (memory / signals / breakpoints)]]  ·  **Status:** `seeded`  ·  **Risk:** `high`  ·  **Specialist:** `.claude/agents/os_allocate_release-specialist.md`

## Description

vmhook::os::allocate_rwx / release — the cross-platform RWX-memory pair the
trampoline allocator is built on.  allocate_rwx reserves+commits `size` bytes
of writable, executable memory (VirtualAlloc PAGE_EXECUTE_READWRITE on Windows;
mmap PROT_READ|WRITE|EXEC, MAP_PRIVATE|ANONYMOUS on POSIX, falling back to RW
on Apple arm64 / iOS where the kernel enforces W^X without the JIT entitlement).
address_hint is a non-binding placement preference: VirtualAlloc is binding-or-
fail, so an occupied hint is retried once with no hint; POSIX mmap simply
relocates.  release frees a prior allocation; release(addr, 0) and
release(nullptr, ...) are no-ops (munmap rejects size 0 with EINVAL, Windows
VirtualFree ignores size).  size==0 to allocate_rwx returns nullptr.

## Depends on

- [[features/os_page_size_granularity|os_page_size_granularity]]

## Related

- [[features/os_protect|os_protect]]
- [[features/os_query_region|os_query_region]]
- [[features/midi2i_trampoline_alloc|midi2i_trampoline_alloc]]

## Depended on by

- [[features/midi2i_trampoline_alloc|midi2i_trampoline_alloc]]

## Implementation anchors

- `vmhook::os::allocate_rwx` — `vmhook/ext/vmhook/vmhook.hpp:703-750` — reserve+commit RWX; non-binding hint rescue + Apple W^X RW fallback
- `vmhook::os::release` — `vmhook/ext/vmhook/vmhook.hpp:774-786` — free; null/zero-size no-op guard

## Tests

- `tests/test_os_layer.cpp`
- `tests/test_os_release_and_protect_edges.cpp`
- `tests/test_os_protect_interaction.cpp`

## Notes

No dedicated JVM module — allocate/release is pure OS-layer, covered by the
no-JVM suites.  release(ptr, 0) idempotence + null-guard edges live in
test_os_release_and_protect_edges.cpp.
