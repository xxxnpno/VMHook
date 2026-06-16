---
slug: collection_hash_tree_map
title: Collection Hash Tree Map
category: collection
status: in_progress
risk: high
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/in_progress, risk/high, category/collection, tag/jvm, tag/collection, tag/map, tag/oops, tag/traversal, tag/safety]
---

# Collection Hash Tree Map

> **Category:** [[categories/collection|Collection wrappers + element helpers]]  ·  **Status:** `in_progress`  ·  **Risk:** `high`  ·  **Specialist:** `.claude/agents/collection_hash_tree_map-specialist.md`

## Description

Decoding `java.util.HashMap` via bucket-walk and `java.util.TreeMap` via red-black
in-order traversal — both from raw OOP without Java dispatch per entry. The feature
routes both container types through a unified dispatcher (`vmhook::map::to_entries<K,V>`)
that probes for `table` field (HashMap/LinkedHashMap) before falling back to `root`
(TreeMap). Returned entries are `std::vector<std::pair<K,V>>` in bucket order
(HashMap) or natural sorted order (TreeMap), with null keys/values decoded to nullptr.

## Depends on

- [[features/collection_map|collection_map]]
- [[features/collection_iteration_safety|collection_iteration_safety]]
- [[features/decode_oop_and_pointers|decode_oop_and_pointers]]
- [[features/klass_introspection|klass_introspection]]

## Related

- [[features/collection_iteration_safety|collection_iteration_safety]]
- [[features/collection_linked_list|collection_linked_list]]

## Implementation anchors

- `vmhook::map` — `vmhook/ext/vmhook/vmhook.hpp:14986-15141` — wrapper over any java.util.Map OOP; contains to_entries<K,V>() dispatcher and size()/is_empty()
- `map::to_entries<K,V>()` — `vmhook/ext/vmhook/vmhook.hpp:15116-15141` — dispatcher — probes table field first (HashMap), then root (TreeMap), returns empty vector if neither resolve
- `get_field_by_oop_klass` — `vmhook/ext/vmhook/vmhook.hpp:15013-15047` — reads klass from OOP header (+8) and resolves field by name without C++ type registry
- `hash_map_walk_entries<K,V>()` — `vmhook/ext/vmhook/vmhook.hpp:15250-15329` — HashMap bucket walk — decodes table array, follows key/value/next Node chains per bucket, capped at 1<<20 per chain
- `tree_map_walk_entries<K,V>()` — `vmhook/ext/vmhook/vmhook.hpp:15414-15521` — TreeMap red-black in-order walk — iterative left-spine/pop/right traversal, outer cap 1<<24
- `field_proxy::value_t::to_entries<K,V>()` — `vmhook/ext/vmhook/vmhook.hpp:11959-11961` — declaration of implicit field-proxy convenience path
- `field_proxy::value_t::to_entries<K,V>()` — `vmhook/ext/vmhook/vmhook.hpp:15696-15711` — definition — decodes compressed OOP and delegates to vmhook::map{}.to_entries<K,V>()
- `vmhook::hash_map` — `vmhook/ext/vmhook/vmhook.hpp:15157-15164` — pure intent tag — derives from map with no override; confirms routing consolidation
- `vmhook::klass_from_oop` — `vmhook/ext/vmhook/vmhook.hpp:14597-14611` — reads klass pointer at hardcoded oop +8 (4 bytes) under narrow-oop assumption
- `vmhook::hotspot::decode_oop_pointer` — `vmhook/ext/vmhook/vmhook.hpp:4288-4352` — narrow_oop_base + (compressed << narrow_oop_shift); version-aware VMStruct names (JDK 8-16/17-24/25+)
- `vmhook::decode_array_oop` — `vmhook/ext/vmhook/vmhook.hpp:16078-16087` — thin decode_oop_pointer + validity wrapper for array OOP
- `vmhook::array_length` — `vmhook/ext/vmhook/vmhook.hpp:11542-11551` — _length field at hardcoded oop +12 under narrow-oop assumption
- `vmhook::get_array_element<T>` — `vmhook/ext/vmhook/vmhook.hpp:11563-11581` — array element fetch at hardcoded +16 + stride*index under narrow-oop assumption

## Tests

- `tests/jvm/modules/collection_hash_tree_map.cpp`

## Known bugs

- **[high]** Whole feature silently mis-decodes when -XX:-UseCompressedOops is set — reads fixed uint32_t and decodes via narrow-oop (table 15266-15269, bucket head 15278-15280, key/value/next 15301-15326, root 15431-15434, left 15463-15466, right 15488-15513), but layout offsets hardcoded for narrow form (klass_from_oop +8/4B, array_length +12, get_array_element +16). Under 64-bit pointers, walks read low 32 bits, decode to nullptr, return empty with no error — size() vs to_entries().size() divergence with silent failure.
- **[medium]** LinkedHashMap returns bucket order instead of insertion/access order — to_entries selects HashMap table path for anything with table field (15127), hash_map_walk_entries follows Node.next in bucket order (15323-15326), but LinkedHashMap preserves order via Entry.before/after which are never read (no before/after anywhere in header). Ordering contract violated silently; contents correct.
- **[low]** TreeMap left-spine inner loop has no iteration cap — documented depth cap is visited > (1<<24) at 15516 in OUTER loop, but inner left-descent while at 15448-15467 has no counter and unbounded stack.push_back at 15462; left-pointer cycle in corrupt heap blows OOM before guard, unlike HashMap walker (15284 guarded only loop).
- **[low]** to_entries routing by field name with HashMap priority — get_field_by_oop_klass(table) probed before (root) at 15127 vs 15134; future Map with both fields force-routes HashMap path, future obfuscation that renames table falls through to empty result with no diagnostic (same flaw as high #1).

## Notes

Narrow-oop base/shift VMStruct names migrate across versions (JDK 8-16 Universe::_narrow_oop, JDK 17-24 CompressedOops::_narrow_oop, JDK 25+ CompressedOops::_base/_shift); decode_oop_pointer 4296-4340 handles all three, but a version whose names match none returns nullptr and walks silently empty. TreeMap.Entry layout (key/value/left/right/parent/color) stable since Java 1.2; walker resolves fields by name via find_field so robust to order shuffles. HashMap class renames (Entry pre-8, Node/TreeNode 8+) handled by field-name walking (key/value/next). Compressed oops is the dominant axis (flaw 1): on by default below ~32 GB heap, so CI always in supported regime; failure invisible until huge-heap or explicit disable. Compact strings (JDK 9+) only affect downstream read_java_string, not Map traversal.
