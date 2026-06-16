---
slug: find_methods_by_signature
title: Find Methods By Signature
category: method
status: in_progress
risk: low
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/in_progress, risk/low, category/method, tag/method-selector, tag/descriptor, tag/enum, tag/declared-only]
---

# Find Methods By Signature

> **Category:** [[categories/method|Method proxies (resolve / call / dispatch)]]  ·  **Status:** `in_progress`  ·  **Risk:** `low`  ·  **Specialist:** `.claude/agents/find_methods_by_signature-specialist.md`

## Description

Template-level feature `find_methods_by_signature<W>(descriptor)` accepts a JVM
descriptor string and returns the complete vector of names of every declared method
on W's class whose descriptor matches exactly (byte-for-byte, no normalization).
Returns empty if W is unregistered or no method matches. Core use case: callers select
methods by stable JVM descriptor in obfuscated builds (method names rotate, descriptors
don't), and detect non-unique descriptors as a vector with all matching names rather
than silent first-match.

## Depends on

- [[features/signature_parsing|signature_parsing]]
- [[features/method_enumeration|method_enumeration]]
- [[features/klass_introspection|klass_introspection]]

## Related

- [[features/method_explicit_signature|method_explicit_signature]]
- [[features/method_call_primitives|method_call_primitives]]
- [[features/method_is_reference|method_is_reference]]
- [[features/method_overload|method_overload]]

## Implementation anchors

- `vmhook::find_methods_by_signature<W>(descriptor)` — `vmhook/ext/vmhook/vmhook.hpp:7081-7094` — main entry point; calls get_class_methods, filters by exact descriptor match, returns vector<string> of names
- `vmhook::get_class_methods<W>()` — `vmhook/ext/vmhook/vmhook.hpp:7030-7048` — substrate resolver; looks up type in type_to_class_map, delegates to collect_klass_methods
- `detail::collect_klass_methods(klass*)` — `vmhook/ext/vmhook/vmhook.hpp:6973-7004` — walks InstanceKlass::_methods directly (no JNI); emits (name, signature) pairs for each valid Method* slot
- `method::get_name / method::get_signature` — `vmhook/ext/vmhook/vmhook.hpp:2292-2362` — Symbol decode guarded by is_valid_pointer; called per-method in collect_klass_methods
- `InstanceKlass::get_methods_count / get_methods_ptr` — `vmhook/ext/vmhook/vmhook.hpp:2651-2701` — VMStruct accessors for _methods array; +8 skip past Array<Method*> header (int _length, int _pad)

## Tests

- `tests/jvm/modules/find_methods_by_signature.cpp`

## Known bugs

- **[low]** No descriptor validation -> malformed/typo'd descriptor indistinguishable from legitimate empty result. vmhook.hpp:7088 raw exact string compare with zero normalization; dotted form (Ljava.lang.String;)Ljava.lang.String; instead of slashed, whitespace-padded, lowercase, or near-miss (I)F returns empty silently with no log/diagnostic. Pinned by ~30-assertion malformed-descriptor battery (dotted, padded, lowercase, missing/unbalanced parens, garbage, NAME-as-descriptor, truncated L...;, foreign-class descriptor). Suggested fix: VMHOOK_LOG hint when descriptor non-empty but lacks ().
- **[low]** Doc/behaviour drift on inheritance: vmhook.hpp:7070 doc-comment says 'every method on T's class' which can read as resolved/inherited table; implementation walks only declared _methods (collect_klass_methods vmhook.hpp:6979-6998), excluding inherited java.lang.Object methods (equals, getClass). Correct for hook selector (you hook declared methods) but wording invites wrong expectation. Pinned by inherited_equals_descriptor_absent / inherited_getClass_descriptor_absent assertions. Suggested fix: doc 'every method DECLARED by T's class (incl. <init>/<clinit>, excl. inherited)'.
- **[low]** Silent under-report on bad Method* slot: collect_klass_methods continues past slots failing is_valid_pointer (vmhook.hpp:6993-6996) with no diagnostic. Under class-redefinition race or corrupted _methods a real method silently vanishes, so descriptor that should be non-unique could appear unique. Hard to trigger without VM memory corruption; defensive-by-design. Suggested fix: VMHOOK_LOG on the skip.
- **[low]** No caching: O(n) re-walk + per-name heap alloc on every call (vmhook.hpp:7086). find_methods_by_signature calls get_class_methods<W>() afresh, re-walking _methods and rebuilding vector<pair<string,string>> before filtering. Loop probing many descriptors against one class pays full walk+allocations each time. Not a correctness bug; get_class_methods cache keyed by klass would help hot selectors.

## Notes

JDK-version sensitivities: JDK 8 emits extra synthetic `()V` accessor (javac nested-class
access method); descriptor count for `()V` is >= 5 on JDK 8 and exactly 5 on JDK 11+ (verified
with javap --release 8/11/17/21). All other distinctive descriptors are JDK-stable.
Descriptors are internal form (slashed, no normalization). Feature does NOT require live
current_java_thread (pure VMStruct read). Test fixture (example/vmhook/fixtures/FindMethodsBySig.java)
verified with javap -s on all four JDK versions; 121 assertions cover shared-descriptor
sets, unique descriptors, return-type discrimination, arity, arrays, void set membership,
substrate consistency, malformed angles, unregistered types, and live post-dispatch stability.
