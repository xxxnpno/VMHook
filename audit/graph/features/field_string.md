---
slug: field_string
title: Field String
category: field
status: in_progress
risk: high
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/in_progress, risk/high, category/field, tag/field, tag/string, tag/compact-strings, tag/utf-16, tag/coder, tag/array-mutation, tag/in-place]
---

# Field String

> **Category:** [[categories/field|Field proxies (get / set / introspection)]]  ·  **Status:** `in_progress`  ·  **Risk:** `high`  ·  **Specialist:** `.claude/agents/field_string-specialist.md`

## Description

Getting and setting `java.lang.String` fields across the zero-JNI boundary via
`vmhook::read_java_string` (GET decode: backing `byte[]`/`char[]` -> `std::string`)
and `vmhook::write_java_string` (SET write: `std::string` -> in-place mutation of
backing array). Input contract: GET collapses null/invalid/array-lookup-failure
into empty `std::string{}` with no diagnostic. SET mutates the existing backing
array in place (never allocates new String), truncates to array length, and leaves
old tail bytes on partial overwrites. Surprising bits: Non-ASCII on UTF-16 silently
becomes `'?'` (lossy); 4 KiB cap is a hard reject not truncation; UTF-16 effective
cap is 2048 chars; SET ignores coder (corrupts UTF-16 targets); empty String logs
spurious warning on every read.

## Depends on

- [[features/field_object_ref|field_object_ref]]
- [[features/read_java_string|read_java_string]]
- [[features/make_java_string|make_java_string]]
- [[features/array_element_helpers|array_element_helpers]]
- [[features/decode_oop_and_pointers|decode_oop_and_pointers]]

## Related

- [[features/field_arrays_object|field_arrays_object]]

## Implementation anchors

- `vmhook::read_java_string(void* string_oop)` — `vmhook/ext/vmhook/vmhook.hpp:15138-15220` — GET workhorse — null/oop guards, klass/value field resolution, array decode, branches on coder (JDK8 char[] / JDK9+ LATIN1 / UTF-16)
- `vmhook::write_java_string(void* string_oop, string_view value)` — `vmhook/ext/vmhook/vmhook.hpp:15256-15310` — SET write — array decode, min(array_length, input_length) truncation, branches only on array signature [C] vs other (JDK8 vs 9+), never reads coder
- `field_proxy::value_t::cast_for_variant<std::string>` — `vmhook/ext/vmhook/vmhook.hpp:11411-11425` — GET implicit conversion — calls read_java_string(decode_oop_and_pointers(value)) for string variant, returns {} for others
- `field_proxy::set(const value_type&) string/string_view arms` — `vmhook/ext/vmhook/vmhook.hpp:11655-11659` — SET entry — routes std::string/string_view to set_str_field, guarded by primitive type-check at 11635-11653
- `vmhook::set_str_field(field_proxy, value)` — `vmhook/ext/vmhook/vmhook.hpp:15320-15330` — SET field wrapper — unpacks field_oop and delegates to write_java_string

## Tests

- `tests/jvm/modules/field_string.cpp`

## Known bugs

- **[high]** Non-ASCII silently becomes '?' on UTF-16 path (data loss, no log) — vmhook.hpp:15213 (and JDK-8 char[] twin at 15196). Any code unit >= 0x80 on coder==1 String replaced by '?'. JDK 21 "日本語" -> "???"  and "A日BéC" -> "A?B?C". Inconsistent with LATIN1 arm (15204) which keeps bytes >= 0x80 verbatim. Trivial UTF-8 encode would round-trip cleanly. Module asserts the lossy results so a future fix deliberately flips them.
- **[medium]** 4 KiB cap is a hard REJECT, not truncation — contradicts docstring — vmhook.hpp:15178 (length <= 0 || length > 4096 -> {}) vs doc at 15435-ish ("Truncates strings longer than 4096 characters"). 4097-char ASCII String returns "", not first 4096 chars. Sibling make_java_string truncates with std::min — opposite behavior. Module: getLen4096 passes full string; getLen4097 and getLen5000 return "".
- **[medium]** UTF-16 effective cap is 2048 *chars*, not 4096 — vmhook.hpp:15178 tests byte count (read at +12) before coder branch divides by 2 (15209). UTF-16 String hits cap at 2048 chars while LATIN1/char[] reach 4096. Platform-conditional, silent. Module: getCjk2048 (2048 chars, byteLen 4096) passes; getCjk2049 (byteLen 4098) rejected -> "".
- **[medium]** write_java_string ignores coder -> corrupts UTF-16 targets and silently truncates/partial-overwrites — vmhook.hpp:15256-15304. Detects only [C] vs other (JDK 8 vs 9+), never runtime coder. Writing into coder==1 String overwrites only low bytes of UTF-16 units (garbled); multi-byte UTF-8 input becomes mojibake. All failure paths (null oop, missing klass, missing value field, null array, writable <= 0) return with no log, unlike loud make_java_string. Module exercises: ASCII into LATIN1 backing of equal length round-trips; shorter write leaves tail ("world" <- "hi" -> "hirld", length stays 5); overlong write truncates ("abc" <- "LONGER" -> "LON", length stays 3); write into zero-length backing is no-op.
- **[low]** Empty Java String logs a warning on every read — vmhook.hpp:15178-15183. length==0 funnelled into same warning_tag branch as corrupt header ("either an empty string or the array header is corrupt"). Hot loop over empty fields spams log. Module reads getEmpty and asserts "" (value correct; noise is defect).

## Notes

Compact strings (JEP 254, JDK 9+): entire GET/SET behavior hinges on coder field.
Test JVM (Temurin 21) always takes has_coder branch; JDK-8 char[] arm is dead code.
ASCII/Latin-1 store as coder=0; any cp > 0xFF promotes entire String to coder=1.
Compressed OOPs: value read as 32-bit compressed OOP. -XX:-UseCompressedOops or
>32 GB heap would break uint32 variant. Array header offsets +12 (length) / +16
(data) are standard compressed-OOP HotSpot layout, stable on 8..25 default flags.
Fixture encoding: non-ASCII content written as (char) 0x.. casts or literal char[],
never \u/raw bytes dependent on javac -encoding, compiles identically under any
locale on javac 8..25. CRITICAL: new String("literal") shares backing byte[] with
interned literal, so in-place write_java_string would corrupt every copy across
JVM. Every SET target in fixture built via new String(text.toCharArray()) or
literal char[], never new String(String), to prevent this catastrophic data loss.
