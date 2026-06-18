---
slug: vmstructs_offset_resolution
title: Vmstructs Offset Resolution
category: klass
status: seeded
risk: high
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/high, category/klass, tag/vmstructs, tag/vmtypes, tag/offset-resolution, tag/hotspot, tag/abi-mirror, tag/klass, tag/foundational, tag/jdk-variance]
---

# Vmstructs Offset Resolution

> **Category:** [[categories/klass|Class / Klass introspection]]  ·  **Status:** `seeded`  ·  **Risk:** `high`  ·  **Specialist:** `.claude/agents/vmstructs_offset_resolution-specialist.md`

## Description

The HotSpot gHotSpotVMStructs / gHotSpotVMTypes table walkers that turn a
(type_name, field_name) pair into a vm_struct_entry_t* whose ->offset (instance
fields) or ->address (static fields) every other feature in the library uses to
reach into live JVM objects. get_jvm_module() caches the jvm.dll/libjvm.so
handle; get_vm_types() / get_vm_structs() resolve the exported global symbols
once (double-pointer deref) and cache the typed head; iterate_type_entries() and
iterate_struct_entries() linearly walk those arrays with null-arg guards, a
defensive `if (!entry->field_name) continue;` to survive partial agent-published
entries, and a strcmp match, returning the RAW entry (the caller reads ->offset
or ->address itself). If this resolver is wrong, every field read, method walk,
thread enumeration, compressed-oop decode, and hook install reads garbage — it
is the single most depended-upon primitive in the header (~130 call sites), and
its nullptr-on-miss contract is what makes the JDK-version fallbacks work.

## Depends on

- [[features/os_safe_read|os_safe_read]]

## Related

- [[features/decode_oop_and_pointers|decode_oop_and_pointers]]
- [[features/compressed_oops_decode|compressed_oops_decode]]
- [[features/compressed_klass_decode|compressed_klass_decode]]
- [[features/klass_introspection|klass_introspection]]

## Depended on by

- [[features/adapter_recovery_c2i|adapter_recovery_c2i]]
- [[features/compressed_klass_decode|compressed_klass_decode]]
- [[features/compressed_oops_decode|compressed_oops_decode]]
- [[features/constantpool_access|constantpool_access]]
- [[features/deoptimize_methods|deoptimize_methods]]
- [[features/field_introspection|field_introspection]]
- [[features/find_class_fallback|find_class_fallback]]
- [[features/hook_basic|hook_basic]]
- [[features/hook_verify_repair|hook_verify_repair]]
- [[features/instanceklass_methods_walk|instanceklass_methods_walk]]
- [[features/interpreter_frame_walk|interpreter_frame_walk]]
- [[features/iterate_entries_safety|iterate_entries_safety]]
- [[features/klass_introspection|klass_introspection]]
- [[features/method_entry_points_i2i_i2c|method_entry_points_i2i_i2c]]
- [[features/method_flags_width|method_flags_width]]

## Implementation anchors

- `vm_type_entry_t / vm_struct_entry_t (ABI mirrors of HotSpot VMTypeEntry / VMStructEntry)` — `vmhook/ext/vmhook/vmhook.hpp:1880-1899` — vm_type_entry_t: type_name, superclass_name, is_oop_type_type (doubled-suffix cosmetic name), is_integer_type, is_unsigned, size (1880-1888). vm_struct_entry_t: type_name, field_name, type_string, is_static(int32), offset(uint64), address(void*) (1891-1899). In real HotSpot offset/address are a UNION (offset valid iff is_static==0); vmhook lays them as two members and never tells the caller which is live
- `get_jvm_module() / get_vm_types() / get_vm_structs()` — `vmhook/ext/vmhook/vmhook.hpp:1908-1960` — get_jvm_module caches the JVM module handle, nullptr when no JVM (1908-1912). get_vm_types resolves gHotSpotVMTypes once with a double-pointer deref and caches the typed head (1921-1936); get_vm_structs the same for gHotSpotVMStructs (1944-1959). The head is a function-local static — a one-shot nullptr is cached forever if called before the JVM is mapped
- `iterate_type_entries(type_name)` — `vmhook/ext/vmhook/vmhook.hpp:1964-1980` — null-arg guard, linear walk terminating on entry && entry->type_name, single strcmp on type_name
- `iterate_struct_entries(type_name, field_name) — THE resolver` — `vmhook/ext/vmhook/vmhook.hpp:1990-2009` — null-arg guard on BOTH args (1994-1997); walk terminating on entry && entry->type_name (2000); defensive `if (!entry->field_name) continue;` (2002-2004) to survive partial entries; double strcmp match (2007). Returns the RAW entry pointer — does NOT inspect is_static; the ~130 callers hand-pick ->offset vs ->address

## Tests

- `tests/test_vmstructs_offset_resolution.cpp`

## Known bugs

- **[medium]** The resolver never tells the caller whether to read ->offset vs ->address; is_static is dropped on the floor. iterate_struct_entries returns the entry but does NOT inspect is_static. The offset/address split is a real HotSpot union, so for a static field ->offset holds garbage and for an instance field ->address does. Correctness depends entirely on each of the ~130 call sites hand-picking the right member — verified correct today, but entry->is_static is read NOWHERE. A future JVM that flips a field's static-ness, or a new caller that copies the wrong idiom, silently reads garbage with zero diagnostic. A safe resolver would expose two typed accessors (resolve_offset asserting !is_static, resolve_static_address asserting is_static).
- **[low]** get_vm_structs / get_vm_types cache a ONE-SHOT nullptr; a JVM loaded after the first call is never seen. The head pointer is a function-local static initialised once. If any path calls a resolver before jvm.dll is mapped (early injection, lazy LoadLibrary, a self-test on a process that loads the JVM later) the nullptr is cached forever and every subsequent lookup returns nullptr even after the JVM is present. No invalidation hook. The no-JVM test actually DEPENDS on this permanence, so a fix must distinguish 'no JVM yet' from 'no JVM ever'.
- **[low]** Head pointer is null-checked but array contents are trusted blindly. The loop guards entry && entry->type_name and skips null field_name, but dereferences entry->type_name / entry->field_name as C strings via strcmp with NO is_readable_pointer check. A corrupted/truncated table (missing zero-terminator, or a type_name pointing into freed memory) walks off the end or faults inside strcmp. The function's stated reason-to-exist is surviving malformed agent-published tables, yet it only hardens the field_name==null case, not a missing terminator or dangling string.
- **[low]** No overflow / bounds relationship between ->offset and the object size is ever checked at the resolver boundary. offset is a uint64_t added to an object base with no sanity ceiling; a bogus huge offset from a mismatched JVM build produces a wild pointer callers then read. The resolver is the natural choke point to reject implausible offsets but does not.
- **[low]** Per-lookup O(N) linear scan with no index; the static-cache idiom is opt-in and easy to forget. Hot callers wrap the result in a `static const ... entry` so the scan runs once, but several call sites (the JavaThread/Thread fallback probes, the CompressedOops/CompressedKlass field-name fallbacks) re-resolve on every invocation inside hot paths. Not a correctness bug, but a real per-call cost on object-walk and oop-decode fast paths and an inconsistency that invites regression.

## Notes

Honest note: the two guards present (null-arg short-circuit, null-field_name
skip) are correct and well-reasoned, and the no-JVM termination is sound. The
hazards above are layout/ABI assumptions and JDK-variance choke points, not
present-day miscompiles.

Test coverage TODAY: tests/test_vmstructs_offset_resolution.cpp (the dedicated
no-JVM lane; the agent def's older name was test_iterate_entries_safety.cpp). In
a no-JVM process both head pointers resolve to nullptr, so it asserts getter
caching (pointer-identity across calls + 1000 hammered calls, the two globals
independently null), JVM-module caching, ~12 real (type,field) pairs returning
nullptr without faulting, real type-name walks, null-arg guards, swapped-args /
empty-string boundaries, and the cross-consistency invariant
getter==nullptr => iterate_*==nullptr. MISSING: the entire SUCCESS path
(a populated synthetic table asserting ->offset/->address round-trip and that
is_static is surfaced) and a live-JVM end-to-end module (none exists) verifying
resolved offsets land on real fields across the JDK matrix.

JDK-version sensitivity is the whole point: symbol presence varies by JDK and
the nullptr-on-miss contract drives the fallbacks — Method._adapter (JDK 8 only),
CompressedOops._base/_shift vs Universe._narrow_oop._base, oopDesc._mark vs
_markWord, _metadata._compressed_klass vs _klass, InstanceKlass._fieldinfo_stream
(JDK 16+) vs _fields (used directly as a version switch), SystemDictionary vs
ClassLoaderData dictionary, Threads SMR (JDK 10+) vs Threads._thread_list, and
JavaThread._* vs Thread._* fallbacks.
