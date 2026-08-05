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
// WHY EVERY ASSERTION IS DETERMINISTIC WITHOUT A JVM
// --------------------------------------------------
// decode_oop_pointer() resolves CompressedOops::_narrow_oop.{_base,_shift} from
// the live JVM's exported gHotSpotVMStructs.  With no jvm.dll/libjvm.so loaded,
// resolve_struct_entry() returns nullptr, so decode_oop_pointer() returns
// nullptr for EVERY non-zero compressed value (1, 2, 0xFFFFFFFF, ...), not just
// 0.  is_valid_pointer() independently rejects anything outside
// [user_address_floor, user_address_ceiling) and any odd / sentinel address.
// Consequently:
//   * value_t::to_vector / to_entries decode the stored OOP -> nullptr -> {}.
//   * collection / map built from a null OOP resolve no klass (klass_from_oop
//     short-circuits on the null/invalid instance) -> size()==0, empty walks.
//   * the '[L...;' / '[[...' raw-array branch of value_t::to_vector still
//     decodes its backing-array OOP -> nullptr -> empty.
// None of this depends on float formatting, NaN bit patterns, long-double
// width, pointer trap representations, or any platform-variant behaviour, so
// the same PASS/FAIL holds on MinGW libstdc++, MSVC STL, and libc++.
//
// Symbols confirmed against vmhook/ext/vmhook/vmhook.hpp before use:
//   * vmhook::collection : oop_reflective_base    (ctor: explicit (oop_t) noexcept)
//   * vmhook::list       : collection             (ctor: explicit (oop_t) noexcept)
//   * vmhook::set        : collection             (ctor: explicit (oop_t) noexcept)
//   * vmhook::linked_list: list                   (ctor: explicit (oop_t) noexcept)
//   * vmhook::map        : oop_reflective_base     (ctor: explicit (oop_t) noexcept)
//   * vmhook::hash_map   : map                    (ctor: explicit (oop_t) noexcept)
//   * vmhook::oop_reflective_base : object_base   (shared live-OOP klass mixin)
//   * vmhook::oop_t == void*
//   * collection::size()/is_empty() noexcept; map::size()/is_empty() noexcept
//   * collection::to_vector<E>()  -> std::vector<std::unique_ptr<E>>
//   * map::to_entries<K,V>()      -> std::vector<std::pair<unique_ptr<K>,unique_ptr<V>>>
//   * field_proxy::value_t (aggregate: std::variant<bool,i8,i16,i32,i64,float,
//                            double,u16,u32> data; std::string signature{})
//   * field_proxy::value_t::to_vector<E>()   / ::to_entries<K,V>()
//   * field_proxy::value_t::is_reference() / ::as_string()
//   * field_proxy{ field_pointer, signature, is_static } 3-arg ctor; .get() -> value_t
//   * value_t::to_vector special-cases a raw object-array field when
//     signature.size() >= 2 && front()=='[' && (sig[1]=='L' || sig[1]=='[').
//
// NOTE on the element type: to_vector<E>() / to_entries<K,V>() instantiate
// std::make_unique<E>(vmhook::oop_t) in their (runtime-unreached) bodies, so E
// must be CONSTRUCTIBLE FROM vmhook::oop_t (== void*).  A plain `int` is NOT
// (std::make_unique<int>(void*) does not compile), so the obvious literal
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
#include <limits>
#include <string_view>
#include <cstring>
#include <array>
#include <tuple>

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

// Convenience aliases for the field_proxy::value_t aggregate so the matrix
// tables below read cleanly.
using value_t = vmhook::field_proxy::value_t;

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

    // The mixin is also distinct from every remaining concrete tag (so a future
    // refactor cannot accidentally typedef one of them onto the shared base).
    check("oop_reflective_base_ne_list",
          !std::is_same_v<vmhook::oop_reflective_base, vmhook::list>);
    check("oop_reflective_base_ne_set",
          !std::is_same_v<vmhook::oop_reflective_base, vmhook::set>);
    check("oop_reflective_base_ne_linked_list",
          !std::is_same_v<vmhook::oop_reflective_base, vmhook::linked_list>);
    check("oop_reflective_base_ne_hash_map",
          !std::is_same_v<vmhook::oop_reflective_base, vmhook::hash_map>);

    // object_base is likewise distinct from every concrete tag — the wrappers
    // add identity even though they add no data members.
    check("object_base_ne_collection",
          !std::is_same_v<vmhook::object_base, vmhook::collection>);
    check("object_base_ne_map",
          !std::is_same_v<vmhook::object_base, vmhook::map>);
    check("object_base_ne_list",
          !std::is_same_v<vmhook::object_base, vmhook::list>);
    check("object_base_ne_hash_map",
          !std::is_same_v<vmhook::object_base, vmhook::hash_map>);

    // The element/key/value wrapper tags this test instantiates to_vector /
    // to_entries with are themselves distinct from the container tags and from
    // each other — guards against a copy/paste alias in the test fixtures.
    check("elem_w_ne_collection",  !std::is_same_v<elem_w, vmhook::collection>);
    check("key_w_ne_val_w",        !std::is_same_v<key_w, val_w>);
    check("elem_w_ne_key_w",       !std::is_same_v<elem_w, key_w>);
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
    check("list_is_object_base",
          (std::is_base_of_v<vmhook::object_base, vmhook::list>));
    check("set_is_object_base",
          (std::is_base_of_v<vmhook::object_base, vmhook::set>));

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
    // hash_map vs the collection-side leaf tags (extra cross-branch guards).
    check("hash_map_not_base_of_linked_list",
          (!std::is_base_of_v<vmhook::hash_map, vmhook::linked_list>));
    check("linked_list_not_base_of_hash_map",
          (!std::is_base_of_v<vmhook::linked_list, vmhook::hash_map>));
    check("set_not_base_of_hash_map",
          (!std::is_base_of_v<vmhook::set, vmhook::hash_map>));

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
    check("collection_base_of_set_not_reversed",
          (std::is_base_of_v<vmhook::collection, vmhook::set>)
          && (!std::is_base_of_v<vmhook::set, vmhook::collection>));
    // The mixin is not a base of object_base (it derives FROM it, not the
    // other way around).
    check("oop_reflective_base_not_base_of_object_base",
          (!std::is_base_of_v<vmhook::oop_reflective_base, vmhook::object_base>));

    // is_base_of is reflexive: every tag is its own base.  Pins that the trait
    // is being read the right way round in the directional checks above.
    check("collection_base_of_self",  (std::is_base_of_v<vmhook::collection, vmhook::collection>));
    check("map_base_of_self",         (std::is_base_of_v<vmhook::map, vmhook::map>));
    check("hash_map_base_of_self",    (std::is_base_of_v<vmhook::hash_map, vmhook::hash_map>));
}

// ---------------------------------------------------------------------------
// 2b. Every tag is constructible from vmhook::oop_t and NOT default- or
//     implicitly-convertible from it (the ctors are `explicit (oop_t)`).
//
// These compile-time traits pin the exact constructor contract: a wrapper is
// built from a decoded OOP, never silently from an unrelated pointer-shaped
// value, and never default-constructed (there is no oop-less wrapper).
// ---------------------------------------------------------------------------
static auto test_tag_construction_traits() -> void
{
    check("collection_constructible_from_oop",  (std::is_constructible_v<vmhook::collection, vmhook::oop_t>));
    check("list_constructible_from_oop",        (std::is_constructible_v<vmhook::list, vmhook::oop_t>));
    check("set_constructible_from_oop",         (std::is_constructible_v<vmhook::set, vmhook::oop_t>));
    check("linked_list_constructible_from_oop", (std::is_constructible_v<vmhook::linked_list, vmhook::oop_t>));
    check("map_constructible_from_oop",         (std::is_constructible_v<vmhook::map, vmhook::oop_t>));
    check("hash_map_constructible_from_oop",    (std::is_constructible_v<vmhook::hash_map, vmhook::oop_t>));

    // The OOP ctor is EXPLICIT on every tag: an oop_t must not implicitly
    // convert to a wrapper (that would let a raw void* silently become a list).
    check("collection_oop_ctor_explicit",  (!std::is_convertible_v<vmhook::oop_t, vmhook::collection>));
    check("list_oop_ctor_explicit",        (!std::is_convertible_v<vmhook::oop_t, vmhook::list>));
    check("set_oop_ctor_explicit",         (!std::is_convertible_v<vmhook::oop_t, vmhook::set>));
    check("linked_list_oop_ctor_explicit", (!std::is_convertible_v<vmhook::oop_t, vmhook::linked_list>));
    check("map_oop_ctor_explicit",         (!std::is_convertible_v<vmhook::oop_t, vmhook::map>));
    check("hash_map_oop_ctor_explicit",    (!std::is_convertible_v<vmhook::oop_t, vmhook::hash_map>));

    // No tag is default-constructible: a wrapper without an OOP is meaningless.
    check("collection_not_default_constructible", (!std::is_default_constructible_v<vmhook::collection>));
    check("map_not_default_constructible",        (!std::is_default_constructible_v<vmhook::map>));
    check("linked_list_not_default_constructible",(!std::is_default_constructible_v<vmhook::linked_list>));
    check("hash_map_not_default_constructible",   (!std::is_default_constructible_v<vmhook::hash_map>));

    // The wrappers are copyable and movable (they inherit object_base's
    // defaulted copy/move) — needed so they can be stored in containers.
    check("collection_copy_constructible", (std::is_copy_constructible_v<vmhook::collection>));
    check("collection_move_constructible", (std::is_move_constructible_v<vmhook::collection>));
    check("map_copy_constructible",        (std::is_copy_constructible_v<vmhook::map>));
    check("map_move_constructible",        (std::is_move_constructible_v<vmhook::map>));

    // Adding no data members, every wrapper is the same size as object_base
    // (the lattice is pure type-tagging, not a fat hierarchy).
    check("collection_same_size_as_object_base",
          sizeof(vmhook::collection) == sizeof(vmhook::object_base));
    check("map_same_size_as_object_base",
          sizeof(vmhook::map) == sizeof(vmhook::object_base));
    check("linked_list_same_size_as_object_base",
          sizeof(vmhook::linked_list) == sizeof(vmhook::object_base));
    check("hash_map_same_size_as_object_base",
          sizeof(vmhook::hash_map) == sizeof(vmhook::object_base));
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
// 3b. Copy / move of a wrapper preserves (or transfers) the OOP and keeps the
//     null/empty contract intact.
//
// The wrappers inherit object_base's defaulted copy (shares the raw OOP) and
// move (transfers the OOP and nulls the source).  A moved-from wrapper must be
// null and still satisfy the empty/never-throw guarantee — this pins that the
// type-tags do not add state that breaks those semantics.
// ---------------------------------------------------------------------------
static auto test_wrapper_copy_move_semantics() -> void
{
    vmhook::collection  c0{ nullptr };
    vmhook::collection  c_copy{ c0 };
    check("collection_copy_null_instance", c_copy.get_instance() == nullptr);
    check("collection_copy_to_vector_empty", c_copy.to_vector<elem_w>().empty());

    vmhook::collection  c_move_src{ nullptr };
    vmhook::collection  c_move_dst{ std::move(c_move_src) };
    check("collection_moved_to_dst_null", c_move_dst.get_instance() == nullptr);
    // Moved-FROM source is nulled by object_base's move ctor; still safe.
    check("collection_moved_from_null", c_move_src.get_instance() == nullptr); // NOLINT(bugprone-use-after-move)
    check("collection_moved_from_to_vector_empty",
          c_move_src.to_vector<elem_w>().empty());                             // NOLINT(bugprone-use-after-move)

    vmhook::map  m0{ nullptr };
    vmhook::map  m_copy{ m0 };
    check("map_copy_null_instance", m_copy.get_instance() == nullptr);
    check("map_copy_to_entries_empty", (m_copy.to_entries<key_w, val_w>().empty()));

    vmhook::hash_map hm0{ nullptr };
    vmhook::hash_map hm_copy{ hm0 };
    check("hash_map_copy_to_entries_empty", (hm_copy.to_entries<key_w, val_w>().empty()));

    // Copy/move ctors are noexcept (inherited from object_base) — needed for
    // strong exception guarantees in std::vector growth.
    check("collection_copy_noexcept",
          (std::is_nothrow_copy_constructible_v<vmhook::collection>));
    check("collection_move_noexcept",
          (std::is_nothrow_move_constructible_v<vmhook::collection>));
    check("map_move_noexcept",
          (std::is_nothrow_move_constructible_v<vmhook::map>));
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

    // size()/is_empty() are documented noexcept on both branches — a throwing
    // size() in a hook detour would escape into the JVM.
    check("collection_size_noexcept",    noexcept(c.size()));
    check("collection_is_empty_noexcept",noexcept(c.is_empty()));
    check("map_size_noexcept",           noexcept(m.size()));
    check("map_is_empty_noexcept",       noexcept(m.is_empty()));

    // size() returns EXACTLY 0 (not a negative sentinel) for a broken / null
    // wrapper.  Flaw #4 in the feature notes: a caller cannot distinguish
    // "empty" from "no klass", and this pins that the contract stays 0 / true
    // so it can never silently flip to e.g. -1.
    check("collection_size_is_exactly_zero_not_negative", c.size() == 0 && !(c.size() < 0));
    check("map_size_is_exactly_zero_not_negative",        m.size() == 0 && !(m.size() < 0));
    check("collection_broken_is_empty_matches_size_zero",
          c.is_empty() == (c.size() == 0));
    check("map_broken_is_empty_matches_size_zero",
          m.is_empty() == (m.size() == 0));
}

// ---------------------------------------------------------------------------
// 4b. The collection and map branches share ONE klass-reflection base, so a
//     null OOP behaves identically across both branches (helper parity).
//
// Feature note Flaw #5 warned the klass helpers were copy-pasted between
// collection and map; the header now hoists them into oop_reflective_base
// (a single implementation both branches inherit).  These checks pin that
// shared ancestry and that the two branches give the SAME null-safe answers,
// so the helpers cannot silently drift apart again.
// ---------------------------------------------------------------------------
static auto test_collection_map_helper_parity() -> void
{
    // Both wrappers derive from the SAME mixin that carries the (single)
    // get_field_by_oop_klass / get_method_by_oop_klass implementation.
    check("collection_and_map_share_mixin",
          (std::is_base_of_v<vmhook::oop_reflective_base, vmhook::collection>)
          && (std::is_base_of_v<vmhook::oop_reflective_base, vmhook::map>));

    vmhook::collection c{ nullptr };
    vmhook::map        m{ nullptr };

    // Identical null OOP -> identical observable answers from the shared helpers.
    check("collection_map_size_parity_null",     c.size() == m.size());
    check("collection_map_is_empty_parity_null", c.is_empty() == m.is_empty());
    check("collection_map_instance_parity_null", c.get_instance() == m.get_instance());

    // The leaf tags inherit the SAME helpers transitively, so list/set/
    // linked_list (collection side) and hash_map (map side) all agree too.
    vmhook::list        l{ nullptr };
    vmhook::set         s{ nullptr };
    vmhook::linked_list ll{ nullptr };
    vmhook::hash_map    hm{ nullptr };
    check("leaf_tags_size_parity_null",
          l.size() == 0 && s.size() == 0 && ll.size() == 0 && hm.size() == 0);
    check("leaf_tags_is_empty_parity_null",
          l.is_empty() && s.is_empty() && ll.is_empty() && hm.is_empty());
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

    // The returned type is exactly std::vector<std::unique_ptr<element_type>>.
    check("collection_to_vector_return_type",
          (std::is_same_v<decltype(vc), const std::vector<std::unique_ptr<elem_w>>>));

    // Instantiating to_vector with DIFFERENT element wrappers still compiles and
    // yields empty — the element type is a pure type parameter on the null path.
    check("collection_to_vector_key_w_empty",  c.to_vector<key_w>().empty());
    check("collection_to_vector_val_w_empty",  c.to_vector<val_w>().empty());

    // A non-null but INVALID oop (a small integer cast to a pointer) is rejected
    // by is_valid_pointer() inside to_vector -> still empty, still no fault.
    // reinterpret_cast<void*>(0x4) is below user_address_floor (0xFFFF) so it is
    // never a real heap pointer on any platform.
    vmhook::collection c_bogus{ reinterpret_cast<vmhook::oop_t>(static_cast<std::uintptr_t>(0x4)) };
    check("collection_bogus_oop_to_vector_empty", c_bogus.to_vector<elem_w>().empty());
    check("collection_bogus_oop_size_zero",       c_bogus.size() == 0);
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

    // The returned type is exactly the documented vector-of-pairs.
    check("map_to_entries_return_type",
          (std::is_same_v<decltype(em),
                          const std::vector<std::pair<std::unique_ptr<key_w>,
                                                      std::unique_ptr<val_w>>>>));

    // Swapped/identical key&value wrapper instantiations also compile + empty.
    check("map_to_entries_val_key_empty",  (m.to_entries<val_w, key_w>().empty()));
    check("map_to_entries_same_type_empty",(m.to_entries<key_w, key_w>().empty()));

    // A non-null but INVALID oop is rejected before any walk -> empty.
    vmhook::map m_bogus{ reinterpret_cast<vmhook::oop_t>(static_cast<std::uintptr_t>(0x6)) };
    check("map_bogus_oop_to_entries_empty", (m_bogus.to_entries<key_w, val_w>().empty()));
    check("map_bogus_oop_size_zero",        m_bogus.size() == 0);
}

// ---------------------------------------------------------------------------
// 6b. Substitutability: a leaf tag binds where its base is expected.
//
// The WHOLE POINT of the lattice is that `const collection&` / `const map&`
// parameters accept the leaf tags by reference (no slicing, the call routes to
// the base's to_vector / to_entries).  is_base_of_v alone does not prove a
// reference actually binds and the call goes through, so do it for real.
// ---------------------------------------------------------------------------
template<typename element_type>
static auto take_collection_ref(const vmhook::collection& col)
    -> std::size_t
{
    // Calls collection::size() + collection::to_vector through a base reference.
    return col.to_vector<element_type>().size() + static_cast<std::size_t>(col.size());
}

template<typename key_type, typename value_type>
static auto take_map_ref(const vmhook::map& mp)
    -> std::size_t
{
    return mp.to_entries<key_type, value_type>().size() + static_cast<std::size_t>(mp.size());
}

static auto test_substitutability_through_base_ref() -> void
{
    vmhook::list        l{ nullptr };
    vmhook::set         s{ nullptr };
    vmhook::linked_list ll{ nullptr };
    vmhook::collection  c{ nullptr };

    // Every collection-side tag binds to `const collection&` and the call
    // through the base reference returns 0 (empty vector + size 0).
    check("collection_ref_binds_collection", take_collection_ref<elem_w>(c) == 0);
    check("collection_ref_binds_list",       take_collection_ref<elem_w>(l) == 0);
    check("collection_ref_binds_set",        take_collection_ref<elem_w>(s) == 0);
    check("collection_ref_binds_linked_list",take_collection_ref<elem_w>(ll) == 0);

    vmhook::map      m{ nullptr };
    vmhook::hash_map hm{ nullptr };
    check("map_ref_binds_map",       (take_map_ref<key_w, val_w>(m) == 0));
    check("map_ref_binds_hash_map",  (take_map_ref<key_w, val_w>(hm) == 0));

    // Reference binding is also a compile-time relationship: a leaf reference is
    // convertible to a base reference, but NOT across branches.
    check("list_ref_convertible_to_collection_ref",
          (std::is_convertible_v<vmhook::list&, vmhook::collection&>));
    check("linked_list_ref_convertible_to_collection_ref",
          (std::is_convertible_v<vmhook::linked_list&, vmhook::collection&>));
    check("hash_map_ref_convertible_to_map_ref",
          (std::is_convertible_v<vmhook::hash_map&, vmhook::map&>));
    check("list_ref_not_convertible_to_map_ref",
          (!std::is_convertible_v<vmhook::list&, vmhook::map&>));
    check("hash_map_ref_not_convertible_to_collection_ref",
          (!std::is_convertible_v<vmhook::hash_map&, vmhook::collection&>));
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
    value_t v{};

    const auto vec{ v.to_vector<elem_w>() };
    const auto entries{ v.to_entries<key_w, val_w>() };

    check("default_value_t_to_vector_empty",   vec.empty());
    check("default_value_t_to_vector_size0",   vec.size() == 0);
    check("default_value_t_to_entries_empty",  entries.empty());
    check("default_value_t_to_entries_size0",  entries.size() == 0);

    // A default value_t holds bool=false, i.e. NOT a reference field.
    check("default_value_t_is_not_reference", !v.is_reference());
    // as_string() on a non-reference alternative yields "" (never faults).
    check("default_value_t_as_string_empty",  v.as_string().empty());

    // Aggregate-init with an explicit signature behaves identically: the OOP
    // alternative is still absent/zero, so both delegators return empty.
    value_t v_sig{ std::uint32_t{ 0 }, std::string{ "Ljava/util/List;" } };
    check("value_t_zero_oop_to_vector_empty",
          v_sig.to_vector<elem_w>().empty());
    check("value_t_zero_oop_to_entries_empty",
          v_sig.to_entries<key_w, val_w>().empty());
    // A uint32 alternative IS a reference field, even when the value is 0.
    check("value_t_uint32_is_reference", v_sig.is_reference());
}

// ---------------------------------------------------------------------------
// 7b. value_t holding EACH non-uint32 variant alternative still yields empty.
//
// to_vector / to_entries read the stored OOP via static_cast<uint32_t>(*this),
// which routes every numeric/bool alternative through cast_for_variant's final
// static_cast (e.g. bool true -> 1, int64 5 -> 5, double 3.9 -> 3).  With no
// JVM, decode_oop_pointer() of ANY value returns nullptr, so all of these are
// empty.  The original suite only ever exercised bool=false -> 0; this pins
// EVERY alternative, including bool=true -> compressed OOP 1 (which must be
// rejected by is_valid_pointer rather than dereferenced).
//
// IMPORTANT: each value is explicitly typed so the std::variant brace-init
// selects the intended alternative unambiguously (a bare integer literal would
// be an ambiguous variant construction across the integral alternatives).
// ---------------------------------------------------------------------------
static auto test_value_t_all_alternatives_empty() -> void
{
    // { display-name, value_t } table covering every variant alternative with
    // representative values (zero, one, max, negative, fractional).
    struct alt_case
    {
        const char* name;
        value_t     value;
    };

    const alt_case cases[]{
        { "bool_false",   value_t{ false } },
        { "bool_true",    value_t{ true } },                                 // -> compressed OOP 1
        { "i8_zero",      value_t{ std::int8_t{ 0 } } },
        { "i8_max",       value_t{ std::int8_t{ 127 } } },
        { "i8_neg",       value_t{ std::int8_t{ -1 } } },                    // -> 0xFFFFFFFF after widening
        { "i16_one",      value_t{ std::int16_t{ 1 } } },
        { "i16_neg",      value_t{ std::int16_t{ -2 } } },
        { "i32_zero",     value_t{ std::int32_t{ 0 } } },
        { "i32_one",      value_t{ std::int32_t{ 1 } } },
        { "i32_big",      value_t{ std::int32_t{ 1 << 20 } } },
        { "i32_neg",      value_t{ std::int32_t{ -123456 } } },
        { "i64_zero",     value_t{ std::int64_t{ 0 } } },
        { "i64_small",    value_t{ std::int64_t{ 7 } } },
        { "i64_huge",     value_t{ std::int64_t{ 0x1'0000'0001LL } } },      // narrows to 1
        { "float_zero",   value_t{ float{ 0.0F } } },
        { "float_frac",   value_t{ float{ 2.5F } } },                        // static_cast -> 2
        { "double_zero",  value_t{ double{ 0.0 } } },
        { "double_frac",  value_t{ double{ 3.9 } } },                        // static_cast -> 3
        { "u16_zero",     value_t{ std::uint16_t{ 0 } } },
        { "u16_max",      value_t{ std::uint16_t{ 0xFFFF } } },
        { "u32_zero",     value_t{ std::uint32_t{ 0 } } },
        { "u32_one",      value_t{ std::uint32_t{ 1 } } },
        { "u32_max",      value_t{ std::uint32_t{ 0xFFFFFFFFu } } },
        { "u32_midheap",  value_t{ std::uint32_t{ 0x0080'0000u } } },        // plausible compressed oop, still nullptr w/o JVM
    };

    for (const auto& tc : cases)
    {
        std::string n_vec{ "alt_to_vector_empty_" };
        n_vec += tc.name;
        std::string n_ent{ "alt_to_entries_empty_" };
        n_ent += tc.name;
        check(n_vec.c_str(), tc.value.to_vector<elem_w>().empty());
        check(n_ent.c_str(), (tc.value.to_entries<key_w, val_w>().empty()));
    }

    // is_reference() is true IFF the stored alternative is uint32 — pin a few.
    check("bool_alt_not_reference",  !value_t{ false }.is_reference());
    check("i32_alt_not_reference",   !value_t{ std::int32_t{ 5 } }.is_reference());
    check("double_alt_not_reference",!value_t{ double{ 1.0 } }.is_reference());
    check("u32_alt_is_reference",    value_t{ std::uint32_t{ 0 } }.is_reference());
    check("u32_alt_one_is_reference",value_t{ std::uint32_t{ 1 } }.is_reference());
}

// ---------------------------------------------------------------------------
// 7c. The '[L...;' / '[[...' raw-object-array branch of value_t::to_vector.
//
// value_t::to_vector special-cases an OBJECT-ARRAY field (signature begins
// "[L" or "[[") by walking the backing array DIRECTLY instead of routing an
// ObjArrayKlass through collection::to_vector (Fix #1, commit ec1c8a8).  This
// branch is the highest-value, most-recently-fixed code and was previously
// untested at the pure-logic level.  With no JVM, the backing-array OOP decodes
// to nullptr, so the branch returns empty WITHOUT faulting — pin the exact
// signature gate by walking a boundary matrix.
//
// Gate (from the header): signature.size() >= 2 && signature.front() == '['
//                         && (signature[1] == 'L' || signature[1] == '[').
// Whether a signature TAKES the array branch or falls through to
// collection::to_vector, the no-JVM result is identical (empty) — what we pin
// here is that NEITHER path faults and BOTH return empty across the matrix.
// ---------------------------------------------------------------------------
static auto test_value_t_array_signature_matrix() -> void
{
    // Use a NON-ZERO but invalid compressed OOP so to_vector gets PAST the
    // "compressed == 0" early-out and actually reaches the signature branch /
    // collection delegation, where is_valid_pointer / decode reject it.  0x7 is
    // below user_address_floor on every platform -> guaranteed invalid.
    constexpr std::uint32_t bogus_oop{ 0x7u };

    struct sig_case
    {
        const char* name;
        const char* signature;
    };

    const sig_case cases[]{
        // --- Take the OBJECT-ARRAY branch (front '[' and sig[1] in {L,[}). ---
        { "arr_object",        "[Ljava/lang/Object;" },
        { "arr_string",        "[Ljava/lang/String;" },
        { "arr_list",          "[Ljava/util/List;" },
        { "arr_2d_int",        "[[I" },
        { "arr_2d_string",     "[[Ljava/lang/String;" },
        { "arr_3d_object",     "[[[Ljava/lang/Object;" },
        // --- Do NOT take the array branch: primitive arrays (sig[1] not L/[). ---
        { "arr_prim_int",      "[I" },
        { "arr_prim_bool",     "[Z" },
        { "arr_prim_byte",     "[B" },
        { "arr_prim_double",   "[D" },
        { "arr_prim_long",     "[J" },
        { "arr_prim_char",     "[C" },
        { "arr_prim_float",    "[F" },
        { "arr_prim_short",    "[S" },
        // --- Reference / scalar signatures (front not '['): fall through. ---
        { "ref_list",          "Ljava/util/List;" },
        { "ref_map",           "Ljava/util/Map;" },
        { "ref_object",        "Ljava/lang/Object;" },
        { "scalar_int",        "I" },
        { "scalar_bool",       "Z" },
        // --- Degenerate signatures exercising the size() >= 2 guard. ---
        { "sig_empty",         "" },          // size 0  -> not array, not 'L'-ref
        { "sig_bracket_only",  "[" },         // size 1  -> fails the >= 2 guard
        { "sig_L_only",        "L" },         // size 1  -> 'L'-ish but truncated
    };

    for (const auto& tc : cases)
    {
        value_t v{ std::uint32_t{ bogus_oop }, std::string{ tc.signature } };

        std::string n_vec{ "arrsig_to_vector_empty_" };
        n_vec += tc.name;
        std::string n_ent{ "arrsig_to_entries_empty_" };
        n_ent += tc.name;

        // to_vector: array branch (if taken) walks a nullptr backing array ->
        // empty; otherwise collection::to_vector rejects the invalid oop ->
        // empty.  Either way: empty, no fault.
        check(n_vec.c_str(), v.to_vector<elem_w>().empty());
        // to_entries ignores the signature entirely (it always tries map) and
        // rejects the invalid oop -> empty.
        check(n_ent.c_str(), (v.to_entries<key_w, val_w>().empty()));
    }

    // Pin the EXACT gate predicate against the same inputs the header uses, so
    // a regression that loosened/tightened it (e.g. accepting "[I" or rejecting
    // "[[I") is caught even though the no-JVM RESULT is empty either way.
    auto takes_array_branch = [](std::string_view s) noexcept -> bool
    {
        return s.size() >= 2u && s.front() == '[' && (s[1] == 'L' || s[1] == '[');
    };
    check("gate_arr_object_yes",   takes_array_branch("[Ljava/lang/Object;"));
    check("gate_arr_2d_int_yes",   takes_array_branch("[[I"));
    check("gate_arr_2d_string_yes",takes_array_branch("[[Ljava/lang/String;"));
    check("gate_prim_int_no",      !takes_array_branch("[I"));
    check("gate_prim_bool_no",     !takes_array_branch("[Z"));
    check("gate_ref_list_no",      !takes_array_branch("Ljava/util/List;"));
    check("gate_scalar_int_no",    !takes_array_branch("I"));
    check("gate_empty_no",         !takes_array_branch(""));
    check("gate_bracket_only_no",  !takes_array_branch("["));
    check("gate_L_only_no",        !takes_array_branch("L"));
}

// ---------------------------------------------------------------------------
// 7d. Signatures with embedded NUL / non-ASCII bytes do not break the reads.
//
// value_t::to_vector inspects signature.front() and signature[1].  std::string
// is length-prefixed (NUL is a normal element), so an embedded NUL or a high
// byte must not be treated as a terminator or trip UB.  All still return empty
// on the null/invalid-oop path, and the gate predicate handles them by length.
// ---------------------------------------------------------------------------
static auto test_value_t_signature_robustness() -> void
{
    constexpr std::uint32_t bogus_oop{ 0x9u };

    // Embedded NUL in the middle: size() counts it; front()=='[' so the array
    // branch is taken, but the backing oop is invalid -> empty.
    {
        std::string sig{ "[L\0X;", 5 };   // 5 bytes incl. the interior NUL
        check("sig_embedded_nul_size5", sig.size() == 5u);
        value_t v{ std::uint32_t{ bogus_oop }, sig };
        check("sig_embedded_nul_to_vector_empty", v.to_vector<elem_w>().empty());
    }
    // Leading NUL: front() is '\0' (not '['), so it falls through to
    // collection::to_vector, which rejects the invalid oop -> empty.
    {
        std::string sig{ std::string(1, '\0') + "[Ljava/lang/Object;" };
        check("sig_leading_nul_front_not_bracket", sig.front() == '\0');
        value_t v{ std::uint32_t{ bogus_oop }, sig };
        check("sig_leading_nul_to_vector_empty", v.to_vector<elem_w>().empty());
    }
    // High / non-ASCII bytes only: front() is not '[', falls through -> empty.
    {
        std::string sig;
        sig.push_back(static_cast<char>(0xFF));
        sig.push_back(static_cast<char>(0x80));
        value_t v{ std::uint32_t{ bogus_oop }, sig };
        check("sig_high_bytes_to_vector_empty", v.to_vector<elem_w>().empty());
        check("sig_high_bytes_to_entries_empty", (v.to_entries<key_w, val_w>().empty()));
    }
    // A single '[' followed by a high byte: size()==2, front()=='[' but sig[1]
    // is neither 'L' nor '[' -> does NOT take the array branch -> empty.
    {
        std::string sig{ "[" };
        sig.push_back(static_cast<char>(0xC3));
        value_t v{ std::uint32_t{ bogus_oop }, sig };
        check("sig_bracket_highbyte_to_vector_empty", v.to_vector<elem_w>().empty());
    }
    // A very long all-'[' signature (size >> 2, sig[1]=='[') takes the array
    // branch; backing oop invalid -> empty.  Stresses that the gate only reads
    // the first two chars regardless of length.
    {
        const std::string sig(64, '[');
        value_t v{ std::uint32_t{ bogus_oop }, sig };
        check("sig_long_brackets_to_vector_empty", v.to_vector<elem_w>().empty());
    }
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
          (list_field.get().to_entries<key_w, val_w>().empty()));
    check("proxy_map_to_vector_empty",
          map_field.get().to_vector<elem_w>().empty());
}

// ---------------------------------------------------------------------------
// 8b. field_proxy::get() over EVERY JVM type descriptor -> value_t, then run
//     the collection entry points.  Always empty on the null-pointer path.
//
// field_proxy::get() with a null field_pointer returns value_t{ int32{0}, sig }
// REGARDLESS of the signature (it cannot read memory), so to_vector /
// to_entries see compressed OOP 0 -> empty.  We still drive every descriptor so
// the real user path is exercised for primitives ("Z".."C"), references
// ("L...;"), and arrays ("[...") alike — and assert as_string()/is_reference()
// behave on each.
// ---------------------------------------------------------------------------
static auto test_field_proxy_all_signatures_empty() -> void
{
    struct sig_case
    {
        const char* name;
        const char* signature;
    };

    const sig_case cases[]{
        { "Z", "Z" }, { "B", "B" }, { "S", "S" }, { "I", "I" },
        { "J", "J" }, { "F", "F" }, { "D", "D" }, { "C", "C" },
        { "ref_string", "Ljava/lang/String;" },
        { "ref_list",   "Ljava/util/List;" },
        { "ref_map",    "Ljava/util/Map;" },
        { "ref_set",    "Ljava/util/Set;" },
        { "arr_int",    "[I" },
        { "arr_object", "[Ljava/lang/Object;" },
        { "arr_2d_str", "[[Ljava/lang/String;" },
    };

    for (const auto& tc : cases)
    {
        vmhook::field_proxy proxy{ nullptr, tc.signature, false };
        const auto v{ proxy.get() };

        std::string n_vec{ "fp_to_vector_empty_" };
        n_vec += tc.name;
        std::string n_ent{ "fp_to_entries_empty_" };
        n_ent += tc.name;
        check(n_vec.c_str(), v.to_vector<elem_w>().empty());
        check(n_ent.c_str(), (v.to_entries<key_w, val_w>().empty()));

        // A null field_pointer yields the int32-zero alternative for EVERY
        // signature (get() short-circuits before the type dispatch), so the
        // value is never a "reference" and as_string() is empty.
        std::string n_ref{ "fp_not_reference_" };
        n_ref += tc.name;
        check(n_ref.c_str(), !v.is_reference());
        std::string n_str{ "fp_as_string_empty_" };
        n_str += tc.name;
        check(n_str.c_str(), v.as_string().empty());
    }

    // get() is documented noexcept; pin it (a throwing field read in a detour
    // would escape into the JVM).
    vmhook::field_proxy ref_proxy{ nullptr, "Ljava/util/List;", false };
    check("field_proxy_get_noexcept", noexcept(ref_proxy.get()));

    // A static-flagged proxy over a null pointer behaves identically (the
    // is_static flag does not change the null-pointer short-circuit in get()).
    vmhook::field_proxy static_proxy{ nullptr, "Ljava/util/Map;", true };
    check("static_proxy_to_entries_empty",
          (static_proxy.get().to_entries<key_w, val_w>().empty()));
    check("static_proxy_to_vector_empty",
          static_proxy.get().to_vector<elem_w>().empty());
}

// ---------------------------------------------------------------------------
// 8c. value_t conversion entry points are noexcept and the lvalue/rvalue and
//     repeated-call forms all return empty.
//
// to_vector / to_entries are const members usable on lvalues and rvalues; a
// value_t can be consumed repeatedly with identical (empty) results on the
// null path.  Pin that a fresh aggregate and a get()-produced value behave the
// same, and that calling twice does not corrupt state.
// ---------------------------------------------------------------------------
static auto test_value_t_call_forms() -> void
{
    // Rvalue form.
    check("rvalue_value_t_to_vector_empty",
          value_t{}.to_vector<elem_w>().empty());
    check("rvalue_value_t_to_entries_empty",
          (value_t{ std::uint32_t{ 0 }, std::string{ "Ljava/util/Map;" } }
               .to_entries<key_w, val_w>().empty()));

    // Lvalue form, called twice — value_t is const-callable and stateless on
    // this path, so the second call matches the first.
    const value_t v{ std::uint32_t{ 0 }, std::string{ "Ljava/util/List;" } };
    const auto first{ v.to_vector<elem_w>() };
    const auto second{ v.to_vector<elem_w>() };
    check("value_t_to_vector_repeatable_first_empty",  first.empty());
    check("value_t_to_vector_repeatable_second_empty", second.empty());
    check("value_t_to_vector_repeatable_same_size",    first.size() == second.size());

    // signature is preserved verbatim by the aggregate (the gate reads it).
    check("value_t_signature_preserved",
          v.signature == "Ljava/util/List;");
    // The variant holds exactly the uint32 alternative we put in.
    check("value_t_holds_uint32_alternative",
          std::holds_alternative<std::uint32_t>(v.data));
}

// ---------------------------------------------------------------------------
// 7e. EXHAUSTIVE byte sweep of the value_t::to_vector OBJECT-ARRAY gate.
//
// This is the element TYPE-TAG mapping of THIS feature: given a field
// signature, value_t::to_vector classifies it into one of two "tags" —
//   * OBJECT-ARRAY  (walk the backing Object[] directly, Fix #1 ec1c8a8), or
//   * FALL-THROUGH  (route the OOP through collection::to_vector).
// The classifier (header, value_t::to_vector) is EXACTLY:
//     sig.size() >= 2 && sig.front() == '[' && (sig[1] == 'L' || sig[1] == '[')
// i.e. the element descriptor immediately after a single leading '[' must be
// 'L' (object element) or '[' (nested-array element); EVERY other element
// descriptor — all eight primitives Z/B/C/S/I/J/F/D and ANY unrecognised byte
// — is the FALL-THROUGH tag (no gap, no UB, a documented total default).
//
// With no JVM the runtime RESULT is empty on both tags (decode_oop_pointer of
// the backing OOP is always nullptr, so the array-walk body is never reached),
// so this sweep pins the SPEC of the gate exhaustively over the full input byte
// space — the no-gaps / default-tag / switch-completeness guarantee STEP 2
// asks for — while the never-throw/empty BEHAVIOUR is pinned by the curated
// to_vector calls at the end.  The predicate below is a verbatim copy of the
// header gate; freezing it here makes any future loosening (accepting "[I") or
// tightening (rejecting "[[I") a loud, reviewable test diff.
// ---------------------------------------------------------------------------
namespace
{
    // Verbatim mirror of the header's value_t::to_vector object-array gate.
    constexpr auto value_t_takes_array_branch(std::string_view sig) noexcept -> bool
    {
        return sig.size() >= 2u && sig.front() == '[' && (sig[1] == 'L' || sig[1] == '[');
    }
}

static auto test_value_t_array_gate_byte_sweep() -> void
{
    // --- (a) SECOND byte sweep, first byte fixed '[': "[<byte>" over 0..255. ---
    // The array branch is taken IFF the element descriptor is 'L' or '['.
    {
        bool all_correct{ true };
        int  yes_count{ 0 };
        for (int b{ 0 }; b < 256; ++b)
        {
            char buf[2]{ '[', static_cast<char>(b) };
            const std::string_view sig{ buf, 2 };
            const bool taken{ value_t_takes_array_branch(sig) };
            const bool expected{ (b == 'L') || (b == '[') };
            if (taken != expected) { all_correct = false; }
            if (taken) { ++yes_count; }
        }
        check("gate_second_byte_full_sweep_matches_LorBracket", all_correct);
        // EXACTLY two of the 256 element descriptors take the array branch.
        check("gate_second_byte_exactly_two_take_branch", yes_count == 2);
    }

    // --- (b) FIRST byte sweep, second byte fixed 'L': "<byte>L" over 0..255. ---
    // The array branch requires the leading byte to be EXACTLY '['.
    {
        bool all_correct{ true };
        int  yes_count{ 0 };
        for (int b{ 0 }; b < 256; ++b)
        {
            char buf[2]{ static_cast<char>(b), 'L' };
            const std::string_view sig{ buf, 2 };
            const bool taken{ value_t_takes_array_branch(sig) };
            const bool expected{ (b == '[') };
            if (taken != expected) { all_correct = false; }
            if (taken) { ++yes_count; }
        }
        check("gate_first_byte_full_sweep_matches_bracket", all_correct);
        check("gate_first_byte_exactly_one_takes_branch", yes_count == 1);
    }

    // --- (c) Every SINGLE-char signature (0..255) fails the size>=2 guard. ---
    // A length-1 signature can never be an array descriptor, even "[".
    {
        bool none_taken{ true };
        for (int b{ 0 }; b < 256; ++b)
        {
            const char one{ static_cast<char>(b) };
            const std::string_view sig{ &one, 1 };
            if (value_t_takes_array_branch(sig)) { none_taken = false; }
        }
        check("gate_no_single_char_signature_takes_branch", none_taken);
    }

    // --- (d) The empty signature fails the size>=2 guard. ---
    check("gate_empty_signature_no_branch", !value_t_takes_array_branch(std::string_view{}));
    check("gate_empty_cstr_signature_no_branch", !value_t_takes_array_branch(""));

    // --- (e) Element-descriptor classification tied to the shared BasicType
    //         table: every PRIMITIVE element ("[Z".."[D") is FALL-THROUGH and
    //         classifies as a primitive BasicType (4..11); 'L'/'[' are the only
    //         array-branch descriptors and classify as T_OBJECT(12)/T_ARRAY(13).
    //         Pins that the gate is STRICTER than sig_char_to_basic_type's
    //         T_OBJECT(12) fallback — it checks the literal 'L'/'[' bytes, not
    //         "classified as object".  Each is also a primitive ARRAY whose
    //         in-heap width is 0 (jvm_primitive_byte_width over the 2-char desc).
    {
        struct prim_elem { char code; int basic; };
        const prim_elem prims[]{
            { 'Z', 4 }, { 'C', 5 }, { 'F', 6 }, { 'D', 7 },
            { 'B', 8 }, { 'S', 9 }, { 'I', 10 }, { 'J', 11 },
        };
        bool all_prim_fall_through{ true };
        bool all_prim_basic_ok{ true };
        for (const auto& p : prims)
        {
            char buf[2]{ '[', p.code };
            const std::string_view sig{ buf, 2 };
            if (value_t_takes_array_branch(sig)) { all_prim_fall_through = false; }
            if (vmhook::detail::sig_char_to_basic_type(p.code) != p.basic) { all_prim_basic_ok = false; }
        }
        check("gate_all_primitive_element_descriptors_fall_through", all_prim_fall_through);
        check("gate_primitive_element_basic_types_match_table", all_prim_basic_ok);

        // 'L' (object) and '[' (array) are the array-branch element descriptors.
        check("gate_object_element_L_takes_branch", value_t_takes_array_branch("[L"));
        check("gate_array_element_bracket_takes_branch", value_t_takes_array_branch("[["));
        check("basic_type_L_is_T_OBJECT_12", vmhook::detail::sig_char_to_basic_type('L') == 12);
        check("basic_type_bracket_is_T_ARRAY_13", vmhook::detail::sig_char_to_basic_type('[') == 13);
        // A String[] is NOT distinguished from any other Object[] by the gate:
        // it reads s[1]=='L' only — the class name is irrelevant.
        check("gate_string_array_same_as_object_array",
              value_t_takes_array_branch("[Ljava/lang/String;")
              && value_t_takes_array_branch("[Ljava/lang/Object;"));
    }

    // --- (f) BEHAVIOUR: across a representative signature matrix (object-array,
    //         nested-array, every primitive-array, reference, scalar, degenerate)
    //         BOTH to_vector and to_entries return empty and never fault, for a
    //         zero OOP AND a non-zero-but-invalid OOP (which gets past the
    //         compressed==0 early-out so the signature branch is reached).
    {
        const char* const sigs[]{
            "[Ljava/lang/Object;", "[Ljava/lang/String;", "[Ljava/util/List;",
            "[[I", "[[Ljava/lang/String;", "[[[Ljava/lang/Object;",
            "[Z", "[B", "[S", "[I", "[J", "[F", "[D", "[C",
            "Ljava/util/List;", "Ljava/util/Map;", "Ljava/util/Set;", "Ljava/lang/Object;",
            "I", "Z", "J", "D", "", "[", "L",
        };
        const std::uint32_t oops[]{ 0u, 0x7u };
        bool all_empty{ true };
        for (const char* s : sigs)
        {
            for (const std::uint32_t o : oops)
            {
                const value_t v{ std::uint32_t{ o }, std::string{ s } };
                if (!v.to_vector<elem_w>().empty()) { all_empty = false; }
                if (!v.to_entries<key_w, val_w>().empty()) { all_empty = false; }
            }
        }
        check("gate_behaviour_matrix_all_empty_no_fault", all_empty);
    }
}

// ---------------------------------------------------------------------------
// 7f. value_t variant alternative completeness (the "covers every enumerator"
//     exhaustiveness guard for the reference/primitive slot the mapping reads).
//
// to_vector / to_entries read the stored OOP via static_cast<uint32_t>(*this),
// which std::visits EVERY variant alternative.  These compile-time assertions
// pin the variant's exact shape — count, order, and per-index type — so a new
// alternative added (or one reordered) without updating the to_vector cast path
// is caught at compile time rather than silently mis-decoding a field.
// ---------------------------------------------------------------------------
static auto test_value_t_variant_completeness() -> void
{
    using variant_t = decltype(value_t::data);

    // EXACTLY nine alternatives — the JVM primitive set plus the compressed-OOP
    // reference slot.  A 10th alternative must come with a conscious test update.
    static_assert(std::variant_size_v<variant_t> == 9,
                  "value_t variant must have exactly 9 alternatives "
                  "(bool,i8,i16,i32,i64,float,double,u16,u32).");

    // Per-index type pin (the documented order at vmhook.hpp value_t struct).
    static_assert(std::is_same_v<std::variant_alternative_t<0, variant_t>, bool>);
    static_assert(std::is_same_v<std::variant_alternative_t<1, variant_t>, std::int8_t>);
    static_assert(std::is_same_v<std::variant_alternative_t<2, variant_t>, std::int16_t>);
    static_assert(std::is_same_v<std::variant_alternative_t<3, variant_t>, std::int32_t>);
    static_assert(std::is_same_v<std::variant_alternative_t<4, variant_t>, std::int64_t>);
    static_assert(std::is_same_v<std::variant_alternative_t<5, variant_t>, float>);
    static_assert(std::is_same_v<std::variant_alternative_t<6, variant_t>, double>);
    static_assert(std::is_same_v<std::variant_alternative_t<7, variant_t>, std::uint16_t>);
    static_assert(std::is_same_v<std::variant_alternative_t<8, variant_t>, std::uint32_t>,
                  "alternative 8 is the compressed-OOP reference slot to_vector/to_entries read.");

    // The conversion the entry points rely on (value_t -> uint32) exists and is
    // noexcept (a throwing field conversion in a detour would escape into the JVM).
    static_assert(std::is_convertible_v<value_t, std::uint32_t>,
                  "value_t must convert to the compressed-OOP uint32 slot.");
    check("value_t_to_uint32_noexcept", noexcept(static_cast<std::uint32_t>(std::declval<value_t>())));

    // RUNTIME: is_reference() partitions the alternatives — EXACTLY the uint32
    // alternative is a reference; all eight others are not.  Built one value_t
    // per alternative; count must be exactly one true.
    const value_t one_per_alt[]{
        value_t{ false },
        value_t{ std::int8_t{ 1 } },
        value_t{ std::int16_t{ 1 } },
        value_t{ std::int32_t{ 1 } },
        value_t{ std::int64_t{ 1 } },
        value_t{ float{ 1.0F } },
        value_t{ double{ 1.0 } },
        value_t{ std::uint16_t{ 1 } },
        value_t{ std::uint32_t{ 1 } },
    };
    int reference_count{ 0 };
    bool all_empty{ true };
    for (const auto& v : one_per_alt)
    {
        if (v.is_reference()) { ++reference_count; }
        // Whichever alternative is stored, both entry points are empty w/o JVM.
        if (!v.to_vector<elem_w>().empty()) { all_empty = false; }
        if (!v.to_entries<key_w, val_w>().empty()) { all_empty = false; }
    }
    check("value_t_exactly_one_alternative_is_reference", reference_count == 1);
    check("value_t_every_alternative_to_vector_entries_empty", all_empty);
}

// ---------------------------------------------------------------------------
// 7g. Compressed-OOP value sweep: EVERY representative point of the 32-bit
//     compressed-OOP space (held in the uint32 reference slot) decodes to
//     nullptr w/o a JVM, so to_vector / to_entries are empty for all of them —
//     across an object-array signature (reaches the gate) and a reference
//     signature (delegates to collection::to_vector / map::to_entries).
//
// This pins that is_valid_pointer / decode_oop_pointer reject the WHOLE
// compressed range without a JVM (the determinism the file header relies on):
// no compressed value — not 1, not a plausible mid-heap offset, not 0xFFFFFFFF
// — is ever mistaken for a live element/entry, so no walk is ever entered.
//
// NOTE: this sweep deliberately exercises ONLY the value_t entry points, which
// route the stored uint32 through decode_oop_pointer() (always nullptr w/o a
// JVM) — never the DIRECT collection{oop}/map{oop} constructors.  Those treat
// their argument as an already-DECODED 64-bit instance pointer and dereference
// its object header, so a large 4 GB-ish value (e.g. 0xFFFFFFFF zero-extended)
// can pass is_valid_pointer and then fault on an unmapped read.  Direct-wrapper
// rejection is covered separately with clearly-out-of-range LOW pointers
// (0x4 / 0x6) in test_to_vector_empty_no_jvm / test_to_entries_empty_no_jvm.
// ---------------------------------------------------------------------------
static auto test_value_t_compressed_oop_value_sweep() -> void
{
    const std::uint32_t oops[]{
        0x00000000u, 0x00000001u, 0x00000002u, 0x00000003u, 0x00000004u,
        0x00000007u, 0x00000008u, 0x000000FFu, 0x0000FFFFu, 0x00010000u,
        0x00080000u, 0x00800000u, 0x01000000u, 0x10000000u,
        0x7FFFFFFFu, 0x80000000u, 0xC0000000u, 0xFFFFFFFEu, 0xFFFFFFFFu,
    };
    const char* const sigs[]{ "[Ljava/lang/Object;", "Ljava/util/List;", "Ljava/util/Map;" };

    bool all_vec_empty{ true };
    bool all_ent_empty{ true };
    for (const std::uint32_t o : oops)
    {
        for (const char* s : sigs)
        {
            const value_t v{ std::uint32_t{ o }, std::string{ s } };
            if (!v.to_vector<elem_w>().empty()) { all_vec_empty = false; }
            if (!v.to_entries<key_w, val_w>().empty()) { all_ent_empty = false; }
        }
    }
    check("compressed_oop_value_sweep_to_vector_all_empty", all_vec_empty);
    check("compressed_oop_value_sweep_to_entries_all_empty", all_ent_empty);
}

// ---------------------------------------------------------------------------
// 7h. Cross product: EVERY variant alternative × a spread of signatures.
//
// test_value_t_all_alternatives_empty fixes the signature empty; the array
// matrix fixes the alternative to uint32.  This closes the gap between them:
// a non-uint32 alternative carrying an OBJECT-ARRAY signature still reaches the
// gate via static_cast<uint32_t>(*this) (which narrows e.g. bool true -> 1) and
// must come back empty — the alternative and the signature are independent
// inputs to the same never-throw contract.
// ---------------------------------------------------------------------------
static auto test_value_t_alternative_signature_cross() -> void
{
    const value_t alts[]{
        value_t{ true },                       // -> compressed OOP 1
        value_t{ std::int8_t{ -1 } },          // -> 0xFFFFFFFF widened
        value_t{ std::int32_t{ 1 << 20 } },
        value_t{ std::int64_t{ 0x1'0000'0001LL } }, // narrows to 1
        value_t{ double{ 3.9 } },              // static_cast -> 3
        value_t{ std::uint16_t{ 0xFFFF } },
        value_t{ std::uint32_t{ 0x00800000u } },
    };
    const char* const sigs[]{
        "[Ljava/lang/Object;",  // object-array  -> reaches the gate
        "[[Ljava/lang/String;", // nested-array  -> reaches the gate
        "[I",                   // primitive-array -> fall-through
        "Ljava/util/List;",     // reference     -> collection::to_vector
        "Ljava/util/Map;",      // reference     -> map::to_entries
        "I",                    // scalar
        "",                     // empty signature
    };

    bool all_empty{ true };
    for (const auto& base : alts)
    {
        for (const char* s : sigs)
        {
            // Re-stamp the alternative with this signature (aggregate copy of the
            // variant + an explicit signature).
            value_t v{ base.data, std::string{ s } };
            if (!v.to_vector<elem_w>().empty()) { all_empty = false; }
            if (!v.to_entries<key_w, val_w>().empty()) { all_empty = false; }
        }
    }
    check("alternative_signature_cross_all_empty", all_empty);
}

// ---------------------------------------------------------------------------
// 8d. Total, disjoint partition of the six container tags across the two
//     branches (the exhaustiveness guard for the tag lattice itself).
//
// Every concrete container tag is on EXACTLY ONE side of the hierarchy:
// collection-side (collection/list/set/linked_list) XOR map-side (map/hash_map).
// is_base_of pairs are checked elsewhere; this pins the PARTITION is total and
// disjoint — no tag is on both sides, none is on neither — so a future tag that
// accidentally bridged the branches (deriving from both) would flip a side
// count and fail here.
// ---------------------------------------------------------------------------
template<typename tag>
static constexpr bool is_collection_side_v =
    std::is_base_of_v<vmhook::collection, tag> && !std::is_base_of_v<vmhook::map, tag>;
template<typename tag>
static constexpr bool is_map_side_v =
    std::is_base_of_v<vmhook::map, tag> && !std::is_base_of_v<vmhook::collection, tag>;

static auto test_tag_lattice_total_partition() -> void
{
    // Each tag is on exactly one side (collection XOR map), never both/neither.
    static_assert(is_collection_side_v<vmhook::collection>  && !is_map_side_v<vmhook::collection>);
    static_assert(is_collection_side_v<vmhook::list>        && !is_map_side_v<vmhook::list>);
    static_assert(is_collection_side_v<vmhook::set>         && !is_map_side_v<vmhook::set>);
    static_assert(is_collection_side_v<vmhook::linked_list> && !is_map_side_v<vmhook::linked_list>);
    static_assert(is_map_side_v<vmhook::map>                && !is_collection_side_v<vmhook::map>);
    static_assert(is_map_side_v<vmhook::hash_map>           && !is_collection_side_v<vmhook::hash_map>);

    // Count form: exactly 4 collection-side, exactly 2 map-side, summing to all 6.
    constexpr int collection_side{
        static_cast<int>(is_collection_side_v<vmhook::collection>)
        + static_cast<int>(is_collection_side_v<vmhook::list>)
        + static_cast<int>(is_collection_side_v<vmhook::set>)
        + static_cast<int>(is_collection_side_v<vmhook::linked_list>)
        + static_cast<int>(is_collection_side_v<vmhook::map>)
        + static_cast<int>(is_collection_side_v<vmhook::hash_map>) };
    constexpr int map_side{
        static_cast<int>(is_map_side_v<vmhook::collection>)
        + static_cast<int>(is_map_side_v<vmhook::list>)
        + static_cast<int>(is_map_side_v<vmhook::set>)
        + static_cast<int>(is_map_side_v<vmhook::linked_list>)
        + static_cast<int>(is_map_side_v<vmhook::map>)
        + static_cast<int>(is_map_side_v<vmhook::hash_map>) };
    static_assert(collection_side == 4, "exactly 4 collection-side container tags.");
    static_assert(map_side == 2, "exactly 2 map-side container tags.");
    static_assert(collection_side + map_side == 6, "the partition covers all six tags exactly once.");

    check("tag_lattice_partition_collection_side_is_4", collection_side == 4);
    check("tag_lattice_partition_map_side_is_2", map_side == 2);
    check("tag_lattice_partition_total_is_6", (collection_side + map_side) == 6);
}

// ---------------------------------------------------------------------------
// 9. decode_oop_pointer() returns nullptr for the ENTIRE compressed-OOP value
//    space with no JVM loaded — the determinism cornerstone of this whole file.
//
// decode_oop_pointer(0) short-circuits to nullptr; every NON-zero value needs
// CompressedOops::_narrow_oop.{_base,_shift} resolved from the live JVM's
// gHotSpotVMStructs.  With no jvm.dll/libjvm.so in this process,
// resolve_struct_entry() returns nullptr, so decode_oop_pointer() returns
// nullptr for EVERY input.  That single fact is WHY value_t::to_vector /
// to_entries (which decode the stored uint32) are empty for any stored OOP —
// pin it directly across a representative spread of the 32-bit space so a
// regression that started returning a non-null decode without a JVM (and thus
// fed a wild pointer into the walks) would fail loudly here, not deep in a walk.
// ---------------------------------------------------------------------------
static auto test_decode_oop_pointer_all_null_no_jvm() -> void
{
    const std::uint32_t compressed_values[]{
        0x00000000u, 0x00000001u, 0x00000002u, 0x00000003u, 0x00000004u,
        0x00000007u, 0x00000008u, 0x0000000Fu, 0x000000FFu, 0x00000100u,
        0x0000FFFFu, 0x00010000u, 0x00080000u, 0x00800000u, 0x01000000u,
        0x08000000u, 0x10000000u, 0x40000000u, 0x7FFFFFFFu, 0x80000000u,
        0xAAAAAAAAu, 0xC0000000u, 0xDEADBEEFu, 0xFEEEFEEEu, 0xFFFFFFFEu,
        0xFFFFFFFFu,
    };
    bool all_null{ true };
    for (const std::uint32_t c : compressed_values)
    {
        if (vmhook::hotspot::decode_oop_pointer(c) != nullptr) { all_null = false; }
    }
    check("decode_oop_pointer_all_values_null_no_jvm", all_null);
    // Pin the two ends explicitly so the boundary cases are individually visible.
    check("decode_oop_pointer_zero_null",     vmhook::hotspot::decode_oop_pointer(0u) == nullptr);
    check("decode_oop_pointer_one_null",      vmhook::hotspot::decode_oop_pointer(1u) == nullptr);
    check("decode_oop_pointer_max_null",      vmhook::hotspot::decode_oop_pointer(0xFFFFFFFFu) == nullptr);
    // decode_oop_pointer is documented noexcept (it runs on detour threads).
    check("decode_oop_pointer_noexcept",      noexcept(vmhook::hotspot::decode_oop_pointer(0u)));

    // decode_array_oop (used by the value_t object-array branch) is the same
    // story: nullptr for every compressed value without a JVM, so the array walk
    // body is never entered and the branch returns empty.
    bool all_array_null{ true };
    for (const std::uint32_t c : compressed_values)
    {
        if (vmhook::decode_array_oop(c) != nullptr) { all_array_null = false; }
    }
    check("decode_array_oop_all_values_null_no_jvm", all_array_null);
}

// ---------------------------------------------------------------------------
// 10. is_valid_pointer() boundary behaviour — the SECOND independent gate that
//     makes every wrapper inert without a JVM (and rejects torn/garbage oops
//     even WITH one).  Pure address arithmetic, fully deterministic, no JVM.
//
// is_valid_pointer rejects: addr <= user_address_floor (0xFFFF), addr >=
// user_address_ceiling (0x00007FFFFFFFFFFF), odd addresses (low bit set), and a
// fixed set of debug-fill / freed-block sentinels matched on the low 32 bits.
// Everything in [floor+1, ceiling), even-aligned, non-sentinel, is accepted.
// These are exactly the rules that turn the bogus 0x4 / 0x6 / 0x7 oops the other
// sections use into "rejected -> empty".  Pinning the gate here proves the
// empty results are EARNED by a real rejection, not an accident of some other
// short-circuit.
// ---------------------------------------------------------------------------
static auto test_is_valid_pointer_boundaries() -> void
{
    auto ivp = [](std::uintptr_t a) noexcept -> bool
    {
        return vmhook::hotspot::is_valid_pointer(reinterpret_cast<const void*>(a));
    };

    // --- Below / at the floor (0xFFFF): always rejected. ---
    check("ivp_null_rejected",        !ivp(0x0u));
    check("ivp_0x4_rejected",         !ivp(0x4u));        // used as a bogus oop elsewhere
    check("ivp_0x6_rejected",         !ivp(0x6u));
    check("ivp_0x8_rejected",         !ivp(0x8u));
    check("ivp_floor_value_rejected", !ivp(0xFFFFu));     // addr == floor -> rejected (<=)
    check("ivp_just_below_floor_rejected", !ivp(0xFFFEu));

    // --- At / above the ceiling: rejected.  The ceiling (0x7FFF'FFFFFFFF)
    //     only fits a 64-bit pointer, so probe it sizeof-branched. ---
    if constexpr (sizeof(void*) >= 8)
    {
        check("ivp_ceiling_value_rejected",
              !ivp(static_cast<std::uintptr_t>(0x00007FFFFFFFFFFFull)));
    }
    // (We do NOT probe values strictly above the ceiling as raw addresses to
    //  avoid any platform's pointer-canonicalisation surprises; the ceiling
    //  itself is rejected by the >= comparison, which is the documented edge.)

    // --- Odd addresses (low bit set) above the floor: rejected. ---
    check("ivp_odd_address_rejected", !ivp(0x10001u));    // just above floor, odd
    check("ivp_odd_large_rejected",   !ivp(0x10000001u));

    // --- Debug-fill / freed-block sentinels (matched on the low 32 bits). ---
    // The header rejects a pointer whose LOW 32 BITS exactly equal one of these
    // (after the floor + odd-address gates).  To prove a sentinel is rejected we
    // must keep its low 32 bits INTACT (so the switch can see them) and lift the
    // address above the floor using a HIGH bit (bit 32) only — never touching
    // low32.  On a 64-bit pointer that yields an in-range, possibly-odd address
    // whose low32 IS the sentinel.  Even sentinels (0xCAFEBABE, 0xCCCCCCCC,
    // 0xFEEEFEEE) are then rejected by the switch; odd sentinels (0xDEADBEEF,
    // 0xBAADF00D, 0xCDCDCDCD, 0xABABABAB, 0xFDFDFDFD, 0xDDDDDDDD) are rejected by
    // the earlier odd-address rule.  Either way the assertion below ("rejected")
    // holds for all nine; the even ones additionally prove the switch fires.
    // (On a 32-bit pointer platform we cannot add a high bit without altering
    //  low32, so guard the high-bit construction on a 64-bit pointer width.)
    if constexpr (sizeof(void*) >= 8)
    {
        auto above_floor_keep_low32 = [](std::uint32_t low32) noexcept -> std::uintptr_t
        {
            // Set bit 32 to clear the floor; low 32 bits stay exactly the sentinel.
            return (std::uintptr_t{ 1 } << 32) | static_cast<std::uintptr_t>(low32);
        };
        const std::uint32_t sentinels[]{
            0xDEADBEEFu, 0xCAFEBABEu, 0xCCCCCCCCu, 0xCDCDCDCDu, 0xBAADF00Du,
            0xFEEEFEEEu, 0xABABABABu, 0xFDFDFDFDu, 0xDDDDDDDDu,
        };
        bool all_sentinels_rejected{ true };
        for (const std::uint32_t s : sentinels)
        {
            if (ivp(above_floor_keep_low32(s))) { all_sentinels_rejected = false; }
        }
        check("ivp_all_debug_sentinels_rejected", all_sentinels_rejected);

        // The three EVEN sentinels are rejected even though they are even and in
        // range — proving the sentinel switch (not the floor/odd rule) fires.
        const std::uint32_t even_sentinels[]{ 0xCAFEBABEu, 0xCCCCCCCCu, 0xFEEEFEEEu };
        bool even_sentinels_rejected_by_switch{ true };
        for (const std::uint32_t s : even_sentinels)
        {
            const std::uintptr_t a{ above_floor_keep_low32(s) };
            // Confirm it is in-range + even (so ONLY the switch can reject it).
            const bool in_range_even{ a > vmhook::os::user_address_floor
                                      && a < vmhook::os::user_address_ceiling
                                      && (a & 0x1u) == 0u };
            if (!in_range_even || ivp(a)) { even_sentinels_rejected_by_switch = false; }
        }
        check("ivp_even_sentinels_rejected_by_switch", even_sentinels_rejected_by_switch);
    }

    // --- A plain even, in-range, non-sentinel address IS accepted. ---
    // This is the positive control: it proves the gate is not rejecting
    // EVERYTHING (which would make the empty results above meaningless).  These
    // are pure range checks — is_valid_pointer never dereferences the address.
    // 0x20000 fits in a 32-bit pointer, is even, above the floor, non-sentinel —
    // accepted on EVERY pointer width.
    check("ivp_small_even_above_floor_accepted", ivp(0x20000u));
    // The larger probes exceed 32 bits, so only assert them where pointers are
    // 64-bit (sizeof-branched per the cross-platform determinism rule).
    if constexpr (sizeof(void*) >= 8)
    {
        check("ivp_plain_inrange_even_accepted", ivp(0x0000000100000000ull));
        check("ivp_midrange_even_accepted", ivp(0x0000100000000000ull));
    }

    // is_valid_pointer is documented noexcept (called on detour threads).
    check("ivp_noexcept",
          noexcept(vmhook::hotspot::is_valid_pointer(static_cast<const void*>(nullptr))));

    // The two constants the gate keys off are exactly the documented values.
    check("user_address_floor_is_0xFFFF",
          vmhook::os::user_address_floor == static_cast<std::uintptr_t>(0xFFFFu));
    check("user_address_ceiling_is_canonical_user_max",
          vmhook::os::user_address_ceiling == static_cast<std::uintptr_t>(0x00007FFFFFFFFFFFull));
    check("user_address_floor_below_ceiling",
          vmhook::os::user_address_floor < vmhook::os::user_address_ceiling);
}

// ---------------------------------------------------------------------------
// 11. EXHAUSTIVE 256-byte sweep of detail::sig_char_to_basic_type — the JVM
//     type-descriptor -> HotSpot BasicType classifier that underlies how an
//     element descriptor is recognised.
//
// This is the "type -> tag" mapping at its most primitive: a single descriptor
// byte maps to exactly one HotSpot BasicType int.  STEP 2 of the task (every
// supported type maps to the right tag; unknown types fall to the documented
// default; the switch is complete with no gaps) is pinned here over the FULL
// input byte space:
//   Z->4 C->5 F->6 D->7 B->8 S->9 I->10 J->11 L->12 [->13 V->14, default->12.
// Note T_OBJECT(12) is BOTH the 'L' mapping AND the catch-all default — the
// classifier is total (never throws, never UB) and its "unknown" sentinel is a
// REAL BasicType (T_OBJECT), exactly as documented.  Stable across all JDKs.
// ---------------------------------------------------------------------------
static auto test_sig_char_to_basic_type_full_sweep() -> void
{
    using vmhook::detail::sig_char_to_basic_type;

    // Reference table for the eleven explicitly-mapped descriptor bytes.
    struct named { char c; int basic; };
    const named mapped[]{
        { 'Z', 4 }, { 'C', 5 }, { 'F', 6 }, { 'D', 7 }, { 'B', 8 },
        { 'S', 9 }, { 'I', 10 }, { 'J', 11 }, { 'L', 12 }, { '[', 13 },
        { 'V', 14 },
    };

    // (a) Each named descriptor maps to exactly its documented BasicType.
    {
        bool all_ok{ true };
        for (const auto& m : mapped)
        {
            if (sig_char_to_basic_type(m.c) != m.basic) { all_ok = false; }
        }
        check("sigchar_named_descriptors_map_exactly", all_ok);
    }

    // (b) FULL 0..255 sweep: every byte not in the named set returns the
    //     T_OBJECT(12) default; every named byte returns its own value.  This is
    //     the no-gaps / total-default guarantee over the entire input space.
    {
        bool all_ok{ true };
        int  default_count{ 0 };
        int  nondefault_count{ 0 };
        for (int b{ 0 }; b < 256; ++b)
        {
            const char c{ static_cast<char>(b) };
            int expected{ 12 };               // T_OBJECT fallback
            for (const auto& m : mapped)
            {
                if (m.c == c) { expected = m.basic; break; }
            }
            const int got{ sig_char_to_basic_type(c) };
            if (got != expected) { all_ok = false; }
            if (got == 12 && expected == 12 && c != 'L') { ++default_count; }
            if (expected != 12) { ++nondefault_count; }
        }
        check("sigchar_full_256_sweep_matches_table", all_ok);
        // Exactly ten descriptors map to a NON-T_OBJECT BasicType (Z C F D B S I
        // J [ V — eleven named minus 'L' which IS T_OBJECT).
        check("sigchar_exactly_ten_nondefault_descriptors", nondefault_count == 10);
        // Of 256 bytes, 'L' plus the 245 unnamed bytes resolve to T_OBJECT(12):
        // 256 - 10 nondefault - ('L' counted via default_count exclusion) ... pin
        // the unnamed-default population directly: 256 minus the 11 named = 245.
        check("sigchar_unnamed_bytes_all_default", default_count == 245);
    }

    // (c) Spot negative-space: ASCII letters NOT in the descriptor set and
    //     punctuation / digits all fall to T_OBJECT(12), never to a wrong real
    //     BasicType (e.g. 'i' lowercase must NOT be mistaken for 'I'->10).
    {
        const char junk[]{ 'A', 'E', 'G', 'H', 'K', 'M', 'N', 'O', 'P', 'Q',
                           'R', 'T', 'U', 'W', 'X', 'Y',
                           'a', 'i', 'j', 'l', 'z', 'v', 's', 'b',
                           '0', '9', ' ', '/', ';', '*', '\0', '@' };
        bool all_default{ true };
        for (const char c : junk)
        {
            if (sig_char_to_basic_type(c) != 12) { all_default = false; }
        }
        check("sigchar_junk_bytes_all_T_OBJECT", all_default);
        // Case sensitivity: lowercase letters never alias their uppercase
        // descriptor counterparts.
        check("sigchar_lowercase_i_not_int",  sig_char_to_basic_type('i') != 10);
        check("sigchar_lowercase_z_not_bool", sig_char_to_basic_type('z') != 4);
        check("sigchar_lowercase_l_is_default", sig_char_to_basic_type('l') == 12);
    }

    // (d) sig_char_to_basic_type is noexcept (used on detour threads).
    check("sigchar_noexcept", noexcept(sig_char_to_basic_type('I')));
}

// ---------------------------------------------------------------------------
// 12. EXHAUSTIVE matrix of detail::jvm_primitive_byte_width — the in-heap
//     width of a primitive field/array-element descriptor.
//
// This is the tag -> width round-trip helper the array-element-width guard in
// value_t::read_array_value keys off.  Contract (header): returns a non-zero
// width ONLY for a length-1 primitive descriptor; 0 for references, arrays,
// length != 1, and any unknown byte.
//   Z=1 B=1  S=2 C=2  I=4 F=4  J=8 D=8  ; everything else = 0.
// Sweep the full single-byte space AND the multi-char / empty cases so the
// "size != 1 -> 0" early-out and the per-byte table are both pinned exhaustively.
// ---------------------------------------------------------------------------
static auto test_jvm_primitive_byte_width_matrix() -> void
{
    using vmhook::detail::jvm_primitive_byte_width;

    struct wcase { char c; std::size_t width; };
    const wcase widths[]{
        { 'Z', 1 }, { 'B', 1 }, { 'S', 2 }, { 'C', 2 },
        { 'I', 4 }, { 'F', 4 }, { 'J', 8 }, { 'D', 8 },
    };

    // (a) Each primitive descriptor reports its exact JVM width.
    {
        bool all_ok{ true };
        for (const auto& w : widths)
        {
            const char buf[1]{ w.c };
            if (jvm_primitive_byte_width(std::string_view{ buf, 1 }) != w.width) { all_ok = false; }
        }
        check("primwidth_named_descriptors_exact", all_ok);
    }

    // (b) FULL 0..255 single-byte sweep: exactly the eight primitive descriptors
    //     are non-zero (with their documented widths); EVERY other byte is 0.
    {
        bool all_ok{ true };
        int  nonzero_count{ 0 };
        std::size_t width_sum{ 0 };
        for (int b{ 0 }; b < 256; ++b)
        {
            const char c{ static_cast<char>(b) };
            std::size_t expected{ 0 };
            for (const auto& w : widths)
            {
                if (w.c == c) { expected = w.width; break; }
            }
            const char buf[1]{ c };
            const std::size_t got{ jvm_primitive_byte_width(std::string_view{ buf, 1 }) };
            if (got != expected) { all_ok = false; }
            if (got != 0) { ++nonzero_count; width_sum += got; }
        }
        check("primwidth_full_256_sweep_matches_table", all_ok);
        check("primwidth_exactly_eight_nonzero", nonzero_count == 8);
        // 1+1+2+2+4+4+8+8 = 30 across the eight primitive descriptors.
        check("primwidth_widths_sum_is_30", width_sum == 30u);
    }

    // (c) size != 1 -> always 0: empty, two-char, full reference / array sigs.
    {
        check("primwidth_empty_is_zero",      jvm_primitive_byte_width(std::string_view{}) == 0u);
        check("primwidth_two_char_II_is_zero", jvm_primitive_byte_width(std::string_view{ "II" }) == 0u);
        check("primwidth_array_int_is_zero",   jvm_primitive_byte_width(std::string_view{ "[I" }) == 0u);
        check("primwidth_array_double_is_zero", jvm_primitive_byte_width(std::string_view{ "[D" }) == 0u);
        check("primwidth_ref_string_is_zero",
              jvm_primitive_byte_width(std::string_view{ "Ljava/lang/String;" }) == 0u);
        check("primwidth_object_descriptor_L_two_char_is_zero",
              jvm_primitive_byte_width(std::string_view{ "L;" }) == 0u);
    }

    // (d) Reference / array / void single bytes are 0 (NOT primitives), so the
    //     read-array width guard correctly skips them (width 0 == "don't gate").
    {
        check("primwidth_single_L_is_zero", jvm_primitive_byte_width(std::string_view{ "L" }) == 0u);
        check("primwidth_single_bracket_is_zero", jvm_primitive_byte_width(std::string_view{ "[" }) == 0u);
        check("primwidth_single_V_is_zero", jvm_primitive_byte_width(std::string_view{ "V" }) == 0u);
    }

    // (e) Cross-check: jvm_primitive_byte_width(c)!=0 IFF the element descriptor
    //     is a primitive (a fall-through for the object-array gate) AND its
    //     BasicType is in the primitive band [4,11].  Ties the two helpers
    //     together so a change to one without the other is caught.
    {
        bool consistent{ true };
        for (int b{ 0 }; b < 256; ++b)
        {
            const char c{ static_cast<char>(b) };
            const char buf[1]{ c };
            const bool has_width{ jvm_primitive_byte_width(std::string_view{ buf, 1 }) != 0u };
            const int  basic{ vmhook::detail::sig_char_to_basic_type(c) };
            const bool is_primitive_band{ basic >= 4 && basic <= 11 };
            // Every byte WITH a width is in the primitive band; the band byte 'V'
            // is excluded by jvm_primitive_byte_width (void has no storage), so
            // has_width implies primitive-band but NOT vice-versa (C/F/D/.. yes,
            // but every primitive-band char here DOES have a width — V is 14, not
            // in [4,11]).  So the relation is exactly: has_width == band.
            if (has_width != is_primitive_band) { consistent = false; }
        }
        check("primwidth_matches_basic_type_primitive_band", consistent);
    }

    // (f) jvm_primitive_byte_width is noexcept.  Build the string_view argument
    //     OUTSIDE the noexcept() operator: libc++ (macOS/iOS) does NOT mark
    //     string_view's (const char*) constructor noexcept, so constructing the
    //     argument inline poisons the measured expression even though the
    //     function itself is noexcept (libstdc++ does mark it, which is why this
    //     passed on Linux/Windows).  Measuring the call on a prebuilt lvalue
    //     (string_view's copy is trivially noexcept) isolates the function.
    {
        const std::string_view prim_desc{ "I" };
        check("primwidth_noexcept", noexcept(jvm_primitive_byte_width(prim_desc)));
    }
}

// ---------------------------------------------------------------------------
// 13. clamp_safe_container_count — the constexpr "never over-allocate / never
//     over-read" guard applied to EVERY element count this feature derives from
//     heap memory (the ArrayList `size` field, the array_length of an Object[]
//     in the value_t array branch, and every walk helper's reserve/loop bound).
//
// Contract (header): clamp raw to [0, k_max_safe_container_elems] where the cap
// is 1<<24.  Negative / zero -> 0; raw < cap -> raw; raw >= cap -> cap.  The
// function is constexpr, so pin its full behaviour at COMPILE TIME with
// static_assert (the strongest possible guard) plus a runtime mirror.  This is
// the load-bearing safety invariant for the array-walk reserve at value_t::
// to_vector and for every walk helper — a regression that widened the cap or
// stopped clamping negatives would let a torn heap read drive an unbounded
// reserve/loop.
// ---------------------------------------------------------------------------
static auto test_clamp_safe_container_count() -> void
{
    using vmhook::clamp_safe_container_count;
    constexpr std::int32_t cap{ static_cast<std::int32_t>(vmhook::k_max_safe_container_elems) };

    // --- The cap is EXACTLY 1<<24 and fits comfortably in int32. ---
    static_assert(vmhook::k_max_safe_container_elems == (1ull << 24),
                  "k_max_safe_container_elems must be 1<<24 (16,777,216).");
    static_assert(cap == 16'777'216, "cap mirrors k_max_safe_container_elems as int32.");
    static_assert(cap > 0 && cap < (std::numeric_limits<std::int32_t>::max)(),
                  "cap must be a positive int32 with headroom.");

    // --- Negative and zero collapse to 0 (a torn _length read can be negative). ---
    static_assert(clamp_safe_container_count(0) == 0);
    static_assert(clamp_safe_container_count(-1) == 0);
    static_assert(clamp_safe_container_count(-12345) == 0);
    static_assert(clamp_safe_container_count((std::numeric_limits<std::int32_t>::min)()) == 0,
                  "INT_MIN clamps to 0, not to a wrapped positive.");

    // --- In-range values pass through unchanged. ---
    static_assert(clamp_safe_container_count(1) == 1);
    static_assert(clamp_safe_container_count(2) == 2);
    static_assert(clamp_safe_container_count(1000) == 1000);
    static_assert(clamp_safe_container_count(cap - 1) == cap - 1,
                  "the largest sub-cap value is returned verbatim.");

    // --- At and above the cap, the result saturates to the cap. ---
    static_assert(clamp_safe_container_count(cap) == cap,
                  "exactly the cap returns the cap.");
    static_assert(clamp_safe_container_count(cap + 1) == cap,
                  "one past the cap saturates.");
    static_assert(clamp_safe_container_count((std::numeric_limits<std::int32_t>::max)()) == cap,
                  "INT_MAX saturates to the cap, never overflows.");

    // --- The result is ALWAYS a valid reserve/loop bound: in [0, cap]. ---
    static_assert(clamp_safe_container_count(-999) >= 0 && clamp_safe_container_count(-999) <= cap);
    static_assert(clamp_safe_container_count(999'999'999) >= 0
                  && clamp_safe_container_count(999'999'999) <= cap);

    // --- clamp is idempotent: clamping a clamped value changes nothing. ---
    static_assert(clamp_safe_container_count(clamp_safe_container_count(cap + 5)) == cap);
    static_assert(clamp_safe_container_count(clamp_safe_container_count(-5)) == 0);
    static_assert(clamp_safe_container_count(clamp_safe_container_count(42)) == 42);

    // --- clamp is monotonic non-decreasing across the boundary samples. ---
    static_assert(clamp_safe_container_count(-1) <= clamp_safe_container_count(0));
    static_assert(clamp_safe_container_count(0) <= clamp_safe_container_count(1));
    static_assert(clamp_safe_container_count(cap - 1) <= clamp_safe_container_count(cap));
    static_assert(clamp_safe_container_count(cap) <= clamp_safe_container_count(cap + 1));

    // --- Runtime mirrors of the same facts (so they show up in the PASS count
    //     and a constexpr-evaluation quirk could never silently drop them). ---
    check("clamp_zero_is_zero",        clamp_safe_container_count(0) == 0);
    check("clamp_negative_is_zero",    clamp_safe_container_count(-1) == 0);
    check("clamp_int_min_is_zero",     clamp_safe_container_count((std::numeric_limits<std::int32_t>::min)()) == 0);
    check("clamp_one_passthrough",     clamp_safe_container_count(1) == 1);
    check("clamp_below_cap_passthrough", clamp_safe_container_count(cap - 1) == cap - 1);
    check("clamp_at_cap_saturates",    clamp_safe_container_count(cap) == cap);
    check("clamp_above_cap_saturates", clamp_safe_container_count(cap + 1) == cap);
    check("clamp_int_max_saturates",   clamp_safe_container_count((std::numeric_limits<std::int32_t>::max)()) == cap);
    check("clamp_result_in_bounds_for_huge",
          clamp_safe_container_count(2'000'000'000) >= 0
          && clamp_safe_container_count(2'000'000'000) <= cap);
    check("clamp_noexcept",            noexcept(clamp_safe_container_count(0)));
    check("clamp_is_constexpr_usable",
          std::integral_constant<std::int32_t, clamp_safe_container_count(5)>::value == 5);

    // --- FULL byte-boundary sweep around the cap: for every offset in a small
    //     window the result is min(max(raw,0), cap), matched against an
    //     independent reference implementation. ---
    {
        // NB: 'cap' is constexpr, so it is usable inside the lambda WITHOUT a
        // capture. Capturing it trips clang's -Wunused-lambda-capture (-Werror);
        // g++ stays silent, so leave the capture list empty for both.
        auto reference = [](std::int64_t raw) noexcept -> std::int32_t
        {
            if (raw <= 0) { return 0; }
            return static_cast<std::int32_t>(raw < cap ? raw : cap);
        };
        bool all_ok{ true };
        const std::int64_t probes[]{
            -3, -2, -1, 0, 1, 2, 3,
            static_cast<std::int64_t>(cap) - 2, static_cast<std::int64_t>(cap) - 1,
            static_cast<std::int64_t>(cap), static_cast<std::int64_t>(cap) + 1,
            static_cast<std::int64_t>(cap) + 2,
        };
        for (const std::int64_t p : probes)
        {
            if (clamp_safe_container_count(static_cast<std::int32_t>(p)) != reference(p)) { all_ok = false; }
        }
        check("clamp_window_matches_reference_impl", all_ok);
    }
}

// ---------------------------------------------------------------------------
// 14. detail::value_t_convertible_target_v — the constexpr gate on value_t's
//     conversion operator that the to_vector / to_entries entry points ride via
//     static_cast<std::uint32_t>(*this).
//
// The trait permits arithmetic / std::string / std::unique_ptr<W> / std::vector
// targets and exactly ONE pointer target (void*, the compressed-OOP decode
// slot), while excising std::nullptr_t and every non-void pointer (char*,
// const char*, W*).  to_vector/to_entries read the OOP as uint32 (an arithmetic
// target, permitted), so this trait must stay TRUE for uint32 or the entry
// points would not compile.  Pin the whole partition at compile time.
// ---------------------------------------------------------------------------
static auto test_value_t_convertible_target_gate() -> void
{
    using vmhook::detail::value_t_convertible_target_v;

    // --- The slot the entry points actually use. ---
    static_assert(value_t_convertible_target_v<std::uint32_t>,
                  "to_vector/to_entries read the OOP via static_cast<uint32_t> — must be permitted.");
    static_assert(value_t_convertible_target_v<void*>,
                  "void* is the single permitted pointer target (compressed-OOP decode).");

    // --- Arithmetic + bool targets: all permitted. ---
    static_assert(value_t_convertible_target_v<bool>);
    static_assert(value_t_convertible_target_v<std::int8_t>);
    static_assert(value_t_convertible_target_v<std::int16_t>);
    static_assert(value_t_convertible_target_v<std::int32_t>);
    static_assert(value_t_convertible_target_v<std::int64_t>);
    static_assert(value_t_convertible_target_v<std::uint16_t>);
    static_assert(value_t_convertible_target_v<float>);
    static_assert(value_t_convertible_target_v<double>);

    // --- Class targets used by the wrappers: permitted. ---
    static_assert(value_t_convertible_target_v<std::string>);
    static_assert(value_t_convertible_target_v<std::unique_ptr<elem_w>>);
    static_assert(value_t_convertible_target_v<std::vector<std::int32_t>>);
    static_assert(value_t_convertible_target_v<std::vector<std::string>>);

    // --- cv / ref qualified forms are classified by their underlying type. ---
    static_assert(value_t_convertible_target_v<const std::uint32_t&>);
    static_assert(value_t_convertible_target_v<const std::string&>);
    static_assert(value_t_convertible_target_v<std::unique_ptr<elem_w>&&>);
    static_assert(value_t_convertible_target_v<const void*>,
                  "cv-qualified void* is still the permitted pointer target.");

    // --- Excised targets: nullptr_t and every non-void pointer. ---
    static_assert(!value_t_convertible_target_v<std::nullptr_t>,
                  "nullptr_t is excised (collides with class-target ctors).");
    static_assert(!value_t_convertible_target_v<char*>);
    static_assert(!value_t_convertible_target_v<const char*>);
    static_assert(!value_t_convertible_target_v<elem_w*>,
                  "a raw wrapper pointer W* is excised (collides with unique_ptr<W> ctor).");
    static_assert(!value_t_convertible_target_v<int*>);
    static_assert(!value_t_convertible_target_v<const std::string*>);

    // --- Runtime mirror: the exact conversion the entry points perform exists,
    //     is noexcept, and yields 0 for the zero-OOP alternative. ---
    check("value_t_uint32_conversion_yields_zero_for_zero_oop",
          static_cast<std::uint32_t>(value_t{ std::uint32_t{ 0 } }) == 0u);
    check("value_t_uint32_conversion_roundtrips_value",
          static_cast<std::uint32_t>(value_t{ std::uint32_t{ 0x00ABCDEFu } }) == 0x00ABCDEFu);
    // bool true narrows to compressed OOP 1 (the value the array gate then
    // decodes to nullptr without a JVM).
    check("value_t_bool_true_narrows_to_one",
          static_cast<std::uint32_t>(value_t{ true }) == 1u);
    check("value_t_int8_neg_one_widens_to_max",
          static_cast<std::uint32_t>(value_t{ std::int8_t{ -1 } }) == 0xFFFFFFFFu);
    check("value_t_void_ptr_conversion_null_no_jvm",
          static_cast<void*>(value_t{ std::uint32_t{ 0x1234u } }) == nullptr);
}

// ---------------------------------------------------------------------------
// 15. The element / key / value wrapper TYPE CONTRACT that to_vector<E>() /
//     to_entries<K,V>() require: E/K/V must be constructible from vmhook::oop_t
//     (== void*), because the (runtime-unreached, but always instantiated)
//     bodies call std::make_unique<E>(oop_t).  This is exactly the type-tag
//     mapping's requirement on the element wrapper.
//
// Pin that the wrappers this test instantiates with satisfy the contract, that
// std::make_unique<E>(oop_t) is well-formed, and that the produced
// vector / pair element types are precisely the documented unique_ptr shapes.
// ---------------------------------------------------------------------------
static auto test_element_wrapper_contract() -> void
{
    // oop_t is void* — the element ctor argument type.
    static_assert(std::is_same_v<vmhook::oop_t, void*>,
                  "vmhook::oop_t is void*; element wrappers take it in their ctor.");

    // Each wrapper is constructible from an oop_t and from a decoded void*.
    static_assert(std::is_constructible_v<elem_w, vmhook::oop_t>);
    static_assert(std::is_constructible_v<key_w, vmhook::oop_t>);
    static_assert(std::is_constructible_v<val_w, vmhook::oop_t>);
    static_assert(std::is_constructible_v<elem_w, void*>);

    // std::make_unique<E>(oop_t) — the exact expression to_vector instantiates —
    // is well-formed for each wrapper (so the template body compiles).
    static_assert(requires { std::make_unique<elem_w>(std::declval<vmhook::oop_t>()); });
    static_assert(requires { std::make_unique<key_w>(std::declval<vmhook::oop_t>()); });
    static_assert(requires { std::make_unique<val_w>(std::declval<vmhook::oop_t>()); });

    // The wrapper ctors are noexcept (matching object_base's contract), so a
    // walk that constructs N of them on a detour thread cannot throw mid-walk.
    static_assert(std::is_nothrow_constructible_v<elem_w, vmhook::oop_t>);
    static_assert(std::is_nothrow_constructible_v<key_w, vmhook::oop_t>);

    // The produced container element types are EXACTLY the documented shapes.
    static_assert(std::is_same_v<
                      decltype(std::declval<vmhook::collection>().to_vector<elem_w>()),
                      std::vector<std::unique_ptr<elem_w>>>,
                  "collection::to_vector<E> -> vector<unique_ptr<E>>.");
    static_assert(std::is_same_v<
                      decltype(std::declval<vmhook::map>().to_entries<key_w, val_w>()),
                      std::vector<std::pair<std::unique_ptr<key_w>, std::unique_ptr<val_w>>>>,
                  "map::to_entries<K,V> -> vector<pair<unique_ptr<K>,unique_ptr<V>>>.");
    // The value_t delegators produce the identical shapes.
    static_assert(std::is_same_v<
                      decltype(std::declval<value_t>().to_vector<elem_w>()),
                      std::vector<std::unique_ptr<elem_w>>>);
    static_assert(std::is_same_v<
                      decltype(std::declval<value_t>().to_entries<key_w, val_w>()),
                      std::vector<std::pair<std::unique_ptr<key_w>, std::unique_ptr<val_w>>>>);

    // A constructed-from-bogus-but-decoded-null path: make_unique<E>(nullptr)
    // yields a non-null unique_ptr wrapping a null OOP (this is what a null Java
    // element would have become, but the walks emit nullptr SLOTS instead — pin
    // that the wrapper itself is still constructible from a null oop).
    {
        auto w{ std::make_unique<elem_w>(static_cast<vmhook::oop_t>(nullptr)) };
        check("make_unique_elem_w_from_null_oop_nonnull_ptr", w != nullptr);
        check("make_unique_elem_w_wraps_null_instance", w->get_instance() == nullptr);
    }

    // Runtime presence marker so this section contributes to the PASS count.
    check("element_wrapper_contract_static_asserts_held", true);
}

// ---------------------------------------------------------------------------
// 16. vmhook::array_length() — the in-heap element-count oracle the value_t
//     OBJECT-ARRAY branch and every collection walk read to bound their loop.
//
// Contract (header): returns 0 for a null OOP and for an invalid (out-of-range
// / odd / sentinel) OOP — it short-circuits BEFORE any memory read on
// `!array_oop || !is_valid_pointer(array_oop)`.  With no JVM we cannot supply a
// real mapped array, but the null + invalid-low-pointer rejection is pure
// address arithmetic and fully deterministic: every bogus OOP the rest of this
// file uses (0x4 / 0x6 / 0x7) is below user_address_floor, so array_length must
// report 0 — which is exactly why the object-array walk body is never entered
// without a JVM.  Pin it directly so a regression that started dereferencing an
// invalid pointer (instead of returning 0) would fail loudly here.
// ---------------------------------------------------------------------------
static auto test_array_length_null_and_invalid() -> void
{
    // Null -> 0 (the first short-circuit clause).
    check("array_length_null_zero", vmhook::array_length(nullptr) == 0);

    // Invalid LOW pointers (below user_address_floor 0xFFFF) -> 0 via the
    // is_valid_pointer clause, before any +12 header read.  These mirror the
    // 0x4 / 0x6 / 0x7 bogus oops the wrapper tests rely on.
    const std::uintptr_t low_invalid[]{ 0x1u, 0x2u, 0x4u, 0x6u, 0x7u, 0x8u, 0xFFFFu, 0xFFFEu };
    bool all_invalid_zero{ true };
    for (const std::uintptr_t a : low_invalid)
    {
        if (vmhook::array_length(reinterpret_cast<vmhook::oop_t>(a)) != 0) { all_invalid_zero = false; }
    }
    check("array_length_low_invalid_all_zero", all_invalid_zero);

    // An ODD in-range pointer is rejected by is_valid_pointer's alignment rule
    // -> 0, again with no header read.  0x10001 is just above the floor + odd.
    check("array_length_odd_pointer_zero",
          vmhook::array_length(reinterpret_cast<vmhook::oop_t>(static_cast<std::uintptr_t>(0x10001u))) == 0);

    // array_length is documented noexcept (it runs on detour threads).
    check("array_length_noexcept", noexcept(vmhook::array_length(nullptr)));

    // The clamp the walks apply to array_length's result keeps a (here always 0)
    // count a valid reserve/loop bound — 0 clamps to 0.
    check("array_length_zero_clamps_to_zero",
          vmhook::clamp_safe_container_count(vmhook::array_length(nullptr)) == 0);
}

// ---------------------------------------------------------------------------
// 17. vmhook::get_array_element<T>() — the per-element primitive read the
//     value_t primitive-array path (append_array_value) sits on.
//
// Contract (header): returns T{} for a null OOP, an invalid OOP, and any index
// < 0 or >= length.  Without a JVM array_length() is always 0, so EVERY index
// is out of bounds -> T{} for every element type.  Plus the null/invalid OOP
// guards fire before array_length is even consulted.  This is the element-level
// never-fault guarantee underneath the object-array / primitive-array tags.
// (T must be trivially copyable per the static_assert; we sweep the JVM
//  primitive C++ element types the append_array_value overloads instantiate.)
// ---------------------------------------------------------------------------
static auto test_get_array_element_null_and_oob() -> void
{
    const vmhook::oop_t null_oop{ nullptr };
    const vmhook::oop_t bogus_oop{ reinterpret_cast<vmhook::oop_t>(static_cast<std::uintptr_t>(0x7u)) };

    // Null OOP -> default for every element type the walks use.
    check("gae_null_bool_default",   vmhook::get_array_element<bool>(null_oop, 0) == false);
    check("gae_null_i8_default",     vmhook::get_array_element<std::int8_t>(null_oop, 0) == 0);
    check("gae_null_u8_default",     vmhook::get_array_element<std::uint8_t>(null_oop, 0) == 0);
    check("gae_null_i16_default",    vmhook::get_array_element<std::int16_t>(null_oop, 0) == 0);
    check("gae_null_u16_default",    vmhook::get_array_element<std::uint16_t>(null_oop, 0) == 0);
    check("gae_null_i32_default",    vmhook::get_array_element<std::int32_t>(null_oop, 0) == 0);
    check("gae_null_u32_default",    vmhook::get_array_element<std::uint32_t>(null_oop, 0) == 0u);
    check("gae_null_i64_default",    vmhook::get_array_element<std::int64_t>(null_oop, 0) == 0);
    check("gae_null_float_default",  vmhook::get_array_element<float>(null_oop, 0) == 0.0F);
    check("gae_null_double_default", vmhook::get_array_element<double>(null_oop, 0) == 0.0);
    check("gae_null_char_default",   vmhook::get_array_element<char>(null_oop, 0) == char{ 0 });

    // Invalid OOP -> default (rejected before the length read).
    check("gae_bogus_i32_default",  vmhook::get_array_element<std::int32_t>(bogus_oop, 0) == 0);
    check("gae_bogus_u32_default",  vmhook::get_array_element<std::uint32_t>(bogus_oop, 0) == 0u);

    // Negative and large indices on a null OOP are all default (the null guard
    // dominates; the index guard is the secondary line of defence).  Sweep a
    // representative index spread including the int32 extremes.
    const std::int32_t indices[]{
        (std::numeric_limits<std::int32_t>::min)(), -123456, -1, 0, 1, 1000,
        (1 << 20), (std::numeric_limits<std::int32_t>::max)(),
    };
    bool all_default{ true };
    for (const std::int32_t i : indices)
    {
        if (vmhook::get_array_element<std::int32_t>(null_oop, i) != 0) { all_default = false; }
        if (vmhook::get_array_element<std::uint32_t>(bogus_oop, i) != 0u) { all_default = false; }
    }
    check("gae_index_sweep_all_default", all_default);

    // The compressed-OOP element read (used by the String[] / Object[] element
    // walks) is also default 0 on the null/invalid path -> decode_oop_pointer(0)
    // is nullptr -> a nullptr element SLOT, never a wild wrapper.
    check("gae_compressed_oop_element_zero_null_oop",
          vmhook::get_array_element<std::uint32_t>(null_oop, 0) == 0u);
    check("gae_compressed_oop_element_decodes_null",
          vmhook::hotspot::decode_oop_pointer(
              vmhook::get_array_element<std::uint32_t>(null_oop, 0)) == nullptr);
}

// ---------------------------------------------------------------------------
// 18. read_java_string() on a null / invalid OOP -> "" — the leaf the value_t
//     uint32 as_string() path and the String[] element walk both decode through.
//
// Contract (header, read_java_string): returns "" for a null OOP and for an
// invalid OOP (is_valid_pointer reject), each BEFORE any heap read.  With no
// JVM every compressed OOP decodes to nullptr, so the String-decode leaf always
// yields "".  This underpins the empty/never-throw guarantee whenever a wrapper
// or value_t ends up reading a String reference, so pin it directly.
// ---------------------------------------------------------------------------
static auto test_read_java_string_null_and_invalid() -> void
{
    check("rjs_null_empty", vmhook::read_java_string(nullptr).empty());

    const std::uintptr_t bogus[]{ 0x1u, 0x4u, 0x6u, 0x7u, 0xFFFFu, 0x10001u };
    bool all_empty{ true };
    for (const std::uintptr_t a : bogus)
    {
        if (!vmhook::read_java_string(reinterpret_cast<void*>(a)).empty()) { all_empty = false; }
    }
    check("rjs_invalid_pointers_all_empty", all_empty);

    // Decoding the whole compressed range then reading as a String is "" (no JVM
    // -> decode is nullptr -> read_java_string(nullptr) -> "").  This is the
    // exact composition value_t::as_string() performs for a uint32 alternative.
    const std::uint32_t oops[]{ 0u, 1u, 0x7u, 0x00800000u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu };
    bool all_decode_empty{ true };
    for (const std::uint32_t o : oops)
    {
        if (!vmhook::read_java_string(vmhook::hotspot::decode_oop_pointer(o)).empty())
        {
            all_decode_empty = false;
        }
    }
    check("rjs_decoded_compressed_range_all_empty", all_decode_empty);
}

// ---------------------------------------------------------------------------
// 19. value_t::as_string() over EVERY variant alternative, including the uint32
//     REFERENCE alternative's TRUE branch (decode -> read_java_string).
//
// as_string() std::visits the variant: the uint32 alternative routes through
// read_java_string(decode_oop_pointer(v)) (the if-constexpr TRUE branch); every
// OTHER alternative returns "" verbatim (the else branch).  The existing suite
// only ever exercised the else branch (non-reference alternatives) and a single
// uint32{0}; this pins that the uint32 branch ALSO yields "" without a JVM for
// the whole compressed range (decode -> nullptr -> read_java_string -> ""), so
// BOTH constexpr arms of as_string() are covered and proven empty/never-throw.
// ---------------------------------------------------------------------------
static auto test_value_t_as_string_all_alternatives() -> void
{
    // (a) Non-reference alternatives -> "" (the else arm).
    const value_t non_ref[]{
        value_t{ false }, value_t{ true },
        value_t{ std::int8_t{ -1 } }, value_t{ std::int16_t{ 1234 } },
        value_t{ std::int32_t{ -123456 } }, value_t{ std::int64_t{ 0x1'0000'0001LL } },
        value_t{ float{ 2.5F } }, value_t{ double{ 3.9 } },
        value_t{ std::uint16_t{ 0xFFFF } },
    };
    bool all_non_ref_empty{ true };
    for (const auto& v : non_ref)
    {
        if (!v.as_string().empty()) { all_non_ref_empty = false; }
        // None of these is a reference field.
        if (v.is_reference()) { all_non_ref_empty = false; }
    }
    check("as_string_non_reference_alternatives_all_empty", all_non_ref_empty);

    // (b) The uint32 REFERENCE alternative -> the TRUE constexpr arm
    //     (read_java_string(decode_oop_pointer(v))).  Without a JVM every value
    //     decodes to nullptr -> "".  This is the arm the old suite never hit.
    const std::uint32_t ref_oops[]{
        0u, 1u, 2u, 0x7u, 0xFFu, 0xFFFFu, 0x00800000u, 0x7FFFFFFFu,
        0x80000000u, 0xC0000000u, 0xFFFFFFFEu, 0xFFFFFFFFu,
    };
    bool all_ref_empty{ true };
    for (const std::uint32_t o : ref_oops)
    {
        const value_t v{ std::uint32_t{ o }, std::string{ "Ljava/lang/String;" } };
        if (!v.is_reference()) { all_ref_empty = false; }   // uint32 alt IS a reference
        if (!v.as_string().empty()) { all_ref_empty = false; }
    }
    check("as_string_uint32_reference_alternative_all_empty", all_ref_empty);

    // (c) as_string() is documented noexcept (a throwing field read in a detour
    //     would escape into the JVM).  Measure on a prebuilt lvalue.
    const value_t ref_v{ std::uint32_t{ 0x1234u }, std::string{ "Ljava/lang/String;" } };
    check("as_string_noexcept", noexcept(ref_v.as_string()));

    // (d) The signature does NOT affect as_string() (it decodes the OOP as a
    //     String regardless): a uint32 alt with a List/Object/array signature
    //     still yields "" without a JVM.
    const char* const sigs[]{
        "Ljava/lang/String;", "Ljava/util/List;", "Ljava/lang/Object;",
        "[Ljava/lang/String;", "[I", "I", "",
    };
    bool sig_independent_empty{ true };
    for (const char* s : sigs)
    {
        const value_t v{ std::uint32_t{ 0x7u }, std::string{ s } };
        if (!v.as_string().empty()) { sig_independent_empty = false; }
    }
    check("as_string_signature_independent_all_empty", sig_independent_empty);
}

// ---------------------------------------------------------------------------
// 20. value_t aggregate field semantics: signature is preserved VERBATIM
//     (including embedded NUL / array / empty), and is_reference() depends ONLY
//     on the variant alternative, never on the signature string.
//
// value_t is a plain aggregate { variant data; std::string signature{}; }.
// to_vector's object-array gate reads `signature`, so the aggregate must hold it
// byte-for-byte; is_reference() reads only `data`.  Pin both invariants so a
// future change that normalised/truncated the signature or coupled
// is_reference() to it would be caught — neither is permitted by the header.
// ---------------------------------------------------------------------------
static auto test_value_t_aggregate_field_semantics() -> void
{
    // signature preserved verbatim across representative shapes.
    {
        const value_t v{ std::uint32_t{ 0 }, std::string{ "[Ljava/lang/Object;" } };
        check("agg_signature_array_preserved", v.signature == "[Ljava/lang/Object;");
        check("agg_signature_array_size", v.signature.size() == 19u);
    }
    {
        const value_t v{ std::uint32_t{ 0 }, std::string{} };
        check("agg_signature_empty_preserved", v.signature.empty());
    }
    // Embedded NUL: the aggregate must keep all 5 bytes (std::string is length-
    // prefixed; NUL is a normal element, not a terminator).
    {
        const std::string sig{ "[L\0X;", 5 };
        const value_t v{ std::uint32_t{ 0 }, sig };
        check("agg_signature_embedded_nul_size5", v.signature.size() == 5u);
        check("agg_signature_embedded_nul_front_bracket", v.signature.front() == '[');
        check("agg_signature_embedded_nul_equal", v.signature == sig);
    }

    // is_reference() is governed SOLELY by the stored alternative: the SAME
    // signature over a uint32 alt is a reference, over any other alt is not.
    {
        const std::string list_sig{ "Ljava/util/List;" };
        check("agg_isref_uint32_with_list_sig_true",
              value_t{ std::uint32_t{ 0 }, list_sig }.is_reference());
        check("agg_isref_int32_with_list_sig_false",
              !value_t{ std::int32_t{ 0 }, list_sig }.is_reference());
        check("agg_isref_bool_with_list_sig_false",
              !value_t{ false, list_sig }.is_reference());
        // And conversely: a uint32 alt is a reference for ANY signature, even a
        // primitive descriptor or empty string.
        check("agg_isref_uint32_with_scalar_sig_true",
              value_t{ std::uint32_t{ 0 }, std::string{ "I" } }.is_reference());
        check("agg_isref_uint32_with_empty_sig_true",
              value_t{ std::uint32_t{ 0 }, std::string{} }.is_reference());
    }

    // Default-constructed aggregate: variant holds the FIRST alternative (bool
    // false) and an empty signature — pins the documented default shape.
    {
        const value_t v{};
        check("agg_default_holds_bool", std::holds_alternative<bool>(v.data));
        check("agg_default_not_reference", !v.is_reference());
        check("agg_default_signature_empty", v.signature.empty());
        check("agg_default_uint32_cast_zero", static_cast<std::uint32_t>(v) == 0u);
    }

    // is_reference() is noexcept (introspection on a detour thread).
    check("is_reference_noexcept", noexcept(value_t{ false }.is_reference()));
}

// ---------------------------------------------------------------------------
// 21. [ADDITIVE] vmhook::hotspot::narrow_decode / narrow_encode -- the PURE
//     compressed-OOP codec ARITHMETIC primitive that decode_oop_pointer() /
//     encode_oop_pointer() (and decode_klass_pointer / encode_klass_pointer)
//     are built on.  Sections 7g / 9 pin that decode_oop_pointer() returns
//     nullptr without a JVM (it needs the JVM-resolved base/shift), but the
//     UNDERLYING shift-add / subtract-shift math is deterministic with NO JVM
//     and NO memory read -- both are noexcept free functions taking the
//     base/shift explicitly.  This section pins the codec's exact formula and
//     its encode-then-decode identity round-trip over a representative bit-pattern
//     matrix, which the file never exercised (it only ever observed the
//     all-nullptr JVM-less decode result).
//
// Contract (header, vmhook.hpp narrow_decode / narrow_encode):
//   narrow_decode(base, shift, c)   == (void*)( base + ((uint64_t)c << shift) )
//   narrow_encode(base, shift, addr)== (uint32_t)( (addr - base) >> shift )
// Both are PURE arithmetic on their arguments -- they never read *addr / *base,
// so feeding them any integer base (even a "heap base"-looking constant) is
// safe: nothing is dereferenced.  is_valid_pointer / klass_from_oop are NOT
// involved here.  Every expected value below is computed by the SAME closed
// form straight from the header, so a regression that altered the shift
// direction, the base offset, or the unsigned widening fails loudly.
//
// ROUND-TRIP SOUNDNESS: encode(base,shift,decode(base,shift,c)) == c holds
// EXACTLY when ((uint64_t)c << shift) does not overflow 64 bits.  c is 32-bit,
// so the sweep restricts shift to [0,31] -- then c<<shift occupies at most
// 63 bits, the low `shift` bits of (c<<shift) are zero, so subtracting base
// and shifting right recovers c bit-for-bit.  Real HotSpot shifts are 0 or 3,
// well inside that bound.
// ---------------------------------------------------------------------------
static auto test_narrow_codec_roundtrip() -> void
{
    using vmhook::hotspot::narrow_decode;
    using vmhook::hotspot::narrow_encode;

    // --- (c) narrow_encode closed-form: (addr - base) >> shift. ---
    // narrow_encode takes addr as a std::uint64_t (NOT a pointer), so it is
    // POINTER-WIDTH-INDEPENDENT -- these run identically on 32- and 64-bit
    // pointer platforms.  Every expected value is the verbatim header formula.
    {
        bool enc_ok{ true };
        const std::uint64_t bases[]{
            0x0ull, 0x1000ull, 0x0000'7FF0'0000'0000ull, 0xFFFF'FFFF'0000'0000ull,
        };
        const std::uint32_t shifts[]{ 0u, 1u, 2u, 3u, 4u, 8u, 16u, 31u };
        // addr values chosen as base + (c << shift) so the subtraction is exact.
        const std::uint32_t comps[]{
            0u, 1u, 5u, 0xFFu, 0x1234u, 0xFFFFu, 0x00AB'CDEFu,
            0x7FFF'FFFFu, 0x8000'0000u, 0xFFFF'FFFFu,
        };
        for (const std::uint64_t base : bases)
        {
            for (const std::uint32_t shift : shifts)
            {
                for (const std::uint32_t c : comps)
                {
                    const std::uint64_t addr{ base + (static_cast<std::uint64_t>(c) << shift) };
                    const std::uint32_t expected{
                        static_cast<std::uint32_t>((addr - base) >> shift) };
                    if (narrow_encode(base, shift, addr) != expected) { enc_ok = false; }
                }
            }
        }
        check("narrow_encode_matches_addr_minus_base_shifted", enc_ok);
        // base==0, shift==0 is the identity for encode.
        check("narrow_encode_base0_shift0_identity",
              narrow_encode(0u, 0u, 0x1234'5678ull) == 0x1234'5678u);
        // shift==0, non-zero base: encode is the exact inverse subtraction.
        check("narrow_encode_shift0_is_addr_minus_base",
              narrow_encode(0x0000'0007'0000'0000ull, 0u,
                            0x0000'0007'0000'0000ull + 0xFFFFu) == 0xFFFFu);
    }

    // --- (d) ROUND-TRIP via the FORMULA addr: encode(base,shift, base+(c<<shift))
    //         == c for the whole 32-bit compressed matrix with shift in [0,31]
    //         (no top overflow).  POINTER-WIDTH-INDEPENDENT: the addr is built
    //         from the closed form in 64-bit, never via the void* result, so the
    //         inverse is pinned on every platform.  This is the encode-then-decode identity
    //         identity expressed through the math the codec is specified by. ---
    {
        const std::uint64_t bases[]{
            0x0ull, 0x1000ull, 0x0000'0007'0000'0000ull, 0xFFFF'FFFF'0000'0000ull,
        };
        const std::uint32_t shifts[]{ 0u, 1u, 3u, 4u, 8u, 16u, 31u };
        const std::uint32_t comps[]{
            0u, 1u, 2u, 3u, 7u, 8u, 0xFu, 0xFFu, 0x0100u, 0xFFFFu,
            0x0001'0000u, 0x0080'0000u, 0x7FFF'FFFFu, 0x8000'0000u,
            0xC000'0000u, 0xFFFF'FFFEu, 0xFFFF'FFFFu,
        };
        bool roundtrip_ok{ true };
        for (const std::uint64_t base : bases)
        {
            for (const std::uint32_t shift : shifts)
            {
                for (const std::uint32_t c : comps)
                {
                    const std::uint64_t addr{ base + (static_cast<std::uint64_t>(c) << shift) };
                    if (narrow_encode(base, shift, addr) != c) { roundtrip_ok = false; }
                }
            }
        }
        check("narrow_codec_encode_decode_roundtrip_is_identity", roundtrip_ok);
    }

    // --- (e) EXHAUSTIVE low-compressed round-trip 0..1023 at the real shifts
    //         {0,3} over base 0 and a non-zero heap base -- small-domain sweep
    //         proving the inverse is exact for every value in the range.  Uses
    //         the formula addr (width-independent). ---
    {
        const std::uint64_t bases[]{ 0x0ull, 0x0000'0008'0000'0000ull };
        const std::uint32_t shifts[]{ 0u, 3u };
        bool exhaustive_ok{ true };
        for (const std::uint64_t base : bases)
        {
            for (const std::uint32_t shift : shifts)
            {
                for (std::uint32_t c{ 0u }; c < 1024u; ++c)
                {
                    const std::uint64_t addr{ base + (static_cast<std::uint64_t>(c) << shift) };
                    if (narrow_encode(base, shift, addr) != c) { exhaustive_ok = false; }
                }
            }
        }
        check("narrow_codec_exhaustive_low_range_roundtrip", exhaustive_ok);
    }

    // --- (a)+(b)+(f) narrow_decode POINTER output matches the closed form
    //     base + (c << shift).  narrow_decode returns void*, so reading its bits
    //     back via reinterpret_cast recovers the full address ONLY where pointers
    //     are 64-bit (a 32-bit uintptr_t would truncate the high base/shift bits).
    //     Guard the pointer-bit comparisons on the pointer width, mirroring the
    //     file's existing `if constexpr (sizeof(void*) >= 8)` idiom -- nothing is
    //     ever dereferenced; the function only computes base + shifted c. ---
    if constexpr (sizeof(void*) >= 8)
    {
        auto decode_bits = [](std::uint64_t base, std::uint32_t shift,
                              std::uint32_t c) noexcept -> std::uint64_t
        {
            return reinterpret_cast<std::uintptr_t>(narrow_decode(base, shift, c));
        };

        // (a) Exact closed-form across a base/shift/compressed matrix.
        {
            const std::uint64_t bases[]{
                0x0ull, 0x1ull, 0x1000ull, 0x0000'0007'0000'0000ull,
                0x0000'7FF0'0000'0000ull, 0xFFFF'FFFF'0000'0000ull,
            };
            const std::uint32_t shifts[]{ 0u, 1u, 2u, 3u, 4u, 8u, 16u, 31u };
            const std::uint32_t comps[]{
                0u, 1u, 2u, 3u, 7u, 0xFFu, 0xFFFFu, 0x0080'0000u,
                0x0100'0000u, 0x7FFF'FFFFu, 0x8000'0000u, 0xFFFF'FFFFu,
            };
            bool all_formula_ok{ true };
            for (const std::uint64_t base : bases)
            {
                for (const std::uint32_t shift : shifts)
                {
                    for (const std::uint32_t c : comps)
                    {
                        const std::uint64_t expected{
                            base + (static_cast<std::uint64_t>(c) << shift) };
                        if (decode_bits(base, shift, c) != expected) { all_formula_ok = false; }
                    }
                }
            }
            check("narrow_decode_matches_base_plus_shifted_compressed", all_formula_ok);
        }

        // (b) shift==0 special case: decode == base + c; full round-trip too.
        {
            bool shift0_ok{ true };
            const std::uint64_t base{ 0x0000'0007'0000'0000ull };
            const std::uint32_t comps[]{ 0u, 1u, 0xFFu, 0xFFFFu, 0x8000'0000u, 0xFFFF'FFFFu };
            for (const std::uint32_t c : comps)
            {
                const std::uint64_t addr{ decode_bits(base, 0u, c) };
                if (addr != base + c) { shift0_ok = false; }
                if (narrow_encode(base, 0u, addr) != c) { shift0_ok = false; }
            }
            check("narrow_codec_shift0_decode_and_encode_exact", shift0_ok);
            check("narrow_decode_base0_shift0_identity",
                  decode_bits(0u, 0u, 0x1234'5678u) == 0x1234'5678ull);
        }

        // (f) shift moves bits LEFT by exactly `shift` positions (direction pin).
        {
            const std::uint32_t one{ 0x0000'0001u };
            check("narrow_decode_shift0_is_c",  decode_bits(0u, 0u, one) == 0x1ull);
            check("narrow_decode_shift1_is_2c", decode_bits(0u, 1u, one) == 0x2ull);
            check("narrow_decode_shift3_is_8c", decode_bits(0u, 3u, one) == 0x8ull);
            check("narrow_decode_shift4_is_16c",decode_bits(0u, 4u, one) == 0x10ull);
            check("narrow_decode_multibit_shift3",
                  decode_bits(0u, 3u, 0xABCDu) == (static_cast<std::uint64_t>(0xABCDu) << 3));
            // FULL pointer-output round-trip closing the encode-then-decode identity loop
            // through the ACTUAL void* result (not just the formula addr).
            const std::uint64_t base{ 0x0000'0007'0000'0000ull };
            bool ptr_roundtrip_ok{ true };
            const std::uint32_t comps[]{ 0u, 1u, 0xFFFFu, 0x0080'0000u, 0xFFFF'FFFFu };
            for (const std::uint32_t shift : { 0u, 3u, 16u })
            {
                for (const std::uint32_t c : comps)
                {
                    if (narrow_encode(base, shift, decode_bits(base, shift, c)) != c)
                    {
                        ptr_roundtrip_ok = false;
                    }
                }
            }
            check("narrow_codec_pointer_output_roundtrip_is_identity", ptr_roundtrip_ok);
        }
    }

    // --- (g) Both primitives are noexcept (they run on detour threads via the
    //         decode/encode wrappers that call them). ---
    check("narrow_decode_noexcept", noexcept(narrow_decode(0u, 0u, 0u)));
    check("narrow_encode_noexcept", noexcept(narrow_encode(0u, 0u, 0u)));

    // --- (h) The PUBLIC decode_oop_pointer still returns nullptr without a JVM
    //         even though the underlying arithmetic above is sound -- because the
    //         base/shift are unresolvable here.  Ties this pure-math section back
    //         to the JVM-less observable contract the rest of the file relies on:
    //         a correct codec + an unresolved base == nullptr, never a wild ptr. ---
    check("decode_oop_pointer_still_null_despite_sound_arithmetic",
          vmhook::hotspot::decode_oop_pointer(0x0080'0000u) == nullptr);
}

// ---------------------------------------------------------------------------
// 22. [ADDITIVE] detail::function_traits<F>::args_tuple_t decomposition over the
//     FULL callable-shape matrix the typed hook<T>() path feeds the collection
//     wrappers through.
//
// function_traits is the compile-time helper hook<T>() uses to enumerate a
// detour's argument list (vmhook.hpp function_traits): EVERY specialisation
// exposes exactly `args_tuple_t = std::tuple<argument_types...>` and NOTHING
// else (no return_type, no arity member — the args tuple IS the surface).  The
// header enumerates a deliberately exhaustive set of specialisations —
// free-fn ptr (plain + noexcept), std::function, lambda/functor via
// operator(), member-fn (plain/const), and the full cv (none/const/volatile/
// const volatile) x ref (none/& /&&) x noexcept (none/noexcept) matrix — so a
// `noexcept`/ref-qualified/`const` detour decomposes correctly instead of
// hitting the undefined primary template ("no member args_tuple_t").  These are
// PURE compile-time traits (no JVM, no codegen), so pin the decomposition for a
// representative point of every specialisation family: the args tuple's arity
// (std::tuple_size_v) and per-element type (std::tuple_element_t).  A regression
// that dropped a specialisation flips one of these to ill-formed at compile time.
// ---------------------------------------------------------------------------
namespace
{
    // Distinct argument/return types so a mis-decomposed tuple is detectable.
    using ft_args0 = void(*)();
    using ft_args1 = int(*)(double);
    using ft_args3 = bool(*)(std::int8_t, std::uint16_t, void*);
    using ft_args1_noexcept = int(*)(double) noexcept;
    using ft_stdfun = std::function<long(char, float)>;

    struct ft_functor_plain        { void operator()(int, int) {} };
    struct ft_functor_const        { void operator()(int, int) const {} };
    struct ft_functor_const_ne     { void operator()(double) const noexcept {} };
    struct ft_functor_ref          { void operator()(std::int64_t) & {} };
    struct ft_functor_const_ref_ne { void operator()(std::uint16_t, bool) const& noexcept {} };
    struct ft_functor_rref         { void operator()() && {} };

    template<typename function_type>
    using ft_tuple = typename vmhook::detail::function_traits<function_type>::args_tuple_t;
}

static auto test_function_traits_decomposition() -> void
{
    // --- Free-function pointer (plain): arity + per-arg type. ---
    static_assert(std::tuple_size_v<ft_tuple<ft_args0>> == 0,
                  "nullary free-fn ptr -> empty args tuple.");
    static_assert(std::tuple_size_v<ft_tuple<ft_args1>> == 1);
    static_assert(std::is_same_v<std::tuple_element_t<0, ft_tuple<ft_args1>>, double>);
    static_assert(std::tuple_size_v<ft_tuple<ft_args3>> == 3);
    static_assert(std::is_same_v<std::tuple_element_t<0, ft_tuple<ft_args3>>, std::int8_t>);
    static_assert(std::is_same_v<std::tuple_element_t<1, ft_tuple<ft_args3>>, std::uint16_t>);
    static_assert(std::is_same_v<std::tuple_element_t<2, ft_tuple<ft_args3>>, void*>);

    // --- noexcept free-fn ptr is a DISTINCT type (C++17): own specialisation,
    //     same args tuple as the plain form. ---
    static_assert(std::tuple_size_v<ft_tuple<ft_args1_noexcept>> == 1);
    static_assert(std::is_same_v<std::tuple_element_t<0, ft_tuple<ft_args1_noexcept>>, double>);
    static_assert(std::is_same_v<ft_tuple<ft_args1>, ft_tuple<ft_args1_noexcept>>,
                  "noexcept is irrelevant to the decoded Java parameter list.");

    // --- std::function specialisation. ---
    static_assert(std::tuple_size_v<ft_tuple<ft_stdfun>> == 2);
    static_assert(std::is_same_v<std::tuple_element_t<0, ft_tuple<ft_stdfun>>, char>);
    static_assert(std::is_same_v<std::tuple_element_t<1, ft_tuple<ft_stdfun>>, float>);

    // --- Functor / lambda via operator() across the cv/ref/noexcept matrix. ---
    static_assert(std::tuple_size_v<ft_tuple<ft_functor_plain>> == 2);
    static_assert(std::is_same_v<std::tuple_element_t<0, ft_tuple<ft_functor_plain>>, int>);
    static_assert(std::tuple_size_v<ft_tuple<ft_functor_const>> == 2);
    static_assert(std::tuple_size_v<ft_tuple<ft_functor_const_ne>> == 1);
    static_assert(std::is_same_v<std::tuple_element_t<0, ft_tuple<ft_functor_const_ne>>, double>);
    static_assert(std::tuple_size_v<ft_tuple<ft_functor_ref>> == 1);
    static_assert(std::is_same_v<std::tuple_element_t<0, ft_tuple<ft_functor_ref>>, std::int64_t>);
    static_assert(std::tuple_size_v<ft_tuple<ft_functor_const_ref_ne>> == 2);
    static_assert(std::is_same_v<std::tuple_element_t<0, ft_tuple<ft_functor_const_ref_ne>>, std::uint16_t>);
    static_assert(std::is_same_v<std::tuple_element_t<1, ft_tuple<ft_functor_const_ref_ne>>, bool>);
    static_assert(std::tuple_size_v<ft_tuple<ft_functor_rref>> == 0);

    // --- A real lambda (the common hook<T>() form) decomposes identically; a
    //     noexcept lambda uses the const-noexcept member specialisation. ---
    auto plain_lambda    = [](int, double) { return 0; };
    auto noexcept_lambda = [](std::int8_t, void*) noexcept { return false; };
    static_assert(std::tuple_size_v<ft_tuple<decltype(plain_lambda)>> == 2);
    static_assert(std::is_same_v<std::tuple_element_t<1, ft_tuple<decltype(plain_lambda)>>, double>);
    static_assert(std::tuple_size_v<ft_tuple<decltype(noexcept_lambda)>> == 2);
    static_assert(std::is_same_v<std::tuple_element_t<0, ft_tuple<decltype(noexcept_lambda)>>, std::int8_t>);
    static_assert(std::is_same_v<std::tuple_element_t<1, ft_tuple<decltype(noexcept_lambda)>>, void*>);
    // Reference the lambdas at runtime so clang -Wunused-variable stays quiet.
    check("function_traits_lambda_callable_plain", plain_lambda(1, 2.0) == 0);
    check("function_traits_lambda_callable_noexcept", noexcept_lambda(std::int8_t{ 0 }, nullptr) == false);

    // Runtime presence marker so the section contributes to the PASS count.
    check("function_traits_decomposition_static_asserts_held", true);
}

// ---------------------------------------------------------------------------
// 23. [ADDITIVE] jni::jvm_descriptor_for_arg<T>() — the C++ argument type -> JVM
//     descriptor classifier the element type-tag mapping rides for reference
//     element/key/value wrappers and primitive args.
//
// Contract (vmhook.hpp jvm_descriptor_for_arg, public jni::signature_for_arg):
//   string / string_view / char* / const char*  -> "Ljava/lang/String;"
//   bool                                          -> "Z"   (claimed before sizeof==1)
//   char16_t / std::uint16_t                      -> "C"   (claimed before sizeof==2)
//   integral sizeof 1 / 2 / 4 / 8                 -> "B" / "S" / "I" / "J"
//   float -> "F"   double -> "D"
//   object_base-derived wrapper (or unique_ptr<W>) -> "Lname;" from the
//                    register_class<W>() map, else "Ljava/lang/Object;".
// This is PURE compile-time-ish string building (no JVM, no memory read): the
// wrapper branch reads only the in-process type_to_class_map populated by the
// register_class<T>() calls in main().  Every descriptor below is derived
// verbatim from the header ladder; a regression that reordered the bool /
// uint16 early claims (so bool encoded "B" or uint16 encoded "S") fails loudly.
// ---------------------------------------------------------------------------
static auto test_jvm_descriptor_for_arg_mapping() -> void
{
    using vmhook::detail::jvm_descriptor_for_arg;

    // --- String-like family all collapse to the String descriptor. ---
    check("sig_string",        jvm_descriptor_for_arg<std::string>() == "Ljava/lang/String;");
    check("sig_string_view",   jvm_descriptor_for_arg<std::string_view>() == "Ljava/lang/String;");
    check("sig_cstr",          jvm_descriptor_for_arg<const char*>() == "Ljava/lang/String;");
    check("sig_mutable_cstr",  jvm_descriptor_for_arg<char*>() == "Ljava/lang/String;");

    // --- bool is claimed FIRST as "Z" (must NOT fall to the sizeof==1 "B"). ---
    check("sig_bool_is_Z",     jvm_descriptor_for_arg<bool>() == "Z");

    // --- char16_t / uint16_t claimed as "C" BEFORE the generic 2-byte "S". ---
    check("sig_char16_is_C",   jvm_descriptor_for_arg<char16_t>() == "C");
    check("sig_uint16_is_C",   jvm_descriptor_for_arg<std::uint16_t>() == "C");

    // --- Fixed-width integral ladder by sizeof. ---
    check("sig_i8_is_B",       jvm_descriptor_for_arg<std::int8_t>() == "B");
    check("sig_u8_is_B",       jvm_descriptor_for_arg<std::uint8_t>() == "B");
    check("sig_i16_is_S",      jvm_descriptor_for_arg<std::int16_t>() == "S");
    check("sig_i32_is_I",      jvm_descriptor_for_arg<std::int32_t>() == "I");
    check("sig_u32_is_I",      jvm_descriptor_for_arg<std::uint32_t>() == "I");
    check("sig_i64_is_J",      jvm_descriptor_for_arg<std::int64_t>() == "J");
    check("sig_u64_is_J",      jvm_descriptor_for_arg<std::uint64_t>() == "J");

    // --- Floating point. ---
    check("sig_float_is_F",    jvm_descriptor_for_arg<float>() == "F");
    check("sig_double_is_D",   jvm_descriptor_for_arg<double>() == "D");

    // --- Wrapper types resolve their "Lname;" descriptor from the register_class
    //     map.  With NO JVM, register_class<T>() returns false WITHOUT populating
    //     type_to_class_map (it early-returns when find_class() finds no klass —
    //     vmhook.hpp register_class), so the map is empty here and the documented
    //     "not registered -> Ljava/lang/Object;" fallback fires for EVERY wrapper.
    //     This is the exact no-JVM fail-soft contract on the reference branch: an
    //     object/wrapper arg never mis-encodes as a primitive, it degrades to the
    //     generic Object descriptor. ---
    check("sig_elem_w_wrapper_object_fallback",
          jvm_descriptor_for_arg<elem_w>() == "Ljava/lang/Object;");
    check("sig_key_w_wrapper_object_fallback",
          jvm_descriptor_for_arg<key_w>() == "Ljava/lang/Object;");
    check("sig_val_w_wrapper_object_fallback",
          jvm_descriptor_for_arg<val_w>() == "Ljava/lang/Object;");
    // unique_ptr<W> takes the SAME (unregistered) wrapper branch -> same fallback.
    check("sig_unique_ptr_elem_w_object_fallback",
          jvm_descriptor_for_arg<std::unique_ptr<elem_w>>() == "Ljava/lang/Object;");
    check("sig_unique_ptr_val_w_object_fallback",
          jvm_descriptor_for_arg<std::unique_ptr<val_w>>() == "Ljava/lang/Object;");

    // --- cv/ref qualified args are decayed first, so they map identically. ---
    check("sig_const_ref_int_is_I", jvm_descriptor_for_arg<const std::int32_t&>() == "I");
    check("sig_rref_double_is_D",   jvm_descriptor_for_arg<double&&>() == "D");
    check("sig_const_bool_is_Z",    jvm_descriptor_for_arg<const bool>() == "Z");

    // --- The mapper is noexcept (built on a detour thread when wiring a hook). ---
    check("sig_for_arg_noexcept", noexcept(jvm_descriptor_for_arg<std::int32_t>()));

    // --- Every primitive descriptor produced is a recognised single-byte
    //     BasicType in the primitive band [4,11] per sig_char_to_basic_type,
    //     tying this classifier to the descriptor parser used by the walks. ---
    {
        const std::string prim_sigs[]{
            jvm_descriptor_for_arg<bool>(),       jvm_descriptor_for_arg<std::int8_t>(),
            jvm_descriptor_for_arg<std::int16_t>(), jvm_descriptor_for_arg<std::int32_t>(),
            jvm_descriptor_for_arg<std::int64_t>(), jvm_descriptor_for_arg<float>(),
            jvm_descriptor_for_arg<double>(),      jvm_descriptor_for_arg<char16_t>(),
        };
        bool all_primitive_band{ true };
        for (const auto& s : prim_sigs)
        {
            if (s.size() != 1u) { all_primitive_band = false; continue; }
            const int basic{ vmhook::detail::sig_char_to_basic_type(s.front()) };
            if (basic < 4 || basic > 11) { all_primitive_band = false; }
        }
        check("sig_primitive_descriptors_in_basic_type_band", all_primitive_band);
    }
}

// ---------------------------------------------------------------------------
// 24. [ADDITIVE] Build-capability / platform macro CONSISTENCY — the compile-
//     time configuration the whole single-header feature (and these tests) is
//     gated by.  PURE preprocessor logic, no JVM, identical reasoning on every
//     OS/compiler; this is exactly STEP 2's capability-macro-consistency angle.
//
// vmhook.hpp publishes mutually-exclusive OS macros (exactly one of WINDOWS /
// LINUX / MACOS / IOS / ANDROID is 1) plus two aggregates (POSIX = LINUX|MACOS|
// IOS|ANDROID, APPLE = MACOS|IOS), mutually-exclusive ARCH macros (exactly one
// of X86_64 / ARM64), a derived RUNTIME_HOOKING_AVAILABLE gate, three compiler
// flags (MSVC / CLANG / GCC, MSVC and GCC mutually exclusive with CLANG by
// construction), and a packed VERSION integer.  Pin every invariant so a future
// edit that set two OS macros, or mis-packed the version, fails here.
// ---------------------------------------------------------------------------
static auto test_capability_macro_consistency() -> void
{
    // --- Exactly ONE OS macro is set. ---
    constexpr int os_sum{ VMHOOK_OS_WINDOWS + VMHOOK_OS_LINUX + VMHOOK_OS_MACOS
                          + VMHOOK_OS_IOS + VMHOOK_OS_ANDROID };
    static_assert(os_sum == 1, "exactly one VMHOOK_OS_* macro must be 1.");
    check("exactly_one_os_macro", os_sum == 1);

    // --- The POSIX / APPLE aggregates are the documented unions. ---
    static_assert(VMHOOK_OS_POSIX == (VMHOOK_OS_LINUX | VMHOOK_OS_MACOS
                                      | VMHOOK_OS_IOS | VMHOOK_OS_ANDROID),
                  "POSIX aggregate must be LINUX|MACOS|IOS|ANDROID.");
    static_assert(VMHOOK_OS_APPLE == (VMHOOK_OS_MACOS | VMHOOK_OS_IOS),
                  "APPLE aggregate must be MACOS|IOS.");
    // Windows is never POSIX; Apple is always POSIX.
    static_assert(!(VMHOOK_OS_WINDOWS && VMHOOK_OS_POSIX),
                  "Windows is mutually exclusive with the POSIX aggregate.");
    static_assert(VMHOOK_OS_APPLE == 0 || VMHOOK_OS_POSIX == 1,
                  "any Apple target is also POSIX.");
    check("posix_apple_aggregates_consistent",
          (VMHOOK_OS_POSIX == (VMHOOK_OS_LINUX | VMHOOK_OS_MACOS | VMHOOK_OS_IOS | VMHOOK_OS_ANDROID))
          && (VMHOOK_OS_APPLE == (VMHOOK_OS_MACOS | VMHOOK_OS_IOS)));

    // --- Exactly ONE arch macro is set. ---
    constexpr int arch_sum{ VMHOOK_ARCH_X86_64 + VMHOOK_ARCH_ARM64 };
    static_assert(arch_sum == 1, "exactly one VMHOOK_ARCH_* macro must be 1.");
    check("exactly_one_arch_macro", arch_sum == 1);

    // --- RUNTIME_HOOKING_AVAILABLE is the documented derived gate:
    //     x86_64 AND not iOS.  (arm64 or iOS -> the runtime hook API no-ops.) ---
    static_assert(VMHOOK_RUNTIME_HOOKING_AVAILABLE
                      == (VMHOOK_ARCH_X86_64 && !VMHOOK_OS_IOS),
                  "RUNTIME_HOOKING_AVAILABLE == X86_64 && !IOS.");
    check("runtime_hooking_gate_derived",
          VMHOOK_RUNTIME_HOOKING_AVAILABLE == (VMHOOK_ARCH_X86_64 && !VMHOOK_OS_IOS));

    // --- Compiler flags: MSVC and GCC are each mutually exclusive with CLANG
    //     by their definitions ( !__clang__ guards).  At most one of MSVC/GCC. ---
    static_assert(!(VMHOOK_COMPILER_MSVC && VMHOOK_COMPILER_CLANG),
                  "MSVC and CLANG flags are mutually exclusive.");
    static_assert(!(VMHOOK_COMPILER_GCC && VMHOOK_COMPILER_CLANG),
                  "GCC and CLANG flags are mutually exclusive.");
    static_assert(!(VMHOOK_COMPILER_MSVC && VMHOOK_COMPILER_GCC),
                  "MSVC and GCC flags are mutually exclusive.");
    constexpr int real_compiler_sum{ VMHOOK_COMPILER_MSVC + VMHOOK_COMPILER_GCC
                                     + VMHOOK_COMPILER_CLANG };
    // This test is built by gcc / clang / msvc, so at least one is set.
    static_assert(real_compiler_sum >= 1, "a recognised compiler must be detected.");
    check("compiler_flags_mutually_exclusive_and_present",
          real_compiler_sum >= 1
          && !(VMHOOK_COMPILER_MSVC && VMHOOK_COMPILER_GCC)
          && !(VMHOOK_COMPILER_GCC && VMHOOK_COMPILER_CLANG)
          && !(VMHOOK_COMPILER_MSVC && VMHOOK_COMPILER_CLANG));

    // --- Capability flags are strictly boolean (0/1). ---
    static_assert(VMHOOK_HAS_STD_FORMAT == 0 || VMHOOK_HAS_STD_FORMAT == 1);
    static_assert(VMHOOK_HAS_STD_PRINT == 0 || VMHOOK_HAS_STD_PRINT == 1);
    static_assert(VMHOOK_HAS_DEDUCING_THIS == 0 || VMHOOK_HAS_DEDUCING_THIS == 1);
    // std::print implies std::format (print is layered on format support).
    static_assert(VMHOOK_HAS_STD_PRINT == 0 || VMHOOK_HAS_STD_FORMAT == 1,
                  "std::print availability implies std::format availability.");
    check("capability_flags_boolean_and_layered",
          (VMHOOK_HAS_STD_FORMAT == 0 || VMHOOK_HAS_STD_FORMAT == 1)
          && (VMHOOK_HAS_STD_PRINT == 0 || VMHOOK_HAS_STD_PRINT == 1)
          && (VMHOOK_HAS_DEDUCING_THIS == 0 || VMHOOK_HAS_DEDUCING_THIS == 1)
          && (VMHOOK_HAS_STD_PRINT == 0 || VMHOOK_HAS_STD_FORMAT == 1));

    // --- Version macros pack/unpack via VMHOOK_MAKE_VERSION exactly. ---
    static_assert(VMHOOK_VERSION
                      == VMHOOK_MAKE_VERSION(VMHOOK_VERSION_MAJOR,
                                             VMHOOK_VERSION_MINOR,
                                             VMHOOK_VERSION_PATCH),
                  "VMHOOK_VERSION must equal MAKE_VERSION of the three components.");
    static_assert(VMHOOK_MAKE_VERSION(0, 5, 3) == ((0 * 1000000) + (5 * 1000) + 3),
                  "MAKE_VERSION packs major*1e6 + minor*1e3 + patch.");
    // Strictly monotone ordering across components (the gate consumers rely on).
    static_assert(VMHOOK_MAKE_VERSION(0, 5, 3) < VMHOOK_MAKE_VERSION(0, 5, 4));
    static_assert(VMHOOK_MAKE_VERSION(0, 5, 3) < VMHOOK_MAKE_VERSION(0, 6, 0));
    static_assert(VMHOOK_MAKE_VERSION(0, 5, 3) < VMHOOK_MAKE_VERSION(1, 0, 0));
    static_assert(VMHOOK_MAKE_VERSION(0, 5, 999) < VMHOOK_MAKE_VERSION(0, 6, 0),
                  "patch field width must not bleed into the minor field.");
    check("version_packs_correctly",
          VMHOOK_VERSION == VMHOOK_MAKE_VERSION(VMHOOK_VERSION_MAJOR,
                                                VMHOOK_VERSION_MINOR,
                                                VMHOOK_VERSION_PATCH));
    check("version_components_nonnegative",
          VMHOOK_VERSION_MAJOR >= 0 && VMHOOK_VERSION_MINOR >= 0 && VMHOOK_VERSION_PATCH >= 0);
}

// ---------------------------------------------------------------------------
// 25. [ADDITIVE] detail::is_unique_ptr_v — the trait jvm_descriptor_for_arg uses
//     to recognise a unique_ptr<W> element/value argument (the exact shape
//     to_vector<E>() / to_entries<K,V>() PRODUCE).  PURE type trait, no JVM.
//
// Contract (vmhook.hpp is_unique_ptr / is_unique_ptr_v): true IFF the
// cvref-stripped type is std::unique_ptr<...>; false for everything else
// (raw pointers, the wrapped type itself, shared_ptr, plain values).  The
// to_vector / to_entries return types are vector<unique_ptr<E>> /
// vector<pair<unique_ptr<K>,unique_ptr<V>>>, so this trait is what lets a
// produced element be passed straight back as a detour argument and encoded as
// the right "Lname;" descriptor — pin its exact partition.
// ---------------------------------------------------------------------------
static auto test_is_unique_ptr_trait() -> void
{
    using vmhook::detail::is_unique_ptr_v;

    // TRUE for unique_ptr of any element/key/value wrapper, with cvref noise.
    static_assert(is_unique_ptr_v<std::unique_ptr<elem_w>>);
    static_assert(is_unique_ptr_v<std::unique_ptr<key_w>>);
    static_assert(is_unique_ptr_v<std::unique_ptr<val_w>>);
    static_assert(is_unique_ptr_v<const std::unique_ptr<elem_w>&>,
                  "cvref-qualified unique_ptr is still recognised (matches header doc).");
    static_assert(is_unique_ptr_v<std::unique_ptr<elem_w>&&>);
    static_assert(is_unique_ptr_v<std::unique_ptr<std::int32_t>>);

    // FALSE for the wrapped type itself, raw pointers, and other smart pointers.
    static_assert(!is_unique_ptr_v<elem_w>);
    static_assert(!is_unique_ptr_v<elem_w*>);
    static_assert(!is_unique_ptr_v<vmhook::oop_t>);
    static_assert(!is_unique_ptr_v<std::shared_ptr<elem_w>>);
    static_assert(!is_unique_ptr_v<int>);
    static_assert(!is_unique_ptr_v<std::vector<std::unique_ptr<elem_w>>>,
                  "a VECTOR of unique_ptr is not itself a unique_ptr.");

    // The exact element type to_vector produces IS a unique_ptr; the vector that
    // holds it is not — pin the relationship the return-type shape encodes.
    using vec_t = decltype(std::declval<vmhook::collection>().to_vector<elem_w>());
    static_assert(!is_unique_ptr_v<vec_t>);
    static_assert(is_unique_ptr_v<vec_t::value_type>,
                  "vector<unique_ptr<E>>::value_type is unique_ptr<E>.");

    check("is_unique_ptr_trait_static_asserts_held", true);
}

// ---------------------------------------------------------------------------
// 26. [ADDITIVE] hotspot::return_slot POD layout + return_value cancel-flag /
//     value-byte ENCODING logic, WITHOUT a live frame.
//
// return_slot is the {bool cancel; int64_t retval;} cell the trampoline
// allocates and the callback writes via return_value::set()/cancel().  set<T>
// is the ENCODER: it sets cancel=true and writes retval as EITHER a sign-
// extended int64 (for signed integral T narrower than 8 bytes) OR a zero-filled
// memcpy of the raw bytes (everything else — bool, unsigned, float/double,
// void*).  cancel() flips cancel WITHOUT touching retval; the object_base null-
// return overload zeroes retval.  All of this is pure POD + byte arithmetic on a
// caller-owned return_slot — no frame is dereferenced (return_value takes the
// frame defaulted to nullptr; set/cancel never read it).  This section feeds a
// REAL stack return_slot and pins the exact bytes the encoder lands, the area
// the file never touched.  Every expected value is computed by the SAME closed
// form the header documents (sign-extend vs zero-fill memcpy).
// ---------------------------------------------------------------------------
static auto test_return_slot_encoding() -> void
{
    // The POD is exactly the documented two fields with their brace defaults.
    {
        vmhook::hotspot::return_slot slot{};
        check("return_slot_default_cancel_false", slot.cancel == false);
        check("return_slot_default_retval_zero",  slot.retval == 0);
    }
    static_assert(std::is_trivially_copyable_v<vmhook::hotspot::return_slot>,
                  "return_slot is a raw stack cell -- must be trivially copyable.");
    static_assert(std::is_standard_layout_v<vmhook::hotspot::return_slot>,
                  "return_slot is written by hand-rolled trampoline asm -- standard layout.");
    static_assert(std::is_same_v<decltype(vmhook::hotspot::return_slot::cancel), bool>);
    static_assert(std::is_same_v<decltype(vmhook::hotspot::return_slot::retval), std::int64_t>);

    // return_value is constructed from a slot pointer with the frame DEFAULTED to
    // nullptr; the ctor and set/cancel are noexcept and never read the frame.
    static_assert(std::is_constructible_v<vmhook::return_value,
                                          vmhook::hotspot::return_slot*>,
                  "return_value{slot} (frame defaulted nullptr) must be constructible.");
    {
        vmhook::hotspot::return_slot probe{};
        check("return_value_ctor_noexcept",
              noexcept(vmhook::return_value{ &probe }));
        const vmhook::return_value rv{ &probe };
        check("return_value_frame_null_when_defaulted", rv.frame() == nullptr);
        check("return_value_frame_noexcept", noexcept(rv.frame()));
    }

    // --- cancel() sets cancel=true and leaves retval UNTOUCHED. ---
    {
        vmhook::hotspot::return_slot slot{};
        slot.retval = static_cast<std::int64_t>(0x0123'4567'89AB'CDEFLL);
        vmhook::return_value rv{ &slot };
        check("cancel_noexcept", noexcept(rv.cancel()));
        rv.cancel();
        check("cancel_sets_cancel_true", slot.cancel == true);
        check("cancel_leaves_retval_untouched",
              slot.retval == static_cast<std::int64_t>(0x0123'4567'89AB'CDEFLL));
    }

    // --- set<T>() always raises cancel. ---
    {
        vmhook::hotspot::return_slot slot{};
        vmhook::return_value rv{ &slot };
        check("set_noexcept", noexcept(rv.set(std::int32_t{ 0 })));
        rv.set(std::int32_t{ 7 });
        check("set_raises_cancel", slot.cancel == true);
        check("set_i32_value_in_slot", slot.retval == 7);
    }

    // --- SIGNED integral narrower than int64 -> SIGN-EXTENDED into retval. ---
    // The header takes the static_cast<int64_t>(value) branch, so -1 fills all
    // 64 bits (0xFFFF...FF == int64 -1), NOT a zero-extended 0x000000FF.
    {
        vmhook::hotspot::return_slot s{}; vmhook::return_value r{ &s };
        r.set(std::int8_t{ -1 });
        check("set_i8_neg1_sign_extended", s.retval == static_cast<std::int64_t>(-1));
    }
    {
        vmhook::hotspot::return_slot s{}; vmhook::return_value r{ &s };
        r.set(std::int16_t{ -2 });
        check("set_i16_neg2_sign_extended", s.retval == static_cast<std::int64_t>(-2));
    }
    {
        vmhook::hotspot::return_slot s{}; vmhook::return_value r{ &s };
        r.set(std::int32_t{ -123456 });
        check("set_i32_neg_sign_extended", s.retval == static_cast<std::int64_t>(-123456));
    }
    {
        vmhook::hotspot::return_slot s{}; vmhook::return_value r{ &s };
        r.set(std::int8_t{ 127 });
        check("set_i8_max_positive", s.retval == 127);
    }
    {
        vmhook::hotspot::return_slot s{}; vmhook::return_value r{ &s };
        r.set(std::int16_t{ -32768 });
        check("set_i16_min_sign_extended", s.retval == static_cast<std::int64_t>(-32768));
    }

    // --- int64 itself takes the ZERO-FILL memcpy branch (sizeof==8, the
    //     sizeof<8 guard is false), but the full 8 bytes are copied so a negative
    //     int64 still lands intact. ---
    {
        vmhook::hotspot::return_slot s{}; vmhook::return_value r{ &s };
        r.set(std::int64_t{ -1 });
        check("set_i64_neg1_full_copy", s.retval == static_cast<std::int64_t>(-1));
    }
    {
        vmhook::hotspot::return_slot s{}; vmhook::return_value r{ &s };
        r.set(std::int64_t{ 0x0011'2233'4455'6677LL });
        check("set_i64_full_pattern", s.retval == 0x0011'2233'4455'6677LL);
    }

    // --- UNSIGNED narrow integral -> ZERO-FILL memcpy (NOT sign-extend): the low
    //     N bytes carry the value, the upper bytes stay zero from the retval=0. ---
    {
        vmhook::hotspot::return_slot s{}; vmhook::return_value r{ &s };
        r.set(std::uint8_t{ 0xFF });
        check("set_u8_max_zero_filled", s.retval == static_cast<std::int64_t>(0xFFLL));
    }
    {
        vmhook::hotspot::return_slot s{}; vmhook::return_value r{ &s };
        r.set(std::uint16_t{ 0xFFFF });
        check("set_u16_max_zero_filled", s.retval == static_cast<std::int64_t>(0xFFFFLL));
    }
    {
        vmhook::hotspot::return_slot s{}; vmhook::return_value r{ &s };
        r.set(std::uint32_t{ 0xDEAD'BEEFu });
        check("set_u32_zero_filled", s.retval == static_cast<std::int64_t>(0xDEAD'BEEFLL));
    }
    // --- bool true/false: integral but unsigned, so the zero-fill branch lands a
    //     single 0x01 / 0x00 byte. ---
    {
        vmhook::hotspot::return_slot s{}; vmhook::return_value r{ &s };
        r.set(true);
        check("set_bool_true_is_one", s.retval == 1);
    }
    {
        vmhook::hotspot::return_slot s{}; vmhook::return_value r{ &s };
        s.retval = 0x55;             // pre-dirty to prove the zero-fill clears it
        r.set(false);
        check("set_bool_false_clears_to_zero", s.retval == 0);
    }

    // --- void* (oop) value: pointer-width memcpy into the low bytes. ---
    {
        vmhook::hotspot::return_slot s{}; vmhook::return_value r{ &s };
        r.set(static_cast<void*>(nullptr));
        check("set_null_void_ptr_zero", s.retval == 0);
    }
    {
        vmhook::hotspot::return_slot s{}; vmhook::return_value r{ &s };
        void* const p{ reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x1234'5678u)) };
        r.set(p);
        check("set_void_ptr_low_bytes",
              s.retval == static_cast<std::int64_t>(0x1234'5678LL));
    }

    // --- float / double take the zero-fill memcpy branch: the IEEE-754 bit
    //     pattern lands in the low bytes.  Recover it via memcpy and compare to
    //     the value's own bit pattern (no float formatting, no NaN — exact bits). ---
    {
        vmhook::hotspot::return_slot s{}; vmhook::return_value r{ &s };
        const float f{ 2.5F };
        r.set(f);
        std::uint32_t f_bits{ 0 };
        std::memcpy(&f_bits, &f, sizeof(f_bits));
        check("set_float_low4_is_bit_pattern",
              static_cast<std::uint32_t>(static_cast<std::uint64_t>(s.retval) & 0xFFFF'FFFFu) == f_bits);
        // The float path zero-fills the high 4 bytes (only sizeof(float)==4 copied).
        check("set_float_high4_zero",
              (static_cast<std::uint64_t>(s.retval) >> 32) == 0u);
    }
    {
        vmhook::hotspot::return_slot s{}; vmhook::return_value r{ &s };
        const double d{ 3.9 };
        r.set(d);
        std::uint64_t d_bits{ 0 };
        std::memcpy(&d_bits, &d, sizeof(d_bits));
        check("set_double_full8_is_bit_pattern",
              static_cast<std::uint64_t>(s.retval) == d_bits);
    }

    // --- The object_base-derived null-return overload: cancel=true, retval=0,
    //     selected for a wrapper type (documentation-only template arg). ---
    {
        vmhook::hotspot::return_slot s{}; vmhook::return_value r{ &s };
        s.retval = 0x77;                          // pre-dirty
        r.set<elem_w>(nullptr);
        check("set_wrapper_nullptr_cancel_true", s.cancel == true);
        check("set_wrapper_nullptr_retval_zero", s.retval == 0);
    }

    // --- set<T> compile-time contract: only trivially-copyable, <=8-byte T are
    //     accepted (the two static_asserts in the body).  Pin the GATE shape that
    //     keeps the wrappers' return path honest. ---
    static_assert(sizeof(std::int64_t) == 8,
                  "the retval cell is a 64-bit word; T must fit it.");
    static_assert(std::is_trivially_copyable_v<vmhook::oop_t>,
                  "an oop_t (void*) return is the canonical reference set<T> case.");
}

// ---------------------------------------------------------------------------
// 27. [ADDITIVE] array_length / get_array_element STRIDE-LENGTH-INDEX boundaries
//     on a REAL OWNED BUFFER laid out as a Java array header.
//
// Sections 16/17 pinned the null / invalid-pointer / OOB rejection (every read
// returns 0 / T{}).  They never exercised the POSITIVE element-read path because
// no JVM is up.  But array_length and get_array_element are PURE offset
// arithmetic over the supplied oop: length at +12, element[i] at +16 + i*stride,
// both routed through os::safe_read on a MAPPED page (never faults on our own
// buffer).  So a caller-owned, 8-byte-aligned buffer whose +12 word holds a
// length and whose +16 body holds known element bytes lets us pin the stride /
// length / index math directly — a REAL owned buffer, no fabricated address.
//
// is_valid_pointer must ACCEPT the buffer for the read path to engage; a stack
// std::array<uint64_t,N> is in user range, even-aligned, and (astronomically)
// non-sentinel, but we GATE every read-back assertion on a runtime
// is_valid_pointer(buf) check so the section stays deterministic and never reads
// a rejected pointer: if accepted we pin the exact decoded values; either way
// the never-fault contract holds.
// ---------------------------------------------------------------------------
static auto test_array_element_real_buffer() -> void
{
    // 8-byte-aligned backing store: header (16 bytes) + body.  Lay out as:
    //   [+0 .. +11] mark/klass header (don't-care)
    //   [+12]       int32 length
    //   [+16 ..]    element body
    // Use a uint64 array so the base is 8-aligned (even -> passes the alignment
    // gate) and large enough for the widest element sweep.
    alignas(8) std::array<std::uint64_t, 16> storage{};
    auto* const base{ reinterpret_cast<std::uint8_t*>(storage.data()) };
    auto* const oop{ static_cast<vmhook::oop_t>(storage.data()) };

    const bool accepted{ vmhook::hotspot::is_valid_pointer(oop) };
    // A null oop is ALWAYS rejected by is_valid_pointer; our non-null buffer is
    // the live input under test.  (We never read it unless `accepted`.)
    check("array_buffer_null_oop_never_valid",
          !vmhook::hotspot::is_valid_pointer(static_cast<vmhook::oop_t>(nullptr)));

    // Write a length of 4 at +12.
    {
        const std::int32_t len{ 4 };
        std::memcpy(base + 12, &len, sizeof(len));
    }
    if (accepted)
    {
        check("real_buffer_array_length_reads_field", vmhook::array_length(oop) == 4);
    }
    else
    {
        // Pointer (improbably) rejected -> documented 0, never a fault.
        check("real_buffer_array_length_rejected_zero", vmhook::array_length(oop) == 0);
    }

    // --- int32 elements at stride 4, body base +16. ---
    {
        const std::int32_t elems[4]{ 0, 0x1111'1111, -1, 0x7FFF'FFFF };
        std::memcpy(base + 16, elems, sizeof(elems));
        if (accepted)
        {
            check("gae_i32_idx0", vmhook::get_array_element<std::int32_t>(oop, 0) == 0);
            check("gae_i32_idx1", vmhook::get_array_element<std::int32_t>(oop, 1) == 0x1111'1111);
            check("gae_i32_idx2", vmhook::get_array_element<std::int32_t>(oop, 2) == -1);
            check("gae_i32_idx3", vmhook::get_array_element<std::int32_t>(oop, 3) == 0x7FFF'FFFF);
            // Index == length and beyond -> default 0 (the >= length guard).
            check("gae_i32_idx4_oob_zero", vmhook::get_array_element<std::int32_t>(oop, 4) == 0);
            check("gae_i32_idx_neg_zero",  vmhook::get_array_element<std::int32_t>(oop, -1) == 0);
        }
    }

    // --- int8 elements at stride 1: with length 4, indices 0..3 read the first
    //     four body bytes.  Pin the stride is sizeof(T)==1, not 4. ---
    {
        const std::uint8_t bytes[4]{ 0xDE, 0xAD, 0xBE, 0xEF };
        std::memcpy(base + 16, bytes, sizeof(bytes));
        if (accepted)
        {
            check("gae_i8_stride1_idx0",
                  vmhook::get_array_element<std::uint8_t>(oop, 0) == 0xDE);
            check("gae_i8_stride1_idx1",
                  vmhook::get_array_element<std::uint8_t>(oop, 1) == 0xAD);
            check("gae_i8_stride1_idx3",
                  vmhook::get_array_element<std::uint8_t>(oop, 3) == 0xEF);
        }
    }

    // --- int64 / double elements at stride 8.  Reset the length to 2 so two
    //     8-byte slots fit the body, and pin both the value and the stride. ---
    {
        const std::int32_t len{ 2 };
        std::memcpy(base + 12, &len, sizeof(len));
        const std::int64_t longs[2]{ static_cast<std::int64_t>(0x0011'2233'4455'6677LL),
                                     static_cast<std::int64_t>(-1) };
        std::memcpy(base + 16, longs, sizeof(longs));
        if (accepted)
        {
            check("real_buffer_length_two", vmhook::array_length(oop) == 2);
            check("gae_i64_stride8_idx0",
                  vmhook::get_array_element<std::int64_t>(oop, 0) == 0x0011'2233'4455'6677LL);
            check("gae_i64_stride8_idx1",
                  vmhook::get_array_element<std::int64_t>(oop, 1) == static_cast<std::int64_t>(-1));
            check("gae_i64_idx2_oob_zero",
                  vmhook::get_array_element<std::int64_t>(oop, 2) == 0);
        }

        const double doubles[2]{ 2.5, 3.9 };
        std::memcpy(base + 16, doubles, sizeof(doubles));
        if (accepted)
        {
            check("gae_double_stride8_idx0", vmhook::get_array_element<double>(oop, 0) == 2.5);
            check("gae_double_stride8_idx1", vmhook::get_array_element<double>(oop, 1) == 3.9);
        }
    }

    // --- A ZERO length makes every index OOB -> default, with the body bytes
    //     still present (proves the bound, not the bytes, gates the read). ---
    {
        const std::int32_t len{ 0 };
        std::memcpy(base + 12, &len, sizeof(len));
        const std::int32_t elems[2]{ 0x4242'4242, 0x4343'4343 };
        std::memcpy(base + 16, elems, sizeof(elems));
        if (accepted)
        {
            check("gae_zero_length_idx0_default",
                  vmhook::get_array_element<std::int32_t>(oop, 0) == 0);
        }
        // Whether the buffer was accepted (reads the 0 field) or rejected (short-
        // circuits to 0), array_length is 0 here — and never faults either way.
        check("array_length_zero_field_reads_zero", vmhook::array_length(oop) == 0);
    }

    // --- The clamp the walks apply rides on whatever length we read: a length of
    //     5 clamps to 5 (well under the 1<<24 cap); pin the composition. ---
    {
        const std::int32_t len{ 5 };
        std::memcpy(base + 12, &len, sizeof(len));
        if (accepted)
        {
            check("array_length_then_clamp_passthrough",
                  vmhook::clamp_safe_container_count(vmhook::array_length(oop)) == 5);
        }
    }
}

// ---------------------------------------------------------------------------
// 28. [ADDITIVE] jni::signature_for_arg WIDTH-LADDER partitions + multi-arg
//     PACKING concatenation — the parts of the classifier section 23 did NOT
//     reach.
//
// Section 23 pinned the FIXED-WIDTH aliases (int8/16/32/64, bool, char16_t,
// uint16, float, double), the string family, and the wrapper Object-fallback.
// It never exercised the EXTENDED integral types that ride the generic
// `is_integral && sizeof==N` ladder: plain `char`, `signed char`, `unsigned
// char`, `short`, `unsigned short`, `int`, `unsigned int`, `long`, `unsigned
// long`, `long long`, `char8_t`, `char32_t`, `wchar_t`, `std::size_t`,
// `std::ptrdiff_t`.  Several of these are LLP64/LP64-variant (`long`,
// `wchar_t`, `size_t`), so the expected descriptor is DERIVED from sizeof +
// signedness rather than hard-coded — exactly as the header's ladder does.
// Plus the MULTI-ARG packing contract jni_make_unique builds the "(...)V"
// descriptor with: a fold concatenating per-arg descriptors.  All pure
// compile-time-ish string building, no JVM.
// ---------------------------------------------------------------------------
namespace
{
    // The header ladder's exact classification for an integral type, derived
    // from the SAME rules: bool->"Z"; char16_t/uint16_t->"C"; then by sizeof
    // 1->"B" 2->"S" 4->"I" 8->"J".  char8_t/char32_t/wchar_t/char/long/... all
    // fall through to the sizeof ladder.
    template<typename integral_t>
    auto expected_integral_descriptor() -> std::string
    {
        if constexpr (std::is_same_v<integral_t, bool>) { return "Z"; }
        else if constexpr (std::is_same_v<integral_t, char16_t>
                           || std::is_same_v<integral_t, std::uint16_t>) { return "C"; }
        else if constexpr (sizeof(integral_t) == 1) { return "B"; }
        else if constexpr (sizeof(integral_t) == 2) { return "S"; }
        else if constexpr (sizeof(integral_t) == 4) { return "I"; }
        else { return "J"; }   // sizeof == 8
    }

    // Mirror of the jni_make_unique constructor-descriptor fold: "(" + each
    // per-arg descriptor (decay-normalised) + ")V".
    template<typename... args_t>
    auto build_ctor_descriptor() -> std::string
    {
        std::string signature{ "(" };
        ((signature += vmhook::detail::jvm_descriptor_for_arg<std::remove_cvref_t<args_t>>()), ...);
        signature += ")V";
        return signature;
    }
}

static auto test_jni_signature_width_ladder_and_packing() -> void
{
    using vmhook::detail::jvm_descriptor_for_arg;

    // --- Extended integral types ride the generic sizeof ladder.  Each expected
    //     descriptor is derived from sizeof/signedness (LLP64-safe), NEVER a
    //     hard-coded letter, so this passes identically on Windows (long==4,
    //     wchar_t==2) and *nix (long==8, wchar_t==4). ---
    check("sig_char_matches_sizeof_ladder",
          jvm_descriptor_for_arg<char>() == expected_integral_descriptor<char>());
    check("sig_signed_char_matches_ladder",
          jvm_descriptor_for_arg<signed char>() == expected_integral_descriptor<signed char>());
    check("sig_unsigned_char_matches_ladder",
          jvm_descriptor_for_arg<unsigned char>() == expected_integral_descriptor<unsigned char>());
    check("sig_short_matches_ladder",
          jvm_descriptor_for_arg<short>() == expected_integral_descriptor<short>());
    check("sig_unsigned_short_matches_ladder",
          jvm_descriptor_for_arg<unsigned short>() == expected_integral_descriptor<unsigned short>());
    check("sig_int_matches_ladder",
          jvm_descriptor_for_arg<int>() == expected_integral_descriptor<int>());
    check("sig_unsigned_int_matches_ladder",
          jvm_descriptor_for_arg<unsigned int>() == expected_integral_descriptor<unsigned int>());
    check("sig_long_matches_ladder",
          jvm_descriptor_for_arg<long>() == expected_integral_descriptor<long>());
    check("sig_unsigned_long_matches_ladder",
          jvm_descriptor_for_arg<unsigned long>() == expected_integral_descriptor<unsigned long>());
    check("sig_long_long_matches_ladder",
          jvm_descriptor_for_arg<long long>() == expected_integral_descriptor<long long>());
    check("sig_unsigned_long_long_matches_ladder",
          jvm_descriptor_for_arg<unsigned long long>() == expected_integral_descriptor<unsigned long long>());
    check("sig_char8_matches_ladder",
          jvm_descriptor_for_arg<char8_t>() == expected_integral_descriptor<char8_t>());
    check("sig_char32_matches_ladder",
          jvm_descriptor_for_arg<char32_t>() == expected_integral_descriptor<char32_t>());
    check("sig_wchar_matches_ladder",
          jvm_descriptor_for_arg<wchar_t>() == expected_integral_descriptor<wchar_t>());
    check("sig_size_t_matches_ladder",
          jvm_descriptor_for_arg<std::size_t>() == expected_integral_descriptor<std::size_t>());
    check("sig_ptrdiff_t_matches_ladder",
          jvm_descriptor_for_arg<std::ptrdiff_t>() == expected_integral_descriptor<std::ptrdiff_t>());

    // --- char8_t is a distinct 1-byte integral -> "B" on every platform (its
    //     size is fixed by the standard, unlike long/wchar_t). ---
    check("sig_char8_is_B_fixed", jvm_descriptor_for_arg<char8_t>() == "B");
    // char32_t is a fixed 4-byte integral -> "I" on every platform.
    check("sig_char32_is_I_fixed", jvm_descriptor_for_arg<char32_t>() == "I");

    // --- The descriptor for any integral is exactly one byte and lands in the
    //     primitive BasicType band [4,11] (ties the classifier to the parser). ---
    {
        const std::string ladder[]{
            jvm_descriptor_for_arg<char>(),      jvm_descriptor_for_arg<short>(),
            jvm_descriptor_for_arg<int>(),       jvm_descriptor_for_arg<long>(),
            jvm_descriptor_for_arg<long long>(), jvm_descriptor_for_arg<wchar_t>(),
            jvm_descriptor_for_arg<char8_t>(),   jvm_descriptor_for_arg<char32_t>(),
            jvm_descriptor_for_arg<std::size_t>(),
        };
        bool all_one_byte_band{ true };
        for (const auto& d : ladder)
        {
            if (d.size() != 1u) { all_one_byte_band = false; continue; }
            const int basic{ vmhook::detail::sig_char_to_basic_type(d.front()) };
            if (basic < 4 || basic > 11) { all_one_byte_band = false; }
        }
        check("sig_width_ladder_all_primitive_band", all_one_byte_band);
    }

    // --- MULTI-ARG packing: the constructor descriptor is "(" + concat + ")V".
    //     With no JVM the wrapper args take the Object fallback, and primitives
    //     use their ladder letter.  Pin the exact concatenation/order. ---
    check("ctor_desc_empty_is_paren_V",
          build_ctor_descriptor<>() == "()V");
    check("ctor_desc_single_int",
          build_ctor_descriptor<std::int32_t>() == "(I)V");
    check("ctor_desc_int_bool_double",
          build_ctor_descriptor<std::int32_t, bool, double>() == "(IZD)V");
    check("ctor_desc_string_then_long",
          build_ctor_descriptor<std::string, std::int64_t>() == "(Ljava/lang/String;J)V");
    // Wrappers (unregistered w/o JVM) fall to Ljava/lang/Object; — packing keeps
    // each descriptor whole, in order, with no separators.
    check("ctor_desc_wrapper_then_int",
          build_ctor_descriptor<elem_w, std::int32_t>() == "(Ljava/lang/Object;I)V");
    check("ctor_desc_unique_ptr_wrapper_pair",
          build_ctor_descriptor<std::unique_ptr<key_w>, std::unique_ptr<val_w>>()
              == "(Ljava/lang/Object;Ljava/lang/Object;)V");
    // char16_t and uint16_t both pack as "C" (claimed before the 2-byte "S").
    check("ctor_desc_char16_uint16_both_C",
          build_ctor_descriptor<char16_t, std::uint16_t>() == "(CC)V");
    // cvref noise on each arg is decayed before classification (remove_cvref_t).
    check("ctor_desc_cvref_decayed",
          build_ctor_descriptor<const std::int32_t&, double&&, const bool>() == "(IDZ)V");

    // --- The single-arg public descriptor equals the first concatenation token,
    //     proving signature_for_arg IS the packing unit. ---
    check("packing_unit_is_signature_for_arg",
          build_ctor_descriptor<float>() == std::string{ "(" } + jvm_descriptor_for_arg<float>() + ")V");
}

// ---------------------------------------------------------------------------
// 29. [ADDITIVE] decode_u5 threaded-walk over a REAL owned buffer, framed as the
//     FieldInfoStream cursor the find_field path threads to recover a field
//     offset/signature index.  (The dedicated test_decode_u5.cpp owns the
//     exhaustive codec matrix; here we add ONLY the small set of inputs that tie
//     the decoder to THIS feature's concerns — a multi-field record walk and the
//     End(0) stop — that this file never exercised, all over a caller-owned
//     std::array buffer, no fabricated address.)
//
// Contract (header decode_u5): value = sum (b_i-1)<<(6*i); first low byte (<192)
// terminates; byte 0 at any position is End -> returns ~0u and REWINDS (cursor
// unchanged); at most 5 bytes read.  Every expected value below is hand-derived
// from that loop body.
// ---------------------------------------------------------------------------
static auto test_decode_u5_threaded_record_walk() -> void
{
    using vmhook::hotspot::klass;

    // A miniature FieldInfoStream record: name_idx=7, sig_idx=64, offset=4096,
    // access=0, flags=0, then End(0).  Encodings (canonical, no 0 bytes):
    //   7    -> {8}            (1 byte:  8-1)
    //   64   -> {65}           (1 byte:  65-1)
    //   4096 -> {193,62}       (2 bytes: 192 + 61*64 == 4096)
    //   0    -> {1}            (1 byte:  1-1)
    //   0    -> {1}            (1 byte)
    // Trailing real End marker byte 0 at the end.  Buffer is caller-owned and
    // padded so the 5-byte peek window is always in-bounds.
    std::array<std::uint8_t, 16> stream{
        8u, 65u, 193u, 62u, 1u, 1u, 0u,  // 7, 64, 4096, 0, 0, End
        0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u
    };

    int pos{ 0 };
    const std::uint32_t name_idx{ klass::decode_u5(stream.data(), pos) };
    const std::uint32_t sig_idx{ klass::decode_u5(stream.data(), pos) };
    const std::uint32_t offset{ klass::decode_u5(stream.data(), pos) };
    const std::uint32_t access{ klass::decode_u5(stream.data(), pos) };
    const std::uint32_t flags{ klass::decode_u5(stream.data(), pos) };
    const int pos_after_record{ pos };
    const std::uint32_t end{ klass::decode_u5(stream.data(), pos) };

    check("u5_walk_name_idx",   name_idx == 7u);
    check("u5_walk_sig_idx",    sig_idx == 64u);
    check("u5_walk_offset",     offset == 4096u);
    check("u5_walk_access",     access == 0u);
    check("u5_walk_flags",      flags == 0u);
    // Cursor after the five fields: 1+1+2+1+1 == 6.
    check("u5_walk_cursor_after_record", pos_after_record == 6);
    // The End(0) marker returns ~0u and REWINDS (cursor unchanged at 6).
    check("u5_walk_end_is_sentinel", end == ~0u);
    check("u5_walk_end_rewinds_cursor", pos == pos_after_record);

    // decode_u5 is noexcept (it runs on a detour thread during find_field).
    check("decode_u5_noexcept", noexcept(klass::decode_u5(stream.data(), pos)));

    // The offset value recovered (4096) feeds straight into the clamp the
    // container walks apply to a derived count — a value well under the cap is
    // returned verbatim, tying the decoder output to the safety clamp.
    check("u5_decoded_offset_clamps_passthrough",
          vmhook::clamp_safe_container_count(static_cast<std::int32_t>(offset)) == 4096);
}

// ---------------------------------------------------------------------------
// W25. collection_list-specific deepening (ledger gaps).
//
// Wave-25 ledger items:
//   (a) Cold-state list / linked_list / collection accessor noexcept + safe
//       defaults (empty/zero) when no JVM is loaded.  Augments earlier null
//       checks with a tag-by-tag noexcept proof on the to_vector entry point.
//   (b) Size > capacity ambiguity probed against a fabricated array header
//       (size=N, elementData "capacity" N/2) — without a JVM the wrapper walk
//       cannot be driven, so we pin the *primitive* it sits on:
//       get_array_element<uint32> clamps EVERY index >= array_length back to
//       T{} (compressed 0 -> decode -> nullptr), so even a corrupted size>cap
//       would yield phantom nullptr slots, NEVER an OOB read.  This is the
//       no-crash characterization the feature notes ask for.
//   (c) Empty-vs-decode-failure ambiguity ([INFO]-gated): with no JVM, a null
//       collection and a "real" wrapper over a bogus oop produce IDENTICAL
//       empty observable state — a caller cannot tell them apart.  Pinned as
//       INFO because the contract intentionally collapses both paths to empty.
//   (d) Null array_oop accessor safe across the element-type sweep AND
//       repeated invocations (idempotent, no hidden state).
// ---------------------------------------------------------------------------
static auto test_w25_collection_list_cold_state() -> void
{
    // (a) Cold-state noexcept on the EXACT user-reached entry points for the
    // list / linked_list wrappers — pin alongside the existing collection set.
    vmhook::list        l{ nullptr };
    vmhook::linked_list ll{ nullptr };
    vmhook::collection  c{ nullptr };

    // to_vector is documented "never throw on cold state" but is NOT declared
    // noexcept (it instantiates std::vector<unique_ptr<>>) — [INFO] only.
    std::printf("[INFO] w25_list_to_vector_noexcept=%d\n",
                noexcept(l.to_vector<elem_w>()) ? 1 : 0);
    std::printf("[INFO] w25_linked_list_to_vector_noexcept=%d\n",
                noexcept(ll.to_vector<elem_w>()) ? 1 : 0);
    std::printf("[INFO] w25_collection_to_vector_noexcept=%d\n",
                noexcept(c.to_vector<elem_w>()) ? 1 : 0);
    check("w25_list_size_noexcept",             noexcept(l.size()));
    check("w25_linked_list_size_noexcept",      noexcept(ll.size()));
    check("w25_list_is_empty_noexcept",         noexcept(l.is_empty()));
    check("w25_linked_list_is_empty_noexcept",  noexcept(ll.is_empty()));

    // Safe-default returns: cold state -> empty vector + zero size + is_empty true.
    check("w25_list_cold_size_zero",        l.size() == 0);
    check("w25_list_cold_to_vector_empty",  l.to_vector<elem_w>().empty());
    check("w25_list_cold_is_empty",         l.is_empty());
    check("w25_linked_list_cold_size_zero", ll.size() == 0);
    check("w25_linked_list_cold_to_vector_empty",
          ll.to_vector<elem_w>().empty());
    check("w25_linked_list_cold_is_empty",  ll.is_empty());

    // (b) size > capacity ambiguity via fabricated header bytes.  Build an
    // 8-aligned buffer with the JVM array layout (length at +12, body at +16),
    // write a length of 2 ("real capacity" 2) but pretend the caller had a
    // size of 8 (size > capacity by 4x).  The walk's loop bound would be
    // index in [0, size).  Every index >= length is clamped by
    // get_array_element to T{} -> compressed 0 -> nullptr slot.  No OOB read.
    alignas(8) std::array<std::uint64_t, 16> storage{};
    auto* const base{ reinterpret_cast<std::uint8_t*>(storage.data()) };
    auto* const oop{ static_cast<vmhook::oop_t>(storage.data()) };
    const bool accepted{ vmhook::hotspot::is_valid_pointer(oop) };

    // length (capacity) = 2
    {
        const std::int32_t cap{ 2 };
        std::memcpy(base + 12, &cap, sizeof(cap));
    }
    // body holds two real compressed oop slots (still bogus / unreadable
    // outside the buffer); we don't care what they decode to — we care that
    // out-of-the-real-length indices return 0u.
    {
        const std::uint32_t slots[2]{ 0x1111'1111u, 0x2222'2222u };
        std::memcpy(base + 16, slots, sizeof(slots));
    }

    if (accepted)
    {
        check("w25_fabricated_header_cap_reads_back",
              vmhook::array_length(oop) == 2);

        // Pretend "size" = 8 (size > capacity).  Every index in [length, size)
        // is clamped to 0u — characterizes no-crash behaviour.
        constexpr std::int32_t fake_size{ 8 };
        bool tail_all_zero{ true };
        for (std::int32_t i{ 2 }; i < fake_size; ++i)
        {
            const std::uint32_t e{ vmhook::get_array_element<std::uint32_t>(oop, i) };
            if (e != 0u) { tail_all_zero = false; }
            // The phantom slot also decodes to nullptr (no JVM) -> a wrapper
            // walk would push std::unique_ptr{nullptr}, never wild.
            const void* decoded{ vmhook::hotspot::decode_oop_pointer(e) };
            if (decoded != nullptr) { tail_all_zero = false; }
        }
        check("w25_size_gt_capacity_tail_all_zero_no_oob", tail_all_zero);

        // The first two indices are within the real capacity and read the raw
        // body bytes verbatim (proves the bound check fires at length, not at
        // index 0).
        check("w25_size_gt_capacity_in_bounds_idx0_reads_body",
              vmhook::get_array_element<std::uint32_t>(oop, 0) == 0x1111'1111u);
        check("w25_size_gt_capacity_in_bounds_idx1_reads_body",
              vmhook::get_array_element<std::uint32_t>(oop, 1) == 0x2222'2222u);
    }
    else
    {
        // Pointer rejected (improbable on a stack-aligned buffer): walks would
        // still be safe (every index -> 0u) — pin the no-crash contract.
        check("w25_fabricated_header_rejected_array_length_zero",
              vmhook::array_length(oop) == 0);
        check("w25_fabricated_header_rejected_idx0_zero",
              vmhook::get_array_element<std::uint32_t>(oop, 0) == 0u);
    }

    // safe_read on a heap-style buffer is the OS-layer leaf the walks lean on
    // (linked_list_walk_items reads `first`/`item`/`next` via safe_read).
    // A safe_read against our own buffer succeeds for the length field — pin
    // that the primitive returns true on a valid in-process address.
    {
        std::int32_t readback{ -1 };
        const bool ok{ vmhook::os::safe_read(&readback, base + 12, sizeof(readback)) };
        check("w25_safe_read_in_process_succeeds", ok);
        check("w25_safe_read_in_process_value_matches",
              readback == 2);
    }
    // safe_read against a guaranteed-invalid low pointer either returns false
    // OR populates a default; either way no crash.  Characterized as no-fault.
    {
        std::int32_t scratch{ 0x7E7E'7E7E };
        const bool ok{ vmhook::os::safe_read(&scratch,
            reinterpret_cast<const void*>(static_cast<std::uintptr_t>(0x4u)),
            sizeof(scratch)) };
        // Result is platform-variant (some safe_read impls succeed for any
        // readable mapping); [INFO] gate.
        std::printf("[INFO] w25_safe_read_low_invalid_ok=%d\n", ok ? 1 : 0);
    }

    // (c) Empty-vs-decode-failure ambiguity.  Without a JVM, both a NULL
    // wrapper and a NON-NULL-but-bogus wrapper deliver IDENTICAL empty state.
    // A caller cannot distinguish "the list was empty" from "decode failed".
    // [INFO] gate: the contract intentionally collapses both — this is the
    // ambiguity flaw #5 in the feature notes, characterized not enforced.
    vmhook::list l_bogus{ reinterpret_cast<vmhook::oop_t>(static_cast<std::uintptr_t>(0x8u)) };
    const bool null_path_empty{ l.to_vector<elem_w>().empty() && l.size() == 0 };
    const bool bogus_path_empty{ l_bogus.to_vector<elem_w>().empty() && l_bogus.size() == 0 };
    const bool indistinguishable{ null_path_empty == bogus_path_empty };
    std::printf("[INFO] w25_empty_vs_decode_failure_indistinguishable=%d\n",
                indistinguishable ? 1 : 0);
    // The HARD invariant underneath is the no-fault piece: both paths produce
    // empty / size 0 without throwing — that we DO assert.
    check("w25_null_path_empty_safe",  null_path_empty);
    check("w25_bogus_path_empty_safe", bogus_path_empty);

    // (d) Null array_oop accessor safe + idempotent across element-type sweep.
    // Same primitive, same null oop, called twice — no hidden state.
    for (int rep{ 0 }; rep < 2; ++rep)
    {
        const bool all_default{
            vmhook::get_array_element<std::uint32_t>(nullptr, 0) == 0u
            && vmhook::get_array_element<std::int32_t>(nullptr, 17) == 0
            && vmhook::get_array_element<std::uint8_t>(nullptr, -1) == 0
            && vmhook::array_length(nullptr) == 0
        };
        std::string n{ "w25_null_array_oop_accessor_safe_rep" };
        n += static_cast<char>('0' + rep);
        check(n.c_str(), all_default);
    }

    // Compile-time facts: every cold-state entry is noexcept (already asserted
    // above via the `noexcept(expr)` operator), and the wrappers themselves
    // are nothrow-default-constructible from oop_t.
    static_assert(std::is_nothrow_constructible_v<vmhook::list, vmhook::oop_t>);
    static_assert(std::is_nothrow_constructible_v<vmhook::linked_list, vmhook::oop_t>);
    static_assert(std::is_nothrow_constructible_v<vmhook::collection, vmhook::oop_t>);
    static_assert(noexcept(std::declval<vmhook::list&>().size()));
    static_assert(noexcept(std::declval<vmhook::linked_list&>().size()));
    static_assert(noexcept(std::declval<vmhook::list&>().is_empty()));
    static_assert(noexcept(vmhook::array_length(nullptr)));
}

// ---------------------------------------------------------------------------
// Wave-31 — hash/tree map specialist cold-state deepening.
//
// Owner: collection_hash_tree_map. The wrappers vmhook::map / vmhook::hash_map
// both route through map::to_entries (the [INFO] in the feature notes).
// to_entries probes the live OOP's klass for a `table` field (HashMap fast
// path) first, then a `root` field (TreeMap path). With no JVM loaded, the
// klass cannot resolve, both probes miss, the walk returns {} — never faults.
//
// THIS wave hardens the contract specialist-owners must guarantee on cold
// state: every entry point is noexcept-safe-by-construction, the safe-default
// is provably empty, a null `root` (TreeMap) and null `table` (HashMap) both
// degrade to empty, the size cap constant has the documented value, and the
// signatures of the specialist's traversal primitives are pinned at compile
// time by static_assert.
// ---------------------------------------------------------------------------
static auto test_w31_hash_tree_map_cold_state() -> void
{
    // -- (a) Cold-state safe-default: EVERY map-side entry returns empty. ----
    // Null OOP, low-bogus OOP (rejected by is_valid_pointer), and an OOP whose
    // header would decode to "no klass": all three must give entries.empty().
    vmhook::map      m_null{ nullptr };
    vmhook::hash_map hm_null{ nullptr };
    vmhook::map      m_bogus{ reinterpret_cast<vmhook::oop_t>(static_cast<std::uintptr_t>(0x6u)) };
    vmhook::hash_map hm_bogus{ reinterpret_cast<vmhook::oop_t>(static_cast<std::uintptr_t>(0x8u)) };

    check("w31_map_null_to_entries_empty",
          (m_null.to_entries<key_w, val_w>().empty()));
    check("w31_hash_map_null_to_entries_empty",
          (hm_null.to_entries<key_w, val_w>().empty()));
    check("w31_map_bogus_to_entries_empty",
          (m_bogus.to_entries<key_w, val_w>().empty()));
    check("w31_hash_map_bogus_to_entries_empty",
          (hm_bogus.to_entries<key_w, val_w>().empty()));

    // size()/is_empty() degrade in lockstep (cold state -> 0 / true).
    check("w31_map_null_size_zero",     m_null.size() == 0);
    check("w31_map_null_is_empty_true", m_null.is_empty());
    check("w31_hash_map_null_size_zero",     hm_null.size() == 0);
    check("w31_hash_map_null_is_empty_true", hm_null.is_empty());
    check("w31_map_bogus_size_zero",          m_bogus.size() == 0);
    check("w31_hash_map_bogus_size_zero",     hm_bogus.size() == 0);

    // -- (b) Cold-state noexcept on the user-reached map entry points. -------
    // size()/is_empty() are documented noexcept on the map branch (flaw notes
    // 15079-15092 for size; is_empty inherits via "size()==0").
    check("w31_map_size_noexcept",            noexcept(m_null.size()));
    check("w31_map_is_empty_noexcept",        noexcept(m_null.is_empty()));
    check("w31_hash_map_size_noexcept",       noexcept(hm_null.size()));
    check("w31_hash_map_is_empty_noexcept",   noexcept(hm_null.is_empty()));

    // to_entries instantiates std::vector<std::pair<unique_ptr<K>, unique_ptr<V>>>,
    // which is NOT a noexcept operation in libstdc++/MSVC STL (allocator may
    // throw bad_alloc). Characterize per-platform; do NOT assert.
    std::printf("[INFO] w31_map_to_entries_noexcept=%d\n",
                noexcept(m_null.to_entries<key_w, val_w>()) ? 1 : 0);
    std::printf("[INFO] w31_hash_map_to_entries_noexcept=%d\n",
                noexcept(hm_null.to_entries<key_w, val_w>()) ? 1 : 0);

    // -- (c) Null root / null table degrade safely via the field_proxy path. -
    // The implicit user-reached path: get_field("foo")->get().to_entries<K,V>().
    // A field_proxy over a null field_pointer yields value_t holding int32{0};
    // the to_entries delegator decodes 0 -> nullptr -> map{nullptr} -> empty.
    // This pins BOTH the HashMap-shaped signature ("Ljava/util/HashMap;") and
    // the TreeMap-shaped signature ("Ljava/util/TreeMap;") degrade identically.
    vmhook::field_proxy hashmap_field{ nullptr, "Ljava/util/HashMap;", false };
    vmhook::field_proxy treemap_field{ nullptr, "Ljava/util/TreeMap;", false };
    vmhook::field_proxy map_iface_field{ nullptr, "Ljava/util/Map;",   false };
    check("w31_hashmap_field_proxy_to_entries_empty",
          (hashmap_field.get().to_entries<key_w, val_w>().empty()));
    check("w31_treemap_field_proxy_to_entries_empty",
          (treemap_field.get().to_entries<key_w, val_w>().empty()));
    check("w31_map_iface_field_proxy_to_entries_empty",
          (map_iface_field.get().to_entries<key_w, val_w>().empty()));

    // Static-flagged proxy (the field IS static) over a null pointer also
    // degrades safely — the static flag does not change the null short-circuit.
    vmhook::field_proxy static_treemap{ nullptr, "Ljava/util/TreeMap;", true };
    check("w31_static_treemap_to_entries_empty",
          (static_treemap.get().to_entries<key_w, val_w>().empty()));

    // -- (d) Size cap CONSTANT — pin the documented red-black walk outer cap.
    // The TreeMap walk uses (1<<24) as visited cap; HashMap bucket walk uses
    // (1<<20) per chain. Both must round-trip via k_max_safe_container_elems
    // (which IS 1<<24) for the cap-driven reserve/loop. Pin compile-time.
    static_assert(vmhook::k_max_safe_container_elems == (1ull << 24),
                  "TreeMap visit cap / HashMap reserve cap is 1<<24.");
    static_assert((1u << 20) < vmhook::k_max_safe_container_elems,
                  "Per-bucket chain cap (1<<20) must be strictly below the outer cap.");
    check("w31_size_cap_constant_is_1_24",
          vmhook::k_max_safe_container_elems == (1ull << 24));
    check("w31_per_bucket_cap_below_outer_cap",
          (1u << 20) < vmhook::k_max_safe_container_elems);
    // The cap clamps every count to a sane reserve bound — re-pin the gate
    // saturates here so the map walkers cannot drive an unbounded reserve.
    static_assert(vmhook::clamp_safe_container_count(
                      (std::numeric_limits<std::int32_t>::max)())
                  == static_cast<std::int32_t>(vmhook::k_max_safe_container_elems));

    // -- (e) Static_asserts on the specialist's signature surface. -----------
    // Wrapper construction from oop_t is nothrow for both tags.
    static_assert(std::is_nothrow_constructible_v<vmhook::map, vmhook::oop_t>);
    static_assert(std::is_nothrow_constructible_v<vmhook::hash_map, vmhook::oop_t>);
    // Copy/move noexcept (inherited from object_base) — STL container safety.
    static_assert(std::is_nothrow_copy_constructible_v<vmhook::map>);
    static_assert(std::is_nothrow_move_constructible_v<vmhook::map>);
    static_assert(std::is_nothrow_copy_constructible_v<vmhook::hash_map>);
    static_assert(std::is_nothrow_move_constructible_v<vmhook::hash_map>);
    // size() / is_empty() noexcept on both tags.
    static_assert(noexcept(std::declval<vmhook::map&>().size()));
    static_assert(noexcept(std::declval<vmhook::map&>().is_empty()));
    static_assert(noexcept(std::declval<vmhook::hash_map&>().size()));
    static_assert(noexcept(std::declval<vmhook::hash_map&>().is_empty()));
    // The two tags MUST stay distinct types AND hash_map MUST derive from map
    // (otherwise the `[INFO]: same to_entries` routing assumption is violated).
    static_assert(!std::is_same_v<vmhook::map, vmhook::hash_map>);
    static_assert(std::is_base_of_v<vmhook::map, vmhook::hash_map>);
    // The cap CONSTANT has the documented type and width — a regression to
    // uint32_t would change the saturation arithmetic at the boundary.
    static_assert(vmhook::k_max_safe_container_elems > 0u);
    static_assert(static_cast<std::uint64_t>(vmhook::k_max_safe_container_elems)
                  == 16'777'216ull);
    // to_entries return type is the documented vector-of-pairs-of-unique_ptr.
    static_assert(std::is_same_v<
        decltype(std::declval<vmhook::map&>().to_entries<key_w, val_w>()),
        std::vector<std::pair<std::unique_ptr<key_w>, std::unique_ptr<val_w>>>>);
    static_assert(std::is_same_v<
        decltype(std::declval<vmhook::hash_map&>().to_entries<key_w, val_w>()),
        std::vector<std::pair<std::unique_ptr<key_w>, std::unique_ptr<val_w>>>>);

    // -- (f) Idempotency: repeated cold-state calls yield identical empties.
    for (int rep{ 0 }; rep < 3; ++rep)
    {
        const bool all_empty{
            m_null.to_entries<key_w, val_w>().empty()
            && hm_null.to_entries<key_w, val_w>().empty()
            && m_bogus.to_entries<key_w, val_w>().empty()
            && hm_bogus.to_entries<key_w, val_w>().empty()
            && m_null.size() == 0
            && hm_null.size() == 0
        };
        std::string n{ "w31_cold_state_idempotent_rep" };
        n += static_cast<char>('0' + rep);
        check(n.c_str(), all_empty);
    }

    // -- (g) Substitutability through base ref: hash_map binds as map&. ------
    // The whole point of the lattice is that a detour declaring `const map&`
    // accepts hash_map by reference (no slicing) and routes through map's
    // to_entries — exactly the `[INFO]` shared-dispatch fact.
    auto route_via_map_ref = [](const vmhook::map& ref) noexcept -> std::size_t
    {
        return ref.to_entries<key_w, val_w>().size()
             + static_cast<std::size_t>(ref.size());
    };
    check("w31_route_map_through_map_ref_zero",      route_via_map_ref(m_null) == 0u);
    check("w31_route_hash_map_through_map_ref_zero", route_via_map_ref(hm_null) == 0u);
    check("w31_route_hash_map_bogus_through_map_ref_zero",
          route_via_map_ref(hm_bogus) == 0u);
}

int main()
{
    // Registering the element wrappers mirrors real usage; harmless with no JVM.
    vmhook::register_class<elem_w>("test/Element");
    vmhook::register_class<key_w>("test/Key");
    vmhook::register_class<val_w>("test/Value");

    test_type_tags_are_distinct();
    test_inheritance_lattice();
    test_tag_construction_traits();
    test_default_null_construction();
    test_wrapper_copy_move_semantics();
    test_size_and_is_empty_null_safe();
    test_collection_map_helper_parity();
    test_to_vector_empty_no_jvm();
    test_to_entries_empty_no_jvm();
    test_substitutability_through_base_ref();
    test_default_value_t_empty();
    test_value_t_all_alternatives_empty();
    test_value_t_array_signature_matrix();
    test_value_t_array_gate_byte_sweep();
    test_value_t_variant_completeness();
    test_value_t_compressed_oop_value_sweep();
    test_value_t_alternative_signature_cross();
    test_value_t_signature_robustness();
    test_value_t_via_field_proxy_empty();
    test_field_proxy_all_signatures_empty();
    test_value_t_call_forms();
    test_tag_lattice_total_partition();
    test_decode_oop_pointer_all_null_no_jvm();
    test_is_valid_pointer_boundaries();
    test_sig_char_to_basic_type_full_sweep();
    test_jvm_primitive_byte_width_matrix();
    test_clamp_safe_container_count();
    test_value_t_convertible_target_gate();
    test_element_wrapper_contract();
    test_array_length_null_and_invalid();
    test_get_array_element_null_and_oob();
    test_read_java_string_null_and_invalid();
    test_value_t_as_string_all_alternatives();
    test_value_t_aggregate_field_semantics();
    test_narrow_codec_roundtrip();
    test_function_traits_decomposition();
    test_jvm_descriptor_for_arg_mapping();
    test_capability_macro_consistency();
    test_is_unique_ptr_trait();
    test_return_slot_encoding();
    test_array_element_real_buffer();
    test_jni_signature_width_ladder_and_packing();
    test_decode_u5_threaded_record_walk();
    test_w25_collection_list_cold_state();
    test_w31_hash_tree_map_cold_state();

    return failures == 0 ? 0 : 1;
}
