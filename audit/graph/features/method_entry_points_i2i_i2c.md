---
slug: method_entry_points_i2i_i2c
title: Method Entry Points I2I / I2C
category: hook
status: in_progress
risk: critical
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/in_progress, risk/critical, category/hook, tag/i2i, tag/i2c, tag/entry-point, tag/method, tag/interpreter, tag/deopt, tag/hotspot, tag/jdk-version]
---

# Method Entry Points I2I / I2C

> **Category:** [[categories/hook|Hooking machinery (install / dispatch / trampolines)]]  ·  **Status:** `in_progress`  ·  **Risk:** `critical`  ·  **Specialist:** `.claude/agents/method_entry_points_i2i_i2c-specialist.md`

## Description

Accessor layer over the HotSpot `Method` object's four dispatch entry-point fields:
`_i2i_entry` (the interpreter-to-interpreter stub, the patch target), `_from_interpreted_entry`
(the slot an interpreted caller reads; equals `_i2i_entry` on a pure-interpreted method but
points at the i2c adapter when a compiled nmethod is active), `_from_compiled_entry` /
`_from_compiled_code_entry_point` (read by compiled callers on every virtualcall; normally the
nmethod prologue, reset to the c2i adapter during deopt), and `_code` (the live nmethod pointer,
cleared to nullptr to suppress compiled dispatch without freeing the nmethod).  The hook install
path patches the i2i stub via `midi2i_hook`, then — when `_code != nullptr` — writes
`_from_interpreted_entry = i2i`, `_from_compiled_entry = c2i`, `_code = nullptr` in that order
to force all call paths through the interpreter and therefore through the patched stub.
Confusing i2i with i2c is the central hazard: writing `_from_interpreted_entry = i2c` (instead
of i2i) leaves the hook permanently silent, while writing `_from_compiled_entry = i2i` (instead
of c2i) routes a compiled caller into the interpreter entry stub mid-frame and crashes the JVM.
Every read goes through `os::safe_read` (fault-safe on all platforms) to survive the watchdog
thread polling a possibly-relocated Method pointer.

## Depends on

- [[features/vmstructs_offset_resolution|vmstructs_offset_resolution]]

## Related

- [[features/adapter_recovery_c2i|adapter_recovery_c2i]]
- [[features/midi2i_trampoline_alloc|midi2i_trampoline_alloc]]
- [[features/hook_basic|hook_basic]]
- [[features/hook_install_after_jit|hook_install_after_jit]]
- [[features/dont_inline_dont_compile|dont_inline_dont_compile]]
- [[features/deoptimize_methods|deoptimize_methods]]

## Depended on by

- [[features/adapter_recovery_c2i|adapter_recovery_c2i]]
- [[features/deoptimize_methods|deoptimize_methods]]
- [[features/hook_basic|hook_basic]]
- [[features/hook_chaining|hook_chaining]]
- [[features/hook_common_detour_dispatch|hook_common_detour_dispatch]]
- [[features/hook_install_after_jit|hook_install_after_jit]]

## Implementation anchors

- `vmhook::hotspot::method::get_i2i_entry` — `vmhook/ext/vmhook/vmhook.hpp:2605-2649` — Reads Method._i2i_entry via VMStructs offset; throw->catch->nullptr on absent VMStruct; os::safe_read fault-safe
- `vmhook::hotspot::method::get_from_interpreted_entry` — `vmhook/ext/vmhook/vmhook.hpp:2654-2691` — Reads Method._from_interpreted_entry; same throw->catch->nullptr shape; equals i2i only when not compiled
- `vmhook::hotspot::method::set_from_interpreted_entry` — `vmhook/ext/vmhook/vmhook.hpp:3159-3179` — Writes _from_interpreted_entry; noexcept silent no-op if VMStruct absent; os::safe_write on watchdog path
- `vmhook::hotspot::method::get_from_compiled_entry` — `vmhook/ext/vmhook/vmhook.hpp:3188-3226` — Dual-name resolve: _from_compiled_code_entry_point (JDK<=20) then _from_compiled_entry (JDK 21+); noexcept; os::safe_read
- `vmhook::hotspot::method::set_from_compiled_entry` — `vmhook/ext/vmhook/vmhook.hpp:3233-3264` — Writes compiled entry; same dual-name resolve; noexcept; os::safe_write; set BEFORE set_code(nullptr)
- `vmhook::hotspot::method::get_code / set_code` — `vmhook/ext/vmhook/vmhook.hpp:3076-3160` — get_code: os::safe_read; set_code(nullptr): deopt trigger — written LAST after fie and fce are stable
- `vmhook::hotspot::method::get_adapter` — `vmhook/ext/vmhook/vmhook.hpp:3282-3360` — JDK 8 fast-path via VMStructs; JDK 9+ heuristic scan cached in atomic<size_t> (0 = not-yet-found sentinel)
- `vmhook::hotspot::hooked_method (snapshot fields)` — `vmhook/ext/vmhook/vmhook.hpp:6961-6968` — original_from_interpreted_entry / original_from_compiled_entry / was_compiled — snapshotted at install; deliberately NOT restored at teardown (dangling-nmethod hazard)
- `hook() deopt sequence` — `vmhook/ext/vmhook/vmhook.hpp:10298-10333` — Canonical fie->fce->code(nullptr) ordering; forced-deopt fallback (fie+code only) when c2i unrecoverable

## Tests

- `tests/jvm/modules/hook_install_after_jit.cpp`
- `tests/jvm/modules/deoptimize_methods.cpp`
- `tests/jvm/modules/dont_inline_dont_compile.cpp`

## Known bugs

- **[high]** original_from_interpreted_entry / original_from_compiled_entry / was_compiled are snapshotted at install (vmhook.hpp:10156-10158, stored at 6966-6968) but every teardown path (shutdown_hooks, hook_handle::stop) deliberately ignores them — the restore the field names promise never happens.  The non-restore is correct-by-design (the nmethod may have been swept, making the saved _code / _from_compiled_entry dangling), but the saved from_* values are dead state that will mislead any future maintainer who "fixes" teardown to write them back, reintroducing a post-uninject AV cascade.  At minimum the snapshot of the two from_* entries is wasted work; at worst it is a footgun for the next contributor.

- **[high]** validate_adapter_handler_entry (vmhook.hpp:7760-7788) hard-codes _i2c_entry at AHE offset 0 instead of resolving it via iterate_struct_entries("AdapterHandlerEntry", "_i2c_entry"). If a future JDK adds a field before _i2c_entry, or the first-member assumption breaks, validation reads the wrong slot: a spurious pass caches a wrong _adapter offset process-wide and every subsequent deopt writes a non-c2i pointer into _from_compiled_entry, causing a JVM crash at the next compiled callsite.

- **[medium]** The full-byte heuristic scan in detect_adapter_offset_from_method (vmhook.hpp:7800-7860) can pick a wrong slot that happens to validate: any Method field that holds a pointer to a struct whose first two pointer-sized slots both land in committed+executable memory is accepted. The skip-set (six known fields, capped at 8 entries) does not cover all Method fields. The preferred-guess path (slot before _from_compiled_entry) already handles the common case; the brute scan is a high-consequence tail: one wrong cached offset corrupts c2i recovery for the entire process lifetime.

- **[medium]** get_adapter() uses cached_offset == 0 as the "not yet found" sentinel (vmhook.hpp:3334-3336). If any JDK ever places Method._adapter at byte offset 0, the cache would never latch and get_adapter() would re-probe on every call forever.  No current JDK does this (offset 0 is the vtable / _constMethod region), so it is latent rather than active; a SIZE_MAX sentinel or a separate bool would make the code correct rather than coincidentally safe.

- **[medium]** set_from_interpreted_entry / set_from_compiled_entry / set_code write plain pointer-sized stores (via os::safe_write, but still non-atomic with respect to the JVM dispatch loop) while HotSpot interpreter dispatch reads these fields concurrently (vmhook.hpp:3175, 3260, 3128-3160).  On x86_64 a naturally-aligned pointer store is architecturally atomic, and the fie->fce->code ordering ensures _code==null is published last; but the ordering is enforced only by source order and os::safe_write, not by C++ release semantics or a memory fence. A reordering-permissive compiler or a non-x86 port could publish _code==null before the entry-point writes, opening a window where a compiled caller jumps into a stale nmethod after _code reports null.

- **[low]** Asymmetric error reporting: get_i2i_entry / get_from_interpreted_entry log via VMHOOK_LOG through the throw->catch path (vmhook.hpp:2644-2647, 2686-2689); the setters and get_from_compiled_entry / get_code are noexcept and silently no-op when their VMStruct is missing (vmhook.hpp:3162-3165, 3203-3205, 3080-3082, 3132-3134).  On a future JVM where _from_compiled_entry cannot be resolved, deopt writes fie=i2i, silently skips fce, then set_code(nullptr) — producing the null-code / stale-compiled-entry combination that the install-path comment (vmhook.hpp:10295-10297) explicitly calls a JVM crash at the next safepoint, with no log line indicating what went wrong.


## Notes

JDK field naming: on JDK <= 20 the compiled entry field is exported as
_from_compiled_code_entry_point; on JDK 21+ it was renamed to _from_compiled_entry.
get_from_compiled_entry / set_from_compiled_entry try both names via a lambda-init static
(vmhook.hpp:3191-3201, 3236-3246), and detect_adapter_offset_from_method's preferred-guess
path also tries both (vmhook.hpp:7856-7859).  A future third rename silently disables the
fast path and forces the brute byte scan.

JDK 8 vs 9+: Method._adapter is exported via VMStructs on JDK 8, giving get_adapter() a
direct read.  JDK 9 dropped the field from VMStructs, so all c2i recovery on JDK 9..26
depends entirely on the heuristic detect_adapter_offset_from_method scan.  The AdapterHandlerEntry
fields (_c2i_entry, _i2c_entry) remain exported on JDK 8..26, which is why
get_c2i_entry_from_adapter can be simple while the Method._adapter lookup cannot.

The deopt write ordering (set_from_interpreted_entry -> set_from_compiled_entry -> set_code)
is load-bearing: _code == nullptr is published last so no window exists where compiled
dispatch finds null-code but still-stale entry pointers.  The forced-deopt fallback
(set_from_interpreted_entry + set_code only, when c2i is unrecoverable) accepts that compiled
inline caches briefly bypass the hook until HotSpot repairs them at the next safepoint.

Teardown deliberately does NOT restore _from_interpreted_entry / _from_compiled_entry / _code
from the snapshotted originals: the nmethod pointed to by original_code may have been freed
by the JIT sweeper between install and teardown.  Writing a dangling nmethod pointer back
into _from_compiled_entry would crash the JVM at the next compiled callsite.

Abstract and native methods: get_i2i_entry() returns nullptr for abstract methods (no
interpreter stub) and may return a native-specific stub for native methods.  Callers must
check for null before installing; midi2i_hook treats a null i2i as a non-hookable method.

No dedicated test module exists for this accessor layer.  It is exercised only transitively
by hook_install_after_jit, deoptimize_methods, and dont_inline_dont_compile.  A dedicated
method_entry_points.cpp module is the next required investment; see the specialist agent for
the full list of test angles (round-trip, deopt invariant, c2i recovery, adapter cache-latch,
null-path safety, dual-name coverage).
