---
slug: method_is_reference
title: Method Is Reference
category: method
status: seeded
risk: low
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/low, category/method, tag/method, tag/is-reference, tag/introspection, tag/descriptor, tag/metadata, tag/call-free]
---

# Method Is Reference

> **Category:** [[categories/method|Method proxies (resolve / call / dispatch)]]  ·  **Status:** `seeded`  ·  **Risk:** `low`  ·  **Specialist:** `.claude/agents/method_is_reference-specialist.md`

## Description

`method_proxy::is_reference()` — the O(1), call-free introspection accessor that
reports whether a resolved Java method's RETURN type is a Java reference
(object / array: the descriptor char after `)` is `L` or `[`) versus a primitive
(`Z B S C I J F D`) or void (`V`). It reads ONLY the cached `signature_text`
string (`close = find(')')`; bail false if npos or `close+1 >= size()`; else
`ret == 'L' || ret == '['`), so it needs no live bytecode dispatch, no
`current_java_thread`, and never touches the `Method*` — every assertion can be
made straight off resolved (or hand-built null-`Method*`) proxies without a
single `call()`. UB-free on "", "(", "()" and a null-`Method*` proxy.

## Depends on

- [[features/signature_parsing|signature_parsing]]

## Related

- [[features/find_methods_by_signature|find_methods_by_signature]]
- [[features/method_explicit_signature|method_explicit_signature]]
- [[features/method_overload|method_overload]]
- [[features/method_proxy_value_t|method_proxy_value_t]]

## Implementation anchors

- `method_proxy::is_reference` — `vmhook/ext/vmhook/vmhook.hpp:17239-17285` — the entire feature: find(')'), npos/close+1>=size guards, ret == 'L' || '['; noexcept, pure-metadata, no Method* deref
- `method_proxy::signature` — `vmhook/ext/vmhook/vmhook.hpp:17189-17208` — returns a string_view over the member signature_text (the descriptor is_reference parses)
- `method_proxy constructor / raw_method` — `vmhook/ext/vmhook/vmhook.hpp:16208-16232` — ctor(owning_object, method_ptr, sig, pinned=false) stores signature_text via std::move, so is_reference works the instant the proxy exists; raw_method at 17286 (is_reference is independent of it)
- `method_proxy::is_static` — `vmhook/ext/vmhook/vmhook.hpp:17209-17238` — reads _access_flags & 0x0008 — proves static-ness is orthogonal to reference-ness

## Tests

- `tests/jvm/modules/method_is_reference.cpp`

## Known bugs

- **[low]** Two divergent return-char parsers in the same class: is_reference() uses find(')') (the FIRST ')'), the hot call_jni path's cached_ret_char uses rfind(')') (the LAST ')') and treats 'nothing after )' as 'V'. For every legal descriptor there is exactly one ')' so they always agree, but a malformed descriptor with two ')' (e.g. "()L)V") would be classified off different parens — so is_reference() is not guaranteed to predict the kind the dispatch path decodes for a hand-built illegal proxy.
- **[low]** is_reference() ignores the cached_ret_char cache and re-scans signature_text on every call (cheap, but redundant with the lazily-populated cache the dispatch path uses).

## Notes

No correctness bug in is_reference() itself — for every well-formed JVM method
descriptor it returns the right answer, and the malformed-input guards (npos,
close+1 >= size) are exactly right, so it is UB-free on the degenerate inputs and
on a null-Method* proxy (it never reads the Method* at all). is_reference is the
no-JVM-driveable slice: hand-built proxies (method_proxy{nullptr, nullptr,
std::string{"..."}}) bind pinned=false and exercise every branch without a JVM.
Resolution entry points (instance get_method, free static_method) all return
std::optional<method_proxy>; is_reference is read off the contained proxy
regardless of which path produced it.
