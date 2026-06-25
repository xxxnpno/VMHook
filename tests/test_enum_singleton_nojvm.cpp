// No-JVM contract checks for the enum_singleton feature: reading Java enum
// constants as wrapped heap singletons through the generic field_proxy /
// static_field / object<T> CRTP machinery.
//
// Wave-32 ledger gaps closed here:
//   (1) COMPILE-TIME enum-wrapper detection: a user-declared `class Color :
//       vmhook::object<Color>` IS a vmhook wrapper — base_of<object_base>,
//       sized exactly like object_base (no per-constant state added by the
//       wrapper layer), nothrow-constructible from oop_t/nullptr, and
//       polymorphic-deletable through object_base*.
//   (2) COLD-STATE singleton accessors return null / safe-default: with no
//       JVM, `Color::static_field("RED")` returns std::nullopt and the
//       user-friendly wrapper pattern `static_field("RED")->get().get<...>()`
//       collapses cleanly when guarded — characterizes the
//       acquire_constant()→nullptr path the JVM module's
//       k_*_resolves==false FAILs report.  We never call .value() on a
//       nullopt — we PIN the nullopt itself.
//   (3) static_assert pins on the function signatures the enum_singleton
//       module reaches for (static_field, static_method, get_field, the
//       field_proxy return type, the make_unique<W> return type).  These are
//       the SAME accessors that decode RED/GREEN/BLUE on a live JVM, so the
//       signatures here ARE the feature's ABI.
//
// All checks are pure-logic.  No JVM, no detour, no dereference.
#include <vmhook/vmhook.hpp>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

static int failures{ 0 };
static auto check(char const* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

// ---------------------------------------------------------------------------
// User-declared enum wrapper, exactly the way an enum_singleton consumer
// would write it: a thin CRTP veneer over object<T>.  The Java side has an
// `rgb` int field and a `brightness()` method — we never call them without
// a JVM, but we pin that the static accessors COMPILE and return the right
// types.
// ---------------------------------------------------------------------------
class Color : public vmhook::object<Color>
{
public:
    explicit Color(vmhook::oop_t oop) noexcept
        : vmhook::object<Color>{ oop }
    {
    }
};

class EnumSingleton : public vmhook::object<EnumSingleton>
{
public:
    explicit EnumSingleton(vmhook::oop_t oop) noexcept
        : vmhook::object<EnumSingleton>{ oop }
    {
    }
};

// ---------------------------------------------------------------------------
// (1) COMPILE-TIME enum-wrapper detection.
// ---------------------------------------------------------------------------
static_assert(std::is_base_of_v<vmhook::object_base, Color>);
static_assert(std::is_base_of_v<vmhook::object<Color>, Color>);
static_assert(std::is_base_of_v<vmhook::object_base, EnumSingleton>);
// The wrapper layer adds NO per-constant state — the OOP is the only thing
// stored in object_base, so RED/GREEN/BLUE are distinguished by OOP value,
// not by per-instance C++ state.  Equal sizes pin that invariant.
static_assert(sizeof(Color) == sizeof(vmhook::object_base));
static_assert(sizeof(EnumSingleton) == sizeof(vmhook::object_base));
// Polymorphic & destructible — required by the unique_ptr<Color> the
// field_proxy value_t -> wrapper decode path hands back.
static_assert(std::has_virtual_destructor_v<vmhook::object_base>);
static_assert(std::is_polymorphic_v<Color>);
static_assert(std::is_nothrow_destructible_v<Color>);
// Constructible from oop_t / nullptr — the exact ctor signature the
// register_class factory pokes via `new Color{ decoded_oop }`.
static_assert(std::is_nothrow_constructible_v<Color, vmhook::oop_t>);
static_assert(std::is_nothrow_constructible_v<Color, std::nullptr_t>);
static_assert(std::is_nothrow_constructible_v<EnumSingleton, vmhook::oop_t>);
// Copy + move semantics inherited from object_base (the enum_singleton
// module relies on these to alias a constant OOP across multiple reads:
// `acquire_constant("GREEN")` twice yields two unique_ptrs that share the
// same wrapped OOP).
static_assert(std::is_copy_constructible_v<Color>);
static_assert(std::is_nothrow_move_constructible_v<Color>);

// ---------------------------------------------------------------------------
// (3) SIGNATURE static_asserts on the accessors enum_singleton.cpp reaches
//     for.  These are the feature's ABI — if any of these change shape, the
//     JVM module fails to compile, so pinning them here turns ANY drift into
//     a static_assert failure on every CI compiler/STL.
// ---------------------------------------------------------------------------
static_assert(std::is_same_v<
    decltype(Color::static_field(std::string_view{})),
    std::optional<vmhook::field_proxy>>);
static_assert(std::is_same_v<
    decltype(Color::static_method(std::string_view{})),
    std::optional<vmhook::method_proxy>>);
static_assert(std::is_same_v<
    decltype(EnumSingleton::static_field(std::string_view{})),
    std::optional<vmhook::field_proxy>>);

// The non-deducing-this static fallback object_base::get_field(type_index,
// name) is what static_field forwards to — pin its return type too.
static_assert(std::is_same_v<
    decltype(vmhook::object_base::get_field(
        std::declval<std::type_index>(), std::string_view{})),
    std::optional<vmhook::field_proxy>>);
static_assert(std::is_same_v<
    decltype(vmhook::object_base::get_method(
        std::declval<std::type_index>(), std::string_view{})),
    std::optional<vmhook::method_proxy>>);

// make_unique<W> returns a plain std::unique_ptr<W> with the default deleter
// — same shape used by the decode-OOP path that produces a wrapped Color.
static_assert(std::is_same_v<
    decltype(vmhook::make_unique<Color>()),
    std::unique_ptr<Color>>);
static_assert(std::is_same_v<
    std::unique_ptr<Color>::deleter_type,
    std::default_delete<Color>>);

int main()
{
    // =====================================================================
    // (2) COLD-STATE singleton accessor returns null / safe-default.
    //     With NO JVM attached, every klass lookup fails, so every
    //     static_field / static_method call returns std::nullopt and every
    //     instance get_field returns std::nullopt.  The enum_singleton
    //     module collapses each nullopt into `nullptr` for its wrapper and
    //     records a clean FAIL on the corresponding *_resolves check — pin
    //     the source of that behavior HERE.
    // =====================================================================
    {
        auto const red{ Color::static_field("RED") };
        check("cold_state_static_field_RED_is_nullopt", !red.has_value());
        auto const green{ Color::static_field("GREEN") };
        check("cold_state_static_field_GREEN_is_nullopt", !green.has_value());
        auto const blue{ Color::static_field("BLUE") };
        check("cold_state_static_field_BLUE_is_nullopt", !blue.has_value());
        auto const singleton{ EnumSingleton::static_field("SINGLETON") };
        check("cold_state_static_field_SINGLETON_is_nullopt", !singleton.has_value());
    }
    {
        auto const brightness{ Color::static_method("brightness") };
        check("cold_state_static_method_brightness_is_nullopt", !brightness.has_value());
        auto const sig{ Color::static_method("brightness", "()I") };
        check("cold_state_static_method_brightness_sig_is_nullopt", !sig.has_value());
    }
    {
        // make_unique<Color>() without a JVM produces a default-constructed
        // unique_ptr (the factory path bails when find_class returns null).
        // Whatever it returns must NOT crash and the unique_ptr's deleter
        // type must still be the default deleter (pinned in static_assert
        // above).
        auto u{ vmhook::make_unique<Color>() };
        check("cold_state_make_unique_returns_safe_default",
              u == nullptr);
    }
    {
        // Instance-context get_field on a null-OOP-wrapped Color: the
        // wrapper layer constructs cleanly, the get_field call returns
        // nullopt (no klass mapped without register_class + a JVM), and the
        // null OOP is preserved.
        void* const sentinel{ reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xC0FFEE10)) };
        Color c{ nullptr };
        check("cold_state_null_wrapped_oop_round_trips",
              c.vmhook::object_base::get_instance() == nullptr);
        Color s{ sentinel };
        check("cold_state_sentinel_wrapped_oop_round_trips",
              s.vmhook::object_base::get_instance() == sentinel);
        // The wrapper is still safely upcastable to object_base* — the same
        // upcast the JVM module's identity checks rely on.
        vmhook::object_base* const pc{ &c };
        vmhook::object_base* const ps{ &s };
        check("cold_state_upcast_null_ok",     pc != nullptr && pc->get_instance() == nullptr);
        check("cold_state_upcast_sentinel_ok", ps != nullptr && ps->get_instance() == sentinel);
        // Distinct C++ types for Color vs EnumSingleton — the type_index keying
        // the register_class map relies on must distinguish them.
        EnumSingleton es{ sentinel };
        check("cold_state_color_vs_enumsingleton_typeids_differ",
              typeid(c) != typeid(es));
    }
    {
        // Identity-via-OOP-equality: two Color wrappers built from the SAME
        // sentinel compare equal at the OOP level.  This is the C++ side of
        // the JVM module's "favoriteColor IS GREEN" assertion — singletons
        // alias the constant OOP.
        void* const oopA{ reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xAAAA0000)) };
        void* const oopB{ reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xBBBB0000)) };
        Color a1{ oopA };
        Color a2{ oopA };
        Color b{  oopB };
        check("identity_same_oop_two_wrappers_equal",
              a1.vmhook::object_base::get_instance()
                  == a2.vmhook::object_base::get_instance());
        check("identity_different_oop_distinct",
              a1.vmhook::object_base::get_instance()
                  != b.vmhook::object_base::get_instance());
    }
    {
        // Polymorphic delete of a null-OOP-wrapped Color through
        // object_base* — the unique_ptr<Color> path the enum_singleton
        // decode uses must release the wrapper cleanly even when the OOP
        // is null (cold-state safe-default).
        std::unique_ptr<Color> u{ new Color{ nullptr } };
        check("cold_state_unique_ptr_holds_null_wrapped",
              u && u->vmhook::object_base::get_instance() == nullptr);
        // Scope exit deletes through Color's vtable -> object_base's virtual
        // dtor — must not throw, must not crash.
    }

    std::printf("%s: %d failure(s)\n", failures == 0 ? "OK" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
