---
slug: make_java_array
title: Make Java Array
category: jni
status: seeded
risk: high
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/high, category/jni, tag/jni, tag/array, tag/allocation, tag/tlab, tag/arrayoop, tag/compressed-oops, tag/jdk8-fallback, tag/x86_64]
---

# Make Java Array

> **Category:** [[categories/jni|JNI plumbing (arg packing / refs / java values)]]  ·  **Status:** `seeded`  ·  **Risk:** `high`  ·  **Specialist:** `.claude/agents/make_java_array-specialist.md`

## Description

`make_java_array(class_name, length, element_size, allow_jni_fallback=true)`
allocates a brand-new Java ARRAY oop straight from native code with NO JNI
NewTypeArray/NewObjectArray, across every primitive and reference element
descriptor. Flow: negative-length guard (first statement, returns nullptr);
`find_class(class_name)` resolves the array klass by descriptor ("[B"/"[I"/
"[Ljava/lang/Object;"); a JDK-8 "FIX D" fallback (when find_class misses on a
'['-prefixed name, because JDK-8 ClassLoader.loadClass rejects array
descriptors) calls `JNIEnv::FindClass` + `jni_klass_from_class_mirror`;
`make_java_object(klass, 16 + length*element_size)` does the raw TLAB
allocation + oopDesc header stamp; finally `*(int32*)(oop+12) = length` writes
the arrayOop `_length` slot (x64 compressed-oops layout: 16-byte header,
`_length` at +12, data at +16). Needs a live `current_java_thread`, so it only
works inside an interpreter detour. It is the primitive `make_java_string` is
built on ([B/[C allocations), so its primitive paths are load-bearing.

## Depends on

- [[features/find_class_fallback|find_class_fallback]]
- [[features/compressed_klass_decode|compressed_klass_decode]]
- [[features/compressed_oops_decode|compressed_oops_decode]]

## Related

- [[features/make_java_string|make_java_string]]
- [[features/make_unique|make_unique]]
- [[features/field_arrays_primitive|field_arrays_primitive]]
- [[features/field_arrays_object|field_arrays_object]]
- [[features/array_element_helpers|array_element_helpers]]

## Implementation anchors

- `vmhook::make_java_array` — `vmhook/ext/vmhook/vmhook.hpp:14181-14263` — negative-length guard, find_class, JDK-8 FIX-D jni_find_class fallback (14199), make_java_object, _length write at oop+12 (14261)
- `vmhook::make_java_object` — `vmhook/ext/vmhook/vmhook.hpp:14053-14180` — raw TLAB allocation (256-thread walk + SMR-list fallback), zeroed, oopDesc header stamped; needs current_java_thread
- `detail::jni_find_class / jni_klass_from_class_mirror` — `vmhook/ext/vmhook/vmhook.hpp:11647-11960` — the FIX-D JNI fallback: FindClass accepts array descriptors JDK-8's loadClass rejects (11647); mirror->Klass* (11920)
- `vmhook::array_length / get_array_element / set_array_element` — `vmhook/ext/vmhook/vmhook.hpp:14556-14650` — data-region validators: array_length reads +12 (14556); element access at +16 stride sizeof(T), bounds-checked (14578/14610)
- `make_java_string backing-array call sites` — `vmhook/ext/vmhook/vmhook.hpp:14357-14397` — make_java_array('[B',n,1) LATIN1 (14357), ('[B',n*2,1) UTF16 (14379), ('[C',n,2) classic (14395) — HARD invariants on every JDK

## Tests

- `tests/jvm/modules/make_java_array.cpp`

## Known bugs

- **[medium]** make_java_array leaks a PENDING JNI exception on the miss path: its internal find_class routes through JNIEnv::FindClass which sets a pending NoClassDefFoundError/ClassNotFoundException on a miss; it is cleared ONLY inside the '['-prefixed fallback and ONLY when that fallback's FindClass returns non-null. So a non-'[' descriptor, or a '[' descriptor whose element class is missing, leaves the exception set on the thread — aborting the next JNI call under -Xcheck:jni and surfacing when the interpreter resumes. Fix: jni_exception_clear() after the initial find_class miss. The module defensively clears at end-of-detour.
- **[low]** Hardcoded x64 compressed-oops arrayOop layout: array_header_size literal 16, _length written at literal +12, data assumed at +16. With compressed oops / compressed class pointers disabled (heaps >32 GB) the header is larger and _length moves; a 32-bit VM differs again. No layout parameter, never consults VMStructs for length_offset/base_offset. Harmless on all-x64 CI (default compressed oops), a real portability ceiling.
- **[low]** element_size is unvalidated and only sizes the allocation (16 + length*element_size) — not the _length slot, not the klass stamp. An element_size SMALLER than the true JVM stride under-allocates the data region while _length advertises the full count, so any later element access past the allocated bytes is out-of-bounds heap corruption the function cannot detect. No overflow guard on length*element_size (a huge product just fails TLAB alloc gracefully).
- **[low]** A non-array descriptor is not rejected structurally: the FIX-D fallback is only entered when class_name.front()=='['. A loadable non-array name (e.g. 'java/lang/Object') would have find_class succeed and the code would stamp that InstanceKlass into an object sized 16+length*element_size and write a bogus _length at +12 — fabricating a malformed 'array' of a non-array klass. Misuse, but no is_array_descriptor guard.

## Notes

Reference-array allocation on JDK 8 is the gated dimension: [Z..[D are HARD on
every JDK (FIX-D's JNI FindClass resolves primitive-array klasses on JDK 8, and
make_java_string proves [B/[C work there). The object-array descriptors
([Ljava/lang/Object;, [Ljava/lang/String;) depend on FIX-D resolving an
ObjArrayKlass on JDK 8; the module detects JDK 8 via the String 'coder' probe
and gates ref-array asserts best-effort (HARD on JDK 9+, [INFO] SKIPPED only
when JDK 8 genuinely returns null). The +12 length offset and 16-byte header
are the compressed-oops x64 layout (every CI host satisfies it). make_java_object's
TLAB path is JDK-dependent in fast/slow selection but driven the same way on
every JDK. Fixture is Java-8 syntax only. Every made oop is null- and
is_valid_pointer-gated before it is wrapped, stored, or element-accessed.
