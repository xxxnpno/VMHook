// collection_map JVM test module  (feature area: collections)
//
// Exhaustively exercises field_proxy::value_t::to_entries<K,V>() and the
// vmhook::map::to_entries machinery it delegates to, on a LIVE JVM:
//
//   value_t::to_entries<K,V>()   vmhook.hpp ~15111  (null/invalid-oop guard + delegate)
//   map::to_entries<K,V>()       vmhook.hpp ~14564  (table fast path / root fast path)
//   hash_map_walk_entries<K,V>   vmhook.hpp ~14698  (Node[] bucket + next-chain walk)
//   tree_map_walk_entries<K,V>   vmhook.hpp ~14863  (iterative red-black in-order walk)
//
// Coverage matrix (every angle the audit flagged, all on real heap objects):
//   * HashMap<String,Box>:  empty / small / MANY(1000, multiple resizes) /
//                           one-null-key / one-null-value / empty-string K+V /
//                           a deliberately TREEIFIED bin (>8 colliding keys).
//   * LinkedHashMap:        small + MANY — proves the "table" fast path is taken
//                           and (audit finding) iteration is BUCKET order, not
//                           insertion order; we verify CONTENT (order-independent)
//                           and additionally pin the order quirk by NOT requiring
//                           insertion order.
//   * TreeMap<String,Box>:  empty / small / MANY — the red-black in-order walk
//                           yields SORTED key order, which we verify exactly.
//   * Robustness:           a NULL Map field and a MISSING field name both return
//                           an empty vector and NEVER throw (the contract).
//                           A non-Map String field returns empty (no table/root).
//
// Verification strategy for unordered maps: the walker visits entries in bucket
// order (not Java insertion order), so per-pair sequence assertions would be
// brittle.  Instead every key is "k"+i and every value is Box(i,"v"+i); the
// module aggregates order-independent checksums over the decoded pairs —
//   keyCharSum = sum of key UTF-16 code units, idSum = sum of value.id,
//   idXor      = xor of value.id — and cross-checks them against values the Java
// fixture computed the identical way.  TreeMap additionally gets a strict
// sorted-order assertion because its walk order is defined.
//
// Mirrors the pilot/hook_basic shape: register_class, a scoped_hook to satisfy
// the interpreter-hook-on-dispatch proof, run_probe for the go/done handshake,
// and a dense battery of ctx.check() angles.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace
{
    // ── KEY wrapper: java.lang.String. ──────────────────────────────────────
    // hash_map_walk_entries / tree_map_walk_entries build make_unique<string_key>
    // from the raw key OOP, which for these maps is a java.lang.String instance.
    // We decode our own String contents through read_java_string(get_instance()).
    class string_key : public vmhook::object<string_key>
    {
    public:
        explicit string_key(vmhook::oop_t instance) noexcept
            : vmhook::object<string_key>{ instance }
        {
        }

        // Returns the decoded Java String text, or "" for an empty/unreadable
        // String (read_java_string rejects length<=0, so "" decodes to "").
        auto text() const -> std::string
        {
            return vmhook::read_java_string(get_instance());
        }
    };

    // ── VALUE wrapper: vmhook.fixtures.CollMap$Box. ─────────────────────────
    // Reads BOTH a primitive (id:int) and a reference (name:String) field from
    // the decoded value OOP, proving the value side round-trips fully.
    class box_value : public vmhook::object<box_value>
    {
    public:
        explicit box_value(vmhook::oop_t instance) noexcept
            : vmhook::object<box_value>{ instance }
        {
        }

        auto id() const -> std::int32_t { return static_cast<std::int32_t>(get_field("id")->get()); }

        auto name() const -> std::string { return get_field("name")->get(); }
    };

    // ── Fixture wrapper: vmhook.fixtures.CollMap. ───────────────────────────
    class coll_map_fixture : public vmhook::object<coll_map_fixture>
    {
    public:
        explicit coll_map_fixture(vmhook::oop_t instance) noexcept
            : vmhook::object<coll_map_fixture>{ instance }
        {
        }

        // handshake + selector
        static auto set_go(bool value) -> void   { static_field("go")->set(value); }
        static auto set_done(bool value) -> void  { static_field("done")->set(value); }
        static auto get_done() -> bool            { return static_field("done")->get(); }
        static auto set_mode(std::int32_t m) -> void { static_field("mode")->set(m); }
        static auto get_observed() -> std::int32_t { return static_field("observed")->get(); }

        // Read a named static Map field and decode it to entries.  Returns an
        // empty vector when the field is unresolved (the robustness contract).
        static auto entries_of(const char* field)
            -> std::vector<std::pair<std::unique_ptr<string_key>, std::unique_ptr<box_value>>>
        {
            const auto proxy{ static_field(field) };
            if (!proxy.has_value())
            {
                return {};
            }
            return proxy->get().to_entries<string_key, box_value>();
        }

        // Java-published cross-check values.
        static auto j_size(const char* f) -> std::int32_t { return static_field(f)->get(); }
        static auto j_long(const char* f) -> std::int64_t { return static_field(f)->get(); }
        static auto j_string(const char* f) -> std::string { return static_field(f)->get(); }
        static auto j_bool(const char* f) -> bool { return static_field(f)->get(); }
    };

    // ── Fixture-mirrored constants (lockstep with CollMap.java). ────────────
    constexpr std::int32_t SMALL_N{ 3 };
    constexpr std::int32_t MANY_N{ 1000 };
    constexpr std::int32_t NULL_KEY_N{ 3 };
    constexpr std::int32_t TREEIFY_N{ 12 };

    // ── Hook observation (pilot-style proof). ───────────────────────────────
    std::atomic<int>          g_hook_calls{ 0 };
    std::atomic<std::int32_t> g_hook_arg{ -1 };
    std::atomic<bool>         g_hook_saw_self{ false };

    // Order-independent fingerprint of a decoded entry set.
    struct entry_stats
    {
        std::int32_t count{ 0 };
        std::int32_t null_keys{ 0 };
        std::int32_t null_values{ 0 };
        std::int64_t key_char_sum{ 0 };   // sum of UTF-16 code units across keys
        std::int64_t id_sum{ 0 };         // sum of value.id across entries
        std::int64_t id_xor{ 0 };         // xor of value.id across entries
        bool         name_id_consistent{ true };  // every non-null value: name=="v"+id (when applicable)
    };

    auto code_unit_sum(const std::string& s) -> std::int64_t
    {
        // The keys are pure ASCII ("k123", "a", "Aa"/"BB" blocks, ""), so each
        // byte is exactly one UTF-16 code unit and char==code unit.
        std::int64_t sum{ 0 };
        for (const unsigned char c : s)
        {
            sum += c;
        }
        return sum;
    }

    template<typename entries_t>
    auto fingerprint(const entries_t& entries) -> entry_stats
    {
        entry_stats st;
        st.count = static_cast<std::int32_t>(entries.size());
        for (const auto& kv : entries)
        {
            if (kv.first == nullptr)
            {
                ++st.null_keys;
            }
            else
            {
                st.key_char_sum += code_unit_sum(kv.first->text());
            }
            if (kv.second == nullptr)
            {
                ++st.null_values;
            }
            else
            {
                const std::int32_t id{ kv.second->id() };
                st.id_sum += id;
                st.id_xor ^= id;
            }
        }
        return st;
    }
}

VMHOOK_JVM_MODULE(collection_map)
{
    vmhook::register_class<coll_map_fixture>("vmhook/fixtures/CollMap");
    vmhook::register_class<box_value>("vmhook/fixtures/CollMap$Box");
    vmhook::register_class<string_key>("java/lang/String");

    // The fixture's static initializer already built every map (buildAll()).
    // Drive one mode-0 probe first so the build also runs on the Java thread and
    // we read a freshly-populated, deterministic snapshot.
    {
        const bool built{ ctx.run_probe(
            [](bool value)
            {
                if (value)
                {
                    coll_map_fixture::set_done(false);
                    coll_map_fixture::set_mode(0);
                }
                coll_map_fixture::set_go(value);
            },
            []() { return coll_map_fixture::get_done(); }) };
        ctx.check("build_probe_completed", built);
    }

    // =====================================================================
    // HashMap — EMPTY.  table exists but every bucket is null → 0 entries,
    // no throw, and Java agrees size()==0.
    // =====================================================================
    {
        const auto e{ coll_map_fixture::entries_of("hashEmpty") };
        ctx.check("hash_empty_size_zero", e.empty());
        ctx.check("hash_empty_java_size_zero",
                  coll_map_fixture::j_size("hashEmptySize") == 0);
    }

    // =====================================================================
    // HashMap — SMALL (3 entries).  Verify count, no nulls, and the exact
    // content fingerprint matches what Java computed.
    // =====================================================================
    {
        const auto e{ coll_map_fixture::entries_of("hashSmall") };
        const entry_stats st{ fingerprint(e) };

        ctx.check("hash_small_count_is_3", st.count == SMALL_N);
        ctx.check("hash_small_count_matches_java_size",
                  st.count == coll_map_fixture::j_size("hashSmallSize"));
        ctx.check("hash_small_no_null_keys", st.null_keys == 0);
        ctx.check("hash_small_no_null_values", st.null_values == 0);
        ctx.check("hash_small_key_char_sum_matches_java",
                  st.key_char_sum == coll_map_fixture::j_long("hashSmallKeyCharSum"));
        ctx.check("hash_small_id_sum_matches_java",
                  st.id_sum == coll_map_fixture::j_long("hashSmallIdSum"));
        ctx.check("hash_small_id_xor_matches_java",
                  st.id_xor == coll_map_fixture::j_long("hashSmallIdXor"));
        // id_sum of 0+1+2 == 3 independently (sanity on the aggregate itself).
        ctx.check("hash_small_id_sum_is_3", st.id_sum == (0 + 1 + 2));

        // Per-pair deep check: for EVERY entry, value.name must equal "v"+id and
        // key must equal "k"+id (the entry is internally consistent regardless of
        // visitation order).
        bool all_pairs_consistent{ true };
        bool saw_k0{ false }, saw_k1{ false }, saw_k2{ false };
        for (const auto& kv : e)
        {
            if (kv.first == nullptr || kv.second == nullptr)
            {
                all_pairs_consistent = false;
                continue;
            }
            const std::string key{ kv.first->text() };
            const std::int32_t id{ kv.second->id() };
            const std::string name{ kv.second->name() };
            if (key != ("k" + std::to_string(id))) { all_pairs_consistent = false; }
            if (name != ("v" + std::to_string(id))) { all_pairs_consistent = false; }
            if (key == "k0") { saw_k0 = true; }
            if (key == "k1") { saw_k1 = true; }
            if (key == "k2") { saw_k2 = true; }
        }
        ctx.check("hash_small_every_pair_k_id_v_consistent", all_pairs_consistent);
        ctx.check("hash_small_contains_k0", saw_k0);
        ctx.check("hash_small_contains_k1", saw_k1);
        ctx.check("hash_small_contains_k2", saw_k2);
    }

    // =====================================================================
    // HashMap — MANY (1000 entries).  Forces multiple table resizes; verify
    // the walker visits ALL buckets and every chain (count + full fingerprint).
    // =====================================================================
    {
        const auto e{ coll_map_fixture::entries_of("hashMany") };
        const entry_stats st{ fingerprint(e) };

        ctx.check("hash_many_count_is_1000", st.count == MANY_N);
        ctx.check("hash_many_count_matches_java_size",
                  st.count == coll_map_fixture::j_size("hashManySize"));
        ctx.check("hash_many_no_null_keys", st.null_keys == 0);
        ctx.check("hash_many_no_null_values", st.null_values == 0);
        ctx.check("hash_many_key_char_sum_matches_java",
                  st.key_char_sum == coll_map_fixture::j_long("hashManyKeyCharSum"));
        ctx.check("hash_many_id_sum_matches_java",
                  st.id_sum == coll_map_fixture::j_long("hashManyIdSum"));
        ctx.check("hash_many_id_xor_matches_java",
                  st.id_xor == coll_map_fixture::j_long("hashManyIdXor"));
        // Closed-form: sum 0..999 == 499500.
        ctx.check("hash_many_id_sum_closed_form",
                  st.id_sum == (static_cast<std::int64_t>(MANY_N) * (MANY_N - 1)) / 2);
    }

    // =====================================================================
    // HashMap — ONE NULL KEY (legal single null key in bucket 0).  The walker
    // must surface it as a nullptr key with its value intact, and still return
    // every other entry.
    // =====================================================================
    {
        const auto e{ coll_map_fixture::entries_of("hashNullKey") };
        const entry_stats st{ fingerprint(e) };

        ctx.check("hash_nullkey_count_is_3", st.count == NULL_KEY_N);
        ctx.check("hash_nullkey_count_matches_java_size",
                  st.count == coll_map_fixture::j_size("hashNullKeySize"));
        ctx.check("hash_nullkey_exactly_one_null_key", st.null_keys == 1);
        ctx.check("hash_nullkey_no_null_values", st.null_values == 0);

        // The null-key entry's VALUE must still decode (Box(-1,"nullkey")), and
        // the two non-null keys "a","b" must be present with the right values.
        bool null_key_value_ok{ false };
        bool saw_a{ false }, saw_b{ false };
        for (const auto& kv : e)
        {
            if (kv.first == nullptr)
            {
                null_key_value_ok = (kv.second != nullptr
                                     && kv.second->id() == -1
                                     && kv.second->name() == "nullkey");
                continue;
            }
            const std::string key{ kv.first->text() };
            if (key == "a" && kv.second && kv.second->id() == 1 && kv.second->name() == "va") { saw_a = true; }
            if (key == "b" && kv.second && kv.second->id() == 2 && kv.second->name() == "vb") { saw_b = true; }
        }
        ctx.check("hash_nullkey_null_entry_value_decoded", null_key_value_ok);
        ctx.check("hash_nullkey_nonnull_a_present", saw_a);
        ctx.check("hash_nullkey_nonnull_b_present", saw_b);
    }

    // =====================================================================
    // HashMap — ONE NULL VALUE (non-null key maps to null).  The walker must
    // surface a nullptr value while keeping the key, and return the sibling too.
    // =====================================================================
    {
        const auto e{ coll_map_fixture::entries_of("hashNullValue") };
        const entry_stats st{ fingerprint(e) };

        ctx.check("hash_nullvalue_count_is_2", st.count == 2);
        ctx.check("hash_nullvalue_count_matches_java_size",
                  st.count == coll_map_fixture::j_size("hashNullValueSize"));
        ctx.check("hash_nullvalue_no_null_keys", st.null_keys == 0);
        ctx.check("hash_nullvalue_exactly_one_null_value", st.null_values == 1);

        bool null_value_key_ok{ false };
        bool sibling_ok{ false };
        for (const auto& kv : e)
        {
            if (kv.second == nullptr)
            {
                null_value_key_ok = (kv.first != nullptr && kv.first->text() == "present");
                continue;
            }
            if (kv.first && kv.first->text() == "alsohere"
                && kv.second->id() == 9 && kv.second->name() == "v9")
            {
                sibling_ok = true;
            }
        }
        ctx.check("hash_nullvalue_null_entry_key_is_present", null_value_key_ok);
        ctx.check("hash_nullvalue_sibling_decoded", sibling_ok);
    }

    // =====================================================================
    // HashMap — EMPTY STRING key AND empty value-name.  read_java_string maps
    // length<=0 → "", so the decoded key text and value.name are both "".
    // =====================================================================
    {
        const auto e{ coll_map_fixture::entries_of("hashEmptyStr") };
        ctx.check("hash_emptystr_count_is_1", e.size() == 1);
        ctx.check("hash_emptystr_java_size_1",
                  coll_map_fixture::j_size("hashEmptyStrSize") == 1);
        if (e.size() == 1)
        {
            const auto& kv{ e.front() };
            // The key OOP is a real (empty) java.lang.String, so the key wrapper
            // is NON-null even though its decoded text is "".
            ctx.check("hash_emptystr_key_wrapper_nonnull", kv.first != nullptr);
            ctx.check("hash_emptystr_key_text_empty",
                      kv.first != nullptr && kv.first->text().empty());
            ctx.check("hash_emptystr_value_nonnull", kv.second != nullptr);
            ctx.check("hash_emptystr_value_id_zero",
                      kv.second != nullptr && kv.second->id() == 0);
            ctx.check("hash_emptystr_value_name_empty",
                      kv.second != nullptr && kv.second->name().empty());
        }
    }

    // =====================================================================
    // HashMap — TREEIFIED bin (>8 keys colliding into one bucket).  Both Node
    // and TreeNode expose key/value/next, so the next-chain walk must still
    // return every entry.  Verify full count + fingerprint; record whether the
    // bin actually treeified (Java confirms via reflection).
    // =====================================================================
    {
        const auto e{ coll_map_fixture::entries_of("hashTreeified") };
        const entry_stats st{ fingerprint(e) };

        ctx.check("hash_treeified_count_is_12", st.count == TREEIFY_N);
        ctx.check("hash_treeified_count_matches_java_size",
                  st.count == coll_map_fixture::j_size("hashTreeifiedSize"));
        ctx.check("hash_treeified_no_null_keys", st.null_keys == 0);
        ctx.check("hash_treeified_no_null_values", st.null_values == 0);
        // Values are Box(1000+i, "t"+i); id_sum = sum 1000..1011.
        std::int64_t expect_id_sum{ 0 };
        std::int64_t expect_id_xor{ 0 };
        for (std::int32_t i{ 0 }; i < TREEIFY_N; ++i)
        {
            expect_id_sum += (1000 + i);
            expect_id_xor ^= (1000 + i);
        }
        ctx.check("hash_treeified_id_sum_ok", st.id_sum == expect_id_sum);
        ctx.check("hash_treeified_id_xor_ok", st.id_xor == expect_id_xor);

        const bool treeified{ coll_map_fixture::j_bool("treeifiedHasTreeBin") };
        ctx.record(std::string{ "[INFO] hashTreeified actually treeified a bin: " }
                   + (treeified ? "yes" : "no"));
        // If Java confirmed a TreeNode bin, the walk-through-TreeNode path was
        // exercised; the count check above already proves it returned everything.
        if (treeified)
        {
            ctx.check("hash_treeified_treenode_path_returned_all", st.count == TREEIFY_N);
        }
    }

    // =====================================================================
    // LinkedHashMap — SMALL + MANY.  Reuses HashMap.table, so the SAME fast
    // path is taken.  Verify CONTENT via the order-independent fingerprint.
    // (Audit note: vmhook walks BUCKET order, not LinkedHashMap insertion
    //  order; we deliberately do NOT assert insertion order here.)
    // =====================================================================
    {
        const auto e{ coll_map_fixture::entries_of("linkedSmall") };
        const entry_stats st{ fingerprint(e) };
        ctx.check("linked_small_count_is_3", st.count == SMALL_N);
        ctx.check("linked_small_count_matches_java_size",
                  st.count == coll_map_fixture::j_size("linkedSmallSize"));
        ctx.check("linked_small_no_null_keys", st.null_keys == 0);
        ctx.check("linked_small_no_null_values", st.null_values == 0);
        // Same content as hashSmall (k0..k2 / v0..v2): fingerprints must match.
        ctx.check("linked_small_id_sum_is_3", st.id_sum == (0 + 1 + 2));
        ctx.check("linked_small_key_char_sum_matches_hash_small",
                  st.key_char_sum == coll_map_fixture::j_long("hashSmallKeyCharSum"));
    }
    {
        const auto e{ coll_map_fixture::entries_of("linkedMany") };
        const entry_stats st{ fingerprint(e) };
        ctx.check("linked_many_count_is_1000", st.count == MANY_N);
        ctx.check("linked_many_count_matches_java_size",
                  st.count == coll_map_fixture::j_size("linkedManySize"));
        ctx.check("linked_many_id_sum_closed_form",
                  st.id_sum == (static_cast<std::int64_t>(MANY_N) * (MANY_N - 1)) / 2);
        ctx.check("linked_many_id_xor_matches_hash_many",
                  st.id_xor == coll_map_fixture::j_long("hashManyIdXor"));
    }

    // =====================================================================
    // TreeMap — EMPTY.  root is null → 0 entries, no throw.
    // =====================================================================
    {
        const auto e{ coll_map_fixture::entries_of("treeEmpty") };
        ctx.check("tree_empty_size_zero", e.empty());
        ctx.check("tree_empty_java_size_zero",
                  coll_map_fixture::j_size("treeEmptySize") == 0);
    }

    // =====================================================================
    // TreeMap — SMALL (3).  The red-black in-order walk yields SORTED key
    // order — verify count, content, AND strict ascending key order.
    // =====================================================================
    {
        const auto e{ coll_map_fixture::entries_of("treeSmall") };
        const entry_stats st{ fingerprint(e) };

        ctx.check("tree_small_count_is_3", st.count == SMALL_N);
        ctx.check("tree_small_count_matches_java_size",
                  st.count == coll_map_fixture::j_size("treeSmallSize"));
        ctx.check("tree_small_no_null_keys", st.null_keys == 0);
        ctx.check("tree_small_no_null_values", st.null_values == 0);
        ctx.check("tree_small_id_sum_matches_java",
                  st.id_sum == coll_map_fixture::j_long("treeSmallIdSum"));

        // Collect decoded keys IN WALK ORDER and assert strictly ascending,
        // first==Java firstKey, last==Java lastKey.
        std::vector<std::string> keys;
        keys.reserve(e.size());
        bool pairs_ok{ true };
        for (const auto& kv : e)
        {
            if (kv.first == nullptr || kv.second == nullptr) { pairs_ok = false; continue; }
            const std::string key{ kv.first->text() };
            keys.push_back(key);
            // internal consistency: key=="k"+id, name=="v"+id
            const std::int32_t id{ kv.second->id() };
            if (key != ("k" + std::to_string(id))) { pairs_ok = false; }
            if (kv.second->name() != ("v" + std::to_string(id))) { pairs_ok = false; }
        }
        ctx.check("tree_small_all_pairs_consistent", pairs_ok);
        const bool sorted{ std::is_sorted(keys.begin(), keys.end()) };
        ctx.check("tree_small_keys_in_ascending_order", sorted);
        ctx.check("tree_small_first_key_is_k0",
                  !keys.empty() && keys.front() == "k0");
        ctx.check("tree_small_last_key_is_k2",
                  !keys.empty() && keys.back() == "k2");
        ctx.check("tree_small_first_key_matches_java",
                  !keys.empty() && keys.front() == coll_map_fixture::j_string("treeSmallFirstKey"));
        ctx.check("tree_small_last_key_matches_java",
                  !keys.empty() && keys.back() == coll_map_fixture::j_string("treeSmallLastKey"));
    }

    // =====================================================================
    // TreeMap — MANY (1000).  Deep red-black tree; the iterative stack walk
    // must visit all nodes in sorted order without blowing the stack.
    // =====================================================================
    {
        const auto e{ coll_map_fixture::entries_of("treeMany") };
        const entry_stats st{ fingerprint(e) };

        ctx.check("tree_many_count_is_1000", st.count == MANY_N);
        ctx.check("tree_many_count_matches_java_size",
                  st.count == coll_map_fixture::j_size("treeManySize"));
        ctx.check("tree_many_no_null_keys", st.null_keys == 0);
        ctx.check("tree_many_no_null_values", st.null_values == 0);
        ctx.check("tree_many_id_sum_matches_java",
                  st.id_sum == coll_map_fixture::j_long("treeManyIdSum"));
        ctx.check("tree_many_id_sum_closed_form",
                  st.id_sum == (static_cast<std::int64_t>(MANY_N) * (MANY_N - 1)) / 2);

        // Strict lexicographic ascending order across all 1000 keys.
        std::vector<std::string> keys;
        keys.reserve(e.size());
        for (const auto& kv : e)
        {
            keys.push_back(kv.first ? kv.first->text() : std::string{});
        }
        ctx.check("tree_many_keys_in_ascending_order",
                  std::is_sorted(keys.begin(), keys.end()));
        // String order is lexicographic, so "k1" < "k10" < ... < "k2" < ...:
        // firstKey is "k0", lastKey is "k999".
        ctx.check("tree_many_first_key_is_k0",
                  !keys.empty() && keys.front() == "k0");
        ctx.check("tree_many_last_key_is_k999",
                  !keys.empty() && keys.back() == "k999");
    }

    // =====================================================================
    // ROBUSTNESS — to_entries must NEVER throw and must return empty on:
    //   (a) a NULL Map field,
    //   (b) a MISSING field name,
    //   (c) a non-Map reference field (String: no table/root).
    // =====================================================================
    {
        // (a) Declared-but-null Map field: the value_t null-oop guard fires.
        const auto e_null{ coll_map_fixture::entries_of("nullMap") };
        ctx.check("null_map_field_returns_empty", e_null.empty());

        // (b) Missing field name: static_field() yields nullopt → entries_of
        //     short-circuits to empty (proves the helper + contract).
        const auto e_missing{ coll_map_fixture::entries_of("noSuchMapFieldXYZ") };
        ctx.check("missing_map_field_returns_empty", e_missing.empty());

        // (c) Non-Map reference (a String): map::to_entries finds neither a
        //     "table" nor a "root" field on java.lang.String → empty.
        const auto e_notmap{ coll_map_fixture::entries_of("notAMap") };
        ctx.check("non_map_field_returns_empty", e_notmap.empty());

        // Re-reading the same null/missing fields twice must remain stable
        // (no state corruption, still empty, still no throw).
        ctx.check("null_map_field_stable_on_reread",
                  coll_map_fixture::entries_of("nullMap").empty());
        ctx.check("missing_map_field_stable_on_reread",
                  coll_map_fixture::entries_of("noSuchMapFieldXYZ").empty());
    }

    // =====================================================================
    // Re-read stability: decoding the same populated map twice yields the same
    // fingerprint (the walk has no destructive side effects on the heap).
    // =====================================================================
    {
        const auto a{ coll_map_fixture::entries_of("hashSmall") };
        const auto b{ coll_map_fixture::entries_of("hashSmall") };
        const entry_stats sa{ fingerprint(a) };
        const entry_stats sb{ fingerprint(b) };
        ctx.check("hash_small_reread_same_count", sa.count == sb.count);
        ctx.check("hash_small_reread_same_id_sum", sa.id_sum == sb.id_sum);
        ctx.check("hash_small_reread_same_key_char_sum", sa.key_char_sum == sb.key_char_sum);
    }

    // =====================================================================
    // Interpreter-hook proof (pilot-style): install a scoped_hook on touch(),
    // drive mode 1, confirm the detour fires on real bytecode dispatch with the
    // right self+arg and the original body runs (observed == seed+42 == 7042).
    // scoped_hook (never shutdown_hooks) so this module stays isolated.
    // =====================================================================
    {
        auto handle{ vmhook::scoped_hook<coll_map_fixture>(
            "touch",
            [](vmhook::return_value&,
               const std::unique_ptr<coll_map_fixture>& self,
               std::int32_t delta)
            {
                g_hook_calls.fetch_add(1, std::memory_order_relaxed);
                g_hook_arg.store(delta, std::memory_order_relaxed);
                g_hook_saw_self.store(self != nullptr, std::memory_order_relaxed);
            }) };
        ctx.check("collmap_hook_installed", handle.installed());

        const bool done{ ctx.run_probe(
            [](bool value)
            {
                if (value)
                {
                    coll_map_fixture::set_done(false);
                    coll_map_fixture::set_mode(1);
                }
                coll_map_fixture::set_go(value);
            },
            []() { return coll_map_fixture::get_done(); }) };

        ctx.check("collmap_probe_completed", done);
        ctx.check("collmap_hook_fired",
                  g_hook_calls.load(std::memory_order_relaxed) >= 1);
        ctx.check("collmap_hook_saw_self",
                  g_hook_saw_self.load(std::memory_order_relaxed));
        ctx.check("collmap_hook_saw_arg_42",
                  g_hook_arg.load(std::memory_order_relaxed) == 42);
        ctx.check("collmap_observed_is_7042",
                  coll_map_fixture::get_observed() == 7042);
    }
}
