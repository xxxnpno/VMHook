---
slug: method_call_jni_fallback
title: Method Call Jni Fallback
category: method
status: seeded
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/medium, category/method]
---

# Method Call Jni Fallback

> **Category:** [[categories/method|Method proxies (resolve / call / dispatch)]]  ·  **Status:** `seeded`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/method_call_jni_fallback-specialist.md`

## Description

TODO: one-paragraph summary of what this feature does and what its input/output contract is.  Replace this with a real description so a spawned specialist can decide if the feature is relevant in ~200 tokens.

## Depends on

- [[features/method_call_primitives|method_call_primitives]]
- [[features/jni_arg_packing|jni_arg_packing]]
- [[features/jni_local_ref_hygiene|jni_local_ref_hygiene]]

## Tests

- `tests/jvm/modules/method_call_jni_fallback.cpp`

## Notes

Stub manifest — populate hpp_anchors, depends_on, known_bugs as they become known.  See audit/features/schema.md for the field reference.
