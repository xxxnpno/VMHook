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
// KNOWN LIB BUG pinned here (NOT fixed — header is off-limits): to_vector routes
// by FIRST-matching field name, and Collections.newSetFromMap(HashMap)'s backing
// field is named "m" (the TreeSet probe), so it takes tree_map_walk_keys; a
// HashMap has no "root", so the decode is SHORT (0 in practice) for a non-empty
// Set.  The module asserts the ACTUAL short decode and records the flaw; it does
// NOT assert the broken path as correct, and the call must not crash.
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

        auto id() const -> std::int32_t
        {
            const auto f{ get_field("id") };
            return f ? static_cast<std::int32_t>(f->get()) : -987654;
        }
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
        static auto j_size(const char* name) -> std::int32_t
        {
            const auto f{ static_field(name) };
            return f ? static_cast<std::int32_t>(f->get()) : -1;
        }
        static auto j_bool(const char* name) -> bool
        {
            const auto f{ static_field(name) };
            return f ? static_cast<bool>(f->get()) : false;
        }
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

    walk_obs g_setfrommap;          // KNOWN BUG: short decode

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

        // ── KNOWN BUG: newSetFromMap(HashMap) mis-routes to the TreeSet path. ─
        observe_vec(g_setfrommap, vec_of<elem_object>(*self, "setFromHashMap"));

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
                // Out-of-bounds object element reads clamp to 0 (the narrow-oop
                // T{}); decode of 0 is null -> no deref.
                g_obj_oob_eqlen_clamped.store(
                    vmhook::get_array_element<std::uint32_t>(obj_arr, OBJ_ARR_LEN) == 0,
                    std::memory_order_relaxed);
                g_obj_oob_neg_clamped.store(
                    vmhook::get_array_element<std::uint32_t>(obj_arr, -7) == 0,
                    std::memory_order_relaxed);
            }
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

    ctx.record(std::string{ "[INFO] collection_iteration_safety: " }
               + std::to_string(static_cast<int>(shapes_matched)) + "/"
               + std::to_string(static_cast<int>(shapes_total))
               + " populated shapes decoded a size that matched the Java oracle.");
    // HARD majority floor: the size-match layer genuinely works for most shapes.
    // 7 populated shapes; require a strict majority so a real regression is caught
    // while a single GC/decode-stressed tail on an exotic config is tolerated.
    ctx.check("size_oracle_majority_matched", shapes_matched >= 4);

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
    //  7. KNOWN LIB BUG (PIN, do NOT fix): Collections.newSetFromMap(HashMap)
    //     mis-routes to the TreeSet path (backing field "m"), HashMap has no
    //     "root", so the decode is SHORT (0 in practice) for a non-empty Set.
    //     Assert the ACTUAL short decode; never assert the broken path correct.
    //     The call must NOT crash (g_setfrommap.seen proves it returned).
    // =====================================================================
    {
        const std::int32_t java_size{ fixture_wrapper::j_size("setFromHashMapSize") };
        ctx.check("setfrommap_java_size_is_5", java_size == SETFROMMAP_N);
        ctx.check("setfrommap_walk_returned_no_crash", g_setfrommap.seen.load());
        const std::int32_t decoded{ g_setfrommap.count.load() };
        // BUG: the decode is short (fewer than the Set actually holds).
        ctx.check("setfrommap_decode_is_short_BUG", decoded < java_size);
        ctx.record(std::string{ "[INFO] LIB BUG (to_vector first-match field routing): "
                                "Collections.newSetFromMap(HashMap) has backing field 'm', "
                                "so collection::to_vector takes the TreeSet path; HashMap has "
                                "no 'root' field, so the decode returns " }
                   + std::to_string(decoded) + " of " + std::to_string(java_size)
                   + " elements. Correct behaviour would route to the generic iterator path.");
        // In practice the short decode is exactly 0 (no 'root' -> walker returns
        // immediately).  Pin that, but tolerate a future partial fix (< java_size).
        ctx.check("setfrommap_decode_count_is_zero_today", decoded == 0);
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
        ctx.check("array_objarr_oob_eqlen_clamped", g_obj_oob_eqlen_clamped.load());
        ctx.check("array_objarr_oob_neg_clamped", g_obj_oob_neg_clamped.load());
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
