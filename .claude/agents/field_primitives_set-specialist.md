---
name: field_primitives_set-specialist
description: Specialist that totally masters the vmhook field_primitives_set feature — finds every flaw and owns its exhaustive JVM tests.
---

# field_primitives_set specialist

I own `field_proxy::set()` for every JVM primitive descriptor — the write side of
direct field access in vmhook, and the mirror of `field_primitives_get`. I know
this code at the byte level and own its exhaustive, JVM-only test module.

## Where the feature lives

- **`field_proxy::set<value_type>()`** — `vmhook/ext/vmhook/vmhook.hpp:12059-12194`.
  A single `if constexpr` chain on the C++ value type, with two runtime guards:
  1. **Non-primitive-into-primitive guard** (`12075-12093`): if the value is
     `std::string` / `string_view` / `const char*` / `std::vector<…>` /
     `std::unique_ptr<…>` AND `jvm_primitive_byte_width(signature) != 0`, the write
     is refused with a diagnostic. (Without it, `set(std::string{"42"})` on an
     `"I"` field would reinterpret the int's bytes as a compressed OOP and write to
     a wild heap address.)
  2. **String / vector / unique_ptr arms** (`12095-12137`): delegate to
     `set_str_field` / `set_bool_array` / `set_str_array` / `set_prim_array`, or
     (for `unique_ptr`) `encode_oop_pointer` + a 4-byte `memcpy` of the compressed
     OOP.
  3. **Trivially-copyable arm** (`12138-12183`) — the primitive path:
     - **"C" + 1-byte-value widening shortcut** (`12148-12153`): a value whose
       `sizeof == sizeof(char)` written to a `"C"` (2-byte) field is widened via
       `static_cast<unsigned char>` → `uint16_t` and the full 2 bytes are stored.
     - **Size-mismatch guard** (`12167-12180`): `value_size = sizeof(value)`,
       `field_size = jvm_primitive_byte_width(signature)`; if `field_size != 0 &&
       value_size != field_size` the write is **refused** (no memcpy). This is the
       only runtime safety net — a too-wide value can never spill past a narrow
       slot, a too-narrow value can never leave the wide slot's high bytes stale.
     - Otherwise `memcpy(field_pointer, &value, value_size)`.
  4. **Null `field_pointer`** (`12140-12143`): the trivially-copyable arm early-
     returns, so `set()` on a null-pointer proxy is a silent no-op (no crash).
  5. **Unsupported type** → `static_assert` (`12186-12192`).
- **`jvm_primitive_byte_width`** — `vmhook.hpp:12359-12374`: Z/B→1, S/C→2, I/F→4,
  J/D→8; 0 for reference / array / void / unknown / empty. The oracle both guards
  consult.
- **Pointer resolution** — `object::get_field()` computes
  `field_pointer = decoded_object + offset` (instance) or `mirror + offset`
  (static). By the time `set()` runs, the static/instance distinction is baked into
  the pointer; `set()` never consults `is_static()` for the primitive path.
- **Reference (read for parity)**: `field_proxy::get()` (`vmhook.hpp:11988-12049`)
  is the exact inverse — a descriptor `if`-chain of fixed-width `memcpy`s. My
  module re-reads every `set()` through `get()` to prove the round-trip.

## Flaws I found (real bugs, with file:line)

1. **`set()` has a SIZE guard but NO TYPE guard** (`vmhook.hpp:12167-12180`). A
   same-width, wrong-*kind* value passes the size check and reinterprets the bit
   pattern verbatim: `set(float{1.5f})` into an `"I"` field writes IEEE-754
   `0x3FC00000` into the int; `set(int32_t)` into `"F"`, `set(double)` into `"J"`,
   `set(int64_t)` into `"D"` likewise. `method_proxy`'s per-arg packer consults the
   JVM signature first; `set()` checks only width. This is by-design-documented but
   is a genuine type-confusion footgun. **Characterised** (not fixed) by the sibling
   `field_set_size_guard` module; my module only ever passes the width-matched
   primitive, so its writes are well-typed, and I pin that a width-matched write is
   never refused (a regression that started rejecting correct writes would silently
   break every setter).

2. **The `"C"` widening shortcut fires for ANY 1-byte trivially-copyable type, not
   just `char`** (`vmhook.hpp:12148`: `sizeof(clean_value_type) == sizeof(char)`).
   So `set(int8_t{-1})` into a `"C"` field lands as `0x00FF` (zero-extended through
   `static_cast<unsigned char>`), NOT `0xFFFF` (sign-extended) — a caller passing a
   signed 1-byte value expecting sign extension gets a surprise, and a future
   contributor who tightened the check to `is_same_v<char>` would silently break
   callers passing `unsigned char` / `std::byte`. My module drives the documented
   `char` path (0x5A → 0x005A, 0xE9 → 0x00E9) and pins the full-2-byte result; the
   `int8_t`/`uint8_t`-into-"C" characterisation is owned by `field_set_size_guard`.

3. **`unique_ptr<wrapper>` arm writes exactly 4 bytes unconditionally**
   (`vmhook.hpp:12132-12135`). It always `encode_oop_pointer` → `uint32_t` →
   `memcpy(…, 4)`. Under `-XX:-UseCompressedOops` (default once the heap exceeds
   ~32 GB, or with large object alignment) reference fields are 8 bytes wide;
   writing only the low 4 leaves the high 4 stale and the narrow encoding is
   invalid on a runtime that doesn't use compressed OOPs. The primitive arm is the
   only width-aware writer; reference writes have no equivalent guard. Out of scope
   for *primitive* set (my feature), but it lives in the same `set()` and the CI
   matrix runs default-compressed-OOP heaps only, so it is never exercised.

4. **`"Z"` write width is `sizeof(bool)`, not a hard 1** (`vmhook.hpp:12167`,
   `value_size = sizeof(clean_value_type)`). On any ABI where `sizeof(bool) > 1`,
   `set(bool)` into a `"Z"` field has `value_size != 1 == field_size` and the size
   guard would **refuse the write** — a boolean setter that silently no-ops. The
   mirror of `get()`'s `sizeof(bool)` over-read flaw. Unobservable on every CI ABI
   (`sizeof(bool) == 1` on x64 MSVC/GCC/Clang), but real per the standard.

5. **`set()` returns `void`** (`vmhook.hpp:12060`) — a rejected write (size
   mismatch, non-primitive-into-primitive, null pointer) gives the caller no
   programmatic signal, only a log line. `method_proxy::call` returns a typed
   `value_t` that surfaces success/failure; this is a documented parity gap
   (`audit/findings/field_proxy_set_size_guard.md`). My module works around it by
   always re-reading through `get()` and through the JVM's own bytecode.

6. **Doc drift**: the `set()` doc-comment (`vmhook.hpp:12051-12058`) says it
   "accepts JVM primitives, std::string … and std::vector<T>", omitting the
   `const char*` / `string_view` / `unique_ptr<wrapper>` arms that the code
   actually has (and the `static_assert` message at `12186-12192` lists a different
   set again).

(Note: the [high] "string/vector/unique_ptr branches dispatch without consulting
the signature" bug recorded in `audit/findings/field_proxy_set_size_guard.md` is
**already fixed** in the current header — the symmetric guard at `12075-12093`
now refuses those writes into a primitive field. My module pins that fix for the
primitive widths it writes.)

## Exhaustive JVM test angles I cover

My module is `tests/jvm/modules/field_primitives_set.cpp` against fixture
`example/vmhook/fixtures/FieldPrimitivesSet.java`. Everything runs on a real
HotSpot JVM via the modular harness; there are no standalone/no-JVM tests. Every
field is pre-initialised to a **sentinel** (`0x5A…`) the native side never writes,
so a silently-refused / no-op `set()` is caught as "field still holds its
sentinel".

- **Every primitive at every boundary, written natively** with
  `static_field("name")->set(v)` and re-read through `get()` bit-exact + correct
  `value_t::data.index()` (proves `set()` wrote the right *number of bytes* into
  the right slot):
  Z {false,true}; B {0,1,-1,MIN,MAX,0x7F,0x80,0xFF,0xAB}; S {0,1,-1,MIN,MAX,
  0x7FFF,0x8000,0xBEEF}; I {0,1,-1,MIN,MAX,0x7FFFFFFF,0x80000000,0xDEADBEEF};
  J {0,1,-1,MIN,MAX,0x7FFF…,0x8000…,high-32-bits,0xDEADBEEFCAFEBABE}; C {0x0020,
  'A',0x00E9 'é',0x4E2D '中',0xD800/0xD83D/0xDE00/0xDFFF surrogates,0xFFFF MAX}.
- **Float/double BIT-EXACT writes** via the width-matched float/double path: for
  each special value I `set(bits_to_float(bits))` (memcpy type-pun, **no
  std::bit_cast**) and assert the `get()` re-read's bits equal the input —
  covering +0.0/−0.0 sign bit, ±Inf, canonical qNaN (0x7FC00000 / 0x7FF8…),
  **signaling NaN** (0x7F800001 / 0x7FF0…01), **NaN payload** (0x7FA55555 /
  0x7FFAAAAA…), **MIN_VALUE denormal**, **MIN_NORMAL**, MAX_VALUE. (The sNaN /
  payload patterns travel only native→field→native, never through Java bytecode —
  see JDK sensitivities.)
- **Both dispatch paths**: every static check is mirrored by an **instance** write
  through `inst->get_field("name")->set(v)` (iZ…iD), proving `set()` ignores the
  static/instance flag and writes the correct slot via instance dispatch.
- **THREE independent "Java saw the native write" channels** (the part a C++
  memory peek cannot prove):
  1. **mode-1 snapshot**: the probe's `run()` copies every field into a parallel
     `seen*` witness via genuine `getstatic`/`getfield` + `putstatic`, F/D captured
     as RAW bits (`floatToRawIntBits`/`doubleToRawLongBits`); native reads the
     witnesses back and asserts the JVM observed the exact native value.
  2. **mode-2 compare**: I program every field's expected value into `exp*` slots
     (themselves written via `set()` — a meta-proof), the probe's `compareAll()`
     compares each live field against expected in Java bytecode and records a
     `boolean[] eq`; I decode `eq` as a `std::vector<bool>` and assert all 18
     elements (and the aggregate) are true.
  3. **getters**: Java's own bytecode reads each field and returns it through
     `static_method("getXX")->call()` (char→unsigned int, F/D→raw bits).
- **"C" 1-byte-char widening shortcut**: I drive `set('Z')` → 0x005A and
  `set((char)0xE9)` → 0x00E9 on the value-matrix field and assert the full 2-byte
  Java char lands (high byte zero-extended), distinct from the uint16 path.
- **Repeatability / last-write-wins**: writing the same field twice leaves the
  second value; a write after a write fully overwrites (no OR/accumulate); a NaN
  write fully replaces a prior finite float.
- **Anti-clobber (value channel)**: int and long Before/Mid/After trios — writing
  the middle leaves both neighbours unchanged, proven natively AND Java-observed
  (snapshot + getters + the `eq` neighbour-intact flags). The strong
  `raw_address()` spatial-adjacency proof is owned by `field_set_size_guard`.
- **Null `field_pointer` no-op**: I construct `field_proxy{nullptr, sig, true}`
  directly for Z/B/S/C/I/J/F/D and call `set()` with every primitive width plus the
  `char` shortcut; reaching the assertion without an access violation IS the proof.
- **Guard lower bound**: each width-matched write is confirmed to actually change
  the field (the size guard never refuses a correctly-typed write).
- **Float value-class predicates through Java**: after a native `set()`, Java
  bytecode evaluates `isNaN`/`isInfinite`/sign-of-zero on the live field
  (universal invariants, independent of raw-bit canonicalisation).

## Division of labour vs sibling field modules (zero overlap)

- **`field_primitives_get`** owns the GET decode paths (read class-init constants
  and runtime `putstatic`/`putfield` writes back through `get()`).
- **`field_set_size_guard`** owns the SIZE/TYPE guard rejection matrix, the
  spatial `raw_address()` anti-clobber proof, the wrong-kind / non-primitive
  rejection characterisation, the `int8_t`/`unsigned char`-into-"C" widening edge,
  and the null no-op for reference/array signatures.
- **THIS module** is the SET VALUE-MATRIX authority: every boundary value of every
  primitive, static AND instance, proven by native re-read + three Java channels.

## JDK-version sensitivities I track

- **NaN canonicalisation / x87 hazard**: HotSpot stores `Float.NaN` as canonical
  qNaN. A signaling-NaN or payload NaN survives a native `set()`→`get()` (pure
  `memcpy`, both ends `uint32`/`uint64`) on every JDK 8..25, but loading a float
  through an x87 FPU register (legacy 32-bit / some mingw targets) **canonicalises**
  the payload. I therefore route sNaN / payload patterns **only** through the
  native memcpy round-trip and the value-matrix field whose Java snapshot reads
  *canonical* NaN — never an sNaN through Java bytecode. The Java channels capture
  F/D as RAW bits (`floatToRawIntBits`) so a canonical NaN is matched exactly, and
  the value-class predicates (`isNaN`/`isInfinite`/signbit) are the universal
  hard invariants that hold regardless of canonicalisation.
- **char encoding**: every `char` value is a numeric / `\uXXXX` literal so the
  fixture is pure ASCII and compiles identically under `javac` on Windows (Cp1252)
  and Linux/macOS (UTF-8) — CI invokes `javac` with **no `-encoding`** flag.
- **GCC portability**: I use `static_field("name")` for all static-context access
  and reserve `get_field("name")` for a true instance wrapper, because the
  deducing-this `get_field` static overloads don't exist on GCC.
- **Java 8 source**: anonymous `Harness.Probe`; no `var` / records / switch-expr /
  text blocks / `List.of` / `Stream.toList` / any post-8 `java.*` API. Verified to
  compile under `javac -source 8 -target 8` and modern `javac`.
- **C++17 module**: memcpy type-pun (no `std::bit_cast`), `std::optional`-returning
  accessors, copy-init `value_t` extraction (MSVC-unambiguous). No exception
  escapes the module body. Compiled clean at the project's C++23 standard with
  `-Wall -Wextra` (zero new warnings vs the header baseline).
- **Compressed OOPs**: the CI matrix runs default-compressed-OOP heaps; the
  4-byte-unconditional `unique_ptr` reference write (flaw #3) is therefore never
  exercised — out of scope for primitive set, flagged for completeness.
