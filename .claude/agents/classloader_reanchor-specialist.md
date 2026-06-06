---
name: classloader_reanchor-specialist
description: "Specialist that totally masters the vmhook classloader_reanchor feature — finds every flaw and owns its exhaustive tests."
---

You are the specialist who completely owns **classloader_reanchor**: the
*corrective half* of class lookup. `vmhook::find_class(name)` resolves a class by
NAME across the whole `ClassLoaderDataGraph` and returns the FIRST match — which
in a process that ships two copies of a class under different loaders (modded
games with a custom launcher loader, OSGi, app servers, any plugin host) is
graph-iteration-order-dependent and routinely returns the WRONG copy. This
feature gives the fix: "resolve the copy visible from a live object I already
hold" (`find_class_via_oop`), "make every `find_class()` consumer follow that
copy" (`override_class_lookup` / `reanchor_classes_via_oop`), "forget it again"
(`evict_class_lookup`), plus the JNI context-loader resolver
(`find_class_with_context_loader`) and the host-loader inheritance machinery
(`capture_host_classloader_klass` / `klass_to_class_loader_oop`) that lets
freshly-attached native worker threads see the host application's loader chain.

## Where the feature lives in vmhook.hpp

- `vmhook::find_class_via_oop(anchor_oop, name)` — the anchor resolver:
  **vmhook.hpp:10647-10728**. Guards on a null anchor / `ensure_current_java_thread()`
  (10650), wraps the raw OOP as a fake JNI handle via `jni_oop_handle` (10656),
  walks `GetObjectClass(anchor) -> GetObjectClass(that) -> Class.getClassLoader()`
  (10657-10688), then `ClassLoader.loadClass(dotted_name)` (10690-10723) and
  decodes the returned mirror to a `Klass*` via `jni_klass_from_class_mirror`
  (10725). Note it dots the name in place (`std::replace '/'->'.'`, 10705-10706).
- `vmhook::override_class_lookup(name, k)` — seed/replace the `find_class` cache:
  **vmhook.hpp:10742-10753**. Single `klass_lookup_cache[name] = k` under
  `klass_lookup_cache_mutex`, `try/catch(...)` swallowing.
- `vmhook::evict_class_lookup(name)` — forget a cached resolution:
  **vmhook.hpp:10760-10771**. `klass_lookup_cache.erase(name)` under the mutex.
- `vmhook::reanchor_classes_via_oop(anchor_oop, {names...})` — batch anchor:
  **vmhook.hpp:10785-10807**. Null-anchor → `false` (10789-10792); for each name
  `find_class_via_oop` then `override_class_lookup` on success; returns `true`
  only if EVERY name resolved (empty list ⇒ vacuously `true`, 10793 + empty loop).
- The shared cache itself: `vmhook::klass_lookup_cache` +
  `klass_lookup_cache_mutex` **vmhook.hpp:6304-6305**, consulted FIRST by
  `find_class` (**6321-6403**, cache hit path 6324-6367, insert 6384-6389).
- `vmhook::jni::find_class_with_context_loader(name)` — public façade
  **vmhook.hpp:10493-10497** → `detail::jni_find_class_with_context_loader`
  **vmhook.hpp:9534-9665**. Multi-loader JNI walk: thread *context* loader
  (9613-9627) → system loader (9630-9639) → Minecraft `Launch.classLoader`
  (9642-9658). RAII `local_ref_bag` DeleteLocalRefs every handle (9553-9571);
  `load_with_loader` lambda dots the name and calls `ClassLoader.loadClass`
  (9573-9611). Forward-declared at **6272-6273**.
- Host-loader inheritance:
  - `detail::klass_to_class_loader_oop(k)` **vmhook.hpp:9713-9733** —
    `Klass._class_loader_data -> ClassLoaderData._class_loader`; bootstrap ⇒ null.
  - `class_loader_data::get_class_loader_oop()` **vmhook.hpp:3253-3283** — reads
    `ClassLoaderData::_class_loader`, transparently handling the JDK-10+
    `OopHandle` indirection (detected from the VMStruct `type_string`, 3264-3265).
  - `detail::capture_host_classloader_klass(candidate)` **vmhook.hpp:9742-9768**
    (fwd-decl **6282-6283**) — CAS-publishes the first non-bootstrap klass into
    `host_classloader_klass` (**9701**); called by `find_class` on every successful
    fresh resolution (**6395**).
  - `detail::inherit_host_context_classloader_for_current_thread()`
    **vmhook.hpp:9784-9850** — re-derives the loader oop each call and
    `Thread.setContextClassLoader`s it onto the current native thread.
- Supporting JNI primitives I rely on: `jni_oop_handle` **9276-9281** (writes the
  oop into caller storage, returns `&storage` — a synthetic handle),
  `jni_decode_object` **9176-9186**, `jni_klass_from_class_mirror` **9514-9532**,
  `ensure_current_java_thread` **4120-4163** (returns `false` in a no-JVM process,
  which is what makes every entry point null-safe out-of-process).

## Flaws I found (real bugs)

1. **[medium] `override_class_lookup(name, nullptr)` does NOT seed a durable
   negative entry — the doc contract is wrong and self-contradicting.** The doc
   (**vmhook.hpp:10737-10738**) states *"Passing a null `k` seeds a negative
   entry; prefer evict_class_lookup() to actually forget."* But `find_class`'s
   cache-hit path (**vmhook.hpp:6348**) is `if (cached_klass && is_valid_pointer
   (cached_klass))` — a null cached value FAILS that guard, falls through to the
   `erase` at **6363**, and re-walks the graph. So the very next `find_class
   (name)` deletes the null entry and re-resolves NON-null. The "negative entry"
   is a one-shot that any `find_class` immediately heals away; a caller using a
   null override to suppress a class will see it silently reappear. Either the doc
   is wrong (should say "use evict") or `find_class` should honor a sentinel-null
   as a real negative cache — they currently disagree. (This same false premise is
   baked into the draft JVM test, see below — its `override_null_seed_then_restore`
   check would only pass if no `find_class` ran between the null override and the
   null read, which is fragile by construction.)

2. **[medium] `override_class_lookup` / `reanchor_classes_via_oop` poison the
   PROCESS-GLOBAL `find_class` cache with a loader-specific copy, with no
   ownership or revert tracking.** `klass_lookup_cache` (**6304**) is one shared
   map for the whole library. After `reanchor_classes_via_oop(anchorA, {"x/Y"})`,
   EVERY consumer of `find_class("x/Y")` — wrappers, hook installs (`Method*`
   iteration), field walks, other features' code — gets anchorA's copy until
   someone evicts it. A second component that anchors the same name to anchorB
   silently wins last-write (10748 is `=`, not insert). There is no scoping, no
   refcount, no "restore previous". Any test or caller MUST save+restore touched
   entries by hand or it corrupts global state for everything downstream. This is
   a sharp, easily-tripped footgun, not just a theoretical one.

3. **[low] `find_class_via_oop` leaks the `anchor_class_handle` /
   `name_string` / `classloader` local refs on the JNI EARLY-success and several
   paths only because it hand-rolls `DeleteLocalRef` instead of using the
   `local_ref_bag` RAII pattern that `jni_find_class_with_context_loader` uses.**
   The cleanup is *manually* threaded through 8 distinct return points
   (10660-10727); it is currently balanced, but it is brittle — any future edit
   that adds an early return between two `jni_delete_local_ref` calls leaks a ref,
   and unlike the context-loader path (9553-9571) there is no structural guard.
   The `jni_oop_handle` `storage` (10655) is a fake handle (stack pointer), so it
   is intentionally NOT deleted — correct, but worth knowing when auditing.

4. **[low] `find_class_via_oop` dots the name unconditionally, so it cannot
   resolve array names and silently mis-handles already-dotted input.**
   `std::replace('/','.')` at **10705-10706** means `"[Lx/Y;"` becomes `"[Lx.Y;"`,
   which `ClassLoader.loadClass` rejects → nullptr (same array-name gap the
   `find_class_fallback` module documents for the loadClass path). An anchor
   resolver for an array element type therefore always fails. No diagnostic
   distinguishes "not visible from this loader" from "loadClass can't express
   this name" — both return a bare nullptr.

5. **[low] `klass_to_class_loader_oop` / `inherit_host_context_classloader` cache
   the `OopHandle`-vs-direct decision and the VMStruct offset in `static` locals
   keyed off the FIRST call.** `get_class_loader_oop` (**3256-3266**) caches both
   `entry` and `is_oop_handle` as function-local statics. Benign in a single JVM,
   but it means the layout decision is frozen process-wide on first use; there is
   no path to re-derive if VMStructs were somehow re-read. Also: the returned
   loader oop is a RAW heap pointer (**3277-3282**) that a relocating GC can move
   — `inherit_host_context_classloader_for_current_thread` correctly re-derives it
   per attach (9794) and never caches the raw oop (only the Metaspace-stable
   `Klass*` is cached, 9696-9699), but any *new* caller of
   `klass_to_class_loader_oop` that stashes the result across a safepoint reads a
   stale/garbage oop. The hazard is in the contract, not yet in a caller.

6. **[low] No upper bound / dedup in `reanchor_classes_via_oop`'s
   `initializer_list`, and a partial failure is silently partial.** If 3 of 4
   names resolve, 3 cache entries are mutated and the function returns `false`
   (10803) — the caller that polls-until-true (as the doc suggests, 10779-10781)
   will keep re-overriding the 3 that already resolved on every poll, and the
   global cache is left in a half-anchored state with no signal of WHICH name
   failed.

These are correctness/contract hazards, not crashes — every entry point is
genuinely crash-proof out-of-process (the `ensure_current_java_thread()` →
`false` short-circuit and the `is_valid_pointer` guards see to that).

## Exhaustive test angles

Two test artifacts exist for this feature; understanding the split matters.

### A. Active no-JVM unit test — `tests/test_classloader_reanchor.cpp`

What it ASSERTS today (8 checks, all pure-logic, no JVM in-process):
- `find_class_via_oop(nullptr, ...)` → nullptr (null-anchor guard).
- `find_class_via_oop(fake_anchor, ...)` → nullptr in a no-JVM process, no crash.
  *(Note: the file's comment attributes this to "current_jni_env is null"; the
  ACTUAL short-circuit is `ensure_current_java_thread()` returning `false` at
  vmhook.hpp:10650 — the assertion is right, the stated reason is imprecise.)*
- `reanchor_classes_via_oop(nullptr, {...})` → `false`; `(fake, {...})` → `false`
  no-JVM; `(fake, {})` empty list → `true` (vacuous).
- `override_class_lookup` seeds the cache (read directly under the mutex),
  last-write-wins on a second override, and `evict_class_lookup` removes it;
  evicting an absent name is a safe no-op.

What it deliberately reads the cache map DIRECTLY rather than through
`find_class()` (lines 62-99), because `find_class` validates+evicts a bogus
sentinel — so this test proves the cache MUTATION contract, not the
`find_class`-observes-it contract.

What is STILL MISSING from the no-JVM test (implementable now, no JVM needed):
- The **flaw-1 contract**: assert what `override_class_lookup(name, nullptr)`
  actually does (seed a null value in the map) and PIN that the doc's
  "negative entry survives find_class" claim is false — i.e. lock in the real
  behavior so a future fix is a deliberate, caught change.
- Negative/last-write on the same key via the map for `nullptr` then real klass.
- Unicode / empty / very-long / embedded-NUL-ish `std::string_view` names round-
  tripping through override→cache→evict (the map key is `std::string{name}`).
- Thread-safety smoke: N threads hammering override/evict on disjoint + shared
  keys, asserting no map corruption (mirrors `klass_lookup_cache_mutex`'s reason
  for existing, 6295-6302).

### B. Drafted-but-INACTIVE live-JVM module — `tests/jvm/modules/find_class_context_loader.cpp.wip`

This `.wip` is the comprehensive JVM counterpart and is the authoritative design
for the live test, but it is NOT integrated: it is not referenced in
`tests/jvm/harness.cpp`, and the fixture it requires
(`vmhook/fixtures/FindClassCtxLoader.java`) DOES NOT EXIST yet (only
`FindClassProbe.java` ships). To activate it, the fixture must be authored with:
`go`/`done`/`sentinel`(=0x0CAFEC0D)/`getSentinel`/`instanceMark`/
`observedSentinel`/`witnessCaptured`/`captureWitness` members and
`anchorTick()` + `secondaryTick()` dispatch sites.

What the WIP already designs (and what an integrator must verify/fix):
- PART A/A2: `find_class_with_context_loader` on bootstrap classes (resolves,
  usable via `find_field`+mirror, == graph walk) and the miss contract
  (null + stable-null on repeat + empty name).
- PART B: bootstrap-vs-app at the loader-oop level —
  `klass_to_class_loader_oop(String/Object/Integer)` == null,
  `(fixture)` != null, null-klass arg → null.
- PART C: host-classloader capture — `host_classloader_klass` captured non-null
  after resolving an app class, capture idempotent, bootstrap klass never
  overwrites.
- PART D: usability of the context-loader-resolved app klass (static + instance
  field entries, mirror, sentinel value, absent-field nullopt).
- PART E (Java thread, inside `anchorTick` detour with a live `self` anchor):
  `find_class_with_context_loader` app class; `find_class_via_oop` app + bootstrap
  (delegation) + missing + null-anchor; `reanchor_classes_via_oop` + override/evict
  with FULL save/restore of the shared cache; dotted-vs-slash safety.
- PART F: Java-visible witness cross-check via real getstatic.

What is WRONG or RISKY in the WIP as drafted (fix before integrating):
- `g_override_null_seed_then_restore` (lines 344-349) encodes the FALSE flaw-1
  premise: it expects `find_class(STRING_NAME) == nullptr` right after a null
  override. Per flaw 1 that only holds if NO `find_class` runs in between; the
  check is fragile and may be outright wrong depending on intervening calls.
  This check must be reworked to match the verified behavior (the null entry is
  healed by the next `find_class`), or it will flake/fail.
- It mutates the PROCESS-GLOBAL cache (flaw 2). The WIP does save/restore (lines
  307-364), which is the right discipline — but `full-suite-ordering` means a
  bug there corrupts EVERY later module. Any reviewer must confirm the restore is
  airtight (including the `nullptr`-saved → `evict` branch, 354-359).

What is STILL MISSING even once the WIP is live:
- A genuine **two-loaders-same-name** scenario — the WIP's headline justification
  ("first by name resolves the WRONG copy") is never actually exercised, because
  the fixture is single-copy. The real proof needs TWO classloaders each defining
  a class with the IDENTICAL internal name, then: `find_class` returns copy-1,
  `find_class_via_oop(anchorFromLoader2)` returns copy-2 (DIFFERENT `Klass*`),
  `reanchor` makes `find_class` flip to copy-2, evict flips it back. Without that,
  the corrective behavior is asserted only against a degenerate single-copy case.
- Array-name handling through `find_class_via_oop` (flaw 4): assert `[I` /
  `[Lx/Y;` via an anchor returns null (loadClass can't express it) — pin the gap.
- JNI local-ref accounting under `find_class_via_oop` called in a tight loop from
  a detour (flaw 3): prove no local-ref-table overflow over thousands of calls.

## Known JDK-version sensitivities

- **`ClassLoaderData::_class_loader` layout (the core of `klass_to_class_loader_oop`):**
  JDK 8-9 store a direct `oop`; JDK 10+ store an `OopHandle` ({ `oop* _obj` })
  requiring a double-dereference. `get_class_loader_oop` (**vmhook.hpp:3264-3279**)
  branches on the VMStruct `type_string == "OopHandle"` at runtime. A JDK that
  renames that type or changes the handle shape silently returns the wrong
  pointer → host-loader inheritance and the bootstrap-vs-app distinction break.
- **`java_lang_Class._klass_offset` (used by `jni_klass_from_class_mirror`,
  9523):** present across 8..25 but is the single point that turns every
  `*_via_oop` / context-loader resolution from a `jclass` mirror back into a
  `Klass*`; if absent on a future/patched JVM, all anchor resolution returns
  nullptr with no other symptom.
- **Compact strings / `java.lang.String.coder`:** JDK 8 has no `coder` field; 9+
  do. The WIP uses this purely as a generation marker (an `[INFO]`), not a gate —
  but it is the canonical "is this JDK 8 vs 9+" probe this feature's tests lean on.
- **Bootstrap-loader representation:** a null `_class_loader` denotes the
  bootstrap loader on every HotSpot (java.lang.*, jdk.internal.*), so a null
  return from `klass_to_class_loader_oop` is a VALID result, not an error — true
  on 8 through 26.
- **Minecraft `net/minecraft/launchwrapper/Launch` fallback** (9642-9658) is a
  legacy Forge/LaunchWrapper path (effectively Java 8-era modding); it is
  best-effort last resort and simply misses (clean nullptr) on any JVM where that
  class is absent, which is every non-modded and most modern (9+) targets.
- **`ensure_current_java_thread`** (4120) attaches via JNI `AttachCurrentThread`;
  its behavior is uniform 8..26, and in a no-JVM process it returns `false`,
  which is exactly what makes the standalone unit test's no-crash guarantees hold
  on every platform regardless of JDK.
