---
slug: hook_signature
title: Hook Signature
category: hook
status: seeded
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/medium, category/hook]
---

# Hook Signature

> **Category:** [[categories/hook|Hooking machinery (install / dispatch / trampolines)]]  ·  **Status:** `seeded`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/hook_signature-specialist.md`

## Description

TODO: one-paragraph summary of what this feature does and what its input/output contract is.  Replace this with a real description so a spawned specialist can decide if the feature is relevant in ~200 tokens.

## Depends on

- [[features/hook_basic|hook_basic]]
- [[features/signature_parsing|signature_parsing]]
- [[features/method_explicit_signature|method_explicit_signature]]

## Related

- [[features/dont_inline_dont_compile|dont_inline_dont_compile]]
- [[features/hook_basic|hook_basic]]
- [[features/hook_chaining|hook_chaining]]
- [[features/hook_install_after_jit|hook_install_after_jit]]
- [[features/hook_reinstall_after_shutdown|hook_reinstall_after_shutdown]]
- [[features/hook_unhook_double_free|hook_unhook_double_free]]

## Tests

- `tests/jvm/modules/hook_signature.cpp`

## Notes

Stub manifest — populate hpp_anchors, depends_on, known_bugs as they become known.  See audit/features/schema.md for the field reference.
