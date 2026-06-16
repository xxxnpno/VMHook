---
slug: field_arrays_object
title: Field Arrays Object
category: field
status: in_progress
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/in_progress, risk/medium, category/field, tag/field, tag/reference, tag/array, tag/string, tag/object, tag/vector, tag/compressed-oop, tag/memory-introspection]
---

# Field Arrays Object

> **Category:** [[categories/field|Field proxies (get / set / introspection)]]  ·  **Status:** `in_progress`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/field_arrays_object-specialist.md`

## Description

Read Java reference arrays (`String[]` and `Object[]` of registered wrapper types)
from static and instance fields, converting them into C++ vectors with null-safe
handling — null array references decode as empty vectors, null elements within
arrays are coerced to empty strings or nullptr slots without crashing. The feature
decodes via compressed-OOP layout (fixed 12/16-byte array header), cross-checks
element counts against Java-published length oracles, and proves object identity
through unique tags and raw-OOP determinism across re-reads.

## Depends on

- [[features/field_object_ref|field_object_ref]]
- [[features/array_element_helpers|array_element_helpers]]
- [[features/wrapper_pattern|wrapper_pattern]]
- [[features/compressed_oops_decode|compressed_oops_decode]]

## Related

- [[features/field_arrays_primitive|field_arrays_primitive]]
- [[features/field_object_ref|field_object_ref]]
- [[features/field_string|field_string]]

## Implementation anchors

- `field_proxy::value_t::operator target_type()` — `vmhook/ext/vmhook/vmhook.hpp:11886-11894` — String[] implicit conversion entry point — std::visit over variant
- `cast_for_variant<vector<...>>` — `vmhook/ext/vmhook/vmhook.hpp:11810-11820` — routes vector target to read_array_value
- `read_array_value<target_type>` — `vmhook/ext/vmhook/vmhook.hpp:11747-11771` — decodes array oop, checks length, reserves, loops per-element append
- `append_array_value(vector<string>&, ...)` — `vmhook/ext/vmhook/vmhook.hpp:11691-11696` — null-coercion site for String[] — reads element, decodes oop, calls read_java_string
- `field_proxy::value_t::to_vector<element_type>()` — `vmhook/ext/vmhook/vmhook.hpp:11945-11947` — declaration of documented Object[] entry point
- `to_vector<element_type>() definition` — `vmhook/ext/vmhook/vmhook.hpp:15638-15686` — Object[] impl — branches on signature '[L' or '[[', walks array directly vs fallback
- `signature branch (signature[0]=='[' && ...)` — `vmhook/ext/vmhook/vmhook.hpp:15659-15683` — raw object array direct walk — null elements become nullptr, non-null become unique_ptr
- `collection::to_vector<element_type>()` — `vmhook/ext/vmhook/vmhook.hpp:14792-14834` — fallback for non-array references — probes InstanceKlass for ArrayList/LinkedList/HashSet
- `vmhook::field_oop(const field_proxy&)` — `vmhook/ext/vmhook/vmhook.hpp:15870-15874` — manual walk entry — decodes stored compressed OOP to array oop or nullptr
- `vmhook::array_length(void*)` — `vmhook/ext/vmhook/vmhook.hpp:11542-11551` — reads int length at array_oop + 12 (compressed-OOP header), 0 on invalid
- `vmhook::get_array_element<uint32_t>` — `vmhook/ext/vmhook/vmhook.hpp:11563-11581` — bounds-checked memcpy of narrow element (4 bytes) at array_oop + 16 + index*4
- `hotspot::decode_oop_pointer(uint32_t)` — `vmhook/ext/vmhook/vmhook.hpp:4288-4352` — narrow_oop_base + (compressed << narrow_oop_shift); returns nullptr for 0 or absent VMStructs
- `vmhook::decode_array_oop(uint32_t)` — `vmhook/ext/vmhook/vmhook.hpp:16078-16087` — decode_oop_pointer + is_valid_pointer, nullptr on 0/invalid
- `read_java_string(void*)` — `vmhook/ext/vmhook/vmhook.hpp:15723-15855` — null/invalid -> warning_log + empty string; else decodes via String 'value' backing array (compact-string aware)

## Tests

- `tests/jvm/modules/field_arrays_object.cpp`

## Known bugs

- **[high]** Test module STALE PREMISE (field_arrays_object.cpp:29-42, PART B1 at 398-411, gated checks at 416-432) — module asserts to_vector<Item>() on raw Object[] mis-routes through collection::to_vector and returns empty. This route is FIXED since vmhook.hpp:15659-15683 added explicit '[L' or '[[' signature branch that walks the array directly. Today to_vector<Item>() returns correct elements; the gated 'if (canon.size() == 3)' and 'if (mixed.size() == 3)' blocks now FIRE and must PASS. The documented Object[] entry point WORKS; PART B manual walk is now redundant cross-check. ACTION: promote gated B1 assertions to unconditional, delete 'broken' [INFO], and make any _elem*_tag* failure a regression in the 15659 branch.
- **[medium]** Null String[] element silently coerced to empty string — null-vs-empty information loss. At vmhook.hpp:11695 null element becomes read_java_string(decode_oop_pointer(0)) = read_java_string(nullptr) which returns empty string (vmhook.hpp:15730) indistinguishable from genuine empty Java string. Same call logs warning_tag per null slot (vmhook.hpp:15728-15729). Asymmetric with Object[] path which preserves null as real nullptr slot. FIX: null-preserving overload (vector<optional<string>>) + non-logging null short-circuit in String[] append.
- **[medium]** Reference-array path silently assumes COMPRESSED oops and fixed 12/16-byte header. Every read uses 4-byte narrow element (vmhook.hpp:11694) plus decode_oop_pointer's base+(c<<shift). Data offset hard-coded +16, length +12 (vmhook.hpp:11550, 11579). With -XX:-UseCompressedOops (heap ≥32GB or future default) element stride is 8 bytes and VMStructs absent -> decode_oop_pointer returns nullptr (vmhook.hpp:4342-4345) -> every element null -> String[] all-empty and Object[] all-nullptr, silent wrong-answer. CI runs default compressed heaps so module green; out-of-test hazard with uncompressed-OOP builds.
- **[low]** No bounds clamp between array_length and element loop beyond per-element checks; corrupt _length trusted for reserve. read_array_value (vmhook.hpp:11758-11764) and to_vector array branch (vmhook.hpp:15663-15666) reserve(length) from raw header int at +12 unvalidated beyond is_valid_pointer(array_oop). Bogus huge length on corrupted array oop is bad_alloc-sized reserve before per-element get_array_element bounds checks. Practically unreachable on sane heap but no upper sanity clamp like read_java_string's 4096 cap (vmhook.hpp:15763).
- **[low]** to_vector<T>()'s fallback mis-route exists for mis-signatured field. If field's stored signature does NOT start with '[' but OOP is raw array (registration/signature mismatch), 15659 guard skipped and collection::to_vector (vmhook.hpp:15685, 14792+) probes ObjArrayKlass as InstanceKlass via get_field_by_oop_klass. is_valid_pointer (vmhook.hpp:1768) coarse even/in-range/sentinel filter, stray aligned probe hit yields bogus count. Signature branch makes unreachable for correct '[L…' fields; latent hazard only under signature mismatch.

## Notes

Compact strings (JDK 8 vs 9+): read_java_string branches on coder field presence.
JDK 8 UTF-16 char[] (length=char count) vs JDK 9+ byte[]+coder (LATIN1=1 byte/char,
UTF16=2 bytes/char). Compact-string regression surfaces in PART A/C element-content
assertions. Compressed-OOP base/shift VMStruct renames across JDK versions (8-16 /
17-24 / 25+) via Universe/CompressedOops namespaces; new layout match failure returns
nullptr. Array header offsets (length at +12, data at +16) hard-coded to compressed-OOP
arrayOopDesc; uncompressed-klass build shifts these. Item$ nested-class must use '$'
separator in registered name. Feature is compressed-layout-locked: both payload stride
and offset decode assumptions fail silently with -XX:-UseCompressedOops or ≥32GB heaps.
