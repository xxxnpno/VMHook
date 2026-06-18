---
slug: watch_static_field
title: Watch Static Field
category: field
status: seeded
risk: high
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/high, category/field, tag/watchpoint, tag/hardware-breakpoint, tag/debug-registers, tag/dr0-dr3, tag/veh, tag/static-field, tag/windows, tag/x86_64]
---

# Watch Static Field

> **Category:** [[categories/field|Field proxies (get / set / introspection)]]  ·  **Status:** `seeded`  ·  **Risk:** `high`  ·  **Specialist:** `.claude/agents/watch_static_field-specialist.md`

## Description

vmhook::watch_static_field<wrapper, field_type>(name, callback) — arms a
HARDWARE data breakpoint (one of the four CPU debug-register slots DR0-DR3) on
a Java static field's backing storage. The trap fires SYNCHRONOUSLY on whichever
thread writes the field, DURING the store, and the callback runs inside a
vectored exception handler (VEH) on that same thread. vmhook resolves the
field's static address via the wrapper's class mirror, allocates a free DR slot,
and arms the trap on EVERY thread that exists at install time (including the
harness loop thread) by writing Dr0-Dr3 + the matching Dr7 enable/RW/LEN bits
via GetThreadContext/SetThreadContext. It returns an RAII watch_handle (stop()
on destruct, idempotent; running() reflects armed state). Capacity is exactly
four: a fifth concurrent watch is cleanly REFUSED with an empty handle rather
than crashing. Only available where VMHOOK_HAS_HW_DATA_BREAKPOINTS is 1 (Windows
x86_64); elsewhere it returns an empty handle (the documented no-op fallback).

## Depends on

- [[features/field_static|field_static]]
- [[features/hw_breakpoint_dr7|hw_breakpoint_dr7]]

## Implementation anchors

- `VMHOOK_HAS_HW_DATA_BREAKPOINTS gate` — `vmhook/ext/vmhook/vmhook.hpp:1201-1205` — 1 only when VMHOOK_OS_WINDOWS && VMHOOK_ARCH_X86_64, else 0; the companion comment (1189-1200) notes watch_static_field returns an empty handle off-platform
- `vmhook::watch_static_field<wrapper_type, field_type, callback_type>(field_name, on_change)` — `vmhook/ext/vmhook/vmhook.hpp:20815-20912` — public template -> watch_handle. #if VMHOOK_HAS_HW_DATA_BREAKPOINTS: resolve the field's static address (empty handle on not-found / null-address), find_free_slot (empty handle + 'all 4 hardware breakpoint slots in use' log when full), dr_arm_one() to ensure the VEH is installed, build the control_block whose on_stop disarms the slot; #else returns watch_handle{} with a 'hardware data breakpoints unsupported' log
- `vmhook::watch_handle (RAII disarm)` — `vmhook/ext/vmhook/vmhook.hpp:8945-9028` — move-only; control_block holds the on_stop disarm callback + a stopped flag; ~watch_handle and move-assignment call stop(); stop() is idempotent and guards the on_stop callback against throws; running() reports whether the watch is still armed (false for the empty/refused handle)
- `detail DR registry + VEH (dr_slot[4], find_free_slot, refresh_thread_drs / clear_thread_drs, dr_exception_handler, dr_arm_one)` — `vmhook/ext/vmhook/vmhook.hpp:20594-20780` — dr_slots[4] + dr_mutex + a 0<->1 refcount (dr_armed_count) gating AddVectoredExceptionHandler / RemoveVectoredExceptionHandler so the VEH is uninstalled when the last watch drops; refresh_thread_drs/clear_thread_drs write Dr0-Dr3 and OR/clear the per-slot Dr7 local + RW/LEN bits across for_each_thread without clobbering sibling slots; dr_exception_handler reads Dr6 to pick the firing slot and dispatches the slot callback

## Tests

- `tests/jvm/modules/watch_static_field.cpp`

## Notes

What the live-JVM module proves (Windows x86_64, the only platform where
VMHOOK_HAS_HW_DATA_BREAKPOINTS is 1): a watch on a static int observes a
Java-driven write — the callback fires once per putstatic (N writes -> N fires)
and its `new` argument carries the field's NEW value, ending at the precise
final value; the four hardware slots fill simultaneously (four independent
watches all running()) and a FIFTH is cleanly REFUSED with an empty handle
(running()==false) instead of crashing; the four armed watches are INDEPENDENT
(driving one field fires ONLY that field's callback); dropping a watch FREES its
slot so a later install that would have been the fifth now succeeds; after a
watch is stopped, further writes to its field DO NOT fire it.

Test coverage: tests/jvm/modules/watch_static_field.cpp against
vmhook/fixtures/WatchStaticField. SAFETY (hardware DRs are delicate): EVERY
watch_handle is RAII-scoped or explicitly stop()'d BEFORE the module returns so
no watchpoint is left armed to fire spuriously inside a later module sharing the
JVM (a stray armed DR or a callback re-entering the JVM can wedge/crash the
process). The callbacks ONLY touch std::atomic<> counters — no allocation, no
JVM re-entry, as the VEH-context contract requires. Every real-trap assertion is
GATED on VMHOOK_HAS_HW_DATA_BREAKPOINTS; the unsupported-platform branch asserts
the empty-handle fallback instead of silently skipping. A watcher module — it
installs no method hooks, so it never calls shutdown_hooks(). MSVC copy-init,
never brace-init from a value_t. Java-8-only fixture.
