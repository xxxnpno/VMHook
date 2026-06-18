---
slug: field_static
title: Field Static
category: field
status: in_progress
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/in_progress, risk/medium, category/field, tag/field, tag/static, tag/mirror, tag/get, tag/set, tag/size-guard, tag/char-widening, tag/gc-stable-mirror, tag/putstatic]
---

# Field Static

> **Category:** [[categories/field|Field proxies (get / set / introspection)]]  ·  **Status:** `in_progress`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/field_static-specialist.md`

## Description

Portable static-field accessor `object<T>::static_field("name")` for both GET and SET across every JVM
primitive (Z B C S I J F D), `java.lang.String`, and object references. A static field lives on the
`java.lang.Class` mirror, not on an instance: the accessor resolves the field on the registered klass,
computes the address as the declaring-klass mirror + offset, and returns a `field_proxy` with
`is_static == true`. GET re-resolves the address through the GC-stable mirror at read time (so a relocated
class mirror never reads a stale slot) and reads the live slot after a runtime `putstatic`. SET runs the
size/type guard (refuses too-wide / mistyped / non-primitive writes into a primitive slot) plus the "C"
1-byte->2-byte char-widening shortcut; reference SET via `unique_ptr<wrapper>` rewrites the compressed OOP
and an empty unique_ptr nulls it. Every write is proven visible to Java via genuine getstatic/putstatic.

## Depends on

- [[features/field_introspection|field_introspection]]

## Related

- [[features/field_inherited|field_inherited]]
- [[features/field_null_safety|field_null_safety]]
- [[features/field_primitives_get|field_primitives_get]]
- [[features/field_proxy_value_t|field_proxy_value_t]]

## Depended on by

- [[features/enum_singleton|enum_singleton]]
- [[features/watch_static_field|watch_static_field]]

## Implementation anchors

- `object<derived>::static_field(name)` — `vmhook/ext/vmhook/vmhook.hpp:18788-18792` — portable factory — forwards to object_base::get_field(type_index{typeid(derived)}, name); returns a field_proxy with is_static == true. The GCC-safe accessor (deducing-this overloads are non-viable from a static context)
- `object_base::get_field(type_index, name) static resolver` — `vmhook/ext/vmhook/vmhook.hpp:17879-17960` — static-only variant — resolves field, requires entry->is_static, computes pointer as declaring-klass mirror (entry->declaring_klass ?: target_klass) + offset; does not require a live instance OOP
- `field_proxy::get() (GC-stable static re-resolve)` — `vmhook/ext/vmhook/vmhook.hpp:15316-15443` — for statics re-resolves read_pointer through mirror_klass->get_java_mirror() at read time; ignores stale init constants, reads the live mirror slot after putstatic
- `field_proxy::set(const value_type&) — guards + char widen` — `vmhook/ext/vmhook/vmhook.hpp:15465-15620` — non-primitive guard (refuses string/vector/unique_ptr into a primitive slot), 'C' 1-byte->2-byte widening shortcut (~15580), size-mismatch guard (~15602: field_size != 0 && value_size != field_size -> log + return, no memcpy)
- `detail::jvm_primitive_byte_width(string_view)` — `vmhook/ext/vmhook/vmhook.hpp:1623-1624` — width oracle both guards consult — 0 unless signature.size()==1 (Z/B=1, S/C=2, I/F=4, J/D=8)

## Tests

- `tests/jvm/modules/field_static.cpp`

## Known bugs

- **[medium]** Size guard is a SIZE guard, not a TYPE guard (vmhook.hpp:~15602): a same-width wrong-KIND static write (set(float) into 'I', set(int32) into 'F', set(double) into 'J') passes and memcpy's the raw bit pattern verbatim. Shared with field_primitives_set / field_proxy_set_guards; characterised, not fixed.
- **[medium]** set() returns void (no programmatic signal on a refused write — size mismatch / non-primitive / null only emit a log line). The static module works around this by re-reading through get() AND pulling each value back through a Java getter (getstatic) so a silently-dropped write is caught two ways.
- **[low]** Inherited-static addressing depends on entry->declaring_klass: a static offset is relative to the DECLARING class's mirror, not the requesting klass's (they differ for an inherited static with a differently-sized mirror oop). The resolver falls back to declaring_klass when set; a future find_field that drops declaring_klass would mis-address an inherited static.

## Notes

Static fields live on the java.lang.Class mirror on every supported JDK (8..26); find_field walks
InstanceKlass metadata uniformly (JDK 8-17 Array<u2> _fields vs JDK 21+ _fieldinfo_stream). GET re-resolves
the GC-stable mirror at read time so a G1-relocated mirror never reads a stale slot. The fixture
(vmhook/fixtures/FieldStatic) proves each write two ways: a mode-driven probe snapshots each field into a
seen* witness via real getstatic/putstatic, AND each value is pulled back through static_method("getX").
Java-8-only fixture (no var/records, (char)0x.. literals only); MSVC copy-init (std::string s = proxy->get(),
never brace-init); every wrapper deref off a decoded OOP gated by is_valid_pointer. Field module — no hooks
needed; if one is installed to satisfy a dispatch contract it is scoped_hook and disarms on scope exit;
never calls shutdown_hooks().
