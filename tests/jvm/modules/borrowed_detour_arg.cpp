// borrowed_detour_arg JVM test module  (feature area: anchored references)
//
// Proves on a LIVE JVM that vmhook::borrowed<T> works as a DETOUR ARGUMENT —
// the zero-ceremony replacement for handing the user a raw oop and asking them
// to keep it fresh.
//
// detail::extract_frame_arg is the choke point every detour argument passes
// through.  The no-JVM sibling (tests/test_borrowed_detour_arg_nojvm.cpp) pins
// its type-level contract: the trait table, the descriptor, the slot widths,
// and the null-frame degradation.  None of that can prove the thing that
// actually matters, which is what this module asserts:
//
//   * a borrowed<W> RECEIVER resolves to the correct live object — verified by
//     reading that instance's own `seed` field through the borrow, on every
//     fire, and across two DIFFERENT instances in one probe cycle;
//   * two borrows of two different objects have DIFFERENT identities, and two
//     borrows of the SAME object in the same epoch have the same identity —
//     the property a raw address gives you by accident and a handle has to give
//     you on purpose;
//   * a borrow taken inside a detour is LIVE there (resolve() non-null, not
//     expired).  A borrow that were born expired would still satisfy every
//     no-JVM assertion, so this is the check that makes the type worth having;
//   * a borrowed<W> OBJECT ARGUMENT (not the receiver) decodes to the right
//     object, distinct from the receiver;
//   * a Java NULL object argument yields an EMPTY borrow that is explicitly NOT
//     expired — "there was no object" and "the object moved" must not be
//     conflated, because the caller's recovery differs;
//   * the slot table holds when borrows sit next to two-slot primitives: a
//     borrow receiver followed by (int, long, int), and the 8-argument
//     manyArgs shape with two longs and a double interleaved.  A wrong slot
//     width here does not crash — it silently feeds the detour plausible
//     garbage — so these are asserted against exact fixture constants.
//
// FIXTURE REUSE: this drives vmhook.fixtures.HookBasic unchanged.  Its modes 1
// / 5 / 3 / 21 / 27 / 26 already stage exactly the object shapes above for the
// unique_ptr-based hook_basic module, so the two modules assert the SAME live
// scenarios through the two different argument models — which is the most
// direct evidence available that switching a detour to borrowed<W> changes
// nothing about what it observes.
//
// JIT-RELIABILITY: identical discipline to hook_basic (see its header for the
// full rationale).  The detour is an i2i INTERPRETER patch, so a JIT-compiled
// dispatch bypasses it; HookBasic's methods can be JIT-warm by the time this
// module runs, especially since hook_basic itself has already driven them.  So
// every asserting drive goes through drive_until_fires(), which deoptimizes the
// method back to the interpreter and re-drives within a bounded budget.  The
// retry exists only to make the firing deterministic — the assertions on the
// final observations stay HARD.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

namespace
{
    // Wrapper for vmhook.fixtures.HookBasic.  Only the members this module
    // needs; hook_basic.cpp carries the exhaustive version.
    class hb_fixture : public vmhook::object<hb_fixture>
    {
    public:
        explicit hb_fixture(vmhook::oop_t instance) noexcept
            : vmhook::object<hb_fixture>{ instance }
        {
        }

        static auto set_go(bool value) -> void       { static_field("go")->set(value); }
        static auto set_done(bool value) -> void     { static_field("done")->set(value); }
        static auto get_done() -> bool               { return static_field("done")->get(); }
        static auto set_mode(std::int32_t m) -> void { static_field("mode")->set(m); }

        static auto get_instance_calls_made() -> std::int32_t
        {
            return static_field("instanceCallsMade")->get();
        }
        static auto get_obj_arg_result() -> std::int32_t
        {
            return static_field("objArgResult")->get();
        }
        static auto get_null_ref_args_both_null() -> bool
        {
            return static_field("nullRefArgsBothNull")->get();
        }

        // The instance's own seed — read through a borrow, this is what proves
        // the borrow resolved to the RIGHT object rather than merely to A object.
        auto seed() const -> std::int32_t { return get_field("seed")->get(); }
    };

    constexpr const char* FIXTURE_CLASS{ "vmhook/fixtures/HookBasic" };

    // ---- Fixture-mirrored constants (lockstep with HookBasic.java) ---------
    constexpr std::int32_t INSTANCE_CALLS{ 3 };
    constexpr std::int32_t PRIMARY_SEED{ 1000 };
    constexpr std::int32_t SEED_A{ 2000 };
    constexpr std::int32_t SEED_B{ 30000 };
    constexpr std::int32_t COMBINE_A{ 5 };
    constexpr std::int64_t COMBINE_B{ 0x1122334455667788LL };
    constexpr std::int32_t COMBINE_C{ -13 };
    constexpr std::int32_t MANY_A{ 11 };
    constexpr std::int64_t MANY_B{ 0x0A0B0C0D0E0F1011LL };
    constexpr std::int32_t MANY_C{ 22 };
    constexpr double       MANY_D{ 6.5 };
    constexpr std::int32_t MANY_E{ 33 };
    constexpr std::int64_t MANY_G{ -0x7FEEDDCCBBAA9988LL };
    constexpr std::int32_t MANY_H{ 44 };
    constexpr std::int32_t OBJ_ARG_SEED{ 7777 };
    constexpr std::int32_t OBJ_ARG_DELTA{ 9 };

    // ---- Observations ------------------------------------------------------
    std::atomic<std::int32_t> g_fires{ 0 };

    // Scenario A — borrowed receiver on touch(int).
    std::atomic<std::int32_t> g_self_truthy_fires{ 0 };   // operator bool
    std::atomic<std::int32_t> g_self_resolved_fires{ 0 }; // resolve() != nullptr
    std::atomic<std::int32_t> g_self_live_fires{ 0 };     // NOT expired
    std::atomic<std::int32_t> g_self_seed_ok_fires{ 0 };  // read the right seed
    std::atomic<std::int32_t> g_self_id_ok_fires{ 0 };    // id() is non-empty
    std::atomic<std::int32_t> g_self_id_stable_fires{ 0 };// two borrows, same id
    std::atomic<std::int32_t> g_self_is_wrapper_fires{ 0 };
    std::atomic<std::int64_t> g_arg_sum{ 0 };

    // Scenario B — two distinct instances in one cycle.
    std::atomic<std::int32_t> g_two_seed_first{ -1 };
    std::atomic<std::int32_t> g_two_seed_second{ -1 };
    std::atomic<bool>         g_two_ids_differ{ false };
    // The id captured on each fire, so the comparison happens across fires.
    vmhook::object_id         g_two_id_first{};

    // Scenario C — combine(int,long,int) behind a borrowed receiver.
    std::atomic<bool>         g_combine_self_ok{ false };
    std::atomic<bool>         g_combine_a_ok{ false };
    std::atomic<bool>         g_combine_b_ok{ false };
    std::atomic<bool>         g_combine_c_ok{ false };

    // Scenario D — manyArgs: 8 args, two longs + a double interleaved.
    std::atomic<bool> g_many_self_ok{ false };
    std::atomic<bool> g_many_a_ok{ false };
    std::atomic<bool> g_many_b_ok{ false };
    std::atomic<bool> g_many_c_ok{ false };
    std::atomic<bool> g_many_d_ok{ false };
    std::atomic<bool> g_many_e_ok{ false };
    std::atomic<bool> g_many_f_ok{ false };
    std::atomic<bool> g_many_g_ok{ false };
    std::atomic<bool> g_many_h_ok{ false };

    // Scenario E — objArg(HookBasic,int): a borrowed OBJECT ARGUMENT.
    std::atomic<bool>         g_obj_other_truthy{ false };
    std::atomic<std::int32_t> g_obj_other_seed{ -1 };
    std::atomic<bool>         g_obj_other_differs_from_self{ false };
    std::atomic<std::int32_t> g_obj_delta{ -1 };

    // Scenario F — nullRefArgs: a Java-null object argument.
    std::atomic<bool> g_null_obj_falsy{ false };
    std::atomic<bool> g_null_obj_not_expired{ false };
    std::atomic<bool> g_null_obj_raw_is_null{ false };
    std::atomic<bool> g_null_obj_id_empty{ false };
    std::atomic<bool> g_null_string_empty{ false };

    auto reset_observations() -> void
    {
        g_fires.store(0);
        g_self_truthy_fires.store(0);
        g_self_resolved_fires.store(0);
        g_self_live_fires.store(0);
        g_self_seed_ok_fires.store(0);
        g_self_id_ok_fires.store(0);
        g_self_id_stable_fires.store(0);
        g_self_is_wrapper_fires.store(0);
        g_arg_sum.store(0);
        g_two_seed_first.store(-1);
        g_two_seed_second.store(-1);
        g_two_ids_differ.store(false);
        g_two_id_first = vmhook::object_id{};
        g_combine_self_ok.store(false);
        g_combine_a_ok.store(false);
        g_combine_b_ok.store(false);
        g_combine_c_ok.store(false);
        g_many_self_ok.store(false);
        g_many_a_ok.store(false);
        g_many_b_ok.store(false);
        g_many_c_ok.store(false);
        g_many_d_ok.store(false);
        g_many_e_ok.store(false);
        g_many_f_ok.store(false);
        g_many_g_ok.store(false);
        g_many_h_ok.store(false);
        g_obj_other_truthy.store(false);
        g_obj_other_seed.store(-1);
        g_obj_other_differs_from_self.store(false);
        g_obj_delta.store(-1);
        g_null_obj_falsy.store(false);
        g_null_obj_not_expired.store(false);
        g_null_obj_raw_is_null.store(false);
        g_null_obj_id_empty.store(false);
        g_null_string_empty.store(false);
    }

    auto drive(vmhook_test::context& ctx, std::int32_t mode) -> bool
    {
        reset_observations();
        return ctx.run_probe(
            [mode](bool value)
            {
                if (value)
                {
                    hb_fixture::set_done(false);
                    hb_fixture::set_mode(mode);
                }
                hb_fixture::set_go(value);
            },
            []() { return hb_fixture::get_done(); });
    }

    // ---- JIT-reliability helpers (same shape as hook_basic.cpp) ------------

    auto find_method(const char* const name, const char* const signature)
        -> vmhook::hotspot::method*
    {
        vmhook::hotspot::klass* const k{ vmhook::find_class(FIXTURE_CLASS) };
        if (!k || !vmhook::hotspot::is_valid_pointer(k))
        {
            return nullptr;
        }
        const std::int32_t count{ k->get_methods_count() };
        vmhook::hotspot::method** const methods{ k->get_methods_ptr() };
        if (!methods || count <= 0)
        {
            return nullptr;
        }
        const std::string want_name{ name };
        const std::string want_sig{ signature };
        for (std::int32_t i{ 0 }; i < count; ++i)
        {
            vmhook::hotspot::method* const m{ methods[i] };
            if (!m || !vmhook::hotspot::is_valid_pointer(m))
            {
                continue;
            }
            const std::string m_name = m->get_name();
            const std::string m_sig = m->get_signature();
            if (m_name == want_name && m_sig == want_sig)
            {
                return m;
            }
        }
        return nullptr;
    }

    auto method_code(vmhook::hotspot::method* const m) -> void*
    {
        if (!m || !vmhook::hotspot::is_valid_pointer(m))
        {
            return nullptr;
        }
        void* const code{ m->get_code() };
        return (code && vmhook::hotspot::is_valid_pointer(code)) ? code : nullptr;
    }

    auto interp_routes_through_i2i(vmhook::hotspot::method* const m) -> bool
    {
        if (!m || !vmhook::hotspot::is_valid_pointer(m))
        {
            return false;
        }
        void* const i2i{ m->get_i2i_entry() };
        void* const fie{ m->get_from_interpreted_entry() };
        return i2i != nullptr && fie != nullptr && i2i == fie;
    }

    auto settle_interpreter_route(vmhook::hotspot::method* const m, int attempts) -> bool
    {
        if (m == nullptr)
        {
            (void)vmhook::verify_hooks();
            return false;
        }
        if (interp_routes_through_i2i(m) && method_code(m) == nullptr)
        {
            return true;
        }
        for (int attempt{ 0 }; attempt < attempts; ++attempt)
        {
            (void)vmhook::deoptimize_methods_if(
                [](const std::string& class_name, vmhook::hotspot::method*) -> bool
                {
                    return class_name == FIXTURE_CLASS;
                });
            (void)vmhook::verify_hooks();

            if (interp_routes_through_i2i(m) && method_code(m) == nullptr)
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{ 40 });
        }
        return interp_routes_through_i2i(m) && method_code(m) == nullptr;
    }

    auto drive_until_fires(vmhook_test::context& ctx,
                           std::int32_t mode,
                           vmhook::hotspot::method* const m,
                           std::int32_t expected_fires,
                           int attempts,
                           bool& done_out) -> void
    {
        done_out = false;
        for (int attempt{ 0 }; attempt < attempts; ++attempt)
        {
            (void)settle_interpreter_route(m, 12);
            done_out = drive(ctx, mode);
            if (done_out && g_fires.load() == expected_fires)
            {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{ 25 });
        }
    }
}

VMHOOK_JVM_MODULE(borrowed_detour_arg)
{
    vmhook::register_class<hb_fixture>("vmhook/fixtures/HookBasic");

    try
    {
    // =====================================================================
    // Scenario A — borrowed<W> RECEIVER on touch(int).
    //   The receiver is the argument every consumer touches on every hook, so
    //   it is where "the user never handles a raw oop" is won or lost.
    // =====================================================================
    {
        auto handle{ vmhook::scoped_hook<hb_fixture>(
            "touch",
            [](vmhook::return_value&,
               vmhook::borrowed<hb_fixture> self,
               std::int32_t delta)
            {
                g_fires.fetch_add(1, std::memory_order_relaxed);
                if (self) { g_self_truthy_fires.fetch_add(1, std::memory_order_relaxed); }
                if (self.resolve() != nullptr)
                {
                    g_self_resolved_fires.fetch_add(1, std::memory_order_relaxed);
                }
                // LIVE, not merely non-empty.  A borrow born expired would pass
                // every no-JVM check in the sibling file and fail here.
                if (!self.expired())
                {
                    g_self_live_fires.fetch_add(1, std::memory_order_relaxed);
                }
                if (self && self->seed() == PRIMARY_SEED)
                {
                    g_self_seed_ok_fires.fetch_add(1, std::memory_order_relaxed);
                }
                if (self.id())
                {
                    g_self_id_ok_fires.fetch_add(1, std::memory_order_relaxed);
                }
                // Two borrows of the SAME object in the SAME epoch must agree
                // on identity — otherwise a borrow could not be used as a map
                // key or compared across two detour arguments.
                const vmhook::borrowed<hb_fixture> again{ self.raw_unsafe() };
                if (again.id() == self.id())
                {
                    g_self_id_stable_fires.fetch_add(1, std::memory_order_relaxed);
                }
                if (self.is<hb_fixture>())
                {
                    g_self_is_wrapper_fires.fetch_add(1, std::memory_order_relaxed);
                }
                g_arg_sum.fetch_add(delta, std::memory_order_relaxed);
            }) };

        ctx.check("bda_touch_hook_installed", handle.installed());

        vmhook::hotspot::method* const m{ find_method("touch", "(I)I") };
        bool done{ false };
        drive_until_fires(ctx, 1, m, INSTANCE_CALLS, 6, done);

        ctx.check("bda_touch_probe_completed", done);
        ctx.check("bda_touch_java_made_3_calls",
                  hb_fixture::get_instance_calls_made() == INSTANCE_CALLS);
        ctx.check("bda_touch_fired_once_per_call", g_fires.load() == INSTANCE_CALLS);

        // The borrow arrived usable on EVERY fire — not "usually".
        ctx.check("bda_receiver_truthy_every_fire",
                  g_self_truthy_fires.load() == INSTANCE_CALLS);
        ctx.check("bda_receiver_resolves_every_fire",
                  g_self_resolved_fires.load() == INSTANCE_CALLS);
        ctx.check("bda_receiver_not_expired_every_fire",
                  g_self_live_fires.load() == INSTANCE_CALLS);
        // ... and it is the RIGHT object, read through the handle.
        ctx.check("bda_receiver_reads_own_seed_every_fire",
                  g_self_seed_ok_fires.load() == INSTANCE_CALLS);
        ctx.check("bda_receiver_has_identity_every_fire",
                  g_self_id_ok_fires.load() == INSTANCE_CALLS);
        ctx.check("bda_receiver_identity_stable_every_fire",
                  g_self_id_stable_fires.load() == INSTANCE_CALLS);
        ctx.check("bda_receiver_is_registered_wrapper_every_fire",
                  g_self_is_wrapper_fires.load() == INSTANCE_CALLS);
        // The trailing primitive still lands in the right slot behind a borrow.
        ctx.check("bda_touch_arg_sum_matches", g_arg_sum.load() == (7 + 11 + 42));
    }

    // =====================================================================
    // Scenario B — TWO DIFFERENT instances in one probe cycle.  Each fire's
    //   borrow must read ITS OWN seed and carry a DIFFERENT identity.  This is
    //   the check that a borrow tracks an object rather than a slot.
    // =====================================================================
    {
        auto handle{ vmhook::scoped_hook<hb_fixture>(
            "touch",
            [](vmhook::return_value&,
               vmhook::borrowed<hb_fixture> self,
               std::int32_t)
            {
                const std::int32_t n{ g_fires.fetch_add(1, std::memory_order_relaxed) };
                const std::int32_t seed{ self ? self->seed() : -1 };
                if (n == 0)
                {
                    g_two_seed_first.store(seed, std::memory_order_relaxed);
                    g_two_id_first = self.id();
                }
                else if (n == 1)
                {
                    g_two_seed_second.store(seed, std::memory_order_relaxed);
                    g_two_ids_differ.store(!(self.id() == g_two_id_first),
                                           std::memory_order_relaxed);
                }
            }) };

        ctx.check("bda_two_hook_installed", handle.installed());

        vmhook::hotspot::method* const m{ find_method("touch", "(I)I") };
        bool done{ false };
        drive_until_fires(ctx, 5, m, 2, 6, done);

        ctx.check("bda_two_probe_completed", done);
        ctx.check("bda_two_fired_twice", g_fires.load() == 2);
        ctx.check("bda_two_first_borrow_reads_seed_a",
                  g_two_seed_first.load() == SEED_A);
        ctx.check("bda_two_second_borrow_reads_seed_b",
                  g_two_seed_second.load() == SEED_B);
        ctx.check("bda_two_borrows_have_distinct_identities",
                  g_two_ids_differ.load());
    }

    // =====================================================================
    // Scenario C — combine(int,long,int) behind a borrowed receiver.  The long
    //   occupies TWO slots, so the trailing int is at slot 4, not 3.  A borrow
    //   miscounted as two slots would shift every argument after it.
    // =====================================================================
    {
        auto handle{ vmhook::scoped_hook<hb_fixture>(
            "combine",
            [](vmhook::return_value&,
               vmhook::borrowed<hb_fixture> self,
               std::int32_t a,
               std::int64_t b,
               std::int32_t c)
            {
                g_fires.fetch_add(1, std::memory_order_relaxed);
                g_combine_self_ok.store(static_cast<bool>(self)
                                        && self->seed() == PRIMARY_SEED,
                                        std::memory_order_relaxed);
                g_combine_a_ok.store(a == COMBINE_A, std::memory_order_relaxed);
                g_combine_b_ok.store(b == COMBINE_B, std::memory_order_relaxed);
                g_combine_c_ok.store(c == COMBINE_C, std::memory_order_relaxed);
            }) };

        ctx.check("bda_combine_hook_installed", handle.installed());

        vmhook::hotspot::method* const m{ find_method("combine", "(IJI)J") };
        bool done{ false };
        drive_until_fires(ctx, 3, m, 1, 6, done);

        ctx.check("bda_combine_probe_completed", done);
        ctx.check("bda_combine_fired_once", g_fires.load() == 1);
        ctx.check("bda_combine_receiver_correct", g_combine_self_ok.load());
        ctx.check("bda_combine_int_before_long_correct", g_combine_a_ok.load());
        ctx.check("bda_combine_long_correct", g_combine_b_ok.load());
        // The one that a slot-width error would break.
        ctx.check("bda_combine_int_after_long_correct", g_combine_c_ok.load());
    }

    // =====================================================================
    // Scenario D — manyArgs: EIGHT arguments behind a borrowed receiver, with
    //   two longs and a double interleaved.  The worst slot-alignment case the
    //   fixture offers; every trailing argument is asserted exactly.
    // =====================================================================
    {
        auto handle{ vmhook::scoped_hook<hb_fixture>(
            "manyArgs",
            [](vmhook::return_value&,
               vmhook::borrowed<hb_fixture> self,
               std::int32_t a,
               std::int64_t b,
               std::int32_t c,
               double d,
               std::int32_t e,
               const std::string& f,
               std::int64_t g,
               std::int32_t h)
            {
                g_fires.fetch_add(1, std::memory_order_relaxed);
                g_many_self_ok.store(static_cast<bool>(self)
                                     && self->seed() == PRIMARY_SEED,
                                     std::memory_order_relaxed);
                g_many_a_ok.store(a == MANY_A, std::memory_order_relaxed);
                g_many_b_ok.store(b == MANY_B, std::memory_order_relaxed);
                g_many_c_ok.store(c == MANY_C, std::memory_order_relaxed);
                g_many_d_ok.store(d == MANY_D, std::memory_order_relaxed);
                g_many_e_ok.store(e == MANY_E, std::memory_order_relaxed);
                g_many_f_ok.store(f == "many", std::memory_order_relaxed);
                g_many_g_ok.store(g == MANY_G, std::memory_order_relaxed);
                g_many_h_ok.store(h == MANY_H, std::memory_order_relaxed);
            }) };

        ctx.check("bda_many_hook_installed", handle.installed());

        vmhook::hotspot::method* const m{
            find_method("manyArgs", "(IJIDILjava/lang/String;JI)J") };
        bool done{ false };
        drive_until_fires(ctx, 21, m, 1, 6, done);

        ctx.check("bda_many_probe_completed", done);
        ctx.check("bda_many_fired_once", g_fires.load() == 1);
        ctx.check("bda_many_receiver_correct", g_many_self_ok.load());
        ctx.check("bda_many_arg_a_int", g_many_a_ok.load());
        ctx.check("bda_many_arg_b_long", g_many_b_ok.load());
        ctx.check("bda_many_arg_c_int_after_long", g_many_c_ok.load());
        ctx.check("bda_many_arg_d_double", g_many_d_ok.load());
        ctx.check("bda_many_arg_e_int_after_double", g_many_e_ok.load());
        ctx.check("bda_many_arg_f_string", g_many_f_ok.load());
        ctx.check("bda_many_arg_g_long_after_ref", g_many_g_ok.load());
        ctx.check("bda_many_arg_h_trailing_int", g_many_h_ok.load());
    }

    // =====================================================================
    // Scenario E — objArg(HookBasic,int): a borrowed OBJECT ARGUMENT that is
    //   NOT the receiver.  Both are borrows in the same detour, so this also
    //   pins that two live borrows in one frame stay distinct.
    // =====================================================================
    {
        auto handle{ vmhook::scoped_hook<hb_fixture>(
            "objArg",
            [](vmhook::return_value&,
               vmhook::borrowed<hb_fixture> self,
               vmhook::borrowed<hb_fixture> other,
               std::int32_t delta)
            {
                g_fires.fetch_add(1, std::memory_order_relaxed);
                g_obj_other_truthy.store(static_cast<bool>(other),
                                         std::memory_order_relaxed);
                if (other)
                {
                    g_obj_other_seed.store(other->seed(), std::memory_order_relaxed);
                }
                g_obj_other_differs_from_self.store(!(other == self),
                                                    std::memory_order_relaxed);
                g_obj_delta.store(delta, std::memory_order_relaxed);
            }) };

        ctx.check("bda_objarg_hook_installed", handle.installed());

        vmhook::hotspot::method* const m{
            find_method("objArg", "(Lvmhook/fixtures/HookBasic;I)I") };
        bool done{ false };
        drive_until_fires(ctx, 27, m, 1, 6, done);

        ctx.check("bda_objarg_probe_completed", done);
        ctx.check("bda_objarg_fired_once", g_fires.load() == 1);
        ctx.check("bda_objarg_other_is_live", g_obj_other_truthy.load());
        ctx.check("bda_objarg_other_reads_its_own_seed",
                  g_obj_other_seed.load() == OBJ_ARG_SEED);
        ctx.check("bda_objarg_other_distinct_from_receiver",
                  g_obj_other_differs_from_self.load());
        ctx.check("bda_objarg_trailing_int_correct",
                  g_obj_delta.load() == OBJ_ARG_DELTA);
        // Allow-through: the original body still ran with the unmodified args.
        ctx.check("bda_objarg_allow_through",
                  hb_fixture::get_obj_arg_result() == (OBJ_ARG_SEED + OBJ_ARG_DELTA));
    }

    // =====================================================================
    // Scenario F — nullRefArgs(String,HookBasic) with both references NULL.
    //   A Java null must produce an EMPTY borrow that is explicitly NOT
    //   expired.  Conflating "there was no object" with "the object moved"
    //   would send the caller down the wrong recovery path.
    // =====================================================================
    {
        auto handle{ vmhook::scoped_hook<hb_fixture>(
            "nullRefArgs",
            [](vmhook::return_value&,
               vmhook::borrowed<hb_fixture> self,
               const std::string& s,
               vmhook::borrowed<hb_fixture> obj)
            {
                g_fires.fetch_add(1, std::memory_order_relaxed);
                (void)self;
                g_null_string_empty.store(s.empty(), std::memory_order_relaxed);
                g_null_obj_falsy.store(!static_cast<bool>(obj),
                                       std::memory_order_relaxed);
                g_null_obj_not_expired.store(!obj.expired(), std::memory_order_relaxed);
                g_null_obj_raw_is_null.store(obj.raw_unsafe() == nullptr,
                                             std::memory_order_relaxed);
                g_null_obj_id_empty.store(!obj.id(), std::memory_order_relaxed);
            }) };

        ctx.check("bda_nullref_hook_installed", handle.installed());

        vmhook::hotspot::method* const m{
            find_method("nullRefArgs", "(Ljava/lang/String;Lvmhook/fixtures/HookBasic;)Z") };
        bool done{ false };
        drive_until_fires(ctx, 26, m, 1, 6, done);

        ctx.check("bda_nullref_probe_completed", done);
        ctx.check("bda_nullref_fired_once", g_fires.load() == 1);
        ctx.check("bda_nullref_java_saw_both_null",
                  hb_fixture::get_null_ref_args_both_null());
        ctx.check("bda_nullref_string_decoded_empty", g_null_string_empty.load());
        ctx.check("bda_nullref_borrow_is_falsy", g_null_obj_falsy.load());
        // The distinction that matters: empty is not expired.
        ctx.check("bda_nullref_borrow_is_not_expired", g_null_obj_not_expired.load());
        ctx.check("bda_nullref_borrow_raw_is_null", g_null_obj_raw_is_null.load());
        ctx.check("bda_nullref_borrow_id_is_empty", g_null_obj_id_empty.load());
    }
    }
    catch (const std::exception& ex)
    {
        ctx.record(std::string{ "[INFO] borrowed_detour_arg: aborted with " } + ex.what());
    }
    catch (...)
    {
        ctx.record("[INFO] borrowed_detour_arg: aborted with an unknown exception");
    }

    // UNCONDITIONAL teardown: on the no-SEH Windows path a faulting body can
    // longjmp past a scoped_hook destructor, leaving a hook armed for the NEXT
    // module.  Force a full reset so the table is empty regardless.
    if (ctx.reset)
    {
        ctx.reset();
    }
}
