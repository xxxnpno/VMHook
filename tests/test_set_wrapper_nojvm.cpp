// No-JVM cold-state coverage for the vmhook::set wrapper specifically.
//
// vmhook::set is a thin type-tag over vmhook::collection.  Without a JVM
// loaded, every accessor must produce the documented safe-default: size()==0,
// is_empty()==true, to_vector<E>() returns an empty std::vector<unique_ptr<E>>,
// none of them throw, and null/invalid array_oop reachable via the cascade
// short-circuits to empty.  This file pins those guarantees in BOTH runtime
// and compile-time form (static_asserts on return types and noexcept).
//
// What is NOT here: a `set::contains(E)` accessor.  The ledger gap names it,
// but the wrapper does not currently expose one (Set contents are reached
// through to_vector and walked by the caller).  The closest cold-state stand-in
// is documented and exercised below: to_vector on a null/invalid set is empty,
// so any future contains() implemented on top would return false.  Pinned with
// an [INFO]-style record line rather than a HARD assert against absent surface.

#include <vmhook/vmhook.hpp>

#include <cstdio>
#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}
static auto info(const char* name, const char* msg) -> void
{
    std::printf("[INFO] %s :: %s\n", name, msg);
}

// Minimal oop-constructible element wrapper for to_vector<E>.
class set_elem : public vmhook::object<set_elem>
{
public:
    explicit set_elem(vmhook::oop_t oop) noexcept
        : vmhook::object<set_elem>{ oop }
    {
    }
};

using value_t = vmhook::field_proxy::value_t;

// ---------------------------------------------------------------------------
// COMPILE-TIME contract: static_asserts on the set wrapper's accessor shape.
// ---------------------------------------------------------------------------

// Identity / lattice.
static_assert(!std::is_same_v<vmhook::set, vmhook::collection>,
              "set must be a distinct type from collection");
static_assert(std::is_base_of_v<vmhook::collection, vmhook::set>,
              "set must derive from collection");
static_assert(std::is_base_of_v<vmhook::oop_reflective_base, vmhook::set>,
              "set must derive from the shared mixin");
static_assert(std::is_base_of_v<vmhook::object_base, vmhook::set>,
              "set must derive from object_base");

// Constructor surface — explicit, oop_t, noexcept, no default ctor.
static_assert(std::is_constructible_v<vmhook::set, vmhook::oop_t>,
              "set must be constructible from oop_t");
static_assert(!std::is_convertible_v<vmhook::oop_t, vmhook::set>,
              "set(oop_t) must be explicit");
static_assert(!std::is_default_constructible_v<vmhook::set>,
              "set must NOT be default-constructible");
static_assert(std::is_nothrow_constructible_v<vmhook::set, vmhook::oop_t>,
              "set(oop_t) must be noexcept");

// Copy / move: required for storage in std::vector, must be noexcept.
static_assert(std::is_nothrow_copy_constructible_v<vmhook::set>,
              "set copy ctor must be noexcept");
static_assert(std::is_nothrow_move_constructible_v<vmhook::set>,
              "set move ctor must be noexcept");

// Type-tagging only — no fattening over object_base.
static_assert(sizeof(vmhook::set) == sizeof(vmhook::object_base),
              "set must not add data members on top of object_base");

// Accessor return-type / noexcept pins on the inherited cold-state surface.
static_assert(std::is_same_v<decltype(std::declval<const vmhook::set&>().size()),
                             std::int32_t>,
              "set::size() must return std::int32_t");
static_assert(std::is_same_v<decltype(std::declval<const vmhook::set&>().is_empty()),
                             bool>,
              "set::is_empty() must return bool");
static_assert(noexcept(std::declval<const vmhook::set&>().size()),
              "set::size() must be noexcept");
static_assert(noexcept(std::declval<const vmhook::set&>().is_empty()),
              "set::is_empty() must be noexcept");
static_assert(noexcept(std::declval<const vmhook::set&>().get_instance()),
              "set::get_instance() must be noexcept");

// to_vector<E>() return type pin.
static_assert(std::is_same_v<decltype(std::declval<const vmhook::set&>().to_vector<set_elem>()),
                             std::vector<std::unique_ptr<set_elem>>>,
              "set::to_vector<E>() must return std::vector<unique_ptr<E>>");

// value_t::to_vector<E>() — same return-type contract for the user path.
static_assert(std::is_same_v<decltype(std::declval<const value_t&>().to_vector<set_elem>()),
                             std::vector<std::unique_ptr<set_elem>>>,
              "value_t::to_vector<E>() must return std::vector<unique_ptr<E>>");

// ---------------------------------------------------------------------------
// 1. Null-OOP cold-state: every accessor safe-defaults.
// ---------------------------------------------------------------------------
static auto test_null_oop_cold_state() -> void
{
    vmhook::set s{ nullptr };

    check("null_set_get_instance_nullptr", s.get_instance() == nullptr);
    check("null_set_size_zero",            s.size() == 0);
    check("null_set_is_empty_true",        s.is_empty());

    // is_empty must mirror size()==0 exactly (no independent sentinel).
    check("null_set_is_empty_mirrors_size", s.is_empty() == (s.size() == 0));

    // size() returns EXACTLY 0, not a negative sentinel.
    check("null_set_size_not_negative", !(s.size() < 0));

    // to_vector returns an empty vector, no throw.
    const auto v{ s.to_vector<set_elem>() };
    check("null_set_to_vector_empty", v.empty());
    check("null_set_to_vector_size_zero", v.size() == 0);

    // noexcept holds for the actual call expressions (not just decltype).
    check("null_set_size_noexcept",    noexcept(s.size()));
    check("null_set_is_empty_noexcept",noexcept(s.is_empty()));
}

// ---------------------------------------------------------------------------
// 2. Bogus (non-null, invalid) OOP: rejected by is_valid_pointer -> safe.
//
// The cascade in collection::to_vector probes get_field_by_oop_klass on the
// instance.  With no JVM, klass_from_oop returns nullptr regardless, so every
// probe yields nullopt and to_vector returns empty without any array_oop
// decode.  Even the lowest-address values that would resemble a compressed
// element pointer in the array fast path are rejected up-front.
// ---------------------------------------------------------------------------
static auto test_bogus_oop_cold_state() -> void
{
    // 0x4 is below user_address_floor on every supported platform.
    vmhook::set bogus_low{
        reinterpret_cast<vmhook::oop_t>(static_cast<std::uintptr_t>(0x4)) };
    check("bogus_low_set_size_zero",        bogus_low.size() == 0);
    check("bogus_low_set_is_empty_true",    bogus_low.is_empty());
    check("bogus_low_set_to_vector_empty",  bogus_low.to_vector<set_elem>().empty());

    // Odd / sentinel-style pointer.
    vmhook::set bogus_odd{
        reinterpret_cast<vmhook::oop_t>(static_cast<std::uintptr_t>(0x1)) };
    check("bogus_odd_set_size_zero",        bogus_odd.size() == 0);
    check("bogus_odd_set_to_vector_empty",  bogus_odd.to_vector<set_elem>().empty());

    // High pseudo-address: also rejected (no live klass to reflect against).
    vmhook::set bogus_hi{
        reinterpret_cast<vmhook::oop_t>(static_cast<std::uintptr_t>(0xdead'beefu)) };
    check("bogus_hi_set_size_zero",         bogus_hi.size() == 0);
    check("bogus_hi_set_to_vector_empty",   bogus_hi.to_vector<set_elem>().empty());
}

// ---------------------------------------------------------------------------
// 3. Null-array_oop branch via value_t with an array-shaped signature.
//
// value_t::to_vector<E>() has a fast-path for an object-array signature
// ("[L...;" / "[[..."); when the stored compressed OOP decodes to nullptr,
// the array walk short-circuits to an empty vector.  Pin the null-array path
// for several array signatures.
// ---------------------------------------------------------------------------
static auto test_null_array_oop_via_value_t() -> void
{
    struct sig_case
    {
        const char* name;
        const char* signature;
    };
    const sig_case cases[]{
        { "set_arr_object",    "[Ljava/lang/Object;" },
        { "set_arr_string",    "[Ljava/lang/String;" },
        { "set_arr_set",       "[Ljava/util/Set;" },
        { "set_arr_2d_object", "[[Ljava/lang/Object;" },
    };

    for (const auto& tc : cases)
    {
        // Stored compressed OOP 0 -> decode_oop_pointer -> nullptr -> empty.
        value_t v_zero{ std::uint32_t{ 0u }, std::string{ tc.signature } };
        std::string n0{ "null_array_oop_zero_" };
        n0 += tc.name;
        check(n0.c_str(), v_zero.to_vector<set_elem>().empty());

        // A non-zero but invalid compressed OOP also decodes to nullptr w/o JVM.
        value_t v_bogus{ std::uint32_t{ 0x7u }, std::string{ tc.signature } };
        std::string n1{ "null_array_oop_bogus_" };
        n1 += tc.name;
        check(n1.c_str(), v_bogus.to_vector<set_elem>().empty());
    }
}

// ---------------------------------------------------------------------------
// 4. value_t reached via field_proxy::get() with a Set-typed signature.
//
// Exact user path: get_field("setOfFoo")->get().to_vector<Foo>().  A field_proxy
// over a null field_pointer reads no memory, so the value_t carries no live
// OOP and to_vector returns empty.
// ---------------------------------------------------------------------------
static auto test_field_proxy_set_path_cold() -> void
{
    vmhook::field_proxy set_field{ nullptr, "Ljava/util/Set;", false };

    const auto vec{ set_field.get().to_vector<set_elem>() };
    check("proxy_set_to_vector_empty", vec.empty());
    check("proxy_set_to_vector_size0", vec.size() == 0);

    // Static-field flavor: same safe default.
    vmhook::field_proxy set_field_static{ nullptr, "Ljava/util/Set;", true };
    check("proxy_static_set_to_vector_empty",
          set_field_static.get().to_vector<set_elem>().empty());

    // Array-of-Set field: takes the array branch, null backing oop -> empty.
    vmhook::field_proxy set_array_field{ nullptr, "[Ljava/util/Set;", false };
    check("proxy_set_array_to_vector_empty",
          set_array_field.get().to_vector<set_elem>().empty());
}

// ---------------------------------------------------------------------------
// 5. Copy / move of a set wrapper preserves the cold-state contract.
// ---------------------------------------------------------------------------
static auto test_copy_move_preserves_cold_state() -> void
{
    vmhook::set s{ nullptr };

    vmhook::set s_copy{ s };
    check("set_copy_null_instance",        s_copy.get_instance() == nullptr);
    check("set_copy_size_zero",            s_copy.size() == 0);
    check("set_copy_is_empty_true",        s_copy.is_empty());
    check("set_copy_to_vector_empty",      s_copy.to_vector<set_elem>().empty());

    vmhook::set s_src{ nullptr };
    vmhook::set s_dst{ std::move(s_src) };
    check("set_move_dst_null",             s_dst.get_instance() == nullptr);
    check("set_move_dst_size_zero",        s_dst.size() == 0);
    check("set_move_dst_to_vector_empty",  s_dst.to_vector<set_elem>().empty());

    // Moved-from src must still be safe — nulled by object_base's move ctor.
    check("set_move_src_null",             s_src.get_instance() == nullptr); // NOLINT(bugprone-use-after-move)
    check("set_move_src_size_zero",        s_src.size() == 0);               // NOLINT(bugprone-use-after-move)
    check("set_move_src_to_vector_empty",  s_src.to_vector<set_elem>().empty()); // NOLINT(bugprone-use-after-move)
}

// ---------------------------------------------------------------------------
// 6. Substitutability via base reference: a set binds to const collection&.
// ---------------------------------------------------------------------------
static auto take_collection_ref(const vmhook::collection& c) -> std::size_t
{
    return c.to_vector<set_elem>().size() + static_cast<std::size_t>(c.size());
}

static auto test_set_binds_collection_ref() -> void
{
    vmhook::set s{ nullptr };
    check("set_binds_collection_ref_returns_zero", take_collection_ref(s) == 0);

    // Compile-time guarantee.
    static_assert(std::is_convertible_v<vmhook::set&, vmhook::collection&>,
                  "set& must be convertible to collection&");
    static_assert(!std::is_convertible_v<vmhook::set&, vmhook::map&>,
                  "set& must NOT cross over to map&");
}

// ---------------------------------------------------------------------------
// 7. Documented absence of set::contains() — recorded, not asserted.
// ---------------------------------------------------------------------------
static auto test_contains_absent_note() -> void
{
    // No has-member trait probe (would be a hard compile error if asserted
    // wrong-way), just an info line so the wave-31 ledger reflects reality.
    info("set_contains_accessor",
         "vmhook::set exposes no contains(); cold-state stand-in is "
         "to_vector().empty() == true (asserted above)");
}

auto main() -> int
{
    test_null_oop_cold_state();
    test_bogus_oop_cold_state();
    test_null_array_oop_via_value_t();
    test_field_proxy_set_path_cold();
    test_copy_move_preserves_cold_state();
    test_set_binds_collection_ref();
    test_contains_absent_note();

    std::printf("\nfailures=%d\n", failures);
    return failures == 0 ? 0 : 1;
}
