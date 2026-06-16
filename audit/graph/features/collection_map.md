---
slug: collection_map
title: Collection Map
category: collection
status: in_progress
risk: high
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/in_progress, risk/high, category/collection, tag/jvm, tag/heap, tag/compressed-oops, tag/collection, tag/field-walk]
---

# Collection Map

> **Category:** [[categories/collection|Collection wrappers + element helpers]]  ·  **Status:** `in_progress`  ·  **Risk:** `high`  ·  **Specialist:** `.claude/agents/collection_map-specialist.md`

## Description

Decodes a live `java.util.Map` field (HashMap, LinkedHashMap, or TreeMap) directly from
the JVM heap into C++ `std::vector<std::pair<unique_ptr<K>,unique_ptr<V>>>` without JNI or
Java call-gates. The entry surface is `field_proxy::value_t::to_entries<K,V>()`, which
delegates to `vmhook::map::to_entries<K,V>()`. The dispatcher selects HashMap/LinkedHashMap
(via `table` field bucket walk) or TreeMap (via iterative red-black `root` in-order traversal).
Null keys/values decode as `nullptr` in the pair. All reads happen via compressed-oops oop
decoding and by-name field resolution off the live klass.

## Depends on

- [[features/collection_type_tags|collection_type_tags]]
- [[features/wrapper_pattern|wrapper_pattern]]

## Related

- [[features/collection_list|collection_list]]
- [[features/collection_set|collection_set]]

## Depended on by

- [[features/collection_hash_tree_map|collection_hash_tree_map]]
- [[features/collection_iteration_safety|collection_iteration_safety]]

## Implementation anchors

- `field_proxy::value_t::to_entries<K,V>()` — `vmhook/ext/vmhook/vmhook.hpp:15696-15711` — entry point — reads field as narrow oop, decodes, null-guards, constructs vmhook::map and delegates
- `vmhook::map::to_entries<K,V>()` — `vmhook/ext/vmhook/vmhook.hpp:15116-15141` — dispatcher — selects HashMap/LinkedHashMap (table field) or TreeMap (root field) fast path
- `hash_map_walk_entries<K,V,out_t>` — `vmhook/ext/vmhook/vmhook.hpp:15249-15329` — bucket walk — decodes table array, iterates buckets, follows next chains per node
- `tree_map_walk_entries<K,V,out_t>` — `vmhook/ext/vmhook/vmhook.hpp:15414-15521` — iterative in-order traversal with explicit stack — no recursion, visits all nodes sorted
- `klass_from_oop` — `vmhook/ext/vmhook/vmhook.hpp:14597-14611` — reads narrow klass at oop+8, decodes via decode_klass_pointer
- `get_field_by_oop_klass` — `vmhook/ext/vmhook/vmhook.hpp:14674-14708` — resolves field by name off live OOP's klass header (not C++ type registry)
- `decode_oop_pointer` — `vmhook/ext/vmhook/vmhook.hpp:4288-4352` — compressed-oop decode — base + (compressed << shift) using CompressedOops VMStruct
- `decode_array_oop` — `vmhook/ext/vmhook/vmhook.hpp:16078-16087` — thin object/array oop variant
- `array_length` — `vmhook/ext/vmhook/vmhook.hpp:11542-11551` — reads _length at array oop+12 (compressed-oops header)
- `read_java_string` — `vmhook/ext/vmhook/vmhook.hpp:15723-15855` — JDK8 char[] vs JDK9+ byte[]+coder LATIN1/UTF16 branching; decodes keys

## Tests

- `tests/jvm/modules/collection_map.cpp`

## Known bugs

- **[high]** Entire feature silently returns empty under -XX:-UseCompressedOops (heaps >32GB). All reference loads are hardcoded 4-byte narrow oops (15700, 15266-15268, 15431-15433, 15301-15309, 15323-15325, 15463-15466, 15510-15512, 14603-14604); decode_oop_pointer returns nullptr when CompressedOops VMStruct entries absent (4342-4345); no 8-byte fallback.
- **[medium]** read_java_string and array_length bake in compressed-oops header offsets (+12 for _length, +16 for data, 15762/15771/11542-11551); with compression off, klass is 8 bytes and lengths move to +16/+20 — keys and bucket indices misread.
- **[medium]** Fast-path selection is purely name-based (get_field_by_oop_klass('table') shadows 'root', 15127 vs 15134); HashMap wins ties; a third-party Map with 'table' field that is not HashMap.Node[] is mis-walked (15296-15299 bails to empty silently).
- **[low]** value_t::to_entries reads field oop with no type/signature check (15700); blindly reinterprets as narrow oop; primitive-typed field would be decoded as oop and fed to klass_from_oop; downstream guard (no table/root → empty) catches non-Map, but defense-by-luck not by signature gate.

## Notes

**Compressed oops assumption (dominant sensitivity).** Feature only works with
-XX:+UseCompressedOops (the default below ~32GB heap on JDK 8..25). Disable it or cross
32GB and flaws #1/#2 make every map decode empty. CI matrix runs the default, masking the
limitation.
**String layout 8 vs 9+.** read_java_string branches on presence of coder field: JDK8 has
UTF-16 char[] value (no coder); JDK9+ has byte[]+coder LATIN1/UTF16. Keys in fixture are
pure ASCII so both paths decode identically.
**Node/TreeNode field stability.** HashMap.Node and HashMap.TreeNode expose key/value/next
since Java 8; TreeMap.Entry (key/value/left/right/parent/color) stable since Java 1.2.
A JDK renaming any field silently breaks the walk (find_field returns nullopt, chain bails
to empty, 15296-15299 / 15484-15487) rather than crashing.
