# vmhook library-fix execution plans (criterion 3)

Source-verified fix-plans from the 10-agent investigation wave (2026-06-20), one per high-leverage AUDIT_FINDINGS bug. Each was confirmed against current `vmhook/ext/vmhook/vmhook.hpp` (the header is ~21,434 lines; AUDIT line numbers had drifted — these are re-verified). **Apply library fixes ONE AT A TIME with mingw pre-flight + GitHub CI** (the header is the crown jewel; the dictionary fix crashed once). Order: safest/default-visible first.

## TIER 1 — clean, additive, default-config visible (apply first)

### 1. set_prim_array stride bug — CRITICAL (heap corruption)
- **Bug:** `set_array_element<T>` (vmhook.hpp:14625) strides the Java backing array by `sizeof(C++ T)`, not the Java element width. Writing `vector<int>` into a `byte[]`/`short[]`/`char[]` overruns each element into adjacent heap → corruption. The READ path (`read_array_value`, ~14806) and scalar `set` (~15602) already have a width guard; `set_prim_array` (20502-20531) does not.
- **Fix:** add an element-width guard at the top of `set_prim_array` (mirror read_array_value): `if constexpr (!is_same_v<T,char>)` → compute `jvm_primitive_byte_width(signature.substr(1))`; if `!=0 && sizeof(T)!=width` → `VMHOOK_LOG` + return (refuse). char/"[C" carve-out preserved.
- **Test:** `field_arrays_primitive.cpp` — write wide-into-narrow → array + neighbor-sentinel unchanged (refused); matching-width still round-trips.
- **Risk:** none on matching-width (guard false); only refuses the corrupting case. noexcept preserved.

### 2. for_each_thread raw derefs — HIGH (#28 cold-fault, JVM-killing)
- **Bug:** thread enumeration raw-derefs JavaThread/thread-list fields (sites 4761, 4804/4814, 4937, 8486-8506) gated only by `is_valid_pointer` (heuristic), on no-SEH legs (mingw/clang-cl) → AV tears down the JVM. for_each_instance routes all such reads through `safe_read`/`safe_read_pointer`.
- **Fix:** 4 patches — replace each `*reinterpret_cast<T*>(addr)` with `safe_read_pointer`/`os::safe_read` into a local, keep the `is_valid_pointer` gate, ADD the missing `threads_array` is_valid_pointer gate at 8506 (the twin at 4998 has it). Also sweep get_thread_state/get_suspend_flags (4660-4720).
- **Test:** `for_each_thread.cpp` non-regression on mingw×8-26 (warm path byte-identical; safe_read returns same bytes for mapped slots).
- **Risk:** none on warm path. Plain struct-field reads (not frame-walk), so no POSIX range-check regression risk → apply uniformly.

### 3. call_jni cache mis-keying across overloads — HIGH (JDK21+) — DONE (cached_keyed_signature)
- **Bug:** call_jni caches jmethodID/jclass/cached_ret_char per-proxy (16258/16282/16324) gated by presence (`!cached_*`), NOT by signature. A reused name-only proxy that calls overload A then B reuses A's methodID + ret_char for B → wrong method invoked, return decoded as wrong type. Surfaces on the call_jni path (every CI JDK; "JDK21+" = where call stub is absent).
- **Fix:** add `mutable std::string cached_keyed_signature;`; right after `effective_signature` is computed (~16264), `if (cached_keyed_signature != effective_signature) { reset cached_method_id/class_handle/ret_char; cached_keyed_signature = effective_signature; }`. The existing `!ret_char`/`!cached_method_id` gates then re-resolve.
- **Test:** `method_call_jni_fallback.cpp` + fixture — `int combo(int)` / `String combo(String)`, call both through ONE held proxy, both orders. Pre-fix the 2nd call returns wrong type.
- **Risk:** ~nil — first/same-overload path unchanged (one string compare); signature-pinned proxies stable.

### 4. static primitive set() stale field pointer post-GC — HIGH
- **Bug:** `field_proxy::set()` primitive arm (15549/15582/15614) raw-`memcpy`s through cached `field_pointer` with NO re-resolve and NO safe_write — but the GET path (15328-15336) and `store_object_oop` (15799-15818) both re-resolve via `mirror_klass` + use safe_write. A relocating GC moves the static mirror → stale write (lost/AV).
- **Fix:** add `resolve_write_pointer()` const helper (mirror store_object_oop), route the 3 primitive stores through it + `os::safe_write`. (Free fn `set_field` at 14012 already re-resolves; just swap memcpy→safe_write.)
- **Test:** `field_static.cpp` — static set after forced System.gc + young churn, read back via Java getstatic (not native peek).
- **Risk:** low — instance/legacy-ctor fields keep field_pointer verbatim (mirror_klass null); precedented by 3 sibling paths.

### 5. find_class array names unresolvable — MEDIUM (correctness)
- **Bug (A):** `find_class("[I")`/`"[Ljava/lang/String;"` → null on JDK8-17 (array klasses aren't in any Dictionary; the JNI fallback's loadClass rejects array names). FindClass accepts them; make_java_array already works around it.
- **Fix (A):** after the empty-name guard in find_class (~8022), `if (class_name.front()=='[') { mirror=jni_find_class(name); klass=jni_klass_from_class_mirror; DeleteLocalRef; jni_exception_clear(); return klass; }`.
- **(B) pending-exception leak:** REFUTED — already fixed (entry+boundary jni_exception_clear at 11979/12156/12168/12173/12188 + load_threw snapshot-then-clear). No action beyond the one clear inside (A).
- **Test:** `find_class_fallback.cpp` — promote the array `[INFO]`s (266-339, 478-486) to HARD checks; add a missing-element-array no-leak negative.
- **Risk:** low — gated on `[`; null→klass is strictly better. (TEST COUPLING: the [INFO]→hard flip.)

### 6. injector HMODULE truncation — HIGH (correctness; rare empirical)
- **Bug:** `injector/src/main.cpp:179-189` uses the 32-bit thread exit code (GetExitCodeThread/DWORD) as the 64-bit HMODULE success oracle → a 4GiB-aligned module base (low dword 0) misreports success as failure. (Can't false-positive a real failure.)
- **Fix:** drop the exit-code test; after WaitForSingleObject, verify the DLL is in the target's module list via `K32EnumProcessModulesEx`+`K32GetModuleBaseNameW` (no psapi link → keeps the static-link self-contained story). `dll_path` already in scope.
- **Test:** CI injection step stays green; self-process unit check of the matcher; negative (bad DLL → false).
- **Risk:** low; only fixes false-negatives. Verify CMake link unchanged (use K32* prefix).

### 11. reference (String) static read via inherited/interface mirror — MEDIUM (correctness; found batch-18)
- **Bug:** reading a REFERENCE (String) `static final` field through `wrapper::static_field(name)->get()` returns the WRONG value when the declaring klass is reached via inheritance or an interface, while (a) the PRIMITIVE sibling (long) resolved through the SAME mirror reads correctly AND (b) a reference static on a DIRECT class (field_static.cpp) reads correctly. Repro (both fail uniformly mingw×8-26, siblings pass): field_inherited.cpp `iface_const_str_value_via_iface_wrapper` (IFACE_CONST_STR on the interface's own mirror) + `static_inherited_grandparent_str_value` (Base.sStr via the depth-2 child wrapper).
- **Suspected:** the reference-static VALUE read (field_proxy::get reference branch / static-mirror+offset resolution ~vmhook.hpp:13947) mis-resolves the declaring-klass mirror or the static-oop offset for a NON-DIRECT declaring klass, or decodes the compressed oop against the wrong base. The primitive path (raw bits at declaring_klass mirror+offset) is correct — isolating it to the reference decode/offset for inherited/interface holders. NOTE: resolution + is_static/is_reference + signature all PASS; only the value differs.
- **Test:** the two reads are currently GATED [INFO] in field_inherited.cpp (resolution/signature/is_reference kept HARD). The fix flips the two value reads back to HARD `== "iface-const"` / `== "base-static-str"`.
- **Risk:** unknown until root-caused — investigate FIRST; the read path is shared with the working direct-class + primitive cases, so a fix must not regress those. Add a field_static interface/grandparent reference-static cell to lock it.

## TIER 2 — needs special handling / coupling (apply after Tier 1, carefully)

### 7. register_class factory-map asymmetry — HIGH (cross-type downcast UB)
- **Bug:** register_class insert_or_assign's the type→name map (8767) but `emplace`s the name→factory map (8775, first-wins). Re-binding a name to a different type leaves the factory building the OLD type; `extract_frame_arg`'s unique_ptr branch (9417-9429) then `static_cast<New*>(new Old)` → UB. Only reachable via re-registration; only the unique_ptr hook-arg path (field/method value_t paths are factory-independent).
- **Fix:** Option B (complete) — re-key `g_type_factory_map` by `std::type_index` (decl ~1671, write 8775, consumer 9417-9426 collapses to one lookup); dissolves the stale-leak too. Option A (1-line) — `insert_or_assign` the factory (residual N-to-1 + benign leak).
- **TEST COUPLING (critical):** `register_class.cpp` currently PINS the bug as expected (checks 547, 608, 707, 712 + the [INFO] at 715). These INVERT under the fix and MUST be rewritten in lockstep, plus add a live-decode regression (shared-name → correct wrapper reads its sentinel).
- **Risk:** first-register path unchanged; the work is the coupled test rewrite.

### 8. uncompressed-class-pointer klass read path — MEDIUM (non-default only)
- **Bug:** 5 sites (8690, 13306, 14949, 17314, 18853) read a 32-bit narrow klass at hardcoded +8 + decode; no uncompressed (full 64-bit Klass*) fallback, no resolved flag. `-XX:-UseCompressedClassPointers` (or -UseCompressedOops) → wrong klass (passes is_valid_pointer → silent mis-decode). The WRITE path (make_java_object 14123-14140) already branches on the oopDesc::_metadata VMStructs.
- **Fix:** `klass_from_header_bytes(header)` helper using `iterate_struct_entries("oopDesc","_metadata._compressed_klass" / "_klass")`; route all 5 sites through it; collapse the klass_from_object_header duplicate. ALSO flag UseCompactObjectHeaders (Lilliput, JDK24/25) → fail-closed (return nullptr) rather than mis-decode.
- **Test:** needs a NEW JVM matrix cell with `-XX:-UseCompressedOops -XX:-UseCompressedClassPointers` (default CI is structurally blind). Pure-logic test of the branch dispatch via stubbed VMStructs.
- **Risk:** nil on default (same 4 bytes at +8); the uncompressed branch reads 8 bytes → widen each site's safe_read buffer to ≥16.

### 9. CDS dictionary bucket-chain truncation — HIGH-labeled / really MEDIUM — RISKY, EXPERIMENT-GATE
- **Bug:** untag_pointer (2092) leaves bit 0; is_valid_pointer rejects odd (2059); the 4 dict chain sites (4344/4364/4394/4413) truncate at the first CDS-marked node. BUT: a prior bit-0-clear fix CRASHED mingw·java8 AND didn't even fix the misses (bit-0 not the whole story — array klasses are in neither dictionary).
- **Fix (only as a gated experiment):** scoped `untag_hashtable_entry` (clear bit 0 at the 4 chain sites ONLY, never global) + a STRICTER `is_plausible_dictionary_entry` (is_readable_pointer via VirtualQuery + structural +16-klass-has-readable-name check) so garbage odd pointers are still rejected. Add the missing inner-loop cap to find_klass (4346-4365).
- **Validation:** MUST run on mingw·java8 (CDS on AND -Xshare:off), ≥10 consecutive no-crash runs, in a separate concurrency group. If the recovered tail doesn't actually add the missing classes → DO NOT LAND (bit-0 isn't the bottleneck). Keep the current for_each_loaded_class JDK8 best-effort gate regardless.
- **Risk:** HIGH (re-crash). Defer to a focused JVM-debug session.

## TIER 3 — defer (low/contracted)

### 10. hook_handle::stop() vs lock-free common_detour scan — LOW (contracted)
- **Finding:** the UAF *can* occur on concurrent remove-vs-dispatch, but the audit's mechanism is WRONG: stop() DOES take g_hooked_methods_mutex (11143); the reader (7187) is INTENTIONALLY lock-free by documented design contract ("only mutate from the driver thread between dispatches"), which the library itself honors (in-tree reachability ≈0). External callers removing a hook from a worker thread while Java traffic flows hit it.
- **Fix (only if enforcing):** reserve(MAX_HOOKS) once + retire-not-erase (tombstone method/detour under lock, defer physical erase to shutdown) + reader guard `&& hook.detour`. NEVER lock common_detour (would serialize every hooked call + risk safepoint lock-order inversion).
- **Risk:** the wrong fix (locking the reader) is worse than the bug. Keep the contract; harden docs.
