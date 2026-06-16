---
category: collection
title: Collection wrappers + element helpers
feature_count: 8
tags: [category/collection]
---

# Collection wrappers + element helpers

**8 feature(s) in this category.**

## Features

- [[features/array_element_helpers|array_element_helpers]] — `in_progress` / `high` — Array Element Helpers
- [[features/collection_hash_tree_map|collection_hash_tree_map]] — `in_progress` / `high` — Collection Hash Tree Map
- [[features/collection_iteration_safety|collection_iteration_safety]] — `in_progress` / `medium` — Collection Iteration Safety
- [[features/collection_linked_list|collection_linked_list]] — `in_progress` / `medium` — Collection Linked List
- [[features/collection_list|collection_list]] — `in_progress` / `medium` — Collection List
- [[features/collection_map|collection_map]] — `in_progress` / `high` — Collection Map
- [[features/collection_set|collection_set]] — `in_progress` / `medium` — Collection Set
- [[features/collection_type_tags|collection_type_tags]] — `in_progress` / `medium` — Collection Type Tags

## Dependency graph

```mermaid
flowchart LR
  subgraph collection["Collection wrappers + element helpers"]
    array_element_helpers([array_element_helpers])
    collection_hash_tree_map([collection_hash_tree_map])
    collection_iteration_safety([collection_iteration_safety])
    collection_linked_list([collection_linked_list])
    collection_list([collection_list])
    collection_map([collection_map])
    collection_set([collection_set])
    collection_type_tags([collection_type_tags])
  end
  subgraph external["(external deps)"]
    decode_oop_and_pointers[/decode_oop_and_pointers/]
    find_class_fallback[/find_class_fallback/]
    klass_introspection[/klass_introspection/]
    wrapper_pattern[/wrapper_pattern/]
  end
  collection_hash_tree_map --> collection_map
  collection_hash_tree_map --> collection_iteration_safety
  collection_hash_tree_map --> decode_oop_and_pointers
  collection_hash_tree_map --> klass_introspection
  collection_iteration_safety --> collection_list
  collection_iteration_safety --> collection_set
  collection_iteration_safety --> collection_map
  collection_linked_list --> collection_list
  collection_linked_list --> collection_iteration_safety
  collection_linked_list --> find_class_fallback
  collection_linked_list --> klass_introspection
  collection_linked_list --> decode_oop_and_pointers
  collection_list --> collection_type_tags
  collection_list --> array_element_helpers
  collection_list --> wrapper_pattern
  collection_list --> decode_oop_and_pointers
  collection_list --> klass_introspection
  collection_map --> collection_type_tags
  collection_map --> wrapper_pattern
  collection_set --> collection_type_tags
  collection_set --> wrapper_pattern
  collection_set --> decode_oop_and_pointers
  collection_set --> klass_introspection
  collection_type_tags --> klass_introspection
  collection_type_tags --> decode_oop_and_pointers
```
