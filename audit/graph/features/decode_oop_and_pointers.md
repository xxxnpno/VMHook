---
slug: decode_oop_and_pointers
title: Decode Oop And Pointers
category: klass
status: in_progress
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/in_progress, risk/medium, category/klass, tag/pointer, tag/codec, tag/compressed-oops, tag/validation, tag/narrow-oop, tag/fault-safe]
---

# Decode Oop And Pointers

> **Category:** [[categories/klass|Class / Klass introspection]]  ·  **Status:** `in_progress`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/decode_oop_and_pointers-specialist.md`

## Description

Pointer-hygiene + compressed-OOP-codec layer validating/decoding every JVM-supplied address
before dereference. Core contracts: `is_valid_pointer` (range + alignment + debug-poison),
`untag_pointer` (strip GC tag bits), `read_pointer<T>` (32-bit zero-extending field read),
`safe_read_pointer` (fault-safe deref via os::safe_read), and the codec pair `decode_oop_pointer` /
`encode_oop_pointer`. Six known flaws: doc undercounting poison set (low), inconsistent 2-byte vs 8-byte alignment
across validators (medium), null-oop codec collision under non-zero base (medium), unguarded 32-bit deref in read_pointer (medium),
blind untag_pointer masking of kernel addresses (low), iOS safe_read unguarded memcpy (low).

## Depends on

- [[features/compressed_oops_decode|compressed_oops_decode]]
- [[features/os_safe_read|os_safe_read]]

## Related

- [[features/vmstructs_offset_resolution|vmstructs_offset_resolution]]
- [[features/klass_introspection|klass_introspection]]

## Depended on by

- [[features/collection_hash_tree_map|collection_hash_tree_map]]
- [[features/collection_linked_list|collection_linked_list]]
- [[features/collection_list|collection_list]]
- [[features/collection_set|collection_set]]
- [[features/collection_type_tags|collection_type_tags]]
- [[features/compressed_klass_decode|compressed_klass_decode]]
- [[features/const_method_bounds|const_method_bounds]]
- [[features/field_inherited|field_inherited]]
- [[features/field_proxy_value_t|field_proxy_value_t]]
- [[features/field_string|field_string]]
- [[features/find_class_context_loader|find_class_context_loader]]
- [[features/hook_basic|hook_basic]]
- [[features/wrapper_pattern|wrapper_pattern]]

## Implementation anchors

- `vmhook::os::user_address_ceiling` — `vmhook/ext/vmhook/vmhook.hpp:505-508` — 0x00007FFFFFFFFFFF constant (exclusive ceiling gate)
- `vmhook::os::user_address_floor` — `vmhook/ext/vmhook/vmhook.hpp:510-513` — 0xFFFF constant (exclusive floor gate)
- `is_valid_pointer(const void*) noexcept -> bool` — `vmhook/ext/vmhook/vmhook.hpp:1768-1805` — Range (floor/ceiling) + 2-byte alignment + debug-poison filter over low-32 bits
- `untag_pointer(const void*) noexcept -> const void*` — `vmhook/ext/vmhook/vmhook.hpp:1813-1818` — AND with user_address_ceiling to strip GC tags (bits 47-63)
- `read_pointer<T>(base, offset) noexcept -> T*` — `vmhook/ext/vmhook/vmhook.hpp:1823-1829` — 32-bit field read at base+offset, zero-extended (unguarded deref, flaw #4)
- `safe_read_pointer(const void*) noexcept -> const void*` — `vmhook/ext/vmhook/vmhook.hpp:1838-1862` — Pre-filters null/range/8-byte-alignment, crosses OS boundary via os::safe_read (flaw #2: 8-byte vs is_valid_pointer's 2-byte)
- `os::safe_read(dst, src, size) noexcept -> bool` — `vmhook/ext/vmhook/vmhook.hpp:899-953` — Windows ReadProcessMemory / macOS mach_vm_read_overwrite / Linux process_vm_readv + sigsetjmp; iOS unguarded memcpy (flaw #6)
- `decode_oop_pointer(std::uint32_t) noexcept -> void*` — `vmhook/ext/vmhook/vmhook.hpp:4288-4352` — Narrow oop to pointer: compressed==0 -> nullptr; resolves base/shift via 3-way VMStruct fallback; computes base + (uint64(compressed) << shift)
- `encode_oop_pointer(void*) noexcept -> std::uint32_t` — `vmhook/ext/vmhook/vmhook.hpp:4360-4424` — Pointer to narrow oop: decoded==nullptr -> 0; same 3-way base/shift resolution; guards decoded < base; returns uint32((addr-base)>>shift) (flaw #3: asymmetric null/base collision)
- `is_readable_pointer(const void*) noexcept -> bool` — `vmhook/ext/vmhook/vmhook.hpp:1739-1753` — Sibling gate: same floor/ceiling + 8-byte alignment (& 0x7) + os::query_region (flaw #2 exposure point)

## Tests

- `tests/test_decode_oop_and_pointers.cpp`

## Known bugs

- **[low]** Doc-comment undercounting poison set (1760-1761 lists 6, but switch at 1791-1799 rejects 9 including 0xABABABAB, 0xFDFDFDFD, 0xDDDDDDDD) — misleads maintainers on poison filter coverage.
- **[medium]** Alignment inconsistency across validators (is_valid_pointer 2-byte @ 1780, safe_read_pointer 8-byte @ 1850, is_readable_pointer 8-byte @ 1746) — addresses passing is_valid_pointer silently drop in safe_read_pointer.
- **[medium]** Null-oop codec collision under non-zero base (decode(0)->nullptr @ 4291, encode(nullptr)->0 @ 4363, encode(base)->0 too; both null and object-at-base round-trip to 0; a decoded < base clamps to 0 @ 4418-4421 rather than flagging).
- **[medium]** Unguarded 32-bit deref in read_pointer<T> @ 1827 — no null/range check on base+offset, faults hard if base is stale, no exception catch (noexcept).
- **[low]** Blind untag_pointer masking @ 1813-1818 — AND any input with no null/validity contract; kernel/non-canonical addresses silently mask into canonical range and pass is_valid_pointer downstream.
- **[low]** iOS safe_read unguarded memcpy @ 919-924 — std::memcpy and return true unconditionally, not fault-safe on target (documented known limitation but 'safe' contract does not hold).

## Notes

JDK-version sensitivities drive the 3-way narrow-oop VMStruct fallback in codec functions (4300-4340 / 4368-4408):
JDK 8-16 export Universe::_narrow_oop._base/_shift; JDK 17-24 moved to CompressedOops::_narrow_oop._base/_shift;
JDK 25+ dropped _narrow_oop prefix to CompressedOops::_base/_shift. Narrow-oop shift is typically 0 (small heaps) or 3 (heaps up to ~32 GB);
above ~32 GB compressed oops disabled entirely. Constants (floor 0xFFFF, ceiling 0x00007FFFFFFFFFFF) assume 48-bit x86-64 canonical split,
architecture-sensitive on 5-level-paging / AArch64. Platform safe_read divergence: Windows/Linux/Android/macOS fault-safe; iOS not (flaw #6).
