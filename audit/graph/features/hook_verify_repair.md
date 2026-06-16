---
slug: hook_verify_repair
title: Hook Verify Repair
category: hook
status: in_progress
risk: critical
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/in_progress, risk/critical, category/hook, tag/watchdog, tag/drift-detection, tag/auto-repair, tag/safe_read, tag/safe_write, tag/cold-fault, tag/jit-drift, tag/detached-thread, tag/no-throw, tag/safety-net]
---

# Hook Verify Repair

> **Category:** [[categories/hook|Hooking machinery (install / dispatch / trampolines)]]  ·  **Status:** `in_progress`  ·  **Risk:** `critical`  ·  **Specialist:** `.claude/agents/hook_verify_repair-specialist.md`

## Description

Drift detector + auto-repair watchdog for every installed `hook<T>()`. Two entry
points: synchronous `vmhook::verify_hooks()` (callable from any thread, no-throw,
returns count of repairs) and the detached `detail::auto_repair` background thread
spawned on the first successful install. Both walk `g_hooked_i2i_entries` (re-arms
the shared HotSpot i2i JMP via `midi2i_hook::verify_and_repair`) and `g_hooked_methods`
(per-Method drift scan with three modes: 1=Method freed / ConstMethod null, 2=address
aliased to a different Method, 3=JIT drift where HotSpot re-populated `Method::_code`
or cleared NO_COMPILE). Mode-3 repair re-arms `_dont_inline` + NO_COMPILE, redirects
`_from_compiled_entry` to the c2i adapter, restores `_from_interpreted_entry` to the
i2i stub, then clears `_code`. Modes 1/2 call `try_reinstall` which re-resolves the
Method by class+name+signature. Every memory access on the watchdog path is routed
through `safe_read` / `safe_write` / `safe_access_flags_*` — the watchdog has zero
raw VM derefs. Run-time master switch `set_auto_repair_enabled(false)` stops the
live thread and suppresses future spawns reversibly; compile-time opt-out via
`VMHOOK_DISABLE_AUTO_REPAIR`; tick cadence via `VMHOOK_AUTO_REPAIR_INTERVAL_MS`
(default 1000ms).

## Depends on

- [[features/hook_basic|hook_basic]]
- [[features/midi2i_trampoline_alloc|midi2i_trampoline_alloc]]
- [[features/os_safe_read|os_safe_read]]
- [[features/os_protect|os_protect]]
- [[features/vmstructs_offset_resolution|vmstructs_offset_resolution]]
- [[features/seh_invoke_detour|seh_invoke_detour]]
- [[features/dont_inline_dont_compile|dont_inline_dont_compile]]

## Implementation anchors

- `vmhook::verify_hooks()` — `vmhook/ext/vmhook/vmhook.hpp:10390-10700` — drift detector entry point; walks g_hooked_i2i_entries (calls verify_and_repair) + g_hooked_methods (3-mode scan: freed / aliased / JIT-drifted); try_reinstall lambda re-resolves Method* by class+name+sig; returns repair count; noexcept catch-all
- `midi2i_hook::verify_and_repair()` — `vmhook/ext/vmhook/vmhook.hpp:6807-6890` — re-arms the shared HotSpot i2i 5-byte JMP if stomped by another hooker; reads via os::safe_read (cold-page-tolerant), chains to a prior trampoline if one was overlaid, rewrites under os::protect; the safe_read landed in 54473de as the final #28 cure
- `detail::auto_repair::worker_loop / ensure_started / notify_shutdown / wait_for_exit` — `vmhook/ext/vmhook/vmhook.hpp:10727-10849` — detached background thread; cv-waits VMHOOK_AUTO_REPAIR_INTERVAL_MS (default 1000ms), calls verify_hooks() each tick; g_started CAS makes spawn idempotent across TUs; observes g_shutdown_requested in the wait predicate; wait_for_exit caps at 2000 x 1ms
- `vmhook::set_auto_repair_enabled / vmhook::auto_repair_enabled` — `vmhook/ext/vmhook/vmhook.hpp:10851-10932` — run-time master switch landed in b738d6c; disabling a live watchdog raises shutdown, notifies cv, wait_for_exit, then clears the latch so re-enable + a fresh install can spawn again. Must NOT be called under g_hooked_methods_mutex
- `method::safe_access_flags_test / safe_access_flags_or` — `vmhook/ext/vmhook/vmhook.hpp:2731-2820` — fault-safe NO_COMPILE probe + RMW used exclusively by the watchdog; routes the u4 access through os::safe_read/safe_write so a cold/relocated Method page defers the re-arm to the next tick instead of faulting on the detached thread

## Tests

- `tests/jvm/modules/hook_verify_repair.cpp`

## Known bugs

- **[high]** RESOLVED (037660d + 54473de + c6ae54f): the watchdog used to raw-deref a stored Method* / i2i stub on its detached thread between 1000ms polls. A GC code-cache sweep, class-unload, or deopt could relocate/unmap the page mid-tick, faulting UNCONTAINED on the no-SEH legs (clang-cl + mingw on Windows have no working SEH around the worker_loop). Cure landed in three passes: 037660d wrapped every Method read + write on the watchdog path (set_code / set_from_compiled_entry / set_from_interpreted_entry / access_flags) in os::safe_read+os::safe_write, 54473de wrapped the last raw read (midi2i_hook::verify_and_repair stub memcmp) in os::safe_read (vmhook.hpp:6824-6837), c6ae54f hardened 8 residual cold reads/writes uncovered by the no-seh-coldread-audit. The watchdog now has ZERO raw VM derefs.
- **[high]** RESOLVED (b738d6c): even fully fault-proofed, the watchdog REPAIRING (os::protect + i2i stub rewrite) concurrently with a JVM code-cache sweep during a GC corrupts JVM state uncontainably — return_set_arg / field_introspection / wrapper_pattern crashed NO-TOTAL on msvc JDK 11+ cells. Mitigation: added detail::auto_repair::g_auto_repair_enabled (default TRUE = unchanged production) + vmhook::set_auto_repair_enabled(); the JVM functional suite now disables the watchdog as run_all's first action and the GC-heavy modules run watchdog-OFF. Only hook_verify_repair + hook_reinstall_after_shutdown locally re-enable it for in-isolation testing. The watchdog stays production-default-ON; the residual risk is REAL in adversarial hosts that JIT-sweep on the same cadence.
- **[medium]** Mode 3 detection is best-effort under page pressure: if safe_access_flags_test returns flags_readable=false (Method page transiently cold this tick, vmhook.hpp:10634-10639), verify_hooks conservatively SKIPS the re-arm and defers to the next tick. Correct safety choice but means a hook can be silently mid-drift for up to one full interval (default 1000ms) under heavy GC. Not a correctness bug per the documented contract; documented here as a fundamental cadence-vs-cold-page tradeoff.
- **[medium]** set_auto_repair_enabled(false) MUST NOT be called while holding g_hooked_methods_mutex — the live watchdog may be mid-verify_hooks() under that mutex, and wait_for_exit's sleep loop would deadlock (vmhook.hpp:10780-10791, 10892-10894). Documented but not statically enforced; a hostile caller can hang the shutdown path. Same constraint applies to shutdown_hooks() (vmhook.hpp:10961-10969).
- **[low]** wait_for_exit caps at 2000 x 1ms = 2s (vmhook.hpp:10785-10790). If the watchdog is mid-verify_hooks under heavy mutex contention or the OS schedules it out, set_auto_repair_enabled(false) / shutdown_hooks can return with g_watchdog_running still true. Function is noexcept so no error surfaces; subsequent set_auto_repair_enabled(true) + a fresh install will then NOT spawn (the leaked thread is still owning g_started=true latch via worker_loop). Rare; observed only on heavily-loaded CI.
- **[low]** drift_logged debounce (vmhook.hpp:10556, 10575, 10595, 10654, 10691) is per-Method, not per-drift-event. A Method that flips drifted -> repaired -> drifted (e.g. an adversarial agent toggling NO_COMPILE on a tight loop) will be repaired silently after the first log line until the next clean steady-state tick resets the latch — operationally invisible repairs hide the contention.

## Notes

#28 cold-fault arc: the watchdog was the lone OFF-suite-thread fault SOURCE that
reddened windows-msvc + windows-clang + windows-mingw cells from late May through
mid-June 2026. Four hardening passes (a802 reads-only, 037660d reads+writes,
54473de stub memcmp, c6ae54f residual audit) made it fault-proof. The CURE that
actually greened the suite + un-quarantined clang+mingw windows was b738d6c —
disabling the watchdog during the GC-heavy functional suite — landing alongside
the forced-System.gc() POSIX-only gate (bc76415) and the tiny in-detour allocs
(79e58be). See cold_fault_28_root_cause memory + commit 0420c40 for the
un-quarantine.

Follow-up risk #38 (no_seh_heap_safepoint_38 memory): the deep no-SEH GC-safepoint
STALL on windows-msvc/clang-cl (java24 fail, clang java11/26 borderline) is a
DIFFERENT hazard — raw VM-walks under suite heap pressure, not the watchdog —
but gates further expansion of this module's drift-mode coverage because more
scenarios = more heap pressure = redder no-SEH cells. Loop-caps don't fix it;
the real fix is _thread_in_native around walks.

The fixture (vmhook/fixtures/HookVerifyRepair) latches `done` per scenario;
install/verify/teardown all run on the native driver thread between probe cycles.
Mode-3 is the only deterministic in-process drift mode — modes 1/2 require a
hostile JVMTI RedefineClasses and are characterised via the no-throw / intact
contract only. Hot-loop warming (well past the JIT threshold) is the deterministic
mode-3 trigger; if a particular JDK doesn't warm fast enough the scenario logs
[INFO] and proves the contract another way — never a spurious [FAIL].
