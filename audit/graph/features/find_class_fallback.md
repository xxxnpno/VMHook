---
slug: find_class_fallback
title: Find Class Fallback
category: klass
status: seeded
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/medium, category/klass]
---

# Find Class Fallback

> **Category:** [[categories/klass|Class / Klass introspection]]  ·  **Status:** `seeded`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/find_class_fallback-specialist.md`

## Description

TODO: one-paragraph summary of what this feature does and what its input/output contract is.  Replace this with a real description so a spawned specialist can decide if the feature is relevant in ~200 tokens.

## Depends on

- [[features/vmstructs_offset_resolution|vmstructs_offset_resolution]]
- [[features/klass_introspection|klass_introspection]]

## Related

- [[features/compressed_klass_decode|compressed_klass_decode]]
- [[features/compressed_oops_decode|compressed_oops_decode]]
- [[features/constantpool_access|constantpool_access]]
- [[features/instanceklass_methods_walk|instanceklass_methods_walk]]
- [[features/interface_polymorphism|interface_polymorphism]]
- [[features/klass_introspection|klass_introspection]]

## Depended on by

- [[features/classloader_reanchor|classloader_reanchor]]
- [[features/find_class_context_loader|find_class_context_loader]]
- [[features/hook_basic|hook_basic]]
- [[features/register_class|register_class]]

## Tests

- `tests/jvm/modules/find_class_fallback.cpp`

## Notes

Stub manifest — populate hpp_anchors, depends_on, known_bugs as they become known.  See audit/features/schema.md for the field reference.
