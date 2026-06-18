---
slug: find_class_fallback
title: Find Class Fallback
category: klass
status: in_progress
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/in_progress, risk/medium, category/klass, tag/klass, tag/class-lookup, tag/name-cache, tag/class-loader-data-graph, tag/jni-fallback, tag/context-loader, tag/stale-cache, tag/internal-name]
---

# Find Class Fallback

> **Category:** [[categories/klass|Class / Klass introspection]]  ·  **Status:** `in_progress`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/find_class_fallback-specialist.md`

## Description

`vmhook::find_class(name)` and its resolution fallback chain — resolving a class by internal JVM name
('/'-separated) across two stages. First an empty-name fast-reject, then a name cache lookup
(`klass_lookup_cache`) with a stale-entry guard that re-validates a cached klass by round-tripping its
own name symbol and evicts on mismatch (RedefineClasses / unload safety). On a cache miss it runs the
HotSpot-internal `ClassLoaderDataGraph` / `SystemDictionary` graph walk (zero JNI); if that misses, it
falls back to `jni_find_class_with_context_loader` (thread context loader -> system loader -> Forge
LaunchWrapper). A successful result is cached and the first non-bootstrap klass is latched for
host-loader inheritance. A nonexistent class returns nullptr gracefully on both paths; repeated lookups
are stable / cached (same name -> identical klass*).

## Depends on

- [[features/vmstructs_offset_resolution|vmstructs_offset_resolution]]
- [[features/klass_introspection|klass_introspection]]

## Related

- [[features/compressed_klass_decode|compressed_klass_decode]]
- [[features/compressed_oops_decode|compressed_oops_decode]]
- [[features/constantpool_access|constantpool_access]]
- [[features/instanceklass_methods_walk|instanceklass_methods_walk]]
- [[features/interface_polymorphism|interface_polymorphism]]
- [[features/klass_introspection|klass_introspection]]
- [[features/find_class_context_loader|find_class_context_loader]]

## Depended on by

- [[features/classloader_reanchor|classloader_reanchor]]
- [[features/collection_linked_list|collection_linked_list]]
- [[features/enum_singleton|enum_singleton]]
- [[features/field_inherited|field_inherited]]
- [[features/field_introspection|field_introspection]]
- [[features/field_null_safety|field_null_safety]]
- [[features/find_class_context_loader|find_class_context_loader]]
- [[features/hook_basic|hook_basic]]
- [[features/make_java_array|make_java_array]]
- [[features/nested_classes|nested_classes]]
- [[features/register_class|register_class]]

## Implementation anchors

- `vmhook::find_class(name)` — `vmhook/ext/vmhook/vmhook.hpp:8004-8105` — real resolver — empty-name fast-reject (8019), cache lookup + stale-revalidate, graph walk (8073), JNI fallback on miss (8077), insert into klass_lookup_cache (8088), capture_host_classloader_klass (8099); catch-all returns nullptr
- `stale-cache revalidation guard` — `vmhook/ext/vmhook/vmhook.hpp:8048-8067` — cache hit returns cached_klass only if is_valid_pointer AND its name symbol round-trips to the requested name; otherwise erase + log + fresh graph walk (RedefineClasses / unload defence)
- `class_loader_data_graph::find_klass(name)` — `vmhook/ext/vmhook/vmhook.hpp:4462-4540` — O(N*M) full graph walk — per-CLD Dictionary find_klass (4496/4527) plus the SystemDictionary fallback; the HotSpot-internal zero-JNI stage
- `detail::jni_find_class_with_context_loader(name)` — `vmhook/ext/vmhook/vmhook.hpp:11951-12195` — JNI fallback engine — ensure_current_java_thread, then ClassLoader.loadClass via thread context loader -> system loader -> Forge Launch.classLoader; always jni_exception_clear()s; logs all-paths-failed at 12189
- `jni::find_class_with_context_loader(name)` — `vmhook/ext/vmhook/vmhook.hpp:13398-13402` — public thin forwarder to the JNI fallback engine — driven directly from inside a detour (app context loader) in the module
- `klass_lookup_cache + mutex` — `vmhook/ext/vmhook/vmhook.hpp:7987-7988` — process-wide name->klass* cache guarded by klass_lookup_cache_mutex; insert (not assign) so a racing same-name resolve isn't clobbered

## Tests

- `tests/jvm/modules/find_class_fallback.cpp`

## Audit docs

- `audit/findings/find_class_jni_fallback_chain.md`

## Known bugs

- **[low]** Empty-name fast-reject is load-bearing for safety, not just an optimisation (vmhook.hpp:8019): without it an empty name falls through to jni_find_class_with_context_loader, which calls ClassLoader.loadClass("") via JNI on the calling (possibly freshly-attached) thread — a far-less-travelled cold JDK 17+ path whose fault escapes the MinGW/clang try/catch (no SEH) and could tear the JVM down. Behaviour-preserving (the old code also returned nullptr) but now without the dangerous detour.
- **[low]** Stale-cache revalidation costs a name-symbol decode + string compare on every cache HIT (vmhook.hpp:8048-8060). Correct defence against a dangling klass after RedefineClasses / unload, but a hot loop resolving the same name pays the round-trip each call. A generation counter on klass_lookup_cache would avoid the per-hit decode.

## Notes

Names are INTERNAL form (slashed, no normalization). Graph-walk resolution is a pure HotSpot-internal
read so most module assertions call find_class straight from the worker thread (no Java thread needed);
the JNI fallback resolves through the CALLING thread's context loader, so its part runs inside a
scoped_hook detour on FindClassProbe.trigger() (the only place a Java thread with the app context loader
is guaranteed). Hard invariants: bootstrap classes resolve via the graph walk (java/lang/Object, String,
Integer, java/util/ArrayList, [I) and are proven usable (name round-trip, valid mirror); the app-loaded
fixture vmhook/fixtures/FindClassProbe resolves with its sentinel static readable through static_field +
getter; FindClassProbe$Inner resolves; [I and [Ljava/lang/String; resolve; a nonexistent class returns
nullptr (no crash) on both the direct call and the JNI fallback; repeated lookups yield the identical
klass* (cache contract). Per-JDK note: platform-vs-application loader split (JDK 9+ module system) means
a jdk.* class may resolve via system-loader delegation on 9+ but not 8 — use java/lang/* and app classes
for hard asserts, jdk.* as [INFO]. Every klass deref gated by is_valid_pointer; never calls
shutdown_hooks(); Java-8-only fixture; MSVC copy-init.
