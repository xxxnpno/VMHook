---
slug: interface_polymorphism
title: Interface Polymorphism
category: klass
status: seeded
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/medium, category/klass, tag/klass, tag/polymorphism, tag/interface, tag/runtime-type, tag/vtable, tag/compressed-klass, tag/default-method]
---

# Interface Polymorphism

> **Category:** [[categories/klass|Class / Klass introspection]]  ·  **Status:** `seeded`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/interface_polymorphism-specialist.md`

## Description

Handles the case where a Java field's DECLARED (static) type is an interface
but its RUNTIME type is a concrete subclass. vmhook (a) decodes the field slot
type-agnostically to whatever OOP it points at, (b) resolves the runtime klass
straight from the object header via `klass_from_oop` (narrow klass at oop+8,
decompressed) — returning the concrete `…$Dog`, never the declared `…$Animal`
interface, (c) reads concrete-only fields and dispatches the overridden
virtual through HotSpot's own vtable (the JVM enforces the polymorphism, not
the C++ wrapper type), and (d) honestly characterises the one thing it CANNOT
do — reach an interface DEFAULT method, because `get_method` walks the
superclass chain only (`get_super()` reads `Klass::_super`, never
`_transitive_interfaces`). Reading the same `pet` slot as Dog and as Animal
yields the SAME oop.

## Depends on

- [[features/klass_introspection|klass_introspection]]
- [[features/method_overload|method_overload]]

## Related

- [[features/find_class_fallback|find_class_fallback]]
- [[features/nested_classes|nested_classes]]
- [[features/poly_inherited_oop|poly_inherited_oop]]

## Implementation anchors

- `vmhook::klass_from_oop` — `vmhook/ext/vmhook/vmhook.hpp:18826-18862` — reads narrow klass at oop+8 (Windows via os::safe_read) and decompresses — returns the CONCRETE runtime klass
- `hotspot::decode_klass_pointer` — `vmhook/ext/vmhook/vmhook.hpp:5468-5505` — _base/_shift across three VMStruct eras (JDK 8-16 / 17-24 / 25+)
- `field_proxy::value_t::cast_for_variant (unique_ptr branch)` — `vmhook/ext/vmhook/vmhook.hpp:15068-15135` — type-agnostic field decode (new wrapper_type{decoded} at 15132): wraps whatever the slot points at, never consults declared type
- `object::get_method (name-only, superclass-chain walk)` — `vmhook/ext/vmhook/vmhook.hpp:17937-18000` — walks for(k=klass; k; k=k->get_super()) — superclass chain ONLY, misses interface defaults
- `klass::get_super` — `vmhook/ext/vmhook/vmhook.hpp:3729-3741` — reads ONLY Klass::_super, never _local_interfaces/_transitive_interfaces

## Tests

- `tests/jvm/modules/interface_polymorphism.cpp`

## Known bugs

- **[medium]** Interface (and interface-default) methods are unreachable via get_method — superclass-chain-only walk (all four get_method overloads iterate k=k->get_super()). Any method declared only on an implemented interface resolves to nullopt with no hint. The module characterises defaultGreet() via the Dog wrapper as [INFO], never a failure. Fix: iterate Klass::_transitive_interfaces or do a vtable-index lookup.
- **[low]** klass_from_oop reads the narrow klass unconditionally at oop+8 assuming UseCompressedClassPointers. Under -XX:-UseCompressedClassPointers the klass is a full 64-bit pointer and the u4 read at +8 yields a wrong klass. CI runs the default (compressed on under ~32 GB), so the headline runtime-type resolution is silently config-dependent.
- **[low]** klass::get_name / symbol::to_string length clamp (>0x1000 -> empty) could truncate a legitimately long internal name to empty and trip runtime-klass resolution. Not reachable for the short InterfacePoly$Dog fixture; documented hazard.

## Notes

The runtime-type headline rides entirely on decode_klass_pointer picking the
right VMStruct era and on UseCompressedClassPointers being ON (default under
~32 GB heaps). read_java_string correctness for the woof/Rex/labrador reads
branches on has_coder: JDK 8 char[]/UTF-16 vs JDK 9+ LATIN1/UTF16 by coder
byte; all three values are ASCII so they hit the LATIN1 byte-per-char branch on
9+. Interface DEFAULT methods are a Java 8+
language feature; on every supported JDK the superclass-only walk misses them,
so the [INFO] outcome is JDK-stable. The pet-slot identity proof is robust
because the unique_ptr decode (11821-11848) is genuinely type-agnostic. The
module installs NO hooks; every native observation is a side-effect-free read.
