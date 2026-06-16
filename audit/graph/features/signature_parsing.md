---
slug: signature_parsing
title: Signature Parsing
category: method
status: seeded
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/medium, category/method]
---

# Signature Parsing

> **Category:** [[categories/method|Method proxies (resolve / call / dispatch)]]  ·  **Status:** `seeded`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/signature_parsing-specialist.md`

## Description

TODO: one-paragraph summary of what this feature does and what its input/output contract is.  Replace this with a real description so a spawned specialist can decide if the feature is relevant in ~200 tokens.

## Depended on by

- [[features/find_methods_by_signature|find_methods_by_signature]]
- [[features/hook_basic|hook_basic]]
- [[features/hook_signature|hook_signature]]
- [[features/jni_arg_packing|jni_arg_packing]]
- [[features/method_explicit_signature|method_explicit_signature]]
- [[features/method_is_reference|method_is_reference]]
- [[features/method_overload|method_overload]]

## Referenced from

- [[features/constantpool_access|constantpool_access]]

## Tests

- `tests/jvm/modules/signature_parsing.cpp`
- `tests/test_signature_parsing.cpp`

## Notes

Stub manifest — populate hpp_anchors, depends_on, known_bugs as they become known.  See audit/features/schema.md for the field reference.
