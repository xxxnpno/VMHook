---
slug: field_arrays_primitive
title: Field Arrays Primitive
category: field
status: in_progress
risk: high
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/in_progress, risk/high, category/field, tag/field, tag/array, tag/primitive, tag/vector, tag/noexcept, tag/bounds-check]
---

# Field Arrays Primitive

> **Category:** [[categories/field|Field proxies (get / set / introspection)]]  ·  **Status:** `in_progress`  ·  **Risk:** `high`  ·  **Specialist:** `.claude/agents/field_arrays_primitive-specialist.md`

## Description

Converts Java primitive array fields (`[Z [B [S [C [I [J [F [D`) into `std::vector<T>` on the C++ side.
The read path decodes a compressed array OOP, reads array length from heap offset +12, and loops over
heap offset +16 to extract elements. Input is a compressed-OOP array reference and an optional type signature.
Output is a vector populated by implicit conversion operator. Critical subtlety: `operator vector<T>()`
on a primitive array is distinct from `to_vector<T>()` which is the object-array path.

## Depends on

- [[features/field_primitives_get|field_primitives_get]]
- [[features/array_element_helpers|array_element_helpers]]

## Related

- [[features/field_arrays_object|field_arrays_object]]

## Implementation anchors

- `vmhook::array_length(void*)` — `vmhook/ext/vmhook/vmhook.hpp:11154-11160` — reads int at array_oop + 12; returns 0 for null or invalid pointer
- `vmhook::get_array_element<T>(void*, int32)` — `vmhook/ext/vmhook/vmhook.hpp:11176-11190` — memcpy sizeof(T) bytes from array_oop + 16 + index*sizeof(T); bounds check index-only
- `field_proxy::value_t::append_array_value(...)` — `vmhook/ext/vmhook/vmhook.hpp:11291-11350` — overloads for vector<bool>, vector<string>, vector<char> (lossy uint16 narrowing), and generic vector<T>
- `field_proxy::value_t::read_array_value<vector<T>>(uint32, sig)` — `vmhook/ext/vmhook/vmhook.hpp:11360-11410` — decodes array OOP, reads length, early-outs on length<=0, reserve(length), loops append_array_value per index
- `cast_for_variant<vector<T>>` — `vmhook/ext/vmhook/vmhook.hpp:11422-11430` — variant cast path reached by implicit operator target_type()
- `field_proxy::value_t::operator target_type()` — `vmhook/ext/vmhook/vmhook.hpp:11486-11500` — implicit conversion operator firing the noexcept read path
- `field_proxy::value_t::to_vector<T>()` — `vmhook/ext/vmhook/vmhook.hpp:11505-11515` — different function — object-array path returning vector<unique_ptr<T>>; log error on primitive
- `set_bool_array(field_id, values)` — `vmhook/ext/vmhook/vmhook.hpp:15339-15360` — write path; silently no-ops on null backing array
- `set_prim_array(field_id, values)` — `vmhook/ext/vmhook/vmhook.hpp:15371-15395` — write path; silently no-ops on null backing array
- `set_str_array(field_id, values)` — `vmhook/ext/vmhook/vmhook.hpp:15417-15440` — write path; silently no-ops on null backing array

## Tests

- `tests/jvm/modules/field_arrays_primitive.cpp`

## Known bugs

- **[high]** read_array_value violates noexcept on bogus _length (11376) — reserve(static_cast<size_t>(length)) throws std::bad_alloc after only length<=0 guard; torn/corrupted/racing _length (e.g. 0x40000001) or legitimately huge array kills JVM via std::terminate. Fix: clamp length to per-element byte ceiling and VMHOOK_LOG + return {} on overflow, or drop noexcept and catch.
- **[high]** No element-width validation — silent OOB / silent garbage (11360) — signature forwarded only to vector<char> overload; numeric reads ignore it. vector<int64_t> over [I (4-byte) array reads array_oop + 16 + N*8, walking past data. Index-only bounds check (11185) does not catch byte-extent overflow. Exercised crash-safe direction only: [J into vector<int32_t> reads interleaved low/high words {long0_low, long0_high, long1_low}.
- **[medium]** char[] -> vector<char> lossy narrowing (11317) — reads uint16 and static_cast<char> drops high byte; code units > 0xFF lose data silently. Verified: {0x61, 0x00FF, 0x0100, 0x20AC} becomes {0x61, 0xFF, 0x00, 0xAC}.
- **[medium]** array setters silently no-op on null backing array (15339/15371/15417) — all if (!array_oop) return; with no log, diverging from v0.4.4 scalar set stance. Also silent truncation when values.size() disagrees with Java length (min(...) at 15348/15380/15426).

## Notes

Compressed OOPs: read assumes 32-bit compressed array references (decode_array_oop); with -XX:-UseCompressedOops
or >32 GB heap the field stores full 64-bit OOP and uint32 variant would be wrong. Array header size: +12 length / +16 data
offsets are standard compressed-OOP HotSpot array layout on JDK 8..25 with default flags, not guaranteed under
-XX:ObjectAlignmentInBytes tweaks or non-HotSpot VM. char/byte storage: Java char[] always 16-bit (no compact-string effect).
Fixture source is pure ASCII so it compiles under any javac -encoding on 8..25 matrix.
