---
slug: jni_arg_packing
title: Jni Arg Packing
category: jni
status: seeded
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/medium, category/jni]
---

# Jni Arg Packing

> **Category:** [[categories/jni|JNI plumbing (arg packing / refs / java values)]]  ·  **Status:** `seeded`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/jni_arg_packing-specialist.md`

## Description

TODO: one-paragraph summary of what this feature does and what its input/output contract is.  Replace this with a real description so a spawned specialist can decide if the feature is relevant in ~200 tokens.

## Depends on

- [[features/signature_parsing|signature_parsing]]

## Depended on by

- [[features/make_java_array|make_java_array]]
- [[features/method_call_jni_fallback|method_call_jni_fallback]]
- [[features/method_call_wide_args|method_call_wide_args]]

## Tests

- `tests/test_jni_arg_packing.cpp`

## Notes

Stub manifest — populate hpp_anchors, depends_on, known_bugs as they become known.  See audit/features/schema.md for the field reference.
