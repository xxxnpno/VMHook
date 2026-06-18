---
slug: poly_inherited_oop
title: Poly Inherited Oop
category: klass
status: seeded
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/medium, category/klass, tag/klass, tag/introspection, tag/field, tag/inheritance, tag/oop]
---

# Poly Inherited Oop

> **Category:** [[categories/klass|Class / Klass introspection]]  ·  **Status:** `seeded`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/poly_inherited_oop-specialist.md`

## Description

Resolving an inherited INSTANCE field and an inherited INSTANCE method through a
subclass wrapper by walking Klass::_super up the hierarchy, on genuine HotSpot
metadata.  Given B extends A (A declares protected int protectedInt and
protectedAdd(int); B declares its own int bInt), the feature proves that reading
protectedInt through B's klass walks one super link up to A and lands on A's
declared field at the correct offset, that B's own bInt resolves at walk depth 0,
that the same inherited slot resolved through a B wrapper and through an A wrapper
around the same oop is the identical physical address, and that inherited
protectedAdd(int) is found via the same get_super() chain and callable from a
native thread (when the JDK exports StubRoutines::_call_stub_entry).  This is the
inherited-INSTANCE counterpart to field_inherited's inherited-STATIC focus.  No
hooks armed; everything is driven through the wrappers plus one Java-witness probe.

## Depends on

- [[features/klass_introspection|klass_introspection]]
- [[features/field_inherited|field_inherited]]

## Related

- [[features/find_class_fallback|find_class_fallback]]
- [[features/interface_polymorphism|interface_polymorphism]]
- [[features/nested_classes|nested_classes]]
- [[features/field_inherited|field_inherited]]

## Implementation anchors

- `vmhook::find_field` — `vmhook/ext/vmhook/vmhook.hpp:13862-13911` — super-chain walk loop (k = k->get_super()); records declaring_klass; g_field_cache
- `vmhook::hotspot::klass::get_super` — `vmhook/ext/vmhook/vmhook.hpp:3729-3741` — reads Klass::_super via VMStructs; nullptr for Object/invalid; the B->A hop
- `vmhook::object::get_method` — `vmhook/ext/vmhook/vmhook.hpp:17937-17986` — inherited-method super-walk; first name match while walking up

## Tests

- `tests/jvm/modules/poly_inherited_oop.cpp`

## Notes

Requires a live JVM (walks real HotSpot Klass metadata).  hpp_anchors are
approximate windows into the live header (find_field walk, get_super hop,
get_method super-walk); the agent def carries the precise line-by-line map.
