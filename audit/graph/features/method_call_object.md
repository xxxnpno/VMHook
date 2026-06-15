---
slug: method_call_object
title: Method Call Object
category: method
status: seeded
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/medium, category/method]
---

# Method Call Object

> **Category:** [[categories/method|Method proxies (resolve / call / dispatch)]]  ·  **Status:** `seeded`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/method_call_object-specialist.md`

## Description

TODO: one-paragraph summary of what this feature does and what its input/output contract is.  Replace this with a real description so a spawned specialist can decide if the feature is relevant in ~200 tokens.

## Depends on

- [[features/method_call_primitives|method_call_primitives]]
- [[features/wrapper_pattern|wrapper_pattern]]

## Related

- [[features/method_call_jni_fallback|method_call_jni_fallback]]
- [[features/method_call_return_void|method_call_return_void]]
- [[features/method_call_string|method_call_string]]
- [[features/method_call_wide_args|method_call_wide_args]]
- [[features/method_proxy_value_t|method_proxy_value_t]]
- [[features/method_return_types|method_return_types]]

## Tests

- `tests/jvm/modules/method_call_object.cpp`

## Notes

Stub manifest — populate hpp_anchors, depends_on, known_bugs as they become known.  See audit/features/schema.md for the field reference.
