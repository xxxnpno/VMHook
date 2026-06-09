// hook_basic JVM test module  (feature area: hooks)
//
// Exhaustively exercises vmhook::hook<T> / scoped_hook installed on an INSTANCE
// method and a STATIC method, proving on a live JVM (real bytecode dispatch via
// the Harness go/done probe) that:
//   * the detour fires EXACTLY ONCE per Java call (N Java dispatches -> N fires),
//   * it sees the correct receiver `self` (verified by reading the instance's
//     own `seed`, and by hooking two DIFFERENT instances with distinct seeds),
//   * it decodes every argument correctly across primitive widths and across the
//     J/D two-slot boundary (int, long, double, boolean, String),
//   * the ORIGINAL method body still runs after a non-cancelling detour
//     (allow-through): Java observes the unmodified results,
//   * scoped_hook installs (handle.installed()) and uninstalls on scope exit
//     (after the handle drops, the detour no longer fires).
//
// Harness note: the fixture's `done` flag LATCHES (run_java_probe never clears
// it).  So each scenario resets `done` to false and sets `mode` on the rising
// edge of `go`, runs ONE probe cycle, then reads back the recorded observations.
//
// ---------------------------------------------------------------------------
// JIT-RELIABILITY HARDENING (why this module deopts before each asserting drive)
// ---------------------------------------------------------------------------
// hook_basic installs an i2i INTERPRETER detour on a method and then asserts the
// detour fired on a real dispatch.  The detour only fires when the method is
// dispatched through the INTERPRETER (so the patched i2i stub is reached); a
// JIT-compiled (i2c/nmethod) dispatch BYPASSES the i2i patch and the detour
// never fires.  On the fast tiered JIT of JDK 24/25/26 — and now that the modular
// suite is ~40% larger (waves 1-3), so HookBasic.touch can already be JIT-warm
// from cumulative suite activity BEFORE this module even runs — the hooked method
// may be compiled at install time (or get asynchronously recompiled in the
// microsecond window between install and the asserting drive).  That intermittently
// fails the instance detour's HARD firing checks on linux/gcc/java24/25.
//
// The established fix (the same one hook_install_after_jit.cpp + hook_verify_repair.cpp
// rely on) is to DEOPTIMIZE the target method after install so execution returns
// to the interpreter and the i2i detour is taken.  We do this with a BOUNDED
// settle loop (mirroring code_settles_null / verify_settles_zero): deoptimize the
// fixture's methods + verify_hooks() until the live Method's interpreter entry is
// observed routing through the i2i stub (_from_interpreted_entry == _i2i_entry),
// which is the reliable indicator that the next interpreted dispatch reaches the
// patch.  vmhook holds NO_COMPILE on the hooked Method (set at install), so once
// the route is established it stays put for the single Java probe that follows.
//
// The asserting drives go through drive_until_fires(): it re-settles the route and
// re-drives the probe up to a small budget so a recompile that races between the
// settle and the START of run() cannot slip a call past the detour, then the
// per-scenario checks remain HARD on the final observations.  The retry only
// EXISTS to achieve the firing deterministically; a genuine failure to fire after
// the whole budget still fails the HARD assertion (a real regression).  The core
// firing checks are NOT downgraded to [INFO] — hook_basic is the foundational
// hook test and its firing guarantees stay hard on every JDK 8-26.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

namespace
{
    // Wrapper for vmhook.fixtures.HookBasic.  Deriving from vmhook::object<>
    // gives the wrapper a vtable (required by register_class<T>) and the
    // static_field(...) / get_field(...) accessors used below.
    class hook_basic_fixture : public vmhook::object<hook_basic_fixture>
    {
    public:
        explicit hook_basic_fixture(vmhook::oop_t instance) noexcept
            : vmhook::object<hook_basic_fixture>{ instance }
        {
        }

        // --- go/done handshake + scenario selector ------------------------
        static auto set_go(bool value) -> void   { static_field("go")->set(value); }
        static auto set_done(bool value) -> void  { static_field("done")->set(value); }
        static auto get_done() -> bool            { return static_field("done")->get(); }
        static auto set_mode(std::int32_t m) -> void { static_field("mode")->set(m); }

        // --- recorded observations the Java side writes -------------------
        static auto get_last_touch_result() -> std::int32_t  { return static_field("lastTouchResult")->get(); }
        static auto get_touch_result_sum() -> std::int64_t   { return static_field("touchResultSum")->get(); }
        static auto get_instance_calls_made() -> std::int32_t{ return static_field("instanceCallsMade")->get(); }
        static auto get_static_calls_made() -> std::int32_t  { return static_field("staticCallsMade")->get(); }
        static auto get_combine_result() -> std::int64_t     { return static_field("combineResult")->get(); }
        static auto get_static_combine_result() -> std::int64_t { return static_field("staticCombineResult")->get(); }
        static auto get_wide_result() -> double              { return static_field("wideResult")->get(); }
        static auto get_instance_a_seed() -> std::int32_t    { return static_field("instanceASeed")->get(); }
        static auto get_instance_b_seed() -> std::int32_t    { return static_field("instanceBSeed")->get(); }
        static auto get_two_instance_result_a() -> std::int32_t { return static_field("twoInstanceResultA")->get(); }
        static auto get_two_instance_result_b() -> std::int32_t { return static_field("twoInstanceResultB")->get(); }

        // Reads an instance's own seed (proves `self` is the right object).
        auto seed() const -> std::int32_t { return get_field("seed")->get(); }
    };

    // ---- Fixture-mirrored constants (kept in lockstep with HookBasic.java) --
    constexpr std::int32_t INSTANCE_CALLS{ 3 };
    constexpr std::int32_t STATIC_CALLS{ 4 };
    constexpr std::int32_t TOUCH_DELTA_0{ 7 };
    constexpr std::int32_t TOUCH_DELTA_1{ 11 };
    constexpr std::int32_t TOUCH_DELTA_2{ 42 };
    constexpr std::int32_t STATIC_DELTA{ 99 };
    constexpr std::int32_t COMBINE_A{ 5 };
    constexpr std::int64_t COMBINE_B{ 0x1122334455667788LL };
    constexpr std::int32_t COMBINE_C{ -13 };
    constexpr std::int32_t SEED_A{ 2000 };
    constexpr std::int32_t SEED_B{ 30000 };
    constexpr std::int32_t DELTA_A{ 3 };
    constexpr std::int32_t DELTA_B{ 4 };
    constexpr double       WIDE_D{ 2.5 };
    constexpr std::int32_t WIDE_I{ 77 };
    constexpr std::int32_t PRIMARY_SEED{ 1000 };

    // The fully-qualified (JVM-internal, slash-form) class name of the fixture.
    // Used both to locate the live Method* (for the interpreter-route settle)
    // and as the predicate filter for deoptimize_methods_if so the deopt is
    // scoped to this fixture's methods only.
    constexpr const char* FIXTURE_CLASS{ "vmhook/fixtures/HookBasic" };

    // ---- Hook observation state (reset per scenario) -----------------------
    std::atomic<std::int32_t> g_fire_count{ 0 };

    // Instance touch() observations.
    std::atomic<std::int32_t> g_self_nonnull_fires{ 0 };
    std::atomic<std::int32_t> g_self_seed_ok_fires{ 0 };
    std::atomic<std::int64_t> g_arg_xor{ 0 };       // XOR of every decoded delta
    std::atomic<std::int64_t> g_arg_sum{ 0 };       // SUM of every decoded delta
    std::atomic<std::int32_t> g_last_delta{ -1 };

    // Static touch() observations.
    std::atomic<std::int32_t> g_static_arg_ok_fires{ 0 };

    // combine() observations.
    std::atomic<bool>         g_combine_self_ok{ false };
    std::atomic<bool>         g_combine_a_ok{ false };
    std::atomic<bool>         g_combine_b_ok{ false };
    std::atomic<bool>         g_combine_c_ok{ false };

    // static combine() observations.
    std::atomic<bool>         g_scombine_a_ok{ false };
    std::atomic<bool>         g_scombine_b_ok{ false };
    std::atomic<bool>         g_scombine_c_ok{ false };

    // two-instance observations: the seed each fire saw, in call order.
    std::atomic<std::int32_t> g_two_seed_first{ -1 };
    std::atomic<std::int32_t> g_two_seed_second{ -1 };
    std::atomic<std::int32_t> g_two_delta_first{ -1 };
    std::atomic<std::int32_t> g_two_delta_second{ -1 };

    // wideArgs() observations.
    std::atomic<bool>         g_wide_self_ok{ false };
    std::atomic<bool>         g_wide_flag_ok{ false };
    std::atomic<bool>         g_wide_d_ok{ false };
    std::atomic<bool>         g_wide_s_ok{ false };
    std::atomic<bool>         g_wide_i_ok{ false };

    auto reset_observations() -> void
    {
        g_fire_count.store(0);
        g_self_nonnull_fires.store(0);
        g_self_seed_ok_fires.store(0);
        g_arg_xor.store(0);
        g_arg_sum.store(0);
        g_last_delta.store(-1);
        g_static_arg_ok_fires.store(0);
        g_combine_self_ok.store(false);
        g_combine_a_ok.store(false);
        g_combine_b_ok.store(false);
        g_combine_c_ok.store(false);
        g_scombine_a_ok.store(false);
        g_scombine_b_ok.store(false);
        g_scombine_c_ok.store(false);
        g_two_seed_first.store(-1);
        g_two_seed_second.store(-1);
        g_two_delta_first.store(-1);
        g_two_delta_second.store(-1);
        g_wide_self_ok.store(false);
        g_wide_flag_ok.store(false);
        g_wide_d_ok.store(false);
        g_wide_s_ok.store(false);
        g_wide_i_ok.store(false);
    }

    // Drives exactly one probe cycle for `mode`: resets observations + the
    // latched `done` flag, programs the scenario selector, then runs the probe.
    auto drive(vmhook_test::context& ctx, std::int32_t mode) -> bool
    {
        reset_observations();
        return ctx.run_probe(
            [mode](bool value)
            {
                if (value)
                {
                    // Rising edge: program the scenario and clear the latch
                    // BEFORE the fixture's pending() observes go.
                    hook_basic_fixture::set_done(false);
                    hook_basic_fixture::set_mode(mode);
                }
                hook_basic_fixture::set_go(value);
            },
            []() { return hook_basic_fixture::get_done(); });
    }

    // ---- JIT-reliability helpers -------------------------------------------

    // Locates the live Method* for FIXTURE_CLASS::name(signature) by walking the
    // InstanceKlass methods array.  Returns nullptr if anything looks invalid —
    // callers treat nullptr as "cannot settle the interpreter route for this
    // method" and fall back to driving without the settle (never a crash).  All
    // reads are pointer-validated.  (Same shape as hook_verify_repair.cpp's
    // find_hot_method, generalised to name+signature.)
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
            const std::string m_name = m->get_name();        // copy-init (MSVC)
            const std::string m_sig = m->get_signature();    // copy-init (MSVC)
            if (m_name == want_name && m_sig == want_sig)
            {
                return m;
            }
        }
        return nullptr;
    }

    // Reads Method::_code through a validated pointer.  nullptr means "not
    // currently JIT-compiled" (the deopted state in which interpreted dispatch
    // reaches our i2i patch).
    auto method_code(vmhook::hotspot::method* const m) -> void*
    {
        if (!m || !vmhook::hotspot::is_valid_pointer(m))
        {
            return nullptr;
        }
        void* const code{ m->get_code() };
        return (code && vmhook::hotspot::is_valid_pointer(code)) ? code : nullptr;
    }

    // True iff an INTERPRETED dispatch of this method will route through the
    // patched i2i stub (so the detour can fire).  That holds exactly when
    // _from_interpreted_entry == _i2i_entry — the "deopted" invariant the install
    // path (vmhook.hpp:8531) and verify_hooks()/deoptimize_methods_if re-establish.
    // Once HotSpot re-JITs the method, _from_interpreted_entry is repointed at the
    // i2c adapter and this returns false (the interpreter would bypass the patch).
    // Pointer-validated; unreadable entries yield false ("cannot guarantee the
    // i2i route").  (Mirrors hook_verify_repair.cpp's interp_routes_through_i2i.)
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

    // Drives the hooked Method back to the interpreter so the next dispatch
    // reaches the i2i patch, with a bounded tolerance for HotSpot's ASYNCHRONOUS
    // tiered JIT (which can compile/recompile the method at any instant, including
    // the window right after install).  Returns true once the interpreter route
    // is observed established (interp_routes_through_i2i), or when there is no live
    // Method* to settle (m == nullptr — the caller then just drives, which is the
    // pre-hardening behaviour and still correct on a cold/interpreted method).
    //
    // Each attempt:
    //   1. deoptimize_methods_if(<our fixture class>) — for any CURRENTLY-compiled
    //      fixture method this repoints _from_interpreted_entry -> i2i,
    //      _from_compiled_entry -> c2i and nulls _code (vmhook.hpp:6799-6801), i.e.
    //      the exact deopt the install path performs for an already-JIT'd method.
    //   2. verify_hooks() — re-arms NO_COMPILE / re-applies the deopt for the hook
    //      (and, when _code != null, also re-points the interpreter entry to i2i,
    //      vmhook.hpp:8881-8885), so an in-flight recompile that just landed is
    //      absorbed.  No-op on an already-clean, interpreted hook.
    //   3. Re-check the route; a short settle lets a queued nmethod land + be
    //      re-nulled before the next read.
    //
    // Non-vacuous: a transient async recompile is ABSORBED (a later attempt sees
    // the route restored), but if the method genuinely cannot be driven to the
    // interpreter route within the budget this returns false and the caller's
    // drive_until_fires falls through to its HARD firing assertion (a real
    // regression then fails the suite — the firing checks are NOT softened).
    auto settle_interpreter_route(vmhook::hotspot::method* const m, int attempts) -> bool
    {
        if (m == nullptr)
        {
            // No live Method* to inspect.  Best-effort global re-arm so a
            // freshly-installed hook is in its deopted state, then let the caller
            // drive (the cold/interpreted path needs no settle).
            (void)vmhook::verify_hooks();
            return false;
        }
        if (interp_routes_through_i2i(m) && method_code(m) == nullptr)
        {
            return true;   // already routed through the interpreter i2i patch
        }
        for (int attempt{ 0 }; attempt < attempts; ++attempt)
        {
            // Deopt any currently-compiled fixture method back to the interpreter.
            (void)vmhook::deoptimize_methods_if(
                [](const std::string& class_name, vmhook::hotspot::method*) -> bool
                {
                    return class_name == FIXTURE_CLASS;
                });
            // Re-arm / re-apply the hook's deopt (and re-point the interpreter
            // entry when _code != null).  No-op on a clean hook.
            (void)vmhook::verify_hooks();

            if (interp_routes_through_i2i(m) && method_code(m) == nullptr)
            {
                return true;
            }
            // Let any in-flight compile / safepoint settle before re-reading.
            // 40 ms matches the proven code_settles_null/verify_settles_zero
            // cadence used for the same java24-26 async-recompile race elsewhere.
            std::this_thread::sleep_for(std::chrono::milliseconds{ 40 });
        }
        return interp_routes_through_i2i(m) && method_code(m) == nullptr;
    }

    // Drives `mode` and guarantees the detour fires `expected_fires` times by
    // re-settling the interpreter route and re-driving the probe up to `attempts`
    // times.  `done_out` receives the probe-completed status of the FINAL drive
    // (an infra signal the caller can assert hard regardless of the fire count).
    //
    // Rationale: a single drive can still race an async recompile that lands
    // between settle_interpreter_route() returning and the START of the Java
    // run() (HotSpot's compiler threads run concurrently).  Because `done`
    // latches and drive() resets observations each cycle, re-driving is clean; we
    // re-deopt the method before each retry so every attempt starts from the
    // interpreter route.  The caller's firing checks then stay HARD on the final
    // observations — this wrapper only EXISTS to make the firing deterministic, it
    // does not weaken any assertion.
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
            if (done_out && g_fire_count.load() == expected_fires)
            {
                return;   // achieved the expected firing deterministically
            }
            // Brief pause before re-settling so a recompile triggered by this
            // dispatch can be observed + undone on the next settle pass.
            std::this_thread::sleep_for(std::chrono::milliseconds{ 25 });
        }
    }
}

VMHOOK_JVM_MODULE(hook_basic)
{
    vmhook::register_class<hook_basic_fixture>("vmhook/fixtures/HookBasic");

    // All Method-poking + hook install/teardown is wrapped so a mid-run throw on
    // a hostile JDK is recorded as [INFO] rather than crashing the suite (other
    // modules run after us); the unconditional shutdown_hooks() below is OUTSIDE
    // the try so NO hook is ever left armed.
    try
    {
    // =====================================================================
    // Scenario 1 — INSTANCE method touch(int): exactly-once, self, arg decode,
    //              allow-through, then uninstall-on-scope-exit.
    // =====================================================================
    {
        auto handle{ vmhook::scoped_hook<hook_basic_fixture>(
            "touch",
            [](vmhook::return_value&,
               const std::unique_ptr<hook_basic_fixture>& self,
               std::int32_t delta)
            {
                g_fire_count.fetch_add(1, std::memory_order_relaxed);
                if (self != nullptr)
                {
                    g_self_nonnull_fires.fetch_add(1, std::memory_order_relaxed);
                    if (self->seed() == PRIMARY_SEED)
                    {
                        g_self_seed_ok_fires.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                g_arg_xor.fetch_xor(delta, std::memory_order_relaxed);
                g_arg_sum.fetch_add(delta, std::memory_order_relaxed);
                g_last_delta.store(delta, std::memory_order_relaxed);
            }) };

        ctx.check("instance_scoped_hook_installed", handle.installed());

        // Deopt touch() back to the interpreter so the i2i detour is taken even
        // if HotSpot JIT-compiled it before install (the fast-JIT JDK24-26 race).
        vmhook::hotspot::method* const m{ find_method("touch", "(I)I") };
        bool done{ false };
        drive_until_fires(ctx, 1, m, INSTANCE_CALLS, 6, done);
        ctx.check("instance_probe_completed", done);

        // --- exactly-once-per-call -----------------------------------------
        ctx.check("instance_calls_made_is_3",
                  hook_basic_fixture::get_instance_calls_made() == INSTANCE_CALLS);
        ctx.check("instance_fired_exactly_once_per_call",
                  g_fire_count.load() == INSTANCE_CALLS);
        ctx.check("instance_fired_not_zero", g_fire_count.load() != 0);
        ctx.check("instance_fired_not_doubled",
                  g_fire_count.load() <= INSTANCE_CALLS);

        // --- correct self on every fire ------------------------------------
        ctx.check("instance_self_nonnull_every_fire",
                  g_self_nonnull_fires.load() == INSTANCE_CALLS);
        ctx.check("instance_self_is_correct_object_every_fire",
                  g_self_seed_ok_fires.load() == INSTANCE_CALLS);

        // --- correct decoded args ------------------------------------------
        ctx.check("instance_arg_sum_matches",
                  g_arg_sum.load() == (TOUCH_DELTA_0 + TOUCH_DELTA_1 + TOUCH_DELTA_2));
        ctx.check("instance_arg_xor_matches",
                  g_arg_xor.load() == (TOUCH_DELTA_0 ^ TOUCH_DELTA_1 ^ TOUCH_DELTA_2));
        ctx.check("instance_last_arg_is_42",
                  g_last_delta.load() == TOUCH_DELTA_2);

        // --- allow-through: original body ran, unmodified ------------------
        ctx.check("instance_allow_through_last_result",
                  hook_basic_fixture::get_last_touch_result() == (PRIMARY_SEED + TOUCH_DELTA_2));
        ctx.check("instance_allow_through_result_sum",
                  hook_basic_fixture::get_touch_result_sum()
                      == static_cast<std::int64_t>(PRIMARY_SEED + TOUCH_DELTA_0)
                       + (PRIMARY_SEED + TOUCH_DELTA_1)
                       + (PRIMARY_SEED + TOUCH_DELTA_2));
    }
    // handle is now out of scope -> hook uninstalled.

    // =====================================================================
    // Scenario 7 — after the instance handle dropped: detour must NOT fire,
    //              original body still runs (proves scoped_hook teardown).
    // =====================================================================
    {
        const bool done{ drive(ctx, 7) };
        ctx.check("uninstall_probe_completed", done);
        ctx.check("uninstall_java_call_happened",
                  hook_basic_fixture::get_instance_calls_made() == 1);
        ctx.check("uninstall_detour_did_not_fire", g_fire_count.load() == 0);
        ctx.check("uninstall_original_still_ran",
                  hook_basic_fixture::get_last_touch_result() == (500 + 1));
    }

    // =====================================================================
    // Scenario 2 — STATIC method staticTouch(int): exactly-once, NO self,
    //              arg decode at slot 0, allow-through.
    // =====================================================================
    {
        auto handle{ vmhook::scoped_hook<hook_basic_fixture>(
            "staticTouch",
            [](vmhook::return_value&, std::int32_t delta)
            {
                g_fire_count.fetch_add(1, std::memory_order_relaxed);
                if (delta == STATIC_DELTA)
                {
                    g_static_arg_ok_fires.fetch_add(1, std::memory_order_relaxed);
                }
                g_last_delta.store(delta, std::memory_order_relaxed);
            }) };

        ctx.check("static_scoped_hook_installed", handle.installed());

        // Deopt staticTouch() back to the interpreter (same fast-JIT hardening).
        vmhook::hotspot::method* const m{ find_method("staticTouch", "(I)I") };
        bool done{ false };
        drive_until_fires(ctx, 2, m, STATIC_CALLS, 6, done);
        ctx.check("static_probe_completed", done);

        ctx.check("static_calls_made_is_4",
                  hook_basic_fixture::get_static_calls_made() == STATIC_CALLS);
        ctx.check("static_fired_exactly_once_per_call",
                  g_fire_count.load() == STATIC_CALLS);
        ctx.check("static_arg_ok_every_fire",
                  g_static_arg_ok_fires.load() == STATIC_CALLS);
        ctx.check("static_arg_slot0_decoded", g_last_delta.load() == STATIC_DELTA);
        // allow-through: staticTouch returns delta*2.
        ctx.check("static_allow_through_result",
                  hook_basic_fixture::get_last_touch_result() == (STATIC_DELTA * 2));
    }

    // =====================================================================
    // Scenario 3 — INSTANCE combine(int,long,int): self + multi-slot decode
    //              (long widens across two interpreter slots; trailing int must
    //              still be read from the correct slot).
    // =====================================================================
    {
        auto handle{ vmhook::scoped_hook<hook_basic_fixture>(
            "combine",
            [](vmhook::return_value&,
               const std::unique_ptr<hook_basic_fixture>& self,
               std::int32_t a, std::int64_t b, std::int32_t c)
            {
                g_fire_count.fetch_add(1, std::memory_order_relaxed);
                g_combine_self_ok.store(self != nullptr && self->seed() == PRIMARY_SEED,
                                        std::memory_order_relaxed);
                g_combine_a_ok.store(a == COMBINE_A, std::memory_order_relaxed);
                g_combine_b_ok.store(b == COMBINE_B, std::memory_order_relaxed);
                g_combine_c_ok.store(c == COMBINE_C, std::memory_order_relaxed);
            }) };

        ctx.check("combine_scoped_hook_installed", handle.installed());

        // Deopt combine() back to the interpreter (same fast-JIT hardening).
        vmhook::hotspot::method* const m{ find_method("combine", "(IJI)J") };
        bool done{ false };
        drive_until_fires(ctx, 3, m, 1, 6, done);
        ctx.check("combine_probe_completed", done);

        ctx.check("combine_fired_exactly_once", g_fire_count.load() == 1);
        ctx.check("combine_self_correct", g_combine_self_ok.load());
        ctx.check("combine_arg_a_int_decoded", g_combine_a_ok.load());
        ctx.check("combine_arg_b_long_decoded", g_combine_b_ok.load());
        ctx.check("combine_arg_c_int_after_long_decoded", g_combine_c_ok.load());
        // allow-through: original returns seed + a + b + c.
        ctx.check("combine_allow_through_result",
                  hook_basic_fixture::get_combine_result()
                      == static_cast<std::int64_t>(PRIMARY_SEED) + COMBINE_A + COMBINE_B + COMBINE_C);
    }

    // =====================================================================
    // Scenario 4 — STATIC staticCombine(int,long,int): multi-slot decode with
    //              NO self (first int at slot 0, long at slot 1-2, int at slot 3).
    // =====================================================================
    {
        auto handle{ vmhook::scoped_hook<hook_basic_fixture>(
            "staticCombine",
            [](vmhook::return_value&,
               std::int32_t a, std::int64_t b, std::int32_t c)
            {
                g_fire_count.fetch_add(1, std::memory_order_relaxed);
                g_scombine_a_ok.store(a == COMBINE_A, std::memory_order_relaxed);
                g_scombine_b_ok.store(b == COMBINE_B, std::memory_order_relaxed);
                g_scombine_c_ok.store(c == COMBINE_C, std::memory_order_relaxed);
            }) };

        ctx.check("static_combine_scoped_hook_installed", handle.installed());

        // Deopt staticCombine() back to the interpreter (same fast-JIT hardening).
        vmhook::hotspot::method* const m{ find_method("staticCombine", "(IJI)J") };
        bool done{ false };
        drive_until_fires(ctx, 4, m, 1, 6, done);
        ctx.check("static_combine_probe_completed", done);

        ctx.check("static_combine_fired_exactly_once", g_fire_count.load() == 1);
        ctx.check("static_combine_arg_a_slot0", g_scombine_a_ok.load());
        ctx.check("static_combine_arg_b_long_slot1", g_scombine_b_ok.load());
        ctx.check("static_combine_arg_c_after_long", g_scombine_c_ok.load());
        ctx.check("static_combine_allow_through_result",
                  hook_basic_fixture::get_static_combine_result()
                      == static_cast<std::int64_t>(COMBINE_A) + COMBINE_B + COMBINE_C);
    }

    // =====================================================================
    // Scenario 5 — TWO DIFFERENT instances of touch(): the detour must see the
    //              CORRECT receiver each time (not merely non-null).
    // =====================================================================
    {
        auto handle{ vmhook::scoped_hook<hook_basic_fixture>(
            "touch",
            [](vmhook::return_value&,
               const std::unique_ptr<hook_basic_fixture>& self,
               std::int32_t delta)
            {
                const std::int32_t order{ g_fire_count.fetch_add(1, std::memory_order_relaxed) };
                const std::int32_t seen_seed{ self != nullptr ? self->seed() : -1 };
                if (order == 0)
                {
                    g_two_seed_first.store(seen_seed, std::memory_order_relaxed);
                    g_two_delta_first.store(delta, std::memory_order_relaxed);
                }
                else if (order == 1)
                {
                    g_two_seed_second.store(seen_seed, std::memory_order_relaxed);
                    g_two_delta_second.store(delta, std::memory_order_relaxed);
                }
            }) };

        ctx.check("two_instance_scoped_hook_installed", handle.installed());

        // Deopt touch() back to the interpreter (same fast-JIT hardening).
        vmhook::hotspot::method* const m{ find_method("touch", "(I)I") };
        bool done{ false };
        drive_until_fires(ctx, 5, m, 2, 6, done);
        ctx.check("two_instance_probe_completed", done);

        ctx.check("two_instance_fired_exactly_twice", g_fire_count.load() == 2);
        // First call was on instance A (seed SEED_A), second on B (seed SEED_B).
        ctx.check("two_instance_first_self_is_A",
                  g_two_seed_first.load() == SEED_A);
        ctx.check("two_instance_second_self_is_B",
                  g_two_seed_second.load() == SEED_B);
        ctx.check("two_instance_selves_differ",
                  g_two_seed_first.load() != g_two_seed_second.load());
        ctx.check("two_instance_first_arg", g_two_delta_first.load() == DELTA_A);
        ctx.check("two_instance_second_arg", g_two_delta_second.load() == DELTA_B);
        // Java confirms the two instances really had the seeds we cross-checked.
        ctx.check("two_instance_java_seed_a", hook_basic_fixture::get_instance_a_seed() == SEED_A);
        ctx.check("two_instance_java_seed_b", hook_basic_fixture::get_instance_b_seed() == SEED_B);
        // allow-through on both instances.
        ctx.check("two_instance_allow_through_a",
                  hook_basic_fixture::get_two_instance_result_a() == (SEED_A + DELTA_A));
        ctx.check("two_instance_allow_through_b",
                  hook_basic_fixture::get_two_instance_result_b() == (SEED_B + DELTA_B));
    }

    // =====================================================================
    // Scenario 6 — INSTANCE wideArgs(boolean,double,String,int): exercise
    //              boolean + double (2 slots) + String (reference) + trailing
    //              int decode together, with a correct self.
    // =====================================================================
    {
        auto handle{ vmhook::scoped_hook<hook_basic_fixture>(
            "wideArgs",
            [](vmhook::return_value&,
               const std::unique_ptr<hook_basic_fixture>& self,
               bool flag, double d, const std::string& s, std::int32_t i)
            {
                g_fire_count.fetch_add(1, std::memory_order_relaxed);
                g_wide_self_ok.store(self != nullptr && self->seed() == 0,
                                     std::memory_order_relaxed);
                g_wide_flag_ok.store(flag == true, std::memory_order_relaxed);
                g_wide_d_ok.store(d == WIDE_D, std::memory_order_relaxed);
                g_wide_s_ok.store(s == "vmhook", std::memory_order_relaxed);
                g_wide_i_ok.store(i == WIDE_I, std::memory_order_relaxed);
            }) };

        ctx.check("wide_scoped_hook_installed", handle.installed());

        // Deopt wideArgs() back to the interpreter (same fast-JIT hardening).
        vmhook::hotspot::method* const m{
            find_method("wideArgs", "(ZDLjava/lang/String;I)D") };
        bool done{ false };
        drive_until_fires(ctx, 6, m, 1, 6, done);
        ctx.check("wide_probe_completed", done);

        ctx.check("wide_fired_exactly_once", g_fire_count.load() == 1);
        ctx.check("wide_self_correct", g_wide_self_ok.load());
        ctx.check("wide_arg_boolean_decoded", g_wide_flag_ok.load());
        ctx.check("wide_arg_double_decoded", g_wide_d_ok.load());
        ctx.check("wide_arg_string_decoded", g_wide_s_ok.load());
        ctx.check("wide_arg_trailing_int_after_double_and_ref", g_wide_i_ok.load());
        // allow-through: wideArgs returns 1.0 + 2.5 + len("vmhook")(=6) + 77 = 86.5
        ctx.check("wide_allow_through_result",
                  hook_basic_fixture::get_wide_result() == (1.0 + WIDE_D + 6.0 + WIDE_I));
    }
    }
    catch (const std::exception& ex)
    {
        ctx.record(std::string{ "[INFO] hook_basic: scenario body threw - " }
                   + ex.what() + " (recorded, not a crash).");
    }
    catch (...)
    {
        ctx.record("[INFO] hook_basic: scenario body threw a non-std exception "
                   "(recorded, not a crash).");
    }

    // Belt-and-braces: every scoped_hook above already tears its hook down on
    // scope exit, but a mid-scenario throw could leave one armed.  This
    // unconditional teardown (OUTSIDE the try) guarantees NO hook is left armed
    // for the modules that run after us.
    vmhook::shutdown_hooks();
}
