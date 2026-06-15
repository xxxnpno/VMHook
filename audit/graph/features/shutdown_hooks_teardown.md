---
slug: shutdown_hooks_teardown
title: Shutdown Hooks Teardown
category: lifecycle
status: seeded
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/medium, category/lifecycle]
---

# Shutdown Hooks Teardown

> **Category:** [[categories/lifecycle|Lifecycle hooks (shutdown / class-load / exception / enum)]]  ·  **Status:** `seeded`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/shutdown_hooks_teardown-specialist.md`

## Description

TODO: one-paragraph summary of what this feature does and what its input/output contract is.  Replace this with a real description so a spawned specialist can decide if the feature is relevant in ~200 tokens.

## Depends on

- [[features/hook_basic|hook_basic]]

## Related

- [[features/on_class_loaded|on_class_loaded]]
- [[features/on_exception|on_exception]]

## Depended on by

- [[features/hook_reinstall_after_shutdown|hook_reinstall_after_shutdown]]
- [[features/hook_unhook_double_free|hook_unhook_double_free]]

## Tests

- `tests/jvm/modules/shutdown_hooks_teardown.cpp`

## Notes

Stub manifest — populate hpp_anchors, depends_on, known_bugs as they become known.  See audit/features/schema.md for the field reference.
