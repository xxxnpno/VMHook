---
slug: hook_basic
title: Hook Install (basic)
category: hook
status: in_progress
risk: critical
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/in_progress, risk/critical, category/hook, tag/hook, tag/runtime, tag/install, tag/i2i-stub, tag/trampoline, tag/critical-path, tag/x86_64, tag/interpreter-dispatch, tag/deopt, tag/raii]
---

# Hook Install (basic)

> **Category:** [[categories/hook|Hooking machinery (install / dispatch / trampolines)]]  ·  **Status:** `in_progress`  ·  **Risk:** `critical`  ·  **Specialist:** `.claude/agents/hook_basic-specialist.md`

## Description

Install path for the typed-callback hook (`vmhook::hook<T>(name[, signature], cb)`
and its RAII sibling `vmhook::scoped_hook<T>`) — the central machine every other
hook feature builds on. Contract: resolve T's klass via `find_class`, walk
`InstanceKlass::_methods` for an exact (name[, signature]) match, set
`_dont_inline` + `NO_COMPILE`, snapshot the four entry points (`_code`,
`_from_interpreted_entry`, `_from_compiled_entry`, `i2i_entry`), build the
`wrapper_detour` that decodes every Java arg via
`detail::java_slot_offsets<tuple>` + `detail::extract_frame_arg<T>` (long/double
widen 2 slots; `self` is slot 0 for instance methods, absent for static), push a
`hooked_method` into `g_hooked_methods`, either reuse or patch+chain the shared
i2i trampoline via `find_hook_location` + `midi2i_hook`, deopt the method if it
was JIT-compiled, and spawn the auto-repair watchdog. Dispatch:
`common_detour` first-match-and-returns on `hook.method == frame.get_method()`,
fires through `seh_invoke_detour` exactly once per call, leaves
`return_slot->cancel==false` so the original body runs (`return_value::cancel()`
/ `set()` flip cancel). If this breaks, every hook-shaped feature
(signature, chaining, after-JIT, verify_repair, cancel/set, arg decode, scoped
RAII) breaks with it — corrupting the install path corrupts dispatch globally.

## Depends on

- [[features/midi2i_trampoline_alloc|midi2i_trampoline_alloc]]
- [[features/hook_common_detour_dispatch|hook_common_detour_dispatch]]
- [[features/method_entry_points_i2i_i2c|method_entry_points_i2i_i2c]]
- [[features/seh_invoke_detour|seh_invoke_detour]]
- [[features/find_class_fallback|find_class_fallback]]
- [[features/klass_introspection|klass_introspection]]
- [[features/decode_oop_and_pointers|decode_oop_and_pointers]]
- [[features/interpreter_frame_walk|interpreter_frame_walk]]
- [[features/signature_parsing|signature_parsing]]
- [[features/vmstructs_offset_resolution|vmstructs_offset_resolution]]
- [[features/adapter_recovery_c2i|adapter_recovery_c2i]]
- [[features/deoptimize_methods|deoptimize_methods]]
- [[features/os_protect|os_protect]]
- [[features/os_safe_read|os_safe_read]]

## Depended on by

- [[features/dont_inline_dont_compile|dont_inline_dont_compile]]
- [[features/hook_chaining|hook_chaining]]
- [[features/hook_install_after_jit|hook_install_after_jit]]
- [[features/hook_reinstall_after_shutdown|hook_reinstall_after_shutdown]]
- [[features/hook_signature|hook_signature]]
- [[features/hook_unhook_double_free|hook_unhook_double_free]]
- [[features/hook_verify_repair|hook_verify_repair]]
- [[features/on_class_loaded|on_class_loaded]]
- [[features/on_exception|on_exception]]
- [[features/shutdown_hooks_teardown|shutdown_hooks_teardown]]

## Implementation anchors

- `vmhook::hook<T>(name, callback)` — `vmhook/ext/vmhook/vmhook.hpp:10003-10008` — thin overload — forwards with empty signature; calls the 4-arg form below
- `vmhook::hook<T>(name, signature, callback, already_hooked)` — `vmhook/ext/vmhook/vmhook.hpp:10018-10337` — real install routine — find_class, walk _methods, set flags, build wrapper_detour, push to g_hooked_methods, patch/chain shared i2i stub, deopt if JIT'd, ensure_started watchdog
- `duplicate-membership short-circuit` — `vmhook/ext/vmhook/vmhook.hpp:10104-10126` — linear scan of g_hooked_methods under install mutex; on hit returns true + sets *already_hooked so scoped_hook<T>() hands back a not-installed handle
- `wrapper_detour (Java arg decode lambda)` — `vmhook/ext/vmhook/vmhook.hpp:10158-10178` — captures user_detour, builds java_slot_offsets<tuple>::value at instantiation time, calls extract_frame_arg<T>(frame, offset) for each arg — long/double get TWO slots, self at slot 0 (instance) or absent (static)
- `i2i install + hook-chaining + midi2i_hook alloc` — `vmhook/ext/vmhook/vmhook.hpp:10196-10266` — find_hook_location -> 0xE9 JMP detect for chain_resume -> new midi2i_hook(target, common_detour, chain_resume); throws on has_error, deletes the instance
- `deopt JIT-compiled targets` — `vmhook/ext/vmhook/vmhook.hpp:10287-10323` — if was_compiled: get_adapter -> c2i_entry; redirect _from_interpreted_entry to i2i, _from_compiled_entry to c2i (or just i2i if adapter unrecoverable), then _code=nullptr last
- `common_detour (dispatch)` — `vmhook/ext/vmhook/vmhook.hpp:7098-7176` — shutdown-flag bail (7105), thread guard, lock-free scan of g_hooked_methods, first match fires through seh_invoke_detour exactly once and returns (7147-7170)
- `seh_invoke_detour` — `vmhook/ext/vmhook/vmhook.hpp:7045-7072` — MSVC __try/__except wraps user detour; MinGW/clang-on-windows fall through to plain catch(...) which CANNOT trap a hardware AV — gates which platforms can survive a wild deref in the user lambda
- `extract_frame_arg<T>` — `vmhook/ext/vmhook/vmhook.hpp:9253-9394` — per-arg slot decode; bounds-guards index in [0, 0xFFFF]; Windows-gated os::safe_read of locals[-index]; decompresses oops (<= 0xFFFFFFFF heuristic); long/double read high slot at index+1; static_assert rejects unsupported types
- `scoped_hook<T>` — `vmhook/ext/vmhook/vmhook.hpp:11191-11310` — RAII wrapper — calls hook<T>() with &already_hooked, re-resolves Method* by walking _methods again, returns hook_handle{m}; honest duplicate-install returns an empty handle so dropping never disarms the prior owner
- `hook_handle::stop` — `vmhook/ext/vmhook/vmhook.hpp:11058-11122` — erases entry under g_hooked_methods_mutex, clears _dont_inline + NO_COMPILE (via safe_access_flags_and, #37 hardening), DELIBERATELY leaves the i2i trampoline + _code/_from_*_entry alone (sweeper may have flushed the saved nmethod)
- `auto-repair watchdog ensure_started call site` — `vmhook/ext/vmhook/vmhook.hpp:10325-10328` — first successful install spawns detail::auto_repair worker_loop (definition at 10727+); no-op when g_auto_repair_enabled is false (suite-side disables it to avoid off-thread cold-page faults)

## Tests

- `tests/jvm/modules/hook_basic.cpp`

## Audit docs

- `audit/findings/hook_basic_install.md`
- `audit/LIBRARY_BUGS.md`
- `audit/NO_SEH_COLDREAD_HARDENING.md`

## Known bugs

- **[high]** Half-installed method permanently poisons re-install. The `g_hooked_methods.push_back` at vmhook.hpp:10194 runs BEFORE the i2i install (10207-10266) and the deopt block (10287-10323). If `find_hook_location` returns nullptr (10209-10213) or `midi2i_hook` reports `has_error()` (10259-10262), the throw is caught at 10332 returning false, but the pushed `hooked_method` entry is never erased. Every subsequent `hook<T>()` for the same Method* hits the duplicate-membership short-circuit at 10104-10126 and returns true with `*already_hooked=true` — `scoped_hook<T>()` then hands back an empty handle (11217-11225) and the original throw is invisible. The library reports installed-or-already-active forever, yet no detour fires. Module assertion target: a deliberately-poisoned install (e.g. by stubbing find_hook_location) then a second `vmhook::hook<T>` MUST return false rather than the current true. Fix: push_back last, or scope-guard the post-push section.  (`audit/findings/hook_basic_install.md`)
- **[high]** `g_hooked_methods.push_back` at vmhook.hpp:10194 can reallocate the vector while a SIBLING method's detour is mid-iteration at 7147 — `common_detour` reads the vector lock-free (see comment block at 6995-7028 and 7101-7104). The install mutex (10102) does not cover the reader; reserving capacity or swapping to a stable-address container (`std::deque`, intrusive list) closes the UAF on the `hook.detour` `std::function` cell. Already documented in the in-source comment at 7015-7028 as a known hazard.
- **[high]** Auto-repair watchdog is dead after `shutdown_hooks()` + re-init. `ensure_started()` is called at 10328 on first successful install. `shutdown_hooks()` sets `g_shutdown_requested` and `common_detour` bails on it at vmhook.hpp:7105 (memory_order_acquire). If `g_shutdown_requested` / `g_started` are not reset by the shutdown path, the first post-re-init `common_detour` returns immediately at 7107 and the watchdog ensure_started no-ops on a still-true `g_started`. Re-init silently produces hooks that never fire. Already partly addressed by `reset_watcher_latches()` at 11055 but the install-side `g_started` latch needs the same treatment. Module assertion: hook -> shutdown -> hook again -> drive Java -> detour fire count > 0.
- **[medium]** Pre-install Method-flag mutations leak on exception. `set_dont_inline(method, true)` at vmhook.hpp:10128 and `*flags |= NO_COMPILE` at 10135 happen BEFORE the throwing i2i-install / midi2i_hook allocation (10207-10266) and BEFORE the deopt block (10287-10323). On a throw the catch at 10332 returns false but the flag mutations are never rolled back — asymmetric with `hook_handle::stop()` (11092-11099) which DOES clear both via `set_dont_inline(false)` + `safe_access_flags_and(~NO_COMPILE)`. A failing install therefore permanently inhibits JIT on the target Method*.
- **[medium]** `_dont_inline` silently no-ops if the Method `_flags` VMStruct is absent. `set_dont_inline()` set path takes the existing fault-safe RMW (commit 16e75d9 hardened the CLEAR path; SET stays atomic), but a JDK whose `_flags` width or VMStruct export is not recognised by the resolver returns nullptr and the call returns silently. `NO_COMPILE` at 10135 still works via `get_access_flags()` (throws on null at 10131-10134), so on a future/patched JVM the inline-guard is HALF-applied with no diagnostic — a JIT'd caller can inline past the i2i patch.
- **[medium]** Argument-decode bounds guard at vmhook.hpp:9264-9268 is the only line standing between a typo'd offset table and a wild read off the stack. The compile-time `java_slot_offsets<tuple>` should never emit a negative or > 0xFFFF index in practice, but the static_assert tail at 9385 only rejects unsupported types, not bad indexes. If a future trait change misroutes a wide-primitive index, the silent return of `base_t{}` will be indistinguishable from a legitimate zero-valued long/double arg — and on detour the user sees a default-constructed arg with no log line. Module assertion: hook_basic.cpp verifies long/int boundary (long widens 2 slots, trailing int read from correct slot).
- **[low]** Doc/scoping risk: the legacy `frame::get_arguments<types...>()` path (audit feature `jni_arg_packing` historically called out at vmhook.hpp:5016 in older revisions; now lives elsewhere after refactor) has no long/double 2-slot widening. A hook author who calls `frame->get_arguments<long,int>()` BY HAND inside a detour — instead of letting the typed `hook<T>` callback decode for them — reads the trailing int from the long's high slot. The supported path (the typed callback driven by `wrapper_detour` at 10158-10178) is correct; the trap is in any manual frame walk.
- **[low]** Static method detection at install is implicit: `find_class` + `_methods` walk at 10063-10094 matches by name and (optionally) signature with no ACC_STATIC filter. The wrapper_detour at 10158-10178 also makes no static-vs-instance distinction — `java_slot_offsets` simply lays out the user lambda's args from slot 0. A user who registers a callback whose first param after `return_value&` is `const std::unique_ptr<W>&` AGAINST a static method will silently decompress an oop out of arg-0 (the first real Java arg) and hand the user a wrapper that points at the wrong receiver. `static_method_only()` exists for the call path (vmhook.hpp ~18119 after the c6ae54f hardening) but the install side has no symmetric guard. Module assertion: hook_basic.cpp's static-method scenario asserts NO `self` and arg-0 decoded from slot 0.

## Notes

JDK-version sensitivities. The i2i injection-point pattern in `find_hook_location`
is matched against HotSpot interpreter-stub layouts; a JDK whose stub fails both
patterns returns nullptr and triggers the half-install flaw above. Tests cover
JDK 8..26 HotSpot. `Method::_adapter` is exported on JDK 8 via VMStructs but
removed on JDK 9+, where `detect_adapter_offset_from_method` (vmhook.hpp ~7751
after the c6ae54f safe_read hardening) heuristically recovers it; the deopt block
at 10287-10323 falls back to the c2i-less path (forced clear of `_code` +
redirect `_from_interpreted_entry` to i2i, accepting one cycle of stale-IC
bypass) when adapter recovery fails. Compressed-OOP decode in extract_frame_arg
at 9320-9333 uses the `bits <= 0xFFFFFFFF` heuristic — only relevant when
`UseCompressedOops` is on (the default under ~32 GB heaps); `self` and any
String arg are the typical decompression targets.

Recent hardening that bears on this feature: c6ae54f hardened 8 residual cold
reads on the detour/revive/shutdown/watchdog paths through `os::safe_read` (incl.
`midi2i_hook::verify_and_repair` stub memcmp, `set_dont_inline` probe,
`detect_adapter_offset_from_method` scan); 037660d fully fault-proofed the
watchdog (reads + RMW writes); 16e75d9 (#37) replaced `set_dont_inline()`'s
CLEAR-path atomic_ref RMW with width-correct safe_read+recompute+safe_write so a
`hook_handle::stop()` against a Method* relocated by a JVMTI RedefineClasses no
longer AVs on the no-SEH legs; b738d6c disables the auto-repair watchdog during
the functional test suite (the watchdog stays the production default ON but is
the suite's only detached thread — its cross-tick poll re-derefs a stale Method*
that no harness `__try` can contain on clang/mingw windows). seh_invoke_detour
at 7045-7072 is `__try/__except` only on real MSVC (gated by
`_MSC_VER && !__clang__`); on MinGW + clang-on-windows it is plain catch(...)
which CANNOT trap a hardware AV — every hook-side cold read on those toolchains
must be safe_read-guarded.

Test-fixture pointers: `vmhook/fixtures/HookBasic.java` exposes go/done + mode
selector; `tests/jvm/modules/hook_basic.cpp` drives one probe cycle per scenario
(done latches, each cycle resets it and sets mode on the rising edge of go).
Seven scenarios cover instance touch(int), uninstall, static staticTouch(int),
instance combine(int,long,int) (long widens 2 slots), static
staticCombine(int,long,int), two distinct instances of touch (receiver demux),
and wideArgs(boolean,double,String,int) — roughly 50 ctx.check() assertions.
Exactly-once-per-call is proven by counting fires against Java-side dispatch
counts and by upper/lower-bounding the fire count.
