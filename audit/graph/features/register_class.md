---
slug: register_class
title: Register Class
category: klass
status: in_progress
risk: high
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/in_progress, risk/high, category/klass, tag/registry, tag/type-map, tag/klass, tag/jni, tag/factory, tag/setup, tag/type-index]
---

# Register Class

> **Category:** [[categories/klass|Class / Klass introspection]]  ·  **Status:** `in_progress`  ·  **Risk:** `high`  ·  **Specialist:** `.claude/agents/register_class-specialist.md`

## Description

`vmhook::register_class<T>(class_name)` is the process-wide binding step that makes a
C++ wrapper type visible to every SDK API that resolves klasses, methods, and fields by
C++ type.  Input: a slash-delimited internal Java class name (e.g. "com/example/Foo");
output: two simultaneous entries — `type_to_class_map[typeid(T)] = name` and
`g_type_factory_map[name] = lambda{ return new T{oop}; }` — installed under
`registration_mutex`.  Before writing either entry the routine calls `find_class(name)`
and, on null, logs and returns false without touching either map, so a bogus name leaves
the type completely unregistered.  Every consumer that calls `get_class_methods<T>()`,
`for_each_instance<T>()`, `make_unique<T>()`, `object_base::resolve_klass()`, or places a
`std::unique_ptr<T>` in a hook callback arg-list silently no-ops, returns nullopt/empty, or
receives a null wrapper when T is unregistered — there is no compile-time guard.  The
factory map is exclusively consumed by `detail::extract_frame_arg` when decoding a
hook callback's `std::unique_ptr<T>` argument from a live interpreter slot; all other
decode paths (`field_proxy::value_t`, `method_proxy::value_t`) bypass the factory and
`new T{oop}` directly.  Registration is a one-time, setup-time operation; the documented
contract requires it to complete before any hook fires.

## Depends on

- [[features/find_class_fallback|find_class_fallback]]
- [[features/klass_introspection|klass_introspection]]
- [[features/decode_oop_and_pointers|decode_oop_and_pointers]]
- [[features/wrapper_pattern|wrapper_pattern]]

## Related

- [[features/classloader_reanchor|classloader_reanchor]]
- [[features/find_class_context_loader|find_class_context_loader]]

## Depended on by

- [[features/for_each_instance|for_each_instance]]
- [[features/method_enumeration|method_enumeration]]

## Implementation anchors

- `vmhook::type_to_class_map / vmhook::registration_mutex / vmhook::g_type_factory_map` — `vmhook/ext/vmhook/vmhook.hpp:1617-1642` — process-global maps: type_to_class_map (type_index->name, line 1617), registration_mutex (line 1618), type_factory_function_t alias + g_type_factory_map (lines 1641-1642)
- `vmhook::register_class<T>` — `vmhook/ext/vmhook/vmhook.hpp:8715-8754` — implementation: find_class guard (8719-8724), lock (8736), insert_or_assign into type_to_class_map (8738), emplace into g_type_factory_map (8746-8751)
- `vmhook::object_base::resolve_klass (static overload)` — `vmhook/ext/vmhook/vmhook.hpp:18132-18149` — primary consumer of type_to_class_map for field/method resolution — type_to_class_map.find -> find_class; returns nullptr + logs when T unregistered
- `vmhook::for_each_instance<T>` — `vmhook/ext/vmhook/vmhook.hpp:8519-8542` — type_to_class_map.find (8526-8527); returns 0 visits with no visitor call when T is unregistered
- `vmhook::get_class_methods<T> (typed overload)` — `vmhook/ext/vmhook/vmhook.hpp:8832-8850` — type_to_class_map.find (8839-8840); returns empty vector when T is unregistered
- `vmhook::detail::extract_frame_arg (unique_ptr path)` — `vmhook/ext/vmhook/vmhook.hpp:9350-9371` — sole consumer of g_type_factory_map — looks up type_to_class_map[typeid(T)] then g_type_factory_map[name] to build the wrapper from a decoded OOP; returns nullptr on any miss

## Tests

- `tests/jvm/modules/register_class.cpp`

## Known bugs

- **[medium]** type_to_class_map uses insert_or_assign (last-wins, line 8738) while g_type_factory_map uses emplace (first-wins, line 8746). Registering a second, different wrapper type to an already-registered class name re-points the type map (new type -> name) but leaves the factory map pointing at the FIRST type's constructor. A hook callback that takes std::unique_ptr<SecondWrapper> then decodes via the old factory (new FirstWrapper{oop}) and extract_frame_arg static_cast<SecondWrapper*>s the result — an invalid downcast, UB on every subsequent field/method access.
- **[low]** Re-registering the SAME type to a DIFFERENT class name via insert_or_assign (line 8738) re-points type_to_class_map but leaves the OLD class name's factory entry alive in g_type_factory_map forever (no erase path exists). The old name permanently resolves to a live factory building T, a small per-rebind permanent leak with no diagnostic.
- **[low]** registration_mutex (line 8736) guards concurrent WRITES in register_class, but all hot-path readers (extract_frame_arg 9358/9363, jni_signature_for_arg, resolve_klass 18135, get_class_methods 8839, for_each_instance 8526) read both maps lock-free. The documented contract requires all registration to complete before hooks fire, but a lazy-registration path triggered from inside a detour races with sibling detours reading the maps. An unordered_map rehash mid-insert + concurrent find is bucket-level UB.
- **[low]** register_class has no null / empty-name fast-reject of its own before calling find_class (line 8719). An empty string_view reaches find_class which does have an empty-name guard (line 7990), so the behaviour is correct today — but the guard is in find_class, not in register_class, making register_class's own contract fragile against future refactors of find_class.

## Notes

Thread-safety contract: the two maps are process-global inline variables (header-level
storage).  The lock in register_class (line 8736) protects concurrent calls to
register_class itself; it does NOT protect the hot-path readers listed above.  All
registration MUST happen from a single thread before any hook fires.  Lazy registration
from inside a detour (mentioned in the comment at lines 8730-8735) is explicitly
contemplated but inherently racy — treat as unsupported unless both maps are additionally
locked on the read side.

The factory's raw-pointer return type (type_factory_function_t, line 1641) is intentional:
returning std::unique_ptr<object_base> would cause libstdc++/libc++ to eagerly instantiate
~unique_ptr<object_base> at lambda-parse time when object_base is still an incomplete type.
Callers receive the raw pointer and immediately wrap it in a unique_ptr where object_base
is complete (extract_frame_arg line 9370).

JDK-version sensitivity: register_class itself is JDK-agnostic — it calls find_class and
does two map writes.  The behaviours it enables are as JDK-sensitive as find_class (which
has a ClassLoaderDataGraph walk + JNI context-loader fallback) and as klass_introspection
(which reads VMStruct offsets).  On every supported JDK 8-26, once the target class is
loaded, find_class resolves it and registration succeeds.

Note: register_class does NOT pin a JNI global reference to the jclass.  find_class returns
a raw HotSpot klass* (not a jobject/jclass); the verified_klass pointer is used only to
confirm the class exists and is then discarded.  The stored value in type_to_class_map is
the class NAME string, not a klass* or jclass handle.
