---
slug: enum_singleton
title: Enum Singleton
category: lifecycle
status: in_progress
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/in_progress, risk/medium, category/lifecycle, tag/enum, tag/singleton, tag/nested-class, tag/field-read, tag/static-field, tag/oop-decode, tag/reference]
---

# Enum Singleton

> **Category:** [[categories/lifecycle|Lifecycle hooks (shutdown / class-load / exception / enum)]]  ·  **Status:** `in_progress`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/enum_singleton-specialist.md`

## Description

Reading Java enum constants as ordinary heap singletons via three paths: instance
enum-reference fields, static enum-reference fields, and enum synthetic constant statics.
Decodes each compressed OOP into a wrapper, reads enum-body fields (rgb), dispatches
instance methods (brightness), and validates OOP-level identity/distinctness. Exercises
generic field-reference and static-field machinery on nested `$`-named enum classes.

## Depends on

- [[features/wrapper_pattern|wrapper_pattern]]
- [[features/klass_introspection|klass_introspection]]
- [[features/field_introspection|field_introspection]]
- [[features/field_static|field_static]]
- [[features/compressed_oops_decode|compressed_oops_decode]]
- [[features/find_class_fallback|find_class_fallback]]

## Implementation anchors

- `vmhook::register_class<T>(class_name)` — `vmhook/ext/vmhook/vmhook.hpp:6916-6952` — resolves klass via find_class, installs factory into g_type_factory_map; module registers EnumSingleton and nested EnumSingleton$Color
- `object<derived> CRTP wrapper` — `vmhook/ext/vmhook/vmhook.hpp:14471-14582` — supplies static_field(name) / static_method(name) accessors (14559-14563 / 14568-14572) used by module; deducing-this instance overloads at 14497-14514
- `object_base::get_field(name) instance` — `vmhook/ext/vmhook/vmhook.hpp:14048-14093` — resolves klass, calls find_field, returns field_proxy at instance + offset (14091-14092); path for favoriteColor / rgb reads
- `object_base::get_field(type_index, name) static` — `vmhook/ext/vmhook/vmhook.hpp:14110-14150` — resolves field, requires entry->is_static (14131), computes pointer as declaring-klass mirror + offset (14140-14148); path for staticColor / RED/GREEN/BLUE constant reads
- `object_base::get_method(name)` — `vmhook/ext/vmhook/vmhook.hpp:14166-14186` — walks superclass chain over InstanceKlass::_methods; module uses for brightness() method dispatch
- `vmhook::find_field(klass, name)` — `vmhook/ext/vmhook/vmhook.hpp:10997-11046` — caches per (klass, name); walks get_super() (11025), records entry->declaring_klass (11037) for inherited static resolution; per-InstanceKlass decode at 3015-3121 with JDK21+ _fieldinfo_stream (3042-3045) vs JDK8-17 Array<u2> (3047-3118)
- `field_proxy::get()` — `vmhook/ext/vmhook/vmhook.hpp:11988-12049` — dispatches on JVM type descriptor; reads 4-byte int for rgb (12014-12019), always reads 4-byte compressed OOP for reference fields (12045-12048)
- `field_proxy::value_t::cast_for_variant<unique_ptr<T>>` — `vmhook/ext/vmhook/vmhook.hpp:11821-11849` — OOP->wrapper decode path; FLAW-B guard at 11833-11836 rejects non-'L' signatures, then decode_oop_pointer (11838), is_valid_pointer (11839), new wrapper (11843)
- `hotspot::decode_oop_pointer(uint32_t)` — `vmhook/ext/vmhook/vmhook.hpp:4288-4352` — reads CompressedOops/Universe narrow-oop base+shift via VMStructs (version-tolerant fallbacks 4296-4340), returns base + (compressed << shift) (4350-4351)
- `hotspot::is_valid_pointer(p)` — `vmhook/ext/vmhook/vmhook.hpp:1768-1805` — range+alignment+sentinel check gating enum-OOP derefs; user-address-range (1772), odd-address reject (1780), debug-fill sentinel (1789-1800); NOT a mapped-page probe
- `method_proxy::call()` — `vmhook/ext/vmhook/vmhook.hpp:13199-13380` — best-effort native brightness() dispatch; finds call_stub_entry (13215), ensures JavaThread (13218), routes to call_jni (13225) or call-stub; returns monostate on gate-miss; value_t::is_void() (12513-12516) tests availability

## Tests

- `tests/jvm/modules/enum_singleton.cpp`

## Known bugs

- **[high]** Reference-field read hard-codes compressed OOPs; -XX:-UseCompressedOops silently mis-decodes every enum singleton. field_proxy::get() reads reference field as 4-byte uint32_t (12045-12048) and cast_for_variant feeds it to decode_oop_pointer (11838, 4350-4351) unconditionally. When compressed oops are OFF (heaps ≳32 GB or explicit flag), field is 8-byte raw pointer: reading 4 bytes grabs half, base/shift mangles further. is_valid_pointer then rejects bogus address, acquire_constant returns nullptr, safe_rgb yields -1, tests FAIL. Feature genuinely does not support uncompressed-oops JVMs. Fix: branch on UseCompressedOops and read 8 bytes when OFF.
- **[medium]** is_valid_pointer is range/alignment/sentinel test, not mapped-page probe — decoded-but-unmapped enum OOP can fault rgb read. safe_rgb gates on is_valid_pointer (1768-1805) before get_rgb() but function never touches page; compressed value decoding to user range yet pointing at freed/never-mapped page passes gate and subsequent instance+offset load (14091) takes access violation. For live enum singletons strongly reachable for JVM lifetime never triggers (module is green) but safety claim in header is overstated: only is_readable_pointer/SEH-guarded read gives guarantee. Low real-world risk, medium because stated invariant false.
- **[low]** acquire_constant mistyped-name path indistinguishable from genuinely-absent-static failure. static_field → get_field(type_index, name) returns std::nullopt for both 'field not found' and 'field not static' (14123-14135); acquire_constant collapses to nullptr (enum_singleton.cpp:127-129) with no diagnostic. Typo in constant name degrades to silent *_resolves==false FAIL with no hint which cause fired. Not library bug per se but sharp edge for authors copying pattern.

## Notes

Field-record decode format split (vmhook.hpp:3015-3121): JDK 21+ uses _fieldinfo_stream (3042-3045); JDK 8–17 uses 6-slot Array<u2> _fields (3047-3118), with JDK 8 trailing _java_fields_count u2 tolerated via integer division (3065-3068, 3086). Compressed-oops VMStruct rename drift (decode_oop_pointer 4296-4340): base/shift live under Universe::_narrow_oop.* (JDK 8–16), CompressedOops::_narrow_oop.* (17–24), CompressedOops::_base/_shift (25+); all three probed; missing VMStruct returns nullptr, collapses every reference decode to clean FAIL (flaw #1). Compressed-oops ON/OFF (flaw #1): default sub-32 GB heap enables them; feature only works in that mode. Native-call gate variance (method_proxy::call 13199-13247): JDK ≤17 usually exposes StubRoutines::_call_stub_entry so call-stub path fires; JDK 21+/25 frequently absent, falls to JNI path (13225) needing ensure_current_java_thread(). brightness() can succeed on some JDKs even without hook (call() attempts OS-thread attach), so assertion best-effort: assert 0xFF when succeeds, [INFO]+soft-pass when gate unavailable. identityHashCode width: Java witnesses are int; collisions across three fresh singletons vanishingly unlikely; Java-side distinctness cross-check (enum_singleton.cpp:427) corroborates OOP-level distinctness rather than replacing. No compact-strings / MethodFlags sensitivity applies.
