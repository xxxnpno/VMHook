---
category: return
title: return_value (detour-side return manipulation)
feature_count: 8
tags: [category/return]
---

# return_value (detour-side return manipulation)

**8 feature(s) in this category.**

## Features

- [[features/interpreter_frame_walk|interpreter_frame_walk]] — `seeded` / `high` — Interpreter Frame Walk
- [[features/return_caller|return_caller]] — `seeded` / `high` — Return Caller
- [[features/return_frame_raw_access|return_frame_raw_access]] — `seeded` / `high` — Return Frame Raw Access
- [[features/return_set_arg|return_set_arg]] — `seeded` / `high` — Return Set Arg
- [[features/return_set_primitives|return_set_primitives]] — `seeded` / `medium` — Return Set Primitives
- [[features/return_set_wrapper_null|return_set_wrapper_null]] — `seeded` / `medium` — Return Set Wrapper Null
- [[features/return_stack_trace_depth|return_stack_trace_depth]] — `seeded` / `high` — Return Stack Trace Depth
- [[features/return_value_cancel|return_value_cancel]] — `seeded` / `medium` — Return Value Cancel

## Dependency graph

```mermaid
flowchart LR
  subgraph return["return_value (detour-side return manipulation)"]
    interpreter_frame_walk([interpreter_frame_walk])
    return_caller([return_caller])
    return_frame_raw_access([return_frame_raw_access])
    return_set_arg([return_set_arg])
    return_set_primitives([return_set_primitives])
    return_set_wrapper_null([return_set_wrapper_null])
    return_stack_trace_depth([return_stack_trace_depth])
    return_value_cancel([return_value_cancel])
  end
  subgraph external["(external deps)"]
    decode_oop_and_pointers[/decode_oop_and_pointers/]
    os_safe_read[/os_safe_read/]
    vmstructs_offset_resolution[/vmstructs_offset_resolution/]
    wrapper_pattern[/wrapper_pattern/]
  end
  interpreter_frame_walk --> vmstructs_offset_resolution
  interpreter_frame_walk --> decode_oop_and_pointers
  interpreter_frame_walk --> os_safe_read
  return_caller --> interpreter_frame_walk
  return_caller --> os_safe_read
  return_frame_raw_access --> interpreter_frame_walk
  return_set_arg --> interpreter_frame_walk
  return_set_arg --> os_safe_read
  return_set_wrapper_null --> wrapper_pattern
  return_stack_trace_depth --> interpreter_frame_walk
  return_stack_trace_depth --> return_caller
  return_stack_trace_depth --> os_safe_read
```
