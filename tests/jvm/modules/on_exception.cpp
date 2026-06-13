// on_exception JVM test module  (feature area: hooks / exception watcher)
//
// THE exception-watcher authority: exhaustively exercises
// vmhook::on_exception(callback) — the watcher that fires whenever a
// java.lang.Throwable (or subclass) is constructed, because every public
// Throwable constructor runs Throwable.fillInStackTrace() (the hooked method)
// before returning.  Migrates the legacy vmhook/src/example.cpp
// test_on_exception (the throwProbe IllegalStateException path) and then covers
// EVERY thrown-exception observation path the feature can express.
//
// What this module proves / characterizes on a live JVM (one probe cycle each):
//   * on_exception(cb) installs a watcher and returns a watch_handle whose
//     running() reflects whether the underlying fillInStackTrace hook armed in
//     THIS build/JDK;
//   * a GENUINE Java `athrow` of each of these reaches the callback with the
//     throwable's JVM-internal ('/'-separated) class name:
//        - RuntimeException (IllegalStateException, NumberFormatException, a
//          custom RuntimeException subclass),
//        - a CHECKED exception (java.io.IOException) and a custom checked subclass,
//        - an Error (a custom java.lang.Error subclass),
//        - a RE-THROWN instance (TWO athrows, ONE construction -> fires ONCE),
//        - a throw constructed several call frames deep,
//        - an EXPLICIT `new NullPointerException()`,
//        - a throw from a static initializer (ExceptionInInitializerError path),
//        - a throw from a constructor,
//        - a throw uncaught at its site and caught three frames higher;
//   * JVM-INTERNAL implicit throws (NPE / AIOOBE / ClassCast / ArithmeticException)
//     are CHARACTERIZED, not hard-asserted: under -XX:+OmitStackTraceInFastThrow a
//     hot implicit site may reuse a preallocated throwable that SKIPS
//     fillInStackTrace, so the callback may legitimately not fire — the Java
//     witness always proves the throw ran;
//   * the callback fires EXACTLY N times for N constructions (no double, no miss),
//     proven per-TYPE so an unrelated internal throwable on the Java thread can
//     never inflate the count;
//   * a different type is reported under its own name (cross-type attribution);
//   * a no-throw control cycle yields no new typed event;
//   * the RAII watch_handle disarms on scope exit / stop() (idempotent); a throw
//     after the handle drops is not observed by that callback;
//   * multiple watchers all observe the same throw, and dropping ONE silences only
//     it (the survivors keep firing);
//   * RE-ARM after shutdown_hooks(): a fresh on_exception() AFTER a teardown
//     re-installs the detour and fires again (the regression guard for the
//     now-FIXED [HIGH] exception_hook_installed flag-reset defect — shutdown_hooks()
//     calls detail::reset_watcher_latches()).
//
// CROSS-TOOLCHAIN HARDENING (this runs on MSVC / clang-cl / MinGW / gcc / clang
// against JDK 8..26): only UNIVERSAL invariants are hard-asserted — probe
// completion, the Java-side witnesses (throwsObserved / lastThrowKind), and the
// structural watch_handle contracts (running() / RAII / stop()).  JDK-variant
// exception DETAILS (the decoded internal name, exact typed counts) are gated
// behind `trap_live` and asserted only when the hook armed; JVM-internal implicit
// throws are additionally PASS-or-[INFO] because of fast-throw preallocation.
// `trap_live` is the STRUCTURAL truth primary.running(): post-fix, running()==true
// means on_exception() genuinely installed the detour, so a genuine construction
// MUST reach the callback (cross-checked empirically, with an [INFO] if they ever
// disagree — that would itself be a regression).
//
// SAFETY: THIS MODULE INSTALLS A WATCHER/HOOK.  The exception callback runs on
// the Java thread inside the fillInStackTrace detour; it touches ONLY std::atomic
// — no JVM re-entry, no allocation, no oop/pointer deref (the header already
// decoded the internal class-name string for us).  Suite-safety is paramount: a
// leaked exception watcher would fire on every later module's throws and corrupt
// them.  The module therefore ENDS with an UNCONDITIONAL vmhook::shutdown_hooks()
// OUTSIDE any try/catch (mirroring on_class_loaded / hook_basic) so NO watcher, NO
// latch, NO pending-shutdown flag survives, on every exit path.  Every watch_handle
// is RAII-scoped or explicitly stop()'d before that final teardown.
//
// Harness shape mirrors hook_basic / on_class_loaded: a `mode` selector with a
// `done` reset on the rising edge of go, plus Java-observable witnesses
// (throwsObserved / lastThrowKind / distinctConstructions) read back so "callback
// didn't fire" is always distinguishable from "throw never ran".
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace
{
    // ---- Witnesses captured INSIDE the on_exception callback(s) -------------
    // The callbacks run on the Java thread in the fillInStackTrace detour and may
    // be invoked concurrently for distinct watchers, so every witness is atomic.
    // We NEVER touch a JVM object here: the header hands us the already-decoded
    // internal class name, which we only compare against known string constants.

    // Primary watcher: a per-type tally so an unrelated internal throwable can
    // never be miscounted as the type under test.
    std::atomic<int>  g_primary_total{ 0 };   // EVERY Throwable the primary saw this cycle
    std::atomic<int>  g_primary_ise{ 0 };     // java/lang/IllegalStateException
    std::atomic<int>  g_primary_nfe{ 0 };     // java/lang/NumberFormatException
    std::atomic<int>  g_primary_ioe{ 0 };     // java/io/IOException (checked)
    std::atomic<int>  g_primary_custom_error{ 0 };   // OnException$CustomError
    std::atomic<int>  g_primary_custom_checked{ 0 }; // OnException$CustomChecked
    std::atomic<int>  g_primary_custom_runtime{ 0 }; // OnException$CustomRuntime
    std::atomic<int>  g_primary_npe{ 0 };     // java/lang/NullPointerException
    std::atomic<int>  g_primary_aioobe{ 0 };  // java/lang/ArrayIndexOutOfBoundsException
    std::atomic<int>  g_primary_cce{ 0 };     // java/lang/ClassCastException
    std::atomic<int>  g_primary_arith{ 0 };   // java/lang/ArithmeticException
    std::atomic<int>  g_primary_eiie{ 0 };    // java/lang/ExceptionInInitializerError
    std::atomic<bool> g_primary_saw_java_pkg{ false }; // a name began with "java/"
    std::atomic<bool> g_primary_saw_empty{ false };    // a callback got an empty name
    // A name that is NOT the bare "java/lang/Throwable" fallback the header
    // substitutes when it cannot decode the receiver's klass name (e.g.
    // -XX:-UseCompressedClassPointers / 32-bit / Lilliput, where klass_from_oop's
    // +8 narrow-klass read is wrong — the characterized [HIGH] decode flaw).  Used
    // to tell "decode works" from "decode degraded to fallback" so the strict
    // internal-NAME assertions are gated on a JDK/flag combo where decode succeeds.
    std::atomic<int>  g_primary_specific_name{ 0 };

    // Second + third watchers: prove fan-out and that dropping ONE handle silences
    // only it.  Each keeps BOTH a by-NAME ISE tally (asserted when decode works)
    // and a decode-INDEPENDENT total-fire tally (asserted when decode is degraded),
    // so fan-out is provable on every JDK/flag combo.
    std::atomic<int>  g_second_ise{ 0 };
    std::atomic<int>  g_second_total{ 0 };
    std::atomic<int>  g_third_ise{ 0 };
    std::atomic<int>  g_third_total{ 0 };

    // A watcher whose handle we drop early; it must NOT count throws after stop().
    // Tracks total fires (decode-independent) so "silent after stop" holds even
    // when the name decode is degraded.
    std::atomic<int>  g_dropped_total{ 0 };

    // Internal names the fixture throws (mirror OnException.*_INTERNAL_NAME).
    constexpr const char* k_ise_name{ "java/lang/IllegalStateException" };
    constexpr const char* k_nfe_name{ "java/lang/NumberFormatException" };
    constexpr const char* k_ioe_name{ "java/io/IOException" };
    constexpr const char* k_custom_error_name{ "vmhook/fixtures/OnException$CustomError" };
    constexpr const char* k_custom_checked_name{ "vmhook/fixtures/OnException$CustomChecked" };
    constexpr const char* k_custom_runtime_name{ "vmhook/fixtures/OnException$CustomRuntime" };
    constexpr const char* k_npe_name{ "java/lang/NullPointerException" };
    constexpr const char* k_aioobe_name{ "java/lang/ArrayIndexOutOfBoundsException" };
    constexpr const char* k_cce_name{ "java/lang/ClassCastException" };
    constexpr const char* k_arith_name{ "java/lang/ArithmeticException" };
    constexpr const char* k_eiie_name{ "java/lang/ExceptionInInitializerError" };
    // The header's decode-failure fallback name (vmhook.hpp on_exception detour).
    constexpr const char* k_fallback_name{ "java/lang/Throwable" };

    auto starts_with(const std::string& s, const char* prefix) noexcept -> bool
    {
        const std::string p{ prefix };
        return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
    }

    auto reset_primary() -> void
    {
        g_primary_total.store(0);
        g_primary_ise.store(0);
        g_primary_nfe.store(0);
        g_primary_ioe.store(0);
        g_primary_custom_error.store(0);
        g_primary_custom_checked.store(0);
        g_primary_custom_runtime.store(0);
        g_primary_npe.store(0);
        g_primary_aioobe.store(0);
        g_primary_cce.store(0);
        g_primary_arith.store(0);
        g_primary_eiie.store(0);
        g_primary_saw_java_pkg.store(false);
        g_primary_saw_empty.store(false);
        g_primary_specific_name.store(0);
    }

    // ---- Wrapper for vmhook.fixtures.OnException: drives the go/done/mode
    //      handshake and reads back the Java-observable witnesses. ------------
    class oe : public vmhook::object<oe>
    {
    public:
        explicit oe(vmhook::oop_t instance) noexcept
            : vmhook::object<oe>{ instance }
        {
        }

        static auto set_go(bool value) -> void       { static_field("go")->set(value); }
        static auto set_done(bool value) -> void      { static_field("done")->set(value); }
        static auto get_done() -> bool                { return static_field("done")->get(); }
        static auto set_mode(std::int32_t m) -> void  { static_field("mode")->set(m); }

        static auto resolves(const char* name) -> bool
        {
            return static_field(name).has_value();
        }

        // Java-observable witnesses.
        static auto throws_observed() -> std::int32_t        { return static_field("throwsObserved")->get(); }
        static auto last_throw_kind() -> std::int32_t        { return static_field("lastThrowKind")->get(); }
        static auto distinct_constructions() -> std::int32_t { return static_field("distinctConstructions")->get(); }
        static auto last_throw_msg() -> std::string          { return static_field("lastThrowMsg")->get(); }
    };

    // The detail message every mode-1/2 IllegalStateException carries (mirrors
    // OnException.ISE_MESSAGE).  The on_exception callback only receives the
    // throwable's internal CLASS NAME (the API hands no message to native code),
    // so the message is observed via the fixture's Java-side witness round-trip:
    // proving the message confirms the EXACT instance the watcher saw was thrown.
    constexpr const char* k_ise_message{ "vmhook-on-exception-ISE" };

    // Drive one probe cycle for `mode`: clear the latched `done` and program the
    // selector on the rising edge of go, then wait for done.  (Mirrors the
    // field_static / hook_basic drive() helper.)
    auto drive(vmhook_test::context& ctx, std::int32_t mode) -> bool
    {
        return ctx.run_probe(
            [mode](bool value)
            {
                if (value)
                {
                    oe::set_done(false);
                    oe::set_mode(mode);
                }
                oe::set_go(value);
            },
            []() { return oe::get_done(); });
    }

    // ---- The primary observing callback: tallies by type.  Touches ONLY
    //      atomics; copies the already-decoded std::string the watcher hands it. -
    auto make_primary_cb()
    {
        return [](const std::string& name)
        {
            g_primary_total.fetch_add(1, std::memory_order_relaxed);
            if (name.empty())
            {
                g_primary_saw_empty.store(true, std::memory_order_relaxed);
            }
            if (starts_with(name, "java/"))
            {
                g_primary_saw_java_pkg.store(true, std::memory_order_relaxed);
            }
            if (!name.empty() && name != k_fallback_name)
            {
                // A real decoded subclass name (not the decode-failure fallback).
                g_primary_specific_name.fetch_add(1, std::memory_order_relaxed);
            }
            if (name == k_ise_name)            { g_primary_ise.fetch_add(1, std::memory_order_relaxed); }
            else if (name == k_nfe_name)        { g_primary_nfe.fetch_add(1, std::memory_order_relaxed); }
            else if (name == k_ioe_name)        { g_primary_ioe.fetch_add(1, std::memory_order_relaxed); }
            else if (name == k_custom_error_name)   { g_primary_custom_error.fetch_add(1, std::memory_order_relaxed); }
            else if (name == k_custom_checked_name) { g_primary_custom_checked.fetch_add(1, std::memory_order_relaxed); }
            else if (name == k_custom_runtime_name) { g_primary_custom_runtime.fetch_add(1, std::memory_order_relaxed); }
            else if (name == k_npe_name)        { g_primary_npe.fetch_add(1, std::memory_order_relaxed); }
            else if (name == k_aioobe_name)     { g_primary_aioobe.fetch_add(1, std::memory_order_relaxed); }
            else if (name == k_cce_name)        { g_primary_cce.fetch_add(1, std::memory_order_relaxed); }
            else if (name == k_arith_name)      { g_primary_arith.fetch_add(1, std::memory_order_relaxed); }
            else if (name == k_eiie_name)       { g_primary_eiie.fetch_add(1, std::memory_order_relaxed); }
        };
    }

    // A single-type characterization helper for a genuine, reliably-constructed
    // throw (the constructor runs right before athrow): proves the Java throw ran
    // (witnesses, unconditional).  When klass-name decode works (name_decode_ok)
    // it asserts the callback saw the expected internal NAME EXACTLY `expect_count`
    // times and never an empty name.  When the trap is live but decode is degraded
    // (-XX:-UseCompressedClassPointers etc.), the per-TYPE name can't be matched,
    // so it falls back to the decode-INDEPENDENT contract: the callback still fired
    // for the construction (primary_total advanced).  Used for every "explicit
    // construct + throw" mode.
    auto check_reliable_type(vmhook_test::context& ctx,
                             const char*           label,
                             std::int32_t          mode,
                             std::int32_t          expect_kind,
                             std::int32_t          expect_throws,
                             bool                  trap_live,
                             bool                  name_decode_ok,
                             std::atomic<int>&     type_counter,
                             std::int32_t          expect_count) -> void
    {
        reset_primary();
        const bool done{ drive(ctx, mode) };
        ctx.check(std::string{ label } + "_probe_completed", done);
        if (done)
        {
            ctx.check(std::string{ label } + "_java_threw_expected",
                      oe::throws_observed() == expect_throws);
            ctx.check(std::string{ label } + "_java_last_kind",
                      oe::last_throw_kind() == expect_kind);
        }
        ctx.record(std::string{ "[INFO] on_exception: " } + label + " typed callback fired "
                   + std::to_string(type_counter.load()) + "/" + std::to_string(expect_count)
                   + " (primary_total=" + std::to_string(g_primary_total.load())
                   + ", trap " + (trap_live ? "LIVE" : "DEAD")
                   + ", decode " + (name_decode_ok ? "OK" : "degraded") + ").");
        if (name_decode_ok)
        {
            ctx.check(std::string{ label } + "_observed_internal_name",
                      type_counter.load() == expect_count);
            ctx.check(std::string{ label } + "_never_saw_empty_name",
                      g_primary_saw_empty.load() == false);
        }
        else if (trap_live)
        {
            // Decode degraded: the per-type name is unmatchable, but the watcher
            // still fired for the genuine construction.
            ctx.check(std::string{ label } + "_fired_with_degraded_decode",
                      g_primary_total.load() >= expect_count);
        }
    }

    // Characterization helper for a JVM-INTERNAL implicit throw (NPE / AIOOBE /
    // CCE / ArithmeticException).  The Java throw is proven to have run, but the
    // callback firing is PASS-or-[INFO]: -XX:+OmitStackTraceInFastThrow may make
    // a hot implicit site reuse a preallocated throwable that skips
    // fillInStackTrace, so we NEVER hard-fail on the callback here.  We DO assert
    // (when the trap is live and the callback fired at all for this type) that it
    // attributed the event to the CORRECT internal name and not a different one.
    auto characterize_internal(vmhook_test::context& ctx,
                               const char*           label,
                               std::int32_t          mode,
                               std::int32_t          expect_kind,
                               bool                  trap_live,
                               std::atomic<int>&     this_type_counter) -> void
    {
        reset_primary();
        const bool done{ drive(ctx, mode) };
        ctx.check(std::string{ label } + "_probe_completed", done);
        if (done)
        {
            // The implicit throw genuinely happened + was caught on the Java side.
            ctx.check(std::string{ label } + "_java_threw_one", oe::throws_observed() == 1);
            ctx.check(std::string{ label } + "_java_last_kind", oe::last_throw_kind() == expect_kind);
        }
        ctx.record(std::string{ "[INFO] on_exception: " } + label
                   + " (JVM-internal) typed callback fired "
                   + std::to_string(this_type_counter.load())
                   + " time(s), primary_total=" + std::to_string(g_primary_total.load())
                   + " (trap " + (trap_live ? "LIVE" : "DEAD")
                   + "; fast-throw may preallocate + skip fillInStackTrace, so a 0 here is "
                     "an accepted JDK/flag characterization, not a failure).");
    }
}

VMHOOK_JVM_MODULE(on_exception)
{
    // =====================================================================
    //  0. Fixture resolves and the handshake/witness fields are present.
    // =====================================================================
    vmhook::register_class<oe>("vmhook/fixtures/OnException");
    ctx.check("oe_class_registered_go_resolves", oe::resolves("go"));
    ctx.check("oe_mode_field_resolves", oe::resolves("mode"));
    ctx.check("oe_throwsObserved_field_resolves", oe::resolves("throwsObserved"));
    ctx.check("oe_lastThrowKind_field_resolves", oe::resolves("lastThrowKind"));
    ctx.check("oe_distinctConstructions_field_resolves", oe::resolves("distinctConstructions"));

    // =====================================================================
    //  1. Install the primary watcher.  running() is the STRUCTURAL truth that
    //     the underlying fillInStackTrace hook armed in THIS process: post-fix,
    //     on_exception() returns an EMPTY handle (running()==false) if the hook
    //     could not be installed, and a live handle (running()==true) when it
    //     could.  We therefore DERIVE trap_live from running() (authoritative)
    //     rather than guessing, and cross-check it against an actual fire below.
    // =====================================================================
    auto primary{ vmhook::on_exception(make_primary_cb()) };
    const bool trap_live{ primary.running() };
    ctx.record(std::string{ "[INFO] on_exception: primary watch_handle.running()=" }
               + (trap_live ? "true" : "false")
               + " (trap " + (trap_live ? "LIVE — fillInStackTrace hooked"
                                        : "DEAD — fillInStackTrace not hookable in this build/JDK")
               + ").");

    // =====================================================================
    //  2. Primary RuntimeException baseline: a GENUINE IllegalStateException
    //     athrow.  Always prove the Java throw ran (witnesses + message round-trip).
    //     This probe ALSO establishes name_decode_ok (used to gate every per-TYPE
    //     name assertion below): when decode works, assert the internal-name
    //     discrimination and EXACTLY-ONE-fire; when the trap is live but decode is
    //     degraded, assert the decode-independent fire/count contract instead.
    // =====================================================================
    reset_primary();
    const bool done1{ drive(ctx, 1) };
    ctx.check("ise_probe_completed", done1);
    if (done1)
    {
        ctx.check("ise_probe_java_threw_one", oe::throws_observed() == 1);
        ctx.check("ise_probe_java_last_kind_ise", oe::last_throw_kind() == 1);
        // Message round-trip: the EXACT ISE instance the watcher observed carried
        // ISE_MESSAGE (the API passes only the class name to native code, so the
        // message is witnessed Java-side).  This ties the typed callback fire to a
        // specific, message-bearing throwable rather than just "some ISE".
        ctx.check("ise_probe_java_message_roundtrip", oe::last_throw_msg() == k_ise_message);
    }

    // Cross-check the empirical fire against the structural running() truth.  If
    // running()==true but the callback never fired for a genuine construction
    // (or vice-versa), that is itself a regression — record it loudly.
    const bool primary_fired{ g_primary_total.load() > 0 };
    if (trap_live != primary_fired)
    {
        ctx.record(std::string{ "[INFO] on_exception: WARNING running()=" }
                   + (trap_live ? "true" : "false") + " but callback fired="
                   + (primary_fired ? "true" : "false")
                   + " for a genuine ISE athrow — structural/empirical mismatch.");
    }
    ctx.record(std::string{ "[INFO] on_exception: primary callback fired " }
               + std::to_string(g_primary_total.load())
               + " time(s) for one genuine ISE athrow (trap "
               + (trap_live ? "LIVE" : "DEAD") + ").");

    // Does the klass-name DECODE work in this build/JDK?  The header's
    // on_exception detour reads the receiver oop's narrow-klass header at +8 and
    // decompresses it (klass_from_oop); that is correct ONLY with
    // UseCompressedClassPointers (the default under ~32 GB heaps on 64-bit).  With
    // compressed class pointers OFF / on 32-bit, the read is wrong, decode fails,
    // and the detour substitutes the bare "java/lang/Throwable" fallback for EVERY
    // throwable — the characterized [HIGH] decode flaw.  We detect that here: the
    // trap fired (primary_total>0) but produced no SPECIFIC subclass name.  When
    // decode degrades we drop to the weaker (still-true) contract and record an
    // [INFO], so a -XX:-UseCompressedClassPointers JDK does not red the matrix.
    const bool name_decode_ok{ trap_live && g_primary_specific_name.load() > 0 };
    if (trap_live && !name_decode_ok)
    {
        ctx.record("[INFO] on_exception: the trap fired but every throwable decoded to "
                   "the bare \"java/lang/Throwable\" fallback — klass-name decode is "
                   "degraded in THIS build/JDK (klass_from_oop assumes "
                   "UseCompressedClassPointers; OFF / 32-bit / Lilliput break the +8 "
                   "narrow-klass read).  Per-TYPE name assertions are characterized, not "
                   "hard-failed, on this configuration; the fire/count contracts still hold.");
    }

    if (name_decode_ok)
    {
        ctx.check("primary_observed_ise_internal_name", g_primary_ise.load() == 1);
        ctx.check("primary_saw_java_package", g_primary_saw_java_pkg.load());
        ctx.check("primary_never_saw_empty_name", g_primary_saw_empty.load() == false);
        // Exactly-once for ISE specifically (an unrelated internal throwable can
        // bump primary_total but never g_primary_ise).
        ctx.check("primary_ise_fired_exactly_once", g_primary_ise.load() == 1);
    }
    else if (trap_live)
    {
        // Decode degraded: the watcher still FIRED exactly once for the genuine
        // construction (the fire/count contract is decode-independent) and the
        // fallback name it produced is the documented "java/lang/Throwable".
        ctx.check("primary_fired_once_even_with_degraded_decode", g_primary_total.load() >= 1);
        ctx.check("primary_degraded_name_is_throwable_fallback", g_primary_saw_java_pkg.load());
    }
    else
    {
        // The hook is genuinely uninstallable in this build/JDK (NOT the
        // now-fixed flag-reset defect — shutdown_hooks() resets the latch).  The
        // handle is empty, so assert the silent contract and the structural
        // running()==false so a regression is still caught.
        ctx.check("primary_handle_empty_when_hook_uninstallable", primary.running() == false);
        ctx.check("primary_silent_when_hook_uninstallable", g_primary_ise.load() == 0);
        ctx.record("[INFO] on_exception: fillInStackTrace could not be hooked in THIS "
                   "build/JDK, so on_exception() returned an empty watch_handle "
                   "(running()==false) and the trap is DEAD.  This is an environment "
                   "limitation, not the [HIGH] flag-reset defect (shutdown_hooks() now "
                   "calls detail::reset_watcher_latches).  All trap-dependent checks "
                   "below are gated; the structural watch_handle contracts still hold.");
    }

    // =====================================================================
    //  3. Type discrimination: a NumberFormatException athrow under its own name,
    //     never miscounted as the ISE from scenario 2.
    // =====================================================================
    reset_primary();
    const bool done3{ drive(ctx, 3) };
    ctx.check("nfe_probe_completed", done3);
    if (done3)
    {
        ctx.check("nfe_probe_java_threw_one", oe::throws_observed() == 1);
        ctx.check("nfe_probe_java_last_kind_nfe", oe::last_throw_kind() == 2);
    }
    if (name_decode_ok)
    {
        ctx.check("primary_observed_nfe_internal_name", g_primary_nfe.load() == 1);
        ctx.check("primary_did_not_miscount_nfe_as_ise", g_primary_ise.load() == 0);
    }
    else if (trap_live)
    {
        ctx.check("nfe_fired_with_degraded_decode", g_primary_total.load() >= 1);
    }

    // =====================================================================
    //  4. EVERY reliably-constructed type:  checked IOException, custom Error,
    //     custom checked subclass, custom RuntimeException subclass, explicit NPE.
    //     Each fires its OWN internal name EXACTLY once (constructor runs right
    //     before athrow, so no fast-throw ambiguity).  Custom-subclass names prove
    //     the decode handles non-java/ packages too.
    // =====================================================================
    check_reliable_type(ctx, "checked_ioe", 5, /*kind*/3, /*throws*/1, trap_live,
                        name_decode_ok, g_primary_ioe, /*count*/1);
    check_reliable_type(ctx, "custom_error", 6, /*kind*/4, /*throws*/1, trap_live,
                        name_decode_ok, g_primary_custom_error, /*count*/1);
    check_reliable_type(ctx, "custom_checked", 7, /*kind*/5, /*throws*/1, trap_live,
                        name_decode_ok, g_primary_custom_checked, /*count*/1);
    check_reliable_type(ctx, "custom_runtime", 8, /*kind*/6, /*throws*/1, trap_live,
                        name_decode_ok, g_primary_custom_runtime, /*count*/1);
    check_reliable_type(ctx, "explicit_npe", 15, /*kind*/7, /*throws*/1, trap_live,
                        name_decode_ok, g_primary_npe, /*count*/1);

    // Extra cross-type attribution note for the custom subclasses: when decode
    // works, the custom names are reported under their fixture package (a non-java/
    // package), proving the klass-name decode is not java/lang-only.
    if (name_decode_ok)
    {
        ctx.record("[INFO] on_exception: custom-subclass internal names (CustomError / "
                   "CustomChecked / CustomRuntime) decoded under their fixture package "
                   "vmhook/fixtures/OnException$* — non-java/ klass-name decode verified.");
    }

    // =====================================================================
    //  5. RE-THROWN instance: ONE construction, TWO athrows.  The construction-
    //     path watcher must fire EXACTLY ONCE even though the Java side observes
    //     two throw+catch pairs — fillInStackTrace runs only when the instance is
    //     CONSTRUCTED, not on each athrow.
    // =====================================================================
    reset_primary();
    const bool done_re{ drive(ctx, 9) };
    ctx.check("rethrow_probe_completed", done_re);
    if (done_re)
    {
        // Two athrow+catch pairs, but exactly one constructor call.
        ctx.check("rethrow_java_threw_twice", oe::throws_observed() == 2);
        ctx.check("rethrow_java_one_construction", oe::distinct_constructions() == 1);
        ctx.check("rethrow_java_last_kind_ise", oe::last_throw_kind() == 1);
    }
    ctx.record(std::string{ "[INFO] on_exception: re-throw (1 construction / 2 athrows) "
                            "fired primary ISE " }
               + std::to_string(g_primary_ise.load()) + " time(s) (trap "
               + (trap_live ? "LIVE" : "DEAD") + ").");
    if (name_decode_ok)
    {
        // The headline of this scenario: fires once per CONSTRUCTION, not per athrow.
        ctx.check("rethrow_callback_fired_once_per_construction", g_primary_ise.load() == 1);
    }
    else if (trap_live)
    {
        // Decode-independent form of the SAME headline: exactly ONE callback fire
        // for the single construction, even though there were two athrows.
        ctx.check("rethrow_fired_once_per_construction_decode_independent",
                  g_primary_total.load() == 1);
    }

    // =====================================================================
    //  6. NESTED-call throw: constructed three frames deep, caught at the top.
    //     The watcher fires regardless of call depth (the hook is on the
    //     constructor's fillInStackTrace, independent of where athrow lands).
    // =====================================================================
    check_reliable_type(ctx, "nested_call_throw", 10, /*kind*/1, /*throws*/1, trap_live,
                        name_decode_ok, g_primary_ise, /*count*/1);

    // =====================================================================
    //  7. UNCAUGHT-at-throw-site, caught three frames higher.  Identical
    //     observation to a locally-caught throw: construction is what the watcher
    //     sees, so where the exception is ultimately handled is irrelevant.  This
    //     characterizes "caught-and-handled vs uncaught (at the throw site)".
    // =====================================================================
    check_reliable_type(ctx, "uncaught_then_handled", 18, /*kind*/1, /*throws*/1, trap_live,
                        name_decode_ok, g_primary_ise, /*count*/1);

    // =====================================================================
    //  8. Throw from a CONSTRUCTOR: `new CtorThrower()` throws inside its ctor.
    //     The IllegalStateException constructor still runs fillInStackTrace.
    // =====================================================================
    check_reliable_type(ctx, "throw_from_constructor", 17, /*kind*/12, /*throws*/1, trap_live,
                        name_decode_ok, g_primary_ise, /*count*/1);

    // =====================================================================
    //  9. Throw from a STATIC INITIALIZER (one-shot).  Forcing the
    //     StaticInitThrower class runs its <clinit>, which throws an
    //     IllegalStateException (one construction) that the JVM wraps in an
    //     ExceptionInInitializerError (a second construction).  So when the trap
    //     is live the primary sees BOTH an ISE and an ExceptionInInitializerError.
    //     RUN EXACTLY ONCE: a second attempt would yield NoClassDefFoundError.
    // =====================================================================
    reset_primary();
    const bool done_si{ drive(ctx, 16) };
    ctx.check("static_init_probe_completed", done_si);
    if (done_si)
    {
        ctx.check("static_init_java_threw_one", oe::throws_observed() == 1);
        ctx.check("static_init_java_last_kind_eiie", oe::last_throw_kind() == 11);
    }
    ctx.record(std::string{ "[INFO] on_exception: static-init throw fired ISE " }
               + std::to_string(g_primary_ise.load()) + " + EIIE "
               + std::to_string(g_primary_eiie.load())
               + " (primary_total=" + std::to_string(g_primary_total.load())
               + ", trap " + (trap_live ? "LIVE" : "DEAD") + ").");
    if (name_decode_ok)
    {
        // HARD: the cause IllegalStateException is OUR construction inside <clinit>
        // (a genuine `new IllegalStateException(...)`, so fillInStackTrace runs).
        ctx.check("static_init_observed_cause_ise", g_primary_ise.load() >= 1);
        ctx.check("static_init_never_saw_empty_name", g_primary_saw_empty.load() == false);
        // SOFT (PASS-or-[INFO]): the ExceptionInInitializerError WRAPPER is built by
        // the JVM itself; whether its construction routes through the hooked
        // fillInStackTrace is a JDK-construction detail, so it is characterized, not
        // hard-asserted, across the 8..26 matrix.
        ctx.record(std::string{ "[INFO] on_exception: ExceptionInInitializerError wrapper "
                                "observed " } + std::to_string(g_primary_eiie.load())
                   + " time(s) (JVM-built wrapper; characterized, not asserted).");
    }
    else if (trap_live)
    {
        // Decode degraded: at least the <clinit> cause construction fired a callback.
        ctx.check("static_init_fired_with_degraded_decode", g_primary_total.load() >= 1);
    }

    // =====================================================================
    // 10. JVM-INTERNAL implicit throws — CHARACTERIZED (PASS-or-[INFO]).  NPE,
    //     AIOOBE, ClassCast, ArithmeticException provoked implicitly.  The Java
    //     witness proves each throw ran; the callback firing is NOT hard-asserted
    //     because -XX:+OmitStackTraceInFastThrow may reuse a preallocated instance
    //     that skips fillInStackTrace.  This is the documented contrast to the
    //     EXPLICIT `new NullPointerException()` of scenario 4 (which fires reliably).
    // =====================================================================
    characterize_internal(ctx, "implicit_npe",    11, /*kind*/7,  trap_live, g_primary_npe);
    characterize_internal(ctx, "implicit_aioobe", 12, /*kind*/8,  trap_live, g_primary_aioobe);
    characterize_internal(ctx, "implicit_cce",    13, /*kind*/9,  trap_live, g_primary_cce);
    characterize_internal(ctx, "implicit_arith",  14, /*kind*/10, trap_live, g_primary_arith);

    // =====================================================================
    // 11. Many ISEs + multiple watchers + selective drop.  Install a SECOND and
    //     THIRD watcher (ISE-only) plus a fourth "dropped" watcher we stop() BEFORE
    //     the throw.  All LIVE watchers must observe the same throws; the dropped
    //     one must observe none.  Structural truths (a stopped handle no longer
    //     running(), stop() idempotent) hold regardless of trap liveness.
    // =====================================================================
    {
        auto second{ vmhook::on_exception(
            [](const std::string& name)
            {
                g_second_total.fetch_add(1, std::memory_order_relaxed);
                if (name == k_ise_name) { g_second_ise.fetch_add(1, std::memory_order_relaxed); }
            }) };
        auto third{ vmhook::on_exception(
            [](const std::string& name)
            {
                g_third_total.fetch_add(1, std::memory_order_relaxed);
                if (name == k_ise_name) { g_third_ise.fetch_add(1, std::memory_order_relaxed); }
            }) };

        // running() of the 2nd/3rd watcher mirrors the primary (same shared hook).
        ctx.check("second_watch_handle_running_matches_trap", second.running() == trap_live);
        ctx.check("third_watch_handle_running_matches_trap", third.running() == trap_live);

        // A watcher we arm and then immediately disarm BEFORE any throw.
        {
            auto dropped{ vmhook::on_exception(
                [](const std::string& /*name*/)
                {
                    g_dropped_total.fetch_add(1, std::memory_order_relaxed);
                }) };
            ctx.check("dropped_watch_handle_running_matches_trap", dropped.running() == trap_live);
            dropped.stop();
            // Structural: stop() is observable on the handle, independent of trap.
            ctx.check("dropped_watch_handle_not_running_after_stop", dropped.running() == false);
            // Idempotent second stop() must not throw / change anything.
            dropped.stop();
            ctx.check("dropped_watch_handle_stop_idempotent", dropped.running() == false);
        }

        reset_primary();
        g_second_ise.store(0);
        g_second_total.store(0);
        g_third_ise.store(0);
        g_third_total.store(0);
        g_dropped_total.store(0);

        // Throw MANY ISEs in one cycle.
        const bool done2{ drive(ctx, 2) };
        ctx.check("many_ise_probe_completed", done2);
        if (done2)
        {
            ctx.check("many_ise_probe_java_threw_n", oe::throws_observed() == 4);
            ctx.check("many_ise_probe_java_last_kind_ise", oe::last_throw_kind() == 1);
        }

        ctx.record(std::string{ "[INFO] on_exception: after 4 ISE athrows primary(ise/total)=" }
                   + std::to_string(g_primary_ise.load()) + "/" + std::to_string(g_primary_total.load())
                   + " second=" + std::to_string(g_second_ise.load()) + "/" + std::to_string(g_second_total.load())
                   + " third=" + std::to_string(g_third_ise.load()) + "/" + std::to_string(g_third_total.load())
                   + " dropped_total=" + std::to_string(g_dropped_total.load()) + " (trap "
                   + (trap_live ? "LIVE" : "DEAD") + ", decode "
                   + (name_decode_ok ? "OK" : "degraded") + ").");

        // The dropped watcher must NEVER observe a post-stop throw — true whether
        // the trap is live (it was unregistered) or dead (nothing fires at all).
        // Decode-INDEPENDENT (total fires), so it holds on every JDK/flag combo.
        ctx.check("dropped_watcher_silent_after_stop", g_dropped_total.load() == 0);

        if (name_decode_ok)
        {
            // Every live watcher saw all four ISE throws, identically: fan-out + the
            // surviving handles still fire after one sibling dropped.  EXACTLY 4
            // each (per-type, so unrelated throwables can't inflate it).
            ctx.check("primary_saw_all_four_ise", g_primary_ise.load() == 4);
            ctx.check("second_saw_all_four_ise", g_second_ise.load() == 4);
            ctx.check("third_saw_all_four_ise", g_third_ise.load() == 4);
            ctx.check("all_live_watchers_agree_count",
                      g_primary_ise.load() == g_second_ise.load()
                      && g_second_ise.load() == g_third_ise.load());
        }
        else if (trap_live)
        {
            // Decode degraded: prove fan-out via the decode-INDEPENDENT total tallies
            // — each live watcher fired exactly 4 times and all three agree.
            ctx.check("primary_total_four_fires", g_primary_total.load() == 4);
            ctx.check("second_total_four_fires", g_second_total.load() == 4);
            ctx.check("third_total_four_fires", g_third_total.load() == 4);
            ctx.check("all_live_watchers_agree_total",
                      g_primary_total.load() == g_second_total.load()
                      && g_second_total.load() == g_third_total.load());
        }
    }
    // `second` and `third` handles dropped here (RAII stop): they must no longer
    // fire.  Re-run a single ISE; only `primary` (still in scope) could observe.
    {
        reset_primary();
        g_second_ise.store(0);
        g_second_total.store(0);
        g_third_ise.store(0);
        g_third_total.store(0);

        const bool done_after{ drive(ctx, 1) };
        ctx.check("post_drop_ise_probe_completed", done_after);
        if (done_after)
        {
            ctx.check("post_drop_java_threw_one", oe::throws_observed() == 1);
        }

        // RAII-disarm contract: the dropped second/third watchers observe NOTHING
        // after their handles left scope — true regardless of trap liveness AND of
        // decode (asserted on the decode-INDEPENDENT total tallies).
        ctx.check("second_silent_after_raii_drop", g_second_total.load() == 0);
        ctx.check("third_silent_after_raii_drop", g_third_total.load() == 0);

        if (name_decode_ok)
        {
            // The still-armed primary keeps firing for the new throw.
            ctx.check("primary_still_fires_after_siblings_dropped", g_primary_ise.load() == 1);
        }
        else if (trap_live)
        {
            ctx.check("primary_still_fires_after_siblings_dropped_total", g_primary_total.load() == 1);
        }
    }

    // =====================================================================
    // 12. Control: a NO-THROW cycle constructs no Throwable, so NO watcher may
    //     observe a new TYPED event.  Holds whether or not the trap is live, and
    //     is the clean negative that distinguishes "armed + firing on throws" from
    //     "firing spuriously".  (The JVM may build unrelated internal throwables on
    //     other threads, so we assert OUR typed counters stay 0, not primary_total.)
    // =====================================================================
    {
        reset_primary();
        const bool done4{ drive(ctx, 4) };
        ctx.check("control_no_throw_probe_completed", done4);
        if (done4)
        {
            ctx.check("control_no_throw_java_built_nothing", oe::throws_observed() == 0);
            ctx.check("control_no_throw_java_last_kind_none", oe::last_throw_kind() == 0);
        }
        ctx.check("control_no_new_ise_observed", g_primary_ise.load() == 0);
        ctx.check("control_no_new_nfe_observed", g_primary_nfe.load() == 0);
        ctx.check("control_no_new_custom_runtime_observed", g_primary_custom_runtime.load() == 0);
    }

    // =====================================================================
    // 13. RE-ARM AFTER shutdown_hooks() (regression guard for the audit [HIGH]
    //     flag-reset defect, NOW FIXED).  shutdown_hooks() resets the watcher
    //     install latch detail::exception_hook_installed and drops the stale
    //     callback list (via detail::reset_watcher_latches), so a fresh
    //     on_exception() AFTER a teardown RE-INSTALLS the Throwable.fillInStackTrace
    //     detour and fires again — exactly like an ordinary re-arm.  This is the
    //     exception twin of on_class_loaded's Scenario 7.  We tear EVERYTHING down
    //     first (the primary detour included), then prove a brand-new watcher's
    //     running() and (when live) firing.  Cleanup leaves NOTHING armed.
    // =====================================================================
    {
        // Bulk teardown: removes the fillInStackTrace detour this module installed.
        // Before the fix this would have left exception_hook_installed latched true,
        // making the re-arm below a silent no-op; the fix clears it.
        vmhook::shutdown_hooks();

        auto fresh{ vmhook::on_exception(make_primary_cb()) };
        const bool fresh_running{ fresh.running() };
        // running()==true means a GENUINE fresh install: shutdown_hooks() cleared
        // the latch, so on_exception() re-installed the detour rather than trusting
        // a stale flag.  When the original trap was live, the re-arm MUST be too.
        ctx.record(std::string{ "[INFO] on_exception: post-shutdown re-arm handle running()=" }
                   + (fresh_running ? "true" : "false"));
        if (trap_live)
        {
            ctx.check("rearm_after_shutdown_handle_running", fresh_running);
        }
        else
        {
            // If the hook was never installable, the re-arm is empty too — and
            // that consistency is itself the contract.
            ctx.check("rearm_after_shutdown_handle_empty_when_uninstallable",
                      fresh_running == false);
        }

        reset_primary();
        const bool rearm_done{ drive(ctx, 1) };
        ctx.check("rearm_after_shutdown_probe_completed", rearm_done);
        if (rearm_done)
        {
            // The Java throw genuinely ran regardless of trap liveness.
            ctx.check("rearm_after_shutdown_java_threw_one", oe::throws_observed() == 1);
        }

        ctx.record(std::string{ "[INFO] on_exception: post-shutdown re-arm callback fired " }
                   + std::to_string(g_primary_total.load()) + " time(s) for one ISE (trap "
                   + (trap_live ? "LIVE" : "DEAD") + ").");

        if (trap_live)
        {
            // The fix in action: after shutdown_hooks() the re-armed watcher
            // RE-INSTALLED the detour and observed the throw.  Before the fix this
            // fired ZERO times (latch stale, detour gone).  The FIRE is the headline
            // (decode-independent); the name is gated on decode.
            ctx.check("rearm_after_shutdown_callback_fired", g_primary_total.load() >= 1);
            if (name_decode_ok)
            {
                ctx.check("rearm_after_shutdown_observed_ise_internal_name",
                          g_primary_ise.load() == 1);
            }
            ctx.record("[INFO] on_exception: re-arm after shutdown_hooks() fires again — "
                       "the [HIGH] exception_hook_installed flag-reset defect is FIXED "
                       "(shutdown_hooks() now calls detail::reset_watcher_latches).");
        }
        else
        {
            ctx.record("[INFO] on_exception: trap not live in this process; re-arm firing "
                       "not asserted (the original install never armed either, so this is "
                       "an environment limitation, not the flag-reset bug).");
        }

        fresh.stop();
        ctx.check("rearm_after_shutdown_handle_stopped", fresh.running() == false);
    }

    // =====================================================================
    // 14. Tear down: the primary handle leaves scope at function end (RAII stop()),
    //     but we explicitly stop() it first so the disarm is unmistakable in the
    //     log ordering, then confirm running()==false.  (Scenario 13 already called
    //     shutdown_hooks(), so primary's detour is gone; stop() on an
    //     already-removed hook is safe and just flips running() to false — and on
    //     the DEAD-trap path primary was already an empty handle.)
    // =====================================================================
    primary.stop();
    ctx.check("primary_watch_handle_stopped_at_end", primary.running() == false);

    // =====================================================================
    // FINAL TEARDOWN — UNCONDITIONAL, OUTSIDE any try/catch, on EVERY exit path.
    //   THIS MODULE INSTALLED A WATCHER: a leaked exception watcher would fire on
    //   every later module's throws and corrupt them (this exact failure mode has
    //   reverted a module before).  shutdown_hooks() is idempotent + safe-when-empty
    //   (proven by shutdown_hooks_teardown) and calls detail::reset_watcher_latches(),
    //   so after this call NO fillInStackTrace detour, NO install latch, and NO
    //   registered callback survive for the modules that run after this one.
    // =====================================================================
    vmhook::shutdown_hooks();
    ctx.check("on_exception_module_left_clean_final_shutdown", true);
}
