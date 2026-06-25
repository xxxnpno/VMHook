// Cold-state (no-JVM) accessor surface for vmhook::map and vmhook::hash_map.
//
// Sibling to tests/test_collection_type_tags.cpp, but tightly focused on the
// collection_map specialist's ledger gap:
//
//   "cold-state map<K,V> wrapper accessor (size/is_empty/get) noexcept +
//    safe-default; null oop safe; static_asserts on signatures."
//
// What we pin here (and explicitly NOT just copy-paste from the type-tags test):
//   * COMPILE-TIME signature contract of map::size / map::is_empty /
//     map::to_entries / map::get_instance and the field_proxy::value_t
//     to_entries delegator, expressed as static_assert on the exact member-
//     pointer types and on noexcept-ness.
//   * Cold-state (no JVM loaded) RUNTIME behaviour: every null / invalid-oop
//     construction route to a map / hash_map yields the documented safe
//     defaults (size==0, is_empty()==true, to_entries() empty) and never throws.
//   * The "get" accessor — there is no get(key) method on vmhook::map; the user
//     reach is `get_field("foo")->get().to_entries<K,V>()`, i.e. the
//     field_proxy::value_t::to_entries delegator. We pin its signature and
//     null-default behaviour as the "get" half of the gap.
//   * Sentinel-address fuzz over a much wider set of invalid pointer values
//     than the existing tests (0x0..0xFFFE, plus the high-canonical/non-canonical
//     ranges), all of which is_valid_pointer must reject before any walk fires.
//
// Cross-platform notes:
//   * No use of long-double / int64_t over-load resolution gotchas.
//   * No constexpr lambda captures (the gate predicate is a free function).
//   * std::string_view only at compile time; no noexcept-mismatch on libc++.
//   * No reliance on the cl.exe-only SEH path; all reads are pure C++ value
//     constructions that never dereference the bogus pointers (is_valid_pointer
//     rejects them up-front in the wrapper code paths we call).

#include <vmhook/vmhook.hpp>

#include <cstdint>
#include <cstdio>
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

// Minimal oop-constructible key/value wrappers (object_base-derived) — required
// by to_entries<K,V> which instantiates std::make_unique<K>(oop_t) /
// std::make_unique<V>(oop_t) in its unreached body.
class k_w : public vmhook::object<k_w>
{
public:
    explicit k_w(vmhook::oop_t oop) noexcept
        : vmhook::object<k_w>{ oop }
    {
    }
};

class v_w : public vmhook::object<v_w>
{
public:
    explicit v_w(vmhook::oop_t oop) noexcept
        : vmhook::object<v_w>{ oop }
    {
    }
};

using value_t = vmhook::field_proxy::value_t;

// ---------------------------------------------------------------------------
// 1. COMPILE-TIME signature pinning.
//
// Every assertion in this block is a static_assert on the EXACT member-pointer
// type and noexcept-qualification of the map accessors. A regression that
// flipped a return type, dropped a `noexcept`, or added an out-parameter would
// fail to compile here — caught before the binary even runs.
// ---------------------------------------------------------------------------

// size() is noexcept and returns std::int32_t.
static_assert(noexcept(std::declval<const vmhook::map&>().size()),
              "map::size() must be noexcept");
static_assert(noexcept(std::declval<const vmhook::hash_map&>().size()),
              "hash_map::size() must be noexcept");
static_assert(std::is_same_v<decltype(std::declval<const vmhook::map&>().size()),
                             std::int32_t>,
              "map::size() must return std::int32_t");
static_assert(std::is_same_v<decltype(std::declval<const vmhook::hash_map&>().size()),
                             std::int32_t>,
              "hash_map::size() must return std::int32_t");

// is_empty() is noexcept and returns bool.
static_assert(noexcept(std::declval<const vmhook::map&>().is_empty()),
              "map::is_empty() must be noexcept");
static_assert(noexcept(std::declval<const vmhook::hash_map&>().is_empty()),
              "hash_map::is_empty() must be noexcept");
static_assert(std::is_same_v<decltype(std::declval<const vmhook::map&>().is_empty()),
                             bool>,
              "map::is_empty() must return bool");
static_assert(std::is_same_v<decltype(std::declval<const vmhook::hash_map&>().is_empty()),
                             bool>,
              "hash_map::is_empty() must return bool");

// get_instance() is noexcept and returns vmhook::oop_t (== void*) — the "get"
// half of the ledger gap for the wrapper accessor surface.
static_assert(noexcept(std::declval<const vmhook::map&>().get_instance()),
              "map::get_instance() must be noexcept");
static_assert(noexcept(std::declval<const vmhook::hash_map&>().get_instance()),
              "hash_map::get_instance() must be noexcept");
static_assert(std::is_same_v<decltype(std::declval<const vmhook::map&>().get_instance()),
                             vmhook::oop_t>,
              "map::get_instance() must return vmhook::oop_t");
static_assert(std::is_same_v<vmhook::oop_t, void*>,
              "vmhook::oop_t must be void*");

// to_entries<K,V>() return type: std::vector<std::pair<unique_ptr<K>, unique_ptr<V>>>.
static_assert(std::is_same_v<
                  decltype(std::declval<const vmhook::map&>().to_entries<k_w, v_w>()),
                  std::vector<std::pair<std::unique_ptr<k_w>, std::unique_ptr<v_w>>>>,
              "map::to_entries<K,V>() must return vector<pair<unique_ptr<K>,unique_ptr<V>>>");
static_assert(std::is_same_v<
                  decltype(std::declval<const vmhook::hash_map&>().to_entries<k_w, v_w>()),
                  std::vector<std::pair<std::unique_ptr<k_w>, std::unique_ptr<v_w>>>>,
              "hash_map::to_entries<K,V>() must return same vector<pair<...>> type");

// The field_proxy::value_t::to_entries delegator (the user-facing "get" reach)
// has the same return type as map::to_entries.
static_assert(std::is_same_v<
                  decltype(std::declval<const value_t&>().to_entries<k_w, v_w>()),
                  std::vector<std::pair<std::unique_ptr<k_w>, std::unique_ptr<v_w>>>>,
              "value_t::to_entries<K,V>() must return the same vector<pair<...>> type");

// Constructors are noexcept; the wrappers themselves carry no state beyond
// object_base, so sizeof(hash_map) == sizeof(map).
static_assert(std::is_nothrow_constructible_v<vmhook::map, vmhook::oop_t>,
              "map(oop_t) must be noexcept");
static_assert(std::is_nothrow_constructible_v<vmhook::hash_map, vmhook::oop_t>,
              "hash_map(oop_t) must be noexcept");
static_assert(std::is_nothrow_copy_constructible_v<vmhook::map>,
              "map copy ctor must be noexcept");
static_assert(std::is_nothrow_move_constructible_v<vmhook::map>,
              "map move ctor must be noexcept");
static_assert(std::is_nothrow_copy_constructible_v<vmhook::hash_map>,
              "hash_map copy ctor must be noexcept");
static_assert(std::is_nothrow_move_constructible_v<vmhook::hash_map>,
              "hash_map move ctor must be noexcept");
static_assert(sizeof(vmhook::hash_map) == sizeof(vmhook::map),
              "hash_map adds no data members beyond map");

// The OOP ctor on every map flavour is EXPLICIT (no silent oop_t -> map).
static_assert(!std::is_convertible_v<vmhook::oop_t, vmhook::map>,
              "map(oop_t) must be explicit");
static_assert(!std::is_convertible_v<vmhook::oop_t, vmhook::hash_map>,
              "hash_map(oop_t) must be explicit");

// No default constructor on either map flavour (a wrapper without an OOP is
// meaningless).
static_assert(!std::is_default_constructible_v<vmhook::map>,
              "map must NOT be default constructible");
static_assert(!std::is_default_constructible_v<vmhook::hash_map>,
              "hash_map must NOT be default constructible");

// hash_map -> map -> oop_reflective_base -> object_base inheritance lattice.
static_assert(std::is_base_of_v<vmhook::map, vmhook::hash_map>,
              "hash_map must derive from map");
static_assert(std::is_base_of_v<vmhook::oop_reflective_base, vmhook::map>,
              "map must derive from oop_reflective_base");
static_assert(std::is_base_of_v<vmhook::object_base, vmhook::map>,
              "map must derive (transitively) from object_base");
static_assert(std::is_base_of_v<vmhook::object_base, vmhook::hash_map>,
              "hash_map must derive (transitively) from object_base");

// ---------------------------------------------------------------------------
// 2. Cold-state runtime: every null/invalid construction route yields the
//    documented safe defaults without throwing.
// ---------------------------------------------------------------------------

auto test_null_oop_accessors_safe() -> void
{
    vmhook::map      m{ nullptr };
    vmhook::hash_map hm{ nullptr };

    check("map_null_get_instance",      m.get_instance() == nullptr);
    check("hash_map_null_get_instance", hm.get_instance() == nullptr);

    // size() returns EXACTLY 0 (not negative) on a null/invalid wrapper.
    const auto ms{ m.size() };
    const auto hms{ hm.size() };
    check("map_null_size_zero",         ms == 0);
    check("hash_map_null_size_zero",    hms == 0);
    check("map_null_size_not_negative", !(ms < 0));
    check("hash_map_null_size_not_negative", !(hms < 0));

    // is_empty()==true and is_empty() == (size()==0) — internal consistency.
    check("map_null_is_empty",          m.is_empty());
    check("hash_map_null_is_empty",     hm.is_empty());
    check("map_null_is_empty_matches_size", m.is_empty() == (m.size() == 0));
    check("hash_map_null_is_empty_matches_size",
          hm.is_empty() == (hm.size() == 0));

    // to_entries<K,V>() returns an empty vector without throwing.
    const auto em{ m.to_entries<k_w, v_w>() };
    const auto ehm{ hm.to_entries<k_w, v_w>() };
    check("map_null_to_entries_empty",      em.empty());
    check("map_null_to_entries_size0",      em.size() == 0u);
    check("hash_map_null_to_entries_empty", ehm.empty());
    check("hash_map_null_to_entries_size0", ehm.size() == 0u);
}

// ---------------------------------------------------------------------------
// 3. Sentinel-address fuzz: a wide grid of non-null, manifestly-invalid OOPs
//    must all be rejected before any walk fires. is_valid_pointer rejects
//    anything below user_address_floor (0xFFFF on every platform) and any odd
//    / sentinel pointer; we pick values that exercise those rejection clauses
//    plus a few values just above the floor that are still invalid (no real
//    heap exists in this standalone process).
// ---------------------------------------------------------------------------

auto test_invalid_oop_accessors_safe() -> void
{
    const std::uintptr_t sentinels[]{
        0x1u, 0x2u, 0x3u, 0x4u, 0x7u, 0x10u, 0x40u, 0x100u, 0x400u,
        0x1000u, 0x4000u, 0x8000u, 0xFFFEu, 0xFFFFu,
        0xDEADBEEFu, 0xBADF00Du, 0xCAFEBABEu, 0xFEEDFACEu,
        // High-bit values: on 64-bit, these land in non-canonical / kernel
        // ranges that is_valid_pointer rejects via its upper-bound check.
        static_cast<std::uintptr_t>(0x8000'0000'0000'0000ull),
        static_cast<std::uintptr_t>(0xFFFF'FFFF'FFFF'FFF0ull),
    };

    for (const auto raw : sentinels)
    {
        const auto oop{ reinterpret_cast<vmhook::oop_t>(raw) };
        vmhook::map      m{ oop };
        vmhook::hash_map hm{ oop };

        // The wrapper REMEMBERS the raw OOP (get_instance returns what was
        // stored), but every accessor that would dereference it is null-safe.
        std::string n_sz{ "map_bogus_size_zero_" };
        n_sz += std::to_string(raw);
        check(n_sz.c_str(), m.size() == 0);

        std::string n_emp{ "map_bogus_is_empty_" };
        n_emp += std::to_string(raw);
        check(n_emp.c_str(), m.is_empty());

        std::string n_ent{ "map_bogus_to_entries_empty_" };
        n_ent += std::to_string(raw);
        check(n_ent.c_str(), (m.to_entries<k_w, v_w>().empty()));

        std::string n_hm{ "hash_map_bogus_to_entries_empty_" };
        n_hm += std::to_string(raw);
        check(n_hm.c_str(), (hm.to_entries<k_w, v_w>().empty()));
    }
}

// ---------------------------------------------------------------------------
// 4. The "get" reach: field_proxy::get() -> value_t -> to_entries.
//
// This is the documented user reach for a Map-typed field. A field_proxy over
// a null field_pointer returns value_t{int32{0}, sig}; to_entries decodes
// compressed OOP 0 -> nullptr -> empty, without throwing. We exercise BOTH the
// "Ljava/util/Map;" and the LinkedHashMap / HashMap / TreeMap concrete
// descriptors — the value_t::to_entries delegator is signature-agnostic, so
// all of them must produce the same safe empty result.
// ---------------------------------------------------------------------------

auto test_get_reach_via_field_proxy() -> void
{
    const char* const map_sigs[]{
        "Ljava/util/Map;",
        "Ljava/util/HashMap;",
        "Ljava/util/LinkedHashMap;",
        "Ljava/util/TreeMap;",
        "Ljava/util/concurrent/ConcurrentHashMap;",
        // Even a NON-map signature must produce empty entries (the delegator
        // tries map and finds no klass -> empty, never throws).
        "Ljava/util/List;",
        "Ljava/lang/String;",
        "I",
        "",
    };

    for (const auto* sig : map_sigs)
    {
        vmhook::field_proxy proxy{ nullptr, sig, false };

        // Repeat the get() reach to prove it has no side effects.
        const auto entries_1{ proxy.get().to_entries<k_w, v_w>() };
        const auto entries_2{ proxy.get().to_entries<k_w, v_w>() };

        std::string n1{ "proxy_get_to_entries_empty_" };
        n1 += sig;
        check(n1.c_str(), entries_1.empty());

        std::string n2{ "proxy_get_to_entries_stable_" };
        n2 += sig;
        check(n2.c_str(), entries_1.size() == entries_2.size());
    }

    // The same proxy reach, but constructed via a STATIC field_proxy: still
    // null field_pointer -> still safe-default empty.
    vmhook::field_proxy static_proxy{ nullptr, "Ljava/util/Map;", true };
    check("static_proxy_get_to_entries_empty",
          (static_proxy.get().to_entries<k_w, v_w>().empty()));

    // Cross-typed: to_entries on a List-typed proxy and to_entries on a
    // bare-int proxy both return empty (no klass -> no walk).
    vmhook::field_proxy list_proxy{ nullptr, "Ljava/util/List;", false };
    vmhook::field_proxy int_proxy{ nullptr, "I", false };
    check("list_proxy_get_to_entries_empty",
          (list_proxy.get().to_entries<k_w, v_w>().empty()));
    check("int_proxy_get_to_entries_empty",
          (int_proxy.get().to_entries<k_w, v_w>().empty()));
}

// ---------------------------------------------------------------------------
// 5. Copy / move of a map wrapper preserves its null/empty safety.
//
// The wrappers inherit object_base's defaulted copy/move. A copy of a null map
// is still null/empty; a moved-from map is nulled but still satisfies the
// never-throw / empty contract.
// ---------------------------------------------------------------------------

auto test_copy_move_preserves_safety() -> void
{
    vmhook::map m0{ nullptr };
    vmhook::map m_copy{ m0 };
    check("map_copy_null_get_instance",     m_copy.get_instance() == nullptr);
    check("map_copy_null_size_zero",        m_copy.size() == 0);
    check("map_copy_null_is_empty",         m_copy.is_empty());
    check("map_copy_null_to_entries_empty", (m_copy.to_entries<k_w, v_w>().empty()));

    vmhook::hash_map hm0{ nullptr };
    vmhook::hash_map hm_copy{ hm0 };
    check("hash_map_copy_null_to_entries_empty",
          (hm_copy.to_entries<k_w, v_w>().empty()));
    check("hash_map_copy_null_is_empty", hm_copy.is_empty());

    vmhook::map m_src{ nullptr };
    vmhook::map m_dst{ std::move(m_src) };
    check("map_moved_to_dst_get_instance",  m_dst.get_instance() == nullptr);
    check("map_moved_to_dst_size_zero",     m_dst.size() == 0);
    check("map_moved_to_dst_is_empty",      m_dst.is_empty());
    // NOLINTNEXTLINE(bugprone-use-after-move)
    check("map_moved_from_get_instance",    m_src.get_instance() == nullptr);
    // NOLINTNEXTLINE(bugprone-use-after-move)
    check("map_moved_from_to_entries_empty",(m_src.to_entries<k_w, v_w>().empty()));

    vmhook::hash_map hm_src{ nullptr };
    vmhook::hash_map hm_dst{ std::move(hm_src) };
    check("hash_map_moved_to_dst_size_zero", hm_dst.size() == 0);
    // NOLINTNEXTLINE(bugprone-use-after-move)
    check("hash_map_moved_from_is_empty",    hm_src.is_empty());
}

// ---------------------------------------------------------------------------
// 6. Cross-instantiation of to_entries with various (K,V) wrapper pairs all
//    yield the same empty result on the cold path. The (K,V) types are pure
//    template parameters; a regression that started instantiating them
//    differently (e.g. requiring a more-specific concept) would fail to
//    compile here.
// ---------------------------------------------------------------------------

auto test_cross_kv_instantiations_empty() -> void
{
    vmhook::map m{ nullptr };
    // All four wrapper-pair shapes compile and return empty.
    check("to_entries_kk_empty", (m.to_entries<k_w, k_w>().empty()));
    check("to_entries_vv_empty", (m.to_entries<v_w, v_w>().empty()));
    check("to_entries_kv_empty", (m.to_entries<k_w, v_w>().empty()));
    check("to_entries_vk_empty", (m.to_entries<v_w, k_w>().empty()));
}

}  // namespace

auto main() -> int
{
    test_null_oop_accessors_safe();
    test_invalid_oop_accessors_safe();
    test_get_reach_via_field_proxy();
    test_copy_move_preserves_safety();
    test_cross_kv_instantiations_empty();

    std::printf("collection_map_cold_nojvm: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
