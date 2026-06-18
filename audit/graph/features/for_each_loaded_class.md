---
slug: for_each_loaded_class
title: For Each Loaded Class
category: enumeration
status: in_progress
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/in_progress, risk/medium, category/enumeration, tag/enumeration, tag/class-loader-data-graph, tag/snapshot, tag/internal-name, tag/system-dictionary, tag/symbol-decode, tag/safety-caps]
---

# For Each Loaded Class

> **Category:** [[categories/enumeration|Live-VM enumeration (heap / classes / threads)]]  ·  **Status:** `in_progress`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/for_each_loaded_class-specialist.md`

## Description

Snapshot enumeration of every Java class currently reachable through the global
`ClassLoaderDataGraph`: `vmhook::for_each_loaded_class(visitor)` invokes
`visitor(const std::string& internal_name, vmhook::hotspot::klass* k)` once per Klass, where the name
uses JVM '/'-separated internal form. The walk strategy is per-JDK — JDK 21+ uses each
`ClassLoaderData::_klasses` list; JDK 8-17 uses the per-CLD `Dictionary` hashtables plus the
`SystemDictionary` `_dictionary` / `_shared_dictionary` fallback — and is hard-capped
(1M-per-CLD / 64K-CLD) so a corrupt graph cannot run away. It descends past the bootstrap loader into
application loaders, decodes each Klass's name symbol, and validates each klass pointer before handing
it to the visitor. Classes loaded after the call returns are not visited (it is a snapshot).

## Depends on

- [[features/iterate_entries_safety|iterate_entries_safety]]
- [[features/klass_introspection|klass_introspection]]

## Related

- [[features/for_each_instance|for_each_instance]]
- [[features/for_each_thread|for_each_thread]]

## Depended on by

- [[features/deoptimize_methods|deoptimize_methods]]

## Implementation anchors

- `vmhook::for_each_loaded_class(visitor)` — `vmhook/ext/vmhook/vmhook.hpp:8145-8160` — entry point — constructs class_loader_data_graph, walks it invoking visitor(name, klass*); top-level try/catch logs and swallows at 8154 so a bad walk never throws to the caller
- `class_loader_data_graph` — `vmhook/ext/vmhook/vmhook.hpp:4420-4520` — HotSpot ClassLoaderDataGraph::_head wrapper — global registry of all classloaders; per-JDK klass-list vs Dictionary strategy lives in its for_each_klass
- `klass::get_name() symbol decode` — `vmhook/ext/vmhook/vmhook.hpp:2286-2362` — decodes each Klass's name Symbol to the '/'-separated internal name handed to the visitor; guarded by is_valid_pointer

## Tests

- `tests/jvm/modules/for_each_loaded_class.cpp`

## Known bugs

- **[low]** Per-JDK walk strategy split (JDK 21+ ClassLoaderData::_klasses vs JDK 8-17 Dictionary hashtables + SystemDictionary _dictionary/_shared_dictionary fallback): a future JDK that renames or restructures the graph VMStructs could silently miss a loader (under-enumerate) with no diagnostic. Characterised by asserting universal bootstrap classes (java/lang/Object, String, Class, Integer, Thread) AND the app-loaded fixture are present on every supported JDK.
- **[low]** Snapshot semantics: classes loaded after the call returns are NOT visited (documented in-header ~8115). A caller relying on for_each_loaded_class for a live registry will miss late-loaded classes; this is by design, not a bug, but is a sharp edge for callers.

## Notes

All assertions are PORTABLE across the JDK matrix — never an exact count or exact class set (the
loaded-class universe differs wildly between JDK 8 and 21+, CDS on/off). Hard invariants: the snapshot
is non-trivial (count > 100 liveness floor); universal bootstrap classes present; the app-loaded fixture
vmhook/fixtures/ForEachLoadedClass (Class.forName'd at startup) appears (proof the walk descends past
bootstrap); every klass* passes is_valid_pointer; for the own fixture the supplied klass*'s get_name()
round-trips to the very name the visitor was handed; no name is empty or malformed (no leading '/', no
NUL, no whitespace); the walk terminates well below the 1M-per-CLD / 64K-CLD caps; a second enumeration
agrees on the bootstrap + fixture set. Pure enumeration module — no hooks installed, never calls
shutdown_hooks(); every klass deref gated by is_valid_pointer before get_name().
