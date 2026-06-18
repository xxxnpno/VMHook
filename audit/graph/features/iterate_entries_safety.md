---
slug: iterate_entries_safety
title: Iterate Entries Safety
category: enumeration
status: seeded
risk: high
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/high, category/enumeration, tag/enumeration, tag/vmstructs, tag/safety, tag/bounds, tag/null-guard, tag/abi, tag/foundation]
---

# Iterate Entries Safety

> **Category:** [[categories/enumeration|Live-VM enumeration (heap / classes / threads)]]  ·  **Status:** `seeded`  ·  **Risk:** `high`  ·  **Specialist:** `.claude/agents/iterate_entries_safety-specialist.md`

## Description

The traversal-safety / bounds / null-guard contract of the two HotSpot
VMStruct walkers — `hotspot::iterate_struct_entries(type, field)` and
`iterate_type_entries(type)` — plus the cached symbol resolvers under them
(`get_vm_structs`, `get_vm_types`, `get_jvm_module`, `find_jvm_module`). The
promise: walk the `gHotSpotVMStructs`/`gHotSpotVMTypes` C arrays exported by
libjvm and NEVER fault — null-guard every string arg, use a two-part loop
guard `entry && entry->type_name` (bail on null head, stop at the
zero-`type_name` terminator), and a per-entry `field_name==nullptr` skip
before the double `strcmp` (a partial JVMTI-published entry has a non-null
`type_name` but null `field_name`). Degrades to `nullptr` when no JVM / a
non-HotSpot VM is present. Every higher-level introspection path bottoms out
here, so its safety is the foundation the whole header stands on.

## Depends on

- [[features/vmstructs_offset_resolution|vmstructs_offset_resolution]]

## Related

- [[features/vmstructs_offset_resolution|vmstructs_offset_resolution]]
- [[features/for_each_loaded_class|for_each_loaded_class]]

## Depended on by

- [[features/for_each_instance|for_each_instance]]
- [[features/for_each_loaded_class|for_each_loaded_class]]
- [[features/for_each_thread|for_each_thread]]
- [[features/method_entry_points_i2i_i2c|method_entry_points_i2i_i2c]]

## Implementation anchors

- `hotspot::iterate_type_entries` — `vmhook/ext/vmhook/vmhook.hpp:1964-1988` — null guard + two-part loop guard (entry && entry->type_name, 1971) + strcmp on type_name
- `hotspot::iterate_struct_entries` — `vmhook/ext/vmhook/vmhook.hpp:1990-2020` — both-args null guard + per-entry field_name==nullptr skip before double strcmp
- `hotspot::get_vm_types` — `vmhook/ext/vmhook/vmhook.hpp:1921-1943` — function-local static, get_proc_address(gHotSpotVMTypes), extra deref of pointer-to-array
- `hotspot::get_vm_structs` — `vmhook/ext/vmhook/vmhook.hpp:1944-1963` — same shape for gHotSpotVMStructs
- `os::find_jvm_module` — `vmhook/ext/vmhook/vmhook.hpp:553-600` — platform candidate list (jvm.dll/libjvm.so/.dylib) via already-loaded-only probe; module cached as a function-local static (1911)

## Tests

- `tests/test_iterate_entries_safety.cpp`

## Known bugs

- **[medium]** is_static is never consulted; entry->offset is read blindly for static fields where it is meaningless (HotSpot puts a static field's location in address, not offset). iterate_struct_entries hands back the entry without exposing/checking is_static, so a caller that looks up a static field and adds ->offset computes garbage. Latent today (all call sites query instance fields).
- **[medium]** ABI layout assumption: vm_struct_entry_t/vm_type_entry_t must bit-match libjvm's VMStructEntry/VMTypeEntry for the running JVM. ++entry strides by sizeof as the header declares it; any member add/reorder or 32-bit padding change makes the stride wrong and feeds misaligned/wild pointers to strcmp. No version/size sanity check. Low probability, unbounded blast radius.
- **[low]** get_vm_*/get_jvm_module cache the FIRST resolution permanently, including a nullptr taken before libjvm was loaded (function-local statics). Touch a getter pre-JNI_CreateJavaVM and every later lookup returns nullptr forever, silently disabling all introspection. Order-of-first-call is load-bearing and undocumented.
- **[low]** Terminator assumption is zero type_name, not a fixed count (the for-loop guards entry->type_name only). A corrupted/unterminated global makes the walk run off the array end until it hits a zero type_name — a long wild read. No max-iteration cap backstop; the field_name skip hardens the value path, not the length path.
- **[low]** Linear O(N) scan on every call with no caching of the entry result; hot paths mitigate by caching the returned entry in a static const at the call site, but iterate_* re-walks from the head every call — a foot-gun the 'iterate' name underadvertises.

## Notes

These two functions are unusually well-hardened (explicit null guards on every
string arg, two-part loop guard, per-entry field_name skip); there is no
out-and-out crash bug on the no-JVM / null path — exactly what the dedicated
no-JVM test (tests/test_iterate_entries_safety.cpp, ~45 checks) proves. Risk is
high by blast radius: every Symbol/Method/ConstMethod/Klass/oopDesc/
ConstantPool/thread/compressed-oop offset path bottoms out here. The walker is
version-agnostic but the NAMES it is asked for changed across JDKs
(oopDesc._mark -> _markWord, Method._from_compiled_code_entry_point ->
_from_compiled_entry, InstanceKlass._fields -> _fieldinfo_stream,
CompressedOops/CompressedKlassPointers _base/_shift eras), so the miss ->
nullptr path is load-bearing for cross-version support. Non-HotSpot / no-VM
runtimes (Android ART, iOS) -> find_jvm_module nullptr -> graceful
degradation, the most important cross-platform invariant.
