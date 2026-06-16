---
slug: signature_parsing
title: Signature Parsing
category: method
status: in_progress
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/in_progress, risk/medium, category/method, tag/descriptor, tag/jvm, tag/parsing, tag/type-system, tag/basic-type, tag/jni, tag/field-set, tag/call-stub]
---

# Signature Parsing

> **Category:** [[categories/method|Method proxies (resolve / call / dispatch)]]  ·  **Status:** `in_progress`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/signature_parsing-specialist.md`

## Description

Three pure, JVM-free helpers translate between JVM type-descriptor representations.
Input: a JVM descriptor string such as "(Ljava/lang/String;I[B)V" or a single-char
primitive code. Output: (a) a HotSpot BasicType integer from a descriptor char
(sig_char_to_basic_type), (b) the in-heap byte width of a primitive field from its
single-char descriptor (jvm_primitive_byte_width), and (c) the JNI descriptor string
for a C++ argument type at compile time (jni_signature_for_arg<T>).
These helpers are the spine of every method_proxy::call() return-type decode
(rfind(')')+1 feeds sig_char_to_basic_type to select the HotSpot call-stub result
type), every field_proxy::set() size guard (jvm_primitive_byte_width gates the
memcpy width check), and every jni_make_unique<T>(args...) constructor-signature
build (jni_signature_for_arg folds each C++ arg type into "(...)V").
A wrong row silently corrupts a heap write or causes GetMethodID to fail with no
diagnostic; a malformed return descriptor fed to the call stub misinterprets a
primitive return register as an oop pointer.

## Depends on

- [[features/method_enumeration|method_enumeration]]

## Depended on by

- [[features/find_methods_by_signature|find_methods_by_signature]]
- [[features/hook_basic|hook_basic]]
- [[features/hook_signature|hook_signature]]
- [[features/jni_arg_packing|jni_arg_packing]]
- [[features/method_explicit_signature|method_explicit_signature]]
- [[features/method_is_reference|method_is_reference]]
- [[features/method_overload|method_overload]]

## Implementation anchors

- `detail::sig_char_to_basic_type(char)` — `vmhook/ext/vmhook/vmhook.hpp:15855-15872` — descriptor-char to HotSpot BasicType int; Z=4,C=5,F=6,D=7,B=8,S=9,I=10,J=11,L=12,'['=13,V=14; default->12 (T_OBJECT)
- `detail::jvm_primitive_byte_width(string_view)` — `vmhook/ext/vmhook/vmhook.hpp:15890-15905` — primitive descriptor -> in-heap byte width; size()!=1 returns 0; Z/B=1,S/C=2,I/F=4,J/D=8; default->0
- `detail::jvm_primitive_byte_width (forward decl)` — `vmhook/ext/vmhook/vmhook.hpp:1583-1584` — forward declaration so field_proxy::set template body sees the name at parse time under GCC -Wtemplate-body
- `detail::jni_signature_for_arg<arg_type>()` — `vmhook/ext/vmhook/vmhook.hpp:12697-12789` — C++ type -> JNI descriptor string; string/string_view/char*->Ljava/lang/String;, bool->Z, uint16_t->C, object_base->L...;, unregistered->Ljava/lang/Object;, else static_assert
- `vmhook::signature_for_arg<arg_type>() (public re-export)` — `vmhook/ext/vmhook/vmhook.hpp:13397-13401` — public API; forwards verbatim to jni_signature_for_arg
- `method_proxy::call() return-type extraction (rfind path)` — `vmhook/ext/vmhook/vmhook.hpp:16829-16856` — rfind(')')+1 with bounds+validity guard; unknown return char degrades to T_VOID(14) not T_OBJECT to avoid oop misinterpretation
- `jni_make_unique constructor-signature fold` — `vmhook/ext/vmhook/vmhook.hpp:13167-13171` — folds jni_signature_for_arg<decay<args>>()... into (...)V for GetMethodID('<init>',...)

## Tests

- `tests/test_signature_parsing.cpp`

## Known bugs

- **[medium]** sig_char_to_basic_type default->12 (T_OBJECT) was historically unsafe: a malformed/unknown return descriptor caused the call stub to treat an arbitrary primitive return register as an oop pointer. The call site at vmhook.hpp:16849-16856 now applies a validity whitelist and degrades to T_VOID(14) for unrecognised chars, but the helper itself still returns 12 for any non-table char. Any future call site that passes the helper result directly to the call stub without the validity guard will silently misinterpret garbage as an oop. The defensive fix is in the consumer, not the helper.
- **[low]** rfind(')')+1 index at vmhook.hpp:16846 carries an implicit bounds assumption: a signature ending in ')' (e.g. '(I)' with the return char stripped) would make rparen+1==size(), which is out-of-bounds for std::string_view::operator[] (unlike std::string which guarantees NUL at [size()]). The current guard 'rparen+1 < sig.size()' (16846) closes this for the hot path, but the call_jni path at vmhook.hpp:16192 repeats the same idiom independently — a divergence risk. Tests cover the npos and well-formed cases but not the 'ends-in-)' boundary for the call_jni copy.
- **[low]** uint16_t -> 'C' (Java char) in jni_signature_for_arg (vmhook.hpp:12718-12720) is asymmetric with numeric intuition: a caller expecting to pass a 16-bit integer to a Java short (S) method and using unsigned short will silently build a '(C)V' descriptor instead of '(S)V'. GetMethodID then fails to find the short-taking overload with only a generic 'method not found' log. This is a documented design choice (matches the BasicType table), not a coding error, but it is the single most surprising row and must be pinned explicitly in tests.
- **[low]** jvm_primitive_byte_width reports heap width and is also used to gate field_proxy::set() value-size comparisons against sizeof(C++ value) (vmhook.hpp:15502). For 'C' (Java char, heap width 2), a C++ char (sizeof==1) would fail the guard; the workaround is a dedicated char-widening special-case at vmhook.hpp:15386-15390 outside this helper. Any future call site that reuses jvm_primitive_byte_width for a new size guard without replicating that widening will wrongly reject a 1-byte char for a C field.
- **[low]** Unregistered wrapper fallback in jni_signature_for_arg (vmhook.hpp:12751-12755, 12767-12771) silently returns 'Ljava/lang/Object;' instead of the correct 'Lcom/example/Foo;'. The descriptor is compilable and plausible but wrong: jni_make_unique will fail at GetMethodID with no error beyond a VMHOOK_LOG warning. Callers who forget register_class<T>() get a misleading fallback that looks valid until runtime.

## Notes

JDK-version stability: the BasicType integer values (T_BOOLEAN=4 .. T_VOID=14) in
sig_char_to_basic_type are part of HotSpot's globalDefinitions.hpp enum and have been
unchanged from JDK 8 through JDK 26. The JVM type-descriptor grammar (JVMS §4.3.2)
is version-invariant: single chars for primitives, L...;  for objects, [ prefix for
arrays. No per-JDK branching is needed in any of the three helpers.
UseCompressedOops interaction: jvm_primitive_byte_width returns 0 for reference/array
types on purpose so field_proxy::set defers to the compressed-OOP path; the
parsing layer is stable regardless of heap mode (-XX:-UseCompressedOops, large heap).
char-signedness: sig_char_to_basic_type takes plain char; on MSVC/MinGW char is
signed, so descriptor bytes >=0x80 (impossible in valid descriptors but reachable from
a malformed caller-supplied signature) arrive as negative ints — they still hit the
default->12 arm. The total-char-sweep test in tests/test_signature_parsing.cpp pins
this across the build matrix.
Inner-class descriptors (Ljava/util/Map$Entry;) contain '$' which is legal in the
L...;  grammar; jni_signature_for_arg emits the class-map entry verbatim (including
'$') so inner-class wrappers are handled correctly as long as register_class is called
with the inner-class slashed name.
