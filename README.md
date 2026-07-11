# vmhook

[![CI](https://github.com/xxxnpno/vmhook/actions/workflows/ci.yml/badge.svg?branch=master)](https://github.com/xxxnpno/vmhook/actions/workflows/ci.yml)
[![License](https://img.shields.io/badge/License-See%20LICENSE.txt-blue)](LICENSE.txt)
[![Single header](https://img.shields.io/badge/Single%20header-vmhook.hpp-brightgreen)](vmhook/ext/vmhook/vmhook.hpp)
[![C++ Standard](https://img.shields.io/badge/C%2B%2B-20%20%2F%2023-blue)](https://en.cppreference.com/w/cpp/23)

vmhook is a C++20/23 single-header library for inspecting, wrapping, calling,
and hooking a running HotSpot JVM from native code. It is intended for injected
tooling that controls the target environment and needs direct access to HotSpot
metadata without shipping a JVMTI agent.

The library currently supports:

- SDK-style C++ wrappers for Java classes
- primitive, string, array, and object field access
- Java method calls with descriptor-aware overload matching
- Java object allocation through registered wrapper types
- interpreted Java method hooks
- hook return cancellation and forced return values
- caller-info introspection from inside any hook
- event-driven class-load notifications
- event-driven field-change notifications (Windows trap-based)
- hook argument replacement through `vmhook::return_value::set_arg`
- HotSpot metadata lookup — pure VMStructs + direct memory, no JNI or JVMTI

## Scope

JNI and JVMTI are the supported Java native interfaces. vmhook is lower-level:
it reads HotSpot VMStruct metadata, resolves JVM internals directly, and patches
interpreter entry stubs for hooks. Use it only when the target JVM and runtime
layout are part of your compatibility surface.

## Platform & toolchain matrix

The header compiles on every combination listed below.  Runtime hooking
needs a HotSpot JVM in-process plus an x86_64 ABI; the columns reflect
what CI actually exercises for each target.

| OS / Arch                | Compiler                  | Header builds | OS layer | Hook trampoline | Real-JVM tests |
|--------------------------|---------------------------|:-------------:|:--------:|:---------------:|:--------------:|
| Windows x86_64           | MSVC 19.36+               | yes           | yes      | yes (Win64 ABI) | yes            |
| Windows x86_64           | clang / clang-cl 20+      | yes           | yes      | yes (Win64 ABI) | yes            |
| Windows x86_64           | MinGW-w64 GCC 14+         | yes           | yes      | yes (Win64 ABI) | yes            |
| Linux x86_64             | GCC 14+                   | yes           | yes      | yes (SysV ABI)  | yes            |
| Linux x86_64             | Clang 18+                 | yes           | yes      | yes (SysV ABI)  | yes            |
| macOS x86_64             | Apple Clang               | yes           | yes      | yes (SysV ABI)  | best-effort    |
| macOS arm64              | Apple Clang               | yes           | yes      | no (arm64)      | best-effort    |
| Android (ARM64 / x86_64) | NDK clang                 | yes           | yes      | x86_64 only     | n/a (no HotSpot) |
| iOS (device / simulator) | Apple Clang               | yes           | yes      | no              | n/a (no HotSpot) |

Notes:
- *Hook trampoline* is x86_64-only.  On arm64 hosts the header still
  builds and the OS layer is fully functional; `vmhook::hook<T>` returns
  false at runtime so consumers can degrade gracefully.  An arm64
  trampoline is a tractable future addition, but on its own it does not
  unlock iOS or Android (see next bullet).
- *iOS and Android do not run HotSpot.*  iOS has no JVM at all; Android
  runs Java bytecode on ART (a completely different runtime with
  different internal structures).  vmhook reads HotSpot-specific data
  (`gHotSpotVMStructs`, `Klass`, `Method`, interpreter frame layout),
  so there is no path to "real-JVM tests" on either platform — the
  thing being tested doesn't exist there.  The CI cross-compile jobs
  verify the header is syntactically valid for those toolchains; at
  runtime `vmhook::find_class`, `vmhook::hook<T>`,
  `vmhook::watch_static_field`, and `vmhook::on_class_loaded` all
  return null / false / empty-handle.
- *Field watcher* (`vmhook::watch_static_field`) needs hardware data
  breakpoints, which we currently wire up only on Windows × x86_64.
  See `VMHOOK_HAS_HW_DATA_BREAKPOINTS`.
- The CI matrix runs the full JVM integration test for Java 8, 11, 17,
  21, 24, 25, and 26 against every compiler that produces a working
  artefact on a hosted runner.

### Building with CMake (recommended)

The repository ships a `CMakeLists.txt` that builds the example shared library,
the Windows-only injector, and the unit-test suite:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

CMake options:

- `VMHOOK_BUILD_EXAMPLE`  (default `ON`)  build the test shared library
- `VMHOOK_BUILD_INJECTOR` (default `ON`)  build the Windows-only injector
- `VMHOOK_BUILD_TESTS`    (default `ON`)  build standalone unit tests
- `VMHOOK_WARNINGS_AS_ERRORS` (default `OFF`)  enforce `-Werror`

Consumers can pull the library into their own CMake project:

```cmake
add_subdirectory(third_party/vmhook)
target_link_libraries(my_target PRIVATE vmhook::vmhook)
```

### Building with MSBuild (legacy)

```powershell
MSBuild vmhook.slnx /p:Configuration=Release /p:Platform=x64 /m
```

The MSBuild solution still works for the Visual Studio IDE workflow; the
GitHub Actions CI also builds it to catch regressions.

## Running the tests

Two test suites ship with the project:

1. **Standalone unit tests** (no JVM required)  built into `build/tests` by
   the CMake build, run via `ctest`.  Covers header compilation on every
   compiler/platform, ODR sanity across translation units, type-trait static
   asserts, the `vmhook::os` portability layer (page size, allocate/protect
   round-trips, safe pointer reads, etc.), and the public API surface.
2. **JVM integration tests**  the injector loads the example DLL into a
   running JVM and the DLL asserts every field/method/hook scenario before
   asking the JVM to exit.  Results land in `test_results.txt`; CI fails if
   any line starts with `[FAIL]`.  The integration suite covers:
   - Every primitive type (bool, byte, short, int, long, float, double, char)
     at regular and boundary values (MIN/MAX, NaN, +/- infinity, negatives).
   - String fields (regular, empty, unicode, long, interned, null).
   - 1-D arrays of every primitive and of String (regular, empty, large).
   - Object reference fields, including `final` and `volatile`.
   - Inheritance: own + inherited (protected) fields and methods.
   - Enums (`Color.RED/GREEN/BLUE` with constructor params and instance methods).
   - Interfaces: static methods on the interface (Animal.kingdomCount), method
     overrides on concrete implementations (Dog.speak overriding Animal).
   - Nested classes (static nested + non-static inner with synthetic outer
     reference).
   - Overloaded methods (same name, three signatures).
   - Each primitive return type plus void / null-returning methods.
   - Methods that throw exceptions.
   - Hooks: installation, force-return, cancel, arg mutation (int + string),
     method calling from inside the detour, `make_unique` allocating
     wrappers from a hook.
   - Container wrappers: `ArrayList`, `LinkedList`, `HashSet`,
     `LinkedHashMap`, `HashMap`, and `TreeMap` — each loaded with three
     deterministic entries, read back through the matching wrapper, and
     checked for size, element validity, and (for maps) key identity.

## Debug And Release Logging

`VMHOOK_DEBUG_LOGS` controls internal diagnostic logging.

- Debug builds enable vmhook logs by default.
- Release builds disable vmhook logs by default.
- Define `VMHOOK_DEBUG_LOGS=1` to force logs on.
- Define `VMHOOK_DEBUG_LOGS=0` to force logs off.

Release builds keep the same behavior, but avoid vmhook diagnostic output on hot
paths.

## Wrapper Pattern

Register each C++ wrapper with the Java internal class name, then keep field and
method names inside wrapper methods.

```cpp
#include <vmhook/vmhook.hpp>

namespace mapping
{
    namespace player
    {
        inline const char* clazz{ "net/minecraft/client/entity/EntityPlayerSP" };
        inline const char* send_chat_message{ "sendChatMessage" };
    }
}

class player : public vmhook::object<player>
{
public:
    explicit player(vmhook::oop_t instance) noexcept
        : vmhook::object<player>{ instance }
    {
    }

    auto send_chat_message(const std::string& message) const
        -> void
    {
        this->get_method(mapping::player::send_chat_message)->call(message);
    }
};

auto register_sdk()
    -> void
{
    vmhook::register_class<player>(mapping::player::clazz);
}
```

## Fields

Instance field access is `get_field("name")`.  Static field access has two
forms depending on the compiler you target:

- On **MSVC** and **Clang**, `get_field("name")` resolves correctly in both
  instance and static contexts: the C++23 deducing-this overload is excluded
  from static-call overload resolution, so the static fallback wins.
- On **GCC** the deducing-this overload is still considered viable in a
  static-call context and produces a compile error.  Call `static_field("name")`
  from static methods to stay portable across all three compilers.

```cpp
class example : public vmhook::object<example>
{
public:
    explicit example(vmhook::oop_t instance) noexcept
        : vmhook::object<example>{ instance }
    {
    }

    // Instance field accessor — works on every compiler.
    auto get_name() const
        -> std::string
    {
        return this->get_field("name")->get();
    }

    auto set_score(std::int32_t value) const
        -> void
    {
        this->get_field("score")->set(value);
    }

    // Static field accessor — pick one:
    //
    //   static_field("...")           portable: MSVC, Clang, GCC
    //   get_field("...")              MSVC and Clang only
    static auto get_total_count()
        -> std::int32_t
    {
        return static_field("totalCount")->get();
    }
};
```

Supported field values include bools, byte-sized and integral values, floats,
doubles, chars, strings, arrays as `std::vector<T>`, and registered object
references as `std::unique_ptr<wrapper_type>`.

## Methods

Same pattern as fields.  `get_method("name")->call(args...)` works for
instance access on every compiler.  Static access is `static_method(...)` for
portable code (or `get_method(...)` on MSVC / Clang).

```cpp
// Instance:
auto add_score(std::int32_t amount) const
    -> void
{
    this->get_method("addScore")->call(amount);
}

auto compute(std::int32_t value) const
    -> std::int32_t
{
    return this->get_method("compute")->call(value);
}

// Static (portable):
static auto reset_global_state()
    -> void
{
    static_method("resetGlobalState")->call();
}
```

## Collections and maps

vmhook converts a Java container field to a native `std::vector` (or
`std::vector<std::pair<...>>` for maps) in one line.  Two entry points
do all the work; both probe the live OOP and pick the right HotSpot
layout walk automatically.  No template argument on `get()` — the field
signature plus the runtime klass tell vmhook which fast path to take.

```cpp
auto vec   = get_field("foo")->get().to_vector<T>();        // Collection / List / Set
auto pairs = get_field("bar")->get().to_entries<K, V>();    // Map
```

| Java field type                              | Fast path used inside `to_vector` / `to_entries`              |
|----------------------------------------------|---------------------------------------------------------------|
| `ArrayList<E>` / any `List<E>` with `elementData` | Direct read of the backing `Object[]`                    |
| `LinkedList<E>`                              | `first → next` Node-chain walk (O(N) instead of O(N²))        |
| `HashSet<E>` / `LinkedHashSet<E>`            | Backing `HashMap.table` bucket walk, keys collected           |
| `TreeSet<E>`                                 | Backing `TreeMap.root` in-order walk, keys collected          |
| Any other `Collection<E>`                    | `size()` + `get(int)` Java-side calls (only valid for List)   |
| `HashMap<K,V>` / `LinkedHashMap<K,V>`        | `table` bucket walk over `Node{key, value, next}`             |
| `TreeMap<K,V>`                               | `root` red-black in-order walk                                |

End-to-end examples:

```cpp
// Java:  public List<A>           listOfAs;
// Java:  public LinkedList<A>     chainOfAs;
// Java:  public HashSet<A>        setOfAs;
// Java:  public HashMap<String,A> mapOfAs;
// Java:  public TreeMap<String,A> treeMapOfAs;

auto v1     = get_field("listOfAs")   ->get().to_vector<a_class>();
auto v2     = get_field("chainOfAs")  ->get().to_vector<a_class>();
auto v3     = get_field("setOfAs")    ->get().to_vector<a_class>();
auto pairs1 = get_field("mapOfAs")    ->get().to_entries<string_w, a_class>();
auto pairs2 = get_field("treeMapOfAs")->get().to_entries<string_w, a_class>();
```

The same flow works inside a hook detour — declare a wrapper-typed
parameter to express intent, then call `to_vector` / `to_entries`:

```cpp
vmhook::hook<example_class>("acceptMap",
    [](vmhook::return_value&,
       const std::unique_ptr<example_class>& self,
       const std::unique_ptr<vmhook::map>& m)
    {
        for (auto& [k, v] : m->to_entries<key_w, value_w>())
        {
            // ...
        }
    });
```

`vmhook::list`, `vmhook::linked_list`, `vmhook::set`, `vmhook::map`,
and `vmhook::hash_map` are thin type tags — they exist so callers can
declare exactly what shape they expect (especially as hook parameters);
the actual heap-layout dispatch always happens in
`collection::to_vector` and `map::to_entries`.

`to_vector` / `to_entries` return empty containers (never throw) when
the container is null, the layout is unrecognised, or the field is
missing.  Null Java elements become `nullptr` slots in the result.

## Hooks

Hooks receive `vmhook::return_value&` first, followed by decoded Java arguments.
For instance methods, include the wrapped Java `this` argument first.

```cpp
vmhook::hook<player>(mapping::player::send_chat_message,
    [](vmhook::return_value& return_value,
       const std::unique_ptr<player>& self,
       const std::string& message)
    {
        if (message.starts_with("/native"))
        {
            return_value.cancel();
        }
    });
```

Force a return value:

```cpp
vmhook::hook<example>("compute",
    [](vmhook::return_value& return_value,
       const std::unique_ptr<example>& self,
       std::int32_t value)
    {
        return_value.set(static_cast<std::int32_t>(12345));
    });
```

Replace an argument and allow the original Java method to continue:

```cpp
vmhook::hook<player>(mapping::player::send_chat_message,
    [](vmhook::return_value& return_value,
       const std::unique_ptr<player>& self,
       const std::string& message)
    {
        if (message.contains("secret"))
        {
            return_value.set_arg(1, std::string{ "[redacted]" });
        }
    });
```

`set_arg` accepts primitive values, object wrappers, object pointers, string
views, strings, and C strings. Java strings are built directly from the VM heap
(TLAB allocation + UTF-16 `value`/`coder` field encode) with no JNI.

Remove installed hooks before unloading:

```cpp
vmhook::shutdown_hooks();
```

## Caller information from inside a hook

`return_value::caller()` walks the saved-rbp chain on the interpreter
stack and reports the method that invoked the currently-hooked one.

```cpp
vmhook::hook<my_class>("innerStep",
    [](vmhook::return_value& ret,
       const std::unique_ptr<my_class>& self,
       std::int32_t value)
    {
        const auto info{ ret.caller() };
        if (info.valid())
        {
            // info.class_name  == "vmhook/CallerProbe"
            // info.method_name == "outerStep"
            // info.signature   == "(I)I"
            // info.method      == raw vmhook::hotspot::method* (cached for
            //                     further calls to method_proxy etc.)
        }
    });
```

`caller()` returns an empty `caller_info` (`valid() == false`) when the
caller frame is not interpreted (compiled, native, or unidentifiable).
Only the immediate caller is exposed; walking deeper requires reading
saved-rbp chains manually starting from `ret.frame()`.

## Watching a static field for changes

`vmhook::watch_static_field<T, value_t>(name, callback)` installs a
**hardware data breakpoint** (one of `DR0`-`DR3`) on the field's
address.  The trap fires *instantly* on every write — no polling, no
idle CPU.  The callback runs inside a vectored exception handler on
whichever thread issued the write.

```cpp
auto watcher{ vmhook::watch_static_field<my_class, std::int32_t>(
    "tickCount",
    [](std::int32_t /*prev*/, std::int32_t next)
    {
        std::println("tickCount written: {}", next);
    }) };

// ... do work ...

watcher.stop();   // optional; happens automatically on scope exit
```

**Platform support**: Windows × x86_64 only.  On any other platform the
function logs an error and returns an empty `watch_handle`
(`watcher.running() == false`) — there is no polling fallback.  Check
the `VMHOOK_HAS_HW_DATA_BREAKPOINTS` macro at compile time to gate
features that depend on the trap.

**Limits**:
- At most 4 concurrent watches per process (one per debug register).
- Threads created *after* the watch is installed do not have the trap
  armed.  Threads alive at install time get it.
- The callback receives `(old, new)` where `old` is zero-initialised —
  the CPU does not save the pre-write value, so the previous value
  cannot be reconstructed from inside the trap handler.  If you need it,
  capture it before installing the watch.
- The callback must not allocate Java objects or call back into the JVM
  (those require a JavaThread and a safe-point window).

## Stack traces from inside a hook

`return_value::stack_trace(max_depth = 64)` walks the saved-rbp chain
the same way `caller()` does, but keeps going as long as each frame
passes the safe-pointer checks.  It returns a `std::vector<caller_info>`
ordered from immediate caller (index 0) outward.

```cpp
auto detour = [](vmhook::return_value& ret,
                 const std::unique_ptr<my_class>& self)
{
    for (auto const& frame : ret.stack_trace())
    {
        VMHOOK_LOG("  at {}.{}{}",
                   frame.class_name, frame.method_name, frame.signature);
    }
};
```

The walk terminates when it hits a compiled or native frame (those
don't follow the interpreter layout), so the trace covers only the
interpreted portion of the stack.

## Hooking exception construction

`vmhook::on_exception(callback)` installs a method hook on
`java.lang.Throwable::fillInStackTrace()` and fires the callback every
time *any* Throwable (or subclass) is constructed through one of the
public constructors.  Catches `NullPointerException`, `IOException`,
`IllegalArgumentException`, your own exceptions, etc.

```cpp
auto watcher{ vmhook::on_exception(
    [](const std::string& internal_name)
    {
        if (internal_name == "java/lang/NullPointerException")
        {
            VMHOOK_LOG("NPE constructed!");
        }
    }) };
```

The callback receives the throwable's *dynamic* class name (read from
the oop's narrow-klass header, not the static `Throwable` type).
Misses Throwables built with `writableStackTrace=false` and any
subclass that overrides `fillInStackTrace` to a no-op.

## RAII hook removal

`vmhook::hook<T>(name, callback)` installs a hook that lives until
`vmhook::shutdown_hooks()` tears it down.  For cases where you want
the hook bound to a C++ scope, `vmhook::scoped_hook<T>(name, callback)`
returns a `hook_handle` that uninstalls **just that hook** when it
goes out of scope:

```cpp
{
    auto handle{ vmhook::scoped_hook<my_class>(
        "doThing",
        [](vmhook::return_value&,
           const std::unique_ptr<my_class>&,
           std::int32_t)
        {
            VMHOOK_LOG("doThing was called");
        }) };

    // ... run probes ...
}   // handle destructed -> hook removed; doThing dispatches normally again
```

The handle is move-only, exposes `installed()` to check success, and
its destructor restores the method's original entry points and clears
the no-inline / no-compile flags.  Other hooks are unaffected;
`shutdown_hooks()` still works as a hard reset.

Caveat: hook removal isn't synchronised with in-flight callbacks.
Make sure no Java thread is currently inside the hooked method when
the handle dies — typically that's the case if you only call
`stop()` after `run_java_probe()` returns.

## Walking every instance of a class

`vmhook::for_each_instance<T>(visitor, max_visits)` scans the live
heap and invokes the visitor with a fresh `std::unique_ptr<T>` for
every object whose narrow-klass pointer matches `T`'s registered
class.  Useful for "give me every loaded `Player`" without installing
a constructor hook.

```cpp
vmhook::for_each_instance<player_class>(
    [](std::unique_ptr<player_class> p)
    {
        VMHOOK_LOG("player at {}", p->name());
    },
    /* max_visits = */ 256);
```

Reads `Universe::_collectedHeap::_reserved` in 4 KiB chunks and walks
candidate object headers at 8-byte stride.  The JVM is **not**
brought to a safepoint, so:

- Concurrent GCs may move objects between when we see them and when
  the visitor accesses them — don't keep the wrappers past the
  call's return.
- Region-based GCs (G1) skip unmapped regions silently; you'll see
  every accessible match but possibly miss objects in regions whose
  safe-read fails.
- Colored-pointer GCs (ZGC, Shenandoah) are unsupported — prefer a
  constructor hook there.

The scan is O(heap-size) — typically 0.5–2 s on a 4 GiB heap.
`max_visits` short-circuits as soon as you have enough.

## Catching already-inlined callers of a hook

`vmhook::hook<T>(...)` patches the hooked method's interpreter entry.
That patch only fires while the method is dispatched through the
interpreter.  If HotSpot has already JIT-compiled a *caller* of the
hooked method and inlined the callee's body directly into the caller's
nmethod, the dispatch is compiled away — calls reaching the hooked
method through that inlined call site bypass the hook entirely.

vmhook's per-hook install already deoptimises the hooked method itself,
which catches direct callers, but it can't reach inlined call sites
that were JIT-compiled before the hook went in.  Two helpers close that
gap:

```cpp
// After installing whichever hooks you need:
vmhook::deoptimize_all_jit_compiled_methods();
```

Throws away every nmethod in the JVM and lets HotSpot recompile from
scratch.  Because each hooked method is marked `_dont_inline` at install
time, the re-compiled callers no longer inline the hooked body —
every dispatch routes through the patched interpreter entry.  Heavy:
expect a brief CPU spike right after the call while HotSpot warms back
up.  Typically called once after installing all hooks at startup.

For a narrower sweep, pass a predicate:

```cpp
// Catch already-inlined Minecraft callers of any chat hook,
// without touching JIT'd JDK / library code:
vmhook::deoptimize_methods_if(
    [](const std::string& class_name, vmhook::hotspot::method*)
    {
        return class_name.starts_with("net/minecraft/");
    });
```

Both helpers use the same atomic Method-side deopt dance as the
per-hook install (set entry points to interpreter, then clear `_code`),
so they have the same race properties as `vmhook::hook<T>(...)` —
exercised continuously in production.  Methods whose c2i adapter
cannot be recovered are skipped rather than left in an inconsistent
state.

## Enumerating Java threads

`vmhook::for_each_thread(visitor)` walks HotSpot's live thread list
(`Threads::_thread_list` on JDK 8/9, `ThreadsSMRSupport::_java_thread_list`
on JDK 10+) and invokes the visitor with a `thread_info` snapshot for
every JavaThread the JVM currently owns:

```cpp
vmhook::for_each_thread([](const vmhook::thread_info& t)
{
    VMHOOK_LOG("tid={} state={} jt={:p}",
               t.os_thread_id,
               static_cast<int>(t.state),
               reinterpret_cast<void*>(t.thread));
});
```

`thread_info::thread` is the underlying `vmhook::hotspot::java_thread*`
— hand it back to vmhook's helpers (`get_thread_state`,
`get_suspend_flags`, etc.) for deeper introspection.  Treat it as
valid only for the duration of the visit; the JVM may reclaim a
JavaThread once the corresponding Java thread exits.

## Reading a Java String directly

`vmhook::read_java_string(oop)` decodes a `java.lang.String` to a
UTF-8 `std::string` without needing to register `java/lang/String`
as a wrapper.  Handles both pre-Java-9 `char[]` and Java-9+
`byte[] + coder` layouts:

```cpp
const std::uint32_t compressed{
    field_proxy->get_compressed_oop() };
const std::string text{
    vmhook::read_java_string(vmhook::hotspot::decode_oop_pointer(compressed)) };
```

Truncates at 4 KiB as a sanity guard.  Returns an empty string on
failure.

## Enumerating loaded classes

`vmhook::for_each_loaded_class(visitor)` snapshots the JVM's
`ClassLoaderDataGraph` and invokes the visitor once per Klass that is
currently reachable.

```cpp
vmhook::for_each_loaded_class(
    [](const std::string& name, vmhook::hotspot::klass*)
    {
        if (name.starts_with("net/minecraft/"))
        {
            VMHOOK_LOG("loaded: {}", name);
        }
    });
```

It's a *snapshot* — classes loaded later won't be visited.  For live
notifications, use `vmhook::on_class_loaded` (which is event-driven on
`ClassLoader.defineClass`).

## Watching for newly-loaded classes

`vmhook::on_class_loaded(callback)` installs a **method hook on
`java.lang.ClassLoader::defineClass`** and fires the callback every
time a class is defined through the Java-side entry point.  This is
event-driven: zero latency, zero idle CPU.

```cpp
auto watcher{ vmhook::on_class_loaded(
    [](const std::string& internal_name)
    {
        std::println("loaded: {}", internal_name);
    }) };
```

`internal_name` uses JVM `/`-separated form (e.g.
`"net/minecraft/client/Minecraft"`).

**Limits**:
- Catches only classes defined through `ClassLoader.defineClass`.
  Bootstrap-loaded classes (`java.*`, `javax.*`, `sun.*`) bypass that
  entry point and are not reported.
- The `(String, ByteBuffer, ProtectionDomain)` overload of
  `defineClass` is not yet hooked.
- The callback runs on the Java thread that triggered the definition,
  with the JVM mid-class-loading — keep it short.

## Object Construction

`vmhook::make_unique<T>(args...)` allocates a Java object for a registered
wrapper type. If the wrapper exposes `construct(args...)`, vmhook calls it after
allocation.

```cpp
class chat_component_text : public vmhook::object<chat_component_text>
{
public:
    explicit chat_component_text(vmhook::oop_t instance) noexcept
        : vmhook::object<chat_component_text>{ instance }
    {
    }

    auto construct(const std::string& text) const
        -> void
    {
        this->get_method("<init>")->call(text);
    }
};

auto component{ vmhook::make_unique<chat_component_text>("hello") };
```

## Class Lookup

vmhook resolves classes purely through HotSpot metadata, with no JNI. It walks
the ClassLoaderDataGraph (every ClassLoaderData's loaded-class list, plus the
SystemDictionary on JDK 8-17) entirely via VMStructs, and resolves array classes
(`[I`, `[Ljava/lang/String;`, …) through the `Universe` primitive-array klasses
and the `InstanceKlass::_array_klasses` / `ArrayKlass::_higher_dimension` chain.

There is no `FindClass` / `loadClass` fallback: lookup finds any class that is
already **loaded** (under any loader) but does not itself trigger class loading.
In custom class-loader environments, pin the right loader's copy against a live
anchor object with `find_class_via_oop` / `reanchor_classes_via_oop`, or seed the
cache directly with `override_class_lookup`.

## Example And CI

The complete example lives in `vmhook/src/example.cpp` with Java fixtures in
`example/vmhook`. It covers:

- field get and set for primitives, strings, arrays, and object references
- method calls and overload resolution
- instance and static hooks
- forced return values and void-method cancellation
- hook argument mutation with `set_arg`
- Java object allocation with `make_unique`
- superclass field and method lookup
- `java.util.{List, LinkedList, Set}` conversion to `std::vector<std::unique_ptr<T>>`
- `java.util.{Map, HashMap, LinkedHashMap, TreeMap}` conversion to vectors of `std::pair<unique_ptr<K>, unique_ptr<V>>`
- enum singletons (`Color.RED/GREEN/BLUE` with constructor args)
- interface default methods + override dispatch (`Animal` + `Dog`)
- static nested classes + non-static inner classes with synthetic outer ref
- overloaded methods (same name, three signatures)
- every primitive return type plus void and null-returning methods
- throwing methods (observing the exception from a non-hooked call)
- caller info from inside a hook (`return_value::caller()`)
- field-change watcher (`watch_static_field`)
- class-load watcher (`on_class_loaded`)
- boundary primitive values (MIN/MAX, NaN, +/- infinity, negatives)
- string edge cases (empty, unicode, long, interned, null)

The GitHub Actions matrix builds and runs the native harness against multiple
JDK versions. The harness writes `test_results.txt`; CI fails if any `[FAIL]`
line appears.

## Notes

- Register wrapper classes before using fields, methods, allocation, or hooks.
- Install hooks and call `shutdown_hooks()` from controlled native code paths.
- Cached HotSpot pointers assume process-lifetime injection; class unloading is
  not handled.
- JVM updates can change HotSpot internals and may require compatibility fixes.
