---
slug: adapter_recovery_c2i
title: Adapter Recovery C2I
category: hook
status: seeded
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/medium, category/hook]
---

# Adapter Recovery C2I

> **Category:** [[categories/hook|Hooking machinery (install / dispatch / trampolines)]]  ·  **Status:** `seeded`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/adapter_recovery_c2i-specialist.md`

## Description

TODO: one-paragraph summary of what this feature does and what its input/output contract is.  Replace this with a real description so a spawned specialist can decide if the feature is relevant in ~200 tokens.

## Depends on

- [[features/method_entry_points_i2i_i2c|method_entry_points_i2i_i2c]]
- [[features/vmstructs_offset_resolution|vmstructs_offset_resolution]]

## Related

- [[features/hook_basic|hook_basic]]
- [[features/hook_common_detour_dispatch|hook_common_detour_dispatch]]
- [[features/hook_install_after_jit|hook_install_after_jit]]
- [[features/method_entry_points_i2i_i2c|method_entry_points_i2i_i2c]]

## Depended on by

- [[features/deoptimize_methods|deoptimize_methods]]
- [[features/hook_install_after_jit|hook_install_after_jit]]

## Notes

Stub manifest — populate hpp_anchors, depends_on, known_bugs as they become known.  See audit/features/schema.md for the field reference.
