---
slug: on_class_loaded
title: On Class Loaded
category: lifecycle
status: seeded
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/medium, category/lifecycle, tag/lifecycle, tag/hook, tag/callback, tag/event-driven]
---

# On Class Loaded

> **Category:** [[categories/lifecycle|Lifecycle hooks (shutdown / class-load / exception / enum)]]  ·  **Status:** `seeded`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/on_class_loaded-specialist.md`

## Description

vmhook::on_class_loaded(callback) — registers a callback fired whenever
java.lang.ClassLoader defines a new class.  It installs an interpreter hook on
ClassLoader::defineClass(String, byte[], int, int, ProtectionDomain) and
dispatches the callback with the internal (`/`-separated) class name read from
the call's first argument.  Event-driven: zero latency, zero idle cost, no
polling.  Returns a watch_handle whose on_stop erases the callback from the
shared list.  Limitations: only catches classes defined via the Java-side
defineClass entry (bootstrap-loaded java.*/javax.*/sun.* classes bypass it), and
the (String, ByteBuffer, ProtectionDomain) overload is not hooked.  Callback
exceptions are caught and logged (noexcept boundary); the callback runs on the
Java thread that triggered the definition.  After a shutdown_hooks() teardown the
next on_class_loaded() re-installs a live detour.

## Depends on

- [[features/hook_basic|hook_basic]]

## Related

- [[features/on_exception|on_exception]]
- [[features/shutdown_hooks_teardown|shutdown_hooks_teardown]]
- [[features/read_java_string|read_java_string]]

## Implementation anchors

- `vmhook::on_class_loaded` — `vmhook/ext/vmhook/vmhook.hpp:20973-21080` — installs ClassLoader.defineClass hook; dispatches internal class name
- `vmhook::detail::class_load_callbacks` — `vmhook/ext/vmhook/vmhook.hpp:20921-20939` — callback registry + class_load_hook_installed flag; class_loader_wrapper

## Tests

- `tests/jvm/modules/on_class_loaded.cpp`

## Notes

Requires a live JVM (arms a real interpreter hook on defineClass).  Shares the
detour re-install lifecycle with on_exception; a shutdown_hooks() drops the stale
callback list so the first post-shutdown register re-installs the detour.
