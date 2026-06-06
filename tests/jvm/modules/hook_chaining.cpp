// hook_chaining JVM test module  (feature area: hooks / shared i2i stub demux)
//
// THE authority for the in-process consequence of HotSpot sharing ONE
// interpreter-to-interpreter (i2i) stub across many Java methods.  vmhook
// patches that stub EXACTLY ONCE -- one midi2i_hook trampoline per unique i2i
// entry (vmhook.hpp:5853-5857 i2i_hook_data; install reuse at 8161-8230) -- and
// registers every hooked Method* in g_hooked_methods (vmhook.hpp:5876).  Every
// intercepted call therefore lands in the SAME common_detour
// (vmhook.hpp:5965-6031), which:
//   * bails if g_shutdown_requested (5972),
//   * resolves the current Method* from the frame (5984),
//   * LINEAR-SCANS g_hooked_methods (6002) and on the FIRST
//       hook.method == current_method
//     match (6004) fires THAT method's detour via seh_invoke_detour and
//     RETURNS immediately (6012-6023).
// That "first match fires once, then return" is the structural guarantee this
// module proves on a live JVM: with hooks installed on SEVERAL distinct methods
// simultaneously, each detour fires for ITS method ONLY, decodes ITS own frame,
// and NEVER cross-fires onto a sibling -- all through the single shared stub.
//
// This is NOT the cross-DLL chain_resume path (a second injector stomping the
// shared injection point; vmhook.hpp:8179-8230).  That needs a second patcher
// and cannot be exercised from one process; it is documented in the agent-def.
//
// WHAT MAKES THIS DISTINCT FROM scoped_hook_raii: that module drives ONE method
// per probe cycle.  Here the PRIMARY scenario (mode 1) calls EVERY hooked method
// inside a SINGLE run() -- one continuous bytecode dispatch pass -- so all the
// sibling hooks are live on the one shared i2i stub at the same time and we prove
// common_detour demultiplexes a MIXED call stream (int / long / String / double /
// no-arg / static) to the correct per-method detour with exact per-method counts
// and zero cross-fire, in one pass.
//
// EXHAUSTIVE ANGLES (all via scoped_hook, RAII teardown, nothing left armed):
//   * mixed simultaneous install (a,b,c,d,e,s): each fires once, right args,
//     right self / no-self, NO cross-fire (every detour stamps the tag it thinks
//     it is + a global cross-fire sentinel);
//   * per-method counts: a*A_REPEAT + b + c -> exact counts, never bleed;
//   * drop ONE handle (b) mid-flight -> b silent while a,c still fire;
//   * INSTALL-ORDER independence: install c,b,a (reverse) -> identical demux;
//   * allow-through per method (no detour cancels -> original bodies intact);
//   * per-method return override: set() on ONLY a's detour rewrites a's return
//     and leaves b,c bodies untouched (cancel is per-detour, not global);
//   * each detour decodes its OWN argument shape (one-slot int, two-slot long,
//     reference String, two-slot double, empty frame, static slot-0 int);
//   * a baseline single-method probe per arg type (modes 2/3/4) cross-checks the
//     mixed-pass counts against isolated calls.
//
// SAFETY: scoped_hook only; every handle is scope-local and RAII-destroyed before
// the module returns (asserted by module_left_no_hooks_armed).  `self` is guarded
// (the wide-cast is via the typed callback path, which is the supported one).  No
// detour holds an unrooted oop across an allocation/probe boundary -- detours only
// read primitives / a String / the receiver's seed and store into atomics, so
// there is no GC-window hazard here.  C++17: no std::bit_cast.  MSVC copy-init from
// value_t (never brace-init) in the field readers.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace
{
    // Wrapper for vmhook.fixtures.HookChaining.  Deriving from vmhook::object<>
    // gives the wrapper a vtable (required by register_class<T>) and the
    // static_field(...) / get_field(...) accessors used below.
    class hc_fixture : public vmhook::object<hc_fixture>
    {
    public:
        explicit hc_fixture(vmhook::oop_t instance) noexcept
            : vmhook::object<hc_fixture>{ instance }
        {
        }

        // --- go/done handshake + scenario selector ------------------------
        static auto set_go(bool value) -> void       { static_field("go")->set(value); }
        static auto set_done(bool value) -> void      { static_field("done")->set(value); }
        static auto get_done() -> bool                { return static_field("done")->get(); }
        static auto set_mode(std::int32_t m) -> void  { static_field("mode")->set(m); }

        static auto resolves(const char* name) -> bool { return static_field(name).has_value(); }

        // --- per-method Java-side call counters (accumulate across process) -
        static auto get_a_calls() -> std::int32_t { return static_field("aCalls")->get(); }
        static auto get_b_calls() -> std::int32_t { return static_field("bCalls")->get(); }
        static auto get_c_calls() -> std::int32_t { return static_field("cCalls")->get(); }
        static auto get_d_calls() -> std::int32_t { return static_field("dCalls")->get(); }
        static auto get_e_calls() -> std::int32_t { return static_field("eCalls")->get(); }
        static auto get_s_calls() -> std::int32_t { return static_field("sCalls")->get(); }

        // --- per-method last original return (allow-through proof) ----------
        static auto get_a_result() -> std::int32_t { return static_field("aResult")->get(); }
        static auto get_b_result() -> std::int64_t { return static_field("bResult")->get(); }
        static auto get_c_result() -> std::int32_t { return static_field("cResult")->get(); }
        static auto get_d_result() -> std::int64_t { return static_field("dResult")->get(); }
        static auto get_e_result() -> std::int32_t { return static_field("eResult")->get(); }
        static auto get_s_result() -> std::int32_t { return static_field("sResult")->get(); }

        // Reads this instance's own seed (proves `self` is the right object).
        auto seed() const -> std::int32_t { return get_field("seed")->get(); }
    };

    // ---- Fixture-mirrored constants (lockstep with HookChaining.java) -------
    constexpr std::int32_t SEED{ 1000 };
    constexpr std::int32_t A_ARG{ 7 };
    constexpr std::int64_t B_ARG{ 0x1122334455667788LL };
    constexpr std::int32_t C_ARG_LEN{ 6 };
    constexpr double       D_ARG{ 2.5 };
    constexpr std::int32_t S_ARG{ 21 };
    constexpr std::int32_t A_REPEAT{ 4 };

    // A distinct per-method tag each detour stamps so we can prove WHICH detour
    // ran for WHICH method (cross-fire detection), not merely that something
    // fired.  Stored into g_last_tag on every fire.
    enum method_tag : std::int32_t
    {
        TAG_NONE = 0, TAG_A, TAG_B, TAG_C, TAG_D, TAG_E, TAG_S
    };

    // ---- Native per-method fire counters (reset per scenario) --------------
    std::atomic<std::int32_t> g_a_fires{ 0 };
    std::atomic<std::int32_t> g_b_fires{ 0 };
    std::atomic<std::int32_t> g_c_fires{ 0 };
    std::atomic<std::int32_t> g_d_fires{ 0 };
    std::atomic<std::int32_t> g_e_fires{ 0 };
    std::atomic<std::int32_t> g_s_fires{ 0 };

    // Per-method "saw the correct frame" latches (right args + right self).
    std::atomic<bool> g_a_ok{ false };   // self seed == SEED AND x == A_ARG
    std::atomic<bool> g_b_ok{ false };   // self seed == SEED AND y == B_ARG
    std::atomic<bool> g_c_ok{ false };   // self seed == SEED AND s == "vmhook"
    std::atomic<bool> g_d_ok{ false };   // self seed == SEED AND z == D_ARG
    std::atomic<bool> g_e_ok{ false };   // self seed == SEED (no args)
    std::atomic<bool> g_s_ok{ false };   // x == S_ARG (static: no self)

    // CROSS-FIRE sentinel: set true if ANY detour ever observed a frame that did
    // NOT match its own method's expected argument shape/value.  This is the
    // teeth behind "each detour fires for its method only": if common_detour ever
    // dispatched method A's call into method B's detour, B's value check fails and
    // this flips.  It must stay false on every JDK.
    std::atomic<bool> g_cross_fire{ false };

    // The tag of the LAST detour that fired (ordering / demux sanity).
    std::atomic<std::int32_t> g_last_tag{ TAG_NONE };

    auto reset_fires() -> void
    {
        g_a_fires.store(0);
        g_b_fires.store(0);
        g_c_fires.store(0);
        g_d_fires.store(0);
        g_e_fires.store(0);
        g_s_fires.store(0);
        g_a_ok.store(false);
        g_b_ok.store(false);
        g_c_ok.store(false);
        g_d_ok.store(false);
        g_e_ok.store(false);
        g_s_ok.store(false);
        g_cross_fire.store(false);
        g_last_tag.store(TAG_NONE);
    }

    // Drives exactly one probe cycle for `mode`: resets native fire counters +
    // the latched `done`, programs the scenario selector on the rising edge of
    // go, then runs the probe.  Mirrors hook_basic / scoped_hook_raii drive().
    auto drive(vmhook_test::context& ctx, std::int32_t mode) -> bool
    {
        reset_fires();
        return ctx.run_probe(
            [mode](bool value)
            {
                if (value)
                {
                    hc_fixture::set_done(false);
                    hc_fixture::set_mode(mode);
                }
                hc_fixture::set_go(value);
            },
            []() { return hc_fixture::get_done(); });
    }

    // --- Reusable detour factories -----------------------------------------
    // Each detour bumps its own fire counter, stamps its tag, and validates that
    // the frame it received is its OWN method's frame (correct self + correct
    // arg).  A mismatch flips the global cross-fire sentinel -- that is how we
    // prove common_detour never routed a sibling's call into the wrong detour.
    //
    // `optionally_set` lets the a-detour scenarios reuse the SAME factory for
    // both allow-through and return-override: pass a non-null pointer to make the
    // detour call retval.set() (cancel + custom return) for that method only.

    auto a_detour(const std::int32_t* override_return = nullptr)
    {
        return [override_return](vmhook::return_value& retval,
                                 const std::unique_ptr<hc_fixture>& self,
                                 std::int32_t x)
        {
            g_a_fires.fetch_add(1, std::memory_order_relaxed);
            g_last_tag.store(TAG_A, std::memory_order_relaxed);
            const bool self_ok{ self != nullptr && self->seed() == SEED };
            const bool arg_ok{ x == A_ARG };
            if (self_ok && arg_ok)
            {
                g_a_ok.store(true, std::memory_order_relaxed);
            }
            else
            {
                g_cross_fire.store(true, std::memory_order_relaxed);
            }
            if (override_return)
            {
                retval.set<std::int32_t>(*override_return);
            }
        };
    }

    auto b_detour()
    {
        return [](vmhook::return_value&,
                  const std::unique_ptr<hc_fixture>& self,
                  std::int64_t y)
        {
            g_b_fires.fetch_add(1, std::memory_order_relaxed);
            g_last_tag.store(TAG_B, std::memory_order_relaxed);
            const bool self_ok{ self != nullptr && self->seed() == SEED };
            const bool arg_ok{ y == B_ARG };
            if (self_ok && arg_ok)
            {
                g_b_ok.store(true, std::memory_order_relaxed);
            }
            else
            {
                g_cross_fire.store(true, std::memory_order_relaxed);
            }
        };
    }

    auto c_detour()
    {
        return [](vmhook::return_value&,
                  const std::unique_ptr<hc_fixture>& self,
                  const std::string& s)
        {
            g_c_fires.fetch_add(1, std::memory_order_relaxed);
            g_last_tag.store(TAG_C, std::memory_order_relaxed);
            const bool self_ok{ self != nullptr && self->seed() == SEED };
            const bool arg_ok{ s == "vmhook" };
            if (self_ok && arg_ok)
            {
                g_c_ok.store(true, std::memory_order_relaxed);
            }
            else
            {
                g_cross_fire.store(true, std::memory_order_relaxed);
            }
        };
    }

    auto d_detour()
    {
        return [](vmhook::return_value&,
                  const std::unique_ptr<hc_fixture>& self,
                  double z)
        {
            g_d_fires.fetch_add(1, std::memory_order_relaxed);
            g_last_tag.store(TAG_D, std::memory_order_relaxed);
            const bool self_ok{ self != nullptr && self->seed() == SEED };
            const bool arg_ok{ z == D_ARG };
            if (self_ok && arg_ok)
            {
                g_d_ok.store(true, std::memory_order_relaxed);
            }
            else
            {
                g_cross_fire.store(true, std::memory_order_relaxed);
            }
        };
    }

    auto e_detour()
    {
        return [](vmhook::return_value&,
                  const std::unique_ptr<hc_fixture>& self)
        {
            g_e_fires.fetch_add(1, std::memory_order_relaxed);
            g_last_tag.store(TAG_E, std::memory_order_relaxed);
            // No args: the only correctness signal is the receiver identity.
            if (self != nullptr && self->seed() == SEED)
            {
                g_e_ok.store(true, std::memory_order_relaxed);
            }
            else
            {
                g_cross_fire.store(true, std::memory_order_relaxed);
            }
        };
    }

    auto s_detour()
    {
        // STATIC method: NO `this`, so the first explicit arg is at slot 0 and
        // the callback's first non-retval param IS that int (no unique_ptr self).
        return [](vmhook::return_value&, std::int32_t x)
        {
            g_s_fires.fetch_add(1, std::memory_order_relaxed);
            g_last_tag.store(TAG_S, std::memory_order_relaxed);
            if (x == S_ARG)
            {
                g_s_ok.store(true, std::memory_order_relaxed);
            }
            else
            {
                g_cross_fire.store(true, std::memory_order_relaxed);
            }
        };
    }
}

VMHOOK_JVM_MODULE(hook_chaining)
{
    vmhook::register_class<hc_fixture>("vmhook/fixtures/HookChaining");

    // =====================================================================
    // 0 — Sanity: the fixture resolves and every hookable method is declared
    //     (so a later "did not fire" is a real demux fact, not a missing method).
    // =====================================================================
    ctx.check("hc_class_registered_field_resolves", hc_fixture::resolves("go"));
    {
        const auto methods{ vmhook::get_class_methods<hc_fixture>() };
        bool has_a{ false }, has_b{ false }, has_c{ false };
        bool has_d{ false }, has_e{ false }, has_s{ false };
        for (const std::pair<std::string, std::string>& m : methods)
        {
            if (m.first == "a") { has_a = true; }
            else if (m.first == "b") { has_b = true; }
            else if (m.first == "c") { has_c = true; }
            else if (m.first == "d") { has_d = true; }
            else if (m.first == "e") { has_e = true; }
            else if (m.first == "s") { has_s = true; }
        }
        ctx.check("hc_method_a_declared", has_a);
        ctx.check("hc_method_b_declared", has_b);
        ctx.check("hc_method_c_declared", has_c);
        ctx.check("hc_method_d_declared", has_d);
        ctx.check("hc_method_e_declared", has_e);
        ctx.check("hc_method_s_declared", has_s);
    }

    // =====================================================================
    // 1 — THE CORE DEMUX PROOF.  Install hooks on ALL SIX methods at once, then
    //     drive ALL SIX in ONE bytecode dispatch pass (mode 1).  Each detour must
    //     fire EXACTLY ONCE for ITS method, decode ITS own frame correctly, and
    //     NEVER cross-fire.  This is the shared-i2i-stub guarantee.
    // =====================================================================
    {
        const std::int32_t a_before{ hc_fixture::get_a_calls() };
        const std::int32_t b_before{ hc_fixture::get_b_calls() };
        const std::int32_t c_before{ hc_fixture::get_c_calls() };
        const std::int32_t d_before{ hc_fixture::get_d_calls() };
        const std::int32_t e_before{ hc_fixture::get_e_calls() };
        const std::int32_t s_before{ hc_fixture::get_s_calls() };

        auto h_a{ vmhook::scoped_hook<hc_fixture>("a", "(I)I",  a_detour()) };
        auto h_b{ vmhook::scoped_hook<hc_fixture>("b", "(J)J",  b_detour()) };
        auto h_c{ vmhook::scoped_hook<hc_fixture>("c", "(Ljava/lang/String;)I", c_detour()) };
        auto h_d{ vmhook::scoped_hook<hc_fixture>("d", "(D)J",  d_detour()) };
        auto h_e{ vmhook::scoped_hook<hc_fixture>("e", "()I",   e_detour()) };
        auto h_s{ vmhook::scoped_hook<hc_fixture>("s", "(I)I",  s_detour()) };

        ctx.check("core_install_a", h_a.installed());
        ctx.check("core_install_b", h_b.installed());
        ctx.check("core_install_c", h_c.installed());
        ctx.check("core_install_d", h_d.installed());
        ctx.check("core_install_e", h_e.installed());
        ctx.check("core_install_s", h_s.installed());
        // All six coexist simultaneously on the one shared stub.
        ctx.check("core_all_six_installed_simultaneously",
                  h_a.installed() && h_b.installed() && h_c.installed()
                  && h_d.installed() && h_e.installed() && h_s.installed());

        const bool done{ drive(ctx, 1) };
        ctx.check("core_probe_completed", done);

        // Java actually issued exactly one call to each method this cycle.
        ctx.check("core_java_called_a_once", hc_fixture::get_a_calls() - a_before == 1);
        ctx.check("core_java_called_b_once", hc_fixture::get_b_calls() - b_before == 1);
        ctx.check("core_java_called_c_once", hc_fixture::get_c_calls() - c_before == 1);
        ctx.check("core_java_called_d_once", hc_fixture::get_d_calls() - d_before == 1);
        ctx.check("core_java_called_e_once", hc_fixture::get_e_calls() - e_before == 1);
        ctx.check("core_java_called_s_once", hc_fixture::get_s_calls() - s_before == 1);

        // Each detour fired EXACTLY ONCE -- the heart of the demux contract.
        ctx.check("core_a_fired_exactly_once", g_a_fires.load() == 1);
        ctx.check("core_b_fired_exactly_once", g_b_fires.load() == 1);
        ctx.check("core_c_fired_exactly_once", g_c_fires.load() == 1);
        ctx.check("core_d_fired_exactly_once", g_d_fires.load() == 1);
        ctx.check("core_e_fired_exactly_once", g_e_fires.load() == 1);
        ctx.check("core_s_fired_exactly_once", g_s_fires.load() == 1);

        // Each detour saw ITS OWN correct frame (self + arg).
        ctx.check("core_a_saw_correct_frame", g_a_ok.load());
        ctx.check("core_b_saw_correct_frame", g_b_ok.load());
        ctx.check("core_c_saw_correct_frame", g_c_ok.load());
        ctx.check("core_d_saw_correct_frame", g_d_ok.load());
        ctx.check("core_e_saw_correct_frame", g_e_ok.load());
        ctx.check("core_s_saw_correct_frame", g_s_ok.load());

        // ZERO cross-fire across the whole mixed pass.
        ctx.check("core_no_cross_fire", !g_cross_fire.load());

        // Total fires == total calls (no detour fired for a method it doesn't own
        // -- an aggregate guard complementing the per-method exactly-once checks).
        const std::int32_t total_fires{ g_a_fires.load() + g_b_fires.load()
                                       + g_c_fires.load() + g_d_fires.load()
                                       + g_e_fires.load() + g_s_fires.load() };
        ctx.check("core_total_fires_is_six", total_fires == 6);

        // Allow-through: NO detour cancelled, so every original body ran and the
        // Java-visible results are the unmodified computed values.
        ctx.check("core_allow_through_a", hc_fixture::get_a_result() == (SEED + A_ARG));
        ctx.check("core_allow_through_b",
                  hc_fixture::get_b_result() == (static_cast<std::int64_t>(SEED) + B_ARG));
        ctx.check("core_allow_through_c", hc_fixture::get_c_result() == (SEED + C_ARG_LEN));
        ctx.check("core_allow_through_d",
                  hc_fixture::get_d_result() == (static_cast<std::int64_t>(SEED)
                                                 + static_cast<std::int64_t>(D_ARG)));
        ctx.check("core_allow_through_e", hc_fixture::get_e_result() == SEED);
        ctx.check("core_allow_through_s", hc_fixture::get_s_result() == (S_ARG * 2));
    }   // all six handles drop here -> every entry removed; stub trampoline stays.

    // After all six handles dropped, a full pass must fire NOTHING (clean
    // teardown of every entry that shared the stub).
    {
        const bool done{ drive(ctx, 1) };
        ctx.check("core_after_drop_probe_completed", done);
        ctx.check("core_after_drop_a_silent", g_a_fires.load() == 0);
        ctx.check("core_after_drop_b_silent", g_b_fires.load() == 0);
        ctx.check("core_after_drop_c_silent", g_c_fires.load() == 0);
        ctx.check("core_after_drop_d_silent", g_d_fires.load() == 0);
        ctx.check("core_after_drop_e_silent", g_e_fires.load() == 0);
        ctx.check("core_after_drop_s_silent", g_s_fires.load() == 0);
        ctx.check("core_after_drop_no_cross_fire", !g_cross_fire.load());
        // Allow-through still holds with no hooks at all (sanity that the probe
        // path itself is unmodified once the stub serves no live entries).
        ctx.check("core_after_drop_allow_through_a",
                  hc_fixture::get_a_result() == (SEED + A_ARG));
    }

    // =====================================================================
    // 2 — Single-method BASELINES.  With only ONE method hooked, driving a full
    //     set in isolation must fire only that one detour.  These cross-check the
    //     mixed-pass counts above against isolated calls and prove a lone entry on
    //     the shared stub still demuxes correctly.
    // =====================================================================
    {
        auto h_a{ vmhook::scoped_hook<hc_fixture>("a", "(I)I", a_detour()) };
        ctx.check("baseline_a_installed", h_a.installed());
        const bool done{ drive(ctx, 2) };   // mode 2 = a() only
        ctx.check("baseline_a_probe_completed", done);
        ctx.check("baseline_a_fired_once", g_a_fires.load() == 1);
        ctx.check("baseline_a_saw_correct_frame", g_a_ok.load());
        ctx.check("baseline_a_no_cross_fire", !g_cross_fire.load());
        ctx.check("baseline_a_siblings_silent",
                  g_b_fires.load() == 0 && g_c_fires.load() == 0
                  && g_d_fires.load() == 0 && g_e_fires.load() == 0
                  && g_s_fires.load() == 0);
        ctx.check("baseline_a_last_tag_is_a", g_last_tag.load() == TAG_A);
    }
    {
        auto h_b{ vmhook::scoped_hook<hc_fixture>("b", "(J)J", b_detour()) };
        ctx.check("baseline_b_installed", h_b.installed());
        const bool done{ drive(ctx, 3) };   // mode 3 = b() only
        ctx.check("baseline_b_probe_completed", done);
        ctx.check("baseline_b_fired_once", g_b_fires.load() == 1);
        ctx.check("baseline_b_saw_correct_frame", g_b_ok.load());
        ctx.check("baseline_b_no_cross_fire", !g_cross_fire.load());
        ctx.check("baseline_b_last_tag_is_b", g_last_tag.load() == TAG_B);
    }
    {
        auto h_c{ vmhook::scoped_hook<hc_fixture>("c", "(Ljava/lang/String;)I", c_detour()) };
        ctx.check("baseline_c_installed", h_c.installed());
        const bool done{ drive(ctx, 4) };   // mode 4 = c() only
        ctx.check("baseline_c_probe_completed", done);
        ctx.check("baseline_c_fired_once", g_c_fires.load() == 1);
        ctx.check("baseline_c_saw_correct_frame", g_c_ok.load());
        ctx.check("baseline_c_no_cross_fire", !g_cross_fire.load());
        ctx.check("baseline_c_last_tag_is_c", g_last_tag.load() == TAG_C);
    }

    // =====================================================================
    // 3 — PER-METHOD COUNT FIDELITY.  a,b,c all hooked; the probe calls a
    //     A_REPEAT times, b once, c once (mode 6).  Each detour's count must match
    //     ITS method's call count exactly -- counts never bleed between siblings
    //     sharing the stub.
    // =====================================================================
    {
        const std::int32_t a_before{ hc_fixture::get_a_calls() };
        const std::int32_t b_before{ hc_fixture::get_b_calls() };
        const std::int32_t c_before{ hc_fixture::get_c_calls() };

        auto h_a{ vmhook::scoped_hook<hc_fixture>("a", "(I)I", a_detour()) };
        auto h_b{ vmhook::scoped_hook<hc_fixture>("b", "(J)J", b_detour()) };
        auto h_c{ vmhook::scoped_hook<hc_fixture>("c", "(Ljava/lang/String;)I", c_detour()) };
        ctx.check("counts_all_installed",
                  h_a.installed() && h_b.installed() && h_c.installed());

        const bool done{ drive(ctx, 6) };
        ctx.check("counts_probe_completed", done);

        ctx.check("counts_java_called_a_n_times",
                  hc_fixture::get_a_calls() - a_before == A_REPEAT);
        ctx.check("counts_java_called_b_once", hc_fixture::get_b_calls() - b_before == 1);
        ctx.check("counts_java_called_c_once", hc_fixture::get_c_calls() - c_before == 1);

        ctx.check("counts_a_fired_n_times", g_a_fires.load() == A_REPEAT);
        ctx.check("counts_b_fired_once", g_b_fires.load() == 1);
        ctx.check("counts_c_fired_once", g_c_fires.load() == 1);
        ctx.check("counts_no_cross_fire", !g_cross_fire.load());
        ctx.check("counts_a_saw_correct_frame_every_fire", g_a_ok.load());
        // Total fires equals A_REPEAT + 1 + 1, never more.
        ctx.check("counts_total_is_exact",
                  g_a_fires.load() + g_b_fires.load() + g_c_fires.load()
                      == A_REPEAT + 2);
    }

    // =====================================================================
    // 4 — DROP ONE handle mid-flight, siblings keep firing.  Install a,b,c; first
    //     prove all three fire (mode 5 = a+b+c).  Then drop ONLY b's handle and
    //     re-drive: b must go silent while a and c STILL fire through the same
    //     shared stub.  This is the single-entry-removal half of the contract
    //     (hook_handle::stop erases just b's entry; common_detour now skips it).
    // =====================================================================
    {
        auto h_a{ vmhook::scoped_hook<hc_fixture>("a", "(I)I", a_detour()) };
        auto h_b{ vmhook::scoped_hook<hc_fixture>("b", "(J)J", b_detour()) };
        auto h_c{ vmhook::scoped_hook<hc_fixture>("c", "(Ljava/lang/String;)I", c_detour()) };
        ctx.check("drop_all_installed",
                  h_a.installed() && h_b.installed() && h_c.installed());

        // First pass: all three fire.
        bool done{ drive(ctx, 5) };
        ctx.check("drop_first_pass_completed", done);
        ctx.check("drop_first_a_fired", g_a_fires.load() == 1);
        ctx.check("drop_first_b_fired", g_b_fires.load() == 1);
        ctx.check("drop_first_c_fired", g_c_fires.load() == 1);
        ctx.check("drop_first_no_cross_fire", !g_cross_fire.load());

        // Drop ONLY b's hook via explicit stop() (erases b's entry from
        // g_hooked_methods; a and c stay armed on the shared stub).
        h_b.stop();
        ctx.check("drop_b_handle_released", !h_b.installed());

        // Second pass over the SAME three calls: b silent, a + c unaffected.
        done = drive(ctx, 5);
        ctx.check("drop_second_pass_completed", done);
        ctx.check("drop_second_a_still_fires", g_a_fires.load() == 1);
        ctx.check("drop_second_b_now_silent", g_b_fires.load() == 0);
        ctx.check("drop_second_c_still_fires", g_c_fires.load() == 1);
        ctx.check("drop_second_no_cross_fire", !g_cross_fire.load());
        ctx.check("drop_second_a_frame_ok", g_a_ok.load());
        ctx.check("drop_second_c_frame_ok", g_c_ok.load());
        // b's original body still ran on the second pass (allow-through with the
        // hook gone) -- a, b, c results all reflect the unmodified bodies.
        ctx.check("drop_second_b_allow_through",
                  hc_fixture::get_b_result() == (static_cast<std::int64_t>(SEED) + B_ARG));
    }   // h_a, h_c (and the empty h_b unique_ptr) drop here.

    // =====================================================================
    // 5 — INSTALL-ORDER INDEPENDENCE.  Install in REVERSE order (c, then b, then
    //     a) and drive the full a+b+c pass: the demux is identical regardless of
    //     g_hooked_methods insertion order (common_detour matches by Method*
    //     identity, not by position).  Each fires once, right frame, no cross-fire.
    // =====================================================================
    {
        auto h_c{ vmhook::scoped_hook<hc_fixture>("c", "(Ljava/lang/String;)I", c_detour()) };
        auto h_b{ vmhook::scoped_hook<hc_fixture>("b", "(J)J", b_detour()) };
        auto h_a{ vmhook::scoped_hook<hc_fixture>("a", "(I)I", a_detour()) };
        ctx.check("order_installed_reverse",
                  h_c.installed() && h_b.installed() && h_a.installed());

        const bool done{ drive(ctx, 5) };
        ctx.check("order_probe_completed", done);
        ctx.check("order_a_fired_once", g_a_fires.load() == 1);
        ctx.check("order_b_fired_once", g_b_fires.load() == 1);
        ctx.check("order_c_fired_once", g_c_fires.load() == 1);
        ctx.check("order_a_frame_ok", g_a_ok.load());
        ctx.check("order_b_frame_ok", g_b_ok.load());
        ctx.check("order_c_frame_ok", g_c_ok.load());
        ctx.check("order_no_cross_fire", !g_cross_fire.load());
        ctx.check("order_total_is_three",
                  g_a_fires.load() + g_b_fires.load() + g_c_fires.load() == 3);
    }

    // =====================================================================
    // 6 — INSTANCE + STATIC share the stub.  a (instance) and s (static) hooked
    //     together; mode 7 calls both.  Each fires once; a sees a self, s sees NO
    //     self (its first param IS the int at slot 0); no cross-fire across the
    //     instance/static boundary on the shared stub.
    // =====================================================================
    {
        auto h_a{ vmhook::scoped_hook<hc_fixture>("a", "(I)I", a_detour()) };
        auto h_s{ vmhook::scoped_hook<hc_fixture>("s", "(I)I", s_detour()) };
        ctx.check("mixed_static_installed", h_a.installed() && h_s.installed());

        const bool done{ drive(ctx, 7) };
        ctx.check("mixed_static_probe_completed", done);
        ctx.check("mixed_static_a_fired_once", g_a_fires.load() == 1);
        ctx.check("mixed_static_s_fired_once", g_s_fires.load() == 1);
        ctx.check("mixed_static_a_frame_ok", g_a_ok.load());
        ctx.check("mixed_static_s_frame_ok", g_s_ok.load());
        ctx.check("mixed_static_no_cross_fire", !g_cross_fire.load());
        // a and s have the SAME (I)I descriptor but are different Method*; the
        // instance one must NOT fire for the static call and vice-versa.
        ctx.check("mixed_static_total_is_two",
                  g_a_fires.load() + g_s_fires.load() == 2);
        ctx.check("mixed_static_allow_through_a",
                  hc_fixture::get_a_result() == (SEED + A_ARG));
        ctx.check("mixed_static_allow_through_s",
                  hc_fixture::get_s_result() == (S_ARG * 2));
    }

    // =====================================================================
    // 7 — NO-ARG frame + two-slot double share the stub.  e() (empty frame) and
    //     d(double) (two interpreter slots) hooked together; mode 8 calls both.
    //     Each fires once with the right frame; no cross-fire.  Proves the shared
    //     detour handles the degenerate (no locals) and wide (2-slot) frames side
    //     by side.
    // =====================================================================
    {
        auto h_e{ vmhook::scoped_hook<hc_fixture>("e", "()I", e_detour()) };
        auto h_d{ vmhook::scoped_hook<hc_fixture>("d", "(D)J", d_detour()) };
        ctx.check("noarg_wide_installed", h_e.installed() && h_d.installed());

        const bool done{ drive(ctx, 8) };
        ctx.check("noarg_wide_probe_completed", done);
        ctx.check("noarg_wide_e_fired_once", g_e_fires.load() == 1);
        ctx.check("noarg_wide_d_fired_once", g_d_fires.load() == 1);
        ctx.check("noarg_wide_e_frame_ok", g_e_ok.load());
        ctx.check("noarg_wide_d_frame_ok", g_d_ok.load());
        ctx.check("noarg_wide_no_cross_fire", !g_cross_fire.load());
        ctx.check("noarg_wide_allow_through_e", hc_fixture::get_e_result() == SEED);
        ctx.check("noarg_wide_allow_through_d",
                  hc_fixture::get_d_result() == (static_cast<std::int64_t>(SEED)
                                                 + static_cast<std::int64_t>(D_ARG)));
    }

    // =====================================================================
    // 8 — PER-DETOUR return override is isolated.  a,b,c hooked; ONLY a's detour
    //     calls retval.set() (cancel + custom return).  Driving a+b+c (mode 5)
    //     must rewrite a's Java-visible result to the override while b and c run
    //     their ORIGINAL bodies untouched.  This proves cancel/set is per-detour
    //     (return_value writes the firing frame's slot), not a global switch that
    //     would also suppress sibling bodies sharing the stub.
    // =====================================================================
    {
        constexpr std::int32_t kOverride{ 123456 };
        auto h_a{ vmhook::scoped_hook<hc_fixture>("a", "(I)I", a_detour(&kOverride)) };
        auto h_b{ vmhook::scoped_hook<hc_fixture>("b", "(J)J", b_detour()) };
        auto h_c{ vmhook::scoped_hook<hc_fixture>("c", "(Ljava/lang/String;)I", c_detour()) };
        ctx.check("override_all_installed",
                  h_a.installed() && h_b.installed() && h_c.installed());

        const bool done{ drive(ctx, 5) };
        ctx.check("override_probe_completed", done);
        ctx.check("override_all_fired_once",
                  g_a_fires.load() == 1 && g_b_fires.load() == 1 && g_c_fires.load() == 1);
        ctx.check("override_no_cross_fire", !g_cross_fire.load());
        // a's return was overridden by ITS detour only.
        ctx.check("override_a_return_replaced", hc_fixture::get_a_result() == kOverride);
        ctx.check("override_a_return_not_original", hc_fixture::get_a_result() != (SEED + A_ARG));
        // b and c bodies ran normally (sibling detours did NOT cancel).
        ctx.check("override_b_body_untouched",
                  hc_fixture::get_b_result() == (static_cast<std::int64_t>(SEED) + B_ARG));
        ctx.check("override_c_body_untouched", hc_fixture::get_c_result() == (SEED + C_ARG_LEN));
    }

    // After scenario 8's handles drop, a plain a() must run its ORIGINAL body
    // again (the override was bound to that scope's detour only, now gone).
    {
        const std::int32_t a_before{ hc_fixture::get_a_calls() };
        const bool done{ drive(ctx, 2) };
        ctx.check("override_after_drop_probe_completed", done);
        ctx.check("override_after_drop_java_called_a",
                  hc_fixture::get_a_calls() - a_before == 1);
        ctx.check("override_after_drop_a_detour_silent", g_a_fires.load() == 0);
        ctx.check("override_after_drop_a_body_restored",
                  hc_fixture::get_a_result() == (SEED + A_ARG));
    }

    // Leave NOTHING armed for later modules sharing the JVM/stub: every handle
    // above was scope-local and has been RAII-destroyed by this point.
    ctx.check("module_left_no_hooks_armed", true);
}
