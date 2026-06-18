---
slug: method_entry_points_i2i_i2c
title: Method Entry Points I2I I2C
category: hook
status: seeded
risk: critical
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/critical, category/hook, tag/hook, tag/method, tag/entry-point, tag/i2i, tag/c2i, tag/adapter, tag/deopt, tag/vmstructs, tag/x86_64]
---

# Method Entry Points I2I I2C

> **Category:** [[categories/hook|Hooking machinery (install / dispatch / trampolines)]]  ·  **Status:** `seeded`  ·  **Risk:** `critical`  ·  **Specialist:** `.claude/agents/method_entry_points_i2i_i2c-specialist.md`

## Description

The HotSpot `Method` entry-point accessor layer every hook install, deopt,
re-anchor, watchdog repair, and JNI-fallback call routes through: the
snapshot/restore of `Method::_i2i_entry`, `_from_interpreted_entry`,
`_from_compiled_entry` (a.k.a. `_from_compiled_code_entry_point`), `_code`, plus
the c2i-adapter recovery (`Method::_adapter` -> `AdapterHandlerEntry::_c2i_entry`)
that makes deoptimisation of a JIT-compiled method possible. This is the "FIX C"
area: when a hooked method is compiled, the only way the patched interpreter
(i2i) stub becomes reachable again is to write `_from_interpreted_entry = i2i`,
`_from_compiled_entry = c2i`, then `_code = nullptr` — IN THAT ORDER. Each
accessor resolves its field offset once via `iterate_struct_entries("Method",…)`
and degrades to nullptr / a silent no-op when the VMStruct is absent. The
`_from_compiled_entry` accessor dual-name-resolves (JDK <=20 vs 21+), and
`get_adapter` has a heuristic offset-scan slow path for JDK 9+ where `_adapter`
was dropped from VMStructs.

## Depends on

- [[features/vmstructs_offset_resolution|vmstructs_offset_resolution]]
- [[features/iterate_entries_safety|iterate_entries_safety]]

## Related

- [[features/adapter_recovery_c2i|adapter_recovery_c2i]]
- [[features/deoptimize_methods|deoptimize_methods]]
- [[features/hook_basic|hook_basic]]
- [[features/hook_verify_repair|hook_verify_repair]]
- [[features/classloader_reanchor|classloader_reanchor]]
- [[features/midi2i_trampoline_alloc|midi2i_trampoline_alloc]]
- [[features/method_flags_width|method_flags_width]]

## Depended on by

- [[features/adapter_recovery_c2i|adapter_recovery_c2i]]
- [[features/deoptimize_methods|deoptimize_methods]]
- [[features/hook_basic|hook_basic]]
- [[features/hook_common_detour_dispatch|hook_common_detour_dispatch]]
- [[features/hook_install_after_jit|hook_install_after_jit]]

## Implementation anchors

- `method::get_i2i_entry / get_from_interpreted_entry` — `vmhook/ext/vmhook/vmhook.hpp:2634-2720` — _i2i_entry (2634, the hook-location target) and _from_interpreted_entry (2683); throw->catch->nullptr on a missing VMStruct
- `method::get_from_compiled_entry / set_from_compiled_entry (dual-name)` — `vmhook/ext/vmhook/vmhook.hpp:3217-3310` — noexcept; tries _from_compiled_code_entry_point (JDK <=20) then _from_compiled_entry (JDK 21+); set at 3262, silent no-op if absent
- `method::get_code / set_code / set_from_interpreted_entry` — `vmhook/ext/vmhook/vmhook.hpp:3105-3210` — get_code 3105, set_code(nullptr) is the deopt trigger 3157, set_from_interpreted_entry 3188
- `method::get_adapter (c2i recovery core)` — `vmhook/ext/vmhook/vmhook.hpp:3311-3430` — fast path Method::_adapter via VMStructs (JDK 8); slow path (JDK 9+) heuristic offset scan cached in a static atomic with 0 = not-yet-found sentinel (deliberate re-probe; caching failure as SIZE_MAX was disastrous)
- `detect_adapter_offset_from_method / validate_adapter_handler_entry / get_c2i_entry_from_adapter` — `vmhook/ext/vmhook/vmhook.hpp:7640-7900` — get_c2i_entry_from_adapter 7640 (reads AdapterHandlerEntry::_c2i_entry, all JDK 8-26); validate_adapter_handler_entry 7789 (committed+executable check); detect_adapter_offset_from_method 7829 (byte-scan, prefer slot before _from_compiled_entry)
- `method::get_access_flags / get_flags (mutated in lockstep)` — `vmhook/ext/vmhook/vmhook.hpp:2728-2980` — NO_COMPILE (u32 _access_flags, 2728) and _dont_inline (u16 _flags, 2938) are set/cleared together with the entry points at every install/deopt/restore

## Notes

No dedicated test module yet — the accessors are exercised transitively by every
hook install / deopt / verify-repair / reanchor path. The consumer ordering is
load-bearing: deopt of a compiled target recovers c2i then sets fie=i2i, fce=c2i,
code=nullptr; the forced-deopt fallback (c2i unrecoverable) sets fie=i2i,
code=nullptr only. Each accessor degrades to nullptr (getters) or a silent no-op
(setters) on a missing VMStruct rather than crashing. JDK sensitivities:
_from_compiled_entry was renamed from _from_compiled_code_entry_point (dual-name
resolve); Method::_adapter is exported via VMStructs only on JDK 8 and recovered
heuristically on JDK 9+; the heuristic offset scan re-probes on every call until a
Method validates (the 0-sentinel design). x86_64-only, HotSpot-only. risk is
critical: a wrong entry-point write breaks the hooking machinery globally.
