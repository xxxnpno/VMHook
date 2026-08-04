# vmhook — Build & Local Validation Operator's Manual

**Scope:** how to build and validate substantial additions to
`vmhook/ext/vmhook/vmhook.hpp` **entirely locally**, without waiting on GitHub Actions.

**Audit date:** 2026-08-04 · **HEAD:** `27db40e` · **Header:** 19,824 lines · **Project version:** 0.5.3
**Machine:** Windows 11 Pro 26200, 16 cores (verified below — every timing in this doc was measured here).

> **READ THIS FIRST — the baseline is already red.**
> Before you change anything, three failures already exist on `master`. Do not spend time
> thinking you caused them. See [§0](#0-known-red-baseline).

---

## 0. Known-red baseline

Verified against GitHub run **29374188146** (`master` @ `27db40e`, 2026-07-14) and reproduced locally.

| # | What | Where it shows | Status |
|---|---|---|---|
| B1 | 3 dead private fields in the header trip clang `-Wunused-private-field` | GitHub `warnings-as-errors (linux / clang)` | **RED**, header bug |
| B2 | 284 `[FAIL]` lines in the JVM suite | **All 21** `jvm · windows · *` cells | **RED**, suite/library |
| B3 | `tests/test_iterate_entries_safety.cpp` C4127 ×3 | Local MSVC `/WX` only (**not** in GitHub CI) | **RED** locally, invisible to CI |

### B1 — the header does not build under clang `-Werror`

`vmhook.hpp:15891,15892,15897`:

```cpp
mutable void* cached_method_id{ nullptr };
mutable void* cached_class_handle{ nullptr };
mutable char  cached_ret_char{ 0 };
```

Verified with `grep -n`: these three fields are **declared and never read or written anywhere
in the header** — the only other occurrences are prose comments at lines 15066, 15903, 15904.
They are leftovers of the de-JNI effort (they used to cache `jmethodID`/`jclass` for the JNI
fallback path that was removed). CI error:

```
vmhook.hpp:15891:23: error: private field 'cached_method_id' is not used [-Werror,-Wunused-private-field]
vmhook.hpp:15892:23: error: private field 'cached_class_handle' is not used [-Werror,-Wunused-private-field]
vmhook.hpp:15897:23: error: private field 'cached_ret_char' is not used [-Werror,-Wunused-private-field]
```

**⚠ Your local clang CANNOT catch this.** I tested it explicitly (§1.4): local
`clang++ 19.1.5` (Windows, MSVC-ABI) with `-Wall -Wextra -Wunused-private-field` emits
**zero** warnings for those lines. CI uses `clang-18` on Linux. This is the single most
important blind spot in the whole local pipeline.

### B2 — the JVM suite is broadly red on Windows

`jvm · windows · mingw · java 8` reports `Summary: 4732 PASS, 284 FAIL`. Failing name prefixes
sampled from the logs:

- `htm_*` → `tests/jvm/modules/collection_hash_tree_map.cpp`
- `msp_*` → `tests/jvm/modules/method_static_portability.cpp` (`msp_arg_echo_*`, `msp_byte_*`, `msp_bool_*` …)
- plus `inst_field_iLabel_value`, `interface_field_reads_who`, `find_class_nested_anno_resolves`,
  `deep_l1Str_inherited_string_value`, `install_handle_running`, `ise_probe_java_message_roundtrip`, …

The `msp_arg_echo_*` cluster is method-**invocation** coverage, which is exactly what the
de-JNI work touched — consistent with B1 (dead JNI cache fields). Treat B1 and B2 as probably
one root cause.

`jvm · linux · *` and `jvm · macos · *` were **green** in that run — but see §4: those two jobs
are *best-effort* (they `exit 0` when `test_results.txt` is missing), so their green is weak
evidence. **`jvm-windows` is the only hard JVM gate.**

### B3 — MSVC `/WX` fails on a test file

Verified locally with `ninja -k 0` (full offender list, nothing else):

```
tests/test_iterate_entries_safety.cpp(574):  error C2220: warning treated as error
tests/test_iterate_entries_safety.cpp(574):  warning C4127: conditional expression is constant
tests/test_iterate_entries_safety.cpp(1617): warning C4127: conditional expression is constant
tests/test_iterate_entries_safety.cpp(2516): warning C4127: conditional expression is constant
```

**The header itself is `/WX`-clean under MSVC.** Only that one test file offends. Note
GitHub CI **never runs `/WX` on Windows** (§4), so this is a local-only tripwire — but it does
block a local `-DVMHOOK_WARNINGS_AS_ERRORS=ON` MSVC build from completing. Workarounds:
build MSVC without `/WX` (§1.2), or add `-- -k 0` so ninja keeps going.

---

## 1. Build system

### 1.1 Layout, targets, options

There are **two** first-party CMakeLists (the rest under `viewer/third_party/imgui/examples/`
are vendored ImGui samples and are never configured by this project):

| File | Role |
|---|---|
| `C:\repos\cpp\vmhook\CMakeLists.txt` | root — options, warning function, `vmhook` interface lib, example DLL, injector |
| `C:\repos\cpp\vmhook\tests\CMakeLists.txt` | the 86 no-JVM test executables (hand-listed, **no glob**) |
| `C:\repos\cpp\vmhook\viewer\CMakeLists.txt` | standalone ImGui viewer app — **separate project, not part of library CI** |

**Targets**

| Target | Kind | Notes |
|---|---|---|
| `vmhook` / `vmhook::vmhook` | `INTERFACE` | header-only; `target_include_directories` → `vmhook/ext` |
| `vmhook_example` | `MODULE` → `build/bin/vmhook.dll` | `vmhook/src/example.cpp` + `tests/jvm/harness.cpp` + **globbed** `tests/jvm/modules/*.cpp` (+ `speedtest.cpp` if JNI found). Built **`NO_WERROR`** by design. |
| `vmhook_injector` | `EXE` → `build/bin/injector.exe` | Windows only; `CreateRemoteThread` + `LoadLibraryW` |
| `vmhook_test_<name>` ×86 | `EXE` | one per `vmhook_add_test()` line; strictest lane (inherits `/WX` / `-Werror`) |

**Options** (root `CMakeLists.txt:95-102`)

| Option | Default | Effect |
|---|---|---|
| `VMHOOK_BUILD_EXAMPLE` | `ON` (OFF on iOS/Android) | the JVM-test DLL |
| `VMHOOK_BUILD_INJECTOR` | `ON` on Windows | `injector.exe` |
| `VMHOOK_BUILD_TESTS` | `ON` | the no-JVM ctest lane |
| `VMHOOK_WARNINGS_AS_ERRORS` | **`OFF`** | adds `-Werror` / `/WX` — but **not** to `vmhook_example` (see below) |

There is **no JVM-test toggle**. The JVM modules are not a CMake option; they are compiled
into `vmhook_example` unconditionally via the glob, and are *run* only when the DLL is
injected into a live JVM. `VMHOOK_MODULAR_HARNESS=1` is defined on that target so
`example.cpp` knows to call `vmhook_test::run_all()` (the legacy MSBuild `vcxproj` does not
define it).

**Warning sets** — `vmhook_apply_warnings()` (`CMakeLists.txt:137-181`):

```
MSVC : /W4 /permissive- /Zc:__cplusplus /utf-8 /wd4505 /wd4101 /EHa   [+ /WX if enabled]
       -DWIN32_LEAN_AND_MEAN -DNOMINMAX -DUNICODE -D_UNICODE
       -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00
GCC/ : -Wall -Wextra -Wpedantic -Wno-unused-function -Wno-unused-local-typedefs
Clang  -Wno-unused-but-set-variable -Wno-cast-function-type            [+ -Werror if enabled]
```

`vmhook_example` is passed `NO_WERROR`, which **also** adds `-Wno-unused-variable
-Wno-unused-const-variable`. So the JVM test modules are the *loosest* lane and the
`tests/vmhook_test_*` executables are the *strictest*.

Two things worth knowing:

- **`/EHsc` is scrubbed** from `CMAKE_CXX_FLAGS` at the top of the root CMakeLists so the
  per-target `/EHa` is the sole EH model. `/EHa` is load-bearing for `seh_invoke_detour`.
  Adding `/EHsc` back yields `D9025` → hard error under `/WX`. Don't touch it.
- **`vmhook_static_runtime()`** links MinGW targets `-static-libgcc -static-libstdc++ -static`
  so `java.exe`'s `LoadLibraryW` can resolve the DLL with no MSYS2 runtime on its search path.

**Build-directory convention:** everything lives under `build/<name>/`. `.gitignore` is just
`build/`, so any subdirectory is ignored. Existing examples: `build/werror/`, `build/mingw/`,
`build/msstatic/`, `build/tests-standalone/`, `build/viewer/`. This audit added
`build/audit-mingw/`, `build/audit-msvc/`, `build/audit-msvc-plain/`.

### 1.2 Toolchains actually installed on this machine — VERIFIED

| Tool | Present? | Path / version |
|---|---|---|
| `g++` / `gcc` | ✅ | `C:\msys64\mingw64\bin\g++.exe` (MSYS2 MINGW64) |
| `cmake` | ✅ | `C:\msys64\mingw64\bin\cmake.exe` |
| `ninja` | ✅ | `C:\msys64\mingw64\bin\ninja.exe` |
| `mingw32-make` | ✅ | `C:\msys64\mingw64\bin\mingw32-make.exe` |
| `cl.exe` | ✅ **not on PATH** | `…\2022\BuildTools\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\cl.exe` |
| `clang-cl.exe` | ✅ **not on PATH** | `…\2022\BuildTools\VC\Tools\Llvm\x64\bin\clang-cl.exe` — **clang 19.1.5** |
| `clang++.exe` | ✅ **not on PATH** | same dir |
| `vcvars64.bat` | ✅ | `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat` |
| VS Build Tools | ✅ | **17.14.35** (June 2026), Build Tools SKU, `installationPath` = `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools` |
| `java` / `javac` | ✅ | Oracle javapath + Adoptium **jdk-21.0.11.10** and **jdk-8.0.492.9**; CMake also finds `C:/Program Files/Java/jdk-26.0.1` for JNI |
| `make`, bare `clang` | ❌ | absent |

**All three toolchains you asked about are available.** `cl`, `clang-cl` and `clang++` require
`vcvars64.bat` first — they are not on PATH.

> ⚠ **Local clang is version-behind CI.** Local = **19.1.5**; CI pins **20.1.8** on Windows and
> **clang-18** on Linux. Clang-version-sensitive constructs in this header (notably
> `VMHOOK_HAS_DEDUCING_THIS`, which is explicitly disabled for `__clang_major__ >= 20`) will
> behave differently. Local clang is a smoke test, not a mirror.

### 1.3 Fast header-only compile checks — MEASURED

All three run from `C:\repos\cpp\vmhook`. `tests/test_header_compile.cpp` is the canonical
"include the header alone" TU.

**(a) MinGW g++ — 5.0 s, PASSES** ✅

```bash
g++ -std=c++23 -Wall -Wextra -Wpedantic -Werror \
    -fsyntax-only -Ivmhook/ext tests/test_header_compile.cpp
```

Passes even *without* the repo's `-Wno-*` suppressions, so this stricter form is fine. To match
the real CMake flag set exactly, append:

```bash
    -Wno-unused-function -Wno-unused-local-typedefs \
    -Wno-unused-but-set-variable -Wno-cast-function-type
```

**(b) MSVC `cl` — 3.5 s, PASSES** ✅

Must be run from a `vcvars64` shell. Save as `msvc_check.cmd` and run `cmd /c msvc_check.cmd`:

```bat
@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d C:\repos\cpp\vmhook
cl /nologo /std:c++latest /W4 /WX /permissive- /Zc:__cplusplus /utf-8 ^
   /wd4505 /wd4101 /EHa ^
   /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DUNICODE /D_UNICODE ^
   /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00 ^
   /Ivmhook\ext /c tests\test_header_compile.cpp /Fo%TEMP%\vmhook_cl_check.obj
echo CL_EXIT=%ERRORLEVEL%
```

> **`/wd4505 /wd4101` are mandatory.** Without them the header fails immediately with
> `C4505 'vmhook::detail::auto_repair::ensure_started': unreferenced function with internal
> linkage` at `vmhook.hpp:11596` and `C4505 'vmhook::shutdown_hooks'` at `:11748`. I hit this;
> it is the project's deliberate design ("header carries multiple by design"), not a defect.

**(c) clang — use `clang++` (GNU driver), NOT `clang-cl` — 2.6 s, PASSES** ✅

```bat
@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
set PATH=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\Llvm\x64\bin;%PATH%
cd /d C:\repos\cpp\vmhook
clang++ -std=c++23 -Wall -Wextra -Wpedantic -Werror ^
    -Wno-unused-function -Wno-unused-local-typedefs ^
    -Wno-unused-but-set-variable -Wno-cast-function-type ^
    -DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_WIN32_WINNT=0x0A00 ^
    -Ivmhook/ext -fsyntax-only tests/test_header_compile.cpp
echo CLANGXX_EXIT=%ERRORLEVEL%
```

**Why not `clang-cl`:** I ran it, and with the MSVC flag set + `/WX` it **fails** with 4 errors
the project never intended to fix:

```
vmhook.hpp:5997:  error: 'static' function 'find_hook_location' ... should be declared 'static inline' [-Wunneeded-internal-declaration]
vmhook.hpp:7563:  error: 'static' function 'common_detour' ...
vmhook.hpp:11596: error: 'static' function 'ensure_started' ...
vmhook.hpp:11748: error: unused function 'shutdown_hooks' [-Wunused-function]
```

MSVC's `/wd4505` does not map onto clang's `-Wunneeded-internal-declaration`, and CMake's
`if(MSVC)` branch is what clang-cl gets — so **clang-cl under `/WX` is a configuration this
project has never supported**. GitHub CI's `windows / clang` leg uses `clang++` (GNU driver,
MSVC ABI via `msvc-dev-cmd`), and so does `.localci`. Use `clang++`. If you insist on
clang-cl, add `/clang:-Wno-unneeded-internal-declaration /clang:-Wno-unused-function`.

### 1.4 The blind-spot test I ran (read this)

To answer "can my local clang catch what reddens the CI clang leg?", I compiled the header
with local `clang++` and `-Wunused-private-field` **explicitly enabled**:

```
clang++ -std=c++23 -Wall -Wextra -Wunused-private-field -DNOMINMAX \
        -DWIN32_LEAN_AND_MEAN -Ivmhook/ext -fsyntax-only tests/test_header_compile.cpp
→ EXIT=0, 4 warnings, NONE of them about cached_method_id / cached_class_handle / cached_ret_char
```

**Answer: no.** The B1 errors are invisible to every toolchain on this machine. Budget for one
GitHub round-trip specifically to clear the `warnings-as-errors (linux / clang)` leg.

### 1.5 Full local builds — MEASURED

**MinGW, warnings-as-errors (recommended primary gate) — 2m11 s total, PASSES** ✅

```bash
cd /c/repos/cpp/vmhook
cmake -S . -B build/werror-mingw -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_COMPILER=g++ -DVMHOOK_WARNINGS_AS_ERRORS=ON     # 3.7 s
cmake --build build/werror-mingw --parallel                        # 2m07 s, 263 targets
ctest --test-dir build/werror-mingw --output-on-failure            # 3.5 s, 86/86 PASS
```

> Note: passing `-DCMAKE_C_COMPILER=` produces `CMake Warning: Manually-specified variables
> were not used by the project: CMAKE_C_COMPILER` — the project is `LANGUAGES CXX` only.
> Harmless; omit it.

**MSVC, CI-equivalent (no `/WX`) — 2m23 s total, PASSES** ✅

```bat
@echo off
call "...\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d C:\repos\cpp\vmhook
cmake -S . -B build/msvc -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=cl
cmake --build build/msvc --parallel
ctest --test-dir build/msvc --output-on-failure
```
Result: `PLAIN_BUILD_EXIT=0`, `100% tests passed, 0 tests failed out of 86`, `3.68 s`.

**MSVC with `/WX`** — add `-DVMHOOK_WARNINGS_AS_ERRORS=ON`; it currently stops at B3. Add
`-- -k 0` to the build line to see the complete offender list rather than the first one.

### 1.6 ⚠ Do not build while the header is being saved

Observed live during this audit. `vmhook/ext/vmhook/vmhook.hpp` was under active editing;
across ~15 minutes it read 19,824 → 19,833 → 19,818 lines, and one `ls` caught it at
**0 bytes** mid-save. A `.localci` build launched during that window failed with:

```
tests/test_seh_invoke_detour.cpp:65:40: error: 'vmhook' was not declared in this scope
```

That is a **truncated header**, not a real error — the failing file is a *test*, and the
namespace itself had vanished. If you see a burst of `'vmhook' was not declared`,
`does not name a type`, or `expected ')' before '{'` errors in files you did not touch,
re-run the 5 s `g++ -fsyntax-only` gate before believing any of it. Save, wait, then build.

---

## 2. Test architecture

Two entirely separate lanes.

### 2.1 No-JVM lane (`tests/*.cpp` → ctest)

- **86 `vmhook_add_test()` registrations** in `tests/CMakeLists.txt` → **86 ctest tests**
  (verified: `grep -c '^vmhook_add_test'` = 86, `ctest` reports 86).
- Each is a standalone executable: header-compile checks, ODR (two TUs), pure-logic
  (signature parsing, `decode_u5`, compressed-oop codec, `value_t` conversions), and the
  `vmhook::os` layer (page protect, `safe_read`, `query_region`).
- **Registration is MANUAL.** `tests/CMakeLists.txt` has **no glob**; every test is a hand-written
  `vmhook_add_test(<name> SOURCES test_<name>.cpp)` line.

**Adding a no-JVM test**

1. Write `tests/test_<name>.cpp` with a `main()` returning non-zero on failure.
2. Add `vmhook_add_test(<name> SOURCES test_<name>.cpp)` to `tests/CMakeLists.txt`.
3. Re-configure (`cmake -S . -B build/<name>`) — adding the line does not auto-trigger.

**⚠ 12 orphan test files exist right now** — present on disk, never registered, therefore never
compiled and never run. Verified by cross-referencing every `tests/test_*.cpp` against
`tests/CMakeLists.txt`, and confirming nothing `#include`s them:

```
test_api_surface.cpp              test_helpers.cpp
test_api_surface_extended.cpp     test_jni_local_ref_hygiene_nojvm.cpp
test_classloader_reanchor.cpp     test_make_java_string_nojvm.cpp
test_find_class_contracts.cpp     test_make_unique.cpp
test_global_ref.cpp               test_method_call_jni_fallback_nojvm.cpp
test_object_factory.cpp           test_traits.cpp
```

Note `test_global_ref.cpp`, `test_make_java_string_nojvm.cpp` and
`test_method_call_jni_fallback_nojvm.cpp` — all three are de-JNI-era casualties. If you plan to
re-enable any, expect them to fail: they assert against the *old* JNI-backed implementation
(see §5).

**Invocation**

```bash
ctest --test-dir build/<name> --output-on-failure              # ~3.5 s for all 86
ctest --test-dir build/<name> --output-on-failure -R os_       # subset by regex
ctest --test-dir build/<name> --output-on-failure -C Release   # needed for multi-config generators only
```

### 2.2 JVM lane (`tests/jvm/` → injected DLL)

- `tests/jvm/harness.hpp` + `harness.cpp` = the registry/runner.
- `tests/jvm/modules/*.cpp` = **83 modules**, one per feature.
- **Registration is AUTOMATIC** — root `CMakeLists.txt:214-216` uses
  `file(GLOB … CONFIGURE_DEPENDS "tests/jvm/modules/*.cpp")`. Dropping a `.cpp` in that
  directory is the entire registration step.

**Adding a JVM test module**

```cpp
// tests/jvm/modules/my_feature.cpp
#include <vmhook/vmhook.hpp>
#include "../harness.hpp"

VMHOOK_JVM_MODULE(my_feature)
{
    ctx.check("my_feature_does_x", cond);        // -> [PASS]/[FAIL], bumps counters
    ctx.record("[INFO] observed: ...");          // -> raw line, no counters
}
```

The macro emits a file-static body function plus an anonymous-namespace registrar whose ctor
calls `vmhook_test::register_module(#modname, &modname##_body)` at DLL load.

`VMHOOK_JVM_MODULE_PRIORITY(name, vmhook_test::priority::first|normal|last)` pins run order.
**Use it — never an `aaa_`-filename trick.** From `harness.hpp:77-87`: GNU ld runs a TU's static
initializers in *reverse* link-line order, so an alphabetically-first file registers *last*.
`run_all()` `stable_sort`s by explicit priority, which is order-independent across all five
toolchains.

**`context` API** (`harness.hpp:23-66`) — there are **no assertion macros**, only `std::function`
members:

| Member | Use |
|---|---|
| `check(name, ok)` | the hard assertion → `[PASS]`/`[FAIL]` |
| `record(line)` | free-form line, counters untouched; the `[INFO]` convention lives here |
| `run_probe(set_go, get_done)` | the **only** way to make a hooked Java method actually run — raises the fixture flag, polls `get_done()` every 1 ms up to ~5 s |
| `reset()` | `vmhook::shutdown_hooks()` — called by `run_all()` after **every** module |
| `set_auto_repair(bool)` | `run_all()` calls `(false)` once before the loop |

**HARD vs INFO is a convention, not an API.** CI greps only for `^\[FAIL\]`. The idiom for a
soft assert is an explicit `if (gate) { ctx.check(...) } else { ctx.record("[INFO] ..."); }`, or a
local helper — the one named example is `pass_or_info()` in
`tests/jvm/modules/field_arrays_object.cpp:551-565`.

**Per-module lifecycle** (`harness.cpp:254-361`): `set_auto_repair(false)` once → stable-sort by
priority → per module: `[INFO] === module: NAME ===` → run under crash containment (MSVC
`__try/__except`; MinGW/clang-cl a process-wide VEH + `setjmp`/`longjmp`; POSIX `try/catch`) →
`[INFO] --- module NAME done ---` → **unconditional `ctx.reset()`**. Modules must use
`vmhook::scoped_hook` in a block scope and never call `shutdown_hooks()` themselves.

**Running the JVM lane** is not a ctest thing — it needs a live JVM plus injection. Use
`.localci` (§3), or by hand:

```bat
javac -encoding UTF-8 -d out example\vmhook\*.java example\vmhook\fixtures\*.java
start /b java -Xmx4g -Xmn3g -cp out vmhook.Main
build\bin\injector.exe <pid>
type test_results.txt
```

`test_results.txt` is opened as a **relative** path by `example.cpp:154`, so it lands in the
**JVM process's cwd**. A run is only valid if it ends with a `TOTAL:` line — its absence means
the JVM died mid-suite.

**Timing:** ~15-25 s per JVM cell once the JDK is extracted (5 s launch wait + inject + suite).

---

## 3. The local CI harness (`.localci/`)

`C:\repos\cpp\vmhook\.localci\run-local-ci.ps1` (379 lines) + `README.md`. Mirrors the GitHub
**Windows** lanes only (`build-and-unit-test` + `jvm-windows`).

### Invocation

```powershell
cd C:\repos\cpp\vmhook
.\.localci\run-local-ci.ps1                                  # all detected compilers x all 7 JDKs
.\.localci\run-local-ci.ps1 -UnitOnly                        # build + no-JVM ctest only, no JVM cells
.\.localci\run-local-ci.ps1 -Compilers msvc -Java 8          # one cell
.\.localci\run-local-ci.ps1 -Compilers mingw -Java 8,17,26
.\.localci\run-local-ci.ps1 -NoBuild -Compilers mingw -Java 17   # reuse existing build
.\.localci\run-local-ci.ps1 -Parallel 1                      # sequential
.\.localci\run-local-ci.ps1 -Parallel 8
```

Parameters: `-Java` (default `8,11,17,21,24,25,26`), `-Compilers` (default = auto-detect),
`-NoBuild`, `-UnitOnly`, `-TimeoutSec 120`, `-Parallel 0` (auto).
Exit code **0** = all cells green, **1** = some not green, **2** = no usable compiler.

### Matrix

**Compilers** — auto-detected: `mingw` (needs `g++` on PATH), `msvc` and `clang` (need VS via
`vswhere -products *`; `clang` accepts `clang++` on PATH *or* the VS-bundled one).
**On this machine all three are detected** — `mingw`, `msvc`, `clang` — since VS Build Tools
17.14 is installed with both the MSVC and the LLVM component. Note the script's `clang` cell
uses **`clang++`**, the GNU driver, not `clang-cl`.

**JDKs** — 7 majors: 8, 11, 17, 21, 24, 25, 26. Full matrix = 3 × 7 = **21 cells**.

### Cached JDKs on this machine

| Path | State |
|---|---|
| `.localci\cache\temurin-{8,11,17,21,24,25,26}-windows-x64.zip` | ✅ **all 7 present**, ~1.05 GB total |
| `.localci\jdks\<major>\` | ⚠ **EMPTY** — nothing extracted yet |
| `.localci\builds\{mingw,msvc,clang}\` | ✅ present; `mingw\bin\` has `vmhook.dll` + `injector.exe` |
| `.localci\work\<compiler>-java<major>\` | ✅ all 21 dirs present from prior runs |

**Consequence:** the next run does **zero network I/O** (archives are cached), but pays a
one-time `Expand-Archive` per JDK. Budget a few minutes of extraction on the first full run;
subsequent runs skip it (`Get-JavaHome` short-circuits on finding `javac.exe`).

### Per-cell behaviour (mirrors `ci.yml` exactly)

1. Build DLL + injector **once per compiler**, CMake **Release**, Ninja — *without*
   `VMHOOK_WARNINGS_AS_ERRORS`. Then run the no-JVM `ctest` lane.
2. `javac -encoding UTF-8 -d out example\vmhook\*.java example\vmhook\fixtures\*.java` **with that
   JDK's javac** — catches per-version fixture issues the C++ build never sees.
3. `java -Xmx4g -Xmn3g -cp out vmhook.Main`, sleep 5 s, `injector.exe <pid>`, wait ≤120 s.
4. Verdict from `test_results.txt`: any `[FAIL]` → FAIL; no `TOTAL:` → INCOMPLETE (crash).

Statuses: `PASS`, `FAIL`, `BUILD-FAIL`, `CTEST-FAIL`, `JAVAC-FAIL`, `INJECT-FAIL`, `STALL`,
`NO-RESULTS`, `INCOMPLETE`.

### Parallelism & runtime

`-Parallel 0` (default) = `min(cores-1, floor((RAM_GB-4)/5))` capped at **6**. Each cell is a
real `java -Xmx4g`, so RAM is the binding constraint. Builds run **serially** first, then cells
fan out as isolated `-NoBuild` subprocesses in their own work dirs.

**Estimated wall time on this box** (16 cores): `-UnitOnly` ≈ **6-8 min** (three ~2 min builds
+ 3× ctest). Full 21-cell matrix ≈ **15-25 min** at parallelism 5-6. A single `-NoBuild` cell
≈ **15-40 s**. **JDK extraction is cheap here** — measured: `Expand-Archive` of the cached
205 MB JDK 21 zip plus a full cell completed in **15 s** total, so budget ~2 min, not more,
for extracting all 7 on the first full run.

### Verified runs of the harness (2026-08-04)

I executed the harness twice; both are instructive.

**Run A — `-NoBuild -Compilers mingw -Java 21`** → `15 s`, verdict
`INCOMPLETE -- no TOTAL line (crash)`. Diagnosis from `.localci\work\mingw-java21\`:
`injector_stdout.txt` says `[OK] Injection successful.`, `java_stdout.txt` reaches
`[INFO] loaded 92 fixture(s).`, and **`test_results.txt` is 0 bytes** — the suite never
produced a line. Root cause: **`.localci\builds\mingw\bin\vmhook.dll` is dated `Jun 23 11:33`,
six weeks stale.**

> ⚠ **`-NoBuild` silently reuses whatever is in `.localci\builds\<compiler>\`, with no staleness
> check.** `Build-Compiler` only tests `Test-Path` on the DLL and injector. Use `-NoBuild` **only**
> to re-run cells within a session where you *just* built. Never as your pre-push gate.

**Run B — same cell WITH a build** → `96 s`, verdict `BUILD-FAIL`, and the harness correctly
pointed at `.localci\logs\build-mingw.log.build.out`, which named the failing TU
(`test_seh_invoke_detour.cpp:65: error: 'vmhook' was not declared in this scope`). That was a
**transient artifact of the header being saved mid-build** (see §1.6), not a repo defect — a
`g++ -fsyntax-only` immediately afterwards returned `EXIT=0`.

**Net: the harness works correctly**, detects all three compilers on this machine
(`detected [mingw, msvc, clang]`), extracts cached JDKs without network I/O, and reports
actionable diagnostics. The two failures above were an environmental stale-artifact and a
concurrent-edit race, not harness bugs.

### Watching a run live

```powershell
Get-Content .\.localci\work\msvc-java21\test_results.txt -Wait -Tail 30   # per-assertion
Get-Content .\.localci\logs\cell-msvc-java21.log -Wait -Tail 30           # cell transcript
Get-Content .\.localci\logs\build-msvc.log.build.out -Wait -Tail 30       # build output
```

### Documented BLIND SPOTS ⚠

From `.localci/README.md`, the harness source, and the repo's history:

1. **Windows only.** No Linux, no macOS, no Android/iOS legs. Therefore **libc++ and
   libstdc++-on-Linux gaps are invisible** — the whole class of "missing `#include` that
   libstdc++ pulls in transitively but libc++ does not" (commit `3300494`).
2. **No `-Werror` at all.** The harness never passes `VMHOOK_WARNINGS_AS_ERRORS=ON`. The
   GitHub `warnings-as-errors` job is Linux gcc-14 + clang-18. **`.localci` cannot catch B1.**
3. **MinGW-only coverage is the weakest.** From the README: the MSVC-ABI cells (`msvc`,
   `clang`) are where the #38 GC-safepoint stalls and the java8 `on_exception` issues
   reproduce, "**which MinGW cannot**".
4. **MinGW misses clang diagnostics.** `-Wunused-const-variable` on namespace-scope
   `constexpr`, `-Wunused-lambda-capture` on a captured `constexpr` (commits `445e140`,
   `04f017a`) — MinGW g++ is silent, clang `-Werror` is not.
5. **MinGW misses MSVC-ABI failures**: `/permissive-` overload ambiguity from `value_t`
   brace-init (`4024802`, `7d79a36`, `dc73048`, `962482c`), and missing `<array>`-style
   includes MSVC-STL doesn't provide (`80b5010`).
6. **POSIX raw-fault blindness.** From `7d79a36`: "no-JVM tests that fabricate unmapped
   addresses are POSIX-unsafe … they RAW-FAULT on POSIX (contained on mingw's
   `ReadProcessMemory`). The mingw `.localci` cannot catch either; **GitHub is the
   cross-platform oracle**."
7. **macOS static-init segfaults** (`d3edc80`, `6fccf13`) — runtime-only, macOS-hardware-only.
8. **More concurrency makes timing-sensitive watchers flake more.** README calls this "a
   feature for surfacing the i2i-vs-JIT fragility, not a bug" — a flaky
   `on_class_loaded`/`on_exception` at high `-Parallel` is not necessarily your regression.
9. **Local clang is 19.1.5 vs CI's 18 (Linux) / 20.1.8 (Windows).** Empirically it misses B1.

---

## 4. GitHub CI

`.github/workflows/ci.yml` (605 lines) and `registry.yml`. Triggers: push to `master`, PRs to
`master`, `workflow_dispatch`. `concurrency: cancel-in-progress: true` keyed on
`workflow-ref` — **every push cancels the in-flight run, so batch your commits and push once**.

### Jobs — **55 total**

| Job | Count | Runs on | Validates |
|---|---:|---|---|
| `build-and-unit-test` | 6 | win×3, linux×2, macos | configure + build + `ctest` (**no** `-Werror`); uploads `vmhook.dll`/`injector.exe`/`.so`/`.dylib` |
| `java-versions` | 1 | ubuntu | emits the JSON version list |
| `jvm-windows` | **21** | windows | 3 compilers × 7 JDKs — **the hard JVM gate** |
| `jvm-linux` | 14 | ubuntu | 2 compilers × 7 JDKs — **best-effort** |
| `jvm-macos` | 6 | macos | 7 JDKs minus java 8 (no Temurin arm64) — **best-effort** |
| `android-build` | 2 | ubuntu | NDK cross-compile, `arm64-v8a` + `x86_64`, build-only |
| `ios-build` | 2 | macos | Xcode, `iphoneos` + `iphonesimulator`, build-only |
| `warnings-as-errors` | 2 | ubuntu | **the only `-Werror` legs**: gcc-14 + clang-18 |
| `msbuild-solution` | 1 | windows | legacy `vmhook.slnx`, `/p:PlatformToolset=v143` |

### Gating order

```
build-and-unit-test ─┐
                     ├─→ jvm-windows (21)   ← HARD gate
java-versions ───────┘   jvm-linux   (14)   ← best-effort
                         jvm-macos   (6)    ← best-effort

android-build / ios-build / warnings-as-errors / msbuild-solution   ← INDEPENDENT, no `needs:`
```

**`needs: [build-and-unit-test, java-versions]`** — so **any** failure in **any** of the 6 build
cells blocks **all 41 JVM cells**. Historically (`61da950`) a trivial version-macro mismatch in
the no-JVM ctest lane skipped the entire JVM matrix. The no-JVM ctest lane is the real
chokepoint.

**Crucially, `warnings-as-errors` does NOT gate anything** and is Linux-only — so a header that
fails clang `-Werror` still lets all 41 JVM cells run and go green (exactly today's B1/B2
situation).

### "Best-effort" caveat

`jvm-linux` and `jvm-macos` `exit 0` when `test_results.txt` is missing
(`::warning::… is best-effort`). `jvm-macos` never even greps for `[FAIL]` — it just `cat`s the
file. **Their green tells you almost nothing.** Only `jvm-windows` fails hard on a missing
`TOTAL:` line, a missing results file, injection failure, or a 120 s stall.

### Wall time — MEASURED

From `gh run list`, the last 6 CI runs on `master`:

| Run | Duration |
|---|---|
| `27db40e` | 18m35s |
| `bc9e6ae` | 17m45s |
| `682dd23` | 17m22s |
| `6e2c5cf` | 18m21s |
| `f00a7cb` (viewer) | 19m07s |

**≈ 18-19 minutes** end-to-end.

### What typically breaks

- **`warnings-as-errors (linux / clang)`** — clang-only diagnostics MinGW never emits
  (`-Wunused-const-variable`, `-Wunused-lambda-capture`, `-Wunused-private-field`). **Currently
  red (B1).**
- **`jvm · windows · *`** — all 21 red today (B2). Historically also: i2i-vs-JIT watcher
  flakes, GC-safepoint stalls on the no-SEH toolchains (#38, mitigated by `-Xmx4g -Xmn3g`),
  and `injector.exe exit=1` when the MinGW DLL isn't self-contained (fixed by
  `vmhook_static_runtime()`).
- **`build · windows / msvc`** — `/permissive-` overload ambiguity; a header compile error here
  produces no artifact, so all 7 msvc JVM cells report "Not Run" and it *looks* like 7 test
  failures.
- **`build · macos / clang` + `android`** — libc++ missing-include gaps; macOS static-init
  segfaults at 0.00 s.
- **CMake configure error on every platform** — a `vmhook_add_test()` line whose `.cpp` was
  deleted (`96cf463`).

### ⚠ `registry.yml` — a trap in your current working tree

`registry.yml` runs `python audit/features/validate.py` and then regenerates `audit/graph/`
and fails on any diff. Its path filter is:

```yaml
paths: ['audit/features/**', 'audit/graph/**', '.github/workflows/registry.yml']
```

Your working tree has **430 deleted files**, of which **249 are under `audit/features/` or
`audit/graph/`**. If you commit those deletions, `registry.yml` **triggers** and
`validate.py` won't exist → red. Either keep the deletions uncommitted, or delete
`.github/workflows/registry.yml` in the same commit.

---

## 5. Proving a GC-root / pin actually works

### 5.1 What `global_ref` is today — read this before designing a test

`vmhook.hpp:19687-19823`. **It stores a raw oop. It is not a GC root.**

```cpp
class global_ref final
{
public:
    explicit global_ref(vmhook::oop_t const raw_oop) noexcept : oop_{ raw_oop } {}
    auto oop()    const noexcept -> vmhook::oop_t { return this->oop_; }
    auto handle() const noexcept -> void*         { return this->oop_; }   // SAME pointer
    auto reset()        noexcept -> void          { this->oop_ = nullptr; }
private:
    vmhook::oop_t oop_{ nullptr };
};
```

There is **no `NewGlobalRef` call anywhere in the header** — the de-JNI effort removed it. The
class's own `@details` admits it:

> "PURE-VM LIMITATION: creating a real GC root (what `JNIEnv::NewGlobalRef` used to do)
> requires a call into the VM … vmhook therefore stores the raw OOP captured at construction
> and returns it as-is … a relocating GC that moves the object between the pin and this call
> leaves the address stale."

Consequences you must design around:

- `handle() == oop()`. No indirection, no handle slot, no tag bits.
- **Nothing keeps the object alive.** A pin is not a root; the object can be collected outright.
- The block comment 60 lines *above* the class (`19687-19698`) still claims it "pins the object
  via `NewGlobalRef`" and that `oop()` "always reads the object's CURRENT (post-relocation)
  address". **That comment is stale and contradicts the code.** Fix it if you touch this area.

### 5.2 What the existing test does

`tests/jvm/modules/global_ref.cpp` (1728 lines) + `example/vmhook/fixtures/GlobalRefProbe.java`.

Shape: registers a wrapper for `vmhook/fixtures/GlobalRefProbe`, installs **one**
`vmhook::scoped_hook` on `trigger()`, and does **all** oop work inside that detour (the suite
worker is a detached native thread with no `JNIEnv` and no `JavaThread`). ~90
`std::atomic` flags carry observations out to the module body, which then calls `ctx.check`.
Two phases via a Java-side `mode` static.

**Phase 1** — `make_unique`, `pin()`, read sentinel `0x5A5A`, exercise move/swap/null/many-pin,
then drop the wrapper. **Phase 2** — Java forces GC, `trigger()` fires again, re-read through
the same pin.

**Java-side GC forcing** (`GlobalRefProbe.java:117-131`):

```java
for (int round = 0; round < 4; round++)
{
    final byte[] churn = new byte[1 << 16];
    if (churn.length == 0) { throw new IllegalStateException("unreachable"); }
    System.gc();
    GlobalRefProbe.gcRounds++;
}
```

**Critical gates in the existing module:**

- Phase 2 is `#if !defined(_WIN32)` — **the GC-survival test does not run on Windows at all**
  (an `[INFO]` line is emitted instead). This was one of the three fixes that un-quarantined
  the no-SEH Windows cells (`bc76415`, `ac3de9c`).
- Inside the POSIX branch, JDK 8 is detected (`String` has no `coder` field) and skipped.
- `g_survive_attainable` gates the value checks; when the post-GC oop can't be safely resolved,
  it records `[INFO]` rather than failing.

**Address comparison is deliberately NOT asserted.** The module header states: *"The numeric
address from `.oop()` is ALLOWED to differ pre/post GC (that is relocation being tracked,
recorded as `[INFO]`, never asserted)."* It emits a diagnostic instead:

```cpp
oss << "[INFO] global_ref diag: handle=0x" << std::hex << hbits
    << " oop_pre_gc=0x"  << pre  << " oop_post_gc=0x" << post
    << " relocated="     << std::dec << (pre != 0 && post != 0 && pre != post);
```

What *is* hard-asserted post-GC is `global_ref_handle_nonnull_after_gc` and
`global_ref_handle_stable_across_gc` — but since `handle() == oop()`, "handle stable" now
silently asserts **that no relocation happened**, the exact opposite of the documented intent.

**Assertions that are stale w.r.t. the current implementation** (verify before trusting):

| Assertion | Why it's now wrong |
|---|---|
| `global_ref_*_distinct_handles`, `..._many_unique_handle_count == 8` | With `handle()==oop()`, two pins to the same object have **identical** handles |
| `global_ref_mask_matches_oop` (`*(void**)(handle & ~7) == .oop()`) | Now dereferences the oop itself and compares to the oop — passes only by accident |
| `global_ref_masked_slot_is_8_aligned` | Trivially true; oops are 8-aligned |
| `global_ref_handle_stable_across_gc` | Asserts non-relocation, not survival |

Also: `tests/test_global_ref.cpp` (the no-JVM twin) is one of the 12 **orphans** and describes
the old JNI implementation; it would fail today if re-registered.

### 5.3 What a rigorous "the pin survives a relocating GC" test looks like

To prove a *real* GC root you must show three things, in this order:

**(1) The object MOVED.** Relocation must be *demonstrated*, not assumed, or a passing test
proves nothing. Capture `oop()` pre-GC into `g_oop_pre_gc`, force GC, capture post-GC, and
**hard-assert `pre != post`** — but only after gating on "a relocating collector is actually in
use". Without that gate the test is a coin flip: `System.gc()` on G1 need not evacuate a given
region, and Epsilon/serial-nonmoving would never move it. Practical gate: retry the
churn+gc cycle up to N times and only assert relocation once observed, else `[INFO]`-skip —
the pattern `field_introspection.cpp` SECTION H already uses (bounded 16-attempt retry).

**(2) The handle still RESOLVES, and to the NEW address.** This is the part the current API
cannot satisfy: it requires an indirection (`*(oop**)handle`), which `global_ref` no longer has.
A genuine root would assert `pin.oop() == post_gc_address && post != pre`.

**(3) The object is still THERE and INTACT.** Address validity is not liveness. Re-read the
`sentinel` field through the resolved oop and assert `== 0x5A5A`, and re-read the `tag` String
and assert `== "pinned-tag"`. A stale-but-mapped address will usually give garbage, not a
fault — so the *value* check is the real proof, not a null check.

**Use the strong safety helpers, not the weak ones.** `global_ref.cpp`'s `resolve_oop_guarded()`
only does `is_valid_pointer()` (range + alignment). The stronger primitives are in
`tests/jvm/modules/field_introspection.cpp:234-292`:

```cpp
constexpr std::size_t k_oop_header_probe_bytes{ 16 };

auto oop_header_safely_readable(void* const decoded) -> bool
{
    if (!decoded || !vmhook::hotspot::is_valid_pointer(decoded)) { return false; }
    std::uint8_t scratch[k_oop_header_probe_bytes] = { 0 };
    // ReadProcessMemory / process_vm_readv: returns false (no fault) if any byte
    // of the header is on an unmapped/relocated page.
    return vmhook::os::safe_read(scratch, decoded, sizeof(scratch));
}
```

**Heap-pressure helpers that already exist** (there is **no shared native `force_gc()`** — all
GC forcing is Java-side via a fixture `mode`, driven through `ctx.run_probe`):

| Java fixture | Loop |
|---|---|
| `example/vmhook/fixtures/Warmup.java:85-96` (`runGcSettle`) | 32 × `new byte[64*1024]`, then `System.gc(); System.gc();` |
| `fixtures/FieldIntrospection.java:321-333` | 64 × `new byte[64*1024]`, then `System.gc(); System.gc();` |
| `fixtures/DontInlineProbe.java:154-174` | 64 allocs then double-gc then `hot(delta)` |
| `fixtures/GlobalRefProbe.java:117-131` | 4 × (`new byte[1<<16]` + `System.gc()`) |

The native driver idiom (`aaa_warmup.cpp:74-91`):

```cpp
auto drive(vmhook_test::context& ctx, std::int32_t mode) -> bool
{
    if (!ctx.run_probe) { return false; }
    return ctx.run_probe(
        [mode](bool value)
        {
            if (value) { fixture::set_done(false); fixture::set_mode(mode); }
            fixture::set_go(value);
        },
        []() { return fixture::get_done(); });
}
```

**Heap flags.** Both CI and `.localci` launch the target JVM with **`-Xmx4g -Xmn3g`**. This is
the #38 mitigation: a 3 GB young gen absorbs transient allocation so *natural* GCs don't fire
mid raw-VM-walk. They deliberately rejected `-XX:+UseEpsilonGC` because it no-ops
`System.gc()` and would destroy GC coverage. **Consequence for your test: a 3 GB young gen
makes it correspondingly harder to provoke a real evacuation** — you will need meaningfully
more churn than the existing 4 × 64 KB, or an explicit `-XX:+UseG1GC`/`-Xmn` override in a
dedicated cell.

**Matrix gating you will have to decide.** Today the entire GC-survival phase is
`#if !defined(_WIN32)`, so it runs on **zero** cells of the `jvm-windows` hard gate and only on
the *best-effort* Linux/macOS jobs. If you want real cross-matrix proof you must either
(a) re-enable Windows and accept the no-SEH stall risk that got it gated in the first place, or
(b) accept that GC-root coverage is Linux/macOS-only and therefore **not hard-gated by CI at
all**. Option (b) is the status quo and is why this has stayed broken silently.

**Model to copy:** `tests/jvm/modules/field_introspection.cpp` SECTION H (lines 1992-2115) —
captures a compressed oop + decoded address, forces GC, re-resolves fresh, asserts *"correct OR
transient miss, never a different live object"*, with a bounded retry loop, an `[INFO]` record
of the observation, and a recompute cross-check:

```cpp
ctx.check("gc_doc_after_addr_matches_recompute",
          after->raw_address() == recompute_static_addr(klass, "sString"));
```

---

## 6. Landmines — specific and actionable

Ordered by how likely they are to bite a large header addition. Every item is backed by a real
commit in this repo.

### 6.1 Diagnostics MinGW will NOT show you

| Landmine | Caught by | Missed by | Fix idiom |
|---|---|---|---|
| **`-Wunused-private-field`** on a dead member | CI clang-18 (Linux) **only** | MinGW, MSVC, **and local clang 19** | Delete it, or actually use it. **This is live bug B1.** |
| **`-Wunused-const-variable`** on namespace-scope `constexpr` | linux/clang `-Werror` | MinGW g++ | Anchor it in a `static_assert` (preferred, `445e140`) or `[[maybe_unused]]` |
| **`-Wunused-lambda-capture`** on a captured `constexpr` | clang legs only | MinGW g++ (`04f017a`) | A `constexpr` used as a constant expression **must not** appear in the capture list. `[&]` is fine; explicit `[cap]` is the error |
| **`-Wunused-but-set-variable`** on a fold-only lambda | MinGW g++ `-Werror` | — | `[[maybe_unused]] auto pack = …` — a zero-arg `(pack(args), ...)` fold expands to nothing (`fa947f0`, live at `vmhook.hpp:15163`) |

### 6.2 MSVC-only breakers

- **`value_t` conversion ambiguity — the #1 MSVC-only breaker, hit 5 separate times.**
  - Never `T x{ proxy->call() };` (brace-init) → C2440 on MSVC, fine on g++ (`4024802`, `9c52877`).
  - Never `static_cast<ClassType>(value_t)` → ambiguous under `/permissive-` (`7d79a36`, `dc73048`).
  - **The one form every compiler agrees on** (`962482c`): copy-init into a **named local**, then
    return it. `static_cast` fails on MSVC for move-only `T`; `.template operator T()` fails on Clang.
  - New conversion operators must be constrained with
    `requires vmhook::detail::value_t_convertible_target_v<T>` like the existing pair (`db7f357`).
  - ⚠ This **conflicts with CONTRIBUTING.md's brace-init style rule** — use `=` and add a comment.
- **Missing `<include>` that libstdc++ provides transitively.** `80b5010`: `std::array` worked on
  MinGW (via `<vector>`) and failed on **every Windows compiler**. Present in the header's block
  (`vmhook.hpp:84-108`): `<array> <cstdint> <cstdlib> <fstream> <iostream> <string>
  <string_view> <vector> <unordered_map> <unordered_set> <typeindex> <memory> <mutex>
  <condition_variable> <thread> <chrono> <algorithm> <atomic> <type_traits> <tuple> <cstring>
  <optional> <variant> <functional> <limits>`. **Absent — add explicitly if you use them:**
  `<bit>`, `<cstddef>`, `<span>`, `<ranges>`, `<numeric>`, `<charconv>`, `<map>`, `<set>`,
  `<deque>`, `<bitset>`, `<cmath>`, `<compare>`, `<concepts>`.
- **Blast radius:** a header compile error on MSVC kills the build job → no artifact → all 7
  `jvm·windows·msvc` cells report "Not Run". It *looks* like 7 test failures.
- **`/bigobj`** is set only in `viewer/CMakeLists.txt:68`, **not** in library CI. A large new
  templated section could hit MSVC **C1128** on `windows/msvc` first. No precedent commit — a
  genuinely new risk for a big addition.

### 6.3 libc++ / macOS / Android only

- **Missing `<bit>` etc.** `3300494`: five modules used `std::bit_cast` without `#include <bit>`;
  libstdc++ pulled it in, **libc++ (macOS, Android NDK) did not**. Failed on macos+android but
  **not** linux/clang.
- **`long` ≠ `std::int64_t`.** `01f4d0a`: on **macOS/iOS LP64**, `long` is 64-bit but
  `std::int64_t` is `long long` — a *distinct* type. `sizeof(long)==8` is true, the trait is
  false. **Never write `sizeof(T)==8`, never write bare `long`.** Key on
  `std::is_same_v<T, std::int64_t>` / the existing `is_java_double_slot_v`
  (`vmhook.hpp:10014-10016`, duplicated at `6523-6524` — **keep both in sync**).
- **`noexcept` probing.** `a7cdbbe` / `53be154`: Android/macOS libc++ does not mark
  `string_view(const char*)` noexcept. Prefer `std::string_view{lit, len}`, and hoist arguments
  to a named lvalue outside `noexcept(...)`.
- **`optional<T>` observers in `static_assert`** — clang+libc++ refuses them as constant
  expressions when `T` has a user-provided dtor (`dc73048`). Use a runtime check.
- **Static-init segfaults.** `d3edc80` / `6fccf13`: `inline auto g_x = []{…}();` at namespace
  scope deterministically segfaulted at static-init on macos/clang, nowhere else. **Do not add
  lambda-as-inline-variable initializers.** Initialize lazily under the relevant mutex.

### 6.4 Windows / SEH / EH

- **The SEH gate spelling is always `#if defined(_MSC_VER) && !defined(__clang__)`** — never bare
  `_MSC_VER` (clang-cl and `clang++ --target=…-msvc` both define it), never `_WIN32`. Only real
  `cl.exe` traps a hardware AV; clang-cl, MinGW, and POSIX do not (`vmhook.hpp:1110-1121`).
- **Any function with `__try`/`__except` must have POD-only locals** → else MSVC **C2712**.
- **`__except` filter must blacklist `EXCEPTION_STACK_OVERFLOW`** → `EXCEPTION_CONTINUE_SEARCH`;
  a catch-all swallows the guard page (`dc73048`).
- **Any new raw dereference of JVM memory must go through `os::safe_read`/`safe_read_fast`.**
  `e6c2109`: an unguarded read crashed the *no-JVM* test process on linux+macos while MinGW
  contained it via `ReadProcessMemory`.
- **Don't reintroduce `/EHsc`** — `D9025` becomes a hard error under `/WX`.
- **`NOMINMAX`** is self-guarded at `vmhook.hpp:264-274`. Put new `windows.h`-dependent code
  **after** line 273; never add a second early `#include <windows.h>`. Belt-and-braces: write
  `(std::numeric_limits<T>::max)()` in parens — a consumer TU that includes `windows.h` *before*
  `vmhook.hpp` defeats the internal guard, and the legacy `vcxproj` supplies no `NOMINMAX`.

### 6.5 Header-structure landmines

- **Declaration ordering.** `92f6710`: helpers referencing `vmhook::hotspot` types must live
  **after** the `hotspot` namespace block. The convention is to *reopen* `namespace vmhook::detail`
  later in the file, not to grow the early block. Misplacement → `C2039`.
- **Never name `std::unique_ptr<Incomplete>`** — even in a return type or a lambda body — where
  the type is only forward-declared. libstdc++-14 **and** libc++ eagerly instantiate the dtor's
  `static_assert(sizeof(T)>0)`; MSVC's lazy instantiation hides it (`593bf6e`). Return a raw
  pointer, wrap at the consumption site.
- **New namespace-scope mutable globals must be `inline`** — `tests/test_odr_unit_a.cpp`/`_b.cpp`
  link two TUs specifically to force the linker to reject duplicate definitions.
- **Deducing-this** (`vmhook.hpp:255-262`) has been re-gated 4×. Current predicate excludes GCC,
  `__ANDROID__`, and `__clang_major__ >= 20`. If you add a member callable from a **static** C++
  context, supply **both** arms and gate call sites on `VMHOOK_HAS_DEDUCING_THIS` — never on
  `_MSC_VER || __clang__`. Compiler floors: **GCC 14+ / Clang 18+**, clang **20+** on Windows.
- **Function-traits specializations:** since C++17 `noexcept` is part of the function type
  (`14ba6e7` — a `noexcept` lambda was a hard compile error). The full cv × ref × noexcept matrix
  (20 specializations) is pinned by `tests/test_traits_function_traits.cpp`.
- **Argument cap:** the call-stub fast path packs into `params[8]`. Keep the `<= 8` slot bound
  and remember `long`/`double` take **two** slots (`6abc74a`).

### 6.6 Build-wiring landmines

1. **New `tests/test_*.cpp` needs a manual `vmhook_add_test()` line.** No glob. Forgetting it =
   a silently dead test (there are already **12**, §2.1).
2. **Deleting a `tests/test_*.cpp` without removing its line** = `CMake Error: Cannot find source
   file` on **every** platform (`96cf463`).
3. **New `tests/jvm/modules/*.cpp` needs nothing** — globbed with `CONFIGURE_DEPENDS`.
4. **Version bump touches 4 places:** `CMakeLists.txt:3-5`, `vmhook.hpp:67-69`
   (`VMHOOK_VERSION_MAJOR/MINOR/PATCH`), `vcpkg.json`, `CHANGELOG.md`. A mismatch fails
   `test_version_macros` → fails the build job → **skips all 41 JVM cells** (`61da950`).
5. **Stale objects can hide a break.** A plain incremental build can pass on stale `.obj`s.
   For a final gate, configure a **fresh** `build/<name>` dir.
6. **New Windows-loaded targets must call `vmhook_static_runtime()`** or `LoadLibraryW` fails in
   the JVM process (`ab56f98`).
7. **No non-ASCII in C++ or Java sources** (CONTRIBUTING.md) — JDK 8 on Windows uses Cp1252 and
   `javac` will fail. Use `---`/`===`.

---

## 7. Recommended pre-flight sequence

```bash
# 0. (30 s) syntax gate — all three toolchains, header only
g++ -std=c++23 -Wall -Wextra -Wpedantic -Werror -fsyntax-only -Ivmhook/ext tests/test_header_compile.cpp
cmd /c msvc_check.cmd        # §1.3(b)
cmd /c clangpp_check.cmd     # §1.3(c)

# 1. (2m15s) MinGW full -Werror, FRESH dir + no-JVM ctest
cmake -S . -B build/pre-mingw -G Ninja -DCMAKE_BUILD_TYPE=Release -DVMHOOK_WARNINGS_AS_ERRORS=ON
cmake --build build/pre-mingw --parallel
ctest --test-dir build/pre-mingw --output-on-failure

# 2. (2m25s) MSVC, CI-equivalent (no /WX — B3 blocks /WX today)
#    -> catches value_t ambiguity + MSVC-STL missing includes
```
```powershell
# 3. (6-8 min) all three compilers built + no-JVM ctest, via the harness
.\.localci\run-local-ci.ps1 -UnitOnly

# 4. (20-40 s) one JVM smoke cell
.\.localci\run-local-ci.ps1 -NoBuild -Compilers mingw -Java 21

# 5. (15-25 min) full local Windows matrix, before you push
.\.localci\run-local-ci.ps1
```

**Then** read your diff hunting specifically for: `sizeof(T)==8`, bare `long`, brace-init from
`value_t`, `static_cast<ClassType>(value_t)`, an unconsumed namespace-scope `constexpr`, `[cap]`
where `cap` is `constexpr`, `inline auto g = []{…}();`, a new STL type without its `#include`, a
bare `_MSC_VER` gate, an unused private member, and a raw dereference of JVM memory.

**Then** push once (`cancel-in-progress` means every push kills the previous run) and check
GitHub. The legs that will tell you something no local toolchain could:
**`warnings-as-errors (linux / clang)`** → **`build · macos / clang`** → **`android`** →
**`build · windows / msvc`**, in that order.
