---
slug: for_each_thread
title: For Each Thread
category: enumeration
status: seeded
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/medium, category/enumeration]
---

# For Each Thread

> **Category:** [[categories/enumeration|Live-VM enumeration (heap / classes / threads)]]  ·  **Status:** `seeded`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/for_each_thread-specialist.md`

## Description

TODO: one-paragraph summary of what this feature does and what its input/output contract is.  Replace this with a real description so a spawned specialist can decide if the feature is relevant in ~200 tokens.

## Depends on

- [[features/iterate_entries_safety|iterate_entries_safety]]

## Related

- [[features/for_each_instance|for_each_instance]]
- [[features/for_each_loaded_class|for_each_loaded_class]]

## Tests

- `tests/jvm/modules/for_each_thread.cpp`

## Notes

Stub manifest — populate hpp_anchors, depends_on, known_bugs as they become known.  See audit/features/schema.md for the field reference.
