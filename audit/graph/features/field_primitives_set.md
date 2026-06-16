---
slug: field_primitives_set
title: Field Primitives Set
category: field
status: in_progress
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/in_progress, risk/medium, category/field, tag/primitives, tag/field-write, tag/set, tag/boundaries, tag/float-exact, tag/instance-static]
---

# Field Primitives Set

> **Category:** [[categories/field|Field proxies (get / set / introspection)]]  ·  **Status:** `in_progress`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/field_primitives_set-specialist.md`

## Description

Native field write (`field_proxy::set()`) for all JVM primitives (Z/B/S/C/I/J/F/D) — the write-side
mirror of `field_primitives_get`. Accepts width-matched primitive values and delegates string/vector/unique_ptr
to specialized arms. Runs exhaustive boundary tests on real HotSpot JVMs (Java 8-26) with three
independent "Java-observed" channels: bytecode snapshot witnesses, compare-in-Java, and bytecode getters.
Covers static AND instance dispatch, bit-exact float/double writes (including sNaN/payload), the "C"
1-byte-char widening shortcut, null-pointer no-op, and repeatability.

## Depends on

- [[features/field_proxy_value_t|field_proxy_value_t]]
- [[features/field_introspection|field_introspection]]
- [[features/field_primitives_get|field_primitives_get]]

## Related

- [[features/field_primitives_get|field_primitives_get]]
- [[features/field_set_size_guard|field_set_size_guard]]
- [[features/field_proxy_set_guards|field_proxy_set_guards]]

## Implementation anchors

- `field_proxy::set<value_type>()` — `vmhook/ext/vmhook/vmhook.hpp:12059-12194` — main dispatch on C++ value type; non-primitive guard, string/vector/unique_ptr arms, trivially-copyable primitive path with size guard and 'C' widening shortcut
- `jvm_primitive_byte_width` — `vmhook/ext/vmhook/vmhook.hpp:12359-12374` — oracle for primitive widths; consulted by both guards and primitive memcpy
- `field_proxy::get<return_type>()` — `vmhook/ext/vmhook/vmhook.hpp:11988-12049` — read-side inverse; module round-trips every set() through get() to prove correctness

## Tests

- `tests/jvm/modules/field_primitives_set.cpp`

## Known bugs

- **[high]** set() has size guard but NO type guard (12167-12180) — same-width, wrong-kind values pass (e.g. set(float) into int reinterprets IEEE-754 bits verbatim). Characterised (not fixed) by field_set_size_guard module; field_primitives_set only writes width-matched primitives and pins width-matched writes are never refused.
- **[high]** 'C' widening shortcut fires for ANY 1-byte trivially-copyable, not just char (12148). set(int8_t{-1}) into 'C' lands as 0x00FF (zero-extended), not 0xFFFF. Documented char path tested; int8_t/uint8_t characterisation owned by field_set_size_guard.
- **[medium]** unique_ptr<wrapper> arm writes exactly 4 bytes unconditionally (12132-12135); under -XX:-UseCompressedOops, reference fields are 8 bytes and the high 4 bytes stay stale. Out of scope for primitive set (lives in same set()), CI never exercises (default-compressed-OOPs).
- **[medium]** 'Z' write width is sizeof(bool), not hard 1 (12167). On ABI where sizeof(bool) > 1, set(bool) into 'Z' has value_size != 1 == field_size and size guard refuses write. Unobservable on CI (sizeof(bool)==1 on x64 MSVC/GCC/Clang), real per standard.
- **[medium]** set() returns void (12060) — rejected writes (size mismatch, non-primitive, null) give no programmatic signal, only log line. Documented gap vs method_proxy::call(). Module works around by re-reading through get() and JVM bytecode.
- **[low]** Doc drift: set() doc-comment (12051-12058) says 'JVM primitives, std::string ... std::vector<T>', omits const char*/string_view/unique_ptr arms that code actually has; static_assert message (12186-12192) lists different set again.

## Notes

NaN canonicalisation sensitivity: HotSpot canonicalises Float.NaN; signaling-NaN / payload survive native
memcpy round-trip (12059-12194 primitive path) on JDK 8..25, but x87 FPU register loading canonicalises.
Module routes sNaN/payload ONLY through native memcpy + value-matrix field's canonical snapshot, never
through Java bytecode. Java channels capture F/D as RAW bits (floatToRawIntBits/doubleToRawLongBits);
value-class predicates (isNaN/isInfinite/signbit) are universal hard invariants independent of
canonicalisation. Char encoding: pure ASCII literals, fixture compiles identically on Windows/Linux/macOS.
C++17 module: memcpy type-pun (no std::bit_cast), clean at -Wall -Wextra, zero new warnings. Java 8
source: no var/records/post-8 APIs, verified under javac -source 8 -target 8.
