# Local CI (`.localci/`)

A self-contained local mirror of the GitHub Actions **Windows** JVM matrix
(`.github/workflows/ci.yml` → `build-and-unit-test` + `jvm-windows`), so the
Windows cells can be validated in minutes without the GitHub round-trip. The
GitHub CI is unchanged and remains authoritative — this is a fast pre-flight.

```powershell
# all detected compilers × all 7 JDKs
.\.localci\run-local-ci.ps1

# reproduce a specific cell fast (e.g. the borderline #38 / java8 cells)
.\.localci\run-local-ci.ps1 -Compilers clang -Java 25
.\.localci\run-local-ci.ps1 -Compilers msvc  -Java 8
.\.localci\run-local-ci.ps1 -Compilers mingw -Java 8,17,26

# build + no-JVM ctest only (skip the JVM cells)
.\.localci\run-local-ci.ps1 -UnitOnly

# reuse the existing per-compiler build, just re-run cells
.\.localci\run-local-ci.ps1 -NoBuild -Compilers mingw -Java 17

# control concurrency (default: auto from RAM/cores; 1 = sequential)
.\.localci\run-local-ci.ps1 -Parallel 1          # sequential
.\.localci\run-local-ci.ps1 -Parallel 8          # 8 cells at once
```

## Parallelism

Cells run **concurrently** by default. Each cell is a real `java -Xmx4g`, so unlike
GitHub (separate runners) we're bounded by one machine — `-Parallel 0` (the default)
auto-picks `min(cores-1, floor((RAM_GB-4)/5))` capped at 6 (e.g. 5 on a 16-core/32 GB
box). Builds run serially first (CPU-heavy), then cells fan out as isolated single-cell
subprocesses (`-NoBuild`) in their own work dirs. Note: more concurrency = more JIT
pressure, which makes timing-sensitive watchers (on_class_loaded / on_exception) flake
*more* — a feature for surfacing the i2i-vs-JIT fragility, not a bug.

## Watching a run live

The harness flushes as it goes, so you can tail it:

```powershell
# within a cell, per-assertion (best granularity) — freezes on the last line if it stalls/crashes
Get-Content .\.localci\work\msvc-java21\test_results.txt -Wait -Tail 30
# a parallel cell's transcript
Get-Content .\.localci\logs\cell-msvc-java21.log -Wait -Tail 30
# build output (cmake/ninja), live
Get-Content .\.localci\logs\build-msvc.log.build.out -Wait -Tail 30
```
(Run it yourself in a terminal for live colored per-cell output directly.)

## What it does (per cell, exactly like CI)

1. Builds the example DLL + injector once per compiler (CMake **Release** — the
   same artifact the build job uploads) and runs the no-JVM `ctest` lane.
2. Compiles the Java fixtures with **that JDK's** `javac`
   (`javac -encoding UTF-8 -d out example\vmhook\*.java example\vmhook\fixtures\*.java`)
   — this catches per-version fixture issues (e.g. a `*/` inside a Javadoc comment)
   that the C++ `-Werror` build never sees.
3. Launches `java -Xmx4g -Xmn3g -cp out vmhook.Main` (the #38 heap lever),
   injects `injector.exe <pid>`, waits ≤120 s, and verifies `test_results.txt`
   has no `[FAIL]` and a `TOTAL:` line (a missing TOTAL = a mid-suite crash/stall).

## Closed environment

Everything lives under `.localci/` and nothing touches your system:

| dir | contents | tracked? |
|---|---|---|
| `jdks/<major>/` | extracted Temurin JDKs (one per version) | git-ignored |
| `cache/` | downloaded JDK archives (reused; **never re-downloaded**) | git-ignored |
| `builds/<compiler>/` | per-compiler CMake build dirs | git-ignored |
| `work/<cell>/` | per-cell scratch (`out\`, `test_results.txt`, logs) | git-ignored |
| `logs/` | per-build / per-cell logs + the run transcript | git-ignored |
| `run-local-ci.ps1`, `README.md` | the harness itself | **tracked** |

JDKs are fetched from the Adoptium API on first use and cached; a re-run with a
cached JDK does no network I/O.

## Compiler coverage

The script auto-detects compilers. **MinGW** (`g++` on PATH) is enough for the
mingw cells. The **MSVC-ABI** cells (`msvc`, `clang-cl`) — where the #38
GC-safepoint stalls and the java8 `on_exception` issues actually reproduce, and
which MinGW *cannot* — require **VS Build Tools 2022** with the
"Desktop development with C++" workload installed (the script finds it via
`vswhere` + `vcvars64`). Cells for an unavailable compiler are reported as
SKIPPED, not failed.
