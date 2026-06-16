---
slug: array_element_helpers
title: Array Element Helpers
category: collection
status: in_progress
risk: high
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/in_progress, risk/high, category/collection, tag/array, tag/pointer-arithmetic, tag/primitive, tag/x86_64, tag/safety, tag/memory]
---

# Array Element Helpers

> **Category:** [[categories/collection|Collection wrappers + element helpers]]  ·  **Status:** `in_progress`  ·  **Risk:** `high`  ·  **Specialist:** `.claude/agents/array_element_helpers-specialist.md`

## Description

Three low-level raw pointer-arithmetic primitives for reading and writing a HotSpot
primitive-array body without a JVM call: `vmhook::array_length()`, `vmhook::get_array_element<T>()`,
and `vmhook::set_array_element<T>()`. They assume x64 compressed-OOP array layout
(mark + narrow-klass at +0/+8, `_length` at +12, `_data[0]` at +16) and validate
only the header pointer via `is_valid_pointer()`, not the element range. These are
the foundation for every array-reading consumer in the header (String backing arrays,
field readers/writers, HashMap bucket walkers, `read_array_value`).

## Related

- [[features/field_proxy_value_t|field_proxy_value_t]]
- [[features/decode_oop_and_pointers|decode_oop_and_pointers]]
- [[features/compressed_oops_decode|compressed_oops_decode]]
- [[features/make_java_string|make_java_string]]
- [[features/make_java_array|make_java_array]]

## Depended on by

- [[features/collection_list|collection_list]]
- [[features/field_arrays_object|field_arrays_object]]
- [[features/field_arrays_primitive|field_arrays_primitive]]
- [[features/field_string|field_string]]

## Implementation anchors

- `vmhook::array_length` — `vmhook/ext/vmhook/vmhook.hpp:11542-11551` — inline static noexcept; reads int32 _length at +12; null/is_valid_pointer check → 0
- `vmhook::get_array_element<element_type>` — `vmhook/ext/vmhook/vmhook.hpp:11563-11581` — template static; memcpy from base+16+index*stride; null/is_valid_pointer/bounds guard → element_type{}
- `vmhook::set_array_element<element_type>` — `vmhook/ext/vmhook/vmhook.hpp:11590-11605` — template static void; memcpy to base+16+index*stride; null/is_valid_pointer/bounds guard → silent return
- `vmhook::hotspot::is_valid_pointer` — `vmhook/ext/vmhook/vmhook.hpp:1768-1805` — lightweight pointer validation: range [0xFFFF, 0x00007FFFFFFFFFFF], odd-address reject, 9 low-32 sentinels
- `field_proxy::value_t::append_array_value (read_array_value)` — `vmhook/ext/vmhook/vmhook.hpp:11679-11769` — higher-level consumer: dispatches on signature ([C vs [B), calls array_length to bound reserve()
- `write_java_string` — `vmhook/ext/vmhook/vmhook.hpp:15921-15941` — consumer: clamps write loop to min(array_length, value.size())

## Tests

- `tests/test_array_element_helpers.cpp`

## Known bugs

- **[high]** index * sizeof(element_type) offset computed in 32-bit (int32 * int32 multiply) overflows before addition; bounds check only verifies index < length, not index*stride in range (lines 11579, 11604). Fix: compute offset in std::size_t or std::ptrdiff_t.
- **[high]** array_length() reads _length with zero sanity bound (line 11550); a valid-looking but garbage _length value flows into consumer reserve() calls (11764), triggering unbounded allocation / std::bad_alloc → std::terminate on noexcept code (11747).
- **[medium]** is_valid_pointer checks header address only (11545/11568/11595), not element address touched by memcpy (11579/11604); element access can straddle unmapped pages and fault despite header being mapped (no OS page-state query, only range/alignment/sentinel).
- **[medium]** get/set_array_element are not noexcept (11564, 11591) but memcpy can SEH fault on Windows; callers (append_array_value, read_array_value at 11679-11769) advertise noexcept and cannot honour hard-fault contract.
- **[low]** No static_assert against pointer/reference element types (11567, 11594); trivially_copyable permits T*, but Java reference arrays store 4-byte compressed OOPs; reading as native 8-byte pointer width yields garbage (documented hazard, not defect in supported usage).

## Notes

Array layout offsets (+12 for `_length`, +16 for `_data[0]`) assume compressed-OOP x64
layout with `UseCompressedClassPointers` enabled (default JDK 8–26 under typical heap sizes).
With compressed pointers disabled, `_length` shifts to +16 and `_data` to +24 — all helpers
read wrong slots silently. JDK 8 String backing is `char[]` (signature `[C`, uint16 stride);
JDK 9+ is `byte[]` (signature `[B`, uint8 stride); consumers dispatch on signature. Compressed-OOP
reference-array decode (11694, 14821, 15279, 15670) assumes 4-byte element stride; under
`-XX:-UseCompressedOops` reference elements are 8 bytes and `get_array_element<uint32>` reads half a slot.
