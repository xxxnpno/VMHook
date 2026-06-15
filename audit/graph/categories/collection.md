---
category: collection
title: Collection wrappers + element helpers
feature_count: 8
tags: [category/collection]
---

# Collection wrappers + element helpers

**8 feature(s) in this category.**

## Features

- [[features/array_element_helpers|array_element_helpers]] — `seeded` / `medium` — Array Element Helpers
- [[features/collection_hash_tree_map|collection_hash_tree_map]] — `seeded` / `medium` — Collection Hash Tree Map
- [[features/collection_iteration_safety|collection_iteration_safety]] — `seeded` / `medium` — Collection Iteration Safety
- [[features/collection_linked_list|collection_linked_list]] — `seeded` / `medium` — Collection Linked List
- [[features/collection_list|collection_list]] — `seeded` / `medium` — Collection List
- [[features/collection_map|collection_map]] — `seeded` / `medium` — Collection Map
- [[features/collection_set|collection_set]] — `seeded` / `medium` — Collection Set
- [[features/collection_type_tags|collection_type_tags]] — `seeded` / `medium` — Collection Type Tags

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
    klass_introspection[/klass_introspection/]
    wrapper_pattern[/wrapper_pattern/]
  end
  collection_hash_tree_map --> collection_map
  collection_hash_tree_map --> collection_iteration_safety
  collection_iteration_safety --> collection_list
  collection_iteration_safety --> collection_set
  collection_iteration_safety --> collection_map
  collection_linked_list --> collection_list
  collection_linked_list --> collection_iteration_safety
  collection_list --> collection_type_tags
  collection_list --> array_element_helpers
  collection_list --> wrapper_pattern
  collection_map --> collection_type_tags
  collection_map --> wrapper_pattern
  collection_set --> collection_type_tags
  collection_set --> wrapper_pattern
  collection_type_tags --> klass_introspection
```
