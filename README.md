# vmhook

[![CI](https://github.com/xxxnpno/vmhook/actions/workflows/ci.yml/badge.svg?branch=master)](https://github.com/xxxnpno/vmhook/actions/workflows/ci.yml)
[![License](https://img.shields.io/badge/License-See%20LICENSE.txt-blue)](LICENSE.txt)
[![Single header](https://img.shields.io/badge/Single%20header-vmhook.hpp-brightgreen)](vmhook/ext/vmhook/vmhook.hpp)
[![C++ Standard](https://img.shields.io/badge/C%2B%2B-20%20%2F%2023-blue)](https://en.cppreference.com/w/cpp/23)

Read fields, call methods, construct objects and hook methods in a **running
HotSpot JVM**, from C++ inside the same process. One header, no JVMTI agent, no
launch flags, no cooperation from the target.

Two rules run through the whole API:

- **No type arguments.** `get`, `set` and `call` never take one. The JVM already
  stores every field's and method's type — vmhook reads it and gives you back
  what your accessor's return type asks for.
- **Objects are `std::unique_ptr<T>`.** That is the only object type you ever
  write, everywhere: fields, arguments, return values, hook parameters. It is
  never null — check `get_instance()` when you need to know whether the Java
  reference was null.

```cmake
add_library(vmhook INTERFACE)
target_include_directories(vmhook INTERFACE path/to/vmhook/ext)
target_link_libraries(your_target PRIVATE vmhook)
```

---

## Wrapping a class

You describe a Java class once, as a C++ class. `get_field` and `get_method`
live **inside** it — they are how a wrapper reaches its own object, not
something callers use.

```cpp
#include <vmhook/vmhook.hpp>

namespace sdk
{
    class entity : public vmhook::object<sdk::entity>
    {
    public:
        explicit entity(const vmhook::oop_t instance) noexcept
            : vmhook::object<sdk::entity>{ instance }
        {
        }

        auto get_health() const noexcept -> float
        {
            return get_field("health")->get();
        }

        auto get_riding_entity() const noexcept -> std::unique_ptr<sdk::entity>
        {
            return get_field("ridingEntity")->get();
        }

        auto kill() const noexcept -> void
        {
            get_method("kill")->call();
        }
    };
}

vmhook::register_class<sdk::entity>("net/minecraft/entity/Entity");
```

Register every wrapper before using it. Wrappers inherit, so
`class entity_living_base : public sdk::entity` gets everything above.

The accessor's **return type is the whole specification**: no `<float>`, no
cast, no extraction call. And nothing in there says which *kind* of member it
is — `get_field` resolves a static or an instance Java field indistinguishably,
and so does `get_method`.

The one exception is a **static C++ method**, which has no object to work
through. Use `static_field` / `static_method` there:

```cpp
static auto get_minecraft() noexcept -> std::unique_ptr<sdk::minecraft>
{
    return static_field("theMinecraft")->get();
}
```

## Using it

Outside the wrapper you only see your own methods and `std::unique_ptr`.

```cpp
const auto minecraft = sdk::minecraft::get_minecraft();
if (!minecraft->get_instance()) { return; }

const auto player = minecraft->get_the_player();
if (!player->get_instance()) { return; }

player->set_health(20.0f);
```

The `unique_ptr` is **never null**, so `->` is always safe to write. What can be
absent is the Java object inside it: `get_instance()` returns `nullptr` when the
Java reference was null or could not be decoded. That keeps "I got a wrapper"
and "there was an object" as two separate questions, instead of folding them
into one null check at every step.

## Getting a value

```cpp
auto get_health() const noexcept -> float
{
    return get_field("health")->get();
}

auto get_name() const noexcept -> std::string
{
    return get_field("name")->get();
}

auto get_riding_entity() const noexcept -> std::unique_ptr<sdk::entity>
{
    return get_field("ridingEntity")->get();
}
```

## Setting a value

```cpp
auto set_health(const float health) const noexcept -> void
{
    get_field("health")->set(health);
}

auto set_name(const std::string& name) const noexcept -> void
{
    get_field("name")->set(name);
}

auto set_riding_entity(const std::unique_ptr<sdk::entity>& entity) const noexcept -> void
{
    get_field("ridingEntity")->set(entity);
}
```

Writing a string builds a real `java.lang.String` and rebinds the field to it,
like a Java `field = value;` — the previous String is never mutated, so a shared
or interned one cannot be corrupted.

## Calling a method

```cpp
auto kill() const noexcept -> void
{
    get_method("kill")->call();
}

auto get_distance_to_entity(const std::unique_ptr<sdk::entity>& entity) const noexcept -> float
{
    return get_method("getDistanceToEntity")->call(entity);
}

auto add_chat_message(const std::string& message, const std::int32_t id) const noexcept -> bool
{
    return get_method("addChatMessage")->call(message, id);
}
```

Argument types are deduced and the Java overload is selected from them.

## Creating objects

`create` allocates **and runs the real Java constructor**, picking the `<init>`
overload from the argument types:

```cpp
const auto fresh = sdk::entity::create();
if (fresh->get_instance()) { fresh->set_health(20.0f); }
```

If no `<init>` matches, nothing is allocated and `get_instance()` is null —
never a half-built object.

## Hooks

A hook replaces a method's interpreter entry, so your function runs every time
Java calls it. Declare the parameters you want and they are decoded for you:
primitives by value, objects as `std::unique_ptr`.

vmhook **deoptimises the hooked method itself** on install and holds
`NO_COMPILE` on it, so hooking a hot method needs no preparation on your part.
(The one case it cannot reach is a *caller* that already inlined the target;
those call sites resolve at the next safepoint.)

### A basic hook

```cpp
auto run_tick_hook(vmhook::return_value& return_value,
                   const std::unique_ptr<sdk::minecraft>& thizz) -> void
{
    // runs on the game thread, inside the call
}

vmhook::hook<sdk::minecraft>("runTick", &zoo::run_tick_hook);
```

Pin an exact overload by descriptor when the name is ambiguous:

```cpp
vmhook::hook<sdk::entity>("getDistanceToEntity",
                          "(Lnet/minecraft/entity/Entity;)F",
                          &zoo::get_distance_hook);
```

### Changing an argument

```cpp
auto send_chat_message_hook(vmhook::return_value& return_value,
                            const std::unique_ptr<sdk::entity>& thizz,
                            const std::string& message) -> void
{
    if (message == "/hi")
    {
        return_value.set_arg(0, std::string{ "/hello" });
    }
}
```

The original method then runs with the replaced argument.

### Cancelling the call

```cpp
auto attack_entity_hook(vmhook::return_value& return_value,
                        const std::unique_ptr<sdk::entity>& thizz,
                        const std::unique_ptr<sdk::entity>& target) -> void
{
    if (!target->get_instance())
    {
        return_value.cancel();
    }
}
```

### Forcing a return value

```cpp
auto is_singleplayer_hook(vmhook::return_value& return_value,
                          const std::unique_ptr<sdk::minecraft>& thizz) -> void
{
    return_value.set(true);   // the original never runs
}
```

### Getting caller info

```cpp
auto ray_trace_blocks_hook(vmhook::return_value& return_value,
                           const std::unique_ptr<sdk::entity>& thizz) -> void
{
    const auto caller = return_value.caller();
    if (caller.valid() && caller.method_name == "orientCamera")
    {
        return_value.cancel();
    }
}
```

`caller()` gives `class_name`, `method_name`, `signature` and `valid()`.
`stack_trace()` walks outward from there and returns the same for every frame.

### Removing hooks

```cpp
vmhook::shutdown_hooks();
```

**Do not let that happen during DLL unload.** Removing a hook stops threads
*entering* it but cannot evict one already inside, and unloading a module a Java
thread is executing takes the process with it. Prefer leaving the module mapped.

Two more things worth knowing:

- **A wrapper is a view for the duration of the call.** It holds a plain
  address, so do not stash one in a global and use it later — the collector
  moves objects. Re-read it from wherever you got it.
- **No exception may reach the JVM.** The frame above your hook is HotSpot's
  interpreter and it has no handler.

## Calling from your own thread

Reading a field works from any thread. *Calling* needs a `JavaThread`, so take a
scope — and resolve and call inside the same one, because between two scopes a
collection can run and move what the first one found.

```cpp
void worker()                         // an ordinary std::thread
{
    const vmhook::java_thread_scope java{};
    if (!java) { return; }            // ALWAYS check

    const auto minecraft = sdk::minecraft::get_minecraft();
    if (minecraft->get_instance()) { minecraft->get_the_player()->kill(); }
}
```

Keep it short: while it is open, the VM waits for this thread at safepoints.

## Licence

See [LICENSE.txt](LICENSE.txt).

---

vmhook reads and patches HotSpot internals. They are not a supported interface
and a JVM update can move anything. Everything resolves by name rather than by
hardcoded offset so a change surfaces as a clean failure rather than as
corruption — but test against the exact VM you target.
