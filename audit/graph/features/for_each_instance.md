---
slug: for_each_instance
title: For Each Instance
category: enumeration
status: in_progress
risk: high
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/in_progress, risk/high, category/enumeration, tag/heap-walk, tag/gc, tag/conservative, tag/safepoint, tag/enumeration, tag/safe-read, tag/klass-filter]
---

# For Each Instance

> **Category:** [[categories/enumeration|Live-VM enumeration (heap / classes / threads)]]  ·  **Status:** `in_progress`  ·  **Risk:** `high`  ·  **Specialist:** `.claude/agents/for_each_instance-specialist.md`

## Description

Template feature `for_each_instance<T>(visitor, max_visits)` performs a conservative,
best-effort raw-memory walk of the collected-heap reservation
(`Universe::_collectedHeap::_reserved`) to enumerate every live Java object whose
narrow klass pointer (at OOP offset +8) decodes to the klass registered for the C++
wrapper type T.  For each match the visitor receives a freshly-allocated
`std::unique_ptr<T>` wrapping the live OOP; the return value is the honest count of
visitor invocations.  An optional `max_visits` cap short-circuits both the chunk loop
and the inner stride loop so the scan terminates early once enough instances are found.
The scan is intentionally NOT GC-cooperative: no safepoint is taken, so a concurrent
GC may move or collect objects between when a header is read and when the visitor
accesses fields through the wrapper.  On region-based collectors (G1) unmapped
reservation pages are skipped via `safe_read` (best-effort: every visit is correct,
but some objects may be missed).  Coloured-pointer collectors (ZGC/Shenandoah) are
explicitly unsupported; constructor-based tracking is the safe alternative there.
Callers must invoke `register_class<T>()` before the scan; an unregistered type
returns 0 immediately with no visitor call.

## Depends on

- [[features/vmstructs_offset_resolution|vmstructs_offset_resolution]]
- [[features/decode_oop_and_pointers|decode_oop_and_pointers]]
- [[features/klass_introspection|klass_introspection]]
- [[features/register_class|register_class]]
- [[features/compressed_klass_decode|compressed_klass_decode]]

## Implementation anchors

- `vmhook::for_each_instance<T>(visitor, max_visits)` — `vmhook/ext/vmhook/vmhook.hpp:8508-8691` — main entry point: type-not-registered guard, VMStruct heap-bounds resolution, 64 GiB clamp, chunked 4 KiB safe_read walk, 8-byte stride klass-match, unique_ptr<T> visitor dispatch, honest visit tally
- `vmhook::hotspot::decode_klass_pointer(compressed)` — `vmhook/ext/vmhook/vmhook.hpp:5428-5465` — decodes the 32-bit narrow klass at OOP+8 via CompressedKlassPointers base/shift (JDK 8-16 Universe path / JDK 17-24 / JDK 25+ field name variants); returns nullptr on zero or unresolved VMStruct entries
- `vmhook::hotspot::iterate_struct_entries (Universe/_collectedHeap, CollectedHeap/_reserved, MemRegion/_start/_word_size)` — `vmhook/ext/vmhook/vmhook.hpp:8534-8550` — four VMStruct lookups cached in statics on first call; all four must be non-null or the scan returns 0 with no walk
- `chunk safe_read loop + stride inner loop (max_visits cap re-checks)` — `vmhook/ext/vmhook/vmhook.hpp:8636-8688` — outer for-loop checks `visits < max_visits` before each chunk; inner loop re-checks `visits < max_visits` per stride; both guards ensure the cap is honoured even if matches cluster at a chunk boundary

## Tests

- `tests/jvm/modules/for_each_instance.cpp`

## Known bugs

- **[high]** #38 — MSVC-ABI (windows-msvc + windows-clang-cl) GC/safepoint HANG: under suite heap pressure HotSpot requests a safepoint; the injected thread executing the raw chunk walk never reaches a safepoint poll, so the whole JVM freezes. Symptom is 'java.exe did not exit within 120 s' (a stall, not a crash or [FAIL]). Hang location moves across re-runs (not a deterministic code bug). Real fix: transition the injected thread to _thread_in_native around the walk (deep, CI-only repro; no local reproduction possible). mingw/linux/macos are not affected. Blocks sustained test expansion — every added module grows heap pressure and pushes no-SEH cells toward the stall threshold.
- **[medium]** TLAB allocation mid-walk: if the JVM performs a TLAB allocation into the reservation region while the scanner is partway through a 4 KiB chunk, newly-written object headers may be half-initialised (mark word written, narrow klass not yet written, or vice versa). A zero narrow klass is silently skipped (correct), but a coincidentally non-zero garbage narrow klass that happens to equal target_klass would be a false positive. Conservative scan contract says every visit is correct; this edge breaks that guarantee on the write path. No safepoint means no way to prevent concurrent allocation.
- **[medium]** Cross-JDK heap-region layout differences (G1 on JDK 17+ vs Serial/Parallel on JDK 8): the reservation can include humongous-region gaps and uncommitted regions that safe_read silently skips. Objects spanning a region boundary that is also a 4 KiB chunk boundary could have their OOP+8 klass word split across chunks (first 4 bytes in chunk N, second 4 bytes in chunk N+1). The inner loop reads `buffer + off + 8` as a uint32 entirely within one chunk (guarded by `off + 12 <= to_read`), so the split case is skipped rather than falsely matched — no crash, but the instance is missed. vmhook.hpp:8648.
- **[low]** No safepoint / not GC-stable: wrappers handed to the visitor carry an OOP that may have been moved by a concurrent compacting GC (G1 full-GC, ZGC relocation) between the safe_read into the buffer and the visitor call. Field reads through the wrapper go via safe_read_fast, so they won't fault, but they may read stale/zeroed memory at the old address. Doc-comment at vmhook.hpp:8483-8486 warns 'don't hold the wrappers past the return', but there is no enforcement — a caller that stores the unique_ptr and reads it later sees silent stale data.
- **[low]** 64 GiB scan-size clamp (vmhook.hpp:8601-8606) is correct for current CI heaps but is a static compile-time constant. A future JVM with a heap reservation legitimately larger than 64 GiB would have instances silently missed past the clamp boundary. The clamp was added to prevent a pathological mis-resolved _word_size from causing a multi-billion-iteration spin; it is the right trade-off, but the value is undocumented as a limitation.

## Notes

Issue #38 no-SEH fragility is the primary blocker for expanding test coverage on this
feature. The conservative scan is safe on windows under mingw/linux/macos but the
MSVC-ABI (msvc + clang-cl) cells hang when suite heap pressure triggers a GC safepoint
mid-walk. The real fix (_thread_in_native wrap around the walk) is deep and requires
CI-only reproduction — no local run possible. Until #38 is fixed, additional test modules
that grow heap footprint must not be added, as they push the no-SEH cells further toward
the stall threshold.

The scan is conservative-not-precise by design: false positives are ruled out (the
klass check is exact), but false negatives are expected on G1 (unmapped gaps),
ZGC/Shenandoah (coloured pointers), and any region whose page is not resident. The
test module hard-asserts only the RELIABLE invariants (visits > 0, count == visits,
visits <= PIN_COUNT, cap honoured, unregistered T => 0) and records best-effort
observations ([INFO]) for how many pinned instances were actually seen.

VMStruct name sets for CollectedHeap/_reserved and MemRegion/_start/_word_size are
stable across JDK 8-26 (verified in CI matrix). The narrow-klass decode uses a
three-candidate fallback for the base/shift field names to cover JDK 8-16
(Universe::_narrow_klass._base/shift), JDK 17-24
(CompressedKlassPointers::_narrow_klass._base/shift), and JDK 25+
(CompressedKlassPointers::_base/shift).
