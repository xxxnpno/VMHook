---
slug: method_static_portability
title: Method Static Portability
category: method
status: seeded
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/medium, category/method, tag/method, tag/static, tag/portability, tag/compiler, tag/deducing-this, tag/slot-0, tag/call-stub, tag/call-jni]
---

# Method Static Portability

> **Category:** [[categories/method|Method proxies (resolve / call / dispatch)]]  ·  **Status:** `seeded`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/method_static_portability-specialist.md`

## Description

The PORTABILITY of the static-method CALL path — `static_method("name")->call(args)`
and `static_method("name","sig")->call(args)` — across EVERY compiler
(MSVC/Clang/GCC), every return type, and every argument shape. This is the
no-receiver dispatch path: the call-stub fast path
(`StubRoutines::_call_stub_entry`, JDK 8-20) or the `CallStatic<T>MethodA`
call_jni fallback (JDK 21+); both must produce the IDENTICAL converted `value_t`.
The portability guarantee: on GCC the deducing-this static fallback is NOT
emitted from a static wrapper method, so the ONLY portable entry is
`static_method(name[,sig])` — the module and any fixture wrapper use that name
exclusively. Slot-0 alignment (no phantom `this` shifting args) is the proof the
no-receiver frame is laid out right: static recorders stamp the args they
actually saw and correct values prove it.

## Depends on

- [[features/method_static|method_static]]
- [[features/method_call_primitives|method_call_primitives]]

## Related

- [[features/method_static|method_static]]
- [[features/method_overload|method_overload]]
- [[features/method_return_types|method_return_types]]
- [[features/method_call_wide_args|method_call_wide_args]]

## Implementation anchors

- `object<T>::static_method(name) / (name, signature)` — `vmhook/ext/vmhook/vmhook.hpp:18797-18815` — the portable factories — the only static entry GCC emits from a static wrapper method
- `object_base::get_method(type_index, name[, signature]) (static path)` — `vmhook/ext/vmhook/vmhook.hpp:18072-18200` — built with object==nullptr; resolve_compatible_method derives the static klass via the Method's ConstantPool _pool_holder (the recently-fixed crash: a primitive blasted into a reference slot used to AV; it now re-picks)
- `method_proxy::is_static / call / call_jni` — `vmhook/ext/vmhook/vmhook.hpp:16233-17240` — is_static reads JVM_ACC_STATIC live (17209); call (16871) / call_jni (16233) produce the path-independent value_t; receiver OOP is 0 for static
- `detail::find_call_stub_entry` — `vmhook/ext/vmhook/vmhook.hpp:15936-15970` — selects call-stub vs CallStatic<T>MethodA call_jni; the module records the live path and asserts the path-independent value

## Tests

- `tests/jvm/modules/method_static_portability.cpp`

## Notes

On GCC the deducing-this static fallback is not emitted from a static wrapper
method, so static_method(name[,sig]) is the only portable entry; brace-init from
->call()/value_t is an MSVC ambiguity hazard (use copy-init r.sval =
v.as_string()). resolve_compatible_method derives the static klass via the
Method's ConstantPool _pool_holder — the recently-fixed AV (a primitive blasted
into a reference slot) now re-picks. The module exercises every return-type decode
(void + Z B C S I J F D at boundary values, String ASCII/UTF-8/empty/null via
as_string, object ref -> null on every path), every arg shape (no-arg, primitive
incl. I/J/D two-slot, String, object, a 4-arg int+long+double+int frame), the
explicit-overload factory, and overload resolution on the portable static path.
Java-8-only fixture; call() runs inside a scoped_hook detour; object usability
recorded as [INFO] (call-path dependent).
