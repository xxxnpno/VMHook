---
slug: method_overload_java_dispatch
title: Method Overload Java Dispatch
category: method
status: seeded
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/medium, category/method, tag/method, tag/overload, tag/dispatch, tag/java-readback, tag/resolution, tag/side-effect, tag/cross-check]
---

# Method Overload Java Dispatch

> **Category:** [[categories/method|Method proxies (resolve / call / dispatch)]]  ·  **Status:** `seeded`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/method_overload_java_dispatch-specialist.md`

## Description

The Java-side READBACK authority for overload dispatch. The companion
`method_overload` proves WHICH overload the resolver SELECTS; this feature proves
the selected overload's REAL effect — its actual computed return value AND a
per-overload side effect Java records — flows back correctly through
`method_proxy::call()`. It drives each overload from native code two ways
(C++-typed `call()` and explicit-signature `call()`), reads each result back, and
cross-checks against Java's own recorded state (per-overload hit counters +
arg/result echoes) so "the intended body ran and no sibling did" is proven from
the JVM's observable side, never from the proxy's name. A name-only proxy's
`signature()` stays the first-by-name descriptor regardless of the typed dispatch
target (call() writes the re-picked descriptor only into a local
`selected_signature` / the mutable `cached_effective_signature`, never back into
`signature_text`). Re-hosts legacy example.cpp test_overloaded_methods.

## Depends on

- [[features/method_overload|method_overload]]
- [[features/method_call_primitives|method_call_primitives]]
- [[features/method_call_string|method_call_string]]

## Related

- [[features/method_overload|method_overload]]
- [[features/method_explicit_signature|method_explicit_signature]]
- [[features/find_methods_by_signature|find_methods_by_signature]]
- [[features/method_return_types|method_return_types]]

## Implementation anchors

- `method_proxy::resolve_compatible_method` — `vmhook/ext/vmhook/vmhook.hpp:17542-17640` — decision order: signature_pinned short-circuit -> signature_text already matches args -> super-chain walk for first name+descriptor match -> fallback `return this->method` on no match (NOT monostate)
- `method_proxy::call (uses selected_method->get_signature())` — `vmhook/ext/vmhook/vmhook.hpp:16871-17050` — calls resolve_compatible_method then uses the SELECTED method's signature as selected_signature for the actual interpreter call; writes it only locally, never to signature_text
- `object::get_method(name) / get_method(name, signature)` — `vmhook/ext/vmhook/vmhook.hpp:17937-18070` — name-only latches the FIRST-by-name descriptor (signature_pinned=false); name+sig pins exact (signature_pinned=true) so it is NEVER re-picked from C++ arg types
- `method_proxy::signature` — `vmhook/ext/vmhook/vmhook.hpp:17189-17208` — returns signature_text unchanged — why a name-only proxy's signature() stays the first-by-name descriptor regardless of the typed dispatch target

## Tests

- `tests/jvm/modules/method_overload_java_dispatch.cpp`

## Notes

resolve_compatible_method's no-match behaviour is `return this->method` (NOT
monostate). argument_matches_descriptor mappings relevant here: std::string /
const char* -> Ljava/lang/String;; 4-byte integral -> I; 8-byte integral -> J;
double -> D ONLY; everything unrecognized (incl. wrapper/unique_ptr in the value
path) -> false. The generic numeric pack branch does static_assert(sizeof<=8) then
memcpy raw bytes into one intptr_t slot. The cross-check proves the body ran from
the JVM's side (per-overload hit counters + arg/result echoes), so a
mis-resolution surfaces as a VALUE or counter mismatch. Java-8-only fixture;
call() runs inside the detour. Re-hosts f(int 30)->130, f("foo")->"[foo]",
f(2,3)->5.
