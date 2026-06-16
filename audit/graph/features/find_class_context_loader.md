---
slug: find_class_context_loader
title: Find Class Context Loader
category: klass
status: in_progress
risk: high
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/in_progress, risk/high, category/klass, tag/classloader, tag/context, tag/bootstrap, tag/host-inheritance, tag/cache-override, tag/anchor, tag/JNI]
---

# Find Class Context Loader

> **Category:** [[categories/klass|Class / Klass introspection]]  ·  **Status:** `in_progress`  ·  **Risk:** `high`  ·  **Specialist:** `.claude/agents/find_class_context_loader-specialist.md`

## Description

Resolves Java classes through a specific classloader — the calling thread's context loader,
a live anchor object's loader, or a pinned override — rather than walking the HotSpot
ClassLoaderDataGraph by name alone. This enables app/Forge/Lunar classes the bare graph walk
misses to be reached, and allows the whole SDK to be re-pointed at one loader's copy. Also owns
the bootstrap-vs-context loader axis and host-classloader capture/inheritance machinery that
publishes the first non-bootstrap app klass to worker threads for context inheritance.

## Depends on

- [[features/find_class_fallback|find_class_fallback]]
- [[features/klass_introspection|klass_introspection]]
- [[features/decode_oop_and_pointers|decode_oop_and_pointers]]
- [[features/jni_local_ref_hygiene|jni_local_ref_hygiene]]

## Related

- [[features/classloader_reanchor|classloader_reanchor]]
- [[features/register_class|register_class]]
- [[features/hook_basic|hook_basic]]
- [[features/for_each_loaded_class|for_each_loaded_class]]

## Implementation anchors

- `vmhook::jni::find_class_with_context_loader(name)` — `vmhook/ext/vmhook/vmhook.hpp:10493-10497` — Public JNI context-loader resolver — thin forwarder to jni_find_class_with_context_loader
- `vmhook::detail::jni_find_class_with_context_loader(name)` — `vmhook/ext/vmhook/vmhook.hpp:9534-9665` — Real resolver engine with local_ref_bag RAII (9553-9571), load_with_loader lambda (9573-9611), three loader attempts in order: thread context, system, Minecraft Launch.classLoader
- `local_ref_bag` — `vmhook/ext/vmhook/vmhook.hpp:9553-9571` — RAII container that DeleteLocalRef's every JNI handle on return — fixes 4-8-local-ref-per-call leak
- `load_with_loader(class_loader) lambda` — `vmhook/ext/vmhook/vmhook.hpp:9573-9611` — Inner lambda: finds ClassLoader.loadClass(String):Class, builds dotted name via std::replace('/',''), calls loadClass, converts mirror to Klass*, always jni_exception_clear()'s
- `Thread.currentThread().getContextClassLoader() attempt` — `vmhook/ext/vmhook/vmhook.hpp:9613-9628` — First loader attempt: thread context loader — reaches app classes on real app threads
- `ClassLoader.getSystemClassLoader() attempt` — `vmhook/ext/vmhook/vmhook.hpp:9630-9640` — Second loader attempt: system/platform loader fallback
- `Launch.classLoader static field attempt (Minecraft LegacyLauncher)` — `vmhook/ext/vmhook/vmhook.hpp:9642-9658` — Third loader attempt: Minecraft Launch.classLoader static field — only on Forge ≤1.12-style stacks
- `find_class integration — fallback + cache insert + host capture` — `vmhook/ext/vmhook/vmhook.hpp:6377-6395` — Graph walk falls back to context resolver on miss, inserts result into klass_lookup_cache, calls capture_host_classloader_klass(found_klass)
- `klass_lookup_cache shared mutex` — `vmhook/ext/vmhook/vmhook.hpp:6304-6305` — Shared cache + mutex protecting all override/evict/insert operations
- `Stale-cache validation path` — `vmhook/ext/vmhook/vmhook.hpp:6348-6367` — Cache-hit guard: if (cached_klass && is_valid_pointer(cached_klass)) return it; else erase + fresh graph walk
- `vmhook::find_class_via_oop(anchor_oop, name)` — `vmhook/ext/vmhook/vmhook.hpp:10647-10728` — Anchor-loader resolver: guards null anchor/no-Java-thread (10650), wraps anchor as fake JNI handle via jni_oop_handle (10656), GetObjectClass chain, loadClass(dotted name), converts mirror to Klass*
- `jni_oop_handle(oop) fake-handle wrapper` — `vmhook/ext/vmhook/vmhook.hpp:9276-9281` — HAZARD: stashes raw oop in caller storage, returns &storage — GC-move hazard if oop not rooted in real handle
- `vmhook::override_class_lookup(name, k)` — `vmhook/ext/vmhook/vmhook.hpp:10742-10753` — Pin a name to a klass (or nullptr for negative entry) in cache under mutex — FLAW #1: null override is silently evicted on next find_class
- `vmhook::evict_class_lookup(name)` — `vmhook/ext/vmhook/vmhook.hpp:10760-10771` — Erase a name from cache under mutex — safe on absent names
- `vmhook::reanchor_classes_via_oop(anchor, {names...})` — `vmhook/ext/vmhook/vmhook.hpp:10785-10807` — For each name: find_class_via_oop then override_class_lookup on success; returns true only if EVERY name resolved — FLAW #3: partial success poisons cache with no rollback
- `vmhook::detail::klass_to_class_loader_oop(k)` — `vmhook/ext/vmhook/vmhook.hpp:9713-9733` — Bootstrap-vs-context axis: Klass._class_loader_data -> ClassLoaderData -> get_class_loader_oop(); returns null for bootstrap loader
- `class_loader_data::get_class_loader_oop() JDK-8-9-vs-10+ branching` — `vmhook/ext/vmhook/vmhook.hpp:3253-3283` — Handles JDK 8-9 direct oop vs JDK 10+ OopHandle layout (type_string == 'OopHandle', 3264-3265); double-dereference without no-GC guard (FLAW #6)
- `vmhook::detail::host_classloader_klass atomic` — `vmhook/ext/vmhook/vmhook.hpp:9701-9701` — Process-wide atomic holding first non-bootstrap app klass for context inheritance — published by capture, read by inherit_*
- `capture_host_classloader_klass(candidate)` — `vmhook/ext/vmhook/vmhook.hpp:9742-9768` — CAS-publishes first non-bootstrap klass, immediately self-applies via inherit_host_context_classloader_for_current_thread — FLAW #5: bootstrap-loaded app classes never publish
- `inherit_host_context_classloader_for_current_thread()` — `vmhook/ext/vmhook/vmhook.hpp:9784-9850` — Sets Thread.currentThread().setContextClassLoader(host loader) using fake JNI handle (9831-9833) — FLAW #2: raw loader_oop GC-move hazard

## Tests

- `tests/jvm/modules/find_class_context_loader.cpp`

## Known bugs

- **[high]** override_class_lookup(name, nullptr) does NOT seed a working negative entry — doc claims it does (10737-10738), but find_class's cache-hit guard at 6348 requires non-null && is_valid_pointer(cached_klass), so cached nullptr fails and falls through to erase+fresh-walk (6361-6367). Null override is silently evicted on next find_class, making the negative-cache feature a no-op. Either the doc/test is wrong or the cache-hit path must honor a sentinel. This is the single most important correctness defect.
- **[high]** find_class_via_oop(anchor_oop, name) / inherit_host_context_classloader_for_current_thread pass raw oops through JNI as fake handles (jni_oop_handle 9276-9281) — GC-move hazard. If a GC moves the object between the caller obtaining the oop and HotSpot reading it, the read is stale/garbage. The .wip module dodges this by passing self->get_instance() inside a detour (frame keeps it live), but the public API makes no such guarantee. A caller who stores an oop and later passes it to find_class_via_oop can crash.
- **[medium]** reanchor_classes_via_oop poisons the shared cache on partial success with no rollback (10793-10806). On a mixed list where some names resolve and some don't, it overrides the resolvers but returns false. The caller naturally retries the whole list, but the already-overridden names are now pinned to the anchor loader's copy and a retry against a different anchor silently leaves the first anchor's klasses cached. No all-or-nothing semantic, no rollback, persistent process-wide poisoning.
- **[medium]** find_class_with_context_loader always converts the name to dotted form for loadClass (9595-9607, identically in find_class_via_oop 10705-10707) but performs zero name validation. Empty string, leading/trailing slash, array descriptor [Ljava/lang/String;, primitive name int are all std::replace'd and handed to loadClass. Exceptions are swallowed by jni_exception_clear (correct), but the empty-name and array cases are untested. A dotted input (the .wip E6 does, line 375) double-no-ops the replace, creating an asymmetry with the '/'-keyed find_class cache.
- **[medium]** Bootstrap-loaded app classes are invisible to the capture machinery (9753-9756), defeating host-context inheritance on -Xbootclasspath/a setups. If the host app's classes are appended to the boot classpath, every app class reads as bootstrap (null loader) and host_classloader_klass is never published — worker threads never inherit a useful context loader and the whole inheritance feature is silently inert. No diagnostic is emitted.
- **[low]** get_class_loader_oop OopHandle path (3270-3278) reads the storage slot then reads the oop out of it — both safe_read_pointer-guarded against AV, but yields a moveable oop treated as stable. For comparison-only uses (bootstrap null vs app non-null) the value identity can shift under GC; null/non-null verdict is stable but pointer-equality assertions across a GC are not sound.
- **[low]** First-attempt local refs are tracked in local_ref_bag but intermediate *_id/field_id values are not — by design (not JNI handles), but the asymmetry is fragile. A future JVM where GetMethodID returns a managed handle would leak. Documented as a maintenance hazard, not a current bug.

## Notes

JDK-version sensitivities: (1) ClassLoaderData._class_loader layout — JDK 8-9 store direct oop, JDK 10+ store OopHandle (oop*); get_class_loader_oop branches on type_string=="OopHandle" (3264-3265, 3270-3279); a future JDK that renames or removes this VMStruct silently breaks bootstrap-vs-context comparison, capture, and inheritance. Verify on 8, 11, 17, 21, 25, 26. (2) String shape — pre-9 has value:char[] no coder; 9+ adds coder:byte. The .wip uses coder presence as generation marker but asserts value field on all versions; keep value as universal probe. (3) Thread.getContextClassLoader/setContextClassLoader — present on all JDKs but on freshly-attached worker the context loader is system/platform not app — the entire reason this feature exists. App-class resolution MUST be driven from real app thread (a detour), not off-thread. (4) Launch.classLoader — only exists on LegacyLauncher/Forge ≤1.12; on stock JDK the path is correctly skipped. (5) Platform-vs-application loader split (JDK 9+ module system) — getSystemClassLoader() returns app loader, platform loader sits between it and bootstrap; a jdk.* class may resolve via system-loader delegation on 9+ but not on 8; use java/lang/* and app classes for hard asserts, jdk.* as [INFO] only.
