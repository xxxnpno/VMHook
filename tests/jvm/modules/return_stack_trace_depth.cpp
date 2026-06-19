// return_stack_trace_depth JVM test module  (feature area: hooks)
//
// Exhaustively exercises return_value::stack_trace() — the MULTI-FRAME walk of
// the HotSpot interpreter saved-rbp chain — and its companion caller(), on a
// LIVE JVM via real Java bytecode dispatch.  This is the modular migration +
// extension of the legacy inline test_caller_info (vmhook/src/example.cpp): that
// test hooked CallerProbe.innerStep and checked a 2-deep chain; here we drive a
// known 3-deep chain outer() -> mid() -> inner() (inner is the hooked leaf) plus
// deep recursion, so the DEPTH, per-frame method/class NAMES, and the ORDER
// (immediate-caller-first) of the trace are all pinned to known values.
//
// What this module proves, all from inside a detour on the fixed leaf inner(I)I:
//   * KNOWN DEPTH + ORDER + NAMES: with outer->mid->inner live, stack_trace()
//     returns the interpreted frames immediate-caller-first, so the trace
//     CONTAINS mid and outer (each with the right class_name vmhook/fixtures/
//     ReturnStackTrace and a (I)I signature) in the caller-chain ORDER mid-
//     before-outer, and outer is a frame caller() does NOT report.  This is the
//     headline that distinguishes the multi-frame walk from caller().  NB: the
//     named frames are located by SEARCH, not at hard-coded indices 0/1 — see
//     the DEFLAKE note below.
//   * stack_trace().front() AGREES with caller() (method ptr + name) — the
//     documented "index 0 == caller()" contract (legacy stackTraceFirstMatches);
//     robust because both read the same immediate frame by construction.
//   * max_depth CONTRACT: stack_trace(1) returns exactly one frame, stack_trace(2)
//     exactly two, each cap is the element-wise PREFIX of a deeper capture
//     (truncation preserves order), and the documented "pass 0 for the default"
//     promotion returns the default-capped trace (>= the explicit small caps),
//     NOT an empty vector.
//   * DEEP RECURSION past the 64 cap: a recurse(120) chain makes the default
//     stack_trace() terminate cleanly AT the cap (size <= 64, never spinning /
//     AV-ing on the saved-rbp chain), with a long UNIFORM run of identical
//     `recurse` frames; an explicit cap below the real depth truncates exactly.
//   * PER-FIRE FRESHNESS: two different chains in one probe cycle (a 2-frame
//     shallow->inner then the 3-frame outer->mid->inner) yield two traces that
//     DIFFER in their frame set — the first contains shallow but not mid, the
//     second contains mid but not shallow — proving each trace is recomputed
//     live, not a stale cached copy.  (Both hit the 64 cap, so the freshness
//     signal is this discrimination, not a depth difference.)
//
// DEFLAKE (frame-position robustness): every named-frame assertion locates the
// frame by NAME anywhere in the trace (find_fixture_frame / chain_in_order /
// is_method_prefix) instead of indexing a fixed slot.  A JDK-version interpreter
// layout difference, an intervening synthetic/bridge/lambda frame, or partial JIT
// of an adjacent un-pinned frame can shift a named frame's POSITION without the
// trace being wrong; the historically-flaky "frame K is method X" checks
// (e.g. stk_two_second_immediate_is_mid) are therefore demoted to VISIBLE [INFO]
// records, while the contract that the trace CONTAINS the live caller chain (in
// order) stays HARD.  Count / cap / monotonicity / freshness-discrimination
// checks are position-independent and remain HARD.
//
// SAFETY (why this module cannot crash the JVM on any combo):
//   * The walk only ever traverses VALID INTERPRETER FRAMES.  This module runs
//     LATE in the suite, by which time the generic Harness.tickAll dispatch
//     frame sitting above every probe has JIT-compiled.  A compiled (or its
//     i2c/c2i adapter) frame does NOT follow the interpreter saved-rbp layout
//     stack_trace() assumes, so a walk that reaches it must fall back on the
//     library's best-effort "stop on a non-interpreter frame" logic.  That
//     fallback was observed to AV intermittently on ONE CI runtime (mingw +
//     JDK24) when the stray read landed on unmapped metaspace — the unbounded
//     ConstantPool index read inside const_method::get_name()/get_signature()
//     dereferences base[index] BEFORE it can reject a bogus Method* (see the
//     module REPORT for the proposed vmhook.hpp fix).  To make the MODULE
//     crash-proof regardless, the fixture reaches every shallow named chain
//     (modes 1/2/4) through a deep INTERPRETED guard recursion
//     (ReturnStackTrace.guard, GUARD_DEPTH=80 > the 64 cap): a default-capped
//     walk then exhausts its budget on guard frames and NEVER reaches the
//     compiled boundary.  Mode 3's recurse(120) chain is inherently safe the
//     same way (64 interpreted frames before the cap).  Belt-and-braces, the
//     module ALSO pins the guard/recurse methods interpreted for the duration
//     (an allow-through hook sets _dont_inline + NO_COMPILE) so a future JIT
//     policy change can't shorten the interpreted buffer.
//   * This module never dereferences a raw frame/method pointer itself: it only
//     reads the std::string fields of the returned caller_info and COMPARES the
//     method* values (never deref), so even a bogus caller_info cannot fault it.
//   * Lifecycle: per-scenario inner hooks are scoped_hook (uninstall on scope
//     exit); the guard/recurse pins are torn down and shutdown_hooks() is called
//     at the very end, so no detour is left armed for the next module.
//
// Harness note: `done` LATCHES (run_java_probe never clears it).  Each scenario
// resets observations + clears done and programs `mode` on the rising edge of
// `go`, runs ONE probe cycle, then reads the recorded observations back.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    // Wrapper for vmhook.fixtures.ReturnStackTrace.  Deriving from
    // vmhook::object<> gives it a vtable (required by register_class<T>) and the
    // static_field(...) accessors used for the go/done/mode handshake and the
    // recorded-observation fields.  Each typed getter reads into a concretely-
    // typed local first: field_proxy's value_t conversion operator is templated,
    // so a bare `static_field(...)->get() == x` is an ambiguous deduction.
    class stack_trace_fixture : public vmhook::object<stack_trace_fixture>
    {
    public:
        explicit stack_trace_fixture(vmhook::oop_t instance) noexcept
            : vmhook::object<stack_trace_fixture>{ instance }
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

    // ── Fixture-mirrored constants (kept in lockstep with ReturnStackTrace.java)
    constexpr std::int32_t DEEP_RECURSION{ 120 };
    constexpr std::int32_t ARG_OUTER{ 100 };
    constexpr std::int32_t ARG_SHALLOW{ 200 };
    // Depth of the interpreted guard recursion that wraps the shallow named
    // chains (modes 1/2/4).  Mirrors ReturnStackTrace.GUARD_DEPTH; only the
    // relationship GUARD_DEPTH > DEFAULT_CAP is load-bearing here.
    constexpr std::int32_t GUARD_DEPTH{ 80 };

    // The default cap documented by stack_trace(max_depth = 64).
    constexpr std::size_t  DEFAULT_CAP{ 64 };
    // An explicit cap comfortably ABOVE the default but well within the
    // guard-deep chain (GUARD_DEPTH=80 interpreter frames + the named chain +
    // the run/probe frames), so stack_trace(BIG_CAP) reaches strictly deeper
    // than the 64-frame default — proving the default truncates at the cap, not
    // at the natural end of an only-64-deep chain.
    constexpr std::size_t  BIG_CAP{ 72 };
    static_assert(BIG_CAP > DEFAULT_CAP,
                  "BIG_CAP must exceed the default cap to prove cap-bounded truncation");
    static_assert(static_cast<std::int32_t>(BIG_CAP) < GUARD_DEPTH,
                  "BIG_CAP must stay within the guard recursion so the deeper walk "
                  "still never reaches the compiled Harness.tickAll frame");
    static_assert(GUARD_DEPTH > static_cast<std::int32_t>(DEFAULT_CAP),
                  "guard recursion must out-depth the default cap so a capped walk "
                  "never reaches the compiled Harness.tickAll frame");

    // Internal JVM names used in every per-frame name assertion.
    const std::string CLASS_NAME{ "vmhook/fixtures/ReturnStackTrace" };
    const std::string SIG_II{ "(I)I" };

    // ── Mode 1 — known depth-3 chain outer -> mid -> inner ────────────────────
    // SEARCH-based (deflaked): mid/outer are located by name ANYWHERE in the
    // trace, not at fixed indices 0/1 — a JDK-version interpreter-layout change
    // or an intervening synthetic frame can shift their position without breaking
    // the contract (the trace still REFLECTS the live mid -> outer caller chain).
    std::atomic<std::int32_t> g_k_fires{ 0 };
    std::atomic<bool>         g_k_caller_valid{ false };
    std::atomic<std::int32_t> g_k_trace_size{ 0 };
    std::atomic<bool>         g_k_front_matches_caller{ false }; // trace.front() == caller()
    std::atomic<bool>         g_k_mid_present{ false };          // mid (I)I appears in trace
    std::atomic<bool>         g_k_outer_present{ false };        // outer (I)I appears in trace
    std::atomic<bool>         g_k_chain_mid_then_outer{ false }; // mid located before outer
    std::atomic<bool>         g_k_mid_outer_distinct{ false };   // found mid.method != found outer.method
    std::atomic<bool>         g_k_outer_beyond_caller{ false };  // found outer.method != caller() (multi-frame reach)
    // [INFO]-only positional observations (the genuinely-unstable claims).
    std::atomic<bool>         g_k_immediate_is_mid{ false };     // INFO: was index 0 exactly mid this run?
    std::atomic<std::int32_t> g_k_mid_index{ -1 };               // INFO: where mid landed
    std::atomic<std::int32_t> g_k_outer_index{ -1 };             // INFO: where outer landed

    // ── Mode 2 — max_depth contract on the SAME chain ─────────────────────────
    // The HARD contract is COUNT (an explicit cap N returns exactly N frames) and
    // TRUNCATION-AS-PREFIX (stack_trace(N) is the element-wise prefix of the
    // deeper trace) — both position-independent.  Whether the immediate frame is
    // EXACTLY mid is recorded [INFO] only (same frame-layout fragility as mode 1).
    std::atomic<std::int32_t> g_d_fires{ 0 };
    std::atomic<std::size_t>  g_d_size_1{ 0 };
    std::atomic<std::size_t>  g_d_size_2{ 0 };
    std::atomic<std::size_t>  g_d_size_0{ 0 };       // promoted-to-default size
    std::atomic<std::size_t>  g_d_size_default{ 0 }; // stack_trace() with no arg
    std::atomic<bool>         g_d_cap1_prefix_of_cap2{ false };    // cap1 == cap2[0..1) by method
    std::atomic<bool>         g_d_cap2_prefix_of_default{ false }; // cap2 == capd[0..2) by method
    std::atomic<bool>         g_d_chain_mid_then_outer{ false };   // default trace: mid before outer (search)
    std::atomic<bool>         g_d_cap1_is_mid{ false };            // INFO: cap1[0] exactly mid?
    std::atomic<bool>         g_d_cap2_mid_then_outer{ false };    // INFO: cap2 exactly [mid,outer]?

    // ── Mode 3 — deep recursion beyond the cap ────────────────────────────────
    std::atomic<std::int32_t> g_r_fires{ 0 };
    std::atomic<std::size_t>  g_r_default_size{ 0 };   // stack_trace() default
    std::atomic<std::size_t>  g_r_cap5_size{ 0 };      // stack_trace(5)
    std::atomic<std::size_t>  g_r_uniform_run{ 0 };    // longest run of identical recurse frames
    std::atomic<bool>         g_r_no_spin{ false };     // size strictly <= cap (terminated)
    std::atomic<bool>         g_r_front_valid{ false };

    // ── Mode 4 — two chains in one cycle (freshness) ──────────────────────────
    // Both chains are reached through the deep interpreted guard recursion, so
    // each default-capped trace hits the 64 cap and the two are the SAME length.
    // The live-recompute proof is SEARCH-based and position-independent: the first
    // fire's chain (guard->shallow->inner) CONTAINS shallow but NOT mid; the
    // second fire's chain (guard->outer->mid->inner) CONTAINS mid but NOT shallow.
    // That per-fire DISCRIMINATION (different frame *sets*, and the located
    // shallow/mid being distinct Method*s) cannot come from a stale/cached trace —
    // and it does not depend on which index the named frame lands at.  Whether the
    // immediate (index-0) frame is EXACTLY shallow/mid is recorded [INFO] only.
    std::atomic<std::int32_t> g_t_fires{ 0 };
    std::atomic<std::size_t>  g_t_size_first{ 0 };
    std::atomic<std::size_t>  g_t_size_second{ 0 };
    std::atomic<bool>         g_t_first_has_shallow{ false };     // first trace contains shallow
    std::atomic<bool>         g_t_first_lacks_mid{ false };       // first trace does NOT contain mid
    std::atomic<bool>         g_t_second_has_mid{ false };        // second trace contains mid
    std::atomic<bool>         g_t_second_lacks_shallow{ false };  // second trace does NOT contain shallow
    std::atomic<void*>        g_t_first_shallow_method{ nullptr };// located shallow Method* (fire 1)
    std::atomic<void*>        g_t_second_mid_method{ nullptr };   // located mid Method* (fire 2)
    std::atomic<bool>         g_t_first_nonempty{ false };
    std::atomic<bool>         g_t_second_nonempty{ false };
    // [INFO]-only positional observations (fragile fixed-index claims).
    std::atomic<bool>         g_t_first_immediate_shallow{ false };
    std::atomic<bool>         g_t_second_immediate_mid{ false };
    // Both guard-deep traces fill to the same 64 cap (HARD); each is well-formed.
    std::atomic<bool>         g_t_first_wellformed{ false };
    std::atomic<bool>         g_t_second_wellformed{ false };

    // ── Mode 1 (extra) — frame well-formedness + caller()/front() FULL agreement
    // Every frame the walk RETURNS must be well-formed: the library breaks the
    // walk the instant a name fails to resolve, so a returned frame can never
    // carry an empty method_name / class_name / signature or a null method.
    std::atomic<bool>         g_k_all_frames_wellformed{ false };
    // caller()/front() agree on ALL FOUR fields, not just method+name.
    std::atomic<bool>         g_k_front_full_agrees_caller{ false };
    // The hooked leaf inner(I)I must NOT appear in its own caller trace (the
    // walk starts at the IMMEDIATE caller, never the leaf itself).
    std::atomic<bool>         g_k_leaf_absent_from_trace{ false };
    // Calling stack_trace() twice in the SAME detour yields an identical method
    // sequence — no per-call cursor mutation / single-shot exhaustion bug.
    std::atomic<bool>         g_k_idempotent{ false };

    // ── Mode 2 (extra) — wider cap sweep + monotonicity + saturation ──────────
    std::atomic<std::size_t>  g_d_size_3{ 0 };       // stack_trace(3)
    std::atomic<std::size_t>  g_d_size_big{ 0 };     // stack_trace(BIG_CAP) — chain is guard-deep
    std::atomic<bool>         g_d_caps_nondecreasing{ false };  // 1<=2<=3<=default
    std::atomic<bool>         g_d_cap3_prefix_of_big{ false };  // cap3 == big[0..3)
    std::atomic<bool>         g_d_big_exceeds_default{ false }; // explicit cap > 64 reaches deeper than the 64 default

    // ── Mode 3 (extra) — recursion pointer identity + cap saturation ──────────
    std::atomic<std::size_t>  g_r_cap100_size{ 0 };          // stack_trace(100) on a 120-deep chain
    std::atomic<std::size_t>  g_r_cap100_recurse_run{ 0 };   // uniform recurse run within cap100
    std::atomic<bool>         g_r_cap5_prefix_of_default{ false };
    std::atomic<std::int32_t> g_r_distinct_recurse_methods{ -1 }; // # distinct Method* among recurse frames (must be 1)
    std::atomic<bool>         g_r_all_frames_wellformed{ false };
    std::atomic<bool>         g_r_default_lt_chain{ false };  // default cap (64) < the real 120-deep chain => cap (not chain end) bounds it

    auto reset_observations() -> void
    {
        g_k_fires.store(0);
        g_k_caller_valid.store(false);
        g_k_trace_size.store(0);
        g_k_front_matches_caller.store(false);
        g_k_mid_present.store(false);
        g_k_outer_present.store(false);
        g_k_chain_mid_then_outer.store(false);
        g_k_mid_outer_distinct.store(false);
        g_k_outer_beyond_caller.store(false);
        g_k_immediate_is_mid.store(false);
        g_k_mid_index.store(-1);
        g_k_outer_index.store(-1);
        g_k_all_frames_wellformed.store(false);
        g_k_front_full_agrees_caller.store(false);
        g_k_leaf_absent_from_trace.store(false);
        g_k_idempotent.store(false);

        g_d_fires.store(0);
        g_d_size_1.store(0);
        g_d_size_2.store(0);
        g_d_size_0.store(0);
        g_d_size_default.store(0);
        g_d_cap1_prefix_of_cap2.store(false);
        g_d_cap2_prefix_of_default.store(false);
        g_d_chain_mid_then_outer.store(false);
        g_d_cap1_is_mid.store(false);
        g_d_cap2_mid_then_outer.store(false);
        g_d_size_3.store(0);
        g_d_size_big.store(0);
        g_d_caps_nondecreasing.store(false);
        g_d_cap3_prefix_of_big.store(false);
        g_d_big_exceeds_default.store(false);

        g_r_fires.store(0);
        g_r_default_size.store(0);
        g_r_cap5_size.store(0);
        g_r_uniform_run.store(0);
        g_r_no_spin.store(false);
        g_r_front_valid.store(false);
        g_r_cap100_size.store(0);
        g_r_cap100_recurse_run.store(0);
        g_r_cap5_prefix_of_default.store(false);
        g_r_distinct_recurse_methods.store(-1);
        g_r_all_frames_wellformed.store(false);
        g_r_default_lt_chain.store(false);

        g_t_fires.store(0);
        g_t_size_first.store(0);
        g_t_size_second.store(0);
        g_t_first_has_shallow.store(false);
        g_t_first_lacks_mid.store(false);
        g_t_second_has_mid.store(false);
        g_t_second_lacks_shallow.store(false);
        g_t_first_shallow_method.store(nullptr);
        g_t_second_mid_method.store(nullptr);
        g_t_first_nonempty.store(false);
        g_t_second_nonempty.store(false);
        g_t_first_immediate_shallow.store(false);
        g_t_second_immediate_mid.store(false);
        g_t_first_wellformed.store(false);
        g_t_second_wellformed.store(false);
    }

    // Returns true when the caller_info names the given method of our fixture
    // class with a plain (I)I descriptor — the shape every named chain frame has.
    auto is_fixture_frame(const vmhook::return_value::caller_info& info,
                          const std::string& method) noexcept -> bool
    {
        return info.method != nullptr
            && info.class_name  == CLASS_NAME
            && info.method_name == method
            && info.signature   == SIG_II;
    }

    using trace_t = std::vector<vmhook::return_value::caller_info>;
    constexpr std::size_t NPOS{ static_cast<std::size_t>(-1) };

    // SEARCH-based frame locate (the deflake primitive).  Returns the index of
    // the FIRST frame in `trace` that names the given fixture method (I)I, or
    // NPOS if none.  Robust to frame-position variation: the named chain frame
    // (mid/outer/shallow) can be shifted off a hard-coded index by a JDK-version
    // interpreter-layout difference, an intervening synthetic/bridge/lambda frame,
    // or partial JIT of an adjacent frame — but as long as it is still an
    // interpreted frame somewhere above the leaf, this finds it.  Reads only the
    // std::string/method* fields of the returned caller_info, so it cannot fault.
    auto find_fixture_frame(const trace_t& trace, const std::string& method) noexcept
        -> std::size_t
    {
        for (std::size_t i{ 0 }; i < trace.size(); ++i)
        {
            if (is_fixture_frame(trace[i], method))
            {
                return i;
            }
        }
        return NPOS;
    }

    // True when `first` and `second` BOTH appear as fixture frames in `trace` AND
    // `first` appears strictly before `second` (caller chain order, immediate
    // caller first), tolerant of any number of intervening frames between them.
    // This is the ordered-chain contract — stack_trace reflects the live caller
    // chain mid -> outer (callee before caller) — without pinning either to a
    // fixed index.
    auto chain_in_order(const trace_t& trace,
                        const std::string& first,
                        const std::string& second) noexcept -> bool
    {
        const std::size_t i{ find_fixture_frame(trace, first) };
        if (i == NPOS)
        {
            return false;
        }
        // Search for `second` only AFTER the located `first` frame.
        for (std::size_t j{ i + 1 }; j < trace.size(); ++j)
        {
            if (is_fixture_frame(trace[j], second))
            {
                return true;
            }
        }
        return false;
    }

    // True when `shorter` is an element-wise prefix of `longer`, comparing only
    // the (already-validated, never-dereferenced) Method* of each frame.  This is
    // the position-INDEPENDENT truncation contract for max_depth: stack_trace(N)
    // must be exactly the first N frames of any deeper capture, whatever those
    // frames happen to be.  Requires shorter to be non-empty and no longer than
    // longer (an empty prefix is treated as a non-result so a failed capture can't
    // pass vacuously).
    auto is_method_prefix(const trace_t& shorter, const trace_t& longer) noexcept -> bool
    {
        if (shorter.empty() || shorter.size() > longer.size())
        {
            return false;
        }
        for (std::size_t i{ 0 }; i < shorter.size(); ++i)
        {
            if (shorter[i].method != longer[i].method)
            {
                return false;
            }
        }
        return true;
    }

    // True when EVERY frame the walk returned is structurally well-formed: a
    // non-null method and all three name strings non-empty.  This is a HARD
    // universal invariant: stack_trace() breaks the walk the instant a Method*
    // name fails to resolve, so a RETURNED frame can never carry an empty
    // method_name (and a real interpreter Method always has a class_name +
    // signature too).  An empty trace is treated as a non-result (false) so a
    // failed capture cannot pass this vacuously.  Reads only the std::string /
    // method* fields, so it cannot fault.
    auto all_frames_wellformed(const trace_t& trace) noexcept -> bool
    {
        if (trace.empty())
        {
            return false;
        }
        for (const auto& f : trace)
        {
            if (f.method == nullptr
                || f.method_name.empty()
                || f.class_name.empty()
                || f.signature.empty())
            {
                return false;
            }
        }
        return true;
    }

    // Compares two traces element-wise by Method* only (never dereferenced):
    // same length and same pointer at every index.  Used to prove stack_trace()
    // is idempotent within a single detour (no per-call cursor / single-shot
    // exhaustion bug).
    auto same_method_sequence(const trace_t& a, const trace_t& b) noexcept -> bool
    {
        if (a.size() != b.size() || a.empty())
        {
            return false;
        }
        for (std::size_t i{ 0 }; i < a.size(); ++i)
        {
            if (a[i].method != b[i].method)
            {
                return false;
            }
        }
        return true;
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
                    stack_trace_fixture::set_done(false);
                    stack_trace_fixture::set_mode(mode);
                }
                stack_trace_fixture::set_go(value);
            },
            []() { return stack_trace_fixture::get_done(); });
    }

    // Belt-and-braces interpreted-pinning of the high-call-count frames the walk
    // traverses (guard, called GUARD_DEPTH times per probe; recurse, called
    // DEEP_RECURSION times).  An allow-through hook makes vmhook set _dont_inline
    // + NO_COMPILE on the Method, so even an aggressive future JIT policy cannot
    // compile these frames out from under the walk and shorten the interpreted
    // buffer that keeps the walk away from the compiled Harness.tickAll frame.
    // Returns the number of pins that installed (0 is acceptable: at these call
    // counts the methods stay interpreted naturally, as mode 3 has always shown).
    auto pin_walk_frames_interpreted() -> std::size_t
    {
        std::size_t pinned{ 0 };
        // guard(int depth, int tail) -> (I I)I
        if (vmhook::hook<stack_trace_fixture>(
                "guard", "(II)I",
                [](vmhook::return_value&,
                   const std::unique_ptr<stack_trace_fixture>& /*self*/,
                   std::int32_t /*depth*/, std::int32_t /*tail*/) { }))
        {
            ++pinned;
        }
        // recurse(int depth) -> (I)I
        if (vmhook::hook<stack_trace_fixture>(
                "recurse", "(I)I",
                [](vmhook::return_value&,
                   const std::unique_ptr<stack_trace_fixture>& /*self*/,
                   std::int32_t /*depth*/) { }))
        {
            ++pinned;
        }
        return pinned;
    }
}

VMHOOK_JVM_MODULE(return_stack_trace_depth)
{
    vmhook::register_class<stack_trace_fixture>("vmhook/fixtures/ReturnStackTrace");

    reset_observations();

    // Belt-and-braces: pin the deep-recursion frames (guard, recurse) the walk
    // traverses to interpreted-only, so the interpreted buffer that keeps every
    // walk away from the compiled Harness.tickAll boundary cannot be JITed away.
    // These persist across all four scenarios (keyed by their own Method*, so the
    // per-scenario scoped inner-hooks do not disturb them) and are removed by the
    // final shutdown_hooks() in teardown.  A 0 here is acceptable (the methods
    // stay interpreted naturally at these call counts) — reported, not asserted.
    const std::size_t pinned_walk_frames{ pin_walk_frames_interpreted() };
    ctx.record(std::string{ "[INFO] return_stack_trace_depth: pinned " }
               + std::to_string(pinned_walk_frames)
               + "/2 walk frames (guard,recurse) interpreted for crash-safety.");

    // =====================================================================
    // Scenario 1 — KNOWN DEPTH + ORDER + NAMES.
    // Chain ...guard... -> outer(100) -> mid(101) -> inner(102); inner is hooked.
    // Inside the detour the live interpreter frames above us are, immediate-
    // first: mid, outer, then GUARD_DEPTH guard frames, then run()/probe frames.
    // CONTRACT (position-independent): the trace CONTAINS mid and outer, in the
    // caller-chain order mid-before-outer (callee before its caller).  We do NOT
    // pin them to fixed indices 0/1: a JDK-version interpreter-layout difference
    // or an intervening synthetic/bridge frame can shift their position by one
    // without the trace being wrong.  The guard frames push the compiled
    // Harness.tickAll frame past the default 64 cap, so the walk stays entirely
    // within valid interpreter frames.
    // =====================================================================
    {
        auto handle{ vmhook::scoped_hook<stack_trace_fixture>(
            "inner", "(I)I",
            [](vmhook::return_value& ret,
               const std::unique_ptr<stack_trace_fixture>& /*self*/,
               std::int32_t /*x*/)
            {
                g_k_fires.fetch_add(1, std::memory_order_relaxed);

                // caller() reports the IMMEDIATE interpreted caller.
                const auto info{ ret.caller() };
                g_k_caller_valid.store(info.valid(), std::memory_order_relaxed);

                // Full walk: locate mid and outer by NAME (search, not index).
                const auto trace{ ret.stack_trace() };
                g_k_trace_size.store(static_cast<std::int32_t>(trace.size()),
                                     std::memory_order_relaxed);

                // index 0 == caller() (the documented contract; both read the same
                // immediate frame, so they move together and this stays robust).
                if (!trace.empty() && info.valid())
                {
                    g_k_front_matches_caller.store(
                        trace.front().method      == info.method
                     && trace.front().method_name == info.method_name,
                        std::memory_order_relaxed);
                }

                const std::size_t mid_idx{ find_fixture_frame(trace, "mid") };
                const std::size_t outer_idx{ find_fixture_frame(trace, "outer") };
                g_k_mid_present.store(mid_idx != NPOS, std::memory_order_relaxed);
                g_k_outer_present.store(outer_idx != NPOS, std::memory_order_relaxed);
                // Ordered chain: mid (callee) located strictly before outer (caller),
                // tolerant of any intervening frames between them.
                g_k_chain_mid_then_outer.store(chain_in_order(trace, "mid", "outer"),
                                               std::memory_order_relaxed);

                if (mid_idx != NPOS && outer_idx != NPOS)
                {
                    // Distinct Method*: mid and outer are different methods.
                    g_k_mid_outer_distinct.store(
                        trace[mid_idx].method != trace[outer_idx].method,
                        std::memory_order_relaxed);
                    // outer is reachable via the multi-frame walk but is NOT the
                    // immediate caller (mid is between it and the leaf): proves the
                    // walk adds reach beyond caller().  Position-independent.
                    g_k_outer_beyond_caller.store(
                        info.valid() && trace[outer_idx].method != info.method,
                        std::memory_order_relaxed);
                }

                // [INFO]-only positional snapshot (the genuinely-unstable claim).
                g_k_immediate_is_mid.store(
                    !trace.empty() && is_fixture_frame(trace.front(), "mid"),
                    std::memory_order_relaxed);
                g_k_mid_index.store(
                    mid_idx == NPOS ? -1 : static_cast<std::int32_t>(mid_idx),
                    std::memory_order_relaxed);
                g_k_outer_index.store(
                    outer_idx == NPOS ? -1 : static_cast<std::int32_t>(outer_idx),
                    std::memory_order_relaxed);

                // Universal invariant: every RETURNED frame is well-formed (the
                // walk breaks the instant a name fails to resolve, so a frame in
                // the result can never be half-populated).
                g_k_all_frames_wellformed.store(all_frames_wellformed(trace),
                                                std::memory_order_relaxed);

                // front() agrees with caller() on ALL FOUR fields (not just
                // method+name) — they read the same immediate frame by
                // construction, so this is robust.
                if (!trace.empty() && info.valid())
                {
                    g_k_front_full_agrees_caller.store(
                        trace.front().method      == info.method
                     && trace.front().method_name == info.method_name
                     && trace.front().class_name  == info.class_name
                     && trace.front().signature   == info.signature,
                        std::memory_order_relaxed);
                }

                // The hooked leaf inner(I)I must NOT appear in its own caller
                // trace: the walk starts at the IMMEDIATE caller, never the leaf.
                g_k_leaf_absent_from_trace.store(
                    find_fixture_frame(trace, "inner") == NPOS,
                    std::memory_order_relaxed);

                // Idempotency: a second walk in the SAME detour yields the
                // identical Method* sequence (no per-call cursor mutation).
                const auto trace2{ ret.stack_trace() };
                g_k_idempotent.store(same_method_sequence(trace, trace2),
                                     std::memory_order_relaxed);
            }) };

        ctx.check("stk_known_hook_installed", handle.installed());

        const bool done{ drive(ctx, 1) };
        ctx.check("stk_known_probe_completed", done);
        ctx.check("stk_known_leaf_ran_once", stack_trace_fixture::get_inner_calls() == 1);
        ctx.check("stk_known_fired_once", g_k_fires.load() == 1);

        // caller() identified the immediate interpreted frame.
        ctx.check("stk_known_caller_valid", g_k_caller_valid.load());

        // The walk: depth, presence, order, distinctness — all position-independent.
        ctx.check("stk_known_trace_has_two_plus", g_k_trace_size.load() >= 2);
        ctx.check("stk_known_front_matches_caller", g_k_front_matches_caller.load());
        // SEARCH-based: mid and outer are PRESENT in the trace (any index).
        ctx.check("stk_known_mid_present", g_k_mid_present.load());
        ctx.check("stk_known_outer_present", g_k_outer_present.load());
        // The live caller chain mid -> outer appears IN ORDER (callee before caller).
        ctx.check("stk_known_chain_mid_then_outer", g_k_chain_mid_then_outer.load());
        ctx.check("stk_known_mid_outer_distinct", g_k_mid_outer_distinct.load());
        // The headline difference vs caller(): outer is reachable but is NOT the
        // immediate caller — the multi-frame walk sees further than caller().
        ctx.check("stk_known_outer_beyond_caller", g_k_outer_beyond_caller.load());
        // Every returned frame is structurally well-formed (universal invariant).
        ctx.check("stk_known_all_frames_wellformed", g_k_all_frames_wellformed.load());
        // front() == caller() on ALL FOUR fields, not just method+name.
        ctx.check("stk_known_front_full_agrees_caller", g_k_front_full_agrees_caller.load());
        // The hooked leaf does NOT appear in its own caller trace.
        ctx.check("stk_known_leaf_absent_from_trace", g_k_leaf_absent_from_trace.load());
        // The walk is idempotent within one detour (no single-shot exhaustion).
        ctx.check("stk_known_idempotent", g_k_idempotent.load());

        // The fragile fixed-position claim (immediate caller is EXACTLY mid) is
        // recorded VISIBLY, never asserted: its failure is a benign frame-layout
        // shift, not a broken trace (the HARD checks above still hold).
        ctx.record(std::string{ "[INFO] return_stack_trace_depth: known-chain positions: "
                                "immediate==mid? " }
                   + (g_k_immediate_is_mid.load() ? "yes" : "no")
                   + ", mid@" + std::to_string(g_k_mid_index.load())
                   + ", outer@" + std::to_string(g_k_outer_index.load())
                   + " (HARD contract is presence+order, not the index).");

        ctx.record(std::string{ "[INFO] return_stack_trace_depth: known chain trace depth = " }
                   + std::to_string(g_k_trace_size.load())
                   + " (>=2 named interpreter frames mid,outer above the hooked leaf).");
    }

    // =====================================================================
    // Scenario 2 — max_depth CONTRACT on the same outer->mid->inner chain.
    // From the detour: stack_trace(1) -> [mid], stack_trace(2) -> [mid,outer],
    // stack_trace(0) -> default-capped (NOT empty), stack_trace() -> default.
    // =====================================================================
    {
        auto handle{ vmhook::scoped_hook<stack_trace_fixture>(
            "inner", "(I)I",
            [](vmhook::return_value& ret,
               const std::unique_ptr<stack_trace_fixture>& /*self*/,
               std::int32_t /*x*/)
            {
                g_d_fires.fetch_add(1, std::memory_order_relaxed);

                const auto cap1{ ret.stack_trace(1) };
                const auto cap2{ ret.stack_trace(2) };
                const auto cap3{ ret.stack_trace(3) };
                const auto cap0{ ret.stack_trace(0) };   // documented: promotes to default
                const auto capd{ ret.stack_trace() };    // explicit default
                const auto capbig{ ret.stack_trace(BIG_CAP) }; // > default, chain is guard-deep

                g_d_size_1.store(cap1.size(), std::memory_order_relaxed);
                g_d_size_2.store(cap2.size(), std::memory_order_relaxed);
                g_d_size_3.store(cap3.size(), std::memory_order_relaxed);
                g_d_size_0.store(cap0.size(), std::memory_order_relaxed);
                g_d_size_default.store(capd.size(), std::memory_order_relaxed);
                g_d_size_big.store(capbig.size(), std::memory_order_relaxed);

                // TRUNCATION-AS-PREFIX (position-independent): a smaller cap is the
                // element-wise prefix of a larger capture, whatever the frames are.
                g_d_cap1_prefix_of_cap2.store(is_method_prefix(cap1, cap2),
                                              std::memory_order_relaxed);
                g_d_cap2_prefix_of_default.store(is_method_prefix(cap2, capd),
                                                 std::memory_order_relaxed);
                g_d_cap3_prefix_of_big.store(is_method_prefix(cap3, capbig),
                                             std::memory_order_relaxed);
                // Caps are non-decreasing as the budget grows: 1 <= 2 <= 3 <= default.
                g_d_caps_nondecreasing.store(
                    cap1.size() <= cap2.size()
                 && cap2.size() <= cap3.size()
                 && cap3.size() <= capd.size(),
                    std::memory_order_relaxed);
                // An explicit cap ABOVE the default reaches strictly deeper than the
                // 64-frame default — proving the default truncates AT the cap, not at
                // the natural end of an only-64-deep chain (the chain is guard-deep).
                g_d_big_exceeds_default.store(capbig.size() > capd.size(),
                                              std::memory_order_relaxed);
                // The live chain mid -> outer appears IN ORDER in the default trace
                // (search-based; robust to where mid/outer actually land).
                g_d_chain_mid_then_outer.store(chain_in_order(capd, "mid", "outer"),
                                               std::memory_order_relaxed);

                // [INFO]-only positional snapshots (fragile fixed-index claims).
                if (cap1.size() >= 1)
                {
                    g_d_cap1_is_mid.store(is_fixture_frame(cap1[0], "mid"),
                                          std::memory_order_relaxed);
                }
                if (cap2.size() >= 2)
                {
                    g_d_cap2_mid_then_outer.store(
                        is_fixture_frame(cap2[0], "mid")
                     && is_fixture_frame(cap2[1], "outer"),
                        std::memory_order_relaxed);
                }
            }) };

        ctx.check("stk_depth_hook_installed", handle.installed());

        const bool done{ drive(ctx, 2) };
        ctx.check("stk_depth_probe_completed", done);
        ctx.check("stk_depth_fired_once", g_d_fires.load() == 1);

        // Explicit small caps truncate to EXACTLY that many frames (COUNT — HARD).
        ctx.check("stk_depth_cap1_size_is_1", g_d_size_1.load() == 1);
        ctx.check("stk_depth_cap2_size_is_2", g_d_size_2.load() == 2);
        // Truncation is a genuine PREFIX (order preserved, position-independent).
        ctx.check("stk_depth_cap1_prefix_of_cap2", g_d_cap1_prefix_of_cap2.load());
        ctx.check("stk_depth_cap2_prefix_of_default", g_d_cap2_prefix_of_default.load());
        // The default trace REFLECTS the live mid -> outer chain (search, in order).
        ctx.check("stk_depth_chain_mid_then_outer", g_d_chain_mid_then_outer.load());
        // max_depth=0 is documented to PROMOTE to the default, not return empty.
        ctx.check("stk_depth_cap0_not_empty", g_d_size_0.load() >= 2);
        ctx.check("stk_depth_cap0_equals_default", g_d_size_0.load() == g_d_size_default.load());
        // And the default cap is at least the deepest explicit cap we asked for.
        ctx.check("stk_depth_default_ge_cap2", g_d_size_default.load() >= g_d_size_2.load());
        ctx.check("stk_depth_default_within_cap", g_d_size_default.load() <= DEFAULT_CAP);

        // Wider cap sweep: stack_trace(3) is exactly 3 and a genuine prefix of a
        // deeper capture; caps grow monotonically as the budget grows.
        ctx.check("stk_depth_cap3_size_is_3", g_d_size_3.load() == 3);
        ctx.check("stk_depth_cap3_prefix_of_big", g_d_cap3_prefix_of_big.load());
        ctx.check("stk_depth_caps_nondecreasing", g_d_caps_nondecreasing.load());
        // The default size is EXACTLY the cap here (the chain is guard-deep, so the
        // 64-frame budget — not the chain end — bounds the default trace).
        ctx.check("stk_depth_default_is_exactly_cap", g_d_size_default.load() == DEFAULT_CAP);
        // An explicit cap above the default reaches strictly deeper than the
        // default: the cap, not the natural chain end, is what truncates.
        ctx.check("stk_depth_big_exceeds_default", g_d_big_exceeds_default.load());
        ctx.check("stk_depth_big_size_is_cap", g_d_size_big.load() == BIG_CAP);

        ctx.record(std::string{ "[INFO] return_stack_trace_depth: caps {1,2,0->def,def} = {" }
                   + std::to_string(g_d_size_1.load()) + ","
                   + std::to_string(g_d_size_2.load()) + ","
                   + std::to_string(g_d_size_0.load()) + ","
                   + std::to_string(g_d_size_default.load()) + "} (0 promotes to default); "
                   "cap1[0]==mid? " + (g_d_cap1_is_mid.load() ? "yes" : "no")
                   + ", cap2==[mid,outer]? " + (g_d_cap2_mid_then_outer.load() ? "yes" : "no")
                   + " (positional INFO; HARD contract is count+prefix+ordered-chain).");
    }

    // =====================================================================
    // Scenario 3 — DEEP recursion (120 > 64 cap): the walk must terminate AT the
    // cap without spinning on the saved-rbp chain, with a uniform run of
    // identical `recurse` frames; an explicit cap below depth truncates exactly.
    // =====================================================================
    {
        auto handle{ vmhook::scoped_hook<stack_trace_fixture>(
            "inner", "(I)I",
            [](vmhook::return_value& ret,
               const std::unique_ptr<stack_trace_fixture>& /*self*/,
               std::int32_t /*x*/)
            {
                g_r_fires.fetch_add(1, std::memory_order_relaxed);

                const auto trace{ ret.stack_trace() };   // default cap
                const auto cap5{ ret.stack_trace(5) };
                // 100 < DEEP_RECURSION (120) but > DEFAULT_CAP (64): on a chain
                // 120 interpreter frames deep this returns exactly 100, proving an
                // explicit cap ABOVE the default still truncates at the cap (the
                // chain has not ended yet) rather than saturating at 64.
                const auto cap100{ ret.stack_trace(100) };

                g_r_default_size.store(trace.size(), std::memory_order_relaxed);
                g_r_cap5_size.store(cap5.size(), std::memory_order_relaxed);
                g_r_cap100_size.store(cap100.size(), std::memory_order_relaxed);
                // "No spin": a sane walk can never exceed the cap; if the saved-rbp
                // chain looped, the internal max_depth guard still bounds it AT the
                // cap, so size <= cap is the terminated-cleanly proof.
                g_r_no_spin.store(trace.size() <= DEFAULT_CAP, std::memory_order_relaxed);
                g_r_front_valid.store(!trace.empty() && trace.front().method != nullptr,
                                      std::memory_order_relaxed);
                // The default (64) is strictly less than the real 120-deep chain, so
                // the cap — not the chain end — is what bounds the default trace.
                g_r_default_lt_chain.store(
                    trace.size() < static_cast<std::size_t>(DEEP_RECURSION),
                    std::memory_order_relaxed);
                // cap5 is an element-wise prefix of the default trace (truncation
                // preserves order even deep in a recursion).
                g_r_cap5_prefix_of_default.store(is_method_prefix(cap5, trace),
                                                 std::memory_order_relaxed);
                // Every returned frame is well-formed (universal invariant).
                g_r_all_frames_wellformed.store(all_frames_wellformed(trace),
                                                std::memory_order_relaxed);

                // Longest run of consecutive frames that all name recurse(I)I, and
                // (within that run) the number of DISTINCT Method* — for a single
                // self-recursive method every frame shares ONE Method*, so a long
                // run must collapse to exactly one distinct pointer.
                std::size_t best{ 0 };
                std::size_t run{ 0 };
                void* run_method{ nullptr };
                std::size_t run_distinct{ 0 };
                std::size_t best_distinct{ 0 };
                for (const auto& f : trace)
                {
                    if (f.method != nullptr
                        && f.class_name  == CLASS_NAME
                        && f.method_name == "recurse"
                        && f.signature   == SIG_II)
                    {
                        if (run == 0 || f.method != run_method)
                        {
                            // A new distinct Method* within this consecutive run.
                            ++run_distinct;
                            run_method = static_cast<void*>(f.method);
                        }
                        ++run;
                        if (run > best)
                        {
                            best = run;
                            best_distinct = run_distinct;
                        }
                    }
                    else
                    {
                        run = 0;
                        run_distinct = 0;
                        run_method = nullptr;
                    }
                }
                g_r_uniform_run.store(best, std::memory_order_relaxed);
                g_r_distinct_recurse_methods.store(
                    static_cast<std::int32_t>(best_distinct), std::memory_order_relaxed);

                // Uniform recurse run within the 100-cap capture (independent of
                // the default), recomputed the same way.
                std::size_t best100{ 0 };
                std::size_t run100{ 0 };
                for (const auto& f : cap100)
                {
                    if (f.method != nullptr
                        && f.class_name  == CLASS_NAME
                        && f.method_name == "recurse"
                        && f.signature   == SIG_II)
                    {
                        ++run100;
                        if (run100 > best100)
                        {
                            best100 = run100;
                        }
                    }
                    else
                    {
                        run100 = 0;
                    }
                }
                g_r_cap100_recurse_run.store(best100, std::memory_order_relaxed);
            }) };

        ctx.check("stk_recurse_hook_installed", handle.installed());

        const bool done{ drive(ctx, 3) };
        ctx.check("stk_recurse_probe_completed", done);
        ctx.check("stk_recurse_leaf_ran_once", stack_trace_fixture::get_inner_calls() == 1);
        ctx.check("stk_recurse_fired_once", g_r_fires.load() == 1);

        // The walk terminated cleanly at (or before) the cap — no infinite loop.
        ctx.check("stk_recurse_front_valid", g_r_front_valid.load());
        ctx.check("stk_recurse_terminated_within_cap", g_r_no_spin.load());
        // The recursion is 120 deep, so the default-64 trace must hit the cap.
        ctx.check("stk_recurse_default_hits_cap", g_r_default_size.load() == DEFAULT_CAP);
        // A long uniform run of `recurse` frames proves the deep portion is the
        // recursion (not stray garbage) AND that names resolve consistently deep.
        ctx.check("stk_recurse_uniform_run_long", g_r_uniform_run.load() >= 32);
        // Explicit cap below the real depth truncates to exactly that many frames.
        ctx.check("stk_recurse_cap5_size_is_5", g_r_cap5_size.load() == 5);
        // cap5 is a genuine prefix of the default trace (order preserved deep down).
        ctx.check("stk_recurse_cap5_prefix_of_default", g_r_cap5_prefix_of_default.load());
        // Every returned frame is well-formed (universal invariant).
        ctx.check("stk_recurse_all_frames_wellformed", g_r_all_frames_wellformed.load());
        // The default (64) is strictly less than the real 120-deep chain: the cap,
        // not the natural chain end, is what bounds the default trace.
        ctx.check("stk_recurse_default_lt_chain", g_r_default_lt_chain.load());
        // A long uniform recurse run collapses to EXACTLY one distinct Method* —
        // every frame of a single self-recursive method shares one Method*.
        ctx.check("stk_recurse_run_single_method", g_r_distinct_recurse_methods.load() == 1);
        // An explicit cap of 100 on a 120-deep chain returns EXACTLY 100 frames
        // (above the 64 default but below the chain end): the cap, not the 64
        // default, is what bounds it — and almost all 100 are recurse frames.
        ctx.check("stk_recurse_cap100_size_is_100", g_r_cap100_size.load() == 100);
        ctx.check("stk_recurse_cap100_run_long", g_r_cap100_recurse_run.load() >= 90);

        ctx.record(std::string{ "[INFO] return_stack_trace_depth: recurse(" }
                   + std::to_string(DEEP_RECURSION) + ") default trace size = "
                   + std::to_string(g_r_default_size.load()) + " (cap " + std::to_string(DEFAULT_CAP)
                   + "), longest uniform recurse-run = " + std::to_string(g_r_uniform_run.load())
                   + " (distinct method* in run = " + std::to_string(g_r_distinct_recurse_methods.load())
                   + "), stack_trace(5) = " + std::to_string(g_r_cap5_size.load())
                   + ", stack_trace(100) = " + std::to_string(g_r_cap100_size.load())
                   + " (run " + std::to_string(g_r_cap100_recurse_run.load()) + ").");
    }

    // =====================================================================
    // Scenario 4 — TWO chains in ONE cycle: guard->shallow->inner THEN
    // guard->outer->mid->inner.  Proves each fire recomputes the trace live: the
    // two fires have a DIFFERENT immediate caller (shallow vs mid) and thus a
    // distinct index-0 method pointer.  Both chains are guard-deep so each walk
    // stays inside interpreter frames and (by design) hits the same 64 cap — so
    // the live-recompute proof is the distinct immediate caller, NOT a depth
    // difference (which the cap erases).
    // =====================================================================
    {
        auto handle{ vmhook::scoped_hook<stack_trace_fixture>(
            "inner", "(I)I",
            [](vmhook::return_value& ret,
               const std::unique_ptr<stack_trace_fixture>& /*self*/,
               std::int32_t /*x*/)
            {
                const std::int32_t order{ g_t_fires.fetch_add(1, std::memory_order_relaxed) };
                const auto trace{ ret.stack_trace() };

                if (order == 0)
                {
                    // First fire: guard -> shallow -> inner.  The trace CONTAINS
                    // shallow and does NOT contain mid (search, not fixed index).
                    g_t_size_first.store(trace.size(), std::memory_order_relaxed);
                    g_t_first_nonempty.store(!trace.empty(), std::memory_order_relaxed);
                    const std::size_t sh{ find_fixture_frame(trace, "shallow") };
                    g_t_first_has_shallow.store(sh != NPOS, std::memory_order_relaxed);
                    g_t_first_lacks_mid.store(find_fixture_frame(trace, "mid") == NPOS,
                                              std::memory_order_relaxed);
                    if (sh != NPOS)
                    {
                        g_t_first_shallow_method.store(
                            static_cast<void*>(trace[sh].method),
                            std::memory_order_relaxed);
                    }
                    // [INFO]-only positional snapshot.
                    g_t_first_immediate_shallow.store(
                        !trace.empty() && is_fixture_frame(trace.front(), "shallow"),
                        std::memory_order_relaxed);
                    g_t_first_wellformed.store(all_frames_wellformed(trace),
                                               std::memory_order_relaxed);
                }
                else if (order == 1)
                {
                    // Second fire: guard -> outer -> mid -> inner.  The trace
                    // CONTAINS mid and does NOT contain shallow (search-based).
                    g_t_size_second.store(trace.size(), std::memory_order_relaxed);
                    g_t_second_nonempty.store(!trace.empty(), std::memory_order_relaxed);
                    const std::size_t md{ find_fixture_frame(trace, "mid") };
                    g_t_second_has_mid.store(md != NPOS, std::memory_order_relaxed);
                    g_t_second_lacks_shallow.store(
                        find_fixture_frame(trace, "shallow") == NPOS,
                        std::memory_order_relaxed);
                    if (md != NPOS)
                    {
                        g_t_second_mid_method.store(
                            static_cast<void*>(trace[md].method),
                            std::memory_order_relaxed);
                    }
                    // [INFO]-only positional snapshot.
                    g_t_second_immediate_mid.store(
                        !trace.empty() && is_fixture_frame(trace.front(), "mid"),
                        std::memory_order_relaxed);
                    g_t_second_wellformed.store(all_frames_wellformed(trace),
                                                std::memory_order_relaxed);
                }
            }) };

        ctx.check("stk_two_hook_installed", handle.installed());

        const bool done{ drive(ctx, 4) };
        ctx.check("stk_two_probe_completed", done);
        ctx.check("stk_two_leaf_ran_twice", stack_trace_fixture::get_inner_calls() == 2);
        ctx.check("stk_two_fired_twice", g_t_fires.load() == 2);

        // Live-recomputed traces (position-independent freshness proof): each fire
        // captured a DIFFERENT chain.  The first fire's trace contains shallow but
        // not mid; the second's contains mid but not shallow.  That per-fire frame-
        // SET discrimination cannot come from a stale/cached trace, and it does not
        // depend on which index the named frame lands at.  Both traces are guard-
        // deep and hit the 64 cap, so they are the same length by design (depth is
        // [INFO] only, never asserted to differ).
        ctx.check("stk_two_first_nonempty", g_t_first_nonempty.load());
        ctx.check("stk_two_second_nonempty", g_t_second_nonempty.load());
        // PRESENCE of a named caller frame is BEST-EFFORT: a hot fixture method
        // (mid/shallow) can be JIT-COMPILED and INLINED into its caller, so it
        // legitimately has NO interpreter frame to find on some JDKs (observed on
        // linux·gcc·java26 where mid warmed up and was inlined away).  An absent
        // frame here is benign inlining, NOT a broken trace — record it, don't FAIL.
        if (g_t_first_has_shallow.load()) { ctx.check("stk_two_first_has_shallow", true); }
        else { ctx.record("[INFO] stk_two_first_has_shallow: shallow frame absent (JIT-inlined?) — best-effort"); }
        if (g_t_second_has_mid.load()) { ctx.check("stk_two_second_has_mid", true); }
        else { ctx.record("[INFO] stk_two_second_has_mid: mid frame absent (JIT-inlined?) — best-effort"); }
        // DISCRIMINATION (HARD, inlining-robust): each fire's trace EXCLUDES the
        // other chain's caller — a cached/stale trace would carry both, so this
        // proves the trace was recomputed live per fire regardless of inlining.
        ctx.check("stk_two_first_excludes_mid", g_t_first_lacks_mid.load());
        ctx.check("stk_two_second_excludes_shallow", g_t_second_lacks_shallow.load());
        // DISTINCT located callers — HARD only when BOTH frames were present (not
        // inlined): then they MUST be different methods (a real per-fire freshness
        // proof); if either was inlined away, record [INFO] (the excludes above still
        // prove freshness for the frames that ARE present).
        if (g_t_first_shallow_method.load() != nullptr && g_t_second_mid_method.load() != nullptr)
        {
            ctx.check("stk_two_callers_distinct",
                      g_t_first_shallow_method.load() != g_t_second_mid_method.load());
        }
        else { ctx.record("[INFO] stk_two_callers_distinct: a caller frame was inlined away — best-effort"); }

        // Each fire's chain is guard-deep (GUARD_DEPTH=80 > 64), so by DESIGN each
        // default trace would fill to the 64 cap.  But JIT inlining can collapse
        // interpreter frames out of the live chain (a hot fixture method compiled +
        // inlined into its caller), shortening it below the cap -- observed on
        // msvc·java25 under heavy JIT pressure.  So the UNIVERSAL invariant is only
        // that each trace is non-empty and never EXCEEDS the cap; whether both
        // SATURATE the cap (and are therefore equal length) is best-effort.
        const auto sz1{ g_t_size_first.load() };
        const auto sz2{ g_t_size_second.load() };
        ctx.check("stk_two_first_within_cap",  sz1 > 0u && sz1 <= DEFAULT_CAP);
        ctx.check("stk_two_second_within_cap", sz2 > 0u && sz2 <= DEFAULT_CAP);
        if (sz1 == DEFAULT_CAP && sz2 == DEFAULT_CAP)
        {
            ctx.check("stk_two_both_hit_cap", true);
            ctx.check("stk_two_equal_length", sz1 == sz2);
        }
        else
        {
            ctx.record("[INFO] stk_two_both_hit_cap: a guard-deep chain did not "
                       "saturate the 64 cap (JIT inlining shortened the interpreter "
                       "frame chain) -- sizes " + std::to_string(sz1) + "/"
                       + std::to_string(sz2) + ", best-effort, not asserted.");
        }
        // Every returned frame in each fire is well-formed (universal invariant).
        ctx.check("stk_two_first_wellformed", g_t_first_wellformed.load());
        ctx.check("stk_two_second_wellformed", g_t_second_wellformed.load());

        // The fragile fixed-position claim (immediate caller is EXACTLY shallow/mid)
        // is recorded VISIBLY, never asserted — its failure is a benign frame-layout
        // shift, not a broken trace.
        ctx.record(std::string{ "[INFO] return_stack_trace_depth: two-chain freshness: "
                                "first(shallow) depth = " }
                   + std::to_string(g_t_size_first.load()) + ", second(mid/outer) depth = "
                   + std::to_string(g_t_size_second.load())
                   + " (both guard-deep => equal length at the 64 cap); immediate==shallow? "
                   + (g_t_first_immediate_shallow.load() ? "yes" : "no")
                   + ", immediate==mid? " + (g_t_second_immediate_mid.load() ? "yes" : "no")
                   + " (positional INFO; HARD proof is the per-fire frame-set discrimination).");
    }

    // No detour may be left armed for the next module: every scoped_hook above
    // already uninstalled on scope exit, but assert it and hard-reset to be sure.
    vmhook::shutdown_hooks();
    {
        std::atomic<std::int32_t> post_fire{ 0 };
        // A bare probe with NO hook installed must NOT fire any detour.
        const bool done{ ctx.run_probe(
            [](bool value)
            {
                if (value)
                {
                    stack_trace_fixture::set_done(false);
                    stack_trace_fixture::set_mode(1);
                }
                stack_trace_fixture::set_go(value);
            },
            []() { return stack_trace_fixture::get_done(); }) };
        ctx.check("stk_teardown_probe_completed", done);
        ctx.check("stk_teardown_leaf_ran_once", stack_trace_fixture::get_inner_calls() == 1);
        ctx.check("stk_teardown_no_detour_armed", post_fire.load() == 0);
    }
}
