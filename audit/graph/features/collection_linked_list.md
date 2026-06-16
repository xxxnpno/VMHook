---
slug: collection_linked_list
title: Collection Linked List
category: collection
status: in_progress
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/in_progress, risk/medium, category/collection, tag/jvm, tag/collection, tag/memory-safe, tag/field-walk]
---

# Collection Linked List

> **Category:** [[categories/collection|Collection wrappers + element helpers]]  ·  **Status:** `in_progress`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/collection_linked_list-specialist.md`

## Description

Reading a live `java.util.LinkedList` by walking its `first -> next` Node
chain — the O(N) fast path inside `collection::to_vector<T>()` instead of
the generic `List.get(int)` fallback (O(N^2) per LinkedList semantics). Three
independent user paths: `field_proxy::value_t::to_vector<T>()` (documented);
`std::unique_ptr<vmhook::linked_list>` wrapper; and free function
`vmhook::linked_list_walk_items<T>(list_oop, size, out)`. Returns elements
as `std::vector<std::unique_ptr<T>>` in strict insertion order with all null
slots preserved as `nullptr` and every dereference pointer-validated.

## Depends on

- [[features/collection_list|collection_list]]
- [[features/collection_iteration_safety|collection_iteration_safety]]
- [[features/find_class_fallback|find_class_fallback]]
- [[features/klass_introspection|klass_introspection]]
- [[features/decode_oop_and_pointers|decode_oop_and_pointers]]

## Related

- [[features/collection_hash_tree_map|collection_hash_tree_map]]
- [[features/collection_iteration_safety|collection_iteration_safety]]
- [[features/collection_list|collection_list]]

## Implementation anchors

- `vmhook::linked_list` — `vmhook/ext/vmhook/vmhook.hpp:14963-14970` — type-tag wrapper (pure inheritance from collection; no added behavior)
- `collection::to_vector<T>()` — `vmhook/ext/vmhook/vmhook.hpp:14792-14903` — dispatch cascade selecting ArrayList/LinkedList/HashSet/TreeSet/fallback based on field presence
- `vmhook::linked_list_walk_items<element_type, out_t>()` — `vmhook/ext/vmhook/vmhook.hpp:15182-15238` — core chain walk — reads first, loops over nodes, reads item+next per node, validates all pointers
- `collection::get_field_by_oop_klass(name)` — `vmhook/ext/vmhook/vmhook.hpp:14674-14708` — live-OOP field resolver for branch-selection probe (first/size/elementData)
- `vmhook::find_field(klass, name)` — `vmhook/ext/vmhook/vmhook.hpp:10997-11046` — field-name resolver with cache (misses never memoized; see flaw #4)
- `vmhook::klass_from_oop(oop)` — `vmhook/ext/vmhook/vmhook.hpp:14597-14611` — reads narrow klass at oop+8; hard-wired to compressed class pointers (flaw #1)
- `field_proxy::value_t::to_vector<T>()` — `vmhook/ext/vmhook/vmhook.hpp:15638-15686` — documented user path — decodes field OOP, constructs collection, delegates to collection::to_vector
- `value_t to unique_ptr<linked_list> conversion` — `vmhook/ext/vmhook/vmhook.hpp:11821-11844` — wrapper constructor path — rejects non-L signatures, validates/decodes OOP, new linked_list
- `vmhook::read_java_string(oop)` — `vmhook/ext/vmhook/vmhook.hpp:15723-15855` — element content decoder for String elements (JDK8 char[] vs JDK9+ coder byte[])

## Tests

- `tests/jvm/modules/collection_linked_list.cpp`

## Known bugs

- **[high]** Entire walk hard-codes narrow (32-bit) OOP reads (15199-15202, 15220-15236) and narrow klass at oop+8 (14603-14604); under -XX:-UseCompressedOops/-XX:-UseCompressedClassPointers (heap > ~32 GB default), reads pick up half-pointers yielding garbage/wild addresses with no diagnostic.
- **[medium]** No cycle/length-overrun detection; chain LONGER than size silently truncated (15205-15207); unsynchronised size read (14840) vs walk can miss elements if list mutated between reads.
- **[medium]** Empty vector indistinguishable from decode failure (14809-14812, 14841); genuinely empty LinkedList and read error both return {} with no API signal.
- **[medium]** find_field never memoizes misses (11025-11045); elementData absence probe re-scans full super chain on every LinkedList read, defeating the field cache (pure perf defect).
- **[low]** Per-node re-resolution of item/next redundant despite doc comment claiming cached offsets (15214-15215 calls find_field every iteration; comment at 15176-15178 overstates optimisation).
- **[low]** read_java_string silently returns empty string for decoded length 0 or > 4096 (15763-15769); empty/corrupt element indistinguishable, >4096-char element silently dropped.

## Notes

Field names (`first`, `size`, `elementData`, `item`, `next`) stable across JDK 8–25.
Compressed oops assumption (flaw #1) is global vmhook; LinkedList has zero fallback.
Element decode via read_java_string branches JDK8 char[] vs JDK9+ coder byte[]
(15772); fixture uses short ASCII but real long/empty strings mis-read silently.
Test covers three independent paths (value_t/wrapper/free-function) converging on
same 3 ordered elements to prove branch selection and chain walk both correct.
