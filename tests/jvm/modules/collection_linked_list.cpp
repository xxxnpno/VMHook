// collection_linked_list JVM test module  (feature area: collections)
//
// THE LinkedList Node-chain authority.  Successor to the legacy
// example.cpp test_linked_list_probe: it proves, on a live JVM, that vmhook's
// LinkedList wrapper reads a java.util.LinkedList by walking the
// `first -> next` Node chain (linked_list_walk_items) — the path that is
// O(N) in the chain length — rather than the generic List.get(int) fallback
// that collection::to_vector keeps as a last resort (and which is O(N^2) on a
// LinkedList because each get(int) re-walks half the chain).
//
// The fixture (vmhook.fixtures.LinkedListProbe) publishes ONE
// LinkedList<String> with exactly three known elements in a known insertion
// order ("alpha","bravo","charlie").  This module reaches it through the
// published SINGLETON and reads it three independent ways:
//
//   (1) field_proxy::value_t::to_vector<elem>()       — the documented user
//       path: get_field("words")->get().to_vector<elem>();
//   (2) std::unique_ptr<vmhook::linked_list> ll = get_field("words")->get();
//       ll->to_vector<elem>()                          — the typed-wrapper path
//       the scope names explicitly (get<unique_ptr<linked_list>> then walk);
//   (3) vmhook::linked_list_walk_items<elem>(oop, size, out) called DIRECTLY
//       on the LinkedList OOP — an INDEPENDENT reproduction of the Node-chain
//       walk, so the size/order/content proof does not rely solely on the
//       dispatch inside collection::to_vector picking the right branch.
//
// And it PROVES the Node-chain branch (not the get(int) fallback) is the one
// collection::to_vector selects, by checking the exact field-shape predicate
// the cascade uses on the live OOP's klass:
//     "first" resolves  AND  "size" resolves   -> LinkedList branch taken
//     "elementData" does NOT resolve            -> ArrayList branch skipped
// (these are read through a tiny test-only subclass of vmhook::collection that
// surfaces the otherwise-protected get_field_by_oop_klass).
//
// Every element-oop dereference is gated with vmhook::hotspot::is_valid_pointer
// and string content is read with vmhook::read_java_string (which itself
// re-validates), so a null / wild slot can never fault this module.  All
// value_t / proxy extractions are COPY-INIT (never brace-init) to stay
// MSVC-unambiguous, exactly as the field_static module documents.
//
// No hooks are installed by this module (the fixture's own trigger() detour
// fires via the harness probe), so there is nothing to shutdown_hooks().
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace
{
    // Element wrapper for the LinkedList's String elements.  to_vector<E>() and
    // linked_list_walk_items<E>() require E to be constructible from
    // vmhook::oop_t; a String's content is then read with
    // vmhook::read_java_string(get_instance()), which needs NO klass
    // registration for the element type (it resolves java/lang/String itself).
    class str_elem : public vmhook::object<str_elem>
    {
    public:
        explicit str_elem(vmhook::oop_t instance) noexcept
            : vmhook::object<str_elem>{ instance }
        {
        }

        // Decode this element's String content.  Gated internally by
        // read_java_string's own is_valid_pointer check; returns "" for a null
        // or invalid backing.
        auto content() const -> std::string
        {
            return vmhook::read_java_string(this->get_instance());
        }
    };

    // Host wrapper for vmhook.fixtures.LinkedListProbe.  Registered so
    // get_field("words") can resolve the field offset off the live SINGLETON.
    class llp : public vmhook::object<llp>
    {
    public:
        explicit llp(vmhook::oop_t instance) noexcept
            : vmhook::object<llp>{ instance }
        {
        }

        // ---- handshake (static fields via the portable static accessor) ----
        static auto set_go(bool value) -> void  { static_field("go")->set(value); }
        static auto set_done(bool value) -> void { static_field("done")->set(value); }
        static auto get_done() -> bool           { return static_field("done")->get(); }
        static auto get_observed_size() -> std::int32_t
        {
            const std::int32_t v = static_field("observedSize")->get();
            return v;
        }

        // ---- Java-observed sizes of the additional LinkedList shapes ----
        static auto get_observed_empty_size() -> std::int32_t
        {
            const std::int32_t v = static_field("observedEmptySize")->get();
            return v;
        }
        static auto get_observed_single_size() -> std::int32_t
        {
            const std::int32_t v = static_field("observedSingleSize")->get();
            return v;
        }
        static auto get_observed_null_size() -> std::int32_t
        {
            const std::int32_t v = static_field("observedNullSize")->get();
            return v;
        }
        static auto get_observed_dup_size() -> std::int32_t
        {
            const std::int32_t v = static_field("observedDupSize")->get();
            return v;
        }
        static auto get_observed_empty_str_size() -> std::int32_t
        {
            const std::int32_t v = static_field("observedEmptyStrSize")->get();
            return v;
        }
        static auto get_observed_many_size() -> std::int32_t
        {
            const std::int32_t v = static_field("observedManySize")->get();
            return v;
        }
        static auto get_many_size_const() -> std::int32_t
        {
            const std::int32_t v = static_field("MANY_SIZE")->get();
            return v;
        }

        // ---- acquire the published SINGLETON instance wrapper ----
        static auto singleton() -> std::unique_ptr<llp> { return static_field("SINGLETON")->get(); }

        // ---- path (1): the documented value_t::to_vector user path ----
        auto words_via_value_to_vector() const -> std::vector<std::unique_ptr<str_elem>>
        {
            const auto proxy{ this->get_field("words") };
            if (!proxy.has_value())
            {
                return {};
            }
            return proxy->get().to_vector<str_elem>();
        }

        // ---- path (2): get<unique_ptr<linked_list>> then ll->to_vector ----
        // Reads the 'L...;' field as a vmhook::linked_list wrapper (copy-init
        // from value_t), then walks it.  The Node-chain fast path lives in the
        // inherited collection::to_vector, so a linked_list-typed wrapper runs
        // the first->next walk just as the value_t path does.
        auto words_via_linked_list_wrapper() const -> std::vector<std::unique_ptr<str_elem>>
        {
            const auto proxy{ this->get_field("words") };
            if (!proxy.has_value())
            {
                return {};
            }
            std::unique_ptr<vmhook::linked_list> ll = proxy->get();   // copy-init
            if (!ll)
            {
                return {};
            }
            return ll->to_vector<str_elem>();
        }

        // ---- the raw decoded OOP of the `words` LinkedList (for path 3 +
        //      the field-shape predicate proof) ----
        auto words_oop() const -> void*
        {
            return this->field_oop("words");
        }

        // ---- the raw decoded OOP of ANY LinkedList field on this instance ----
        // Generalises words_oop() so the additional shapes (emptyList,
        // singleList, nullList, dupList, emptyStrList, manyList) can each be
        // walked the same three ways.  Returns nullptr on a missing field or an
        // invalid decode (an EMPTY LinkedList still decodes to a valid list OOP —
        // it is the list object that is non-null, only its `first` is null).
        auto field_oop(const char* name) const -> void*
        {
            const auto proxy{ this->get_field(name) };
            if (!proxy.has_value())
            {
                return nullptr;
            }
            const std::uint32_t compressed{ static_cast<std::uint32_t>(proxy->get()) };
            void* const oop{ vmhook::hotspot::decode_oop_pointer(compressed) };
            if (!oop || !vmhook::hotspot::is_valid_pointer(oop))
            {
                return nullptr;
            }
            return oop;
        }

        // ---- path (1) for an arbitrary LinkedList field ----
        auto via_value_to_vector(const char* name) const
            -> std::vector<std::unique_ptr<str_elem>>
        {
            const auto proxy{ this->get_field(name) };
            if (!proxy.has_value())
            {
                return {};
            }
            return proxy->get().to_vector<str_elem>();
        }

        // ---- path (2) for an arbitrary LinkedList field ----
        auto via_linked_list_wrapper(const char* name) const
            -> std::vector<std::unique_ptr<str_elem>>
        {
            const auto proxy{ this->get_field(name) };
            if (!proxy.has_value())
            {
                return {};
            }
            std::unique_ptr<vmhook::linked_list> ll = proxy->get();   // copy-init
            if (!ll)
            {
                return {};
            }
            return ll->to_vector<str_elem>();
        }
    };

    // Test-only subclass of vmhook::collection that surfaces the protected
    // live-OOP field probe.  This lets the module assert, on the live
    // LinkedList klass, the EXACT field-shape predicate collection::to_vector
    // uses to pick the Node-chain branch over the ArrayList branch and the
    // get(int) fallback.
    class probe_collection : public vmhook::collection
    {
    public:
        explicit probe_collection(vmhook::oop_t oop) noexcept
            : vmhook::collection{ oop }
        {
        }

        auto has_oop_field(const char* name) const -> bool
        {
            return this->get_field_by_oop_klass(name).has_value();
        }
    };

    // Validate that a to_vector result is the expected 3 known elements in
    // insertion order, with every element a non-null, pointer-valid wrapper.
    // Records each sub-assertion under a path-tagged name so a failure pinpoints
    // which of the three read paths regressed.
    auto check_three_words(vmhook_test::context& ctx,
                           const char* path,
                           const std::vector<std::unique_ptr<str_elem>>& vec) -> void
    {
        const std::string tag{ path };

        ctx.check(tag + "_size_is_3", vec.size() == 3u);
        if (vec.size() != 3u)
        {
            return;   // order/content checks below assume exactly 3 slots
        }

        // Every element must be a non-null wrapper over a pointer-valid OOP.
        bool all_valid{ true };
        for (const auto& e : vec)
        {
            if (!e || !vmhook::hotspot::is_valid_pointer(e->get_instance()))
            {
                all_valid = false;
            }
        }
        ctx.check(tag + "_all_elements_valid_nonnull", all_valid);
        if (!all_valid)
        {
            return;   // do not deref a null/invalid element for content
        }

        // Content + insertion order: index k holds WORD k.
        ctx.check(tag + "_elem0_is_alpha",   vec[0]->content() == "alpha");
        ctx.check(tag + "_elem1_is_bravo",   vec[1]->content() == "bravo");
        ctx.check(tag + "_elem2_is_charlie", vec[2]->content() == "charlie");

        // Order is strict and distinct (guards against a stable-but-wrong
        // permutation passing the per-index checks by coincidence).
        ctx.check(tag + "_order_strictly_a_b_c",
                  vec[0]->content() == "alpha"
                  && vec[1]->content() == "bravo"
                  && vec[2]->content() == "charlie"
                  && vec[0]->content() != vec[1]->content()
                  && vec[1]->content() != vec[2]->content());
    }

    // Decode a slot's content, treating a null/invalid wrapper as the sentinel
    // "<null>" so a null Node.item slot is distinguishable from a real "" string
    // (read_java_string returns "" for BOTH a null backing AND an empty Java
    // string — flaw #6 — so we must branch on the WRAPPER being null, not on the
    // decoded content, to tell a null element from an empty one).
    auto slot_text(const std::unique_ptr<str_elem>& e) -> std::string
    {
        if (!e || !vmhook::hotspot::is_valid_pointer(e->get_instance()))
        {
            return std::string{ "<null>" };
        }
        return e->content();
    }

    // Assert a to_vector result is exactly `expected` (in order), where each
    // expected entry is either a real string or the "<null>" sentinel for a null
    // Node.item slot.  Path-tagged so a failure pinpoints which read path / shape
    // regressed.  Returns true iff the size matched (so callers can gate further
    // per-shape assertions).
    auto check_exact(vmhook_test::context& ctx,
                     const char* path,
                     const std::vector<std::unique_ptr<str_elem>>& vec,
                     const std::vector<std::string>& expected) -> bool
    {
        const std::string tag{ path };
        const bool size_ok{ vec.size() == expected.size() };
        ctx.check(tag + "_size_matches", size_ok);
        if (!size_ok)
        {
            return false;
        }
        bool all_match{ true };
        for (std::size_t i{ 0 }; i < vec.size(); ++i)
        {
            if (slot_text(vec[i]) != expected[i])
            {
                all_match = false;
            }
        }
        ctx.check(tag + "_elements_match_in_order", all_match);
        return true;
    }

    // Run a shape through ALL THREE read paths (value_t::to_vector, the typed
    // linked_list wrapper, and a direct linked_list_walk_items on the list OOP),
    // asserting each path yields exactly `expected`.  `walk_size` is the size
    // handed to the DIRECT walk — normally the real element count.  This is the
    // workhorse that proves the Node-chain walk handles every shape identically
    // across the three documented entry points.
    auto check_shape_all_paths(vmhook_test::context& ctx,
                               llp& inst,
                               const char* field,
                               const std::string& tagbase,
                               const std::vector<std::string>& expected,
                               const std::int32_t walk_size) -> void
    {
        check_exact(ctx, (tagbase + "_value").c_str(),
                    inst.via_value_to_vector(field), expected);
        check_exact(ctx, (tagbase + "_wrapper").c_str(),
                    inst.via_linked_list_wrapper(field), expected);

        void* const oop{ inst.field_oop(field) };
        ctx.check(tagbase + "_field_decodes_to_valid_oop", oop != nullptr);
        if (oop)
        {
            std::vector<std::unique_ptr<str_elem>> direct;
            vmhook::linked_list_walk_items<str_elem>(oop, walk_size, direct);
            check_exact(ctx, (tagbase + "_direct").c_str(), direct, expected);
        }
    }
}

VMHOOK_JVM_MODULE(collection_linked_list)
{
    vmhook::register_class<llp>("vmhook/fixtures/LinkedListProbe");

    // =====================================================================
    //  0. Sanity: the host class resolves through the portable accessor.
    // =====================================================================
    ctx.check("llp_class_registered_static_field_resolves",
              llp::static_field("SINGLETON").has_value());

    // =====================================================================
    //  1. Drive one probe cycle: populate the LinkedList, republish the
    //     Java-observed size, and fire trigger() so a real JavaThread runs
    //     the fixture (parity with how the legacy probe and every other
    //     module coordinate with Java).  The LinkedList READS below happen on
    //     the injector thread afterwards — the Node-chain walk is pure memory
    //     reads (no Java method dispatch), exactly as the legacy
    //     test_linked_list_probe did.
    // =====================================================================
    const bool probe_done{ ctx.run_probe(
        [](bool value)
        {
            if (value)
            {
                llp::set_done(false);
            }
            llp::set_go(value);
        },
        []() { return llp::get_done(); }) };

    ctx.check("linked_list_probe_completed", probe_done);
    ctx.check("java_observed_size_is_3", llp::get_observed_size() == 3);

    // =====================================================================
    //  2. Acquire the published SINGLETON.
    // =====================================================================
    const auto inst{ llp::singleton() };
    ctx.check("singleton_acquired", inst != nullptr);
    ctx.check("singleton_oop_valid",
              inst != nullptr && vmhook::hotspot::is_valid_pointer(inst->get_instance()));
    if (!inst || !vmhook::hotspot::is_valid_pointer(inst->get_instance()))
    {
        ctx.record("[INFO] collection_linked_list: SINGLETON not acquired — "
                   "remaining LinkedList checks skipped.");
        return;
    }

    // The `words` field must resolve and decode to a pointer-valid LinkedList.
    void* const list_oop{ inst->words_oop() };
    ctx.check("words_field_decodes_to_valid_oop", list_oop != nullptr);
    if (!list_oop)
    {
        ctx.record("[INFO] collection_linked_list: `words` field did not decode "
                   "to a valid OOP — remaining checks skipped.");
        return;
    }

    // =====================================================================
    //  3. PROVE the Node-chain branch is the one collection::to_vector picks.
    //     The cascade's selector is field-presence on the LIVE OOP's klass:
    //       ArrayList branch  needs "elementData" + "size"
    //       LinkedList branch needs "first"       + "size"  (checked next)
    //       ... else generic List.get(int) fallback
    //     A java.util.LinkedList has "first"+"size" and NO "elementData", so
    //     the LinkedList branch is selected and the get(int) O(N^2) fallback is
    //     never reached.  We assert that exact shape here.
    // =====================================================================
    {
        const probe_collection pc{ list_oop };
        const bool has_first{ pc.has_oop_field("first") };
        const bool has_size{ pc.has_oop_field("size") };
        const bool has_element_data{ pc.has_oop_field("elementData") };

        ctx.check("live_klass_has_first_field", has_first);
        ctx.check("live_klass_has_size_field", has_size);
        ctx.check("live_klass_has_no_elementData_field", !has_element_data);

        // The composite predicate that uniquely selects the LinkedList
        // Node-chain branch (first+size present, elementData absent).
        const bool node_walk_selected{ has_first && has_size && !has_element_data };
        ctx.check("linkedlist_node_chain_branch_selected", node_walk_selected);

        if (node_walk_selected)
        {
            ctx.record("[INFO] collection_linked_list: live `words` klass exposes "
                       "first+size and NO elementData -> collection::to_vector takes "
                       "the dedicated LinkedList first->next Node-chain walk "
                       "(linked_list_walk_items), NOT the generic List.get(int) "
                       "O(N^2) fallback.  vmhook routes LinkedList through the "
                       "dedicated node-walk path (expected).");
        }
        else
        {
            // Characterize, per the brief, if vmhook ever routed LinkedList
            // through the generic path instead.  Values are still asserted by
            // paths (1)/(2)/(3) below regardless of which branch ran.
            ctx.record("[INFO] collection_linked_list: live `words` klass did NOT "
                       "match the first+size / no-elementData LinkedList shape — "
                       "to_vector would fall through to a generic path.  Element "
                       "values are still validated below either way.");
        }
    }

    // =====================================================================
    //  4. PATH (1): the documented value_t::to_vector user path.
    // =====================================================================
    check_three_words(ctx, "value_to_vector", inst->words_via_value_to_vector());

    // =====================================================================
    //  5. PATH (2): get<unique_ptr<linked_list>> then ll->to_vector.
    //     This is the wrapper-typed path the scope names explicitly.
    // =====================================================================
    {
        // First prove the field decodes into a USABLE linked_list wrapper
        // (non-null) before walking it — the get<unique_ptr<linked_list>> step.
        const auto proxy{ inst->get_field("words") };
        ctx.check("words_field_resolves", proxy.has_value());
        if (proxy.has_value())
        {
            std::unique_ptr<vmhook::linked_list> ll = proxy->get();   // copy-init
            ctx.check("words_decodes_to_linked_list_wrapper", ll != nullptr);
            ctx.check("linked_list_wrapper_oop_matches_field",
                      ll != nullptr && ll->get_instance() == list_oop);
        }
    }
    check_three_words(ctx, "linked_list_wrapper", inst->words_via_linked_list_wrapper());

    // =====================================================================
    //  6. PATH (3): linked_list_walk_items called DIRECTLY on the LinkedList
    //     OOP — an independent reproduction of the Node-chain walk.  This is
    //     the strongest single proof that the first->next traversal itself
    //     yields exactly the 3 known elements in order, decoupled from the
    //     branch-selection logic inside collection::to_vector.
    // =====================================================================
    {
        std::vector<std::unique_ptr<str_elem>> direct;
        vmhook::linked_list_walk_items<str_elem>(list_oop, 3, direct);
        check_three_words(ctx, "direct_node_walk", direct);

        // Cross-path agreement: the direct Node walk and the documented
        // value_t path must produce the same count (both walked the same
        // chain).  Proven on content above; this pins the sizes together.
        const auto via_value{ inst->words_via_value_to_vector() };
        ctx.check("direct_walk_and_value_path_same_size",
                  direct.size() == via_value.size() && direct.size() == 3u);
    }

    // =====================================================================
    //  7. Cross-check the native size against the Java-observed size.
    // =====================================================================
    {
        const auto vec{ inst->words_via_value_to_vector() };
        ctx.check("native_size_matches_java_observed_size",
                  static_cast<std::int32_t>(vec.size()) == llp::get_observed_size());
    }

    // =====================================================================
    //  8. EMPTY LinkedList shape (size 0, first == null).
    //     The Java side reports size 0 and `first` is null, so the cascade's
    //     LinkedList branch takes the `n > 0` guard FALSE and returns an empty
    //     vector without walking; a direct linked_list_walk_items with size 0
    //     also no-ops.  Proves the empty shape reads back as 0 elements on every
    //     path (NOT a spurious element, NOT a fault).  Note flaw #3: an empty
    //     list and a decode failure both yield {} — we additionally assert the
    //     list OOP itself is a valid (non-null) object, distinguishing "empty
    //     collection" from "could not read the field" here.
    // =====================================================================
    {
        void* const empty_oop{ inst->field_oop("emptyList") };
        ctx.check("emptyList_field_decodes_to_valid_oop", empty_oop != nullptr);

        const std::vector<std::string> expected_empty{};
        check_exact(ctx, "empty_value",
                    inst->via_value_to_vector("emptyList"), expected_empty);
        check_exact(ctx, "empty_wrapper",
                    inst->via_linked_list_wrapper("emptyList"), expected_empty);

        // Direct walk with the REAL size (0): must yield nothing and not fault.
        if (empty_oop)
        {
            std::vector<std::unique_ptr<str_elem>> direct0;
            vmhook::linked_list_walk_items<str_elem>(empty_oop, 0, direct0);
            ctx.check("empty_direct_size0_yields_empty", direct0.empty());

            // Degenerate: a POSITIVE size on a list whose `first` is null.  The
            // walk's loop guard is `i < size AND is_valid_pointer(node_oop)`;
            // node_oop is decoded from a null `first`, so the FIRST iteration's
            // pointer guard is false and the walk yields 0 elements regardless of
            // the (wrong) size.  This is the size-says-more-than-the-chain-holds
            // case for an empty chain — it must NOT fabricate elements.
            std::vector<std::unique_ptr<str_elem>> direct_overclaim;
            vmhook::linked_list_walk_items<str_elem>(empty_oop, 5, direct_overclaim);
            ctx.check("empty_direct_oversize_still_empty", direct_overclaim.empty());
        }

        ctx.check("empty_java_observed_size_is_0", llp::get_observed_empty_size() == 0);
    }

    // =====================================================================
    //  9. SINGLE-element LinkedList shape (first -> "solo" -> null).
    //     The minimal non-empty chain: exactly one Node, one item, next == null.
    // =====================================================================
    {
        const std::vector<std::string> expected_single{ std::string{ "solo" } };
        check_shape_all_paths(ctx, *inst, "singleList", std::string{ "single" },
                              expected_single, 1);
        ctx.check("single_java_observed_size_is_1", llp::get_observed_single_size() == 1);
    }

    // =====================================================================
    //  10. NULL-bearing LinkedList shape (null, "mid", null, null).
    //      Every position is a real Node; the Node.item REFERENCE is null at
    //      indices 0/2/3.  The walk must (a) advance first->next across a Node
    //      whose item is null without desyncing, and (b) preserve each null item
    //      as a nullptr wrapper while keeping "mid" at index 1.  This is the
    //      core null-element-preservation proof for the chain walk.
    // =====================================================================
    {
        const std::vector<std::string> expected_null{
            std::string{ "<null>" }, std::string{ "mid" },
            std::string{ "<null>" }, std::string{ "<null>" } };
        check_shape_all_paths(ctx, *inst, "nullList", std::string{ "nulls" },
                              expected_null, 4);

        // Explicit: the non-null slot sits at index 1 and only there.
        const auto nv{ inst->via_value_to_vector("nullList") };
        ctx.check("null_shape_size_is_4", nv.size() == 4u);
        if (nv.size() == 4u)
        {
            const bool slot0_null{ !nv[0] || !vmhook::hotspot::is_valid_pointer(nv[0]->get_instance()) };
            const bool slot1_live{ nv[1] && vmhook::hotspot::is_valid_pointer(nv[1]->get_instance()) };
            const bool slot2_null{ !nv[2] || !vmhook::hotspot::is_valid_pointer(nv[2]->get_instance()) };
            const bool slot3_null{ !nv[3] || !vmhook::hotspot::is_valid_pointer(nv[3]->get_instance()) };
            ctx.check("null_shape_slot0_is_null", slot0_null);
            ctx.check("null_shape_slot1_is_live_mid",
                      slot1_live && nv[1]->content() == "mid");
            ctx.check("null_shape_slot2_is_null", slot2_null);
            ctx.check("null_shape_slot3_is_null", slot3_null);
        }
        ctx.check("null_java_observed_size_is_4", llp::get_observed_null_size() == 4);
    }

    // =====================================================================
    //  11. DUPLICATE-element LinkedList shape ("dup","dup","dup").
    //      A List keeps every occurrence (unlike a Set, which would collapse
    //      them to one).  The walk must return all three identical elements;
    //      this guards against any accidental de-duplication and proves the
    //      chain length — not the distinct-value count — drives the result.
    // =====================================================================
    {
        const std::vector<std::string> expected_dup{
            std::string{ "dup" }, std::string{ "dup" }, std::string{ "dup" } };
        check_shape_all_paths(ctx, *inst, "dupList", std::string{ "dupes" },
                              expected_dup, 3);
        ctx.check("dup_java_observed_size_is_3", llp::get_observed_dup_size() == 3);
    }

    // =====================================================================
    //  12. EMPTY-STRING element shape ("" , "tail").
    //      A legitimately empty Java String element must read back as a NON-null,
    //      pointer-valid wrapper whose content() is exactly "" — distinct from a
    //      null Node.item slot (which yields a null wrapper).  This exercises the
    //      element-content boundary flaw #6 notes (read_java_string returns "" for
    //      both empty and null backing); here we assert the WRAPPER is non-null so
    //      the empty-string element is correctly NOT a null slot.
    // =====================================================================
    {
        const std::vector<std::string> expected_estr{
            std::string{}, std::string{ "tail" } };
        // check_exact's slot_text returns the real "" for a live empty-string
        // wrapper (it only substitutes "<null>" when the WRAPPER is null), so the
        // empty string compares equal to expected[0] == "".
        check_shape_all_paths(ctx, *inst, "emptyStrList", std::string{ "estr" },
                              expected_estr, 2);

        const auto ev{ inst->via_value_to_vector("emptyStrList") };
        ctx.check("emptyStr_shape_size_is_2", ev.size() == 2u);
        if (ev.size() == 2u)
        {
            // Index 0 is an EMPTY STRING element: the wrapper must be live
            // (non-null, pointer-valid) and its content exactly "".
            const bool elem0_live{ ev[0] && vmhook::hotspot::is_valid_pointer(ev[0]->get_instance()) };
            ctx.check("emptyStr_elem0_wrapper_is_nonnull", elem0_live);
            ctx.check("emptyStr_elem0_content_is_empty",
                      elem0_live && ev[0]->content().empty());
            ctx.check("emptyStr_elem1_is_tail",
                      ev[1] && vmhook::hotspot::is_valid_pointer(ev[1]->get_instance())
                      && ev[1]->content() == "tail");
        }
        ctx.check("emptyStr_java_observed_size_is_2", llp::get_observed_empty_str_size() == 2);
    }

    // =====================================================================
    //  13. MANY-element LinkedList shape (16 decimal-string elements 0..15).
    //      A longer chain than `words` so the first->next traversal is exercised
    //      over many links; proves order is preserved across the whole chain and
    //      that every element decodes to its own distinct String.  Heap stays
    //      modest (16 short ASCII strings).
    // =====================================================================
    {
        const std::int32_t many_n{ llp::get_observed_many_size() };
        const std::int32_t many_const{ llp::get_many_size_const() };
        ctx.check("many_java_observed_size_is_16", many_n == 16);
        ctx.check("many_const_matches_observed", many_const == many_n);

        // Build the expected sequence "0","1",...,"15" from the live MANY_SIZE
        // const so the assertion tracks the fixture rather than a magic literal.
        std::vector<std::string> expected_many;
        if (many_const > 0 && many_const <= 4096)
        {
            expected_many.reserve(static_cast<std::size_t>(many_const));
            for (std::int32_t k{ 0 }; k < many_const; ++k)
            {
                expected_many.push_back(std::to_string(k));
            }
        }
        check_shape_all_paths(ctx, *inst, "manyList", std::string{ "many" },
                              expected_many, many_const);
    }

    // =====================================================================
    //  14. NODE-CHAIN-WALK vs SIZE: the direct walk's `size` argument is an
    //      upper BOUND, not the source of truth (the chain's null terminator is).
    //      On the `words` list (real length 3) we probe the asymmetry flaw #2
    //      documents, all crash-safe (the loop is bounded by BOTH i < size AND a
    //      live is_valid_pointer(node_oop)):
    //        * size LARGER than the chain -> the null `next` terminator stops the
    //          walk at the true end; result is the real 3 (NOT padded with nulls).
    //        * size SMALLER than the chain -> the walk is truncated to `size`
    //          (the documented truncate-low behaviour); result is the first 2.
    //        * size 0 -> the size<=0 guard returns immediately; 0 elements.
    // =====================================================================
    {
        // size LARGER than the 3-element chain: terminator wins, exactly 3.
        std::vector<std::unique_ptr<str_elem>> over;
        vmhook::linked_list_walk_items<str_elem>(list_oop, 9, over);
        ctx.check("words_walk_oversize_stops_at_terminator", over.size() == 3u);
        if (over.size() == 3u)
        {
            ctx.check("words_walk_oversize_content_intact",
                      slot_text(over[0]) == "alpha"
                      && slot_text(over[1]) == "bravo"
                      && slot_text(over[2]) == "charlie");
        }

        // size SMALLER than the chain: truncated to exactly `size` elements.
        std::vector<std::unique_ptr<str_elem>> under;
        vmhook::linked_list_walk_items<str_elem>(list_oop, 2, under);
        ctx.check("words_walk_undersize_truncates_to_2", under.size() == 2u);
        if (under.size() == 2u)
        {
            ctx.check("words_walk_undersize_keeps_prefix_order",
                      slot_text(under[0]) == "alpha"
                      && slot_text(under[1]) == "bravo");
        }

        // size 0: immediate guard, no walk, no fault.
        std::vector<std::unique_ptr<str_elem>> zero;
        vmhook::linked_list_walk_items<str_elem>(list_oop, 0, zero);
        ctx.check("words_walk_size0_yields_empty", zero.empty());

        // negative size: same guard (size <= 0), no fault.
        std::vector<std::unique_ptr<str_elem>> neg;
        vmhook::linked_list_walk_items<str_elem>(list_oop, -3, neg);
        ctx.check("words_walk_negative_size_yields_empty", neg.empty());
    }

    // =====================================================================
    //  15. CRASH-SAFETY: linked_list_walk_items on a null / invalid list OOP.
    //      The size<=0 / null / !is_valid_pointer guards must return cleanly
    //      with an empty vector and no fault on every degenerate input.
    // =====================================================================
    {
        std::vector<std::unique_ptr<str_elem>> on_null;
        vmhook::linked_list_walk_items<str_elem>(nullptr, 3, on_null);
        ctx.check("walk_on_null_oop_yields_empty", on_null.empty());

        // A clearly-bogus, non-null pointer: is_valid_pointer must reject it.
        std::vector<std::unique_ptr<str_elem>> on_bogus;
        vmhook::linked_list_walk_items<str_elem>(
            reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x1)), 3, on_bogus);
        ctx.check("walk_on_bogus_oop_yields_empty", on_bogus.empty());
    }

    // =====================================================================
    //  16. Every distinct shape is a java.util.LinkedList -> the SAME field-shape
    //      predicate (first+size present, elementData absent) selects the
    //      Node-chain branch for ALL of them, not just `words`.  Asserted on the
    //      LIVE klass of each list OOP via the probe_collection subclass.  (All
    //      these lists share one LinkedList klass, but reading the predicate off
    //      each live OOP independently proves the routing is per-OOP and uniform.)
    // =====================================================================
    {
        const char* const shape_fields[]{
            "emptyList", "singleList", "nullList", "dupList", "emptyStrList", "manyList" };
        for (const char* const field : shape_fields)
        {
            void* const oop{ inst->field_oop(field) };
            const std::string tag{ field };
            ctx.check(tag + "_shape_field_decodes", oop != nullptr);
            if (!oop)
            {
                continue;
            }
            const probe_collection pc{ oop };
            const bool predicate{
                pc.has_oop_field("first")
                && pc.has_oop_field("size")
                && !pc.has_oop_field("elementData") };
            ctx.check(tag + "_selects_node_chain_branch", predicate);
        }
    }

    // No hooks were installed by this module, so there is nothing to tear down.
}
