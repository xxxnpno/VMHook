---
slug: dont_inline_dont_compile
title: Dont Inline Dont Compile
category: hook
status: in_progress
risk: critical
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/in_progress, risk/critical, category/hook, tag/jit-inhibitor, tag/method-flags, tag/compiler-guard, tag/interpreter-dispatch]
---

# Dont Inline Dont Compile

> **Category:** [[categories/hook|Hooking machinery (install / dispatch / trampolines)]]  ·  **Status:** `in_progress`  ·  **Risk:** `critical`  ·  **Specialist:** `.claude/agents/dont_inline_dont_compile-specialist.md`

## Description

Setting and clearing the `_dont_inline` bit in `Method::_flags` (bit 2,
JDK 13-20) and `NO_COMPILE` in `Method::_access_flags` on a hooked Method
so the JIT never inlines or compiles callers past the i2i interpreter patch.
Input: live Method* retrieved from a hooked class. Output: hooked method
fires its interpreter detour on every dispatch, even after JIT threshold is
exceeded, until `shutdown_hooks()` or `hook_handle::stop()` clears both flags.
The `_dont_inline` readback observability is JDK-version-sensitive (width hazard
on JDK 8-12 and JDK 21+) and tested best-effort; NO_COMPILE sets are universally
enforced.

## Depends on

- [[features/method_flags_width|method_flags_width]]
- [[features/hook_basic|hook_basic]]
- [[features/hook_common_detour_dispatch|hook_common_detour_dispatch]]

## Related

- [[features/hook_chaining|hook_chaining]]
- [[features/hook_install_after_jit|hook_install_after_jit]]
- [[features/hook_reinstall_after_shutdown|hook_reinstall_after_shutdown]]
- [[features/hook_signature|hook_signature]]
- [[features/hook_unhook_double_free|hook_unhook_double_free]]
- [[features/hook_verify_repair|hook_verify_repair]]

## Depended on by

- [[features/hook_verify_repair|hook_verify_repair]]

## Implementation anchors

- `vmhook::hotspot::set_dont_inline(method*, bool)` — `vmhook/ext/vmhook/vmhook.hpp:7474-7535` — fault-safe set/clear of Method::_flags bit 2 (_dont_inline) with width-aware read-modify-write; called on install at 10128 and teardown at 10995
- `vmhook::hotspot::NO_COMPILE` — `vmhook/ext/vmhook/vmhook.hpp:7409-7413` — Access-flags mask (0x02|0x04|0x08|0x01000000) that disables C1/C2/C2-OSR compilation + queues; written at 10135 and 10475, cleared at 10995-11000
- `vmhook::hotspot::method::get_flags()` — `vmhook/ext/vmhook/vmhook.hpp:2843-2908` — returns uint16_t* to Method::_flags for readback; hard-coded u2 width incorrect on JDK 8-12 (u1) and JDK 21+ (u4)
- `hook<T>(name, signature, callback)` — `vmhook/ext/vmhook/vmhook.hpp:10100-10330` — install path that applies both flags at 10128/10135; detailed install at 10128-10135
- `shutdown_hooks()` — `vmhook/ext/vmhook/vmhook.hpp:10980-11005` — teardown path that clears both flags via set_dont_inline(m,false) at 10995 and safe_access_flags_and(~NO_COMPILE) at 10995-11000
- `hook_handle::stop()` — `vmhook/ext/vmhook/vmhook.hpp:8614-8675` — per-hook teardown that mirrors shutdown_hooks flag-clearing (referenced but defined in older lines; actual impl in 10995+ region during shutdown)
- `scoped_hook<T>` — `vmhook/ext/vmhook/vmhook.hpp:8733-8830` — RAII wrapper that calls hook<T>() and clears flags on destruction via hook_handle::stop()

## Tests

- `tests/jvm/modules/dont_inline_dont_compile.cpp`

## Audit docs

- `audit/findings/dont_inline_dont_compile_flags.md`

## Known bugs

- **[high]** set_dont_inline dereferences Method* without null or validity check (7474-7535 entry); every other Method accessor guards with is_valid_pointer, but set_dont_inline does not. On JVMTI RedefineClasses between install and shutdown, Method* aliases freed memory; set_dont_inline fetches _flags from alien object and writes garbage, causing AV on Windows or silent corruption on Linux. suggest_fix: add is_valid_pointer guard at top of function.
- **[high]** get_flags() hard-codes uint16_t width (2843-2908) yet Method::_flags is u1 on JDK 8-12 and u4 on JDK 21+; OR/AND on JDK 8 u1 overwrites the adjacent byte (_intrinsic_id/_jfr_towrite depending on layout), silently corrupting unrelated fields. JDK 21+ u4 fields are partially updated. suggest_fix: dispatch on type_string (u1/u2/u4) and use width-aware read-modify-write per field width.
- **[high]** _flags read-modify-write is non-atomic (7474-7535, specifically atomic_ref fetch_or/fetch_and missing); HotSpot JIT threads concurrently write _flags with Atomic::cmpxchg. Non-atomic OR/AND races with JIT CAS and loses bits. Worst case on JDK 21+ u4 _flags: we clear _changes_current_thread/_jvmci_alias bits and trigger stale JIT deopt/barrier logic. suggest_fix: use std::atomic_ref<uintN_t>::fetch_or/fetch_and with memory_order_acq_rel.
- **[high]** set_dont_inline silent failure on VMStructs lookup (2843-2908) — when iterate_struct_entries returns nullptr (JVM not loaded / mismatched build), get_flags() returns nullptr and set_dont_inline quietly no-ops with no log. Install succeeds (only checks access_flags not _flags), hook fires interpreted, silently dies when JIT inlines any caller, no diagnostic. suggest_fix: wrap lookup in try/catch, log via VMHOOK_LOG with error_tag, have hook<T>() fail or warn on outcome.
- **[medium]** Hard-coded bit position (1 << 2) never consults Method::_dont_inline VMTypes constant (7474-7535); HotSpot exports enum values via gHotSpotVMIntConstants (pattern Method::_dont_inline) reachable through VMStructs. When JDK reorders enum (IBM Semeru OMR build observed), set_dont_inline silently sets WRONG bit (_force_inline/_caller_sensitive) and JIT inlines hook callee. suggest_fix: one-time lookup of Method::_dont_inline constant with fallback to (1<<2).
- **[medium]** NO_COMPILE includes JVM_ACC_QUEUED (0x01000000) without justification (7409-7413); QUEUED signals 'sitting in compile queue' and blocks re-enqueueing. Bit left set forever; if HotSpot sees QUEUED without matching CompileBroker entry (mid-shutdown), emits 'compile queue inconsistent' warnings. suggest_fix: document why QUEUED is needed (CompileBroker fast-path on is_queued() in JDK 12+) or drop it and rely on NOT_*_COMPILABLE alone.
- **[low]** Pre-install Method-flag mutations (set_dont_inline + NO_COMPILE at 10128/10135 in hook<T>()) leak on exception before midi2i_hook/common_detour install completes (no rollback); asymmetric with hook_handle::stop() cleanup. If install fails mid-sequence, flags stay set on a method with no detour installed.

## Notes

JDK 8-12 and JDK 21+ have different Method::_flags widths (u1 vs u4). The
test module uses best-effort readback of _dont_inline bit only when observable
on the running JDK (dont_inline_observable() gate); all other assertions
(NO_COMPILE set, idempotency, teardown clearing) are hard assertions on every
version. The VM-walk safe_read/safe_write paths (vmhook.hpp lines 2754-2840)
and method-validity guards introduced post-v0.5.0 defend against cold Method*
pages during verify_hooks repair cycles, but the initial install path (10128-10135)
still lacks guards. Fault-safe conversion of this path is pending.
