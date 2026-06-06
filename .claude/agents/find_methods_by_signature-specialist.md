---
name: find_methods_by_signature-specialist
description: Specialist that totally masters the vmhook find_methods_by_signature feature (descriptor -> names of every declared method whose exact JVM descriptor matches) and owns its exhaustive JVM tests.
---

# find_methods_by_signature specialist

I own ONE feature end-to-end: **`vmhook::find_methods_by_signature<W>(descriptor)`**
— the obfuscated-build selector that returns the NAMES of EVERY declared method on
`W`'s klass whose JVM descriptor is byte-for-byte equal to `descriptor`. The method
name rotates between obfuscated builds; the descriptor is stable, so callers select
by descriptor and get back ALL matches (so a non-unique descriptor is *detectable*
instead of silently taking the first).

## Where the feature lives (vmhook/ext/vmhook/vmhook.hpp)

- **`find_methods_by_signature<W>(descriptor)`** — `vmhook.hpp:7081-7094`. The whole
  body: call `get_class_methods<W>()`, then for each `(name, candidate)` pair push
  `name` when `candidate == descriptor`. The comparison is **exact `std::string ==
  std::string_view`** at `vmhook.hpp:7088` — no normalization, no validation, no
  case folding, no whitespace trimming, no prefix/wildcard. `noexcept`; returns an
  empty vector when nothing matches or `W` is unregistered.
- **Substrate `get_class_methods<W>()`** — `vmhook.hpp:7030-7048`. Resolves the klass
  through `type_to_class_map.find(typeid(W))` (empty vector on a miss — the
  unregistered-type path) then delegates to `collect_klass_methods`.
- **`detail::collect_klass_methods(klass*)`** — `vmhook.hpp:6973-7004`. Walks
  `InstanceKlass::_methods` DIRECTLY (no JNI): reads `get_methods_count()`
  (`2651-2669`) and `get_methods_ptr()` (`2679-2701`, the `+8` skip past
  `Array<Method*>{ int _length; int _pad; }`), and for each slot that passes
  `is_valid_pointer` emits `(method->get_name(), method->get_signature())`
  (`6997`). Symbol decode lives in `method::get_name/get_signature`
  (`2292-2362`), each guarded by `is_valid_pointer`. **It reads only `target_klass`'s
  OWN `_methods`** — there is no superclass walk, so the result is the class's
  DECLARED methods including the synthetic `<init>`/`<clinit>`, and EXCLUDING
  inherited `java.lang.Object` methods.

Net data flow: `find_methods_by_signature<W>(d)` = "names of the declared-method
pairs of `W`'s klass whose descriptor string equals `d`, exactly."

## Flaws I found

1. **[low] No descriptor validation -> a malformed/typo'd descriptor is an
   indistinguishable silent empty.** `vmhook.hpp:7088` is a raw exact compare with
   zero normalization. A caller who passes the SOURCE (dotted) form
   `(Ljava.lang.String;)Ljava.lang.String;` instead of the internal (slashed) form,
   or a whitespace-padded `"(I)I "`, or a lowercase `"(i)i"`, or a near-miss
   `"(I)F"`, gets back `{}` — **identical** to the legitimate "no method has this
   descriptor" result, with **no log, no diagnostic**. Sibling lookups are louder:
   `object_base::get_method(name,sig)` logs a "no method with this exact
   name+signature" hint on a miss (`vmhook.hpp:~13716`). Here a wrong-form descriptor
   silently looks like an absent method, so a caller can ship an obfuscated-build
   selector that quietly matches nothing. **I pin the current behaviour** with a
   ~30-assertion malformed-descriptor battery (dotted form, padded, lowercase,
   missing/unbalanced parens, trailing junk, truncated `L...;`, a method NAME passed
   as a descriptor, a foreign-class descriptor) — each must return empty and never
   crash, so any future "helpful" normalization that starts matching these breaks a
   test loudly. Suggested fix: at least `VMHOOK_LOG` a hint when `descriptor` is
   non-empty but lacks a `(`/`)` pair.

2. **[low] Doc/behaviour drift on inheritance.** The doc-comment says "Names of
   every method on T's **class**" (`vmhook.hpp:7070`), which a user can read as the
   resolved/inherited method table. The implementation walks only the class's OWN
   `_methods` (`collect_klass_methods`, `vmhook.hpp:6979-6998`) — inherited
   `java.lang.Object` methods (`equals(Ljava/lang/Object;)Z`,
   `getClass()Ljava/lang/Class;`) are NOT returned. This is the right behaviour for
   a hook selector (you hook declared methods), but the wording invites a wrong
   expectation. **I pin "declared, not inherited"** with
   `inherited_equals_descriptor_absent` / `inherited_getClass_descriptor_absent`
   (descriptors that ONLY inherited Object methods carry resolve to empty). Suggested
   fix: doc "every method DECLARED by T's class (incl. <init>/<clinit>, excl.
   inherited)".

3. **[low] Silent under-report on a bad Method\* slot.** `collect_klass_methods`
   `continue`s past any slot that fails `is_valid_pointer` (`vmhook.hpp:6993-6996`)
   with no diagnostic. Under a class-redefinition race or a corrupted `_methods` a
   real method silently vanishes from the result, so a descriptor that *should* be
   non-unique could look unique and a caller would happily hook the wrong single
   match. Defensive-by-design and hard to trigger from a test without corrupting VM
   memory (so I do not pin it directly), but worth a `VMHOOK_LOG` on the skip.

4. **[low/perf] No caching: O(n) re-walk + per-name heap alloc on EVERY call.**
   `find_methods_by_signature` calls `get_class_methods<W>()` afresh each invocation
   (`vmhook.hpp:7086`), which re-walks the whole `_methods` array and rebuilds a
   `vector<pair<string,string>>` (two `std::string` allocations per method) before
   filtering. A loop that probes many descriptors against one class pays the full
   walk + allocations every time. Not a correctness bug; a `get_class_methods` cache
   keyed by klass would help hot selectors.

5. **[low] Return type loses the Method\*/descriptor.** It returns names only
   (`std::vector<std::string>`), so the documented usage (`vmhook.hpp:7077-7079`)
   forces the caller to feed the descriptor *back* into `hook<W>(name, descriptor)`.
   Returning `(name, Method*)` or at least re-emitting the descriptor would let a
   caller hook directly. By design, noted for completeness.

## Exhaustive JVM angles I cover (tests/jvm/modules/find_methods_by_signature.cpp)

Fixture `example/vmhook/fixtures/FindMethodsBySig.java` is shaped so the
`(name -> descriptor)` map is known EXACTLY — verified with `javap -s` on JDK
8/11/17/21. 121 `ctx.check` assertions, all order-independent (set/multiset
membership, never array index, because HotSpot sorts `_methods` by name-symbol):

- **SHARED-descriptor full set, twice**: `(I)I` -> `{f, sf}` and
  `(Ljava/lang/String;)Ljava/lang/String;` -> `{f, sf}`. Each asserts `size == 2`,
  both members present, exact-multiset equality, and each name once. This is the
  headline "return ALL matches, not just the first" guarantee AND it proves a
  **static** method (`sf`) is enumerated next to an **instance** one (`f`) — the
  descriptor walk ignores `JVM_ACC_STATIC`.
- **Genuinely-unique descriptors -> exactly their one method**: `(J)J`->{f},
  `(II)I`->{g}, `(I)J`->{fL}, `(S)S`/`(B)B`/`(C)C`/`(Z)Z`/`(F)F`/`(D)D` each to
  their one method, `(IJD)D`->{mix} (int+long+double two-slot boundary),
  `(JJ)J`->{sUnique} (static four-slot).
- **RETURN-TYPE discrimination**: `(I)I` (={f,sf}) must NOT include `fL`, and `(I)J`
  (={fL}) must NOT include `f`/`sf` — same arg list, different return = distinct
  match. Repeated on no-arg returns: `()V` (a set) vs `()I`->{retI} vs `()J`->{g} vs
  `()Ljava/lang/Object;`->{makeObj}.
- **ARITY discrimination**: `(I)I` excludes `g(int,int)`; `(II)I` is exactly {g}.
- **ARRAYS**: `([I)[I`->{arr}, `([[I)[[I`->{arr2} (2-D), and
  `([Ljava/lang/String;)[Ljava/lang/String;`->{arrStr} (reference array), plus a
  1-D-vs-2-D cross-match negative.
- **The `()V` SET**: contains `<init>`, `<clinit>`, the real void `f()`/`uniqueVoid()`,
  and the private static `driveDispatch()` — and EXCLUDES every value-returning
  method (`g`/`retI`/`makeObj`). Count asserted as a portable `>= 5` (see JDK note).
- **Substrate consistency**: for 20 descriptors, `find(...).size()` equals that
  descriptor's multiplicity in `get_class_methods<W>()`, and every returned name
  actually carries that descriptor as a `(name, descriptor)` pair (find IS that
  filter, so the two views must agree exactly). Plus a by-NAME resolution cross-check
  (`get_class_methods("vmhook/fixtures/FindMethodsBySig")` descriptor counts == find
  sizes) proving find is anchored to the right klass.
- **Returned names are real**: every returned name is in the substrate's declared
  name set and never empty; inherited Object descriptors resolve to empty (flaw #2).
- **~30 NEGATIVE / malformed angles** (flaw #1): absent-but-well-formed,
  empty string, whitespace-only / leading / trailing / inner-space, near-miss
  (right shape wrong type), lowercase type chars, missing/only/unbalanced parens,
  garbage, a method NAME as descriptor, trailing junk after the return type, a
  doubled descriptor, truncated `L...;` (missing `;`), dotted (source) form, and a
  foreign-class descriptor — every one EMPTY, no crash.
- **Unregistered wrapper type** -> empty for a matching descriptor, a no-arg
  descriptor, and the empty descriptor (`type_to_class_map` miss path).
- **Determinism**: calling find twice for `(I)I` and `()V` yields the same multiset
  (pure read, no enumeration side effects).
- **LIVE post-dispatch stability**: the probe's `run()` drives REAL bytecode through
  `f(int)`/`f(long)`/`g(int,int)`/`arr(int[])` (invokevirtual) and
  `sf(int)`/`sUnique(long,long)` (invokestatic) — which also makes those methods JIT
  candidates — recording witness values (`f(7)=8`, `sf(9)=18`, `sUnique(2,3)=5`,
  etc.) that the module asserts. THEN it re-runs find for six descriptors and the
  total method count and asserts every set is byte-identical to the pre-dispatch
  snapshot, proving calling/compiling a method does not add, drop, or reorder
  `_methods` entries. This is the proof that the enumeration reflects live JVM state,
  not just class-initializer-time shape.

## JDK-version sensitivities I track

- **JDK 8 emits an extra synthetic `()V` accessor.** The fixture's anonymous inner
  `Harness.Probe` accesses the private `static void driveDispatch()`. Under
  `javac --release 8` this generates a synthetic `static void access$000()`
  (descriptor `()V`); on JDK 11+ (nestmates, JEP 181) it does NOT. I verified with
  `javap` across `--release 8/11/17/21`: the `()V` match count is **6 on JDK 8 and 5
  on JDK 11+**, while EVERY distinctive descriptor (`(I)I`=2, `(J)J`=1, `(II)I`=1, …)
  is identical on all four. So I hard-assert `()V` only as `>= 5` + contains the five
  universal members, and `ctx.record` the exact count with the JDK-8-synthetic
  explanation — the synthetic delta never breaks CI. All other set-equality checks
  use synthetic-immune distinctive descriptors.
- **Descriptors are JDK-stable**: I pinned every method's descriptor with `javap -s`
  against the compiled fixture; the internal-form strings
  (`(Ljava/lang/String;)Ljava/lang/String;`, `([[I)[[I`, etc.) are identical across
  JDK 8/11/17/21.
- **`<init>`/`<clinit>` always present**: the class always has a constructor and,
  because of the `static { Harness.register(...) }` block, a `<clinit>` — both live
  in `_methods` and are returned (no inheritance/resolution table involved), so the
  `()V` membership checks for them are universal.
- **No JNI / no call gate needed for the feature itself**: `find_methods_by_signature`
  is a pure VMStruct read, valid from any thread once the class is loaded — it does
  NOT need a live `current_java_thread`. Only the post-dispatch-stability section
  uses the harness `run_probe` handshake (to execute real bytecode); the feature's
  own assertions run directly in the module body.
- **GCC portability**: the module uses `static_field("name")` for all handshake /
  witness access (never the deducing-this static `get_field` fallback, which fails to
  compile in a static-call context on GCC), constructs no `field_proxy`/`value_t`
  via brace-init from a templated conversion, and is C++17-clean. Verified the TU
  compiles with **zero diagnostics** under g++ 15 (MSYS2) at `-std=c++23`
  (the project standard) with `-Wall -Wextra -Wpedantic -Werror`.
