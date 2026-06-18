---
slug: method_call_object
title: Method Call Object
category: method
status: seeded
risk: high
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/high, category/method, tag/method, tag/call, tag/object, tag/reference, tag/unique-ptr, tag/compressed-oop, tag/call-jni, tag/jdk21]
---

# Method Call Object

> **Category:** [[categories/method|Method proxies (resolve / call / dispatch)]]  ·  **Status:** `seeded`  ·  **Risk:** `high`  ·  **Specialist:** `.claude/agents/method_call_object-specialist.md`

## Description

`method_proxy::call()` that returns a Java reference type (`L…;` or `[…`) and
whose `value_t` implicitly converts to a `std::unique_ptr<wrapper>`. This is the
method-vs-field parity path — `field_proxy::value_t` always decoded a compressed
OOP into a `unique_ptr<wrapper>`; `method_proxy::value_t` was taught the same
trick. Contract: a non-null Object return yields a USABLE wrapper (read a field
AND call a method through it); a null return yields a null `unique_ptr`. The
conversion operator's `unique_ptr` arm decodes the stored `uint32` (compressed
OOP) via `decode_oop_pointer`, `is_valid_pointer`-checks it, and wraps
`new wrapper_type{ decoded }` (a `static_assert` pins `wrapper_type :
object_base`). The two store-side paths DISAGREE: the call-stub path stores a
real compressed OOP that round-trips, but the call_jni fallback stores the low
32 bits of a JNI indirect local-ref handle (then frees it) — so the feature is
broken for non-String objects on JDK 21+.

## Depends on

- [[features/method_call_primitives|method_call_primitives]]
- [[features/method_proxy_value_t|method_proxy_value_t]]
- [[features/wrapper_pattern|wrapper_pattern]]
- [[features/decode_oop_and_pointers|decode_oop_and_pointers]]

## Related

- [[features/method_call_jni_fallback|method_call_jni_fallback]]
- [[features/method_call_return_void|method_call_return_void]]
- [[features/method_call_string|method_call_string]]
- [[features/method_call_wide_args|method_call_wide_args]]
- [[features/method_proxy_value_t|method_proxy_value_t]]
- [[features/method_return_types|method_return_types]]
- [[features/field_object_ref|field_object_ref]]

## Implementation anchors

- `method_proxy::value_t conversion operator (unique_ptr arm)` — `vmhook/ext/vmhook/vmhook.hpp:16033-16207` — the feature: uint32 -> decode_oop_pointer -> is_valid_pointer -> new wrapper_type{decoded}; static_assert wrapper_type : object_base; mirrors field_proxy::value_t::cast_for_variant
- `method_proxy::call_jni ('L'/'[' branch)` — `vmhook/ext/vmhook/vmhook.hpp:16233-16870` — the BUG site: for a non-String reference stores the truncated low 32 bits of the JNI local-ref handle and DeleteLocalRefs it before returning
- `method_proxy::call (call-stub default arm)` — `vmhook/ext/vmhook/vmhook.hpp:16871-17050` — the WORKING path: encode_oop_pointer(result_oop) stores a real compressed OOP that round-trips through decode_oop_pointer
- `detail::find_call_stub_entry` — `vmhook/ext/vmhook/vmhook.hpp:15936-15970` — selects which store path runs; absent on CI JDKs (-> the broken call_jni path)

## Tests

- `tests/jvm/modules/method_call_object.cpp`

## Known bugs

- **[high]** [STILL LIVE on JDK 21+] The call_jni 'L'/'[' branch returns a truncated, already-freed JNI handle for every non-String Object return: it keeps only the low 32 bits of a 64-bit JNI INDIRECT local-ref pointer and DeleteLocalRefs it on the next line. The unique_ptr arm then feeds those 32 bits to decode_oop_pointer, computing narrow_oop_base + (low32 << shift) — a garbage address neither the handle nor the heap OOP — which is_valid_pointer usually rejects (-> null unique_ptr) or occasionally accepts (bogus pointer). So a non-null Object-returning call() -> unique_ptr<wrapper> WORKS on JDK 8-20 (call-stub present) but silently yields null/garbage on JDK 21+ (call_jni). Fix: decode the local ref to its underlying OOP and store the full pointer, moving the DeleteLocalRef after the decode.
- **[medium]** The encode_oop_pointer round-trip on the call-stub path is fragile when compressed oops are disabled — the uint32 variant alternative cannot represent a >32-bit narrow oop, so a large-heap / -XX:-UseCompressedOops configuration would lose the high bits even on the working path.

## Notes

The variant's 'uint32 == compressed OOP' invariant holds on the call-stub path
but is violated on the call_jni path (the high bug above). The companion comment
claiming uint32 -> void* yields 'the FULL 64-bit decoded heap pointer' is true
only on the call-stub path. String returns are decoded correctly on BOTH paths
(read_java_string on the stub, GetStringUTFChars on call_jni), so the breakage is
specific to NON-String reference returns. The module installs a scoped_hook on a
trivial tick() and performs every object-returning call on self inside that
detour (call() needs current_java_thread); $-nested wrapper registration is the
established pattern. Every decoded-OOP deref is is_valid_pointer-gated.
