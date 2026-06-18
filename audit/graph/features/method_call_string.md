---
slug: method_call_string
title: Method Call String
category: method
status: seeded
risk: high
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/high, category/method, tag/method, tag/call, tag/string, tag/utf-8, tag/modified-utf8, tag/cesu-8, tag/read-java-string, tag/call-jni, tag/decode-divergence]
---

# Method Call String

> **Category:** [[categories/method|Method proxies (resolve / call / dispatch)]]  ·  **Status:** `seeded`  ·  **Risk:** `high`  ·  **Specialist:** `.claude/agents/method_call_string-specialist.md`

## Description

`method_proxy::call()` that returns `java.lang.String`, and the UTF-8 decode
that turns the Java String into an owned `std::string` (extracted via the
unambiguous `value_t::as_string()`, never the implicit conversion which is
ambiguous against `const char*` on MSVC). Contract:
`get_method("foo")->call().as_string()` returns the Java string's bytes
exactly. This was the site of the critical "call-stub truncation" bug (a
String-returning call() handed back a truncated 32-bit OOP handle instead of
decoding the text). The decode has TWO paths that DISAGREE on non-ASCII: the
call_jni fallback (the CI path) uses `jni_get_string_utf` -> GetStringUTFChars
which returns MODIFIED UTF-8 (U+0000 -> C0 80, supplementary scalars ->
CESU-8 surrogate pairs); the call-stub path uses `read_java_string` which walks
the String's value byte[]/char[] off the heap, SUBSTITUTES '?' for every code
unit >= 0x80, and REJECTS length <= 0 or > 4096 (-> "" for empty and >4096).

## Depends on

- [[features/method_call_primitives|method_call_primitives]]
- [[features/make_java_string|make_java_string]]
- [[features/read_java_string|read_java_string]]
- [[features/jni_local_ref_hygiene|jni_local_ref_hygiene]]

## Related

- [[features/method_call_jni_fallback|method_call_jni_fallback]]
- [[features/method_call_object|method_call_object]]
- [[features/method_call_return_void|method_call_return_void]]
- [[features/method_call_wide_args|method_call_wide_args]]
- [[features/method_return_types|method_return_types]]
- [[features/method_static|method_static]]
- [[features/field_string|field_string]]

## Depended on by

- [[features/method_overload_java_dispatch|method_overload_java_dispatch]]

## Implementation anchors

- `method_proxy::value_t::as_string` — `vmhook/ext/vmhook/vmhook.hpp:16180-16207` — std::visit: returns the std::string alternative as-is, read_java_string(decode_oop_pointer(v)) for the uint32 alternative, else ""
- `method_proxy::call_jni ('L'/'[' String branch)` — `vmhook/ext/vmhook/vmhook.hpp:16233-16870` — the CI path: detects Ljava/lang/String;, jni_get_string_utf (GetStringUTFChars -> MODIFIED UTF-8) then jni_delete_local_ref (extract-then-release leak fix)
- `method_proxy::call (call-stub default arm)` — `vmhook/ext/vmhook/vmhook.hpp:16871-17050` — read_java_string(result_oop) off the heap — substitutes '?' for code units >= 0x80 and rejects length <= 0 or > 4096
- `detail::jni_get_string_utf / read_java_string` — `vmhook/ext/vmhook/vmhook.hpp:12709-12740` — jni_get_string_utf: null-guard, GetStringUTFChars(169) into owned std::string, ReleaseStringUTFChars(170); read_java_string defn at 20098

## Tests

- `tests/jvm/modules/method_call_string.cpp`

## Known bugs

- **[high]** [headline bug, now fixed; the module is its regression wall] The call-stub default arm used to be value_t{static_cast<uint32_t>(result_holder)}, which truncated the 64-bit oop AND mislabelled it as a compressed OOP, so a Ljava/lang/String; call() returned "" on JDK 8/11/17 (call-stub present) while the JNI path returned the text. Now decodes via read_java_string on the stub and GetStringUTFChars on call_jni. The interior-NUL case (a\0b) is the sharpest guard (a naive C-string copy would terminate at the NUL).
- **[high]** [STILL LIVE divergence] The two decoders produce DIFFERENT bytes for the same Java String: GetStringUTFChars (call_jni) preserves unicode as modified UTF-8 (BMP correct, supplementary as CESU-8 surrogate pairs), while read_java_string (call-stub) substitutes '?' for every code unit >= 0x80 and rejects strings longer than 4096. So a non-ASCII or >4096-char String returns different content depending on which dispatch path the live JDK takes — only ASCII <= 4096-char strings round-trip identically on both.

## Notes

CI actually takes the call_jni path (_call_stub_entry is absent from VMStructs on
every JDK CI runs: 8, 11, 17, 21, 24, 25). read_java_string rejects length <= 0
(so the empty string returns "" on the call-stub path) and length > 4096; the
coder branch handles JDK 8 char[] vs JDK 9+ LATIN1 (raw byte copy) / UTF16 ('?'
for >= 0x80). The module uses as_string() everywhere (never the implicit
conversion), installs a scoped_hook on trigger(), captures results into a
mutex-guarded map, and asserts after run_probe. To keep the two decoders from
diverging, the fixture strings are ASCII; the unicode/interior-NUL cases are
characterized as known divergences, not hard-asserted equal across paths.
Java-8-only fixture; call() runs inside the trigger() detour.
