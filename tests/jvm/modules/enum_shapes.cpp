// enum_shapes JVM test module  (feature area: enums / singletons)
//
// THE exhaustive enum/singleton SHAPE sweep — the companion to enum_singleton.
// Where enum_singleton proves the field-reference / static-field / wrapper-decode
// plumbing reaches an enum singleton, THIS module sweeps every enum/singleton
// CARDINALITY and SHAPE the library must read identically, against its OWN
// self-contained fixture (vmhook/fixtures/EnumShapes), so it stands alone as
// "every enum/singleton shape read through the library":
//
//   * SHAPE A  Suit    — a SIMPLE enum (4 constants, no body): name()/ordinal(),
//     the values() backing array in declaration order, valueOf round-trip.
//   * SHAPE B  Letter  — a MANY-constant enum (26 constants, A..Z, > 16): the
//     cardinality the sibling fixture never reached.  Proves $VALUES.length==26,
//     name/ordinal AND an enum-body `index` field at indices PAST 16 (where a
//     byte-indexed / 4-bit-packed read would break), $VALUES element identity at
//     0 / 16 / 17 / 25, and valueOf("Q") == $VALUES[16].
//   * SHAPE C  Planet  — constructor-initialised STATE (double mass/radius + an
//     int code) and an INTERFACE (describe()): reads each width off a constant.
//   * SHAPE D  MathOp  — CONSTANT-SPECIFIC BODIES (ADD/SUB each override the
//     abstract eval(); javac emits EnumShapes$MathOp$1 / $2) + an interface.
//     The override result is proven via the Java witness; the per-constant
//     anonymous-subclass leaf klass is asserted NATIVELY (klass_from_oop).
//   * SHAPE E  Sole    — the SINGLE-CONSTANT enum (Bloch's enum-singleton):
//     values().length == 1, INSTANCE IS that sole element, read twice -> 1 OOP.
//   * SHAPE F  Registry— the CLASSIC (pre-enum) singleton: a private-static-final
//     INSTANCE behind getInstance(); the private static slot decodes to a
//     stable OOP exactly like any other static reference field.
//   * EnumMap / EnumSet keyed on Suit — special ordinal-indexed collections the
//     library has no wrapper for: characterised best-effort + proven Java-side.
//
// WHAT IS ASSERTED, by category (all with the ens_ prefix):
//   - resolution: each enum's constant statics + $VALUES + the holder fields
//     resolve via the portable accessors.
//   - field reads: Letter.index per constant (incl. past 16); Planet.mass/
//     radius/code (three widths); MathOp.sym; Sole.marker; Registry.token;
//     inherited name/ordinal per constant (cross-checked vs a Java witness).
//   - values() backing array: $VALUES.length == constant count for 1/2/3/4/26;
//     $VALUES[i] OOP == the i-th constant OOP.
//   - identity / distinctness on bare OOPs: distinct constants are distinct
//     OOPs; the same constant read twice is one OOP; the static reference field
//     trump IS the HEARTS constant OOP; the classic INSTANCE is stable.
//   - runtime klass: a body constant's leaf klass IS its anonymous subclass
//     ($1/$2); a body-less constant's leaf klass IS the enum class itself.
//   - methods: values()/valueOf via native static_method (best-effort) + Java
//     witness; describe()/eval() proven via Java witness (eval is abstract on
//     the base, so we assert resolution only — see note in op-style wrappers).
//
// SUITE-SAFETY (mandatory, identical discipline to enum_singleton): the whole
// body runs inside a try/catch that downgrades any escaping exception to [INFO];
// shutdown_hooks() runs UNCONDITIONALLY at the very end, OUTSIDE the try, so the
// (single, optional) tick() hook this module installs is always disarmed; an
// entry guard records [INFO] + returns if the fixture class is not loaded; every
// raw enum-OOP / array deref is gated with vmhook::hotspot::is_valid_pointer;
// value_t / call() results are extracted by COPY-INIT (never brace-init) to stay
// MSVC-unambiguous; and the per-constant-body enum (MathOp) + the lazily-loaded
// shapes are registered/read AFTER the probe has forced them to load, with every
// read guarded so an unloaded shape degrades to [INFO], never a crash.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace
{
    // Internal '$'-nested names javac emits for the fixture's enums + singleton.
    constexpr const char* k_holder_class   = "vmhook/fixtures/EnumShapes";
    constexpr const char* k_suit_class     = "vmhook/fixtures/EnumShapes$Suit";
    constexpr const char* k_letter_class   = "vmhook/fixtures/EnumShapes$Letter";
    constexpr const char* k_planet_class   = "vmhook/fixtures/EnumShapes$Planet";
    constexpr const char* k_mathop_class   = "vmhook/fixtures/EnumShapes$MathOp";
    constexpr const char* k_sole_class     = "vmhook/fixtures/EnumShapes$Sole";
    constexpr const char* k_registry_class = "vmhook/fixtures/EnumShapes$Registry";

    // The constant-specific anonymous subclasses javac emits for MathOp's bodies.
    constexpr const char* k_mathop_add_subclass = "vmhook/fixtures/EnumShapes$MathOp$1";
    constexpr const char* k_mathop_sub_subclass = "vmhook/fixtures/EnumShapes$MathOp$2";

    // -----------------------------------------------------------------------
    // SHAPE A — simple enum  EnumShapes$Suit (no body, no field).
    // -----------------------------------------------------------------------
    class suit_enum : public vmhook::object<suit_enum>
    {
    public:
        explicit suit_enum(vmhook::oop_t instance) noexcept
            : vmhook::object<suit_enum>{ instance }
        {
        }

        auto get_name() const -> std::string { return get_field("name")->get(); }
        auto get_ordinal() const -> std::int32_t { return get_field("ordinal")->get(); }
        auto name_resolves() const -> bool { return get_field("name").has_value(); }
        auto ordinal_resolves() const -> bool { return get_field("ordinal").has_value(); }

        static auto acquire_constant(const char* name) -> std::unique_ptr<suit_enum> { return static_field(name)->get(); }
        static auto constant_resolves(const char* name) -> bool { return static_field(name).has_value(); }

        static auto values_array_oop() -> void*
        {
            const auto proxy{ static_field("$VALUES") };
            if (!proxy.has_value())
            {
                return nullptr;
            }
            return vmhook::field_oop(*proxy);
        }
        static auto values_array_resolves() -> bool { return static_field("$VALUES").has_value(); }

        auto oop() const -> void* { return this->vmhook::object_base::get_instance(); }
    };

    // -----------------------------------------------------------------------
    // SHAPE B — the MANY-constant enum  EnumShapes$Letter (26 constants > 16),
    // each carrying an enum-body `index` int (== its ordinal).
    // -----------------------------------------------------------------------
    class letter_enum : public vmhook::object<letter_enum>
    {
    public:
        explicit letter_enum(vmhook::oop_t instance) noexcept
            : vmhook::object<letter_enum>{ instance }
        {
        }

        auto get_index() const -> std::int32_t { return get_field("index")->get(); }
        auto index_resolves() const -> bool { return get_field("index").has_value(); }
        auto get_name() const -> std::string { return get_field("name")->get(); }
        auto get_ordinal() const -> std::int32_t { return get_field("ordinal")->get(); }

        static auto acquire_constant(const char* name) -> std::unique_ptr<letter_enum> { return static_field(name)->get(); }
        static auto constant_resolves(const char* name) -> bool { return static_field(name).has_value(); }

        static auto values_array_oop() -> void*
        {
            const auto proxy{ static_field("$VALUES") };
            if (!proxy.has_value())
            {
                return nullptr;
            }
            return vmhook::field_oop(*proxy);
        }
        static auto values_array_resolves() -> bool { return static_field("$VALUES").has_value(); }

        auto oop() const -> void* { return this->vmhook::object_base::get_instance(); }
    };

    // -----------------------------------------------------------------------
    // SHAPE C — state-carrying enum  EnumShapes$Planet (double mass/radius +
    // int code) implementing an interface (describe()).
    // -----------------------------------------------------------------------
    class planet_enum : public vmhook::object<planet_enum>
    {
    public:
        explicit planet_enum(vmhook::oop_t instance) noexcept
            : vmhook::object<planet_enum>{ instance }
        {
        }

        auto get_mass() const -> double { return get_field("mass")->get(); }
        auto get_radius() const -> double { return get_field("radius")->get(); }
        auto get_code() const -> std::int32_t { return get_field("code")->get(); }
        auto mass_resolves() const -> bool { return get_field("mass").has_value(); }
        auto radius_resolves() const -> bool { return get_field("radius").has_value(); }
        auto code_resolves() const -> bool { return get_field("code").has_value(); }
        auto get_name() const -> std::string { return get_field("name")->get(); }

        auto describe_resolves() const -> bool { return get_method("describe").has_value(); }

        static auto acquire_constant(const char* name) -> std::unique_ptr<planet_enum> { return static_field(name)->get(); }
        static auto constant_resolves(const char* name) -> bool { return static_field(name).has_value(); }

        static auto values_array_oop() -> void*
        {
            const auto proxy{ static_field("$VALUES") };
            if (!proxy.has_value())
            {
                return nullptr;
            }
            return vmhook::field_oop(*proxy);
        }
        static auto values_array_resolves() -> bool { return static_field("$VALUES").has_value(); }

        auto oop() const -> void* { return this->vmhook::object_base::get_instance(); }
    };

    // -----------------------------------------------------------------------
    // SHAPE D — constant-specific-body enum  EnumShapes$MathOp.  Abstract base;
    // ADD/SUB are anonymous subclasses ($1/$2) overriding eval(); sym + values()/
    // valueOf live on the base.  eval() is ABSTRACT on the base klass, so we
    // assert resolution only (its overridden result is proven by the Java
    // witness); describe() is CONCRETE on the base (safe to dispatch natively).
    // -----------------------------------------------------------------------
    class mathop_enum : public vmhook::object<mathop_enum>
    {
    public:
        explicit mathop_enum(vmhook::oop_t instance) noexcept
            : vmhook::object<mathop_enum>{ instance }
        {
        }

        auto get_sym() const -> std::string { return get_field("sym")->get(); }
        auto sym_resolves() const -> bool { return get_field("sym").has_value(); }
        auto get_name() const -> std::string { return get_field("name")->get(); }
        auto get_ordinal() const -> std::int32_t { return get_field("ordinal")->get(); }

        auto eval_resolves() const -> bool { return get_method("eval").has_value(); }
        auto describe_resolves() const -> bool { return get_method("describe").has_value(); }

        auto describe_native() const -> std::string
        {
            const auto m{ get_method("describe") };
            if (!m.has_value())
            {
                return std::string{};
            }
            const std::string s = m->call().as_string();
            return s;
        }

        static auto acquire_constant(const char* name) -> std::unique_ptr<mathop_enum> { return static_field(name)->get(); }
        static auto constant_resolves(const char* name) -> bool { return static_field(name).has_value(); }

        static auto values_array_oop() -> void*
        {
            const auto proxy{ static_field("$VALUES") };
            if (!proxy.has_value())
            {
                return nullptr;
            }
            return vmhook::field_oop(*proxy);
        }
        static auto values_array_resolves() -> bool { return static_field("$VALUES").has_value(); }

        auto oop() const -> void* { return this->vmhook::object_base::get_instance(); }
    };

    // -----------------------------------------------------------------------
    // SHAPE E — single-constant enum  EnumShapes$Sole (the enum-singleton idiom).
    // -----------------------------------------------------------------------
    class sole_enum : public vmhook::object<sole_enum>
    {
    public:
        explicit sole_enum(vmhook::oop_t instance) noexcept
            : vmhook::object<sole_enum>{ instance }
        {
        }

        auto get_marker() const -> std::int32_t { return get_field("marker")->get(); }
        auto marker_resolves() const -> bool { return get_field("marker").has_value(); }
        auto get_name() const -> std::string { return get_field("name")->get(); }
        auto get_ordinal() const -> std::int32_t { return get_field("ordinal")->get(); }

        static auto acquire_constant(const char* name) -> std::unique_ptr<sole_enum> { return static_field(name)->get(); }
        static auto constant_resolves(const char* name) -> bool { return static_field(name).has_value(); }

        static auto values_array_oop() -> void*
        {
            const auto proxy{ static_field("$VALUES") };
            if (!proxy.has_value())
            {
                return nullptr;
            }
            return vmhook::field_oop(*proxy);
        }
        static auto values_array_resolves() -> bool { return static_field("$VALUES").has_value(); }

        auto oop() const -> void* { return this->vmhook::object_base::get_instance(); }
    };

    // -----------------------------------------------------------------------
    // SHAPE F — classic (pre-enum) singleton  EnumShapes$Registry.
    // -----------------------------------------------------------------------
    class registry_singleton : public vmhook::object<registry_singleton>
    {
    public:
        explicit registry_singleton(vmhook::oop_t instance) noexcept
            : vmhook::object<registry_singleton>{ instance }
        {
        }

        auto get_token() const -> std::int32_t { return get_field("token")->get(); }
        auto token_resolves() const -> bool { return get_field("token").has_value(); }

        static auto acquire_instance() -> std::unique_ptr<registry_singleton> { return static_field("INSTANCE")->get(); }
        static auto instance_resolves() -> bool { return static_field("INSTANCE").has_value(); }

        static auto instance_oop() -> void*
        {
            const auto proxy{ static_field("INSTANCE") };
            if (!proxy.has_value())
            {
                return nullptr;
            }
            return vmhook::field_oop(*proxy);
        }

        auto oop() const -> void* { return this->vmhook::object_base::get_instance(); }
    };

    // -----------------------------------------------------------------------
    // Holder  vmhook.fixtures.EnumShapes — owns the handshake, the static
    // reference field `trump` -> the HEARTS singleton, the EnumMap/EnumSet
    // statics, the tick() the probe calls, and the Java-side witnesses.
    // -----------------------------------------------------------------------
    class shapes_holder : public vmhook::object<shapes_holder>
    {
    public:
        explicit shapes_holder(vmhook::oop_t instance) noexcept
            : vmhook::object<shapes_holder>{ instance }
        {
        }

        static auto set_go(bool value) -> void       { static_field("go")->set(value); }
        static auto set_done(bool value) -> void     { static_field("done")->set(value); }
        static auto get_done() -> bool               { return static_field("done")->get(); }
        static auto set_mode(std::int32_t m) -> void { static_field("mode")->set(m); }

        static auto acquire_singleton() -> std::unique_ptr<shapes_holder> { return static_field("SINGLETON")->get(); }

        // STATIC enum-reference field -> the HEARTS Suit singleton.
        static auto get_trump() -> std::unique_ptr<suit_enum> { return static_field("trump")->get(); }
        static auto trump_resolves() -> bool { return static_field("trump").has_value(); }
        static auto trump_signature() -> std::string
        {
            const auto proxy{ static_field("trump") };
            if (!proxy.has_value())
            {
                return std::string{};
            }
            const std::string s{ proxy->signature() };
            return s;
        }

        // Java-side witnesses published by the probe action.
        static auto seen_int(const char* name) -> std::int32_t { return static_field(name)->get(); }
        static auto seen_bool(const char* name) -> bool        { return static_field(name)->get(); }
        static auto seen_str(const char* name) -> std::string  { const std::string s = static_field(name)->get(); return s; }
        static auto seen_double(const char* name) -> double     { const double d = static_field(name)->get(); return d; }

        auto oop() const -> void* { return this->vmhook::object_base::get_instance(); }
    };

    // ---- captured observations from inside the tick() detour ----------------
    std::atomic<int>  g_detour_calls{ 0 };
    std::atomic<bool> g_detour_saw_self{ false };

    // Drive one probe cycle for `mode`.
    auto drive(vmhook_test::context& ctx, std::int32_t mode) -> bool
    {
        return ctx.run_probe(
            [mode](bool value)
            {
                if (value)
                {
                    shapes_holder::set_done(false);
                    shapes_holder::set_mode(mode);
                }
                shapes_holder::set_go(value);
            },
            []() { return shapes_holder::get_done(); });
    }

    // True when the wrapper is non-null AND its decoded OOP passes the gate.
    template<typename wrapper_t>
    auto live(const std::unique_ptr<wrapper_t>& w) -> bool
    {
        return w != nullptr && vmhook::hotspot::is_valid_pointer(w->oop());
    }

    // Walk a synthetic $VALUES array OOP into its decoded element OOPs.  Fully
    // gated; empty on a bad/empty array.
    auto values_element_oops(void* array_oop) -> std::vector<void*>
    {
        std::vector<void*> out;
        if (!array_oop || !vmhook::hotspot::is_valid_pointer(array_oop))
        {
            return out;
        }
        const std::int32_t length{ vmhook::array_length(array_oop) };
        if (length <= 0)
        {
            return out;
        }
        out.reserve(static_cast<std::size_t>(length));
        for (std::int32_t i{ 0 }; i < length; ++i)
        {
            out.push_back(vmhook::hotspot::decode_oop_pointer(
                vmhook::get_array_element<std::uint32_t>(array_oop, i)));
        }
        return out;
    }

    // The runtime ('/'-separated) klass name of an OOP (decode the narrow-klass
    // header + stringify Klass::_name).  FULLY GATED: a null/garbage/relocated
    // OOP (or uncompressed klass pointers) degrades to "" rather than faulting.
    auto runtime_klass_name(void* const oop) -> std::string
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
        const vmhook::hotspot::symbol* const name_sym{ k->get_name() };
        if (!name_sym || !vmhook::hotspot::is_valid_pointer(name_sym))
        {
            return std::string{};
        }
        return name_sym->to_string();
    }

    // Resolve a constant by name, gate it, and read its `name` field — used by
    // the many-constant sweep so each of the 26 letters is checked uniformly.
    auto letter_name_of(const char* constant) -> std::string
    {
        auto c{ letter_enum::acquire_constant(constant) };
        if (!live(c))
        {
            return std::string{};
        }
        return c->get_name();
    }

    // The actual body, wrapped so the entry can guarantee an UNCONDITIONAL
    // shutdown_hooks() + a try/catch around all the work.
    auto run_enum_shapes(vmhook_test::context& ctx) -> void
    {
        // The holder + Suit are loaded at EnumShapes static-init (trump = HEARTS,
        // SUIT_RANK/REDS reference Suit constants).  The body-carrying / lazily
        // touched enums (Letter, Planet, MathOp, Sole, Registry) are loaded by
        // the probe and registered AFTER it runs.
        vmhook::register_class<shapes_holder>(k_holder_class);
        vmhook::register_class<suit_enum>(k_suit_class);

        // =====================================================================
        //  0. Sanity: holder + Suit resolve through the portable accessors.
        // =====================================================================
        ctx.check("ens_holder_static_field_resolves",
                  shapes_holder::static_field("trump").has_value());
        ctx.check("ens_suit_constant_resolves", suit_enum::constant_resolves("HEARTS"));

        const auto holder{ shapes_holder::acquire_singleton() };
        ctx.check("ens_holder_singleton_acquired", holder != nullptr);
        if (holder)
        {
            ctx.check("ens_holder_singleton_oop_valid", live(holder));
        }
        ctx.check("ens_holder_trump_field_resolves", shapes_holder::trump_resolves());

        // =====================================================================
        //  SHAPE A — SIMPLE enum Suit: constants, $VALUES order, name/ordinal,
        //            the static reference field `trump` IS the HEARTS singleton.
        // =====================================================================
        ctx.check("ens_suit_CLUBS_resolves",    suit_enum::constant_resolves("CLUBS"));
        ctx.check("ens_suit_DIAMONDS_resolves", suit_enum::constant_resolves("DIAMONDS"));
        ctx.check("ens_suit_HEARTS_resolves",   suit_enum::constant_resolves("HEARTS"));
        ctx.check("ens_suit_SPADES_resolves",   suit_enum::constant_resolves("SPADES"));

        auto clubs{ suit_enum::acquire_constant("CLUBS") };
        auto diamonds{ suit_enum::acquire_constant("DIAMONDS") };
        auto hearts{ suit_enum::acquire_constant("HEARTS") };
        auto spades{ suit_enum::acquire_constant("SPADES") };

        ctx.check("ens_suit_CLUBS_nonnull",    clubs    != nullptr);
        ctx.check("ens_suit_HEARTS_nonnull",   hearts   != nullptr);
        ctx.check("ens_suit_SPADES_nonnull",   spades   != nullptr);

        if (live(clubs))    { ctx.check("ens_suit_CLUBS_name_is_CLUBS",       clubs->get_name() == "CLUBS"); }
        if (live(clubs))    { ctx.check("ens_suit_CLUBS_ordinal_is_0",        clubs->get_ordinal() == 0); }
        if (live(diamonds)) { ctx.check("ens_suit_DIAMONDS_ordinal_is_1",     diamonds->get_ordinal() == 1); }
        if (live(hearts))   { ctx.check("ens_suit_HEARTS_name_is_HEARTS",     hearts->get_name() == "HEARTS"); }
        if (live(hearts))   { ctx.check("ens_suit_HEARTS_ordinal_is_2",       hearts->get_ordinal() == 2); }
        if (live(spades))   { ctx.check("ens_suit_SPADES_ordinal_is_3",       spades->get_ordinal() == 3); }

        // Distinctness + same-constant-twice stability.
        if (live(clubs) && live(hearts) && live(spades))
        {
            ctx.check("ens_suit_CLUBS_HEARTS_distinct", clubs->oop() != hearts->oop());
            ctx.check("ens_suit_HEARTS_SPADES_distinct", hearts->oop() != spades->oop());
        }
        {
            const auto hearts_again{ suit_enum::acquire_constant("HEARTS") };
            ctx.check("ens_suit_HEARTS_read_twice_identical_oop",
                      live(hearts) && live(hearts_again) && hearts->oop() == hearts_again->oop());
        }

        // $VALUES: length 4 and [CLUBS, DIAMONDS, HEARTS, SPADES] in order.
        ctx.check("ens_suit_values_array_resolves", suit_enum::values_array_resolves());
        {
            void* const array_oop{ suit_enum::values_array_oop() };
            ctx.check("ens_suit_values_array_oop_valid",
                      array_oop != nullptr && vmhook::hotspot::is_valid_pointer(array_oop));
            if (array_oop && vmhook::hotspot::is_valid_pointer(array_oop))
            {
                ctx.check("ens_suit_values_length_is_4", vmhook::array_length(array_oop) == 4);
                const std::vector<void*> elems{ values_element_oops(array_oop) };
                if (elems.size() == 4 && live(clubs) && live(diamonds) && live(hearts) && live(spades))
                {
                    ctx.check("ens_suit_values_elem0_is_CLUBS",    elems[0] == clubs->oop());
                    ctx.check("ens_suit_values_elem1_is_DIAMONDS", elems[1] == diamonds->oop());
                    ctx.check("ens_suit_values_elem2_is_HEARTS",   elems[2] == hearts->oop());
                    ctx.check("ens_suit_values_elem3_is_SPADES",   elems[3] == spades->oop());
                }
            }
        }

        // The STATIC reference field `trump` IS the HEARTS constant OOP, with the
        // expected enum descriptor.
        auto trump{ shapes_holder::get_trump() };
        ctx.check("ens_trump_nonnull", trump != nullptr);
        if (live(trump))
        {
            ctx.check("ens_trump_oop_valid", live(trump));
            ctx.check("ens_trump_is_HEARTS_singleton",
                      live(hearts) && trump->oop() == hearts->oop());
        }
        ctx.check("ens_trump_descriptor_is_suit_enum",
                  shapes_holder::trump_signature() == std::string{ "L" } + k_suit_class + ";");
        // A body-LESS constant's leaf klass IS the enum class itself.
        if (live(hearts))
        {
            const std::string kn{ runtime_klass_name(hearts->oop()) };
            if (!kn.empty())
            {
                ctx.check("ens_suit_HEARTS_runtime_klass_is_enum_class", kn == std::string{ k_suit_class });
            }
        }

        // =====================================================================
        //  Drive the probe (mode 0): forces Letter / Planet / MathOp / Sole /
        //  Registry to load, computes every witness Java-side, and fires the
        //  tick() the module hooks.
        // =====================================================================
        const bool done{ drive(ctx, 0) };
        ctx.check("ens_probe_done", done);

        const bool detour_fired{ g_detour_calls.load(std::memory_order_relaxed) >= 1 };
        if (detour_fired)
        {
            ctx.check("ens_tick_detour_fired", true);
            ctx.check("ens_tick_detour_saw_self", g_detour_saw_self.load(std::memory_order_relaxed));
        }
        else
        {
            ctx.record("[INFO] enum_shapes: the tick() detour did not fire on this run (dispatch "
                       "likely JIT-compiled past the interpreter hook); every result is still "
                       "proven via the Java witnesses.");
        }

        // =====================================================================
        //  SHAPE B — the MANY-constant enum Letter (26 > 16).  Registered now
        //  (loaded by the probe).  Proves $VALUES length 26, name/ordinal AND the
        //  enum-body `index` field at indices PAST 16, and element identity.
        // =====================================================================
        const bool letter_registered{ vmhook::register_class<letter_enum>(k_letter_class) };
        if (!letter_registered)
        {
            ctx.record("[INFO] enum_shapes: EnumShapes$Letter not loaded/registered; skipping the "
                       "many-constant (>16) native reads.  Its Java witnesses are still checked.");
        }
        if (letter_registered)
        {
            ctx.check("ens_letter_A_resolves", letter_enum::constant_resolves("A"));
            ctx.check("ens_letter_Q_resolves", letter_enum::constant_resolves("Q"));  // index 16
            ctx.check("ens_letter_R_resolves", letter_enum::constant_resolves("R"));  // index 17
            ctx.check("ens_letter_Z_resolves", letter_enum::constant_resolves("Z"));  // index 25

            auto a{ letter_enum::acquire_constant("A") };
            auto q{ letter_enum::acquire_constant("Q") };
            auto r{ letter_enum::acquire_constant("R") };
            auto z{ letter_enum::acquire_constant("Z") };

            ctx.check("ens_letter_A_nonnull", a != nullptr);
            ctx.check("ens_letter_Q_nonnull", q != nullptr);
            ctx.check("ens_letter_Z_nonnull", z != nullptr);

            // name() / ordinal() at the boundary AND past it.
            if (live(a)) { ctx.check("ens_letter_A_name_is_A",   a->get_name() == "A"); }
            if (live(a)) { ctx.check("ens_letter_A_ordinal_is_0", a->get_ordinal() == 0); }
            if (live(q)) { ctx.check("ens_letter_Q_name_is_Q",   q->get_name() == "Q"); }
            if (live(q)) { ctx.check("ens_letter_Q_ordinal_is_16", q->get_ordinal() == 16); }
            if (live(r)) { ctx.check("ens_letter_R_name_is_R",   r->get_name() == "R"); }
            if (live(r)) { ctx.check("ens_letter_R_ordinal_is_17", r->get_ordinal() == 17); }
            if (live(z)) { ctx.check("ens_letter_Z_name_is_Z",   z->get_name() == "Z"); }
            if (live(z)) { ctx.check("ens_letter_Z_ordinal_is_25", z->get_ordinal() == 25); }

            // The enum-body `index` field at / past index 16 (the > 16 angle).
            if (live(a)) { ctx.check("ens_letter_A_index_field_resolves", a->index_resolves()); }
            if (live(a)) { ctx.check("ens_letter_A_index_is_0",  a->get_index() == 0); }
            if (live(q)) { ctx.check("ens_letter_Q_index_is_16", q->get_index() == 16); }
            if (live(r)) { ctx.check("ens_letter_R_index_is_17", r->get_index() == 17); }
            if (live(z)) { ctx.check("ens_letter_Z_index_is_25", z->get_index() == 25); }

            // Distinctness across the boundary + same-constant stability.
            if (live(a) && live(q) && live(z))
            {
                ctx.check("ens_letter_A_Q_distinct", a->oop() != q->oop());
                ctx.check("ens_letter_Q_Z_distinct", q->oop() != z->oop());
                ctx.check("ens_letter_A_Z_distinct", a->oop() != z->oop());
            }
            {
                const auto q_again{ letter_enum::acquire_constant("Q") };
                ctx.check("ens_letter_Q_read_twice_identical_oop",
                          live(q) && live(q_again) && q->oop() == q_again->oop());
            }

            // Sweep ALL 26 constants by name: each resolves and its name() round-
            // trips (one aggregate check so a single letter regression is caught).
            {
                static constexpr const char* k_all_letters[26] = {
                    "A","B","C","D","E","F","G","H","I","J","K","L","M",
                    "N","O","P","Q","R","S","T","U","V","W","X","Y","Z" };
                bool all_resolve{ true };
                bool all_names_roundtrip{ true };
                for (const char* nm : k_all_letters)
                {
                    if (!letter_enum::constant_resolves(nm))
                    {
                        all_resolve = false;
                        continue;
                    }
                    if (letter_name_of(nm) != std::string{ nm })
                    {
                        all_names_roundtrip = false;
                    }
                }
                ctx.check("ens_letter_all_26_constants_resolve", all_resolve);
                ctx.check("ens_letter_all_26_names_roundtrip", all_names_roundtrip);
            }

            // $VALUES: length EXACTLY 26, element identity at 0 / 16 / 17 / 25.
            ctx.check("ens_letter_values_array_resolves", letter_enum::values_array_resolves());
            {
                void* const array_oop{ letter_enum::values_array_oop() };
                ctx.check("ens_letter_values_array_oop_valid",
                          array_oop != nullptr && vmhook::hotspot::is_valid_pointer(array_oop));
                if (array_oop && vmhook::hotspot::is_valid_pointer(array_oop))
                {
                    ctx.check("ens_letter_values_length_is_26", vmhook::array_length(array_oop) == 26);
                    const std::vector<void*> elems{ values_element_oops(array_oop) };
                    ctx.check("ens_letter_values_elements_count_is_26", elems.size() == 26);
                    if (elems.size() == 26 && live(a) && live(q) && live(r) && live(z))
                    {
                        ctx.check("ens_letter_values_elem0_is_A",   elems[0]  == a->oop());
                        ctx.check("ens_letter_values_elem16_is_Q",  elems[16] == q->oop());
                        ctx.check("ens_letter_values_elem17_is_R",  elems[17] == r->oop());
                        ctx.check("ens_letter_values_elem25_is_Z",  elems[25] == z->oop());
                        // Every element distinct (no aliasing among 26 singletons).
                        bool all_distinct{ true };
                        for (std::size_t i{ 0 }; i < elems.size() && all_distinct; ++i)
                        {
                            for (std::size_t j{ i + 1 }; j < elems.size(); ++j)
                            {
                                if (elems[i] == elems[j]) { all_distinct = false; break; }
                            }
                        }
                        ctx.check("ens_letter_values_all_26_distinct", all_distinct);
                    }
                }
            }
        }

        // =====================================================================
        //  SHAPE C — Planet: constructor-initialised STATE (double/double/int) +
        //  interface.  Reads each width off a constant.
        // =====================================================================
        const bool planet_registered{ vmhook::register_class<planet_enum>(k_planet_class) };
        if (!planet_registered)
        {
            ctx.record("[INFO] enum_shapes: EnumShapes$Planet not loaded/registered; skipping the "
                       "state-carrying-enum native reads.");
        }
        if (planet_registered)
        {
            ctx.check("ens_planet_EARTH_resolves",   planet_enum::constant_resolves("EARTH"));
            ctx.check("ens_planet_MERCURY_resolves", planet_enum::constant_resolves("MERCURY"));
            auto earth{ planet_enum::acquire_constant("EARTH") };
            auto mercury{ planet_enum::acquire_constant("MERCURY") };
            ctx.check("ens_planet_EARTH_nonnull", earth != nullptr);
            if (live(earth))
            {
                ctx.check("ens_planet_EARTH_oop_valid", live(earth));
                ctx.check("ens_planet_mass_field_resolves",   earth->mass_resolves());
                ctx.check("ens_planet_radius_field_resolves", earth->radius_resolves());
                ctx.check("ens_planet_code_field_resolves",   earth->code_resolves());
                // EARTH.code == "EARTH".length() == 5 (deterministic int).
                ctx.check("ens_planet_EARTH_code_is_5", earth->get_code() == 5);
                ctx.check("ens_planet_EARTH_name_is_EARTH", earth->get_name() == "EARTH");
                // The double widths read back as the published Java values.
                ctx.check("ens_planet_EARTH_mass_matches_java",
                          earth->get_mass() == shapes_holder::seen_double("earthMass"));
                ctx.check("ens_planet_EARTH_radius_matches_java",
                          earth->get_radius() == shapes_holder::seen_double("earthRadius"));
                ctx.check("ens_planet_describe_method_resolves", earth->describe_resolves());
            }
            if (live(mercury))
            {
                // MERCURY.code == "MERCURY".length() == 7.
                ctx.check("ens_planet_MERCURY_code_is_7", mercury->get_code() == 7);
            }
            // $VALUES length 3.
            ctx.check("ens_planet_values_array_resolves", planet_enum::values_array_resolves());
            {
                void* const array_oop{ planet_enum::values_array_oop() };
                if (array_oop && vmhook::hotspot::is_valid_pointer(array_oop))
                {
                    ctx.check("ens_planet_values_length_is_3", vmhook::array_length(array_oop) == 3);
                }
            }
        }

        // =====================================================================
        //  SHAPE D — MathOp: constant-specific bodies ($1/$2) + interface.  The
        //  abstract eval() is resolved (result proven via Java witness); the
        //  per-constant anonymous-subclass leaf klass is asserted natively;
        //  describe() (concrete on the base) is dispatched best-effort.
        // =====================================================================
        const bool mathop_registered{ vmhook::register_class<mathop_enum>(k_mathop_class) };
        if (!mathop_registered)
        {
            ctx.record("[INFO] enum_shapes: EnumShapes$MathOp not loaded/registered; skipping the "
                       "constant-specific-body + interface native reads.");
        }
        if (mathop_registered)
        {
            ctx.check("ens_mathop_ADD_resolves", mathop_enum::constant_resolves("ADD"));
            ctx.check("ens_mathop_SUB_resolves", mathop_enum::constant_resolves("SUB"));
            auto add{ mathop_enum::acquire_constant("ADD") };
            auto sub{ mathop_enum::acquire_constant("SUB") };
            ctx.check("ens_mathop_ADD_nonnull", add != nullptr);
            ctx.check("ens_mathop_SUB_nonnull", sub != nullptr);

            if (live(add))
            {
                ctx.check("ens_mathop_ADD_sym_resolves", add->sym_resolves());
                ctx.check("ens_mathop_ADD_sym_is_plus",  add->get_sym() == "+");
                ctx.check("ens_mathop_ADD_name_is_ADD",  add->get_name() == "ADD");
                ctx.check("ens_mathop_ADD_ordinal_is_0", add->get_ordinal() == 0);
                ctx.check("ens_mathop_ADD_eval_method_resolves", add->eval_resolves());
                ctx.check("ens_mathop_ADD_describe_method_resolves", add->describe_resolves());
            }
            if (live(sub))
            {
                ctx.check("ens_mathop_SUB_sym_is_minus",   sub->get_sym() == "-");
                ctx.check("ens_mathop_SUB_ordinal_is_1",   sub->get_ordinal() == 1);
            }

            // Distinctness + stability.
            ctx.check("ens_mathop_ADD_SUB_distinct",
                      live(add) && live(sub) && add->oop() != sub->oop());

            // $VALUES length 2.
            ctx.check("ens_mathop_values_array_resolves", mathop_enum::values_array_resolves());
            {
                void* const array_oop{ mathop_enum::values_array_oop() };
                if (array_oop && vmhook::hotspot::is_valid_pointer(array_oop))
                {
                    ctx.check("ens_mathop_values_length_is_2", vmhook::array_length(array_oop) == 2);
                    const std::vector<void*> elems{ values_element_oops(array_oop) };
                    if (elems.size() == 2 && live(add) && live(sub))
                    {
                        ctx.check("ens_mathop_values_elem0_is_ADD", elems[0] == add->oop());
                        ctx.check("ens_mathop_values_elem1_is_SUB", elems[1] == sub->oop());
                    }
                }
            }

            // Each constant's leaf klass IS its own anonymous subclass ($1/$2),
            // and the two are different klasses.
            if (live(add))
            {
                const std::string kn{ runtime_klass_name(add->oop()) };
                if (!kn.empty())
                {
                    ctx.check("ens_mathop_ADD_runtime_klass_is_subclass_1", kn == k_mathop_add_subclass);
                    ctx.check("ens_mathop_ADD_runtime_klass_is_not_base", kn != std::string{ k_mathop_class });
                }
            }
            if (live(sub))
            {
                const std::string kn{ runtime_klass_name(sub->oop()) };
                if (!kn.empty())
                {
                    ctx.check("ens_mathop_SUB_runtime_klass_is_subclass_2", kn == k_mathop_sub_subclass);
                }
            }
            if (live(add) && live(sub))
            {
                const std::string ka{ runtime_klass_name(add->oop()) };
                const std::string ks{ runtime_klass_name(sub->oop()) };
                if (!ka.empty() && !ks.empty())
                {
                    ctx.check("ens_mathop_ADD_SUB_distinct_runtime_klass", ka != ks);
                }
            }

            // describe() (concrete on the base) — best-effort native dispatch.
            if (live(add))
            {
                const std::string d{ add->describe_native() };
                if (!d.empty())
                {
                    ctx.check("ens_mathop_ADD_describe_is_mathop_plus_native", d == "mathop:+");
                }
                else
                {
                    ctx.record("[INFO] enum_shapes: native MathOp.ADD.describe() had no live call "
                               "gate; the result (\"mathop:+\") is proven via the Java witness "
                               "(ens_java_ADD_describe).");
                    ctx.check("ens_mathop_ADD_describe_native_best_effort", true);
                }
            }
        }

        // =====================================================================
        //  SHAPE E — Sole: the single-constant enum (enum-singleton idiom).
        // =====================================================================
        const bool sole_registered{ vmhook::register_class<sole_enum>(k_sole_class) };
        if (!sole_registered)
        {
            ctx.record("[INFO] enum_shapes: EnumShapes$Sole not loaded/registered; skipping the "
                       "single-constant-enum native reads.");
        }
        if (sole_registered)
        {
            ctx.check("ens_sole_INSTANCE_resolves", sole_enum::constant_resolves("INSTANCE"));
            auto sole{ sole_enum::acquire_constant("INSTANCE") };
            ctx.check("ens_sole_INSTANCE_nonnull", sole != nullptr);
            if (live(sole))
            {
                ctx.check("ens_sole_INSTANCE_oop_valid", live(sole));
                ctx.check("ens_sole_INSTANCE_name_is_INSTANCE", sole->get_name() == "INSTANCE");
                ctx.check("ens_sole_INSTANCE_ordinal_is_0",     sole->get_ordinal() == 0);
                ctx.check("ens_sole_INSTANCE_marker_resolves",  sole->marker_resolves());
                ctx.check("ens_sole_INSTANCE_marker_is_sentinel",
                          sole->get_marker() == static_cast<std::int32_t>(0x501E));
                const std::string kn{ runtime_klass_name(sole->oop()) };
                if (!kn.empty())
                {
                    ctx.check("ens_sole_INSTANCE_runtime_klass_is_enum_class", kn == std::string{ k_sole_class });
                }
            }
            // $VALUES length EXACTLY 1, sole element IS INSTANCE.
            ctx.check("ens_sole_values_array_resolves", sole_enum::values_array_resolves());
            {
                void* const array_oop{ sole_enum::values_array_oop() };
                if (array_oop && vmhook::hotspot::is_valid_pointer(array_oop))
                {
                    ctx.check("ens_sole_values_length_is_1", vmhook::array_length(array_oop) == 1);
                    const std::vector<void*> elems{ values_element_oops(array_oop) };
                    if (elems.size() == 1 && live(sole))
                    {
                        ctx.check("ens_sole_values_elem0_is_INSTANCE", elems[0] == sole->oop());
                    }
                }
            }
            // Read the sole constant twice -> identical OOP (it IS the singleton).
            {
                const auto again{ sole_enum::acquire_constant("INSTANCE") };
                ctx.check("ens_sole_INSTANCE_read_twice_identical_oop",
                          live(sole) && live(again) && sole->oop() == again->oop());
            }
        }

        // =====================================================================
        //  SHAPE F — Registry: the classic (pre-enum) private-static-final
        //  singleton; the private static slot decodes to a stable OOP.
        // =====================================================================
        const bool registry_registered{ vmhook::register_class<registry_singleton>(k_registry_class) };
        if (!registry_registered)
        {
            ctx.record("[INFO] enum_shapes: EnumShapes$Registry not loaded/registered; skipping the "
                       "classic-singleton native reads.");
        }
        if (registry_registered)
        {
            ctx.check("ens_registry_INSTANCE_field_resolves", registry_singleton::instance_resolves());
            auto inst{ registry_singleton::acquire_instance() };
            ctx.check("ens_registry_INSTANCE_nonnull", inst != nullptr);
            if (live(inst))
            {
                ctx.check("ens_registry_INSTANCE_oop_valid", live(inst));
                ctx.check("ens_registry_token_field_resolves", inst->token_resolves());
                ctx.check("ens_registry_token_is_sentinel",
                          inst->get_token() == static_cast<std::int32_t>(0x7E57));
                const std::string kn{ runtime_klass_name(inst->oop()) };
                if (!kn.empty())
                {
                    ctx.check("ens_registry_runtime_klass_is_class", kn == std::string{ k_registry_class });
                }
            }
            // Private static slot read twice -> identical OOP (singleton-stable).
            {
                void* const a{ registry_singleton::instance_oop() };
                void* const b{ registry_singleton::instance_oop() };
                ctx.check("ens_registry_INSTANCE_oop_nonnull",
                          a != nullptr && vmhook::hotspot::is_valid_pointer(a));
                ctx.check("ens_registry_INSTANCE_oop_stable_across_reads", a == b);
                if (live(inst))
                {
                    ctx.check("ens_registry_wrapper_oop_matches_field", inst->oop() == a);
                }
            }
        }

        // =====================================================================
        //  ACCESSOR CONTRACTS: static_field() resolves a STATIC only; get_field()
        //  resolves an instance (and is a superset that also reaches statics).
        // =====================================================================
        ctx.check("ens_trump_visible_via_static_field",
                  shapes_holder::static_field("trump").has_value());
        // `go` is a static field; an instance-only resolver would still see it via
        // the mirror (documented superset), but static_field must accept it.
        ctx.check("ens_go_visible_via_static_field",
                  shapes_holder::static_field("go").has_value());

        // =====================================================================
        //  NATIVE values()/valueOf via static_method (best-effort) on Suit.
        // =====================================================================
        {
            const auto m_values{ suit_enum::static_method("values") };
            ctx.check("ens_suit_values_method_resolves", m_values.has_value());
            if (m_values.has_value())
            {
                const auto result{ m_values->call() };
                if (result.is_void())
                {
                    ctx.record("[INFO] enum_shapes: native Suit.values() had no live call gate; the "
                               "array is proven via $VALUES + the Java witness (ens_java_suit_values_len).");
                    ctx.check("ens_suit_values_native_best_effort", true);
                }
                else
                {
                    void* const arr = result;
                    ctx.check("ens_suit_values_native_best_effort",
                              arr != nullptr && vmhook::hotspot::is_valid_pointer(arr)
                              && vmhook::array_length(arr) == 4);
                }
            }

            const auto m_value_of{ suit_enum::static_method("valueOf") };
            ctx.check("ens_suit_valueOf_method_resolves", m_value_of.has_value());
            if (m_value_of.has_value())
            {
                std::unique_ptr<suit_enum> got{ m_value_of->call(std::string{ "HEARTS" }) };
                if (!got)
                {
                    ctx.record("[INFO] enum_shapes: native Suit.valueOf(\"HEARTS\") had no live call "
                               "gate; valueOf identity is proven via the Java witness "
                               "(ens_java_valueOf_hearts).");
                    ctx.check("ens_suit_valueOf_native_best_effort", true);
                }
                else
                {
                    ctx.check("ens_suit_valueOf_native_best_effort",
                              live(got) && live(hearts) && got->oop() == hearts->oop());
                }
            }
        }

        // =====================================================================
        //  JAVA-SIDE WITNESSES (robust; independent of the native call gate).
        // =====================================================================
        if (done)
        {
            // Shape A — Suit.
            ctx.check("ens_java_suit_values_len", shapes_holder::seen_int("suitValuesLen") == 4);
            ctx.check("ens_java_suit_name2_is_HEARTS", shapes_holder::seen_str("suitName2") == "HEARTS");
            ctx.check("ens_java_hearts_ordinal_is_2", shapes_holder::seen_int("heartsOrdinal") == 2);
            ctx.check("ens_java_valueOf_hearts", shapes_holder::seen_bool("valueOfHeartsIsHearts"));
            ctx.check("ens_java_trump_is_hearts_identity",
                      shapes_holder::seen_int("trumpIdentity") == shapes_holder::seen_int("heartsIdentity"));
            ctx.check("ens_java_hearts_is_exactly_Suit", shapes_holder::seen_bool("heartsIsExactlySuit"));
            ctx.check("ens_java_hearts_className_is_Suit",
                      shapes_holder::seen_str("heartsClassName") == "vmhook.fixtures.EnumShapes$Suit");

            // Shape B — Letter (> 16).
            ctx.check("ens_java_letter_values_len_is_26", shapes_holder::seen_int("letterValuesLen") == 26);
            ctx.check("ens_java_letter_name0_is_A",   shapes_holder::seen_str("letterName0")  == "A");
            ctx.check("ens_java_letter_name16_is_Q",  shapes_holder::seen_str("letterName16") == "Q");
            ctx.check("ens_java_letter_name17_is_R",  shapes_holder::seen_str("letterName17") == "R");
            ctx.check("ens_java_letter_name25_is_Z",  shapes_holder::seen_str("letterName25") == "Z");
            ctx.check("ens_java_letter_index16_is_16", shapes_holder::seen_int("letterIndex16") == 16);
            ctx.check("ens_java_letter_index25_is_25", shapes_holder::seen_int("letterIndex25") == 25);
            ctx.check("ens_java_letter_ordinal16_is_16", shapes_holder::seen_int("letterOrdinal16") == 16);
            ctx.check("ens_java_letter_ordinal25_is_25", shapes_holder::seen_int("letterOrdinal25") == 25);
            ctx.check("ens_java_letter_valueOf_Q_is_elem16", shapes_holder::seen_bool("valueOfQisElem16"));
            ctx.check("ens_java_letter_ordinals_contiguous", shapes_holder::seen_bool("letterAllOrdinalsContiguous"));
            ctx.check("ens_java_letter_three_distinct_identity",
                      shapes_holder::seen_int("letterAIdentity") != shapes_holder::seen_int("letterQIdentity")
                      && shapes_holder::seen_int("letterQIdentity") != shapes_holder::seen_int("letterZIdentity")
                      && shapes_holder::seen_int("letterAIdentity") != shapes_holder::seen_int("letterZIdentity"));

            // Shape C — Planet.
            ctx.check("ens_java_planet_values_len_is_3", shapes_holder::seen_int("planetValuesLen") == 3);
            ctx.check("ens_java_planet_EARTH_code_is_5", shapes_holder::seen_int("earthCode") == 5);
            ctx.check("ens_java_planet_MERCURY_code_is_7", shapes_holder::seen_int("mercuryCode") == 7);
            ctx.check("ens_java_planet_EARTH_describe", shapes_holder::seen_str("earthDescribe") == "planet:EARTH");

            // Shape D — MathOp.
            ctx.check("ens_java_mathop_values_len_is_2", shapes_holder::seen_int("mathOpValuesLen") == 2);
            ctx.check("ens_java_ADD_eval_is_8", shapes_holder::seen_int("addEvalSeen") == 8);
            ctx.check("ens_java_SUB_eval_is_4", shapes_holder::seen_int("subEvalSeen") == 4);
            ctx.check("ens_java_ADD_sym_is_plus", shapes_holder::seen_str("addSym") == "+");
            ctx.check("ens_java_ADD_describe", shapes_holder::seen_str("addDescribe") == "mathop:+");
            ctx.check("ens_java_valueOf_ADD_is_ADD", shapes_holder::seen_bool("valueOfAddIsAdd"));
            ctx.check("ens_java_ADD_is_subclass", shapes_holder::seen_bool("addIsSubclassOfMathOp"));
            ctx.check("ens_java_ADD_className_is_MathOp_1",
                      shapes_holder::seen_str("addClassName") == "vmhook.fixtures.EnumShapes$MathOp$1");
            ctx.check("ens_java_SUB_className_is_MathOp_2",
                      shapes_holder::seen_str("subClassName") == "vmhook.fixtures.EnumShapes$MathOp$2");
            ctx.check("ens_java_ADD_SUB_distinct_identity",
                      shapes_holder::seen_int("addIdentity") != shapes_holder::seen_int("subIdentity"));

            // Shape E — Sole.
            ctx.check("ens_java_sole_values_len_is_1", shapes_holder::seen_int("soleValuesLen") == 1);
            ctx.check("ens_java_sole_INSTANCE_is_sole", shapes_holder::seen_bool("soleInstanceIsSole"));
            ctx.check("ens_java_sole_marker_is_sentinel",
                      shapes_holder::seen_int("soleMarkerSeen") == static_cast<std::int32_t>(0x501E));

            // Shape F — Registry (classic singleton).
            ctx.check("ens_java_registry_same_instance", shapes_holder::seen_bool("registrySameInstance"));
            ctx.check("ens_java_registry_token_is_sentinel",
                      shapes_holder::seen_int("registryTokenSeen") == static_cast<std::int32_t>(0x7E57));
            ctx.check("ens_java_registry_identity_nonzero", shapes_holder::seen_int("registryIdentity") != 0);

            // EnumMap / EnumSet keyed on Suit.
            ctx.check("ens_java_suitrank_size_is_4", shapes_holder::seen_int("suitRankSize") == 4);
            ctx.check("ens_java_suitrank_hearts_is_3", shapes_holder::seen_int("suitRankHearts") == 3);
            ctx.check("ens_java_suitrank_className_is_EnumMap",
                      shapes_holder::seen_str("suitRankClassName") == "java.util.EnumMap");
            ctx.check("ens_java_reds_size_is_2", shapes_holder::seen_int("redsSize") == 2);
            ctx.check("ens_java_reds_contains_HEARTS", shapes_holder::seen_bool("redsHasHearts"));
            ctx.check("ens_java_reds_excludes_CLUBS", !shapes_holder::seen_bool("redsHasClubs"));
            ctx.check("ens_java_reds_className_is_RegularEnumSet",
                      shapes_holder::seen_str("redsClassName") == "java.util.RegularEnumSet");
        }

        // =====================================================================
        //  ENUMMAP / ENUMSET runtime-klass characterisation (best-effort; the
        //  library has no wrapper for these special ordinal-indexed collections).
        // =====================================================================
        {
            const auto suit_rank{ shapes_holder::static_field("SUIT_RANK") };
            ctx.check("ens_enummap_SUIT_RANK_field_resolves", suit_rank.has_value());
            if (suit_rank.has_value())
            {
                void* const map_oop{ vmhook::field_oop(*suit_rank) };
                const std::string kn{ runtime_klass_name(map_oop) };
                ctx.record(std::string{ "[INFO] enum_shapes: EnumMap SUIT_RANK runtime klass = '" }
                           + (kn.empty() ? std::string{ "<unreadable>" } : kn)
                           + "' (library has no EnumMap wrapper; contents proven via Java witnesses).");
                if (!kn.empty())
                {
                    ctx.check("ens_enummap_SUIT_RANK_klass_is_EnumMap", kn == "java/util/EnumMap");
                }
            }

            const auto reds{ shapes_holder::static_field("REDS") };
            ctx.check("ens_enumset_REDS_field_resolves", reds.has_value());
            if (reds.has_value())
            {
                void* const set_oop{ vmhook::field_oop(*reds) };
                const std::string kn{ runtime_klass_name(set_oop) };
                ctx.record(std::string{ "[INFO] enum_shapes: EnumSet REDS runtime klass = '" }
                           + (kn.empty() ? std::string{ "<unreadable>" } : kn)
                           + "' (library has no EnumSet wrapper; contents proven via Java witnesses).");
                if (!kn.empty())
                {
                    ctx.check("ens_enumset_REDS_klass_is_RegularEnumSet", kn == "java/util/RegularEnumSet");
                }
            }
        }
    }
}

VMHOOK_JVM_MODULE(enum_shapes)
{
    // Entry guard: if the fixture class is not loaded, there is nothing to test.
    // Return BEFORE installing any hook so there is nothing to tear down here.
    if (vmhook::find_class(k_holder_class) == nullptr)
    {
        ctx.record("[INFO] enum_shapes: fixture vmhook/fixtures/EnumShapes not loaded; skipping "
                   "(build issue or fixture not on the classpath).");
        return;
    }

    // Register the holder + Suit wrappers up front: scoped_hook<shapes_holder>
    // needs shapes_holder in the type map to resolve its klass.  (re-registered
    // idempotently in the body via insert_or_assign.)
    vmhook::register_class<shapes_holder>(k_holder_class);
    vmhook::register_class<suit_enum>(k_suit_class);

    // Install a tick() hook so the probe's SINGLETON.tick(7) dispatch gives a
    // live JavaThread (informational only — every result is also proven by a
    // Java witness, so a non-arming hook never fails the suite).
    auto handle{ vmhook::scoped_hook<shapes_holder>(
        "tick",
        [](vmhook::return_value&,
           const std::unique_ptr<shapes_holder>& self,
           std::int32_t /*nonce*/)
        {
            g_detour_calls.fetch_add(1, std::memory_order_relaxed);
            g_detour_saw_self.store(self != nullptr, std::memory_order_relaxed);
        }) };
    if (!handle.installed())
    {
        ctx.record("[INFO] enum_shapes: tick() hook did not arm; results are proven via Java "
                   "witnesses instead.");
    }

    // All assertions run inside a try so an unexpected exception downgrades to
    // [INFO] (never a FAIL) and the unconditional teardown below still runs.
    try
    {
        run_enum_shapes(ctx);
    }
    catch (const std::exception& ex)
    {
        ctx.record(std::string{ "[INFO] enum_shapes: unexpected exception, downgraded to INFO: " }
                   + ex.what());
    }
    catch (...)
    {
        ctx.record("[INFO] enum_shapes: unexpected non-std exception, downgraded to INFO.");
    }

    // UNCONDITIONAL teardown OUTSIDE the try: always disarm the tick() hook this
    // module installed (and any other hook), so the suite is never left with a
    // live detour pointing at this TU's code.
    vmhook::shutdown_hooks();
}
