---
slug: return_stack_trace_depth
title: Return Stack Trace Depth
category: return
status: seeded
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/medium, category/return]
---

# Return Stack Trace Depth

> **Category:** [[categories/return|return_value (detour-side return manipulation)]]  ·  **Status:** `seeded`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/return_stack_trace_depth-specialist.md`

## Description

TODO: one-paragraph summary of what this feature does and what its input/output contract is.  Replace this with a real description so a spawned specialist can decide if the feature is relevant in ~200 tokens.

## Depends on

- [[features/interpreter_frame_walk|interpreter_frame_walk]]
- [[features/return_caller|return_caller]]

## Related

- [[features/return_caller|return_caller]]
- [[features/return_frame_raw_access|return_frame_raw_access]]
- [[features/return_set_arg|return_set_arg]]
- [[features/return_set_primitives|return_set_primitives]]
- [[features/return_set_wrapper_null|return_set_wrapper_null]]
- [[features/return_value_cancel|return_value_cancel]]

## Tests

- `tests/jvm/modules/return_stack_trace_depth.cpp`

## Notes

Stub manifest — populate hpp_anchors, depends_on, known_bugs as they become known.  See audit/features/schema.md for the field reference.
