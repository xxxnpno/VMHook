---
slug: collection_type_tags
title: Collection Type Tags
category: collection
status: in_progress
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/in_progress, risk/medium, category/collection, tag/containers, tag/type-tags, tag/to_vector, tag/to_entries, tag/ArrayList, tag/LinkedList, tag/HashMap, tag/TreeMap, tag/field-detection]
---

# Collection Type Tags

> **Category:** [[categories/collection|Collection wrappers + element helpers]]  ·  **Status:** `in_progress`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/collection_type_tags-specialist.md`

## Description

Six C++ wrapper type-tags over java.util containers (collection, list, set, linked_list, map, hash_map)
and the element type-tag mapping that converts a live OOP's field shape (ArrayList/LinkedList/HashSet/TreeSet/
HashMap/TreeMap detected by hard-coded field names) into traversal helpers. Entry points to_vector<E>() and
to_entries<K,V>() return std::vector<std::unique_ptr<T>> and std::vector<std::pair<...>> with the never-throw /
empty-on-failure guarantee: null/invalid OOP or unrecognized container layout silently yields {}.

## Depends on

- [[features/klass_introspection|klass_introspection]]
- [[features/decode_oop_and_pointers|decode_oop_and_pointers]]

## Related

- [[features/collection_list|collection_list]]
- [[features/collection_set|collection_set]]
- [[features/collection_map|collection_map]]
- [[features/collection_linked_list|collection_linked_list]]
- [[features/collection_hash_tree_map|collection_hash_tree_map]]

## Depended on by

- [[features/collection_list|collection_list]]
- [[features/collection_map|collection_map]]
- [[features/collection_set|collection_set]]

## Implementation anchors

- `Forward declarations of six type-tag classes` — `vmhook/ext/vmhook/vmhook.hpp:1471-1476` — collection, list, set, linked_list, map, hash_map
- `class collection : public object_base` — `vmhook/ext/vmhook/vmhook.hpp:14643-14650` — Base wrapper and dispatch hub; ctor at 14656-14659
- `collection::oop_klass()` — `vmhook/ext/vmhook/vmhook.hpp:14666-14669` — Extract live OOP's klass via klass_from_oop
- `collection::get_field_by_oop_klass()` — `vmhook/ext/vmhook/vmhook.hpp:14674-14708` — Klass-driven field lookup; inherited-static mirror path 14687-14701
- `collection::get_method_by_oop_klass()` — `vmhook/ext/vmhook/vmhook.hpp:14713-14734` — Klass-driven method walk up super chain
- `collection::size()` — `vmhook/ext/vmhook/vmhook.hpp:14742-14750` — Virtual dispatch to klass-driven size() lookup; returns 0 on no method
- `collection::is_empty()` — `vmhook/ext/vmhook/vmhook.hpp:14760-14763` — Returns true on null OOP or empty size (indistinguishable, Flaw #4)
- `collection::to_vector<element_type>()` — `vmhook/ext/vmhook/vmhook.hpp:14792-14903` — Element type-tag mapping cascade; ArrayList (14803-14834), LinkedList (14837-14846), HashSet (14850-14859), TreeSet (14863-14872), generic get(int) fallback (14874-14902)
- `class list : public collection` — `vmhook/ext/vmhook/vmhook.hpp:14921-14928` — Pure type-tag; no own logic
- `class set : public collection` — `vmhook/ext/vmhook/vmhook.hpp:14942-14949` — Pure type-tag; no own logic
- `class linked_list : public list` — `vmhook/ext/vmhook/vmhook.hpp:14963-14970` — Pure type-tag; no own logic; substitutable for list and collection
- `class map : public object_base` — `vmhook/ext/vmhook/vmhook.hpp:14986-14993` — Deliberately not a collection (Java semantics); duplicates oop_klass/get_field/get_method helpers 15002-15073
- `map::size()` — `vmhook/ext/vmhook/vmhook.hpp:15079-15092` — Falls back to size field when no size() method; returns 0 on failure
- `map::to_entries<key_type,value_type>()` — `vmhook/ext/vmhook/vmhook.hpp:15116-15141` — Key-value type-tag mapping; HashMap (table → hash_map_walk_entries), TreeMap (root → tree_map_walk_entries), else empty
- `class hash_map : public map` — `vmhook/ext/vmhook/vmhook.hpp:15157-15164` — Pure type-tag; no own logic
- `linked_list_walk_items()` — `vmhook/ext/vmhook/vmhook.hpp:15182-15238` — Walk helper; per-Node bound via snapshotted size; null Java elements → nullptr
- `hash_map_walk_entries()` — `vmhook/ext/vmhook/vmhook.hpp:15249-15329` — Walk helper; per-bucket chain cap 1<<20; HashMap/LinkedHashMap entry traversal; concurrent-mutation race risk
- `hash_map_walk_keys()` — `vmhook/ext/vmhook/vmhook.hpp:15335-15402` — Walk helper; HashSet/LinkedHashSet key traversal via map field
- `tree_map_walk_entries()` — `vmhook/ext/vmhook/vmhook.hpp:15414-15521` — Walk helper; iterative in-order via std::vector<void*> stack; visited cap > 1<<24 (concurrent-mutation race)
- `tree_map_walk_keys()` — `vmhook/ext/vmhook/vmhook.hpp:15527-15559` — Walk helper; TreeSet key traversal via m field
- `field_proxy::value_t::to_vector<E>()` — `vmhook/ext/vmhook/vmhook.hpp:15638-15686` — User-facing API; decodes compressed OOP at 15644-15650, special-cases [L/[[ arrays (15659-15683), else routes to collection::to_vector (15685)
- `field_proxy::value_t::to_entries<K,V>()` — `vmhook/ext/vmhook/vmhook.hpp:15696-15711` — User-facing API; decodes compressed OOP, routes to map::to_entries; returns {} on null/invalid

## Tests

- `tests/test_collection_type_tags.cpp`

## Known bugs

- **[medium]** value_t::to_vector maps onto Map field via collection::to_vector, which tries get(int) — wrong arity (Map.get(Object), not get(int)), silently invokes interpreter gate per element returning nulls instead of rejecting the type. Signature guard at 15659 ("[L"/"[[") skips arrays but does not guard against Map fields ("L...;"). Flaw exists at 14890 + 15685; symmetric guard at 11833-11836 exists for cast_for_variant<unique_ptr> but was never extended.
- **[medium]** Generic to_vector fallback (14874-14902) blindly calls get(int) on any Collection after fast-path miss, including non-RandomAccess and Queue/Deque impls. PriorityQueue/custom-List get(int)=O(N) → O(N²) with N live call gates, no klass-shape confirmation. Comment at 14874 says "only valid for List impls" but is the only guard.
- **[low]** Concurrent structural mutation races every walk helper — bucket rehash (HashMap), red-black rotation (TreeMap), node relink (LinkedList) can yield stale next/left/right compressed OOP. Bounds (1<<20 per bucket, 1<<24 visited entries, size-capped LinkedList) prevent infinite loop but not torn reads; returned vector can contain duplicates or miss entries.
- **[low]** size() returns 0 indistinguishably for empty and for null/broken OOP (no klass / no size() method); is_empty() true both cases. Documented null-safe contract but caller cannot distinguish — latent foot-gun the tests must pin so contract never silently flips to sentinel.
- **[low]** map and collection duplicate ~70 lines of klass helpers (15002-15073 vs 14666-14734); byte-identical get_field_by_oop_klass and get_method_by_oop_klass. Future fix to one (e.g. inherited-static mirror path) that misses the other would diverge silently — drift hazard flagged for parity regression test.

## Notes

Feature depends on hard-coded field names stable across JDK 8/11/17/21/24/25/26: elementData/size (ArrayList),
first/item/next (LinkedList.Node), map (HashSet), m (TreeSet), table/key/value/next (HashMap.Node & TreeNode),
root/left/right (TreeMap.Entry). These are OpenJDK private names, not JLS-stable — non-HotSpot VMs (J9/GraalVM)
or a future JDK rename silently drops to empty path with no diagnostic. HashMap treeification (JDK 8+) converts
≥8-collision buckets to TreeNode; hash_map_walk_entries/_keys rely on TreeNode exposing same key/value/next field
names as Node (true through JDK 21 per 15243-15247). Compressed oops required (all JDK 8-26 heaps <~32GB default);
-XX:-UseCompressedOops or huge heap silently breaks every walk. Java 21+ sequenced collections did not alter backing
layout so unaffected through 21; JDK 26 Valhalla record/value-class flattening or field rename is a silent-break risk.
