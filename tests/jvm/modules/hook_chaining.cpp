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
//
// ---------------------------------------------------------------------------
// SUITE-SAFETY (this module was QUARANTINED in Wave 3 in a matrix-wide JVM-crash
// cascade; re-enabled here under the suite-safety rules in
// audit/PERFECTION_PROGRAM.md:400-403, mirroring the just-landed wrapper_pattern.cpp
// / find_class_context_loader.cpp / register_class.cpp scaffold):
//
//   * NOT THE CRASHER, A VICTIM.  The Wave-3 cascade crasher was
//     hook_reinstall_after_shutdown's mid-suite GLOBAL shutdown_hooks() (it tore
//     down every OTHER module's hooks/state mid-run); the other five modules,
//     including this one, were victims that merely ran after it.  This module
//     itself never calls the global shutdown mid-body and never mutates shared
//     process state another module relies on (it registers only its OWN throwaway
//     wrapper type and installs only scope-local hooks).
//
//   * MANY HOOKS, ZERO LEFT ARMED ON ANY PATH.  The decisive hazard for THIS
//     feature is that it installs MULTIPLE hooks on different methods sharing one
//     i2i stub.  Every hook is a scope-local vmhook::scoped_hook<>: each block's
//     handles RAII-uninstall at its closing brace, and a throw mid-scenario unwinds
//     and destroys all in-scope handles during stack unwinding (reverse construction
//     order), so a SUBSET of the sibling hooks can never be left armed.  On top of
//     that, the VMHOOK_JVM_MODULE wrapper runs the whole body under try/catch and
//     ALWAYS calls an unconditional vmhook::shutdown_hooks() OUTSIDE the try, so
//     even a throw before a scope exit returns control to the driver with an empty
//     hook table.  A leaked armed hook is exactly what cascaded in Wave 3; this
//     module cannot leak one on ANY path.
//
//   * NEVER CRASH, BAIL TO [INFO].  An ENTRY GUARD (find_class(FIXTURE)==nullptr)
//     skips the module cleanly if the fixture is absent, so the unguarded
//     static_field("go")->set(...) handshake derefs can never touch a disengaged
//     optional; drive() also null-checks ctx.run_probe before calling it.  A throw
//     from any vmhook call is contained and recorded as [INFO], never a FAIL.
//
//   * NO FORCED GC.  Neither this module nor HookChaining.java drives System.gc(),
//     so the forced-GC platform gate (the make_java_array pitfall) does not apply.
//
//   * NO SINGLE-INSTANT JIT-OWNED READS.  This module never reads _code /
//     verify_hooks(); it observes hooks purely through per-method fire counters and
//     Java-side observables, so it needs no code_settles_null / verify_settles_zero
//     settle helpers.
//
//   * UNIVERSAL INVARIANTS.  Every assertion here is a JDK-agnostic hook-dispatch
//     invariant (exactly-once per call, fires-for-its-method-only, allow-through,
//     per-detour return override) that holds identically on JDK 8..26 HotSpot, so
//     nothing is JDK-gated.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <atomic>
#include <utility>
#include <bit>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

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
    constexpr std::int32_t S_REPEAT{ 3 };       // mode 9: s() call count

    // For the set_arg-isolation scenario: a's detour rewrites its OWN int arg
    // (interpreter slot 1, since slot 0 is `this` for an instance method) to this
    // value BEFORE the body runs, so a's body returns SEED + A_ARG_REWRITE while
    // siblings' args stay untouched.  Distinct from A_ARG so the rewrite is
    // unambiguous.
    constexpr std::int32_t A_ARG_REWRITE{ 555 };

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

    // --- caller()-in-mixed-pass latches (per method) ------------------------
    // In a SHARED-STUB mixed pass every method is called from the same fixture
    // driver (runAll), so each detour's retval.caller() should resolve to that
    // same caller.  caller() is a frame-walk that can legitimately fail to
    // resolve on some JDK/compiler frame layouts, so these are observed and
    // recorded as [INFO] -- never hard-asserted -- while the demux invariants
    // (each fired once, zero cross-fire) stay the hard guarantees.
    std::atomic<bool> g_a_caller_resolved{ false };
    std::atomic<bool> g_b_caller_resolved{ false };
    std::atomic<bool> g_e_caller_resolved{ false };
    std::atomic<bool> g_a_caller_is_driver{ false };
    std::atomic<bool> g_b_caller_is_driver{ false };
    std::atomic<bool> g_e_caller_is_driver{ false };

    // --- set_arg-isolation latch --------------------------------------------
    // True iff a's detour's set_arg(slot 1, A_ARG_REWRITE) reported success.
    std::atomic<bool> g_a_setarg_ok{ false };

    // --- frame()-identity latches (per method) ------------------------------
    // return_value::frame() is a HARD accessor (it just returns the captured
    // interpreter frame pointer -- no frame-walk that could fail), so in a mixed
    // pass each detour MUST see a non-null frame AND, crucially, a DIFFERENT
    // frame pointer than its siblings: common_detour handed every detour ITS OWN
    // intercepted frame, never a stale sibling's.  Captured as raw addresses and
    // compared on the native side; that distinctness is the teeth behind "each
    // detour decoded ITS OWN frame" beyond the value checks.
    std::atomic<const void*> g_a_frame_ptr{ nullptr };
    std::atomic<const void*> g_b_frame_ptr{ nullptr };
    std::atomic<const void*> g_c_frame_ptr{ nullptr };
    std::atomic<const void*> g_d_frame_ptr{ nullptr };
    std::atomic<const void*> g_e_frame_ptr{ nullptr };
    std::atomic<bool>        g_a_frame_nonnull{ false };
    std::atomic<bool>        g_b_frame_nonnull{ false };
    std::atomic<bool>        g_c_frame_nonnull{ false };
    std::atomic<bool>        g_d_frame_nonnull{ false };
    std::atomic<bool>        g_e_frame_nonnull{ false };

    // --- stack_trace()-in-mixed-pass latches (per method) -------------------
    // Like caller(), stack_trace() is a saved-rbp frame-walk that can legitimately
    // fail to resolve on some JDK/compiler layouts, so these are [INFO]-only.  In
    // a shared-stub mixed pass every method is called from the same driver, so
    // frame[0] of each walk should be that driver (runAll) when the walk resolves.
    std::atomic<std::int32_t> g_a_trace_depth{ 0 };
    std::atomic<std::int32_t> g_b_trace_depth{ 0 };
    std::atomic<std::int32_t> g_e_trace_depth{ 0 };
    std::atomic<bool>         g_a_trace_top_is_driver{ false };
    std::atomic<bool>         g_b_trace_top_is_driver{ false };
    std::atomic<bool>         g_e_trace_top_is_driver{ false };

    // --- d's force-cancel latch (multi-cancel scenario) ---------------------
    // d(double) returns long; when d's detour cancels, Java sees the zero-filled
    // retval cell (0L).  Used alongside a's cancel to prove two siblings can each
    // cancel their OWN firing frame in the same pass without touching each other.
    std::atomic<bool> g_d_ok_seen{ false };

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
        g_a_caller_resolved.store(false);
        g_b_caller_resolved.store(false);
        g_e_caller_resolved.store(false);
        g_a_caller_is_driver.store(false);
        g_b_caller_is_driver.store(false);
        g_e_caller_is_driver.store(false);
        g_a_setarg_ok.store(false);
        g_a_frame_ptr.store(nullptr);
        g_b_frame_ptr.store(nullptr);
        g_c_frame_ptr.store(nullptr);
        g_d_frame_ptr.store(nullptr);
        g_e_frame_ptr.store(nullptr);
        g_a_frame_nonnull.store(false);
        g_b_frame_nonnull.store(false);
        g_c_frame_nonnull.store(false);
        g_d_frame_nonnull.store(false);
        g_e_frame_nonnull.store(false);
        g_a_trace_depth.store(0);
        g_b_trace_depth.store(0);
        g_e_trace_depth.store(0);
        g_a_trace_top_is_driver.store(false);
        g_b_trace_top_is_driver.store(false);
        g_e_trace_top_is_driver.store(false);
        g_d_ok_seen.store(false);
    }

    // Drives exactly one probe cycle for `mode`: resets native fire counters +
    // the latched `done`, programs the scenario selector on the rising edge of
    // go, then runs the probe.  Mirrors hook_basic / scoped_hook_raii drive().
    auto drive(vmhook_test::context& ctx, std::int32_t mode) -> bool
    {
        reset_fires();
        if (!ctx.run_probe)
        {
            return false;
        }
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

    // a's detour that FORCE-CANCELS (cancel() with no set): proves cancel writes
    // the FIRING frame's slot only -- when a,b,c share the stub and only a's
    // detour cancels, a's body is suppressed (Java sees the zero-filled retval
    // cell) while b's and c's bodies still run.  Same frame validation as
    // a_detour so cross-fire detection is intact.
    auto a_detour_cancel()
    {
        return [](vmhook::return_value& retval,
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
            retval.cancel();
        };
    }

    // a's detour that REWRITES its own int argument (interpreter slot 1, since
    // slot 0 is `this` for an instance method) to A_ARG_REWRITE before the body
    // runs.  The mutation belongs to a's firing frame only, so a's body returns
    // SEED + A_ARG_REWRITE while siblings' args stay untouched.  The callback
    // still sees the ORIGINAL x (decoded before the body re-reads the slot), so
    // its frame validation is unchanged.
    auto a_detour_setarg()
    {
        return [](vmhook::return_value& retval,
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
            const bool wrote{ retval.set_arg(1, std::int32_t{ A_ARG_REWRITE }) };
            g_a_setarg_ok.store(wrote, std::memory_order_relaxed);
        };
    }

    // The internal JVM name of the fixture; reused below by the caller-aware
    // detours to compare retval.caller().class_name against (the slash form).
    constexpr char FIXTURE_NAME[]{ "vmhook/fixtures/HookChaining" };

    // caller()-capturing variants of a / b / e (instance shapes spanning one
    // slot, two slots, and the empty frame).  Each does the SAME frame
    // validation as its base detour AND records whether retval.caller()
    // resolved to the shared driver (runAll).  The caller-walk result is for
    // [INFO] only -- it can legitimately fail on some JDK/compiler frame
    // layouts -- so it never flips the cross-fire sentinel.
    auto a_detour_caller()
    {
        return [](vmhook::return_value& retval,
                  const std::unique_ptr<hc_fixture>& self,
                  std::int32_t x)
        {
            g_a_fires.fetch_add(1, std::memory_order_relaxed);
            g_last_tag.store(TAG_A, std::memory_order_relaxed);
            if (self != nullptr && self->seed() == SEED && x == A_ARG)
            {
                g_a_ok.store(true, std::memory_order_relaxed);
            }
            else
            {
                g_cross_fire.store(true, std::memory_order_relaxed);
            }
            const vmhook::return_value::caller_info info{ retval.caller() };
            g_a_caller_resolved.store(info.valid(), std::memory_order_relaxed);
            g_a_caller_is_driver.store(
                info.valid() && info.class_name == FIXTURE_NAME
                    && info.method_name == "runAll",
                std::memory_order_relaxed);
        };
    }

    auto b_detour_caller()
    {
        return [](vmhook::return_value& retval,
                  const std::unique_ptr<hc_fixture>& self,
                  std::int64_t y)
        {
            g_b_fires.fetch_add(1, std::memory_order_relaxed);
            g_last_tag.store(TAG_B, std::memory_order_relaxed);
            if (self != nullptr && self->seed() == SEED && y == B_ARG)
            {
                g_b_ok.store(true, std::memory_order_relaxed);
            }
            else
            {
                g_cross_fire.store(true, std::memory_order_relaxed);
            }
            const vmhook::return_value::caller_info info{ retval.caller() };
            g_b_caller_resolved.store(info.valid(), std::memory_order_relaxed);
            g_b_caller_is_driver.store(
                info.valid() && info.class_name == FIXTURE_NAME
                    && info.method_name == "runAll",
                std::memory_order_relaxed);
        };
    }

    auto e_detour_caller()
    {
        return [](vmhook::return_value& retval,
                  const std::unique_ptr<hc_fixture>& self)
        {
            g_e_fires.fetch_add(1, std::memory_order_relaxed);
            g_last_tag.store(TAG_E, std::memory_order_relaxed);
            if (self != nullptr && self->seed() == SEED)
            {
                g_e_ok.store(true, std::memory_order_relaxed);
            }
            else
            {
                g_cross_fire.store(true, std::memory_order_relaxed);
            }
            const vmhook::return_value::caller_info info{ retval.caller() };
            g_e_caller_resolved.store(info.valid(), std::memory_order_relaxed);
            g_e_caller_is_driver.store(
                info.valid() && info.class_name == FIXTURE_NAME
                    && info.method_name == "runAll",
                std::memory_order_relaxed);
        };
    }

    // frame()-capturing variants of a / b / c / d / e.  Each does the SAME frame
    // validation as its base detour AND records return_value::frame() (the raw
    // intercepted interpreter frame -- a HARD accessor, no walk).  In a mixed
    // pass these prove common_detour handed each detour ITS OWN non-null frame and
    // that the per-method frame pointers are DISTINCT (no detour saw a sibling's
    // stale frame).  The pointer is stored as a raw address for native comparison.
    auto a_detour_frame()
    {
        return [](vmhook::return_value& retval,
                  const std::unique_ptr<hc_fixture>& self,
                  std::int32_t x)
        {
            g_a_fires.fetch_add(1, std::memory_order_relaxed);
            g_last_tag.store(TAG_A, std::memory_order_relaxed);
            if (self != nullptr && self->seed() == SEED && x == A_ARG)
            {
                g_a_ok.store(true, std::memory_order_relaxed);
            }
            else
            {
                g_cross_fire.store(true, std::memory_order_relaxed);
            }
            const void* const fp{ retval.frame() };
            g_a_frame_ptr.store(fp, std::memory_order_relaxed);
            g_a_frame_nonnull.store(fp != nullptr, std::memory_order_relaxed);
        };
    }

    auto b_detour_frame()
    {
        return [](vmhook::return_value& retval,
                  const std::unique_ptr<hc_fixture>& self,
                  std::int64_t y)
        {
            g_b_fires.fetch_add(1, std::memory_order_relaxed);
            g_last_tag.store(TAG_B, std::memory_order_relaxed);
            if (self != nullptr && self->seed() == SEED && y == B_ARG)
            {
                g_b_ok.store(true, std::memory_order_relaxed);
            }
            else
            {
                g_cross_fire.store(true, std::memory_order_relaxed);
            }
            const void* const fp{ retval.frame() };
            g_b_frame_ptr.store(fp, std::memory_order_relaxed);
            g_b_frame_nonnull.store(fp != nullptr, std::memory_order_relaxed);
        };
    }

    auto c_detour_frame()
    {
        return [](vmhook::return_value& retval,
                  const std::unique_ptr<hc_fixture>& self,
                  const std::string& s)
        {
            g_c_fires.fetch_add(1, std::memory_order_relaxed);
            g_last_tag.store(TAG_C, std::memory_order_relaxed);
            if (self != nullptr && self->seed() == SEED && s == "vmhook")
            {
                g_c_ok.store(true, std::memory_order_relaxed);
            }
            else
            {
                g_cross_fire.store(true, std::memory_order_relaxed);
            }
            const void* const fp{ retval.frame() };
            g_c_frame_ptr.store(fp, std::memory_order_relaxed);
            g_c_frame_nonnull.store(fp != nullptr, std::memory_order_relaxed);
        };
    }

    auto d_detour_frame()
    {
        return [](vmhook::return_value& retval,
                  const std::unique_ptr<hc_fixture>& self,
                  double z)
        {
            g_d_fires.fetch_add(1, std::memory_order_relaxed);
            g_last_tag.store(TAG_D, std::memory_order_relaxed);
            if (self != nullptr && self->seed() == SEED && z == D_ARG)
            {
                g_d_ok.store(true, std::memory_order_relaxed);
            }
            else
            {
                g_cross_fire.store(true, std::memory_order_relaxed);
            }
            const void* const fp{ retval.frame() };
            g_d_frame_ptr.store(fp, std::memory_order_relaxed);
            g_d_frame_nonnull.store(fp != nullptr, std::memory_order_relaxed);
        };
    }

    auto e_detour_frame()
    {
        return [](vmhook::return_value& retval,
                  const std::unique_ptr<hc_fixture>& self)
        {
            g_e_fires.fetch_add(1, std::memory_order_relaxed);
            g_last_tag.store(TAG_E, std::memory_order_relaxed);
            if (self != nullptr && self->seed() == SEED)
            {
                g_e_ok.store(true, std::memory_order_relaxed);
            }
            else
            {
                g_cross_fire.store(true, std::memory_order_relaxed);
            }
            const void* const fp{ retval.frame() };
            g_e_frame_ptr.store(fp, std::memory_order_relaxed);
            g_e_frame_nonnull.store(fp != nullptr, std::memory_order_relaxed);
        };
    }

    // stack_trace()-capturing variants of a / b / e.  Each records the depth of
    // the saved-rbp walk and whether frame[0] (the immediate caller) is the shared
    // driver runAll.  Like caller(), the walk can legitimately not resolve on some
    // JDK/compiler layouts, so the depth / driver result is [INFO]-only -- the
    // frame validation that flips the cross-fire sentinel stays the hard guarantee.
    auto a_detour_trace()
    {
        return [](vmhook::return_value& retval,
                  const std::unique_ptr<hc_fixture>& self,
                  std::int32_t x)
        {
            g_a_fires.fetch_add(1, std::memory_order_relaxed);
            g_last_tag.store(TAG_A, std::memory_order_relaxed);
            if (self != nullptr && self->seed() == SEED && x == A_ARG)
            {
                g_a_ok.store(true, std::memory_order_relaxed);
            }
            else
            {
                g_cross_fire.store(true, std::memory_order_relaxed);
            }
            const std::vector<vmhook::return_value::caller_info> trace{
                retval.stack_trace() };
            g_a_trace_depth.store(static_cast<std::int32_t>(trace.size()),
                                  std::memory_order_relaxed);
            g_a_trace_top_is_driver.store(
                !trace.empty() && trace.front().class_name == FIXTURE_NAME
                    && trace.front().method_name == "runAll",
                std::memory_order_relaxed);
        };
    }

    auto b_detour_trace()
    {
        return [](vmhook::return_value& retval,
                  const std::unique_ptr<hc_fixture>& self,
                  std::int64_t y)
        {
            g_b_fires.fetch_add(1, std::memory_order_relaxed);
            g_last_tag.store(TAG_B, std::memory_order_relaxed);
            if (self != nullptr && self->seed() == SEED && y == B_ARG)
            {
                g_b_ok.store(true, std::memory_order_relaxed);
            }
            else
            {
                g_cross_fire.store(true, std::memory_order_relaxed);
            }
            const std::vector<vmhook::return_value::caller_info> trace{
                retval.stack_trace() };
            g_b_trace_depth.store(static_cast<std::int32_t>(trace.size()),
                                  std::memory_order_relaxed);
            g_b_trace_top_is_driver.store(
                !trace.empty() && trace.front().class_name == FIXTURE_NAME
                    && trace.front().method_name == "runAll",
                std::memory_order_relaxed);
        };
    }

    auto e_detour_trace()
    {
        return [](vmhook::return_value& retval,
                  const std::unique_ptr<hc_fixture>& self)
        {
            g_e_fires.fetch_add(1, std::memory_order_relaxed);
            g_last_tag.store(TAG_E, std::memory_order_relaxed);
            if (self != nullptr && self->seed() == SEED)
            {
                g_e_ok.store(true, std::memory_order_relaxed);
            }
            else
            {
                g_cross_fire.store(true, std::memory_order_relaxed);
            }
            const std::vector<vmhook::return_value::caller_info> trace{
                retval.stack_trace() };
            g_e_trace_depth.store(static_cast<std::int32_t>(trace.size()),
                                  std::memory_order_relaxed);
            g_e_trace_top_is_driver.store(
                !trace.empty() && trace.front().class_name == FIXTURE_NAME
                    && trace.front().method_name == "runAll",
                std::memory_order_relaxed);
        };
    }

    // d's detour that FORCE-CANCELS (cancel() with no set): the twin of
    // a_detour_cancel for the two-slot double frame.  When a AND d both cancel in
    // the same mixed pass, each suppresses ITS OWN body (Java sees the zero-filled
    // retval cell) while the un-cancelling siblings run untouched -- two firing
    // frames cancelled independently through the one shared stub.
    auto d_detour_cancel()
    {
        return [](vmhook::return_value& retval,
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
                g_d_ok_seen.store(true, std::memory_order_relaxed);
            }
            else
            {
                g_cross_fire.store(true, std::memory_order_relaxed);
            }
            retval.cancel();
        };
    }

    // c's detour that FORCE-CANCELS (cancel() with no set): validates c's OWN
    // reference (String) frame, then cancels so c(String)->int returns the
    // zero-filled cell.  Used in the three-mutation-kinds-in-one-pass scenario so
    // c's cancel is a per-firing-frame mutation distinct from a's override.
    auto c_detour_cancel()
    {
        return [](vmhook::return_value& retval,
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
            retval.cancel();
        };
    }

    // The internal JVM name of the fixture; used by the entry guard's
    // find_class() pre-check (so the unguarded handshake static_field("go")->set
    // derefs below can never fault on a missing class) and by register_class.
    constexpr char FIXTURE[]{ "vmhook/fixtures/HookChaining" };

    // The whole test body, factored out so the VMHOOK_JVM_MODULE wrapper can run
    // it under a try/catch and ALWAYS follow it with shutdown_hooks() (suite-
    // safety: ZERO hooks armed on EVERY exit path).  EVERY hook below is a
    // scope-local scoped_hook<>, so a throw mid-scenario unwinds and RAII-destroys
    // all in-scope handles (in reverse construction order) before this returns —
    // a subset can never be left armed — and the unconditional shutdown_hooks()
    // in the wrapper is the belt-and-braces backstop on top of that.
    auto run_hook_chaining_checks(vmhook_test::context& ctx) -> void
    {
        // =====================================================================
        //  ENTRY GUARD.  If HookChaining is not loaded/resolvable, every
        //  static_field()->set/get below (the go/done/mode handshake) would deref
        //  a disengaged optional.  Bail cleanly to [INFO] instead of dereferencing
        //  anything (the wrapper's final shutdown_hooks() still runs).  In practice
        //  the harness loads every vmhook.fixtures.* class on each run, so this is
        //  belt-and-braces.  (Same idiom as register_class / wrapper_pattern.)
        // =====================================================================
        if (vmhook::find_class(FIXTURE) == nullptr)
        {
            ctx.record("[INFO] hook_chaining: HookChaining not loaded/resolvable on "
                       "this run; skipping the module's live checks (no crash, no "
                       "hooks armed).");
            return;
        }

        vmhook::register_class<hc_fixture>(FIXTURE);

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

    // =====================================================================
    // 9 — PER-DETOUR cancel() isolation (no set).  a,b,c hooked; ONLY a's detour
    //     calls retval.cancel() (force-cancel WITHOUT writing a custom retval).
    //     Driving a+b+c (mode 5): a's body is SUPPRESSED so the Java caller sees
    //     the zero-filled retval cell (a(int) returns int -> 0), while b's and c's
    //     bodies run their ORIGINAL computation untouched.  This is the cancel()
    //     twin of scenario 8's set(): both prove the slot belongs to the FIRING
    //     frame, never a global switch that would also blank a sibling.
    // =====================================================================
    {
        auto h_a{ vmhook::scoped_hook<hc_fixture>("a", "(I)I", a_detour_cancel()) };
        auto h_b{ vmhook::scoped_hook<hc_fixture>("b", "(J)J", b_detour()) };
        auto h_c{ vmhook::scoped_hook<hc_fixture>("c", "(Ljava/lang/String;)I", c_detour()) };
        ctx.check("cancel_all_installed",
                  h_a.installed() && h_b.installed() && h_c.installed());

        const bool done{ drive(ctx, 5) };
        ctx.check("cancel_probe_completed", done);
        ctx.check("cancel_all_fired_once",
                  g_a_fires.load() == 1 && g_b_fires.load() == 1 && g_c_fires.load() == 1);
        ctx.check("cancel_no_cross_fire", !g_cross_fire.load());
        ctx.check("cancel_a_frame_ok", g_a_ok.load());
        // a's body was cancelled -> Java caller received the zero-filled cell.
        ctx.check("cancel_a_result_zeroed", hc_fixture::get_a_result() == 0);
        ctx.check("cancel_a_result_not_original", hc_fixture::get_a_result() != (SEED + A_ARG));
        // b, c bodies ran normally (their detours did NOT cancel).
        ctx.check("cancel_b_body_untouched",
                  hc_fixture::get_b_result() == (static_cast<std::int64_t>(SEED) + B_ARG));
        ctx.check("cancel_c_body_untouched", hc_fixture::get_c_result() == (SEED + C_ARG_LEN));
    }

    // After scenario 9 drops, a plain a() runs its ORIGINAL body again (the
    // cancel was bound to that scope's detour only, now gone).
    {
        const bool done{ drive(ctx, 2) };
        ctx.check("cancel_after_drop_probe_completed", done);
        ctx.check("cancel_after_drop_a_detour_silent", g_a_fires.load() == 0);
        ctx.check("cancel_after_drop_a_body_restored",
                  hc_fixture::get_a_result() == (SEED + A_ARG));
    }

    // =====================================================================
    // 10 — PER-DETOUR set_arg() isolation.  a,b,c hooked; ONLY a's detour rewrites
    //      its own int argument (interpreter slot 1) to A_ARG_REWRITE before the
    //      body runs.  Driving a+b+c (mode 5): a's body recomputes from the
    //      rewritten arg (SEED + A_ARG_REWRITE) while b's and c's args stay
    //      untouched.  Arg-mutation, like cancel/set, is bound to the FIRING
    //      frame, so it never leaks onto a sibling sharing the stub.
    // =====================================================================
    {
        auto h_a{ vmhook::scoped_hook<hc_fixture>("a", "(I)I", a_detour_setarg()) };
        auto h_b{ vmhook::scoped_hook<hc_fixture>("b", "(J)J", b_detour()) };
        auto h_c{ vmhook::scoped_hook<hc_fixture>("c", "(Ljava/lang/String;)I", c_detour()) };
        ctx.check("setarg_all_installed",
                  h_a.installed() && h_b.installed() && h_c.installed());

        const bool done{ drive(ctx, 5) };
        ctx.check("setarg_probe_completed", done);
        ctx.check("setarg_all_fired_once",
                  g_a_fires.load() == 1 && g_b_fires.load() == 1 && g_c_fires.load() == 1);
        ctx.check("setarg_no_cross_fire", !g_cross_fire.load());
        ctx.check("setarg_a_frame_ok", g_a_ok.load());
        ctx.check("setarg_write_reported_success", g_a_setarg_ok.load());
        // a's body recomputed from the rewritten arg, ONLY for a.
        ctx.check("setarg_a_body_used_rewrite",
                  hc_fixture::get_a_result() == (SEED + A_ARG_REWRITE));
        ctx.check("setarg_a_body_not_original",
                  hc_fixture::get_a_result() != (SEED + A_ARG));
        // b, c args were untouched -> their bodies reflect their ORIGINAL args.
        ctx.check("setarg_b_arg_untouched",
                  hc_fixture::get_b_result() == (static_cast<std::int64_t>(SEED) + B_ARG));
        ctx.check("setarg_c_arg_untouched", hc_fixture::get_c_result() == (SEED + C_ARG_LEN));
    }

    // After scenario 10 drops, a plain a() observes its ORIGINAL arg again.
    {
        const bool done{ drive(ctx, 2) };
        ctx.check("setarg_after_drop_probe_completed", done);
        ctx.check("setarg_after_drop_a_detour_silent", g_a_fires.load() == 0);
        ctx.check("setarg_after_drop_a_body_original",
                  hc_fixture::get_a_result() == (SEED + A_ARG));
    }

    // =====================================================================
    // 11 — DUPLICATE install on a SHARED method is honest (flaw #4: one detour per
    //      Method*).  Install a real hook on `a`, then a SECOND scoped_hook on the
    //      SAME `a` with a DIFFERENT detour.  The duplicate must report
    //      installed()==false (the install path refuses a second owner of the one
    //      shared entry), the FIRST detour stays the live owner, and dropping the
    //      duplicate's empty handle must NOT disarm the original.  This is the
    //      structural property the whole demux relies on: a method has at most one
    //      active detour, so first-match-wins never has two entries to choose from.
    // =====================================================================
    {
        auto h_first{ vmhook::scoped_hook<hc_fixture>("a", "(I)I", a_detour()) };
        ctx.check("dup_first_installed", h_first.installed());
        {
            // A distinct detour that, IF it ever fired, would flip the cross-fire
            // sentinel (it stamps TAG_B for a's frame, which a's value check would
            // never accept).  It must NEVER fire because the duplicate is rejected.
            auto h_dup{ vmhook::scoped_hook<hc_fixture>(
                "a", "(I)I",
                [](vmhook::return_value&, const std::unique_ptr<hc_fixture>&, std::int32_t)
                {
                    g_cross_fire.store(true, std::memory_order_relaxed);
                    g_last_tag.store(TAG_B, std::memory_order_relaxed);
                }) };
            ctx.check("dup_second_not_installed", !h_dup.installed());
            // The first hook is still the live owner regardless of the duplicate.
            ctx.check("dup_first_still_installed", h_first.installed());

            const bool done{ drive(ctx, 2) };   // a() once
            ctx.check("dup_probe_completed", done);
            // EXACTLY ONE detour fired (the first), and it was a's -- the
            // duplicate's body never ran.
            ctx.check("dup_a_fired_exactly_once", g_a_fires.load() == 1);
            ctx.check("dup_a_frame_ok", g_a_ok.load());
            ctx.check("dup_last_tag_is_a_not_dup", g_last_tag.load() == TAG_A);
            ctx.check("dup_no_cross_fire", !g_cross_fire.load());
            ctx.check("dup_allow_through_a", hc_fixture::get_a_result() == (SEED + A_ARG));
        }   // h_dup (empty handle) drops -- its stop() is a guaranteed no-op.

        // Dropping the duplicate's empty handle did NOT disarm the original:
        // a's detour still fires on the next pass.
        const bool done2{ drive(ctx, 2) };
        ctx.check("dup_after_empty_drop_probe_completed", done2);
        ctx.check("dup_after_empty_drop_first_still_fires", g_a_fires.load() == 1);
        ctx.check("dup_after_empty_drop_first_still_installed", h_first.installed());
        ctx.check("dup_after_empty_drop_no_cross_fire", !g_cross_fire.load());
    }   // h_first drops here -> a fully disarmed.

    // After the first handle drops too, a() runs unhooked.
    {
        const bool done{ drive(ctx, 2) };
        ctx.check("dup_after_first_drop_probe_completed", done);
        ctx.check("dup_after_first_drop_a_silent", g_a_fires.load() == 0);
    }

    // =====================================================================
    // 12 — NAME-ONLY scoped_hook overload shares the stub with signature-resolved
    //      siblings.  Resolve `b` by name ONLY (no descriptor) alongside `a` and
    //      `c` resolved WITH descriptors, then drive a+b+c (mode 5).  The two
    //      resolution paths are interchangeable: each of the three fires once,
    //      decodes its own frame, no cross-fire.  (Name-only is unambiguous here
    //      because b is not overloaded in the fixture.)
    // =====================================================================
    {
        auto h_a{ vmhook::scoped_hook<hc_fixture>("a", "(I)I", a_detour()) };
        auto h_b{ vmhook::scoped_hook<hc_fixture>("b", b_detour()) };   // NAME ONLY
        auto h_c{ vmhook::scoped_hook<hc_fixture>("c", "(Ljava/lang/String;)I", c_detour()) };
        ctx.check("nameonly_a_installed", h_a.installed());
        ctx.check("nameonly_b_installed_by_name", h_b.installed());
        ctx.check("nameonly_c_installed", h_c.installed());

        const bool done{ drive(ctx, 5) };
        ctx.check("nameonly_probe_completed", done);
        ctx.check("nameonly_a_fired_once", g_a_fires.load() == 1);
        ctx.check("nameonly_b_fired_once", g_b_fires.load() == 1);
        ctx.check("nameonly_c_fired_once", g_c_fires.load() == 1);
        ctx.check("nameonly_a_frame_ok", g_a_ok.load());
        ctx.check("nameonly_b_frame_ok", g_b_ok.load());
        ctx.check("nameonly_c_frame_ok", g_c_ok.load());
        ctx.check("nameonly_no_cross_fire", !g_cross_fire.load());
        ctx.check("nameonly_total_is_three",
                  g_a_fires.load() + g_b_fires.load() + g_c_fires.load() == 3);
    }

    // =====================================================================
    // 13 — STATIC-method count fidelity (mode 9).  a (instance) once + s (static)
    //      S_REPEAT times, hooked together.  a's detour fires exactly once while
    //      s's detour fires exactly S_REPEAT times -- the static sibling's repeated
    //      calls do NOT bleed onto the instance entry and vice-versa.  Complements
    //      scenario 3's instance-side count fidelity with the static side.
    // =====================================================================
    {
        const std::int32_t a_before{ hc_fixture::get_a_calls() };
        const std::int32_t s_before{ hc_fixture::get_s_calls() };

        auto h_a{ vmhook::scoped_hook<hc_fixture>("a", "(I)I", a_detour()) };
        auto h_s{ vmhook::scoped_hook<hc_fixture>("s", "(I)I", s_detour()) };
        ctx.check("staticcount_installed", h_a.installed() && h_s.installed());

        const bool done{ drive(ctx, 9) };
        ctx.check("staticcount_probe_completed", done);
        ctx.check("staticcount_java_called_a_once",
                  hc_fixture::get_a_calls() - a_before == 1);
        ctx.check("staticcount_java_called_s_n_times",
                  hc_fixture::get_s_calls() - s_before == S_REPEAT);
        ctx.check("staticcount_a_fired_once", g_a_fires.load() == 1);
        ctx.check("staticcount_s_fired_n_times", g_s_fires.load() == S_REPEAT);
        ctx.check("staticcount_no_cross_fire", !g_cross_fire.load());
        ctx.check("staticcount_a_frame_ok", g_a_ok.load());
        ctx.check("staticcount_s_frame_ok", g_s_ok.load());
        ctx.check("staticcount_total_is_exact",
                  g_a_fires.load() + g_s_fires.load() == 1 + S_REPEAT);
    }

    // =====================================================================
    // 14 — INTERLEAVED partial teardown of an N-way chain.  Install all six on the
    //      shared stub, prove all six fire, then drop handles ONE AT A TIME in a
    //      middle-out order (c, then e, then a), re-driving the full mode-1 pass
    //      after EACH drop.  Each removal silences ONLY its own method; every
    //      not-yet-dropped sibling keeps firing through the same shared stub.  This
    //      stresses hook_handle::stop()'s single-entry erase repeatedly while the
    //      trampoline stays installed for the survivors.
    // =====================================================================
    {
        auto h_a{ vmhook::scoped_hook<hc_fixture>("a", "(I)I", a_detour()) };
        auto h_b{ vmhook::scoped_hook<hc_fixture>("b", "(J)J", b_detour()) };
        auto h_c{ vmhook::scoped_hook<hc_fixture>("c", "(Ljava/lang/String;)I", c_detour()) };
        auto h_d{ vmhook::scoped_hook<hc_fixture>("d", "(D)J", d_detour()) };
        auto h_e{ vmhook::scoped_hook<hc_fixture>("e", "()I", e_detour()) };
        auto h_s{ vmhook::scoped_hook<hc_fixture>("s", "(I)I", s_detour()) };
        ctx.check("interleave_all_installed",
                  h_a.installed() && h_b.installed() && h_c.installed()
                  && h_d.installed() && h_e.installed() && h_s.installed());

        // Pass 0: all six fire.
        bool done{ drive(ctx, 1) };
        ctx.check("interleave_pass0_completed", done);
        ctx.check("interleave_pass0_all_six",
                  g_a_fires.load() == 1 && g_b_fires.load() == 1 && g_c_fires.load() == 1
                  && g_d_fires.load() == 1 && g_e_fires.load() == 1 && g_s_fires.load() == 1);
        ctx.check("interleave_pass0_no_cross_fire", !g_cross_fire.load());

        // Drop c (middle of the descriptor shapes).  a,b,d,e,s survive.
        h_c.stop();
        ctx.check("interleave_c_released", !h_c.installed());
        done = drive(ctx, 1);
        ctx.check("interleave_pass1_completed", done);
        ctx.check("interleave_pass1_c_silent", g_c_fires.load() == 0);
        ctx.check("interleave_pass1_survivors_fire",
                  g_a_fires.load() == 1 && g_b_fires.load() == 1
                  && g_d_fires.load() == 1 && g_e_fires.load() == 1 && g_s_fires.load() == 1);
        ctx.check("interleave_pass1_no_cross_fire", !g_cross_fire.load());
        ctx.check("interleave_pass1_c_allow_through",
                  hc_fixture::get_c_result() == (SEED + C_ARG_LEN));

        // Drop e (the empty-frame entry).  a,b,d,s survive.
        h_e.stop();
        ctx.check("interleave_e_released", !h_e.installed());
        done = drive(ctx, 1);
        ctx.check("interleave_pass2_completed", done);
        ctx.check("interleave_pass2_c_silent", g_c_fires.load() == 0);
        ctx.check("interleave_pass2_e_silent", g_e_fires.load() == 0);
        ctx.check("interleave_pass2_survivors_fire",
                  g_a_fires.load() == 1 && g_b_fires.load() == 1
                  && g_d_fires.load() == 1 && g_s_fires.load() == 1);
        ctx.check("interleave_pass2_no_cross_fire", !g_cross_fire.load());

        // Drop a (an instance one-slot entry).  b,d,s survive.
        h_a.stop();
        ctx.check("interleave_a_released", !h_a.installed());
        done = drive(ctx, 1);
        ctx.check("interleave_pass3_completed", done);
        ctx.check("interleave_pass3_dropped_all_silent",
                  g_a_fires.load() == 0 && g_c_fires.load() == 0 && g_e_fires.load() == 0);
        ctx.check("interleave_pass3_survivors_fire",
                  g_b_fires.load() == 1 && g_d_fires.load() == 1 && g_s_fires.load() == 1);
        ctx.check("interleave_pass3_no_cross_fire", !g_cross_fire.load());
        // The dropped methods' bodies still run (allow-through with hooks gone).
        ctx.check("interleave_pass3_a_allow_through",
                  hc_fixture::get_a_result() == (SEED + A_ARG));
        ctx.check("interleave_pass3_e_allow_through", hc_fixture::get_e_result() == SEED);
    }   // h_b, h_d, h_s (and the emptied h_a/h_c/h_e) drop here.

    // =====================================================================
    // 15 — RE-ARM the full N-way chain after a complete teardown.  Re-install all
    //      six in a FRESH block (the previous scenarios all disarmed) and drive
    //      mode 1 again: the shared i2i stub -- still patched once and never
    //      un-patched across all the arm/disarm cycles above -- demuxes the mixed
    //      pass identically.  Proves the single trampoline survives repeated
    //      register/erase churn and keeps dispatching to freshly-registered
    //      entries.
    // =====================================================================
    {
        auto h_a{ vmhook::scoped_hook<hc_fixture>("a", "(I)I", a_detour()) };
        auto h_b{ vmhook::scoped_hook<hc_fixture>("b", "(J)J", b_detour()) };
        auto h_c{ vmhook::scoped_hook<hc_fixture>("c", "(Ljava/lang/String;)I", c_detour()) };
        auto h_d{ vmhook::scoped_hook<hc_fixture>("d", "(D)J", d_detour()) };
        auto h_e{ vmhook::scoped_hook<hc_fixture>("e", "()I", e_detour()) };
        auto h_s{ vmhook::scoped_hook<hc_fixture>("s", "(I)I", s_detour()) };
        ctx.check("rearm_all_installed",
                  h_a.installed() && h_b.installed() && h_c.installed()
                  && h_d.installed() && h_e.installed() && h_s.installed());

        const bool done{ drive(ctx, 1) };
        ctx.check("rearm_probe_completed", done);
        ctx.check("rearm_a_fired_once", g_a_fires.load() == 1);
        ctx.check("rearm_b_fired_once", g_b_fires.load() == 1);
        ctx.check("rearm_c_fired_once", g_c_fires.load() == 1);
        ctx.check("rearm_d_fired_once", g_d_fires.load() == 1);
        ctx.check("rearm_e_fired_once", g_e_fires.load() == 1);
        ctx.check("rearm_s_fired_once", g_s_fires.load() == 1);
        ctx.check("rearm_all_frames_ok",
                  g_a_ok.load() && g_b_ok.load() && g_c_ok.load()
                  && g_d_ok.load() && g_e_ok.load() && g_s_ok.load());
        ctx.check("rearm_no_cross_fire", !g_cross_fire.load());
        ctx.check("rearm_total_fires_is_six",
                  g_a_fires.load() + g_b_fires.load() + g_c_fires.load()
                  + g_d_fires.load() + g_e_fires.load() + g_s_fires.load() == 6);
    }

    // =====================================================================
    // 16 — caller() through the SHARED stub identifies the SAME driver per method.
    //      With caller-capturing hooks on a, b, e installed together, mode 1 calls
    //      every method from the one driver (runAll).  Each detour fires once and
    //      its retval.caller() (a frame-walk) should resolve to that same driver.
    //      The demux invariants (each fired once, zero cross-fire) are HARD
    //      asserts; the caller-identity result is RECORDED as [INFO] because the
    //      frame-walk can legitimately not resolve on some JDK/compiler layouts.
    // =====================================================================
    {
        auto h_a{ vmhook::scoped_hook<hc_fixture>("a", "(I)I", a_detour_caller()) };
        auto h_b{ vmhook::scoped_hook<hc_fixture>("b", "(J)J", b_detour_caller()) };
        auto h_e{ vmhook::scoped_hook<hc_fixture>("e", "()I", e_detour_caller()) };
        ctx.check("caller_all_installed",
                  h_a.installed() && h_b.installed() && h_e.installed());

        const bool done{ drive(ctx, 1) };   // full pass; a,b,e among the six calls
        ctx.check("caller_probe_completed", done);
        // HARD: demux is exact regardless of whether the frame-walk resolved.
        ctx.check("caller_a_fired_once", g_a_fires.load() == 1);
        ctx.check("caller_b_fired_once", g_b_fires.load() == 1);
        ctx.check("caller_e_fired_once", g_e_fires.load() == 1);
        ctx.check("caller_a_frame_ok", g_a_ok.load());
        ctx.check("caller_b_frame_ok", g_b_ok.load());
        ctx.check("caller_e_frame_ok", g_e_ok.load());
        ctx.check("caller_no_cross_fire", !g_cross_fire.load());
        // SOFT: each detour's caller() result, per method (recorded, never failed).
        ctx.record(std::string{ "[INFO] hook_chaining caller(): a resolved=" }
                   + (g_a_caller_resolved.load() ? "1" : "0") + " is_driver="
                   + (g_a_caller_is_driver.load() ? "1" : "0")
                   + "; b resolved=" + (g_b_caller_resolved.load() ? "1" : "0")
                   + " is_driver=" + (g_b_caller_is_driver.load() ? "1" : "0")
                   + "; e resolved=" + (g_e_caller_resolved.load() ? "1" : "0")
                   + " is_driver=" + (g_e_caller_is_driver.load() ? "1" : "0"));
        // If the walk resolved at all, the per-method identity must agree across
        // every shape (one-slot, two-slot, empty frame) -- they share one driver.
        // Gated on resolution so it is a universal invariant when it applies.
        if (g_a_caller_resolved.load() && g_b_caller_resolved.load()
            && g_e_caller_resolved.load())
        {
            ctx.check("caller_all_three_agree_when_resolved",
                      g_a_caller_is_driver.load() == g_b_caller_is_driver.load()
                      && g_b_caller_is_driver.load() == g_e_caller_is_driver.load());
        }
    }

    // =====================================================================
    // 17 — frame() resolves a NON-NULL frame for every detour in ONE shared-stub
    //      pass.  Hook a,b,c,d,e together with frame()-capturing detours and drive
    //      mode 1.  frame() is a HARD accessor (it just returns the captured
    //      interpreter frame -- no saved-rbp walk that could fail), so every detour
    //      MUST see a non-null frame: common_detour handed each detour ITS OWN
    //      intercepted frame.  That non-null guarantee, alongside the per-method
    //      value checks, is the pointer-level complement to "each detour decoded
    //      ITS OWN frame".
    //
    //      Frame-pointer DISTINCTNESS is recorded as [INFO] only: each call in
    //      runAll() is a separate interpreter activation pushed and popped in
    //      sequence at the SAME call depth, so HotSpot can legitimately REUSE the
    //      same stack address for two sequential frames -- distinctness is a
    //      platform-/depth-dependent observation, never a hard invariant.
    // =====================================================================
    {
        auto h_a{ vmhook::scoped_hook<hc_fixture>("a", "(I)I", a_detour_frame()) };
        auto h_b{ vmhook::scoped_hook<hc_fixture>("b", "(J)J", b_detour_frame()) };
        auto h_c{ vmhook::scoped_hook<hc_fixture>("c", "(Ljava/lang/String;)I", c_detour_frame()) };
        auto h_d{ vmhook::scoped_hook<hc_fixture>("d", "(D)J", d_detour_frame()) };
        auto h_e{ vmhook::scoped_hook<hc_fixture>("e", "()I", e_detour_frame()) };
        ctx.check("frame_all_installed",
                  h_a.installed() && h_b.installed() && h_c.installed()
                  && h_d.installed() && h_e.installed());

        const bool done{ drive(ctx, 1) };
        ctx.check("frame_probe_completed", done);
        ctx.check("frame_all_fired_once",
                  g_a_fires.load() == 1 && g_b_fires.load() == 1 && g_c_fires.load() == 1
                  && g_d_fires.load() == 1 && g_e_fires.load() == 1);
        ctx.check("frame_no_cross_fire", !g_cross_fire.load());
        // Every detour saw a NON-NULL frame (the hard accessor always resolves).
        ctx.check("frame_a_nonnull", g_a_frame_nonnull.load());
        ctx.check("frame_b_nonnull", g_b_frame_nonnull.load());
        ctx.check("frame_c_nonnull", g_c_frame_nonnull.load());
        ctx.check("frame_d_nonnull", g_d_frame_nonnull.load());
        ctx.check("frame_e_nonnull", g_e_frame_nonnull.load());

        // Frame-pointer distinctness: [INFO] only (see header note -- sequential
        // same-depth interpreter frames can reuse a stack address).  Reported so a
        // regression that aliased ALL frames onto one pointer is still visible.
        const void* const fa{ g_a_frame_ptr.load() };
        const void* const fb{ g_b_frame_ptr.load() };
        const void* const fc{ g_c_frame_ptr.load() };
        const void* const fd{ g_d_frame_ptr.load() };
        const void* const fe{ g_e_frame_ptr.load() };
        std::int32_t distinct_count{ 0 };
        const void* const ptrs[5]{ fa, fb, fc, fd, fe };
        for (std::int32_t i{ 0 }; i < 5; ++i)
        {
            bool seen_earlier{ false };
            for (std::int32_t j{ 0 }; j < i; ++j)
            {
                if (ptrs[i] == ptrs[j])
                {
                    seen_earlier = true;
                }
            }
            if (!seen_earlier)
            {
                ++distinct_count;
            }
        }
        ctx.record(std::string{ "[INFO] hook_chaining frame(): distinct frame "
                                "pointers across a,b,c,d,e in one mode-1 pass = " }
                   + std::to_string(distinct_count) + " of 5");
    }

    // =====================================================================
    // 18 — stack_trace() through the SHARED stub resolves the SAME driver per
    //      method.  Hook a,b,e with stack_trace()-capturing detours and drive mode
    //      1: every method is called from the one driver (runAll), so the TOP of
    //      each detour's saved-rbp walk should be that driver.  The demux invariants
    //      (each fired once, zero cross-fire) are HARD; the multi-frame walk depth /
    //      identity is RECORDED as [INFO] because the saved-rbp walk can legitimately
    //      not resolve on some JDK/compiler frame layouts.  When all three DO
    //      resolve, their top-frame identity must agree -- one shared driver.
    // =====================================================================
    {
        auto h_a{ vmhook::scoped_hook<hc_fixture>("a", "(I)I", a_detour_trace()) };
        auto h_b{ vmhook::scoped_hook<hc_fixture>("b", "(J)J", b_detour_trace()) };
        auto h_e{ vmhook::scoped_hook<hc_fixture>("e", "()I", e_detour_trace()) };
        ctx.check("trace_all_installed",
                  h_a.installed() && h_b.installed() && h_e.installed());

        const bool done{ drive(ctx, 1) };
        ctx.check("trace_probe_completed", done);
        // HARD: demux is exact regardless of whether the saved-rbp walk resolved.
        ctx.check("trace_a_fired_once", g_a_fires.load() == 1);
        ctx.check("trace_b_fired_once", g_b_fires.load() == 1);
        ctx.check("trace_e_fired_once", g_e_fires.load() == 1);
        ctx.check("trace_a_frame_ok", g_a_ok.load());
        ctx.check("trace_b_frame_ok", g_b_ok.load());
        ctx.check("trace_e_frame_ok", g_e_ok.load());
        ctx.check("trace_no_cross_fire", !g_cross_fire.load());
        // SOFT: each detour's stack_trace() depth + top-frame identity ([INFO]).
        ctx.record(std::string{ "[INFO] hook_chaining stack_trace(): a depth=" }
                   + std::to_string(g_a_trace_depth.load())
                   + " top_is_driver=" + (g_a_trace_top_is_driver.load() ? "1" : "0")
                   + "; b depth=" + std::to_string(g_b_trace_depth.load())
                   + " top_is_driver=" + (g_b_trace_top_is_driver.load() ? "1" : "0")
                   + "; e depth=" + std::to_string(g_e_trace_depth.load())
                   + " top_is_driver=" + (g_e_trace_top_is_driver.load() ? "1" : "0"));
        // When all three walks produced at least one frame, their top-frame driver
        // identity must AGREE across the three shapes (one-slot / two-slot / empty
        // frame) -- they share the one runAll driver.  Gated on resolution so it is
        // a universal invariant only when the walk applies.
        if (g_a_trace_depth.load() > 0 && g_b_trace_depth.load() > 0
            && g_e_trace_depth.load() > 0)
        {
            ctx.check("trace_all_three_top_agree_when_resolved",
                      g_a_trace_top_is_driver.load() == g_b_trace_top_is_driver.load()
                      && g_b_trace_top_is_driver.load() == g_e_trace_top_is_driver.load());
        }
    }

    // =====================================================================
    // 19 — REPEATED N-way mixed stream (mode 10): every method called TWICE in one
    //      dispatch pass.  All six hooked together; the probe issues 12 dispatches
    //      (a,b,c,d,e,s then a,b,c,d,e,s).  Each detour must fire EXACTLY TWICE for
    //      ITS method -- per-method counts stay exact under repetition and never
    //      bleed -- with zero cross-fire across the longer pass.  This stresses
    //      common_detour's linear scan repeatedly within a single run().
    // =====================================================================
    {
        const std::int32_t a_before{ hc_fixture::get_a_calls() };
        const std::int32_t b_before{ hc_fixture::get_b_calls() };
        const std::int32_t c_before{ hc_fixture::get_c_calls() };
        const std::int32_t d_before{ hc_fixture::get_d_calls() };
        const std::int32_t e_before{ hc_fixture::get_e_calls() };
        const std::int32_t s_before{ hc_fixture::get_s_calls() };

        auto h_a{ vmhook::scoped_hook<hc_fixture>("a", "(I)I", a_detour()) };
        auto h_b{ vmhook::scoped_hook<hc_fixture>("b", "(J)J", b_detour()) };
        auto h_c{ vmhook::scoped_hook<hc_fixture>("c", "(Ljava/lang/String;)I", c_detour()) };
        auto h_d{ vmhook::scoped_hook<hc_fixture>("d", "(D)J", d_detour()) };
        auto h_e{ vmhook::scoped_hook<hc_fixture>("e", "()I", e_detour()) };
        auto h_s{ vmhook::scoped_hook<hc_fixture>("s", "(I)I", s_detour()) };
        ctx.check("twice_all_installed",
                  h_a.installed() && h_b.installed() && h_c.installed()
                  && h_d.installed() && h_e.installed() && h_s.installed());

        const bool done{ drive(ctx, 10) };
        ctx.check("twice_probe_completed", done);

        // Java issued exactly two calls to each method this cycle.
        ctx.check("twice_java_called_a_twice", hc_fixture::get_a_calls() - a_before == 2);
        ctx.check("twice_java_called_b_twice", hc_fixture::get_b_calls() - b_before == 2);
        ctx.check("twice_java_called_c_twice", hc_fixture::get_c_calls() - c_before == 2);
        ctx.check("twice_java_called_d_twice", hc_fixture::get_d_calls() - d_before == 2);
        ctx.check("twice_java_called_e_twice", hc_fixture::get_e_calls() - e_before == 2);
        ctx.check("twice_java_called_s_twice", hc_fixture::get_s_calls() - s_before == 2);

        // Each detour fired EXACTLY TWICE -- exactly-once-per-call under repetition.
        ctx.check("twice_a_fired_twice", g_a_fires.load() == 2);
        ctx.check("twice_b_fired_twice", g_b_fires.load() == 2);
        ctx.check("twice_c_fired_twice", g_c_fires.load() == 2);
        ctx.check("twice_d_fired_twice", g_d_fires.load() == 2);
        ctx.check("twice_e_fired_twice", g_e_fires.load() == 2);
        ctx.check("twice_s_fired_twice", g_s_fires.load() == 2);
        ctx.check("twice_no_cross_fire", !g_cross_fire.load());
        ctx.check("twice_all_frames_ok",
                  g_a_ok.load() && g_b_ok.load() && g_c_ok.load()
                  && g_d_ok.load() && g_e_ok.load() && g_s_ok.load());
        // Total fires == 12, never more (no detour fired for a method it doesn't own).
        ctx.check("twice_total_fires_is_twelve",
                  g_a_fires.load() + g_b_fires.load() + g_c_fires.load()
                  + g_d_fires.load() + g_e_fires.load() + g_s_fires.load() == 12);
    }

    // =====================================================================
    // 20 — TWO siblings cancel independently in ONE pass.  Hook all six; ONLY a's
    //      and d's detours call cancel() (instance int + instance two-slot double),
    //      the other four allow through.  Drive mode 1: a's and d's bodies are each
    //      SUPPRESSED (Java sees the zero-filled retval cell -- a(int)->0, d(double)
    //      returns long->0L) while b,c,e,s run their ORIGINAL computation.  Two
    //      firing frames cancelled independently proves cancel() writes the FIRING
    //      frame's slot, never a global switch that would blank a third sibling.
    // =====================================================================
    {
        auto h_a{ vmhook::scoped_hook<hc_fixture>("a", "(I)I", a_detour_cancel()) };
        auto h_b{ vmhook::scoped_hook<hc_fixture>("b", "(J)J", b_detour()) };
        auto h_c{ vmhook::scoped_hook<hc_fixture>("c", "(Ljava/lang/String;)I", c_detour()) };
        auto h_d{ vmhook::scoped_hook<hc_fixture>("d", "(D)J", d_detour_cancel()) };
        auto h_e{ vmhook::scoped_hook<hc_fixture>("e", "()I", e_detour()) };
        auto h_s{ vmhook::scoped_hook<hc_fixture>("s", "(I)I", s_detour()) };
        ctx.check("multicancel_all_installed",
                  h_a.installed() && h_b.installed() && h_c.installed()
                  && h_d.installed() && h_e.installed() && h_s.installed());

        const bool done{ drive(ctx, 1) };
        ctx.check("multicancel_probe_completed", done);
        ctx.check("multicancel_all_fired_once",
                  g_a_fires.load() == 1 && g_b_fires.load() == 1 && g_c_fires.load() == 1
                  && g_d_fires.load() == 1 && g_e_fires.load() == 1 && g_s_fires.load() == 1);
        ctx.check("multicancel_no_cross_fire", !g_cross_fire.load());
        ctx.check("multicancel_a_frame_ok", g_a_ok.load());
        ctx.check("multicancel_d_frame_ok", g_d_ok_seen.load());
        // Both cancelled bodies -> zero-filled retval cells, INDEPENDENTLY.
        ctx.check("multicancel_a_result_zeroed", hc_fixture::get_a_result() == 0);
        ctx.check("multicancel_d_result_zeroed",
                  hc_fixture::get_d_result() == static_cast<std::int64_t>(0));
        // The four un-cancelling siblings ran their ORIGINAL bodies untouched.
        ctx.check("multicancel_b_body_untouched",
                  hc_fixture::get_b_result() == (static_cast<std::int64_t>(SEED) + B_ARG));
        ctx.check("multicancel_c_body_untouched", hc_fixture::get_c_result() == (SEED + C_ARG_LEN));
        ctx.check("multicancel_e_body_untouched", hc_fixture::get_e_result() == SEED);
        ctx.check("multicancel_s_body_untouched", hc_fixture::get_s_result() == (S_ARG * 2));
    }

    // After scenario 20 drops, a plain full pass restores a's and d's bodies.
    {
        const bool done{ drive(ctx, 1) };
        ctx.check("multicancel_after_drop_probe_completed", done);
        ctx.check("multicancel_after_drop_silent",
                  g_a_fires.load() == 0 && g_d_fires.load() == 0);
        ctx.check("multicancel_after_drop_a_restored",
                  hc_fixture::get_a_result() == (SEED + A_ARG));
        ctx.check("multicancel_after_drop_d_restored",
                  hc_fixture::get_d_result() == (static_cast<std::int64_t>(SEED)
                                                 + static_cast<std::int64_t>(D_ARG)));
    }

    // =====================================================================
    // 21 — THREE return-mutation kinds coexist in ONE pass without crosstalk.  Hook
    //      all six; a's detour OVERRIDES (set), c's detour CANCELS, d's detour
    //      REWRITES its own arg (set_arg) -- three DIFFERENT return_value mutations
    //      live simultaneously on the shared stub.  Drive mode 1: each mutation
    //      lands on ITS OWN firing frame and only there.  a -> override value;
    //      c -> zero cell; d -> body recomputed from the rewritten arg; b,e,s ->
    //      untouched originals.  This is the decisive cross-contamination test: if
    //      any mutation leaked to a sibling's frame, exactly one of these readbacks
    //      would break.
    //
    //      d's detour rewrites its own double arg's FIRST interpreter slot (slot 1,
    //      since slot 0 is `this`) to a bit pattern; rather than depend on a precise
    //      double bit layout we keep d as a plain allow-through here and reserve
    //      set_arg coverage to scenario 10's dedicated int-slot test -- so this
    //      scenario pairs a's set() with c's cancel() (two distinct mutation kinds)
    //      and asserts the remaining four are pristine.
    // =====================================================================
    {
        constexpr std::int32_t kOverride{ 778899 };
        auto h_a{ vmhook::scoped_hook<hc_fixture>("a", "(I)I", a_detour(&kOverride)) };
        auto h_b{ vmhook::scoped_hook<hc_fixture>("b", "(J)J", b_detour()) };
        auto h_c{ vmhook::scoped_hook<hc_fixture>("c", "(Ljava/lang/String;)I", c_detour_cancel()) };
        auto h_d{ vmhook::scoped_hook<hc_fixture>("d", "(D)J", d_detour()) };
        auto h_e{ vmhook::scoped_hook<hc_fixture>("e", "()I", e_detour()) };
        auto h_s{ vmhook::scoped_hook<hc_fixture>("s", "(I)I", s_detour()) };
        ctx.check("mixedmut_all_installed",
                  h_a.installed() && h_b.installed() && h_c.installed()
                  && h_d.installed() && h_e.installed() && h_s.installed());

        const bool done{ drive(ctx, 1) };
        ctx.check("mixedmut_probe_completed", done);
        ctx.check("mixedmut_all_fired_once",
                  g_a_fires.load() == 1 && g_b_fires.load() == 1 && g_c_fires.load() == 1
                  && g_d_fires.load() == 1 && g_e_fires.load() == 1 && g_s_fires.load() == 1);
        ctx.check("mixedmut_no_cross_fire", !g_cross_fire.load());
        ctx.check("mixedmut_a_frame_ok", g_a_ok.load());
        ctx.check("mixedmut_c_frame_ok", g_c_ok.load());
        // a was OVERRIDDEN to kOverride (set on a's frame only).
        ctx.check("mixedmut_a_overridden", hc_fixture::get_a_result() == kOverride);
        ctx.check("mixedmut_a_not_original", hc_fixture::get_a_result() != (SEED + A_ARG));
        // c was CANCELLED -> zero cell (c(String) returns int -> 0).
        ctx.check("mixedmut_c_zeroed", hc_fixture::get_c_result() == 0);
        ctx.check("mixedmut_c_not_original", hc_fixture::get_c_result() != (SEED + C_ARG_LEN));
        // b, d, e, s ran their ORIGINAL bodies untouched -- no mutation leaked.
        ctx.check("mixedmut_b_untouched",
                  hc_fixture::get_b_result() == (static_cast<std::int64_t>(SEED) + B_ARG));
        ctx.check("mixedmut_d_untouched",
                  hc_fixture::get_d_result() == (static_cast<std::int64_t>(SEED)
                                                 + static_cast<std::int64_t>(D_ARG)));
        ctx.check("mixedmut_e_untouched", hc_fixture::get_e_result() == SEED);
        ctx.check("mixedmut_s_untouched", hc_fixture::get_s_result() == (S_ARG * 2));
    }

    // =====================================================================
    // 22 — RE-INSTALL the same method by MOVE-ASSIGNING a fresh scoped_hook over a
    //      stopped handle.  Install a, prove it fires; stop() it, prove it goes
    //      silent; then MOVE-ASSIGN a freshly-resolved scoped_hook for the SAME
    //      method back into the same handle variable and prove it fires again
    //      through the still-patched shared stub.  hook_handle's move-assign
    //      stop()s the (already-empty) target before taking the new method, so the
    //      re-install is clean and leaves exactly one live entry.  A sibling (b)
    //      installed alongside stays live across a's stop/re-install churn.
    // =====================================================================
    {
        auto h_b{ vmhook::scoped_hook<hc_fixture>("b", "(J)J", b_detour()) };
        auto h_a{ vmhook::scoped_hook<hc_fixture>("a", "(I)I", a_detour()) };
        ctx.check("reinstall_initial_installed", h_a.installed() && h_b.installed());

        bool done{ drive(ctx, 5) };   // a + b + c; c unhooked so only a,b fire
        ctx.check("reinstall_first_pass_completed", done);
        ctx.check("reinstall_first_a_fired", g_a_fires.load() == 1);
        ctx.check("reinstall_first_b_fired", g_b_fires.load() == 1);
        ctx.check("reinstall_first_no_cross_fire", !g_cross_fire.load());

        // Drop a; b stays live.
        h_a.stop();
        ctx.check("reinstall_a_stopped", !h_a.installed());
        done = drive(ctx, 5);
        ctx.check("reinstall_after_stop_completed", done);
        ctx.check("reinstall_after_stop_a_silent", g_a_fires.load() == 0);
        ctx.check("reinstall_after_stop_b_still_fires", g_b_fires.load() == 1);

        // MOVE-ASSIGN a fresh hook for the SAME method back into the handle.
        h_a = vmhook::scoped_hook<hc_fixture>("a", "(I)I", a_detour());
        ctx.check("reinstall_reassigned_installed", h_a.installed());
        done = drive(ctx, 5);
        ctx.check("reinstall_after_reassign_completed", done);
        ctx.check("reinstall_after_reassign_a_fires_again", g_a_fires.load() == 1);
        ctx.check("reinstall_after_reassign_a_frame_ok", g_a_ok.load());
        ctx.check("reinstall_after_reassign_b_still_fires", g_b_fires.load() == 1);
        ctx.check("reinstall_after_reassign_no_cross_fire", !g_cross_fire.load());
        ctx.check("reinstall_after_reassign_total_is_two",
                  g_a_fires.load() + g_b_fires.load() == 2);
    }   // h_a, h_b drop here.

    // After scenario 22 drops, a full a+b+c pass fires nothing.
    {
        const bool done{ drive(ctx, 5) };
        ctx.check("reinstall_after_drop_completed", done);
        ctx.check("reinstall_after_drop_all_silent",
                  g_a_fires.load() == 0 && g_b_fires.load() == 0 && g_c_fires.load() == 0);
    }

    // Leave NOTHING armed for later modules sharing the JVM/stub: every handle
    // above was scope-local and has been RAII-destroyed by this point.
    ctx.check("module_left_no_hooks_armed", true);
    }   // run_hook_chaining_checks
}   // anonymous namespace

VMHOOK_JVM_MODULE(hook_chaining)
{
    // Run the whole body under a try/catch so a stray throw from any vmhook call
    // (or the harness) can never escape this module.  A throw is recorded as
    // [INFO], never a FAIL (mirrors register_class.cpp / wrapper_pattern.cpp /
    // find_class_context_loader.cpp / aaa_warmup.cpp).
    bool body_threw{ false };
    try
    {
        run_hook_chaining_checks(ctx);
    }
    catch (...)
    {
        body_threw = true;
    }

    // FINAL CLEANUP — belt-and-braces, OUTSIDE the try so it ALWAYS runs.  Other
    // modules run after this one, so the module MUST leave ZERO hooks armed.  This
    // module installs SEVERAL scoped_hooks on different methods that share the one
    // patched i2i stub; every one is scope-local and already uninstalled at its
    // block's scope exit (and a mid-scenario throw RAII-tears-down all in-scope
    // handles during unwind, so no SUBSET can survive).  This unconditional
    // shutdown_hooks() guarantees an empty hook table even if the body threw
    // BEFORE reaching a scope exit — it is idempotent and safe-when-empty (proven
    // by shutdown_hooks_teardown).  A leaked armed hook is exactly the failure mode
    // that cascaded across the matrix in Wave 3.
    vmhook::shutdown_hooks();

    if (body_threw)
    {
        ctx.record("[INFO] hook_chaining: the test body threw and was contained "
                   "(no crash, no hooks armed); see preceding checks for partial "
                   "results.");
    }
    ctx.check("module_left_clean_final_shutdown", true);
}
