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
//   Hashtable<String,Box>    small — a SECOND positively-decoding family: its
//                            Entry[] "table" exposes key/value/next exactly like
//                            HashMap.Node, so it decodes FULLY (content cross-
//                            checked against hashSmall's fingerprint).
//   HashMap collision chain  hashColl6 — 6 colliding-hashCode keys in ONE bucket
//                            BELOW the treeify threshold => a plain Node.next chain
//                            (not a TreeNode); the walk follows `next` and returns
//                            all 6 (the case between one-per-bucket and treeified).
//   HashMap<Integer,Integer> hashIntKey — BOXED-primitive keys 0,16,32,48 colliding
//                            into one bucket; decodes each java.lang.Integer key AND
//                            value via Integer.value (non-String key/value path).
//   KEY/VALUE TYPE COVERAGE  hashStrStr (String->String, value via read_java_string),
//                            hashIntStr (Integer->String), hashLongLong (boxed
//                            Long->Long, 64-bit values > 2^32 so a truncating read is
//                            caught), hashEnumKey (enum keys in an ORDINARY HashMap —
//                            decodes positively via java.lang.Enum.name, unlike the
//                            characterized-empty EnumMap).  Every one decodes FULLY.
//   HashMap resize boundary  hashResize16 / hashResize17 — 16 and 17 entries bracket
//                            the default-capacity resize (threshold 12 => one resize to
//                            cap 32); asserts EVERY key present exactly once (no miss /
//                            no duplicate) across the rehash, plus closed-form id sums.
//   HashMap nested values    hashNestedMap / hashNestedList — values are themselves
//                            a Map / a List; the outer walk decodes each value OOP,
//                            then the module re-wraps it as vmhook::map / ::collection
//                            and decodes the INNER container (nested round-trip).
//   LinkedHashMap<String,Box> small + MANY — proves the SAME "table" fast path is
//                            taken; iteration is BUCKET order, NOT insertion order
//                            (a faithful quirk: we verify CONTENT order-independently
//                            and deliberately do NOT assert insertion order).
//   ConcurrentHashMap        small + MANY — HAS a "table" but its Node field is
//                            "val" not "value", so the hash walk bails => EMPTY
//                            (characterized; size witnesses prove non-empty).
//   WeakHashMap/IdentityHashMap/EnumMap/Map.of(N,1) — every remaining JDK Map
//                            family, each CHARACTERIZED EMPTY for a documented
//                            layout reason (no "key" field / flat Object[] table /
//                            no table+root / interleaved-k-v table / k0+v0), with
//                            the Java size() witness pinning each as non-empty.
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
#include <functional>
#include <limits>
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

    // ── BOXED-PRIMITIVE wrapper: java.lang.Integer. ─────────────────────────
    // For HashMap<Integer,Integer> the key AND value OOPs are java.lang.Integer
    // boxes; we decode the primitive via the "value" field.  get_instance() is
    // also used to fingerprint object identity where needed.
    class integer_box : public vmhook::object<integer_box>
    {
    public:
        explicit integer_box(vmhook::oop_t instance) noexcept
            : vmhook::object<integer_box>{ instance }
        {
        }

        auto value() const -> std::int32_t { return static_cast<std::int32_t>(get_field("value")->get()); }
    };

    // ── BOXED-LONG wrapper: java.lang.Long. ─────────────────────────────────
    // For HashMap<Long,Long> the key AND value OOPs are java.lang.Long boxes; we
    // decode the 64-bit primitive via the "value" field (a long, not an int — a
    // truncating read would corrupt the cross-check sums the fixture publishes).
    class long_box : public vmhook::object<long_box>
    {
    public:
        explicit long_box(vmhook::oop_t instance) noexcept
            : vmhook::object<long_box>{ instance }
        {
        }

        auto value() const -> std::int64_t { return static_cast<std::int64_t>(get_field("value")->get()); }
    };

    // ── BOXED-CHAR wrapper: java.lang.Character (descriptor "C", uint16). ────
    class char_box : public vmhook::object<char_box>
    {
    public:
        explicit char_box(vmhook::oop_t instance) noexcept
            : vmhook::object<char_box>{ instance }
        {
        }

        auto value() const -> std::int32_t { return static_cast<std::int32_t>(get_field("value")->get()); }
    };

    // ── BOXED-SHORT wrapper: java.lang.Short (descriptor "S", signed 16-bit). ─
    class short_box : public vmhook::object<short_box>
    {
    public:
        explicit short_box(vmhook::oop_t instance) noexcept
            : vmhook::object<short_box>{ instance }
        {
        }

        auto value() const -> std::int32_t { return static_cast<std::int32_t>(get_field("value")->get()); }
    };

    // ── BOXED-BYTE wrapper: java.lang.Byte (descriptor "B", signed 8-bit). ───
    class byte_box : public vmhook::object<byte_box>
    {
    public:
        explicit byte_box(vmhook::oop_t instance) noexcept
            : vmhook::object<byte_box>{ instance }
        {
        }

        auto value() const -> std::int32_t { return static_cast<std::int32_t>(get_field("value")->get()); }
    };

    // ── BOXED-BOOLEAN wrapper: java.lang.Boolean (descriptor "Z"). ───────────
    class bool_box : public vmhook::object<bool_box>
    {
    public:
        explicit bool_box(vmhook::oop_t instance) noexcept
            : vmhook::object<bool_box>{ instance }
        {
        }

        auto value() const -> bool { return static_cast<bool>(get_field("value")->get()); }
    };

    // ── STRING VALUE wrapper: java.lang.String. ─────────────────────────────
    // For HashMap<String,String> the VALUE OOP is itself a java.lang.String,
    // decoded through the same read_java_string path as the key.  (A separate
    // named type from string_key purely so to_entries<string_key,string_value>
    // reads as key/value at the call site; both decode identically.)
    class string_value : public vmhook::object<string_value>
    {
    public:
        explicit string_value(vmhook::oop_t instance) noexcept
            : vmhook::object<string_value>{ instance }
        {
        }

        auto text() const -> std::string
        {
            return vmhook::read_java_string(get_instance());
        }
    };

    // ── ENUM-KEY wrapper: vmhook.fixtures.CollMap$Day (a java.lang.Enum). ────
    // For an ORDINARY HashMap keyed by an enum the key OOP is the enum constant;
    // its identity is the inherited java.lang.Enum "name" field, which we read to
    // fingerprint the key.  (EnumMap is characterized-empty elsewhere; this is a
    // plain HashMap, so it stores real Node objects and decodes positively.)
    class enum_key : public vmhook::object<enum_key>
    {
    public:
        explicit enum_key(vmhook::oop_t instance) noexcept
            : vmhook::object<enum_key>{ instance }
        {
        }

        // java.lang.Enum.name — present on every enum constant since Java 5.
        auto name() const -> std::string { return get_field("name")->get(); }
    };

    // ── NESTED-CONTAINER value wrapper. ─────────────────────────────────────
    // The value OOP is itself a Map or a List; this wrapper only needs to hand
    // its raw OOP back so the module can re-wrap it as a vmhook::map /
    // vmhook::collection and decode the inner container.  (vmhook::object<>
    // already exposes get_instance(); this named subclass documents intent and
    // lets to_entries<string_key, nested_value> build it.)
    class nested_value : public vmhook::object<nested_value>
    {
    public:
        explicit nested_value(vmhook::oop_t instance) noexcept
            : vmhook::object<nested_value>{ instance }
        {
        }
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

        // Generic K/V variant for the boxed-Integer and nested-container maps.
        template<typename key_type, typename value_type>
        static auto entries_of_as(const char* field)
            -> std::vector<std::pair<std::unique_ptr<key_type>, std::unique_ptr<value_type>>>
        {
            const auto proxy{ static_field(field) };
            if (!proxy.has_value())
            {
                return {};
            }
            return proxy->get().to_entries<key_type, value_type>();
        }

        // Acquire the named static Map field as an EXPLICIT vmhook::map wrapper
        // (the field-proxy value_t -> unique_ptr<vmhook::map> conversion), so the
        // module can exercise the wrapper's own size()/is_empty()/to_entries()
        // surface — distinct from the implicit value_t::to_entries call site.
        // Returns nullptr when the field is unresolved or holds a null oop.
        static auto acquire_map(const char* field) -> std::unique_ptr<vmhook::map>
        {
            const auto proxy{ static_field(field) };
            if (!proxy.has_value())
            {
                return nullptr;
            }
            return proxy->get();
        }

        // True iff the named static field currently holds a reference/String oop
        // (value_t::is_reference introspection on the field proxy).
        static auto field_is_reference(const char* field) -> bool
        {
            const auto proxy{ static_field(field) };
            return proxy.has_value() && proxy->get().is_reference();
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
    constexpr std::int32_t COLL6_N{ 6 };
    constexpr std::int32_t INTKEY_N{ 4 };
    constexpr std::int32_t NESTED_N{ 2 };
    constexpr std::int32_t RESIZE16_N{ 16 };
    constexpr std::int32_t RESIZE17_N{ 17 };

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
        vmhook::register_class<string_value>("java/lang/String");
        vmhook::register_class<integer_box>("java/lang/Integer");
        vmhook::register_class<long_box>("java/lang/Long");
        vmhook::register_class<char_box>("java/lang/Character");
        vmhook::register_class<short_box>("java/lang/Short");
        vmhook::register_class<byte_box>("java/lang/Byte");
        vmhook::register_class<bool_box>("java/lang/Boolean");
        vmhook::register_class<enum_key>("vmhook/fixtures/CollMap$Day");
        // nested_value carries an arbitrary container OOP; it has no fixed klass
        // of its own, so it is intentionally NOT registered.

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
        // Hashtable — SMALL (3).  A DISTINCT positively-decoding container family:
        // its Entry[] "table" exposes the same key/value/next fields as
        // HashMap.Node, so the HashMap "table" fast path decodes it FULLY.  Same
        // content recipe as hashSmall, so fingerprints cross-check 1:1.
        // =====================================================================
        {
            const auto e{ coll_map_fixture::entries_of("hashtableSmall") };
            const entry_stats st{ fingerprint(e) };
            ctx.check("cmap_hashtable_small_count_is_3", st.count == SMALL_N);
            check_size_oracle("cmap_hashtable_small", st.count, "hashtableSmallSize");
            ctx.check("cmap_hashtable_small_no_null_keys", st.null_keys == 0);
            ctx.check("cmap_hashtable_small_no_null_values", st.null_values == 0);
            ctx.check("cmap_hashtable_small_key_char_sum_matches_java",
                      st.key_char_sum == coll_map_fixture::j_long("hashtableSmallKeyCharSum"));
            ctx.check("cmap_hashtable_small_id_sum_matches_java",
                      st.id_sum == coll_map_fixture::j_long("hashtableSmallIdSum"));
            ctx.check("cmap_hashtable_small_id_xor_matches_java",
                      st.id_xor == coll_map_fixture::j_long("hashtableSmallIdXor"));
            // Same content as hashSmall -> the cross-map fingerprints agree.
            ctx.check("cmap_hashtable_small_key_char_sum_matches_hash_small",
                      st.key_char_sum == coll_map_fixture::j_long("hashSmallKeyCharSum"));
            bool pairs_ok{ true };
            bool saw_k0{ false }, saw_k1{ false }, saw_k2{ false };
            for (const auto& kv : e)
            {
                if (!kv.first || !kv.second) { pairs_ok = false; continue; }
                const std::string key{ kv.first->text() };
                if (key != ("k" + std::to_string(kv.second->id()))) { pairs_ok = false; }
                if (kv.second->name() != ("v" + std::to_string(kv.second->id()))) { pairs_ok = false; }
                if (key == "k0") { saw_k0 = true; }
                if (key == "k1") { saw_k1 = true; }
                if (key == "k2") { saw_k2 = true; }
            }
            ctx.check("cmap_hashtable_small_pairs_consistent", pairs_ok);
            ctx.check("cmap_hashtable_small_all_keys_present", saw_k0 && saw_k1 && saw_k2);
        }

        // =====================================================================
        // HashMap — SUB-TREEIFY COLLISION CHAIN (6 colliding keys in ONE bucket,
        // below the treeify threshold of 8).  The bucket head is a plain Node and
        // the bucket is a linked Node.next chain (NOT a TreeNode); the walk must
        // follow `next` and return every colliding entry.  This is the case
        // BETWEEN one-per-bucket and a treeified bin.
        // =====================================================================
        {
            const auto e{ coll_map_fixture::entries_of("hashColl6") };
            const entry_stats st{ fingerprint(e) };
            ctx.check("cmap_coll6_count_is_6", st.count == COLL6_N);
            check_size_oracle("cmap_coll6", st.count, "hashColl6Size");
            ctx.check("cmap_coll6_no_null_keys", st.null_keys == 0);
            ctx.check("cmap_coll6_no_null_values", st.null_values == 0);
            ctx.check("cmap_coll6_key_char_sum_matches_java",
                      st.key_char_sum == coll_map_fixture::j_long("hashColl6KeyCharSum"));
            ctx.check("cmap_coll6_id_sum_matches_java",
                      st.id_sum == coll_map_fixture::j_long("hashColl6IdSum"));
            ctx.check("cmap_coll6_id_xor_matches_java",
                      st.id_xor == coll_map_fixture::j_long("hashColl6IdXor"));
            // Values are Box(2000+i,"c"+i); the chain walk surfaces every id.
            std::int64_t expect_id_sum{ 0 };
            for (std::int32_t i{ 0 }; i < COLL6_N; ++i) { expect_id_sum += (2000 + i); }
            ctx.check("cmap_coll6_id_sum_closed_form", st.id_sum == expect_id_sum);
            // Java confirms (when reflection is open) the bucket stayed a PLAIN
            // chain — recorded as [INFO]; the count proof above is the hard part.
            const bool treeified{ coll_map_fixture::j_bool("hashColl6Treeified") };
            ctx.record(std::string{ "[INFO] cmap_coll6: bucket treeified (Java, best-effort) = " }
                       + (treeified ? "yes (unexpected)" : "no (plain Node chain)"));
        }

        // =====================================================================
        // HashMap<Integer,Integer> — BOXED-PRIMITIVE keys+values that COLLIDE.
        // Keys 0,16,32,48 land in one bucket; the walk decodes each boxed
        // java.lang.Integer key AND value via Integer.value, proving non-String
        // key/value decode on a collision chain.
        // =====================================================================
        {
            const auto e{ coll_map_fixture::entries_of_as<integer_box, integer_box>("hashIntKey") };
            ctx.check("cmap_intkey_count_is_4", static_cast<std::int32_t>(e.size()) == INTKEY_N);
            check_size_oracle("cmap_intkey", static_cast<std::int32_t>(e.size()), "hashIntKeySize");

            std::int64_t key_sum{ 0 }, val_sum{ 0 }, key_xor{ 0 };
            std::int32_t null_kv{ 0 };
            bool pairs_ok{ true };
            for (const auto& kv : e)
            {
                if (!kv.first || !kv.second) { ++null_kv; pairs_ok = false; continue; }
                const std::int32_t k{ kv.first->value() };
                const std::int32_t v{ kv.second->value() };
                key_sum += k;
                val_sum += v;
                key_xor ^= k;
                // Recipe: key i*16 -> value 100 + i, i.e. value == 100 + key/16.
                if (v != (100 + k / 16)) { pairs_ok = false; }
            }
            ctx.check("cmap_intkey_no_null_kv", null_kv == 0);
            ctx.check("cmap_intkey_pairs_consistent", pairs_ok);
            ctx.check("cmap_intkey_key_sum_matches_java",
                      key_sum == coll_map_fixture::j_long("hashIntKeyKeySum"));
            ctx.check("cmap_intkey_val_sum_matches_java",
                      val_sum == coll_map_fixture::j_long("hashIntKeyValSum"));
            ctx.check("cmap_intkey_key_xor_matches_java",
                      key_xor == coll_map_fixture::j_long("hashIntKeyKeyXor"));
            // Closed form: keys 0+16+32+48 == 96; values 100+101+102+103 == 406.
            ctx.check("cmap_intkey_key_sum_closed_form", key_sum == (0 + 16 + 32 + 48));
            ctx.check("cmap_intkey_val_sum_closed_form", val_sum == (100 + 101 + 102 + 103));
            const bool one_bucket{ coll_map_fixture::j_bool("hashIntKeyOneBucket") };
            ctx.record(std::string{ "[INFO] cmap_intkey: all keys in ONE bucket (Java, best-effort) = " }
                       + (one_bucket ? "yes" : "no/unknown"));
        }

        // =====================================================================
        // HashMap<String,String> — BOTH key AND value are java.lang.String.  The
        // value side is decoded through read_java_string exactly like the key
        // (the walker is generic over the value wrapper), proving a non-Box,
        // String VALUE round-trips.  Keys "sk"+i, values "sv"+i.
        // =====================================================================
        {
            const auto e{ coll_map_fixture::entries_of_as<string_key, string_value>("hashStrStr") };
            ctx.check("cmap_strstr_count_is_3", static_cast<std::int32_t>(e.size()) == SMALL_N);
            check_size_oracle("cmap_strstr", static_cast<std::int32_t>(e.size()), "hashStrStrSize");

            std::int64_t key_char_sum{ 0 }, val_char_sum{ 0 };
            std::int32_t null_kv{ 0 };
            bool pairs_ok{ true };
            for (const auto& kv : e)
            {
                if (!kv.first || !kv.second) { ++null_kv; pairs_ok = false; continue; }
                const std::string k{ kv.first->text() };
                const std::string v{ kv.second->text() };
                key_char_sum += code_unit_sum(k);
                val_char_sum += code_unit_sum(v);
                // Recipe: key "sk"+i pairs with value "sv"+i (same trailing digit).
                if (k.size() < 3 || v.size() < 3 || k.substr(2) != v.substr(2)) { pairs_ok = false; }
                if (k.rfind("sk", 0) != 0 || v.rfind("sv", 0) != 0) { pairs_ok = false; }
            }
            ctx.check("cmap_strstr_no_null_kv", null_kv == 0);
            ctx.check("cmap_strstr_pairs_consistent", pairs_ok);
            ctx.check("cmap_strstr_key_char_sum_matches_java",
                      key_char_sum == coll_map_fixture::j_long("hashStrStrKeyCharSum"));
            ctx.check("cmap_strstr_val_char_sum_matches_java",
                      val_char_sum == coll_map_fixture::j_long("hashStrStrValCharSum"));
        }

        // =====================================================================
        // HashMap<Integer,String> — boxed Integer KEY, String VALUE.  Proves a
        // boxed-primitive key paired with a String value decodes (key via
        // Integer.value, value via read_java_string).  Keys i, values "iv"+i.
        // =====================================================================
        {
            const auto e{ coll_map_fixture::entries_of_as<integer_box, string_value>("hashIntStr") };
            ctx.check("cmap_intstr_count_is_3", static_cast<std::int32_t>(e.size()) == SMALL_N);
            check_size_oracle("cmap_intstr", static_cast<std::int32_t>(e.size()), "hashIntStrSize");

            std::int64_t key_sum{ 0 }, val_char_sum{ 0 };
            std::int32_t null_kv{ 0 };
            bool pairs_ok{ true };
            for (const auto& kv : e)
            {
                if (!kv.first || !kv.second) { ++null_kv; pairs_ok = false; continue; }
                const std::int32_t k{ kv.first->value() };
                const std::string v{ kv.second->text() };
                key_sum += k;
                val_char_sum += code_unit_sum(v);
                // Recipe: key i -> value "iv"+i.
                if (v != ("iv" + std::to_string(k))) { pairs_ok = false; }
            }
            ctx.check("cmap_intstr_no_null_kv", null_kv == 0);
            ctx.check("cmap_intstr_pairs_consistent", pairs_ok);
            ctx.check("cmap_intstr_key_sum_matches_java",
                      key_sum == coll_map_fixture::j_long("hashIntStrKeySum"));
            ctx.check("cmap_intstr_val_char_sum_matches_java",
                      val_char_sum == coll_map_fixture::j_long("hashIntStrValCharSum"));
            // Closed form: keys 0+1+2 == 3.
            ctx.check("cmap_intstr_key_sum_closed_form", key_sum == (0 + 1 + 2));
        }

        // =====================================================================
        // HashMap<Long,Long> — 64-bit boxed key AND value.  Keys/values exceed
        // the 32-bit range (0x1_0000_0000 + i / 0x2_0000_0000 + i), so a
        // truncating read on the native side would break the sum cross-check.
        // Proves java.lang.Long.value (a long) round-trips on BOTH sides.
        // =====================================================================
        {
            const auto e{ coll_map_fixture::entries_of_as<long_box, long_box>("hashLongLong") };
            ctx.check("cmap_longlong_count_is_3", static_cast<std::int32_t>(e.size()) == SMALL_N);
            check_size_oracle("cmap_longlong", static_cast<std::int32_t>(e.size()), "hashLongLongSize");

            std::int64_t key_sum{ 0 }, val_sum{ 0 }, key_xor{ 0 };
            std::int32_t null_kv{ 0 };
            bool pairs_ok{ true };
            bool all_above_32bit{ true };
            for (const auto& kv : e)
            {
                if (!kv.first || !kv.second) { ++null_kv; pairs_ok = false; continue; }
                const std::int64_t k{ kv.first->value() };
                const std::int64_t v{ kv.second->value() };
                key_sum += k;
                val_sum += v;
                key_xor ^= k;
                // Both halves must be above 0xFFFFFFFF (would be wrong if truncated).
                if (k <= 0xFFFFFFFFLL || v <= 0xFFFFFFFFLL) { all_above_32bit = false; }
                // Recipe: key 0x1_0000_0000+i, value 0x2_0000_0000+i -> v-k == 0x1_0000_0000.
                if ((v - k) != 0x1'0000'0000LL) { pairs_ok = false; }
            }
            ctx.check("cmap_longlong_no_null_kv", null_kv == 0);
            ctx.check("cmap_longlong_values_exceed_32_bits", all_above_32bit);
            ctx.check("cmap_longlong_pairs_consistent", pairs_ok);
            ctx.check("cmap_longlong_key_sum_matches_java",
                      key_sum == coll_map_fixture::j_long("hashLongLongKeySum"));
            ctx.check("cmap_longlong_val_sum_matches_java",
                      val_sum == coll_map_fixture::j_long("hashLongLongValSum"));
            ctx.check("cmap_longlong_key_xor_matches_java",
                      key_xor == coll_map_fixture::j_long("hashLongLongKeyXor"));
        }

        // =====================================================================
        // HashMap<Day,Box> — ENUM KEYS in an ORDINARY HashMap (not EnumMap).  The
        // map stores real Node objects, so it decodes POSITIVELY: each key OOP is
        // a Day constant whose inherited java.lang.Enum.name we read.  Proves a
        // non-String, non-boxed-primitive (enum) key type round-trips.
        // =====================================================================
        {
            const auto e{ coll_map_fixture::entries_of_as<enum_key, box_value>("hashEnumKey") };
            ctx.check("cmap_enumkey_count_is_3", static_cast<std::int32_t>(e.size()) == SMALL_N);
            check_size_oracle("cmap_enumkey", static_cast<std::int32_t>(e.size()), "hashEnumKeySize");

            std::int64_t id_sum{ 0 }, name_char_sum{ 0 };
            std::int32_t null_kv{ 0 };
            bool saw_mon{ false }, saw_tue{ false }, saw_wed{ false };
            for (const auto& kv : e)
            {
                if (!kv.first || !kv.second) { ++null_kv; continue; }
                const std::string nm{ kv.first->name() };
                id_sum += kv.second->id();
                name_char_sum += code_unit_sum(nm);
                if (nm == "MON") { saw_mon = true; }
                if (nm == "TUE") { saw_tue = true; }
                if (nm == "WED") { saw_wed = true; }
            }
            ctx.check("cmap_enumkey_no_null_kv", null_kv == 0);
            ctx.check("cmap_enumkey_all_constants_present", saw_mon && saw_tue && saw_wed);
            ctx.check("cmap_enumkey_id_sum_matches_java",
                      id_sum == coll_map_fixture::j_long("hashEnumKeyIdSum"));
            ctx.check("cmap_enumkey_name_char_sum_matches_java",
                      name_char_sum == coll_map_fixture::j_long("hashEnumKeyNameCharSum"));
        }

        // =====================================================================
        // HashMap — RESIZE BOUNDARY at 16 and 17 entries.  With default capacity
        // 16 / load factor 0.75 the threshold is 12, so 13+ entries already forced
        // a resize to capacity 32.  These pin that the bucket walk visits EVERY
        // entry across a table that has been resized exactly at/after the
        // boundary (no entry dropped or duplicated during/after rehash).
        // =====================================================================
        {
            const auto e16{ coll_map_fixture::entries_of("hashResize16") };
            const entry_stats s16{ fingerprint(e16) };
            ctx.check("cmap_resize16_count_is_16", s16.count == RESIZE16_N);
            check_size_oracle("cmap_resize16", s16.count, "hashResize16Size");
            ctx.check("cmap_resize16_no_null_keys", s16.null_keys == 0);
            ctx.check("cmap_resize16_no_null_values", s16.null_values == 0);
            ctx.check("cmap_resize16_id_sum_matches_java",
                      s16.id_sum == coll_map_fixture::j_long("hashResize16IdSum"));
            // Closed form: sum 0..15 == 120.
            ctx.check("cmap_resize16_id_sum_closed_form",
                      s16.id_sum == (static_cast<std::int64_t>(RESIZE16_N) * (RESIZE16_N - 1)) / 2);
            // Every key "k0".."k15" present exactly once (no miss, no duplicate).
            std::vector<std::string> keys16{ keys_in_walk_order(e16) };
            std::sort(keys16.begin(), keys16.end());
            const bool unique16{ std::adjacent_find(keys16.begin(), keys16.end()) == keys16.end() };
            ctx.check("cmap_resize16_keys_unique_no_dup", unique16);
            bool all16_present{ keys16.size() == static_cast<std::size_t>(RESIZE16_N) };
            for (std::int32_t i{ 0 }; i < RESIZE16_N && all16_present; ++i)
            {
                if (!std::binary_search(keys16.begin(), keys16.end(), "k" + std::to_string(i)))
                {
                    all16_present = false;
                }
            }
            ctx.check("cmap_resize16_every_key_present", all16_present);
        }
        {
            const auto e17{ coll_map_fixture::entries_of("hashResize17") };
            const entry_stats s17{ fingerprint(e17) };
            ctx.check("cmap_resize17_count_is_17", s17.count == RESIZE17_N);
            check_size_oracle("cmap_resize17", s17.count, "hashResize17Size");
            ctx.check("cmap_resize17_no_null_keys", s17.null_keys == 0);
            ctx.check("cmap_resize17_no_null_values", s17.null_values == 0);
            ctx.check("cmap_resize17_id_sum_matches_java",
                      s17.id_sum == coll_map_fixture::j_long("hashResize17IdSum"));
            // Closed form: sum 0..16 == 136.
            ctx.check("cmap_resize17_id_sum_closed_form",
                      s17.id_sum == (static_cast<std::int64_t>(RESIZE17_N) * (RESIZE17_N - 1)) / 2);
            std::vector<std::string> keys17{ keys_in_walk_order(e17) };
            std::sort(keys17.begin(), keys17.end());
            const bool unique17{ std::adjacent_find(keys17.begin(), keys17.end()) == keys17.end() };
            ctx.check("cmap_resize17_keys_unique_no_dup", unique17);
            bool all17_present{ keys17.size() == static_cast<std::size_t>(RESIZE17_N) };
            for (std::int32_t i{ 0 }; i < RESIZE17_N && all17_present; ++i)
            {
                if (!std::binary_search(keys17.begin(), keys17.end(), "k" + std::to_string(i)))
                {
                    all17_present = false;
                }
            }
            ctx.check("cmap_resize17_every_key_present", all17_present);
        }

        // =====================================================================
        // HashMap with NESTED-MAP values.  The outer walk decodes each value OOP;
        // we then re-wrap that OOP as a vmhook::map and decode the INNER entries,
        // proving nested-Map values round-trip through a second to_entries.
        // =====================================================================
        {
            const auto e{ coll_map_fixture::entries_of_as<string_key, nested_value>("hashNestedMap") };
            ctx.check("cmap_nested_map_outer_count",
                      static_cast<std::int32_t>(e.size()) == NESTED_N);
            check_size_oracle("cmap_nested_map", static_cast<std::int32_t>(e.size()), "hashNestedMapSize");

            std::int32_t inner_total{ 0 };
            std::int64_t inner_id_sum{ 0 };
            bool outer_keys_ok{ true };
            bool all_inner_pairs_ok{ true };
            for (const auto& kv : e)
            {
                if (!kv.first || !kv.second) { outer_keys_ok = false; continue; }
                const std::string okey{ kv.first->text() };
                if (okey.size() < 1 || okey.front() != 'n') { outer_keys_ok = false; }
                // Re-wrap the nested value OOP as a Map and decode its entries.
                const auto inner{ vmhook::map{ kv.second->get_instance() }
                                      .to_entries<string_key, box_value>() };
                inner_total += static_cast<std::int32_t>(inner.size());
                for (const auto& ikv : inner)
                {
                    if (!ikv.first || !ikv.second) { all_inner_pairs_ok = false; continue; }
                    inner_id_sum += ikv.second->id();
                    // inner key "ik"+j, inner value name "iv"+id.
                    if (ikv.second->name() != ("iv" + std::to_string(ikv.second->id())))
                    {
                        all_inner_pairs_ok = false;
                    }
                }
            }
            ctx.check("cmap_nested_map_outer_keys_ok", outer_keys_ok);
            ctx.check("cmap_nested_map_inner_total_is_4", inner_total == (NESTED_N * NESTED_N));
            ctx.check("cmap_nested_map_inner_pairs_consistent", all_inner_pairs_ok);
            // ids are i*10+j over i,j in [0,NESTED_N): {0,1,10,11} -> sum 22.
            std::int64_t expect_inner_id_sum{ 0 };
            for (std::int32_t i{ 0 }; i < NESTED_N; ++i)
            {
                for (std::int32_t j{ 0 }; j < NESTED_N; ++j) { expect_inner_id_sum += (i * 10 + j); }
            }
            ctx.check("cmap_nested_map_inner_id_sum_ok", inner_id_sum == expect_inner_id_sum);
        }

        // =====================================================================
        // HashMap with NESTED-LIST values.  Same idea as nested-map, but each
        // value OOP is decoded as a vmhook::collection (ArrayList) -> to_vector.
        // =====================================================================
        {
            const auto e{ coll_map_fixture::entries_of_as<string_key, nested_value>("hashNestedList") };
            ctx.check("cmap_nested_list_outer_count",
                      static_cast<std::int32_t>(e.size()) == NESTED_N);
            check_size_oracle("cmap_nested_list", static_cast<std::int32_t>(e.size()), "hashNestedListSize");

            std::int32_t inner_total{ 0 };
            std::int64_t inner_id_sum{ 0 };
            bool all_inner_ok{ true };
            for (const auto& kv : e)
            {
                if (!kv.second) { all_inner_ok = false; continue; }
                const auto inner{ vmhook::collection{ kv.second->get_instance() }
                                      .to_vector<box_value>() };
                inner_total += static_cast<std::int32_t>(inner.size());
                for (const auto& el : inner)
                {
                    if (!el) { all_inner_ok = false; continue; }
                    inner_id_sum += el->id();
                    if (el->name() != ("lv" + std::to_string(el->id()))) { all_inner_ok = false; }
                }
            }
            ctx.check("cmap_nested_list_inner_total_is_4", inner_total == (NESTED_N * NESTED_N));
            ctx.check("cmap_nested_list_inner_consistent", all_inner_ok);
            std::int64_t expect_inner_id_sum{ 0 };
            for (std::int32_t i{ 0 }; i < NESTED_N; ++i)
            {
                for (std::int32_t j{ 0 }; j < NESTED_N; ++j) { expect_inner_id_sum += (i * 10 + j); }
            }
            ctx.check("cmap_nested_list_inner_id_sum_ok", inner_id_sum == expect_inner_id_sum);
        }

        // =====================================================================
        // HashMap<String,String> — NON-ASCII keys AND values.  Exercises
        // read_java_string's non-ASCII path: LATIN1 (coder 0) and UTF16 (coder 1)
        // on JDK 9+, and the char[] path on JDK 8 — all converge on the SAME UTF-8
        // output, so we assert EXACT UTF-8 string equality (a code-unit sum would
        // NOT cross-check, C++ sees UTF-8 bytes while Java sees UTF-16 units).
        //   U+00E9 -> C3 A9, U+00FC -> C3 BC, U+4E2D -> E4 B8 AD.
        // =====================================================================
        {
            const auto e{ coll_map_fixture::entries_of_as<string_key, string_value>("hashUnicode") };
            ctx.check("cmap_unicode_count_is_2", static_cast<std::int32_t>(e.size()) == 2);
            check_size_oracle("cmap_unicode", static_cast<std::int32_t>(e.size()), "hashUnicodeSize");

            // Expected exact UTF-8 byte sequences (kept as explicit bytes so the
            // SOURCE is pure-ASCII and the comparison is unambiguous on every host).
            const std::string e_acute{ "\xC3\xA9" };           // U+00E9
            const std::string u_umlaut{ "\xC3\xBC" };          // U+00FC
            const std::string cjk{ "\xE4\xB8\xAD" };           // U+4E2D
            const std::string e_acute_cjk{ e_acute + cjk };    // U+00E9 U+4E2D

            std::int32_t null_kv{ 0 };
            bool latin_pair_ok{ false };
            bool bmp_pair_ok{ false };
            for (const auto& kv : e)
            {
                if (!kv.first || !kv.second) { ++null_kv; continue; }
                const std::string k{ kv.first->text() };
                const std::string v{ kv.second->text() };
                if (k == e_acute && v == u_umlaut) { latin_pair_ok = true; }
                if (k == cjk && v == e_acute_cjk) { bmp_pair_ok = true; }
            }
            ctx.check("cmap_unicode_no_null_kv", null_kv == 0);
            ctx.check("cmap_unicode_latin1_pair_exact_utf8", latin_pair_ok);
            ctx.check("cmap_unicode_utf16_pair_exact_utf8", bmp_pair_ok);
        }

        // =====================================================================
        // HashMap<Character,Character> — boxed 16-bit char key AND value
        // (descriptor "C", UNSIGNED 16-bit).  Proves a Character.value round-trips.
        // 'A'(65)->'Z'(90), '0'(48)->'9'(57).
        // =====================================================================
        {
            const auto e{ coll_map_fixture::entries_of_as<char_box, char_box>("hashCharKey") };
            ctx.check("cmap_charkey_count_is_2", static_cast<std::int32_t>(e.size()) == 2);
            check_size_oracle("cmap_charkey", static_cast<std::int32_t>(e.size()), "hashCharKeySize");

            std::int64_t key_sum{ 0 }, val_sum{ 0 };
            std::int32_t null_kv{ 0 };
            bool pairs_ok{ true };
            for (const auto& kv : e)
            {
                if (!kv.first || !kv.second) { ++null_kv; pairs_ok = false; continue; }
                const std::int32_t k{ kv.first->value() };
                const std::int32_t v{ kv.second->value() };
                key_sum += k;
                val_sum += v;
                // 'A'->'Z' (delta 25); '0'->'9' (delta 9).
                if (!((k == 'A' && v == 'Z') || (k == '0' && v == '9'))) { pairs_ok = false; }
            }
            ctx.check("cmap_charkey_no_null_kv", null_kv == 0);
            ctx.check("cmap_charkey_pairs_consistent", pairs_ok);
            ctx.check("cmap_charkey_key_sum_matches_java",
                      key_sum == coll_map_fixture::j_long("hashCharKeyKeySum"));
            ctx.check("cmap_charkey_val_sum_matches_java",
                      val_sum == coll_map_fixture::j_long("hashCharKeyValSum"));
            ctx.check("cmap_charkey_key_sum_closed_form", key_sum == ('A' + '0'));
        }

        // =====================================================================
        // HashMap<Short,Short> — boxed SIGNED 16-bit key AND value ("S").  Keys
        // include a NEGATIVE short (-1); proves sign extension on a narrow field.
        // =====================================================================
        {
            const auto e{ coll_map_fixture::entries_of_as<short_box, short_box>("hashShortKey") };
            ctx.check("cmap_shortkey_count_is_3", static_cast<std::int32_t>(e.size()) == SMALL_N);
            check_size_oracle("cmap_shortkey", static_cast<std::int32_t>(e.size()), "hashShortKeySize");

            std::int64_t key_sum{ 0 }, val_sum{ 0 };
            std::int32_t null_kv{ 0 };
            bool saw_negative_key{ false };
            bool pairs_ok{ true };
            for (const auto& kv : e)
            {
                if (!kv.first || !kv.second) { ++null_kv; pairs_ok = false; continue; }
                const std::int32_t k{ kv.first->value() };
                const std::int32_t v{ kv.second->value() };
                key_sum += k;
                val_sum += v;
                if (k < 0) { saw_negative_key = true; }
                // value == key + 1 by construction.
                if (v != (k + 1)) { pairs_ok = false; }
            }
            ctx.check("cmap_shortkey_no_null_kv", null_kv == 0);
            ctx.check("cmap_shortkey_negative_key_present", saw_negative_key);
            ctx.check("cmap_shortkey_pairs_consistent", pairs_ok);
            ctx.check("cmap_shortkey_key_sum_matches_java",
                      key_sum == coll_map_fixture::j_long("hashShortKeyKeySum"));
            ctx.check("cmap_shortkey_val_sum_matches_java",
                      val_sum == coll_map_fixture::j_long("hashShortKeyValSum"));
        }

        // =====================================================================
        // HashMap<Byte,Byte> — boxed SIGNED 8-bit key AND value ("B").  Keys
        // include a NEGATIVE byte (-1); proves sign extension on a 1-byte field.
        // =====================================================================
        {
            const auto e{ coll_map_fixture::entries_of_as<byte_box, byte_box>("hashByteKey") };
            ctx.check("cmap_bytekey_count_is_3", static_cast<std::int32_t>(e.size()) == SMALL_N);
            check_size_oracle("cmap_bytekey", static_cast<std::int32_t>(e.size()), "hashByteKeySize");

            std::int64_t key_sum{ 0 }, val_sum{ 0 };
            std::int32_t null_kv{ 0 };
            bool saw_negative_key{ false };
            bool pairs_ok{ true };
            for (const auto& kv : e)
            {
                if (!kv.first || !kv.second) { ++null_kv; pairs_ok = false; continue; }
                const std::int32_t k{ kv.first->value() };
                const std::int32_t v{ kv.second->value() };
                key_sum += k;
                val_sum += v;
                if (k < 0) { saw_negative_key = true; }
                // value == key - 1 by construction (a (byte) cast wrap is fine: the
                // sums cross-check against Java's identical (byte) arithmetic).
                if (static_cast<std::int8_t>(v) != static_cast<std::int8_t>(k - 1)) { pairs_ok = false; }
            }
            ctx.check("cmap_bytekey_no_null_kv", null_kv == 0);
            ctx.check("cmap_bytekey_negative_key_present", saw_negative_key);
            ctx.check("cmap_bytekey_pairs_consistent", pairs_ok);
            ctx.check("cmap_bytekey_key_sum_matches_java",
                      key_sum == coll_map_fixture::j_long("hashByteKeyKeySum"));
            ctx.check("cmap_bytekey_val_sum_matches_java",
                      val_sum == coll_map_fixture::j_long("hashByteKeyValSum"));
        }

        // =====================================================================
        // HashMap<Boolean,Box> — boxed boolean key ("Z").  Exactly FALSE and TRUE.
        // =====================================================================
        {
            const auto e{ coll_map_fixture::entries_of_as<bool_box, box_value>("hashBoolKey") };
            ctx.check("cmap_boolkey_count_is_2", static_cast<std::int32_t>(e.size()) == 2);
            check_size_oracle("cmap_boolkey", static_cast<std::int32_t>(e.size()), "hashBoolKeySize");

            std::int32_t null_kv{ 0 };
            bool saw_false{ false }, saw_true{ false }, pairs_ok{ true };
            for (const auto& kv : e)
            {
                if (!kv.first || !kv.second) { ++null_kv; pairs_ok = false; continue; }
                if (!kv.first->value())
                {
                    saw_false = true;
                    if (kv.second->id() != 0 || kv.second->name() != "false-v") { pairs_ok = false; }
                }
                else
                {
                    saw_true = true;
                    if (kv.second->id() != 1 || kv.second->name() != "true-v") { pairs_ok = false; }
                }
            }
            ctx.check("cmap_boolkey_no_null_kv", null_kv == 0);
            ctx.check("cmap_boolkey_both_constants_present", saw_false && saw_true);
            ctx.check("cmap_boolkey_pairs_consistent", pairs_ok);
        }

        // =====================================================================
        // HashMap<String,Box> — EXTREME / NEGATIVE value ids (INT_MIN, -1, 0,
        // INT_MAX).  Proves the signed 32-bit "id" field round-trips across the
        // FULL int range with no truncation or sign error.
        // =====================================================================
        {
            const auto e{ coll_map_fixture::entries_of("hashNegIds") };
            ctx.check("cmap_negids_count_is_4", static_cast<std::int32_t>(e.size()) == 4);
            check_size_oracle("cmap_negids", static_cast<std::int32_t>(e.size()), "hashNegIdsSize");

            std::int64_t id_sum{ 0 };
            std::int32_t null_kv{ 0 };
            bool saw_min{ false }, saw_max{ false }, saw_neg_one{ false };
            for (const auto& kv : e)
            {
                if (!kv.first || !kv.second) { ++null_kv; continue; }
                const std::int32_t id{ kv.second->id() };
                id_sum += id;
                if (id == (std::numeric_limits<std::int32_t>::min)()) { saw_min = true; }
                if (id == (std::numeric_limits<std::int32_t>::max)()) { saw_max = true; }
                if (id == -1) { saw_neg_one = true; }
            }
            ctx.check("cmap_negids_no_null_kv", null_kv == 0);
            ctx.check("cmap_negids_has_int_min", saw_min);
            ctx.check("cmap_negids_has_int_max", saw_max);
            ctx.check("cmap_negids_has_minus_one", saw_neg_one);
            ctx.check("cmap_negids_id_sum_matches_java",
                      id_sum == coll_map_fixture::j_long("hashNegIdsIdSum"));
            // Closed form: INT_MIN + (-1) + 0 + INT_MAX == -2 (the +/- cancel,
            // leaving INT_MIN+INT_MAX == -1, plus -1).
            ctx.check("cmap_negids_id_sum_closed_form",
                      id_sum == (static_cast<std::int64_t>((std::numeric_limits<std::int32_t>::min)())
                                 + (std::numeric_limits<std::int32_t>::max)() + (-1) + 0));
        }

        // =====================================================================
        // EXPLICIT vmhook::map WRAPPER API — size() / is_empty() / direct
        // to_entries().  These exercise the map wrapper's OWN surface (distinct
        // from the implicit value_t::to_entries call site the rest of the module
        // uses), mirroring the sibling collection_hash_tree_map module.  Uses the
        // already-built hashSmall / treeSmall / hashEmpty / treeEmpty — no new heap.
        // =====================================================================
        {
            const auto hm{ coll_map_fixture::acquire_map("hashSmall") };
            ctx.check("cmap_wrapper_hashsmall_acquired", hm != nullptr);
            if (hm)
            {
                ctx.check("cmap_wrapper_hashsmall_size_is_3", hm->size() == SMALL_N);
                ctx.check("cmap_wrapper_hashsmall_not_empty", hm->is_empty() == false);
                const auto e{ hm->to_entries<string_key, box_value>() };
                ctx.check("cmap_wrapper_hashsmall_direct_to_entries_count",
                          static_cast<std::int32_t>(e.size()) == SMALL_N);
                // The explicit-wrapper walk must AGREE with the implicit path.
                const entry_stats st{ fingerprint(e) };
                ctx.check("cmap_wrapper_hashsmall_id_sum_matches_java",
                          st.id_sum == coll_map_fixture::j_long("hashSmallIdSum"));
            }

            const auto tm{ coll_map_fixture::acquire_map("treeSmall") };
            ctx.check("cmap_wrapper_treesmall_acquired", tm != nullptr);
            if (tm)
            {
                ctx.check("cmap_wrapper_treesmall_size_is_3", tm->size() == SMALL_N);
                ctx.check("cmap_wrapper_treesmall_not_empty", tm->is_empty() == false);
                const auto e{ tm->to_entries<string_key, box_value>() };
                ctx.check("cmap_wrapper_treesmall_direct_to_entries_count",
                          static_cast<std::int32_t>(e.size()) == SMALL_N);
                const std::vector<std::string> keys{ keys_in_walk_order(e) };
                ctx.check("cmap_wrapper_treesmall_keys_sorted",
                          std::is_sorted(keys.begin(), keys.end()));
            }

            // Empty maps: Java size()==0, is_empty()==true, to_entries empty.
            const auto he{ coll_map_fixture::acquire_map("hashEmpty") };
            ctx.check("cmap_wrapper_hashempty_acquired", he != nullptr);
            if (he)
            {
                ctx.check("cmap_wrapper_hashempty_size_zero", he->size() == 0);
                ctx.check("cmap_wrapper_hashempty_is_empty_true", he->is_empty());
                ctx.check("cmap_wrapper_hashempty_to_entries_empty",
                          he->to_entries<string_key, box_value>().empty());
            }

            const auto te{ coll_map_fixture::acquire_map("treeEmpty") };
            ctx.check("cmap_wrapper_treeempty_acquired", te != nullptr);
            if (te)
            {
                ctx.check("cmap_wrapper_treeempty_size_zero", te->size() == 0);
                ctx.check("cmap_wrapper_treeempty_is_empty_true", te->is_empty());
                ctx.check("cmap_wrapper_treeempty_to_entries_empty",
                          te->to_entries<string_key, box_value>().empty());
            }
        }

        // =====================================================================
        // field_proxy::value_t::is_reference() — a populated Map field holds a
        // reference oop (true); a primitive size-witness field does not (false).
        // =====================================================================
        {
            // A populated Map field reads as a reference oop; a primitive int
            // size-witness field does NOT (it is the int32 value_t alternative);
            // a (non-null) String reference field also reads as a reference.
            ctx.check("cmap_map_field_is_reference", coll_map_fixture::field_is_reference("hashSmall"));
            ctx.check("cmap_int_field_not_reference",
                      coll_map_fixture::field_is_reference("hashSmallSize") == false);
            ctx.check("cmap_string_field_is_reference", coll_map_fixture::field_is_reference("notAMap"));
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
        // TreeMap — REVERSE COMPARATOR.  Built with Collections.reverseOrder(), so
        // a correct in-order red-black walk emits keys in the COMPARATOR's order:
        // DESCENDING ("k2","k1","k0").  Proves the walk honours the tree's
        // comparator rather than assuming natural ordering.  Same content as
        // treeSmall, so the value fingerprint still cross-checks.
        // =====================================================================
        {
            const auto e{ coll_map_fixture::entries_of("treeReverseComparator") };
            const entry_stats st{ fingerprint(e) };
            ctx.check("tree_revcmp_count_is_3", st.count == SMALL_N);
            check_size_oracle("tree_revcmp", st.count, "treeReverseComparatorSize");
            ctx.check("tree_revcmp_no_null_keys", st.null_keys == 0);
            ctx.check("tree_revcmp_no_null_values", st.null_values == 0);
            ctx.check("tree_revcmp_id_sum_is_3", st.id_sum == (0 + 1 + 2));

            const std::vector<std::string> keys{ keys_in_walk_order(e) };
            // In-order over a reverse comparator => DESCENDING (NOT std::is_sorted).
            const bool descending{ std::is_sorted(keys.begin(), keys.end(),
                                                  std::greater<std::string>{}) };
            ctx.check("tree_revcmp_walk_is_descending", descending);
            ctx.check("tree_revcmp_first_is_k2", !keys.empty() && keys.front() == "k2");
            ctx.check("tree_revcmp_last_is_k0", !keys.empty() && keys.back() == "k0");
            // The walk's first/last must match Java's comparator-ordered first/last.
            ctx.check("tree_revcmp_first_matches_java",
                      !keys.empty() && keys.front() == coll_map_fixture::j_string("treeReverseComparatorFirstKey"));
            ctx.check("tree_revcmp_last_matches_java",
                      !keys.empty() && keys.back() == coll_map_fixture::j_string("treeReverseComparatorLastKey"));
        }

        // =====================================================================
        // TreeMap<Integer,Box> — NUMERIC key order.  Keys 10,2,1 inserted; the
        // in-order walk emits the natural NUMERIC comparator order 1,2,10 — which
        // is DISTINCT from the lexicographic "10"<"2" a String key would give.
        // Proves the in-order walk emits boxed-Integer numeric ordering.
        // =====================================================================
        {
            const auto e{ coll_map_fixture::entries_of_as<integer_box, box_value>("treeIntKey") };
            ctx.check("tree_intkey_count_is_3", static_cast<std::int32_t>(e.size()) == SMALL_N);
            check_size_oracle("tree_intkey", static_cast<std::int32_t>(e.size()), "treeIntKeySize");

            std::vector<std::int32_t> keys;
            keys.reserve(e.size());
            bool pairs_ok{ true };
            std::int32_t null_kv{ 0 };
            for (const auto& kv : e)
            {
                if (!kv.first || !kv.second) { ++null_kv; pairs_ok = false; continue; }
                const std::int32_t k{ kv.first->value() };
                keys.push_back(k);
                // value.id == numeric key, name == "v"+key.
                if (kv.second->id() != k) { pairs_ok = false; }
                if (kv.second->name() != ("v" + std::to_string(k))) { pairs_ok = false; }
            }
            ctx.check("tree_intkey_no_null_kv", null_kv == 0);
            ctx.check("tree_intkey_pairs_consistent", pairs_ok);
            ctx.check("tree_intkey_numeric_ascending", std::is_sorted(keys.begin(), keys.end()));
            ctx.check("tree_intkey_first_is_1", !keys.empty() && keys.front() == 1);
            ctx.check("tree_intkey_last_is_10", !keys.empty() && keys.back() == 10);
            // The numeric order 1,2,10 differs from lexicographic "1","10","2" —
            // pin that the middle key is 2 (a String tree would put "10" there).
            ctx.check("tree_intkey_middle_is_2", keys.size() == 3 && keys[1] == 2);
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
        // ConcurrentHashMap — CHARACTERIZED EMPTY.  CHM HAS a "table" field, so
        // the HashMap fast path is SELECTED, but its Node names the value field
        // "val" (not "value"); the walker's find_field(node,"value") misses and
        // the bucket bails, so to_entries reads EMPTY for BOTH small and MANY.
        // Java size() is non-zero, proving these are genuinely populated maps.
        // (If a future walker learns the "val" alias, these flip to populated —
        // a deliberate tripwire pinned here with the size witnesses.)
        // =====================================================================
        {
            const auto e_small{ coll_map_fixture::entries_of("chmSmall") };
            ctx.check("cmap_chm_small_decodes_empty", e_small.empty());
            ctx.check("cmap_chm_small_java_size_is_3",
                      coll_map_fixture::j_size("chmSmallSize") == SMALL_N);

            const auto e_many{ coll_map_fixture::entries_of("chmMany") };
            ctx.check("cmap_chm_many_decodes_empty", e_many.empty());
            ctx.check("cmap_chm_many_java_size_is_1000",
                      coll_map_fixture::j_size("chmManySize") == MANY_N);
            ctx.record("[INFO] cmap_chm: ConcurrentHashMap has a \"table\" but its Node "
                       "uses field \"val\" not \"value\"; the hash walk finds no \"value\" "
                       "field and returns EMPTY. Characterized contract (size witness "
                       "proves the map is non-empty Java-side), not a crash.");
        }

        // =====================================================================
        // WeakHashMap / IdentityHashMap — CHARACTERIZED EMPTY (both HAVE "table").
        //   WeakHashMap.Entry holds the key as the WeakReference REFERENT (no
        //     "key" field) -> find_field(node,"key") misses -> EMPTY.
        //   IdentityHashMap.table is a FLAT Object[] of alternating key,value
        //     (no Node objects) -> a bucket element is a bare String/Box, not a
        //     Node -> find_field(element,"key") misses -> EMPTY.
        // Both keys are strongly held (weakSmall via keyHolder) so nothing is
        // GC-cleared mid-read; the size witnesses prove they are non-empty.
        // =====================================================================
        {
            const auto e_weak{ coll_map_fixture::entries_of("weakSmall") };
            ctx.check("cmap_weak_small_decodes_empty", e_weak.empty());
            ctx.check("cmap_weak_small_java_size_is_3",
                      coll_map_fixture::j_size("weakSmallSize") == SMALL_N);

            const auto e_identity{ coll_map_fixture::entries_of("identitySmall") };
            ctx.check("cmap_identity_small_decodes_empty", e_identity.empty());
            ctx.check("cmap_identity_small_java_size_is_3",
                      coll_map_fixture::j_size("identitySmallSize") == SMALL_N);
            ctx.record("[INFO] cmap_weak/identity: WeakHashMap.Entry has no \"key\" field "
                       "(key is the WeakReference referent) and IdentityHashMap.table is a "
                       "flat alternating-k/v Object[] (no Node), so both decode EMPTY "
                       "despite owning a \"table\". Characterized, size witnesses non-zero.");
        }

        // =====================================================================
        // EnumMap — CHARACTERIZED EMPTY.  EnumMap stores values in a parallel
        // "vals" Object[] keyed by ordinal and exposes NEITHER "table" NOR
        // "root", so the dispatcher finds no fast path -> EMPTY.
        // =====================================================================
        {
            const auto e_enum{ coll_map_fixture::entries_of("enumSmall") };
            ctx.check("cmap_enum_small_decodes_empty", e_enum.empty());
            ctx.check("cmap_enum_small_java_size_is_3",
                      coll_map_fixture::j_size("enumSmallSize") == SMALL_N);
            ctx.record("[INFO] cmap_enum: EnumMap exposes neither \"table\" nor \"root\" "
                       "(values live in a parallel \"vals\" Object[] by ordinal), so "
                       "to_entries reads EMPTY. Characterized, size witness == 3.");
        }

        // =====================================================================
        // Map.of(...) immutable maps (JDK 9+) — CHARACTERIZED EMPTY.
        //   MapN (3 entries): "table" is an Object[] of INTERLEAVED key,value
        //     (not a Node[]), so the hash fast path is selected but every slot is
        //     a bare String/Box, never a Node -> EMPTY.
        //   Map1 (1 entry): fields k0/v0, no "table"/"root" -> EMPTY.
        // Built reflectively in the fixture so JDK 8 compiles; on a pre-9 JVM the
        // field is null (size witness -1) and to_entries is empty too — still the
        // characterized contract.  We gate the size assertion on JDK 9+.
        // =====================================================================
        {
            const std::int32_t mapN_size{ coll_map_fixture::j_size("mapOfNSize") };
            const std::int32_t map1_size{ coll_map_fixture::j_size("mapOf1Size") };
            const bool have_map_of{ mapN_size >= 0 };   // -1 => Map.of unavailable (JDK 8)

            const auto e_mapN{ coll_map_fixture::entries_of("mapOfN") };
            ctx.check("cmap_mapof_n_decodes_empty", e_mapN.empty());
            const auto e_map1{ coll_map_fixture::entries_of("mapOf1") };
            ctx.check("cmap_mapof_1_decodes_empty", e_map1.empty());

            if (have_map_of)
            {
                ctx.check("cmap_mapof_n_java_size_is_3", mapN_size == SMALL_N);
                ctx.check("cmap_mapof_1_java_size_is_1", map1_size == 1);
                ctx.record("[INFO] cmap_mapof: Map.of MapN \"table\" is an interleaved "
                           "k/v Object[] (not a Node[]) and Map1 uses k0/v0; neither "
                           "exposes a walkable Node, so both decode EMPTY. Size "
                           "witnesses (3 / 1) confirm they are non-empty Java-side.");
            }
            else
            {
                ctx.record("[INFO] cmap_mapof: Map.of unavailable on this JDK (pre-9); "
                           "fields are null and decode empty (still the contract).");
            }
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
