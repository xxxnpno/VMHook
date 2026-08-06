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
  what you asked for.
- **Objects are `std::unique_ptr<T>`.** That is the only object type you ever
  write, everywhere: fields, arguments, return values, hook parameters.

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

class entity final : public vmhook::object<entity>
{
public:
    explicit entity(vmhook::oop_t instance) : vmhook::object<entity>{ instance } {}

    // --- reading -----------------------------------------------------------
    auto health() -> float       { return get_field("health")->get(); }
    auto alive()  -> bool        { return get_field("isAlive")->get(); }
    auto name()   -> std::string { return get_field("name")->get(); }

    // a STATIC Java field — same call, the field itself says which it is
    auto count()  -> std::int32_t { return get_field("entityCount")->get(); }

    // an object field
    auto passenger() -> std::unique_ptr<entity> { return get_field("passenger")->get(); }

    // --- writing -----------------------------------------------------------
    auto set_health(float hp)           -> void { get_field("health")->set(hp); }
    auto set_name(const std::string& n) -> void { get_field("name")->set(n); }
    auto set_passenger(const std::unique_ptr<entity>& e) -> void
    {
        get_field("passenger")->set(e);
    }

    // --- calling -----------------------------------------------------------
    auto kill() -> void { get_method("kill")->call(); }

    auto distance_to(const std::unique_ptr<entity>& other) -> double
    {
        return get_method("distanceTo")->call(other);
    }

    auto add_tag(const std::string& tag, std::int32_t level, bool sticky) -> bool
    {
        return get_method("addTag")->call(tag, level, sticky);
    }

    // a method that returns an object
    auto rider() -> std::unique_ptr<entity> { return get_method("getRidingEntity")->call(); }
};

vmhook::register_class<entity>("net/minecraft/entity/Entity");
```

Register every wrapper before using it.

Notice what is *not* in there: no `<float>`, no `<std::string>`, no cast, no
extraction call. The return type of your accessor is the whole specification —
`get()` and `call()` produce whatever it says.

When two Java overloads differ only in their descriptor, name it:

```cpp
auto add(const std::unique_ptr<entity>& other) -> bool
{
    return get_method("add", "(Ljava/lang/Object;)Z")->call(other);
}
```

## Using it

Outside the wrapper you only ever see your own methods and `std::unique_ptr`.

```cpp
float       hp  = e->health();
std::string n   = e->name();
std::int32_t all = e->count();

e->set_health(20.0f);
e->set_name("Bob");
e->set_passenger(other);

e->kill();
double d = e->distance_to(other);

std::unique_ptr<entity> p = e->passenger();
if (p) { p->set_health(1.0f); }
```

A null Java reference comes back as a null `unique_ptr`, so `if (p)` is the only
check you need.

## Creating objects

`create` allocates **and runs the real Java constructor**, picking the `<init>`
overload from the argument types:

```cpp
std::unique_ptr<entity> fresh = entity::create();
std::unique_ptr<entity> named = entity::create("Bob", 12);
```

If no `<init>` matches, nothing is allocated and you get null — never a
half-built object.

## Hooks

A hook replaces a method's interpreter entry, so your lambda runs every time
Java calls it. Declare the parameters you want and they are decoded for you:
primitives by value, objects as `std::unique_ptr`.

```cpp
auto h = vmhook::scoped_hook<entity>("hurt", "(F)V",
    [](vmhook::return_value& ret, std::unique_ptr<entity> self, float amount) noexcept {

        if (self) { self->set_health(20.0f); }

        ret.set_arg(0, 0.0f);       // change what the original sees
        ret.cancel();               // or stop it running at all
        ret.set(true);              // or force a return value

        const auto who   = ret.caller();       // who called it:
                                               //   class_name, method_name, valid()
        const auto stack = ret.stack_trace();  // ...or the whole interpreter stack
    });
```

Hook every method matching a descriptor instead of naming one:

```cpp
vmhook::hook_by_signature<entity>("(F)V", [](vmhook::return_value&) noexcept {});
```

Watch a field change, a class load, or a thrown exception:

```cpp
auto w = vmhook::watch_static_field<entity, std::int32_t>("entityCount",
    [](std::int32_t before, std::int32_t after) noexcept {});

auto c = vmhook::on_class_loaded([](const std::string& name) noexcept {});
auto e = vmhook::on_exception   ([](const std::string& type) noexcept {});
```

Manage them:

```cpp
vmhook::verify_hooks();     // re-check every installed hook is still in place
vmhook::shutdown_hooks();   // remove all of them
```

Four things that bite:

- **`scoped_hook` removes its hook on destruction — do not let that happen
  during DLL unload.** Removing a hook stops threads *entering* it but cannot
  evict one already inside, and unloading a module a Java thread is executing
  takes the process with it. Prefer leaving the module mapped.
- **Deoptimise a hot method before hooking it**, or the JIT routes around the
  detour and the hook silently never fires.
- **No exception may reach the JVM.** Hook bodies are `noexcept`; the frame
  above you is HotSpot's interpreter and it has no handler.
- **A wrapper is a view for the duration of the call.** It holds a plain
  address, so do not stash one in a global and use it later — the collector
  moves objects. Re-read it from wherever you got it.

## Calling from your own thread

Reading a field works from any thread. *Calling* needs a `JavaThread`, so take a
scope — and resolve and call inside the same one, because between two scopes a
collection can run and move what the first one found.

```cpp
void worker() {                       // an ordinary std::thread
    const vmhook::java_thread_scope java{};
    if (!java) { return; }            // ALWAYS check

    e->kill();
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
