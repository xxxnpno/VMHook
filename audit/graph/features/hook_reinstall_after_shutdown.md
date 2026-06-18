---
slug: hook_reinstall_after_shutdown
title: Hook Reinstall After Shutdown
category: hook
status: in_progress
risk: high
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/in_progress, risk/high, category/hook, tag/hook, tag/lifecycle, tag/revive, tag/reversible-shutdown, tag/latch-reset, tag/watchdog-respawn, tag/shutdown-bail, tag/idempotent]
---

# Hook Reinstall After Shutdown

> **Category:** [[categories/hook|Hooking machinery (install / dispatch / trampolines)]]  ·  **Status:** `in_progress`  ·  **Risk:** `high`  ·  **Specialist:** `.claude/agents/hook_reinstall_after_shutdown-specialist.md`

## Description

The property that `vmhook::shutdown_hooks()` is REVERSIBLE. After a hook is installed, fired, torn down
by `shutdown_hooks()`, and installed AGAIN, the detour must come back to LIFE and fire on real bytecode
dispatch — on both instance and static methods, through both teardown surfaces (the bulk
`shutdown_hooks()` AND the per-handle `scoped_hook` RAII drop), durably across many cycles. This was a
real flaw: teardown latched three pieces of global state true forever, so every post-teardown install
reported success but its detour was silently dead. The revive hinges on `shutdown_hooks()` clearing
`g_started`, `g_shutdown_requested`, and calling `reset_watcher_latches()` at its END (after the
bounded `wait_for_exit()` for the old watchdog), so the next install's `ensure_started()` can spawn a
fresh watchdog and `common_detour`'s shutdown bail no longer returns early.

## Depends on

- [[features/shutdown_hooks_teardown|shutdown_hooks_teardown]]
- [[features/hook_basic|hook_basic]]

## Related

- [[features/hook_unhook_double_free|hook_unhook_double_free]]
- [[features/dont_inline_dont_compile|dont_inline_dont_compile]]
- [[features/hook_chaining|hook_chaining]]
- [[features/hook_install_after_jit|hook_install_after_jit]]
- [[features/hook_signature|hook_signature]]
- [[features/hook_verify_repair|hook_verify_repair]]

## Implementation anchors

- `shutdown_hooks()` — `vmhook/ext/vmhook/vmhook.hpp:11015-11127` — bulk teardown + revive-enabling reset — sets g_shutdown_requested (11022), notify_shutdown + wait_for_exit (bounded, before clearing g_started), takes g_hooked_methods_mutex, deletes trampolines + clears NO_COMPILE/_dont_inline, clears the vectors
- `shutdown_hooks() revive reset (the latch clear)` — `vmhook/ext/vmhook/vmhook.hpp:11112-11125` — the three lines that revive everything — g_started = false (11112), g_shutdown_requested = false (11113), reset_watcher_latches() (11125)
- `detail::reset_watcher_latches()` — `vmhook/ext/vmhook/vmhook.hpp:21263-21275` — clears class_load_hook_installed / exception_hook_installed and drops the stale callback lists so a post-shutdown on_class_loaded() / on_exception() re-installs a FIRING detour instead of a dead watch_handle
- `common_detour shutdown bail` — `vmhook/ext/vmhook/vmhook.hpp:7138-7148` — first statement reads g_shutdown_requested (acquire) and returns — the gate the latched-flag bug got stuck behind; with the flag permanently true EVERY detour returned here before scanning g_hooked_methods
- `auto_repair::ensure_started()` — `vmhook/ext/vmhook/vmhook.hpp:10863-10900` — g_started CAS (10875) spawns the JIT-drift watchdog exactly once; bails if the CAS loses OR g_shutdown_requested is already true (10879); hook<T>() calls it at the end of a successful install
- `hook_handle::stop() (RAII teardown surface)` — `vmhook/ext/vmhook/vmhook.hpp:11128-11175` — the scoped/RAII teardown — erases this Method*'s g_hooked_methods entry, touches NEITHER g_shutdown_requested NOR g_started NOR the watchdog, so RAII drop leaves the library revivable WITHOUT a shutdown_hooks()

## Tests

- `tests/jvm/modules/hook_reinstall_after_shutdown.cpp`

## Known bugs

- **[high]** RESOLVED (historical, pinned as regression): teardown latched g_started / g_shutdown_requested / the class-load+exception watch latches TRUE forever, so every post-shutdown install reported success while common_detour's shutdown bail (vmhook.hpp:7138-7148) returned early before scanning g_hooked_methods — the detour was silently dead. Fixed by clearing all three at the END of shutdown_hooks() (g_started 11112, g_shutdown_requested 11113, reset_watcher_latches() 11125). The module hard-asserts hook -> shutdown -> hook again -> drive Java -> fire count > 0 so a re-latch trips it.
- **[medium]** wait_for_exit() before clearing g_started is bounded (<=2000 x 1ms). If the old watchdog is mid-verify_hooks() under heavy contention and does not exit within the cap, g_started is cleared anyway and a subsequent install can spawn a SECOND watchdog (or the next install's ensure_started CAS races the leaked thread). Rare; observed only on heavily-loaded CI (shared with hook_verify_repair).

## Notes

Order of operations in shutdown_hooks() that makes revive possible: (1) set g_shutdown_requested = true
so in-flight detours bail; (2) auto_repair::notify_shutdown() wakes the watchdog; (3) wait_for_exit()
bounded spin until the old watchdog actually exits BEFORE clearing g_started (so the next install can't
leak a second watchdog); (4) take g_hooked_methods_mutex, delete trampolines, clear NO_COMPILE /
_dont_inline on each still-valid Method, clear the vectors; (5) the revive reset (g_started false,
g_shutdown_requested false, reset_watcher_latches). hook_handle::stop() (the RAII surface) is the OTHER
teardown path and deliberately touches none of the three globals, so a scope-exit drop leaves the
library revivable without a full shutdown_hooks(). Module proves revive on BOTH instance and static
methods, through BOTH teardown surfaces, durably across many consecutive cycles. This module locally
re-enables the auto-repair watchdog for in-isolation testing (the suite disables it by default). Tests
cover JDK 8..26 HotSpot. Java-8-only fixture; MSVC copy-init; every Method* / decoded-OOP deref gated by
is_valid_pointer; the module leaves NOTHING armed.
