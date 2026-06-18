---
slug: read_java_string
title: Read Java String
category: jni
status: seeded
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/medium, category/jni, tag/jni, tag/string, tag/heap, tag/safety]
---

# Read Java String

> **Category:** [[categories/jni|JNI plumbing (arg packing / refs / java values)]]  ·  **Status:** `seeded`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/read_java_string-specialist.md`

## Description

vmhook::read_java_string(string_oop) — decodes a live java.lang.String oop into
a C++ std::string.  Resolves the String klass and its `value` field, reads the
compressed backing-array oop through os::safe_read (never a raw memcpy, since a
GC-relocated String still passes is_valid_pointer but may point at an unmapped
page), then decodes the array per JDK layout: JDK 8 char[] (UTF-16), or JDK 9+
compact strings (LATIN1 single-byte or UTF16 two-byte, selected by the `coder`
field).  The backing-array length is validated against read_java_string_max_units
(16 Mi chars) before any allocation or body read, so a corrupt length can neither
drive an unbounded allocation nor walk past the array.  A null/invalid oop or a
failed klass/field resolution returns an empty string.

## Depends on

- [[features/compressed_oops_decode|compressed_oops_decode]]
- [[features/os_safe_read|os_safe_read]]

## Related

- [[features/make_java_string|make_java_string]]
- [[features/field_string|field_string]]
- [[features/klass_introspection|klass_introspection]]

## Depended on by

- [[features/field_string|field_string]]
- [[features/make_java_string|make_java_string]]
- [[features/method_call_string|method_call_string]]
- [[features/method_proxy_value_t|method_proxy_value_t]]
- [[features/method_return_types|method_return_types]]

## Implementation anchors

- `vmhook::read_java_string` — `vmhook/ext/vmhook/vmhook.hpp:20098-20349` — JDK 8 char[] vs JDK 9+ LATIN1/UTF16 (coder) decode; safe_read value oop
- `vmhook::read_java_string_max_units` — `vmhook/ext/vmhook/vmhook.hpp:1687-1700` — 16 Mi-char ceiling validated before allocation (robustness bug #29)

## Tests

- `tests/jvm/modules/read_java_string.cpp`

## Notes

Ceiling raised from the old hard 4096-char cap that silently decoded longer
Strings to "" (robustness bug #29).  Requires a live JVM (reads a real String
oop); no no-JVM test file.
