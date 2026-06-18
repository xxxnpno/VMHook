---
slug: interpreter_frame_walk
title: Interpreter Frame Walk
category: return
status: seeded
risk: high
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/high, category/return, tag/return, tag/frame, tag/interpreter, tag/saved-rbp, tag/slot-model, tag/two-slot, tag/locals, tag/x86_64, tag/stack-walk]
---

# Interpreter Frame Walk

> **Category:** [[categories/return|return_value (detour-side return manipulation)]]  ·  **Status:** `seeded`  ·  **Risk:** `high`  ·  **Specialist:** `.claude/agents/interpreter_frame_walk-specialist.md`

## Description

Reads (and writes) the live HotSpot x64 interpreter frame from inside a
detour: recovers the `Method*` (at `[rbp-24]`) and the local-variable array of
the hooked frame, decodes each argument out of its slot honouring the JVM
two-slot rule for `long`/`double`, and walks the saved-rbp chain upward to
recover the caller chain. Covers `frame::get_method`/`get_locals`/
`get_argument`/`get_arguments`, the compile-time `java_slot_offsets` +
`is_java_double_slot_v` slot model, `detail::extract_frame_arg` (the per-arg
decoder the typed `hook<T>` callback uses), the runtime-cached `locals_offset`
(`-56` default, back-scanned from `4C 8B 75 ?? C3`), and
`return_value::caller`/`stack_trace`/`frame`/`set_arg`. `get_locals` flips
encoding at JDK 21 (direct pointer 8-20 vs spilled index `(r14-rbp)>>3` on
21+). x86_64-only, HotSpot-only.

## Depends on

- [[features/vmstructs_offset_resolution|vmstructs_offset_resolution]]
- [[features/decode_oop_and_pointers|decode_oop_and_pointers]]
- [[features/os_safe_read|os_safe_read]]

## Related

- [[features/return_caller|return_caller]]
- [[features/return_stack_trace_depth|return_stack_trace_depth]]
- [[features/return_frame_raw_access|return_frame_raw_access]]
- [[features/return_set_arg|return_set_arg]]
- [[features/traits_function_traits|traits_function_traits]]

## Depended on by

- [[features/hook_basic|hook_basic]]
- [[features/hook_common_detour_dispatch|hook_common_detour_dispatch]]
- [[features/return_caller|return_caller]]
- [[features/return_frame_raw_access|return_frame_raw_access]]
- [[features/return_set_arg|return_set_arg]]
- [[features/return_stack_trace_depth|return_stack_trace_depth]]

## Implementation anchors

- `hotspot::frame (get_method / get_locals)` — `vmhook/ext/vmhook/vmhook.hpp:5976-6150` — this == rbp; get_method [rbp-24] (5991); get_locals direct-ptr vs spilled-index JDK 21 flip (6074)
- `frame::get_argument / get_arguments` — `vmhook/ext/vmhook/vmhook.hpp:6152-6420` — typed tuple (6152) + signature-parsed (6204) arg decode + private get_argument (6341); two-slot J/D bump, narrow-oop heuristic
- `locals_offset (constinit, find_hook_location back-scan)` — `vmhook/ext/vmhook/vmhook.hpp:5633-5633` — single process-global int8 (-56 default); written by find_hook_location (5650) scanning 4C 8B 75 ?? C3 (5725)
- `detail::java_slot_offsets / is_java_double_slot_v` — `vmhook/ext/vmhook/vmhook.hpp:9271-9320` — compile-time slot model: long/ulong/double = 2 slots, everything else = 1
- `detail::extract_frame_arg` — `vmhook/ext/vmhook/vmhook.hpp:9324-9460` — the decoder the typed hook<T> callback uses; static_asserts unsupported types
- `return_value::caller` — `vmhook/ext/vmhook/vmhook.hpp:9551-9670` — single-frame saved-rbp walk; lacks the stack_trace chain-validation guards (flaw)
- `return_value::stack_trace` — `vmhook/ext/vmhook/vmhook.hpp:9674-9815` — multi-frame walk with monotonic/<=1MiB rbp guard + ConstMethod->ConstantPool validation
- `return_value::set_arg` — `vmhook/ext/vmhook/vmhook.hpp:9817-10010` — write side: full 64-bit uncompressed oop store, lower-slot write for sizeof>4

## Tests

- `tests/test_interpreter_frame_walk.cpp`
- `tests/test_traits.cpp`
- `tests/test_traits_extra.cpp`
- `tests/test_helpers.cpp`
- `tests/jvm/modules/return_frame_raw_access.cpp`
- `tests/jvm/modules/return_stack_trace_depth.cpp`

## Known bugs

- **[high]** caller() (vmhook.hpp:9551) is NOT hardened against the stray-pointer AV that stack_trace() explicitly defends against — it lacks both the monotonic/<=1MiB saved-rbp distance guard and the up-front ConstMethod/ConstantPool chain validation that stack_trace() (9674) carries, then calls caller_method->get_name()/get_signature() with only an is_valid_pointer gate. A hook whose immediate caller is a compiled/native frame (common once the call site JITs) can land [caller_rbp-24] on a bogus Method* and AV inside get_name(). Fix: route caller() through the shared stack_trace guards / stack_trace(1).front().
- **[medium]** locals_offset is a single process-global, last-writer-wins (decl vmhook.hpp:5633, written by find_hook_location at 5725). find_hook_location overwrites it on every i2i-stub scan; two stubs with different mov r14,[rbp+disp] displacements make the second offset apply to both, silently mis-reading the first method's locals. A failed pattern match leaves the stale/-56 default.
- **[medium]** get_locals() JDK-era heuristic (vmhook.hpp:6074) has an unguarded middle band: a JDK-21+ frame whose recovered index is >= 0x1000 (a method with >4096 locals-worth of frame span — legal, max_locals is u2 up to 65535) silently yields nullptr, making every typed-arg decode return defaults with no diagnostic. The 0x1000 boundary is unrelated to the real 0xFFFF max_locals bound.
- **[medium]** The bits <= 0xFFFFFFFF narrow-oop heuristic (shared by extract_frame_arg, get_argument, get_arguments) is ambiguous with compressed oops disabled (-XX:-UseCompressedOops / large heaps): a genuine uncompressed heap pointer below 4 GiB is wrongly decode_oop_pointer'd, corrupting the decoded self/String/wrapper. Silent either way.
- **[low]** Slot-model J/D two-slot rule duplicated across six sites (java_slot_offsets::compute, get_arguments inline, get_argument, extract_frame_arg, set_arg, descriptor parser). Agree today, latent regression surface.
- **[low]** float and bool ride the generic sizeof<=4 primitive path (in get_argument and extract_frame_arg) with no sign/width normalisation; a non-canonical slot byte would surface as a bool with non-{0,1} representation. Edge case, not a confirmed bug.

## Notes

Method* offset -24, locals_offset back-scan, and the saved-rbp walks are the
x64 interpreter constants (interpreter_frame_method_offset = -3 words); stable
across HotSpot x64 JDK 8-26 but architecture/ABI-specific. The locals encoding
flips at JDK 21 (direct pointer 8-20 vs spilled index (r14-rbp)>>3 on 21+),
detected by the is_valid_pointer-vs-<0x1000 heuristic. The typed hook<T>
callback path is sound (bounds-guarded, null-guarded, static_assert on
unsupported types); the hazards bite the raw/manual path (ret.frame() ->
get_locals()/get_argument by hand) and the caller() convenience. No-JVM
coverage is in test_interpreter_frame_walk.cpp (saved-rbp guard contract) +
test_traits[_extra].cpp (compile-time slot model) + test_helpers.cpp (no-frame
contract); live-JVM coverage in return_frame_raw_access.cpp (slot model,
set_arg round-trip) + return_stack_trace_depth.cpp (multi-frame walk).
