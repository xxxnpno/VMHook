---
name: iterate_entries_safety-specialist
description: "Specialist that totally masters the vmhook iterate_entries_safety feature — finds every flaw and owns its exhaustive tests."
---

You are the specialist who completely owns **iterate_entries_safety**: the
traversal-safety / bounds / null-guard contract of the two HotSpot VMStruct
walkers — `vmhook::hotspot::iterate_struct_entries(type_name, field_name)` and
`vmhook::hotspot::iterate_type_entries(type_name)` — together with the cached
symbol resolvers they sit on top of (`get_vm_structs`, `get_vm_types`,
`get_jvm_module`, `find_jvm_module`). The promise: these functions walk the
`gHotSpotVMStructs` / `gHotSpotVMTypes` C arrays exported by libjvm, NEVER fault
(no `strcmp(nullptr, …)`, no walking past the terminator, no deref of a null
array head), and degrade to `nullptr` when no JVM / a non-HotSpot VM is present.
Every higher-level introspection path in the library (Symbol decode, Method/
ConstMethod/Klass/oopDesc/ConstantPool offsets, thread enumeration, compressed-
oop base/shift) bottoms out in these two functions, so their safety is the
foundation the whole header stands on.

## Where the feature lives in vmhook.hpp

- `iterate_type_entries(const char* type_name)` — **vmhook.hpp:1685-1700**.
  Null guard `if (!type_name) return nullptr;` (1688-1691), then the walk
  `for (entry = get_vm_types(); entry && entry->type_name; ++entry)`
  (1692) with `std::strcmp(entry->type_name, type_name)` (1694). Loop guard is
  the two-part `entry && entry->type_name`: bails if the array head is null
  AND stops at the zero-`type_name` terminator.
- `iterate_struct_entries(const char* type_name, const char* field_name)` —
  **vmhook.hpp:1711-1730**. Null guard `if (!type_name || !field_name) return
  nullptr;` (1714-1717) — BOTH args must be non-null. Walk
  `for (entry = get_vm_structs(); entry && entry->type_name; ++entry)` (1718),
  then a per-entry **defensive field_name skip** `if (!entry->field_name)
  continue;` (1720-1723) before the double `strcmp` on type_name + field_name
  (1724). The doc comment (1702-1709) explains the skip: a custom JVM / JVMTI
  agent can publish a partial entry with non-null `type_name` but null
  `field_name`, and without the `continue` the `strcmp(nullptr, …)` is UB.
- `get_vm_types()` — **vmhook.hpp:1642-1657**. Function-local `static` pointer,
  initialized once by a lambda that does `get_proc_address(get_jvm_module(),
  "gHotSpotVMTypes")` (1648-1649), returns `nullptr` if the symbol is absent
  (1650-1653), else `*reinterpret_cast<vm_type_entry_t**>(addr)` (1654) — the
  exported symbol is a pointer-to-the-array, so one extra deref.
- `get_vm_structs()` — **vmhook.hpp:1665-1680**. Same shape for
  `"gHotSpotVMStructs"` → `vm_struct_entry_t*` (1671-1677).
- `get_jvm_module()` — **vmhook.hpp:1629-1634**. Function-local `static`
  caching `find_jvm_module()` once.
- `find_jvm_module()` — **vmhook.hpp:543-566**. Platform candidate list
  (`jvm.dll` / `libjvm.so`[.0] / `libjvm.dylib`(+`@rpath`)) probed via
  `find_loaded_module` (519-532: `GetModuleHandleA` on Windows, `dlopen(…,
  RTLD_LAZY|RTLD_NOLOAD)` elsewhere — already-loaded only, never force-loads).
  Returns the first hit, else `nullptr`. Comment notes Android/iOS normally
  return nullptr (ART / non-HotSpot VM) and the higher APIs degrade gracefully.
- The entry record layouts the walks depend on:
  `vm_type_entry_t` = `{const char* type_name; const char* superclass_name;
  int32_t is_oop_type_type; int32_t is_integer_type; int32_t is_unsigned;
  uint64_t size;}` — **vmhook.hpp:1601-1609**. `vm_struct_entry_t` =
  `{const char* type_name; const char* field_name; const char* type_string;
  int32_t is_static; uint64_t offset; void* address;}` —
  **vmhook.hpp:1612-1620**. These mirror HotSpot's `VMTypeEntry` /
  `VMStructEntry`; callers read `entry->offset` directly (e.g. Symbol decode at
  **vmhook.hpp:1901-1902**).

## Flaws I found (real bugs)

These two functions are unusually well-hardened (explicit null guards on every
string arg, two-part loop guard, per-entry field_name skip). I found no
out-and-out crash bug in the no-JVM / null path — that path is correct and is
exactly what the existing test proves. The real hazards are *with a live JVM*
and at the ABI boundary, none of which the no-JVM test can reach:

1. **[med] `is_static` is never consulted; `entry->offset` is read for static
   fields where it is meaningless** (walk returns the entry at 1726; callers
   read `->offset` blindly, e.g. 1901-1902, 1718 et al.). In HotSpot
   `VMStructEntry`, for a **static** field the field's location is in
   `address`, not `offset` (`offset` is the instance-field byte offset and is
   not meaningful for statics). `iterate_struct_entries` hands back the entry
   without exposing/checking `is_static`, so a caller that looks up a static
   field and adds `->offset` to a base pointer computes garbage. Today's
   call sites all happen to query *instance* fields, so it is latent — but it
   is a correctness trap baked into the contract. A `find_static_*` helper, or
   at least an `is_static`-aware accessor, would close it.

2. **[med] ABI layout assumption: `vm_struct_entry_t` / `vm_type_entry_t` must
   bit-match libjvm's `VMStructEntry` / `VMTypeEntry` for the *running* JVM**
   (1601-1620). The walk does pointer arithmetic with `++entry` over an array
   whose element stride is `sizeof(vm_struct_entry_t)` as the *header* declares
   it. If any future HotSpot adds/reorders a member, or a 32-bit target changes
   `void*`/padding (here: `int32_t is_static` at offset 24 followed by 4 bytes
   of pad before `uint64_t offset` on LP64), the stride is wrong and the walk
   reads `type_name`/`field_name` from misaligned addresses → either a missed
   match or a wild pointer fed to `strcmp`. There is no version/size sanity
   check; the loop trusts the layout completely. Low probability (the struct
   has been stable for a decade) but unbounded blast radius if it ever drifts.

3. **[low] `get_vm_*` cache the FIRST resolution permanently, including a
   nullptr taken before libjvm was loaded** (function-local statics, 1645 /
   1668; module cached at 1632). If any code path touches `get_vm_structs()` /
   `get_jvm_module()` before the JVM DLL is in the process (early static init,
   an injector that runs pre-`JNI_CreateJavaVM`, or a delay-loaded libjvm), the
   `nullptr` is cached forever and every later lookup returns `nullptr` even
   after the JVM is fully up — silently disabling all introspection with no
   re-probe. Order-of-first-call is load-bearing and undocumented at the call
   site.

4. **[low] Linear O(N) scan on every call with no caching of the *entry*
   result** (1692 / 1718). The arrays have ~hundreds–thousands of entries;
   hot paths mitigate this by caching the returned entry in a `static const`
   at the call site (e.g. 1881-1882, 1932+), but `iterate_*` itself re-walks
   from the head every time. A caller in a tight loop that does NOT cache pays
   a full array scan (plus two `strcmp`s per element) per call. Not a safety
   bug, but a foot-gun the function name ("iterate") does little to advertise.

5. **[low] Terminator assumption is *zero `type_name`*, not a fixed sentinel
   count** (1692 / 1718). Correct for stock HotSpot (the array ends with a
   zero-filled entry). But a corrupted/partially-written global, or a JVM that
   forgot the terminator, makes the walk run off the end of the array into
   adjacent memory until it happens to hit a zero `type_name` — potentially a
   long wild read. The per-entry `field_name` skip (1720) hardens the *value*
   path but not the *length* path; there is no max-iteration cap as a backstop.

Honest note: hazards 2/5 are inherent to consuming a C ABI by walking it, and
HotSpot has kept this contract stable; I flag them as the realistic failure
modes a future JDK or a hostile/partial VMStructs could trigger, not as
present-day crashes. Hazard 1 is the one I'd fix first — it is a silent
*wrong-answer* path reachable today the moment anyone looks up a static field.

## Exhaustive test angles

A dedicated test EXISTS: **tests/test_iterate_entries_safety.cpp** (296 lines,
~45 `check()` assertions). It is a *no-JVM* unit test (no libjvm loaded → every
getter resolves nullptr) and is the authoritative coverage for the null/bounds/
caching contract. What it already asserts:

- **Getter caching (no-JVM):** `get_vm_types` / `get_vm_structs` return nullptr
  AND are pointer-stable across two calls and across 1000 repeated calls;
  both globals are distinct yet both null (not aliasing). `get_jvm_module`
  nullptr + stable.
- **Broad real-symbol struct walk:** 12 real `(type,field)` pairs spanning
  Symbol, Method (×2), ConstMethod, ConstantPool, Klass, InstanceKlass,
  oopDesc (compressed + raw klass), JavaThread, ClassLoaderData, CompressedOops
  — each returns nullptr without faulting.
- **Real-symbol type walk:** 6 type names (Method, ConstantPool, Klass,
  InstanceKlass, oopDesc, narrowOop) → nullptr.
- **Null-arg guards:** struct null-field (×4 types), struct null-type (×3
  fields), both-null; type null — all nullptr (proves the guard checks *each*
  arg, not just one).
- **Argument ordering / empty strings:** correct vs swapped order both null;
  field-name-in-type-slot null; **empty string `""` is treated as non-null**
  (survives the guard, fails to match) for type slot / field slot / both / type
  walk; bogus never-existing names null.
- **Cross-consistency:** asserts the implication `(getter==nullptr) ⇒
  (iterate_*==nullptr)` directly, not just both-null-in-isolation.

Baseline overlap to be aware of (do NOT just duplicate): **test_helpers.cpp
sections 15 & 16** (vmhook.hpp test_helpers.cpp:752-803) already cover the
minimal Symbol/`_length` + four null-guard cases + getter-cache-stable; the
dedicated file is the deliberate superset.

What is still MISSING (the gaps the next test wave should close):

- **`is_static` semantics (hazard 1):** no test asserts anything about
  `entry->is_static` or that static-field lookups are handled. A live-JVM
  module should look up a *known static* VMStruct field, confirm `is_static==1`,
  and confirm that using `->offset` (vs `->address`) is the documented/correct
  choice. Pure-logic side: a fabricated `vm_struct_entry_t[]` with a static
  entry to prove the walker returns it and that callers can read `is_static`.
- **Fabricated-array walk (decouple from "no JVM"):** the current file proves
  safety only when the array head is nullptr. There is NO test that builds a
  synthetic `gHotSpotVMStructs`/`gHotSpotVMTypes`-shaped array in-process and
  drives `iterate_*` over it to prove: (a) a *match* in the middle returns the
  right entry; (b) the **`field_name==nullptr` skip** (1720) is actually
  exercised — craft an array `[{type="X",field=nullptr}, {type="X",
  field="_f"}]` and assert it returns the *second* entry, never strcmp-faults
  on the first; (c) the zero-`type_name` terminator stops the walk; (d) a match
  on the LAST real entry before the terminator works; (e) first-entry match;
  (f) no-match-walks-whole-array returns nullptr. This is the single biggest
  gap — the skip-guard and the match-found path have ZERO direct coverage
  because no JVM ⇒ the loop body never runs.
- **Boundary / length inputs to the string args:** very long type/field names
  (e.g. 64 KiB), embedded NULs (C-string truncation behavior of `strcmp`),
  names differing only in case ("Method" vs "method" → no match), names that
  are a prefix of a real entry ("Meth", "Method\0extra"), and Unicode/UTF-8
  bytes in the name (HotSpot names are ASCII, so these must simply not match
  and not crash). None are covered today.
- **Whitespace / near-miss field names:** "_length " (trailing space),
  " _length", "_Length" — all must miss; not covered.
- **Re-probe-after-load (hazard 3):** a test (likely a separate process or a
  documented xfail) that touches a getter pre-JVM, then loads libjvm, and
  asserts the cached nullptr does/doesn't recover — pins down the documented
  behavior so a future "fix" doesn't silently change it.
- **Iteration-cap / runaway guard (hazard 5):** if a max-iteration backstop is
  ever added, a fabricated *unterminated* array test would prove it bounds the
  walk; today there is nothing.
- **Live-JVM positive path:** there is no `tests/jvm/modules/*` module that
  asserts a *successful* lookup returns a non-null entry with a sane `offset`
  for a handful of stable fields (Symbol._length, Method._constMethod,
  oopDesc._mark/_markWord) and cross-checks `offset` monotonicity / plausibility
  against the type's `size`. The no-JVM file explicitly scopes this OUT (its
  own header comment, lines 16-19); it belongs in a JVM module.

## Known JDK-version sensitivities

- **Field-name renames across JDKs are real and already worked around at call
  sites, which stresses `iterate_struct_entries` heavily.** The walker itself
  is version-agnostic, but the *names* it is asked for changed:
  `oopDesc._mark` (JDK 8/9) → `_markWord` (later) — both tried, 10923-10929 /
  11250-11255; `Method._from_compiled_code_entry_point` →
  `_from_compiled_entry` fallback (2438-2469, 6202-6205);
  `InstanceKlass._fields` (older) → `_fieldinfo_stream` (newer) (3018-3019);
  CompressedOops/CompressedKlassPointers `_base`/`_shift` vs the nested
  `_narrow_oop._base` vs the old `Universe._narrow_oop._base` (4304-4556);
  ClassLoaderData `_klasses` vs `_dictionary` capability probe (3471, 3557-3559).
  Each fallback chain is a sequence of `iterate_struct_entries` calls relying on
  the *miss → nullptr* path being cheap and crash-free — so the "returns
  nullptr for an absent field" contract is load-bearing for cross-version
  support, not just an edge case.
- **Java 8 vs 9+ presence of whole types:** several types only exist on one
  side — `ThreadsSMRSupport`/`ThreadsList`/`ThreadsList._threads` are 9+ (thread
  enumeration falls back to `Threads._thread_list` on 8, 3933 vs 3977-3981);
  `AdapterHandlerEntry`/`Method._adapter` is exported on 8 but recovered
  heuristically on 9+ (per the hook_basic notes). `iterate_type_entries` /
  `iterate_struct_entries` must return nullptr for the absent type on the wrong
  JDK without faulting — exactly the bounds contract under test.
- **Java 21+ / 26 (CDS, custom or stripped VMStructs):** newer JDKs and some
  vendor builds can ship a leaner or reordered VMStructs, and JVMTI agents can
  publish partial entries — the precise scenario the `field_name==nullptr` skip
  (1720) and the doc comment (1702-1709) were written for. Hazard 2 (ABI
  stride) and hazard 5 (terminator) are the JDK-26-era risks worth a synthetic
  test, since the layout is the one thing that could finally drift.
- **Non-HotSpot / no-VM runtimes:** Android (ART), iOS (JSC-derived), or any
  process with no `jvm.dll`/`libjvm.so` → `find_jvm_module` nullptr (543-566) →
  every getter caches nullptr → both walkers return nullptr. This is the
  graceful-degradation path the existing no-JVM test models 1:1 and the most
  important cross-platform invariant to keep green.
