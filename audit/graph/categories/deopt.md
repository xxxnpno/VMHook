---
category: deopt
title: De-optimisation
feature_count: 1
tags: [category/deopt]
---

# De-optimisation

**1 feature(s) in this category.**

## Features

- [[features/deoptimize_methods|deoptimize_methods]] — `in_progress` / `medium` — Deoptimize Methods

## Dependency graph

```mermaid
flowchart LR
  subgraph deopt["De-optimisation"]
    deoptimize_methods([deoptimize_methods])
  end
  subgraph external["(external deps)"]
    adapter_recovery_c2i[/adapter_recovery_c2i/]
    for_each_loaded_class[/for_each_loaded_class/]
    instanceklass_methods_walk[/instanceklass_methods_walk/]
    method_entry_points_i2i_i2c[/method_entry_points_i2i_i2c/]
    method_enumeration[/method_enumeration/]
    vmstructs_offset_resolution[/vmstructs_offset_resolution/]
  end
  deoptimize_methods --> method_enumeration
  deoptimize_methods --> adapter_recovery_c2i
  deoptimize_methods --> method_entry_points_i2i_i2c
  deoptimize_methods --> for_each_loaded_class
  deoptimize_methods --> vmstructs_offset_resolution
  deoptimize_methods --> instanceklass_methods_walk
```
