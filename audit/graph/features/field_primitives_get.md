---
slug: field_primitives_get
title: Field Primitives Get
category: field
status: in_progress
risk: high
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/in_progress, risk/high, category/field, tag/primitive, tag/field-access, tag/variant, tag/value_t, tag/memcpy, tag/descriptor]
---

# Field Primitives Get

> **Category:** [[categories/field|Field proxies (get / set / introspection)]]  ·  **Status:** `in_progress`  ·  **Risk:** `high`  ·  **Specialist:** `.claude/agents/field_primitives_get-specialist.md`

## Description

Read side of direct field access for all JVM primitives: a linear signature-dispatch chain
in `field_proxy::get()` that memcpy's the field value into a fixed-width C++ local and wraps
it in a `value_t` variant. Input is a decoded object pointer + field offset (instance or static);
output is a `value_t` whose alternative (bool/int8/…/uint32) is determined by the JVM
descriptor. Surprising: null-pointer fallback always returns int32_t variant regardless of
signature; bool reads raw bytes instead of normalizing; no width guard on Z/B/S reads; unspecified
behavior on NaN/Inf to integer casts; Java char > 0x7F silently narrows to C++ char.

## Depends on

- [[features/field_introspection|field_introspection]]
- [[features/field_proxy_value_t|field_proxy_value_t]]

## Related

- [[features/field_inherited|field_inherited]]
- [[features/field_null_safety|field_null_safety]]
- [[features/field_primitives_set|field_primitives_set]]
- [[features/field_proxy_set_guards|field_proxy_set_guards]]
- [[features/field_set_size_guard|field_set_size_guard]]

## Depended on by

- [[features/field_arrays_primitive|field_arrays_primitive]]
- [[features/field_primitives_set|field_primitives_set]]

## Implementation anchors

- `field_proxy::get()` — `vmhook/ext/vmhook/vmhook.hpp:11548-11609` — Linear if-chain dispatching on signature_text; each primitive declares local, memcpy's sizeof(local) bytes from field_pointer, wraps in value_t
- `field_proxy::get() null-pointer fallback` — `vmhook/ext/vmhook/vmhook.hpp:11551-11554` — Returns value_t{ int32_t{}, signature_text } for every signature when field_pointer is null
- `value_t variant definition` — `vmhook/ext/vmhook/vmhook.hpp:11270-11522` — Variant alternatives ordered: bool(0), int8(1), int16(2), int32(3), int64(4), float(5), double(6), uint16(7), uint32(8)
- `cast_for_variant<T>` — `vmhook/ext/vmhook/vmhook.hpp:11404-11470` — Implicit-conversion engine; OOP alternatives only convert from uint32_t; others use static_cast<T>
- `cast_for_variant generic branch (UB site)` — `vmhook/ext/vmhook/vmhook.hpp:11462-11465` — Unconstrained static_cast<T>(value) — UB for non-finite float/double to integer
- `object::get_field()` — `vmhook/ext/vmhook/vmhook.hpp:13517-13557` — Computes field_pointer = decoded_object + offset (instance) or mirror + offset (static); constructs proxy

## Tests

- `tests/jvm/modules/field_primitives_get.cpp`

## Known bugs

- **[high]** Z raw-memcpy into bool can synthesize non-canonical bool (11558-11560). bool value{}; memcpy(&value, ptr, sizeof(value)) copies raw byte straight into bool with UB on any non-0/1 value. Every sibling site normalizes (method_proxy::call does (raw & 1), [Z array reader does != 0); only get() reads raw. Fix: read through uint8_t then raw != 0.
- **[high]** Z reads sizeof(bool), not 1 byte (11558-11559). Implementation-defined width; on ABI where sizeof(bool) > 1 this over-reads past 1-byte JVM Z slot into next field. Unlike set() (gated by jvm_primitive_byte_width), get() has no width guard.
- **[high]** Null-pointer fallback returns wrong variant alternative for every signature (11551-11554). Null J/D/F/C proxy returns int32_t{0}, not descriptor's alternative, so std::holds_alternative<int64_t> false for long and std::get<bool> throws bad_variant_access.
- **[high]** static_cast<int>(NaN/Inf) is UB and static_cast<bool>(NaN) is true (cast_for_variant generic 11462-11465). Reading F/D field holding NaN/±Inf into integer executes UB static_cast per [conv.fpint]; NaN float to bool silently reports true.
- **[medium]** Java char > 0x7F silently narrows when target is C++ char (cast_for_variant generic). get() loads full uint16_t correctly but char c = proxy->get() truncates: U+4E2D becomes 0x2D, U+00E9 becomes negative char. Lossless char16_t/uint16_t path correct; only 1-byte target loses data.
- **[medium]** No diagnostic on unknown/malformed signature (11605-11608). Typo'd descriptor (Q, empty) silently falls through to 4-byte compressed-OOP read and uint32_t alternative, unlike set() which logs and guards.
- **[low]** Doc drift: doc-comments at vmhook.hpp:1447 and 6821 reference field_proxy::get_as<T>() which does not exist. Class exposes only get()/set()/signature()/raw_address()/is_static()/get_compressed_oop().

## Notes

NaN canonicalization: HotSpot stores Float.NaN as canonical qNaN 0x7FC00000 (double 0x7FF8…).
Signaling-NaN or payload NaN written via intBitsToFloat must survive get()'s memcpy untouched on every JDK 8..26 —
any float→double→float promotion would canonicalise payload, so F/D values are read into matching type only and bit-checked.
char encoding: fixture uses numeric/\uXXXX literals only (pure ASCII) for javac portability across Windows (Cp1252) and Unix (UTF-8).
GCC portability: static_field("name") used for all static context (always available); get_field() reserved for true instance (deducing-this overloads absent on GCC).
Compressed OOPs: reference/array fall-through reads 4-byte narrow OOP (default); out of scope for primitives but instance field is real reference for JDK portability.
