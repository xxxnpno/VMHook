# JNI Surface Inventory — road to literal zero-JNI

**Scope:** `vmhook/ext/vmhook/vmhook.hpp` (19824 lines, v0.5.3) plus the whole repo for
build-level JNI dependencies.
**Audit date:** 2026-08-04 · **Mode:** read-only, no source edited.
**Headline:** the *library header* is already 100% JNI-free at the **code** level — no
`#include <jni.h>`, no JNI type, no JNIEnv/JavaVM function-table call, no
`JNI_GetCreatedJavaVMs`. What remains is **naming leakage**, **dead ABI ballast**, and a
large body of **stale documentation** that still describes a JNI implementation that was
deleted.

---

## 0. Executive scoreboard

| Axis | Status | Residue |
|---|---|---|
| Real JNI code in `vmhook.hpp` | **CLEAN** | 0 lines |
| JNI types / includes in `vmhook.hpp` | **CLEAN** | 0 |
| `vmhook::jni` namespace | **LEAKING** | 1 class (`global_ref`), 0 real JNI dependency |
| `jni_*` public/detail names | **LEAKING** | `detail::jni_signature_for_arg` (~629 external references) |
| Dead JNI-era members / params | **PRESENT** | `cached_method_id`, `cached_class_handle`, `make_java_array`'s 4th param |
| Stale JNI documentation | **HEAVY** | ~128 `jni`/`JNI` textual hits, of which ~60 are actively wrong |
| Live log/diagnostic strings mentioning JNI | **PRESENT** | 6 user-visible strings |
| Build-level `jni.h` dependency | **PRESENT** | `viewer/payload` (deliberate) + `vmhook/src/speedtest.cpp` (optional bench) |

Raw counts in `vmhook.hpp`:

```
total lines containing jni|JNI        128
  pure comment-prose lines             79
  doc-block body / code / strings      49
actual C++ tokens containing "jni"     19   (all in section 2 + 3 below)
```

---

## 1. Actual JNI code still present

### 1a. In `vmhook/ext/vmhook/vmhook.hpp` — **NONE**

Verified by three independent greps:

| Probe | Result |
|---|---|
| `#\s*include\s*[<"]jni` | **0 hits** |
| `JNIEnv_ \| JNINativeInterface \| jvalue \| jobject \| jclass \| jmethodID \| jstring \| jarray \| jni_function` | **0 code hits** (2 comment hits: 15887, 15907) |
| `JNI_GetCreatedJavaVMs \| JNI_CreateJavaVM \| GetEnv( \| AttachCurrentThread \| current_jni_env \| JNI_OK \| JNI_VERSION` | **0 code hits** (4 comment hits: 5384, 5411, 12140, 15293) |

There is **no** forward declaration of a JNI type, **no** function-pointer table indexed by
JNIEnv slot number, and **no** `GetProcAddress`/`dlsym` of a `JNI_*` export anywhere in the
header. Every former JNI call site has been replaced by a pure-VM path and the removal is
documented in-place:

| Line | Former JNI call | Pure-VM replacement now in place | Live? |
|---|---|---|---|
| 5411-5418 | `JavaVM::AttachCurrentThread` | thread-list walk in `ensure_current_java_thread()`; returns `false` for a non-JavaThread | live |
| 8456-8460 | `JNIEnv::FindClass` for `'['` names | `vmhook::resolve_array_klass()` (Universe statics + `InstanceKlass::_array_klasses`) | live |
| 8516-8523 | `ClassLoader.loadClass` context-loader fallback | none — CLDG walk only; miss ⇒ `nullptr` | live |
| 12236-12237 | `Thread.setContextClassLoader` on attached threads | removed entirely (`capture_host_classloader_klass` now only latches the Klass\*) | live, **now a no-op body** |
| 12498-12508 | `anchor.getClass().getClassLoader().loadClass(name)` | `find_class_via_oop()` validates the anchor then delegates to `find_class()` | live, **loader disambiguation LOST** |
| 12638-12641 | `JNIEnv::NewObjectA` (`<init>` chain) in `make_unique` | `make_java_object()` + C++ `construct()` | live |
| 13058-13065 | `JNIEnv::New<Type>Array` GC-aware fallback | none — TLAB only, `nullptr` on failure | live |
| 13246-13252 | `JNIEnv::NewString` GC-aware fallback | none — `build_via_tlab()` at any length | live |
| 10644-10651, 14699-14706 | `jni_new_string_utf16_local` → `NewString` + `DeleteLocalRef` | `vmhook::make_java_string()` | live |
| 15071-15079 | `Call<Type>MethodA` fallback in `method_proxy::call` | none — `StubRoutines::_call_stub_entry` only; **invocation unavailable on JDK 21+** | live |
| 15296-15300 | `ExceptionDescribe` / `ExceptionClear` after a call | none — pending exception left on the JavaThread | live |

> **Behavioural note for the checklist:** rows 5, 9, 10, 11 are genuine capability
> regressions the docs still hide (see §4). Row 11 is why `viewer/payload/payload.cpp`
> keeps `<jni.h>` (see §5).

### 1b. Elsewhere in the repo — real JNI code

| File | Line | What | Live? | Replacement in a pure-VM world |
|---|---|---|---|---|
| `viewer/payload/payload.cpp` | 64 | `#include <jni.h>` | **LIVE, deliberate** | none available today (see §5) |
| `vmhook/src/speedtest.cpp` | 18 | `#include <jni.h>` | LIVE but **opt-in bench only** | delete the "pure JNI baseline" leg of the microbenchmark |
| `vmhook/src/speedtest.cpp` | 49-59 | `JNI_GetCreatedJavaVMs`, `GetEnv`, `JNI_OK`, `JNI_VERSION_1_8` | LIVE (bench) | same as above |
| `vmhook/src/example.cpp` | 269 | `return 0x00010008; // JNI_VERSION_1_8` | LIVE | **not JNI** — a hand-coded integer returned from `JNI_OnLoad`; the JVM requires this ABI contract of any injected DLL. Keep; it needs no `jni.h`. |

### 1c. Mere mentions in comments — catalogue (counts + ranges)

79 lines are pure comment prose; another ~30 are doc-block bodies. Grouped by area
(full stale/misleading classification in §4):

| Area | Line range | Count | Character |
|---|---|---|---|
| File banner / API claim | 8 | 1 | accurate ("zero-JNI") |
| Thread adoption | 1625, 1644, 5379-5417 | 6 | accurate pure-VM notes |
| Registration-map thread-safety | 1672, 9434 | 2 | accurate (name refs) |
| `is_unique_ptr` trap note | 1830 | 1 | **stale** ("the JNI arg slot") |
| ClassLoader oop | 4506 | 1 | **stale** ("fake JNI handle target") |
| find_class / array klass | 4759, 8379, 8419, 8432-8456, 8516, 8546, 9117 | 12 | mixed — 8432-8453 stale, rest accurate |
| Klass header decode | 5819 | 1 | **stale** (`jni_make_unique` gone) |
| Method enumeration | 9479, 9544 | 2 | accurate |
| `return_value::set_arg` string | 10609-10644 | 5 | **stale block**, superseded at 10644 |
| `hook<T>` doxygen | 10720 | 1 | accurate (historical comparison) |
| `shutdown_hooks` | 11842 | 1 | historical (`call_jni` crash) |
| "JNI helper layer" banner | 12127-12134 | 4 | **stale banner** — the layer no longer exists |
| Host-loader inheritance | 12140-12148, 12233-12237 | 10 | **stale block**, superseded at 12236 |
| `utf8_to_utf16` doc | 12245-12257, 12319 | 4 | **stale** (NewString fallback deleted) |
| `jni_signature_for_arg` doc | 12326-12348, 12455 | 6 | 12343 + 12455 **stale** |
| `find_class_via_oop` / reanchor | 12487-12500, 12572 | 5 | 12487-12488, 12572 **stale** |
| `make_unique` | 12638-12641 | 1 | accurate (historical) |
| `make_java_array` doc | 13008-13031 | 9 | **stale** — documents a deleted param + fallback |
| `make_java_string` doc | 13082-13097, 13119, 13137-13142, 13230-13250 | 18 | **stale block**, superseded at 13237/13246 |
| `field_proxy::set` array note | 14278 | 1 | accurate |
| `store_object_oop` GC note | 14613 | 1 | **stale** ("live JNI local reference") |
| `store_string` doc | 14673-14700 | 10 | **stale block**, superseded at 14699 |
| `value_t::cast_for_variant` | 14923, 14948 | 2 | **stale** (`call_jni` path gone) |
| `method_proxy::call` | 15065-15084, 15149, 15225, 15275-15300, 15339-15341, 15407 | 26 | mostly **stale**, superseded at 15071/15296 |
| `argument_matches_descriptor` | 15630 | 1 | historical, fine |
| `method_proxy` members | 15886-15910 | 8 | **stale** — documents dead members |
| `oop_type_t` / `object_base` | 15921, 15975 | 2 | accurate ("NOT a JNI global reference") |
| `is_instance_of` | 17139, 17144 | 2 | accurate |
| `read_java_string` log | 18482 | 1 | **stale** live log string |
| `global_ref` block header + doxygen | 19688-19723 | 12 | **stale — actively wrong**, contradicted by 19758-19766 |

---

## 2. The `vmhook::jni` namespace

**Opens at line 19700, closes at line 19797.** It contains **exactly one symbol**.

| Line | Symbol | Kind | Real JNI dependency? | Live? | Proposed home / name |
|---|---|---|---|---|---|
| 19700 | `namespace vmhook::jni` | namespace | **none** | live | **delete** |
| 19725-19796 | `class global_ref final` | public class | **none** — pure `void*` holder | live (no-op stub) | `vmhook::oop_pin` (or `vmhook::gc_pin`) at namespace `vmhook` scope |
| 19728 | `global_ref() noexcept = default` | ctor | none | live | ↳ |
| 19730-19733 | `explicit global_ref(vmhook::oop_t)` | ctor | none — just stores the raw oop | live | ↳ |
| 19735 | `~global_ref() noexcept = default` | dtor | none — **no release happens** | live | ↳ |
| 19737-19738 | deleted copy ctor / copy assign | — | none | live | ↳ |
| 19740-19754 | move ctor / move assign | — | none | live | ↳ |
| 19768-19771 | `oop() const noexcept -> oop_t` | observer | none | live | ↳ |
| 19776-19779 | `reset() noexcept -> void` | mutator | none | live | ↳ |
| 19784-19787 | `handle() const noexcept -> void*` | observer | none — **"retained for API compatibility"** | live, redundant with `oop()` | drop, or keep as a deprecated alias |
| 19789-19792 | `explicit operator bool()` | observer | none | live | ↳ |

Two free functions live **outside** the namespace but return its type — they are the
reason `jni::` reaches user code even for people who never type it:

| Line | Symbol | Returns | Fix |
|---|---|---|---|
| 19803-19807 | `vmhook::pin(oop_t)` | `vmhook::jni::global_ref` | retarget to `vmhook::oop_pin` |
| 19814-19823 | `vmhook::pin(const std::unique_ptr<T>&)` | `vmhook::jni::global_ref` | retarget to `vmhook::oop_pin` |

**Verdict:** `vmhook::jni` is a pure vestige. Nothing in it touches JNI. The class is a
**no-op stub** — the ctor stores a raw oop, the dtor does nothing, and the doxygen at
19703-19723 still promises `NewGlobalRef`/`DeleteGlobalRef` semantics that the honest note
at 19756-19767 then retracts. **The doc block and the implementation contradict each other
inside the same class.**

**External consumers of `vmhook::jni::global_ref`** (blast radius of the rename):

| File | Nature |
|---|---|
| `tests/jvm/modules/global_ref.cpp` | live: `using gref = vmhook::jni::global_ref;` (line 82) + ~40 `static_assert`s; several of its assertion *messages* also state deleted JNI semantics (e.g. line 148 "the OOP ctor issues NewGlobalRef") |
| `tests/test_global_ref.cpp` | no-JVM contract test |
| `README.md` | **no mention at all** — the public pin API is undocumented in the README |
| `viewer/**` | **zero references** |

**Recommended migration:** rename to `vmhook::oop_pin`, keep
`namespace jni { using global_ref [[deprecated("use vmhook::oop_pin")]] = vmhook::oop_pin; }`
for one release, then delete the namespace.

> Naming note: the viewer's generated wrapper legitimately emits a `sdk::org::lwjgl::system::jni`
> namespace from the Java package `org.lwjgl.system.JNI`
> (`build/viewer/bin/Release/generated/wrapper.hpp:622845+`). That is a *Java* package name,
> not vmhook API, and does not collide with `vmhook::jni` — but it is a good reason not to
> reserve the identifier `jni` in the library's own namespace.

---

## 3. Naming / API leakage — names that say "jni" but are pure logic

| Line | Symbol | Kind | Live? | External callers | Proposed name |
|---|---|---|---|---|---|
| 12351 | `vmhook::detail::jni_signature_for_arg<T>()` | function template, `detail` | **live, load-bearing** | **~629 refs across 12 test files** (see table below) | `detail::jvm_descriptor_for_arg<T>()` — it produces a **JVM type descriptor**, which is a *class-file* concept, not a JNI one |
| 12413 | `"vmhook::detail::jni_signature_for_arg: unique_ptr<T> arg's T must ..."` | `static_assert` message | live | compiler-visible to users | rename with the symbol |
| 12422 | `VMHOOK_LOG("… jni_signature_for_arg<{}>: unique_ptr<T> wrapper not registered …")` | log string | live | user-visible | rename with the symbol |
| 12438 | `VMHOOK_LOG("… jni_signature_for_arg<{}>: object wrapper not registered …")` | log string | live | user-visible | rename with the symbol |
| 12452-12458 | `"vmhook::detail::jni_signature_for_arg: unsupported argument type… made GetMethodID fail in jni_make_unique …"` | `static_assert` message | live | user-visible | rename **and** drop the `GetMethodID`/`jni_make_unique` rationale (both symbols are gone) |
| 13034 | `make_java_array(..., [[maybe_unused]] const bool retained_for_abi = true)` | **dead parameter** | live signature, **ignored body** | `tests/test_make_java_array_nojvm.cpp:47,52,165`; `tests/test_object_factory.cpp:296,306` pass a 4th arg | its **doc still calls it `allow_jni_fallback`** (13012, 13026) — either delete the param and fix those 5 test call sites, or at minimum rename the doc to match `retained_for_abi` |
| 15891 | `mutable void* cached_method_id{ nullptr };` | **dead member** | never read or written anywhere | none | **delete** ("Lazy caches for the JNI fallback path" — the path is gone) |
| 15892 | `mutable void* cached_class_handle{ nullptr };` | **dead member** | never read or written anywhere | none | **delete** |
| 15897 | `mutable char cached_ret_char{ 0 };` | member | never read/written outside its own decl | none | delete (same JNI-era cache family) |
| 15902 | `mutable std::string cached_effective_signature{};` | member | never read/written outside its own decl | none | delete |
| 15910 | `mutable std::string cached_keyed_signature{};` | member | never read/written outside its own decl | none | delete |

`jni_signature_for_arg` external blast radius (occurrence counts):

| File | Refs |
|---|---|
| `tests/test_signature_parsing.cpp` | 296 |
| `tests/jvm/modules/signature_parsing.cpp` | 100 |
| `tests/test_collection_type_tags.cpp` | 61 |
| `tests/test_helpers.cpp` | 37 |
| `tests/test_return_value.cpp` | 29 |
| `tests/test_make_unique.cpp` | 27 |
| `tests/test_object_factory.cpp` | 27 |
| `tests/test_traits.cpp` | 25 |
| `tests/test_api_surface.cpp` | 14 |
| `tests/test_method_overload_nojvm.cpp` | 8 |
| `tests/test_api_surface_extended.cpp` | 4 |
| `tests/jvm/modules/make_unique.cpp` | 1 (comment) |
| **total** | **~629** |

Mitigation: rename in the header and add
`template<typename T> [[deprecated]] inline auto jni_signature_for_arg() { return jvm_descriptor_for_arg<T>(); }`
so the ~629 call sites keep compiling; migrate tests in a follow-up sweep.

**Names that are already gone from the header but still named in prose:**
`call_jni`, `jni_make_unique`, `jni_new_string_utf16_local`, `jni_new_primitive_array`,
`jni_find_class_with_context_loader`, `jni_exception_clear`, `jni_delete_local_ref`,
`jni_new_global_ref`, `jni_value`, `current_jni_env`, `jni_function<N>`,
`check_callee_exception`, `allow_jni_fallback`, `inherit_host_context_classloader_for_current_thread`.

**Viewer usage of these names: ZERO.** No file under `viewer/src/` contains `jni`, `JNI`,
`global_ref`, `call_jni`, `allow_jni_fallback`, or `jni_signature_for_arg` in any casing —
not in code, comments, or string literals. Neither emitter leaks a `jni::` name into
user-facing generated C++:

- `viewer/src/wrapper_gen.hpp:530-531` — generated header includes only
  `<vmhook/vmhook.hpp>`, `<cstdint>`, `<memory>`, `<string>`; accessors are emitted as
  `get_method(name, sig)->call(...)` (line 442).
- `viewer/src/script_host.hpp:132-135` — composed script TU includes no `jni.h`; the MSVC
  command line at `script_host.hpp:212-214` carries no JNI include dir, so a user script
  *cannot* reach JNI even deliberately.

---

## 4. Stale / misleading documentation

Roughly **60 of the 128 `jni`/`JNI` textual hits are actively wrong** — they describe an
implementation that no longer exists. The worst offenders are blocks where the *original*
JNI doc was left intact and a one-line "Pure-VM: ..." note was appended below it, so the
block now says two opposite things.

### 4a. Doc blocks that contradict their own implementation (fix first)

| Lines | Area | What it claims | Reality |
|---|---|---|---|
| **19688-19698** | `global_ref` section banner | "pins the object via `NewGlobalRef` and its `oop()` always reads the object's CURRENT (post-relocation) address out of the handle slot" | ctor stores a raw `void*`; `oop()` returns it verbatim; **stale after any relocating GC** — retracted 30 lines later at 19756-19766 |
| **19703-19723** | `global_ref` doxygen | "constructor promotes it to a JNI global reference"; "destructor releases it exactly once"; "double `DeleteGlobalRef` corrupts the handle table"; "Requires a JVM (and an attached thread)" | `~global_ref() = default` — nothing is released; no JVM required |
| **13008-13031** | `make_java_array` | documents a 2-step allocation strategy with a `jni_new_primitive_array()` GC-aware fallback and a `@param allow_jni_fallback` | there is one TLAB path; the 4th param is `retained_for_abi` and is `[[maybe_unused]]` |
| **13082-13097** | `make_java_string` LENGTH HANDLING | "LONGER inputs … are built in full by the GC-aware `JNIEnv::NewString` fallback" | no fallback exists (13246-13252): over-cap inputs go through the same TLAB path, and allocation failure returns `nullptr` |
| **13133-13142** | `build_via_tlab` preamble | "its internal `make_java_array` calls pass `allow_jni_fallback=false`" | the three calls at 13156, 13178, 13194 pass **three** arguments |
| **13230-13236** | `make_java_string` path selection | "we deliberately SKIP it … and go straight to the GC-aware `JNIEnv::NewString` fallback below" | superseded by 13237-13240 immediately underneath |
| **14665-14682** | `field_proxy::store_string` doxygen | "built via the SAME … path … (`jni_new_string_utf16_local` -> `NewString`, slot 163)"; "That JNI call also returns a LOCAL REFERENCE, which roots the new String"; "If the JNI String path is unavailable we fall back to `make_java_string()`" | superseded by 14699-14700; there is only `make_java_string()`, and **the GC-rooting guarantee is gone** |
| **15275-15295** | `method_proxy::call` exception handling | "Mirror `call_jni`'s `check_callee_exception()` here so BOTH dispatch paths leave the thread clean … `ExceptionDescribe` (slot 16) … fall back to `ExceptionClear` (slot 17)" | superseded by 15296-15300: **nothing clears the pending exception** |
| **15886-15910** | `method_proxy` member block | "Lazy caches for the JNI fallback path … the single biggest speedup against pure JNI"; "so the `call_jni` dispatch uses the right signature for `GetMethodID`" | the members are never touched (§3) |
| **12127-12134** | `// --- JNI helper layer ---` banner | "Low-level wrappers around the `JNIEnv` function table. All functions … call `jni_exception_clear()` instead" | the namespace now holds `host_classloader_klass`, `klass_to_class_loader_oop`, `capture_host_classloader_klass`, `utf8_to_utf16`, `jni_signature_for_arg` — none of them JNI |
| **12137-12155 + 12231-12238** | Host context-classloader inheritance | 19-line rationale for inheriting the host loader onto attached threads | the CAS body is **empty** except a comment; the feature was removed |
| **10609-10623** | `return_value::set_arg` | 15 lines on `DeleteLocalRef` bookkeeping and "local reference table overflow" | superseded by 10624-10626 |
| **12485-12490** | `find_class_via_oop` doxygen | "Walks anchor -> getClass() -> getClassLoader() -> loadClass(name), forcing the copy visible from THAT object's loader"; "Complexity: O(JNI loadClass)"; "clears any pending JNI exception" | it validates the anchor and calls plain `find_class()` — **the loader disambiguation the function exists for no longer happens**; the `@brief` is now false |
| **12572** | `reanchor_classes_via_oop` | "Complexity: O(N · JNI loadClass)" | O(N · CLDG walk) |
| **8430-8455** | `find_class` empty-name / array-klass comments | "falls through … into `jni_find_class_with_context_loader`, which invokes `ClassLoader.loadClass("")` via JNI"; "`JNIEnv::FindClass` DOES accept array descriptors (make_java_array relies on this at 14203)" | both paths deleted; the `14203` cross-reference is also a dead line number |

### 4b. Live user-visible strings that mention JNI

These are not comments — they are emitted at runtime and will confuse users of a
"zero-JNI" library:

| Line | String |
|---|---|
| 5417 | `"… is not a HotSpot JavaThread and pure-VM cannot attach it (no JNI)."` (accurate, but leaks the term) |
| 9117 | `"… class is not loaded in the JVM yet (or VMStructs+JNI fallback couldn't resolve it)."` — **stale**, there is no JNI fallback |
| 13250 | `"… no JNI fallback (pure-VM build)."` (accurate) |
| 18482 | `"… very early bootstrap, or VMStructs+JNI both failed."` — **stale** |
| 12422 / 12438 / 12452 | the `jni_signature_for_arg` diagnostics (§3) |

### 4c. Smaller stale one-liners

1830 · 4506 · 5819 · 8379 · 11842 · 12245 · 12257 · 12319 · 12343 · 12455 · 14613 · 14923 ·
14948 · 15065-15068 · 15149 · 15225 · 15339-15341 · 15407 · 15630

### 4d. Repo docs

| File | Status |
|---|---|
| `README.md:25, 29, 385, 732` | **accurate** — already claims "no JNI or JVMTI" everywhere |
| `CHANGELOG.md` | **no entry exists for the de-JNI removal.** `grep 'pure-VM\|zero-JNI\|de-JNI\|Removed'` → 0 hits. Worse, the current `[Unreleased]` section (lines 10-67) documents *bug fixes to `method_proxy::call_jni` / `jni_value` / `write_jni_arg_to_slot`* — code that has since been deleted. Historical entries (lines 126-452) are fine as history. |
| `viewer/README.md:140-147` | accurate — only the "call a method" capability row names JNI |
| `viewer/mcp/vmhook_mcp.py:69` | tool-description string: "pure-VM introspection (no JNI/JVMTI)" — accurate |

**Estimated fix volume:** ~60 stale lines to correct, concentrated in **11 doc blocks**
(19688-19723 · 15275-15295 · 15886-15910 · 14665-14682 · 13082-13142 · 13230-13236 ·
13008-13031 · 12485-12490 · 12127-12155 · 12231-12238 · 10609-10623). Deleting the
superseded halves — every block already carries its correct "Pure-VM: …" replacement text —
removes most of them mechanically.

---

## 5. Build-level JNI dependency

### 5a. Files that actually `#include <jni.h>` — the whole repo

Repo-wide grep for `jni.h` returns **8 hits**; only **2 are real includes**.

| File | Line | Real include? | Verdict |
|---|---|---|---|
| `viewer/payload/payload.cpp` | **64** | **YES** | **deliberate and still required** — see 5c |
| `vmhook/src/speedtest.cpp` | **18** | **YES** | opt-in microbench only, double-gated |
| `CMakeLists.txt` | 219, 229 | no (comment / message string) | — |
| `vmhook/src/example.cpp` | 201 | no (comment) | — |
| `vmhook/src/speedtest.cpp` | 11, 45, 188 | no (comment / string) | — |

`vcpkg.json` declares no JDK/JNI port. No `JAVA_HOME` appears in any build file in the repo
(only in a stray `hs_err_pid*.log` crash dump under `.localci/work/`).

### 5b. Per-target verdict

| Target | Defined at | Needs `jni.h`? |
|---|---|---|
| `vmhook` (INTERFACE) | `CMakeLists.txt:113-129` | **No.** Include dirs are `vmhook/ext` only. |
| `vmhook_example` (MODULE) | `CMakeLists.txt:232-257` | **Optional, fully gated.** `find_package(JNI QUIET)` at 221-224 (never `REQUIRED`); `speedtest.cpp` is appended to the sources only at 227 under `if(JNI_FOUND)`; `${JNI_INCLUDE_DIRS}` + `VMHOOK_BENCH_USE_JNI=1` only at 247-250. Builds cleanly with no JDK — CMake prints `"<jni.h> not found, speedtest will be a no-op"` (229). |
| `vmhook_injector` | `CMakeLists.txt:261-270` | **No.** `injector/src/main.cpp` is pure Win32; only Java-ish content is `find_processes(L"javaw.exe")` / `L"java.exe"` (lines 305, 309). |
| all `vmhook_test_*` | `tests/CMakeLists.txt:8-34` | **No.** `vmhook_add_test()` links only `vmhook::vmhook`; `JNI_INCLUDE_DIRS` is never propagated into `tests/`. |
| `tests/jvm/harness.cpp` + `tests/jvm/modules/*.cpp` | globbed at `CMakeLists.txt:214-216` | **No.** Only surviving `jni::global_ref` / `vmhook::pin`. |
| MSBuild lane (`vmhook/vmhook.vcxproj`, `injector/injector.vcxproj`) | — | **No.** `vmhook.vcxproj:25` lists only `src\example.cpp`; include dirs = `$(ProjectDir)ext\`. Never compiles `speedtest.cpp`. Already literally zero-JNI. |
| `vmhook_payload` (viewer) | `viewer/CMakeLists.txt:64-65` | **YES — the only genuine consumer.** `target_include_directories(vmhook_payload PRIVATE "${VMHOOK_INCLUDE}" ${JNI_INCLUDE_DIRS})` (`PRIVATE`, does not propagate). |
| `vmhook_viewer`, `imgui`, `vmhook_cli`, `vmhook_cli_probe` (viewer) | `viewer/CMakeLists.txt:79, 91-100, 127-137` | **No** — but see the FATAL_ERROR note below. |

**`viewer/CMakeLists.txt:45-61` is the one hard gate in the repo:**

```
50: find_package(JNI QUIET)
51: if(NOT JNI_INCLUDE_DIRS)
52:     file(GLOB _vmhook_jdk_jni "C:/Program Files/Java/*/include/jni.h")
...
59: if(NOT JNI_INCLUDE_DIRS)
60:     message(FATAL_ERROR "JNI headers (jni.h) not found — install a JDK or set JAVA_HOME so the payload can invoke Java methods.")
61: endif()
```

That `FATAL_ERROR` is top-level and unconditional, so a missing JDK blocks the **entire**
viewer configure — including the GUI exe and the two CLI tools that never touch JNI. Wrapping
it in the payload target's own scope (or making the payload target optional) is the cheap fix.

### 5c. `viewer/payload/payload.cpp` — confirmed still JNI, and why

`#include <jni.h>` at **line 64**. Confirmed **live and load-bearing**. Its own banner
(line 4) claims "pure-VM, no JNI/JVMTI", but that is scoped to *enumeration*; invocation is
explicitly JNI, stated at **1283-1286**:

> `// Invoke a method via JNI, from INSIDE the trigger detour (a real JavaThread).`
> `// This works on every JDK — unlike the pure-VM call_stub, which JDK 21+ no`
> `// longer exposes through VMStructs.`

So the owner's premise is confirmed in-source. JNI is used for **four** distinct duties, not
just invocation:

| Duty | Calls | Lines |
|---|---|---|
| **1. Thread promotion** (native serve thread → JavaThread) | `GetProcAddress("JNI_GetCreatedJavaVMs")`, `JNI_GetCreatedJavaVMs`, `JavaVM::GetEnv`, `JavaVM::AttachCurrentThreadAsDaemon` | 117, 121, 125/249/1292, 127 |
| **2. Detour triggering** (get onto a valid Java frame) | `FindClass("java/lang/Runtime")`, `GetStaticMethodID("getRuntime")`, `CallStaticObjectMethod` | 205, 208, 212 |
| **3. Object allocation** (`make_java_string` "fails to allocate from this context", comment 236-242) | `NewStringUTF`, `NewObjectArray` + `GetObjectArrayElement` (the only oop→jobject bridge; comment at 1250: "JNI has no oop->jobject") | 250, 1349, 1263, 1271 |
| **4. Invocation** | `GetStaticMethodID` / `GetMethodID`, `NewObjectA`, `Call{Static,}{Void,Boolean,Byte,Short,Char,Int,Long,Float,Double,Object}MethodA` | 1308, 1309, 1384, 1399-1428 |
| ref management | `NewGlobalRef` (206, 258, 1260), `DeleteLocalRef` (206, 214, 259, 1260, 1273, 1303, 1304, 1393, 1443) | |
| exception handling | `ExceptionCheck` / `ExceptionClear` (209, 213, 251, 1264, 1272, 1310, 1350, 1375-1377), `ExceptionDescribe` (1376) | |

JNI types used: `JavaVM*`, `JNIEnv*`, `JavaVMAttachArgs`, `jclass`, `jmethodID`, `jobject`,
`jobjectArray`, `jstring`, `jvalue`, `jint/jsize/jboolean/jbyte/jshort/jchar/jlong/jfloat/jdouble`,
`JNICALL`, `JNI_OK`, `JNI_VERSION_1_6`, `JNI_TRUE/JNI_FALSE`.

**Lost if `<jni.h>` were removed:** the entire `CALL` protocol command (`stream_call`
1452-1508 → `invoke_jni` 1287), object construction (`NewObjectA`), String writes to fields
and array elements (`alloc_java_string` 243-262, used at 821, 950, 1139), String freezing,
and `run_on_java_thread` (187-234) as a whole (it early-returns `false` with no `JNIEnv`).
**Survives untouched:** all enumeration, instance scanning, field reads, primitive/oop field
writes, non-String freezing, the class-load hook, deoptimization — corroborated by
`viewer/README.md:140-147`, where only the "call a method" row names JNI.

### 5d. CI — no compile-time JDK requirement anywhere

| Job | Lines | JDK use |
|---|---|---|
| `build-and-unit-test` (`.github/workflows/ci.yml:29-192`) | 122-127 | **No `setup-java` step at all.** Plain `cmake -S . -B build` / `--build` / `ctest`. No `JAVA_HOME`, no `-DJNI_*`. This is the job that compiles the header, `vmhook_example`, the injector and all 87 registered unit tests. It only picks up `speedtest.cpp` incidentally, because GitHub runner images ship a preinstalled JDK that `find_package(JNI QUIET)` finds. |
| `jvm-windows` | 236-246 | `setup-java@v4` → `javac` fixtures → run `java.exe` + inject. **Runtime only.** |
| `jvm-linux` | 373-384 | same. **Runtime only.** |
| `jvm-macos` | 445-456 | same. **Runtime only.** |
| `registry.yml` | — | zero Java references (Python lint). |
| `.localci/run-local-ci.ps1` | 194-197 configure; 82-109, 211+, 318 | configure passes only generator + compilers; Temurin JDKs used purely for `javac` + `java.exe`. |

**Verdict: CI contains zero zero-JNI blockers.** The step names "Run JNI integration test"
(ci.yml:384, 456) are stale labels, not real JNI compilation.

### 5e. Orphaned tests — the hidden cost of the de-JNI removal

`tests/CMakeLists.txt` registers **87** of the **99** `.cpp` files in `tests/`. The **12
unregistered** files are exactly the JNI-dependent ones, dropped by commit `eaff990`
*"test(nojvm): adapt the no-JVM lane to the JNI-free header"* — *"files kept in-tree for
later salvage of the non-JNI portions"*. They no longer compile:

| Orphan file | Deleted symbol referenced in LIVE code |
|---|---|
| `tests/test_api_surface.cpp` | `method_proxy::call_jni` (static_assert 1112-1116, live call 1431), `jni::new_string_utf` (1188, 1190), `jni::get_string_utf` (1192) |
| `tests/test_api_surface_extended.cpp` | `detail::jni_new_global_ref` (642), `detail::jni_delete_global_ref` (644), `detail::jni_value`, `jni::value`/`jni::function` (545) + 14 `jni::*` forwarders |
| `tests/test_global_ref.cpp` | `hotspot::current_jni_env`, `detail::jni_new_global_ref` |
| `tests/test_make_unique.cpp` | `hotspot::current_jni_env` (live `check()` at 360) |
| `tests/test_object_factory.cpp` | 14 `vmhook::jni::*` forwarders (2176-2248) |
| `tests/test_jni_local_ref_hygiene_nojvm.cpp` | `detail::jni_delete_local_ref` (RAII deleter, ~44) — the file's entire premise |
| `tests/test_find_class_contracts.cpp` | `hotspot::current_jni_env` (234, 2221) |
| `tests/test_classloader_reanchor.cpp` | `current_jni_env` (310), `detail::jni_find_class_with_context_loader` (static_assert 1331-1333) |
| `tests/test_helpers.cpp` | `detail::jni_delete_local_ref`, `detail::jni_value` |
| `tests/test_traits.cpp` | 14 `vmhook::jni::*` forwarders + `detail::jni_value` (781, 784, 1161) |
| `tests/test_make_java_string_nojvm.cpp` | `detail::jni_new_string_utf16_local` (static_assert 64-65, live calls 195, 199) |
| `tests/test_method_call_jni_fallback_nojvm.cpp` | **NONE** — dropped by name only; touches just `method_proxy` / `value_t`. **Re-registerable as-is.** |

**Every registered test is clean** — no live reference to a deleted symbol survives in the
built suite. The JVM lane (`tests/jvm/modules/*.cpp`, globbed at `CMakeLists.txt:214-216`) is
also clean; the `call_jni` / `check_callee_exception` hits there are test-description string
literals only (`find_class_fallback.cpp:791`, `method_call_jni_fallback.cpp:1994`,
`method_throwing_call_site.cpp:1017`, `:1030`).

### 5f. ⚠ Two JVM-lane tests now assert semantics the pure-VM stub cannot deliver

`tests/jvm/modules/global_ref.cpp` **is compiled and run** (it uses only surviving symbols),
but its premise is a real `NewGlobalRef` pin:

- header comment lines 4-22: "Its constructor promotes … `.oop()` re-derives the object's
  CURRENT (post-relocation) heap address out of the [handle slot]"; the test **drops the local
  reference, forces a GC, then re-reads the sentinel through the same pin**.
- line 148 assertion message: *"global_ref must NOT be trivial (the OOP ctor issues NewGlobalRef)"*.

With the stub, the pin is **not a GC root at all**: after dropping the last Java-side
reference and forcing `System.gc()`, the object may be **collected**, and `.oop()` then hands
back a pointer into reclaimed heap. The relocation checks are `[INFO]`-gated
(`g_survive_attainable`, lines 332-340), so the module is unlikely to hard-FAIL — but it is
now **reading freed memory to produce that `[INFO]`**, on every JDK, in CI.

The orphaned `tests/test_global_ref.cpp:376-398` is even more explicit: it asserts
`non_null_oop_without_jvm_is_empty` / `..._oop_is_null` / `..._handle_is_null` for seven fake
addresses. The stub stores whatever it is given, so all three checks would now **fail** if
the file were re-registered.

---

## 6. Prioritized checklist — to reach literal zero-JNI

### P0 — kills the `jni::` identifier in user code (the owner's stated goal)

1. **Rename `vmhook::jni::global_ref` → `vmhook::oop_pin`** at namespace `vmhook` scope
   (19725-19796). Retarget both `pin()` overloads (19803, 19814). Leave
   `namespace jni { using global_ref [[deprecated]] = oop_pin; }` for one release, then
   delete lines 19700 & 19797. **Only external consumers:** `tests/jvm/modules/global_ref.cpp`
   (line 82 + ~40 assertion messages) and `tests/test_global_ref.cpp`. Viewer: zero impact.
2. **Rewrite the `global_ref` doc** (19688-19723) to state what it actually is: a move-only
   non-owning oop holder with **no GC root**. Today the block promises `NewGlobalRef`
   semantics the class does not implement — this is the single most dangerous doc in the
   header, because a user who believes it will hold a stale pointer across a GC.
3. **Rename `detail::jni_signature_for_arg` → `detail::jvm_descriptor_for_arg`** (12351) with
   a deprecated alias so the ~629 test references keep building. Update the four diagnostic
   strings (12413, 12422, 12438, 12452) and drop the `GetMethodID`/`jni_make_unique`
   rationale from 12452-12458.

### P1 — delete dead JNI-era ballast

4. **Delete `cached_method_id` (15891) and `cached_class_handle` (15892)** and their comment
   block (15886-15890). Verified unreferenced. While there, audit `cached_ret_char` (15897),
   `cached_effective_signature` (15902), `cached_keyed_signature` (15910) — same situation.
5. **Resolve `make_java_array`'s 4th parameter** (13034). Either delete
   `retained_for_abi` and fix the 5 test call sites
   (`tests/test_make_java_array_nojvm.cpp:47,52,165`, `tests/test_object_factory.cpp:296,306`),
   or keep it and rewrite the `@param allow_jni_fallback` doc (13026) to match the real name.
   Delete the two-step allocation-strategy doc at 13008-13018 either way.
6. **Empty `capture_host_classloader_klass`'s CAS body** (12228-12238) is now a
   comment-only success branch; either restore a pure-VM equivalent or simplify the function
   and delete the 19-line rationale at 12137-12155.

### P2 — stop emitting "JNI" at runtime

7. **Fix the two stale log strings**: 9117 (`"VMStructs+JNI fallback"`) and 18482
   (`"VMStructs+JNI both failed"`). Neither fallback exists.
8. Consider rephrasing 5417 and 13250 to avoid the term entirely
   ("cannot attach a non-Java thread without a VM call" / "TLAB allocation failed").

### P3 — the doc sweep

9. **Delete the superseded half of the 11 contradictory blocks** listed in §4a. Each already
   has its correct pure-VM replacement text immediately below; the fix is deletion, not
   rewriting.
10. **Fix the two doxygen `@brief`s that are now false**: `find_class_via_oop` (12485-12490)
    no longer disambiguates by loader, and `reanchor_classes_via_oop` (12572) no longer
    triggers class loading. These are **behavioural regressions hidden by their docs** — the
    NPNOQOL multi-classloader use case they were written for is silently broken.
11. **Add a CHANGELOG entry for the de-JNI removal** and prune the `[Unreleased]` fixes that
    describe deleted `call_jni` / `jni_value` / `write_jni_arg_to_slot` code (lines 10-67).
12. Sweep the ~19 stale one-liners in §4c.

### P4 — build-level

13. **Nothing in the core repo blocks literal zero-JNI.** To finish it:
    - Delete `vmhook/src/speedtest.cpp` and the `find_package(JNI)` block at
      `CMakeLists.txt:218-230` + `247-250`, plus the `VMHOOK_BENCH_USE_JNI` guards at
      `vmhook/src/example.cpp:8-10, 203-205`. Cost: the "pure JNI baseline" leg of the
      microbench (`speedtest.cpp:129-161` and the ratio print at 172-180). The vmhook leg
      (67-127) is JNI-free and survives.
    - Rename the two stale CI step labels `"Run JNI integration test"`
      (`.github/workflows/ci.yml:384, 456`) — they compile no JNI.
    - Leave `vmhook/src/example.cpp:266-269` (`JNI_OnLoad` returning `0x00010008`) alone:
      JVM ABI contract, no `jni.h`.
14. **Scope `viewer/CMakeLists.txt`'s `FATAL_ERROR` (line 60) to the payload target** so a
    machine without a JDK can still configure and build `vmhook_viewer`, `vmhook_cli`, and
    `vmhook_cli_probe`. Today a missing JDK blocks the whole viewer configure.
15. **`viewer/payload/payload.cpp`'s `<jni.h>` is the last one to go — see §7 for the plan.**
    Three of its four duties are architectural and dissolve under a detour-pump; only method
    invocation is a genuine blocker, and it is one eleven-line function
    (`find_call_stub_entry`, 14771-14783) with no fallback tier.
16. **Decide the fate of the 12 orphaned test files (§5e).** Re-register
    `tests/test_method_call_jni_fallback_nojvm.cpp` immediately (it is already clean and just
    dropped by name). For the other 11, salvage the non-JNI assertions — the suite silently
    lost ~12 files of coverage including the entire `test_traits.cpp`, `test_helpers.cpp` and
    `test_api_surface.cpp` API-surface lanes.
17. **⚠ Highest real-world risk in the whole audit — `tests/jvm/modules/global_ref.cpp`.**
    It runs in CI on every JDK, drops the last Java reference, forces a GC, and then reads
    through a pin that is no longer a GC root (§5f). Either make `oop_pin` a real root again
    (needs a VM call — impossible pure-VM), or rewrite the module to stop dereferencing a pin
    across a forced GC. Fix the assertion message at line 148 either way.

---

## 7. Zero JNI **with every feature still working** — the actual plan

The owner's requirement is stricter than the checklist above: delete JNI *and* keep method
invocation, object construction, and String writes working in the viewer. This section is the
feasibility answer.

### 7a. The four JNI duties are not four problems — they are one

`viewer/payload/payload.cpp` uses JNI for four duties (§5c). Three of them exist **only to
manufacture a valid Java execution context from a cold native thread**:

| Duty | Why JNI today | Pure-VM status |
|---|---|---|
| 1. Thread promotion (`AttachCurrentThreadAsDaemon`) | the viewer's socket-serve thread is a native thread the JVM has never seen | **dissolvable — architectural** |
| 2. Detour triggering (`Runtime.getRuntime` via JNI) | needed to *get onto* a JavaThread on demand | **dissolvable — architectural** |
| 3. Allocation (`NewStringUTF`, `NewObjectArray`) | `make_java_string` "fails to allocate from this context" (payload.cpp:236-242) — no TLAB on an attached-native thread | **dissolvable — follows from 1** |
| 4. **Invocation** (`Call*MethodA`, `NewObjectA`) | JDK 21+ dropped `StubRoutines::_call_stub_entry` from VMStructs | **the one real blocker** |

Duties 1-3 vanish under a single architectural change: **stop entering Java from native, and
instead let Java come to you.**

**The detour-pump.** vmhook can already hook any method. Hook one the JVM calls on its own
(any per-frame / per-tick method in the target app), and let the detour drain a native work
queue: each queued item executes on a **real JavaThread, inside a real interpreter frame,
with a valid frame anchor, thread state, and TLAB**. Results are posted back to the socket
thread. No attach, no native→Java entry, no JNI.

The payload is already 80% of the way there — it *already* runs invocation inside a detour
(`payload.cpp:1283-1286`). It just uses JNI to *kick* the detour instead of waiting for the
JVM to call the hooked method naturally. The only cost of the change is latency: "next time
that method runs" (sub-millisecond in any GUI/game loop) instead of immediate. For a viewer
that is free.

Once every Java-touching operation runs in that context, `vmhook::make_java_string()` and
`make_java_object()` work — same context vmhook's own JVM test modules already allocate in —
so duty 3 goes away with duty 1.

### 7b. Duty 4 — the only hard problem, and it is narrower than it looks

`vmhook::detail::find_call_stub_entry()` (**14771-14783**) is **eleven lines**: one
`iterate_struct_entries("StubRoutines", "_call_stub_entry")`, one deref, one validity check.
There is **no fallback of any kind**. When that VMStructs entry is absent, `method_proxy::call`
gives up at 15068-15079 and the whole invocation feature is dead — which is exactly why the
payload reaches for JNI.

So "zero JNI with all features" reduces to: **obtain the call-stub address on JDK 21+ without
VMStructs.** The stub itself still exists at runtime — it is generated into the StubRoutines
code buffer during VM init. Only the *published static-field address* is missing.

Escalation ladder, cheapest first:

| # | Approach | Effort | Notes |
|---|---|---|---|
| **0** | **Confirm what is actually exported.** Dump every VMStructs entry whose type string is `StubRoutines` on live JDK 21 / 26 and read the real list. | **hours** | The viewer can already dump the table. This is the gate: it decides whether this is a 1-day fix or a 2-week one. Today's code asks for exactly one field name and concludes "gone" — it may simply have been *renamed* or *re-typed* by the JDK 21 stub refactor (stubs were split into initial/continuation/compiler blobs). **Do this before anything else.** |
| **1** | If a *sibling* `StubRoutines::_*` field is exported, derive `_call_stub_entry` from it. All StubRoutines statics live contiguously in libjvm's data segment. | days | Self-validating: a candidate is correct only if it points inside the stub code blob and matches the call-stub prologue. Reject and fall through otherwise. |
| **2** | Resolve the symbol from the module itself — export table (`jvm.dll`) or `.symtab`/`.dynsym` (`libjvm.so`/`.dylib`). | days | The JVM already exports `gHotSpotVMStructs` as a data symbol, so data exports are not off-limits. Platform-specific but vmhook already has an `os::` layer with `get_proc_address`. |
| **3** | Pattern-scan the stub blob for the call-stub prologue. | days | Fragile across JDK builds; use only as a last-resort tier behind 1 and 2. |
| **4** | **Hand-roll the entry frame** — do what the call stub does: set the `JavaFrameAnchor` (`last_Java_sp`/`_pc`/`_fp`), transition thread state, marshal args into the interpreter's expected layout, jump to `Method::_from_interpreted_entry`, restore. | weeks | The genuine fallback if 0-3 all fail. |

**Why option 4 is far more tractable here than it sounds:** the classic call stub exists to
build a *native→Java entry frame* for a cold native thread. Under the 7a architecture you are
**already inside a Java frame on a JavaThread** — the anchor is valid, the thread state is
already `_thread_in_Java`, the frame-walker is already happy. A nested Java call from a
detour needs a small fraction of the ceremony.

**And the header already owns every primitive it would need:**

| Primitive | Where |
|---|---|
| `Method::_from_interpreted_entry` read/write | 2716-2750, 3227-3250 |
| `Method::_i2i_entry` read | 2667-2708 |
| `Method::_from_compiled_entry` read/write | 3256-3320 |
| `java_thread::set_thread_state()` | 4997-5013 |
| thread-state transition already used around a stub call | 7617 |
| BasicType mapping for the stub ABI | 14789-14806 |
| the full Windows-x64 stub ABI, already documented | 14764-14769 |

Nothing new has to be invented — the work is to add tiers 1-3 (and, if needed, 4) *behind*
the existing VMStructs lookup at 14771, leaving the warm path byte-identical.

### 7c. Two capability regressions to restore at the same time

These are already broken pure-VM today (§4a) and would otherwise be silently shipped as
"working":

- **`find_class_via_oop` (12485-12508)** no longer resolves by classloader — it validates the
  anchor and calls plain `find_class()`. The multi-classloader disambiguation it exists for is
  gone. Pure-VM fix: walk `anchor → klass → ClassLoaderData → _class_loader` and match the
  candidate copy's CLD, rather than taking the first name hit in the CLDG. All the reads
  already exist (`klass_to_class_loader_oop`, 12183-12203).
- **`method_proxy::call` (15296-15300)** leaves a thrown callee exception pending on the
  JavaThread. Pure-VM fix: a VMStructs write to `JavaThread::_pending_exception` (read it,
  surface it, null it) — the field is a normal VMStructs-exported member; no VM call needed.

### 7d. Recommended order of execution

1. **Option 0 above** — dump the live `StubRoutines` VMStructs entries on JDK 21 and 26. One
   session. Everything else is sized by the answer.
2. Build the **detour-pump** in the payload (work queue drained from a naturally-called hooked
   method). This alone deletes duties 1, 2, 3 — roughly **two thirds of payload.cpp's JNI** —
   and is independent of the call-stub work, so it can land first.
3. Add the **call-stub resolution tiers** behind `find_call_stub_entry()` (14771).
4. Restore the two regressions in 7c.
5. Only then do the cosmetic work in §6 (rename `jni::global_ref`, `jni_signature_for_arg`,
   the doc sweep) — those are safe, mechanical, and gate nothing.
6. Delete `<jni.h>` from `payload.cpp` and the `FATAL_ERROR` at `viewer/CMakeLists.txt:60`.

**Honest risk statement:** steps 1-3 are the project. If option 0 shows the call stub is
genuinely unreachable on JDK 21+ by any of tiers 1-3, then "zero JNI with every feature" costs
a hand-rolled entry frame (tier 4) with real GC/frame-walker correctness risk, per platform.
That is the only scenario in which the goal is expensive rather than merely fiddly — and it is
resolvable in a single investigation session.

### Known non-goals / cannot be fixed by renaming

- **`vmhook/src/example.cpp:269`** returns `0x00010008` from `JNI_OnLoad`. That is a JVM ABI
  contract for any injected DLL, satisfied with a hand-coded integer and **no `jni.h`**.
  Leave it.
- **`viewer/payload/payload.cpp`** needs JNI *today* (§5c), but not inherently — §7 is the
  route to removing it without losing a single feature. The blocker is not JNI itself; it is
  that the library has exactly one way to reach the call stub and no fallback when JDK 21+
  stops publishing it.
