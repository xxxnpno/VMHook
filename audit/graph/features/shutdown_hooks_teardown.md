---
slug: shutdown_hooks_teardown
title: Shutdown Hooks Teardown
category: lifecycle
status: seeded
risk: high
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/high, category/lifecycle, tag/shutdown, tag/teardown, tag/reversibility, tag/lifecycle, tag/hook, tag/watchdog, tag/restore, tag/safety]
---

# Shutdown Hooks Teardown

> **Category:** [[categories/lifecycle|Lifecycle hooks (shutdown / class-load / exception / enum)]]  ·  **Status:** `seeded`  ·  **Risk:** `high`  ·  **Specialist:** `.claude/agents/shutdown_hooks_teardown-specialist.md`

## Description

vmhook::shutdown_hooks() — the BULK teardown that removes EVERY installed hook
and restores the JVM to a clean state. Unlike hook_basic's scoped_hook
(auto-removed on scope exit), this feature exercises the low-level
vmhook::hook<T>() path so the ONLY thing that takes a hook back down is
shutdown_hooks() itself, making it the right place to prove the teardown
contract end-to-end. The function flips g_shutdown_requested BEFORE taking the
install mutex (so any in-flight common_detour early-returns and the watchdog
observes shutdown), waits for the auto-repair watchdog to exit, then under the
teardown lock restores each method (clear _dont_inline / NO_COMPILE flags,
restore entry points) and clears g_hooked_methods / g_hooked_i2i_entries. The
HEADLINE property is REVERSIBILITY: shutdown_hooks() must NOT be one-shot — it
clears g_shutdown_requested (and the watchdog g_started latch) at the END so a
FRESH hook<T>() installed afterwards FIRES. O(H) in the number of active hooks.

## Depends on

- [[features/hook_basic|hook_basic]]

## Related

- [[features/on_class_loaded|on_class_loaded]]
- [[features/on_exception|on_exception]]
- [[features/seh_invoke_detour|seh_invoke_detour]]

## Depended on by

- [[features/hook_reinstall_after_shutdown|hook_reinstall_after_shutdown]]
- [[features/hook_unhook_double_free|hook_unhook_double_free]]

## Implementation anchors

- `vmhook::shutdown_hooks()` — `vmhook/ext/vmhook/vmhook.hpp:11015-11126` — the bulk teardown. Flips g_shutdown_requested true before the install mutex (11052) so in-flight common_detour bails; wakes + waits for the auto-repair watchdog to exit (reversible without a duplicate watchdog); under g_hooked_methods_mutex restores each method (set_dont_inline(method,false) + fault-safe RMW clear of NO_COMPILE + entry-point restore) then clears both vectors (11100-11101)
- `REVERSIBILITY flag-reset (the [high] audit fix)` — `vmhook/ext/vmhook/vmhook.hpp:11103-11113` — g_started.store(false) (11112) and g_shutdown_requested.store(false) (11113) at the END make shutdown_hooks() reversible. Previously g_shutdown_requested latched true forever — after one teardown a fresh hook<T>() returned true but its detour was silently dead (common_detour's early-out on the flag; watchdog refused to respawn). This reset is the litmus fix
- `g_shutdown_requested + common_detour early-out` — `vmhook/ext/vmhook/vmhook.hpp:7067-7145` — the atomic flag declared at 7067; common_detour loads it with acquire at 7145 and returns immediately when set — so a latched flag silently kills every detour. The race-free design flips it BEFORE the mutex so the dispatch loop never iterates g_hooked_methods mid-clear

## Tests

- `tests/jvm/modules/shutdown_hooks_teardown.cpp`

## Known bugs

- **[high]** REGRESSION TARGET (fixed, pinned): permanently latched shutdown flag breaks re-install after teardown. g_shutdown_requested formerly latched true forever; after one shutdown_hooks() a fresh hook<T>() returned true but its detour was silently dead because common_detour early-returns on the flag (vmhook.hpp:7145) and the auto-repair watchdog refused to respawn. Fixed by clearing g_shutdown_requested / g_started at the end of shutdown_hooks() (11112-11113). The canonical proof is the three-beat dance: install -> FIRES; shutdown_hooks() -> ORIGINAL body runs byte-exact, detour SILENT; FRESH hook AFTER shutdown_hooks() -> MUST FIRE. The third beat is the litmus test for the latched-flag regression.

## Notes

REVERSIBILITY is the headline contract and the one [high] bug (now fixed) the
module guards as a regression. Other angles the specialist covers: shutdown_hooks()
with NO hooks installed is safe (library still usable); double shutdown_hooks()
(back-to-back and on an already-clean state) is safe and still reversible; ONE
shutdown_hooks() removes hooks on MULTIPLE distinct methods (instance + static +
multi-slot — three different Method* shapes); the method body is BYTE-EXACT-ORIGINAL
after teardown, proven the strong way (a force-RETURN hook makes Java observe a
sentinel, then teardown makes Java observe the unmodified original result again —
genuinely restored, not merely 'the detour stopped firing'); allow-through both
before and after.

Test coverage: tests/jvm/modules/shutdown_hooks_teardown.cpp against
vmhook/fixtures/ShutdownHooksTeardown. Lifecycle discipline (non-negotiable):
installs are low-level (persist until shutdown_hooks()), so EVERY scenario that
installs hooks ends by calling shutdown_hooks() and the module's FINAL statement
is a belt-and-braces shutdown_hooks() so no hook is left armed when control
returns to the driver — this is the one feature whose contract legitimately calls
shutdown_hooks(). Every decoded-OOP deref gated by is_valid_pointer; MSVC
copy-init (never brace-init from value_t/->call()); Java-8-only fixture.
