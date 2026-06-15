---
slug: hook_install_after_jit
title: Hook Install After Jit
category: hook
status: seeded
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/medium, category/hook]
---

# Hook Install After Jit

> **Category:** [[categories/hook|Hooking machinery (install / dispatch / trampolines)]]  ·  **Status:** `seeded`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/hook_install_after_jit-specialist.md`

## Description

TODO: one-paragraph summary of what this feature does and what its input/output contract is.  Replace this with a real description so a spawned specialist can decide if the feature is relevant in ~200 tokens.

## Depends on

- [[features/hook_basic|hook_basic]]
- [[features/deoptimize_methods|deoptimize_methods]]
- [[features/method_entry_points_i2i_i2c|method_entry_points_i2i_i2c]]
- [[features/adapter_recovery_c2i|adapter_recovery_c2i]]

## Tests

- `tests/jvm/modules/hook_install_after_jit.cpp`

## Notes

Stub manifest — populate hpp_anchors, depends_on, known_bugs as they become known.  See audit/features/schema.md for the field reference.
