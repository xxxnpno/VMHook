---
slug: adapter_recovery_c2i
title: Adapter Recovery C2I
category: hook
status: in_progress
risk: high
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/in_progress, risk/high, category/hook, tag/jvm, tag/safety, tag/deopt, tag/heuristic, tag/recovery]
---

# Adapter Recovery C2I

> **Category:** [[categories/hook|Hooking machinery (install / dispatch / trampolines)]]  ·  **Status:** `in_progress`  ·  **Risk:** `high`  ·  **Specialist:** `.claude/agents/adapter_recovery_c2i-specialist.md`

## Description

Recovers a HotSpot Method's AdapterHandlerEntry pointer (Method::_adapter) and
its compiled-to-interpreter (c2i) stub entry. On JDK 8 the field is exported via
gHotSpotVMStructs; on JDK 9+ it was dropped, so recovery uses a heuristic scan
over Method bytes validated against the AHE's _i2c_entry/_c2i_entry pointing into
executable code-cache memory. This is the load-bearing engine for deoptimizing
JIT-compiled methods: when c2i recovery fails, deopt paths silently degrade to
forced fallback or skip the method entirely, leaving compiled callers bypassing
the hook.

## Depends on

- [[features/method_entry_points_i2i_i2c|method_entry_points_i2i_i2c]]
- [[features/vmstructs_offset_resolution|vmstructs_offset_resolution]]
- [[features/os_query_region|os_query_region]]

## Related

- [[features/hook_basic|hook_basic]]
- [[features/hook_common_detour_dispatch|hook_common_detour_dispatch]]
- [[features/hook_install_after_jit|hook_install_after_jit]]
- [[features/method_entry_points_i2i_i2c|method_entry_points_i2i_i2c]]

## Depended on by

- [[features/deoptimize_methods|deoptimize_methods]]
- [[features/hook_install_after_jit|hook_install_after_jit]]

## Implementation anchors

- `detect_adapter_offset_from_method` — `vmhook/ext/vmhook/vmhook.hpp:2154-2155` — forward declaration of the heuristic offset recovery
- `method::get_adapter()` — `vmhook/ext/vmhook/vmhook.hpp:2499-2548` — public entry point — fast path (JDK 8 export) or slow path (JDK 9+ heuristic scan with process-wide cache)
- `get_c2i_entry_from_adapter(void* adapter)` — `vmhook/ext/vmhook/vmhook.hpp:6084-6098` — reads _c2i_entry offset from recovered AHE
- `validate_adapter_handler_entry(candidate, c2i_offset_in_ahe)` — `vmhook/ext/vmhook/vmhook.hpp:6115-6143` — AHE-shape oracle — validates _i2c_entry at offset 0 and _c2i_entry at exported offset both in executable memory
- `detect_adapter_offset_from_method(method* probe)` — `vmhook/ext/vmhook/vmhook.hpp:6155-6251` — main heuristic scan — preferred-guess slot before _from_compiled_entry, fallback full scan with skip set
- `per-hook install deopt (was_compiled branch)` — `vmhook/ext/vmhook/vmhook.hpp:8251-8287` — consumer with forced-deopt fallback on unrecoverable c2i
- `deoptimize_methods_if / deoptimize_all_jit_compiled_methods` — `vmhook/ext/vmhook/vmhook.hpp:6510-6573` — sweep consumer that skips methods when c2i unrecoverable (no forced fallback)
- `os::region_info` — `vmhook/ext/vmhook/vmhook.hpp:462-471` — committed/executable fields for validation
- `os::query_region` — `vmhook/ext/vmhook/vmhook.hpp:737-800` — checks if a pointer lands in committed+executable memory

## Known bugs

- **[high]** _i2c_entry-at-offset-0 is hardcoded unverified ABI assumption (vmhook.hpp:6122-6125) — _c2i_entry offset is resolved from VMStructs but _i2c_entry is raw memcpy at offset 0; if future JDK reorders AHE every real recovery fails silently with no diagnostic.
- **[high]** Skip-set overflows silently and stops excluding real Method fields (vmhook.hpp:6182-6191) — std::array<…,8> with add_skip early-return at size 8; six fields added today but no bounds checking or assertion on overflow, risking scan returning wrong offset.
- **[medium]** False-positive validation can return non-_adapter offset that merely looks like AHE (vmhook.hpp:6115-6143 + 6229-6248) — purely structural validation walks every 8-byte Method slot, returns first valid match, and caches process-wide; any slot holding two code-cache pointers succeeds.
- **[medium]** Process-wide cached offset assumes single uniform Method layout (vmhook.hpp:2530-2540) — one global std::size_t with no per-Method/per-JVM keying; early in-flight Method with different offset poisons all later reads, dual of the failure-caching bug at 2511-2529.
- **[medium]** deoptimize_methods_if has no forced-deopt fallback unlike per-hook install (vmhook.hpp:6550-6556 vs 8267-8286) — sweep API silently leaves JIT-compiled methods intact when c2i unrecoverable, asymmetric behavioural trap.
- **[low]** query_region 'executable' requirement may be too strict on locked-down JITs (vmhook.hpp:6139-6142) — W^X/dual-mapped code cache could query non-executable through chosen mapping, failing legitimate AHE validation.
- **[low]** noexcept design swallows recovery failures invisibly (get_adapter 2499, get_c2i_entry_from_adapter 6084, validate 6115, detect 6155 all noexcept) — callers cannot distinguish 'no adapter' from 'recovery failed', only surface downstream as hooks not firing on compiled callers.

## Notes

JDK 8 exports Method::_adapter via gHotSpotVMStructs, so fast path (2502-2510) returns immediately
and heuristic scan never runs — flaws #1-#4 manifest only on JDK 9+. JDK 9+ entirely depends on the
heuristic scan (2139-2140, 8241-8244). JDK 17/21/24/25 have _adapter immediately before
_from_compiled_entry, optimized by preferred-guess (6202-6214); JDK 26's _c2i_entry export (claimed at
6081-6082) must be verified by actual CI run. AHE layout assumption (_i2c_entry at offset 0, flaw #1)
has held across JDK 9-25 but is most likely to break on future JDK or vendor JVM re-layout. CRITICAL
MISSING TEST: no dedicated unit or JVM test exists; feature is only transitively exercised by
deoptimize_methods.cpp and hook_install_after_jit.cpp as [INFO]-safe skips, so a total heuristic
regression on JDK 9+ would not turn either red. Exhaustive plan needed: 15 pure-logic tests
(test_adapter_recovery_c2i.cpp) + 8 live-JVM tests (jvm/modules/adapter_recovery_c2i.cpp), with
hard assertions on recovery success per JDK (#16/#21) and skip-set/first-match heuristic pinned (#11/#12).
