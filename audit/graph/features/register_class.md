---
slug: register_class
title: Register Class
category: klass
status: seeded
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/medium, category/klass, tag/klass, tag/wrapper, tag/registration]
---

# Register Class

> **Category:** [[categories/klass|Class / Klass introspection]]  ·  **Status:** `seeded`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/register_class-specialist.md`

## Description

vmhook::register_class<T>(class_name) — associates a C++ wrapper type with its
Java class (internal `/`-separated name).  It verifies the class is loaded by
calling find_class up front and refuses (returns false, logs) if the class is not
found in the JVM.  On success it records two mappings under registration_mutex:
type_to_class_map (typeid(T) -> name) and g_type_factory_map (name -> factory
that heap-allocates a T wrapper for a given oop), the latter used by
frame::get_arguments to reconstruct reference arguments.  Registration is the
prerequisite for hook<T>(), for_each_instance<T>(), and instance/static wrapper
access on T.  The contract is single-threaded setup before hooks fire: detour-hot
readers do NOT take the lock; the mutex only guards concurrent registers (e.g.
lazy registration from inside a detour) against an unordered_map rehash race.

## Depends on

- [[features/wrapper_pattern|wrapper_pattern]]
- [[features/find_class_fallback|find_class_fallback]]

## Related

- [[features/classloader_reanchor|classloader_reanchor]]
- [[features/find_class_context_loader|find_class_context_loader]]
- [[features/hook_basic|hook_basic]]
- [[features/for_each_instance|for_each_instance]]

## Depended on by

- [[features/make_unique|make_unique]]

## Implementation anchors

- `vmhook::register_class` — `vmhook/ext/vmhook/vmhook.hpp:8744-8783` — find_class verify; type_to_class_map + g_type_factory_map under registration_mutex
- `vmhook::type_to_class_map / g_type_factory_map` — `vmhook/ext/vmhook/vmhook.hpp:1628-1676` — the two maps register_class populates; type_factory_function_t

## Tests

- `tests/jvm/modules/register_class.cpp`

## Notes

Requires a live JVM (find_class verification touches real klass metadata).  The
forward-declared template lives near line 1673; the definition is at ~8744.
