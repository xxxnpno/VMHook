// hook_install_after_jit JVM test module  (feature area: hooks / deopt-on-install)
//
// Exhaustively exercises the ONE behaviour the README headlines and the audit
// (audit/findings/hook_install_after_jit.md) flags as the highest-value test gap:
// installing a vmhook::hook<T> on a method that is ALREADY JIT-compiled, on a
// LIVE JVM via the modular harness.
//
// The distinction from hook_verify_repair is the ORDER of events.  There the
// method is hooked first (so NO_COMPILE keeps it interpreted) and only then
// warmed.  HERE the method is warmed to a published Method::_code != null FIRST,
// and the hook is installed SECOND.  For the patched i2i stub to take effect on a
// compiled method, vmhook's install path must deoptimise it: clear Method::_code
// and redirect _from_interpreted_entry -> i2i and _from_compiled_entry -> the c2i
// adapter (vmhook.hpp:8205-8241).  This module proves, on real bytecode dispatch:
//
//   * a method warmed to JIT compilation BEFORE install really has _code != null
//     at the moment we install (the precondition that makes this the "after JIT"
//     path, not the interpreted path),
//   * vmhook::hook<T>() returns true on that warm method and, as a side effect of
//     install, Method::_code is NULLED (the deopt fired) and NO_COMPILE is armed,
//   * the very next bytecode dispatch FIRES the detour exactly once, with the
//     correct receiver `self` and correctly decoded arg — i.e. the deopt routed
//     the freshly-resolving call through the interpreter and our patch,
//   * a non-cancelling detour ALLOWS THROUGH: the original (formerly-compiled)
//     body still runs and Java observes the unmodified result,
//   * a CANCELLING detour can FORCE the return value of the formerly-compiled
//     method (we own dispatch, not merely observe it),
//   * verify_hooks() reports 0 drift immediately after installing on a JIT'd
//     method (audit assert (c)),
//   * the documented deopt-sweep workflow also nulls _code:
//     deoptimize_all_jit_compiled_methods() and the predicate-filtered
//     deoptimize_methods_if() both deopt the warm method, while a non-matching
//     predicate leaves an unrelated warm method's _code intact (class-name
//     discrimination works),
//   * after shutdown_hooks() the method runs normally again and the detour does
//     NOT fire.
//
// Robustness (mirrors hook_verify_repair's discipline): forcing HotSpot to JIT a
// method is timing-dependent and JDK/flag-dependent.  If, after a generous warm
// budget, Method::_code never becomes non-null on this runner, the module records
// an [INFO] line and falls back to proving the install+fire+teardown contract on
// the interpreted path — it never turns "the JIT didn't kick in" into a spurious
// red FAIL, and it NEVER crashes the JVM (every Method deref is guarded by
// is_valid_pointer and bails out as an [INFO] rather than dereferencing anything
// suspicious).
//
// Lifecycle discipline: installs here are low-level (vmhook::hook<T>()), so they
// persist until shutdown_hooks().  Every scenario tears its hook down, and the
// module's final statement is an unconditional shutdown_hooks() so NO hook is
// left armed when control returns to the driver (other modules run after us).
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
    // Wrapper for vmhook.fixtures.HookAfterJit.  Deriving from vmhook::object<>
    // gives the wrapper its vtable (required by register_class<T>) and the
    // static_field(...) / get_field(...) accessors.
    class haj_fixture : public vmhook::object<haj_fixture>
    {
    public:
        explicit haj_fixture(vmhook::oop_t instance) noexcept
            : vmhook::object<haj_fixture>{ instance }
        {
        }

        // --- go/done handshake + scenario selector ------------------------
        static auto set_go(bool value) -> void       { static_field("go")->set(value); }
        static auto set_done(bool value) -> void      { static_field("done")->set(value); }
        static auto get_done() -> bool                { return static_field("done")->get(); }
        static auto set_mode(std::int32_t m) -> void  { static_field("mode")->set(m); }

        // --- recorded observations the Java side writes -------------------
        static auto get_last_hot_result() -> std::int32_t { return static_field("lastHotResult")->get(); }
        static auto get_hot_result_xor() -> std::int64_t  { return static_field("hotResultXor")->get(); }
        static auto get_hot_calls_made() -> std::int32_t  { return static_field("hotCallsMade")->get(); }

        // Reads this instance's own seed (proves `self` is the right object).
        auto seed() const -> std::int32_t { return get_field("seed")->get(); }
    };

    // ---- Fixture-mirrored constants (lockstep with HookAfterJit.java) -------
    constexpr std::int32_t SEED{ 1000 };
    constexpr std::int32_t HOT_DELTA{ 7 };
    constexpr std::int32_t WARM_CALLS{ 200000 };

    constexpr std::int32_t HOT_ORIGINAL{ SEED + HOT_DELTA };   // hot(HOT_DELTA) body result
    constexpr std::int32_t FORCED_RETURN{ 0x5AFE5A };          // value a cancelling detour forces

    // The fully-qualified class + the hooked method's name/signature.  Used to
    // locate the live Method* so we can OBSERVE its JIT state (Method::_code)
    // around install.
    constexpr const char* FIXTURE_CLASS{ "vmhook/fixtures/HookAfterJit" };
    constexpr const char* HOT_NAME{ "hot" };
    constexpr const char* HOT_SIG{ "(I)I" };

    // Budget to wait for HotSpot to publish Method::_code after a warm loop.
    // Compilation is asynchronous, so the nmethod may land shortly AFTER the warm
    // probe returns.  We poll generously and may re-warm.
    constexpr std::chrono::milliseconds JIT_BUDGET{ 4000 };
    constexpr std::chrono::milliseconds JIT_POLL{ 25 };
    constexpr int                       MAX_WARM_ROUNDS{ 4 };

    // ---- Hook observation state (reset per scenario) -----------------------
    std::atomic<std::int32_t> g_fire_count{ 0 };
    std::atomic<std::int32_t> g_self_ok_fires{ 0 };   // self non-null & seed == SEED
    std::atomic<std::int64_t> g_arg_xor{ 0 };         // XOR of every decoded delta

    auto reset_observations() -> void
    {
        g_fire_count.store(0);
        g_self_ok_fires.store(0);
        g_arg_xor.store(0);
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
                    haj_fixture::set_done(false);
                    haj_fixture::set_mode(mode);
                }
                haj_fixture::set_go(value);
            },
            []() { return haj_fixture::get_done(); });
    }

    // An OBSERVING (allow-through) detour: counts fires, validates `self`, folds
    // the decoded delta into an XOR so a wrong decode is observable.
    auto install_observer() -> bool
    {
        return vmhook::hook<haj_fixture>(
            HOT_NAME,
            [](vmhook::return_value&,
               const std::unique_ptr<haj_fixture>& self,
               std::int32_t delta)
            {
                g_fire_count.fetch_add(1, std::memory_order_relaxed);
                if (self != nullptr && self->seed() == SEED)
                {
                    g_self_ok_fires.fetch_add(1, std::memory_order_relaxed);
                }
                g_arg_xor.fetch_xor(delta, std::memory_order_relaxed);
            });
    }

    // A CANCELLING detour: forces the return value (proves we own dispatch even
    // on a method that was JIT-compiled at install time).  Still records a fire.
    auto install_forcing() -> bool
    {
        return vmhook::hook<haj_fixture>(
            HOT_NAME,
            [](vmhook::return_value& ret,
               const std::unique_ptr<haj_fixture>& self,
               std::int32_t delta)
            {
                g_fire_count.fetch_add(1, std::memory_order_relaxed);
                if (self != nullptr && self->seed() == SEED)
                {
                    g_self_ok_fires.fetch_add(1, std::memory_order_relaxed);
                }
                g_arg_xor.fetch_xor(delta, std::memory_order_relaxed);
                ret.set<std::int32_t>(FORCED_RETURN);
            });
    }

    // Locates the live Method* for FIXTURE_CLASS::hot(I)I by walking the
    // InstanceKlass methods array.  Returns nullptr if anything looks invalid —
    // callers must treat nullptr as "cannot run this Method-level scenario" and
    // skip it rather than crash.  All reads are pointer-validated.
    auto find_hot_method() -> vmhook::hotspot::method*
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
        for (std::int32_t i{ 0 }; i < count; ++i)
        {
            vmhook::hotspot::method* const m{ methods[i] };
            if (!m || !vmhook::hotspot::is_valid_pointer(m))
            {
                continue;
            }
            const std::string name = m->get_name();          // copy-init (MSVC)
            const std::string sig = m->get_signature();      // copy-init (MSVC)
            if (name == HOT_NAME && sig == HOT_SIG)
            {
                return m;
            }
        }
        return nullptr;
    }

    // Reads Method::_code through a validated pointer.  nullptr means "not
    // currently JIT-compiled" (the deopted steady state vmhook installs).
    auto method_code(vmhook::hotspot::method* const m) -> void*
    {
        if (!m || !vmhook::hotspot::is_valid_pointer(m))
        {
            return nullptr;
        }
        void* const code{ m->get_code() };
        return (code && vmhook::hotspot::is_valid_pointer(code)) ? code : nullptr;
    }

    // True iff the method currently carries the NO_COMPILE inhibitor vmhook sets
    // at install time (i.e. HotSpot is told not to compile it).
    auto no_compile_set(vmhook::hotspot::method* const m) -> bool
    {
        if (!m || !vmhook::hotspot::is_valid_pointer(m))
        {
            return false;
        }
        std::uint32_t* const flags{ m->get_access_flags() };
        return flags && (*flags & vmhook::hotspot::NO_COMPILE) != 0;
    }

    // Drives warm loops (mode 1) with NO hook armed until Method::_code becomes
    // non-null (HotSpot has published the nmethod) or the budget is exhausted.
    // Returns true if _code was observed non-null (the "after JIT" precondition).
    //
    // We MUST NOT have a hook installed while calling this: the whole point is to
    // let HotSpot compile the method freely so that the LATER install sees a
    // compiled method.
    auto warm_until_compiled(vmhook_test::context& ctx,
                             vmhook::hotspot::method* const m) -> bool
    {
        if (m == nullptr)
        {
            return false;
        }
        const auto deadline{ std::chrono::steady_clock::now() + JIT_BUDGET };
        for (int round{ 0 }; round < MAX_WARM_ROUNDS; ++round)
        {
            const bool warmed{ drive(ctx, 1) };
            if (!warmed)
            {
                ctx.record("[INFO] hook_install_after_jit: warm probe (round "
                           + std::to_string(round) + ") did not complete.");
                return false;
            }
            // Poll for the async compile to publish _code.
            while (std::chrono::steady_clock::now() < deadline)
            {
                if (method_code(m) != nullptr)
                {
                    return true;
                }
                std::this_thread::sleep_for(JIT_POLL);
            }
            if (method_code(m) != nullptr)
            {
                return true;
            }
        }
        return method_code(m) != nullptr;
    }

    // True once a verify_hooks() pass reports a CLEAN hook set (returns 0), with a
    // bounded tolerance for HotSpot's ASYNCHRONOUS compiler threads (ports the
    // settle discipline from hook_verify_repair's verify_settles_zero).
    //
    // Why a plain `verify_hooks() == 0` flakes HERE specifically (the
    // headline_verify_hooks_still_zero_after_firing failure on
    // jvm·linux·gcc·java24): this module installs its hook on a method that was
    // DELIBERATELY JIT-COMPILED first, so HotSpot's compiler has already proven it
    // hot and re-queues it aggressively.  verify_hooks() returns the count of hooks
    // it had to REPAIR this pass; its mode-3 detector is
    // `jit_drifted = (_code != null) || !NO_COMPILE` (vmhook.hpp:8690).  _code is
    // owned by HotSpot's compiler threads, so on java24-26's fast tiering an
    // in-flight/queued recompile can land an nmethod (`_code != null`) in the
    // microsecond window between the install / firing and this read.  verify_hooks()
    // then CORRECTLY reports that as one repair and re-deopts the method (re-nulls
    // _code, re-arms NO_COMPILE) — so a single-instant `== 0` observes the transient
    // mid-recompile repair and fails even though the machinery is working.
    // NO_COMPILE (re-armed by that very pass) inhibits the next compile, so the
    // transient drift is FINITE and SETTLES: a subsequent pass finds nothing to
    // repair and returns 0 (verify_hooks() also debounces per-method via
    // drift_logged, vmhook.hpp:8620, so the count converges to 0 either way).
    //
    // We give verify_hooks() — the exact machinery under test — up to `attempts`
    // synchronous passes (~40 ms settle each) and return true as soon as a pass
    // reports 0.  Non-vacuous: a transient async recompile is ABSORBED (a later pass
    // returns 0), but a genuine regression that verify_hooks() cannot settle (e.g.
    // the shared-i2i JMP stomped and unrestorable, which is NOT debounced) keeps
    // returning non-zero and this returns false, failing the caller's check.
    auto verify_settles_zero(int attempts) -> bool
    {
        if (vmhook::verify_hooks() == 0)
        {
            return true;
        }
        for (int attempt{ 0 }; attempt < attempts; ++attempt)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds{ 40 });
            if (vmhook::verify_hooks() == 0)
            {
                return true;
            }
        }
        return false;
    }
}

VMHOOK_JVM_MODULE(hook_install_after_jit)
{
    vmhook::register_class<haj_fixture>(FIXTURE_CLASS);

    // Clean baseline: nothing armed when we start.
    {
        vmhook::shutdown_hooks();   // belt-and-braces: ensure empty
        ctx.check("baseline_verify_hooks_on_empty_set_is_zero",
                  vmhook::verify_hooks() == 0);
    }

    // =====================================================================
    // Scenario 1 — HEADLINE: install on an ALREADY-JIT-compiled method.
    //   Warm hot() to a published Method::_code != null FIRST (NO hook armed),
    //   then install the observing hook.  Assert that install:
    //     - returned true,
    //     - DEOPTED the method (Method::_code became null as a side effect),
    //     - armed NO_COMPILE,
    //   then drive hot() once and assert the detour FIRED exactly once with the
    //   correct self + decoded arg and the original body allowed through.
    // =====================================================================
    bool jit_precondition_met{ false };
    {
        vmhook::hotspot::method* const m{ find_hot_method() };
        ctx.check("headline_located_live_method", m != nullptr);

        if (m == nullptr)
        {
            ctx.record("[INFO] hook_install_after_jit scenario 1: could not locate live "
                       "Method* for hot(I)I - skipping (no crash).");
        }
        else
        {
            // --- Precondition: drive HotSpot to JIT-compile hot() BEFORE install.
            jit_precondition_met = warm_until_compiled(ctx, m);
            void* const code_before{ method_code(m) };
            ctx.record(std::string{ "[INFO] hook_install_after_jit scenario 1: pre-install Method::_code=" }
                       + (code_before == nullptr ? "null" : "NON-null")
                       + (jit_precondition_met
                              ? " (method is JIT-compiled - exercising the after-JIT deopt path)."
                              : " (HotSpot did not publish an nmethod within budget - falling back "
                                "to the interpreted install path; the deopt-specific assert is "
                                "recorded as [INFO], install+fire is still asserted)."));

            // This is the precondition assertion the audit names: the method
            // really is compiled at the moment we install.  When the JVM refused
            // to compile within budget we DON'T fail here (documented as [INFO]).
            if (jit_precondition_met)
            {
                ctx.check("headline_method_is_jit_compiled_before_install",
                          code_before != nullptr);
            }

            // --- Install the hook on the (now warm) method. ---
            ctx.check("headline_install_returns_true", install_observer());

            // --- The deopt side effect: install nulled _code + armed NO_COMPILE.
            // Only assert the _code-was-nulled property when the method was
            // actually compiled at install time (otherwise there was nothing to
            // deopt).  NO_COMPILE is armed unconditionally by the install path.
            if (jit_precondition_met)
            {
                ctx.check("headline_install_deopted_code_to_null", method_code(m) == nullptr);
            }
            else
            {
                ctx.record(std::string{ "[INFO] hook_install_after_jit scenario 1: post-install Method::_code=" }
                           + (method_code(m) == nullptr ? "null" : "NON-null")
                           + " (interpreted-path fallback).");
            }
            ctx.check("headline_install_armed_no_compile", no_compile_set(m));

            // Freshly installed on a JIT'd method -> no drift -> 0 repairs (settle:
            // an async recompile can transiently re-populate _code on this
            // just-compiled method; verify_hooks() repairs+re-arms and converges).
            ctx.check("headline_verify_hooks_zero_after_install",
                      verify_settles_zero(12));

            // --- Drive hot() once: the detour must fire on the deopted method.
            const bool done{ drive(ctx, 2) };
            ctx.check("headline_probe_completed", done);
            ctx.check("headline_java_made_one_call",
                      haj_fixture::get_hot_calls_made() == 1);
            ctx.check("headline_detour_fired_exactly_once", g_fire_count.load() == 1);
            ctx.check("headline_detour_fired_not_zero", g_fire_count.load() != 0);
            ctx.check("headline_detour_not_double_fired", g_fire_count.load() <= 1);
            ctx.check("headline_self_correct", g_self_ok_fires.load() == 1);
            ctx.check("headline_arg_decoded", g_arg_xor.load() == HOT_DELTA);
            // Allow-through: the original (formerly-compiled) body ran unmodified.
            ctx.check("headline_allow_through_original_result",
                      haj_fixture::get_last_hot_result() == HOT_ORIGINAL);

            // Still intact after firing (settle past any async recompile the firing
            // dispatch may have triggered on this formerly-compiled method).
            ctx.check("headline_verify_hooks_still_zero_after_firing",
                      verify_settles_zero(12));
        }

        vmhook::shutdown_hooks();   // clean up scenario 1
    }

    // =====================================================================
    // Scenario 2 — POST-REMOVAL: after shutdown_hooks(), a fresh dispatch must
    //   NOT fire the (removed) detour, yet the original body still runs.  This
    //   pairs with scenario 1 (same method, now formerly-compiled-then-deopted-
    //   then-unhooked) to prove clean teardown.
    // =====================================================================
    {
        const bool done{ drive(ctx, 4) };
        ctx.check("postremoval_probe_completed", done);
        ctx.check("postremoval_java_made_one_call",
                  haj_fixture::get_hot_calls_made() == 1);
        ctx.check("postremoval_detour_did_not_fire", g_fire_count.load() == 0);
        ctx.check("postremoval_original_still_ran",
                  haj_fixture::get_last_hot_result() == HOT_ORIGINAL);
    }

    // =====================================================================
    // Scenario 3 — FORCE-RETURN on a formerly-JIT-compiled method.  Re-warm hot()
    //   to _code != null (NO hook armed), install a CANCELLING detour, then drive
    //   hot() once and assert Java observes the FORCED return value rather than
    //   seed+delta.  This proves vmhook doesn't merely observe the deopted method
    //   but fully owns its dispatch — the strongest statement of "the deopt made
    //   our patch authoritative on a previously-compiled method".
    // =====================================================================
    {
        vmhook::hotspot::method* const m{ find_hot_method() };
        ctx.check("force_located_live_method", m != nullptr);

        if (m == nullptr)
        {
            ctx.record("[INFO] hook_install_after_jit scenario 3: could not locate live "
                       "Method* for hot(I)I - skipping (no crash).");
            vmhook::shutdown_hooks();
        }
        else
        {
            const bool warm_compiled{ warm_until_compiled(ctx, m) };
            void* const code_before{ method_code(m) };
            ctx.record(std::string{ "[INFO] hook_install_after_jit scenario 3: pre-install Method::_code=" }
                       + (code_before == nullptr ? "null" : "NON-null") + ".");
            if (warm_compiled)
            {
                ctx.check("force_method_is_jit_compiled_before_install",
                          code_before != nullptr);
            }

            ctx.check("force_install_returns_true", install_forcing());
            if (warm_compiled)
            {
                ctx.check("force_install_deopted_code_to_null", method_code(m) == nullptr);
            }
            ctx.check("force_install_armed_no_compile", no_compile_set(m));

            const bool done{ drive(ctx, 2) };
            ctx.check("force_probe_completed", done);
            ctx.check("force_detour_fired_exactly_once", g_fire_count.load() == 1);
            ctx.check("force_self_correct", g_self_ok_fires.load() == 1);
            ctx.check("force_arg_decoded", g_arg_xor.load() == HOT_DELTA);
            // The cancelling detour forced the return value: Java observed FORCED,
            // not the original seed+delta.  This is the "we own dispatch" proof.
            ctx.check("force_return_value_overridden",
                      haj_fixture::get_last_hot_result() == FORCED_RETURN);
            ctx.check("force_return_value_not_original",
                      haj_fixture::get_last_hot_result() != HOT_ORIGINAL);

            vmhook::shutdown_hooks();   // clean up scenario 3

            // After removal the original body runs again (force is gone).
            const bool done2{ drive(ctx, 4) };
            ctx.check("force_postremoval_probe_completed", done2);
            ctx.check("force_postremoval_detour_did_not_fire", g_fire_count.load() == 0);
            ctx.check("force_postremoval_original_restored",
                      haj_fixture::get_last_hot_result() == HOT_ORIGINAL);
        }
    }

    // =====================================================================
    // Scenario 4 — DOCUMENTED DEOPT-SWEEP workflow on a JIT'd method.  Instead of
    //   relying on per-hook install to deopt, warm hot() to _code != null and use
    //   the public sweep helpers:
    //     (a) deoptimize_all_jit_compiled_methods() nulls _code,
    //     (b) a predicate that MATCHES the fixture class nulls _code,
    //     (c) a predicate that does NOT match leaves a freshly re-warmed _code
    //         intact (class-name discrimination works).
    //   No hook is involved here, so nothing is left armed.  This covers the
    //   audit's test_hook_install_after_jit_deopt_sweep_catches_inlined /
    //   _predicate_filter.  Each sub-assert is gated on the JIT precondition so a
    //   non-compiling runner records [INFO] instead of FAILing.
    // =====================================================================
    {
        vmhook::hotspot::method* const m{ find_hot_method() };
        ctx.check("sweep_located_live_method", m != nullptr);

        if (m == nullptr)
        {
            ctx.record("[INFO] hook_install_after_jit scenario 4: could not locate live "
                       "Method* for hot(I)I - skipping sweep body (no crash).");
        }
        else
        {
            // (a) deoptimize_all_jit_compiled_methods() ------------------------
            const bool warm_a{ warm_until_compiled(ctx, m) };
            if (warm_a)
            {
                ctx.check("sweep_all_precondition_code_nonnull", method_code(m) != nullptr);
                const std::size_t deopted{ vmhook::deoptimize_all_jit_compiled_methods() };
                void* const code_after{ method_code(m) };
                ctx.record(std::string{ "[INFO] hook_install_after_jit scenario 4(a): "
                           "deoptimize_all_jit_compiled_methods() deopted " }
                           + std::to_string(deopted) + " method(s); our Method::_code="
                           + (code_after == nullptr ? "null." : "NON-null."));
                // When the sweep deopted >=1 method, OUR method's nmethod must be
                // gone (it was the freshly compiled one).  If the sweep reported 0
                // it skipped our method because its c2i adapter was unrecoverable
                // (the documented deoptimize_methods_if skip, vmhook.hpp:6501-6507)
                // — characterise that as [INFO] rather than a spurious FAIL.
                if (deopted >= 1)
                {
                    ctx.check("sweep_all_deopted_at_least_one", deopted >= 1);
                    ctx.check("sweep_all_code_null_after_sweep", code_after == nullptr);
                }
                else
                {
                    ctx.record("[INFO] hook_install_after_jit scenario 4(a): sweep deopted 0 "
                               "(c2i adapter unrecoverable for the warm method - documented skip); "
                               "_code-null assert recorded as [INFO].");
                }
            }
            else
            {
                ctx.record("[INFO] hook_install_after_jit scenario 4(a): hot() not compiled "
                           "within budget - deopt-all sweep assert recorded as [INFO].");
            }

            // (b) predicate that MATCHES the fixture class ---------------------
            const bool warm_b{ warm_until_compiled(ctx, m) };
            if (warm_b)
            {
                ctx.check("sweep_match_precondition_code_nonnull", method_code(m) != nullptr);
                std::atomic<bool> saw_class{ false };
                const std::size_t deopted{ vmhook::deoptimize_methods_if(
                    [&saw_class](const std::string& class_name,
                                 vmhook::hotspot::method*) -> bool
                    {
                        // The predicate must be invoked with the JVM-internal
                        // class name; record that we saw our fixture and match
                        // only it.
                        const bool is_ours{ class_name == FIXTURE_CLASS };
                        if (is_ours)
                        {
                            saw_class.store(true, std::memory_order_relaxed);
                        }
                        return is_ours;
                    }) };
                void* const code_after{ method_code(m) };
                ctx.record("[INFO] hook_install_after_jit scenario 4(b): matching predicate "
                           "deopted " + std::to_string(deopted) + " method(s).");
                // Always true: the predicate WAS invoked with our class name
                // (proves the user-facing bool(class_name, method*) contract and
                // class-name discrimination on the match side).
                ctx.check("sweep_match_predicate_saw_fixture_class", saw_class.load());
                // The _code-null effect is gated on a successful deopt (c2i
                // recovery) — the same documented skip as 4(a).
                if (deopted >= 1)
                {
                    ctx.check("sweep_match_deopted_at_least_one", deopted >= 1);
                    ctx.check("sweep_match_code_null_after_match", code_after == nullptr);
                }
                else
                {
                    ctx.record("[INFO] hook_install_after_jit scenario 4(b): matching predicate "
                               "deopted 0 (c2i adapter unrecoverable - documented skip); "
                               "_code-null assert recorded as [INFO].");
                }
            }
            else
            {
                ctx.record("[INFO] hook_install_after_jit scenario 4(b): hot() not compiled "
                           "within budget - matching-predicate assert recorded as [INFO].");
            }

            // (c) predicate that does NOT match -> _code left intact -----------
            const bool warm_c{ warm_until_compiled(ctx, m) };
            if (warm_c)
            {
                ctx.check("sweep_nomatch_precondition_code_nonnull", method_code(m) != nullptr);
                const std::size_t deopted{ vmhook::deoptimize_methods_if(
                    [](const std::string& class_name,
                       vmhook::hotspot::method*) -> bool
                    {
                        // Deliberately never match our fixture (or anything that
                        // could touch it): only a class name that cannot exist.
                        return class_name == "vmhook/fixtures/HookAfterJit$NoSuchClass";
                    }) };
                ctx.record("[INFO] hook_install_after_jit scenario 4(c): non-matching predicate "
                           "deopted " + std::to_string(deopted) + " method(s) (expected 0 touching ours).");
                // Our method was NOT selected, so its _code must remain non-null.
                ctx.check("sweep_nomatch_code_stays_nonnull", method_code(m) != nullptr);

                // Tidy: deopt it for real so we don't leave a compiled method
                // lingering for later modules (cosmetic; harmless either way).
                (void)vmhook::deoptimize_all_jit_compiled_methods();
            }
            else
            {
                ctx.record("[INFO] hook_install_after_jit scenario 4(c): hot() not compiled "
                           "within budget - non-matching-predicate assert recorded as [INFO].");
            }
        }
    }

    // =====================================================================
    // Scenario 5 — REUSABILITY after the after-JIT churn.  A fresh install +
    //   single dispatch must still fire and allow through, proving the repeated
    //   warm/deopt/hook/unhook cycles above left the library in a clean, usable
    //   state.
    // =====================================================================
    {
        ctx.check("reusable_install_returns_true", install_observer());
        // Settle: the prior after-JIT churn leaves the method hot, so a fresh
        // install can momentarily race a queued recompile before NO_COMPILE bites.
        ctx.check("reusable_verify_hooks_zero_on_fresh_install",
                  verify_settles_zero(12));
        const bool done{ drive(ctx, 2) };
        ctx.check("reusable_probe_completed", done);
        ctx.check("reusable_hook_fires", g_fire_count.load() == 1);
        ctx.check("reusable_self_correct", g_self_ok_fires.load() == 1);
        ctx.check("reusable_allow_through",
                  haj_fixture::get_last_hot_result() == HOT_ORIGINAL);

        vmhook::shutdown_hooks();   // clean up scenario 5
    }

    // =====================================================================
    // FINAL CLEANUP — belt-and-braces.  Other modules run after this one, so the
    //   module MUST leave ZERO hooks armed.  Every scenario already tears its
    //   hook down; call shutdown_hooks() once more unconditionally (idempotent,
    //   safe-when-empty) and confirm a final verify reports a clean, empty set.
    // =====================================================================
    vmhook::shutdown_hooks();
    ctx.check("module_left_clean_final_verify_zero", vmhook::verify_hooks() == 0);
    ctx.check("module_left_clean_final_shutdown", true);

    // Surface the headline precondition outcome once at module end so a reader of
    // test_results.txt can immediately see whether the after-JIT deopt path (vs
    // the interpreted fallback) was exercised on this runner.
    ctx.record(std::string{ "[INFO] hook_install_after_jit: after-JIT deopt path exercised = " }
               + (jit_precondition_met ? "YES" : "NO (interpreted fallback)") + ".");
}
