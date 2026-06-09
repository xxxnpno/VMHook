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
            const auto proxy{ this->get_field("words") };
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

    // No hooks were installed by this module, so there is nothing to tear down.
}
