---
slug: hook_chaining
title: Hook Chaining
category: hook
status: in_progress
risk: high
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/in_progress, risk/high, category/hook, tag/hook, tag/i2i-stub, tag/trampoline, tag/demux, tag/multi-method, tag/shared-stub, tag/dispatch, tag/raii]
---

# Hook Chaining

> **Category:** [[categories/hook|Hooking machinery (install / dispatch / trampolines)]]  ·  **Status:** `in_progress`  ·  **Risk:** `high`  ·  **Specialist:** `.claude/agents/hook_chaining-specialist.md`

## Description

Multiple `vmhook::scoped_hook<T>` handles installed on DIFFERENT Java methods that share
the ONE HotSpot interpreter-to-interpreter (i2i) stub cause only a single patch of that
stub and a single trampoline allocation (`midi2i_hook`).  At dispatch time all trampolines
converge on one `common_detour`, which linear-scans `g_hooked_methods` (lock-free) and
fires the first entry whose `hook.method == frame.get_method()` — exactly once per call,
for the owning method only, then returns immediately.  Methods with no entry in the
registry pass through silently.  Per-method teardown (`hook_handle::stop()`) erases only
the one entry and deliberately leaves the shared trampoline in place so sibling hooks keep
firing; the shared stub is only dismantled when all entries are gone (shutdown_hooks()).
A malformed chain — e.g. two entries for the same Method* or a stale Method* address
reused after JVMTI redefinition — mis-routes an entire dispatch into the wrong detour,
corrupting shared global state (`g_hooked_methods`) for every method that shares the stub.

## Depends on

- [[features/hook_basic|hook_basic]]
- [[features/midi2i_trampoline_alloc|midi2i_trampoline_alloc]]
- [[features/hook_common_detour_dispatch|hook_common_detour_dispatch]]
- [[features/method_entry_points_i2i_i2c|method_entry_points_i2i_i2c]]
- [[features/seh_invoke_detour|seh_invoke_detour]]
- [[features/dont_inline_dont_compile|dont_inline_dont_compile]]

## Implementation anchors

- `vmhook::hotspot::i2i_hook_data / g_hooked_i2i_entries` — `vmhook/ext/vmhook/vmhook.hpp:6986-7080` — i2i_hook_data struct (i2i entry + midi2i_hook*) at 6986-6990; g_hooked_i2i_entries vector at 7080; install path checks this vector at 10198-10205 to reuse an already-patched stub rather than patching twice
- `common_detour (lock-free demux)` — `vmhook/ext/vmhook/vmhook.hpp:7098-7176` — single entry point for all trampolines; shutdown bail at 7105-7108; safe_read-gated frame.get_method() at 7129; lock-free linear scan of g_hooked_methods at 7147; first hook.method==current_method match fires seh_invoke_detour at 7157, forces _thread_in_Java at 7167, returns at 7168 — one fire, one method, exact demux
- `g_hooked_methods / g_hooked_methods_mutex (registry + lock-free contract)` — `vmhook/ext/vmhook/vmhook.hpp:7009-7010` — vector and mutex declared together; comment block at 6995-7007 documents the lock-free iteration contract: mutations happen BEFORE detours fire; reallocation mid-iteration is an explicitly-documented UAF hazard
- `i2i install — reuse-or-patch branch` — `vmhook/ext/vmhook/vmhook.hpp:10196-10266` — 10198-10205 scan g_hooked_i2i_entries for the same i2i_entry; 10207 branches — new entry only; 10257-10265 new midi2i_hook(target, common_detour, chain_resume) allocated once per unique stub and pushed; multiple methods sharing that stub accumulate in g_hooked_methods but share the one midi2i_hook
- `hook_handle::stop() — single-entry erase, trampoline preserved` — `vmhook/ext/vmhook/vmhook.hpp:11058-11122` — erases exactly one g_hooked_methods entry under mutex (11073-11108); clears _dont_inline + NO_COMPILE; DELIBERATELY leaves the midi2i_hook trampoline (comment at 11088-11091); sibling entries in g_hooked_methods keep firing; common_detour skips the erased method because it finds no matching entry
- `vmhook::scoped_hook<T>` — `vmhook/ext/vmhook/vmhook.hpp:11191-11311` — RAII wrapper; calls hook<T>() then re-resolves Method* to build hook_handle; already_hooked path at 11217-11224 returns an empty (not-installed) handle to prevent double-disarm across concurrent scoped_hook calls on the same Method*

## Tests

- `tests/jvm/modules/hook_chaining.cpp`

## Known bugs

- **[high]** Lock-free common_detour iteration races push_back when a NEW method is hooked while a SIBLING's detour is mid-flight. g_hooked_methods (vmhook.hpp:7009) is iterated at 7147 without holding g_hooked_methods_mutex; hook<T>()'s push_back at 10194 takes the mutex but common_detour never does. If a user installs a third hook on a method sharing the same i2i stub WHILE another method's detour is executing, push_back can reallocate the vector's backing buffer, invalidating the live iterator and causing a use-after-free on the hook.detour std::function cell. This is a hook_chaining-specific hazard (single-method hook is safe because no sibling can trigger common_detour simultaneously). The library's own contract (7005-7007) requires all installs before detours fire; this is a documented footgun, not a guarded invariant. Mitigation: pre-reserve the vector or snapshot inside common_detour.
- **[medium]** Method*-identity dispatch mis-routes after JVMTI RedefineClasses recycles an address. common_detour matches at vmhook.hpp:7149 purely on hook.method == current_method. If another JVMTI agent redefines a class and the allocator hands the freed Method* of hooked method A back to an unrelated method, calls to that unrelated method match A's g_hooked_methods entry and fire A's detour — a cross-fire invisible to the install-time expected_method_name drift detector (which logs during verify_hooks but does not prevent the dispatch). This is a shared-stub amplifier: one recycled address corrupts demux for every sibling sharing the stub. Out of scope for a single-process test.
- **[low]** O(N) linear scan per intercepted call (vmhook.hpp:7147-7170). Every call to any hooked method scans the full g_hooked_methods vector to first match. hook_chaining is the multi-method scenario where N grows; a hash keyed on Method* would make dispatch O(1) but correctness is unaffected.
- **[low]** First-match-wins makes a duplicate Method* entry permanently shadow later ones (vmhook.hpp:7149, return at 7168). The install short-circuit at 10104-10126 prevents a second live entry for the same Method*, so this cannot happen in practice today; but if that guard were ever bypassed or relaxed, only the first entry would ever fire and the second detour would be silently dead.

## Notes

The "chaining" this feature documents is the in-process shared-stub demux consequence
(many methods, one patched stub, one common_detour, per-method dispatch). This is
distinct from the cross-DLL chain_resume path (10215-10255) where a SECOND injector
overwrites the same 5-byte JMP at the injection point: that is detected at install time
by checking target[0]==0xE9 and baking the prior trampoline address into chain_resume.
Cross-DLL chaining cannot be exercised from a single-process test and is not covered by
the hook_chaining test module.

The test fixture vmhook/fixtures/HookChaining.java exposes six methods spanning every
decode shape: a(int), b(long), c(String), d(double), e() (no-arg), s(int) (static).
The decisive scenario (mode 1) calls all six inside a single run() so every sibling hook
is live on the shared i2i stub at once. Tests assert: exact-once fire per method,
per-method frame validation, zero cross-fire (cross-fire sentinel), allow-through,
drop-one-handle (h_b.stop() leaves a+c firing), install-order independence (c,b,a
reverse order same result), instance+static sharing the stub, no-arg+double-slot
sharing the stub, and isolated per-detour return override (retval.set() rewrites only
the firing method's frame slot, not a global switch). Approximately 110 ctx.check()
assertions.

All hooks are installed BEFORE driving any probe and never while a detour is in flight,
staying inside the lock-free-iteration contract (see known_bug severity:high above).
Detours touch only primitives, std::string, and atomics — no unrooted oop is held
across the probe boundary, so GC-window hazards do not apply.

JDK-version note: the single-trampoline-per-i2i sharing assumes find_hook_location
(called at 10209) can pattern-match the shared stub layout on the running JDK. A JDK
whose stub layout matches neither known pattern returns nullptr, triggering the
throw at 10212, which the install catch converts to an empty handle. Tests run
JDK 8..26 HotSpot. Compressed-OOP decode in extract_frame_arg governs the self/String
decode (default under ~32 GB heaps).
