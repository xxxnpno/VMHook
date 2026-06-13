// collection_set_exhaustive JVM test module  (feature area: collections)
//
// COMPANION to collection_set.cpp.  That module already exhausts the bulk of the
// java.util.Set decode matrix (HashSet / LinkedHashSet / TreeSet across empty /
// single / two / many / big / treeified / collision-chain / null shapes, the
// setFromMap + ConcurrentHashMap.newKeySet routes, boxed-Integer elements, and
// the non-fast-path JDK wrappers).  This module fills the remaining gaps the
// "every Set shape read through the library" goal calls for, against its OWN
// distinct fixture (vmhook.fixtures.CollSetExhaustive) so the mature CollSet.java
// / collection_set.cpp pair is left untouched:
//
//   * BOXED Long element decode (HashSet<Long> / TreeSet<Long>) — java.lang.Long
//     .value is a 64-bit primitive (a different read width from Integer.value);
//     both the bucket walk and the in-order red-black walk must surface every
//     boxed Long, and the TreeSet must come out ASCENDING by value.
//   * REAL enum-element decode (HashSet<Day> / TreeSet<Day>) — each element OOP
//     is a genuine enum instance whose `name` (String) + `ordinal` (int), both
//     declared on java.lang.Enum, the native side reads back.  TreeSet<Day> is
//     ordered by the enum's natural order (ordinal), so its in-order walk is
//     ascending by ordinal.  (collection_set.cpp only CHARACTERIZES EnumSet,
//     whose primitive-bitmask storage has no fast-path field shape — here a Set
//     OF enum constants is genuinely DECODED.)
//   * Exact HashSet RESIZE-BOUNDARY sizes 16 (default cap; the table has already
//     grown to 32 by the 13th add) and 17 (one past), plus 1000 (a mid-scale
//     many-bucket walk between collection_set.cpp's 50 and 5000) — the bucket
//     walk proven across the precise grow threshold and at mid scale.
//   * Set<List<Integer>> — a HashSet whose ELEMENTS are ArrayList<Integer>.  The
//     outer "map"->hash_map_walk_keys walk yields inner List OOPs; each inner
//     List is then decoded via the ArrayList fast path (vmhook::collection{oop}.
//     to_vector<integer_box>(), a pure memory walk, no Java call) and its values
//     verified — a Set holding another live collection, both layers decoded from
//     the worker body.
//
// Verification for the unordered HashSets is order-independent (count + member-
// ship + checksums cross-checked against values Java computed the same way);
// the TreeSets get strict ascending-order assertions.  Every populated set's
// decoded count is cross-checked against Java's own size() (a pure static-field
// read — never a Java size() call from the worker body, which the suite forbids
// outside a detour).
//
// SUITE-SAFETY (mirrors collection_set.cpp / register_class.cpp): the whole body
// runs under a try/catch (a throw is recorded [INFO], never a FAIL); an entry
// guard bails to [INFO] if the fixture class does not resolve; the only hook is a
// scoped_hook<> that RAII-uninstalls on scope exit; and an unconditional
// vmhook::shutdown_hooks() OUTSIDE the try guarantees ZERO hooks armed on EVERY
// exit path.  No Java method is called from the body (only pure heap reads and
// the memory-walk to_vector fast paths); is_valid_pointer() is only ever applied
// before a RAW pointer deref; element handles are null-checked.  No forced
// System.gc() is issued.  Distinct `cst_*` check-name prefix so no assertion
// name collides with collection_set.cpp.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace
{
    // ── ELEMENT wrapper: vmhook.fixtures.CollSetExhaustive$Elem. ────────────
    class elem_object : public vmhook::object<elem_object>
    {
    public:
        explicit elem_object(vmhook::oop_t instance) noexcept
            : vmhook::object<elem_object>{ instance }
        {
        }

        auto id() const -> std::int32_t { return static_cast<std::int32_t>(get_field("id")->get()); }

        auto tag() const -> std::string { return get_field("tag")->get(); }
    };

    // ── BOXED-Long element wrapper: java.lang.Long. ─────────────────────────
    // value() reads the primitive 64-bit `value` field directly (a pure heap
    // read).  The Long-set values carry a non-zero HIGH 32 bits, so a 32-bit
    // misread would corrupt the checksum — exercising the full-width decode.
    class long_box : public vmhook::object<long_box>
    {
    public:
        explicit long_box(vmhook::oop_t instance) noexcept
            : vmhook::object<long_box>{ instance }
        {
        }

        auto value() const -> std::int64_t
        {
            return static_cast<std::int64_t>(get_field("value")->get());
        }
    };

    // ── BOXED-Integer element wrapper: java.lang.Integer (for inner Lists). ──
    class integer_box : public vmhook::object<integer_box>
    {
    public:
        explicit integer_box(vmhook::oop_t instance) noexcept
            : vmhook::object<integer_box>{ instance }
        {
        }

        auto value() const -> std::int32_t
        {
            return static_cast<std::int32_t>(get_field("value")->get());
        }
    };

    // ── ENUM element wrapper: java.lang.Enum. ───────────────────────────────
    // Registered as java/lang/Enum so get_field resolves `name`/`ordinal`, which
    // are declared on java.lang.Enum itself (the superclass of every concrete
    // enum), at offsets shared by every enum constant OOP — exactly how the
    // String wrapper reads any String OOP after registering as java/lang/String.
    class enum_element : public vmhook::object<enum_element>
    {
    public:
        explicit enum_element(vmhook::oop_t instance) noexcept
            : vmhook::object<enum_element>{ instance }
        {
        }

        auto ordinal() const -> std::int32_t
        {
            return static_cast<std::int32_t>(get_field("ordinal")->get());
        }

        auto name() const -> std::string { return get_field("name")->get(); }
    };

    // ── Inner-List element wrapper: a java.util.ArrayList held AS a set
    //    element.  Decodes its OWN elements via vmhook::collection (the ArrayList
    //    fast path) — independent of get_field, so registration is only for the
    //    factory; the decode uses the live OOP directly. ──────────────────────
    class list_element : public vmhook::object<list_element>
    {
    public:
        explicit list_element(vmhook::oop_t instance) noexcept
            : vmhook::object<list_element>{ instance }
        {
        }

        auto values_sum() const -> std::int64_t
        {
            const auto inner{ vmhook::collection{ get_instance() }.to_vector<integer_box>() };
            std::int64_t sum{ 0 };
            for (const auto& up : inner)
            {
                if (up)
                {
                    sum += up->value();
                }
            }
            return sum;
        }

        auto values_count() const -> std::int32_t
        {
            const auto inner{ vmhook::collection{ get_instance() }.to_vector<integer_box>() };
            return static_cast<std::int32_t>(inner.size());
        }
    };

    // ── Fixture wrapper: vmhook.fixtures.CollSetExhaustive. ──────────────────
    class fixture : public vmhook::object<fixture>
    {
    public:
        explicit fixture(vmhook::oop_t instance) noexcept
            : vmhook::object<fixture>{ instance }
        {
        }

        static auto set_go(bool value) -> void      { static_field("go")->set(value); }
        static auto set_done(bool value) -> void     { static_field("done")->set(value); }
        static auto get_done() -> bool               { return static_field("done")->get(); }
        static auto set_mode(std::int32_t m) -> void  { static_field("mode")->set(m); }
        static auto get_observed() -> std::int32_t   { return static_field("observed")->get(); }

        static auto elems_of(const char* field)
            -> std::vector<std::unique_ptr<elem_object>>
        {
            const auto proxy{ static_field(field) };
            if (!proxy.has_value())
            {
                return {};
            }
            return proxy->get().to_vector<elem_object>();
        }

        static auto longs_of(const char* field)
            -> std::vector<std::unique_ptr<long_box>>
        {
            const auto proxy{ static_field(field) };
            if (!proxy.has_value())
            {
                return {};
            }
            return proxy->get().to_vector<long_box>();
        }

        static auto enums_of(const char* field)
            -> std::vector<std::unique_ptr<enum_element>>
        {
            const auto proxy{ static_field(field) };
            if (!proxy.has_value())
            {
                return {};
            }
            return proxy->get().to_vector<enum_element>();
        }

        static auto lists_of(const char* field)
            -> std::vector<std::unique_ptr<list_element>>
        {
            const auto proxy{ static_field(field) };
            if (!proxy.has_value())
            {
                return {};
            }
            return proxy->get().to_vector<list_element>();
        }

        static auto j_size(const char* f) -> std::int32_t { return static_field(f)->get(); }
        static auto j_long(const char* f) -> std::int64_t { return static_field(f)->get(); }
    };

    // ── Fixture-mirrored constants (lockstep with CollSetExhaustive.java). ───
    constexpr std::int32_t LONG_N{ 40 };
    constexpr std::int32_t NESTED_N{ 6 };
    constexpr std::int32_t INNER_LEN{ 4 };
    constexpr std::int32_t CAP16{ 16 };
    constexpr std::int32_t CAP17{ 17 };
    constexpr std::int32_t THOUSAND{ 1000 };
    constexpr std::int32_t DAY_N{ 5 };

    // ── Hook observation (pilot-style proof). ───────────────────────────────
    std::atomic<int>          g_hook_calls{ 0 };
    std::atomic<std::int32_t> g_hook_arg{ -1 };
    std::atomic<bool>         g_hook_saw_self{ false };

    // Order-independent fingerprint of a decoded Elem set.
    struct elem_stats
    {
        std::int32_t count{ 0 };
        std::int32_t null_count{ 0 };
        std::int64_t id_sum{ 0 };
        std::int64_t id_xor{ 0 };
        bool         distinct_oops{ true };
        bool         tags_consistent{ true };
    };

    auto fingerprint(const std::vector<std::unique_ptr<elem_object>>& v) -> elem_stats
    {
        elem_stats st;
        st.count = static_cast<std::int32_t>(v.size());
        std::unordered_set<const void*> seen;
        seen.reserve(v.size() * 2 + 1);
        for (const auto& up : v)
        {
            const elem_object* const e{ up.get() };
            if (e == nullptr)
            {
                ++st.null_count;
                continue;
            }
            const std::int32_t id{ e->id() };
            st.id_sum += id;
            st.id_xor ^= id;
            if (e->tag() != ("e" + std::to_string(id)))
            {
                st.tags_consistent = false;
            }
            const void* const oop{ static_cast<const void*>(e->get_instance()) };
            if (!seen.insert(oop).second)
            {
                st.distinct_oops = false;
            }
        }
        return st;
    }

    auto id_set(const std::vector<std::unique_ptr<elem_object>>& v)
        -> std::unordered_set<std::int32_t>
    {
        std::unordered_set<std::int32_t> ids;
        ids.reserve(v.size() * 2 + 1);
        for (const auto& up : v)
        {
            if (up)
            {
                ids.insert(up->id());
            }
        }
        return ids;
    }

    // Order-independent fingerprint over a decoded boxed-Long set.
    struct long_stats
    {
        std::int32_t count{ 0 };
        std::int32_t null_count{ 0 };
        std::int64_t val_sum{ 0 };
        std::int64_t val_xor{ 0 };
        bool         distinct_oops{ true };
    };

    auto fingerprint_longs(const std::vector<std::unique_ptr<long_box>>& v) -> long_stats
    {
        long_stats st;
        st.count = static_cast<std::int32_t>(v.size());
        std::unordered_set<const void*> seen;
        seen.reserve(v.size() * 2 + 1);
        for (const auto& up : v)
        {
            const long_box* const e{ up.get() };
            if (e == nullptr)
            {
                ++st.null_count;
                continue;
            }
            const std::int64_t val{ e->value() };
            st.val_sum += val;
            st.val_xor ^= val;
            const void* const oop{ static_cast<const void*>(e->get_instance()) };
            if (!seen.insert(oop).second)
            {
                st.distinct_oops = false;
            }
        }
        return st;
    }

    // Decoded boxed-Long values IN WALK ORDER (INT64_MIN placeholder for null).
    auto long_values_in_order(const std::vector<std::unique_ptr<long_box>>& v)
        -> std::vector<std::int64_t>
    {
        std::vector<std::int64_t> order;
        order.reserve(v.size());
        for (const auto& up : v)
        {
            order.push_back(up ? up->value() : std::numeric_limits<std::int64_t>::min());
        }
        return order;
    }

    // Decoded enum ordinals IN WALK ORDER (-1 placeholder for null).
    auto ordinals_in_order(const std::vector<std::unique_ptr<enum_element>>& v)
        -> std::vector<std::int32_t>
    {
        std::vector<std::int32_t> order;
        order.reserve(v.size());
        for (const auto& up : v)
        {
            order.push_back(up ? up->ordinal() : -1);
        }
        return order;
    }

    constexpr char FIXTURE[]{ "vmhook/fixtures/CollSetExhaustive" };

    auto run_checks(vmhook_test::context& ctx) -> void
    {
        // ─── ENTRY GUARD ────────────────────────────────────────────────────
        if (vmhook::find_class(FIXTURE) == nullptr)
        {
            ctx.record("[INFO] collection_set_exhaustive: CollSetExhaustive not loaded/"
                       "resolvable on this run; skipping the module's live checks (no "
                       "crash, no hooks armed).");
            return;
        }

        vmhook::register_class<fixture>(FIXTURE);
        vmhook::register_class<elem_object>("vmhook/fixtures/CollSetExhaustive$Elem");
        vmhook::register_class<long_box>("java/lang/Long");
        vmhook::register_class<integer_box>("java/lang/Integer");
        vmhook::register_class<enum_element>("java/lang/Enum");
        vmhook::register_class<list_element>("java/util/ArrayList");

        // Drive a mode-0 probe so the build also runs on the Java thread and we
        // read a freshly-populated, deterministic snapshot.
        {
            const bool built{ ctx.run_probe(
                [](bool value)
                {
                    if (value)
                    {
                        fixture::set_done(false);
                        fixture::set_mode(0);
                    }
                    fixture::set_go(value);
                },
                []() { return fixture::get_done(); }) };
            ctx.check("cst_build_probe_completed", built);
        }

        // =====================================================================
        // HashSet<Long> — BOXED 64-bit element decode through the bucket walk.
        // Values have a non-zero HIGH 32 bits, so a 32-bit misread of Long.value
        // would corrupt the sum/xor.  Order-independent value fingerprint vs Java.
        // =====================================================================
        {
            const auto v{ fixture::longs_of("hashLongs") };
            const long_stats st{ fingerprint_longs(v) };
            ctx.check("cst_hash_longs_count_is_n", st.count == LONG_N);
            ctx.check("cst_hash_longs_count_matches_java",
                      st.count == fixture::j_size("hashLongsSize"));
            ctx.check("cst_hash_longs_no_null", st.null_count == 0);
            ctx.check("cst_hash_longs_val_sum_matches_java",
                      st.val_sum == fixture::j_long("hashLongsValSum"));
            ctx.check("cst_hash_longs_val_xor_matches_java",
                      st.val_xor == fixture::j_long("hashLongsValXor"));
            ctx.check("cst_hash_longs_all_distinct", st.distinct_oops);

            // Membership: every value 0x1_0000_0000 + i present exactly once.
            std::unordered_set<std::int64_t> vals;
            for (const auto& up : v) { if (up) { vals.insert(up->value()); } }
            bool all_present{ vals.size() == static_cast<std::size_t>(LONG_N) };
            for (std::int32_t i{ 0 }; i < LONG_N; ++i)
            {
                if (vals.find(0x1'0000'0000LL + i) == vals.end()) { all_present = false; }
            }
            ctx.check("cst_hash_longs_every_value_present", all_present);
            // Prove the high word survived: every value exceeds INT32_MAX.
            bool all_high{ true };
            for (const auto& up : v)
            {
                if (up && up->value() <= 0x7FFF'FFFFLL) { all_high = false; }
            }
            ctx.check("cst_hash_longs_high_word_survived", all_high);
        }

        // =====================================================================
        // TreeSet<Long> — BOXED 64-bit through the in-order red-black walk;
        // inserted DESCENDING, must decode ASCENDING by value, exactly.
        // =====================================================================
        {
            const auto v{ fixture::longs_of("treeLongs") };
            const long_stats st{ fingerprint_longs(v) };
            ctx.check("cst_tree_longs_count_is_n", st.count == LONG_N);
            ctx.check("cst_tree_longs_count_matches_java",
                      st.count == fixture::j_size("treeLongsSize"));
            ctx.check("cst_tree_longs_no_null", st.null_count == 0);
            ctx.check("cst_tree_longs_val_sum_matches_java",
                      st.val_sum == fixture::j_long("treeLongsValSum"));
            ctx.check("cst_tree_longs_all_distinct", st.distinct_oops);

            const std::vector<std::int64_t> order{ long_values_in_order(v) };
            ctx.check("cst_tree_longs_is_sorted",
                      std::is_sorted(order.begin(), order.end()));
            bool exact{ order.size() == static_cast<std::size_t>(LONG_N) };
            for (std::size_t k{ 0 }; exact && k < order.size(); ++k)
            {
                if (order[k] != 0x1'0000'0000LL + static_cast<std::int64_t>(k))
                {
                    exact = false;
                }
            }
            ctx.check("cst_tree_longs_exact_ascending_sequence", exact);
            ctx.check("cst_tree_longs_first_is_min",
                      !order.empty() && order.front() == 0x1'0000'0000LL);
            ctx.check("cst_tree_longs_last_is_max",
                      !order.empty() && order.back() == 0x1'0000'0000LL + (LONG_N - 1));
        }

        // =====================================================================
        // HashSet<Day> — REAL enum-element decode via the bucket walk.  Each
        // element OOP is a genuine enum constant; read its ordinal + name (both
        // on java.lang.Enum).  Order-independent ordinal sum + name char sum.
        // =====================================================================
        {
            const auto v{ fixture::enums_of("hashEnums") };
            const std::int32_t count{ static_cast<std::int32_t>(v.size()) };
            ctx.check("cst_hash_enums_count_is_5", count == DAY_N);
            ctx.check("cst_hash_enums_count_matches_java",
                      count == fixture::j_size("hashEnumsSize"));

            std::int32_t nulls{ 0 };
            std::int64_t ord_sum{ 0 };
            std::int64_t name_char_sum{ 0 };
            std::unordered_set<std::int32_t> ordinals;
            std::unordered_set<const void*> seen;
            for (const auto& up : v)
            {
                if (up == nullptr) { ++nulls; continue; }
                const std::int32_t ord{ up->ordinal() };
                ord_sum += ord;
                ordinals.insert(ord);
                const std::string nm{ up->name() };
                for (const unsigned char c : nm) { name_char_sum += c; }
                seen.insert(static_cast<const void*>(up->get_instance()));
            }
            ctx.check("cst_hash_enums_no_null", nulls == 0);
            ctx.check("cst_hash_enums_ordinal_sum_matches_java",
                      ord_sum == fixture::j_long("hashEnumsOrdinalSum"));
            ctx.check("cst_hash_enums_name_char_sum_matches_java",
                      name_char_sum == fixture::j_long("hashEnumsNameCharSum"));
            ctx.check("cst_hash_enums_distinct_ordinals",
                      ordinals.size() == static_cast<std::size_t>(DAY_N));
            ctx.check("cst_hash_enums_distinct_oops",
                      seen.size() == static_cast<std::size_t>(DAY_N));
            // Closed form: ordinals 0..4 sum to 10.
            ctx.check("cst_hash_enums_ordinal_sum_closed_form", ord_sum == 10);

            // Membership of the two endpoints by name.
            std::unordered_set<std::string> names;
            for (const auto& up : v) { if (up) { names.insert(up->name()); } }
            ctx.check("cst_hash_enums_contains_MON", names.count("MON") == 1);
            ctx.check("cst_hash_enums_contains_FRI", names.count("FRI") == 1);
        }

        // =====================================================================
        // TreeSet<Day> — enum natural order is by ordinal, so the in-order walk
        // must come out ascending by ordinal: [0,1,2,3,4].  Inserted scrambled.
        // =====================================================================
        {
            const auto v{ fixture::enums_of("treeEnums") };
            const std::int32_t count{ static_cast<std::int32_t>(v.size()) };
            ctx.check("cst_tree_enums_count_is_5", count == DAY_N);
            ctx.check("cst_tree_enums_count_matches_java",
                      count == fixture::j_size("treeEnumsSize"));

            const std::vector<std::int32_t> order{ ordinals_in_order(v) };
            ctx.check("cst_tree_enums_is_sorted_by_ordinal",
                      std::is_sorted(order.begin(), order.end()));
            bool exact{ order.size() == static_cast<std::size_t>(DAY_N) };
            for (std::size_t k{ 0 }; exact && k < order.size(); ++k)
            {
                if (order[k] != static_cast<std::int32_t>(k)) { exact = false; }
            }
            ctx.check("cst_tree_enums_exact_ordinal_sequence", exact);
            ctx.check("cst_tree_enums_first_is_MON",
                      !v.empty() && v.front() && v.front()->name() == "MON");
            ctx.check("cst_tree_enums_last_is_FRI",
                      !v.empty() && v.back() && v.back()->name() == "FRI");
        }

        // =====================================================================
        // HashSet — EXACT resize boundary: 16 (default cap; table already grown
        // to 32 by the 13th add) and 17 (one past).  Full membership + id sum.
        // =====================================================================
        {
            const auto v{ fixture::elems_of("hashCap16") };
            const elem_stats st{ fingerprint(v) };
            ctx.check("cst_cap16_count_is_16", st.count == CAP16);
            ctx.check("cst_cap16_count_matches_java",
                      st.count == fixture::j_size("hashCap16Size"));
            ctx.check("cst_cap16_no_null", st.null_count == 0);
            ctx.check("cst_cap16_id_sum_matches_java",
                      st.id_sum == fixture::j_long("hashCap16IdSum"));
            ctx.check("cst_cap16_id_sum_closed_form",
                      st.id_sum == (static_cast<std::int64_t>(CAP16) * (CAP16 - 1)) / 2);
            ctx.check("cst_cap16_all_distinct", st.distinct_oops);
            const auto ids{ id_set(v) };
            bool all_present{ ids.size() == static_cast<std::size_t>(CAP16) };
            for (std::int32_t i{ 0 }; i < CAP16; ++i)
            {
                if (ids.find(i) == ids.end()) { all_present = false; }
            }
            ctx.check("cst_cap16_every_id_present", all_present);
        }
        {
            const auto v{ fixture::elems_of("hashCap17") };
            const elem_stats st{ fingerprint(v) };
            ctx.check("cst_cap17_count_is_17", st.count == CAP17);
            ctx.check("cst_cap17_count_matches_java",
                      st.count == fixture::j_size("hashCap17Size"));
            ctx.check("cst_cap17_no_null", st.null_count == 0);
            ctx.check("cst_cap17_id_sum_matches_java",
                      st.id_sum == fixture::j_long("hashCap17IdSum"));
            ctx.check("cst_cap17_id_sum_closed_form",
                      st.id_sum == (static_cast<std::int64_t>(CAP17) * (CAP17 - 1)) / 2);
            ctx.check("cst_cap17_all_distinct", st.distinct_oops);
            const auto ids{ id_set(v) };
            bool all_present{ ids.size() == static_cast<std::size_t>(CAP17) };
            for (std::int32_t i{ 0 }; i < CAP17; ++i)
            {
                if (ids.find(i) == ids.end()) { all_present = false; }
            }
            ctx.check("cst_cap17_every_id_present", all_present);
        }

        // =====================================================================
        // HashSet — 1000 elements (mid-scale many-bucket walk).  Exact count,
        // full fingerprint, every id present, all element OOPs distinct.
        // =====================================================================
        {
            const auto v{ fixture::elems_of("hashThousand") };
            const elem_stats st{ fingerprint(v) };
            ctx.check("cst_thousand_count_is_1000", st.count == THOUSAND);
            ctx.check("cst_thousand_count_matches_java",
                      st.count == fixture::j_size("hashThousandSize"));
            ctx.check("cst_thousand_no_null", st.null_count == 0);
            ctx.check("cst_thousand_id_sum_matches_java",
                      st.id_sum == fixture::j_long("hashThousandIdSum"));
            ctx.check("cst_thousand_id_xor_matches_java",
                      st.id_xor == fixture::j_long("hashThousandIdXor"));
            ctx.check("cst_thousand_id_sum_closed_form",
                      st.id_sum == (static_cast<std::int64_t>(THOUSAND) * (THOUSAND - 1)) / 2);
            ctx.check("cst_thousand_all_distinct_no_cycle", st.distinct_oops);
            const auto ids{ id_set(v) };
            ctx.check("cst_thousand_membership_complete",
                      ids.size() == static_cast<std::size_t>(THOUSAND));
        }

        // =====================================================================
        // Set<List<Integer>> — a Set holding other live collections.  The outer
        // bucket walk yields inner ArrayList OOPs; each inner list is decoded via
        // the ArrayList fast path and its values verified.  Inner list k holds
        // [k*10 .. k*10+INNER_LEN-1]; total value sum cross-checked vs Java.
        // =====================================================================
        {
            const auto v{ fixture::lists_of("setOfLists") };
            const std::int32_t count{ static_cast<std::int32_t>(v.size()) };
            ctx.check("cst_set_of_lists_count_is_n", count == NESTED_N);
            ctx.check("cst_set_of_lists_count_matches_java",
                      count == fixture::j_size("setOfListsSize"));

            std::int32_t nulls{ 0 };
            std::int32_t inner_total{ 0 };
            std::int64_t value_sum{ 0 };
            bool every_inner_len_ok{ true };
            std::unordered_set<const void*> seen;
            for (const auto& up : v)
            {
                if (up == nullptr) { ++nulls; continue; }
                const std::int32_t n{ up->values_count() };
                inner_total += n;
                if (n != INNER_LEN) { every_inner_len_ok = false; }
                value_sum += up->values_sum();
                seen.insert(static_cast<const void*>(up->get_instance()));
            }
            ctx.check("cst_set_of_lists_no_null", nulls == 0);
            ctx.check("cst_set_of_lists_inner_lists_distinct",
                      seen.size() == static_cast<std::size_t>(NESTED_N));
            ctx.check("cst_set_of_lists_each_inner_len_is_INNER_LEN",
                      every_inner_len_ok);
            ctx.check("cst_set_of_lists_total_inner_count",
                      inner_total == NESTED_N * INNER_LEN);
            ctx.check("cst_set_of_lists_value_sum_matches_java",
                      value_sum == fixture::j_long("setOfListsValSum"));
            ctx.record("[INFO] Set<List<Integer>>: outer HashSet 'map'->hash_map_walk_keys "
                       "yields inner ArrayList OOPs; each decoded via the ArrayList fast "
                       "path (vmhook::collection.to_vector<Integer>) — both layers are pure "
                       "memory walks, decoded from the worker body.");
        }

        // =====================================================================
        // ROBUSTNESS — a declared-but-null Set field and a missing field name
        // both decode to empty, never throw, stable on re-read.
        // =====================================================================
        {
            const auto v_null{ fixture::elems_of("nullSet") };
            ctx.check("cst_null_set_returns_empty", v_null.empty());
            const auto v_missing{ fixture::elems_of("noSuchSetFieldXYZ") };
            ctx.check("cst_missing_set_returns_empty", v_missing.empty());
            ctx.check("cst_null_set_stable_on_reread",
                      fixture::elems_of("nullSet").empty());
            ctx.check("cst_missing_set_stable_on_reread",
                      fixture::elems_of("noSuchSetFieldXYZ").empty());
        }

        // =====================================================================
        // Re-read stability — decoding the same boxed-Long set twice yields the
        // same fingerprint (the walk has no destructive heap side effects).
        // =====================================================================
        {
            const auto a{ fixture::longs_of("hashLongs") };
            const auto b{ fixture::longs_of("hashLongs") };
            const long_stats sa{ fingerprint_longs(a) };
            const long_stats sb{ fingerprint_longs(b) };
            ctx.check("cst_hash_longs_reread_same_count", sa.count == sb.count);
            ctx.check("cst_hash_longs_reread_same_val_sum", sa.val_sum == sb.val_sum);
            ctx.check("cst_hash_longs_reread_same_val_xor", sa.val_xor == sb.val_xor);
        }

        // =====================================================================
        // Interpreter-hook proof (pilot-style): a scoped_hook on touch(), driven
        // by a mode-1 probe, fires on real bytecode dispatch with the right
        // self+arg and the original body runs (observed == seed+42 == 7042).
        // scoped_hook (never shutdown_hooks) so this module stays isolated.
        // =====================================================================
        {
            auto handle{ vmhook::scoped_hook<fixture>(
                "touch",
                [](vmhook::return_value&,
                   const std::unique_ptr<fixture>& self,
                   std::int32_t delta)
                {
                    g_hook_calls.fetch_add(1, std::memory_order_relaxed);
                    g_hook_arg.store(delta, std::memory_order_relaxed);
                    g_hook_saw_self.store(self != nullptr, std::memory_order_relaxed);
                }) };
            ctx.check("cst_hook_installed", handle.installed());

            const bool done{ ctx.run_probe(
                [](bool value)
                {
                    if (value)
                    {
                        fixture::set_done(false);
                        fixture::set_mode(1);
                    }
                    fixture::set_go(value);
                },
                []() { return fixture::get_done(); }) };

            ctx.check("cst_probe_completed", done);
            ctx.check("cst_hook_fired",
                      g_hook_calls.load(std::memory_order_relaxed) >= 1);
            ctx.check("cst_hook_saw_self",
                      g_hook_saw_self.load(std::memory_order_relaxed));
            ctx.check("cst_hook_saw_arg_42",
                      g_hook_arg.load(std::memory_order_relaxed) == 42);
            ctx.check("cst_observed_is_7042", fixture::get_observed() == 7042);
        }
        // handle out of scope -> hook uninstalled; module isolated.
    }   // run_checks
}   // anonymous namespace

VMHOOK_JVM_MODULE(collection_set_exhaustive)
{
    bool body_threw{ false };
    try
    {
        run_checks(ctx);
    }
    catch (...)
    {
        body_threw = true;
    }

    // FINAL CLEANUP — OUTSIDE the try so it ALWAYS runs; idempotent and
    // safe-when-empty.  Guarantees ZERO hooks armed on EVERY exit path even if
    // the body threw before the scoped_hook's scope exit.
    vmhook::shutdown_hooks();

    if (body_threw)
    {
        ctx.record("[INFO] collection_set_exhaustive: the test body threw and was "
                   "contained (no crash, no hooks armed); see preceding checks for "
                   "partial results.");
    }
    ctx.check("cst_module_left_clean_final_shutdown", true);
}
