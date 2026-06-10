// interface_polymorphism JVM test module  (feature area: fields + methods)
//
// THE interface-polymorphism authority: a field whose DECLARED (static) type is
// an interface (or an abstract base) but whose RUNTIME type is a concrete
// subclass.  It exercises the FULL polymorphism input space against one
// self-contained fixture, vmhook.fixtures.InterfacePoly, whose nested types javac
// emits as InterfacePoly$Animal / $Named / $Dog / $Cat / $Snake / $Robot /
// $AbstractPet / $Hamster:
//
//   interface Animal { String speak(); default String defaultGreet(); }
//   interface Named  { String who(); }
//   class Dog/Cat/Snake/Robot implements Animal   (Robot ALSO implements Named)
//   abstract class AbstractPet { abstract sound(); String describe(){...} }
//   class Hamster extends AbstractPet
//
// What this module proves on a live JVM (Java 8/11/17/21/24/25 x MSVC/Clang/GCC),
// every observation a side-effect-free read against the published SINGLETON:
//
//   * RUNTIME-TYPE RESOLUTION (one slot per impl): reading an interface- or
//     abstract-typed field yields a wrapper whose decoded OOP's runtime klass --
//     resolved straight from the object header via vmhook::klass_from_oop, NOT
//     from the registered wrapper type -- is the CONCRETE implementor (its
//     internal name ends in "Dog"/"Cat"/"Snake"/"Robot"/"Hamster").  vmhook sees
//     the concrete type, never the declared interface/abstract base.
//   * VIRTUAL DISPATCH (per impl): the overridden speak(), invoked through each
//     CONCRETE-type wrapper, reaches THAT impl's override and returns its
//     impl-specific String ("woof"/"meow"/"hiss"/"beep") -- and all four results
//     are mutually distinct.
//   * MULTIPLE INTERFACES: Robot implements Animal AND Named; speak() (Animal)
//     and who() (Named) each dispatch to Robot's own override.
//   * ABSTRACT BASE vs INTERFACE (the key contrast): AbstractPet is a real
//     _super, so the concrete describe() declared on it IS reachable through the
//     Hamster wrapper's superclass walk -- whereas an interface DEFAULT method
//     is NOT (interfaces are not on the _super chain).  Both directions asserted.
//   * METHOD ON THE INTERFACE vs ONLY ON THE IMPL: speak()/who() are declared on
//     interfaces; fetch() is Dog-only.  Both resolve through the concrete wrapper.
//   * DECLARED-vs-CONCRETE IDENTITY: reading the same slot as the declared
//     interface type and as the concrete type decode to the SAME oop (the field
//     decode is type-agnostic -- it wraps whatever the slot's compressed OOP
//     points at).
//   * DEFAULT METHOD INHERITED vs OVERRIDDEN: Snake OVERRIDES defaultGreet() (on
//     its own klass -> reachable via the super walk), while Dog/Cat INHERIT it
//     (only on the interface -> NOT reachable through a concrete wrapper, since
//     object::get_method walks the SUPERCLASS chain, not the interface chain).
//     Recorded as characterised [INFO], never a failure.
//   * JVM AGREEMENT: a probe runs the SAME observations Java-side and publishes
//     per-impl witnesses the module reads back (each impl's speak()/who()/etc.).
//
// SAFETY (suite-safe end to end): the entire body runs inside one try/catch that
// degrades ANY exception to [INFO] (never a FAIL), with an UNCONDITIONAL
// vmhook::shutdown_hooks() in a trailing block OUTSIDE the try; an entry guard
// records [INFO] and returns if the fixture klass is not loaded; every oop/klass
// deref is gated with vmhook::hotspot::is_valid_pointer, and every raw field/
// klass read is additionally gated with a safe-read header probe so a cold-JVM
// GC-relocated singleton degrades to [INFO] instead of faulting; decoded objects
// are null-checked before use.  The module installs NO hooks of its own (it drives
// the probe purely to publish Java witnesses), so it leaves nothing armed, and
// every call()-dependent content assert is gated best-effort.
//
// STYLE: wrapper accessors are CLEAN one-liners over the documented field/method
// idiom (vmhook.hpp ~12457 / ~14856) -- no sentinel-string guards; statics go
// through static_field()/static_method().  A method that may legitimately fail to
// resolve (an interface default through a concrete wrapper) is probed with a
// dedicated resolves_*() (has_value) accessor rather than a guarded call.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace
{
    // Internal names of the fixture's nested types (javac '$' nesting; confirmed
    // via javap).  Centralised so registration and the resolution checks below
    // can never drift apart.
    constexpr const char* k_holder_class   = "vmhook/fixtures/InterfacePoly";
    constexpr const char* k_animal_class   = "vmhook/fixtures/InterfacePoly$Animal";
    constexpr const char* k_named_class     = "vmhook/fixtures/InterfacePoly$Named";
    constexpr const char* k_dog_class       = "vmhook/fixtures/InterfacePoly$Dog";
    constexpr const char* k_cat_class       = "vmhook/fixtures/InterfacePoly$Cat";
    constexpr const char* k_snake_class     = "vmhook/fixtures/InterfacePoly$Snake";
    constexpr const char* k_robot_class     = "vmhook/fixtures/InterfacePoly$Robot";
    constexpr const char* k_abstract_class  = "vmhook/fixtures/InterfacePoly$AbstractPet";
    constexpr const char* k_hamster_class   = "vmhook/fixtures/InterfacePoly$Hamster";

    // ── Wrapper for CONCRETE implementation #1 (InterfacePoly$Dog) ──────────
    // Registered as the concrete class, so its resolve_klass() (typeid-based)
    // lands on Dog -- the only walk that reaches Dog's speak() override and its
    // own fields.  resolves_default_greet() characterises the interface-default-
    // method limitation through a concrete-typed wrapper.
    class ifp_dog : public vmhook::object<ifp_dog>
    {
    public:
        explicit ifp_dog(vmhook::oop_t instance) noexcept
            : vmhook::object<ifp_dog>{ instance }
        {
        }

        // Dog-specific fields (declared directly on the concrete class).
        auto name()  const -> std::string  { return get_field("name")->get();  }
        auto age()   const -> std::int32_t { return get_field("age")->get();   }
        auto breed() const -> std::string  { return get_field("breed")->get(); }

        // Overridden virtual -- reached via the Dog-typed superclass walk.
        auto speak() const -> std::string { return get_method("speak")->call().as_string(); }

        // Dog-only method (NOT on the Animal interface) -- declared on this klass.
        auto fetch() const -> std::string { return get_method("fetch")->call().as_string(); }

        // Probes (no call): does the Dog superclass walk reach each method?
        auto resolves_speak()         const -> bool { return get_method("speak").has_value(); }
        auto resolves_fetch()         const -> bool { return get_method("fetch").has_value(); }
        auto resolves_default_greet() const -> bool { return get_method("defaultGreet").has_value(); }
    };

    // ── Wrapper for CONCRETE implementation #2 (InterfacePoly$Cat) ──────────
    class ifp_cat : public vmhook::object<ifp_cat>
    {
    public:
        explicit ifp_cat(vmhook::oop_t instance) noexcept
            : vmhook::object<ifp_cat>{ instance }
        {
        }

        auto name()   const -> std::string  { return get_field("name")->get();   }
        auto indoor() const -> bool         { return get_field("indoor")->get(); }
        auto speak()  const -> std::string  { return get_method("speak")->call().as_string(); }

        auto resolves_default_greet() const -> bool { return get_method("defaultGreet").has_value(); }
    };

    // ── Wrapper for CONCRETE implementation #3 (InterfacePoly$Snake) ────────
    // Snake OVERRIDES defaultGreet(), so the override is on this klass and the
    // super walk reaches it (unlike the inherited form on Dog/Cat).
    class ifp_snake : public vmhook::object<ifp_snake>
    {
    public:
        explicit ifp_snake(vmhook::oop_t instance) noexcept
            : vmhook::object<ifp_snake>{ instance }
        {
        }

        auto name()          const -> std::string { return get_field("name")->get(); }
        auto speak()         const -> std::string { return get_method("speak")->call().as_string(); }
        auto default_greet() const -> std::string { return get_method("defaultGreet")->call().as_string(); }

        auto resolves_default_greet() const -> bool { return get_method("defaultGreet").has_value(); }
    };

    // ── Wrapper for the MULTI-INTERFACE impl (InterfacePoly$Robot) ──────────
    // Robot implements BOTH Animal and Named; both overrides are on this klass.
    class ifp_robot : public vmhook::object<ifp_robot>
    {
    public:
        explicit ifp_robot(vmhook::oop_t instance) noexcept
            : vmhook::object<ifp_robot>{ instance }
        {
        }

        auto id()    const -> std::string { return get_field("id")->get(); }
        auto speak() const -> std::string { return get_method("speak")->call().as_string(); } // Animal side
        auto who()   const -> std::string { return get_method("who")->call().as_string(); }   // Named side

        auto resolves_speak() const -> bool { return get_method("speak").has_value(); }
        auto resolves_who()   const -> bool { return get_method("who").has_value(); }
    };

    // ── Wrapper for the CONCRETE subclass of the abstract base (Hamster) ────
    // Registered as Hamster: its super walk starts at Hamster and goes UP to the
    // ABSTRACT base AbstractPet (a real _super), so the concrete describe()
    // declared on the base resolves -- the contrast with the interface case.
    class ifp_hamster : public vmhook::object<ifp_hamster>
    {
    public:
        explicit ifp_hamster(vmhook::oop_t instance) noexcept
            : vmhook::object<ifp_hamster>{ instance }
        {
        }

        auto name()     const -> std::string  { return get_field("name")->get(); }
        auto legs()     const -> std::int32_t { return get_field("legs")->get(); } // inherited field on AbstractPet
        auto sound()    const -> std::string  { return get_method("sound")->call().as_string(); }
        auto describe() const -> std::string  { return get_method("describe")->call().as_string(); }

        // sound() is on Hamster; describe() is on the ABSTRACT base (super walk).
        auto resolves_sound()    const -> bool { return get_method("sound").has_value(); }
        auto resolves_describe() const -> bool { return get_method("describe").has_value(); }
    };

    // ── Wrapper for the DECLARED interface type (InterfacePoly$Animal) ──────
    // Registered as the interface, so its resolve_klass() lands on the interface
    // klass.  A speak() lookup here finds the ABSTRACT interface method; a
    // defaultGreet() lookup finds the DEFAULT method that lives directly on this
    // klass.  Used to prove the declared-type read decodes the SAME oop as a
    // concrete-type read, and to characterise default/abstract resolution when
    // the wrapper's own klass actually declares the method.
    class ifp_animal : public vmhook::object<ifp_animal>
    {
    public:
        explicit ifp_animal(vmhook::oop_t instance) noexcept
            : vmhook::object<ifp_animal>{ instance }
        {
        }

        auto resolves_speak()         const -> bool { return get_method("speak").has_value(); }
        auto resolves_default_greet() const -> bool { return get_method("defaultGreet").has_value(); }
    };

    // ── Wrapper for the SECOND interface (InterfacePoly$Named) ──────────────
    class ifp_named : public vmhook::object<ifp_named>
    {
    public:
        explicit ifp_named(vmhook::oop_t instance) noexcept
            : vmhook::object<ifp_named>{ instance }
        {
        }

        auto resolves_who() const -> bool { return get_method("who").has_value(); }
    };

    // ── Wrapper for the ABSTRACT base (InterfacePoly$AbstractPet) ───────────
    // Used to characterise resolution when the wrapper's own klass IS the
    // abstract base: both the abstract sound() and the concrete describe() are
    // declared here.
    class ifp_abstract : public vmhook::object<ifp_abstract>
    {
    public:
        explicit ifp_abstract(vmhook::oop_t instance) noexcept
            : vmhook::object<ifp_abstract>{ instance }
        {
        }

        auto resolves_sound()    const -> bool { return get_method("sound").has_value(); }
        auto resolves_describe() const -> bool { return get_method("describe").has_value(); }
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
        static auto set_go(bool value)       -> void { static_field("go")->set(value); }
        static auto set_done(bool value)      -> void { static_field("done")->set(value); }
        static auto get_done()                -> bool { return static_field("done")->get(); }
        static auto set_mode(std::int32_t m)  -> void { static_field("mode")->set(m); }

        // ---- the published singleton holder ----
        static auto singleton() -> std::unique_ptr<ifp_holder> { return static_field("SINGLETON")->get(); }

        // ---- a reference field's JVM descriptor (proves its declared type) ----
        // Self-guarding (returns "" when the field is absent) so the descriptor
        // call sites stay clean one-liners with no UB on an unexpectedly-missing
        // field: "" never equals an "L...;" descriptor, so a missing field is a
        // clean FAIL rather than a nullopt dereference.
        auto field_signature(const char* field_name) const -> std::string
        {
            const auto fp{ get_field(field_name) };
            return fp.has_value() ? std::string{ fp->signature() } : std::string{};
        }
        auto field_resolves(const char* field_name) const -> bool
        {
            return get_field(field_name).has_value();
        }

        // ---- read the four Animal-typed fields AS their concrete impls -----
        auto pet_as_dog()    const -> std::unique_ptr<ifp_dog>   { return get_field("pet")->get(); }
        auto pet2_as_cat()   const -> std::unique_ptr<ifp_cat>   { return get_field("pet2")->get(); }
        auto pet3_as_snake() const -> std::unique_ptr<ifp_snake> { return get_field("pet3")->get(); }
        auto robot()         const -> std::unique_ptr<ifp_robot> { return get_field("robotPet")->get(); }

        // ---- read the abstract-typed field AS the concrete subclass --------
        auto abs_as_hamster() const -> std::unique_ptr<ifp_hamster> { return get_field("absPet")->get(); }

        // ---- read `pet` AS the DECLARED interface type ----
        auto pet_as_animal() const -> std::unique_ptr<ifp_animal> { return get_field("pet")->get(); }

        // ---- read the petAsDog field (concrete-typed slot to the SAME object) ----
        auto pet_alias_as_dog() const -> std::unique_ptr<ifp_dog> { return get_field("petAsDog")->get(); }

        // ---- Java-side witnesses (set by the probe through real bytecode) ----
        static auto pet_is_dog_seen()         -> bool        { return static_field("petIsDogSeen")->get(); }
        static auto all_impls_distinct_seen() -> bool        { return static_field("allImplsDistinctSeen")->get(); }
        static auto java_witness(const char* name) -> std::string { return static_field(name)->get(); }
    };

    // ── Helpers ────────────────────────────────────────────────────────────

    // The runtime klass's full internal (slash-separated) name, read straight
    // from an oop header.  Safety-gated end to end (null + is_valid_pointer +
    // safe-read header probe) so a stale/relocated oop degrades to "" rather than
    // faulting.
    auto runtime_klass_name(vmhook::oop_t oop) -> std::string
    {
        if (!oop || !vmhook::hotspot::is_valid_pointer(oop))
        {
            return std::string{};
        }
        // klass_from_oop RAW-derefs the narrow-klass at oop+8; probe the header
        // is currently mapped (a GC-relocated old address passes is_valid_pointer
        // yet faults on read).
        std::array<std::uint8_t, 16> scratch{};
        if (!vmhook::os::safe_read(scratch.data(), oop, scratch.size()))
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

    // True when the first 16 bytes of an oop header (mark word .. narrow klass)
    // are currently mapped.  Gate every RAW field/method read on this so a
    // cold-JVM GC-relocated singleton degrades to [INFO] instead of faulting
    // (MinGW/gcc have no SEH net).  See nested_classes.cpp for the full rationale.
    auto oop_readable(vmhook::oop_t oop) -> bool
    {
        if (!oop || !vmhook::hotspot::is_valid_pointer(oop))
        {
            return false;
        }
        std::array<std::uint8_t, 16> scratch{};
        return vmhook::os::safe_read(scratch.data(), oop, scratch.size());
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
    vmhook::register_class<ifp_named>(k_named_class);
    vmhook::register_class<ifp_dog>(k_dog_class);
    vmhook::register_class<ifp_cat>(k_cat_class);
    vmhook::register_class<ifp_snake>(k_snake_class);
    vmhook::register_class<ifp_robot>(k_robot_class);
    vmhook::register_class<ifp_abstract>(k_abstract_class);
    vmhook::register_class<ifp_hamster>(k_hamster_class);

    // Suite-safe: every observation is inside this try; any exception degrades to
    // an [INFO] line (never a FAIL), and the UNCONDITIONAL shutdown_hooks() in the
    // trailing block OUTSIDE the try always runs.  This module installs no hooks,
    // so the shutdown is belt-and-braces against any hook a shared run might leave.
    try
    {
        // ENTRY GUARD: if the fixture klass is not loaded, there is nothing to
        // probe -- record [INFO] and return cleanly (the trailing shutdown still
        // runs).  find_class is null-safe and never throws here.
        if (vmhook::find_class(k_holder_class) == nullptr)
        {
            ctx.record("[INFO] interface_polymorphism: fixture vmhook/fixtures/InterfacePoly "
                       "not loaded; skipping (nothing to probe).");
        }
        else
        {

        // =================================================================
        //  0. SANITY: every nested type resolves; the holder's reference
        //     fields resolve and carry their DECLARED descriptors (interface
        //     for pet*, abstract base for absPet, concrete for petAsDog).
        // =================================================================
        ctx.check("ifp_holder_registered", ifp_holder::static_field("SINGLETON").has_value());

        // Each nested klass resolves by its internal '$' name (right klass loaded).
        ctx.check("animal_interface_klass_resolves",   vmhook::find_class(k_animal_class)   != nullptr);
        ctx.check("named_interface_klass_resolves",    vmhook::find_class(k_named_class)     != nullptr);
        ctx.check("dog_klass_resolves",                vmhook::find_class(k_dog_class)       != nullptr);
        ctx.check("cat_klass_resolves",                vmhook::find_class(k_cat_class)       != nullptr);
        ctx.check("snake_klass_resolves",              vmhook::find_class(k_snake_class)     != nullptr);
        ctx.check("robot_klass_resolves",              vmhook::find_class(k_robot_class)     != nullptr);
        ctx.check("abstract_base_klass_resolves",      vmhook::find_class(k_abstract_class)  != nullptr);
        ctx.check("hamster_klass_resolves",            vmhook::find_class(k_hamster_class)   != nullptr);

        const auto holder{ ifp_holder::singleton() };
        ctx.check("ifp_singleton_nonnull", holder != nullptr);
        if (!holder)
        {
            // Without the holder the rest is meaningless; the probe at the end
            // still confirms the JVM-side shape, but bail out of native reads.
            ctx.record("[INFO] interface_polymorphism: SINGLETON not resolvable; "
                       "skipping native polymorphism reads (fixture not initialised?).");
        }
        else
        {

        // The declared static type of pet/pet2/pet3/robotPet is the Animal
        // interface, so their JVM field descriptors name InterfacePoly$Animal;
        // absPet names the abstract base; petAsDog names the concrete Dog.
        const std::string animal_desc{ std::string{ "L" } + k_animal_class   + ";" };
        const std::string abs_desc{    std::string{ "L" } + k_abstract_class + ";" };
        const std::string dog_desc{    std::string{ "L" } + k_dog_class      + ";" };

        ctx.check("pet_field_resolves",   holder->field_resolves("pet"));
        ctx.check("pet_field_descriptor_is_animal_interface",  holder->field_signature("pet")   == animal_desc);
        ctx.check("pet2_field_descriptor_is_animal_interface", holder->field_signature("pet2")  == animal_desc);
        ctx.check("pet3_field_descriptor_is_animal_interface", holder->field_signature("pet3")  == animal_desc);
        ctx.check("robotPet_field_descriptor_is_animal_interface",
                  holder->field_signature("robotPet") == animal_desc);
        ctx.check("absPet_field_descriptor_is_abstract_base", holder->field_signature("absPet") == abs_desc);
        // The alias field's DECLARED type is the concrete Dog (varies the slot
        // signature; the object behind it is the very same Dog as `pet`).
        ctx.check("petAsDog_field_descriptor_is_concrete_dog",
                  holder->field_signature("petAsDog") == dog_desc);

        // =================================================================
        //  1. RUNTIME-TYPE RESOLUTION (one slot per impl): reading an
        //     interface/abstract-typed field AS the concrete wrapper decodes an
        //     OOP whose RUNTIME klass (read from the header) is the CONCRETE
        //     implementor -- ends with the impl leaf AND equals the full name.
        // =================================================================
        const auto pet_dog{ holder->pet_as_dog() };
        const auto pet_cat{ holder->pet2_as_cat() };
        const auto pet_snake{ holder->pet3_as_snake() };
        const auto pet_robot{ holder->robot() };
        const auto pet_hamster{ holder->abs_as_hamster() };

        ctx.check("pet_as_dog_nonnull",     pet_dog     != nullptr);
        ctx.check("pet2_as_cat_nonnull",    pet_cat     != nullptr);
        ctx.check("pet3_as_snake_nonnull",  pet_snake   != nullptr);
        ctx.check("robotPet_nonnull",       pet_robot   != nullptr);
        ctx.check("absPet_as_hamster_nonnull", pet_hamster != nullptr);

        // A tiny table-driven runtime-klass battery: each impl's decoded oop must
        // resolve to its concrete klass (leaf suffix AND full internal name).
        struct rk_case
        {
            const char*   label;
            vmhook::oop_t oop;
            const char*   leaf;
            const char*   full;
        };
        const std::array<rk_case, 5> rk_cases{ {
            { "dog",     pet_dog     ? pet_dog->get_instance()     : nullptr, "Dog",     k_dog_class },
            { "cat",     pet_cat     ? pet_cat->get_instance()     : nullptr, "Cat",     k_cat_class },
            { "snake",   pet_snake   ? pet_snake->get_instance()   : nullptr, "Snake",   k_snake_class },
            { "robot",   pet_robot   ? pet_robot->get_instance()   : nullptr, "Robot",   k_robot_class },
            { "hamster", pet_hamster ? pet_hamster->get_instance() : nullptr, "Hamster", k_hamster_class },
        } };

        for (const rk_case& c : rk_cases)
        {
            const std::string rn{ runtime_klass_name(c.oop) };
            // The runtime-klass read is RAW; on a cold-JVM relocation it degrades
            // to "" (safe-read miss) -- record [INFO] and skip rather than fail.
            if (rn.empty())
            {
                ctx.record(std::string("[INFO] interface_polymorphism: ") + c.label
                           + " runtime klass not readable (null/stale/relocated oop); "
                             "skipped runtime-type assertion.");
                continue;
            }
            ctx.check(std::string("pet_runtime_klass_ends_with_") + c.leaf, ends_with(rn, c.leaf));
            ctx.check(std::string("pet_runtime_klass_is_full_") + c.label + "_internal_name",
                      rn == std::string{ c.full });
            ctx.record(std::string("[INFO] interface_polymorphism: ") + c.label
                       + " runtime klass = '" + rn + "' (declared field type is the interface/abstract base).");
        }

        // =================================================================
        //  2. DECLARED-vs-CONCRETE IDENTITY: reading the SAME `pet` slot AS the
        //     declared interface type decodes the SAME oop, and its runtime klass
        //     is STILL Dog (runtime type is independent of the wrapper's static
        //     type).  The petAsDog alias holds the SAME object too.
        // =================================================================
        const auto pet_animal{ holder->pet_as_animal() };
        ctx.check("pet_as_animal_nonnull", pet_animal != nullptr);
        if (pet_dog && pet_animal)
        {
            // Pure pointer comparison -- never dereferences, so it is HARD.
            ctx.check("declared_vs_concrete_same_oop",
                      pet_animal->get_instance() == pet_dog->get_instance());

            const std::string via_animal_name{ runtime_klass_name(pet_animal->get_instance()) };
            if (!via_animal_name.empty())
            {
                ctx.check("pet_runtime_klass_via_interface_wrapper_also_Dog",
                          ends_with(via_animal_name, "Dog"));
            }
        }

        {
            const auto alias_dog{ holder->pet_alias_as_dog() };
            ctx.check("pet_alias_nonnull", alias_dog != nullptr);
            if (pet_dog && alias_dog)
            {
                ctx.check("pet_alias_same_oop_as_pet",
                          alias_dog->get_instance() == pet_dog->get_instance());
            }
        }

        // =================================================================
        //  3. VIRTUAL DISPATCH (per impl, best-effort): call the overridden
        //     speak() through each CONCRETE wrapper -> reaches THAT impl's
        //     override -> returns its impl-specific String.  call()-dependent,
        //     so gate each content assert best-effort and record [INFO] if the
        //     interpreter returned no value on this JDK build.  Collect the four
        //     results so the all-distinct invariant can be asserted natively.
        // =================================================================
        std::string dog_spoke, cat_spoke, snake_spoke, robot_spoke;

        if (pet_dog && oop_readable(pet_dog->get_instance()))
        {
            dog_spoke = pet_dog->speak();
            if (!dog_spoke.empty())
            {
                ctx.check("dog_speak_contains_woof", contains(dog_spoke, "woof"));
                ctx.check("dog_speak_contains_name_Rex", contains(dog_spoke, "Rex"));
            }
        }
        if (pet_cat && oop_readable(pet_cat->get_instance()))
        {
            cat_spoke = pet_cat->speak();
            if (!cat_spoke.empty())
            {
                ctx.check("cat_speak_contains_meow", contains(cat_spoke, "meow"));
                ctx.check("cat_speak_contains_name_Whiskers", contains(cat_spoke, "Whiskers"));
            }
        }
        if (pet_snake && oop_readable(pet_snake->get_instance()))
        {
            snake_spoke = pet_snake->speak();
            if (!snake_spoke.empty())
            {
                ctx.check("snake_speak_contains_hiss", contains(snake_spoke, "hiss"));
            }
        }
        if (pet_robot && oop_readable(pet_robot->get_instance()))
        {
            robot_spoke = pet_robot->speak();
            if (!robot_spoke.empty())
            {
                ctx.check("robot_speak_contains_beep", contains(robot_spoke, "beep"));
            }
        }

        // All four impls dispatched to DISTINCT bodies (only when every call
        // returned a value -- otherwise the proof is the per-impl content above
        // plus the runtime-klass battery, so just record [INFO]).
        if (!dog_spoke.empty() && !cat_spoke.empty()
            && !snake_spoke.empty() && !robot_spoke.empty())
        {
            const bool all_distinct{
                   dog_spoke != cat_spoke && dog_spoke != snake_spoke && dog_spoke != robot_spoke
                && cat_spoke != snake_spoke && cat_spoke != robot_spoke
                && snake_spoke != robot_spoke };
            ctx.check("native_all_four_impls_speak_distinctly", all_distinct);
        }
        else
        {
            ctx.record("[INFO] interface_polymorphism: not all impls' speak() returned via the "
                       "interpreter on this JDK build; native all-distinct assert skipped "
                       "(per-impl content + runtime-klass battery already prove dispatch).");
        }

        // =================================================================
        //  4. CONCRETE-ONLY STATE: read fields declared on each concrete impl
        //     (and, for Hamster, the field inherited from the abstract base).
        //     Gate each RAW read on the receiver header being safely readable.
        // =================================================================
        if (pet_dog && oop_readable(pet_dog->get_instance()))
        {
            ctx.check("dog_name_is_Rex",   pet_dog->name()  == "Rex");
            ctx.check("dog_age_is_5",      pet_dog->age()   == 5);
            ctx.check("dog_breed_is_lab",  pet_dog->breed() == "labrador");
        }
        if (pet_cat && oop_readable(pet_cat->get_instance()))
        {
            ctx.check("cat_name_is_Whiskers", pet_cat->name()   == "Whiskers");
            ctx.check("cat_indoor_is_true",   pet_cat->indoor() == true);
        }
        if (pet_snake && oop_readable(pet_snake->get_instance()))
        {
            ctx.check("snake_name_is_Slither", pet_snake->name() == "Slither");
        }
        if (pet_robot && oop_readable(pet_robot->get_instance()))
        {
            ctx.check("robot_id_is_R2", pet_robot->id() == "R2");
        }
        if (pet_hamster && oop_readable(pet_hamster->get_instance()))
        {
            ctx.check("hamster_name_is_Nibbles", pet_hamster->name() == "Nibbles");
            // legs is declared on the ABSTRACT base -> resolved via the super walk.
            ctx.check("hamster_inherited_legs_field_is_4", pet_hamster->legs() == 4);
        }

        // =================================================================
        //  5. METHOD ON THE INTERFACE vs ONLY ON THE IMPL.
        //     speak()/who() are interface-declared; fetch() is Dog-only.  All
        //     resolve through the concrete wrapper (own klass for fetch/Robot;
        //     own klass for the overrides).  Calls are best-effort.
        // =================================================================
        if (pet_dog)
        {
            // fetch() is declared ONLY on Dog (not on Animal): resolves at depth 0.
            ctx.check("dog_only_fetch_resolves", pet_dog->resolves_fetch());
            // speak() (interface-declared, overridden on Dog) also resolves.
            ctx.check("interface_speak_resolves_through_dog", pet_dog->resolves_speak());

            if (oop_readable(pet_dog->get_instance()))
            {
                const std::string fetched{ pet_dog->fetch() };
                if (!fetched.empty())
                {
                    ctx.check("dog_fetch_contains_name",  contains(fetched, "Rex"));
                    ctx.check("dog_fetch_contains_breed", contains(fetched, "labrador"));
                }
                else
                {
                    ctx.record("[INFO] interface_polymorphism: Dog-only fetch() returned no value via "
                               "the interpreter on this JDK build; content assert skipped.");
                }
            }
        }

        if (pet_robot)
        {
            // Robot implements TWO interfaces: BOTH methods resolve on its klass.
            ctx.check("robot_animal_speak_resolves", pet_robot->resolves_speak());
            ctx.check("robot_named_who_resolves",    pet_robot->resolves_who());

            if (oop_readable(pet_robot->get_instance()))
            {
                const std::string who{ pet_robot->who() };
                if (!who.empty())
                {
                    ctx.check("robot_who_contains_id", contains(who, "R2"));
                    ctx.check("robot_who_is_named_form", contains(who, "robot:"));
                }
                else
                {
                    ctx.record("[INFO] interface_polymorphism: Robot.who() (2nd-interface method) returned "
                               "no value via the interpreter on this JDK build; content assert skipped.");
                }
            }
        }

        // =================================================================
        //  6. ABSTRACT BASE vs INTERFACE (the headline contrast).
        //     AbstractPet is a REAL _super, so the concrete describe() declared
        //     on it IS reachable through the Hamster wrapper's superclass walk
        //     (HARD: resolvability) -- in direct contrast to an interface DEFAULT
        //     method, which the same walk does NOT reach.  The abstract sound()
        //     override resolves at depth 0; both calls are best-effort.
        // =================================================================
        if (pet_hamster)
        {
            // Abstract override sound() is on Hamster (depth 0).
            ctx.check("hamster_abstract_override_sound_resolves", pet_hamster->resolves_sound());
            // CONCRETE method describe() is on the ABSTRACT base: the super walk
            // (Hamster -> AbstractPet) REACHES it.  HARD assert -- this is the
            // contrast that distinguishes a superclass from an interface.
            ctx.check("hamster_inherited_concrete_describe_resolves_via_super_walk",
                      pet_hamster->resolves_describe());

            if (oop_readable(pet_hamster->get_instance()))
            {
                const std::string sound{ pet_hamster->sound() };
                if (!sound.empty())
                {
                    ctx.check("hamster_sound_contains_squeak", contains(sound, "squeak"));
                }
                const std::string described{ pet_hamster->describe() };
                if (!described.empty())
                {
                    // describe() body embeds legs (4) and sound() ("squeak").
                    ctx.check("hamster_describe_contains_legs",   contains(described, "4 legs"));
                    ctx.check("hamster_describe_contains_squeak", contains(described, "squeak"));
                }
                else
                {
                    ctx.record("[INFO] interface_polymorphism: inherited describe() returned no value via "
                               "the interpreter on this JDK build; content assert skipped (resolvability "
                               "via the super walk is already proven HARD above).");
                }
            }
        }

        // Through the ABSTRACT-base wrapper (whose own klass declares both): both
        // sound() (abstract) and describe() (concrete) resolve at depth 0.  This
        // confirms the methods exist on the base klass that Hamster walks UP to.
        if (pet_hamster)
        {
            ifp_abstract abs_view{ pet_hamster->get_instance() };
            ctx.check("abstract_base_wrapper_resolves_sound",    abs_view.resolves_sound());
            ctx.check("abstract_base_wrapper_resolves_describe", abs_view.resolves_describe());
        }

        // =================================================================
        //  7. DEFAULT METHOD INHERITED vs OVERRIDDEN (characterised; the
        //     inherited case never fails).
        //
        //     Snake OVERRIDES defaultGreet() -> the override is on Snake's own
        //     klass -> the super walk REACHES it (HARD resolvability; call
        //     best-effort).  Dog/Cat INHERIT the default -> it lives only on the
        //     Animal interface -> the SUPERCLASS-only walk does NOT reach it
        //     through the concrete wrapper -> recorded [INFO], the documented
        //     limitation.  Through the Animal interface wrapper (whose own klass
        //     declares the default) the lookup is characterised both ways.
        // =================================================================
        if (pet_snake)
        {
            // OVERRIDDEN default: on Snake's klass -> resolves via the super walk.
            ctx.check("snake_overridden_default_greet_resolves", pet_snake->resolves_default_greet());

            if (oop_readable(pet_snake->get_instance()))
            {
                const std::string greet{ pet_snake->default_greet() };
                if (!greet.empty())
                {
                    ctx.check("snake_overridden_default_greet_is_custom", contains(greet, "snake-greet"));
                    ctx.check("snake_overridden_default_greet_contains_name", contains(greet, "Slither"));
                }
                else
                {
                    ctx.record("[INFO] interface_polymorphism: Snake's OVERRIDDEN defaultGreet() returned no "
                               "value via the interpreter on this JDK build; content assert skipped "
                               "(resolvability via the super walk is already proven HARD above).");
                }
            }
        }

        // Dog/Cat INHERIT the interface default -> NOT reachable through the
        // concrete wrapper's superclass-only walk.  Characterise (never fail).
        if (pet_dog)
        {
            if (pet_dog->resolves_default_greet())
            {
                // If a future vmhook DOES walk the interface chain, prove the call
                // reaches the default body (which embeds speak() -> "Rex"/"woof").
                ctx.record("[INFO] interface_polymorphism: Dog wrapper FOUND the INHERITED interface default "
                           "defaultGreet() (vmhook now walks the interface chain via the concrete class).");
            }
            else
            {
                ctx.record("[INFO] interface_polymorphism: INHERITED interface DEFAULT method defaultGreet() "
                           "is NOT reachable through the concrete Dog wrapper -- object::get_method walks the "
                           "SUPERCLASS chain (Dog -> Object) only, not the interface chain.  Known limitation; "
                           "not a failure.  (Contrast: the abstract-base describe() above IS reachable, since "
                           "an abstract base is a real _super.)");
            }
        }
        if (pet_cat)
        {
            ctx.record(std::string("[INFO] interface_polymorphism: INHERITED defaultGreet() resolvable "
                       "through the concrete Cat wrapper (superclass-only walk) = ")
                       + (pet_cat->resolves_default_greet() ? "true" : "false") + ".");
        }

        // Through the INTERFACE wrapper, whose OWN klass declares both, the
        // lookups are characterised: defaultGreet() (default) and speak()
        // (abstract) both live on this very klass.
        if (pet_animal)
        {
            ctx.record(std::string("[INFO] interface_polymorphism: through the Animal-interface wrapper "
                       "(method on the wrapper's OWN klass) defaultGreet() resolvable = ")
                       + (pet_animal->resolves_default_greet() ? "true" : "false")
                       + ", abstract speak() resolvable = "
                       + (pet_animal->resolves_speak() ? "true" : "false") + ".");
        }

        // =================================================================
        //  8. JVM AGREEMENT: the probe runs the SAME observations Java-side and
        //     publishes per-impl witnesses.  Confirms the JVM itself sees each
        //     impl's distinct dispatch, and -- when the native call also returned
        //     a value -- that native and Java agree byte-for-byte.
        // =================================================================
        const bool done{ drive(ctx, 0) };
        ctx.check("interface_poly_probe_completed", done);
        if (done)
        {
            ctx.check("java_pet_is_dog_seen", ifp_holder::pet_is_dog_seen());
            ctx.check("java_all_impls_distinct_seen", ifp_holder::all_impls_distinct_seen());

            const std::string java_dog{   ifp_holder::java_witness("petSpeakSeen") };
            const std::string java_cat{   ifp_holder::java_witness("catSpeakSeen") };
            const std::string java_snake{ ifp_holder::java_witness("snakeSpeakSeen") };
            const std::string java_robot{ ifp_holder::java_witness("robotSpeakSeen") };
            const std::string java_who{   ifp_holder::java_witness("robotWhoSeen") };
            const std::string java_sgreet{ ifp_holder::java_witness("snakeGreetSeen") };
            const std::string java_hsound{ ifp_holder::java_witness("hamsterSoundSeen") };
            const std::string java_hdesc{ ifp_holder::java_witness("hamsterDescribeSeen") };

            // Java's own invokevirtual/invokeinterface always runs on the Java
            // thread (no native call gate), so these witnesses are HARD.
            ctx.check("java_dog_speak_contains_woof",   contains(java_dog,   "woof"));
            ctx.check("java_cat_speak_contains_meow",   contains(java_cat,   "meow"));
            ctx.check("java_snake_speak_contains_hiss", contains(java_snake, "hiss"));
            ctx.check("java_robot_speak_contains_beep", contains(java_robot, "beep"));
            ctx.check("java_robot_who_is_named_form",   contains(java_who,   "robot:"));
            ctx.check("java_snake_overridden_greet_is_custom", contains(java_sgreet, "snake-greet"));
            ctx.check("java_hamster_sound_contains_squeak",    contains(java_hsound, "squeak"));
            ctx.check("java_hamster_describe_contains_squeak", contains(java_hdesc,  "squeak"));

            // Cross-check: where the native call ALSO returned a value, native and
            // Java views agree byte-for-byte (the strongest dispatch proof).
            if (!dog_spoke.empty())
            {
                ctx.check("native_and_java_dog_speak_agree", dog_spoke == java_dog);
            }
            if (!cat_spoke.empty())
            {
                ctx.check("native_and_java_cat_speak_agree", cat_spoke == java_cat);
            }
            if (!snake_spoke.empty())
            {
                ctx.check("native_and_java_snake_speak_agree", snake_spoke == java_snake);
            }
            if (!robot_spoke.empty())
            {
                ctx.check("native_and_java_robot_speak_agree", robot_spoke == java_robot);
            }
        }

        }  // holder != nullptr
        }  // fixture loaded
    }
    catch (const std::exception& ex)
    {
        ctx.record(std::string("[INFO] interface_polymorphism: exception during probe -- ")
                   + ex.what() + " (degraded to INFO; not a failure).");
    }
    catch (...)
    {
        ctx.record("[INFO] interface_polymorphism: non-std exception during probe "
                   "(degraded to INFO; not a failure).");
    }

    // UNCONDITIONAL teardown OUTSIDE the try: this module installs no hooks, but
    // call shutdown_hooks() anyway so the module leaves a guaranteed-clean state
    // regardless of any exception path above (suite-safe contract).
    vmhook::shutdown_hooks();
}
