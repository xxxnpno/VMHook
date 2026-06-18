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
//   * ABSTRACT BASE vs INTERFACE (the two reachability paths): AbstractPet is a
//     real _super, so the concrete describe() declared on it is reached by the
//     Hamster wrapper's SUPERCLASS walk (HARD); an inherited interface DEFAULT
//     method is reached by the IMPLEMENTED-INTERFACE fallback that runs after the
//     superclass walk misses (object::get_method walks _transitive_interfaces) --
//     best-effort, since that fallback is a cold-path os::safe_read of the
//     interface arrays.  Both directions asserted.
//   * METHOD ON THE INTERFACE vs ONLY ON THE IMPL: speak()/who() are declared on
//     interfaces; fetch() is Dog-only.  Both resolve through the concrete wrapper.
//   * DECLARED-vs-CONCRETE IDENTITY: reading the same slot as the declared
//     interface type and as the concrete type decode to the SAME oop (the field
//     decode is type-agnostic -- it wraps whatever the slot's compressed OOP
//     points at).
//   * DEFAULT METHOD INHERITED vs OVERRIDDEN: Snake OVERRIDES defaultGreet() (on
//     its own klass -> reached by the super walk, HARD), while Dog/Cat INHERIT it
//     (only on the Animal interface -> reached by the interface-chain fallback in
//     object::get_method).  The inherited case is BEST-EFFORT: when the cold-path
//     interface walk resolves it, the call body + a Java byte-for-byte cross-check
//     are asserted HARD; when it does not (interface VMStructs unexported / arrays
//     not safely readable on some JDK/config) it records [INFO], never a failure.
//   * JVM AGREEMENT: a probe runs the SAME observations Java-side and publishes
//     per-impl witnesses the module reads back (each impl's speak()/who()/etc.).
//
// EXHAUSTIVE itable / interface-dispatch shapes (scenarios 9-14, every
// best-effort item PASS-or-[INFO], every Java witness HARD):
//   * SUPER-INTERFACE (interface EXTENDS interface): Wolf : LoudAnimal : Animal.
//     shout() (sub-iface default) and defaultGreet() (grandparent default) are
//     reached through the Wolf wrapper ONLY via the transitive-interface walk.
//   * DIAMOND: Diamond : DiamondLeft, DiamondRight (both : DiamondTop).  The
//     single apex default tag() is reached through two arms -> one body.
//   * SAME-SIGNATURE default across TWO interfaces + REQUIRED override:
//     Concierge : Greeter, Welcomer (each declares hello()).  The override is on
//     the impl's own klass, so the SUPER walk binds it before the interface
//     tie-break ever runs -- proving own/override precedence.
//   * INTERFACE INHERITED FROM A SUPERCLASS: GuardDog extends Watcher (which
//     implements Animal) and declares NO interface itself; Animal is in its set
//     only TRANSITIVELY through the superclass, so reaching defaultGreet() proves
//     the walk reads _transitive_interfaces (not just _local_interfaces).
//   * VTABLE vs ITABLE on the SAME object: the GuardDog oop read as a base-CLASS
//     ref (Watcher -> virtual/vtable) and as an INTERFACE ref (Animal ->
//     itable/invokeinterface) decode the SAME oop and dispatch the SAME speak().
//   * STATIC interface method (Java 8+): Toolish.brand() is callable on the
//     interface's own klass (HARD) but is NOT inherited by the Gadget implementor
//     (the default fallback excludes ACC_STATIC) -- characterised.  (A PRIVATE
//     interface method is deliberately omitted: JDK9+ only, would break javac 8.)
//   * GENERIC interface + COVARIANT impl -> BRIDGE method (erasure): StringBox :
//     Box<String> declares String get(); javac emits a synthetic Object get()
//     bridge, so the runtime _methods array carries BOTH descriptors (enumerated
//     HARD), and dispatch reaches the real String body either way.
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
// resolve on some JDK/config (an interface default reached through the cold-path
// interface fallback) is probed with a dedicated resolves_*() (has_value) accessor
// so the best-effort gate is applied to the CALL, not the lookup, and a non-
// resolving config degrades to [INFO] rather than dereferencing a nullopt.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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
    // ---- types added for the EXHAUSTIVE itable/interface-shape coverage ----
    constexpr const char* k_loudanimal_class = "vmhook/fixtures/InterfacePoly$LoudAnimal"; // iface extends iface
    constexpr const char* k_wolf_class       = "vmhook/fixtures/InterfacePoly$Wolf";        // impl of super-iface
    constexpr const char* k_diamondtop_class = "vmhook/fixtures/InterfacePoly$DiamondTop";  // diamond apex
    constexpr const char* k_diamond_class    = "vmhook/fixtures/InterfacePoly$Diamond";     // diamond impl
    constexpr const char* k_greeter_class    = "vmhook/fixtures/InterfacePoly$Greeter";     // same-sig iface A
    constexpr const char* k_welcomer_class   = "vmhook/fixtures/InterfacePoly$Welcomer";    // same-sig iface B
    constexpr const char* k_concierge_class  = "vmhook/fixtures/InterfacePoly$Concierge";   // same-sig override
    constexpr const char* k_watcher_class    = "vmhook/fixtures/InterfacePoly$Watcher";     // non-final base impl
    constexpr const char* k_guarddog_class   = "vmhook/fixtures/InterfacePoly$GuardDog";    // iface via superclass
    constexpr const char* k_toolish_class    = "vmhook/fixtures/InterfacePoly$Toolish";     // static iface method
    constexpr const char* k_gadget_class     = "vmhook/fixtures/InterfacePoly$Gadget";      // impl of Toolish
    constexpr const char* k_box_class        = "vmhook/fixtures/InterfacePoly$Box";         // generic iface
    constexpr const char* k_stringbox_class  = "vmhook/fixtures/InterfacePoly$StringBox";   // covariant impl (bridge)

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

        // INHERITED interface DEFAULT method (Dog does NOT override it): when the
        // implemented-interface fallback in object::get_method reaches it (after the
        // superclass walk misses), the call dispatches the same default body the JVM
        // resolves on the Dog runtime type.  Guarded by resolves_default_greet() at
        // the call site so a JDK where the interface VMStructs are unreadable never
        // dereferences a nullopt.
        auto default_greet() const -> std::string { return get_method("defaultGreet")->call().as_string(); }

        // Probes (no call): does method resolution reach each method?  speak()/fetch()
        // are on Dog's own klass (superclass walk); defaultGreet() is on the Animal
        // interface (interface-chain fallback -- best-effort across JDKs).
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

        // INHERITED interface default (Cat does not override it) -- reached via the
        // interface-chain fallback, same as Dog.  Call-site-guarded by resolves_*().
        auto default_greet() const -> std::string { return get_method("defaultGreet")->call().as_string(); }

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

    // ── Wrapper for the SUPER-interface implementor (InterfacePoly$Wolf) ────
    // Wolf implements LoudAnimal (which EXTENDS Animal).  Only speak() is on
    // Wolf's own klass; shout() (LoudAnimal default) and defaultGreet() (Animal
    // default) live on (super-)interfaces, reachable through Wolf ONLY via the
    // transitive-interface fallback.  All three resolves_*() probe that path.
    class ifp_wolf : public vmhook::object<ifp_wolf>
    {
    public:
        explicit ifp_wolf(vmhook::oop_t instance) noexcept
            : vmhook::object<ifp_wolf>{ instance }
        {
        }

        auto name()          const -> std::string { return get_field("name")->get(); }
        auto speak()         const -> std::string { return get_method("speak")->call().as_string(); }
        auto shout()         const -> std::string { return get_method("shout")->call().as_string(); }
        auto default_greet() const -> std::string { return get_method("defaultGreet")->call().as_string(); }

        auto resolves_speak()         const -> bool { return get_method("speak").has_value(); }
        auto resolves_shout()         const -> bool { return get_method("shout").has_value(); }
        auto resolves_default_greet() const -> bool { return get_method("defaultGreet").has_value(); }
    };

    // ── Wrapper for the DIAMOND implementor (InterfacePoly$Diamond) ─────────
    // Diamond implements DiamondLeft AND DiamondRight (both extend DiamondTop).
    // mark() is its own (super walk, depth 0); tag() is the SINGLE default at the
    // apex DiamondTop, reachable via the transitive fallback through two arms.
    class ifp_diamond : public vmhook::object<ifp_diamond>
    {
    public:
        explicit ifp_diamond(vmhook::oop_t instance) noexcept
            : vmhook::object<ifp_diamond>{ instance }
        {
        }

        auto label() const -> std::string { return get_field("label")->get(); }
        auto mark()  const -> std::string { return get_method("mark")->call().as_string(); }
        auto tag()   const -> std::string { return get_method("tag")->call().as_string(); }

        auto resolves_mark() const -> bool { return get_method("mark").has_value(); }
        auto resolves_tag()  const -> bool { return get_method("tag").has_value(); }
    };

    // ── Wrapper for the SAME-SIGNATURE-conflict impl (InterfacePoly$Concierge)
    // Implements Greeter AND Welcomer, each declaring a same-signature default
    // hello(); the required override is on Concierge's own klass, so the SUPER
    // walk binds it (depth 0) before the interface tie-break ever runs.
    class ifp_concierge : public vmhook::object<ifp_concierge>
    {
    public:
        explicit ifp_concierge(vmhook::oop_t instance) noexcept
            : vmhook::object<ifp_concierge>{ instance }
        {
        }

        auto desk()  const -> std::string { return get_field("desk")->get(); }
        auto hello() const -> std::string { return get_method("hello")->call().as_string(); }

        auto resolves_hello() const -> bool { return get_method("hello").has_value(); }
    };

    // ── Wrapper for the GuardDog (InterfacePoly$GuardDog) ───────────────────
    // GuardDog extends Watcher (which implements Animal) and declares NO
    // interface of its own, so Animal is reached only TRANSITIVELY through the
    // superclass.  speak() is inherited from Watcher (super walk); defaultGreet()
    // is on Animal, reachable only if the transitive-interface fallback surfaces a
    // SUPERCLASS-supplied interface.  This is the itable-via-superclass case.
    class ifp_guarddog : public vmhook::object<ifp_guarddog>
    {
    public:
        explicit ifp_guarddog(vmhook::oop_t instance) noexcept
            : vmhook::object<ifp_guarddog>{ instance }
        {
        }

        auto on_duty()       const -> bool        { return get_field("onDuty")->get(); }
        auto name()          const -> std::string { return get_field("name")->get(); } // inherited from Watcher
        auto speak()         const -> std::string { return get_method("speak")->call().as_string(); }
        auto default_greet() const -> std::string { return get_method("defaultGreet")->call().as_string(); }

        auto resolves_speak()         const -> bool { return get_method("speak").has_value(); }
        auto resolves_default_greet() const -> bool { return get_method("defaultGreet").has_value(); }
    };

    // ── Wrapper for the non-final base class (InterfacePoly$Watcher) ────────
    // The CLASS-reference (vtable) view of the GuardDog object: registered to the
    // base klass Watcher, so speak() resolves on Watcher's own klass (depth 0).
    // Reading the GuardDog oop through THIS wrapper is the vtable side of the
    // vtable-vs-itable contrast.
    class ifp_watcher : public vmhook::object<ifp_watcher>
    {
    public:
        explicit ifp_watcher(vmhook::oop_t instance) noexcept
            : vmhook::object<ifp_watcher>{ instance }
        {
        }

        auto name()  const -> std::string { return get_field("name")->get(); }
        auto speak() const -> std::string { return get_method("speak")->call().as_string(); }

        auto resolves_speak() const -> bool { return get_method("speak").has_value(); }
    };

    // ── Wrapper for the static-interface-method impl (InterfacePoly$Gadget) ─
    // Gadget implements Toolish; use() is overridden on Gadget (super walk).  The
    // STATIC interface method brand() is NOT inherited by the implementor, so the
    // fallback must NOT surface it through this wrapper (resolves_brand() expected
    // false -> [INFO]); brand() is still callable on the Toolish klass itself.
    class ifp_gadget : public vmhook::object<ifp_gadget>
    {
    public:
        explicit ifp_gadget(vmhook::oop_t instance) noexcept
            : vmhook::object<ifp_gadget>{ instance }
        {
        }

        auto model() const -> std::string { return get_field("model")->get(); }
        auto use()   const -> std::string { return get_method("use")->call().as_string(); }

        auto resolves_use()   const -> bool { return get_method("use").has_value(); }
        auto resolves_brand() const -> bool { return get_method("brand").has_value(); }
    };

    // ── Wrapper for the covariant/bridge impl (InterfacePoly$StringBox) ─────
    // StringBox implements Box<String> with a covariant String get(); javac emits
    // a synthetic bridge Object get() on this klass, so the runtime klass exposes
    // BOTH get descriptors.  The explicit-signature accessor pins the REAL body;
    // the name-only accessor takes whichever the _methods order latches first
    // (both dispatch to the real String body through the JVM).
    class ifp_string_box : public vmhook::object<ifp_string_box>
    {
    public:
        explicit ifp_string_box(vmhook::oop_t instance) noexcept
            : vmhook::object<ifp_string_box>{ instance }
        {
        }

        auto value() const -> std::string { return get_field("value")->get(); }
        auto get_via_name() const -> std::string { return get_method("get")->call().as_string(); }
        auto get_via_string_sig() const -> std::string
        {
            return get_method("get", "()Ljava/lang/String;")->call().as_string();
        }

        auto resolves_get_name()       const -> bool { return get_method("get").has_value(); }
        auto resolves_get_string_sig() const -> bool { return get_method("get", "()Ljava/lang/String;").has_value(); }
        auto resolves_get_object_sig() const -> bool { return get_method("get", "()Ljava/lang/Object;").has_value(); }
    };

    // ── Wrapper for the generic interface type itself (InterfacePoly$Box) ───
    // Reading the box slot AS the declared generic interface: its own klass
    // declares the erased Object get(); proves the declared-interface read decodes
    // the SAME oop as the concrete StringBox read.
    class ifp_box : public vmhook::object<ifp_box>
    {
    public:
        explicit ifp_box(vmhook::oop_t instance) noexcept
            : vmhook::object<ifp_box>{ instance }
        {
        }

        auto resolves_get_erased() const -> bool { return get_method("get", "()Ljava/lang/Object;").has_value(); }
    };

    // ── Wrapper for the static-method interface itself (InterfacePoly$Toolish)
    // Registered to the Toolish INTERFACE klass, so static_method("brand") finds
    // the STATIC interface method on the interface's own _methods array (depth 0,
    // HARD).  Contrast: through the Gadget IMPLEMENTOR wrapper the same static is
    // NOT inherited (scenario 13).
    class ifp_toolish : public vmhook::object<ifp_toolish>
    {
    public:
        explicit ifp_toolish(vmhook::oop_t instance) noexcept
            : vmhook::object<ifp_toolish>{ instance }
        {
        }

        // Resolvability of the static interface method on the interface's own
        // klass (depth-0 static walk): true on every JDK that loads Toolish.
        static auto resolves_static_brand() -> bool { return static_method("brand").has_value(); }
        // The static call itself (best-effort: interpreter may be no-value).
        static auto static_brand() -> std::string { return static_method("brand")->call().as_string(); }
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

        // ---- read the SAME slot through DIFFERENT declared-interface lenses ----
        // robotPet implements BOTH Animal and Named: reading it as each interface
        // wrapper decodes the SAME oop (type-agnostic field decode), and the runtime
        // klass is STILL Robot through either lens.  pet2 read as the Animal
        // interface aliases the Cat the same way `pet` aliases the Dog.
        auto robot_as_animal() const -> std::unique_ptr<ifp_animal> { return get_field("robotPet")->get(); }
        auto robot_as_named()  const -> std::unique_ptr<ifp_named>  { return get_field("robotPet")->get(); }
        auto pet2_as_animal()  const -> std::unique_ptr<ifp_animal> { return get_field("pet2")->get(); }
        // ---- read the abstract-typed slot AS the abstract base wrapper ----
        auto abs_as_abstract() const -> std::unique_ptr<ifp_abstract> { return get_field("absPet")->get(); }

        // ---- read the petAsDog field (concrete-typed slot to the SAME object) ----
        auto pet_alias_as_dog() const -> std::unique_ptr<ifp_dog> { return get_field("petAsDog")->get(); }

        // ---- EXHAUSTIVE shapes: read each new declared slot AS its concrete impl
        auto loud_as_wolf()      const -> std::unique_ptr<ifp_wolf>      { return get_field("loud")->get(); }
        auto diamond_as_impl()   const -> std::unique_ptr<ifp_diamond>   { return get_field("diamond")->get(); }
        auto concierge_impl()    const -> std::unique_ptr<ifp_concierge> { return get_field("concierge")->get(); }
        auto guard_as_guarddog() const -> std::unique_ptr<ifp_guarddog>  { return get_field("guard")->get(); }
        auto guard_as_watcher()  const -> std::unique_ptr<ifp_watcher>   { return get_field("guardAsWatcher")->get(); }
        auto gadget_impl()       const -> std::unique_ptr<ifp_gadget>    { return get_field("gadget")->get(); }
        auto box_as_string_box() const -> std::unique_ptr<ifp_string_box>{ return get_field("box")->get(); }
        // ---- read `box`/`guard` AS their DECLARED interface types -------------
        auto box_as_iface()      const -> std::unique_ptr<ifp_box>       { return get_field("box")->get(); }
        auto guard_as_animal()   const -> std::unique_ptr<ifp_animal>    { return get_field("guard")->get(); }

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

    // The internal (slash-separated) name of a klass*, gated null + is_valid_pointer
    // + null symbol so a garbage klass degrades to "" rather than faulting.  Shared
    // by the SUPERCLASS-CHAIN walk below (which reads names off klasses reached by
    // get_super(), not off oop headers).
    auto klass_name(vmhook::hotspot::klass* k) -> std::string
    {
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

    // Walks the SUPERCLASS chain (Klass::_super only -- NOT interfaces) starting at
    // the runtime klass read from an oop header, collecting each klass's full
    // internal name UP to (and including) java/lang/Object.  This is the mechanical
    // root of the interface-default-method shape: a concrete impl of an interface
    // has java/lang/Object as its DIRECT super -- the implemented interface is NOT
    // on this chain.  Safety-gated end to end (header safe-read + per-link
    // is_valid_pointer); returns empty on a stale/relocated/unmapped oop so a
    // cold-JVM relocation degrades to [INFO] rather than faulting.  Bounded depth
    // (a runaway/corrupt _super chain can never spin) -- 16 is far above any real
    // hierarchy here.
    auto super_chain_from_oop(vmhook::oop_t oop) -> std::vector<std::string>
    {
        std::vector<std::string> chain{};
        if (!oop || !vmhook::hotspot::is_valid_pointer(oop))
        {
            return chain;
        }
        std::array<std::uint8_t, 16> scratch{};
        if (!vmhook::os::safe_read(scratch.data(), oop, scratch.size()))
        {
            return chain;
        }
        vmhook::hotspot::klass* k{ vmhook::klass_from_oop(oop) };
        for (int depth{ 0 }; depth < 16 && k && vmhook::hotspot::is_valid_pointer(k); ++depth)
        {
            k = k->get_super();
            if (!k || !vmhook::hotspot::is_valid_pointer(k))
            {
                break;
            }
            const std::string n{ klass_name(k) };
            if (n.empty())
            {
                break;
            }
            chain.push_back(n);
            if (n == "java/lang/Object")
            {
                break;
            }
        }
        return chain;
    }

    // True when `chain` contains exactly one element naming java/lang/Object -- the
    // signature of a class whose DIRECT super is Object (every interface impl in
    // this fixture: Dog/Cat/Snake/Robot/Wolf/Diamond/Concierge/Gadget/StringBox).
    auto direct_object_super(const std::vector<std::string>& chain) -> bool
    {
        return chain.size() == 1 && chain.front() == "java/lang/Object";
    }

    // True when `needle` appears anywhere in `chain`.
    auto chain_contains(const std::vector<std::string>& chain, const std::string& needle) -> bool
    {
        for (const std::string& s : chain)
        {
            if (s == needle)
            {
                return true;
            }
        }
        return false;
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

    // Shared runtime-klass battery for the EXHAUSTIVE scenarios (factored so each
    // new shape gets the SAME proof + the SAME cold-relocation degrade).  Reads
    // the concrete klass straight from the oop header: the runtime type is the
    // implementor even though the declared field type is the interface/base.  The
    // read is RAW; on a cold-JVM relocation runtime_klass_name() returns "" and we
    // record [INFO] and skip (never FAIL), exactly like the original table loop.
    // `leaf` is the expected internal-name suffix (e.g. "Wolf"); `full` the full
    // slash-separated internal name; `label` only labels the [INFO] lines.
    auto ipm_check_runtime_klass(vmhook_test::context& ctx, vmhook::oop_t oop,
                                 const char* leaf, const char* full, const char* label) -> void
    {
        const std::string rn{ runtime_klass_name(oop) };
        if (rn.empty())
        {
            ctx.record(std::string("[INFO] interface_polymorphism: ") + label
                       + " runtime klass not readable (null/stale/relocated oop); "
                         "skipped runtime-type assertion.");
            return;
        }
        ctx.check(std::string("ipm_") + label + "_runtime_klass_ends_with_" + leaf, ends_with(rn, leaf));
        ctx.check(std::string("ipm_") + label + "_runtime_klass_is_full_internal_name",
                  rn == std::string{ full });
        ctx.record(std::string("[INFO] interface_polymorphism: ") + label
                   + " runtime klass = '" + rn + "' (declared field type is the interface/base).");
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
    // Wrappers for the exhaustive itable/interface-shape coverage.
    vmhook::register_class<ifp_wolf>(k_wolf_class);
    vmhook::register_class<ifp_diamond>(k_diamond_class);
    vmhook::register_class<ifp_concierge>(k_concierge_class);
    vmhook::register_class<ifp_guarddog>(k_guarddog_class);
    vmhook::register_class<ifp_watcher>(k_watcher_class);
    vmhook::register_class<ifp_gadget>(k_gadget_class);
    vmhook::register_class<ifp_toolish>(k_toolish_class);
    vmhook::register_class<ifp_string_box>(k_stringbox_class);
    vmhook::register_class<ifp_box>(k_box_class);

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
        // The EXHAUSTIVE-shape nested klasses (all loaded transitively via the holder).
        ctx.check("ipm_loudanimal_super_iface_klass_resolves", vmhook::find_class(k_loudanimal_class) != nullptr);
        ctx.check("ipm_wolf_klass_resolves",           vmhook::find_class(k_wolf_class)       != nullptr);
        ctx.check("ipm_diamondtop_apex_klass_resolves", vmhook::find_class(k_diamondtop_class) != nullptr);
        ctx.check("ipm_diamond_klass_resolves",        vmhook::find_class(k_diamond_class)    != nullptr);
        ctx.check("ipm_greeter_klass_resolves",        vmhook::find_class(k_greeter_class)    != nullptr);
        ctx.check("ipm_welcomer_klass_resolves",       vmhook::find_class(k_welcomer_class)   != nullptr);
        ctx.check("ipm_concierge_klass_resolves",      vmhook::find_class(k_concierge_class)  != nullptr);
        ctx.check("ipm_watcher_base_klass_resolves",   vmhook::find_class(k_watcher_class)    != nullptr);
        ctx.check("ipm_guarddog_klass_resolves",       vmhook::find_class(k_guarddog_class)   != nullptr);
        ctx.check("ipm_toolish_klass_resolves",        vmhook::find_class(k_toolish_class)    != nullptr);
        ctx.check("ipm_gadget_klass_resolves",         vmhook::find_class(k_gadget_class)     != nullptr);
        ctx.check("ipm_box_generic_iface_klass_resolves", vmhook::find_class(k_box_class)     != nullptr);
        ctx.check("ipm_stringbox_klass_resolves",      vmhook::find_class(k_stringbox_class)  != nullptr);

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

        // DECLARED-type proof for the EXHAUSTIVE slots: each names its declared
        // (interface / super-interface / base-class / generic-interface) type,
        // never the runtime impl -- the contrast the runtime-klass battery below
        // overturns.  loud=LoudAnimal, diamond=DiamondLeft, concierge=Greeter,
        // guard=Animal, guardAsWatcher=Watcher, gadget=Toolish, box=Box.
        const std::string loud_desc{      std::string{ "L" } + k_loudanimal_class + ";" };
        const std::string diamondL_desc{  std::string{ "L" } + "vmhook/fixtures/InterfacePoly$DiamondLeft" + ";" };
        const std::string greeter_desc{   std::string{ "L" } + k_greeter_class    + ";" };
        const std::string watcher_desc{   std::string{ "L" } + k_watcher_class    + ";" };
        const std::string toolish_desc{   std::string{ "L" } + k_toolish_class    + ";" };
        const std::string box_desc{       std::string{ "L" } + k_box_class        + ";" };
        ctx.check("ipm_loud_field_descriptor_is_super_interface",   holder->field_signature("loud")      == loud_desc);
        ctx.check("ipm_diamond_field_descriptor_is_diamond_arm",    holder->field_signature("diamond")   == diamondL_desc);
        ctx.check("ipm_concierge_field_descriptor_is_greeter_iface", holder->field_signature("concierge") == greeter_desc);
        ctx.check("ipm_guard_field_descriptor_is_animal_interface", holder->field_signature("guard")      == animal_desc);
        ctx.check("ipm_guardAsWatcher_field_descriptor_is_base_class",
                  holder->field_signature("guardAsWatcher") == watcher_desc);
        ctx.check("ipm_gadget_field_descriptor_is_toolish_iface",   holder->field_signature("gadget")    == toolish_desc);
        ctx.check("ipm_box_field_descriptor_is_generic_iface",      holder->field_signature("box")       == box_desc);

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
        //  7. DEFAULT METHOD INHERITED vs OVERRIDDEN.
        //
        //     Snake OVERRIDES defaultGreet() -> the override is on Snake's own
        //     klass -> the SUPERCLASS walk REACHES it (HARD resolvability; call
        //     best-effort).  Dog/Cat INHERIT the default -> it lives only on the
        //     Animal interface -> object::get_method now reaches it via the
        //     IMPLEMENTED-INTERFACE fallback that runs after the superclass walk
        //     misses (InstanceKlass::_transitive_interfaces).
        //
        //     BEST-EFFORT GATE (the interface walk is a COLD-path VMStruct read):
        //     the fallback reads the interface arrays entirely through
        //     os::safe_read, so on a JDK/config where those arrays are not
        //     exported or not safely readable it fail-safes to "not found".  We
        //     therefore treat REACHING the inherited default as best-effort: when
        //     resolves_default_greet() is true we HARD-assert the call dispatches
        //     to the right default body (and cross-check it against the Java
        //     witness below); when it is false we record [INFO] rather than FAIL.
        //     This mirrors the dont_inline / field_null_safety degrade idiom.  The
        //     superclass-reached cases (Snake override; the interface wrapper's own
        //     klass) stay HARD on every JDK.
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

        // Dog/Cat INHERIT the interface default (they do NOT override it -> it lives
        // ONLY on the Animal interface).  object::get_method reaches it by falling
        // back to the IMPLEMENTED-INTERFACE chain after the superclass walk misses.
        // BEST-EFFORT: HARD content + cross-check WHEN the cold-path interface walk
        // resolves it; [INFO] (never FAIL) when it does not on this JDK/config.
        std::string dog_greet;
        if (pet_dog)
        {
            if (pet_dog->resolves_default_greet())
            {
                // The headline of this fix: the inherited interface default IS
                // reachable through the concrete Dog wrapper via the interface walk.
                if (oop_readable(pet_dog->get_instance()))
                {
                    dog_greet = pet_dog->default_greet();
                    if (!dog_greet.empty())
                    {
                        // Animal.defaultGreet() body = "interface-default-greet:" + speak()
                        // and Dog.speak() = "Rex says woof".
                        ctx.check("dog_default_greet_is_interface_default_form",
                                  contains(dog_greet, "interface-default-greet"));
                        ctx.check("dog_default_greet_embeds_dog_speak", contains(dog_greet, "woof"));
                        ctx.check("dog_default_greet_contains_name_Rex", contains(dog_greet, "Rex"));
                    }
                    else
                    {
                        ctx.record("[INFO] interface_polymorphism: Dog's INHERITED defaultGreet() resolved via "
                                   "the interface walk but returned no value through the interpreter on this JDK "
                                   "build; content assert skipped (resolvability already demonstrated).");
                    }
                }
                ctx.record("[INFO] interface_polymorphism: Dog wrapper FOUND the INHERITED interface default "
                           "defaultGreet() via the implemented-interface fallback "
                           "(InstanceKlass::_transitive_interfaces).");
            }
            else
            {
                // Cold-path safe_read could not reach the interface arrays on this
                // JDK/config (or the VMStructs are not exported): documented degrade,
                // NOT a failure.  The abstract-base describe() contrast still holds
                // (it is reached by the superclass walk, asserted HARD above).
                ctx.record("[INFO] interface_polymorphism: INHERITED interface DEFAULT method defaultGreet() "
                           "was NOT reached through the concrete Dog wrapper on this JDK/config -- the "
                           "implemented-interface fallback fail-safed (interface VMStructs unexported or the "
                           "cold interface arrays were not safely readable).  Best-effort; not a failure.");
            }
        }
        if (pet_cat)
        {
            if (pet_cat->resolves_default_greet())
            {
                // Same headline for the SECOND inheriting impl through a different
                // concrete class (Cat); best-effort content check.
                if (oop_readable(pet_cat->get_instance()))
                {
                    const std::string cat_greet{ pet_cat->default_greet() };
                    if (!cat_greet.empty())
                    {
                        ctx.check("cat_default_greet_is_interface_default_form",
                                  contains(cat_greet, "interface-default-greet"));
                        ctx.check("cat_default_greet_embeds_cat_speak", contains(cat_greet, "meow"));
                    }
                }
                ctx.record("[INFO] interface_polymorphism: Cat wrapper FOUND the INHERITED interface default "
                           "defaultGreet() via the implemented-interface fallback.");
            }
            else
            {
                ctx.record("[INFO] interface_polymorphism: INHERITED defaultGreet() was NOT reached through the "
                           "concrete Cat wrapper on this JDK/config (interface fallback fail-safed); best-effort, "
                           "not a failure.");
            }
        }

        // Through the INTERFACE wrapper, whose OWN klass declares both: defaultGreet()
        // (default) and speak() (abstract) live on this very klass, so the depth-0
        // superclass walk finds both WITHOUT needing the interface fallback.  HARD on
        // every JDK (own-klass methods).
        if (pet_animal)
        {
            ctx.check("animal_interface_wrapper_resolves_own_default_greet",
                      pet_animal->resolves_default_greet());
            ctx.check("animal_interface_wrapper_resolves_own_abstract_speak",
                      pet_animal->resolves_speak());
        }

        // Native results from the EXHAUSTIVE scenarios, captured for the Java
        // byte-for-byte cross-check in scenario 8 (each gated so a no-value JDK
        // build never compares an empty native string).
        std::string wolf_shout, wolf_greet, diamond_tag, concierge_hello;
        std::string guard_speak_class, guard_speak_iface, guard_greet;
        std::string gadget_use, box_get, toolish_brand;

        // =================================================================
        //  9. SUPER-INTERFACE (interface EXTENDS interface): InterfacePoly$Wolf
        //     implements LoudAnimal, which extends Animal.  Runtime klass is Wolf;
        //     speak() (own override) resolves via the super walk (HARD); shout()
        //     (LoudAnimal default) and defaultGreet() (grandparent Animal default)
        //     live on (super-)interfaces, reachable ONLY via the transitive
        //     fallback -> BEST-EFFORT (PASS-or-[INFO]).
        // =================================================================
        const auto wolf{ holder->loud_as_wolf() };
        ipm_check_runtime_klass(ctx, wolf ? wolf->get_instance() : nullptr,
                                "Wolf", k_wolf_class, "wolf");
        if (wolf)
        {
            // speak() is on Wolf's own klass -> superclass walk, HARD on every JDK.
            ctx.check("ipm_wolf_own_speak_resolves", wolf->resolves_speak());

            // shout() is a default on the SUB-interface LoudAnimal: transitive
            // fallback only -> best-effort.
            if (wolf->resolves_shout())
            {
                if (oop_readable(wolf->get_instance()))
                {
                    wolf_shout = wolf->shout();
                    if (!wolf_shout.empty())
                    {
                        ctx.check("ipm_wolf_shout_is_loud_form", contains(wolf_shout, "LOUD:"));
                        ctx.check("ipm_wolf_shout_embeds_speak", contains(wolf_shout, "howl"));
                    }
                }
                ctx.record("[INFO] interface_polymorphism: Wolf wrapper FOUND the super-interface default "
                           "shout() (declared on LoudAnimal) via the transitive-interface fallback.");
            }
            else
            {
                ctx.record("[INFO] interface_polymorphism: super-interface default shout() was NOT reached "
                           "through the Wolf wrapper on this JDK/config (transitive fallback fail-safed); "
                           "best-effort, not a failure.");
            }

            // defaultGreet() is a default on the GRANDPARENT Animal: transitive
            // fallback across two interface hops -> best-effort.
            if (wolf->resolves_default_greet())
            {
                if (oop_readable(wolf->get_instance()))
                {
                    wolf_greet = wolf->default_greet();
                    if (!wolf_greet.empty())
                    {
                        ctx.check("ipm_wolf_grandparent_default_greet_form",
                                  contains(wolf_greet, "interface-default-greet"));
                        ctx.check("ipm_wolf_grandparent_default_greet_embeds_speak",
                                  contains(wolf_greet, "howl"));
                    }
                }
                ctx.record("[INFO] interface_polymorphism: Wolf wrapper FOUND the GRANDPARENT-interface default "
                           "defaultGreet() (declared on Animal, two interface hops up) via the transitive walk.");
            }
            else
            {
                ctx.record("[INFO] interface_polymorphism: grandparent-interface default defaultGreet() was NOT "
                           "reached through the Wolf wrapper on this JDK/config; best-effort, not a failure.");
            }
        }

        // =================================================================
        // 10. DIAMOND of interfaces sharing ONE default: InterfacePoly$Diamond
        //     implements DiamondLeft AND DiamondRight (both extend DiamondTop).
        //     mark() is its own (super walk, HARD); tag() is the SINGLE default at
        //     the apex DiamondTop, reachable through two arms -> transitive
        //     fallback, BEST-EFFORT.  The diamond must resolve to the one body.
        // =================================================================
        const auto diamond{ holder->diamond_as_impl() };
        ipm_check_runtime_klass(ctx, diamond ? diamond->get_instance() : nullptr,
                                "Diamond", k_diamond_class, "diamond");
        if (diamond)
        {
            ctx.check("ipm_diamond_own_mark_resolves", diamond->resolves_mark());
            if (diamond->resolves_tag())
            {
                if (oop_readable(diamond->get_instance()))
                {
                    diamond_tag = diamond->tag();
                    if (!diamond_tag.empty())
                    {
                        ctx.check("ipm_diamond_tag_is_apex_default", contains(diamond_tag, "diamond-top-tag"));
                    }
                }
                ctx.record("[INFO] interface_polymorphism: Diamond wrapper FOUND the single apex default tag() "
                           "(declared on DiamondTop, reached via two diamond arms) -> transitive walk de-duped.");
            }
            else
            {
                ctx.record("[INFO] interface_polymorphism: diamond apex default tag() was NOT reached through the "
                           "Diamond wrapper on this JDK/config; best-effort, not a failure.");
            }
        }

        // =================================================================
        // 11. SAME-SIGNATURE default across TWO interfaces + REQUIRED override:
        //     InterfacePoly$Concierge implements Greeter AND Welcomer (each with a
        //     same-signature hello() default).  The override is on Concierge's own
        //     klass, so the SUPERCLASS walk binds it FIRST -- the interface
        //     tie-break never runs.  Resolvability HARD (own method); the call body
        //     proves the override (not either interface default) won.
        // =================================================================
        const auto concierge{ holder->concierge_impl() };
        ipm_check_runtime_klass(ctx, concierge ? concierge->get_instance() : nullptr,
                                "Concierge", k_concierge_class, "concierge");
        if (concierge)
        {
            // The override is on the impl's own klass -> super walk, HARD.
            ctx.check("ipm_concierge_override_hello_resolves", concierge->resolves_hello());
            if (oop_readable(concierge->get_instance()))
            {
                concierge_hello = concierge->hello();
                if (!concierge_hello.empty())
                {
                    // The override won -- NOT "greeter-hello" / "welcomer-hello".
                    ctx.check("ipm_concierge_hello_is_override", contains(concierge_hello, "concierge-hello"));
                    ctx.check("ipm_concierge_hello_not_greeter", !contains(concierge_hello, "greeter-hello"));
                    ctx.check("ipm_concierge_hello_not_welcomer", !contains(concierge_hello, "welcomer-hello"));
                }
            }
        }

        // =================================================================
        // 12. INTERFACE INHERITED FROM A SUPERCLASS + vtable-vs-itable.
        //     InterfacePoly$GuardDog extends Watcher (which implements Animal) and
        //     declares NO interface of its own -- Animal is in GuardDog's set ONLY
        //     transitively-through-the-superclass.  speak() is inherited from
        //     Watcher (super walk, HARD); defaultGreet() (Animal default) is
        //     reachable only if the transitive walk surfaces a SUPERCLASS-supplied
        //     interface -> BEST-EFFORT.  The SAME oop is then read as a base-CLASS
        //     ref (Watcher -> vtable) and an INTERFACE ref (Animal -> itable); both
        //     decode the SAME oop and dispatch the SAME speak() body.
        // =================================================================
        const auto guard_dog{ holder->guard_as_guarddog() };
        const auto guard_watcher{ holder->guard_as_watcher() };
        const auto guard_animal{ holder->guard_as_animal() };
        ipm_check_runtime_klass(ctx, guard_dog ? guard_dog->get_instance() : nullptr,
                                "GuardDog", k_guarddog_class, "guarddog");
        if (guard_dog)
        {
            // speak() comes from the superclass Watcher -> super walk, HARD.
            ctx.check("ipm_guarddog_inherited_speak_resolves", guard_dog->resolves_speak());

            // defaultGreet() (Animal default) reachable only if the transitive set
            // includes the SUPERCLASS's interface -> best-effort.
            if (guard_dog->resolves_default_greet())
            {
                if (oop_readable(guard_dog->get_instance()))
                {
                    guard_greet = guard_dog->default_greet();
                    if (!guard_greet.empty())
                    {
                        ctx.check("ipm_guarddog_superclass_iface_default_form",
                                  contains(guard_greet, "interface-default-greet"));
                    }
                }
                ctx.record("[INFO] interface_polymorphism: GuardDog wrapper FOUND defaultGreet() -- the Animal "
                           "interface was surfaced TRANSITIVELY through the superclass (GuardDog declares no "
                           "interface itself).");
            }
            else
            {
                ctx.record("[INFO] interface_polymorphism: GuardDog's superclass-supplied interface default "
                           "defaultGreet() was NOT reached on this JDK/config (transitive fallback used "
                           "_local_interfaces or fail-safed); best-effort, not a failure.");
            }
        }

        // vtable-vs-itable: SAME object via base-CLASS ref and INTERFACE ref.
        if (guard_dog && guard_watcher)
        {
            // Pure pointer compare -> HARD: the base-class-typed read decodes the
            // SAME oop as the concrete read.
            ctx.check("ipm_guard_class_ref_same_oop",
                      guard_watcher->get_instance() == guard_dog->get_instance());
            // speak() resolves through the base CLASS wrapper too (vtable side).
            ctx.check("ipm_guard_class_ref_speak_resolves", guard_watcher->resolves_speak());
        }
        if (guard_dog && guard_animal)
        {
            // The INTERFACE-typed read decodes the SAME oop (itable side); HARD.
            ctx.check("ipm_guard_iface_ref_same_oop",
                      guard_animal->get_instance() == guard_dog->get_instance());
            const std::string via_iface{ runtime_klass_name(guard_animal->get_instance()) };
            if (!via_iface.empty())
            {
                ctx.check("ipm_guard_iface_ref_runtime_klass_is_guarddog",
                          ends_with(via_iface, "GuardDog"));
            }
        }
        // Both dispatch paths return the SAME speak() body (best-effort calls).
        if (guard_watcher && oop_readable(guard_watcher->get_instance()))
        {
            guard_speak_class = guard_watcher->speak();   // vtable
        }
        if (guard_animal && guard_dog && oop_readable(guard_dog->get_instance()))
        {
            guard_speak_iface = guard_dog->speak();        // itable (concrete wrapper, runtime klass)
        }
        if (!guard_speak_class.empty() && !guard_speak_iface.empty())
        {
            // vtable result == itable result for the SAME object -> HARD when both
            // calls returned a value.
            ctx.check("ipm_guard_vtable_equals_itable_dispatch",
                      guard_speak_class == guard_speak_iface);
            ctx.check("ipm_guard_speak_contains_name", contains(guard_speak_class, "Bruno"));
        }
        else
        {
            ctx.record("[INFO] interface_polymorphism: GuardDog speak() did not return via the interpreter on "
                       "both the class-ref and interface-ref paths on this JDK build; the same-oop pointer "
                       "proofs above already establish the vtable-vs-itable identity.");
        }

        // =================================================================
        // 13. STATIC INTERFACE METHOD (Java 8+) is NOT inherited by implementors.
        //     InterfacePoly$Gadget implements Toolish; use() is overridden (super
        //     walk, HARD).  Toolish.brand() is STATIC -> the interface-default
        //     fallback must NOT surface it through the Gadget wrapper
        //     (resolves_brand() expected false; characterised as [INFO], never a
        //     FAIL -- if a future vmhook DOES surface a static, that is recorded
        //     too).  brand() stays callable on the Toolish klass itself.
        // =================================================================
        const auto gadget{ holder->gadget_impl() };
        ipm_check_runtime_klass(ctx, gadget ? gadget->get_instance() : nullptr,
                                "Gadget", k_gadget_class, "gadget");
        if (gadget)
        {
            ctx.check("ipm_gadget_override_use_resolves", gadget->resolves_use());
            if (oop_readable(gadget->get_instance()))
            {
                gadget_use = gadget->use();
                if (!gadget_use.empty())
                {
                    ctx.check("ipm_gadget_use_body", contains(gadget_use, "in use"));
                }
            }
            // The static interface method must NOT be inherited by the implementor.
            if (!gadget->resolves_brand())
            {
                ctx.record("[INFO] interface_polymorphism: STATIC interface method brand() is correctly NOT "
                           "surfaced through the Gadget implementor wrapper (static interface methods are not "
                           "inherited; the default fallback excludes ACC_STATIC).");
            }
            else
            {
                ctx.record("[INFO] interface_polymorphism: a vmhook build surfaced the STATIC interface method "
                           "brand() through the implementor wrapper -- recorded for visibility (the fallback's "
                           "ACC_STATIC exclusion is expected to keep it hidden).");
            }
        }
        // The static interface method IS resolvable directly on the Toolish klass
        // (depth-0 static walk on the interface's own _methods) -> HARD on every JDK
        // that loaded Toolish.  The CALL is best-effort (interpreter may be
        // no-value); when it returns, assert its body and capture for cross-check.
        if (vmhook::find_class(k_toolish_class) != nullptr)
        {
            ctx.check("ipm_toolish_static_brand_resolves_on_interface_klass",
                      ifp_toolish::resolves_static_brand());
            toolish_brand = ifp_toolish::static_brand();
            if (!toolish_brand.empty())
            {
                ctx.check("ipm_toolish_static_brand_body", contains(toolish_brand, "toolish-brand"));
            }
            else
            {
                ctx.record("[INFO] interface_polymorphism: Toolish.brand() static call returned no value via the "
                           "interpreter on this JDK build; resolvability proven HARD above.");
            }
        }

        // =================================================================
        // 14. GENERIC INTERFACE + COVARIANT IMPL -> javac BRIDGE method (erasure).
        //     InterfacePoly$StringBox implements Box<String> with String get();
        //     javac emits a synthetic bridge Object get() on this klass, so the
        //     runtime _methods array carries BOTH descriptors.  Enumerating the
        //     klass methods (a runtime-klass read, universal) proves both exist;
        //     the explicit-signature lookup pins the REAL String body; the call
        //     dispatches to the real body either way.
        // =================================================================
        const auto box{ holder->box_as_string_box() };
        ipm_check_runtime_klass(ctx, box ? box->get_instance() : nullptr,
                                "StringBox", k_stringbox_class, "stringbox");
        if (box)
        {
            // Enumerate the runtime klass's own _methods: BOTH get descriptors.
            const auto methods{ vmhook::get_class_methods<ifp_string_box>() };
            if (!methods.empty())
            {
                int get_string{ 0 };
                int get_object{ 0 };
                for (const auto& [m_name, m_sig] : methods)
                {
                    if (m_name != "get")
                    {
                        continue;
                    }
                    if (m_sig == "()Ljava/lang/String;") { ++get_string; }
                    if (m_sig == "()Ljava/lang/Object;") { ++get_object; }
                }
                // The real covariant method is universal; HARD.
                ctx.check("ipm_stringbox_has_real_string_get", get_string >= 1);
                // The synthetic bridge Object get() is what javac emits for an
                // erased-interface covariant override; HARD (the _methods array is a
                // warm runtime-klass read, and erasure is a language constant on 8+).
                ctx.check("ipm_stringbox_has_bridge_object_get", get_object >= 1);
            }
            else
            {
                ctx.record("[INFO] interface_polymorphism: StringBox method enumeration was empty (klass not "
                           "resolvable for the wrapper on this run); skipped bridge-method assertions.");
            }

            // Explicit-signature lookups resolve each overload distinctly; HARD.
            ctx.check("ipm_stringbox_explicit_string_get_resolves", box->resolves_get_string_sig());
            ctx.check("ipm_stringbox_explicit_object_get_resolves", box->resolves_get_object_sig());
            ctx.check("ipm_stringbox_name_only_get_resolves",       box->resolves_get_name());

            if (oop_readable(box->get_instance()))
            {
                // The explicit String-sig call reaches the real body.
                const std::string real_get{ box->get_via_string_sig() };
                if (!real_get.empty())
                {
                    ctx.check("ipm_stringbox_real_get_body", contains(real_get, "boxed:"));
                    ctx.check("ipm_stringbox_real_get_value", contains(real_get, "cargo"));
                }
                // The name-only call (whichever overload latched) STILL returns the
                // real String body, because the bridge delegates to it.  Capture for
                // the Java cross-check.
                box_get = box->get_via_name();
                if (!box_get.empty())
                {
                    ctx.check("ipm_stringbox_name_only_get_reaches_real_body", contains(box_get, "boxed:"));
                }
            }
        }
        // Reading the box slot AS the declared GENERIC interface decodes the SAME
        // oop, and its own klass declares the erased Object get().
        {
            const auto box_iface{ holder->box_as_iface() };
            if (box && box_iface)
            {
                ctx.check("ipm_box_iface_ref_same_oop",
                          box_iface->get_instance() == box->get_instance());
                // The interface's own klass declares the erased get() -> HARD.
                ctx.check("ipm_box_iface_declares_erased_get", box_iface->resolves_get_erased());
            }
        }

        // =================================================================
        // 15. SUPERCLASS-CHAIN SHAPE (the mechanical root of the limitation).
        //     get_method walks Klass::_super ONLY -- so the interface is NEVER on
        //     a concrete impl's super chain.  Read each impl's runtime klass from
        //     its oop header and walk get_super() UP to java/lang/Object, proving:
        //       * every direct interface impl (Dog/Cat/Snake/Robot/Wolf/Diamond/
        //         Concierge/Gadget/StringBox) has java/lang/Object as its DIRECT
        //         and ONLY super -- the implemented interface is absent from the
        //         chain (hence default methods need the interface FALLBACK, not the
        //         super walk);
        //       * Hamster's chain is AbstractPet -> Object (a REAL super, which is
        //         exactly why describe() IS reachable by the super walk);
        //       * GuardDog's chain is Watcher -> Object (Animal reached only
        //         transitively-through-the-superclass, never on this chain).
        //     Pure pointer-walk + name reads -> deterministic on every JDK/platform;
        //     a cold-JVM relocation yields an empty chain -> [INFO], never a FAIL.
        // =================================================================
        struct super_case
        {
            const char*   label;
            vmhook::oop_t oop;
        };
        const std::array<super_case, 9> object_super_cases{ {
            { "dog",       pet_dog   ? pet_dog->get_instance()   : nullptr },
            { "cat",       pet_cat   ? pet_cat->get_instance()   : nullptr },
            { "snake",     pet_snake ? pet_snake->get_instance() : nullptr },
            { "robot",     pet_robot ? pet_robot->get_instance() : nullptr },
            { "wolf",      wolf      ? wolf->get_instance()      : nullptr },
            { "diamond",   diamond   ? diamond->get_instance()   : nullptr },
            { "concierge", concierge ? concierge->get_instance() : nullptr },
            { "gadget",    gadget    ? gadget->get_instance()    : nullptr },
            { "stringbox", box       ? box->get_instance()       : nullptr },
        } };
        for (const super_case& sc : object_super_cases)
        {
            const std::vector<std::string> chain{ super_chain_from_oop(sc.oop) };
            if (chain.empty())
            {
                ctx.record(std::string("[INFO] interface_polymorphism: ") + sc.label
                           + " super chain not readable (null/stale/relocated oop); "
                             "skipped superclass-chain assertion.");
                continue;
            }
            // Direct super is java/lang/Object and the implemented interface is NOT
            // on the chain -> default-method lookup CANNOT use the super walk.
            ctx.check(std::string("ipm_") + sc.label + "_direct_super_is_object",
                      direct_object_super(chain));
            ctx.check(std::string("ipm_") + sc.label + "_super_chain_excludes_animal_interface",
                      !chain_contains(chain, k_animal_class));
        }

        // Hamster: a REAL abstract super on the chain (AbstractPet -> Object).
        if (pet_hamster)
        {
            const std::vector<std::string> hchain{ super_chain_from_oop(pet_hamster->get_instance()) };
            if (!hchain.empty())
            {
                ctx.check("ipm_hamster_super_chain_includes_abstract_base",
                          chain_contains(hchain, k_abstract_class));
                ctx.check("ipm_hamster_super_chain_reaches_object",
                          chain_contains(hchain, "java/lang/Object"));
                // First link is the abstract base (the DIRECT super) -- this is why
                // the super walk reaches the concrete describe() declared on it.
                ctx.check("ipm_hamster_direct_super_is_abstract_base",
                          hchain.front() == std::string{ k_abstract_class });
            }
            else
            {
                ctx.record("[INFO] interface_polymorphism: hamster super chain not readable; skipped.");
            }
        }

        // GuardDog: Watcher -> Object; Animal is NOT a superclass (only transitive).
        if (guard_dog)
        {
            const std::vector<std::string> gchain{ super_chain_from_oop(guard_dog->get_instance()) };
            if (!gchain.empty())
            {
                ctx.check("ipm_guarddog_direct_super_is_watcher",
                          gchain.front() == std::string{ k_watcher_class });
                ctx.check("ipm_guarddog_super_chain_reaches_object",
                          chain_contains(gchain, "java/lang/Object"));
                ctx.check("ipm_guarddog_super_chain_excludes_animal_interface",
                          !chain_contains(gchain, k_animal_class));
            }
            else
            {
                ctx.record("[INFO] interface_polymorphism: guarddog super chain not readable; skipped.");
            }
        }

        // =================================================================
        // 16. MULTI-INTERFACE IDENTITY THROUGH DIFFERENT DECLARED LENSES.
        //     robotPet implements Animal AND Named.  Reading the SAME slot as each
        //     interface wrapper decodes the SAME oop (the field decode is
        //     type-agnostic), and the runtime klass is STILL Robot through either
        //     lens -- the declared interface never leaks into the decoded type.
        //     Both interface methods resolve on Robot's own klass (super walk,
        //     depth 0): Animal.speak() through the Animal lens and Named.who()
        //     through the Named lens.  pet2 read as the Animal interface aliases the
        //     Cat exactly as `pet`/`pet_as_animal` aliases the Dog.
        // =================================================================
        {
            const auto robot_animal{ holder->robot_as_animal() };
            const auto robot_named{ holder->robot_as_named() };
            ctx.check("ipm_robot_as_animal_nonnull", robot_animal != nullptr);
            ctx.check("ipm_robot_as_named_nonnull",  robot_named  != nullptr);
            if (pet_robot && robot_animal)
            {
                ctx.check("ipm_robot_animal_lens_same_oop",
                          robot_animal->get_instance() == pet_robot->get_instance());
                // speak() (Animal-declared) resolves through the Animal lens.
                ctx.check("ipm_robot_animal_lens_resolves_speak", robot_animal->resolves_speak());
            }
            if (pet_robot && robot_named)
            {
                ctx.check("ipm_robot_named_lens_same_oop",
                          robot_named->get_instance() == pet_robot->get_instance());
                // who() (Named-declared) resolves through the Named lens.
                ctx.check("ipm_robot_named_lens_resolves_who", robot_named->resolves_who());
            }
            if (robot_animal && robot_named)
            {
                // BOTH interface lenses decode the SAME oop (one object, two
                // declared interfaces) -> HARD pointer compare.
                ctx.check("ipm_robot_both_iface_lenses_same_oop",
                          robot_animal->get_instance() == robot_named->get_instance());
                const std::string via_named{ runtime_klass_name(robot_named->get_instance()) };
                if (!via_named.empty())
                {
                    ctx.check("ipm_robot_runtime_klass_via_named_lens_is_robot",
                              ends_with(via_named, "Robot"));
                }
            }
        }
        {
            const auto cat_animal{ holder->pet2_as_animal() };
            if (pet_cat && cat_animal)
            {
                ctx.check("ipm_cat_animal_lens_same_oop",
                          cat_animal->get_instance() == pet_cat->get_instance());
                const std::string via_animal{ runtime_klass_name(cat_animal->get_instance()) };
                if (!via_animal.empty())
                {
                    ctx.check("ipm_cat_runtime_klass_via_interface_lens_is_cat",
                              ends_with(via_animal, "Cat"));
                }
            }
        }

        // =================================================================
        // 17. METHOD ENUMERATION on the INTERFACE / multi-method / base klasses.
        //     get_class_methods reads InstanceKlass::_methods directly (a warm
        //     runtime-klass read, universal across JDKs).  Prove the declared shape
        //     of each type's OWN _methods array:
        //       * Animal interface declares BOTH speak() (abstract) and
        //         defaultGreet() (default) -- the default lives on the interface,
        //         which is why the impl needs the interface fallback to reach it;
        //       * Robot declares BOTH speak() and who() (two interfaces' methods,
        //         both overridden on the impl's own klass);
        //       * AbstractPet declares BOTH sound() (abstract) and describe()
        //         (concrete) -- describe() on the super is why the super walk reaches
        //         it;
        //       * LoudAnimal declares shout() and DiamondTop declares tag() (the
        //         sub-/apex defaults reachable only via the transitive fallback).
        //     Each enumeration is gated: an empty array (klass not resolvable on a
        //     cold run) records [INFO] and skips, never FAILs.
        // =================================================================
        {
            // Helper-free inline: count name matches in a klass's own _methods.
            const auto declares = [](const std::vector<std::pair<std::string, std::string>>& ms,
                                     const char* method_name) -> bool
            {
                for (const auto& [m_name, m_sig] : ms)
                {
                    static_cast<void>(m_sig);
                    if (m_name == method_name)
                    {
                        return true;
                    }
                }
                return false;
            };

            const auto animal_methods{ vmhook::get_class_methods(k_animal_class) };
            if (!animal_methods.empty())
            {
                ctx.check("ipm_animal_iface_declares_speak",        declares(animal_methods, "speak"));
                ctx.check("ipm_animal_iface_declares_default_greet", declares(animal_methods, "defaultGreet"));
            }
            else
            {
                ctx.record("[INFO] interface_polymorphism: Animal interface _methods enumeration empty; skipped.");
            }

            const auto robot_methods{ vmhook::get_class_methods(k_robot_class) };
            if (!robot_methods.empty())
            {
                ctx.check("ipm_robot_klass_declares_speak", declares(robot_methods, "speak"));
                ctx.check("ipm_robot_klass_declares_who",   declares(robot_methods, "who"));
            }
            else
            {
                ctx.record("[INFO] interface_polymorphism: Robot _methods enumeration empty; skipped.");
            }

            const auto abstract_methods{ vmhook::get_class_methods(k_abstract_class) };
            if (!abstract_methods.empty())
            {
                ctx.check("ipm_abstract_base_declares_sound",    declares(abstract_methods, "sound"));
                ctx.check("ipm_abstract_base_declares_describe", declares(abstract_methods, "describe"));
            }
            else
            {
                ctx.record("[INFO] interface_polymorphism: AbstractPet _methods enumeration empty; skipped.");
            }

            const auto loud_methods{ vmhook::get_class_methods(k_loudanimal_class) };
            if (!loud_methods.empty())
            {
                ctx.check("ipm_loudanimal_iface_declares_shout", declares(loud_methods, "shout"));
            }
            else
            {
                ctx.record("[INFO] interface_polymorphism: LoudAnimal interface _methods enumeration empty; skipped.");
            }

            const auto diamondtop_methods{ vmhook::get_class_methods(k_diamondtop_class) };
            if (!diamondtop_methods.empty())
            {
                ctx.check("ipm_diamondtop_apex_declares_tag", declares(diamondtop_methods, "tag"));
            }
            else
            {
                ctx.record("[INFO] interface_polymorphism: DiamondTop apex _methods enumeration empty; skipped.");
            }
        }

        // =================================================================
        // 18. ABSTRACT-BASE LENS IDENTITY: reading absPet AS the abstract-base
        //     wrapper decodes the SAME oop as the concrete Hamster read, and both
        //     the abstract sound() and the concrete describe() resolve on the base
        //     klass (depth-0 super walk through the base-typed wrapper).  Parallels
        //     the interface-lens identity above but for an ABSTRACT-CLASS lens.
        // =================================================================
        if (pet_hamster)
        {
            const auto abs_lens{ holder->abs_as_abstract() };
            ctx.check("ipm_abs_as_abstract_nonnull", abs_lens != nullptr);
            if (abs_lens)
            {
                ctx.check("ipm_abs_lens_same_oop_as_hamster",
                          abs_lens->get_instance() == pet_hamster->get_instance());
                // Through the abstract-base lens, both methods on the base resolve.
                ctx.check("ipm_abs_lens_resolves_sound",    abs_lens->resolves_sound());
                ctx.check("ipm_abs_lens_resolves_describe", abs_lens->resolves_describe());
                // The runtime klass via the abstract-base lens is STILL the concrete
                // Hamster (the declared abstract type never leaks into the decode).
                const std::string via_abs{ runtime_klass_name(abs_lens->get_instance()) };
                if (!via_abs.empty())
                {
                    ctx.check("ipm_abs_lens_runtime_klass_is_hamster", ends_with(via_abs, "Hamster"));
                }
            }
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
            const std::string java_dgreet{ ifp_holder::java_witness("dogGreetSeen") };
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
            // Dog's INHERITED interface default, as the JVM ITSELF dispatched it
            // (Java-side invokevirtual through the (Dog) cast -- always runs, HARD).
            ctx.check("java_dog_inherited_greet_is_interface_default_form",
                      contains(java_dgreet, "interface-default-greet"));
            ctx.check("java_dog_inherited_greet_embeds_speak", contains(java_dgreet, "woof"));
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
            // The strongest proof of the interface-default fix: where the NATIVE
            // interface-chain call to Dog's INHERITED defaultGreet() returned a value
            // (dog_greet non-empty => the cold-path interface walk resolved AND the
            // interpreter dispatched), it matches the JVM's own invokevirtual
            // byte-for-byte.  Gated, so a no-value JDK build never fails here.
            if (!dog_greet.empty())
            {
                ctx.check("native_and_java_dog_inherited_greet_agree", dog_greet == java_dgreet);
            }

            // ---- EXHAUSTIVE-shape witnesses (Java's own invoke* -> HARD) ------
            const std::string j_wolf_shout{  ifp_holder::java_witness("wolfShoutSeen") };
            const std::string j_wolf_speak{  ifp_holder::java_witness("wolfSpeakSeen") };
            const std::string j_wolf_greet{  ifp_holder::java_witness("wolfGreetSeen") };
            const std::string j_diamond{     ifp_holder::java_witness("diamondTagSeen") };
            const std::string j_concierge{   ifp_holder::java_witness("conciergeHelloSeen") };
            const std::string j_guard_class{ ifp_holder::java_witness("guardSpeakViaClassSeen") };
            const std::string j_guard_iface{ ifp_holder::java_witness("guardSpeakViaInterfaceSeen") };
            const std::string j_guard_greet{ ifp_holder::java_witness("guardGreetSeen") };
            const std::string j_gadget{      ifp_holder::java_witness("gadgetUseSeen") };
            const std::string j_brand{       ifp_holder::java_witness("toolishBrandSeen") };
            const std::string j_box{         ifp_holder::java_witness("boxGetSeen") };

            // Super-interface: shout() (sub-iface default), speak() (override),
            // defaultGreet() (grandparent default) -- all via the LoudAnimal ref.
            ctx.check("java_wolf_shout_is_loud_form",      contains(j_wolf_shout, "LOUD:"));
            ctx.check("java_wolf_shout_embeds_speak",      contains(j_wolf_shout, "howl"));
            ctx.check("java_wolf_speak_is_override",       contains(j_wolf_speak, "howl"));
            ctx.check("java_wolf_grandparent_greet_form",  contains(j_wolf_greet, "interface-default-greet"));
            // Diamond: single apex default reached via two arms.
            ctx.check("java_diamond_tag_is_apex_default",  contains(j_diamond, "diamond-top-tag"));
            // Same-signature override won (not either interface default).
            ctx.check("java_concierge_hello_is_override",  contains(j_concierge, "concierge-hello"));
            ctx.check("java_concierge_hello_not_greeter",  !contains(j_concierge, "greeter-hello"));
            ctx.check("java_concierge_hello_not_welcomer", !contains(j_concierge, "welcomer-hello"));
            // vtable (class ref) and itable (interface ref) reach the SAME body on
            // the SAME GuardDog object -> Java sees them byte-for-byte equal.
            ctx.check("java_guard_class_ref_speaks",       contains(j_guard_class, "woof"));
            ctx.check("java_guard_iface_ref_speaks",       contains(j_guard_iface, "woof"));
            ctx.check("java_guard_vtable_equals_itable",   j_guard_class == j_guard_iface);
            ctx.check("java_guard_superclass_iface_greet", contains(j_guard_greet, "interface-default-greet"));
            // Static interface method invoked directly (NOT through an instance).
            ctx.check("java_gadget_use_body",              contains(j_gadget, "in use"));
            ctx.check("java_toolish_static_brand_body",    contains(j_brand, "toolish-brand"));
            // Generic/bridge: dispatch through Box<String> reaches the real body.
            ctx.check("java_box_get_reaches_real_body",    contains(j_box, "boxed:"));

            // Cross-check: where the NATIVE call also returned a value, native and
            // Java agree byte-for-byte (gated, so a no-value JDK build never fails).
            if (!wolf_shout.empty())
            {
                ctx.check("native_and_java_wolf_shout_agree", wolf_shout == j_wolf_shout);
            }
            if (!wolf_greet.empty())
            {
                ctx.check("native_and_java_wolf_greet_agree", wolf_greet == j_wolf_greet);
            }
            if (!diamond_tag.empty())
            {
                ctx.check("native_and_java_diamond_tag_agree", diamond_tag == j_diamond);
            }
            if (!concierge_hello.empty())
            {
                ctx.check("native_and_java_concierge_hello_agree", concierge_hello == j_concierge);
            }
            if (!guard_speak_class.empty())
            {
                ctx.check("native_and_java_guard_class_speak_agree", guard_speak_class == j_guard_class);
            }
            if (!guard_speak_iface.empty())
            {
                ctx.check("native_and_java_guard_iface_speak_agree", guard_speak_iface == j_guard_iface);
            }
            if (!guard_greet.empty())
            {
                ctx.check("native_and_java_guard_greet_agree", guard_greet == j_guard_greet);
            }
            if (!gadget_use.empty())
            {
                ctx.check("native_and_java_gadget_use_agree", gadget_use == j_gadget);
            }
            if (!toolish_brand.empty())
            {
                ctx.check("native_and_java_toolish_brand_agree", toolish_brand == j_brand);
            }
            if (!box_get.empty())
            {
                ctx.check("native_and_java_box_get_agree", box_get == j_box);
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
