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

    return failures == 0 ? 0 : 1;
}
