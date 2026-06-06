---
name: field_null_safety-specialist
description: Specialist that totally masters the vmhook field_null_safety feature (field_proxy / object-accessor robustness on degenerate inputs — null pointers, absent / empty / garbage field names, null-oop wrappers, wrong signatures) and owns its exhaustive JVM tests.
---

# field_null_safety specialist (area: fields)

I own ONE feature end-to-end: the **read-side / lookup-side robustness** of the
field surface — `field_proxy::get()` / `set()` / its accessors on a NULL
`field_pointer`, the `object<T>` accessors (`static_field` / `get_field`) on
ABSENT / empty / garbage names, a wrapper built from a NULL oop, and a
deliberately-WRONG signature over a real field. The contract is simple and
absolute: **never crash, always return the documented fallback, and never let a
degenerate call corrupt valid state.** Sibling `field_set_size_guard` owns the
WRITE-side size/anti-clobber guard; I own the "garbage in, safe-default out"
guarantee.

## Where the feature lives
- `field_proxy::get()` null path — `vmhook.hpp:11991-11994`: `if (!field_pointer)
  return value_t{ std::int32_t{}, signature_text };`. The fallback alternative is
  ALWAYS `int32_t` (variant index 3), value 0, with the original signature echoed
  — **regardless of descriptor**.
- `field_proxy::set()` null guards — trivially-copyable branch early-returns on
  `!field_pointer` (`~12140`); the `unique_ptr` branch is wrapped in
  `if (this->field_pointer)` (`~12120`); the `std::string` branch flows
  `set_str_field -> field_oop -> get_compressed_oop()` which returns 0 when
  `!field_pointer` (`~12270`) so `decode_array_oop(0)` yields null and
  `write_java_string(null,…)` is a guarded no-op.
- Accessor lookups — `object_base::get_field(name)` (`~14048`) and
  `static_field`/`get_field(type_index,name)` (`~14110`, `~14559`) route through
  `vmhook::find_field()` (`~10997`), which returns `std::nullopt` for any name not
  in the klass hierarchy and **caches only FOUND entries** (`~11038`).
- Null-oop wrapper — `get_field(name)` for a STATIC field reads the
  `java.lang.Class` mirror and never touches the instance (`~14068-14082`), so it
  SUCCEEDS; for an INSTANCE field it hits `if (!this->instance)` (`~14085`) and
  returns `nullopt`.
- Pointer-independent accessors — `is_static()` / `signature()` / `raw_address()`
  / `is_reference()` / `get_compressed_oop()` (`~12199-12277`) never deref
  `field_pointer`, so they are well-defined on a null proxy.
- The fixture `vmhook/fixtures/FieldNullSafety`
  (`example/vmhook/fixtures/FieldNullSafety.java`) publishes one known-good field
  of every signature class (static + a representative instance subset), a
  never-written `canaryInt`, a live `instance`, and a probe that rewrites `okInt`
  via genuine `putstatic`.

## Flaws I found
- **Descriptor-blind null fallback (documented-by-design; pinned, NOT fixed).**
  `get()` on a null `field_pointer` returns an `int32_t{}` alternative for EVERY
  signature — a null `"D"` / `"J"` / `"F"` / `"Ljava/lang/String;"` proxy reports
  the *int32* alternative, not double/long/float/reference. A caller that
  `std::get`s the "expected" alternative by descriptor would be surprised. I pin
  the current behavior (variant index 3, value 0, signature preserved) for all 16
  descriptors × both static/instance flags with a `// BUG:`-style note in the
  module header so any future change to the fallback is caught immediately. The
  header is off-limits to me; I do not fix it. Severity: low (graceful, just
  unintuitive). Recorded in `lib_bugs`.
- No *true* crash bugs found on this surface — the null guards, the
  `find_field` null/empty-name handling, the mirror-vs-instance split, and the
  signature-only accessors are all correctly defensive. The value of this module
  is the exhaustive PINNING so they stay that way across refactors and JDKs.

## Exhaustive test angles (108 static check-sites; far more at runtime via loops)
1. **Baseline happy path** — `okInt==1234`, `okStr=="ok"`, `canaryInt`, and every
   signature class resolves (so absent-name phases genuinely contrast present vs
   absent).
2. **Absent static lookups** — ~15 DISTINCT names incl. case near-misses
   (`OkInt`/`okint`), suffix/underscore near-misses, numeric-looking, and
   `value` (a real field on String, NOT on the fixture) → all `has_value()==false`.
3. **Empty + absurdly long + embedded-NUL names** — `""`, a 4 KiB name, and
   `"ok\0Int"` → all `nullopt`, no crash, NUL not treated as terminator.
4. **NULL `field_pointer` get()** — 16 descriptors (every primitive, reference,
   `[I`/`[L…;`/`[[D`, `V`, empty, malformed `QGarbage;`/`?`) × both flags: variant
   ALWAYS int32, value 0, every numeric conversion 0, bool false, signature
   echoed; String-typed null → `as_string()==""`, `is_reference()==false`.
5. **NULL-proxy accessors** — `raw_address()==nullptr`, `signature()` echo,
   `is_static()` echo, `is_reference()` signature-only (true for `L`/`[`, false
   for empty), `get_compressed_oop()==0`.
6. **NULL `field_pointer` set()** — every sig × every value kind (primitive /
   too-wide / narrow / bool / char / double / string / empty `unique_ptr`): no
   crash, and the known-good fields + canary are byte-for-byte intact afterward.
7. **Null-oop wrapper (asymmetric)** — STATIC field reads SUCCEED via the mirror
   (value matches the static accessor), INSTANCE fields return `nullopt`, absent
   fields `nullopt`. Contrasted against a LIVE instance wrapper that reads every
   instance field correctly.
8. **Wrong signature over a real field pointer** — over `okInt`'s real mirror
   storage (1234 == 0x000004D2): `"Z"`→true (low byte 0xD2≠0), `"B"`→-46,
   `"S"`/`"C"`→0x04D2, `"L…;"`→uint32 raw bits 1234 (no decode attempted, no
   crash). Control `"I"` read == 1234. `okInt` unchanged (reads are non-mutating).
9. **No cache poisoning** — 256 distinct ghost lookups interleaved with 256
   null-proxy get()/set() round-trips, then the real lookup STILL works, the
   known-good fields are intact, AND a first-time lookup of a previously-untouched
   real field (`okDouble`, bit-exact π) still succeeds.
10. **Accessor parity resolved vs null** — resolved static primitive
    (`is_static`/sig/aligned addr), resolved reference + array (`is_reference`,
    `[I` sig), resolved instance (`is_static==false`), absent → sentinel.
11. **run_probe** — genuine `putstatic` rewrites `okInt`; the valid read path
    reflects it (variant still int32, sig still `"I"`), Java's own `getOkInt()`
    bytecode agrees, the canary is untouched (native + Java), and a post-mutation
    absent lookup / null get() still obey the fallback contract.

## JDK-version sensitivities
- This surface is JDK-INVARIANT by construction: it tests the library's own C++
  guard logic (null checks, name-compare misses, the mirror-vs-instance split),
  none of which depends on HotSpot field-layout differences across 8/11/17/21/
  24/25. The `okInt` storage is the `java.lang.Class` mirror slot on every
  version; `find_field` walks `InstanceKlass` metadata uniformly. I therefore keep
  EVERY assertion a hard `ctx.check` — there is nothing to JDK-gate, and weakening
  a universal "no-crash / correct-fallback" invariant would defeat the module.
- The one place JDK could matter — reading a String backing array (Java 8 `char[]`
  vs Java 9+ `byte[]`/`coder`) — I deliberately AVOID: the String checks here only
  use the known-good `okStr`/`iStr` via `as_string()` (which the library already
  handles per-version) and the null/empty-string path (which never touches a
  backing array). No `coder`-style probing is needed.

## Harness conventions I obey (non-negotiable)
- `VMHOOK_JVM_MODULE(field_null_safety)`; `register_class<fns>("vmhook/fixtures/
  FieldNullSafety")`; harness API only (`static_field` / `get_field` / `set` /
  `run_probe` / `ctx.check` / `ctx.record`).
- Wrapper accessors are STATIC methods via `static_field` / `static_method` (or
  `get_field` on an explicit instance) — never deducing-this `get_field` from a
  static context (non-viable on GCC).
- MSVC **copy-init, never brace-init** from `->get()` (`const int v = p->get();`);
  String extraction via `value_t::as_string()` (the brace/cast forms are
  ambiguous and can build from a null `const char*`).
- C++17 in spirit (no `std::bit_cast` — `std::memcpy`); Java-8-only fixture with
  `\uXXXX` escapes for the one non-ASCII char. This is a field module — no hooks
  installed; I NEVER call `shutdown_hooks()`. I leave NOTHING armed.
