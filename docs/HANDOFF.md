# Handoff — 2026-08-05 (second session)

Session goal: **finish** the zero-JNI / zero-ceremony program, plus a new ask — use current C++
features to improve `vmhook.hpp`. Roadmap: `docs/ROADMAP_ZERO_JNI.md`. Evidence: `docs/research/`.

## ⚠ READ FIRST: NOTHING IN THIS SESSION WAS COMPILED

The user asked, explicitly and mid-session, to stop running CI and to stop compiling — *"just
code for now"*. So this handoff describes a large body of **unverified** work:

- ~1300 lines of header change across 20+ edit operations, much of it applied by script.
- Two file renames with ~900 identifier substitutions.
- A C++26 reflection block that **no available toolchain can even parse** (Clang 21+ behind
  `-freflection` only; GCC hasn't merged P2996; MSVC hasn't shipped it). Its syntax is written
  against the published papers and has never been through a compiler.

**The first action next session is a build, not more features.** Expect real errors. The
scripted edits most likely to have gone wrong are flagged in "Where to look first" below.

## Working tree

- `viewer/payload/payload.cpp` — **Arno's own uncommitted edit** (40 insertions). Untouched
  this session, deliberately: see Phase 4.4 below.
- the `audit/` + `.claude/` deletions — **intentionally unstaged, never commit them** (249 are
  under `audit/features` + `audit/graph`, the exact path filter for `registry.yml`; committing
  them makes that workflow red because `validate.py` would be gone).

## Goal A — zero JNI: the header and test suite are DONE; the viewer is not

| Item | Status |
|---|---|
| `namespace jni` | **deleted**, no alias. `global_ref` → `vmhook::oop_pin` at `vmhook` scope |
| `detail::jni_signature_for_arg` | → `jvm_descriptor_for_arg`, 711 refs across 14 files |
| `MethodCallJni` fixture + module | → `MethodCallDispatch` / `method_call_dispatch` |
| `JniLocalRef` fixture + module | → `RepeatCallProbe` / `repeat_call_stability` |
| `viewer/payload/payload.cpp` | **still `#include <jni.h>`** — the last real JNI in the project |
| ~200 stale JNI mentions in test comments | **not swept** |

**Why the viewer was left alone.** Two of its three JNI uses are now trivially replaceable —
`NewStringUTF` → `make_java_string`, and `invoke_jni`'s ~120 lines → `method_proxy::call()`,
which works now. Only thread promotion genuinely needs the detour pump. But it is a ~200-line
rewrite in a file with the user's own uncommitted changes, and rewriting that blind, without a
compiler, would both risk their work and produce something unverifiable. It is the single
biggest remaining Goal-A item and it wants a session with a build loop.

**Why the ~200 comments were left.** They are prose (`call_jni`, "JNI fallback", "JNI local
ref"), not names. A sed turns explanations into nonsense — *"call() short-circuits into
call_jni()"* has no single-word fix. Two symbols named in comments no longer exist at all:
`jni::find_class_with_context_loader` and `jni_delete_local_ref`. Catalogued as roadmap 4.6.

The remaining `JNI` hits in the header are **correct**: `JNIHandleBlock` is a real HotSpot
VMStructs type the code genuinely reads, and the "no JNI" claims are accurate.

## Goal B — zero ceremony: all six Phase-3 intercepts now have handle forms

| # | Intercept | Handle form |
|---|---|---|
| 1 | every detour argument | `extract_frame_arg` accepts `borrowed<W>` *(last session)* |
| 2 | `object_base` ctor + `get_instance` | `object<W>::self()` *(last session)* |
| 3 | `field_proxy` read + **write** | `value_t::to_borrowed<W>()` · **`store_object(borrowed<W>)`** |
| 4 | `method_proxy` result | `value_t::to_borrowed<W>()` |
| 5 | the six collection ctors | each takes `const borrowed<W>&` |
| 6 | `make_java_*` | `new_object` / `new_array` / `new_string` → `borrowed<W>` |

Two are more than ergonomics:

- **`store_object`** resolves the handle immediately before the write and **refuses an expired
  one**. A raw `store_object_oop(addr)` cannot tell whether `addr` survived the gap between the
  caller reading it and the store; if it did not, the field holds a pointer into relocated
  space and the corruption surfaces arbitrarily later in unrelated code. This closes that
  window as far as a pure-VM build can. It does *not* close a collection landing between the
  resolve and the `safe_write` a few instructions later — that needs Layer 2.
- **`new_*`** exists because a fresh address is the most dangerous shape in the API: it *looks*
  trustworthy and is completely unrooted.

Invariant at every intercept: an **EMPTY** handle (Java null, failed allocation) is never an
**EXPIRED** one. Different causes, different recovery.

The collection constructors are member templates taking `const borrowed<W>&` so they can name
`vmhook::borrowed`, which is only complete ~4000 lines later in the header; the body is
instantiated at the call site, by which point it is. **This is one of the riskier scripted
edits — check it compiles.**

## Phase 4b — modern C++ (the new ask)

Before this the header used essentially none: **zero** `[[nodiscard]]`, `std::expected`,
`std::span`, `std::bit_cast`, `consteval`, or concepts. The only C++20+ feature in use was
`requires`.

- **`VMHOOK_HAS_REFLECTION`** (P2996 + P3394), gated exactly like `VMHOOK_HAS_DEDUCING_THIS`.
  - `detail::type_name<T>()` — **wired into all 33 diagnostics that emitted mangled names.**
    Someone who forgot `register_class<player>()` was told about `"6playerE"`. MSVC returns
    `"class player"`, which is why this survived: invisible to anyone testing only on Windows.
    **This one benefits every user today**, reflection or not — the fallback is what we had.
  - **`vmhook::java_class` annotation** + no-string `register_class<T>()`. Kills a real silent
    bug: `register_class<item>("com/example/Player")` binds `item` to Player's klass and every
    field read after it is nonsense at a plausible offset.
  - `jvm_descriptor_for_arg` short-circuits on the annotation at compile time, so an annotated
    wrapper's descriptor cannot disagree with the registry or degrade to `Ljava/lang/Object;`.
- **`std::expected`** — `object_base::try_field` / `try_method` → `expected<T, access_error>`.
  Not style: `get_field` collapses four causes into one empty optional, and they want different
  responses. That conflation is *why* `get_field("x")->get()` became the house idiom and why it
  has taken the whole JVM suite down.
- **`[[nodiscard]]`** on this session's new API only. A blanket sweep would turn every existing
  legitimate discard into a `-Werror` failure — not a change to make without a compiler.

## Where to look first when it fails to build

Ranked by how likely the scripted edit went wrong:

1. **`jvm_descriptor_for_arg`** — three arms were wrapped in new `if constexpr/else` blocks by
   script, then re-indented by a second script. Brace balance was eyeballed, not compiled.
2. **The six collection borrow constructors** — the member-template-naming-an-incomplete-type
   trick is correct in principle; verify the compiler agrees at each of the six.
3. **`try_field` / `try_method`** — inserted, then *moved* between access sections by a second
   script. Confirm they are `public` and that `this->instance` / `this->get_field` resolve.
4. **The `oop_pin` de-namespacing** — the class body was dedented 4 spaces by script after the
   `namespace jni {` wrapper was removed.
5. **Anything reflection-gated** — `VMHOOK_HAS_REFLECTION` is 0 everywhere available, so these
   blocks are not even parsed. They are unverified by construction; do not treat "it builds" as
   evidence they are right.

## What's next, in priority order

1. **Build it.** `cmake --build` + `ctest`, MinGW `-Werror` and the clang CI line. Then the JVM
   matrix, then push. Nothing from this session or the previous one has run against a JVM.
2. **`tests/jvm/modules/borrowed_detour_arg.cpp`** (previous session) has still never run on a
   JVM. Per this project's own "build-only risk" lesson, build-only modules ship deterministic
   wrong hard-asserts that only CI catches.
3. **Tests for this session's surface** — none were written. Needed: `access_error` /
   `error_message`, the `new_*` helpers cold, `store_object`'s expired-refusal, the collection
   borrow ctors, `method_proxy::value_t::to_borrowed`, and `type_name`'s fallback.
4. **Phase 4.4 — the viewer detour pump.** The last real JNI. Needs a build loop and Arno's
   `payload.cpp` edit resolved first.
5. **Layer 2 — a real pin** (`ClassLoaderData::_handles` on 11+, `JNIHandles::_global_handles`
   on 8). Design in `gc_root_feasibility.md` §10. Everything shipped only *detects* relocation.
   `store_object`'s residual window is a Layer 2 problem.
6. **The ~400-failure JVM baseline** — untouched, uninvestigated. `java_getter_*` (95),
   `htm_size_oracle_*` (16), `xchk_*` (15), `const_*` (14). Predates the de-JNI work.
7. **Roadmap 4.6** — the ~200 stale JNI comments.

## Small things left on the floor

- **B3**: MSVC `/WX` C4127 ×3 in `tests/test_iterate_entries_safety.cpp`. Two are
  `if (sizeof(void*) == 8u)` at 1617/2516 (`if constexpr` is the honest fix); the third at line
  574 is **not** obviously constant and needs an actual MSVC run — don't guess.
- Latent bugs **L1-L14** in `ROADMAP_ZERO_JNI.md` §3.2b. **L1** (barrier-less reference store)
  and **L2** (unconditional compressed-oop assumption) are shipping heap-corruption risks, and
  their fixes are the same primitives Layer 2 needs.
- `borrowed::operator*` returns the `access` proxy, not the wrapper, so the wrapper is one hop
  further than `*b` suggests (`(*b)->method()`). Pinned by a test; worth revisiting.
- `test_global_ref.cpp` / the `global_ref` ctest target still carry the old name; contents were
  swept to `oop_pin` but the filenames were not.

## Useful commands

```bash
# header compile, ~5s
g++ -std=c++23 -Wall -Wextra -Wpedantic -Werror -c -Ivmhook/ext -o /dev/null <tu>.cpp

# reproduces the Linux clang CI job locally (clang is NOT on PATH).  -Wno-unused-function is
# needed because this ad-hoc line is stricter than the project's cmake flags.
"/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/Llvm/x64/bin/clang++.exe" \
  --target=x86_64-w64-windows-gnu --sysroot=C:/msys64/mingw64 -std=c++23 \
  -Wall -Wextra -Wpedantic -Wunused-private-field -Wno-unneeded-internal-declaration \
  -Wno-unused-function -Werror -c -Ivmhook/ext -Itests/jvm -o /dev/null <tu>.cpp

cmake -S . -B build/x -G Ninja -DCMAKE_BUILD_TYPE=Release -DVMHOOK_WARNINGS_AS_ERRORS=ON
cmake --build build/x --parallel && ctest --test-dir build/x

# restrict a JVM run to named modules
VMHOOK_JVM_MODULES=borrowed_detour_arg,method_call_dispatch,repeat_call_stability
```

Never use `.localci -NoBuild` as a gate — it silently reuses a stale DLL.

```bash
# CI failure-diff triage: find REAL regressions in the 21-cell matrix by diffing unique
# [FAIL] names against a baseline run.  Collapsed 3098 vs 406 failures to 3 real ones.
gh run view <run> --json jobs > jobs.json
id=$(jq -r --arg n "jvm · windows · msvc · java 26" '.jobs[]|select(.name==$n)|.databaseId' jobs.json)
gh run view --job $id --log | grep -o '\[FAIL\] [A-Za-z0-9_]*' | sed 's/\[FAIL\] //' | sort -u
# gh's --jq does NOT accept --arg; pipe to jq separately as above.
# A cell that CRASHED early has a short list — its "new" names are mostly newly-REACHED tests,
# not regressions.  Only compare cells that completed in BOTH runs.
```
