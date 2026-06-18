---
slug: method_return_types
title: Method Return Types
category: method
status: seeded
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/medium, category/method, tag/method, tag/call, tag/return-types, tag/value-t, tag/basictype, tag/decode, tag/call-stub, tag/call-jni]
---

# Method Return Types

> **Category:** [[categories/method|Method proxies (resolve / call / dispatch)]]  ·  **Status:** `seeded`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/method_return_types-specialist.md`

## Description

The decode of EVERY Java return type back into a C++ value when a native caller
invokes a Java method through `method_proxy::call()`. One Java method per
HotSpot BasicType (`Z B S C I J F D`), `java.lang.String`, and an `Object`/null
returner — each round-tripped through a real interpreter dispatch and asserted
bit-exact on the C++ side, across BOTH dispatch paths (call-stub fast path and
call_jni fallback) and the `value_t` variant that holds the result. The
call-stub decode reads `result_holder` by ret char (Z `&1`; B/S/I/J sign-narrow;
C zero-extend; F/D memcpy; V monostate; String -> read_java_string; other ref ->
encode_oop_pointer into the uint32 alt; null -> monostate). call_jni has parity:
per-ret-char JNIEnv vtable slots, String -> jni_get_string_utf + DeleteLocalRef,
other ref -> jni_decode_object -> encode_oop_pointer, null -> monostate.

## Depends on

- [[features/method_call_primitives|method_call_primitives]]
- [[features/method_proxy_value_t|method_proxy_value_t]]
- [[features/read_java_string|read_java_string]]
- [[features/decode_oop_and_pointers|decode_oop_and_pointers]]

## Related

- [[features/method_call_jni_fallback|method_call_jni_fallback]]
- [[features/method_call_object|method_call_object]]
- [[features/method_call_return_void|method_call_return_void]]
- [[features/method_call_string|method_call_string]]
- [[features/method_call_wide_args|method_call_wide_args]]
- [[features/method_static|method_static]]

## Implementation anchors

- `method_proxy::value_t (variant + conversion operator)` — `vmhook/ext/vmhook/vmhook.hpp:16033-16207` — 11-alternative variant; operator target_type() unique_ptr/std::string/void*/static_cast arms; is_void/is_string/as_string
- `method_proxy::call (call-stub result decode)` — `vmhook/ext/vmhook/vmhook.hpp:16871-17050` — per-ret-char decode switch; reference default arm: String->read_java_string, other ref->encode_oop_pointer into uint32, null->monostate
- `method_proxy::call_jni (return decode parity)` — `vmhook/ext/vmhook/vmhook.hpp:16233-16870` — per-ret-char JNIEnv vtable slots + check_callee_exception; L/[ : String->jni_get_string_utf+DeleteLocalRef, other ref->jni_decode_object->encode_oop_pointer
- `detail::find_call_stub_entry` — `vmhook/ext/vmhook/vmhook.hpp:15936-15970` — StubRoutines::_call_stub_entry; nullptr (norm on CI JDKs) forces the JNI path

## Tests

- `tests/jvm/modules/method_return_types.cpp`

## Notes

Both paths yield the same value_t for primitives; the non-String reference
return differs by path (encode_oop_pointer of a real OOP on the call-stub path
vs jni_decode_object on call_jni — both recover the heap OOP into the uint32
alt). The decode leans on read_java_string, decode_oop_pointer, encode_oop_pointer
and is_valid_pointer. find_call_stub_entry returns nullptr on the CI JDKs, so the
module exercises the call_jni decode primarily but records the live path and
asserts the path-independent value. The module uses one Java method per BasicType
+ String + Object/null returner, asserts bit-exact (F/D memcpy, J full width),
and uses as_string() (not the implicit cast) for the String. Java-8 fixture;
call() runs inside a hook detour.
