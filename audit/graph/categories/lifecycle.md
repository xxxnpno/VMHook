---
category: lifecycle
title: Lifecycle hooks (shutdown / class-load / exception / enum)
feature_count: 4
tags: [category/lifecycle]
---

# Lifecycle hooks (shutdown / class-load / exception / enum)

**4 feature(s) in this category.**

## Features

- [[features/enum_singleton|enum_singleton]] — `seeded` / `medium` — Enum Singleton
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
    hook_basic[/hook_basic/]
    klass_introspection[/klass_introspection/]
    wrapper_pattern[/wrapper_pattern/]
  end
  enum_singleton --> wrapper_pattern
  enum_singleton --> klass_introspection
  on_class_loaded --> hook_basic
  on_exception --> hook_basic
  shutdown_hooks_teardown --> hook_basic
```
