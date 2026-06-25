// No-JVM deepening for the LinkedList fast-path wrapper + the free function
// vmhook::linked_list_walk_items<T>(list_oop, size, out).
//
// Wave-31 LEDGER gap closing:
//   * cold-state linked_list wrapper noexcept + safe-default empty
//   * null head safe (null + invalid list_oop -> no walk, untouched out_t)
//   * size cap CONSTANT static_assert (k_max_safe_container_elems / clamp)
//   * static_asserts on accessor signatures (collection::to_vector return type,
//     linked_list_walk_items void-return, noexcept ctor, size_t element width)
//
// With no JVM loaded:
//   * decode_oop_pointer() returns nullptr for every compressed value.
//   * is_valid_pointer() rejects every small / sentinel address.
//   * klass_from_oop() short-circuits on a null/invalid instance.
// So every walk into linked_list_walk_items / linked_list::to_vector returns
// an empty std::vector<std::unique_ptr<elem>> WITHOUT throwing or dereferencing
// anything — the documented safe-default and the same property the JVM-loaded
// path relies on at its bail-out edges.
#include <vmhook/vmhook.hpp>

#include <cstdio>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{
    int failures{ 0 };

    auto check(const char* name, bool ok) -> void
    {
        std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
        if (!ok)
        {
            ++failures;
        }
    }

    // Smallest oop-constructible element wrapper -- std::make_unique<elem_w>(oop)
    // is what linked_list_walk_items / collection::to_vector synthesise.
    class elem_w : public vmhook::object<elem_w>
    {
    public:
        explicit elem_w(vmhook::oop_t oop) noexcept
            : vmhook::object<elem_w>{ oop }
        {
        }
    };

    // ---- compile-time accessor-signature pins (static_asserts) --------------
    // These fire at TU compile-time so a regression in the public surface is
    // caught even when the runtime body short-circuits.

    static_assert(std::is_base_of_v<vmhook::list, vmhook::linked_list>,
                  "linked_list must derive from list (collection fast-path dispatch).");
    static_assert(std::is_base_of_v<vmhook::collection, vmhook::linked_list>,
                  "linked_list must derive transitively from collection.");
    static_assert(std::is_constructible_v<vmhook::linked_list, vmhook::oop_t>,
                  "linked_list is built from a decoded OOP.");
    static_assert(std::is_nothrow_constructible_v<vmhook::linked_list, vmhook::oop_t>,
                  "linked_list(oop) is noexcept -- detour-safe.");
    static_assert(!std::is_convertible_v<vmhook::oop_t, vmhook::linked_list>,
                  "linked_list(oop) is EXPLICIT; oop_t must not implicitly become a wrapper.");
    static_assert(!std::is_default_constructible_v<vmhook::linked_list>,
                  "linked_list has no oop-less default ctor.");
    static_assert(std::is_nothrow_copy_constructible_v<vmhook::linked_list>,
                  "linked_list copy is noexcept (inherited).");
    static_assert(std::is_nothrow_move_constructible_v<vmhook::linked_list>,
                  "linked_list move is noexcept (inherited).");
    static_assert(sizeof(vmhook::linked_list) == sizeof(vmhook::object_base),
                  "linked_list adds no data members -- pure type tag.");

    // collection::to_vector<elem_w>() returns std::vector<std::unique_ptr<elem_w>>.
    using to_vec_t = decltype(std::declval<const vmhook::linked_list&>().to_vector<elem_w>());
    static_assert(std::is_same_v<to_vec_t, std::vector<std::unique_ptr<elem_w>>>,
                  "linked_list::to_vector<T> must return vector<unique_ptr<T>>.");

    // Same vector type as `list` and `collection` -- the wrapper adds NO override.
    static_assert(std::is_same_v<
                      decltype(std::declval<const vmhook::collection&>().to_vector<elem_w>()),
                      decltype(std::declval<const vmhook::linked_list&>().to_vector<elem_w>())>,
                  "linked_list inherits to_vector unchanged from collection.");

    // size() / is_empty() inherited contract.
    static_assert(std::is_same_v<decltype(std::declval<const vmhook::linked_list&>().size()),
                                 std::int32_t>,
                  "linked_list::size() returns int32_t (signed -- caller relies on >=0).");
    static_assert(std::is_same_v<decltype(std::declval<const vmhook::linked_list&>().is_empty()),
                                 bool>,
                  "linked_list::is_empty() returns bool.");
    static_assert(noexcept(std::declval<const vmhook::linked_list&>().size()),
                  "linked_list::size() is noexcept.");
    static_assert(noexcept(std::declval<const vmhook::linked_list&>().is_empty()),
                  "linked_list::is_empty() is noexcept.");

    // ---- size-cap CONSTANT pins --------------------------------------------
    // The cap is exposed as vmhook::k_max_safe_container_elems and consumed by
    // every oop-derived count -- in particular by linked_list_walk_items via
    // clamp_safe_container_count(size).  Pin the EXACT value so a regression that
    // raised/lowered/changed the type is caught at compile time.
    static_assert(std::is_same_v<decltype(vmhook::k_max_safe_container_elems), const std::size_t>,
                  "k_max_safe_container_elems must be a std::size_t constant.");
    static_assert(vmhook::k_max_safe_container_elems == (1ull << 24),
                  "k_max_safe_container_elems is the documented 16M cap.");
    static_assert(vmhook::k_max_safe_container_elems
                      <= static_cast<std::size_t>((std::numeric_limits<std::int32_t>::max)()),
                  "cap must fit int32_t so clamp_safe_container_count cannot overflow.");

    // clamp_safe_container_count is a constexpr accessor with a fixed contract.
    static_assert(vmhook::clamp_safe_container_count(0) == 0,
                  "clamp(0) == 0 (empty -> empty).");
    static_assert(vmhook::clamp_safe_container_count(-1) == 0,
                  "clamp(<0) == 0 (size field flipped negative -> no walk).");
    static_assert(vmhook::clamp_safe_container_count(
                      (std::numeric_limits<std::int32_t>::min)())
                      == 0,
                  "clamp(INT32_MIN) == 0 (no UB on the negative branch).");
    static_assert(vmhook::clamp_safe_container_count(1) == 1,
                  "clamp(1) == 1 (honest small list unchanged).");
    static_assert(vmhook::clamp_safe_container_count(3) == 3,
                  "clamp(3) == 3 (the LinkedListProbe fixture size).");
    static_assert(vmhook::clamp_safe_container_count(
                      static_cast<std::int32_t>(vmhook::k_max_safe_container_elems - 1))
                      == static_cast<std::int32_t>(vmhook::k_max_safe_container_elems - 1),
                  "clamp(cap-1) is unchanged -- the cap is exclusive on the unchanged branch.");
    static_assert(vmhook::clamp_safe_container_count(
                      static_cast<std::int32_t>(vmhook::k_max_safe_container_elems))
                      == static_cast<std::int32_t>(vmhook::k_max_safe_container_elems),
                  "clamp(cap) == cap (the boundary value is itself the saturation result).");
    static_assert(vmhook::clamp_safe_container_count(
                      (std::numeric_limits<std::int32_t>::max)())
                      == static_cast<std::int32_t>(vmhook::k_max_safe_container_elems),
                  "clamp(INT32_MAX) saturates at the cap -- corrupt huge size cannot reserve TB.");

    // The clamp is noexcept (it is called inside a noexcept walk path).
    static_assert(noexcept(vmhook::clamp_safe_container_count(0)),
                  "clamp_safe_container_count is noexcept.");

    // ---- linked_list_walk_items signature pins -----------------------------
    // The free function the cascade dispatches to.  Pin the return type and the
    // parameter types so a refactor cannot widen size to size_t (signedness
    // mismatch with size() return) or drop the out_t reference parameter.
    using out_vec_t = std::vector<std::unique_ptr<elem_w>>;
    using walk_fn_t = void (*)(void*, std::int32_t, out_vec_t&);
    static_assert(std::is_same_v<
                      walk_fn_t,
                      decltype(&vmhook::linked_list_walk_items<elem_w, out_vec_t>)>,
                  "linked_list_walk_items<T, out_t>: void(void*, int32_t, out_t&).");

    // ---- runtime checks ----------------------------------------------------

    auto test_cold_default_construction() -> void
    {
        // Cold state: no JVM, null head.  The wrapper must be inert.
        vmhook::linked_list ll{ nullptr };
        check("ll_null_instance", ll.get_instance() == nullptr);
        check("ll_size_zero",     ll.size() == 0);
        check("ll_is_empty_true", ll.is_empty());
        check("ll_size_not_negative", !(ll.size() < 0));

        // Repeat (the wrapper is stateless -- adjacent calls agree).
        check("ll_size_idempotent",     ll.size() == ll.size());
        check("ll_is_empty_idempotent", ll.is_empty() == ll.is_empty());
    }

    auto test_to_vector_safe_default_empty() -> void
    {
        vmhook::linked_list ll{ nullptr };

        const auto v0{ ll.to_vector<elem_w>() };
        check("ll_to_vector_empty",  v0.empty());
        check("ll_to_vector_size_0", v0.size() == 0);

        // Run several times -- never throws, always empty.
        for (int i = 0; i < 8; ++i)
        {
            const auto v{ ll.to_vector<elem_w>() };
            std::string name{ "ll_to_vector_repeat_empty_" };
            name += std::to_string(i);
            check(name.c_str(), v.empty());
        }

        // Bogus non-null (below user_address_floor on every platform) -> empty.
        const auto bogus_oops = {
            static_cast<std::uintptr_t>(0x1u),
            static_cast<std::uintptr_t>(0x4u),
            static_cast<std::uintptr_t>(0x7u),
            static_cast<std::uintptr_t>(0x10u),
            static_cast<std::uintptr_t>(0xFFFu),
        };
        std::size_t idx{ 0 };
        for (auto raw : bogus_oops)
        {
            vmhook::linked_list bogus{ reinterpret_cast<vmhook::oop_t>(raw) };
            std::string name{ "ll_bogus_oop_to_vector_empty_" };
            name += std::to_string(idx++);
            check(name.c_str(), bogus.to_vector<elem_w>().empty());
            // size()/is_empty() never fault on a bogus oop either.
            check((std::string{ "ll_bogus_size_zero_" } + std::to_string(idx - 1)).c_str(),
                  bogus.size() == 0);
            check((std::string{ "ll_bogus_is_empty_true_" } + std::to_string(idx - 1)).c_str(),
                  bogus.is_empty());
        }
    }

    auto test_walk_items_null_head_safe() -> void
    {
        // Direct free-function entry: every guard must short-circuit, leaving
        // out_vec untouched.  Use a NON-EMPTY sentinel out_vec so we can prove
        // the function did not clear it / replace it / write garbage.
        std::vector<std::unique_ptr<elem_w>> out;
        out.push_back(std::make_unique<elem_w>(reinterpret_cast<vmhook::oop_t>(
            static_cast<std::uintptr_t>(0xC001u))));
        const std::size_t before{ out.size() };

        vmhook::linked_list_walk_items<elem_w>(nullptr, 3, out);
        check("walk_null_oop_untouched_size", out.size() == before);

        vmhook::linked_list_walk_items<elem_w>(nullptr, 0, out);
        check("walk_null_oop_zero_size_untouched", out.size() == before);

        vmhook::linked_list_walk_items<elem_w>(nullptr, -1, out);
        check("walk_null_oop_neg_size_untouched", out.size() == before);

        // Bogus (non-null but invalid) list_oop with any size -> guarded out.
        void* const bogus{ reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x4u)) };
        vmhook::linked_list_walk_items<elem_w>(bogus, 3, out);
        check("walk_bogus_oop_untouched_size", out.size() == before);

        vmhook::linked_list_walk_items<elem_w>(bogus, 0, out);
        check("walk_bogus_oop_zero_size_untouched", out.size() == before);

        // Even with an absurdly huge size (>cap), a null/invalid head must not
        // walk OR reserve.  We cannot directly observe reserve in vector, but
        // we can observe that size() did not change and no fault occurred.
        vmhook::linked_list_walk_items<elem_w>(
            nullptr, (std::numeric_limits<std::int32_t>::max)(), out);
        check("walk_null_oop_int32max_size_untouched", out.size() == before);
        vmhook::linked_list_walk_items<elem_w>(
            bogus, (std::numeric_limits<std::int32_t>::max)(), out);
        check("walk_bogus_oop_int32max_size_untouched", out.size() == before);
    }

    auto test_walk_items_into_fresh_empty() -> void
    {
        // The documented "drives out_t" contract: a freshly-empty out remains
        // empty after a null/invalid walk -- safe default.
        std::vector<std::unique_ptr<elem_w>> out;
        vmhook::linked_list_walk_items<elem_w>(nullptr, 3, out);
        check("walk_fresh_null_remains_empty", out.empty());

        vmhook::linked_list_walk_items<elem_w>(
            reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x7u)), 100, out);
        check("walk_fresh_bogus_remains_empty", out.empty());
    }

    auto test_wrapper_copy_move_preserves_empty() -> void
    {
        vmhook::linked_list ll0{ nullptr };
        vmhook::linked_list copy{ ll0 };
        check("ll_copy_null_instance", copy.get_instance() == nullptr);
        check("ll_copy_to_vector_empty", copy.to_vector<elem_w>().empty());
        check("ll_copy_size_zero", copy.size() == 0);

        vmhook::linked_list moved_src{ nullptr };
        vmhook::linked_list moved_dst{ std::move(moved_src) };
        check("ll_moved_dst_null_instance", moved_dst.get_instance() == nullptr);
        check("ll_moved_dst_to_vector_empty", moved_dst.to_vector<elem_w>().empty());
        // Moved-from is also safe (object_base null-out semantics).
        check("ll_moved_src_to_vector_empty",
              moved_src.to_vector<elem_w>().empty());  // NOLINT(bugprone-use-after-move)
    }

    auto test_collection_to_vector_noexcept_traits() -> void
    {
        // The to_vector body allocates inside std::make_unique -- it is NOT
        // marked noexcept (and cannot be, since allocation can throw bad_alloc).
        // Pin THIS fact so a refactor that mis-marks it noexcept (which would
        // silently terminate on OOM inside a detour) is caught.
        vmhook::linked_list ll{ nullptr };
        check("ll_to_vector_is_NOT_noexcept",
              !noexcept(ll.to_vector<elem_w>()));

        // But on the null path it never actually allocates -- the contract is
        // "empty on failure", and the wrapper construction itself IS noexcept.
        check("ll_ctor_is_noexcept", noexcept(vmhook::linked_list{ nullptr }));
        check("ll_size_is_noexcept", noexcept(ll.size()));
        check("ll_is_empty_is_noexcept", noexcept(ll.is_empty()));
    }
}  // namespace

auto main() -> int
{
    test_cold_default_construction();
    test_to_vector_safe_default_empty();
    test_walk_items_null_head_safe();
    test_walk_items_into_fresh_empty();
    test_wrapper_copy_move_preserves_empty();
    test_collection_to_vector_noexcept_traits();

    std::printf("\n%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
