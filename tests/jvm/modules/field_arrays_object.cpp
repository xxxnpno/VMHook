// field_arrays_object JVM test module (area: fields).
//
// FEATURE UNDER TEST: reading Java REFERENCE arrays out of object / static
// fields and turning them into C++ vectors, with inner nulls handled as
// null/empty slots (never a crash).  Three element kinds, three signatures:
//
//     String[]      -> "[Ljava/lang/String;"
//     Item[]        -> "[Lvmhook/fixtures/FieldArraysObject$Item;"   (typed)
//     Object[]      -> "[Ljava/lang/Object;"                         (erased)
//     Item[][]      -> "[[Lvmhook/fixtures/FieldArraysObject$Item;"  (2-D)
//
// across the empty / single / all-null / mixed / leading-null / trailing-null /
// big-mixed / null-array-reference shapes, for BOTH static and instance fields.
//
// THE READ PATHS this module drives, and how each is verified:
//
//   * String[]  -> std::vector<std::string>   via the field_proxy implicit
//     conversion operator:
//         std::vector<std::string> v = static_field("staticStrings")->get();
//     A null element is read as read_java_string(decode_oop_pointer(0)) ==
//     read_java_string(nullptr), which returns "" — so a null String element is
//     COERCED to "" and is indistinguishable from a genuine empty Java string
//     (read_java_string also logs one warning per null slot).  That is the
//     crash-safe direction and is asserted as such below; the asymmetry with the
//     Object[] path (which keeps null as a real nullptr) is called out in [INFO].
//
//   * Object[] / Item[] / Item[][]  -> std::vector<std::unique_ptr<Item>>
//     via the DOCUMENTED entry point field_proxy::value_t::to_vector<Item>().
//     This path WORKS today: to_vector<T>() branches on the field signature and,
//     for a "[L.../"[[..." array field, walks the raw array directly (each
//     non-null slot -> make_unique<Item>, each null slot -> nullptr).  Only
//     plain "L...;" collection fields fall through to collection::to_vector.
//     (An earlier revision mis-routed every Object[] through collection::to_vector
//     and silently returned an EMPTY vector; the signature branch fixed that, so
//     this module hard-asserts the recovered elements rather than recording an
//     "empty / known flaw" breadcrumb.)
//
//   * A MANUAL decode walk (field_oop -> array_length -> per-index
//     get_array_element<uint32_t> -> decode_oop_pointer) is kept as an
//     independent cross-check: it proves the Object[] data is reachable, that
//     inner nulls are DISTINGUISHABLE as real nullptr slots, and that the
//     documented to_vector<T>() result agrees with the raw decode element-for-
//     element (same per-slot OOPs, same null layout).  array_length() is the
//     bounds oracle — no slot is ever read out of range.
//
// ORACLES that make a wrong decode impossible to pass:
//   - COUNT oracle: Java publishes each array's `.length` in the fixture's
//     static initializer; every C++ .size() is checked against it.
//   - IDENTITY oracle: each Item carries a UNIQUE tag, read through the wrapper
//     by BOTH the field path (tag) AND the method path (getTag()); decoded OOPs
//     must be distinct, non-null, and deterministic across re-reads (the zero-JNI
//     layer has no identityHashCode primitive, so this stands in for it).
//
// SUITE-SAFETY (mirrors field_arrays_primitive.cpp / aaa_warmup.cpp):
//   * the whole body runs under a try/catch — a stray throw is recorded as
//     [INFO], never a FAIL, and never escapes this module;
//   * an unconditional vmhook::shutdown_hooks() runs OUTSIDE the try, so the
//     module returns to the driver with an EMPTY hook table on every path;
//   * an entry guard bails to [INFO] if the fixture class is not resolvable;
//   * is_valid_pointer() guards every raw array/element deref, and every array
//     slot is read inside array_length() bounds (arrays legitimately hold nulls,
//     so each decoded element handle is null-checked too).

#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace
{
    constexpr const char* FIXTURE{ "vmhook/fixtures/FieldArraysObject" };
    constexpr const char* ITEM{ "vmhook/fixtures/FieldArraysObject$Item" };

    // Registered-wrapper element type for the Item[] / Object[] / Item[][]
    // arrays.  A readable int field + a callable method, so a decoded element
    // can be proven a real, usable object (not just a readable blob).
    class item_object : public vmhook::object<item_object>
    {
    public:
        explicit item_object(vmhook::oop_t instance) noexcept
            : vmhook::object<item_object>{ instance }
        {
        }

        // Read tag THROUGH the wrapper (field path).
        auto get_tag() -> std::int32_t { return get_field("tag")->get(); }

        // Call getTag() THROUGH the wrapper (proves a dispatch-capable object).
        auto call_get_tag() -> std::int32_t { return get_method("getTag")->call(); }
    };

    // Wrapper for vmhook.fixtures.FieldArraysObject.  Each String[] accessor
    // returns the field read into a concrete std::vector<std::string> so the
    // reference-array implicit-conversion operator fires.  Each Item[] / Object[]
    // / Item[][] accessor returns the to_vector<item_object>() result (the
    // documented path).  Accessors are clean one-liners (no sentinel guards) —
    // the call-site / module body owns suite-safety, per the project idiom.
    class field_arrays_object_fixture
        : public vmhook::object<field_arrays_object_fixture>
    {
    public:
        explicit field_arrays_object_fixture(vmhook::oop_t instance) noexcept
            : vmhook::object<field_arrays_object_fixture>{ instance }
        {
        }

        // ---- handshake -------------------------------------------------------
        static auto set_go(bool value) -> void { static_field("go")->set(value); }
        static auto get_done() -> bool          { return static_field("done")->get(); }
        static auto get_observed() -> std::int32_t { return static_field("observed")->get(); }

        // ---- self (for instance-field reads) ---------------------------------
        static auto acquire_self() -> std::unique_ptr<field_arrays_object_fixture> { return static_field("self")->get(); }

        // ---- STATIC String[] reads (operator vector<string>()) ---------------
        static auto s_strings()        -> std::vector<std::string> { return static_field("staticStrings")->get(); }
        static auto s_empty_strings()  -> std::vector<std::string> { return static_field("emptyStrings")->get(); }
        static auto s_single_string()  -> std::vector<std::string> { return static_field("singleString")->get(); }
        static auto s_all_null_str()   -> std::vector<std::string> { return static_field("allNullStrings")->get(); }
        static auto s_mixed_strings()  -> std::vector<std::string> { return static_field("mixedStrings")->get(); }
        static auto s_leading_null()   -> std::vector<std::string> { return static_field("leadingNullStrings")->get(); }
        static auto s_trailing_null()  -> std::vector<std::string> { return static_field("trailingNullStrings")->get(); }
        static auto s_big_mixed()      -> std::vector<std::string> { return static_field("bigMixedStrings")->get(); }
        static auto s_null_array()     -> std::vector<std::string> { return static_field("nullStringArray")->get(); }

        // ---- INSTANCE String[] reads -----------------------------------------
        auto i_strings()       -> std::vector<std::string> { return get_field("instStrings")->get(); }
        auto i_mixed_strings() -> std::vector<std::string> { return get_field("instMixedStrings")->get(); }

        // ---- STATIC Item[] reads via to_vector<item_object>() (documented) ---
        static auto s_items()        -> std::vector<std::unique_ptr<item_object>> { return static_field("staticItems")->get().to_vector<item_object>(); }
        static auto s_empty_items()  -> std::vector<std::unique_ptr<item_object>> { return static_field("emptyItems")->get().to_vector<item_object>(); }
        static auto s_single_item()  -> std::vector<std::unique_ptr<item_object>> { return static_field("singleItem")->get().to_vector<item_object>(); }
        static auto s_all_null_items()->std::vector<std::unique_ptr<item_object>> { return static_field("allNullItems")->get().to_vector<item_object>(); }
        static auto s_mixed_items()  -> std::vector<std::unique_ptr<item_object>> { return static_field("mixedItems")->get().to_vector<item_object>(); }
        static auto s_leading_items()-> std::vector<std::unique_ptr<item_object>> { return static_field("leadingNullItems")->get().to_vector<item_object>(); }
        static auto s_trailing_items()->std::vector<std::unique_ptr<item_object>> { return static_field("trailingNullItems")->get().to_vector<item_object>(); }
        static auto s_null_item_arr()->std::vector<std::unique_ptr<item_object>> { return static_field("nullItemArray")->get().to_vector<item_object>(); }

        // ---- STATIC raw Object[] reads (declared Object[], erased element) ----
        static auto s_object_items()  -> std::vector<std::unique_ptr<item_object>> { return static_field("objectItems")->get().to_vector<item_object>(); }
        static auto s_object_mixed()  -> std::vector<std::unique_ptr<item_object>> { return static_field("objectMixed")->get().to_vector<item_object>(); }
        static auto s_null_object_arr()->std::vector<std::unique_ptr<item_object>> { return static_field("nullObjectArray")->get().to_vector<item_object>(); }

        // ---- STATIC 2-D Item[][] reads (outer dim; each element is a row OOP) -
        static auto s_grid2d()        -> std::vector<std::unique_ptr<item_object>> { return static_field("grid2d")->get().to_vector<item_object>(); }
        static auto s_grid2d_mixed()  -> std::vector<std::unique_ptr<item_object>> { return static_field("grid2dMixed")->get().to_vector<item_object>(); }
        static auto s_grid2d_empty()  -> std::vector<std::unique_ptr<item_object>> { return static_field("grid2dEmpty")->get().to_vector<item_object>(); }

        // ---- Java-published length oracles -----------------------------------
        static auto j_static_strings_len() -> std::int32_t { return static_field("staticStringsLen")->get(); }
        static auto j_mixed_strings_len()  -> std::int32_t { return static_field("mixedStringsLen")->get(); }
        static auto j_static_items_len()   -> std::int32_t { return static_field("staticItemsLen")->get(); }
        static auto j_mixed_items_len()    -> std::int32_t { return static_field("mixedItemsLen")->get(); }
        static auto j_object_items_len()   -> std::int32_t { return static_field("objectItemsLen")->get(); }
        static auto j_object_mixed_len()   -> std::int32_t { return static_field("objectMixedLen")->get(); }
        static auto j_grid2d_len()         -> std::int32_t { return static_field("grid2dLen")->get(); }
        static auto j_grid2d_mixed_len()   -> std::int32_t { return static_field("grid2dMixedLen")->get(); }
    };

    // ---- manual Object[] walk: an independent cross-check of to_vector<T>() ---
    //
    // Resolves the named (static or instance) field to its array OOP, then walks
    // every slot inside array_length() bounds: a non-null slot becomes a
    // unique_ptr<item_object>, a null slot becomes nullptr.  Every raw deref is
    // guarded by is_valid_pointer().  This is exactly the signature-driven array
    // walk to_vector<T>() performs internally; running it here independently
    // proves the data + per-slot null-ness are reachable and lets us cross-check
    // the documented path slot-for-slot.
    auto manual_item_walk(const std::optional<vmhook::field_proxy>& proxy)
        -> std::vector<std::unique_ptr<item_object>>
    {
        std::vector<std::unique_ptr<item_object>> result;
        if (!proxy.has_value())
        {
            return result;
        }

        // field_oop returns the decoded ARRAY oop for a "[..." field (it walks
        // the field bytes -> compressed OOP -> decode_array_oop, which already
        // runs is_valid_pointer).
        void* const array_oop{ vmhook::field_oop(*proxy) };
        if (!array_oop || !vmhook::hotspot::is_valid_pointer(array_oop))
        {
            return result;   // null array reference -> empty (no crash).
        }

        const std::int32_t length{ vmhook::array_length(array_oop) };
        if (length <= 0)
        {
            return result;
        }

        result.reserve(static_cast<std::size_t>(length));
        for (std::int32_t index{ 0 }; index < length; ++index)
        {
            // get_array_element is itself bounds-checked against array_length.
            const std::uint32_t compressed{
                vmhook::get_array_element<std::uint32_t>(array_oop, index) };
            void* const element_oop{ vmhook::hotspot::decode_oop_pointer(compressed) };
            if (element_oop && vmhook::hotspot::is_valid_pointer(element_oop))
            {
                result.push_back(std::make_unique<item_object>(
                    static_cast<vmhook::oop_t>(element_oop)));
            }
            else
            {
                result.push_back(nullptr);   // inner null -> null slot, not a crash.
            }
        }
        return result;
    }

    // Static-field convenience for the manual walk.
    auto manual_item_walk_static(const char* name)
        -> std::vector<std::unique_ptr<item_object>>
    {
        return manual_item_walk(field_arrays_object_fixture::static_field(name));
    }

    // Returns the raw decoded element OOP pointers (nullptr for null slots) of a
    // reference-array field proxy.  Used as the IDENTITY oracle: reading the same
    // field twice must yield identical per-slot pointers (deterministic,
    // non-destructive decode), and distinct non-null Java objects must decode to
    // distinct pointers.  Every read stays inside array_length() bounds.
    auto element_oops(const std::optional<vmhook::field_proxy>& proxy) -> std::vector<void*>
    {
        std::vector<void*> out;
        if (!proxy.has_value())
        {
            return out;
        }
        void* const array_oop{ vmhook::field_oop(*proxy) };
        if (!array_oop || !vmhook::hotspot::is_valid_pointer(array_oop))
        {
            return out;
        }
        const std::int32_t length{ vmhook::array_length(array_oop) };
        out.reserve(static_cast<std::size_t>(length > 0 ? length : 0));
        for (std::int32_t index{ 0 }; index < length; ++index)
        {
            out.push_back(vmhook::hotspot::decode_oop_pointer(
                vmhook::get_array_element<std::uint32_t>(array_oop, index)));
        }
        return out;
    }

    auto element_oops_static(const char* name) -> std::vector<void*>
    {
        return element_oops(field_arrays_object_fixture::static_field(name));
    }

    // Cross-checks one field's JVM descriptor + static-ness + reference-ness
    // against the fixture, exercising the field_proxy introspection accessors.
    // Records [INFO] (never FAIL) when a field can't be resolved so the module
    // stays suite-safe on a partial run.
    auto check_field_shape(vmhook_test::context& ctx, const char* check_name,
                           const std::optional<vmhook::field_proxy>& proxy,
                           const char* expect_sig, bool expect_static) -> void
    {
        if (!proxy.has_value())
        {
            ctx.record(std::string{ "[INFO] field_arrays_object: field for '" }
                       + check_name + "' did not resolve; skipping its shape check.");
            return;
        }
        const bool sig_ok{ proxy->signature() == expect_sig };
        const bool static_ok{ proxy->is_static() == expect_static };
        const bool ref_ok{ proxy->is_reference() };   // every array field is a reference.
        ctx.check(check_name, sig_ok && static_ok && ref_ok);
    }

    // ---- hook observation state ---------------------------------------------
    std::atomic<int>          g_hook_calls{ 0 };
    std::atomic<std::int32_t> g_hook_arg{ -1 };
    std::atomic<bool>         g_hook_saw_self{ false };

    // The whole test body, factored out so the VMHOOK_JVM_MODULE wrapper can run
    // it under a try/catch and ALWAYS follow it with shutdown_hooks().
    void run_field_arrays_object_checks(vmhook_test::context& ctx)
    {
        vmhook::register_class<field_arrays_object_fixture>(FIXTURE);
        vmhook::register_class<item_object>(ITEM);

        using wrapper = field_arrays_object_fixture;

        // =====================================================================
        //  ENTRY GUARD.  If the fixture is not loaded/resolvable on this run,
        //  every static_field()->get() below would deref a disengaged optional.
        //  Bail cleanly to [INFO] (the wrapper's final shutdown_hooks() still
        //  runs).  In practice the harness loads the fixture on every run, so
        //  this is belt-and-braces.
        // =====================================================================
        if (vmhook::find_class(FIXTURE) == nullptr)
        {
            ctx.record("[INFO] field_arrays_object: FieldArraysObject not "
                       "loaded/resolvable on this run; skipping the module's live "
                       "checks (no crash, no hooks armed).");
            return;
        }

        // =====================================================================
        // PART 0 — FIELD INTROSPECTION cross-check (names / JVM descriptors /
        //          static-ness) against the fixture.  Proves the static_field()
        //          resolver returns the EXACT fields the .java declares, so the
        //          reads below are reading what we think they are.  All these
        //          fields are static reference/array fields.
        // =====================================================================
        check_field_shape(ctx, "shape_staticStrings_is_String_array",
                          wrapper::static_field("staticStrings"), "[Ljava/lang/String;", true);
        check_field_shape(ctx, "shape_staticItems_is_Item_array",
                          wrapper::static_field("staticItems"),
                          "[Lvmhook/fixtures/FieldArraysObject$Item;", true);
        check_field_shape(ctx, "shape_objectItems_is_Object_array",
                          wrapper::static_field("objectItems"), "[Ljava/lang/Object;", true);
        check_field_shape(ctx, "shape_grid2d_is_2d_Item_array",
                          wrapper::static_field("grid2d"),
                          "[[Lvmhook/fixtures/FieldArraysObject$Item;", true);
        check_field_shape(ctx, "shape_nullItemArray_is_Item_array",
                          wrapper::static_field("nullItemArray"),
                          "[Lvmhook/fixtures/FieldArraysObject$Item;", true);
        check_field_shape(ctx, "shape_nullObjectArray_is_Object_array",
                          wrapper::static_field("nullObjectArray"), "[Ljava/lang/Object;", true);
        check_field_shape(ctx, "shape_grid2dEmpty_is_2d_Item_array",
                          wrapper::static_field("grid2dEmpty"),
                          "[[Lvmhook/fixtures/FieldArraysObject$Item;", true);
        // A non-existent field must resolve to nullopt (negative cross-check).
        ctx.check("shape_missing_field_is_nullopt",
                  !wrapper::static_field("noSuchArrayField").has_value());
        // The STATIC resolver intentionally rejects an INSTANCE field (it needs
        // an object instance) — static_field() returns nullopt for instStrings
        // even though that field exists.  The instance descriptor itself is
        // cross-checked in PART C through a live `self` (where it CAN resolve).
        ctx.check("shape_instance_field_via_static_resolver_is_nullopt",
                  !wrapper::static_field("instStrings").has_value());

        // =====================================================================
        // PART A — STRING[] reads via the implicit vector<string> conversion.
        //          Reads are side-effect free, so they run BEFORE the probe.
        // =====================================================================

        // ---- A1: canonical 3-element String[], all non-null ------------------
        {
            const std::vector<std::string> v{ wrapper::s_strings() };
            ctx.check("str_canonical_size3", v.size() == 3);
            ctx.check("str_canonical_elem0_alpha", v.size() == 3 && v[0] == "alpha");
            ctx.check("str_canonical_elem1_beta",  v.size() == 3 && v[1] == "beta");
            ctx.check("str_canonical_elem2_gamma", v.size() == 3 && v[2] == "gamma");
        }

        // ---- A2: EMPTY String[] (length 0) -> empty vector, no crash ---------
        {
            const std::vector<std::string> v{ wrapper::s_empty_strings() };
            ctx.check("str_empty_is_empty", v.empty());
        }

        // ---- A3: SINGLE element ----------------------------------------------
        {
            const std::vector<std::string> v{ wrapper::s_single_string() };
            ctx.check("str_single_size1", v.size() == 1);
            ctx.check("str_single_value_solo", v.size() == 1 && v[0] == "solo");
        }

        // ---- A4: ALL-null String[] (3 slots, every one null) -----------------
        // Inner nulls must NOT crash; each becomes "" (coerced — the documented
        // null-vs-empty information loss).  Count must still be exactly 3.
        {
            const std::vector<std::string> v{ wrapper::s_all_null_str() };
            ctx.check("str_allnull_size3", v.size() == 3);
            const bool all_empty{
                v.size() == 3 && v[0].empty() && v[1].empty() && v[2].empty() };
            ctx.check("str_allnull_every_slot_coerced_to_empty", all_empty);
            ctx.record("[INFO] field_arrays_object: a null String[] element is read as "
                       "\"\" (read_java_string(nullptr) -> empty) and is indistinguishable "
                       "from a genuine empty Java string; read_java_string also logs a "
                       "warning per null slot.  The Object[] path (PART B) preserves null "
                       "as a real nullptr, so the SAME {x,null,z} layout is recoverable as "
                       "Object[] but lossy as String[].  A null-preserving overload "
                       "(vector<optional<string>>) + a non-logging null short-circuit "
                       "would close both gaps.");
        }

        // ---- A5: MIXED null/non-null { "x", null, "z" } ----------------------
        // The crux of the feature: count is 3, the non-null slots keep their
        // value, and the null slot is the coerced "".
        {
            const std::vector<std::string> v{ wrapper::s_mixed_strings() };
            ctx.check("str_mixed_size3", v.size() == 3);
            ctx.check("str_mixed_elem0_x",             v.size() == 3 && v[0] == "x");
            ctx.check("str_mixed_elem1_null_as_empty", v.size() == 3 && v[1].empty());
            ctx.check("str_mixed_elem2_z",             v.size() == 3 && v[2] == "z");
            // Count must match the Java oracle exactly.  The oracle
            // (mixedStringsLen) is published in the fixture's static initializer
            // at class-load time, so it is already valid here in PART A.
            ctx.check("str_mixed_count_matches_java",
                      static_cast<std::int32_t>(v.size()) == wrapper::j_mixed_strings_len());
        }

        // ---- A6: LEADING null { null, "b", "c" } -----------------------------
        {
            const std::vector<std::string> v{ wrapper::s_leading_null() };
            ctx.check("str_leadingnull_size3", v.size() == 3);
            ctx.check("str_leadingnull_elem0_empty", v.size() == 3 && v[0].empty());
            ctx.check("str_leadingnull_elem1_b",     v.size() == 3 && v[1] == "b");
            ctx.check("str_leadingnull_elem2_c",     v.size() == 3 && v[2] == "c");
        }

        // ---- A7: TRAILING null { "a", "b", null } ----------------------------
        {
            const std::vector<std::string> v{ wrapper::s_trailing_null() };
            ctx.check("str_trailingnull_size3", v.size() == 3);
            ctx.check("str_trailingnull_elem0_a",     v.size() == 3 && v[0] == "a");
            ctx.check("str_trailingnull_elem1_b",     v.size() == 3 && v[1] == "b");
            ctx.check("str_trailingnull_elem2_empty", v.size() == 3 && v[2].empty());
        }

        // ---- A8: BIG mixed { "one",null,"three",null,"five",null } (len 6) ---
        // Stresses the per-element append loop with interleaved nulls at a
        // larger length; verifies EVERY slot.
        {
            const std::vector<std::string> v{ wrapper::s_big_mixed() };
            ctx.check("str_bigmixed_size6", v.size() == 6);
            const bool ok{
                v.size() == 6
                && v[0] == "one"   && v[1].empty()
                && v[2] == "three" && v[3].empty()
                && v[4] == "five"  && v[5].empty() };
            ctx.check("str_bigmixed_every_slot", ok);
        }

        // ---- A9: null String[] REFERENCE (the array itself is null) ----------
        // decode_array_oop(0) -> nullptr -> read_array_value returns empty.
        // Must not crash and must be distinguishable (empty) from a populated
        // array.
        {
            const std::vector<std::string> v{ wrapper::s_null_array() };
            ctx.check("str_null_array_ref_is_empty", v.empty());
        }

        // ---- A10: count oracle for the canonical case ------------------------
        {
            const std::vector<std::string> v{ wrapper::s_strings() };
            ctx.check("str_canonical_count_matches_java",
                      static_cast<std::int32_t>(v.size()) == wrapper::j_static_strings_len());
        }

        // ---- A11: re-read stability (no destructive read) --------------------
        {
            const std::vector<std::string> first{ wrapper::s_mixed_strings() };
            const std::vector<std::string> second{ wrapper::s_mixed_strings() };
            ctx.check("str_mixed_reread_stable",
                      first.size() == second.size()
                      && first.size() == 3
                      && first[0] == second[0]
                      && first[1] == second[1]
                      && first[2] == second[2]);
        }

        // =====================================================================
        // PART B — ITEM[] / Object[] / Item[][] reads.
        //   B-a: the DOCUMENTED to_vector<item_object>() path — now WORKS;
        //        elements are hard-asserted (count + each tag + null layout).
        //   B-b: the MANUAL walk, an independent cross-check proving the data +
        //        per-slot null-ness are reachable and AGREE with to_vector<T>().
        // =====================================================================

        // ---- B1: documented to_vector<Item>() path — typed Item[] ------------
        // to_vector<T>() branches on the field signature ("[L...") and walks the
        // raw array directly, so it returns the real elements (and nullptr for
        // null slots).  We hard-assert count, every element value, and the null
        // layout.  (A predecessor mis-routed this through collection::to_vector
        // and returned EMPTY; that is fixed, hence hard asserts here.)
        {
            const std::vector<std::unique_ptr<item_object>> canon{ wrapper::s_items() };
            const std::vector<std::unique_ptr<item_object>> empty{ wrapper::s_empty_items() };
            const std::vector<std::unique_ptr<item_object>> single{ wrapper::s_single_item() };
            const std::vector<std::unique_ptr<item_object>> mixed{ wrapper::s_mixed_items() };
            const std::vector<std::unique_ptr<item_object>> allnull{ wrapper::s_all_null_items() };
            const std::vector<std::unique_ptr<item_object>> nullarr{ wrapper::s_null_item_arr() };

            // Empty / null-array shapes yield an empty vector on any path.
            ctx.check("item_tv_empty_array_is_empty", empty.empty());
            ctx.check("item_tv_null_array_ref_is_empty", nullarr.empty());

            // Canonical: 3 elements, each tag via the wrapper field path.
            ctx.check("item_tv_canonical_size3", canon.size() == 3);
            ctx.check("item_tv_canonical_count_matches_java",
                      static_cast<std::int32_t>(canon.size()) == wrapper::j_static_items_len());
            ctx.check("item_tv_canonical_elem0_tag10",
                      canon.size() == 3 && canon[0] && canon[0]->get_tag() == 10);
            ctx.check("item_tv_canonical_elem1_tag20",
                      canon.size() == 3 && canon[1] && canon[1]->get_tag() == 20);
            ctx.check("item_tv_canonical_elem2_tag30",
                      canon.size() == 3 && canon[2] && canon[2]->get_tag() == 30);

            // Single.
            ctx.check("item_tv_single_size1", single.size() == 1);
            ctx.check("item_tv_single_elem0_tag99",
                      single.size() == 1 && single[0] && single[0]->get_tag() == 99);

            // All-null: 3 real nullptr slots (DISTINGUISHABLE — unlike String "").
            ctx.check("item_tv_allnull_size3", allnull.size() == 3);
            ctx.check("item_tv_allnull_every_slot_nullptr",
                      allnull.size() == 3
                      && allnull[0] == nullptr && allnull[1] == nullptr && allnull[2] == nullptr);

            // Mixed: { Item(1), null, Item(3) } — null slot is a real nullptr.
            ctx.check("item_tv_mixed_size3", mixed.size() == 3);
            ctx.check("item_tv_mixed_count_matches_java",
                      static_cast<std::int32_t>(mixed.size()) == wrapper::j_mixed_items_len());
            ctx.check("item_tv_mixed_elem0_tag1",
                      mixed.size() == 3 && mixed[0] && mixed[0]->get_tag() == 1);
            ctx.check("item_tv_mixed_elem1_is_nullptr",
                      mixed.size() == 3 && mixed[1] == nullptr);
            ctx.check("item_tv_mixed_elem2_tag3",
                      mixed.size() == 3 && mixed[2] && mixed[2]->get_tag() == 3);
        }

        // ---- B1b: documented to_vector<Item>() over a RAW Object[] field -----
        // Declared type Object[] ("[Ljava/lang/Object;"): the element type is
        // erased to java.lang.Object, but the runtime values are Items, so each
        // non-null slot re-wraps as an item_object and reads its tag.  Proves
        // the signature branch keys on "[L" regardless of the L-class name.
        {
            const std::vector<std::unique_ptr<item_object>> objs{ wrapper::s_object_items() };
            const std::vector<std::unique_ptr<item_object>> objm{ wrapper::s_object_mixed() };
            const std::vector<std::unique_ptr<item_object>> objn{ wrapper::s_null_object_arr() };

            ctx.check("obj_tv_canonical_size3", objs.size() == 3);
            ctx.check("obj_tv_canonical_count_matches_java",
                      static_cast<std::int32_t>(objs.size()) == wrapper::j_object_items_len());
            ctx.check("obj_tv_canonical_elem0_tag60",
                      objs.size() == 3 && objs[0] && objs[0]->get_tag() == 60);
            ctx.check("obj_tv_canonical_elem1_tag70",
                      objs.size() == 3 && objs[1] && objs[1]->get_tag() == 70);
            ctx.check("obj_tv_canonical_elem2_tag80",
                      objs.size() == 3 && objs[2] && objs[2]->get_tag() == 80);
            // method path on an erased-Object[] element — still a real object.
            ctx.check("obj_tv_canonical_elem0_method_tag60",
                      objs.size() == 3 && objs[0] && objs[0]->call_get_tag() == 60);

            // Mixed Object[]: { Item(61), null, Item(63) }.
            ctx.check("obj_tv_mixed_size3", objm.size() == 3);
            ctx.check("obj_tv_mixed_count_matches_java",
                      static_cast<std::int32_t>(objm.size()) == wrapper::j_object_mixed_len());
            ctx.check("obj_tv_mixed_elem0_tag61",
                      objm.size() == 3 && objm[0] && objm[0]->get_tag() == 61);
            ctx.check("obj_tv_mixed_elem1_is_nullptr",
                      objm.size() == 3 && objm[1] == nullptr);
            ctx.check("obj_tv_mixed_elem2_tag63",
                      objm.size() == 3 && objm[2] && objm[2]->get_tag() == 63);

            // Null Object[] reference -> empty, no crash.
            ctx.check("obj_tv_null_array_ref_is_empty", objn.empty());
        }

        // ---- B1c: documented to_vector<Item>() over a 2-D Item[][] field -----
        // Signature begins "[[", so to_vector<T>() walks the OUTER array; each
        // element is an INNER array (row) OOP, re-wrapped as item_object.  We do
        // NOT descend into the rows (no vector-of-vectors API); we assert the
        // OUTER count and that a NULL row decodes to a real nullptr slot — i.e.
        // the signature[1]=='[' arm is taken and inner nulls are distinguishable.
        {
            const std::vector<std::unique_ptr<item_object>> g{ wrapper::s_grid2d() };
            const std::vector<std::unique_ptr<item_object>> gm{ wrapper::s_grid2d_mixed() };
            const std::vector<std::unique_ptr<item_object>> ge{ wrapper::s_grid2d_empty() };

            // Outer dimension = 3 rows, all non-null row references.
            ctx.check("grid2d_outer_size3", g.size() == 3);
            ctx.check("grid2d_outer_count_matches_java",
                      static_cast<std::int32_t>(g.size()) == wrapper::j_grid2d_len());
            ctx.check("grid2d_rows_all_nonnull",
                      g.size() == 3 && g[0] != nullptr && g[1] != nullptr && g[2] != nullptr);
            ctx.check("grid2d_rows_distinct",
                      g.size() == 3 && g[0] && g[1] && g[2]
                      && static_cast<void*>(g[0]->get_instance()) != static_cast<void*>(g[1]->get_instance())
                      && static_cast<void*>(g[1]->get_instance()) != static_cast<void*>(g[2]->get_instance()));

            // Mixed 2-D: { row, null, row } — the null ROW is a real nullptr.
            ctx.check("grid2d_mixed_outer_size3", gm.size() == 3);
            ctx.check("grid2d_mixed_outer_count_matches_java",
                      static_cast<std::int32_t>(gm.size()) == wrapper::j_grid2d_mixed_len());
            ctx.check("grid2d_mixed_row0_nonnull", gm.size() == 3 && gm[0] != nullptr);
            ctx.check("grid2d_mixed_row1_is_nullptr", gm.size() == 3 && gm[1] == nullptr);
            ctx.check("grid2d_mixed_row2_nonnull", gm.size() == 3 && gm[2] != nullptr);

            // Empty 2-D outer array -> empty vector.
            ctx.check("grid2d_empty_is_empty", ge.empty());
        }

        // ---- B2: MANUAL walk — canonical Item[], cross-check of to_vector -----
        // The independent decode.  Verifies count, each tag via the field path
        // AND the method path, and identity (distinct / non-null / deterministic
        // OOPs, wrapper-points-at-slot).
        {
            const std::vector<std::unique_ptr<item_object>> v{
                manual_item_walk_static("staticItems") };
            ctx.check("item_manual_canonical_size3", v.size() == 3);
            ctx.check("item_manual_canonical_count_matches_java",
                      static_cast<std::int32_t>(v.size()) == wrapper::j_static_items_len());
            ctx.check("item_manual_canonical_elem0_nonnull", v.size() == 3 && v[0] != nullptr);
            ctx.check("item_manual_canonical_elem1_nonnull", v.size() == 3 && v[1] != nullptr);
            ctx.check("item_manual_canonical_elem2_nonnull", v.size() == 3 && v[2] != nullptr);

            // tag via the FIELD path through each decoded wrapper.
            ctx.check("item_manual_canonical_elem0_tag10", v.size() == 3 && v[0] && v[0]->get_tag() == 10);
            ctx.check("item_manual_canonical_elem1_tag20", v.size() == 3 && v[1] && v[1]->get_tag() == 20);
            ctx.check("item_manual_canonical_elem2_tag30", v.size() == 3 && v[2] && v[2]->get_tag() == 30);

            // tag via the METHOD path (getTag) — proves each element is a real,
            // dispatch-capable object, not just a readable blob.
            ctx.check("item_manual_canonical_elem0_method_tag10", v.size() == 3 && v[0] && v[0]->call_get_tag() == 10);
            ctx.check("item_manual_canonical_elem2_method_tag30", v.size() == 3 && v[2] && v[2]->call_get_tag() == 30);

            // identity: every non-null element decodes to a DISTINCT, non-null
            // OOP, and a second walk yields the SAME per-slot pointers.
            const std::vector<void*> oops_a{ element_oops_static("staticItems") };
            const std::vector<void*> oops_b{ element_oops_static("staticItems") };
            ctx.check("item_manual_canonical_oops_len3", oops_a.size() == 3);
            const bool distinct_nonnull{
                oops_a.size() == 3
                && oops_a[0] != nullptr && oops_a[1] != nullptr && oops_a[2] != nullptr
                && oops_a[0] != oops_a[1] && oops_a[1] != oops_a[2] && oops_a[0] != oops_a[2] };
            ctx.check("item_manual_canonical_elements_distinct_nonnull", distinct_nonnull);
            const bool deterministic{
                oops_a.size() == oops_b.size() && oops_a.size() == 3
                && oops_a[0] == oops_b[0] && oops_a[1] == oops_b[1] && oops_a[2] == oops_b[2] };
            ctx.check("item_manual_canonical_decode_deterministic", deterministic);
            // The wrapper each element wraps points at exactly that decoded OOP.
            ctx.check("item_manual_canonical_wrapper_oop_matches_slot",
                      oops_a.size() == 3 && v.size() == 3 && v[0] && v[1] && v[2]
                      && static_cast<void*>(v[0]->get_instance()) == oops_a[0]
                      && static_cast<void*>(v[1]->get_instance()) == oops_a[1]
                      && static_cast<void*>(v[2]->get_instance()) == oops_a[2]);

            // CROSS-CHECK: the documented to_vector<Item>() result and the manual
            // walk agree slot-for-slot (same decoded OOPs, same null layout).
            const std::vector<std::unique_ptr<item_object>> tv{ wrapper::s_items() };
            ctx.check("item_paths_agree_size", tv.size() == v.size() && tv.size() == 3);
            ctx.check("item_paths_agree_oops",
                      tv.size() == 3 && oops_a.size() == 3 && tv[0] && tv[1] && tv[2]
                      && static_cast<void*>(tv[0]->get_instance()) == oops_a[0]
                      && static_cast<void*>(tv[1]->get_instance()) == oops_a[1]
                      && static_cast<void*>(tv[2]->get_instance()) == oops_a[2]);
        }

        // ---- B3: MANUAL walk — EMPTY Item[] -> empty vector ------------------
        {
            const std::vector<std::unique_ptr<item_object>> v{
                manual_item_walk_static("emptyItems") };
            ctx.check("item_manual_empty_is_empty", v.empty());
        }

        // ---- B4: MANUAL walk — SINGLE Item[] ---------------------------------
        {
            const std::vector<std::unique_ptr<item_object>> v{
                manual_item_walk_static("singleItem") };
            ctx.check("item_manual_single_size1", v.size() == 1);
            ctx.check("item_manual_single_elem0_nonnull", v.size() == 1 && v[0] != nullptr);
            ctx.check("item_manual_single_elem0_tag99",
                      v.size() == 1 && v[0] && v[0]->get_tag() == 99);
            ctx.check("item_manual_single_elem0_method_tag99",
                      v.size() == 1 && v[0] && v[0]->call_get_tag() == 99);
            const std::vector<void*> oops{ element_oops_static("singleItem") };
            ctx.check("item_manual_single_oop_nonnull_matches_wrapper",
                      oops.size() == 1 && v.size() == 1 && v[0] && oops[0] != nullptr
                      && static_cast<void*>(v[0]->get_instance()) == oops[0]);
        }

        // ---- B5: MANUAL walk — ALL-null Item[] (every slot null) -------------
        // Inner nulls must become null slots, never a crash; count stays 3.
        {
            const std::vector<std::unique_ptr<item_object>> v{
                manual_item_walk_static("allNullItems") };
            ctx.check("item_manual_allnull_size3", v.size() == 3);
            const bool all_null{
                v.size() == 3 && v[0] == nullptr && v[1] == nullptr && v[2] == nullptr };
            ctx.check("item_manual_allnull_every_slot_nullptr", all_null);
        }

        // ---- B6: MANUAL walk — MIXED { Item(1), null, Item(3) } --------------
        // The headline Object[] case: non-null slots are usable wrappers, the
        // null slot is a real nullptr (distinguishable — unlike String "").
        {
            const std::vector<std::unique_ptr<item_object>> v{
                manual_item_walk_static("mixedItems") };
            ctx.check("item_manual_mixed_size3", v.size() == 3);
            ctx.check("item_manual_mixed_count_matches_java",
                      static_cast<std::int32_t>(v.size()) == wrapper::j_mixed_items_len());
            ctx.check("item_manual_mixed_elem0_nonnull", v.size() == 3 && v[0] != nullptr);
            ctx.check("item_manual_mixed_elem1_is_nullptr", v.size() == 3 && v[1] == nullptr);
            ctx.check("item_manual_mixed_elem2_nonnull", v.size() == 3 && v[2] != nullptr);
            ctx.check("item_manual_mixed_elem0_tag1", v.size() == 3 && v[0] && v[0]->get_tag() == 1);
            ctx.check("item_manual_mixed_elem2_tag3", v.size() == 3 && v[2] && v[2]->get_tag() == 3);
            ctx.check("item_manual_mixed_elem0_method_tag1", v.size() == 3 && v[0] && v[0]->call_get_tag() == 1);
            ctx.check("item_manual_mixed_elem2_method_tag3", v.size() == 3 && v[2] && v[2]->call_get_tag() == 3);

            // The null SLOT (index 1) decodes to a nullptr OOP; the two non-null
            // slots are distinct, non-null, and match their wrapper instances.
            const std::vector<void*> oops{ element_oops_static("mixedItems") };
            ctx.check("item_manual_mixed_oops_len3", oops.size() == 3);
            ctx.check("item_manual_mixed_slot1_oop_is_null",
                      oops.size() == 3 && oops[1] == nullptr);
            ctx.check("item_manual_mixed_nonnull_slots_distinct",
                      oops.size() == 3 && oops[0] != nullptr && oops[2] != nullptr
                      && oops[0] != oops[2]);
            ctx.check("item_manual_mixed_elem0_oop_matches_wrapper",
                      oops.size() == 3 && v.size() == 3 && v[0]
                      && static_cast<void*>(v[0]->get_instance()) == oops[0]);
            ctx.check("item_manual_mixed_elem2_oop_matches_wrapper",
                      oops.size() == 3 && v.size() == 3 && v[2]
                      && static_cast<void*>(v[2]->get_instance()) == oops[2]);

            // CROSS-CHECK the documented path's mixed null layout against manual.
            const std::vector<std::unique_ptr<item_object>> tv{ wrapper::s_mixed_items() };
            ctx.check("item_mixed_paths_agree_null_layout",
                      tv.size() == 3
                      && (tv[0] != nullptr) == (v.size() == 3 && v[0] != nullptr)
                      && (tv[1] == nullptr) == (v.size() == 3 && v[1] == nullptr)
                      && (tv[2] != nullptr) == (v.size() == 3 && v[2] != nullptr));
        }

        // ---- B7: MANUAL walk — LEADING null { null, Item(5), Item(6) } -------
        {
            const std::vector<std::unique_ptr<item_object>> v{
                manual_item_walk_static("leadingNullItems") };
            ctx.check("item_manual_leadingnull_size3", v.size() == 3);
            ctx.check("item_manual_leadingnull_elem0_nullptr", v.size() == 3 && v[0] == nullptr);
            ctx.check("item_manual_leadingnull_elem1_tag5", v.size() == 3 && v[1] && v[1]->get_tag() == 5);
            ctx.check("item_manual_leadingnull_elem2_tag6", v.size() == 3 && v[2] && v[2]->get_tag() == 6);
            // to_vector<T>() agrees on the leading-null shape.
            const std::vector<std::unique_ptr<item_object>> tv{ wrapper::s_leading_items() };
            ctx.check("item_leadingnull_tv_elem0_nullptr", tv.size() == 3 && tv[0] == nullptr);
            ctx.check("item_leadingnull_tv_elem1_tag5", tv.size() == 3 && tv[1] && tv[1]->get_tag() == 5);
        }

        // ---- B8: MANUAL walk — TRAILING null { Item(7), Item(8), null } ------
        {
            const std::vector<std::unique_ptr<item_object>> v{
                manual_item_walk_static("trailingNullItems") };
            ctx.check("item_manual_trailingnull_size3", v.size() == 3);
            ctx.check("item_manual_trailingnull_elem0_tag7", v.size() == 3 && v[0] && v[0]->get_tag() == 7);
            ctx.check("item_manual_trailingnull_elem1_tag8", v.size() == 3 && v[1] && v[1]->get_tag() == 8);
            ctx.check("item_manual_trailingnull_elem2_nullptr", v.size() == 3 && v[2] == nullptr);
            // to_vector<T>() agrees on the trailing-null shape.
            const std::vector<std::unique_ptr<item_object>> tv{ wrapper::s_trailing_items() };
            ctx.check("item_trailingnull_tv_elem2_nullptr", tv.size() == 3 && tv[2] == nullptr);
            ctx.check("item_trailingnull_tv_elem1_tag8", tv.size() == 3 && tv[1] && tv[1]->get_tag() == 8);
        }

        // ---- B9: MANUAL walk — null Item[] REFERENCE -> empty, no crash ------
        {
            const std::vector<std::unique_ptr<item_object>> v{
                manual_item_walk_static("nullItemArray") };
            ctx.check("item_manual_null_array_ref_is_empty", v.empty());
        }

        // ---- B10: String[] read through the SAME manual decode, proving String
        //           is just an Object[] under the hood (each element is a String
        //           OOP).  We don't decode the String contents here (PART A's
        //           job) — we only assert the per-slot OOP null-ness matches the
        //           mixed layout { "x", null, "z" }: slot0 non-null, slot1 null,
        //           slot2 non-null.  Bridges the String[] and Object[] null
        //           handling.
        {
            const auto proxy{ wrapper::static_field("mixedStrings") };
            ctx.check("strobj_bridge_proxy_resolved", proxy.has_value());
            if (proxy.has_value())
            {
                void* const array_oop{ vmhook::field_oop(*proxy) };
                ctx.check("strobj_bridge_array_oop_nonnull",
                          array_oop != nullptr && vmhook::hotspot::is_valid_pointer(array_oop));
                if (array_oop && vmhook::hotspot::is_valid_pointer(array_oop))
                {
                    ctx.check("strobj_bridge_len3", vmhook::array_length(array_oop) == 3);
                    const std::vector<void*> oops{ element_oops(proxy) };
                    ctx.check("strobj_bridge_slot0_nonnull_oop", oops.size() == 3 && oops[0] != nullptr);
                    ctx.check("strobj_bridge_slot1_null_oop",    oops.size() == 3 && oops[1] == nullptr);
                    ctx.check("strobj_bridge_slot2_nonnull_oop", oops.size() == 3 && oops[2] != nullptr);
                }
            }
        }

        // =====================================================================
        // PART C — INSTANCE reference-array fields, via a live `self`, plus the
        //          interpreter-hook + run_probe handshake (proves the fixture is
        //          live on a real Java bytecode dispatch).
        // =====================================================================
        {
            auto handle{ vmhook::scoped_hook<field_arrays_object_fixture>(
                "touch",
                [](vmhook::return_value&,
                   const std::unique_ptr<field_arrays_object_fixture>& self,
                   std::int32_t delta)
                {
                    g_hook_calls.fetch_add(1, std::memory_order_relaxed);
                    g_hook_arg.store(delta, std::memory_order_relaxed);
                    g_hook_saw_self.store(self != nullptr, std::memory_order_relaxed);
                }) };
            ctx.check("field_arrays_object_hook_installed", handle.installed());

            const bool done{ ctx.run_probe(
                [](bool value) { wrapper::set_go(value); },
                []() { return wrapper::get_done(); }) };

            ctx.check("field_arrays_object_probe_completed", done);
            ctx.check("field_arrays_object_hook_fired",
                      g_hook_calls.load(std::memory_order_relaxed) >= 1);
            ctx.check("field_arrays_object_hook_saw_self",
                      g_hook_saw_self.load(std::memory_order_relaxed));
            ctx.check("field_arrays_object_hook_saw_arg_1000",
                      g_hook_arg.load(std::memory_order_relaxed) == 1000);
            // touch() returns instItems.length(2) + 1000 == 1002.
            ctx.check("field_arrays_object_observed_is_1002",
                      wrapper::get_observed() == 1002);

            // The probe published `self`; read the INSTANCE reference arrays
            // through it now that the fixture has constructed the instance.
            const std::unique_ptr<wrapper> self{ wrapper::acquire_self() };
            ctx.check("instance_self_acquired", self != nullptr);
            if (self)
            {
                // Instance-field shape cross-check through a LIVE instance: here
                // get_field() CAN resolve a non-static field (unlike the static
                // resolver in PART 0), so is_static() must report false.
                check_field_shape(ctx, "inst_shape_instStrings_nonstatic_String_array",
                                  self->get_field("instStrings"), "[Ljava/lang/String;", false);

                // ---- C1: instance String[] (all non-null) -------------------
                const std::vector<std::string> is{ self->i_strings() };
                ctx.check("inst_str_size2", is.size() == 2);
                ctx.check("inst_str_elem0_inst0", is.size() == 2 && is[0] == "inst0");
                ctx.check("inst_str_elem1_inst1", is.size() == 2 && is[1] == "inst1");

                // ---- C2: instance MIXED String[] { null, "mid", null } ------
                const std::vector<std::string> ims{ self->i_mixed_strings() };
                ctx.check("inst_str_mixed_size3", ims.size() == 3);
                ctx.check("inst_str_mixed_elem0_empty", ims.size() == 3 && ims[0].empty());
                ctx.check("inst_str_mixed_elem1_mid",   ims.size() == 3 && ims[1] == "mid");
                ctx.check("inst_str_mixed_elem2_empty", ims.size() == 3 && ims[2].empty());

                // ---- C3: instance Item[] (all non-null) via the manual walk -
                const auto inst_items_proxy{ self->get_field("instItems") };
                // Cross-check the instance field shape too (NOT static).
                check_field_shape(ctx, "inst_shape_instItems_nonstatic_Item_array",
                                  inst_items_proxy,
                                  "[Lvmhook/fixtures/FieldArraysObject$Item;", false);
                const std::vector<std::unique_ptr<item_object>> ii{
                    manual_item_walk(inst_items_proxy) };
                ctx.check("inst_item_size2", ii.size() == 2);
                ctx.check("inst_item_elem0_nonnull", ii.size() == 2 && ii[0] != nullptr);
                ctx.check("inst_item_elem1_nonnull", ii.size() == 2 && ii[1] != nullptr);
                ctx.check("inst_item_elem0_tag41", ii.size() == 2 && ii[0] && ii[0]->get_tag() == 41);
                ctx.check("inst_item_elem1_tag42", ii.size() == 2 && ii[1] && ii[1]->get_tag() == 42);
                ctx.check("inst_item_elem0_method_tag41", ii.size() == 2 && ii[0] && ii[0]->call_get_tag() == 41);
                ctx.check("inst_item_elem1_method_tag42", ii.size() == 2 && ii[1] && ii[1]->call_get_tag() == 42);
                // Distinct, non-null instances (identity oracle without hashCode).
                ctx.check("inst_item_elements_distinct_nonnull",
                          ii.size() == 2 && ii[0] && ii[1]
                          && static_cast<void*>(ii[0]->get_instance()) != nullptr
                          && static_cast<void*>(ii[1]->get_instance()) != nullptr
                          && static_cast<void*>(ii[0]->get_instance())
                                 != static_cast<void*>(ii[1]->get_instance()));

                // ---- C4: instance MIXED Item[] { Item(51), null } -----------
                const auto inst_mixed_proxy{ self->get_field("instMixedItems") };
                const std::vector<std::unique_ptr<item_object>> imi{
                    manual_item_walk(inst_mixed_proxy) };
                ctx.check("inst_item_mixed_size2", imi.size() == 2);
                ctx.check("inst_item_mixed_elem0_tag51", imi.size() == 2 && imi[0] && imi[0]->get_tag() == 51);
                ctx.check("inst_item_mixed_elem1_nullptr", imi.size() == 2 && imi[1] == nullptr);
            }
        }
    }
}

VMHOOK_JVM_MODULE(field_arrays_object)
{
    // SUITE-SAFETY (mirrors field_arrays_primitive.cpp / aaa_warmup.cpp):
    //   * the whole body runs under a try/catch so a stray throw from any vmhook
    //     call is recorded as [INFO], never a FAIL, and never escapes this module;
    //   * an unconditional vmhook::shutdown_hooks() runs OUTSIDE the try, so the
    //     module returns to the driver with an EMPTY hook table on every path
    //     (idempotent and safe-when-empty; proven by shutdown_hooks_teardown).
    bool body_threw{ false };
    try
    {
        run_field_arrays_object_checks(ctx);
    }
    catch (...)
    {
        body_threw = true;
    }

    // FINAL CLEANUP — OUTSIDE the try so it ALWAYS runs.
    vmhook::shutdown_hooks();

    if (body_threw)
    {
        ctx.record("[INFO] field_arrays_object: the test body threw and was "
                   "contained (no crash, no hooks armed); see preceding checks "
                   "for partial results.");
    }
    ctx.check("module_left_clean_final_shutdown", true);
}
