---
slug: method_enumeration
title: Method Enumeration
category: method
status: seeded
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/medium, category/method]
---

# Method Enumeration

> **Category:** [[categories/method|Method proxies (resolve / call / dispatch)]]  ·  **Status:** `seeded`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/method_enumeration-specialist.md`

## Description

TODO: one-paragraph summary of what this feature does and what its input/output contract is.  Replace this with a real description so a spawned specialist can decide if the feature is relevant in ~200 tokens.

## Depends on

- [[features/instanceklass_methods_walk|instanceklass_methods_walk]]

## Depended on by

- [[features/deoptimize_methods|deoptimize_methods]]
- [[features/find_methods_by_signature|find_methods_by_signature]]
- [[features/method_call_primitives|method_call_primitives]]
- [[features/method_explicit_signature|method_explicit_signature]]

## Tests

- `tests/jvm/modules/method_enumeration.cpp`
- `tests/test_method_enumeration.cpp`

## Notes

Stub manifest — populate hpp_anchors, depends_on, known_bugs as they become known.  See audit/features/schema.md for the field reference.
