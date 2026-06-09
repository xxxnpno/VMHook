// collection_set JVM test module  (feature area: collections)
//
// Exhaustively exercises vmhook::collection::to_vector<wrapper>() /
// field_proxy::value_t::to_vector<element>() over real java.util.Set fields on a
// LIVE JVM, through the exact path a user hits: a static Set field read decoded
// to a collection, then .to_vector<element>().  Covers the two underlying
// element walkers the audit analysed:
//
//   hash_map_walk_keys<E>   vmhook.hpp ~15233  (HashSet / LinkedHashSet:
//                           Node[] bucket array + Node.next chain; field "map")
//   tree_map_walk_keys<E>   vmhook.hpp ~15425  (TreeSet: iterative in-order
//                           red-black walk over TreeMap.root; field "m")
//   collection::to_vector   vmhook.hpp ~14690  (the field-shape cascade that
//                           routes "map"→HashSet path, "m"→TreeSet path)
//
// Coverage matrix (every scenario / flaw the two audit files flagged):
//
//   HashSet  (map → hash_map_walk_keys):
//     * empty (table all-null → 0 elements, no read)
//     * single
//     * many (MANY_N=50 → backing HashMap resized past the default 16 buckets)
//     * BIG (BIG_N=5000 → many buckets + chains; every element distinct, count
//       exact; a cycle/dup walk bug would re-emit a node → duplicate OOP)
//     * HashSet<String> (String element decode through the key walk)
//     * TREEIFIED bin (>8 colliding-hashCode keys → at least one bucket head is
//       a TreeNode; the Node.next chain must still surface EVERY element — the
//       TreeNode-via-Node-super find_field path the audit confirmed but had no
//       runtime coverage for)
//     * legal single NULL element → a nullptr slot (audit: locks the documented
//       "Null Java elements become nullptr" promise for the HashSet key path)
//
//   LinkedHashSet (also map → SAME hash_map_walk_keys):
//     * small + many — CONTENT verified order-independently; insertion order is
//       deliberately NOT required, characterizing the documented [low]
//       "LinkedHashSet insertion order is silently lost" behaviour (vmhook walks
//       bucket order, ignoring the LinkedHashMap before/after overlay).
//
//   TreeSet  (m → tree_map_walk_keys):
//     * empty (root null → 0 elements, no throw)
//     * single / small / many (TREE_MANY_N=200 deep tree) — in-order == SORTED
//       element order, asserted EXACTLY (the only place the live red-black walk
//       is exercised).
//     * TreeSet<String> (sorted lexicographic order, exact).
//
//   Collections.newSetFromMap(new HashMap<>())  [REAL vmhook BUG, characterized]:
//     * Its backing-map field is literally named "m" (collides with TreeSet), so
//       collection::to_vector routes it to tree_map_walk_keys; find_field(map,
//       "root") misses on a HashMap; an EMPTY vector is returned for a NON-empty
//       Set.  This module CHARACTERIZES the actual (buggy) behaviour: asserts the
//       decode is empty/partial (size < Java size) and records an [INFO] flaw
//       note.  It never crashes and never edits vmhook.hpp.  (audit:
//       to_vector_treeset_redblack.md [medium] "TreeSet fast path silently
//       returns empty for Collections.newSetFromMap(...)".)
//
//   Robustness:
//     * a NULL Set field and a MISSING field name → empty vector, never throws.
//
// Verification strategy for unordered sets: the walker visits HashSet /
// LinkedHashSet elements in BUCKET order (not Java insertion order), so per-index
// sequence assertions would be brittle.  Instead each Elem has id i and the
// module aggregates order-independent fingerprints over decoded elements —
// count, null count, idSum, idXor, and OOP-distinctness — cross-checking idSum /
// idXor against values the Java fixture computed the identical way.  Membership
// of specific ids is also checked via a presence set.  TreeSet additionally gets
// a strict sorted-order assertion because its walk order is defined.
//
// HARD RULE honoured: every raw element OOP is wrapped only after to_vector's own
// is_valid_pointer guard; the module's own field reads go through the harness
// wrappers (get_field / static_field / read_java_string), which guard internally.
// Mirrors the collection_map / pilot shape: register_class, a scoped_hook to
// prove an interpreter hook fires through the modular path, run_probe for the
// go/done handshake, and a dense battery of ctx.check() angles.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace
{
    // ── ELEMENT wrapper: vmhook.fixtures.CollSet$Elem. ──────────────────────
    // hash_map_walk_keys / tree_map_walk_keys build make_unique<elem_object>
    // from each decoded key OOP.  Reads BOTH a primitive (id:int) and a
    // reference (tag:String) field, proving each element OOP round-trips fully.
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

    // ── STRING element wrapper: java.lang.String. ───────────────────────────
    // For HashSet<String> / TreeSet<String>, each element OOP is a String; we
    // decode its text directly via read_java_string(get_instance()).
    class string_element : public vmhook::object<string_element>
    {
    public:
        explicit string_element(vmhook::oop_t instance) noexcept
            : vmhook::object<string_element>{ instance }
        {
        }

        auto text() const -> std::string
        {
            return vmhook::read_java_string(get_instance());
        }
    };

    // ── Fixture wrapper: vmhook.fixtures.CollSet. ───────────────────────────
    class coll_set_fixture : public vmhook::object<coll_set_fixture>
    {
    public:
        explicit coll_set_fixture(vmhook::oop_t instance) noexcept
            : vmhook::object<coll_set_fixture>{ instance }
        {
        }

        // handshake + selector
        static auto set_go(bool value) -> void    { static_field("go")->set(value); }
        static auto set_done(bool value) -> void   { static_field("done")->set(value); }
        static auto get_done() -> bool             { return static_field("done")->get(); }
        static auto set_mode(std::int32_t m) -> void { static_field("mode")->set(m); }
        static auto get_observed() -> std::int32_t { return static_field("observed")->get(); }

        // Read a named static Set field and decode it to an Elem vector.  Returns
        // an empty vector when the field is unresolved (the robustness contract).
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

        // Same, decoded to a String vector (HashSet<String> / TreeSet<String>).
        static auto strings_of(const char* field)
            -> std::vector<std::unique_ptr<string_element>>
        {
            const auto proxy{ static_field(field) };
            if (!proxy.has_value())
            {
                return {};
            }
            return proxy->get().to_vector<string_element>();
        }

        // Java-published cross-check values.
        static auto j_size(const char* f) -> std::int32_t { return static_field(f)->get(); }
        static auto j_long(const char* f) -> std::int64_t { return static_field(f)->get(); }
        static auto j_bool(const char* f) -> bool { return static_field(f)->get(); }
    };

    // ── Fixture-mirrored constants (lockstep with CollSet.java). ────────────
    constexpr std::int32_t SMALL_N{ 3 };
    constexpr std::int32_t MANY_N{ 50 };
    constexpr std::int32_t BIG_N{ 5000 };
    constexpr std::int32_t TREEIFY_N{ 64 };
    constexpr std::int32_t TREE_MANY_N{ 200 };
    constexpr std::int32_t NULL_SET_NONNULL{ 3 };
    constexpr std::int32_t SETFROMMAP_N{ 4 };

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
        bool         distinct_oops{ true };  // every non-null element OOP unique
        bool         tags_consistent{ true }; // every non-null element: tag=="e"+id
    };

    auto fingerprint(const std::vector<std::unique_ptr<elem_object>>& v) -> elem_stats
    {
        elem_stats st;
        st.count = static_cast<std::int32_t>(v.size());

        std::unordered_set<const void*> seen_oops;
        seen_oops.reserve(v.size() * 2 + 1);

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
            if (!seen_oops.insert(oop).second)
            {
                st.distinct_oops = false;
            }
        }
        return st;
    }

    // Build a presence set of decoded non-null Elem ids (for membership checks
    // that do not depend on iteration order).
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

    auto code_unit_sum(const std::string& s) -> std::int64_t
    {
        std::int64_t sum{ 0 };
        for (const unsigned char c : s)
        {
            sum += c;
        }
        return sum;
    }

    // Order-independent char-sum + distinctness over a decoded String set.
    struct string_stats
    {
        std::int32_t count{ 0 };
        std::int32_t null_count{ 0 };
        std::int64_t char_sum{ 0 };
        bool         distinct_text{ true };
    };

    auto fingerprint_strings(const std::vector<std::unique_ptr<string_element>>& v)
        -> string_stats
    {
        string_stats st;
        st.count = static_cast<std::int32_t>(v.size());
        std::unordered_set<std::string> seen;
        seen.reserve(v.size() * 2 + 1);
        for (const auto& up : v)
        {
            if (up == nullptr)
            {
                ++st.null_count;
                continue;
            }
            const std::string t{ up->text() };
            st.char_sum += code_unit_sum(t);
            if (!seen.insert(t).second)
            {
                st.distinct_text = false;
            }
        }
        return st;
    }
}

VMHOOK_JVM_MODULE(collection_set)
{
    vmhook::register_class<coll_set_fixture>("vmhook/fixtures/CollSet");
    vmhook::register_class<elem_object>("vmhook/fixtures/CollSet$Elem");
    vmhook::register_class<string_element>("java/lang/String");

    // The fixture's static initializer already built every set (buildAll()).
    // Drive one mode-0 probe first so the build also runs on the Java thread and
    // we read a freshly-populated, deterministic snapshot.
    {
        const bool built{ ctx.run_probe(
            [](bool value)
            {
                if (value)
                {
                    coll_set_fixture::set_done(false);
                    coll_set_fixture::set_mode(0);
                }
                coll_set_fixture::set_go(value);
            },
            []() { return coll_set_fixture::get_done(); }) };
        ctx.check("build_probe_completed", built);
    }

    // =====================================================================
    // HashSet — EMPTY.  table exists but every bucket is null → 0 elements,
    // no read, no throw; Java agrees size()==0.
    // =====================================================================
    {
        const auto v{ coll_set_fixture::elems_of("hashEmpty") };
        ctx.check("hash_empty_size_zero", v.empty());
        ctx.check("hash_empty_java_size_zero",
                  coll_set_fixture::j_size("hashEmptySize") == 0);
    }

    // =====================================================================
    // HashSet — SINGLE element.
    // =====================================================================
    {
        const auto v{ coll_set_fixture::elems_of("hashSingle") };
        const elem_stats st{ fingerprint(v) };
        ctx.check("hash_single_count_is_1", st.count == 1);
        ctx.check("hash_single_count_matches_java",
                  st.count == coll_set_fixture::j_size("hashSingleSize"));
        ctx.check("hash_single_no_null", st.null_count == 0);
        ctx.check("hash_single_id_is_0", st.id_sum == 0);
        ctx.check("hash_single_tag_round_trips", st.tags_consistent);
    }

    // =====================================================================
    // HashSet — MANY (50).  Backing HashMap resized past the default 16 buckets;
    // verify the walker visits ALL buckets/chains (count + full fingerprint +
    // every id 0..49 present + all element OOPs distinct).
    // =====================================================================
    {
        const auto v{ coll_set_fixture::elems_of("hashMany") };
        const elem_stats st{ fingerprint(v) };

        ctx.check("hash_many_count_is_50", st.count == MANY_N);
        ctx.check("hash_many_count_matches_java",
                  st.count == coll_set_fixture::j_size("hashManySize"));
        ctx.check("hash_many_no_null", st.null_count == 0);
        ctx.check("hash_many_id_sum_matches_java",
                  st.id_sum == coll_set_fixture::j_long("hashManyIdSum"));
        ctx.check("hash_many_id_xor_matches_java",
                  st.id_xor == coll_set_fixture::j_long("hashManyIdXor"));
        // Closed form: sum 0..49 == 1225.
        ctx.check("hash_many_id_sum_closed_form",
                  st.id_sum == (static_cast<std::int64_t>(MANY_N) * (MANY_N - 1)) / 2);
        ctx.check("hash_many_all_elements_distinct", st.distinct_oops);
        ctx.check("hash_many_tags_round_trip", st.tags_consistent);

        // Membership: every id 0..49 must appear exactly once (set semantics).
        const auto ids{ id_set(v) };
        bool all_present{ ids.size() == static_cast<std::size_t>(MANY_N) };
        for (std::int32_t i{ 0 }; i < MANY_N; ++i)
        {
            if (ids.find(i) == ids.end()) { all_present = false; }
        }
        ctx.check("hash_many_every_id_present_no_dupes", all_present);
    }

    // =====================================================================
    // HashSet — BIG (5000).  Many buckets + chains; the core "bucket walk is
    // correct at scale" battery: exact count, full fingerprint, ALL element OOPs
    // distinct (a cycle/dup bug re-emits a node → duplicate OOP), no nulls.
    // =====================================================================
    {
        const auto v{ coll_set_fixture::elems_of("hashBig") };
        const elem_stats st{ fingerprint(v) };

        ctx.check("hash_big_count_is_5000", st.count == BIG_N);
        ctx.check("hash_big_count_matches_java",
                  st.count == coll_set_fixture::j_size("hashBigSize"));
        ctx.check("hash_big_no_null", st.null_count == 0);
        ctx.check("hash_big_id_sum_matches_java",
                  st.id_sum == coll_set_fixture::j_long("hashBigIdSum"));
        ctx.check("hash_big_id_xor_matches_java",
                  st.id_xor == coll_set_fixture::j_long("hashBigIdXor"));
        ctx.check("hash_big_id_sum_closed_form",
                  st.id_sum == (static_cast<std::int64_t>(BIG_N) * (BIG_N - 1)) / 2);
        ctx.check("hash_big_all_elements_distinct_no_cycle", st.distinct_oops);

        const auto ids{ id_set(v) };
        ctx.check("hash_big_membership_complete",
                  ids.size() == static_cast<std::size_t>(BIG_N));
    }

    // =====================================================================
    // HashSet<String> — MANY.  Element OOPs are java.lang.String; decode each
    // via read_java_string and verify content (order-independent char sum +
    // distinctness + membership of "s0".."s(MANY_N-1)").
    // =====================================================================
    {
        const auto v{ coll_set_fixture::strings_of("hashStrings") };
        const string_stats st{ fingerprint_strings(v) };

        ctx.check("hash_strings_count_matches", st.count == MANY_N);
        ctx.check("hash_strings_count_matches_java",
                  st.count == coll_set_fixture::j_size("hashStringsSize"));
        ctx.check("hash_strings_no_null", st.null_count == 0);
        ctx.check("hash_strings_char_sum_matches_java",
                  st.char_sum == coll_set_fixture::j_long("hashStringsCharSum"));
        ctx.check("hash_strings_all_distinct", st.distinct_text);

        // Membership of a few representative keys (order-independent).
        std::unordered_set<std::string> texts;
        for (const auto& up : v) { if (up) { texts.insert(up->text()); } }
        ctx.check("hash_strings_contains_s0", texts.count("s0") == 1);
        ctx.check("hash_strings_contains_s49",
                  texts.count("s" + std::to_string(MANY_N - 1)) == 1);
    }

    // =====================================================================
    // HashSet — TREEIFIED bin (>8 colliding-hashCode String keys).  After
    // treeification a bucket head is a TreeNode, but the Node.next chain stays
    // populated, so the key walk must still return EVERY element.  Verify full
    // count + char sum + distinctness; record whether a bin actually treeified
    // (Java confirms via reflection).
    // =====================================================================
    {
        const auto v{ coll_set_fixture::strings_of("hashTreeified") };
        const string_stats st{ fingerprint_strings(v) };

        ctx.check("hash_treeified_count_matches_n", st.count == TREEIFY_N);
        ctx.check("hash_treeified_count_matches_java",
                  st.count == coll_set_fixture::j_size("hashTreeifiedSize"));
        ctx.check("hash_treeified_no_null", st.null_count == 0);
        ctx.check("hash_treeified_char_sum_matches_java",
                  st.char_sum == coll_set_fixture::j_long("hashTreeifiedCharSum"));
        ctx.check("hash_treeified_all_distinct", st.distinct_text);

        const bool treeified{ coll_set_fixture::j_bool("treeifiedHasTreeBin") };
        ctx.record(std::string{ "[INFO] hashTreeified actually treeified a bin: " }
                   + (treeified ? "yes" : "no"));
        // If Java confirmed a TreeNode bin, the walk-through-TreeNode-via-Node-
        // super path was exercised; the count check above proves it returned
        // everything.
        if (treeified)
        {
            ctx.check("hash_treeified_treenode_path_returned_all",
                      st.count == TREEIFY_N);
        }
    }

    // =====================================================================
    // HashSet — legal single NULL element + NULL_SET_NONNULL reals.  vmhook
    // surfaces the null element as a nullptr slot; the real elements decode
    // intact.  (audit: locks the "Null Java elements become nullptr" promise for
    // the HashSet key path — a HashSet CAN legally hold one null.)
    // =====================================================================
    {
        const auto v{ coll_set_fixture::elems_of("hashWithNull") };
        const elem_stats st{ fingerprint(v) };

        ctx.check("hash_withnull_count_is_4",
                  st.count == NULL_SET_NONNULL + 1);
        ctx.check("hash_withnull_count_matches_java",
                  st.count == coll_set_fixture::j_size("hashWithNullSize"));
        ctx.check("hash_withnull_exactly_one_null", st.null_count == 1);
        ctx.check("hash_withnull_nonnull_count",
                  (st.count - st.null_count) == NULL_SET_NONNULL);
        // Real elements are ids 100..102; their idSum is fixed and order-free.
        std::int64_t expect_id_sum{ 0 };
        for (std::int32_t i{ 0 }; i < NULL_SET_NONNULL; ++i)
        {
            expect_id_sum += (100 + i);
        }
        ctx.check("hash_withnull_nonnull_id_sum_ok", st.id_sum == expect_id_sum);
        ctx.check("hash_withnull_nonnull_distinct", st.distinct_oops);
        ctx.check("hash_withnull_nonnull_tags_ok", st.tags_consistent);

        const auto ids{ id_set(v) };
        ctx.check("hash_withnull_contains_100", ids.count(100) == 1);
        ctx.check("hash_withnull_contains_102", ids.count(102) == 1);
    }

    // =====================================================================
    // LinkedHashSet — SMALL + MANY.  Reuses HashSet's "map"→hash_map_walk_keys
    // fast path, so the SAME walker runs.  Verify CONTENT order-independently.
    // (Audit [low]: vmhook walks BUCKET order, NOT LinkedHashSet insertion order
    //  — we deliberately do NOT assert insertion order, and record the quirk.)
    // =====================================================================
    {
        const auto v{ coll_set_fixture::elems_of("linkedSmall") };
        const elem_stats st{ fingerprint(v) };
        ctx.check("linked_small_count_is_3", st.count == SMALL_N);
        ctx.check("linked_small_count_matches_java",
                  st.count == coll_set_fixture::j_size("linkedSmallSize"));
        ctx.check("linked_small_no_null", st.null_count == 0);
        ctx.check("linked_small_id_sum_is_3", st.id_sum == (0 + 1 + 2));
        ctx.check("linked_small_distinct", st.distinct_oops);

        // Characterize the insertion-order-lost behaviour: content is correct,
        // but the decode order is bucket order, not [0,1,2] insertion order.
        std::vector<std::int32_t> order;
        order.reserve(v.size());
        for (const auto& up : v) { if (up) { order.push_back(up->id()); } }
        const bool is_insertion_order{
            order.size() == 3 && order[0] == 0 && order[1] == 1 && order[2] == 2 };
        ctx.record(std::string{ "[INFO] LinkedHashSet decode order == Java "
                                "insertion order [0,1,2]: " }
                   + (is_insertion_order ? "yes (coincidental bucket order)"
                                         : "no — vmhook walks bucket order, "
                                           "NOT the LinkedHashMap insertion overlay "
                                           "(documented [low] behaviour)"));
    }
    {
        const auto v{ coll_set_fixture::elems_of("linkedMany") };
        const elem_stats st{ fingerprint(v) };
        ctx.check("linked_many_count_is_50", st.count == MANY_N);
        ctx.check("linked_many_count_matches_java",
                  st.count == coll_set_fixture::j_size("linkedManySize"));
        ctx.check("linked_many_no_null", st.null_count == 0);
        ctx.check("linked_many_id_sum_matches_java",
                  st.id_sum == coll_set_fixture::j_long("linkedManyIdSum"));
        ctx.check("linked_many_id_xor_matches_java",
                  st.id_xor == coll_set_fixture::j_long("linkedManyIdXor"));
        ctx.check("linked_many_all_distinct", st.distinct_oops);
        const auto ids{ id_set(v) };
        ctx.check("linked_many_membership_complete",
                  ids.size() == static_cast<std::size_t>(MANY_N));
    }

    // =====================================================================
    // TreeSet — EMPTY.  root is null → 0 elements, no throw.
    // =====================================================================
    {
        const auto v{ coll_set_fixture::elems_of("treeEmpty") };
        ctx.check("tree_empty_size_zero", v.empty());
        ctx.check("tree_empty_java_size_zero",
                  coll_set_fixture::j_size("treeEmptySize") == 0);
    }

    // =====================================================================
    // TreeSet — SINGLE.
    // =====================================================================
    {
        const auto v{ coll_set_fixture::elems_of("treeSingle") };
        const elem_stats st{ fingerprint(v) };
        ctx.check("tree_single_count_is_1", st.count == 1);
        ctx.check("tree_single_count_matches_java",
                  st.count == coll_set_fixture::j_size("treeSingleSize"));
        ctx.check("tree_single_no_null", st.null_count == 0);
        ctx.check("tree_single_id_is_0", st.id_sum == 0);
    }

    // =====================================================================
    // TreeSet — SMALL (3).  The in-order red-black walk yields SORTED element
    // order — verify count, content, AND strict ascending id order (defined!).
    // =====================================================================
    {
        const auto v{ coll_set_fixture::elems_of("treeSmall") };
        const elem_stats st{ fingerprint(v) };

        ctx.check("tree_small_count_is_3", st.count == SMALL_N);
        ctx.check("tree_small_count_matches_java",
                  st.count == coll_set_fixture::j_size("treeSmallSize"));
        ctx.check("tree_small_no_null", st.null_count == 0);
        ctx.check("tree_small_tags_round_trip", st.tags_consistent);

        // Collect decoded ids IN WALK ORDER; TreeSet<Elem> orders by Elem.id, so
        // the in-order walk MUST produce [0,1,2] exactly.
        std::vector<std::int32_t> order;
        order.reserve(v.size());
        bool pairs_ok{ true };
        for (const auto& up : v)
        {
            if (up == nullptr) { pairs_ok = false; continue; }
            order.push_back(up->id());
        }
        ctx.check("tree_small_all_nonnull", pairs_ok);
        ctx.check("tree_small_in_sorted_order",
                  order.size() == 3 && order[0] == 0 && order[1] == 1 && order[2] == 2);
        ctx.check("tree_small_strictly_ascending",
                  std::is_sorted(order.begin(), order.end()));
        ctx.check("tree_small_first_is_0", !order.empty() && order.front() == 0);
        ctx.check("tree_small_last_is_2", !order.empty() && order.back() == 2);
    }

    // =====================================================================
    // TreeSet — MANY (200).  Deep red-black tree; the iterative stack walk must
    // visit all nodes in SORTED order without blowing the stack.
    // =====================================================================
    {
        const auto v{ coll_set_fixture::elems_of("treeMany") };
        const elem_stats st{ fingerprint(v) };

        ctx.check("tree_many_count_is_200", st.count == TREE_MANY_N);
        ctx.check("tree_many_count_matches_java",
                  st.count == coll_set_fixture::j_size("treeManySize"));
        ctx.check("tree_many_no_null", st.null_count == 0);
        ctx.check("tree_many_id_sum_matches_java",
                  st.id_sum == coll_set_fixture::j_long("treeManyIdSum"));
        ctx.check("tree_many_id_sum_closed_form",
                  st.id_sum == (static_cast<std::int64_t>(TREE_MANY_N) * (TREE_MANY_N - 1)) / 2);
        ctx.check("tree_many_all_distinct", st.distinct_oops);

        // Strict ascending id order across all 200 elements + exact endpoints.
        std::vector<std::int32_t> order;
        order.reserve(v.size());
        for (const auto& up : v) { order.push_back(up ? up->id() : -1); }
        ctx.check("tree_many_in_ascending_id_order",
                  std::is_sorted(order.begin(), order.end()));
        ctx.check("tree_many_first_is_0", !order.empty() && order.front() == 0);
        ctx.check("tree_many_last_is_199",
                  !order.empty() && order.back() == TREE_MANY_N - 1);
        // Full identity: the in-order walk must be exactly [0,1,...,199].
        bool exact_sequence{ order.size() == static_cast<std::size_t>(TREE_MANY_N) };
        for (std::size_t k{ 0 }; exact_sequence && k < order.size(); ++k)
        {
            if (order[k] != static_cast<std::int32_t>(k)) { exact_sequence = false; }
        }
        ctx.check("tree_many_exact_in_order_sequence", exact_sequence);
    }

    // =====================================================================
    // TreeSet<String> — sorted lexicographic order, exact.
    // =====================================================================
    {
        const auto v{ coll_set_fixture::strings_of("treeStrings") };
        const string_stats st{ fingerprint_strings(v) };
        ctx.check("tree_strings_count_is_3", st.count == 3);
        ctx.check("tree_strings_count_matches_java",
                  st.count == coll_set_fixture::j_size("treeStringsSize"));
        ctx.check("tree_strings_no_null", st.null_count == 0);

        std::vector<std::string> order;
        order.reserve(v.size());
        for (const auto& up : v) { order.push_back(up ? up->text() : std::string{}); }
        ctx.check("tree_strings_sorted",
                  std::is_sorted(order.begin(), order.end()));
        ctx.check("tree_strings_first_is_apple",
                  !order.empty() && order.front() == "apple");
        ctx.check("tree_strings_last_is_cherry",
                  !order.empty() && order.back() == "cherry");
    }

    // =====================================================================
    // Collections.newSetFromMap(new HashMap<>())  — REAL vmhook BUG, characterized.
    //
    // SetFromMap's backing-map field is literally named "m" (same probe as
    // TreeSet), so collection::to_vector routes it to tree_map_walk_keys, which
    // does find_field(mapKlass,"root") on a HashMap (no "root") and returns an
    // EMPTY vector for a NON-empty Set.  We pin the ACTUAL (buggy) behaviour and
    // record the flaw; the call must NOT crash.
    // (audit: to_vector_treeset_redblack.md [medium].)
    // =====================================================================
    {
        const std::int32_t java_size{ coll_set_fixture::j_size("setFromHashMapSize") };
        ctx.check("setfrommap_java_size_is_4", java_size == SETFROMMAP_N);

        const auto v{ coll_set_fixture::elems_of("setFromHashMap") };
        const elem_stats st{ fingerprint(v) };

        // The bug manifests as a SHORT decode: vmhook returns fewer elements than
        // the Set actually holds (in practice 0, because HashMap has no "root").
        const bool short_decode{ st.count < java_size };
        ctx.record(std::string{ "[INFO] vmhook BUG (to_vector_treeset_redblack.md "
                                "[medium]): Collections.newSetFromMap(HashMap) has "
                                "field 'm' so to_vector takes the TreeSet path; "
                                "HashMap has no 'root' field, so the decode returns " }
                   + std::to_string(st.count) + " of " + std::to_string(java_size)
                   + " elements (expected behaviour: route to the generic iterator "
                     "path and return all " + std::to_string(java_size) + ").");

        // Characterize, do not fix: assert the ACTUAL behaviour (short/empty).
        ctx.check("setfrommap_decode_is_short_BUG", short_decode);
        ctx.check("setfrommap_decode_did_not_crash", true);
        // In practice the count is exactly 0 (no 'root' → walker returns
        // immediately).  Pin that too, but tolerate a future partial fix by also
        // accepting any count strictly below the Java size above.
        ctx.record(std::string{ "[INFO] setFromHashMap decoded element count == " }
                   + std::to_string(st.count));
        ctx.check("setfrommap_decode_count_is_zero_today", st.count == 0);
    }

    // =====================================================================
    // ROBUSTNESS — to_vector must NEVER throw and must return empty on:
    //   (a) a NULL Set field,
    //   (b) a MISSING field name.
    // =====================================================================
    {
        // (a) Declared-but-null Set field: value_t's null-oop guard fires.
        const auto v_null{ coll_set_fixture::elems_of("nullSet") };
        ctx.check("null_set_field_returns_empty", v_null.empty());

        // (b) Missing field name: static_field() yields nullopt → elems_of
        //     short-circuits to empty (proves the helper + contract).
        const auto v_missing{ coll_set_fixture::elems_of("noSuchSetFieldXYZ") };
        ctx.check("missing_set_field_returns_empty", v_missing.empty());

        // Re-reading the same null/missing fields twice must remain stable.
        ctx.check("null_set_field_stable_on_reread",
                  coll_set_fixture::elems_of("nullSet").empty());
        ctx.check("missing_set_field_stable_on_reread",
                  coll_set_fixture::elems_of("noSuchSetFieldXYZ").empty());
    }

    // =====================================================================
    // Re-read stability: decoding the same populated set twice yields the same
    // fingerprint (the walk has no destructive side effects on the heap).
    // =====================================================================
    {
        const auto a{ coll_set_fixture::elems_of("hashMany") };
        const auto b{ coll_set_fixture::elems_of("hashMany") };
        const elem_stats sa{ fingerprint(a) };
        const elem_stats sb{ fingerprint(b) };
        ctx.check("hash_many_reread_same_count", sa.count == sb.count);
        ctx.check("hash_many_reread_same_id_sum", sa.id_sum == sb.id_sum);
        ctx.check("hash_many_reread_same_id_xor", sa.id_xor == sb.id_xor);
    }

    // =====================================================================
    // Interpreter-hook proof (pilot-style): install a scoped_hook on touch(),
    // drive mode 1, confirm the detour fires on real bytecode dispatch with the
    // right self+arg and the original body runs (observed == seed+42 == 6042).
    // scoped_hook (never shutdown_hooks) so this module stays isolated.
    // =====================================================================
    {
        auto handle{ vmhook::scoped_hook<coll_set_fixture>(
            "touch",
            [](vmhook::return_value&,
               const std::unique_ptr<coll_set_fixture>& self,
               std::int32_t delta)
            {
                g_hook_calls.fetch_add(1, std::memory_order_relaxed);
                g_hook_arg.store(delta, std::memory_order_relaxed);
                g_hook_saw_self.store(self != nullptr, std::memory_order_relaxed);
            }) };
        ctx.check("collset_hook_installed", handle.installed());

        const bool done{ ctx.run_probe(
            [](bool value)
            {
                if (value)
                {
                    coll_set_fixture::set_done(false);
                    coll_set_fixture::set_mode(1);
                }
                coll_set_fixture::set_go(value);
            },
            []() { return coll_set_fixture::get_done(); }) };

        ctx.check("collset_probe_completed", done);
        ctx.check("collset_hook_fired",
                  g_hook_calls.load(std::memory_order_relaxed) >= 1);
        ctx.check("collset_hook_saw_self",
                  g_hook_saw_self.load(std::memory_order_relaxed));
        ctx.check("collset_hook_saw_arg_42",
                  g_hook_arg.load(std::memory_order_relaxed) == 42);
        ctx.check("collset_observed_is_6042",
                  coll_set_fixture::get_observed() == 6042);
    }
}
