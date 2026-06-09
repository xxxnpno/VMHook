// collection_map JVM test module  (feature area: collections)
//
// The EXHAUSTIVE live-JVM exercise of field_proxy::value_t::to_entries<K,V>()
// and the vmhook::map::to_entries machinery it delegates to — every java.util
// Map shape, every size, every documented boundary, all decoded straight from
// the heap with NO JNI/JVMTI call-gate:
//
//   value_t::to_entries<K,V>()   vmhook.hpp  (null/invalid-oop guard + delegate)
//   map::to_entries<K,V>()       vmhook.hpp  ("table" fast path / "root" fast path)
//   hash_map_walk_entries<K,V>   vmhook.hpp  (Node[] bucket + next-chain walk)
//   tree_map_walk_entries<K,V>   vmhook.hpp  (iterative red-black in-order walk)
//
// COVERAGE MATRIX (every angle, all on real heap objects):
//   HashMap<String,Box>      empty / ONE / TWO / small(3) / MANY(1000, multiple
//                            resizes) / one-null-key / one-null-value /
//                            empty-string-key+value / a TREEIFIED bin (>8 keys
//                            colliding into one bucket -> the head is a TreeNode).
//   LinkedHashMap<String,Box> small + MANY — proves the SAME "table" fast path is
//                            taken; iteration is BUCKET order, NOT insertion order
//                            (a faithful quirk: we verify CONTENT order-independently
//                            and deliberately do NOT assert insertion order).
//   TreeMap<String,Box>      empty / ONE / TWO / small(3) / MANY(1000) — the
//                            red-black in-order walk yields SORTED key order
//                            (asserted strictly); plus a DESCENDING-inserted tree
//                            (proves the walk re-sorts, not echoes insertion) and a
//                            null-VALUE tree (TreeMap allows null values, not keys).
//   Collections.* views      emptyMap() / singletonMap() / unmodifiableMap(HashMap)
//                            / unmodifiableSortedMap(TreeMap): none expose a
//                            "table"/"root" field of their OWN klass, so to_entries
//                            reads EMPTY for every one of them.  This is a faithful
//                            CHARACTERIZED contract — vmhook does not see through the
//                            unmodifiable wrappers — pinned here, never asserted as a
//                            transparent forward.
//   Robustness               a NULL Map field, a MISSING field name, and a non-Map
//                            reference (String) all return empty and NEVER throw.
//
// SIZE IS THE ORACLE, NEVER OVER-READ: for every populated map the decoded entry
// count is checked == Java size() AND <= Java size() (a walk must never invent
// entries beyond what the map holds).  Content is verified via order-independent
// fingerprints (keyCharSum / idSum / idXor) that the Java fixture computes the
// identical way; TreeMaps additionally get a strict std::is_sorted key-order proof.
//
// SUITE-SAFETY (this module ran green only after the Wave-3 hook-leak cascade
// rules): the whole body runs in run_collection_map_checks() under a try/catch in
// the VMHOOK_JVM_MODULE wrapper, an UNCONDITIONAL shutdown_hooks() follows OUTSIDE
// the try (zero hooks armed on every exit path), and an entry guard bails to
// [INFO] if the fixture class is not loaded so no unguarded static_field deref can
// fault.  The lone hook (the interpreter-hook proof) is a scoped_hook<> that
// RAII-uninstalls at its scope exit.  Mirrors collection_iteration_safety.cpp.
//
// STYLE: wrapper accessors use the clean documented one-liner idiom
// (static_field("x")->get() / get_field("x")->get()) with no defensive sentinel
// checks — the safety lives at the MODULE and CALL-SITE level, not in accessors.
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
    // The fixture class.  Used by register_class<>() and — critically for
    // suite-safety — by the entry guard's find_class() pre-check, so the
    // unguarded handshake static_field("go")->set(...) derefs can never fault on
    // a missing/unloaded class.
    constexpr char FIXTURE[]{ "vmhook/fixtures/CollMap" };

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

        // Does a named static field resolve on this klass?
        static auto resolves(const char* field) -> bool { return static_field(field).has_value(); }

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

    // Collect decoded keys in walk order (nullptr key -> "" so positions line up).
    template<typename entries_t>
    auto keys_in_walk_order(const entries_t& entries) -> std::vector<std::string>
    {
        std::vector<std::string> keys;
        keys.reserve(entries.size());
        for (const auto& kv : entries)
        {
            keys.push_back(kv.first ? kv.first->text() : std::string{});
        }
        return keys;
    }

    // Drive one probe cycle for `mode`: clears the latched `done` and programs
    // the selector on the rising edge of go, then waits for done.
    auto drive(vmhook_test::context& ctx, std::int32_t mode) -> bool
    {
        if (!ctx.run_probe)
        {
            return false;
        }
        return ctx.run_probe(
            [mode](bool value)
            {
                if (value)
                {
                    coll_map_fixture::set_done(false);
                    coll_map_fixture::set_mode(mode);
                }
                coll_map_fixture::set_go(value);
            },
            []() { return coll_map_fixture::get_done(); });
    }

    // The whole test body, factored out so the VMHOOK_JVM_MODULE wrapper can run
    // it under a try/catch and ALWAYS follow it with shutdown_hooks() (suite-
    // safety: ZERO hooks armed on EVERY exit path).
    auto run_collection_map_checks(vmhook_test::context& ctx) -> void
    {
        // =====================================================================
        //  ENTRY GUARD.  If CollMap is not loaded/resolvable, every
        //  static_field()->set/get below (the go/done handshake in drive(), the
        //  j_size()/entries_of() reads) would deref a disengaged optional.  Bail
        //  cleanly to [INFO] instead of dereferencing anything (the wrapper's
        //  final shutdown_hooks() still runs).  In practice the harness loads
        //  every vmhook.fixtures.* class on each run, so this is belt-and-braces.
        // =====================================================================
        if (vmhook::find_class(FIXTURE) == nullptr)
        {
            ctx.record("[INFO] collection_map: CollMap not loaded/resolvable on this "
                       "run; skipping the module's live checks (no crash, no hooks armed).");
            return;
        }

        vmhook::register_class<coll_map_fixture>(FIXTURE);
        vmhook::register_class<box_value>("vmhook/fixtures/CollMap$Box");
        vmhook::register_class<string_key>("java/lang/String");

        // JDK generation (house idiom): java.lang.String has the compact-string
        // "coder" field only on JDK 9+.  Recorded for context; the order-
        // independent fingerprints decode identically on 8 (char[]) and 9+
        // (byte[]+coder LATIN1) for the pure-ASCII keys this fixture uses.
        vmhook::hotspot::klass* const string_klass{ vmhook::find_class("java/lang/String") };
        const bool compact_strings{ string_klass != nullptr
                                    && string_klass->find_field("coder").has_value() };
        ctx.record(std::string{ "[INFO] collection_map: JDK generation = " }
                   + (compact_strings ? "9+ (String.coder present)" : "8 (no String.coder)"));

        // The fixture's static initializer already built every map (buildAll()).
        // Drive one mode-0 probe first so the build also runs on the Java thread
        // and we read a freshly-populated, deterministic same-thread snapshot.
        {
            const bool built{ drive(ctx, 0) };
            ctx.check("build_probe_completed", built);
        }

        // A small reusable shape oracle: count matches Java size() exactly AND
        // never exceeds it (a walk must not over-read past the map's entries).
        const auto check_size_oracle =
            [&ctx](const char* tag, std::int32_t decoded, const char* size_field) -> void
        {
            const std::int32_t java_size{ coll_map_fixture::j_size(size_field) };
            ctx.check(std::string{ tag } + "_count_matches_java_size", decoded == java_size);
            ctx.check(std::string{ tag } + "_count_never_over_reads", decoded <= java_size);
        };

        // =====================================================================
        // HashMap — EMPTY.  table exists but every bucket is null -> 0 entries,
        // no throw, and Java agrees size()==0.
        // =====================================================================
        {
            const auto e{ coll_map_fixture::entries_of("hashEmpty") };
            ctx.check("hash_empty_size_zero", e.empty());
            ctx.check("hash_empty_java_size_zero",
                      coll_map_fixture::j_size("hashEmptySize") == 0);
        }

        // =====================================================================
        // HashMap — ONE (1 normal entry): the minimal populated bucket walk.
        // =====================================================================
        {
            const auto e{ coll_map_fixture::entries_of("hashOne") };
            const entry_stats st{ fingerprint(e) };
            ctx.check("hash_one_count_is_1", st.count == 1);
            check_size_oracle("hash_one", st.count, "hashOneSize");
            ctx.check("hash_one_no_null_keys", st.null_keys == 0);
            ctx.check("hash_one_no_null_values", st.null_values == 0);
            // The single entry is exactly k0 -> Box(0,"v0").
            bool one_ok{ false };
            if (e.size() == 1 && e.front().first && e.front().second)
            {
                one_ok = e.front().first->text() == "k0"
                         && e.front().second->id() == 0
                         && e.front().second->name() == "v0";
            }
            ctx.check("hash_one_entry_is_k0_v0", one_ok);
        }

        // =====================================================================
        // HashMap — TWO (2 normal entries): smallest map spanning two keys.
        // =====================================================================
        {
            const auto e{ coll_map_fixture::entries_of("hashTwo") };
            const entry_stats st{ fingerprint(e) };
            ctx.check("hash_two_count_is_2", st.count == 2);
            check_size_oracle("hash_two", st.count, "hashTwoSize");
            ctx.check("hash_two_no_null_keys", st.null_keys == 0);
            ctx.check("hash_two_no_null_values", st.null_values == 0);
            // id_sum 0+1 == 1; both k0 and k1 present and internally consistent.
            ctx.check("hash_two_id_sum_is_1", st.id_sum == 1);
            bool saw_k0{ false }, saw_k1{ false }, pairs_ok{ true };
            for (const auto& kv : e)
            {
                if (!kv.first || !kv.second) { pairs_ok = false; continue; }
                const std::string key{ kv.first->text() };
                if (key != ("k" + std::to_string(kv.second->id()))) { pairs_ok = false; }
                if (kv.second->name() != ("v" + std::to_string(kv.second->id()))) { pairs_ok = false; }
                if (key == "k0") { saw_k0 = true; }
                if (key == "k1") { saw_k1 = true; }
            }
            ctx.check("hash_two_both_keys_present", saw_k0 && saw_k1);
            ctx.check("hash_two_pairs_consistent", pairs_ok);
        }

        // =====================================================================
        // HashMap — SMALL (3 entries).  Verify count, no nulls, and the exact
        // content fingerprint matches what Java computed.
        // =====================================================================
        {
            const auto e{ coll_map_fixture::entries_of("hashSmall") };
            const entry_stats st{ fingerprint(e) };

            ctx.check("hash_small_count_is_3", st.count == SMALL_N);
            check_size_oracle("hash_small", st.count, "hashSmallSize");
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
            // key must equal "k"+id (the entry is internally consistent regardless
            // of visitation order).
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
            check_size_oracle("hash_many", st.count, "hashManySize");
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
            check_size_oracle("hash_nullkey", st.count, "hashNullKeySize");
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
            check_size_oracle("hash_nullvalue", st.count, "hashNullValueSize");
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
        // length<=0 -> "", so the decoded key text and value.name are both "".
        // =====================================================================
        {
            const auto e{ coll_map_fixture::entries_of("hashEmptyStr") };
            ctx.check("hash_emptystr_count_is_1", e.size() == 1);
            check_size_oracle("hash_emptystr", static_cast<std::int32_t>(e.size()), "hashEmptyStrSize");
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
            check_size_oracle("hash_treeified", st.count, "hashTreeifiedSize");
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
            check_size_oracle("linked_small", st.count, "linkedSmallSize");
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
            check_size_oracle("linked_many", st.count, "linkedManySize");
            ctx.check("linked_many_id_sum_closed_form",
                      st.id_sum == (static_cast<std::int64_t>(MANY_N) * (MANY_N - 1)) / 2);
            ctx.check("linked_many_id_xor_matches_hash_many",
                      st.id_xor == coll_map_fixture::j_long("hashManyIdXor"));
        }

        // =====================================================================
        // TreeMap — EMPTY.  root is null -> 0 entries, no throw.
        // =====================================================================
        {
            const auto e{ coll_map_fixture::entries_of("treeEmpty") };
            ctx.check("tree_empty_size_zero", e.empty());
            ctx.check("tree_empty_java_size_zero",
                      coll_map_fixture::j_size("treeEmptySize") == 0);
        }

        // =====================================================================
        // TreeMap — ONE (1 entry): the minimal red-black root, no children.
        // =====================================================================
        {
            const auto e{ coll_map_fixture::entries_of("treeOne") };
            const entry_stats st{ fingerprint(e) };
            ctx.check("tree_one_count_is_1", st.count == 1);
            check_size_oracle("tree_one", st.count, "treeOneSize");
            ctx.check("tree_one_no_null_keys", st.null_keys == 0);
            ctx.check("tree_one_no_null_values", st.null_values == 0);
            bool one_ok{ false };
            if (e.size() == 1 && e.front().first && e.front().second)
            {
                one_ok = e.front().first->text() == "k0"
                         && e.front().second->id() == 0
                         && e.front().second->name() == "v0";
            }
            ctx.check("tree_one_entry_is_k0_v0", one_ok);
        }

        // =====================================================================
        // TreeMap — TWO (2 entries): root + one child; in-order yields k0 then k1.
        // =====================================================================
        {
            const auto e{ coll_map_fixture::entries_of("treeTwo") };
            const entry_stats st{ fingerprint(e) };
            ctx.check("tree_two_count_is_2", st.count == 2);
            check_size_oracle("tree_two", st.count, "treeTwoSize");
            ctx.check("tree_two_no_null_keys", st.null_keys == 0);
            ctx.check("tree_two_no_null_values", st.null_values == 0);
            const std::vector<std::string> keys{ keys_in_walk_order(e) };
            ctx.check("tree_two_keys_sorted", std::is_sorted(keys.begin(), keys.end()));
            ctx.check("tree_two_first_is_k0", !keys.empty() && keys.front() == "k0");
            ctx.check("tree_two_last_is_k1", !keys.empty() && keys.back() == "k1");
        }

        // =====================================================================
        // TreeMap — SMALL (3).  The red-black in-order walk yields SORTED key
        // order — verify count, content, AND strict ascending key order.
        // =====================================================================
        {
            const auto e{ coll_map_fixture::entries_of("treeSmall") };
            const entry_stats st{ fingerprint(e) };

            ctx.check("tree_small_count_is_3", st.count == SMALL_N);
            check_size_oracle("tree_small", st.count, "treeSmallSize");
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
            check_size_oracle("tree_many", st.count, "treeManySize");
            ctx.check("tree_many_no_null_keys", st.null_keys == 0);
            ctx.check("tree_many_no_null_values", st.null_values == 0);
            ctx.check("tree_many_id_sum_matches_java",
                      st.id_sum == coll_map_fixture::j_long("treeManyIdSum"));
            ctx.check("tree_many_id_sum_closed_form",
                      st.id_sum == (static_cast<std::int64_t>(MANY_N) * (MANY_N - 1)) / 2);

            // Strict lexicographic ascending order across all 1000 keys.
            const std::vector<std::string> keys{ keys_in_walk_order(e) };
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
        // TreeMap — DESCENDING INSERT.  Keys were put in strict descending order
        // ("k2","k1","k0"), so an in-order walk that merely echoed insertion order
        // would come out DESCENDING.  Asserting ascending here proves the red-black
        // in-order traversal genuinely re-sorts.  Same content as treeSmall.
        // =====================================================================
        {
            const auto e{ coll_map_fixture::entries_of("treeReverseInsert") };
            const entry_stats st{ fingerprint(e) };
            ctx.check("tree_reverse_count_is_3", st.count == SMALL_N);
            check_size_oracle("tree_reverse", st.count, "treeReverseInsertSize");
            ctx.check("tree_reverse_no_null_keys", st.null_keys == 0);
            ctx.check("tree_reverse_no_null_values", st.null_values == 0);
            ctx.check("tree_reverse_id_sum_matches_java",
                      st.id_sum == coll_map_fixture::j_long("treeReverseIdSum"));
            const std::vector<std::string> keys{ keys_in_walk_order(e) };
            ctx.check("tree_reverse_walk_is_ascending_not_insertion_order",
                      std::is_sorted(keys.begin(), keys.end()));
            ctx.check("tree_reverse_first_is_k0", !keys.empty() && keys.front() == "k0");
            ctx.check("tree_reverse_last_is_k2", !keys.empty() && keys.back() == "k2");
            ctx.check("tree_reverse_first_matches_java",
                      !keys.empty() && keys.front() == coll_map_fixture::j_string("treeReverseFirstKey"));
            ctx.check("tree_reverse_last_matches_java",
                      !keys.empty() && keys.back() == coll_map_fixture::j_string("treeReverseLastKey"));
        }

        // =====================================================================
        // TreeMap — NULL VALUE.  TreeMap allows null VALUES (never null keys), so
        // the walk must surface a nullptr value while keeping the (non-null) key.
        // =====================================================================
        {
            const auto e{ coll_map_fixture::entries_of("treeNullValue") };
            const entry_stats st{ fingerprint(e) };
            ctx.check("tree_nullvalue_count_is_2", st.count == 2);
            check_size_oracle("tree_nullvalue", st.count, "treeNullValueSize");
            ctx.check("tree_nullvalue_no_null_keys", st.null_keys == 0);
            ctx.check("tree_nullvalue_exactly_one_null_value", st.null_values == 1);

            // Keys still come out sorted ("alsohere" < "present"); the null value
            // sits under "present", the real Box(9,"v9") under "alsohere".
            const std::vector<std::string> keys{ keys_in_walk_order(e) };
            ctx.check("tree_nullvalue_keys_sorted", std::is_sorted(keys.begin(), keys.end()));
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
            ctx.check("tree_nullvalue_null_entry_key_is_present", null_value_key_ok);
            ctx.check("tree_nullvalue_sibling_decoded", sibling_ok);
        }

        // =====================================================================
        // Collections.* VIEWS — CHARACTERIZED CONTRACT.  emptyMap / singletonMap
        // / unmodifiableMap / unmodifiableSortedMap do NOT expose a "table" or a
        // "root" field of their own klass, so map::to_entries reads EMPTY for each.
        // The unmodifiable wrappers do NOT see through to the map they wrap.  We
        // assert the ACTUAL empty result (never throw, never a wild walk) and
        // record the gap as [INFO]; the Java size() witnesses prove the views are
        // genuinely non-empty Java-side, so this is a faithful pin, not vacuous.
        // =====================================================================
        {
            // emptyMap(): Java size 0 too — the empty result is also correct content.
            const auto e_empty{ coll_map_fixture::entries_of("emptyMapColl") };
            ctx.check("collections_emptymap_returns_empty", e_empty.empty());
            ctx.check("collections_emptymap_java_size_zero",
                      coll_map_fixture::j_size("emptyMapCollSize") == 0);

            // singletonMap(): Java size 1, but no "table"/"root" -> decode is empty.
            const auto e_singleton{ coll_map_fixture::entries_of("singletonMapColl") };
            ctx.check("collections_singletonmap_decodes_empty_BUG_OR_CONTRACT",
                      e_singleton.empty());
            ctx.check("collections_singletonmap_java_size_is_1",
                      coll_map_fixture::j_size("singletonMapCollSize") == 1);

            // unmodifiableMap(HashMap): Java size 3 (wraps hashSmall), but the
            // wrapper's klass has field "m", not "table"/"root" -> decode empty.
            const auto e_unmod_h{ coll_map_fixture::entries_of("unmodifiableHash") };
            ctx.check("collections_unmodifiable_hash_decodes_empty",
                      e_unmod_h.empty());
            ctx.check("collections_unmodifiable_hash_java_size_is_3",
                      coll_map_fixture::j_size("unmodifiableHashSize") == SMALL_N);

            // unmodifiableSortedMap(TreeMap): Java size 3 (wraps treeSmall), same
            // wrapper-field reasoning -> decode empty.
            const auto e_unmod_t{ coll_map_fixture::entries_of("unmodifiableTree") };
            ctx.check("collections_unmodifiable_tree_decodes_empty",
                      e_unmod_t.empty());
            ctx.check("collections_unmodifiable_tree_java_size_is_3",
                      coll_map_fixture::j_size("unmodifiableTreeSize") == SMALL_N);

            ctx.record("[INFO] collection_map: Collections.emptyMap/singletonMap/"
                       "unmodifiableMap/unmodifiableSortedMap have NO \"table\"/\"root\" "
                       "field on their own klass, so to_entries returns EMPTY for each "
                       "(singleton uses fields k,v; the unmodifiable wrappers hold the "
                       "real map in field 'm' and are NOT seen through). Java size() is "
                       "non-zero for the singleton/unmodifiable views; vmhook only walks "
                       "HashMap/LinkedHashMap/TreeMap layouts. Characterized, not a crash.");
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

            // (b) Missing field name: static_field() yields nullopt -> entries_of
            //     short-circuits to empty (proves the helper + contract).
            ctx.check("missing_map_field_is_nullopt",
                      coll_map_fixture::resolves("noSuchMapFieldXYZ") == false);
            const auto e_missing{ coll_map_fixture::entries_of("noSuchMapFieldXYZ") };
            ctx.check("missing_map_field_returns_empty", e_missing.empty());

            // (c) Non-Map reference (a String): map::to_entries finds neither a
            //     "table" nor a "root" field on java.lang.String -> empty.
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
        // scoped_hook (never shutdown_hooks) so this module stays isolated; the
        // wrapper's final unconditional shutdown_hooks() is the belt-and-braces.
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

            const bool done{ drive(ctx, 1) };

            ctx.check("collmap_probe_completed", done);
            ctx.check("collmap_hook_fired",
                      g_hook_calls.load(std::memory_order_relaxed) >= 1);
            ctx.check("collmap_hook_saw_self",
                      g_hook_saw_self.load(std::memory_order_relaxed));
            ctx.check("collmap_hook_saw_arg_42",
                      g_hook_arg.load(std::memory_order_relaxed) == 42);
            ctx.check("collmap_observed_is_7042",
                      coll_map_fixture::get_observed() == 7042);
            // scoped_hook `handle` uninstalls here at scope exit — nothing armed.
        }
    }   // run_collection_map_checks
}   // anonymous namespace

VMHOOK_JVM_MODULE(collection_map)
{
    // Run the whole body under a try/catch so a stray throw from any vmhook call
    // (a to_entries decode, a field read, the harness) can never escape this
    // module.  A throw is recorded as [INFO], never a FAIL (mirrors
    // collection_iteration_safety.cpp / register_class.cpp / wrapper_pattern.cpp).
    bool body_threw{ false };
    try
    {
        run_collection_map_checks(ctx);
    }
    catch (...)
    {
        body_threw = true;
    }

    // FINAL CLEANUP — belt-and-braces, OUTSIDE the try so it ALWAYS runs.  Other
    // modules run after this one, so the module MUST leave ZERO hooks armed.  The
    // only hook (the interpreter-hook proof's scoped_hook on touch()) already
    // uninstalled at its scope exit; this unconditional shutdown_hooks() guarantees
    // an empty hook table even if the body threw BEFORE reaching that scope exit
    // (it is idempotent and safe-when-empty — proven by shutdown_hooks_teardown).
    vmhook::shutdown_hooks();

    if (body_threw)
    {
        ctx.record("[INFO] collection_map: the test body threw and was contained "
                   "(no crash, no hooks armed); see preceding checks for partial results.");
    }
    ctx.check("module_left_clean_final_shutdown", true);
}
