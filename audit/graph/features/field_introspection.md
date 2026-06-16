---
slug: field_introspection
title: Field Introspection
category: field
status: in_progress
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/in_progress, risk/medium, category/field, tag/introspection, tag/field, tag/reference, tag/primitive, tag/static, tag/instance, tag/oop, tag/addressing]
---

# Field Introspection

> **Category:** [[categories/field|Field proxies (get / set / introspection)]]  ·  **Status:** `in_progress`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/field_introspection-specialist.md`

## Description

Five introspection accessors on field_proxy expose the exact JVM type descriptor,
static/instance flag, reference/primitive classification, raw backing memory address,
and decoded compressed OOP. Covers all field shapes (eight primitives, String refs,
arrays, object refs, interface refs, self-references) across instance and static
field contexts. Input contract: a resolved field_proxy (from static_field / get_field).
Output contract: signature() yields exact descriptor (Z/B/S/I/J/F/D/C/L.../[...),
is_static() / is_reference() return boolean invariants, raw_address() and
get_compressed_oop() return addressing and OOP decoding; known flaws: raw_address
has GC-staleness risk (no pinning), get_compressed_oop reads exactly 4 bytes (low
half of J/D) and lacks signature guard on primitive fields.

## Depends on

- [[features/constantpool_access|constantpool_access]]
- [[features/vmstructs_offset_resolution|vmstructs_offset_resolution]]
- [[features/find_class_fallback|find_class_fallback]]
- [[features/klass_introspection|klass_introspection]]

## Depended on by

- [[features/enum_singleton|enum_singleton]]
- [[features/field_inherited|field_inherited]]
- [[features/field_null_safety|field_null_safety]]
- [[features/field_primitives_get|field_primitives_get]]
- [[features/field_primitives_set|field_primitives_set]]
- [[features/field_proxy_value_t|field_proxy_value_t]]
- [[features/field_static|field_static]]

## Implementation anchors

- `field_proxy::signature()` — `vmhook/ext/vmhook/vmhook.hpp:15532-15536` — returns std::string_view of JVM descriptor (Z/B/S/I/J/F/D/C/L.../[...)
- `field_proxy::is_static()` — `vmhook/ext/vmhook/vmhook.hpp:15560-15564` — reflects JVM_ACC_STATIC flag; true for class-level fields regardless of accessor path
- `field_proxy::is_reference()` — `vmhook/ext/vmhook/vmhook.hpp:15578-15587` — returns signature()[0] is L or [; complement of jvm_primitive_byte_width(sig) != 0
- `field_proxy::raw_address()` — `vmhook/ext/vmhook/vmhook.hpp:15546-15549` — non-null for resolved field; byte-equal to independently-recomputed mirror+offset (static) / oop+offset (instance)
- `field_proxy::get_compressed_oop()` — `vmhook/ext/vmhook/vmhook.hpp:15593-15650` — decodes compressed OOP from 32-bit field slot; guards on is_reference() after FLAW C fix; returns 0 for null or invalid pointer

## Tests

- `tests/jvm/modules/field_introspection.cpp`

## Known bugs

- **[high]** raw_address() has GC-staleness risk: instance fields return cached oop+offset without pinning or live-mirror re-resolution; a relocating GC can invalidate the oop after lookup, leaving raw_address pointing to a stale heap location (lines 15546-15549). Static fields similarly return field_pointer (mirror+offset) without live-mirror validation at read time.
- **[medium]** get_compressed_oop() reads exactly 4 bytes (lines 15648): reading low 32 bits of a 64-bit J/D field yields garbage in the high word. Caller must account for double-width primitive fields (e.g., Long/Double not addressable as compressed OOP slots).
- **[medium]** get_compressed_oop() pre-FLAW-C accepted primitive fields and returned bogus decoded pointers: lines 15596-15605 now guard on is_reference(), returning 0 for primitives, but the function signature carries no type safety — misuse (calling on a non-reference field) still type-checks.

## Notes

JDK-sensitive: signature descriptors match Java 8–26 type system exactly
(primitives Z/B/S/I/J/F/D/C, refs Ljava/lang/..., arrays [...). GC
behavior (mirror relocation, class unloading) varies by JDK version; Mode 2
fixture forces GC between lookups to document raw_address staleness.
