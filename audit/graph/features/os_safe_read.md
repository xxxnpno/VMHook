---
slug: os_safe_read
title: Os Safe Read
category: os
status: in_progress
risk: critical
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/in_progress, risk/critical, category/os, tag/fault-safe, tag/seh, tag/sigaction, tag/cold-page, tag/safe-read, tag/os, tag/no-access, tag/cross-platform]
---

# Os Safe Read

> **Category:** [[categories/os|OS abstraction (memory / signals / breakpoints)]]  ·  **Status:** `in_progress`  ·  **Risk:** `critical`  ·  **Specialist:** `.claude/agents/os_safe_read-specialist.md`

## Description

vmhook::os::safe_read(void* dst, const void* src, std::size_t size) is the
cross-platform, fault-tolerant memory-read primitive that every HotSpot
introspection helper uses to dereference untrusted JVM pointers — OOP
candidates, Klass*, saved-rbp frame slots, raw heap words — without crashing
when the source page is no-access, freed, or unmapped.  Input contract: dst
and src must be non-null, size must be > 0 and must not wrap the address
space; any violation returns false immediately without an OS call.  Output
contract: returns true iff ALL size bytes were transferred to dst; a partial
transfer (read straddles a readable page into a no-access page) returns false
— all-or-nothing.  Platform split: Windows uses ReadProcessMemory on the
current process (kernel-validated, never faults); macOS uses
mach_vm_read_overwrite (same guarantee); Linux/Android try process_vm_readv
first, then fall back to a SIGSEGV/SIGBUS-catching sigsetjmp probe via
detail_signal::install_once; iOS falls back to a raw std::memcpy (no
fault-safe API available without entitlements — bad pointer WILL fault).  This
is the last safety net before a crash on unmapped or cold pages; if it ever
lets a fault escape the entire library's crash-proof promise collapses and the
host JVM dies.

## Depends on

- [[features/os_signal_handler|os_signal_handler]]
- [[features/seh_invoke_detour|seh_invoke_detour]]

## Depended on by

- [[features/decode_oop_and_pointers|decode_oop_and_pointers]]
- [[features/vmstructs_offset_resolution|vmstructs_offset_resolution]]

## Implementation anchors

- `vmhook::os::safe_read` — `vmhook/ext/vmhook/vmhook.hpp:926-991` — main entry point — null/zero/wrap guard then Windows RPM / macOS mach / Linux process_vm_readv+sigsetjmp / iOS raw memcpy / unknown false
- `vmhook::os::detail_signal::probe_state` — `vmhook/ext/vmhook/vmhook.hpp:871-913` — Linux/Android signal machinery — probe_state struct, thread_local active_state, SIGSEGV/SIGBUS handler, install_once
- `vmhook::hotspot::safe_read_pointer` — `vmhook/ext/vmhook/vmhook.hpp:2077-2101` — primary HotSpot caller — pre-filters null/floor/ceiling/alignment then delegates to safe_read; used by every Klass/Symbol/class-loader-data walk

## Tests

- `tests/test_os_safe_read.cpp`
- `tests/test_os_protect_interaction.cpp`

## Audit docs

- `audit/NO_SEH_COLDREAD_HARDENING.md`

## Known bugs

- **[critical]** iOS safe_read violates the no-fault contract and always returns true (vmhook.hpp:957-962). The platform arm does a raw std::memcpy and unconditionally returns true — a bad src faults the process (the opposite of the contract) and a caller like safe_read_pointer treats the true return as "bytes in dst are valid". On iOS safe_read is a misnomer; it should return false like the unknown-platform arm.

- **[high]** clang+mingw (no-SEH) Windows has no working fault net (vmhook.hpp:926-991). The Windows arm uses ReadProcessMemory which is kernel-safe, but clang-cl and MinGW toolchains lack working __try/__except (seh_invoke is gated to real cl.exe per cold_fault_28_root_cause memory). Any raw deref that reaches a cold page on those toolchains faults uncontained before safe_read is even called. safe_read itself is correct but callers that bypass it (the residual raw derefs catalogued in audit/NO_SEH_COLDREAD_HARDENING.md) are the live hazard; safe_read is the fix target for all of them.

- **[medium]** Asymmetric handler self-disarm on Linux/Android (vmhook.hpp:895-898): when a genuine fault fires outside a probe, the handler resets SIGSEGV to SIG_DFL but does NOT reset SIGBUS. A genuine SIGBUS with no active probe loops forever. The fix is to reset whichever signal actually fired (use the int sig argument, currently ignored at line 887).

- **[medium]** detail_signal::install_once saves no previous handler (vmhook.hpp:900-912): sigaction is called with nullptr oldact, permanently replacing HotSpot's own SIGSEGV handler (which drives implicit null-check, safe-point polling, and guard-page stack-overflow recovery). After the first Linux fallback entry, HotSpot's handler is gone for the process lifetime and is never restored or chained. Robust design: save oldact and chain when not in a probe.

- **[low]** process_vm_readv partial-read leaves dst half-written on false return (vmhook.hpp:966-970). When process_vm_readv returns 0 < n < size the code falls through to the sigsetjmp memcpy path over the full original range; the partial bytes written by the kernel remain in dst. The all-or-nothing bool contract is satisfied but dst is torn on a false return; callers that inspect dst after false see stale+new bytes. The API contract should state dst is unspecified when the call returns false.


## Notes

INCIDENT #28 (cold_fault_28_root_cause memory): the #28 quarantine of
clang+mingw windows was triggered by the watchdog thread's raw memcmp of
Method->get_name() calling constant_pool::get_length() which did a raw int32
deref of ConstantPool::_length on a potentially-cold page. safe_read was the
correct fix but the raw caller had to be hardened — not safe_read itself.
commit c6ae54f ("fix(lib): harden 8 residual cold VM-pointer reads/writes")
converted those raw derefs to safe_read, resolving #28 and un-quarantining
clang+mingw (PR #4, 0420c40).

The root cause pattern: any accessor that does is_valid_pointer(this) (a
range/alignment heuristic) then a raw deref is vulnerable — is_valid_pointer
never probes the page. safe_read is the correct replacement on all platforms
for metadata reads (not frame-slot walks, which have a POSIX regression risk
for the stack_trace path and must stay Windows-gated). See
audit/NO_SEH_COLDREAD_HARDENING.md for the full ordered fix list.

No-SEH Windows (clang-cl, MinGW): these toolchains have no working SEH
(__try/__except is gated to real cl.exe via seh_invoke_detour). safe_read via
ReadProcessMemory IS fault-safe there, but any caller that bypasses safe_read
and raw-derefs a cold pointer crashes uncontained. This is the live hazard
that #28 surfaced; safe_read's own Windows arm is correct.

iOS note: safe_read on iOS is a lie — raw memcpy + return true. The library
documents HotSpot-only and iOS ships no HotSpot, so this is latent but the
function name and noexcept signature actively misrepresent the platform's
behaviour.

The all-or-nothing contract (transferred == size) is correct and intentional:
a read that straddles into a no-access page cannot be partially trusted, so
false is the only safe return even though the readable prefix was available.
