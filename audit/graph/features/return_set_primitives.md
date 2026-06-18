---
slug: return_set_primitives
title: Return Set Primitives
category: return
status: seeded
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/medium, category/return, tag/return, tag/primitives, tag/x86_64]
---

# Return Set Primitives

> **Category:** [[categories/return|return_value (detour-side return manipulation)]]  ·  **Status:** `seeded`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/return_set_primitives-specialist.md`

## Description

return_value::set<T>(value) for primitive return types — overrides a hooked
method's return value by writing the raw bit-pattern into the trampoline's 64-bit
return_slot and setting the cancel flag, so the original Java method body is
suppressed and the interpreter returns the custom value.  T must be trivially
copyable and <= 8 bytes (static_asserted).  The load-bearing subtlety: signed
integers narrower than int64 are sign-extended into the 64-bit slot before the
write (so a hook returning int8_t{-1} lands as -1, not +255, when the interpreter
pops ireturn); everything else (float/double/void*/unsigned/bool) goes through a
zero-then-memcpy of the low N bytes.  Pure slot write — no interpreter frame walk
is required, so it works whether or not the return_value carries a frame.

## Related

- [[features/return_caller|return_caller]]
- [[features/return_frame_raw_access|return_frame_raw_access]]
- [[features/return_set_arg|return_set_arg]]
- [[features/return_set_wrapper_null|return_set_wrapper_null]]
- [[features/return_stack_trace_depth|return_stack_trace_depth]]
- [[features/return_value_cancel|return_value_cancel]]

## Implementation anchors

- `vmhook::return_value::set` — `vmhook/ext/vmhook/vmhook.hpp:1353-1382` — sign-extend signed <8-byte ints; zero+memcpy others; sets cancel
- `vmhook::hotspot::return_slot` — `vmhook/ext/vmhook/vmhook.hpp:1313-1317` — the {cancel, retval} cell the trampoline reads after the callback

## Tests

- `tests/jvm/modules/return_set_primitives.cpp`
- `tests/test_return_value.cpp`

## Notes

set<T>() only touches the return_slot, so test_return_value.cpp exercises the
sign-extension branch and the memcpy path exhaustively with no JVM; the JVM
module confirms the interpreter honours the overridden value end-to-end.
