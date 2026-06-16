---
slug: jni_local_ref_hygiene
title: JNI Local Reference Hygiene
category: jni
status: in_progress
risk: high
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/in_progress, risk/high, category/jni, tag/jni, tag/local-ref, tag/leak, tag/frame, tag/hygiene, tag/detour-thread, tag/table-overflow]
---

# JNI Local Reference Hygiene

> **Category:** [[categories/jni|JNI plumbing (arg packing / refs / java values)]]  ·  **Status:** `in_progress`  ·  **Risk:** `high`  ·  **Specialist:** `.claude/agents/jni_local_ref_hygiene-specialist.md`

## Description

Every JNI call that allocates a local reference (FindClass, GetObjectClass,
NewString/NewStringUTF, CallObjectMethodA, NewObjectA) increments the JNI
local-reference table on the calling thread. HotSpot's default table capacity
is 16 entries; attached detour threads never push or pop a JNI frame and receive
no implicit per-call teardown, so each unmatched reference permanently consumes
a slot. Once the table is full, HotSpot emits "JNI local reference table overflow"
and every subsequent allocating call returns null, silently degrading String
returns to empty, object returns to monostate, and injected String args to no-ops.
The contract is: every local reference produced by vmhook's JNI helpers must be
released via jni_delete_local_ref (JNI slot 23) on every exit path before the
detour returns. The string_handle_cleanup RAII covers String args in call_jni,
the 'L'/'[' arm releases result_handle after decoding, and set_arg/store_string
release the NewString handle after OOP extraction. Violations are silent until
table saturation; the observable symptom is degraded return values after ~16
iterations in a tight loop on a long-lived attached thread.

## Related

- [[features/global_ref|global_ref]]

## Depended on by

- [[features/global_ref|global_ref]]
- [[features/make_java_string|make_java_string]]
- [[features/method_call_jni_fallback|method_call_jni_fallback]]

## Implementation anchors

- `vmhook::detail::jni_delete_local_ref` — `vmhook/ext/vmhook/vmhook.hpp:11418-11432` — Implementation of DeleteLocalRef via JNI slot 23. Null handle is a safe no-op. This is the single release site every local-ref producer must call on every exit path, including early-return error branches.

- `method_proxy::call_jni string_handle_cleanup RAII` — `vmhook/ext/vmhook/vmhook.hpp:16372-16388` — RAII struct that releases NewString local refs created for String args. Uses a per-slot needs_release tag (not value.l) to avoid the union-aliasing footgun where a primitive jvalue cell would be handed to DeleteLocalRef as a garbage pointer.

- `method_proxy::call_jni 'L'/'[' result_handle release` — `vmhook/ext/vmhook/vmhook.hpp:16704-16737` — CallObjectMethodA result_handle is released after jni_get_string_utf (String arm, line 16720) and after jni_decode_object (generic object arm, line 16737). The v0.4.3 fix; skipping either release leaked one local ref per call.

- `method_proxy::call_jni cached_class_handle (FindClass / GetObjectClass)` — `vmhook/ext/vmhook/vmhook.hpp:16276-16310` — LEAK SUSPECT: class_handle from jni_find_class (static path, line 16276) and jni_get_object_class (instance path, line 16299) are stored in cached_class_handle (line 17568) and reused across calls. The jclass local reference is never DeleteLocalRef'd. On the first call, one slot is consumed permanently for the lifetime of the method_proxy.

- `inherit_host_context_classloader_for_current_thread thread_class / current_thread` — `vmhook/ext/vmhook/vmhook.hpp:12253-12319` — LEAK SUSPECT: thread_class (jni_find_class, line 12274) and current_thread (jni_call_static_object_method, line 12291) are never DeleteLocalRef'd on any exit path. Called once per thread attach, so two slots are permanently lost per attached thread.

- `jni_find_class_with_context_loader local_ref_bag RAII` — `vmhook/ext/vmhook/vmhook.hpp:11966-12135` — Clean pattern: every local ref acquired during the classloader walk is tracked in a local_ref_bag vector whose destructor calls DeleteLocalRef on each handle. PushLocalFrame / PopLocalFrame guards supplement the bag as a belt-and-suspenders strategy for code paths that run close to the 16-entry default table limit.


## Tests

- `tests/jvm/modules/jni_local_ref_hygiene.cpp`

## Audit docs

- `audit/findings/jni_delete_local_ref_table_slot_23.md`
- `audit/findings/method_proxy_call_jni_local_ref_leaks.md`

## Known bugs

- **[high]** cached_class_handle holds a raw JNI local reference from FindClass (static dispatch, line 16276) or GetObjectClass (instance dispatch, line 16299) that is stored in method_proxy::cached_class_handle (line 17568) and never released. On a long-lived detour thread each unique method_proxy whose call_jni path resolves the class for the first time permanently occupies one local-ref slot. With 16 slots total, 16 distinct method proxies will saturate the table even if every other site is clean.
  (`audit/findings/method_proxy_call_jni_local_ref_leaks.md`)
- **[medium]** inherit_host_context_classloader_for_current_thread (lines 12253-12319) leaks two local refs per thread attach: thread_class from jni_find_class (line 12274) and current_thread from jni_call_static_object_method (line 12291). Neither is passed to jni_delete_local_ref on any exit path, including the early-return branches at lines 12277 and 12287. Called once per attach so not a tight-loop hazard, but the two slots are consumed immediately on the first call, shrinking the effective table budget from 16 to 14 for all subsequent operations on that thread.
  (`audit/findings/jni_delete_local_ref_table_slot_23.md`)
- **[high]** Union-aliasing footgun: jni_value is a union whose members (.l, .j, .i, .d) share storage. Any non-zero primitive argument written by write_jni_arg_to_slot leaves bits in .l that look like a non-null pointer. A naive cleanup loop that reads value.l to decide whether to DeleteLocalRef will hand garbage to slot 23 for every non-zero primitive arg, causing an assertion or access violation on debug/fastdebug HotSpot builds. The current string_handle_cleanup at line 16372 avoids this via the separate needs_release tag; any future cleanup loop that reverts to reading value.l directly will re-introduce this class of crash.
  (`audit/findings/method_proxy_call_jni_local_ref_leaks.md`)

## Notes

JNI local-frame limit: HotSpot's default local-reference table capacity is 16
entries per thread. The value is a JVM implementation detail, not a JNI spec
constant; it can be raised with -XX:+IgnoreUnrecognizedVMOptions or by calling
EnsureLocalCapacity (slot 26), but vmhook does not call either, so 16 is the
effective budget for every attached detour thread.

JDK-version differences in enforcement: JDK 8 and 9 log "JNI local reference
table overflow" to stderr but continue, returning null from the failing allocator.
JDK 17+ with -Xcheck:jni (the fastdebug HotSpot configuration used in CI) promotes
the overflow to a fatal error on some builds. Debug builds on all JDKs assert inside
the JNI table implementation. This means the observable symptoms differ: on JDK 8
release the caller sees null/empty returns (detectable by the stability assertions
in the test module); on JDK 17+ fastdebug the process aborts. Both are covered by
the jni_local_ref_hygiene test module's 100-iteration stability loops.

PushLocalFrame / PopLocalFrame (slots 19 / 20, lines 11507-11572) are used
defensively in jni_find_class_with_context_loader as a belt-and-suspenders
supplement to the local_ref_bag RAII. They are NOT used around method_proxy::call_jni
or inherit_host_context_classloader_for_current_thread, which is why the leaks at
those sites matter.

The needs_release tag vector (line 16344, std::vector<char> not <bool> to ensure
addressable elements) is the canonical way to track which jvalue slots hold real
JNI local refs without reading value.l back through the union.
