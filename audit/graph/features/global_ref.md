---
slug: global_ref
title: Global Ref
category: jni
status: seeded
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/medium, category/jni]
---

# Global Ref

> **Category:** [[categories/jni|JNI plumbing (arg packing / refs / java values)]]  ·  **Status:** `seeded`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/global_ref-specialist.md`

## Description

TODO: one-paragraph summary of what this feature does and what its input/output contract is.  Replace this with a real description so a spawned specialist can decide if the feature is relevant in ~200 tokens.

## Depends on

- [[features/jni_local_ref_hygiene|jni_local_ref_hygiene]]

## Related

- [[features/make_java_string|make_java_string]]

## Depended on by

- [[features/wrapper_pattern|wrapper_pattern]]

## Tests

- `tests/jvm/modules/global_ref.cpp`
- `tests/test_global_ref.cpp`

## Notes

Stub manifest — populate hpp_anchors, depends_on, known_bugs as they become known.  See audit/features/schema.md for the field reference.
