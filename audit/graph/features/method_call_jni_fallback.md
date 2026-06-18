---
slug: method_call_jni_fallback
title: Method Call Jni Fallback
category: method
status: seeded
risk: high
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/high, category/method, tag/method, tag/call, tag/jni, tag/fallback, tag/local-ref, tag/jvalue, tag/call-stub, tag/cache]
---

# Method Call Jni Fallback

> **Category:** [[categories/method|Method proxies (resolve / call / dispatch)]]  ·  **Status:** `seeded`  ·  **Risk:** `high`  ·  **Specialist:** `.claude/agents/method_call_jni_fallback-specialist.md`

## Description

The JNI INVOCATION FALLBACK path of `method_proxy::call()` —
`method_proxy::call_jni()`. `call()` probes `find_call_stub_entry()`
(`StubRoutines::_call_stub_entry`): PRESENT (typ. JDK 8-20) -> interpreter
call-stub fast path; ABSENT (JDK 21+, and on every JDK where the entry isn't
exported via VMStructs — which is what CI exercises) -> `call()`
short-circuits straight into `call_jni()`, resolving + caching jclass +
jmethodID, marshalling args into a `jvalue[]`, and dispatching via the typed
`Call(Static)?<Type>MethodA` JNIEnv slot. The converted `value_t` MUST be
identical on either dispatcher. The crash-sensitive surfaces are: JNI
local-ref discipline (a leaked NewStringUTF / GetStringUTFChars / result ref
fills HotSpot's 16-entry table within ~16 iters and degrades returns), the
union-aliasing footgun (a primitive jvalue cell must never reach
DeleteLocalRef), and method-id/class-handle cache warm-up reuse.

## Depends on

- [[features/method_call_primitives|method_call_primitives]]
- [[features/jni_arg_packing|jni_arg_packing]]
- [[features/jni_local_ref_hygiene|jni_local_ref_hygiene]]

## Related

- [[features/method_call_wide_args|method_call_wide_args]]
- [[features/method_call_object|method_call_object]]
- [[features/method_call_return_void|method_call_return_void]]
- [[features/method_call_string|method_call_string]]
- [[features/method_return_types|method_return_types]]
- [[features/method_static|method_static]]

## Implementation anchors

- `method_proxy::call_jni` — `vmhook/ext/vmhook/vmhook.hpp:16233-16870` — the fallback: resolve+cache jclass/jmethodID, pack jvalue[], dispatch Call(Static)?<Type>MethodA, drain pending exception
- `method_proxy::call (path selection)` — `vmhook/ext/vmhook/vmhook.hpp:16871-16920` — probes find_call_stub_entry(); short-circuits to call_jni after ensure_current_java_thread when the stub is absent
- `detail::find_call_stub_entry` — `vmhook/ext/vmhook/vmhook.hpp:15936-15970` — StubRoutines::_call_stub_entry; nullptr (the CI norm) is what makes call_jni the live path
- `method_proxy::value_t` — `vmhook/ext/vmhook/vmhook.hpp:16033-16207` — the path-independent result the module asserts identical across call-stub vs call_jni

## Tests

- `tests/jvm/modules/method_call_jni_fallback.cpp`

## Notes

call() probes find_call_stub_entry() and the converted value_t is identical on
either dispatcher, so the module RECORDS which path is live (find_call_stub_entry)
and asserts the value — a thorough exercise of call() that NATURALLY drives
call_jni on modern JDKs while staying correct (not skipped) on the legacy
call-stub JDKs. The hazards stressed: detour threads attach and STAY attached
(never pop a JNI frame), so the default 16-entry local-ref table fills within ~16
iterations if a NewStringUTF / GetStringUTFChars / result ref leaks (symptom:
String returns come back "", reference returns null) — the module drives
String-RETURN, String-ARG and long+double MULTI-ARG loops 100+ times asserting
STABLE results; the union-aliasing footgun (a primitive jvalue cell must NEVER be
handed to DeleteLocalRef); cache warm-up (repeated calls reuse cached_method_id /
cached_class_handle without corruption). jmethodID is version-sensitive (JDK 8
raw Method* vs JDK 9+ tagged slot pointer); the jvalue array itself is
JDK-agnostic. Java-8-only fixture, hook trigger(int).
