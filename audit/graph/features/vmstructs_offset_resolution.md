---
slug: vmstructs_offset_resolution
title: VMStructs Offset Resolution
category: klass
status: in_progress
risk: critical
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/in_progress, risk/critical, category/klass, tag/vmstructs, tag/offset, tag/hotspot, tag/jdk-version, tag/critical-path, tag/static-cache]
---

# VMStructs Offset Resolution

> **Category:** [[categories/klass|Class / Klass introspection]]  ·  **Status:** `in_progress`  ·  **Risk:** `critical`  ·  **Specialist:** `.claude/agents/vmstructs_offset_resolution-specialist.md`

## Description

Resolves HotSpot VM struct and type metadata by walking the `gHotSpotVMStructs`
and `gHotSpotVMTypes` tables exported by jvm.dll / libjvm.so.  Input: a
(type_name, field_name) string pair.  Output: a raw `vm_struct_entry_t*` whose
`->offset` (instance fields, `is_static==0`) or `->address` (static fields,
`is_static!=0`) is the byte displacement every other feature in the library uses
to reach into live JVM objects.  The JVM module handle is resolved once via
`get_jvm_module()` and both table head pointers are cached after the first call
to `get_vm_structs()` / `get_vm_types()`.  On a nullptr head (no JVM in process,
or JVM loaded after first call) all lookups return nullptr and callers fall back
to hardcoded heuristics.  This is the single lowest-level primitive: a wrong
offset silently corrupts every field read, method walk, thread enumeration,
compressed-oop decode, and hook install across all ~130 call sites.

## Depends on

- [[features/os_safe_read|os_safe_read]]

## Related

- [[features/decode_oop_and_pointers|decode_oop_and_pointers]]
- [[features/compressed_oops_decode|compressed_oops_decode]]
- [[features/compressed_klass_decode|compressed_klass_decode]]
- [[features/klass_introspection|klass_introspection]]
- [[features/constantpool_access|constantpool_access]]

## Depended on by

- [[features/adapter_recovery_c2i|adapter_recovery_c2i]]
- [[features/compressed_klass_decode|compressed_klass_decode]]
- [[features/compressed_oops_decode|compressed_oops_decode]]
- [[features/constantpool_access|constantpool_access]]
- [[features/field_introspection|field_introspection]]
- [[features/find_class_fallback|find_class_fallback]]
- [[features/for_each_instance|for_each_instance]]
- [[features/instanceklass_methods_walk|instanceklass_methods_walk]]
- [[features/klass_introspection|klass_introspection]]
- [[features/method_entry_points_i2i_i2c|method_entry_points_i2i_i2c]]
- [[features/method_enumeration|method_enumeration]]
- [[features/method_flags_width|method_flags_width]]

## Implementation anchors

- `vmhook::hotspot::vm_struct_entry_t` — `vmhook/ext/vmhook/vmhook.hpp:1862-1870` — ABI mirror of HotSpot VMStructEntry; offset (instance) and address (static) are separate members, not a union
- `vmhook::hotspot::vm_type_entry_t` — `vmhook/ext/vmhook/vmhook.hpp:1851-1859` — ABI mirror of HotSpot VMTypeEntry; size field used for ConstantPool header sizing
- `vmhook::hotspot::get_jvm_module` — `vmhook/ext/vmhook/vmhook.hpp:1879-1884` — One-shot module handle cache; nullptr if JVM not yet mapped at first call
- `vmhook::hotspot::get_vm_structs` — `vmhook/ext/vmhook/vmhook.hpp:1915-1930` — Double-pointer deref of gHotSpotVMStructs symbol; head cached after first call
- `vmhook::hotspot::get_vm_types` — `vmhook/ext/vmhook/vmhook.hpp:1892-1907` — Double-pointer deref of gHotSpotVMTypes symbol; head cached after first call
- `vmhook::hotspot::iterate_struct_entries` — `vmhook/ext/vmhook/vmhook.hpp:1961-1980` — THE resolver: linear walk with null-arg guard and null-field_name skip; returns raw entry or nullptr
- `vmhook::hotspot::iterate_type_entries` — `vmhook/ext/vmhook/vmhook.hpp:1935-1950` — Type-table walker; returns vm_type_entry_t* whose ->size drives ConstantPool header sizing

## Tests

- `tests/test_iterate_entries_safety.cpp`

## Known bugs

- **[medium]** The resolver returns the raw entry but never validates `is_static`; callers must hand-pick `->offset` vs `->address` with zero enforcement.  A future JVM that flips a field's static-ness, or a new call site that copies the wrong idiom, silently reads garbage with no diagnostic.  `is_static` is never read at the resolver boundary (vmhook.hpp:1961-1980; struct 1867).

- **[low]** One-shot nullptr cache in `get_vm_structs` / `get_vm_types` / `get_jvm_module` (vmhook.hpp:1882, 1895, 1918): if any code path triggers a lookup before jvm.dll is mapped (early injection, lazy LoadLibrary), nullptr is cached permanently and all subsequent lookups fail even after the JVM is present. No invalidation hook exists.

- **[low]** Per-lookup O(N) linear scan with no index.  Hot callers wrap results in `static const` locals (e.g. vmhook.hpp:2211-2212, 2305, 2336), but several fallback-probe paths (vmhook.hpp:4717-4823, 4901, 4945) re-resolve on every call inside object-walk and thread-enumeration hot paths.

- **[low]** Array contents are trusted blindly: the loop guards `entry->type_name` but dereferences it as a C string via `strcmp` with no `is_readable_pointer` check (vmhook.hpp:1968-1978).  A truncated table (missing zero-terminator entry) or a dangling `type_name` pointer walks off the end or faults inside `strcmp`.

- **[low]** No overflow check on `->offset` at the resolver boundary (struct vmhook.hpp:1868). A mismatched JVM build can produce a huge offset; the caller computes `this + entry->offset` into a wild pointer with no sanity ceiling enforced at the one place all callers share.

- **[low]** Stale JDK path hazard: if the JDK installation directory changes after the first module lookup (e.g. jdk-26 renamed to jdk-26.0.1), `get_jvm_module` returns nullptr forever for that process lifetime because the cache is populated once at static-init time.  This broke build-werror JNI cache resolution in CI (see project memory: jvm_buildonly_and_array_super).

- **[low]** `vm_type_entry_t::is_oop_type_type` (vmhook.hpp:1855) doubles the `_type` suffix; the HotSpot field is `isOopType`.  No current caller reads it, but the name will mislead the next person who needs OOP-type classification from gHotSpotVMTypes.


## Notes

JDK 8 exports the richest set of fields via gHotSpotVMStructs; successive
releases progressively drop entries (e.g. `Method._adapter` gone on JDK 9+,
`oopDesc._mark` renamed to `_markWord` around JDK 9, `InstanceKlass._fields`
replaced by `_fieldinfo_stream` on JDK 16+).  The resolver's nullptr-on-miss
contract is what makes JDK-variant fallback probes work throughout the library.

Static-field entries expose `->address` (absolute pointer) not `->offset`; the
caller must read the correct member.  The two disciplines are a load-bearing
convention, not a type-system guarantee.

Stale-JDK-path gotcha (per project memory jvm_buildonly_and_array_super): when
a JDK installation directory is renamed or upgraded (jdk-26 -> jdk-26.0.1) after
the build system caches the module path, `get_jvm_module` finds nothing and the
entire offset resolution layer returns nullptr for the process lifetime.  Re-run
CMake configure to repopulate the cached JVM path.

The no-JVM test suite (tests/test_iterate_entries_safety.cpp) covers null-head
safety and argument-guard paths exhaustively but has zero positive-path coverage:
no test ever feeds a populated table and asserts a correct `->offset` value.
A synthetic in-memory table fixture with a seam into the walk function is the
next required test investment.
