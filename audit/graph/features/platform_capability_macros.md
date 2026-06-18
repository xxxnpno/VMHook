---
slug: platform_capability_macros
title: Platform Capability Macros
category: infra
status: seeded
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/medium, category/infra, tag/infra, tag/macros, tag/x86_64, tag/arm64, tag/portability]
---

# Platform Capability Macros

> **Category:** [[categories/infra|Infrastructure (wrappers, traits, macros, logging)]]  ·  **Status:** `seeded`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/platform_capability_macros-specialist.md`

## Description

The preprocessor capability layer the whole header is built on.  Exactly one of
the five OS macros (VMHOOK_OS_WINDOWS / _LINUX / _MACOS / _IOS / _ANDROID) is 1
and the rest 0, with aggregates VMHOOK_OS_POSIX (Linux|macOS|iOS|Android) and
VMHOOK_OS_APPLE (macOS|iOS).  Exactly one arch macro (VMHOOK_ARCH_X86_64 /
_ARM64) is set, and VMHOOK_RUNTIME_HOOKING_AVAILABLE is 1 only on
x86_64 && !iOS (the trampoline emits x64 bytes and walks the HotSpot frame
layout).  Compiler macros (VMHOOK_COMPILER_MSVC / _CLANG / _GCC) distinguish
cl.exe from clang-cl, and feature probes (VMHOOK_HAS_STD_FORMAT, _STD_PRINT,
_HAS_DEDUCING_THIS) gate optional C++20/23 code paths.  Unsupported OS/arch
combinations #error at compile time.  These macros are the single source of
truth every other feature branches on.

## Related

- [[features/version_macros|version_macros]]
- [[features/api_surface_no_jvm|api_surface_no_jvm]]

## Implementation anchors

- `VMHOOK_OS_* / VMHOOK_OS_POSIX / VMHOOK_OS_APPLE` — `vmhook/ext/vmhook/vmhook.hpp:128-171` — five-OS detection (exactly one == 1) + POSIX/Apple aggregates
- `VMHOOK_ARCH_* / VMHOOK_RUNTIME_HOOKING_AVAILABLE` — `vmhook/ext/vmhook/vmhook.hpp:173-196` — x86_64/arm64 select; runtime-hooking gate (x86_64 && !iOS)
- `VMHOOK_COMPILER_* / VMHOOK_HAS_* / VMHOOK_HAS_DEDUCING_THIS` — `vmhook/ext/vmhook/vmhook.hpp:198-262` — compiler split + std::format/print + deducing-this feature probes

## Tests

- `tests/test_platform_capability_macros.cpp`

## Notes

Pure preprocessor; no JVM module.  Tested cross-platform by the no-JVM suite to
assert the exactly-one-OS / exactly-one-arch invariants and the
RUNTIME_HOOKING_AVAILABLE gate.
