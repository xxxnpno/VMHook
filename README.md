# vmhook

Read fields, call methods, construct objects and hook methods in a **running HotSpot JVM**, from C++, without asking the permission to the JVM. No JVMTI, minimal JNI.

---

## Wrapping a class

When you want to work with a java class, reproduice the elements you want to work with in cpp.

```cpp
#include <vmhook/vmhook.hpp>

namespace sdk
{
    // if you want to work the the java Entity class
    // reproduice this template for every class you'll work with
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
// Entity class has a field named health, here is how to obtrain it
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

// for other objects then string always return unique_ptr not just the object
auto get_riding_entity()
    -> std::unique_ptr<sdk::entity>
{
    return get_field("ridingEntity")->get();
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
```

## Creating objects

```cpp
// creating objects using a java constructor (vmhook_make_unique not std::make_unique!!!)
const std::unique_ptr<sdk::entity>& fresh{ vmhook::make_unique<sdk::entity>() };

// the unique_ptr is never nullptr (unless no more ram ofc), but if the java object is null, ->get_instance() will return nullptr
// the unique_ptr is NOT the java onject 
if (fresh->get_instance())
{
    fresh->set_health(20.0f);
}
```

### A basic hook

```cpp
// reproduice the java args with cpp ones and add vmhook::return_value& return_value as the first one
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
        // 0 is NOT thizz becarful
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
        // only work for java void methods 
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
    const auto caller = return_value.caller();

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
<here explain how the i2i hook works>
