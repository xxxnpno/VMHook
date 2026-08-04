# Handoff — 2026-08-05, 02:4x

Session goal: make vmhook **0 JNI** and make it so **the user never manages global refs or
oops** (driven by `npnoqol`). Roadmap: `docs/ROADMAP_ZERO_JNI.md`. Evidence: `docs/research/`.

## State: everything is committed AND pushed. Nothing is lost by shutting down.

`master` = `593c897`, pushed to `origin`. Local working tree has only:
- `viewer/payload/payload.cpp` — **Arno's own pre-existing edit**, untouched, unstaged.
- the `audit/` + `.claude/` deletions — **intentionally left unstaged**, never commit them
  (249 are under `audit/features` + `audit/graph`, the exact path filter for `registry.yml`;
  committing them makes that workflow red because `validate.py` would be gone).

## What landed (7 commits, `27db40e..593c897`)

| Commit | What |
|---|---|
| `ff66f47` | 12 orphaned test files restored — 86 → 97 tests |
| `3b7f8e7` | **method invocation restored** — it had never worked on any JDK |
| `db2a865` | capability gate + GC relocation detector — `global_ref` stops dangling |
| `fb1fdf2` | research notes consolidated under `docs/research/` |
| `eb8e2b8` | **`ref<T>` anchored references** + 2 JVM test modules |
| `827b238` | three JVM crashes fixed (entry-frame stack-walk truncation, `[B` klass, test UB) |
| `593c897` | roadmap updates |

Local validation at `593c897`: **100/100 ctest**, MinGW `-Werror` clean, CI-equivalent clang
clean, full build with example + injector clean. Live JVM: invocation 29/29 and refs 46/46 on
JDK 8/21/26; whole JVM suite **completes** on 21 and 26 (it crashed before).

## ⏳ THE ONE THING IN FLIGHT — check this first when you come back

**GitHub CI run `30960739495`** for `593c897`.

```
gh run view 30960739495 --json status,conclusion --jq '{status,conclusion}'
gh run view 30960739495 --json jobs --jq '[.jobs[]|{name,conclusion}]|group_by(.conclusion)|map({conclusion:.[0].conclusion,count:length})'
```

Last observed: **14 gating jobs PASSED**, 41 JVM cells queued. That is already meaningful —
the `warnings-as-errors (linux/clang)` job that had been **red on master** is now green, and
the no-JVM gate passed, which is what unlocks the JVM matrix.

**Expect the JVM matrix to still show failures.** ~525 pre-existing failures remain (the
documented "B2" baseline — `java_*` field getters, `tcs_*`, `mcj_*` which asserts a *removed*
JNI fallback is live, `htm_*`/`cmap_*`, `global_*`). None are in the invocation path. The
important comparison is against the two prior master runs, which were **both failures**:
`29374188146` (`27db40e`) and `29335856623`.

**Known still-broken, not a regression:** the JDK 8 whole-suite run does not complete. It
didn't before either (baseline crashed at `method_call_object`); now it stalls and the stop
point moves between runs — the documented `mingw · java8` fragility.

## What's next, in priority order

1. **Read the CI result** and triage the 41 JVM cells against the 525-failure baseline. Fix
   anything that is genuinely new; leave the pre-existing baseline for a separate pass.
2. **`detail::extract_frame_arg`** — the single highest-leverage next change. It is the choke
   point that turns every detour argument into a `borrowed<T>`, and the precondition for the
   hook-context consumer patterns (§3 patterns 9/10 in `consumer_requirements.md`).
3. **Phase 3 migration** — carry a `ref` through `object_base` / `field_proxy` /
   `method_proxy` / the six collection constructors. The "minimum viable intercept set" of 6
   places covering ~90% of user exposure is listed in `docs/research/api_surface_map.md` §5.2.
4. **Phase 4 zero-JNI finish** — rename `jni::global_ref` → `vmhook::oop_pin` and delete the
   `jni` namespace; rename `detail::jni_signature_for_arg` (~629 references, one mechanical
   pass); then rebuild `viewer/payload` on a **detour pump** (stop entering Java from native —
   hook a method the JVM already calls and drain a work queue) and delete its `<jni.h>`.
   Note the payload's `<jni.h>` is now *removable in principle*, because invocation works.
5. **Layer 2 — a real pin** (`ClassLoaderData::_handles` on 11+, `JNIHandles::_global_handles`
   on 8). Design in `gc_root_feasibility.md` §10. Everything shipped so far only *detects*
   relocation; nothing keeps an object alive.
6. **House-convention sweep**: `get_field("x")->get()` with no `has_value()` check is UB for
   any class that may not be loaded. It crashed the whole JVM suite once. Several modules use it.

## Small things left on the floor

- **B3**: MSVC `/WX` C4127 ×3 in `tests/test_iterate_entries_safety.cpp`. Two are
  `if (sizeof(void*) == 8u)` at 1617/2516 (`if constexpr` is the honest fix); the third at
  line 574 is **not** obviously constant and needs an actual MSVC run — don't guess. Local-only,
  invisible to CI.
- `tests/test_jni_local_ref_hygiene_nojvm.cpp` tests only a deleted forwarder — recommended
  for deletion, not deleted.
- `tests/jvm/modules/method_call_jni_fallback_nojvm.cpp` is misnamed (the fallback is gone);
  rename queued.
- Latent bugs **L1-L14** catalogued in `ROADMAP_ZERO_JNI.md` §3.2b. **L1** (barrier-less
  reference store) and **L2** (unconditional compressed-oop assumption) are shipping
  heap-corruption risks, and their fixes are the same primitives Layer 2 needs.
- A subagent drifted and wrote an unrequested C++26 reflection assessment. I deleted it twice
  as out of scope. If you actually want that analysis, ask for it deliberately.

## Useful commands

```bash
# header compile, ~5s
g++ -std=c++23 -Wall -Wextra -Wpedantic -Werror -c -Ivmhook/ext -o /dev/null <tu>.cpp

# reproduces the Linux clang CI job locally (this was thought impossible; it is how a red
# reached master). clang is NOT on PATH.
"/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/Llvm/x64/bin/clang++.exe" \
  --target=x86_64-w64-windows-gnu --sysroot=C:/msys64/mingw64 -std=c++23 \
  -Wall -Wextra -Wunused-private-field -Wno-unneeded-internal-declaration -Werror -c -Ivmhook/ext -o /dev/null <tu>.cpp

cmake -S . -B build/x -G Ninja -DCMAKE_BUILD_TYPE=Release -DVMHOOK_WARNINGS_AS_ERRORS=ON
cmake --build build/x --parallel && ctest --test-dir build/x

# restrict a JVM run to named modules (added this session, for bisecting cross-module crashes)
VMHOOK_JVM_MODULES=invocation_capability,gc_relocation_detector
```

Never use `.localci -NoBuild` as a gate — it silently reuses a stale DLL.
