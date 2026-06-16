---
slug: collection_list
title: Collection List
category: collection
status: in_progress
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/in_progress, risk/medium, category/collection, tag/jvm, tag/collections, tag/oop_decode, tag/reference_walk]
---

# Collection List

> **Category:** [[categories/collection|Collection wrappers + element helpers]]  ·  **Status:** `in_progress`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/collection_list-specialist.md`

## Description

Converting a live Java `java.util.List` OOP (decoded from a field/argument inside a hook detour)
into a `std::vector<std::unique_ptr<wrapper>>` via `field_proxy::value_t::to_vector<T>()`.
Covers two fast paths — ArrayList (elementData + size backing-array walk) and LinkedList (first + size
Node chain walk) — with correct size bound, null slots, element order, and per-element heap-object
identity. Input: compressed 32-bit OOP. Output: vector of decoded element wrappers or null slots.

## Depends on

- [[features/collection_type_tags|collection_type_tags]]
- [[features/array_element_helpers|array_element_helpers]]
- [[features/wrapper_pattern|wrapper_pattern]]
- [[features/decode_oop_and_pointers|decode_oop_and_pointers]]
- [[features/klass_introspection|klass_introspection]]

## Related

- [[features/collection_map|collection_map]]
- [[features/collection_set|collection_set]]

## Depended on by

- [[features/collection_iteration_safety|collection_iteration_safety]]
- [[features/collection_linked_list|collection_linked_list]]

## Implementation anchors

- `field_proxy::value_t::to_vector<element_type>()` — `vmhook/ext/vmhook/vmhook.hpp:15638-15686` — public entry point — decodes compressed OOP, branches on field signature, delegates to collection::to_vector for List objects
- `collection::to_vector<element_type>()` — `vmhook/ext/vmhook/vmhook.hpp:14792-14903` — cascade probing ArrayList / LinkedList fast paths; ArrayList 14802-14834, LinkedList 14836-14846
- `ArrayList fast path` — `vmhook/ext/vmhook/vmhook.hpp:14802-14834` — resolves size + elementData, decodes backing array, bounds loop by size (not capacity), reads elementData[i] as compressed uint32
- `LinkedList fast path` — `vmhook/ext/vmhook/vmhook.hpp:14836-14846` — checks first + size present, delegates to linked_list_walk_items helper
- `linked_list_walk_items<element_type, out_t>()` — `vmhook/ext/vmhook/vmhook.hpp:15183-15238` — chain-walk helper — per-node klass_from_oop + find_field(item/next), loop bounded by i < size && valid node
- `decode_oop_pointer(compressed)` — `vmhook/ext/vmhook/vmhook.hpp:4288-4352` — unconditionally treats input as 32-bit narrow oop, reads base/shift from VMStructs, returns base + (compressed << shift)
- `get_array_element<uint32>()` — `vmhook/ext/vmhook/vmhook.hpp:11563-11581` — bounds-checks index against real array_length, returns 0 (→ nullptr) for out-of-range
- `find_field(klass, name)` — `vmhook/ext/vmhook/vmhook.hpp:10997-11046` — memoised per (klass,name) in g_field_cache under g_field_cache_mutex, superclass-chain walk on miss
- `klass_from_oop(oop)` — `vmhook/ext/vmhook/vmhook.hpp:14597-14611` — decodes narrow-klass slot at oop+8 + validity check

## Tests

- `tests/jvm/modules/collection_list.cpp`

## Known bugs

- **[medium]** No uncompressed-OOP path (lines 14814/14821/14852/14865, 15199-15201/15220-15222/15233-15235) — all list fields/slots/Node-links read as 32-bit narrow oops; -XX:-UseCompressedOops or large-heap targets silently return empty vectors with no diagnostic.
- **[low]** linked_list_walk_items re-resolves first and pays mutex-locked find_field per node (15194 + 15214-15215); two g_field_cache_mutex lock/unlock per node = linear O(N) cost, not quadratic, but inefficient vs hoisting item/next offsets.
- **[low]** LinkedList walk has no cycle detection; corrupt next forming a cycle is bounded only by size loop guard (15205), observable as duplicate element OOPs caught by distinct_ok / linkedlist_big_no_cycle_no_dup_nodes check.
- **[low]** ArrayList size trusted without clamping to elementData.length (read at 14808, loop bound at 14819); cannot OOB read (get_array_element bounds-checks at 11572-11573) but yields phantom nullptr tail for size > capacity.
- **[low]** Silent empty-vector on any failure (lines 14799/14811/14834/14845, 15644-15650) indistinguishable from genuinely empty list — no diagnostic on failed decode, wrong-shape klass, or compressed-oops-off.

## Notes

Compressed oops (primary): entire feature assumes narrow-oop field/array/Node-link encoding; default heaps
(< ~32 GB) on JDK 8..25 keep compressed oops on; -XX:-UseCompressedOops or large-heap targets break silently.
VMStruct name fallback: decode_oop_pointer probes Universe::_narrow_oop._* (JDK 8-16) → CompressedOops::_narrow_oop._*
(17-24) → CompressedOops::_* (25+) — future rename would return null base/shift and empty every vector.
ArrayList layout stable across JDK 6..25 (elementData + size field names); array offsets (+12 length, +16 data)
assume HotSpot header layout. LinkedList Node field names (first, item, next) stable JDK 7+; pre-7 Entry had
different names. String readback in fixture tag() inherits compact-strings sensitivity (JDK 9+ coder vs JDK 8).
