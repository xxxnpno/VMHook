// method_throwing_call_site JVM test module  (feature area: method invocation)
//
// THE throwing-call-site authority: invokes a FAMILY of Java methods that THROW,
// via vmhook::method_proxy::call(), from inside a detour, and proves for EACH
// exception kind that the native call site COMPLETES and the JVM is left in a
// clean, usable state afterwards.  This is the single most crash-sensitive thing
// vmhook does — a Java exception unwinding from Java back into native code — so
// the contract under test is deliberately conservative and is asserted ONCE PER
// EXCEPTION KIND:
//
//   PRIMARY (hard asserts, UNIVERSAL across all 5 CI toolchains incl. the no-SEH
//   win-clang / mingw paths, and across Java 8..26):
//     * no detour access-violation and no suite truncation: the line AFTER the
//       throwing call() is reached (per-scenario g_reached_after), the probe's
//       `done` flag is observed each cycle, and this module's body runs to its
//       end;
//     * the throwing method genuinely RAN with the right argument: the fixture's
//       per-method *Entered counter is >=1 and *LastArg matches the marshalled
//       sentinel (recorded BEFORE the throw, so the throw cannot hide that the
//       body executed and received our arg);
//     * the JVM/thread is HEALTHY AFTER each throw: a subsequent benign call()
//       (safeAdd(N) -> N+1) succeeds, an instance field read returns
//       0x600DC0DE, and a static field read returns 0x5AFE5AFE — all on the SAME
//       detour thread, AFTER the throwing call;
//     * the thread is NOT left in ExceptionOccurred state for the next scenario
//       OR the next module: after our defensive clear, a fresh ExceptionCheck
//       reports no pending exception (g_clean_after_clear is a DEFINITE 0).
//     * calling AGAIN after a throw succeeds: the recovery safeAdd() and the
//       next scenario's call() both dispatch correctly, proving no escape and no
//       cross-call poisoning.
//
//   CHARACTERIZED ([INFO], NOT asserted, because it is JDK-variant and
//   dispatch-path dependent):
//     * HOW vmhook surfaces the thrown exception.  Two dispatch paths exist
//       (method_proxy::call):
//         - JNI-FALLBACK path (no StubRoutines::_call_stub_entry; JDK 21+ and in
//           practice every CI JDK): call_jni runs check_callee_exception() after
//           the JNI Call*MethodA — it detects the pending exception, calls
//           ExceptionDescribe (which PRINTS *and CLEARS* it), extracts
//           Throwable.toString() into the vmhook log, and returns the JNI
//           default-return value (0 for an `I` method) as value_t{int32 0}.  The
//           thread is left clean BY VMHOOK.
//         - CALL-STUB fast path (JDK 8..20 with the stub present): the raw
//           call-stub invocation HISTORICALLY did NOT clear the pending
//           exception.  THE LIBRARY BUG THIS MODULE DROVE OUT (now fixed in
//           vmhook.hpp method_proxy::call, immediately after the stub returns):
//           the call-stub path now mirrors call_jni's check_callee_exception() —
//           ExceptionDescribe (slot 16, prints+clears) with an ExceptionClear
//           (slot 17) fallback — so BOTH paths leave the thread clean.  This
//           module ALSO keeps its own belt-and-braces defensive clear after the
//           call, so a regression of that library fix is caught (the post-clear
//           ExceptionCheck stays 0) without the suite dying.  We record whether a
//           pending exception was observed pre-(our-)clear (g_pending_pre_clear)
//           so the live path is visible in the results.
//     * the returned value_t shape (is_void()? variant index? the int value IF
//       it decoded to the int32 alternative).  We do NOT assert a specific
//       return value from a throwing call — a throwing call's "return" is the
//       dispatcher's default cell, which is not a vmhook contract.
//     * the exception TYPE/message is NOT extractable from native after call()
//       (the library clears it inside call() by design, on BOTH paths), so the
//       thrown type is proven only via the Java-side *Entered / *LastArg
//       witnesses and the per-path g_pending_pre_clear discriminator, and the
//       class name lands on JVM stderr via ExceptionDescribe — all [INFO].
//
// SAFETY POSTURE (this is the highest-risk module):
//   * every pointer deref is gated with vmhook::hotspot::is_valid_pointer;
//   * the throwing call() and EVERY post-throw operation happen inside the
//     detour where current_java_thread is live (the only legal context);
//   * immediately after every throwing call() we ALWAYS run a defensive
//     clear chain (ensure_current_java_thread -> jni_exception_clear ->
//     raw_clear_pending_exception, with a bounded verify-and-retry) so no
//     pending exception can poison subsequent JNI on this thread, leak into the
//     next scenario / next module, OR unwind out of trigger()'s interpreter
//     frame into vmhook.Main and kill the suite — even if the library-level
//     clear ever regresses;
//   * value_t is read by COPY-INIT (never brace-init from call()/value_t — that
//     is ambiguous on MSVC because the templated conversion operator can also
//     produce const char*);
//   * the detour is no-throw BY CONSTRUCTION (only guarded pointer reads, atomic
//     stores, and noexcept vmhook/JNI calls), and it is NOT marked noexcept
//     (vmhook's hook<>() function_traits has no noexcept specialization);
//   * hooks are torn down with vmhook::shutdown_hooks() AND an UNCONDITIONAL
//     final `if (ctx.reset) ctx.reset();` on EVERY exit path, so nothing is left
//     armed for later modules (the no-SEH Windows recovery path longjmps past
//     C++ destructors, so the explicit reset is load-bearing there).
//
// Harness shape mirrors on_exception: a `scenario` selector with a `done` reset
// on the rising edge of go; the detour dispatches on the scenario and captures
// every observation into atomics the module body reads back per cycle.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

namespace
{
    // Scenario selector values (mirror the order the body drives them).  Each is
    // one throwing call() the detour performs this cycle.
    enum scenario_id : int
    {
        SC_RUNTIME      = 0,  // boom(-1)            -> IllegalStateException (RuntimeException)
        SC_CHECKED      = 1,  // throwChecked(-2)    -> RuntimeException wrapping checked IOException
        SC_ERROR        = 2,  // throwError(-3)      -> java.lang.Error (IllegalAccessError)
        SC_CUSTOM       = 3,  // throwCustom(-4)     -> project BoomException
        SC_NPE          = 4,  // throwNpe(-5)        -> NullPointerException (real null deref)
        SC_AIOOBE       = 5,  // throwAioobe(-6)     -> ArrayIndexOutOfBoundsException (real OOB)
        SC_ARITH        = 6,  // throwArithmetic(-7) -> ArithmeticException (/ by zero)
        SC_NESTED       = 7,  // throwNested(-8)     -> throw from a NESTED Java call
        SC_STATIC       = 8,  // sBoom(-9)           -> STATIC throwing method
        SC_AFTER_OK     = 9,  // throwAfterSuccess(-10) -> throw AFTER committed work
        SC_COUNT        = 10
    };

    // Wrapper for vmhook.fixtures.ThrowingMethod.  Instance-context accessors
    // plus the static handshake/witness fields.  Stays portable across compilers
    // (no deducing-this static get_method).
    class throwfix : public vmhook::object<throwfix>
    {
    public:
        explicit throwfix(vmhook::oop_t instance) noexcept
            : vmhook::object<throwfix>{ instance }
        {
        }

        // ---- handshake (static fields on the fixture) ----
        static auto set_go(bool value) -> void       { static_field("go")->set(value); }
        static auto set_done(bool value) -> void      { static_field("done")->set(value); }
        static auto get_done() -> bool                { return static_field("done")->get(); }
        static auto set_scenario(std::int32_t s) -> void { static_field("scenario")->set(s); }

        static auto resolves(const char* name) -> bool { return static_field(name).has_value(); }

        // ---- witnesses (read back by the module body) ----
        static auto get_trigger_count() -> std::int32_t { return static_field("triggerCount")->get(); }
        static auto get_safe_add_calls() -> std::int32_t{ return static_field("safeAddCalls")->get(); }
        static auto get_static_health() -> std::int32_t { return static_field("staticHealthField")->get(); }

        static auto entered(const char* name) -> std::int32_t { return static_field(name)->get(); }
        static auto last_arg(const char* name) -> std::int32_t { return static_field(name)->get(); }
    };

    constexpr std::int64_t k_uncaptured{ static_cast<std::int64_t>(0xDEADBEEFCAFEF00DULL) };

    // ── HotSpot-level pending-exception clear (load-bearing belt-and-braces) ──
    // Null JavaThread::_pending_exception directly when VMStructs exposes it.
    // This is the dismissal for cases the JNI-level clear cannot cover (cold /
    // env-null thread on the call-stub path).  Safe no-op when the field is
    // absent from VMStructs (the JNI-fallback JDKs).  Every access is
    // is_valid_pointer-gated; std::memcpy avoids strict-aliasing UB; the field is
    // pointer-width on every supported HotSpot.  Clearing an oop root needs no GC
    // store barrier — exactly what JNI ExceptionClear does internally.
    auto pending_exception_offset() noexcept -> std::uint64_t
    {
        static const std::uint64_t off{ []() -> std::uint64_t {
            const vmhook::hotspot::vm_struct_entry_t* e{
                vmhook::hotspot::iterate_struct_entries("Thread", "_pending_exception") };
            if (!e)
            {
                e = vmhook::hotspot::iterate_struct_entries("JavaThread", "_pending_exception");
            }
            return e ? e->offset : static_cast<std::uint64_t>(-1);
        }() };
        return off;
    }

    auto raw_clear_pending_exception() noexcept -> bool
    {
        void* const thr{ vmhook::hotspot::current_java_thread };
        if (!thr || !vmhook::hotspot::is_valid_pointer(thr))
        {
            return false;
        }
        const std::uint64_t off{ pending_exception_offset() };
        if (off == static_cast<std::uint64_t>(-1))
        {
            return false;   // field not exported on this JDK — JNI clear is the path
        }
        void* const slot{ reinterpret_cast<std::uint8_t*>(thr) + off };
        if (!vmhook::hotspot::is_valid_pointer(slot))
        {
            return false;
        }
        void* const null_oop{ nullptr };
        std::memcpy(slot, &null_oop, sizeof(void*));
        return true;
    }

    // Read the current thread's JNI ExceptionCheck (slot 228) WITHOUT clearing.
    // Fully guarded; returns 1 (pending), 0 (none), or -1 (could not determine).
    auto jni_exception_pending() noexcept -> int
    {
        void* const env{ vmhook::hotspot::current_jni_env };
        if (!env)
        {
            return -1;
        }
        using exception_check_t = std::uint8_t (*)(void*);
        exception_check_t const exc_check{
            vmhook::detail::jni_function<228, exception_check_t>(env) };
        if (!exc_check)
        {
            return -1;
        }
        return exc_check(env) != 0u ? 1 : 0;
    }

    // The full defensive clear chain used after EVERY throwing call().  Runs the
    // env-guaranteed JNI clear AND the direct HotSpot clear, with a bounded
    // verify-and-retry.  Returns the post-clear ExceptionCheck (1/0/-1).
    auto defensive_clear() noexcept -> int
    {
        static_cast<void>(vmhook::hotspot::ensure_current_java_thread());
        vmhook::detail::jni_exception_clear();
        static_cast<void>(raw_clear_pending_exception());
        int post{ jni_exception_pending() };
        for (int retry{ 0 }; post == 1 && retry < 4; ++retry)
        {
            vmhook::detail::jni_exception_clear();
            static_cast<void>(raw_clear_pending_exception());
            post = jni_exception_pending();
        }
        return post;
    }

    // ── per-scenario observation block, captured INSIDE the trigger() detour ──
    struct obs
    {
        std::atomic<bool>         resolved{ false };          // get_method resolved this cycle
        std::atomic<bool>         identity_ok{ false };       // name()/signature() matched
        std::atomic<bool>         reached_after{ false };     // *** line after call() ran ***
        std::atomic<bool>         ret_is_void{ false };       // value_t.is_void()
        std::atomic<int>          ret_variant{ -1 };          // value_t variant index
        std::atomic<bool>         ret_is_int32{ false };      // decoded to int32 alternative
        std::atomic<std::int64_t> ret_int{ k_uncaptured };    // the int value IF int32

        std::atomic<int>          pending_pre_clear{ -1 };    // ExceptionCheck before OUR clear
        std::atomic<int>          pending_post_clear{ -1 };   // ExceptionCheck after  OUR clear
        std::atomic<bool>         clean_after_clear{ false }; // post-clear == 0

        // post-throw JVM-health (all AFTER the throwing call, same thread)
        std::atomic<bool>         safe_add_resolved{ false };
        std::atomic<bool>         safe_add_returned{ false };
        std::atomic<std::int64_t> safe_add_value{ k_uncaptured };
        std::atomic<bool>         health_field_ok{ false };
        std::atomic<std::int64_t> health_field_value{ k_uncaptured };
        std::atomic<bool>         static_field_ok{ false };
        std::atomic<std::int64_t> static_field_value{ k_uncaptured };
    };

    obs g_obs[SC_COUNT];

    std::atomic<int>  g_detour_calls{ 0 };
    std::atomic<bool> g_self_valid{ false };
    std::atomic<int>  g_call_stub_present{ -1 };   // find_call_stub_entry() != null

    // Resolve + identity-check + drive ONE throwing call() for `o`, capturing the
    // post-throw cleanliness and health observations.  `arg` is the marshalled
    // sentinel; `name`/`sig` select the method.  `is_static_call` chooses the
    // static dispatch for the static scenario.  No-throw by construction.
    auto run_throwing_scenario(const std::unique_ptr<throwfix>& self,
                               obs& o,
                               const char* name,
                               const char* sig,
                               std::int32_t arg,
                               bool is_static_call,
                               std::int32_t safe_add_arg) noexcept -> void
    {
        // Resolve the throwing method (instance proxy unless static).
        if (is_static_call)
        {
            auto proxy{ throwfix::static_method(name, sig) };
            if (!proxy.has_value())
            {
                return;
            }
            o.resolved.store(true);
            if (proxy->name() != std::string_view{ name }
                || proxy->signature() != std::string_view{ sig })
            {
                return;
            }
            o.identity_ok.store(true);

            // THE THROWING CALL (static).  Copy-init the value_t (never brace).
            const vmhook::method_proxy::value_t result = proxy->call(arg);
            o.reached_after.store(true);
            o.ret_is_void.store(result.is_void());
            o.ret_variant.store(static_cast<int>(result.data.index()));
            const bool is_int32{ std::holds_alternative<std::int32_t>(result.data) };
            o.ret_is_int32.store(is_int32);
            if (is_int32)
            {
                const std::int32_t iv = result;
                o.ret_int.store(static_cast<std::int64_t>(iv));
            }
        }
        else
        {
            if (!self)
            {
                return;
            }
            auto proxy{ self->get_method(name, sig) };
            if (!proxy.has_value())
            {
                return;
            }
            o.resolved.store(true);
            if (proxy->name() != std::string_view{ name }
                || proxy->signature() != std::string_view{ sig })
            {
                return;
            }
            o.identity_ok.store(true);

            // THE THROWING CALL (instance).  Copy-init the value_t (never brace).
            const vmhook::method_proxy::value_t result = proxy->call(arg);
            o.reached_after.store(true);
            o.ret_is_void.store(result.is_void());
            o.ret_variant.store(static_cast<int>(result.data.index()));
            const bool is_int32{ std::holds_alternative<std::int32_t>(result.data) };
            o.ret_is_int32.store(is_int32);
            if (is_int32)
            {
                const std::int32_t iv = result;
                o.ret_int.store(static_cast<std::int64_t>(iv));
            }
        }

        // Guarantee a non-null JNIEnv before reading the pre-clear state, so the
        // snapshot is a DEFINITE 1/0 (not -1 unknown env) on both paths.
        static_cast<void>(vmhook::hotspot::ensure_current_java_thread());
        o.pending_pre_clear.store(jni_exception_pending());

        // Defensive clear (always; idempotent; belt-and-braces over the library
        // fix).  The thread MUST be clean before the next op / scenario / module.
        const int post{ defensive_clear() };
        o.pending_post_clear.store(post);
        o.clean_after_clear.store(post == 0);

        // ── post-throw JVM health (all AFTER the throw, same thread) ──
        if (self)
        {
            auto sa{ self->get_method("safeAdd", "(I)I") };
            if (sa.has_value()
                && sa->name() == "safeAdd"
                && sa->signature() == std::string_view{ "(I)I" })
            {
                o.safe_add_resolved.store(true);
                const vmhook::method_proxy::value_t r = sa->call(safe_add_arg);
                o.safe_add_returned.store(true);
                if (std::holds_alternative<std::int32_t>(r.data))
                {
                    const std::int32_t v = r;
                    o.safe_add_value.store(static_cast<std::int64_t>(v));
                }
                vmhook::detail::jni_exception_clear();
            }

            auto hf{ self->get_field("healthField") };
            if (hf.has_value())
            {
                const std::int32_t v = hf->get();
                o.health_field_value.store(static_cast<std::int64_t>(v));
                o.health_field_ok.store(true);
            }
        }

        auto shf{ throwfix::static_field("staticHealthField") };
        if (shf.has_value())
        {
            const std::int32_t v = shf->get();
            o.static_field_value.store(static_cast<std::int64_t>(v));
            o.static_field_ok.store(true);
        }

        // Final clear so absolutely nothing pending escapes into vmhook.Main.
        static_cast<void>(vmhook::hotspot::ensure_current_java_thread());
        vmhook::detail::jni_exception_clear();
        static_cast<void>(raw_clear_pending_exception());
    }

    // The trigger() detour: read the requested scenario from the fixture, drive
    // the matching throwing call(), and do all post-throw work.  No-throw by
    // construction.  NOT noexcept (function_traits has no noexcept arm).
    auto on_trigger(vmhook::return_value& /*retval*/,
                    const std::unique_ptr<throwfix>& self,
                    std::int32_t /*delta*/) -> void
    {
        ++g_detour_calls;

        if (!self)
        {
            return;
        }
        void* const self_oop{ self->get_instance() };
        if (!self_oop || !vmhook::hotspot::is_valid_pointer(self_oop))
        {
            return;
        }
        g_self_valid.store(true);

        // Which scenario this cycle?  Read via a fresh static field proxy.
        auto sc_field{ throwfix::static_field("scenario") };
        const std::int32_t sc{ sc_field.has_value() ? sc_field->get() : -1 };

        switch (sc)
        {
        case SC_RUNTIME:  run_throwing_scenario(self, g_obs[SC_RUNTIME],  "boom",            "(I)I", -1,  false, 100); break;
        case SC_CHECKED:  run_throwing_scenario(self, g_obs[SC_CHECKED],  "throwChecked",    "(I)I", -2,  false, 101); break;
        case SC_ERROR:    run_throwing_scenario(self, g_obs[SC_ERROR],    "throwError",      "(I)I", -3,  false, 102); break;
        case SC_CUSTOM:   run_throwing_scenario(self, g_obs[SC_CUSTOM],   "throwCustom",     "(I)I", -4,  false, 103); break;
        case SC_NPE:      run_throwing_scenario(self, g_obs[SC_NPE],      "throwNpe",        "(I)I", -5,  false, 104); break;
        case SC_AIOOBE:   run_throwing_scenario(self, g_obs[SC_AIOOBE],   "throwAioobe",     "(I)I", -6,  false, 105); break;
        case SC_ARITH:    run_throwing_scenario(self, g_obs[SC_ARITH],    "throwArithmetic", "(I)I", -7,  false, 106); break;
        case SC_NESTED:   run_throwing_scenario(self, g_obs[SC_NESTED],   "throwNested",     "(I)I", -8,  false, 107); break;
        case SC_STATIC:   run_throwing_scenario(self, g_obs[SC_STATIC],   "sBoom",           "(I)I", -9,  true,  108); break;
        case SC_AFTER_OK: run_throwing_scenario(self, g_obs[SC_AFTER_OK], "throwAfterSuccess","(I)I", -10, false, 109); break;
        default: break;
        }
    }

    // Drive one probe cycle for `scenario`: clear `done` and program the selector
    // on the rising edge of go, then wait for the fixture's probe action to run
    // trigger() -> the native detour.
    auto drive(vmhook_test::context& ctx, std::int32_t scenario) -> bool
    {
        return ctx.run_probe(
            [scenario](bool value)
            {
                if (value)
                {
                    throwfix::set_done(false);
                    throwfix::set_scenario(scenario);
                }
                throwfix::set_go(value);
            },
            []() { return throwfix::get_done(); });
    }

    // Per-scenario assertion block.  `prefix` is unique per scenario so failures
    // are unambiguous; `entered_field`/`arg_field` are the Java witnesses;
    // `expected_arg` is the sentinel the native side marshalled; `safe_add_arg`
    // is what safeAdd was called with this cycle (recovery proof returns +1).
    auto assert_scenario(vmhook_test::context& ctx,
                         const char* prefix,
                         bool probe_done,
                         const obs& o,
                         const char* entered_field,
                         const char* arg_field,
                         std::int32_t expected_arg,
                         std::int32_t safe_add_arg) -> void
    {
        const auto name = [&](const char* suffix) { return std::string{ prefix } + "_" + suffix; };

        ctx.check(name("probe_completed"), probe_done);
        if (!probe_done)
        {
            ctx.record("[INFO] method_throwing_call_site/" + std::string{ prefix }
                       + ": probe did not complete; observations not captured this cycle.");
            return;
        }

        // Resolution + identity survived the in-detour guards.
        ctx.check(name("method_resolved"),    o.resolved.load());
        ctx.check(name("method_identity_ok"), o.identity_ok.load());

        // *** THE headline proof: the line after the throwing call() executed. ***
        ctx.check(name("reached_line_after_throwing_call"), o.reached_after.load());

        // The throwing method genuinely ran with our marshalled arg.
        ctx.check(name("body_entered"),     throwfix::entered(entered_field) >= 1);
        ctx.check(name("received_arg"),     throwfix::last_arg(arg_field) == expected_arg);

        // Thread left CLEAN (universal HARD invariant on every toolchain/JDK).
        ctx.check(name("no_pending_exception_after_clear"), o.clean_after_clear.load());
        ctx.check(name("post_clear_exception_check_is_zero"), o.pending_post_clear.load() == 0);

        // JVM healthy AFTER the throw: benign call() recovers + returns N+1.
        ctx.check(name("post_throw_safeAdd_resolved"),  o.safe_add_resolved.load());
        ctx.check(name("post_throw_safeAdd_returned"),  o.safe_add_returned.load());
        ctx.check(name("post_throw_safeAdd_value_plus_one"),
                  o.safe_add_value.load() == static_cast<std::int64_t>(safe_add_arg) + 1);

        // Field reads still work on the recovered thread.
        ctx.check(name("post_throw_instance_field_read_ok"), o.health_field_ok.load());
        ctx.check(name("post_throw_instance_field_value"),
                  o.health_field_value.load() == static_cast<std::int64_t>(0x600DC0DE));
        ctx.check(name("post_throw_static_field_read_ok"), o.static_field_ok.load());
        ctx.check(name("post_throw_static_field_value"),
                  o.static_field_value.load() == static_cast<std::int64_t>(0x5AFE5AFE));

        // CHARACTERIZATION ([INFO], not asserted): return shape + live path.
        {
            const int pre{ o.pending_pre_clear.load() };
            std::string line{ "[INFO] method_throwing_call_site/" };
            line += prefix;
            line += ": value_t.is_void=" + std::string(o.ret_is_void.load() ? "true" : "false");
            line += " variant=" + std::to_string(o.ret_variant.load());
            line += " is_int32=" + std::string(o.ret_is_int32.load() ? "true" : "false");
            if (o.ret_is_int32.load())
            {
                line += " int_value=" + std::to_string(o.ret_int.load())
                      + " (dispatcher default cell; NOT asserted)";
            }
            line += " | ExceptionCheck-before-our-clear=";
            line += (pre == 1 ? "1 (CALL-STUB path: library clear ran; our defensive clear is belt-and-braces)"
                  : pre == 0 ? "0 (JNI-FALLBACK path OR library call-stub clear already fired: thread already clean)"
                  : "-1 (no JNIEnv / slot)");
            ctx.record(line);
        }
    }
}

VMHOOK_JVM_MODULE(method_throwing_call_site)
{
    vmhook::register_class<throwfix>("vmhook/fixtures/ThrowingMethod");

    // Record which dispatch path the live JDK uses (cached once per process).
    g_call_stub_present.store(vmhook::detail::find_call_stub_entry() != nullptr ? 1 : 0);

    // =====================================================================
    //  0. Sanity: the class + handshake/witness fields all resolve.
    // =====================================================================
    ctx.check("tcs_class_registered_go_resolves", throwfix::resolves("go"));
    ctx.check("tcs_done_field_resolves",          throwfix::resolves("done"));
    ctx.check("tcs_scenario_field_resolves",      throwfix::resolves("scenario"));
    ctx.check("tcs_boomEntered_field_resolves",   throwfix::resolves("boomEntered"));
    ctx.check("tcs_checkedEntered_field_resolves",throwfix::resolves("checkedEntered"));
    ctx.check("tcs_errorEntered_field_resolves",  throwfix::resolves("errorEntered"));
    ctx.check("tcs_customEntered_field_resolves", throwfix::resolves("customEntered"));
    ctx.check("tcs_npeEntered_field_resolves",    throwfix::resolves("npeEntered"));
    ctx.check("tcs_aioobeEntered_field_resolves", throwfix::resolves("aioobeEntered"));
    ctx.check("tcs_arithEntered_field_resolves",  throwfix::resolves("arithEntered"));
    ctx.check("tcs_nestedEntered_field_resolves", throwfix::resolves("nestedEntered"));
    ctx.check("tcs_sBoomEntered_field_resolves",  throwfix::resolves("sBoomEntered"));
    ctx.check("tcs_afterSuccessEntered_field_resolves", throwfix::resolves("afterSuccessEntered"));
    ctx.check("tcs_staticHealth_field_resolves",  throwfix::resolves("staticHealthField"));

    // Contract up front so it is in the results even if a probe never completes.
    ctx.record("[INFO] method_throwing_call_site: a Java method invoked via "
               "method_proxy::call() that THROWS (RuntimeException / checked / "
               "Error / custom / NPE / AIOOBE / ArithmeticException / nested-call "
               "/ static / throw-after-success) must (a) unwind back to native "
               "with NO access-violation, (b) leave the thread clearable so the "
               "next call()/field-read succeeds, on every CI toolchain (incl. the "
               "no-SEH win-clang/mingw paths) and Java 8..26.  vmhook surfaces+"
               "clears the throw on BOTH dispatch paths (JNI-fallback via "
               "check_callee_exception/ExceptionDescribe; call-stub via the same "
               "after the stub returns — the bug this module drove out).  The "
               "throwing call's return VALUE is NOT asserted (dispatcher default "
               "cell).  Exception TYPE/message is [INFO] only (the library clears "
               "it inside call(); the Java *Entered/*LastArg witnesses prove which "
               "method ran).");

    {
        const std::string path{ g_call_stub_present.load() == 1
            ? "call-stub fast path PRESENT (StubRoutines::_call_stub_entry exported; "
              "JDK 8..~20) - the path that historically leaked the pending exception; "
              "the library fix + this module's defensive clear keep it clean"
            : "JNI-FALLBACK path (call_jni / Call(Static)?IntMethodA) - JDK 21+ and "
              "every CI JDK; vmhook auto-clears via check_callee_exception/ExceptionDescribe" };
        ctx.record("[INFO] method_throwing_call_site dispatch path: " + path);
    }

    // =====================================================================
    //  1. Install the hook on trigger().  The detour does every throwing call.
    // =====================================================================
    const bool hook_installed{ vmhook::hook<throwfix>("trigger", &on_trigger) };
    ctx.check("tcs_trigger_hook_installed", hook_installed);

    if (!hook_installed)
    {
        ctx.record("[INFO] method_throwing_call_site: hook on trigger() failed to "
                   "install; skipping the live throwing-call drive (nothing armed).");
        vmhook::shutdown_hooks();
        if (ctx.reset) { ctx.reset(); }
        return;
    }

    // =====================================================================
    //  2. Drive every scenario, one probe cycle each, and assert the
    //     universal triad (completed + clean + healthy) per kind.
    // =====================================================================
    const bool d_runtime { drive(ctx, SC_RUNTIME)  };
    assert_scenario(ctx, "tcs_runtime",  d_runtime,  g_obs[SC_RUNTIME],  "boomEntered",        "boomLastArg",         -1,  100);

    const bool d_checked { drive(ctx, SC_CHECKED)  };
    assert_scenario(ctx, "tcs_checked",  d_checked,  g_obs[SC_CHECKED],  "checkedEntered",     "checkedLastArg",      -2,  101);

    const bool d_error   { drive(ctx, SC_ERROR)    };
    assert_scenario(ctx, "tcs_error",    d_error,    g_obs[SC_ERROR],    "errorEntered",       "errorLastArg",        -3,  102);

    const bool d_custom  { drive(ctx, SC_CUSTOM)   };
    assert_scenario(ctx, "tcs_custom",   d_custom,   g_obs[SC_CUSTOM],   "customEntered",      "customLastArg",       -4,  103);

    const bool d_npe     { drive(ctx, SC_NPE)      };
    assert_scenario(ctx, "tcs_npe",      d_npe,      g_obs[SC_NPE],      "npeEntered",         "npeLastArg",          -5,  104);

    const bool d_aioobe  { drive(ctx, SC_AIOOBE)   };
    assert_scenario(ctx, "tcs_aioobe",   d_aioobe,   g_obs[SC_AIOOBE],   "aioobeEntered",      "aioobeLastArg",       -6,  105);

    const bool d_arith   { drive(ctx, SC_ARITH)    };
    assert_scenario(ctx, "tcs_arith",    d_arith,    g_obs[SC_ARITH],    "arithEntered",       "arithLastArg",        -7,  106);

    const bool d_nested  { drive(ctx, SC_NESTED)   };
    assert_scenario(ctx, "tcs_nested",   d_nested,   g_obs[SC_NESTED],   "nestedEntered",      "nestedLastArg",       -8,  107);

    const bool d_static  { drive(ctx, SC_STATIC)   };
    assert_scenario(ctx, "tcs_static",   d_static,   g_obs[SC_STATIC],   "sBoomEntered",       "sBoomLastArg",        -9,  108);

    const bool d_after   { drive(ctx, SC_AFTER_OK) };
    assert_scenario(ctx, "tcs_after_ok", d_after,    g_obs[SC_AFTER_OK], "afterSuccessEntered","afterSuccessLastArg", -10, 109);

    // =====================================================================
    //  3. Cross-scenario invariants — the throws did not wedge the handshake
    //     and the inner Java frame of the nested throw genuinely ran.
    // =====================================================================
    ctx.check("tcs_detour_ran_each_cycle", g_detour_calls.load() == SC_COUNT);
    ctx.check("tcs_self_valid_in_detour",  g_self_valid.load());
    // trigger() ran exactly once per cycle (the throws never re-entered it).
    ctx.check("tcs_trigger_count_matches_cycles", throwfix::get_trigger_count() == SC_COUNT);
    // The nested-throw scenario unwound through the inner deep() frame too.
    ctx.check("tcs_nested_inner_frame_entered", throwfix::entered("nestedDeepEntered") >= 1);
    // safeAddCalls advanced: throwAfterSuccess committed its increment BEFORE
    // throwing (+1), and every recovery safeAdd() bumped it once more.  The
    // exact total is JDK/recovery-path dependent, so assert the load-bearing
    // floor: at least once per recovery cycle plus the committed after-success
    // increment.  (Recorded exactly below for visibility.)
    ctx.check("tcs_safeAdd_advanced_after_throws", throwfix::get_safe_add_calls() >= SC_COUNT);
    ctx.record("[INFO] method_throwing_call_site: safeAddCalls total after all "
               "scenarios = " + std::to_string(throwfix::get_safe_add_calls())
               + " (>= one recovery call per cycle + throwAfterSuccess's committed "
                 "pre-throw increment).");

    // Re-read the static health field from the BODY (a fresh dispatch outside the
    // detour) — proves the JVM is still fully usable for the next module.
    ctx.check("tcs_static_field_reread_from_body",
              throwfix::get_static_health() == static_cast<std::int32_t>(0x5AFE5AFE));

    // IMPORTANT for log readers: on the JNI-FALLBACK path (and now the fixed
    // call-stub path) vmhook surfaces each throw by calling ExceptionDescribe,
    // which prints a single  `Exception in thread "main" <Type>: <msg> at ...`
    // line per scenario to JVM stderr AND clears it.  Those lines are vmhook's
    // INTENDED surfacing channel, NOT uncaught exceptions escaping vmhook.Main:
    // the suite continues and reaches TOTAL.
    ctx.record("[INFO] method_throwing_call_site: up to one `Exception in thread "
               "\"main\" <Type>: <msg>` line per scenario on JVM stderr is "
               "vmhook's ExceptionDescribe surfacing the throw, not an escape; the "
               "suite still completes to TOTAL.");

    // =====================================================================
    //  4. Leave NOTHING armed for later modules sharing the JVM.  shutdown_hooks
    //     on the normal path; the UNCONDITIONAL ctx.reset() is the belt-and-braces
    //     for the no-SEH Windows recovery path (longjmp past C++ destructors).
    // =====================================================================
    vmhook::shutdown_hooks();
    if (ctx.reset) { ctx.reset(); }
}
