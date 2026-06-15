---
slug: compressed_oops_decode
title: Compressed Oops Decode
category: klass
status: seeded
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/medium, category/klass]
---

# Compressed Oops Decode

> **Category:** [[categories/klass|Class / Klass introspection]]  ·  **Status:** `seeded`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/compressed_oops_decode-specialist.md`

## Description

TODO: one-paragraph summary of what this feature does and what its input/output contract is.  Replace this with a real description so a spawned specialist can decide if the feature is relevant in ~200 tokens.

## Depends on

- [[features/vmstructs_offset_resolution|vmstructs_offset_resolution]]

## Related

- [[features/compressed_klass_decode|compressed_klass_decode]]
- [[features/constantpool_access|constantpool_access]]
- [[features/find_class_fallback|find_class_fallback]]
- [[features/instanceklass_methods_walk|instanceklass_methods_walk]]
- [[features/klass_introspection|klass_introspection]]

## Depended on by

- [[features/decode_oop_and_pointers|decode_oop_and_pointers]]
- [[features/field_object_ref|field_object_ref]]
- [[features/read_java_string|read_java_string]]

## Tests

- `tests/jvm/modules/compressed_oops_decode.cpp`

## Notes

Stub manifest — populate hpp_anchors, depends_on, known_bugs as they become known.  See audit/features/schema.md for the field reference.
