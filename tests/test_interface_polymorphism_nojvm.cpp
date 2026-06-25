// No-JVM contract checks for the interface_polymorphism feature.
//
// The feature: a Java field's DECLARED slot type is an interface (e.g. Animal)
// but the RUNTIME oop is a concrete subclass (e.g. Dog).  vmhook handles this
// with a type-agnostic field decode — the field_proxy unique_ptr branch
// (vmhook.hpp:11821-11848) wraps WHATEVER oop the slot points at into the
// CALLER-REQUESTED wrapper type.  Two different wrapper C++ types can therefore
// be constructed from the SAME oop, and the C++ wrapper class hierarchy is
// INDEPENDENT of the Java interface/class hierarchy — the polymorphism lives
// entirely on the JVM side, enforced by HotSpot's vtable, not by the wrapper.
//
// Wave-32 ledger gaps covered here:
//   (A) SFINAE detector for "is a valid interface-polymorphism wrapper" —
//       must be (i) explicitly constructible from vmhook::oop_t, (ii) derived
//       from vmhook::object_base, (iii) inherit object_base's noexcept-move
//       guarantee.  Positive samples: animal_w, dog_w (both legal Animal/Dog
//       wrappers).  Negative samples: a struct without the oop ctor; a class
//       not derived from object_base; void.
//   (B) Compile-time matrix (base vs derived): pin that animal_w and dog_w —
//       which mirror a Java interface and its concrete implementor — are
//       PEER C++ TYPES, not related via inheritance.  This is the load-bearing
//       contract: a field of declared type Animal can be read AS Dog (and vice
//       versa) because the decode never consults a C++ is_base_of relation.
//   (C) Identity proof: a single oop sentinel constructs animal_w AND dog_w;
//       both round-trip the same get_instance() (proves the type-agnostic
//       decode contract — "same slot, two wrapper types, same oop").
//   (D) Null-OOP safety on every wrapper.
//   (E) Static_asserts on object_base / make_unique deleter identity for the
//       interface-typed wrapper.
//
// All checks are pure-logic: no JVM, no VM state, no oop is ever dereferenced.

#include <vmhook/vmhook.hpp>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <type_traits>
#include <utility>

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

// ---------------------------------------------------------------------------
// Wrappers mirroring the InterfacePoly fixture.  animal_w mirrors the
// Java interface; dog_w mirrors the concrete implementor; cat_w is an unrelated
// peer used to widen the SFINAE matrix.  Crucially, animal_w and dog_w are
// PEER C++ TYPES — neither inherits from the other — because the Java
// interface/class relationship is enforced by HotSpot, not by the C++ wrappers.
// ---------------------------------------------------------------------------
class animal_w : public vmhook::object<animal_w>
{
public:
    explicit animal_w(vmhook::oop_t oop) noexcept
        : vmhook::object<animal_w>{ oop }
    {
    }
};

class dog_w : public vmhook::object<dog_w>
{
public:
    explicit dog_w(vmhook::oop_t oop) noexcept
        : vmhook::object<dog_w>{ oop }
    {
    }
};

class cat_w : public vmhook::object<cat_w>
{
public:
    explicit cat_w(vmhook::oop_t oop) noexcept
        : vmhook::object<cat_w>{ oop }
    {
    }
};

// SFINAE-negative samples.  Neither qualifies as an interface-polymorphism
// wrapper.
struct not_a_wrapper_no_oop_ctor
{
    int x{ 0 };
};

class not_a_wrapper_no_object_base
{
public:
    explicit not_a_wrapper_no_object_base(vmhook::oop_t) noexcept {}
};

// ---------------------------------------------------------------------------
// (A) SFINAE detector for a valid interface-polymorphism wrapper.
//
// A type T qualifies iff it is:
//   - explicitly constructible from vmhook::oop_t (the decode path's
//     construction call in field_proxy::value_t::cast_for_variant);
//   - derived from vmhook::object_base (so the decoded wrapper has a valid
//     get_instance() and participates in the typeid registry);
//   - nothrow-move-constructible (object_base's move ctor is noexcept; any
//     valid wrapper inherits this contract).
// ---------------------------------------------------------------------------
template <typename T, typename = void>
struct is_interface_poly_wrapper : std::false_type {};

template <typename T>
struct is_interface_poly_wrapper<
    T,
    std::void_t<
        decltype(T{ std::declval<vmhook::oop_t>() }),
        std::enable_if_t<std::is_base_of_v<vmhook::object_base, T>>,
        std::enable_if_t<std::is_nothrow_move_constructible_v<T>>
    >
> : std::true_type {};

template <typename T>
constexpr bool is_interface_poly_wrapper_v = is_interface_poly_wrapper<T>::value;

// SFINAE POSITIVES — every Animal/Dog/Cat wrapper qualifies.
static_assert(is_interface_poly_wrapper_v<animal_w>);
static_assert(is_interface_poly_wrapper_v<dog_w>);
static_assert(is_interface_poly_wrapper_v<cat_w>);

// SFINAE NEGATIVES — neither degenerate sample qualifies.
static_assert(!is_interface_poly_wrapper_v<not_a_wrapper_no_oop_ctor>);
static_assert(!is_interface_poly_wrapper_v<not_a_wrapper_no_object_base>);
static_assert(!is_interface_poly_wrapper_v<int>);

// ---------------------------------------------------------------------------
// (B) Compile-time matrix: base vs derived.
//
// animal_w and dog_w are PEERS — neither is a C++ base of the other, even
// though Java-side Dog implements Animal.  This is the load-bearing contract:
// the field-decode path NEVER consults a C++ is_base_of relation; it just
// constructs the requested wrapper from whatever oop the slot points at.
// ---------------------------------------------------------------------------
static_assert(!std::is_base_of_v<animal_w, dog_w>);
static_assert(!std::is_base_of_v<dog_w,    animal_w>);
static_assert(!std::is_base_of_v<animal_w, cat_w>);
static_assert(!std::is_base_of_v<dog_w,    cat_w>);
// But ALL THREE share the same C++ flatten-point: vmhook::object_base.
static_assert(std::is_base_of_v<vmhook::object_base, animal_w>);
static_assert(std::is_base_of_v<vmhook::object_base, dog_w>);
static_assert(std::is_base_of_v<vmhook::object_base, cat_w>);
// They are NOT convertible to each other — even with the same layout — because
// the C++ type system enforces nominal typing.  An "Animal" oop read AS Dog
// has to be re-wrapped, not type-punned.
static_assert(!std::is_convertible_v<animal_w, dog_w>);
static_assert(!std::is_convertible_v<dog_w, animal_w>);
static_assert(!std::is_convertible_v<animal_w*, dog_w*>);
static_assert(!std::is_convertible_v<dog_w*, animal_w*>);
// And they ARE all convertible UP to object_base*, so the typeid registry can
// canonicalize them as object_base via the polymorphic upcast.
static_assert(std::is_convertible_v<animal_w*, vmhook::object_base*>);
static_assert(std::is_convertible_v<dog_w*,    vmhook::object_base*>);
static_assert(std::is_convertible_v<cat_w*,    vmhook::object_base*>);
// Same sizeof / same trivial layout (all three are thin CRTP veneers over the
// single instance pointer in object_base).
static_assert(sizeof(animal_w) == sizeof(vmhook::object_base));
static_assert(sizeof(dog_w)    == sizeof(vmhook::object_base));
static_assert(sizeof(cat_w)    == sizeof(vmhook::object_base));
static_assert(sizeof(animal_w) == sizeof(dog_w));

// (E) Static_asserts on object_base + make_unique deleter identity for an
// interface-typed wrapper.
static_assert(std::has_virtual_destructor_v<vmhook::object_base>);
static_assert(std::is_nothrow_constructible_v<animal_w, vmhook::oop_t>);
static_assert(std::is_nothrow_constructible_v<dog_w,    vmhook::oop_t>);
static_assert(std::is_nothrow_constructible_v<animal_w, std::nullptr_t>);
static_assert(std::is_nothrow_constructible_v<dog_w,    std::nullptr_t>);
static_assert(std::is_nothrow_move_constructible_v<animal_w>);
static_assert(std::is_nothrow_move_constructible_v<dog_w>);
static_assert(std::is_same_v<
    decltype(vmhook::make_unique<animal_w>()),
    std::unique_ptr<animal_w>>);
static_assert(std::is_same_v<
    decltype(vmhook::make_unique<dog_w>()),
    std::unique_ptr<dog_w>>);
static_assert(std::is_same_v<
    std::unique_ptr<animal_w>::deleter_type,
    std::default_delete<animal_w>>);
static_assert(std::is_same_v<
    std::unique_ptr<dog_w>::deleter_type,
    std::default_delete<dog_w>>);

// typeids of animal_w and dog_w must DIFFER, so the type_to_class_map keyed by
// std::type_index can resolve them to distinct klass registrations even though
// they share layout.
// (Runtime-only — typeid() is not a constexpr expression.)

int main()
{
    void* const sentinel{ reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xDEADD09)) };

    // =====================================================================
    // (C) Identity proof — the type-agnostic decode contract.  A single oop
    // sentinel constructs animal_w AND dog_w; both expose the SAME oop through
    // get_instance().  This is the no-JVM mirror of the fixture's
    // pet_animal->get_instance() == pet_dog->get_instance() assertion.
    // =====================================================================
    {
        animal_w a{ sentinel };
        dog_w    d{ sentinel };
        cat_w    c{ sentinel };
        check("interface_typed_wrapper_round_trips_oop",
              a.vmhook::object_base::get_instance() == sentinel);
        check("concrete_typed_wrapper_round_trips_oop",
              d.vmhook::object_base::get_instance() == sentinel);
        check("peer_typed_wrapper_round_trips_oop",
              c.vmhook::object_base::get_instance() == sentinel);

        // Same slot, two wrapper types, same oop.
        check("animal_and_dog_view_same_oop",
              a.vmhook::object_base::get_instance() ==
              d.vmhook::object_base::get_instance());
        // And cat_w (an unrelated peer) reads the same too — because the
        // wrapper does NOT validate the Java type; it just adopts the oop.
        check("animal_and_cat_view_same_oop",
              a.vmhook::object_base::get_instance() ==
              c.vmhook::object_base::get_instance());

        // Distinct C++ typeids — the typeid registry can tell them apart.
        check("animal_dog_typeid_differ",  typeid(a) != typeid(d));
        check("animal_cat_typeid_differ",  typeid(a) != typeid(c));
        check("dog_cat_typeid_differ",     typeid(d) != typeid(c));
    }

    // =====================================================================
    // Upcast both wrappers to object_base* — the typeid registry's canonical
    // form.  Both expose the SAME oop through the BASE accessor.
    // =====================================================================
    {
        animal_w a{ sentinel };
        dog_w    d{ sentinel };
        vmhook::object_base* const pa{ &a };
        vmhook::object_base* const pd{ &d };
        check("animal_upcast_to_object_base_ok",
              pa != nullptr && pa->get_instance() == sentinel);
        check("concrete_upcast_to_object_base_ok",
              pd != nullptr && pd->get_instance() == sentinel);
        check("both_upcasts_agree_on_oop",
              pa->get_instance() == pd->get_instance());
    }

    // =====================================================================
    // (D) Null-OOP safety on every wrapper — the wrapped-null state the
    // field-decode path produces when the slot's oop is null.
    // =====================================================================
    {
        animal_w a{ nullptr };
        dog_w    d{ nullptr };
        cat_w    c{ nullptr };
        check("null_oop_animal_wrapper_is_null",
              a.vmhook::object_base::get_instance() == nullptr);
        check("null_oop_dog_wrapper_is_null",
              d.vmhook::object_base::get_instance() == nullptr);
        check("null_oop_cat_wrapper_is_null",
              c.vmhook::object_base::get_instance() == nullptr);
    }

    // =====================================================================
    // Move semantics survive on the interface-typed wrapper.  The animal_w
    // move-ctor must transfer the oop AND null the source — the documented
    // object_base value-semantics contract.
    // =====================================================================
    {
        animal_w src{ sentinel };
        animal_w dst{ std::move(src) };
        check("animal_move_ctor_transfers_oop",
              dst.vmhook::object_base::get_instance() == sentinel);
        check("animal_move_ctor_nulls_source",
              src.vmhook::object_base::get_instance() == nullptr);
    }
    {
        // Re-wrap an Animal-decoded oop as Dog.  Mirrors the runtime path of
        // "the slot is declared Animal but the runtime oop is a Dog; ask for
        // it AS Dog now".
        animal_w a{ sentinel };
        const auto raw_oop{ a.vmhook::object_base::get_instance() };
        dog_w d{ raw_oop };
        check("rewrap_animal_oop_as_dog_preserves_oop",
              d.vmhook::object_base::get_instance() == sentinel);
    }

    // =====================================================================
    // unique_ptr<animal_w> — the type the decode-as-Animal path returns.
    // Default deleter, holds the oop, releases through the virtual object_base
    // dtor on scope exit.
    // =====================================================================
    {
        std::unique_ptr<animal_w> u{ new animal_w{ sentinel } };
        check("unique_ptr_animal_holds_oop",
              u && u->vmhook::object_base::get_instance() == sentinel);
        animal_w* const raw{ u.release() };
        check("unique_ptr_animal_release_yields_same_oop",
              raw != nullptr && raw->vmhook::object_base::get_instance() == sentinel);
        std::unique_ptr<animal_w> readopted{ raw };
        check("unique_ptr_animal_readopt_holds_oop",
              readopted && readopted->vmhook::object_base::get_instance() == sentinel);
    }

    std::printf("%s: %d failure(s)\n", failures == 0 ? "OK" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
