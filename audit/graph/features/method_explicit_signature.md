---
slug: method_explicit_signature
title: Method Explicit Signature
category: method
status: seeded
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/medium, category/method, tag/method, tag/explicit-signature, tag/overload, tag/descriptor, tag/exact-match, tag/pinned, tag/superclass-walk]
---

# Method Explicit Signature

> **Category:** [[categories/method|Method proxies (resolve / call / dispatch)]]  ·  **Status:** `seeded`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/method_explicit_signature-specialist.md`

## Description

`object::get_method(name, signature)` selecting a single overload by EXACT JVM
descriptor, and the guarantee that a wrong / absent / empty signature yields NO
method (so the resulting call is a safe no-op). Four signature-aware entry
points plus portable aliases: the instance overload and the static overload
(built with a null owning object), the deducing-this forwarder
(`VMHOOK_HAS_DEDUCING_THIS` only), and the always-available portable
`static_method(name, sig)`. Each walks the klass superclass chain and matches
with the exact compare `name == method_name && signature == method_signature`
(zero looseness: no empty-string short-circuit, no prefix match, no case fold),
pinning the proxy with `signature_pinned=true`. At call time
`resolve_compatible_method` honours the pinned overload verbatim (it only
re-resolves when the proxy's own descriptor does NOT match the supplied C++
args), so a no-arg / uniquely-typed-arg explicit selection survives end-to-end.

## Depends on

- [[features/signature_parsing|signature_parsing]]
- [[features/method_enumeration|method_enumeration]]

## Related

- [[features/find_methods_by_signature|find_methods_by_signature]]
- [[features/method_call_primitives|method_call_primitives]]
- [[features/method_is_reference|method_is_reference]]
- [[features/method_overload|method_overload]]
- [[features/method_static|method_static]]

## Depended on by

- [[features/hook_signature|hook_signature]]
- [[features/method_overload|method_overload]]
- [[features/method_throwing_call_site|method_throwing_call_site]]

## Implementation anchors

- `object::get_method(name, signature) (instance, exact compare)` — `vmhook/ext/vmhook/vmhook.hpp:18002-18070` — walks superclass chain; exact name+signature compare; builds method_proxy{instance, method, sig, pinned=true} (FLAW: always passes this->instance even for a static match)
- `object_base::get_method(type_index, name, signature) (static, exact)` — `vmhook/ext/vmhook/vmhook.hpp:18135-18200` — same superclass walk; builds method_proxy{nullptr, method, sig, pinned=true} — null owning object, the correct choice for a static accessor
- `object::static_method(name, signature) (portable alias)` — `vmhook/ext/vmhook/vmhook.hpp:18806-18815` — always-available portable entry GCC users must use from a static wrapper method
- `method_proxy::resolve_compatible_method / signature_matches_arguments` — `vmhook/ext/vmhook/vmhook.hpp:17497-17600` — signature_pinned short-circuit + 'proxy's own descriptor already matches args' check that preserves the explicit selection unless the C++ args don't match

## Tests

- `tests/jvm/modules/method_explicit_signature.cpp`

## Known bugs

- **[low]** Instance overload sets a phantom `this` for STATIC methods: it always passes this->instance as the owning object even when the matched Method* is JVM_ACC_STATIC (the static overload correctly passes nullptr). Calling obj.get_method("smap","(I)I") on an instance handle to a static method yields a proxy whose object != nullptr, so call() pushes a bogus receiver into slot 0 and shifts the real arg one slot too high. Fix: peek get_access_flags() for JVM_ACC_STATIC and pass nullptr when static. The module sidesteps it by using static_method(name,sig) for the static family.
- **[low]** Empty signature is a silent strict-miss with a misleading log: get_method(name, "") can never match (every real descriptor starts with '(') so it returns nullopt and logs the generic 'no method with this exact name+signature found', diverging silently from how hook<T>(name, signature) treats an empty signature.

## Notes

The exact-match comparison has zero looseness, so defineClass(String,[B,I,I,
ProtectionDomain) cannot be confused with any sibling overload; the match logic
is solid and the only way to 'lose' an explicit selection is to call with C++
args that don't match the descriptor you asked for (user error, not a lookup
flaw). resolve_compatible_method FIRST checks signature_matches_arguments and
returns this->method unchanged when the proxy's own signature already matches —
so even combo(CharSequence) vs combo(String) is honored because a single L...;
parameter matches std::string. Java fixture; the module pins the static family
via static_method to avoid the phantom-this flaw.
