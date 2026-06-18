---
slug: hw_breakpoint_dr7
title: Hw Breakpoint Dr7
category: os
status: seeded
risk: low
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/low, category/os, tag/os, tag/hardware-breakpoint, tag/dr7, tag/debug-register, tag/bit-mask, tag/x86_64, tag/windows-only, tag/watch-static-field, tag/compile-time-gated]
---

# Hw Breakpoint Dr7

> **Category:** [[categories/os|OS abstraction (memory / signals / breakpoints)]]  ·  **Status:** `seeded`  ·  **Risk:** `low`  ·  **Specialist:** `.claude/agents/hw_breakpoint_dr7-specialist.md`

## Description

The pure bit-mask helper `vmhook::os::detail_dr::build_dr7(slot, rw, len)` that packs an Intel x86_64
DR7 debug-control register value (local-enable bit + R/W field + LEN field) for ONE of the four
hardware data-breakpoint slots (DR0-DR3). Three terms are OR'd: local-enable `1 << (slot*2)` (L0-L3 at
bits 0/2/4/6), the R/W field `rw << (16 + slot*4)`, and the LEN field `len << (18 + slot*4)`. The
global-enable bits G0-G3 (odd bits) are DELIBERATELY never set, so the trap only applies to threads
vmhook explicitly programs. This mask is what `watch_static_field<>` programs into every thread's
`CONTEXT.Dr7` to arm a zero-overhead write-trap on a Java static field's backing storage. The scope is
the bit layout itself — every shift, slot, and enum encoding — not the live arming of the trap (that
belongs to watch_static_field). Compile-time gated on Windows + x86_64.

## Related

- [[features/watch_static_field|watch_static_field]]

## Depended on by

- [[features/watch_static_field|watch_static_field]]

## Implementation anchors

- `vmhook::os::detail_dr::build_dr7(slot, rw, len)` — `vmhook/ext/vmhook/vmhook.hpp:1241-1250` — three OR'd terms — local_enable = 1 << (slot*2); rw_bits = rw << (16 + slot*4); len_bits = len << (18 + slot*4). Global-enable bits G0-G3 deliberately never set so the trap is per-thread, not process-wide
- `enum class data_breakpoint_kind` — `vmhook/ext/vmhook/vmhook.hpp:1210-1218` — write = 0b01, read_write = 0b11 — raw values shifted verbatim into the Intel R/W field; no execute kind (0b00 = execute, 0b10 = I/O) is exposed because this is a DATA breakpoint API
- `enum class data_breakpoint_length` — `vmhook/ext/vmhook/vmhook.hpp:1219-1226` — one_byte = 0b00, two_bytes = 0b01, eight_bytes = 0b10, four_bytes = 0b11 — the Intel LEN ordering is genuinely counter-intuitive (0b10 = EIGHT bytes, 0b11 = FOUR); 'fixing' it to ascending order would silently mis-size every 4/8-byte watch
- `VMHOOK_HAS_HW_DATA_BREAKPOINTS gate` — `vmhook/ext/vmhook/vmhook.hpp:1196-1227` — build_dr7 and detail_dr::for_each_thread exist only when VMHOOK_OS_WINDOWS && VMHOOK_ARCH_X86_64; the enums are declared unconditionally, only the helpers are behind the gate
- `detail::refresh_thread_drs(slot, address, dr7_bits)` — `vmhook/ext/vmhook/vmhook.hpp:20629-20656` — consumer — writes the field address into CONTEXT.DrN then merges the build_dr7 bits under a per-slot mask (slot_mask_local 0b11<<(slot*2) | slot_mask_rwlen 0xF<<(16+slot*4)) so it never clobbers another slot's bits
- `watch_static_field<wrapper, field_type>` — `vmhook/ext/vmhook/vmhook.hpp:20816-20910` — sole caller — picks length from sizeof(field_type) via a constexpr ladder (1->one_byte, 2->two_bytes, 4->four_bytes, else->eight_bytes), always passes data_breakpoint_kind::write, calls build_dr7 and stashes the result in dr_slots[slot].dr7_bits

## Known bugs

- **[low]** data_breakpoint_length enum encodes the genuinely counter-intuitive Intel LEN ordering (eight_bytes = 0b10, four_bytes = 0b11). This is CORRECT per the Intel SDM, but a maintainer 'fixing' it to a natural ascending order (4 before 8) would silently mis-size every 4/8-byte hardware watch with no compile error. Pinned as a regression-target: the bit layout for each (slot, kind, length) triple should be asserted against the SDM-specified mask.
- **[low]** No test module exists for the bit math yet (test_modules: []). build_dr7 is exercised only indirectly through watch_static_field at runtime on Windows/x86_64; the shift arithmetic, slot bookkeeping, and the G-bits-never-set invariant have no standalone unit coverage. The math is JDK-independent and host-OS-gated, so a pure-logic (no-JVM) test would be the natural home.

## Notes

This is a pure bit-mask helper — its blast radius is contained to the watch_static_field write-trap, so
risk is low (a wrong mask mis-sizes or mis-targets one watch slot, it does not corrupt the JVM or the
hooking machinery). The math is JDK-independent: it depends only on the Intel x86_64 DR7 register format,
not on any HotSpot struct, so all supported Java versions are equally valid (the java_versions list
reflects 'compiles/works on the whole matrix', not a per-JDK behavioural difference). The feature is
compile-time gated to VMHOOK_OS_WINDOWS && VMHOOK_ARCH_X86_64 (VMHOOK_HAS_HW_DATA_BREAKPOINTS); the enums
are declared unconditionally so the type names resolve on every platform, but build_dr7 / for_each_thread
/ refresh_thread_drs are Windows-x86_64 only. The G-bits-never-set choice is load-bearing: a stray
global-enable bit would arm the breakpoint process-wide and outside vmhook's slot bookkeeping.
