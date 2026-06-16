---
slug: const_method_bounds
title: Const Method Bounds
category: klass
status: in_progress
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/in_progress, risk/medium, category/klass, tag/bounds-check, tag/constant-pool, tag/symbol-resolution, tag/defensive, tag/AV-guard]
---

# Const Method Bounds

> **Category:** [[categories/klass|Class / Klass introspection]]  ·  **Status:** `in_progress`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/const_method_bounds-specialist.md`

## Description

Defensive bounds logic (FIX B, commit ab87ea7) hardening ConstMethod::get_name() and
get_signature() against corrupt/out-of-range u2 indices and unmapped constant-pool slots.
The feature reads a Symbol* from ConstantPool::get_base()[index] with two guards:
(1) index-bounds check via ConstantPool::_length, (2) per-slot is_readable_pointer() probe.
Input: a u2 index from ConstMethod metadata; output: Symbol* or nullptr (no AV on corrupt input).
Critical gap: sibling field-symbol readers (klass::find_field*) use the same ConstantPool
array but lack identical guards — identical AV hazard left unpatched.

## Depends on

- [[features/constantpool_access|constantpool_access]]
- [[features/decode_oop_and_pointers|decode_oop_and_pointers]]
- [[features/os_query_region|os_query_region]]

## Related

- [[features/hook_basic|hook_basic]]
- [[features/method_enumeration|method_enumeration]]
- [[features/klass_introspection|klass_introspection]]
- [[features/find_methods_by_signature|find_methods_by_signature]]

## Implementation anchors

- `const_method::get_constants()` — `vmhook/ext/vmhook/vmhook.hpp:1995-2014` — reads ConstMethod._constants VMStruct field, returns constant_pool* (the pool every index is resolved against)
- `const_method::get_name()` — `vmhook/ext/vmhook/vmhook.hpp:2019-2072` — protected read: resolve _name_index → guard this → read u2 index → resolve and guard cp → FIX B length bound (2051-2055) → FIX B slot probe (2056-2059) → read and reinterpret as Symbol*
- `const_method::get_signature()` — `vmhook/ext/vmhook/vmhook.hpp:2077-2130` — byte-for-byte identical to get_name() but keyed on _signature_index (2080, 2093); same FIX B guards at 2109-2113 (length bound) and 2114-2117 (slot probe)
- `constant_pool::get_base()` — `vmhook/ext/vmhook/vmhook.hpp:1940-1959` — returns base pointer to entry array (immediately after ConstantPool header); indices are 1-based (0 is unused)
- `constant_pool::get_length()` — `vmhook/ext/vmhook/vmhook.hpp:1971-1980` — NEW in FIX B: reads ConstantPool._length as jint, returns -1 when field absent (unknown sentinel); documented to degrade gracefully on JDKs that drop _length
- `is_readable_pointer()` — `vmhook/ext/vmhook/vmhook.hpp:1739-1753` — FIX B slot probe: rejects unmapped/unreadable addresses via os::query_region (VirtualQuery/proc); required for crash-proofing on unmapped slots
- `is_valid_pointer()` — `vmhook/ext/vmhook/vmhook.hpp:1768-1805` — range + bit-0-alignment + poison rejection; guards this, cp, base, and resolved entry_pointer
- `method::get_name()` — `vmhook/ext/vmhook/vmhook.hpp:2292-2325` — public consumer: guards this → get_const_method() → const_method::get_name() → symbol::to_string()
- `method::get_signature()` — `vmhook/ext/vmhook/vmhook.hpp:2330-2362` — public consumer: guards this → get_const_method() → const_method::get_signature() → symbol::to_string()
- `symbol::to_string()` — `vmhook/ext/vmhook/vmhook.hpp:1878-1916` — final consumer: safe_read_pointer(this) → read u2 _length/_body → length sanity gate (0 < length <= 0x1000); catches wrong-but-mapped Symbol*
- `klass::find_field()` — `vmhook/ext/vmhook/vmhook.hpp:3088-3114` — [UNGUARDED] reads constant_pool_base[name_index] at 3094 and [sig_index] at 3114 — same CP array, no FIX B guards (flaw #1)
- `klass::find_field_in_stream()` — `vmhook/ext/vmhook/vmhook.hpp:2975-2984` — [UNGUARDED] reads constant_pool_base[name_index]/[sig_index] at 2975-2977 / 2982-2984 — same CP array, no FIX B guards (flaw #1)

## Known bugs

- **[medium]** Sibling field-name/signature reads in klass::find_field() (3094, 3114) and klass::find_field_in_stream() (2975-2977, 2982-2984) use the same ConstantPool::get_base() array with u2 indices from metadata, but lack identical FIX B guards (no get_length() bound, no is_readable_pointer slot probe). They go straight to is_valid_pointer(base[index]), which dereferences before validating — identical AV hazard as the method path pre-FIX-B. The fix belongs in a shared helper (constant_pool::symbol_at(index)) used by all five call sites. JDK 8..early-21 hit klass::find_field; JDK 21.0.x+/22+ hit find_field_in_stream — different code on different versions.
- **[low]** Negative/garbage _length silently disables the bound (2052: cp_length >= 0 && index >= cp_length). When _length reads as negative (including -1 sentinel and any corrupt jint), the length guard is skipped entirely, leaving only is_readable_pointer protection. Mitigated by slot probe and symbol::to_string length gate, but the precise case the feature targets (corrupted ConstantPool) is most likely to corrupt _length and turn the bound off. The length bound is best-effort fast-reject, not a guarantee.
- **[low]** Index 0 (documented as unused/reserved in ConstantPool header at 1934) is not rejected — bound is [0, cp_length) not semantically-correct [1, cp_length). For valid method _name_index/_signature_index >= 1, so never fires green path; base[0] is caught by downstream is_valid_pointer. But documents flaw: a future fix to enforce [1, cp_length) is deliberately test-visible.
- **[low]** No upper sanity cap on cp_length itself (get_length() returns whatever the _length slot holds at 1979, no ceiling). A corrupt huge positive _length (e.g. 0x7FFFFFFF) makes index >= cp_length vacuously false for every realistic u2 (max 65535), deferring all protection to slot probe. Real pools are small; a sane upper clamp would tighten the fast path but absence is not a crash.
- **[low]** get_length() reads a 4-byte field via uint64_t offset with no readability check on the field address (1975 guards is_valid_pointer(this) — range/alignment only, no is_readable_pointer on this+offset). If this passes range/alignment/poison filters yet this+offset lands in unmapped page, the _length read itself can fault before any bound applies. Subtle, low-probability, but the only read in FIX B path crossing a VMStruct offset without region probe.

## Notes

ConstantPool._length export presence drives the entire bound: exported → index bounded;
absent → get_length() returns -1, bound skipped (only is_readable_pointer protects).
The field has been stable VMStruct export on HotSpot 8..25; feature written to survive removal.
ConstantPool header size (→ get_base() offset) varies by JDK — uses runtime iterate_type_entries("ConstantPool")->size,
so base is correct across versions; missing type entry makes get_base() return nullptr.
ConstMethod._name_index/_signature_index offsets VMStruct-resolved (2022/2080); present 8..25.
Missing struct entry makes corresponding getter return nullptr (2026-2028 / 2084-2086).
JDK 8 vs 9+ field-symbol path divergence (flaw #1): JDK 8..early-21 use klass::find_field Array<u2> (3094);
JDK 21.0.x+/22+ use klass::find_field_in_stream (UNSIGNED5, 2975).
Unguarded sibling reads split by version — corrupt-field-index test must run on both old and new JDK.
Compressed class pointers / heap size irrelevant — CP entries are raw Symbol* (Metaspace), pure u2-index arithmetic, heap-config independent.
