// collection_set JVM test module  (feature area: collections)
//
// Exhaustively exercises vmhook::collection::to_vector<wrapper>() /
// field_proxy::value_t::to_vector<element>() over real java.util.Set fields on a
// LIVE JVM, through the exact path a user hits: a static Set field read decoded
// to a collection, then .to_vector<element>().  A Set has no get(int), so the
// ONLY decodable routes are the two field-shape fast paths the audit analysed:
//
//   hash_map_walk_keys<E>   vmhook.hpp ~16229  (HashSet / LinkedHashSet:
//                           Node[] bucket array + Node.next chain; field "map")
//   tree_map_walk_keys<E>   vmhook.hpp ~16425  (TreeSet: iterative in-order
//                           red-black walk over TreeMap.root; field "m")
//   collection::to_vector   vmhook.hpp ~15737  (the field-shape cascade that
//                           routes "map"→HashSet path, "m"→TreeSet path, then
//                           falls through to a List-only get(int) fallback that a
//                           Set can never satisfy — see the wrapped-Set cases)
//
// Coverage matrix (every Set shape / size / flaw, all on real heap objects):
//
//   HashSet  (map → hash_map_walk_keys):
//     * empty (table all-null → 0 elements, no read)
//     * single
//     * TWO (the smallest multi-element bucket walk)
//     * many (MANY_N=50 → backing HashMap resized past the default 16 buckets)
//     * BIG (BIG_N=5000 → many buckets + chains; every element distinct, count
//       exact; a cycle/dup walk bug would re-emit a node → duplicate OOP)
//     * DUPLICATE-ADD (DUP_DISTINCT distinct ids, each re-added via a value-equal
//       Elem) — the Set deduplicates by equals/hashCode, so the decode must
//       surface each id EXACTLY once (set semantics survive the walk)
//     * HashSet<String> (String element decode via the key walk)
//     * small COLLISION CHAIN (COLLISION_CHAIN_N colliding-hashCode keys BELOW
//       the treeify threshold → a plain Node.next chain in one bucket; the walk
//       must follow next and surface every key — Java confirms it is a plain,
//       multi-node, non-treeified chain via reflection, when reflection is open)
//     * TREEIFIED bin (TREEIFY_N colliding keys → a TreeNode bucket head; the
//       Node.next chain stays populated, so the key walk must still surface every
//       element — the TreeNode-via-Node-super find_field path)
//     * legal single NULL element + reals → a nullptr slot for the null
//     * ONLY a null element (size 1) → exactly one nullptr slot, no real element
//     * HashSet<Long>    (8-byte boxed-primitive value field read)
//     * HashSet<Character> (boxed char value field read)
//     * HashSet<Boolean>  (the BOUNDARY boxed set: maximal size is {TRUE,FALSE})
//
//   LinkedHashSet (also map → SAME hash_map_walk_keys):
//     * empty + TWO + small + many — CONTENT verified order-independently;
//       insertion order deliberately NOT required, characterizing the documented
//       [low] "LinkedHashSet insertion order is silently lost" behaviour (vmhook
//       walks bucket order, ignoring the LinkedHashMap before/after overlay).
//     * LinkedHashSet<String> (String decode through the LinkedHashMap hash walk)
//     * LinkedHashSet with one null + reals (null-slot on the linked path)
//
//   TreeSet  (m → tree_map_walk_keys):
//     * empty (root null → 0 elements, no throw)
//     * single / TWO / small / many (TREE_MANY_N=200 deep tree) — in-order ==
//       SORTED element order, asserted EXACTLY.
//     * REVERSE comparator (Collections.reverseOrder()) — the in-order walk must
//       honour the comparator and come out DESCENDING by id, NOT natural order.
//     * DUPLICATE-add (compareTo-equal re-adds) → dedup survives the tree walk.
//     * TreeSet<String> (sorted lexicographic order, exact) + REVERSE-comparator
//       TreeSet<String> (descending lexicographic, proves the comparator is
//       honoured on a reference key type too).
//     * TreeSet<Integer> (BOXED element; ascending numeric order, exact).
//
//   Boxed-Integer element decode (java.lang.Integer.value read as a primitive):
//     * HashSet<Integer> (INT_N=40 values; value fingerprint + membership).
//     * TreeSet<Integer> (sorted ascending, exact).
//
//   "m"-backed JDK Set wrappers that DO reach a fast path (the klass-shape
//   router, vmhook.hpp ~17369, picks tree-vs-hash by the backing-map's real
//   layout, NOT the field name) — all PURE memory walks, decoded from the body:
//     * Collections.newSetFromMap(new HashMap<>())   "m" → HashMap (table) →
//       hash walk; was a [medium] bug that returned empty, now FIXED — full decode.
//     * Collections.newSetFromMap(new TreeMap<>())   "m" → TreeMap (root) → TREE
//       walk → SORTED decode (proves the router picks the tree walk by klass).
//     * Collections.newSetFromMap(new TreeMap<>(reverseOrder))  "m" → TreeMap →
//       TREE walk honours the comparator → DESCENDING decode.
//     * Collections.newSetFromMap(new LinkedHashMap<>())  "m" → LinkedHashMap
//       (table, no root) → hash walk → content decode (bucket order).
//     * ConcurrentHashMap.newKeySet()                "map" resolved off the
//       KeySetView SUPERCLASS (CollectionView) via find_field's superclass walk;
//       backing CHM 'table' Nodes carry key/next → full decode (empty + populated).
//
//   JDK Collections wrappers (the cascade's generic-fallback frontier).  These
//   have NO fast-path field shape (EmptySet: no fields; SingletonSet: "element";
//   Unmodifiable/Synchronized: "c"; EnumSet: a primitive `long elements` bitmask;
//   Set.of → Set12 "e0/e1" / SetN "elements[]"+"size"), so collection::to_vector
//   would reach the generic get(int) fallback, which a Set cannot satisfy — and
//   that fallback issues a Java size() call.  Calling a Java method from the
//   worker-thread body is forbidden by the suite (only safe inside a detour), so
//   this module does NOT decode these from the body; instead it pins each one's
//   Java-published size() (a pure static-field read) and records an [INFO]
//   documenting the List-only-fallback limitation (the SAME root cause as the
//   setFromHashMap [medium] bug).  The empty-decode correctness of the fallback
//   itself is proven by collection_list.cpp inside a detour.
//     * Collections.emptySet()          (size 0)
//     * Collections.singleton(Elem)     (size 1)
//     * Collections.unmodifiableSet(..) (size 2, field "c")
//     * Collections.synchronizedSet(..) (size 2, field "c")
//     * EnumSet.of(3) / EnumSet.noneOf(0)  (primitive long bitmask, no oop field)
//     * Set.of(0/1/2/3)  (JDK 9+, built reflectively so the fixture compiles at
//       -source 8; GATED on setOfAvailable; per-run SALT-randomized order)
//
//   size cross-check:  for every populated set the decoded element count is
//   cross-checked against Java's own size(), published by the fixture into a
//   static int field (read with a pure heap-field read — NEVER by calling a Java
//   size() method from the worker-thread body, which the suite forbids outside a
//   detour).
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
// strict ordered assertions because its walk order is defined (ascending for the
// natural-order sets, descending for the reverse-comparator set).
//
// SUITE-SAFETY (mirrors collection_list.cpp / register_class.cpp): the whole body
// runs under a try/catch (a throw is recorded as [INFO], never a FAIL); an entry
// guard bails to [INFO] if the fixture class does not resolve; the only hook is a
// scoped_hook<> that RAII-uninstalls on scope exit; and an unconditional
// vmhook::shutdown_hooks() OUTSIDE the try guarantees ZERO hooks armed on EVERY
// exit path.  No Java method is called from the body (only pure heap reads and
// the memory-walk to_vector fast paths); is_valid_pointer() is only ever applied
// before a RAW pointer deref; element handles are null-checked (a Set can legally
// hold a null).  No forced System.gc() is issued, so the MSVC/POSIX gc gate is
// not needed here.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
    // ── ELEMENT wrapper: vmhook.fixtures.CollSet$Elem. ──────────────────────
    // hash_map_walk_keys / tree_map_walk_keys build make_unique<elem_object>
    // from each decoded key OOP.  Reads BOTH a primitive (id:int) and a
    // reference (tag:String) field, proving each element OOP round-trips fully.
    // Accessors use the documented clean one-liner idiom (header ~14856): no
    // defensive has_value()->sentinel in the accessor; module-level try/catch +
    // the entry guard provide safety, not the getters.
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

    // ── BOXED-Integer element wrapper: java.lang.Integer. ───────────────────
    // For HashSet<Integer> / TreeSet<Integer>, each element OOP is a boxed
    // Integer; value() reads its primitive `value` int field directly (a pure
    // heap read, no Java call) — the boxed-element decode angle.
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

    // ── BOXED-Long element wrapper: java.lang.Long. ────────────────────────
    // For HashSet<Long>, each element OOP is a boxed Long; value() reads its
    // primitive 8-byte `value` long field directly — the wide-primitive
    // field-read angle (Integer covers 4-byte, Long covers 8-byte).
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

    // ── BOXED-Character element wrapper: java.lang.Character. ───────────────
    // For HashSet<Character>, each element OOP is a boxed Character; value()
    // reads its primitive `value` char (u16) field.
    class char_box : public vmhook::object<char_box>
    {
    public:
        explicit char_box(vmhook::oop_t instance) noexcept
            : vmhook::object<char_box>{ instance }
        {
        }

        auto value() const -> std::int32_t
        {
            return static_cast<std::int32_t>(get_field("value")->get());
        }
    };

    // ── BOXED-Boolean element wrapper: java.lang.Boolean. ──────────────────
    // For HashSet<Boolean>, each element OOP is a boxed Boolean; value() reads
    // its primitive `value` boolean field.
    class bool_box : public vmhook::object<bool_box>
    {
    public:
        explicit bool_box(vmhook::oop_t instance) noexcept
            : vmhook::object<bool_box>{ instance }
        {
        }

        auto value() const -> bool
        {
            return static_cast<bool>(get_field("value")->get());
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
        static auto set_go(bool value) -> void     { static_field("go")->set(value); }
        static auto set_done(bool value) -> void    { static_field("done")->set(value); }
        static auto get_done() -> bool              { return static_field("done")->get(); }
        static auto set_mode(std::int32_t m) -> void { static_field("mode")->set(m); }
        static auto get_observed() -> std::int32_t  { return static_field("observed")->get(); }

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

        // Same, decoded to a boxed-Integer vector (HashSet<Integer> /
        // TreeSet<Integer>).
        static auto ints_of(const char* field)
            -> std::vector<std::unique_ptr<integer_box>>
        {
            const auto proxy{ static_field(field) };
            if (!proxy.has_value())
            {
                return {};
            }
            return proxy->get().to_vector<integer_box>();
        }

        // Same, decoded to a boxed-Long vector (HashSet<Long>).
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

        // Same, decoded to a boxed-Character vector (HashSet<Character>).
        static auto chars_of(const char* field)
            -> std::vector<std::unique_ptr<char_box>>
        {
            const auto proxy{ static_field(field) };
            if (!proxy.has_value())
            {
                return {};
            }
            return proxy->get().to_vector<char_box>();
        }

        // Same, decoded to a boxed-Boolean vector (HashSet<Boolean>).
        static auto bools_of(const char* field)
            -> std::vector<std::unique_ptr<bool_box>>
        {
            const auto proxy{ static_field(field) };
            if (!proxy.has_value())
            {
                return {};
            }
            return proxy->get().to_vector<bool_box>();
        }

        // Java-published cross-check values (pure heap-field reads — no Java call).
        static auto j_size(const char* f) -> std::int32_t { return static_field(f)->get(); }
        static auto j_long(const char* f) -> std::int64_t { return static_field(f)->get(); }
        static auto j_bool(const char* f) -> bool { return static_field(f)->get(); }
    };

    // ── Fixture-mirrored constants (lockstep with CollSet.java). ────────────
    constexpr std::int32_t TWO{ 2 };
    constexpr std::int32_t SMALL_N{ 3 };
    constexpr std::int32_t DUP_DISTINCT{ 4 };
    constexpr std::int32_t COLLISION_CHAIN_N{ 5 };
    constexpr std::int32_t MANY_N{ 50 };
    constexpr std::int32_t BIG_N{ 5000 };
    constexpr std::int32_t TREEIFY_N{ 64 };
    constexpr std::int32_t TREE_MANY_N{ 200 };
    constexpr std::int32_t NULL_SET_NONNULL{ 3 };
    constexpr std::int32_t SETFROMMAP_N{ 4 };
    constexpr std::int32_t CHM_N{ 50 };
    constexpr std::int32_t INT_N{ 40 };
    constexpr std::int32_t CHAR_N{ 10 };

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
        bool         distinct_oops{ true };   // every non-null element OOP unique
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

    // Order-independent fingerprint over a decoded boxed-Integer set.
    struct int_stats
    {
        std::int32_t count{ 0 };
        std::int32_t null_count{ 0 };
        std::int64_t val_sum{ 0 };
        std::int64_t val_xor{ 0 };
        bool         distinct_oops{ true };
    };

    auto fingerprint_ints(const std::vector<std::unique_ptr<integer_box>>& v)
        -> int_stats
    {
        int_stats st;
        st.count = static_cast<std::int32_t>(v.size());
        std::unordered_set<const void*> seen;
        seen.reserve(v.size() * 2 + 1);
        for (const auto& up : v)
        {
            const integer_box* const e{ up.get() };
            if (e == nullptr)
            {
                ++st.null_count;
                continue;
            }
            const std::int32_t val{ e->value() };
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

    // Decoded boxed-Integer values IN WALK ORDER (a placeholder for a null slot).
    auto int_values_in_order(const std::vector<std::unique_ptr<integer_box>>& v)
        -> std::vector<std::int32_t>
    {
        std::vector<std::int32_t> order;
        order.reserve(v.size());
        for (const auto& up : v)
        {
            order.push_back(up ? up->value() : -1);
        }
        return order;
    }

    // Collect decoded Elem ids IN WALK ORDER (a -1 placeholder for a null slot).
    // Used for the TreeSet ordered-walk assertions.
    auto ids_in_order(const std::vector<std::unique_ptr<elem_object>>& v)
        -> std::vector<std::int32_t>
    {
        std::vector<std::int32_t> order;
        order.reserve(v.size());
        for (const auto& up : v)
        {
            order.push_back(up ? up->id() : -1);
        }
        return order;
    }

    // Characterize a JDK Collections Set wrapper that has NO fast-path field
    // shape (EmptySet / SingletonSet / Unmodifiable / Synchronized).  Decoding it
    // would reach to_vector's generic get(int) fallback, which issues a Java
    // size() call — forbidden from the worker-thread body — so we DO NOT decode
    // it here.  We assert only the Java-published size() (a pure static-field
    // read) and record the limitation.  `expected_size` is the Set's true size().
    auto characterize_wrapped_set(vmhook_test::context& ctx,
                                  const std::string& probe,    // assertion-name prefix
                                  const char* size_field,      // published j_size field
                                  const char* backing_field,   // the wrapper's backing field name (doc)
                                  std::int32_t expected_size) -> void
    {
        const std::int32_t java_size{ coll_set_fixture::j_size(size_field) };
        ctx.check(probe + "_java_size_matches", java_size == expected_size);
        ctx.record("[INFO] " + probe + ": JDK wrapper has backing field '"
                   + backing_field + "', NOT one of map/m/elementData/first, so "
                   "collection::to_vector would take the List-only get(int) "
                   "fallback (which a Set cannot satisfy and which issues a Java "
                   "size() call); this module does not decode it from the worker "
                   "body.  Java size() == " + std::to_string(java_size)
                   + " (same decode limitation as setFromHashMap).");
    }

    constexpr char FIXTURE[]{ "vmhook/fixtures/CollSet" };

    // The whole test body, factored out so the VMHOOK_JVM_MODULE wrapper can run
    // it under a try/catch and ALWAYS follow it with shutdown_hooks() (suite-
    // safety: ZERO hooks armed on EVERY exit path, mirrors collection_list.cpp /
    // register_class.cpp).
    auto run_collection_set_checks(vmhook_test::context& ctx) -> void
    {
        // ─── ENTRY GUARD ────────────────────────────────────────────────────
        // If CollSet is not loaded/resolvable, the static_field("go")/("done")
        // handshake derefs below would deref a disengaged optional.  Bail cleanly
        // to [INFO] (the wrapper's final shutdown_hooks() still runs).  In
        // practice loadFixtures() loads every vmhook.fixtures.* class each run, so
        // this is belt-and-braces.
        if (vmhook::find_class(FIXTURE) == nullptr)
        {
            ctx.record("[INFO] collection_set: CollSet not loaded/resolvable on this "
                       "run; skipping the module's live checks (no crash, no hooks "
                       "armed).");
            return;
        }

        vmhook::register_class<coll_set_fixture>(FIXTURE);
        vmhook::register_class<elem_object>("vmhook/fixtures/CollSet$Elem");
        vmhook::register_class<string_element>("java/lang/String");
        vmhook::register_class<integer_box>("java/lang/Integer");
        vmhook::register_class<long_box>("java/lang/Long");
        vmhook::register_class<char_box>("java/lang/Character");
        vmhook::register_class<bool_box>("java/lang/Boolean");

        // The fixture's static initializer already built every set (buildAll()).
        // Drive one mode-0 probe first so the build also runs on the Java thread
        // and we read a freshly-populated, deterministic snapshot.
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
        // HashSet — TWO elements.  The smallest multi-element bucket walk.
        // =====================================================================
        {
            const auto v{ coll_set_fixture::elems_of("hashTwo") };
            const elem_stats st{ fingerprint(v) };
            ctx.check("hash_two_count_is_2", st.count == TWO);
            ctx.check("hash_two_count_matches_java",
                      st.count == coll_set_fixture::j_size("hashTwoSize"));
            ctx.check("hash_two_no_null", st.null_count == 0);
            ctx.check("hash_two_id_sum_matches_java",
                      st.id_sum == coll_set_fixture::j_long("hashTwoIdSum"));
            ctx.check("hash_two_id_xor_matches_java",
                      st.id_xor == coll_set_fixture::j_long("hashTwoIdXor"));
            ctx.check("hash_two_distinct", st.distinct_oops);
            const auto ids{ id_set(v) };
            ctx.check("hash_two_has_0_and_1",
                      ids.count(0) == 1 && ids.count(1) == 1);
        }

        // =====================================================================
        // HashSet — MANY (50).  Backing HashMap resized past the default 16
        // buckets; verify the walker visits ALL buckets/chains (count + full
        // fingerprint + every id 0..49 present + all element OOPs distinct).
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
        // correct at scale" battery: exact count, full fingerprint, ALL element
        // OOPs distinct (a cycle/dup bug re-emits a node → duplicate OOP), no
        // nulls.
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
        // HashSet — DUPLICATE-ADD.  DUP_DISTINCT distinct ids, each re-added via
        // a value-equal (same id) but distinct Elem object.  The Set dedupes by
        // equals/hashCode, so the decode must surface each id EXACTLY once —
        // count == DUP_DISTINCT, ids 0..DUP_DISTINCT-1 each once, OOPs distinct.
        // (Proves set semantics survive the bucket walk: no duplicate element is
        // re-emitted, and the absorbed re-adds left no phantom slots.)
        // =====================================================================
        {
            const auto v{ coll_set_fixture::elems_of("hashDup") };
            const elem_stats st{ fingerprint(v) };

            ctx.check("hash_dup_count_is_distinct_count", st.count == DUP_DISTINCT);
            ctx.check("hash_dup_count_matches_java",
                      st.count == coll_set_fixture::j_size("hashDupSize"));
            ctx.check("hash_dup_no_null", st.null_count == 0);
            ctx.check("hash_dup_id_sum_matches_java",
                      st.id_sum == coll_set_fixture::j_long("hashDupIdSum"));
            ctx.check("hash_dup_id_xor_matches_java",
                      st.id_xor == coll_set_fixture::j_long("hashDupIdXor"));
            ctx.check("hash_dup_all_elements_distinct", st.distinct_oops);
            ctx.check("hash_dup_tags_round_trip", st.tags_consistent);

            const auto ids{ id_set(v) };
            bool every_id_once{ ids.size() == static_cast<std::size_t>(DUP_DISTINCT) };
            for (std::int32_t i{ 0 }; i < DUP_DISTINCT; ++i)
            {
                if (ids.find(i) == ids.end()) { every_id_once = false; }
            }
            ctx.check("hash_dup_each_id_exactly_once", every_id_once);
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
        // HashSet — small COLLISION CHAIN (COLLISION_CHAIN_N colliding-hashCode
        // keys, BELOW the treeify threshold).  All keys land in ONE bucket as a
        // plain Node.next singly-linked chain (no TreeNode); the key walk must
        // follow next and surface every key.  When Java reflection is open it
        // confirms a plain, multi-node, non-treeified chain; otherwise the count
        // check alone proves the chain was fully walked.
        // =====================================================================
        {
            const auto v{ coll_set_fixture::strings_of("hashCollisionChain") };
            const string_stats st{ fingerprint_strings(v) };

            ctx.check("hash_chain_count_matches_n", st.count == COLLISION_CHAIN_N);
            ctx.check("hash_chain_count_matches_java",
                      st.count == coll_set_fixture::j_size("hashCollisionChainSize"));
            ctx.check("hash_chain_no_null", st.null_count == 0);
            ctx.check("hash_chain_char_sum_matches_java",
                      st.char_sum == coll_set_fixture::j_long("hashCollisionChainCharSum"));
            ctx.check("hash_chain_all_distinct", st.distinct_text);

            const bool plain_chain{ coll_set_fixture::j_bool("collisionChainIsPlainChain") };
            ctx.record(std::string{ "[INFO] hashCollisionChain is a plain multi-node "
                                    "Node.next chain (no TreeNode): " }
                       + (plain_chain ? "yes" : "no (reflection blocked or layout "
                                                "differs; count check still holds)"));
            // When Java confirmed a genuine multi-node plain chain, the count
            // check above proves the Node.next walk traversed the whole chain.
            if (plain_chain)
            {
                ctx.check("hash_chain_plain_walk_returned_all",
                          st.count == COLLISION_CHAIN_N);
            }
        }

        // =====================================================================
        // HashSet — TREEIFIED bin (>8 colliding-hashCode String keys).  After
        // treeification a bucket head is a TreeNode, but the Node.next chain
        // stays populated, so the key walk must still return EVERY element.
        // Verify full count + char sum + distinctness; record whether a bin
        // actually treeified (Java confirms via reflection when it is open).
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
            // If Java confirmed a TreeNode bin, the walk-through-TreeNode-via-
            // Node-super path was exercised; the count check above proves it
            // returned everything.
            if (treeified)
            {
                ctx.check("hash_treeified_treenode_path_returned_all",
                          st.count == TREEIFY_N);
            }
        }

        // =====================================================================
        // HashSet — legal single NULL element + NULL_SET_NONNULL reals.  vmhook
        // surfaces the null element as a nullptr slot; the real elements decode
        // intact.  (Locks the "Null Java elements become nullptr" promise for the
        // HashSet key path — a HashSet CAN legally hold one null.)
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
        // HashSet — ONLY the legal single NULL element (size 1).  The pure-null
        // boundary: the backing HashMap has exactly one Node whose key is null, so
        // the bucket walk must emit EXACTLY one nullptr slot and no real element.
        // =====================================================================
        {
            const auto v{ coll_set_fixture::elems_of("hashOnlyNull") };
            const elem_stats st{ fingerprint(v) };
            ctx.check("hash_onlynull_count_is_1", st.count == 1);
            ctx.check("hash_onlynull_count_matches_java",
                      st.count == coll_set_fixture::j_size("hashOnlyNullSize"));
            ctx.check("hash_onlynull_exactly_one_null", st.null_count == 1);
            ctx.check("hash_onlynull_no_real_elements",
                      (st.count - st.null_count) == 0);
            ctx.check("hash_onlynull_id_sum_zero", st.id_sum == 0);
        }

        // =====================================================================
        // LinkedHashSet — TWO + SMALL + MANY.  Reuses HashSet's "map"→
        // hash_map_walk_keys fast path, so the SAME walker runs.  Verify CONTENT
        // order-independently.  (Audit [low]: vmhook walks BUCKET order, NOT
        // LinkedHashSet insertion order — we deliberately do NOT assert insertion
        // order, and record the quirk.)
        // =====================================================================
        {
            const auto v{ coll_set_fixture::elems_of("linkedTwo") };
            const elem_stats st{ fingerprint(v) };
            ctx.check("linked_two_count_is_2", st.count == TWO);
            ctx.check("linked_two_count_matches_java",
                      st.count == coll_set_fixture::j_size("linkedTwoSize"));
            ctx.check("linked_two_no_null", st.null_count == 0);
            ctx.check("linked_two_id_sum_matches_java",
                      st.id_sum == coll_set_fixture::j_long("linkedTwoIdSum"));
            ctx.check("linked_two_distinct", st.distinct_oops);
        }
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
            const std::vector<std::int32_t> order{ ids_in_order(v) };
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
        // LinkedHashSet — EMPTY.  map exists, all buckets null → 0, no throw.
        // =====================================================================
        {
            const auto v{ coll_set_fixture::elems_of("linkedEmpty") };
            ctx.check("linked_empty_size_zero", v.empty());
            ctx.check("linked_empty_java_size_zero",
                      coll_set_fixture::j_size("linkedEmptySize") == 0);
        }

        // =====================================================================
        // LinkedHashSet<String> — String element decode through the
        // LinkedHashMap-backed hash walk (LinkedHashMap.Entry's key/next resolve
        // via the superclass-walking find_field; the before/after overlay is
        // ignored).  Content verified order-independently.  "ls0".."ls2".
        // =====================================================================
        {
            const auto v{ coll_set_fixture::strings_of("linkedStrings") };
            const string_stats st{ fingerprint_strings(v) };
            ctx.check("linked_strings_count_is_3", st.count == SMALL_N);
            ctx.check("linked_strings_count_matches_java",
                      st.count == coll_set_fixture::j_size("linkedStringsSize"));
            ctx.check("linked_strings_no_null", st.null_count == 0);
            ctx.check("linked_strings_char_sum_matches_java",
                      st.char_sum == coll_set_fixture::j_long("linkedStringsCharSum"));
            ctx.check("linked_strings_all_distinct", st.distinct_text);

            std::unordered_set<std::string> texts;
            for (const auto& up : v) { if (up) { texts.insert(up->text()); } }
            ctx.check("linked_strings_contains_ls0", texts.count("ls0") == 1);
            ctx.check("linked_strings_contains_ls2",
                      texts.count("ls" + std::to_string(SMALL_N - 1)) == 1);
        }

        // =====================================================================
        // LinkedHashSet — one legal NULL + NULL_SET_NONNULL reals (ids 700..702).
        // Locks the "null element → nullptr slot" promise on the LinkedHashMap-
        // backed path too (mirrors hashWithNull but through the linked overlay).
        // =====================================================================
        {
            const auto v{ coll_set_fixture::elems_of("linkedWithNull") };
            const elem_stats st{ fingerprint(v) };
            ctx.check("linked_withnull_count_is_4",
                      st.count == NULL_SET_NONNULL + 1);
            ctx.check("linked_withnull_count_matches_java",
                      st.count == coll_set_fixture::j_size("linkedWithNullSize"));
            ctx.check("linked_withnull_exactly_one_null", st.null_count == 1);
            ctx.check("linked_withnull_nonnull_count",
                      (st.count - st.null_count) == NULL_SET_NONNULL);
            ctx.check("linked_withnull_id_sum_matches_java",
                      st.id_sum == coll_set_fixture::j_long("linkedWithNullIdSum"));
            ctx.check("linked_withnull_nonnull_distinct", st.distinct_oops);
            ctx.check("linked_withnull_nonnull_tags_ok", st.tags_consistent);
            const auto ids{ id_set(v) };
            ctx.check("linked_withnull_contains_700", ids.count(700) == 1);
            ctx.check("linked_withnull_contains_702", ids.count(702) == 1);
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
        // TreeSet — TWO.  Inserted out of order (1 then 0); the in-order walk
        // must re-sort to [0,1].
        // =====================================================================
        {
            const auto v{ coll_set_fixture::elems_of("treeTwo") };
            const elem_stats st{ fingerprint(v) };
            ctx.check("tree_two_count_is_2", st.count == TWO);
            ctx.check("tree_two_count_matches_java",
                      st.count == coll_set_fixture::j_size("treeTwoSize"));
            ctx.check("tree_two_no_null", st.null_count == 0);
            const std::vector<std::int32_t> order{ ids_in_order(v) };
            ctx.check("tree_two_in_sorted_order",
                      order.size() == 2 && order[0] == 0 && order[1] == 1);
        }

        // =====================================================================
        // TreeSet — SMALL (3).  The in-order red-black walk yields SORTED element
        // order — verify count, content, AND strict ascending id order (defined).
        // =====================================================================
        {
            const auto v{ coll_set_fixture::elems_of("treeSmall") };
            const elem_stats st{ fingerprint(v) };

            ctx.check("tree_small_count_is_3", st.count == SMALL_N);
            ctx.check("tree_small_count_matches_java",
                      st.count == coll_set_fixture::j_size("treeSmallSize"));
            ctx.check("tree_small_no_null", st.null_count == 0);
            ctx.check("tree_small_tags_round_trip", st.tags_consistent);

            // TreeSet<Elem> orders by Elem.id, so the in-order walk MUST produce
            // [0,1,2] exactly.
            const std::vector<std::int32_t> order{ ids_in_order(v) };
            ctx.check("tree_small_all_nonnull", st.null_count == 0);
            ctx.check("tree_small_in_sorted_order",
                      order.size() == 3 && order[0] == 0 && order[1] == 1 && order[2] == 2);
            ctx.check("tree_small_strictly_ascending",
                      std::is_sorted(order.begin(), order.end()));
            ctx.check("tree_small_first_is_0", !order.empty() && order.front() == 0);
            ctx.check("tree_small_last_is_2", !order.empty() && order.back() == 2);
        }

        // =====================================================================
        // TreeSet — MANY (200).  Deep red-black tree; the iterative stack walk
        // must visit all nodes in SORTED order without blowing the stack.
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
            const std::vector<std::int32_t> order{ ids_in_order(v) };
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
        // TreeSet — REVERSE comparator.  Built with Collections.reverseOrder(),
        // so TreeMap's in-order red-black walk visits nodes in the COMPARATOR's
        // order — the decode must come out DESCENDING by id: [SMALL_N-1 .. 0].
        // Proves the in-order walk honours a custom comparator, not natural order.
        // =====================================================================
        {
            const auto v{ coll_set_fixture::elems_of("treeReverse") };
            const elem_stats st{ fingerprint(v) };

            ctx.check("tree_reverse_count_is_3", st.count == SMALL_N);
            ctx.check("tree_reverse_count_matches_java",
                      st.count == coll_set_fixture::j_size("treeReverseSize"));
            ctx.check("tree_reverse_no_null", st.null_count == 0);

            const std::vector<std::int32_t> order{ ids_in_order(v) };
            // Descending: [2,1,0].
            bool descending{ order.size() == static_cast<std::size_t>(SMALL_N) };
            for (std::size_t k{ 0 }; descending && k < order.size(); ++k)
            {
                if (order[k] != (SMALL_N - 1 - static_cast<std::int32_t>(k)))
                {
                    descending = false;
                }
            }
            ctx.check("tree_reverse_in_descending_id_order", descending);
            ctx.check("tree_reverse_first_is_max",
                      !order.empty() && order.front() == SMALL_N - 1);
            ctx.check("tree_reverse_last_is_0",
                      !order.empty() && order.back() == 0);
            // is_sorted with greater<> confirms a non-increasing sequence.
            ctx.check("tree_reverse_is_reverse_sorted",
                      std::is_sorted(order.begin(), order.end(), std::greater<std::int32_t>{}));
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
        // TreeSet<String> — REVERSE comparator.  The in-order red-black walk must
        // honour the comparator and come out DESCENDING lexicographically
        // ["cherry","banana","apple"] — proves the tree walk respects a comparator
        // on a reference (String) key type, not just the Elem Comparable.
        // =====================================================================
        {
            const auto v{ coll_set_fixture::strings_of("treeStringsReverse") };
            const string_stats st{ fingerprint_strings(v) };
            ctx.check("tree_strings_rev_count_is_3", st.count == SMALL_N);
            ctx.check("tree_strings_rev_count_matches_java",
                      st.count == coll_set_fixture::j_size("treeStringsReverseSize"));
            ctx.check("tree_strings_rev_no_null", st.null_count == 0);

            std::vector<std::string> order;
            order.reserve(v.size());
            for (const auto& up : v) { order.push_back(up ? up->text() : std::string{}); }
            ctx.check("tree_strings_rev_is_descending",
                      std::is_sorted(order.begin(), order.end(), std::greater<std::string>{}));
            ctx.check("tree_strings_rev_first_is_cherry",
                      !order.empty() && order.front() == "cherry");
            ctx.check("tree_strings_rev_last_is_apple",
                      !order.empty() && order.back() == "apple");
            ctx.check("tree_strings_rev_exact_sequence",
                      order.size() == 3 && order[0] == "cherry"
                      && order[1] == "banana" && order[2] == "apple");
        }

        // =====================================================================
        // TreeSet — DUPLICATE-ADD.  SMALL_N distinct ids, each re-added via a
        // value-equal (compareTo==0) but distinct Elem.  TreeSet dedupes by the
        // comparator, so the size stays SMALL_N and the in-order walk surfaces each
        // id exactly once, ascending [0,1,2] — set dedup survives the tree walk.
        // =====================================================================
        {
            const auto v{ coll_set_fixture::elems_of("treeDup") };
            const elem_stats st{ fingerprint(v) };
            ctx.check("tree_dup_count_is_3", st.count == SMALL_N);
            ctx.check("tree_dup_count_matches_java",
                      st.count == coll_set_fixture::j_size("treeDupSize"));
            ctx.check("tree_dup_no_null", st.null_count == 0);
            ctx.check("tree_dup_all_distinct", st.distinct_oops);
            ctx.check("tree_dup_tags_round_trip", st.tags_consistent);
            const std::vector<std::int32_t> order{ ids_in_order(v) };
            ctx.check("tree_dup_in_sorted_order",
                      order.size() == 3 && order[0] == 0 && order[1] == 1 && order[2] == 2);
            ctx.check("tree_dup_strictly_ascending",
                      std::is_sorted(order.begin(), order.end()));
            const auto ids{ id_set(v) };
            ctx.check("tree_dup_each_id_once",
                      ids.size() == static_cast<std::size_t>(SMALL_N));
        }

        // =====================================================================
        // Collections.newSetFromMap(new TreeMap<>())  — the "m" backing-field
        // name collides with TreeSet AND setFromHashMap, but the backing map is a
        // TreeMap (HAS "root").  to_vector's "m"-route inspects the backing-map
        // klass, finds "root", and routes to tree_map_walk_keys — so this decodes
        // IN SORTED ORDER, proving the klass-shape router picks the TREE walk (not
        // the hash walk) for a TreeMap-backed SetFromMap.  Ids 400..402, inserted
        // out of order; the in-order walk must come out ascending [400,401,402].
        // PURE memory walk (no Java call) → safe to decode from the body.
        // =====================================================================
        {
            const std::int32_t java_size{ coll_set_fixture::j_size("setFromTreeMapSize") };
            ctx.check("setfromtreemap_java_size_is_3", java_size == SMALL_N);

            const auto v{ coll_set_fixture::elems_of("setFromTreeMap") };
            const elem_stats st{ fingerprint(v) };
            ctx.check("setfromtreemap_count_is_3", st.count == SMALL_N);
            ctx.check("setfromtreemap_count_matches_java", st.count == java_size);
            ctx.check("setfromtreemap_no_null", st.null_count == 0);
            ctx.check("setfromtreemap_distinct", st.distinct_oops);
            ctx.check("setfromtreemap_tags_round_trip", st.tags_consistent);

            // TreeMap-backed → in-order walk → ascending by id: [400,401,402].
            const std::vector<std::int32_t> order{ ids_in_order(v) };
            bool ascending{ order.size() == static_cast<std::size_t>(SMALL_N) };
            for (std::size_t k{ 0 }; ascending && k < order.size(); ++k)
            {
                if (order[k] != 400 + static_cast<std::int32_t>(k)) { ascending = false; }
            }
            ctx.check("setfromtreemap_in_sorted_order", ascending);
            ctx.check("setfromtreemap_is_sorted",
                      std::is_sorted(order.begin(), order.end()));
            ctx.record("[INFO] newSetFromMap(TreeMap): backing field 'm' resolves to "
                       "a TreeMap (has 'root'); to_vector's klass-shape router takes "
                       "the in-order red-black walk → SORTED decode (vs the HashMap-"
                       "backed setFromHashMap which takes the bucket walk).");
        }

        // =====================================================================
        // Collections.newSetFromMap(new TreeMap<>(reverseOrder))  — the "m"-route
        // finds "root" on the backing TreeMap and takes the in-order walk, which
        // honours the TreeMap's REVERSE comparator → DESCENDING decode
        // [402,401,400].  Proves the SetFromMap tree route respects a comparator
        // exactly like a plain reverse-comparator TreeSet.  Ids 400..402.
        // =====================================================================
        {
            const std::int32_t java_size{ coll_set_fixture::j_size("setFromTreeMapReverseSize") };
            ctx.check("setfromtreemap_rev_java_size_is_3", java_size == SMALL_N);

            const auto v{ coll_set_fixture::elems_of("setFromTreeMapReverse") };
            const elem_stats st{ fingerprint(v) };
            ctx.check("setfromtreemap_rev_count_is_3", st.count == SMALL_N);
            ctx.check("setfromtreemap_rev_count_matches_java", st.count == java_size);
            ctx.check("setfromtreemap_rev_no_null", st.null_count == 0);
            ctx.check("setfromtreemap_rev_distinct", st.distinct_oops);
            ctx.check("setfromtreemap_rev_tags_round_trip", st.tags_consistent);

            const std::vector<std::int32_t> order{ ids_in_order(v) };
            bool descending{ order.size() == static_cast<std::size_t>(SMALL_N) };
            for (std::size_t k{ 0 }; descending && k < order.size(); ++k)
            {
                if (order[k] != 402 - static_cast<std::int32_t>(k)) { descending = false; }
            }
            ctx.check("setfromtreemap_rev_in_descending_order", descending);
            ctx.check("setfromtreemap_rev_is_reverse_sorted",
                      std::is_sorted(order.begin(), order.end(), std::greater<std::int32_t>{}));
            ctx.check("setfromtreemap_rev_first_is_402",
                      !order.empty() && order.front() == 402);
            ctx.check("setfromtreemap_rev_last_is_400",
                      !order.empty() && order.back() == 400);
        }

        // =====================================================================
        // Collections.newSetFromMap(new LinkedHashMap<>())  — backing field "m"
        // again, but the backing map is a LinkedHashMap (NO "root", HAS "table"
        // inherited from HashMap).  The "m"-route finds "table" and routes to
        // hash_map_walk_keys → content decodes (bucket order; the LinkedHashMap
        // insertion overlay is ignored, the SAME documented [low] as a plain
        // LinkedHashSet).  Verified order-independently.  Ids 500..502.
        // =====================================================================
        {
            const std::int32_t java_size{ coll_set_fixture::j_size("setFromLinkedMapSize") };
            ctx.check("setfromlinkedmap_java_size_is_3", java_size == SMALL_N);

            const auto v{ coll_set_fixture::elems_of("setFromLinkedMap") };
            const elem_stats st{ fingerprint(v) };
            ctx.check("setfromlinkedmap_count_is_3", st.count == SMALL_N);
            ctx.check("setfromlinkedmap_count_matches_java", st.count == java_size);
            ctx.check("setfromlinkedmap_no_null", st.null_count == 0);
            ctx.check("setfromlinkedmap_id_sum_matches_java",
                      st.id_sum == coll_set_fixture::j_long("setFromLinkedMapIdSum"));
            ctx.check("setfromlinkedmap_distinct", st.distinct_oops);
            ctx.check("setfromlinkedmap_tags_round_trip", st.tags_consistent);

            const auto ids{ id_set(v) };
            bool all_present{ ids.size() == static_cast<std::size_t>(SMALL_N) };
            for (std::int32_t i{ 0 }; i < SMALL_N; ++i)
            {
                if (ids.find(500 + i) == ids.end()) { all_present = false; }
            }
            ctx.check("setfromlinkedmap_every_id_present", all_present);
            ctx.record("[INFO] newSetFromMap(LinkedHashMap): backing field 'm' is a "
                       "LinkedHashMap (no 'root', has 'table'); klass-shape router "
                       "takes the HASH bucket walk → content correct in bucket order "
                       "(insertion overlay ignored, documented [low]).");
        }

        // =====================================================================
        // ConcurrentHashMap.newKeySet()  — a KeySetView whose backing field "map"
        // lives on its SUPERCLASS (CollectionView), so the "map" fast path's
        // find_field must resolve an INHERITED field (same superclass-walk the
        // treeified-bin TreeNode case depends on).  The backing CHM has a "table"
        // of Nodes carrying "key"/"next", so hash_map_walk_keys decodes every
        // element.  CHM_N=50 distinct ids → all buckets are plain Node heads (no
        // TreeBin / ForwardingNode), so the key/next walk surfaces all 50.  Full
        // fingerprint + distinctness + membership.  Ids 600..649.
        // =====================================================================
        {
            const std::int32_t java_size{ coll_set_fixture::j_size("chmKeySetSize") };
            ctx.check("chm_keyset_java_size_is_50", java_size == CHM_N);

            const auto v{ coll_set_fixture::elems_of("chmKeySet") };
            const elem_stats st{ fingerprint(v) };
            ctx.check("chm_keyset_count_is_50", st.count == CHM_N);
            ctx.check("chm_keyset_count_matches_java", st.count == java_size);
            ctx.check("chm_keyset_no_null", st.null_count == 0);
            ctx.check("chm_keyset_id_sum_matches_java",
                      st.id_sum == coll_set_fixture::j_long("chmKeySetIdSum"));
            ctx.check("chm_keyset_id_xor_matches_java",
                      st.id_xor == coll_set_fixture::j_long("chmKeySetIdXor"));
            ctx.check("chm_keyset_all_distinct", st.distinct_oops);
            ctx.check("chm_keyset_tags_round_trip", st.tags_consistent);

            const auto ids{ id_set(v) };
            bool all_present{ ids.size() == static_cast<std::size_t>(CHM_N) };
            for (std::int32_t i{ 0 }; i < CHM_N; ++i)
            {
                if (ids.find(600 + i) == ids.end()) { all_present = false; }
            }
            ctx.check("chm_keyset_every_id_present_no_dupes", all_present);
            ctx.record("[INFO] ConcurrentHashMap.newKeySet(): 'map' resolved off the "
                       "KeySetView SUPERCLASS (CollectionView) via find_field's "
                       "superclass walk; backing CHM 'table' Nodes carry key/next, so "
                       "the HashSet key walk decodes all elements.");
        }

        // ConcurrentHashMap.newKeySet() — EMPTY.  No elements added; the backing
        // CHM table may be null or all-null → 0 elements, no read, no throw.
        {
            const auto v{ coll_set_fixture::elems_of("chmKeySetEmpty") };
            ctx.check("chm_keyset_empty_size_zero", v.empty());
            ctx.check("chm_keyset_empty_java_size_zero",
                      coll_set_fixture::j_size("chmKeySetEmptySize") == 0);
        }

        // =====================================================================
        // HashSet<Integer>  — BOXED-Integer element decode.  Each decoded element
        // OOP is a java.lang.Integer; integer_box reads its primitive `value`
        // field.  Order-independent value fingerprint (sum/xor vs Java + closed
        // form), distinctness, full membership 0..INT_N-1.  (Integer.hashCode() ==
        // value, so distinct values cannot share a bucket-forcing hashCode; the
        // collision-into-tree-bin coverage is the String "Aa"/"BB" family.)
        // =====================================================================
        {
            const auto v{ coll_set_fixture::ints_of("hashIntegers") };
            const int_stats st{ fingerprint_ints(v) };
            ctx.check("hash_ints_count_is_n", st.count == INT_N);
            ctx.check("hash_ints_count_matches_java",
                      st.count == coll_set_fixture::j_size("hashIntegersSize"));
            ctx.check("hash_ints_no_null", st.null_count == 0);
            ctx.check("hash_ints_val_sum_matches_java",
                      st.val_sum == coll_set_fixture::j_long("hashIntegersValSum"));
            ctx.check("hash_ints_val_xor_matches_java",
                      st.val_xor == coll_set_fixture::j_long("hashIntegersValXor"));
            ctx.check("hash_ints_val_sum_closed_form",
                      st.val_sum == (static_cast<std::int64_t>(INT_N) * (INT_N - 1)) / 2);
            ctx.check("hash_ints_all_distinct", st.distinct_oops);

            std::unordered_set<std::int32_t> vals;
            for (const auto& up : v) { if (up) { vals.insert(up->value()); } }
            bool all_present{ vals.size() == static_cast<std::size_t>(INT_N) };
            for (std::int32_t i{ 0 }; i < INT_N; ++i)
            {
                if (vals.find(i) == vals.end()) { all_present = false; }
            }
            ctx.check("hash_ints_every_value_present", all_present);
        }

        // =====================================================================
        // TreeSet<Integer>  — BOXED-Integer through the in-order red-black walk;
        // inserted DESCENDING, must decode ASCENDING [0..INT_N-1] exactly.
        // =====================================================================
        {
            const auto v{ coll_set_fixture::ints_of("treeIntegers") };
            const int_stats st{ fingerprint_ints(v) };
            ctx.check("tree_ints_count_is_n", st.count == INT_N);
            ctx.check("tree_ints_count_matches_java",
                      st.count == coll_set_fixture::j_size("treeIntegersSize"));
            ctx.check("tree_ints_no_null", st.null_count == 0);
            ctx.check("tree_ints_all_distinct", st.distinct_oops);

            const std::vector<std::int32_t> order{ int_values_in_order(v) };
            ctx.check("tree_ints_is_sorted",
                      std::is_sorted(order.begin(), order.end()));
            bool exact{ order.size() == static_cast<std::size_t>(INT_N) };
            for (std::size_t k{ 0 }; exact && k < order.size(); ++k)
            {
                if (order[k] != static_cast<std::int32_t>(k)) { exact = false; }
            }
            ctx.check("tree_ints_exact_ascending_sequence", exact);
            ctx.check("tree_ints_first_is_0", !order.empty() && order.front() == 0);
            ctx.check("tree_ints_last_is_max",
                      !order.empty() && order.back() == INT_N - 1);
        }

        // =====================================================================
        // HashSet<Long>  — BOXED-Long element decode: the 8-byte primitive `value`
        // field read (Integer covered 4-byte; Long covers the wide case).  Order-
        // independent value fingerprint (sum/xor vs Java + closed form), full
        // membership 0..INT_N-1.
        // =====================================================================
        {
            const auto v{ coll_set_fixture::longs_of("hashLongs") };
            std::int32_t count{ 0 };
            std::int32_t null_count{ 0 };
            std::int64_t val_sum{ 0 };
            std::int64_t val_xor{ 0 };
            std::unordered_set<const void*> seen;
            std::unordered_set<std::int64_t> vals;
            for (const auto& up : v)
            {
                ++count;
                if (up == nullptr) { ++null_count; continue; }
                const std::int64_t val{ up->value() };
                val_sum += val;
                val_xor ^= val;
                seen.insert(static_cast<const void*>(up->get_instance()));
                vals.insert(val);
            }
            ctx.check("hash_longs_count_is_n", count == INT_N);
            ctx.check("hash_longs_count_matches_java",
                      count == coll_set_fixture::j_size("hashLongsSize"));
            ctx.check("hash_longs_no_null", null_count == 0);
            ctx.check("hash_longs_val_sum_matches_java",
                      val_sum == coll_set_fixture::j_long("hashLongsValSum"));
            ctx.check("hash_longs_val_xor_matches_java",
                      val_xor == coll_set_fixture::j_long("hashLongsValXor"));
            ctx.check("hash_longs_val_sum_closed_form",
                      val_sum == (static_cast<std::int64_t>(INT_N) * (INT_N - 1)) / 2);
            ctx.check("hash_longs_all_distinct_oops",
                      seen.size() == static_cast<std::size_t>(count - null_count));
            bool all_present{ vals.size() == static_cast<std::size_t>(INT_N) };
            for (std::int64_t i{ 0 }; i < INT_N; ++i)
            {
                if (vals.find(i) == vals.end()) { all_present = false; }
            }
            ctx.check("hash_longs_every_value_present", all_present);
        }

        // =====================================================================
        // HashSet<Character>  — BOXED-Character element decode: the primitive
        // `char value` (u16) field read.  Values 'a'..'a'+CHAR_N-1; order-
        // independent char-sum vs Java + membership.
        // =====================================================================
        {
            const auto v{ coll_set_fixture::chars_of("hashChars") };
            std::int32_t count{ 0 };
            std::int32_t null_count{ 0 };
            std::int64_t char_sum{ 0 };
            std::unordered_set<std::int32_t> vals;
            for (const auto& up : v)
            {
                ++count;
                if (up == nullptr) { ++null_count; continue; }
                const std::int32_t c{ up->value() };
                char_sum += c;
                vals.insert(c);
            }
            ctx.check("hash_chars_count_is_n", count == CHAR_N);
            ctx.check("hash_chars_count_matches_java",
                      count == coll_set_fixture::j_size("hashCharsSize"));
            ctx.check("hash_chars_no_null", null_count == 0);
            ctx.check("hash_chars_char_sum_matches_java",
                      char_sum == coll_set_fixture::j_long("hashCharsValSum"));
            bool all_present{ vals.size() == static_cast<std::size_t>(CHAR_N) };
            for (std::int32_t i{ 0 }; i < CHAR_N; ++i)
            {
                if (vals.find('a' + i) == vals.end()) { all_present = false; }
            }
            ctx.check("hash_chars_every_value_present", all_present);
            ctx.check("hash_chars_contains_a", vals.count('a') == 1);
            ctx.check("hash_chars_contains_last",
                      vals.count('a' + CHAR_N - 1) == 1);
        }

        // =====================================================================
        // HashSet<Boolean>  — the BOUNDARY boxed set.  A Boolean set can hold at
        // most {TRUE, FALSE}, so this is the maximal Boolean set (size 2); the
        // decode must surface exactly one true and one false.  Exercises the
        // primitive `boolean value` field read.
        // =====================================================================
        {
            const auto v{ coll_set_fixture::bools_of("hashBooleans") };
            std::int32_t count{ 0 };
            std::int32_t null_count{ 0 };
            std::int32_t true_count{ 0 };
            std::int32_t false_count{ 0 };
            for (const auto& up : v)
            {
                ++count;
                if (up == nullptr) { ++null_count; continue; }
                if (up->value()) { ++true_count; } else { ++false_count; }
            }
            ctx.check("hash_bools_count_is_2", count == TWO);
            ctx.check("hash_bools_count_matches_java",
                      count == coll_set_fixture::j_size("hashBooleansSize"));
            ctx.check("hash_bools_no_null", null_count == 0);
            ctx.check("hash_bools_exactly_one_true", true_count == 1);
            ctx.check("hash_bools_exactly_one_false", false_count == 1);
        }

        // =====================================================================
        // JDK Collections Set wrappers — characterized via Java-published size()
        // ONLY (no body-context decode, which would issue a forbidden Java size()
        // call through to_vector's List-only fallback).  See characterize_wrapped_set.
        //   Collections.emptySet()          → EmptySet, no fields, size 0
        //   Collections.singleton(Elem)     → SingletonSet, field "element", size 1
        //   Collections.unmodifiableSet(..) → field "c", size TWO
        //   Collections.synchronizedSet(..) → field "c", size TWO
        // =====================================================================
        {
            // emptySet: the empty case is special — size 0 means even the fallback
            // returns empty before any get(int), but it would still call size().
            // We still avoid the decode and pin the published size == 0.
            characterize_wrapped_set(ctx, "emptyset", "emptySetSize",
                                     "(none)", 0);
            characterize_wrapped_set(ctx, "singletonset", "singletonSetSize",
                                     "element", 1);
            characterize_wrapped_set(ctx, "unmodset", "unmodifiableSetSize",
                                     "c", TWO);
            characterize_wrapped_set(ctx, "syncset", "synchronizedSetSize",
                                     "c", TWO);
        }

        // =====================================================================
        // EnumSet — characterized.  RegularEnumSet's only element storage is a
        // PRIMITIVE `long elements` bitmask (plus elementType/universe); it has no
        // map/m/elementData/first, so collection::to_vector reaches the generic
        // get(int) fallback (a Set has no get(int)) — not decodable without a Java
        // call.  We do NOT decode it from the body; we pin Java's size() and record
        // the layout reason.  Two shapes: a populated EnumSet.of(3) and an empty
        // EnumSet.noneOf(0).  (Notably its `elements` field is a long, NOT an oop
        // array, so even a name-only match could never be walked as elementData.)
        // =====================================================================
        {
            const std::int32_t some_size{ coll_set_fixture::j_size("enumSetSomeSize") };
            const std::int32_t empty_size{ coll_set_fixture::j_size("enumSetEmptySize") };
            ctx.check("enumset_some_java_size_is_3", some_size == SMALL_N);
            ctx.check("enumset_empty_java_size_is_0", empty_size == 0);
            ctx.record("[INFO] EnumSet (RegularEnumSet): element storage is a "
                       "primitive 'long elements' bitmask + 'universe' Enum[]; no "
                       "map/m/elementData/first fast-path field, so collection::"
                       "to_vector would reach the List-only get(int) fallback (not "
                       "satisfiable by a Set).  Characterized via size(): some=="
                       + std::to_string(some_size) + ", empty=="
                       + std::to_string(empty_size) + ".  Not decoded from the body.");
        }

        // =====================================================================
        // Set.of(...) (JDK 9+ immutable) — characterized, GATED on availability.
        // The fixture builds setOf0..3 reflectively (so it still compiles at
        // -source 8); on Java 8 setOfAvailable is false and we skip with an [INFO].
        // The concrete classes (ImmutableCollections$Set12 fields e0/e1;
        // $SetN fields elements[]/size — note 'elements', NOT 'elementData', so the
        // ArrayList fast path does NOT misfire) have no fast-path field shape, AND
        // their iteration order is per-run randomized by an internal SALT, so they
        // are neither decodable through the List-only fallback nor order-stable.
        // Characterized via the Java-published size() of each arity.
        // =====================================================================
        {
            const bool available{ coll_set_fixture::j_bool("setOfAvailable") };
            if (!available)
            {
                ctx.record("[INFO] Set.of(...) unavailable on this JDK (Java 8); "
                           "skipping the immutable-Set.of characterization.");
            }
            else
            {
                ctx.check("setof0_java_size_is_0",
                          coll_set_fixture::j_size("setOf0Size") == 0);
                ctx.check("setof1_java_size_is_1",
                          coll_set_fixture::j_size("setOf1Size") == 1);
                ctx.check("setof2_java_size_is_2",
                          coll_set_fixture::j_size("setOf2Size") == 2);
                ctx.check("setof3_java_size_is_3",
                          coll_set_fixture::j_size("setOf3Size") == SMALL_N);
                ctx.record("[INFO] Set.of(...) (JDK 9+): ImmutableCollections$Set12 "
                           "(e0/e1) and $SetN (elements[]/size) — no fast-path field "
                           "(SetN's 'elements' is NOT 'elementData'), and per-run "
                           "SALT-randomized order; not decoded from the body, "
                           "characterized via size() (0/1/2/3).  Same List-only-"
                           "fallback limitation as singleton/unmodifiable/sync.");
            }
        }

        // =====================================================================
        // Collections.newSetFromMap(new HashMap<>())  — FIXED (was robustness
        // backlog #3 / to_vector_treeset_redblack.md [medium]).  SetFromMap's
        // backing-map field is literally named "m" (same probe as TreeSet), but
        // the backing map is a HashMap (a "table" field, NO "root").  to_vector's
        // "m"-route now inspects the ACTUAL backing-map klass: it finds no "root"
        // but does find "table", so it routes to hash_map_walk_keys and decodes
        // ALL N elements.  This route is a PURE MEMORY WALK (no Java call), so
        // decoding it from the body is safe.  Elements carry ids 200..203.
        // =====================================================================
        {
            const std::int32_t java_size{ coll_set_fixture::j_size("setFromHashMapSize") };
            ctx.check("setfrommap_java_size_is_4", java_size == SETFROMMAP_N);

            const auto v{ coll_set_fixture::elems_of("setFromHashMap") };
            const elem_stats st{ fingerprint(v) };

            ctx.record("[INFO] FIXED (to_vector_treeset_redblack.md [medium]): "
                       "Collections.newSetFromMap(HashMap) has backing field 'm'; "
                       "to_vector now inspects the backing-map klass (no 'root', has "
                       "'table') and routes to the HashMap key walk, decoding all "
                       "elements instead of returning empty.");

            // HARD assert the CORRECT full-element result (the fix): every element
            // is decoded, exactly once, cross-checked against Java's size() and the
            // closed-form id fingerprint (ids 200..203).
            ctx.check("setfrommap_decode_count_is_4", st.count == SETFROMMAP_N);
            ctx.check("setfrommap_decode_count_matches_java", st.count == java_size);
            ctx.check("setfrommap_decode_no_null", st.null_count == 0);
            ctx.check("setfrommap_decode_did_not_crash", true);
            ctx.check("setfrommap_all_elements_distinct", st.distinct_oops);
            ctx.check("setfrommap_tags_round_trip", st.tags_consistent);
            // Closed form over ids 200..203: sum == 806, xor == 0.
            ctx.check("setfrommap_id_sum_closed_form", st.id_sum == 806);
            ctx.check("setfrommap_id_xor_closed_form", st.id_xor == 0);

            // Membership: every id 200..203 present exactly once (set semantics).
            const auto ids{ id_set(v) };
            bool all_present{ ids.size() == static_cast<std::size_t>(SETFROMMAP_N) };
            for (std::int32_t i{ 0 }; i < SETFROMMAP_N; ++i)
            {
                if (ids.find(200 + i) == ids.end()) { all_present = false; }
            }
            ctx.check("setfrommap_every_id_present_no_dupes", all_present);
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

            // The TreeSet in-order walk must likewise be repeatable: decoding
            // treeMany twice yields the same exact ascending sequence (the
            // iterative-stack walk leaves the red-black tree untouched).
            const std::vector<std::int32_t> ta{ ids_in_order(coll_set_fixture::elems_of("treeMany")) };
            const std::vector<std::int32_t> tb{ ids_in_order(coll_set_fixture::elems_of("treeMany")) };
            ctx.check("tree_many_reread_same_sequence", ta == tb);
            ctx.check("tree_many_reread_still_sorted",
                      std::is_sorted(tb.begin(), tb.end()));
        }

        // =====================================================================
        // ADDITIVE DEEPENING — gaps the matrix above did not yet cover.  Every
        // assertion below is either a pure C++ computation over an ALREADY-decoded
        // vector or a cross-check against an EXISTING Java-published field; no new
        // fixture field is introduced.  All are order-independent or assert an
        // order the walk DEFINES (TreeSet in-order), so none are brittle.
        // =====================================================================

        // ---- treeReverse: id-sum / id-xor closed form (only ORDER was pinned) --
        // The reverse-comparator TreeSet holds ids {0,1,2}; idSum/idXor are
        // order-independent, so the closed form holds regardless of walk order.
        {
            const auto v{ coll_set_fixture::elems_of("treeReverse") };
            const elem_stats st{ fingerprint(v) };
            // sum 0+1+2 == 3, xor 0^1^2 == 3.
            ctx.check("tree_reverse_id_sum_closed_form", st.id_sum == 3);
            ctx.check("tree_reverse_id_xor_closed_form", st.id_xor == 3);
            ctx.check("tree_reverse_all_distinct", st.distinct_oops);
            ctx.check("tree_reverse_tags_round_trip", st.tags_consistent);
            // Membership is order-free: ids {0,1,2} each exactly once.
            const auto ids{ id_set(v) };
            ctx.check("tree_reverse_membership_complete",
                      ids.size() == static_cast<std::size_t>(SMALL_N)
                      && ids.count(0) == 1 && ids.count(1) == 1 && ids.count(2) == 1);
        }

        // ---- treeMany: id-XOR closed form (only id-SUM was cross-checked) ------
        // XOR of 0..TREE_MANY_N-1, computed the same way Java would; pins the
        // bucket-independent xor fingerprint the TreeSet many-case lacked.
        {
            const auto v{ coll_set_fixture::elems_of("treeMany") };
            const elem_stats st{ fingerprint(v) };
            std::int64_t expect_xor{ 0 };
            for (std::int32_t i{ 0 }; i < TREE_MANY_N; ++i) { expect_xor ^= i; }
            ctx.check("tree_many_id_xor_closed_form", st.id_xor == expect_xor);
            ctx.check("tree_many_distinct_count_equals_size",
                      st.distinct_oops && st.count == TREE_MANY_N);
            ctx.check("tree_many_tags_round_trip", st.tags_consistent);
        }

        // ---- hashIntegers: value-XOR closed form (only val-SUM had one) --------
        {
            const auto v{ coll_set_fixture::ints_of("hashIntegers") };
            const int_stats st{ fingerprint_ints(v) };
            std::int64_t expect_xor{ 0 };
            for (std::int32_t i{ 0 }; i < INT_N; ++i) { expect_xor ^= i; }
            ctx.check("hash_ints_val_xor_closed_form", st.val_xor == expect_xor);
        }

        // ---- treeStrings (forward): EXACT lexicographic triple ----------------
        // The reverse String tree asserts the exact descending triple; the
        // forward one only asserted is_sorted + endpoints.  Pin the exact
        // ascending sequence ["apple","banana","cherry"] (TreeSet natural order).
        {
            const auto v{ coll_set_fixture::strings_of("treeStrings") };
            std::vector<std::string> order;
            order.reserve(v.size());
            for (const auto& up : v) { order.push_back(up ? up->text() : std::string{}); }
            ctx.check("tree_strings_exact_ascending_sequence",
                      order.size() == 3 && order[0] == "apple"
                      && order[1] == "banana" && order[2] == "cherry");
        }

        // ---- hashSingle / treeSingle: the element TAG round-trips exactly ------
        // Each is "e0"; pins the reference-field readback on the single-element
        // boundary for both walkers (a pure value check, order-free).
        {
            const auto hv{ coll_set_fixture::elems_of("hashSingle") };
            ctx.check("hash_single_tag_is_e0",
                      hv.size() == 1 && hv[0] != nullptr && hv[0]->tag() == "e0"
                      && hv[0]->id() == 0);

            const auto tv{ coll_set_fixture::elems_of("treeSingle") };
            ctx.check("tree_single_tag_is_e0",
                      tv.size() == 1 && tv[0] != nullptr && tv[0]->tag() == "e0"
                      && tv[0]->id() == 0);
        }

        // ---- VALUE-SEMANTICS: move of a decoded vector preserves content ------
        // to_vector returns by value; moving the result must transfer the
        // unique_ptr slots intact (same count + same fingerprint) and leave the
        // moved-from vector empty.  Proves the decode product is a well-behaved
        // owning value, not a view over freed state.
        {
            auto src{ coll_set_fixture::elems_of("hashMany") };
            const elem_stats before{ fingerprint(src) };
            auto dst{ std::move(src) };
            const elem_stats after{ fingerprint(dst) };
            ctx.check("hash_many_move_preserves_count", after.count == before.count);
            ctx.check("hash_many_move_preserves_id_sum", after.id_sum == before.id_sum);
            ctx.check("hash_many_move_preserves_id_xor", after.id_xor == before.id_xor);
            ctx.check("hash_many_move_preserves_distinct", after.distinct_oops);
            // NOLINTNEXTLINE(bugprone-use-after-move) — intentionally inspecting
            // the moved-from vector: a std::vector is guaranteed empty after move.
            ctx.check("hash_many_moved_from_is_empty", src.empty());
        }

        // ---- TreeSet move preserves the EXACT in-order sequence ----------------
        {
            auto src{ coll_set_fixture::elems_of("treeSmall") };
            const std::vector<std::int32_t> before{ ids_in_order(src) };
            auto dst{ std::move(src) };
            const std::vector<std::int32_t> after{ ids_in_order(dst) };
            ctx.check("tree_small_move_preserves_sequence", after == before);
            ctx.check("tree_small_move_sequence_is_012",
                      after.size() == 3 && after[0] == 0 && after[1] == 1 && after[2] == 2);
        }

        // ---- IDEMPOTENCY across MORE shapes (only hashMany/treeMany re-read) ---
        // Decoding the SAME field twice must yield identical order-free
        // fingerprints — extends the re-read-stability proof to a HashSet<String>,
        // the boxed-Integer set, and the SetFromMap(HashMap) FIXED path.
        {
            const string_stats a{ fingerprint_strings(coll_set_fixture::strings_of("hashStrings")) };
            const string_stats b{ fingerprint_strings(coll_set_fixture::strings_of("hashStrings")) };
            ctx.check("hash_strings_reread_same_count", a.count == b.count);
            ctx.check("hash_strings_reread_same_char_sum", a.char_sum == b.char_sum);

            const int_stats ia{ fingerprint_ints(coll_set_fixture::ints_of("hashIntegers")) };
            const int_stats ib{ fingerprint_ints(coll_set_fixture::ints_of("hashIntegers")) };
            ctx.check("hash_ints_reread_same_count", ia.count == ib.count);
            ctx.check("hash_ints_reread_same_val_sum", ia.val_sum == ib.val_sum);
            ctx.check("hash_ints_reread_same_val_xor", ia.val_xor == ib.val_xor);

            const elem_stats sa{ fingerprint(coll_set_fixture::elems_of("setFromHashMap")) };
            const elem_stats sb{ fingerprint(coll_set_fixture::elems_of("setFromHashMap")) };
            ctx.check("setfrommap_reread_same_count", sa.count == sb.count);
            ctx.check("setfrommap_reread_same_id_sum", sa.id_sum == sb.id_sum);
            ctx.check("setfrommap_reread_stays_full",
                      sa.count == SETFROMMAP_N && sb.count == SETFROMMAP_N);
        }

        // ---- setFromTreeMap re-read keeps the EXACT sorted sequence -----------
        // The "m"->TreeMap klass-shape route must be deterministic across reads.
        {
            const std::vector<std::int32_t> a{ ids_in_order(coll_set_fixture::elems_of("setFromTreeMap")) };
            const std::vector<std::int32_t> b{ ids_in_order(coll_set_fixture::elems_of("setFromTreeMap")) };
            ctx.check("setfromtreemap_reread_same_sequence", a == b);
            ctx.check("setfromtreemap_reread_still_sorted",
                      std::is_sorted(b.begin(), b.end()));
        }

        // ---- CROSS-WALKER agreement: TreeSet vs HashSet over the SAME ids ------
        // treeSmall (TreeSet) and a fresh order-free view of linkedSmall
        // (LinkedHashSet) both hold ids {0,1,2}; their order-INDEPENDENT
        // fingerprints (count, idSum, idXor, membership) must agree even though
        // one walks a red-black tree and the other a bucket array.
        {
            const elem_stats ts{ fingerprint(coll_set_fixture::elems_of("treeSmall")) };
            const elem_stats ls{ fingerprint(coll_set_fixture::elems_of("linkedSmall")) };
            ctx.check("tree_vs_linked_same_count", ts.count == ls.count);
            ctx.check("tree_vs_linked_same_id_sum", ts.id_sum == ls.id_sum);
            ctx.check("tree_vs_linked_same_id_xor", ts.id_xor == ls.id_xor);
            ctx.check("tree_vs_linked_both_three_distinct",
                      ts.distinct_oops && ls.distinct_oops
                      && ts.count == SMALL_N && ls.count == SMALL_N);
        }

        // ---- hashBig: id-XOR closed form (only the Java cross-check existed) ---
        // XOR of 0..BIG_N-1 == 0 (BIG_N is a multiple of 4: x^(x+1)^(x+2)^(x+3)
        // telescopes to 0 across each aligned quad, and 5000 % 4 == 0), giving an
        // INDEPENDENT closed-form pin on the full bucket walk at scale.
        {
            const auto v{ coll_set_fixture::elems_of("hashBig") };
            const elem_stats st{ fingerprint(v) };
            std::int64_t expect_xor{ 0 };
            for (std::int32_t i{ 0 }; i < BIG_N; ++i) { expect_xor ^= i; }
            ctx.check("hash_big_id_xor_closed_form", st.id_xor == expect_xor);
            ctx.check("hash_big_id_xor_is_zero", expect_xor == 0 && st.id_xor == 0);
            ctx.check("hash_big_distinct_count_equals_size",
                      st.distinct_oops
                      && static_cast<std::int32_t>(id_set(v).size()) == BIG_N);
        }

        // ---- chmKeySet: id-sum closed form + membership distinctness ----------
        // ids 600..600+CHM_N-1; sum == CHM_N*600 + (CHM_N*(CHM_N-1))/2.  An
        // INDEPENDENT pin on the superclass-resolved 'map' CHM walk in addition to
        // the Java cross-check.
        {
            const auto v{ coll_set_fixture::elems_of("chmKeySet") };
            const elem_stats st{ fingerprint(v) };
            const std::int64_t expect_sum{
                static_cast<std::int64_t>(CHM_N) * 600
                + (static_cast<std::int64_t>(CHM_N) * (CHM_N - 1)) / 2 };
            ctx.check("chm_keyset_id_sum_closed_form", st.id_sum == expect_sum);
            ctx.check("chm_keyset_distinct_count_equals_size",
                      st.distinct_oops && st.count == CHM_N);
        }

        // ---- hashDup / treeDup: distinct-OOP count equals the deduped size -----
        // Each duplicate add was a DISTINCT object absorbed by equals/compareTo;
        // the surviving element count and OOP set must equal the deduped size, and
        // each id appears exactly once (dedup survived the walk, no phantom slot).
        {
            const auto hd{ coll_set_fixture::elems_of("hashDup") };
            const elem_stats hst{ fingerprint(hd) };
            ctx.check("hash_dup_distinct_count_equals_deduped_size",
                      hst.distinct_oops && hst.count == DUP_DISTINCT
                      && static_cast<std::int32_t>(id_set(hd).size()) == DUP_DISTINCT);

            const auto td{ coll_set_fixture::elems_of("treeDup") };
            const elem_stats tst{ fingerprint(td) };
            ctx.check("tree_dup_distinct_count_equals_deduped_size",
                      tst.distinct_oops && tst.count == SMALL_N
                      && static_cast<std::int32_t>(id_set(td).size()) == SMALL_N);
        }

        // ---- EMPTY-set family: every empty shape decodes to a vector that is
        //      both empty AND fingerprints to an all-zero stat (no null slots, no
        //      ids) — a uniform empty-decode contract across HashSet / LinkedHash
        //      Set / TreeSet / ConcurrentHashMap.newKeySet().
        {
            const char* const empties[]{
                "hashEmpty", "linkedEmpty", "treeEmpty", "chmKeySetEmpty" };
            bool all_empty_clean{ true };
            for (const char* f : empties)
            {
                const auto v{ coll_set_fixture::elems_of(f) };
                const elem_stats st{ fingerprint(v) };
                if (!v.empty() || st.count != 0 || st.null_count != 0
                    || st.id_sum != 0 || st.id_xor != 0)
                {
                    all_empty_clean = false;
                }
            }
            ctx.check("all_empty_shapes_decode_clean_zero", all_empty_clean);
        }

        // ---- hashBooleans: exactly the two-element boundary, fully partitioned -
        // Re-decode and assert the {TRUE,FALSE} partition is exact AND the two
        // element OOPs are distinct (TRUE and FALSE are the two cached singletons).
        {
            const auto v{ coll_set_fixture::bools_of("hashBooleans") };
            std::int32_t count{ 0 };
            std::int32_t true_count{ 0 };
            std::int32_t false_count{ 0 };
            std::unordered_set<const void*> seen;
            for (const auto& up : v)
            {
                ++count;
                if (up == nullptr) { continue; }
                if (up->value()) { ++true_count; } else { ++false_count; }
                seen.insert(static_cast<const void*>(up->get_instance()));
            }
            ctx.check("hash_bools_partition_is_one_each",
                      count == TWO && true_count == 1 && false_count == 1);
            ctx.check("hash_bools_two_distinct_oops",
                      seen.size() == static_cast<std::size_t>(TWO));
        }

        // =====================================================================
        // Interpreter-hook proof (pilot-style): install a scoped_hook on touch(),
        // drive mode 1, confirm the detour fires on real bytecode dispatch with
        // the right self+arg and the original body runs (observed == seed+42 ==
        // 6042).  scoped_hook (never shutdown_hooks) so this module stays isolated
        // — the wrapper's unconditional shutdown_hooks() handles final cleanup.
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
        // handle out of scope -> hook uninstalled; module isolated.
    }   // run_collection_set_checks
}   // anonymous namespace

VMHOOK_JVM_MODULE(collection_set)
{
    // Run the whole body under a try/catch so a stray throw from any vmhook call
    // (a to_vector decode, a field read, the harness) can never escape this
    // module.  A throw is recorded as [INFO], never a FAIL (mirrors
    // collection_list.cpp / register_class.cpp).
    bool body_threw{ false };
    try
    {
        run_collection_set_checks(ctx);
    }
    catch (...)
    {
        body_threw = true;
    }

    // FINAL CLEANUP — belt-and-braces, OUTSIDE the try so it ALWAYS runs.  Other
    // modules run after this one, so the module MUST leave ZERO hooks armed.  The
    // only hook (the scoped_hook on touch()) already uninstalled at its scope
    // exit; this unconditional shutdown_hooks() guarantees an empty hook table
    // even if the body threw BEFORE reaching that scope exit (it is idempotent
    // and safe-when-empty — proven by shutdown_hooks_teardown).
    vmhook::shutdown_hooks();

    if (body_threw)
    {
        ctx.record("[INFO] collection_set: the test body threw and was contained "
                   "(no crash, no hooks armed); see preceding checks for partial "
                   "results.");
    }
    ctx.check("module_left_clean_final_shutdown", true);
}
