---
slug: global_ref
title: Global Ref (JNI GC-survival pin)
category: jni
status: in_progress
risk: high
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/in_progress, risk/high, category/jni, tag/jni, tag/global-ref, tag/gc-pin, tag/raii, tag/move-only]
---

# Global Ref (JNI GC-survival pin)

> **Category:** [[categories/jni|JNI plumbing (arg packing / refs / java values)]]  ·  **Status:** `in_progress`  ·  **Risk:** `high`  ·  **Specialist:** `.claude/agents/global_ref-specialist.md`

## Description

`vmhook::jni::global_ref` is a move-only RAII wrapper around a JNI global reference that
keeps a Java object alive across relocating garbage collections.  Input: a raw decoded heap
OOP (e.g. from a field read, `method_proxy::call()`, or `wrapper->get_instance()`).
Output: an owning handle whose `oop()` always re-derives the object's CURRENT heap address
out of the global-ref slot, masking the JDK 9+ low-3-bit handle-tag so the deref is
well-aligned on every modern JDK.  The constructor calls `NewGlobalRef` (JNI table slot 21)
via `detail::jni_new_global_ref`; the destructor and `reset()` call `DeleteGlobalRef`
(slot 22) exactly once; the class is non-copyable to prevent double-release.
Unlike a local ref, this handle survives JNI frame pops, cross-thread hand-off, and
relocation; unlike `vmhook::object<T>` (which stores a decoded raw OOP that goes stale
after any relocating GC), `global_ref` holds a JVM-maintained slot so the live address is
always recovered through `oop()` rather than cached.  The convenience overloads
`vmhook::pin(oop_t)` and `vmhook::pin(unique_ptr<T>)` are thin wrappers around construction.

## Depends on

- [[features/jni_local_ref_hygiene|jni_local_ref_hygiene]]
- [[features/decode_oop_and_pointers|decode_oop_and_pointers]]

## Related

- [[features/make_java_string|make_java_string]]
- [[features/wrapper_pattern|wrapper_pattern]]

## Implementation anchors

- `vmhook::jni::global_ref` — `vmhook/ext/vmhook/vmhook.hpp:21228-21318` — complete move-only class — ctor (NewGlobalRef), dtor/reset (DeleteGlobalRef), oop() tag-mask deref, handle(), operator bool
- `vmhook::detail::jni_new_global_ref` — `vmhook/ext/vmhook/vmhook.hpp:11446-11457` — JNI table slot 21 — null-guards input; no JVM returns nullptr
- `vmhook::detail::jni_delete_global_ref` — `vmhook/ext/vmhook/vmhook.hpp:11468-11482` — JNI table slot 22 — null handle is an explicit no-op; double-release corrupts the handle table
- `vmhook::detail::jni_oop_handle` — `vmhook/ext/vmhook/vmhook.hpp:11499-11504` — creates a stack-allocated synthetic local handle from a raw OOP so the JNI table functions can consume it
- `vmhook::pin (oop_t overload + unique_ptr overload)` — `vmhook/ext/vmhook/vmhook.hpp:21325-21345` — convenience factory; unique_ptr overload static_asserts T derives from object_base

## Tests

- `tests/jvm/modules/global_ref.cpp`
- `tests/test_global_ref.cpp`

## Known bugs

- **[high]** jni_delete_global_ref silently skips the JNI call when current_jni_env has no table entry for slot 22 (e.g. during early bootstrap or after a detach), leaving the handle leaked with no diagnostic — the destructor returns without releasing the pin, so the object is never collected.
- **[high]** oop() tag-mask is hardcoded to 0b111 (3 bits); if a future HotSpot build widens the tag to 4+ bits the mask is insufficient and oop() returns a misaligned, garbage address that will AV on first field read — no version guard or assertion exists.
- **[medium]** The synthetic jni_oop_handle stack slot (handle_storage in the constructor body, line 21240) is in-scope for the NewGlobalRef call but there is no fence preventing the compiler from eliminating it as unused after the assignment; on aggressive optimisation levels this is undefined behaviour (reading through a pointer to a destroyed local).
- **[medium]** global_ref has no jclass / jarray distinction — a jclass global ref and a jobject global ref share the same handle_ field and oop() path; code that attempts to pass handle() back to a JNI function expecting specifically jclass (e.g. FindClass cache) gets silent undefined-behaviour from the implicit C cast.
- **[low]** reset() calls jni_delete_global_ref then nulls handle_, but if the detour thread is detached at the time of reset() (current_jni_env == nullptr), jni_delete_global_ref silently no-ops (slot-22 lookup fails), the handle is not released, and handle_ is set to nullptr — effectively a silent leak with no log line.

## Notes

The comment block immediately above the jni namespace (lines 21195-21201) documents the
use-after-relocation hazard that motivates global_ref; it is a useful anchor for reviewers
unfamiliar with the feature.

jni_oop_handle is a deliberate ABI shim: JNI functions expect a pointer-to-OOP (jobject is
void**, not void*), so passing the raw OOP directly would be a type mismatch.  The stack
storage trick avoids allocating a real JNI local frame but means the synthetic handle
lifetime is bounded to the constructor scope — NewGlobalRef must be called before the
constructor returns, which it is.

The move-assign path correctly releases the *current* handle before taking ownership of
other.handle_ (line 21262), so no double-release occurs on move-assign.  Self-move is
guarded by the `this != &other` check (line 21260), leaving handle_ intact.

wrapper_pattern (object<T>) stores a raw decoded OOP; global_ref stores a JVM-maintained
slot.  They serve different lifetime requirements: object<T> is valid for the duration of
a single detour tick; global_ref is valid until explicitly released.
