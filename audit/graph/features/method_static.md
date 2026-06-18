---
slug: method_static
title: Method Static
category: method
status: seeded
risk: high
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/high, category/method, tag/method, tag/static, tag/no-receiver, tag/callstatic, tag/pool-holder, tag/is-static, tag/slot-0]
---

# Method Static

> **Category:** [[categories/method|Method proxies (resolve / call / dispatch)]]  ·  **Status:** `seeded`  ·  **Risk:** `high`  ·  **Specialist:** `.claude/agents/method_static-specialist.md`

## Description

The static-method call surface reached through `object<T>::static_method("name")`
(and the `(name, signature)` overload), invoked as
`static_method("m")->call(args...)` from inside a live interpreter detour. A
static dispatch differs from an instance one end to end: no `this` is pushed, the
first declared argument lands at parameter slot 0, the receiver OOP is null, the
JVM dispatch uses HotSpot's `CallStatic*` JNI slots (or the call-stub fast path),
and `method_proxy::is_static()` must report the JVM truth.
`object_base::get_method(type_index, name[, sig])` resolves the klass from the
registered wrapper, walks the superclass chain, and constructs `method_proxy{
nullptr, method, sig }` (receiver deliberately null). The receiver is pushed into
params[0] ONLY when `object && !static_field`, so for a static proxy slot 0 is the
first real arg. call_jni derives `is_static_call` as `object == nullptr` and uses
`_pool_holder` name -> FindClass -> GetStaticMethodID.

## Depends on

- [[features/method_call_primitives|method_call_primitives]]
- [[features/method_proxy_value_t|method_proxy_value_t]]

## Related

- [[features/method_call_jni_fallback|method_call_jni_fallback]]
- [[features/method_call_object|method_call_object]]
- [[features/method_call_return_void|method_call_return_void]]
- [[features/method_call_string|method_call_string]]
- [[features/method_call_wide_args|method_call_wide_args]]
- [[features/method_return_types|method_return_types]]
- [[features/method_static_portability|method_static_portability]]
- [[features/method_overload|method_overload]]

## Depended on by

- [[features/method_static_portability|method_static_portability]]

## Implementation anchors

- `object::static_method(name) / (name, signature)` — `vmhook/ext/vmhook/vmhook.hpp:18797-18815` — portable static-method entries; forward to the type_index-keyed object_base::get_method
- `object_base::get_method(type_index, name[, signature]) (static resolution)` — `vmhook/ext/vmhook/vmhook.hpp:18072-18200` — resolve_klass(wrapper_type), superclass walk, FIRST name match (name-only) / name+sig match, method_proxy{nullptr,...}; FLAW: JVM_ACC_STATIC never checked
- `method_proxy::call (receiver-slot gating)` — `vmhook/ext/vmhook/vmhook.hpp:16871-17050` — receiver pushed into params[0] ONLY when object && !static_field; for a static proxy object==nullptr so slot 0 is the first real arg
- `method_proxy::call_jni (CallStatic* dispatch) / is_static` — `vmhook/ext/vmhook/vmhook.hpp:16233-16870` — is_static_call = object==nullptr; static branch derives jclass from ConstantPool::_pool_holder name + FindClass + GetStaticMethodID; is_static() at 17209 reads JVM_ACC_STATIC live

## Tests

- `tests/jvm/modules/method_static.cpp`

## Known bugs

- **[high]** Static resolution is name-only; JVM_ACC_STATIC is never checked at the resolution site (get_method(type_index, name) and the name+sig overload). The static_field member of method_proxy is a DEAD input for methods (no caller ever sets it true), which is why is_static() reading JVM_ACC_STATIC live is load-bearing; a name match that hit a non-static method would still be built as a static proxy.
- **[medium]** Static-overload re-resolution is dead on the call path (shared with method_overload): a static proxy is built with object==nullptr and resolve_compatible_method bails on it, so a name-only static proxy never re-picks among overloads — only the explicit static_method(name,sig) path (which pins) is reliable for overloaded statics.

## Notes

Dispatch fork on find_call_stub_entry(): absent (the JDK-21+ default, where the
call-stub VMStruct is gone) -> call_jni. On call_jni the static branch derives
the jclass from the Method's ConstantPool::_pool_holder name + FindClass then
GetStaticMethodID; primitive decode parity with the instance path (Z masks &1, C
zero-extends u16, F/D bit-copied), reference decode: null -> monostate, String ->
read_java_string, other ref -> encode_oop_pointer -> u32. Slot-0 alignment (no
phantom this shifting args) is proven by static recorders stamping the args they
actually saw. Java-8-only fixture; static calls ALWAYS via static_method(name[,sig])
(the GCC-portable entry), never the deducing-this fallback; call() runs inside a
scoped_hook detour.
