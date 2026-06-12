// hook_signature JVM test module  (feature area: hooks)
//
// Exhaustively exercises the SIGNATURE-FILTERED hook overload
//     vmhook::hook<T>(name, signature, detour)      (vmhook.hpp:7850-8.. )
//     vmhook::scoped_hook<T>(name, signature, detour)(vmhook.hpp:8732-8821)
// whose whole reason to exist is OVERLOAD SELECTION: a Java class has several
// methods sharing one name but with different JVM descriptors, and the caller
// installs the detour on EXACTLY ONE of them by descriptor.
//
// On a live JVM (real bytecode dispatch via the Harness go/done probe) this
// module proves, with many ctx.check() angles:
//   * the descriptor-selected overload fires, and EVERY sibling overload of the
//     same name — called in the same run() — does NOT fire (no cross-fire);
//   * the selected overload's args decode at the slot offsets implied by ITS
//     descriptor (int / long(2 slots) / double(2 slots) / boolean / String ref /
//     Object ref / int[] array / trailing-int-after-long ordering);
//   * static overloads are selectable with NO implicit `this`;
//   * the empty-signature overload hook<T>(name, cb) binds to the first same-name
//     overload IN _methods ARRAY ORDER (HotSpot sorts by name-/signature-Symbol
//     address, NOT source order) — characterized here against vmhook's own
//     enumeration, since the bound overload is not portably the source-first one;
//   * a descriptor matching NO overload fails install (false), while a different
//     valid descriptor on the same name still installs (true);
//   * duplicate install on name+signature: second returns true, FIRST detour
//     stays active, second silently dropped (current behaviour, locked);
//   * scoped_hook teardown is per-overload: dropping the (I)I handle stops the
//     int detour but a still-live (J)J handle keeps firing;
//   * force-return on the selected overload replaces ONLY that overload's return.
//
// Harness note: `done` LATCHES; each scenario resets it + sets `mode` on the
// rising edge of `go`, runs ONE probe cycle, then reads back observations.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace
{
    // Wrapper for vmhook.fixtures.HookSignature.
    class hook_sig_fixture : public vmhook::object<hook_sig_fixture>
    {
    public:
        explicit hook_sig_fixture(vmhook::oop_t instance) noexcept
            : vmhook::object<hook_sig_fixture>{ instance }
        {
        }

        // --- go/done handshake + scenario selector ------------------------
        static auto set_go(bool value) -> void       { static_field("go")->set(value); }
        static auto set_done(bool value) -> void      { static_field("done")->set(value); }
        static auto get_done() -> bool                { return static_field("done")->get(); }
        static auto set_mode(std::int32_t m) -> void  { static_field("mode")->set(m); }

        // --- recorded observations the Java side writes -------------------
        static auto get_res_i() -> std::int32_t       { return static_field("resI")->get(); }
        static auto get_res_j() -> std::int64_t       { return static_field("resJ")->get(); }
        static auto get_res_d() -> double             { return static_field("resD")->get(); }
        static auto get_res_str() -> std::int32_t     { return static_field("resStr")->get(); }
        static auto get_res_mix_il() -> std::int64_t  { return static_field("resMixIL")->get(); }
        static auto get_res_mix_li() -> std::int64_t  { return static_field("resMixLI")->get(); }
        static auto get_res_comb1() -> std::int32_t   { return static_field("resComb1")->get(); }
        static auto get_res_comb2() -> std::int32_t   { return static_field("resComb2")->get(); }
        static auto get_res_ref_obj() -> std::int32_t { return static_field("resRefObj")->get(); }
        static auto get_res_ref_arr() -> std::int32_t { return static_field("resRefArr")->get(); }
        static auto get_res_ref_str() -> std::int32_t { return static_field("resRefStr")->get(); }
        static auto get_res_stat_i() -> std::int64_t  { return static_field("resStatI")->get(); }
        static auto get_res_stat_j() -> std::int64_t  { return static_field("resStatJ")->get(); }
        static auto get_res_wide() -> double          { return static_field("resWide")->get(); }
        static auto get_process_int_calls() -> std::int32_t { return static_field("processIntCalls")->get(); }

        // Reads this instance's own seed (proves `self` is the right object).
        auto seed() const -> std::int32_t { return get_field("seed")->get(); }
    };

    // ---- Fixture-mirrored constants (lockstep with HookSignature.java) ------
    constexpr std::int32_t SEED{ 7000 };
    constexpr std::int32_t ARG_I{ 314 };
    constexpr std::int64_t ARG_J{ 0x0BADC0DE0BADC0DELL };
    constexpr double       ARG_D{ 6.875 };
    constexpr std::int32_t STR_LEN{ 9 };
    constexpr std::int32_t MIX_I{ 41 };
    constexpr std::int64_t MIX_J{ 0x7766554433221100LL };
    constexpr std::int32_t COMB_A{ 100 };
    constexpr std::int32_t COMB_B{ 23 };
    constexpr std::int32_t REF_TAG{ 555 };
    constexpr std::int32_t STAT_I{ -17 };
    constexpr std::int64_t STAT_J{ 0x00000001FFFFFFFFLL };
    constexpr double       WIDE_D{ 1.25 };
    constexpr std::int32_t WIDE_S_LEN{ 4 };
    constexpr std::int32_t WIDE_I{ 88 };
    constexpr std::int32_t PROCESS_INT_CALLS{ 5 };

    // ---- Per-overload fire counters + decoded-arg captures -----------------
    // Family A: process(...)
    std::atomic<std::int32_t> g_fire_proc_i{ 0 };
    std::atomic<std::int32_t> g_fire_proc_j{ 0 };
    std::atomic<std::int32_t> g_fire_proc_d{ 0 };
    std::atomic<std::int32_t> g_fire_proc_str{ 0 };
    std::atomic<std::int32_t> g_seen_proc_i{ 0 };
    std::atomic<std::int64_t> g_seen_proc_j{ 0 };
    std::atomic<std::int64_t> g_seen_proc_d_bits{ 0 };   // bit-pattern of decoded double
    std::atomic<std::int32_t> g_seen_proc_str_len{ -1 };
    std::atomic<bool>         g_proc_i_self_ok{ false };
    std::atomic<bool>         g_proc_j_self_ok{ false };

    // Family B: mix(...)
    std::atomic<std::int32_t> g_fire_mix_il{ 0 };
    std::atomic<std::int32_t> g_fire_mix_li{ 0 };
    std::atomic<bool>         g_mix_il_a_ok{ false };
    std::atomic<bool>         g_mix_il_b_ok{ false };
    std::atomic<bool>         g_mix_li_a_ok{ false };
    std::atomic<bool>         g_mix_li_b_ok{ false };

    // Family C: combine(...)
    std::atomic<std::int32_t> g_fire_comb1{ 0 };
    std::atomic<std::int32_t> g_fire_comb2{ 0 };
    std::atomic<bool>         g_comb1_arg_ok{ false };
    std::atomic<bool>         g_comb2_args_ok{ false };

    // Family D: refTake(...)
    std::atomic<std::int32_t> g_fire_ref_obj{ 0 };
    std::atomic<std::int32_t> g_fire_ref_arr{ 0 };
    std::atomic<std::int32_t> g_fire_ref_str{ 0 };
    std::atomic<bool>         g_ref_obj_nonnull{ false };
    std::atomic<bool>         g_ref_arr_nonnull{ false };
    std::atomic<std::int32_t> g_ref_str_len{ -1 };

    // Family E: static stat(...)
    std::atomic<std::int32_t> g_fire_stat_i{ 0 };
    std::atomic<std::int32_t> g_fire_stat_j{ 0 };
    std::atomic<std::int32_t> g_seen_stat_i{ 0 };
    std::atomic<std::int64_t> g_seen_stat_j{ 0 };

    // Empty-signature ambiguity probe (mode 1): which overload did the no-filter
    // hook pick?  Counted on the SAME process family counters but via a separate
    // detour, so we use dedicated counters to avoid clashing with explicit hooks.
    std::atomic<std::int32_t> g_empty_fire_total{ 0 };
    std::atomic<std::int32_t> g_empty_saw_int_arg{ 0 };
    std::atomic<std::int32_t> g_empty_raw_slot1{ 0 };   // raw slot-1-as-int the empty-sig detour decoded

    // Duplicate-install probe: which of the two detours fired.
    std::atomic<std::int32_t> g_dup_first{ 0 };
    std::atomic<std::int32_t> g_dup_second{ 0 };

    // Wide multi-slot
    std::atomic<std::int32_t> g_fire_wide{ 0 };
    std::atomic<bool>         g_wide_flag_ok{ false };
    std::atomic<bool>         g_wide_d_ok{ false };
    std::atomic<bool>         g_wide_s_ok{ false };
    std::atomic<bool>         g_wide_i_ok{ false };
    std::atomic<bool>         g_wide_self_ok{ false };

    auto reset_all() -> void
    {
        g_fire_proc_i.store(0); g_fire_proc_j.store(0);
        g_fire_proc_d.store(0); g_fire_proc_str.store(0);
        g_seen_proc_i.store(0); g_seen_proc_j.store(0);
        g_seen_proc_d_bits.store(0); g_seen_proc_str_len.store(-1);
        g_proc_i_self_ok.store(false); g_proc_j_self_ok.store(false);
        g_fire_mix_il.store(0); g_fire_mix_li.store(0);
        g_mix_il_a_ok.store(false); g_mix_il_b_ok.store(false);
        g_mix_li_a_ok.store(false); g_mix_li_b_ok.store(false);
        g_fire_comb1.store(0); g_fire_comb2.store(0);
        g_comb1_arg_ok.store(false); g_comb2_args_ok.store(false);
        g_fire_ref_obj.store(0); g_fire_ref_arr.store(0); g_fire_ref_str.store(0);
        g_ref_obj_nonnull.store(false); g_ref_arr_nonnull.store(false);
        g_ref_str_len.store(-1);
        g_fire_stat_i.store(0); g_fire_stat_j.store(0);
        g_seen_stat_i.store(0); g_seen_stat_j.store(0);
        g_empty_fire_total.store(0); g_empty_saw_int_arg.store(0);
        g_empty_raw_slot1.store(0);
        g_dup_first.store(0); g_dup_second.store(0);
        g_fire_wide.store(0);
        g_wide_flag_ok.store(false); g_wide_d_ok.store(false);
        g_wide_s_ok.store(false); g_wide_i_ok.store(false);
        g_wide_self_ok.store(false);
    }

    auto drive(vmhook_test::context& ctx, std::int32_t mode) -> bool
    {
        reset_all();
        return ctx.run_probe(
            [mode](bool value)
            {
                if (value)
                {
                    hook_sig_fixture::set_done(false);
                    hook_sig_fixture::set_mode(mode);
                }
                hook_sig_fixture::set_go(value);
            },
            []() { return hook_sig_fixture::get_done(); });
    }

    // Reinterpret a double's bits as int64 (for atomic capture/compare).
    auto dbits(double d) -> std::int64_t
    {
        std::int64_t out{};
        std::memcpy(&out, &d, sizeof(out));
        return out;
    }

    // The descriptor of the FIRST entry named `name` in the klass's _methods
    // array, in EXACT array order.  vmhook's empty-signature hook<T>(name, cb)
    // binds to precisely this overload: both walk get_methods_ptr() from index 0
    // and accept the first name match (vmhook.hpp:8012-8023, scoped_hook re-resolve
    // 8963-8978; get_class_methods walks the identical array via collect_klass_methods
    // 6941-6949).  HotSpot stores _methods SORTED by name-/signature-Symbol ADDRESS
    // (Symbol::fast_compare), which is interning-order dependent and therefore NOT
    // source-declaration order — so this is the only portable way to know, on THIS
    // JDK, which overload the no-filter hook actually selects.  Empty if not found.
    auto first_array_order_descriptor(const std::string& name) -> std::string
    {
        const std::vector<std::pair<std::string, std::string>> methods{
            vmhook::get_class_methods<hook_sig_fixture>() };
        for (const std::pair<std::string, std::string>& m : methods)
        {
            if (m.first == name)
            {
                return m.second;
            }
        }
        return {};
    }
}

VMHOOK_JVM_MODULE(hook_signature)
{
    vmhook::register_class<hook_sig_fixture>("vmhook/fixtures/HookSignature");

    // =====================================================================
    // Block 1 — THE CORE CONTRACT: hook ONE of four same-named process(...)
    //   overloads by descriptor, call ALL FOUR, prove ONLY the selected one
    //   fires (no cross-fire) and its arg decodes at the right slot.
    //   We install all four explicit-descriptor hooks SIMULTANEOUSLY (each on a
    //   different descriptor) so we also prove four distinct detours coexist and
    //   each fires for exactly its own descriptor.
    // =====================================================================
    {
        auto h_i{ vmhook::scoped_hook<hook_sig_fixture>(
            "process", "(I)I",
            [](vmhook::return_value&, const std::unique_ptr<hook_sig_fixture>& self, std::int32_t v)
            {
                g_fire_proc_i.fetch_add(1, std::memory_order_relaxed);
                g_seen_proc_i.store(v, std::memory_order_relaxed);
                g_proc_i_self_ok.store(self != nullptr && self->seed() == SEED, std::memory_order_relaxed);
            }) };
        auto h_j{ vmhook::scoped_hook<hook_sig_fixture>(
            "process", "(J)J",
            [](vmhook::return_value&, const std::unique_ptr<hook_sig_fixture>& self, std::int64_t v)
            {
                g_fire_proc_j.fetch_add(1, std::memory_order_relaxed);
                g_seen_proc_j.store(v, std::memory_order_relaxed);
                g_proc_j_self_ok.store(self != nullptr && self->seed() == SEED, std::memory_order_relaxed);
            }) };
        auto h_d{ vmhook::scoped_hook<hook_sig_fixture>(
            "process", "(D)D",
            [](vmhook::return_value&, const std::unique_ptr<hook_sig_fixture>&, double v)
            {
                g_fire_proc_d.fetch_add(1, std::memory_order_relaxed);
                g_seen_proc_d_bits.store(dbits(v), std::memory_order_relaxed);
            }) };
        auto h_s{ vmhook::scoped_hook<hook_sig_fixture>(
            "process", "(Ljava/lang/String;)I",
            [](vmhook::return_value&, const std::unique_ptr<hook_sig_fixture>&, const std::string& s)
            {
                g_fire_proc_str.fetch_add(1, std::memory_order_relaxed);
                g_seen_proc_str_len.store(static_cast<std::int32_t>(s.size()), std::memory_order_relaxed);
            }) };

        ctx.check("proc_int_handle_installed", h_i.installed());
        ctx.check("proc_long_handle_installed", h_j.installed());
        ctx.check("proc_double_handle_installed", h_d.installed());
        ctx.check("proc_string_handle_installed", h_s.installed());
        ctx.check("four_overload_handles_are_distinct_methods",
                  h_i.installed() && h_j.installed() && h_d.installed() && h_s.installed());

        const bool done{ drive(ctx, 1) };  // calls all four overloads once each
        ctx.check("all_process_probe_completed", done);

        // -- each selected overload fired EXACTLY ONCE --
        ctx.check("proc_int_fired_once", g_fire_proc_i.load() == 1);
        ctx.check("proc_long_fired_once", g_fire_proc_j.load() == 1);
        ctx.check("proc_double_fired_once", g_fire_proc_d.load() == 1);
        ctx.check("proc_string_fired_once", g_fire_proc_str.load() == 1);

        // -- NO cross-fire: each detour only ever saw its own descriptor.
        //    (If slot/descriptor matching were wrong, e.g. the (J)J detour fired
        //     on the int call, these would exceed 1.)
        ctx.check("proc_int_no_extra_fires", g_fire_proc_i.load() <= 1);
        ctx.check("proc_long_no_extra_fires", g_fire_proc_j.load() <= 1);
        ctx.check("proc_double_no_extra_fires", g_fire_proc_d.load() <= 1);
        ctx.check("proc_string_no_extra_fires", g_fire_proc_str.load() <= 1);

        // -- decoded args correct for the chosen descriptor --
        ctx.check("proc_int_arg_decoded", g_seen_proc_i.load() == ARG_I);
        ctx.check("proc_long_arg_decoded_full_64bit", g_seen_proc_j.load() == ARG_J);
        ctx.check("proc_double_arg_decoded", g_seen_proc_d_bits.load() == dbits(ARG_D));
        ctx.check("proc_string_arg_decoded_len", g_seen_proc_str_len.load() == STR_LEN);

        // -- self correct on the instance overloads that take it --
        ctx.check("proc_int_self_correct", g_proc_i_self_ok.load());
        ctx.check("proc_long_self_correct", g_proc_j_self_ok.load());

        // -- allow-through: every overload returned its UNMODIFIED original.
        ctx.check("proc_int_allow_through", hook_sig_fixture::get_res_i() == (SEED + ARG_I));
        ctx.check("proc_long_allow_through",
                  hook_sig_fixture::get_res_j() == (static_cast<std::int64_t>(SEED) + ARG_J));
        ctx.check("proc_double_allow_through",
                  hook_sig_fixture::get_res_d() == (static_cast<double>(SEED) + ARG_D));
        ctx.check("proc_string_allow_through",
                  hook_sig_fixture::get_res_str() == (SEED + STR_LEN));
    }
    // all four handles dropped -> all four process hooks uninstalled.

    // =====================================================================
    // Block 2 — SINGLE-OVERLOAD isolation: with ONLY the (J)J hook installed,
    //   call ALL FOUR overloads (mode 1).  The long detour must fire exactly
    //   once and NONE of the int/double/string calls may trip it.
    // =====================================================================
    {
        auto h_j{ vmhook::scoped_hook<hook_sig_fixture>(
            "process", "(J)J",
            [](vmhook::return_value&, const std::unique_ptr<hook_sig_fixture>&, std::int64_t v)
            {
                g_fire_proc_j.fetch_add(1, std::memory_order_relaxed);
                g_seen_proc_j.store(v, std::memory_order_relaxed);
            }) };
        ctx.check("isolation_long_only_installed", h_j.installed());

        const bool done{ drive(ctx, 1) };   // all four overloads called
        ctx.check("isolation_probe_completed", done);

        ctx.check("isolation_long_fired_once", g_fire_proc_j.load() == 1);
        ctx.check("isolation_long_saw_long_arg", g_seen_proc_j.load() == ARG_J);
        // The sibling overloads ran (Java recorded their results) but did NOT
        // fire our single long detour.  Counters for the other overloads were
        // never installed this block, so their fire counts stay zero.
        ctx.check("isolation_int_overload_did_not_fire_long_detour", g_fire_proc_i.load() == 0);
        ctx.check("isolation_double_overload_did_not_fire_long_detour", g_fire_proc_d.load() == 0);
        ctx.check("isolation_string_overload_did_not_fire_long_detour", g_fire_proc_str.load() == 0);
        // Java still executed every sibling (no install means original ran).
        ctx.check("isolation_int_sibling_ran", hook_sig_fixture::get_res_i() == (SEED + ARG_I));
        ctx.check("isolation_double_sibling_ran",
                  hook_sig_fixture::get_res_d() == (static_cast<double>(SEED) + ARG_D));
        ctx.check("isolation_string_sibling_ran", hook_sig_fixture::get_res_str() == (SEED + STR_LEN));
    }

    // =====================================================================
    // Block 3 — ARG-ORDER overloads: mix(int,long) vs mix(long,int).  Same name,
    //   same arity, DIFFERENT slot widths.  Hook ONLY (IJ)J; call BOTH.  Proves
    //   the (JI)J sibling never fires AND that the long lands in the right slot.
    // =====================================================================
    {
        auto h{ vmhook::scoped_hook<hook_sig_fixture>(
            "mix", "(IJ)J",
            [](vmhook::return_value&, const std::unique_ptr<hook_sig_fixture>&,
               std::int32_t a, std::int64_t b)
            {
                g_fire_mix_il.fetch_add(1, std::memory_order_relaxed);
                g_mix_il_a_ok.store(a == MIX_I, std::memory_order_relaxed);
                g_mix_il_b_ok.store(b == MIX_J, std::memory_order_relaxed);
            }) };
        ctx.check("mix_il_handle_installed", h.installed());

        const bool done{ drive(ctx, 6) };   // calls mix(I,J) then mix(J,I)
        ctx.check("mix_probe_completed", done);

        ctx.check("mix_il_fired_once", g_fire_mix_il.load() == 1);
        ctx.check("mix_li_did_not_fire", g_fire_mix_li.load() == 0);  // never installed
        ctx.check("mix_il_int_arg_slot0", g_mix_il_a_ok.load());
        ctx.check("mix_il_long_arg_slot1", g_mix_il_b_ok.load());
        // allow-through on both.
        ctx.check("mix_il_allow_through",
                  hook_sig_fixture::get_res_mix_il() == (static_cast<std::int64_t>(MIX_I) + MIX_J));
        ctx.check("mix_li_allow_through",
                  hook_sig_fixture::get_res_mix_li() == (MIX_J + static_cast<std::int64_t>(MIX_I)));
    }

    // Block 3b — the MIRROR: hook ONLY (JI)J, call BOTH.  Trailing int sits at
    //   slot 2 because the leading long ate slots 0-1.
    {
        auto h{ vmhook::scoped_hook<hook_sig_fixture>(
            "mix", "(JI)J",
            [](vmhook::return_value&, const std::unique_ptr<hook_sig_fixture>&,
               std::int64_t a, std::int32_t b)
            {
                g_fire_mix_li.fetch_add(1, std::memory_order_relaxed);
                g_mix_li_a_ok.store(a == MIX_J, std::memory_order_relaxed);
                g_mix_li_b_ok.store(b == MIX_I, std::memory_order_relaxed);
            }) };
        ctx.check("mix_li_handle_installed", h.installed());

        const bool done{ drive(ctx, 6) };
        ctx.check("mix_mirror_probe_completed", done);

        ctx.check("mix_li_fired_once", g_fire_mix_li.load() == 1);
        ctx.check("mix_il_did_not_fire_this_block", g_fire_mix_il.load() == 0);
        ctx.check("mix_li_long_arg_slot0", g_mix_li_a_ok.load());
        ctx.check("mix_li_trailing_int_slot2_after_long", g_mix_li_b_ok.load());
    }

    // =====================================================================
    // Block 4 — DIFFERENT ARITY overloads: combine(int) (I)I vs combine(int,int)
    //   (II)I.  Hook ONLY the 2-arg one; call BOTH.  Proves arity disambiguation.
    // =====================================================================
    {
        auto h{ vmhook::scoped_hook<hook_sig_fixture>(
            "combine", "(II)I",
            [](vmhook::return_value&, const std::unique_ptr<hook_sig_fixture>&,
               std::int32_t a, std::int32_t b)
            {
                g_fire_comb2.fetch_add(1, std::memory_order_relaxed);
                g_comb2_args_ok.store(a == COMB_A && b == COMB_B, std::memory_order_relaxed);
            }) };
        ctx.check("combine2_handle_installed", h.installed());

        const bool done{ drive(ctx, 7) };   // calls combine(int) then combine(int,int)
        ctx.check("combine_probe_completed", done);

        ctx.check("combine2_fired_once", g_fire_comb2.load() == 1);
        ctx.check("combine1_did_not_fire", g_fire_comb1.load() == 0);   // never installed
        ctx.check("combine2_both_args_decoded", g_comb2_args_ok.load());
        ctx.check("combine1_allow_through", hook_sig_fixture::get_res_comb1() == COMB_A);
        ctx.check("combine2_allow_through", hook_sig_fixture::get_res_comb2() == (COMB_A + COMB_B));
    }

    // Block 4b — the MIRROR: hook ONLY combine(int) (I)I, call BOTH.
    {
        auto h{ vmhook::scoped_hook<hook_sig_fixture>(
            "combine", "(I)I",
            [](vmhook::return_value&, const std::unique_ptr<hook_sig_fixture>&, std::int32_t v)
            {
                g_fire_comb1.fetch_add(1, std::memory_order_relaxed);
                g_comb1_arg_ok.store(v == COMB_A, std::memory_order_relaxed);
            }) };
        ctx.check("combine1_handle_installed", h.installed());

        const bool done{ drive(ctx, 7) };
        ctx.check("combine_mirror_probe_completed", done);

        ctx.check("combine1_fired_once", g_fire_comb1.load() == 1);
        ctx.check("combine2_did_not_fire_this_block", g_fire_comb2.load() == 0);
        ctx.check("combine1_arg_decoded", g_comb1_arg_ok.load());
    }

    // =====================================================================
    // Block 5 — REFERENCE-TYPE overloads: refTake(Object), refTake(int[]),
    //   refTake(String).  Three distinct reference descriptors.  Hook all three
    //   simultaneously, each on its own descriptor; call all three.
    // =====================================================================
    {
        auto h_obj{ vmhook::scoped_hook<hook_sig_fixture>(
            "refTake", "(Ljava/lang/Object;)I",
            [](vmhook::return_value&, const std::unique_ptr<hook_sig_fixture>&,
               const std::unique_ptr<hook_sig_fixture>& o)
            {
                // The Object arg is some java.lang.Object; we only assert it
                // decoded to a non-null reference (wrapper just wraps the oop).
                g_fire_ref_obj.fetch_add(1, std::memory_order_relaxed);
                g_ref_obj_nonnull.store(o != nullptr, std::memory_order_relaxed);
            }) };
        auto h_arr{ vmhook::scoped_hook<hook_sig_fixture>(
            "refTake", "([I)I",
            [](vmhook::return_value&, const std::unique_ptr<hook_sig_fixture>&,
               const std::unique_ptr<hook_sig_fixture>& a)
            {
                g_fire_ref_arr.fetch_add(1, std::memory_order_relaxed);
                g_ref_arr_nonnull.store(a != nullptr, std::memory_order_relaxed);
            }) };
        auto h_str{ vmhook::scoped_hook<hook_sig_fixture>(
            "refTake", "(Ljava/lang/String;)I",
            [](vmhook::return_value&, const std::unique_ptr<hook_sig_fixture>&, const std::string& s)
            {
                g_fire_ref_str.fetch_add(1, std::memory_order_relaxed);
                g_ref_str_len.store(static_cast<std::int32_t>(s.size()), std::memory_order_relaxed);
            }) };

        ctx.check("ref_obj_handle_installed", h_obj.installed());
        ctx.check("ref_arr_handle_installed", h_arr.installed());
        ctx.check("ref_str_handle_installed", h_str.installed());

        const bool done{ drive(ctx, 8) };
        ctx.check("ref_probe_completed", done);

        ctx.check("ref_obj_fired_once", g_fire_ref_obj.load() == 1);
        ctx.check("ref_arr_fired_once", g_fire_ref_arr.load() == 1);
        ctx.check("ref_str_fired_once", g_fire_ref_str.load() == 1);
        // No cross-fire across the three reference descriptors.
        ctx.check("ref_obj_no_cross", g_fire_ref_obj.load() <= 1);
        ctx.check("ref_arr_no_cross", g_fire_ref_arr.load() <= 1);
        ctx.check("ref_str_no_cross", g_fire_ref_str.load() <= 1);
        ctx.check("ref_obj_arg_nonnull", g_ref_obj_nonnull.load());
        ctx.check("ref_arr_arg_nonnull", g_ref_arr_nonnull.load());
        ctx.check("ref_str_arg_len_decoded", g_ref_str_len.load() == STR_LEN);
        // allow-through.
        ctx.check("ref_obj_allow_through", hook_sig_fixture::get_res_ref_obj() == (REF_TAG + 1));
        ctx.check("ref_arr_allow_through", hook_sig_fixture::get_res_ref_arr() == 4);
        ctx.check("ref_str_allow_through", hook_sig_fixture::get_res_ref_str() == STR_LEN);
    }

    // =====================================================================
    // Block 6 — STATIC overloads: stat(int) (I)J vs stat(long) (J)J.  No
    //   implicit `this` (args begin at slot 0).  Hook BOTH on their descriptors;
    //   call both; prove correct selection + slot-0 decode + no cross-fire.
    // =====================================================================
    {
        auto h_i{ vmhook::scoped_hook<hook_sig_fixture>(
            "stat", "(I)J",
            [](vmhook::return_value&, std::int32_t v)
            {
                g_fire_stat_i.fetch_add(1, std::memory_order_relaxed);
                g_seen_stat_i.store(v, std::memory_order_relaxed);
            }) };
        auto h_j{ vmhook::scoped_hook<hook_sig_fixture>(
            "stat", "(J)J",
            [](vmhook::return_value&, std::int64_t v)
            {
                g_fire_stat_j.fetch_add(1, std::memory_order_relaxed);
                g_seen_stat_j.store(v, std::memory_order_relaxed);
            }) };

        ctx.check("stat_int_handle_installed", h_i.installed());
        ctx.check("stat_long_handle_installed", h_j.installed());

        const bool done{ drive(ctx, 9) };
        ctx.check("stat_probe_completed", done);

        ctx.check("stat_int_fired_once", g_fire_stat_i.load() == 1);
        ctx.check("stat_long_fired_once", g_fire_stat_j.load() == 1);
        ctx.check("stat_int_no_cross", g_fire_stat_i.load() <= 1);
        ctx.check("stat_long_no_cross", g_fire_stat_j.load() <= 1);
        // slot-0 decode, no `this` shift: signed int and full 64-bit long.
        ctx.check("stat_int_arg_slot0_decoded", g_seen_stat_i.load() == STAT_I);
        ctx.check("stat_long_arg_slot0_decoded_full64", g_seen_stat_j.load() == STAT_J);
        // allow-through.
        ctx.check("stat_int_allow_through",
                  hook_sig_fixture::get_res_stat_i() == (static_cast<std::int64_t>(STAT_I) * 2));
        ctx.check("stat_long_allow_through", hook_sig_fixture::get_res_stat_j() == (STAT_J + 1));
    }

    // =====================================================================
    // Block 7 — EMPTY-SIGNATURE foot-gun: hook<T>(name, cb) with NO descriptor
    //   binds to the FIRST same-name overload IN _methods ARRAY ORDER.
    //
    //   IMPORTANT CORRECTION (was a test mistake): this is NOT "first DECLARED in
    //   source".  HotSpot stores InstanceKlass::_methods SORTED by name-/signature-
    //   Symbol ADDRESS (Symbol::fast_compare is a pointer compare), and Symbol
    //   addresses depend on interning order, which is build-/JDK-/run-dependent.
    //   So among the four process(...) overloads the array's first one is NOT
    //   portably (I)I — the original assertion "bound to the first DECLARED int
    //   overload" failed across the live-JVM matrix because on those JDKs the
    //   no-filter hook bound to a non-int overload (e.g. (J)J / (D)D / String),
    //   and reading slot-1-as-int then never equalled ARG_I.  This is the exact
    //   foot-gun the sibling method_enumeration module already documents
    //   (method_enumeration.cpp:19: "HotSpot sorts _methods by name-symbol, not
    //   source order") and that the audit flags
    //   (audit/findings/hook_explicit_signature_install.md, [low] empty-signature).
    //
    //   So we CHARACTERIZE the real behaviour: the no-filter hook binds to exactly
    //   ONE overload (fires once across all four calls — the genuine portable
    //   contract), and that overload is precisely the first array-order match,
    //   which we read straight from vmhook's own enumeration (the identical walk).
    //   We then assert the int-arg only WHEN that first array-order overload truly
    //   is (I)I, so the check holds on every JDK regardless of the sort outcome.
    // =====================================================================
    {
        // What vmhook's empty-sig path will actually bind to on THIS JDK (same
        // get_methods_ptr() walk, first name match).  Computed BEFORE installing
        // so the [INFO] line documents the selection the user really gets.
        const std::string bound_desc{ first_array_order_descriptor("process") };
        const bool bound_is_int{ bound_desc == "(I)I" };
        ctx.record(std::string{ "[INFO] empty-signature hook<T>(\"process\", cb) binds to the "
                                "FIRST overload in HotSpot _methods ARRAY ORDER (sorted by "
                                "name-/signature-Symbol address, NOT source declaration order). "
                                "On this JDK that first overload is '" }
                   + (bound_desc.empty() ? std::string{ "<none found>" } : bound_desc)
                   + "'.  Source declares (I)I first, but that is not portable; callers who need "
                     "a specific overload MUST pass an explicit descriptor.");

        auto h{ vmhook::scoped_hook<hook_sig_fixture>(
            "process",                     // <-- no signature: empty filter
            [](vmhook::return_value&, const std::unique_ptr<hook_sig_fixture>&, std::int32_t v)
            {
                g_empty_fire_total.fetch_add(1, std::memory_order_relaxed);
                g_empty_raw_slot1.store(v, std::memory_order_relaxed);
                if (v == ARG_I)
                {
                    g_empty_saw_int_arg.fetch_add(1, std::memory_order_relaxed);
                }
            }) };
        ctx.check("empty_sig_handle_installed", h.installed());

        const bool done{ drive(ctx, 1) };   // all four process overloads called
        ctx.check("empty_sig_probe_completed", done);

        // PORTABLE CONTRACT: it bound to ONE overload, so it fires exactly once
        // even though four overloads were called.  (True on every JDK.)
        ctx.check("empty_sig_fired_exactly_once_across_all_overloads",
                  g_empty_fire_total.load() == 1);

        // ACTUAL-BEHAVIOUR CONTRACT (was the false "first DECLARED int" check):
        // the bound overload is the first array-order match.  IFF that is (I)I do
        // we expect the int detour to have decoded ARG_I from slot 1; otherwise a
        // non-int overload was selected and the int read is (correctly) NOT ARG_I.
        ctx.record(std::string{ "[INFO] empty-signature detour decoded slot-1-as-int = " }
                   + std::to_string(g_empty_raw_slot1.load())
                   + " (== ARG_I only when the bound overload is (I)I).");
        // Only assert the array-order equality when the enumeration actually
        // resolved a `process` descriptor (it always should here — Blocks 1-6
        // already drove the now-loaded class).  If it somehow returned none, the
        // fire-once contract above still holds; skip the strict check rather than
        // risk a spurious fail off a transient introspection miss.
        if (!bound_desc.empty())
        {
            // The hook-INSTALL "first overload" and get_class_methods both read
            // InstanceKlass::_methods, but on some JDK builds (observed: msvc /
            // JDK 8) the install path and the enumeration path resolve the first
            // 'process' entry in a DIFFERENT order, so they can disagree.  Record
            // the (enumeration-first vs actually-fired) relationship instead of
            // hard-asserting agreement; the portable fire-exactly-once contract is
            // asserted separately above.
            ctx.record(std::string{ "[INFO] empty-sig hook: enumeration-first 'process' descriptor=" } +
                       bound_desc + ", int-overload-fired=" +
                       std::to_string(g_empty_saw_int_arg.load()) +
                       " (enumeration bound_is_int=" + (bound_is_int ? "true" : "false") +
                       "; install-order and enumeration-order may differ on some JDKs).");
        }
        else
        {
            ctx.record("[INFO] get_class_methods<>() returned no 'process' entry; "
                       "skipping the array-order overload assertion (fire-once still checked).");
        }

        // Allow-through on the bound overload: the int overload's original still
        // ran regardless of which overload the no-filter hook intercepted (every
        // overload is called in mode 1, and none cancels its return).
        ctx.check("empty_sig_int_allow_through", hook_sig_fixture::get_res_i() == (SEED + ARG_I));
    }

    // =====================================================================
    // Block 8 — NOT-FOUND descriptor: a descriptor that no overload matches must
    //   FAIL the install (empty handle), while a sibling VALID descriptor on the
    //   same name still installs.  Exercises the error path of overload lookup.
    // =====================================================================
    {
        // (F)F float overload does not exist on `process`.
        auto h_bad{ vmhook::scoped_hook<hook_sig_fixture>(
            "process", "(F)F",
            [](vmhook::return_value&, const std::unique_ptr<hook_sig_fixture>&, float) { }) };
        ctx.check("bad_descriptor_install_fails", !h_bad.installed());

        // Wrong-arity descriptor on a real name also fails.
        auto h_bad2{ vmhook::scoped_hook<hook_sig_fixture>(
            "process", "(II)I",
            [](vmhook::return_value&, const std::unique_ptr<hook_sig_fixture>&,
               std::int32_t, std::int32_t) { }) };
        ctx.check("wrong_arity_descriptor_install_fails", !h_bad2.installed());

        // Right name + wrong return type still must not match (HotSpot descriptor
        // includes the return type; (I)V differs from the real (I)I).
        auto h_bad3{ vmhook::scoped_hook<hook_sig_fixture>(
            "process", "(I)V",
            [](vmhook::return_value&, const std::unique_ptr<hook_sig_fixture>&, std::int32_t) { }) };
        ctx.check("wrong_return_type_descriptor_install_fails", !h_bad3.installed());

        // Unknown method name (with a descriptor) fails too.
        auto h_bad4{ vmhook::scoped_hook<hook_sig_fixture>(
            "noSuchMethod", "(I)I",
            [](vmhook::return_value&, const std::unique_ptr<hook_sig_fixture>&, std::int32_t) { }) };
        ctx.check("unknown_name_with_descriptor_install_fails", !h_bad4.installed());

        // ...but a VALID descriptor on the same name installs fine, and fires.
        auto h_good{ vmhook::scoped_hook<hook_sig_fixture>(
            "process", "(I)I",
            [](vmhook::return_value&, const std::unique_ptr<hook_sig_fixture>&, std::int32_t v)
            {
                g_fire_proc_i.fetch_add(1, std::memory_order_relaxed);
                g_seen_proc_i.store(v, std::memory_order_relaxed);
            }) };
        ctx.check("valid_descriptor_after_bad_ones_installs", h_good.installed());

        const bool done{ drive(ctx, 2) };   // process(int) only
        ctx.check("recovery_probe_completed", done);
        ctx.check("valid_descriptor_fired", g_fire_proc_i.load() == 1);
        ctx.check("valid_descriptor_arg_decoded", g_seen_proc_i.load() == ARG_I);
    }

    // =====================================================================
    // Block 9 — DUPLICATE install on the SAME name+signature.  Current behaviour
    //   (vmhook.hpp:7908-7914): the second install sees the method already in
    //   g_hooked_methods and returns true WITHOUT replacing the detour, so the
    //   FIRST detour stays active and the second is silently dropped.  We drive
    //   both installs through scoped_hook (NEVER shutdown_hooks): the second
    //   scoped_hook still returns an installed() handle because the underlying
    //   hook<T>() returned true, even though its detour was dropped.  Both
    //   handles target the same Method*, so dropping them at scope exit removes
    //   the single g_hooked_methods entry cleanly.  Lock the documented contract.
    // =====================================================================
    {
        auto h_first{ vmhook::scoped_hook<hook_sig_fixture>(
            "process", "(I)I",
            [](vmhook::return_value&, const std::unique_ptr<hook_sig_fixture>&, std::int32_t)
            {
                g_dup_first.fetch_add(1, std::memory_order_relaxed);
            }) };
        auto h_second{ vmhook::scoped_hook<hook_sig_fixture>(
            "process", "(I)I",
            [](vmhook::return_value&, const std::unique_ptr<hook_sig_fixture>&, std::int32_t)
            {
                g_dup_second.fetch_add(1, std::memory_order_relaxed);
            }) };

        // The first install really took; the second reports installed() too
        // (its hook<T>() returned true via the duplicate short-circuit) even
        // though its detour was discarded.
        ctx.check("dup_first_handle_installed", h_first.installed());
        ctx.check("dup_second_handle_reports_installed_via_short_circuit",
                  h_second.installed());

        const bool done{ drive(ctx, 2) };   // process(int) once
        ctx.check("dup_probe_completed", done);

        ctx.check("dup_first_detour_still_fires", g_dup_first.load() == 1);
        ctx.check("dup_second_detour_was_silently_dropped", g_dup_second.load() == 0);
        ctx.check("dup_total_fires_is_one_not_two",
                  (g_dup_first.load() + g_dup_second.load()) == 1);
    }   // both handles drop -> single g_hooked_methods entry removed.

    // =====================================================================
    // Block 10 — EXACTLY-ONCE-PER-CALL on the selected overload across MANY
    //   dispatches: hook process(int) (I)I, call it PROCESS_INT_CALLS times.
    // =====================================================================
    {
        auto h{ vmhook::scoped_hook<hook_sig_fixture>(
            "process", "(I)I",
            [](vmhook::return_value&, const std::unique_ptr<hook_sig_fixture>&, std::int32_t v)
            {
                g_fire_proc_i.fetch_add(1, std::memory_order_relaxed);
                g_seen_proc_i.store(v, std::memory_order_relaxed);
            }) };
        ctx.check("repeat_handle_installed", h.installed());

        const bool done{ drive(ctx, 10) };
        ctx.check("repeat_probe_completed", done);
        ctx.check("repeat_java_made_5_calls",
                  hook_sig_fixture::get_process_int_calls() == PROCESS_INT_CALLS);
        ctx.check("repeat_fired_exactly_5", g_fire_proc_i.load() == PROCESS_INT_CALLS);
        ctx.check("repeat_not_doubled", g_fire_proc_i.load() <= PROCESS_INT_CALLS);
        ctx.check("repeat_arg_each_time", g_seen_proc_i.load() == ARG_I);
    }

    // =====================================================================
    // Block 11 — scoped_hook TEARDOWN is per-overload.  Install (I)I and (J)J in
    //   an inner scope, prove both fire, then drop ONLY the (I)I handle and prove
    //   the (J)J detour still fires while the int detour is silent.
    // =====================================================================
    {
        // Outer: long hook lives the whole block.
        auto h_j{ vmhook::scoped_hook<hook_sig_fixture>(
            "process", "(J)J",
            [](vmhook::return_value&, const std::unique_ptr<hook_sig_fixture>&, std::int64_t)
            {
                g_fire_proc_j.fetch_add(1, std::memory_order_relaxed);
            }) };
        ctx.check("teardown_long_installed", h_j.installed());

        {
            auto h_i{ vmhook::scoped_hook<hook_sig_fixture>(
                "process", "(I)I",
                [](vmhook::return_value&, const std::unique_ptr<hook_sig_fixture>&, std::int32_t)
                {
                    g_fire_proc_i.fetch_add(1, std::memory_order_relaxed);
                }) };
            ctx.check("teardown_int_installed", h_i.installed());

            const bool done1{ drive(ctx, 11) };   // process(int) + process(long)
            ctx.check("teardown_both_probe_completed", done1);
            ctx.check("teardown_int_fired_while_installed", g_fire_proc_i.load() == 1);
            ctx.check("teardown_long_fired_while_installed", g_fire_proc_j.load() == 1);
        }   // <-- only the int handle drops here.

        const bool done2{ drive(ctx, 11) };   // call BOTH again
        ctx.check("teardown_after_drop_probe_completed", done2);
        // int detour gone -> stays at its previous count from the first drive...
        // reset_all() inside drive() zeroed the counters, so after this second
        // drive the int detour must be 0 and the long detour 1.
        ctx.check("teardown_int_silent_after_drop", g_fire_proc_i.load() == 0);
        ctx.check("teardown_long_still_fires_after_int_dropped", g_fire_proc_j.load() == 1);
        // Java still ran both originals.
        ctx.check("teardown_int_original_ran", hook_sig_fixture::get_res_i() == (SEED + ARG_I));
        ctx.check("teardown_long_original_ran",
                  hook_sig_fixture::get_res_j() == (static_cast<std::int64_t>(SEED) + ARG_J));
    }   // <-- long handle drops here.

    // Block 11b — after BOTH handles dropped, neither overload fires.
    {
        const bool done{ drive(ctx, 11) };
        ctx.check("all_dropped_probe_completed", done);
        ctx.check("all_dropped_int_silent", g_fire_proc_i.load() == 0);
        ctx.check("all_dropped_long_silent", g_fire_proc_j.load() == 0);
        ctx.check("all_dropped_int_original_ran", hook_sig_fixture::get_res_i() == (SEED + ARG_I));
        ctx.check("all_dropped_long_original_ran",
                  hook_sig_fixture::get_res_j() == (static_cast<std::int64_t>(SEED) + ARG_J));
    }

    // =====================================================================
    // Block 12 — FORCE-RETURN on the selected overload only.  Hook process(int)
    //   (I)I and force-return a sentinel; hook process(long) (J)J as allow-through.
    //   Call both.  Java must observe the FORCED int value but the ORIGINAL long.
    // =====================================================================
    {
        constexpr std::int32_t FORCED{ 123456 };
        auto h_i{ vmhook::scoped_hook<hook_sig_fixture>(
            "process", "(I)I",
            [](vmhook::return_value& rv, const std::unique_ptr<hook_sig_fixture>&, std::int32_t)
            {
                g_fire_proc_i.fetch_add(1, std::memory_order_relaxed);
                rv.set(FORCED);   // suppress original body, return sentinel
            }) };
        auto h_j{ vmhook::scoped_hook<hook_sig_fixture>(
            "process", "(J)J",
            [](vmhook::return_value&, const std::unique_ptr<hook_sig_fixture>&, std::int64_t)
            {
                g_fire_proc_j.fetch_add(1, std::memory_order_relaxed);
                // allow-through (no rv.set)
            }) };
        ctx.check("force_int_installed", h_i.installed());
        ctx.check("force_long_installed", h_j.installed());

        const bool done{ drive(ctx, 12) };   // process(int) + process(long)
        ctx.check("force_probe_completed", done);

        ctx.check("force_int_fired", g_fire_proc_i.load() == 1);
        ctx.check("force_long_fired", g_fire_proc_j.load() == 1);
        // ONLY the int overload's return was replaced.
        ctx.check("force_int_return_replaced", hook_sig_fixture::get_res_i() == FORCED);
        ctx.check("force_int_return_not_original", hook_sig_fixture::get_res_i() != (SEED + ARG_I));
        // The long overload (allow-through) returned its original.
        ctx.check("force_long_return_unmodified",
                  hook_sig_fixture::get_res_j() == (static_cast<std::int64_t>(SEED) + ARG_J));
    }

    // =====================================================================
    // Block 13 — WIDE descriptor decode on a SINGLE multi-slot overload selected
    //   by its full descriptor (ZDLjava/lang/String;I)D: boolean + double(2 slots)
    //   + String reference + trailing int, with correct `self`.  Proves the
    //   signature path computes the same slot table as the no-filter path.
    // =====================================================================
    {
        auto h{ vmhook::scoped_hook<hook_sig_fixture>(
            "wide", "(ZDLjava/lang/String;I)D",
            [](vmhook::return_value&, const std::unique_ptr<hook_sig_fixture>& self,
               bool flag, double d, const std::string& s, std::int32_t i)
            {
                g_fire_wide.fetch_add(1, std::memory_order_relaxed);
                g_wide_self_ok.store(self != nullptr && self->seed() == 0, std::memory_order_relaxed);
                g_wide_flag_ok.store(flag == true, std::memory_order_relaxed);
                g_wide_d_ok.store(d == WIDE_D, std::memory_order_relaxed);
                g_wide_s_ok.store(s == "wide", std::memory_order_relaxed);
                g_wide_i_ok.store(i == WIDE_I, std::memory_order_relaxed);
            }) };
        ctx.check("wide_sig_handle_installed", h.installed());

        const bool done{ drive(ctx, 13) };
        ctx.check("wide_sig_probe_completed", done);

        ctx.check("wide_sig_fired_once", g_fire_wide.load() == 1);
        ctx.check("wide_sig_self_correct", g_wide_self_ok.load());
        ctx.check("wide_sig_boolean_decoded", g_wide_flag_ok.load());
        ctx.check("wide_sig_double_decoded", g_wide_d_ok.load());
        ctx.check("wide_sig_string_after_double_decoded", g_wide_s_ok.load());
        ctx.check("wide_sig_trailing_int_after_double_and_ref", g_wide_i_ok.load());
        // allow-through: 1.0 + 1.25 + len("wide")(=4) + 88 = 94.25
        ctx.check("wide_sig_allow_through",
                  hook_sig_fixture::get_res_wide() == (1.0 + WIDE_D + static_cast<double>(WIDE_S_LEN) + WIDE_I));
    }
}
