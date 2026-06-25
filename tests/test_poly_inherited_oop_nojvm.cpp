// Cold-state (no-JVM) contract checks for the poly_inherited_oop feature.
//
// The JVM module proves that vmhook::object<T>::get_field / get_method walk
// the Klass::_super chain to resolve INHERITED instance fields/methods through
// a subclass wrapper.  Without a JVM we cannot exercise the walk itself, but
// we CAN pin every safe-default the wrapper must honour cold:
//
//   (1) A three-level wrapper hierarchy (poly_a < poly_b < poly_c), each its
//       own object<T> CRTP root, is constructible with a sentinel oop AND with
//       a null oop, copyable/movable, polymorphically delete-safe via
//       object_base*, and reads back the held instance through both the
//       deriving accessor and the base accessor — independently of which
//       layer is registered.
//   (2) NO klass is registered for any layer (register_class<T> is never
//       called).  In that cold state object<T>::get_field("x") on any wrapper
//       layer MUST return std::nullopt rather than dereferencing — proving the
//       type-not-registered branch (resolve_klass()->nullptr) is fail-safe.
//   (3) The same call on a NULL OOP (every wrapper holds nullptr) ALSO must
//       return nullopt — no crash from the instance pointer being null.
//   (4) object<T>::get_method("x") cold has the same nullopt contract for
//       BOTH the single-name and name+signature overloads.
//   (5) The same-oop / cross-layer aliasing the JVM module relies on
//       (a B oop also viewable through an A wrapper) is value-equal at the
//       object_base layer: build poly_a{sentinel} and poly_c{sentinel}; both
//       get_instance() round-trip to the same byte pattern.  Pinned both with
//       static_assert (constexpr-style traits on the wrappers themselves) AND
//       at runtime.
//   (6) Polymorphic delete through object_base* of any layer's wrapper
//       containing a NULL oop runs the virtual dtor and does not touch the
//       (null) instance.
//
// All checks are pure-logic; no pointer is ever decoded.  Runs on every CI
// compiler/STL without a JVM.
#include <vmhook/vmhook.hpp>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

// Three-level wrappers — each layer is its own CRTP root so type_to_class_map
// keying is per-layer (matches the JVM module's pi_a / pi_b registration model).
class poly_a : public vmhook::object<poly_a>
{
public:
    explicit poly_a(vmhook::oop_t oop) noexcept
        : vmhook::object<poly_a>{ oop } {}
};

class poly_b : public vmhook::object<poly_b>
{
public:
    explicit poly_b(vmhook::oop_t oop) noexcept
        : vmhook::object<poly_b>{ oop } {}
};

class poly_c : public vmhook::object<poly_c>
{
public:
    explicit poly_c(vmhook::oop_t oop) noexcept
        : vmhook::object<poly_c>{ oop } {}
};

// ---------------------------------------------------------------------------
// COMPILE-TIME contracts — base/derived wrapper conversion and value semantics.
// ---------------------------------------------------------------------------

// Every layer is an object_base AND its own object<T>.
static_assert(std::is_base_of_v<vmhook::object_base, poly_a>);
static_assert(std::is_base_of_v<vmhook::object_base, poly_b>);
static_assert(std::is_base_of_v<vmhook::object_base, poly_c>);
static_assert(std::is_base_of_v<vmhook::object<poly_a>, poly_a>);
static_assert(std::is_base_of_v<vmhook::object<poly_b>, poly_b>);
static_assert(std::is_base_of_v<vmhook::object<poly_c>, poly_c>);

// Layers are NOT base-of each other — they are sibling CRTP roots.  This is
// the same fixture pattern the JVM poly_inherited_oop module uses (pi_a / pi_b
// are sibling wrappers around klasses that DO inherit, but the C++ wrappers
// share no inheritance).
static_assert(!std::is_base_of_v<poly_a, poly_b>);
static_assert(!std::is_base_of_v<poly_b, poly_a>);
static_assert(!std::is_base_of_v<poly_a, poly_c>);
static_assert(!std::is_base_of_v<poly_c, poly_a>);

// No layer adds storage — the wrapper is a pure CRTP veneer over the single
// oop the base holds.
static_assert(sizeof(poly_a) == sizeof(vmhook::object_base));
static_assert(sizeof(poly_b) == sizeof(vmhook::object_base));
static_assert(sizeof(poly_c) == sizeof(vmhook::object_base));

// Conversion contract: a poly_X* is convertible to object_base* (the upcast
// the library's polymorphic delete path takes), but the reverse is NOT
// implicit — a same-oop A/B/C view is a deliberate construction, never a
// silent narrowing/widening conversion.
static_assert(std::is_convertible_v<poly_a*, vmhook::object_base*>);
static_assert(std::is_convertible_v<poly_b*, vmhook::object_base*>);
static_assert(std::is_convertible_v<poly_c*, vmhook::object_base*>);
static_assert(!std::is_convertible_v<vmhook::object_base*, poly_a*>);
static_assert(!std::is_convertible_v<poly_a*, poly_b*>);
static_assert(!std::is_convertible_v<poly_b*, poly_a*>);

// Polymorphic delete safety: every wrapper has a virtual dtor reachable via
// object_base*.
static_assert(std::has_virtual_destructor_v<vmhook::object_base>);
static_assert(std::has_virtual_destructor_v<poly_a>);
static_assert(std::has_virtual_destructor_v<poly_b>);
static_assert(std::has_virtual_destructor_v<poly_c>);

// Cold-construction contract: every layer is nothrow-constructible from a raw
// oop_t AND from std::nullptr_t (the "wrapped null" state).
static_assert(std::is_nothrow_constructible_v<poly_a, vmhook::oop_t>);
static_assert(std::is_nothrow_constructible_v<poly_b, vmhook::oop_t>);
static_assert(std::is_nothrow_constructible_v<poly_c, vmhook::oop_t>);
static_assert(std::is_nothrow_constructible_v<poly_a, std::nullptr_t>);
static_assert(std::is_nothrow_constructible_v<poly_b, std::nullptr_t>);
static_assert(std::is_nothrow_constructible_v<poly_c, std::nullptr_t>);

// Value semantics inherited from object_base — pin per derived layer so a
// future regression in the CRTP layout shows up here.
static_assert(std::is_copy_constructible_v<poly_a>);
static_assert(std::is_copy_constructible_v<poly_b>);
static_assert(std::is_copy_constructible_v<poly_c>);
static_assert(std::is_nothrow_move_constructible_v<poly_a>);
static_assert(std::is_nothrow_move_constructible_v<poly_b>);
static_assert(std::is_nothrow_move_constructible_v<poly_c>);
static_assert(std::is_nothrow_move_assignable_v<poly_a>);
static_assert(std::is_nothrow_move_assignable_v<poly_b>);
static_assert(std::is_nothrow_move_assignable_v<poly_c>);

// get_field / get_method return types are std::optional — the cold-fail contract
// hinges on this exact return type.
static_assert(std::is_same_v<
    decltype(std::declval<const poly_b&>().vmhook::object_base::get_field(std::string_view{})),
    std::optional<vmhook::field_proxy>>);
static_assert(std::is_same_v<
    decltype(std::declval<const poly_b&>().vmhook::object_base::get_method(std::string_view{})),
    std::optional<vmhook::method_proxy>>);
static_assert(std::is_same_v<
    decltype(std::declval<const poly_b&>().vmhook::object_base::get_method(std::string_view{}, std::string_view{})),
    std::optional<vmhook::method_proxy>>);

int main()
{
    void* const sentinel{ reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xDEADBEEF)) };

    // ===================================================================
    // (1) Construction + base/derived round-trip across all three layers.
    // ===================================================================
    {
        poly_a a{ sentinel };
        poly_b b{ sentinel };
        poly_c c{ sentinel };
        check("ctor_poly_a_round_trips", a.get_instance() == sentinel);
        check("ctor_poly_b_round_trips", b.get_instance() == sentinel);
        check("ctor_poly_c_round_trips", c.get_instance() == sentinel);

        // Upcast to object_base* reads the SAME oop.
        vmhook::object_base* const pa{ &a };
        vmhook::object_base* const pb{ &b };
        vmhook::object_base* const pc{ &c };
        check("upcast_poly_a_same_oop", pa->get_instance() == sentinel);
        check("upcast_poly_b_same_oop", pb->get_instance() == sentinel);
        check("upcast_poly_c_same_oop", pc->get_instance() == sentinel);

        // Distinct typeids — the type_to_class_map keying by typeid(*this) will
        // see three different layers, mirroring the JVM module's pi_a / pi_b
        // wrapper distinction.
        check("typeid_a_b_differ", typeid(a) != typeid(b));
        check("typeid_b_c_differ", typeid(b) != typeid(c));
        check("typeid_a_c_differ", typeid(a) != typeid(c));
    }

    // ===================================================================
    // (5) Same-oop / cross-layer aliasing: viewing the same sentinel through
    //     poly_a, poly_b, poly_c yields three wrappers whose get_instance()
    //     all read the IDENTICAL pointer.  This is the C++-side analogue of
    //     the JVM module's inherited_field_B_and_A_same_address proof — the
    //     library never copies/decodes the oop on construction.
    // ===================================================================
    {
        poly_a a{ sentinel };
        poly_b b{ sentinel };
        poly_c c{ sentinel };
        const bool all_equal{
            a.get_instance() == b.get_instance() &&
            b.get_instance() == c.get_instance() &&
            a.get_instance() == sentinel
        };
        check("same_oop_three_layers_alias_identical_address", all_equal);
    }

    // ===================================================================
    // (2) Cold get_field on UNREGISTERED wrappers: every layer's get_field
    //     must fall through resolve_klass() == nullptr and return nullopt.
    //     If the library ever started dereferencing without the registered
    //     klass this would crash — pin the safe default.
    // ===================================================================
    {
        const poly_a a{ sentinel };
        const poly_b b{ sentinel };
        const poly_c c{ sentinel };
        const auto fa{ a.vmhook::object_base::get_field("inheritedInt") };
        const auto fb{ b.vmhook::object_base::get_field("inheritedInt") };
        const auto fc{ c.vmhook::object_base::get_field("inheritedInt") };
        check("cold_get_field_poly_a_nullopt", !fa.has_value());
        check("cold_get_field_poly_b_nullopt", !fb.has_value());
        check("cold_get_field_poly_c_nullopt", !fc.has_value());
    }

    // ===================================================================
    // (3) Same call on a NULL oop wrapper: still nullopt, no crash from the
    //     null instance pointer.  resolve_klass() fails BEFORE the instance
    //     check, so the order of guards is also pinned here.
    // ===================================================================
    {
        const poly_a a{ nullptr };
        const poly_b b{ nullptr };
        const poly_c c{ nullptr };
        check("null_oop_poly_a_get_instance_is_null", a.get_instance() == nullptr);
        check("null_oop_poly_b_get_instance_is_null", b.get_instance() == nullptr);
        check("null_oop_poly_c_get_instance_is_null", c.get_instance() == nullptr);
        const auto fa{ a.vmhook::object_base::get_field("anyName") };
        const auto fb{ b.vmhook::object_base::get_field("anyName") };
        const auto fc{ c.vmhook::object_base::get_field("anyName") };
        check("null_oop_cold_get_field_poly_a_nullopt", !fa.has_value());
        check("null_oop_cold_get_field_poly_b_nullopt", !fb.has_value());
        check("null_oop_cold_get_field_poly_c_nullopt", !fc.has_value());
    }

    // ===================================================================
    // (4) Cold get_method (both single-name and name+signature) on every
    //     layer + null-oop wrapper: nullopt, no crash.
    // ===================================================================
    {
        const poly_a a{ sentinel };
        const poly_b b{ sentinel };
        const poly_c c_null{ nullptr };
        const auto ma{ a.vmhook::object_base::get_method("protectedAdd") };
        const auto mb{ b.vmhook::object_base::get_method("protectedAdd") };
        const auto mc{ c_null.vmhook::object_base::get_method("protectedAdd") };
        check("cold_get_method_name_poly_a_nullopt", !ma.has_value());
        check("cold_get_method_name_poly_b_nullopt", !mb.has_value());
        check("cold_get_method_name_null_oop_poly_c_nullopt", !mc.has_value());

        const auto ms_a{ a.vmhook::object_base::get_method("protectedAdd", "(I)I") };
        const auto ms_b{ b.vmhook::object_base::get_method("protectedAdd", "(I)I") };
        const auto ms_c{ c_null.vmhook::object_base::get_method("protectedAdd", "(I)I") };
        check("cold_get_method_sig_poly_a_nullopt", !ms_a.has_value());
        check("cold_get_method_sig_poly_b_nullopt", !ms_b.has_value());
        check("cold_get_method_sig_null_oop_poly_c_nullopt", !ms_c.has_value());
    }

    // ===================================================================
    // Move/copy across layers preserves oop identity; move nulls the source.
    // This pins the value-semantics contract per LAYER — a regression in
    // object_base's move would otherwise only show up at the base type.
    // ===================================================================
    {
        poly_a src{ sentinel };
        poly_a moved{ std::move(src) };
        check("move_ctor_poly_a_transfers", moved.get_instance() == sentinel);
        check("move_ctor_poly_a_nulls_source", src.get_instance() == nullptr);

        poly_b src_b{ sentinel };
        poly_b copy_b{ src_b };
        check("copy_ctor_poly_b_aliases",
              src_b.get_instance() == sentinel && copy_b.get_instance() == sentinel);

        poly_c src_c{ sentinel };
        poly_c dst_c{ nullptr };
        dst_c = std::move(src_c);
        check("move_assign_poly_c_transfers", dst_c.get_instance() == sentinel);
        check("move_assign_poly_c_nulls_source", src_c.get_instance() == nullptr);
    }

    // ===================================================================
    // (6) Polymorphic delete of a null-oop wrapper through object_base* is
    //     crash-safe at every layer.  No counter — survival is the assertion.
    // ===================================================================
    {
        vmhook::object_base* const wa{ new poly_a{ nullptr } };
        vmhook::object_base* const wb{ new poly_b{ nullptr } };
        vmhook::object_base* const wc{ new poly_c{ nullptr } };
        check("poly_delete_null_oop_poly_a_constructed", wa && wa->get_instance() == nullptr);
        check("poly_delete_null_oop_poly_b_constructed", wb && wb->get_instance() == nullptr);
        check("poly_delete_null_oop_poly_c_constructed", wc && wc->get_instance() == nullptr);
        delete wa;
        delete wb;
        delete wc;
        check("poly_delete_null_oop_all_layers_survived", true);
    }
    {
        // And via std::unique_ptr<object_base> holding a derived layer — the
        // default deleter reaches the derived dtor through the virtual.
        std::unique_ptr<vmhook::object_base> u_a{ new poly_a{ sentinel } };
        std::unique_ptr<vmhook::object_base> u_b{ new poly_b{ nullptr } };
        std::unique_ptr<vmhook::object_base> u_c{ new poly_c{ sentinel } };
        check("unique_ptr_base_holds_poly_a_oop", u_a && u_a->get_instance() == sentinel);
        check("unique_ptr_base_holds_poly_b_null_oop", u_b && u_b->get_instance() == nullptr);
        check("unique_ptr_base_holds_poly_c_oop", u_c && u_c->get_instance() == sentinel);
        // u_a/u_b/u_c destructors run at scope exit through the virtual dtor.
    }
    check("unique_ptr_base_poly_layers_destruction_survived", true);

    std::printf("%s: %d failure(s)\n", failures == 0 ? "OK" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
