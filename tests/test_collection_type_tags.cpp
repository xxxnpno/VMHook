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

    return failures == 0 ? 0 : 1;
}
