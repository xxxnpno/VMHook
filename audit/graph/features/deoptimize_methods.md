---
slug: deoptimize_methods
title: Deoptimize Methods
category: deopt
status: seeded
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/medium, category/deopt]
---

# Deoptimize Methods

> **Category:** [[categories/deopt|De-optimisation]]  ·  **Status:** `seeded`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/deoptimize_methods-specialist.md`

## Description

TODO: one-paragraph summary of what this feature does and what its input/output contract is.  Replace this with a real description so a spawned specialist can decide if the feature is relevant in ~200 tokens.

## Depends on

- [[features/method_enumeration|method_enumeration]]
- [[features/adapter_recovery_c2i|adapter_recovery_c2i]]
- [[features/method_entry_points_i2i_i2c|method_entry_points_i2i_i2c]]

## Depended on by

- [[features/hook_install_after_jit|hook_install_after_jit]]

## Tests

- `tests/jvm/modules/deoptimize_methods.cpp`

## Notes

Stub manifest — populate hpp_anchors, depends_on, known_bugs as they become known.  See audit/features/schema.md for the field reference.
