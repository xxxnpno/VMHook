# Design sketch — anchored references (`vmhook::ref<T>`)

> Library-side design for Goal B ("the user never manages an oop"). Written from the header's
> existing mechanisms; to be reconciled with `docs/research/consumer_requirements.md` and
> `docs/research/gc_root_feasibility.md` before implementation.

## The problem in one paragraph

Every heap address vmhook hands out today is valid only until the next relocating GC.
`object_base` stores a raw `oop_type_t` (`vmhook.hpp:15978-16037`); wrappers are handed around
as `std::unique_ptr<T>`; and `jni::jni::global_ref` — the type whose entire purpose is to fix
this — is a no-op that stores the same raw pointer and returns it forever
(`vmhook.hpp:19730-19760`). So the standard consumer pattern "find the object on tick N, use it
on tick N+1" is a use-after-relocation by construction, and the consumer is the one expected to
notice.

## The insight already in the codebase

Static field reads are *already* relocation-proof, with no pin and no JNI. `field_proxy`'s
static overload stores the **`Klass*`** (metadata — the GC never moves it) and the field offset,
then re-derives `mirror_klass->get_java_mirror() + offset` on every `get()`
(`vmhook.hpp:14135-14152`). `Klass::_java_mirror` is an `OopHandle` — an `oop*` into an
OopStorage slot that the GC **updates in place** on relocation (`vmhook.hpp:3705-3760`).

So the library already owns one relocation-proof root and knows how to read through it.
The whole design is: **generalise "GC-stable root + path" from static fields to everything.**

## The type

```cpp
namespace vmhook
{
    template<typename T = void> class ref;
}
```

`ref<T>` is a value type. It is copyable, movable, cheap, and has no destructor obligations
(nothing is pinned in the common case). It holds:

| member | purpose |
|--------|---------|
| `anchor` | how to find the object again from a root the GC maintains |
| `cached`  | last resolved address (a memo, never authoritative) |
| `epoch`   | the GC generation `cached` was resolved in |

### Anchor kinds

```
static_root(Klass* k, size_t offset)     // k->java_mirror() + offset   — always live
field_of(ref parent, size_t offset)      // parent.resolve() + offset
element_of(ref parent, size_t index)     // parent.resolve() + header + index*scale
pinned(slot)                             // a real GC root                 [feasibility TBD]
ephemeral(oop)                           // detour-scoped only; expires on GC
```

`static_root` is the base case and terminates every chain. `field_of` / `element_of` are
recursive, so a `ref` to a deeply nested object costs a few pointer reads to resolve and is
*always* correct — it re-walks live state rather than trusting a remembered address.

### Resolution

```cpp
auto resolve() const noexcept -> oop_t;   // nullptr if the chain is broken or the ref expired
```

* If `epoch == current_gc_epoch()` and `cached != nullptr`, return `cached`.
* Otherwise walk the anchor chain from the root, validating each hop with `is_valid_pointer`
  and `safe_read`, memoise, and stamp the epoch.
* `ephemeral` refs whose epoch is stale resolve to `nullptr` — **expired, not dangling.**

That last line is the whole safety argument: the only way to get a wrong answer today is to
trust a remembered address across a GC, and this design structurally cannot do that.

## The GC epoch

A monotonically-increasing counter that changes whenever objects may have moved. Candidate
sources (to be pinned down by the feasibility research): `CollectedHeap::_total_collections`,
`_total_full_collections`, `SafepointSynchronize::_safepoint_counter`. The counter needs only
two properties:

1. it never fails to change when a relocating collection happened (soundness), and
2. it is cheap to read (a single memory load).

Spurious changes are harmless — they only cost a re-walk. **A missed change is unsound**, so
where a collector cannot be observed reliably the epoch must be treated as always-changed
(re-walk every time) rather than never-changed.

## What the user writes

The `ref` type is the plumbing; the point is that it disappears at the call site.

```cpp
// today
auto* mc_klass = vmhook::find_class("net/minecraft/client/Minecraft");
void*  mc      = /* static field read, raw oop */;
vmhook::jni::global_ref pinned{ mc };           // no-op; silently stale after GC
// ... next tick, different thread ...
if (pinned) { auto player = read_field_chain(pinned.oop(), ...); }
```

```cpp
// target
auto mc = vmhook::statics<minecraft>();          // ref<minecraft>, anchored at a Klass root
// ... any number of ticks / GCs later, any thread ...
if (auto player = mc->player())                  // ref<entity_player_sp>, anchored to mc
{
    player->health() = 20.0f;
}
```

No pin. No `void*`. No `unique_ptr`. No validity question the user has to answer — `mc->player()`
returns something falsy if the chain is broken, exactly like a null check they already write.

## Integration with the existing model

* `object<T>` keeps working. Internally its instance becomes a `ref<T>` rather than a raw oop;
  `get_instance()` becomes `resolve()` and is marked as the raw escape hatch.
* Wrapper constructors currently take `vmhook::oop_t` (see README's `player`/`example` samples).
  They gain a `ref`-taking constructor; the oop-taking one stays for one version, deprecated.
* Collection wrappers (`list`/`set`/`map`/...) return `ref<T>` elements anchored with
  `element_of`, so an iterated snapshot survives a GC instead of rotting.
* Hook detours hand out `ephemeral` refs — correct by construction, since a detour frame's
  arguments genuinely are only valid for that call, and the epoch check makes leaking one out
  of the detour *fail loudly* instead of silently.

## Research answers (2026-08-04)

1. **Is a real 0-JNI relocation-tracking pin reachable?** **Yes**, on JDK 8-26 for
   Serial/Parallel/G1 — but *not* by the obvious route. Append to a **C-heap root list**
   (`ClassLoaderData::_handles` on 11-26, `JNIHandles::_global_handles` on 8). Those live
   outside the Java heap and are enumerated in full every GC, so **no write barrier is
   involved**. The `Object[]`-in-a-class-mirror design is barrier-blocked on G1 8-25: the
   post-barrier needs a dirty-card-queue enqueue VMStructs does not expose, and writing the
   card byte alone permanently suppresses the JVM's own barrier for that card.
2. **Which epoch counter is sound?** `Universe::_collectedHeap` + `CollectedHeap::_total_collections`,
   exported on **every** JDK 8-26. Verified at the source level: G1 young bumps it in
   `pre_evacuate_collection_set` *before* any copying; Serial and Parallel likewise. Sample it
   together with `_is_gc_active` / `_is_stw_gc_active` (renamed at JDK 21 — probe both names).
   `_total_full_collections`, `_gc_cause` and all of `SafepointSynchronize` are exported
   nowhere, on any JDK.
3. **Can a reference store honour the barriers with pure memory writes?** Only partly.
   "Write only into an already-null slot" is exactly sufficient to skip G1's SATB pre-barrier
   (it is the VM's own early-out). *Clearing* a slot is not — prefer leaking a slot to clearing
   one. The post-barrier is the blocker described in (1).

**The precondition that outranks all three:** the read-oop → store-oop pair must not straddle a
GC, and it does not only on a `JavaThread` in `_thread_in_Java` executing no safepoint poll —
proved from `safepoint_safe_with`, where `_thread_in_Java` hits `default: return false` and the
VM thread spins unbounded. **From a cold native thread the pin is unsound for every mechanism**,
including a hypothetical `NewGlobalRef`. The API must enforce this rather than document it.

**ZGC and Shenandoah are refused**, not merely unsupported: they relocate concurrently behind
load barriers, their counters tick before relocation, and vmhook's whole direct-memory model is
invalid there.
