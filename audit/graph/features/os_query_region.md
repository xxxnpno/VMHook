---
slug: os_query_region
title: Os Query Region
category: os
status: seeded
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/medium, category/os, tag/os, tag/memory, tag/x86_64, tag/arm64]
---

# Os Query Region

> **Category:** [[categories/os|OS abstraction (memory / signals / breakpoints)]]  ·  **Status:** `seeded`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/os_query_region-specialist.md`

## Description

vmhook::os::query_region — returns a region_info describing the VM region that
contains a pointer (base, size, committed/free, readable, executable, guarded).
Used by safe_readable() and by the trampoline allocator to find a free region
within +/-2 GiB of the hook target.  Backends differ per platform: Windows
VirtualQuery (MEMORY_BASIC_INFORMATION, decoding PAGE_* flags into the portable
bools), Linux/Android parse /proc/self/maps (and report the gap before the next
mapping as free), macOS mach_vm_region, iOS returns a permissive "looks
committed/readable" stub (no /proc, no callable mach_vm).  A null address yields
a zeroed region_info.  region_info also exposes whether a page is guarded
(PAGE_GUARD) so the allocator never picks a guard page.

## Related

- [[features/os_protect|os_protect]]
- [[features/os_allocate_release|os_allocate_release]]
- [[features/midi2i_trampoline_alloc|midi2i_trampoline_alloc]]

## Depended on by

- [[features/adapter_recovery_c2i|adapter_recovery_c2i]]
- [[features/const_method_bounds|const_method_bounds]]
- [[features/midi2i_trampoline_alloc|midi2i_trampoline_alloc]]

## Implementation anchors

- `vmhook::os::query_region` — `vmhook/ext/vmhook/vmhook.hpp:793-898` — VirtualQuery / proc-maps / mach_vm_region / iOS stub; null guard
- `vmhook::os::region_info` — `vmhook/ext/vmhook/vmhook.hpp:472-481` — base/size/committed/free/readable/executable/guarded result struct

## Tests

- `tests/test_os_query_region.cpp`
- `tests/test_os_protect_interaction.cpp`
- `tests/test_os_layer.cpp`

## Notes

No dedicated JVM module — pure OS-layer.  protect_interaction asserts a freshly
allocate_rwx'd block reports committed+readable through query_region.
