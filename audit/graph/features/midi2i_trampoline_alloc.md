---
slug: midi2i_trampoline_alloc
title: Midi2I Trampoline Alloc
category: hook
status: in_progress
risk: critical
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/in_progress, risk/critical, category/hook, tag/trampoline, tag/i2i, tag/executable-page, tag/allocation, tag/stub, tag/x86_64, tag/hook]
---

# Midi2I Trampoline Alloc

> **Category:** [[categories/hook|Hooking machinery (install / dispatch / trampolines)]]  ·  **Status:** `in_progress`  ·  **Risk:** `critical`  ·  **Specialist:** `.claude/agents/midi2i_trampoline_alloc-specialist.md`

## Description

`vmhook::hotspot::midi2i_hook` is the low-level machinery that makes every hook fire.
The constructor calls `find_hook_location()` (via the install site in `hook<T>()`) to
locate the i2i injection point — a `mov BYTE PTR [r15+disp32],imm8` thread-state write
that is present on every supported JDK — then calls `allocate_nearby_memory()` to obtain
an executable page within 32-bit relative-JMP range of that injection point.  The
trampoline layout written into the page is: an 8-byte copy of the original bytes being
overwritten, followed by a hand-assembled x64 stub that saves volatile registers, calls
`common_detour(frame*, java_thread*, return_slot*)`, branches on the cancel flag, and
either returns the custom value or JMPs back to `target+8` (or a chained prior
trampoline) to resume normal interpreter execution.  The detour pointer and the
resume-JMP rel32 are baked into the stub at fixed ABI-specific byte offsets.  After the
stub is built, the page is made execute-read, the injection point is made writable, a
5-byte `0xE9 + rel32` is stamped over the original bytes, and the target page is
restored to execute-read.  `~midi2i_hook()` reverses the patch (if byte 0 is still
`0xE9`) and releases the trampoline page via `os::release()`.  One trampoline serves all
methods that share the same i2i stub; `hook<T>()` reuses an existing entry when the i2i
pointer is already in `g_hooked_i2i_entries`.  Wrong offsets, a lost page-protect, or a
mis-assembled stub produce incorrect method dispatch or a JVM crash on first hook fire.

## Depends on

- [[features/os_allocate_release|os_allocate_release]]
- [[features/os_protect|os_protect]]
- [[features/os_query_region|os_query_region]]
- [[features/os_page_size_granularity|os_page_size_granularity]]
- [[features/hook_common_detour_dispatch|hook_common_detour_dispatch]]

## Related

- [[features/hook_basic|hook_basic]]
- [[features/hook_chaining|hook_chaining]]
- [[features/hook_verify_repair|hook_verify_repair]]
- [[features/shutdown_hooks_teardown|shutdown_hooks_teardown]]
- [[features/method_entry_points_i2i_i2c|method_entry_points_i2i_i2c]]

## Depended on by

- [[features/hook_basic|hook_basic]]
- [[features/hook_chaining|hook_chaining]]
- [[features/hook_unhook_double_free|hook_unhook_double_free]]
- [[features/hook_verify_repair|hook_verify_repair]]

## Implementation anchors

- `vmhook::hotspot::find_hook_location` — `vmhook/ext/vmhook/vmhook.hpp:5621-5708` — scans the i2i stub for the full 4-spill+thread-state-write pattern (JDK 8-early 21) or the fallback thread-state-write pattern (JDK 21+); also back-scans for locals_offset; returns the injection point or nullptr
- `vmhook::hotspot::allocate_nearby_memory` — `vmhook/ext/vmhook/vmhook.hpp:5720-5848` — walks process address space via os::query_region, allocates an RWX page within INT32_MAX of nearby_addr using os::allocate_rwx; rejects null/zero, clamps search window to user address space bounds
- `vmhook::hotspot::midi2i_hook` — `vmhook/ext/vmhook/vmhook.hpp:6418-6953` — complete class: constructor bakes the Windows x64 or SysV AMD64 trampoline stub (assembly arrays at 6483/6579), flips page protections, stamps the 5-byte JMP; destructor restores and releases; verify_and_repair() re-applies and re-chains
- `vmhook::hotspot::midi2i_hook (constructor)` — `vmhook/ext/vmhook/vmhook.hpp:6434-6751` — allocates trampoline (6649), bakes je-delta (6665-6666), computes effective_resume (6682-6686), bakes resume-JMP rel32 (6688-6690), bakes detour pointer (6692), memcpys original+assembly (6694-6695), flips protections (6707-6730), stamps 5-byte JMP (6732-6734), flushes I-cache (6746-6747)
- `vmhook::hotspot::midi2i_hook::rewrite_chain_resume` — `vmhook/ext/vmhook/vmhook.hpp:6913-6945` — rewrites the resume-JMP rel32 in-place in the live trampoline; private copy of RESUME_JMP_OFFSET (0x73 Windows / 0x74 SysV) that must stay in sync with the ctor constants
- `vmhook::hotspot::i2i_hook_data / g_hooked_i2i_entries` — `vmhook/ext/vmhook/vmhook.hpp:6997-7001` — one entry per unique i2i stub; hook<T>() reuses existing entry (10209-10216) rather than allocating a second trampoline; shutdown_hooks() deletes every hook* (10988-10991) then clears the vector (11042)

## Known bugs

- **[high]** Two hand-maintained copies of the resume-JMP byte offsets — ctor constants RESUME_JMP_OFFSET (0x73 Windows / 0x74 SysV, lines 6476/6575) vs. rewrite_chain_resume's private copy (lines 6921/6924) — can silently drift if the assembly array is edited and only the ctor constants are updated. verify_and_repair() would then patch the resume rel32 at the wrong byte offset, corrupting the live trampoline mid-flight. No static_assert ties them together.
- **[high]** No self-validation that the baked offsets match the emitted assembly bytes. The je-delta write (6666), detour pointer bake (6692), and resume rel32 (6690) all assume JE_OFFSET, DETOUR_ADDRESS_OFFSET, and RESUME_JMP_OFFSET land on specific instruction bytes. A one-byte edit to the assembly array shifts every landmark; the constructor still sets error=false and returns a 'successful' hook that jumps to garbage.
- **[medium]** Constructor swallows the allocation-failure exception instead of propagating it (6652-6663 catch and return). The class comment at 6444-6448 explicitly anticipates direct callers outside the hook<T> gateway who expect throw-on-failure; they receive a silent error-state object instead.
- **[medium]** Destructor's 'byte 0 still 0xE9' restore guard (6780-6788) mis-fires under chaining: if a later hooker overwrote our injection point with their own 0xE9+rel32, byte 0 is still 0xE9, so we memcpy our stale saved bytes over their live JMP — silently un-chaining them. The check should compare all 5 bytes against our expected JMP (as verify_and_repair already does).
- **[medium]** verify_and_repair's re-chain (6860-6869) can adopt a since-unloaded hooker's trampoline: is_valid_pointer is a readability/region check, not a live-trampoline provenance check. A freed DLL whose page is still mapped execute passes the gate; a two-hop A->B->A cycle through a sibling is not detected.
- **[low]** target+HOOK_SIZE resume assumes the injection window is exactly 8 bytes (HOOK_SIZE=8, line 6463). find_hook_location returns the start of the thread-state-write instruction (41 C6 87 disp32 imm8 = 8 bytes), which happens to be correct for all observed JDKs. A future JDK encoding the same write in fewer or more bytes would cause the trampoline to copy a split instruction and resume mid-instruction without any length-decode guard.
- **[low]** allocate_nearby_memory page-walk (5817-5845) can issue up to ~512K os::query_region calls on a fully-reserved 2 GB span before giving up (the page_size fallback at 5824 combined with the ±INT32_MAX window). Correct but can stall for multiple milliseconds on first install in a dense address space.

## Notes

Platform notes:
- On Windows, os::allocate_rwx wraps VirtualAlloc(MEM_RESERVE|MEM_COMMIT, PAGE_EXECUTE_READWRITE);
  os::protect wraps VirtualProtect; os::release wraps VirtualFree(MEM_RELEASE). The two-phase
  protect sequence (allocate RWX -> write stub -> flip to execute-read, then flip target to
  execute-rw -> stamp JMP -> flip back) is required because W^X hardening (e.g. CET/DEP) can
  forbid simultaneous W+X; the implementation obtains RWX only for the brief write window.
- On POSIX (Linux/macOS), os::allocate_rwx wraps mmap(PROT_READ|PROT_WRITE|PROT_EXEC) and
  os::protect wraps mprotect. The two-phase protect sequence is identical.
- The SysV AMD64 trampoline (6579-6645) uses rdi/rsi/rdx for the three arguments and has no
  shadow space; the Windows x64 trampoline (6483-6544) uses rcx/rdx/r8 and allocates 0x20
  bytes of shadow space before the call. Both are selected at compile time via VMHOOK_OS_WINDOWS.
- arm64 / iOS builds (!VMHOOK_RUNTIME_HOOKING_AVAILABLE) reach the early-return at 6454-6461
  and leave error==true; hook<T>() throws and no trampoline is allocated. Clean no-op on every
  unsupported ABI and JDK.
- Trampoline lifetime is tied exclusively to shutdown_hooks(): hook_handle::stop() deliberately
  leaves the shared stub patch in place (one trampoline serves N methods on the same i2i stub).
  Processes that inject-and-forget without calling shutdown_hooks() keep every trampoline page
  mapped for the life of the JVM — expected behaviour, but worth documenting as a contract.
- JDK version sensitivity: find_hook_location returns nullptr on any JDK whose i2i stub lacks
  both pattern_full and pattern_fallback. JDK 26+ validation should confirm the thread-state
  write is still encoded as 41 C6 87 disp32 imm8 (8 bytes) so HOOK_SIZE=8 remains correct.
