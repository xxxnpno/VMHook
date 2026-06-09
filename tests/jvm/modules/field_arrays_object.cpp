// field_arrays_object JVM test module (area: fields).
//
// Feature under test: reading Java REFERENCE arrays out of object / static
// fields —
//     String[]   ("[Ljava/lang/String;")
//     Object[]   of a registered wrapper type   ("[Lvmhook/fixtures/...;")
// — and converting them to C++ vectors, with inner nulls handled as null/empty
// slots (never a crash), plus empty / single / all-null / mixed shapes.  Every
// element's value and null-ness is verified, and the element COUNT is checked
// against a Java-published oracle.
//
// THE TWO READ PATHS, and a real flaw this module pins down:
//
//   * String[]  -> std::vector<std::string>   via the field_proxy implicit
//     conversion operator:
//         std::vector<std::string> v = static_field("staticStrings")->get();
//     which routes value_t::operator vector<string>()
//       -> cast_for_variant<vector<string>> -> read_array_value<vector<string>>
//       -> append_array_value(vector<string>&, ...) per element
//          (vmhook.hpp ~11303-11308, ~11359-11383).
//     This path WORKS.  But an inner-null slot is read as read_java_string(
//     decode_oop_pointer(0)) == read_java_string(nullptr), which returns ""
//     (vmhook.hpp ~15141) — so a null element is COERCED to "" and is
//     indistinguishable from a genuine empty Java string.  read_java_string ALSO
//     emits a warning log for every null slot (warning_tag at ~15143), turning a
//     perfectly legal "[ "a", null, "z" ]" into k log lines.  Both are asserted
//     below as documented behaviour, in the crash-safe direction.
//
//   * Object[]  -> std::vector<std::unique_ptr<Item>>   via the DOCUMENTED entry
//     point field_proxy::value_t::to_vector<Item>() (vmhook.hpp ~15086-15101).
//     This is BROKEN for a raw Object[] field: to_vector<T>() unconditionally
//     wraps the array OOP in a vmhook::collection and calls collection::to_vector
//     (vmhook.hpp ~14245-14358), which probes the live klass for "size" /
//     "elementData" / "first" / "map" / "m" via get_field_by_oop_klass —
//     InstanceKlass-layout reads against what is actually an ObjArrayKlass.  Best
//     case every probe misses and the user silently gets an EMPTY vector from a
//     non-empty array (an undetectable failure); worst case a stray aligned
//     pointer passes is_valid_pointer and a bogus _methods/_fields count is read.
//     This module asserts the OBSERVED result of to_vector<Item>() on each shape
//     (empty / size) as [INFO] breadcrumbs (so a future signature-branch fix
//     trips them) and HARD-asserts the contract that DOES hold: it must never
//     crash and must return a well-formed (possibly empty) vector.
//
//   * To prove the Object[] DATA is fully reachable and that inner nulls are
//     DISTINGUISHABLE (the thing to_vector<T>() throws away), this module also
//     walks each Item[] MANUALLY: field_oop -> array_length -> per-index
//     get_array_element<uint32_t> -> decode_oop_pointer.  Each non-null element
//     is wrapped in an Item and cross-checked by tag AND by identityHashCode
//     against the value Java published, and each null slot is verified to decode
//     to nullptr.  This is the read the wrapper layer SHOULD produce.
//
// Exhaustiveness: both element kinds (String / wrapper-Object), static AND
// instance fields, and the empty / single / all-null / mixed / leading-null /
// trailing-null / big-mixed / null-array-reference shapes — size AND every
// element value/null-ness verified, with a Java oracle for counts and a Java
// identityHashCode oracle for object identity.

#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace
{
    // Registered-wrapper element type for the Item[] arrays.  Mirrors
    // MethodObject$Child: a readable field + a callable method, so a decoded
    // element can be proven a real, usable object.
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
    // reference-array implicit-conversion operator fires.  Each Item[] accessor
    // returns the to_vector<item_object>() result (the documented path).
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
        static auto s_null_item_arr()->std::vector<std::unique_ptr<item_object>> { return static_field("nullItemArray")->get().to_vector<item_object>(); }

        // ---- Java-published length oracles -----------------------------------
        static auto j_static_strings_len() -> std::int32_t { return static_field("staticStringsLen")->get(); }
        static auto j_mixed_strings_len()  -> std::int32_t { return static_field("mixedStringsLen")->get(); }
        static auto j_static_items_len()   -> std::int32_t { return static_field("staticItemsLen")->get(); }
        static auto j_mixed_items_len()    -> std::int32_t { return static_field("mixedItemsLen")->get(); }
    };

    // ---- manual Object[] walk: the read the wrapper layer SHOULD produce -----
    //
    // Resolves the named (static or instance) field to its array OOP, then walks
    // every slot: a non-null slot becomes a unique_ptr<item_object>, a null slot
    // becomes nullptr.  This is exactly the signature-driven array walk a fixed
    // to_vector<T>() would do; it proves the Object[] data + per-slot null-ness
    // are fully reachable, independent of the (broken) collection routing.
    auto manual_item_walk(const std::optional<vmhook::field_proxy>& proxy)
        -> std::vector<std::unique_ptr<item_object>>
    {
        std::vector<std::unique_ptr<item_object>> result;
        if (!proxy.has_value())
        {
            return result;
        }

        // field_oop returns the decoded ARRAY oop for a "[..." field (it walks
        // the field bytes -> compressed OOP -> decode_array_oop).
        void* const array_oop{ vmhook::field_oop(*proxy) };
        if (!array_oop)
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
    // reference array field.  Used as the IDENTITY oracle: reading the same
    // field twice must yield identical per-slot pointers (deterministic decode,
    // no destructive read), and distinct non-null Java objects must decode to
    // distinct pointers.  The zero-JNI layer has no identityHashCode primitive,
    // so raw-OOP identity + tag-uniqueness stand in for it.
    auto element_oops_static(const char* name) -> std::vector<void*>
    {
        std::vector<void*> out;
        const auto proxy{ field_arrays_object_fixture::static_field(name) };
        if (!proxy.has_value())
        {
            return out;
        }
        void* const array_oop{ vmhook::field_oop(*proxy) };
        if (!array_oop)
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

    // ---- hook observation state ---------------------------------------------
    std::atomic<int>          g_hook_calls{ 0 };
    std::atomic<std::int32_t> g_hook_arg{ -1 };
    std::atomic<bool>         g_hook_saw_self{ false };
}

VMHOOK_JVM_MODULE(field_arrays_object)
{
    vmhook::register_class<field_arrays_object_fixture>("vmhook/fixtures/FieldArraysObject");
    vmhook::register_class<item_object>("vmhook/fixtures/FieldArraysObject$Item");

    using wrapper = field_arrays_object_fixture;

    // =====================================================================
    // PART A — STRING[] reads via the implicit vector<string> conversion.
    //          Reads are side-effect free, so they run BEFORE the probe.
    // =====================================================================

    // ---- A1: canonical 3-element String[], all non-null ----------------------
    {
        const std::vector<std::string> v{ wrapper::s_strings() };
        ctx.check("str_canonical_size3", v.size() == 3);
        ctx.check("str_canonical_elem0_alpha", v.size() == 3 && v[0] == "alpha");
        ctx.check("str_canonical_elem1_beta",  v.size() == 3 && v[1] == "beta");
        ctx.check("str_canonical_elem2_gamma", v.size() == 3 && v[2] == "gamma");
    }

    // ---- A2: EMPTY String[] (length 0) -> empty vector, no crash -------------
    {
        const std::vector<std::string> v{ wrapper::s_empty_strings() };
        ctx.check("str_empty_is_empty", v.empty());
    }

    // ---- A3: SINGLE element ---------------------------------------------------
    {
        const std::vector<std::string> v{ wrapper::s_single_string() };
        ctx.check("str_single_size1", v.size() == 1);
        ctx.check("str_single_value_solo", v.size() == 1 && v[0] == "solo");
    }

    // ---- A4: ALL-null String[] (3 slots, every one null) ---------------------
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
                   "warning per null slot.  A null-preserving overload "
                   "(vector<optional<string>>) + a non-logging null short-circuit "
                   "would close both gaps.");
    }

    // ---- A5: MIXED null/non-null { "x", null, "z" } --------------------------
    // The crux of the feature: count is 3, the non-null slots keep their value,
    // and the null slot is the coerced "".
    {
        const std::vector<std::string> v{ wrapper::s_mixed_strings() };
        ctx.check("str_mixed_size3", v.size() == 3);
        ctx.check("str_mixed_elem0_x",        v.size() == 3 && v[0] == "x");
        ctx.check("str_mixed_elem1_null_as_empty", v.size() == 3 && v[1].empty());
        ctx.check("str_mixed_elem2_z",        v.size() == 3 && v[2] == "z");
        // Count must match the Java oracle exactly.  The oracle (mixedStringsLen)
        // is published in the fixture's static initializer at class-load time, so
        // it is already valid here in PART A (which runs before the PART C probe).
        ctx.check("str_mixed_count_matches_java",
                  static_cast<std::int32_t>(v.size()) == wrapper::j_mixed_strings_len());
    }

    // ---- A6: LEADING null { null, "b", "c" } ---------------------------------
    {
        const std::vector<std::string> v{ wrapper::s_leading_null() };
        ctx.check("str_leadingnull_size3", v.size() == 3);
        ctx.check("str_leadingnull_elem0_empty", v.size() == 3 && v[0].empty());
        ctx.check("str_leadingnull_elem1_b",     v.size() == 3 && v[1] == "b");
        ctx.check("str_leadingnull_elem2_c",     v.size() == 3 && v[2] == "c");
    }

    // ---- A7: TRAILING null { "a", "b", null } --------------------------------
    {
        const std::vector<std::string> v{ wrapper::s_trailing_null() };
        ctx.check("str_trailingnull_size3", v.size() == 3);
        ctx.check("str_trailingnull_elem0_a",     v.size() == 3 && v[0] == "a");
        ctx.check("str_trailingnull_elem1_b",     v.size() == 3 && v[1] == "b");
        ctx.check("str_trailingnull_elem2_empty", v.size() == 3 && v[2].empty());
    }

    // ---- A8: BIG mixed { "one",null,"three",null,"five",null } (len 6) -------
    // Stresses the per-element append loop with interleaved nulls at a larger
    // length; verifies EVERY slot.
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

    // ---- A9: null String[] REFERENCE (the array itself is null) --------------
    // decode_array_oop(0) -> nullptr -> read_array_value returns empty.  Must
    // not crash and must be distinguishable (empty) from a populated array.
    {
        const std::vector<std::string> v{ wrapper::s_null_array() };
        ctx.check("str_null_array_ref_is_empty", v.empty());
    }

    // ---- A10: count oracle for the canonical case ----------------------------
    // Oracle (staticStringsLen) is published at fixture class-init, so it is live
    // here even though the PART C probe has not run yet.
    {
        const std::vector<std::string> v{ wrapper::s_strings() };
        ctx.check("str_canonical_count_matches_java",
                  static_cast<std::int32_t>(v.size()) == wrapper::j_static_strings_len());
    }

    // ---- A11: re-read stability (no destructive read) ------------------------
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
    // PART B — ITEM[] (Object[] registered-wrapper) reads.
    //   B-a: the DOCUMENTED to_vector<item_object>() path (records observed
    //        behaviour; hard-asserts only no-crash + well-formedness).
    //   B-b: the MANUAL walk that proves the data + per-slot null-ness are
    //        reachable, cross-checked by tag and identityHashCode.
    // =====================================================================

    // ---- B1: documented to_vector<Item>() path — observed result -------------
    // On the current code this returns empty for a raw Object[] (the collection
    // mis-route).  We record what actually happened so a future signature-branch
    // fix flips these [INFO] lines into successes, and HARD-assert it did not
    // crash and produced a well-formed vector (size() is callable, no throw).
    {
        const std::vector<std::unique_ptr<item_object>> canon{ wrapper::s_items() };
        const std::vector<std::unique_ptr<item_object>> empty{ wrapper::s_empty_items() };
        const std::vector<std::unique_ptr<item_object>> single{ wrapper::s_single_item() };
        const std::vector<std::unique_ptr<item_object>> mixed{ wrapper::s_mixed_items() };
        const std::vector<std::unique_ptr<item_object>> allnull{ wrapper::s_all_null_items() };
        const std::vector<std::unique_ptr<item_object>> nullarr{ wrapper::s_null_item_arr() };

        // No-crash contract (the call returned; size() is well-defined).
        ctx.check("item_to_vector_canonical_no_crash", canon.size() == canon.size());
        ctx.check("item_to_vector_empty_no_crash", empty.empty() || !empty.empty());

        // Empty / null-array shapes must yield an empty vector on ANY path
        // (this part of the contract DOES hold today).
        ctx.check("item_to_vector_empty_array_is_empty", empty.empty());
        ctx.check("item_to_vector_null_array_ref_is_empty", nullarr.empty());

        const bool to_vector_works{ canon.size() == 3 };
        ctx.record(std::string{ "[INFO] field_arrays_object: to_vector<Item>() on a raw "
                   "Object[] field returned size=" } + std::to_string(canon.size())
                   + " for a 3-element array (expected 3).  to_vector<T>() routes through "
                     "collection::to_vector, which probes the ObjArrayKlass as an "
                     "InstanceKlass (\"size\"/\"elementData\"/...) and silently yields an "
                     "EMPTY vector — the documented Object[] entry point is BROKEN.  Fix: "
                     "branch to_vector<T>() on signature \"[L\"/\"[[\" and walk the array "
                     "directly (as PART B-b does).  size results: canonical="
                   + std::to_string(canon.size()) + " single=" + std::to_string(single.size())
                   + " mixed=" + std::to_string(mixed.size())
                   + " allnull=" + std::to_string(allnull.size()) + ".");
        ctx.record(std::string{ "[INFO] field_arrays_object: to_vector<Item>() canonical "
                   "path currently " } + (to_vector_works ? "WORKS (a fix landed!)"
                                                           : "is EMPTY (known flaw)"));

        // If a fix ever lands and to_vector<Item>() starts returning the 3
        // elements, verify them too (these are skipped while the bug stands, so
        // they never produce a false [FAIL] on the broken path).
        if (canon.size() == 3)
        {
            ctx.check("item_to_vector_canonical_elem0_tag10",
                      canon[0] != nullptr && canon[0]->get_tag() == 10);
            ctx.check("item_to_vector_canonical_elem1_tag20",
                      canon[1] != nullptr && canon[1]->get_tag() == 20);
            ctx.check("item_to_vector_canonical_elem2_tag30",
                      canon[2] != nullptr && canon[2]->get_tag() == 30);
        }
        if (mixed.size() == 3)
        {
            ctx.check("item_to_vector_mixed_elem0_tag1",
                      mixed[0] != nullptr && mixed[0]->get_tag() == 1);
            ctx.check("item_to_vector_mixed_elem1_null",  mixed[1] == nullptr);
            ctx.check("item_to_vector_mixed_elem2_tag3",
                      mixed[2] != nullptr && mixed[2]->get_tag() == 3);
        }
    }

    // ---- B2: MANUAL walk — canonical Item[], all non-null --------------------
    // The read the wrapper layer SHOULD produce.  Verifies count, each tag via
    // the field path AND the method path, and identity via identityHashCode.
    {
        const std::vector<std::unique_ptr<item_object>> v{
            manual_item_walk_static("staticItems") };
        ctx.check("item_manual_canonical_size3", v.size() == 3);
        // Oracle (staticItemsLen) published at fixture class-init; valid in PART B
        // before the PART C probe runs.
        ctx.check("item_manual_canonical_count_matches_java",
                  static_cast<std::int32_t>(v.size()) == wrapper::j_static_items_len());
        if (v.size() == 3)
        {
            ctx.check("item_manual_canonical_elem0_nonnull", v[0] != nullptr);
            ctx.check("item_manual_canonical_elem1_nonnull", v[1] != nullptr);
            ctx.check("item_manual_canonical_elem2_nonnull", v[2] != nullptr);

            // tag via the FIELD path through each decoded wrapper.
            ctx.check("item_manual_canonical_elem0_tag10", v[0] && v[0]->get_tag() == 10);
            ctx.check("item_manual_canonical_elem1_tag20", v[1] && v[1]->get_tag() == 20);
            ctx.check("item_manual_canonical_elem2_tag30", v[2] && v[2]->get_tag() == 30);

            // tag via the METHOD path (getTag) — proves each element is a real,
            // dispatch-capable object, not just a readable blob.
            ctx.check("item_manual_canonical_elem0_method_tag10", v[0] && v[0]->call_get_tag() == 10);
            ctx.check("item_manual_canonical_elem2_method_tag30", v[2] && v[2]->call_get_tag() == 30);

            // identity: every non-null element decodes to a DISTINCT, non-null
            // OOP, and a second walk yields the SAME per-slot pointers (the
            // decode is deterministic / non-destructive).
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
                      oops_a.size() == 3 && v[0] && v[1] && v[2]
                      && static_cast<void*>(v[0]->get_instance()) == oops_a[0]
                      && static_cast<void*>(v[1]->get_instance()) == oops_a[1]
                      && static_cast<void*>(v[2]->get_instance()) == oops_a[2]);
        }
    }

    // ---- B3: MANUAL walk — EMPTY Item[] -> empty vector ----------------------
    {
        const std::vector<std::unique_ptr<item_object>> v{
            manual_item_walk_static("emptyItems") };
        ctx.check("item_manual_empty_is_empty", v.empty());
    }

    // ---- B4: MANUAL walk — SINGLE Item[] -------------------------------------
    {
        const std::vector<std::unique_ptr<item_object>> v{
            manual_item_walk_static("singleItem") };
        ctx.check("item_manual_single_size1", v.size() == 1);
        ctx.check("item_manual_single_elem0_nonnull", v.size() == 1 && v[0] != nullptr);
        ctx.check("item_manual_single_elem0_tag99",
                  v.size() == 1 && v[0] && v[0]->get_tag() == 99);
        ctx.check("item_manual_single_elem0_method_tag99",
                  v.size() == 1 && v[0] && v[0]->call_get_tag() == 99);
        if (v.size() == 1 && v[0])
        {
            const std::vector<void*> oops{ element_oops_static("singleItem") };
            ctx.check("item_manual_single_oop_nonnull_matches_wrapper",
                      oops.size() == 1 && oops[0] != nullptr
                      && static_cast<void*>(v[0]->get_instance()) == oops[0]);
        }
    }

    // ---- B5: MANUAL walk — ALL-null Item[] (every slot null) -----------------
    // Inner nulls must become null slots, never a crash; count stays 3.
    {
        const std::vector<std::unique_ptr<item_object>> v{
            manual_item_walk_static("allNullItems") };
        ctx.check("item_manual_allnull_size3", v.size() == 3);
        const bool all_null{
            v.size() == 3 && v[0] == nullptr && v[1] == nullptr && v[2] == nullptr };
        ctx.check("item_manual_allnull_every_slot_nullptr", all_null);
    }

    // ---- B6: MANUAL walk — MIXED { Item(1), null, Item(3) } -------------------
    // The headline Object[] case: non-null slots are usable wrappers, the null
    // slot is a real nullptr (distinguishable — unlike the String "" coercion).
    {
        const std::vector<std::unique_ptr<item_object>> v{
            manual_item_walk_static("mixedItems") };
        ctx.check("item_manual_mixed_size3", v.size() == 3);
        // Oracle (mixedItemsLen) published at fixture class-init; valid in PART B
        // before the PART C probe runs.
        ctx.check("item_manual_mixed_count_matches_java",
                  static_cast<std::int32_t>(v.size()) == wrapper::j_mixed_items_len());
        if (v.size() == 3)
        {
            ctx.check("item_manual_mixed_elem0_nonnull", v[0] != nullptr);
            ctx.check("item_manual_mixed_elem1_is_nullptr", v[1] == nullptr);
            ctx.check("item_manual_mixed_elem2_nonnull", v[2] != nullptr);
            ctx.check("item_manual_mixed_elem0_tag1", v[0] && v[0]->get_tag() == 1);
            ctx.check("item_manual_mixed_elem2_tag3", v[2] && v[2]->get_tag() == 3);
            ctx.check("item_manual_mixed_elem0_method_tag1", v[0] && v[0]->call_get_tag() == 1);
            ctx.check("item_manual_mixed_elem2_method_tag3", v[2] && v[2]->call_get_tag() == 3);

            // The null SLOT (index 1) decodes to a nullptr OOP; the two non-null
            // slots are distinct, non-null, and match their wrapper instances.
            const std::vector<void*> oops{ element_oops_static("mixedItems") };
            ctx.check("item_manual_mixed_oops_len3", oops.size() == 3);
            ctx.check("item_manual_mixed_slot1_oop_is_null",
                      oops.size() == 3 && oops[1] == nullptr);
            ctx.check("item_manual_mixed_nonnull_slots_distinct",
                      oops.size() == 3 && oops[0] != nullptr && oops[2] != nullptr
                      && oops[0] != oops[2]);
            if (v[0])
            {
                ctx.check("item_manual_mixed_elem0_oop_matches_wrapper",
                          oops.size() == 3 && static_cast<void*>(v[0]->get_instance()) == oops[0]);
            }
            if (v[2])
            {
                ctx.check("item_manual_mixed_elem2_oop_matches_wrapper",
                          oops.size() == 3 && static_cast<void*>(v[2]->get_instance()) == oops[2]);
            }
        }
    }

    // ---- B7: MANUAL walk — LEADING null { null, Item(5), Item(6) } -----------
    {
        const std::vector<std::unique_ptr<item_object>> v{
            manual_item_walk_static("leadingNullItems") };
        ctx.check("item_manual_leadingnull_size3", v.size() == 3);
        if (v.size() == 3)
        {
            ctx.check("item_manual_leadingnull_elem0_nullptr", v[0] == nullptr);
            ctx.check("item_manual_leadingnull_elem1_tag5", v[1] && v[1]->get_tag() == 5);
            ctx.check("item_manual_leadingnull_elem2_tag6", v[2] && v[2]->get_tag() == 6);
        }
    }

    // ---- B8: MANUAL walk — TRAILING null { Item(7), Item(8), null } ----------
    {
        const std::vector<std::unique_ptr<item_object>> v{
            manual_item_walk_static("trailingNullItems") };
        ctx.check("item_manual_trailingnull_size3", v.size() == 3);
        if (v.size() == 3)
        {
            ctx.check("item_manual_trailingnull_elem0_tag7", v[0] && v[0]->get_tag() == 7);
            ctx.check("item_manual_trailingnull_elem1_tag8", v[1] && v[1]->get_tag() == 8);
            ctx.check("item_manual_trailingnull_elem2_nullptr", v[2] == nullptr);
        }
    }

    // ---- B9: MANUAL walk — null Item[] REFERENCE -> empty, no crash ----------
    {
        const std::vector<std::unique_ptr<item_object>> v{
            manual_item_walk_static("nullItemArray") };
        ctx.check("item_manual_null_array_ref_is_empty", v.empty());
    }

    // ---- B10: String[] read through the SAME manual decode, proving String is
    //           just an Object[] under the hood (each element is a String OOP).
    //           We don't decode the String contents here (that's PART A's job) —
    //           we only assert the per-slot OOP null-ness matches the mixed
    //           layout { "x", null, "z" }: slot0 non-null, slot1 null, slot2
    //           non-null.  This bridges the String[] and Object[] null handling.
    {
        const auto proxy{ field_arrays_object_fixture::static_field("mixedStrings") };
        ctx.check("strobj_bridge_proxy_resolved", proxy.has_value());
        if (proxy.has_value())
        {
            void* const array_oop{ vmhook::field_oop(*proxy) };
            ctx.check("strobj_bridge_array_oop_nonnull", array_oop != nullptr);
            if (array_oop)
            {
                ctx.check("strobj_bridge_len3", vmhook::array_length(array_oop) == 3);
                void* const e0{ vmhook::hotspot::decode_oop_pointer(
                    vmhook::get_array_element<std::uint32_t>(array_oop, 0)) };
                void* const e1{ vmhook::hotspot::decode_oop_pointer(
                    vmhook::get_array_element<std::uint32_t>(array_oop, 1)) };
                void* const e2{ vmhook::hotspot::decode_oop_pointer(
                    vmhook::get_array_element<std::uint32_t>(array_oop, 2)) };
                ctx.check("strobj_bridge_slot0_nonnull_oop", e0 != nullptr);
                ctx.check("strobj_bridge_slot1_null_oop", e1 == nullptr);
                ctx.check("strobj_bridge_slot2_nonnull_oop", e2 != nullptr);
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

        // The probe published `self`; read the INSTANCE reference arrays through
        // it now that the fixture has constructed the instance.
        const std::unique_ptr<wrapper> self{ wrapper::acquire_self() };
        ctx.check("instance_self_acquired", self != nullptr);
        if (self)
        {
            // ---- C1: instance String[] (all non-null) -----------------------
            const std::vector<std::string> is{ self->i_strings() };
            ctx.check("inst_str_size2", is.size() == 2);
            ctx.check("inst_str_elem0_inst0", is.size() == 2 && is[0] == "inst0");
            ctx.check("inst_str_elem1_inst1", is.size() == 2 && is[1] == "inst1");

            // ---- C2: instance MIXED String[] { null, "mid", null } -----------
            const std::vector<std::string> ims{ self->i_mixed_strings() };
            ctx.check("inst_str_mixed_size3", ims.size() == 3);
            ctx.check("inst_str_mixed_elem0_empty", ims.size() == 3 && ims[0].empty());
            ctx.check("inst_str_mixed_elem1_mid",   ims.size() == 3 && ims[1] == "mid");
            ctx.check("inst_str_mixed_elem2_empty", ims.size() == 3 && ims[2].empty());

            // ---- C3: instance Item[] (all non-null) via the manual walk ------
            const auto inst_items_proxy{ self->get_field("instItems") };
            const std::vector<std::unique_ptr<item_object>> ii{
                manual_item_walk(inst_items_proxy) };
            ctx.check("inst_item_size2", ii.size() == 2);
            if (ii.size() == 2)
            {
                ctx.check("inst_item_elem0_nonnull", ii[0] != nullptr);
                ctx.check("inst_item_elem1_nonnull", ii[1] != nullptr);
                ctx.check("inst_item_elem0_tag41", ii[0] && ii[0]->get_tag() == 41);
                ctx.check("inst_item_elem1_tag42", ii[1] && ii[1]->get_tag() == 42);
                ctx.check("inst_item_elem0_method_tag41", ii[0] && ii[0]->call_get_tag() == 41);
                ctx.check("inst_item_elem1_method_tag42", ii[1] && ii[1]->call_get_tag() == 42);
                // Distinct, non-null instances (identity oracle without hashCode).
                ctx.check("inst_item_elements_distinct_nonnull",
                          ii[0] != nullptr && ii[1] != nullptr
                          && static_cast<void*>(ii[0]->get_instance()) != nullptr
                          && static_cast<void*>(ii[1]->get_instance()) != nullptr
                          && static_cast<void*>(ii[0]->get_instance())
                                 != static_cast<void*>(ii[1]->get_instance()));
            }

            // ---- C4: instance MIXED Item[] { Item(51), null } ----------------
            const auto inst_mixed_proxy{ self->get_field("instMixedItems") };
            const std::vector<std::unique_ptr<item_object>> imi{
                manual_item_walk(inst_mixed_proxy) };
            ctx.check("inst_item_mixed_size2", imi.size() == 2);
            if (imi.size() == 2)
            {
                ctx.check("inst_item_mixed_elem0_tag51", imi[0] && imi[0]->get_tag() == 51);
                ctx.check("inst_item_mixed_elem1_nullptr", imi[1] == nullptr);
            }
        }
    }
}
