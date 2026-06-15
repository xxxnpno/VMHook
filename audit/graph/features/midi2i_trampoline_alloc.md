---
slug: midi2i_trampoline_alloc
title: Midi2I Trampoline Alloc
category: hook
status: seeded
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/medium, category/hook]
---

# Midi2I Trampoline Alloc

> **Category:** [[categories/hook|Hooking machinery (install / dispatch / trampolines)]]  ·  **Status:** `seeded`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/midi2i_trampoline_alloc-specialist.md`

## Description

TODO: one-paragraph summary of what this feature does and what its input/output contract is.  Replace this with a real description so a spawned specialist can decide if the feature is relevant in ~200 tokens.

## Depends on

- [[features/os_allocate_release|os_allocate_release]]
- [[features/os_protect|os_protect]]
- [[features/os_query_region|os_query_region]]
- [[features/os_page_size_granularity|os_page_size_granularity]]

## Depended on by

- [[features/hook_basic|hook_basic]]
- [[features/hook_chaining|hook_chaining]]
- [[features/hook_verify_repair|hook_verify_repair]]

## Notes

Stub manifest — populate hpp_anchors, depends_on, known_bugs as they become known.  See audit/features/schema.md for the field reference.
