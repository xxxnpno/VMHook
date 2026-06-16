---
category: klass
title: Class / Klass introspection
feature_count: 15
tags: [category/klass]
---

# Class / Klass introspection

**15 feature(s) in this category.**

## Features

- [[features/classloader_reanchor|classloader_reanchor]] — `seeded` / `medium` — Classloader Reanchor
- [[features/compressed_klass_decode|compressed_klass_decode]] — `seeded` / `medium` — Compressed Klass Decode
- [[features/compressed_oops_decode|compressed_oops_decode]] — `seeded` / `medium` — Compressed Oops Decode
- [[features/const_method_bounds|const_method_bounds]] — `seeded` / `medium` — Const Method Bounds
- [[features/constantpool_access|constantpool_access]] — `seeded` / `medium` — Constantpool Access
- [[features/decode_oop_and_pointers|decode_oop_and_pointers]] — `seeded` / `medium` — Decode Oop And Pointers
- [[features/find_class_context_loader|find_class_context_loader]] — `seeded` / `medium` — Find Class Context Loader
- [[features/find_class_fallback|find_class_fallback]] — `seeded` / `medium` — Find Class Fallback
- [[features/instanceklass_methods_walk|instanceklass_methods_walk]] — `seeded` / `medium` — Instanceklass Methods Walk
- [[features/interface_polymorphism|interface_polymorphism]] — `seeded` / `medium` — Interface Polymorphism
- [[features/klass_introspection|klass_introspection]] — `in_progress` / `high` — Klass Introspection
- [[features/nested_classes|nested_classes]] — `seeded` / `medium` — Nested Classes
- [[features/poly_inherited_oop|poly_inherited_oop]] — `seeded` / `medium` — Poly Inherited Oop
- [[features/register_class|register_class]] — `in_progress` / `high` — Register Class
- [[features/vmstructs_offset_resolution|vmstructs_offset_resolution]] — `in_progress` / `critical` — VMStructs Offset Resolution

## Dependency graph

```mermaid
flowchart LR
  subgraph klass["Class / Klass introspection"]
    classloader_reanchor([classloader_reanchor])
    compressed_klass_decode([compressed_klass_decode])
    compressed_oops_decode([compressed_oops_decode])
    const_method_bounds([const_method_bounds])
    constantpool_access([constantpool_access])
    decode_oop_and_pointers([decode_oop_and_pointers])
    find_class_context_loader([find_class_context_loader])
    find_class_fallback([find_class_fallback])
    instanceklass_methods_walk([instanceklass_methods_walk])
    interface_polymorphism([interface_polymorphism])
    klass_introspection([klass_introspection])
    nested_classes([nested_classes])
    poly_inherited_oop([poly_inherited_oop])
    register_class([register_class])
    vmstructs_offset_resolution([vmstructs_offset_resolution])
  end
  subgraph external["(external deps)"]
    field_inherited[/field_inherited/]
    method_overload[/method_overload/]
    os_safe_read[/os_safe_read/]
    wrapper_pattern[/wrapper_pattern/]
  end
  classloader_reanchor --> find_class_fallback
  compressed_klass_decode --> vmstructs_offset_resolution
  compressed_oops_decode --> vmstructs_offset_resolution
  const_method_bounds --> constantpool_access
  constantpool_access --> vmstructs_offset_resolution
  decode_oop_and_pointers --> compressed_oops_decode
  decode_oop_and_pointers --> os_safe_read
  find_class_context_loader --> find_class_fallback
  find_class_fallback --> vmstructs_offset_resolution
  find_class_fallback --> klass_introspection
  instanceklass_methods_walk --> vmstructs_offset_resolution
  interface_polymorphism --> klass_introspection
  interface_polymorphism --> method_overload
  klass_introspection --> vmstructs_offset_resolution
  klass_introspection --> compressed_klass_decode
  klass_introspection --> decode_oop_and_pointers
  nested_classes --> klass_introspection
  poly_inherited_oop --> klass_introspection
  poly_inherited_oop --> field_inherited
  register_class --> find_class_fallback
  register_class --> klass_introspection
  register_class --> decode_oop_and_pointers
  register_class --> wrapper_pattern
  vmstructs_offset_resolution --> os_safe_read
```
