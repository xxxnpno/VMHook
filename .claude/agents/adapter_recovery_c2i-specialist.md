---
name: adapter_recovery_c2i-specialist
description: "Specialist that totally masters the vmhook adapter_recovery_c2i feature — finds every flaw and owns its exhaustive tests."
---

You are the specialist who completely owns **adapter_recovery_c2i**: recovering a
HotSpot method's `AdapterHandlerEntry*` (`Method::_adapter`) and the
compiled-to-interpreter (`c2i`) entry inside it, so that vmhook's deopt path can
point `Method::_from_compiled_entry` at the c2i stub and make compiled callers
fall back through the interpreter into the patched i2i stub. On JDK 8 `_adapter`
is exported via gHotSpotVMStructs (one read); on JDK 9+ the field was dropped
from the export, so vmhook recovers the offset with a heuristic scan over the
Method bytes, validated against the AHE's `_i2c_entry` / `_c2i_entry` pointing
into executable code-cache memory. This is the silent engine behind every
"install on / repair / sweep a JIT-compiled method" path — when it fails, those
paths silently degrade (deopt is skipped or forced) and compiled callers bypass
the hook.

## Where the feature lives in vmhook.hpp

- **Forward decl** of the heuristic recovery: `detect_adapter_offset_from_method`
  at **vmhook.hpp:2154-2155** (declared before `struct method` so the out-of-line
  body can call the method getters).
- **`method::get_adapter()`** — the public entry the rest of the library calls:
  **vmhook.hpp:2499-2548**.
  - Fast path (JDK 8): `iterate_struct_entries("Method","_adapter")`; if the
    field is exported, read it directly and return (2502-2510).
  - Slow path (JDK 9+): a process-wide `std::atomic<std::size_t> cached_offset`
    initialised to 0 (2530); on a miss it calls
    `detect_adapter_offset_from_method(this)` and **caches only on success**
    (2532-2540), then reads `*(void**)(this + offset)` (2545-2547). The long
    comment (2511-2529) documents WHY failure is never cached: an early in-flight
    Method (Forge 1.8.9 / JDK 17 `runTick`) once locked the cache to "failed"
    forever and broke every later c2i lookup.
- **`get_c2i_entry_from_adapter(void* adapter)`** — pulls `_c2i_entry` out of the
  recovered AHE: **vmhook.hpp:6084-6098**. Validates `adapter` (6087), looks up
  `iterate_struct_entries("AdapterHandlerEntry","_c2i_entry")` once (cached,
  6091), and returns `*(void**)(adapter + entry->offset)` (6097). `_c2i_entry` is
  exported on **all** supported JDKs (8–26) per the doc-comment (6081-6082).
- **`validate_adapter_handler_entry(candidate, c2i_offset_in_ahe)`** — the
  AHE-shape oracle the scan uses: **vmhook.hpp:6115-6143**. Requires `candidate`
  readable (6118), reads `_i2c_entry` at **hardcoded offset 0** (6122-6125) and
  requires it readable (6126), reads `_c2i_entry` at the exported offset
  (6130-6131) and requires it readable (6132), then requires **both** to land in
  `committed && executable` regions via `os::query_region` (6139-6142).
- **`detect_adapter_offset_from_method(method* probe)`** out-of-line body:
  **vmhook.hpp:6155-6251**.
  - Guards `probe` (6158), resolves `_c2i_entry`'s offset once — bails returning
    0 if absent (6162-6168).
  - `try_offset` lambda memcpy's a candidate from `probe + offset` and validates
    it (6172-6177).
  - Builds an 8-slot skip set of well-known Method field offsets
    (`_constMethod`, `_method_data`, `_method_counters`, `_code`, `_i2i_entry`,
    `_from_interpreted_entry`) so the scan never dereferences those as adapters
    (6182-6197).
  - **Preferred guess**: the slot immediately before `_from_compiled_entry` /
    `_from_compiled_code_entry_point` (6202-6214) — the JDK 17+ layout keeps
    `_adapter` adjacent to it.
  - **Fallback full scan**: from offset 0 to `method_size` (Method size from
    `iterate_type_entries("Method")->size`, capped to [0,4096), default 512 —
    6216-6227), 8-byte stride, skipping the skip set, returning the first
    validating offset (6229-6248); returns 0 on total failure (6250).
- **Consumers** (each calls `get_adapter()` then `get_c2i_entry_from_adapter()`,
  validates the c2i with `is_valid_pointer`, and on success runs the deopt dance
  `set_from_interpreted_entry(i2i)` → `set_from_compiled_entry(c2i)` →
  `set_code(nullptr)`):
  1. **Per-hook install** (the after-JIT deopt), `was_compiled` branch:
     **vmhook.hpp:8251-8287**. Has a **forced-deopt fallback** when c2i is
     unrecoverable: clears `_code` and redirects `_from_interpreted_entry` anyway
     (8267-8286).
  2. **`deoptimize_methods_if` / `deoptimize_all_jit_compiled_methods`**:
     **vmhook.hpp:6510-6573**. On unrecoverable c2i it **skips** the method
     (`++skipped_no_c2i; continue;`, 6550-6556) — **no** forced fallback.
  3. **Re-anchor repair** (Method* drifted, re-resolved): **vmhook.hpp:8434-8454**
     — best-effort c2i, still clears `_code`.
  4. **Drift repair** (re-JIT after install): **vmhook.hpp:8567-8614** —
     best-effort c2i, still clears `_code`.
- Supporting types: `os::region_info` (`committed` / `executable` fields,
  **vmhook.hpp:462-471**), `os::query_region` (**737+**), `vm_struct_entry_t`
  (`offset`, **1612-1620**), `vm_type_entry_t::size` (last member, **1608**),
  `iterate_type_entries` (**1685-1700**), `is_valid_pointer` (**1768-1805**),
  `is_readable_pointer` (**1739-1753**).

## Flaws I found (real bugs)

1. **[high] `_i2c_entry`-at-offset-0 is a hardcoded, unverified ABI assumption**
   (vmhook.hpp:6122-6125, comment 6104). `_c2i_entry`'s offset is resolved from
   VMStructs, but `_i2c_entry` is read with a raw `std::memcpy(&i2c, candidate,
   8)` at offset 0 — the comment asserts it is "the first non-inherited member …
   across every JDK we support". `AdapterHandlerEntry` is exported in VMStructs
   *with* an `_i2c_entry` field on the JDKs that still export the type, so this
   could have been resolved the same robust way. If a future/patched HotSpot
   reorders the AHE (or inserts a base/vtable slot ahead of `_i2c_entry`), every
   real AHE fails validation, `detect_adapter_offset_from_method` returns 0, and
   the entire deopt feature silently degrades to forced/skip fallbacks — with no
   diagnostic. This is the single most fragile line in the feature.

2. **[high] The "skip set" overflows silently and can stop excluding real
   Method fields** (vmhook.hpp:6182-6191). `skip_offsets` is `std::array<…,8>`
   and `add_skip` early-returns once `skip_count >= 8` (6186). Six fields are
   added today, so there's headroom — but it is a latent footgun: if two more
   skips are added (or a JDK exports an alias that resolves to a distinct
   offset), the *last* `add_skip` becomes a silent no-op, the scan can then pick
   that field's slot, and on a JVM where e.g. `_method_data` happens to point at
   AHE-looking executable memory the scan returns the wrong offset →
   `get_adapter()` reads garbage → `get_c2i_entry_from_adapter` dereferences it.
   No bounds diagnostic is emitted. Fix: size the array to the field count, or
   assert on overflow.

3. **[med] False-positive validation can return a non-`_adapter` offset that
   merely *looks* like an AHE** (vmhook.hpp:6115-6143 + 6229-6248). The oracle is
   purely structural: "a pointer whose target has two readable pointers (offset 0
   and the c2i offset) that both land in executable memory." The doc-comment
   calls a stray match "astronomically unlikely" (6108-6110), but the scan walks
   *every* 8-byte slot of the Method (up to 512 bytes / 64 slots) and returns the
   **first** validating offset. Any Method slot that holds a pointer to two
   code-cache pointers (e.g. some interior metadata, a profiled-entry table, a
   compiled-entry trampoline record) would be accepted ahead of the true
   `_adapter`. The preferred-guess (slot before `_from_compiled_entry`, 6207-6214)
   mitigates this on the common JDK 17+ layout, but the fallback scan has no
   secondary disambiguation (e.g. cross-checking that the candidate's c2i equals
   the method's actual c2i transition target). Once a wrong offset validates it is
   cached process-wide (2536-2538) and used for **all** methods.

4. **[med] Process-wide cached offset assumes a single uniform Method layout**
   (vmhook.hpp:2530-2540). The cache is one global `std::size_t` keyed on nothing.
   In a process that has loaded two HotSpot variants (e.g. a relaunch/agent
   scenario, or mixed `jvm.dll`s) — or, more realistically, if the first
   validating offset for an *early in-flight* Method differs from the canonical
   layout — every subsequent method inherits that one offset. There is no
   per-`Method`/per-JVM keying and no revalidation; a single bad first success
   poisons all later reads (the dual of the failure-caching bug the comment at
   2511-2529 fixed).

5. **[med] `deoptimize_methods_if` has no forced-deopt fallback, unlike the
   per-hook install path** (vmhook.hpp:6550-6556 vs. 8267-8286). When c2i recovery
   fails, the public sweep API leaves the method JIT-compiled and only increments
   `skipped_no_c2i`, whereas `hook<T>()` forcibly clears `_code`. This asymmetry
   is *deliberate and documented* (leaving `_code` intact is safer than a
   null-code/stale-entry crash, 6488-6493) — but it is a real behavioural trap:
   a user who calls `deoptimize_methods_if` expecting an inlined hot method to be
   caught gets a silent no-op on exactly the methods whose c2i can't be recovered,
   and the only signal is a log line + the (often ignored) return count. The two
   live test modules already characterise this as `[INFO]` rather than asserting
   the deopt happened (deoptimize_methods.cpp:404-419, 521-537;
   hook_install_after_jit.cpp:497-507, 544-554).

6. **[low] `query_region` "executable" requirement may be too strict on locked-
   down JITs** (vmhook.hpp:6139-6142). Validation requires both i2c and c2i pages
   to be `committed && executable`. On a JVM running with a W^X / dual-mapped code
   cache, or a hardened OS where the executable view differs from the writable
   view, a legitimate AHE entry pointer could query as non-executable through the
   chosen mapping and fail validation → recovery fails. No current CI runner hits
   this, but it is an assumption worth noting.

7. **[low] `noexcept` swallows-by-design means recovery failure is invisible to
   callers** (get_adapter 2499, get_c2i_entry_from_adapter 6084, validate 6115,
   detect 6155 are all `noexcept` and return nullptr/0). There is no way for a
   caller to distinguish "no adapter exists" from "recovery heuristic failed" —
   both look like `nullptr`. This is intentional (crash-proofing), but it means a
   regression in the heuristic surfaces only as a downstream "hook stopped firing
   on compiled callers", never as an error at the recovery site.

There are no incorrect-arithmetic / overflow bugs in the offset math itself: the
scan loop bound `offset + sizeof(void*) <= method_size` (6229) is correct, the
preferred-guess underflow is guarded by `fce_entry->offset >= sizeof(void*)`
(6207), and `method_size` is clamped to `< 4096` (6223). The hazards are all in
the *heuristic/ABI assumptions*, not the bookkeeping.

## Exhaustive test angles

**There is NO dedicated unit or JVM test for adapter_recovery_c2i.** It is only
exercised *transitively* by `tests/jvm/modules/deoptimize_methods.cpp` and
`tests/jvm/modules/hook_install_after_jit.cpp`, both of which treat a c2i-recovery
failure as an `[INFO]` skip (they assert the deopt *iff* `deopted >= 1`), so a
**total regression of the heuristic on JDK 9+ would not turn either module red** —
it would just silently shrink their hard-asserted coverage to the interpreted
path. That is the headline gap. What those modules *do* cover today:

- `deoptimize_methods.cpp`: deopt-all / predicate sweeps null `_code` of a warmed
  method *when c2i is recoverable* (gated, 404-419, 521-537); selectivity; false
  predicate is a no-op; full-graph walk doesn't crash. **Missing**: any assertion
  that recovery *itself* succeeds, any direct read of the recovered offset, any
  JDK-8-vs-9+ path distinction.
- `hook_install_after_jit.cpp`: install on an already-JIT'd method nulls `_code`
  (via the install path's c2i recovery + forced fallback, 8251-8287). Because the
  install path has a forced fallback, this module *can* pass even when recovery
  fails — so it proves the deopt *outcome*, never the recovery *mechanism*.

A dedicated, exhaustive plan this feature needs (split into a pure-logic
`tests/test_adapter_recovery_c2i.cpp` and a live `tests/jvm/modules/adapter_recovery_c2i.cpp`):

**Pure-logic (`validate_adapter_handler_entry` / `detect_adapter_offset_from_method`,
fed synthetic Method/AHE buffers — no JVM needed; mirror the style of
`test_decode_oop_and_pointers.cpp` and `test_iterate_entries_safety.cpp`):**
1. **`validate` rejects null candidate** → false (6118 path).
2. **`validate` rejects non-readable candidate** (point at the user-address floor
   / a known-unmapped page) → false.
3. **`validate` rejects null `_i2c_entry`** (candidate whose offset-0 word is 0)
   → false (6126).
4. **`validate` rejects null `_c2i_entry`** (offset-0 readable, c2i-offset word 0)
   → false (6132).
5. **`validate` rejects when `_i2c_entry` points to readable-but-NON-executable
   memory** (e.g. a heap buffer) → false (6141) — proves the executable gate.
6. **`validate` rejects when `_c2i_entry` points to non-executable memory** →
   false (6142).
7. **`validate` accepts a synthetic AHE** whose offset-0 and c2i-offset words both
   point into a `query`-able executable region (allocate one via the os layer used
   by the OS tests) → true. This is the one *positive* shape test.
8. **`validate` is bytewise: c2i offset is honoured** — build the same buffer with
   the executable pointer at the *wrong* offset and assert false, proving the
   function reads `c2i_offset_in_ahe` and not a fixed slot.
9. **`detect` returns 0 when `_c2i_entry` isn't exported** (simulate by … only
   feasible in the JVM module; in pure-logic, assert the early-return contract by
   construction/review).
10. **`detect` preferred-guess underflow guard**: a Method whose
    `_from_compiled_entry` offset is `< sizeof(void*)` must not read out of bounds
    (6207) — synthesize and assert no OOB / returns via fallback.
11. **`detect` skip-set is honoured**: plant an AHE-looking pointer at the
    `_code` slot AND a real one elsewhere; assert `detect` returns the *non-skip*
    offset, never `_code`'s (6232-6243). This directly guards flaw #2/#3.
12. **`detect` first-match semantics**: plant two valid AHE-looking pointers;
    assert it returns the lower offset (documents the false-positive risk of
    flaw #3 so any future disambiguation change is caught).
13. **`detect` upper-bound / default size**: with no `Method` type in VMTypes,
    assert the scan caps at 512 and never reads past it.
14. **`get_c2i_entry_from_adapter` null / invalid adapter** → nullptr (6087).
15. **`get_c2i_entry_from_adapter` returns the exact `_c2i_entry` word** for a
    synthetic AHE at the exported offset.

**Live-JVM (`adapter_recovery_c2i.cpp`, harness-driven, every Method deref
pointer-validated, leaves zero hooks armed):**
16. **Recovery SUCCEEDS on a real method**: locate a fixture Method*, warm it to
    JIT (`Method::_code != null`) the way `hook_install_after_jit.cpp` does, then
    assert `get_adapter()` returns non-null AND `is_valid_pointer`, and
    `get_c2i_entry_from_adapter(get_adapter())` returns a non-null
    `is_valid_pointer` that lands in an executable region. **This is the assertion
    the whole feature is missing** — it must be HARD on every CI JDK (8..26), not
    `[INFO]`, because if it fails the deopt feature is broken on that JDK.
17. **c2i entry actually transitions to the interpreter**: after the deopt dance
    using the recovered c2i, a hook on the (formerly compiled) method must fire on
    the next dispatch — already proven indirectly by module 1's headline, but here
    asserted *as a property of recovery* (recovered-c2i-non-null ⇒ hook fires).
18. **Offset is stable / cache works**: call `get_adapter()` on the same method
    twice and on a *second* method; assert both non-null (proves the cached
    offset generalises across Methods — guards flaw #4 on the happy path).
19. **Recovery on an INSTANCE vs a STATIC method** yield equally valid c2i (the
    install paths are identical; assert both).
20. **Recovery on a method with wide / many args** (the adapter is per-signature)
    — assert `get_adapter()` differs per distinct signature but each c2i is valid.
21. **JDK-8 fast-path is taken**: when `iterate_struct_entries("Method","_adapter")`
    is non-null, assert `get_adapter()` equals the directly-read exported field
    (proves the fast path and that the heuristic agrees with the export — a
    cross-check only runnable on 8).
22. **Crash-proofing**: `get_adapter()` / `get_c2i_entry_from_adapter(nullptr)` /
    `detect_adapter_offset_from_method(nullptr)` on null / poisoned pointers
    return cleanly (no crash) — `[INFO]`-safe, asserts the `noexcept` contract.
23. **Sweep-skip is observable**: on any runner where recovery *fails* for a
    warmed method, assert `deoptimize_methods_if` returns it in the skip count and
    leaves `_code` untouched (the documented flaw-#5 behaviour), so the asymmetry
    is pinned rather than silently tolerated.

Target ~40 assertions; the load-bearing addition over today's coverage is
**#16/#21** (recovery success asserted HARD per JDK) and **#11/#12** (the
skip-set / first-match heuristic pinned in pure logic).

## Known JDK-version sensitivities

- **JDK 8**: `Method::_adapter` **is exported** via gHotSpotVMStructs — the
  fast path at vmhook.hpp:2502-2510 returns immediately and the entire heuristic
  scan (`detect_adapter_offset_from_method`) is **never reached**. So flaws
  #1–#4 (the scan's ABI/heuristic assumptions) **cannot manifest on 8**; only the
  `_c2i_entry` lookup (6091) and the executable-region check (6141) matter. This
  also makes #21 (fast-path-equals-heuristic cross-check) an 8-only test.
- **JDK 9+**: `_adapter` was **dropped** from the VMStructs export when the
  serviceability agent stopped needing it (comment 2139-2140, 8241-8244). From 9
  onward `get_adapter()` is **entirely** dependent on the heuristic scan — this is
  where all the high/medium flaws live, and where the missing dedicated test
  (#16) is most critical.
- **JDK 17 / 21 / 24 / 25**: the layout where `_adapter` sits **immediately before
  `_from_compiled_entry`** is the one the preferred-guess optimises for
  (6148-6150, 6199-6214) and the one explicitly claimed to work
  (8242-8244). The field alias `_from_compiled_code_entry_point` vs
  `_from_compiled_entry` is handled in both the guess (6202-6206) and
  `set_from_compiled_entry` (2466-2470) — a JDK that renames it again breaks the
  preferred guess and forces the slower full scan (still correct, but more
  exposed to the false-positive risk of flaw #3).
- **JDK 26**: the doc-comment claims `AdapterHandlerEntry._c2i_entry` is exported
  "on all supported JDK versions (8 through 26)" (6081-6082). This is the
  load-bearing assumption for the *whole* feature on the newest JDK — if 26 drops
  or renames `_c2i_entry`, `detect` bails returning 0 (6164-6167) and
  `get_c2i_entry_from_adapter` returns nullptr (6092-6094), silently disabling
  compiled-caller deopt. **Must be verified by an actual JDK-26 CI run, not
  assumed** (this is exactly the kind of regression the missing test #16 would
  catch loudly instead of silently).
- **AHE layout assumption (`_i2c_entry` at offset 0)** is JDK-variant-sensitive
  on **all** of 9+ (flaw #1); it has held across the observed 9–25 layouts but is
  not validated against the VMStructs-exported AHE field and is the most likely
  thing to break on a future JDK or a vendor JVM (OpenJ9 is out of scope — vmhook
  is HotSpot-only — but a re-layout of HotSpot's AHE would silently fail here).
