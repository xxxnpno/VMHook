# vmhook — Zero-JNI & Zero-Ceremony Roadmap

**Target:** v0.6.0 · **Written:** 2026-08-04 · **Driver:** make vmhook usable in `npnoqol`
without the consumer ever touching an oop, and remove the last JNI from the project.

Evidence for every claim below is in `docs/research/` (six audits + one live-JVM probe).
Line numbers refer to `vmhook/ext/vmhook/vmhook.hpp` at `27db40e`.

---

## 1. The finding that reframes everything

> **vmhook hands the consumer *addresses* and asks the consumer to keep them fresh.**

Every runtime pathology npnoqol has suffered in its entire history is an address that went
stale between the read and the use. The consumer-side audit counted what that costs in a
14.4 kLOC codebase that uses just 23 vmhook entry points:

| Symptom | Count |
|---|---|
| `jni::global_ref` pins hand-managed | 46 |
| `.oop()` re-reads | 67 |
| `get_instance()` calls | 95 |
| raw `void*` oop carriers | 39 |
| `std::make_unique<sdk::T>(oop)` re-wraps that are pure ceremony | 17 |

The single most damning artefact reads **16 primitive fields off one entity using 17 address
re-reads and 17 null checks** (`flag_manager.cpp:632-652`). Roots are never cached, so
`theWorld` is re-read 4× and `thePlayer` 3× *per tick*.

That is not a consumer style problem. It is the consumer paying, by hand and in every
function, for a lifetime model the library declined to own. **Goal B is therefore the real
work, and it is worth more than Goal A.**

---

## 2. The two goals

### Goal A — Zero JNI
No `<jni.h>`, no JNI type, no JNIEnv call, no `jni::` namespace, no `jni_*` public name, and
no comment describing JNI behaviour the code no longer has — anywhere in the project, not just
the header. Every feature must still work afterwards; "zero JNI" achieved by deleting
functionality is not achieving it.

### Goal B — Zero ceremony
The consumer never writes, sees, or reasons about a raw address, a pin, or a re-resolution.
The library owns object identity and lifetime. A handle is valid or explicitly empty —
**never dangling**.

---

## 3. Where the project actually stands

### 3.1 Good news: the header is already JNI-free in code

Three independent greps confirm `vmhook.hpp` has **zero** real JNI: no include, no type, no
forward declaration, no JNIEnv/JavaVM table call, no `JNI_GetCreatedJavaVMs`. All 128
`jni`/`JNI` hits are names, strings, and comments.

So Goal A in the header is **naming and documentation**, plus one genuine functional hole
(§3.3). The only real JNI left in the project is `viewer/payload/payload.cpp`.

### 3.2 Bad news: master is red, and has been since the de-JNI work

| # | Failure | Where | Root cause |
|---|---|---|---|
| **B1** | `warnings-as-errors (linux/clang)` fails | GitHub CI | 3 dead private fields (15891, 15892, 15897) left by the de-JNI removal, tripping `-Wunused-private-field` |
| **B2** | **284 `[FAIL]`** in the JVM suite, all 21 Windows cells | GitHub CI (the only hard JVM gate) | `msp_arg_echo_*` is method-**invocation** coverage — same area the de-JNI work touched |
| **B3** | MSVC `/WX` fails on `tests/test_iterate_entries_safety.cpp` (C4127 ×3) | local only | CI never runs `/WX` on Windows |
| **B4** | **12 test files silently unregistered** from ctest | invisible | the de-JNI refactor dropped them from CMake; coverage vanished unnoticed |

`B1` and `B2` are probably one root cause. **Nothing new should land on top of this** — Phase 0
is getting back to green.

> ⚠ `jvm · linux` and `jvm · macos` are *best-effort* jobs that `exit 0` when results are
> missing. Their green is weak evidence. **`jvm-windows` is the only hard JVM gate.**
> `warnings-as-errors` gates nothing and is Linux-only. `build-and-unit-test` gates all 41
> JVM cells — one no-JVM ctest failure skips the entire matrix.

### 3.2a Crashes woken up by making invocation real

Restoring `method_proxy::call()` turned a silent no-op into actual Java execution, which has
woken up defects that dead code could not produce. **These will redden the JVM matrix until
fixed**, and they are the honest cost of the fix — the calls were never happening before, so
nothing downstream of them was ever exercised.

All three are **fixed** *(commit `827b238`)*. The root causes are worth recording because none was
what the symptom suggested.

**X1 — the synthetic entry frame truncated the GC's stack walk.** ✅ FIXED

The symptom was a GC worker crashing on a *later* collection. The cause was not anchor teardown
but anchor **content**. A detour runs as native code on a JavaThread already in
`_thread_in_Java`, and in that state **HotSpot keeps no frame anchor at all** — measured from
inside a live detour on 21.0.11: `sp=0 pc=0 fp=0 state=8`. `call()` copied that empty anchor
into the synthetic `JavaCallWrapper`, and to HotSpot an entry frame whose wrapper anchor has a
null sp **is the bottom of the Java stack**:

```cpp
bool frame::entry_frame_is_first() const { return entry_frame_call_wrapper()->is_first_frame(); }
bool JavaCallWrapper::is_first_frame() const { return _anchor.last_Java_sp() == nullptr; }
```

So the forced collection **stopped at our entry frame and never scanned the application's real
Java frames underneath**, relocating every object they held without updating their slots. The
next collection walked the dangling oops. (`jvm.dll+0xcad1` disassembles to a virtual call on a
klass word sitting at the compressed-klass base — an oop whose header had been zeroed.)

Fixed by recording the interrupted Java frame in a thread-local from `common_detour` and
publishing it when the thread's own anchor is empty. Two further real defects on the same path
went with it: the caller's `JNIHandleBlock` was being reset under it (HotSpot survives that only
because `JavaCallWrapper`'s constructor installs a *fresh* block first), and the anchor restore
order was inverted — `JavaFrameAnchor::copy()` writes sp **last**, deliberately.

**X2 — `make_java_string()` null on JDK 21/26.** ✅ FIXED. `find_class` short-circuited every
`[` name into `resolve_array_klass()` (`Universe::_*ArrayKlassObj`) and returned that
unconditionally. Those statics exist only on the **JDK 8 generation**; from 9 on the CLDG walk is
the route. The two are exactly complementary, measured. `[Ljava/lang/String;` was unaffected
because it comes from `InstanceKlass::_array_klasses`, which every version publishes — which is
why only *primitive* arrays broke, and with them the compact-String path.

**X3 — `method_static` JVM crash.** ✅ FIXED, and it was **test UB, not the library**: a
`->get()` on a *disengaged* `std::optional`, constructing a `std::string` from a null pointer.
Latent for a long time; it only started faulting when the stack layout shifted. The optional is
empty for a legitimate reason, so the probe is now tri-state and the affected assertions stay
**HARD and red** rather than being papered over.

> ⚠️ **A house-convention hazard this exposed.** The documented style
> `get_field("x")->get()` with no `has_value()` check is **unsafe for any class that may not be
> loaded**, and several modules use it. Worth a repo-wide sweep.

**Result** — whole suite, MinGW, `-Xmx4g -Xmn3g`:

| | before | after |
|---|---|---|
| JDK 21 | **crashed** at `interface_polymorphism`, hs_err, no `TOTAL` | `TOTAL 24027/24552`, completes, no hs_err |
| JDK 26 | — | `TOTAL 24026/24551`, completes, no hs_err |

The 525 remaining failures are the pre-existing B2 baseline, none in the invocation path.
**JDK 8 whole-suite still does not complete — and did not before either** (it crashed at
`method_call_object` at baseline; now it stalls, and the stop point moves between runs). That is
the documented `mingw · java8` fragility, not a regression from this work.

### 3.2b Four latent bugs found during the research

Independent of everything else, and two of them are live heap-corruption risks:

| # | Bug | Where | Severity |
|---|---|---|---|
| **L1** | `store_object_oop` performs a **barrier-less reference store** into a Java object — no card mark. Its doc claims "the field slot is itself a GC root once the reference lands in it", which is **false** for a slot inside a Java object. Old-gen mirror + young referent (everything `make_java_string` just allocated) ⇒ the referent is reclaimed while referenced. `store_string()` is the shipping caller, so `field.set(std::string)` on a String field is exposed **today**. | 14621, 14691 | **high** |
| **L2** | `store_object_oop` **assumes compressed oops unconditionally** — always writes a `uint32_t`. Under `-XX:-UseCompressedOops`, or a heap >32 GB where HotSpot disables compression automatically, that writes 4 bytes into an 8-byte reference slot, corrupting half a live pointer. `encode_oop_pointer` cannot detect the mode. Same assumption is baked into `make_java_array`'s hardcoded `array_header_size = 16` and `+12` length offset. | 14621, 13054, 13068 | **high** |
| **L3** | `make_java_object` can allocate out of **another thread's TLAB**: the fallback walk bumps `ThreadLocalAllocBuffer::_top` on concurrently-running threads with a plain non-atomic read-modify-write. Two allocators can hand out the same bytes. | 12910 | **high** |
| **L4** | `find_class_via_oop` no longer disambiguates by classloader, and `call()` leaves thrown exceptions pending on the JavaThread — both hidden by docs that still describe the old behaviour. | 12495, 15275 | medium |
| **L9** | **The host-classloader subsystem is write-only.** `host_classloader_klass` is latched by `capture_host_classloader_klass` (called from `find_class`), but **nothing ever reads it**; `klass_to_class_loader_oop` exists only to gate that write, and the CAS success branch is an empty block. Documented rather than deleted, since removing it changes control flow. Candidate for deletion. | — | cleanup |
| **L11** | **`vmhook::find_class` is declared `static` at namespace scope in a header** (8424) — internal linkage, so **every TU gets its own copy and its own `klass_lookup_cache`**. `override_class_lookup` seeded in one TU is invisible to `find_class` called from another, contradicting the documented mutex-serialised cache contract. Everything else nearby is `inline`. Found independently by two agents. | 8424 | **high** |
| **L12** | `vmhook::make_unique` breaks any MSVC `/WX` build (C4100, unreferenced parameter pack) when the wrapper has no matching `construct(args...)` — the `else if constexpr` branch only logs. Latent: the MSVC lane does not set `VMHOOK_WARNINGS_AS_ERRORS` and the `warnings-as-errors` job is Linux-only. One `[[maybe_unused]]`. | 12611 | medium |
| **L13** | **No pure-VM successor for `jclass` → `klass*`.** `klass_from_class_mirror` was deleted; `get_java_mirror()` is only the inverse. Confirm whether that capability loss was intentional. | — | open |
| **L14** | `method_proxy::argument_matches_descriptor` is private while its own docs claim `jni_signature_for_arg` "mirrors it EXACTLY" — nothing outside the class can cross-check, so the two ladders can silently drift and the no-JVM lane cannot catch it. | 15539 | medium |
| **L10** | **`store_object_oop`'s GC contract is unsatisfiable as written.** Its doc told callers to root the stored value "via a live JNI local reference" — there is no rooting primitive in the library at all any more. Until mechanism **P** lands, the only available discipline is keeping the alloc→store window short and GC-free. | 14621 | design debt |

L1 and L2 are not incidental: **L1's fix is the same `dirty_card()` helper the pin needs**, and
L2's fix is the oop-width detection the pin's stores need. They are on the critical path, not
beside it.

### 3.3 The functional hole: method invocation

`method_proxy::call` reaches Java through `StubRoutines::_call_stub_entry`, looked up by
`find_call_stub_entry()` (14771-14783) — **eleven lines, one exact VMStructs lookup, no
fallback**. When the lookup misses, `call()` gives up (15068-15079). The consumer audit
reports `call()` behaving as a **silent no-op on every tested JDK**, which is runtime-fatal
and consistent with B2's failing invocation tests.

`viewer/payload/payload.cpp` keeps `<jni.h>` *because of this* — and for three other duties
(thread promotion, detour triggering, allocation) that all exist to manufacture a Java
execution context from a cold native thread.

**MEASURED — and the diagnosis on record was wrong.** A harness was built that loads real
`jvm.dll`s for JDK 8, 21 and 26 in-process, dumps their VMStructs tables, and performs
end-to-end pure-VM invocation. **8/8 tests pass identically on all three.**

| Claim on record | Measurement |
|---|---|
| "JDK 21+ dropped `_call_stub_entry` from VMStructs" | It is **not there on JDK 8 either — it has never been in VMStructs on any version.** `find_call_stub_entry()` returns `nullptr` on *every* JDK, so `method_proxy::call()` has been a **silent no-op everywhere**, not just on 21+. |
| "pure-VM invocation needs a hand-rolled interpreter entry" | No. `StubRoutines::_call_stub_return_address` **is** exposed on 8/21/26, and the entry is deterministically recoverable by a short backward scan for `55 48 8B EC` (`enter()`), validated by the four argument spills. Measured distances: JDK 8 = 179 B, 21 = 404 B, 26 = 175 B — **exactly one candidate in every case.** |
| "the CallStub ABI drifts across versions" | It has **not drifted since JDK 8**. Decoded from live machine code: `rbp+0x10` wrapper, `+0x18` result*, `+0x20` result_type (32-bit), `+0x28` `Method*`→rbx, `+0x30` entry, `+0x38` params*, `+0x40` slot count→r9d, `+0x48` thread→r15. |
| "exceptions can't be cleared without JNI" (15296-15300) | **False.** `ThreadShadow::_pending_exception` is at offset 8 on 8/21/26; read-and-clear verified working. |

**This is almost certainly the root cause of B2's 284 failures** — `msp_arg_engine`-style
invocation tests cannot pass when `call()` never calls anything.

**Verdict: Phase 4.3 is a one-day fix** — a tiered lookup fallback plus a corrected call site.
No interpreter-entry reimplementation needed.

### 3.3b Four more real bugs on the invocation path

| # | Bug | Where | Severity |
|---|---|---|---|
| **L5** | The `link = -1` argument is **a live VM-corrupting bug**. That parameter is a `JavaCallWrapper*`, not a sentinel. Negative control: a GC crashed reading `0x1f` = `(-1) + 0x20` = `JavaCallWrapper::_anchor._last_Java_sp`. Fix: a synthetic 64-byte `JavaCallWrapper` on the C++ stack (`_anchor` at offset 32 on all three JDKs, VMStructs-confirmed). With it, a full `System.gc()` walked the synthetic frame safely, including two stacked frames. | 15143 area | **critical** |
| **L6** | **Long/double argument packing is wrong.** They take **two** slots with the value in the **higher** slot; the header packs into one. Measured: slot 0 → wrong, slot 1 → correct. | 15143 | high |
| **L7** | Calling a `native` Java method **zeroes `_active_handles->_top`**, silently invalidating the caller's local refs. Save/restore `_top` — verified necessary. | invocation path | medium |
| **L8** | `Method::_from_compiled_code_entry_point` does not exist on 8/21/26 — the field is `_from_compiled_entry` everywhere, so the first lookup is dead work. | 3256-3265 | trivial |

Also worth knowing, and worth never "optimising" into the code: **`jmethodID` is no longer a
`Method**` on JDK 25+** — on JDK 26 `GetStaticMethodID` returns a `JmethodIDTable` index (the
literal value `3`). vmhook is immune because it never uses `jmethodID`; keep it that way.

### 3.3c Measured VMStructs facts — never hardcode any of these

Two independent probes (one booting real VMs via `JNI_CreateJavaVM`, one dumping tables live)
agree on all of the following, MEASURED on JDK 8 / 21 / 26:

| Need | JDK 8 | JDK 21 | JDK 26 |
|---|---|---|---|
| call-stub entry | derive from `StubRoutines::_call_stub_return_address` (present on all three) | same | same |
| adjacency of entry to RA in `.data` | `+8` | `+8` | **`−8`** |
| RA − entry distance | 179 B | 404 B | 175 B |
| relocation counter | `CollectedHeap::_total_collections` ✓ (off 56) | ✓ (64) | ✓ (72) |
| `_total_full_collections` | **absent** | absent | absent |
| `SafepointSynchronize::_safepoint_counter` | **absent** | absent | absent |
| GC-active flag | `_is_gc_active` | `_is_stw_gc_active` | `_is_stw_gc_active` |
| card table path | `CollectedHeap::_barrier_set` → `CardTableModRefBS::byte_map_base` *(no leading underscore)* | static `BarrierSet::_barrier_set` → `CardTableBarrierSet::_card_table` (72) → `CardTable::_byte_map_base` | same, `_card_table` at **64**; `_guard_region` **gone** |
| card shift | published (9 / 512) | **not published** | **not published** |
| `CardTableBarrierSet` tag value | — | **1** | **0** |
| narrow-oop spelling | `Universe::_narrow_oop._base` | `CompressedOops::_narrow_oop._base` | **`CompressedOops::_base`** |
| `heapOopSize` | absent | absent | absent |

Consequences for the implementation:

- **Neither adjacency direction nor the RA−entry distance is stable.** Derive, then *validate*
  by the prologue signature (`55 48 8B EC`) and `FF D2` at `RA−2`; fall back to a data scan for
  `max{p : codecache_lo ≤ p < RA}` (verified rank-0 correct on all three). On failure return
  `nullptr` — today's behaviour, so the fallback can only improve things.
- **Card shift must be verified at runtime** (`_byte_map_base + (whole_heap.start >> 9) == _byte_map`),
  not read from a constant that two of three JDKs do not publish.
- **Gate on barrier kind, read from `gHotSpotVMIntConstants`** — the `CardTableBarrierSet` tag
  value differs between 21 and 26, and ZGC/Shenandoah have no card table at all.
- **`UseCompressedOops` is answerable**: the JVM flag table is fully published on all three and
  was walked live. Note `_addr` is at offset **16 on JDK 8 but 0 on 21/26**. This is how L2 gets
  fixed properly.
- **JDK 26 ships Lilliput**: `markWord::klass_shift=42` is published, and when
  `UseCompactObjectHeaders` is on the klass lives in the mark word. Klass decode must check that
  flag on 26+ or it will read garbage. Latent, not yet biting (the flag was 0 in the probe).
- VMStructs is shrinking fast — JDK 26 has 572 entries vs JDK 21's 813, and exactly **one**
  `StubRoutines` field remains. Lookups must be tiered with runtime validation, never
  single-name.
- **Exports are a dead end, measured**: 2998 / 4385 / 4295 exports, with **zero** occurrences of
  `StubRoutines`, `call_stub`, `JavaCalls`, or `SharedRuntime` in any of them.

### 3.4 The lifetime hole: `global_ref` is a no-op that lies

`vmhook::jni::global_ref` (19725-19796) stores the raw oop at construction and returns it
forever. Its doxygen (19703-19723) still promises `NewGlobalRef` pinning and relocation
tracking, and the implementation retracts it 40 lines further down. **A user who reads the
doc block holds a stale pointer across a GC.** 46 npnoqol sites depend on it.

`tests/jvm/modules/global_ref.cpp` (1727 lines) still runs in CI: it drops the last Java
reference, forces a GC, then reads back through the pin. It is guarded (`resolve_oop_guarded`
+ `is_valid_pointer`) and gates its assertions on "safely attainable", so it does not fault —
but `is_valid_pointer` cannot distinguish reclaimed-but-mapped heap from live heap, so it is
reading garbage and calling it `[INFO]`. **It is also a ready-made acceptance harness**: once
a real root exists, its `[INFO]` gates become `HARD`.

---

## 4. The design

### 4.1 The insight the codebase already proves

Static field reads are **already relocation-proof, with zero JNI and zero pinning**.
`field_proxy`'s static overload does not cache `mirror_oop + offset`. It stores the
**GC-stable `Klass*`** plus the offset and re-derives `mirror_klass->get_java_mirror() + offset`
on *every* read (14135-14152). `Klass` is metadata — the GC never moves it — and
`Klass::_java_mirror` is an `OopHandle` (`oop* _obj`) that the GC **updates in place**
(3705-3760).

That is **a GC-stable root plus a path**. The whole design is: generalise it from static
fields to everything.

### 4.2 Four rooting mechanisms, one API

The consumer API is identical under all four. This is the axiom that unblocks the project:
**the pin mechanism is policy, invisible at the call site.**

| # | Mechanism | Cost | Covers | Status |
|---|---|---|---|---|
| **D** | **Anchored path** — GC-stable `Klass*` root + field/element path, re-walked on access | zero roots, a few loads | anything reachable from a static root — *most real usage* | **available today**, no research needed |
| **E** | **Epoch-guarded borrow** — raw oop + GC counter stamp; expires instead of dangling | ~zero | everything, as a safety net | ✅ **confirmed available on every JDK 8..26** |
| **P** | **Real pin** — a Java-object-rooted handle table | one array slot per ref | path-less objects (method results, library allocations) | ✅ **confirmed feasible** — needs 3 new primitives |
| **J** | JNI global ref | one slot | everything | **rejected** — violates Goal A |

**Mechanism D is the default.** npnoqol's hot objects — `theMinecraft`, `theWorld`,
`thePlayer`, the entity list, the tab list — are *all* reachable from a static root by a field
path. Under D they cost zero root allocations per tick, which answers the audit's open question
about whether 3,200 root ops/s is affordable: under D it is 0.

### 4.2b The prior "infeasible" verdict is overturned

`npnoqol/.claude/knowledge/vmhook-migration.md` recorded a 0-JNI pin as *proven infeasible*.
That verdict examined **exactly one mechanism** — calling or hand-rolling
`OopStorage::allocate` — which is indeed unsafe, for four independent reasons. Two other
mechanisms survive scrutiny, and the recommended one is:

**Mechanism P — append to a C-heap root list.** `ClassLoaderData::_handles` (JDK 11-26) or
`JNIHandles::_global_handles` (JDK 8, fully exported). These root lists live **outside the Java
heap** and are enumerated in full on every GC, so **no write barrier is needed at all** — which
is what makes them the right choice. Costs: `_handles` is never exported by VMStructs (its
offset needs a structural signature scan), the append races a lock we cannot take, and capacity
is opportunistic — we must never allocate a VM-owned chunk.

> **Corrected from an earlier draft of this roadmap.** The obvious design — an `Object[]` rooted
> in a class-mirror static field — is **barrier-blocked and must not be used on G1 JDK 8-25**.
> The post-barrier there requires a dirty-card-queue enqueue that VMStructs does not expose, and
> writing the card byte alone is *actively harmful*: it permanently suppresses the JVM's own
> barrier for that 512-byte card. That design is legal only on Serial/Parallel (one
> unconditional byte) and on G1 26+, where JEP 522 removed the queue. Keep it as a
> collector-gated fast path at most, never as the base mechanism.

Useful corollary for L1 and for any future reference store: **writing only into a slot that is
already null is exactly sufficient to skip G1's SATB pre-barrier** — it is the VM's own early
-out. *Clearing* a slot is not; prefer leaking a slot to clearing one.

**Coverage: Serial, Parallel, G1 on JDK 8..26. ZGC and Shenandoah are out of scope and must be
detected and refused** — they relocate concurrently behind load barriers, so a raw oop read by
native code is stale before it is stored. That invalidates vmhook's entire direct-memory model
there, not just the pin. Refusing loudly is the only correct behaviour.

**The atomicity rule that applies to every mechanism, including a hypothetical `NewGlobalRef`:**
the read-oop → store-oop sequence must not straddle a relocating GC. It does not, and only does
not, when the store executes on a real `JavaThread` in `_thread_in_Java` that executes no
safepoint poll — i.e. inside a vmhook interpreter-entry detour. **Pinning from a cold native
thread is unsound no matter what mechanism is used.** This is a hard constraint on the API, not
an implementation detail.

### 4.2c Ship the detector first

Before any of that, a **relocation detector** should land, because it is strictly better than
today's stub and costs almost nothing. `Universe::_collectedHeap` (static) +
`CollectedHeap::_total_collections` (nonstatic) are exported on **every** JDK 8..26 and the
header already reaches the former (line 9126). Sample the counter at capture, re-check it on
access: **today's silent use-after-relocation UB becomes a detectable, safe `nullptr`.**
No barrier, no root, no race, ~40 lines.

This is mechanism **E**, and it is the safety net under D and P both.

**Soundness is verified at the source level, not assumed.** The increment call sites were read
in OpenJDK: G1 young collections bump `_total_collections` in `pre_evacuate_collection_set`,
**before any copying**; likewise Serial, Parallel young/full, and JDK 8's explicit sites. **No
Serial/Parallel/G1 relocating path skips it.** Sample the *pair* `(_total_collections,
_is_gc_active)`; the rename to `_is_stw_gc_active` is at **JDK 21** (not 23 — probe both names,
never key off a version number). That flag is set by the RAII `IsGCActiveMark`, so it is true
only inside a stop-the-world GC.

**ZGC and Shenandoah must be refused outright, and the detector is the reason why**: their
counters tick at *cycle start*, phases before relocation, so an unchanged epoch is meaningless
there — the detector itself would become silent UB. Refusing is not conservatism, it is the
only correct behaviour.

Prior art check: **nil**. async-profiler, JOL, jnihook, the Minecraft-internal tooling cluster
and Cheat Engine all fall back to JNI or JVMTI for this. If mechanism P lands, it is new.

### 4.3 The type vocabulary

```cpp
namespace vmhook
{
    template<class T> class ref;          // rooted, revalidating on every ->, copyable, hashable
    template<class T> class borrowed;     // detour-scoped, no root allocated; .pin() promotes
    template<class T> class weak_ref;     // observes collection
    template<class T> class ref_vector;   // rooted container, built pin-during-walk
    template<class K, class V> class ref_map_view;
    template<class T> class root;         // revalidating handle onto a static field / singleton
    class object_id;                      // stable, hashable, relocation-proof identity
}
```

`ref<T>::operator->` returns an internal `access<T>` proxy that re-reads the root, materialises
a `T` bound to the *current* address, and lives exactly as long as the full expression. The
revalidation is the whole safety argument, and it is invisible.

Wrapper authoring gets simpler, and the existing `object<T>` inheritance model — the part that
works, with 32 wrapper classes in npnoqol — is kept:

```cpp
class entity_player : public vmhook::object<entity_player>
{
public:
    using vmhook::object<entity_player>::object;

    auto pos_x()     const -> double      { return field<double>("posX"); }
    auto name()      const -> std::string { return call<std::string>("getName"); }
    auto inventory() const -> vmhook::ref<inventory_player> { return field<vmhook::ref<inventory_player>>("inventory"); }
};
```

### 4.4 Design axioms

1. The consumer never sees an address.
2. A reference revalidates on every dereference, inside the library.
3. Rooting is automatic and total — no unrooted intermediate exists to lose.
4. Containers of Java objects are rooted during the walk, or they do not exist. The
   walk-then-pin shape is *unexpressible*.
5. Identity is stable, hashable, relocation-proof — usable as a map key.
6. Liveness is a cheap first-class state, distinct from null.
7. Handles are thread-agnostic: creatable, copyable and destructible on any thread.
8. Lookup failure is a value, never an unchecked `optional` deref.
9. **The rooting mechanism is invisible policy.**

**Never dangle** overrides all of them. A handle reporting "expired" is acceptable; a handle
returning a stale address is not. That is exactly the bug we are removing, and no amount of
convenience justifies reintroducing it.

---

## 5. Phases

Each phase is independently landable and locally validated. **GitHub CI is checked once, at
the very end.**

### Phase 0 — back to green *(in progress)*
- **0.1** ✅ **DONE** — removed the 3 CI-breaking dead fields, plus 2 provably-dead siblings
  from the same JNI-era cache family (`cached_effective_signature`, `cached_keyed_signature`;
  clang skips non-trivial types, which is why CI only named three). Verified positively by
  reproducing the exact 3 CI errors against a scratch copy with the fields re-inserted, then
  confirming silence against the fixed header.
- **0.2** ✅ **DONE** — 11 of the 12 orphaned test files repaired and re-registered.
  **86 → 97 tests, 100% pass on MinGW `-Werror` and MSVC**, ~1,912 runtime assertions and
  ~1,168 `static_assert`s restored. The 12th (`test_jni_local_ref_hygiene_nojvm.cpp`) tests only
  a deleted JNI forwarder and is recommended for deletion.

  Root cause: a single commit, `eaff990` *"test(nojvm): adapt the no-JVM lane to the JNI-free
  header"*, deliberately deferred them "for later salvage" — and the salvage never happened. It
  also silently dropped `find_class_contracts`, which its message never mentions, and removed
  three registrations while leaving their explanatory comments behind, which is what made the
  gap invisible.

  **Worse finding:** that commit's mechanical `jni::find_class` → `vmhook::find_class` rewrite
  left **self-contradictory and outright false assertions** across five files — the same
  expression asserted both `noexcept` and `!noexcept`, both returning `void*` and `klass*`, plus
  several `X == X` tautologies. Because the files were unregistered, nobody ever saw them fail.
  `test_global_ref.cpp` was worse still: its entire premise was *"with no JVM, `NewGlobalRef`
  cannot run, so every holder is empty"* — ~60 assertions **vacuously passing against a contract
  that no longer exists**. It has been rewritten around real value-transfer assertions, and
  carries a deliberate tripwire (`static_assert(is_trivially_destructible_v<global_ref>)`) that
  will fail the day a real releasing destructor lands, pointing the implementer back at the
  missing GC-survival coverage.
- **0.3** ✅ **DONE** — `jni|JNI` hit count **128 → 23**, all 23 survivors individually
  justified (5 accurate product claims, 7 references to the still-misnamed
  `detail::jni_signature_for_arg`, 1 deliberate signpost, 10 references to the real
  `vmhook::jni::global_ref` type). All 11 self-contradicting blocks fixed **by deletion**, ~40
  smaller stale comments corrected, 6 user-visible diagnostic strings reworded, and
  `global_ref`'s doc rewritten to open with the truth: non-owning, not a GC root, the object may
  be *collected*, the address goes stale across a relocating GC, do not carry it across ticks or
  threads. `find_class_via_oop` and `method_proxy::call` now document what they actually do.
  Every `call_jni`, `jni_make_unique`, `allow_jni_fallback`, `JNIEnv`, `NewGlobalRef`,
  `DeleteLocalRef`, `current_jni_env`, `jni_value` mention: **0 hits**.
- **0.4** Fix B3 (MSVC C4127 ×3 in `test_iterate_entries_safety.cpp`). Two of the three are
  `if (sizeof(void*) == 8u)` at 1617 and 2516 — `if constexpr` is the honest fix. The third
  (line 574, `if (got < 0 || got > k_cap)`) is **not** obviously constant and needs an actual
  MSVC run to pin down; do not guess at it. Local-only, invisible to CI — lowest priority.
- **0.5** Diagnose B2's 284 JVM failures. Hypothesis: they are §3.3, not 284 separate bugs.

### Phase 1 — the safety floor *(nothing here needs an open question answered)*
The lifetime work stages into four independently-shippable layers:

| Layer | What | Coverage | Risk | Ship |
|---|---|---|---|---|
| **0** | **Capability gate** — collector, JDK barrier shape, compressed oops | all | none (reads only) | **first**, everything needs it |
| **1** | **Relocation detector** — `global_ref` invalidates instead of dangling | all JDKs, all collectors | none | **first release** |
| **2** | **Pin via a C-heap root slot** (CLD handle list / JDK 8 global handle block) | JDK 8-26, Serial/Parallel/G1 | medium (lock race, offset discovery) | opt-in, after CI |
| **3** | Bulk `Object[]` handle table in a static field | Serial/Parallel any JDK; **G1 26+ only** | **high on G1 ≤25 — do not** | last, collector-gated |

- **1.0** **Layer 0, the capability gate.** Determine the collector by walking the JVM flag
  table (`UseSerialGC`/`UseG1GC`/`UseZGC`/…), which is exported on every JDK 8-26. Two rules
  make it robust: take the array stride from `gHotSpotVMTypes["JVMFlag"].size`, **never
  `sizeof`** (the `_doc` member exists only in non-product builds), and branch on the exported
  `typeString` of `_type` rather than sniffing a JDK version — the table tells you its own
  shape. Walk `numFlags - 1`; the last entry is an all-null sentinel. This is what the
  Serviceability Agent does.
  **Do not** infer the collector from VM_TYPES presence (`declare_type(ZCollectedHeap,…)` is a
  build-time `INCLUDE_ZGC` guard, present even when ZGC is not running) or from vtable symbols
  (not exported from `jvm.dll`). Fallbacks: `HeapRegion::GrainBytes` (zero until G1 initialises),
  then `sun.gc.collector.0.name` from PerfMemory.
  Also read `UseCompressedOops` here — **the flag table is the only reliable source** (`_base ==
  0 && _shift == 0` is genuinely ambiguous between "off" and "on with an unscaled sub-4 GB
  heap"; `heapOopSize` is exported nowhere). This is the proper fix for **L2**. Read
  `UseCompactObjectHeaders` too (JDK 25+, **default true on master**) — it changes the header
  and klass encoding this library decodes.
- **1.1** **Layer 1, the relocation detector** (mechanism **E**, §4.2c). ~40 lines. Turns
  silent use-after-relocation into a safe `nullptr`. `global_ref` records the epoch alongside
  the oop; `oop()` returns `nullptr` when stale; add `is_stale()`. Land it first; it makes every
  later phase safe to build on.
- **1.2** **`dirty_card()` + oop-width detection.** Fixes latent bugs **L1** and **L2**, which
  are shipping heap-corruption risks today — *and* they are exactly the two primitives
  mechanism **P** needs. One piece of work, two payoffs.
- **1.3** **Refuse ZGC/Shenandoah loudly** at init. Direct-memory reads are invalid under
  concurrent relocation with load barriers; silently producing garbage there is not acceptable.
- **1.4** Fix **L3** (cross-thread TLAB race in `make_java_object`).
- **1.5** ✅ **RESOLVED by measurement** — see §3.3. Invocation is derivable and cheap.

### Phase 1b — restore method invocation ✅ **DONE** *(commit `3b7f8e7`)*

All seven items implemented and **proven on live JVMs: 29/29 on Temurin 8.0.492+9, Temurin
21.0.11+10 and Oracle 26.0.1**, driving the real `find_call_stub_entry()` and `call()` from the
edited header. Coverage included object args and returns, 2-slot long/double, an `int` widened
into a `J` parameter, a `native` callee with `_top` preserved, a throwing callee classified as
`java/lang/NumberFormatException` and cleared, `System.gc()` through the synthetic entry frame,
20 000 invocations with a stable handle block, and a real vmhook hook on a cold `String` method
with a **nested `call()` plus a full GC inside the detour** (two stacked synthetic entry frames).
Before/after control on the same three JVMs: `HEAD` resolves `0x0` on all three.

Two live-JVM findings that came out of that work and belong to later phases:

- **Raw oops are still not GC roots.** The harness crashed on JDK 21 holding a receiver oop
  across a young GC in a 20 000-call loop, until it re-read the oop each iteration. Nothing in
  `call()` can fix that. This is direct empirical confirmation that the lifetime work
  (Phase 1/2) is the real blocker, not a nicety.
- **`make_java_string` fails on a freshly-booted VM** on all three JDKs — the `[B` array klass
  reports "not loaded", then TLAB allocation fails. Unrelated to invocation, but it makes string
  arguments unavailable in early-boot contexts. Not yet diagnosed.

Not done from the research plan: an explicit opt-in for the `_thread_in_native` flip (the state
is gated but the flip is implicit), the detour-hosted MPSC executor for non-JavaThread callers,
and a public `invocation_capability()` reporting which tier resolved the stub. The tiers are
x86-64-only by construction; aarch64 needs its own prologue pattern, and the non-Windows SysV
`FF D6` variant is inferred, not measured.

**Worth adding to CI:** assert `find_call_stub_entry() != nullptr` on every JDK in the matrix.
It returned null on all of them for years and nothing noticed.

<details><summary>Original plan (all items completed)</summary>

- **1b.1** Tiered `find_call_stub_entry()`: read `StubRoutines::_call_stub_return_address`,
  derive the entry (`±8` adjacency, then a data scan), **validate positively** by prologue
  signature and the `FF D2` before RA, else return `nullptr` as today. ~70 lines, purely
  additive.
- **1b.2** Fix **L5** — replace the `link = -1` argument with a synthetic 64-byte
  `JavaCallWrapper` (`_anchor` at offset 32, VMStructs-confirmed on all three). This one is a
  live VM-corrupter, not a nicety.
- **1b.3** Fix **L6** — long/double take two slots, value in the higher slot.
- **1b.4** Fix **L7** — save/restore `_active_handles->_top` around native-method calls.
- **1b.5** Clear pending exceptions via `ThreadShadow::_pending_exception` (offset 8), and
  delete the comment claiming this needs JNI.
- **1b.6** Fix **L8** and correct the two comments claiming "JDK 21+ dropped `_call_stub_entry`".
- **1b.7** Re-run the JVM suite and confirm the 284 failures collapse.

</details>

### Phase 2 — the reference core
- **2.1** ✅ **DONE** *(commit `eb8e2b8`)* — `object_id`, `ref<T>`, `borrowed<T>`, `root<T>`,
  `ref_vector<T>` with mechanism **D**, backed by the **E** detector. Additive: nothing existing
  changed signature.

  **A design correction the live JVM forced.** This roadmap specified an epoch-keyed address
  memo. That is wrong, and the first live run proved it: a static field can be **overwritten
  without any collection**. Replace `Minecraft.theWorld` on a world reload and a memoised ref
  keeps returning the previous World — live, valid, and wrong — until an unrelated GC happens
  to clear it. That is precisely the silent-staleness class this model exists to delete. **A
  `ref` now stores no address at all**: it names a *slot*, `resolve()` is a pure function of
  live VM state, and consequently a `ref` has no mutable state, so one can be dereferenced
  concurrently from any number of threads with no synchronisation. Cost 1.30-1.64 µs/resolve
  vs 0.57 with the memo — the right trade.

  Axiom A4 is enforced by the type system, not by documentation: `ref_vector` is
  `static_assert`-proven **not constructible** from `oop_t`, `void*`, `vector<void*>` or even
  `vector<ref<T>>`, so the walk-then-pin shape is unexpressible. `detail::access<T>` has
  deleted copy *and* move, making "bound only for this expression" a compile-time fact.

  Proven on live JDK 8 (Parallel), 21 (G1), 26 (G1) + Serial/Parallel on 21: **46/46**.
  Objects verified to physically move against a JNI oracle, then re-resolved to the **new**
  address — including 2-hop chains where both objects relocated, and a 256-element array where
  the array and every element moved (256/256 resolved).
- **2.2** `ref_map_view<K,V>` — **not done.** Anchoring a HashMap/TreeMap *node* needs the
  collection walkers to hand back `(holder, offset)` pairs rather than decoded oops.
  `ref_vector` + `elements_of` covers the array-backed case today.
- **2.3** `field<R>()` / `call<R>()` on `object<T>`; `try_field` / `try_call` returning
  `std::expected`. `operator bool` and `operator==` on `object<T>`. **Not done.**
- **2.4** Mechanism **P** (a real pin) behind the same API, for path-less objects. Requires 1.2,
  plus the anchor policy and the no-safepoint window. **Pin acquisition is only sound inside a
  detour** (§4.2b) — the API must enforce that, not document it. **Not done.**
- **2.5** `weak_ref<T>` — **not done**, needs a real GC root.
- **2.6** `detail::extract_frame_arg` — **DONE.** The detour-argument choke point now accepts
  `vmhook::borrowed<W>`, so a detour can declare its receiver and any object argument as a
  lifetime-checked handle instead of a raw address. Three tables had to agree and all three
  were wired: `extract_frame_arg` (produces the handle), `jni_signature_for_arg`
  (`borrowed<W>` -> `Lclass;`, `borrowed<void>` -> `Ljava/lang/Object;`), and
  `is_java_double_slot_v` (one slot — the failure mode a wrong width causes is silent, so it is
  pinned at compile time against argument lists that interleave borrows with long/double).
  A null slot yields an EMPTY borrow, never an expired one — "there was no object" and "the
  object moved" stay distinguishable.

  Found while adding it: `extract_frame_arg` called `frame->get_locals()` with no null check.
  `get_locals()` survives it (it gates on `is_valid_pointer(this)`), but the member call on a
  null pointer is already UB by then, and GCC diagnoses exactly that under `-Wnonnull` as soon
  as a caller can be seen passing null. Guarded at the choke point.

  Coverage: `tests/test_borrowed_detour_arg_nojvm.cpp` (traits, descriptor, slot table,
  null-frame degradation) and `tests/jvm/modules/borrowed_detour_arg.cpp` (live receiver
  identity across two instances, borrowed object arguments, Java-null arguments, and the
  slot table behind `combine(int,long,int)` and the 8-arg `manyArgs`). The live module drives
  `HookBasic` UNCHANGED, on the same modes hook_basic drives through `unique_ptr` — so the two
  argument models are asserted against the same scenarios.

Placement: top-level `namespace vmhook`, just above the field-proxy section (~13433) — every
primitive it needs (`decode/encode_oop_pointer`, `safe_read/write`, `get_java_mirror`) is
complete by then and all consumers follow. **Do not redefine `oop_t` as a class**:
`cast_for_variant` has an `is_same_v<target, void*>` branch that a class type silently
disables.

### Phase 3 — retire the raw-oop surface — **ALL SIX INTERCEPTS DONE**
80 raw-oop boundary crossings are catalogued in 7 categories. The **minimum viable intercept
set** covering ~90% of user exposure is 6 places. Every one now has a handle form:

| # | Intercept | Handle form |
|---|---|---|
| 1 | every detour argument | `detail::extract_frame_arg` accepts `borrowed<W>` |
| 2 | `object_base` ctor + `get_instance` | `object<W>::self()` → `borrowed<W>` |
| 3 | `field_proxy` read + write | `value_t::to_borrowed<W>()` · `store_object(borrowed<W>)` |
| 4 | `method_proxy` result | `value_t::to_borrowed<W>()` |
| 5 | the six collection ctors | each takes `const borrowed<W>&` |
| 6 | `make_java_*` | `new_object` / `new_array` / `new_string` return `borrowed<W>` |

Two of these are more than ergonomics:

- **`store_object`** resolves the handle immediately before the write and REFUSES an expired
  one. A raw `store_object_oop(addr)` cannot tell whether `addr` survived the gap between the
  caller reading it and the store; if it did not, the field ends up holding a pointer into
  relocated space and the corruption surfaces arbitrarily later. This closes that window as
  far as a pure-VM build can. What it cannot close is a collection landing between the resolve
  and the `safe_write` a few instructions later — that needs Layer 2.
- **`new_*`** exists because a fresh address is the most dangerous shape in the API: it *looks*
  trustworthy and is completely unrooted, so the next allocation can move it. The handle at
  least reports EXPIRED afterwards instead of reading as valid.

Throughout: an EMPTY handle (Java null, or a failed allocation) is never an EXPIRED one. The
caller's recovery differs, so the two states stay distinct at every intercept.

Raw accessors survive as explicitly-named escape hatches (`get_instance`, `make_java_*`,
`store_object_oop`), not as the default. The header itself still uses the raw forms internally,
where the address is consumed immediately inside a documented no-safepoint window.

### Phase 4 — finish Goal A
- **4.1 DONE** `vmhook::jni::global_ref` → `vmhook::oop_pin` at `vmhook` scope; the `jni`
  namespace is **deleted**, no alias. Both halves of the old name were wrong: there is no JNI
  in the header, and the type was never a global reference in the JNI sense — it registers
  nothing with the VM. A `pin(borrowed<W>)` overload bridges the handle model back to an
  address for code that still needs one, resolving first so an expired handle cannot be pinned.
- **4.2 DONE** `detail::jni_signature_for_arg` → `jvm_descriptor_for_arg`, 711 references
  across 14 files, one mechanical pass.
- **4.3 DONE** (earlier session) Method invocation restored pure-VM.
- **4.4 NOT DONE** Rebuild `viewer/payload` on a **detour pump**: stop entering Java from
  native; hook a method the JVM already calls and let the detour drain a native work queue.
  Thread promotion, detour triggering and allocation all dissolve — they run on a real
  JavaThread with a valid anchor and TLAB. Then delete `<jni.h>`.
  **This is the last real JNI in the project.** Two of its three JNI uses are now trivially
  replaceable (`NewStringUTF` → `make_java_string`; `invoke_jni`'s ~120 lines →
  `method_proxy::call()`, which works now); only thread promotion needs the pump.
- **4.5** Fix the two regressions currently hidden by their own docs: `find_class_via_oop` no
  longer disambiguates by classloader, and `call()` leaves thrown exceptions pending on the
  JavaThread. *(The second is stale — `call()` clears `ThreadShadow::_pending_exception` and
  reports the throw through `value_t::threw()`.)*
- **4.6 PARTIAL** Test-suite naming and prose. Done: the `method_call_jni_fallback` module and
  its `MethodCallJni` fixture → `method_call_dispatch` / `MethodCallDispatch`;
  `jni_local_ref_hygiene` / `JniLocalRef` → `repeat_call_stability` / `RepeatCallProbe`; both
  fixture doc blocks rewritten to say what they now prove and why the loops were kept.
  **Not done:** ~200 further `call_jni` / "JNI fallback" / "JNI local ref" mentions in test
  comments across ~50 files. These are prose, not names — each needs a real edit, not a sed,
  because a blind substitution turns explanations into nonsense. Two more symbols named in
  comments no longer exist at all: `jni::find_class_with_context_loader` and
  `jni_delete_local_ref`.

### Phase 4b — modern C++ in the header

Requested 2026-08-05: use current language features to improve `vmhook.hpp`. Before this the
header used essentially none — **zero** `[[nodiscard]]`, `std::expected`, `std::span`,
`std::bit_cast`, `consteval`, or concepts; the only C++20+ feature in use was `requires`.

**Landed:**

- **`VMHOOK_HAS_REFLECTION`** (P2996 + P3394 annotations). Gated exactly like
  `VMHOOK_HAS_DEDUCING_THIS`: every use is additive with a C++23 fallback that behaves the
  same, only with a worse diagnostic. Shipping reality — Clang 21+ has it behind
  `-freflection`, GCC has not merged it, MSVC has not shipped it, and vmhook's CI is
  `-std=c++23` on all three, so nothing may depend on it.
  - `detail::type_name<T>()` — `std::meta::display_string_of` when available, `typeid().name()`
    otherwise. **Wired into all 33 diagnostics that previously emitted mangled names.** A user
    who forgot `register_class<player>()` was being told about `"6playerE"` and had to read
    Itanium mangling to understand their own error. MSVC happened to return `"class player"`,
    which is why this survived so long — it was invisible to anyone testing only on Windows.
  - **`vmhook::java_class` annotation** + a no-string `register_class<T>()`. The name travels
    with the type instead of being a runtime argument, which kills a real silent bug: a
    copy-pasted `register_class<item>("com/example/Player")` binds `item` to Player's klass and
    every field read after it is nonsense at a plausible offset.
  - `jvm_descriptor_for_arg` **short-circuits on the annotation at compile time**, so an
    annotated wrapper's descriptor is a constant that cannot disagree with the registry and can
    never degrade to `Ljava/lang/Object;` because someone forgot to register.

- **`std::expected` (C++23)** — roadmap 2.3's `try_*`, previously "not done".
  `object_base::try_field` / `try_method` return `std::expected<T, access_error>`.
  The point is not style: `get_field` collapses four different causes into one empty optional,
  and those four want different responses — "not loaded yet" is retryable, "not registered" is
  a setup bug, "no such member" is a typo or an obfuscated rename, "null receiver" is ordinary
  Java. Conflating them is *why* the house one-liner `get_field("x")->get()` became common and
  why it has taken the whole JVM suite down. `try_*` deliberately does not log — the caller has
  the reason in hand, and probing whether a class is loaded yet is a legitimate loop.

- **`[[nodiscard]]`** on the API added this session.

**Not done, deliberately:** a blanket `[[nodiscard]]` sweep over the existing surface. Every
existing call site that legitimately discards a result would become a `-Werror` failure, and
that is not a change to make without a compiler. Same for replacing the `static_assert(
is_base_of_v<object_base, T>)` pattern with concepts — better diagnostics, but it touches
overload resolution at existing call sites.

### Phase 5 — prove it
- **5.1** Un-gate `tests/jvm/modules/global_ref.cpp` from `[INFO]` to `HARD`: force a
  relocating GC under heap pressure, assert the address **changed** *and* the handle still
  resolves to the right object.
- **5.2** New JVM module for anchored refs: survive N forced GCs, survive class reload,
  cross-thread handoff, container-of-refs.
- **5.3** Port npnoqol's 8 acceptance functions and check the targets:

  | ID | Function | Today | Target |
  |---|---|---|---|
  | A | `flag_manager` 16-field read | 21 lines, 17 re-reads | ≤12 lines, **0** re-reads |
  | B | `build_network_player_info` | 64 lines, 2 pins, 2 latent bugs | ≤12 lines, 0 pins |
  | C | `update_tab_list` | 125 lines, ~40 lifetime bookkeeping | ≤60 lines, 0 raw addresses |
  | D | `item_stack::classify` | 37 lines, 1 pin, 6 `.oop()` | ≤14 lines, 0 pins |
  | E | `chat::add_chat_message` | 35 lines + 8-line ordering comment | ≤8 lines, order-independent |
  | F | root re-resolution per tick | `theWorld` ×4, `thePlayer` ×3 | ×1 each |
  | G | `feature::minecraft_accessor` | custom `operator->`, allocates per access | `vmhook::root<sdk::minecraft>` |
  | H | whole-codebase grep | `void*` 39 · `.oop()` 67 · `get_instance()` 95 · `global_ref` 46 | **0 · 0 · 0 · 0** |

- **5.4** Migration guide + deprecation shims.
- **5.5** **Then, and only then, look at GitHub CI.**

---

## 6. Acceptance criteria

| # | Criterion |
|---|---|
| A1 | Zero JNI types, calls, includes, and public `jni`/`jni_*` names in `vmhook.hpp` |
| A2 | Nothing in the repo requires `jni.h` to build — including `viewer/payload` |
| A3 | Every feature that worked before still works; method invocation works on JDK 8..26 |
| A4 | Comments describe what the code does; no stale JNI prose |
| A5 | No public API takes or returns a raw heap address except explicit `raw_*` escape hatches |
| A6 | A handle held across a forced relocating GC resolves to the object's **new** address, or reports expired — never a stale address |
| A7 | npnoqol acceptance table H reaches `0 · 0 · 0 · 0` |
| A8 | Existing consumers migrate without a rewrite (shims + guide) |
| A9 | Full GitHub CI green — B1..B4 fixed, 284 JVM failures resolved |

---

## 7. Validation without watching CI

Local gates, fastest first:

| Gate | Command | Measured |
|---|---|---|
| Header compile (MinGW) | `g++ -std=c++23 -Wall -Wextra -Wpedantic -Werror -c -Ivmhook/ext` | **5.0 s** |
| Header compile (MSVC) | `cl /std:c++latest /W4 /WX /wd4505 /wd4101` after `vcvars64.bat` | **3.5 s** |
| Full MinGW build + ctest | `-DVMHOOK_WARNINGS_AS_ERRORS=ON` | **2m11 s** + 86/86 in 3.5 s |
| Local JDK matrix | `.localci/run-local-ci.ps1` — 3 compilers × 7 JDKs, all cached | longer |

`/wd4505 /wd4101` are **mandatory** for MSVC or it fails instantly on the header's by-design
internal-linkage functions. Use `clang++` (GNU driver), **not `clang-cl`** — clang-cl under
`/WX` fails on `-Wunneeded-internal-declaration` because `/wd4505` does not map; that
configuration was never supported.

**The clang blind spot is now closed.** Local clang on Windows uses the MSVC ABI and cannot
compile the header (the installed MSVC STL needs clang ≥ 20), which is why
`-Wunused-private-field` appeared to emit nothing and how B1 reached master. **Retargeting
clang at the MinGW sysroot reproduces the Linux CI compiler exactly:**

```
clang++ --target=x86_64-w64-windows-gnu --sysroot=C:/msys64/mingw64 -std=c++23 \
        -Wall -Wextra -Wno-unneeded-internal-declaration -Werror -c -Ivmhook/ext
```

clang lives at `…/BuildTools/VC/Tools/Llvm/x64/bin/clang++.exe` (19.1.5) and is not on PATH.

Verified both ways: against a scratch copy with the three dead fields re-inserted it emitted
**exactly the three CI errors**; against the fixed header it exits 0. That is positive
verification, not absence of evidence. **Add this to the standard pre-push gate.**

`-Wno-unneeded-internal-declaration` is required *for a minimal one-line TU only*: three
`static` helpers (`find_hook_location`, `common_detour`, `ensure_started`) are genuinely unused
when nothing references them, and clang reports that. CI's real translation units use them, so
this is a minimal-TU artifact, not a suppressed defect. It is also why `clang-cl /WX` fails —
`/wd4505` does not map to it — a configuration the project never supported.

**Remaining blind spots — do not mistake local green for CI green:**
- MSVC `/WX` is never run on Windows in CI, so B3 is invisible there and blocks locally.
- `jvm · linux` / `jvm · macos` are best-effort and exit 0 on missing results.
- **Never use `.localci -NoBuild` as a pre-push gate** — it silently reuses a stale DLL with no
  staleness check (observed reusing a 6-week-old build and reporting a bogus result).
- The `global_ref` JVM module's entire GC-survival phase is `#if !defined(_WIN32)`, so it runs
  on **zero** hard-gated cells today. Fixing that is part of Phase 5.1.

> ⚠ **Do not commit the working tree's `audit/` deletions.** 249 of them are under
> `audit/features/` and `audit/graph/`, which is exactly the path filter for the `registry.yml`
> workflow — committing them triggers it, and `validate.py` will no longer exist, turning it
> red. Research notes belong in `docs/research/`.

---

## 8. Open decisions

| # | Decision | Blocks | Status |
|---|---|---|---|
| 1 | Is `_call_stub_entry` gone, renamed, or derivable on JDK 21/26? | Phase 4.3 scope: 1 day vs weeks | ⏳ measuring on live JDK 8/21/26 |
| 2 | Is a pure-memory GC root achievable? | path-less object lifetime | ✅ **yes** — mechanism P |
| 3 | Can reference stores honour the GC's barriers without a VM call? | Phase 1.2, L1, and P | ⏳ card-table detail landing |
| 4 | Which Java class's static field anchors the handle table? | Phase 2.4 | open — the one hard policy call in P |
| 5 | `ref<T>` refcount granularity — one root per copy, or a shared control block? | perf; npnoqol copies ~80 refs/tick | open |
| 6 | `object_id` derivation — slot address, `identityHashCode`, or a library identity map? | map-key semantics only are needed | open |

**Nothing here blocks Phase 1 or Phase 2.1-2.3.** Mechanism D carries the design and the E
detector makes it safe; P slots in behind an unchanged API.

---

## 9. Working rules

- **Do not watch CI.** Validate locally; check GitHub once, at the end (5.5).
- Ignore the deleted `.claude/` and `audit/` paths in the working tree; never stage them.
- Land coherent, individually-validated commits.
- Never raw-deref a HotSpot pointer — MinGW and clang-on-Windows have no working SEH, so an AV
  inside a detour kills the JVM. Every cold read goes through `os::safe_read`.
