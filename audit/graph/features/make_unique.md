---
slug: make_unique
title: Make Unique
category: infra
status: seeded
risk: high
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/high, category/infra, tag/infra, tag/object, tag/allocation, tag/newobjecta, tag/tlab, tag/construct, tag/jni, tag/register-class]
---

# Make Unique

> **Category:** [[categories/infra|Infrastructure (wrappers, traits, macros, logging)]]  ·  **Status:** `seeded`  ·  **Risk:** `high`  ·  **Specialist:** `.claude/agents/make_unique-specialist.md`

## Description

`vmhook::make_unique<wrapper_type>(args...)` allocates a fresh Java object from
native code with no-arg / int / multi-arg / String constructors and returns a
`std::unique_ptr<wrapper_type>`. Flow: `ensure_current_java_thread()` (HotSpot
needs a live JavaThread — captured inside a detour); resolve the JVM class name
from `type_to_class_map` by `typeid(T)` (so `register_class<T>("pkg/Cls")` is
mandatory first); PREFER `detail::jni_make_unique` (NewObjectA, which runs the
REAL Java `<init>` chain) — returned immediately if non-null; only on a null
NewObjectA does the TLAB fallback run (raw allocate, zero, stamp oopDesc
header, wrap, then dispatch `construct(args...)` if the wrapper declares a
matching one). Critical semantic: `construct()` runs ONLY on the TLAB fallback;
the NewObjectA path never calls it. Arg conversion + the `(...)V` descriptor go
through `append_jni_arg` / `jni_signature_for_arg`, with `jni_arg_cleanup`
releasing only real jstring local refs.

## Depends on

- [[features/wrapper_pattern|wrapper_pattern]]
- [[features/jni_arg_packing|jni_arg_packing]]
- [[features/register_class|register_class]]

## Related

- [[features/jni_arg_packing|jni_arg_packing]]
- [[features/jni_local_ref_hygiene|jni_local_ref_hygiene]]
- [[features/make_java_array|make_java_array]]
- [[features/make_java_string|make_java_string]]
- [[features/method_call_object|method_call_object]]

## Implementation anchors

- `vmhook::make_unique (free function)` — `vmhook/ext/vmhook/vmhook.hpp:13743-13831` — ensure_current_java_thread, type_to_class_map lookup, prefer jni_make_unique, TLAB fallback + construct() dispatch (13818)
- `detail::jni_make_unique (NewObjectA backbone)` — `vmhook/ext/vmhook/vmhook.hpp:13202-13330` — find_class -> mirror -> make_jni_args -> (...)V descriptor -> GetMethodID('<init>') (null -> nullptr -> fallback) -> NewObjectA slot 30 -> decode oop
- `construct() detection (if constexpr requires)` — `vmhook/ext/vmhook/vmhook.hpp:13818-13828` — requires{ w.construct(a...) } probe; invoked only on the TLAB fallback, else a warning_tag log if any args were passed
- `detail::append_jni_arg / jni_signature_for_arg` — `vmhook/ext/vmhook/vmhook.hpp:12772-13140` — per-type compile-time arg conversion + the descriptor twin; value.j=0 union clear before narrow write
- `detail::jni_arg_cleanup (local-ref hygiene)` — `vmhook/ext/vmhook/vmhook.hpp:13237-13258` — arg_needs_release tag array drives DeleteLocalRef so ONLY real jstrings are released; NewObjectA result handle also released after decode

## Tests

- `tests/jvm/modules/make_unique.cpp`
- `tests/test_make_unique.cpp`

## Known bugs

- **[high]** make_unique<T>("string literal") fails to compile under GCC -Werror: a raw string-literal arg deduces to const char(&)[N], decays to const char*, and hits the const char* branch in append_jni_arg whose `arg ? ... : ...` null check fires -Werror=address + -Werror=nonnull-compare (the address of a literal is never null); the requires-instantiated construct forwarding trips it too. Breaks the warnings-as-errors CI job and any downstream -Werror user. Hit live building the build-werror (MinGW g++ -Wall -Wextra -Wpedantic -Werror) DLL. Fix: dispatch char* vs const char(&)[N] separately / decay-to-pointer through a helper before the ternary.
- **[medium]** No exception/pending-exception clear after a failed NewObjectA: when the Java <init> throws (a validating constructor), NewObjectA returns null, jni_decode_object yields null, and jni_make_unique returns nullptr WITHOUT calling ExceptionClear. The pending JNI exception is left set on the thread; the next JNI call asserts 'JNI call made with exception pending' under -Xcheck:jni and on fastdebug HotSpot can abort. Fix: clear the pending exception on the null-OOP path.
- **[medium]** TLAB fallback bypasses the Java <init> entirely -> partial objects: when NewObjectA is unavailable (JDK 21+ where StubRoutines::_call_stub_entry is missing), the object is raw-allocated and only construct() runs. Superclass fields the Java constructor would set are left zero unless the wrapper's construct() re-creates that work by hand. By-design but a sharp edge: omitting construct() for a constructor that does meaningful work gives a silently half-initialised object (only a warning_tag log).
- **[low]** construct() detection is arity/convertibility-based, so an unintended overload can capture the fallback: the requires probe matches any construct(args...) reachable by overload resolution, so a wrapper declaring construct(long) satisfies the probe for an int fallback via promotion, running the wrong initializer silently. Wrappers should declare exactly the construct overloads they intend.

## Notes

NewObjectA vs TLAB selection is JDK-dependent: on JDK <=20 with _call_stub_entry
present every standard descriptor resolves through NewObjectA; on JDK 21+ the
TLAB/call_jni path is degraded — but NewObjectA itself is a JNIEnv slot-30
function and works regardless, so the NewObjectA angles hold across JDK 8-25.
construct() runs ONLY on the TLAB fallback (the NewObjectA path's Java
constructor already did the work), so the only way to exercise construct() is to
force the fallback by passing an arg whose (descriptor)V has no matching Java
<init> (the test fixture deliberately omits (Z)V so a bool arg forces it).
Compressed klass/oop header stamping in the TLAB path reads
oopDesc._metadata._compressed_klass vs _klass from VMStructs (uncompressed arm on
huge heaps). NewStringUTF is modified-UTF-8; the multibyte test angle stays in
the BMP so the round-trip is exact. The no-JVM contract is pinned by
tests/test_make_unique.cpp; the live constructor/field-readback/construct()
coverage is in tests/jvm/modules/make_unique.cpp.
