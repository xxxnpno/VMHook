---
name: collection_type_tags-specialist
description: "Specialist that totally masters the vmhook collection_type_tags feature — finds every flaw and owns its exhaustive tests."
---

You are the specialist who completely owns **collection_type_tags**: the C++
type-tag layer over `java.util` containers — the six wrapper classes
(`vmhook::collection`, `list`, `set`, `linked_list`, `map`, `hash_map`), the
inheritance lattice that makes a `list`/`set`/`hash_map` substitutable for its
base, and the *element type-tag mapping* that turns a live OOP's field shape
into the right traversal so `get_field("foo")->get().to_vector<T>()` /
`to_entries<K,V>()` builds `std::vector<std::unique_ptr<T>>` /
`std::vector<std::pair<unique_ptr<K>,unique_ptr<V>>>` — and the documented
**never-throw / empty-on-failure** guarantee on every one of those entry points
when the OOP is null/invalid or has no recognised layout.

The *behavioural* live-JVM walks (real ArrayList/LinkedList/HashSet/TreeSet/
HashMap/LinkedHashMap/TreeMap content with a JVM up) are exercised by the five
`tests/jvm/modules/collection_*.cpp` modules. **My** owned surface is the
type-tag identity + lattice + the field-shape→walk-helper dispatch cascade + the
null/empty/never-throw contract, and my dedicated pure-logic test
`tests/test_collection_type_tags.cpp`.

## Where the feature lives in vmhook.hpp

- Forward declares of the six tags: **vmhook.hpp:1471-1476**.
- `class collection : public object_base` — the base wrapper and the dispatch
  hub: **vmhook.hpp:14643**. ctor `explicit collection(oop_t) noexcept`
  (**14656-14659**). Protected helpers that key off the *live OOP's* klass (not
  the C++ type registry): `oop_klass()` → `klass_from_oop` (**14666-14669**),
  `get_field_by_oop_klass` (**14674-14708**, incl. an inherited-static mirror
  path 14687-14701), `get_method_by_oop_klass` walking the super chain
  (**14713-14734**). `size()` via virtual `size()` dispatch (**14742-14750**),
  `is_empty()` (**14760-14763**).
- **The element type-tag mapping itself** — `collection::to_vector<element_type>()`
  (**14792-14903**). A field-presence cascade picks the traversal:
  ArrayList (`size`+`elementData` → direct array walk, 14803-14834),
  LinkedList (`first`+`size` → `linked_list_walk_items`, 14837-14846),
  HashSet/LinkedHashSet (`map` → `hash_map_walk_keys`, 14850-14859),
  TreeSet (`m` → `tree_map_walk_keys`, 14863-14872), then a generic
  `size()`+`get(int)` fallback valid only for List impls (14874-14902).
  Each decoded element becomes `make_unique<element_type>(oop)`, **null Java
  elements become `nullptr` slots** (14828-14830, 14897-14899).
- The pure type-tags (no own logic — they exist so a detour can express intent
  as `const std::unique_ptr<vmhook::list>&` etc.):
  `class list : public collection` **14921-14928**;
  `class set : public collection` **14942-14949**;
  `class linked_list : public list` **14963-14970**;
  `class hash_map : public map` **15157-15164**.
- `class map : public object_base` — the Map branch (deliberately **not** a
  `collection`, since Map is not a Collection in Java): **14986**. ctor
  **14993-14996**. It *duplicates* `oop_klass`/`get_field_by_oop_klass`/
  `get_method_by_oop_klass` (**15002-15073**) rather than share a base.
  `size()` here has an extra fallback to the `size` **field** when no `size()`
  method resolves (**15079-15092**). `to_entries<key_type,value_type>()`
  (**15116-15141**): HashMap/LinkedHashMap (`table` → `hash_map_walk_entries`)
  then TreeMap (`root` → `tree_map_walk_entries`), else empty.
- The out-of-line walk helpers that do the raw header reads (all template
  `<…, out_t>`, all bound-guarded, all null Java → `nullptr`):
  `linked_list_walk_items` **15182-15238** (caps the walk at `size`),
  `hash_map_walk_entries` **15249-15329** (per-bucket guard `< (1<<20)`),
  `hash_map_walk_keys` **15335-15402**, `tree_map_walk_entries`
  **15414-15521** (iterative in-order with an explicit `std::vector<void*>`
  stack + a `visited > (1<<24)` cap), `tree_map_walk_keys` **15527+**.
  Forward-declared at **14619-14628** so GCC first-phase lookup resolves them
  inside the templated `to_vector` body.
- The user-facing reach: `field_proxy::value_t::to_vector<E>()` declared
  **vmhook.hpp:11945-11947**, defined out-of-line **15638-15686**;
  `value_t::to_entries<K,V>()` declared **11959-11961**, defined
  **15696-15711**. Both read the stored compressed OOP via
  `static_cast<std::uint32_t>(*this)` (the templated `operator target_type()`,
  **11886-11894** → `cast_for_variant<uint32_t,bool>` final
  `static_cast` branch, **11862-11865**), `decode_oop_pointer` it, and on
  null/invalid return `{}` after a `VMHOOK_LOG` (15644-15650, 15702-15709).
  `value_t::to_vector` additionally **special-cases a raw object-array field**
  (`signature` begins `"[L"` or `"[["`) and walks the array directly instead of
  routing an ObjArrayKlass through `collection::to_vector` (**15659-15683**) —
  this is Fix #1 (commit ec1c8a8); everything else (`"L…;"` reference fields)
  falls through to `collection{oop}.to_vector<E>()` (**15685**).
- `value_t` itself is the aggregate `{ std::variant<bool,i8,i16,i32,i64,float,
  double,u16,u32> data; std::string signature{}; }` (**11658-11671**); the
  `u32` alternative is the compressed-OOP/reference slot.

## Flaws I found (real bugs)

1. **[medium] `value_t::to_vector` array special-case keys off `signature` but
   `collection::to_vector` does not — a Map field routed through `to_vector`
   silently returns empty with no diagnostic.** `value_t::to_vector`
   (15659-15685) only branches on `"[…"`; any `"L…;"` falls through to
   `collection::to_vector`, which probes for `size`/`elementData`/`first`/
   `map`/`m` and `get(int)`. A `java.util.Map` field has none of those, so the
   generic fallback's `get_method_by_oop_klass("get")` finds Map.get(Object)
   (wrong arity/semantics: it's `get(Object)`, not `get(int)`) — the
   `call<std::uint32_t>(index)` at **14890** invokes `Map.get` with an int
   boxed/passed as the key. On most maps that returns null → `nullptr` slots
   (benign-looking), but it is a **silent wrong-path** with a live call gate per
   element rather than an empty/typed rejection. The symmetric guard exists for
   `cast_for_variant<unique_ptr>` (the `signature.front() != 'L'` reject at
   **11833-11836**, "FLAW B fix") but was never extended to the collection
   entry points. Honest severity: medium — no crash, but the documented
   "to_vector on a Map field" behaviour is undefined-by-omission.

2. **[medium] Generic `to_vector` fallback calls `get(int)` on any Collection
   that reached it, including non-RandomAccess and Queue/Deque impls where
   `get(int)` is O(N) or absent.** **14874-14902**: after the four fast paths
   miss, it blindly resolves `get` and calls it `n` times. For an
   `ArrayDeque` (no `get(int)`, no `elementData`/`first`/`map`/`m`) the
   `get_method_by_oop_klass("get")` returns nullopt → empty (acceptable). But
   for a `PriorityQueue` or a custom List whose `get` is O(N), this is O(N^2)
   with N live interpreter call-gate dispatches — and the comment at 14874
   ("only valid for List impls") is the only guard. No klass-shape check
   confirms the impl actually *is* a List. Low blast radius but a real
   performance/correctness cliff outside the four fast-path containers.

3. **[low] Concurrent structural mutation races every walk helper; the guards
   bound runaway loops but not torn reads.** `hash_map_walk_entries`
   (15284-15287) caps a bucket chain at `1<<20` and `tree_map_walk_entries`
   caps at `visited > (1<<24)` (15516), and `linked_list_walk_items` is bounded
   by the snapshotted `size` (15205-15207). These prevent an infinite loop on a
   corrupt/concurrently-rehashed structure, but a `table` realloc (HashMap
   resize) or a red-black rotation racing the walk can still yield a stale
   `next`/`left`/`right` compressed OOP. `is_valid_pointer` + `klass_from_oop`
   gate dereferences so it won't crash, but the returned vector can contain
   duplicates or miss entries. Documented as O(N) snapshot semantics; worth a
   note that it is **not** atomic w.r.t. a live mutator thread.

4. **[low] `size()` returns `0` indistinguishably for "empty" and "no klass /
   no size() method".** `collection::size()` (14742-14750) returns 0 when
   `get_method_by_oop_klass("size")` is nullopt; `is_empty()` (14760-14763)
   then reports `true` for a *broken* wrapper exactly as for a genuinely empty
   one. Same on the map branch (15079-15092, though it has the extra `size`
   field fallback). This is the documented null-safe contract, but a caller
   cannot tell a null/garbage OOP from an empty container — a latent foot-gun
   the tests must pin so it never silently changes to e.g. `-1`.

5. **[low] `map` duplicates ~70 lines of `collection`'s klass helpers
   (15002-15073 vs 14666-14734) — drift hazard, not a live bug.** The two
   copies of `get_field_by_oop_klass` / `get_method_by_oop_klass` are
   byte-identical today; a future fix to one (e.g. the inherited-static mirror
   path) that misses the other would diverge silently. No behaviour bug now;
   flagged so a regression test locks parity.

Subtle hazards that are **correct today** but fragile (worth pinning, not bugs):
- **Compressed-OOP-only reads.** Every helper reads a `std::uint32_t` at the
  field offset and `decode_oop_pointer`s it (e.g. 15301-15309, 15463-15466).
  This is hard-wired to **compressed oops**; on a JVM with
  `-XX:-UseCompressedOops` (or a >32 GB heap) the field is a full 8-byte
  pointer and these 4-byte reads decode garbage. The `is_valid_pointer` gate
  keeps it from crashing (it returns empty/`nullptr`), but the whole type-tag
  mapping is silently a no-op without compressed oops.
- **Hard-coded field names** (`elementData`, `first`, `item`, `next`, `map`,
  `m`, `table`, `key`, `value`, `root`, `left`, `right`, `size`) — these are
  HotSpot/OpenJDK internal field names, not JLS-stable. Any JDK that renames a
  field (or a non-HotSpot VM) drops to the empty path with no error (see JDK
  sensitivities below).
- **`tree_map_walk_entries` uses a heap `std::vector<void*>` stack** (15440)
  to avoid C++ stack overflow on deep trees — good — but it `reserve(32)` then
  grows unbounded; a pathological tree depth is bounded only by the
  `visited > 1<<24` entry cap, not the stack size.

## Exhaustive test angles

A dedicated pure-logic test already exists: **`tests/test_collection_type_tags.cpp`**
(304 lines, no JVM loaded). What it asserts today:

1. **Tags are distinct C++ types** (`test_type_tags_are_distinct`, lines 91-102)
   — 9 `!std::is_same_v` checks (collection≠list≠set≠map≠hash_map≠linked_list,
   etc.). Catches a regression that collapses two tags into a typedef.
2. **Inheritance lattice** (`test_inheritance_lattice`, 112-122) — list/set are
   `collection`, linked_list is list (transitively collection), hash_map is
   map, and collection⊥map both directions.
3. **Null construction + noexcept** (`test_default_null_construction`, 131-153)
   — every tag builds from `nullptr`, `get_instance()==nullptr`, and every
   ctor is `noexcept(...)`.
4. **`size()`/`is_empty()` null-safe** (155-179) — all six tags `size()==0`,
   collection+map `is_empty()` true on a null OOP.
5. **`to_vector` empty + never-throw** on collection/list/set/linked_list
   (188-205).
6. **`to_entries` empty + never-throw** on map/hash_map (213-225).
7. **Default `value_t::to_vector`/`to_entries` empty** (227-255) incl. an
   explicit-signature aggregate-init variant.
8. **`value_t` via `field_proxy::get()`** — the real user path
   `get_field(...)->get().to_vector<T>()` — with List- and Map-typed
   signatures, plus cross-shaped calls (to_entries on a List-typed proxy,
   to_vector on a Map-typed proxy) all empty (265-284).

It uses oop-constructible wrapper tags `elem_w`/`key_w`/`val_w` (57-82) because
`to_vector<E>()` instantiates `std::make_unique<E>(oop_t)` (a plain `int`
won't compile) — the same pattern as `test_api_surface.cpp`.

**What is still MISSING (the gap the next test wave should close):**

- **The `"[L…;"` raw-object-array branch of `value_t::to_vector` is untested
  here.** Lines 15659-15683 are the highest-value, most-recently-fixed code
  (Fix #1, ec1c8a8) and the current test only ever exercises the null/empty
  path. Add a `value_t` whose `signature == "[Ljava/lang/Object;"` and a
  *non-zero but invalid* compressed OOP, asserting it returns empty without
  faulting — and ideally a JVM module that builds a real `Object[]` field and
  checks element count + null-slot preservation.
- **Signature-shape boundary matrix on the empty path**, with no JVM, asserting
  every entry point returns empty *and* logs nothing fatal for: `""` (empty
  signature), `"["` (length-1, fails the `>=2` guard at 15659), `"[I"`
  (primitive array — must NOT take the object-array branch), `"[[I"`,
  `"[Ljava/lang/String;"`, `"L…;"`, `"I"`, and a `signature` containing
  embedded NULs / non-ASCII bytes (UTF-8 robustness of the `front()`/`[1]`
  reads). Pin that `signature.size() >= 2u && front()=='[' && (sig[1]=='L' ||
  sig[1]=='[')` is the exact gate.
- **value_t holding a non-`u32` alternative.** Construct `value_t` with each
  variant alternative (`bool true`, `int64`, `double`, `u16`, etc.) and assert
  `to_vector`/`to_entries` return empty — the `static_cast<uint32_t>(*this)`
  conversion narrows them, and only `false`→0 is currently exercised. A
  `bool{true}` narrows to compressed OOP `1` → `decode_oop_pointer(1)` must be
  rejected by `is_valid_pointer` → empty (verify, don't assume).
- **`is_empty()` vs broken-wrapper distinguishability** (Flaw #4) — pin that a
  null OOP yields `size()==0`/`is_empty()==true` so the contract can't silently
  flip to a sentinel.
- **map/collection helper parity** (Flaw #5) — a compile-time or behavioural
  assertion that map and collection resolve the same field/method on an
  identical OOP shape, so the duplicated helpers can't drift.
- **Substitutability proof** — that a `std::unique_ptr<vmhook::linked_list>`
  binds where a `std::unique_ptr<vmhook::collection>` is expected (the whole
  point of the lattice), e.g. via a templated function taking
  `const collection&`. Currently only `is_base_of_v` is checked, not actual
  reference binding / slicing-free call-through.
- **Live behavioural coverage already lives in** `tests/jvm/modules/
  collection_list.cpp`, `collection_set.cpp`, `collection_map.cpp`,
  `collection_linked_list.cpp`, `collection_hash_tree_map.cpp` — when extending
  the type-tag tests, assert here only the *type-surface + null/empty/never-
  throw* invariants and leave real-content walks to those modules to avoid
  duplication.

## Known JDK-version sensitivities

- **Field-name stability is the whole feature's load-bearing assumption.** The
  cascade matches `elementData`/`size` (ArrayList), `first`/`item`/`next`
  (LinkedList.Node), `map` (HashSet), `m` (TreeSet), `table`/`key`/`value`/
  `next` (HashMap.Node & TreeNode), `root`/`left`/`right` (TreeMap.Entry).
  These are OpenJDK private field names, stable across **8 / 11 / 17 / 21** for
  the listed containers (the comment at 15407 calls TreeMap.Entry stable "since
  Java 1.2"), but they are **not** a spec guarantee — a JDK that renames any of
  them, or a non-HotSpot VM (J9/OpenJ9, GraalVM substrate), silently drops to
  the empty path with no diagnostic.
- **Java 8 vs 9+ String/compact-strings is irrelevant here** (these walks read
  references, not String bodies), **but** the `read_java_string` reached only
  if a wrapper later reads a String field is JDK-sensitive elsewhere — out of
  scope for the type-tag mapping.
- **HashMap treeification (JDK 8+).** A bucket with ≥8 colliding entries
  converts `HashMap$Node`→`HashMap$TreeNode`. `hash_map_walk_entries`/`_keys`
  rely on TreeNode exposing the same `key`/`value`/`next` field names as Node
  (true on 8/11/17/21, noted at 15243-15247) so the `next` chain still visits
  every entry — if a future JDK changes TreeNode's field layout the treeified
  buckets would be under-walked.
- **Compressed-oops default** (heaps < ~32 GB, all JDK 8-26 defaults) is
  *required*: every helper does a 4-byte `uint32` read + `decode_oop_pointer`.
  With `-XX:-UseCompressedOops` or a huge heap the type-tag mapping returns
  empty across the board (see hazards).
- **Java 21+ / 26 sequenced collections and value/identity changes:**
  `SequencedCollection`/`SequencedMap` (JEP 431, JDK 21) did not alter the
  backing field layout of ArrayList/LinkedList/HashMap/TreeMap, so the cascade
  is unaffected through 21. For **JDK 26** the risk is any record-/value-class
  ("Valhalla") flattening of node/entry objects or a field rename — both would
  break the hard-coded offsets/names silently; a JDK-26 live module should
  re-verify each fast path actually fires (non-empty result on a populated
  container) rather than trusting the empty-path tests.
