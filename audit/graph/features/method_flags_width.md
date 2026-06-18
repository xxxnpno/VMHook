---
slug: method_flags_width
title: Method Flags Width
category: method
status: seeded
risk: high
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/high, category/method, tag/method, tag/flags, tag/access-flags, tag/width, tag/abi, tag/dont-inline, tag/no-compile, tag/vmstructs]
---

# Method Flags Width

> **Category:** [[categories/method|Method proxies (resolve / call / dispatch)]]  ·  **Status:** `seeded`  ·  **Risk:** `high`  ·  **Specialist:** `.claude/agents/method_flags_width-specialist.md`

## Description

The width-correctness of `vmhook::hotspot::method`'s two flag accessors —
`get_flags()` (HotSpot `Method::_flags`, home of `_dont_inline`) and
`get_access_flags()` (HotSpot `Method::_access_flags`, home of `JVM_ACC_*`
including `JVM_ACC_STATIC` and the `NO_COMPILE` mask). The crux: HotSpot's
`Method::_flags` has changed BYTE WIDTH across JDK generations (u1 -> u2 -> u4
+ a sibling `_flags2`), yet vmhook reads/writes it through a HARD-CODED
`std::uint16_t*` (`get_flags` returns a `uint16_t*`, and `set_dont_inline` does a
non-atomic read-modify-write `*flags |= (1<<2)` / `&= ~(1<<2)`). This specialist
owns whether vmhook reads the right bytes, writes only the right bytes, and never
clobbers the adjacent Method field on a width mismatch. `get_access_flags()` is
safe at u32 across all JDKs (AccessFlags is a 4-byte int-backed wrapper and every
bit vmhook reads lives in those 4 bytes); only `get_flags()` is width-fragile.

## Depends on

- [[features/vmstructs_offset_resolution|vmstructs_offset_resolution]]

## Related

- [[features/method_static|method_static]]
- [[features/method_entry_points_i2i_i2c|method_entry_points_i2i_i2c]]
- [[features/iterate_entries_safety|iterate_entries_safety]]

## Depended on by

- [[features/dont_inline_dont_compile|dont_inline_dont_compile]]

## Implementation anchors

- `method::get_access_flags` — `vmhook/ext/vmhook/vmhook.hpp:2728-2770` — resolves the ('Method','_access_flags') VMStruct and returns a uint32_t* at this+offset; try/catch + VMHOOK_LOG; u32 width is safe across all JDKs
- `method::get_flags` — `vmhook/ext/vmhook/vmhook.hpp:2938-2980` — resolves ('Method','_flags') and returns a HARD-CODED uint16_t* — the only width-fragile accessor; does NOT consult entry->type_string for the real width and has NO try/catch
- `hotspot::set_dont_inline` — `vmhook/ext/vmhook/vmhook.hpp:7514-7560` — the ONLY consumer of get_flags(); non-atomic RMW `*flags |= (1<<2)` / `&= ~(1<<2)` with a hard-coded _dont_inline bit position (never looked up from gHotSpotVMIntConstants)
- `method_proxy::is_static (the correct width-independent pattern)` — `vmhook/ext/vmhook/vmhook.hpp:17209-17238` — reads get_access_flags() as uint32_t* and masks 0x0008 — width-independent because the static bit is in the low byte; the right contrast to get_flags()'s fixed-width read

## Tests

- `tests/test_method_flags_width.cpp`

## Known bugs

- **[high]** get_flags() hard-codes std::uint16_t*; wrong width corrupts the adjacent field on JDK 8-12 (u1) and truncates on JDK 21+ (u4). Method::_flags is u1 on JDK 8..~12 (the RMW `*flags |= (1<<2)` reads+writes 2 bytes, so the byte at offset+1 — a DIFFERENT Method field — is read into the RMW and written back: silent adjacent-field corruption of a live Method), u2 on JDK 13..20 (what the code assumes; correct), u4 on JDK 21+ (a 2-byte read sees only the low half). get_flags ignores entry->type_string which carries the real width literal (u1/u2/u4/MethodFlags).
- **[low]** set_dont_inline hard-codes the _dont_inline bit position (1 << 2) rather than looking it up from gHotSpotVMIntConstants, and the RMW is non-atomic on a live Method word.

## Notes

vm_struct_entry_t carries type_string (the HotSpot field-type literal, e.g.
u1/u2/u4/MethodFlags), offset, and is_static but NO explicit byte-size field — the
width must be inferred from type_string, which get_flags() ignores entirely. The
NO_COMPILE mask (0x0F000000) and JVM_ACC_STATIC (0x0008) live within the safe u32
_access_flags word. Install/teardown wiring touches both flag words together
(install sets _dont_inline + NO_COMPILE; verify/repair re-applies on JIT drift;
shutdown clears both). method_proxy::is_static demonstrates the correct
width-independent approach (read as u4, mask 0x0008) and is the contrast to
get_flags()'s fixed-width read. This is a no-JVM unit-tested feature
(tests/test_method_flags_width.cpp).
