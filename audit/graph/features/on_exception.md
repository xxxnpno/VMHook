---
slug: on_exception
title: On Exception
category: lifecycle
status: seeded
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/seeded, risk/medium, category/lifecycle, tag/lifecycle, tag/hook, tag/callback, tag/event-driven]
---

# On Exception

> **Category:** [[categories/lifecycle|Lifecycle hooks (shutdown / class-load / exception / enum)]]  ·  **Status:** `seeded`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/on_exception-specialist.md`

## Description

vmhook::on_exception(callback) — registers a callback fired whenever a
java.lang.Throwable (or any subclass) is constructed.  It installs an interpreter
hook on Throwable::fillInStackTrace()Ljava/lang/Throwable;, which every public
Throwable constructor calls before returning; when the hook fires it reads the
dynamic klass off the receiver oop's narrow-klass header and dispatches the
callback with the fully-qualified internal (`/`-separated) class name (e.g.
"java/lang/NullPointerException").  Event-driven: zero polling, zero idle cost.
Returns a watch_handle whose on_stop erases the callback.  Misses exceptions
constructed with writableStackTrace=false and subclasses that override
fillInStackTrace to a no-op (rare, some preallocated VM errors).  Callback
exceptions are caught and logged (noexcept boundary); the callback runs on the
Java thread that constructed the throwable.

## Depends on

- [[features/hook_basic|hook_basic]]

## Related

- [[features/on_class_loaded|on_class_loaded]]
- [[features/shutdown_hooks_teardown|shutdown_hooks_teardown]]
- [[features/klass_introspection|klass_introspection]]

## Depended on by

- [[features/method_throwing_call_site|method_throwing_call_site]]

## Implementation anchors

- `vmhook::on_exception` — `vmhook/ext/vmhook/vmhook.hpp:21139-21252` — installs Throwable.fillInStackTrace hook; dispatches throwable klass name
- `vmhook::detail::exception_callbacks` — `vmhook/ext/vmhook/vmhook.hpp:21088-21103` — callback registry + exception_hook_installed flag; throwable_wrapper

## Tests

- `tests/jvm/modules/on_exception.cpp`

## Notes

Requires a live JVM (arms a real interpreter hook on fillInStackTrace).  Shares
the detour re-install lifecycle with on_class_loaded; a shutdown_hooks() drops the
stale callback list so the first post-shutdown register re-installs the detour.
