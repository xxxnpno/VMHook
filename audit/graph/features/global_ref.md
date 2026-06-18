---
slug: global_ref
title: Global Ref
category: jni
status: in_progress
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/in_progress, risk/medium, category/jni, tag/jni, tag/global-ref, tag/gc-survival, tag/move-only, tag/raii, tag/handle-tag-mask, tag/new-global-ref, tag/delete-global-ref, tag/pin]
---

# Global Ref

> **Category:** [[categories/jni|JNI plumbing (arg packing / refs / java values)]]  ·  **Status:** `in_progress`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/global_ref-specialist.md`

## Description

`vmhook::jni::global_ref` — the move-only RAII pin that keeps a Java object alive across a relocating
garbage collection. Its constructor promotes a raw decoded OOP to a JNI global reference
(`NewGlobalRef`, vtable slot 21); the destructor / `reset()` release it exactly once
(`DeleteGlobalRef`, slot 22); and `.oop()` re-derives the object's CURRENT (post-relocation) heap
address from the handle slot on every call, masking the JDK 9+ JNI handle low-3-bit tag so the deref is
well-aligned on modern JDKs (a no-op on the untagged JDK 8 handle). Copy is statically deleted; move
transfers ownership and empties the source. `vmhook::pin(oop)` / `pin(unique_ptr<wrapper>)` are the
one-liner factories (a null wrapper yields an empty, falsy pin). Null / empty pins are safe: `.oop()`
is null and `reset()` issues no JNI call.

## Depends on

- [[features/jni_local_ref_hygiene|jni_local_ref_hygiene]]

## Related

- [[features/make_java_string|make_java_string]]
- [[features/find_class_context_loader|find_class_context_loader]]

## Depended on by

- [[features/wrapper_pattern|wrapper_pattern]]

## Implementation anchors

- `vmhook::jni::global_ref class` — `vmhook/ext/vmhook/vmhook.hpp:21316-21407` — move-only RAII pin — ctor NewGlobalRef (21321-21330), dtor DeleteGlobalRef (21332-21335), copy deleted (21337-21338), move ctor + move-assign release-then-steal (21340-21360)
- `global_ref::oop()` — `vmhook/ext/vmhook/vmhook.hpp:21362-21381` — null handle -> nullptr; else masks low 3 bits of the JDK 9+ tagged handle (& ~0b111) to recover the 8-byte-aligned OopStorage slot, then derefs *slot to the CURRENT post-GC address (no-op mask on untagged JDK 8)
- `global_ref::reset()` — `vmhook/ext/vmhook/vmhook.hpp:21385-21390` — idempotent early release — DeleteGlobalRef then null the handle; oop() returns nullptr after
- `detail::jni_new_global_ref / jni_delete_global_ref` — `vmhook/ext/vmhook/vmhook.hpp:11505-11542` — JNIEnv vtable slot 21 (NewGlobalRef) / slot 22 (DeleteGlobalRef); null input / no JVM returns nullptr / no-ops
- `vmhook::pin(oop) / pin(unique_ptr<wrapper>)` — `vmhook/ext/vmhook/vmhook.hpp:21413-21439` — factories — pin(oop) returns global_ref{oop}; pin(wrapper) static_asserts T derives object_base and returns an empty pin for a null wrapper

## Tests

- `tests/jvm/modules/global_ref.cpp`
- `tests/test_global_ref.cpp`

## Known bugs

- **[low]** oop() masks the low 3 bits of the JNI handle to recover the OopStorage slot (vmhook.hpp:21378-21379). This is correct on JDK 9+ (tagged, 8-byte-aligned handles) and a no-op on JDK 8 (untagged). The mask width is hard-coded to 3 bits; a future JDK that widens the handle tag would mis-align the slot — characterised by reading the sentinel back through .oop() on every supported JDK so a tag-mask drift trips the test.
- **[low]** Move-assignment must release the current handle BEFORE stealing the source's (vmhook.hpp:21346-21358) or the old global ref leaks. Pinned by the move-only contract test (move-construct/move-assign empty the source, no double DeleteGlobalRef, self-move leaves the handle intact); copy is compile-time deleted (static_assert in the specialist's traits).

## Notes

Every JNI-touching step (make_unique, NewGlobalRef, DeleteGlobalRef) needs a live JavaThread + attached
JNIEnv, which the detached worker lacks — so the live-JVM module runs all of it INSIDE a scoped_hook
detour on trigger(): the surviving pin lives in a file-scope global_ref to persist across the
phase-1/phase-2 probe boundary and is released explicitly inside the phase-2 detour (on a live JNIEnv)
to exercise the real DeleteGlobalRef path. The no-JVM test (tests/test_global_ref.cpp) pins the move-only
/ copy-deleted type traits as compile-time static_asserts. Proofs: build + pin a probe with a known
sentinel, read it back through .oop() (functional proof, never a raw-address identity assert since a bare
OOP goes stale after GC while .oop() tracks relocation), drop the wrapper so the pin is the only
keep-alive; SURVIVE GC (Java forces System.gc() several times, the relocated object's sentinel still reads
through the SAME pin's .oop(); the numeric address is ALLOWED to differ pre/post GC, recorded [INFO]);
null / empty pins are falsy with null .oop() and no-op reset(). The handle-tag mask is the only JDK-shaped
behaviour (no-op on 8, load-bearing on 11-26). .oop() is additionally guarded by is_valid_pointer before
any field read; never calls shutdown_hooks(); Java-8-only fixture; MSVC copy-init.
