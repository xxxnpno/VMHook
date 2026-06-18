---
category: field
title: Field proxies (get / set / introspection)
feature_count: 14
tags: [category/field]
---

# Field proxies (get / set / introspection)

**14 feature(s) in this category.**

## Features

- [[features/field_arrays_object|field_arrays_object]] — `in_progress` / `medium` — Field Arrays Object
- [[features/field_arrays_primitive|field_arrays_primitive]] — `in_progress` / `high` — Field Arrays Primitive
- [[features/field_inherited|field_inherited]] — `in_progress` / `medium` — Field Inherited
- [[features/field_introspection|field_introspection]] — `in_progress` / `medium` — Field Introspection
- [[features/field_null_safety|field_null_safety]] — `in_progress` / `low` — Field Null Safety
- [[features/field_object_ref|field_object_ref]] — `in_progress` / `medium` — Field Object Ref
- [[features/field_primitives_get|field_primitives_get]] — `in_progress` / `high` — Field Primitives Get
- [[features/field_primitives_set|field_primitives_set]] — `in_progress` / `medium` — Field Primitives Set
- [[features/field_proxy_set_guards|field_proxy_set_guards]] — `in_progress` / `medium` — Field Proxy Set Guards
- [[features/field_proxy_value_t|field_proxy_value_t]] — `in_progress` / `medium` — Field Proxy Value T
- [[features/field_set_size_guard|field_set_size_guard]] — `in_progress` / `low` — Field Set Size Guard
- [[features/field_static|field_static]] — `in_progress` / `medium` — Field Static
- [[features/field_string|field_string]] — `in_progress` / `high` — Field String
- [[features/watch_static_field|watch_static_field]] — `seeded` / `high` — Watch Static Field

## Dependency graph

```mermaid
flowchart LR
  subgraph field["Field proxies (get / set / introspection)"]
    field_arrays_object([field_arrays_object])
    field_arrays_primitive([field_arrays_primitive])
    field_inherited([field_inherited])
    field_introspection([field_introspection])
    field_null_safety([field_null_safety])
    field_object_ref([field_object_ref])
    field_primitives_get([field_primitives_get])
    field_primitives_set([field_primitives_set])
    field_proxy_set_guards([field_proxy_set_guards])
    field_proxy_value_t([field_proxy_value_t])
    field_set_size_guard([field_set_size_guard])
    field_static([field_static])
    field_string([field_string])
    watch_static_field([watch_static_field])
  end
  subgraph external["(external deps)"]
    array_element_helpers[/array_element_helpers/]
    compressed_oops_decode[/compressed_oops_decode/]
    constantpool_access[/constantpool_access/]
    decode_oop_and_pointers[/decode_oop_and_pointers/]
    find_class_fallback[/find_class_fallback/]
    hw_breakpoint_dr7[/hw_breakpoint_dr7/]
    klass_introspection[/klass_introspection/]
    make_java_string[/make_java_string/]
    read_java_string[/read_java_string/]
    vmstructs_offset_resolution[/vmstructs_offset_resolution/]
    wrapper_pattern[/wrapper_pattern/]
  end
  field_arrays_object --> field_object_ref
  field_arrays_object --> array_element_helpers
  field_arrays_object --> wrapper_pattern
  field_arrays_object --> compressed_oops_decode
  field_arrays_primitive --> field_primitives_get
  field_arrays_primitive --> array_element_helpers
  field_inherited --> field_introspection
  field_inherited --> klass_introspection
  field_inherited --> decode_oop_and_pointers
  field_inherited --> find_class_fallback
  field_introspection --> constantpool_access
  field_introspection --> vmstructs_offset_resolution
  field_introspection --> find_class_fallback
  field_introspection --> klass_introspection
  field_null_safety --> field_introspection
  field_null_safety --> field_proxy_value_t
  field_null_safety --> find_class_fallback
  field_object_ref --> wrapper_pattern
  field_object_ref --> compressed_oops_decode
  field_primitives_get --> field_introspection
  field_primitives_get --> field_proxy_value_t
  field_primitives_set --> field_proxy_value_t
  field_primitives_set --> field_introspection
  field_primitives_set --> field_primitives_get
  field_proxy_set_guards --> field_set_size_guard
  field_proxy_set_guards --> field_proxy_value_t
  field_proxy_value_t --> field_introspection
  field_proxy_value_t --> decode_oop_and_pointers
  field_set_size_guard --> field_proxy_value_t
  field_static --> field_introspection
  field_string --> field_object_ref
  field_string --> read_java_string
  field_string --> make_java_string
  field_string --> array_element_helpers
  field_string --> decode_oop_and_pointers
  watch_static_field --> field_static
  watch_static_field --> hw_breakpoint_dr7
```
