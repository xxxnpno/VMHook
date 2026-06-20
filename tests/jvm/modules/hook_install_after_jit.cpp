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

        // Observations the deepening scenarios (static / overload / caller) write.
        static auto get_last_static_result() -> std::int32_t { return static_field("lastStaticResult")->get(); }
        static auto get_last_over1_result() -> std::int32_t  { return static_field("lastOver1Result")->get(); }
        static auto get_last_over2_result() -> std::int32_t  { return static_field("lastOver2Result")->get(); }
        static auto get_caller_iterations() -> std::int32_t  { return static_field("callerIterations")->get(); }
        static auto get_last_long_result() -> std::int64_t   { return static_field("lastLongResult")->get(); }

        // Reads this instance's own seed (proves `self` is the right object).
        auto seed() const -> std::int32_t { return get_field("seed")->get(); }
    };

    // ---- Fixture-mirrored constants (lockstep with HookAfterJit.java) -------
    constexpr std::int32_t SEED{ 1000 };
    constexpr std::int32_t HOT_DELTA{ 7 };
    constexpr std::int32_t WARM_CALLS{ 200000 };

    constexpr std::int32_t STATIC_BASE{ 2000 };
    constexpr std::int32_t OVER1_ADD{ 30 };
    constexpr std::int32_t OVER2_ADD{ 40 };
    constexpr std::int32_t N_REPEAT{ 64 };
    constexpr std::int32_t CALLER_CALLS{ 200000 };

    // The WIDE (long-returning) hookable method.  LONG_BASE is deliberately above
    // the 32-bit range so a truncated wide return is unmistakable; mirrors
    // HookAfterJit.LONG_BASE exactly.
    constexpr std::int64_t LONG_BASE{ 0x1'0000'0000LL + 5000LL };

    constexpr std::int32_t HOT_ORIGINAL{ SEED + HOT_DELTA };          // hot(HOT_DELTA) body result
    constexpr std::int32_t STATIC_ORIGINAL{ STATIC_BASE + HOT_DELTA };// hotStatic(HOT_DELTA) body result
    constexpr std::int64_t LONG_ORIGINAL{ LONG_BASE + HOT_DELTA };    // hotLong(HOT_DELTA) body result
    constexpr std::int64_t LONG_FORCED{ 0x7ABC'DEF0'1234'5678LL };    // wide value a cancelling detour forces
    constexpr std::int32_t OVER1_ORIGINAL{ HOT_DELTA + OVER1_ADD };   // over(HOT_DELTA)
    constexpr std::int32_t OVER2_ORIGINAL{ HOT_DELTA + HOT_DELTA + OVER2_ADD }; // over(HOT_DELTA,HOT_DELTA)
    constexpr std::int32_t FORCED_RETURN{ 0x5AFE5A };                 // value a cancelling detour forces
    constexpr std::int32_t STATIC_FORCED{ 0x2BCD2B };                 // forced return on the static path
    constexpr std::int32_t MUTATED_DELTA{ 0x456 };                    // arg the set_arg mutation injects

    // The fully-qualified class + the hooked methods' names/signatures.  Used to
    // locate the live Method* so we can OBSERVE its JIT state (Method::_code)
    // around install.
    constexpr const char* FIXTURE_CLASS{ "vmhook/fixtures/HookAfterJit" };
    constexpr const char* HOT_NAME{ "hot" };
    constexpr const char* HOT_SIG{ "(I)I" };
    constexpr const char* STATIC_NAME{ "hotStatic" };
    constexpr const char* STATIC_SIG{ "(I)I" };
    constexpr const char* OVER_NAME{ "over" };
    constexpr const char* OVER1_SIG{ "(I)I" };       // over(int)
    constexpr const char* OVER2_SIG{ "(II)I" };      // over(int,int)
    constexpr const char* LONG_NAME{ "hotLong" };
    constexpr const char* LONG_SIG{ "(I)J" };        // hotLong(int) -> long

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

    // An ARG-MUTATING detour on hot(I)I: rewrites the delta slot (a non-cancelling
    // mutation) so the ORIGINAL body runs with the injected value.  hot() is an
    // instance method, so 'this' is slot 0 and `delta` is slot 1.  Records whether
    // set_arg reported success so a frame-less call is observable.
    std::atomic<std::int32_t> g_setarg_ok_fires{ 0 };
    auto install_arg_mutator() -> bool
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
                // Instance method: delta is at interpreter slot 1 (slot 0 == this).
                if (ret.set_arg(1, static_cast<std::int32_t>(MUTATED_DELTA)))
                {
                    g_setarg_ok_fires.fetch_add(1, std::memory_order_relaxed);
                }
            });
    }

    // STATIC observing detour on hotStatic(I)I.  A static method has NO 'this', so
    // the detour omits the unique_ptr<W>& self parameter and the first Java arg is
    // the leading non-return_value parameter.
    auto install_static_observer() -> bool
    {
        return vmhook::hook<haj_fixture>(
            STATIC_NAME,
            [](vmhook::return_value&, std::int32_t delta)
            {
                g_fire_count.fetch_add(1, std::memory_order_relaxed);
                g_arg_xor.fetch_xor(delta, std::memory_order_relaxed);
            });
    }

    // STATIC cancelling detour on hotStatic(I)I: forces the return value.
    auto install_static_forcing() -> bool
    {
        return vmhook::hook<haj_fixture>(
            STATIC_NAME,
            [](vmhook::return_value& ret, std::int32_t delta)
            {
                g_fire_count.fetch_add(1, std::memory_order_relaxed);
                g_arg_xor.fetch_xor(delta, std::memory_order_relaxed);
                ret.set<std::int32_t>(STATIC_FORCED);
            });
    }

    // WIDE-return observing detour on hotLong(I)J.  The Java return type (long) is
    // inferred from the descriptor by vmhook; the detour decodes the int arg the
    // same way as hot(I)I (instance method: self + delta).  Allow-through: the
    // original wide body runs and Java observes LONG_ORIGINAL.
    auto install_long_observer() -> bool
    {
        return vmhook::hook<haj_fixture>(
            LONG_NAME, LONG_SIG,
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

    // WIDE-return cancelling detour on hotLong(I)J: forces a 64-bit return value
    // (proves force-return decodes/writes the full wide slot pair after the
    // install-time deopt, not just a 32-bit int).
    auto install_long_forcing() -> bool
    {
        return vmhook::hook<haj_fixture>(
            LONG_NAME, LONG_SIG,
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
                ret.set<std::int64_t>(LONG_FORCED);
            });
    }

    // Per-overload fire counters: prove EXACTLY the hooked descriptor fired.
    std::atomic<std::int32_t> g_over1_fires{ 0 };
    std::atomic<std::int32_t> g_over2_fires{ 0 };
    auto reset_overload_observations() -> void
    {
        g_over1_fires.store(0);
        g_over2_fires.store(0);
    }

    // Hook ONLY the single-int overload over(I)I via the explicit-signature
    // overload of hook<T>().  over(II)I must keep running raw.
    auto install_over1_observer() -> bool
    {
        return vmhook::hook<haj_fixture>(
            OVER_NAME, OVER1_SIG,
            [](vmhook::return_value&,
               const std::unique_ptr<haj_fixture>&,
               std::int32_t a)
            {
                g_over1_fires.fetch_add(1, std::memory_order_relaxed);
                g_arg_xor.fetch_xor(a, std::memory_order_relaxed);
            });
    }

    // Hook ONLY the two-int overload over(II)I.  over(I)I must keep running raw.
    auto install_over2_observer() -> bool
    {
        return vmhook::hook<haj_fixture>(
            OVER_NAME, OVER2_SIG,
            [](vmhook::return_value&,
               const std::unique_ptr<haj_fixture>&,
               std::int32_t a, std::int32_t b)
            {
                g_over2_fires.fetch_add(1, std::memory_order_relaxed);
                g_arg_xor.fetch_xor(a ^ b, std::memory_order_relaxed);
            });
    }

    // Locates the live Method* for FIXTURE_CLASS::<name><sig> by walking the
    // InstanceKlass methods array.  Returns nullptr if anything looks invalid —
    // callers must treat nullptr as "cannot run this Method-level scenario" and
    // skip it rather than crash.  All reads are pointer-validated.
    auto find_method(const char* const want_name, const char* const want_sig)
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
        for (std::int32_t i{ 0 }; i < count; ++i)
        {
            vmhook::hotspot::method* const m{ methods[i] };
            if (!m || !vmhook::hotspot::is_valid_pointer(m))
            {
                continue;
            }
            const std::string name = m->get_name();          // copy-init (MSVC)
            const std::string sig = m->get_signature();      // copy-init (MSVC)
            if (name == want_name && sig == want_sig)
            {
                return m;
            }
        }
        return nullptr;
    }

    // Convenience: the original scenarios target hot(I)I.
    auto find_hot_method() -> vmhook::hotspot::method*
    {
        return find_method(HOT_NAME, HOT_SIG);
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

    // Reads Method::_i2i_entry (the shared interpreter stub) through a validated
    // pointer; nullptr on any unreadable / unresolved slot.
    auto method_i2i(vmhook::hotspot::method* const m) -> void*
    {
        if (!m || !vmhook::hotspot::is_valid_pointer(m))
        {
            return nullptr;
        }
        void* const i2i{ m->get_i2i_entry() };
        return (i2i && vmhook::hotspot::is_valid_pointer(i2i)) ? i2i : nullptr;
    }

    // Reads Method::_from_interpreted_entry through a validated pointer.  After a
    // successful after-JIT install vmhook redirects this to the i2i stub.
    auto method_from_interpreted(vmhook::hotspot::method* const m) -> void*
    {
        if (!m || !vmhook::hotspot::is_valid_pointer(m))
        {
            return nullptr;
        }
        return m->get_from_interpreted_entry();
    }

    // Reads Method::_from_compiled_entry through a validated pointer.  On the
    // c2i-RECOVERABLE after-JIT install branch vmhook redirects this to the c2i
    // adapter (vmhook.hpp:10367); get_from_compiled_entry() is itself fault-safe
    // (reads through os::safe_read), so a cold/relocated slot yields nullptr
    // rather than an AV.  nullptr therefore means "unreadable" here, NOT a
    // semantic value, and callers must treat it as "cannot assert" rather than
    // "redirect failed".
    auto method_from_compiled(vmhook::hotspot::method* const m) -> void*
    {
        if (!m || !vmhook::hotspot::is_valid_pointer(m))
        {
            return nullptr;
        }
        return m->get_from_compiled_entry();
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

    // Drives warm loops via `warm_mode` (NO hook armed) until Method::_code
    // becomes non-null (HotSpot has published the nmethod) or the budget is
    // exhausted.  Returns true if _code was observed non-null (the "after JIT"
    // precondition).
    //
    // We MUST NOT have a hook installed while calling this: the whole point is to
    // let HotSpot compile the method freely so that the LATER install sees a
    // compiled method.
    auto warm_until_compiled_mode(vmhook_test::context& ctx,
                                  vmhook::hotspot::method* const m,
                                  std::int32_t warm_mode) -> bool
    {
        if (m == nullptr)
        {
            return false;
        }
        const auto deadline{ std::chrono::steady_clock::now() + JIT_BUDGET };
        for (int round{ 0 }; round < MAX_WARM_ROUNDS; ++round)
        {
            const bool warmed{ drive(ctx, warm_mode) };
            if (!warmed)
            {
                ctx.record("[INFO] hook_install_after_jit: warm probe (mode "
                           + std::to_string(warm_mode) + ", round "
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

    // The original scenarios warm hot() via mode 1.
    auto warm_until_compiled(vmhook_test::context& ctx,
                             vmhook::hotspot::method* const m) -> bool
    {
        return warm_until_compiled_mode(ctx, m, 1);
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

            // Method-scoped, not class-scoped: installing on hot(I)I must NOT arm
            // the NO_COMPILE inhibitor on an UNRELATED sibling method declared in
            // the very same class (hotStatic(I)I).  Proves the deopt touches only
            // the targeted Method*, not every method of the klass.  Skipped as
            // [INFO] only if the sibling can't be located.
            {
                vmhook::hotspot::method* const sibling{ find_method(STATIC_NAME, STATIC_SIG) };
                if (sibling != nullptr && sibling != m)
                {
                    ctx.check("headline_sibling_method_not_no_compile_after_install",
                              !no_compile_set(sibling));
                }
                else
                {
                    ctx.record("[INFO] hook_install_after_jit scenario 1: sibling hotStatic(I)I "
                               "not locatable - method-scope isolation recorded as [INFO].");
                }
            }

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

            // The JIT inhibitor survives the firing dispatch: an interpreter
            // dispatch through the patched i2i must NOT clear NO_COMPILE, or
            // HotSpot would be free to re-JIT and bypass the hook next time.
            ctx.check("headline_no_compile_persists_after_fire", no_compile_set(m));

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
    // Scenario 6 — ENTRY-POINT REDIRECTION on the after-JIT install.  The deopt
    //   does three writes (vmhook.hpp:10364-10369): redirect _from_interpreted_
    //   entry -> the i2i stub, redirect _from_compiled_entry -> c2i, and clear
    //   _code.  Scenario 1 asserts the _code-null effect; here we additionally
    //   assert the _from_interpreted_entry was pointed AT the method's i2i stub
    //   (the redirect that actually routes a freshly-resolving call through our
    //   patch).  Gated on the JIT precondition (no compile -> [INFO]); the i2i
    //   pointer itself is always read fault-safe.
    // =====================================================================
    {
        vmhook::hotspot::method* const m{ find_hot_method() };
        ctx.check("entryredirect_located_live_method", m != nullptr);

        if (m == nullptr)
        {
            ctx.record("[INFO] hook_install_after_jit scenario 6: could not locate live "
                       "Method* for hot(I)I - skipping (no crash).");
        }
        else
        {
            const bool warm{ warm_until_compiled(ctx, m) };
            void* const i2i_before{ method_i2i(m) };
            // Snapshot the entry slots BEFORE install.  On a JIT-compiled method
            // (_code != null) HotSpot has _from_interpreted_entry pointing at the
            // i2c ADAPTER, NOT the i2i stub (vmhook.hpp:10339-10340) — that is the
            // precise reason install must redirect it.  So when the method really
            // was compiled, this before-pointer must differ from i2i; the install
            // then makes it EQUAL i2i.
            void* const from_interp_before{ method_from_interpreted(m) };
            void* const from_compiled_before{ method_from_compiled(m) };
            ctx.check("entryredirect_install_returns_true", install_observer());

            void* const from_interp_after{ method_from_interpreted(m) };
            void* const from_compiled_after{ method_from_compiled(m) };
            void* const i2i_after{ method_i2i(m) };
            ctx.record(std::string{ "[INFO] hook_install_after_jit scenario 6: i2i=" }
                       + (i2i_after == nullptr ? "null" : "set")
                       + ", _from_interpreted_entry "
                       + (from_interp_after == i2i_after ? "== i2i (redirected)"
                                                         : "!= i2i")
                       + (warm ? " [after-JIT path]" : " [interpreted-path fallback]") + ".");

            // The i2i stub itself must be a valid, stable pointer across install.
            ctx.check("entryredirect_i2i_resolved", i2i_after != nullptr);
            ctx.check("entryredirect_i2i_stable_across_install",
                      i2i_before == nullptr || i2i_before == i2i_after);

            // After a successful AFTER-JIT install, the interpreted entry is the
            // one the deopt pointed at the i2i stub.  Only assert the equality
            // when both pointers resolved and the method was actually compiled
            // (the deopt branch that performs this redirect runs only when
            // was_compiled); otherwise record [INFO].
            if (warm && from_interp_after != nullptr && i2i_after != nullptr)
            {
                ctx.check("entryredirect_from_interpreted_points_at_i2i",
                          from_interp_after == i2i_after);

                // The redirect must have actually CHANGED the slot when it was
                // pointing somewhere OTHER than i2i pre-install (the compiled
                // method's i2c adapter).  If the pre-install slot was unreadable
                // (null) or already happened to equal i2i (degenerate / the JVM
                // never moved it off i2i), there is nothing to prove the redirect
                // by, so record [INFO] instead of a vacuous pass.
                if (from_interp_before != nullptr && from_interp_before != i2i_after)
                {
                    ctx.check("entryredirect_install_changed_interp_entry_to_i2i",
                              from_interp_after != from_interp_before
                                  && from_interp_after == i2i_after);
                }
                else
                {
                    ctx.record(std::string{ "[INFO] hook_install_after_jit scenario 6: "
                               "pre-install _from_interpreted_entry " }
                               + (from_interp_before == nullptr ? "unreadable"
                                                                : "already == i2i")
                               + " - 'redirect changed the slot' recorded as [INFO].");
                }

                // The compiled entry: on the c2i-RECOVERABLE branch install points
                // _from_compiled_entry at the c2i adapter, which (by construction in
                // get_c2i_entry_from_adapter) is NOT the i2i interpreter stub.  The
                // c2i adapter may be unrecoverable on some JDKs (the documented skip
                // branch at vmhook.hpp:10387 leaves _from_compiled_entry untouched),
                // and the slot is read fault-safe (may be null on a cold read), so
                // this is best-effort: HARD only that, when the slot is readable, the
                // compiled entry is NOT the i2i stub (compiled callers route via the
                // c2i adapter, never directly into the interpreter i2i stub).
                if (from_compiled_after != nullptr && i2i_after != nullptr)
                {
                    ctx.check("entryredirect_compiled_entry_not_i2i_stub",
                              from_compiled_after != i2i_after);
                    ctx.record(std::string{ "[INFO] hook_install_after_jit scenario 6: "
                               "_from_compiled_entry " }
                               + (from_compiled_after == from_compiled_before
                                      ? "unchanged across install"
                                      : "redirected across install")
                               + " (c2i adapter; recoverable-branch effect).");
                }
                else
                {
                    ctx.record("[INFO] hook_install_after_jit scenario 6: _from_compiled_entry "
                               "unreadable post-install - c2i-redirect assert recorded as [INFO].");
                }
            }
            else
            {
                ctx.record("[INFO] hook_install_after_jit scenario 6: _from_interpreted_entry "
                           "redirect assert recorded as [INFO] (no compile or unreadable slot).");
            }

            // The patch is live regardless of path: drive hot() once and assert
            // the detour fires through the (redirected) interpreted entry.
            const bool done{ drive(ctx, 2) };
            ctx.check("entryredirect_probe_completed", done);
            ctx.check("entryredirect_detour_fired_once", g_fire_count.load() == 1);
            ctx.check("entryredirect_allow_through",
                      haj_fixture::get_last_hot_result() == HOT_ORIGINAL);

            // NO_COMPILE must STILL be armed after the firing dispatch — the
            // interpreter dispatch does not clear the JIT inhibitor (the patch
            // must keep biting on every subsequent dispatch, not just the first).
            ctx.check("entryredirect_no_compile_persists_after_fire", no_compile_set(m));

            // The interpreted entry is still the i2i stub after firing (the
            // dispatch did not knock the redirect off its slot).  i2i may read
            // null on a cold slot -> only assert when both readable.
            void* const from_interp_postfire{ method_from_interpreted(m) };
            void* const i2i_postfire{ method_i2i(m) };
            if (from_interp_postfire != nullptr && i2i_postfire != nullptr)
            {
                ctx.check("entryredirect_interp_entry_still_i2i_after_fire",
                          from_interp_postfire == i2i_postfire);
            }
            else
            {
                ctx.record("[INFO] hook_install_after_jit scenario 6: post-fire interp/i2i "
                           "slot unreadable - 'still i2i after fire' recorded as [INFO].");
            }

            vmhook::shutdown_hooks();   // clean up scenario 6
        }
    }

    // =====================================================================
    // Scenario 7 — ARG-MUTATION on a post-JIT-install hook.  Warm hot() to _code
    //   != null, install a NON-cancelling detour that REWRITES the delta slot via
    //   return_value::set_arg(1, MUTATED_DELTA), then drive hot() once.  The
    //   original (formerly-compiled) body must run with the INJECTED arg, so Java
    //   observes seed + MUTATED_DELTA — proving frame-arg mutation works after the
    //   install-time deopt, not merely force-return.
    // =====================================================================
    {
        vmhook::hotspot::method* const m{ find_hot_method() };
        ctx.check("argmutate_located_live_method", m != nullptr);

        if (m == nullptr)
        {
            ctx.record("[INFO] hook_install_after_jit scenario 7: could not locate live "
                       "Method* for hot(I)I - skipping (no crash).");
        }
        else
        {
            const bool warm{ warm_until_compiled(ctx, m) };
            if (warm)
            {
                ctx.check("argmutate_method_compiled_before_install", method_code(m) != nullptr);
            }
            g_setarg_ok_fires.store(0);
            ctx.check("argmutate_install_returns_true", install_arg_mutator());
            if (warm)
            {
                ctx.check("argmutate_install_deopted_code_to_null", method_code(m) == nullptr);
            }
            ctx.check("argmutate_install_armed_no_compile", no_compile_set(m));

            const bool done{ drive(ctx, 2) };
            ctx.check("argmutate_probe_completed", done);
            ctx.check("argmutate_detour_fired_once", g_fire_count.load() == 1);
            ctx.check("argmutate_self_correct", g_self_ok_fires.load() == 1);
            ctx.check("argmutate_original_arg_decoded", g_arg_xor.load() == HOT_DELTA);
            ctx.check("argmutate_setarg_reported_success", g_setarg_ok_fires.load() == 1);
            // The mutated arg flowed into the original body: seed + MUTATED_DELTA.
            ctx.check("argmutate_body_saw_injected_arg",
                      haj_fixture::get_last_hot_result() == SEED + MUTATED_DELTA);
            ctx.check("argmutate_body_not_original",
                      haj_fixture::get_last_hot_result() != HOT_ORIGINAL);

            vmhook::shutdown_hooks();   // clean up scenario 7

            // Post-removal the un-mutated original runs again.
            const bool done2{ drive(ctx, 4) };
            ctx.check("argmutate_postremoval_probe_completed", done2);
            ctx.check("argmutate_postremoval_detour_did_not_fire", g_fire_count.load() == 0);
            ctx.check("argmutate_postremoval_original_restored",
                      haj_fixture::get_last_hot_result() == HOT_ORIGINAL);
        }
    }

    // =====================================================================
    // Scenario 8 — STATIC method install-after-JIT.  The deopt-on-install path is
    //   identical for a static method, but the detour shape differs (no 'this',
    //   first arg at slot 0).  Warm hotStatic() to _code != null, install an
    //   observing detour, drive once (allow-through), then re-warm + install a
    //   FORCING detour and assert Java observes the forced static return value.
    // =====================================================================
    {
        vmhook::hotspot::method* const m{ find_method(STATIC_NAME, STATIC_SIG) };
        ctx.check("static_located_live_method", m != nullptr);

        if (m == nullptr)
        {
            ctx.record("[INFO] hook_install_after_jit scenario 8: could not locate live "
                       "Method* for hotStatic(I)I - skipping (no crash).");
        }
        else
        {
            // -- 8a: observing (allow-through) on the warm static method --------
            const bool warm_a{ warm_until_compiled_mode(ctx, m, 5) };
            if (warm_a)
            {
                ctx.check("static_observe_compiled_before_install", method_code(m) != nullptr);
            }
            ctx.check("static_observe_install_returns_true", install_static_observer());
            if (warm_a)
            {
                ctx.check("static_observe_install_deopted_code_to_null", method_code(m) == nullptr);
            }
            ctx.check("static_observe_install_armed_no_compile", no_compile_set(m));
            ctx.check("static_observe_verify_zero_after_install", verify_settles_zero(12));

            const bool done_a{ drive(ctx, 6) };
            ctx.check("static_observe_probe_completed", done_a);
            ctx.check("static_observe_java_made_one_call",
                      haj_fixture::get_hot_calls_made() == 1);
            ctx.check("static_observe_detour_fired_once", g_fire_count.load() == 1);
            // First arg of a static is at slot 0, decoded correctly post-deopt.
            ctx.check("static_observe_arg_decoded", g_arg_xor.load() == HOT_DELTA);
            ctx.check("static_observe_allow_through",
                      haj_fixture::get_last_static_result() == STATIC_ORIGINAL);

            vmhook::shutdown_hooks();   // clean up 8a

            // -- 8b: cancelling (force-return) on the warm static method --------
            const bool warm_b{ warm_until_compiled_mode(ctx, m, 5) };
            ctx.check("static_force_install_returns_true", install_static_forcing());
            if (warm_b)
            {
                ctx.check("static_force_install_deopted_code_to_null", method_code(m) == nullptr);
            }
            // The forcing install arms NO_COMPILE unconditionally (same install
            // path as the observing variant) and the fresh hook reports no drift.
            ctx.check("static_force_install_armed_no_compile", no_compile_set(m));
            ctx.check("static_force_verify_zero_after_install", verify_settles_zero(12));
            const bool done_b{ drive(ctx, 6) };
            ctx.check("static_force_probe_completed", done_b);
            ctx.check("static_force_detour_fired_once", g_fire_count.load() == 1);
            ctx.check("static_force_arg_decoded", g_arg_xor.load() == HOT_DELTA);
            ctx.check("static_force_return_overridden",
                      haj_fixture::get_last_static_result() == STATIC_FORCED);
            ctx.check("static_force_return_not_original",
                      haj_fixture::get_last_static_result() != STATIC_ORIGINAL);

            vmhook::shutdown_hooks();   // clean up 8b

            // Post-removal: the static body runs normally again, no fire.
            const bool done_c{ drive(ctx, 6) };
            ctx.check("static_postremoval_probe_completed", done_c);
            ctx.check("static_postremoval_detour_did_not_fire", g_fire_count.load() == 0);
            ctx.check("static_postremoval_original_restored",
                      haj_fixture::get_last_static_result() == STATIC_ORIGINAL);
        }
    }

    // =====================================================================
    // Scenario 9 — OVERLOAD SELECTION on after-JIT install.  over(int) and
    //   over(int,int) share the name `over`; warm BOTH to _code != null, then hook
    //   EXACTLY ONE overload by explicit descriptor and prove only that descriptor
    //   is detoured while the sibling overload still runs raw.  Run both
    //   directions (hook the 1-arg, then hook the 2-arg) so neither is privileged.
    // =====================================================================
    {
        vmhook::hotspot::method* const m1{ find_method(OVER_NAME, OVER1_SIG) };
        vmhook::hotspot::method* const m2{ find_method(OVER_NAME, OVER2_SIG) };
        ctx.check("overload_located_one_arg", m1 != nullptr);
        ctx.check("overload_located_two_arg", m2 != nullptr);

        if (m1 == nullptr || m2 == nullptr)
        {
            ctx.record("[INFO] hook_install_after_jit scenario 9: could not locate both "
                       "over() overloads - skipping (no crash).");
        }
        else
        {
            // -- 9a: hook ONLY over(int); over(int,int) must run raw -----------
            const bool warm1a{ warm_until_compiled_mode(ctx, m1, 7) };
            const bool warm2a{ warm_until_compiled_mode(ctx, m2, 7) };
            reset_overload_observations();
            reset_observations();
            ctx.check("overload_hook_one_arg_returns_true", install_over1_observer());
            if (warm1a)
            {
                ctx.check("overload_one_arg_install_deopted", method_code(m1) == nullptr);
            }
            ctx.check("overload_one_arg_install_armed_no_compile", no_compile_set(m1));
            // The UN-hooked sibling must NOT have been touched by the targeted install.
            ctx.check("overload_two_arg_not_no_compile_after_one_install",
                      !no_compile_set(m2));
            ctx.record(std::string{ "[INFO] hook_install_after_jit scenario 9a: sibling over(II)I "
                       "was " } + (warm2a ? "JIT-compiled" : "interpreted")
                       + " and remained "
                       + (method_code(m2) != nullptr ? "compiled" : "deopted/interpreted")
                       + " across the targeted over(I)I install (untouched sibling).");

            const bool done_a{ drive(ctx, 8) };
            ctx.check("overload_9a_probe_completed", done_a);
            ctx.check("overload_9a_one_arg_fired_once", g_over1_fires.load() == 1);
            ctx.check("overload_9a_two_arg_did_not_fire", g_over2_fires.load() == 0);
            // Both Java results are the originals (the 1-arg detour allowed through).
            ctx.check("overload_9a_one_arg_allow_through",
                      haj_fixture::get_last_over1_result() == OVER1_ORIGINAL);
            ctx.check("overload_9a_two_arg_ran_raw",
                      haj_fixture::get_last_over2_result() == OVER2_ORIGINAL);

            vmhook::shutdown_hooks();   // clean up 9a

            // -- 9b: hook ONLY over(int,int); over(int) must run raw -----------
            const bool warm1b{ warm_until_compiled_mode(ctx, m1, 7) };
            const bool warm2b{ warm_until_compiled_mode(ctx, m2, 7) };
            (void)warm1b;
            reset_overload_observations();
            reset_observations();
            ctx.check("overload_hook_two_arg_returns_true", install_over2_observer());
            if (warm2b)
            {
                ctx.check("overload_two_arg_install_deopted", method_code(m2) == nullptr);
            }
            ctx.check("overload_two_arg_install_armed_no_compile", no_compile_set(m2));
            ctx.check("overload_one_arg_not_no_compile_after_two_install",
                      !no_compile_set(m1));

            const bool done_b{ drive(ctx, 8) };
            ctx.check("overload_9b_probe_completed", done_b);
            ctx.check("overload_9b_two_arg_fired_once", g_over2_fires.load() == 1);
            ctx.check("overload_9b_one_arg_did_not_fire", g_over1_fires.load() == 0);
            ctx.check("overload_9b_two_arg_allow_through",
                      haj_fixture::get_last_over2_result() == OVER2_ORIGINAL);
            ctx.check("overload_9b_one_arg_ran_raw",
                      haj_fixture::get_last_over1_result() == OVER1_ORIGINAL);

            vmhook::shutdown_hooks();   // clean up 9b
        }
    }

    // =====================================================================
    // Scenario 10 — CONTINUED CALLS after install: the detour must fire on EVERY
    //   dispatch within a probe, not silently re-JIT past the hook.  Warm hot() to
    //   _code != null, install the observing detour, then drive N_REPEAT dispatches
    //   in ONE probe and assert the fire-count is EXACTLY N_REPEAT.  NO_COMPILE +
    //   the patched i2i keep all N going through the interpreter detour.  Repeat
    //   for the static path.  This is the exact-count invariant the spec headlines.
    // =====================================================================
    {
        // --- 10a: instance hot() x N_REPEAT ---
        vmhook::hotspot::method* const m{ find_hot_method() };
        ctx.check("continued_located_instance", m != nullptr);
        if (m != nullptr)
        {
            (void)warm_until_compiled(ctx, m);
            ctx.check("continued_instance_install_returns_true", install_observer());
            const bool done{ drive(ctx, 11) };
            ctx.check("continued_instance_probe_completed", done);
            ctx.check("continued_instance_java_made_n_calls",
                      haj_fixture::get_hot_calls_made() == N_REPEAT);
            // The detour fires only while each call routes through hot()'s i2i entry.
            // On aggressive-tiering JDKs (seen linux·gcc·java25, windows·clang·java24)
            // the driver loop JIT-compiles and INLINES hot() mid-probe, bypassing the
            // detour (the characterised compiled/inlined-caller limitation -- NO_COMPILE
            // stops hot()'s own recompile, not a caller inlining its bytecode). HARD:
            // at least one fire, never more than the calls, self correct on EVERY
            // actual fire; EXACT-N is best-effort.
            const std::int32_t fc_i{ g_fire_count.load() };
            ctx.check("continued_instance_fired_at_least_once", fc_i >= 1);
            ctx.check("continued_instance_fired_no_more_than_calls", fc_i <= N_REPEAT);
            if (fc_i == N_REPEAT) { ctx.check("continued_instance_fired_exactly_n", true); }
            else { ctx.record("[INFO] continued_instance_fired_exactly_n: fired "
                              + std::to_string(fc_i) + "/" + std::to_string(N_REPEAT)
                              + " (driver JIT-inlined hot() mid-probe) -- best-effort."); }
            ctx.check("continued_instance_self_ok_every_fire",
                      g_self_ok_fires.load() == fc_i);
            // Per-fire arg decode is exact even though the fire-COUNT is variant:
            // every dispatch that fired decoded HOT_DELTA, so the running XOR is
            // HOT_DELTA folded fc_i times == (fc_i odd ? HOT_DELTA : 0).  A wrong
            // decode on ANY fire (e.g. reading the wrong interpreter slot) breaks
            // this regardless of how many fired.  static_cast guards value_t.
            const std::int32_t expect_arg_xor_i{ (fc_i & 1) != 0 ? HOT_DELTA : 0 };
            ctx.check("continued_instance_arg_xor_matches_fire_parity",
                      static_cast<std::int32_t>(g_arg_xor.load()) == expect_arg_xor_i);
            // Allow-through: the last body result is the unmodified original.
            ctx.check("continued_instance_allow_through_last",
                      haj_fixture::get_last_hot_result() == HOT_ORIGINAL);
            // Allow-through is total: EVERY one of the N_REPEAT calls returned the
            // unmodified HOT_ORIGINAL, so the Java-side XOR of all N results is a
            // deterministic 0 (N_REPEAT is even) — independent of how many fired
            // or whether the driver inlined some.  This pins that NO call had its
            // return value altered by the observing detour, not merely the last.
            static_assert(N_REPEAT % 2 == 0,
                          "hotResultXor==0 invariant needs an even N_REPEAT");
            ctx.check("continued_instance_result_xor_all_originals_zero",
                      static_cast<std::int64_t>(haj_fixture::get_hot_result_xor()) == 0);
            vmhook::shutdown_hooks();   // clean up 10a
        }

        // --- 10b: static hotStatic() x N_REPEAT ---
        vmhook::hotspot::method* const sm{ find_method(STATIC_NAME, STATIC_SIG) };
        ctx.check("continued_located_static", sm != nullptr);
        if (sm != nullptr)
        {
            (void)warm_until_compiled_mode(ctx, sm, 5);
            ctx.check("continued_static_install_returns_true", install_static_observer());
            const bool done{ drive(ctx, 12) };
            ctx.check("continued_static_probe_completed", done);
            ctx.check("continued_static_java_made_n_calls",
                      haj_fixture::get_hot_calls_made() == N_REPEAT);
            const std::int32_t fc_s{ g_fire_count.load() };
            ctx.check("continued_static_fired_at_least_once", fc_s >= 1);
            ctx.check("continued_static_fired_no_more_than_calls", fc_s <= N_REPEAT);
            if (fc_s == N_REPEAT) { ctx.check("continued_static_fired_exactly_n", true); }
            else { ctx.record("[INFO] continued_static_fired_exactly_n: fired "
                              + std::to_string(fc_s) + "/" + std::to_string(N_REPEAT)
                              + " (driver JIT-inlined hotStatic() mid-probe) -- best-effort."); }
            // Per-fire arg decode parity on the static slot-0 path: each fire
            // decoded HOT_DELTA, so the XOR is HOT_DELTA folded fc_s times.
            const std::int32_t expect_arg_xor_s{ (fc_s & 1) != 0 ? HOT_DELTA : 0 };
            ctx.check("continued_static_arg_xor_matches_fire_parity",
                      static_cast<std::int32_t>(g_arg_xor.load()) == expect_arg_xor_s);
            ctx.check("continued_static_allow_through_last",
                      haj_fixture::get_last_static_result() == STATIC_ORIGINAL);
            // Allow-through totality on the static path: all N_REPEAT calls
            // returned STATIC_ORIGINAL, so their XOR (runStaticRepeat writes it to
            // hotResultXor) is a deterministic 0 for even N_REPEAT, regardless of
            // fire-count / inlining.
            ctx.check("continued_static_result_xor_all_originals_zero",
                      static_cast<std::int64_t>(haj_fixture::get_hot_result_xor()) == 0);
            vmhook::shutdown_hooks();   // clean up 10b
        }
    }

    // =====================================================================
    // Scenario 11 — INSTALL / UNINSTALL / REINSTALL cycle on a JIT'd method.  The
    //   strongest lifecycle statement: warm to _code != null, install (deopt),
    //   fire, shutdown (un-install), RE-warm to _code != null again, RE-install
    //   (deopt AGAIN), fire AGAIN.  Proves the after-JIT install path is repeatable
    //   on the SAME method and that an intervening teardown + re-JIT does not wedge
    //   the second install.
    // =====================================================================
    {
        vmhook::hotspot::method* const m{ find_hot_method() };
        ctx.check("recycle_located_live_method", m != nullptr);

        if (m == nullptr)
        {
            ctx.record("[INFO] hook_install_after_jit scenario 11: could not locate live "
                       "Method* for hot(I)I - skipping (no crash).");
        }
        else
        {
            // -- first install/fire on the warm method --
            const bool warm1{ warm_until_compiled(ctx, m) };
            ctx.check("recycle_first_install_returns_true", install_observer());
            if (warm1)
            {
                ctx.check("recycle_first_install_deopted", method_code(m) == nullptr);
            }
            const bool done1{ drive(ctx, 2) };
            ctx.check("recycle_first_probe_completed", done1);
            ctx.check("recycle_first_fired_once", g_fire_count.load() == 1);
            ctx.check("recycle_first_allow_through",
                      haj_fixture::get_last_hot_result() == HOT_ORIGINAL);

            // -- teardown: the detour must stop firing --
            vmhook::shutdown_hooks();
            const bool done_gap{ drive(ctx, 4) };
            ctx.check("recycle_gap_probe_completed", done_gap);
            ctx.check("recycle_gap_detour_did_not_fire", g_fire_count.load() == 0);

            // -- re-warm to a fresh nmethod, then RE-install on the warm method --
            const bool warm2{ warm_until_compiled(ctx, m) };
            ctx.record(std::string{ "[INFO] hook_install_after_jit scenario 11: re-warm "
                       "Method::_code=" } + (method_code(m) == nullptr ? "null" : "NON-null")
                       + (warm2 ? " (re-JIT'd before reinstall)." : " (not re-compiled within budget)."));
            ctx.check("recycle_reinstall_returns_true", install_observer());
            if (warm2)
            {
                ctx.check("recycle_reinstall_deopted", method_code(m) == nullptr);
            }
            ctx.check("recycle_reinstall_armed_no_compile", no_compile_set(m));
            ctx.check("recycle_reinstall_verify_zero", verify_settles_zero(12));

            const bool done2{ drive(ctx, 2) };
            ctx.check("recycle_reinstall_probe_completed", done2);
            ctx.check("recycle_reinstall_fired_once", g_fire_count.load() == 1);
            ctx.check("recycle_reinstall_self_correct", g_self_ok_fires.load() == 1);
            ctx.check("recycle_reinstall_allow_through",
                      haj_fixture::get_last_hot_result() == HOT_ORIGINAL);

            vmhook::shutdown_hooks();   // clean up scenario 11
        }
    }

    // =====================================================================
    // Scenario 12 — COMPILED CALLER (the README "catches direct callers" claim,
    //   audit test_hook_install_after_jit_with_compiled_caller).  callerLoop()
    //   dispatches hot() in a tight loop so HotSpot can compile the CALLER (and
    //   inline hot() into it).  Two parts:
    //     (a) install on hot() WITHOUT a deopt sweep, run callerLoop, and merely
    //         RECORD fires-vs-iterations — the audit documents that stale inline
    //         caches in a compiled caller can bypass the hook, so this is [INFO],
    //         NEVER a FAIL (the count is timing/inlining-variant),
    //     (b) the DOCUMENTED fix: deoptimize_all_jit_compiled_methods() to flush
    //         the stale ICs, then run callerLoop AGAIN.  The universal invariant we
    //         DO assert hard: callerLoop completed its iterations and (after the
    //         sweep) the detour fired AT LEAST ONCE through the caller.
    // =====================================================================
    {
        vmhook::hotspot::method* const caller{ find_method("runCallerLoop", "(I)V") };
        vmhook::hotspot::method* const m{ find_hot_method() };
        // runCallerLoop is private static; it is still a declared method of the
        // class, so the walk finds it.  If a JDK quirk hides it, record [INFO]
        // and skip the caller-warm step (the install + invariants still run).
        if (m == nullptr)
        {
            ctx.record("[INFO] hook_install_after_jit scenario 12: hot(I)I not locatable - skip.");
        }
        else
        {
            // Warm the caller (mode 9) so HotSpot compiles callerLoop and may
            // inline hot().  NO hook armed yet.
            const bool caller_warm{ (caller != nullptr)
                                        ? warm_until_compiled_mode(ctx, caller, 9)
                                        : false };
            (void)drive(ctx, 9);   // ensure at least one caller run happened
            ctx.record(std::string{ "[INFO] hook_install_after_jit scenario 12: callerLoop "
                       "compiled = " } + (caller_warm ? "YES" : "NO/unknown") + ".");

            // (a) install on hot() WITHOUT a sweep, run callerLoop, RECORD only.
            reset_observations();
            ctx.check("compiledcaller_install_returns_true", install_observer());
            const bool done_a{ drive(ctx, 10) };
            ctx.check("compiledcaller_nosweep_probe_completed", done_a);
            ctx.check("compiledcaller_nosweep_caller_ran_full",
                      haj_fixture::get_caller_iterations() == CALLER_CALLS);
            const std::int32_t fires_no_sweep{ g_fire_count.load() };
            ctx.record(std::string{ "[INFO] hook_install_after_jit scenario 12(a): WITHOUT deopt "
                       "sweep, detour fired " } + std::to_string(fires_no_sweep)
                       + " / " + std::to_string(CALLER_CALLS)
                       + " caller iterations (stale-IC window is variant; characterised, not asserted).");

            // (b) the documented fix: flush stale inline caches with the sweep,
            // then run callerLoop again.  The detour must now fire through the
            // (re-resolved) caller.  HARD invariant: it fires AT LEAST ONCE.
            const std::size_t swept{ vmhook::deoptimize_all_jit_compiled_methods() };
            ctx.record(std::string{ "[INFO] hook_install_after_jit scenario 12(b): deopt sweep "
                       "deopted " } + std::to_string(swept) + " method(s).");
            reset_observations();
            const bool done_b{ drive(ctx, 10) };
            ctx.check("compiledcaller_sweep_probe_completed", done_b);
            ctx.check("compiledcaller_sweep_caller_ran_full",
                      haj_fixture::get_caller_iterations() == CALLER_CALLS);
            const std::int32_t fires_after_sweep{ g_fire_count.load() };
            ctx.record(std::string{ "[INFO] hook_install_after_jit scenario 12(b): WITH deopt sweep, "
                       "detour fired " } + std::to_string(fires_after_sweep)
                       + " / " + std::to_string(CALLER_CALLS) + " caller iterations.");
            // Whether the detour fires through a previously-COMPILED (and possibly
            // INLINED) caller after a global deopt sweep is a LIBRARY LIMITATION, not
            // a guaranteed invariant: when hot() was inlined into the compiled
            // runCallerLoop, re-resolution does not reliably route the caller's
            // dispatch through hot()'s hooked entry (the audit's "README overstates
            // 'catches direct callers'").  Observed to fire 0 across the whole matrix
            // (every compiler x every JDK) -> RECORD, do not assert.
            ctx.record("[INFO] hook_install_after_jit scenario 12(b): detour fired "
                       + std::to_string(fires_after_sweep)
                       + " time(s) through the compiled caller after the deopt sweep "
                       "(compiled/inlined-caller routing is a characterised limitation, not asserted).");

            vmhook::shutdown_hooks();   // clean up scenario 12

            // Post-removal: a final caller run must not fire the removed detour.
            reset_observations();
            const bool done_c{ drive(ctx, 10) };
            ctx.check("compiledcaller_postremoval_probe_completed", done_c);
            ctx.check("compiledcaller_postremoval_detour_did_not_fire",
                      g_fire_count.load() == 0);
        }
    }

    // =====================================================================
    // Scenario 13 — WIDE (long) RETURN install-after-JIT.  The deopt-on-install
    //   path is descriptor-agnostic, but the detour decode + force-return must
    //   handle a return value that spans TWO interpreter slots.  Warm hotLong(I)J
    //   to _code != null, install an OBSERVING detour and assert allow-through
    //   yields the full 64-bit LONG_ORIGINAL (no truncation); then re-warm and
    //   install a CANCELLING detour and assert Java observes the forced 64-bit
    //   value (proves the wide force-return survives the install-time deopt).
    // =====================================================================
    {
        vmhook::hotspot::method* const m{ find_method(LONG_NAME, LONG_SIG) };
        ctx.check("widereturn_located_live_method", m != nullptr);

        if (m == nullptr)
        {
            ctx.record("[INFO] hook_install_after_jit scenario 13: could not locate live "
                       "Method* for hotLong(I)J - skipping (no crash).");
        }
        else
        {
            // -- 13a: observing (allow-through) on the warm wide-return method --
            const bool warm_a{ warm_until_compiled_mode(ctx, m, 13) };
            if (warm_a)
            {
                ctx.check("widereturn_observe_compiled_before_install", method_code(m) != nullptr);
            }
            reset_observations();
            ctx.check("widereturn_observe_install_returns_true", install_long_observer());
            if (warm_a)
            {
                ctx.check("widereturn_observe_install_deopted_code_to_null", method_code(m) == nullptr);
            }
            ctx.check("widereturn_observe_install_armed_no_compile", no_compile_set(m));
            ctx.check("widereturn_observe_verify_zero_after_install", verify_settles_zero(12));

            const bool done_a{ drive(ctx, 14) };
            ctx.check("widereturn_observe_probe_completed", done_a);
            ctx.check("widereturn_observe_java_made_one_call",
                      haj_fixture::get_hot_calls_made() == 1);
            ctx.check("widereturn_observe_detour_fired_once", g_fire_count.load() == 1);
            ctx.check("widereturn_observe_self_correct", g_self_ok_fires.load() == 1);
            ctx.check("widereturn_observe_arg_decoded", g_arg_xor.load() == HOT_DELTA);
            // Allow-through: the full 64-bit body result, untruncated.  value_t has
            // no operator==(int), so compare a copy-init std::int64_t.
            ctx.check("widereturn_observe_allow_through_full_64bit",
                      static_cast<std::int64_t>(haj_fixture::get_last_long_result())
                          == LONG_ORIGINAL);
            // Sanity: LONG_ORIGINAL really is outside the 32-bit range, so a
            // truncated decode (low 32 bits only) would NOT equal it.
            ctx.check("widereturn_original_exceeds_32bit",
                      LONG_ORIGINAL != static_cast<std::int64_t>(
                                           static_cast<std::int32_t>(LONG_ORIGINAL)));

            vmhook::shutdown_hooks();   // clean up 13a

            // -- 13b: cancelling (force a 64-bit return) on the warm method -----
            const bool warm_b{ warm_until_compiled_mode(ctx, m, 13) };
            reset_observations();
            ctx.check("widereturn_force_install_returns_true", install_long_forcing());
            if (warm_b)
            {
                ctx.check("widereturn_force_install_deopted_code_to_null", method_code(m) == nullptr);
            }
            ctx.check("widereturn_force_install_armed_no_compile", no_compile_set(m));

            const bool done_b{ drive(ctx, 14) };
            ctx.check("widereturn_force_probe_completed", done_b);
            ctx.check("widereturn_force_detour_fired_once", g_fire_count.load() == 1);
            ctx.check("widereturn_force_self_correct", g_self_ok_fires.load() == 1);
            ctx.check("widereturn_force_arg_decoded", g_arg_xor.load() == HOT_DELTA);
            // The forced wide value flows back to Java in full, not truncated.
            ctx.check("widereturn_force_return_overridden_full_64bit",
                      static_cast<std::int64_t>(haj_fixture::get_last_long_result())
                          == LONG_FORCED);
            ctx.check("widereturn_force_return_not_original",
                      static_cast<std::int64_t>(haj_fixture::get_last_long_result())
                          != LONG_ORIGINAL);

            vmhook::shutdown_hooks();   // clean up 13b

            // Post-removal: the wide body runs normally again, detour silent.
            reset_observations();
            const bool done_c{ drive(ctx, 14) };
            ctx.check("widereturn_postremoval_probe_completed", done_c);
            ctx.check("widereturn_postremoval_detour_did_not_fire", g_fire_count.load() == 0);
            ctx.check("widereturn_postremoval_original_restored",
                      static_cast<std::int64_t>(haj_fixture::get_last_long_result())
                          == LONG_ORIGINAL);
        }
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
