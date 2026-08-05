# Handoff — 2026-08-05

Session goal: make vmhook **0 JNI** and make it so **the user never manages global refs or
oops** (driven by `npnoqol`). Roadmap: `docs/ROADMAP_ZERO_JNI.md`. Evidence: `docs/research/`.

## State

Working tree has:
- `viewer/payload/payload.cpp` — **Arno's own pre-existing edit**, untouched, unstaged.
- the `audit/` + `.claude/` deletions — **intentionally left unstaged, never commit them**
  (249 are under `audit/features` + `audit/graph`, the exact path filter for `registry.yml`;
  committing them makes that workflow red because `validate.py` would be gone).

## The previous session's open question — answered

It pointed at CI run `30960739495`. That run was **cancelled**: the docs push (`325c77c`)
triggered cancel-in-progress. The real result is run **`30961706826`** for `325c77c`:
**34 green / 21 red**, and the 21 red are exactly the 21 JVM cells.

`warnings-as-errors (linux/clang)` — red on master since the de-JNI work — **is now green**.
That closes B1.

### JVM triage against the `27db40e` baseline (run `29374188146`)

Comparing unique `[FAIL]` names per cell:

| cell | baseline | at `325c77c` | genuinely new |
|---|---|---|---|
| msvc · java 26 | 3098 | **406** | 3 |
| msvc · java 21 | 3095 | **406** | 3 |
| mingw · java 8 | 284 | **18** | — |

Caveat: `mingw·java26` and `clang·java26` baselines **died early**, so their "new" names are
overwhelmingly tests that never *ran* before, not regressions. The msvc cells are the honest
comparison — the failure set collapsed ~7.6×.

Only **3** genuinely new assertion names existed, all in the `mcj_*` module, and all three were
stale expectations rather than library defects. Fixed this session (see below).

## What landed this session

### 1. The 3 new CI failures, fixed at the root

`tests/jvm/modules/method_call_jni_fallback.cpp` → renamed **`method_call_dispatch.cpp`**
(module name `method_call_dispatch`; `mcj_` assertion prefix deliberately KEPT so several
thousand names stay comparable against historical CI runs — that comparability is what made the
triage above possible). Sibling `test_method_call_jni_fallback_nojvm.cpp` →
`test_method_call_dispatch_nojvm.cpp`.

- `mcj_jni_fallback_is_the_live_path` asserted `!stub` on the premise that no JDK publishes
  `_call_stub_entry`. Both halves were wrong, and the fallback it guarded no longer exists.
  Now `mcj_call_stub_is_the_live_path`, asserting a live dispatcher exists at all.
- `mcj_echo_str_unicode_call_stub` expected `"caf??"` from a per-path split written against two
  lossy behaviours that were since fixed (`make_java_string` used to copy raw UTF-8 bytes into
  LATIN1; `read_java_string` used to substitute `'?'` for every char ≥ 0x80). "café" now
  round-trips byte-for-byte on every layout — one assertion, no path split.
- `mcj_exc_throw_mechanism_fired_call_stub` was **unsatisfiable**: the de-JNI removal took out
  the `ExceptionCheck` that produced the observation and left `const bool pend{ false };`
  behind, so `seen_pending` could never be non-zero. Rewritten to read the returned `value_t`
  (`threw()` / `exception_class`), which is both pure-VM and strictly stronger — it now pins
  WHICH exception each call reported (`IllegalStateException` / `ArithmeticException` /
  `IllegalStateException`) plus the value-initialised-on-throw contract. All these assertions
  are now unconditional.

### 2. Roadmap §2.6 — `detail::extract_frame_arg` now produces `borrowed<T>`

The single highest-leverage item on the previous list. A detour can now declare its receiver
and any object argument as a lifetime-checked handle:

```cpp
vmhook::scoped_hook<player>("damage",
    [](vmhook::return_value&, vmhook::borrowed<player> self, std::int32_t amount) {
        if (self) { /* self->hp() — no raw address, expires instead of dangling */ }
    });
```

Three tables had to agree, all wired: `extract_frame_arg` (produces the handle),
`jni_signature_for_arg` (`borrowed<W>` → `Lclass;`, `borrowed<void>` → `Ljava/lang/Object;`),
and `is_java_double_slot_v` (one slot).

**Library bug found while doing it:** `extract_frame_arg` called `frame->get_locals()` with no
null check. `get_locals()` survives it — it gates on `is_valid_pointer(this)` — but the member
call on a null pointer is already UB by then. GCC diagnoses exactly that under `-Wnonnull` once
a caller can be seen passing null. Guarded at the choke point.

### 3. Two more Phase-3 intercepts

- `object<W>::self()` → `borrowed<W>` (the wrapper's own instance; intercept #2).
- `field_proxy::value_t::to_borrowed<W>()` (a reference FIELD; the read half of intercept #3).

Both yield an EMPTY borrow — never an expired one — for Java null, and `to_borrowed` yields
empty for a non-reference alternative rather than a borrow of reinterpreted primitive bits.

### 4. Cleanup

`std::format("L{};", …)` replaced the three-line
`std::string sig{"L"}; sig.append(…); sig.push_back(';')` idiom at all three sites in
`jni_signature_for_arg`.

### New test files

- `tests/test_borrowed_detour_arg_nojvm.cpp` — 36 checks + the compile-time pins: trait truth
  table, descriptors, the **slot-offset table** across borrow/long/double orderings (a wrong
  slot width does not crash — it silently feeds the detour garbage — so it is pinned at compile
  time), null-frame degradation, and the `self()` / `to_borrowed()` intercepts.
- `tests/jvm/modules/borrowed_detour_arg.cpp` — live-JVM proof. Drives `HookBasic`
  **unchanged**, on the same modes `hook_basic` drives through `unique_ptr`, so the two argument
  models are asserted against identical scenarios: receiver identity across two instances,
  borrowed object arguments distinct from the receiver, Java-null arguments, and the slot table
  behind `combine(int,long,int)` and the 8-arg `manyArgs`.

## Validation status — READ THIS

- **Local no-JVM ctest: 100/100 green**, MinGW `-Werror`, before the last two intercepts.
- Every changed/new TU compiles clean under **both** MinGW g++ `-Werror` and the CI-equivalent
  clang line.
- `test_borrowed_detour_arg_nojvm` runs green (36/36) standalone.
- ⚠ **The live JVM module `borrowed_detour_arg.cpp` has NEVER been run against a JVM.** It is
  compile-validated only. Per the project's own "build-only risk" lesson, a build-only test
  module can ship a deterministic wrong hard-assert that only CI catches. Run it first.
- ⚠ No `.localci` run and no GitHub run covers the final tree — the user asked to stop running
  CI mid-session. A full local pre-flight is the first thing to do.

## What's next, in priority order

1. **Validate.** `.localci/run-local-ci.ps1 -Compilers mingw,msvc -Java 8,21,26`, then push.
   `VMHOOK_JVM_MODULES=borrowed_detour_arg,method_call_dispatch` narrows a run to the two
   modules that changed. Never use `.localci -NoBuild` as a gate — it silently reuses a stale DLL.
2. **The remaining Phase 3 intercepts** — `method_proxy`'s receiver, the six collection ctors,
   `make_java_object`/`make_java_array`/`make_java_string`, and the WRITE half of `field_proxy`
   (`store_object_oop`). These are the harder half: a write needs the address valid at the
   instant of the store, so they want the store to happen THROUGH the handle rather than to
   take one as an argument.
3. **Phase 4 zero-JNI finish** — rename `jni::global_ref` → `vmhook::oop_pin` and delete the
   `jni` namespace; rename `detail::jni_signature_for_arg` (~629 references, one mechanical
   pass); then rebuild `viewer/payload` on a **detour pump** (stop entering Java from native —
   hook a method the JVM already calls and drain a work queue) and delete its `<jni.h>`.
4. **Layer 2 — a real pin** (`ClassLoaderData::_handles` on 11+, `JNIHandles::_global_handles`
   on 8). Design in `gc_root_feasibility.md` §10. Everything shipped so far only *detects*
   relocation; nothing keeps an object alive.
5. **The ~400-failure JVM baseline.** Untouched this session and not investigated —
   `java_getter_*` (95), `htm_size_oracle_*` (16), `xchk_*` (15), `const_*` (14). These
   predate the de-JNI work.
6. **House-convention sweep**: `get_field("x")->get()` with no `has_value()` check is UB for any
   class that may not be loaded. It crashed the whole JVM suite once. Several modules use it.

## Small things left on the floor

- **B3**: MSVC `/WX` C4127 ×3 in `tests/test_iterate_entries_safety.cpp`. Two are
  `if (sizeof(void*) == 8u)` at 1617/2516 (`if constexpr` is the honest fix); the third at
  line 574 is **not** obviously constant and needs an actual MSVC run — don't guess. Local-only,
  invisible to CI.
- `tests/test_jni_local_ref_hygiene_nojvm.cpp` tests only a deleted forwarder — recommended
  for deletion, not deleted.
- Latent bugs **L1-L14** catalogued in `ROADMAP_ZERO_JNI.md` §3.2b. **L1** (barrier-less
  reference store) and **L2** (unconditional compressed-oop assumption) are shipping
  heap-corruption risks, and their fixes are the same primitives Layer 2 needs.
- `borrowed::operator*` returns the `access` proxy, not the wrapper, so the wrapper is one hop
  further than `*b` suggests (`(*b)->method()`). Pinned by a test; worth revisiting as
  ergonomics.

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
VMHOOK_JVM_MODULES=borrowed_detour_arg,method_call_dispatch
```

```bash
# how the CI triage above was done — diff unique failure names per cell against a baseline run
gh run view <run> --json jobs > jobs.json
id=$(jq -r --arg n "jvm · windows · msvc · java 26" '.jobs[]|select(.name==$n)|.databaseId' jobs.json)
gh run view --job $id --log | grep -o '\[FAIL\] [A-Za-z0-9_]*' | sed 's/\[FAIL\] //' | sort -u
# NOTE: gh's --jq does NOT accept --arg; pipe to jq separately as above.
# NOTE: a cell that CRASHED early has a short list — its "new" names are mostly newly-REACHED
# tests, not regressions.  Only compare cells that completed in both runs.
```
