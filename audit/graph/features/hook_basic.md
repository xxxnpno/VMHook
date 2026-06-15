---
slug: hook_basic
title: Hook Install (basic)
category: hook
status: in_progress
risk: critical
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/in_progress, risk/critical, category/hook, tag/hook, tag/runtime, tag/x86_64, tag/install]
---

# Hook Install (basic)

> **Category:** [[categories/hook|Hooking machinery (install / dispatch / trampolines)]]  ·  **Status:** `in_progress`  ·  **Risk:** `critical`  ·  **Specialist:** `.claude/agents/hook_basic-specialist.md`

## Description

Installing a `vmhook::hook<T>` / `vmhook::scoped_hook<T>` on a Java method and
having the detour fire on real bytecode dispatch — on both instance and static
methods — seeing the correct `self`, decoding every argument correctly, and
allowing the original method body to run through.  This is the central
install path; nearly every other hook feature builds on top of it.

## Depends on

- [[features/midi2i_trampoline_alloc|midi2i_trampoline_alloc]]
- [[features/hook_common_detour_dispatch|hook_common_detour_dispatch]]
- [[features/method_entry_points_i2i_i2c|method_entry_points_i2i_i2c]]
- [[features/find_class_fallback|find_class_fallback]]
- [[features/klass_introspection|klass_introspection]]
- [[features/decode_oop_and_pointers|decode_oop_and_pointers]]
- [[features/interpreter_frame_walk|interpreter_frame_walk]]
- [[features/signature_parsing|signature_parsing]]

## Related

- [[features/hook_chaining|hook_chaining]]
- [[features/hook_signature|hook_signature]]
- [[features/hook_install_after_jit|hook_install_after_jit]]
- [[features/hook_verify_repair|hook_verify_repair]]
- [[features/dont_inline_dont_compile|dont_inline_dont_compile]]
- [[features/return_value_cancel|return_value_cancel]]
- [[features/return_set_primitives|return_set_primitives]]

## Depended on by

- [[features/dont_inline_dont_compile|dont_inline_dont_compile]]
- [[features/hook_chaining|hook_chaining]]
- [[features/hook_install_after_jit|hook_install_after_jit]]
- [[features/hook_reinstall_after_shutdown|hook_reinstall_after_shutdown]]
- [[features/hook_signature|hook_signature]]
- [[features/hook_unhook_double_free|hook_unhook_double_free]]
- [[features/hook_verify_repair|hook_verify_repair]]
- [[features/on_class_loaded|on_class_loaded]]
- [[features/on_exception|on_exception]]
- [[features/shutdown_hooks_teardown|shutdown_hooks_teardown]]

## Implementation anchors

- `vmhook::hook<T>(name, callback)` — `vmhook/ext/vmhook/vmhook.hpp:7835-7840` — thin overload — forwards with empty signature
- `vmhook::hook<T>(name, signature, callback)` — `vmhook/ext/vmhook/vmhook.hpp:7850-8125` — real install routine
- `common_detour` — `vmhook/ext/vmhook/vmhook.hpp:5846-5912` — dispatch entrypoint — looks up Method*, scans g_hooked_methods, fires once via seh_invoke_detour
- `scoped_hook<T>` — `vmhook/ext/vmhook/vmhook.hpp:8733-8830` — RAII wrapper that calls hook<T>() and re-resolves Method* for a hook_handle
- `hook_handle::stop` — `vmhook/ext/vmhook/vmhook.hpp:8614-8675` — uninstall path — erases entry + clears _dont_inline/NO_COMPILE (does NOT restore _code)
- `extract_frame_arg<T>` — `vmhook/ext/vmhook/vmhook.hpp:7325-7401` — per-arg interpreter-slot decode used by the typed callback path

## Tests

- `tests/jvm/modules/hook_basic.cpp`

## Audit docs

- `audit/findings/hook_basic_install.md`

## Known bugs

- **[high]** Half-installed method permanently poisons re-install — push_back at 7982 happens before i2i install (8000) / midi2i_hook (8047-8051); on failure the entry is never erased, so every later hook<T>() short-circuits at 7908-7914 and returns true without installing.  (`audit/findings/hook_basic_install.md`)
- **[high]** push_back can realloc g_hooked_methods while a sibling detour iterates it lock-free (7982 vs 5883) — UAF on hook.detour std::function cell.  Install mutex (7906) does not cover the reader.
- **[high]** Auto-repair watchdog is dead after shutdown_hooks() + re-init (8116 ensure_started, g_shutdown_requested at ~8554 / g_started never reset) — first common_detour sees the shutdown flag (5853) and bails forever.
- **[medium]** _dont_inline silently no-ops if Method._flags VMStruct is absent (set_dont_inline path), while NO_COMPILE via get_access_flags throws — half-applied inline guard with no diagnostic on future / patched JVMs.
- **[low]** Pre-install Method-flag mutations (NO_COMPILE / _dont_inline, 7916-7923) leak on exception — no rollback, asymmetric with hook_handle::stop().

## Notes

Reference seed manifest — populated from .claude/agents/hook_basic-specialist.md and
audit/LIBRARY_BUGS.md to demonstrate the schema's fully-loaded shape.  Other
manifests stay stubbed until their specialist populates them.
