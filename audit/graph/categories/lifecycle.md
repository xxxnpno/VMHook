---
category: lifecycle
title: Lifecycle hooks (shutdown / class-load / exception / enum)
feature_count: 4
tags: [category/lifecycle]
---

# Lifecycle hooks (shutdown / class-load / exception / enum)

**4 feature(s) in this category.**

## Features

- [[features/enum_singleton|enum_singleton]] — `in_progress` / `medium` — Enum Singleton
- [[features/on_class_loaded|on_class_loaded]] — `seeded` / `medium` — On Class Loaded
- [[features/on_exception|on_exception]] — `seeded` / `medium` — On Exception
- [[features/shutdown_hooks_teardown|shutdown_hooks_teardown]] — `seeded` / `medium` — Shutdown Hooks Teardown

## Dependency graph

```mermaid
flowchart LR
  subgraph lifecycle["Lifecycle hooks (shutdown / class-load / exception / enum)"]
    enum_singleton([enum_singleton])
    on_class_loaded([on_class_loaded])
    on_exception([on_exception])
    shutdown_hooks_teardown([shutdown_hooks_teardown])
  end
  subgraph external["(external deps)"]
    compressed_oops_decode[/compressed_oops_decode/]
    field_introspection[/field_introspection/]
    field_static[/field_static/]
    find_class_fallback[/find_class_fallback/]
    hook_basic[/hook_basic/]
    klass_introspection[/klass_introspection/]
    wrapper_pattern[/wrapper_pattern/]
  end
  enum_singleton --> wrapper_pattern
  enum_singleton --> klass_introspection
  enum_singleton --> field_introspection
  enum_singleton --> field_static
  enum_singleton --> compressed_oops_decode
  enum_singleton --> find_class_fallback
  on_class_loaded --> hook_basic
  on_exception --> hook_basic
  shutdown_hooks_teardown --> hook_basic
```
