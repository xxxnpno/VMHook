---
slug: hook_install_after_jit
title: Hook Install After Jit
category: hook
status: in_progress
risk: high
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/in_progress, risk/high, category/hook, tag/hook, tag/deopt-on-install, tag/jit, tag/method-code, tag/entry-point-redirect, tag/c2i-adapter, tag/interpreter-routing, tag/after-jit]
---

# Hook Install After Jit

> **Category:** [[categories/hook|Hooking machinery (install / dispatch / trampolines)]]  ·  **Status:** `in_progress`  ·  **Risk:** `high`  ·  **Specialist:** `.claude/agents/hook_install_after_jit-specialist.md`

## Description

Installing a `vmhook::hook<T>` on a method that is ALREADY JIT-compiled. The distinction from the
interpreted-install path is the ORDER of events: here the method is warmed to a published
`Method::_code != null` FIRST and the hook is installed SECOND. For the patched i2i stub to take effect
on a compiled method, the install path must DEOPTIMISE it: when `was_compiled` is detected it redirects
`_from_interpreted_entry` to the i2i stub, redirects `_from_compiled_entry` to the c2i adapter (so
compiled callers route through the interpreter), and clears `Method::_code` LAST (entry-point writes
visible first). If the c2i adapter is unrecoverable it falls back to a forced clear of `_code`. The next
bytecode dispatch then routes through the interpreter and fires the detour exactly once. The documented
deopt-sweep helpers (`deoptimize_all_jit_compiled_methods` / `deoptimize_methods_if`) also null `_code`.

## Depends on

- [[features/hook_basic|hook_basic]]
- [[features/deoptimize_methods|deoptimize_methods]]
- [[features/method_entry_points_i2i_i2c|method_entry_points_i2i_i2c]]
- [[features/adapter_recovery_c2i|adapter_recovery_c2i]]

## Related

- [[features/adapter_recovery_c2i|adapter_recovery_c2i]]
- [[features/dont_inline_dont_compile|dont_inline_dont_compile]]
- [[features/hook_basic|hook_basic]]
- [[features/hook_chaining|hook_chaining]]
- [[features/hook_common_detour_dispatch|hook_common_detour_dispatch]]
- [[features/hook_reinstall_after_shutdown|hook_reinstall_after_shutdown]]

## Implementation anchors

- `hook<T> install — was_compiled detection` — `vmhook/ext/vmhook/vmhook.hpp:10217-10220` — reads original Method::_code and gates is_valid_pointer — true means the method is JIT-compiled at install time (the precondition that makes this the 'after JIT' path)
- `hook<T> install — deopt-on-install entry redirection` — `vmhook/ext/vmhook/vmhook.hpp:10350-10395` — if was_compiled: get c2i_entry from adapter, set_from_interpreted_entry(i2i), set_from_compiled_entry(c2i), set_code(nullptr) LAST; falls back to forced _code clear when the c2i adapter is unrecoverable (one cycle of stale-IC bypass)
- `common_detour dispatch after deopt` — `vmhook/ext/vmhook/vmhook.hpp:7138-7210` — the deopt routes the next call through the interpreter / i2i stub, where common_detour first-matches the Method* and fires the detour exactly once
- `deoptimize_methods_if(predicate)` — `vmhook/ext/vmhook/vmhook.hpp:8211-8345` — walks every loaded klass + method, deopts those matching the predicate (nulls _code, redirects entries); logs deoptimised/skipped counts
- `deoptimize_all_jit_compiled_methods()` — `vmhook/ext/vmhook/vmhook.hpp:8360-8370` — convenience always-true predicate over deoptimize_methods_if — deopts every JIT'd method

## Tests

- `tests/jvm/modules/hook_install_after_jit.cpp`

## Audit docs

- `audit/findings/hook_install_after_jit.md`

## Known bugs

- **[high]** When the c2i adapter is unrecoverable (Method::_adapter removed on JDK 9+ and detect_adapter_offset_from_method heuristic fails), the deopt block forces _code = nullptr and redirects _from_interpreted_entry to i2i WITHOUT a valid _from_compiled_entry — accepting ONE cycle of stale-IC bypass where an inline cache pointing at the now-flushed nmethod may dispatch the compiled body once before re-resolution. Documented tradeoff, not a crash, but the detour can miss the very first post-install call on those JDKs.
- **[medium]** Forcing JIT is timing / JDK / flag dependent: if after a generous warm budget _code never becomes non-null (tiered comp off, small method, -Xint), the 'after JIT' precondition is not met and the scenario is NOT actually testing the deopt path. The module records [INFO] and falls back to interpreted install+fire+teardown rather than emitting a spurious [FAIL] — but a CI cell where JIT never warms gives no real after-JIT coverage.

## Notes

Distinction from hook_verify_repair: there the method is hooked FIRST (NO_COMPILE keeps it interpreted)
and only then warmed; HERE the method is warmed to _code != null FIRST and hooked SECOND, forcing the
install-time deopt. Method::_adapter is exported on JDK 8 via VMStructs but removed on JDK 9+, where the
c2i recovery is heuristic — the forced-clear fallback covers that case. Module proofs (on real bytecode
dispatch): the method really has _code != null at the moment of install; hook<T>() returns true and as a
side effect _code is NULLED and NO_COMPILE is armed; the very next dispatch fires the detour exactly once
with the correct receiver self + decoded arg; a non-cancelling detour allows through (Java observes the
unmodified result), a cancelling detour forces the return; verify_hooks() reports 0 drift immediately
after; deoptimize_all_jit_compiled_methods / deoptimize_methods_if both deopt the warm method while a
non-matching predicate leaves an unrelated warm method intact; after shutdown_hooks() the method runs
normally and the detour does not fire. Hooks via low-level vmhook::hook<T>() (the scenario is about the
install path); every scenario that installs ends by removing them so nothing leaks; the module leaves
NOTHING armed. Every decoded-OOP / Method* deref gated by is_valid_pointer; MSVC copy-init; Java-8-only fixture.
