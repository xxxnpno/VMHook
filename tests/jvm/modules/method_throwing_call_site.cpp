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
#include <string_view>
#include <variant>

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
        // ── return-descriptor variety (the unwound-path ret_char decode) ──
        SC_RET_VOID     = 10, // throwVoid(int)      -> (I)V  'V' decode on throw
        SC_RET_LONG     = 11, // throwLong(int)      -> (I)J  'J' 64-bit default cell
        SC_RET_DOUBLE   = 12, // throwDouble(int)    -> (I)D  'D' default cell
        SC_RET_BOOL     = 13, // throwBool(int)      -> (I)Z  'Z' decode
        SC_RET_STRING   = 14, // throwString(int)    -> (I)Lj/l/String; reference decode
        // ── argument-shape variety (the pack() branch under a throw) ──
        SC_ARG_NONE     = 15, // throwNoArg()        -> ()I   no-extra-arg pack
        SC_ARG_LONG     = 16, // throwLongArg(long)  -> (J)I  wide-long pack
        SC_ARG_DOUBLE   = 17, // throwDoubleArg(dbl) -> (D)I  wide-double pack
        SC_ARG_STRING   = 18, // throwStringArg(Str) -> (Lj/l/String;)I  ref pack
        SC_ARG_TWO      = 19, // throwTwoArgs(int,long) -> (IJ)I  multi-slot pack
        // ── extra unwind shapes ──
        SC_DEEP3        = 20, // throwDeep3(int)     -> 3-frame unwind
        SC_FINALLY      = 21, // throwInFinally(int) -> throw through a finally
        // ── CONTAINED throws: caught/swallowed in Java, NOTHING escapes ──
        SC_THEN_CATCH   = 22, // throwThenCatch(int)   -> caught in Java; returns x+1000
        SC_SWALLOW      = 23, // swallowInFinally(int) -> finally-return suppresses; x+2000
        SC_SELF_RECOVER = 24, // catchSelfRecover(int) -> caught, internal safeAdd; x+1
        // ── escaping throw whose TYPE changes across a catch ──
        SC_RETHROW_DIFF = 25, // rethrowDifferent(int) -> catch IOException, throw ISE
        // ── arg-position throw: (III)I, throw gated on the LAST arg ──
        SC_ARG_POS      = 26, // throwArgPos(a,b,c)    -> throws from the 3rd position
        // ── recover after a throw with NO defensive clear (library-clear only) ──
        SC_NOCLEAR_REC  = 27, // boom(-1) then safeAdd with NO module clear between
        SC_COUNT        = 28
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

        // Wide / reference witnesses recorded by the argument-shape throwing
        // methods BEFORE they throw (read back to prove the marshalled arg
        // arrived intact across the boundary the exception then unwinds).
        static auto long_field(const char* name) -> std::int64_t { return static_field(name)->get(); }
        static auto string_field(const char* name) -> std::string { return static_field(name)->get().as_string(); }
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

    // ── CONTAINED-throw observation block (throw caught/swallowed in Java) ────
    // Unlike an escaping throw, a contained throw lets call() return a GENUINE
    // value and leaves NO pending exception — so here the returned value IS a
    // contract and the pre-clear ExceptionCheck MUST already be 0.
    struct contained_obs
    {
        std::atomic<bool>         resolved{ false };
        std::atomic<bool>         identity_ok{ false };
        std::atomic<bool>         reached_after{ false };
        std::atomic<bool>         is_int32{ false };
        std::atomic<std::int64_t> value{ k_uncaptured };
        std::atomic<int>          pending_after_call{ -1 };  // ExceptionCheck right after call() (must be 0)
        // a benign safeAdd AFTER the contained call must still work
        std::atomic<bool>         recovery_ok{ false };
        std::atomic<std::int64_t> recovery_value{ k_uncaptured };
    };
    contained_obs g_then_catch;
    contained_obs g_swallow;
    contained_obs g_self_recover;

    // ── NO-CLEAR recovery observation block: throw, then a benign call WITHOUT
    // running the module's defensive clear in between, so the only thing that can
    // keep the thread usable is vmhook's OWN post-call clear (the call-stub fix /
    // the JNI-fallback clear).  A regression of that library clear shows up as the
    // recovery call mis-dispatching.  Best-effort [INFO] (dispatch-path + no-SEH
    // dependent); a final defensive clear still runs so nothing leaks forward.
    struct noclear_obs
    {
        std::atomic<bool>         throw_reached_after{ false };
        std::atomic<int>          pending_after_throw{ -1 };   // ExceptionCheck right after the throwing call(), BEFORE any clear
        std::atomic<bool>         recovery_returned{ false };
        std::atomic<bool>         recovery_is_int32{ false };
        std::atomic<std::int64_t> recovery_value{ k_uncaptured };
        std::atomic<int>          pending_after_final_clear{ -1 };
    };
    noclear_obs g_noclear;

    // ── boundary / non-throwing-branch / idempotency observations ───────────
    // A separate cycle that drives the SAME boom proxy through its NON-throwing
    // branch (x >= 0 returns x) AND its throwing branch on the same thread, plus
    // a back-to-back double throw with NO recovery call between (proving the
    // defensive clear is idempotent), and an INT_MIN boundary throw.
    struct boundary_obs
    {
        std::atomic<bool>         proxy_resolved{ false };
        // boom(0): non-throwing branch; value_t must be int32 0 and is_void false.
        std::atomic<bool>         nothrow_zero_reached{ false };
        std::atomic<bool>         nothrow_zero_is_int32{ false };
        std::atomic<bool>         nothrow_zero_is_void{ true };
        std::atomic<std::int64_t> nothrow_zero_value{ k_uncaptured };
        std::atomic<int>          nothrow_zero_pending{ -1 };   // ExceptionCheck after (must be 0)
        // boom(INT_MAX): non-throwing branch returns INT_MAX.
        std::atomic<bool>         nothrow_max_reached{ false };
        std::atomic<std::int64_t> nothrow_max_value{ k_uncaptured };
        // boom(INT_MIN): throwing branch (boundary-most negative).
        std::atomic<bool>         intmin_reached{ false };
        std::atomic<bool>         intmin_clean{ false };
        // Back-to-back throw with NO recovery between (idempotent clear).
        std::atomic<bool>         double_first_reached{ false };
        std::atomic<bool>         double_second_reached{ false };
        std::atomic<int>          double_pending_after{ -1 };   // ExceptionCheck after both (must be 0)
        // A benign safeAdd AFTER the whole boundary sequence must still work.
        std::atomic<bool>         recovery_ok{ false };
        std::atomic<std::int64_t> recovery_value{ k_uncaptured };

        // ── ADDITIVE deepening: proxy/field introspection + arithmetic edges ──
        // All captured in the SAME boundary detour cycle (no new drive cycle, so
        // the detour/trigger fire-count invariants are untouched).
        // (A) boom proxy introspection: an instance (I)I method is NOT static and
        //     NOT a reference return.
        std::atomic<bool>         boom_is_static{ true };       // expect false
        std::atomic<bool>         boom_is_reference{ true };    // expect false
        std::atomic<bool>         boom_sig_ok{ false };         // signature()=="(I)I"
        std::atomic<bool>         boom_name_ok{ false };        // name()=="boom"
        // (B) sBoom proxy introspection: a static (I)I method IS static, NOT a ref.
        std::atomic<bool>         sboom_resolved{ false };
        std::atomic<bool>         sboom_is_static{ false };     // expect true
        std::atomic<bool>         sboom_is_reference{ true };   // expect false
        std::atomic<bool>         sboom_sig_ok{ false };
        // (C) throwString proxy: (I)Ljava/lang/String; IS a reference return, NOT
        //     static.
        std::atomic<bool>         tstring_resolved{ false };
        std::atomic<bool>         tstring_is_reference{ false };// expect true
        std::atomic<bool>         tstring_is_static{ true };    // expect false
        // (D) safeAdd proxy: instance (I)I, not static, not reference.
        std::atomic<bool>         safeadd_resolved{ false };
        std::atomic<bool>         safeadd_is_static{ true };    // expect false
        std::atomic<bool>         safeadd_is_reference{ true }; // expect false
        // (E) NEGATIVE resolution: an exact name+signature mismatch must NOT
        //     resolve (pinned-overload resolution never falls back to a sibling).
        std::atomic<bool>         neg_wrong_sig_resolved{ true };   // expect false: boom "(J)I"
        std::atomic<bool>         neg_wrong_name_resolved{ true };  // expect false: nope "(I)I"
        std::atomic<bool>         neg_probe_ran{ false };           // both negative probes attempted
        // (F) field_proxy introspection on the health fields.
        std::atomic<bool>         hf_probe_ran{ false };
        std::atomic<bool>         hf_is_static{ true };         // healthField: expect false
        std::atomic<bool>         hf_is_reference{ true };      // healthField: expect false
        std::atomic<bool>         hf_sig_is_I{ false };         // healthField sig=="I"
        std::atomic<bool>         shf_is_static{ false };       // staticHealthField: expect true
        std::atomic<bool>         shf_sig_is_I{ false };        // staticHealthField sig=="I"
        std::atomic<bool>         strfield_resolved{ false };
        std::atomic<bool>         strfield_is_reference{ false };// stringArgLastValue: expect true
        // (G) safeAdd arithmetic edges (well-defined Java int wraparound: x+1).
        std::atomic<bool>         sa_zero_reached{ false };
        std::atomic<std::int64_t> sa_neg_one_value{ k_uncaptured };   // safeAdd(-1)==0
        std::atomic<std::int64_t> sa_int_max_value{ k_uncaptured };   // safeAdd(INT_MAX)==INT_MIN (wrap)
        std::atomic<std::int64_t> sa_int_min_value{ k_uncaptured };   // safeAdd(INT_MIN)==INT_MIN+1
        std::atomic<bool>         sa_edges_clean{ false };             // ExceptionCheck 0 after edges
        // (H) extra non-throwing boom branch values (the SAME proxy that threw).
        std::atomic<std::int64_t> nothrow_one_value{ k_uncaptured };  // boom(1)==1
        std::atomic<std::int64_t> nothrow_big_value{ k_uncaptured };  // boom(0x600D)==0x600D
        // (I) value_t self-consistency on a non-throwing int return: is_void()
        //     false, is_string() false, variant index is the int32 alternative.
        std::atomic<int>          nothrow_zero_variant{ -1 };
        std::atomic<bool>         nothrow_zero_is_string{ true };  // expect false
        // (J) is_void()/is_string() agree with the monostate/string variant on a
        //     contained-throw style value is covered elsewhere; here only the
        //     non-throwing int path is exercised.
    };
    boundary_obs g_boundary;

    constexpr std::int32_t k_int_max{ 0x7FFFFFFF };
    constexpr std::int32_t k_int_min{ static_cast<std::int32_t>(0x80000000U) };

    // Snapshot the dispatch return-value shape of a throwing call() into `o`.
    // Always [INFO]-only on the native side (a throwing call's "return" is the
    // dispatcher's zero default cell, not a contract).
    auto record_return_shape(obs& o, const vmhook::method_proxy::value_t& result) noexcept -> void
    {
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

    // The UNIVERSAL post-throw tail shared by every throwing scenario: snapshot
    // the pre-clear exception state, run the defensive clear, then prove the
    // thread is healthy (benign safeAdd() recovers + returns N+1, an instance
    // field read returns the sentinel, a static field read returns its sentinel).
    // Captures into `o`.  `safe_add_arg` is the recovery call's argument.
    auto post_throw_cleanup_and_health(const std::unique_ptr<throwfix>& self,
                                       obs& o,
                                       std::int32_t safe_add_arg) noexcept -> void
    {
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
            record_return_shape(o, result);
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
            record_return_shape(o, result);
        }

        post_throw_cleanup_and_health(self, o, safe_add_arg);
    }

    // ── argument-shape throwing runners ──────────────────────────────────────
    // Each resolves the throwing method by EXACT name+signature, checks the proxy
    // identity, drives ONE throwing call() with the shape-specific argument
    // (copy-init the value_t), records the return shape, then runs the shared
    // cleanliness + health tail.  The Java body records the marshalled argument
    // BEFORE throwing, so the body-entered / received-arg witnesses (asserted by
    // the caller) prove the shape-specific arg crossed the boundary intact.

    auto run_throwing_no_arg(const std::unique_ptr<throwfix>& self, obs& o,
                             std::int32_t safe_add_arg) noexcept -> void
    {
        if (!self) { return; }
        auto proxy{ self->get_method("throwNoArg", "()I") };
        if (!proxy.has_value()) { return; }
        o.resolved.store(true);
        if (proxy->name() != std::string_view{ "throwNoArg" }
            || proxy->signature() != std::string_view{ "()I" }) { return; }
        o.identity_ok.store(true);
        const vmhook::method_proxy::value_t result = proxy->call();
        record_return_shape(o, result);
        post_throw_cleanup_and_health(self, o, safe_add_arg);
    }

    auto run_throwing_long_arg(const std::unique_ptr<throwfix>& self, obs& o,
                               std::int64_t arg, std::int32_t safe_add_arg) noexcept -> void
    {
        if (!self) { return; }
        auto proxy{ self->get_method("throwLongArg", "(J)I") };
        if (!proxy.has_value()) { return; }
        o.resolved.store(true);
        if (proxy->name() != std::string_view{ "throwLongArg" }
            || proxy->signature() != std::string_view{ "(J)I" }) { return; }
        o.identity_ok.store(true);
        const vmhook::method_proxy::value_t result = proxy->call(arg);
        record_return_shape(o, result);
        post_throw_cleanup_and_health(self, o, safe_add_arg);
    }

    auto run_throwing_double_arg(const std::unique_ptr<throwfix>& self, obs& o,
                                 double arg, std::int32_t safe_add_arg) noexcept -> void
    {
        if (!self) { return; }
        auto proxy{ self->get_method("throwDoubleArg", "(D)I") };
        if (!proxy.has_value()) { return; }
        o.resolved.store(true);
        if (proxy->name() != std::string_view{ "throwDoubleArg" }
            || proxy->signature() != std::string_view{ "(D)I" }) { return; }
        o.identity_ok.store(true);
        const vmhook::method_proxy::value_t result = proxy->call(arg);
        record_return_shape(o, result);
        post_throw_cleanup_and_health(self, o, safe_add_arg);
    }

    auto run_throwing_string_arg(const std::unique_ptr<throwfix>& self, obs& o,
                                 const std::string& arg, std::int32_t safe_add_arg) noexcept -> void
    {
        if (!self) { return; }
        auto proxy{ self->get_method("throwStringArg", "(Ljava/lang/String;)I") };
        if (!proxy.has_value()) { return; }
        o.resolved.store(true);
        if (proxy->name() != std::string_view{ "throwStringArg" }
            || proxy->signature() != std::string_view{ "(Ljava/lang/String;)I" }) { return; }
        o.identity_ok.store(true);
        const vmhook::method_proxy::value_t result = proxy->call(arg);
        record_return_shape(o, result);
        post_throw_cleanup_and_health(self, o, safe_add_arg);
    }

    auto run_throwing_two_args(const std::unique_ptr<throwfix>& self, obs& o,
                               std::int32_t a, std::int64_t b, std::int32_t safe_add_arg) noexcept -> void
    {
        if (!self) { return; }
        auto proxy{ self->get_method("throwTwoArgs", "(IJ)I") };
        if (!proxy.has_value()) { return; }
        o.resolved.store(true);
        if (proxy->name() != std::string_view{ "throwTwoArgs" }
            || proxy->signature() != std::string_view{ "(IJ)I" }) { return; }
        o.identity_ok.store(true);
        const vmhook::method_proxy::value_t result = proxy->call(a, b);
        record_return_shape(o, result);
        post_throw_cleanup_and_health(self, o, safe_add_arg);
    }

    // ── CONTAINED-throw runner: the method throws INTERNALLY but catches /
    // swallows it in Java, so call() returns a GENUINE value and leaves NO
    // pending exception.  Here the returned value IS a contract: we capture it,
    // assert the post-call ExceptionCheck is already 0 (no native clear needed),
    // then prove a benign safeAdd still works.  A defensive clear still runs at
    // the very end as belt-and-braces (it should be a no-op).
    auto run_contained_scenario(const std::unique_ptr<throwfix>& self,
                                contained_obs& o,
                                const char* name,
                                const char* sig,
                                std::int32_t arg,
                                std::int32_t safe_add_arg) noexcept -> void
    {
        if (!self) { return; }
        auto proxy{ self->get_method(name, sig) };
        if (!proxy.has_value()) { return; }
        o.resolved.store(true);
        if (proxy->name() != std::string_view{ name }
            || proxy->signature() != std::string_view{ sig }) { return; }
        o.identity_ok.store(true);

        const vmhook::method_proxy::value_t result = proxy->call(arg);
        o.reached_after.store(true);
        const bool is_int32{ std::holds_alternative<std::int32_t>(result.data) };
        o.is_int32.store(is_int32);
        if (is_int32)
        {
            const std::int32_t v = result;
            o.value.store(static_cast<std::int64_t>(v));
        }

        // A contained throw leaves NO pending exception — verify BEFORE any clear.
        static_cast<void>(vmhook::hotspot::ensure_current_java_thread());
        o.pending_after_call.store(jni_exception_pending());

        // Benign recovery call (no exception involved this time).
        auto sa{ self->get_method("safeAdd", "(I)I") };
        if (sa.has_value())
        {
            const vmhook::method_proxy::value_t r = sa->call(safe_add_arg);
            if (std::holds_alternative<std::int32_t>(r.data))
            {
                const std::int32_t v = r;
                o.recovery_value.store(static_cast<std::int64_t>(v));
                o.recovery_ok.store(true);
            }
            vmhook::detail::jni_exception_clear();
        }

        // Belt-and-braces: nothing should be pending, but clear regardless.
        static_cast<void>(vmhook::hotspot::ensure_current_java_thread());
        vmhook::detail::jni_exception_clear();
        static_cast<void>(raw_clear_pending_exception());
    }

    // ── ARG-POSITION throw runner: throwArgPos(a,b,c) -> (III)I throws on the
    // third arg.  Reuses the shared cleanliness + health tail via an `obs` so the
    // standard triad applies; the positional witnesses are asserted by the caller.
    auto run_arg_pos_scenario(const std::unique_ptr<throwfix>& self,
                              obs& o,
                              std::int32_t a, std::int32_t b, std::int32_t c,
                              std::int32_t safe_add_arg) noexcept -> void
    {
        if (!self) { return; }
        auto proxy{ self->get_method("throwArgPos", "(III)I") };
        if (!proxy.has_value()) { return; }
        o.resolved.store(true);
        if (proxy->name() != std::string_view{ "throwArgPos" }
            || proxy->signature() != std::string_view{ "(III)I" }) { return; }
        o.identity_ok.store(true);

        const vmhook::method_proxy::value_t result = proxy->call(a, b, c);
        record_return_shape(o, result);

        post_throw_cleanup_and_health(self, o, safe_add_arg);
    }

    // ── NO-CLEAR recovery runner: throw boom(-1), then call safeAdd WITHOUT the
    // module's defensive clear in between.  Snapshots the pre-clear exception
    // state, then leans ONLY on vmhook's own post-call clear to keep the recovery
    // call dispatchable.  A final defensive clear runs at the end so nothing leaks
    // forward.  Best-effort [INFO] on the recovery value (dispatch-path / no-SEH
    // dependent); the final-clear cleanliness is the load-bearing HARD invariant.
    auto run_noclear_recovery(const std::unique_ptr<throwfix>& self,
                              noclear_obs& o,
                              std::int32_t safe_add_arg) noexcept -> void
    {
        if (!self) { return; }
        auto boom{ self->get_method("boom", "(I)I") };
        if (!boom.has_value()) { return; }

        const vmhook::method_proxy::value_t r = boom->call(static_cast<std::int32_t>(-1));
        o.throw_reached_after.store(true);
        static_cast<void>(r.is_void());

        // Snapshot the exception state right after the throw, BEFORE any clear:
        // 0 here means vmhook's own post-call clear already fired.
        static_cast<void>(vmhook::hotspot::ensure_current_java_thread());
        o.pending_after_throw.store(jni_exception_pending());

        // The recovery call with NO module clear between (library clear only).
        auto sa{ self->get_method("safeAdd", "(I)I") };
        if (sa.has_value())
        {
            const vmhook::method_proxy::value_t rr = sa->call(safe_add_arg);
            o.recovery_returned.store(true);
            const bool is_int32{ std::holds_alternative<std::int32_t>(rr.data) };
            o.recovery_is_int32.store(is_int32);
            if (is_int32)
            {
                const std::int32_t v = rr;
                o.recovery_value.store(static_cast<std::int64_t>(v));
            }
        }

        // Final defensive clear so absolutely nothing escapes into vmhook.Main.
        o.pending_after_final_clear.store(defensive_clear());
        static_cast<void>(vmhook::hotspot::ensure_current_java_thread());
        vmhook::detail::jni_exception_clear();
        static_cast<void>(raw_clear_pending_exception());
    }

    // Boundary / non-throwing-branch / idempotency drive: ONE detour cycle that
    // exercises a family of edge inputs on the SAME boom proxy.  Proves:
    //   (1) the proxy that JUST threw is reusable for a NON-throwing call —
    //       boom(0) and boom(INT_MAX) return their argument (the real
    //       non-throwing branch), value_t is the int32 alternative (NOT void),
    //       and no exception is pending after a non-throwing call;
    //   (2) the most-negative boundary arg (INT_MIN) throws and clears cleanly;
    //   (3) two throwing calls BACK-TO-BACK with NO recovery call between leave
    //       the thread clean (the defensive clear is idempotent across throws);
    //   (4) a benign safeAdd() still works after the whole sequence.
    auto run_boundary_sequence(const std::unique_ptr<throwfix>& self) noexcept -> void
    {
        if (!self) { return; }
        auto boom{ self->get_method("boom", "(I)I") };
        if (!boom.has_value()) { return; }
        g_boundary.proxy_resolved.store(true);

        // (1a) Non-throwing branch: boom(0) -> 0.  No throw, so no clear needed,
        // but we snapshot ExceptionCheck to prove a non-throwing call leaves it 0.
        {
            const vmhook::method_proxy::value_t r = boom->call(static_cast<std::int32_t>(0));
            g_boundary.nothrow_zero_reached.store(true);
            g_boundary.nothrow_zero_is_void.store(r.is_void());
            const bool is_int32{ std::holds_alternative<std::int32_t>(r.data) };
            g_boundary.nothrow_zero_is_int32.store(is_int32);
            if (is_int32)
            {
                const std::int32_t v = r;
                g_boundary.nothrow_zero_value.store(static_cast<std::int64_t>(v));
            }
            static_cast<void>(vmhook::hotspot::ensure_current_java_thread());
            g_boundary.nothrow_zero_pending.store(jni_exception_pending());
        }

        // (1b) Non-throwing branch boundary: boom(INT_MAX) -> INT_MAX.
        {
            const vmhook::method_proxy::value_t r = boom->call(k_int_max);
            g_boundary.nothrow_max_reached.store(true);
            if (std::holds_alternative<std::int32_t>(r.data))
            {
                const std::int32_t v = r;
                g_boundary.nothrow_max_value.store(static_cast<std::int64_t>(v));
            }
        }

        // (2) Most-negative boundary arg throws; clear and verify clean.
        {
            const vmhook::method_proxy::value_t r = boom->call(k_int_min);
            g_boundary.intmin_reached.store(true);
            static_cast<void>(r.is_void());
            g_boundary.intmin_clean.store(defensive_clear() == 0);
        }

        // (3) Two throws BACK-TO-BACK, NO recovery between, ONE clear after.
        {
            const vmhook::method_proxy::value_t r1 = boom->call(static_cast<std::int32_t>(-100));
            g_boundary.double_first_reached.store(true);
            static_cast<void>(r1.is_void());
            const vmhook::method_proxy::value_t r2 = boom->call(static_cast<std::int32_t>(-200));
            g_boundary.double_second_reached.store(true);
            static_cast<void>(r2.is_void());
            g_boundary.double_pending_after.store(defensive_clear());
        }

        // (4) Benign recovery call after the whole boundary sequence.
        {
            auto sa{ self->get_method("safeAdd", "(I)I") };
            if (sa.has_value())
            {
                const vmhook::method_proxy::value_t r = sa->call(static_cast<std::int32_t>(199));
                if (std::holds_alternative<std::int32_t>(r.data))
                {
                    const std::int32_t v = r;
                    g_boundary.recovery_value.store(static_cast<std::int64_t>(v));
                    g_boundary.recovery_ok.store(true);
                }
                vmhook::detail::jni_exception_clear();
            }
        }

        // ── ADDITIVE deepening, all on this same boundary detour cycle ──────────

        // (5) Proxy introspection on the boom proxy that just threw repeatedly:
        // an instance (I)I method is NOT static and NOT a reference return, and its
        // name/signature are exactly as resolved (pure Method* metadata reads — no
        // JNI, no exception risk).
        g_boundary.boom_is_static.store(boom->is_static());
        g_boundary.boom_is_reference.store(boom->is_reference());
        g_boundary.boom_name_ok.store(boom->name() == std::string_view{ "boom" });
        g_boundary.boom_sig_ok.store(boom->signature() == std::string_view{ "(I)I" });

        // (6) sBoom proxy introspection: a STATIC (I)I method IS static, NOT a ref.
        {
            auto sb{ throwfix::static_method("sBoom", "(I)I") };
            if (sb.has_value())
            {
                g_boundary.sboom_resolved.store(true);
                g_boundary.sboom_is_static.store(sb->is_static());
                g_boundary.sboom_is_reference.store(sb->is_reference());
                g_boundary.sboom_sig_ok.store(sb->signature() == std::string_view{ "(I)I" });
            }
        }

        // (7) throwString proxy: (I)Ljava/lang/String; IS a reference return and is
        // NOT static.  (We do NOT call it here — resolution + descriptor inspection
        // only — so no extra throw is driven this cycle.)
        {
            auto ts{ self->get_method("throwString", "(I)Ljava/lang/String;") };
            if (ts.has_value())
            {
                g_boundary.tstring_resolved.store(true);
                g_boundary.tstring_is_reference.store(ts->is_reference());
                g_boundary.tstring_is_static.store(ts->is_static());
            }
        }

        // (8) safeAdd proxy introspection: instance (I)I, not static, not reference.
        {
            auto sa{ self->get_method("safeAdd", "(I)I") };
            if (sa.has_value())
            {
                g_boundary.safeadd_resolved.store(true);
                g_boundary.safeadd_is_static.store(sa->is_static());
                g_boundary.safeadd_is_reference.store(sa->is_reference());
            }
        }

        // (9) NEGATIVE resolution: an exact name+signature mismatch must NOT
        // resolve.  get_method walks the hierarchy for an EXACT name+descriptor
        // match and pins it; a wrong-descriptor or wrong-name lookup yields nullopt
        // (never a sibling-overload fallback).  This is a pure metadata walk.
        {
            auto wrong_sig { self->get_method("boom", "(J)I") };           // no (J)I boom exists
            auto wrong_name{ self->get_method("noSuchMethodXYZ", "(I)I") };// name absent entirely
            g_boundary.neg_wrong_sig_resolved.store(wrong_sig.has_value());
            g_boundary.neg_wrong_name_resolved.store(wrong_name.has_value());
            g_boundary.neg_probe_ran.store(true);
        }

        // (10) field_proxy introspection on the health/witness fields (metadata
        // only — get()/set() are NOT called here).
        {
            auto hf{ self->get_field("healthField") };
            if (hf.has_value())
            {
                g_boundary.hf_probe_ran.store(true);
                g_boundary.hf_is_static.store(hf->is_static());
                g_boundary.hf_is_reference.store(hf->is_reference());
                g_boundary.hf_sig_is_I.store(hf->signature() == std::string_view{ "I" });
            }
            auto shf{ throwfix::static_field("staticHealthField") };
            if (shf.has_value())
            {
                g_boundary.shf_is_static.store(shf->is_static());
                g_boundary.shf_sig_is_I.store(shf->signature() == std::string_view{ "I" });
            }
            auto strf{ throwfix::static_field("stringArgLastValue") };
            if (strf.has_value())
            {
                g_boundary.strfield_resolved.store(true);
                g_boundary.strfield_is_reference.store(strf->is_reference());
            }
        }

        // (11) safeAdd arithmetic edges — Java int is 32-bit two's-complement with
        // well-defined wraparound, so each result is CERTAIN: safeAdd(x) == x+1.
        //   safeAdd(-1)       == 0
        //   safeAdd(INT_MAX)  == INT_MIN   (0x7FFFFFFF + 1 wraps to 0x80000000)
        //   safeAdd(INT_MIN)  == INT_MIN+1 (0x80000001)
        {
            auto sa{ self->get_method("safeAdd", "(I)I") };
            if (sa.has_value())
            {
                g_boundary.sa_zero_reached.store(true);
                {
                    const vmhook::method_proxy::value_t r = sa->call(static_cast<std::int32_t>(-1));
                    if (std::holds_alternative<std::int32_t>(r.data))
                    {
                        const std::int32_t v = r;
                        g_boundary.sa_neg_one_value.store(static_cast<std::int64_t>(v));
                    }
                }
                {
                    const vmhook::method_proxy::value_t r = sa->call(k_int_max);
                    if (std::holds_alternative<std::int32_t>(r.data))
                    {
                        const std::int32_t v = r;
                        g_boundary.sa_int_max_value.store(static_cast<std::int64_t>(v));
                    }
                }
                {
                    const vmhook::method_proxy::value_t r = sa->call(k_int_min);
                    if (std::holds_alternative<std::int32_t>(r.data))
                    {
                        const std::int32_t v = r;
                        g_boundary.sa_int_min_value.store(static_cast<std::int64_t>(v));
                    }
                }
                vmhook::detail::jni_exception_clear();
                static_cast<void>(vmhook::hotspot::ensure_current_java_thread());
                g_boundary.sa_edges_clean.store(jni_exception_pending() == 0);
            }
        }

        // (12) Extra non-throwing boom branch values via the SAME proxy that threw.
        // boom(x) returns x unchanged for x >= 0, so each is exact.
        {
            const vmhook::method_proxy::value_t r = boom->call(static_cast<std::int32_t>(1));
            if (std::holds_alternative<std::int32_t>(r.data))
            {
                const std::int32_t v = r;
                g_boundary.nothrow_one_value.store(static_cast<std::int64_t>(v));
            }
        }
        {
            const vmhook::method_proxy::value_t r = boom->call(static_cast<std::int32_t>(0x600D));
            if (std::holds_alternative<std::int32_t>(r.data))
            {
                const std::int32_t v = r;
                g_boundary.nothrow_big_value.store(static_cast<std::int64_t>(v));
            }
        }

        // (13) value_t self-consistency on a non-throwing int return: is_string()
        // is false and the live variant index equals the int32 alternative's index.
        {
            const vmhook::method_proxy::value_t r = boom->call(static_cast<std::int32_t>(0));
            g_boundary.nothrow_zero_is_string.store(r.is_string());
            g_boundary.nothrow_zero_variant.store(static_cast<int>(r.data.index()));
        }

        // Final clear so nothing escapes into vmhook.Main.
        static_cast<void>(vmhook::hotspot::ensure_current_java_thread());
        vmhook::detail::jni_exception_clear();
        static_cast<void>(raw_clear_pending_exception());
    }

    // A scenario id reserved for the boundary cycle (drive() programs `scenario`
    // with it; the detour dispatches to run_boundary_sequence).
    constexpr std::int32_t SC_BOUNDARY{ 1000 };

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
        // ── return-descriptor variety: (I)<ret>, int arg, recorded before throw ──
        case SC_RET_VOID:   run_throwing_scenario(self, g_obs[SC_RET_VOID],   "throwVoid",   "(I)V",                  -11, false, 110); break;
        case SC_RET_LONG:   run_throwing_scenario(self, g_obs[SC_RET_LONG],   "throwLong",   "(I)J",                  -12, false, 111); break;
        case SC_RET_DOUBLE: run_throwing_scenario(self, g_obs[SC_RET_DOUBLE], "throwDouble", "(I)D",                  -13, false, 112); break;
        case SC_RET_BOOL:   run_throwing_scenario(self, g_obs[SC_RET_BOOL],   "throwBool",   "(I)Z",                  -14, false, 113); break;
        case SC_RET_STRING: run_throwing_scenario(self, g_obs[SC_RET_STRING], "throwString", "(I)Ljava/lang/String;", -15, false, 114); break;
        // ── argument-shape variety: dedicated runners marshal the wide/ref arg ──
        case SC_ARG_NONE:   run_throwing_no_arg    (self, g_obs[SC_ARG_NONE],                                              115); break;
        case SC_ARG_LONG:   run_throwing_long_arg  (self, g_obs[SC_ARG_LONG],   static_cast<std::int64_t>(0x0123456789ABCDEFLL), 116); break;
        case SC_ARG_DOUBLE: run_throwing_double_arg(self, g_obs[SC_ARG_DOUBLE], 3.141592653589793,                              117); break;
        case SC_ARG_STRING: run_throwing_string_arg(self, g_obs[SC_ARG_STRING], std::string{ "throw-arg-marshal" },             118); break;
        case SC_ARG_TWO:    run_throwing_two_args  (self, g_obs[SC_ARG_TWO],    static_cast<std::int32_t>(0x6A6A),
                                                    static_cast<std::int64_t>(0x7EDCBA9812345678LL),                            119); break;
        // ── extra unwind shapes ──
        case SC_DEEP3:    run_throwing_scenario(self, g_obs[SC_DEEP3],    "throwDeep3",    "(I)I", -21, false, 120); break;
        case SC_FINALLY:  run_throwing_scenario(self, g_obs[SC_FINALLY],  "throwInFinally","(I)I", -22, false, 121); break;
        // ── CONTAINED throws: the exception never escapes Java; value IS a contract ──
        case SC_THEN_CATCH:   run_contained_scenario(self, g_then_catch,   "throwThenCatch",  "(I)I", 23, 122); break;
        case SC_SWALLOW:      run_contained_scenario(self, g_swallow,      "swallowInFinally","(I)I", 24, 123); break;
        case SC_SELF_RECOVER: run_contained_scenario(self, g_self_recover, "catchSelfRecover","(I)I", 25, 124); break;
        // ── escaping throw whose TYPE changes across a catch ──
        case SC_RETHROW_DIFF: run_throwing_scenario(self, g_obs[SC_RETHROW_DIFF], "rethrowDifferent", "(I)I", -26, false, 125); break;
        // ── arg-position throw: (III)I, throws from the 3rd position ──
        case SC_ARG_POS:  run_arg_pos_scenario(self, g_obs[SC_ARG_POS], 0x1A, 0x2B, 0x3C, 126); break;
        // ── recover after a throw with NO module clear (library-clear only) ──
        case SC_NOCLEAR_REC: run_noclear_recovery(self, g_noclear, 127); break;
        default:
            if (sc == SC_BOUNDARY) { run_boundary_sequence(self); }
            break;
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

    // The portion of the per-scenario contract that is IDENTICAL for every
    // throwing call regardless of its argument shape or return descriptor:
    // resolution + identity, the headline "reached the line after call()" proof,
    // the thread-left-clean invariant, and the post-throw JVM-health triad.
    // Returns false (and records the skip) when the probe did not complete this
    // cycle, so callers can short-circuit their shape-specific witness checks.
    auto assert_common_triad(vmhook_test::context& ctx,
                             const char* prefix,
                             bool probe_done,
                             const obs& o,
                             std::int32_t safe_add_arg) -> bool
    {
        const auto name = [&](const char* suffix) { return std::string{ prefix } + "_" + suffix; };

        ctx.check(name("probe_completed"), probe_done);
        if (!probe_done)
        {
            ctx.record("[INFO] method_throwing_call_site/" + std::string{ prefix }
                       + ": probe did not complete; observations not captured this cycle.");
            return false;
        }

        // Resolution + identity survived the in-detour guards.
        ctx.check(name("method_resolved"),    o.resolved.load());
        ctx.check(name("method_identity_ok"), o.identity_ok.load());

        // *** THE headline proof: the line after the throwing call() executed. ***
        ctx.check(name("reached_line_after_throwing_call"), o.reached_after.load());

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
        return true;
    }

    // Per-scenario assertion block for the int-arg throwing methods.  `prefix` is
    // unique per scenario so failures are unambiguous; `entered_field`/`arg_field`
    // are the Java witnesses; `expected_arg` is the sentinel the native side
    // marshalled; `safe_add_arg` is what safeAdd was called with this cycle.
    auto assert_scenario(vmhook_test::context& ctx,
                         const char* prefix,
                         bool probe_done,
                         const obs& o,
                         const char* entered_field,
                         const char* arg_field,
                         std::int32_t expected_arg,
                         std::int32_t safe_add_arg) -> void
    {
        if (!assert_common_triad(ctx, prefix, probe_done, o, safe_add_arg))
        {
            return;
        }
        const auto name = [&](const char* suffix) { return std::string{ prefix } + "_" + suffix; };
        // The throwing method genuinely ran with our marshalled int arg.
        ctx.check(name("body_entered"), throwfix::entered(entered_field) >= 1);
        ctx.check(name("received_arg"), throwfix::last_arg(arg_field) == expected_arg);
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
    // New return-descriptor + argument-shape + extra-unwind witnesses.
    ctx.check("tcs_voidEntered_field_resolves",      throwfix::resolves("voidEntered"));
    ctx.check("tcs_longRetEntered_field_resolves",   throwfix::resolves("longRetEntered"));
    ctx.check("tcs_doubleRetEntered_field_resolves", throwfix::resolves("doubleRetEntered"));
    ctx.check("tcs_boolRetEntered_field_resolves",   throwfix::resolves("boolRetEntered"));
    ctx.check("tcs_stringRetEntered_field_resolves", throwfix::resolves("stringRetEntered"));
    ctx.check("tcs_noArgEntered_field_resolves",     throwfix::resolves("noArgEntered"));
    ctx.check("tcs_longArgLast_field_resolves",      throwfix::resolves("longArgLast"));
    ctx.check("tcs_doubleArgLastBits_field_resolves",throwfix::resolves("doubleArgLastBits"));
    ctx.check("tcs_stringArgLastLen_field_resolves", throwfix::resolves("stringArgLastLen"));
    ctx.check("tcs_twoArgsLastB_field_resolves",     throwfix::resolves("twoArgsLastB"));
    ctx.check("tcs_deep3InnerEntered_field_resolves",throwfix::resolves("deep3InnerEntered"));
    ctx.check("tcs_finallyRan_field_resolves",       throwfix::resolves("finallyRan"));
    // Contained-throw / rethrow / arg-position witnesses.
    ctx.check("tcs_catchHandled_field_resolves",     throwfix::resolves("catchHandled"));
    ctx.check("tcs_swallowFinallyRan_field_resolves",throwfix::resolves("swallowFinallyRan"));
    ctx.check("tcs_selfRecoverHandled_field_resolves",throwfix::resolves("selfRecoverHandled"));
    ctx.check("tcs_rethrowCaughtInner_field_resolves",throwfix::resolves("rethrowCaughtInner"));
    ctx.check("tcs_argPosC_field_resolves",          throwfix::resolves("argPosC"));

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

    // ── return-descriptor variety: same int-arg contract, different ret_char ──
    const bool d_ret_void   { drive(ctx, SC_RET_VOID)   };
    assert_scenario(ctx, "tcs_ret_void",   d_ret_void,   g_obs[SC_RET_VOID],   "voidEntered",      "voidLastArg",      -11, 110);
    const bool d_ret_long   { drive(ctx, SC_RET_LONG)   };
    assert_scenario(ctx, "tcs_ret_long",   d_ret_long,   g_obs[SC_RET_LONG],   "longRetEntered",   "longRetLastArg",   -12, 111);
    const bool d_ret_double { drive(ctx, SC_RET_DOUBLE) };
    assert_scenario(ctx, "tcs_ret_double", d_ret_double, g_obs[SC_RET_DOUBLE], "doubleRetEntered", "doubleRetLastArg", -13, 112);
    const bool d_ret_bool   { drive(ctx, SC_RET_BOOL)   };
    assert_scenario(ctx, "tcs_ret_bool",   d_ret_bool,   g_obs[SC_RET_BOOL],   "boolRetEntered",   "boolRetLastArg",   -14, 113);
    const bool d_ret_string { drive(ctx, SC_RET_STRING) };
    assert_scenario(ctx, "tcs_ret_string", d_ret_string, g_obs[SC_RET_STRING], "stringRetEntered", "stringRetLastArg", -15, 114);

    // Characterize the unwound-path return SHAPE per return descriptor (the
    // "default cell": result_holder was never written by the throwing callee).
    // [INFO] only — a throwing call's return value is not a contract — but it
    // documents the decoded variant for each width so a future regression that
    // (e.g.) started returning monostate uniformly is visible.
    if (d_ret_void)
    {
        ctx.record("[INFO] method_throwing_call_site/tcs_ret_void: (I)V throwing call -> is_void="
                   + std::string(g_obs[SC_RET_VOID].ret_is_void.load() ? "true" : "false")
                   + " variant=" + std::to_string(g_obs[SC_RET_VOID].ret_variant.load())
                   + " (void decode on the unwound path).");
    }
    if (d_ret_long)
    {
        ctx.record("[INFO] method_throwing_call_site/tcs_ret_long: (I)J throwing call -> variant="
                   + std::to_string(g_obs[SC_RET_LONG].ret_variant.load())
                   + " (64-bit default-cell decode; NOT asserted).");
    }
    if (d_ret_string)
    {
        ctx.record("[INFO] method_throwing_call_site/tcs_ret_string: (I)Ljava/lang/String; throwing "
                   "call -> is_void=" + std::string(g_obs[SC_RET_STRING].ret_is_void.load() ? "true" : "false")
                   + " variant=" + std::to_string(g_obs[SC_RET_STRING].ret_variant.load())
                   + " (reference decode of the zero default cell -> null -> monostate).");
    }

    // ── argument-shape variety: dedicated witnesses prove the wide/ref arg ──
    const bool d_arg_none   { drive(ctx, SC_ARG_NONE)   };
    if (assert_common_triad(ctx, "tcs_arg_none", d_arg_none, g_obs[SC_ARG_NONE], 115))
    {
        ctx.check("tcs_arg_none_body_entered", throwfix::entered("noArgEntered") >= 1);
    }

    const bool d_arg_long   { drive(ctx, SC_ARG_LONG)   };
    if (assert_common_triad(ctx, "tcs_arg_long", d_arg_long, g_obs[SC_ARG_LONG], 116))
    {
        ctx.check("tcs_arg_long_body_entered", throwfix::entered("longArgEntered") >= 1);
        // The full 64-bit arg crossed the boundary intact (a truncation/shift
        // would corrupt this even though the method threw).
        ctx.check("tcs_arg_long_received_full_64bit",
                  throwfix::long_field("longArgLast")
                  == static_cast<std::int64_t>(0x0123456789ABCDEFLL));
    }

    const bool d_arg_double { drive(ctx, SC_ARG_DOUBLE) };
    if (assert_common_triad(ctx, "tcs_arg_double", d_arg_double, g_obs[SC_ARG_DOUBLE], 117))
    {
        ctx.check("tcs_arg_double_body_entered", throwfix::entered("doubleArgEntered") >= 1);
        // doubleToRawLongBits(3.141592653589793) == 0x400921FB54442D18 (Math.PI).
        ctx.check("tcs_arg_double_received_exact_bits",
                  throwfix::long_field("doubleArgLastBits")
                  == static_cast<std::int64_t>(0x400921FB54442D18LL));
    }

    const bool d_arg_string { drive(ctx, SC_ARG_STRING) };
    if (assert_common_triad(ctx, "tcs_arg_string", d_arg_string, g_obs[SC_ARG_STRING], 118))
    {
        ctx.check("tcs_arg_string_body_entered", throwfix::entered("stringArgEntered") >= 1);
        // The marshalled java.lang.String reached the body: its length matches and
        // its decoded content equals the native sentinel.
        ctx.check("tcs_arg_string_received_length",
                  throwfix::last_arg("stringArgLastLen")
                  == static_cast<std::int32_t>(std::string{ "throw-arg-marshal" }.size()));
        ctx.check("tcs_arg_string_received_value",
                  throwfix::string_field("stringArgLastValue") == "throw-arg-marshal");
    }

    const bool d_arg_two    { drive(ctx, SC_ARG_TWO)    };
    if (assert_common_triad(ctx, "tcs_arg_two", d_arg_two, g_obs[SC_ARG_TWO], 119))
    {
        ctx.check("tcs_arg_two_body_entered", throwfix::entered("twoArgsEntered") >= 1);
        // The int and the wide long both arrived intact across the two-slot frame
        // (a slot-shift from the wide arg would corrupt one of these).
        ctx.check("tcs_arg_two_received_int",
                  throwfix::last_arg("twoArgsLastA") == static_cast<std::int32_t>(0x6A6A));
        ctx.check("tcs_arg_two_received_long",
                  throwfix::long_field("twoArgsLastB")
                  == static_cast<std::int64_t>(0x7EDCBA9812345678LL));
    }

    // ── extra unwind shapes ──
    const bool d_deep3   { drive(ctx, SC_DEEP3)   };
    assert_scenario(ctx, "tcs_deep3",   d_deep3,   g_obs[SC_DEEP3],   "deep3Entered",   "deep3LastArg",   -21, 120);
    const bool d_finally { drive(ctx, SC_FINALLY) };
    assert_scenario(ctx, "tcs_finally", d_finally, g_obs[SC_FINALLY], "finallyEntered", "finallyLastArg", -22, 121);

    // The three-frame unwind ran every intermediate frame; the finally handler's
    // committed side effect ran on the way out.
    if (d_deep3)
    {
        ctx.check("tcs_deep3_mid_frame_entered",   throwfix::entered("deep3MidEntered")   >= 1);
        ctx.check("tcs_deep3_inner_frame_entered", throwfix::entered("deep3InnerEntered") >= 1);
    }
    if (d_finally)
    {
        ctx.check("tcs_finally_handler_ran", throwfix::entered("finallyRan") >= 1);
    }

    // =====================================================================
    //  2a-bis. CONTAINED throws: the exception is caught / swallowed INSIDE
    //  Java, so call() returns a GENUINE value and leaves NO pending exception.
    //  Here the returned value IS a contract (unlike an escaping throw) and the
    //  thread must already be clean WITHOUT a native clear.
    // =====================================================================
    // A small shared assertion block for a contained-throw scenario.
    const auto assert_contained =
        [&](const char* prefix, bool probe_done, const contained_obs& o,
            const char* entered_field, const char* handled_field,
            std::int32_t expected_value, std::int32_t safe_add_arg) -> void
    {
        const auto nm = [&](const char* s) { return std::string{ prefix } + "_" + s; };
        ctx.check(nm("probe_completed"), probe_done);
        if (!probe_done) { return; }
        ctx.check(nm("method_resolved"),    o.resolved.load());
        ctx.check(nm("method_identity_ok"), o.identity_ok.load());
        // The line after the (contained) call() ran — no AV.
        ctx.check(nm("reached_line_after_call"), o.reached_after.load());
        // The body genuinely entered and its catch/finally handler ran.
        ctx.check(nm("body_entered"),  throwfix::entered(entered_field) >= 1);
        ctx.check(nm("handler_ran"),   throwfix::entered(handled_field) >= 1);
        // A contained throw returns a REAL int and leaves the thread clean
        // WITHOUT any native clear — both are HARD contracts here.
        ctx.check(nm("returned_int32"), o.is_int32.load());
        ctx.check(nm("returned_expected_value"),
                  o.value.load() == static_cast<std::int64_t>(expected_value));
        ctx.check(nm("no_pending_exception_without_clear"),
                  o.pending_after_call.load() == 0);
        // The thread is still usable for a benign call afterwards.
        ctx.check(nm("recovery_ok"), o.recovery_ok.load());
        ctx.check(nm("recovery_value_plus_one"),
                  o.recovery_value.load() == static_cast<std::int64_t>(safe_add_arg) + 1);
    };

    const bool d_then_catch{ drive(ctx, SC_THEN_CATCH) };
    // throwThenCatch(23) catches and returns 23 + 1000 == 1023.
    assert_contained("tcs_then_catch", d_then_catch, g_then_catch,
                     "catchEntered", "catchHandled", 1023, 122);

    const bool d_swallow{ drive(ctx, SC_SWALLOW) };
    // swallowInFinally(24): finally-return suppresses the throw -> 24 + 2000 == 2024.
    assert_contained("tcs_swallow", d_swallow, g_swallow,
                     "swallowEntered", "swallowFinallyRan", 2024, 123);

    const bool d_self_recover{ drive(ctx, SC_SELF_RECOVER) };
    // catchSelfRecover(25): catches then returns safeAdd(25) == 26.
    assert_contained("tcs_self_recover", d_self_recover, g_self_recover,
                     "selfRecoverEntered", "selfRecoverHandled", 26, 124);

    // =====================================================================
    //  2a-ter. RETHROW with a DIFFERENT type: the type that escapes into native
    //  is NOT the type first thrown.  Standard escaping-throw triad applies; the
    //  inner-caught witness proves the catch-and-rethrow path ran.
    // =====================================================================
    const bool d_rethrow{ drive(ctx, SC_RETHROW_DIFF) };
    assert_scenario(ctx, "tcs_rethrow_diff", d_rethrow, g_obs[SC_RETHROW_DIFF],
                    "rethrowEntered", "rethrowLastArg", -26, 125);
    if (d_rethrow)
    {
        // The inner IOException was caught before the different type was thrown.
        ctx.check("tcs_rethrow_inner_caught", throwfix::entered("rethrowCaughtInner") >= 1);
    }

    // =====================================================================
    //  2a-quater. ARG-POSITION throw: throwArgPos(a,b,c) -> (III)I throws from
    //  the THIRD argument position.  The standard escaping triad applies AND
    //  every positional int is proven to have crossed the boundary intact even
    //  though the call unwinds from the last position.
    // =====================================================================
    const bool d_arg_pos{ drive(ctx, SC_ARG_POS) };
    if (assert_common_triad(ctx, "tcs_arg_pos", d_arg_pos, g_obs[SC_ARG_POS], 126))
    {
        ctx.check("tcs_arg_pos_body_entered", throwfix::entered("argPosEntered") >= 1);
        ctx.check("tcs_arg_pos_received_a", throwfix::last_arg("argPosA") == static_cast<std::int32_t>(0x1A));
        ctx.check("tcs_arg_pos_received_b", throwfix::last_arg("argPosB") == static_cast<std::int32_t>(0x2B));
        ctx.check("tcs_arg_pos_received_c", throwfix::last_arg("argPosC") == static_cast<std::int32_t>(0x3C));
    }

    // =====================================================================
    //  2a-quinquies. NO-CLEAR recovery: throw boom(-1), then a benign safeAdd
    //  WITHOUT the module's defensive clear between them.  The only thing that can
    //  keep the recovery call dispatchable is vmhook's OWN post-call clear (the
    //  call-stub fix / JNI-fallback clear).  HARD: the line after the throw ran,
    //  and the FINAL defensive clear leaves the thread clean.  [INFO] (best-
    //  effort, dispatch-path + no-SEH dependent): whether the recovery call
    //  succeeded on the library clear alone, and the pre-(module-)clear state.
    // =====================================================================
    const bool d_noclear{ drive(ctx, SC_NOCLEAR_REC) };
    ctx.check("tcs_noclear_probe_completed", d_noclear);
    if (d_noclear)
    {
        // HARD: no AV reaching the line after the throwing call.
        ctx.check("tcs_noclear_reached_after_throw", g_noclear.throw_reached_after.load());
        // HARD: after the FINAL defensive clear the thread is clean (load-bearing
        // cross-module poison guard — independent of whether the library auto-
        // clear fired).
        ctx.check("tcs_noclear_final_clear_clean",
                  g_noclear.pending_after_final_clear.load() == 0);
        // [INFO]: the library-clear-only recovery result + the pre-module-clear
        // exception state (1 => call-stub path left it for our final clear; 0 =>
        // vmhook already cleared inside call()).  Not asserted: it is dispatch-
        // path + no-SEH dependent.
        {
            std::string line{ "[INFO] method_throwing_call_site/tcs_noclear: "
                              "ExceptionCheck-immediately-after-throw=" };
            line += std::to_string(g_noclear.pending_after_throw.load());
            line += " recovery_returned=" + std::string(g_noclear.recovery_returned.load() ? "true" : "false");
            line += " recovery_is_int32=" + std::string(g_noclear.recovery_is_int32.load() ? "true" : "false");
            line += " recovery_value=" + std::to_string(g_noclear.recovery_value.load())
                  + " (safeAdd(127) should be 128 IFF the library clear kept the "
                    "thread usable without our clear; best-effort).";
            ctx.record(line);
        }
        // When the library clear DID keep the thread clean (pending==0 right after
        // the throw), the recovery call must have produced 128 — assert only in
        // that observed-clean case so the check is never vacuous nor path-fragile.
        if (g_noclear.pending_after_throw.load() == 0 && g_noclear.recovery_returned.load())
        {
            ctx.check("tcs_noclear_recovery_value_when_lib_cleared",
                      g_noclear.recovery_value.load() == 128);
        }
    }

    // =====================================================================
    //  2b. Boundary / non-throwing-branch / idempotency cycle (one drive).
    // =====================================================================
    const bool d_boundary{ drive(ctx, SC_BOUNDARY) };
    ctx.check("tcs_boundary_probe_completed", d_boundary);
    if (d_boundary)
    {
        ctx.check("tcs_boundary_proxy_resolved", g_boundary.proxy_resolved.load());

        // (1) The SAME proxy that throws is reusable for the NON-throwing branch:
        //     boom(0) -> 0 (int32 alternative, NOT void), no pending exception.
        ctx.check("tcs_boundary_nothrow_zero_reached",  g_boundary.nothrow_zero_reached.load());
        ctx.check("tcs_boundary_nothrow_zero_is_int32", g_boundary.nothrow_zero_is_int32.load());
        ctx.check("tcs_boundary_nothrow_zero_not_void", !g_boundary.nothrow_zero_is_void.load());
        ctx.check("tcs_boundary_nothrow_zero_value_is_zero",
                  g_boundary.nothrow_zero_value.load() == 0);
        ctx.check("tcs_boundary_nothrow_call_left_thread_clean",
                  g_boundary.nothrow_zero_pending.load() == 0);

        // (1b) boom(INT_MAX) non-throwing branch returns INT_MAX exactly.
        ctx.check("tcs_boundary_nothrow_max_reached", g_boundary.nothrow_max_reached.load());
        ctx.check("tcs_boundary_nothrow_max_value_is_int_max",
                  g_boundary.nothrow_max_value.load() == static_cast<std::int64_t>(k_int_max));

        // (2) INT_MIN throwing-branch boundary unwinds and clears cleanly.
        ctx.check("tcs_boundary_intmin_reached", g_boundary.intmin_reached.load());
        ctx.check("tcs_boundary_intmin_clean",   g_boundary.intmin_clean.load());

        // (3) Two throws back-to-back with NO recovery between leave the thread
        //     clean after a SINGLE clear (the defensive clear is idempotent).
        ctx.check("tcs_boundary_double_first_reached",  g_boundary.double_first_reached.load());
        ctx.check("tcs_boundary_double_second_reached", g_boundary.double_second_reached.load());
        ctx.check("tcs_boundary_double_thread_clean_after_both",
                  g_boundary.double_pending_after.load() == 0);

        // (4) A benign call still works after the whole boundary sequence.
        ctx.check("tcs_boundary_recovery_ok", g_boundary.recovery_ok.load());
        ctx.check("tcs_boundary_recovery_value_plus_one",
                  g_boundary.recovery_value.load() == static_cast<std::int64_t>(200));

        // ── ADDITIVE deepening assertions (captured on the same boundary cycle) ──

        // (5) boom proxy introspection: an instance (I)I method is NOT static and
        //     NOT a reference return; name()/signature() are exactly as resolved.
        ctx.check("tcs_boundary_boom_not_static",     !g_boundary.boom_is_static.load());
        ctx.check("tcs_boundary_boom_not_reference",  !g_boundary.boom_is_reference.load());
        ctx.check("tcs_boundary_boom_name_is_boom",   g_boundary.boom_name_ok.load());
        ctx.check("tcs_boundary_boom_sig_is_int_int", g_boundary.boom_sig_ok.load());

        // (6) sBoom proxy introspection: a STATIC (I)I method IS static, NOT a ref.
        ctx.check("tcs_boundary_sboom_resolved",     g_boundary.sboom_resolved.load());
        if (g_boundary.sboom_resolved.load())
        {
            ctx.check("tcs_boundary_sboom_is_static",     g_boundary.sboom_is_static.load());
            ctx.check("tcs_boundary_sboom_not_reference", !g_boundary.sboom_is_reference.load());
            ctx.check("tcs_boundary_sboom_sig_is_int_int", g_boundary.sboom_sig_ok.load());
        }

        // (7) throwString proxy: (I)Ljava/lang/String; IS a reference return and is
        //     NOT static (descriptor introspection only — not called).
        ctx.check("tcs_boundary_tstring_resolved", g_boundary.tstring_resolved.load());
        if (g_boundary.tstring_resolved.load())
        {
            ctx.check("tcs_boundary_tstring_is_reference", g_boundary.tstring_is_reference.load());
            ctx.check("tcs_boundary_tstring_not_static",   !g_boundary.tstring_is_static.load());
        }

        // (8) safeAdd proxy introspection: instance (I)I, not static, not reference.
        ctx.check("tcs_boundary_safeadd_resolved", g_boundary.safeadd_resolved.load());
        if (g_boundary.safeadd_resolved.load())
        {
            ctx.check("tcs_boundary_safeadd_not_static",    !g_boundary.safeadd_is_static.load());
            ctx.check("tcs_boundary_safeadd_not_reference", !g_boundary.safeadd_is_reference.load());
        }

        // (9) NEGATIVE resolution: a wrong-descriptor or wrong-name lookup must NOT
        //     resolve (pinned exact-match resolution; no sibling fallback).
        ctx.check("tcs_boundary_neg_probe_ran", g_boundary.neg_probe_ran.load());
        if (g_boundary.neg_probe_ran.load())
        {
            ctx.check("tcs_boundary_neg_wrong_sig_not_resolved",  !g_boundary.neg_wrong_sig_resolved.load());
            ctx.check("tcs_boundary_neg_wrong_name_not_resolved", !g_boundary.neg_wrong_name_resolved.load());
        }

        // (10) field_proxy introspection: healthField is a non-static primitive int
        //      field; staticHealthField is a static primitive int field;
        //      stringArgLastValue is a reference field.
        ctx.check("tcs_boundary_hf_probe_ran", g_boundary.hf_probe_ran.load());
        if (g_boundary.hf_probe_ran.load())
        {
            ctx.check("tcs_boundary_healthField_not_static",    !g_boundary.hf_is_static.load());
            ctx.check("tcs_boundary_healthField_not_reference", !g_boundary.hf_is_reference.load());
            ctx.check("tcs_boundary_healthField_sig_is_I",      g_boundary.hf_sig_is_I.load());
            ctx.check("tcs_boundary_staticHealthField_is_static", g_boundary.shf_is_static.load());
            ctx.check("tcs_boundary_staticHealthField_sig_is_I",  g_boundary.shf_sig_is_I.load());
        }
        ctx.check("tcs_boundary_strfield_resolved", g_boundary.strfield_resolved.load());
        if (g_boundary.strfield_resolved.load())
        {
            ctx.check("tcs_boundary_stringArgLastValue_is_reference",
                      g_boundary.strfield_is_reference.load());
        }

        // (11) safeAdd arithmetic edges — Java int wraparound is well-defined, so
        //      each result is certain (safeAdd(x) == x+1, 32-bit two's complement).
        ctx.check("tcs_boundary_sa_edges_ran", g_boundary.sa_zero_reached.load());
        if (g_boundary.sa_zero_reached.load())
        {
            ctx.check("tcs_boundary_safeAdd_neg_one_is_zero",
                      g_boundary.sa_neg_one_value.load() == 0);
            // INT_MAX + 1 wraps to INT_MIN.
            ctx.check("tcs_boundary_safeAdd_int_max_wraps_to_int_min",
                      g_boundary.sa_int_max_value.load() == static_cast<std::int64_t>(k_int_min));
            // INT_MIN + 1 == 0x80000001.
            ctx.check("tcs_boundary_safeAdd_int_min_plus_one",
                      g_boundary.sa_int_min_value.load()
                      == static_cast<std::int64_t>(static_cast<std::int32_t>(k_int_min + 1)));
            ctx.check("tcs_boundary_safeAdd_edges_left_thread_clean",
                      g_boundary.sa_edges_clean.load());
        }

        // (12) Extra non-throwing boom branch values via the SAME proxy that threw.
        ctx.check("tcs_boundary_nothrow_one_value_is_one",
                  g_boundary.nothrow_one_value.load() == 1);
        ctx.check("tcs_boundary_nothrow_big_value_exact",
                  g_boundary.nothrow_big_value.load() == static_cast<std::int64_t>(0x600D));

        // (13) value_t self-consistency on a non-throwing int return: is_string()
        //      is false, and the live variant index equals the int32 alternative's
        //      index (derived from the variant type itself, so no magic number).
        {
            // Derive the int32 alternative's index from the variant type itself
            // (a runtime probe, so no constexpr-variant toolchain edge cases) —
            // value_t is an aggregate, so this brace-inits .data with the int32.
            const vmhook::method_proxy::value_t int32_probe{ std::int32_t{ 0 } };
            const int k_int32_variant_index{ static_cast<int>(int32_probe.data.index()) };
            ctx.check("tcs_boundary_nothrow_zero_not_string", !g_boundary.nothrow_zero_is_string.load());
            ctx.check("tcs_boundary_nothrow_zero_variant_is_int32",
                      g_boundary.nothrow_zero_variant.load() == k_int32_variant_index);
            // Cross-check: the variant index agrees with the earlier is_int32 flag.
            ctx.check("tcs_boundary_nothrow_zero_variant_agrees_with_is_int32",
                      (g_boundary.nothrow_zero_variant.load() == k_int32_variant_index)
                      == g_boundary.nothrow_zero_is_int32.load());
        }
    }

    // =====================================================================
    //  3. Cross-scenario invariants — the throws did not wedge the handshake
    //     and the inner Java frame of the nested throw genuinely ran.
    // =====================================================================
    // One detour per throwing scenario PLUS the boundary cycle.
    constexpr int k_total_cycles{ static_cast<int>(SC_COUNT) + 1 };
    ctx.check("tcs_detour_ran_each_cycle", g_detour_calls.load() == k_total_cycles);
    ctx.check("tcs_self_valid_in_detour",  g_self_valid.load());
    // trigger() ran exactly once per cycle (the throws never re-entered it).
    ctx.check("tcs_trigger_count_matches_cycles", throwfix::get_trigger_count() == k_total_cycles);
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
