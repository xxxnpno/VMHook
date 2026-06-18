---
slug: nested_classes
title: Nested Classes
category: klass
status: seeded
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/medium, category/klass, tag/klass, tag/introspection, tag/oop, tag/composition]
---

# Nested Classes

> **Category:** [[categories/klass|Class / Klass introspection]]  ·  **Status:** `seeded`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/nested_classes-specialist.md`

## Description

Resolving Java nested-class shapes by their javac-generated `$`-internal name
through vmhook::find_class, reading instance fields off the resolved klasses,
and decoding a non-static inner class's synthetic `this$0` back-reference into a
usable wrapper whose oop is the very enclosing instance javac wired in.  Nothing
is nested-class-specific in the header — a `$` is just another byte in the name
string — so the feature is a composition proof that the generic klass-resolution
(find_class), field-walk (find_field / klass::find_field), klass_from_oop, and
compressed-oop decode machinery all stay correct when the name carries `$`
separators and the field read is the synthetic `this$0`.  Two stable shapes are
in scope: a STATIC nested class (no synthetic outer ref) and a non-static INNER
class (synthetic this$0 field + ctor param).  Anonymous/local classes ($1, ...)
are out of scope — their names are unstable.

## Depends on

- [[features/klass_introspection|klass_introspection]]
- [[features/find_class_fallback|find_class_fallback]]

## Related

- [[features/find_class_fallback|find_class_fallback]]
- [[features/interface_polymorphism|interface_polymorphism]]
- [[features/poly_inherited_oop|poly_inherited_oop]]
- [[features/register_class|register_class]]

## Implementation anchors

- `vmhook::find_class` — `vmhook/ext/vmhook/vmhook.hpp:8004-8103` — by-$-name resolver: cache re-validation, CLD-graph walk, ctx-loader fallback
- `vmhook::klass_from_oop` — `vmhook/ext/vmhook/vmhook.hpp:18826-18862` — ties resolved-by-name klass to the actual object the fields are read off
- `vmhook::hotspot::klass::get_name` — `vmhook/ext/vmhook/vmhook.hpp:3433-3460` — echoes the exact $-name to prove the right klass (not a stale cache hit)

## Tests

- `tests/jvm/modules/nested_classes.cpp`

## Notes

Requires a live JVM (resolves and reads real $-nested klasses).  The fixture
force-instantiates each $-nested singleton in <clinit> so the klasses are
loaded before register_class/find_class run.  hpp_anchors are approximate
windows into the live header; the agent def carries the precise breakdown.
