---
slug: hook_chaining
title: Hook Chaining
category: hook
status: in_progress
risk: high
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/in_progress, risk/high, category/hook, tag/hook, tag/chaining, tag/shared-i2i-stub, tag/demux, tag/one-trampoline-per-entry, tag/first-match, tag/per-method-counts, tag/single-entry-drop, tag/install-order-independence]
---

# Hook Chaining

> **Category:** [[categories/hook|Hooking machinery (install / dispatch / trampolines)]]  ·  **Status:** `in_progress`  ·  **Risk:** `high`  ·  **Specialist:** `.claude/agents/hook_chaining-specialist.md`

## Description

Installing several `vmhook::scoped_hook<T>` on DIFFERENT Java methods at once, all of which share the
ONE HotSpot interpreter-to-interpreter (i2i) stub vmhook patches a single time, and proving the single
shared `common_detour` demultiplexes a mixed call stream so each method's detour fires for ITS method
only — exactly once per call, with the correct frame, never cross-firing onto a sibling. The install
path checks `g_hooked_i2i_entries` for an already-patched i2i entry and REUSES it, allocating a new
`midi2i_hook` only when the entry is new: N methods sharing an i2i stub get N entries in
`g_hooked_methods` but only ONE patched stub / ONE trampoline. Per-method teardown
(`hook_handle::stop()`) erases just that method's entry and DELIBERATELY leaves the shared trampoline in
place, so dropping one handle leaves the siblings firing. ("chaining" here is the in-process shared-stub
consequence, NOT the cross-DLL `chain_resume` path.)

## Depends on

- [[features/hook_basic|hook_basic]]
- [[features/midi2i_trampoline_alloc|midi2i_trampoline_alloc]]
- [[features/hook_common_detour_dispatch|hook_common_detour_dispatch]]

## Related

- [[features/hook_basic|hook_basic]]
- [[features/hook_verify_repair|hook_verify_repair]]
- [[features/dont_inline_dont_compile|dont_inline_dont_compile]]
- [[features/hook_install_after_jit|hook_install_after_jit]]
- [[features/hook_reinstall_after_shutdown|hook_reinstall_after_shutdown]]
- [[features/hook_signature|hook_signature]]

## Implementation anchors

- `hook<T> install — i2i reuse-or-allocate` — `vmhook/ext/vmhook/vmhook.hpp:10268-10330` — scans g_hooked_i2i_entries for an already-patched entry and REUSES it; only on a new entry calls find_hook_location (10279) and allocates a new midi2i_hook(target, common_detour, chain_resume) (10328), pushing into g_hooked_i2i_entries (10335)
- `i2i_hook_data` — `vmhook/ext/vmhook/vmhook.hpp:7026-7034` — associates one i2i entry point with its single midi2i_hook — the structural 'one trampoline per unique i2i entry' record
- `g_hooked_methods + g_hooked_i2i_entries` — `vmhook/ext/vmhook/vmhook.hpp:7049-7124` — N-methods registry (7049) vs the per-i2i-entry trampoline registry (7120); every install push_back's to g_hooked_methods but reuses a shared i2i entry
- `common_detour first-match demux` — `vmhook/ext/vmhook/vmhook.hpp:7138-7210` — the single shared detour; linear-scans g_hooked_methods and on FIRST hook.method == current_method fires that detour once and returns — a method with no entry simply never matches
- `hook_handle::stop() — per-entry teardown keeps shared trampoline` — `vmhook/ext/vmhook/vmhook.hpp:11128-11175` — erases just this method's g_hooked_methods entry + clears its _dont_inline / NO_COMPILE, DELIBERATELY leaves the midi2i_hook trampoline installed because siblings may still share the i2i entry; common_detour simply skips a method missing from g_hooked_methods

## Tests

- `tests/jvm/modules/hook_chaining.cpp`

## Known bugs

- **[high]** common_detour iterates g_hooked_methods lock-free (contract at vmhook.hpp:7049-7060); installing a NEW hook on another method push_back's to that vector and can reallocate it while a sibling method's detour is mid-iteration — UAF on the hook.detour std::function cell. The install mutex does not cover the reader. In the chaining scenario this is the central hazard because multiple hooks are added/dropped while the shared stub is live. Reserving capacity or a stable-address container closes it (shared with hook_basic / hook_common_detour_dispatch).
- **[medium]** First-match-and-return means only ONE g_hooked_methods entry per Method* can ever fire — if two distinct hooks were registered for the same Method* (a misuse), the second is silently dead. Correct for the documented one-hook-per-method model but undiagnosed; the duplicate-membership short-circuit at install partly guards this by returning early on a method already in g_hooked_methods.

## Notes

Naming: "chaining" is the in-process SHARED-STUB consequence (many methods, one patched i2i stub, one
trampoline, one common_detour demux), NOT the cross-DLL chain_resume path (a foreign DLL's 0xE9 JMP
detected at find_hook_location and resumed after our detour). Module proofs: per-method fire counts;
no cross-firing onto a sibling; single-entry drop (drop one handle, the others keep firing — the proof
at the heart of leave-the-shared-trampoline teardown); install-order independence; allow-through; and
per-detour return override. The first-match -> one-fire -> return structure of common_detour is the
guarantee of "each detour fires for its method only, exactly once per call". Tests cover JDK 8..26
HotSpot. Hooks via scoped_hook<T> that disarm on scope exit; the module leaves NOTHING armed and NEVER
calls shutdown_hooks() (would tear down sibling modules' hooks). Java-8-only fixture; MSVC copy-init;
every Method* / decoded-OOP deref gated by is_valid_pointer.
