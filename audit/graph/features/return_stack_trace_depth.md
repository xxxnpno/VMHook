---
slug: return_stack_trace_depth
title: Return Stack Trace Depth
category: return
status: seeded
risk: high
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/high, category/return, tag/return, tag/frame, tag/interpreter, tag/x86_64, tag/safety]
---

# Return Stack Trace Depth

> **Category:** [[categories/return|return_value (detour-side return manipulation)]]  ·  **Status:** `seeded`  ·  **Risk:** `high`  ·  **Specialist:** `.claude/agents/return_stack_trace_depth-specialist.md`

## Description

return_value::stack_trace(max_depth = 64) — captures the full interpreter call
stack as a vector<caller_info>, starting with the hooked method's immediate
caller (index 0, identical to caller()) and walking outward up the saved-rbp
chain.  The walk keeps going while every saved-rbp and Method* read passes the
safe-pointer checks, and terminates when max_depth frames are captured, the next
saved-rbp slot fails validation (typically a compiled/native frame that breaks
the interpreter layout), or the next Method* fails validation.  An empty result
means the immediate caller already failed validation (compiled/native/no frame).
max_depth is a hard cap on returned frames (pass 0 for the default 64).  Same
saved-rbp machinery as caller(), generalised to depth; every dereference is gated
by is_valid_pointer so it can only ever fail-safe.

## Depends on

- [[features/interpreter_frame_walk|interpreter_frame_walk]]
- [[features/return_caller|return_caller]]
- [[features/os_safe_read|os_safe_read]]

## Related

- [[features/return_caller|return_caller]]
- [[features/return_frame_raw_access|return_frame_raw_access]]
- [[features/return_set_arg|return_set_arg]]
- [[features/return_set_primitives|return_set_primitives]]
- [[features/return_set_wrapper_null|return_set_wrapper_null]]
- [[features/return_value_cancel|return_value_cancel]]

## Implementation anchors

- `vmhook::return_value::stack_trace` — `vmhook/ext/vmhook/vmhook.hpp:9674-9814` — depth-bounded saved-rbp walk; index 0 == caller(); max_depth cap
- `vmhook::return_value::stack_trace (declaration + doc)` — `vmhook/ext/vmhook/vmhook.hpp:1479-1523` — termination contract; default max_depth 64; pass 0 for default

## Tests

- `tests/jvm/modules/return_stack_trace_depth.cpp`
- `tests/test_return_value.cpp`

## Notes

test_return_value.cpp covers the no-frame default (empty vector, no crash);
multi-frame depth walking needs live interpreter frames and is JVM-only.
