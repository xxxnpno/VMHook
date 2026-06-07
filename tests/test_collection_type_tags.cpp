// No-JVM type-tag + never-throw checks for the java.util container wrappers
// (vmhook::collection / list / linked_list / set / map / hash_map) and the
// field_proxy::value_t::to_vector / to_entries entry points users reach via
// `get_field("foo")->get().to_vector<T>()`.
//
// Everything here is pure C++ type-surface and null-safety: no JVM library is
// loaded in this standalone process, so every wrapper built from a null OOP
// must resolve no klass, walk no array, and return an EMPTY container WITHOUT
// throwing or dereferencing anything (the documented never-throw / empty-on-
// failure guarantee on collection::to_vector, map::to_entries, and the two
// out-of-line field_proxy::value_t delegators).
//
// Symbols confirmed against vmhook/ext/vmhook/vmhook.hpp before use:
//   * vmhook::collection : object_base            (ctor: explicit (oop_t) noexcept)
//   * vmhook::list       : collection             (ctor: explicit (oop_t) noexcept)
//   * vmhook::set        : collection             (ctor: explicit (oop_t) noexcept)
//   * vmhook::linked_list: list                   (ctor: explicit (oop_t) noexcept)
//   * vmhook::map        : object_base            (ctor: explicit (oop_t) noexcept)
//   * vmhook::hash_map   : map                    (ctor: explicit (oop_t) noexcept)
//   * vmhook::oop_t == void*
//   * collection::to_vector<E>()  -> std::vector<std::unique_ptr<E>>
//   * map::to_entries<K,V>()      -> std::vector<std::pair<unique_ptr<K>,unique_ptr<V>>>
//   * field_proxy::value_t (aggregate: std::variant<...> data; std::string signature{})
//   * field_proxy::value_t::to_vector<E>()   / ::to_entries<K,V>()
//   * field_proxy{ field_pointer, signature, is_static } 3-arg ctor; .get() -> value_t
//
// NOTE on the element type: to_vector<E>() / to_entries<K,V>() instantiate
// std::make_unique<E>(vmhook::oop_t) in their (runtime-unreached) bodies, so E
// must be CONSTRUCTIBLE FROM vmhook::oop_t (== void*).  A plain `int` is NOT
// (std::make_unique<int>(void*) does not compile), so the task's literal
// `to_vector<int>()` / `to_entries<int,int>()` cannot build.  We therefore use
// minimal oop-constructible wrapper tags (elem_w / key_w / val_w) — the exact
// pattern tests/test_api_surface.cpp uses — which faithfully exercises the same
// documented empty/never-throw behaviour.
#include <vmhook/vmhook.hpp>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

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
// Minimal oop-constructible element / key / value wrappers.  to_vector<E>()
// and to_entries<K,V>() require E/K/V to be constructible from vmhook::oop_t;
// these are the smallest types that satisfy that contract.
// ---------------------------------------------------------------------------
class elem_w : public vmhook::object<elem_w>
{
public:
    explicit elem_w(vmhook::oop_t oop) noexcept
        : vmhook::object<elem_w>{ oop }
    {
    }
};

class key_w : public vmhook::object<key_w>
{
public:
    explicit key_w(vmhook::oop_t oop) noexcept
        : vmhook::object<key_w>{ oop }
    {
    }
};

class val_w : public vmhook::object<val_w>
{
public:
    explicit val_w(vmhook::oop_t oop) noexcept
        : vmhook::object<val_w>{ oop }
    {
    }
};

// ---------------------------------------------------------------------------
// 1. The container type tags are DISTINCT C++ types.
//
// collection / list / set / linked_list / map / hash_map each name a separate
// class.  A regression that collapsed two of them into a typedef alias would
// make one of these std::is_same_v checks flip to true and fail loudly.
// ---------------------------------------------------------------------------
static auto test_type_tags_are_distinct() -> void
{
    check("collection_ne_list",     !std::is_same_v<vmhook::collection, vmhook::list>);
    check("collection_ne_set",      !std::is_same_v<vmhook::collection, vmhook::set>);
    check("collection_ne_map",      !std::is_same_v<vmhook::collection, vmhook::map>);
    check("list_ne_set",            !std::is_same_v<vmhook::list, vmhook::set>);
    check("list_ne_linked_list",    !std::is_same_v<vmhook::list, vmhook::linked_list>);
    check("set_ne_linked_list",     !std::is_same_v<vmhook::set, vmhook::linked_list>);
    check("map_ne_hash_map",        !std::is_same_v<vmhook::map, vmhook::hash_map>);
    check("collection_ne_hash_map", !std::is_same_v<vmhook::collection, vmhook::hash_map>);
    check("list_ne_map",            !std::is_same_v<vmhook::list, vmhook::map>);

    // Exhaustive remaining distinctness pairs across the six tags so a typedef
    // alias collapsing ANY two of them flips exactly one of these to true.
    check("set_ne_map",             !std::is_same_v<vmhook::set, vmhook::map>);
    check("set_ne_hash_map",        !std::is_same_v<vmhook::set, vmhook::hash_map>);
    check("set_ne_linked_list2",    !std::is_same_v<vmhook::set, vmhook::linked_list>);
    check("linked_list_ne_map",     !std::is_same_v<vmhook::linked_list, vmhook::map>);
    check("linked_list_ne_hash_map",!std::is_same_v<vmhook::linked_list, vmhook::hash_map>);
    check("list_ne_hash_map",       !std::is_same_v<vmhook::list, vmhook::hash_map>);
    check("collection_ne_linked_list",
          !std::is_same_v<vmhook::collection, vmhook::linked_list>);

    // The shared mixin base is itself a distinct type from every concrete tag
    // and from object_base (it sits strictly between them in the hierarchy).
    check("oop_reflective_base_ne_object_base",
          !std::is_same_v<vmhook::oop_reflective_base, vmhook::object_base>);
    check("oop_reflective_base_ne_collection",
          !std::is_same_v<vmhook::oop_reflective_base, vmhook::collection>);
    check("oop_reflective_base_ne_map",
          !std::is_same_v<vmhook::oop_reflective_base, vmhook::map>);
}

// ---------------------------------------------------------------------------
// 2. The documented inheritance lattice holds.
//
// list/set derive from collection; linked_list derives from list (and so
// transitively from collection); hash_map derives from map.  These are the
// relationships the README/header promise so that a `const std::unique_ptr<
// vmhook::list>&` detour parameter is substitutable for a collection.
// ---------------------------------------------------------------------------
static auto test_inheritance_lattice() -> void
{
    check("list_is_collection",          (std::is_base_of_v<vmhook::collection, vmhook::list>));
    check("set_is_collection",           (std::is_base_of_v<vmhook::collection, vmhook::set>));
    check("linked_list_is_list",         (std::is_base_of_v<vmhook::list, vmhook::linked_list>));
    check("linked_list_is_collection",   (std::is_base_of_v<vmhook::collection, vmhook::linked_list>));
    check("hash_map_is_map",             (std::is_base_of_v<vmhook::map, vmhook::hash_map>));
    // collection and map are unrelated branches of the hierarchy.
    check("collection_not_base_of_map",  (!std::is_base_of_v<vmhook::collection, vmhook::map>));
    check("map_not_base_of_collection",  (!std::is_base_of_v<vmhook::map, vmhook::collection>));

    // --- The shared mixin: oop_reflective_base sits above BOTH branches. ---
    // collection and map both derive from oop_reflective_base, which derives
    // from object_base.  This shared ancestry is exactly what the task warns
    // must NOT make the two branches relatives of each other.
    check("collection_is_oop_reflective_base",
          (std::is_base_of_v<vmhook::oop_reflective_base, vmhook::collection>));
    check("map_is_oop_reflective_base",
          (std::is_base_of_v<vmhook::oop_reflective_base, vmhook::map>));
    check("oop_reflective_base_is_object_base",
          (std::is_base_of_v<vmhook::object_base, vmhook::oop_reflective_base>));

    // Every concrete tag transitively derives from the mixin and from object_base.
    check("list_is_oop_reflective_base",
          (std::is_base_of_v<vmhook::oop_reflective_base, vmhook::list>));
    check("set_is_oop_reflective_base",
          (std::is_base_of_v<vmhook::oop_reflective_base, vmhook::set>));
    check("linked_list_is_oop_reflective_base",
          (std::is_base_of_v<vmhook::oop_reflective_base, vmhook::linked_list>));
    check("hash_map_is_oop_reflective_base",
          (std::is_base_of_v<vmhook::oop_reflective_base, vmhook::hash_map>));
    check("collection_is_object_base",
          (std::is_base_of_v<vmhook::object_base, vmhook::collection>));
    check("map_is_object_base",
          (std::is_base_of_v<vmhook::object_base, vmhook::map>));
    check("hash_map_is_object_base",
          (std::is_base_of_v<vmhook::object_base, vmhook::hash_map>));
    check("linked_list_is_object_base",
          (std::is_base_of_v<vmhook::object_base, vmhook::linked_list>));

    // --- The CROSS-branch non-relationships (the core regression guard). ---
    // Despite sharing oop_reflective_base + object_base, NOTHING on the
    // collection side is a base/derived of anything on the map side.
    check("map_not_base_of_list",      (!std::is_base_of_v<vmhook::map, vmhook::list>));
    check("list_not_base_of_map",      (!std::is_base_of_v<vmhook::list, vmhook::map>));
    check("map_not_base_of_set",       (!std::is_base_of_v<vmhook::map, vmhook::set>));
    check("set_not_base_of_map",       (!std::is_base_of_v<vmhook::set, vmhook::map>));
    check("hash_map_not_base_of_collection",
          (!std::is_base_of_v<vmhook::hash_map, vmhook::collection>));
    check("collection_not_base_of_hash_map",
          (!std::is_base_of_v<vmhook::collection, vmhook::hash_map>));
    check("hash_map_not_base_of_list",
          (!std::is_base_of_v<vmhook::hash_map, vmhook::list>));
    check("linked_list_not_base_of_map",
          (!std::is_base_of_v<vmhook::linked_list, vmhook::map>));
    check("map_not_base_of_linked_list",
          (!std::is_base_of_v<vmhook::map, vmhook::linked_list>));
    check("hash_map_not_base_of_set",
          (!std::is_base_of_v<vmhook::hash_map, vmhook::set>));

    // --- Direction / reflexivity sanity on the lattice. ---
    // is_base_of is reflexive in C++ (a class is its own base), but a derived
    // class is NEVER a base of its parent.
    check("collection_not_base_of_self_derived_list",
          (!std::is_base_of_v<vmhook::list, vmhook::collection>));   // list is NOT a base of collection
    check("list_not_base_of_linked_list_reversed",
          (std::is_base_of_v<vmhook::list, vmhook::linked_list>)     // forward holds
          && (!std::is_base_of_v<vmhook::linked_list, vmhook::list>)); // reverse does not
    check("map_not_base_of_hash_map_reversed",
          (std::is_base_of_v<vmhook::map, vmhook::hash_map>)
          && (!std::is_base_of_v<vmhook::hash_map, vmhook::map>));
    // The mixin is not a base of object_base (it derives FROM it, not the
    // other way around).
    check("oop_reflective_base_not_base_of_object_base",
          (!std::is_base_of_v<vmhook::oop_reflective_base, vmhook::object_base>));
}

// ---------------------------------------------------------------------------
// 3. Each tag is usable as a declaration / constructible from a null OOP.
//
// Every wrapper has an `explicit (vmhook::oop_t) noexcept` constructor, so it
// is a usable declaration target and a null OOP yields a fully inert wrapper
// whose get_instance() reports nullptr.  These also prove the ctors are noexcept.
// ---------------------------------------------------------------------------
static auto test_default_null_construction() -> void
{
    vmhook::collection  c{ nullptr };
    vmhook::list        l{ nullptr };
    vmhook::set         s{ nullptr };
    vmhook::linked_list ll{ nullptr };
    vmhook::map         m{ nullptr };
    vmhook::hash_map    hm{ nullptr };

    check("collection_null_instance",  c.get_instance() == nullptr);
    check("list_null_instance",        l.get_instance() == nullptr);
    check("set_null_instance",         s.get_instance() == nullptr);
    check("linked_list_null_instance", ll.get_instance() == nullptr);
    check("map_null_instance",         m.get_instance() == nullptr);
    check("hash_map_null_instance",    hm.get_instance() == nullptr);

    check("collection_ctor_noexcept",  noexcept(vmhook::collection{ nullptr }));
    check("list_ctor_noexcept",        noexcept(vmhook::list{ nullptr }));
    check("set_ctor_noexcept",         noexcept(vmhook::set{ nullptr }));
    check("linked_list_ctor_noexcept", noexcept(vmhook::linked_list{ nullptr }));
    check("map_ctor_noexcept",         noexcept(vmhook::map{ nullptr }));
    check("hash_map_ctor_noexcept",    noexcept(vmhook::hash_map{ nullptr }));
}

// ---------------------------------------------------------------------------
// 4. size()/is_empty() are null-safe on every collection/map tag.
//
// With no JVM, get_method_by_oop_klass("size") finds no klass and short-
// circuits, so size() returns 0 and is_empty() returns true — no fault.
// ---------------------------------------------------------------------------
static auto test_size_and_is_empty_null_safe() -> void
{
    vmhook::collection  c{ nullptr };
    vmhook::list        l{ nullptr };
    vmhook::set         s{ nullptr };
    vmhook::linked_list ll{ nullptr };
    vmhook::map         m{ nullptr };
    vmhook::hash_map    hm{ nullptr };

    check("collection_size_zero",  c.size() == 0);
    check("list_size_zero",        l.size() == 0);
    check("set_size_zero",         s.size() == 0);
    check("linked_list_size_zero", ll.size() == 0);
    check("map_size_zero",         m.size() == 0);
    check("hash_map_size_zero",    hm.size() == 0);

    check("collection_is_empty_true", c.is_empty());
    check("map_is_empty_true",        m.is_empty());
    // is_empty() is null-safe on EVERY tag (size()==0 -> empty), not just the
    // two the original suite checked.
    check("list_is_empty_true",        l.is_empty());
    check("set_is_empty_true",         s.is_empty());
    check("linked_list_is_empty_true", ll.is_empty());
    check("hash_map_is_empty_true",    hm.is_empty());
    check("hash_map_size_zero2",       hm.size() == 0);
}

// ---------------------------------------------------------------------------
// 5. Direct wrapper to_vector<E>() returns an EMPTY vector, never throws.
//
// collection::to_vector (inherited by list / set / linked_list) early-returns
// an empty result when `instance` is null/invalid — the core never-throw
// guarantee.  We assert empty() AND size()==0 on each tag.
// ---------------------------------------------------------------------------
static auto test_to_vector_empty_no_jvm() -> void
{
    vmhook::collection  c{ nullptr };
    vmhook::list        l{ nullptr };
    vmhook::set         s{ nullptr };
    vmhook::linked_list ll{ nullptr };

    const auto vc{ c.to_vector<elem_w>() };
    const auto vl{ l.to_vector<elem_w>() };
    const auto vs{ s.to_vector<elem_w>() };
    const auto vll{ ll.to_vector<elem_w>() };

    check("collection_to_vector_empty",  vc.empty());
    check("collection_to_vector_size0",  vc.size() == 0);
    check("list_to_vector_empty",        vl.empty());
    check("set_to_vector_empty",         vs.empty());
    check("linked_list_to_vector_empty", vll.empty());
}

// ---------------------------------------------------------------------------
// 6. Direct map to_entries<K,V>() returns an EMPTY entries vector, never throws.
//
// map::to_entries (inherited by hash_map) early-returns empty when `instance`
// is null/invalid.
// ---------------------------------------------------------------------------
static auto test_to_entries_empty_no_jvm() -> void
{
    vmhook::map      m{ nullptr };
    vmhook::hash_map hm{ nullptr };

    const auto em{ m.to_entries<key_w, val_w>() };
    const auto ehm{ hm.to_entries<key_w, val_w>() };

    check("map_to_entries_empty",      em.empty());
    check("map_to_entries_size0",      em.size() == 0);
    check("hash_map_to_entries_empty", ehm.empty());
    check("hash_map_to_entries_size0", ehm.size() == 0);
}

// ---------------------------------------------------------------------------
// 7. A DEFAULT-constructed field_proxy::value_t yields empty containers.
//
// value_t is an aggregate { std::variant<bool,...,uint32_t> data; std::string
// signature{}; }.  Default-constructed, `data` holds the first alternative
// (bool=false), which converts to compressed OOP 0; decode_oop_pointer(0) is
// null, so the out-of-line value_t::to_vector / ::to_entries delegators return
// {} per their documented "empty on failure" contract — without throwing.
// ---------------------------------------------------------------------------
static auto test_default_value_t_empty() -> void
{
    vmhook::field_proxy::value_t v{};

    const auto vec{ v.to_vector<elem_w>() };
    const auto entries{ v.to_entries<key_w, val_w>() };

    check("default_value_t_to_vector_empty",   vec.empty());
    check("default_value_t_to_vector_size0",   vec.size() == 0);
    check("default_value_t_to_entries_empty",  entries.empty());
    check("default_value_t_to_entries_size0",  entries.size() == 0);

    // Aggregate-init with an explicit signature behaves identically: the OOP
    // alternative is still absent/zero, so both delegators return empty.
    vmhook::field_proxy::value_t v_sig{ std::uint32_t{ 0 }, std::string{ "Ljava/util/List;" } };
    check("value_t_zero_oop_to_vector_empty",
          v_sig.to_vector<elem_w>().empty());
    check("value_t_zero_oop_to_entries_empty",
          v_sig.to_entries<key_w, val_w>().empty());
}

// ---------------------------------------------------------------------------
// 8. value_t reached via field_proxy::get() (a null-OOP proxy) is also empty.
//
// This is the exact path users hit: get_field("foo")->get().to_vector<T>().
// A field_proxy over a null field_pointer produces a value_t whose decoded OOP
// is null, so both entry points return empty without faulting.  We cover both
// a List-typed and a Map-typed signature.
// ---------------------------------------------------------------------------
static auto test_value_t_via_field_proxy_empty() -> void
{
    vmhook::field_proxy list_field{ nullptr, "Ljava/util/List;", false };
    vmhook::field_proxy map_field{ nullptr, "Ljava/util/Map;", false };

    const auto vec{ list_field.get().to_vector<elem_w>() };
    const auto entries{ map_field.get().to_entries<key_w, val_w>() };

    check("proxy_list_to_vector_empty",  vec.empty());
    check("proxy_list_to_vector_size0",  vec.size() == 0);
    check("proxy_map_to_entries_empty",  entries.empty());
    check("proxy_map_to_entries_size0",  entries.size() == 0);

    // Same proxy, cross-shaped call: to_entries on a List-typed proxy and
    // to_vector on a Map-typed proxy still return empty (null OOP dominates).
    check("proxy_list_to_entries_empty",
          list_field.get().to_entries<key_w, val_w>().empty());
    check("proxy_map_to_vector_empty",
          map_field.get().to_vector<elem_w>().empty());
}

int main()
{
    // Registering the element wrappers mirrors real usage; harmless with no JVM.
    vmhook::register_class<elem_w>("test/Element");
    vmhook::register_class<key_w>("test/Key");
    vmhook::register_class<val_w>("test/Value");

    test_type_tags_are_distinct();
    test_inheritance_lattice();
    test_default_null_construction();
    test_size_and_is_empty_null_safe();
    test_to_vector_empty_no_jvm();
    test_to_entries_empty_no_jvm();
    test_default_value_t_empty();
    test_value_t_via_field_proxy_empty();

    return failures == 0 ? 0 : 1;
}
