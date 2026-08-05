# vmhook

[![CI](https://github.com/xxxnpno/vmhook/actions/workflows/ci.yml/badge.svg?branch=master)](https://github.com/xxxnpno/vmhook/actions/workflows/ci.yml)
[![License](https://img.shields.io/badge/License-See%20LICENSE.txt-blue)](LICENSE.txt)
[![Single header](https://img.shields.io/badge/Single%20header-vmhook.hpp-brightgreen)](vmhook/ext/vmhook/vmhook.hpp)
[![C++ Standard](https://img.shields.io/badge/C%2B%2B-20%20%2F%2023-blue)](https://en.cppreference.com/w/cpp/23)

**Read, call and hook a running HotSpot JVM from C++, from inside the process.**

One header. No JVMTI agent, no `-agentlib`, no cooperation from the target. You
inject a DLL or `.so`, and from that moment Java objects are things you can read
fields off, call methods on, allocate, and intercept.

```cpp
#include <vmhook/vmhook.hpp>

class minecraft : public vmhook::object<minecraft> { using object::object; };

vmhook::register_class<minecraft>("net/minecraft/client/Minecraft");

// a static field, then an instance field off it
auto mc     = minecraft::static_field("theMinecraft")->get().to_borrowed<minecraft>();
auto player = mc->get_field("thePlayer")->get();

// intercept a method: the lambda runs every time Java calls it
auto hook = vmhook::scoped_hook<minecraft>("runTick", "()V",
    [](vmhook::return_value&, vmhook::borrowed<minecraft>) noexcept {
        // on the game thread, inside the call
    });
```

---

## Contents

- [What it is for](#what-it-is-for)
- [How it works](#how-it-works)
- [The JNI boundary — read this](#the-jni-boundary--read-this)
- [Calling Java, and which thread you are on](#calling-java-and-which-thread-you-are-on)
- [Supported JVMs](#supported-jvms)
- [Getting started](#getting-started)
- [Guide](#guide)
- [Safety rules](#safety-rules)
- [Example, tests and CI](#example-tests-and-ci)

---

## What it is for

Tooling that lives **inside** a process you control and needs the JVM's real
state — game tooling, live debugging, instrumentation, research.

| You want to | vmhook | JVMTI | JNI |
|---|---|---|---|
| Attach to an already-running VM, no launch flags | **yes** | needs `-agentlib` at launch | needs a loaded native lib |
| Read a private field, no reflection | **yes** | `GetFieldID` + capability | reflection or JNI ids |
| Intercept a Java method | **yes**, per method | `RetransformClasses` | no |
| See who the caller was | **yes** | limited | no |
| Enumerate every instance of a class | **yes** | `IterateOverHeap` | no |

If you can add `-agentlib` and want stability guarantees, **use JVMTI**. vmhook is
for when you cannot, and it trades version-stability for that.

## How it works

HotSpot exports two tables describing its own memory layout:

- **`gHotSpotVMStructs`** — what the Serviceability Agent reads: field offsets,
  static addresses, type sizes.
- **`jvmciHotSpotVMStructs`** — a second, richer table published for the Graal
  compiler, containing things the first one omits.

vmhook resolves everything from those tables **by name, at runtime**. No offset is
hardcoded, so a VM whose layout it has never seen either works or fails cleanly.
Every read goes through a fault-safe probe (`ReadProcessMemory` /
`process_vm_readv`), so a stale pointer reports "unreadable" instead of killing
the VM.

Hooking patches a method's **interpreter entry** (`Method::_i2i_entry`) to a
generated trampoline and holds `NO_COMPILE` on the method so the JIT cannot route
around it. Hot methods must be deoptimised first, or the hook installs
successfully and silently never fires.

## The JNI boundary — read this

Earlier versions claimed "no JNI". **That claim is retired, and why matters.**

Everything is pure VMStructs work **except two things**, both about threads, both
unavoidable:

| What | Why there is no alternative |
|---|---|
| `AttachCurrentThreadAsDaemon` (JavaVM invocation table) | A `JavaThread` cannot be manufactured. It must be constructed, registered under `Threads_lock`, and given a `java.lang.Thread` oop. VMStructs publishes offsets, not constructors. |
| `NewStringUTF`, `Call<T>MethodA` and a few id/field lookups — **only when calling Java from a thread that is not inside a hook** | On a modern JVM there is no sound way to enter `_thread_in_Java` from outside without it. See below. |

**Inside a hook, no JNI is used at all.** The thread is already `_thread_in_Java`
and calls go through vmhook's own call stub. The JNI path exists solely for the
off-hook case, and only where the pure path cannot be made safe.

Both are reached **by index into the function table**, the same way the invocation
table already is. `jni.h` is never included and no JNI type is declared, so vmhook
still builds on a machine with no JDK.

## Calling Java, and which thread you are on

This is the part that bites people, so it is spelled out.

**Reading is free.** A field get is a load from an address. It works from any
thread and always has.

**Calling is not.** A call needs a `JavaThread`: the VM-side object holding the
frame anchor a GC stack-walk follows and the state a safepoint reads. A native
thread HotSpot has never seen has none of that, and calling on one corrupts the
heap the first time a collection runs during the call.

### Inside a hook — always correct

```cpp
auto hook = vmhook::scoped_hook<player>("hurt", "(F)V",
    [](vmhook::return_value& ret, vmhook::borrowed<player> self, float amount) noexcept {
        self->call<void>("heal", amount);   // fine: already _thread_in_Java
        ret.cancel();                        // and the original never runs
    });
```

### From your own thread — take a scope

```cpp
void worker() {                       // an ordinary std::thread
    const vmhook::java_thread_scope java{};
    if (!java) { return; }            // ALWAYS check

    auto p = find_player();           // resolve AND call in ONE scope
    p->call<void>("sendChatMessage", "hi");
}
```

`java_thread_scope` attaches the thread if needed and holds it where **no
collection can begin**, so an object resolved at the top of the scope is still
valid at the bottom. Two scopes throw that guarantee away.

Keep it short: while it is open, the VM waits for this thread at safepoints.

### Why this is hard, and what was measured

Moving from `_thread_in_native` to `_thread_in_Java` requires HotSpot's own
transition, which calls `SafepointMechanism::process_if_requested` — that blocks
if a safepoint is running. The function is `inline`, and on Windows `jvm.dll`
exports only C++ vtables plus the `JNIEXPORT` C ABI, so it cannot be borrowed.
Reproducing it needs one bit: *has a safepoint begun?*

| Signal | Verdict |
|---|---|
| `os::_polling_page` | **Sound.** Armed before any thread is examined. Measured **0 violations in 8922 gated entries** on Temurin 8 under 2860 collections/sec. Present JDK 8–20, removed in 21, and exported-but-**NULL** on some 17 builds — so the *value* must be checked, never the presence of the entry. |
| `CollectedHeap::_is_gc_active` | **Not sufficient.** Goes true only after synchronisation completes, so a clear reading does not mean "no safepoint starting". Measured **55 violations in 26463 entries**. |
| Per-thread poll word (JVMCI `JavaThread::_poll_data`) | **Not sufficient.** A thread parked in native rests *armed*, so the word only works as an edge detector and misses a safepoint that began before the disarm. Measured **6 violations in 26463 entries** on Zulu 17. |
| `SafepointSynchronize::_state` | Answers it exactly, and is published **nowhere**: absent from vmStructs on every JDK 8–25, absent from the JVMCI table, not exported on Windows. A scan of all 89,230 plausible slots in `jvm.dll`'s writable data found no candidate. |

Once a safepoint has counted a thread as safe, it **cannot be un-counted**. So on
a JVM without a usable polling page there is no sound pure-VMStructs answer — and
that is exactly where the JNI path takes over, because JNI's own entry performs
the real, safepoint-checked transition and its references are GC-tracked.

## Supported JVMs

| JDK | Read / hook | Call inside a hook | Call off a hook |
|---|---|---|---|
| 8 – 20 | yes | yes | yes — polling-page gate, pure VMStructs |
| 21 – 26+ | yes | yes | yes — minimal JNI path |

**Collectors.** Serial, Parallel, G1 and Epsilon are supported. **ZGC and
Shenandoah are refused** for off-hook calls: they relocate objects *outside*
safepoints, so no safepoint signal means anything there. Hooks still work.

**Platforms.** Windows and Linux, x86-64; MSVC, GCC and Clang.

## Getting started

Single header. Put `vmhook/ext/vmhook/vmhook.hpp` on your include path:

```cmake
add_library(vmhook INTERFACE)
target_include_directories(vmhook INTERFACE path/to/vmhook/ext)
target_link_libraries(your_target PRIVATE vmhook)
```

C++20 minimum, C++23 recommended. Nothing links against the JVM — symbols are
resolved from the loaded module at runtime.

### Build options

| Macro | Default | Effect |
|---|---|---|
| `VMHOOK_AUTO_ATTACH_THREADS` | `1` | Attach a thread automatically the first time it needs to call. `0` requires an explicit `java_thread_scope`. |
| `VMHOOK_LOG_FILE` | unset | Append logs to a file instead of stdout. |
| `VMHOOK_DEBUG_LOGS` | `!NDEBUG` | Verbose diagnostics. |

## Guide

### Wrapping a class

```cpp
class entity : public vmhook::object<entity> { using object::object; };
vmhook::register_class<entity>("net/minecraft/entity/Entity");
```

Register before using fields, methods, allocation or hooks.

### Fields

```cpp
float health = self->get_field("health")->get<float>();
self->get_field("health")->set(20.0f);

std::string name = self->get_field("name")->get().as_string();   // java.lang.String
auto rider       = self->get_field("passenger")->get().to_borrowed<entity>();

int count = entity::static_field("entityCount")->get<int>();     // statics via the type
```

### Methods

```cpp
self->call<void>("kill");
double d = self->call<double>("distanceTo", other);
```

Overloads are matched by descriptor; pass one when ambiguous:
`self->get_method("add", "(Ljava/lang/Object;)Z")`.

### Hooks

```cpp
auto hook = vmhook::scoped_hook<gui>("printChatMessage",
                                     "(Lnet/minecraft/util/IChatComponent;)V",
    [](vmhook::return_value& ret,
       vmhook::borrowed<gui> self,
       vmhook::borrowed<chat_component> line) noexcept {
        ret.set_arg(0, replacement);   // change what the original sees
        ret.cancel();                  // or stop it happening at all
    });
```

`scoped_hook` removes the hook on destruction. **Do not let that happen during
DLL unload** — see the safety rules.

Also available: `return_value::caller()` for caller info, `watch_static_field`
for field-change events, `on_class_loaded` for class-load events, and instance
enumeration for walking every live object of a class.

### References: raw oops, `borrowed`, `ref`

- **raw oop / `borrowed<T>`** — a bare address. Valid only for the duration of the
  hook invocation that produced it. Nothing roots it, nothing tracks relocation.
- **`ref<T>`** — remembers *how to find the object again* (a GC-stable root plus a
  path of field hops) and re-walks it on every dereference. It never returns an
  address a collection may have invalidated: it re-derives, or reports empty.

Prefer `ref` for anything held across a call.

## Safety rules

Not style suggestions — each corresponds to a crash that actually happened.

1. **No exception may reach the JVM.** Hook bodies are `noexcept` and catch-all
   guarded. The frame above you is HotSpot's interpreter; it has no handler.
2. **Check `java_thread_scope`.** A false scope means the call is not safe. There
   is no "try anyway" that is not a corrupted heap.
3. **Resolve and call in the same scope.** Between two scopes, objects move.
4. **Never block inside a scope.** No locks, no logging, no sockets, no waiting on
   another thread — the whole VM is queued behind it.
5. **Do not remove hooks during static destruction or DLL unload.** Removing a
   hook stops threads *entering* it; it cannot evict one already inside, and
   vmhook keeps no in-flight count. Unloading a module a game thread is executing
   raises a DEP violation on that thread and takes the process with it. If your
   tool supports detaching, prefer leaving the module mapped.
6. **Deoptimise before hooking a hot method**, or the JIT routes around the detour
   and it silently never fires.

## Example, tests and CI

The full example is `vmhook/src/example.cpp` with Java fixtures in
`example/vmhook`. It covers field access for every primitive plus strings, arrays
and object references; overload resolution; instance and static hooks; forced
returns and cancellation; `set_arg`; allocation; superclass lookup;
`List`/`Set`/`Map` conversion; enum singletons; interface default methods; nested
and inner classes; throwing methods; caller info; and the field-change and
class-load watchers.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Most tests run with **no JVM present** — they exercise decoding, layout
resolution and the fault-safe reads against synthetic data. The JVM-dependent
ones start a VM in-process and are skipped when no JDK is found. CI builds the
harness against several JDK versions and fails on any `[FAIL]` line in
`test_results.txt`.

## Notes

- Cached HotSpot pointers assume process-lifetime injection; class unloading is
  not handled.
- Install hooks and call `shutdown_hooks()` from controlled native code paths.
- JVM updates can change HotSpot internals and may require compatibility fixes.

## Licence

See [LICENSE.txt](LICENSE.txt).

---

**A caution worth repeating.** vmhook reads and patches HotSpot internals. They
are not a supported interface and a JVM update can move anything. It resolves by
name rather than by hardcoded offset precisely so a change surfaces as a clean
failure rather than as corruption — but the cost of getting this wrong is
someone's process dying. Test against the exact VM you target.
