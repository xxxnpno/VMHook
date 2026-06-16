---
slug: field_inherited
title: Field Inherited
category: field
status: in_progress
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/in_progress, risk/medium, category/field, tag/inheritance, tag/shadowing, tag/static-fields, tag/access-flags-ignored]
---

# Field Inherited

> **Category:** [[categories/field|Field proxies (get / set / introspection)]]  ·  **Status:** `in_progress`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/field_inherited-specialist.md`

## Description

Inherited and shadowed field access through the superclass chain via vmhook::find_field's
Klass::get_super() walk. Lookup starts at the requesting wrapper's klass and walks up the
superclass hierarchy, with the first klass declaring the name winning (child fields shadow
ancestor fields). Both instance and static fields resolve through the same walk; access
flags (private/protected/package) are ignored, enabling reads of ancestor private fields
inaccessible to Java source. Shadowing is correctly determined by which wrapper type initiates
the walk, not the live OOP header.

## Depends on

- [[features/field_introspection|field_introspection]]
- [[features/klass_introspection|klass_introspection]]
- [[features/decode_oop_and_pointers|decode_oop_and_pointers]]
- [[features/find_class_fallback|find_class_fallback]]

## Related

- [[features/field_null_safety|field_null_safety]]
- [[features/field_primitives_get|field_primitives_get]]
- [[features/field_proxy_value_t|field_proxy_value_t]]
- [[features/field_static|field_static]]

## Depended on by

- [[features/poly_inherited_oop|poly_inherited_oop]]

## Implementation anchors

- `vmhook::find_field(klass* target_klass, string_view name)` — `vmhook/ext/vmhook/vmhook.hpp:10728-10769` — super-chain walker; loop at 10756, per-klass call at 10758, cache insert at 10762, not-found at 10767-10768
- `klass::get_super()` — `vmhook/ext/vmhook/vmhook.hpp:2707-2719` — returns Klass::_super from VM struct; nullptr for Object (terminator) or invalid pointer
- `klass::find_field(name)` — `vmhook/ext/vmhook/vmhook.hpp:2953-3059` — searches only fields declared on this klass (no walk); with JDK-21+ find_field_in_stream variant at 2841-2933
- `find_field_in_stream` — `vmhook/ext/vmhook/vmhook.hpp:2841-2933` — JDK-21+ FieldInfoStream variant; selected at 2980-2983 by probing _fieldinfo_stream existence
- `g_field_cache` — `vmhook/ext/vmhook/vmhook.hpp:10708-10709` — unordered_map<klass*, unordered_map<string, field_entry_t>>; keyed by requesting target_klass, guarded by g_field_cache_mutex
- `object_base::get_field(name)` — `vmhook/ext/vmhook/vmhook.hpp:10792-13529` — caller that funnels through vmhook::find_field for inherited-walk
- `object_base::resolve_klass()` — `vmhook/ext/vmhook/vmhook.hpp:13847-13884` — keys off C++ wrapper's static type (typeid(*this)) to determine chain start-point; uses type_to_class_map -> find_class, not live OOP header

## Tests

- `tests/jvm/modules/field_inherited.cpp`

## Known bugs

- **[medium]** Doc comment contradicts implementation (10713-10714, 10718-10719) — says 'Only the declaring class is searched - not superclasses' and 'full InstanceKlass._fields array is walked', yet code walks get_super() at 10756 and not-found log reads 'field not found in class hierarchy'. Users hand-roll super-walks for a feature already working.
- **[low]** No cycle/depth guard on super-walk (10756) while all sibling walkers have caps — for_each_thread caps at 4096, find_class at <65536/<1048576, dictionary chain at <1048576. Corrupt/looping _super (teardown garbage or malicious RedefineClasses) spins forever; real hierarchies ~12 deep, 256 cap harmless.
- **[low]** Negative results never cached (10767-10768 returns nullopt without inserting) — per-tick get_field('typo') re-walks child→mid→base→Object, re-parses _fields/_fieldinfo_stream, emits error_tag log every frame. find_class short-circuits misses via klass_lookup_cache; fields do not.
- **[low]** Shadowing duplicates offsets across descendants in cache (key=target_klass,name at 10762) — 50 subclasses looking up inherited Object.hash store 50 identical inner entries; every new-subclass miss grabs g_field_cache_mutex. Memory waste + contention point, not a correctness bug.
- **[low]** field_proxy cannot tell inherited from own (2510-2515 drops declaring klass and 16-bit access flags, keeping only is_static) — no declaring_klass() / is_inherited() / is_protected() / is_private() methods; access-level-independence proven behaviourally not via proxy introspection.

## Notes

Field metadata layout splits across JDK versions: 8–17 use InstanceKlass._fields (Array<u2> of 6-slot FieldInfo);
JDK 21+ use _fieldinfo_stream (UNSIGNED5-encoded). The super-walk is layout-agnostic (calls per-klass find_field);
inheritance/shadowing assertions hold on both. Compressed oops/klass assumed (HotSpot default) — String shadow and
reference inherited fields exercise decode_oop_pointer. Walk terminates at java.lang.Object (get_super()==nullptr),
stable across all supported JDKs.
