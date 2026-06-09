// collection_hash_tree_map JVM test module  (feature area: collections)
//
// THE Map-walker authority for the two HotSpot container layouts vmhook decodes
// from a raw OOP without a single Java call-gate dispatch:
//
//   * java.util.HashMap   -> the "table" Node[] BUCKET walk, reached through the
//                            EXPLICIT vmhook::hash_map wrapper (the typed
//                            intent-declaring path:
//                            `std::unique_ptr<vmhook::hash_map> = proxy->get()`),
//                            and cross-checked through value_t::to_entries.
//   * java.util.TreeMap   -> the "root" red-black IN-ORDER walk, reached through
//                            the generic vmhook::map wrapper.  The fixture inserts
//                            the keys OUT OF NATURAL ORDER (t2,t0,t1) so a correct
//                            in-order traversal proving itself is non-trivial.
//
// This is the live-JVM successor to the legacy Example.test_hash_map_probe /
// Example.test_tree_map_probe pair (vmhook/src/example.cpp).  The maps are
// HashMap<String,String> / TreeMap<String,String>, so BOTH the key and value
// OOPs decode directly through vmhook::read_java_string and the module asserts
// exact value CONTENT, not just identity.
//
// What this module proves on a live JVM (Java 8/11/17/21/24/25 x MSVC/Clang/GCC):
//   * hash_map::size() and map::size() == 3 for both maps (Java size() via the
//     live OOP's klass), matching Java's own published size() witness.
//   * hash_map::to_entries<K,V>() returns all 3 HashMap (key,value) pairs with
//     correct values, ORDER-INDEPENDENT (HashMap iteration is bucket order):
//     h0->hash-zero, h1->hash-one, h2->hash-two.
//   * map::to_entries<K,V>() over the TreeMap returns the 3 entries in NATURAL
//     SORTED key order t0 < t1 < t2 (proving the red-black in-order walk
//     re-sorted the out-of-order insertion), each with the correct value.
//   * value_t::to_entries (the implicit field-proxy path) and the explicit
//     wrapper path agree for the HashMap.
//   * the TreeMap first/last keys the native walk yields match Java's
//     firstKey()/lastKey().
//
// CHARACTERIZED ([INFO], not a failure): vmhook routes BOTH containers through
// the SAME vmhook::map::to_entries (HashMap "table" path, then TreeMap "root"
// path); vmhook::hash_map adds NO distinct hash-specific traversal — it is a
// pure intent tag whose to_entries IS map::to_entries.  The contents/ordering
// assertions below hold regardless.
//
// SAFETY: there are no hooks in this feature (only reads), so there is nothing
// to tear down.  Every key/value OOP is gated with is_valid_pointer before any
// deref.  All value_t extractions are COPY-INIT (never brace-init) to stay
// MSVC-unambiguous, and every static accessor goes through static_field /
// static_method (the portable GCC path).
//
// Harness shape mirrors field_static: register_class, a `mode` selector with a
// `done` reset on the rising edge of go, and a dense battery of ctx.check()s.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace
{
    // Key/value wrapper for the Map entries.  Plain vmhook::object<> — the entry
    // OOPs are java.lang.String, decoded via read_java_string(get_instance()).
    using str_oop = vmhook::object<>;

    // The vector type both walk paths return.
    using entries_t = std::vector<std::pair<std::unique_ptr<str_oop>, std::unique_ptr<str_oop>>>;

    // Wrapper for vmhook.fixtures.HashTreeMap.
    //
    // EVERY accessor here is a STATIC method reaching the field through
    // static_field(...) / static_method(...): the portable path that also
    // compiles on GCC (the deducing-this get_field overloads are non-viable from
    // a static context there).
    class htm : public vmhook::object<htm>
    {
    public:
        explicit htm(vmhook::oop_t instance) noexcept
            : vmhook::object<htm>{ instance }
        {
        }

        // ---- handshake + scenario selector (all via static_field) ----
        static auto set_go(bool value) -> void      { static_field("go")->set(value); }
        static auto set_done(bool value) -> void     { static_field("done")->set(value); }
        static auto get_done() -> bool               { return static_field("done")->get(); }
        static auto set_mode(std::int32_t m) -> void { static_field("mode")->set(m); }

        // ---- resolve helper ----
        static auto resolves(const char* name) -> bool
        {
            return static_field(name).has_value();
        }

        // ---- Java's own published witnesses ----
        static auto java_hash_size() -> std::int32_t { return static_field("hashMapSize")->get(); }
        static auto java_tree_size() -> std::int32_t { return static_field("treeMapSize")->get(); }
        static auto java_tree_first_key() -> std::string { return static_field("treeFirstKey")->get().as_string(); }
        static auto java_tree_last_key() -> std::string { return static_field("treeLastKey")->get().as_string(); }

        // ---- the HashMap via the EXPLICIT vmhook::hash_map wrapper ----
        // value_t -> unique_ptr<hash_map> is the typed intent-declaring path the
        // legacy test_hash_map_probe used (get<unique_ptr<hash_map>>()).
        static auto acquire_hash_map() -> std::unique_ptr<vmhook::hash_map> { return static_field("hashMap")->get(); }

        // ---- the TreeMap via the generic vmhook::map wrapper ----
        static auto acquire_tree_map() -> std::unique_ptr<vmhook::map> { return static_field("treeMap")->get(); }

        // ---- the HashMap via the IMPLICIT value_t::to_entries path ----
        // The cross-check that the field-proxy convenience path agrees with the
        // explicit wrapper.  to_entries returns the unique_ptr-pair vector.
        static auto hash_entries_via_value_t() -> entries_t
        {
            const auto p{ static_field("hashMap") };
            if (!p.has_value()) { return entries_t{}; }
            return p->get().to_entries<str_oop, str_oop>();
        }
    };

    // Decode an entry-side String wrapper to std::string, gating the deref with
    // is_valid_pointer (SAFETY).  A null/invalid OOP yields the sentinel so a
    // missing entry can never masquerade as a real value.
    auto decode_entry(const std::unique_ptr<str_oop>& w) -> std::string
    {
        if (!w)
        {
            return std::string{ "<<null>>" };
        }
        void* const oop{ w->get_instance() };
        if (!oop || !vmhook::hotspot::is_valid_pointer(oop))
        {
            return std::string{ "<<invalid>>" };
        }
        return vmhook::read_java_string(oop);
    }

    // Drive one probe cycle for `mode`: clears the latched `done` and programs
    // the selector on the rising edge of go, then waits for done.
    auto drive(vmhook_test::context& ctx, std::int32_t mode) -> bool
    {
        return ctx.run_probe(
            [mode](bool value)
            {
                if (value)
                {
                    htm::set_done(false);
                    htm::set_mode(mode);
                }
                htm::set_go(value);
            },
            []() { return htm::get_done(); });
    }
}

VMHOOK_JVM_MODULE(collection_hash_tree_map)
{
    vmhook::register_class<htm>("vmhook/fixtures/HashTreeMap");

    // =====================================================================
    //  0. Sanity: the class resolves and the map fields are reachable as
    //     static reference fields through the portable accessor.
    // =====================================================================
    ctx.check("htm_class_registered_field_resolves", htm::resolves("hashMap"));
    ctx.check("htm_treeMap_field_resolves", htm::resolves("treeMap"));

    {
        const auto p{ htm::static_field("hashMap") };
        if (p)
        {
            // A HashMap<String,String> field is an object reference: signature
            // "Ljava/util/HashMap;" and is_static()==true.
            ctx.check("hashMap_proxy_is_static_true", p->is_static() == true);
            ctx.check("hashMap_proxy_signature_is_ref",
                      !std::string{ p->signature() }.empty()
                          && std::string{ p->signature() }.front() == 'L');
        }
    }

    // Characterize the routing up-front: hash_map and the generic map share the
    // SAME to_entries (HashMap "table" path then TreeMap "root" path); the
    // hash_map wrapper is a pure intent tag, adding no distinct traversal.
    ctx.record("[INFO] collection_hash_tree_map: vmhook routes BOTH HashMap and "
               "TreeMap through the same vmhook::map::to_entries (HashMap \"table\" "
               "bucket walk, then TreeMap \"root\" red-black walk); vmhook::hash_map "
               "adds no hash-specific traversal -- it is a typed intent tag whose "
               "to_entries IS map::to_entries. Key/value contents and TreeMap sorted "
               "order are asserted regardless.");

    // =====================================================================
    //  1. Build both maps on the Java thread (mode 0), so the native reads
    //     below see a fresh, deterministic population published this tick.
    // =====================================================================
    const bool built{ drive(ctx, 0) };
    ctx.check("build_probe_completed", built);

    // Java's own view of the sizes (published by the fixture's buildAll()).
    ctx.check("java_hashMapSize_is_3", htm::java_hash_size() == 3);
    ctx.check("java_treeMapSize_is_3", htm::java_tree_size() == 3);
    // The TreeMap's sorted bounds, straight from Java's firstKey()/lastKey().
    ctx.check("java_treeFirstKey_is_t0", htm::java_tree_first_key() == "t0");
    ctx.check("java_treeLastKey_is_t2",  htm::java_tree_last_key() == "t2");

    // =====================================================================
    //  2. HASHMAP via the EXPLICIT vmhook::hash_map wrapper.
    //     size()==3; to_entries yields all 3 (key,value) pairs with correct
    //     values, ORDER-INDEPENDENT (HashMap iteration is bucket order).
    // =====================================================================
    {
        const auto hm{ htm::acquire_hash_map() };
        ctx.check("hash_map_wrapper_acquired", hm != nullptr);

        if (hm)
        {
            ctx.check("hash_map_size_is_3", hm->size() == 3);
            ctx.check("hash_map_not_empty", hm->is_empty() == false);

            const entries_t entries{ hm->to_entries<str_oop, str_oop>() };
            ctx.check("hash_map_entries_size_is_3",
                      static_cast<std::int32_t>(entries.size()) == 3);

            // Order-independent presence+value check for h0/h1/h2.
            std::array<bool, 3> seen{ false, false, false };
            bool values_ok{ true };
            bool keys_ok{ true };
            for (const auto& kv : entries)
            {
                const std::string key{ decode_entry(kv.first) };
                const std::string val{ decode_entry(kv.second) };
                if      (key == "h0") { seen[0] = true; if (val != "hash-zero") { values_ok = false; } }
                else if (key == "h1") { seen[1] = true; if (val != "hash-one")  { values_ok = false; } }
                else if (key == "h2") { seen[2] = true; if (val != "hash-two")  { values_ok = false; } }
                else                  { keys_ok = false; }
            }
            ctx.check("hash_map_all_keys_present", seen[0] && seen[1] && seen[2]);
            ctx.check("hash_map_no_unexpected_keys", keys_ok);
            ctx.check("hash_map_all_values_correct", values_ok);
        }
    }

    // =====================================================================
    //  3. HASHMAP via the IMPLICIT value_t::to_entries path -- must AGREE with
    //     the explicit wrapper (the field-proxy convenience path is correct).
    // =====================================================================
    {
        const entries_t entries{ htm::hash_entries_via_value_t() };
        ctx.check("hash_map_value_t_entries_size_is_3",
                  static_cast<std::int32_t>(entries.size()) == 3);

        std::array<bool, 3> seen{ false, false, false };
        bool pairs_ok{ true };
        for (const auto& kv : entries)
        {
            const std::string key{ decode_entry(kv.first) };
            const std::string val{ decode_entry(kv.second) };
            if      (key == "h0") { seen[0] = true; if (val != "hash-zero") { pairs_ok = false; } }
            else if (key == "h1") { seen[1] = true; if (val != "hash-one")  { pairs_ok = false; } }
            else if (key == "h2") { seen[2] = true; if (val != "hash-two")  { pairs_ok = false; } }
            else                  { pairs_ok = false; }
        }
        ctx.check("hash_map_value_t_path_agrees",
                  seen[0] && seen[1] && seen[2] && pairs_ok);
    }

    // =====================================================================
    //  4. TREEMAP via the generic vmhook::map wrapper.
    //     size()==3; to_entries MUST come out in NATURAL SORTED key order
    //     t0 < t1 < t2 (the keys were inserted OUT OF ORDER t2,t0,t1, so this
    //     proves the red-black IN-ORDER walk), each with the correct value.
    // =====================================================================
    {
        const auto tm{ htm::acquire_tree_map() };
        ctx.check("tree_map_wrapper_acquired", tm != nullptr);

        if (tm)
        {
            ctx.check("tree_map_size_is_3", tm->size() == 3);
            ctx.check("tree_map_not_empty", tm->is_empty() == false);

            const entries_t entries{ tm->to_entries<str_oop, str_oop>() };
            ctx.check("tree_map_entries_size_is_3",
                      static_cast<std::int32_t>(entries.size()) == 3);

            // Pin the FULL ordered sequence: the i-th entry must be exactly
            // t{i} -> tree-{word(i)}.  This is the strongest in-order proof.
            static constexpr std::array<const char*, 3> expect_key{ "t0", "t1", "t2" };
            static constexpr std::array<const char*, 3> expect_val{ "tree-zero", "tree-one", "tree-two" };

            bool order_ok{ true };
            bool values_ok{ true };
            std::array<bool, 3> seen{ false, false, false };
            for (std::size_t i{ 0 }; i < entries.size(); ++i)
            {
                const std::string key{ decode_entry(entries[i].first) };
                const std::string val{ decode_entry(entries[i].second) };
                if      (key == "t0") { seen[0] = true; }
                else if (key == "t1") { seen[1] = true; }
                else if (key == "t2") { seen[2] = true; }
                // Ordered position check: entry i must be t{i}.
                if (i < expect_key.size())
                {
                    if (key != expect_key[i]) { order_ok = false; }
                    if (val != expect_val[i]) { values_ok = false; }
                }
            }
            ctx.check("tree_map_all_keys_present", seen[0] && seen[1] && seen[2]);
            ctx.check("tree_map_in_order_sorted_t0_t1_t2", order_ok);
            ctx.check("tree_map_all_values_correct_in_order", values_ok);

            // Cross-check the native walk's bounds against Java's firstKey/lastKey.
            if (entries.size() == 3)
            {
                const std::string first{ decode_entry(entries.front().first) };
                const std::string last{ decode_entry(entries.back().first) };
                ctx.check("tree_map_native_first_matches_java",
                          first == htm::java_tree_first_key());
                ctx.check("tree_map_native_last_matches_java",
                          last == htm::java_tree_last_key());
            }
        }
    }

    // =====================================================================
    //  5. EDGE: a null / non-existent Map field must walk to EMPTY, never crash.
    //     (Guards the to_entries null-OOP and missing-field paths the audit
    //     flagged; mirrors the legacy "map left null" coverage.)
    // =====================================================================
    {
        // A field that does not exist -> static_field is nullopt; the wrapper
        // helpers return nullptr and the walks stay empty.
        ctx.check("nonexistent_map_field_is_nullopt",
                  htm::static_field("noSuchMap").has_value() == false);

        // value_t::to_entries on a non-Map String field ("treeFirstKey" holds a
        // plain String, not a Map): the "table"/"root" probes both miss, so the
        // result is an empty entries vector (never a throw / wild walk).
        const auto p{ htm::static_field("treeFirstKey") };
        if (p)
        {
            const entries_t entries{ p->get().to_entries<str_oop, str_oop>() };
            ctx.check("non_map_field_to_entries_is_empty", entries.empty());
        }
    }
}
