---
slug: method_overload
title: Method Overload
category: method
status: seeded
risk: high
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/high, category/method, tag/method, tag/overload, tag/resolution, tag/descriptor, tag/argument-matching, tag/wildcard, tag/static]
---

# Method Overload

> **Category:** [[categories/method|Method proxies (resolve / call / dispatch)]]  ·  **Status:** `seeded`  ·  **Risk:** `high`  ·  **Specialist:** `.claude/agents/method_overload-specialist.md`

## Description

`method_proxy` OVERLOAD RESOLUTION — `get_method("name")->call(args)` picking the
overload whose JVM parameter descriptors match the C++ argument TYPES.
`argument_matches_descriptor<T>` maps one C++ type to one JVM descriptor letter
(bool->Z, int8->B, int16->S, uint16->C, int32->I, int64->J, float->F, double->D,
std::string->Ljava/lang/String;, wrapper/oop->L...; with a WILDCARD fallback when
the wrapper isn't `register_class<>`'d); `next_argument_descriptor` tokenises one
param (and arrays); `signature_matches_arguments<...>` checks the whole param
list; `resolve_compatible_method<...>` is the picker, re-run by BOTH dispatch
paths on every call. `get_method("name")` resolves FIRST-by-name;
`get_method("name","sig")` is exact; `static_method(...)` is the static entry.

## Depends on

- [[features/signature_parsing|signature_parsing]]
- [[features/method_explicit_signature|method_explicit_signature]]
- [[features/method_enumeration|method_enumeration]]

## Related

- [[features/find_methods_by_signature|find_methods_by_signature]]
- [[features/method_explicit_signature|method_explicit_signature]]
- [[features/method_is_reference|method_is_reference]]
- [[features/method_overload_java_dispatch|method_overload_java_dispatch]]
- [[features/method_static|method_static]]

## Depended on by

- [[features/interface_polymorphism|interface_polymorphism]]
- [[features/method_call_wide_args|method_call_wide_args]]
- [[features/method_overload_java_dispatch|method_overload_java_dispatch]]
- [[features/unified_call_syntax|unified_call_syntax]]

## Implementation anchors

- `method_proxy::argument_matches_descriptor` — `vmhook/ext/vmhook/vmhook.hpp:17351-17496` — one C++ type -> one JVM descriptor letter; wrapper/oop -> L...; with a WILDCARD fallback for an unregistered wrapper (the ambiguity flaw)
- `method_proxy::signature_matches_arguments` — `vmhook/ext/vmhook/vmhook.hpp:17497-17541` — the whole-param-list matcher driving the picker
- `method_proxy::resolve_compatible_method` — `vmhook/ext/vmhook/vmhook.hpp:17542-17640` — the picker; the DEAD static-overload path: bails `if (!resolved) return this->method` and a static proxy is built with object==nullptr so static overloads never re-pick
- `method_proxy::call / call_jni (resolve + dispatch)` — `vmhook/ext/vmhook/vmhook.hpp:16233-17050` — both paths re-run resolve_compatible_method on every call; call_jni (16233) is the CI path, call (16871) the call-stub path

## Tests

- `tests/jvm/modules/method_overload.cpp`

## Known bugs

- **[high]** Static-overload resolution is DEAD: resolve_compatible_method bails `if (!resolved) return this->method` and every static method_proxy is built with object==nullptr, so STATIC overloads never re-pick. On JDK 21 (call stub absent) the bug is LIVE. The module records every static-overload outcome as [INFO], never fails CI for it, but DOES hard-assert the explicit-signature static path static_method("spick","(D)I") which bypasses resolution, proving the fixture is sound.
- **[medium]** First-match-wins with no ambiguity detection for an UNREGISTERED wrapper arg: argument_matches_descriptor falls back to a WILDCARD L...; match for a wrapper type that was never register_class<>'d, so the first name-matching overload with any reference parameter is taken with no ambiguity signal. Characterized, not failed.

## Notes

Each Java pick(...) overload in the fixture returns a DISTINCT int sentinel so a
mis-resolution is caught as a VALUE mismatch (not a crash, not 'some int came
back'). The module records which dispatch path is live via find_call_stub_entry()
and asserts the converted value_t (path-independent). argument_matches_descriptor
handles the two-slot J/D types in the param walk; call((int64_t)x) on a name-only
proxy with both int and long overloads picks long; signature_pinned (set by
get_method(name,sig)) forces the exact overload verbatim. Java-8-only fixture,
ASCII strings (so read_java_string vs GetStringUTFChars never diverges). call()
runs inside the detour where current_java_thread is live.
