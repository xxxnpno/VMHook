---
category: klass
title: Class / Klass introspection
feature_count: 15
tags: [category/klass]
---

# Class / Klass introspection

**15 feature(s) in this category.**

## Features

- [[features/classloader_reanchor|classloader_reanchor]] — `in_progress` / `medium` — Classloader Reanchor
- [[features/compressed_klass_decode|compressed_klass_decode]] — `in_progress` / `medium` — Compressed Klass Decode
- [[features/compressed_oops_decode|compressed_oops_decode]] — `in_progress` / `medium` — Compressed Oops Decode
- [[features/const_method_bounds|const_method_bounds]] — `in_progress` / `medium` — Const Method Bounds
- [[features/constantpool_access|constantpool_access]] — `in_progress` / `high` — Constantpool Access
- [[features/decode_oop_and_pointers|decode_oop_and_pointers]] — `in_progress` / `medium` — Decode Oop And Pointers
- [[features/find_class_context_loader|find_class_context_loader]] — `in_progress` / `high` — Find Class Context Loader
- [[features/find_class_fallback|find_class_fallback]] — `seeded` / `medium` — Find Class Fallback
- [[features/instanceklass_methods_walk|instanceklass_methods_walk]] — `seeded` / `medium` — Instanceklass Methods Walk
- [[features/interface_polymorphism|interface_polymorphism]] — `seeded` / `medium` — Interface Polymorphism
- [[features/klass_introspection|klass_introspection]] — `seeded` / `medium` — Klass Introspection
- [[features/nested_classes|nested_classes]] — `seeded` / `medium` — Nested Classes
- [[features/poly_inherited_oop|poly_inherited_oop]] — `seeded` / `medium` — Poly Inherited Oop
- [[features/register_class|register_class]] — `seeded` / `medium` — Register Class
- [[features/vmstructs_offset_resolution|vmstructs_offset_resolution]] — `seeded` / `medium` — Vmstructs Offset Resolution

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
    jni_local_ref_hygiene[/jni_local_ref_hygiene/]
    method_overload[/method_overload/]
    os_query_region[/os_query_region/]
    os_safe_read[/os_safe_read/]
    wrapper_pattern[/wrapper_pattern/]
  end
  classloader_reanchor --> find_class_fallback
  compressed_klass_decode --> vmstructs_offset_resolution
  compressed_klass_decode --> compressed_oops_decode
  compressed_klass_decode --> decode_oop_and_pointers
  compressed_oops_decode --> vmstructs_offset_resolution
  const_method_bounds --> constantpool_access
  const_method_bounds --> decode_oop_and_pointers
  const_method_bounds --> os_query_region
  constantpool_access --> vmstructs_offset_resolution
  decode_oop_and_pointers --> compressed_oops_decode
  decode_oop_and_pointers --> os_safe_read
  find_class_context_loader --> find_class_fallback
  find_class_context_loader --> klass_introspection
  find_class_context_loader --> decode_oop_and_pointers
  find_class_context_loader --> jni_local_ref_hygiene
  find_class_fallback --> vmstructs_offset_resolution
  find_class_fallback --> klass_introspection
  instanceklass_methods_walk --> vmstructs_offset_resolution
  interface_polymorphism --> klass_introspection
  interface_polymorphism --> method_overload
  klass_introspection --> vmstructs_offset_resolution
  klass_introspection --> instanceklass_methods_walk
  klass_introspection --> compressed_klass_decode
  nested_classes --> klass_introspection
  poly_inherited_oop --> klass_introspection
  poly_inherited_oop --> field_inherited
  register_class --> wrapper_pattern
  register_class --> find_class_fallback
  vmstructs_offset_resolution --> os_safe_read
```
