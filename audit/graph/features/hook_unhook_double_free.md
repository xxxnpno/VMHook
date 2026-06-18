---
slug: hook_unhook_double_free
title: Hook Unhook Double Free
category: hook
status: in_progress
risk: high
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/in_progress, risk/high, category/hook, tag/hook, tag/lifecycle, tag/uninstall, tag/double-free, tag/use-after-free, tag/idempotent-stop, tag/structural-restore, tag/move-only, tag/raii]
---

# Hook Unhook Double Free

> **Category:** [[categories/hook|Hooking machinery (install / dispatch / trampolines)]]  ·  **Status:** `in_progress`  ·  **Risk:** `high`  ·  **Specialist:** `.claude/agents/hook_unhook_double_free-specialist.md`

## Description

The single-hook install / uninstall lifecycle on a live JVM — `scoped_hook<T>()` ->
`hook_handle::stop()` -> drop -> re-install — proving the exactly-once-teardown contract with NO
use-after-free, NO double-free, and NO double-restore corruption. The whole double-free story keys off
one field: `hook_handle::method`, nulled the instant `stop()` does its work. `stop()` is idempotent —
a handle whose method is already null (after a prior stop(), a move-from, or default construction) does
nothing; method is nulled BEFORE the locked erase so a re-entrant stop() short-circuits; and if the
g_hooked_methods entry is already gone (a sibling handle on a shared entry erased it) the `find_if`
returns end() and stop() returns without touching anything. The byte-exact-original restore is achieved
STRUCTURALLY: stop() clears `_dont_inline` + NO_COMPILE and erases the entry but DELIBERATELY does not
restore `_code` / `_from_*_entry` (the captured nmethod may have been flushed); erasing the entry makes
common_detour skip the method and the i2i allow-through runs the original body.

## Depends on

- [[features/hook_basic|hook_basic]]
- [[features/shutdown_hooks_teardown|shutdown_hooks_teardown]]

## Related

- [[features/hook_reinstall_after_shutdown|hook_reinstall_after_shutdown]]
- [[features/dont_inline_dont_compile|dont_inline_dont_compile]]
- [[features/hook_chaining|hook_chaining]]
- [[features/hook_install_after_jit|hook_install_after_jit]]
- [[features/hook_signature|hook_signature]]
- [[features/hook_verify_repair|hook_verify_repair]]

## Implementation anchors

- `hook_handle class` — `vmhook/ext/vmhook/vmhook.hpp:9051-9100` — move-only; installed() is literally method != nullptr (9088-9090); ~hook_handle() calls stop() (9080); move-assign stops *this first then steals — both are how scope-local handles disarm at }
- `hook_handle::stop() idempotency gate + method-null-before-work` — `vmhook/ext/vmhook/vmhook.hpp:11128-11136` — if (!this->method) return; (11130) then target = method; this->method = nullptr; (11135) — a re-entrant stop() short-circuits even if the work below threw
- `hook_handle::stop() locked single-entry erase + double-free-safe no-op` — `vmhook/ext/vmhook/vmhook.hpp:11140-11155` — takes g_hooked_methods_mutex, std::find_if for h.method == target; if already gone (sibling on a shared entry erased it) find_if returns end() and stop() returns without touching anything (the 'second stop() finds nothing' path)
- `hook_handle::stop() partial restore (deliberately leaves _code / entries)` — `vmhook/ext/vmhook/vmhook.hpp:11162-11178` — clears _dont_inline (set_dont_inline(false)) + NO_COMPILE (fault-safe RMW), then hooks.erase(entry_it); does NOT restore _code / _from_compiled_entry / _from_interpreted_entry (the captured nmethod may have been sweeper-flushed) — byte-exact restore is structural via erase + i2i allow-through
- `common_detour skips erased method` — `vmhook/ext/vmhook/vmhook.hpp:7138-7210` — after erase the method has no g_hooked_methods entry; common_detour first-match never matches it, the original body runs — the structural byte-exact restore

## Tests

- `tests/jvm/modules/hook_unhook_double_free.cpp`

## Known bugs

- **[medium]** Two distinct hook_handles for the SAME shared i2i entry (e.g. a duplicate-install scenario): when the first stop() erases the g_hooked_methods entry, the second handle's stop() find_if returns end() and no-ops (double-free-safe, vmhook.hpp:11140-11155). Correct, but means the second handle silently believed it owned a live target — the duplicate-membership short-circuit at install hands back an empty handle precisely to avoid this, so a caller bypassing that contract could be surprised.
- **[low]** stop() deliberately leaves _code / _from_compiled_entry / _from_interpreted_entry untouched (vmhook.hpp:11162-11172) — the captured nmethod may have been flushed by the code-cache sweeper between install and stop, so writing it back would hand the JVM a dangling code-cache pointer. Byte-exact restore is therefore NOT a literal entry-point rewrite; it relies on common_detour skipping the erased method + the i2i allow-through. A future JDK whose i2i allow-through path changed would need this re-examined.

## Notes

This is the single-hook remove path. Its siblings: scoped_hook RAII proves the scope-exit auto-removal;
hook_reinstall_after_shutdown / shutdown_hooks_teardown prove the bulk reset. This module zeroes in on
hook_handle::stop() idempotency and its double-remove / double-restore safety. The load-bearing
invariant is byte-exact-original restore: after a remove, real Java bytecode dispatch must observe the
unmodified original method body, not merely 'the detour stopped firing'. The second explicit stop() and
the destructor's third stop() are guaranteed no-ops because method is nulled before the work and the
idempotency gate short-circuits. Tests cover JDK 8..26 HotSpot. Hooks via scoped_hook<T> that disarm on
scope exit; the module leaves NOTHING armed and NEVER calls shutdown_hooks() except where the bulk-reset
contract is explicitly under test. Java-8-only fixture; MSVC copy-init; every Method* / decoded-OOP deref
gated by is_valid_pointer.
