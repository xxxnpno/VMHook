---
slug: constantpool_access
title: Constantpool Access
category: klass
status: in_progress
risk: high
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/in_progress, risk/high, category/klass, tag/constant-pool, tag/symbol, tag/bounds, tag/map-check, tag/field-metadata, tag/method-metadata]
---

# Constantpool Access

> **Category:** [[categories/klass|Class / Klass introspection]]  ·  **Status:** `in_progress`  ·  **Risk:** `high`  ·  **Specialist:** `.claude/agents/constantpool_access-specialist.md`

## Description

Reading HotSpot ConstantPool entries by index — symbol_at-equivalent lookups
(base[index]) that resolve a ConstMethod's name/signature and an InstanceKlass's
field names/signatures. Core consumers: const_method::get_name() / get_signature()
(bounded, safe), klass::find_field() (unbounded, flawed on two code paths), and
raw _pool_holder back-pointer reads. Input: constant_pool* and uint16/uint32 index
from class metadata. Output: symbol* or nullptr on bounds/map failure. Key contract:
1-based indexing (slot 0 unused), per-slot mapped-memory guard before dereference,
_length-bound overflow check (skipped when _length unavailable). Critical asymmetry:
field-stream consumers skip both guards; method consumers have full protection.

## Depends on

- [[features/vmstructs_offset_resolution|vmstructs_offset_resolution]]

## Related

- [[features/compressed_klass_decode|compressed_klass_decode]]
- [[features/compressed_oops_decode|compressed_oops_decode]]
- [[features/find_class_fallback|find_class_fallback]]
- [[features/instanceklass_methods_walk|instanceklass_methods_walk]]
- [[features/klass_introspection|klass_introspection]]
- [[features/signature_parsing|signature_parsing]]
- [[features/field_introspection|field_introspection]]
- [[features/method_overload|method_overload]]

## Depended on by

- [[features/const_method_bounds|const_method_bounds]]
- [[features/field_introspection|field_introspection]]

## Implementation anchors

- `struct constant_pool` — `vmhook/ext/vmhook/vmhook.hpp:1926-1981` — whole type — get_base(), get_length() methods and 1-based indexing contract
- `constant_pool::get_base` — `vmhook/ext/vmhook/vmhook.hpp:1940-1959` — base of entries array — reads ConstantPool type size via VMStruct, computes this+size; throws on missing type; returns nullptr on error
- `constant_pool::get_length` — `vmhook/ext/vmhook/vmhook.hpp:1971-1980` — reads ConstantPool::_length field via cached iterate_struct_entries; returns -1 (unknown/skip-bound) on missing field or invalid this pointer; else reads int32 at this+offset (no is_readable_pointer guard)
- `const_method::get_name` — `vmhook/ext/vmhook/vmhook.hpp:2019-2072` — canonical bounded read — uint16 _name_index, cp->get_base(), cp->get_length() bound check (index >= cp_length), is_readable_pointer per-slot guard, is_valid_pointer on result, try/catch wrapper
- `const_method::get_signature` — `vmhook/ext/vmhook/vmhook.hpp:2077-2130` — parallel bounded read for _signature_index — same guards as get_name
- `const_method::get_constants` — `vmhook/ext/vmhook/vmhook.hpp:1995-2014` — resolves cp pointer from ConstMethod._constants; throws/logs/returns nullptr on missing field
- `klass::find_field_in_stream` — `vmhook/ext/vmhook/vmhook.hpp:2903-2995` — JDK 21+ FieldInfo stream: decodes uint32 name_index / sig_index via decode_u5; indexes constant_pool_base[index] with ONLY (index && is_valid_pointer(loaded_value)) guards — missing _length bound and is_readable_pointer per-slot check (flaw #1)
- `klass::find_field (Array<u2> path)` — `vmhook/ext/vmhook/vmhook.hpp:3086-3118` — JDK 8–17 field array: uint16 name_index / sig_index from 6-slot records; name read has (index && is_valid_pointer) guard (3089/3094); signature read at 3114 is ENTIRELY UNGUARDED before is_valid_pointer on result (flaw #2)
- `klass::find_field (cp base resolution)` — `vmhook/ext/vmhook/vmhook.hpp:3027-3039` — reads InstanceKlass._constants -> constant_pool* -> get_base(); JDK-21+ dispatch at 3042–3045
- `symbol::to_string` — `vmhook/ext/vmhook/vmhook.hpp:1878-1916` — consumes returned symbol*: safe_read_pointer guard, _length/_body via VMStructs, rejects length==0 || length>0x1000
- `is_valid_pointer` — `vmhook/ext/vmhook/vmhook.hpp:1768-1805` — range + 2-byte-align + debug-poison reject — validates pointer value, not memory commitment
- `is_readable_pointer` — `vmhook/ext/vmhook/vmhook.hpp:1739-1753` — range + 8-byte-align + os::query_region committed/readable/!guarded — validates slot is mapped and accessible
- `caller_info (_pool_holder read)` — `vmhook/ext/vmhook/vmhook.hpp:7674-7675` — raw _pool_holder dereference — back-pointer from ConstantPool to owning Klass
- `stack_trace (_pool_holder chain)` — `vmhook/ext/vmhook/vmhook.hpp:7793-7810` — chain of _pool_holder reads; pre-validated at 7772–7776
- `method_proxy JNI fallback (_pool_holder)` — `vmhook/ext/vmhook/vmhook.hpp:12687-12688` — raw _pool_holder dereference for class recovery on method_proxy failure
- `static-overload klass recovery (_pool_holder)` — `vmhook/ext/vmhook/vmhook.hpp:13827-13828` — raw _pool_holder dereference to recover declaring class for static method overload disambiguation

## Tests

- `tests/test_const_method_bounds.cpp`

## Known bugs

- **[high]** Field-stream constant-pool reads (klass::find_field_in_stream JDK 21+ and Array<u2> path JDK 8–17) have NO _length bound and NO per-slot is_readable_pointer check — asymmetric with const_method::get_name/get_signature bounded path. Indices decoded from heap metadata (UNSIGNED5 / u2 array) can be corrupted/mis-decoded; dereference base[idx] without guards at 2975, 2982 (stream), 3094, 3114 (Array). Access violation not recoverable nullptr. Fix: shared bounded accessor mirroring method-path guards.
- **[high]** Array<u2> signature read entirely unguarded (vmhook.hpp:3114) — constant_pool_base[sig_index] loaded with NO preceding (if sig_index && is_valid_pointer(...)) unlike name read at 3094. Only is_valid_pointer on result. Field with signature_index==0 or out-of-range still dereferenced. Most reachable variant of flaw #1 on JDK 8.
- **[medium]** get_length() returns raw int32 with no sanity ceiling; garbage _length (e.g. 0x40000000) passes cp_length >= 0 check, silently disabling bound for all real uint16 indices. Negative _length also disables bound. Bound only trustworthy when _length itself trustworthy. Field-stream indices are uint32 (not uint16), so decode_u5 result in (real_length, 0x7fffffff] bypasses bound entirely.
- **[medium]** get_base() does not validate this pointer (unlike get_length at 1975 and get_name/get_signature at call site). Caller handing bogus constant_pool* to get_base() gets bogus-but-non-null base; downstream is_valid_pointer(base) only checks pointer value, not whether base (this+size) points at committed array. Header-size addition can push near-ceiling this past user_address_ceiling.
- **[low]** get_length() reads _length field without is_readable_pointer (line 1979 only guards with is_valid_pointer at 1975). Possible fault on ConstantPool* range-valid yet pointing into decommitted/guard page during class unload or GC churn. Minor because method-path callers already passed cp through is_valid_pointer, but unguarded raw read on HotSpot-lifetime object.
- **[low]** 1-based-index contract undocumented at read sites and not asserted. Base is 1-based (doc 1934) but get_name/get_signature accept index==0 (only index >= cp_length rejected); base[0] read relies on is_valid_pointer usually failing. Field-stream paths DO special-case name_index==0 (2947, 2975, 3089), enforcing contract inconsistently across two paths.

## Notes

JDK-version sensitivities: ConstantPool header size (get_base, line 1943) is read
per-JDK from gHotSpotVMTypes and adapts across JDK 8–26, but no cross-check if
type is exported with truncated size. ConstantPool::_length availability governs
bound effectiveness — when absent (drops on some future JDK), get_length() returns
-1 and method-path bound disables, leaving only is_readable_pointer. Field metadata
layout diverges: JDK 8–20 use InstanceKlass._fields (Array<u2> 6-slot, cp reads
at 3094/3114); JDK 21.0.x+ / 22+ use _fieldinfo_stream (Array<u1> UNSIGNED5, cp
reads at 2975/2982). Both feed unbounded cp indices, so out-of-range exposure has
two triggers. _pool_holder is exported on every tested JDK (used at 7674/12687/13827)
but raw-offset read assumes it sits at VMStruct offset; field drop → class-name
lookup skipped, not crash. Compressed-class pointers do not change entry width
(Symbol* is native, not narrow); compressed oops irrelevant (cp entries are metadata).
