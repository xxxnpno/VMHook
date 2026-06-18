---
slug: method_call_primitives
title: Method Call Primitives
category: method
status: seeded
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/medium, category/method, tag/method, tag/call, tag/primitives, tag/value-t, tag/call-stub, tag/call-jni, tag/basictype, tag/void]
---

# Method Call Primitives

> **Category:** [[categories/method|Method proxies (resolve / call / dispatch)]]  ·  **Status:** `seeded`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/method_call_primitives-specialist.md`

## Description

`vmhook::method_proxy::call()` returning every JVM primitive (`Z B S C I J F D`)
and `void`, plus the `value_t` variant that carries the result back into C++.
`call()` probes `find_call_stub_entry()`: if present (typ. JDK 8-20) it builds a
`params[8]` intptr_t slot array, flips the JavaThread to `_thread_in_Java`,
invokes the hand-written call-stub trampoline with the BasicType return id, then
decodes `result_holder` per the return char (Z -> `&1`; B/S/I/J sign-narrow; C
zero-extend; F/D memcpy bit-copy; V -> monostate). If the call stub is absent
(the norm on every CI JDK) it routes to `call_jni()`, which resolves the
cached jclass+jmethodID and calls the matching `Call(Static)?<Type>MethodA`
slot, draining any pending Java exception. Either path yields the SAME
`value_t`, implicitly converted to the target C++ type. Driven only from inside
a hook detour (where `current_java_thread` is set).

## Depends on

- [[features/method_proxy_value_t|method_proxy_value_t]]
- [[features/method_enumeration|method_enumeration]]
- [[features/jni_arg_packing|jni_arg_packing]]

## Related

- [[features/find_methods_by_signature|find_methods_by_signature]]
- [[features/method_explicit_signature|method_explicit_signature]]
- [[features/method_return_types|method_return_types]]
- [[features/method_call_wide_args|method_call_wide_args]]
- [[features/method_call_jni_fallback|method_call_jni_fallback]]
- [[features/method_call_return_void|method_call_return_void]]

## Depended on by

- [[features/method_call_jni_fallback|method_call_jni_fallback]]
- [[features/method_call_object|method_call_object]]
- [[features/method_call_return_void|method_call_return_void]]
- [[features/method_call_string|method_call_string]]
- [[features/method_call_wide_args|method_call_wide_args]]
- [[features/method_overload_java_dispatch|method_overload_java_dispatch]]
- [[features/method_return_types|method_return_types]]
- [[features/method_static|method_static]]
- [[features/method_static_portability|method_static_portability]]
- [[features/method_throwing_call_site|method_throwing_call_site]]

## Implementation anchors

- `method_proxy::call (call-stub fast path + result decode)` — `vmhook/ext/vmhook/vmhook.hpp:16871-17050` — params[8] pack, _thread_in_Java flip, call-stub trampoline, per-return-char decode switch (Z/B/S/C/I/J/F/D/V)
- `method_proxy::call_jni (JNI fallback)` — `vmhook/ext/vmhook/vmhook.hpp:16233-16870` — Call(Static)?<Type>MethodA per-primitive slot pairs (Z=39/119 ... D=60/140, V=63/143); the path taken on every CI JDK
- `method_proxy::value_t (variant + conversion operator)` — `vmhook/ext/vmhook/vmhook.hpp:16033-16207` — 11-alternative variant; templated operator target_type() static_casts the stored alternative; monostate == void/failure
- `detail::find_call_stub_entry` — `vmhook/ext/vmhook/vmhook.hpp:15936-15970` — StubRoutines::_call_stub_entry from VMStructs; nullptr (the norm on CI) forces the call_jni path

## Tests

- `tests/jvm/modules/method_call_primitives.cpp`

## Known bugs

- **[low]** Silent failure on a null primitive JNI slot: every primitive branch in call_jni's switch does `if (!fn) return value_t{monostate}` with NO VMHOOK_LOG, whereas the sibling V and L/[ branches both log a 'slot N is null' diagnostic. A corrupt/unsupported JNI function table only surfaces for void/object returns; an ()I method silently returns a monostate that converts to 0.
- **[low]** case 'F' bit-cast routes through SIGNED int32_t then memcpy — the value is an opaque IEEE-754 bit pattern, not a signed int, and the upper 32 bits of result_holder are call-stub garbage. uint32_t would document intent; case 'D' already uses the full width directly, so F is the odd one out. Same behaviour on the x64 target (clarity, not correctness).
- **[low]** monostate collapses 'void return' and 'dispatch failure': value_t returns monostate for a legitimate ()V success AND for every failure path (no env, malformed sig, null method id, null slot, Java exception), so is_void() cannot tell them apart — a failed ()I and a successful ()V look identical to a caller without is_void().

## Notes

call() first checks find_call_stub_entry(); the converted value_t is identical
on either dispatcher, so the module RECORDS which path is live and asserts the
path-independent value. The only way to drive call() from a test is from inside
a hook detour (current_java_thread is set only while the Java thread executes
inside the interpreter detour), so the module hooks a trigger method and
performs every call() there. sig_char_to_basic_type maps the return-type char to
a HotSpot BasicType id (T_BOOLEAN=4 ... T_VOID=14); V->14 tells the call-stub not
to write a result. Java-8-only fixture, ASCII strings.
