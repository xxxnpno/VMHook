---
slug: compressed_klass_decode
title: Compressed Klass Decode
category: klass
status: in_progress
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/in_progress, risk/medium, category/klass, tag/klass, tag/compressed-pointers, tag/header, tag/metadata, tag/OOP-sibling, tag/Lilliput-risk, tag/JDK-variant]
---

# Compressed Klass Decode

> **Category:** [[categories/klass|Class / Klass introspection]]  ·  **Status:** `in_progress`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/compressed_klass_decode-specialist.md`

## Description

Decodes a 32-bit narrow Klass pointer (from the `_metadata._compressed_klass` slot at byte offset +8 in a Java object header) into a real 64-bit `Klass*` via the CompressedKlassPointers base/shift VM registers, and encodes the inverse. This is the metadata-pointer twin of the OOP codec—a separate compressed-pointer scheme with distinct base/shift (`CompressedKlassPointers::_narrow_klass.{_base,_shift}`), not the OOP base/shift. Every `klass_from_oop()` call funnels through this decode, making it the backbone of runtime-type resolution for `collection<T>`, `for_each_instance`, polymorphic field/method dispatch, and `make_unique` sanity checks. Input contract: `decode_klass_pointer(uint32_t compressed) -> void*` (nullable on zero or missing VMStructs); `encode_klass_pointer(void* decoded) -> uint32_t` (nullable on nullptr or out-of-base). Surprising bit: the hard-coded `oop + 8` read in `klass_from_oop` is wrong under compact object headers (Lilliput JEP 450), and the codec assumes compressed class pointers are enabled—no fallback for `-XX:-UseCompressedClassPointers`.

## Depends on

- [[features/vmstructs_offset_resolution|vmstructs_offset_resolution]]
- [[features/compressed_oops_decode|compressed_oops_decode]]
- [[features/decode_oop_and_pointers|decode_oop_and_pointers]]

## Related

- [[features/compressed_oops_decode|compressed_oops_decode]]
- [[features/constantpool_access|constantpool_access]]
- [[features/find_class_fallback|find_class_fallback]]
- [[features/instanceklass_methods_walk|instanceklass_methods_walk]]
- [[features/klass_introspection|klass_introspection]]
- [[features/for_each_instance|for_each_instance]]
- [[features/make_unique|make_unique]]

## Depended on by

- [[features/klass_introspection|klass_introspection]]

## Implementation anchors

- `hotspot::decode_klass_pointer(uint32_t compressed)` — `vmhook/ext/vmhook/vmhook.hpp:4433-4495` — early-return nullptr for compressed==0 (4436-4439); lazy-resolved cached base/shift with 3-tier JDK fallback (4445-4484); reads uint64_t base and uint32_t shift; returns base + (uint64_t(compressed) << shift) (4494)
- `hotspot::encode_klass_pointer(void* decoded)` — `vmhook/ext/vmhook/vmhook.hpp:4511-4575` — inverse path; nullptr -> 0 (4514-4517); cached base/shift same as decode (4519-4558); guards decoded_address < base (4569-4572); returns uint32_t((decoded_address - base) >> shift) (4574)
- `vmhook::klass_from_oop(void* oop)` — `vmhook/ext/vmhook/vmhook.hpp:14597-14611` — headline consumer; null/validity-gates oop (14599-14602); reads narrow klass at HARD-CODED oop+8 (14603-14604); decodes (14605); validity-gates result (14606-14609); casts to klass*
- `object_base::klass_from_object_header(void* oop)` — `vmhook/ext/vmhook/vmhook.hpp:13546-13563` — second structurally-identical copy of klass_from_oop; same literal +8 (13554-13555), decode (13556), re-validate (13557-13560); byte-for-byte duplicate — maintenance hazard
- `vm_struct_entry_t` — `vmhook/ext/vmhook/vmhook.hpp:1612-1620` — VMStruct entry record (type_name, field_name, type_string, is_static, offset, address); codec reads through ->address which dereferences live global, picking up post-init mutations
- `hotspot::iterate_struct_entries(type, field)` — `vmhook/ext/vmhook/vmhook.hpp:1711-1730` — linear strcmp scan of gHotSpotVMStructs; returns nullptr if absent (no-JVM / future-JDK fallback, codec degrades gracefully)
- `hotspot::is_valid_pointer(result)` — `vmhook/ext/vmhook/vmhook.hpp:1768-1805` — range/alignment/poison gate applied to decode result by consumers, NOT inside codec itself; checks [floor, ceiling] + bit-0 alignment + poison switch
- `for_each_instance heap scan (decode caller)` — `vmhook/ext/vmhook/vmhook.hpp:6868-6878` — 8-byte-stride walk; at each step decodes narrow klass at +8 and compares against target_klass; vulnerable to garbage decode from non-object reads
- `jni_make_unique decode site` — `vmhook/ext/vmhook/vmhook.hpp:10401-10412` — returned-type sanity log; decodes and guards via is_valid_pointer

## Tests

- `tests/test_compressed_klass_decode.cpp`

## Known bugs

- **[medium]** Decoded address is trusted completely — no alignment or class-space-range check. decode_klass_pointer (4494) returns base + (compressed << shift) for any non-zero input. A garbage or torn narrow-klass word (e.g. from for_each_instance 8-byte stride scan at 6866-6878, or from mark-adjacent forwarded/locked slot) yields plausible pointer. Only safety is caller's is_valid_pointer (14606, 13557, 6875); codec itself unguarded. Klass structures are 8-byte aligned in practice but codec never enforces (result & 0x7) == 0, never checks result in [base, base + 2^(32+shift)). Wrong-but-canonical class-space pointer passes is_valid_pointer.
- **[medium]** klass_from_oop / klass_from_object_header hard-code narrow klass at object offset +8 — wrong under compact object headers (Lilliput JEP 450, preview JDK 24/25). vmhook.hpp:14603-14604 and 13554-13555 both do *(uint32_t*)(oop + 8). Correct for traditional header (8-byte mark at +0, narrow klass at +8) but NOT under -XX:+UseCompactObjectHeaders where klass is packed into mark word and +8 is first instance field. Reading +8 feeds instance-field bit pattern into decoder -> wrong klass still passing is_valid_pointer. No detection of compact-header mode in this path. Severity capped at medium because compact headers are opt-in as of JDK range vmhook targets, but latent correctness break.
- **[medium]** No handling for '-XX:-UseCompressedClassPointers' case. On 64-bit JVM with compressed class pointers disabled, +8 slot holds full 64-bit Klass*, not 32-bit narrow. Both header readers unconditionally read 4 bytes (14603-14604, 13554-13555) and route through narrow decoder. When narrow pointers off, CompressedKlassPointers::_narrow_klass._base/shift not meaningful (VMStruct may be absent), so decode_klass_pointer returns nullptr (entries missing -> 4486-4489) or garbage (truncated_low32 << shift). Feature silently assumes compressed class pointers ON. No fallback to 64-bit read exists.
- **[low]** Asymmetric guard between codec twins: encode_klass_pointer guards decoded_address < base (4569-4572) before subtraction, but decode_klass_pointer performs no symmetric overflow/range check on output. Separate maintenance hazard: klass_from_oop (14597) and klass_from_object_header (13546) are byte-for-byte duplicated; any future fix (compact headers, alignment check) must be applied in both or they silently diverge.
- **[low]** Round-trip identity only holds when shift == 0; codec never documents/enforces it. encode(decode(c)) == c holds only if low shift bits of c << shift are zero. decode accepts arbitrary c, so encode(decode(0x...001)) with shift==3 silently loses low 3 bits. Unaligned decoded pointer fed to encode loses low shift bits with no diagnostic. Edge case but worth test pin so future refactor changing shift handling is caught.
- **[low]** Static-cached base/shift entries resolved once, forever. Function-local statics (4445-4484, 4519-4558) cache entry pointer; codec re-reads *entry->address each call (4491-4492) so mutated base is seen. But if first call happens before gHotSpotVMStructs populated (ultra-early injection) cached value is nullptr permanently, codec returns nullptr/0 forever even after JVM init. Same first-call-wins hazard as OOP codec; relevant only to ultra-early injection.

## Notes

VMStruct field-name migration (3-tier fallback at 4449-4462 / 4469-4482): JDK 8-16 had Universe::_narrow_klass._base/._shift (third tier); JDK 17-24 moved to CompressedKlassPointers::_narrow_klass._base/._shift (first tier); JDK 25+ dropped _narrow_klass prefix -> CompressedKlassPointers::_base/._shift (second tier). Fallback order is first-match-wins, so future rename returns nullptr and codec degrades gracefully. Compressed class pointers introduced JDK 8 (Metaspace); absent on JDK 7 / 32-bit / -XX:-UseCompressedClassPointers. Compact object headers (JEP 450, preview JDK 24-25) move klass out of +8 slot into mark word — biggest forward-compat risk (directly breaks hard-coded +8 reads). Narrow klass shift typically 0 (compressed class space fits without shift) or 3 (fitting with shift); NOT necessarily equal to OOP shift. vmhook re-reads live _shift each call (4492), tracking correctly, but tests must cover both 0 and non-0. klass_from_oop is resolution backbone for collection<T>, for_each_instance, polymorphic dispatch, make_unique — any JDK breaking this decode breaks all at once with symptom being wrong/empty type, not crash.
