---
slug: field_set_size_guard
title: Field Set Size Guard
category: field
status: in_progress
risk: low
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/in_progress, risk/low, category/field, tag/size-guard, tag/anti-clobber, tag/field, tag/safety, tag/primitives]
---

# Field Set Size Guard

> **Category:** [[categories/field|Field proxies (get / set / introspection)]]  ·  **Status:** `in_progress`  ·  **Risk:** `low`  ·  **Specialist:** `.claude/agents/field_set_size_guard-specialist.md`

## Description

Size/type guard + anti-clobber safety net for `field_proxy::set()` — the API
that writes C++ values into raw Java field storage. Every trivially-copyable
write is width-checked against `jvm_primitive_byte_width()` to prevent
too-wide C++ values from spilling into adjacent fields; non-primitives
(string/vector/unique_ptr) are rejected on primitive-field targets to block
reinterpretation as compressed OOPs. The char-widening shortcut auto-promotes
1-byte C++ char to 2-byte Java char ("C" field). ANTI-CLOBBER PROOF tracks
all three: sentinels Before/Target/After, verifies contiguity, and confirms
too-wide writes are refused.

## Depends on

- [[features/field_proxy_value_t|field_proxy_value_t]]

## Related

- [[features/field_primitives_get|field_primitives_get]]
- [[features/field_primitives_set|field_primitives_set]]
- [[features/field_proxy_set_guards|field_proxy_set_guards]]

## Depended on by

- [[features/field_proxy_set_guards|field_proxy_set_guards]]

## Implementation anchors

- `jvm_primitive_byte_width(std::string_view)` — `vmhook/ext/vmhook/vmhook.hpp:15890-15905` — maps JVM primitive signatures (Z/B/C/S/I/J/F/D) to byte widths; 0 for non-primitives
- `field_proxy::set(const value_type&) — non-primitive guard` — `vmhook/ext/vmhook/vmhook.hpp:15371-15399` — rejects string/vector/unique_ptr writes on primitive fields (fires if jvm_primitive_byte_width != 0)
- `field_proxy::set(const value_type&) — char widening shortcut` — `vmhook/ext/vmhook/vmhook.hpp:15460-15486` — auto-widens 1-byte C++ char to 2-byte uint16 for Java 'C' field (if constexpr guards non-arithmetic types)
- `field_proxy::set(const value_type&) — size-mismatch guard` — `vmhook/ext/vmhook/vmhook.hpp:15488-15516` — refuses trivially-copyable write if sizeof(value) != jvm_primitive_byte_width(sig); prevents clobber of adjacent fields

## Tests

- `tests/jvm/modules/field_set_size_guard.cpp`

## Notes

Stub manifest — no JDK sensitivities known at this time. Guard behavior is deterministic and does not vary across JVM versions (width mappings are part of the JVM spec).
