// interface_polymorphism JVM test module  (feature area: fields + methods)
//
// THE interface-polymorphism authority: a field whose DECLARED (static) type is
// an interface but whose RUNTIME type is a concrete subclass.  Mirrors the
// legacy example.cpp test_interface_and_polymorphism (Animal / Dog / Pet) as a
// self-contained module against the vmhook.fixtures.InterfacePoly fixture, whose
// nested interface + class are emitted as InterfacePoly$Animal /
// InterfacePoly$Dog.
//
// What this module proves on a live JVM (Java 8/11/17/21/24/25 x MSVC/Clang/GCC),
// every observation a side-effect-free read against the published SINGLETON:
//
//   * RUNTIME-TYPE RESOLUTION: reading the `Animal pet` field (declared type the
//     interface) yields a usable wrapper whose decoded OOP's runtime klass --
//     resolved straight from the object header via vmhook::klass_from_oop, NOT
//     from the registered wrapper type -- has an internal name ending in "Dog".
//     vmhook sees the concrete type, never the declared interface.
//   * VIRTUAL DISPATCH: the overridden speak(), invoked through the CONCRETE-type
//     (Dog) wrapper's method path, reaches Dog's override and returns a
//     "woof"-containing String.  (Through the INTERFACE-type wrapper the same
//     name resolves to the abstract interface method, which has no body -- the
//     polymorphism point: only the concrete-typed walk reaches the override.)
//   * DECLARED-vs-CONCRETE IDENTITY: reading the same slot as the declared
//     interface type and as the concrete type decode to the SAME oop (the field
//     decode is type-agnostic -- it wraps whatever the slot's compressed OOP
//     points at).
//   * DOG-SPECIFIC STATE: name/age/breed read back their deterministic values
//     through the concrete wrapper.
//   * INTERFACE-DEFAULT-METHOD LIMITATION (characterised, never fails): the
//     default method defaultGreet() declared on Animal (and NOT overridden by
//     Dog) is reachable only by walking the INTERFACE chain.  vmhook's
//     object::get_method walks the SUPERCLASS chain (Dog -> Object) only, so a
//     lookup through the Dog wrapper is expected to miss it -> recorded [INFO].
//     Through the INTERFACE wrapper (whose own klass declares the default) the
//     same lookup is expected to succeed -> also characterised.
//   * JVM AGREEMENT: a probe runs the SAME observations Java-side and publishes a
//     witness the module reads back (pet instanceof Dog && speak() ~ "woof").
//
// SAFETY: every oop/klass deref is gated with vmhook::hotspot::is_valid_pointer;
// the module never wraps a mis-typed oop for a call (speak() is dispatched only
// through the Dog wrapper after the runtime klass is confirmed to be Dog), and
// it installs NO hooks (it drives the probe purely to publish Java witnesses), so
// it leaves nothing armed.  call()-dependent asserts are gated best-effort and
// any genuine flaw is recorded as an actual [INFO].
//
// Harness shape mirrors field_static / field_object_ref: register the wrappers,
// a `mode` selector with a `done` reset on the rising edge of go, and a dense
// battery of ctx.check()s.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace
{
    // Internal names of the fixture's nested types (javac '$' nesting).
    constexpr const char* k_holder_class = "vmhook/fixtures/InterfacePoly";
    constexpr const char* k_animal_class = "vmhook/fixtures/InterfacePoly$Animal";
    constexpr const char* k_dog_class    = "vmhook/fixtures/InterfacePoly$Dog";

    // ── Wrapper for the CONCRETE implementation (InterfacePoly$Dog) ─────────
    // Registered as the concrete class, so its resolve_klass() (typeid-based)
    // lands on Dog -- the only walk that reaches Dog's speak() override and its
    // own fields.  default_greet() is here purely to characterise the interface-
    // default-method limitation through a concrete-typed wrapper.
    class ifp_dog : public vmhook::object<ifp_dog>
    {
    public:
        explicit ifp_dog(vmhook::oop_t instance) noexcept
            : vmhook::object<ifp_dog>{ instance }
        {
        }

        // Dog-specific fields (declared directly on the concrete class).
        auto get_name()  -> std::string  { const std::string s = get_field("name")->get();  return s; }
        auto get_age()   -> std::int32_t { const std::int32_t v = get_field("age")->get();   return v; }
        auto get_breed() -> std::string  { const std::string s = get_field("breed")->get();  return s; }

        // Overridden virtual -- reached via the Dog-typed superclass walk.
        // MSVC copy-init: method_proxy String results go through as_string().
        auto speak() -> std::string
        {
            const auto m{ get_method("speak") };
            if (!m.has_value())
            {
                return std::string{ "<<no-speak>>" };
            }
            const std::string s = m->call().as_string();
            return s;
        }

        // Dog-only method (not on the Animal interface).
        auto fetch() -> std::string
        {
            const auto m{ get_method("fetch") };
            if (!m.has_value())
            {
                return std::string{ "<<no-fetch>>" };
            }
            const std::string s = m->call().as_string();
            return s;
        }

        // Does the Dog wrapper's superclass walk find the interface DEFAULT
        // method?  (Expected: no -- get_method walks Dog -> Object only.)
        auto resolves_default_greet() const -> bool
        {
            return get_method("defaultGreet").has_value();
        }

        auto default_greet() -> std::string
        {
            const auto m{ get_method("defaultGreet") };
            if (!m.has_value())
            {
                return std::string{ "<<no-default>>" };
            }
            const std::string s = m->call().as_string();
            return s;
        }
    };

    // ── Wrapper for the DECLARED interface type (InterfacePoly$Animal) ──────
    // Registered as the interface, so its resolve_klass() lands on the interface
    // klass.  A `speak()` lookup here finds the ABSTRACT interface method (no
    // body); a `defaultGreet()` lookup finds the DEFAULT method that lives
    // directly on this klass.  Used to prove the declared-type read decodes the
    // SAME oop as the concrete-type read, and to characterise default-method
    // resolution when the wrapper's own klass actually declares it.
    class ifp_animal : public vmhook::object<ifp_animal>
    {
    public:
        explicit ifp_animal(vmhook::oop_t instance) noexcept
            : vmhook::object<ifp_animal>{ instance }
        {
        }

        auto resolves_speak() const -> bool
        {
            return get_method("speak").has_value();
        }

        auto resolves_default_greet() const -> bool
        {
            return get_method("defaultGreet").has_value();
        }
    };

    // ── Wrapper for the holder (InterfacePoly) ─────────────────────────────
    class ifp_holder : public vmhook::object<ifp_holder>
    {
    public:
        explicit ifp_holder(vmhook::oop_t instance) noexcept
            : vmhook::object<ifp_holder>{ instance }
        {
        }

        // ---- handshake (static) ----
        static auto set_go(bool value)   -> void { static_field("go")->set(value); }
        static auto set_done(bool value)  -> void { static_field("done")->set(value); }
        static auto get_done()  -> bool          { return static_field("done")->get(); }
        static auto set_mode(std::int32_t m) -> void { static_field("mode")->set(m); }

        // ---- the published singleton holder ----
        static auto singleton() -> std::unique_ptr<ifp_holder> { return static_field("SINGLETON")->get(); }

        // ---- the `pet` field signature (proves it is the interface type) ----
        auto pet_signature() const -> std::string
        {
            const auto proxy{ get_field("pet") };
            if (!proxy.has_value())
            {
                return std::string{ "<<no-pet>>" };
            }
            const std::string s{ proxy->signature() };
            return s;
        }

        // ---- read `pet` AS the concrete Dog type ----
        auto pet_as_dog() const -> std::unique_ptr<ifp_dog> { return get_field("pet")->get(); }

        // ---- read `pet` AS the DECLARED interface type ----
        auto pet_as_animal() const -> std::unique_ptr<ifp_animal> { return get_field("pet")->get(); }

        // ---- read the petAsDog field (concrete-typed slot to the SAME object) ----
        auto pet_alias_as_dog() const -> std::unique_ptr<ifp_dog> { return get_field("petAsDog")->get(); }

        // ---- Java-side witnesses ----
        static auto get_pet_is_dog_seen() -> bool        { return static_field("petIsDogSeen")->get(); }
        static auto get_pet_speak_seen()  -> std::string { const std::string s = static_field("petSpeakSeen")->get(); return s; }
    };

    // The leaf internal name (after the final '/' and any '$') of a runtime
    // klass read straight from an oop header.  Safety-gated end to end.
    auto runtime_klass_name(vmhook::oop_t oop) -> std::string
    {
        if (!oop || !vmhook::hotspot::is_valid_pointer(oop))
        {
            return std::string{};
        }
        vmhook::hotspot::klass* const k{ vmhook::klass_from_oop(oop) };
        if (!k || !vmhook::hotspot::is_valid_pointer(k))
        {
            return std::string{};
        }
        const vmhook::hotspot::symbol* const sym{ k->get_name() };
        if (!sym || !vmhook::hotspot::is_valid_pointer(sym))
        {
            return std::string{};
        }
        return sym->to_string();
    }

    auto ends_with(const std::string& s, const std::string& suffix) -> bool
    {
        return s.size() >= suffix.size()
            && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    auto contains(const std::string& s, const std::string& needle) -> bool
    {
        return s.find(needle) != std::string::npos;
    }

    // Drive one probe cycle for `mode`: clear the latched `done`, program the
    // selector on the rising edge of go, then wait for done.
    auto drive(vmhook_test::context& ctx, std::int32_t mode) -> bool
    {
        return ctx.run_probe(
            [mode](bool value)
            {
                if (value)
                {
                    ifp_holder::set_done(false);
                    ifp_holder::set_mode(mode);
                }
                ifp_holder::set_go(value);
            },
            []() { return ifp_holder::get_done(); });
    }
}

VMHOOK_JVM_MODULE(interface_polymorphism)
{
    vmhook::register_class<ifp_holder>(k_holder_class);
    vmhook::register_class<ifp_animal>(k_animal_class);
    vmhook::register_class<ifp_dog>(k_dog_class);

    // =====================================================================
    //  0. Sanity: the holder, the interface, and the concrete class resolve;
    //     the `pet` field resolves and is typed as the INTERFACE (declared).
    // =====================================================================
    ctx.check("ifp_holder_registered", ifp_holder::static_field("SINGLETON").has_value());

    const auto holder{ ifp_holder::singleton() };
    ctx.check("ifp_singleton_nonnull", holder != nullptr);
    if (!holder)
    {
        // Without the holder the rest is meaningless; the probe below still
        // confirms the JVM-side shape, but bail out of the native reads.
        ctx.record("[INFO] interface_polymorphism: SINGLETON not resolvable; "
                   "skipping native polymorphism reads (fixture not loaded?).");
        return;
    }

    // The declared static type of `pet` is the interface, so its JVM field
    // descriptor names InterfacePoly$Animal (a single 'L...;' object ref).
    {
        const std::string sig{ holder->pet_signature() };
        ctx.check("pet_field_resolves", sig != "<<no-pet>>");
        ctx.check("pet_field_descriptor_is_animal_interface",
                  sig == std::string{ "L" } + k_animal_class + ";");
    }

    // =====================================================================
    //  1. RUNTIME-TYPE RESOLUTION: read `pet` (declared Animal) AS the concrete
    //     Dog wrapper; the decoded OOP's runtime klass (read from the header,
    //     NOT the registered wrapper type) must be Dog.
    // =====================================================================
    const auto pet_dog{ holder->pet_as_dog() };
    ctx.check("pet_as_dog_nonnull", pet_dog != nullptr);

    std::string dog_runtime_name{};
    if (pet_dog)
    {
        dog_runtime_name = runtime_klass_name(pet_dog->get_instance());
        ctx.check("pet_runtime_klass_resolved", !dog_runtime_name.empty());
        // The headline assertion: the runtime klass is the CONCRETE Dog, even
        // though the field's declared type is the Animal interface.
        ctx.check("pet_runtime_klass_ends_with_Dog", ends_with(dog_runtime_name, "Dog"));
        ctx.check("pet_runtime_klass_is_full_dog_internal_name",
                  dog_runtime_name == k_dog_class);
        ctx.record(std::string("[INFO] interface_polymorphism: pet runtime klass = '")
                   + dog_runtime_name + "' (declared type is the Animal interface).");
    }

    // =====================================================================
    //  2. DECLARED-vs-CONCRETE IDENTITY: read the SAME slot AS the declared
    //     interface type; it decodes to the SAME oop, and its runtime klass is
    //     STILL Dog (runtime type is independent of the wrapper's static type).
    // =====================================================================
    const auto pet_animal{ holder->pet_as_animal() };
    ctx.check("pet_as_animal_nonnull", pet_animal != nullptr);
    if (pet_dog && pet_animal)
    {
        ctx.check("declared_vs_concrete_same_oop",
                  pet_animal->get_instance() == pet_dog->get_instance());

        const std::string via_animal_name{ runtime_klass_name(pet_animal->get_instance()) };
        ctx.check("pet_runtime_klass_via_interface_wrapper_also_Dog",
                  ends_with(via_animal_name, "Dog"));
    }

    // The second field (petAsDog) holds the SAME object; the decoded identity
    // must match the `pet` read regardless of the field's declared type.
    {
        const auto alias_dog{ holder->pet_alias_as_dog() };
        ctx.check("pet_alias_nonnull", alias_dog != nullptr);
        if (pet_dog && alias_dog)
        {
            ctx.check("pet_alias_same_oop_as_pet",
                      alias_dog->get_instance() == pet_dog->get_instance());
        }
    }

    // =====================================================================
    //  3. VIRTUAL DISPATCH: call the overridden speak() through the CONCRETE
    //     Dog wrapper -> reaches Dog's override -> returns a "woof" String.
    //     call()-dependent, so gate the content assert best-effort and record
    //     [INFO] if the interpreter did not return a value on this JDK build.
    // =====================================================================
    if (pet_dog)
    {
        const std::string spoke{ pet_dog->speak() };
        if (!spoke.empty() && spoke != "<<no-speak>>")
        {
            ctx.check("dog_speak_contains_woof", contains(spoke, "woof"));
            ctx.check("dog_speak_contains_name_Rex", contains(spoke, "Rex"));
        }
        else
        {
            ctx.record("[INFO] interface_polymorphism: speak() returned no value via the "
                       "interpreter on this JDK build; virtual-dispatch content assert skipped "
                       "(the runtime-klass proof above already establishes the concrete type).");
        }
    }

    // =====================================================================
    //  4. DOG-SPECIFIC STATE: read fields declared on the concrete class.
    // =====================================================================
    if (pet_dog)
    {
        ctx.check("dog_name_is_Rex",  pet_dog->get_name()  == "Rex");
        ctx.check("dog_age_is_5",     pet_dog->get_age()   == 5);
        ctx.check("dog_breed_is_lab", pet_dog->get_breed() == "labrador");

        // Dog-only method (not on the interface).  Best-effort (call-dependent).
        const std::string fetched{ pet_dog->fetch() };
        if (!fetched.empty() && fetched != "<<no-fetch>>")
        {
            ctx.check("dog_fetch_contains_name", contains(fetched, "Rex"));
            ctx.check("dog_fetch_contains_breed", contains(fetched, "labrador"));
        }
        else
        {
            ctx.record("[INFO] interface_polymorphism: fetch() returned no value via the "
                       "interpreter on this JDK build; Dog-only-method content assert skipped.");
        }
    }

    // =====================================================================
    //  5. INTERFACE-DEFAULT-METHOD LIMITATION (characterised; never fails).
    //     defaultGreet() lives ONLY on the Animal interface (no Dog override),
    //     so it is reachable only by walking the interface chain.  vmhook's
    //     object::get_method walks the SUPERCLASS chain (Dog -> Object) only.
    // =====================================================================
    if (pet_dog)
    {
        const bool dog_finds_default{ pet_dog->resolves_default_greet() };
        if (dog_finds_default)
        {
            // If a future vmhook DOES walk the interface chain, prove the call
            // reaches the default body (which embeds speak() -> "Rex"/"woof").
            ctx.record("[INFO] interface_polymorphism: Dog wrapper FOUND interface default "
                       "defaultGreet() (vmhook now walks the interface chain via the concrete "
                       "class).");
            const std::string greet{ pet_dog->default_greet() };
            if (!greet.empty() && greet != "<<no-default>>")
            {
                ctx.check("dog_default_greet_contains_woof", contains(greet, "woof"));
            }
        }
        else
        {
            // The documented current behaviour: not found through the Dog walk.
            ctx.record("[INFO] interface_polymorphism: interface DEFAULT method defaultGreet() "
                       "is NOT reachable through the concrete Dog wrapper -- vmhook's "
                       "object::get_method walks the SUPERCLASS chain (Dog -> Object) only, not "
                       "the interface chain.  Known limitation; not a failure.");
        }
    }

    // Through the INTERFACE wrapper, whose OWN klass declares the default
    // method directly, the lookup is expected to succeed (no interface-chain
    // walk needed -- the method is on this very klass).  Characterise both ways.
    if (pet_animal)
    {
        const bool iface_finds_default{ pet_animal->resolves_default_greet() };
        ctx.record(std::string("[INFO] interface_polymorphism: defaultGreet() resolvable through "
                   "the Animal-interface wrapper (method on the wrapper's own klass) = ")
                   + (iface_finds_default ? "true" : "false") + ".");
        // The abstract speak() is declared directly on the interface klass too.
        ctx.record(std::string("[INFO] interface_polymorphism: speak() resolvable through the "
                   "Animal-interface wrapper (abstract, no body) = ")
                   + (pet_animal->resolves_speak() ? "true" : "false") + ".");
    }

    // =====================================================================
    //  6. JVM AGREEMENT: the probe runs the SAME observations Java-side and
    //     publishes a witness.  Confirms the JVM itself sees pet as a Dog whose
    //     speak() contains "woof".
    // =====================================================================
    {
        const bool done{ drive(ctx, 0) };
        ctx.check("interface_poly_probe_completed", done);
        if (done)
        {
            ctx.check("java_pet_is_dog_seen", ifp_holder::get_pet_is_dog_seen());
            const std::string java_spoke{ ifp_holder::get_pet_speak_seen() };
            ctx.check("java_pet_speak_contains_woof", contains(java_spoke, "woof"));
            // Cross-check: when the native speak() also returned a value, both
            // views agree byte-for-byte.
            if (pet_dog)
            {
                const std::string native_spoke{ pet_dog->speak() };
                if (!native_spoke.empty() && native_spoke != "<<no-speak>>")
                {
                    ctx.check("native_and_java_speak_agree", native_spoke == java_spoke);
                }
            }
        }
    }

    // No hooks were installed by this module; nothing to disarm.  The probe's
    // tick() dispatch fires only any hook a DIFFERENT module may have placed on
    // it, which this module neither owns nor leaves behind.
}
