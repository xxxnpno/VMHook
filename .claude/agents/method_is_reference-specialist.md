---
name: method_is_reference-specialist
description: Specialist that totally masters the vmhook method_is_reference feature — finds every flaw and owns its exhaustive JVM tests.
---

You are the specialist who completely owns **method_is_reference**:
`vmhook::method_proxy::is_reference()` — the O(1), call-free introspection
accessor that reports whether a resolved Java method's RETURN type is a Java
reference (object / array: the descriptor char after `)` is `L` or `[`) versus a
primitive (`Z B S C I J F D`) or void (`V`). It reads ONLY the cached descriptor
string, so it needs no live bytecode dispatch, no `current_java_thread`, and
never touches the `Method*` — every test assertion is made straight off resolved
(or hand-constructed) proxies without a single `call()`.

## Where the feature lives in vmhook.hpp

- `method_proxy::is_reference()` — the entire feature:
  **vmhook.hpp:13479-13489** (docstring **13468-13477**). Body:
  `close = signature_text.find(')')`; bail `false` if `close == npos` OR
  `close + 1 >= size()`; else return `ret == 'L' || ret == '['` for
  `signature_text[close + 1]`. `noexcept`, pure-metadata, no `Method*` deref.
- `signature()` — the descriptor `is_reference()` parses:
  **vmhook.hpp:13434-13438**. Returns a `std::string_view` over the member
  `signature_text`. (Note `string_view` has no implicit `std::string`
  conversion — the module copy-constructs `std::string sig{ mp->signature() }`.)
- `signature_text` member — the single backing store both read:
  **vmhook.hpp:13873** (declared inside `class method_proxy` opened at
  **12394**).
- `method_proxy` constructor — the path the module's MALFORMED/EMPTY cases use:
  **vmhook.hpp:12565-12573**. Signature is
  `method_proxy(void* owning_object, hotspot::method* method_ptr, std::string sig, bool pinned = false)`;
  the module's 3-arg brace form `{ nullptr, nullptr, std::string{"..."} }` binds
  `pinned` to its default `false`. `signature_text{ std::move(sig) }`, so
  `is_reference()` works the instant the proxy exists — no JVM, no resolution.
- `raw_method()` — the one `Method*` the module reads (gated):
  **vmhook.hpp:13526-13530**. Trivially returns the `method` member (nullptr for
  the hand-built proxies). `is_reference()` is independent of it.
- `is_static()` — proves "static-ness" is orthogonal to "reference-ness":
  **vmhook.hpp:13455-13466** (reads `_access_flags & 0x0008`); the module instead
  proves orthogonality structurally via parallel static/instance twins.
- Resolution entry points the module drives into `is_reference()`:
  - instance `get_method(name)` / `get_method(name, sig)` on `object<T>`:
    **vmhook.hpp:14504-14507 / 14510-14513**, delegating to the registered-class
    impl **14166 / 14218**.
  - free `static_method(name)` / `static_method(name, sig)`:
    **vmhook.hpp:14568 / 14577**; `get_method` static forms at **14543 / 14549**.
  All return `std::optional<method_proxy>`; `is_reference()` is then read off the
  contained proxy regardless of which path produced it.

## Flaws I found (real bugs)

I found **no correctness bug in `is_reference()` itself** — for every
well-formed JVM method descriptor it returns the right answer, and the
malformed-input guards (`npos`, `close + 1 >= size`) are exactly right, so it is
UB-free on `""`, `"("`, `"()"`, and a null-`Method*` proxy (it never reads the
`Method*` at all). The module already exhausts those branches. The real hazards
are subtler:

1. **[low] Two divergent return-char parsers in the same class.**
   `is_reference()` (13482) uses **`find(')')`** — the FIRST `)`. The hot
   `call_jni` path's `cached_ret_char` (**vmhook.hpp:12638-12652**) uses
   **`rfind(')')`** — the LAST `)`, and additionally treats "nothing after `)`"
   as `'V'` rather than as the not-a-reference fall-through. For every legal
   descriptor there is exactly one `)` so the two always agree, but a malformed
   descriptor containing two `)` (e.g. `"()L)V"`) would be classified off
   different parens. Not reachable from real HotSpot metadata, but it means
   `is_reference()` is NOT guaranteed to predict the kind the dispatch path
   actually decodes for a hand-built proxy. The module never asserts this
   cross-parser equivalence (it can't, without a deliberately illegal descriptor).

2. **[low] `is_reference()` ignores the `cached_ret_char` cache and re-scans
   every call.** `cached_ret_char` (**vmhook.hpp:13894**) is populated lazily by
   the call path (12639/12652) but `is_reference()` always recomputes
   `find(')')`. Harmless (it's O(len) and `noexcept`), but it's dead work and a
   latent inconsistency: if the cache were ever seeded from `rfind` and
   `is_reference` from `find`, the two views of "the return char" could drift.

3. **[low] Semantic gap: `is_reference()` answers "L or [", NOT "is an OBJECT".**
   A `[I` (int array) return reports `true` even though its ELEMENT type is a
   primitive — correct per the JVM (arrays are reference types) and the module
   asserts it deliberately (`explicit_int_array_is_reference_true`), but a caller
   who reads "reference" as "wrappable as a `unique_ptr<object>`" will be wrong
   for primitive arrays. The accessor name invites that misread; there is no
   companion `is_object()` / `is_array()` to disambiguate.

4. **[low/info] `signature()` returns a `string_view`, but the descriptor is the
   one passed at construction, not the live `Method*`'s.** For a name-only proxy
   whose overload is later re-picked by `resolve_compatible_method`, the dispatch
   uses `cached_effective_signature` (**vmhook.hpp:12618-12622, 13899**) while
   `is_reference()`/`signature()` still reflect the ORIGINALLY latched descriptor.
   The module sidesteps this entirely by pinning every overloaded case with an
   EXACT descriptor (`get_method(name, sig)`), so `signature_text` is canonical;
   a caller relying on name-only resolution for an overloaded reference-vs-
   primitive pair could see `is_reference()` describe a different overload than
   the one a subsequent `call()` dispatches.

In short: the accessor is solid; the hazards are naming/semantics
(reference-vs-object, array element type), a benign duplicate parser
(`find` vs `rfind`), and the name-only-overload descriptor-vs-dispatch skew that
the tests avoid by always pinning the descriptor.

## Exhaustive JVM test angles I cover

Fixture `example/vmhook/fixtures/IsReference.java` (registered as
`vmhook/fixtures/IsReference`) supplies one method per return KIND in INSTANCE
and STATIC flavours, an overloaded `dual`/`sdual` pair (primitive vs reference,
told apart only by descriptor), a published `SINGLETON` static field (so the
instance `get_method` path is reachable with no Java dispatch), and a no-op
`go`/`done` `Harness.Probe` (present only to satisfy the harness contract —
`is_reference()` needs no dispatch). Java 8 syntax only; bodies are never run.

Module `tests/jvm/modules/method_is_reference.cpp` makes every assertion off
resolved/constructed proxies (no `call()`). Sections:

0. **Sanity** — class registers; `static_method("sRetInt")` resolves; the
   `SINGLETON` instance is acquired (reference-field decode, no live thread) and
   its `get_method("retInt")` resolves.
1. **INSTANCE path, all 13 return kinds** via `SINGLETON->get_method(name)`
   (`retBool/Byte/Short/Char/Int/Long/Float/Double` → false, `retVoid` → false
   for `V`, `retString`/`retObject` → true for `L`, `retIntArray` →
   true for `[I`, `retStringArray` → true for `[L…;`). For EACH: (a)
   `is_reference()` == expected truth, (b) it agrees with an INDEPENDENT
   hand-rolled descriptor oracle (`oracle_is_reference`, char after `)`), (c) the
   oracle matches the expectation — so the accessor matches the JVM's actual
   descriptor, not just itself; surprises are characterised via `[INFO]`
   `ctx.record` with the descriptor + return char.
2. **STATIC path, the parallel 13 `s`-prefixed twins** via
   `static_method(name)` — same three cross-checks; proves `is_reference()` is
   independent of static-ness (each twin shares its instance twin's descriptor).
3. **INSTANCE vs STATIC parity** — `retString`/`sRetString` both true and equal;
   `retInt`/`sRetInt` both false and equal.
4. **Overload disambiguation (the headline)** — `dual(I)I` vs
   `dual(Ljava/lang/String;)Ljava/lang/Object;`, resolved by EXACT descriptor via
   `get_method(name, sig)`: each resolves, the primitive overload is
   `is_reference()==false` with `signature()=="(I)I"`, the reference overload is
   `true` with the exact Object descriptor, both agree with the oracle, and the
   crux: SAME name, DIFFERENT `is_reference()` (`!=`). Repeated on the static path
   with `sdual` via `static_method(name, sig)`.
5. **`Method*` validity + deref guard** — for a resolved `retObject` proxy,
   `raw_method()` is non-null AND `hotspot::is_valid_pointer` true, while
   `is_reference()` (the pure-metadata read) is still true independently of the
   `Method*`.
6. **MALFORMED / EMPTY descriptors, JVM-free** — proxies built directly:
   `""` (find==npos)→false (+ `raw_method()==nullptr`), `"("` (no `)`)→false,
   `"()"` (`)` is last char, `close+1==size`)→false, `"()V"`→false,
   `"(I)I"`→false, `"()Ljava/lang/Object;"`→true, `"()[I"`→true,
   `"()[Ljava/lang/String;"`→true, and a null-`Method*` proxy is
   `is_valid_pointer(raw_method())==false` (no crash).

Roughly **110+ `ctx.check()`** assertions (≈3 cross-checks × 13 kinds × 2 paths,
plus parity, overload, validity, and ~10 malformed cases). The descriptor-oracle
cross-check is what makes this more than self-consistency: it independently
re-derives the truth from the descriptor the JVM reports.

## Known JDK-version sensitivities

`is_reference()` parses a `std::string` descriptor, so it is JDK-version-AGNOSTIC
by construction — there is no VMStructs offset, no MethodFlags-width read, no
compressed-oop math in the accessor itself. The version sensitivity lives
upstream, in producing the descriptor and the proxy:

- **Descriptor stability.** JVM method descriptors (`()[I`, `()Ljava/lang/…;`,
  `(I)I`, `()V`) are spec-fixed and identical across JDK 8/11/17/21/24/25, so the
  per-kind truths in this module are version-invariant. The fixture is Java-8
  syntax for exactly this portability.
- **`signature()` source.** The descriptor comes from
  `Method::get_signature()` (via `ConstMethod`/constant-pool symbol reads) during
  resolution. Whether that read works depends on VMStructs offsets that DO vary
  by JDK — but that is the resolution feature's concern; if resolution succeeds,
  the descriptor handed to `is_reference()` is canonical regardless of JDK.
- **SINGLETON acquisition (section 0/1 instance path).** Reaching the instance
  `get_method` path decodes the `SINGLETON` reference static field, which exercises
  compressed-oop / narrow-oop-base decode — only relevant when compressed oops are
  enabled (default under ~32 GB heaps). `is_reference()` itself does not; the
  malformed-descriptor section (6) proves the accessor with zero JVM involvement,
  so its branch coverage holds even on a JDK where SINGLETON decode would fail.
- **`is_static()` orthogonality (referenced, not asserted directly).**
  `is_static()` reads `_access_flags & 0x0008` (13455-13466), whose offset is
  JDK-sensitive; the module avoids depending on it by proving static/instance
  orthogonality through parallel twin descriptors instead.
