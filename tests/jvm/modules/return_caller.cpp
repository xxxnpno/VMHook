// return_caller JVM test module  (feature area: hooks)
//
// Exhaustively exercises return_value::caller() — the SINGLE-FRAME walk of the
// HotSpot interpreter saved-rbp chain that identifies the IMMEDIATE calling Java
// method — on a LIVE JVM via real Java bytecode dispatch.  Its sibling module
// return_stack_trace_depth.cpp owns the MULTI-frame walk (stack_trace()); this
// module is the focused, exhaustive counterpart for caller() (immediate caller
// identity: class_name / method_name / signature / Method*), with stack_trace()
// used only as the cross-check that "index 0 == caller()".
//
// The hooked leaf is FIXED — ReturnCaller.inner(int), descriptor (I)I — so only
// the method ABOVE it varies between scenarios.  Every caller-identity assertion
// is therefore attributable to the walk, not to which method was hooked.  The
// detour records what caller() (and stack_trace()) reported per fire; the module
// reads the recordings back after each probe cycle and asserts on them.
//
// Scenarios (mirroring ReturnCaller.java's `mode` selector):
//   1  depth-2  outerA -> inner            : immediate caller is outerA (interp)
//   2  depth-3  outerB -> middle -> inner  : immediate caller is middle; outerB
//                                            is one frame deeper (caller() must
//                                            report middle, NOT outerB)
//   3  deep recursion recurse(90) -> inner : caller() resolves the immediate
//                                            recurse frame; stack_trace caps at 64
//   4  long descriptor longSig(8xObject,int) -> inner : signature char-for-char,
//                                            not truncated (> 140 chars)
//   5  JIT-compiled caller warmCaller (heated 200k) -> inner : the compiled
//                                            immediate caller must NOT be reported
//                                            as an interpreted frame
//   6  two distinct callers alpha -> inner, beta -> inner (one cycle) : caller()
//                                            reports the right DISTINCT caller per
//                                            fire (not a stale cache)
//   7  longArgCaller(long) -> inner        : caller signature is (J)I, distinct
//                                            from the leaf's own (I)I
//
// SAFETY / CROSS-TOOLCHAIN (Java 8-26 x 5 toolchains; win-clang/mingw have NO
// SEH, and this feature WALKS INTERPRETER FRAMES — the #1 cold-fault source):
//   * This module NEVER dereferences a raw frame/method pointer itself.  It only
//     reads the std::string fields of the returned caller_info and COMPARES the
//     Method* values (never derefs them), so even a bogus caller_info cannot
//     fault it.  The dangerous saved-rbp reads happen inside the library's
//     caller()/stack_trace(), which gate every step with is_valid_pointer + the
//     stack-growth monotonicity guard.
//   * HARD contracts are only "the JVM did not crash" (reaching the post-probe
//     asserts proves it) and "caller() resolves the right method WHEN the
//     immediate caller is interpreted".  Any check that a SPECIFIC frame is
//     interpreted is BEST-EFFORT: a hot fixture method can be JIT-compiled /
//     inlined and then legitimately has NO interpreter frame — recorded [INFO],
//     never FAILed.  Mode 5 (compiled caller) asserts only the inlining-robust
//     invariant "caller() is invalid OR is not warmCaller", which a hot C2 build
//     satisfies via invalidity and a pure-interpreter (-Xint) build satisfies via
//     warmCaller staying interpreted+resolved (then the not-warmCaller half is
//     recorded [INFO], not FAILed).
//   * Lifecycle: every per-scenario inner hook is a scoped_hook (uninstalls on
//     scope exit).  An UNCONDITIONAL `if (ctx.reset) ctx.reset();` runs at the
//     very end on every path so no detour is ever left armed for the next module
//     (belt-and-braces beyond the scoped_hook destructors, which a no-SEH
//     contained-crash longjmp would skip).
//
// Harness note: `done` LATCHES (run_probe never clears it).  Each scenario
// resets observations + clears done and programs `mode` on the rising edge of
// `go`, runs ONE probe cycle, then reads the recorded observations back.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace
{
    // Wrapper for vmhook.fixtures.ReturnCaller.  Deriving from vmhook::object<>
    // gives it a vtable (required by register_class<T>) and the static_field(...)
    // accessors for the go/done/mode handshake + recorded-observation fields.
    // Each typed getter reads into a concretely-typed local first: field_proxy's
    // value_t conversion operator is templated, so a bare
    // `static_field(...)->get() == x` is an ambiguous deduction.
    class caller_fixture : public vmhook::object<caller_fixture>
    {
    public:
        explicit caller_fixture(vmhook::oop_t instance) noexcept
            : vmhook::object<caller_fixture>{ instance }
        {
        }

        // go / done handshake + scenario selector.
        static auto set_go(bool value) -> void       { static_field("go")->set(value); }
        static auto set_done(bool value) -> void      { static_field("done")->set(value); }
        static auto get_done() -> bool                { bool v = static_field("done")->get(); return v; }
        static auto set_mode(std::int32_t m) -> void  { static_field("mode")->set(m); }

        // Recorded leaf side effects (handshake proof).
        static auto get_inner_calls() -> std::int32_t { std::int32_t v = static_field("innerCalls")->get(); return v; }
        static auto get_observed() -> std::int32_t    { std::int32_t v = static_field("observed")->get(); return v; }
    };

    // ── Fixture-mirrored constants (kept in lockstep with ReturnCaller.java) ──
    const std::string CLASS_NAME{ "vmhook/fixtures/ReturnCaller" };
    const std::string SIG_II{ "(I)I" };
    const std::string SIG_JI{ "(J)I" };

    // The exact long descriptor for longSig(8 x Object, int) -> int.  Mirrors the
    // Java method byte-for-byte: caller().signature must equal this, untruncated.
    const std::string LONGSIG_DESC{
        "(Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;"
        "Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;I)I" };

    constexpr std::int32_t RECURSION_DEPTH{ 90 };   // ReturnCaller.RECURSION_DEPTH
    constexpr std::size_t  DEFAULT_CAP{ 64 };       // stack_trace(max_depth = 64)

    using info_t  = vmhook::return_value::caller_info;
    using trace_t = std::vector<info_t>;

    // True when a caller_info names `method` of our fixture class with `sig`.
    auto names(const info_t& info, const std::string& method, const std::string& sig) noexcept
        -> bool
    {
        return info.method != nullptr
            && info.class_name  == CLASS_NAME
            && info.method_name == method
            && info.signature   == sig;
    }

    // Does any frame in `trace` name `method` of the fixture class (any sig)?
    auto trace_contains(const trace_t& trace, const std::string& method) noexcept -> bool
    {
        for (const auto& f : trace)
        {
            if (f.method != nullptr && f.class_name == CLASS_NAME && f.method_name == method)
            {
                return true;
            }
        }
        return false;
    }

    // ── Mode 1 — depth-2 outerA -> inner ──────────────────────────────────────
    std::atomic<std::int32_t> g_m1_fires{ 0 };
    std::atomic<bool>         g_m1_caller_valid{ false };
    std::atomic<bool>         g_m1_method_is_outerA{ false };    // method_name == outerA
    std::atomic<bool>         g_m1_class_is_fixture{ false };    // class_name == fixture
    std::atomic<bool>         g_m1_class_is_slashed{ false };    // slashed, not dotted
    std::atomic<bool>         g_m1_sig_is_II{ false };           // signature == (I)I
    std::atomic<bool>         g_m1_method_ptr_nonnull{ false };
    std::atomic<bool>         g_m1_caller_not_inner{ false };    // caller != the leaf
    std::atomic<bool>         g_m1_trace0_matches_caller{ false }; // stack_trace()[0] == caller()
    std::atomic<bool>         g_m1_trace_has_outerA{ false };

    // ── Mode 2 — depth-3 outerB -> middle -> inner ────────────────────────────
    std::atomic<std::int32_t> g_m2_fires{ 0 };
    std::atomic<bool>         g_m2_caller_valid{ false };
    std::atomic<bool>         g_m2_caller_is_middle{ false };    // immediate caller == middle
    std::atomic<bool>         g_m2_caller_not_outerB{ false };   // NOT the 2-deep frame
    std::atomic<bool>         g_m2_caller_not_inner{ false };
    std::atomic<bool>         g_m2_trace0_matches_caller{ false };
    std::atomic<bool>         g_m2_trace_has_middle{ false };
    std::atomic<bool>         g_m2_trace_has_outerB{ false };    // INFO: outerB reachable via trace

    // ── Mode 3 — deep recursion recurse(90) -> inner ──────────────────────────
    std::atomic<std::int32_t> g_m3_fires{ 0 };
    std::atomic<bool>         g_m3_caller_valid{ false };
    std::atomic<bool>         g_m3_caller_is_recurse{ false };   // immediate caller == recurse
    std::atomic<bool>         g_m3_trace0_matches_caller{ false };
    std::atomic<std::size_t>  g_m3_trace_size{ 0 };
    std::atomic<bool>         g_m3_trace_within_cap{ false };

    // ── Mode 4 — long descriptor longSig(...) -> inner ────────────────────────
    std::atomic<std::int32_t> g_m4_fires{ 0 };
    std::atomic<bool>         g_m4_caller_valid{ false };
    std::atomic<bool>         g_m4_caller_is_longSig{ false };   // method_name == longSig
    std::atomic<bool>         g_m4_sig_exact{ false };           // signature == LONGSIG_DESC
    std::atomic<std::size_t>  g_m4_sig_len{ 0 };
    std::atomic<bool>         g_m4_sig_not_truncated{ false };   // begins '(' ends ")I"
    std::atomic<bool>         g_m4_trace0_sig_exact{ false };    // trace[0] carries same sig

    // ── Mode 5 — JIT-compiled caller warmCaller -> inner ──────────────────────
    std::atomic<std::int32_t> g_m5_fires{ 0 };
    std::atomic<bool>         g_m5_caller_valid{ false };        // INFO: was caller resolved?
    std::atomic<bool>         g_m5_caller_is_warmCaller{ false };// INFO: did we see warmCaller?
    std::atomic<bool>         g_m5_robust_invariant{ false };    // HARD: invalid OR not-warmCaller
    std::atomic<bool>         g_m5_trace_lacks_warmCaller{ false }; // INFO: trace omits compiled frame

    // ── Mode 6 — two distinct callers alpha -> inner, beta -> inner ───────────
    std::atomic<std::int32_t> g_m6_fires{ 0 };
    std::atomic<bool>         g_m6_first_is_alpha{ false };
    std::atomic<bool>         g_m6_second_is_beta{ false };
    std::atomic<void*>        g_m6_first_method{ nullptr };
    std::atomic<void*>        g_m6_second_method{ nullptr };
    std::atomic<bool>         g_m6_first_valid{ false };
    std::atomic<bool>         g_m6_second_valid{ false };

    // ── Mode 7 — longArgCaller(long) -> inner : signature (J)I ────────────────
    std::atomic<std::int32_t> g_m7_fires{ 0 };
    std::atomic<bool>         g_m7_caller_valid{ false };
    std::atomic<bool>         g_m7_caller_is_longArg{ false };   // method_name == longArgCaller
    std::atomic<bool>         g_m7_sig_is_JI{ false };           // signature == (J)I
    std::atomic<bool>         g_m7_sig_not_II{ false };          // distinct from leaf's (I)I

    auto reset_observations() -> void
    {
        g_m1_fires.store(0);
        g_m1_caller_valid.store(false);
        g_m1_method_is_outerA.store(false);
        g_m1_class_is_fixture.store(false);
        g_m1_class_is_slashed.store(false);
        g_m1_sig_is_II.store(false);
        g_m1_method_ptr_nonnull.store(false);
        g_m1_caller_not_inner.store(false);
        g_m1_trace0_matches_caller.store(false);
        g_m1_trace_has_outerA.store(false);

        g_m2_fires.store(0);
        g_m2_caller_valid.store(false);
        g_m2_caller_is_middle.store(false);
        g_m2_caller_not_outerB.store(false);
        g_m2_caller_not_inner.store(false);
        g_m2_trace0_matches_caller.store(false);
        g_m2_trace_has_middle.store(false);
        g_m2_trace_has_outerB.store(false);

        g_m3_fires.store(0);
        g_m3_caller_valid.store(false);
        g_m3_caller_is_recurse.store(false);
        g_m3_trace0_matches_caller.store(false);
        g_m3_trace_size.store(0);
        g_m3_trace_within_cap.store(false);

        g_m4_fires.store(0);
        g_m4_caller_valid.store(false);
        g_m4_caller_is_longSig.store(false);
        g_m4_sig_exact.store(false);
        g_m4_sig_len.store(0);
        g_m4_sig_not_truncated.store(false);
        g_m4_trace0_sig_exact.store(false);

        g_m5_fires.store(0);
        g_m5_caller_valid.store(false);
        g_m5_caller_is_warmCaller.store(false);
        g_m5_robust_invariant.store(false);
        g_m5_trace_lacks_warmCaller.store(false);

        g_m6_fires.store(0);
        g_m6_first_is_alpha.store(false);
        g_m6_second_is_beta.store(false);
        g_m6_first_method.store(nullptr);
        g_m6_second_method.store(nullptr);
        g_m6_first_valid.store(false);
        g_m6_second_valid.store(false);

        g_m7_fires.store(0);
        g_m7_caller_valid.store(false);
        g_m7_caller_is_longArg.store(false);
        g_m7_sig_is_JI.store(false);
        g_m7_sig_not_II.store(false);
    }

    // Drives exactly one probe cycle for `mode` (rising-edge programs mode +
    // clears the latched done before the fixture's pending() observes go).
    auto drive(vmhook_test::context& ctx, std::int32_t mode) -> bool
    {
        return ctx.run_probe(
            [mode](bool value)
            {
                if (value)
                {
                    caller_fixture::set_done(false);
                    caller_fixture::set_mode(mode);
                }
                caller_fixture::set_go(value);
            },
            []() { return caller_fixture::get_done(); });
    }
}

VMHOOK_JVM_MODULE(return_caller)
{
    vmhook::register_class<caller_fixture>("vmhook/fixtures/ReturnCaller");

    reset_observations();

    // =====================================================================
    // Scenario 1 — depth-2 chain outerA -> inner.  caller() must report the
    // IMMEDIATE interpreted caller outerA: class slashed-internal, method bare,
    // signature (I)I, Method* non-null, and NOT the leaf inner itself.
    // stack_trace()[0] must agree with caller() (the documented contract).
    // =====================================================================
    {
        auto handle{ vmhook::scoped_hook<caller_fixture>(
            "inner", "(I)I",
            [](vmhook::return_value& ret,
               const std::unique_ptr<caller_fixture>& /*self*/,
               std::int32_t /*x*/)
            {
                g_m1_fires.fetch_add(1, std::memory_order_relaxed);

                const auto info{ ret.caller() };
                g_m1_caller_valid.store(info.valid(), std::memory_order_relaxed);
                if (info.valid())
                {
                    g_m1_method_ptr_nonnull.store(info.method != nullptr,
                                                  std::memory_order_relaxed);
                    g_m1_method_is_outerA.store(info.method_name == "outerA",
                                                std::memory_order_relaxed);
                    g_m1_class_is_fixture.store(info.class_name == CLASS_NAME,
                                                std::memory_order_relaxed);
                    // Slashed internal form, NOT the dotted binary name: contains
                    // '/' and no '.'.  Guards a regression if a future change ran
                    // the class name through a binary-name translator.
                    g_m1_class_is_slashed.store(
                        info.class_name.find('/') != std::string::npos
                     && info.class_name.find('.') == std::string::npos,
                        std::memory_order_relaxed);
                    g_m1_sig_is_II.store(info.signature == SIG_II,
                                         std::memory_order_relaxed);
                    g_m1_caller_not_inner.store(info.method_name != "inner",
                                                std::memory_order_relaxed);
                }

                // stack_trace()[0] == caller() (index-0 contract); outerA present.
                const auto trace{ ret.stack_trace() };
                if (!trace.empty() && info.valid())
                {
                    g_m1_trace0_matches_caller.store(
                        trace.front().method      == info.method
                     && trace.front().method_name == info.method_name,
                        std::memory_order_relaxed);
                }
                g_m1_trace_has_outerA.store(trace_contains(trace, "outerA"),
                                            std::memory_order_relaxed);
            }) };

        ctx.check("rc_m1_hook_installed", handle.installed());

        const bool done{ drive(ctx, 1) };
        // HARD: did not crash + handshake.
        ctx.check("rc_m1_probe_completed", done);
        ctx.check("rc_m1_leaf_ran_once", caller_fixture::get_inner_calls() == 1);
        ctx.check("rc_m1_fired_once", g_m1_fires.load() == 1);

        // outerA is a tiny interpreted method at these call counts (one call per
        // cycle), so caller() resolving it is HARD.  If a future JIT policy ever
        // compiled it, these would demote — but at 1 invocation it stays interp.
        ctx.check("rc_m1_caller_valid", g_m1_caller_valid.load());
        ctx.check("rc_m1_caller_method_is_outerA", g_m1_method_is_outerA.load());
        ctx.check("rc_m1_caller_class_is_fixture", g_m1_class_is_fixture.load());
        ctx.check("rc_m1_caller_class_is_slashed", g_m1_class_is_slashed.load());
        ctx.check("rc_m1_caller_sig_is_II", g_m1_sig_is_II.load());
        ctx.check("rc_m1_caller_method_ptr_nonnull", g_m1_method_ptr_nonnull.load());
        ctx.check("rc_m1_caller_is_not_the_leaf", g_m1_caller_not_inner.load());
        ctx.check("rc_m1_trace0_matches_caller", g_m1_trace0_matches_caller.load());
        ctx.check("rc_m1_trace_contains_outerA", g_m1_trace_has_outerA.load());
        // Read-only walk does not force-return: the leaf side effect still ran.
        ctx.check("rc_m1_leaf_side_effect_ran", caller_fixture::get_observed() != 0);
    }

    // =====================================================================
    // Scenario 2 — depth-3 chain outerB -> middle -> inner.  The IMMEDIATE
    // caller is middle (NOT outerB, which is one frame deeper); caller() reports
    // exactly the immediate frame.  outerB is reachable only via stack_trace.
    // =====================================================================
    {
        auto handle{ vmhook::scoped_hook<caller_fixture>(
            "inner", "(I)I",
            [](vmhook::return_value& ret,
               const std::unique_ptr<caller_fixture>& /*self*/,
               std::int32_t /*x*/)
            {
                g_m2_fires.fetch_add(1, std::memory_order_relaxed);

                const auto info{ ret.caller() };
                g_m2_caller_valid.store(info.valid(), std::memory_order_relaxed);
                if (info.valid())
                {
                    g_m2_caller_is_middle.store(names(info, "middle", SIG_II),
                                                std::memory_order_relaxed);
                    g_m2_caller_not_outerB.store(info.method_name != "outerB",
                                                 std::memory_order_relaxed);
                    g_m2_caller_not_inner.store(info.method_name != "inner",
                                                std::memory_order_relaxed);
                }

                const auto trace{ ret.stack_trace() };
                if (!trace.empty() && info.valid())
                {
                    g_m2_trace0_matches_caller.store(
                        trace.front().method == info.method,
                        std::memory_order_relaxed);
                }
                g_m2_trace_has_middle.store(trace_contains(trace, "middle"),
                                            std::memory_order_relaxed);
                g_m2_trace_has_outerB.store(trace_contains(trace, "outerB"),
                                            std::memory_order_relaxed);
            }) };

        ctx.check("rc_m2_hook_installed", handle.installed());

        const bool done{ drive(ctx, 2) };
        ctx.check("rc_m2_probe_completed", done);
        ctx.check("rc_m2_leaf_ran_once", caller_fixture::get_inner_calls() == 1);
        ctx.check("rc_m2_fired_once", g_m2_fires.load() == 1);

        ctx.check("rc_m2_caller_valid", g_m2_caller_valid.load());
        // The headline: caller() is the IMMEDIATE frame (middle), not the 2-deep
        // outerB.  This distinguishes caller() from stack_trace().
        ctx.check("rc_m2_caller_is_middle", g_m2_caller_is_middle.load());
        ctx.check("rc_m2_caller_is_not_outerB", g_m2_caller_not_outerB.load());
        ctx.check("rc_m2_caller_is_not_the_leaf", g_m2_caller_not_inner.load());
        ctx.check("rc_m2_trace0_matches_caller", g_m2_trace0_matches_caller.load());
        ctx.check("rc_m2_trace_contains_middle", g_m2_trace_has_middle.load());
        // outerB reachability via the multi-frame walk is BEST-EFFORT: it can be
        // JIT-inlined into its caller and then have no interpreter frame.  Record,
        // don't FAIL — caller()'s "middle not outerB" already proves the depth-1
        // contract.
        if (g_m2_trace_has_outerB.load())
        {
            ctx.check("rc_m2_trace_contains_outerB", true);
        }
        else
        {
            ctx.record("[INFO] rc_m2_trace_contains_outerB: outerB frame absent "
                       "(JIT-inlined?) — best-effort, caller()==middle is the HARD contract");
        }
    }

    // =====================================================================
    // Scenario 3 — deep self-recursion recurse(90) -> inner.  caller() resolves
    // the IMMEDIATE recurse frame (the bottom of the recursion); stack_trace()
    // walks the deep chain but must terminate AT the 64 cap without spinning.
    // =====================================================================
    {
        auto handle{ vmhook::scoped_hook<caller_fixture>(
            "inner", "(I)I",
            [](vmhook::return_value& ret,
               const std::unique_ptr<caller_fixture>& /*self*/,
               std::int32_t /*x*/)
            {
                g_m3_fires.fetch_add(1, std::memory_order_relaxed);

                const auto info{ ret.caller() };
                g_m3_caller_valid.store(info.valid(), std::memory_order_relaxed);
                if (info.valid())
                {
                    g_m3_caller_is_recurse.store(names(info, "recurse", SIG_II),
                                                 std::memory_order_relaxed);
                }

                const auto trace{ ret.stack_trace() };   // default cap
                g_m3_trace_size.store(trace.size(), std::memory_order_relaxed);
                g_m3_trace_within_cap.store(trace.size() <= DEFAULT_CAP,
                                            std::memory_order_relaxed);
                if (!trace.empty() && info.valid())
                {
                    g_m3_trace0_matches_caller.store(
                        trace.front().method == info.method,
                        std::memory_order_relaxed);
                }
            }) };

        ctx.check("rc_m3_hook_installed", handle.installed());

        const bool done{ drive(ctx, 3) };
        // HARD: no crash / no infinite loop on the deep saved-rbp chain.
        ctx.check("rc_m3_probe_completed", done);
        ctx.check("rc_m3_leaf_ran_once", caller_fixture::get_inner_calls() == 1);
        ctx.check("rc_m3_fired_once", g_m3_fires.load() == 1);

        ctx.check("rc_m3_caller_valid", g_m3_caller_valid.load());
        // The immediate caller of inner in the recursion is a `recurse` frame.
        ctx.check("rc_m3_caller_is_recurse", g_m3_caller_is_recurse.load());
        ctx.check("rc_m3_trace0_matches_caller", g_m3_trace0_matches_caller.load());
        // The walk terminated cleanly within the cap (never spun on the chain).
        ctx.check("rc_m3_trace_within_cap", g_m3_trace_within_cap.load());

        ctx.record(std::string{ "[INFO] return_caller: recurse(" }
                   + std::to_string(RECURSION_DEPTH) + ") default trace size = "
                   + std::to_string(g_m3_trace_size.load()) + " (cap "
                   + std::to_string(DEFAULT_CAP) + ").");
    }

    // =====================================================================
    // Scenario 4 — long reference-heavy descriptor longSig(8 x Object, int).
    // caller().signature must equal the exact 140+ char JVM descriptor,
    // character-for-character, NOT truncated by any fixed buffer.
    // =====================================================================
    {
        auto handle{ vmhook::scoped_hook<caller_fixture>(
            "inner", "(I)I",
            [](vmhook::return_value& ret,
               const std::unique_ptr<caller_fixture>& /*self*/,
               std::int32_t /*x*/)
            {
                g_m4_fires.fetch_add(1, std::memory_order_relaxed);

                const auto info{ ret.caller() };
                g_m4_caller_valid.store(info.valid(), std::memory_order_relaxed);
                if (info.valid())
                {
                    g_m4_caller_is_longSig.store(info.method_name == "longSig",
                                                 std::memory_order_relaxed);
                    g_m4_sig_exact.store(info.signature == LONGSIG_DESC,
                                         std::memory_order_relaxed);
                    g_m4_sig_len.store(info.signature.size(), std::memory_order_relaxed);
                    // Not truncated: begins '(' and ends with the return-type tail ")I".
                    const std::string& s{ info.signature };
                    g_m4_sig_not_truncated.store(
                        s.size() >= 3 && s.front() == '('
                     && s.compare(s.size() - 2, 2, ")I") == 0,
                        std::memory_order_relaxed);
                }

                const auto trace{ ret.stack_trace() };
                if (!trace.empty())
                {
                    g_m4_trace0_sig_exact.store(trace.front().signature == LONGSIG_DESC,
                                                std::memory_order_relaxed);
                }
            }) };

        ctx.check("rc_m4_hook_installed", handle.installed());

        const bool done{ drive(ctx, 4) };
        ctx.check("rc_m4_probe_completed", done);
        ctx.check("rc_m4_leaf_ran_once", caller_fixture::get_inner_calls() == 1);
        ctx.check("rc_m4_fired_once", g_m4_fires.load() == 1);

        ctx.check("rc_m4_caller_valid", g_m4_caller_valid.load());
        ctx.check("rc_m4_caller_is_longSig", g_m4_caller_is_longSig.load());
        // The headline: the exact long descriptor survives, untruncated.
        ctx.check("rc_m4_signature_exact_match", g_m4_sig_exact.load());
        ctx.check("rc_m4_signature_over_140_chars", g_m4_sig_len.load() > 140);
        ctx.check("rc_m4_signature_not_truncated", g_m4_sig_not_truncated.load());
        ctx.check("rc_m4_trace0_signature_exact", g_m4_trace0_sig_exact.load());

        ctx.record(std::string{ "[INFO] return_caller: longSig descriptor length = " }
                   + std::to_string(g_m4_sig_len.load()) + " chars (expected "
                   + std::to_string(LONGSIG_DESC.size()) + ").");
    }

    // =====================================================================
    // Scenario 5 — JIT-compiled immediate caller warmCaller (heated 200k iters
    // above the C2 threshold) -> inner.  The contract: a COMPILED immediate
    // caller must NOT be reported as an interpreted frame.  Written as the
    // inlining/JIT-timing-robust invariant "caller() is INVALID, OR (if valid)
    // it is NOT warmCaller" — a hot C2 build satisfies it via invalidity (the
    // saved-rbp chain breaks at the compiled frame, monotonicity guard bails);
    // a pure-interpreter (-Xint) build satisfies it via warmCaller staying
    // interpreted (then the not-warmCaller half is naturally false, recorded
    // [INFO]).  We never assert "warmCaller appears" or "warmCaller absent" as
    // HARD — only the disjunction.
    // =====================================================================
    {
        auto handle{ vmhook::scoped_hook<caller_fixture>(
            "inner", "(I)I",
            [](vmhook::return_value& ret,
               const std::unique_ptr<caller_fixture>& /*self*/,
               std::int32_t /*x*/)
            {
                g_m5_fires.fetch_add(1, std::memory_order_relaxed);

                const auto info{ ret.caller() };
                const bool valid{ info.valid() };
                const bool is_warm{ valid && info.method_name == "warmCaller" };
                g_m5_caller_valid.store(valid, std::memory_order_relaxed);
                g_m5_caller_is_warmCaller.store(is_warm, std::memory_order_relaxed);
                // HARD invariant: compiled caller is not reported as interpreted.
                //   invalid           -> compiled frame correctly broke the chain, OR
                //   valid & !warmCaller -> we resolved an interpreted ANCESTOR, never
                //                          the compiled warmCaller frame itself.
                g_m5_robust_invariant.store(!valid || !is_warm,
                                            std::memory_order_relaxed);

                const auto trace{ ret.stack_trace() };
                g_m5_trace_lacks_warmCaller.store(!trace_contains(trace, "warmCaller"),
                                                  std::memory_order_relaxed);
            }) };

        ctx.check("rc_m5_hook_installed", handle.installed());

        const bool done{ drive(ctx, 5) };
        // HARD: the hot-loop + the leaf fire did not crash the JVM.
        ctx.check("rc_m5_probe_completed", done);
        ctx.check("rc_m5_leaf_ran_once", caller_fixture::get_inner_calls() == 1);
        ctx.check("rc_m5_fired_once", g_m5_fires.load() == 1);

        // The only HARD identity check here is the inlining/JIT-robust invariant.
        ctx.check("rc_m5_compiled_caller_not_reported_interpreted",
                  g_m5_robust_invariant.load());

        // BEST-EFFORT: on a hot C2 build the trace omits the compiled frame.  On
        // -Xint warmCaller stays interpreted and legitimately appears — record,
        // never FAIL.
        if (g_m5_trace_lacks_warmCaller.load())
        {
            ctx.check("rc_m5_trace_omits_compiled_warmCaller", true);
        }
        else
        {
            ctx.record("[INFO] rc_m5_trace_omits_compiled_warmCaller: warmCaller present "
                       "in trace (interpreter / -Xint build, not yet compiled) — best-effort");
        }

        ctx.record(std::string{ "[INFO] return_caller: warmCaller scenario — caller valid=" }
                   + (g_m5_caller_valid.load() ? "yes" : "no")
                   + ", caller==warmCaller=" + (g_m5_caller_is_warmCaller.load() ? "yes" : "no")
                   + " (HARD contract = invalid OR not-warmCaller; satisfied by C2 via "
                   "invalidity and by -Xint via interpreted ancestor).");
    }

    // =====================================================================
    // Scenario 6 — two DISTINCT interpreted callers in ONE cycle: alpha -> inner
    // then beta -> inner.  caller() must report alpha on fire 0 and beta on fire
    // 1 — proving it is NOT a stale cache across fires.
    // =====================================================================
    {
        auto handle{ vmhook::scoped_hook<caller_fixture>(
            "inner", "(I)I",
            [](vmhook::return_value& ret,
               const std::unique_ptr<caller_fixture>& /*self*/,
               std::int32_t /*x*/)
            {
                const std::int32_t order{ g_m6_fires.fetch_add(1, std::memory_order_relaxed) };
                const auto info{ ret.caller() };
                if (order == 0)
                {
                    g_m6_first_valid.store(info.valid(), std::memory_order_relaxed);
                    g_m6_first_is_alpha.store(names(info, "alpha", SIG_II),
                                              std::memory_order_relaxed);
                    g_m6_first_method.store(static_cast<void*>(info.method),
                                            std::memory_order_relaxed);
                }
                else if (order == 1)
                {
                    g_m6_second_valid.store(info.valid(), std::memory_order_relaxed);
                    g_m6_second_is_beta.store(names(info, "beta", SIG_II),
                                              std::memory_order_relaxed);
                    g_m6_second_method.store(static_cast<void*>(info.method),
                                             std::memory_order_relaxed);
                }
            }) };

        ctx.check("rc_m6_hook_installed", handle.installed());

        const bool done{ drive(ctx, 6) };
        ctx.check("rc_m6_probe_completed", done);
        ctx.check("rc_m6_leaf_ran_twice", caller_fixture::get_inner_calls() == 2);
        ctx.check("rc_m6_fired_twice", g_m6_fires.load() == 2);

        ctx.check("rc_m6_first_valid", g_m6_first_valid.load());
        ctx.check("rc_m6_second_valid", g_m6_second_valid.load());
        // The headline: per-fire correctness — fire 0 == alpha, fire 1 == beta.
        ctx.check("rc_m6_first_caller_is_alpha", g_m6_first_is_alpha.load());
        ctx.check("rc_m6_second_caller_is_beta", g_m6_second_is_beta.load());
        // Distinct Method*s prove caller() recomputed live per fire (no stale cache).
        if (g_m6_first_method.load() != nullptr && g_m6_second_method.load() != nullptr)
        {
            ctx.check("rc_m6_callers_distinct",
                      g_m6_first_method.load() != g_m6_second_method.load());
        }
        else
        {
            ctx.record("[INFO] rc_m6_callers_distinct: a caller Method* was null "
                       "(frame not interpreted?) — best-effort");
        }
    }

    // =====================================================================
    // Scenario 7 — longArgCaller(long) -> inner.  The caller's descriptor is
    // (J)I, explicitly DISTINCT from the leaf's own (I)I — proving caller()
    // decodes the per-caller descriptor, not echoing the hooked method's.
    // =====================================================================
    {
        auto handle{ vmhook::scoped_hook<caller_fixture>(
            "inner", "(I)I",
            [](vmhook::return_value& ret,
               const std::unique_ptr<caller_fixture>& /*self*/,
               std::int32_t /*x*/)
            {
                g_m7_fires.fetch_add(1, std::memory_order_relaxed);

                const auto info{ ret.caller() };
                g_m7_caller_valid.store(info.valid(), std::memory_order_relaxed);
                if (info.valid())
                {
                    g_m7_caller_is_longArg.store(info.method_name == "longArgCaller",
                                                 std::memory_order_relaxed);
                    g_m7_sig_is_JI.store(info.signature == SIG_JI,
                                         std::memory_order_relaxed);
                    g_m7_sig_not_II.store(info.signature != SIG_II,
                                          std::memory_order_relaxed);
                }
            }) };

        ctx.check("rc_m7_hook_installed", handle.installed());

        const bool done{ drive(ctx, 7) };
        ctx.check("rc_m7_probe_completed", done);
        ctx.check("rc_m7_leaf_ran_once", caller_fixture::get_inner_calls() == 1);
        ctx.check("rc_m7_fired_once", g_m7_fires.load() == 1);

        ctx.check("rc_m7_caller_valid", g_m7_caller_valid.load());
        ctx.check("rc_m7_caller_is_longArgCaller", g_m7_caller_is_longArg.load());
        // The headline: the caller's own descriptor (J)I is reported, distinct
        // from the hooked leaf's (I)I.
        ctx.check("rc_m7_caller_sig_is_JI", g_m7_sig_is_JI.load());
        ctx.check("rc_m7_caller_sig_distinct_from_leaf", g_m7_sig_not_II.load());
    }

    // No detour may be left armed for the next module: every scoped_hook above
    // uninstalled on scope exit.  Belt-and-braces, hard-reset all global hook
    // state UNCONDITIONALLY on every path (a no-SEH contained-crash longjmp would
    // skip the scoped_hook destructors, leaving a detour armed for the next
    // module — this restores a clean slate regardless).
    if (ctx.reset)
    {
        ctx.reset();
    }
}
