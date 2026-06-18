---
slug: return_caller
title: Return Caller
category: return
status: seeded
risk: high
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/high, category/return, tag/return, tag/frame, tag/interpreter, tag/x86_64, tag/safety]
---

# Return Caller

> **Category:** [[categories/return|return_value (detour-side return manipulation)]]  ·  **Status:** `seeded`  ·  **Risk:** `high`  ·  **Specialist:** `.claude/agents/return_caller-specialist.md`

## Description

return_value::caller() — from inside a hook detour, returns a caller_info for the
method that invoked the hooked method.  It walks the saved-rbp chain on the
HotSpot x64 interpreter stack: the caller's frame base lives at [rbp] and its
Method* at [caller_rbp - 24], from which it resolves class_name, method_name and
signature.  Every pointer is validated through the safe-read helpers before
dereference, so an unfamiliar (compiled/native/unidentifiable) frame yields an
empty caller_info (method == nullptr, caller_info::valid() == false) rather than
a crash.  When the return_value was constructed with frame == nullptr (no-frame
default), caller() returns the empty result without touching the stack.  This is
the single-frame front-end of the same saved-rbp walk stack_trace() generalises.

## Depends on

- [[features/interpreter_frame_walk|interpreter_frame_walk]]
- [[features/os_safe_read|os_safe_read]]

## Related

- [[features/return_frame_raw_access|return_frame_raw_access]]
- [[features/return_set_arg|return_set_arg]]
- [[features/return_set_primitives|return_set_primitives]]
- [[features/return_set_wrapper_null|return_set_wrapper_null]]
- [[features/return_stack_trace_depth|return_stack_trace_depth]]
- [[features/return_value_cancel|return_value_cancel]]

## Depended on by

- [[features/return_stack_trace_depth|return_stack_trace_depth]]

## Implementation anchors

- `vmhook::return_value::caller` — `vmhook/ext/vmhook/vmhook.hpp:9551-9672` — saved-rbp walk: [rbp], Method* at [caller_rbp - 24]; safe-read gated
- `vmhook::return_value::caller_info` — `vmhook/ext/vmhook/vmhook.hpp:1447-1461` — method/class_name/method_name/signature + valid() == (method != nullptr)

## Tests

- `tests/jvm/modules/return_caller.cpp`
- `tests/test_return_value.cpp`

## Notes

test_return_value.cpp covers the no-frame default (empty caller_info, no crash)
without a JVM; the live saved-rbp walk needs an interpreter frame and is covered
by the JVM module.
