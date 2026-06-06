---
name: method_call_wide_args-specialist
description: Specialist that totally masters the vmhook method_call_wide_args feature — method_proxy::call() passing long/double (two interpreter slots each) arguments — finds every flaw and owns its exhaustive JVM tests.
---

# method_call_wide_args specialist (area: methods)

I own ONE feature end-to-end: `vmhook::method_proxy::call(args...)` passing
`long` / `double` arguments correctly. Each of those occupies TWO interpreter
local slots; every other primitive (and an object reference) occupies one. The
bug class I defend against is a wide argument that either (1) TRUNCATES to 32
bits, or (2) SHIFTS / corrupts the FOLLOWING parameter slot (the next `int`
silently wrong, or everything after it mis-aligned). I prove neither happens for
wide args in the leading / middle / trailing position, long+double mixed, two
longs, two doubles, an all-wide four-arg frame, and the minimal "narrow value
immediately after a wide arg" witness — on BOTH dispatch paths, instance AND
static.

## Where the feature lives (vmhook/ext/vmhook/vmhook.hpp)

- `method_proxy::call()` — public entry + call_stub fast path: **13199-13416**.
  - The arg packer `pack` lambda: **13275-13329**. The generic-primitive branch
    (**13321-13327**) is the heart of wide handling:
    ```cpp
    static_assert(sizeof(clean_t) <= 8);
    std::intptr_t v{};                 // zero-init: a narrow arg leaves no stale high bits
    std::memcpy(&v, &a, sizeof(clean_t));
    params[param_idx++] = v;           // ONE intptr_t slot per C++ arg
    ```
    So a `long`/`double` fills all 8 bytes of ONE `params[]` word; a `float`/`int`
    fills the low 4 and the zero-init keeps the high 4 clean. `param_idx` counts
    ONE per C++ argument (NOT per interpreter slot).
  - `std::intptr_t params[8]{}` (**13267**) — zero-initialised, so a missing/short
    arg reads back as 0 (this is what makes the wrong-arity abuse calls crash-safe).
  - the receiver is placed in `params[0]` for instance calls (**13270-13273**),
    so the first declared arg starts at slot 1 (slot 0 for static).
  - result decode switch (**13367-13415**): `J`→`int64_t`, `D`→`memcpy` from the
    full `intptr_t`, `F`→`memcpy` from the low `int32_t`.
- `call_jni()` — the JNI fallback (the path actually taken on EVERY CI JDK, see
  below): **12590-13168**. Args are packed by `write_jni_arg_to_slot`
  (**10200-10273**): the union cell is fully cleared via `value.j = 0`
  (**10212**), then `value.j` is set for a 64-bit integral (**10253-10256**) and
  `value.d` for a double (**10261-10264**). JNI's `Call(Static)?<T>MethodA`
  expands each `jvalue` into the two interpreter locals internally, so the
  two-slot expansion is the JVM's job, not vmhook's, on this path.
- Overload selection: `resolve_compatible_method<args_t...>` (**13781-13869**) is
  re-run by BOTH paths on every call. `argument_matches_descriptor`
  (**13590-13672**) maps `int64_t`→"J", `double`→"D", `int32_t`→"I",
  `float`→"F", `char16_t`/`uint16_t`→"C". So `call((int64_t)x)` on a name-only
  proxy that has both an `int` and a `long` overload picks the `long` one;
  `signature_pinned` (**13791-13794**, set by `get_method(name, "(J)I")`) forces
  the exact pinned overload verbatim.

## How it works internally

`call()` checks `find_call_stub_entry()`. If present, it packs `params[8]`
(receiver + one word per C++ arg via the memcpy above), flips the JavaThread to
`_thread_in_Java`, invokes the hand-written call-stub trampoline with
`size_of_parameters = param_idx`, then decodes `result_holder`. If the call stub
is absent it routes to `call_jni()`, which resolves+caches the jclass+jmethodID,
packs a `jni_value[8]`, and dispatches the matching `Call(Static)?<T>MethodA`
slot. Either path yields the SAME `value_t`. `call()` only works from INSIDE a
hook detour (where `current_java_thread` is set), so my module hooks
`MethodCallWideArgs.trigger(int)` and performs every wide-arg `call()` in that
detour.

## Flaws I found (real, current)

- **[medium] The call-stub fast path's wide-arg slot accounting is unproven and
  suspect.** `call()` passes ONE `intptr_t` word per C++ argument and sets the
  stub's `size_of_parameters` to `param_idx` — the *C++ argument count*, NOT the
  JVM *slot count*. A Java `(JJ)J` body has interpreter parameter_size = 5 slots
  (this + 2 + 2), but vmhook hands the stub only 3 words for `call(a, b)` on an
  instance. Whether the call stub re-expands wide values or copies words verbatim
  determines if this is correct or a latent mis-read. It is NOT exercised on CI
  because `StubRoutines::_call_stub_entry` is absent from VMStructs on every CI
  JDK (8-26) — the JNI fallback runs instead, and JNI expands wide jvalues
  correctly. So my hard assertions validate the JNI path; I `ctx.record` the live
  path and would catch a regression the moment a JDK exposed the stub. I PIN
  current behaviour rather than asserting a slot count, so CI stays green.
- **[low] `call()` silently drops args past 8 with no diagnostic** — **13277**:
  `if (param_idx >= 8) { return; }` inside `pack`. `call_jni` static-asserts
  `sizeof...(args) <= 8` (**12757**) but `call()` has no such guard, and because
  each C++ wide arg still consumes one `params[]` word here while the *callee*
  needs two interpreter slots, the real safe arity is signature-dependent and
  undocumented. A 5-long call already needs 10 callee slots from a 8-word array.
- **[low] `case 'F'` decodes through a *signed* `int32_t`** — **13378**:
  `const std::int32_t bits{ static_cast<std::int32_t>(result_holder) };` then
  memcpy. A float return is an opaque bit pattern, not a signed int; `uint32_t`
  would document intent (the sibling `case 'D'` already uses the full width). Same
  bits on x64, so clarity not correctness. (Return-side, but in the same switch.)
- **[design] `monostate` conflates "void" and "dispatch failure"** — a wrong-arity
  wide call that the JVM happens to no-op, a null method id, and a real `()V`
  success all yield `monostate`; `is_void()` cannot tell them apart. My
  wrong-arity probes therefore assert *process survival*, never a specific value.

I did NOT modify the header (off-limits, edited serially by the lead); every flaw
above is PINNED by a test that locks in current behaviour with a `// BUG:`-style
note in the module comments where relevant.

## Exhaustive JVM test angles I cover (tests/jvm/modules/method_call_wide_args.cpp + example/vmhook/fixtures/MethodCallWideArgs.java)

162 `ctx.check()` assertions, all on a live JVM, all driven through the
`trigger(int)` detour so a real bytecode dispatch + the real call gate run. Every
method combines its args into a deterministic return AND stamps each arg into a
per-parameter witness field, so the native side proves correctness TWO ways
(combined return + isolated witness) — a wide arg corrupting the next int leaves
the witness wrong even if the return coincidentally matched.

- **long ECHO** (`idL`): the full boundary set — 0, 1, -1, `Long.MIN_VALUE`,
  `Long.MAX_VALUE`, `0x0123456789ABCDEF`, `0xFFFFFFFF00000000` (high half only),
  `0x00000000FFFFFFFF` (low half only), `0xDEADBEEFCAFEBABE` (both halves) — each
  round-trips bit-exact; the high-vs-low halves are the pair a 32-bit truncation
  bug would confuse.
- **two longs** (`addL`): min+max, high-half+low-half, 1+(-1), pattern+pattern;
  asymmetric formula (`a*1000003 + b`, computed with unsigned-wrap helpers to
  match Java's two's-complement `long` and avoid C++ signed-overflow UB) catches
  a<->b swap; both witnesses prove no truncation.
- **double ECHO** (`idD`): +0.0, -0.0, +1.0, -1.0, Math.PI, a negative, +Inf,
  -Inf, canonical NaN, signaling NaN, payload qNaN, smallest subnormal
  (MIN_VALUE), MIN_NORMAL, MAX_VALUE — all reconstructed from raw bits and
  asserted BIT-EXACT (NaN payload, signaling bit, denormal mantissa, sign-of-zero
  all survive the memcpy round trip).
- **wide in the MIDDLE** (`mixA(int,long,int)`): both flanking ints survive the
  two-slot long; the high-half-only long case is the precise witness for "wide
  high bits leaked into the next slot" (trailing int must stay 99).
- **wide LEADING + TRAILING** (`mixB(long,int,long)`): the squeezed int survives
  two wide args.
- **wide LEADING, int TRAILING** (`scaleD(double,int)`): the canonical "a double
  must not corrupt the following int" — the trailing int witness must equal
  exactly what was passed; return `x*n` compared bit-exact (IEEE-754 deterministic).
- **double in the MIDDLE** (`mixC(int,double,int)`): both flanking ints survive.
- **ALL FOUR WIDE** (`mixD(long,double,long,double)`): long+double interleaved,
  positive and negative; return bit-exact; both long witnesses exact.
- **minimal two-slot witnesses** (`intAfterLong`, `intAfterDouble`): the int
  immediately after one wide arg equals exactly what was passed AND is returned.
- **wide AFTER a narrow** (`longAfterInt`, `doubleAfterInt`): the wide value must
  START at the correct slot (slot 1 after an int at slot 0 for instance).
- **overload selection by width**: `widthTag(int)` vs `widthTag(long)` and
  `fdTag(float)` vs `fdTag(double)` — the C++ arg type must pick the wide
  overload; plus an explicit `(J)I` signature that pins the long overload.
- **STATIC variants** (`sAddL`, `sIdL`, `sIdD`, `sMixA`, `sScaleD`, `sMixD`): the
  no-receiver frame puts the first wide arg at slot 0; witnesses prove no phantom
  `this` shifted the wide value.
- **wrong arity / wrong type** (`addL()` with no args, `scaleD(double)` missing
  the int, `mixA(long,long,long)` with mismatched widths): the process must
  SURVIVE (zero-init `params[]` makes missing args read as 0; the abuse method is
  primitive-only so there is no reference-slot store barrier to AV). Value is
  unspecified and intentionally NOT asserted; survival is.
- Records the live dispatch path (`call_stub` vs JNI fallback) as `[INFO]`.

## Known JDK-version sensitivities

- `StubRoutines::_call_stub_entry` is **absent from VMStructs on every JDK 8-26**
  tested, so in practice `call()` runs the **JNI fallback**, where the JVM itself
  expands wide `jvalue`s into the two interpreter locals. My assertions are
  path-independent (both paths must agree) and I only *record* the path. The
  call-stub wide-arg accounting (the [medium] flaw) becomes live only if a future
  JDK re-exposes the stub — my tests would catch a divergence immediately.
- The JNI `Call(Static)?<Long|Double>MethodA` slot indices (J=54/134, D=60/140)
  are fixed by the JNI spec and stable across all versions; a wrong slot would
  dispatch the wrong width and my boundary/witness assertions would fail.
- `long`/`double` two's-complement and IEEE-754 semantics are identical on every
  HotSpot target, so the fixture's deterministic formulas and bit-exact double
  comparisons are JDK-independent. I compute every expected `long` formula with
  unsigned-wrap helpers so the C++ side matches Java exactly at the boundaries
  without invoking C++ signed-overflow UB.
- The fixture is pure Java-8 source (no var / records / switch-expr / text blocks
  / `List.of`), verified to compile under javac 8 AND javac 26; wide values are
  built from `Double.longBitsToDouble` / `Long.MIN_VALUE` etc. (all 1.x APIs) so
  no post-8 API leaks in. It is ASCII-only so source encoding never matters.
