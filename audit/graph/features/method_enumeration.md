---
slug: method_enumeration
title: Method Enumeration
category: method
status: in_progress
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/in_progress, risk/medium, category/method, tag/method, tag/enumeration, tag/klass, tag/vmstruct, tag/declared-only, tag/instanceklass, tag/hotspot]
---

# Method Enumeration

> **Category:** [[categories/method|Method proxies (resolve / call / dispatch)]]  ·  **Status:** `in_progress`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/method_enumeration-specialist.md`

## Description

Enumerates the declared methods of a Java class by reading
`InstanceKlass::_methods` directly — no JNI, no JVMTI.  Input is either a
registered wrapper type `W` (via `get_class_methods<W>()`) or a raw internal
class name such as `"net/minecraft/Foo"` (via `get_class_methods(name)`); both
delegate to `detail::collect_klass_methods(klass*)`.  Output is a
`std::vector<std::pair<std::string,std::string>>` where each pair is
`(method-name, JVM-descriptor)`, one entry per slot in the `Array<Method*>`
that passes the `is_valid_pointer` guard.  Synthetic and compiler-generated
methods (`<init>`, `<clinit>`, bridge methods) are included because they live in
`_methods`; inherited methods from superclasses are absent because HotSpot stores
only declared methods there.  The function is `noexcept` and returns empty on a
null or unregistered class; a corrupt slot is silently skipped rather than
reported.  Three callers depend on this walk: `find_methods_by_signature<W>(desc)`
filters the result to locate a method by descriptor when its name rotates across
obfuscated builds; `hook_by_signature<W>(desc, detour)` uses that result to
enforce a unique-descriptor policy before installing a hook; and
`method_proxy` resolution re-walks the same vector to bind named fields.

## Depends on

- [[features/instanceklass_methods_walk|instanceklass_methods_walk]]
- [[features/vmstructs_offset_resolution|vmstructs_offset_resolution]]
- [[features/register_class|register_class]]

## Related

- [[features/find_methods_by_signature|find_methods_by_signature]]
- [[features/find_class_fallback|find_class_fallback]]

## Depended on by

- [[features/deoptimize_methods|deoptimize_methods]]
- [[features/find_methods_by_signature|find_methods_by_signature]]
- [[features/method_call_primitives|method_call_primitives]]
- [[features/method_explicit_signature|method_explicit_signature]]
- [[features/signature_parsing|signature_parsing]]

## Implementation anchors

- `vmhook::detail::collect_klass_methods` — `vmhook/ext/vmhook/vmhook.hpp:8775-8806` — Core engine: null-klass guard, snapshots count and base pointer, reserves, walks with per-slot is_valid_pointer check, emplace (name, signature) pairs, outer try/catch so a corrupt slot yields a short vector not a throw.

- `vmhook::get_class_methods` — `vmhook/ext/vmhook/vmhook.hpp:8821-8825` — By-internal-name overload: delegates directly to collect_klass_methods(find_class(name)). Useful for classes never wrapped with register_class<T>(); the discovery path for obfuscated builds.

- `vmhook::get_class_methods<W>` — `vmhook/ext/vmhook/vmhook.hpp:8832-8850` — Template overload: looks up type_to_class_map[typeid(W)] and returns {} if W is unregistered; otherwise delegates to collect_klass_methods(find_class(name)).

- `vmhook::hotspot::klass::get_methods_count` — `vmhook/ext/vmhook/vmhook.hpp:3469-3498` — Reads InstanceKlass._methods via VMStruct offset, then *(int32_t*)array (_length at offset 0).  Clamped to [0, 65535] (class-file u2 ceiling) before returning; negative or giant values return 0.

- `vmhook::hotspot::klass::get_methods_ptr` — `vmhook/ext/vmhook/vmhook.hpp:3508-3530` — Reads _methods independently (second VMStruct deref) and returns array+8 — the hard-coded x64 Array<Method*> data offset after [_length:4][_pad:4].


## Tests

- `tests/jvm/modules/method_enumeration.cpp`
- `tests/test_method_enumeration.cpp`

## Known bugs

- **[low]** Corrupt slot skipped silently with no log (vmhook.hpp:8795-8797).  When is_valid_pointer(method_ptr) returns false inside the loop the slot is `continue`d without any diagnostic, so a caller seeing a short result vector has no way to distinguish "class has N methods" from "M slots were corrupt". Adds a log line or counter so callers can detect partial enumeration.

- **[low]** get_methods_count and get_methods_ptr re-read _methods independently (vmhook.hpp:3472 vs 3511, surfaced in collect_klass_methods 8785-8786). A concurrent JVMTI RedefineClasses swap between the two reads yields a count from the old Array<Method*> but a base pointer from the new one (or vice versa), walking past the end.  The per-slot is_valid_pointer guard is the only protection.  Fix: read _methods once and derive both length and base from the single snapshot.

- **[low]** Array<Method*> data offset +8 is hard-coded (vmhook.hpp:3529).  The [_length:4][_pad:4][_data] layout is assumed for x64 HotSpot and is not derived from VMStructs.  A JDK that changes Array<T> header padding or a 32-bit build would shift every enumerated (name, descriptor) pair by one slot or produce garbage.  The no_empty_name_or_descriptor and wellformed descriptor checks in the test suite surface the symptom but not the cause.

- **[low]** No caching: collect_klass_methods re-walks the full _methods array on every call (vmhook.hpp:8775-8806).  find_methods_by_signature<W> calls get_class_methods<W>() on every invocation; hook_by_signature does the same (vmhook.hpp:11158).  For an obfuscated class with hundreds of methods, repeated descriptor searches are O(n) re-walks.  A caller that scans multiple descriptors on the same class should cache the result of get_class_methods<W>() rather than calling find_methods_by_signature<W> in a tight loop.


## Notes

`()V` multiplicity is a lower bound, not an exact count: JDK 8 javac emits
extra synthetic bridge methods for this fixture (18 total vs 16 on JDK 9+),
some void-returning, so the test suite asserts >= 6 for ()V rather than an
exact value.  The per-method count_pair == 1 checks for application methods
are exact across JDKs.

`<clinit>` is present only when the class has a static initialiser or static
field initialisers.  The MethodEnumeration fixture has both, so <clinit> is
guaranteed across all tested JDKs.

ConstantPool::_length is not exported on every JDK (vmhook.hpp:2051-2055 /
2109-2113 guard `cp_length < 0`).  When absent, the name/descriptor index
bounds check is skipped and the per-slot is_readable_pointer guard is the
sole protection against an out-of-range _name_index or _signature_index.

Symbol decode is raw byte-copy (vmhook.hpp:1909); only ASCII method names and
descriptors are decoded correctly.  A class with a Unicode method name or a
descriptor referencing a Unicode class returns modified-UTF-8 raw bytes, so a
caller's `candidate == descriptor` compare in find_methods_by_signature only
matches if the caller also passes modified-UTF-8.  This does not affect any
real or fixture class covered by the test suite.

The Array<Method*> +8 hard-coded offset is stable across all 64-bit HotSpot
8-26 builds; a JDK that changes this layout (or any 32-bit target) would break
enumeration silently.  See known_bugs entry above.
