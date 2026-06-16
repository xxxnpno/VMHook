---
slug: hook_unhook_double_free
title: Hook Unhook / Double-Free Guards
category: hook
status: in_progress
risk: high
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/in_progress, risk/high, category/hook, tag/lifetime, tag/unhook, tag/trampoline, tag/idempotent, tag/double-free, tag/raii, tag/hook-remove, tag/noexcept]
---

# Hook Unhook / Double-Free Guards

> **Category:** [[categories/hook|Hooking machinery (install / dispatch / trampolines)]]  ·  **Status:** `in_progress`  ·  **Risk:** `high`  ·  **Specialist:** `.claude/agents/hook_unhook_double_free-specialist.md`

## Description

`hook_handle::stop()` is the single-hook remove path. It tears down exactly one
installed hook without touching any other: clears `_dont_inline` (via
`set_dont_inline(..., false)`) and `NO_COMPILE` (via fault-safe
`safe_access_flags_and(~NO_COMPILE)`), erases the matching `hooked_method` entry
from `g_hooked_methods` under `g_hooked_methods_mutex`, and deliberately leaves
the shared midi2i trampoline and the captured `_code`/`_from_*_entry` pointers
untouched (the nmethod the sweeper may have flushed makes restoring `_code`
a dangling-pointer crash). Behavioural-exact restore is achieved structurally:
erasing the entry makes `common_detour`'s first-match scan skip the method, so
the i2i allow-through runs the original body. Idempotency is enforced by
nulling `this->method` BEFORE the vector mutation — a second `stop()` or the
destructor's implicit call short-circuits at the `!this->method` gate (line 11060)
and touches nothing. `scoped_hook<T>` signals duplicate installs via
`already_hooked` and returns an empty handle (method == nullptr) so a second
RAII owner can never disarm the first hook's single shared entry, eliminating
the double-free-via-double-erase path entirely.

## Depends on

- [[features/hook_basic|hook_basic]]
- [[features/midi2i_trampoline_alloc|midi2i_trampoline_alloc]]
- [[features/os_protect|os_protect]]
- [[features/shutdown_hooks_teardown|shutdown_hooks_teardown]]

## Related

- [[features/hook_reinstall_after_shutdown|hook_reinstall_after_shutdown]]
- [[features/dont_inline_dont_compile|dont_inline_dont_compile]]
- [[features/hook_chaining|hook_chaining]]
- [[features/hook_install_after_jit|hook_install_after_jit]]
- [[features/hook_signature|hook_signature]]
- [[features/hook_verify_repair|hook_verify_repair]]

## Implementation anchors

- `hook_handle::stop() — idempotency gate + method null + entry erase` — `vmhook/ext/vmhook/vmhook.hpp:11058-11122` — full stop() body; nulls this->method at 11065 before the lock, then find_if for h.method==target; end()-hit at 11081-11084 is the double-free-safe no-op; set_dont_inline(false) + safe_access_flags_and(~NO_COMPILE) + hooks.erase at 11092-11108; entire body is noexcept catch-all
- `hook_handle class — move-only RAII, installed(), destructor` — `vmhook/ext/vmhook/vmhook.hpp:9011-9068` — method field (9067); installed() == (method != nullptr) at 9048-9051; ~hook_handle() calls stop() at 9040-9043; move-ctor steals + nulls at 9023-9027; move-assign calls stop() on *this first at 9033
- `scoped_hook<T> — duplicate-install honest return` — `vmhook/ext/vmhook/vmhook.hpp:11191-11302` — passes &already_hooked to hook<T>() at 11197-11199; on already_hooked==true returns empty hook_handle{} at 11224 so caller cannot believe a distinct detour is installed; re-resolves Method* by walking _methods (11264-11279) to get stable target for future stop()
- `duplicate-membership short-circuit in hook<T>()` — `vmhook/ext/vmhook/vmhook.hpp:10104-10126` — under g_hooked_methods_mutex, linear scan; if found_method already in g_hooked_methods sets *already_hooked=true and returns true WITHOUT installing a second detour — only the first fires (common_detour first-match-and-return)
- `g_hooked_methods.push_back — happens BEFORE i2i install` — `vmhook/ext/vmhook/vmhook.hpp:10194-10214` — push_back at 10194, i2i find_hook_location at 10209-10213; a throw here leaves the pushed entry and the flag mutations orphaned with no rollback, poisoning future hook<T>() calls via the short-circuit above

## Tests

- `tests/jvm/modules/hook_unhook_double_free.cpp`

## Known bugs

- **[high]** Half-installed method permanently poisons re-install. push_back at vmhook.hpp:10194 runs BEFORE the i2i trampoline install (10207-10213). If find_hook_location returns nullptr the throw is caught and returns false, but the pushed hooked_method entry is NEVER erased and the set_dont_inline/NO_COMPILE mutations at 10128-10135 are never rolled back. Every subsequent hook<T>() for the same Method* then hits the duplicate short-circuit at 10104-10126 and returns true (with already_hooked=true), so scoped_hook<T>() hands back an empty handle and the caller sees installed()==false forever while no detour fires. The method is also permanently deopted with no diagnostic. Fix: push_back AFTER successful i2i install, or scope-guard the post-push section to erase on throw — mirroring the already-correct stop() which erases only what find_if actually finds.
- **[high]** Duplicate install silently discards the second detour; both scoped_hook handles alias one entry. vmhook::hook<T>() short-circuit at vmhook.hpp:10104-10126 returns true without installing the second user_detour. scoped_hook<T>() at 11217-11224 correctly detects already_hooked and returns an empty handle (installed()==false), so the double-disarm crash is closed. However if the duplicate path was reached through the raw hook<T>() API (not scoped_hook), the caller has no way to distinguish 'fresh install' from 'method already owned' and may incorrectly believe a distinct detour is live. The stop() safety invariant is maintained — the surviving handle's stop() hits the entry and erases it; the second handle is already empty.
- **[medium]** Restore is intentionally partial: _code / _from_compiled_entry / _from_interpreted_entry are never written back. hook_handle::stop() at vmhook.hpp:11100-11106 deliberately skips these because the captured nmethod may have been flushed by the JVM sweeper between install and remove. Writing the stale pointer back would hand the JVM a dangling code-cache address (0x10??????-range AV). Behavioural restore is correct via allow-through, but a structural inspector of Method._code after stop() sees a permanently-deopted method, not the original JIT pointer. This is intentional and must not be 'fixed' back into the dangling-pointer crash.
- **[medium]** _dont_inline clear in stop() silently no-ops if the Method _flags VMStruct is absent. set_dont_inline(false) at vmhook.hpp:11092 returns early on a null get_flags() (set_dont_inline body gates on VMStruct resolution). On a future/patched JVM where _flags is unresolved but get_access_flags() succeeds, the NO_COMPILE bit is cleared (11098-11099) but the inline-guard bit stays set — the just-removed method is permanently un-inlinable with no diagnostic. Symmetric with the install-side half-apply (10128-10135). Fix: surface a VMHOOK_LOG warning when get_flags() returns null rather than silently returning.
- **[low]** In-flight-callback race on stop() is caller-contracted, not enforced. stop() acquires g_hooked_methods_mutex at vmhook.hpp:11073, but common_detour reads g_hooked_methods lock-free (see comment block ~6996-7028) and holds a reference to the hook.detour std::function across seh_invoke_detour. If a stop() races a live dispatch, hooks.erase(entry_it) at 11108 can destroy the std::function cell a detour is mid-call on — UAF. The class doc at 9002-9009 contracts the caller to ensure no Java thread is inside the hooked method when the handle is dropped. The test module avoids this by calling stop() only between probe cycles, never concurrently with a dispatch.

## Notes

JDK-version sensitivities. The _dont_inline clear path in stop() was hardened
in commit 16e75d9 (#37): the CLEAR-path atomic_ref RMW was replaced by a
width-correct safe_read+recompute+safe_write (safe_access_flags_and) so a
stop() against a Method* relocated by JVMTI RedefineClasses no longer AVs on
the no-SEH legs (clang/mingw). The NO_COMPILE clear at vmhook.hpp:11098-11099
uses the same safe_access_flags_and path; a cold page simply skips the clear.

The "no _code restore" decision is uniform across JDK 8..26 because the
nmethod sweeper behaviour (flushing compiled methods) is present on all
supported HotSpot versions. Byte-exact behavioural restore relies on
allow-through, not structural entry restoration.

shutdown_hooks() at vmhook.hpp:10954-11055 is the belt-and-braces bookend: it
drains the watchdog, restores _dont_inline/NO_COMPILE for every remaining
entry (same deliberate skip of _code), clears both vectors, and — critically
for re-arm — resets g_started and g_shutdown_requested to false so a
post-shutdown install is live again. This module calls shutdown_hooks() at
open and close to reason about hook counts from a known-clean table.

The midi2i trampoline (g_hooked_i2i_entries) is intentionally left installed
by stop() — other methods may share the same i2i_entry, and common_detour
simply skips methods absent from g_hooked_methods. The trampoline is only
torn down by shutdown_hooks(). This is why stop() alone does not leak the
trampoline, and why re-installing after stop() safely reuses the existing
i2i_already_patched entry (vmhook.hpp:10197-10205) rather than double-patching.

Compressed-OOP self decode in the detour (extract_frame_arg <= 0xFFFFFFFF
heuristic) is exercised only when UseCompressedOops is on (default under
~32 GB heaps); the static-method scenarios in the test module bypass this
path. seh_invoke_detour is __try/__except only under real MSVC; clang/mingw
fall through to plain catch(...) which cannot trap a hardware AV, so all
in-detour cold reads on those toolchains must be safe_read-guarded.
