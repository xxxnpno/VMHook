---
category: enumeration
title: Live-VM enumeration (heap / classes / threads)
feature_count: 4
tags: [category/enumeration]
---

# Live-VM enumeration (heap / classes / threads)

**4 feature(s) in this category.**

## Features

- [[features/for_each_instance|for_each_instance]] — `in_progress` / `high` — For Each Instance
- [[features/for_each_loaded_class|for_each_loaded_class]] — `seeded` / `medium` — For Each Loaded Class
- [[features/for_each_thread|for_each_thread]] — `seeded` / `medium` — For Each Thread
- [[features/iterate_entries_safety|iterate_entries_safety]] — `seeded` / `medium` — Iterate Entries Safety

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
    compressed_klass_decode[/compressed_klass_decode/]
    decode_oop_and_pointers[/decode_oop_and_pointers/]
    klass_introspection[/klass_introspection/]
    register_class[/register_class/]
    vmstructs_offset_resolution[/vmstructs_offset_resolution/]
  end
  for_each_instance --> vmstructs_offset_resolution
  for_each_instance --> decode_oop_and_pointers
  for_each_instance --> klass_introspection
  for_each_instance --> register_class
  for_each_instance --> compressed_klass_decode
  for_each_loaded_class --> iterate_entries_safety
  for_each_loaded_class --> klass_introspection
  for_each_thread --> iterate_entries_safety
```
