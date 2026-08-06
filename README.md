# vmhook

[![CI](https://github.com/xxxnpno/vmhook/actions/workflows/ci.yml/badge.svg?branch=master)](https://github.com/xxxnpno/vmhook/actions/workflows/ci.yml)
[![License](https://img.shields.io/badge/License-See%20LICENSE.txt-blue)](LICENSE.txt)
[![Single header](https://img.shields.io/badge/Single%20header-vmhook.hpp-brightgreen)](vmhook/ext/vmhook/vmhook.hpp)
[![C++ Standard](https://img.shields.io/badge/C%2B%2B-20%20%2F%2023-blue)](https://en.cppreference.com/w/cpp/23)

Read fields, call methods, construct objects and hook methods in a **running
HotSpot JVM**, from C++ inside the same process. One header, no JVMTI agent, no
launch flags, no cooperation from the target.

You never write a type argument to `get`, `set` or `call`. The JVM already
stores every field's and method's type; vmhook reads it and gives you back what
you asked for.

```cmake
add_library(vmhook INTERFACE)
target_include_directories(vmhook INTERFACE path/to/vmhook/ext)
target_link_libraries(your_target PRIVATE vmhook)
```

---

## Wrapping a class

```cpp
#include <vmhook/vmhook.hpp>

class entity : public vmhook::object<entity> { using object::object; };

vmhook::register_class<entity>("net/minecraft/entity/Entity");
```

Register before using fields, methods, construction or hooks.

## Getting a primitive field

`get_field` resolves **instance and static** fields the same way — the field
itself says which it is, so you never pick a spelling.

```cpp
float health = self->get_field("health")->get();
bool  alive  = self->get_field("isAlive")->get();
int   count  = self->get_field("entityCount")->get();   // a static Java field
```

Without an instance, go through the type:

```cpp
int count = entity::static_field("entityCount")->get();
```

Use `float f = ...->get();`, not `float f{ ...->get() };` — brace-init
reconsiders the target's own constructors and picks surprising ones.

## Getting a string

```cpp
std::string name = self->get_field("name")->get();
```

## Getting an object

```cpp
auto rider = self->get_field("passenger")->get();

float x           = rider->get_field("posX")->get();
std::string label = rider->get_field("name")->get();
```

No type is named and nothing is borrowed. `rider` resolves its members from the
**live object's own class**, so this works even for a class you never wrapped.
A Java null ends the chain quietly instead of faulting.

Name a type when you want one — the declaration alone is enough:

```cpp
entity typed = self->get_field("passenger")->get();
```

## Setting a primitive field

```cpp
self->get_field("health")->set(20.0f);
self->get_field("isAlive")->set(true);
self->get_field("entityCount")->set(0);        // static field, same call
```

## Setting a string

```cpp
self->get_field("name")->set("Bob");
```

Builds a real `java.lang.String` and rebinds the field to it, like a Java
`field = value;`. The previous String is never mutated, so a shared or interned
one cannot be corrupted.

## Setting an object

```cpp
self->get_field("passenger")->set(other);
self->get_field("passenger")->set(other->get_field("passenger")->get());
```

Anything that names a live object is accepted. Each is revalidated before the
write, so an expired or empty source stores nothing rather than writing a stale
address into the heap.

## Calling a method

```cpp
self->get_method("kill")->call();
double d = self->get_method("distanceTo")->call(other);
```

Without an instance:

```cpp
int n = entity::static_method("countAll")->call();
```

## Calling with arguments

Argument types are deduced, and the overload is selected from them.

```cpp
self->get_method("setName")->call("Bob");                       // string
self->get_method("mount")->call(other);                         // object
bool ok = self->get_method("addTag")->call("hostile", 3, true);  // mixed
```

The result behaves like a field read — it is whatever you assign it to, and it
chains:

```cpp
auto held         = self->get_method("getHeldItem")->call();
std::string label = held->get_method("getDisplayName")->call();
```

When two overloads differ only in their descriptor, name it:

```cpp
self->get_method("add", "(Ljava/lang/Object;)Z")->call(other);
```

## Creating objects

`create` allocates **and runs the real Java constructor**, picking the
`<init>` overload from the argument types:

```cpp
auto p = player::create("Bob", 12);
if (p) { p->get_field("name")->get(); }

auto blank = item::create();     // no-arg constructor
```

If no `<init>` matches, nothing is allocated and you get an empty handle — never
a half-built object.

## Hooks

A hook replaces a method's interpreter entry, so your lambda runs every time
Java calls it.

```cpp
auto h = vmhook::scoped_hook<entity>("hurt", "(F)V",
    [](vmhook::return_value& ret, vmhook::borrowed<entity> self, float amount) noexcept {

        ret.set_arg(0, 0.0f);       // change what the original sees
        ret.cancel();               // or stop it running at all
        ret.set(true);              // or force a return value

        const auto who   = ret.caller();       // who called it:
                                               //   class_name, method_name, valid()
        const auto stack = ret.stack_trace();  // ...or the whole interpreter stack
    });
```

Arguments are declared on the lambda and decoded for you: primitives by value,
objects as handles.

Hook every method matching a descriptor instead of naming one:

```cpp
vmhook::hook_by_signature<entity>("(F)V", [](vmhook::return_value&) noexcept {});
```

Watch a field change, a class load, or a thrown exception:

```cpp
auto w = vmhook::watch_static_field<entity, int>("entityCount",
    [](int before, int after) noexcept {});

auto c = vmhook::on_class_loaded([](const std::string& name) noexcept {});
auto e = vmhook::on_exception   ([](const std::string& type) noexcept {});
```

Manage them:

```cpp
vmhook::verify_hooks();     // re-check every installed hook is still in place
vmhook::shutdown_hooks();   // remove all of them
```

`scoped_hook` removes its hook on destruction. **Do not let that happen during
DLL unload** — removing a hook stops threads *entering* it, but cannot evict one
already inside, and unloading a module a Java thread is executing takes the
process with it. Prefer leaving the module mapped.

Two more things that bite:

- **Deoptimise a hot method before hooking it**, or the JIT routes around the
  detour and the hook silently never fires.
- **No exception may reach the JVM.** Hook bodies are `noexcept`; the frame
  above you is HotSpot's interpreter and it has no handler.

## Licence

See [LICENSE.txt](LICENSE.txt).

---

vmhook reads and patches HotSpot internals. They are not a supported interface
and a JVM update can move anything. Everything resolves by name rather than by
hardcoded offset so a change surfaces as a clean failure rather than as
corruption — but test against the exact VM you target.
