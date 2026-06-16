---
slug: decode_u5_unsigned5
title: Decode U5 Unsigned5
category: infra
status: in_progress
risk: high
java_versions: [21, 24, 25, 26]
tags: [status/in_progress, risk/high, category/infra, tag/variable-length-encoding, tag/unsigned5, tag/field-stream, tag/jdk21+, tag/hotspot-codec]
---

# Decode U5 Unsigned5

> **Category:** [[categories/infra|Infrastructure (wrappers, traits, macros, logging)]]  ·  **Status:** `in_progress`  ·  **Risk:** `high`  ·  **Specialist:** `.claude/agents/decode_u5_unsigned5-specialist.md`

## Description

UNSIGNED5 variable-length integer decoder for JDK 21+ `InstanceKlass::_fieldinfo_stream`.
Takes a byte buffer pointer, an in/out cursor `stream_pos`, and returns a `std::uint32_t` value
using HotSpot's base-64 excess-1 codec (value = Σ (b_i − 1)·64^i) with up to 5 bytes max.
High bytes (≥192) signal continuation; low bytes (<192) are terminal.  Byte 0 is the End
sentinel, rewinding the cursor.  Sole production caller is `find_field_in_stream()`, which
decodes field metadata records in the JDK 21+ format; the decoder has no internal bounds checks
and can over-read on truncated/corrupt streams.

## Depends on

- [[features/klass_introspection|klass_introspection]]

## Related

- [[features/field_introspection|field_introspection]]
- [[features/field_object_ref|field_object_ref]]

## Implementation anchors

- `decode_u5(const std::uint8_t* data, int& stream_pos) noexcept` — `vmhook/ext/vmhook/vmhook.hpp:2870-2889` — the decoder itself — bounded 5-byte loop, continuation/terminal logic, End sentinel rewind
- `decode_u5 contract docblock` — `vmhook/ext/vmhook/vmhook.hpp:2862-2869` — base-64 excess-1 codec spec — high byte ≥192, low byte <192, byte 0 = End
- `find_field_in_stream(std::string_view name, void** constant_pool_base)` — `vmhook/ext/vmhook/vmhook.hpp:2903-2995` — sole production caller — resolves Array<u1>, reads stream header, decodes field records with 5 mandatory + 3 optional UNSIGNED5 values
- `find_field(std::string_view name)` — `vmhook/ext/vmhook/vmhook.hpp:3015-3045` — format-dispatch entry — routes to find_field_in_stream if _fieldinfo_stream VMStruct exists
- `Array<u1> data offset layout note` — `vmhook/ext/vmhook/vmhook.hpp:2921-2925` — u1 arrays have 1-byte alignment so data offset is +4 (not +8 like u2/pointer arrays)

## Tests

- `tests/jvm/modules/decode_u5_unsigned5.cpp`
- `tests/test_decode_u5.cpp`

## Known bugs

- **[high]** No internal bounds check; over-read past stream is possible on truncated/corrupt field record (2876 vs 2944). decode_u5() reads data[stream_pos++] with zero awareness of array length. Caller checks stream_pos < length only once per field at loop top (2944), but each field decodes 5 mandatory + up to 3 optional values (2946-2972) with no re-check between them. If _length is honest but stream internally malformed (field near tail claiming optionals running off end), decoder walks past arr_ptr+4+length reading adjacent heap metadata. With all-0 trailing heap stops at first 0, but non-zero neighbours produce silent garbage offsets. Fix: pass length to decode_u5 (or data_end pointer) and bail when stream_pos >= length; re-check after each per-field decode.
- **[medium]** field_offset returned raw, never validated (2953, 2990). JDK 8–17 path derives offset arithmetically and is range-bound by packed u2s; stream path returns decode_u5 output directly. Corrupt/over-read stream can hand back multi-MB or wrapped offset that later read_field/get_field dereferences as this+offset → wild read. Decoder is correct per spec but untrusted output flows unchecked into pointer computation. Fix: sanity-cap field_offset (e.g. against instance size) before constructing field_entry_t.
- **[low]** 5-byte values silently truncate to 32 bits with no diagnostic (2882, byte_position 4 shifts by 6*4=24). At position 4, contribution (current_byte-1)<<24 places high digit in bits 24..31; any magnitude encoder would spill into 6th group never read (loop cap <5 at 2874). Top bits of position-4 digit >=0x40 fall off std::uint32_t. Matches HotSpot's MAX_LENGTH==5 (correct for real u4 field-stream values), but function returns plausible wrong number not signalling truncation. Arithmetic is not UB — unsigned wraparound defined, five 255 bytes sum to 4278124286 < UINT32_MAX. Pure boundary/documentation hazard.
- **[low]** ~0u (4294967295) is both End sentinel and representable decode value (2880 vs 2888). Genuine 5-continuation-byte value can never equal ~0u (max is 4278124286), so no collision today — but contract leans on numeric coincidence rather than explicit success/marker flag. Caller treats name_index==~0u as stop (2947) and num_*_fields==~0u as abort (2939); both correct only while 5-byte cap holds. Any future widening to u8/6 bytes would alias End with real value. Hazard, not present bug.

## Notes

JDK 21+ feature only. decode_u5 reached exclusively when InstanceKlass::_fieldinfo_stream VMStruct
exported (fis_entry, 3019/3042). UNSIGNED5 Array<u1> format introduced JDK 21 (JDK-8292758), spans JDK 22-26.
JDK 8–17 use Array<u2> 6-slot FieldInfo path (3047+). Pure decoder test compiles/runs every toolchain
(byte-buffer-only, no JVM); integration coverage meaningful only JDK 21+. Array<u1> data offset +4
(no 4-byte pad after int32_t _length, unlike pointer/u2 arrays) — holds across x64 HotSpot 21..26.
Optional-entry field_flags bits (0x01 initval, 0x04 generic_sig, 0x10 contended_group, 2961-2972)
track fieldInfo.hpp FieldInfo::FieldFlags; JDK adding/renumbering flag bits carrying UNSIGNED5 payload
would desynchronize cursor. UNSIGNED5 MAX_LENGTH==5 matches HotSpot's 32-bit codec across all versions.
