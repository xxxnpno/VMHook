---
slug: method_static
title: Method Static
category: method
status: seeded
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/medium, category/method]
---

# Method Static

> **Category:** [[categories/method|Method proxies (resolve / call / dispatch)]]  ·  **Status:** `seeded`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/method_static-specialist.md`

## Description

TODO: one-paragraph summary of what this feature does and what its input/output contract is.  Replace this with a real description so a spawned specialist can decide if the feature is relevant in ~200 tokens.

## Depends on

- [[features/method_call_primitives|method_call_primitives]]

## Related

- [[features/method_call_jni_fallback|method_call_jni_fallback]]
- [[features/method_call_object|method_call_object]]
- [[features/method_call_return_void|method_call_return_void]]
- [[features/method_call_string|method_call_string]]
- [[features/method_call_wide_args|method_call_wide_args]]
- [[features/method_return_types|method_return_types]]

## Depended on by

- [[features/method_static_portability|method_static_portability]]

## Tests

- `tests/jvm/modules/method_static.cpp`

## Notes

Stub manifest — populate hpp_anchors, depends_on, known_bugs as they become known.  See audit/features/schema.md for the field reference.
