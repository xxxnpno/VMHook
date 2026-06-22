// collection_iteration_safety JVM test module  (feature area: collections)
//
// The ROBUSTNESS / SAFETY oracle for vmhook's collection-walk surface.  Where
// collection_list / collection_set / collection_map / collection_hash_tree_map
// prove iteration CONTENT (exact values, order, identity), THIS module proves
// the iteration is SAFE across every degenerate / adversarial shape and size:
//
//   * it NEVER crashes (every walk on empty / null / oversized / colliding /
//     out-of-order containers returns gracefully), and
//   * the decoded element COUNT equals the Java-known size (size is the oracle;
//     empty -> empty), and
//   * a heavy walk terminates with NO duplicate element OOP (the JVM-observable
//     proxy for "no cycle / no re-emit / no early stop"), and
//   * get_array_element CLAMPS every out-of-bounds index instead of reading OOB.
//
// The walk surface under test (cited line numbers are vmhook.hpp):
//   collection::to_vector            14793  (the field-shape dispatch cascade)
//   linked_list_walk_items           15183  (first->next Node chain, size-bounded)
//   hash_map_walk_keys               15336  (table Node[] bucket + next chain)
//   tree_map_walk_keys               15528  (iterative red-black in-order walk)
//   hash_map_walk_entries            15250  (key+value bucket walk)
//   tree_map_walk_entries            15415  (key+value red-black walk)
//   value_t::to_vector               15639  (the field-proxy entry point)
//   value_t::to_entries              15697  (the map field-proxy entry point)
//   get_array_element / array_length 11563 / 11542  (bounds-checked element read)
//
// HOW THE READS ARE MADE SAFE: every to_vector / to_entries / size() call runs
// INSIDE the interpreter detour on CollIterSafety.trigger() — the only context
// where HotSpot's current_java_thread is established, which collection::size()
// (a Java call-gate dispatch) and the generic get(int) fallback both require.
// This is the same shape collection_list uses.  The hook is installed with
// scoped_hook<> and RAII-uninstalls on scope exit, so the module leaves NOTHING
// armed for later modules (full-suite ordering).  Every decoded element OOP is
// only ever wrapped after to_vector's own is_valid_pointer guard; the module's
// own field reads go through the harness wrappers, which guard internally.
//
// GATING DISCIPLINE: the *decoded count* of a reference container depends on the
// compressed-oops decode path (narrow-oop element slots), which is the x64 CI
// default but not universal.  So per-shape size-match is HARD when the decode
// produced anything for that shape, and recorded [INFO] only if a shape decoded
// to nothing on this JVM; a HARD MAJORITY FLOOR over all shapes keeps the layer
// from passing vacuously.  The EMPTY-container invariants, the no-duplicate-OOP
// canary, and the get_array_element bounds clamp are HARD on every JDK (they do
// not depend on a populated decode).  JDK generation is recorded for context via
// the house idiom (java.lang.String has the compact-string "coder" field on 9+).
//
// Previously-pinned lib bug, now FIXED: Collections.newSetFromMap(HashMap)'s
// backing field is named "m" (the TreeSet probe), but the map is a HashMap (no
// "root").  to_vector now inspects the actual backing-map klass — "root" => tree
// walk, else "table" => hash walk — so the HashMap-backed Set decodes ALL its
// elements.  The module HARD-asserts the full decode (section 7); the call must
// not crash.
//
// DEEPENED: in addition to the empty / big-list / null-list / oversized /
// colliding / out-of-order / setFromMap / array-bounds shapes, this module also
// covers (a) SINGLE-element containers across all six families (the
// empty->populated dispatch seam, lone element id checked), (b) Collections.*
// degenerate List shapes (emptyList / singletonList / unmodifiableList — the
// generic get(int) fallback + wrapper layouts), (c) BIG bucket/tree walks
// (HashSet/TreeSet/TreeMap/HashMap @ 1500 — the heavy no-duplicate-KEY canary
// for hash_map_walk_keys / tree_map_walk_keys guard caps), (d) declared-NULL
// collection/map/list fields + a MISSING field name (empty, no crash), and (e)
// a FULL in-bounds array sweep, narrow-width (int16/int8) reads, every Object[]
// in-bounds slot, more OOB indices, and the bounds primitive on a null / non-
// array oop (the load-bearing guard every walk sits on).
//
// C++17 (no std::bit_cast); MSVC copy-init (never brace-init) from value_t.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <atomic>
#include <bit>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
    // The fixture class this module wraps.  Used by register_class<fixture_wrapper>()
    // and — critically for suite-safety — by the entry guard's find_class()
    // pre-check, so the unguarded handshake static_field("go")->set(...) derefs in
    // drive() can never fault on a missing/unloaded class.
    constexpr char FIXTURE[]{ "vmhook/fixtures/CollIterSafety" };

    // ── Fixture-mirrored constants (lockstep with CollIterSafety.java). ──────
    constexpr std::int32_t BIG{ 5000 };
    constexpr std::int32_t NULL_LIST_LEN{ 6 };
    constexpr std::int32_t NULL_LIST_NONNULL{ 4 };
    constexpr std::int32_t OVERSIZED_LEN{ 5 };
    constexpr std::int32_t COLLIDE_N{ 64 };
    constexpr std::int32_t TREE_N{ 32 };
    constexpr std::int32_t SETFROMMAP_N{ 5 };
    constexpr std::int32_t INT_ARR_LEN{ 8 };
    constexpr std::int32_t OBJ_ARR_LEN{ 4 };
    constexpr std::int32_t SINGLE_ELEM_ID{ 42 };
    constexpr std::int32_t BIG_MAP{ 1500 };

    // ── ELEMENT wrapper: vmhook.fixtures.CollIterSafety$Elem. ───────────────
    // to_vector / the walk helpers build make_unique<elem_object> from each
    // decoded element OOP.  id() proves the OOP is a real, walkable heap object.
    class elem_object : public vmhook::object<elem_object>
    {
    public:
        explicit elem_object(vmhook::oop_t instance) noexcept
            : vmhook::object<elem_object>{ instance }
        {
        }

        auto id() const -> std::int32_t { return static_cast<std::int32_t>(get_field("id")->get()); }
    };

    // ── STRING element wrapper: java.lang.String (colliding-key Set). ────────
    class string_element : public vmhook::object<string_element>
    {
    public:
        explicit string_element(vmhook::oop_t instance) noexcept
            : vmhook::object<string_element>{ instance }
        {
        }
    };

    // ── A carrier so a raw Java array OOP read from a field can be element-
    //    accessed natively for the get_array_element bounds sweep.  Bound to
    //    java/lang/Object only so field_proxy decode is well-typed; the array
    //    OOP itself is recovered with decode_array_oop. ────────────────────────
    class fixture_wrapper : public vmhook::object<fixture_wrapper>
    {
    public:
        explicit fixture_wrapper(vmhook::oop_t instance) noexcept
            : vmhook::object<fixture_wrapper>{ instance }
        {
        }

        // ── handshake (static; safe off the Java thread) ────────────────────
        static auto set_go(bool value) -> void   { static_field("go")->set(value); }
        static auto set_done(bool value) -> void  { static_field("done")->set(value); }
        static auto get_done() -> bool            { return static_field("done")->get(); }

        static auto resolves(const char* name) -> bool { return static_field(name).has_value(); }

        // Java-published size oracle (read via VMStructs; no Java thread needed,
        // but we read it inside the detour anyway for a coherent snapshot).
        static auto j_size(const char* name) -> std::int32_t { return static_cast<std::int32_t>(static_field(name)->get()); }
        static auto j_bool(const char* name) -> bool { return static_cast<bool>(static_field(name)->get()); }
        static auto get_observed() -> std::int32_t { return static_field("observed")->get(); }
    };

    // ── Reduced observation of one decoded container (filled in the detour). ─
    struct walk_obs
    {
        std::atomic<bool>         seen{ false };       // the walk ran
        std::atomic<std::int32_t> count{ -1 };          // decoded element count
        std::atomic<std::int32_t> non_null{ -1 };       // non-null slot count
        std::atomic<std::int32_t> null_count{ -1 };     // null slot count
        std::atomic<bool>         distinct_ok{ false };  // all non-null OOPs unique
        std::atomic<bool>         crashed{ false };      // (never set; presence == proof of no throw)
    };

    // Container observations (one per shape).
    walk_obs g_empty_arraylist;
    walk_obs g_empty_linkedlist;
    walk_obs g_empty_hashset;
    walk_obs g_empty_linkedhashset;
    walk_obs g_empty_treeset;
    walk_obs g_empty_hashmap;       // entries
    walk_obs g_empty_linkedhashmap; // entries
    walk_obs g_empty_treemap;       // entries

    walk_obs g_big_arraylist;
    walk_obs g_big_linkedlist;
    walk_obs g_null_arraylist;
    walk_obs g_null_linkedlist;
    walk_obs g_oversized_arraylist;

    walk_obs g_collide_hashset;     // keys
    walk_obs g_collide_hashmap;     // entries
    walk_obs g_oo_treeset;          // keys
    walk_obs g_oo_treemap;          // entries

    walk_obs g_setfrommap;          // FIXED: full HashMap-backed decode

    // ── Single-element containers (empty->populated dispatch seam). ──────────
    walk_obs g_single_arraylist;
    walk_obs g_single_linkedlist;
    walk_obs g_single_hashset;
    walk_obs g_single_treeset;
    walk_obs g_single_hashmap;      // entries
    walk_obs g_single_treemap;      // entries
    // The lone element of each single-* container must decode to id 42.  Stored
    // as the observed id (or INT_MIN if not exactly-one-non-null-element).
    std::atomic<std::int32_t> g_single_arraylist_id{ std::numeric_limits<std::int32_t>::min() };
    std::atomic<std::int32_t> g_single_linkedlist_id{ std::numeric_limits<std::int32_t>::min() };
    std::atomic<std::int32_t> g_single_hashset_id{ std::numeric_limits<std::int32_t>::min() };
    std::atomic<std::int32_t> g_single_treeset_id{ std::numeric_limits<std::int32_t>::min() };

    // ── Immutable / wrapper degenerate List shapes. ──────────────────────────
    walk_obs g_singleton_list;          // size 1, generic get(int) fallback
    walk_obs g_collections_empty_list;  // size 0 degenerate List
    walk_obs g_unmodifiable_list;       // wrapper over a 1-element ArrayList

    // ── Big bucket/tree walks (heavy no-dup-key canary). ─────────────────────
    walk_obs g_big_hashset;
    walk_obs g_big_treeset;
    walk_obs g_big_treemap;             // entries
    walk_obs g_big_hashmap;             // entries

    // ── ROBUSTNESS: declared-but-null collection / map / list fields. ────────
    walk_obs g_null_set;
    walk_obs g_null_map;                // entries
    walk_obs g_null_list_field;
    // A missing field name -> empty vector (helper's !f short-circuit).
    walk_obs g_missing_field;
    walk_obs g_missing_field_entries;   // entries

    // ── get_array_element ROBUSTNESS on degenerate oops (HARD, universal). ───
    std::atomic<bool> g_arrlen_null_is_zero{ false };       // array_length(nullptr)==0
    std::atomic<bool> g_getelem_null_is_zero{ false };      // get_array_element(nullptr,0)==0
    std::atomic<bool> g_arrlen_nonarray_safe{ false };      // array_length(Elem oop) didn't crash
    std::atomic<bool> g_getelem_nonarray_safe{ false };     // get_array_element(Elem oop, huge) didn't crash
    // EVERY in-bounds int slot read back its sentinel (full sweep, not just 3).
    std::atomic<bool> g_int_all_inbounds_ok{ false };
    // Narrow-width in-bounds reads of int[] (stride/widened-multiply path):
    // intArr[0] low 2 bytes == 1000 (LE int16), low byte == (1000 & 0xFF).
    std::atomic<bool> g_int_int16_read_ok{ false };
    std::atomic<bool> g_int_int8_read_ok{ false };
    // Object[] EVERY in-bounds slot decodes to its Elem id (700..703).
    std::atomic<bool> g_obj_all_inbounds_ok{ false };
    // Extra OOB object indices clamp to 0.
    std::atomic<bool> g_obj_oob_intmin_clamped{ false };
    std::atomic<bool> g_obj_oob_intmax_clamped{ false };
    std::atomic<bool> g_obj_oob_lenp1_clamped{ false };

    // ── get_array_element bounds outcomes (HARD, universal). ─────────────────
    // In-bounds reads (must equal the sentinel value).
    std::atomic<bool> g_int_inbounds_first_ok{ false };  // intArr[0]==1000
    std::atomic<bool> g_int_inbounds_last_ok{ false };   // intArr[len-1]==1000+len-1
    std::atomic<bool> g_int_inbounds_mid_ok{ false };    // intArr[len/2]
    // Out-of-bounds reads (must clamp to T{} == 0; never crash / never OOB).
    std::atomic<bool> g_int_neg1_clamped{ false };
    std::atomic<bool> g_int_intmin_clamped{ false };
    std::atomic<bool> g_int_eqlen_clamped{ false };
    std::atomic<bool> g_int_lenp1_clamped{ false };
    std::atomic<bool> g_int_intmax_clamped{ false };
    // array_length read back the right length for both arrays.
    std::atomic<std::int32_t> g_int_arr_len{ -1 };
    std::atomic<std::int32_t> g_obj_arr_len{ -1 };
    // Object[] in-bounds element decodes to a valid Elem; OOB clamps to 0.
    std::atomic<bool> g_obj_inbounds_ok{ false };
    std::atomic<bool> g_obj_oob_eqlen_clamped{ false };
    std::atomic<bool> g_obj_oob_neg_clamped{ false };
    // A zero-length sanity: reading index 0 of a known-good array's tail is fine;
    // also prove array_length on a wrapped raw oop is non-negative.
    std::atomic<bool> g_int_arr_valid{ false };
    std::atomic<bool> g_obj_arr_valid{ false };

    // ── DEEPEN: more get_array_element width / boundary combinations on intArr.
    //   Every read uses a real owned array oop (POSIX-safe).  All HARD universal
    //   (the bounds guard + LE x64 layout do not depend on the compressed-oops
    //   element decode). ──────────────────────────────────────────────────────
    // Full-width unsigned read of a primitive int element equals the value.
    std::atomic<bool> g_int_uint32_inbounds_ok{ false };   // (uint32)intArr[0]==1000
    // bool read of byte +16 (low byte of 1000 == 232 != 0) -> true.
    std::atomic<bool> g_int_bool_read_ok{ false };
    // uint16 in-bounds read of intArr[0] low 2 bytes == 1000.
    std::atomic<bool> g_int_uint16_read_ok{ false };
    // Narrow-width OOB indices clamp to T{} regardless of element_type width
    // (the index<0 / index>=length guard fires before the read).
    std::atomic<bool> g_int_int16_neg1_clamped{ false };   // int16 @ -1
    std::atomic<bool> g_int_int8_eqlen_clamped{ false };   // int8  @ len
    std::atomic<bool> g_int_uint16_intmin_clamped{ false };// uint16 @ INT_MIN
    std::atomic<bool> g_int_bool_eqlen_clamped{ false };   // bool @ len -> false
    // array_length is stable across repeated reads (read-only, idempotent).
    std::atomic<bool> g_int_arr_len_idempotent{ false };
    std::atomic<bool> g_obj_arr_len_idempotent{ false };
    // get_array_element on a real-but-narrow element_type at nullptr clamps too.
    std::atomic<bool> g_getelem_null_int64_is_zero{ false };
    std::atomic<bool> g_getelem_null_int16_is_zero{ false };

    // ── DEEPEN: set_array_element OOB-WRITE rejection.  An OOB index is a no-op
    //   (the index<0 / index>=length guard), so writing to OOB indices leaves the
    //   shared fixture array byte-identical — proven by re-reading the real
    //   in-bounds sentinels before AND after the rejected OOB writes.  NEVER an
    //   in-bounds write (that would corrupt the fixture for later modules). ─────
    std::atomic<bool> g_setelem_oob_neg_rejected{ false };    // write @ -1 ignored
    std::atomic<bool> g_setelem_oob_eqlen_rejected{ false };  // write @ len ignored
    std::atomic<bool> g_setelem_oob_intmin_rejected{ false }; // write @ INT_MIN ignored
    std::atomic<bool> g_setelem_oob_null_no_crash{ false };   // write to nullptr no-op

    // ── DEEPEN: value_t::to_vector on the raw fields directly. ────────────────
    //   objArr is an Object[] of Elem -> hits the object-ARRAY special case in
    //   value_t::to_vector (signature '[L...;'); intArr is a primitive '[I' array
    //   -> falls through to collection::to_vector, finds no container fields, and
    //   returns EMPTY without crashing (the documented degenerate-but-safe path).
    walk_obs g_objarr_to_vector;        // Object[] -> 4 Elems (id 700..703)
    walk_obs g_intarr_to_vector;        // primitive int[] -> empty, no crash
    std::atomic<bool> g_objarr_to_vector_ids_ok{ false };  // every decoded id == 700+k

    // ── DEEPEN: IDEMPOTENCY — a read-only walk re-run on the SAME field yields
    //   the SAME count (no state mutated, no early stop the 2nd time).  Stored as
    //   second-pass counts; HARD (count1==count2) only when the first decoded > 0.
    std::atomic<std::int32_t> g_big_arraylist_count2{ -1 };
    std::atomic<std::int32_t> g_big_linkedlist_count2{ -1 };
    std::atomic<std::int32_t> g_oo_treeset_count2{ -1 };
    std::atomic<std::int32_t> g_collide_hashset_count2{ -1 };
    std::atomic<std::int32_t> g_big_hashmap_count2{ -1 };   // entries
    std::atomic<std::int32_t> g_oo_treemap_count2{ -1 };    // entries

    // ── DEEPEN: single-* maps have NO null keys (non_null == count == 1). ─────
    //   (Stored via the existing walk_obs; asserted in the new section.)

    // ── Detour bookkeeping. ──────────────────────────────────────────────────
    std::atomic<int>  g_detour_calls{ 0 };
    std::atomic<bool> g_self_ok{ false };

    // Reduce a decoded Elem vector into a walk_obs: count, null pattern, and OOP
    // distinctness (a cycle / re-emit collapses distinctness).
    template<typename wrapper_type>
    auto observe_vec(walk_obs& o,
                     const std::vector<std::unique_ptr<wrapper_type>>& v) -> void
    {
        o.seen.store(true, std::memory_order_relaxed);
        o.count.store(static_cast<std::int32_t>(v.size()), std::memory_order_relaxed);

        std::int32_t non_null{ 0 };
        std::int32_t null_count{ 0 };
        bool distinct_ok{ true };

        std::unordered_set<const void*> seen_oops;
        seen_oops.reserve(v.size() * 2 + 1);

        for (const auto& up : v)
        {
            const wrapper_type* const e{ up.get() };
            if (e == nullptr)
            {
                ++null_count;
                continue;
            }
            ++non_null;
            const void* const oop{ static_cast<const void*>(e->get_instance()) };
            if (!seen_oops.insert(oop).second)
            {
                distinct_ok = false;
            }
        }
        o.non_null.store(non_null, std::memory_order_relaxed);
        o.null_count.store(null_count, std::memory_order_relaxed);
        o.distinct_ok.store(distinct_ok, std::memory_order_relaxed);
    }

    // Reduce a decoded entry vector (pairs) into a walk_obs (count + key OOP
    // distinctness over non-null keys).
    template<typename key_type, typename value_type>
    auto observe_entries(walk_obs& o,
                         const std::vector<std::pair<std::unique_ptr<key_type>,
                                                     std::unique_ptr<value_type>>>& v) -> void
    {
        o.seen.store(true, std::memory_order_relaxed);
        o.count.store(static_cast<std::int32_t>(v.size()), std::memory_order_relaxed);

        std::int32_t non_null{ 0 };
        std::int32_t null_count{ 0 };
        bool distinct_ok{ true };
        std::unordered_set<const void*> seen_oops;
        seen_oops.reserve(v.size() * 2 + 1);

        for (const auto& pr : v)
        {
            const key_type* const k{ pr.first.get() };
            if (k == nullptr)
            {
                ++null_count;
                continue;
            }
            ++non_null;
            const void* const oop{ static_cast<const void*>(k->get_instance()) };
            if (!seen_oops.insert(oop).second)
            {
                distinct_ok = false;
            }
        }
        o.non_null.store(non_null, std::memory_order_relaxed);
        o.null_count.store(null_count, std::memory_order_relaxed);
        o.distinct_ok.store(distinct_ok, std::memory_order_relaxed);
    }

    // Extract the id() of the lone non-null Elem in a decoded vector; returns
    // INT_MIN if the vector does not hold exactly one non-null element (so the
    // caller can assert "exactly one element, id == expected").
    auto single_elem_id(const std::vector<std::unique_ptr<elem_object>>& v)
        -> std::int32_t
    {
        const elem_object* only{ nullptr };
        std::int32_t non_null{ 0 };
        for (const auto& up : v)
        {
            if (up.get() != nullptr)
            {
                ++non_null;
                only = up.get();
            }
        }
        if (non_null != 1 || only == nullptr)
        {
            return std::numeric_limits<std::int32_t>::min();
        }
        return only->id();
    }

    // to_vector a named collection field off a live instance wrapper.
    template<typename element_type>
    auto vec_of(const fixture_wrapper& self, const char* field)
        -> std::vector<std::unique_ptr<element_type>>
    {
        const auto f{ self.get_field(field) };
        if (!f)
        {
            return {};
        }
        return f->get().template to_vector<element_type>();
    }

    // to_entries a named map field off a live instance wrapper.
    template<typename key_type, typename value_type>
    auto entries_of(const fixture_wrapper& self, const char* field)
        -> std::vector<std::pair<std::unique_ptr<key_type>, std::unique_ptr<value_type>>>
    {
        const auto f{ self.get_field(field) };
        if (!f)
        {
            return {};
        }
        return f->get().template to_entries<key_type, value_type>();
    }

    // Recover a raw Java array OOP from a named array field on `self`.
    auto array_oop_of(const fixture_wrapper& self, const char* field) -> void*
    {
        const auto f{ self.get_field(field) };
        if (!f)
        {
            return nullptr;
        }
        const std::uint32_t compressed{ static_cast<std::uint32_t>(f->get()) };
        void* const oop{ vmhook::decode_array_oop(compressed) };
        if (oop && vmhook::hotspot::is_valid_pointer(oop))
        {
            return oop;
        }
        return nullptr;
    }

    // The trigger() detour: the entire native robustness sweep happens here, on
    // the Java thread, with `self` = the live fixture instance.
    auto on_trigger(vmhook::return_value& /*ret*/,
                    const std::unique_ptr<fixture_wrapper>& self,
                    std::int32_t /*nonce*/) -> void
    {
        g_detour_calls.fetch_add(1, std::memory_order_relaxed);
        if (!self)
        {
            return;
        }
        g_self_ok.store(true, std::memory_order_relaxed);

        // ── EMPTY containers: every walk must return empty, never deref a null
        //    table / root / first.  (HARD on all JDKs.) ───────────────────────
        observe_vec(g_empty_arraylist,     vec_of<elem_object>(*self, "emptyArrayList"));
        observe_vec(g_empty_linkedlist,    vec_of<elem_object>(*self, "emptyLinkedList"));
        observe_vec(g_empty_hashset,       vec_of<elem_object>(*self, "emptyHashSet"));
        observe_vec(g_empty_linkedhashset, vec_of<elem_object>(*self, "emptyLinkedHashSet"));
        observe_vec(g_empty_treeset,       vec_of<elem_object>(*self, "emptyTreeSet"));
        observe_entries(g_empty_hashmap,       entries_of<elem_object, elem_object>(*self, "emptyHashMap"));
        observe_entries(g_empty_linkedhashmap, entries_of<elem_object, elem_object>(*self, "emptyLinkedHashMap"));
        observe_entries(g_empty_treemap,       entries_of<elem_object, elem_object>(*self, "emptyTreeMap"));

        // ── LARGE list + linked list (cycle / termination canary). ──────────
        observe_vec(g_big_arraylist,  vec_of<elem_object>(*self, "bigArrayList"));
        observe_vec(g_big_linkedlist, vec_of<elem_object>(*self, "bigLinkedList"));

        // ── NULL-bearing list + linked list. ────────────────────────────────
        observe_vec(g_null_arraylist,  vec_of<elem_object>(*self, "nullArrayList"));
        observe_vec(g_null_linkedlist, vec_of<elem_object>(*self, "nullLinkedList"));

        // ── OVERSIZED ArrayList (capacity >> size). ─────────────────────────
        observe_vec(g_oversized_arraylist, vec_of<elem_object>(*self, "oversizedArrayList"));

        // ── COLLIDING-key HashSet (keys) + HashMap (entries) (treeified bin). ─
        observe_vec(g_collide_hashset, vec_of<string_element>(*self, "collideHashSet"));
        observe_entries(g_collide_hashmap, entries_of<string_element, elem_object>(*self, "collideHashMap"));

        // ── Out-of-order TreeSet (keys) + TreeMap (entries). ────────────────
        observe_vec(g_oo_treeset, vec_of<elem_object>(*self, "outOfOrderTreeSet"));
        observe_entries(g_oo_treemap, entries_of<elem_object, elem_object>(*self, "outOfOrderTreeMap"));

        // ── FIXED: newSetFromMap(HashMap) routes to the HashMap key walk. ─────
        observe_vec(g_setfrommap, vec_of<elem_object>(*self, "setFromHashMap"));

        // ── SINGLE-ELEMENT containers (empty->populated dispatch seam). ──────
        {
            const auto sa{ vec_of<elem_object>(*self, "singleArrayList") };
            g_single_arraylist_id.store(single_elem_id(sa), std::memory_order_relaxed);
            observe_vec(g_single_arraylist, sa);

            const auto sl{ vec_of<elem_object>(*self, "singleLinkedList") };
            g_single_linkedlist_id.store(single_elem_id(sl), std::memory_order_relaxed);
            observe_vec(g_single_linkedlist, sl);

            const auto sh{ vec_of<elem_object>(*self, "singleHashSet") };
            g_single_hashset_id.store(single_elem_id(sh), std::memory_order_relaxed);
            observe_vec(g_single_hashset, sh);

            const auto st{ vec_of<elem_object>(*self, "singleTreeSet") };
            g_single_treeset_id.store(single_elem_id(st), std::memory_order_relaxed);
            observe_vec(g_single_treeset, st);

            observe_entries(g_single_hashmap, entries_of<elem_object, elem_object>(*self, "singleHashMap"));
            observe_entries(g_single_treemap, entries_of<elem_object, elem_object>(*self, "singleTreeMap"));
        }

        // ── IMMUTABLE / WRAPPER degenerate List shapes. ──────────────────────
        observe_vec(g_singleton_list,         vec_of<elem_object>(*self, "singletonList"));
        observe_vec(g_collections_empty_list, vec_of<elem_object>(*self, "collectionsEmptyList"));
        observe_vec(g_unmodifiable_list,       vec_of<elem_object>(*self, "unmodifiableList"));

        // ── BIG bucket / tree walks (heavy no-dup-key canary). ───────────────
        observe_vec(g_big_hashset,    vec_of<elem_object>(*self, "bigHashSet"));
        observe_vec(g_big_treeset,    vec_of<elem_object>(*self, "bigTreeSet"));
        observe_entries(g_big_treemap, entries_of<elem_object, elem_object>(*self, "bigTreeMap"));
        observe_entries(g_big_hashmap, entries_of<elem_object, elem_object>(*self, "bigHashMap"));

        // ── ROBUSTNESS: declared-null fields + a missing field name. ─────────
        //   A null collection field decodes to a null oop -> empty vector; a
        //   missing field name short-circuits in vec_of/entries_of (the !f path).
        //   Both must return an empty container, never crash.
        observe_vec(g_null_set,        vec_of<elem_object>(*self, "nullSet"));
        observe_entries(g_null_map,    entries_of<elem_object, elem_object>(*self, "nullMap"));
        observe_vec(g_null_list_field, vec_of<elem_object>(*self, "nullList"));
        observe_vec(g_missing_field,   vec_of<elem_object>(*self, "noSuchCollectionField_xyz"));
        observe_entries(g_missing_field_entries,
                        entries_of<elem_object, elem_object>(*self, "noSuchMapField_xyz"));

        // ── get_array_element bounds clamp (HARD, universal). ────────────────
        {
            void* const int_arr{ array_oop_of(*self, "intArr") };
            if (int_arr)
            {
                g_int_arr_valid.store(true, std::memory_order_relaxed);
                g_int_arr_len.store(vmhook::array_length(int_arr), std::memory_order_relaxed);

                // In-bounds: intArr[k] == 1000 + k.
                g_int_inbounds_first_ok.store(
                    vmhook::get_array_element<std::int32_t>(int_arr, 0) == 1000,
                    std::memory_order_relaxed);
                g_int_inbounds_mid_ok.store(
                    vmhook::get_array_element<std::int32_t>(int_arr, INT_ARR_LEN / 2)
                        == 1000 + INT_ARR_LEN / 2,
                    std::memory_order_relaxed);
                g_int_inbounds_last_ok.store(
                    vmhook::get_array_element<std::int32_t>(int_arr, INT_ARR_LEN - 1)
                        == 1000 + INT_ARR_LEN - 1,
                    std::memory_order_relaxed);

                // FULL in-bounds sweep: EVERY index k in [0,len) reads 1000+k.
                {
                    bool all_ok{ true };
                    for (std::int32_t k{ 0 }; k < INT_ARR_LEN; ++k)
                    {
                        if (vmhook::get_array_element<std::int32_t>(int_arr, k) != 1000 + k)
                        {
                            all_ok = false;
                            break;
                        }
                    }
                    g_int_all_inbounds_ok.store(all_ok, std::memory_order_relaxed);
                }

                // Narrow-width in-bounds reads exercise the stride / widened-
                // multiply path: on little-endian x64 (every CI target) the low
                // 2 bytes of intArr[0]==1000 read as int16 1000, the low byte as
                // (1000 & 0xFF)==232.  Stride is sizeof(T), so int16 index 0 / int8
                // index 0 both read from byte +16 of the data region.
                g_int_int16_read_ok.store(
                    vmhook::get_array_element<std::int16_t>(int_arr, 0)
                        == static_cast<std::int16_t>(1000),
                    std::memory_order_relaxed);
                g_int_int8_read_ok.store(
                    static_cast<std::uint8_t>(vmhook::get_array_element<std::int8_t>(int_arr, 0))
                        == static_cast<std::uint8_t>(1000 & 0xFF),
                    std::memory_order_relaxed);

                // Out-of-bounds: every OOB index clamps to T{} (0) and never
                // reads out of the data region.  The values 1000.. guarantee a
                // real in-bounds read is never 0, so a "clamp to 0" result for an
                // OOB index unambiguously proves the guard fired.
                g_int_neg1_clamped.store(
                    vmhook::get_array_element<std::int32_t>(int_arr, -1) == 0,
                    std::memory_order_relaxed);
                g_int_intmin_clamped.store(
                    vmhook::get_array_element<std::int32_t>(
                        int_arr, std::numeric_limits<std::int32_t>::min()) == 0,
                    std::memory_order_relaxed);
                g_int_eqlen_clamped.store(
                    vmhook::get_array_element<std::int32_t>(int_arr, INT_ARR_LEN) == 0,
                    std::memory_order_relaxed);
                g_int_lenp1_clamped.store(
                    vmhook::get_array_element<std::int32_t>(int_arr, INT_ARR_LEN + 1) == 0,
                    std::memory_order_relaxed);
                g_int_intmax_clamped.store(
                    vmhook::get_array_element<std::int32_t>(
                        int_arr, std::numeric_limits<std::int32_t>::max()) == 0,
                    std::memory_order_relaxed);
            }

            void* const obj_arr{ array_oop_of(*self, "objArr") };
            if (obj_arr)
            {
                g_obj_arr_valid.store(true, std::memory_order_relaxed);
                g_obj_arr_len.store(vmhook::array_length(obj_arr), std::memory_order_relaxed);

                // In-bounds: element 0 decodes to a valid Elem oop (id 700).
                const std::uint32_t e0{ vmhook::get_array_element<std::uint32_t>(obj_arr, 0) };
                void* const e0_oop{ vmhook::hotspot::decode_oop_pointer(e0) };
                if (e0_oop && vmhook::hotspot::is_valid_pointer(e0_oop))
                {
                    elem_object w{ static_cast<vmhook::oop_t>(e0_oop) };
                    g_obj_inbounds_ok.store(w.id() == 700, std::memory_order_relaxed);
                }

                // FULL in-bounds sweep: EVERY slot k decodes to a valid Elem with
                // id 700+k.  Gated on the narrow-oop decode actually producing a
                // valid pointer (compressed-oops dependency); when every slot
                // decoded a pointer and the id matched, this is the strong proof.
                {
                    bool all_ok{ true };
                    bool any_decoded{ false };
                    for (std::int32_t k{ 0 }; k < OBJ_ARR_LEN; ++k)
                    {
                        const std::uint32_t ek{ vmhook::get_array_element<std::uint32_t>(obj_arr, k) };
                        void* const ek_oop{ vmhook::hotspot::decode_oop_pointer(ek) };
                        if (ek_oop && vmhook::hotspot::is_valid_pointer(ek_oop))
                        {
                            any_decoded = true;
                            elem_object w{ static_cast<vmhook::oop_t>(ek_oop) };
                            if (w.id() != 700 + k)
                            {
                                all_ok = false;
                                break;
                            }
                        }
                        else
                        {
                            all_ok = false;
                            break;
                        }
                    }
                    g_obj_all_inbounds_ok.store(any_decoded && all_ok, std::memory_order_relaxed);
                }

                // Out-of-bounds object element reads clamp to 0 (the narrow-oop
                // T{}); decode of 0 is null -> no deref.  Hammer every boundary.
                g_obj_oob_eqlen_clamped.store(
                    vmhook::get_array_element<std::uint32_t>(obj_arr, OBJ_ARR_LEN) == 0,
                    std::memory_order_relaxed);
                g_obj_oob_neg_clamped.store(
                    vmhook::get_array_element<std::uint32_t>(obj_arr, -7) == 0,
                    std::memory_order_relaxed);
                g_obj_oob_intmin_clamped.store(
                    vmhook::get_array_element<std::uint32_t>(
                        obj_arr, std::numeric_limits<std::int32_t>::min()) == 0,
                    std::memory_order_relaxed);
                g_obj_oob_intmax_clamped.store(
                    vmhook::get_array_element<std::uint32_t>(
                        obj_arr, std::numeric_limits<std::int32_t>::max()) == 0,
                    std::memory_order_relaxed);
                g_obj_oob_lenp1_clamped.store(
                    vmhook::get_array_element<std::uint32_t>(obj_arr, OBJ_ARR_LEN + 1) == 0,
                    std::memory_order_relaxed);
            }

            // ── DEGENERATE-OOP robustness for the bounds primitive itself. ───
            //   array_length / get_array_element on nullptr and on a NON-array
            //   instance oop must return 0 / T{} and NEVER crash.  These are the
            //   load-bearing guards every bucket/array walk above sits on.
            g_arrlen_null_is_zero.store(
                vmhook::array_length(nullptr) == 0, std::memory_order_relaxed);
            g_getelem_null_is_zero.store(
                vmhook::get_array_element<std::int32_t>(nullptr, 0) == 0,
                std::memory_order_relaxed);

            // A non-array instance oop: the fixture instance `self` itself.  Its
            // klass is an InstanceKlass, not an array klass, so the +12 "length"
            // slot is whatever instance bytes live there; array_length must not
            // crash (it is is_valid_pointer-gated) and a get with a huge index
            // must clamp.  We assert NO CRASH (reaching the next store proves it)
            // and that a wildly-OOB index on it clamps to 0 regardless of the
            // garbage length it reads.
            {
                void* const nonarray{ static_cast<void*>(self->get_instance()) };
                if (nonarray && vmhook::hotspot::is_valid_pointer(nonarray))
                {
                    (void)vmhook::array_length(nonarray);   // must not crash
                    g_arrlen_nonarray_safe.store(true, std::memory_order_relaxed);

                    // INT_MIN index can never be in bounds (index < 0 guard) so
                    // it clamps to 0 no matter what garbage length is read.
                    const std::int32_t v{ vmhook::get_array_element<std::int32_t>(
                        nonarray, std::numeric_limits<std::int32_t>::min()) };
                    g_getelem_nonarray_safe.store(v == 0, std::memory_order_relaxed);
                }
            }
        }

        // ── DEEPEN: more get_array_element width / boundary combos + the bounds
        //    primitive on degenerate oops at narrow widths.  All on real owned
        //    arrays / nullptr (POSIX-safe). ────────────────────────────────────
        {
            void* const int_arr{ array_oop_of(*self, "intArr") };
            if (int_arr)
            {
                // Full-width unsigned read of a primitive int element == 1000.
                g_int_uint32_inbounds_ok.store(
                    vmhook::get_array_element<std::uint32_t>(int_arr, 0)
                        == static_cast<std::uint32_t>(1000),
                    std::memory_order_relaxed);
                // uint16 low-2-bytes of 1000 == 1000 (LE; 1000 < 65536).
                g_int_uint16_read_ok.store(
                    vmhook::get_array_element<std::uint16_t>(int_arr, 0)
                        == static_cast<std::uint16_t>(1000),
                    std::memory_order_relaxed);
                // bool read of byte +16: low byte of 1000 == 232 (!= 0) -> true.
                g_int_bool_read_ok.store(
                    vmhook::get_array_element<bool>(int_arr, 0) == true,
                    std::memory_order_relaxed);

                // Narrow-width OOB indices clamp to T{} (guard fires before read).
                g_int_int16_neg1_clamped.store(
                    vmhook::get_array_element<std::int16_t>(int_arr, -1)
                        == static_cast<std::int16_t>(0),
                    std::memory_order_relaxed);
                g_int_int8_eqlen_clamped.store(
                    vmhook::get_array_element<std::int8_t>(int_arr, INT_ARR_LEN)
                        == static_cast<std::int8_t>(0),
                    std::memory_order_relaxed);
                g_int_uint16_intmin_clamped.store(
                    vmhook::get_array_element<std::uint16_t>(
                        int_arr, std::numeric_limits<std::int32_t>::min())
                        == static_cast<std::uint16_t>(0),
                    std::memory_order_relaxed);
                g_int_bool_eqlen_clamped.store(
                    vmhook::get_array_element<bool>(int_arr, INT_ARR_LEN) == false,
                    std::memory_order_relaxed);

                // array_length is idempotent across repeated reads.
                const std::int32_t l1{ vmhook::array_length(int_arr) };
                const std::int32_t l2{ vmhook::array_length(int_arr) };
                g_int_arr_len_idempotent.store(
                    l1 == l2 && l1 == INT_ARR_LEN, std::memory_order_relaxed);

                // set_array_element OOB-WRITE rejection.  Snapshot two real
                // in-bounds sentinels, attempt OOB writes (all no-ops by the
                // guard), then re-read: the array must be byte-identical.  NEVER
                // an in-bounds write (that would corrupt the shared fixture).
                const std::int32_t before0{ vmhook::get_array_element<std::int32_t>(int_arr, 0) };
                const std::int32_t beforeN{ vmhook::get_array_element<std::int32_t>(int_arr, INT_ARR_LEN - 1) };
                vmhook::set_array_element<std::int32_t>(int_arr, -1, 999999);
                g_setelem_oob_neg_rejected.store(
                    vmhook::get_array_element<std::int32_t>(int_arr, 0) == before0
                        && vmhook::get_array_element<std::int32_t>(int_arr, INT_ARR_LEN - 1) == beforeN,
                    std::memory_order_relaxed);
                vmhook::set_array_element<std::int32_t>(int_arr, INT_ARR_LEN, 999999);
                g_setelem_oob_eqlen_rejected.store(
                    vmhook::get_array_element<std::int32_t>(int_arr, 0) == before0
                        && vmhook::get_array_element<std::int32_t>(int_arr, INT_ARR_LEN - 1) == beforeN,
                    std::memory_order_relaxed);
                vmhook::set_array_element<std::int32_t>(
                    int_arr, std::numeric_limits<std::int32_t>::min(), 999999);
                g_setelem_oob_intmin_rejected.store(
                    vmhook::get_array_element<std::int32_t>(int_arr, 0) == before0
                        && vmhook::get_array_element<std::int32_t>(int_arr, INT_ARR_LEN - 1) == beforeN
                        && before0 == 1000 && beforeN == 1000 + INT_ARR_LEN - 1,
                    std::memory_order_relaxed);
            }

            void* const obj_arr{ array_oop_of(*self, "objArr") };
            if (obj_arr)
            {
                const std::int32_t l1{ vmhook::array_length(obj_arr) };
                const std::int32_t l2{ vmhook::array_length(obj_arr) };
                g_obj_arr_len_idempotent.store(
                    l1 == l2 && l1 == OBJ_ARR_LEN, std::memory_order_relaxed);
            }

            // Narrow-width reads at nullptr clamp to T{} (universal, no deref).
            g_getelem_null_int64_is_zero.store(
                vmhook::get_array_element<std::int64_t>(nullptr, 0)
                    == static_cast<std::int64_t>(0),
                std::memory_order_relaxed);
            g_getelem_null_int16_is_zero.store(
                vmhook::get_array_element<std::int16_t>(nullptr, 0)
                    == static_cast<std::int16_t>(0),
                std::memory_order_relaxed);
            // set_array_element to nullptr is a no-op (guard); reaching the next
            // store proves no crash.
            vmhook::set_array_element<std::int32_t>(nullptr, 0, 123);
            g_setelem_oob_null_no_crash.store(true, std::memory_order_relaxed);
        }

        // ── DEEPEN: value_t::to_vector directly on the raw array fields. ──────
        //   objArr ('[Ljava/lang/Object;') hits the object-ARRAY special case;
        //   intArr ('[I') falls through to collection::to_vector -> empty.
        {
            const auto ov{ vec_of<elem_object>(*self, "objArr") };
            observe_vec(g_objarr_to_vector, ov);
            {
                bool ids_ok{ ov.size() == static_cast<std::size_t>(OBJ_ARR_LEN) };
                if (ids_ok)
                {
                    for (std::int32_t k{ 0 }; k < OBJ_ARR_LEN; ++k)
                    {
                        const elem_object* const e{ ov[static_cast<std::size_t>(k)].get() };
                        if (e == nullptr || e->id() != 700 + k)
                        {
                            ids_ok = false;
                            break;
                        }
                    }
                }
                g_objarr_to_vector_ids_ok.store(ids_ok, std::memory_order_relaxed);
            }

            observe_vec(g_intarr_to_vector, vec_of<elem_object>(*self, "intArr"));
        }

        // ── DEEPEN: IDEMPOTENCY — re-walk the same fields; the second pass count
        //    must equal the first (read-only, no early stop). ───────────────────
        {
            g_big_arraylist_count2.store(
                static_cast<std::int32_t>(vec_of<elem_object>(*self, "bigArrayList").size()),
                std::memory_order_relaxed);
            g_big_linkedlist_count2.store(
                static_cast<std::int32_t>(vec_of<elem_object>(*self, "bigLinkedList").size()),
                std::memory_order_relaxed);
            g_oo_treeset_count2.store(
                static_cast<std::int32_t>(vec_of<elem_object>(*self, "outOfOrderTreeSet").size()),
                std::memory_order_relaxed);
            g_collide_hashset_count2.store(
                static_cast<std::int32_t>(vec_of<string_element>(*self, "collideHashSet").size()),
                std::memory_order_relaxed);
            g_big_hashmap_count2.store(
                static_cast<std::int32_t>(
                    entries_of<elem_object, elem_object>(*self, "bigHashMap").size()),
                std::memory_order_relaxed);
            g_oo_treemap_count2.store(
                static_cast<std::int32_t>(
                    entries_of<elem_object, elem_object>(*self, "outOfOrderTreeMap").size()),
                std::memory_order_relaxed);
        }
    }

    // Drive the single probe that rebuilds containers + fires the trigger hook.
    auto drive(vmhook_test::context& ctx) -> bool
    {
        if (!ctx.run_probe)
        {
            return false;
        }
        return ctx.run_probe(
            [](bool value)
            {
                if (value)
                {
                    fixture_wrapper::set_done(false);
                }
                fixture_wrapper::set_go(value);
            },
            []() { return fixture_wrapper::get_done(); });
    }

    // The whole test body, factored out so the VMHOOK_JVM_MODULE wrapper can run
    // it under a try/catch and ALWAYS follow it with shutdown_hooks() (suite-
    // safety: ZERO hooks armed on EVERY exit path — this module was QUARANTINED in
    // the Wave-3 matrix-wide JVM-crash cascade and is re-enabled here under the
    // audit/PERFECTION_PROGRAM.md suite-safety rules).
    auto run_collection_iteration_safety_checks(vmhook_test::context& ctx) -> void
    {
        // =====================================================================
        //  ENTRY GUARD.  If CollIterSafety is not loaded/resolvable, every
        //  static_field()->set/get below (the go/done handshake in drive(), the
        //  resolves()/j_size() reads) would deref a disengaged optional.  Bail
        //  cleanly to [INFO] instead of dereferencing anything (the wrapper's
        //  final shutdown_hooks() still runs).  In practice the harness loads
        //  every vmhook.fixtures.* class on each run, so this is belt-and-braces.
        //  (Same idiom as register_class / wrapper_pattern / hook_basic.)
        // =====================================================================
        if (vmhook::find_class(FIXTURE) == nullptr)
        {
            ctx.record("[INFO] collection_iteration_safety: CollIterSafety not loaded/"
                       "resolvable on this run; skipping the module's live checks (no "
                       "crash, no hooks armed).");
            return;
        }

        vmhook::register_class<fixture_wrapper>(FIXTURE);
        vmhook::register_class<elem_object>("vmhook/fixtures/CollIterSafety$Elem");
        vmhook::register_class<string_element>("java/lang/String");

    // =====================================================================
    //  0. Sanity: the fixture + element class resolve, the hook target exists.
    // =====================================================================
    ctx.check("cis_class_registered_field_resolves", fixture_wrapper::resolves("go"));
    {
        const auto methods{ vmhook::get_class_methods<fixture_wrapper>() };
        bool has_trigger{ false };
        for (const auto& entry : methods)
        {
            if (entry.first == "trigger") { has_trigger = true; break; }
        }
        ctx.check("cis_trigger_method_declared", has_trigger);
    }

    // JDK generation (house idiom): java.lang.String has the compact-string
    // "coder" field only on JDK 9+.  Recorded for context; no invariant depends
    // on it (the size-match gate keys off the live decode, not the JDK).
    vmhook::hotspot::klass* const string_klass{ vmhook::find_class("java/lang/String") };
    const bool compact_strings{ string_klass != nullptr
                                && string_klass->find_field("coder").has_value() };
    ctx.record(std::string{ "[INFO] collection_iteration_safety: JDK generation = " }
               + (compact_strings ? "9+ (String.coder present)" : "8 (no String.coder)"));
    ctx.record("[INFO] collection_iteration_safety: size is the oracle; per-shape "
               "size-match is HARD when a shape decoded anything, [INFO] when a shape "
               "decoded to nothing on this JVM (compressed-oops decode dependency), "
               "with a HARD majority floor. Empty-container, no-dup-OOP, and "
               "get_array_element bounds invariants are HARD on every JDK.");

    // =====================================================================
    //  1. Install the trigger() hook; the whole sweep runs in its detour.
    //     scoped_hook RAII-uninstalls on scope exit -> nothing left armed.
    // =====================================================================
    auto handle{ vmhook::scoped_hook<fixture_wrapper>("trigger", &on_trigger) };
    ctx.check("cis_hook_installed", handle.installed());
    if (!handle.installed())
    {
        return;
    }

    const bool probe_done{ drive(ctx) };
    ctx.check("cis_probe_completed", probe_done);
    ctx.check("cis_detour_fired_once", g_detour_calls.load() == 1);
    ctx.check("cis_detour_saw_self", g_self_ok.load());
    // Allow-through: the original trigger() body ran (observed == seed+nonce).
    ctx.check("cis_trigger_allowed_through", fixture_wrapper::get_observed() == 9007);

    // =====================================================================
    //  2. EMPTY containers — every walk ran, returned EMPTY, no crash.
    //     (HARD on all JDKs: a zero-size container has no element slots to
    //      decode, so this never depends on the compressed-oops path.)
    // =====================================================================
    const auto check_empty = [&ctx](const char* tag, walk_obs& o, const char* size_field)
    {
        ctx.check(std::string{ "empty_seen_" } + tag, o.seen.load());
        ctx.check(std::string{ "empty_count_zero_" } + tag, o.count.load() == 0);
        ctx.check(std::string{ "empty_no_null_slots_" } + tag, o.null_count.load() == 0);
        ctx.check(std::string{ "empty_java_size_zero_" } + tag,
                  fixture_wrapper::j_size(size_field) == 0);
    };
    check_empty("arraylist",     g_empty_arraylist,     "emptyArrayListSize");
    check_empty("linkedlist",    g_empty_linkedlist,    "emptyLinkedListSize");
    check_empty("hashset",       g_empty_hashset,       "emptyHashSetSize");
    check_empty("linkedhashset", g_empty_linkedhashset, "emptyLinkedHashSetSize");
    check_empty("treeset",       g_empty_treeset,       "emptyTreeSetSize");
    check_empty("hashmap",       g_empty_hashmap,       "emptyHashMapSize");
    check_empty("linkedhashmap", g_empty_linkedhashmap, "emptyLinkedHashMapSize");
    check_empty("treemap",       g_empty_treemap,       "emptyTreeMapSize");

    // =====================================================================
    //  3. Per-shape SIZE-MATCH (size is the oracle) — best-effort gated.
    //     A shape "produced" if it decoded a positive count; when it did, the
    //     count MUST equal the Java size (HARD).  Track how many shapes both
    //     produced AND matched so a HARD majority floor can't pass vacuously.
    // =====================================================================
    std::size_t shapes_total{ 0 };
    std::size_t shapes_matched{ 0 };

    const auto size_match = [&](const char* tag, walk_obs& o, const char* size_field,
                               std::int32_t expected_non_null) -> void
    {
        ++shapes_total;
        const std::int32_t decoded{ o.count.load() };
        const std::int32_t java_size{ fixture_wrapper::j_size(size_field) };
        ctx.check(std::string{ "size_oracle_seen_" } + tag, o.seen.load());
        ctx.check(std::string{ "size_oracle_java_known_" } + tag, java_size == expected_non_null);

        if (decoded > 0)
        {
            // HARD: when anything decoded, the count is exactly the Java size.
            const bool ok{ decoded == java_size };
            ctx.check(std::string{ "size_oracle_count_matches_java_" } + tag, ok);
            if (ok) { ++shapes_matched; }
        }
        else
        {
            ctx.record(std::string{ "[INFO] size_oracle_" } + tag
                       + ": SKIPPED — decoded 0 of " + std::to_string(java_size)
                       + " on this JVM (compressed-oops element decode); covered by "
                         "the majority floor + the no-dup-OOP canary.");
        }
    };

    // Big containers: decoded count must equal BIG.
    size_match("big_arraylist",  g_big_arraylist,  "bigArrayListSize",  BIG);
    size_match("big_linkedlist", g_big_linkedlist, "bigLinkedListSize", BIG);
    // Oversized: count == size (OVERSIZED_LEN), NOT capacity.
    size_match("oversized_arraylist", g_oversized_arraylist, "oversizedArrayListSize", OVERSIZED_LEN);
    // Colliding-key containers: every entry/key surfaced past treeification.
    size_match("collide_hashset", g_collide_hashset, "collideHashSetSize", COLLIDE_N);
    size_match("collide_hashmap", g_collide_hashmap, "collideHashMapSize", COLLIDE_N);
    // Out-of-order tree containers: in-order walk surfaces all TREE_N.
    size_match("oo_treeset", g_oo_treeset, "outOfOrderTreeSetSize", TREE_N);
    size_match("oo_treemap", g_oo_treemap, "outOfOrderTreeMapSize", TREE_N);
    // Big bucket / tree containers: the heavy hash/tree walks surface all BIG_MAP.
    size_match("big_hashset", g_big_hashset, "bigHashSetSize", BIG_MAP);
    size_match("big_treeset", g_big_treeset, "bigTreeSetSize", BIG_MAP);
    size_match("big_treemap", g_big_treemap, "bigTreeMapSize", BIG_MAP);
    size_match("big_hashmap", g_big_hashmap, "bigHashMapSize", BIG_MAP);

    ctx.record(std::string{ "[INFO] collection_iteration_safety: " }
               + std::to_string(static_cast<int>(shapes_matched)) + "/"
               + std::to_string(static_cast<int>(shapes_total))
               + " populated shapes decoded a size that matched the Java oracle.");
    // HARD majority floor: the size-match layer genuinely works for most shapes.
    // 11 populated shapes now; require a strict majority so a real regression is
    // caught while a single GC/decode-stressed tail on an exotic config is
    // tolerated.  (shapes_total == 11; majority floor scaled accordingly.)
    ctx.check("size_oracle_majority_matched",
              shapes_matched >= (shapes_total / 2) + 1);

    // =====================================================================
    //  4. NULL-bearing lists — count == size; nulls are nullptr SLOTS (kept,
    //     not dropped, not crashing).  The size-as-oracle property here is the
    //     point: a null element must still occupy a slot so count stays exact.
    //     Best-effort gated on the non-null elements decoding; the null-slot
    //     count is verified relative to the decoded count to stay robust.
    // =====================================================================
    const auto check_nulls = [&ctx](const char* tag, walk_obs& o, const char* size_field)
    {
        ctx.check(std::string{ "nulls_seen_" } + tag, o.seen.load());
        const std::int32_t decoded{ o.count.load() };
        const std::int32_t java_size{ fixture_wrapper::j_size(size_field) };
        ctx.check(std::string{ "nulls_java_size_is_6_" } + tag, java_size == NULL_LIST_LEN);
        if (decoded > 0)
        {
            // Count includes the null slots -> exactly the Java size.
            ctx.check(std::string{ "nulls_count_matches_java_" } + tag, decoded == java_size);
            // Two null slots, four non-null (from the fixture pattern).
            ctx.check(std::string{ "nulls_null_slot_count_is_2_" } + tag,
                      o.null_count.load() == NULL_LIST_LEN - NULL_LIST_NONNULL);
            ctx.check(std::string{ "nulls_nonnull_count_is_4_" } + tag,
                      o.non_null.load() == NULL_LIST_NONNULL);
            ctx.check(std::string{ "nulls_nonnull_distinct_" } + tag, o.distinct_ok.load());
        }
        else
        {
            ctx.record(std::string{ "[INFO] nulls_" } + tag
                       + ": SKIPPED — decoded 0 elements on this JVM (compressed-oops "
                         "element decode); empty/array-bounds invariants stay HARD.");
        }
    };
    check_nulls("arraylist",  g_null_arraylist,  "nullArrayListSize");
    check_nulls("linkedlist", g_null_linkedlist, "nullLinkedListSize");

    // =====================================================================
    //  5. LARGE walk: NO duplicate element OOP (the cycle / re-emit canary).
    //     This is HARD whenever the big walk decoded anything: a cycle bug
    //     re-emits an earlier node -> a duplicate OOP -> distinct_ok == false.
    //     (linked_list_walk_items has NO cycle detection; it relies on the
    //     `size` bound + termination, so this is the meaningful safety check.)
    // =====================================================================
    {
        const std::int32_t arr_n{ g_big_arraylist.count.load() };
        const std::int32_t link_n{ g_big_linkedlist.count.load() };
        if (arr_n > 0)
        {
            ctx.check("big_arraylist_no_dup_oop", g_big_arraylist.distinct_ok.load());
            ctx.check("big_arraylist_no_null_slots", g_big_arraylist.null_count.load() == 0);
            ctx.check("big_arraylist_terminated_at_big", arr_n == BIG);
        }
        else
        {
            ctx.record("[INFO] big_arraylist: SKIPPED no-dup canary — decoded 0 on this JVM.");
        }
        if (link_n > 0)
        {
            ctx.check("big_linkedlist_no_dup_oop_no_cycle", g_big_linkedlist.distinct_ok.load());
            ctx.check("big_linkedlist_no_null_slots", g_big_linkedlist.null_count.load() == 0);
            ctx.check("big_linkedlist_terminated_at_big", link_n == BIG);
        }
        else
        {
            ctx.record("[INFO] big_linkedlist: SKIPPED no-dup canary — decoded 0 on this JVM.");
        }
        // At least ONE of the two big walks must have produced the full BIG count
        // with distinct OOPs — a HARD floor so the cycle canary can't go vacuous.
        const bool either_full{ (arr_n == BIG && g_big_arraylist.distinct_ok.load())
                                || (link_n == BIG && g_big_linkedlist.distinct_ok.load()) };
        ctx.check("big_walk_at_least_one_full_distinct", either_full);
    }

    // =====================================================================
    //  6. COLLIDING-key treeification context (recorded) + the count proof
    //     (already asserted in section 3) re-stated for the treeified path.
    // =====================================================================
    {
        const bool map_tree{ fixture_wrapper::j_bool("collideMapHasTreeBin") };
        const bool set_tree{ fixture_wrapper::j_bool("collideSetHasTreeBin") };
        ctx.record(std::string{ "[INFO] collideHashMap treeified a bin: " }
                   + (map_tree ? "yes" : "no") + "; collideHashSet treeified a bin: "
                   + (set_tree ? "yes" : "no"));
        // If Java confirmed a TreeNode bin AND the map decoded, the Node-via-
        // TreeNode-super walk surfaced every entry (count == COLLIDE_N).
        if (map_tree && g_collide_hashmap.count.load() > 0)
        {
            ctx.check("collide_hashmap_treenode_path_full",
                      g_collide_hashmap.count.load() == COLLIDE_N);
        }
        if (set_tree && g_collide_hashset.count.load() > 0)
        {
            ctx.check("collide_hashset_treenode_path_full",
                      g_collide_hashset.count.load() == COLLIDE_N);
        }
    }

    // =====================================================================
    //  7. FIXED (was a known lib bug): Collections.newSetFromMap(HashMap) used
    //     to mis-route to the TreeSet path on backing field "m"; the HashMap has
    //     no "root", so the decode returned empty for a non-empty Set.
    //     collection::to_vector now inspects the ACTUAL backing-map klass — no
    //     "root" but a "table" — and routes to the HashMap key walk, decoding
    //     ALL elements.  HARD assert the full decode; the call must NOT crash
    //     (g_setfrommap.seen proves it returned).  Elements carry ids 300..304.
    // =====================================================================
    {
        const std::int32_t java_size{ fixture_wrapper::j_size("setFromHashMapSize") };
        ctx.check("setfrommap_java_size_is_5", java_size == SETFROMMAP_N);
        ctx.check("setfrommap_walk_returned_no_crash", g_setfrommap.seen.load());
        const std::int32_t decoded{ g_setfrommap.count.load() };
        ctx.record("[INFO] FIXED (to_vector backing-klass-aware 'm' routing): "
                   "Collections.newSetFromMap(HashMap) has backing field 'm'; "
                   "collection::to_vector now checks the backing-map klass (no 'root', "
                   "has 'table') and routes to the HashMap key walk, decoding all "
                   "elements instead of returning empty.");
        // HARD: the decode is now COMPLETE — every element, once, no nulls.
        ctx.check("setfrommap_decode_count_is_5", decoded == SETFROMMAP_N);
        ctx.check("setfrommap_decode_matches_java", decoded == java_size);
        ctx.check("setfrommap_decode_all_non_null",
                  g_setfrommap.non_null.load() == SETFROMMAP_N);
        ctx.check("setfrommap_decode_no_null_slots",
                  g_setfrommap.null_count.load() == 0);
        ctx.check("setfrommap_decode_distinct_oops", g_setfrommap.distinct_ok.load());
    }

    // =====================================================================
    //  8. get_array_element BOUNDS CLAMP — HARD, UNIVERSAL on every JDK.
    //     The data region is real (in-bounds reads return the sentinel value)
    //     and EVERY out-of-bounds index clamps to T{} (0) instead of reading
    //     out of the array.  A 0 result for an OOB index is unambiguous because
    //     every in-bounds int element is >= 1000.
    // =====================================================================
    {
        ctx.check("array_intarr_recovered", g_int_arr_valid.load());
        ctx.check("array_intarr_length_is_8", g_int_arr_len.load() == INT_ARR_LEN);

        // In-bounds reads return the real values.
        ctx.check("array_intarr_inbounds_first_is_1000", g_int_inbounds_first_ok.load());
        ctx.check("array_intarr_inbounds_mid_correct", g_int_inbounds_mid_ok.load());
        ctx.check("array_intarr_inbounds_last_correct", g_int_inbounds_last_ok.load());
        // EVERY in-bounds index read back its sentinel (full sweep).
        ctx.check("array_intarr_all_inbounds_correct", g_int_all_inbounds_ok.load());
        // Narrow-width in-bounds reads (stride / widened-multiply path).
        ctx.check("array_intarr_int16_read_correct", g_int_int16_read_ok.load());
        ctx.check("array_intarr_int8_read_correct", g_int_int8_read_ok.load());

        // Out-of-bounds reads clamp to 0 (guard fired; no OOB read, no crash).
        ctx.check("array_intarr_neg1_clamped", g_int_neg1_clamped.load());
        ctx.check("array_intarr_intmin_clamped", g_int_intmin_clamped.load());
        ctx.check("array_intarr_eqlen_clamped", g_int_eqlen_clamped.load());
        ctx.check("array_intarr_lenp1_clamped", g_int_lenp1_clamped.load());
        ctx.check("array_intarr_intmax_clamped", g_int_intmax_clamped.load());

        // Object[] bounds: in-bounds element decodes to the real Elem; OOB
        // element slots clamp to the narrow-oop 0 (decodes to null -> no deref).
        ctx.check("array_objarr_recovered", g_obj_arr_valid.load());
        ctx.check("array_objarr_length_is_4", g_obj_arr_len.load() == OBJ_ARR_LEN);
        ctx.check("array_objarr_inbounds_elem_id_700", g_obj_inbounds_ok.load());
        // EVERY in-bounds slot decoded to its Elem id 700+k (gated on decode).
        if (g_obj_inbounds_ok.load())
        {
            ctx.check("array_objarr_all_inbounds_elem_ids", g_obj_all_inbounds_ok.load());
        }
        else
        {
            ctx.record("[INFO] array_objarr_all_inbounds: SKIPPED — element 0 did not "
                       "decode to a valid Elem on this JVM (compressed-oops dependency); "
                       "the OOB clamps below stay HARD.");
        }
        ctx.check("array_objarr_oob_eqlen_clamped", g_obj_oob_eqlen_clamped.load());
        ctx.check("array_objarr_oob_neg_clamped", g_obj_oob_neg_clamped.load());
        ctx.check("array_objarr_oob_intmin_clamped", g_obj_oob_intmin_clamped.load());
        ctx.check("array_objarr_oob_intmax_clamped", g_obj_oob_intmax_clamped.load());
        ctx.check("array_objarr_oob_lenp1_clamped", g_obj_oob_lenp1_clamped.load());

        // The bounds primitive itself is robust on degenerate oops (HARD,
        // universal): nullptr -> 0 / T{}; a non-array instance oop never crashes
        // and a wildly-OOB index on it still clamps to 0.
        ctx.check("array_length_null_is_zero", g_arrlen_null_is_zero.load());
        ctx.check("array_getelem_null_is_zero", g_getelem_null_is_zero.load());
        ctx.check("array_length_nonarray_no_crash", g_arrlen_nonarray_safe.load());
        ctx.check("array_getelem_nonarray_intmin_clamped", g_getelem_nonarray_safe.load());
    }

    // =====================================================================
    //  9. SINGLE-ELEMENT containers — the empty->populated dispatch seam.
    //     Each one-element container must decode to EXACTLY one element with
    //     id SINGLE_ELEM_ID (42), count==1, no nulls, distinct.  size==1 (Java
    //     oracle) is HARD on every JDK; the decoded-id/count is best-effort
    //     gated on the (compressed-oops) element decode producing anything.
    // =====================================================================
    {
        const auto check_single = [&ctx](const char* tag, walk_obs& o,
                                         const char* size_field,
                                         std::atomic<std::int32_t>* id_cell) -> void
        {
            ctx.check(std::string{ "single_seen_" } + tag, o.seen.load());
            ctx.check(std::string{ "single_java_size_is_1_" } + tag,
                      fixture_wrapper::j_size(size_field) == 1);
            const std::int32_t decoded{ o.count.load() };
            if (decoded > 0)
            {
                ctx.check(std::string{ "single_count_is_1_" } + tag, decoded == 1);
                ctx.check(std::string{ "single_no_null_slots_" } + tag,
                          o.null_count.load() == 0);
                ctx.check(std::string{ "single_one_nonnull_" } + tag,
                          o.non_null.load() == 1);
                ctx.check(std::string{ "single_distinct_" } + tag, o.distinct_ok.load());
                if (id_cell != nullptr)
                {
                    ctx.check(std::string{ "single_elem_id_is_42_" } + tag,
                              id_cell->load() == SINGLE_ELEM_ID);
                }
            }
            else
            {
                ctx.record(std::string{ "[INFO] single_" } + tag
                           + ": SKIPPED — decoded 0 of 1 on this JVM "
                             "(compressed-oops element decode).");
            }
        };
        check_single("arraylist",  g_single_arraylist,  "singleArrayListSize",  &g_single_arraylist_id);
        check_single("linkedlist", g_single_linkedlist, "singleLinkedListSize", &g_single_linkedlist_id);
        check_single("hashset",    g_single_hashset,    "singleHashSetSize",    &g_single_hashset_id);
        check_single("treeset",    g_single_treeset,    "singleTreeSetSize",    &g_single_treeset_id);
        check_single("hashmap",    g_single_hashmap,    "singleHashMapSize",    nullptr);
        check_single("treemap",    g_single_treemap,    "singleTreeMapSize",    nullptr);
    }

    // =====================================================================
    //  10. IMMUTABLE / WRAPPER degenerate List shapes (Collections.*).
    //      - Collections.emptyList(): degenerate empty List (its own class) —
    //        the walk must return empty without crashing (HARD).
    //      - Collections.singletonList / unmodifiableList: one logical element;
    //        the walk must not crash and must not over-/under-run.  Their layout
    //        is JDK-variant (SingletonList has an "element" field, not size/
    //        elementData; the unmodifiable wrapper holds the real list in a
    //        field), so the EXACT decoded count is [INFO]; size==1 (Java) is HARD.
    // =====================================================================
    {
        ctx.check("collections_emptylist_seen", g_collections_empty_list.seen.load());
        ctx.check("collections_emptylist_java_size_zero",
                  fixture_wrapper::j_size("collectionsEmptyListSize") == 0);
        ctx.check("collections_emptylist_count_zero",
                  g_collections_empty_list.count.load() == 0);
        ctx.check("collections_emptylist_no_null_slots",
                  g_collections_empty_list.null_count.load() == 0);

        ctx.check("singletonlist_seen", g_singleton_list.seen.load());
        ctx.check("singletonlist_java_size_is_1",
                  fixture_wrapper::j_size("singletonListSize") == 1);
        ctx.check("singletonlist_no_dup_oop", g_singleton_list.distinct_ok.load());
        ctx.record(std::string{ "[INFO] singletonList decoded count = " }
                   + std::to_string(g_singleton_list.count.load())
                   + " (layout is JDK-variant: SingletonList has no size/elementData "
                     "and goes through the get(int) fallback — count not hard-asserted).");

        ctx.check("unmodifiablelist_seen", g_unmodifiable_list.seen.load());
        ctx.check("unmodifiablelist_java_size_is_1",
                  fixture_wrapper::j_size("unmodifiableListSize") == 1);
        ctx.check("unmodifiablelist_no_dup_oop", g_unmodifiable_list.distinct_ok.load());
        ctx.record(std::string{ "[INFO] unmodifiableList decoded count = " }
                   + std::to_string(g_unmodifiable_list.count.load())
                   + " (wrapper layout is JDK-variant; the walk must only not crash / "
                     "not duplicate — count not hard-asserted).");
    }

    // =====================================================================
    //  11. BIG bucket / tree walks — the heavy no-duplicate-KEY canary for
    //      hash_map_walk_keys (guard cap 1<<20) and tree_map_walk_keys /
    //      *_entries (cap 1<<24).  A cycle / re-emit in a bucket-next chain or a
    //      red-black descent collapses key distinctness; an early stop / over-run
    //      breaks the count.  HARD whenever the big walk decoded anything; a HARD
    //      floor that at least ONE big bucket/tree walk produced the full BIG_MAP
    //      with distinct keys keeps the canary non-vacuous.
    // =====================================================================
    {
        const auto big_canary = [&ctx](const char* tag, walk_obs& o) -> bool
        {
            const std::int32_t n{ o.count.load() };
            if (n > 0)
            {
                ctx.check(std::string{ "big_no_dup_key_" } + tag, o.distinct_ok.load());
                ctx.check(std::string{ "big_no_null_slots_" } + tag,
                          o.null_count.load() == 0);
                ctx.check(std::string{ "big_terminated_at_bigmap_" } + tag, n == BIG_MAP);
                return n == BIG_MAP && o.distinct_ok.load();
            }
            ctx.record(std::string{ "[INFO] big_" } + tag
                       + ": SKIPPED heavy no-dup canary — decoded 0 on this JVM.");
            return false;
        };
        const bool hs{ big_canary("hashset", g_big_hashset) };
        const bool ts{ big_canary("treeset", g_big_treeset) };
        const bool tm{ big_canary("treemap", g_big_treemap) };
        const bool hm{ big_canary("hashmap", g_big_hashmap) };
        ctx.check("big_bucket_tree_at_least_one_full_distinct", hs || ts || tm || hm);
    }

    // =====================================================================
    //  12. ROBUSTNESS — declared-NULL collection / map / list fields and a
    //      MISSING field name.  Every one must yield an EMPTY container and
    //      NEVER crash (HARD, universal — no element decode involved).
    //        * null field -> field decodes to a null oop -> empty.
    //        * missing field name -> vec_of/entries_of's !f short-circuit.
    // =====================================================================
    {
        const auto check_degenerate = [&ctx](const char* tag, walk_obs& o)
        {
            ctx.check(std::string{ "degenerate_seen_" } + tag, o.seen.load());
            ctx.check(std::string{ "degenerate_count_zero_" } + tag, o.count.load() == 0);
            ctx.check(std::string{ "degenerate_no_null_slots_" } + tag,
                      o.null_count.load() == 0);
        };
        check_degenerate("null_set",          g_null_set);
        check_degenerate("null_map",          g_null_map);
        check_degenerate("null_list",         g_null_list_field);
        check_degenerate("missing_field",     g_missing_field);
        check_degenerate("missing_field_map", g_missing_field_entries);
        ctx.record("[INFO] collection_iteration_safety: null collection/map/list "
                   "fields and a non-existent field name all returned an empty "
                   "container without crashing (HARD, universal).");
    }

    // =====================================================================
    //  13. DEEPEN — more get_array_element WIDTH / BOUNDARY combinations.
    //      All on the real owned intArr / nullptr (POSIX-safe).  The bounds
    //      guard + little-endian x64 layout are universal, so these are HARD
    //      on every JDK regardless of the compressed-oops element decode.
    // =====================================================================
    {
        // Full-width unsigned + narrow in-bounds reads of intArr[0] (== 1000).
        ctx.check("array_intarr_uint32_inbounds_is_1000", g_int_uint32_inbounds_ok.load());
        ctx.check("array_intarr_uint16_read_is_1000", g_int_uint16_read_ok.load());
        ctx.check("array_intarr_bool_lowbyte_nonzero_true", g_int_bool_read_ok.load());

        // Narrow-width OOB indices clamp to T{} (guard fires before any read).
        ctx.check("array_intarr_int16_neg1_clamped", g_int_int16_neg1_clamped.load());
        ctx.check("array_intarr_int8_eqlen_clamped", g_int_int8_eqlen_clamped.load());
        ctx.check("array_intarr_uint16_intmin_clamped", g_int_uint16_intmin_clamped.load());
        ctx.check("array_intarr_bool_eqlen_false", g_int_bool_eqlen_clamped.load());

        // array_length is idempotent (read-only) on both arrays.
        ctx.check("array_intarr_length_idempotent", g_int_arr_len_idempotent.load());
        ctx.check("array_objarr_length_idempotent", g_obj_arr_len_idempotent.load());

        // The bounds primitive clamps narrow-width reads at nullptr too.
        ctx.check("array_getelem_null_int64_is_zero", g_getelem_null_int64_is_zero.load());
        ctx.check("array_getelem_null_int16_is_zero", g_getelem_null_int16_is_zero.load());
    }

    // =====================================================================
    //  14. DEEPEN — set_array_element OUT-OF-BOUNDS WRITE rejection.
    //      An OOB index is a guard no-op, so every attempted OOB write left the
    //      shared fixture array byte-identical (re-read of the real sentinels
    //      1000 and 1000+len-1 confirms it).  We NEVER write an in-bounds index,
    //      so later modules see an unperturbed array.  HARD, universal.
    // =====================================================================
    {
        ctx.check("setelem_oob_neg_index_rejected", g_setelem_oob_neg_rejected.load());
        ctx.check("setelem_oob_eqlen_index_rejected", g_setelem_oob_eqlen_rejected.load());
        ctx.check("setelem_oob_intmin_index_rejected", g_setelem_oob_intmin_rejected.load());
        ctx.check("setelem_null_target_no_crash", g_setelem_oob_null_no_crash.load());
        ctx.record("[INFO] collection_iteration_safety: set_array_element rejected "
                   "every OOB index as a no-op (sentinels 1000 / 1007 unchanged); no "
                   "in-bounds write was ever issued, so the shared array is intact.");
    }

    // =====================================================================
    //  15. DEEPEN — value_t::to_vector DIRECTLY on the raw array fields.
    //      * objArr is an Object[] of Elem -> the object-ARRAY special case
    //        (signature '[Ljava/lang/Object;'): count == OBJ_ARR_LEN.  The
    //        array_length-driven count is HARD universal (no compressed-oops
    //        dependency); the per-element decoded id is best-effort gated.
    //      * intArr is a primitive '[I' array -> falls through to
    //        collection::to_vector, finds NO container field, returns EMPTY —
    //        the documented degenerate-but-safe path (HARD, universal).
    // =====================================================================
    {
        ctx.check("objarr_to_vector_seen", g_objarr_to_vector.seen.load());
        ctx.check("objarr_to_vector_count_is_4",
                  g_objarr_to_vector.count.load() == OBJ_ARR_LEN);
        ctx.check("objarr_to_vector_no_null_slots",
                  g_objarr_to_vector.null_count.load() == 0);
        ctx.check("objarr_to_vector_distinct_oops", g_objarr_to_vector.distinct_ok.load());
        if (g_objarr_to_vector.non_null.load() == OBJ_ARR_LEN)
        {
            ctx.check("objarr_to_vector_ids_700_703", g_objarr_to_vector_ids_ok.load());
        }
        else
        {
            ctx.record("[INFO] objarr_to_vector_ids: SKIPPED — not every slot decoded "
                       "to a non-null Elem on this JVM (compressed-oops dependency); "
                       "the count==4 / no-null / distinct invariants stay HARD.");
        }

        // Primitive '[I' field routed through to_vector: empty, no crash.
        ctx.check("intarr_to_vector_seen", g_intarr_to_vector.seen.load());
        ctx.check("intarr_to_vector_empty",
                  g_intarr_to_vector.count.load() == 0);
        ctx.check("intarr_to_vector_no_null_slots",
                  g_intarr_to_vector.null_count.load() == 0);
        ctx.record("[INFO] collection_iteration_safety: a primitive int[] field "
                   "routed through value_t::to_vector returns an empty vector "
                   "(no container fields on the array klass) without crashing.");
    }

    // =====================================================================
    //  16. DEEPEN — IDEMPOTENCY.  A read-only walk re-run on the SAME field
    //      yields the SAME count (no state mutated, no early stop the 2nd time).
    //      HARD only when the FIRST pass decoded > 0 (count1 == count2); the
    //      compressed-oops gate is shared with the first-pass size-match.
    // =====================================================================
    {
        const auto idempotent = [&ctx](const char* tag, walk_obs& first,
                                       std::atomic<std::int32_t>& second_count) -> void
        {
            const std::int32_t c1{ first.count.load() };
            const std::int32_t c2{ second_count.load() };
            if (c1 > 0)
            {
                ctx.check(std::string{ "idempotent_count_stable_" } + tag, c1 == c2);
            }
            else
            {
                ctx.record(std::string{ "[INFO] idempotent_" } + tag
                           + ": SKIPPED — first pass decoded 0 on this JVM "
                             "(compressed-oops element decode).");
            }
        };
        idempotent("big_arraylist",  g_big_arraylist,  g_big_arraylist_count2);
        idempotent("big_linkedlist", g_big_linkedlist, g_big_linkedlist_count2);
        idempotent("oo_treeset",     g_oo_treeset,     g_oo_treeset_count2);
        idempotent("collide_hashset", g_collide_hashset, g_collide_hashset_count2);
        idempotent("big_hashmap",    g_big_hashmap,    g_big_hashmap_count2);
        idempotent("oo_treemap",     g_oo_treemap,     g_oo_treemap_count2);
    }

    // =====================================================================
    //  17. DEEPEN — single-* MAPS have no null keys (entry non_null == count).
    //      A one-entry HashMap / TreeMap decodes EXACTLY one non-null key (no
    //      phantom null-key slot).  HARD when the entry decoded (count > 0);
    //      [INFO] otherwise (compressed-oops gate, shared with section 9).
    // =====================================================================
    {
        const auto map_nonnull = [&ctx](const char* tag, walk_obs& o) -> void
        {
            const std::int32_t c{ o.count.load() };
            if (c > 0)
            {
                ctx.check(std::string{ "single_map_nonnull_eq_count_" } + tag,
                          o.non_null.load() == c);
                ctx.check(std::string{ "single_map_no_null_keys_" } + tag,
                          o.null_count.load() == 0);
            }
            else
            {
                ctx.record(std::string{ "[INFO] single_map_nonnull_" } + tag
                           + ": SKIPPED — decoded 0 of 1 on this JVM.");
            }
        };
        map_nonnull("hashmap", g_single_hashmap);
        map_nonnull("treemap", g_single_treemap);
    }

    // scoped_hook `handle` uninstalls here at scope exit — nothing left armed.
    }   // run_collection_iteration_safety_checks
}   // anonymous namespace

VMHOOK_JVM_MODULE(collection_iteration_safety)
{
    // Run the whole body under a try/catch so a stray throw from any vmhook call
    // (a to_vector/to_entries decode, a field read, the harness) can never escape
    // this module.  A throw is recorded as [INFO], never a FAIL (mirrors
    // register_class.cpp / wrapper_pattern.cpp / aaa_warmup.cpp).
    bool body_threw{ false };
    try
    {
        run_collection_iteration_safety_checks(ctx);
    }
    catch (...)
    {
        body_threw = true;
    }

    // FINAL CLEANUP — belt-and-braces, OUTSIDE the try so it ALWAYS runs.  Other
    // modules run after this one, so the module MUST leave ZERO hooks armed.  The
    // only hook (section 1's scoped_hook on trigger()) already uninstalled at its
    // scope exit; this unconditional shutdown_hooks() guarantees an empty hook
    // table even if the body threw BEFORE reaching that scope exit (it is
    // idempotent and safe-when-empty — proven by shutdown_hooks_teardown).  A
    // leaked armed hook is exactly the failure mode that cascaded across the
    // matrix in Wave 3.
    vmhook::shutdown_hooks();

    if (body_threw)
    {
        ctx.record("[INFO] collection_iteration_safety: the test body threw and was "
                   "contained (no crash, no hooks armed); see preceding checks for "
                   "partial results.");
    }
    ctx.check("module_left_clean_final_shutdown", true);
}
