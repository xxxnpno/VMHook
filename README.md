# vmhook

Read fields, call methods, construct objects and hook methods in a **running HotSpot JVM**, from C++, without asking the JVM's permission. No JVMTI, no JNI.

**A C++26 module, and g++ only**, with a **generated C++23 header** beside it for a toolchain
that has neither modules nor reflection. No preprocessor in the library beyond the global module
fragment that `<windows.h>` requires. The portability branches for MSVC, clang, Linux, macOS,
iOS, Android and aarch64 are gone: this targets the newest g++ on Windows x86-64 and nothing
else.

```bash
# the module — C++26, static reflection, and what the library is written in
g++ -std=c++26 -freflection -fmodules -c vmhook/ext/vmhook/vmhook.ixx -o vmhook.o
g++ -std=c++26 -freflection -fmodules your.cpp vmhook.o -o your.exe -Wl,--allow-multiple-definition

# or the header — C++23, no flags, nothing to build first
g++ -std=c++23 -I vmhook/ext your.cpp -o your.exe
```

`vmhook.hpp` is **generated from `vmhook.ixx`** by `tools/make_header.py`, so the two cannot
drift: everything is edited in the module. The transform removes exactly the C++26 from it —
the module preamble, `std::meta`, `^^`, annotations, `= delete("reason")` — and supplies a
C++23 answer for each, so the same call returns the same string either way. **The header is
C++23 and stays C++23**; nothing in it may ever need a C++26 feature.

Two things the module does that the header cannot, both diagnostics rather than behaviour:

| | module | header |
|---|---|---|
| `type_name<T>()` | `std::meta::display_string_of(^^T)` | parses `std::source_location`, same spelling |
| enumerator names | `std::meta::enumerators_of(^^E)` | probes values 0–127 via `source_location` |
| `[[= vmhook::java_class("…")]]` | read at compile time | not readable; falls back to `register_class` |

A consumer of the module includes its own headers **before** the import — `#include <cstdio>`
then `import vmhook;`, never the other way round, or GCC 16.2 reports a redefinition of
`std::__is_constant_evaluated`. It also includes `<string_view>` if it means to compare the
views vmhook returns: their operators live in the global module fragment, which a consumer's
lookup does not reach.

That link flag is working around a **GCC 16.2 bug**, not a design choice: a function-local
`static` inside an inline function in a module's purview is emitted into the module's object
*and* into every consumer's, so the linker sees the same symbol twice. It happens inside
libstdc++'s own `std::format` as well as in vmhook, so no source change avoids it. The
definitions are identical, which is why letting the linker pick one is safe.

What used to be a macro is now a value or a function template — macros do not cross a module
boundary, so a consumer would never have seen them:

| was | is |
|---|---|
| `VMHOOK_VERSION_MAJOR` … | `vmhook::version_major`, `version_minor`, `version_patch`, `version`, `version_string` |
| `VMHOOK_DEBUG_LOGS` | `vmhook::debug_logs` — a `constexpr bool` behind `if constexpr` |
| `VMHOOK_LOG_FILE` | `vmhook::log_file_path` — a **run-time** path, settable after the fact |
| `VMHOOK_AUTO_ATTACH_THREADS` | `vmhook::auto_attach_threads` |
| `VMHOOK_DISABLE_AUTO_REPAIR` | `vmhook::auto_repair` |
| `VMHOOK_LOG(...)` | `vmhook::detail::log_line(...)` — a variadic function template |

---

## Wrapping a class

When you want to work with a java class, reproduce the elements you want to work with in cpp.

```cpp
import vmhook;

namespace sdk
{
    // if you want to work with the java Entity class
    // reproduce this template for every class you'll work with
    class entity : public vmhook::object<sdk::entity>
    {
    public:
        entity(const vmhook::oop_t instance)
            : vmhook::object<sdk::entity>{ instance } {}

        // methods
    };
}

// associate your cpp class with the java class
vmhook::register_class<sdk::entity>("net/minecraft/entity/Entity");
```

## Getting a value

```cpp
// Entity class has a field named health, here is how to obtain it
auto get_health()
     -> float
{
    return get_field("health")->get();
}

// std::string = java.lang.String
auto get_name()
    -> std::string
{
    return get_field("name")->get();
}

// for objects other than string always return a unique_ptr, not the object itself
auto get_riding_entity()
    -> std::unique_ptr<sdk::entity>
{
    return get_field("ridingEntity")->get();
}

// a STATIC java field reads exactly like an instance one, nothing extra
auto get_entity_count()
    -> std::int32_t
{
    return get_field("entityCount")->get();
}
```

## Setting a value

```cpp
// same logic as get(), if the field is static, use the same syntax
auto set_health(float health)
    -> void
{
    get_field("health")->set(health);
}

auto set_name(const std::string& name)
    -> void
{
    get_field("name")->set(name);
}

auto set_riding_entity(const std::unique_ptr<sdk::entity>& entity)
    -> void
{
    get_field("ridingEntity")->set(entity);
}

// a STATIC java field writes exactly like an instance one
auto set_entity_count(std::int32_t count)
    -> void
{
    get_field("entityCount")->set(count);
}
```
## Calling a method

```cpp
// syntax for method calling
auto kill() const
    -> void
{
    get_method("kill")->call();
}

auto get_distance_to_entity(const std::unique_ptr<sdk::entity>& entity)
    -> float
{
    return get_method("getDistanceToEntity")->call(entity);
}

auto add_chat_message(const std::string& message, const std::int32_t id)
    -> bool
{
    return get_method("addChatMessage")->call(message, id);
}

// a STATIC java method calls exactly like an instance one
auto count_all()
    -> std::int32_t
{
    return get_method("countAll")->call();
}
```

## Creating objects

```cpp
// creating objects using a java constructor (vmhook::make_unique, NOT std::make_unique!!!)
const std::unique_ptr<sdk::entity> fresh{ vmhook::make_unique<sdk::entity>() };

// the unique_ptr is never nullptr (unless no more ram ofc), but if the java object is null, ->get_instance() will return nullptr
// the unique_ptr is NOT the java object
if (fresh->get_instance())
{
    fresh->set_health(20.0f);
}
```

### A basic hook

```cpp
// reproduce the java args with cpp ones and add vmhook::return_value& return_value as the first one
// no thizz in static methods ofc
auto run_tick_hook(vmhook::return_value& return_value, const std::unique_ptr<sdk::minecraft>& thizz)
    -> void
{
    // runs on the game thread, inside the call
}

// don't forget to declare the hook
vmhook::hook<sdk::minecraft>("runTick", &zoo::run_tick_hook);
```

### Changing an argument

```cpp
auto send_chat_message_hook(vmhook::return_value& return_value, const std::unique_ptr<sdk::entity>& thizz, const std::string& message)
    -> void
{
    if (message == "/hi")
    {
        // 0 is NOT thizz, be careful
        return_value.set_arg(0, "/hello");
    }

    // the java method will run as normal but its arg will be changed
}
```

### Cancelling the call

```cpp
auto attack_entity_hook(vmhook::return_value& return_value, const std::unique_ptr<sdk::entity>& thizz, const std::unique_ptr<sdk::entity>& target)
    -> void
{
    // the java method will not run
    if (!target->get_instance())
    {
        // only works for java void methods
        return_value.cancel();
    }
}
```

### Forcing a return value

```cpp
auto is_singleplayer_hook(vmhook::return_value& return_value, const std::unique_ptr<sdk::minecraft>& thizz)
    -> void
{
    // java isSingleplayer returns a bool
    // we force the return value to true
    return_value.set(true);

    // even though isSingleplayer returns a bool, cpp hooks are always void
}
```

### Getting caller info

```cpp
auto ray_trace_blocks_hook(vmhook::return_value& return_value, const std::unique_ptr<sdk::entity>& thizz)
    -> void
{
    const vmhook::return_value::caller_info caller{ return_value.caller() };

    // if you want to hook a method but only when it's called from "orientCamera"
    // check caller struct for more tools
    if (caller.valid() && caller.method_name == "orientCamera")
    {
        return_value.cancel();
    }
}
```
### Removing hooks

```cpp
// run this once before uninjecting
vmhook::shutdown_hooks();
```

## Faster builds

Measured here on GCC 16.2.0, one TU that names `vmhook::version` and nothing
else:

| | cost |
|---|---|
| `#include <vmhook/vmhook.hpp>` | **1.74 s per TU**, every TU |
| `import vmhook;` | **0.10 s per TU**, plus 4 s once to build the module |

Seventeen times cheaper per TU, and that is the whole argument for the module.

### Take the prebuilt one

Every [release](https://github.com/xxxnpno/vmhook/releases) carries the module
already compiled, so you pay neither the 4 s nor the 1.7 s:

```
libvmhook.a    the module's static archive — this toolchain's "vmhook.lib"
vmhook.gcm     the compiled module interface
vmhook.ixx     the module source
vmhook.hpp     the generated C++23 header
```

Put `vmhook.gcm` in a `gcm.cache/` directory beside where you compile, link
`libvmhook.a`, and `import vmhook;` costs a tenth of a second with nothing to
build first:

```bash
mkdir -p gcm.cache && cp /path/to/vmhook.gcm gcm.cache/
g++ -std=c++26 -freflection -fmodules -O2 -c your.cpp -o your.o
g++ your.o -l:libvmhook.a -o your.exe -Wl,--allow-multiple-definition
```

**The `.gcm` is not portable, by design.** It is GCC's own on-disk form of the
module, valid only for the exact compiler build and flags named in the release's
`BUILD-INFO.txt`. Any other compiler rejects it and rebuilds from `vmhook.ixx` —
correct behaviour, and it costs only the 4 s the file was saving. The archive and
the two sources have no such tie.

### Or build it yourself

```cmake
add_subdirectory(vmhook)
set(VMHOOK_BUILD_MODULE ON)
target_link_libraries(your_payload PRIVATE vmhook::module)
```

### If you are on the header

Link `vmhook::compiled` instead of `vmhook::vmhook` and your targets inherit a
precompiled header built from `vmhook.hpp`:

```cmake
target_link_libraries(your_payload PRIVATE vmhook::compiled)
```

A 7-TU project, rebuilding all of its sources: **12.3 s → 2.2 s** on MSVC 19.44,
**12.1 s → 6.8 s** on GCC 15. The PCH costs a few seconds to build once, so a
one-TU project gains nothing — keep `vmhook::vmhook` there. A PCH cannot ship
(it is tied to the exact compiler and flags that made it), so an installed
vmhook needs you to ask for one yourself:

```cmake
target_precompile_headers(your_payload PRIVATE <vmhook/vmhook.hpp>)
```

`vmhook::compiled_version()` returns the version the archive was built from —
compare it with `vmhook::version` if you ever suspect a stale one.

## How the hook actually works

Nothing here rewrites your Java method. The bytecode is untouched, the class is
never redefined, and the JVM is never told anything happened.

### 1. Every method has an entry point, and it is a pointer

A HotSpot `Method` is a metadata object, and three of its fields are addresses
the VM jumps to when someone calls that method:

```
Method
  _i2i_entry             <- the interpreter's entry for this method
  _from_interpreted_entry <- what interpreted callers jump to
  _from_compiled_entry    <- what JIT-compiled callers jump to
  _code                   <- the compiled nmethod, or null if not JIT'd yet
```

`_i2i_entry` points into the "interpreter to interpreter" stub — the machine
code that runs when a Java method starts executing interpreted. vmhook reads all
of these through VMStructs, by name, so no offset is hardcoded.

One thing that matters later: **that stub is shared**. HotSpot generates one per
method *shape*, not one per method, so many methods enter through the same
address.

### 2. vmhook writes a jump inside that stub

`vmhook::hook<T>("name", &fn)` finds the `Method`, then scans its i2i stub for an
injection point — a specific `mov BYTE PTR [r15+imm32], imm8` (the thread-state
write) that sits after the frame is built and the arguments are laid out, but
before the first bytecode runs. That is the moment where `thizz` and every
argument are already in place.

There it saves the original 5 bytes and writes `E9 <rel32>` — a jump to a small
trampoline vmhook generated itself. The trampoline is allocated *within ±2GB* of
the stub on purpose, because a 5-byte relative jump cannot reach further.

The trampoline:

- saves the interpreter's register state,
- hands the frame pointer (`rbp`) and the `JavaThread` (`r15`) to vmhook's C++
  dispatcher,
- the dispatcher reads the **real `Method*` out of the frame** — remember the
  stub is shared, so most calls arriving here belong to methods you never hooked,
  and those are passed straight through,
- for a hooked one, it walks the frame to `locals[]`, where `thizz` and the
  arguments live, and decodes them into the C++ parameter types you declared,
- it calls your function,
- then either continues into the real method, or — if you called `cancel()` or
  `set()` — writes the return slot and returns to the caller without the Java
  method ever running.

That is why your parameters mirror the Java signature: they are read
positionally out of interpreter slots. A `long` or a `double` occupies **two**
slots, which vmhook works out from the method's descriptor rather than from your
C++ types.

If another tool already patched that injection point (the first byte is already
`E9`), vmhook chains in front of it rather than overwriting it, so both hooks
still fire.

### 3. The JIT has to be pushed out of the way

Patching `_i2i_entry` only affects the *interpreted* path. A method HotSpot has
already compiled does not go through the interpreter at all — `_code` points at
an nmethod and callers jump straight into machine code. The hook would install
successfully and never fire.

So on install, for a method that is already compiled, vmhook:

- points `_from_interpreted_entry` back at the (now patched) `_i2i_entry`,
- points `_from_compiled_entry` at the **c2i adapter**, the stub that converts a
  compiled call frame into an interpreter frame,
- clears `_code` last, so the two writes above are visible before anyone
  observes that the compiled version is gone.

The c2i adapter is not always available to ask for: `AdapterHandlerEntry` is not
exported by any JDK's VMStructs. vmhook derives it instead — adapters are shared
per signature, so any interpreted method with the same descriptor and
static-ness has the right one in its `_from_compiled_entry`, and a method that
has itself been deoptimised once publishes its own from then on.

The method is now interpreted again, and interpreted means patched. On the same
install `hook()` also marks the method not-compilable so the JIT does not simply
recompile it a second later (that bit lives in `Method::_access_flags` up to JDK
23 and in `MethodFlags::_status` from JDK 24), and `_dont_inline` so it does not
get inlined into a future caller. A background watchdog then re-checks every
installed hook once a second and re-applies any that HotSpot managed to undo.

**You never call any of this yourself.** There is no "deoptimise first" step:
`vmhook::deoptimize_methods_if` and `deoptimize_all_jit_compiled_methods` exist
for sweeping *other* methods you did not hook, and hooking a hot method needs
none of it. You also never call `verify_hooks()` — the watchdog is what calls
it, from the moment your first hook installs.

The one case none of this reaches: a *caller* that was compiled with your method
**inlined** into it has no call to intercept at all. Those call sites resolve
themselves the next time HotSpot reaches a safepoint and re-evaluates the
inline cache.

### 4. Removing them

`shutdown_hooks()` writes the saved original bytes back over every patched
entry and stops the watchdog.

Removing a hook stops threads from *entering* it. It cannot evict a thread that
is already inside one, and vmhook keeps no in-flight count — so unloading your
DLL right after is how you kill the process. If your tool supports detaching,
prefer leaving the module mapped.

