---
slug: field_object_ref
title: Field Object Ref
category: field
status: in_progress
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/in_progress, risk/medium, category/field, tag/field, tag/reference, tag/object, tag/unique_ptr, tag/compressed-oop, tag/oop-decode, tag/wrapper, tag/self-ref, tag/shared-ref]
---

# Field Object Ref

> **Category:** [[categories/field|Field proxies (get / set / introspection)]]  ·  **Status:** `in_progress`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/field_object_ref-specialist.md`

## Description

Object-reference field access: `field_proxy::get()` on a field whose JVM descriptor starts with `'L'`,
decoded straight into a `std::unique_ptr<wrapper>`. The get() reference fallthrough reads a 4-byte
compressed OOP from the object slot (instance: oop+offset; static: re-resolved through the GC-stable
mirror_klass) and stores it as the variant's uint32 alternative; `value_t::cast_for_variant` then
decodes that OOP via `decode_oop_pointer`, validates it with `is_valid_pointer`, and wraps it in a new
`wrapper_type`. A NULL slot must never fabricate a wrapper (yields an empty unique_ptr). Because the
field path reads a real compressed OOP directly from the slot (unlike the method-return twin, which
truncates a JNI handle on JDK 21+), "non-null ref -> usable wrapper" holds on every supported JDK,
making this the JDK-independent proof of the whole OOP-decode pipeline.

## Depends on

- [[features/wrapper_pattern|wrapper_pattern]]
- [[features/compressed_oops_decode|compressed_oops_decode]]

## Related

- [[features/field_arrays_object|field_arrays_object]]
- [[features/field_string|field_string]]
- [[features/field_introspection|field_introspection]]

## Depended on by

- [[features/field_arrays_object|field_arrays_object]]
- [[features/field_string|field_string]]

## Implementation anchors

- `field_proxy::get() reference/array fallthrough` — `vmhook/ext/vmhook/vmhook.hpp:15439-15442` — after the eight primitive descriptor branches, any L.../[... descriptor reads exactly 4 bytes via os::safe_read_fast and stores the raw compressed OOP as the value_t uint32 alternative
- `field_proxy::get() (GC-stable static re-resolve)` — `vmhook/ext/vmhook/vmhook.hpp:15316-15443` — read_pointer = field_pointer; for statics re-resolves through mirror_klass->get_java_mirror() so a relocated class mirror never reads a stale slot; instance fast path byte-identical
- `field_proxy::value_t::cast_for_variant<unique_ptr<W>>` — `vmhook/ext/vmhook/vmhook.hpp:15113-15133` — is_unique_ptr_v<target_type> arm — decode_oop_pointer(value), reject when !decoded || !is_valid_pointer (empty unique_ptr), else new wrapper_type{ decoded }; FLAW B: no signature-shape check, a '[' field decoded as unique_ptr<W> is NOT rejected
- `field_proxy::get_compressed_oop()` — `vmhook/ext/vmhook/vmhook.hpp:15692-15760` — FLAW C fixed: guards on is_reference() (returns 0 on a primitive field) before reading 4 bytes; GC-stable static re-resolve + os::safe_read_fast; used for round-trip identity proof
- `hotspot::decode_oop_pointer(uint32_t) / encode_oop_pointer` — `vmhook/ext/vmhook/vmhook.hpp:5374-5460` — narrow-oop base+shift decode (zero guard at 5377-5380) and its inverse encode used for the re-encode(decode(x)) == x identity check
- `vmhook::field_oop(const field_proxy&)` — `vmhook/ext/vmhook/vmhook.hpp:20364-20368` — manual decode entry — decode_array_oop(field.get_compressed_oop()); operator void* must agree with this

## Tests

- `tests/jvm/modules/field_object_ref.cpp`

## Known bugs

- **[medium]** FLAW A — no wrapper-klass match check (cast_for_variant unique_ptr arm, vmhook.hpp:15113-15133): a Ref-typed slot read through an UNRELATED Decoy wrapper is NOT rejected. decode_oop_pointer + is_valid_pointer only prove the OOP is a plausible heap pointer, not that it is an instance of the requested wrapper's klass — the decoy's field offsets then read garbage off a real-but-wrong object. Surfaced as [INFO] and asserted against actual behaviour.
- **[medium]** FLAW B — no signature-shape check (vmhook.hpp:15113-15133): a '[' (reference-array) field read as unique_ptr<W> is NOT rejected; the wrapper ends up pointing at the array oop rather than an element. The arm only branches on stored_type == uint32 and the target being a unique_ptr, never inspecting signature_text[0].
- **[low]** FLAW C (FIXED, pinned as regression) — get_compressed_oop() now guards on is_reference() (vmhook.hpp:15703-15705) so a primitive 'I' field returns 0 instead of the int's first 4 bytes mis-decoded as a wild OOP. Pinned so a future regression of the guard trips a test.

## Notes

JDK-independent proof: the field reference path reads a real compressed OOP straight from the slot, so a
non-null ref yields a usable wrapper on EVERY JDK (8..26) — unlike the method-return twin which
truncates/frees a JNI handle on JDK 21+. Compressed-OOP layout is assumed (HotSpot default sub-32 GB
heap); -XX:-UseCompressedOops makes reference slots 8 bytes and the 4-byte uint32 read would be wrong
(out-of-test hazard shared with field_string / field_arrays_object). Static-field reads re-resolve the
GC-stable mirror at read time; instance reads use oop+offset. Module proofs: non-null ref yields a usable
wrapper (read int/String/nested-ref fields AND dispatch a method through it); null ref (instance + static)
-> empty unique_ptr; self-ref decodes to the receiver; shared ref -> same heap address; get_compressed_oop()
round-trips re-encode(decode(x)) == x; operator void* agrees with field_oop(). Java-8-only fixture
(vmhook/fixtures/FieldObjectRef); MSVC copy-init (= proxy->get(), never brace-init); every decoded-OOP
deref gated by is_valid_pointer; never calls shutdown_hooks().
