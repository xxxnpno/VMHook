---
slug: method_proxy_value_t
title: Method Proxy Value T
category: method
status: seeded
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/medium, category/method, tag/method, tag/value-t, tag/variant, tag/conversion, tag/no-jvm, tag/as-string, tag/compressed-oop]
---

# Method Proxy Value T

> **Category:** [[categories/method|Method proxies (resolve / call / dispatch)]]  ·  **Status:** `seeded`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/method_proxy_value_t-specialist.md`

## Description

`method_proxy::value_t` — the variant return-value type produced by
`method_proxy::call()`/`call_jni()`/`call_stub()`, its templated implicit
conversion operator (every C++ target type), and the `is_void()`/`is_string()`/
`as_string()` introspection helpers, plus the adjacent `method_proxy` accessor
surface (`name`, `signature`, `raw_method`, `is_static`, `is_reference`) that a
no-JVM unit test can drive on a null-`Method*` proxy. The variant has exactly 11
alternatives: `monostate, bool, i8, i16, i32, i64, float, double, uint16, uint32,
std::string` — `uint32` is the reference/array (compressed OOP) alternative,
`std::string` the eagerly-decoded java.lang.String, `monostate` void/null/
failure. The conversion operator's four `if constexpr` arms route by target:
unique_ptr<wrapper> (decode uint32 OOP, wrap), std::string (eager / read_java_string),
void* (full decode_oop_pointer, never a truncating cast), else static_cast.
Crash-free with NO JVM because decode_oop_pointer/read_java_string null-guard up front.

## Depends on

- [[features/wrapper_pattern|wrapper_pattern]]
- [[features/decode_oop_and_pointers|decode_oop_and_pointers]]
- [[features/read_java_string|read_java_string]]

## Related

- [[features/method_call_object|method_call_object]]
- [[features/method_call_string|method_call_string]]
- [[features/method_call_primitives|method_call_primitives]]
- [[features/method_return_types|method_return_types]]
- [[features/method_is_reference|method_is_reference]]
- [[features/field_proxy_value_t|field_proxy_value_t]]

## Depended on by

- [[features/method_call_object|method_call_object]]
- [[features/method_call_primitives|method_call_primitives]]
- [[features/method_call_return_void|method_call_return_void]]
- [[features/method_return_types|method_return_types]]
- [[features/method_static|method_static]]

## Implementation anchors

- `method_proxy::value_t (struct + variant)` — `vmhook/ext/vmhook/vmhook.hpp:16033-16207` — single-member aggregate over the 11-alternative variant; uint32 = ref/array OOP, std::string = eager java.lang.String
- `value_t::operator target_type / is_void / is_string / as_string` — `vmhook/ext/vmhook/vmhook.hpp:16100-16207` — unique_ptr arm (decode+validate+wrap, static_assert : object_base), std::string arm, void* full-decode arm, static_cast fallback; as_string dodges the MSVC const char* ambiguity
- `method_proxy constructor / signature / raw_method` — `vmhook/ext/vmhook/vmhook.hpp:16208-16232` — ctor(owning_object, method_ptr, sig, pinned=false); signature() at 17189 returns signature_text; raw_method() at 17286 returns the method member
- `decode_oop_pointer / read_java_string (the no-JVM-safe seams)` — `vmhook/ext/vmhook/vmhook.hpp:1367-1368` — decode_oop_pointer returns nullptr for compressed==0 and when base/shift VMStructs can't resolve; read_java_string null/invalid-oop-safe at the top — so uint32->void*/string/unique_ptr is crash-free with no JVM

## Tests

- `tests/test_method_proxy_value_t.cpp`

## Notes

value_t is an aggregate with a single variant member, so value_t{X}
aggregate-initialises the variant from X. The conversion-operator arm order is:
is_unique_ptr_v (only meaningful when stored is uint32), == std::string, ==
void* AND stored uint32, then the static_cast fallback. The producers decide
which alternative is stored: call_jni stores eager std::string for a String
return, encode_oop_pointer (uint32) for an object/array, per-type returns for
primitives, monostate for void/failure; the call-stub per-descriptor switch is
the authoritative descriptor-char -> variant-slot map. This is the no-JVM-testable
slice of method_proxy (it shares the test file with the proxy accessors); the
uint32->void*/string/unique_ptr paths are crash-free with no JVM because both
decode seams null-guard up front.
