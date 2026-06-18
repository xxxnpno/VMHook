---
slug: return_value_cancel
title: Return Value Cancel
category: return
status: seeded
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/medium, category/return, tag/return, tag/x86_64]
---

# Return Value Cancel

> **Category:** [[categories/return|return_value (detour-side return manipulation)]]  ·  **Status:** `seeded`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/return_value_cancel-specialist.md`

## Description

return_value::cancel() — suppresses the hooked method's original body without
supplying a replacement return value.  It sets the return_slot's cancel flag to
true (leaving retval at its zero default); the trampoline checks cancel after the
callback returns and short-circuits to the stored retval instead of running the
original method.  This is the void-method counterpart to set<T>(value): use
cancel() when the method returns void (or when a zero/null return is acceptable),
set() when a specific value must be returned.  If neither cancel() nor set() is
called, the original method runs normally.  A bare flag write — no interpreter
frame walk, no allocation, noexcept — so it is valid regardless of whether the
return_value carries a frame.

## Related

- [[features/return_caller|return_caller]]
- [[features/return_frame_raw_access|return_frame_raw_access]]
- [[features/return_set_arg|return_set_arg]]
- [[features/return_set_primitives|return_set_primitives]]
- [[features/return_set_wrapper_null|return_set_wrapper_null]]
- [[features/return_stack_trace_depth|return_stack_trace_depth]]

## Implementation anchors

- `vmhook::return_value::cancel` — `vmhook/ext/vmhook/vmhook.hpp:1411-1415` — sets return_slot->cancel = true; leaves retval at zero
- `vmhook::hotspot::return_slot` — `vmhook/ext/vmhook/vmhook.hpp:1313-1317` — the {cancel, retval} cell the trampoline reads after the callback

## Tests

- `tests/jvm/modules/return_value_cancel.cpp`
- `tests/test_return_value.cpp`

## Notes

Bare cancel-flag write; test_return_value.cpp exercises it with no JVM.  The JVM
module confirms the trampoline actually short-circuits the original body.
