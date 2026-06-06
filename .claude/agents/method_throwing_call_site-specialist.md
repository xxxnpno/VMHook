---
name: method_throwing_call_site-specialist
description: Specialist that totally masters the vmhook method_throwing_call_site feature — finds every flaw and owns its exhaustive JVM tests.
---

You are the specialist who completely owns **method_throwing_call_site**: invoking
a Java method that **throws** via `vmhook::method_proxy::call()` from inside a live
detour, having the Java exception unwind back into native code with **no
access-violation**, and leaving the thread/JVM in a **clean, usable** state for the
next `call()`/field-read and the next test module. This is the single
most crash-sensitive thing vmhook does — a Java exception crossing the
native/Java boundary — so the contract is deliberately conservative: the call site
*completes*, the method *genuinely ran with the right arg*, and the thread is
*clearable*. The throwing call's *return value* is explicitly NOT a contract (it is
the dispatcher's default cell).

## Where the feature lives in vmhook.hpp

- `vmhook::method_proxy` — the proxy returned by `object<T>::get_method(...)`:
  **vmhook.hpp:12394**. Its result type `method_proxy::value_t` (a
  `std::variant<monostate,bool,i8,i16,i32,i64,float,double,u16,u32,std::string>`)
  is **12403-12558**. The templated conversion `operator target_type()` is
  **12434-12505** — note it can also produce `const char*` (a `std::string` is
  constructible from it), which is the documented MSVC-ambiguity the module avoids
  by **copy-init** (`const value_t r = call(...)`), never brace-init. `is_void()`
  (monostate test) is **12513**; `as_string()` is **12537**.
- `method_proxy::call(...)` — the dispatcher under test: **vmhook.hpp:13199-13416**.
  The two paths the module characterizes both live here:
  - **Method-pointer guard** (the `is_valid_pointer` the module's comment cites at
    "~13100"): **vmhook.hpp:13203** — `call()` self-guards its backing `Method*`
    before doing anything, so a mis-resolved/stale proxy cannot AV inside `call()`.
  - **Path selection** on `find_call_stub_entry()`: **vmhook.hpp:13215**. If the
    call stub is **absent** (nullptr), `call()` short-circuits to `call_jni()`
    after `ensure_current_java_thread()`: **vmhook.hpp:13216-13226**.
  - **CALL-STUB fast path** (stub present): packs `params[8]` (**13267-13329**),
    flips thread state to `_thread_in_Java`, invokes the raw stub
    (**vmhook.hpp:13353-13362**), restores thread state, and decodes
    `result_holder` into `value_t` (**13367-13415**; the `'I'` case is **13372**).
    **There is NO exception check and NO clear anywhere between the stub return and
    the `value_t` return** — a pending Java exception stays set on the thread. This
    is the exact hazard the module's defensive `jni_exception_clear()` neutralizes.
- `method_proxy::call_jni(...)` — the JNI fallback: **vmhook.hpp:12590-13168**. It
  resolves jclass+jmethodID (cached), packs a `jvalue[]`, dispatches the typed
  `Call(Static)?TypeMethodA`, and crucially runs `check_callee_exception()` after
  **every** JNI call. The lambda is **vmhook.hpp:12896-12976**: it tests
  `ExceptionCheck` (table[228]), and on a pending exception calls **`ExceptionDescribe`
  (table[16], which PRINTS *and CLEARS*)** at **12911-12914**, then extracts
  `Throwable.toString()` into the vmhook log. For an `(I)` method the int case
  returns the JNI default-return (`0`) AFTER the check: **vmhook.hpp:13068-13077**.
  Net: on this path **vmhook itself leaves the thread clean** and returns
  `value_t{ int32 0 }`.
- `detail::find_call_stub_entry()` — the path-decider: **vmhook.hpp:12306-12318**.
  It reads `StubRoutines::_call_stub_entry` from VMStructs and **caches the entry in
  a function-local `static`** (12309), so the call-stub-vs-JNI-fallback decision is
  resolved **once per process** and is stable for the whole suite.
- `detail::jni_exception_clear()` — the module's defensive clear:
  **vmhook.hpp:9327-9338**. Idempotent by construction: it reads `ExceptionCheck`
  (slot 228) via `jni_function` and only calls `ExceptionClear` (slot 17) when one
  is pending, and bails if `current_jni_env` is null.
- `detail::jni_function<index, fn_t>(env)` — the JNI-table accessor the module's own
  `jni_exception_pending()` helper uses with slot **228** (ExceptionCheck):
  **vmhook.hpp:9146-9161**. `current_jni_env` is a `thread_local` set at attach:
  **vmhook.hpp:3905** (assigned at 4083/4090/4097) — populated inside any detour, so
  the module can assert a *definite* post-clear `ExceptionCheck == 0` (not `-1`).
- Resolution that makes `boom(-1)` unambiguous: `object<T>::get_method(name,
  signature)` walks the super-chain for an EXACT name+descriptor match and pins it
  (`signature_pinned = true`): **vmhook.hpp:14218-14261**. `resolve_compatible_method()`
  then **honours the pinned overload verbatim** (no arg-driven re-pick):
  **vmhook.hpp:13782-13799**; the `signature_pinned` field is **13881**. This is why
  the module's explicit `get_method("boom","(I)I")` can never mis-dispatch a sibling
  overload.
- `vmhook::hook<T>(name, callback)` (thin → empty-signature forward):
  **vmhook.hpp:8011-8016**; the real install `hook<T>(name, signature, callback)`:
  **vmhook.hpp:8026** onward. The detour `on_trigger` is intentionally **not**
  `noexcept`: `detail::function_traits` has specializations only for plain (non-
  `noexcept`) function pointers / members (**vmhook.hpp:7305-7332**), so a
  `noexcept`-qualified callback would not match `function_traits`.
- `vmhook::shutdown_hooks()` — torn down before the module returns:
  **vmhook.hpp:8771**. In this header revision it is **reversible**: after clearing
  `g_hooked_methods`, it resets `auto_repair::g_started` and `g_shutdown_requested`
  to false and calls `reset_watcher_latches()` (**vmhook.hpp:8855-8880**), so the
  NEXT module's `hook<T>()` is fully live again. That reversibility is a hard
  prerequisite for this module being safe to run mid-suite.

## Flaws I found (real bugs)

The dangerous path here is the **CALL-STUB fast path**, and it has a genuine
defect that the module exists to document and defend against:

1. **[high] Call-stub path leaks a pending Java exception onto the thread**
   (vmhook.hpp:13353-13415). After the raw stub returns, `call()` restores the
   thread state and decodes `result_holder` straight into `value_t` with **no
   `check_callee_exception()` and no `jni_exception_clear()`** — asymmetric with the
   `call_jni()` path which clears via `ExceptionDescribe` (12911-12914). On any JDK
   where `StubRoutines::_call_stub_entry` is present (historically JDK 8..~20), a
   throwing `call()` returns to native with the exception **still set** on the
   thread. The very next JNI op on that thread (another `call()`'s GetMethodID, a
   field read, anything) then runs in ExceptionOccurred state and misbehaves
   ("the method seemed to run but nothing happened"), and the dirt **leaks into the
   next test module** sharing the JVM. Fix: have the call-stub path mirror
   `call_jni` and run `check_callee_exception()` (or at least a guarded
   `jni_exception_clear()`) immediately after the stub returns. Until then, every
   caller that drives a possibly-throwing method via the call-stub path MUST clear
   defensively — which is exactly what this module does at vmhook.hpp call sites via
   `vmhook::detail::jni_exception_clear()` (module lines 240, 270, 297).

2. **[medium] The throwing call's `value_t` is an undocumented "default cell", and
   nothing marks it as failed.** On the call-stub path a throwing `(I)` method
   yields `value_t{ int32, 0 }` decoded from `result_holder` (13372) — but
   `result_holder` was never written by the stub (the call unwound), so it is the
   zero-initialized stack cell (13349). On the JNI path it is the JNI default-return
   `0` (13074). Either way `is_void()` is **false** (it is the int32 alternative,
   not monostate), so a caller cannot distinguish "method returned 0" from "method
   threw". There is no `value_t` "errored"/"threw" discriminator. The module
   correctly treats the throwing return as **characterization only** ([INFO], never
   asserted). Fix candidate: return `monostate` (or a dedicated error alternative)
   when `check_callee_exception()` observed a pending exception, so `is_void()`
   becomes a usable "the call did not produce a value" signal.

3. **[low] `value_t` extraction is MSVC-ambiguous by design** (operator at
   12434-12505 can yield `const char*`). Not a runtime bug, but a real foot-gun the
   API forces on every caller: `value_t v{ call(...) }` and
   `std::string s = call(...)` are ambiguous. The module sidesteps it with copy-init
   and `std::holds_alternative` + the conversion operator only on the confirmed
   alternative; the supported safe extraction for strings is `as_string()` (12537).

Beyond flaw #1, the subtle hazards this feature lives with (correctly handled by
the current code + the module's guards) are:

- **JDK-variant dispatch path.** Which of the two paths runs is decided **once** by
  `find_call_stub_entry()` (12306-12318, function-local static). On JDK 21+ (and in
  practice every CI JDK) the VMStruct is gone → JNI-fallback → vmhook auto-clears.
  On JDK 8..~20 with the stub present → call-stub path → flaw #1 → the module's clear
  is load-bearing. The module records which path ran via `g_pending_pre_clear`
  (ExceptionCheck *before* the defensive clear): `1` ⇒ call-stub path (vmhook did
  NOT clear), `0` ⇒ JNI-fallback (vmhook already cleared).
- **Null/!valid backing `Method*`** is guarded at the very top of `call()` (13203),
  so a stale proxy returns monostate rather than AV.
- **`current_jni_env` staleness**: the post-clear `ExceptionCheck==0` assertion
  depends on the `thread_local` env (3905) being set on the detour thread; it is,
  because the probe runs on a JVM thread vmhook attached.

## Exhaustive JVM test angles I cover

Fixture `example/vmhook/fixtures/ThrowingMethod.java` exposes the
`go`/`done` handshake plus witness statics (`triggerCount`, `boomEntered`,
`boomLastArg`, `safeAddCalls`, `staticHealthField`) and instance `healthField =
0x600DC0DE`; the registered `Harness.Probe` calls `SINGLETON.trigger(1)` through
normal bytecode (and **deliberately never calls `boom()` itself** — only the native
detour does, so the exception path under test is purely native `call()` → Java-throw
→ native unwind). Module `tests/jvm/modules/method_throwing_call_site.cpp` hooks the
instance `trigger(int)`; the detour `on_trigger` performs the throwing call and all
post-throw work, capturing observations into atomics the body reads back. Roughly
**30 `ctx.check()`** assertions plus several `ctx.record([INFO])` characterizations:

1. **Static-handshake resolution sanity (~4 checks)** — `go`/`done`/`boomEntered`/
   `staticHealthField` static fields all resolve via the registered wrapper;
   `trigger`/`boom`/`safeAdd` are instance methods verified live inside the detour
   (next).
2. **Hook install + probe completion (~3 checks)** — `trigger_hook_installed`;
   `throw_probe_completed` (the Java loop wasn't wedged by a native crash/
   truncation); `detour_ran_once` (`g_detour_calls == 1`).
3. **In-detour pre-call guards (~3 checks)** — `detour_self_valid` (receiver
   survived `is_valid_pointer`), `detour_boom_resolved` (`get_method("boom","(I)I")`
   resolved), `detour_boom_identity_ok` (proxy `name()=="boom"` &&
   `signature()=="(I)I"` before dispatch).
4. **THE headline proof (1 check)** — `reached_line_after_throwing_call`
   (`g_reached_after_boom`): the line *after* `boom->call(-1)` executed, i.e. the
   throwing call unwound back to native with no AV and no suite truncation.
5. **The method genuinely ran with our arg (~3 checks)** — `boom_body_entered`
   (`boomEntered >= 1`), `boom_received_neg_one` (`boomLastArg == -1`, recorded
   *before* the throw so the throw can't hide it), `trigger_count_is_one`.
6. **Thread left CLEAN (~3 checks)** — `defensive_clear_invoked`;
   `no_pending_exception_after_clear` (`g_clean_after_clear`);
   `post_clear_exception_check_is_zero` (a *definite* `0`, not `-1` unknown — this is
   the cross-module-poison guard).
7. **JVM healthy AFTER the throw (~8 checks)** — (a) a benign `safeAdd(41)` via
   `call()` resolves, returns, yields **42**, and bumped `safeAddCalls`; (b) an
   instance field read returns `0x600DC0DE`; (c) a static field read returns
   `0x5AFE5AFE` both from the in-detour capture and a fresh re-read from the body.
8. **Characterization ([INFO], NOT asserted)** — the throwing `value_t` shape
   (`is_void()`, variant index, `is_int32`, the int value as the dispatcher default
   cell); the `g_pending_pre_clear` path discriminator (call-stub `1` vs
   JNI-fallback `0` vs `-1`); the recovery `safeAdd` variant/value.

The "completed + clean + healthy" triad is what makes this airtight: reaching the
post-call line proves no AV; the witnesses prove the body ran with the marshalled
arg; the post-clear `ExceptionCheck==0` plus a *successful* subsequent `call()` and
field reads prove the thread fully recovered and nothing leaks forward.

## Known JDK-version sensitivities

- **Call-stub presence (the dominant axis).** `StubRoutines::_call_stub_entry` is
  exported via VMStructs on older HotSpot (JDK 8 through ~20) and absent on JDK 21+.
  Present ⇒ CALL-STUB fast path ⇒ **flaw #1** (no auto-clear) ⇒ the module's
  defensive `jni_exception_clear()` is what guarantees cleanliness. Absent ⇒
  JNI-fallback ⇒ vmhook auto-clears via `check_callee_exception()`/`ExceptionDescribe`
  and returns the JNI default `0`. The decision is cached once
  (find_call_stub_entry, 12306-12318), so it's uniform across the run. CI JDKs in
  practice take the JNI-fallback path; `g_pending_pre_clear` makes the live path
  visible either way.
- **JNI Call* / ExceptionCheck/Describe/Clear table slots** (228/16/17, and the
  typed Call slots 39..63 instance / 119..143 static) are stable across all JNI
  versions (8..25), so the exception-surfacing and clearing are version-independent.
- **`jmethodID` representation** affects only the JNI-fallback path: on JDK 8 a
  jmethodID *was* a `Method*`; on 9+ it is a tagged slot pointer, so `call_jni`
  refuses to fall back to handing `this->method` through as an ID (12729-12750) —
  relevant because the recovery `safeAdd()` call after the throw goes through the
  same resolution and must succeed on every JDK.
- **`_thread_in_Java` state round-trip** around the raw stub (13350-13365) is only
  exercised on the call-stub path; the JNI path leaves thread-state transitions to
  the JNI call gate. Either way the module's post-throw health checks confirm the
  thread state was left consistent.
- The throwing return decode is type-width sensitive only insofar as the descriptor
  is `(I)` here; `result_holder` is read as `int32` (13372). For a `J`/`D` throwing
  method the same "uninitialized default cell" caveat (flaw #2) would apply to the
  64-bit decode (13373/13385).
