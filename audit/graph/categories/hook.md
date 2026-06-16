---
category: hook
title: Hooking machinery (install / dispatch / trampolines)
feature_count: 13
tags: [category/hook]
---

# Hooking machinery (install / dispatch / trampolines)

**13 feature(s) in this category.**

## Features

- [[features/adapter_recovery_c2i|adapter_recovery_c2i]] — `seeded` / `medium` — Adapter Recovery C2I
- [[features/dont_inline_dont_compile|dont_inline_dont_compile]] — `seeded` / `medium` — Dont Inline Dont Compile
- [[features/hook_basic|hook_basic]] — `in_progress` / `critical` — Hook Install (basic)
- [[features/hook_chaining|hook_chaining]] — `in_progress` / `high` — Hook Chaining
- [[features/hook_common_detour_dispatch|hook_common_detour_dispatch]] — `seeded` / `medium` — Hook Common Detour Dispatch
- [[features/hook_install_after_jit|hook_install_after_jit]] — `in_progress` / `critical` — Hook Install After Jit
- [[features/hook_reinstall_after_shutdown|hook_reinstall_after_shutdown]] — `seeded` / `medium` — Hook Reinstall After Shutdown
- [[features/hook_signature|hook_signature]] — `seeded` / `medium` — Hook Signature
- [[features/hook_unhook_double_free|hook_unhook_double_free]] — `in_progress` / `high` — Hook Unhook / Double-Free Guards
- [[features/hook_verify_repair|hook_verify_repair]] — `seeded` / `medium` — Hook Verify Repair
- [[features/method_entry_points_i2i_i2c|method_entry_points_i2i_i2c]] — `in_progress` / `critical` — Method Entry Points I2I / I2C
- [[features/midi2i_trampoline_alloc|midi2i_trampoline_alloc]] — `in_progress` / `critical` — Midi2I Trampoline Alloc
- [[features/seh_invoke_detour|seh_invoke_detour]] — `seeded` / `medium` — Seh Invoke Detour

## Dependency graph

```mermaid
flowchart LR
  subgraph hook["Hooking machinery (install / dispatch / trampolines)"]
    adapter_recovery_c2i([adapter_recovery_c2i])
    dont_inline_dont_compile([dont_inline_dont_compile])
    hook_basic([hook_basic])
    hook_chaining([hook_chaining])
    hook_common_detour_dispatch([hook_common_detour_dispatch])
    hook_install_after_jit([hook_install_after_jit])
    hook_reinstall_after_shutdown([hook_reinstall_after_shutdown])
    hook_signature([hook_signature])
    hook_unhook_double_free([hook_unhook_double_free])
    hook_verify_repair([hook_verify_repair])
    method_entry_points_i2i_i2c([method_entry_points_i2i_i2c])
    midi2i_trampoline_alloc([midi2i_trampoline_alloc])
    seh_invoke_detour([seh_invoke_detour])
  end
  subgraph external["(external deps)"]
    decode_oop_and_pointers[/decode_oop_and_pointers/]
    deoptimize_methods[/deoptimize_methods/]
    find_class_fallback[/find_class_fallback/]
    interpreter_frame_walk[/interpreter_frame_walk/]
    klass_introspection[/klass_introspection/]
    method_explicit_signature[/method_explicit_signature/]
    method_flags_width[/method_flags_width/]
    os_allocate_release[/os_allocate_release/]
    os_page_size_granularity[/os_page_size_granularity/]
    os_protect[/os_protect/]
    os_query_region[/os_query_region/]
    os_signal_handler[/os_signal_handler/]
    shutdown_hooks_teardown[/shutdown_hooks_teardown/]
    signature_parsing[/signature_parsing/]
    vmstructs_offset_resolution[/vmstructs_offset_resolution/]
  end
  adapter_recovery_c2i --> method_entry_points_i2i_i2c
  adapter_recovery_c2i --> vmstructs_offset_resolution
  dont_inline_dont_compile --> method_flags_width
  dont_inline_dont_compile --> hook_basic
  hook_basic --> midi2i_trampoline_alloc
  hook_basic --> hook_common_detour_dispatch
  hook_basic --> method_entry_points_i2i_i2c
  hook_basic --> find_class_fallback
  hook_basic --> klass_introspection
  hook_basic --> decode_oop_and_pointers
  hook_basic --> interpreter_frame_walk
  hook_basic --> signature_parsing
  hook_chaining --> hook_basic
  hook_chaining --> midi2i_trampoline_alloc
  hook_chaining --> hook_common_detour_dispatch
  hook_chaining --> method_entry_points_i2i_i2c
  hook_chaining --> seh_invoke_detour
  hook_chaining --> dont_inline_dont_compile
  hook_common_detour_dispatch --> seh_invoke_detour
  hook_common_detour_dispatch --> interpreter_frame_walk
  hook_common_detour_dispatch --> method_entry_points_i2i_i2c
  hook_install_after_jit --> hook_basic
  hook_install_after_jit --> deoptimize_methods
  hook_install_after_jit --> adapter_recovery_c2i
  hook_install_after_jit --> method_entry_points_i2i_i2c
  hook_reinstall_after_shutdown --> shutdown_hooks_teardown
  hook_reinstall_after_shutdown --> hook_basic
  hook_signature --> hook_basic
  hook_signature --> signature_parsing
  hook_signature --> method_explicit_signature
  hook_unhook_double_free --> hook_basic
  hook_unhook_double_free --> midi2i_trampoline_alloc
  hook_unhook_double_free --> os_protect
  hook_unhook_double_free --> shutdown_hooks_teardown
  hook_verify_repair --> hook_basic
  hook_verify_repair --> midi2i_trampoline_alloc
  method_entry_points_i2i_i2c --> vmstructs_offset_resolution
  midi2i_trampoline_alloc --> os_allocate_release
  midi2i_trampoline_alloc --> os_protect
  midi2i_trampoline_alloc --> os_query_region
  midi2i_trampoline_alloc --> os_page_size_granularity
  midi2i_trampoline_alloc --> hook_common_detour_dispatch
  seh_invoke_detour --> os_signal_handler
```
