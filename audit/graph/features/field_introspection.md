---
slug: field_introspection
title: Field Introspection
category: field
status: seeded
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/medium, category/field]
---

# Field Introspection

> **Category:** [[categories/field|Field proxies (get / set / introspection)]]  ·  **Status:** `seeded`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/field_introspection-specialist.md`

## Description

TODO: one-paragraph summary of what this feature does and what its input/output contract is.  Replace this with a real description so a spawned specialist can decide if the feature is relevant in ~200 tokens.

## Depends on

- [[features/constantpool_access|constantpool_access]]
- [[features/vmstructs_offset_resolution|vmstructs_offset_resolution]]

## Depended on by

- [[features/field_inherited|field_inherited]]
- [[features/field_null_safety|field_null_safety]]
- [[features/field_primitives_get|field_primitives_get]]
- [[features/field_proxy_value_t|field_proxy_value_t]]
- [[features/field_static|field_static]]

## Tests

- `tests/jvm/modules/field_introspection.cpp`

## Notes

Stub manifest — populate hpp_anchors, depends_on, known_bugs as they become known.  See audit/features/schema.md for the field reference.
