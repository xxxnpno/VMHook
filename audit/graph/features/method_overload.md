---
slug: method_overload
title: Method Overload
category: method
status: seeded
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/medium, category/method]
---

# Method Overload

> **Category:** [[categories/method|Method proxies (resolve / call / dispatch)]]  ·  **Status:** `seeded`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/method_overload-specialist.md`

## Description

TODO: one-paragraph summary of what this feature does and what its input/output contract is.  Replace this with a real description so a spawned specialist can decide if the feature is relevant in ~200 tokens.

## Depends on

- [[features/signature_parsing|signature_parsing]]
- [[features/method_explicit_signature|method_explicit_signature]]

## Related

- [[features/find_methods_by_signature|find_methods_by_signature]]
- [[features/method_explicit_signature|method_explicit_signature]]
- [[features/method_is_reference|method_is_reference]]

## Depended on by

- [[features/interface_polymorphism|interface_polymorphism]]
- [[features/method_overload_java_dispatch|method_overload_java_dispatch]]
- [[features/unified_call_syntax|unified_call_syntax]]

## Tests

- `tests/jvm/modules/method_overload.cpp`

## Notes

Stub manifest — populate hpp_anchors, depends_on, known_bugs as they become known.  See audit/features/schema.md for the field reference.
