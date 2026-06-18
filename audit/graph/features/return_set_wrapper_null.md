---
slug: return_set_wrapper_null
title: Return Set Wrapper Null
category: return
status: seeded
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/medium, category/return, tag/return, tag/oop, tag/wrapper, tag/x86_64]
---

# Return Set Wrapper Null

> **Category:** [[categories/return|return_value (detour-side return manipulation)]]  ·  **Status:** `seeded`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/return_set_wrapper_null-specialist.md`

## Description

return_value::set<wrapper_type>(nullptr) — the typed null-return overload for
hooks on methods that return a Java reference type.  It is equivalent to
set<oop_t>(nullptr) (sets the cancel flag and zeroes the 64-bit retval oop slot)
but documents the slot's Java type at the call site, e.g.
`ret.set<sdk::moving_object_position>(nullptr)` reads as "return null
MovingObjectPosition".  The wrapper_type template argument is documentation only
— no instance is ever touched, just a null oop written into the slot — and the
overload is constrained (requires std::is_base_of_v<object_base, wrapper_type>)
so primitive set<int32_t>(...) calls stay on the integer path.  Pure slot write;
no interpreter frame walk required.

## Depends on

- [[features/wrapper_pattern|wrapper_pattern]]

## Related

- [[features/return_caller|return_caller]]
- [[features/return_frame_raw_access|return_frame_raw_access]]
- [[features/return_set_arg|return_set_arg]]
- [[features/return_set_primitives|return_set_primitives]]
- [[features/return_stack_trace_depth|return_stack_trace_depth]]
- [[features/return_value_cancel|return_value_cancel]]

## Implementation anchors

- `vmhook::return_value::set (nullptr overload)` — `vmhook/ext/vmhook/vmhook.hpp:1402-1409` — requires is_base_of<object_base, wrapper_type>; zeroes oop slot, sets cancel
- `vmhook::hotspot::return_slot` — `vmhook/ext/vmhook/vmhook.hpp:1313-1317` — the {cancel, retval} cell the trampoline reads after the callback

## Tests

- `tests/jvm/modules/return_set_wrapper_null.cpp`
- `tests/test_return_value.cpp`

## Notes

Documentation-only template arg; the overload just zeroes the oop slot, so
test_return_value.cpp covers it with no JVM.  JVM module confirms the hooked
reference-returning method actually returns null.
