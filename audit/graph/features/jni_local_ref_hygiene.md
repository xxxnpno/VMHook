---
slug: jni_local_ref_hygiene
title: Jni Local Ref Hygiene
category: jni
status: seeded
risk: high
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/high, category/jni, tag/jni, tag/local-ref, tag/lifecycle, tag/delete-local-ref, tag/leak, tag/safety, tag/detour]
---

# Jni Local Ref Hygiene

> **Category:** [[categories/jni|JNI plumbing (arg packing / refs / java values)]]  ·  **Status:** `seeded`  ·  **Risk:** `high`  ·  **Specialist:** `.claude/agents/jni_local_ref_hygiene-specialist.md`

## Description

vmhook's JNI LOCAL-REFERENCE discipline: the proof that vmhook does NOT leak
JNI local references on the paths that create them, when those paths run from a
long-lived attached detour thread in a tight loop far past HotSpot's default
16-entry local-ref table. Detour threads attach and STAY attached — they never
push/pop a JNI frame, so there is no implicit per-call teardown; every
NewStringUTF / Call(Static)?ObjectMethodA / FindClass / GetObjectClass ref must
be released via `JNIEnv::DeleteLocalRef` (`detail::jni_delete_local_ref`,
table slot 23) or the table fills, NewStringUTF/CallObjectMethodA start
returning null, and String returns come back "" / reference returns become
null. Also pins the union-aliasing footgun: a primitive `jvalue` cell must
NEVER be handed to DeleteLocalRef (only entries tagged `needs_release` are).
Observable contract: results stay CORRECT on every one of 100+ iterations.

## Depends on

- [[features/jni_arg_packing|jni_arg_packing]]

## Related

- [[features/jni_arg_packing|jni_arg_packing]]
- [[features/method_call_string|method_call_string]]
- [[features/method_call_object|method_call_object]]
- [[features/return_set_arg|return_set_arg]]
- [[features/make_unique|make_unique]]

## Depended on by

- [[features/find_class_context_loader|find_class_context_loader]]
- [[features/global_ref|global_ref]]
- [[features/make_java_string|make_java_string]]
- [[features/method_call_jni_fallback|method_call_jni_fallback]]
- [[features/method_call_string|method_call_string]]
- [[features/wrapper_pattern|wrapper_pattern]]

## Implementation anchors

- `detail::jni_delete_local_ref (table slot 23)` — `vmhook/ext/vmhook/vmhook.hpp:11477-11500` — JNIEnv::DeleteLocalRef wrapper; forward-declared at 9547. The single release point.
- `detail::jni_new_string_utf` — `vmhook/ext/vmhook/vmhook.hpp:11775-11800` — NewStringUTF local-ref source: each arg-String / set_arg-String creates a ref to release
- `method_proxy::call_jni (string_handle_cleanup + arg_needs_release)` — `vmhook/ext/vmhook/vmhook.hpp:16233-16450` — stack-path arg packing with a RAII cleanup keyed on arg_needs_release[] (16432); deletes only real jstrings, never an aliased primitive cell
- `return_value::set_arg (String NewStringUTF + DeleteLocalRef)` — `vmhook/ext/vmhook/vmhook.hpp:9817-10010` — the v0.4.x set_arg(String) leak fix: NewStringUTF ref is DeleteLocalRef'd after the slot write
- `detail::jni_arg_cleanup (jni_make_unique path)` — `vmhook/ext/vmhook/vmhook.hpp:13237-13258` — DeleteLocalRefs slot i iff arg_needs_release[i]; the make_unique-path mirror of the same discipline

## Tests

- `tests/jvm/modules/jni_local_ref_hygiene.cpp`

## Audit docs

- `audit/findings/jni_delete_local_ref_table_slot_23.md`
- `audit/findings/method_proxy_call_jni_local_ref_leaks.md`

## Notes

The local-ref-creating paths exercised (each 100+ times, asserting STABLE
results): call() String return (CallObjectMethodA jstring), call() returning a
FRESH String each call (no constant-pool reuse to mask a leak), call(String
arg) (NewStringUTF arg + returned jstring = TWO refs/iter), call() Object/array
return (CallObjectMethodA on the L/[ arm), STATIC dispatch (FindClass jclass),
INSTANCE dispatch (GetObjectClass jclass), and return_value::set_arg(String)
(NewStringUTF + DeleteLocalRef). A leak surfaces as the benign table-overflow
warning + degraded return values (caught as [FAIL] by the stability asserts),
NEVER an access violation. Loops are bounded; the module installs scoped_hooks
only (never shutdown_hooks()), runs call() inside a hook detour (where
current_java_thread is set), and is Java-8-only / ASCII. Every decoded-OOP
deref is is_valid_pointer-gated.
