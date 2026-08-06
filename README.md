# vmhook

Read fields, call methods, construct objects and hook methods in a **running HotSpot JVM**, from C++, without asking the JVM's permission. No JVMTI, minimal JNI.

---

## Wrapping a class

When you want to work with a java class, reproduce the elements you want to work with in cpp.

```cpp
#include <vmhook/vmhook.hpp>

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

