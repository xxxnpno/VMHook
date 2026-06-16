---
slug: klass_introspection
title: Klass Introspection
category: klass
status: in_progress
risk: high
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/in_progress, risk/high, category/klass, tag/klass, tag/hotspot, tag/vmstruct, tag/introspection, tag/metadata, tag/instanceklass, tag/safety]
---

# Klass Introspection

> **Category:** [[categories/klass|Class / Klass introspection]]  ·  **Status:** `in_progress`  ·  **Risk:** `high`  ·  **Specialist:** `.claude/agents/klass_introspection-specialist.md`

## Description

Reads a live HotSpot `klass*` or object-header OOP directly (no JNI, no JVMTI) to
extract typed metadata: the internal name symbol (`Klass._name`), the superclass chain
(`Klass._super`), the java.lang.Class mirror (`Klass._java_mirror`), the declared
methods array (`InstanceKlass._methods`), the field records (`InstanceKlass._fields`
or `_fieldinfo_stream`), and the heap-instance size (`Klass._layout_helper`).  The
entry-point for OOP callers is `klass_from_oop`, which decodes the compressed narrow
klass at OOP+8 via `compressed_klass_decode` before handing off to the struct
accessors.  Every accessor resolves its VMStruct offset once (static cache), gates on
`is_valid_pointer`, and falls back to a safe-read or outer try/catch so a bad pointer
silently returns nullptr/0/nullopt instead of faulting.  Because nearly every other
feature (`hook<T>`, `find_class`, `find_field`, collection helpers, `klass_from_oop`
callers) reads through these accessors, a mis-resolved offset or missed validity guard
corrupts the entire downstream call graph silently.

## Depends on

- [[features/vmstructs_offset_resolution|vmstructs_offset_resolution]]
- [[features/compressed_klass_decode|compressed_klass_decode]]
- [[features/decode_oop_and_pointers|decode_oop_and_pointers]]

## Depended on by

- [[features/collection_type_tags|collection_type_tags]]
- [[features/enum_singleton|enum_singleton]]
- [[features/field_inherited|field_inherited]]
- [[features/find_class_fallback|find_class_fallback]]
- [[features/for_each_instance|for_each_instance]]
- [[features/for_each_loaded_class|for_each_loaded_class]]
- [[features/hook_basic|hook_basic]]
- [[features/interface_polymorphism|interface_polymorphism]]
- [[features/nested_classes|nested_classes]]
- [[features/poly_inherited_oop|poly_inherited_oop]]
- [[features/register_class|register_class]]

## Implementation anchors

- `vmhook::hotspot::is_valid_pointer` — `vmhook/ext/vmhook/vmhook.hpp:2018-2055` — Gate behind every klass/method/symbol deref — range, alignment, and nine debug-fill sentinel checks
- `vmhook::hotspot::klass::get_name` — `vmhook/ext/vmhook/vmhook.hpp:3404-3431` — Reads Klass._name via safe_read_pointer + untag_pointer (strips GC tag bits); returns symbol* or nullptr
- `vmhook::hotspot::klass::get_super` — `vmhook/ext/vmhook/vmhook.hpp:3700-3712` — Reads Klass._super; JDK-variant for ArrayKlass/TypeArrayKlass — gated to [INFO] in shapes test at 14413f1
- `vmhook::hotspot::klass::get_instance_size` — `vmhook/ext/vmhook/vmhook.hpp:3729-3747` — Reads Klass._layout_helper; negative value indicates array/interface klass (not instantiable)
- `vmhook::hotspot::klass::get_methods_count / get_methods_ptr` — `vmhook/ext/vmhook/vmhook.hpp:3469-3530` — InstanceKlass._methods Array<Method*>: _length at +0, data at +8 (x64-specific pad); count clamped [0,65535]
- `vmhook::hotspot::klass::find_field / find_field_in_stream` — `vmhook/ext/vmhook/vmhook.hpp:3962-4069` — JDK dispatch: _fieldinfo_stream (JDK 21+, UNSIGNED5) or _fields Array<u2> (JDK 8-20, 6 slots/field)
- `vmhook::klass_from_oop` — `vmhook/ext/vmhook/vmhook.hpp:18738-18774` — Decodes narrow klass from OOP+8 via decode_klass_pointer; Windows-gates the header read through safe_read

## Tests

- `tests/jvm/modules/klass_introspection.cpp`
- `tests/jvm/modules/klass_introspection_shapes.cpp`

## Audit docs

- `audit/LIBRARY_BUGS.md`
- `audit/NO_SEH_COLDREAD_HARDENING.md`

## Known bugs

- **[medium]** No klass-kind check before reading InstanceKlass._methods layout (get_methods_count 3469 / get_methods_ptr 3508). find_class('[I') or any array descriptor resolves an ArrayKlass, not an InstanceKlass; the cached _methods offset then reads a field that does not exist on that layout. Currently saved by downstream is_valid_pointer(array)+count<=0 bailouts, but a future JDK whose ArrayKlass holds a valid-looking pointer at that offset would silently walk garbage as a method array. Fix: gate on _layout_helper <= 0 before touching _methods.  (`audit/LIBRARY_BUGS.md`)
- **[medium]** Array<Method*>::_length trusted as an unbounded reserve/loop bound (get_methods_count 3492). Negatives are filtered but a corrupt large-positive count drove reserve(size_t(count)) (huge alloc caught by outer catch -> silent empty) and indexed methods_array[0..count) past the real array end. Partially mitigated by the [0,65535] clamp added in the same function; verify the clamp is present before the reserve on all callers.  (`audit/LIBRARY_BUGS.md`)
- **[low]** get_super() JDK-variant for ArrayKlass / TypeArrayKlass (klass.get_super 3700). The _super read on ObjArrayKlass/TypeArrayKlass yields JDK/config-dependent results rather than a guaranteed 'java/lang/Object' pointer. Tests in klass_introspection_shapes were hard-asserting this equality and broke on JDK 17+ / windows; gated to [INFO] at commit 14413f1. InstanceKlass._super is reliable; array-klass _super is not.
- **[low]** Symbol length cap in to_string (reached via get_name / method name/descriptor decode): symbols with _length > 0x1000 (4096) decode to ''. An accessor that calls get_name() on a klass or method with an unusually long symbol silently receives nullptr->nullptr->'' rather than the real name. The JVM legal name limit is u2 (65535), so obfuscated/synthetic names in (4096,65535] are silently lost.  (`audit/LIBRARY_BUGS.md`)
- **[low]** UNSIGNED5 stream over-read within a single field record (find_field_in_stream 3850 / decode_u5 3813). The per-field loop guard checks stream_pos < length only at iteration start; up to 5+3 decode_u5 calls inside a single iteration advance stream_pos with no per-call bound. A truncated/malformed Array<u1> stream can read bytes past the logical array end (within the is_valid_pointer page envelope, no AV). Bound each decode_u5 call against length.  (`audit/LIBRARY_BUGS.md`)

## Notes

JDK layout differences that affect this feature:

_java_mirror changed from a plain `oop` (JDK 8-16) to an OopHandle (JDK 17+).
get_java_mirror() detects this at runtime via entry->type_string and performs one
extra level of indirection for OopHandle.  Hardcoding the JDK version would break
on patched builds; the type_string check is the correct approach.

Field storage format split: JDK 8 through early JDK 21 use InstanceKlass._fields
(Array<u2>, 6 slots per field, data at +4, offset reconstructed via >>2).  JDK 21.0.x+
and JDK 22+ export _fieldinfo_stream (Array<u1>, UNSIGNED5 compressed).  find_field()
dispatches on which VMStruct entry is present; a JDK exporting neither degrades to
nullopt.  Array<u2> also carries a trailing _java_fields_count u2 on JDK 8 — handled
by integer-division loop bound.

Array<Method*> data offset +8 (get_methods_ptr 3529) is x64-specific: 4-byte _length
at +0, 4 bytes alignment padding at +4, 8-byte-aligned Method* _data[0] at +8.  A 32-bit
or differently-aligned build would need a different constant.  On all 64-bit HotSpot
8-26 builds this is stable.

Klass._name can carry GC tag bits (stripped via untag_pointer in get_name 3422).  This
makes the name read GC-robust across all collectors including ZGC.

The array-klass get_super() defect (commit 14413f1): ObjArrayKlass and TypeArrayKlass
_super reads are JDK/config-variant — deterministically failing on JDK 17+ in CI
when asserted as hard checks in klass_introspection_shapes.  InstanceKlass._super
is reliable.  The shapes test now [INFO]-characterizes array-klass super reads rather
than hard-asserting them.

klass_from_oop on Windows gates the OOP+8 narrow-klass header read through
os::safe_read (ReadProcessMemory) to avoid uncontained faults on no-SEH toolchains
(MinGW / clang-on-windows).  POSIX keeps the raw read because the JVM's signal
handler contains stray AVs there and gating regressed other oop-walk paths.
