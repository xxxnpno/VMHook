---
category: jni
title: JNI plumbing (arg packing / refs / java values)
feature_count: 6
tags: [category/jni]
---

# JNI plumbing (arg packing / refs / java values)

**6 feature(s) in this category.**

## Features

- [[features/global_ref|global_ref]] — `in_progress` / `medium` — Global Ref
- [[features/jni_arg_packing|jni_arg_packing]] — `seeded` / `medium` — Jni Arg Packing
- [[features/jni_local_ref_hygiene|jni_local_ref_hygiene]] — `seeded` / `high` — Jni Local Ref Hygiene
- [[features/make_java_array|make_java_array]] — `seeded` / `high` — Make Java Array
- [[features/make_java_string|make_java_string]] — `in_progress` / `medium` — Make Java String
- [[features/read_java_string|read_java_string]] — `seeded` / `medium` — Read Java String

## Dependency graph

```mermaid
flowchart LR
  subgraph jni["JNI plumbing (arg packing / refs / java values)"]
    global_ref([global_ref])
    jni_arg_packing([jni_arg_packing])
    jni_local_ref_hygiene([jni_local_ref_hygiene])
    make_java_array([make_java_array])
    make_java_string([make_java_string])
    read_java_string([read_java_string])
  end
  subgraph external["(external deps)"]
    compressed_klass_decode[/compressed_klass_decode/]
    compressed_oops_decode[/compressed_oops_decode/]
    find_class_fallback[/find_class_fallback/]
    os_safe_read[/os_safe_read/]
    signature_parsing[/signature_parsing/]
  end
  global_ref --> jni_local_ref_hygiene
  jni_arg_packing --> signature_parsing
  jni_local_ref_hygiene --> jni_arg_packing
  make_java_array --> find_class_fallback
  make_java_array --> compressed_klass_decode
  make_java_array --> compressed_oops_decode
  make_java_string --> read_java_string
  make_java_string --> jni_local_ref_hygiene
  read_java_string --> compressed_oops_decode
  read_java_string --> os_safe_read
```
