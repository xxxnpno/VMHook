---
slug: method_call_return_void
title: Method Call Return Void
category: method
status: seeded
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/medium, category/method, tag/method, tag/call, tag/void, tag/monostate, tag/is-void, tag/call-stub, tag/call-jni, tag/nonvirtual]
---

# Method Call Return Void

> **Category:** [[categories/method|Method proxies (resolve / call / dispatch)]]  ·  **Status:** `seeded`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/method_call_return_void-specialist.md`

## Description

Invoking VOID-returning Java methods through `method_proxy::call()` and the
`value_t::is_void()` introspection contract that distinguishes "returned void /
the call failed" from a primitive zero or an empty string. Covers both dispatch
backends — the HotSpot call-stub fast path (`case 'V'` returns
`value_t{monostate}` WITHOUT reading result_holder) and the JNI
`Call(Static)?VoidMethodA` fallback (slot 63/143, with a `<init>`/`<clinit>`
-> CallNonvirtualVoidMethodA slot-93 reroute) — and the guarantee that a void
dispatch delivers its arguments, runs the real Java body, and does NOT poison
subsequent calls on the detour thread. The void/failure alternative is
`std::monostate`, so `static_cast<int32_t>(call())` on a void/failed call yields
0, indistinguishable from a real 0 without `is_void()` — which is why the
contrast assertions exist.

## Depends on

- [[features/method_call_primitives|method_call_primitives]]
- [[features/method_proxy_value_t|method_proxy_value_t]]

## Related

- [[features/method_call_jni_fallback|method_call_jni_fallback]]
- [[features/method_call_object|method_call_object]]
- [[features/method_call_string|method_call_string]]
- [[features/method_call_wide_args|method_call_wide_args]]
- [[features/method_return_types|method_return_types]]
- [[features/method_static|method_static]]

## Implementation anchors

- `method_proxy::call (call-stub 'V' decode)` — `vmhook/ext/vmhook/vmhook.hpp:16871-17050` — receiver pushed only when object && !static_field; case 'V' returns value_t{monostate} without reading result_holder
- `method_proxy::call_jni (void dispatch)` — `vmhook/ext/vmhook/vmhook.hpp:16233-16870` — is_static_call derived from object==nullptr; <init>/<clinit> -> CallNonvirtualVoidMethodA slot 93, else CallVoidMethodA/CallStaticVoidMethodA slot 63/143; null fn-slot also returns monostate
- `method_proxy::value_t::is_void / is_string / as_string` — `vmhook/ext/vmhook/vmhook.hpp:16033-16207` — is_void = holds_alternative<monostate>; the conversion operator returns a default-constructed T for monostate
- `detail::find_call_stub_entry` — `vmhook/ext/vmhook/vmhook.hpp:15936-15970` — StubRoutines::_call_stub_entry; absent (typical JDK 21+) forces the call_jni void path

## Tests

- `tests/jvm/modules/method_call_return_void.cpp`

## Known bugs

- **[low]** is_void() cannot distinguish 'ran cleanly' from 'couldn't dispatch': both the void success and the null-fn-slot failure path in call_jni (and every other failure) return value_t{monostate}, so a caller has no way to tell a clean ()V call from a dispatch failure.

## Notes

method_proxy fields: object is the raw receiver OOP, static_field is the
discriminator the CALL-STUB path uses to decide whether to push a receiver slot
(receiver pushed only when object && !static_field); call_jni instead derives
is_static_call as object==nullptr (NOT from static_field). make_java_string backs
a String arg on the call-stub path; the JNI path routes a String arg through
jni_new_string_utf (NewStringUTF) inside write_jni_arg_to_slot — the two paths
build the arg differently but must deliver the same value. find_call_stub_entry
is memoised in a static; the module reads it at module top to log which path is
live. Java-8-only fixture; call() runs inside a hook detour.
