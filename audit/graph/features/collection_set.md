---
slug: collection_set
title: Collection Set
category: collection
status: in_progress
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/in_progress, risk/medium, category/collection, tag/jvm, tag/collection, tag/vector, tag/HashSet, tag/TreeSet, tag/field-decode]
---

# Collection Set

> **Category:** [[categories/collection|Collection wrappers + element helpers]]  ·  **Status:** `in_progress`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/collection_set-specialist.md`

## Description

Decoding a live `java.util.Set` field — `HashSet`, `LinkedHashSet`, `TreeSet`, and
`Collections.newSetFromMap(...)` — into a `std::vector<std::unique_ptr<wrapper>>` via
`field_proxy::value_t::to_vector<element_type>()`. The feature routes through two field-shape
fast paths: `map` field for HashSet/LinkedHashSet (walks the backing `HashMap` bucket chain),
and `m` field for TreeSet (walks the backing `TreeMap` red-black tree in sorted order). Sets
have no `get(int)`, so the generic List fallback is unreachable for real Sets.

## Depends on

- [[features/collection_type_tags|collection_type_tags]]
- [[features/wrapper_pattern|wrapper_pattern]]
- [[features/decode_oop_and_pointers|decode_oop_and_pointers]]
- [[features/klass_introspection|klass_introspection]]

## Related

- [[features/collection_list|collection_list]]
- [[features/collection_map|collection_map]]

## Depended on by

- [[features/collection_iteration_safety|collection_iteration_safety]]

## Implementation anchors

- `field_proxy::value_t::to_vector<element_type>()` — `vmhook/ext/vmhook/vmhook.hpp:15638-15686` — entry point — decodes compressed OOP, guards null, delegates to collection cascade for Set field signatures
- `collection::to_vector<element_type>()` — `vmhook/ext/vmhook/vmhook.hpp:14792-14903` — field-shape cascade routing logic — ArrayList/LinkedList guards, then map→HashSet or m→TreeSet fast paths
- `hash_map_walk_keys<element_type, out_t>` — `vmhook/ext/vmhook/vmhook.hpp:15335-15402` — HashSet/LinkedHashSet walker — decodes HashMap.table bucket array, walks Node.next chains, emits keys
- `tree_map_walk_keys<element_type, out_t>` — `vmhook/ext/vmhook/vmhook.hpp:15528-15622` — TreeSet walker — iterative Morris-free in-order red-black tree walk, yields sorted elements
- `vmhook::find_field(klass, name)` — `vmhook/ext/vmhook/vmhook.hpp:10997-11046` — superclass-walking field resolver with caching — required for treeified-bin inherited fields and field-name collisions
- `get_field_by_oop_klass` — `vmhook/ext/vmhook/vmhook.hpp:14674-14708` — OOP field read via leaf-klass offset resolution — all key/next/left/right/root/table reads decode via this path

## Tests

- `tests/jvm/modules/collection_set.cpp`
- `tests/jvm/modules/collection_set_exhaustive.cpp`

## Known bugs

- **[medium]** `Collections.newSetFromMap(new HashMap<>())` decodes to EMPTY for a non-empty Set (cascade 14863-14872 + tree walker 15539-15543). The wrapper `Collections$SetFromMap` stores its backing map in field `m` (same probe as TreeSet), routing to `tree_map_walk_keys`, which does `find_field(mapKlass, "root")`. HashMap has no root, so the walker returns empty at 15543 with no throw.
- **[medium]** Field-name precedence is structural and fragile for any future Set impl (cascade 14803-14872, resolver walks supers at 11025). Routing by field name alone with no klass-name assertion — e.g. a custom Set with inherited `int size` and `Object[] elementData` would be mis-decoded as an ArrayList.
- **[low]** LinkedHashSet insertion order is silently lost (hash_map_walk_keys 15362-15400 ignores the LinkedHashMap's before/after overlay). Walk is correct as a set (every element once) but in bucket order, not Java-contract insertion order.
- **[low]** Hard element caps can silently truncate a pathological Set (HashSet walker caps each bucket at 1 << 20 nodes at 15368-15370, TreeSet caps total visited at 1 << 24 at 15617-15620). Anti-runaway guards that would silently drop tail on a legitimately enormous Set.
- **[low]** No element-count cross-check against Set.size() — neither walker compares emitted count to the backing map's size field, so partial walk returns a short-but-non-throwing vector that looks like a smaller Set.

## Notes

Compressed OOPs are assumed unconditionally — every node/array read is a 32-bit load + decode;
uncompressed oops (`-XX:-UseCompressedOops`) would read garbage. HashMap.Node field names
`key`/`next` and TreeNode inheritance are stable JDK 8..21+. TreeMap layout (root/Entry/left/right)
stable since Java 1.2. HashSet.map/TreeSet.m field names are JDK-stable. HashMap treeification
thresholds (TREEIFY_THRESHOLD=8, MIN_TREEIFY_CAPACITY=64) are OpenJDK 8+ details the fixture relies on.
String element decode is delegated to read_java_string (Java 9+ compact strings vs Java 8 char[]).
