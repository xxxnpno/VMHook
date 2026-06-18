---
slug: api_surface_no_jvm
title: API Surface (no-JVM contract)
category: infra
status: in_progress
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/in_progress, risk/medium, category/infra, tag/safety, tag/no-throw, tag/no-jvm, tag/compile-surface, tag/runtime-contract]
---

# API Surface (no-JVM contract)

> **Category:** [[categories/infra|Infrastructure (wrappers, traits, macros, logging)]]  ·  **Status:** `in_progress`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/api_surface_no_jvm-specialist.md`

## Description

Every public vmhook entry point must be callable with its documented signature,
return its safe-default (`nullptr` / `false` / `0` / empty string / inert handle),
invoke no visitor, and never throw or crash when no HotSpot JVM is loaded into the
process. This is the "library linked but JVM not attached/not yet attached" state —
the single most common way a consumer first exercises the API, and the contract
funnels through `os::find_jvm_module()` (null → VMStructs null → class lookup null).

## Referenced from

- [[features/platform_capability_macros|platform_capability_macros]]

## Implementation anchors

- `os::find_jvm_module()` — `vmhook/ext/vmhook/vmhook.hpp:543-566` — scans already-loaded modules for jvm.dll/libjvm.so/libjvm.dylib; returns nullptr if absent
- `hotspot::get_jvm_module()` — `vmhook/ext/vmhook/vmhook.hpp:1629-1634` — caches module-resolution result in static (first-call-wins); null propagates to VMStructs
- `os::get_proc_address(module, symbol)` — `vmhook/ext/vmhook/vmhook.hpp:571-582` — guarded !module || !symbol check; returns nullptr on null module
- `hotspot::get_vm_structs()` — `vmhook/ext/vmhook/vmhook.hpp:1665-1680` — caches VMStruct array via get_proc_address; null with no module
- `hotspot::get_vm_types()` — `vmhook/ext/vmhook/vmhook.hpp:1642-1657` — caches VMType array via get_proc_address; null with no module
- `iterate_struct_entries(name, field)` — `vmhook/ext/vmhook/vmhook.hpp:1711-1730` — walks VMStruct entries with null-propagating loop; returns nullptr base
- `iterate_type_entries(name)` — `vmhook/ext/vmhook/vmhook.hpp:1685-1700` — walks VMType entries; null base means loop never executes
- `find_class(class_name)` — `vmhook/ext/vmhook/vmhook.hpp:6321-6403` — HotSpot-internal klass resolution; wrapped try-catch returns nullptr on any VMStruct miss
- `class_loader_data_graph::get_head()` — `vmhook/ext/vmhook/vmhook.hpp:3433-3454` — calls iterate_struct_entries for _head; throws/returns nullptr with no JVM
- `class_loader_data_graph::find_klass()` — `vmhook/ext/vmhook/vmhook.hpp:3465-3473` — JDK-adaptive (_klasses vs _dictionary); no head to walk with no JVM
- `register_class<T>(class_name)` — `vmhook/ext/vmhook/vmhook.hpp:6915-6952` — calls find_class first; null → returns false before touching type_to_class_map
- `hook<T>(name, callback)` — `vmhook/ext/vmhook/vmhook.hpp:8011-8016` — thin overload; forwards to hook<T>(name, "", callback)
- `hook<T>(name, signature, callback)` — `vmhook/ext/vmhook/vmhook.hpp:8026-8301` — real install; unregistered type throws → caught → returns false
- `shutdown_hooks()` — `vmhook/ext/vmhook/vmhook.hpp:8600-8760` — idempotent teardown; noexcept-safe catch tails with no installed hooks
- `for_each_loaded_class(visitor)` — `vmhook/ext/vmhook/vmhook.hpp:6405-6410` — graph walk rooted in VMStructs; no VMStructs → zero entries → visitor never called
- `for_each_thread(visitor)` — `vmhook/ext/vmhook/vmhook.hpp:6425-6440` — thread-list walk; no VMStructs → zero entries → visitor never called
- `for_each_instance<T>(visitor, max_visits)` — `vmhook/ext/vmhook/vmhook.hpp:6780-6802` — unregistered type → returns 0; even if registered, find_class null → returns 0
- `make_unique<T>()` — `vmhook/ext/vmhook/vmhook.hpp:10867-10871` — find_class null → logs, returns nullptr before factory invoke
- `on_class_loaded(callback)` — `vmhook/ext/vmhook/vmhook.hpp:16500-16520` — resolves java/lang/ClassLoader via register_class+find_class; both null → inert handle
- `on_exception(callback)` — `vmhook/ext/vmhook/vmhook.hpp:16665-16685` — resolves java/lang/Throwable via register_class+find_class; both null → inert handle
- `watch_handle::running()` — `vmhook/ext/vmhook/vmhook.hpp:16410-16415` — default-constructed handle is inert (running()==false)
- `read_java_string(string_oop)` — `vmhook/ext/vmhook/vmhook.hpp:15723-15740` — guarded !string_oop || !is_valid_pointer check; returns empty string
- `is_valid_pointer(ptr)` — `vmhook/ext/vmhook/vmhook.hpp:1768-1783` — address-range + alignment + debug-poison filter; rejects low/high pointers

## Tests

- `tests/test_api_surface.cpp`
- `tests/test_api_surface_extended.cpp`

## Known bugs

- **[medium]** First-call-wins module caching (get_jvm_module static at 1629-1634, get_vm_structs/get_vm_types statics at 1665-1680/1642-1657) latches no-JVM verdict permanently — JVM attached later is never seen; real early-init consumers hit this. Fix: re-probe while null or expose reset_jvm_module_cache().
- **[low]** register_class<T> leaves no rollback for partially-applied re-registration (6938/6944 map writes), but no-JVM path is clean (find_class checked at 6919 before both writes).
- **[low]** hook<T> no-JVM result depends on unregistered-type throw (8037-8040 → 8296-8299 catch), not explicit JVM-absence guard — two coincidences deep, contract holds today but fragile.
- **[low]** read_java_string((void*)0x1) safety leans entirely on address-floor in is_valid_pointer (15726 → 1768-1783); committed-but-wrong-oop would pass range check and read from wrong location (15742-15762).

## Notes

No-JVM contract is JDK-independent (no JVM to vary), but absent symbols/structs produce safe-defaults uniformly across JDK 8..26. Module export names (gHotSpotVMStructs, gHotSpotVMTypes, jvm.dll/libjvm.so/libjvm.dylib) are stable; find_class graph strategy is JDK-adaptive (_klasses vs _dictionary selected at runtime, both probes null with no JVM); read_java_string String layout is JDK-versioned (compact-strings coder field JDK 9+) but moot on null-oop path. JNI fallback (jni_find_class_with_context_loader at 6377) also null with no JVM — context-loader resolution heuristics vary by JDK 8 but only when JVM attached.
