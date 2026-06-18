---
category: enumeration
title: Live-VM enumeration (heap / classes / threads)
feature_count: 4
tags: [category/enumeration]
---

# Live-VM enumeration (heap / classes / threads)

**4 feature(s) in this category.**

## Features

- [[features/for_each_instance|for_each_instance]] — `in_progress` / `medium` — For Each Instance
- [[features/for_each_loaded_class|for_each_loaded_class]] — `in_progress` / `medium` — For Each Loaded Class
- [[features/for_each_thread|for_each_thread]] — `in_progress` / `medium` — For Each Thread
- [[features/iterate_entries_safety|iterate_entries_safety]] — `seeded` / `high` — Iterate Entries Safety

## Dependency graph

```mermaid
flowchart LR
  subgraph enumeration["Live-VM enumeration (heap / classes / threads)"]
    for_each_instance([for_each_instance])
    for_each_loaded_class([for_each_loaded_class])
    for_each_thread([for_each_thread])
    iterate_entries_safety([iterate_entries_safety])
  end
  subgraph external["(external deps)"]
    klass_introspection[/klass_introspection/]
    vmstructs_offset_resolution[/vmstructs_offset_resolution/]
  end
  for_each_instance --> klass_introspection
  for_each_instance --> iterate_entries_safety
  for_each_loaded_class --> iterate_entries_safety
  for_each_loaded_class --> klass_introspection
  for_each_thread --> iterate_entries_safety
  iterate_entries_safety --> vmstructs_offset_resolution
```
