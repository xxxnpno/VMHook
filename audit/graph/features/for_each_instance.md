---
slug: for_each_instance
title: For Each Instance
category: enumeration
status: in_progress
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/in_progress, risk/medium, category/enumeration, tag/enumeration, tag/heap-scan, tag/live-instance, tag/safe_read, tag/narrow-klass, tag/conservative, tag/best-effort, tag/max-visits-cap, tag/no-safepoint]
---

# For Each Instance

> **Category:** [[categories/enumeration|Live-VM enumeration (heap / classes / threads)]]  ·  **Status:** `in_progress`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/for_each_instance-specialist.md`

## Description

Conservative, best-effort live-instance heap scan: `vmhook::for_each_instance<T>(visitor[, max_visits])`
resolves T's registered klass, walks the collected-heap reservation
(`Universe::_collectedHeap::_reserved`) linearly in 4 KiB chunks via `os::safe_read`, decodes each
candidate oop's narrow-klass pointer, and invokes the visitor with a fresh `std::unique_ptr<T>` for
every header whose klass matches T. Returns the number of instances reported and honours an optional
`max_visits` cap (re-checked in both the chunk loop and the inner stride loop). It is NOT a
GC-cooperative precise iteration: it runs without a safepoint, so every visit is a real object (no
false positives) but objects MAY be missed — the honest tally and no-false-positive guarantees are the
hard invariants, completeness is best-effort.

## Depends on

- [[features/klass_introspection|klass_introspection]]
- [[features/iterate_entries_safety|iterate_entries_safety]]

## Related

- [[features/for_each_loaded_class|for_each_loaded_class]]
- [[features/for_each_thread|for_each_thread]]

## Implementation anchors

- `vmhook::for_each_instance<T>(visitor, max_visits)` — `vmhook/ext/vmhook/vmhook.hpp:8549-8730` — entry point — resolves T's registered klass (type-not-registered guard at 8558), find_class null guard (8565), heap-VMStruct-missing guard (8587); chunked safe_read walk, +narrow-klass decode, visitor under is_valid_pointer; visitor exception caught + logged at 8723
- `type-not-registered guard` — `vmhook/ext/vmhook/vmhook.hpp:8558-8566` — returns 0 and never calls the visitor when register_class<T>() was not called first
- `heap reservation VMStruct gate` — `vmhook/ext/vmhook/vmhook.hpp:8587-8595` — bails (returns 0, logs) when Universe::_collectedHeap::_reserved cannot be resolved on this JDK
- `vmhook::os::safe_read` — `vmhook/ext/vmhook/vmhook.hpp:8600-8722` — every chunk + header read goes through safe_read so an unmapped region page (G1) defers rather than faulting

## Tests

- `tests/jvm/modules/for_each_instance.cpp`

## Known bugs

- **[medium]** Conservative raw-memory walk is NOT GC-cooperative — runs without a safepoint, so a concurrent GC may move/collect an object between the header read and the visitor call. On region-based collectors (G1) the reservation contains unmapped pages; on coloured-pointer collectors (ZGC/Shenandoah) the layout is undefined. Contract (documented in-header): every visit is a real object (no false positives) but some objects MAY be missed. Completeness is best-effort and must be recorded as [INFO], never hard-asserted — the legacy inline test FLAKED on exactly this point.
- **[low]** Per-element correctness depends on the +narrow-klass-offset decode being valid for the running JDK's oop header layout; a future header change would silently mis-match klass pointers (under-report, never over-report) without diagnostic.

## Notes

RELIABLE hard invariants the scan promises: visits > 0 when the heap holds pinned instances; returned
count == visitor-call count (honest tally); visits <= number actually allocated (no false positives);
max_visits cap honoured by BOTH the returned tally and the visitor-call count; max_visits == 0 yields 0
visits; unregistered T yields 0 and the visitor is never called; the scan terminates in bounded
wall-clock and never crashes the JVM; every wrapper handed to the visitor is non-null with a valid OOP.
Best-effort (record [INFO], never [FAIL]): how many pinned ids were actually seen, whether ALL were
found, whether a specific pinned instance was found. The fixture (vmhook/fixtures/ForEachInstance) uses
a private ctor + probe to guarantee exactly PIN_COUNT objects and a MARKER sentinel field to confirm a
matched header is genuinely ours. Pure enumeration module — no hooks installed, never calls
shutdown_hooks(). Every visitor deref gated by is_valid_pointer; every call passes a finite max_visits.
