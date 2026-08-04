# Can vmhook obtain a real GC root without JNI? — feasibility research

**Scope:** `vmhook/ext/vmhook/vmhook.hpp` v0.5.3 (19 824 lines), pure-VM build.
**Question:** is there ANY way, using only memory reads/writes plus data reachable through
`gHotSpotVMStructs`, to obtain a storage slot for an oop that the garbage collector will
(a) treat as a strong root and (b) update when it relocates the object — on HotSpot
JDK 8…26, collectors Serial / Parallel / G1?
**Date:** 2026-08-04 · **Mode:** read-only research, no source edited.
**Supersedes:** the "PROVEN infeasible-without-JNI" verdict in
`C:\repos\cpp\npnoqol\.claude\knowledge\vmhook-migration.md` (2026-07-22), which examined
exactly one mechanism (calling / hand-rolling `OopStorage::allocate`).

---

## 0. Verdict

**Yes — a real, relocation-tracking, 0-JNI pin is achievable on JDK 8…26 for Serial,
Parallel and G1, and the prior "infeasible" verdict was too narrow.** It is infeasible only
for the mechanism that was examined (hand-rolling `OopStorage::allocate`, which is
confirmed unsafe here for four independent reasons, §5). Two mechanisms survive scrutiny:

* **(A) — RECOMMENDED — a VM-native handle-list append into a root list that lives *outside*
  the Java heap**: `ClassLoaderData::_handles` (`ChunkedHandleList`, JDK 11…26) or
  `JNIHandles::_global_handles` (`JNIHandleBlock` chain, JDK 8). Both are strong, GC-updated
  roots, and both have a publication protocol that is *pure memory writes* in the VM itself
  (`_data[_size] = oop; release_store(_size + 1)` / `_handles[_top] = oop; _top++`).
  **Decisively: because the slots are C-heap and roots are enumerated in full at every
  collection, no write barrier of any kind is needed** — which is what makes this the only
  mechanism that works on G1. Costs: `ClassLoaderData::_handles` is **not** exported by
  VMStructs on any JDK, so its offset must be recovered by a structural signature scan; the
  append races the `metaspace_lock` / `JNIGlobalHandle_lock` we cannot take; and capacity is
  **opportunistic and small** — we may only append to a non-full chunk/block, because
  allocating a new one is a VM-owned C-heap allocation we must not perform. Pin once, reuse.
* **(B) The Java-object-rooted handle table — good, but NOT on G1 before JDK 26.** Allocate
  an `Object[]` from a TLAB (vmhook already does this), store it into a *static reference
  field of a Java class* via that class's `java.lang.Class` mirror, and address every pin as
  `mirror(klass) → handle_array → element[i]`. Every hop is GC-traced and GC-updated, and
  vmhook already implements the first two (`get_java_mirror`, `field_proxy::store_object_oop`
  with its GC-stable mirror re-resolution). It gives unlimited capacity. But every store is
  a reference store *into a Java object*, which needs the post-barrier — and **on G1 JDK
  8…25 the card-table post-barrier cannot be emulated by memory writes at all** (§6.3): the
  card must also be enqueued into a thread-local dirty-card queue that is not reachable from
  VMStructs, and writing the card byte alone is *actively harmful* — it permanently
  suppresses the JVM's own barrier for that 512-byte card. So (B) is legal on **Serial and
  Parallel (all JDKs, one unconditional byte)** and on **G1 JDK 26+** (JEP 522 removed the
  queue; the real barrier is now literally the byte write), and must be **refused** on G1
  8…25. Since G1 is the default collector across most of the matrix, (B) is the bulk
  optimisation, not the base mechanism.

Both are subject to one hazard that no external mechanism can remove by itself: the
**read-oop → store-oop sequence must be atomic with respect to a relocating GC**. It is,
and only is, when the store executes on a real `JavaThread` whose `_thread_state` is
`_thread_in_Java` and which executes no safepoint poll — which is exactly the situation
inside a vmhook interpreter-entry detour (§7). Outside that window the pin operation itself
is unsound, for any mechanism, including a hypothetical `NewGlobalRef` on a raw oop.

**Coverage:** Serial, Parallel, G1 on JDK 8…26 via (A); (B) adds unlimited-capacity pinning
on Serial/Parallel any JDK and on G1 26+. **ZGC and Shenandoah are out of scope** and must be
*detected and refused*: they relocate concurrently behind load barriers — the copy can even
be performed by the reading thread itself from a load-barrier slow path — so a raw oop read
by native code is stale before it is stored, their from-space is recycled early enough that a
stale read silently returns another object's bytes, and neither `_total_collections` nor the
jstat counters tick per relocation, so even the *detector* is unsound there. vmhook's whole
direct-memory model, not just the pin, is invalid on those collectors.

**Fallback that should ship regardless, and first:** a **relocation *detector***.
`Universe::_collectedHeap` (static) + `CollectedHeap::_total_collections` (nonstatic) are
exported on **every** JDK 8…26 and are already used by this header
(`for_each_instance`, line 9126). Sampling that counter at pin time and re-checking it in
`oop()` converts today's silent use-after-relocation UB into a detectable, safe
`nullptr`. That is a ~40-line change with no barrier, no root, and no race, and it is
strictly better than the current no-op stub. See §8.

---

## 1. What vmhook already has (the starting inventory)

| Capability | Where | Note |
|---|---|---|
| VMStructs offset/static lookup | `iterate_struct_entries`, `vm_struct_entry_t` (1923, 2022) | `{type_name, field_name, type_string, is_static, offset, address}` |
| Fault-safe read/write | `os::safe_read` / `os::safe_write` (988, 1081) | `ReadProcessMemory` / `process_vm_readv` / `mach_vm_*` |
| `Klass::_java_mirror` deref, incl. the `OopHandle` double-indirection | `klass::get_java_mirror` (3711) | already keys off `entry->type_string == "OopHandle"` |
| Static-field addressing = `mirror + offset`, **re-resolved through a GC-stable `mirror_klass` at every access** | `field_proxy` (14135, 14432, 14546, 14634) | this is already the §4.1 chain, minus the barrier |
| Raw oop store into a Java field | `field_proxy::store_object_oop` (14621) | **no write barrier** — see §11 |
| Compressed-oop encode/decode | `hotspot::{en,de}code_oop_pointer` (5634, 5680) | `CompressedOops`/`Universe` `_narrow_oop._base/_shift`, plus the JDK 24+ flattened names |
| TLAB bump allocation of a Java object/array/string | `java_thread::allocate_tlab` (5189), `make_java_object` (12910), `make_java_array` (13034) | header stamped from `Klass::get_prototype_header()` + encoded klass |
| `Object[]` klass resolution | `resolve_array_klass` (8556) → `InstanceKlass::_array_klasses` of `java/lang/Object` | `find_class("[Ljava/lang/Object;")` works today |
| Thread state read/write | `java_thread::{get,set}_thread_state` (4956, 4997) | `JavaThread::_thread_state`; already used around the call-stub path (15255) |
| Heap bounds | `Universe::_collectedHeap` → `CollectedHeap::_reserved` (9126) | proven on the JDK 8…26 CI matrix |
| Class re-anchoring after a mirror moves | `reanchor_classes_via_oop` (12574) | the "re-derive instead of pin" pattern already exists |

So the *only* genuinely new primitives the recommended design needs are: a card-table
write, a safepoint-safety gate, and an anchor policy.

---

## 2. Method and sources

All VMStructs quotes below were taken from the actual `vmStructs.cpp` /
`vmStructs_gc.hpp` / `vmStructs_g1.hpp` at these tags: `jdk8u-dev@master`,
`openjdk/jdk@jdk-11-ga`, `jdk-12-ga`…`jdk-16-ga`, `jdk-17-ga`, `jdk-21-ga`, `jdk-22-ga`,
`jdk-23-ga`, `jdk-24-ga`, `jdk-25-ga`, `jdk-26-ga`, `master` (28-dev); HotSpot sources from
`openjdk/jdk11u`, `jdk17u`, `jdk21u`, `openjdk/jdk@master`. Where an entry does not exist in
a version that is stated explicitly — the negatives are as load-bearing as the positives.

---

## 3. The root-slot inventory (mechanism by mechanism)

### 3.0 Summary table

| # | Mechanism | Strong root? | GC-updates the slot? | Offsets in VMStructs? | JDK range | Barrier needed | Verdict |
|---|---|---|---|---|---|---|---|
| 1 | `Object[]` rooted in a **static field of a class mirror** | yes | yes | **yes, fully** | 8…26 | **yes** (card mark) | **bulk layer** — Serial/Parallel any JDK, G1 **26+** only (§6.3) |
| 2 | `ClassLoaderData::_handles` (`ChunkedHandleList`) append | yes (live CLD) | yes | **no** (`_handles` never exported) | 11…26 | **none** | **RECOMMENDED base** (needs signature scan; small capacity) |
| 3 | `JNIHandles::_global_handles` (`JNIHandleBlock`) append | yes | yes | **yes, fully** | **8 only** | **none** | **RECOMMENDED base on JDK 8** |
| 4 | `JavaThread::_active_handles` (`JNIHandleBlock`) append | yes | yes | **yes, fully** | 8…26 | no | rejected: lifetime ends at the next native-method return (§3.3) |
| 5 | `Universe` static oop roots (`_main_thread_group`, …) | yes | yes | 8…15 only | 8…15 | no | rejected: destructive + type-unsafe |
| 6 | `OopStorage` hand-allocation | yes | yes | **no fields at all** | 11…26 | no | **REJECTED — unsafe** (§5) |
| 7 | `ConstantPoolCache::_resolved_references` slot | yes | yes | `OopHandle` exported 11+ | 11…26 | yes | rejected: VM overwrites slots |
| 8 | G1 region pinning (`_pinned_object_count`) | n/a (address stability only) | n/a | **yes** | **22…26, G1 only** | no | interesting adjunct, not a pin |

### 3.1 Mechanism 1 — the Java-object-rooted handle table

> **Read §6.3 before implementing this.** The chain below is correct and fully discoverable,
> but the *store* into it needs a write barrier that cannot be emulated on G1 JDK 8…25. This
> mechanism is the bulk/capacity layer for Serial, Parallel and G1 26+; the base mechanism is
> §3.2 / §3.3.

The chain, and why every hop survives relocation:

```
Klass* K                         (metadata, never moves)
  └─ K->_java_mirror             VMStructs: nonstatic_field(Klass, _java_mirror, OopHandle)   [JDK 11+]
                                            nonstatic_field(Klass, _java_mirror, oop)         [JDK 8]
       └─ *(oop*)OopHandle._obj  a fixed C-heap cell; the GC WRITES the new mirror address here
            └─ mirror + off      static reference field of the anchor class, inside the mirror oop;
                                 scanned + updated by InstanceMirrorKlass::oop_oop_iterate
                 └─ Object[]     our handle array (objArrayOop)
                      └─ [i]     the pinned oop; scanned + updated as an ordinary array element
```

Exact VMStructs declarations (verbatim):

```
JDK 8    (jdk8u-dev)  nonstatic_field(Klass,  _java_mirror,  oop)
JDK 11   (jdk-11-ga)  nonstatic_field(Klass,  _java_mirror,  OopHandle)
JDK 17   (jdk-17-ga)  nonstatic_field(Klass,  _java_mirror,  OopHandle)
JDK 21   (jdk-21-ga)  nonstatic_field(Klass,  _java_mirror,  OopHandle)
JDK 25   (jdk-25-ga)  nonstatic_field(Klass,  _java_mirror,  OopHandle)
JDK 26   (jdk-26-ga)  nonstatic_field(Klass,  _java_mirror,  OopHandle)
```

Correction to a long-standing note in this repo (and in the header comment at line 3729):
`_java_mirror` became an `OopHandle` in **JDK 11**, not JDK 17 — JDK 8 is the only version
in vmhook's matrix where it is a bare `oop`. The header's runtime `type_string` sniff
handles both, so the code is right; only the comment is wrong.

The `OopHandle` type itself:

```
JDK 11, 12            declare_oop_type(OopHandle)                      // no _obj field exported
JDK 13…26, master     nonstatic_field(OopHandle, _obj, oop*)           // e.g. jdk-17-ga:333, jdk-21-ga:326
```

So on JDK 11/12 the `_obj` offset is not exported, but it is offset 0 of a single-pointer
struct and the header already assumes that.

**Why the mirror hop is the load-bearing one:** an `OopHandle` points into a VM-global
`OopStorage`, and `OopStorage` slots are strong roots that the GC *updates in place*. That
means the address of the cell is stable forever while the mirror it names moves freely.
vmhook has been relying on this since `get_java_mirror` was written; the pin design simply
extends the same trick one hop further into the Java heap.

**JDK 8 caveat:** with `_java_mirror` a bare `oop`, the *first* hop is itself a relocatable
value stored in metadata. It is still correct to read — the GC updates `Klass::_java_mirror`
during the class-root walk (`Klass::oops_do`), so re-reading it after a GC yields the moved
mirror. The value in the Klass is authoritative and updated; it is only *our cached copy*
that would be stale, and the header already re-reads it on every access.

**What remains to be solved for this mechanism:** (a) which static field to use as the
anchor (§4.1), (b) the card-table write barrier for the two heap stores (§6), (c) the
no-safepoint window (§7).

### 3.2 Mechanism 2 — `ClassLoaderData::_handles` (`ChunkedHandleList`)

This is the VM's own general-purpose "keep this oop alive for the lifetime of this class
loader" list, and its append protocol is *pure memory writes*. JDK 21 / master
(`classfile/classLoaderData.hpp`, `.cpp`):

```cpp
class ChunkedHandleList {
  struct Chunk : public CHeapObj<mtClass> {
    static const size_t CAPACITY = 32;
    oop _data[CAPACITY];
    volatile juint _size;
    Chunk* _next;
  };
  Chunk* volatile _head;
};

OopHandle ClassLoaderData::ChunkedHandleList::add(oop o) {
  if (_head == nullptr || _head->_size == Chunk::CAPACITY) {
    Chunk* next = new Chunk(_head);
    AtomicAccess::release_store(&_head, next);
  }
  oop* handle = &_head->_data[_head->_size];
  NativeAccess<IS_DEST_UNINITIALIZED>::oop_store(handle, o);
  AtomicAccess::release_store(&_head->_size, _head->_size + 1);
  return OopHandle(handle);
}
```

and the GC side, JDK 21 / master:

```cpp
void ClassLoaderData::ChunkedHandleList::oops_do(OopClosure* f) {
  Chunk* head = AtomicAccess::load_acquire(&_head);
  if (head != nullptr) {
    oops_do_chunk(f, head, AtomicAccess::load_acquire(&head->_size));
    for (Chunk* c = head->_next; c != nullptr; c = c->_next) oops_do_chunk(f, c, c->_size);
  }
}
inline void ClassLoaderData::ChunkedHandleList::oops_do_chunk(OopClosure* f, Chunk* c, const juint size) {
  for (juint i = 0; i < size; i++) f->do_oop(&c->_data[i]);   // JDK 21+/master: unconditional
}
```
(JDK 11 and 17 have `if (c->_data[i] != NULL)` around the `do_oop`; the `add()` body is the
same modulo `OrderAccess::release_store` and a plain `*handle = o`.)

So an external append is exactly: write the oop into `_head->_data[_size]`, then
`release`-store `_size + 1`. Slots live in **C-heap**, so **no card marking and no SATB
concerns** — root slots are enumerated in full at every collection.

**Blockers, honestly stated:**

1. `ClassLoaderData::_handles` is **not in VMStructs on any JDK**. The complete CLD export
   is:
   ```
   JDK 8    nonstatic_field(ClassLoaderData, _class_loader, oop)
            nonstatic_field(ClassLoaderData, _next, ClassLoaderData*)
               static_field(ClassLoaderDataGraph, _head, ClassLoaderData*)
   JDK 11   _class_loader (OopHandle), _next, _klasses, _is_anonymous, _dictionary
   JDK 17   _class_loader (OopHandle), _next, _klasses, _has_class_mirror_holder, _dictionary
            static_ptr_volatile_field(ClassLoaderDataGraph, _head, ClassLoaderData*)
   JDK 21   _class_loader, _next, _klasses, _has_class_mirror_holder
   JDK 25/26 same as 21, plus static _the_null_class_loader_data; macro renamed
            volatile_static_field(ClassLoaderDataGraph, _head, ClassLoaderData*)
   ```
   `ChunkedHandleList` and `Chunk` are absent from `VM_TYPES` in every version. The offset
   would have to be recovered by a **structural signature scan** of the CLD (find a word
   that points to a C-heap block whose `+256` is a `juint` in `[1,32]` and whose `+264`
   chains to more such blocks or null, and whose `_data[0]` decodes as a live oop). That is
   doable but is the kind of heuristic that quietly breaks on a JDK bump — exactly the
   class of fragility vmhook has been burned by before.
2. `add()` is single-writer by contract, serialised by `ClassLoaderData::metaspace_lock()`,
   which we cannot take. A concurrent VM `add_handle` (class definition, module creation,
   `ConstantPool::_resolved_references` installation) would race us: both write `_data[n]`
   and both store `_size = n+1`, so **one of the two oops is silently lost**. If the lost
   one is the VM's, a live `resolved_references` array or module oop loses its only root →
   collected → crash. The window is nanoseconds and CLD handle adds are rare after startup,
   but "rare heap corruption" is the worst possible failure mode.
3. JDK 8 has no `ChunkedHandleList` (its `ClassLoaderData::_handles` is a
   `JNIHandleBlock*`), so this mechanism needs the §3.3 variant there anyway.
4. Liveness of the list is conditional:
   ```cpp
   void ClassLoaderDataGraph::roots_cld_do(CLDClosure* strong, CLDClosure* weak) {
     for (ClassLoaderData* cld = ...; cld != nullptr; cld = cld->next()) {
       CLDClosure* closure = (cld->keep_alive_ref_count() > 0) ? strong : weak;
       if (closure != nullptr) closure->do_cld(cld);
     }
   }
   ```
   The **boot loader CLD** always has `keep_alive_ref_count() > 0`, so targeting the boot
   CLD (reachable as `ClassLoaderDataGraph::_head`'s entry with a null `_class_loader`, or
   in JDK 25+ the exported `_the_null_class_loader_data`) makes it an unconditional strong
   root that is never unloaded. Any other CLD can be unloaded, and unloading `delete`s every
   `Chunk` with no tombstone — leaving a dangling C-heap pointer with no way to detect it.

**Verdict:** technically sound and barrier-free, but gated on a signature scan and a real
(if narrow) lock race. Keep as a **second implementation** behind the same API, useful when
no acceptable static-field anchor exists.

### 3.3 Mechanism 3/4 — `JNIHandleBlock`

JDK 8 exports the **global** handle chain in full (`vmStructs.cpp:965-974`):

```
     static_field(JNIHandles,               _global_handles,      JNIHandleBlock*)
     static_field(JNIHandles,               _weak_global_handles, JNIHandleBlock*)
     static_field(JNIHandles,               _deleted_handle,      oop)
  unchecked_nonstatic_field(JNIHandleBlock, _handles, JNIHandleBlock::block_size_in_oops * sizeof(Oop))
  nonstatic_field(JNIHandleBlock,           _top,   int)
  nonstatic_field(JNIHandleBlock,           _next,  JNIHandleBlock*)
```
plus `declare_constant(JNIHandleBlock::block_size_in_oops)` (= 32). The VM's own allocation
is a `_top` bump on the last block:

```cpp
  if (_last->_top < block_size_in_oops) {
    oop* handle = &(_last->_handles)[_last->_top++];
    *handle = obj;
    return (jobject) handle;
  }
```
and the GC walks `[0, _top)` per block, filtering by heap range, stopping at the first
non-full block:

```cpp
      for (int index = 0; index < current->_top; index++) {
        oop* root = &(current->_handles)[index];
        oop value = *root;
        if (value != NULL && Universe::heap()->is_in_reserved(value)) f->do_oop(root);
      }
      if (current->_top < block_size_in_oops) break;
```
`_last` is not exported but is recoverable by walking `_next` while `_top == 32`. So on
JDK 8 a global JNI handle can be hand-allocated with two stores and no lock — racing only
`JNIGlobalHandle_lock`-holding `make_global`/`destroy_global` callers, with a lost-update
window identical in shape to §3.2. **From JDK 11 on, `_global_handles` is an `OopStorage*`
and this route disappears** (see §5).

The **thread-local** block is exported on *every* JDK 8…26:
`nonstatic_field(Thread, _active_handles, JNIHandleBlock*)` (JDK 8/11/17) →
`nonstatic_field(JavaThread, _active_handles, JNIHandleBlock*)` (JDK 21+), with the same
`_handles`/`_top`/`_next` offsets and constant. `JavaThread::oops_do_no_frames` walks it at every GC:
```cpp
void JavaThread::oops_do_no_frames(OopClosure* f, NMethodClosure* cf) {
  Thread::oops_do_no_frames(f, cf);
  if (active_handles() != nullptr) { active_handles()->oops_do(f); }
```
Appending there on the *current* JavaThread inside a no-safepoint window is race-free (the
block is thread-private) and needs no barrier. **But the lifetime is useless in practice:**
the template interpreter's native-method epilogue resets the block wholesale —
`movptr(t, Address(thread, JavaThread::active_handles_offset()); movl(Address(t,
JNIHandleBlock::top_offset_in_bytes()), NULL_WORD)` — i.e. **`_top` is set back to 0 on the
next native-method return on that thread**, and `JavaCallWrapper` swaps the whole block for
VM→Java calls. (INFERRED: quoted from memory of `generate_native_entry`; not re-verified
against a tag in this session — verify before relying on it.) So a handle placed there
survives only until the next arbitrary native call on that thread, which is unpredictable
and typically microseconds. Inside a detour a GC cannot happen anyway (§7), so this buys
nothing. **Downgraded to: not useful.** The §3.0 table's "bounded lifetime" entry should be
read as "unbounded *un*predictability".

### 3.4 Mechanism 5 — `Universe`'s static oop roots

They exist, they are strong roots, they are GC-updated, and they are **exported only up to
JDK 15**. JDK 8 / 11 (verbatim, `jdk8u-dev:443-468`):

```
     static_field(Universe, _mirrors[0],                  oop)
     static_field(Universe, _main_thread_group,           oop)
     static_field(Universe, _system_thread_group,         oop)
     static_field(Universe, _the_empty_class_klass_array, objArrayOop)
     static_field(Universe, _null_ptr_exception_instance, oop)
     static_field(Universe, _arithmetic_exception_instance, oop)
     static_field(Universe, _vm_exception,                oop)
     static_field(Universe, _collectedHeap,               CollectedHeap*)
```

From **JDK 16** onward the entire block collapses to a single line — this is the cut point,
not JDK 12 as previously believed:

```
JDK 16, 17, 21, 25, 26, master:
     static_field(Universe, _collectedHeap, CollectedHeap*)
```

(In the same release the underlying C++ fields became `OopHandle`s, `_vm_exception` was
deleted, `_the_empty_class_klass_array` was renamed `_the_empty_class_array`, the six OOME
roots collapsed into one `_out_of_memory_errors`, and in JDK 21 `_mirrors` was renamed
`_basic_type_mirrors`. None of `_the_null_string`, `_the_min_jint_string`, the OOME roots,
or the individual primitive mirrors was **ever** exported, in any version.)

Even on JDK 8…15 this mechanism is rejected: every one of these slots is *in use* by the VM
with a specific expected type, so writing an `Object[]` into `_main_thread_group` or
`_vm_exception` is a type-confusion time bomb (the VM hands the value to Java code declared
as `ThreadGroup`/`Throwable`, with no checkcast). There is no spare slot. Read-only use as a
*heap anchor* (e.g. "give me a known-live oop to sanity-check the narrow-oop base") is fine
and harmless — but that is not a pin.

### 3.5 Mechanism 7 — `ConstantPoolCache::_resolved_references`

`nonstatic_field(ConstantPoolCache, _resolved_references, OopHandle)` is exported JDK 11…26
(in JDK 8 it is a `jobject`). It is a ready-made, per-class `Object[]` behind a stable
`OopHandle` — i.e. mechanism 1's chain with the array pre-supplied. Rejected as a *slot
donor*: the VM writes resolved String/MethodHandle/MethodType constants into exactly those
elements as bytecodes are linked, so an element that is null today can be overwritten
tomorrow, silently dropping our pin. It is, however, a perfectly good **read-only anchor**
for validating the mirror→array→element addressing math during bring-up.

### 3.6 Mechanism 8 — G1 region pinning (`_pinned_object_count`)

Genuinely new, genuinely exported, and worth recording even though it is not a pin:

```
jdk-21-ga: (absent)
jdk-22-ga: volatile_nonstatic_field(HeapRegion,   _pinned_object_count, size_t)
jdk-23-ga: volatile_nonstatic_field(G1HeapRegion, _pinned_object_count, size_t)
```
(`gc/g1/vmStructs_g1.hpp`; the class was renamed in JDK 23.) `G1CollectedHeap::_hrm`,
`G1HeapRegionTable::_base/_biased_base/_bias/_shift_by` and `_bottom/_top/_end/_type` are
also exported, so the region containing an arbitrary heap address is locatable purely from
VMStructs. Per JEP 423 and `g1YoungCollector.cpp` / `g1FullCollector.cpp`, a non-zero pin
count makes G1 treat the region as evacuation-failed (young: promoted in place; old and
full GC: skipped for compaction), and `g1CollectedHeap.cpp` asserts a pinned region is never
freed.

**Why it is not the answer:** it buys *address stability*, not *liveness*, at *region*
granularity (1–32 MB), only on **G1**, only on **JDK 22+**. A leaked increment wedges the
region for the life of the process (OpenJDK tracks pin-count underflow/leak bugs under the
`gc-g1-pinned-regions` label). It is a plausible belt-and-braces adjunct for a
G1-22+-only fast path; it is not a portable pin.

### 3.7 Prior art

**There is none.** A search across profilers, injectors, JVM game-hacking projects and the
literature found **zero** implementations of a GC-tracked root obtained from native code
using only raw memory + `gHotSpotVMStructs`:

* **async-profiler** reads `gHotSpotVMStructs` (`readSymbol("gHotSpotVMStructs")`) but only
  ever touches *metadata* (Klass/Method/CodeCache), which never moves; in `--live` mode it
  falls back to `jni->NewWeakGlobalRef` and then *dereferences the handle cell directly*
  (`*(void**)((uintptr_t)w & ~1)`) — confirming the "handle cell is a stable native address
  whose contents the GC updates" model, while still calling JNI to allocate the cell. It
  also already *writes* VM state through the VMStructs flag table
  (`JVMFlag::find("DebugNonSafepoints")->set(1)`), which is useful precedent for the
  legitimacy of poking VM data structures.
* **HotSpot SA / opentelemetry-ebpf-profiler / perforator** are out-of-process or read-only;
  they have no relocation problem because the target is stopped.
* **JOL** uses the *exact* shape of mechanism 1 from the Java side —
  `Object[] BUFFERS; array[0] = o; addr = U.getLong(array, base); array[0] = null;` — and
  the corresponding JEP draft (8249196) documents that the address "may be outdated very
  early". It is the same peephole, read-only and Java-side.
* The **JVM internal-mod / Minecraft cheat cluster** (`baier233/younkoo`,
  `baier233/nobody-client`, `rdbo/jnihook`, `Lefraudeur/RiptermsGhost`, `x4e/RaionNative`,
  Cheat Engine's `CEJVMTI`) uses VMStructs for exactly the same metadata/hooking purposes as
  vmhook, and *all* of them revert to `NewGlobalRef` or JVMTI tags for object lifetime.
* **GraalVM/SubstrateVM** implements JNI globals internally as "a variable number of object
  arrays … inserted and nullified using atomic operations" — i.e. mechanism 1 is how a real
  runtime implements global refs; nobody has built it from outside.

Conclusion: the design below is novel. That cuts both ways — there is no field-proven
implementation to copy, and no community bug reports to learn from. Validation on the live
JVM matrix is therefore not optional (§10).

---

## 4. Choosing the anchor (mechanism 1's one hard policy question)

The chain needs exactly **one** rooted reference slot. Everything else hangs off the array,
so the dangerous operation happens **once per process**, not once per pin.

### 4.1 Candidate anchor slots

**The mirror's own injected fields are a dead end for an external tool.** HotSpot injects
`klass`, `array_klass`, `oop_size`, `static_oop_field_count`, `protection_domain`,
`init_lock`, `signers`, `source_file` into `java.lang.Class` (`CLASS_INJECTED_FIELDS` in
`classfile/javaClasses.hpp`), and two of them look ideal at first glance:

* `init_lock` (`object_signature`) is **guaranteed null after class initialization** —
  `InstanceKlass::fence_and_clear_init_lock()` does
  `java_lang_Class::set_init_lock(java_mirror(), NULL)` (JDK 8/11; `clear_init_lock` in
  17+), and `InstanceKlass::init_lock()` asserts
  `assert((oop)lock != NULL || !is_not_initialized(), "only fully initialized state can have a null lock")`.
* `signers` (`objArrayOop`) is null for every class not loaded from a signed JAR.

Both are unusable in practice:

1. **Their offsets are not exported.** In *every* version 8/11/17/21/master the
   `java_lang_Class` block of `vmStructs.cpp` contains exactly four entries:
   ```
   static_field(java_lang_Class, _klass_offset,                  int)
   static_field(java_lang_Class, _array_klass_offset,            int)
   static_field(java_lang_Class, _oop_size_offset,               int)
   static_field(java_lang_Class, _static_oop_field_count_offset, int)
   ```
   `_signers_offset`, `_init_lock_offset`, `_protection_domain_offset`,
   `_component_mirror_offset`, `_module_offset`, `_classData_offset` are **never** exported.
2. **Their names are not resolvable through the constant pool**, so vmhook's field walk
   cannot find them either. Injected fields carry vmSymbols indices, not CP indices:
   ```cpp
   // JDK 21 / master, oops/fieldInfo.inline.hpp
   inline Symbol* FieldInfo::name(ConstantPool* cp) const {
     int index = _name_index;
     if (_field_flags.is_injected()) { return lookup_symbol(index); }   // vmSymbols, NOT the CP
     return cp->symbol_at(index);
   }
   ```
   (JDK 8/17 are the same shape gated on `JVM_ACC_FIELD_INTERNAL` /
   `FieldInfo::is_internal()`.) `klass::find_field_in_stream` (line 4003) resolves every
   name via `resolve_constant_pool_symbol`, so injected fields simply never match — which
   is *correct* behaviour, but it means `signers`/`init_lock` are invisible to it.
3. **`init_lock` is a C union alias of `component_mirror` in JDK 11, 17 and GA-21**:
   ```cpp
   // javaClasses.cpp, compute_offsets()
   // Init lock is a C union with component_mirror. Only instanceKlass mirrors have
   // init_lock and only ArrayKlass mirrors have component_mirror. …
   _init_lock_offset = _component_mirror_offset;
   ```
   Writing it on an array-class mirror there destroys `component_mirror`. The union was
   removed again in jdk21u/23+/master.
4. **They move between releases anyway.** `signers` leaves the injected list in **JDK 24**
   (it becomes a real `private transient Object[] signers` Java field — which *is*
   CP-discoverable and `[Ljava/lang/Object;`-typed, so on JDK 24+ it becomes an excellent
   anchor); `protection_domain` leaves in JDK 25; `source_file` arrives in JDK 11;
   `init_lock` is absent from the injected list in 11/17/GA-21/22u and present in
   8/21u/23+/master. No injected slot is stable across 8…26.

**Therefore the anchor must be an ordinary, class-file-declared `static` reference field**,
which vmhook can already locate by name and offset with `find_field` + `field_proxy`
(`is_static` + `offset` relative to the declaring class's mirror, lines 3449, 16079). Static
oop fields are packed immediately after the `java.lang.Class` instance layout and are
traced+updated unconditionally:

```cpp
// oops/instanceMirrorKlass.inline.hpp (master; equivalent macro form in JDK 8)
template <typename T, class OopClosureType>
void InstanceMirrorKlass::oop_oop_iterate_statics(oop obj, OopClosureType* closure) {
  T* p         = (T*)start_of_static_fields(obj);
  T* const end = p + java_lang_Class::static_oop_field_count(obj);
  for (; p < end; ++p) Devirtualizer::do_oop(closure, p);   // traces AND relocates
}
```

**Recommended anchor policy** (in priority order):

1. **Caller-supplied anchor** — `vmhook::set_pin_anchor("com/example/Anchor", "vmhookHandles")`
   naming a `static Object[]` (or `static Object`) field in a class the *target application*
   owns. This is the only fully safe option and costs the integrator one field. vmhook is
   always injected into a known target, so this is realistic.
2. **Auto-discovered anchor** — walk loaded classes (the CLDG walk already exists,
   `for_each_loaded_class`) for a `static` field whose signature is exactly
   `[Ljava/lang/Object;` and whose current value is **null**, preferring classes outside
   `java.*`/`jdk.*`/`sun.*`. Claim it by writing our array and stamping a magic sentinel
   into `array[0]` (e.g. a `java.lang.String` we allocate containing `"vmhook:pin-table"`,
   or simply a known length + a klass identity check). On every subsequent access, verify
   the slot still points at an `Object[]` whose `[0]` is our sentinel; if not, the anchor
   was reclaimed by its real owner — invalidate every outstanding pin and re-anchor.
3. **JDK 24+ only** — `java.lang.Class.signers` of a deliberately chosen, never-signed
   class becomes a legal, type-correct, CP-discoverable `Object[]` slot. Not portable
   below 24; not worth special-casing unless (1) and (2) both fail.

**Non-negotiable rule for every anchor: only ever write into a slot that reads as null, and
never null out a non-null slot.** This is simultaneously the anti-clobber rule and the
condition that makes the missing G1 SATB pre-barrier sound (§6.2).


---

## 5. `OopStorage` hand-allocation — re-derived verdict: **unsafe, confirmed**

The prior investigation's conclusion is correct, but its reasons were weaker than the real
ones ("undocumented, version-specific layout" — in fact the layout is fully derivable). The
actual blockers, from the JDK 11/17/21/master sources:

**Layout (LP64, product).** `OopStorage::Block` is declared in `oopStorage.inline.hpp`
(not the `.cpp`):

```cpp
class OopStorage::Block {
  oop _data[BitsPerWord];            // 64 oops on LP64, offset 0, 512 bytes
  Atomic<uintx> _allocated_bitmask;  // offset 512
  intptr_t _owner_address;           // offset 520   (JDK 11: `const OopStorage* _owner`)
  void* _memory;                     // offset 528
  size_t _active_index;              // offset 536
  AllocationListEntry _allocation_list_entry;  // offset 544, 16 bytes
  Atomic<Block*> _deferred_updates_next;       // offset 560
  Atomic<uintx> _release_refcount;             // offset 568
};                                   // sizeof = 576, allocation_size = 632
const unsigned block_alignment = sizeof(oop) * section_size;   // 64 bytes
```

So a `Block*` **is** recoverable from any slot address (`block_for_ptr` aligns down and
probes back up to `section_count` candidates, comparing `_owner_address`), and vmhook can
get a legitimate slot address for free from any `Klass::_java_mirror`. The mechanism is
*reachable*. It is still unsafe:

1. **Zero VMStructs coverage.** `OopStorage` appears only as
   `declare_toplevel_type(OopStorage)`; there is not a single `nonstatic_field(OopStorage,
   …)` or `Block` entry in any JDK. Every offset above would be hardcoded — and the tail of
   `OopStorage` (`_concurrent_iteration_count`, `_mem_tag`, `_needs_cleanup`) sits *after*
   `SingleWriterSynchronizer`, which embeds a `Semaphore` = `HANDLE` (8 B, Windows) vs
   `sem_t` (32 B, Linux). The struct layout is therefore **OS-variant as well as
   version-variant**, and JDK 11 debug builds insert a vptr (`CHeapObj<mtGC>` →
   `AllocatedObj`) at offset 0.
2. **Bit-claiming corrupts the bitmask.** Since JDK 17 the allocator does *not* CAS:
   ```cpp
   void OopStorage::Block::atomic_add_allocated(uintx add) {
     uintx sum = _allocated_bitmask.add_then_fetch(add);   // precondition: (_allocated_bitmask & add) == 0
   }
   oop* OopStorage::Block::allocate() {
     uintx allocated = allocated_bitmask();                // NON-atomic read
     unsigned index = count_trailing_zeros(~allocated);
     atomic_add_allocated(bitmask_for_index(index));
     return get_pointer(index);
   }
   ```
   It reads the mask non-atomically under `_allocation_mutex`, then atomically **adds** the
   chosen bit. If we claimed that same bit between the read and the add, the add carries
   into neighbouring bits — silently marking *other* slots allocated. That is heap
   corruption, not a lost update. (JDK 11 used a CAS loop, which degrades to an ordinary
   double-allocation instead.)
3. **The block can be freed under us, asynchronously.** `ServiceThread` runs
   `OopStorage::delete_empty_blocks()` (≥ 500 ms cadence, `cleanup_defer_period`), which
   takes `_allocation_mutex` + `_active_mutex`, removes the block from `_active_array` and
   `FREE_C_HEAP_ARRAY`s its memory. `_active_array` itself can be replaced
   (`replace_active_array`, guarded by `SingleWriterSynchronizer`). Nothing we can hold
   prevents either.
4. **`release()` is fully lock-free and runs on any thread**, CAS-ing `_allocated_bitmask`
   and CAS-pushing onto `_deferred_updates` with a `_release_refcount` bracket. Any
   non-CAS external write to that word can be lost or can corrupt the deferred-update
   claim.

**Is there a read-only use — an existing allocated-but-provably-free-forever slot?** No.
Iteration is strictly bitmask-gated:

```cpp
inline bool OopStorage::Block::iterate_impl(F f, BlockPtr block) {
  uintx bitmask = block->allocated_bitmask();
  while (bitmask != 0) {
    unsigned index = count_trailing_zeros(bitmask);
    bitmask ^= block->bitmask_for_index(index);
    if (!f(block->get_pointer(index))) return false;
  }
  return true;
}
```
so an unallocated slot is *never* handed to the GC closure (writing there is a no-op that
the GC will never update), and an allocated slot always belongs to a live owner who will
eventually clear and release it — `check_release_entry` asserts the slot is null at release
and `make_global` asserts it is null at allocate, so a squatted value is an assertion
failure on fastdebug and a silently-lost pin on product. There is no free-forever slot.

Two further practical points: `OopStorageSet::jni_global()` existed only in JDK 15 and was
removed by 17 (17+ keep `_storages[]` private behind opaque `enum class StrongId/WeakId`),
and `OopStorageSet` does not exist at all in JDK 11 — so even *naming* the JNI-global
storage from outside is version-specific. `JNIHandles::_global_handles` (an `OopStorage*`
from JDK 11) remains the only exported handle onto it.

**Verdict: rejected.** Not "hard" — actively unsafe, in four independent ways, on the
current JDK matrix.

---

## 6. Write barriers

This section is the one that reshaped the verdict. **Summary: a raw reference store into a
Java object is emulatable on Serial and Parallel with one byte, and on G1 only on JDK 26+.
On G1 JDK 8…25 it is *not* emulatable, and attempting it is worse than doing nothing.**

| Collector | Post-barrier emulatable by pure memory write? | Pre-barrier needed? |
|---|---|---|
| Serial | **yes** — one unconditional byte | none exists |
| Parallel | **yes** — one unconditional byte | none exists |
| **G1 JDK 8…25** | **NO** — see §6.3 | yes, while marking is active |
| G1 JDK 26+ (JEP 522) | **yes** — the real barrier *is* "write the dirty byte" | yes, while marking is active |
| ZGC | no (colored pointers; the stored value must be a correctly-coloured `zpointer`) | n/a |
| Shenandoah | no in practice | yes (SATB) |

### 6.1 Serial / Parallel — trivially emulatable

The entire post-barrier, `gc/shared/cardTableBarrierSet.inline.hpp` (master, and identical
in shape back to JDK 8's `CardTableModRefBS::inline_write_ref_field`):

```cpp
template <DecoratorSet decorators, typename T>
inline void CardTableBarrierSet::write_ref_field_post(T* field) {
  volatile CardValue* byte = card_table()->byte_for(field);
  *byte = CardTable::dirty_card_val();
}
```

There is **no pre-barrier at all** — `inline_write_ref_field_pre` is an empty function on
JDK 8 and `ModRefBarrierSet::write_ref_field_pre` is an unoverridden empty default on 9+.
Serial and Parallel have no concurrent marking phase, so there is nothing to snapshot.

And the card table is scanned **wholesale** at every young collection, so a raw dirty byte
is honoured with certainty. Serial (`gc/serial/cardTableRS.cpp`) walks
`[byte_for(mr.start()), byte_for(mr.last())+1)` looking for dirty runs, clears them as it
goes and scans the covered objects with an exact `MemRegion` limit — meaning **precise
(field-address) marking works**; you do not have to dirty the card of the object *header*.
Parallel (`gc/parallel/psCardTable.cpp::scavenge_contents_parallel`) stripes the whole
old-gen card range across workers every young GC.

No other structure needs updating: the `ObjectStartArray` / block-offset table is an
*allocation*-time structure, and we do not allocate in the old generation.

### 6.2 The address computation and its version drift

```cpp
// gc/shared/cardTable.hpp, master (same shape since JDK 8)
CardValue* byte_for(const void* p) const {
  CardValue* result = &_byte_map_base[uintptr_t(p) >> _card_shift];
  ...
}
```
`_byte_map_base` is pre-biased (`_byte_map - (heap_start >> shift)`), so no heap-base
subtraction is needed. Reaching it from VMStructs:

```
JDK 8    static_field(oopDesc,        _bs,           BarrierSet*)          // or CollectedHeap::_barrier_set
         nonstatic_field(CardTableModRefBS, byte_map_base, jbyte*)         // NOTE: no leading underscore
JDK 11+  static_field(BarrierSet,     _barrier_set,  BarrierSet*)
         nonstatic_field(CardTableBarrierSet, _card_table, CardTable*)
         nonstatic_field(CardTable,   _byte_map_base, jbyte*)              // JDK 11
         nonstatic_field(CardTable,   _byte_map_base, CardTable::CardValue*) // JDK 17+
```
`_byte_map_base` is the one card-table field present unbroken across JDK 8…26.
`G1BarrierSet`/`G1CardTable` are **never** declared types in VM_TYPES, but
`class G1BarrierSet : public CardTableBarrierSet`, so the base-class offsets work for G1 too.

**`card_shift` is *not* a constant on modern JDKs.** `declare_constant(CardTable::card_shift)`
exists on JDK 8…17 and was **removed in JDK 21**; worse, JDK 18 (JDK-8272773, flexible card
size) turned it into a runtime `static uint _card_shift` driven by
`-XX:GCCardSizeInBytes` (default 512, range 128…1024 on LP64). It is **derivable at runtime**
without the constant:
`card_size = CollectedHeap::_reserved.byte_size() / CardTable::_byte_map_size`, and both of
those *are* exported on every JDK (`_reserved` on `CollectedHeap`, `_byte_map_size` on
`CardTable`/`CardTableModRefBS`). Do that rather than hardcoding 9.

Card values, per JDK (`clean_card = -1`, `dirty_card = 0` throughout):

| JDK | `CT_MR_BS_last_reserved` | `g1_young_gen` |
|---|---|---|
| 8…13 | 16 | **32 (0x20)** |
| 14…15 | 4 | **8 (0x08)** |
| 16…19 | 2 | **4 (0x04)** |
| 20…25 | 1 | **2 (0x02)** |
| 26+ | 1 | **does not exist** |

`declare_constant(G1CardTable::g1_young_gen)` is exported JDK 11…25 and **removed in JDK 26**;
`CardTable::clean_card`/`dirty_card` are exported on every JDK. So the values are all
obtainable without hardcoding — which matters, because they moved four times.

### 6.3 G1 JDK 8…25 — why the card byte alone is not merely insufficient but harmful

The real barrier is card byte **plus a dirty-card-queue enqueue**:

```cpp
// jdk21u, g1BarrierSet.inline.hpp / .cpp  (JDK 18..25; 11/17 identical modulo types)
inline void G1BarrierSet::write_ref_field_post(T* field) {
  volatile CardValue* byte = _card_table->byte_for(field);
  if (*byte != G1CardTable::g1_young_card_val()) write_ref_field_post_slow(byte);
}
void G1BarrierSet::write_ref_field_post_slow(volatile CardValue* byte) {
  OrderAccess::storeload();
  if (*byte != G1CardTable::dirty_card_val()) {
    *byte = G1CardTable::dirty_card_val();
    G1DirtyCardQueue& queue = G1ThreadLocalData::dirty_card_queue(Thread::current());
    G1BarrierSet::dirty_card_queue_set().enqueue(queue, byte);
  }
}
```
JDK 8's `G1SATBCardTableLoggingModRefBS::write_ref_field_work` is the same shape, and its
class comment states the contract outright: *"Usual invariant: all dirty cards are logged in
the DirtyCardQueueSet."*

**Is the card found at pause time anyway?** No.

* **JDK 8…13** — the pause root set is `Update RS` (drains the dirty-card *queues*) plus
  `Scan RS` (walks per-region remembered sets). Nothing scans the card table for dirty
  bytes. A dirty-but-unenqueued card is never refined, never enters an RSet, never scanned.
* **JDK 14…25** — "merge heap roots" (JDK-8213108, landed **JDK 14**, verified by tag bisect:
  `G1MergeHeapRootsTask` absent in `jdk-13-ga`, present in `jdk-14-ga`) *does* write into
  the card table, but only *from* RSet containers and log buffers, and each merged card also
  registers its region and its 128-card chunk:
  ```cpp
  void process_card(CardValue* card_ptr) {
    if (*card_ptr == G1CardTable::dirty_card_val()) {
      uint const region_idx = _ct->region_idx_for(card_ptr);
      _scan_state->add_dirty_region(region_idx);
      _scan_state->set_chunk_dirty(_ct->index_for_cardvalue(card_ptr));
    }
  }
  ```
  Scanning is then gated twice — `if (_scan_state->has_cards_to_scan(region_idx))` per
  region and `if (_scan_state->chunk_needs_scan(_region_idx, _cur_claim))` per chunk — and
  `prepare_region_for_scan` in JDK 21 deliberately does **not** add old/humongous regions to
  the dirty list. So an externally-dirtied card is found only by accident, if some *other*
  card in the same chunk of the same region happened to be merged.
* **JDK 26+ (JEP 522, JDK-8342382)** — the dirty card queue is gone entirely; refinement
  threads sweep a whole refinement card table, and `prepare_region_for_scan` now adds
  **every** non-CSet old/humongous region:
  ```cpp
  } else if (r->is_old_or_humongous()) {
    _scan_state->set_scan_top(hrm_index, r->top());
    _scan_state->add_dirty_region(hrm_index);      // NEW in JDK 26
  }
  ```
  with no `chunk_needs_scan` gate. **On JDK 26 the raw dirty byte is honoured**, and the
  barrier reduces to `if (*byte == clean) *byte = dirty;`. Two caveats even there: read
  `byte_map_base` from **that thread's** `G1ThreadLocalData::card_table_base_offset()`
  (the global table is swapped with a refinement table), and preserve the
  `== clean_card` conditional so you never clobber the new
  `g1_to_cset_card` / `g1_from_remset_card` / `g1_card_already_scanned` colours.

**And the active hazard:** on JDK 8…25 the barrier's fast path is
`if (*byte != dirty) { dirty; enqueue; }`. Once *we* have pre-dirtied a card without
enqueuing, **every subsequent legitimate Java store into that same 512-byte card skips the
enqueue**, and the card is never cleaned (cleaning happens only in
`clean_card_before_refine`, which is buffer-driven, or in the end-of-GC
`G1ClearCardTableTask`, which only clears `_all_dirty_regions`). So one stray card write
converts a one-shot missed root into a **permanently blind 512-byte window** for the rest of
the process. That is the single most dangerous line of code this research considered, and it
would be a natural "obvious fix" for the §12.1 latent bug. Do not write it.

### 6.4 G1 pre-barrier (SATB)

```cpp
// master, g1BarrierSet.inline.hpp
template <class T> inline void G1BarrierSet::enqueue(T* dst) {
  G1SATBMarkQueueSet& queue_set = G1BarrierSet::satb_mark_queue_set();
  if (!queue_set.is_active()) return;
  T heap_oop = RawAccess<MO_RELAXED>::oop_load(dst);
  if (!CompressedOops::is_null(heap_oop)) { ... enqueue_known_active(...); }
}
```

* **Skipping it when the previous value was null is sound, unconditionally** — that is the
  VM's own early-out, in C++ (`if (!CompressedOops::is_null(heap_oop))`) and in the
  generated barrier (`cmpptr(pre_val, NULL_WORD); jcc(equal, done)`). JDK 8 identical.
* **Overwriting or clearing a non-null slot while marking is active breaks SATB**: the
  overwritten referent — *and its entire transitive closure reachable only through it* —
  is never marked, is accounted garbage at Remark, and its region is reclaimed while still
  referenced. This is strictly worse than a missed card mark.
* **The safe protocol is therefore: only ever store into a slot that currently reads null;
  never overwrite and never clear a non-null slot.** For release, prefer to **leak** the
  slot (leave it populated, mark it reusable only after a marking-inactive check) over
  clearing it. Note this rule kills the *pre*-barrier requirement only — it does nothing
  for the post-barrier, which a null→non-null store is precisely the case that needs.
* Reading "is marking active" from outside: JDK 8 exports it directly
  (`nonstatic_field(JavaThread, _satb_mark_queue, ObjPtrQueue)` +
  `nonstatic_field(PtrQueue, _active, bool)`). JDK 9+ moved the queues into
  `G1ThreadLocalData` inside `Thread::_gc_data`, which is **not** in `gHotSpotVMStructs`;
  the per-thread offsets *are* exported as named constants in the **JVMCI** table
  (`jvmciHotSpotVMIntConstants`), verified present in 21u/25/26:
  ```cpp
  declare_constant_with_value("G1ThreadLocalData::satb_mark_queue_active_offset", ...)
  declare_constant_with_value("G1ThreadLocalData::satb_mark_queue_index_offset",  ...)
  declare_constant_with_value("G1ThreadLocalData::satb_mark_queue_buffer_offset", ...)
  declare_constant_with_value("G1CardTable::g1_young_gen", ...)
  declare_constant_with_value("CardTable::dirty_card", ...)
  ```
  That is a **second exported table** vmhook does not currently read at all, and it is worth
  adding a resolver for it independently of this feature.

### 6.5 What this means for the mechanisms

* **A root slot that lives *outside* the Java heap needs no barrier of any kind** — root
  slots are enumerated in full at every collection. That is true for `ClassLoaderData`'s
  `Chunk::_data` (C-heap), `JNIHandleBlock::_handles` (C-heap) and `OopStorage` blocks
  (C-heap). This is why mechanisms 2 and 3 leapfrog mechanism 1 on G1.
* **A store into a Java object in a *young* region needs no barrier either** — on
  Serial/Parallel young-gen fields are never reached through the card table, and on
  G1 ≤ 25 the barrier's own first test is `*card != g1_young_card_val()`. Region type is
  checkable from VMStructs (`G1HeapRegion::_type` / `G1HeapRegionType::_tag`,
  `G1CollectedHeap::_hrm`, `G1HeapRegionTable::_biased_base/_shift_by`). But an object does
  not *stay* young — it is promoted after a couple of collections — so "the handle array is
  young" is not a durable property and cannot be the basis of a design.
* **`make_java_object`'s TLAB allocation itself needs no barrier**: a fresh eden object's
  fields are scanned because the object is found by copying from roots, not through the card
  table, and under G1 concurrent marking it lies above TAMS and is implicitly live. What it
  *does* need is heap parsability: write the complete header (mark = prototype, klass slot,
  and `length` for arrays), zero the payload (TLAB memory is **not** pre-zeroed unless
  `-XX:+ZeroTLAB`), and publish `_top` **last**; never leave a gap smaller than
  `CollectedHeap::min_fill_size()`, which cannot be made parsable. vmhook's
  `make_java_object` bumps `_top` first and fills afterwards (5189, 12963) — safe only
  because the whole sequence runs in a no-safepoint window, which is exactly the §7
  requirement, and *not* safe on the fallback paths that allocate from another thread's TLAB.
* **A humongous `Object[]`** (≥ half a G1 region — 512 KB with the default 1 MB regions, so
  ~65 000 slots) is allocated *outside* the TLAB into an old-typed humongous region, so it
  gets the worst of both worlds: full post-barrier requirements and no TLAB path. Keep any
  handle array well under the humongous threshold.


---

## 7. The atomicity problem: read-oop → store-oop must not straddle a GC

Every mechanism in §3 has the same hidden precondition, and it is easy to miss because
JNI hides it: **the oop you are about to root must not move between the moment you read it
and the moment the root slot contains it.** `NewGlobalRef` is safe not because it is a VM
call but because its *argument* is already a root (a JNI local ref); it never handles a raw
address. vmhook's pin takes a raw address, so the window is real.

If a relocating GC lands inside that window, the consequence is not a stale read — it is a
**stale oop written into a live GC root**, which the collector will then dereference and
"update" at the next collection. That is heap corruption, and it is strictly worse than
today's no-op stub.

### 7.1 The guarantee that closes the window

For Serial, Parallel and G1 (and CMS and Epsilon), **all object relocation happens inside a
stop-the-world safepoint**. Primary sources:

* JEP 189 (Shenandoah), *Alternatives*: **"G1 does some parallel and concurrent work, but it
  does not do concurrent evacuation."**
* Oracle HotSpot GC Tuning Guide (JDK 25), G1: *"G1 is a generational, incremental,
  parallel, mostly concurrent, **stop-the-world, and evacuating** garbage collector… G1
  performs garbage collections and space reclamation in stop-the-world pauses. Live objects
  are typically copied from source regions to one or more destination regions in the heap."*
* Structural argument: G1 has **no load barrier**, only a card-table write barrier, so there
  is no mechanism by which a mutator could observe a concurrently-moved object. Concurrent
  cleanup frees only regions with zero live objects (`_cleanup_list` → `hr->par_clear()`);
  humongous eager reclaim happens inside the young pause; JEP 307 parallelised the full GC
  but it is still one STW pause; JEP 522's second card table is touched by refinement
  threads that never move object payloads. No JEP proposes concurrent G1 evacuation.

And a safepoint cannot *complete* while a `JavaThread` sits in `_thread_in_Java` without
reaching a poll. The classification is a two-case switch, unchanged in substance from JDK 8
to master (`runtime/safepoint.cpp`, master:524-539; 21u:688; 17u:676; JDK 8's
`SafepointSynchronize::safepoint_safe`, :688-702):

```cpp
static bool safepoint_safe_with(JavaThread *thread, JavaThreadState state) {
  switch(state) {
  case _thread_in_native:
    // native threads are safe if they have no java stack or have walkable stack
    return !thread->has_last_Java_frame() || thread->frame_anchor()->walkable();
  case _thread_blocked:
    assert(!thread->has_last_Java_frame() || thread->frame_anchor()->walkable(), "blocked and not walkable");
    return true;
  default:
    return false;
  }
}
```
```cpp
void ThreadSafepointState::examine_state_of_thread(uint64_t safepoint_count) {   // master:712-734
  ...
  if (safepoint_safe_with(_thread, stable_state)) { account_safe_thread(); return; }
  // All other thread states will continue to run until they
  // transition and self-block in state _blocked
```
`_thread_in_Java` hits `default: return false` in every version, so the thread stays in
`still_running` and the VM thread spins on it without bound —
`do { ... back_off(start_time); } while (still_running > 0);` (master:233-274, 17u:226-278,
21u:237-289, JDK 8:302). `SafepointTimeout` only *prints*; it never gives up. And the intent
is stated in the source, in the same words in JDK 8's `begin()` and master's
`arm_safepoint()`:

> *"2. Running in native code — When returning from the native code, a Java thread must check
> the safepoint `_state` to see if we must block. **If the VM thread sees a Java thread in
> native, it does not wait for this thread to block.**"*

JDK 10+ handshakes use the identical predicate (`handshake_safe` → `safepoint_safe_with`;
`handshake.cpp:655-671`), so the property is not a JDK-8-era accident.

One transition detail that matters for §7.2: `transition_from_java` merely *publishes* the
new state and does **not** poll — so a thread that changes its own state to
`_thread_in_native` immediately lets a pending safepoint proceed *without waiting for it*.
That is precisely why forging the thread state is unsafe. `transition_from_native` and
`transition_from_vm(..., _thread_in_Java)` do call
`SafepointMechanism::process_if_requested_with_exit_check`, i.e. the thread self-blocks in
`SafepointSynchronize::block()`.

**Therefore: native code running on a real `JavaThread` whose `_thread_state` is
`_thread_in_Java`, which executes no safepoint poll, blocks every safepoint — and hence
every relocating collection — for its whole duration.** Inside that window, read-then-store
is atomic with respect to GC. This is exactly the situation inside a vmhook interpreter-entry
detour, and it is the same property the header already depends on for the call-stub path
(15255) and documents for hooks generally (4934: *"This is the only state in which method
hooks are safely intercepted"*).

The contrapositive matters just as much: **on a non-JavaThread (vmhook's watchdog, a worker
thread, the viewer's payload thread) or on a JavaThread parked in `_thread_in_native`, a
full relocating GC can run concurrently with your code.** Pinning from there is unsound for
*every* mechanism in §3, and no amount of care fixes it.

**ZGC and Shenandoah void the guarantee entirely.** Both relocate concurrently, and worse:
the copy can be performed *by your own thread* from a load-barrier slow path
(`ShenandoahBarrierSet` → `_heap->evacuate_object(obj, Thread::current())`; ZGC →
`ZRelocate::relocate_object` → `ZUtils::object_copy_disjoint`), so merely *reading a
reference field* can move an object. And from-space is recycled early — JEP 333: *"It allows
us to reclaim and reuse memory during the relocation/compaction phase, before pointers
pointing into the reclaimed/reused regions have been fixed"* — so a stale pointer silently
returns another object's bytes rather than faulting. Add colored pointers (the raw 64-bit
field word is not an address until healed) and Shenandoah's forwarding-word move into the
mark word in JDK 13, and the conclusion is: **detect and refuse.** Platform note for CI: on
Windows x64, Oracle JDK ships ZGC but **not** Shenandoah (*"Oracle — Does not ship Shenandoah
in any release"*), while Temurin/Corretto/Zulu ship both from 11.0.9+; ZGC arrived on Windows
in JDK 14 (JEP 365).

One more G1-22+ wrinkle worth recording: JEP 423 changed JNI-critical semantics from
"GCLocker blocks GC globally" to "the region is pinned and GC proceeds". Any design that
was relying on *"a JNI critical section ⟹ no GC anywhere"* is wrong from JDK 22 on.

### 7.2 Implementer's checklist — how to *verify* you are in the window

Do not assume; check. In order, all of it already available in the header:

1. **Am I on a JavaThread that is really *this* OS thread?**
   `vmhook::hotspot::current_java_thread` is the TLS pointer set by the detour trampoline.
   Require it to be non-null and `is_valid_pointer`. Do **not** use
   `find_any_java_thread()` / `last_java_thread` / the SMR list walk here — those return
   *other* threads, and pinning against another thread's state is exactly the unsound case.
   (`make_java_object`'s fallbacks do exactly that; see §12.4.)
2. **Is its state `_thread_in_Java`?**
   `thread->get_thread_state() == java_thread_state::_thread_in_Java` (value 8).
   The field and the constants are exported on **every** JDK 8…master:
   ```
   volatile_nonstatic_field(JavaThread, _thread_state, JavaThreadState)   // 8:935, 11u:792, 17u:753,
                                                                         // 21u:727, 25u:619, 26u:615, master:600
   declare_integer_type(JavaThreadState)
   declare_constant(_thread_uninitialized) … _thread_new … _thread_in_native … _thread_in_vm …
   declare_constant(_thread_in_Java) … _thread_in_Java_trans … _thread_blocked … _thread_blocked_trans
   ```
   and the numeric values are **identical in JDK 8 `globalDefinitions.hpp:901` and master
   `:1027`** — `_thread_in_native = 4`, `_thread_in_vm = 6`, `_thread_in_Java = 8`,
   `_thread_blocked = 10`, `_thread_max_state = 12` — which exactly matches the hardcoded
   `java_thread_state` enum at vmhook.hpp:4923. (Prefer reading the constants from
   `gHotSpotVMIntConstants` anyway; they are free.)
   Note `_thread_blocked` is *also* classified safe by `safepoint_safe_with`, so it is not an
   acceptable state either — only `_thread_in_Java` (and `_thread_in_vm`, which JDK 8 handles
   with `roll_forward(_call_back)` and modern JDKs leave running) blocks the safepoint.
   Require `_thread_in_Java` exactly.
   Anything else — `_thread_in_native` (4), `_thread_in_vm` (6), any `_trans` variant —
   means a GC may be running right now. Refuse and return an empty pin.
   Do **not** "fix" this by calling `set_thread_state(_thread_in_Java)`: lying to the VM
   about a thread that is genuinely in native converts a safe refusal into a safepoint
   deadlock or a crash. (The header does force the state at 7617 after a user detour, which
   is defensible there because the thread genuinely was executing Java, but it is not a
   licence to do it here.)
3. **Is a GC in progress right now?**
   `CollectedHeap::_is_gc_active` (JDK ≤ 22) / `_is_stw_gc_active` (JDK 23+) — if true,
   something is very wrong with (2); refuse.
4. **Is the collector supported?** Refuse unless Serial / Parallel / G1 (§10.1).
5. **Bracket the operation with the collection counter.** Sample
   `CollectedHeap::_total_collections` immediately before reading the oop and again
   immediately before the store; if it changed, **abort without storing**. Inside a genuine
   no-safepoint window this check can never fire, so it costs two loads and is free
   insurance; outside one it converts most would-be corruptions into a clean refusal.
   This is the single highest-value guard in the design.
6. **Keep the window short and syscall-light.** While you hold the window, *every* thread in
   the JVM that wants a safepoint is stalled. `os::safe_read`/`safe_write` are syscalls
   (`ReadProcessMemory` / `process_vm_readv`); they do not poll and do not change thread
   state, so they are *correct* here, but a page fault inside one stalls the whole VM. Do
   not log, do not allocate, do not take a `std::mutex` that another thread could hold while
   blocked at a safepoint — that is a self-deadlock, and it is the same failure family as
   the repo's `#38` no-SEH heap/safepoint stall.
7. **Never do any of this from the auto-repair watchdog thread.** It is not a JavaThread.

### 7.3 What is *not* protected

The no-safepoint window stops the **GC**. It does not stop other **mutators**. Any structure
we touch that other threads also touch without a lock we can take — the CLD handle list
(`metaspace_lock`), the JDK 8 global handle chain (`JNIGlobalHandle_lock`), another thread's
TLAB `_top` — is still racy inside the window. Those races are per-mechanism and are called
out in §3.2, §3.3 and §12.4.


---

## 8. Detecting relocation instead of preventing it (the fallback that should ship first)

This is the layer that ships first, alone, and it is implementable from this section without
any of the rest of the document. It does not pin anything — it makes today's silent
use-after-relocation into a detectable `nullptr`.

### 8.1 What is exported, per JDK

```
JDK 8    (jdk8u-dev:458)   static_field(Universe, _collectedHeap, CollectedHeap*)
JDK 11   (jdk-11-ga:394)   static_field(Universe, _collectedHeap, CollectedHeap*)
JDK 17   (jdk-17-ga:375)   static_field(Universe, _collectedHeap, CollectedHeap*)
JDK 21   (jdk-21-ga:369)   static_field(Universe, _collectedHeap, CollectedHeap*)
JDK 25   (jdk-25-ga:331)   static_field(Universe, _collectedHeap, CollectedHeap*)
JDK 26   (jdk-26-ga:328)   static_field(Universe, _collectedHeap, CollectedHeap*)
master   (28-dev:311)      static_field(Universe, _collectedHeap, CollectedHeap*)
```
This is the *only* `Universe` entry that survives from JDK 16 onward (§3.4), and vmhook
already resolves it at line 9126.

```
JDK 8    (vmStructs.cpp:519-523)   nonstatic_field(CollectedHeap, _reserved,                 MemRegion)
                                   nonstatic_field(CollectedHeap, _barrier_set,              BarrierSet*)
                                   nonstatic_field(CollectedHeap, _defer_initial_card_mark,  bool)
                                   nonstatic_field(CollectedHeap, _is_gc_active,             bool)
                                   nonstatic_field(CollectedHeap, _total_collections,        unsigned int)
JDK 11/17/21/22 (vmStructs_gc.hpp) nonstatic_field(CollectedHeap, _reserved,                 MemRegion)
                                   nonstatic_field(CollectedHeap, _is_gc_active,             bool)
                                   nonstatic_field(CollectedHeap, _total_collections,        unsigned int)
JDK 23+  (and jdk21u)              nonstatic_field(CollectedHeap, _is_stw_gc_active,         bool)   // renamed
                                   nonstatic_field(CollectedHeap, _reserved,                 MemRegion)
                                   nonstatic_field(CollectedHeap, _total_collections,        unsigned int)
JDK 24/25/26                       + static_field(CollectedHeap, _lab_alignment_reserve,     size_t)
```

**`_total_collections` is exported on every JDK 8…26 (and master), under that exact name and
type.** The `_is_gc_active` → `_is_stw_gc_active` rename is at **JDK 21** (verified:
`20u:103` still `_is_gc_active`, `21u:99` already `_is_stw_gc_active`) — probe both names,
never key off a version number. The flag is set by the RAII `IsGCActiveMark` /
`IsSTWGCActiveMark` and is therefore true **only inside a stop-the-world GC**
(`collectedHeap.hpp` 8:86 *"friend class IsGCActiveMark; // Block structured external access
to _is_gc_active"*; master:94 `friend class IsSTWGCActiveMark;`). It is false throughout every
Shenandoah/ZGC concurrent phase — another reason those collectors must be refused outright.

Useful corollary despite the negative: `_total_full_collections` is declared **immediately
after** `_total_collections`, same type, in every version (`collectedHeap.hpp` 8:106-107,
11u:125-126, 17u:122-123, 21u:131-132, master:130-131), so `offset(_total_collections) + 4`
reaches it. That is an offset inference, not an export — use it only for diagnostics
(distinguishing "a full GC happened" in a log line), never as the safety-critical epoch.

**Explicit negatives, verified by exhaustive grep across `vmStructs.cpp`,
`vmStructs_gc.hpp` and every per-GC `vmStructs_*.hpp` for JDK 8, 11, 12, 15, 16, 17, 21, 22,
23, 24, 25, 26 and master:** `_total_full_collections`, `_gc_cause`, `_gc_lastcause`,
`_capacity_at_last_gc` and the `GCCause` type are **never** exported, in any version. There
is no VMStructs path to full-collection counts or GC cause. Also absent everywhere:
`SafepointSynchronize::_state` and `_safepoint_counter` — the safepoint counter idea in the
brief is a dead end on every JDK (corroborated by the SA having no `SafepointSynchronize`
class at all, since SA can only model what VMStructs exports).

That negative does not hurt, because `_total_collections` counts **both** kinds:

```cpp
// gc/shared/collectedHeap.hpp (jdk21u), verbatim
unsigned int _total_collections;          // ... started
unsigned int _total_full_collections;     // ... started

void increment_total_collections(bool full = false) {
  _total_collections++;
  if (full) { increment_total_full_collections(); }
}
unsigned int total_collections() const { return _total_collections; }
```
Sampling `_total_collections` alone is therefore a **superset** of every counted collection.

### 8.2 Does it increment for a relocating *young* collection? — yes, and *before* the moves

This is the load-bearing question, so here are the call sites, verified in source (jdk21u):

```cpp
// gc/g1/g1CollectedHeap.cpp
void G1CollectedHeap::gc_prologue(bool full) {
  assert(InlineCacheBuffer::is_empty(), "should have cleaned up ICBuffer");
  // Update common counters.
  increment_total_collections(full /* full gc */);
  if (full || collector_state()->in_concurrent_start_gc()) { increment_old_marking_cycles_started(); }
}

// gc/g1/g1YoungCollector.cpp — G1YoungCollector::pre_evacuate_collection_set()
_evac_failure_regions.pre_collection(_g1h->max_reserved_regions());
_g1h->gc_prologue(false);
// Initialize the GC alloc regions.
```
So a **G1 young evacuation pause** — the relocating one — bumps `_total_collections`, and it
does so in `pre_evacuate_collection_set`, i.e. **before any object is copied**.

```
// gc/shared/genCollectedHeap.cpp — GenCollectedHeap::do_collection(...)
   increment_total_collections(complete);      // twice: young path and full path,
                                               // after gc_prologue, before the collection work
// gc/parallel/psScavenge.cpp — PSScavenge::invoke_no_policy()
   heap->increment_total_collections();        // early, before ScavengeRootsTask
// gc/parallel/psParallelCompact.cpp — PSParallelCompact::pre_compact()
   heap->increment_total_collections(true);    // before marking/summary/compaction
```
Serial young + full, Parallel young (PSScavenge) and Parallel full (PSParallelCompact) all
increment, always before the work. **JDK 8 is explicit at the call site** rather than inside
`gc_prologue`, and covers the young pause the same way:

```cpp
// jdk8u g1CollectedHeap.cpp:4064-4065  (young evacuation pause)
    gc_prologue(false);
    increment_total_collections(false /* full gc */);
// jdk8u g1CollectedHeap.cpp:1309-1310  (full)
    gc_prologue(true);
    increment_total_collections(true /* full gc */);
// jdk8u genCollectedHeap.cpp:401-402
    gc_prologue(complete);
    increment_total_collections(complete);
// jdk8u: psScavenge.cpp:296, psParallelCompact.cpp:995 ("// Increment the invocation count"),
//        psMarkSweep.cpp:135 (the -XX:-UseParallelOldGC serial-old path)
```
Modern equivalents: master `serialHeap.cpp:388` (young) and `:566` (full); master
`g1YoungCollector.cpp:540 _g1h->gc_prologue(false);` and
`g1FullCollector.cpp:202 _heap->gc_prologue(true);`. Note `parallelScavengeHeap.cpp` contains
no increment in any version — the Parallel counters live in the two collector files above.

**Is there any Serial/Parallel/G1 collection that relocates without incrementing? No.** Every
relocating entry point funnels through one of the above. Explicitly checked and cleared:
G1 concurrent marking / refinement / cleanup (do not move objects); humongous eager reclaim
(happens inside the young pause, reclaims rather than moves); evacuation failure and JEP 423
region pinning (leave objects in place); `upgrade_to_full_collection` /
`satisfy_failed_allocation` (route to `do_full_collection` → `prepare_collection` →
`gc_prologue(true)`, so they *do* increment). JDK 8 has one escalation edge that looks like a
gap and is not — when a young collection escalates to a major one, only the *full* counter is
back-filled, because `_total_collections` was already bumped exactly once:
```cpp
// jdk8u genCollectedHeap.cpp:423-426
        if (i == n_gens() - 1) {  // a major collection is to happen
          if (!complete) {
            // The full_collections increment was missed above.
            increment_total_full_collections();
```
The error is only ever in the safe direction: the counter also increments for collections
that move nothing.

### 8.3 The soundness argument, and the exact epoch to sample

Sample a **pair**, not a scalar:

```
epoch := ( *(unsigned int*)(collected_heap + off_total_collections),
           *(bool*)        (collected_heap + off_is_gc_active) )
```

Ordering inside a pause is: `IsGCActiveMark` sets `_is_gc_active = true` → `gc_prologue`
increments `_total_collections` → objects are moved → the mark clears `_is_gc_active`. So:

* A pin is only *created* when `is_gc_active == false`; otherwise refuse (something is
  already wrong — see §7.2 step 3).
* At read time, the pin is fresh iff `total_collections` is **equal** to the recorded value
  **and** `is_gc_active` is false.
* A pause that started after the pin and is still running is caught by `is_gc_active`; a
  pause that started and finished is caught by the counter; a pause that had already
  incremented before we sampled cannot exist, because we refuse to sample while active.

**Therefore, for Serial, Parallel and G1: "epoch unchanged ⇒ the object did not move."** The
converse is deliberately conservative — a non-relocating collection (or one that did not move
*this* object) also invalidates the pin. That is the correct trade: false "stale" costs a
re-derivation, false "fresh" costs the process.

**Where this is NOT sound, stated loudly:**

* **ZGC and Shenandoah relocate objects with no safepoint at all, and their counters tick at
  the *start of the cycle*, phases before the relocation.** Concretely:
  `shenandoahControlThread.cpp:288` increments on the control thread *before* the concurrent
  cycle starts, and `zGeneration.cpp:600/637` increments inside
  `VM_ZMarkStartYoungAndOld` / `VM_ZMarkStartYoung`. Relocation happens later in the same
  cycle, so a window lying entirely inside the evacuation/relocation phase sees an
  **unchanged counter while objects move**. The same holds for the jstat counters —
  `ShenandoahConcurrentGC::entry_evacuate()` bumps `concurrent_collection_counters()`
  *before* calling `op_evacuate()`. And the copy can be performed by the reading thread
  itself from a load-barrier slow path.
  **`backend()` must return `none` and every pin must be empty when ZGC or Shenandoah is
  detected** (§10.1). This is the one case where the detector would otherwise re-introduce
  exactly the silent UB it exists to remove.
* **G1 concurrent string deduplication** (JEP 192, concurrent since JDK 18) can swap a
  `String`'s `value` array for a canonical one *without* any relocation. A cached
  `byte[]`/`char[]` oop obtained by reading `String.value` can therefore stop being the array
  that `String` points at, with the epoch unchanged. It is still a *live* array, so this is a
  correctness-of-content hazard, not a memory-safety one — but `read_java_string` callers
  that cache the backing array should be aware.
* **Epsilon** never moves anything; the epoch is trivially always valid.

Two mechanical notes: `_total_collections` is a plain `unsigned int` written by the VM thread
at a safepoint, so an aligned 4-byte load cannot tear on any supported architecture — read it
through `os::safe_read` anyway, for the unmapped-page discipline the rest of the header uses.
And compare for **inequality**, not ordering: it is 32-bit and will wrap after ~4.3e9
collections. If a monotonic 64-bit epoch is wanted, accumulate deltas.

### 8.4 The independent cross-check: PerfMemory / jstat

Exported **identically in JDK 8, 11, 17, 21, 25 and 26** (`vmStructs.cpp` 8:597, 11:440,
17:424, 21:412, master:353):

```
     static_field(PerfMemory, _start,      char*)
     static_field(PerfMemory, _end,        char*)
     static_field(PerfMemory, _top,        char*)
     static_field(PerfMemory, _capacity,   size_t)
     static_field(PerfMemory, _prologue,   PerfDataPrologue*)
     static_field(PerfMemory, _initialized, jint)   // JDK 8; volatile_static_field(..., int) on 11+
  nonstatic_field(PerfDataPrologue, magic/byte_order/major_version/minor_version/
                                    accessible/used/overflow/mod_time_stamp/entry_offset/num_entries, …)
  nonstatic_field(PerfDataEntry,    entry_length/name_offset/vector_length/data_type/
                                    flags/data_units/data_variability/data_offset, …)
```

The two structs are **byte-identical in jdk8u-dev and master** (`runtime/perfMemory.hpp`,
`PERFDATA_MAJOR_VERSION 2` / `MINOR 0` throughout):

```c
typedef struct {
  jint magic; jbyte byte_order; jbyte major_version; jbyte minor_version; jbyte accessible;
  jint used; jint overflow; jlong mod_time_stamp; jint entry_offset; jint num_entries;
} PerfDataPrologue;
typedef struct {
  jint entry_length; jint name_offset; jint vector_length;
  jbyte data_type; jbyte flags; jbyte data_units; jbyte data_variability;
  jint data_offset;
} PerfDataEntry;
```
Walk: `base = _prologue` (== `_start`); check `magic == 0xcafec0c0`, `accessible != 0`,
`major_version == 2`; `entry = base + entry_offset`; then `num_entries` times — name at
`entry + name_offset` (NUL-terminated), value at `entry + data_offset`,
`entry += entry_length`. Requires `-XX:+UsePerfData`, which is the default.

Counter semantics (`collectorCounters.cpp`: `PerfDataManager::name_space("collector",
ordinal)` + `create_counter(SUN_GC, …, U_Events)` → `V_Monotonic`, `data_type 'J'`), ordinal
0 per collector: Serial `defNewGeneration.cpp:250 new CollectorCounters(policy, 0)` = young
(full is ordinal 1 in `tenuredGeneration.cpp:332`); Parallel `psScavenge.cpp:545 "Parallel
young collection pauses", 0` (full = 1); G1 `g1MonitoringSupport.cpp:126 "G1 young collection
pauses", 0` (full = 1, concurrent cycle = 2), driven by `G1YoungGCMonitoringScope` at
`g1YoungCollector.cpp:1121` — the same event as `_total_collections`, so it is an honest
cross-check. **Not** an honest one for the concurrent collectors: ZGC uses ordinal 0 for
"minor" and **2** for "major" (1 unused), and Shenandoah's `stw_collection_counters()`,
`full_stw_collection_counters()` and `concurrent_collection_counters()` all return the *same*
`_full_counters`, so ordinal 1 is shared across concurrent, STW and full.

This yields the jstat counters, including `sun.gc.collector.0.invocations`,
`sun.gc.collector.1.invocations` and — the reason to bother — **`sun.gc.collector.0.name`
and `sun.gc.policy.name`, which name the live collector unambiguously on every JDK and every
GC**, with no flag table, no vtable symbols, and no build-config ambiguity. Use it as (a) the
collector-detection fallback of §10.1 and (b) a Phase-0 cross-check that
`_total_collections` really moved when a collection happened. Do not use it as the primary
epoch: it is string-keyed, requires parsing, and its per-collector semantics differ between
GCs.

Other counters considered and rejected: `Generation::StatRecord::invocations` /
`accumulated_time` (exported JDK 8…21, **removed with the `Generation` hierarchy in 24/25**),
and the `G1MonitoringSupport::_eden_used/_survivor_used/_old_used` fields (G1-only, and they
measure occupancy, not collections).

### 8.5 Implementation sketch

```
// resolved once
Universe::_collectedHeap            -> entry->address, deref -> CollectedHeap*
CollectedHeap::_total_collections   -> offset
CollectedHeap::_is_gc_active | _is_stw_gc_active -> offset   (probe both names)

struct epoch_t { std::uint32_t collections; bool gc_active; bool valid; };

epoch_t current_epoch() noexcept:
    if (!heap || !off_total) return { 0, true, false };          // unreadable -> always stale
    if (collector is ZGC/Shenandoah/unknown) return { 0, true, false };
    safe_read collections; safe_read gc_active;
    return { collections, gc_active, true };

global_ref::global_ref(oop):  oop_ = oop; epoch_ = current_epoch();
                              if (!epoch_.valid || epoch_.gc_active) oop_ = nullptr;
global_ref::is_stale():       e = current_epoch();
                              return !epoch_.valid || !e.valid || e.gc_active
                                     || e.collections != epoch_.collections;
global_ref::oop():            return is_stale() ? nullptr : oop_;
```

Cost: two cached-offset loads per `oop()` call. That is cheap enough to leave on permanently
and removes an entire class of UB from the header.


---

## 9. Forwarding pointers

Short answer: **no, following a forwarding pointer after the fact is never sound**, on any
collector. Worth stating precisely, because the encoding is trivially readable and therefore
tempting.

### 9.1 The encoding

```cpp
// jdk21u, oops/oop.inline.hpp
bool oopDesc::is_forwarded() const { return mark().is_marked(); }
void oopDesc::forward_to(oop p) { markWord m = markWord::encode_pointer_as_mark(p); set_mark(m); }
oop  oopDesc::forwardee() const  { return cast_to_oop(mark().decode_pointer()); }

// oops/markWord.hpp
static const uintptr_t marked_value = 3;
bool is_marked() const { return (mask_bits(value(), lock_mask_in_place) == marked_value); }
inline void* decode_pointer() const { return (void*)clear_lock_bits().value(); }
```
So: low two bits of the mark word `== 0b11` ⇒ forwarded, and the forwardee is
`mark & ~0b11` — a full native address, never compressed. JDK 24+ adds self-forwarding for
evacuation failure (`is_self_forwarded()`, `self_fwd_shift`), and with
`UseCompactObjectHeaders` the **full GCs** of Serial/Parallel/G1/Shenandoah use a completely
different compressed encoding (`gc/shared/fullGCForwarding.hpp`: *"…preserves upper N bits
of object mark-words, which contain crucial Klass* information when running with compact
headers… we have 40 bits to encode forwarding pointers"*). So even the decode is version- and
flag-dependent.

### 9.2 Lifetime, per collector

| Collector / phase | Forwarding pointer in the from-copy? | From-space afterwards |
|---|---|---|
| Serial young (DefNew) | yes (`forward_to`) | eden + from-survivor `clear()`ed; bytes intact in product, reused by the very next allocation |
| Serial full (mark-compact) | yes, during phases 2–3 only | slid over by the compaction memmove |
| Parallel young (PSScavenge) | yes (`forward_to_atomic`) | as Serial young |
| **Parallel full (PSParallelCompact)** | **no** — destinations come from the region summary via `calc_new_pointer`; original marks go to `PreservedMarks` | compacted over |
| G1 evacuation (young/mixed) | yes (`forward_to_atomic`) | CSet regions freed → `hr_clear()` → `set_top(bottom()); set_free(); clear(Mangle)` and onto the free list; **and the range may be uncommitted** on heap shrink |
| G1 full GC | yes, or `FullGCForwarding` with compact headers | compacted over |
| Shenandoah | yes (same mark-word encoding; separate word before the header pre-JDK-13, JDK-8224584 moved it into the mark word in 13) | from-space regions trashed/recycled immediately after evac, concurrently |
| **ZGC (both generations)** | **no** — forwarding lives in off-heap `ZForwarding`/`ZForwardingTable` hash tables keyed by from-offset | pages unmapped/recycled; and pointers are *colored*, so a raw field word is not an address at all |

### 9.3 Why it can never be exploited safely

1. **ZGC and Parallel-full have no in-object forwarding pointer to read.**
2. **The lifetime is entirely inside the pause.** The from-copy's mark word is valid only
   between "the copy happened" and "the from-region was recycled", and on Serial/Parallel/G1
   both happen within the same STW pause. A mutator-side hook only ever runs *after* the
   pause, by which time the memory is on a free list and legitimately reusable by any
   allocating thread. Reading it is an unsynchronised race that gives the right answer for
   microseconds and then silently the wrong object.
3. **G1 makes it unsafe rather than merely racy**: freed regions can be *uncommitted*, so a
   stale read is an access violation, not a wrong value.
4. **Zapping hides the bug in exactly the wrong direction.** `ZapUnusedHeapArea`
   (`badHeapWordVal = 0xBAADBABE`) is a `develop` flag — compile-time `false` in product. So
   in a production JVM freed heap memory is *not* zapped and the stale pointer physically
   survives until overwritten; in fastdebug it is mangled. That is precisely the shape of
   bug that passes CI and kills a user's process.

The correct alternatives are the ones this document is about: hold a GC-visible root the
collector updates for you, or re-resolve the object from a stable identity, or read fields
only inside a single safepoint-free window and never cache the raw address across it.


---

## 10. Recommended design

Four layers, shippable independently, in this order. Layer 1 is a strict improvement on
today's behaviour and has no prerequisites; layers 2–3 are opt-in.

| Layer | What | Coverage | Risk | Ship |
|---|---|---|---|---|
| 0 | Capability gate (collector, JDK shape, compressed oops) | all | none (reads only) | first, needed by everything |
| 1 | **Relocation detector** — `global_ref` invalidates instead of dangling | all JDK, all collectors | none | **first release** |
| 2 | **Pin via a C-heap root slot** (CLD handle list / JDK 8 global handle block) | JDK 8…26, Serial/Parallel/G1 | medium (lock race, offset discovery) | opt-in, after CI |
| 3 | Bulk `Object[]` handle table rooted in a static field | Serial/Parallel any JDK; G1 **26+** only | high on G1 ≤ 25 (do not) | last, gated |

### 10.1 Layer 0 — capability gate

Compute once, cache in a `static` struct, re-check nothing at runtime except the counter.

1. **Collector.** Walk the flag table and read `UseSerialGC` / `UseParallelGC` / `UseG1GC` /
   `UseZGC` / `UseShenandoahGC` / `UseEpsilonGC`. All are `product(bool, …, false)` in every
   version 8…master. The walk itself is universally available:
   ```
   JDK 8      static_field(Flag,    flags,    Flag*)        static_field(Flag,    numFlags, size_t)
              nonstatic_field(Flag, _type,    const char*)  nonstatic_field(Flag, _name,    const char*)
              unchecked_nonstatic_field(Flag, _addr, sizeof(void*))
   JDK 11     same, class renamed JVMFlag (JDK-8211821)
   JDK 17/21/25/26/master   identical to 11 except:  nonstatic_field(JVMFlag, _type, int)
   ```
   Two rules that make this robust: take the array **stride from `gHotSpotVMTypes["JVMFlag"].size`**
   (never `sizeof` — the `_doc` member exists only in non-product builds), and branch on
   `_type`'s *exported `typeString`* (`"const char*"` ⇒ JDK 8/11 string types; `"int"` ⇒ 17+
   ordinal `0=bool,1=int,2=uint,3=intx,4=uintx,5=uint64_t,6=size_t,7=double,8=ccstr`). The
   table itself tells you which shape you are on, so no JDK-version sniffing. Walk
   `numFlags - 1` entries; the last is an all-null sentinel. This is exactly what the
   Serviceability Agent does (`sun/jvm/hotspot/runtime/VM.java`), and async-profiler already
   ships the write half of it.
   *Fallbacks if the flag walk fails:* `HeapRegion::GrainBytes` /
   `G1HeapRegion::GrainBytes` (master) is a zero-until-G1-initialises static in every
   version, and `ShenandoahHeapRegion::RegionSizeBytes` likewise for Shenandoah; last resort,
   parse `sun.gc.collector.0.name` out of PerfMemory (§8.4), which names the live collector
   on every version and every GC. Do **not** use VM_TYPES presence (`declare_type(ZCollectedHeap,…)`
   is a *build*-time `INCLUDE_ZGC` guard, present even when ZGC is not running), and do not
   use the SA's vtable-symbol trick (`??_7G1CollectedHeap@@6B@` is not exported from
   `jvm.dll`).
2. **Refuse ZGC and Shenandoah outright** (§7.1). Also refuse if the collector cannot be
   determined. Epsilon needs no pin (it never moves anything) — treat as "supported, pin is
   a no-op that always succeeds".
3. **Compressed oops.** Read `UseCompressedOops` from the flag table. This is the **only**
   reliable source: `_narrow_oop._base == 0 && _shift == 0` is genuinely ambiguous between
   "off" and "on, unscaled heap under 4 GB", `oopSize` is `sizeof(char*)` (always 8 on LP64),
   and `heapOopSize` is **not** exported in any table in any version. Partial cross-check:
   `_shift != 0` ⇒ definitely on; `_shift == 0 && _base == 0 &&` reserved-heap-top > 4 GB ⇒
   definitely off. Also read `UseCompactObjectHeaders` (JDK 25+, **default true on master**)
   because it changes the header/klass encoding this header decodes.
4. **JDK barrier shape**, derived from what resolves, never from a version number:
   `CardTableModRefBS::byte_map_base` present ⇒ JDK 8 shape;
   `CardTable::_byte_map_base` + `BarrierSet::_barrier_set` ⇒ 11+;
   `declare_constant(G1CardTable::g1_young_gen)` absent while G1 is active ⇒ JDK 26 shape
   (card byte alone is honoured).
5. **Card geometry** (only needed by layer 3): `card_size = CollectedHeap::_reserved.byte_size()
   / CardTable::_byte_map_size`, `card_shift = log2(card_size)`. Do not use
   `declare_constant(CardTable::card_shift)` — removed in JDK 21 and made runtime-variable in
   JDK 18 by `-XX:GCCardSizeInBytes`.

### 10.2 Layer 1 — the relocation detector (ship this first)

Full algorithm in §8. API delta on `vmhook::jni::global_ref`:

```
explicit global_ref(oop_t raw);   // records epoch_ = current_gc_epoch() alongside oop_
auto oop()       const -> oop_t;  // returns nullptr if is_stale()
auto is_stale()  const -> bool;   // current_gc_epoch() != epoch_
auto raw_unsafe()const -> oop_t;  // the captured address, no staleness check (for diagnostics)
```

Semantics: **a pin is valid exactly until the next collection.** That is a real, documented,
useful contract (it covers "compute on this tick, consume later on this tick" and every
cross-thread hand-off that completes before a GC), and it replaces silent UB with a null.
`operator bool` becomes `oop_ != nullptr && !is_stale()`.

Degradation: if the epoch cannot be read at all (no `Universe::_collectedHeap` or no
`CollectedHeap::_total_collections`), `is_stale()` must return **true** — i.e. the pin is
permanently useless rather than silently unsafe. Never default to "assume not moved".

### 10.3 Layer 2 — the pin: one C-heap root slot per pin

This is the mechanism that works on G1, because it needs **no write barrier of any kind**
(§6.5). Order of operations, all inside the §7.2 window:

**JDK 11…26 — `ClassLoaderData::_handles` append**

1. *(once)* Resolve the boot CLD: `ClassLoaderDataGraph::_head` (exported in every version;
   `static_ptr_volatile_field` in 17…23, `volatile_static_field` in 24+) and walk `_next`
   for the CLD whose `_class_loader` `OopHandle` dereferences to null. On JDK 25+ prefer the
   exported `static_field(ClassLoaderData, _the_null_class_loader_data, ClassLoaderData*)`.
   The boot CLD is never unloaded and always has `keep_alive_ref_count() > 0`, so it is an
   unconditional strong root (`roots_cld_do` picks the `strong` closure for it).
2. *(once)* Recover the `_handles` offset by **signature scan**, because it is not exported
   in any JDK. Scan the CLD's first ~256 bytes for a word `p` such that: `p` is a readable
   C-heap address; `*(juint*)(p + 32*sizeof(oop))` is in `[1, 32]`; `*(void**)(p + 32*sizeof(oop) + 8)`
   is either null or another candidate with the same shape; and `*(oop*)p` decodes to an oop
   whose klass is a valid `Klass*`. Require the candidate to be **unique** in the scan range;
   if zero or multiple candidates match, disable layer 2. Re-validate the candidate on every
   use (cheap: `_size <= 32`).
3. Read `_head` (acquire). **If `_head == nullptr` or `_head->_size == 32`, fail the pin.**
   Do not allocate a `Chunk`: the VM does `new Chunk(_head)` through `CHeapObj<mtClass>` and
   will later `delete` it, so a chunk we `malloc` ourselves is an allocator mismatch (and an
   NMT-header mismatch) at CLD teardown. This caps us at whatever the current head chunk has
   free — a handful of slots, refreshed whenever the VM itself pushes a new chunk. **State
   this capacity limit in the API**; it is the mechanism's defining constraint.
4. Write the oop into `_head->_data[_size]` (raw store; the slot is C-heap, no barrier), then
   publish with a **release** store of `_size + 1`. Use a real release store
   (`std::atomic_ref` / `_ReadWriteBarrier` + volatile store), not `os::safe_write` — the
   syscall happens to be a full barrier but it is 100× the cost and obscures the intent.
   This mirrors the VM's own `ChunkedHandleList::add` exactly.
5. Record `(chunk, index)` in the `global_ref`. `oop()` re-reads `_data[index]` every call —
   that slot is what the GC updates.
6. **Release: leak by default.** Clearing the slot is a non-null→null store into a root,
   which needs the SATB pre-barrier when G1 marking is active (§6.4). Options, in order:
   leave it populated and mark it reusable-by-us (store a subsequent pin's oop there — a
   non-null→non-null overwrite has the *same* SATB problem, so this is only legal when
   marking is inactive); or clear only after reading the marking-active flag inside the
   window. Reading that flag: JDK 8 exports it directly (`nonstatic_field(JavaThread,
   _satb_mark_queue, ObjPtrQueue)` + `nonstatic_field(PtrQueue, _active, bool)`); JDK 9+
   needs `G1ThreadLocalData::satb_mark_queue_active_offset` from the **JVMCI** constant table
   (`jvmciHotSpotVMIntConstants`), which vmhook does not read today and should learn to.
   If the flag is unreadable, **never clear** — leaking a handle slot costs one retained
   object; clearing it wrongly costs the heap.

**JDK 8 — `JNIHandles::_global_handles` append**

Same shape, and fully exported (§3.3): walk `_next` while `_top == block_size_in_oops` to
find the last block, then `_handles[_top] = oop; _top = _top + 1` (write the oop **first**,
publish `_top` second — the GC scans `[0, _top)`). Fail if the last block is full. Release =
store `JNIHandles::_deleted_handle` (exported) into the slot, which is exactly what
`destroy_global` does and is a non-null→non-null store, so no SATB concern; the VM's own
`rebuild_free_list` will reclaim it.

**Races to accept and document** (neither is closable from outside): the CLD append races
`metaspace_lock`-holding `add_handle` callers, and the JDK 8 append races
`JNIGlobalHandle_lock`-holding `make_global`. Both windows are a few instructions and both
are rare after startup, but a lost update loses *their* oop, not just ours. Mitigate by (a)
doing all pins in the §7.2 window, (b) re-reading `_size`/`_top` immediately after the
publish and refusing to trust the pin if it does not equal what we wrote, and (c) keeping
the number of pin operations small — pin once, reuse.

### 10.4 Layer 3 — the bulk handle table, and why it is G1-26+ only

Chain: anchor static field (§4.1) → `Object[]` → element. Allocation and addressing are
already implemented (`make_java_array("[Ljava/lang/Object;", n, oop_size)`,
`klass::get_java_mirror`, `field_proxy` static re-resolution). The only new code is the
store:

```
store(slot_addr, value):
    write the (narrow or wide) oop at slot_addr
    if collector is Serial or Parallel:            dirty_card(slot_addr)          // always safe
    else if collector is G1 and jdk_shape >= 26:   if (*card == clean) *card = dirty
    else if collector is G1 (8..25):               REFUSE — see §6.3
dirty_card(p):  byte_map_base[ uintptr_t(p) >> card_shift ] = CardTable::dirty_card
```

Keep the array under the humongous threshold (≥ half a G1 region goes to an old-typed
humongous region and is not TLAB-allocated). Only ever write into elements that read null,
and never clear one (§6.4).

**The G1 ≤ 25 residual problem, and the one idea that might close it** (research, not a
recommendation): make every write target an object that is still in a *young* region, where
no barrier is required at all. Concretely — an *epoch* scheme: on each GC epoch (detected by
layer 1's counter), allocate a **fresh** `Object[]` from the TLAB (hence eden), copy the
previous epoch's now-GC-updated entries into it (writes into a young array need no card
mark), and swap the C-heap root slot from layer 2 to the new array (a C-heap store, also no
card mark). Every write we ever perform then targets an array allocated *since the last
collection*, which cannot yet have been promoted. The catch is the swap itself: overwriting
a non-null root while G1 marking is active drops the old array from the SATB snapshot, and
chaining `new[0] = old` does **not** save it (objects above TAMS are implicitly live and are
*not* traversed by the marker, so the marker would never follow `new[0]`). So the swap must
be gated on the marking-inactive flag — the same JVMCI-sourced flag as §10.3 step 6. That is
implementable, but it is three interlocking assumptions deep and must not ship without the
§11 soak.

### 10.5 Public API and failure modes

```
vmhook::gc::backend()      -> enum { none, detector_only, cheap_root, handle_table }
vmhook::gc::epoch()        -> std::uint64_t          // §8
vmhook::jni::global_ref    -> gains is_stale(), raw_unsafe(); oop() is null when stale
vmhook::pin(oop)           -> global_ref             // uses the best available backend
vmhook::enable_pin_table(config) -> bool             // opt-in, returns false when unsupported
```

| Situation | Behaviour |
|---|---|
| ZGC / Shenandoah / unknown collector | `backend() == none`; `pin()` returns an empty ref; `oop()` null. **Never** a raw address. |
| Counter unreadable | `is_stale()` always true; `oop()` always null |
| Not on a JavaThread in `_thread_in_Java` | `pin()` refuses (empty ref) and logs once |
| Counter changed between read and store | `pin()` aborts before storing; empty ref |
| No free C-heap root slot | falls back to `detector_only` for that pin |
| Anchor slot found non-null / not ours | invalidate every outstanding pin, disable layer 3, log loudly |
| G1 ≤ 25 with layer 3 requested | refuse at `enable_pin_table` time, with the §6.3 reason in the log |

Every failure mode above degrades to *the current behaviour or better*, and none of them
writes a stale oop into a GC-visible slot — which is the one outcome that must be impossible.


---

## 11. Live JVM validation plan

Nothing here has prior art (§3.7), so every claim below is a hypothesis until the matrix
says otherwise. The repo already has the vehicles: `tests/jvm/modules/*.cpp` (83 modules,
self-registering via `VMHOOK_JVM_MODULE`, driven through `ctx.run_probe` on a real
JavaThread) and `.localci/run-local-ci.ps1` (Windows × {msvc, mingw, clang-cl} × 7 JDKs,
cached Temurin JDKs, ~5 parallel cells). GitHub's `jvm-*` matrix (JDK 8…26 × 3 OS) stays
authoritative.

### Phase 0 — read-only probes (no writes, zero risk). Ship these first.

New module `gc_root_probe.cpp`, all checks `[INFO]`-recorded rather than hard-asserted on
first landing, run on every cell to build a per-JDK/per-collector fact table:

| Probe | Records |
|---|---|
| `iterate_struct_entries` for every entry named in this document | present/absent per JDK — the machine-readable version of §3/§6 |
| `Klass::_java_mirror` `type_string` | `oop` vs `OopHandle` per JDK (confirms the JDK-11 cut) |
| `BarrierSet::_barrier_set` → `CardTableBarrierSet::_card_table` → `CardTable::_byte_map_base` (JDK 11+); `CardTableModRefBS::byte_map_base` via `oopDesc::_bs` / `CollectedHeap::_barrier_set` (JDK 8) | that the chain resolves and the base is a plausible, readable address |
| `byte_map_base[(heap_start) >> 9]` and `[(heap_end-8) >> 9]` readable | that the derived card address arithmetic lands inside the byte map |
| Card value of a slot in a known-old object vs a known-young object | empirical `clean`/`dirty`/`g1_young` values per collector, cross-checked against `declare_constant(CardTable::clean_card/dirty_card)` and `G1CardTable::g1_young_gen` where exported |
| `CollectedHeap::_total_collections` before/after a probe-driven `System.gc()` and after a young-GC-inducing allocation loop | **whether the counter moves for every relocating collection**, per collector — the single most important fact for §8 |
| `CollectedHeap::_is_gc_active` / `_is_stw_gc_active` | readable, and observed true only inside a pause |
| `JavaThread::_thread_state` inside a detour | that it is `_thread_in_Java` (8) on every JDK/toolchain — the premise of §7 |
| Flag-table walk (`Flag`/`JVMFlag` `flags`+`numFlags`+`_name`+`_addr`) | that `UseG1GC`/`UseSerialGC`/`UseParallelGC`/`UseZGC`/`UseShenandoahGC`/`UseCompressedOops` are readable per JDK |
| Anchor discovery: count of loaded classes with a null `static [Ljava/lang/Object;` field | whether the §4.1 auto-discovery has any candidates in a real app |

Phase 0 alone is worth landing: it turns every "should be present" in this document into a
CI-verified fact, and it is pure reads.

### Phase 1 — the detector (§8), hard-asserted

Extend `tests/jvm/modules/global_ref.cpp` (which today still documents the JNI contract):

* pin an object, force `System.gc()` via the probe, assert `oop()` returns **null** and
  `is_stale()` is true — i.e. the failure is now *detected*, on every collector, on every
  OS. This is a hard assert and it is safe: nothing is dereferenced.
* assert a pin taken and read with no intervening GC returns the identical address (no
  false-positive staleness) across a few thousand iterations.
* assert the counter is monotonic and never regresses under a concurrent allocation storm.

Phase 1 replaces the current Windows-wide skip of the survive-GC phase (that module
compiles out all of phase 2 on Windows because dereferencing a relocated JNI pin crashed
the suite) — with a detector there is nothing to dereference.

### Phase 2 — the pin (§10), heavily gated

Land behind an opt-in (`vmhook::enable_pin_table(...)`), default **off**, and validate in
this order:

1. **Barrier-only test, no pin.** Store a freshly TLAB-allocated `Object[]` into an
   anchor's static field, dirty the card, then force 20 young GCs and a full GC and verify
   the array is still there, its klass is intact, and its elements read back. Do this
   *first* on Serial and Parallel (where a raw dirty card is sufficient), then on G1. A
   failure here is the whole design failing, and it fails visibly (crash or garbage klass)
   rather than subtly.
2. **Round-trip — and this is the only test that actually proves anything.** The cheap,
   decisive assertion is that **the address CHANGED and the object is still ours**:
   * allocate a probe object with a unique sentinel field (`0x5A5A…`), pin it, record
     `addr_before = pin.raw_unsafe()`;
   * drop every other reference so the pin is the sole keep-alive;
   * force relocation from the Java side — `System.gc()` is only a hint, so drive it with a
     probe loop that allocates enough garbage to guarantee a young evacuation, and assert the
     epoch counter actually moved (§8) before drawing any conclusion;
   * assert `pin.oop() != nullptr`, then `addr_after = pin.oop()`, then **read the sentinel
     through `addr_after` and assert it is still `0x5A5A…`**.
   * Record `addr_after != addr_before` as the **relocation-observed** signal. If it is
     `[INFO] addresses identical`, the run proved nothing — the object simply did not move
     (very common for a promoted or non-evacuated object), and the test must be *retried*
     with more allocation pressure rather than counted as a pass. A suite where
     `addr_after != addr_before` never once fires across the whole matrix is a suite that is
     not testing the feature.
   * Control arm, run in the same detour: a second probe object held **only** as a raw
     `oop_t` in native memory. After the same GC, that raw address must be observed to be
     stale — detected via the epoch, never by dereferencing it. The pair
     (pinned survives / unpinned detected stale) is the actual proof.
   Prioritise this on **JDK 8 (G1 + Parallel), JDK 21 (G1), JDK 26 (G1, the JEP 522 shape)**
   — those three bracket every barrier and counter discontinuity found in this document
   (JDK 11 `CardTable` split, JDK 14 merge-heap-roots, JDK 18 flexible card size, JDK 21
   `_is_stw_gc_active` + dropped card constants, JDK 26 JEP 522). Serial and Parallel should
   be run explicitly (`-XX:+UseSerialGC` / `-XX:+UseParallelGC`) rather than left to the
   default, because they are the only configurations where layer 3 is legal.
3. **Anchor-loss recovery**: null the anchor slot from Java mid-test and assert every pin
   invalidates cleanly instead of dereferencing garbage.
4. **Negative gating**: run one cell with `-XX:+UseZGC` and one with
   `-XX:+UseShenandoahGC` and assert `enable_pin_table` **refuses** and every pin is empty.
5. **Soak**: the existing suite, with the pin table enabled and a background allocation
   thread, for the full run — the failure mode of a missed card is a delayed crash, so the
   test that matters is a long one, not a targeted one.

Run everything through `.localci` (mingw × all 7 JDKs) before GitHub — per the repo's own
history that pre-flight is what catches JDK-variance. Note the documented `.localci` blind
spots apply here too: MinGW misses MSVC-ABI failures, and POSIX cells are not covered
locally at all, which matters because the card-table and CLD paths are the same code on
POSIX but the fault behaviour of a bad write is not (`os::safe_write` is
`process_vm_writev` there, and there is no SEH containment on MinGW/clang-cl).

**Explicit non-goal:** do not enable the pin by default until the G1 result in step 1 is
green on all of JDK 8, 11, 17, 21, 25, 26 on both Windows and Linux. A missed card mark
does not fail the test that caused it; it corrupts the heap for whatever runs next.


---

## 12. Latent issues in the current header found during this research

These are pre-existing issues in `vmhook.hpp` that this research surfaced. They are
independent of whether the pin is ever implemented; two of them are the same
heap-corruption class the pin design has to solve.

1. **`field_proxy::store_object_oop` (14621) performs a barrier-less reference store into a
   Java object.** It writes a narrow oop into `mirror + offset` (static) or
   `object + offset` (instance) with `os::safe_write` and no card mark. Its own doc-comment
   says *"the field slot is itself a GC root once the reference lands in it"* — that is
   **false** for a slot inside a Java object. If the target object is in the old generation
   (a class mirror almost always is, after startup) and the stored value is a young object
   (anything `make_java_string` / `make_java_array` just allocated is, by construction),
   then with **Serial/Parallel** the reference is invisible to the next young collection
   and the referent is reclaimed while referenced; with **G1** it is worse (see §6.3, the
   card is a filter *and* an enqueue is required pre-JDK-16). `store_string()` (14691) is
   the shipping caller, so `field.set(std::string)` on a String field is exposed to this
   today. **Fix = the same `dirty_card()` helper the pin needs.**
2. **`store_object_oop` assumes compressed oops unconditionally** — it always writes a
   `std::uint32_t`. With `-XX:-UseCompressedOops` (or a heap > 32 GB, where HotSpot turns
   compression off automatically) that writes 4 bytes into an 8-byte reference slot,
   corrupting half of a live pointer. `encode_oop_pointer` cannot detect the mode either:
   its `_narrow_oop._base/_shift` entries exist regardless of whether compression is on.
   Detection must come from the flags table (§6.4) or from `heapOopSize`/`oopSize` in
   `gHotSpotVMIntConstants`. The same assumption is baked into `make_java_array`'s
   caller-supplied `element_size` and the hardcoded `array_header_size = 16` and
   `+12` length offset (13054, 13068).
3. **`jni::global_ref`'s documentation still describes the JNI implementation.** The
   class comment (19687-19724) says it *"pins the object via NewGlobalRef"* and that
   `oop()` *"always reads the object's CURRENT (post-relocation) address out of the handle
   slot"*, while the implementation (19730-19771) stores a raw oop and returns it verbatim.
   Only the `oop()` doc-comment carries the correction. Any reader of the class-level
   comment is actively misled about lifetime guarantees. The JVM test module
   `tests/jvm/modules/global_ref.cpp` likewise still documents and exercises the
   `NewGlobalRef`/`DeleteGlobalRef` contract.
4. **`make_java_object` (12910) can allocate out of *another thread's* TLAB.** The fallback
   walk (`find_any_java_thread`, `allocate_from_threads_list`) bumps `ThreadLocalAllocBuffer::_top`
   on threads that are running concurrently, with a plain non-atomic read-modify-write. Two
   allocators (us and the owning thread's inlined allocation sequence) can hand out the same
   bytes. This is orthogonal to the pin but is in the same "raw heap write from the wrong
   thread" family, and the pin's no-safepoint-window rule (§7) does **not** protect against
   it — that rule stops GC, not other mutators.

