---
slug: hook_signature
title: Hook Signature
category: hook
status: in_progress
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/in_progress, risk/medium, category/hook, tag/hook, tag/signature, tag/overload-selection, tag/jvm-descriptor, tag/return-type-in-descriptor, tag/slot-table, tag/install]
---

# Hook Signature

> **Category:** [[categories/hook|Hooking machinery (install / dispatch / trampolines)]]  ·  **Status:** `in_progress`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/hook_signature-specialist.md`

## Description

The signature-filtered interpreter-hook overloads `vmhook::hook<T>(name, signature, detour)` and their
`scoped_hook<T>` twin — purpose: OVERLOAD SELECTION. Install a detour on EXACTLY ONE of several
same-named Java methods by JVM descriptor, leaving the sibling overloads un-hooked. The empty-filter
convenience overload thin-wraps the signature one with `std::string_view{}`. The selection loop linearly
scans the InstanceKlass `_methods` array and accepts the first entry whose `get_name()` matches AND
(`signature.empty()` OR `get_signature() == signature`). The descriptor is the FULL JVM descriptor
including the return type ((I)I != (I)V). Crucially the arg-decode slot table is derived from the
callback's C++ tuple (long/double take two slots), NOT from the descriptor string — the descriptor
selects the method while the lambda's C++ types drive slot widths, and a mismatch between the two is
undiagnosed.

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

## Implementation anchors

- `hook<T>(name, signature, detour, already_hooked)` — `vmhook/ext/vmhook/vmhook.hpp:10089-10407` — the real signature-filtered install routine — selection loop, JIT-inhibit + state mutation, wrapper_detour build, push to g_hooked_methods, i2i reuse/alloc, deopt; catch-all returns false
- `hook<T>(name, detour) empty-filter overload` — `vmhook/ext/vmhook/vmhook.hpp:10074-10078` — convenience overload — thin-wraps the signature form by passing std::string_view{} (matches the first same-named method regardless of descriptor)
- `overload-selection loop` — `vmhook/ext/vmhook/vmhook.hpp:10151-10156` — linear scan of the InstanceKlass _methods array — accepts first method_ptr whose get_name() == name AND (signature.empty() OR get_signature() == signature)
- `method::get_signature() (string)` — `vmhook/ext/vmhook/vmhook.hpp:3065-3120` — the descriptor source compared in the loop — defensive ConstMethod/Symbol decode that returns "" on any read error (full JVM descriptor incl. return type)
- `scoped_hook<T>(name, signature, detour) re-resolve loop` — `vmhook/ext/vmhook/vmhook.hpp:11262-11370` — calls hook<T> then RE-RESOLVES the Method* with its own copy of the same name+sig loop (filter at 11345) to build a hook_handle; honest duplicate-install returns an empty handle

## Tests

- `tests/jvm/modules/hook_signature.cpp`

## Audit docs

- `audit/findings/hook_explicit_signature_install.md`

## Known bugs

- **[medium]** Mid-install failure leaks NO_COMPILE + a dead g_hooked_methods entry (shared with hook_basic): the selection loop sets _dont_inline, ORs in NO_COMPILE, and push_back's to g_hooked_methods BEFORE find_hook_location / midi2i_hook can throw. On a throw the catch returns false but the flag mutations + pushed entry are never rolled back, permanently inhibiting JIT and poisoning re-install for that Method*.
- **[medium]** Descriptor selects the method but the C++ callback tuple drives the decode slot widths, and a MISMATCH is undiagnosed. A descriptor like (J)V selects a long-taking method, but if the lambda declares (int) the slot table reads one slot for what is a two-slot long — the trailing arg / receiver decodes from the wrong slot with no diagnostic. The descriptor compare and the slot table are independent (descriptor matched against get_signature(), slots derived from java_slot_offsets<tuple>).
- **[low]** The descriptor compare is byte-exact against the full JVM descriptor INCLUDING return type, with no normalization: (I)I != (I)V, a dotted or whitespace-padded descriptor never matches, and a near-miss silently selects no method (the loop falls through to a not-found return). Correct but a sharp edge — a typo'd descriptor is indistinguishable from a genuinely-absent overload.

## Notes

function_traits deduces the callback's arg tuple; the leading return_value& is stripped, leaving the
Java-visible params. For instance methods the first remaining arg is the implicit this (a
unique_ptr<wrapper>) at slot 0; for static methods params begin at slot 0 with no this. The descriptor
string is matched ONLY against Method::get_signature() (full JVM descriptor incl. return type). The
empty-filter overload matches the first same-named method regardless of descriptor (the classic
ambiguity when same-named overloads exist). JDK 8 emits an extra synthetic ()V nested-class accessor —
irrelevant to signature selection but a known descriptor-count wrinkle. Hooks via scoped_hook<T> that
disarm on scope exit; the module proves exactly-one-overload selection (the sibling overloads run
unhooked), leaves NOTHING armed, and NEVER calls shutdown_hooks(). Tests cover JDK 8..26 HotSpot.
Java-8-only fixture; MSVC copy-init; every Method* / decoded-OOP deref gated by is_valid_pointer.
