---
slug: compressed_oops_decode
title: Compressed Oops Decode
category: klass
status: in_progress
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/in_progress, risk/medium, category/klass, tag/oop, tag/codec, tag/heap, tag/hotspot, tag/compression]
---

# Compressed Oops Decode

> **Category:** [[categories/klass|Class / Klass introspection]]  ·  **Status:** `in_progress`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/compressed_oops_decode-specialist.md`

## Description

The narrow-OOP (Compressed Object Pointer) codec converts HotSpot's 32-bit compressed
object references into real 64-bit heap pointers and back. Decode applies the formula
`real = narrow_oop_base + ((uint64)compressed << narrow_oop_shift)`; encode applies
`narrow = (uint32)((addr - narrow_oop_base) >> narrow_oop_shift)`. The base and shift are
resolved live from the JVM's `gHotSpotVMStructs` table, making the codec completely
JVM-state-dependent with three version-specific fallback lookup paths (Java 8-16 vs 17-24
vs 25+). Every higher-level feature (field reads, collection walkers, typed hook args)
bottoms out in this pair of functions, so a defect here silently corrupts all reference decoding.

## Depends on

- [[features/vmstructs_offset_resolution|vmstructs_offset_resolution]]

## Related

- [[features/compressed_klass_decode|compressed_klass_decode]]
- [[features/constantpool_access|constantpool_access]]
- [[features/find_class_fallback|find_class_fallback]]
- [[features/instanceklass_methods_walk|instanceklass_methods_walk]]
- [[features/klass_introspection|klass_introspection]]

## Depended on by

- [[features/compressed_klass_decode|compressed_klass_decode]]
- [[features/decode_oop_and_pointers|decode_oop_and_pointers]]
- [[features/enum_singleton|enum_singleton]]
- [[features/field_arrays_object|field_arrays_object]]
- [[features/field_object_ref|field_object_ref]]
- [[features/make_java_array|make_java_array]]
- [[features/read_java_string|read_java_string]]

## Implementation anchors

- `decode_oop_pointer(std::uint32_t)` — `vmhook/ext/vmhook/vmhook.hpp:4288-4352` — Forward declaration at 1367-1368; main definition — null guard (4291-4294), base/shift resolution (4304-4317 / 4325-4338), no-resolve guard (4342-4345), read base/shift from VMStruct, compute and return decoded pointer
- `encode_oop_pointer(void*)` — `vmhook/ext/vmhook/vmhook.hpp:4360-4424` — Forward declaration at 1380-1381; main definition — null guard (4363-4366), same triple-fallback base/shift statics (4368-4408), no-resolve guard (4410-4413), below-base guard (4418-4421), return encoded narrow oop
- `vm_struct_entry_t` — `vmhook/ext/vmhook/vmhook.hpp:1612-1620` — VMStruct entry structure; codec dereferences entry->address as the address of the _base/_shift global field
- `iterate_struct_entries(type, field)` — `vmhook/ext/vmhook/vmhook.hpp:1711-1730` — Linear strcmp walk over gHotSpotVMStructs until NULL sentinel; used by base/shift resolution to locate CompressedOops or Universe entries
- `get_vm_structs()` — `vmhook/ext/vmhook/vmhook.hpp:1665-1684` — Resolves gHotSpotVMStructs from JVM module once and caches; returns nullptr with no JVM
- `decode_klass_pointer(std::uint32_t)` — `vmhook/ext/vmhook/vmhook.hpp:4433-4500` — Sibling klass-pointer codec using CompressedKlassPointers / Universe::_narrow_klass; same structure and vulnerabilities as decode_oop_pointer

## Tests

- `tests/test_compressed_oops_decode.cpp`
- `tests/jvm/modules/compressed_oops_decode.cpp`

## Known bugs

- **[medium]** encode_oop_pointer silently maps any sub-base pointer to Java null (4418-4421: if decoded_address < narrow_oop_base return 0). When base is non-zero (typical for heaps 4–32 GB), any foreign/native/stale pointer encodes as 0 (null oop), so a write that should fail instead nulls a live reference. No diagnostic; both decode(0) and encode(out-of-range) return 0, making null indistinguishable from out-of-range from no-JVM.
- **[medium]** encode_oop_pointer has no upper-bound or shift-residue check; static_cast<uint32_t> (4423) can truncate. The delta (addr - base) >> shift is computed in 64 bits then narrowed to uint32; for addresses past the representable range (4GB << shift), high bits are silently dropped, yielding a valid-looking but wrong narrow oop instead of detectable failure. Address not checked to be a multiple of (1 << shift), so encode(decode(x)) ≠ x for non-shift-aligned x.
- **[low]** Doc/lookup-order mismatch (4296-4317, mirrored 4368-4385): comment says JDK 8-16 uses Universe::_narrow_oop, but code tries CompressedOops paths FIRST and falls back to Universe LAST. On real JDK 8-16 every cold decode does two full linear scans of gHotSpotVMStructs before hitting Universe (cached after first call, but first-call cost is ~thousands of strcmp iterations).
- **[low]** Field width assumed without consulting VMStruct type_string (4347-4348, 4415-4416): _base read as uint64, _shift read as uint32. Correct on LP64 but assumes ABI; on hypothetical big-endian target or non-LP64 JVM port, reinterpret_cast reads would mis-decode silently.
- **[low]** No validation that resolved VMStruct entry has backing address (4347 / 4415 dereference entry->address without checking non-null). For a static field HotSpot stores the field address in entry->address; implicit trust that it is readable. If entry->address were null (field present in table but not relocated), decode would dereference null in noexcept function → hard crash, not graceful nullptr. Guard (is_valid_pointer / is_readable_pointer) used elsewhere in file but absent here.

## Notes

JDK version sensitivities: codec correctness depends on which gHotSpotVMStructs row holds
_base/_shift. Java 8–16 stores under Universe::_narrow_oop.{_base,_shift} (reached via third/last
fallback, 4317/4338); Java 17–24 moved to CompressedOops::_narrow_oop.{_base,_shift} (first attempt,
4304/4325, the happy path); Java 25+ dropped _narrow_oop prefix to CompressedOops::{_base,_shift} (second attempt,
4311/4332). Heap-size driven: shift=0 for heaps <4GB (raw address), shift=3 for heaps up to ~32GB
(8-byte alignment); very large heaps disable compressed oops entirely and consumer heuristic skips decode.
Round-trip guarantees: only null is guaranteed (encode(decode(0))=0, decode(encode(nullptr))=nullptr);
non-null round-trip requires a real heap base/shift from JVM, and non-shift-aligned addresses may lose
low bits with no error due to flaw #2.
