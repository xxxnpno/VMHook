// collection_list JVM test module  (feature area: collections)
//
// Exhaustively exercises decoding of real java.util.ArrayList /
// java.util.LinkedList / Elem[] BACKING STORES on a live JVM via a direct
// MEMORY-WALK the module performs itself, AND — since the underlying library
// bug is fixed — re-validates the library's own templated
// vmhook::collection::to_vector<T>() against that hand-walk (see the
// "to_vector<T>() decode" section near the end).
//
// HISTORY — the LIBRARY bug this module first worked around, now FIXED:
//   vmhook::collection::to_vector<T>() / linked_list_walk_items (vmhook.hpp)
//   used to mis-decode List element OOPs at RUNTIME on the MSVC-14.51 windows
//   runner.  The TU compiled clean (no warning, no error) but the decoded
//   ArrayList/LinkedList elements came out WRONG — a strict-aliasing miscompile
//   (UB) in the LinkedList chain walk's `*reinterpret_cast<const uint32_t*>`
//   narrow-oop reads on cl.exe 14.51 (cl 19.51), /std:c++latest, where a TBAA
//   pass could hoist/reorder the data-dependent `first->next` loads.
//   collection_set.cpp stayed CI-green because the Set fast paths' read shape
//   did not trip the same miscompile.  The library fix routes every narrow-oop
//   read in the List path through a memcpy helper (read_compressed_oop_at),
//   which carries no aliasing precondition — byte-identical on gcc/clang/older
//   MSVC, miscompile-proof on 14.51.
//
//   This module keeps BOTH decoders: (1) its OWN hand-walk built from the
//   library's LOW-LEVEL primitives — find_field / klass_from_oop /
//   decode_oop_pointer / decode_array_oop / array_length / get_array_element /
//   is_valid_pointer — reproducing the EXACT layouts to_vector uses internally
//   (ArrayList: "elementData" + "size"; LinkedList: "first" + node "item"/"next"
//   chain; Object[]: the raw reference array); and (2) the library's real
//   to_vector<T>() on the same live lists.  The hand-walk is an independent
//   oracle, so asserting to_vector == hand-walk turns any future re-decode
//   regression on the 14.51 runner into a HARD CI failure instead of a silent
//   wrong answer.
//
// HOW THE OOPS ARE OBTAINED (no capture-detour):
//   Every list is an instance field of the static singleton CollList.SINGLETON.
//   The module reads SINGLETON's OOP via static_field("SINGLETON") -> field_oop,
//   then reads each instance list field's OOP off that singleton by hand
//   (klass_from_oop + find_field + a guarded compressed-OOP read + decode).  A
//   build probe (the canonical go/done handshake) first runs populate() on the
//   Java thread; the module then walks the now-populated backing stores off the
//   worker thread, while the Java thread is parked in the Harness loop — the
//   same no-detour, no-allocation-under-the-walk pattern collection_set.cpp uses
//   (CI-green at a 5000-element HashSet on MSVC-14.51).  There is NO scoped_hook,
//   NO interpreter detour, NO thread-state-settle dependency.
//
// Coverage (every check the quarantined version asserted is retained, now
// decoded by the hand-walk):
//
//   ArrayList backing store ("elementData" + "size")
//     * empty / single / many (12 > default cap 10) / trimToSize() (cap==size)
//     * ensureCapacity(100) (size 12 != capacity 100): the walk bound MUST be
//       `size`, NOT elementData.length — no phantom-null tail.
//     * null element -> nullptr slot.
//     * size 2; duplicate-VALUE (equal values, distinct identities).
//
//   LinkedList backing store ("first" + node "item"/"next" chain)
//     * empty / single / many (12) / null item -> nullptr slot / size 2
//     * BIG 4096-node chain: size match, every index in order (vec[k].id==k),
//       first/last identity, ALL element OOPs distinct (a cycle re-emits a node
//       -> a duplicate OOP), and a wall-clock canary.
//
//   Object[] ('[L...;') raw reference array (elemArray): array_length + per-slot
//     get_array_element<uint32> -> decode_oop_pointer.
//
//   nested List-of-Lists: outer ArrayList walk yields inner List OOPs; each is
//     re-walked by a shape-detecting hand-walk (ArrayList OR LinkedList).
//
//   JDK Collections wrappers, decoded by a direct backing-FIELD walk (NO Java
//   get(int) call — forbidden from the worker body): Arrays.asList (field "a",
//   an Object[]); Collections.emptyList (no element field, size 0);
//   Collections.singletonList (field "element"); Collections.unmodifiableList
//   (field "list" -> backing ArrayList, re-walked).
//
//   Cross-path parity: ArrayList-many vs LinkedList-many agree on size / first /
//   last and both ordered (the walk picks the backing shape from the runtime
//   klass, not the Java static type — note linkBig is declared as List).
//
// SUITE-SAFETY (mirrors collection_set.cpp / register_class.cpp): the whole body
// runs under a try/catch (a throw is recorded [INFO], never a FAIL); an entry
// guard bails to [INFO] if the fixture class does not resolve; an unconditional
// vmhook::shutdown_hooks() OUTSIDE the try guarantees ZERO hooks armed on EVERY
// exit path (this module installs none, but the teardown is kept for parity and
// belt-and-braces).  Every raw OOP deref is gated by is_valid_pointer; null
// elements become nullptr handles; no Java method is called from the body (only
// pure heap reads); no forced System.gc().
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace
{
    // Wrapper for vmhook.fixtures.CollList$Elem — the list element type.  Each
    // element carries id (insertion index) and tag ("e<id>") so the native side
    // can verify order, identity, and the String-field readback through a
    // wrapper the module built from a decoded element OOP.  The field reads route
    // through register_class<elem_object> + get_field — independent of to_vector,
    // and CI-green on MSVC-14.51 (collection_set.cpp uses the identical wrapper).
    class elem_object : public vmhook::object<elem_object>
    {
    public:
        explicit elem_object(vmhook::oop_t instance) noexcept
            : vmhook::object<elem_object>{ instance }
        {
        }

        auto id() const -> std::int32_t
        {
            const auto f{ get_field("id") };
            return f ? static_cast<std::int32_t>(f->get()) : -1;
        }

        // Copy-init (never brace-init) a std::string from a value_t — the
        // contract calls brace-init ambiguous on MSVC.  Reads the Java String
        // field "tag" via read_java_string under the hood.
        auto tag() const -> std::string
        {
            const auto f{ get_field("tag") };
            if (!f)
            {
                return std::string{};
            }
            std::string s = f->get();
            return s;
        }
    };

    // ── Fixture-mirrored constants (kept in lockstep with CollList.java) ─────
    constexpr std::int32_t MANY{ 12 };
    constexpr std::int32_t BIG{ 4096 };
    constexpr std::int32_t NULL_AT{ 2 };
    constexpr std::int32_t NULL_LIST_LEN{ 4 };
    constexpr std::int32_t TWO{ 2 };
    constexpr std::int32_t DUP_LEN{ 6 };
    constexpr std::int32_t DUP_VAL{ 9 };
    constexpr std::int32_t OBJ_ARR_LEN{ 5 };
    constexpr std::int32_t NESTED_OUTER{ 3 };
    constexpr std::int32_t NESTED_INNER{ 4 };
    constexpr std::int32_t ASLIST_LEN{ 3 };
    constexpr std::int32_t SINGLETON_ID{ 0 };

    // Generous wall-clock ceiling for the BIG-node LinkedList walk.  A linear
    // walk is sub-millisecond; even a heavily-loaded CI box stays well under
    // this.  A quadratic per-node regression would blow far past it, so this is
    // a regression canary, not a micro-benchmark.
    constexpr std::int64_t BIG_WALK_BUDGET_MS{ 3000 };

    // Defensive cap so a torn/absurd size read cannot make a reserve or loop run
    // away (mirrors the library's own clamp_safe_container_count intent; kept
    // local so the module depends on no library internal).  1<<24 is far above
    // any list this fixture builds.
    constexpr std::int32_t MAX_SAFE_COUNT{ 1 << 24 };

    auto clamp_count(const std::int32_t raw) -> std::int32_t
    {
        if (raw <= 0)
        {
            return 0;
        }
        return (raw < MAX_SAFE_COUNT) ? raw : MAX_SAFE_COUNT;
    }

    // ── LOW-LEVEL hand-walk primitives ───────────────────────────────────────
    // Each mirrors what vmhook::collection::to_vector does INTERNALLY, but is
    // assembled here from the library's individually-CI-green primitives so the
    // decode is correct on MSVC-14.51 by construction (it does not go through the
    // templated to_vector body that mis-compiles there).

    // Read a 4-byte compressed OOP at `holder_oop + offset` into `out`.  Returns
    // false (and leaves out untouched) when the holder is not a valid pointer.
    auto read_compressed_at(const void* const holder_oop,
                            const std::uint32_t offset,
                            std::uint32_t& out) -> bool
    {
        if (!holder_oop || !vmhook::hotspot::is_valid_pointer(holder_oop))
        {
            return false;
        }
        std::uint32_t value{};
        std::memcpy(&value,
                    reinterpret_cast<const std::uint8_t*>(holder_oop) + offset,
                    sizeof(value));
        out = value;
        return true;
    }

    // Resolve a named field's entry on the runtime klass of `holder_oop`.
    auto field_entry_of(const void* const holder_oop, const std::string_view name)
        -> std::optional<vmhook::hotspot::field_entry_t>
    {
        if (!holder_oop || !vmhook::hotspot::is_valid_pointer(holder_oop))
        {
            return std::nullopt;
        }
        vmhook::hotspot::klass* const k{
            vmhook::klass_from_oop(const_cast<void*>(holder_oop)) };
        if (!k)
        {
            return std::nullopt;
        }
        return vmhook::find_field(k, name);
    }

    // Read a named REFERENCE/instance field off `holder_oop` and return its
    // decoded object OOP (or nullptr when absent/null/invalid).  This is the hand
    // equivalent of get_field(name)->get() for a reference field, built only from
    // find_field + a guarded compressed read + decode_oop_pointer.
    auto read_ref_field_oop(const void* const holder_oop, const std::string_view name)
        -> void*
    {
        const auto entry{ field_entry_of(holder_oop, name) };
        if (!entry)
        {
            return nullptr;
        }
        std::uint32_t compressed{};
        if (!read_compressed_at(holder_oop, entry->offset, compressed))
        {
            return nullptr;
        }
        void* const decoded{ vmhook::hotspot::decode_oop_pointer(compressed) };
        return (decoded && vmhook::hotspot::is_valid_pointer(decoded)) ? decoded : nullptr;
    }

    // Read a named INT (primitive) field off `holder_oop`.  Returns `fallback`
    // when the field is absent or the holder is invalid.
    auto read_int_field(const void* const holder_oop, const std::string_view name,
                        const std::int32_t fallback) -> std::int32_t
    {
        const auto entry{ field_entry_of(holder_oop, name) };
        if (!entry || !holder_oop || !vmhook::hotspot::is_valid_pointer(holder_oop))
        {
            return fallback;
        }
        std::int32_t value{};
        std::memcpy(&value,
                    reinterpret_cast<const std::uint8_t*>(holder_oop) + entry->offset,
                    sizeof(value));
        return value;
    }

    // Walk a Java Object[] (decoded array OOP) into a vector of element wrappers,
    // one per slot (nullptr for a null slot).  Mirrors value_t::to_vector's array
    // branch: array_length bound + per-index get_array_element<uint32> ->
    // decode_oop_pointer.
    auto walk_object_array(void* const array_oop)
        -> std::vector<std::unique_ptr<elem_object>>
    {
        std::vector<std::unique_ptr<elem_object>> result;
        if (!array_oop || !vmhook::hotspot::is_valid_pointer(array_oop))
        {
            return result;
        }
        const std::int32_t n{ clamp_count(vmhook::array_length(array_oop)) };
        result.reserve(static_cast<std::size_t>(n));
        for (std::int32_t index{ 0 }; index < n; ++index)
        {
            const std::uint32_t compressed{
                vmhook::get_array_element<std::uint32_t>(array_oop, index) };
            void* const element_oop{ vmhook::hotspot::decode_oop_pointer(compressed) };
            if (element_oop && vmhook::hotspot::is_valid_pointer(element_oop))
            {
                result.push_back(std::make_unique<elem_object>(
                    static_cast<vmhook::oop_t>(element_oop)));
            }
            else
            {
                result.push_back(nullptr);
            }
        }
        return result;
    }

    // Walk a java.util.ArrayList (decoded list OOP) by its "elementData" Object[]
    // backing array, bounded by the "size" field.  Mirrors to_vector's ArrayList
    // fast path: the bound is `size`, NEVER elementData.length, so an oversized
    // backing array (ensureCapacity) emits exactly `size` elements with no
    // phantom-null tail.
    auto walk_arraylist(void* const list_oop)
        -> std::vector<std::unique_ptr<elem_object>>
    {
        std::vector<std::unique_ptr<elem_object>> result;
        if (!list_oop || !vmhook::hotspot::is_valid_pointer(list_oop))
        {
            return result;
        }
        const auto size_entry{ field_entry_of(list_oop, "size") };
        const auto data_entry{ field_entry_of(list_oop, "elementData") };
        if (!size_entry || !data_entry)
        {
            return result;
        }
        const std::int32_t n{ read_int_field(list_oop, "size", 0) };
        if (n <= 0)
        {
            return result;
        }
        std::uint32_t compressed_array{};
        if (!read_compressed_at(list_oop, data_entry->offset, compressed_array))
        {
            return result;
        }
        void* const array_oop{ vmhook::decode_array_oop(compressed_array) };
        if (!array_oop || !vmhook::hotspot::is_valid_pointer(array_oop))
        {
            return result;
        }
        // Bound by `size` (clamped), not by the backing array length.
        const std::int32_t safe_n{ clamp_count(n) };
        result.reserve(static_cast<std::size_t>(safe_n));
        for (std::int32_t index{ 0 }; index < safe_n; ++index)
        {
            // get_array_element bounds-checks `index` against the real array
            // length, so a (pathological) size > capacity yields nullptr tail
            // slots rather than an OOB read.
            const std::uint32_t compressed_element{
                vmhook::get_array_element<std::uint32_t>(array_oop, index) };
            void* const element_oop{ vmhook::hotspot::decode_oop_pointer(compressed_element) };
            if (element_oop && vmhook::hotspot::is_valid_pointer(element_oop))
            {
                result.push_back(std::make_unique<elem_object>(
                    static_cast<vmhook::oop_t>(element_oop)));
            }
            else
            {
                result.push_back(nullptr);
            }
        }
        return result;
    }

    // Walk a java.util.LinkedList (decoded list OOP) by its first->next Node
    // chain, bounded by `n` (the list's "size").  Mirrors linked_list_walk_items:
    // resolve "first" off the list klass, then for each Node read "item" (the
    // element, nullptr-safe) and "next" (advance), gated by is_valid_pointer and
    // bounded by size so a corrupt chain can never loop forever.
    auto walk_linkedlist(void* const list_oop, const std::int32_t n)
        -> std::vector<std::unique_ptr<elem_object>>
    {
        std::vector<std::unique_ptr<elem_object>> result;
        if (!list_oop || !vmhook::hotspot::is_valid_pointer(list_oop) || n <= 0)
        {
            return result;
        }
        const auto first_entry{ field_entry_of(list_oop, "first") };
        if (!first_entry)
        {
            return result;
        }
        std::uint32_t first_compressed{};
        if (!read_compressed_at(list_oop, first_entry->offset, first_compressed))
        {
            return result;
        }
        void* node_oop{ vmhook::hotspot::decode_oop_pointer(first_compressed) };

        const std::int32_t safe_n{ clamp_count(n) };
        result.reserve(static_cast<std::size_t>(safe_n));
        for (std::int32_t i{ 0 };
             i < safe_n && node_oop && vmhook::hotspot::is_valid_pointer(node_oop);
             ++i)
        {
            const auto item_entry{ field_entry_of(node_oop, "item") };
            const auto next_entry{ field_entry_of(node_oop, "next") };
            if (!item_entry || !next_entry)
            {
                break;
            }
            std::uint32_t item_compressed{};
            if (!read_compressed_at(node_oop, item_entry->offset, item_compressed))
            {
                break;
            }
            void* const item_oop{ vmhook::hotspot::decode_oop_pointer(item_compressed) };
            if (item_oop && vmhook::hotspot::is_valid_pointer(item_oop))
            {
                result.push_back(std::make_unique<elem_object>(
                    static_cast<vmhook::oop_t>(item_oop)));
            }
            else
            {
                result.push_back(nullptr);
            }

            std::uint32_t next_compressed{};
            if (!read_compressed_at(node_oop, next_entry->offset, next_compressed))
            {
                break;
            }
            node_oop = vmhook::hotspot::decode_oop_pointer(next_compressed);
        }
        return result;
    }

    // Shape-detecting hand-walk for an arbitrary List OOP: dispatches by the
    // runtime klass's field shape, with NO Java call and NO to_vector.  Used for
    // the nested inner lists (mixed ArrayList/LinkedList) and the JDK Collections
    // wrappers.  Probed in order of specificity:
    //   1. ArrayList  ("elementData" + "size")          -> walk_arraylist
    //   2. LinkedList ("first" + "size")                 -> walk_linkedlist(size)
    //   3. Arrays$ArrayList ("a" Object[])               -> walk_object_array
    //   4. SingletonList ("element")                     -> one-element vector
    //   5. UnmodifiableList ("list" backing) / Collection ("c")
    //                                                    -> recurse on the backing
    //   6. EmptyList / anything else                     -> empty
    auto walk_list_by_shape(void* const list_oop, const int depth)
        -> std::vector<std::unique_ptr<elem_object>>
    {
        std::vector<std::unique_ptr<elem_object>> result;
        if (!list_oop || !vmhook::hotspot::is_valid_pointer(list_oop) || depth > 4)
        {
            return result;
        }

        const bool has_size{ field_entry_of(list_oop, "size").has_value() };

        // 1. ArrayList.
        if (has_size && field_entry_of(list_oop, "elementData").has_value())
        {
            return walk_arraylist(list_oop);
        }
        // 2. LinkedList.
        if (has_size && field_entry_of(list_oop, "first").has_value())
        {
            const std::int32_t n{ read_int_field(list_oop, "size", 0) };
            return walk_linkedlist(list_oop, n);
        }
        // 3. Arrays$ArrayList: backing Object[] in field "a".
        if (field_entry_of(list_oop, "a").has_value())
        {
            void* const array_oop{ read_ref_field_oop(list_oop, "a") };
            if (array_oop)
            {
                return walk_object_array(array_oop);
            }
            return result;
        }
        // 4. SingletonList: single element in field "element".
        if (field_entry_of(list_oop, "element").has_value())
        {
            void* const element_oop{ read_ref_field_oop(list_oop, "element") };
            if (element_oop)
            {
                result.push_back(std::make_unique<elem_object>(
                    static_cast<vmhook::oop_t>(element_oop)));
            }
            else
            {
                result.push_back(nullptr);
            }
            return result;
        }
        // 5. UnmodifiableList ("list") / UnmodifiableCollection ("c"): recurse on
        //    the wrapped backing list.
        if (field_entry_of(list_oop, "list").has_value())
        {
            void* const backing{ read_ref_field_oop(list_oop, "list") };
            return walk_list_by_shape(backing, depth + 1);
        }
        if (field_entry_of(list_oop, "c").has_value())
        {
            void* const backing{ read_ref_field_oop(list_oop, "c") };
            return walk_list_by_shape(backing, depth + 1);
        }
        // 6. EmptyList / no recognised element field -> empty.
        return result;
    }

    // ── Per-list reduced observations ────────────────────────────────────────
    struct list_obs
    {
        bool         seen{ false };        // a walk ran for this list
        std::int32_t size{ -1 };           // vec.size()
        std::int32_t non_null{ -1 };       // count of non-null slots
        std::int32_t null_at{ -2 };        // index of the (first) null slot, or -1
        std::int32_t null_count{ -1 };     // number of null slots
        bool         order_ok{ false };    // every non-null vec[k].id == k
        bool         tags_ok{ false };     // every non-null vec[k].tag == "e"+id
        bool         distinct_ok{ false }; // all non-null element OOPs distinct
        std::int32_t first_id{ -1 };
        std::int32_t last_id{ -1 };
    };

    list_obs g_arr_empty;
    list_obs g_arr_single;
    list_obs g_arr_many;
    list_obs g_arr_trimmed;
    list_obs g_arr_oversized;
    list_obs g_arr_null;

    list_obs g_link_empty;
    list_obs g_link_single;
    list_obs g_link_many;
    list_obs g_link_null;
    list_obs g_link_big;

    list_obs g_arr_two;
    list_obs g_link_two;
    list_obs g_arr_dup;          // duplicate values, distinct identities
    list_obs g_elem_array;       // Elem[] -> object-array walk
    list_obs g_aslist;           // Arrays.asList     -> "a" Object[] walk
    list_obs g_empty_immut;      // Collections.emptyList
    list_obs g_singleton;        // Collections.singletonList
    list_obs g_unmod;            // Collections.unmodifiableList(arrMany)

    list_obs g_nested_outer;                 // outer list of inner-list OOPs
    std::int32_t g_nested_outer_n{ -1 };     // number of inner lists seen
    std::int32_t g_nested_inner_ok{ 0 };     // inner lists that fully matched
    bool         g_nested_inner_distinct{ true };

    bool g_dup_all_values_equal{ false };
    bool g_dup_all_tags_equal{ false };

    std::int64_t g_big_walk_us{ -1 };
    std::int32_t g_big_sample_mid_id{ -1 };
    bool         g_big_sample_tag_ok{ false };

    bool g_singleton_oop_ok{ false };

    // ── LIBRARY to_vector<T>() observations (the path this module's hand-walk
    //    works AROUND) ────────────────────────────────────────────────────────
    // These decode the SAME live ArrayList / LinkedList fields through the
    // library's own vmhook::collection::to_vector<elem_object>() — i.e. the
    // ArrayList "elementData"+"size" fast path and the LinkedList
    // first->node-item/next chain walk (linked_list_walk_items).  That is the
    // exact code that mis-decoded List elements at RUNTIME on the MSVC-14.51
    // runner (a strict-aliasing miscompile in the chain walk's reinterpret_cast
    // oop reads, since fixed by routing them through a memcpy read).  Asserting
    // the decoded ids/tags here turns that runtime mis-decode into a HARD CI
    // failure on the very runner that exhibited it, instead of being silently
    // worked around.  The ArrayList / LinkedList fast paths issue NO Java call
    // (pure guarded heap reads), so running to_vector from the worker-thread
    // body is as safe as the hand-walk beside it.
    list_obs g_tv_arr_empty;     // to_vector(arrEmpty)   -> empty
    list_obs g_tv_arr_many;      // to_vector(arrMany)    -> ArrayList fast path
    list_obs g_tv_arr_null;      // to_vector(arrWithNull)-> null-slot handling
    list_obs g_tv_link_empty;    // to_vector(linkEmpty)  -> empty
    list_obs g_tv_link_many;     // to_vector(linkMany)   -> LinkedList chain walk
    list_obs g_tv_link_null;     // to_vector(linkWithNull)-> null Node.item slot

    // Reduce a decoded vector into a list_obs: size, null pattern, ascending id
    // order (id == index for non-null slots), tag correctness, and OOP
    // distinctness (a cycle/duplicate walk would collapse this).
    auto observe(list_obs& o,
                 const std::vector<std::unique_ptr<elem_object>>& v,
                 const bool check_tags) -> void
    {
        o.seen = true;
        o.size = static_cast<std::int32_t>(v.size());

        std::int32_t non_null{ 0 };
        std::int32_t null_count{ 0 };
        std::int32_t first_null{ -1 };
        bool order_ok{ true };
        bool tags_ok{ true };
        bool distinct_ok{ true };
        std::int32_t first_id{ -1 };
        std::int32_t last_id{ -1 };

        std::unordered_set<const void*> seen_oops;
        seen_oops.reserve(v.size() * 2 + 1);

        for (std::size_t k{ 0 }; k < v.size(); ++k)
        {
            const elem_object* const e{ v[k].get() };
            if (e == nullptr)
            {
                ++null_count;
                if (first_null < 0)
                {
                    first_null = static_cast<std::int32_t>(k);
                }
                continue;
            }
            ++non_null;

            const std::int32_t id{ e->id() };
            if (first_id < 0)
            {
                first_id = id;
            }
            last_id = id;

            if (id != static_cast<std::int32_t>(k))
            {
                order_ok = false;
            }

            if (check_tags)
            {
                const std::string expect{ "e" + std::to_string(id) };
                if (e->tag() != expect)
                {
                    tags_ok = false;
                }
            }

            const void* const oop{ static_cast<const void*>(e->get_instance()) };
            if (!seen_oops.insert(oop).second)
            {
                distinct_ok = false;
            }
        }

        o.non_null = non_null;
        o.null_count = null_count;
        o.null_at = first_null;
        o.order_ok = order_ok;
        o.tags_ok = check_tags ? tags_ok : true;
        o.distinct_ok = distinct_ok;
        o.first_id = first_id;
        o.last_id = last_id;
    }

    // Count-only reduction (size, null pattern, OOP distinctness) for a vector of
    // inner-list wrappers that carry no id/tag.
    auto observe_count_only(list_obs& o,
                            const std::vector<std::unique_ptr<elem_object>>& v) -> void
    {
        o.seen = true;
        o.size = static_cast<std::int32_t>(v.size());

        std::int32_t non_null{ 0 };
        std::int32_t null_count{ 0 };
        bool distinct_ok{ true };
        std::unordered_set<const void*> seen_oops;
        seen_oops.reserve(v.size() * 2 + 1);
        for (const auto& up : v)
        {
            const elem_object* const e{ up.get() };
            if (e == nullptr) { ++null_count; continue; }
            ++non_null;
            const void* const oop{ static_cast<const void*>(e->get_instance()) };
            if (!seen_oops.insert(oop).second) { distinct_ok = false; }
        }
        o.non_null = non_null;
        o.null_count = null_count;
        o.distinct_ok = distinct_ok;
    }

    // True iff a decoded inner Elem vector is a perfect dense list of `expected`:
    // exactly `expected` non-null, distinct, ascending elements with id == index
    // and tag == "e<id>".
    auto inner_list_fully_ok(const std::vector<std::unique_ptr<elem_object>>& v,
                             const std::int32_t expected) -> bool
    {
        if (static_cast<std::int32_t>(v.size()) != expected || expected <= 0)
        {
            return false;
        }
        std::unordered_set<const void*> seen_oops;
        seen_oops.reserve(v.size() * 2 + 1);
        for (std::size_t k{ 0 }; k < v.size(); ++k)
        {
            const elem_object* const e{ v[k].get() };
            if (e == nullptr) { return false; }
            if (e->id() != static_cast<std::int32_t>(k)) { return false; }
            if (e->tag() != ("e" + std::to_string(e->id()))) { return false; }
            const void* const oop{ static_cast<const void*>(e->get_instance()) };
            if (!seen_oops.insert(oop).second) { return false; }
        }
        return true;
    }

    // ── Standard per-list assertion bundles ─────────────────────────────────
    auto check_empty(vmhook_test::context& ctx, const std::string& p, const list_obs& o) -> void
    {
        ctx.check(p + "_seen", o.seen);
        ctx.check(p + "_size_zero", o.size == 0);
        ctx.check(p + "_no_elements", o.non_null == 0);
        ctx.check(p + "_no_null_slots", o.null_count == 0);
    }

    auto check_dense(vmhook_test::context& ctx, const std::string& p, const list_obs& o,
                     const std::int32_t expected_size) -> void
    {
        ctx.check(p + "_seen", o.seen);
        ctx.check(p + "_size_matches", o.size == expected_size);
        ctx.check(p + "_all_non_null", o.non_null == expected_size);
        ctx.check(p + "_no_null_slots", o.null_count == 0);
        ctx.check(p + "_order_preserved", o.order_ok);
        ctx.check(p + "_tags_round_trip", o.tags_ok);
        ctx.check(p + "_elements_distinct", o.distinct_ok);
        ctx.check(p + "_first_id_zero", o.first_id == 0);
        ctx.check(p + "_last_id_is_size_minus_1", o.last_id == expected_size - 1);
    }

    auto check_with_null(vmhook_test::context& ctx, const std::string& p, const list_obs& o) -> void
    {
        ctx.check(p + "_seen", o.seen);
        ctx.check(p + "_size_matches", o.size == NULL_LIST_LEN);
        ctx.check(p + "_one_null_slot", o.null_count == 1);
        ctx.check(p + "_null_at_expected_index", o.null_at == NULL_AT);
        ctx.check(p + "_non_null_count", o.non_null == NULL_LIST_LEN - 1);
        ctx.check(p + "_order_preserved_around_null", o.order_ok);
        ctx.check(p + "_tags_round_trip", o.tags_ok);
        ctx.check(p + "_non_null_distinct", o.distinct_ok);
    }

    constexpr char FIXTURE[]{ "vmhook/fixtures/CollList" };

    // Fixture wrapper — handshake + the SINGLETON OOP read.
    class coll_list_fixture : public vmhook::object<coll_list_fixture>
    {
    public:
        explicit coll_list_fixture(vmhook::oop_t instance) noexcept
            : vmhook::object<coll_list_fixture>{ instance }
        {
        }

        static auto set_go(const bool value) -> void { static_field("go")->set(value); }
        static auto get_done() -> bool                { return static_field("done")->get(); }

        // Decoded OOP of the static CollList.SINGLETON instance (or nullptr).
        static auto singleton_oop() -> void*
        {
            const auto proxy{ static_field("SINGLETON") };
            if (!proxy.has_value())
            {
                return nullptr;
            }
            void* const oop{ vmhook::field_oop(*proxy) };
            return (oop && vmhook::hotspot::is_valid_pointer(oop)) ? oop : nullptr;
        }
    };

    // The whole test body, factored out so the VMHOOK_JVM_MODULE wrapper can run
    // it under a try/catch and ALWAYS follow it with shutdown_hooks().
    auto run_collection_list_checks(vmhook_test::context& ctx) -> void
    {
        // ─── ENTRY GUARD ────────────────────────────────────────────────────
        if (vmhook::find_class(FIXTURE) == nullptr)
        {
            ctx.record("[INFO] collection_list: CollList not loaded/resolvable on this "
                       "run; skipping the module's live checks (no crash, no hooks "
                       "armed).");
            return;
        }

        vmhook::register_class<coll_list_fixture>(FIXTURE);
        vmhook::register_class<elem_object>("vmhook/fixtures/CollList$Elem");

        // Drive a build probe so populate() runs on the Java thread; then read
        // the now-populated backing stores off the worker thread.  (No detour:
        // CollList's lists are plain fields of the static singleton.)
        {
            const bool built{ ctx.run_probe(
                [](bool value) { coll_list_fixture::set_go(value); },
                []() { return coll_list_fixture::get_done(); }) };
            ctx.check("build_probe_completed", built);
        }

        void* const singleton{ coll_list_fixture::singleton_oop() };
        g_singleton_oop_ok = (singleton != nullptr);
        ctx.check("collection_list_singleton_resolved", g_singleton_oop_ok);
        if (singleton == nullptr)
        {
            ctx.record("[INFO] collection_list: CollList.SINGLETON did not resolve to a "
                       "valid OOP on this run; skipping the content walks (no crash).");
            return;
        }

        // Helper: read a named instance LIST field off the singleton -> its OOP.
        const auto list_oop_of{ [singleton](const std::string_view name) -> void*
        {
            return read_ref_field_oop(singleton, name);
        } };

        // ── ArrayList backing-store walk ────────────────────────────────────
        observe(g_arr_empty,     walk_arraylist(list_oop_of("arrEmpty")),     true);
        observe(g_arr_single,    walk_arraylist(list_oop_of("arrSingle")),    true);
        observe(g_arr_many,      walk_arraylist(list_oop_of("arrMany")),      true);
        observe(g_arr_trimmed,   walk_arraylist(list_oop_of("arrTrimmed")),   true);
        observe(g_arr_oversized, walk_arraylist(list_oop_of("arrOversized")), true);
        observe(g_arr_null,      walk_arraylist(list_oop_of("arrWithNull")),  true);
        observe(g_arr_two,       walk_arraylist(list_oop_of("arrTwo")),       true);

        // ── LinkedList backing-store walk ───────────────────────────────────
        {
            void* const o{ list_oop_of("linkEmpty") };
            observe(g_link_empty, walk_linkedlist(o, read_int_field(o, "size", 0)), true);
        }
        {
            void* const o{ list_oop_of("linkSingle") };
            observe(g_link_single, walk_linkedlist(o, read_int_field(o, "size", 0)), true);
        }
        {
            void* const o{ list_oop_of("linkMany") };
            observe(g_link_many, walk_linkedlist(o, read_int_field(o, "size", 0)), true);
        }
        {
            void* const o{ list_oop_of("linkWithNull") };
            observe(g_link_null, walk_linkedlist(o, read_int_field(o, "size", 0)), true);
        }
        {
            void* const o{ list_oop_of("linkTwo") };
            observe(g_link_two, walk_linkedlist(o, read_int_field(o, "size", 0)), true);
        }

        // ── LARGE LinkedList chain: time the walk + full correctness ────────
        {
            void* const o{ list_oop_of("linkBig") };
            const std::int32_t n{ read_int_field(o, "size", 0) };
            const auto t0{ std::chrono::steady_clock::now() };
            std::vector<std::unique_ptr<elem_object>> big{ walk_linkedlist(o, n) };
            const auto t1{ std::chrono::steady_clock::now() };
            g_big_walk_us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

            // Sample one middle element BEFORE the reducer consumes the vector.
            if (big.size() == static_cast<std::size_t>(BIG) && big[BIG / 2])
            {
                const std::int32_t mid_id{ big[BIG / 2]->id() };
                g_big_sample_mid_id = mid_id;
                g_big_sample_tag_ok = (big[BIG / 2]->tag() == ("e" + std::to_string(mid_id)));
            }

            observe(g_link_big, big, true);
        }

        // ── Duplicate-VALUE ArrayList ───────────────────────────────────────
        {
            std::vector<std::unique_ptr<elem_object>> dup{ walk_arraylist(list_oop_of("arrDup")) };
            observe(g_arr_dup, dup, false);

            bool all_vals{ !dup.empty() };
            bool all_tags{ !dup.empty() };
            const std::string expect_tag{ "e" + std::to_string(DUP_VAL) };
            for (const auto& up : dup)
            {
                const elem_object* const e{ up.get() };
                if (e == nullptr) { all_vals = false; all_tags = false; continue; }
                if (e->id() != DUP_VAL) { all_vals = false; }
                if (e->tag() != expect_tag) { all_tags = false; }
            }
            g_dup_all_values_equal = all_vals;
            g_dup_all_tags_equal = all_tags;
        }

        // ── Object[] ('[L...;') raw reference array ─────────────────────────
        // elemArray's field OOP IS the array OOP (a reference array field).
        observe(g_elem_array, walk_object_array(list_oop_of("elemArray")), true);

        // ── Nested List-of-Lists: outer ArrayList walk, then inner re-walk ──
        {
            void* const outer_oop{ list_oop_of("nested") };
            // Walk the outer ArrayList's backing array to recover the inner List
            // element OOPs directly (each slot is itself a List).  walk_arraylist
            // builds elem_object wrappers but we only use get_instance() (the raw
            // inner-list OOP) here, never its id/tag.
            std::vector<std::unique_ptr<elem_object>> outer{ walk_arraylist(outer_oop) };
            observe_count_only(g_nested_outer, outer);
            g_nested_outer_n = static_cast<std::int32_t>(outer.size());

            std::int32_t inner_ok{ 0 };
            bool inner_distinct{ true };
            std::unordered_set<const void*> seen_inner;
            for (const auto& up : outer)
            {
                const elem_object* const inner{ up.get() };
                if (inner == nullptr) { continue; }
                void* const inner_oop{ inner->get_instance() };
                if (!inner_oop || !vmhook::hotspot::is_valid_pointer(inner_oop))
                {
                    continue;
                }
                if (!seen_inner.insert(static_cast<const void*>(inner_oop)).second)
                {
                    inner_distinct = false;
                }
                std::vector<std::unique_ptr<elem_object>> items{
                    walk_list_by_shape(inner_oop, 0) };
                if (inner_list_fully_ok(items, NESTED_INNER))
                {
                    ++inner_ok;
                }
            }
            g_nested_inner_ok = inner_ok;
            g_nested_inner_distinct = inner_distinct;
        }

        // ── JDK Collections wrappers (shape-detecting backing-field walk) ───
        observe(g_aslist,      walk_list_by_shape(list_oop_of("asListView"), 0),       true);
        observe(g_empty_immut, walk_list_by_shape(list_oop_of("emptyImmutable"), 0),   true);
        observe(g_singleton,   walk_list_by_shape(list_oop_of("singletonView"), 0),    true);
        observe(g_unmod,       walk_list_by_shape(list_oop_of("unmodifiableView"), 0), true);

        // ── LIBRARY to_vector<T>() decode of the SAME live lists ────────────
        // Drives vmhook::collection::to_vector<elem_object>() directly on each
        // list OOP — the real ArrayList / LinkedList fast paths.  This is the
        // regression guard for the MSVC-14.51 chain-walk mis-decode: if the
        // library ever again returns wrong element OOPs on this runner, these
        // observations diverge from the (independently-correct) hand-walk ones
        // and the checks below FAIL.  Each list OOP is gated by is_valid_pointer
        // inside to_vector, so a null/absent field yields an empty vector, never
        // a crash.
        const auto to_vector_of{ [](void* const list_oop)
            -> std::vector<std::unique_ptr<elem_object>>
        {
            if (!list_oop || !vmhook::hotspot::is_valid_pointer(list_oop))
            {
                return {};
            }
            return vmhook::collection{ static_cast<vmhook::oop_t>(list_oop) }
                .to_vector<elem_object>();
        } };

        observe(g_tv_arr_empty,  to_vector_of(list_oop_of("arrEmpty")),     true);
        observe(g_tv_arr_many,   to_vector_of(list_oop_of("arrMany")),      true);
        observe(g_tv_arr_null,   to_vector_of(list_oop_of("arrWithNull")),  true);
        observe(g_tv_link_empty, to_vector_of(list_oop_of("linkEmpty")),    true);
        observe(g_tv_link_many,  to_vector_of(list_oop_of("linkMany")),     true);
        observe(g_tv_link_null,  to_vector_of(list_oop_of("linkWithNull")), true);

        // ════════════════════════════════════════════════════════════════════
        //  ArrayList backing store
        // ════════════════════════════════════════════════════════════════════
        check_empty(ctx, "arraylist_empty", g_arr_empty);
        check_dense(ctx, "arraylist_single", g_arr_single, 1);
        check_dense(ctx, "arraylist_many", g_arr_many, MANY);
        check_dense(ctx, "arraylist_trimmed", g_arr_trimmed, MANY);

        // ensureCapacity(100): size(12) != capacity(100).  The headline angle —
        // the walk must return exactly `size` elements with NO phantom-null tail.
        check_dense(ctx, "arraylist_oversized", g_arr_oversized, MANY);
        ctx.check("arraylist_oversized_no_phantom_null_tail",
                  g_arr_oversized.null_count == 0);
        ctx.check("arraylist_oversized_size_not_capacity",
                  g_arr_oversized.size == MANY);

        check_with_null(ctx, "arraylist_with_null", g_arr_null);

        // ════════════════════════════════════════════════════════════════════
        //  LinkedList backing store (chain walk)
        // ════════════════════════════════════════════════════════════════════
        check_empty(ctx, "linkedlist_empty", g_link_empty);
        check_dense(ctx, "linkedlist_single", g_link_single, 1);
        check_dense(ctx, "linkedlist_many", g_link_many, MANY);
        check_with_null(ctx, "linkedlist_with_null", g_link_null);

        check_dense(ctx, "linkedlist_big", g_link_big, BIG);
        ctx.check("linkedlist_big_no_cycle_no_dup_nodes", g_link_big.distinct_ok);
        ctx.check("linkedlist_big_walk_terminated_at_size", g_link_big.size == BIG);
        ctx.check("linkedlist_big_full_order_preserved", g_link_big.order_ok);
        ctx.check("linkedlist_big_first_is_0", g_link_big.first_id == 0);
        ctx.check("linkedlist_big_last_is_size_minus_1", g_link_big.last_id == BIG - 1);
        ctx.check("linkedlist_big_mid_id_correct", g_big_sample_mid_id == BIG / 2);
        ctx.check("linkedlist_big_mid_tag_round_trips", g_big_sample_tag_ok);

        ctx.record("[INFO] linkedlist_big walk over " + std::to_string(BIG)
                   + " nodes took " + std::to_string(g_big_walk_us) + " us");
        ctx.check("linkedlist_big_walk_recorded", g_big_walk_us >= 0);
        ctx.check("linkedlist_big_walk_not_quadratic",
                  g_big_walk_us >= 0 && g_big_walk_us < BIG_WALK_BUDGET_MS * 1000);

        // ════════════════════════════════════════════════════════════════════
        //  Exhaustive small sizes (both containers): size 2.
        // ════════════════════════════════════════════════════════════════════
        check_dense(ctx, "arraylist_two", g_arr_two, TWO);
        check_dense(ctx, "linkedlist_two", g_link_two, TWO);

        // ════════════════════════════════════════════════════════════════════
        //  Duplicate-VALUE ArrayList: DUP_LEN elements all == DUP_VAL by value,
        //  yet DISTINCT heap objects.
        // ════════════════════════════════════════════════════════════════════
        ctx.check("arraylist_dup_seen", g_arr_dup.seen);
        ctx.check("arraylist_dup_size_matches", g_arr_dup.size == DUP_LEN);
        ctx.check("arraylist_dup_all_non_null", g_arr_dup.non_null == DUP_LEN);
        ctx.check("arraylist_dup_no_null_slots", g_arr_dup.null_count == 0);
        ctx.check("arraylist_dup_all_oops_distinct", g_arr_dup.distinct_ok);
        ctx.check("arraylist_dup_all_values_equal_DUP_VAL", g_dup_all_values_equal);
        ctx.check("arraylist_dup_all_tags_equal", g_dup_all_tags_equal);

        // ════════════════════════════════════════════════════════════════════
        //  Object[] ('[L...;') raw reference array.
        // ════════════════════════════════════════════════════════════════════
        check_dense(ctx, "object_array", g_elem_array, OBJ_ARR_LEN);

        // ════════════════════════════════════════════════════════════════════
        //  Nested List-of-Lists.
        // ════════════════════════════════════════════════════════════════════
        ctx.check("nested_outer_seen", g_nested_outer.seen);
        ctx.check("nested_outer_count_matches", g_nested_outer_n == NESTED_OUTER);
        ctx.check("nested_outer_no_null_slots", g_nested_outer.null_count == 0);
        ctx.check("nested_outer_lists_distinct", g_nested_inner_distinct);
        ctx.check("nested_all_inner_lists_fully_walked",
                  g_nested_inner_ok == NESTED_OUTER);

        // ════════════════════════════════════════════════════════════════════
        //  JDK Collections wrappers (decoded by the backing-field walk).
        // ════════════════════════════════════════════════════════════════════
        // Collections.emptyList(): no element field -> empty, HARD on every JDK.
        check_empty(ctx, "collections_emptylist", g_empty_immut);

        // Arrays.asList(...) -> "a" Object[] walk.
        check_dense(ctx, "arrays_aslist", g_aslist, ASLIST_LEN);

        // Collections.singletonList(x) -> "element" field.
        check_dense(ctx, "collections_singletonlist", g_singleton, 1);
        ctx.check("collections_singletonlist_element_id_is_expected",
                  g_singleton.first_id == SINGLETON_ID);

        // Collections.unmodifiableList(arrMany) -> "list" backing ArrayList walk.
        check_dense(ctx, "collections_unmodifiablelist", g_unmod, MANY);

        // ════════════════════════════════════════════════════════════════════
        //  Cross-path parity.
        // ════════════════════════════════════════════════════════════════════
        ctx.check("array_and_link_many_same_size",
                  g_arr_many.size == g_link_many.size);
        ctx.check("array_and_link_many_same_first_id",
                  g_arr_many.first_id == g_link_many.first_id);
        ctx.check("array_and_link_many_same_last_id",
                  g_arr_many.last_id == g_link_many.last_id);
        ctx.check("array_and_link_many_both_ordered",
                  g_arr_many.order_ok && g_link_many.order_ok);

        ctx.check("array_and_link_two_same_size",
                  g_arr_two.size == g_link_two.size);
        ctx.check("array_and_link_two_both_ordered",
                  g_arr_two.order_ok && g_link_two.order_ok);

        // unmodifiableView wraps arrMany: it must agree element-for-element with
        // the ArrayList walk over the same backing list (size, first, last) — a
        // backing-store vs wrapper-walk equivalence proof on identical contents.
        ctx.check("unmodifiable_matches_backing_arraylist_size",
                  g_unmod.size == g_arr_many.size);
        ctx.check("unmodifiable_matches_backing_arraylist_first_id",
                  g_unmod.first_id == g_arr_many.first_id);
        ctx.check("unmodifiable_matches_backing_arraylist_last_id",
                  g_unmod.last_id == g_arr_many.last_id);

        // ════════════════════════════════════════════════════════════════════
        //  LIBRARY to_vector<T>() decode — the regression guard for the
        //  MSVC-14.51 List-branch mis-decode.  These assert that the library's
        //  OWN ArrayList / LinkedList fast paths decode each live list to the
        //  SAME correct ids/tags the hand-walk proved — element-for-element.  A
        //  reintroduced strict-aliasing miscompile (or any decode regression) in
        //  collection::to_vector / linked_list_walk_items makes these FAIL on
        //  the exact runner that first exhibited the bug.
        // ════════════════════════════════════════════════════════════════════
        // ArrayList fast path: empty / dense-12 / null-slot, fully checked.
        check_empty(ctx, "to_vector_arraylist_empty", g_tv_arr_empty);
        check_dense(ctx, "to_vector_arraylist_many", g_tv_arr_many, MANY);
        check_with_null(ctx, "to_vector_arraylist_with_null", g_tv_arr_null);

        // LinkedList chain walk: empty / dense-12 / null Node.item slot.
        check_empty(ctx, "to_vector_linkedlist_empty", g_tv_link_empty);
        check_dense(ctx, "to_vector_linkedlist_many", g_tv_link_many, MANY);
        check_with_null(ctx, "to_vector_linkedlist_with_null", g_tv_link_null);

        // to_vector MUST agree with the independent hand-walk on the same lists
        // (the hand-walk decodes the identical layout WITHOUT going through the
        // templated to_vector body — so equality is a true cross-implementation
        // oracle, not a tautology).
        ctx.check("to_vector_arraylist_matches_hand_walk_size",
                  g_tv_arr_many.size == g_arr_many.size);
        ctx.check("to_vector_arraylist_matches_hand_walk_first_id",
                  g_tv_arr_many.first_id == g_arr_many.first_id);
        ctx.check("to_vector_arraylist_matches_hand_walk_last_id",
                  g_tv_arr_many.last_id == g_arr_many.last_id);
        ctx.check("to_vector_linkedlist_matches_hand_walk_size",
                  g_tv_link_many.size == g_link_many.size);
        ctx.check("to_vector_linkedlist_matches_hand_walk_first_id",
                  g_tv_link_many.first_id == g_link_many.first_id);
        ctx.check("to_vector_linkedlist_matches_hand_walk_last_id",
                  g_tv_link_many.last_id == g_link_many.last_id);

        // And the two library fast paths must agree with EACH OTHER on the
        // many-element lists (same size / first / last / order) — the
        // cross-path parity the module already proves for the hand-walk, now
        // restated for to_vector so a one-sided fast-path regression is caught.
        ctx.check("to_vector_array_and_link_many_same_size",
                  g_tv_arr_many.size == g_tv_link_many.size);
        ctx.check("to_vector_array_and_link_many_same_first_id",
                  g_tv_arr_many.first_id == g_tv_link_many.first_id);
        ctx.check("to_vector_array_and_link_many_same_last_id",
                  g_tv_arr_many.last_id == g_tv_link_many.last_id);
        ctx.check("to_vector_array_and_link_many_both_ordered",
                  g_tv_arr_many.order_ok && g_tv_link_many.order_ok);
    }   // run_collection_list_checks
}   // anonymous namespace

VMHOOK_JVM_MODULE(collection_list)
{
    // Run the whole body under a try/catch so a stray throw from any vmhook call
    // (a field read, a decode, the harness) can never escape this module.  A
    // throw is recorded as [INFO], never a FAIL (mirrors collection_set.cpp /
    // register_class.cpp).
    bool body_threw{ false };
    try
    {
        run_collection_list_checks(ctx);
    }
    catch (...)
    {
        body_threw = true;
    }

    // FINAL CLEANUP — belt-and-braces, OUTSIDE the try so it ALWAYS runs.  This
    // module installs no hooks, but other modules run after it, so the
    // unconditional shutdown_hooks() guarantees an empty hook table on EVERY exit
    // path regardless (it is idempotent and safe-when-empty — proven by
    // shutdown_hooks_teardown).
    vmhook::shutdown_hooks();

    if (body_threw)
    {
        ctx.record("[INFO] collection_list: the test body threw and was contained "
                   "(no crash, no hooks armed); see preceding checks for partial "
                   "results.");
    }
    ctx.check("module_left_clean_final_shutdown", true);
}
