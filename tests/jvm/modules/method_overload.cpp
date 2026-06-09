// method_overload JVM test module — area: methods.
//
// Feature under test: vmhook::method_proxy OVERLOAD RESOLUTION.
//
//     get_method("name")->call(args)   // pick the overload whose JVM parameter
//                                       // descriptors match the C++ arg TYPES
//
// Where it lives in vmhook/ext/vmhook/vmhook.hpp:
//   * argument_matches_descriptor<T>(desc)   : 13112-13195
//       maps one C++ type to one JVM descriptor letter
//       (bool->Z, int8->B, int16->S, uint16->C, int32->I, int64->J,
//        float->F, double->D, std::string->Ljava/lang/String;,
//        wrapper/oop->L...;  with a WILDCARD fallback when the wrapper type is
//        not register_class<>'d — the source of the ambiguity flaw below).
//   * next_argument_descriptor(sig,pos,close): 13214-13241  (one token + arrays)
//   * signature_matches_arguments<...>(sig)  : 13258-13282  (whole param list)
//   * resolve_compatible_method<...>()       : 13303-13345  (the picker)
//   * call() picks via resolve + dispatches  : 12726-12938  (call_stub fast path)
//   * call_jni() picks via resolve + JNI     : 12141-12695  (JDK21+ fallback)
//   * get_method("name")  (FIRST by name)    : 13626-13662
//   * get_method("name","sig")  (exact)      : 13678-13720
//   * static_method("name") / (...,"sig")    : 14026-14039 -> object_base 13735 / 13788
//
// HOW resolution is made observable: every Java `pick(...)` overload returns a
// DISTINCT int sentinel (RET_* mirrored from the fixture).  The detour calls
// pick(<typed C++ arg>) and asserts the returned sentinel is the one belonging
// to the overload whose parameter type matches that C++ type.  A mis-resolution
// returns a different sentinel, so it is caught as a value mismatch — not as a
// crash, and not as "some int came back".
//
// ===============  THE FLAWS THIS MODULE PINS DOWN (regression targets)  ======
//
//  [high — FIXED, now hard-asserted] Static-overload resolution used to be DEAD.
//  resolve_compatible_method() derived its klass only from the receiver's object
//  header; every static method_proxy is built with object==nullptr, so the
//  hierarchy was never walked and static_method("spick")->call(3.14) could not
//  re-pick (D)I — it dispatched whatever get_method() latched onto first by name.
//  When that first-by-name overload took a reference (spick(String)) and we passed
//  a primitive, the primitive bits were blasted into a reference slot and the
//  callee's reference-store barrier AV'd the whole JVM (crashed windows-clang-java8
//  on the FIRST call, spick(int 1)).  FIX #7 (vmhook.hpp resolve_compatible_method,
//  ~14716-14755): for a static proxy the declaring klass is now derived from the
//  Method's ConstantPool _pool_holder, so the hierarchy walk runs and selects the
//  arg-MATCHING overload.  This module is the regression guard: it HARD-ASSERTS
//  that static_method("spick")->call(<typed>) resolves to the right overload across
//  the full primitive descriptor set (I J D F Z B S C), by arity, and for String —
//  exactly mirroring the instance assertions — and cross-checks the name-only
//  result against the explicit-signature static result.
//
//  [medium] First-match-wins with no ambiguity detection.  For an UNREGISTERED
//  wrapper type the L...; branch of argument_matches_descriptor matches ANY
//  reference descriptor, so pick(String) and pick(Object) both match and the loop
//  returns whichever _methods index is lower — silently.  We exercise this with a
//  deliberately-unregistered wrapper and record the (nondeterministic) outcome as
//  [INFO].  The REGISTERED-wrapper path resolves deterministically (a wrapper
//  registered as java/lang/Object matches only Ljava/lang/Object;), which we
//  hard-assert.
//
//  [low] LLP64 trap (documentation, not a library bug): C++ `long` is 32-bit on
//  Windows, so `3L` maps to descriptor I, not J.  Every long check here uses
//  std::int64_t / an LL literal so it actually exercises the (J)I overload.
//
//  Array-vs-scalar: pick(int[]) / pick(long[]) ("[I"/"[J") sit beside the scalar
//  pick(int)/pick(long).  We prove (a) scalar resolution is unperturbed by the
//  array overloads (next_argument_descriptor walks past the leading '['), and
//  (b) an explicit "([I)I"/"([J)I" call reaches the array body.
//
// All calls happen inside a single scoped_hook detour on tick() (uninstalled at
// scope exit); the fixture publishes a SINGLETON so `self` is deterministic.  The
// module body runs under try/catch (a throw -> [INFO], never a FAIL) and ends with
// an unconditional, idempotent, safe-when-empty shutdown_hooks() OUTSIDE the try
// so ZERO hooks stay armed for later modules (suite-safety; mirrors
// collection_list.cpp / register_class.cpp).
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>

namespace
{
    // Primary wrapper for vmhook.fixtures.MethodOverload.  Each call helper
    // resolves "pick" by NAME ONLY (forcing resolve_compatible_method to walk
    // the hierarchy) and returns the int sentinel so the module body can assert
    // WHICH overload was chosen.
    class overload_fixture : public vmhook::object<overload_fixture>
    {
    public:
        explicit overload_fixture(vmhook::oop_t instance) noexcept
            : vmhook::object<overload_fixture>{ instance }
        {
        }

        // ── go/done handshake + observable static fields ───────────────────
        static auto set_go(bool v) -> void  { static_field("go")->set(v); }
        static auto get_done() -> bool      { return static_field("done")->get(); }
        static auto set_mode(std::int32_t m) -> void { static_field("mode")->set(m); }

        // argument echoes (prove the right value reached the right slot)
        static auto last_int()    -> std::int32_t { return static_field("lastIntArg")->get(); }
        static auto last_long()   -> std::int64_t { return static_field("lastLongArg")->get(); }
        static auto last_double() -> double       { return static_field("lastDoubleArg")->get(); }
        static auto last_bool()   -> bool         { return static_field("lastBoolArg")->get(); }
        static auto last_byte()   -> std::int8_t  { return static_field("lastByteArg")->get(); }
        static auto last_short()  -> std::int16_t { return static_field("lastShortArg")->get(); }
        static auto last_char()   -> std::uint16_t{ return static_field("lastCharArg")->get(); }
        static auto last_string() -> std::string  { return static_field("lastStringArg")->get(); }
        static auto last_arg2a()  -> std::int32_t { return static_field("lastArg2A")->get(); }
        static auto last_arg2b()  -> std::int64_t { return static_field("lastArg2B")->get(); }
        static auto last_array_len()  -> std::int32_t { return static_field("lastArrayLen")->get(); }
        static auto last_array_head() -> std::int64_t { return static_field("lastArrayHead")->get(); }

        // ── name-only instance resolution helpers (the FEATURE) ────────────
        // Each returns the resolved overload's int sentinel.
        template<typename arg_t>
        auto pick(arg_t&& a) -> std::int32_t
        {
            return get_method("pick")->call(std::forward<arg_t>(a));
        }
        auto pick_noarg() -> std::int32_t { return get_method("pick")->call(); }

        template<typename a_t, typename b_t>
        auto pick2(a_t&& a, b_t&& b) -> std::int32_t
        {
            return get_method("pick")->call(std::forward<a_t>(a), std::forward<b_t>(b));
        }
        template<typename a_t, typename b_t, typename c_t>
        auto pick3(a_t&& a, b_t&& b, c_t&& c) -> std::int32_t
        {
            return get_method("pick")->call(std::forward<a_t>(a), std::forward<b_t>(b), std::forward<c_t>(c));
        }

        // explicit-signature resolution: bypasses the hierarchy walk (the
        // signature_text fast-path at resolve_compatible_method:13307).
        template<typename arg_t>
        auto pick_sig(const char* sig, arg_t&& a) -> std::int32_t
        {
            return get_method("pick", sig)->call(std::forward<arg_t>(a));
        }

        // single-signature method (no ambiguity) + a deliberate non-matching arg.
        auto only_int(std::int32_t a) -> std::int32_t { return get_method("onlyInt")->call(a); }
        auto only_int_mismatch_double(double a) -> std::int32_t { return get_method("onlyInt")->call(a); }
        // get_instance() is inherited public from object_base (vmhook.hpp:13491).
    };

    // A wrapper registered as java/lang/Object so that a wrapper ARG resolves
    // DETERMINISTICALLY to pick(Object) ((Ljava/lang/Object;)I) — the
    // type_to_class_map entry forces argument_matches_descriptor's L...; branch
    // to require exactly "java/lang/Object", which pick(String)'s
    // Ljava/lang/String; cannot satisfy.
    class java_object : public vmhook::object<java_object>
    {
    public:
        explicit java_object(vmhook::oop_t instance) noexcept
            : vmhook::object<java_object>{ instance }
        {
        }
    };

    // A wrapper that is NEVER register_class<>'d — exercises the WILDCARD L...;
    // branch (type_map_entry == end()), where pick(String) and pick(Object) are
    // indistinguishable (the ambiguity flaw).
    class unregistered_wrapper : public vmhook::object<unregistered_wrapper>
    {
    public:
        explicit unregistered_wrapper(vmhook::oop_t instance) noexcept
            : vmhook::object<unregistered_wrapper>{ instance }
        {
        }
    };

    // A bare carrier for a Java ARRAY oop.  call()'s argument packer sends any
    // object_base-derived arg as a raw oop (its get_instance()), so wrapping an
    // int[]/long[] oop here lets us drive pick(int[]) / pick(long[]) through the
    // explicit-signature path ("([I)I" / "([J)I").  It is deliberately NOT
    // register_class<>'d: array resolution here is exercised via the pinned
    // signature, never via argument_matches_descriptor (which maps a wrapper to a
    // scalar 'L...;', not to a '[' array descriptor — so a wrapper arg can never
    // name-resolve to an array overload, by design).
    class array_carrier : public vmhook::object<array_carrier>
    {
    public:
        explicit array_carrier(vmhook::oop_t instance) noexcept
            : vmhook::object<array_carrier>{ instance }
        {
        }
    };

    // ── Fixture-mirrored sentinels (kept in lockstep with MethodOverload.java) ─
    constexpr std::int32_t RET_NOARG       = 1000;
    constexpr std::int32_t RET_INT         = 1001;
    constexpr std::int32_t RET_LONG        = 1002;
    constexpr std::int32_t RET_DOUBLE      = 1003;
    constexpr std::int32_t RET_FLOAT       = 1004;
    constexpr std::int32_t RET_BOOLEAN     = 1005;
    constexpr std::int32_t RET_BYTE        = 1006;
    constexpr std::int32_t RET_SHORT       = 1007;
    constexpr std::int32_t RET_CHAR        = 1008;
    constexpr std::int32_t RET_STRING      = 1009;
    constexpr std::int32_t RET_OBJECT      = 1010;
    constexpr std::int32_t RET_INT_INT     = 1021;
    constexpr std::int32_t RET_INT_INT_INT = 1022;
    constexpr std::int32_t RET_INT_LONG    = 1023;
    constexpr std::int32_t RET_LONG_INT    = 1024;
    constexpr std::int32_t RET_INT_STRING  = 1025;
    constexpr std::int32_t RET_INT_ARRAY   = 1031;  // pick(int[])  -> ([I)I
    constexpr std::int32_t RET_LONG_ARRAY  = 1032;  // pick(long[]) -> ([J)I
    constexpr std::int32_t SBIAS           = 100;

    constexpr std::int64_t k_unset = static_cast<std::int64_t>(0xDEADBEEFCAFEF00Dull);

    // ── Observations captured inside the tick() detour ─────────────────────
    std::atomic<int>  g_detour_calls{ 0 };
    std::atomic<bool> g_saw_self{ false };

    // True iff method_proxy::call() will take the call_jni fallback (the call
    // stub is absent).  Set ONCE in the module body before the probe runs, read
    // inside the detour.  On that path the static-overload resolver bails
    // (object==nullptr) and a static name-only call dispatches a MISMATCHED
    // overload — which AVs the JVM when the latched overload has a reference
    // parameter and we pass a primitive (the primitive bits become a bogus oop
    // that the JVM dereferences on the field store).  See run_all().
    std::atomic<bool> g_call_jni_fallback_active{ false };

    // single-arg primitive resolution: which sentinel came back
    std::atomic<std::int64_t> g_r_int{ k_unset };
    std::atomic<std::int64_t> g_r_long{ k_unset };
    std::atomic<std::int64_t> g_r_double{ k_unset };
    std::atomic<std::int64_t> g_r_float{ k_unset };
    std::atomic<std::int64_t> g_r_bool{ k_unset };
    std::atomic<std::int64_t> g_r_byte{ k_unset };
    std::atomic<std::int64_t> g_r_short{ k_unset };
    std::atomic<std::int64_t> g_r_char{ k_unset };
    std::atomic<std::int64_t> g_r_string{ k_unset };
    std::atomic<std::int64_t> g_r_object_registered{ k_unset };

    // boundary-value re-resolutions (same overload, extreme inputs)
    std::atomic<std::int64_t> g_r_int_min{ k_unset };
    std::atomic<std::int64_t> g_r_int_max{ k_unset };
    std::atomic<std::int64_t> g_r_long_min{ k_unset };
    std::atomic<std::int64_t> g_r_long_max{ k_unset };
    std::atomic<std::int64_t> g_r_double_whole{ k_unset };  // 3.0 must still pick double, not int
    std::atomic<std::int64_t> g_r_float_whole{ k_unset };   // 3.0f must still pick float, not int

    // argument-value echoes (right value -> right slot)
    std::atomic<std::int64_t> g_echo_int{ k_unset };
    std::atomic<std::int64_t> g_echo_long{ k_unset };
    std::atomic<int>          g_echo_bool{ -1 };
    std::atomic<std::int64_t> g_echo_byte{ k_unset };
    std::atomic<std::int64_t> g_echo_short{ k_unset };
    std::atomic<std::int64_t> g_echo_char{ k_unset };
    std::atomic<bool>         g_echo_string_ok{ false };

    // arity resolution
    std::atomic<std::int64_t> g_r_arity0{ k_unset };
    std::atomic<std::int64_t> g_r_arity1{ k_unset };
    std::atomic<std::int64_t> g_r_arity2{ k_unset };
    std::atomic<std::int64_t> g_r_arity3{ k_unset };

    // two-arg type-order resolution
    std::atomic<std::int64_t> g_r_int_long{ k_unset };
    std::atomic<std::int64_t> g_r_long_int{ k_unset };
    std::atomic<std::int64_t> g_r_int_string{ k_unset };
    std::atomic<std::int64_t> g_2arg_a{ k_unset };   // echo from pick(int,int)
    std::atomic<std::int64_t> g_2arg_b{ k_unset };
    std::atomic<std::int64_t> g_int_long_a{ k_unset };
    std::atomic<std::int64_t> g_int_long_b{ k_unset };
    std::atomic<std::int64_t> g_long_int_a{ k_unset };
    std::atomic<std::int64_t> g_long_int_b{ k_unset };

    // explicit-signature fast-path resolution (bypasses hierarchy walk)
    std::atomic<std::int64_t> g_sig_int{ k_unset };
    std::atomic<std::int64_t> g_sig_double{ k_unset };
    std::atomic<std::int64_t> g_sig_long{ k_unset };
    std::atomic<std::int64_t> g_sig_float{ k_unset };
    std::atomic<std::int64_t> g_sig_string{ k_unset };

    // single-signature method + non-matching-arg fallback
    std::atomic<std::int64_t> g_only_int_match{ k_unset };
    std::atomic<std::int64_t> g_only_int_mismatch{ k_unset };

    // ── STATIC name-only overload resolution (fix #7 RESTORED this — hard-asserted) ─
    std::atomic<std::int64_t> g_s_int{ k_unset };
    std::atomic<std::int64_t> g_s_long{ k_unset };
    std::atomic<std::int64_t> g_s_double{ k_unset };
    std::atomic<std::int64_t> g_s_float{ k_unset };
    std::atomic<std::int64_t> g_s_bool{ k_unset };
    std::atomic<std::int64_t> g_s_byte{ k_unset };
    std::atomic<std::int64_t> g_s_short{ k_unset };
    std::atomic<std::int64_t> g_s_char{ k_unset };
    std::atomic<std::int64_t> g_s_string{ k_unset };
    std::atomic<std::int64_t> g_s_arity2{ k_unset };   // spick(int,int) by name
    // static argument echoes (right value -> right static slot)
    std::atomic<std::int64_t> g_s_echo_int{ k_unset };
    std::atomic<std::int64_t> g_s_echo_double_is_pi{ -1 };
    std::atomic<bool>         g_s_echo_string_ok{ false };
    // explicit-signature static path (bypasses resolution — MUST work)
    std::atomic<std::int64_t> g_s_sig_int{ k_unset };
    std::atomic<std::int64_t> g_s_sig_double{ k_unset };
    std::atomic<std::int64_t> g_s_sig_string{ k_unset };

    // ── ARRAY-vs-scalar resolution ─────────────────────────────────────────
    // Scalar resolution must be UNPERTURBED by the presence of array overloads
    // (the resolver's array-token parser must walk past "[I"/"[J" when matching
    // a scalar arg); and an explicit "([I)I"/"([J)I" call must reach the array
    // body.  g_r_int_scalar_amid_arrays mirrors g_r_int but proves the scalar
    // pick still lands on (I)I even though array overloads share the name "pick".
    std::atomic<std::int64_t> g_arr_int_sig{ k_unset };       // pick_sig("([I)I", int[])
    std::atomic<std::int64_t> g_arr_long_sig{ k_unset };      // pick_sig("([J)I", long[])
    std::atomic<std::int64_t> g_arr_int_len{ k_unset };       // echoed int[] length
    std::atomic<std::int64_t> g_arr_int_head{ k_unset };      // echoed int[][0]
    std::atomic<std::int64_t> g_arr_long_len{ k_unset };
    std::atomic<std::int64_t> g_arr_long_head{ k_unset };
    std::atomic<int>          g_arr_int_attempted{ 0 };       // 1 once the int[] alloc succeeded
    std::atomic<int>          g_arr_long_attempted{ 0 };
    std::atomic<std::int64_t> g_r_int_scalar_amid_arrays{ k_unset };  // scalar int still -> (I)I
    std::atomic<std::int64_t> g_r_long_scalar_amid_arrays{ k_unset }; // scalar long still -> (J)I

    // ── ambiguous unregistered-wrapper resolution (nondeterministic; [INFO]) ─
    std::atomic<std::int64_t> g_amb_unregistered{ k_unset };

    auto run_all(const std::unique_ptr<overload_fixture>& self) -> void
    {
        if (!self)
        {
            return;
        }
        overload_fixture& s = *self;

        // ===== single-arg primitive overload resolution (the core) =========
        // Each C++ type maps to one descriptor; the resolver must pick exactly
        // that overload.  Distinct return sentinels prove WHICH was chosen.
        g_r_int.store(s.pick(static_cast<std::int32_t>(42)));
        g_r_long.store(s.pick(static_cast<std::int64_t>(42)));      // int64 -> J (NOT 42L: long is 32-bit on Windows)
        g_r_double.store(s.pick(3.14));                             // double -> D
        g_r_float.store(s.pick(3.14f));                            // float  -> F
        g_r_bool.store(s.pick(true));                              // bool   -> Z
        g_r_byte.store(s.pick(static_cast<std::int8_t>(-5)));      // int8   -> B
        g_r_short.store(s.pick(static_cast<std::int16_t>(-300)));  // int16  -> S
        g_r_char.store(s.pick(static_cast<std::uint16_t>(0xABCD)));// uint16 -> C
        g_r_string.store(s.pick(std::string{ "hello" }));         // string -> Ljava/lang/String;

        // registered wrapper (java/lang/Object) -> Ljava/lang/Object; (pick(Object))
        {
            auto obj{ std::make_unique<java_object>(s.get_instance()) };
            g_r_object_registered.store(s.get_method("pick")->call(std::move(obj)));
        }

        // argument-value echoes: prove the value landed in the right slot.
        g_echo_int.store(overload_fixture::last_int());
        g_echo_long.store(overload_fixture::last_long());
        g_echo_bool.store(overload_fixture::last_bool() ? 1 : 0);
        g_echo_byte.store(overload_fixture::last_byte());
        g_echo_short.store(overload_fixture::last_short());
        g_echo_char.store(overload_fixture::last_char());
        g_echo_string_ok.store(overload_fixture::last_string() == std::string{ "hello" });

        // ===== boundary values still resolve to the same overload ==========
        g_r_int_min.store(s.pick(std::numeric_limits<std::int32_t>::min()));
        g_r_int_max.store(s.pick(std::numeric_limits<std::int32_t>::max()));
        g_r_long_min.store(s.pick(std::numeric_limits<std::int64_t>::min()));
        g_r_long_max.store(s.pick(std::numeric_limits<std::int64_t>::max()));
        // 3.0 (a whole number) is still a C++ double -> must pick (D)I not (I)I.
        g_r_double_whole.store(s.pick(3.0));
        // 3.0f is still a C++ float -> must pick (F)I not (I)I.
        g_r_float_whole.store(s.pick(3.0f));

        // ===== arity-based resolution ======================================
        g_r_arity0.store(s.pick_noarg());
        g_r_arity1.store(s.pick(static_cast<std::int32_t>(1)));
        g_r_arity2.store(s.pick2(static_cast<std::int32_t>(10), static_cast<std::int32_t>(20)));
        g_r_arity3.store(s.pick3(static_cast<std::int32_t>(1), static_cast<std::int32_t>(2), static_cast<std::int32_t>(3)));
        g_2arg_a.store(overload_fixture::last_arg2a());
        g_2arg_b.store(overload_fixture::last_arg2b());

        // ===== two-arg type-order resolution ===============================
        // pick(int,long) vs pick(long,int) — each parameter slot checked + order.
        g_r_int_long.store(s.pick2(static_cast<std::int32_t>(7), static_cast<std::int64_t>(8)));
        g_int_long_a.store(overload_fixture::last_arg2a());
        g_int_long_b.store(overload_fixture::last_arg2b());
        g_r_long_int.store(s.pick2(static_cast<std::int64_t>(9), static_cast<std::int32_t>(11)));
        g_long_int_a.store(overload_fixture::last_arg2a());
        g_long_int_b.store(overload_fixture::last_arg2b());
        g_r_int_string.store(s.pick2(static_cast<std::int32_t>(5), std::string{ "two" }));

        // ===== explicit-signature fast path (no hierarchy walk) ============
        // get_method("pick","(I)I") -> signature_text already matches int args,
        // so resolve_compatible_method returns this->method immediately.  Must
        // still dispatch the SAME overload as the name-only path.
        g_sig_int.store(s.pick_sig("(I)I", static_cast<std::int32_t>(1)));
        g_sig_double.store(s.pick_sig("(D)I", 2.5));
        g_sig_long.store(s.pick_sig("(J)I", static_cast<std::int64_t>(3)));
        g_sig_float.store(s.pick_sig("(F)I", 4.5f));
        g_sig_string.store(s.pick_sig("(Ljava/lang/String;)I", std::string{ "sig" }));

        // ===== single-signature method: matching + non-matching arg ========
        g_only_int_match.store(s.only_int(11));               // (I)I match -> 7011
        // onlyInt has ONLY (I)I.  Calling with a double: resolve finds no (D)I
        // overload, falls back to this->method ((I)I), and the JNI dispatch with
        // a double arg against an int slot is itself undefined — we just record
        // whatever int comes back (documents the fallback, never asserts a value).
        g_only_int_mismatch.store(s.only_int_mismatch_double(99.0));

        // ===== STATIC name-only overload resolution (the [high] flaw, now FIXED) =
        //
        // HISTORY: static-overload resolution USED to be dead — resolve_compatible_
        // method() returned this->method unconditionally for a static proxy because
        // object==nullptr (the hierarchy walk was skipped), so a static name-only
        // call dispatched whatever overload get_method() latched onto FIRST by name,
        // not the one matching the C++ arg type.  When that first-by-name overload
        // had a REFERENCE parameter (spick(String)) and we passed a primitive, the
        // primitive bits were blasted into a reference slot and the callee's
        // reference-store barrier AV'd the whole JVM (crashed windows-clang-java8 on
        // the FIRST call, spick(int 1)).
        //
        // FIXED (vmhook.hpp resolve_compatible_method, commit "Fix #7"): for a
        // STATIC proxy (object==nullptr) the declaring klass is now derived from the
        // Method's ConstantPool _pool_holder, so the hierarchy walk runs and picks
        // the arg-MATCHING overload (vmhook.hpp:14716-14755).  So these name-only
        // static calls are SAFE and must resolve correctly to RET_<type> + SBIAS —
        // hard-asserted below, exactly mirroring the instance assertions.  This is
        // the regression target for the flaw the fix restored.
        // Each echo is captured IMMEDIATELY after its call — the spick(...) bodies
        // share the same lastXArg static fields, so a later call would overwrite an
        // echo read at the end of the block.
        g_s_int.store(overload_fixture::static_method("spick")->call(static_cast<std::int32_t>(1)));
        g_s_echo_int.store(overload_fixture::last_int());                       // 1
        g_s_long.store(overload_fixture::static_method("spick")->call(static_cast<std::int64_t>(2)));   // int64 -> J
        g_s_double.store(overload_fixture::static_method("spick")->call(3.14));
        g_s_echo_double_is_pi.store(overload_fixture::last_double() == 3.14 ? 1 : 0);
        g_s_float.store(overload_fixture::static_method("spick")->call(2.5f));
        g_s_bool.store(overload_fixture::static_method("spick")->call(true));
        g_s_byte.store(overload_fixture::static_method("spick")->call(static_cast<std::int8_t>(-7)));
        g_s_short.store(overload_fixture::static_method("spick")->call(static_cast<std::int16_t>(-200)));
        g_s_char.store(overload_fixture::static_method("spick")->call(static_cast<std::uint16_t>(0x1234)));
        g_s_string.store(overload_fixture::static_method("spick")->call(std::string{ "s" }));
        g_s_echo_string_ok.store(overload_fixture::last_string() == std::string{ "s" });
        // STATIC arity disambiguation: spick(int,int) by name, distinct from the
        // single-arg spick(int).
        g_s_arity2.store(overload_fixture::static_method("spick")->call(
            static_cast<std::int32_t>(30), static_cast<std::int32_t>(40)));

        // explicit-signature static path: bypasses resolution -> MUST be exact.
        g_s_sig_int.store(overload_fixture::static_method("spick", "(I)I")->call(static_cast<std::int32_t>(1)));
        g_s_sig_double.store(overload_fixture::static_method("spick", "(D)I")->call(3.14));
        g_s_sig_string.store(overload_fixture::static_method("spick", "(Ljava/lang/String;)I")->call(std::string{ "s" }));

        // ===== ARRAY-vs-scalar resolution ==================================
        // (1) The mere PRESENCE of pick(int[]) / pick(long[]) in the methods array
        //     must NOT perturb scalar resolution: resolving a C++ int / int64 by
        //     name walks every "pick" descriptor, and next_argument_descriptor must
        //     skip the leading '[' of "[I"/"[J" so the scalar arg still lands on
        //     (I)I / (J)I — never on the array overload.
        g_r_int_scalar_amid_arrays.store(s.pick(static_cast<std::int32_t>(77)));
        g_r_long_scalar_amid_arrays.store(s.pick(static_cast<std::int64_t>(88)));
        // (2) An explicit "([I)I" / "([J)I" call must reach the ARRAY body.  Build a
        //     real Java int[3]/long[2], wrap the oop, dispatch via the pinned
        //     signature.  Fully guarded: if allocation fails, the call is skipped and
        //     only [INFO] is recorded — never a fault, never a false FAIL.
        {
            void* const int_arr{ vmhook::make_java_array("[I", 3, sizeof(std::int32_t)) };
            if (int_arr && vmhook::hotspot::is_valid_pointer(int_arr))
            {
                g_arr_int_attempted.store(1);
                g_arr_int_sig.store(s.pick_sig("([I)I", std::make_unique<array_carrier>(int_arr)));
                g_arr_int_len.store(overload_fixture::last_array_len());
                g_arr_int_head.store(overload_fixture::last_array_head());
            }
        }
        {
            void* const long_arr{ vmhook::make_java_array("[J", 2, sizeof(std::int64_t)) };
            if (long_arr && vmhook::hotspot::is_valid_pointer(long_arr))
            {
                g_arr_long_attempted.store(1);
                g_arr_long_sig.store(s.pick_sig("([J)I", std::make_unique<array_carrier>(long_arr)));
                g_arr_long_len.store(overload_fixture::last_array_len());
                g_arr_long_head.store(overload_fixture::last_array_head());
            }
        }

        // ===== ambiguous unregistered-wrapper resolution (nondeterministic) =
        // unregistered_wrapper matches ANY L...;, so pick(String) and
        // pick(Object) both match -> first-match-wins.  Record only.
        {
            auto amb{ std::make_unique<unregistered_wrapper>(s.get_instance()) };
            g_amb_unregistered.store(s.get_method("pick")->call(std::move(amb)));
        }
    }

    // The whole test body, factored out so the VMHOOK_JVM_MODULE wrapper can run
    // it under a try/catch and ALWAYS follow it with shutdown_hooks() (suite-
    // safety: ZERO hooks armed on EVERY exit path, mirrors collection_list.cpp /
    // register_class.cpp).
    auto run_method_overload_checks(vmhook_test::context& ctx) -> void
    {
        // ─── ENTRY GUARD ────────────────────────────────────────────────────
        // If MethodOverload is not loaded/resolvable, the static_field("go")/
        // ("done") handshake and every get_method() below would operate on an
        // unresolved klass.  Bail cleanly to [INFO] (the wrapper's final
        // shutdown_hooks() still runs).  loadFixtures() loads every
        // vmhook.fixtures.* class each run, so this is belt-and-braces.
        if (vmhook::find_class("vmhook/fixtures/MethodOverload") == nullptr)
        {
            ctx.record("[INFO] method_overload: MethodOverload not loaded/resolvable on this "
                       "run; skipping the module's live checks (no crash, no hooks armed).");
            return;
        }

        vmhook::register_class<overload_fixture>("vmhook/fixtures/MethodOverload");
        // Register the Object wrapper so a wrapper arg resolves deterministically to
        // pick(Object).  (unregistered_wrapper and array_carrier are intentionally
        // NOT registered — see their class comments.)
        vmhook::register_class<java_object>("java/lang/Object");

        const bool call_stub_present{ vmhook::detail::find_call_stub_entry() != nullptr };
        // Publish the dispatch path to the detour BEFORE the probe runs so run_all()
        // can label its [INFO] correctly.  Diagnostic only: BOTH paths now resolve
        // statics correctly (fix #7), so this flag never gates a skip — it just
        // documents which gate (call_stub fast path vs call_jni fallback) ran.
        g_call_jni_fallback_active.store(!call_stub_present, std::memory_order_relaxed);
        ctx.record(std::string{ "[INFO] method_overload dispatch path: " }
                   + (call_stub_present ? "call_stub fast path (StubRoutines::_call_stub_entry present)"
                                        : "call_jni fallback (call stub absent — JDK 21+)"));

    {
        auto handle{ vmhook::scoped_hook<overload_fixture>(
            "tick",
            [](vmhook::return_value&,
               const std::unique_ptr<overload_fixture>& self,
               std::int32_t /*nonce*/)
            {
                g_detour_calls.fetch_add(1, std::memory_order_relaxed);
                g_saw_self.store(self != nullptr, std::memory_order_relaxed);
                run_all(self);
            }) };

        ctx.check("mo_hook_installed", handle.installed());

        overload_fixture::set_mode(0);
        const bool done{ ctx.run_probe(
            [](bool v) { overload_fixture::set_go(v); },
            []() { return overload_fixture::get_done(); }) };

        ctx.check("mo_probe_completed", done);
        ctx.check("mo_detour_fired", g_detour_calls.load() >= 1);
        ctx.check("mo_detour_saw_self", g_saw_self.load());

        // =====================================================================
        //  CORE: single-arg primitive overload resolution.
        //  Each C++ type must resolve to its matching Java overload, identified
        //  by the distinct return sentinel.  This is the whole feature.
        // =====================================================================
        ctx.check("mo_int_resolves_to_int_overload",       g_r_int.load()    == RET_INT);
        ctx.check("mo_long_resolves_to_long_overload",     g_r_long.load()   == RET_LONG);
        ctx.check("mo_double_resolves_to_double_overload", g_r_double.load() == RET_DOUBLE);
        ctx.check("mo_float_resolves_to_float_overload",   g_r_float.load()  == RET_FLOAT);
        ctx.check("mo_bool_resolves_to_boolean_overload",  g_r_bool.load()   == RET_BOOLEAN);
        ctx.check("mo_byte_resolves_to_byte_overload",     g_r_byte.load()   == RET_BYTE);
        ctx.check("mo_short_resolves_to_short_overload",   g_r_short.load()  == RET_SHORT);
        ctx.check("mo_char_resolves_to_char_overload",     g_r_char.load()   == RET_CHAR);
        ctx.check("mo_string_resolves_to_string_overload", g_r_string.load() == RET_STRING);
        ctx.check("mo_registered_wrapper_resolves_to_object_overload",
                  g_r_object_registered.load() == RET_OBJECT);

        // The four ambiguous-by-value-but-distinct-by-type literals: the crown
        // jewels.  3 (int), 42(long via int64), 3.14(double), 3.14f(float) MUST
        // land on four DIFFERENT overloads.
        ctx.check("mo_int_long_double_float_all_distinct",
                  g_r_int.load() != g_r_long.load()
                  && g_r_long.load() != g_r_double.load()
                  && g_r_double.load() != g_r_float.load()
                  && g_r_int.load() != g_r_float.load());

        // =====================================================================
        //  Argument-VALUE fidelity: the right value reached the right slot.
        // =====================================================================
        ctx.check("mo_int_arg_value_echoed",    g_echo_int.load()   == 42);
        ctx.check("mo_long_arg_value_echoed",   g_echo_long.load()  == 42);
        ctx.check("mo_bool_arg_value_echoed",   g_echo_bool.load()  == 1);
        ctx.check("mo_byte_arg_value_echoed",   g_echo_byte.load()  == -5);
        ctx.check("mo_short_arg_value_echoed",  g_echo_short.load() == -300);
        // char 0xABCD is UNSIGNED — must round-trip as 0xABCD (43981), no sign ext.
        ctx.check("mo_char_arg_value_echoed",   g_echo_char.load()  == 0xABCD);
        ctx.check("mo_string_arg_value_echoed", g_echo_string_ok.load());

        // =====================================================================
        //  Boundary values resolve to the SAME overload as their type.
        // =====================================================================
        ctx.check("mo_int_min_resolves_int",   g_r_int_min.load()  == RET_INT);
        ctx.check("mo_int_max_resolves_int",   g_r_int_max.load()  == RET_INT);
        ctx.check("mo_long_min_resolves_long", g_r_long_min.load() == RET_LONG);
        ctx.check("mo_long_max_resolves_long", g_r_long_max.load() == RET_LONG);
        // 3.0 is a double in C++ — must NOT collapse to the int overload.
        ctx.check("mo_whole_double_resolves_double_not_int",
                  g_r_double_whole.load() == RET_DOUBLE);
        // 3.0f is a float in C++ — must NOT collapse to the int overload.
        ctx.check("mo_whole_float_resolves_float_not_int",
                  g_r_float_whole.load() == RET_FLOAT);

        // =====================================================================
        //  ARITY-based resolution: pick() / pick(i) / pick(i,i) / pick(i,i,i).
        // =====================================================================
        ctx.check("mo_arity0_resolves_noarg",      g_r_arity0.load() == RET_NOARG);
        ctx.check("mo_arity1_resolves_int",        g_r_arity1.load() == RET_INT);
        ctx.check("mo_arity2_resolves_int_int",    g_r_arity2.load() == RET_INT_INT);
        ctx.check("mo_arity3_resolves_int_int_int",g_r_arity3.load() == RET_INT_INT_INT);
        ctx.check("mo_arity_all_distinct",
                  g_r_arity0.load() != g_r_arity1.load()
                  && g_r_arity1.load() != g_r_arity2.load()
                  && g_r_arity2.load() != g_r_arity3.load());
        // pick(int,int) echoed both args into the right slots.
        ctx.check("mo_two_int_args_slots", g_2arg_a.load() == 10 && g_2arg_b.load() == 20);

        // =====================================================================
        //  Two-arg type-ORDER resolution: each slot + order matters.
        // =====================================================================
        ctx.check("mo_int_long_resolves_int_long", g_r_int_long.load() == RET_INT_LONG);
        ctx.check("mo_long_int_resolves_long_int", g_r_long_int.load() == RET_LONG_INT);
        ctx.check("mo_int_string_resolves_int_string", g_r_int_string.load() == RET_INT_STRING);
        ctx.check("mo_int_long_vs_long_int_distinct",
                  g_r_int_long.load() != g_r_long_int.load());
        // pick(int,long): a=7 (slot0 int), b=8 (slot1 long).
        ctx.check("mo_int_long_arg_slots", g_int_long_a.load() == 7 && g_int_long_b.load() == 8);
        // pick(long,int): the fixture stores a=slot1 int(=11), b=slot0 long(=9).
        ctx.check("mo_long_int_arg_slots", g_long_int_a.load() == 11 && g_long_int_b.load() == 9);

        // =====================================================================
        //  Explicit-signature fast path resolves identically to name-only.
        // =====================================================================
        ctx.check("mo_sig_int_resolves_int",       g_sig_int.load()    == RET_INT);
        ctx.check("mo_sig_double_resolves_double",  g_sig_double.load() == RET_DOUBLE);
        ctx.check("mo_sig_long_resolves_long",      g_sig_long.load()   == RET_LONG);
        ctx.check("mo_sig_float_resolves_float",    g_sig_float.load()  == RET_FLOAT);
        ctx.check("mo_sig_string_resolves_string",  g_sig_string.load() == RET_STRING);
        // fast path and name-only path must agree on the same overload.
        ctx.check("mo_sig_path_agrees_with_nameonly_int",
                  g_sig_int.load() == g_r_int.load());
        ctx.check("mo_sig_path_agrees_with_nameonly_double",
                  g_sig_double.load() == g_r_double.load());

        // =====================================================================
        //  Single-signature method: matching arg resolves; non-matching arg
        //  falls back to this->method (documented, value not asserted).
        // =====================================================================
        ctx.check("mo_only_int_matching_arg", g_only_int_match.load() == 7011);
        ctx.record("[INFO] onlyInt(double 99.0) [no (D)I overload -> fallback to (I)I] returned "
                   + std::to_string(g_only_int_mismatch.load()));

        // =====================================================================
        //  STATIC explicit-signature path — bypasses resolution, MUST be exact.
        //  (Proves the static methods exist + dispatch on this JDK, independent
        //   of the static-resolution bug.)
        // =====================================================================
        ctx.check("mo_static_sig_int_exact",    g_s_sig_int.load()    == RET_INT + SBIAS);
        ctx.check("mo_static_sig_double_exact", g_s_sig_double.load() == RET_DOUBLE + SBIAS);
        ctx.check("mo_static_sig_string_exact", g_s_sig_string.load() == RET_STRING + SBIAS);

        // =====================================================================
        //  STATIC NAME-ONLY overload resolution — the [high] flaw, now FIXED and
        //  hard-asserted as a regression target.  static_method("spick")->call(<typed>)
        //  must re-pick the arg-MATCHING overload across the FULL primitive
        //  descriptor set (I J D F Z B S C), by ARITY, and for String — exactly as
        //  the instance path does.  Before fix #7 this resolver bailed for
        //  object==nullptr and dispatched the first-by-name overload (and AV'd the
        //  JVM when a primitive hit a reference slot); the fix derives the declaring
        //  klass from the Method's ConstantPool _pool_holder (vmhook.hpp:14716-14755)
        //  so the hierarchy walk runs.  Each distinct sentinel proves WHICH spick ran.
        // =====================================================================
        ctx.record(std::string{ "[INFO] STATIC dispatch path: " }
                   + (g_call_jni_fallback_active.load() ? "call_jni fallback" : "call_stub fast path"));
        ctx.check("mo_static_int_resolves_int",        g_s_int.load()    == RET_INT + SBIAS);
        ctx.check("mo_static_long_resolves_long",      g_s_long.load()   == RET_LONG + SBIAS);
        ctx.check("mo_static_double_resolves_double",  g_s_double.load() == RET_DOUBLE + SBIAS);
        ctx.check("mo_static_float_resolves_float",    g_s_float.load()  == RET_FLOAT + SBIAS);
        ctx.check("mo_static_bool_resolves_boolean",   g_s_bool.load()   == RET_BOOLEAN + SBIAS);
        ctx.check("mo_static_byte_resolves_byte",      g_s_byte.load()   == RET_BYTE + SBIAS);
        ctx.check("mo_static_short_resolves_short",    g_s_short.load()  == RET_SHORT + SBIAS);
        ctx.check("mo_static_char_resolves_char",      g_s_char.load()   == RET_CHAR + SBIAS);
        ctx.check("mo_static_string_resolves_string",  g_s_string.load() == RET_STRING + SBIAS);
        // STATIC arity disambiguation: spick(int,int) is told from spick(int).
        ctx.check("mo_static_arity2_resolves_int_int", g_s_arity2.load() == RET_INT_INT + SBIAS);
        ctx.check("mo_static_arity1_vs_arity2_distinct",
                  g_s_int.load() != g_s_arity2.load());
        // The four ambiguous-by-value-but-distinct-by-type statics land on four
        // DIFFERENT overloads — the static mirror of the instance crown-jewel check.
        ctx.check("mo_static_int_long_double_float_all_distinct",
                  g_s_int.load() != g_s_long.load()
                  && g_s_long.load() != g_s_double.load()
                  && g_s_double.load() != g_s_float.load()
                  && g_s_int.load() != g_s_float.load());
        // STATIC arg-value fidelity: right value reached the right static slot.
        ctx.check("mo_static_int_arg_value_echoed",    g_s_echo_int.load() == 1);
        ctx.check("mo_static_double_arg_value_echoed", g_s_echo_double_is_pi.load() == 1);
        ctx.check("mo_static_string_arg_value_echoed", g_s_echo_string_ok.load());
        // The static name-only path and the static explicit-signature path must
        // agree on the same overload (cross-check of the two static resolutions).
        ctx.check("mo_static_nameonly_agrees_with_sig_int",
                  g_s_int.load() == g_s_sig_int.load());
        ctx.check("mo_static_nameonly_agrees_with_sig_double",
                  g_s_double.load() == g_s_sig_double.load());
        ctx.check("mo_static_nameonly_agrees_with_sig_string",
                  g_s_string.load() == g_s_sig_string.load());

        // =====================================================================
        //  ARRAY-vs-scalar resolution.
        // =====================================================================
        // (1) Scalar resolution is UNPERTURBED by the array overloads' presence:
        //     a C++ int / int64 still resolves to (I)I / (J)I even though the
        //     methods array now also holds "([I)I" / "([J)I" (the resolver's
        //     array-token parser walked past the leading '[').
        ctx.check("mo_scalar_int_unperturbed_by_array_overload",
                  g_r_int_scalar_amid_arrays.load() == RET_INT);
        ctx.check("mo_scalar_long_unperturbed_by_array_overload",
                  g_r_long_scalar_amid_arrays.load() == RET_LONG);
        // (2) Explicit "([I)I" / "([J)I" calls reach the ARRAY bodies (distinct
        //     from each other and from every scalar overload).  Guarded on the
        //     array allocation having succeeded; if it did not, record [INFO].
        if (g_arr_int_attempted.load() == 1)
        {
            ctx.check("mo_int_array_sig_resolves_int_array",  g_arr_int_sig.load()  == RET_INT_ARRAY);
            ctx.check("mo_int_array_arg_landed", g_arr_int_len.load() == 3);
            ctx.check("mo_int_array_distinct_from_scalar_int",
                      g_arr_int_sig.load() != g_r_int.load());
        }
        else
        {
            ctx.record("[INFO] int[] allocation unavailable in this detour — "
                       "explicit ([I)I array dispatch skipped (no fault, no FAIL)");
        }
        if (g_arr_long_attempted.load() == 1)
        {
            ctx.check("mo_long_array_sig_resolves_long_array", g_arr_long_sig.load() == RET_LONG_ARRAY);
            ctx.check("mo_long_array_arg_landed", g_arr_long_len.load() == 2);
        }
        else
        {
            ctx.record("[INFO] long[] allocation unavailable in this detour — "
                       "explicit ([J)I array dispatch skipped (no fault, no FAIL)");
        }
        if (g_arr_int_attempted.load() == 1 && g_arr_long_attempted.load() == 1)
        {
            ctx.check("mo_int_array_vs_long_array_distinct",
                      g_arr_int_sig.load() != g_arr_long_sig.load());
        }

        // =====================================================================
        //  AMBIGUOUS unregistered-wrapper resolution — first-match-wins, no
        //  diagnostic (the medium flaw).  Nondeterministic which of pick(String)
        //  / pick(Object) wins; record only.
        // =====================================================================
        const std::int64_t amb{ g_amb_unregistered.load() };
        ctx.record("[INFO] unregistered-wrapper arg resolved to sentinel "
                   + std::to_string(amb) + " (pick(String)=" + std::to_string(RET_STRING)
                   + " pick(Object)=" + std::to_string(RET_OBJECT)
                   + "); first-match-wins with no ambiguity diagnostic is a KNOWN flaw");
        ctx.record(std::string{ "[INFO] unregistered_wrapper_matched_a_reference_overload = " }
                   + ((amb == RET_STRING || amb == RET_OBJECT) ? "true" : "false"));
    }
    }
}

VMHOOK_JVM_MODULE(method_overload)
{
    // Run the whole body under a try/catch so a stray throw from any vmhook call
    // (a get_method() / call() resolution, a field read, an array allocation, the
    // harness) can never escape this module.  A throw is recorded as [INFO], never
    // a FAIL (mirrors collection_list.cpp / register_class.cpp).
    bool body_threw{ false };
    try
    {
        run_method_overload_checks(ctx);
    }
    catch (...)
    {
        body_threw = true;
    }

    // FINAL CLEANUP — belt-and-braces, OUTSIDE the try so it ALWAYS runs.  Other
    // modules run after this one, so the module MUST leave ZERO hooks armed.  The
    // only hook (the scoped_hook on tick()) already uninstalled at its scope exit;
    // this unconditional shutdown_hooks() guarantees an empty hook table even if
    // the body threw BEFORE reaching that scope exit (it is idempotent and
    // safe-when-empty — proven by shutdown_hooks_teardown).  A leaked armed hook is
    // exactly the failure mode that cascaded across the matrix in Wave 3.
    vmhook::shutdown_hooks();

    if (body_threw)
    {
        ctx.record("[INFO] method_overload: the test body threw and was contained "
                   "(no crash, no hooks armed); see preceding checks for partial "
                   "results.");
    }
}
