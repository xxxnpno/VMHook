---
slug: field_static
title: Field Static
category: field
status: seeded
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/medium, category/field]
---

# Field Static

> **Category:** [[categories/field|Field proxies (get / set / introspection)]]  ·  **Status:** `seeded`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/field_static-specialist.md`

## Description

TODO: one-paragraph summary of what this feature does and what its input/output contract is.  Replace this with a real description so a spawned specialist can decide if the feature is relevant in ~200 tokens.

## Depends on

- [[features/field_introspection|field_introspection]]

## Related

- [[features/field_inherited|field_inherited]]
- [[features/field_null_safety|field_null_safety]]
- [[features/field_primitives_get|field_primitives_get]]
- [[features/field_proxy_value_t|field_proxy_value_t]]

## Depended on by

- [[features/enum_singleton|enum_singleton]]
- [[features/watch_static_field|watch_static_field]]

## Tests

- `tests/jvm/modules/field_static.cpp`

## Notes

Stub manifest — populate hpp_anchors, depends_on, known_bugs as they become known.  See audit/features/schema.md for the field reference.
