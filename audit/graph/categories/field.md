---
category: field
title: Field proxies (get / set / introspection)
feature_count: 14
tags: [category/field]
---

# Field proxies (get / set / introspection)

**14 feature(s) in this category.**

## Features

- [[features/field_arrays_object|field_arrays_object]] — `seeded` / `medium` — Field Arrays Object
- [[features/field_arrays_primitive|field_arrays_primitive]] — `seeded` / `medium` — Field Arrays Primitive
- [[features/field_inherited|field_inherited]] — `seeded` / `medium` — Field Inherited
- [[features/field_introspection|field_introspection]] — `seeded` / `medium` — Field Introspection
- [[features/field_null_safety|field_null_safety]] — `seeded` / `medium` — Field Null Safety
- [[features/field_object_ref|field_object_ref]] — `seeded` / `medium` — Field Object Ref
- [[features/field_primitives_get|field_primitives_get]] — `seeded` / `medium` — Field Primitives Get
- [[features/field_primitives_set|field_primitives_set]] — `seeded` / `medium` — Field Primitives Set
- [[features/field_proxy_set_guards|field_proxy_set_guards]] — `seeded` / `medium` — Field Proxy Set Guards
- [[features/field_proxy_value_t|field_proxy_value_t]] — `seeded` / `medium` — Field Proxy Value T
- [[features/field_set_size_guard|field_set_size_guard]] — `seeded` / `medium` — Field Set Size Guard
- [[features/field_static|field_static]] — `seeded` / `medium` — Field Static
- [[features/field_string|field_string]] — `seeded` / `medium` — Field String
- [[features/watch_static_field|watch_static_field]] — `seeded` / `medium` — Watch Static Field

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
  field_arrays_primitive --> field_primitives_get
  field_arrays_primitive --> array_element_helpers
  field_inherited --> field_introspection
  field_inherited --> klass_introspection
  field_introspection --> constantpool_access
  field_introspection --> vmstructs_offset_resolution
  field_null_safety --> field_introspection
  field_object_ref --> wrapper_pattern
  field_object_ref --> compressed_oops_decode
  field_primitives_get --> field_introspection
  field_primitives_get --> field_proxy_value_t
  field_primitives_set --> field_proxy_set_guards
  field_primitives_set --> field_proxy_value_t
  field_proxy_set_guards --> field_set_size_guard
  field_proxy_set_guards --> field_proxy_value_t
  field_proxy_value_t --> field_introspection
  field_set_size_guard --> field_proxy_value_t
  field_static --> field_introspection
  field_string --> field_object_ref
  field_string --> read_java_string
  field_string --> make_java_string
  watch_static_field --> field_static
  watch_static_field --> hw_breakpoint_dr7
```
