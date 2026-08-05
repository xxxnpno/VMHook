# Handoff — 2026-08-05 (third session)

Goal: **finish** the zero-JNI / zero-ceremony program, use current C++ features in
`vmhook.hpp`, and add a C++20 module. Roadmap: `docs/ROADMAP_ZERO_JNI.md`.

## Status: both goals are done. Everything below is compiled and tested.

Unlike the previous session, nothing here is speculative:

- **102/102 ctest green** (MinGW g++, `-Werror`), including the two-TU ODR test.
- Every changed TU is clean under the **CI-equivalent clang** line.
- The header compiles under **both `-std=c++23` and `-std=c++26`**.
- The CMake **module target builds** (scan → dyndep → compile → link).
- A consumer that does `import vmhook;` **builds, links and runs**.

Still not run against a live JVM: the JVM matrix. That is the one remaining gate.

## Goal A — zero JNI: DONE

`#include <jni.h>` no longer appears anywhere in the project — with one deliberate exception,
`vmhook/src/speedtest.cpp`, which is the vmhook-vs-pure-JNI microbenchmark. There JNI is the
BASELINE being measured, not a dependency; deleting it would delete the measurement that shows
the pure-VM path is worth having. It is opt-in (`find_package(JNI)`) and no library or viewer
code touches it.

| Item | Status |
|---|---|
| `namespace jni` | deleted; `global_ref` → `vmhook::oop_pin` |
| `jni_signature_for_arg` | → `jvm_descriptor_for_arg` (711 refs / 14 files) |
| `MethodCallJni`, `JniLocalRef` fixtures + modules | → `MethodCallDispatch`, `RepeatCallProbe` |
| **`viewer/payload`** | **`<jni.h>` deleted — rebuilt on a detour pump** |

### The viewer, in detail

The old design attached the serve thread with `AttachCurrentThreadAsDaemon` and then FIRED the
detour with a JNI `CallStaticObjectMethod` — JNI used purely as a doorbell. The doorbell was
unnecessary: a running JVM rings it constantly on its own. The payload now hooks five methods
the JVM's own threads already call (`Thread.currentThread`, `System.nanoTime`,
`System.currentTimeMillis`, `Object.hashCode`, `Runtime.getRuntime`) and drains a work queue
from whichever fires first. Several targets rather than one because any single choice is a bet
on the target app's behaviour.

The cost is latency: a task now runs on the next natural call rather than immediately, so the
wait went 5 s → 15 s. A viewer does not care. **A fully quiescent JVM will time out** — that is
the honest answer, and it is the one real behaviour change.

The other two JNI uses collapsed: `NewStringUTF` → `vmhook::make_java_string`, and
`invoke_jni`'s ~145 lines → `method_proxy::call_packed()`.

**Arno's uncommitted `payload.cpp` edit** (`parse_ll` / `parse_ull` / `fmt_g`) is untouched and
still uncommitted — it is in field formatting, orthogonal to everything above. `invoke_vm` now
uses `fmt_g` for float/double results, so the two fit together.

### What is NOT done in Goal A

~190 stale `call_jni` / "JNI fallback" mentions remain in test comments. A bounded sweep fixed
20 lines across 16 files where the replacement was unambiguous. The rest are left on purpose:

- **`tests/jvm/modules/method_call_string.cpp`** (72 refs) is the important one. Its `call_jni`
  references are not prose — they are **modified-UTF-8 / CESU-8 byte expectations** asserted
  under a two-decode-path premise. `read_java_string` produces real UTF-8, so some of those
  expectations are probably wrong now. Rewriting them means changing what the test asserts,
  which needs a live JVM to verify, not a text editor.
- Assertion NAMES containing `call_jni` are kept: renaming them costs the CI failure-name diff
  its comparability with history, which is the technique that found the real regressions.

## Goal B — zero ceremony: all six Phase-3 intercepts DONE

| # | Intercept | Handle form |
|---|---|---|
| 1 | every detour argument | `extract_frame_arg` accepts `borrowed<W>` |
| 2 | `object_base` ctor + `get_instance` | `object<W>::self()` |
| 3 | `field_proxy` read + write | `value_t::to_borrowed<W>()` · `store_object(borrowed<W>)` |
| 4 | `method_proxy` result | `value_t::to_borrowed<W>()` |
| 5 | six collection ctors | each takes `const borrowed<W>&` |
| 6 | `make_java_*` | `new_object` / `new_array` / `new_string` |

## New this session

### `method_proxy::call_packed()` — the runtime-typed call

`call()` picks its overload from C++ argument TYPES, which a caller holding runtime values
cannot do. `call_packed(method, span<java_arg>)` is the entry point for a script, a viewer, or
an RPC that received a descriptor and a list of strings.

Making it share `call()`'s implementation meant extracting **`invoke_packed()`** — the
type-independent half (call-stub resolve, JavaThread transition, JNIHandleBlock swap, stub
invocation, result decode). Both entries funnel through one body, so the delicate part has one
implementation rather than a copy that drifts. It is declared BEFORE `call()` on purpose: GCC
does first-phase lookup for non-dependent names in template bodies.

### `vmhook.ixx` — the C++20 module

`import vmhook;`. It does not duplicate the header — it includes it in the global module
fragment and re-exports the public surface with `export using`, which is the only shape that
keeps `import` and `#include` naming the SAME entities.

This required fixing a real latent problem: the header declared **47 namespace-scope functions
`static`**, i.e. internal linkage — a separate copy of each function, and of any function-local
state, in every TU. It also made them unexportable and made any exported inline that called one
ill-formed. All 47 are now `inline`.

Opt-in via `-DVMHOOK_BUILD_MODULE=ON` (needs CMake 3.28+); OFF by default so no existing
consumer or CI cell is affected.

⚠ **GCC 15 ICEs if a consumer writes `#include <cstdio>` AFTER `import vmhook;`.**
Includes-before-import works. That is a GCC modules bug, not a defect here.

### C++26

- **Compile-time descriptors.** `detail::descriptor_of_v<T>` is a `consteval` `string_view`;
  `detail::descriptor_for<Ret, Args...>::view()` assembles `"(IJLjava/lang/String;)V"` into a
  `static constexpr std::array`. **This is where reflection pays**: an annotated wrapper's class
  name is a compile-time constant, so its descriptor never touches the runtime registry.
  `descriptor_of_v` is EMPTY for an unannotated wrapper, and that empty piece disqualifies the
  whole signature — one unknown piece is enough. `jvm_descriptor_for_arg()` returns the constant
  when there is one.
- **`= delete("reason")`** behind `VMHOOK_DELETED`, on the five invariants that ARE deletions.
  Verified on GCC 15 in C++26 mode: the reason appears in the actual error text.
  The language-mode check in that gate is not redundant — Clang defines
  `__cpp_deleted_function` in C++23 mode, accepts the syntax as an extension, then warns about
  it, which is an error under `-Werror`.
- From the previous session: `VMHOOK_HAS_REFLECTION`, `type_name<T>()` (fixed 33 mangled-name
  diagnostics), the `java_class` annotation, and `std::expected` `try_field` / `try_method`.

`VMHOOK_HAS_REFLECTION` is still 0 on every toolchain available here — Clang 21+ behind
`-freflection` only. Those branches remain **unverifiable by construction**; "it builds" is not
evidence they are right.

## What's next, in priority order

1. **Run the JVM matrix.** `.localci/run-local-ci.ps1 -Compilers mingw,msvc -Java 8,21,26`,
   then GitHub. Nothing in the last three sessions has run against a live JVM.
   `VMHOOK_JVM_MODULES=borrowed_detour_arg,method_call_dispatch,repeat_call_stability` narrows
   it. Never use `.localci -NoBuild` as a gate — it silently reuses a stale DLL.
2. **`tests/jvm/modules/borrowed_detour_arg.cpp`** has still never run on a JVM. This project's
   own "build-only risk" lesson says build-only modules ship deterministic wrong hard-asserts.
3. **Exercise the viewer against a live JVM.** The pump is a real behaviour change — the
   quiescent-JVM timeout is the thing to watch.
4. **`method_call_string.cpp`'s byte expectations** (see above).
5. **Layer 2 — a real pin.** Everything shipped only *detects* relocation.
   `store_object`'s residual window (a collection between the resolve and the `safe_write`) is a
   Layer 2 problem.
6. **The ~400-failure JVM baseline** — untouched, uninvestigated. `java_getter_*` (95),
   `htm_size_oracle_*` (16), `xchk_*` (15), `const_*` (14). Predates the de-JNI work.

## Small things left on the floor

- **B3**: MSVC `/WX` C4127 ×3 in `tests/test_iterate_entries_safety.cpp`. Two are
  `if (sizeof(void*) == 8u)` at 1617/2516 (`if constexpr` is the honest fix); the third at line
  574 is **not** obviously constant and needs an actual MSVC run — don't guess.
- Latent bugs **L1-L14** in `ROADMAP_ZERO_JNI.md` §3.2b. **L1** (barrier-less reference store)
  and **L2** (unconditional compressed-oop assumption) are shipping heap-corruption risks.
- `borrowed::operator*` returns the `access` proxy, not the wrapper (`(*b)->method()`).
- `test_global_ref.cpp` / its ctest target still carry the old name; contents were swept.

## Useful commands

```bash
# header compile, ~5s
g++ -std=c++23 -fsyntax-only -Wall -Wextra -Wpedantic -Ivmhook/ext <tu>.cpp

# the CI-equivalent clang job (clang is NOT on PATH)
"/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/Llvm/x64/bin/clang++.exe" \
  --target=x86_64-w64-windows-gnu --sysroot=C:/msys64/mingw64 -std=c++23 \
  -Wall -Wextra -Wpedantic -Wunused-private-field -Wno-unneeded-internal-declaration \
  -Wno-unused-function -Werror -fsyntax-only -Ivmhook/ext <tu>.cpp

cmake -S . -B build/x -G Ninja -DCMAKE_BUILD_TYPE=Release -DVMHOOK_WARNINGS_AS_ERRORS=ON
cmake --build build/x --parallel && ctest --test-dir build/x

# the module
cmake -S . -B build/mod -G Ninja -DVMHOOK_BUILD_MODULE=ON && cmake --build build/mod

# module by hand (GCC): build the interface unit first, then the consumer
g++ -std=c++23 -fmodules-ts -c -Ivmhook/ext -x c++ vmhook/ext/vmhook/vmhook.ixx -o m.o
g++ -std=c++23 -fmodules-ts -Ivmhook/ext consumer.cpp m.o -o consumer
```

```bash
# CI failure-diff triage: find REAL regressions by diffing unique [FAIL] names
# against a baseline run.  Collapsed 3098 vs 406 failures to 3 real ones.
gh run view <run> --json jobs > jobs.json
id=$(jq -r --arg n "jvm · windows · msvc · java 26" '.jobs[]|select(.name==$n)|.databaseId' jobs.json)
gh run view --job $id --log | grep -o '\[FAIL\] [A-Za-z0-9_]*' | sed 's/\[FAIL\] //' | sort -u
# gh's --jq does NOT accept --arg; pipe to jq separately.
# A cell that CRASHED early has a short list — its "new" names are mostly
# newly-REACHED tests.  Only compare cells that completed in BOTH runs.
```
