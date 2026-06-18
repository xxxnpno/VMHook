---
slug: method_enumeration
title: Method Enumeration
category: method
status: seeded
risk: high
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/high, category/method, tag/method, tag/enumeration, tag/descriptor, tag/hook-by-signature, tag/obfuscation, tag/declared-only, tag/no-jvm]
---

# Method Enumeration

> **Category:** [[categories/method|Method proxies (resolve / call / dispatch)]]  ·  **Status:** `seeded`  ·  **Risk:** `high`  ·  **Specialist:** `.claude/agents/method_enumeration-specialist.md`

## Description

Reading a class's `InstanceKlass::_methods` array directly (no JNI/JVMTI) into
`(name, JVM-descriptor)` pairs, selecting methods by descriptor when the name
rotates per obfuscated build, and installing a hook on the UNIQUE descriptor
match while REFUSING an ambiguous one. The four public entry points are
`get_class_methods<T>()`, `get_class_methods("internal/Name")`,
`find_methods_by_signature<T>(desc)` (exact string-equality, returns ALL
matches so the caller can detect non-uniqueness), and
`hook_by_signature<T>(desc, detour)` (returns false on no-match OR on > 1 match,
else delegates to `hook<T>`). All sit on `detail::collect_klass_methods` (the
shared, noexcept, try/catch-wrapped engine that walks the array skipping
is_valid_pointer-failing slots). Enumeration is DECLARED-only; works on classes
never `register_class<T>()`'d (the discovery path for obfuscated builds).

## Depends on

- [[features/instanceklass_methods_walk|instanceklass_methods_walk]]
- [[features/klass_introspection|klass_introspection]]
- [[features/signature_parsing|signature_parsing]]

## Related

- [[features/find_methods_by_signature|find_methods_by_signature]]
- [[features/klass_introspection|klass_introspection]]
- [[features/hook_signature|hook_signature]]
- [[features/method_explicit_signature|method_explicit_signature]]

## Depended on by

- [[features/deoptimize_methods|deoptimize_methods]]
- [[features/find_methods_by_signature|find_methods_by_signature]]
- [[features/method_call_primitives|method_call_primitives]]
- [[features/method_explicit_signature|method_explicit_signature]]
- [[features/method_overload|method_overload]]

## Implementation anchors

- `detail::collect_klass_methods` — `vmhook/ext/vmhook/vmhook.hpp:8804-8835` — shared engine: noexcept, snapshots count+ptr once, skips is_valid_pointer-failing slots, emplaces get_name()/get_signature(), outer try/catch
- `vmhook::get_class_methods / find_methods_by_signature` — `vmhook/ext/vmhook/vmhook.hpp:8853-8930` — by-name (8853), by-wrapper (8873), find_methods_by_signature exact-equality filter (8913) returning ALL matches
- `vmhook::hook_by_signature` — `vmhook/ext/vmhook/vmhook.hpp:11214-11260` — resolves names via find_methods_by_signature; false if names.empty() (no-match) OR names.size() > 1 (ambiguous), else hook<T>(names.front(), descriptor, detour)
- `klass::get_methods_count / get_methods_ptr (the substrate)` — `vmhook/ext/vmhook/vmhook.hpp:3498-3580` — Array<Method*>::_length at +0 (3498), data at hardcoded x64 +8 (3537) — the real ABI risk under this feature

## Tests

- `tests/jvm/modules/method_enumeration.cpp`
- `tests/test_method_enumeration.cpp`
- `tests/jvm/modules/find_methods_by_signature.cpp`

## Known bugs

- **[medium]** The four public functions are thin and well-guarded; the defects live in the PRIMITIVES they call: get_methods_ptr hardcodes the x64 Array<Method*> +8 data offset and get_methods_count reads _length at +0 as a raw int32 with no ceiling (see instanceklass_methods_walk), and symbol::to_string yields "" for length > 0x1000 so a pathological method could enumerate as ("","").
- **[low]** find_methods_by_signature does pure std::string == std::string_view exact-equality with NO normalization/validation of the descriptor, so a whitespace-padded / lowercased / dotted-form descriptor silently matches nothing (the module pins ~30 such negative inputs as empty, no crash).

## Notes

log_class_methods<T>() is compiled out in release (VMHOOK_LOG no-op), so the
test must use the data-returning overloads. find_class is the resolver behind all
overloads (name-cache with a stale-pointer guard that re-reads the cached klass's
own name and evicts on mismatch; ClassLoaderDataGraph walk -> context-loader JNI
fallback; nullptr for an unknown name = the bogus/empty-name -> empty path).
_methods is stable across JDK 8-26, so the method leg is far less version-sensitive
than the field leg; the only variance is the synthetic-method set javac emits (JDK
8 extra synthetics — the fixtures see 18 vs 16 members, ()V multiplicity 6 vs 5),
so tests assert SET membership + LOWER bounds, never an exact total. The no-JVM
contract (every entry point empty without throwing) is pinned by
tests/test_method_enumeration.cpp; the live SET + descriptor-selector + ambiguity
refusal coverage is in the two JVM modules.
