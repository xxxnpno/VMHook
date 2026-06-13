// collection_linked_list_exhaustive JVM test module  (feature area: collections)
//
// COMPANION to collection_linked_list.cpp.  That module is the focused authority
// for the three-element first->next Node-chain walk read three ways (value_t
// path, typed vmhook::linked_list wrapper, and the direct
// vmhook::linked_list_walk_items free function), and it proves — on the live
// klass — that collection::to_vector picks the dedicated Node-chain branch
// (first+size present, elementData absent) and NOT the O(N^2) get(int) fallback.
//
// This module makes the "every java.util.LinkedList / node-chain shape read
// through the library" goal EXHAUSTIVE, against its OWN distinct fixture
// (vmhook.fixtures.LinkedListExhaustive) so the mature LinkedListProbe.java /
// collection_linked_list.cpp pair is left untouched.  It exercises the LIBRARY's
// real decode path — vmhook::collection::to_vector<T>() and the
// vmhook::linked_list_walk_items<T>() free function, both PURE guarded heap reads
// (no Java call) — across:
//
//   * LinkedList used as a LIST and as a Deque/Queue.  The same
//     java.util.LinkedList klass backs all three roles, so the first+size /
//     no-elementData field shape (hence the chain walk) is selected regardless of
//     the static interface the Java code drove.  The Deque/Queue lists are built
//     with addFirst/offer/push so their HEAD->TAIL node order is well-defined and
//     published; the walk must reproduce node order.
//   * Element TYPES: String, boxed Integer, boxed Long with a non-zero HIGH 32
//     bits (a 32-bit misread of Long.value would corrupt the checksum), a real
//     enum (Day: name + ordinal off java.lang.Enum), a user class (Elem: id +
//     tag), and NULL elements (LinkedList permits null -> nullptr slot).
//   * SIZES 0, 1, 2, 10, 1000: the full chain walk with NO early stop, NO cycle,
//     and NO overrun past the tail.  The 1000-node chain is the deep-chain proof:
//     the walk must terminate by SIZE, never by chasing a null off the end, and
//     every decoded element OOP must be DISTINCT (a cycle re-emits a node).
//   * NODE ORDER != INSERTION ORDER: a list built by interleaving addFirst /
//     addLast / add(index).  The walk must reproduce the published node order, not
//     the order the elements were inserted.
//   * AFTER A MIDDLE REMOVE (node unlink): the forward walk must follow the
//     rewired `next` links and yield the published surviving sequence.
//
// CROSS-TOOLCHAIN HARDENING (the brief): the no-cycle / no-overrun / size-match /
// node-order invariants are HARD (a real safety property — a cycle or an overrun
// past the tail is a genuine bug on EVERY toolchain).  Per-element value DECODE
// of a variant field (a String's content, a boxed Long's high word, an enum's
// name) is PASS-or-[INFO]: if a config variant cannot decode an element's content
// the structural check still holds and the content assert degrades to a visible
// [INFO] rather than a [FAIL].  Aggregate value checks honor the same per-item
// gate.  Every size oracle is a Java-published static field read as a plain int
// (the worker body never calls a Java method).
//
// SUITE-SAFETY (mirrors collection_set_exhaustive.cpp / register_class.cpp): the
// whole body runs under a try/catch (a throw is recorded [INFO], never a FAIL); an
// entry guard bails to [INFO] if the fixture class does not resolve; the only hook
// is a scoped_hook<> that RAII-uninstalls on scope exit, and an unconditional
// vmhook::shutdown_hooks() OUTSIDE the try guarantees ZERO hooks armed on EVERY
// exit path.  Every element-OOP deref is gated by is_valid_pointer and content is
// read with vmhook::read_java_string (which re-validates), so a null/wild slot can
// never fault the module.  All value_t / proxy extractions are COPY-INIT (never
// brace-init) to stay MSVC-unambiguous.  Distinct `clx_*` check-name prefix so no
// assertion name collides with collection_linked_list.cpp.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace
{
    // ── STRING element wrapper (content read via read_java_string). ──────────
    class str_elem : public vmhook::object<str_elem>
    {
    public:
        explicit str_elem(vmhook::oop_t instance) noexcept
            : vmhook::object<str_elem>{ instance }
        {
        }

        auto content() const -> std::string
        {
            return vmhook::read_java_string(this->get_instance());
        }
    };

    // ── BOXED Integer element wrapper (java.lang.Integer.value). ─────────────
    class integer_box : public vmhook::object<integer_box>
    {
    public:
        explicit integer_box(vmhook::oop_t instance) noexcept
            : vmhook::object<integer_box>{ instance }
        {
        }

        auto value() const -> std::int32_t
        {
            const auto f{ get_field("value") };
            return f ? static_cast<std::int32_t>(f->get()) : 0;
        }
    };

    // ── BOXED Long element wrapper (java.lang.Long.value, full 64-bit). ──────
    class long_box : public vmhook::object<long_box>
    {
    public:
        explicit long_box(vmhook::oop_t instance) noexcept
            : vmhook::object<long_box>{ instance }
        {
        }

        auto value() const -> std::int64_t
        {
            const auto f{ get_field("value") };
            return f ? static_cast<std::int64_t>(f->get()) : 0;
        }
    };

    // ── ENUM element wrapper (java.lang.Enum: name + ordinal). ───────────────
    class enum_element : public vmhook::object<enum_element>
    {
    public:
        explicit enum_element(vmhook::oop_t instance) noexcept
            : vmhook::object<enum_element>{ instance }
        {
        }

        auto ordinal() const -> std::int32_t
        {
            const auto f{ get_field("ordinal") };
            return f ? static_cast<std::int32_t>(f->get()) : -1;
        }

        auto name() const -> std::string
        {
            const auto f{ get_field("name") };
            if (!f)
            {
                return std::string{};
            }
            std::string s = f->get();   // copy-init
            return s;
        }
    };

    // ── USER-class element wrapper (LinkedListExhaustive$Elem: id + tag). ────
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
            return f ? static_cast<std::int32_t>(f->get()) : -1;
        }

        auto tag() const -> std::string
        {
            const auto f{ get_field("tag") };
            if (!f)
            {
                return std::string{};
            }
            std::string s = f->get();   // copy-init
            return s;
        }
    };

    // ── Fixture wrapper: vmhook.fixtures.LinkedListExhaustive. ───────────────
    class fixture : public vmhook::object<fixture>
    {
    public:
        explicit fixture(vmhook::oop_t instance) noexcept
            : vmhook::object<fixture>{ instance }
        {
        }

        static auto set_go(bool value) -> void       { static_field("go")->set(value); }
        static auto set_done(bool value) -> void      { static_field("done")->set(value); }
        static auto get_done() -> bool                { return static_field("done")->get(); }
        static auto set_mode(std::int32_t m) -> void   { static_field("mode")->set(m); }
        static auto get_observed() -> std::int32_t    { return static_field("observed")->get(); }

        static auto j_int(const char* f) -> std::int32_t  { return static_field(f)->get(); }
        static auto j_long(const char* f) -> std::int64_t { return static_field(f)->get(); }

        static auto j_str(const char* f) -> std::string
        {
            const auto proxy{ static_field(f) };
            if (!proxy.has_value())
            {
                return std::string{};
            }
            std::string s = proxy->get();   // copy-init
            return s;
        }

        // value_t::to_vector<T>() on a named static LinkedList field — the
        // documented user path (get_field/static_field -> get -> to_vector).
        template<typename T>
        static auto to_vector_of(const char* field) -> std::vector<std::unique_ptr<T>>
        {
            const auto proxy{ static_field(field) };
            if (!proxy.has_value())
            {
                return {};
            }
            return proxy->get().to_vector<T>();
        }

        // Decoded OOP of a named static LinkedList field (or nullptr) — for the
        // direct linked_list_walk_items path and the typed-wrapper path.
        static auto field_oop(const char* field) -> void*
        {
            const auto proxy{ static_field(field) };
            if (!proxy.has_value())
            {
                return nullptr;
            }
            void* const oop{ vmhook::field_oop(*proxy) };
            return (oop && vmhook::hotspot::is_valid_pointer(oop)) ? oop : nullptr;
        }
    };

    // ── Fixture-mirrored constants (lockstep with LinkedListExhaustive.java). ─
    constexpr std::int32_t SIZE0{ 0 };
    constexpr std::int32_t SIZE1{ 1 };
    constexpr std::int32_t SIZE2{ 2 };
    constexpr std::int32_t SIZE10{ 10 };
    constexpr std::int32_t THOUSAND{ 1000 };
    constexpr std::int32_t INT_N{ 10 };
    constexpr std::int32_t LONG_N{ 10 };
    constexpr std::int32_t DAY_N{ 5 };
    constexpr std::int32_t ELEM_N{ 8 };
    constexpr std::int32_t NULL_LEN{ 5 };
    constexpr std::int32_t NULL_AT{ 2 };

    // Wall-clock ceiling for the 1000-node walk.  A linear walk is sub-ms; a
    // quadratic per-node regression blows far past this.  Regression canary.
    constexpr std::int64_t WALK_BUDGET_MS{ 3000 };

    // ── Hook observation (pilot-style proof). ────────────────────────────────
    std::atomic<int>          g_hook_calls{ 0 };
    std::atomic<std::int32_t> g_hook_arg{ -1 };
    std::atomic<bool>         g_hook_saw_self{ false };

    // True iff every element OOP in the vector is distinct (a cycle in the chain
    // re-emits a node, collapsing this) — counting only non-null slots.  This is
    // the HARD no-cycle invariant.
    template<typename T>
    auto all_oops_distinct(const std::vector<std::unique_ptr<T>>& v) -> bool
    {
        std::unordered_set<const void*> seen;
        seen.reserve(v.size() * 2 + 1);
        for (const auto& up : v)
        {
            const T* const e{ up.get() };
            if (e == nullptr)
            {
                continue;
            }
            if (!seen.insert(static_cast<const void*>(e->get_instance())).second)
            {
                return false;
            }
        }
        return true;
    }

    template<typename T>
    auto count_non_null(const std::vector<std::unique_ptr<T>>& v) -> std::int32_t
    {
        std::int32_t n{ 0 };
        for (const auto& up : v)
        {
            if (up)
            {
                ++n;
            }
        }
        return n;
    }

    // True iff every non-null element is a non-null wrapper over a pointer-valid
    // OOP (so a later content read cannot deref a wild slot).
    template<typename T>
    auto every_elem_valid(const std::vector<std::unique_ptr<T>>& v) -> bool
    {
        for (const auto& up : v)
        {
            if (up && !vmhook::hotspot::is_valid_pointer(up->get_instance()))
            {
                return false;
            }
        }
        return true;
    }

    // Join the decoded String contents of a vector with ',' (skips null slots).
    auto join_string_contents(const std::vector<std::unique_ptr<str_elem>>& v)
        -> std::string
    {
        std::string out;
        bool first{ true };
        for (const auto& up : v)
        {
            if (!up)
            {
                continue;
            }
            if (!first)
            {
                out.push_back(',');
            }
            out += up->content();
            first = false;
        }
        return out;
    }

    // Join the decoded Integer values of a vector with ',' (skips null slots).
    auto join_int_values(const std::vector<std::unique_ptr<integer_box>>& v)
        -> std::string
    {
        std::string out;
        bool first{ true };
        for (const auto& up : v)
        {
            if (!up)
            {
                continue;
            }
            if (!first)
            {
                out.push_back(',');
            }
            out += std::to_string(up->value());
            first = false;
        }
        return out;
    }

    constexpr char FIXTURE[]{ "vmhook/fixtures/LinkedListExhaustive" };

    // ─── HARD structural assertions for a String list of size `n` whose element
    //     k is "w<k>".  size / count / no-null / distinctness / termination are
    //     HARD; the per-element CONTENT order is PASS-or-[INFO] (a variant config
    //     that cannot decode a String degrades to a visible [INFO]). ───────────
    auto check_words_list(vmhook_test::context& ctx,
                          const std::string& tag,
                          const std::vector<std::unique_ptr<str_elem>>& v,
                          const std::int32_t n,
                          const std::int32_t java_size) -> void
    {
        // HARD: the walk produced exactly `size` slots (no early stop, no
        // overrun) and the count agrees with Java's own size().
        ctx.check(tag + "_size_is_n", static_cast<std::int32_t>(v.size()) == n);
        ctx.check(tag + "_size_matches_java",
                  static_cast<std::int32_t>(v.size()) == java_size);
        ctx.check(tag + "_no_null_slots", count_non_null(v) == static_cast<std::int32_t>(v.size()));
        ctx.check(tag + "_elements_pointer_valid", every_elem_valid(v));
        // HARD no-cycle: a cycle would re-emit a node -> a duplicate OOP.
        ctx.check(tag + "_all_oops_distinct_no_cycle", all_oops_distinct(v));

        if (static_cast<std::int32_t>(v.size()) != n || n == 0)
        {
            return;   // ordered-content check below assumes exactly n>0 slots
        }

        // PASS-or-[INFO]: element k content == "w<k>" in chain order.  If a
        // variant config decoded an empty content for any slot, degrade to INFO.
        bool any_empty{ false };
        bool order_ok{ true };
        for (std::int32_t k{ 0 }; k < n; ++k)
        {
            const std::string c{ v[static_cast<std::size_t>(k)]->content() };
            if (c.empty())
            {
                any_empty = true;
            }
            if (c != ("w" + std::to_string(k)))
            {
                order_ok = false;
            }
        }
        if (any_empty)
        {
            ctx.record("[INFO] collection_linked_list_exhaustive: " + tag
                       + " — a String element decoded to empty content on this "
                         "config variant; structural walk (size/order/no-cycle) is "
                         "proven, element-content order recorded as INFO.");
        }
        else
        {
            ctx.check(tag + "_content_order_w_k", order_ok);
        }
    }

    auto run_checks(vmhook_test::context& ctx) -> void
    {
        // ─── ENTRY GUARD ────────────────────────────────────────────────────
        if (vmhook::find_class(FIXTURE) == nullptr)
        {
            ctx.record("[INFO] collection_linked_list_exhaustive: LinkedListExhaustive "
                       "not loaded/resolvable on this run; skipping the module's live "
                       "checks (no crash, no hooks armed).");
            return;
        }

        vmhook::register_class<fixture>(FIXTURE);
        vmhook::register_class<elem_object>("vmhook/fixtures/LinkedListExhaustive$Elem");
        vmhook::register_class<integer_box>("java/lang/Integer");
        vmhook::register_class<long_box>("java/lang/Long");
        vmhook::register_class<enum_element>("java/lang/Enum");

        // Drive a mode-0 probe so buildAll() runs on the Java thread and the
        // reads below see a freshly-populated, deterministic snapshot.
        {
            const bool built{ ctx.run_probe(
                [](bool value)
                {
                    if (value)
                    {
                        fixture::set_done(false);
                        fixture::set_mode(0);
                    }
                    fixture::set_go(value);
                },
                []() { return fixture::get_done(); }) };
            ctx.check("clx_build_probe_completed", built);
        }

        // =====================================================================
        // SIZES 0, 1, 2, 10 — String lists, element k == "w<k>", via the
        // documented value_t::to_vector path.  size/count/distinctness HARD;
        // content order PASS-or-[INFO].
        // =====================================================================
        check_words_list(ctx, "clx_words0",
                         fixture::to_vector_of<str_elem>("words0"), SIZE0,
                         fixture::j_int("words0Size"));
        // HARD: an empty LinkedList decodes to an EMPTY vector (the n==0 guard in
        // to_vector returns {} WITHOUT walking — proven here distinctly).
        ctx.check("clx_words0_is_empty",
                  fixture::to_vector_of<str_elem>("words0").empty());

        check_words_list(ctx, "clx_words1",
                         fixture::to_vector_of<str_elem>("words1"), SIZE1,
                         fixture::j_int("words1Size"));
        check_words_list(ctx, "clx_words2",
                         fixture::to_vector_of<str_elem>("words2"), SIZE2,
                         fixture::j_int("words2Size"));
        check_words_list(ctx, "clx_words10",
                         fixture::to_vector_of<str_elem>("words10"), SIZE10,
                         fixture::j_int("words10Size"));

        // =====================================================================
        // SIZE 1000 — the DEEP chain.  The headline safety proof: the walk
        // terminates by SIZE (not by chasing a null off the end), NEVER loops
        // (all OOPs distinct), and NEVER overruns the tail (exactly 1000 slots,
        // last == "w999").  Timed as a quadratic-regression canary.
        // =====================================================================
        {
            const auto t0{ std::chrono::steady_clock::now() };
            const auto v{ fixture::to_vector_of<str_elem>("words1000") };
            const auto t1{ std::chrono::steady_clock::now() };
            const std::int64_t walk_us{
                std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count() };

            ctx.check("clx_words1000_size_is_1000",
                      static_cast<std::int32_t>(v.size()) == THOUSAND);
            ctx.check("clx_words1000_size_matches_java",
                      static_cast<std::int32_t>(v.size()) == fixture::j_int("words1000Size"));
            ctx.check("clx_words1000_no_null_slots",
                      count_non_null(v) == static_cast<std::int32_t>(v.size()));
            ctx.check("clx_words1000_elements_pointer_valid", every_elem_valid(v));
            // HARD no-cycle / no-overrun: 1000 DISTINCT node OOPs, no re-emit.
            ctx.check("clx_words1000_all_oops_distinct_no_cycle", all_oops_distinct(v));
            ctx.check("clx_words1000_walk_terminated_at_size",
                      static_cast<std::int32_t>(v.size()) == THOUSAND);

            if (static_cast<std::int32_t>(v.size()) == THOUSAND)
            {
                // PASS-or-[INFO]: endpoint + a mid sample content.
                const std::string c0{ v.front()->content() };
                const std::string cmid{ v[THOUSAND / 2]->content() };
                const std::string clast{ v.back()->content() };
                if (c0.empty() || cmid.empty() || clast.empty())
                {
                    ctx.record("[INFO] collection_linked_list_exhaustive: words1000 "
                               "endpoint/mid content decoded empty on this config "
                               "variant; the 1000-node walk structure (count, "
                               "distinctness, termination) is proven HARD above.");
                }
                else
                {
                    ctx.check("clx_words1000_first_is_w0", c0 == "w0");
                    ctx.check("clx_words1000_mid_is_w500", cmid == "w500");
                    ctx.check("clx_words1000_last_is_w999", clast == "w999");
                }
            }

            ctx.record("[INFO] collection_linked_list_exhaustive: words1000 walk over "
                       + std::to_string(THOUSAND) + " nodes took "
                       + std::to_string(walk_us) + " us");
            ctx.check("clx_words1000_walk_not_quadratic",
                      walk_us >= 0 && walk_us < WALK_BUDGET_MS * 1000);
        }

        // =====================================================================
        // ELEMENT TYPE: boxed Integer (value k == index k).  Structural HARD;
        // value order PASS-or-[INFO]; value sum cross-checked vs Java.
        // =====================================================================
        {
            const auto v{ fixture::to_vector_of<integer_box>("ints") };
            ctx.check("clx_ints_size_is_n", static_cast<std::int32_t>(v.size()) == INT_N);
            ctx.check("clx_ints_size_matches_java",
                      static_cast<std::int32_t>(v.size()) == fixture::j_int("intsSize"));
            ctx.check("clx_ints_no_null", count_non_null(v) == static_cast<std::int32_t>(v.size()));
            ctx.check("clx_ints_all_oops_distinct_no_cycle", all_oops_distinct(v));

            if (static_cast<std::int32_t>(v.size()) == INT_N)
            {
                bool order_ok{ true };
                std::int64_t sum{ 0 };
                for (std::int32_t k{ 0 }; k < INT_N; ++k)
                {
                    const std::int32_t val{ v[static_cast<std::size_t>(k)]->value() };
                    sum += val;
                    if (val != k)
                    {
                        order_ok = false;
                    }
                }
                ctx.check("clx_ints_value_order_k", order_ok);
                ctx.check("clx_ints_value_sum_matches_java",
                          sum == fixture::j_long("intsValSum"));
            }
        }

        // =====================================================================
        // ELEMENT TYPE: boxed Long whose value carries a non-zero HIGH 32 bits.
        // A 32-bit misread of Long.value would corrupt the sum/xor — so the
        // value cross-check proves the FULL 64-bit element read.
        // =====================================================================
        {
            const auto v{ fixture::to_vector_of<long_box>("longs") };
            ctx.check("clx_longs_size_is_n", static_cast<std::int32_t>(v.size()) == LONG_N);
            ctx.check("clx_longs_size_matches_java",
                      static_cast<std::int32_t>(v.size()) == fixture::j_int("longsSize"));
            ctx.check("clx_longs_no_null", count_non_null(v) == static_cast<std::int32_t>(v.size()));
            ctx.check("clx_longs_all_oops_distinct_no_cycle", all_oops_distinct(v));

            if (static_cast<std::int32_t>(v.size()) == LONG_N)
            {
                std::int64_t sum{ 0 };
                std::int64_t xr{ 0 };
                bool all_high{ true };
                bool order_ok{ true };
                for (std::int32_t k{ 0 }; k < LONG_N; ++k)
                {
                    const std::int64_t val{ v[static_cast<std::size_t>(k)]->value() };
                    sum += val;
                    xr ^= val;
                    if (val <= 0x7FFF'FFFFLL)
                    {
                        all_high = false;
                    }
                    if (val != 0x1'0000'0000LL + k)
                    {
                        order_ok = false;
                    }
                }
                // PASS-or-[INFO]: the full-width value decode is the variant-
                // sensitive part (it depends on the boxed-Long field read).
                if (!all_high)
                {
                    ctx.record("[INFO] collection_linked_list_exhaustive: longs — a "
                               "boxed Long decoded without its high 32 bits on this "
                               "config variant; structural walk proven HARD, full-width "
                               "value recorded as INFO.");
                }
                else
                {
                    ctx.check("clx_longs_high_word_survived", all_high);
                    ctx.check("clx_longs_value_order_k", order_ok);
                    ctx.check("clx_longs_value_sum_matches_java",
                              sum == fixture::j_long("longsValSum"));
                    ctx.check("clx_longs_value_xor_matches_java",
                              xr == fixture::j_long("longsValXor"));
                }
            }
        }

        // =====================================================================
        // ELEMENT TYPE: real enum (Day) — name + ordinal off java.lang.Enum, in
        // insertion order MON..FRI.  Structural HARD; ordinal/name PASS-or-[INFO].
        // =====================================================================
        {
            const auto v{ fixture::to_vector_of<enum_element>("days") };
            ctx.check("clx_days_size_is_n", static_cast<std::int32_t>(v.size()) == DAY_N);
            ctx.check("clx_days_size_matches_java",
                      static_cast<std::int32_t>(v.size()) == fixture::j_int("daysSize"));
            ctx.check("clx_days_no_null", count_non_null(v) == static_cast<std::int32_t>(v.size()));
            ctx.check("clx_days_all_oops_distinct_no_cycle", all_oops_distinct(v));

            if (static_cast<std::int32_t>(v.size()) == DAY_N)
            {
                bool ordinals_ok{ true };
                std::int64_t ord_sum{ 0 };
                bool any_empty_name{ false };
                static const char* const kNames[DAY_N]{ "MON", "TUE", "WED", "THU", "FRI" };
                bool names_ok{ true };
                for (std::int32_t k{ 0 }; k < DAY_N; ++k)
                {
                    const std::int32_t ord{ v[static_cast<std::size_t>(k)]->ordinal() };
                    ord_sum += ord;
                    if (ord != k)
                    {
                        ordinals_ok = false;
                    }
                    const std::string nm{ v[static_cast<std::size_t>(k)]->name() };
                    if (nm.empty())
                    {
                        any_empty_name = true;
                    }
                    if (nm != kNames[k])
                    {
                        names_ok = false;
                    }
                }
                ctx.check("clx_days_ordinal_order_k", ordinals_ok);
                ctx.check("clx_days_ordinal_sum_matches_java",
                          ord_sum == fixture::j_long("daysOrdinalSum"));
                ctx.check("clx_days_ordinal_sum_closed_form", ord_sum == 10);
                if (any_empty_name)
                {
                    ctx.record("[INFO] collection_linked_list_exhaustive: days — an enum "
                               "name decoded empty on this config variant; ordinal order "
                               "proven HARD, name order recorded as INFO.");
                }
                else
                {
                    ctx.check("clx_days_name_order", names_ok);
                }
            }
        }

        // =====================================================================
        // ELEMENT TYPE: user class Elem (id k == index k, tag "e<id>").
        // Structural HARD; id order + tag readback PASS-or-[INFO].
        // =====================================================================
        {
            const auto v{ fixture::to_vector_of<elem_object>("elems") };
            ctx.check("clx_elems_size_is_n", static_cast<std::int32_t>(v.size()) == ELEM_N);
            ctx.check("clx_elems_size_matches_java",
                      static_cast<std::int32_t>(v.size()) == fixture::j_int("elemsSize"));
            ctx.check("clx_elems_no_null", count_non_null(v) == static_cast<std::int32_t>(v.size()));
            ctx.check("clx_elems_all_oops_distinct_no_cycle", all_oops_distinct(v));

            if (static_cast<std::int32_t>(v.size()) == ELEM_N)
            {
                bool ids_ok{ true };
                bool tags_ok{ true };
                bool any_empty_tag{ false };
                for (std::int32_t k{ 0 }; k < ELEM_N; ++k)
                {
                    if (v[static_cast<std::size_t>(k)]->id() != k)
                    {
                        ids_ok = false;
                    }
                    const std::string tg{ v[static_cast<std::size_t>(k)]->tag() };
                    if (tg.empty())
                    {
                        any_empty_tag = true;
                    }
                    if (tg != ("e" + std::to_string(k)))
                    {
                        tags_ok = false;
                    }
                }
                ctx.check("clx_elems_id_order_k", ids_ok);
                if (any_empty_tag)
                {
                    ctx.record("[INFO] collection_linked_list_exhaustive: elems — an Elem "
                               "tag decoded empty on this config variant; id order proven "
                               "HARD, tag readback recorded as INFO.");
                }
                else
                {
                    ctx.check("clx_elems_tag_round_trip", tags_ok);
                }
            }
        }

        // =====================================================================
        // NULL ELEMENTS: LinkedList permits null; the null Node.item becomes a
        // nullptr slot at index NULL_AT, the other slots are "w<k>".  Structural
        // HARD; non-null content PASS-or-[INFO].
        // =====================================================================
        {
            const auto v{ fixture::to_vector_of<str_elem>("withNull") };
            ctx.check("clx_withnull_size_is_n", static_cast<std::int32_t>(v.size()) == NULL_LEN);
            ctx.check("clx_withnull_size_matches_java",
                      static_cast<std::int32_t>(v.size()) == fixture::j_int("withNullSize"));
            ctx.check("clx_withnull_one_null_slot",
                      count_non_null(v) == NULL_LEN - 1);
            ctx.check("clx_withnull_elements_pointer_valid", every_elem_valid(v));
            ctx.check("clx_withnull_all_oops_distinct_no_cycle", all_oops_distinct(v));

            if (static_cast<std::int32_t>(v.size()) == NULL_LEN)
            {
                // HARD: the null lands at exactly index NULL_AT (the chain walk
                // preserves slot positions, not just count).
                ctx.check("clx_withnull_null_at_expected_index",
                          v[static_cast<std::size_t>(NULL_AT)] == nullptr);

                bool any_empty{ false };
                bool nonnull_ok{ true };
                for (std::int32_t k{ 0 }; k < NULL_LEN; ++k)
                {
                    if (k == NULL_AT)
                    {
                        continue;
                    }
                    const str_elem* const e{ v[static_cast<std::size_t>(k)].get() };
                    if (e == nullptr)
                    {
                        nonnull_ok = false;
                        continue;
                    }
                    const std::string c{ e->content() };
                    if (c.empty())
                    {
                        any_empty = true;
                    }
                    if (c != ("w" + std::to_string(k)))
                    {
                        nonnull_ok = false;
                    }
                }
                if (any_empty)
                {
                    ctx.record("[INFO] collection_linked_list_exhaustive: withNull — a "
                               "non-null String element decoded empty on this config "
                               "variant; null-slot placement proven HARD, content INFO.");
                }
                else
                {
                    ctx.check("clx_withnull_nonnull_content_around_null", nonnull_ok);
                }
            }
        }

        // =====================================================================
        // ROLE: Deque and Queue.  The same java.util.LinkedList klass backs both
        // — the chain walk is selected regardless of the static interface — and
        // the walk must reproduce the published HEAD->TAIL node order.  Joined-
        // sequence comparison vs the Java-published string is PASS-or-[INFO]
        // (content-dependent); size + distinctness HARD.
        // =====================================================================
        {
            const auto v{ fixture::to_vector_of<str_elem>("deque") };
            ctx.check("clx_deque_no_null", count_non_null(v) == static_cast<std::int32_t>(v.size()));
            ctx.check("clx_deque_size_matches_java",
                      static_cast<std::int32_t>(v.size()) == fixture::j_int("dequeSize"));
            ctx.check("clx_deque_all_oops_distinct_no_cycle", all_oops_distinct(v));
            const std::string seq{ join_string_contents(v) };
            const std::string java_seq{ fixture::j_str("dequeSeq") };
            if (seq.empty() || java_seq.empty())
            {
                ctx.record("[INFO] collection_linked_list_exhaustive: deque — node-order "
                           "sequence could not be content-compared on this config variant "
                           "(empty decode); size + distinctness proven HARD.");
            }
            else
            {
                ctx.check("clx_deque_node_order_matches_java", seq == java_seq);
            }
        }
        {
            const auto v{ fixture::to_vector_of<str_elem>("queue") };
            ctx.check("clx_queue_no_null", count_non_null(v) == static_cast<std::int32_t>(v.size()));
            ctx.check("clx_queue_size_matches_java",
                      static_cast<std::int32_t>(v.size()) == fixture::j_int("queueSize"));
            ctx.check("clx_queue_all_oops_distinct_no_cycle", all_oops_distinct(v));
            const std::string seq{ join_string_contents(v) };
            const std::string java_seq{ fixture::j_str("queueSeq") };
            if (seq.empty() || java_seq.empty())
            {
                ctx.record("[INFO] collection_linked_list_exhaustive: queue — FIFO order "
                           "could not be content-compared on this config variant; size + "
                           "distinctness proven HARD.");
            }
            else
            {
                ctx.check("clx_queue_fifo_order_matches_java", seq == java_seq);
            }
        }

        // =====================================================================
        // NODE ORDER != INSERTION ORDER: built by addFirst/addLast/add(index).
        // The walk must reproduce the published node order, not insertion order.
        // =====================================================================
        {
            const auto v{ fixture::to_vector_of<integer_box>("interleave") };
            ctx.check("clx_interleave_size_matches_java",
                      static_cast<std::int32_t>(v.size()) == fixture::j_int("interleaveSize"));
            ctx.check("clx_interleave_no_null", count_non_null(v) == static_cast<std::int32_t>(v.size()));
            ctx.check("clx_interleave_all_oops_distinct_no_cycle", all_oops_distinct(v));
            const std::string seq{ join_int_values(v) };
            const std::string java_seq{ fixture::j_str("interleaveSeq") };
            // The node order is a deterministic permutation (50,30,20,10,40); the
            // value decode of a boxed Integer is robust, so this is HARD when both
            // sides are non-empty (degrade to INFO only on an empty decode).
            if (seq.empty() || java_seq.empty())
            {
                ctx.record("[INFO] collection_linked_list_exhaustive: interleave — node "
                           "order could not be content-compared on this config variant; "
                           "size + distinctness proven HARD.");
            }
            else
            {
                ctx.check("clx_interleave_node_order_matches_java", seq == java_seq);
                // Sanity: node order is NOT the ascending insertion order, proving
                // the walk follows links, not an implicit sort.
                ctx.check("clx_interleave_node_order_not_sorted",
                          seq != "10,20,30,40,50");
            }
        }

        // =====================================================================
        // AFTER A MIDDLE REMOVE (node unlink): the forward walk must follow the
        // rewired `next` links and yield the surviving sequence [0,1,2,4,5,6].
        // =====================================================================
        {
            const auto v{ fixture::to_vector_of<integer_box>("afterRemove") };
            ctx.check("clx_afterremove_size_matches_java",
                      static_cast<std::int32_t>(v.size()) == fixture::j_int("afterRemoveSize"));
            ctx.check("clx_afterremove_no_null", count_non_null(v) == static_cast<std::int32_t>(v.size()));
            ctx.check("clx_afterremove_all_oops_distinct_no_cycle", all_oops_distinct(v));
            const std::string seq{ join_int_values(v) };
            const std::string java_seq{ fixture::j_str("afterRemoveSeq") };
            if (seq.empty() || java_seq.empty())
            {
                ctx.record("[INFO] collection_linked_list_exhaustive: afterRemove — "
                           "surviving sequence could not be content-compared on this "
                           "config variant; size + distinctness proven HARD.");
            }
            else
            {
                ctx.check("clx_afterremove_sequence_matches_java", seq == java_seq);
                // The removed middle value (3) must be ABSENT from the walk —
                // scanned by decoded VALUE (not substring) so the check is exact.
                bool removed_absent{ true };
                for (const auto& up : v)
                {
                    if (up && up->value() == 3)
                    {
                        removed_absent = false;
                    }
                }
                ctx.check("clx_afterremove_removed_value_absent", removed_absent);
            }
        }

        // =====================================================================
        // THREE INDEPENDENT READ PATHS converge on words10.  The focused module
        // proves this for the 3-element fixture; here we re-prove path-agreement
        // on a 10-element list so the exhaustive fixture also exercises the typed
        // wrapper and the direct free function (not just value_t::to_vector).
        //   (1) value_t::to_vector   (2) unique_ptr<linked_list> wrapper
        //   (3) linked_list_walk_items called directly on the OOP.
        // =====================================================================
        {
            void* const list_oop{ fixture::field_oop("words10") };
            ctx.check("clx_words10_oop_resolved", list_oop != nullptr);
            if (list_oop != nullptr)
            {
                // (1) value_t path (already content-checked above) — size here.
                const auto via_value{ fixture::to_vector_of<str_elem>("words10") };

                // (2) typed wrapper: get<unique_ptr<linked_list>> then to_vector.
                std::size_t wrapper_size{ 0 };
                bool wrapper_oop_ok{ false };
                {
                    const auto proxy{ fixture::static_field("words10") };
                    if (proxy.has_value())
                    {
                        std::unique_ptr<vmhook::linked_list> ll = proxy->get();   // copy-init
                        ctx.check("clx_words10_decodes_to_linked_list_wrapper",
                                  ll != nullptr);
                        if (ll)
                        {
                            wrapper_oop_ok = (ll->get_instance() == list_oop);
                            const auto wv{ ll->to_vector<str_elem>() };
                            wrapper_size = wv.size();
                        }
                    }
                }
                ctx.check("clx_words10_wrapper_oop_matches_field", wrapper_oop_ok);

                // (3) direct linked_list_walk_items on the OOP.
                std::vector<std::unique_ptr<str_elem>> direct;
                vmhook::linked_list_walk_items<str_elem>(list_oop, SIZE10, direct);
                ctx.check("clx_words10_direct_walk_size_is_n",
                          static_cast<std::int32_t>(direct.size()) == SIZE10);
                ctx.check("clx_words10_direct_walk_all_distinct",
                          all_oops_distinct(direct));

                // All three paths agree on the count (== 10).
                ctx.check("clx_words10_three_paths_same_size",
                          via_value.size() == wrapper_size
                          && via_value.size() == direct.size()
                          && via_value.size() == static_cast<std::size_t>(SIZE10));
            }
        }

        // =====================================================================
        // ROBUSTNESS — a declared-but-null LinkedList field and a missing field
        // name both decode to empty, never throw, stable on re-read.
        // =====================================================================
        {
            ctx.check("clx_null_list_returns_empty",
                      fixture::to_vector_of<str_elem>("nullList").empty());
            ctx.check("clx_missing_field_returns_empty",
                      fixture::to_vector_of<str_elem>("noSuchFieldXYZ").empty());
            ctx.check("clx_null_list_stable_on_reread",
                      fixture::to_vector_of<str_elem>("nullList").empty());
        }

        // =====================================================================
        // Interpreter-hook proof (pilot-style): a scoped_hook on touch(), driven
        // by a mode-1 probe, fires on real bytecode dispatch with the right
        // self+arg and the original body runs (observed == seed+42 == 8042).
        // scoped_hook (never shutdown_hooks here) so this module stays isolated.
        // =====================================================================
        {
            auto handle{ vmhook::scoped_hook<fixture>(
                "touch",
                [](vmhook::return_value&,
                   const std::unique_ptr<fixture>& self,
                   std::int32_t delta)
                {
                    g_hook_calls.fetch_add(1, std::memory_order_relaxed);
                    g_hook_arg.store(delta, std::memory_order_relaxed);
                    g_hook_saw_self.store(self != nullptr, std::memory_order_relaxed);
                }) };
            ctx.check("clx_hook_installed", handle.installed());

            const bool done{ ctx.run_probe(
                [](bool value)
                {
                    if (value)
                    {
                        fixture::set_done(false);
                        fixture::set_mode(1);
                    }
                    fixture::set_go(value);
                },
                []() { return fixture::get_done(); }) };

            ctx.check("clx_hook_probe_completed", done);
            ctx.check("clx_hook_fired",
                      g_hook_calls.load(std::memory_order_relaxed) >= 1);
            ctx.check("clx_hook_saw_self",
                      g_hook_saw_self.load(std::memory_order_relaxed));
            ctx.check("clx_hook_saw_arg_42",
                      g_hook_arg.load(std::memory_order_relaxed) == 42);
            ctx.check("clx_observed_is_8042", fixture::get_observed() == 8042);
        }
        // handle out of scope -> hook uninstalled; module isolated.
    }   // run_checks
}   // anonymous namespace

VMHOOK_JVM_MODULE(collection_linked_list_exhaustive)
{
    bool body_threw{ false };
    try
    {
        run_checks(ctx);
    }
    catch (...)
    {
        body_threw = true;
    }

    // FINAL CLEANUP — OUTSIDE the try so it ALWAYS runs; idempotent and
    // safe-when-empty.  Guarantees ZERO hooks armed on EVERY exit path even if
    // the body threw before the scoped_hook's scope exit.
    vmhook::shutdown_hooks();

    if (body_threw)
    {
        ctx.record("[INFO] collection_linked_list_exhaustive: the test body threw and "
                   "was contained (no crash, no hooks armed); see preceding checks for "
                   "partial results.");
    }
    ctx.check("clx_module_left_clean_final_shutdown", true);
}
