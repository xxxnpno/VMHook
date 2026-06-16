---
slug: field_null_safety
title: Field Null Safety
category: field
status: in_progress
risk: low
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/in_progress, risk/low, category/field, tag/field, tag/null-safety, tag/robustness, tag/lookup, tag/degenerate-input]
---

# Field Null Safety

> **Category:** [[categories/field|Field proxies (get / set / introspection)]]  ·  **Status:** `in_progress`  ·  **Risk:** `low`  ·  **Specialist:** `.claude/agents/field_null_safety-specialist.md`

## Description

Read-side / lookup-side robustness of the field surface under degenerate inputs:
`field_proxy::get()` / `set()` on a NULL field_pointer, `object_base` accessors
on absent/empty/garbage field names, wrappers built from NULL oops, and deliberately-wrong
signatures over real field pointers. The absolute contract is: never crash, always
return the documented fallback (int32 zero for null field_proxy::get; std::nullopt for
absent lookups; guarded no-op for null field_proxy::set), and never corrupt valid state.

## Depends on

- [[features/field_introspection|field_introspection]]
- [[features/field_proxy_value_t|field_proxy_value_t]]
- [[features/find_class_fallback|find_class_fallback]]

## Related

- [[features/field_inherited|field_inherited]]
- [[features/field_primitives_get|field_primitives_get]]
- [[features/field_static|field_static]]

## Implementation anchors

- `field_proxy::get()` — `vmhook/ext/vmhook/vmhook.hpp:15217-15350` — null-pointer guard at 15239-15241 (returns int32_t{} + signature); valid-pointer gate + sub-word alignment carve-out at 15272-15277
- `field_proxy::set()` — `vmhook/ext/vmhook/vmhook.hpp:15366-15600` — trivially-copyable branch guards at 15380; unique_ptr branch at ~15465; string branch routed through get_compressed_oop (returns 0 when !field_pointer)
- `find_field()` — `vmhook/ext/vmhook/vmhook.hpp:13763-13812` — null-klass check at 13766-13770; returns std::nullopt for absent names; caches only FOUND entries at 13805
- `object_base::get_field(const std::string_view name)` — `vmhook/ext/vmhook/vmhook.hpp:17716-17763` — instance-field null-oop guard at 17755-17759; static-field reads mirror independently at 17742-17752; routes through find_field which handles absent names
- `object_base::get_field(type_index, name) static` — `vmhook/ext/vmhook/vmhook.hpp:17780-17822` — static-only variant; does not require live OOP; reads mirror at 17811-17821; routes through find_field for absent-name safety
- `field_proxy value_t::signature / raw_address / is_static / is_reference / get_compressed_oop` — `vmhook/ext/vmhook/vmhook.hpp:15199-15216` — pointer-independent accessors that never deref field_pointer; well-defined on null proxy

## Tests

- `tests/jvm/modules/field_null_safety.cpp`

## Known bugs

- **[low]** Descriptor-blind null fallback (documented-by-design, pinned NOT fixed): field_proxy::get() on a null field_pointer returns int32_t{} alternative (variant index 3, value 0) for EVERY signature — null "D"/"J"/"F"/"Ljava/lang/String;" proxy reports int32 alternative, not double/long/float/reference. Caller that std::get the expected alternative by descriptor may be surprised. Severity: low (graceful, unintuitive). Recorded in lib_bugs. Line 15241, pinned across all 16 descriptors × both static/instance flags.

## Notes

JDK-INVARIANT by construction: tests the library's own C++ guard logic (null checks,
name-compare misses, mirror-vs-instance split), none of which depend on HotSpot field-layout
differences across JDK 8/11/17/21/24/25/26. okInt storage is the Class mirror slot on every
version; find_field walks InstanceKlass metadata uniformly. Every assertion is a hard ctx.check —
there is nothing to JDK-gate. String backing array differences (Java 8 char[] vs Java 9+ byte[]/coder)
are deliberately avoided; String checks only use as_string() (already JDK-version-handled by library)
and null/empty-string paths (never touch backing array).
