---
category: method
title: Method proxies (resolve / call / dispatch)
feature_count: 19
tags: [category/method]
---

# Method proxies (resolve / call / dispatch)

**19 feature(s) in this category.**

## Features

- [[features/find_methods_by_signature|find_methods_by_signature]] — `seeded` / `medium` — Find Methods By Signature
- [[features/method_call_jni_fallback|method_call_jni_fallback]] — `seeded` / `medium` — Method Call Jni Fallback
- [[features/method_call_object|method_call_object]] — `seeded` / `medium` — Method Call Object
- [[features/method_call_primitives|method_call_primitives]] — `seeded` / `medium` — Method Call Primitives
- [[features/method_call_return_void|method_call_return_void]] — `seeded` / `medium` — Method Call Return Void
- [[features/method_call_string|method_call_string]] — `seeded` / `medium` — Method Call String
- [[features/method_call_wide_args|method_call_wide_args]] — `seeded` / `medium` — Method Call Wide Args
- [[features/method_enumeration|method_enumeration]] — `in_progress` / `medium` — Method Enumeration
- [[features/method_explicit_signature|method_explicit_signature]] — `seeded` / `medium` — Method Explicit Signature
- [[features/method_flags_width|method_flags_width]] — `seeded` / `medium` — Method Flags Width
- [[features/method_is_reference|method_is_reference]] — `seeded` / `medium` — Method Is Reference
- [[features/method_overload|method_overload]] — `seeded` / `medium` — Method Overload
- [[features/method_overload_java_dispatch|method_overload_java_dispatch]] — `seeded` / `medium` — Method Overload Java Dispatch
- [[features/method_proxy_value_t|method_proxy_value_t]] — `seeded` / `medium` — Method Proxy Value T
- [[features/method_return_types|method_return_types]] — `seeded` / `medium` — Method Return Types
- [[features/method_static|method_static]] — `seeded` / `medium` — Method Static
- [[features/method_static_portability|method_static_portability]] — `seeded` / `medium` — Method Static Portability
- [[features/method_throwing_call_site|method_throwing_call_site]] — `seeded` / `medium` — Method Throwing Call Site
- [[features/signature_parsing|signature_parsing]] — `in_progress` / `medium` — Signature Parsing

## Dependency graph

```mermaid
flowchart LR
  subgraph method["Method proxies (resolve / call / dispatch)"]
    find_methods_by_signature([find_methods_by_signature])
    method_call_jni_fallback([method_call_jni_fallback])
    method_call_object([method_call_object])
    method_call_primitives([method_call_primitives])
    method_call_return_void([method_call_return_void])
    method_call_string([method_call_string])
    method_call_wide_args([method_call_wide_args])
    method_enumeration([method_enumeration])
    method_explicit_signature([method_explicit_signature])
    method_flags_width([method_flags_width])
    method_is_reference([method_is_reference])
    method_overload([method_overload])
    method_overload_java_dispatch([method_overload_java_dispatch])
    method_proxy_value_t([method_proxy_value_t])
    method_return_types([method_return_types])
    method_static([method_static])
    method_static_portability([method_static_portability])
    method_throwing_call_site([method_throwing_call_site])
    signature_parsing([signature_parsing])
  end
  subgraph external["(external deps)"]
    instanceklass_methods_walk[/instanceklass_methods_walk/]
    jni_arg_packing[/jni_arg_packing/]
    jni_local_ref_hygiene[/jni_local_ref_hygiene/]
    make_java_string[/make_java_string/]
    on_exception[/on_exception/]
    read_java_string[/read_java_string/]
    register_class[/register_class/]
    vmstructs_offset_resolution[/vmstructs_offset_resolution/]
    wrapper_pattern[/wrapper_pattern/]
  end
  find_methods_by_signature --> signature_parsing
  find_methods_by_signature --> method_enumeration
  method_call_jni_fallback --> method_call_primitives
  method_call_jni_fallback --> jni_arg_packing
  method_call_jni_fallback --> jni_local_ref_hygiene
  method_call_object --> method_call_primitives
  method_call_object --> wrapper_pattern
  method_call_primitives --> method_proxy_value_t
  method_call_primitives --> method_enumeration
  method_call_return_void --> method_call_primitives
  method_call_string --> method_call_primitives
  method_call_string --> make_java_string
  method_call_string --> read_java_string
  method_call_wide_args --> method_call_primitives
  method_call_wide_args --> jni_arg_packing
  method_enumeration --> instanceklass_methods_walk
  method_enumeration --> vmstructs_offset_resolution
  method_enumeration --> register_class
  method_explicit_signature --> signature_parsing
  method_explicit_signature --> method_enumeration
  method_flags_width --> vmstructs_offset_resolution
  method_is_reference --> signature_parsing
  method_overload --> signature_parsing
  method_overload --> method_explicit_signature
  method_overload_java_dispatch --> method_overload
  method_proxy_value_t --> wrapper_pattern
  method_return_types --> method_call_primitives
  method_static --> method_call_primitives
  method_static_portability --> method_static
  method_throwing_call_site --> method_call_primitives
  method_throwing_call_site --> on_exception
  signature_parsing --> method_enumeration
```
