---
slug: return_frame_raw_access
title: Return Frame Raw Access
category: return
status: seeded
risk: high
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/high, category/return, tag/return, tag/frame, tag/interpreter, tag/x86_64, tag/advanced]
---

# Return Frame Raw Access

> **Category:** [[categories/return|return_value (detour-side return manipulation)]]  ·  **Status:** `seeded`  ·  **Risk:** `high`  ·  **Specialist:** `.claude/agents/return_frame_raw_access-specialist.md`

## Description

return_value::frame() — exposes the intercepted HotSpot interpreter frame
(vmhook::hotspot::frame*) to the detour, or nullptr when the return_value was
constructed without one.  It is the raw escape hatch for advanced callers that
want to walk the call stack themselves (read locals, the Method*, the saved rbp)
rather than going through the higher-level caller() / stack_trace() / set_arg()
helpers.  The accessor itself is a trivial getter of the private stack_frame
pointer; the safety burden moves to the caller, who must validate every
dereference (the library's own helpers route through is_valid_pointer /
safe_read).  Callers should prefer caller() when only the immediate caller is
needed; frame() exists for the cases those convenience APIs do not cover.

## Depends on

- [[features/interpreter_frame_walk|interpreter_frame_walk]]

## Related

- [[features/return_caller|return_caller]]
- [[features/return_set_arg|return_set_arg]]
- [[features/return_set_primitives|return_set_primitives]]
- [[features/return_set_wrapper_null|return_set_wrapper_null]]
- [[features/return_stack_trace_depth|return_stack_trace_depth]]
- [[features/return_value_cancel|return_value_cancel]]

## Implementation anchors

- `vmhook::return_value::frame` — `vmhook/ext/vmhook/vmhook.hpp:1525-1535` — trivial getter of the private stack_frame pointer; nullptr if none
- `vmhook::return_value` — `vmhook/ext/vmhook/vmhook.hpp:1344-1539` — the hook-callback handle; stack_frame member set at construction

## Tests

- `tests/jvm/modules/return_frame_raw_access.cpp`
- `tests/test_return_value.cpp`

## Notes

test_return_value.cpp asserts frame() returns nullptr (and caller()/stack_trace()
give their empty defaults) when constructed frame == nullptr, no JVM needed;
walking a real frame is JVM-only.
