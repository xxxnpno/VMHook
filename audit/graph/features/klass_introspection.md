---
slug: klass_introspection
title: Klass Introspection
category: klass
status: seeded
risk: high
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/high, category/klass, tag/klass, tag/introspection, tag/method, tag/field, tag/descriptor, tag/vmstructs, tag/unsigned5, tag/x86_64]
---

# Klass Introspection

> **Category:** [[categories/klass|Class / Klass introspection]]  ·  **Status:** `seeded`  ·  **Risk:** `high`  ·  **Specialist:** `.claude/agents/klass_introspection-specialist.md`

## Description

Reads a loaded HotSpot `Klass`/`InstanceKlass` directly (no JNI, no JVMTI) to
enumerate its declared methods as `(name, JVM-descriptor)` pairs, select method
names by a stable descriptor, and walk the class shape — `_name`, `_super`, and
the `_methods`/`_fields` arrays — that every higher-level helper (`hook<T>`,
`scoped_hook`, `find_field`) is built on. The headline public surface is
`get_class_methods` (three overloads) + `find_methods_by_signature<T>` (an
exact-descriptor-equality selector returning ALL matches); the load-bearing
internals are `detail::collect_klass_methods` (the shared, noexcept,
try/catch-wrapped engine) and the `klass`/`method`/`symbol` accessors it
drives. Enumeration is DECLARED-only (inherited `Object`/superclass methods
absent, synthetic `<init>`/`<clinit>` present). The `find_field` leg
format-dispatches on JDK era: `_fields` Array<u2> (JDK 8-~20) vs
`_fieldinfo_stream` Array<u1>/UNSIGNED5 (JDK 21+).

## Depends on

- [[features/vmstructs_offset_resolution|vmstructs_offset_resolution]]
- [[features/instanceklass_methods_walk|instanceklass_methods_walk]]
- [[features/compressed_klass_decode|compressed_klass_decode]]

## Related

- [[features/compressed_klass_decode|compressed_klass_decode]]
- [[features/compressed_oops_decode|compressed_oops_decode]]
- [[features/constantpool_access|constantpool_access]]
- [[features/find_class_fallback|find_class_fallback]]
- [[features/instanceklass_methods_walk|instanceklass_methods_walk]]
- [[features/method_enumeration|method_enumeration]]
- [[features/find_methods_by_signature|find_methods_by_signature]]
- [[features/field_introspection|field_introspection]]

## Depended on by

- [[features/collection_hash_tree_map|collection_hash_tree_map]]
- [[features/collection_linked_list|collection_linked_list]]
- [[features/collection_list|collection_list]]
- [[features/collection_set|collection_set]]
- [[features/collection_type_tags|collection_type_tags]]
- [[features/decode_u5_unsigned5|decode_u5_unsigned5]]
- [[features/enum_singleton|enum_singleton]]
- [[features/field_inherited|field_inherited]]
- [[features/field_introspection|field_introspection]]
- [[features/find_class_context_loader|find_class_context_loader]]
- [[features/find_class_fallback|find_class_fallback]]
- [[features/find_methods_by_signature|find_methods_by_signature]]
- [[features/for_each_instance|for_each_instance]]
- [[features/for_each_loaded_class|for_each_loaded_class]]
- [[features/hook_basic|hook_basic]]
- [[features/interface_polymorphism|interface_polymorphism]]
- [[features/method_enumeration|method_enumeration]]
- [[features/nested_classes|nested_classes]]
- [[features/poly_inherited_oop|poly_inherited_oop]]

## Implementation anchors

- `detail::collect_klass_methods` — `vmhook/ext/vmhook/vmhook.hpp:8804-8835` — shared engine: noexcept, null-klass->empty, count<=0 bail, per-slot is_valid_pointer skip, outer try/catch
- `vmhook::get_class_methods / find_methods_by_signature` — `vmhook/ext/vmhook/vmhook.hpp:8853-8930` — by-name (8853, no own try/catch) and by-wrapper (8873, wrapped) overloads; find_methods_by_signature exact-equality filter at 8913
- `klass::get_methods_count / get_methods_ptr` — `vmhook/ext/vmhook/vmhook.hpp:3498-3580` — _methods Array<Method*> length at +0 (3498) and data at +8 (3537) — the most ABI-fragile constant
- `klass::get_super / symbol::to_string` — `vmhook/ext/vmhook/vmhook.hpp:3729-3741` — _super inheritance-chain primitive (3729); symbol::to_string (2237) reads u16 _length, rejects length 0 or >0x1000
- `klass::find_field / find_field_in_stream / decode_u5` — `vmhook/ext/vmhook/vmhook.hpp:3842-4100` — JDK-era field-format dispatch: decode_u5 (3842), _fieldinfo_stream UNSIGNED5 (3879), _fields Array<u2> + stream selection (3991)
- `vmhook::find_class` — `vmhook/ext/vmhook/vmhook.hpp:13390-13470` — klass resolver: name-keyed cache with stale-cache guard, ClassLoaderDataGraph walk + JNI-context-loader fallback

## Tests

- `tests/jvm/modules/klass_introspection.cpp`
- `tests/jvm/modules/klass_introspection_shapes.cpp`
- `tests/jvm/modules/method_enumeration.cpp`
- `tests/jvm/modules/find_methods_by_signature.cpp`
- `tests/test_method_enumeration.cpp`

## Known bugs

- **[medium]** No klass-kind check before reading the InstanceKlass _methods layout: find_class('[I')/'[Ljava/lang/String;' resolves an ArrayKlass, then get_methods_count/get_methods_ptr apply the cached InstanceKlass._methods offset to a structurally different object. Saved only by the downstream is_valid_pointer + count<=0 bailouts (usually returns empty), but it reads a field that does not exist on that klass kind. Fix: gate on Klass._layout_helper / oop_is_instance before touching _methods.
- **[medium]** _length trusted as an unbounded loop/reserve bound: method_count is the raw int32 read; negatives are filtered (count<=0) but a corrupt large-positive length drives reserve(count) (huge alloc / length_error, caught by the outer catch -> silent empty) and a loop over methods_array[0..count). Only the per-element is_valid_pointer prevents dereferencing the bogus tail. Fix: clamp count to a sane ceiling.
- **[low]** Silent name/descriptor loss on an over-long symbol: symbol::to_string yields "" for _length > 0x1000, and collect_klass_methods then emplaces a ("","") pair rather than skipping the slot. The JVM legal limit is 65535 (u2), so reachable in principle (obfuscators, synthetic lambda names); fixtures use short symbols so untested.
- **[low]** Asymmetric exception handling between the two non-template overloads: get_class_methods<T>() wraps its body in try/catch but get_class_methods(string_view) does not — it is noexcept yet relies on callees never throwing. If a future find_class edit throws past its own catch, the by-name overload std::terminates while the by-type overload degrades to empty.
- **[low]** UNSIGNED5 stream over-read within a record (fields leg): find_field_in_stream checks stream_pos < length only at the top of each field iteration; inside one iteration it issues up to 5+3 decode_u5 calls with no per-call bound check, so a truncated/malformed stream reads a few bytes past the logical Array<u1> end. Inside the is_valid_pointer page envelope (no AV) but reads bytes not part of the array. Fix: bound every decode_u5 against length.

## Notes

The code is heavily defended (null/validity gate on every deref + outer
try/catch), so there is no AV-grade crash on a loaded InstanceKlass; the
defects are type-safety / silent-degradation hazards. Array<Method*> data
offset +8 is x64-specific (4-byte _length + 4-byte pad + 8-byte-aligned
pointers). The fields-format split is the JDK-sensitive leg: JDK 8-~20
InstanceKlass._fields (Array<u2>, 6 slots/field, data at +4, packed-offset
>>2) vs JDK 21.0.x+/22+ _fieldinfo_stream (Array<u1>, UNSIGNED5), selected
purely by which VMStruct gHotSpotVMStructs exports (neither -> std::nullopt).
_methods is stable across 8-26 (an Array<Method*> throughout), so the method
leg is far less JDK-sensitive than the field leg; the only method-side variance
is the synthetic-method set javac emits (JDK 8 extra synthetics), so tests
assert SET membership + LOWER bounds, never an exact total. Klass._name can
carry GC tag bits (stripped via untag_pointer). The no-JVM contract (every entry
point empty without throwing) is pinned by tests/test_method_enumeration.cpp.
