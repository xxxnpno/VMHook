---
slug: return_set_arg
title: Return Set Arg
category: return
status: seeded
risk: high
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/high, category/return, tag/return, tag/frame, tag/interpreter, tag/x86_64, tag/safety]
---

# Return Set Arg

> **Category:** [[categories/return|return_value (detour-side return manipulation)]]  ·  **Status:** `seeded`  ·  **Risk:** `high`  ·  **Specialist:** `.claude/agents/return_set_arg-specialist.md`

## Description

return_value::set_arg(index, value) — mutates a Java method argument in place on
the interpreter stack from inside a hook detour.  It writes `value` directly into
the local-variable slot at `index` within the intercepted frame (index 0 is
`this` for instance methods, the first argument for static methods).  Returns
true on a successful write, false on the guard/early-return paths: a missing
stack_frame (return_value built with frame == nullptr), a negative index, an
index above the JVM u2 max_locals bound (0xFFFF), or a null get_locals().  The
value must be trivially copyable and fit a Java local slot (<= 8 bytes); the slot
write itself is fault-safe.  Mutating a JIT-compiled method's frame can trigger a
DEOPT, which the surrounding machinery treats as a recoverable false.

## Depends on

- [[features/interpreter_frame_walk|interpreter_frame_walk]]
- [[features/os_safe_read|os_safe_read]]

## Related

- [[features/return_caller|return_caller]]
- [[features/return_frame_raw_access|return_frame_raw_access]]
- [[features/return_set_primitives|return_set_primitives]]
- [[features/return_set_wrapper_null|return_set_wrapper_null]]
- [[features/return_stack_trace_depth|return_stack_trace_depth]]
- [[features/return_value_cancel|return_value_cancel]]

## Implementation anchors

- `vmhook::return_value::set_arg` — `vmhook/ext/vmhook/vmhook.hpp:9817-10009` — local-slot write; missing-frame/negative/>0xFFFF guards; fault-safe store
- `vmhook::return_value::set_arg (declaration + doc)` — `vmhook/ext/vmhook/vmhook.hpp:1417-1437` — contract: index semantics, <= 8-byte trivially-copyable value, return bool

## Tests

- `tests/jvm/modules/return_set_arg.cpp`
- `tests/test_return_value.cpp`

## Notes

test_return_value.cpp covers the guard/early-return paths (missing frame,
negative index, index above 0xFFFF) without a JVM; the actual interpreter-local
mutation when a frame is present is covered by the JVM module.
