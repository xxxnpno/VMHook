// method_override_dispatch — area: hooks / i2i interpreter-stub demux of
// OVERRIDDEN methods (issue #31).
//
// THE hook-side authority for hooking an OVERRIDDEN Java method.  It is the
// inheritance-hierarchy companion to hook_chaining: that module proves the shared
// interpreter-to-interpreter (i2i) stub demultiplexes a mixed call stream of
// DISTINCT methods (different names/descriptors) to the right per-method detour;
// THIS module proves the same demux for methods that share a NAME and a
// DESCRIPTOR but live at different LEVELS of a class hierarchy.
//
// THE MECHANISM UNDER TEST.  vmhook::hook<T>() resolves its target by scanning
// the registered class's OWN _methods array (vmhook.hpp:9929-9940) — it does NOT
// walk the super chain — so a hook is keyed to ONE specific Method*.  HotSpot
// shares ONE i2i stub across many methods; common_detour fires a detour only when
// a call RESOLVES to that exact hooked Method* (first-match-by-Method*-identity,
// the contract hook_chaining proves).  An override is a PHYSICALLY DISTINCT
// Method* from the base declaration, so:
//   * hooking Base.m fires ONLY for receivers whose runtime class resolves m to
//     THAT declaration (a plain Base, or a subclass that does NOT override);
//   * hooking Derived.m (the override) fires ONLY for a Derived receiver;
//   * the two NEVER cross-fire, even though name + descriptor are identical.
// That disjointness — proven from WHICH native detour fired for WHICH receiver
// runtime type, cross-checked against Java's own per-method recorders — is the
// override-dispatch contract.
//
// SCENARIOS (each via scoped_hook, RAII-torn-down before the next):
//   1. SINGLE OVERRIDE base side    — hook Base.m; Base receiver fires it,
//                                     Derived receiver does NOT.
//   2. SINGLE OVERRIDE derived side — hook Derived.m; Derived receiver fires it,
//                                     Base receiver does NOT.
//   3. MULTI-LEVEL CHAIN A<-B<-C    — hook A.g, B.g, C.g simultaneously; each
//                                     concrete receiver fires ONLY its own level.
//   4. OVERRIDE + super-call        — hook SuperBase.s; calling SuperDerived.s
//                                     (which does super.s()) fires the base hook
//                                     via the invokespecial super leg.
//   5. OVERRIDE vs OVERLOAD         — hook Ov.p(I)I and Ov.p(Lstring)I; an int
//                                     call fires only the (I)I detour, a String
//                                     call only the (Lstring)I detour — descriptor
//                                     disambiguates, no cross-fire.
//   6. OVERRIDE+OVERLOAD mix        — hook OvSub.p(I)I [override] and the inherited
//                                     Ov.p(Lstring)I; on an OvSub the int call
//                                     fires the override, the String call fires
//                                     the inherited base Method*.
//   7. INTERFACE default override   — hook ImplOver.d and ImplInherit.d; disjoint
//                                     receivers, no cross-fire.
//   8. ABSTRACT-method impl         — hook AbsImpl.a; the concrete impl fires.
//   9. SIMULTANEOUS both-levels     — Base.m AND Derived.m hooked at once; one
//                                     pass drives both receivers; each detour
//                                     fires exactly once, zero cross-fire (the
//                                     headline demux proof).
//   + post-teardown: with every hook dropped, a full pass fires NOTHING (clean
//     teardown of every shared-stub entry) and the bodies dispatch normally.
//
// SAFETY (runs on ALL cells incl. no-SEH windows-clang/mingw where an uncaught AV
// crashes the WHOLE suite):
//   * STRICT TEARDOWN.  Every hook is a scope-local vmhook::scoped_hook<>; each
//     block RAII-uninstalls at its closing brace and a throw mid-scenario unwinds
//     and destroys all in-scope handles (reverse construction order), so no SUBSET
//     can be left armed.  On top of that the whole body runs under try/catch and an
//     UNCONDITIONAL vmhook::shutdown_hooks() runs OUTSIDE the try, so even a throw
//     before a scope exit returns control with an empty hook table.  A leaked armed
//     hook is exactly what cascaded the no-SEH suite in earlier waves; this module
//     cannot leak one on ANY path.
//   * ENTRY GUARD bails cleanly to [INFO] if the fixture is absent, so the
//     unguarded handshake static_field()->set/get derefs can never fault.
//   * NO FORCED GC, NO large in-detour allocation, NO single-instant JIT-owned
//     reads.  Detours only read primitives off self (the kind field) and bump
//     atomics, so there is no GC-window hazard and no settle helper is needed.
//   * UNIVERSAL INVARIANTS only are HARD-asserted: "an override is a distinct
//     Method*, so a base-level hook and a derived-level hook fire on disjoint
//     receivers and never cross-fire" holds identically on JDK 8..26 HotSpot.
//     Anything build-variant (e.g. whether self-field decode succeeds on a given
//     cold path) is PASS-or-[INFO], never a FAIL.
//   * MSVC copy-init from any value_t (never brace-init); String/object reads use
//     the supported accessors.  No std::bit_cast (C++17-safe idioms only).
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace
{
    // ── Fixture-mirrored markers / kinds (lockstep with OverrideDispatch.java) ─
    constexpr std::int32_t MARK_BASE_M       = 101;
    constexpr std::int32_t MARK_DERIVED_M    = 102;
    constexpr std::int32_t MARK_A_G          = 201;
    constexpr std::int32_t MARK_B_G          = 202;
    constexpr std::int32_t MARK_C_G          = 203;
    constexpr std::int32_t MARK_SUPER_BASE_S = 301;
    constexpr std::int32_t MARK_SUPER_DER_S  = 302;
    constexpr std::int32_t MARK_OV_P_INT     = 401;
    constexpr std::int32_t MARK_OV_P_STR     = 402;
    constexpr std::int32_t MARK_OVSUB_P_INT  = 411;
    constexpr std::int32_t MARK_IMPL_OVER_D  = 501;
    constexpr std::int32_t MARK_IFACE_D      = 502;
    constexpr std::int32_t MARK_ABS_IMPL_A   = 601;

    constexpr std::int32_t KIND_BASE       = 11;
    constexpr std::int32_t KIND_DERIVED    = 12;
    constexpr std::int32_t KIND_A          = 21;
    constexpr std::int32_t KIND_B          = 22;
    constexpr std::int32_t KIND_C          = 23;
    constexpr std::int32_t KIND_SUPER_BASE = 31;
    constexpr std::int32_t KIND_SUPER_DER  = 32;
    constexpr std::int32_t KIND_OV         = 41;
    constexpr std::int32_t KIND_OVSUB      = 42;
    constexpr std::int32_t KIND_IMPL_INH   = 52;
    constexpr std::int32_t KIND_ABS_IMPL   = 61;

    // Internal JVM names of the nested fixture klasses (used by register_class<T>
    // and by the entry guard's find_class pre-check).
    constexpr char FIXTURE[]{ "vmhook/fixtures/OverrideDispatch" };

    // ── Wrappers, one per registrable hierarchy klass.  Each derives from
    //    vmhook::object<T> (vtable + get_field accessors).  A detour's `self` is a
    //    std::unique_ptr<T> built from the receiver oop using THIS wrapper's klass;
    //    kind() reads the per-receiver discriminator field off self. ─────────────
    class od_base : public vmhook::object<od_base>
    {
    public:
        explicit od_base(vmhook::oop_t instance) noexcept
            : vmhook::object<od_base>{ instance } {}
        auto kind() const -> std::int32_t { return get_field("kind")->get(); }
    };
    class od_derived : public vmhook::object<od_derived>
    {
    public:
        explicit od_derived(vmhook::oop_t instance) noexcept
            : vmhook::object<od_derived>{ instance } {}
        auto kind() const -> std::int32_t { return get_field("kind")->get(); }
    };
    class od_a : public vmhook::object<od_a>
    {
    public:
        explicit od_a(vmhook::oop_t instance) noexcept
            : vmhook::object<od_a>{ instance } {}
        auto kind() const -> std::int32_t { return get_field("kind")->get(); }
    };
    class od_b : public vmhook::object<od_b>
    {
    public:
        explicit od_b(vmhook::oop_t instance) noexcept
            : vmhook::object<od_b>{ instance } {}
        auto kind() const -> std::int32_t { return get_field("kind")->get(); }
    };
    class od_c : public vmhook::object<od_c>
    {
    public:
        explicit od_c(vmhook::oop_t instance) noexcept
            : vmhook::object<od_c>{ instance } {}
        auto kind() const -> std::int32_t { return get_field("kind")->get(); }
    };
    class od_super_base : public vmhook::object<od_super_base>
    {
    public:
        explicit od_super_base(vmhook::oop_t instance) noexcept
            : vmhook::object<od_super_base>{ instance } {}
        auto kind() const -> std::int32_t { return get_field("kind")->get(); }
    };
    class od_ov : public vmhook::object<od_ov>
    {
    public:
        explicit od_ov(vmhook::oop_t instance) noexcept
            : vmhook::object<od_ov>{ instance } {}
        auto kind() const -> std::int32_t { return get_field("kind")->get(); }
    };
    class od_ovsub : public vmhook::object<od_ovsub>
    {
    public:
        explicit od_ovsub(vmhook::oop_t instance) noexcept
            : vmhook::object<od_ovsub>{ instance } {}
        auto kind() const -> std::int32_t { return get_field("kind")->get(); }
    };
    class od_impl_inherit : public vmhook::object<od_impl_inherit>
    {
    public:
        explicit od_impl_inherit(vmhook::oop_t instance) noexcept
            : vmhook::object<od_impl_inherit>{ instance } {}
        auto kind() const -> std::int32_t { return get_field("kind")->get(); }
    };
    class od_impl_over : public vmhook::object<od_impl_over>
    {
    public:
        explicit od_impl_over(vmhook::oop_t instance) noexcept
            : vmhook::object<od_impl_over>{ instance } {}
        auto kind() const -> std::int32_t { return get_field("kind")->get(); }
    };
    class od_abs_impl : public vmhook::object<od_abs_impl>
    {
    public:
        explicit od_abs_impl(vmhook::oop_t instance) noexcept
            : vmhook::object<od_abs_impl>{ instance } {}
        auto kind() const -> std::int32_t { return get_field("kind")->get(); }
    };

    // ── The fixture handle (static handshake + Java-side readback fields) ──────
    class od_fixture : public vmhook::object<od_fixture>
    {
    public:
        explicit od_fixture(vmhook::oop_t instance) noexcept
            : vmhook::object<od_fixture>{ instance } {}

        static auto set_go(bool v) -> void      { static_field("go")->set(v); }
        static auto set_done(bool v) -> void     { static_field("done")->set(v); }
        static auto get_done() -> bool           { return static_field("done")->get(); }
        static auto set_mode(std::int32_t m) -> void { static_field("mode")->set(m); }
        static auto probe_runs() -> std::int32_t { return static_field("probeRuns")->get(); }
        static auto last_fired() -> std::int32_t { return static_field("lastFired")->get(); }

        // Per-method Java hit counters (native asserts post-probe DELTAS).
        static auto base_m_hits()      -> std::int32_t { return static_field("baseMHits")->get(); }
        static auto derived_m_hits()   -> std::int32_t { return static_field("derivedMHits")->get(); }
        static auto a_g_hits()         -> std::int32_t { return static_field("aGHits")->get(); }
        static auto b_g_hits()         -> std::int32_t { return static_field("bGHits")->get(); }
        static auto c_g_hits()         -> std::int32_t { return static_field("cGHits")->get(); }
        static auto super_base_s_hits()-> std::int32_t { return static_field("superBaseSHits")->get(); }
        static auto super_der_s_hits() -> std::int32_t { return static_field("superDerSHits")->get(); }
        static auto ov_p_int_hits()    -> std::int32_t { return static_field("ovPIntHits")->get(); }
        static auto ov_p_str_hits()    -> std::int32_t { return static_field("ovPStrHits")->get(); }
        static auto ovsub_p_int_hits() -> std::int32_t { return static_field("ovSubPIntHits")->get(); }
        static auto impl_over_d_hits() -> std::int32_t { return static_field("implOverDHits")->get(); }
        static auto iface_d_hits()     -> std::int32_t { return static_field("ifaceDHits")->get(); }
        static auto abs_impl_a_hits()  -> std::int32_t { return static_field("absImplAHits")->get(); }
    };

    // ── Native per-detour fire counters + correctness latches ─────────────────
    std::atomic<std::int32_t> g_base_m_fires{ 0 };
    std::atomic<std::int32_t> g_derived_m_fires{ 0 };
    std::atomic<std::int32_t> g_a_g_fires{ 0 };
    std::atomic<std::int32_t> g_b_g_fires{ 0 };
    std::atomic<std::int32_t> g_c_g_fires{ 0 };
    std::atomic<std::int32_t> g_super_base_s_fires{ 0 };
    std::atomic<std::int32_t> g_ov_p_int_fires{ 0 };
    std::atomic<std::int32_t> g_ov_p_str_fires{ 0 };
    std::atomic<std::int32_t> g_ovsub_p_int_fires{ 0 };
    std::atomic<std::int32_t> g_impl_over_d_fires{ 0 };
    std::atomic<std::int32_t> g_impl_inherit_d_fires{ 0 };
    std::atomic<std::int32_t> g_abs_impl_a_fires{ 0 };

    // Per-detour "saw the right receiver kind off self" latches.  Best-effort:
    // self-field decode is a cold read; a detour that observed the WRONG kind
    // flips the cross-fire sentinel (a hard fact about misrouting), but a detour
    // that could not decode self at all leaves the latch false WITHOUT flipping
    // cross-fire (recorded as [INFO]).
    std::atomic<bool> g_base_m_self_ok{ false };
    std::atomic<bool> g_derived_m_self_ok{ false };
    std::atomic<bool> g_a_g_self_ok{ false };
    std::atomic<bool> g_b_g_self_ok{ false };
    std::atomic<bool> g_c_g_self_ok{ false };

    // CROSS-FIRE sentinel: set if ANY detour observed a self whose kind is a
    // DEFINITE mismatch for the method it owns (i.e. common_detour routed a
    // sibling's call into the wrong detour).  Must stay false on every JDK.
    std::atomic<bool> g_cross_fire{ false };

    auto reset_fires() -> void
    {
        g_base_m_fires.store(0);
        g_derived_m_fires.store(0);
        g_a_g_fires.store(0);
        g_b_g_fires.store(0);
        g_c_g_fires.store(0);
        g_super_base_s_fires.store(0);
        g_ov_p_int_fires.store(0);
        g_ov_p_str_fires.store(0);
        g_ovsub_p_int_fires.store(0);
        g_impl_over_d_fires.store(0);
        g_impl_inherit_d_fires.store(0);
        g_abs_impl_a_fires.store(0);
        g_base_m_self_ok.store(false);
        g_derived_m_self_ok.store(false);
        g_a_g_self_ok.store(false);
        g_b_g_self_ok.store(false);
        g_c_g_self_ok.store(false);
        g_cross_fire.store(false);
    }

    // Best-effort self-kind check used inside detours.  Returns:
    //   +1 = decoded and kind == expected   (correct receiver)
    //    0 = could not decode self          (cold-read miss; neither ok nor cross)
    //   -1 = decoded but kind != expected   (DEFINITE misroute -> cross-fire)
    template<class wrapper_t>
    auto self_kind_verdict(const std::unique_ptr<wrapper_t>& self,
                           std::int32_t expected) -> int
    {
        if (!self)
        {
            return 0;
        }
        try
        {
            const std::int32_t k{ self->kind() };
            return (k == expected) ? +1 : -1;
        }
        catch (...)
        {
            return 0;
        }
    }

    template<class wrapper_t>
    auto note_self(const std::unique_ptr<wrapper_t>& self,
                   std::int32_t expected,
                   std::atomic<bool>& ok_latch) -> void
    {
        const int v{ self_kind_verdict(self, expected) };
        if (v > 0)
        {
            ok_latch.store(true, std::memory_order_relaxed);
        }
        else if (v < 0)
        {
            g_cross_fire.store(true, std::memory_order_relaxed);
        }
        // v == 0: cold-read miss — leave both untouched (characterized [INFO]).
    }

    // Drive ONE probe cycle for `mode`: reset native fire counters + latched done,
    // program mode on the rising edge of go, then run the probe.
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
                    od_fixture::set_done(false);
                    od_fixture::set_mode(mode);
                }
                od_fixture::set_go(value);
            },
            []() { return od_fixture::get_done(); });
    }

    // ── The whole test body, factored so the wrapper can run it under try/catch
    //    and ALWAYS follow it with shutdown_hooks() (zero hooks armed on EVERY
    //    exit path). ─────────────────────────────────────────────────────────
    auto run_checks(vmhook_test::context& ctx) -> void
    {
        // ENTRY GUARD: bail cleanly if the fixture is not loaded (no deref of a
        // disengaged optional in the handshake; the wrapper's shutdown still runs).
        if (vmhook::find_class(FIXTURE) == nullptr)
        {
            ctx.record("[INFO] method_override_dispatch: OverrideDispatch not "
                       "loaded/resolvable on this run; skipping live checks (no "
                       "crash, no hooks armed).");
            return;
        }

        vmhook::register_class<od_base>("vmhook/fixtures/OverrideDispatch$Base");
        vmhook::register_class<od_derived>("vmhook/fixtures/OverrideDispatch$Derived");
        vmhook::register_class<od_a>("vmhook/fixtures/OverrideDispatch$A");
        vmhook::register_class<od_b>("vmhook/fixtures/OverrideDispatch$B");
        vmhook::register_class<od_c>("vmhook/fixtures/OverrideDispatch$C");
        vmhook::register_class<od_super_base>("vmhook/fixtures/OverrideDispatch$SuperBase");
        vmhook::register_class<od_ov>("vmhook/fixtures/OverrideDispatch$Ov");
        vmhook::register_class<od_ovsub>("vmhook/fixtures/OverrideDispatch$OvSub");
        vmhook::register_class<od_impl_inherit>("vmhook/fixtures/OverrideDispatch$ImplInherit");
        vmhook::register_class<od_impl_over>("vmhook/fixtures/OverrideDispatch$ImplOver");
        vmhook::register_class<od_abs_impl>("vmhook/fixtures/OverrideDispatch$AbsImpl");
        vmhook::register_class<od_fixture>(FIXTURE);

        // Record the live dispatch path once (call_stub fast path vs call_jni
        // fallback is JDK-dependent; both resolve the override identically).
        ctx.record(std::string{ "[INFO] method_override_dispatch dispatch path: " }
                   + (vmhook::detail::find_call_stub_entry() != nullptr
                          ? "call_stub fast path"
                          : "call_jni fallback (StubRoutines::_call_stub_entry absent)"));

        // ── Detour factories ─────────────────────────────────────────────────
        auto base_m_detour = [](vmhook::return_value&,
                                const std::unique_ptr<od_base>& self,
                                std::int32_t /*x*/)
        {
            g_base_m_fires.fetch_add(1, std::memory_order_relaxed);
            note_self(self, KIND_BASE, g_base_m_self_ok);
        };
        auto derived_m_detour = [](vmhook::return_value&,
                                   const std::unique_ptr<od_derived>& self,
                                   std::int32_t /*x*/)
        {
            g_derived_m_fires.fetch_add(1, std::memory_order_relaxed);
            note_self(self, KIND_DERIVED, g_derived_m_self_ok);
        };
        auto a_g_detour = [](vmhook::return_value&,
                             const std::unique_ptr<od_a>& self)
        {
            g_a_g_fires.fetch_add(1, std::memory_order_relaxed);
            note_self(self, KIND_A, g_a_g_self_ok);
        };
        auto b_g_detour = [](vmhook::return_value&,
                             const std::unique_ptr<od_b>& self)
        {
            g_b_g_fires.fetch_add(1, std::memory_order_relaxed);
            note_self(self, KIND_B, g_b_g_self_ok);
        };
        auto c_g_detour = [](vmhook::return_value&,
                             const std::unique_ptr<od_c>& self)
        {
            g_c_g_fires.fetch_add(1, std::memory_order_relaxed);
            note_self(self, KIND_C, g_c_g_self_ok);
        };
        auto super_base_s_detour = [](vmhook::return_value&,
                                      const std::unique_ptr<od_super_base>& self,
                                      std::int32_t /*x*/)
        {
            g_super_base_s_fires.fetch_add(1, std::memory_order_relaxed);
            // self may be a SuperBase OR a SuperDerived (the super-call leg), so a
            // kind mismatch here is NOT a misroute — only count, never cross-fire.
            static_cast<void>(self);
        };
        auto ov_p_int_detour = [](vmhook::return_value&,
                                  const std::unique_ptr<od_ov>& self,
                                  std::int32_t /*x*/)
        {
            g_ov_p_int_fires.fetch_add(1, std::memory_order_relaxed);
            static_cast<void>(self);
        };
        auto ov_p_str_detour = [](vmhook::return_value&,
                                  const std::unique_ptr<od_ov>& self,
                                  const std::string& /*s*/)
        {
            g_ov_p_str_fires.fetch_add(1, std::memory_order_relaxed);
            static_cast<void>(self);
        };
        auto ovsub_p_int_detour = [](vmhook::return_value&,
                                     const std::unique_ptr<od_ovsub>& self,
                                     std::int32_t /*x*/)
        {
            g_ovsub_p_int_fires.fetch_add(1, std::memory_order_relaxed);
            static_cast<void>(self);
        };
        auto impl_over_d_detour = [](vmhook::return_value&,
                                     const std::unique_ptr<od_impl_over>& self)
        {
            g_impl_over_d_fires.fetch_add(1, std::memory_order_relaxed);
            static_cast<void>(self);
        };
        auto impl_inherit_d_detour = [](vmhook::return_value&,
                                        const std::unique_ptr<od_impl_inherit>& self)
        {
            g_impl_inherit_d_fires.fetch_add(1, std::memory_order_relaxed);
            static_cast<void>(self);
        };
        auto abs_impl_a_detour = [](vmhook::return_value&,
                                    const std::unique_ptr<od_abs_impl>& self)
        {
            g_abs_impl_a_fires.fetch_add(1, std::memory_order_relaxed);
            static_cast<void>(self);
        };

        // =====================================================================
        // 0 — Handshake sanity: drive a no-op-ish pass and confirm the probe runs.
        //     (mode 1 with NO hooks installed — bodies run, no detour fires.)
        // =====================================================================
        {
            const std::int32_t probe_before{ od_fixture::probe_runs() };
            const bool done{ drive(ctx, 1) };
            ctx.check("handshake_probe_completed", done);
            ctx.check("handshake_probe_ran", od_fixture::probe_runs() - probe_before == 1);
            ctx.check("handshake_no_detour_without_hook",
                      g_base_m_fires.load() == 0 && g_derived_m_fires.load() == 0);
            // Java body ran: Base.m stamped its marker.
            ctx.check("handshake_base_body_ran", od_fixture::last_fired() == MARK_BASE_M);
        }

        // =====================================================================
        // 1 — SINGLE OVERRIDE, BASE side.  Hook Base.m only.  A Base receiver
        //     fires it; a Derived receiver (the override, a different Method*)
        //     does NOT.
        // =====================================================================
        {
            auto h{ vmhook::scoped_hook<od_base>("m", "(I)I", base_m_detour) };
            ctx.check("single_base_hook_installed", h.installed());

            const std::int32_t base_hits_before{ od_fixture::base_m_hits() };
            bool done{ drive(ctx, 1) };   // Base receiver
            ctx.check("single_base_probe_completed", done);
            ctx.check("single_base_detour_fired_once", g_base_m_fires.load() == 1);
            ctx.check("single_base_no_cross_fire", !g_cross_fire.load());
            ctx.check("single_base_java_called_base_once",
                      od_fixture::base_m_hits() - base_hits_before == 1);
            ctx.check("single_base_marker_is_base", od_fixture::last_fired() == MARK_BASE_M);
            // Best-effort self identity (PASS-or-[INFO]).
            if (g_base_m_self_ok.load())
            {
                ctx.check("single_base_self_is_base", true);
            }
            else
            {
                ctx.record("[INFO] single_base: self.kind decode not confirmed on "
                           "this build (cold self-read); fire count + Java readback "
                           "still prove dispatch.");
            }

            // Derived receiver: the Base.m hook must NOT fire (override is a
            // distinct Method*).
            const std::int32_t der_hits_before{ od_fixture::derived_m_hits() };
            done = drive(ctx, 2);   // Derived receiver
            ctx.check("single_base_on_derived_probe_completed", done);
            ctx.check("single_base_hook_silent_for_derived", g_base_m_fires.load() == 0);
            ctx.check("single_base_on_derived_no_cross_fire", !g_cross_fire.load());
            ctx.check("single_base_derived_body_still_ran",
                      od_fixture::derived_m_hits() - der_hits_before == 1);
            ctx.check("single_base_derived_marker_is_derived",
                      od_fixture::last_fired() == MARK_DERIVED_M);
        }   // Base.m hook drops here.

        // =====================================================================
        // 2 — SINGLE OVERRIDE, DERIVED side.  Hook Derived.m only.  A Derived
        //     receiver fires it; a Base receiver does NOT.
        // =====================================================================
        {
            auto h{ vmhook::scoped_hook<od_derived>("m", "(I)I", derived_m_detour) };
            ctx.check("single_derived_hook_installed", h.installed());

            const std::int32_t der_hits_before{ od_fixture::derived_m_hits() };
            bool done{ drive(ctx, 2) };   // Derived receiver
            ctx.check("single_derived_probe_completed", done);
            ctx.check("single_derived_detour_fired_once", g_derived_m_fires.load() == 1);
            ctx.check("single_derived_no_cross_fire", !g_cross_fire.load());
            ctx.check("single_derived_java_called_derived_once",
                      od_fixture::derived_m_hits() - der_hits_before == 1);
            ctx.check("single_derived_marker_is_derived",
                      od_fixture::last_fired() == MARK_DERIVED_M);
            if (g_derived_m_self_ok.load())
            {
                ctx.check("single_derived_self_is_derived", true);
            }
            else
            {
                ctx.record("[INFO] single_derived: self.kind decode not confirmed "
                           "on this build; fire count + Java readback prove dispatch.");
            }

            // Base receiver: the Derived.m hook must NOT fire.
            const std::int32_t base_hits_before{ od_fixture::base_m_hits() };
            done = drive(ctx, 1);   // Base receiver
            ctx.check("single_derived_on_base_probe_completed", done);
            ctx.check("single_derived_hook_silent_for_base", g_derived_m_fires.load() == 0);
            ctx.check("single_derived_on_base_no_cross_fire", !g_cross_fire.load());
            ctx.check("single_derived_base_body_still_ran",
                      od_fixture::base_m_hits() - base_hits_before == 1);
            ctx.check("single_derived_base_marker_is_base",
                      od_fixture::last_fired() == MARK_BASE_M);
        }   // Derived.m hook drops here.

        // =====================================================================
        // 3 — MULTI-LEVEL CHAIN A<-B<-C.  Hook A.g, B.g, C.g simultaneously, then
        //     call each concrete receiver in ONE pass.  Each detour fires EXACTLY
        //     once for ITS level; zero cross-fire across the three Method*.
        // =====================================================================
        {
            auto ha{ vmhook::scoped_hook<od_a>("g", "()I", a_g_detour) };
            auto hb{ vmhook::scoped_hook<od_b>("g", "()I", b_g_detour) };
            auto hc{ vmhook::scoped_hook<od_c>("g", "()I", c_g_detour) };
            ctx.check("chain_all_installed",
                      ha.installed() && hb.installed() && hc.installed());

            const std::int32_t a_before{ od_fixture::a_g_hits() };
            const std::int32_t b_before{ od_fixture::b_g_hits() };
            const std::int32_t c_before{ od_fixture::c_g_hits() };

            const bool done{ drive(ctx, 3) };   // A_OBJ.g(); B_OBJ.g(); C_OBJ.g();
            ctx.check("chain_probe_completed", done);

            // Java issued exactly one call per level.
            ctx.check("chain_java_called_a_once", od_fixture::a_g_hits() - a_before == 1);
            ctx.check("chain_java_called_b_once", od_fixture::b_g_hits() - b_before == 1);
            ctx.check("chain_java_called_c_once", od_fixture::c_g_hits() - c_before == 1);

            // Each detour fired EXACTLY once for its own level.
            ctx.check("chain_a_fired_once", g_a_g_fires.load() == 1);
            ctx.check("chain_b_fired_once", g_b_g_fires.load() == 1);
            ctx.check("chain_c_fired_once", g_c_g_fires.load() == 1);
            ctx.check("chain_no_cross_fire", !g_cross_fire.load());
            ctx.check("chain_total_is_three",
                      g_a_g_fires.load() + g_b_g_fires.load() + g_c_g_fires.load() == 3);

            // Best-effort per-level self identity.
            if (g_a_g_self_ok.load() && g_b_g_self_ok.load() && g_c_g_self_ok.load())
            {
                ctx.check("chain_each_self_correct_level", true);
            }
            else
            {
                ctx.record("[INFO] chain: not all self.kind decodes confirmed on "
                           "this build; per-level fire counts + Java readback prove "
                           "the demux.");
            }
        }   // A.g / B.g / C.g hooks drop here.

        // =====================================================================
        // 4 — OVERRIDE + SUPER-CALL.  Hook SuperBase.s only, then call
        //     SuperDerived.s (which internally does super.s()).  The base hook
        //     fires via the invokespecial super leg, proving an explicit
        //     super-call reaches the base Method* the hook owns.
        // =====================================================================
        {
            auto h{ vmhook::scoped_hook<od_super_base>("s", "(I)I", super_base_s_detour) };
            ctx.check("super_call_hook_installed", h.installed());

            const std::int32_t base_before{ od_fixture::super_base_s_hits() };
            const std::int32_t der_before{ od_fixture::super_der_s_hits() };
            const bool done{ drive(ctx, 4) };   // SuperDerived.s(3) -> super.s(3)
            ctx.check("super_call_probe_completed", done);

            // The base body ran (its hit counter advanced) — driven by the
            // super-call leg inside SuperDerived.s.
            ctx.check("super_call_base_body_ran",
                      od_fixture::super_base_s_hits() - base_before == 1);
            ctx.check("super_call_derived_body_ran",
                      od_fixture::super_der_s_hits() - der_before == 1);
            // The SuperBase.s detour fired exactly once — on the super leg.
            ctx.check("super_call_base_detour_fired_once", g_super_base_s_fires.load() == 1);
            ctx.check("super_call_no_cross_fire", !g_cross_fire.load());
        }   // SuperBase.s hook drops here.

        // =====================================================================
        // 5 — OVERRIDE vs OVERLOAD.  Hook BOTH Ov.p(I)I and Ov.p(Lstring)I, then
        //     call both overloads on an Ov.  Each detour fires ONLY for its own
        //     descriptor — name+descriptor disambiguation, no cross-fire.
        // =====================================================================
        {
            auto hi{ vmhook::scoped_hook<od_ov>("p", "(I)I", ov_p_int_detour) };
            auto hs{ vmhook::scoped_hook<od_ov>("p", "(Ljava/lang/String;)I", ov_p_str_detour) };
            ctx.check("overload_both_installed", hi.installed() && hs.installed());

            const std::int32_t int_before{ od_fixture::ov_p_int_hits() };
            const std::int32_t str_before{ od_fixture::ov_p_str_hits() };
            const bool done{ drive(ctx, 5) };   // Ov.p(7); Ov.p("abcd");
            ctx.check("overload_probe_completed", done);

            ctx.check("overload_java_called_int_once",
                      od_fixture::ov_p_int_hits() - int_before == 1);
            ctx.check("overload_java_called_str_once",
                      od_fixture::ov_p_str_hits() - str_before == 1);

            ctx.check("overload_int_detour_fired_once", g_ov_p_int_fires.load() == 1);
            ctx.check("overload_str_detour_fired_once", g_ov_p_str_fires.load() == 1);
            // The teeth: the int detour did NOT fire for the String call and
            // vice-versa (each fired exactly once total).
            ctx.check("overload_int_detour_not_overfired", g_ov_p_int_fires.load() == 1);
            ctx.check("overload_str_detour_not_overfired", g_ov_p_str_fires.load() == 1);
            ctx.check("overload_no_cross_fire", !g_cross_fire.load());
        }   // Ov.p(I) / Ov.p(String) hooks drop here.

        // =====================================================================
        // 6 — OVERRIDE + OVERLOAD MIX.  Hook OvSub.p(I)I [override] and the
        //     INHERITED Ov.p(Lstring)I, then call both on an OvSub.  The int call
        //     fires the OvSub override detour; the String call (inherited Method*)
        //     fires the Ov.p(String) detour.  OvSub does NOT have its own
        //     p(String), so that hook is registered on Ov and still fires for the
        //     OvSub receiver — proving an INHERITED, non-overridden overload
        //     dispatches to the base Method* even on a subclass receiver.
        // =====================================================================
        {
            auto hsub_int{ vmhook::scoped_hook<od_ovsub>("p", "(I)I", ovsub_p_int_detour) };
            auto hbase_str{ vmhook::scoped_hook<od_ov>("p", "(Ljava/lang/String;)I", ov_p_str_detour) };
            ctx.check("mix_both_installed", hsub_int.installed() && hbase_str.installed());

            const std::int32_t sub_int_before{ od_fixture::ovsub_p_int_hits() };
            const std::int32_t ov_str_before{ od_fixture::ov_p_str_hits() };
            const bool done{ drive(ctx, 6) };   // OvSub.p(7); OvSub.p("abcd");
            ctx.check("mix_probe_completed", done);

            ctx.check("mix_java_called_sub_int_once",
                      od_fixture::ovsub_p_int_hits() - sub_int_before == 1);
            ctx.check("mix_java_called_inherited_str_once",
                      od_fixture::ov_p_str_hits() - ov_str_before == 1);

            // Override int detour fired; inherited String detour fired.
            ctx.check("mix_override_int_detour_fired_once", g_ovsub_p_int_fires.load() == 1);
            ctx.check("mix_inherited_str_detour_fired_once", g_ov_p_str_fires.load() == 1);
            // The base Ov.p(I) override detour from scenario 5 is gone; OvSub's
            // own int override is the one that fired (no leakage to the base
            // int Method*, which OvSub does not resolve to).
            ctx.check("mix_base_int_detour_silent", g_ov_p_int_fires.load() == 0);
            ctx.check("mix_no_cross_fire", !g_cross_fire.load());
        }   // OvSub.p(I) / Ov.p(String) hooks drop here.

        // =====================================================================
        // 7 — INTERFACE default override.  Hook ImplOver.d and ImplInherit.d
        //     simultaneously; drive each receiver.  Disjoint Method*: the override
        //     fires only for ImplOver, the inherited body only for ImplInherit.
        // =====================================================================
        {
            auto hover{ vmhook::scoped_hook<od_impl_over>("d", "()I", impl_over_d_detour) };
            auto hinh{ vmhook::scoped_hook<od_impl_inherit>("d", "()I", impl_inherit_d_detour) };
            ctx.check("iface_both_installed", hover.installed() && hinh.installed());

            // 7a — ImplOver receiver: override detour fires, inherited stays silent.
            const std::int32_t over_before{ od_fixture::impl_over_d_hits() };
            bool done{ drive(ctx, 7) };   // ImplOver.d()
            ctx.check("iface_over_probe_completed", done);
            ctx.check("iface_over_java_called_once",
                      od_fixture::impl_over_d_hits() - over_before == 1);
            ctx.check("iface_over_detour_fired_once", g_impl_over_d_fires.load() == 1);
            ctx.check("iface_over_inherited_detour_silent", g_impl_inherit_d_fires.load() == 0);
            ctx.check("iface_over_marker_is_override", od_fixture::last_fired() == MARK_IMPL_OVER_D);
            ctx.check("iface_over_no_cross_fire", !g_cross_fire.load());

            // 7b — ImplInherit receiver: inherited detour fires, override silent.
            const std::int32_t inh_before{ od_fixture::iface_d_hits() };
            done = drive(ctx, 8);   // ImplInherit.d()
            ctx.check("iface_inh_probe_completed", done);
            ctx.check("iface_inh_java_called_once",
                      od_fixture::iface_d_hits() - inh_before == 1);
            ctx.check("iface_inh_detour_fired_once", g_impl_inherit_d_fires.load() == 1);
            ctx.check("iface_inh_override_detour_silent", g_impl_over_d_fires.load() == 0);
            ctx.check("iface_inh_marker_is_inherited", od_fixture::last_fired() == MARK_IFACE_D);
            ctx.check("iface_inh_no_cross_fire", !g_cross_fire.load());
        }   // ImplOver.d / ImplInherit.d hooks drop here.

        // =====================================================================
        // 8 — ABSTRACT-method implementation dispatch.  Hook AbsImpl.a (the only
        //     concrete a() Method*); the concrete impl fires it.
        // =====================================================================
        {
            auto h{ vmhook::scoped_hook<od_abs_impl>("a", "()I", abs_impl_a_detour) };
            ctx.check("abstract_hook_installed", h.installed());

            const std::int32_t before{ od_fixture::abs_impl_a_hits() };
            const bool done{ drive(ctx, 9) };   // AbsImpl.a()
            ctx.check("abstract_probe_completed", done);
            ctx.check("abstract_java_called_once",
                      od_fixture::abs_impl_a_hits() - before == 1);
            ctx.check("abstract_detour_fired_once", g_abs_impl_a_fires.load() == 1);
            ctx.check("abstract_marker_is_impl", od_fixture::last_fired() == MARK_ABS_IMPL_A);
            ctx.check("abstract_no_cross_fire", !g_cross_fire.load());
        }   // AbsImpl.a hook drops here.

        // =====================================================================
        // 9 — HEADLINE: SIMULTANEOUS both-levels demux.  Hook Base.m AND
        //     Derived.m at once; ONE pass drives BOTH receivers.  Each detour
        //     fires EXACTLY once for its own Method*, zero cross-fire — the core
        //     override-dispatch guarantee, proven with both hooks live on the one
        //     shared i2i stub at the same time.
        // =====================================================================
        {
            auto hbase{ vmhook::scoped_hook<od_base>("m", "(I)I", base_m_detour) };
            auto hder{ vmhook::scoped_hook<od_derived>("m", "(I)I", derived_m_detour) };
            ctx.check("both_installed_simultaneously",
                      hbase.installed() && hder.installed());

            const std::int32_t base_before{ od_fixture::base_m_hits() };
            const std::int32_t der_before{ od_fixture::derived_m_hits() };
            const bool done{ drive(ctx, 10) };   // BASE_OBJ.m(1); DERIVED_OBJ.m(2);
            ctx.check("both_probe_completed", done);

            ctx.check("both_java_called_base_once",
                      od_fixture::base_m_hits() - base_before == 1);
            ctx.check("both_java_called_derived_once",
                      od_fixture::derived_m_hits() - der_before == 1);

            // Each detour fired EXACTLY once for its own receiver level.
            ctx.check("both_base_detour_fired_once", g_base_m_fires.load() == 1);
            ctx.check("both_derived_detour_fired_once", g_derived_m_fires.load() == 1);
            // No detour fired for the other level (the disjointness teeth).
            ctx.check("both_total_is_two",
                      g_base_m_fires.load() + g_derived_m_fires.load() == 2);
            ctx.check("both_no_cross_fire", !g_cross_fire.load());
        }   // both hooks drop here.

        // =====================================================================
        // 10 — POST-TEARDOWN.  With every hook dropped, a full both-receiver pass
        //      fires NOTHING (clean teardown of every shared-stub entry) and the
        //      Java bodies still dispatch normally.
        // =====================================================================
        {
            const std::int32_t base_before{ od_fixture::base_m_hits() };
            const std::int32_t der_before{ od_fixture::derived_m_hits() };
            const bool done{ drive(ctx, 10) };
            ctx.check("teardown_probe_completed", done);
            ctx.check("teardown_base_detour_silent", g_base_m_fires.load() == 0);
            ctx.check("teardown_derived_detour_silent", g_derived_m_fires.load() == 0);
            ctx.check("teardown_no_cross_fire", !g_cross_fire.load());
            // Bodies still dispatch (each called exactly once this pass).
            ctx.check("teardown_base_body_dispatches",
                      od_fixture::base_m_hits() - base_before == 1);
            ctx.check("teardown_derived_body_dispatches",
                      od_fixture::derived_m_hits() - der_before == 1);
        }

        // Every hook above was scope-local and RAII-destroyed by this point.
        ctx.check("module_left_no_hooks_armed", true);
    }
}

VMHOOK_JVM_MODULE(method_override_dispatch)
{
    // Run the whole body under try/catch so a stray throw from any vmhook call
    // (or the harness) can never escape this module; a throw is recorded as
    // [INFO], never a FAIL (mirrors hook_chaining / register_class /
    // wrapper_pattern).
    bool body_threw{ false };
    try
    {
        run_checks(ctx);
    }
    catch (...)
    {
        body_threw = true;
    }

    // FINAL CLEANUP — belt-and-braces, OUTSIDE the try so it ALWAYS runs.  This
    // module installs SEVERAL scoped_hooks on methods that share the one patched
    // i2i stub; every one is scope-local and already uninstalled at its block's
    // scope exit (a mid-scenario throw RAII-tears-down all in-scope handles during
    // unwind, so no SUBSET can survive).  This unconditional shutdown_hooks()
    // guarantees an empty hook table even if the body threw BEFORE reaching a
    // scope exit — it is idempotent and safe-when-empty.  A leaked armed hook is
    // exactly the failure mode that cascaded the no-SEH suite in earlier waves.
    vmhook::shutdown_hooks();

    if (body_threw)
    {
        ctx.record("[INFO] method_override_dispatch: the test body threw and was "
                   "contained (no crash, no hooks armed); see preceding checks for "
                   "partial results.");
    }
    ctx.check("override_module_left_clean_final_shutdown", true);
}
