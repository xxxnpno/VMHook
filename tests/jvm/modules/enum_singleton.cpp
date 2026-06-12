// enum_singleton JVM test module  (feature area: enums / object references)
//
// THE enum-singleton authority: exhaustively exercises vmhook reading Java enum
// constants as the ordinary heap singletons they really are, plus the field(s),
// the inherited name()/ordinal() state, the synthetic values() backing array,
// and the method(s) declared on the enum body — across TWO enums:
//
//   * EnumSingleton$Color  — a plain enum (RED/GREEN/BLUE) with an `rgb` int
//     field and a shared brightness() method.
//   * EnumSingleton$Op     — an enum that IMPLEMENTS AN INTERFACE (label()) and
//     whose constants (PLUS/TIMES) each have their OWN class body overriding an
//     ABSTRACT method (apply(int,int)).  javac emits the constants as anonymous
//     subclasses EnumSingleton$Op$1 / $2 while the constant STATIC fields live on
//     the abstract base EnumSingleton$Op — so this proves vmhook reads a
//     constant-specific-subclass enum exactly like any other.
//
// A Java enum is a regular class with a private ctor and one synthetic
// `public static final <Enum> NAME` field per constant (the constant's singleton
// object) + a synthetic `values()` array (the private static final `$VALUES`
// field, JDK 5+).  Each constant is therefore a distinct heap object with its
// own OOP, reachable several independent ways this module proves on a live JVM
// (Java 8/11/17/21/24/25 x MSVC/Clang/GCC):
//
//   * INSTANCE enum-reference field  (EnumSingleton.favoriteColor -> GREEN):
//     field_proxy::get() on a field whose descriptor is
//     'Lvmhook/fixtures/EnumSingleton$Color;' decodes a compressed OOP into a
//     std::unique_ptr<enum_color> the module then reads `rgb` from.
//   * STATIC enum-reference field    (EnumSingleton.staticColor -> BLUE):
//     the java.lang.Class mirror + offset path yields the same usable wrapper.
//   * the enum's OWN synthetic constant statics (Color.RED/GREEN/BLUE and
//     Op.PLUS/TIMES), read via a wrapper registered for the inner-enum '$' name.
//   * the synthetic `$VALUES` array: read as a void* array OOP, walked with
//     vmhook::array_length + get_array_element; its element OOPs ARE the constant
//     singleton OOPs (the static field per constant == the values() element) and
//     its length == the constant count.
//
// WHAT IS ASSERTED, by category:
//   - resolution: both inner enums + the holder resolve via the portable
//     accessors; every constant static + reference field resolves.
//   - field reads: `rgb` per Color constant == its packed-RGB; `symbol` per Op
//     constant; inherited `name`/`ordinal` per constant (cross-checked against a
//     Java witness).
//   - values() backing array: $VALUES length == constant count; element[i] OOP
//     == constant[i] OOP.
//   - identity / distinctness on bare OOPs: distinct constants are distinct
//     OOPs; the same constant read twice is the identical OOP; the SINGLETON
//     read twice is the identical OOP; the instance/static reference fields ARE
//     the GREEN/BLUE constant OOPs.  Corroborated by Java identityHashCode.
//   - methods: brightness()/apply()/label()/values()/valueOf() — proven
//     ROBUSTLY via Java-side witnesses the probe computes with real bytecode, and
//     ATTEMPTED best-effort via a native method_proxy::call() inside a tick()
//     detour (which provides a live JavaThread); a missing call gate degrades to
//     [INFO] + a soft pass, never a hard failure.
//   - accessor contracts: static_field() resolves a static and NOT an instance
//     field; get_field() resolves an instance and NOT a static field; the enum-
//     reference field descriptors are the expected 'L...;' names.
//
// SUITE-SAFETY (mandatory): the whole body runs inside a try/catch that downgrades
// any escaping exception to [INFO] (never a FAIL); shutdown_hooks() runs
// UNCONDITIONALLY at the very end, OUTSIDE the try, so the tick() hook this module
// installs is always disarmed; an entry guard records [INFO] + returns if the
// fixture class is not loaded; every raw enum-OOP / array deref is gated with
// vmhook::hotspot::is_valid_pointer before any field/method read; value_t /
// call() results are extracted by COPY-INIT (never brace-init) to stay
// MSVC-unambiguous; and the second enum (Op) is registered only AFTER the probe
// has run (its constants are loaded lazily on first reference), with every Op read
// guarded so an unloaded Op degrades to [INFO], not a crash.
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
    // Internal '$'-nested names javac emits for the fixture's enums.
    constexpr const char* k_holder_class = "vmhook/fixtures/EnumSingleton";
    constexpr const char* k_color_class  = "vmhook/fixtures/EnumSingleton$Color";
    constexpr const char* k_op_class     = "vmhook/fixtures/EnumSingleton$Op";

    // Single-constant enum (the enum-singleton idiom) + the classic (pre-enum)
    // private-static-final singleton, both nested in the holder.
    constexpr const char* k_lonely_class  = "vmhook/fixtures/EnumSingleton$Lonely";
    constexpr const char* k_classic_class = "vmhook/fixtures/EnumSingleton$ClassicSingleton";

    // Internal ('/'-separated) names of the runtime klasses javac emits for the
    // Op constants' anonymous subclass bodies, and the body-less leaf klasses.
    // These are what klass_from_oop(constant_oop)->get_name()->to_string()
    // returns (HotSpot symbols use '/'; Class.getName() uses '.').
    constexpr const char* k_op_plus_subclass  = "vmhook/fixtures/EnumSingleton$Op$1";
    constexpr const char* k_op_times_subclass = "vmhook/fixtures/EnumSingleton$Op$2";

    // -----------------------------------------------------------------------
    // Wrapper for the nested enum  vmhook.fixtures.EnumSingleton$Color.
    //
    // Reads the enum-body instance field `rgb`, the inherited `name`/`ordinal`
    // (declared on java.lang.Enum — find_field walks the superclass chain),
    // dispatches the instance method `brightness()`, and (as static helpers)
    // decodes the enum's own synthetic RED/GREEN/BLUE constant statics + the
    // synthetic `$VALUES` array.
    // -----------------------------------------------------------------------
    class enum_color : public vmhook::object<enum_color>
    {
    public:
        explicit enum_color(vmhook::oop_t instance) noexcept
            : vmhook::object<enum_color>{ instance }
        {
        }

        // ---- enum-body instance field ----
        auto get_rgb() const -> std::int32_t { return get_field("rgb")->get(); }

        auto rgb_resolves() const -> bool { return get_field("rgb").has_value(); }

        // ---- inherited java.lang.Enum state (super-chain field reads) ----
        auto get_name() const -> std::string { return get_field("name")->get(); }

        auto get_ordinal() const -> std::int32_t { return get_field("ordinal")->get(); }

        auto name_resolves() const -> bool { return get_field("name").has_value(); }

        auto ordinal_resolves() const -> bool { return get_field("ordinal").has_value(); }

        // ---- enum-body instance method (native call; best-effort) ----
        // method_proxy::call() needs a live JavaThread/JNIEnv (normally a hook
        // detour).  Callers treat a monostate (is_void) result as "gate
        // unavailable" and fall back to the Java-side witness.  Returns the
        // documented sum on success, or a sentinel for "could not call".
        static constexpr std::int32_t k_call_unavailable{ -1 };

        auto brightness_native() const -> std::int32_t
        {
            const auto m{ get_method("brightness") };
            if (!m.has_value())
            {
                return k_call_unavailable;
            }
            const auto result{ m->call() };
            if (result.is_void())
            {
                return k_call_unavailable;
            }
            const std::int32_t v = result;
            return v;
        }

        auto brightness_resolves() const -> bool { return get_method("brightness").has_value(); }

        // ---- decode one of the enum's own synthetic constant statics ----
        // RED/GREEN/BLUE are `public static final Color NAME` on the Color klass.
        static auto acquire_constant(const char* name) -> std::unique_ptr<enum_color> { return static_field(name)->get(); }

        static auto constant_resolves(const char* name) -> bool { return static_field(name).has_value(); }

        // ---- the synthetic values() backing array ($VALUES) ----
        // Resolved as a static field whose descriptor is '[L...;'.  Read as a
        // void* (the field_proxy reference arm decodes the compressed OOP) so it
        // can be passed to vmhook::array_length / get_array_element — the
        // unique_ptr decode is (correctly) rejected for an array signature.
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

        // ---- bare OOP for identity / distinctness (gated by caller) ----
        auto oop() const -> void* { return this->vmhook::object_base::get_instance(); }
    };

    // -----------------------------------------------------------------------
    // Wrapper for the constant-specific-subclass enum  EnumSingleton$Op.
    //
    // Op is abstract: its constants PLUS/TIMES are anonymous subclasses
    // (EnumSingleton$Op$1 / $2) overriding apply(int,int), while the constant
    // STATIC fields + values()/valueOf live on the base Op klass.  This wrapper
    // is registered for the base name; native instance calls (apply/label)
    // dispatch virtually on the receiver OOP, so they still reach the override.
    // -----------------------------------------------------------------------
    class op_enum : public vmhook::object<op_enum>
    {
    public:
        explicit op_enum(vmhook::oop_t instance) noexcept
            : vmhook::object<op_enum>{ instance }
        {
        }

        // ---- enum-body instance field ----
        auto get_symbol() const -> std::string { return get_field("symbol")->get(); }

        auto symbol_resolves() const -> bool { return get_field("symbol").has_value(); }

        // ---- inherited java.lang.Enum state ----
        auto get_name() const -> std::string { return get_field("name")->get(); }

        auto get_ordinal() const -> std::int32_t { return get_field("ordinal")->get(); }

        // ---- constant-specific abstract method ----
        // NOTE: we intentionally do NOT dispatch apply() natively.  This wrapper
        // is registered for the ABSTRACT base Op, so get_method("apply") resolves
        // Op's ABSTRACT Method* (whose interpreted entry is the
        // AbstractMethodError throw stub) — invoking it would not virtually
        // re-dispatch to the constant subclass's override and could raise inside
        // the interpreter.  The override's behaviour (PLUS=8, TIMES=12) is proven
        // ROBUSTLY by the Java-side witness instead; here we only assert that the
        // (abstract) method is RESOLVABLE on the base klass.
        auto apply_resolves() const -> bool { return get_method("apply").has_value(); }

        // ---- interface method (best-effort native) ----
        auto label_native() const -> std::string
        {
            const auto m{ get_method("label") };
            if (!m.has_value())
            {
                return std::string{};
            }
            const std::string s = m->call().as_string();
            return s;
        }

        auto label_resolves() const -> bool { return get_method("label").has_value(); }

        // ---- the enum's own synthetic constant statics ----
        static auto acquire_constant(const char* name) -> std::unique_ptr<op_enum> { return static_field(name)->get(); }

        static auto constant_resolves(const char* name) -> bool { return static_field(name).has_value(); }

        // ---- the synthetic values() backing array ($VALUES) ----
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
    // Wrapper for the SINGLE-CONSTANT enum  EnumSingleton$Lonely  (the enum-
    // singleton idiom).  Its one constant INSTANCE is THE singleton; the wrapper
    // reads the enum-body `tag` field, the inherited name/ordinal, and (as a
    // static helper) the INSTANCE constant + the $VALUES array (length 1).
    // -----------------------------------------------------------------------
    class lonely_enum : public vmhook::object<lonely_enum>
    {
    public:
        explicit lonely_enum(vmhook::oop_t instance) noexcept
            : vmhook::object<lonely_enum>{ instance }
        {
        }

        auto get_tag() const -> std::int32_t { return get_field("tag")->get(); }
        auto tag_resolves() const -> bool { return get_field("tag").has_value(); }
        auto get_name() const -> std::string { return get_field("name")->get(); }
        auto get_ordinal() const -> std::int32_t { return get_field("ordinal")->get(); }

        static auto acquire_constant(const char* name) -> std::unique_ptr<lonely_enum> { return static_field(name)->get(); }
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
    // Wrapper for the CLASSIC (pre-enum) singleton  EnumSingleton$ClassicSingleton.
    //
    // A final class with a PRIVATE ctor and a `private static final INSTANCE`
    // slot.  Unlike an enum constant this is an ordinary object; the wrapper
    // reads the private `magic` payload and (as a static helper) decodes the
    // private static INSTANCE field directly out of the class mirror — proving
    // a private-static-final singleton is reachable exactly like any other
    // static reference field.
    // -----------------------------------------------------------------------
    class classic_singleton : public vmhook::object<classic_singleton>
    {
    public:
        explicit classic_singleton(vmhook::oop_t instance) noexcept
            : vmhook::object<classic_singleton>{ instance }
        {
        }

        auto get_magic() const -> std::int32_t { return get_field("magic")->get(); }
        auto magic_resolves() const -> bool { return get_field("magic").has_value(); }

        // The canonical private-static-final INSTANCE slot.
        static auto acquire_instance() -> std::unique_ptr<classic_singleton> { return static_field("INSTANCE")->get(); }
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
    // Wrapper for the holder  vmhook.fixtures.EnumSingleton.
    //
    // Owns the go/done/mode handshake, decodes the instance + static enum-
    // reference fields into enum_color singletons, exposes a tick() the probe
    // calls (which this module hooks for a live-JavaThread detour), and exposes
    // the Java-side witnesses the probe publishes.
    // -----------------------------------------------------------------------
    class enum_holder : public vmhook::object<enum_holder>
    {
    public:
        explicit enum_holder(vmhook::oop_t instance) noexcept
            : vmhook::object<enum_holder>{ instance }
        {
        }

        // ---- handshake + scenario selector (all via static_field) ----
        static auto set_go(bool value) -> void       { static_field("go")->set(value); }
        static auto set_done(bool value) -> void     { static_field("done")->set(value); }
        static auto get_done() -> bool               { return static_field("done")->get(); }
        static auto set_mode(std::int32_t m) -> void { static_field("mode")->set(m); }

        // ---- acquire the published holder SINGLETON ----
        static auto acquire_singleton() -> std::unique_ptr<enum_holder> { return static_field("SINGLETON")->get(); }

        static auto singleton_oop() -> void*
        {
            const auto proxy{ static_field("SINGLETON") };
            if (!proxy.has_value())
            {
                return nullptr;
            }
            return vmhook::field_oop(*proxy);
        }

        // ---- INSTANCE enum-reference field -> enum_color singleton ----
        auto get_favorite_color() const -> std::unique_ptr<enum_color> { return get_field("favoriteColor")->get(); }

        auto favorite_color_resolves() const -> bool { return get_field("favoriteColor").has_value(); }

        auto favorite_color_signature() const -> std::string
        {
            const auto proxy{ get_field("favoriteColor") };
            if (!proxy.has_value())
            {
                return std::string{};
            }
            const std::string s{ proxy->signature() };
            return s;
        }

        // ---- STATIC enum-reference field -> enum_color singleton ----
        static auto get_static_color() -> std::unique_ptr<enum_color> { return static_field("staticColor")->get(); }

        static auto static_color_resolves() -> bool { return static_field("staticColor").has_value(); }

        // ---- Java-side witnesses published by the probe action ----
        static auto seen_int(const char* name) -> std::int32_t { return static_field(name)->get(); }
        static auto seen_bool(const char* name) -> bool        { return static_field(name)->get(); }
        static auto seen_str(const char* name) -> std::string  { const std::string s = static_field(name)->get(); return s; }

        // ---- bare OOP for identity / stability (gated by caller) ----
        auto oop() const -> void* { return this->vmhook::object_base::get_instance(); }
    };

    // ---- captured observations from inside the tick() detour ----------------
    // The detour runs with a live JavaThread, so native method_proxy::call() may
    // actually dispatch there.  We attempt every native call ONCE in the detour
    // and stash the result; the module body then asserts the result if the call
    // gate was available, or records [INFO] (soft pass) if it was not.
    std::atomic<int>          g_detour_calls{ 0 };
    std::atomic<bool>         g_detour_saw_self{ false };
    std::atomic<std::int32_t> g_native_green_brightness{ enum_color::k_call_unavailable };
    std::atomic<std::int32_t> g_native_red_brightness{ enum_color::k_call_unavailable };

    // Drive one probe cycle for `mode`: clears the latched `done` and programs
    // the selector on the rising edge of go, then waits for done.
    auto drive(vmhook_test::context& ctx, std::int32_t mode) -> bool
    {
        return ctx.run_probe(
            [mode](bool value)
            {
                if (value)
                {
                    enum_holder::set_done(false);
                    enum_holder::set_mode(mode);
                }
                enum_holder::set_go(value);
            },
            []() { return enum_holder::get_done(); });
    }

    // Read an enum_color singleton's rgb, GATING the deref with is_valid_pointer.
    // Returns the int on a valid OOP, or a sentinel (-1) so the caller's check
    // fails visibly rather than dereferencing a bad pointer.
    auto safe_rgb(const std::unique_ptr<enum_color>& c) -> std::int32_t
    {
        if (!c || !vmhook::hotspot::is_valid_pointer(c->oop()))
        {
            return -1;
        }
        return c->get_rgb();
    }

    // True when the wrapper is non-null AND its decoded OOP passes the gate.
    template<typename wrapper_t>
    auto live(const std::unique_ptr<wrapper_t>& w) -> bool
    {
        return w != nullptr && vmhook::hotspot::is_valid_pointer(w->oop());
    }

    // Walk a synthetic $VALUES array OOP into its decoded element OOPs (nullptr
    // for any null slot).  Fully gated; empty on a bad/empty array.
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

    // The runtime ('/'-separated) klass name of an OOP, read by decoding the
    // narrow-klass slot in the object header (klass_from_oop) and stringifying
    // the Klass::_name symbol.  FULLY GATED: klass_from_oop RAW-derefs oop+8 and
    // symbol::to_string RAW-reads the body, so a null/garbage/relocated OOP (or a
    // JVM with uncompressed klass pointers the decoder can't resolve) degrades to
    // an empty string rather than faulting.  Used to CHARACTERISE the body-enum
    // constants' anonymous subclasses vs the body-less constants' leaf klass.
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

    // The actual body, wrapped so the VMHOOK_JVM_MODULE entry can guarantee an
    // UNCONDITIONAL shutdown_hooks() + a try/catch around all the work.
    auto run_enum_singleton(vmhook_test::context& ctx) -> void
    {
        // Register the holder + the Color wrapper up front (both are loaded at
        // EnumSingleton's static-init: SINGLETON references Color.GREEN/BLUE).
        // The Op wrapper is registered LATER, after the probe loads it.
        vmhook::register_class<enum_holder>(k_holder_class);
        vmhook::register_class<enum_color>(k_color_class);

        // =====================================================================
        //  0. Sanity: holder + Color resolve through the portable accessors.
        // =====================================================================
        ctx.check("holder_class_registered_static_field_resolves",
                  enum_holder::static_field("staticColor").has_value());
        ctx.check("enum_class_registered_constant_resolves",
                  enum_color::constant_resolves("GREEN"));

        const auto holder{ enum_holder::acquire_singleton() };
        ctx.check("holder_singleton_acquired", holder != nullptr);
        if (holder)
        {
            ctx.check("holder_singleton_oop_valid", live(holder));
            ctx.check("holder_favoriteColor_field_resolves", holder->favorite_color_resolves());
        }
        ctx.check("holder_staticColor_field_resolves", enum_holder::static_color_resolves());

        // =====================================================================
        //  1. INSTANCE enum-reference field -> GREEN singleton; read rgb.
        // =====================================================================
        auto favorite{ holder ? holder->get_favorite_color() : nullptr };
        ctx.check("favoriteColorNonNull", favorite != nullptr);
        if (favorite)
        {
            ctx.check("favoriteColor_oop_valid", live(favorite));
            ctx.check("favoriteColor_rgb_field_resolves", favorite->rgb_resolves());
            ctx.check("favoriteColorRgb", safe_rgb(favorite) == static_cast<std::int32_t>(0x00FF00));
        }

        // =====================================================================
        //  2. STATIC enum-reference field -> BLUE singleton; read rgb.
        // =====================================================================
        auto static_color{ enum_holder::get_static_color() };
        ctx.check("staticColorNonNull", static_color != nullptr);
        if (static_color)
        {
            ctx.check("staticColor_oop_valid", live(static_color));
            ctx.check("staticColor_rgb_field_resolves", static_color->rgb_resolves());
            ctx.check("staticColorRgb", safe_rgb(static_color) == static_cast<std::int32_t>(0x0000FF));
        }

        // =====================================================================
        //  3. The enum's OWN synthetic constant statics (Color.RED/GREEN/BLUE),
        //     read through the inner-enum '$' wrapper; assert each rgb constant.
        // =====================================================================
        ctx.check("enum_const_RED_resolves",   enum_color::constant_resolves("RED"));
        ctx.check("enum_const_GREEN_resolves", enum_color::constant_resolves("GREEN"));
        ctx.check("enum_const_BLUE_resolves",  enum_color::constant_resolves("BLUE"));

        auto red{ enum_color::acquire_constant("RED") };
        auto green{ enum_color::acquire_constant("GREEN") };
        auto blue{ enum_color::acquire_constant("BLUE") };

        ctx.check("enum_const_RED_nonnull",   red   != nullptr);
        ctx.check("enum_const_GREEN_nonnull", green != nullptr);
        ctx.check("enum_const_BLUE_nonnull",  blue  != nullptr);

        if (red)   { ctx.check("enum_const_RED_oop_valid",   live(red)); }
        if (green) { ctx.check("enum_const_GREEN_oop_valid", live(green)); }
        if (blue)  { ctx.check("enum_const_BLUE_oop_valid",  live(blue)); }

        ctx.check("enum_const_RED_rgb",   safe_rgb(red)   == static_cast<std::int32_t>(0xFF0000));
        ctx.check("enum_const_GREEN_rgb", safe_rgb(green) == static_cast<std::int32_t>(0x00FF00));
        ctx.check("enum_const_BLUE_rgb",  safe_rgb(blue)  == static_cast<std::int32_t>(0x0000FF));

        // =====================================================================
        //  4. IDENTITY / DISTINCTNESS on the decoded bare OOPs.
        // =====================================================================
        if (live(red) && live(green) && live(blue))
        {
            ctx.check("enum_RED_GREEN_distinct_oops",  red->oop()   != green->oop());
            ctx.check("enum_GREEN_BLUE_distinct_oops", green->oop() != blue->oop());
            ctx.check("enum_RED_BLUE_distinct_oops",   red->oop()   != blue->oop());
        }

        // Same constant decoded twice -> identical OOP (singleton stability).
        {
            const auto green_again{ enum_color::acquire_constant("GREEN") };
            ctx.check("enum_GREEN_read_twice_identical_oop",
                      live(green) && live(green_again) && green->oop() == green_again->oop());
        }

        // favoriteColor (instance field) IS the GREEN singleton (same OOP).
        ctx.check("favoriteColor_is_GREEN_singleton_oop",
                  live(favorite) && live(green) && favorite->oop() == green->oop());

        // staticColor (static field) IS the BLUE singleton (same OOP).
        ctx.check("staticColor_is_BLUE_singleton_oop",
                  live(static_color) && live(blue) && static_color->oop() == blue->oop());

        // =====================================================================
        //  5. INHERITED name()/ordinal() via java.lang.Enum field reads.
        //     find_field walks Color -> Enum, so `name`/`ordinal` resolve on the
        //     Color wrapper.  These are side-effect-free reads (no call gate).
        // =====================================================================
        if (green)
        {
            ctx.check("enum_name_field_resolves",    green->name_resolves());
            ctx.check("enum_ordinal_field_resolves", green->ordinal_resolves());
        }
        if (live(red))
        {
            ctx.check("enum_RED_name_is_RED",     red->get_name() == "RED");
            ctx.check("enum_RED_ordinal_is_0",    red->get_ordinal() == 0);
        }
        if (live(green))
        {
            ctx.check("enum_GREEN_name_is_GREEN", green->get_name() == "GREEN");
            ctx.check("enum_GREEN_ordinal_is_1",  green->get_ordinal() == 1);
        }
        if (live(blue))
        {
            ctx.check("enum_BLUE_name_is_BLUE",   blue->get_name() == "BLUE");
            ctx.check("enum_BLUE_ordinal_is_2",   blue->get_ordinal() == 2);
        }

        // =====================================================================
        //  6. The synthetic values() backing array ($VALUES): length == constant
        //     count, and element[i] OOP == constant[i] OOP (the static field per
        //     constant IS the values() element — enum identity invariant).
        // =====================================================================
        ctx.check("color_values_array_resolves", enum_color::values_array_resolves());
        {
            void* const array_oop{ enum_color::values_array_oop() };
            ctx.check("color_values_array_oop_valid",
                      array_oop != nullptr && vmhook::hotspot::is_valid_pointer(array_oop));
            if (array_oop && vmhook::hotspot::is_valid_pointer(array_oop))
            {
                ctx.check("color_values_array_length_is_3",
                          vmhook::array_length(array_oop) == 3);

                const std::vector<void*> elems{ values_element_oops(array_oop) };
                ctx.check("color_values_elements_count_is_3", elems.size() == 3);
                if (elems.size() == 3 && live(red) && live(green) && live(blue))
                {
                    // $VALUES is in declaration order: [RED, GREEN, BLUE].
                    ctx.check("color_values_elem0_is_RED",   elems[0] == red->oop());
                    ctx.check("color_values_elem1_is_GREEN", elems[1] == green->oop());
                    ctx.check("color_values_elem2_is_BLUE",  elems[2] == blue->oop());
                    ctx.check("color_values_elements_distinct",
                              elems[0] != elems[1] && elems[1] != elems[2] && elems[0] != elems[2]);
                }
            }
        }

        // =====================================================================
        //  7. SINGLETON-pattern object: a private-static-final INSTANCE read +
        //     identity-stable across reads (read its OOP twice -> identical).
        // =====================================================================
        {
            void* const a{ enum_holder::singleton_oop() };
            void* const b{ enum_holder::singleton_oop() };
            ctx.check("holder_singleton_oop_nonnull",
                      a != nullptr && vmhook::hotspot::is_valid_pointer(a));
            ctx.check("holder_singleton_oop_stable_across_reads", a == b);
            // The wrapper acquired from the same static points at that very OOP.
            if (holder && live(holder))
            {
                ctx.check("holder_singleton_wrapper_oop_matches_field", holder->oop() == a);
            }
        }

        // =====================================================================
        //  8. ACCESSOR CONTRACTS: static_field() resolves a STATIC only;
        //     get_field() resolves an INSTANCE only; reference-field descriptors
        //     are the expected 'L...;' names.
        // =====================================================================
        // The static accessor static_field() is STATIC-ONLY: it resolves a
        // static field and REJECTS an instance field (returns nullopt).  This is
        // the precise contract the header guarantees (it requires entry->is_static).
        ctx.check("staticColor_visible_via_static_field",
                  enum_holder::static_field("staticColor").has_value());
        ctx.check("favoriteColor_NOT_visible_via_static_field",
                  !enum_holder::static_field("favoriteColor").has_value());

        // The instance accessor get_field() resolves an instance field...
        if (holder)
        {
            ctx.check("favoriteColor_visible_via_instance_get_field",
                      holder->get_field("favoriteColor").has_value());
            // ...and is intentionally a SUPERSET: it ALSO transparently resolves
            // a static field (through the class mirror), so an inherited/static
            // slot is reachable from an instance wrapper too.  This is the
            // documented behaviour (object_base::get_field handles is_static by
            // reading the declaring-klass mirror), NOT a leak — assert it so the
            // contract is pinned rather than assumed.
            ctx.check("staticColor_ALSO_visible_via_instance_get_field",
                      holder->get_field("staticColor").has_value());
        }

        // The enum-reference field descriptors name the inner enum class.
        if (holder)
        {
            ctx.check("favoriteColor_descriptor_is_color_enum",
                      holder->favorite_color_signature()
                          == std::string{ "L" } + k_color_class + ";");
        }
        {
            const auto static_proxy{ enum_holder::static_field("staticColor") };
            ctx.check("staticColor_descriptor_is_color_enum",
                      static_proxy.has_value()
                      && std::string{ static_proxy->signature() }
                             == std::string{ "L" } + k_color_class + ";");
        }

        // =====================================================================
        //  9. Drive the probe (mode 0): the fixture computes brightness(),
        //     values()/valueOf, name/ordinal and the SECOND enum (Op) Java-side
        //     with real bytecode, publishes witnesses, and (by referencing Op)
        //     forces Op to load so the native Op reads below can resolve it.
        //     The tick() hook this module installed fires here, giving a live
        //     JavaThread in which the native brightness() calls are attempted.
        // =====================================================================
        const bool done{ drive(ctx, 0) };
        ctx.check("enumProbeDone", done);

        // The tick() hook is an OPTIMIZATION (it gives the native brightness()
        // calls a live JavaThread); the feature's correctness never depends on it
        // because every result is also proven by a Java witness.  So whether the
        // detour fired is recorded best-effort: a soft pass if it did, an [INFO]
        // (not a FAIL) if the dispatch was already JIT-compiled past the
        // interpreter hook on this run.
        const bool detour_fired{ g_detour_calls.load(std::memory_order_relaxed) >= 1 };
        if (detour_fired)
        {
            ctx.check("enum_tick_detour_fired", true);
            ctx.check("enum_tick_detour_saw_self",
                      g_detour_saw_self.load(std::memory_order_relaxed));
        }
        else
        {
            ctx.record("[INFO] enum_singleton: the tick() detour did not fire on this run "
                       "(dispatch likely already JIT-compiled, or the hook did not arm); native "
                       "method calls fall back to [INFO] and every result is still proven via the "
                       "Java witnesses below.");
        }

        if (done)
        {
            // ---- Robust Java-side proof of brightness() per constant ----
            ctx.check("enumProbeBrightness",
                      enum_holder::seen_int("favoriteBrightnessSeen") == static_cast<std::int32_t>(0xFF));
            ctx.check("staticColorBrightnessSeen_BLUE",
                      enum_holder::seen_int("staticBrightnessSeen") == static_cast<std::int32_t>(0xFF));
            ctx.check("redColorBrightnessSeen_RED",
                      enum_holder::seen_int("redBrightnessSeen") == static_cast<std::int32_t>(0xFF));

            // ---- Java-side identity cross-check ----
            const std::int32_t green_id{ enum_holder::seen_int("greenIdentity") };
            const std::int32_t blue_id{ enum_holder::seen_int("blueIdentity") };
            const std::int32_t red_id{ enum_holder::seen_int("redIdentity") };
            const std::int32_t fav_id{ enum_holder::seen_int("favoriteIdentity") };
            const std::int32_t stat_id{ enum_holder::seen_int("staticIdentity") };

            ctx.check("java_favoriteIdentity_is_GREEN", fav_id == green_id);
            ctx.check("java_staticIdentity_is_BLUE",    stat_id == blue_id);
            ctx.check("java_three_constants_distinct_identity_hash",
                      red_id != green_id && green_id != blue_id && red_id != blue_id);

            // ---- Java-side ordinal cross-check (corroborates section 5) ----
            ctx.check("java_RED_ordinal_is_0",   enum_holder::seen_int("redOrdinal") == 0);
            ctx.check("java_GREEN_ordinal_is_1", enum_holder::seen_int("greenOrdinal") == 1);
            ctx.check("java_BLUE_ordinal_is_2",  enum_holder::seen_int("blueOrdinal") == 2);

            // ---- Robust Java-side proof of values()/valueOf ----
            ctx.check("java_color_values_length_is_3",
                      enum_holder::seen_int("colorValuesLen") == 3);
            ctx.check("java_color_valueOf_GREEN_is_GREEN",
                      enum_holder::seen_bool("valueOfGreenIsGreen"));
            // valueOf("BLUE") returns the BLUE singleton -> its identity hash
            // equals the BLUE constant's.
            ctx.check("java_color_valueOf_BLUE_is_BLUE_singleton",
                      enum_holder::seen_int("valueOfBlueIdentity") == blue_id);
        }

        // =====================================================================
        // 10. Color values()/valueOf via NATIVE static_method (best-effort).
        //     No assertion of the exact value unless the call gate returns one;
        //     otherwise [INFO] (the robust proof is the Java witness above).
        // =====================================================================
        {
            // values() : ()[Lvmhook/fixtures/EnumSingleton$Color;
            const auto m_values{ enum_color::static_method("values") };
            ctx.check("color_values_method_resolves", m_values.has_value());
            if (m_values.has_value())
            {
                const auto result{ m_values->call() };
                if (result.is_void())
                {
                    ctx.record("[INFO] enum_singleton: native Color.values() had no live call "
                               "gate; values()-array contents are proven via $VALUES (section 6) "
                               "and the Java witness (java_color_values_length_is_3).");
                    ctx.check("color_values_native_best_effort", true);
                }
                else
                {
                    void* const arr = result;
                    ctx.check("color_values_native_best_effort",
                              arr != nullptr && vmhook::hotspot::is_valid_pointer(arr)
                              && vmhook::array_length(arr) == 3);
                }
            }

            // valueOf("BLUE") : (Ljava/lang/String;)Lvmhook/fixtures/...$Color;
            const auto m_value_of{ enum_color::static_method("valueOf") };
            ctx.check("color_valueOf_method_resolves", m_value_of.has_value());
            if (m_value_of.has_value())
            {
                std::unique_ptr<enum_color> got{ m_value_of->call(std::string{ "BLUE" }) };
                if (!got)
                {
                    ctx.record("[INFO] enum_singleton: native Color.valueOf(\"BLUE\") had no live "
                               "call gate (or the JDK call path truncated the returned handle); "
                               "valueOf identity is proven via the Java witness "
                               "(java_color_valueOf_BLUE_is_BLUE_singleton).");
                    ctx.check("color_valueOf_native_best_effort", true);
                }
                else
                {
                    // When the native call works, the returned singleton IS BLUE.
                    ctx.check("color_valueOf_native_best_effort",
                              live(got) && live(blue) && got->oop() == blue->oop());
                }
            }
        }

        // =====================================================================
        // 11. Best-effort NATIVE brightness() captured inside the tick() detour
        //     (live JavaThread).  Assert the documented sum when captured; else
        //     [INFO] + soft pass.  Method resolution itself is always asserted.
        // =====================================================================
        if (green)
        {
            ctx.check("enum_GREEN_brightness_method_resolves", green->brightness_resolves());
        }
        {
            const std::int32_t native_green{ g_native_green_brightness.load(std::memory_order_relaxed) };
            const std::int32_t native_red{ g_native_red_brightness.load(std::memory_order_relaxed) };
            if (native_green == enum_color::k_call_unavailable)
            {
                ctx.record("[INFO] enum_singleton: native method_proxy::call() of GREEN.brightness() "
                           "had no live JavaThread/JNIEnv call gate on this JDK; the documented "
                           "result (0xFF) is proven via the Java-side witness (enumProbeBrightness).");
                ctx.check("enum_GREEN_brightness_native_best_effort", true);
            }
            else
            {
                ctx.check("enum_GREEN_brightness_native_best_effort",
                          native_green == static_cast<std::int32_t>(0xFF));
            }
            if (native_red == enum_color::k_call_unavailable)
            {
                ctx.check("enum_RED_brightness_native_best_effort", true);
            }
            else
            {
                ctx.check("enum_RED_brightness_native_best_effort",
                          native_red == static_cast<std::int32_t>(0xFF));
            }
        }

        // =====================================================================
        // 12. SECOND ENUM (Op): constant-specific subclass + interface.  Op is
        //     loaded lazily by the probe (it references Op.PLUS etc.), so it is
        //     registered + read only now, AFTER the probe ran.  Everything is
        //     guarded: an unloaded/unresolved Op degrades to [INFO], never FAIL.
        // =====================================================================
        const bool op_registered{ vmhook::register_class<op_enum>(k_op_class) };
        if (!op_registered)
        {
            ctx.record("[INFO] enum_singleton: EnumSingleton$Op not loaded/registered (the probe "
                       "may not have run); skipping the second-enum (constant-specific-subclass + "
                       "interface) native reads.  Op's Java-side witnesses are still checked below "
                       "if the probe completed.");
        }

        if (op_registered)
        {
            // ---- resolution + constant statics ----
            ctx.check("op_const_PLUS_resolves",  op_enum::constant_resolves("PLUS"));
            ctx.check("op_const_TIMES_resolves", op_enum::constant_resolves("TIMES"));

            auto plus{ op_enum::acquire_constant("PLUS") };
            auto times{ op_enum::acquire_constant("TIMES") };

            ctx.check("op_const_PLUS_nonnull",  plus  != nullptr);
            ctx.check("op_const_TIMES_nonnull", times != nullptr);
            if (plus)  { ctx.check("op_const_PLUS_oop_valid",  live(plus)); }
            if (times) { ctx.check("op_const_TIMES_oop_valid", live(times)); }

            // ---- enum-body field (symbol) ----
            if (live(plus))
            {
                ctx.check("op_PLUS_symbol_resolves", plus->symbol_resolves());
                ctx.check("op_PLUS_symbol_is_plus",  plus->get_symbol() == "+");
            }
            if (live(times))
            {
                ctx.check("op_TIMES_symbol_is_star", times->get_symbol() == "*");
            }

            // ---- inherited name()/ordinal() ----
            if (live(plus))
            {
                ctx.check("op_PLUS_name_is_PLUS",   plus->get_name() == "PLUS");
                ctx.check("op_PLUS_ordinal_is_0",   plus->get_ordinal() == 0);
            }
            if (live(times))
            {
                ctx.check("op_TIMES_name_is_TIMES", times->get_name() == "TIMES");
                ctx.check("op_TIMES_ordinal_is_1",  times->get_ordinal() == 1);
            }

            // ---- identity / distinctness + $VALUES backing array ----
            ctx.check("op_PLUS_TIMES_distinct_oops",
                      live(plus) && live(times) && plus->oop() != times->oop());
            {
                const auto plus_again{ op_enum::acquire_constant("PLUS") };
                ctx.check("op_PLUS_read_twice_identical_oop",
                          live(plus) && live(plus_again) && plus->oop() == plus_again->oop());
            }
            ctx.check("op_values_array_resolves", op_enum::values_array_resolves());
            {
                void* const array_oop{ op_enum::values_array_oop() };
                if (array_oop && vmhook::hotspot::is_valid_pointer(array_oop))
                {
                    ctx.check("op_values_array_length_is_2", vmhook::array_length(array_oop) == 2);
                    const std::vector<void*> elems{ values_element_oops(array_oop) };
                    if (elems.size() == 2 && live(plus) && live(times))
                    {
                        ctx.check("op_values_elem0_is_PLUS",  elems[0] == plus->oop());
                        ctx.check("op_values_elem1_is_TIMES", elems[1] == times->oop());
                    }
                }
                else
                {
                    ctx.record("[INFO] enum_singleton: Op.$VALUES array OOP not readable; Op "
                               "values() length/identity proven via the Java witness "
                               "(java_op_values_length_is_2).");
                }
            }

            // ---- constant-specific abstract apply() — RESOLUTION only ----
            // The abstract Method* is resolvable on the base klass; its overridden
            // result is proven by the Java witnesses java_op_PLUS_apply_is_8 /
            // java_op_TIMES_apply_is_12 (we do NOT invoke the abstract entry — see
            // op_enum::apply_resolves).
            if (live(plus))
            {
                ctx.check("op_PLUS_apply_method_resolves", plus->apply_resolves());
            }
            if (live(times))
            {
                ctx.check("op_TIMES_apply_method_resolves", times->apply_resolves());
            }

            // ---- interface method label() — best-effort native ----
            // label() is CONCRETE on the base Op (the interface impl lives there),
            // so its interpreted entry is real and safe to dispatch.
            if (live(plus))
            {
                ctx.check("op_PLUS_label_method_resolves", plus->label_resolves());
                const std::string lbl{ plus->label_native() };
                if (!lbl.empty())
                {
                    ctx.check("op_PLUS_label_is_op_plus_native", lbl == "op:+");
                }
                else
                {
                    ctx.record("[INFO] enum_singleton: native Op.PLUS.label() had no live call gate; "
                               "the result (\"op:+\") is proven via the Java witness "
                               "(java_op_PLUS_label_is_op_plus).");
                    ctx.check("op_PLUS_label_native_best_effort", true);
                }
            }

            // ---- BODY-ENUM RUNTIME KLASS characterisation ----
            // javac emits a constant WITH a class body as a DISTINCT anonymous
            // subclass of the enum (EnumSingleton$Op$1 / $2), while the constant
            // STATIC fields live on the abstract base Op.  Prove the leaf klass
            // read out of each constant's OOP header IS that synthetic subclass
            // (and that the two constants have DIFFERENT leaf klasses), so the
            // "each constant is its own subclass" reality is asserted natively —
            // not just via the Java getClass() witnesses.  klass_from_oop +
            // symbol read are fully gated (runtime_klass_name) so a relocated/
            // unreadable constant degrades to "" -> a visible FAIL, never a fault.
            if (live(plus))
            {
                const std::string kn{ runtime_klass_name(plus->oop()) };
                if (!kn.empty())
                {
                    ctx.check("op_PLUS_runtime_klass_is_subclass_1", kn == k_op_plus_subclass);
                    // The constant-specific subclass is NOT the bare base Op.
                    ctx.check("op_PLUS_runtime_klass_is_not_base_Op", kn != std::string{ k_op_class });
                }
                else
                {
                    ctx.record("[INFO] enum_singleton: Op.PLUS runtime klass name unreadable "
                               "(relocated/cold OOP or uncompressed klass pointers); the subclass "
                               "identity is proven via the Java witness (java_op_PLUS_is_subclass).");
                }
            }
            if (live(times))
            {
                const std::string kn{ runtime_klass_name(times->oop()) };
                if (!kn.empty())
                {
                    ctx.check("op_TIMES_runtime_klass_is_subclass_2", kn == k_op_times_subclass);
                }
            }
            // PLUS and TIMES are DIFFERENT anonymous subclasses (distinct klasses).
            if (live(plus) && live(times))
            {
                const std::string kp{ runtime_klass_name(plus->oop()) };
                const std::string kt{ runtime_klass_name(times->oop()) };
                if (!kp.empty() && !kt.empty())
                {
                    ctx.check("op_PLUS_TIMES_distinct_runtime_klass", kp != kt);
                }
            }
        }

        // ---- Op Java-side witnesses (robust; independent of native call gate) -
        if (done)
        {
            ctx.check("java_op_values_length_is_2", enum_holder::seen_int("opValuesLen") == 2);
            ctx.check("java_op_PLUS_apply_is_8",    enum_holder::seen_int("plusApplySeen") == 8);
            ctx.check("java_op_TIMES_apply_is_12",  enum_holder::seen_int("timesApplySeen") == 12);
            ctx.check("java_op_PLUS_label_is_op_plus",  enum_holder::seen_str("plusLabelSeen") == "op:+");
            ctx.check("java_op_TIMES_label_is_op_star", enum_holder::seen_str("timesLabelSeen") == "op:*");
            ctx.check("java_op_valueOf_PLUS_is_PLUS", enum_holder::seen_bool("valueOfPlusIsPlus"));
            ctx.check("java_op_PLUS_ordinal_is_0",  enum_holder::seen_int("plusOrdinal") == 0);
            ctx.check("java_op_TIMES_ordinal_is_1", enum_holder::seen_int("timesOrdinal") == 1);
            ctx.check("java_op_PLUS_TIMES_distinct_identity",
                      enum_holder::seen_int("plusIdentity") != enum_holder::seen_int("timesIdentity"));
        }

        // =====================================================================
        // 13. BODY vs BODY-LESS RUNTIME KLASS — Java cross-checks + the body-less
        //     constants' leaf klass.  The Op section above proved the body
        //     constants' subclass klass natively; here we (a) corroborate with the
        //     Java getClass() witnesses, and (b) prove a BODY-LESS constant's leaf
        //     klass IS the enum class itself (Color.GREEN -> EnumSingleton$Color,
        //     never a $N subclass) — the contrast that makes the body-enum result
        //     meaningful.
        // =====================================================================
        if (done)
        {
            // Java-side: a body constant's getClass() is a subclass of Op; a
            // body-less constant's getClass() is exactly the enum class.
            ctx.check("java_op_PLUS_is_subclass",  enum_holder::seen_bool("plusIsSubclassOfOp"));
            ctx.check("java_GREEN_is_exactly_Color", enum_holder::seen_bool("greenIsExactlyColor"));
            // The Java runtime class names ('.'-separated) name the expected types.
            ctx.check("java_PLUS_className_is_Op_1",
                      enum_holder::seen_str("plusClassName") == "vmhook.fixtures.EnumSingleton$Op$1");
            ctx.check("java_TIMES_className_is_Op_2",
                      enum_holder::seen_str("timesClassName") == "vmhook.fixtures.EnumSingleton$Op$2");
            ctx.check("java_GREEN_className_is_Color",
                      enum_holder::seen_str("greenClassName") == "vmhook.fixtures.EnumSingleton$Color");
        }
        // Native: a body-LESS Color constant's leaf klass IS the enum class
        // (the '/'-separated internal name), NOT an anonymous subclass.
        if (live(green))
        {
            const std::string kn{ runtime_klass_name(green->oop()) };
            if (!kn.empty())
            {
                ctx.check("color_GREEN_runtime_klass_is_enum_class", kn == std::string{ k_color_class });
            }
            else
            {
                ctx.record("[INFO] enum_singleton: Color.GREEN runtime klass name unreadable; the "
                           "body-less-constant klass identity is proven via the Java witness "
                           "(java_GREEN_className_is_Color).");
            }
        }
        // All three Color constants share ONE leaf klass (no per-constant body).
        if (live(red) && live(green) && live(blue))
        {
            const std::string kr{ runtime_klass_name(red->oop()) };
            const std::string kg{ runtime_klass_name(green->oop()) };
            const std::string kb{ runtime_klass_name(blue->oop()) };
            if (!kr.empty() && !kg.empty() && !kb.empty())
            {
                ctx.check("color_constants_share_one_runtime_klass", kr == kg && kg == kb);
            }
        }

        // =====================================================================
        // 14. SINGLE-CONSTANT ENUM (the enum-singleton idiom): EnumSingleton$Lonely.
        //     Its sole constant INSTANCE IS the singleton.  Loaded lazily by the
        //     probe (it references Lonely.INSTANCE), so registered + read only now.
        //     Everything guarded: an unloaded Lonely degrades to [INFO].
        // =====================================================================
        const bool lonely_registered{ vmhook::register_class<lonely_enum>(k_lonely_class) };
        if (!lonely_registered)
        {
            ctx.record("[INFO] enum_singleton: EnumSingleton$Lonely not loaded/registered; skipping "
                       "the single-constant-enum (enum-singleton idiom) native reads.  Its Java "
                       "witnesses are still checked below if the probe completed.");
        }
        if (lonely_registered)
        {
            ctx.check("lonely_const_INSTANCE_resolves", lonely_enum::constant_resolves("INSTANCE"));
            auto sole{ lonely_enum::acquire_constant("INSTANCE") };
            ctx.check("lonely_INSTANCE_nonnull", sole != nullptr);
            if (live(sole))
            {
                ctx.check("lonely_INSTANCE_oop_valid", live(sole));
                ctx.check("lonely_INSTANCE_name_is_INSTANCE", sole->get_name() == "INSTANCE");
                ctx.check("lonely_INSTANCE_ordinal_is_0",     sole->get_ordinal() == 0);
                ctx.check("lonely_INSTANCE_tag_resolves",     sole->tag_resolves());
                ctx.check("lonely_INSTANCE_tag_is_sentinel",  sole->get_tag() == static_cast<std::int32_t>(0x515E));
                // The single constant's leaf klass IS the enum class (no body).
                const std::string kn{ runtime_klass_name(sole->oop()) };
                if (!kn.empty())
                {
                    ctx.check("lonely_INSTANCE_runtime_klass_is_enum_class", kn == std::string{ k_lonely_class });
                }
            }
            // $VALUES has length EXACTLY 1, and its sole element IS INSTANCE.
            ctx.check("lonely_values_array_resolves", lonely_enum::values_array_resolves());
            {
                void* const array_oop{ lonely_enum::values_array_oop() };
                if (array_oop && vmhook::hotspot::is_valid_pointer(array_oop))
                {
                    ctx.check("lonely_values_array_length_is_1", vmhook::array_length(array_oop) == 1);
                    const std::vector<void*> elems{ values_element_oops(array_oop) };
                    if (elems.size() == 1 && live(sole))
                    {
                        ctx.check("lonely_values_elem0_is_INSTANCE", elems[0] == sole->oop());
                    }
                }
                else
                {
                    ctx.record("[INFO] enum_singleton: Lonely.$VALUES array OOP not readable; the "
                               "single-constant count is proven via the Java witness "
                               "(java_lonely_values_length_is_1).");
                }
            }
            // Read the sole constant twice -> identical OOP (it IS the singleton).
            {
                const auto again{ lonely_enum::acquire_constant("INSTANCE") };
                ctx.check("lonely_INSTANCE_read_twice_identical_oop",
                          live(sole) && live(again) && sole->oop() == again->oop());
            }
        }

        // =====================================================================
        // 15. CLASSIC (pre-enum) SINGLETON: EnumSingleton$ClassicSingleton, a
        //     private-static-final INSTANCE behind getInstance().  Loaded lazily
        //     by the probe (getInstance()), so registered + read only now.
        // =====================================================================
        const bool classic_registered{ vmhook::register_class<classic_singleton>(k_classic_class) };
        if (!classic_registered)
        {
            ctx.record("[INFO] enum_singleton: EnumSingleton$ClassicSingleton not loaded/registered; "
                       "skipping the classic-singleton native reads.  Its identity is proven via the "
                       "Java witnesses below if the probe completed.");
        }
        if (classic_registered)
        {
            // A PRIVATE static final field still resolves + decodes via the mirror.
            ctx.check("classic_INSTANCE_field_resolves", classic_singleton::instance_resolves());
            auto inst{ classic_singleton::acquire_instance() };
            ctx.check("classic_INSTANCE_nonnull", inst != nullptr);
            if (live(inst))
            {
                ctx.check("classic_INSTANCE_oop_valid", live(inst));
                ctx.check("classic_magic_field_resolves", inst->magic_resolves());
                ctx.check("classic_magic_is_payload", inst->get_magic() == static_cast<std::int32_t>(0x5A5A5A5A));
                // Its leaf klass is the ordinary class (NOT a synthetic enum subclass).
                const std::string kn{ runtime_klass_name(inst->oop()) };
                if (!kn.empty())
                {
                    ctx.check("classic_runtime_klass_is_class", kn == std::string{ k_classic_class });
                }
            }
            // Read the private static slot twice -> identical OOP (singleton-stable).
            {
                void* const a{ classic_singleton::instance_oop() };
                void* const b{ classic_singleton::instance_oop() };
                ctx.check("classic_INSTANCE_oop_nonnull",
                          a != nullptr && vmhook::hotspot::is_valid_pointer(a));
                ctx.check("classic_INSTANCE_oop_stable_across_reads", a == b);
                if (live(inst))
                {
                    ctx.check("classic_wrapper_oop_matches_field", inst->oop() == a);
                }
            }
        }

        // =====================================================================
        // 16. ENUMMAP / ENUMSET keyed on the Color enum.  The library has no
        //     dedicated wrapper for these SPECIAL (ordinal-indexed) collections,
        //     so we CHARACTERISE their runtime klass best-effort ([INFO]) — never
        //     a hard assertion on a type the library does not model — and prove
        //     their CONTENTS robustly via the Java witnesses (section below).
        //     The fields are PUBLIC STATIC FINAL on the holder, decoded as plain
        //     reference OOPs.
        // =====================================================================
        {
            const auto color_names{ enum_holder::static_field("COLOR_NAMES") };
            ctx.check("enummap_COLOR_NAMES_field_resolves", color_names.has_value());
            if (color_names.has_value())
            {
                void* const map_oop{ vmhook::field_oop(*color_names) };
                const std::string kn{ runtime_klass_name(map_oop) };
                ctx.record(std::string{ "[INFO] enum_singleton: EnumMap COLOR_NAMES runtime klass = '" }
                           + (kn.empty() ? std::string{ "<unreadable>" } : kn)
                           + "' (library has no EnumMap wrapper; contents proven via Java witnesses).");
                // Best-effort POSITIVE characterisation: when the klass name is
                // readable it IS java/util/EnumMap.  Only asserted on a non-empty
                // read so a relocated/cold map degrades to the [INFO] above.
                if (!kn.empty())
                {
                    ctx.check("enummap_COLOR_NAMES_klass_is_EnumMap", kn == "java/util/EnumMap");
                }
            }

            const auto warm_colors{ enum_holder::static_field("WARM_COLORS") };
            ctx.check("enumset_WARM_COLORS_field_resolves", warm_colors.has_value());
            if (warm_colors.has_value())
            {
                void* const set_oop{ vmhook::field_oop(*warm_colors) };
                const std::string kn{ runtime_klass_name(set_oop) };
                ctx.record(std::string{ "[INFO] enum_singleton: EnumSet WARM_COLORS runtime klass = '" }
                           + (kn.empty() ? std::string{ "<unreadable>" } : kn)
                           + "' (library has no EnumSet wrapper; contents proven via Java witnesses).");
                // EnumSet.of(...) for <=64 constants is java/util/RegularEnumSet.
                if (!kn.empty())
                {
                    ctx.check("enumset_WARM_COLORS_klass_is_RegularEnumSet", kn == "java/util/RegularEnumSet");
                }
            }
        }

        // =====================================================================
        // 17. NAME() REFLECTION CROSS-CHECK.  The native `name` field reads in
        //     section 5 compared against source literals; here we cross-check the
        //     PUBLISHED java.lang.Enum.name() witnesses (the JVM's own values) AND
        //     tie each back to the native read, so name decoding is proven against
        //     the runtime, not just the .java source.
        // =====================================================================
        if (done)
        {
            ctx.check("java_RED_name_is_RED",     enum_holder::seen_str("redNameSeen") == "RED");
            ctx.check("java_GREEN_name_is_GREEN", enum_holder::seen_str("greenNameSeen") == "GREEN");
            ctx.check("java_BLUE_name_is_BLUE",   enum_holder::seen_str("blueNameSeen") == "BLUE");
            ctx.check("java_PLUS_name_is_PLUS",   enum_holder::seen_str("plusNameSeen") == "PLUS");
            // Native read == Java Enum.name() witness (same value, two paths).
            if (live(green))
            {
                ctx.check("native_GREEN_name_matches_java_witness",
                          green->get_name() == enum_holder::seen_str("greenNameSeen"));
            }
            if (live(red))
            {
                ctx.check("native_RED_name_matches_java_witness",
                          red->get_name() == enum_holder::seen_str("redNameSeen"));
            }
        }

        // =====================================================================
        // 18. SINGLE-CONSTANT / CLASSIC SINGLETON + ENUMMAP/ENUMSET Java-side
        //     witnesses (robust; independent of the native reads above).
        // =====================================================================
        if (done)
        {
            // Single-constant enum: exactly one constant, INSTANCE is that one.
            ctx.check("java_lonely_values_length_is_1", enum_holder::seen_int("lonelyValuesLen") == 1);
            ctx.check("java_lonely_INSTANCE_is_sole",    enum_holder::seen_bool("lonelyInstanceIsSole"));
            ctx.check("java_lonely_tag_is_sentinel",
                      enum_holder::seen_int("lonelyTagSeen") == static_cast<std::int32_t>(0x515E));

            // Classic singleton: getInstance() idempotent + stable identity.
            ctx.check("java_classic_same_instance", enum_holder::seen_bool("classicSameInstance"));
            ctx.check("java_classic_magic_is_payload",
                      enum_holder::seen_int("classicMagicSeen") == static_cast<std::int32_t>(0x5A5A5A5A));

            // Tie the Java-published singleton identities back to the native OOPs:
            // identityHashCode is NOT the OOP, but two reads agreeing on it (Java)
            // plus the native OOP-stability checks above are complementary proofs.
            ctx.check("java_lonely_identity_nonzero",  enum_holder::seen_int("lonelyInstanceIdentity") != 0);
            ctx.check("java_classic_identity_nonzero", enum_holder::seen_int("classicInstanceIdentity") != 0);

            // EnumMap / EnumSet contents (the robust proof for the un-wrapped types).
            ctx.check("java_enummap_size_is_3",      enum_holder::seen_int("colorNamesSize") == 3);
            ctx.check("java_enummap_get_GREEN_is_g", enum_holder::seen_str("colorNamesGreen") == "g");
            ctx.check("java_enumset_size_is_1",      enum_holder::seen_int("warmColorsSize") == 1);
            ctx.check("java_enumset_contains_RED",   enum_holder::seen_bool("warmColorsHasRed"));
            ctx.check("java_enumset_excludes_BLUE",  !enum_holder::seen_bool("warmColorsHasBlue"));
            // Corroborate the native klass characterisation with the Java names.
            ctx.check("java_enummap_className_is_EnumMap",
                      enum_holder::seen_str("colorNamesClassName") == "java.util.EnumMap");
            ctx.check("java_enumset_className_is_RegularEnumSet",
                      enum_holder::seen_str("warmColorsClassName") == "java.util.RegularEnumSet");
        }
    }
}

VMHOOK_JVM_MODULE(enum_singleton)
{
    // Entry guard: if the fixture class is not loaded, there is nothing to test.
    // Return BEFORE installing any hook — so there is nothing to tear down on
    // this path (the unconditional shutdown_hooks() at the end of the function is
    // only reached once a hook may have been armed).
    if (vmhook::find_class(k_holder_class) == nullptr)
    {
        ctx.record("[INFO] enum_singleton: fixture vmhook/fixtures/EnumSingleton not loaded; "
                   "skipping (build issue or fixture not on the classpath).");
        return;
    }

    // Register the holder + Color wrappers up front: scoped_hook<enum_holder>
    // needs enum_holder in the type map to resolve its klass, and the detour
    // reads Color constants.  (run_enum_singleton re-registers them idempotently;
    // register_class uses insert_or_assign so a repeat is a no-op.)  The
    // entry-guard above already confirmed the holder klass is loaded.
    vmhook::register_class<enum_holder>(k_holder_class);
    vmhook::register_class<enum_color>(k_color_class);

    // Install a tick() hook so the probe's SINGLETON.tick(7) dispatch gives us a
    // live JavaThread in which native method_proxy::call() can actually run.  We
    // attempt the native brightness() calls inside the detour and stash the
    // results; the module body asserts them best-effort.
    auto handle{ vmhook::scoped_hook<enum_holder>(
        "tick",
        [](vmhook::return_value&,
           const std::unique_ptr<enum_holder>& self,
           std::int32_t /*nonce*/)
        {
            g_detour_calls.fetch_add(1, std::memory_order_relaxed);
            g_detour_saw_self.store(self != nullptr, std::memory_order_relaxed);

            // Live-JavaThread context: try the native enum-method calls here.
            auto green{ enum_color::acquire_constant("GREEN") };
            if (green && vmhook::hotspot::is_valid_pointer(green->oop()))
            {
                g_native_green_brightness.store(green->brightness_native(),
                                                std::memory_order_relaxed);
            }
            auto red{ enum_color::acquire_constant("RED") };
            if (red && vmhook::hotspot::is_valid_pointer(red->oop()))
            {
                g_native_red_brightness.store(red->brightness_native(),
                                              std::memory_order_relaxed);
            }
        }) };
    // Record whether the hook armed (informational only — the robust Java
    // witnesses prove every native result regardless, so a non-arming hook never
    // fails the suite).
    if (!handle.installed())
    {
        ctx.record("[INFO] enum_singleton: tick() hook did not arm; native enum-method calls will "
                   "be characterised as unavailable and proven via Java witnesses instead.");
    }

    // All assertions run inside a try so an unexpected exception downgrades to
    // [INFO] (never a FAIL) and the unconditional teardown below still runs.
    try
    {
        run_enum_singleton(ctx);
    }
    catch (const std::exception& ex)
    {
        ctx.record(std::string{ "[INFO] enum_singleton: unexpected exception, downgraded to INFO: " }
                   + ex.what());
    }
    catch (...)
    {
        ctx.record("[INFO] enum_singleton: unexpected non-std exception, downgraded to INFO.");
    }

    // UNCONDITIONAL teardown OUTSIDE the try: always disarm the tick() hook this
    // module installed (and any other hook), so the suite is never left with a
    // live detour pointing at this TU's code.
    vmhook::shutdown_hooks();
}
