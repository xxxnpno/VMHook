---
category: return
title: return_value (detour-side return manipulation)
feature_count: 8
tags: [category/return]
---

# return_value (detour-side return manipulation)

**8 feature(s) in this category.**

## Features

- [[features/interpreter_frame_walk|interpreter_frame_walk]] — `seeded` / `medium` — Interpreter Frame Walk
- [[features/return_caller|return_caller]] — `seeded` / `medium` — Return Caller
- [[features/return_frame_raw_access|return_frame_raw_access]] — `seeded` / `medium` — Return Frame Raw Access
- [[features/return_set_arg|return_set_arg]] — `seeded` / `medium` — Return Set Arg
- [[features/return_set_primitives|return_set_primitives]] — `seeded` / `medium` — Return Set Primitives
- [[features/return_set_wrapper_null|return_set_wrapper_null]] — `seeded` / `medium` — Return Set Wrapper Null
- [[features/return_stack_trace_depth|return_stack_trace_depth]] — `seeded` / `medium` — Return Stack Trace Depth
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
    wrapper_pattern[/wrapper_pattern/]
  end
  return_caller --> interpreter_frame_walk
  return_frame_raw_access --> interpreter_frame_walk
  return_set_arg --> interpreter_frame_walk
  return_set_primitives --> interpreter_frame_walk
  return_set_wrapper_null --> interpreter_frame_walk
  return_set_wrapper_null --> wrapper_pattern
  return_stack_trace_depth --> interpreter_frame_walk
  return_stack_trace_depth --> return_caller
  return_value_cancel --> interpreter_frame_walk
```
