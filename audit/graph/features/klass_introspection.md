---
slug: klass_introspection
title: Klass Introspection
category: klass
status: seeded
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/medium, category/klass]
---

# Klass Introspection

> **Category:** [[categories/klass|Class / Klass introspection]]  ·  **Status:** `seeded`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/klass_introspection-specialist.md`

## Description

TODO: one-paragraph summary of what this feature does and what its input/output contract is.  Replace this with a real description so a spawned specialist can decide if the feature is relevant in ~200 tokens.

## Depends on

- [[features/vmstructs_offset_resolution|vmstructs_offset_resolution]]
- [[features/instanceklass_methods_walk|instanceklass_methods_walk]]
- [[features/compressed_klass_decode|compressed_klass_decode]]

## Related

- [[features/compressed_klass_decode|compressed_klass_decode]]
- [[features/compressed_oops_decode|compressed_oops_decode]]
- [[features/constantpool_access|constantpool_access]]
- [[features/find_class_fallback|find_class_fallback]]
- [[features/instanceklass_methods_walk|instanceklass_methods_walk]]

## Depended on by

- [[features/collection_type_tags|collection_type_tags]]
- [[features/enum_singleton|enum_singleton]]
- [[features/field_inherited|field_inherited]]
- [[features/find_class_fallback|find_class_fallback]]
- [[features/for_each_instance|for_each_instance]]
- [[features/for_each_loaded_class|for_each_loaded_class]]
- [[features/hook_basic|hook_basic]]
- [[features/interface_polymorphism|interface_polymorphism]]
- [[features/nested_classes|nested_classes]]
- [[features/poly_inherited_oop|poly_inherited_oop]]

## Tests

- `tests/jvm/modules/klass_introspection.cpp`
- `tests/jvm/modules/klass_introspection_shapes.cpp`

## Notes

Stub manifest — populate hpp_anchors, depends_on, known_bugs as they become known.  See audit/features/schema.md for the field reference.
