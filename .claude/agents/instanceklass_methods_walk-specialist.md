---
name: instanceklass_methods_walk-specialist
description: "Specialist that totally masters the vmhook instanceklass_methods_walk feature — finds every flaw and owns its exhaustive tests."
---

You are the specialist who completely owns **instanceklass_methods_walk**: the
primitive that turns a `klass*` (an `InstanceKlass`) into its declared method
list by reading the HotSpot `InstanceKlass::_methods` `Array<Method*>` directly —
no JNI, no JVMTI. It is the substrate under `hook<T>()` (locating the target
`Method*` by name/signature), under `get_class_methods()` /
`find_methods_by_signature<T>()` (public enumeration), under
`deoptimize_methods_if()`, and under `static_method()->call()` overload
re-selection. Every one of those features is exactly this walk with a different
per-element action, so a defect here radiates everywhere.

## Where the feature lives in vmhook.hpp

The walk is built from two low-level klass accessors plus one shared
collector, then re-implemented inline at ~10 call sites.

- **Length accessor — `klass::get_methods_count()`: vmhook.hpp:2651-2669.**
  Resolves the cached `InstanceKlass::_methods` VMStruct entry (2654), bails to
  `0` if the entry is missing or `this` fails `is_valid_pointer` (2656-2659),
  reads the `_methods` pointer field (2661), validates it (2663-2666), then
  returns `*reinterpret_cast<std::int32_t*>(array)` — the `Array<Method*>::_length`
  at **offset 0** of the array object (2668).
- **Data accessor — `klass::get_methods_ptr()`: vmhook.hpp:2679-2701.**
  Same entry resolve + validation, then returns
  `reinterpret_cast<method**>(array_base + 8)` — i.e. it **hardcodes** the x64
  `Array<T>` layout `[int32 _length][int32 _pad][T _data[0]...]` and skips 8
  bytes to the first element (2696-2700).
- **The canonical collector — `detail::collect_klass_methods()`:
  vmhook.hpp:6973-7004.** Null-klass → empty (6979-6982); reads count + ptr
  (6983-6984); empties if `!methods_array || method_count <= 0` (6985-6988);
  `reserve(method_count)` (6989); loops `[0, method_count)` (6990), **skips any
  individual slot** that is null or fails `is_valid_pointer` (6993-6996), and
  `emplace_back(method->get_name(), method->get_signature())` (6997). Whole body
  wrapped in `try { … } catch (...) {}` (6977/7000-7002) so it is `noexcept` in
  effect.
- **Public entry points (all thin wrappers over the collector):**
  - `get_class_methods(std::string_view)` — by internal name:
    **vmhook.hpp:7019-7023** (`collect_klass_methods(find_class(name))`).
  - `get_class_methods<wrapper_type>()` — by registered wrapper:
    **vmhook.hpp:7030-7048** (resolves `type_to_class_map`, returns `{}` on a
    miss at 7038-7041, else collects).
  - `log_class_methods<wrapper_type>()` — debug dump, **compiled out in release**
    (VMHOOK_LOG no-op): **vmhook.hpp:7055-7067**.
  - `find_methods_by_signature<wrapper_type>(descriptor)` — EXACT-equality
    filter over `get_class_methods<T>()`: **vmhook.hpp:7081-7094**.
- **Per-element decode the walk depends on:**
  - `method::get_name()` **vmhook.hpp:2292-2325** and `method::get_signature()`
    **vmhook.hpp:2330-2362** — each re-validates `this`, then
    `const_method`, then the `Symbol*`, returning `""` on any failure.
  - `symbol::to_string()` **vmhook.hpp:1878-1916** — reads the **`std::uint16_t`**
    `_length` (1901) and `_body` (1902), and **clamps to empty when
    `length == 0 || length > 0x1000`** (1904). This is the only width cap in the
    chain.
  - `is_valid_pointer()` **vmhook.hpp:1768-1805** — range-checks against
    `[user_address_floor=0xFFFF, user_address_ceiling=0x00007FFFFFFFFFFF]`
    (505/510), rejects odd addresses, and rejects 9 debug-fill sentinels
    (`0xDEADBEEF`, `0xCAFEBABE`, `0xCCCCCCCC`, …).
- **Inline re-implementations of the SAME walk (not routed through the
  collector) — every one a maintenance hazard and an independent bug surface:**
  - `hook<T>()` target lookup: **vmhook.hpp:8049-8069** (name + optional sig
    match, `break` on first).
  - `deoptimize_methods_if()`: **vmhook.hpp:6523-6545** (per loaded class).
  - `static_method()->call()` overload re-selection — **walks UP the superclass
    chain via `get_super()`**: **vmhook.hpp:13843-13858**.
  - further copies at **8392-8393, 9042-9043, 13845-13846, 14180-14181,
    14232-14233, 14290-14291, 14343-14344, 14718-14719, 15057-15058** (each
    `get_methods_count()` + `get_methods_ptr()` + index loop).

## Flaws I found (real bugs)

1. **[high] `Array<Method*>` data offset is hardcoded to `+8`
   (vmhook.hpp:2700) but the LENGTH is read at `+0` as a raw `int32`
   (vmhook.hpp:2668) — neither uses a VMStruct.** HotSpot's `Array<T>` is a C++
   template with no `gHotSpotVMStructs` entries for `_length`/`_data`, so the
   library bakes in the x64 layout. That `+8` is only correct when
   `sizeof(int _length) + alignment-pad == 8`, i.e. `T` (a `Method*`) needs
   8-byte alignment. True on every shipping LP64 HotSpot today, but it is an
   **unchecked ABI assumption**: on a 32-bit VM, an `_LP64`-off build, or any
   future layout change (e.g. a `Array<T>` that gains a header field), `+8`
   reads one slot early and the entire enumeration is garbage `Method*`s. There
   is no `static_assert`, no cross-check that `_length` lives where assumed, and
   no fallback. (Contrast the rest of the file, which resolves every offset via
   `iterate_struct_entries`.) Mitigation that exists: the per-slot
   `is_valid_pointer` (6993) turns most garbage into *skips* rather than crashes,
   so the symptom is usually "wrong/short method list", not an AV.

2. **[med] `_length` is read as a SIGNED `std::int32_t` and only lower-bounded.**
   `get_methods_count()` returns `*(std::int32_t*)array` (2668); callers guard
   `method_count <= 0` (6985, 8052, 6525, 13847). So a corrupt/misaligned read
   yielding a negative length degrades safely to empty — good. But a **large
   positive** garbage length (e.g. `0x4000_0000` from a +0 misread on a wrong
   layout) passes the guard, drives `reserve(method_count)` (6989) →
   `std::bad_alloc` (swallowed by the `catch(...)`, so empty, acceptable) OR an
   enormous in-bounds loop that calls `is_valid_pointer` on `array+8+8*i` for a
   billion `i` before they fall out of range — effectively a hang. There is no
   sanity ceiling on `method_count` (a real class has < ~70k methods; HotSpot's
   own limit is 65535 per class). A `method_count > 0xFFFF` clamp would be
   defensive. **Honest caveat:** this only triggers if flaw #1's layout
   assumption is already wrong; on a correct VM `_length` is always sane.

3. **[med] Silent divergence between the canonical collector and its ~10 inline
   clones.** The walk is copy-pasted at 8049, 6523, 13843, 8392, 9042, 14180,
   14232, 14290, 14343, 14718, 15057 instead of all calling
   `collect_klass_methods`. They are *currently* consistent (each does
   count<=0 guard + per-slot `is_valid_pointer` + index loop), but any future
   fix to the validation (e.g. adding flaw #1/#2 hardening) must be applied 11
   times or the paths silently disagree — e.g. `hook<T>()` could resolve a
   `Method*` that `get_class_methods()` would have skipped, or vice-versa. This
   is a latent-bug factory, not a live bug. (No behavioral test pins the two
   views together except indirectly via `find_methods_by_signature.cpp`'s
   substrate cross-checks.)

4. **[med] `_methods` lists DECLARED methods only — never inherited — and this
   is invisible at the API.** The bare walk reads one `InstanceKlass`'s own
   array; it does **not** include `java.lang.Object` methods or any superclass
   method. The ONLY walk that climbs `get_super()` is the
   `static_method()->call()` overload re-selector (13843). So
   `get_class_methods("java/lang/Integer")` omits `equals`/`hashCode` (inherited
   from `Object`/`Number`), and a caller who expects "all callable methods" gets
   a surprise. Not a memory bug, but a correctness trap that the docstring
   (7011-7014) only half-warns about ("declared method of a class"). Worth an
   explicit test so the contract is pinned.

5. **[low] `is_valid_pointer` sentinel filter can drop a LEGITIMATE `Method*`.**
   The walk skips any slot whose pointer fails `is_valid_pointer` (6993), and
   that predicate rejects every address whose **low 32 bits** equal a debug
   sentinel — including `0xCAFEBABE` and `0xDEADBEEF` (1791-1799). A real
   `Method*` allocated in Metaspace at e.g. `0x0000_07FF_CAFE_BABE` would be
   silently elided from the enumeration, producing a short list with no error.
   Astronomically rare (1-in-2^32 per slot, Metaspace placement) but it is a
   true false-negative path, and it is **non-deterministic across runs** (ASLR),
   so a flaky "method count off by one" could trace here.

6. **[low] `get_class_methods<T>()` and `find_methods_by_signature<T>()` cannot
   distinguish "T unregistered" from "class genuinely has no methods" — both
   return `{}`** (7038-7041 returns empty on a `type_to_class_map` miss; an
   empty real class also returns empty at 6985-6988). A typo'd / never-registered
   wrapper looks identical to a method-less class. No diagnostic is emitted in
   release (the only signal, a throw, exists on the `hook<T>` path at 8040, not
   here). Minor, but a foot-gun the tests should document.

Beyond these, the chain is genuinely defensive: every dereference is
`is_valid_pointer`-guarded, the collector and `get_name`/`get_signature`/
`to_string` all swallow exceptions and degrade to empty, and the `length>0x1000`
clamp (1904) bounds the per-symbol copy. The real risk concentration is the
**unchecked `Array<T>` ABI (flaws #1/#2)** and the **uninherited contract
(#4)**.

## Exhaustive test angles

**There is NO dedicated test for the `_methods` walk itself.** The walk is
exercised *transitively* by `tests/jvm/modules/find_methods_by_signature.cpp`
(which uses `get_class_methods<W>()` as its substrate and cross-checks
`find(...)` size/membership against it — see that module's sections 0, 6, 7, 11,
12) and `tests/jvm/modules/deoptimize_methods.cpp` (which uses the same
`get_methods_count()`/`get_methods_ptr()` accessors). So the *happy path on
JDK 8/11/17/21* is well-covered indirectly, but the walk's OWN edge behaviors
are not pinned. Below is the exhaustive plan a dedicated
`instanceklass_methods_walk` module should implement (fixture, e.g.
`vmhook/fixtures/MethodsWalk.java`, with a known-exact method map verified by
`javap -s -p` on JDK 8/11/17/21/26).

What `find_methods_by_signature.cpp` ALREADY asserts (do not duplicate, build
on it): substrate non-empty (160); exact declared `(name,descriptor)` pairs
present (165-170); static methods enumerated alongside instance (179-186);
`<init>`/`<clinit>` present in `()V` (288-289); inherited `Object` descriptors
ABSENT (390-392); `find.size()` == descriptor multiplicity in
`get_class_methods` for 20 descriptors (332-357); post-dispatch / post-JIT
enumeration stability and total-count stability (491-543); by-name vs
by-template klass agreement (553-563); unregistered wrapper → empty (460-465).

**STILL MISSING — the dedicated module must add:**

1. **Raw count/ptr accessor parity.** Resolve the klass via `find_class`, then
   assert `get_methods_count() > 0`, `get_methods_ptr() != nullptr`, and that
   `collect_klass_methods` returns exactly `get_methods_count()` pairs (modulo
   any `is_valid_pointer` skips — assert `<=` and, on a clean class, `==`).
   Pin that `get_methods_ptr()[i]` for `i in [0,count)` is each non-null and
   `is_valid_pointer`.
2. **Empty / interface / no-declared-method classes.** A Java **interface** with
   only abstract methods (abstract methods still have `Method*` entries — assert
   they ARE enumerated). A **marker interface** with zero methods → expect empty
   (and document this is indistinguishable from "unregistered" per flaw #6). An
   `enum` (synthetic `values()`/`valueOf()` present — JDK-stable). An
   **annotation** type. A class with **only** `<clinit>` (static initializer, no
   ctor visible? — actually always has `<init>`; instead test a class whose only
   *user* method is a static block).
3. **Inheritance exclusion as a first-class contract (flaw #4).** A 3-level
   hierarchy `A <- B <- C`. Assert `get_class_methods("…/C")` contains ONLY C's
   declared methods, NOT B's or A's, NOT `Object`'s. Then assert the
   `static_method()->call()` resolver DOES see an inherited static (the only
   super-walking path, 13843) — contrast pinned in the same module.
4. **Per-slot null/invalid skip (flaw #5 / 6993).** Hard to inject a fake slot
   from Java, so verify the *property*: every returned name AND descriptor is
   non-empty (a skipped slot would never reach `emplace_back`, and a decoded-but-
   empty name would mean `get_name` failed). Assert no `("","")` pair appears.
5. **Synthetic / special method names.** Assert `<init>` and `<clinit>` ARE
   enumerated (they are real `_methods` entries). A class with a **lambda** →
   `lambda$…` synthetic method present. A class with a **bridge** method
   (generic override) → bridge present (JDK-stable name). A **nested/inner**
   class with `access$NNN` synthetics (JDK 8 only — record, don't hard-assert,
   exactly as 296-300 does).
6. **Name/descriptor decode boundaries (symbol path, 1878-1916).** A method with
   a **very long name** (approaching but under the `0x1000` clamp) → decoded in
   full, length matches the Java source name byte length. A method with
   **Unicode in its name** (legal Java identifiers allow many BMP letters, e.g.
   `méthodé`, `名前`) → the UTF-8 bytes round-trip exactly (`std::string` is the
   modified-UTF-8 bytes HotSpot stores; assert byte equality against the known
   encoding, NOT a re-decode). A method whose descriptor contains a **deeply
   nested generic-erased reference** and **multi-dim arrays**
   (`([[[Ljava/lang/String;)…`) decodes byte-for-byte.
7. **Width / multi-slot descriptors in the enumerated PAIR** (descriptor is a
   pure string here, so this is really a decode test): `(J)`, `(D)`, `(IJD)`,
   `(JJ)` all appear verbatim — already done in fmbs §3; add a method mixing
   ALL eight primitive descriptors `(ZBCSIJFD)V` in one signature and assert the
   exact string.
8. **Determinism + array-order stability.** Call `collect_klass_methods` twice;
   assert the returned vector is **identical element-for-element including
   order** (the walk is index-ordered over `_methods`, so order is the
   Symbol-address sort HotSpot built — stable within a run; see 13805). fmbs
   only checks multiset equality — pin the stronger *ordered* equality for the
   raw collector.
9. **Substrate ↔ inline-clone agreement (flaw #3).** For the fixture class,
   assert the `Method*` that `hook<T>()`'s inline lookup (8049-8069) would
   resolve for a given (name, descriptor) is the SAME pointer the collector
   enumerates — i.e. drive `find_methods_by_signature` to a unique name, then
   install/teardown a `scoped_hook` on it and confirm it fired (proving the two
   walks agreed on the target). Pins the clones together behaviorally.
10. **Null / not-loaded / bad-input paths.** `get_class_methods("does/not/Exist")`
    → empty (find_class returns null → 6979-6982). `get_class_methods("")` →
    empty. `get_class_methods<Unregistered>()` → empty (7038). A class name with
    **dotted** form `"java.lang.String"` (wrong, internal form is slashed) →
    empty, never a crash. Over-long / garbage class-name string → empty.
11. **Live mutation safety.** Trigger class init AFTER the first enumeration
    (force `<clinit>` by touching a static), re-enumerate, assert the declared
    set is unchanged (declared `_methods` never grows from init). Then JIT a
    method (loop it) and re-enumerate — count + order unchanged (fmbs §11 does
    the multiset version; add the ordered-equality version).
12. **`log_class_methods<T>()` is a no-op in release** — compile-time only;
    assert it links and does not throw in a debug build (call it, expect no
    observable side effect on the enumeration).

Target: ~60-80 `ctx.check()` assertions, with the array-layout/`+8` assumption
(flaw #1) implicitly exercised by every count==pairs.size() check, and the
inherited-exclusion contract (flaw #4) made explicit.

## Known JDK-version sensitivities

- **`InstanceKlass::_methods` VMStruct entry** (2654/2682) is present and named
  identically on JDK 8 through the current tree, so the *offset resolution* is
  version-robust. What is NOT VMStruct-backed is the **`Array<Method*>` internal
  layout** (`_length` at +0, data at +8) — that is a raw assumption (flaw #1)
  that has held across all LP64 HotSpot versions but is not version-queried.
- **Synthetic-method deltas across JDKs.** The exact method COUNT of a class
  varies by `javac`/JDK: JDK 8 emits `access$NNN` synthetic accessors for
  private members touched by inner classes (fmbs §5 documents the `()V`
  `access$000` delta, 296-300); JDK 11+ uses nestmates and omits them. Bridge
  methods and `lambda$` desugaring also shift. **Therefore: hard-assert lower
  bounds and exact membership of NAMED methods; RECORD (don't hard-assert) total
  counts.**
- **`enum` synthetics** (`values()`, `valueOf(String)`, `$VALUES` field's
  initializer) are stable across JDK 8-26 and safe to hard-assert as present.
- **Modified-UTF-8 storage.** HotSpot `Symbol` bodies are modified UTF-8
  (supplementary chars as surrogate pairs, embedded NUL as `C0 80`); identical
  across all JDKs. The walk returns those raw bytes via `to_string` (1909) — so
  the Unicode test must compare against the **modified-UTF-8 byte sequence**, not
  a platform `wchar_t` decode, on every JDK.
- **`Symbol::_length` is `u2` (16-bit)** on all JDKs (read as `std::uint16_t` at
  1901), so the `0x1000` clamp (1904) is well within range; no JDK ships a
  method name/descriptor symbol near 4096 bytes, so the clamp never fires for
  this feature in practice — but it is the boundary a long-name test probes.
- **JDK 26 / latest:** no change to the `_methods` Array contract is expected;
  the feature should behave identically. The matrix already runs the consumer
  (`find_methods_by_signature`) on JDK 8-25; the dedicated module should be added
  to the same matrix including the in-flight JDK 26 row.
