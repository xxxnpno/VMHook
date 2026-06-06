# vmhook Perfection Program

> **Mission (user directive, 2026-06-06):** Make the vmhook library *perfect*.
> A sustained, multi-agent engineering program. This file is the **durable spine** —
> it survives context compaction and session restarts. Update it every wave.

## The mandate (verbatim intent)

1. **≥100 specialized agents**, one per vmhook feature (decomposed finely). Each agent
   *owns* its feature: masters it, finds every flaw, and owns its exhaustive tests.
2. Each agent **first writes exhaustive GitHub Actions unit tests covering every
   possible input** of its feature (more tests = better), **then** redesigns/improves
   the feature.
3. Every feature must work on **every Java version: 8, 11, 17, 21, 24, 25 (→ latest)**.
4. **Tests execute on GitHub Actions ONLY.** Never run the test suite (JVM modules or
   ctest) on the user's machine. Local builds are **compile-only** validation
   (`-DVMHOOK_WARNINGS_AS_ERRORS=ON`, MinGW) to catch build breaks before push.
   Watch CI, see what fails, fix.
5. **Refactor the whole repo** — delete dead code, improve structure. **Completely
   refactor the JVM test harness** (retire the legacy inline `test_*()` driver +
   legacy fixtures; modular-only).
6. **Audit and improve every single file** in the repo.

## Constraints & invariants (do not violate)

- **Push after every meaningful improvement** (user standing order). Commit valuable
  work promptly — concurrent codex automation `git stash -u`s untracked files.
- **Local = compile only.** `cmake --build` to validate `-Werror`; **never `ctest`**,
  never launch the injected JVM locally. GHA is the test oracle.
- **Parallel-safe vs serial:**
  - *Parallel-safe (fan out with agents/workflows):* new test modules
    (`tests/jvm/modules/*.cpp`), Java fixtures (`example/vmhook/fixtures/*.java`),
    pure-logic tests (`tests/test_*.cpp`), agent-def files (`.claude/agents/*.md`),
    read-only file audits. Distinct files → no merge conflicts.
  - *Serial (main loop or one agent at a time, reviewed):* `vmhook/ext/vmhook/vmhook.hpp`
    (single 17k-line header — ALL library fixes), `CMakeLists.txt`, `tests/CMakeLists.txt`,
    `vmhook/src/example.cpp` driver, `.github/workflows/*`. One shared file = one writer.
- **Newly-created `.claude/agents/*.md` are NOT spawnable mid-session** (the agent-type
  registry is read at startup). They become available next session. Until then, drive
  their work via `general-purpose` agents / Workflow with the same specialist prompt.
  The 37 pre-existing specialists ARE spawnable now (see roster).
- **JDK-variance discipline:** gate JDK-variant JIT/deopt/flags/heap-scan assertions
  *best-effort* (emit `[INFO]` when a runtime precondition is absent), keep behavioral
  invariants *hard*. JDK8 detector idiom:
  `find_class("java/lang/String")->find_field("coder").has_value() == false`.

## Architecture facts (verified 2026-06-06 @ e23d1fc)

- **Header:** `vmhook/ext/vmhook/vmhook.hpp` (16,952 lines). Single-header library.
- **JVM test driver:** `vmhook/src/example.cpp` (137 KB). `run_test_suite()` @3202:
  registers ~14 legacy classes (3208-3220), runs ~40 legacy inline `test_*()` (3226-3282),
  THEN runs the modular harness `vmhook_test::run_all(ctx)` (3290-3309, gated on
  `VMHOOK_MODULAR_HARNESS`). **Rework D** = delete the legacy inline section + legacy
  fixtures; keep modular-only.
- **Modular harness:** `tests/jvm/harness.{hpp,cpp}`. Modules self-register via
  `VMHOOK_JVM_MODULE(name)` → get `context& ctx` with `ctx.check(name,ok)`,
  `ctx.record(line)`, `ctx.run_probe(set_go,get_done)`. Globbed into the example DLL by
  `CMakeLists.txt` (168-170, `CONFIGURE_DEPENDS tests/jvm/modules/*.cpp`).
- **Java fixtures:** `example/vmhook/fixtures/*.java` (one per feature) + legacy
  `example/vmhook/*.java` (A, B, Animal, Color, Dog, Example, CallerProbe, ...).
- **Pure-logic tests:** `tests/test_*.cpp` wired in `tests/CMakeLists.txt` (run on every
  OS/compiler, no JVM). These compile locally but execute on GHA.
- **CI:** `.github/workflows/` — build matrix (linux/win/mac × gcc/clang/mingw/msvc) +
  cross-compile (android/ios) + JVM matrix (Java 8/11/17/21/24/25 × OS × compiler) +
  warnings-as-errors. Baseline e23d1fc: ALL 45 jobs green.

## Orchestration loop (per wave)

1. **Fan out tests (parallel):** N agents, each authors the exhaustive test module +
   fixture for one feature — *every input* (all primitive widths, null, empty, boundary,
   overflow, unicode, signed/unsigned edges, JDK-variant paths). Distinct files.
2. **Author/refresh agent-def** for each feature (`.claude/agents/<slug>-specialist.md`).
3. **Integrate + compile (main loop):** local `-Werror` build (compile only). Fix build
   breaks. **Commit + push.**
4. **Watch CI** (`gh run watch` / `.git/watch_ci.sh`). Tests run on GHA across all JDKs.
5. **Collect failures → serial fixes:** apply header fixes one at a time (reviewed),
   gate JDK-variance best-effort, push, re-watch until green.
6. **Record** results + new flaws in this doc; update memory.

## Feature → agent roster (target ≥100)

Legend: **A** = agent-def exists (`✓` spawnable now / `+` file exists, not yet spawnable),
**M** = test module exists, **F** = Java fixture exists, **St** = status.
Status: `green` (in CI, passing) · `thin` (exists, needs exhaustive expansion) ·
`new` (to create) · `fix` (known library bug queued).

### G1 — Hooks: install / dispatch / lifecycle
| # | slug | A | M | F | St | notes |
|---|------|---|---|---|----|-------|
| 1 | hook_basic | ✓ | ✓ | ✓ | thin | exactly-once dispatch, arg decode, self |
| 2 | hook_signature | ✓ | ✓ | ✓ | thin | hook_by_signature overload select |
| 3 | hook_install_after_jit | ✓ | ✓ | ✓ | thin | deopt already-JIT'd target |
| 4 | hook_verify_repair | ✓ | ✓ | ✓ | thin | verify_hooks drift+auto-repair (FIX C area) |
| 5 | hook_unhook_double_free | + | ✓ | ✓ | thin | stop() idempotency |
| 6 | scoped_hook_raii | ✓ | ✓ | ✓ | thin | RAII teardown |
| 7 | dont_inline_dont_compile | ✓ | ✓ | ✓ | thin | _dont_inline + NO_COMPILE |
| 8 | deoptimize_methods | ✓ | ✓ | ✓ | thin | deoptimize_methods_if / _all |
| 9 | hook_chaining | new | new | new | new | multiple hooks share i2i stub |
| 10 | hook_reinstall_after_shutdown | new | new | new | new | shutdown+reinit dispatch revives |
| 11 | hook_common_detour_dispatch | new | new | new | new | common_detour match/fire/return |
| 12 | hook_auto_repair_watchdog | new | new | new | new | watchdog re-detour after drift |
| 13 | hook_find_location_i2i | new | new | n/a | new | find_hook_location pattern match |
| 14 | midi2i_trampoline_alloc | new | new | n/a | new | trampoline allocator/lifetime |
| 15 | hook_arg_decode_slots | new | new | new | new | java_slot_offsets/extract_frame_arg J/D widening |
| 16 | seh_invoke_detour | new | new | n/a | new | signal-safe detour invoke |

### G2 — return_value
| # | slug | A | M | F | St | notes |
|---|------|---|---|---|----|-------|
| 17 | return_set_primitives | ✓ | ✓ | ✓ | thin | force-return each primitive width |
| 18 | return_set_arg | ✓ | ✓ | ✓ | fix | arg mutation, wide oop slot (FIX A) |
| 19 | return_set_wrapper_null | ✓ | ✓ | ✓ | thin | object-wrapper + null injection |
| 20 | return_value_cancel | + | ✓ | ✓ | thin | cancel() suppress original |
| 21 | return_frame_raw_access | ✓ | ✓ | ✓ | thin | return_value::frame() escape hatch |
| 22 | return_caller | ✓ | ✓ | ✓ | thin | caller() saved-rbp walk |
| 23 | return_stack_trace_depth | ✓ | ✓ | ✓ | thin | stack_trace() multi-frame |

### G3 — Fields
| # | slug | A | M | F | St | notes |
|---|------|---|---|---|----|-------|
| 24 | field_primitives_get | ✓ | ✓ | ✓ | thin | get each primitive |
| 25 | field_primitives_set | new | new | new | new | set path, anti-clobber |
| 26 | field_object_ref | ✓ | ✓ | ✓ | thin | oop field get/set (FLAW A area) |
| 27 | field_string | ✓ | ✓ | ✓ | thin | String field decode |
| 28 | field_arrays_primitive | ✓ | ✓ | ✓ | thin | primitive arrays |
| 29 | field_arrays_object | + | ✓ | ✓ | thin | object arrays |
| 30 | field_static | ✓ | ✓ | ✓ | thin | static field get/set |
| 31 | field_inherited | ✓ | ✓ | ✓ | thin | superclass field resolve |
| 32 | field_introspection | ✓ | ✓ | ✓ | thin | field enumeration/metadata |
| 33 | field_set_size_guard | ✓ | ✓ | ✓ | thin | set size/type guard |
| 34 | field_proxy_value_t | new | n/a (logic) | n/a | thin | value_t conversions (no-JVM) |
| 35 | field_null_safety | new | new | new | new | null receiver / absent field |
| 36 | field_bool_byte_char_short | new | new | new | new | sub-int widening/sign |

### G4 — Methods
| # | slug | A | M | F | St | notes |
|---|------|---|---|---|----|-------|
| 37 | method_call_primitives | ✓ | ✓ | ✓ | thin | call returning each primitive |
| 38 | method_call_string | ✓ | ✓ | ✓ | thin | returns java.lang.String→std::string |
| 39 | method_call_object | ✓ | ✓ | ✓ | thin | returns object→unique_ptr<wrapper> |
| 40 | method_call_return_void | + | ✓ | ✓ | thin | void return |
| 41 | method_call_jni_fallback | ✓ | ✓ | ✓ | thin | call_jni invocation fallback |
| 42 | method_static | + | ✓ | ✓ | thin | static method call |
| 43 | method_static_portability | ✓ | ✓ | ✓ | thin | portable static path across compilers |
| 44 | method_overload | ✓ | ✓ | ✓ | thin | arg-type-driven overload resolution |
| 45 | method_overload_java_dispatch | + | ✓ | ✓ | thin | virtual dispatch |
| 46 | method_explicit_signature | ✓ | ✓ | ✓ | thin | get_method(name,sig) |
| 47 | method_enumeration | + | ✓ | ✓ | thin | enumerate klass methods |
| 48 | method_is_reference | + | ✓ | ✓ | thin | is_reference predicate |
| 49 | method_return_types | + | ✓ | ✓ | thin | all return types |
| 50 | method_throwing_call_site | + | ✓ | ✓ | thin | exception across call |
| 51 | method_proxy_value_t | new | n/a (logic) | n/a | thin | value_t (no-JVM) |
| 52 | method_call_wide_args | new | new | new | new | long/double 2-slot args |
| 53 | jni_arg_packing | new | n/a (logic) | n/a | thin | append/make/write jni args |
| 54 | find_methods_by_signature | new | new | new | new | signature-filtered method find |

### G5 — Collections
| # | slug | A | M | F | St | notes |
|---|------|---|---|---|----|-------|
| 55 | collection_list | new | ✓ | ✓ | thin | List wrapper |
| 56 | collection_linked_list | new | ✓ | ✓ | thin | LinkedList wrapper |
| 57 | collection_set | new | ✓ | ✓ | thin | Set wrapper |
| 58 | collection_map | new | ✓ | ✓ | thin | Map wrapper |
| 59 | collection_hash_tree_map | new | ✓ | ✓ | thin | HashMap/TreeMap |
| 60 | collection_type_tags | new | n/a (logic) | n/a | thin | type-tag mapping (no-JVM) |
| 61 | collection_iteration_safety | new | new | new | new | iterate_entries safety |

### G6 — Object model / wrapper / construction
| # | slug | A | M | F | St | notes |
|---|------|---|---|---|----|-------|
| 62 | wrapper_pattern | new | new | new | new | object_base/object<T> |
| 63 | register_class | new | new | new | new | register_class<T> machinery |
| 64 | make_unique | ✓ | ✓ | ✓ | thin | jni_make_unique ctor |
| 65 | make_java_string | new | ✓ | ✓ | thin | make_java_string |
| 66 | make_java_array | new | new | new | fix | JDK8 array-klass fallback (FIX D) |
| 67 | enum_singleton | new | ✓ | ✓ | thin | enum constant wrappers |
| 68 | interface_polymorphism | new | ✓ | ✓ | thin | interface dispatch |
| 69 | nested_classes | new | ✓ | ✓ | thin | inner/static-nested |
| 70 | poly_inherited_oop | new | ✓ | ✓ | thin | polymorphic inherited oop |

### G7 — Class lookup / loading / strings
| # | slug | A | M | F | St | notes |
|---|------|---|---|---|----|-------|
| 71 | find_class_fallback | ✓ | ✓ | ✓ | thin | find_class + JNI fallback chain |
| 72 | find_class_context_loader | new | new | new | new | classloader reanchor |
| 73 | on_class_loaded | ✓ | ✓ | ✓ | thin | defineClass watcher |
| 74 | for_each_loaded_class | ✓ | ✓ | ✓ | thin | loaded-class snapshot |
| 75 | read_java_string | new | ✓ | ✓ | thin | direct String reader |

### G8 — Runtime enumeration / introspection / lifecycle
| # | slug | A | M | F | St | notes |
|---|------|---|---|---|----|-------|
| 76 | for_each_instance | ✓ | ✓ | ✓ | thin | conservative live-instance scan |
| 77 | for_each_thread | ✓ | ✓ | ✓ | thin | live JavaThread enum |
| 78 | global_ref | ✓ | ✓ | ✓ | thin | jni::global_ref GC-survival pin |
| 79 | jni_local_ref_hygiene | ✓ | ✓ | ✓ | thin | DeleteLocalRef discipline |
| 80 | watch_static_field | ✓ | ✓ | ✓ | thin | HW data-breakpoint watchpoint |
| 81 | on_exception | new | ✓ | ✓ | thin | Throwable construction hook |
| 82 | shutdown_hooks_teardown | ✓ | ✓ | ✓ | thin | bulk teardown + reversibility |

### G9 — HotSpot internals (cross-JDK portability linchpins)
| # | slug | A | M | F | St | notes |
|---|------|---|---|---|----|-------|
| 83 | vmstructs_offset_resolution | new | new | n/a | new | iterate_struct_entries (the linchpin) |
| 84 | compressed_oops_decode | new | new | new | new | base/shift across heaps |
| 85 | compressed_klass_decode | new | new | new | new | narrow klass base/shift |
| 86 | klass_introspection | new | new | new | new | collect_klass_methods/klass walk |
| 87 | const_method_bounds | new | new | new | fix | ConstantPool bounds (FIX B) |
| 88 | method_flags_width | new | new | new | fix | get_flags u16 vs u4 (FIX E) |
| 89 | interpreter_frame_walk | new | new | new | new | frame locals/saved-rbp |
| 90 | method_entry_points_i2i_i2c | new | new | new | fix | i2i restore after re-JIT (FIX C) |
| 91 | adapter_recovery_c2i | new | new | new | new | Method::_adapter recovery 8 vs 9+ |
| 92 | instanceklass_methods_walk | new | new | new | new | InstanceKlass::_methods array |
| 93 | constantpool_access | new | new | new | new | CP element reads |

### G10 — OS abstraction layer
| # | slug | A | M | F | St | notes |
|---|------|---|---|---|----|-------|
| 94 | os_protect | new | n/a (logic) | n/a | thin | mprotect/VirtualProtect |
| 95 | os_allocate_release | new | n/a (logic) | n/a | new | alloc/free round-trip |
| 96 | os_query_region | new | n/a (logic) | n/a | new | region attributes |
| 97 | os_safe_read | new | n/a (logic) | n/a | new | read across PROT_NONE |
| 98 | os_page_size_granularity | new | n/a (logic) | n/a | new | page/granularity relation |
| 99 | os_signal_handler | new | n/a (logic) | n/a | new | SEH/POSIX install/restore |
| 100 | hw_breakpoint_dr7 | new | n/a (logic) | n/a | thin | build_dr7 bit layout |

### G11 — Pure-logic / helpers
| # | slug | A | M | F | St | notes |
|---|------|---|---|---|----|-------|
| 101 | signature_parsing | new | n/a (logic) | n/a | thin | JVM descriptor → BasicType |
| 102 | decode_u5_unsigned5 | new | n/a (logic) | n/a | thin | UNSIGNED5 decode |
| 103 | value_t_variants | new | n/a (logic) | n/a | thin | variant conversions |
| 104 | traits_function_traits | new | n/a (logic) | n/a | thin | function_traits/tuple |
| 105 | logging_format | new | n/a (logic) | n/a | thin | format_log fallback |
| 106 | version_macros | new | n/a (logic) | n/a | green | version macro consistency |
| 107 | platform_capability_macros | new | n/a (logic) | n/a | green | capability macros |
| 108 | odr_inline_safety | new | n/a (logic) | n/a | green | 2-TU ODR |
| 109 | decode_oop_and_pointers | new | n/a (logic) | n/a | thin | untag/filter pointers |
| 110 | array_element_helpers | new | n/a (logic) | n/a | thin | array_length/get/set |
| 111 | iterate_entries_safety | new | n/a (logic) | n/a | thin | VMStructs iterate safety |

### G12 — Build / infra / injection / packaging
| # | slug | A | M | F | St | notes |
|---|------|---|---|---|----|-------|
| 112 | injector_remote_loadlibrary | new | n/a | n/a | new | injector/ tool audit |
| 113 | dllmain_bootstrap | new | n/a | n/a | new | DllMain/entry bootstrap |
| 114 | cmake_build_matrix | new | n/a | n/a | new | CMake/build audit |
| 115 | ci_workflow_matrix | new | n/a | n/a | new | .github/workflows audit |
| 116 | header_packaging_single_header | new | n/a | n/a | new | single-header packaging |
| 117 | api_surface_no_jvm | new | n/a (logic) | n/a | green | no-JVM API callable surface |

**Total roster: 117 features → ≥100 specialist agents. ✅**

## Wave plan (phases)

- **Phase 0 — Foundation** *(in progress)*: this doc, memory, baseline confirmed green
  @e23d1fc. Orchestration model fixed.
- **Phase 1 — JVM harness refactor (Rework D)**: retire legacy inline `test_*()` +
  legacy fixtures from `example.cpp`; modular-only driver; shrink example.cpp. Serial.
- **Phase 2 — Test fan-out waves (G1…G12)**: per group, parallel-author exhaustive
  modules+fixtures+agent-defs; compile; push; watch CI; serial-fix failures. Repeat.
- **Phase 3 — Library fixes**: FIX A/B/C/D/E + FLAW A + every flaw the agents surface.
- **Phase 4 — Repo refactor**: delete dead code, build dirs, dead fixtures; restructure.
- **Phase 5 — Per-file audit**: every tracked file audited + improved, recorded below.

## Local compile-only validation (NEVER runs tests)

- C++ (`-Werror`, compiles header + ALL modules + pure-logic tests):
  `cmake --build build-mingw -j`  (build-mingw configured: MinGW g++, WARNINGS_AS_ERRORS=ON)
- Java fixtures (compile, no run): `javac -d /tmp/fixcheck example/vmhook/*.java example/vmhook/fixtures/*.java`
- DO NOT run `etc/run_local_mingw.sh` (it launches the JVM + injects + RUNS tests = violates rule).
- DO NOT run `ctest`. GHA is the only test oracle.

## Phase 1 (Rework D) prep — legacy inline `test_*()` → module parity map

Call-site mapping (example.cpp:3226-3282 → modules) shows 1:1 coverage — every legacy
inline test has a module equivalent (claim "all migrated" looks accurate):
make_unique→make_unique · method_hook→hook_basic · force_return→return_set_primitives ·
cancel→return_value_cancel · list/linked/set/map/hash/tree→collection_* · poly→
poly_inherited_oop+interface_polymorphism · arg_mutation→return_set_arg · edge_primitives→
field_primitives_get · string_edge→field_string+read_java_string · array_edge→field_arrays_* ·
enum→enum_singleton · interface→interface_polymorphism · nested→nested_classes · throwing→
method_throwing_call_site · overloaded→method_overload(+_java_dispatch) · return_types→
method_return_types · for_each_*→for_each_* · read_java_string→read_java_string · global_ref→
global_ref · method_enumeration→method_enumeration · on_exception→on_exception · scoped_hook→
scoped_hook_raii · caller_info→return_caller · field_watcher→watch_static_field ·
class_load_watcher→on_class_loaded.
**Phase 1 GATE before deletion:** diff each legacy body vs its module to confirm the module
asserts ≥ what the legacy test did (don't lose coverage). Then delete the legacy section +
legacy top-level fixtures, shrinking example.cpp to a thin modular-only driver.

## Wave 1 agent-reported library bugs (pinned by tests; fix in serial header pass — task #5)

- **[medium] `make_java_array` leaks a PENDING JNI exception on the miss path**
  (`jni_find_class`, vmhook.hpp:9297-9314 via 11314-11322). Surfaces under `-Xcheck:jni`
  / fastdebug. Fix: `ExceptionClear` after a failed FindClass fallback.
- **[medium] `field_proxy::set()` has a SIZE guard but NO TYPE guard** (vmhook.hpp:12167-12180):
  a wrong-type value of the right width slips through.
- **[low] `method_proxy::call()` silently drops args past 8** (vmhook.hpp:13277) — no diagnostic.
- **[low] `call()` `case 'F'` decodes through a signed int32_t** (vmhook.hpp:13378).
- **[low] `find_methods_by_signature` does no descriptor validation** (vmhook.hpp:7088) — a
  malformed/typo'd descriptor is an indistinguishable silent empty; doc says inherited names
  but only declared are returned (doc/behaviour drift).
- **[low] `make_java_array` hardcodes x64 compressed-oops arrayOop layout** (header +12/+16).

## High-priority queued findings (act in serial passes)

- **[CI] Add Java 26 to the matrix** — `ci.yml:18 env.JAVA_VERSIONS` is hardcoded
  `["8","11","17","21","24","25"]`. **Eclipse Temurin 26 went GA 2026-04-13** (verified
  via adoptium.net), so "every Java version up to latest" now requires **26**. Add "26"
  (and keep an eye on the cadence: 27 ≈ Sept 2026). `actions/setup-java@temurin` resolves
  GA versions. Serial edit + watch CI (a bad version id fails all JVM jobs). Also reword the
  env comment to note the 6-month cadence so "latest" is maintained going forward.
- **[build] Example DLL is `NO_WERROR`** (CMakeLists:205) by design — module style-warnings
  don't fail CI; only hard errors + the strict first-party `tests/test_*.cpp` (-Werror) do.
  So new-module integration only needs to *compile* clean (errors), not be warning-free.

## Progress log

- **2026-06-06** — Phase 0 done. Baseline e23d1fc fully green (45 CI jobs). Built feature
  catalog (117 features), orchestration model, this spine. 37 specialist agent-defs
  pre-exist + spawnable; 60 JVM modules, 76 fixtures, 28 pure-logic tests in tree.
  Confirmed: existing modules are EXEMPLARY (field_primitives_get ~150 checks), so program
  pivots to (a) cover the ~50 uncovered features, esp. G9 HotSpot-internals cross-JDK
  linchpins, (b) refactor, (c) per-file audit. Fixture mechanism: auto-discovered by
  Main.loadFixtures + auto-globbed by CMake → new modules/fixtures self-wire (no driver edit).
- **2026-06-06** — Phase 2 Wave 1 LAUNCHED (workflow wf_0f3f75ae-7a5, 5 parallel agents):
  field_primitives_set, method_call_wide_args, field_null_safety, find_methods_by_signature,
  make_java_array. Each authors fixture + module + agent-def.
- **2026-06-06** — Wave 1 INTEGRATED + PUSHED (commit 18ed5b5). ~520 ctx.check assertions
  across 5 modules. Validated: C++ compile clean (build-mingw exit 0), javac fixtures clean,
  3/5 modules deep-reviewed (excellent), 2/5 quality-scanned (high density, no vacuous checks).
  CI running (watch bv3bq44q2). Roster now 41 agent-defs (method_call_wide_args def pending
  workflow finalize → follow-up commit).
  **CI CONSTRAINT LEARNED:** ci.yml has `concurrency: cancel-in-progress: true` — every push
  cancels the in-flight run. So BATCH commits and push once per CI cycle; never push while a
  run you care about is live. (Wave-1 agent-def follow-up + Java-26 edit are batched to land
  after this run reports.)
  NEXT once green: (1) commit method_call_wide_args agent-def, (2) add Java 26 to matrix,
  (3) Wave 2 = G9 HotSpot-internals modules. If red: serial-fix per the loop.
- **2026-06-06** — Wave 1 CI RESULT: build FAILED on all compilers — `method_call_wide_args.cpp`
  had a dangling `(void)cap_wrong;` referencing a helper the agent removed in a post-commit
  edit. ROOT CAUSE: I committed 18ed5b5 while the authoring workflow was STILL finalizing files
  (the agent kept editing method_call_wide_args.cpp after I read+committed it), AND my local
  `cmake --build` had passed on a STALE object (CONFIGURE_DEPENDS/mtime). TWO LESSONS BANKED:
  (1) **wait for the workflow completion signal before committing its output**; (2) **force a
  fresh recompile** (delete the obj / clean) to validate — a plain build can pass on stale objects.
  Working tree already had the agent's corrected module; re-validated with a FORCED recompile
  (saw method_call_wide_args.cpp.obj rebuild, exit 0). Re-pushing build fix + the 2 trailing
  agent-defs (method_call_wide_args, make_java_array). The other 4 modules compiled fine on CI.
  Also CONFIRMED: pre-existing JVM flakes are real + SEPARATE — a doc-only commit (76f29d5)
  failed ONLY on jvm·linux·gcc·java11 + jvm·windows·clang·java25 (the milestone's GC/thread-
  timing flake class), not the build. Wave 1's re-run will be judged by: build jobs green +
  no [FAIL] from the 5 NEW modules; java11/java25 flakes are the separate deflake workstream.
