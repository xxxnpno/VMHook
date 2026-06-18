---
slug: hook_common_detour_dispatch
title: Hook Common Detour Dispatch
category: hook
status: in_progress
risk: critical
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/in_progress, risk/critical, category/hook, tag/hook, tag/dispatch, tag/common-detour, tag/hot-path, tag/lock-free, tag/first-match, tag/seh-firewall, tag/critical-path, tag/shutdown-bail, tag/thread-guard]
---

# Hook Common Detour Dispatch

> **Category:** [[categories/hook|Hooking machinery (install / dispatch / trampolines)]]  ·  **Status:** `in_progress`  ·  **Risk:** `critical`  ·  **Specialist:** `.claude/agents/hook_common_detour_dispatch-specialist.md`

## Description

The single dispatch core every interpreter-hook trampoline funnels into:
`common_detour(frame*, java_thread*, return_slot*)`. When a patched i2i stub jumps into a
`midi2i_hook` trampoline, the stub builds `(frame*, java_thread*, return_slot*)` and calls this
function. It (1) bails immediately if `g_shutdown_requested` is set (acquire load) so a detour racing
`shutdown_hooks()` never touches the about-to-be-cleared vector; (2) opens a C++ try; (3) validates the
thread pointer; (4) reads the current `Method*` from the interpreter frame via `frame::get_method()`
(cold-page-safe, os::safe_read-gated); (5) installs a `current_thread_guard` RAII that saves/sets the
thread-local current_java_thread; (6) LINEAR-SCANS `g_hooked_methods` lock-free and on the FIRST
`hook.method == current_method` match fires that detour through `seh_invoke_detour` exactly once, forces
`_thread_in_Java`, and RETURNS — the structural "fire exactly once per call" guarantee. A method with no
entry never matches and its original body runs unhooked.

## Depends on

- [[features/seh_invoke_detour|seh_invoke_detour]]
- [[features/interpreter_frame_walk|interpreter_frame_walk]]
- [[features/method_entry_points_i2i_i2c|method_entry_points_i2i_i2c]]

## Related

- [[features/hook_basic|hook_basic]]
- [[features/hook_chaining|hook_chaining]]
- [[features/adapter_recovery_c2i|adapter_recovery_c2i]]
- [[features/hook_install_after_jit|hook_install_after_jit]]

## Depended on by

- [[features/dont_inline_dont_compile|dont_inline_dont_compile]]
- [[features/hook_basic|hook_basic]]
- [[features/hook_chaining|hook_chaining]]

## Implementation anchors

- `hotspot::common_detour(frame*, java_thread*, return_slot*)` — `vmhook/ext/vmhook/vmhook.hpp:7138-7210` — the whole feature — shutdown bail (acquire load), try-open, thread validity check, frame->get_method() read, current_thread_guard RAII, lock-free scan of g_hooked_methods, first-match -> seh_invoke_detour -> set_thread_state(_thread_in_Java) -> return
- `g_hooked_methods + iteration contract` — `vmhook/ext/vmhook/vmhook.hpp:7049-7060` — the registry common_detour scans lock-free; documented contract that it is only mutated from the setup thread BEFORE detours fire (the lock is held by install / stop / shutdown, NOT by the reader)
- `seh_invoke_detour` — `vmhook/ext/vmhook/vmhook.hpp:7085-7112` — SEH firewall around the user detour — __try/__except(EXECUTE_HANDLER) on real MSVC, plain try/catch(...) elsewhere (which CANNOT trap a hardware AV); converts a detour fault into a false return so the JVM doesn't tear down
- `g_shutdown_requested` — `vmhook/ext/vmhook/vmhook.hpp:7138-7148` — first statement reads it acquire and returns; set true at top of shutdown_hooks() and reset false at its end (vmhook.hpp:11113) so the bail is reversible across teardown/re-init
- `midi2i_hook trampoline -> common_detour wiring` — `vmhook/ext/vmhook/vmhook.hpp:10327-10328` — the install path bakes hotspot::common_detour as the trampoline's detour_function_t when allocating a new midi2i_hook for an i2i entry

## Known bugs

- **[high]** Only `thread` is validated (vmhook.hpp ~7152), NOT `frame_pointer`. common_detour reads frame_pointer->get_method() with no is_valid_pointer gate on the frame itself; get_method() is internally os::safe_read-gated so a torn/cold frame yields nullptr (no match, original body runs) rather than an AV, but the absent explicit frame guard is a structural asymmetry.
- **[high]** The lock-free scan of g_hooked_methods relies on the documented contract that the vector is only mutated from the setup thread BEFORE detours fire. A hook<T>() push_back that reallocates the vector while a sibling detour is mid-iteration is a UAF on the hook.detour std::function cell — the install mutex does not cover the reader (shared with hook_basic; documented in-source). A stable-address container would close it.
- **[medium]** seh_invoke_detour is a real SEH firewall ONLY on cl.exe MSVC (_MSC_VER && !__clang__); on MinGW and clang-on-windows it is plain catch(...) which cannot trap a hardware AV inside the user detour — a wild deref in a user lambda is uncontained on those toolchains and tears the JVM down.

## Notes

This feature is the dispatch hot path, not the install path, argument decoding, or return_value
semantics — it is the lock-free function that decides WHICH detour fires for THIS Method* and that it
fires EXACTLY ONCE per call. No dedicated test module exists: the contract is exercised end-to-end
through hook_basic.cpp (exactly-once-per-call counting), hook_chaining.cpp (first-match demux across
several methods sharing one i2i stub), and hook_install_after_jit.cpp (dispatch after deopt). The
first-match -> one-fire -> return structure is the proof of "each detour fires for its method only,
exactly once per call"; the shutdown bail being reversible (reset at shutdown_hooks() end, vmhook.hpp:11113)
is what makes re-install after teardown revivable (see hook_reinstall_after_shutdown). The trampoline ABI
is detour_function_t = void(*)(frame*, java_thread*, return_slot*); the trampoline pushes a zeroed
return_slot on the native stack and common_detour leaves return_slot->cancel == false so the original
body runs unless a detour flips it via return_value::cancel()/set().
