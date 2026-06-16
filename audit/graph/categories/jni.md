---
category: jni
title: JNI plumbing (arg packing / refs / java values)
feature_count: 6
tags: [category/jni]
---

# JNI plumbing (arg packing / refs / java values)

**6 feature(s) in this category.**

## Features

- [[features/global_ref|global_ref]] — `in_progress` / `high` — Global Ref (JNI GC-survival pin)
- [[features/jni_arg_packing|jni_arg_packing]] — `seeded` / `medium` — Jni Arg Packing
- [[features/jni_local_ref_hygiene|jni_local_ref_hygiene]] — `in_progress` / `high` — JNI Local Reference Hygiene
- [[features/make_java_array|make_java_array]] — `seeded` / `medium` — Make Java Array
- [[features/make_java_string|make_java_string]] — `seeded` / `medium` — Make Java String
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
    compressed_oops_decode[/compressed_oops_decode/]
    decode_oop_and_pointers[/decode_oop_and_pointers/]
    signature_parsing[/signature_parsing/]
  end
  global_ref --> jni_local_ref_hygiene
  global_ref --> decode_oop_and_pointers
  jni_arg_packing --> signature_parsing
  make_java_array --> jni_arg_packing
  make_java_string --> jni_local_ref_hygiene
  read_java_string --> compressed_oops_decode
```
