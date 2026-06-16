---
slug: collection_iteration_safety
title: Collection Iteration Safety
category: collection
status: in_progress
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/in_progress, risk/medium, category/collection, tag/jvm, tag/x86_64, tag/safety, tag/bounds, tag/robustness, tag/collection]
---

# Collection Iteration Safety

> **Category:** [[categories/collection|Collection wrappers + element helpers]]  ·  **Status:** `in_progress`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/collection_iteration_safety-specialist.md`

## Description

Robustness and safety contract for collection walks across all degenerate and adversarial container shapes and sizes.
The feature proves four invariants: never crashes on empty/null/oversized/colliding/out-of-order containers;
decoded element count always equals the Java-known size (size is the oracle); walks terminate with no duplicate
element OOP (proving no cycle/re-emit/early-stop); and get_array_element clamps every out-of-bounds index
instead of reading OOB. Decoding works for ArrayList (elementData+size), LinkedList (first+size chain walk),
HashSet (map → hash_map_walk_keys), TreeSet (m → tree_map_walk_keys), and generic size()+get(int) fallback
for Maps. Every path is bounded by size or guards; every element OOP is is_valid_pointer-gated before wrap.

## Depends on

- [[features/collection_list|collection_list]]
- [[features/collection_set|collection_set]]
- [[features/collection_map|collection_map]]

## Related

- [[features/collection_hash_tree_map|collection_hash_tree_map]]
- [[features/collection_linked_list|collection_linked_list]]

## Depended on by

- [[features/collection_hash_tree_map|collection_hash_tree_map]]
- [[features/collection_linked_list|collection_linked_list]]

## Implementation anchors

- `collection::to_vector<E>()` — `vmhook/ext/vmhook/vmhook.hpp:14793-14903` — field-shape dispatch cascade for ArrayList/LinkedList/HashSet/TreeSet/generic fallback; all paths bounded by size or guards
- `linked_list_walk_items<E>` — `vmhook/ext/vmhook/vmhook.hpp:15183-15238` — first→next Node chain walk, re-reads node klass + item/next fields per node, bounded by size argument
- `hash_map_walk_keys<E>` — `vmhook/ext/vmhook/vmhook.hpp:15336-15402` — table Node[] bucket array with next-chain following, guard < (1<<20) cap, handles both Node and TreeNode
- `tree_map_walk_keys<E>` — `vmhook/ext/vmhook/vmhook.hpp:15528-15622` — iterative in-order red-black walk of root using explicit std::vector stack, visited > (1<<24) cap
- `hash_map_walk_entries<K,V>` — `vmhook/ext/vmhook/vmhook.hpp:15250-15329` — key+value twin of hash_map_walk_keys for Map entry decoding
- `tree_map_walk_entries<K,V>` — `vmhook/ext/vmhook/vmhook.hpp:15415-15521` — key+value twin of tree_map_walk_keys for Map entry decoding
- `value_t::to_vector<E>()` — `vmhook/ext/vmhook/vmhook.hpp:15639-15686` — field-proxy entry point: decodes compressed OOP, special-cases OBJECT-ARRAY fields, delegates to collection
- `value_t::to_entries<K,V>()` — `vmhook/ext/vmhook/vmhook.hpp:15697-15711` — Map twin of value_t::to_vector for key+value decoding
- `get_array_element<T>` — `vmhook/ext/vmhook/vmhook.hpp:11563-11581` — bounds-checked element read: is_valid_pointer guard, clamps out-of-bounds index to T{}, memcpy of sizeof(T)
- `array_length` — `vmhook/ext/vmhook/vmhook.hpp:11542-11551` — reads int32 _length slot at byte +12 of array OOP; load-bearing primitive for all bucket/array walks

## Tests

- `tests/jvm/modules/collection_iteration_safety.cpp`

## Known bugs

- **[medium]** to_vector mis-routes Collections.newSetFromMap(HashMap) (cascade at 14848–14872): the cascade routes by first-matching field name, so SetFromMap's backing-map field 'm' is confused with TreeSet's 'm' (line 14863), causing tree_map_walk_keys to be called on a HashMap; find_field(mapKlass, 'root') misses and walk returns empty vector for non-empty Set (lines 60–73 in specialist file).

## Notes

Compressed-oops dependency: every populated reference container decodes narrow-oop element slots. On the all-x64
default-compressed-oops CI matrix this is HARD; on exotic configs (>32GB heap / compressed-oops-disabled / 32-bit VM),
a shape could decode 0, gated by [INFO] + majority floor. Treeification is environmental: HotSpot only treeifies HashMap bins
once table cap ≥ MIN_TREEIFY_CAPACITY(64) AND bin length > 8; the module gates treeified-path assertion on runtime reflection
probe and keeps count==size invariant HARD regardless. newSetFromMap backing-field name 'm' is stable across JDK 8–25.
Container field layout (elementData/size/first/item/next/map/m/table/root/key/value/left/right) is stable across JDK 8–25;
walk helpers re-resolve every field by name per call, so a renamed field on future JDK would surface as short decode (caught by
size oracle), not crash. JDK generation recorded via java.lang.String.coder field presence (9+) for context only.
