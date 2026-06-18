---
slug: seh_invoke_detour
title: Seh Invoke Detour
category: hook
status: seeded
risk: high
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/high, category/hook, tag/seh, tag/crash-safety, tag/detour, tag/exception-handling, tag/hook, tag/windows, tag/toolchain-asymmetric, tag/safety]
---

# Seh Invoke Detour

> **Category:** [[categories/hook|Hooking machinery (install / dispatch / trampolines)]]  ·  **Status:** `seeded`  ·  **Risk:** `high`  ·  **Specialist:** `.claude/agents/seh_invoke_detour-specialist.md`

## Description

The single crash-safety boundary that wraps every user detour invocation so a
stale-OOP deref, a null receiver, a half-initialised wrapper, or any other
access violation inside a hook callback becomes a RECOVERABLE event (skip this
invocation, run the original Java method body) instead of tearing the whole JVM
process down. It is the one and only __try/__except in the library. Declared
noexcept, returns bool (true = clean completion, false = a fault/throw was
swallowed), with a two-branch body: on MSVC-non-clang, real SEH
(__try/__except(EXCEPTION_EXECUTE_HANDLER)) — the only path that traps a
hardware access violation (0xC0000005); everywhere else (MinGW/GCC/Clang) a
plain C++ try/catch(...) that catches C++ throws but CANNOT trap a hardware
segfault. Its only caller is common_detour: on a hook match it invokes the
guard and, regardless of the bool, forces _thread_in_Java and returns
(allow-through) so the original body runs whether the detour completed or
faulted. It is split into its own tiny function precisely because __try/__except
must live in a frame with no automatic objects needing C++ unwinding.

## Depends on

- [[features/os_signal_handler|os_signal_handler]]

## Related

- [[features/hook_basic|hook_basic]]
- [[features/wrapper_pattern|wrapper_pattern]]
- [[features/decode_oop_and_pointers|decode_oop_and_pointers]]

## Depended on by

- [[features/hook_basic|hook_basic]]
- [[features/hook_common_detour_dispatch|hook_common_detour_dispatch]]
- [[features/hook_verify_repair|hook_verify_repair]]

## Implementation anchors

- `vmhook::hotspot::seh_invoke_detour(detour_fn, frame, thread, slot)` — `vmhook/ext/vmhook/vmhook.hpp:7085-7113` — the wrapper itself; noexcept -> bool. #if defined(_MSC_VER) && !defined(__clang__): __try { detour_fn(...); return true; } __except(EXCEPTION_EXECUTE_HANDLER) { return false; } (the only HW-AV-trapping arm); #else: try { ... return true; } catch(...) { return false; } (C++ throws only, no SIGSEGV trap). The doc/rationale block immediately above states the two hard design constraints
- `common_detour() — the one and only caller` — `vmhook/ext/vmhook/vmhook.hpp:7138-7210` — on hook.method == current_method (7189) calls seh_invoke_detour(hook.detour, ...) (7197); on a false return VMHOOK_LOGs a warning naming the method pointer (7199-7204) then UNCONDITIONALLY forces thread->set_thread_state(_thread_in_Java) (7207) and returns (7208) — the original Java body runs (allow-through) whether the detour completed or faulted. The bool is used for LOGGING ONLY, not control flow
- `wrapper_detour lambda (what detour_fn actually is) + return_value retval` — `vmhook/ext/vmhook/vmhook.hpp:10229-10256` — the per-hook lambda stored as hooked_method::detour; constructs vmhook::return_value retval{ slot, frame_pointer } (10232, a non-trivial C++ object — the unwinding hazard the split-out function exists to avoid), then extract_frame_arg per arg, then the user callback last. A stale/unmapped OOP page-faults inside extract_frame_arg, exactly where the SEH guard must catch it
- `return_slot (bool cancel; int64 retval) + detail::extract_frame_arg<T>` — `vmhook/ext/vmhook/vmhook.hpp:1313-1317` — return_slot at 1313-1317; on a swallowed fault slot is left in whatever state the detour managed to set before faulting (no reset on the false path). extract_frame_arg (def at 9324) reads locals[-offset] from the live interpreter frame — the canonical fault site the guard wraps

## Known bugs

- **[high]** On MinGW/GCC/Clang the feature does NOT deliver crash-safety against hardware faults. The catch(...) arm only catches C++ exceptions; the dominant real-world failure mode — a stale-OOP / null-receiver hardware segfault inside extract_frame_arg — is a SIGSEGV, not a C++ throw, so it sails straight through catch(...) and aborts the process. The doc comment frames the SEH guard as THE protection against '0xC0000005 tears the JVM down', but that protection silently evaporates on three of the four toolchains the project builds with. There is no SIGSEGV->exception translation on the non-MSVC path.
- **[high]** The build never sets /EHa, so the MSVC __except arm is not guaranteed to catch hardware access violations. CMake's MSVC default EH model is /EHsc; under it the optimizer may elide state needed for correct unwinding through frames BETWEEN the faulting instruction and this __except. A leaf __except usually still catches an in-frame AV, but when the AV happens deeper in the call tree (inside extract_frame_arg -> is_valid_pointer -> a decode helper) the /EHsc async-unwind guarantees are exactly what /EHa exists to provide. The single most load-bearing flag for the library's headline crash-safety promise, and it is implicit/absent.
- **[high]** C++ exceptions thrown by a user detour are handled ASYMMETRICALLY across toolchains. Under MSVC, a C++ throw is SEH code 0xE06D7363, so __except(EXCEPTION_EXECUTE_HANDLER) swallows it — but as a raw SEH unwind with NO C++ catch semantics: intervening catch/__finally/destructor logic is bypassed and the thrown object's destructor never runs. Under MinGW/Clang the catch(...) is a proper C++ catch that DOES run intervening destructors. The same throwing detour produces two different unwind behaviours / two different leak profiles by toolchain. Worse, return_value retval is constructed OUTSIDE any guard the throw can see on the MSVC path; its destructor is skipped on an SEH-terminated unwind.
- **[medium]** Partial/torn return_slot state survives a swallowed fault. When the detour faults AFTER calling retval.set(...)/cancel() (writing slot->retval / slot->cancel), seh_invoke_detour returns false but common_detour ignores the bool for control flow — it ALWAYS falls through to the original body. If the detour set cancel=true then faulted, the slot still says cancel yet the original body runs anyway; there is no slot reset on the false path. Fix: zero *slot before returning false, and/or have the caller treat false as 'force allow-through with a clean slot'.
- **[low]** seh_invoke_detour is noexcept but its non-MSVC body's catch(...) is the only thing keeping that promise; the MSVC body relies on __except swallowing everything. If a future edit ever lets an exception escape either arm (e.g. an added automatic object whose destructor throws during unwind), the noexcept converts it to std::terminate — the exact JVM-killing outcome this function exists to prevent. Correct today but load-bearing and fragile: any code added between the two returns must itself be nothrow.

## Notes

There are NO logic bugs inside the four-line bodies themselves — the control
flow is correct. The defects are all about (a) the build not enabling the EH
mode the MSVC arm needs (/EHa), (b) the non-MSVC arm being a crash-safety no-op
against hardware faults, and (c) the caller not acting on the false return
beyond logging.

NO dedicated test exists today (test_modules intentionally empty). The only
references to seh_invoke_detour in the test tree are doc comments describing
behaviour, not assertions (harness.cpp's run_one is a structural twin but guards
MODULES, not detours; global_ref.cpp / hook_chaining mention it only in prose).
The needed suite is a live-JVM module tests/jvm/modules/seh_invoke_detour.cpp +
fixture that deliberately makes a detour fault (null-receiver deref, stale-OOP
after GC, wild-pointer write, extract_frame_arg-induced fault, C++ throw, throw
non-std, set/cancel-then-fault, per-invocation recovery, static-method fault,
1000x stress) then asserts the JVM survives and produces the ORIGINAL result —
with the hardware-fault probes gated to MSVC-non-clang only (they are EXPECTED
to abort on MinGW/GCC/Clang per the high bug) and the C++-throw probes running
everywhere.

JDK sensitivity: the guard's four lines are JDK-agnostic, but WHAT makes the
detour fault is highly JDK-sensitive (compressed-OOP decode in extract_frame_arg,
the stale-OOP-after-GC collector/class-unloading behaviour). JDK 8 is the
high-risk fault source (global-ref-backed slot reads SIGSEGV in a way
is_valid_pointer doesn't catch — why JDK 8 is gated off MinGW). JDK 21+ dropped
the pre-detour thread-state precondition, so containment + the forced
_thread_in_Java state-repair must be re-verified on 21+/26.
