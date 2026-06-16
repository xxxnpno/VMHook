---
slug: field_proxy_value_t
title: Field Proxy Value T
category: field
status: in_progress
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/in_progress, risk/medium, category/field, tag/variant, tag/conversion, tag/reference, tag/OOP, tag/null-safety, tag/compression, tag/descriptor-driven, tag/test-pure-logic]
---

# Field Proxy Value T

> **Category:** [[categories/field|Field proxies (get / set / introspection)]]  ·  **Status:** `in_progress`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/field_proxy_value_t-specialist.md`

## Description

The typed return value of `field_proxy::get()` — a variant-backed box holding exactly one
JVM-primitive alternative (bool, int8_t, int16_t, int32_t, int64_t, float, double, uint16_t)
or a raw compressed OOP for reference/array fields (uint32_t). Carries the field's JVM descriptor
string alongside and implicitly converts to any C++ target type via templated `operator target_type()`,
dispatching through `cast_for_variant` to handle numerics, bool, `std::string`, `void*`,
`std::vector<T>`, and `std::unique_ptr<wrapper>`. The null-pointer contract returns int32 zero
for every signature without requiring a live JVM, enabling pure-logic testing of boundaries,
sign-extension, narrowing casts, and reference/OOP reject paths.

## Depends on

- [[features/field_introspection|field_introspection]]
- [[features/decode_oop_and_pointers|decode_oop_and_pointers]]

## Related

- [[features/field_inherited|field_inherited]]
- [[features/field_null_safety|field_null_safety]]
- [[features/field_primitives_get|field_primitives_get]]
- [[features/field_static|field_static]]

## Depended on by

- [[features/field_null_safety|field_null_safety]]
- [[features/field_primitives_get|field_primitives_get]]
- [[features/field_primitives_set|field_primitives_set]]
- [[features/field_proxy_set_guards|field_proxy_set_guards]]
- [[features/field_set_size_guard|field_set_size_guard]]

## Implementation anchors

- `field_proxy::value_t struct` — `vmhook/ext/vmhook/vmhook.hpp:11658-11962` — variant body + operators + signature carrier
- `value_t data variant` — `vmhook/ext/vmhook/vmhook.hpp:11660-11670` — 9-alternative variant: bool(0), int8(1), int16(2), int32(3), int64(4), float(5), double(6), uint16(7), uint32(8) — uint32 is raw compressed OOP
- `value_t signature field` — `vmhook/ext/vmhook/vmhook.hpp:11671-11671` — JVM descriptor string (Z/B/S/I/J/F/D/C/L.../[...)
- `cast_for_variant<target, source>` — `vmhook/ext/vmhook/vmhook.hpp:11792-11870` — conversion dispatch: std::string / std::vector / std::unique_ptr / void* / numeric fallback
- `cast_for_variant string arm` — `vmhook/ext/vmhook/vmhook.hpp:11799-11809` — read_java_string(decode_oop_pointer(value)) only from uint32 alt, else {}
- `cast_for_variant vector arm` — `vmhook/ext/vmhook/vmhook.hpp:11810-11820` — read_array_value only from uint32 alt, else {}
- `cast_for_variant unique_ptr arm` — `vmhook/ext/vmhook/vmhook.hpp:11821-11849` — decode + validate + new wrapper; FLAW B fix: rejects non-L sigs at 11833-11836 → nullptr
- `cast_for_variant void* arm` — `vmhook/ext/vmhook/vmhook.hpp:11850-11861` — decode_oop_pointer(value) only from uint32 alt, else nullptr
- `cast_for_variant numeric arm` — `vmhook/ext/vmhook/vmhook.hpp:11862-11869` — static_cast<target_type>(value) with requires guard; silent narrowing on cross-alt casts
- `operator target_type()` — `vmhook/ext/vmhook/vmhook.hpp:11886-11894` — std::visit dispatch, noexcept, runtime alternative → source_type
- `as_string()` — `vmhook/ext/vmhook/vmhook.hpp:11910-11925` — uint32 alt → read_java_string(decode_oop_pointer(v)); other alts → ""; avoids MSVC ambiguity
- `as_string() docstring` — `vmhook/ext/vmhook/vmhook.hpp:11896-11909` — load-bearing: explains why unambiguous extractor exists despite implicit operator
- `is_reference() on value_t` — `vmhook/ext/vmhook/vmhook.hpp:11931-11934` — std::holds_alternative<uint32_t>(data) — alternative-based, differs from descriptor-based
- `to_vector / to_entries` — `vmhook/ext/vmhook/vmhook.hpp:11945-11961` — declared in-class; definitions at 15639 / 15697; JVM-only (walk array/map OOPs)
- `read_array_value and helpers` — `vmhook/ext/vmhook/vmhook.hpp:11679-11771` — append_array_value overloads + read_array_value; JVM-only (get_array_element / decode_array_oop / array_length)
- `field_proxy::get()` — `vmhook/ext/vmhook/vmhook.hpp:11988-12049` — only producer of value_t; null-pointer guard; descriptor → alt dispatch
- `get() null-pointer guard` — `vmhook/ext/vmhook/vmhook.hpp:11991-11994` — null field_pointer → value_t{int32{}, sig} for every signature; no JVM needed
- `get() Z descriptor` — `vmhook/ext/vmhook/vmhook.hpp:11996-12001` — bool alt (index 0); memcpy 1 byte — UB if backing byte non-canonical
- `get() B descriptor` — `vmhook/ext/vmhook/vmhook.hpp:12002-12007` — int8 alt (index 1); memcpy 1 byte
- `get() S descriptor` — `vmhook/ext/vmhook/vmhook.hpp:12008-12013` — int16 alt (index 2); memcpy 2 bytes
- `get() I descriptor` — `vmhook/ext/vmhook/vmhook.hpp:12014-12019` — int32 alt (index 3); memcpy 4 bytes
- `get() J descriptor` — `vmhook/ext/vmhook/vmhook.hpp:12020-12025` — int64 alt (index 4); memcpy 8 bytes
- `get() F descriptor` — `vmhook/ext/vmhook/vmhook.hpp:12026-12031` — float alt (index 5); memcpy 4 bytes — bit-exact
- `get() D descriptor` — `vmhook/ext/vmhook/vmhook.hpp:12032-12037` — double alt (index 6); memcpy 8 bytes — bit-exact
- `get() C descriptor` — `vmhook/ext/vmhook/vmhook.hpp:12038-12043` — uint16 alt (index 7); memcpy 2 bytes — Java char is 0x0000..0xFFFF
- `get() L/[ fallthrough` — `vmhook/ext/vmhook/vmhook.hpp:12045-12048` — uint32 alt (index 8); memcpy 4 bytes — raw compressed OOP
- `field_proxy class` — `vmhook/ext/vmhook/vmhook.hpp:11635-11635` — proxy wrapper holding field_pointer + signature_text
- `field_proxy signature()` — `vmhook/ext/vmhook/vmhook.hpp:12199-12203` — string_view over signature_text
- `field_proxy raw_address()` — `vmhook/ext/vmhook/vmhook.hpp:12213-12216` — echoes ctor pointer, noexcept
- `field_proxy is_static()` — `vmhook/ext/vmhook/vmhook.hpp:12227-12231` — echoes ctor flag
- `field_proxy::is_reference()` — `vmhook/ext/vmhook/vmhook.hpp:12245-12254` — descriptor-based (front()=='L'||'['); false on empty — differs from value_t::is_reference()
- `field_proxy get_compressed_oop()` — `vmhook/ext/vmhook/vmhook.hpp:12260-12277` — FLAW C fix: returns 0 unless field_pointer non-null AND is_reference(); else memcpy first 4 bytes
- `field_proxy ctor` — `vmhook/ext/vmhook/vmhook.hpp:11971-11976` — captures alternative + signature; foundation for get()
- `hotspot::decode_oop_pointer()` — `vmhook/ext/vmhook/vmhook.hpp:4288-4352` — OOP → void* engine; zero-OOP early return (4291-4294) makes pure-logic test viable
- `decode_oop_pointer zero guard` — `vmhook/ext/vmhook/vmhook.hpp:4291-4294` — decode_oop_pointer(0) == nullptr before VMStruct access
- `decode_oop_pointer VMStruct walk` — `vmhook/ext/vmhook/vmhook.hpp:4304-4338` — iterate_struct_entries for non-zero OOP; JVM-only
- `read_java_string()` — `vmhook/ext/vmhook/vmhook.hpp:15726-15731` — nullptr → {}; safe for pure-logic zero-OOP testing

## Tests

- `tests/test_field_proxy_value_conversions.cpp`

## Known bugs

- **[medium]** Z (bool) get() at vmhook.hpp:11998-12000 — memcpy non-canonical backing byte (anything other than 0x00/0x01) into bool, creating trap representation. Every subsequent static_cast<bool> / std::get<bool> is UB. Test deliberately covers only canonical 0x00/0x01 (lines 122-135). Robust fix would normalize via 'value = (raw != 0)'.
- **[low]** Divergent is_reference() semantics at vmhook.hpp:11931-11934 vs 12245-12254 — value_t::is_reference() answers from variant alternative (holds_alternative<uint32_t>), while field_proxy::is_reference() answers from descriptor (front()=='L'||'['). Null-pointer proxy of reference field disagrees: value.is_reference()==false while proxy.is_reference()==true. Correctness trap but not memory-unsafe. Test never cross-checks the two.
- **[low]** Cross-alternative numeric narrowing at vmhook.hpp:11862-11865 — static_cast<target_type>(value) silently truncates on implicit conversion. int64 MAX → int32, double → int, negative signed → unsigned wraps (test pins B(-1) → uint32==0xFFFFFFFF, lines 169-173). By design but undocumented at value_t level; not fixable without breaking implicit-conversion ergonomics.
- **[low]** Positional brace-aggregate initialization at vmhook.hpp throughout get() (value_t{value, sig}) is fragile against struct/variant reordering — no explicit constructor to localize the index contract. Adding any member before data or reordering variant alternatives silently repoints both the index (hard-coded as 0..8 in test lines 38-49) and every call site. Latent maintenance hazard, not a live bug.

## Notes

Compressed-OOP decode VMStruct names evolve per JDK: 8-16 use `Universe::_narrow_oop._base/_shift`;
17-24 use `CompressedOops::_narrow_oop._base/_shift`; 25+ use `CompressedOops::_base/_shift` (no `_narrow_oop.`).
Every non-zero OOP conversion depends on resolving one of these — a JDK match failure returns nullptr silently.
The zero-OOP path (pure-logic test) is version-independent. Compact Strings (JDK 9+, JEP 254) means `as_string()`
and `to_vector()` must test both LATIN1 (coder=0) and UTF16 (coder=1) backing to catch encoding regressions.
Compressed OOPs only active under ~32 GB heaps; `-XX:-UseCompressedOops` truncates every reference field
to 4 bytes (garbage on 64-bit). Char array data offset constant (12 bytes) assumes 32-bit compressed-header layout.
