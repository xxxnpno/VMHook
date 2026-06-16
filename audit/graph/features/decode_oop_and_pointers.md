---
slug: decode_oop_and_pointers
title: Decode Oop And Pointers
category: klass
status: seeded
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/medium, category/klass]
---

# Decode Oop And Pointers

> **Category:** [[categories/klass|Class / Klass introspection]]  ·  **Status:** `seeded`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/decode_oop_and_pointers-specialist.md`

## Description

TODO: one-paragraph summary of what this feature does and what its input/output contract is.  Replace this with a real description so a spawned specialist can decide if the feature is relevant in ~200 tokens.

## Depends on

- [[features/compressed_oops_decode|compressed_oops_decode]]
- [[features/os_safe_read|os_safe_read]]

## Related

- [[features/vmstructs_offset_resolution|vmstructs_offset_resolution]]

## Depended on by

- [[features/for_each_instance|for_each_instance]]
- [[features/global_ref|global_ref]]
- [[features/hook_basic|hook_basic]]
- [[features/klass_introspection|klass_introspection]]
- [[features/read_java_string|read_java_string]]
- [[features/register_class|register_class]]

## Tests

- `tests/test_decode_oop_and_pointers.cpp`

## Notes

Stub manifest — populate hpp_anchors, depends_on, known_bugs as they become known.  See audit/features/schema.md for the field reference.
