---
slug: read_java_string
title: Read Java String
category: jni
status: seeded
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/medium, category/jni]
---

# Read Java String

> **Category:** [[categories/jni|JNI plumbing (arg packing / refs / java values)]]  ·  **Status:** `seeded`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/read_java_string-specialist.md`

## Description

TODO: one-paragraph summary of what this feature does and what its input/output contract is.  Replace this with a real description so a spawned specialist can decide if the feature is relevant in ~200 tokens.

## Depends on

- [[features/compressed_oops_decode|compressed_oops_decode]]

## Depended on by

- [[features/field_string|field_string]]
- [[features/method_call_string|method_call_string]]

## Tests

- `tests/jvm/modules/read_java_string.cpp`

## Notes

Stub manifest — populate hpp_anchors, depends_on, known_bugs as they become known.  See audit/features/schema.md for the field reference.
