---
slug: field_proxy_set_guards
title: Field Proxy Set Guards
category: field
status: in_progress
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/in_progress, risk/medium, category/field, tag/field, tag/safety, tag/guard, tag/type-check, tag/memory-safety]
---

# Field Proxy Set Guards

> **Category:** [[categories/field|Field proxies (get / set / introspection)]]  ·  **Status:** `in_progress`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/field_proxy_set_guards-specialist.md`

## Description

The two runtime safety nets inside `vmhook::field_proxy::set<T>()` that stop a mistyped C++ write from corrupting a Java object.
The non-primitive guard refuses `std::string` / `std::string_view` / `const char*` / `std::vector<T>` / `std::unique_ptr` writes into a primitive field.
The size-mismatch guard in the trivially-copyable arm refuses writes whose `sizeof(value)` differs from the field's JVM width.
Both consult `detail::jvm_primitive_byte_width` and run entirely on raw `field_pointer`, so the pure-logic test exercises them over a stack buffer with sentinel bytes, never touching a JVM.

## Depends on

- [[features/field_set_size_guard|field_set_size_guard]]
- [[features/field_proxy_value_t|field_proxy_value_t]]

## Related

- [[features/field_primitives_get|field_primitives_get]]
- [[features/field_primitives_set|field_primitives_set]]
- [[features/field_set_size_guard|field_set_size_guard]]

## Implementation anchors

- `field_proxy::set<value_type>(const value_type&)` — `vmhook/ext/vmhook/vmhook.hpp:12059-12194` — the only field setter — branch order: non-primitive guard (12075-12093), string arm (12095-12102), vector arm (12103-12117), unique_ptr arm (12118-12137), trivially-copyable arm (12138-12183), static_assert fallback (12184-12192)
- `clean_value_type` — `vmhook/ext/vmhook/vmhook.hpp:12063-12063` — std::remove_cvref_t<value_type> — strips cv/ref before every trait test
- `Non-primitive guard` — `vmhook/ext/vmhook/vmhook.hpp:12075-12093` — if constexpr on std::string OR (std::is_convertible_v<value_type, std::string_view> and not std::string) OR is_vector_v OR is_unique_ptr_v; fires when jvm_primitive_byte_width(signature_text) != 0 — VMHOOK_LOG + return
- `Size-mismatch guard` — `vmhook/ext/vmhook/vmhook.hpp:12167-12180` — field_size = jvm_primitive_byte_width(signature_text); if field_size != 0 && value_size != field_size, VMHOOK_LOG + return (no memcpy)
- `"C" 1-byte widening shortcut` — `vmhook/ext/vmhook/vmhook.hpp:12148-12153` — if signature_text == "C" and sizeof(clean_value_type) == sizeof(char), widens via static_cast<std::uint16_t>(static_cast<unsigned char>(value)) and writes 2 bytes — zero-extension, never sign-extension
- `detail::jvm_primitive_byte_width` — `vmhook/ext/vmhook/vmhook.hpp:12359-12374` — width oracle both guards consult — returns 0 unless signature.size() == 1; then Z/B=1, S/C=2, I/F=4, J/D=8, default 0. The .size() != 1 early-out makes "II", "[I", "Ljava/lang/String;", "" all return 0
- `detail::is_vector_v` — `vmhook/ext/vmhook/vmhook.hpp:1530-1554` — trait helper for detecting std::vector<T>, includes remove_cvref_t strip
- `detail::is_unique_ptr_v` — `vmhook/ext/vmhook/vmhook.hpp:1566-1592` — trait helper for detecting std::unique_ptr<T>, includes remove_cvref_t strip
- `field_proxy ctor` — `vmhook/ext/vmhook/vmhook.hpp:11971-11976` — field_proxy(void*, std::string, bool) — takes raw void* for test stack-buffer construction
- `field_proxy private members` — `vmhook/ext/vmhook/vmhook.hpp:12280-12282` — field_pointer / signature_text / static_field
- `set_str_field (unguarded danger path)` — `vmhook/ext/vmhook/vmhook.hpp:15957-15961` — set_str_field -> field_oop (15870-15874) -> decode_array_oop (reinterprets field as compressed OOP) -> write_java_string (15893-15942) reads *string_oop. Non-primitive guard blocks this entirely

## Tests

- `tests/test_field_proxy_set_guards.cpp`

## Known bugs

- **[medium]** The size guard is a SIZE guard, not a TYPE guard — same-width wrong-KIND writes silently reinterpret bits (vmhook.hpp:12167-12182). A set(float{1.5f}) into an "I" field, set(std::int32_t) into "F", set(double) into "J", or set(std::int64_t) into "D" all have matching widths, pass the guard, and memcpy the raw IEEE-754 / two's-complement bit pattern verbatim. No diagnostic. Documented and characterised by the sibling JVM module (field_set_size_guard.cpp phase 6), not fixed — a future signature-aware type check would reject these. The pure-logic test currently does NOT pin these same-width confusions.
- **[low]** signed char / plain char sign-extension is invisible to the width oracle for non-"C" 1-byte fields (vmhook.hpp:12148-12153 vs. 12182). The widening shortcut only fires for signature_text == "C". A set(std::int8_t{-1}) into a "B" field passes the size guard (1 == 1) and memcpy's the single byte 0xFF — correct for a byte field, but the shortcut's unsigned-cast semantics are a silent contract. Not a corruption bug, but an asymmetry worth a regression pin.
- **[low]** const char* reaches the string arm only via implicit string_view conversion, and a char[N] array decays the same way — but a std::array<char,N> or other 'stringy' container does NOT (vmhook.hpp:12076, 12099). A std::array<char,4> is NOT string_view-convertible and falls through to the trivially-copyable arm, treated as a 4-byte primitive blob. For an "I" field the array's 4 bytes are memcpy'd as an int (size matches) rather than refused. Low severity (callers rarely do this) but a real edge of the guard's type set.
- **[low]** Guard predicate mixes value_type and clean_value_type (vmhook.hpp:12075-12078). The string_view-convertibility sub-clause tests value_type (raw, possibly-ref type) while vector/unique_ptr sub-clauses test clean_value_type. In practice is_convertible_v is ref-tolerant so this is benign today, but the asymmetry is a latent hazard if someone later swaps the predicate to a trait that is NOT ref-tolerant. The test should pin a const std::string& lvalue path to lock current behaviour.

## Notes

The guards themselves are JDK-independent — both run on the in-memory signature_text + a raw pointer; jvm_primitive_byte_width only inspects the descriptor string.
There is no HotSpot struct access on the rejection paths, so the pure-logic test is identical on every JDK and on every host with no JVM at all.
The danger the non-primitive guard blocks IS JDK-shaped (relevant only to the success/JVM side): set_str_field -> write_java_string branches on backing array signature — [C (JDK 8 char[], writes uint16 per char) vs. else byte-array path (JDK 9+ compact-strings byte[]/LATIN1, writes uint8).
If the guard ever regressed, the corruption a primitive field would suffer differs by JDK.
The sizeof(wchar_t) host-platform split (Windows 2 / Linux-Mac 4) is the only place this feature's behaviour (accept vs refuse a wchar_t into "C") changes across the CI matrix — it is a C++ ABI difference, not a JDK one.
