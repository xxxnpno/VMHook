---
slug: for_each_thread
title: For Each Thread
category: enumeration
status: in_progress
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/in_progress, risk/medium, category/enumeration, tag/enumeration, tag/java-thread, tag/thread-list, tag/threads-smr, tag/dedup, tag/osthread, tag/runaway-cap, tag/is-valid-pointer]
---

# For Each Thread

> **Category:** [[categories/enumeration|Live-VM enumeration (heap / classes / threads)]]  ·  **Status:** `in_progress`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/for_each_thread-specialist.md`

## Description

Live HotSpot `JavaThread` enumeration: `vmhook::for_each_thread(visitor)` walks the VM thread list and
hands the visitor a `thread_info{ JavaThread*, state, os_thread_id }` snapshot per thread. It tries, in
order of availability, the classic intrusive `Threads::_thread_list` chain (JDK 8-9 and later builds
that still ship the VMStruct), de-duplicated through an `unordered_set` and hard-capped at 4096 entries,
and the JDK 10+ Safe-Memory-Reclamation `ThreadsList` snapshot
(`ThreadsSMRSupport::_java_thread_list`) iterated `[0, _length)`. Every `JavaThread*` is validated by
`is_valid_pointer` before the visitor runs, the OSThread chain is decoded for `os_thread_id`, and the
walk terminates under a wall-clock bound. There is no thread name in `thread_info` — callers track a
specific thread by live-count / pointer-set delta, not by name.

## Depends on

- [[features/iterate_entries_safety|iterate_entries_safety]]

## Related

- [[features/for_each_instance|for_each_instance]]
- [[features/for_each_loaded_class|for_each_loaded_class]]

## Implementation anchors

- `vmhook::for_each_thread(visitor)` — `vmhook/ext/vmhook/vmhook.hpp:8419-8520` — entry point — invoke_visitor builds thread_info, gating each JavaThread* through is_valid_pointer (8423); Path 1 Threads::_thread_list dedup chain + 4096 cap, Path 2 ThreadsSMRSupport::_java_thread_list [0,_length) snapshot
- `thread_info struct` — `vmhook/ext/vmhook/vmhook.hpp:8378-8412` — visitor payload { JavaThread*, thread state, os_thread_id } — no name field, by design
- `is_valid_pointer` — `vmhook/ext/vmhook/vmhook.hpp:1768-1805` — range/alignment/sentinel gate every enumerated JavaThread* passes before reaching the visitor

## Tests

- `tests/jvm/modules/for_each_thread.cpp`

## Known bugs

- **[low]** Path 2 (ThreadsSMRSupport::_java_thread_list snapshot) does NOT de-duplicate, unlike Path 1 which dedupes through an unordered_set. On a healthy JVM a single source is used and no duplicate appears, but the asymmetry means a duplicate JavaThread* on Path 2 would surface unfiltered. Characterised by asserting no JavaThread* is reported twice in a single enumeration.
- **[low]** Legacy Path-1 cycle hazard: the intrusive _thread_list chain walk relies on the 4096 runaway cap to terminate; a corrupt/looping chain would otherwise spin. Cannot be forged on a live JVM without corrupting it, so the termination guarantee is characterised empirically (bounded count + bounded wall-clock + no duplicate pointer observed).

## Notes

Hard invariants: every enumerated JavaThread* is non-null and passes is_valid_pointer; the walk
terminates under a wall-clock bound; visit count is >= 1 and strictly < the 4096 cap; no JavaThread* is
reported twice; every os_thread_id is non-zero (OSThread chain decoded); repeated enumeration in a
quiescent window is stable (same count, same pointer set, current thread present both times); a freshly
spawned, parked, named Java thread is observed (live count rises by >=1, a brand-new valid JavaThread*
appears) and disappears once released. Recorded as [INFO]: thread name correlation (not exposed in
thread_info). Pure enumeration module — needs no JavaThread, installs no hooks, never calls
shutdown_hooks(); the module never forces a cycle, never mutates the thread list, and bounds every poll
loop. Fixture vmhook/fixtures/ForEachThread drives the worker lifecycle through the go/done + mode probe.
