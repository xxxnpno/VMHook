---
slug: jni_arg_packing
title: Jni Arg Packing
category: jni
status: seeded
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/medium, category/jni, tag/jni, tag/arg-packing, tag/jvalue, tag/union, tag/local-ref, tag/descriptor, tag/little-endian]
---

# Jni Arg Packing

> **Category:** [[categories/jni|JNI plumbing (arg packing / refs / java values)]]  ·  **Status:** `seeded`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/jni_arg_packing-specialist.md`

## Description

Converts a C++ variadic argument pack into the `jvalue` (`detail::jni_value`)
array HotSpot's `CallXMethodA`/`NewObjectA` expect — choosing the correct union
member per arg type, allocating + tagging JNI local refs (`jstring`s) for
release, and materialising object args as synthetic stack handles
(`value.l = &storage`). Two parallel packers exist — the heap path
(`append_jni_arg`/`make_jni_args`, `push_back` into vectors) and the stack path
(`write_jni_arg_to_slot`, a fixed slot, no heap) — plus a sibling descriptor
builder `jni_signature_for_arg` that MUST stay consistent with them. The union
cell is full-width cleared (`value.j = 0`) before each narrow write to kill
stale high bits; `needs_release` is tagged so only real jstrings reach
`DeleteLocalRef` (never an aliased primitive cell). x86/x64 little-endian only.

## Depends on

- [[features/signature_parsing|signature_parsing]]

## Related

- [[features/jni_local_ref_hygiene|jni_local_ref_hygiene]]
- [[features/method_call_wide_args|method_call_wide_args]]
- [[features/make_unique|make_unique]]
- [[features/make_java_string|make_java_string]]
- [[features/read_java_string|read_java_string]]

## Depended on by

- [[features/jni_local_ref_hygiene|jni_local_ref_hygiene]]
- [[features/make_unique|make_unique]]
- [[features/method_call_jni_fallback|method_call_jni_fallback]]
- [[features/method_call_primitives|method_call_primitives]]
- [[features/method_call_wide_args|method_call_wide_args]]

## Implementation anchors

- `detail::jni_value (tagged union)` — `vmhook/ext/vmhook/vmhook.hpp:11398-11409` — z/b/c/s/i/j/f/d/l members all alias the same pointer-sized cell — root of the union-aliasing hazards
- `detail::append_jni_arg (heap path)` — `vmhook/ext/vmhook/vmhook.hpp:13066-13128` — one arg -> push_back; full-width clear (value.j=0) then compile-time per-type dispatch
- `detail::make_jni_args` — `vmhook/ext/vmhook/vmhook.hpp:13129-13140` — fold over append_jni_arg; pre-reserves so &object_handles.back() in .l is not invalidated by realloc
- `detail::write_jni_arg_to_slot (stack path)` — `vmhook/ext/vmhook/vmhook.hpp:13165-13200` — same dispatch ladder into a single caller-owned storage cell; needs_release is a bool&
- `detail::jni_signature_for_arg (descriptor builder)` — `vmhook/ext/vmhook/vmhook.hpp:12772-12880` — must stay consistent with the packers; hard static_assert on unsupported types
- `detail::jni_new_string_utf` — `vmhook/ext/vmhook/vmhook.hpp:11775-11800` — the local-ref source: copies into std::string and passes c_str() to NewStringUTF (NUL-truncation origin)

## Tests

- `tests/test_jni_arg_packing.cpp`
- `tests/jvm/modules/method_call_wide_args.cpp`
- `tests/jvm/modules/jni_local_ref_hygiene.cpp`

## Known bugs

- **[medium]** Union write-member disagrees with the declared JNI descriptor for sub-int widths (int8/int16/uint16): the packers route every integral && sizeof<=4 arg through value.i (32-bit), but jni_signature_for_arg types them B/S/C so the JVM reads the slot as jbyte/jshort/jchar. Only correct because the union aliases AND the host is little-endian (the .b/.s/.c overlap the low bytes the value.j=0 clear zeroed above). A big-endian JVM would read the high bytes and get 0. x86/x64-only today; an undocumented ABI assumption the unit test even pins as intended.
- **[medium]** jni_signature_for_arg rejects 8-byte integrals the packer silently accepts (compile-time asymmetry): the packer's last integral arm is generic sizeof==8 -> .j, but jni_signature_for_arg has only explicit int64_t/uint64_t branches + a generic sizeof==4 -> I, else hard static_assert. An 8-byte integral not spelled int64_t/uint64_t (long vs long long; size_t/ptrdiff_t) packs fine in isolation but fails to compile at jni_make_unique / method_proxy::call_jni. Fails closed (build error), but the two surfaces accept different type sets.
- **[medium]** char/wchar_t/char16_t/char32_t pack but break signature derivation: jni_signature_for_arg's narrow-integral branches test the EXACT fixed-width types, so plain char, MSVC wchar_t/char16_t hit the static_assert; char32_t packs to .i and gets I (Java int, never jchar). The supported integral arg types are narrower than the packer advertises, undocumented.
- **[medium]** std::string/string_view args silently truncate at the first embedded NUL — jni_new_string_utf routes through std::string::c_str(), so std::string{"a\0b",3} becomes the Java String "a". Length/terminator bug, present even for pure ASCII. No diagnostic.
- **[low]** String args pass raw UTF-8 to NewStringUTF, which expects MODIFIED UTF-8 (U+0000 as C0 80, supplementary chars as CESU-8 surrogate pairs not 4-byte UTF-8). A 4-byte-encoded emoji is malformed input; HotSpot behaviour is implementation-defined (mojibake or thrown -> null result). Library scope arguably ASCII/BMP only, but undocumented at the packing layer.
- **[low]** No arity bound in the heap path; the stack path caps at arg_cap=8 via static_assert. make_jni_args packs an unbounded pack; mismatched arity is 'survive, result unspecified', relying on GetMethodID failing for a wrong signature, not rejected.

## Notes

No memory-safety bug in the packers themselves: the union full-width clear
(value.j = 0) is correct and deliberate, the needs_release tag avoids reading
.l back to classify a slot (the union-aliasing DeleteLocalRef footgun), and
make_jni_args reserves before storing &back() pointers so the heap path has no
realloc-dangling-.l bug. JNIEnv table slots (NewStringUTF=167, NewObjectA=30,
GetStringUTFChars=169/Release=170) are JNI-spec indices, identical JDK 8-26 —
the packer is NOT layout-version-sensitive here. The companion jmethodID is the
version-sensitive piece (JDK 8 raw Method* vs JDK 9+ tagged slot pointer). On
JDKs where StubRoutines::_call_stub_entry is present, method_proxy::call may use
a fast raw-intptr path INSTEAD of this jvalue union; on JDK 21+ it falls back to
call_jni (this packer). Object args are synthetic handles (value.l = &storage,
raw oop), so the packer does NOT compress/decompress oops and is insensitive to
-XX:±UseCompressedOops. The dedicated pure-logic test (tests/test_jni_arg_packing.cpp)
runs with NO JVM (every string arm returns null); the wide-arm end-to-end and
local-ref-release coverage live in the two sibling JVM modules listed above.
