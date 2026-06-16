---
slug: deoptimize_methods
title: Deoptimize Methods
category: deopt
status: in_progress
risk: medium
java_versions: [8, 11, 17, 21, 24, 25, 26]
tags: [status/in_progress, risk/medium, category/deopt, tag/jit, tag/deoptimization, tag/interpreter, tag/entry-points, tag/klass-walk, tag/fault-safe]
---

# Deoptimize Methods

> **Category:** [[categories/deopt|De-optimisation]]  ·  **Status:** `in_progress`  ·  **Risk:** `medium`  ·  **Specialist:** `.claude/agents/deoptimize_methods-specialist.md`

## Description

Deoptimization API to force JIT-compiled Java methods back to interpreted execution,
enabling hooks installed on the interpreter entry (`_i2i_entry`) to fire on already-inlined
callers. Two entry points: `deoptimize_all_jit_compiled_methods()` (force every compiled
method back to interpreted) and `deoptimize_methods_if(predicate)` (force only methods the
predicate selects). The sweep walks EVERY loaded klass (including array klasses) fault-safely
via `safe_read_pointer` / `safe_klass_methods`, nulls each method's `_code`, and redirects
entry points to interpreter + c2i adapter. Crash-safe by construction: used by deopt sweep
means MSVC __try/__except contains EXCEPTION_ACCESS_VIOLATION on unreadable cold klasses/Methods.

## Depends on

- [[features/method_enumeration|method_enumeration]]
- [[features/adapter_recovery_c2i|adapter_recovery_c2i]]
- [[features/method_entry_points_i2i_i2c|method_entry_points_i2i_i2c]]
- [[features/for_each_loaded_class|for_each_loaded_class]]
- [[features/vmstructs_offset_resolution|vmstructs_offset_resolution]]
- [[features/instanceklass_methods_walk|instanceklass_methods_walk]]

## Depended on by

- [[features/hook_basic|hook_basic]]
- [[features/hook_install_after_jit|hook_install_after_jit]]

## Implementation anchors

- `deoptimize_methods_if(predicate_type&&)` — `vmhook/ext/vmhook/vmhook.hpp:8170-8301` — Template function that selectively deoptimizes methods matching predicate; walks every loaded klass fault-safely.
- `deoptimize_all_jit_compiled_methods()` — `vmhook/ext/vmhook/vmhook.hpp:8320-8325` — Convenience wrapper calling deoptimize_methods_if with always-true predicate.
- `safe_klass_methods(klass*, Method**&, int32_t&)` — `vmhook/ext/vmhook/vmhook.hpp:7653-7695` — Fault-safe read of klass's Method array (pointer + clamped length); returns false on unreadable/array klasses.
- `safe_method_pointer_field(method*, vm_struct_entry_t*, void*&)` — `vmhook/ext/vmhook/vmhook.hpp:7713-7732` — Fault-safe read of Method fields (_code, _i2i_entry, _adapter) via safe_read_pointer; critical for deopt sweep.
- `for_each_loaded_class(visitor_type&&)` — `vmhook/ext/vmhook/vmhook.hpp:8104-8116` — Walks every loaded klass via class_loader_data_graph.for_each_klass (snapshot at call time).

## Tests

- `tests/jvm/modules/deoptimize_methods.cpp`

## Known bugs

- **[high]** Array-klass walk hazard at line 8178-8180 / 7636-7639: for_each_loaded_class yields array klasses (non-InstanceKlass), safe_klass_methods rejects them by design (returns false at 7674-7675 on non-InstanceKlass). Risk is a cold/mid-GC-relocation array klass passes is_valid_pointer but faults on raw _methods read — mitigated by safe_read_pointer at 7671-7673 + try/catch at 8107, but MinGW has NO SEH (seh_invoke gated at runtime) so uncontained AV crashes JVM mid-suite.
- **[medium]** Double-sweep idempotence: deoptimize_methods_if called twice back-to-back on the same live-compiled method with no intervening recompile — the second call reads _code == nullptr (line 8243-8244) and skips it (continue at 8247), so no crash, but semantics are a no-op on the second call (healthy JVM behavior proved in module tests).
- **[medium]** Predicate edge case at line 8249: if predicate throws an exception (not noexcept), the sweep propagates it and may leave methods in a half-deoptimized state (some methods had _code cleared, some did not). The function signature does not enforce noexcept on the predicate, only on the function itself (line 8171).

## Notes

JDK sensitivity: _adapter field is exported via VMStructs on JDK 8 only (line 8206-8207 iterates it).
On JDK 9+ the field is absent, so the sweep falls back to get_adapter() heuristic at line 8273 — this is safe because
_code proved the Method's page is mapped beforehand (line 8242-8244). The heuristic scan (in-object, memcpy-bounded)
stays on the committed Method allocation and cannot fault. Array klasses (type arrays, object arrays) are deliberately
rejected by safe_klass_methods and never walked past line 7674; cold klasses that are mid-GC-relocation are the primary
crash-hazard if any read dereferences the raw klass/Method/array without safe_read_pointer or is_valid_pointer gates.
