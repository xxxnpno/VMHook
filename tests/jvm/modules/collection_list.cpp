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
//   Vector / Stack backing store ("elementData" + "elementCount", NOT "size")
//     * empty / many (default cap 10 DOUBLES to 20, so size 12 != length 20 —
//       the bound MUST be elementCount) / ensureCapacity-style oversized (cap 100)
//       / null element -> nullptr slot / size 2 / Stack (extends Vector).
//
//   CopyOnWriteArrayList backing store ("array" Object[]; length IS the size)
//     * empty / many / null element (COW permits null) / size 2.
//
//   Boxed-Integer element lists (ArrayList<Integer> / Vector<Integer>): the
//     backing walk is element-TYPE-agnostic — each slot decodes to a real
//     java.lang.Integer read through integer_object (value == index).
//
//   Object[] ('[L...;') raw reference array (elemArray): array_length + per-slot
//     get_array_element<uint32> -> decode_oop_pointer.
//
//   nested List-of-Lists: outer ArrayList walk yields inner List OOPs; each is
//     re-walked by a shape-detecting hand-walk (ArrayList OR LinkedList).
//   nested List-of-Maps: outer ArrayList walk yields inner Map OOPs; each proven
//     a real, distinct heap object (Map content decode is collection_map's job).
//
//   JDK Collections wrappers, decoded by a direct backing-FIELD walk (NO Java
//   get(int) call — forbidden from the worker body): Arrays.asList (field "a",
//   an Object[]); Collections.emptyList (no element field, size 0);
//   Collections.singletonList (field "element"); Collections.unmodifiableList
//   (field "list" -> backing ArrayList); Collections.synchronizedList (field "c"
//   -> backing ArrayList) — both re-walked.
//
//   List.of(...) immutable (JDK 9+, built reflectively so the fixture compiles at
//   -source 8): ListN (field "elements" Object[]) for List.of()/List.of(4) IS
//   hand-walked; List12 (e0/e1 with a shared non-null EMPTY sentinel for size 1)
//   is CHARACTERIZED via its published size() witness — a raw e0/e1 read cannot
//   tell size-1 from size-2.  On Java 8 the whole List.of block is [INFO]-skipped.
//
//   CHARACTERIZED (size()-witness + [INFO], not element-decoded): subList(from,to)
//   views (ArrayList- and LinkedList-backed) whose SubList backing shape moved
//   across JDKs (parent/parentOffset -> root/parent/offset) and carries no element
//   array — the module pins their published size() (== SUB_LEN) instead of a
//   fragile raw walk.  This is exactly the CollSet.java handling for non-fast-path
//   Set wrappers: a Java size() oracle the native side reads as a plain int field.
//
//   Cross-path parity: ArrayList-many vs LinkedList-many agree on size / first /
//   last and both ordered; AND ArrayList / Vector / Stack / COW (four backing
//   shapes, same 12-element content) agree on size / first / last and all ordered
//   — the walk picks the backing shape from the runtime klass, not the Java static
//   type (note linkBig is declared as List).
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
#include <functional>
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

    // Wrapper for java.lang.Integer — the BOXED element type.  Reads the single
    // primitive field "value" (an int, stable JDK 6..26).  Proves the backing
    // walk is element-TYPE-agnostic: it hands back a decoded element OOP that is a
    // real java.lang.Integer the native side reads through, exactly as it does for
    // the fixture's own Elem type.
    class integer_object : public vmhook::object<integer_object>
    {
    public:
        explicit integer_object(vmhook::oop_t instance) noexcept
            : vmhook::object<integer_object>{ instance }
        {
        }

        auto value() const -> std::int32_t
        {
            const auto f{ get_field("value") };
            return f ? static_cast<std::int32_t>(f->get()) : -1;
        }
    };

    // Wrapper for java.lang.Long — the BOXED 64-bit element type.  Reads the
    // single primitive field "value" (a long, stable JDK 6..26) FULL-WIDTH via the
    // value_t -> std::int64_t conversion.  The boxed-Long list stores values above
    // 2^32, so a truncating 32-bit read would drop the high word; reading the whole
    // 64-bit value back proves the element decode is full-width.  No element-klass
    // registration beyond java/lang/Long is needed (find_field resolves "value").
    class long_object : public vmhook::object<long_object>
    {
    public:
        explicit long_object(vmhook::oop_t instance) noexcept
            : vmhook::object<long_object>{ instance }
        {
        }

        auto value() const -> std::int64_t
        {
            const auto f{ get_field("value") };
            return f ? static_cast<std::int64_t>(f->get()) : -1;
        }
    };

    // Wrapper for java.lang.String list elements.  read_java_string resolves
    // java/lang/String itself and is internally gated by is_valid_pointer, so this
    // needs NO klass registration; "" comes back for a null/invalid backing.  Same
    // idiom as collection_linked_list.cpp's str_elem.
    class string_object : public vmhook::object<string_object>
    {
    public:
        explicit string_object(vmhook::oop_t instance) noexcept
            : vmhook::object<string_object>{ instance }
        {
        }

        auto content() const -> std::string
        {
            return vmhook::read_java_string(this->get_instance());
        }
    };

    // Wrapper for an enum-constant list element.  Registered as java/lang/Enum so
    // find_field resolves `name` (String) and `ordinal` (int), both declared on
    // java.lang.Enum (the shared superclass of every concrete enum) at offsets
    // common to every enum constant OOP — the same idiom collection_set_exhaustive
    // uses.  Proves a decoded List element can be a real enum constant.
    class enum_object : public vmhook::object<enum_object>
    {
    public:
        explicit enum_object(vmhook::oop_t instance) noexcept
            : vmhook::object<enum_object>{ instance }
        {
        }

        auto ordinal() const -> std::int32_t
        {
            const auto f{ get_field("ordinal") };
            return f ? static_cast<std::int32_t>(f->get()) : -1;
        }

        auto name() const -> std::string
        {
            const auto f{ get_field("name") };
            if (!f)
            {
                return std::string{};
            }
            std::string s = f->get();
            return s;
        }
    };

    // Wrappers for boxed PRIMITIVE elements whose single field is named "value".
    // java.lang.Boolean/Byte/Short/Character each expose a primitive "value" the
    // value_t::get() conversion reads as an integer (bool/byte/short/char all widen
    // to int32 cleanly).  Each box klass gets its OWN wrapper type: object::get_field
    // resolves the field off the wrapper's REGISTERED klass (typeid(*this)), so a
    // single shared wrapper registered to one box klass would read the others through
    // the wrong klass's field entry — distinct types keep each registration exact.
    // Proves the backing walk is element-TYPE-agnostic across every boxed-primitive
    // element family.  CRTP base factors the identical body; only the registered
    // klass differs.
    template<typename derived_box>
    class boxed_int_base : public vmhook::object<derived_box>
    {
    public:
        explicit boxed_int_base(vmhook::oop_t instance) noexcept
            : vmhook::object<derived_box>{ instance }
        {
        }

        auto value() const -> std::int32_t
        {
            const auto f{ this->get_field("value") };
            return f ? static_cast<std::int32_t>(f->get()) : -1;
        }
    };

    class boolean_object : public boxed_int_base<boolean_object>
    {
    public:
        using boxed_int_base<boolean_object>::boxed_int_base;
    };

    class byte_object : public boxed_int_base<byte_object>
    {
    public:
        using boxed_int_base<byte_object>::boxed_int_base;
    };

    class short_object : public boxed_int_base<short_object>
    {
    public:
        using boxed_int_base<short_object>::boxed_int_base;
    };

    class char_object : public boxed_int_base<char_object>
    {
    public:
        using boxed_int_base<char_object>::boxed_int_base;
    };

    // Wrapper for java.lang.Double — the boxed 8-byte FLOATING element type.
    // Reads "value" (a double) full-width via the value_t -> double conversion.
    // The floating-point analogue of long_object; proves a D-typed (wide) boxed
    // primitive element decodes through the element-type-agnostic backing walk.
    class double_object : public vmhook::object<double_object>
    {
    public:
        explicit double_object(vmhook::oop_t instance) noexcept
            : vmhook::object<double_object>{ instance }
        {
        }

        auto value() const -> double
        {
            const auto f{ get_field("value") };
            return f ? static_cast<double>(f->get()) : -1.0;
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

    // New shapes (mirror CollList.java).
    constexpr std::int32_t VEC_MANY{ 12 };
    constexpr std::int32_t INT_LEN{ 6 };
    constexpr std::int32_t MAP_OUTER{ 3 };
    constexpr std::int32_t LISTOF_N{ 4 };
    constexpr std::int32_t SUB_LEN{ 6 };   // SUB_TO(9) - SUB_FROM(3)

    // Extra element-TYPE / SIZE coverage (mirror CollList.java).
    constexpr std::int32_t STR_LEN{ 5 };
    constexpr std::int32_t LONG_LEN{ 6 };
    constexpr std::int64_t LONG_BASE{ 0x1'0000'0007LL };   // > 2^32 (4294967303)
    constexpr std::int32_t ENUM_LEN{ 3 };                  // Color.values().length
    constexpr std::int32_t TEN{ 10 };
    constexpr std::int32_t SIXTEEN{ 16 };
    constexpr std::int32_t THOUSAND{ 1000 };

    // Null-PATTERN coverage (all-null / null-at-boundary lists).
    constexpr std::int32_t NULL_PATTERN_LEN{ 4 };

    // Extra boxed-type coverage (Boolean / Byte / Short / Character / Double).
    constexpr std::int32_t BOX_LEN{ 3 };
    constexpr std::int32_t CHAR_BASE{ 'a' };

    // Mixed-shape nested list (Vector inner + COW inner).
    constexpr std::int32_t NESTED_MIX_OUTER{ 2 };

    // Aliased-duplicate lists (same Elem object at every slot).
    constexpr std::int32_t ALIAS_LEN{ 4 };
    constexpr std::int32_t ALIAS_VAL{ 7 };

    // Heterogeneous List<Object> (String,Integer,Elem,null,Elem).
    constexpr std::int32_t HETERO_LEN{ 5 };
    constexpr std::int32_t HETERO_NULL_AT{ 3 };

    // Shuffled-insertion-order lists.  SHUF_ORDER is a fixed non-sorted
    // permutation of 0..SHUF_LEN-1 kept in lockstep with CollList.java.
    constexpr std::int32_t SHUF_LEN{ 6 };
    constexpr std::int32_t SHUF_ORDER[]{ 3, 0, 5, 1, 4, 2 };

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

    // Generic Object[] walk into a vector of `T` wrappers (one per slot, nullptr
    // for a null slot), bounded by `bound`.  `bound < 0` means "use the array's
    // real length"; a non-negative `bound` is the LOGICAL element count (e.g. a
    // Vector's elementCount) and is the loop limit instead of the backing length.
    // Per-slot get_array_element bounds-checks every index against the real array
    // length, so a (pathological) bound > length yields nullptr tail slots rather
    // than an OOB read — the same size-vs-capacity safety walk_arraylist relies on.
    template<typename T>
    auto walk_object_array_as(void* const array_oop, const std::int32_t bound)
        -> std::vector<std::unique_ptr<T>>
    {
        std::vector<std::unique_ptr<T>> result;
        if (!array_oop || !vmhook::hotspot::is_valid_pointer(array_oop))
        {
            return result;
        }
        const std::int32_t length{ vmhook::array_length(array_oop) };
        const std::int32_t n{ clamp_count(bound < 0 ? length : bound) };
        result.reserve(static_cast<std::size_t>(n));
        for (std::int32_t index{ 0 }; index < n; ++index)
        {
            const std::uint32_t compressed{
                vmhook::get_array_element<std::uint32_t>(array_oop, index) };
            void* const element_oop{ vmhook::hotspot::decode_oop_pointer(compressed) };
            if (element_oop && vmhook::hotspot::is_valid_pointer(element_oop))
            {
                result.push_back(std::make_unique<T>(
                    static_cast<vmhook::oop_t>(element_oop)));
            }
            else
            {
                result.push_back(nullptr);
            }
        }
        return result;
    }

    // Walk a java.util.Vector / java.util.Stack (Stack extends Vector) by its
    // "elementData" Object[] backing array, bounded by "elementCount" — NOT
    // elementData.length and NOT "size" (Vector has no "size" field; its count
    // field is "elementCount").  A default Vector grows by DOUBLING (cap 10 -> 20),
    // so the elementCount bound is the size-vs-capacity property on a second
    // container family: emit exactly elementCount elements, no phantom-null tail.
    auto walk_vector(void* const list_oop)
        -> std::vector<std::unique_ptr<elem_object>>
    {
        std::vector<std::unique_ptr<elem_object>> result;
        if (!list_oop || !vmhook::hotspot::is_valid_pointer(list_oop))
        {
            return result;
        }
        const auto count_entry{ field_entry_of(list_oop, "elementCount") };
        const auto data_entry{ field_entry_of(list_oop, "elementData") };
        if (!count_entry || !data_entry)
        {
            return result;
        }
        const std::int32_t n{ read_int_field(list_oop, "elementCount", 0) };
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
        return walk_object_array_as<elem_object>(array_oop, clamp_count(n));
    }

    // Walk a java.util.concurrent.CopyOnWriteArrayList by its "array" Object[]
    // backing.  COW has NO separate size field — the backing array's LENGTH is the
    // element count — so the walk bound is the array length itself (bound = -1).
    // COW permits a null element, surfaced as a nullptr slot.
    auto walk_cow(void* const list_oop)
        -> std::vector<std::unique_ptr<elem_object>>
    {
        std::vector<std::unique_ptr<elem_object>> result;
        if (!list_oop || !vmhook::hotspot::is_valid_pointer(list_oop))
        {
            return result;
        }
        void* const array_oop{ read_ref_field_oop(list_oop, "array") };
        if (!array_oop)
        {
            return result;
        }
        return walk_object_array_as<elem_object>(array_oop, -1);
    }

    // Walk an ArrayList- or Vector-shaped list ("elementData" Object[] bounded by
    // "size" OR "elementCount") into a vector of `T` wrappers.  Used for the
    // boxed-Integer lists (T = integer_object): proves the same backing-array walk
    // is element-TYPE-agnostic.  The bound is the logical count field, never
    // elementData.length, so a grown backing array contributes no phantom tail.
    template<typename T>
    auto walk_indexed_backing_as(void* const list_oop)
        -> std::vector<std::unique_ptr<T>>
    {
        std::vector<std::unique_ptr<T>> result;
        if (!list_oop || !vmhook::hotspot::is_valid_pointer(list_oop))
        {
            return result;
        }
        if (!field_entry_of(list_oop, "elementData").has_value())
        {
            return result;
        }
        // ArrayList uses "size"; Vector uses "elementCount".  Read whichever the
        // klass exposes as the logical element count.
        std::int32_t n{ 0 };
        if (field_entry_of(list_oop, "size").has_value())
        {
            n = read_int_field(list_oop, "size", 0);
        }
        else if (field_entry_of(list_oop, "elementCount").has_value())
        {
            n = read_int_field(list_oop, "elementCount", 0);
        }
        if (n <= 0)
        {
            return result;
        }
        const auto data_entry{ field_entry_of(list_oop, "elementData") };
        std::uint32_t compressed_array{};
        if (!data_entry || !read_compressed_at(list_oop, data_entry->offset, compressed_array))
        {
            return result;
        }
        void* const array_oop{ vmhook::decode_array_oop(compressed_array) };
        return walk_object_array_as<T>(array_oop, clamp_count(n));
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
    //   2. Vector/Stack ("elementData" + "elementCount")-> walk_vector
    //   3. LinkedList ("first" + "size")                 -> walk_linkedlist(size)
    //   4. CopyOnWriteArrayList ("array", no "size")     -> walk_cow (len == size)
    //   5. Arrays$ArrayList ("a" Object[])               -> walk_object_array
    //   6. ImmutableCollections$ListN ("elements")       -> walk_object_array
    //   7. SingletonList ("element")                     -> one-element vector
    //   8. UnmodifiableList ("list") / Collection ("c")  -> recurse on the backing
    //   9. EmptyList / anything else                     -> empty
    // NOTE: ImmutableCollections$List12 ("e0"/"e1") is intentionally NOT here: its
    // unused "e1" slot holds a shared non-null EMPTY sentinel for a size-1 list, so
    // a raw e0/e1 read cannot tell size-1 from size-2 without size().  The module
    // CHARACTERIZES List12 via the published size() witness instead of decoding it.
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
        // 2. Vector / Stack: "elementData" + "elementCount" (NO "size" field).
        if (field_entry_of(list_oop, "elementCount").has_value()
            && field_entry_of(list_oop, "elementData").has_value())
        {
            return walk_vector(list_oop);
        }
        // 3. LinkedList.
        if (has_size && field_entry_of(list_oop, "first").has_value())
        {
            const std::int32_t n{ read_int_field(list_oop, "size", 0) };
            return walk_linkedlist(list_oop, n);
        }
        // 4. CopyOnWriteArrayList: backing "array" Object[] whose length is the
        //    size (no separate size field).  Guard on the absence of "size" so a
        //    future List that happened to expose both does not misroute here.
        if (!has_size && field_entry_of(list_oop, "array").has_value())
        {
            return walk_cow(list_oop);
        }
        // 5. Arrays$ArrayList: backing Object[] in field "a".
        if (field_entry_of(list_oop, "a").has_value())
        {
            void* const array_oop{ read_ref_field_oop(list_oop, "a") };
            if (array_oop)
            {
                return walk_object_array(array_oop);
            }
            return result;
        }
        // 6. ImmutableCollections$ListN (List.of with 0 or 3+ elems): backing
        //    "elements" Object[] holds exactly the real elements (no sentinel).
        if (field_entry_of(list_oop, "elements").has_value())
        {
            void* const array_oop{ read_ref_field_oop(list_oop, "elements") };
            if (array_oop)
            {
                return walk_object_array(array_oop);
            }
            return result;
        }
        // 7. SingletonList: single element in field "element".
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
        // 8. UnmodifiableList ("list") / Unmodifiable/Synchronized Collection ("c"):
        //    recurse on the wrapped backing list.  Collections.synchronizedList's
        //    backing field is also "c" (its mutex is a separate field), so this one
        //    branch covers both unmodifiable and synchronized wrappers.
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
        // 9. EmptyList / no recognised element field -> empty.
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

    // ── New List-family observations (Vector/Stack/COW/synchronized/ListN) ───
    list_obs g_vec_empty;
    list_obs g_vec_many;
    list_obs g_vec_oversized;    // size != elementData.length (cap 100)
    list_obs g_vec_null;
    list_obs g_vec_two;
    list_obs g_stack_many;       // Stack extends Vector
    list_obs g_cow_empty;
    list_obs g_cow_many;
    list_obs g_cow_null;         // COW permits null
    list_obs g_cow_two;
    list_obs g_sync;             // Collections.synchronizedList -> "c" recurse
    list_obs g_listof0;          // List.of() ListN (hand-walked)
    list_obs g_listofN;          // List.of(4 elems) ListN (hand-walked)

    // Boxed-Integer lists: observed by VALUE, not id/tag (java.lang.Integer).
    list_obs g_int_arr;          // ArrayList<Integer>
    list_obs g_int_vec;          // Vector<Integer>
    bool g_int_arr_values_ok{ false };
    bool g_int_vec_values_ok{ false };

    // String / boxed-Long / enum element lists (extra element-TYPE coverage).
    list_obs     g_str_arr;            // ArrayList<String>
    bool         g_str_values_ok{ false };
    list_obs     g_long_arr;           // ArrayList<Long> (values > 2^32)
    bool         g_long_values_ok{ false };
    list_obs     g_enum_arr;           // ArrayList<Color> (enum constants)
    bool         g_enum_values_ok{ false };

    // Extra-size Elem lists (positional order at sizes 10 / 16 / 1000).
    list_obs g_arr_ten;
    list_obs g_arr_sixteen;
    list_obs g_arr_thousand;
    list_obs g_link_thousand;
    // to_vector parity on a round-1000 ArrayList (library fast path at scale).
    list_obs g_tv_arr_thousand;

    // Nested List-of-Map: outer ArrayList walk -> inner Map OOP identity only.
    std::int32_t g_nested_map_outer_n{ -1 };
    bool         g_nested_map_distinct{ true };
    std::int32_t g_nested_map_nonnull{ -1 };

    // Null-PATTERN lists (all-null / null-at-head / null-at-tail).
    list_obs g_arr_all_null;
    list_obs g_link_all_null;
    list_obs g_arr_null_first;
    list_obs g_link_null_first;
    list_obs g_arr_null_last;

    // Extra boxed element-type lists, observed by VALUE.
    list_obs g_bool_arr;
    bool     g_bool_values_ok{ false };
    list_obs g_byte_arr;
    bool     g_byte_values_ok{ false };
    list_obs g_short_arr;
    bool     g_short_values_ok{ false };
    list_obs g_char_arr;
    bool     g_char_values_ok{ false };
    list_obs g_double_arr;
    bool     g_double_values_ok{ false };

    // Mixed-shape nested list (Vector inner + COW inner).
    std::int32_t g_nested_mix_outer_n{ -1 };
    std::int32_t g_nested_mix_inner_ok{ 0 };
    bool         g_nested_mix_distinct{ true };

    // Aliased-duplicate lists: ONE Elem at every slot.  Walk must emit the SAME
    // OOP ALIAS_LEN times (no dedup, no slot dropped); since the slots ARE the
    // same heap object, distinct_ok is EXPECTED false here — that is the
    // legitimate-repeated-OOP case, not a cycle.  We assert size / non-null /
    // values, and that exactly ONE unique OOP backs all the slots.
    list_obs     g_arr_alias;
    list_obs     g_link_alias;
    std::int32_t g_arr_alias_unique_oops{ -1 };
    std::int32_t g_link_alias_unique_oops{ -1 };
    bool         g_arr_alias_all_val{ false };
    bool         g_link_alias_all_val{ false };

    // Heterogeneous List<Object>: one decoded OOP per slot regardless of class.
    list_obs     g_hetero;                  // observed count-only (varied types)
    std::int32_t g_hetero_nonnull{ -1 };
    std::int32_t g_hetero_null_at{ -2 };
    bool         g_hetero_elem_slots_ok{ false };  // indices 2 & 4 are valid Elems
    bool         g_hetero_elem4_id_ok{ false };    // index 4 Elem has id == 4

    // Shuffled-insertion-order lists: vec[k].id == SHUF_ORDER[k] (insertion, not
    // sorted).  insertion_ok proves the walk preserves insertion order.
    list_obs g_arr_shuffled;
    list_obs g_link_shuffled;
    bool     g_arr_shuf_insertion_ok{ false };
    bool     g_link_shuf_insertion_ok{ false };

    // Size-1 null-only lists (the smallest "has a null" case).
    list_obs g_arr_single_null;
    list_obs g_link_single_null;

    // get_array_element bounds probe: reading an index AT and PAST the real
    // backing-array length must yield compressed-0 (-> nullptr), never an OOB
    // read.  Recorded as booleans the assertion block checks HARD.
    bool g_bounds_in_range_nonzero{ false };  // index 0 of a non-empty array != 0
    bool g_bounds_at_length_zero{ false };    // index == length -> 0
    bool g_bounds_past_length_zero{ false };  // index == length+7 -> 0
    bool g_bounds_negative_zero{ false };     // negative index -> 0

    // Adversarial / degenerate walk inputs that must NOT crash and must return an
    // EMPTY vector (wrong-shape / non-list OOP / nullptr).  Each records the
    // decoded size; the assertion block proves 0 + no crash (we reached the check).
    std::int32_t g_adv_null_arraylist{ -1 };   // walk_arraylist(nullptr)
    std::int32_t g_adv_null_linkedlist{ -1 };  // walk_linkedlist(nullptr, n)
    std::int32_t g_adv_elem_as_list{ -1 };     // walk_arraylist(an Elem OOP)
    std::int32_t g_adv_string_as_list{ -1 };   // walk_list_by_shape(a String OOP)
    std::int32_t g_adv_array_as_list{ -1 };    // walk_arraylist(an Object[] OOP)
    std::int32_t g_adv_linked_bogus_n{ -1 };   // walk_linkedlist(empty, huge n)
    std::int32_t g_adv_shape_null{ -1 };       // walk_list_by_shape(nullptr)
    bool         g_adv_reached_end{ false };    // all adversarial calls returned

    // Size-as-oracle: emitted element count == the list's own published size().
    bool g_oracle_arr_many{ false };
    bool g_oracle_link_many{ false };
    bool g_oracle_arr_thousand{ false };
    bool g_oracle_link_thousand{ false };
    bool g_oracle_arr_alias{ false };
    bool g_oracle_hetero{ false };
    bool g_oracle_arr_shuffled{ false };

    // ── PUBLIC value_t::to_vector entry-point observations ───────────────────
    // The library's PUBLIC field-to-vector entry (field_proxy::value_t::to_vector,
    // reached via get_field("...")->get().to_vector<T>()) — distinct from the
    // lower-level collection::to_vector the g_tv_* group drives.  It branches on
    // the field SIGNATURE: a 'L...;' reference List field falls through to
    // collection::to_vector, while a '[L...;' object-array field is walked
    // directly as a raw Java array.  Both branches are exercised here.
    list_obs g_pv_arr_single;     // 'L' branch: ArrayList single
    list_obs g_pv_arr_many;       // 'L' branch: ArrayList many
    list_obs g_pv_arr_null;       // 'L' branch: ArrayList null-slot
    list_obs g_pv_link_single;    // 'L' branch: LinkedList single
    list_obs g_pv_link_many;      // 'L' branch: LinkedList many
    list_obs g_pv_link_null;      // 'L' branch: LinkedList null Node.item
    list_obs g_pv_link_big;       // 'L' branch: LinkedList declared as List (runtime-klass dispatch)
    list_obs g_pv_elem_array;     // '[L' branch: Elem[] object-array field

    // Characterized-via-size() families (NOT element-decoded by the hand-walk):
    // the module reads each list's published Java size() witness (a plain int
    // field — no Java call) and asserts it matches the expected constant.
    bool         g_listof_available{ false };
    std::int32_t g_listof1_size_witness{ -1 };
    std::int32_t g_listof2_size_witness{ -1 };
    std::int32_t g_arr_sublist_size_witness{ -1 };
    std::int32_t g_link_sublist_size_witness{ -1 };

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

    // Reduce a decoded vector of java.lang.Integer wrappers: size / null pattern /
    // OOP distinctness (into `o`), and set `values_ok` iff every slot is non-null
    // with value() == its index (the boxed analogue of the Elem id==index order
    // proof).  Boxed Integers in [-128,127] are interned (cached), so two equal
    // small Integers ARE the same OOP — to keep the distinctness check meaningful
    // the fixture uses values 0..INT_LEN-1 which are DISTINCT, hence distinct OOPs.
    auto observe_integers(list_obs& o,
                          const std::vector<std::unique_ptr<integer_object>>& v,
                          bool& values_ok) -> void
    {
        o.seen = true;
        o.size = static_cast<std::int32_t>(v.size());
        std::int32_t non_null{ 0 };
        std::int32_t null_count{ 0 };
        bool distinct_ok{ true };
        bool vals_ok{ !v.empty() };
        std::unordered_set<const void*> seen_oops;
        seen_oops.reserve(v.size() * 2 + 1);
        for (std::size_t k{ 0 }; k < v.size(); ++k)
        {
            const integer_object* const e{ v[k].get() };
            if (e == nullptr) { ++null_count; vals_ok = false; continue; }
            ++non_null;
            if (e->value() != static_cast<std::int32_t>(k)) { vals_ok = false; }
            const void* const oop{ static_cast<const void*>(e->get_instance()) };
            if (!seen_oops.insert(oop).second) { distinct_ok = false; }
        }
        o.non_null = non_null;
        o.null_count = null_count;
        o.distinct_ok = distinct_ok;
        values_ok = vals_ok;
    }

    // Reduce a decoded vector of java.lang.String wrappers: size / null pattern /
    // OOP distinctness (into `o`), and set `values_ok` iff every slot is non-null
    // with content() == "s<index>" (the String analogue of the id==index proof).
    auto observe_strings(list_obs& o,
                         const std::vector<std::unique_ptr<string_object>>& v,
                         bool& values_ok) -> void
    {
        o.seen = true;
        o.size = static_cast<std::int32_t>(v.size());
        std::int32_t non_null{ 0 };
        std::int32_t null_count{ 0 };
        bool distinct_ok{ true };
        bool vals_ok{ !v.empty() };
        std::unordered_set<const void*> seen_oops;
        seen_oops.reserve(v.size() * 2 + 1);
        for (std::size_t k{ 0 }; k < v.size(); ++k)
        {
            const string_object* const e{ v[k].get() };
            if (e == nullptr) { ++null_count; vals_ok = false; continue; }
            ++non_null;
            if (e->content() != ("s" + std::to_string(k))) { vals_ok = false; }
            const void* const oop{ static_cast<const void*>(e->get_instance()) };
            if (!seen_oops.insert(oop).second) { distinct_ok = false; }
        }
        o.non_null = non_null;
        o.null_count = null_count;
        o.distinct_ok = distinct_ok;
        values_ok = vals_ok;
    }

    // Reduce a decoded vector of java.lang.Long wrappers: size / null pattern /
    // OOP distinctness (into `o`), and set `values_ok` iff every slot is non-null
    // with value() == LONG_BASE + index.  Because LONG_BASE > 2^32, a truncating
    // 32-bit read would yield index instead of LONG_BASE + index, flipping vals_ok.
    auto observe_longs(list_obs& o,
                       const std::vector<std::unique_ptr<long_object>>& v,
                       bool& values_ok) -> void
    {
        o.seen = true;
        o.size = static_cast<std::int32_t>(v.size());
        std::int32_t non_null{ 0 };
        std::int32_t null_count{ 0 };
        bool distinct_ok{ true };
        bool vals_ok{ !v.empty() };
        std::unordered_set<const void*> seen_oops;
        seen_oops.reserve(v.size() * 2 + 1);
        for (std::size_t k{ 0 }; k < v.size(); ++k)
        {
            const long_object* const e{ v[k].get() };
            if (e == nullptr) { ++null_count; vals_ok = false; continue; }
            ++non_null;
            if (e->value() != LONG_BASE + static_cast<std::int64_t>(k)) { vals_ok = false; }
            const void* const oop{ static_cast<const void*>(e->get_instance()) };
            if (!seen_oops.insert(oop).second) { distinct_ok = false; }
        }
        o.non_null = non_null;
        o.null_count = null_count;
        o.distinct_ok = distinct_ok;
        values_ok = vals_ok;
    }

    // Reduce a decoded vector of enum-constant wrappers: size / null pattern / OOP
    // distinctness (into `o`), and set `values_ok` iff every slot is non-null with
    // ordinal() == index (positional/ordinal order) AND name() non-empty (the
    // inherited Enum String field reads back).
    auto observe_enums(list_obs& o,
                       const std::vector<std::unique_ptr<enum_object>>& v,
                       bool& values_ok) -> void
    {
        o.seen = true;
        o.size = static_cast<std::int32_t>(v.size());
        std::int32_t non_null{ 0 };
        std::int32_t null_count{ 0 };
        bool distinct_ok{ true };
        bool vals_ok{ !v.empty() };
        std::unordered_set<const void*> seen_oops;
        seen_oops.reserve(v.size() * 2 + 1);
        for (std::size_t k{ 0 }; k < v.size(); ++k)
        {
            const enum_object* const e{ v[k].get() };
            if (e == nullptr) { ++null_count; vals_ok = false; continue; }
            ++non_null;
            if (e->ordinal() != static_cast<std::int32_t>(k)) { vals_ok = false; }
            if (e->name().empty()) { vals_ok = false; }
            const void* const oop{ static_cast<const void*>(e->get_instance()) };
            if (!seen_oops.insert(oop).second) { distinct_ok = false; }
        }
        o.non_null = non_null;
        o.null_count = null_count;
        o.distinct_ok = distinct_ok;
        values_ok = vals_ok;
    }

    // Reduce a decoded vector of boxed-int wrappers (Boolean/Byte/Short/Character),
    // proving value() == expected(index).  `expected` maps an index to the value
    // the fixture stored at that slot.  Distinctness is NOT asserted here: the JDK
    // INTERNS small Boolean/Byte/Short/Character boxes, so two equal values share a
    // cached OOP (e.g. Boolean.FALSE appears twice in boolArrList) — a real, valid
    // duplicate-OOP that the by-value check tolerates.  Shape/size/non-null are the
    // hard structural signal; the per-element value is gated best-effort by caller.
    template<typename box_t>
    auto observe_boxed_ints(list_obs& o,
                            const std::vector<std::unique_ptr<box_t>>& v,
                            const std::function<std::int32_t(std::int32_t)>& expected,
                            bool& values_ok) -> void
    {
        o.seen = true;
        o.size = static_cast<std::int32_t>(v.size());
        std::int32_t non_null{ 0 };
        std::int32_t null_count{ 0 };
        bool vals_ok{ !v.empty() };
        for (std::size_t k{ 0 }; k < v.size(); ++k)
        {
            const box_t* const e{ v[k].get() };
            if (e == nullptr) { ++null_count; vals_ok = false; continue; }
            ++non_null;
            if (e->value() != expected(static_cast<std::int32_t>(k))) { vals_ok = false; }
        }
        o.non_null = non_null;
        o.null_count = null_count;
        o.distinct_ok = true;   // not meaningful for interned boxes; see comment.
        values_ok = vals_ok;
    }

    // Reduce a decoded vector of java.lang.Double wrappers, proving value() == index
    // as a full-width 8-byte read.  Doubles are NOT interned, so distinctness holds;
    // an exact == against the small integral values 0.0/1.0/2.0 is representable
    // exactly in double, so the comparison is deterministic (no epsilon needed).
    auto observe_doubles(list_obs& o,
                         const std::vector<std::unique_ptr<double_object>>& v,
                         bool& values_ok) -> void
    {
        o.seen = true;
        o.size = static_cast<std::int32_t>(v.size());
        std::int32_t non_null{ 0 };
        std::int32_t null_count{ 0 };
        bool distinct_ok{ true };
        bool vals_ok{ !v.empty() };
        std::unordered_set<const void*> seen_oops;
        seen_oops.reserve(v.size() * 2 + 1);
        for (std::size_t k{ 0 }; k < v.size(); ++k)
        {
            const double_object* const e{ v[k].get() };
            if (e == nullptr) { ++null_count; vals_ok = false; continue; }
            ++non_null;
            if (e->value() != static_cast<double>(k)) { vals_ok = false; }
            const void* const oop{ static_cast<const void*>(e->get_instance()) };
            if (!seen_oops.insert(oop).second) { distinct_ok = false; }
        }
        o.non_null = non_null;
        o.null_count = null_count;
        o.distinct_ok = distinct_ok;
        values_ok = vals_ok;
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

    // All-null list: exactly NULL_PATTERN_LEN slots, EVERY one null.  The walk must
    // emit `size` slots (the bound is `size`, not the backing length), all decoded
    // to nullptr — the degenerate all-null case the single-null lists don't cover.
    auto check_all_null(vmhook_test::context& ctx, const std::string& p, const list_obs& o) -> void
    {
        ctx.check(p + "_seen", o.seen);
        ctx.check(p + "_size_matches", o.size == NULL_PATTERN_LEN);
        ctx.check(p + "_all_slots_null", o.null_count == NULL_PATTERN_LEN);
        ctx.check(p + "_no_elements_decoded", o.non_null == 0);
        ctx.check(p + "_first_null_at_index_0", o.null_at == 0);
    }

    // Null at a specified boundary index, the rest dense (id == index).  Proves the
    // null slot is preserved positionally at the head (index 0) or tail
    // (NULL_PATTERN_LEN-1), with order intact around it and exactly one null.
    auto check_null_boundary(vmhook_test::context& ctx, const std::string& p,
                             const list_obs& o, const std::int32_t null_index) -> void
    {
        ctx.check(p + "_seen", o.seen);
        ctx.check(p + "_size_matches", o.size == NULL_PATTERN_LEN);
        ctx.check(p + "_one_null_slot", o.null_count == 1);
        ctx.check(p + "_null_at_expected_index", o.null_at == null_index);
        ctx.check(p + "_non_null_count", o.non_null == NULL_PATTERN_LEN - 1);
        ctx.check(p + "_order_preserved_around_null", o.order_ok);
        ctx.check(p + "_non_null_distinct", o.distinct_ok);
    }

    // Best-effort gate for a CONTENT/VALUE read that decodes a reference element
    // (compressed-oops-dependent): records a PASS when the expected value reads,
    // else an [INFO] (never a FAIL).  Structural shape (size / non-null / distinct)
    // is checked HARD elsewhere; only the per-element value round-trip is gated this
    // way, mirroring the cross-toolchain hardening the suite uses for config-variant
    // reads.  CI runs default (compressed-oops-on) heaps, so on green CI this PASSes.
    auto check_or_info(vmhook_test::context& ctx, const std::string& name,
                       const bool ok, const std::string& info_detail) -> void
    {
        if (ok)
        {
            ctx.check(name, true);
        }
        else
        {
            ctx.record("[INFO] " + name + ": " + info_detail);
        }
    }

    // Structural bundle for a typed (String/Long/enum) dense list of `expected`:
    // size / non-null / no-null / distinct are HARD (these hold whenever the
    // backing array decodes at all, exactly like the Elem lists).  The per-element
    // VALUE round-trip is gated separately via check_or_info by the caller.
    auto check_typed_dense_shape(vmhook_test::context& ctx, const std::string& p,
                                 const list_obs& o, const std::int32_t expected) -> void
    {
        ctx.check(p + "_seen", o.seen);
        ctx.check(p + "_size_matches", o.size == expected);
        ctx.check(p + "_all_non_null", o.non_null == expected);
        ctx.check(p + "_no_null_slots", o.null_count == 0);
        ctx.check(p + "_elements_distinct", o.distinct_ok);
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
        // java.lang.Integer / java.lang.Long are always loaded; needed for the
        // boxed-element lists.  java.lang.Enum is the shared enum superclass that
        // declares name/ordinal, so registering the enum wrapper there resolves
        // those fields off any concrete enum-constant OOP.  (string_object needs
        // no registration — read_java_string resolves java/lang/String itself.)
        vmhook::register_class<integer_object>("java/lang/Integer");
        vmhook::register_class<long_object>("java/lang/Long");
        vmhook::register_class<enum_object>("java/lang/Enum");
        // Extra boxed-primitive element types (Boolean/Byte/Short/Character/Double):
        // each maps to its own always-loaded box klass so get_field("value")
        // resolves the right field entry per type.
        vmhook::register_class<boolean_object>("java/lang/Boolean");
        vmhook::register_class<byte_object>("java/lang/Byte");
        vmhook::register_class<short_object>("java/lang/Short");
        vmhook::register_class<char_object>("java/lang/Character");
        vmhook::register_class<double_object>("java/lang/Double");

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
        observe(g_sync,        walk_list_by_shape(list_oop_of("synchronizedView"), 0), true);

        // ── Vector / Stack ("elementData" + "elementCount") ─────────────────
        observe(g_vec_empty,     walk_vector(list_oop_of("vecEmpty")),     true);
        observe(g_vec_many,      walk_vector(list_oop_of("vecMany")),      true);
        observe(g_vec_oversized, walk_vector(list_oop_of("vecOversized")), true);
        observe(g_vec_null,      walk_vector(list_oop_of("vecWithNull")),  true);
        observe(g_vec_two,       walk_vector(list_oop_of("vecTwo")),       true);
        observe(g_stack_many,    walk_vector(list_oop_of("stackMany")),    true);

        // ── CopyOnWriteArrayList ("array" Object[], length == size) ─────────
        observe(g_cow_empty, walk_cow(list_oop_of("cowEmpty")), true);
        observe(g_cow_many,  walk_cow(list_oop_of("cowMany")),  true);
        observe(g_cow_null,  walk_cow(list_oop_of("cowWithNull")), true);
        observe(g_cow_two,   walk_cow(list_oop_of("cowTwo")),   true);

        // ── Boxed-Integer element lists (element decode is type-agnostic) ───
        observe_integers(g_int_arr,
                         walk_indexed_backing_as<integer_object>(list_oop_of("intArrList")),
                         g_int_arr_values_ok);
        observe_integers(g_int_vec,
                         walk_indexed_backing_as<integer_object>(list_oop_of("intVecList")),
                         g_int_vec_values_ok);

        // ── String / boxed-Long / enum element lists (type-agnostic decode) ──
        observe_strings(g_str_arr,
                        walk_indexed_backing_as<string_object>(list_oop_of("strList")),
                        g_str_values_ok);
        observe_longs(g_long_arr,
                      walk_indexed_backing_as<long_object>(list_oop_of("longArrList")),
                      g_long_values_ok);
        observe_enums(g_enum_arr,
                      walk_indexed_backing_as<enum_object>(list_oop_of("enumList")),
                      g_enum_values_ok);

        // ── Extra boxed-primitive element lists (Boolean/Byte/Short/Char/Double) ─
        observe_boxed_ints<boolean_object>(
            g_bool_arr,
            walk_indexed_backing_as<boolean_object>(list_oop_of("boolArrList")),
            [](std::int32_t k) { return (k == 1) ? 1 : 0; },   // false,true,false
            g_bool_values_ok);
        observe_boxed_ints<byte_object>(
            g_byte_arr,
            walk_indexed_backing_as<byte_object>(list_oop_of("byteArrList")),
            [](std::int32_t k) { return k; },
            g_byte_values_ok);
        observe_boxed_ints<short_object>(
            g_short_arr,
            walk_indexed_backing_as<short_object>(list_oop_of("shortArrList")),
            [](std::int32_t k) { return k; },
            g_short_values_ok);
        observe_boxed_ints<char_object>(
            g_char_arr,
            walk_indexed_backing_as<char_object>(list_oop_of("charArrList")),
            [](std::int32_t k) { return CHAR_BASE + k; },
            g_char_values_ok);
        observe_doubles(g_double_arr,
                        walk_indexed_backing_as<double_object>(list_oop_of("doubleArrList")),
                        g_double_values_ok);

        // ── Null-PATTERN lists (all-null / null-at-head / null-at-tail) ──────
        observe(g_arr_all_null,   walk_arraylist(list_oop_of("arrAllNull")),   true);
        observe(g_arr_null_first, walk_arraylist(list_oop_of("arrNullFirst")), true);
        observe(g_arr_null_last,  walk_arraylist(list_oop_of("arrNullLast")),  true);
        {
            void* const o{ list_oop_of("linkAllNull") };
            observe(g_link_all_null, walk_linkedlist(o, read_int_field(o, "size", 0)), true);
        }
        {
            void* const o{ list_oop_of("linkNullFirst") };
            observe(g_link_null_first, walk_linkedlist(o, read_int_field(o, "size", 0)), true);
        }

        // ── Extra-size Elem lists (positional order at 10 / 16 / 1000) ───────
        observe(g_arr_ten,      walk_arraylist(list_oop_of("arrTen")),      true);
        observe(g_arr_sixteen,  walk_arraylist(list_oop_of("arrSixteen")),  true);
        observe(g_arr_thousand, walk_arraylist(list_oop_of("arrThousand")), true);
        {
            void* const o{ list_oop_of("linkThousand") };
            observe(g_link_thousand,
                    walk_linkedlist(o, read_int_field(o, "size", 0)), true);
        }

        // ── Nested List-of-Map: outer ArrayList -> inner Map OOP identity ───
        {
            std::vector<std::unique_ptr<elem_object>> outer_maps{
                walk_arraylist(list_oop_of("nestedMaps")) };
            g_nested_map_outer_n = static_cast<std::int32_t>(outer_maps.size());
            std::int32_t nonnull{ 0 };
            bool distinct{ true };
            std::unordered_set<const void*> seen;
            for (const auto& up : outer_maps)
            {
                const elem_object* const m{ up.get() };
                if (m == nullptr) { continue; }
                void* const map_oop{ m->get_instance() };
                if (!map_oop || !vmhook::hotspot::is_valid_pointer(map_oop)) { continue; }
                ++nonnull;
                if (!seen.insert(static_cast<const void*>(map_oop)).second) { distinct = false; }
            }
            g_nested_map_nonnull = nonnull;
            g_nested_map_distinct = distinct;
        }

        // ── Mixed-shape nested list: outer ArrayList -> Vector inner + COW inner.
        //    Each inner is re-walked by the shape-detecting walk, which must
        //    dispatch to walk_vector ("elementCount") and walk_cow ("array")
        //    respectively — the nested case only covered ArrayList/LinkedList
        //    inners before.
        {
            std::vector<std::unique_ptr<elem_object>> outer{
                walk_arraylist(list_oop_of("nestedMixed")) };
            g_nested_mix_outer_n = static_cast<std::int32_t>(outer.size());
            std::int32_t inner_ok{ 0 };
            std::unordered_set<const void*> seen_inner;
            for (const auto& up : outer)
            {
                const elem_object* const inner{ up.get() };
                if (inner == nullptr) { continue; }
                void* const inner_oop{ inner->get_instance() };
                if (!inner_oop || !vmhook::hotspot::is_valid_pointer(inner_oop)) { continue; }
                if (!seen_inner.insert(static_cast<const void*>(inner_oop)).second)
                {
                    g_nested_mix_distinct = false;
                }
                std::vector<std::unique_ptr<elem_object>> items{
                    walk_list_by_shape(inner_oop, 0) };
                if (inner_list_fully_ok(items, NESTED_INNER))
                {
                    ++inner_ok;
                }
            }
            g_nested_mix_inner_ok = inner_ok;
        }

        // ── Aliased-duplicate lists: the SAME Elem at every slot ─────────────
        // The decoded element OOP is LEGITIMATELY identical across all slots, so
        // the walk must still emit ALIAS_LEN slots (no dedup) each carrying the
        // SAME OOP and value ALIAS_VAL.  We count the UNIQUE OOPs (must be 1) and
        // the value of every slot (must be ALIAS_VAL).  This separates an honest
        // repeated reference from the cycle/dup-node corruption the distinctness
        // check guards: a real list of one aliased object is NOT corruption.
        {
            const auto observe_alias{ [](list_obs& o,
                                         const std::vector<std::unique_ptr<elem_object>>& v,
                                         std::int32_t& unique_oops, bool& all_val) -> void
            {
                o.seen = true;
                o.size = static_cast<std::int32_t>(v.size());
                std::int32_t non_null{ 0 };
                std::int32_t null_count{ 0 };
                bool vals_ok{ !v.empty() };
                std::unordered_set<const void*> seen_oops;
                for (const auto& up : v)
                {
                    const elem_object* const e{ up.get() };
                    if (e == nullptr) { ++null_count; vals_ok = false; continue; }
                    ++non_null;
                    if (e->id() != ALIAS_VAL) { vals_ok = false; }
                    seen_oops.insert(static_cast<const void*>(e->get_instance()));
                }
                o.non_null = non_null;
                o.null_count = null_count;
                unique_oops = static_cast<std::int32_t>(seen_oops.size());
                all_val = vals_ok;
            } };
            observe_alias(g_arr_alias, walk_arraylist(list_oop_of("arrAlias")),
                          g_arr_alias_unique_oops, g_arr_alias_all_val);
            {
                void* const o{ list_oop_of("linkAlias") };
                observe_alias(g_link_alias,
                              walk_linkedlist(o, read_int_field(o, "size", 0)),
                              g_link_alias_unique_oops, g_link_alias_all_val);
            }
        }

        // ── Heterogeneous List<Object>: String,Integer,Elem,null,Elem ───────
        // One decoded OOP per slot regardless of the slot's runtime class; the
        // null slot becomes nullptr.  We only interpret the two REAL Elem slots
        // (indices 2 and 4) through the Elem wrapper — index 4 carries id == 4.
        {
            std::vector<std::unique_ptr<elem_object>> hetero{
                walk_arraylist(list_oop_of("heteroList")) };
            observe_count_only(g_hetero, hetero);
            std::int32_t nonnull{ 0 };
            std::int32_t first_null{ -1 };
            for (std::size_t k{ 0 }; k < hetero.size(); ++k)
            {
                if (hetero[k].get() == nullptr)
                {
                    if (first_null < 0) { first_null = static_cast<std::int32_t>(k); }
                    continue;
                }
                ++nonnull;
            }
            g_hetero_nonnull = nonnull;
            g_hetero_null_at = first_null;
            // Slots 2 and 4 are the genuine Elem objects: both must be present,
            // distinct, valid; slot 4's id reads back as 4 through the Elem wrapper.
            const bool have_slots{ hetero.size() == static_cast<std::size_t>(HETERO_LEN) };
            const elem_object* const e2{ have_slots ? hetero[2].get() : nullptr };
            const elem_object* const e4{ have_slots ? hetero[4].get() : nullptr };
            g_hetero_elem_slots_ok =
                e2 != nullptr && e4 != nullptr
                && e2->get_instance() != e4->get_instance();
            g_hetero_elem4_id_ok = (e4 != nullptr) && (e4->id() == 4);
        }

        // ── Shuffled-insertion-order lists: vec[k].id == SHUF_ORDER[k] ───────
        // Proves the walk preserves INSERTION order, not sorted order.  The ids
        // were added in the SHUF_ORDER permutation (non-ascending), so a sorted
        // or reordered walk would read a wrong id at some position.
        {
            const auto observe_shuffled{ [](list_obs& o,
                                            const std::vector<std::unique_ptr<elem_object>>& v,
                                            bool& insertion_ok) -> void
            {
                o.seen = true;
                o.size = static_cast<std::int32_t>(v.size());
                std::int32_t non_null{ 0 };
                bool ins_ok{ static_cast<std::int32_t>(v.size()) == SHUF_LEN };
                for (std::size_t k{ 0 }; k < v.size(); ++k)
                {
                    const elem_object* const e{ v[k].get() };
                    if (e == nullptr) { ins_ok = false; continue; }
                    ++non_null;
                    if (k >= static_cast<std::size_t>(SHUF_LEN)
                        || e->id() != SHUF_ORDER[k])
                    {
                        ins_ok = false;
                    }
                }
                o.non_null = non_null;
                o.null_count = o.size - non_null;
                insertion_ok = ins_ok;
            } };
            observe_shuffled(g_arr_shuffled, walk_arraylist(list_oop_of("arrShuffled")),
                             g_arr_shuf_insertion_ok);
            {
                void* const o{ list_oop_of("linkShuffled") };
                observe_shuffled(g_link_shuffled,
                                 walk_linkedlist(o, read_int_field(o, "size", 0)),
                                 g_link_shuf_insertion_ok);
            }
        }

        // ── Size-1 null-only lists (smallest "has a null") ──────────────────
        observe(g_arr_single_null, walk_arraylist(list_oop_of("arrSingleNull")), true);
        {
            void* const o{ list_oop_of("linkSingleNull") };
            observe(g_link_single_null,
                    walk_linkedlist(o, read_int_field(o, "size", 0)), true);
        }

        // ── get_array_element bounds probe (direct) ─────────────────────────
        // get_array_element bounds-checks `index` against the real array_length
        // and returns T{} (compressed 0 -> nullptr) for any index >= length or < 0.
        // Probe the elemArray backing array directly at index 0 (in range), at the
        // length (one past the last valid), well past it, and at a negative index.
        {
            void* const arr{ list_oop_of("elemArray") };   // a real Object[] OOP
            if (arr && vmhook::hotspot::is_valid_pointer(arr))
            {
                const std::int32_t len{ vmhook::array_length(arr) };
                g_bounds_in_range_nonzero =
                    (len > 0)
                    && (vmhook::get_array_element<std::uint32_t>(arr, 0) != 0u);
                g_bounds_at_length_zero =
                    (vmhook::get_array_element<std::uint32_t>(arr, len) == 0u);
                g_bounds_past_length_zero =
                    (vmhook::get_array_element<std::uint32_t>(arr, len + 7) == 0u);
                g_bounds_negative_zero =
                    (vmhook::get_array_element<std::uint32_t>(arr, -1) == 0u);
            }
        }

        // ── Adversarial / degenerate walk inputs: must NOT crash, must be empty.
        // Feed the walks nullptr, a non-list OOP (an Elem), a String, a raw
        // Object[], and an absurd LinkedList size.  Each walk gates on field
        // presence / is_valid_pointer, so a wrong-shape OOP yields an empty vector
        // (no recognised backing field) and a nullptr yields empty immediately —
        // never a crash, cycle, or OOB.  Reaching g_adv_reached_end proves no call
        // faulted.  (This is the crash-safety HARD angle on the walk surface.)
        {
            g_adv_null_arraylist =
                static_cast<std::int32_t>(walk_arraylist(nullptr).size());
            g_adv_null_linkedlist =
                static_cast<std::int32_t>(walk_linkedlist(nullptr, MANY).size());
            g_adv_shape_null =
                static_cast<std::int32_t>(walk_list_by_shape(nullptr, 0).size());

            // A real Elem OOP (no "size"/"elementData"/"first" -> empty).  Reuse
            // the singleton's ELEM_CLASS_PIN-independent route: read one element
            // out of arrSingle as a concrete non-list OOP.
            std::vector<std::unique_ptr<elem_object>> one{
                walk_arraylist(list_oop_of("arrSingle")) };
            void* const elem_oop{ (!one.empty() && one[0]) ? one[0]->get_instance()
                                                            : nullptr };
            g_adv_elem_as_list =
                static_cast<std::int32_t>(walk_arraylist(elem_oop).size());

            // A String OOP (from strList slot 0): wrong shape for every backing.
            void* const str_list{ list_oop_of("strList") };
            void* const str_oop{ read_ref_field_oop(str_list, "elementData") };
            // str_oop is the backing Object[]; route it through walk_list_by_shape,
            // which (lacking size/first/array/a/elements/element/list/c on a raw
            // array klass) must return empty without crashing.
            g_adv_string_as_list =
                static_cast<std::int32_t>(walk_list_by_shape(str_oop, 0).size());

            // A raw Object[] OOP fed to walk_arraylist (an array klass has no
            // "size"/"elementData" instance fields) -> empty, no crash.
            void* const obj_arr{ list_oop_of("elemArray") };
            g_adv_array_as_list =
                static_cast<std::int32_t>(walk_arraylist(obj_arr).size());

            // An EMPTY LinkedList walked with an absurd claimed size: first==null
            // so the loop exits at the first iteration regardless of the bound —
            // no run-away, no crash.
            void* const empty_link{ list_oop_of("linkEmpty") };
            g_adv_linked_bogus_n = static_cast<std::int32_t>(
                walk_linkedlist(empty_link, MAX_SAFE_COUNT).size());

            g_adv_reached_end = true;
        }

        // ── Size-as-oracle: emitted count == the list's published size() ────
        // The fixture publishes each list's own size() as a plain int field; the
        // walk's emitted element count must equal it exactly.  This is the
        // size-as-oracle invariant stated directly against the Java-reported size,
        // independent of the mirrored constants.
        g_oracle_arr_many =
            (g_arr_many.size == read_int_field(singleton, "arrManySize", -1));
        g_oracle_link_many =
            (g_link_many.size == read_int_field(singleton, "linkManySize", -1));
        g_oracle_arr_thousand =
            (g_arr_thousand.size == read_int_field(singleton, "arrThousandSize", -1));
        g_oracle_link_thousand =
            (g_link_thousand.size == read_int_field(singleton, "linkThousandSize", -1));
        g_oracle_arr_alias =
            (g_arr_alias.size == read_int_field(singleton, "arrAliasSize", -1));
        g_oracle_hetero =
            (g_hetero.size == read_int_field(singleton, "heteroListSize", -1));
        g_oracle_arr_shuffled =
            (g_arr_shuffled.size == read_int_field(singleton, "arrShuffledSize", -1));

        // ── List.of(...) (JDK 9+).  ListN ("elements") is hand-walked; List12
        //    ("e0"/"e1" with an EMPTY sentinel) is characterized via size().  The
        //    fixture publishes listOfAvailable + per-list size() witnesses.
        g_listof_available =
            (read_int_field(singleton, "listOfAvailable", 0) != 0);
        if (g_listof_available)
        {
            observe(g_listof0, walk_list_by_shape(list_oop_of("listOf0"), 0), true);
            observe(g_listofN, walk_list_by_shape(list_oop_of("listOfN"), 0), true);
            g_listof1_size_witness = read_int_field(singleton, "listOf1Size", -1);
            g_listof2_size_witness = read_int_field(singleton, "listOf2Size", -1);
        }

        // ── subList views: CHARACTERIZED via the published Java size() witness
        //    (the backing field shape moved across JDKs; no stable raw walk).
        g_arr_sublist_size_witness  = read_int_field(singleton, "arrSubListSize", -1);
        g_link_sublist_size_witness = read_int_field(singleton, "linkSubListSize", -1);

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
        // Library ArrayList fast path at a round-1000 scale (cross-oracle vs the
        // hand-walked arrThousand below).
        observe(g_tv_arr_thousand, to_vector_of(list_oop_of("arrThousand")), true);

        // ── PUBLIC value_t::to_vector ENTRY POINT (get_field(...)->get().to_vector)
        // The g_tv_* group above calls the LOWER-LEVEL vmhook::collection::to_vector
        // directly on a decoded OOP.  Here we drive the PUBLIC field-to-vector entry
        // — field_proxy::value_t::to_vector — which is what a real caller uses and
        // which adds the field-SIGNATURE branch the lower-level path never sees:
        //   * 'L...;' reference List field  -> falls through to collection::to_vector
        //   * '[L...;' object-array field    -> walked directly as a raw Java array
        // Both branches issue NO Java call for these shapes (ArrayList/LinkedList
        // fast paths + the array branch), so they are safe from the no-detour body.
        // The wrapper is built around the live singleton OOP so get_field resolves
        // each list field off it, exactly as a detour parameter would.
        {
            coll_list_fixture sw{ static_cast<vmhook::oop_t>(singleton) };
            const auto pub_to_vector{ [&sw](const char* const field)
                -> std::vector<std::unique_ptr<elem_object>>
            {
                const auto f{ sw.get_field(field) };
                if (!f) { return {}; }
                return f->get().to_vector<elem_object>();
            } };

            // 'L...;' reference branch (ArrayList + LinkedList fast paths).
            observe(g_pv_arr_single, pub_to_vector("arrSingle"),   true);
            observe(g_pv_arr_many,   pub_to_vector("arrMany"),     true);
            observe(g_pv_arr_null,   pub_to_vector("arrWithNull"), true);
            observe(g_pv_link_single, pub_to_vector("linkSingle"),  true);
            observe(g_pv_link_many,   pub_to_vector("linkMany"),    true);
            observe(g_pv_link_null,   pub_to_vector("linkWithNull"),true);
            // linkBig is DECLARED as List but is a LinkedList at runtime — proves
            // the public entry picks the fast path from the runtime klass, not the
            // Java static field type.
            observe(g_pv_link_big,   pub_to_vector("linkBig"),     true);

            // '[L...;' object-array branch (the documented Object[] entry point).
            observe(g_pv_elem_array, pub_to_vector("elemArray"),   true);
        }

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

        // Collections.synchronizedList(arrMany) -> "c" backing ArrayList walk.
        check_dense(ctx, "collections_synchronizedlist", g_sync, MANY);
        ctx.check("collections_synchronizedlist_size_witness_matches",
                  read_int_field(singleton, "synchronizedViewSize", -1) == MANY);

        // ════════════════════════════════════════════════════════════════════
        //  Vector / Stack ("elementData" + "elementCount").  The headline angle:
        //  the bound is elementCount, NOT elementData.length — a default Vector
        //  grows by DOUBLING (cap 10 -> 20) so vecMany has size 12 in a length-20
        //  backing array; the walk must emit exactly 12 with no phantom-null tail.
        // ════════════════════════════════════════════════════════════════════
        check_empty(ctx, "vector_empty", g_vec_empty);
        check_dense(ctx, "vector_many", g_vec_many, VEC_MANY);
        ctx.check("vector_many_size_witness_matches",
                  read_int_field(singleton, "vecManySize", -1) == VEC_MANY);
        ctx.check("vector_many_no_phantom_null_tail", g_vec_many.null_count == 0);

        check_dense(ctx, "vector_oversized", g_vec_oversized, VEC_MANY);
        ctx.check("vector_oversized_no_phantom_null_tail",
                  g_vec_oversized.null_count == 0);
        ctx.check("vector_oversized_size_not_capacity",
                  g_vec_oversized.size == VEC_MANY);

        check_with_null(ctx, "vector_with_null", g_vec_null);
        check_dense(ctx, "vector_two", g_vec_two, TWO);

        // Stack extends Vector: same elementData/elementCount backing walk.
        check_dense(ctx, "stack_many", g_stack_many, VEC_MANY);

        // ════════════════════════════════════════════════════════════════════
        //  CopyOnWriteArrayList ("array" Object[]; length IS the size).
        // ════════════════════════════════════════════════════════════════════
        check_empty(ctx, "cow_empty", g_cow_empty);
        check_dense(ctx, "cow_many", g_cow_many, VEC_MANY);
        ctx.check("cow_many_size_witness_matches",
                  read_int_field(singleton, "cowManySize", -1) == VEC_MANY);
        check_with_null(ctx, "cow_with_null", g_cow_null);
        check_dense(ctx, "cow_two", g_cow_two, TWO);

        // ════════════════════════════════════════════════════════════════════
        //  Boxed-Integer element lists: the backing walk is element-TYPE-agnostic.
        //  Each slot is a java.lang.Integer the native side reads through; value
        //  == index proves order, and the values 0..INT_LEN-1 are distinct so the
        //  decoded OOPs are distinct too (small Integers are interned per value).
        // ════════════════════════════════════════════════════════════════════
        ctx.check("integer_arraylist_seen", g_int_arr.seen);
        ctx.check("integer_arraylist_size_matches", g_int_arr.size == INT_LEN);
        ctx.check("integer_arraylist_all_non_null", g_int_arr.non_null == INT_LEN);
        ctx.check("integer_arraylist_values_equal_index", g_int_arr_values_ok);
        ctx.check("integer_arraylist_oops_distinct", g_int_arr.distinct_ok);

        ctx.check("integer_vector_seen", g_int_vec.seen);
        ctx.check("integer_vector_size_matches", g_int_vec.size == INT_LEN);
        ctx.check("integer_vector_all_non_null", g_int_vec.non_null == INT_LEN);
        ctx.check("integer_vector_values_equal_index", g_int_vec_values_ok);
        ctx.check("integer_vector_oops_distinct", g_int_vec.distinct_ok);

        // ════════════════════════════════════════════════════════════════════
        //  String-element ArrayList: the backing walk is element-TYPE-agnostic;
        //  each slot is a java.lang.String the native side reads via
        //  read_java_string.  Shape is HARD; the per-element content round-trip is
        //  best-effort (reference decode is compressed-oops-dependent).
        // ════════════════════════════════════════════════════════════════════
        check_typed_dense_shape(ctx, "string_arraylist", g_str_arr, STR_LEN);
        check_or_info(ctx, "string_arraylist_content_equals_s_index", g_str_values_ok,
                      "String element content did not all read back as \"s<index>\" on "
                      "this run (reference/String decode is compressed-oops-dependent); "
                      "size/shape still checked hard.");

        // ════════════════════════════════════════════════════════════════════
        //  Boxed-Long ArrayList: element k == LONG_BASE + k, every value > 2^32.
        //  This is the TRUNCATION catch — a 32-bit-truncating read of the boxed
        //  long would yield k (the low word) instead of LONG_BASE + k, flipping
        //  the value check.  Shape HARD; full-width value round-trip best-effort.
        // ════════════════════════════════════════════════════════════════════
        check_typed_dense_shape(ctx, "long_arraylist", g_long_arr, LONG_LEN);
        check_or_info(ctx, "long_arraylist_values_full_width_above_2pow32",
                      g_long_values_ok,
                      "boxed Long element values did not all read back as LONG_BASE+index "
                      "(> 2^32) on this run; either the reference decode or the 64-bit "
                      "Long.value read did not resolve here. A truncating 32-bit read "
                      "would also land here. size/shape still checked hard.");

        // ════════════════════════════════════════════════════════════════════
        //  Enum-element ArrayList: each slot is a real enum constant; ordinal ==
        //  index (positional order) and the inherited Enum `name` reads back
        //  non-empty.  Shape HARD; ordinal/name round-trip best-effort.
        // ════════════════════════════════════════════════════════════════════
        check_typed_dense_shape(ctx, "enum_arraylist", g_enum_arr, ENUM_LEN);
        check_or_info(ctx, "enum_arraylist_ordinal_equals_index_and_name_reads",
                      g_enum_values_ok,
                      "enum element ordinal!=index or inherited Enum name read empty on "
                      "this run (enum-constant reference decode / name String decode is "
                      "compressed-oops-dependent); size/shape still checked hard.");

        // ════════════════════════════════════════════════════════════════════
        //  Extra boxed-primitive element types: Boolean / Byte / Short / Character
        //  / Double.  The backing walk is element-TYPE-agnostic; shape is HARD and
        //  the per-element VALUE round-trip is best-effort (reference decode is
        //  compressed-oops-dependent).  These small boxes (Boolean/Byte/Short/Char)
        //  are JDK-INTERNED, so equal values share a cached OOP — distinctness is
        //  not asserted for them (Double boxes are NOT interned, so it is for them).
        // ════════════════════════════════════════════════════════════════════
        ctx.check("boolean_arraylist_seen", g_bool_arr.seen);
        ctx.check("boolean_arraylist_size_matches", g_bool_arr.size == BOX_LEN);
        ctx.check("boolean_arraylist_all_non_null", g_bool_arr.non_null == BOX_LEN);
        ctx.check("boolean_arraylist_no_null_slots", g_bool_arr.null_count == 0);
        check_or_info(ctx, "boolean_arraylist_values_false_true_false", g_bool_values_ok,
                      "boxed Boolean element values did not all read back as the "
                      "false,true,false pattern on this run (reference decode is "
                      "compressed-oops-dependent); size/shape still checked hard.");

        ctx.check("byte_arraylist_seen", g_byte_arr.seen);
        ctx.check("byte_arraylist_size_matches", g_byte_arr.size == BOX_LEN);
        ctx.check("byte_arraylist_all_non_null", g_byte_arr.non_null == BOX_LEN);
        ctx.check("byte_arraylist_no_null_slots", g_byte_arr.null_count == 0);
        check_or_info(ctx, "byte_arraylist_values_equal_index", g_byte_values_ok,
                      "boxed Byte element values did not all read back as index on this "
                      "run (reference decode is compressed-oops-dependent); size/shape "
                      "still checked hard.");

        ctx.check("short_arraylist_seen", g_short_arr.seen);
        ctx.check("short_arraylist_size_matches", g_short_arr.size == BOX_LEN);
        ctx.check("short_arraylist_all_non_null", g_short_arr.non_null == BOX_LEN);
        ctx.check("short_arraylist_no_null_slots", g_short_arr.null_count == 0);
        check_or_info(ctx, "short_arraylist_values_equal_index", g_short_values_ok,
                      "boxed Short element values did not all read back as index on this "
                      "run (reference decode is compressed-oops-dependent); size/shape "
                      "still checked hard.");

        ctx.check("char_arraylist_seen", g_char_arr.seen);
        ctx.check("char_arraylist_size_matches", g_char_arr.size == BOX_LEN);
        ctx.check("char_arraylist_all_non_null", g_char_arr.non_null == BOX_LEN);
        ctx.check("char_arraylist_no_null_slots", g_char_arr.null_count == 0);
        check_or_info(ctx, "char_arraylist_values_a_b_c", g_char_values_ok,
                      "boxed Character element values did not all read back as 'a'+index "
                      "on this run (reference decode is compressed-oops-dependent); "
                      "size/shape still checked hard.");

        // Double: wide (8-byte) boxed primitive, NOT interned -> distinctness HARD.
        check_typed_dense_shape(ctx, "double_arraylist", g_double_arr, BOX_LEN);
        check_or_info(ctx, "double_arraylist_values_equal_index_full_width",
                      g_double_values_ok,
                      "boxed Double element values did not all read back as (double)index "
                      "on this run; either the reference decode or the 8-byte Double.value "
                      "read did not resolve here. size/shape still checked hard.");

        // ════════════════════════════════════════════════════════════════════
        //  Null-PATTERN coverage: all-null lists, and null at the head / tail
        //  boundary.  Complements the single-mid-null with-null lists already
        //  covered — these prove the null slot is decoded correctly at index 0,
        //  at the last index, and when EVERY slot is null, in both backing
        //  families, with the bound staying `size`.
        // ════════════════════════════════════════════════════════════════════
        check_all_null(ctx, "arraylist_all_null", g_arr_all_null);
        check_all_null(ctx, "linkedlist_all_null", g_link_all_null);
        check_null_boundary(ctx, "arraylist_null_first", g_arr_null_first, 0);
        check_null_boundary(ctx, "linkedlist_null_first", g_link_null_first, 0);
        check_null_boundary(ctx, "arraylist_null_last", g_arr_null_last,
                            NULL_PATTERN_LEN - 1);

        // ════════════════════════════════════════════════════════════════════
        //  Extra SIZE coverage (10 / 16 / 1000) — positional order at each size
        //  on both backing families.  These are plain Elem lists, so the full
        //  dense bundle (size/order/tags/distinct/first/last) applies as HARD.
        // ════════════════════════════════════════════════════════════════════
        check_dense(ctx, "arraylist_ten", g_arr_ten, TEN);
        check_dense(ctx, "arraylist_sixteen", g_arr_sixteen, SIXTEEN);
        check_dense(ctx, "arraylist_thousand", g_arr_thousand, THOUSAND);
        check_dense(ctx, "linkedlist_thousand", g_link_thousand, THOUSAND);

        // Library to_vector ArrayList fast path agrees with the hand-walk at 1000.
        check_dense(ctx, "to_vector_arraylist_thousand", g_tv_arr_thousand, THOUSAND);
        ctx.check("to_vector_arraylist_thousand_matches_hand_walk_size",
                  g_tv_arr_thousand.size == g_arr_thousand.size);
        ctx.check("to_vector_arraylist_thousand_matches_hand_walk_last_id",
                  g_tv_arr_thousand.last_id == g_arr_thousand.last_id);

        // ArrayList vs LinkedList parity at 1000 (same content, two backing shapes).
        ctx.check("array_and_link_thousand_same_size",
                  g_arr_thousand.size == g_link_thousand.size);
        ctx.check("array_and_link_thousand_same_last_id",
                  g_arr_thousand.last_id == g_link_thousand.last_id);
        ctx.check("array_and_link_thousand_both_ordered",
                  g_arr_thousand.order_ok && g_link_thousand.order_ok);

        // ════════════════════════════════════════════════════════════════════
        //  Nested List-of-Map: outer ArrayList walk recovers inner Map OOPs; each
        //  is a real, DISTINCT heap object (Map content decode is collection_map).
        // ════════════════════════════════════════════════════════════════════
        ctx.check("nested_maps_outer_count_matches", g_nested_map_outer_n == MAP_OUTER);
        ctx.check("nested_maps_all_non_null", g_nested_map_nonnull == MAP_OUTER);
        ctx.check("nested_maps_distinct", g_nested_map_distinct);

        // ════════════════════════════════════════════════════════════════════
        //  Mixed-shape nested list: outer ArrayList -> Vector inner + COW inner.
        //  Both inners are fully re-walked by the shape-detecting walk, proving it
        //  dispatches a decoded element OOP to the Vector and COW backing shapes
        //  (the nested case previously covered only ArrayList/LinkedList inners).
        // ════════════════════════════════════════════════════════════════════
        ctx.check("nested_mixed_outer_count_matches", g_nested_mix_outer_n == NESTED_MIX_OUTER);
        ctx.check("nested_mixed_inners_distinct", g_nested_mix_distinct);
        ctx.check("nested_mixed_all_inner_lists_fully_walked",
                  g_nested_mix_inner_ok == NESTED_MIX_OUTER);

        // ════════════════════════════════════════════════════════════════════
        //  Aliased-duplicate lists: ONE Elem object stored at every slot.  The
        //  decoded element OOP is LEGITIMATELY the SAME across all slots, so the
        //  walk must still emit ALIAS_LEN slots (no dedup, no slot dropped), each
        //  value == ALIAS_VAL, and exactly ONE unique backing OOP — proving an
        //  honest repeated reference is handled (and is NOT the cycle/dup-node
        //  corruption the distinctness check guards in the BIG LinkedList case).
        // ════════════════════════════════════════════════════════════════════
        ctx.check("arraylist_alias_seen", g_arr_alias.seen);
        ctx.check("arraylist_alias_size_matches", g_arr_alias.size == ALIAS_LEN);
        ctx.check("arraylist_alias_all_non_null", g_arr_alias.non_null == ALIAS_LEN);
        ctx.check("arraylist_alias_no_null_slots", g_arr_alias.null_count == 0);
        ctx.check("arraylist_alias_single_unique_oop", g_arr_alias_unique_oops == 1);
        ctx.check("arraylist_alias_all_values_equal_ALIAS_VAL", g_arr_alias_all_val);

        ctx.check("linkedlist_alias_seen", g_link_alias.seen);
        ctx.check("linkedlist_alias_size_matches", g_link_alias.size == ALIAS_LEN);
        ctx.check("linkedlist_alias_all_non_null", g_link_alias.non_null == ALIAS_LEN);
        ctx.check("linkedlist_alias_no_null_slots", g_link_alias.null_count == 0);
        ctx.check("linkedlist_alias_single_unique_oop", g_link_alias_unique_oops == 1);
        ctx.check("linkedlist_alias_all_values_equal_ALIAS_VAL", g_link_alias_all_val);

        // ════════════════════════════════════════════════════════════════════
        //  Heterogeneous List<Object>: String, Integer, Elem, null, Elem — five
        //  different runtime element types (and a null) in one ArrayList.  The
        //  walk hands back one decoded OOP per slot regardless of class; the null
        //  slot is nullptr; the two genuine Elem slots (2 and 4) decode to
        //  distinct valid Elem objects and slot 4 reads id == 4 through the Elem
        //  wrapper — element-type-agnostic decoding proven at the slot level.
        // ════════════════════════════════════════════════════════════════════
        ctx.check("hetero_seen", g_hetero.seen);
        ctx.check("hetero_size_matches", g_hetero.size == HETERO_LEN);
        ctx.check("hetero_one_null_slot", g_hetero.null_count == 1);
        ctx.check("hetero_null_at_expected_index", g_hetero_null_at == HETERO_NULL_AT);
        ctx.check("hetero_non_null_count", g_hetero_nonnull == HETERO_LEN - 1);
        ctx.check("hetero_elem_slots_distinct_and_valid", g_hetero_elem_slots_ok);
        check_or_info(ctx, "hetero_elem4_id_reads_back_4", g_hetero_elem4_id_ok,
                      "the Elem at heteroList index 4 did not read id == 4 on this run "
                      "(reference decode is compressed-oops-dependent); shape/null pattern "
                      "still checked hard.");

        // ════════════════════════════════════════════════════════════════════
        //  Shuffled-insertion-order lists: ids were added in a NON-sorted
        //  permutation (SHUF_ORDER).  The walk must preserve INSERTION order, so
        //  vec[k].id == SHUF_ORDER[k] for every k — a list that sorted or
        //  reordered its walk would read a wrong id at some position.  This is the
        //  List insertion-order invariant stated against a permutation that is
        //  neither ascending nor descending (the ascending dense lists cannot
        //  distinguish "insertion order preserved" from "happens to be sorted").
        // ════════════════════════════════════════════════════════════════════
        ctx.check("arraylist_shuffled_seen", g_arr_shuffled.seen);
        ctx.check("arraylist_shuffled_size_matches", g_arr_shuffled.size == SHUF_LEN);
        ctx.check("arraylist_shuffled_all_non_null", g_arr_shuffled.non_null == SHUF_LEN);
        ctx.check("arraylist_shuffled_insertion_order_preserved", g_arr_shuf_insertion_ok);

        ctx.check("linkedlist_shuffled_seen", g_link_shuffled.seen);
        ctx.check("linkedlist_shuffled_size_matches", g_link_shuffled.size == SHUF_LEN);
        ctx.check("linkedlist_shuffled_all_non_null", g_link_shuffled.non_null == SHUF_LEN);
        ctx.check("linkedlist_shuffled_insertion_order_preserved", g_link_shuf_insertion_ok);

        // ════════════════════════════════════════════════════════════════════
        //  Size-1 null-only lists: a length-1 list whose only slot is null.  The
        //  smallest "has a null" case — size 1, one null slot, zero non-null, the
        //  lone slot decoded to nullptr (the bound is `size`==1).
        // ════════════════════════════════════════════════════════════════════
        ctx.check("arraylist_single_null_seen", g_arr_single_null.seen);
        ctx.check("arraylist_single_null_size_is_1", g_arr_single_null.size == 1);
        ctx.check("arraylist_single_null_one_null_slot", g_arr_single_null.null_count == 1);
        ctx.check("arraylist_single_null_no_elements", g_arr_single_null.non_null == 0);
        ctx.check("arraylist_single_null_at_index_0", g_arr_single_null.null_at == 0);

        ctx.check("linkedlist_single_null_seen", g_link_single_null.seen);
        ctx.check("linkedlist_single_null_size_is_1", g_link_single_null.size == 1);
        ctx.check("linkedlist_single_null_one_null_slot", g_link_single_null.null_count == 1);
        ctx.check("linkedlist_single_null_no_elements", g_link_single_null.non_null == 0);
        ctx.check("linkedlist_single_null_at_index_0", g_link_single_null.null_at == 0);

        // ════════════════════════════════════════════════════════════════════
        //  get_array_element bounds: in-range reads a real slot; index == length,
        //  index past length, and a negative index all return compressed 0
        //  (-> nullptr), NEVER an out-of-bounds read.  Probed directly against the
        //  elemArray backing Object[] (length OBJ_ARR_LEN).
        // ════════════════════════════════════════════════════════════════════
        ctx.check("get_array_element_in_range_nonzero", g_bounds_in_range_nonzero);
        ctx.check("get_array_element_at_length_is_zero", g_bounds_at_length_zero);
        ctx.check("get_array_element_past_length_is_zero", g_bounds_past_length_zero);
        ctx.check("get_array_element_negative_index_is_zero", g_bounds_negative_zero);

        // ════════════════════════════════════════════════════════════════════
        //  Adversarial / degenerate walk inputs: nullptr, a non-list Elem OOP, a
        //  raw Object[] OOP, a wrong-shape OOP, and an absurd LinkedList size.
        //  Each walk gates on backing-field presence + is_valid_pointer, so a
        //  wrong-shape or null input yields an EMPTY vector — no crash, no cycle,
        //  no OOB.  Reaching g_adv_reached_end proves every call returned.  This
        //  is the crash-safety HARD angle on the walk surface itself.
        // ════════════════════════════════════════════════════════════════════
        ctx.check("adversarial_all_calls_returned_no_crash", g_adv_reached_end);
        ctx.check("adversarial_null_arraylist_empty", g_adv_null_arraylist == 0);
        ctx.check("adversarial_null_linkedlist_empty", g_adv_null_linkedlist == 0);
        ctx.check("adversarial_null_shape_walk_empty", g_adv_shape_null == 0);
        ctx.check("adversarial_elem_as_arraylist_empty", g_adv_elem_as_list == 0);
        ctx.check("adversarial_objarray_shape_walk_empty", g_adv_string_as_list == 0);
        ctx.check("adversarial_objarray_as_arraylist_empty", g_adv_array_as_list == 0);
        ctx.check("adversarial_empty_linkedlist_bogus_size_empty",
                  g_adv_linked_bogus_n == 0);

        // ════════════════════════════════════════════════════════════════════
        //  Size-as-oracle: emitted element count == the list's OWN published
        //  size() (a plain int witness field the fixture exposes).  Asserts the
        //  walk emits exactly as many elements as the list reports, stated
        //  directly against the Java-reported size rather than a mirrored constant
        //  — across both core families, the at-scale lists, and the new shapes.
        // ════════════════════════════════════════════════════════════════════
        ctx.check("size_oracle_arraylist_many", g_oracle_arr_many);
        ctx.check("size_oracle_linkedlist_many", g_oracle_link_many);
        ctx.check("size_oracle_arraylist_thousand", g_oracle_arr_thousand);
        ctx.check("size_oracle_linkedlist_thousand", g_oracle_link_thousand);
        ctx.check("size_oracle_arraylist_alias", g_oracle_arr_alias);
        ctx.check("size_oracle_heterogeneous", g_oracle_hetero);
        ctx.check("size_oracle_arraylist_shuffled", g_oracle_arr_shuffled);

        // ════════════════════════════════════════════════════════════════════
        //  List.of(...) (JDK 9+).  ListN backing "elements" Object[] IS decoded
        //  (List.of() empty, List.of(4) dense); List12 (e0/e1 EMPTY sentinel) is
        //  CHARACTERIZED via the published Java size() witness, never element-
        //  decoded.  On Java 8 the whole block is recorded [INFO] and skipped.
        // ════════════════════════════════════════════════════════════════════
        if (g_listof_available)
        {
            check_empty(ctx, "list_of_empty_listN", g_listof0);
            check_dense(ctx, "list_of_n_listN", g_listofN, LISTOF_N);
            // List12 characterization: the published size() is the oracle.
            ctx.check("list_of_1_size_witness_is_1", g_listof1_size_witness == 1);
            ctx.check("list_of_2_size_witness_is_2", g_listof2_size_witness == 2);
            ctx.record("[INFO] collection_list: List.of(1)/List.of(1,2) are "
                       "ImmutableCollections$List12; their unused 'e1' slot holds a "
                       "shared non-null EMPTY sentinel for size 1, so the backing-field "
                       "walk CHARACTERIZES them via the published size() witness rather "
                       "than decoding e0/e1 (which would mis-emit 2 elements for size 1). "
                       "List.of()/List.of(4) are ListN and ARE hand-walked.");
        }
        else
        {
            ctx.record("[INFO] collection_list: List.of(...) unavailable on this JDK "
                       "(Java 8); skipping the immutable-List.of coverage. The fixture "
                       "builds those fields reflectively, so they are simply absent here.");
        }

        // ════════════════════════════════════════════════════════════════════
        //  subList views: CHARACTERIZED via the published Java size() witness.
        //  The ArrayList$SubList / AbstractList$SubList backing field shape moved
        //  across JDKs (8: parent/parentOffset; 9+: root/parent/offset) and the
        //  view carries no element array of its own, so the module pins its
        //  size() oracle (== SUB_LEN) and records [INFO] instead of a fragile raw
        //  walk.  (The library's to_vector CAN decode a subList — via its generic
        //  size()+get(int) fallback — but that issues Java calls, forbidden from
        //  this no-detour worker body; that is the characterization reason.)
        // ════════════════════════════════════════════════════════════════════
        ctx.check("arraylist_sublist_size_witness_is_sub_len",
                  g_arr_sublist_size_witness == SUB_LEN);
        ctx.check("linkedlist_sublist_size_witness_is_sub_len",
                  g_link_sublist_size_witness == SUB_LEN);
        ctx.record("[INFO] collection_list: ArrayList/LinkedList subList(from,to) views "
                   "are characterized via their published size() witness (== "
                   + std::to_string(SUB_LEN) + "); their SubList backing shape "
                   "(parent/offset, renamed root/offset in JDK 9+) carries no element "
                   "array of its own, so the no-Java-call worker body does not raw-walk "
                   "them. to_vector decodes them through its get(int) fallback.");

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

        // Four backing shapes (ArrayList / Vector / Stack / COW) hold the SAME
        // 12-element id==index content, so all four fast paths must agree on
        // size / first / last and all be ordered — proving the hand-walk selects
        // each backing shape from the runtime klass and decodes them equivalently.
        ctx.check("arraylist_vector_cow_same_many_size",
                  g_arr_many.size == VEC_MANY
                  && g_vec_many.size == VEC_MANY
                  && g_stack_many.size == VEC_MANY
                  && g_cow_many.size == VEC_MANY);
        ctx.check("arraylist_vector_cow_same_many_first_id",
                  g_arr_many.first_id == g_vec_many.first_id
                  && g_vec_many.first_id == g_stack_many.first_id
                  && g_stack_many.first_id == g_cow_many.first_id);
        ctx.check("arraylist_vector_cow_same_many_last_id",
                  g_arr_many.last_id == g_vec_many.last_id
                  && g_vec_many.last_id == g_stack_many.last_id
                  && g_stack_many.last_id == g_cow_many.last_id);
        ctx.check("arraylist_vector_cow_all_ordered",
                  g_vec_many.order_ok && g_stack_many.order_ok && g_cow_many.order_ok);

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

        // ════════════════════════════════════════════════════════════════════
        //  PUBLIC value_t::to_vector ENTRY POINT — get_field(...)->get().to_vector,
        //  the call a real detour uses.  Distinct from the g_tv_* group (which
        //  drives the lower-level collection::to_vector on a decoded OOP directly):
        //  this exercises the field-SIGNATURE branch in value_t::to_vector —
        //    * 'L...;' reference List field  -> collection::to_vector fast path
        //    * '[L...;' object-array field   -> the raw-array branch
        //  — neither of which the lower-level path can ever reach.
        // ════════════════════════════════════════════════════════════════════
        // 'L...;' reference branch: ArrayList single / many / null-slot.
        check_dense(ctx, "public_to_vector_arraylist_single", g_pv_arr_single, 1);
        check_dense(ctx, "public_to_vector_arraylist_many", g_pv_arr_many, MANY);
        check_with_null(ctx, "public_to_vector_arraylist_with_null", g_pv_arr_null);

        // 'L...;' reference branch: LinkedList single / many / null Node.item.
        check_dense(ctx, "public_to_vector_linkedlist_single", g_pv_link_single, 1);
        check_dense(ctx, "public_to_vector_linkedlist_many", g_pv_link_many, MANY);
        check_with_null(ctx, "public_to_vector_linkedlist_with_null", g_pv_link_null);

        // linkBig is declared `List` but is a LinkedList at runtime: the public
        // entry must still select the LinkedList fast path from the runtime klass.
        check_dense(ctx, "public_to_vector_linkbig_runtime_klass_dispatch",
                    g_pv_link_big, BIG);

        // '[L...;' object-array branch: the documented Object[] entry point.
        check_dense(ctx, "public_to_vector_object_array", g_pv_elem_array, OBJ_ARR_LEN);

        // The public entry must agree element-for-element with the lower-level
        // collection::to_vector AND the hand-walk on the same lists — three
        // independent decoders converging is a strong cross-implementation oracle.
        ctx.check("public_to_vector_arraylist_matches_collection_to_vector",
                  g_pv_arr_many.size == g_tv_arr_many.size
                  && g_pv_arr_many.last_id == g_tv_arr_many.last_id);
        ctx.check("public_to_vector_linkedlist_matches_collection_to_vector",
                  g_pv_link_many.size == g_tv_link_many.size
                  && g_pv_link_many.last_id == g_tv_link_many.last_id);
        ctx.check("public_to_vector_object_array_matches_hand_walk",
                  g_pv_elem_array.size == g_elem_array.size
                  && g_pv_elem_array.last_id == g_elem_array.last_id);
        ctx.check("public_to_vector_linkbig_matches_hand_walk",
                  g_pv_link_big.size == g_link_big.size
                  && g_pv_link_big.last_id == g_link_big.last_id);
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
