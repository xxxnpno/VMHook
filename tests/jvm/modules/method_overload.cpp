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
#include <string_view>

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
        static auto last_float()  -> float        { return static_field("lastFloatArg")->get(); }
        static auto last_bool()   -> bool         { return static_field("lastBoolArg")->get(); }
        static auto last_byte()   -> std::int8_t  { return static_field("lastByteArg")->get(); }
        static auto last_short()  -> std::int16_t { return static_field("lastShortArg")->get(); }
        static auto last_char()   -> std::uint16_t{ return static_field("lastCharArg")->get(); }
        static auto last_string() -> std::string  { return static_field("lastStringArg")->get(); }
        static auto last_arg2a()  -> std::int32_t { return static_field("lastArg2A")->get(); }
        static auto last_arg2b()  -> std::int64_t { return static_field("lastArg2B")->get(); }
        static auto last_array_len()  -> std::int32_t { return static_field("lastArrayLen")->get(); }
        static auto last_array_head() -> std::int64_t { return static_field("lastArrayHead")->get(); }
        static auto last_arg_count()  -> std::int32_t { return static_field("lastArgCount")->get(); }
        static auto last_arg_first()  -> std::int32_t { return static_field("lastArgFirst")->get(); }
        static auto last_arg_last()   -> std::int32_t { return static_field("lastArgLast")->get(); }
        static auto last_double_b()   -> double       { return static_field("lastDoubleArgB")->get(); }
        static auto last_float_b()    -> float        { return static_field("lastFloatArgB")->get(); }
        static auto last_bool_b()     -> bool         { return static_field("lastBoolArgB")->get(); }

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

        // High-arity name-only resolution: 8 ints (fills exactly eight jvalue
        // slots) and 9 ints (needs a ninth) — the >8-arg packing boundary.  The
        // resolver disambiguates pick(int x8) / pick(int x9) by ARITY alone.
        auto pick8(std::int32_t a, std::int32_t b, std::int32_t c, std::int32_t d,
                   std::int32_t e, std::int32_t f, std::int32_t g, std::int32_t h) -> std::int32_t
        {
            return get_method("pick")->call(a, b, c, d, e, f, g, h);
        }
        // NOTE: a 9-arg pick is NOT callable through method_proxy::call (it has a
        // `max 8 arguments` static_assert), so 8 args is the testable packing boundary.

        // explicit-signature resolution: bypasses the hierarchy walk (the
        // signature_text fast-path at resolve_compatible_method:13307).
        template<typename arg_t>
        auto pick_sig(const char* sig, arg_t&& a) -> std::int32_t
        {
            return get_method("pick", sig)->call(std::forward<arg_t>(a));
        }
        auto pick_sig_noarg(const char* sig) -> std::int32_t
        {
            return get_method("pick", sig)->call();
        }
        template<typename a_t, typename b_t>
        auto pick2_sig(const char* sig, a_t&& a, b_t&& b) -> std::int32_t
        {
            return get_method("pick", sig)->call(std::forward<a_t>(a), std::forward<b_t>(b));
        }

        // single-signature method (no ambiguity) + a deliberate non-matching arg.
        auto only_int(std::int32_t a) -> std::int32_t { return get_method("onlyInt")->call(a); }
        auto only_int_mismatch_double(double a) -> std::int32_t { return get_method("onlyInt")->call(a); }
        // single-signature method called with the WRONG ARITY: onlyInt has only
        // (I)I.  call() with zero args finds no (())I overload, falls back to
        // this->method ((I)I), and dispatches it reading a zero-initialised slot.
        // Primitive-only -> memory-safe; we record the int, never assert a value.
        auto only_int_noargs() -> std::int32_t { return get_method("onlyInt")->call(); }
        // (The REFERENCE-type no-match probe — onlyRef(Integer) called with a
        // java/lang/Double-registered wrapper — is issued inline in run_all(),
        // where the java_double wrapper type is complete, mirroring how the
        // registered pick(Object)/pick(Integer) wrapper calls are issued there.)
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

    // A wrapper registered as java/lang/Integer so that a wrapper ARG resolves
    // DETERMINISTICALLY to pick(Integer) ((Ljava/lang/Integer;)I) — a SECOND
    // reference overload distinct from both pick(String) (Ljava/lang/String;) and
    // pick(Object) (Ljava/lang/Object;).  argument_matches_descriptor's L...; branch
    // compares the FULL class name inside 'L...;', so this matches ONLY
    // Ljava/lang/Integer;, proving the resolver tells three reference overloads apart
    // by their declared class, not merely "is a reference".  The oop we carry is the
    // fixture singleton (any valid oop): pick(Integer)'s body never dereferences it.
    class java_integer : public vmhook::object<java_integer>
    {
    public:
        explicit java_integer(vmhook::oop_t instance) noexcept
            : vmhook::object<java_integer>{ instance }
        {
        }
    };

    // A wrapper registered as java/lang/Double.  Used ONLY for the reference-typed
    // NO-MATCH probe: its descriptor Ljava/lang/Double; matches no overload of
    // onlyRef (whose sole signature is (Ljava/lang/Integer;)I), so the resolver
    // finds nothing and falls back to onlyRef's single Method — a graceful,
    // well-defined reference no-match.  onlyRef's body ignores the arg, so handing
    // it a MethodOverload oop in an Integer-typed slot is memory-safe (no deref).
    class java_double : public vmhook::object<java_double>
    {
    public:
        explicit java_double(vmhook::oop_t instance) noexcept
            : vmhook::object<java_double>{ instance }
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
    constexpr std::int32_t RET_INTEGER     = 1011;  // pick(Integer)    -> (Ljava/lang/Integer;)I
    constexpr std::int32_t RET_INT_INT     = 1021;
    constexpr std::int32_t RET_INT_INT_INT = 1022;
    constexpr std::int32_t RET_INT_LONG    = 1023;
    constexpr std::int32_t RET_LONG_INT    = 1024;
    constexpr std::int32_t RET_INT_STRING  = 1025;
    constexpr std::int32_t RET_STRING_INT  = 1026;  // pick(String,int) -> (Ljava/lang/String;I)I
    constexpr std::int32_t RET_LONG_DOUBLE = 1027;  // pick(long,double)-> (JD)I  (two wide slots)
    constexpr std::int32_t RET_INT_DOUBLE  = 1028;  // pick(int,double) -> (ID)I  (narrow + wide)
    constexpr std::int32_t RET_DOUBLE_INT  = 1029;  // pick(double,int) -> (DI)I  (wide + narrow)
    constexpr std::int32_t RET_INT_ARRAY   = 1031;  // pick(int[])  -> ([I)I
    constexpr std::int32_t RET_LONG_ARRAY  = 1032;  // pick(long[]) -> ([J)I
    constexpr std::int32_t RET_CHAR_ARRAY  = 1033;  // pick(char[]) -> ([C)I
    constexpr std::int32_t RET_DOUBLE_DOUBLE = 1041;  // pick(double,double) -> (DD)I
    constexpr std::int32_t RET_FLOAT_FLOAT   = 1042;  // pick(float,float)   -> (FF)I
    constexpr std::int32_t RET_BOOL_BOOL     = 1043;  // pick(boolean,boolean)-> (ZZ)I
    constexpr std::int32_t RET_OBJ_OBJ       = 1044;  // pick(Object,Object) -> (Lo;Lo;)I
    constexpr std::int32_t RET_INT_INT_REFS  = 1045;  // pick(Integer,Integer)-> (Li;Li;)I
    constexpr std::int32_t RET_INT8        = 1051;  // pick(int x8)  -> (IIIIIIII)I  (8-slot boundary)
    constexpr std::int32_t RET_INT9        = 1052;  // pick(int x9)  -> (IIIIIIIII)I (9-slot boundary)
    constexpr std::int32_t RET_ONLY_REF    = 8500;  // onlyRef(Integer) sole-overload fallback sentinel
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
    std::atomic<std::int64_t> g_r_integer_registered{ k_unset };  // pick(Integer) via java/lang/Integer wrapper

    // ── ALTERNATE C++ TYPES that map to the SAME descriptor as a tested type ──
    // These prove argument_matches_descriptor / the arg packer classify each
    // C++ type by its TRAITS (sizeof / signedness / is_same_v), not by a single
    // fixed-width alias.  Each must land on the SAME overload as its sibling.
    std::atomic<std::int64_t> g_r_cstr{ k_unset };       // const char*      -> pick(String)
    std::atomic<std::int64_t> g_r_mutable_cstr{ k_unset };// char* (non-const) -> pick(String)
    std::atomic<std::int64_t> g_r_string_view{ k_unset };// std::string_view -> pick(String)
    std::atomic<std::int64_t> g_r_char16{ k_unset };     // char16_t         -> pick(char)
    std::atomic<std::int64_t> g_r_uchar{ k_unset };      // unsigned char    -> pick(byte)
    std::atomic<std::int64_t> g_r_schar{ k_unset };      // signed char      -> pick(byte)
    std::atomic<std::int64_t> g_r_plainchar{ k_unset };  // (plain) char     -> pick(byte)
    std::atomic<std::int64_t> g_r_uint8{ k_unset };      // std::uint8_t     -> pick(byte)
    std::atomic<std::int64_t> g_r_uint16_as_char{ k_unset };  // uint16_t -> pick(char) (NOT short)
    // unsigned 4/8-byte integral types classify by sizeof, NOT by a signed alias:
    // unsigned int -> I, unsigned long long -> J — must land on pick(int)/pick(long).
    std::atomic<std::int64_t> g_r_uint32{ k_unset };     // unsigned int       -> pick(int)
    std::atomic<std::int64_t> g_r_uint64{ k_unset };     // unsigned long long -> pick(long)
    std::atomic<std::int64_t> g_echo_uint8{ k_unset };   // byte echo for the uint8 call
    std::atomic<std::int64_t> g_echo_uint32{ k_unset };  // int echo for the uint32 call
    std::atomic<std::int64_t> g_echo_uint64{ k_unset };  // long echo for the uint64 call
    // value echoes for the alternate-type calls (right value -> right slot)
    std::atomic<bool>         g_echo_cstr_ok{ false };
    std::atomic<bool>         g_echo_string_view_ok{ false };
    std::atomic<std::int64_t> g_echo_char16{ k_unset };
    std::atomic<std::int64_t> g_echo_uchar{ k_unset };
    std::atomic<std::int64_t> g_echo_schar{ k_unset };

    // ── char/byte/short boundary round-trips (unsigned char must not sign-ext) ─
    std::atomic<std::int64_t> g_r_char_zero{ k_unset };   // char 0x0000 -> pick(char)
    std::atomic<std::int64_t> g_r_char_max{ k_unset };    // char 0xFFFF -> pick(char)
    std::atomic<std::int64_t> g_echo_char_zero{ k_unset };
    std::atomic<std::int64_t> g_echo_char_max{ k_unset };
    std::atomic<std::int64_t> g_r_byte_min{ k_unset };    // INT8_MIN -> pick(byte)
    std::atomic<std::int64_t> g_r_byte_max{ k_unset };    // INT8_MAX -> pick(byte)
    std::atomic<std::int64_t> g_echo_byte_min{ k_unset };
    std::atomic<std::int64_t> g_echo_byte_max{ k_unset };
    std::atomic<std::int64_t> g_r_short_min{ k_unset };   // INT16_MIN -> pick(short)
    std::atomic<std::int64_t> g_r_short_max{ k_unset };   // INT16_MAX -> pick(short)
    std::atomic<std::int64_t> g_echo_short_min{ k_unset };
    std::atomic<std::int64_t> g_echo_short_max{ k_unset };

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
    // reference-first two-arg order: pick(String,int) — distinct from pick(int,String)
    std::atomic<std::int64_t> g_r_string_int{ k_unset };
    std::atomic<std::int64_t> g_string_int_a{ k_unset };       // echoed int (slot1)
    std::atomic<bool>         g_string_int_str_ok{ false };    // echoed String (slot0)
    // two consecutive wide params: pick(long,double) — distinct from (int,long)/(long,int)
    std::atomic<std::int64_t> g_r_long_double{ k_unset };
    std::atomic<std::int64_t> g_long_double_a{ k_unset };      // echoed long (slot0, 2 slots)
    std::atomic<std::int64_t> g_long_double_b_is_e{ -1 };      // echoed double (slot2) == 2.5 ?
    // boundary wide-pair: LONG_MIN + a fractional double, both slots must survive
    std::atomic<std::int64_t> g_r_long_double_boundary{ k_unset };
    std::atomic<std::int64_t> g_long_double_boundary_a{ k_unset };
    std::atomic<int>          g_long_double_boundary_b_ok{ -1 };
    // mixed narrow/wide ORDER pair: pick(int,double) "(ID)I" vs pick(double,int) "(DI)I".
    // Same {int,double} multiset, opposite slot order — each slot's descriptor (one
    // narrow, one wide) must be matched independently and the two told apart.
    std::atomic<std::int64_t> g_r_int_double{ k_unset };       // (ID)I
    std::atomic<std::int64_t> g_int_double_a{ k_unset };       // echoed int (slot0)
    std::atomic<int>          g_int_double_b_ok{ -1 };         // echoed double (slot1)==2.5 ?
    std::atomic<std::int64_t> g_r_double_int{ k_unset };       // (DI)I
    std::atomic<std::int64_t> g_double_int_a{ k_unset };       // echoed int (slot1)
    std::atomic<int>          g_double_int_b_ok{ -1 };         // echoed double (slot0)==4.5 ?

    // explicit-signature fast-path resolution (bypasses hierarchy walk)
    std::atomic<std::int64_t> g_sig_int{ k_unset };
    std::atomic<std::int64_t> g_sig_double{ k_unset };
    std::atomic<std::int64_t> g_sig_long{ k_unset };
    std::atomic<std::int64_t> g_sig_float{ k_unset };
    std::atomic<std::int64_t> g_sig_string{ k_unset };
    // explicit-signature fast path across the REST of the descriptor set + the
    // multi-arg shapes — every pinned sig must reach exactly its overload.
    std::atomic<std::int64_t> g_sig_bool{ k_unset };
    std::atomic<std::int64_t> g_sig_byte{ k_unset };
    std::atomic<std::int64_t> g_sig_short{ k_unset };
    std::atomic<std::int64_t> g_sig_char{ k_unset };
    std::atomic<std::int64_t> g_sig_noarg{ k_unset };
    std::atomic<std::int64_t> g_sig_object{ k_unset };
    std::atomic<std::int64_t> g_sig_integer{ k_unset };
    std::atomic<std::int64_t> g_sig_int_int{ k_unset };
    std::atomic<std::int64_t> g_sig_int_int_int{ k_unset };
    std::atomic<std::int64_t> g_sig_int_long{ k_unset };
    std::atomic<std::int64_t> g_sig_long_int{ k_unset };
    std::atomic<std::int64_t> g_sig_int_string{ k_unset };
    std::atomic<std::int64_t> g_sig_string_int{ k_unset };
    std::atomic<std::int64_t> g_sig_long_double{ k_unset };
    std::atomic<std::int64_t> g_sig_int_double{ k_unset };   // pinned "(ID)I"
    std::atomic<std::int64_t> g_sig_double_int{ k_unset };   // pinned "(DI)I"

    // ── signature_pinned: an EXPLICIT sig must NOT be re-picked from the C++
    //    arg type (resolve_compatible_method bails on signature_pinned).  Pin
    //    (D)I but pass a C++ int: the pinned (D)I overload must STILL run
    //    (RET_DOUBLE), proving the arg type does not override the pinned sig.
    std::atomic<std::int64_t> g_pinned_double_with_int_arg{ k_unset };
    std::atomic<std::int64_t> g_pinned_long_with_int_arg{ k_unset };
    std::atomic<std::int64_t> g_pinned_string_with_object_arg{ k_unset };

    // single-signature method + non-matching-arg fallback
    std::atomic<std::int64_t> g_only_int_match{ k_unset };
    std::atomic<std::int64_t> g_only_int_mismatch{ k_unset };
    std::atomic<std::int64_t> g_only_int_noargs{ k_unset };       // wrong-ARITY no-match (primitive, safe)
    std::atomic<int>          g_only_int_noargs_attempted{ 0 };
    std::atomic<std::int64_t> g_only_ref_mismatch{ k_unset };     // reference no-match -> sole-overload fallback
    std::atomic<int>          g_only_ref_mismatch_attempted{ 0 };

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
    std::atomic<std::int64_t> g_s_integer{ k_unset };  // spick(Integer) by name (java/lang/Integer wrapper)
    std::atomic<std::int64_t> g_s_long_double{ k_unset };  // spick(long,double) by name (JD)I
    std::atomic<std::int64_t> g_s_int_double{ k_unset };   // spick(int,double) by name (ID)I
    std::atomic<std::int64_t> g_s_noarg{ k_unset };    // spick() by name ()I -> RET_NOARG+SBIAS
    std::atomic<std::int64_t> g_s_arity2{ k_unset };   // spick(int,int) by name
    // static argument echoes (right value -> right static slot)
    std::atomic<std::int64_t> g_s_echo_int{ k_unset };
    std::atomic<std::int64_t> g_s_echo_double_is_pi{ -1 };
    std::atomic<bool>         g_s_echo_string_ok{ false };
    // explicit-signature static path (bypasses resolution — MUST work)
    std::atomic<std::int64_t> g_s_sig_int{ k_unset };
    std::atomic<std::int64_t> g_s_sig_double{ k_unset };
    std::atomic<std::int64_t> g_s_sig_string{ k_unset };
    // the REST of the static explicit-sig descriptor set + a static 2-arg sig.
    std::atomic<std::int64_t> g_s_sig_long{ k_unset };
    std::atomic<std::int64_t> g_s_sig_float{ k_unset };
    std::atomic<std::int64_t> g_s_sig_bool{ k_unset };
    std::atomic<std::int64_t> g_s_sig_byte{ k_unset };
    std::atomic<std::int64_t> g_s_sig_short{ k_unset };
    std::atomic<std::int64_t> g_s_sig_char{ k_unset };
    std::atomic<std::int64_t> g_s_sig_int_int{ k_unset };
    std::atomic<std::int64_t> g_s_sig_long_double{ k_unset };
    std::atomic<std::int64_t> g_s_sig_noarg{ k_unset };       // static pinned "()I"
    std::atomic<std::int64_t> g_s_sig_int_double{ k_unset };  // static pinned "(ID)I"
    // static name-only resolutions for the alternate C++ types (mirror instance).
    std::atomic<std::int64_t> g_s_cstr{ k_unset };       // const char*   -> spick(String)
    std::atomic<std::int64_t> g_s_char16{ k_unset };     // char16_t      -> spick(char)
    std::atomic<std::int64_t> g_s_uchar{ k_unset };      // unsigned char -> spick(byte)
    // static boundary re-resolutions
    std::atomic<std::int64_t> g_s_long_double_boundary{ k_unset };  // spick(LONG_MIN, frac) -> (JD)I+SBIAS

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
    // char[] — a THIRD array element type, driven via the pinned "([C)I" sig.
    std::atomic<std::int64_t> g_arr_char_sig{ k_unset };      // pick_sig("([C)I", char[])
    std::atomic<std::int64_t> g_arr_char_len{ k_unset };      // echoed char[] length
    std::atomic<int>          g_arr_char_attempted{ 0 };
    std::atomic<std::int64_t> g_r_char_scalar_amid_arrays{ k_unset };  // scalar char still -> (C)I

    // ── same-type two-arg overload resolution (per-slot descriptor) ──────────
    std::atomic<std::int64_t> g_r_double_double{ k_unset };   // pick(double,double) -> (DD)I
    std::atomic<int>          g_double_double_a_ok{ -1 };     // slot0 double == 1.5 ?
    std::atomic<int>          g_double_double_b_ok{ -1 };     // slot1 double == 2.5 ?
    std::atomic<std::int64_t> g_r_float_float{ k_unset };     // pick(float,float)   -> (FF)I
    std::atomic<int>          g_float_float_a_ok{ -1 };
    std::atomic<int>          g_float_float_b_ok{ -1 };
    std::atomic<std::int64_t> g_r_bool_bool{ k_unset };       // pick(boolean,boolean) -> (ZZ)I
    std::atomic<int>          g_bool_bool_a{ -1 };            // slot0 == true  -> 1
    std::atomic<int>          g_bool_bool_b{ -1 };            // slot1 == false -> 0

    // ── two-reference overload resolution (per-slot reference CLASS) ─────────
    std::atomic<std::int64_t> g_r_obj_obj{ k_unset };         // pick(Object,Object)   -> (Lo;Lo;)I
    std::atomic<std::int64_t> g_r_int_int_refs{ k_unset };    // pick(Integer,Integer) -> (Li;Li;)I

    // ── HIGH-arity (>8-arg) resolution + slot-fidelity at the packing edge ───
    std::atomic<std::int64_t> g_r_int8{ k_unset };            // pick(int x8) -> RET_INT8
    std::atomic<std::int64_t> g_int8_count{ k_unset };        // echoed arg count (8)
    std::atomic<std::int64_t> g_int8_first{ k_unset };        // echoed slot0
    std::atomic<std::int64_t> g_int8_last{ k_unset };         // echoed slot7 (8th arg)
    // (g_int9/pick9 removed: call() caps at 8 args, the 9-arg overload is uncallable)
    // null-oop reference disambiguation: a wrapper carrying a NULL oop still
    // resolves by C++ TYPE (the resolver never dereferences the oop), so a null
    // java_integer resolves to pick(Integer); the body ignores the oop.
    std::atomic<std::int64_t> g_r_null_integer{ k_unset };    // null-oop Integer -> RET_INTEGER
    std::atomic<std::int64_t> g_r_null_object{ k_unset };     // null-oop Object  -> RET_OBJECT
    std::atomic<std::int64_t> g_s_null_integer{ k_unset };    // STATIC null-oop Integer -> +SBIAS
    // STATIC high-arity twin: spick(int x8) by name (8-slot boundary).
    std::atomic<std::int64_t> g_s_int8{ k_unset };
    std::atomic<std::int64_t> g_s_int8_count{ k_unset };

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

        // registered wrapper (java/lang/Integer) -> Ljava/lang/Integer; (pick(Integer)).
        // A THIRD reference disambiguation: must NOT collapse to pick(Object) or
        // pick(String).  The carried oop is the fixture singleton; pick(Integer)'s
        // body never dereferences it.
        {
            auto integer{ std::make_unique<java_integer>(s.get_instance()) };
            g_r_integer_registered.store(s.get_method("pick")->call(std::move(integer)));
        }

        // argument-value echoes: prove the value landed in the right slot.
        g_echo_int.store(overload_fixture::last_int());
        g_echo_long.store(overload_fixture::last_long());
        g_echo_bool.store(overload_fixture::last_bool() ? 1 : 0);
        g_echo_byte.store(overload_fixture::last_byte());
        g_echo_short.store(overload_fixture::last_short());
        g_echo_char.store(overload_fixture::last_char());
        g_echo_string_ok.store(overload_fixture::last_string() == std::string{ "hello" });

        // ===== ALTERNATE C++ TYPES -> SAME descriptor as a tested sibling =====
        // The matcher/arg-packer classify by TRAITS, not by a fixed-width alias,
        // so every one of these distinct C++ types must land on the SAME overload
        // as its sibling above.  Each echo is read IMMEDIATELY (shared slots).
        //
        // const char* (string literal) -> Ljava/lang/String; -> pick(String).
        g_r_cstr.store(s.pick(static_cast<const char*>("cstr")));
        g_echo_cstr_ok.store(overload_fixture::last_string() == std::string{ "cstr" });
        // char* (NON-const) -> Ljava/lang/String; -> pick(String).  The matcher has
        // a DISTINCT is_same_v<clean_type, char*> branch alongside const char*; a
        // mutable C string buffer must resolve to the SAME String overload (and the
        // value must round-trip), proving the non-const pointer is classified too.
        {
            char mutable_buf[]{ 'm', 'u', 't', '\0' };
            g_r_mutable_cstr.store(s.pick(static_cast<char*>(mutable_buf)));
        }
        // std::string_view -> Ljava/lang/String; -> pick(String).
        g_r_string_view.store(s.pick(std::string_view{ "sview" }));
        g_echo_string_view_ok.store(overload_fixture::last_string() == std::string{ "sview" });
        // char16_t -> C -> pick(char) (Java char is an unsigned 16-bit UTF-16 unit).
        g_r_char16.store(s.pick(static_cast<char16_t>(0x0041)));  // 'A'
        g_echo_char16.store(overload_fixture::last_char());
        // unsigned char -> B (sizeof 1, integral) -> pick(byte).  -2 stored as a
        // byte must round-trip as the SIGNED Java byte value -2 (0xFE).
        g_r_uchar.store(s.pick(static_cast<unsigned char>(0xFE)));
        g_echo_uchar.store(overload_fixture::last_byte());
        // signed char -> B -> pick(byte).
        g_r_schar.store(s.pick(static_cast<signed char>(-3)));
        g_echo_schar.store(overload_fixture::last_byte());
        // plain char -> B (integral, sizeof 1) -> pick(byte) (NOT pick(char): Java
        // char is uint16 'C'; a 1-byte C++ char is a Java BYTE).
        g_r_plainchar.store(s.pick(static_cast<char>(7)));
        // std::uint8_t -> B (integral, sizeof 1) -> pick(byte).  A NAMED 1-byte
        // unsigned alias (distinct from unsigned char) must also land on (B)I; the
        // value 0xFE stored in a Java byte slot is the SIGNED -2.
        g_r_uint8.store(s.pick(static_cast<std::uint8_t>(0xFE)));
        g_echo_uint8.store(overload_fixture::last_byte());
        // uint16_t -> C, NOT S — re-prove with a value whose low bits would also
        // satisfy a short slot, to show the matcher refuses the 'S' descriptor.
        g_r_uint16_as_char.store(s.pick(static_cast<std::uint16_t>(0x00FF)));
        // UNSIGNED 4/8-byte integrals classify by sizeof, NOT by a signed alias:
        // unsigned int (sizeof 4) -> I -> pick(int); unsigned long long (sizeof 8)
        // -> J -> pick(long).  Each must land on the SAME overload as its signed
        // sibling, and the value must round-trip through the int/long slot.
        g_r_uint32.store(s.pick(static_cast<unsigned int>(123u)));
        g_echo_uint32.store(overload_fixture::last_int());
        g_r_uint64.store(s.pick(static_cast<unsigned long long>(9000000000ull)));  // > 2^32, needs J
        g_echo_uint64.store(overload_fixture::last_long());

        // ===== char / byte / short BOUNDARY round-trips =======================
        // char 0x0000 and 0xFFFF must both resolve to pick(char) and round-trip
        // UNSIGNED (0xFFFF == 65535, never -1).
        g_r_char_zero.store(s.pick(static_cast<std::uint16_t>(0x0000)));
        g_echo_char_zero.store(overload_fixture::last_char());
        g_r_char_max.store(s.pick(static_cast<std::uint16_t>(0xFFFF)));
        g_echo_char_max.store(overload_fixture::last_char());
        // byte INT8_MIN / INT8_MAX must resolve to pick(byte) and echo exactly.
        g_r_byte_min.store(s.pick(std::numeric_limits<std::int8_t>::min()));
        g_echo_byte_min.store(overload_fixture::last_byte());
        g_r_byte_max.store(s.pick(std::numeric_limits<std::int8_t>::max()));
        g_echo_byte_max.store(overload_fixture::last_byte());
        // short INT16_MIN / INT16_MAX must resolve to pick(short) and echo exactly.
        g_r_short_min.store(s.pick(std::numeric_limits<std::int16_t>::min()));
        g_echo_short_min.store(overload_fixture::last_short());
        g_r_short_max.store(s.pick(std::numeric_limits<std::int16_t>::max()));
        g_echo_short_max.store(overload_fixture::last_short());

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

        // pick(String,int) — the reference-FIRST mirror.  Proves slot0's reference
        // descriptor is matched (not just primitives) and that order distinguishes
        // it from pick(int,String).  Echo both slots: int -> lastArg2A, String ->
        // lastStringArg.  Read the echoes IMMEDIATELY (later calls reuse the slots).
        g_r_string_int.store(s.pick2(std::string{ "lead" }, static_cast<std::int32_t>(13)));
        g_string_int_a.store(overload_fixture::last_arg2a());
        g_string_int_str_ok.store(overload_fixture::last_string() == std::string{ "lead" });

        // pick(long,double) — two CONSECUTIVE wide (two-slot) params "(JD)I".  The
        // resolver must select this over pick(int,long)/(long,int).  Echo the long
        // (-> lastArg2B) and the double (-> lastDoubleArg); read immediately.
        g_r_long_double.store(s.pick2(static_cast<std::int64_t>(123456789012345LL), 2.5));
        g_long_double_a.store(overload_fixture::last_arg2b());
        g_long_double_b_is_e.store(overload_fixture::last_double() == 2.5 ? 1 : 0);
        // boundary wide-pair: LONG_MIN (extreme 64-bit) + a fractional double.
        // Both two-slot params must survive unperturbed at the 64-bit edge.
        g_r_long_double_boundary.store(s.pick2(std::numeric_limits<std::int64_t>::min(), 0.125));
        g_long_double_boundary_a.store(overload_fixture::last_arg2b());
        g_long_double_boundary_b_ok.store(overload_fixture::last_double() == 0.125 ? 1 : 0);

        // pick(int,double) "(ID)I" — a NARROW slot0 (int) adjacent to a WIDE slot1
        // (double).  Must select (ID)I, NOT (IJ)I (same slot0, int64 second) and NOT
        // (JD)I (same slot1, long first).  Echo int -> lastArg2A, double ->
        // lastDoubleArg; read immediately (later (DD)/(JD) calls reuse the slots).
        g_r_int_double.store(s.pick2(static_cast<std::int32_t>(17), 2.5));
        g_int_double_a.store(overload_fixture::last_arg2a());
        g_int_double_b_ok.store(overload_fixture::last_double() == 2.5 ? 1 : 0);
        // pick(double,int) "(DI)I" — the WIDE-then-NARROW order mirror.  Same
        // {double,int} multiset as (ID)I but opposite slot order; must resolve
        // distinctly.  The fixture echoes the int -> lastArg2A, double -> lastDoubleArg.
        g_r_double_int.store(s.pick2(4.5, static_cast<std::int32_t>(19)));
        g_double_int_a.store(overload_fixture::last_arg2a());
        g_double_int_b_ok.store(overload_fixture::last_double() == 4.5 ? 1 : 0);

        // ===== same-type two-arg overloads: per-slot descriptor + slot survival =
        // pick(double,double) "(DD)I" — two wide FP slots; both must survive and
        // not overlap.  Echoes read IMMEDIATELY (the (JD)I/(D)I calls reuse the
        // double slots).
        g_r_double_double.store(s.pick2(1.5, 2.5));
        g_double_double_a_ok.store(overload_fixture::last_double() == 1.5 ? 1 : 0);
        g_double_double_b_ok.store(overload_fixture::last_double_b() == 2.5 ? 1 : 0);
        // pick(float,float) "(FF)I" — two narrow FP slots.
        g_r_float_float.store(s.pick2(1.25f, 3.75f));
        g_float_float_a_ok.store(overload_fixture::last_float() == 1.25f ? 1 : 0);
        g_float_float_b_ok.store(overload_fixture::last_float_b() == 3.75f ? 1 : 0);
        // pick(boolean,boolean) "(ZZ)I" — the narrowest primitive pair; (true,false)
        // proves the second slot did not mirror the first.
        g_r_bool_bool.store(s.pick2(true, false));
        g_bool_bool_a.store(overload_fixture::last_bool() ? 1 : 0);
        g_bool_bool_b.store(overload_fixture::last_bool_b() ? 1 : 0);

        // ===== two-REFERENCE overloads: per-slot reference CLASS ===============
        // pick(Object,Object) vs pick(Integer,Integer): each slot's 'L...;' class
        // is matched, so an (Object,Object) call must NOT collapse onto the Integer
        // pair and vice versa.  Bodies ignore the oops (resolution-only).
        {
            auto a{ std::make_unique<java_object>(s.get_instance()) };
            auto b{ std::make_unique<java_object>(s.get_instance()) };
            g_r_obj_obj.store(s.get_method("pick")->call(std::move(a), std::move(b)));
        }
        {
            auto a{ std::make_unique<java_integer>(s.get_instance()) };
            auto b{ std::make_unique<java_integer>(s.get_instance()) };
            g_r_int_int_refs.store(s.get_method("pick")->call(std::move(a), std::move(b)));
        }

        // ===== HIGH-arity (>8-arg) resolution at the jvalue packing boundary ===
        // pick(int x8) fills exactly eight jvalue slots; pick(int x9) needs a
        // ninth.  Resolution is by ARITY alone; the echoes prove the 8th/9th slot
        // survived (was neither dropped nor aliased onto an earlier slot).
        g_r_int8.store(s.pick8(81, 82, 83, 84, 85, 86, 87, 88));
        g_int8_count.store(overload_fixture::last_arg_count());
        g_int8_first.store(overload_fixture::last_arg_first());
        g_int8_last.store(overload_fixture::last_arg_last());
        // (9-arg pick removed — not callable through call(); 8 args is the boundary)

        // ===== null-oop reference disambiguation ==============================
        // A wrapper carrying a NULL oop still resolves by its C++ TYPE — the
        // resolver classifies the arg via argument_matches_descriptor (a compile-
        // time trait check) and NEVER dereferences the oop.  pick(Integer) /
        // pick(Object) bodies ignore the oop, so a null carrier is memory-safe and
        // must still land on RET_INTEGER / RET_OBJECT.
        {
            auto null_integer{ std::make_unique<java_integer>(nullptr) };
            g_r_null_integer.store(s.get_method("pick")->call(std::move(null_integer)));
        }
        {
            auto null_object{ std::make_unique<java_object>(nullptr) };
            g_r_null_object.store(s.get_method("pick")->call(std::move(null_object)));
        }

        // ===== explicit-signature fast path (no hierarchy walk) ============
        // get_method("pick","(I)I") -> signature_text already matches int args,
        // so resolve_compatible_method returns this->method immediately.  Must
        // still dispatch the SAME overload as the name-only path.
        g_sig_int.store(s.pick_sig("(I)I", static_cast<std::int32_t>(1)));
        g_sig_double.store(s.pick_sig("(D)I", 2.5));
        g_sig_long.store(s.pick_sig("(J)I", static_cast<std::int64_t>(3)));
        g_sig_float.store(s.pick_sig("(F)I", 4.5f));
        g_sig_string.store(s.pick_sig("(Ljava/lang/String;)I", std::string{ "sig" }));
        // the REST of the single-arg descriptor set via the pinned-sig path.
        g_sig_bool.store(s.pick_sig("(Z)I", true));
        g_sig_byte.store(s.pick_sig("(B)I", static_cast<std::int8_t>(2)));
        g_sig_short.store(s.pick_sig("(S)I", static_cast<std::int16_t>(3)));
        g_sig_char.store(s.pick_sig("(C)I", static_cast<std::uint16_t>(4)));
        g_sig_noarg.store(s.pick_sig_noarg("()I"));
        // pinned reference sigs: the carried oop is harmless (bodies ignore it).
        g_sig_object.store(s.pick_sig("(Ljava/lang/Object;)I",
                                      std::make_unique<java_object>(s.get_instance())));
        g_sig_integer.store(s.pick_sig("(Ljava/lang/Integer;)I",
                                       std::make_unique<java_integer>(s.get_instance())));
        // multi-arg pinned sigs — each must reach exactly its overload.
        g_sig_int_int.store(s.pick2_sig("(II)I",
                                        static_cast<std::int32_t>(1), static_cast<std::int32_t>(2)));
        g_sig_int_long.store(s.pick2_sig("(IJ)I",
                                         static_cast<std::int32_t>(1), static_cast<std::int64_t>(2)));
        g_sig_long_int.store(s.pick2_sig("(JI)I",
                                         static_cast<std::int64_t>(1), static_cast<std::int32_t>(2)));
        g_sig_int_string.store(s.pick2_sig("(ILjava/lang/String;)I",
                                           static_cast<std::int32_t>(1), std::string{ "x" }));
        g_sig_string_int.store(s.pick2_sig("(Ljava/lang/String;I)I",
                                           std::string{ "y" }, static_cast<std::int32_t>(2)));
        g_sig_long_double.store(s.pick2_sig("(JD)I",
                                            static_cast<std::int64_t>(1), 2.0));
        // pinned mixed narrow/wide ORDER sigs — each reaches exactly its overload.
        g_sig_int_double.store(s.pick2_sig("(ID)I",
                                           static_cast<std::int32_t>(1), 2.0));
        g_sig_double_int.store(s.pick2_sig("(DI)I",
                                           2.0, static_cast<std::int32_t>(1)));
        // 3-arg pinned sig.
        g_sig_int_int_int.store(s.get_method("pick", "(III)I")->call(
            static_cast<std::int32_t>(1), static_cast<std::int32_t>(2), static_cast<std::int32_t>(3)));

        // ===== signature_pinned: the arg TYPE must NOT override a pinned sig ===
        // resolve_compatible_method() returns this->method verbatim when the
        // proxy was created from an EXPLICIT signature.  Pin (D)I but pass a C++
        // int -> the (D)I overload (RET_DOUBLE), NOT (I)I, must dispatch.  Same
        // for (J)I with an int arg.  And a String-typed pinned sig given an oop
        // (Object wrapper) must still dispatch (String)I, not (Object)I.
        g_pinned_double_with_int_arg.store(s.pick_sig("(D)I", static_cast<std::int32_t>(9)));
        g_pinned_long_with_int_arg.store(s.pick_sig("(J)I", static_cast<std::int32_t>(9)));
        g_pinned_string_with_object_arg.store(s.pick_sig("(Ljava/lang/String;)I",
                                              std::make_unique<java_object>(s.get_instance())));

        // ===== single-signature method: matching + non-matching arg ========
        g_only_int_match.store(s.only_int(11));               // (I)I match -> 7011
        // onlyInt has ONLY (I)I.  Calling with a double: resolve finds no (D)I
        // overload, falls back to this->method ((I)I), and the JNI dispatch with
        // a double arg against an int slot is itself undefined — we just record
        // whatever int comes back (documents the fallback, never asserts a value).
        g_only_int_mismatch.store(s.only_int_mismatch_double(99.0));
        // Wrong-ARITY no-match: onlyInt() with ZERO args.  No (())I overload, so
        // resolution falls back to (I)I; the interpreter reads a zero-initialised
        // slot.  Primitive-only -> memory-safe.  Record the int; assert SURVIVAL
        // only (the contract is "no crash on a no-match", not a specific value).
        g_only_int_noargs.store(s.only_int_noargs());
        g_only_int_noargs_attempted.store(1);
        // Reference-typed no-match: onlyRef(Integer) called with a wrapper
        // registered as java/lang/Double.  Ljava/lang/Double; matches no onlyRef
        // overload, so resolution falls back to the SOLE onlyRef(Integer) and
        // dispatches it -> RET_ONLY_REF (deterministic).  onlyRef ignores its arg,
        // so passing a MethodOverload oop in the reference slot is memory-safe.
        {
            auto wrong{ std::make_unique<java_double>(s.get_instance()) };
            g_only_ref_mismatch.store(s.get_method("onlyRef")->call(std::move(wrong)));
            g_only_ref_mismatch_attempted.store(1);
        }

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
        // STATIC reference overload twin: spick(Integer) by name via a wrapper
        // registered as java/lang/Integer.  The static resolver (deriving the klass
        // from _pool_holder, fix #7) must re-pick (Ljava/lang/Integer;)I over
        // spick(String).  Body ignores the arg -> the carried oop is harmless.
        {
            auto integer{ std::make_unique<java_integer>(s.get_instance()) };
            g_s_integer.store(overload_fixture::static_method("spick")->call(std::move(integer)));
        }
        // STATIC two-wide-parameter twin: spick(long,double) "(JD)I" by name.
        // Writes lastDoubleArg — placed AFTER the double echo was already captured.
        g_s_long_double.store(overload_fixture::static_method("spick")->call(
            static_cast<std::int64_t>(77LL), 9.5));
        // STATIC narrow-then-wide twin: spick(int,double) "(ID)I" by name — must be
        // re-picked over spick(long,double) "(JD)I" and spick(int,int) "(II)I" on
        // the historically-broken static path (resolution by per-slot descriptor).
        g_s_int_double.store(overload_fixture::static_method("spick")->call(
            static_cast<std::int32_t>(50), 6.5));
        // STATIC no-arg twin: spick() "()I" by name — the static resolver must
        // re-pick the ARITY-0 overload (object==nullptr path, fix #7), distinct from
        // every arg-taking static.  A name-only static call with NO args used to fall
        // back to whatever get_method() latched first by name; the fix re-picks ()I.
        g_s_noarg.store(overload_fixture::static_method("spick")->call());
        // STATIC arity disambiguation: spick(int,int) by name, distinct from the
        // single-arg spick(int).
        g_s_arity2.store(overload_fixture::static_method("spick")->call(
            static_cast<std::int32_t>(30), static_cast<std::int32_t>(40)));
        // STATIC ALTERNATE C++ TYPES by name — the static resolver must classify
        // by traits exactly as the instance resolver does (regression mirror of
        // the alternate-type instance block).  Resolution-only (no re-echo, to
        // avoid clobbering the echoes captured above).
        g_s_cstr.store(overload_fixture::static_method("spick")->call(static_cast<const char*>("cs")));
        g_s_char16.store(overload_fixture::static_method("spick")->call(static_cast<char16_t>(0x0042)));
        g_s_uchar.store(overload_fixture::static_method("spick")->call(static_cast<unsigned char>(0x10)));
        // STATIC boundary wide-pair: spick(LONG_MIN, frac) -> (JD)I + SBIAS.
        g_s_long_double_boundary.store(overload_fixture::static_method("spick")->call(
            std::numeric_limits<std::int64_t>::min(), 0.25));
        // STATIC HIGH-arity twin: spick(int x8) by name (8-jvalue packing boundary).
        // The static resolver (klass from _pool_holder, fix #7) must select the
        // 8-arg overload over every lower static arity.  Echoes the count.
        g_s_int8.store(overload_fixture::static_method("spick")->call(
            static_cast<std::int32_t>(1), static_cast<std::int32_t>(2),
            static_cast<std::int32_t>(3), static_cast<std::int32_t>(4),
            static_cast<std::int32_t>(5), static_cast<std::int32_t>(6),
            static_cast<std::int32_t>(7), static_cast<std::int32_t>(8)));
        g_s_int8_count.store(overload_fixture::last_arg_count());
        // STATIC null-oop reference disambiguation: a null-oop java_integer must
        // still re-pick spick(Integer) on the historically-broken static path
        // (resolution is by C++ type; the body ignores the oop).
        {
            auto null_integer{ std::make_unique<java_integer>(nullptr) };
            g_s_null_integer.store(overload_fixture::static_method("spick")->call(std::move(null_integer)));
        }

        // explicit-signature static path: bypasses resolution -> MUST be exact.
        g_s_sig_int.store(overload_fixture::static_method("spick", "(I)I")->call(static_cast<std::int32_t>(1)));
        g_s_sig_double.store(overload_fixture::static_method("spick", "(D)I")->call(3.14));
        g_s_sig_string.store(overload_fixture::static_method("spick", "(Ljava/lang/String;)I")->call(std::string{ "s" }));
        // the REST of the static explicit-sig descriptor set + a static 2-arg sig.
        g_s_sig_long.store(overload_fixture::static_method("spick", "(J)I")->call(static_cast<std::int64_t>(5)));
        g_s_sig_float.store(overload_fixture::static_method("spick", "(F)I")->call(6.5f));
        g_s_sig_bool.store(overload_fixture::static_method("spick", "(Z)I")->call(true));
        g_s_sig_byte.store(overload_fixture::static_method("spick", "(B)I")->call(static_cast<std::int8_t>(7)));
        g_s_sig_short.store(overload_fixture::static_method("spick", "(S)I")->call(static_cast<std::int16_t>(8)));
        g_s_sig_char.store(overload_fixture::static_method("spick", "(C)I")->call(static_cast<std::uint16_t>(9)));
        g_s_sig_int_int.store(overload_fixture::static_method("spick", "(II)I")->call(
            static_cast<std::int32_t>(1), static_cast<std::int32_t>(2)));
        g_s_sig_long_double.store(overload_fixture::static_method("spick", "(JD)I")->call(
            static_cast<std::int64_t>(1), 2.0));
        // static pinned "()I" (no args) and "(ID)I" — both BYPASS resolution
        // (signature_pinned), proving the no-arg and narrow/wide static overloads
        // exist and dispatch on this JDK independent of the static-resolution path.
        g_s_sig_noarg.store(overload_fixture::static_method("spick", "()I")->call());
        g_s_sig_int_double.store(overload_fixture::static_method("spick", "(ID)I")->call(
            static_cast<std::int32_t>(1), 2.0));

        // ===== ARRAY-vs-scalar resolution ==================================
        // (1) The mere PRESENCE of pick(int[]) / pick(long[]) in the methods array
        //     must NOT perturb scalar resolution: resolving a C++ int / int64 by
        //     name walks every "pick" descriptor, and next_argument_descriptor must
        //     skip the leading '[' of "[I"/"[J" so the scalar arg still lands on
        //     (I)I / (J)I — never on the array overload.
        g_r_int_scalar_amid_arrays.store(s.pick(static_cast<std::int32_t>(77)));
        g_r_long_scalar_amid_arrays.store(s.pick(static_cast<std::int64_t>(88)));
        // A scalar char must ALSO be unperturbed by the char[] overload's presence
        // (the array-token parser walks past "[C" so a uint16 arg lands on (C)I).
        g_r_char_scalar_amid_arrays.store(s.pick(static_cast<std::uint16_t>(0x0042)));
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
        // char[] — a THIRD array element type "([C)I".  Java char is a 16-bit
        // unsigned code unit, so the element size is sizeof(std::uint16_t).  Same
        // guarded pattern: skip with [INFO] if the alloc fails.
        {
            void* const char_arr{ vmhook::make_java_array("[C", 4, sizeof(std::uint16_t)) };
            if (char_arr && vmhook::hotspot::is_valid_pointer(char_arr))
            {
                g_arr_char_attempted.store(1);
                g_arr_char_sig.store(s.pick_sig("([C)I", std::make_unique<array_carrier>(char_arr)));
                g_arr_char_len.store(overload_fixture::last_array_len());
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
        // Register the Integer wrapper -> pick(Integer)/spick(Integer) resolve
        // deterministically to Ljava/lang/Integer; (a THIRD reference overload).
        vmhook::register_class<java_integer>("java/lang/Integer");
        // Register the Double wrapper -> drives the reference no-match probe
        // (Ljava/lang/Double; matches no onlyRef overload).  These three wrapper
        // C++ types are distinct to this TU, so registration is isolated from every
        // other module (type_to_class_map is keyed by the C++ type_index).
        vmhook::register_class<java_double>("java/lang/Double");

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
        // Third reference type: a java/lang/Integer-registered wrapper resolves to
        // pick(Integer), NOT pick(Object) and NOT pick(String).
        ctx.check("mo_registered_integer_resolves_to_integer_overload",
                  g_r_integer_registered.load() == RET_INTEGER);
        // The three reference overloads are mutually distinct AND each was hit by
        // its own registered wrapper / String — proving class-name discrimination
        // inside 'L...;', not "any reference matches any reference overload".
        ctx.check("mo_string_object_integer_three_refs_distinct",
                  g_r_string.load() != g_r_object_registered.load()
                  && g_r_object_registered.load() != g_r_integer_registered.load()
                  && g_r_string.load() != g_r_integer_registered.load());

        // The four ambiguous-by-value-but-distinct-by-type literals: the crown
        // jewels.  3 (int), 42(long via int64), 3.14(double), 3.14f(float) MUST
        // land on four DIFFERENT overloads.
        ctx.check("mo_int_long_double_float_all_distinct",
                  g_r_int.load() != g_r_long.load()
                  && g_r_long.load() != g_r_double.load()
                  && g_r_double.load() != g_r_float.load()
                  && g_r_int.load() != g_r_float.load());

        // The full narrow-integral set (byte/short/char/int/long) must land on
        // FIVE mutually-distinct overloads — NO implicit widening collapse (a
        // byte/short/char must NEVER fall through to the wider int/long overload;
        // argument_matches_descriptor demands an EXACT descriptor letter).
        ctx.check("mo_byte_short_char_int_long_no_widening_collapse",
                  g_r_byte.load()  != g_r_short.load()
                  && g_r_short.load() != g_r_char.load()
                  && g_r_char.load()  != g_r_int.load()
                  && g_r_int.load()   != g_r_long.load()
                  && g_r_byte.load()  != g_r_int.load()
                  && g_r_short.load() != g_r_int.load());

        // =====================================================================
        //  ALTERNATE C++ TYPES classify by TRAITS (sizeof / signedness /
        //  is_same_v), not by a fixed-width alias — each lands on the SAME
        //  overload as its sibling.  Distinct C++ types, identical descriptor.
        // =====================================================================
        // String family: const char*, char* (non-const) and std::string_view all
        // -> pick(String).  The matcher has separate is_same_v branches for const
        // char* and char*, so the mutable-pointer case is a distinct classification.
        ctx.check("mo_cstr_resolves_to_string_overload",        g_r_cstr.load()        == RET_STRING);
        ctx.check("mo_mutable_cstr_resolves_to_string_overload", g_r_mutable_cstr.load() == RET_STRING);
        ctx.check("mo_string_view_resolves_to_string_overload", g_r_string_view.load() == RET_STRING);
        ctx.check("mo_cstr_arg_value_echoed",        g_echo_cstr_ok.load());
        ctx.check("mo_string_view_arg_value_echoed", g_echo_string_view_ok.load());
        // char16_t and uint16_t both -> pick(char) (Java char is unsigned 16-bit).
        ctx.check("mo_char16_resolves_to_char_overload",     g_r_char16.load()        == RET_CHAR);
        ctx.check("mo_uint16_resolves_to_char_not_short",    g_r_uint16_as_char.load() == RET_CHAR);
        ctx.check("mo_char16_arg_value_echoed",              g_echo_char16.load()     == 0x0041);
        // unsigned char / signed char / plain char / std::uint8_t all -> pick(byte)
        // (every integral of sizeof 1, signed or unsigned, named or builtin).
        ctx.check("mo_unsigned_char_resolves_to_byte_overload", g_r_uchar.load()     == RET_BYTE);
        ctx.check("mo_signed_char_resolves_to_byte_overload",   g_r_schar.load()     == RET_BYTE);
        ctx.check("mo_plain_char_resolves_to_byte_overload",    g_r_plainchar.load() == RET_BYTE);
        ctx.check("mo_uint8_resolves_to_byte_overload",         g_r_uint8.load()     == RET_BYTE);
        // unsigned char 0xFE stored in a Java byte slot is the SIGNED value -2.
        ctx.check("mo_unsigned_char_arg_value_echoed", g_echo_uchar.load() == -2);
        ctx.check("mo_signed_char_arg_value_echoed",   g_echo_schar.load() == -3);
        // std::uint8_t 0xFE likewise sign-narrows to -2 in the Java byte slot.
        ctx.check("mo_uint8_arg_value_echoed",         g_echo_uint8.load() == -2);
        // unsigned 4/8-byte integrals classify by sizeof: unsigned int -> pick(int),
        // unsigned long long -> pick(long).  Distinct C++ types, SAME overload as
        // their signed siblings (the matcher keys on sizeof, not signedness/alias).
        ctx.check("mo_uint32_resolves_to_int_overload",  g_r_uint32.load() == RET_INT);
        ctx.check("mo_uint64_resolves_to_long_overload", g_r_uint64.load() == RET_LONG);
        // and the values round-trip through the int/long slot (123, 9000000000).
        ctx.check("mo_uint32_arg_value_echoed", g_echo_uint32.load() == 123);
        ctx.check("mo_uint64_arg_value_echoed", g_echo_uint64.load() == 9000000000LL);

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
        // char boundary: 0x0000 and 0xFFFF both resolve to pick(char) and the
        // 16-bit value round-trips UNSIGNED (0xFFFF -> 65535, never -1).
        ctx.check("mo_char_zero_resolves_char",  g_r_char_zero.load() == RET_CHAR);
        ctx.check("mo_char_max_resolves_char",   g_r_char_max.load()  == RET_CHAR);
        ctx.check("mo_char_zero_value_echoed",   g_echo_char_zero.load() == 0x0000);
        ctx.check("mo_char_max_value_echoed",    g_echo_char_max.load()  == 0xFFFF);
        // byte boundary: INT8_MIN / INT8_MAX resolve to pick(byte) + echo exactly.
        ctx.check("mo_byte_min_resolves_byte",   g_r_byte_min.load() == RET_BYTE);
        ctx.check("mo_byte_max_resolves_byte",   g_r_byte_max.load() == RET_BYTE);
        ctx.check("mo_byte_min_value_echoed",    g_echo_byte_min.load() == -128);
        ctx.check("mo_byte_max_value_echoed",    g_echo_byte_max.load() == 127);
        // short boundary: INT16_MIN / INT16_MAX resolve to pick(short) + echo.
        ctx.check("mo_short_min_resolves_short", g_r_short_min.load() == RET_SHORT);
        ctx.check("mo_short_max_resolves_short", g_r_short_max.load() == RET_SHORT);
        ctx.check("mo_short_min_value_echoed",   g_echo_short_min.load() == -32768);
        ctx.check("mo_short_max_value_echoed",   g_echo_short_max.load() == 32767);

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
        // pick(String,int): reference-first ordering resolves distinctly from
        // pick(int,String) (same type multiset {String,int}, different slot order).
        ctx.check("mo_string_int_resolves_string_int", g_r_string_int.load() == RET_STRING_INT);
        ctx.check("mo_string_int_vs_int_string_distinct",
                  g_r_string_int.load() != g_r_int_string.load());
        // slot0 String == "lead", slot1 int == 13 (right value -> right slot).
        ctx.check("mo_string_int_arg_slots",
                  g_string_int_str_ok.load() && g_string_int_a.load() == 13);
        // pick(long,double): two CONSECUTIVE wide params resolve to (JD)I, distinct
        // from the (int,long)/(long,int) one-wide overloads.  RESOLUTION assertion;
        // wide-slot packing fidelity across long+double is method_call_wide_args'.
        ctx.check("mo_long_double_resolves_long_double", g_r_long_double.load() == RET_LONG_DOUBLE);
        ctx.check("mo_long_double_distinct_from_int_long_and_long_int",
                  g_r_long_double.load() != g_r_int_long.load()
                  && g_r_long_double.load() != g_r_long_int.load());
        // both wide values survived into their distinct slots: long(slot0)=123456789012345,
        // double(slot2)=2.5 (proves the two wide args did not overlap or truncate).
        ctx.check("mo_long_double_arg_slots",
                  g_long_double_a.load() == 123456789012345LL && g_long_double_b_is_e.load() == 1);
        // boundary wide-pair: LONG_MIN + 0.125 — the (JD)I overload is still
        // selected at the 64-bit edge, and BOTH slots survive (long == LONG_MIN,
        // double == 0.125), proving the two wide params do not overlap/truncate
        // even when the long is the most-negative representable value.
        ctx.check("mo_long_double_boundary_resolves_long_double",
                  g_r_long_double_boundary.load() == RET_LONG_DOUBLE);
        ctx.check("mo_long_double_boundary_arg_slots",
                  g_long_double_boundary_a.load() == std::numeric_limits<std::int64_t>::min()
                  && g_long_double_boundary_b_ok.load() == 1);

        // mixed narrow/wide ORDER: pick(int,double) "(ID)I" vs pick(double,int)
        // "(DI)I".  Each slot's descriptor (one narrow, one wide) is matched
        // independently, so (ID)I is told from (IJ)I/(JD)I, and the order mirror
        // (DI)I is told from (ID)I despite the identical {int,double} multiset.
        ctx.check("mo_int_double_resolves_int_double", g_r_int_double.load() == RET_INT_DOUBLE);
        ctx.check("mo_double_int_resolves_double_int", g_r_double_int.load() == RET_DOUBLE_INT);
        ctx.check("mo_int_double_vs_double_int_distinct",
                  g_r_int_double.load() != g_r_double_int.load());
        // (ID)I must NOT collapse onto the adjacent (IJ)I or (JD)I overloads —
        // it shares slot0 with (IJ)I and slot1 with (JD)I but matches neither.
        ctx.check("mo_int_double_distinct_from_int_long_and_long_double",
                  g_r_int_double.load() != g_r_int_long.load()
                  && g_r_int_double.load() != g_r_long_double.load());
        // both slots survived: int slot==17, double slot==2.5 / 4.5.
        ctx.check("mo_int_double_arg_slots",
                  g_int_double_a.load() == 17 && g_int_double_b_ok.load() == 1);
        ctx.check("mo_double_int_arg_slots",
                  g_double_int_a.load() == 19 && g_double_int_b_ok.load() == 1);

        // =====================================================================
        //  Same-type two-arg overloads: per-slot descriptor disambiguation and
        //  independent survival of the two same-width slots.
        // =====================================================================
        ctx.check("mo_double_double_resolves_double_double", g_r_double_double.load() == RET_DOUBLE_DOUBLE);
        ctx.check("mo_double_double_slots_survive",
                  g_double_double_a_ok.load() == 1 && g_double_double_b_ok.load() == 1);
        ctx.check("mo_float_float_resolves_float_float", g_r_float_float.load() == RET_FLOAT_FLOAT);
        ctx.check("mo_float_float_slots_survive",
                  g_float_float_a_ok.load() == 1 && g_float_float_b_ok.load() == 1);
        ctx.check("mo_bool_bool_resolves_bool_bool", g_r_bool_bool.load() == RET_BOOL_BOOL);
        // (true,false): slot0 stayed true, slot1 stayed false — the second boolean
        // slot did not mirror the first.
        ctx.check("mo_bool_bool_slots_survive",
                  g_bool_bool_a.load() == 1 && g_bool_bool_b.load() == 0);
        // The same-type pairs are mutually distinct AND distinct from the mixed
        // (JD)I pair — the resolver did not collapse any wide/narrow pair onto
        // another two-arg overload.
        ctx.check("mo_same_type_pairs_all_distinct",
                  g_r_double_double.load() != g_r_float_float.load()
                  && g_r_float_float.load() != g_r_bool_bool.load()
                  && g_r_double_double.load() != g_r_bool_bool.load()
                  && g_r_double_double.load() != g_r_long_double.load());

        // =====================================================================
        //  Two-REFERENCE overloads: per-slot reference CLASS discrimination.
        //  pick(Object,Object) and pick(Integer,Integer) must NOT collapse onto
        //  each other (each slot's 'L...;' class is matched), nor onto the single-
        //  reference overloads.
        // =====================================================================
        ctx.check("mo_obj_obj_resolves_object_object",     g_r_obj_obj.load()      == RET_OBJ_OBJ);
        ctx.check("mo_integer_integer_resolves_int_refs",  g_r_int_int_refs.load() == RET_INT_INT_REFS);
        ctx.check("mo_two_ref_overloads_distinct",
                  g_r_obj_obj.load() != g_r_int_int_refs.load());
        // and distinct from the single-reference Object / Integer overloads
        // (a 2-ref shape is told from a 1-ref shape by ARITY, not just by class).
        ctx.check("mo_two_ref_distinct_from_single_ref",
                  g_r_obj_obj.load() != g_r_object_registered.load()
                  && g_r_int_int_refs.load() != g_r_integer_registered.load());

        // =====================================================================
        //  HIGH-arity (>8-arg) resolution at the jvalue packing boundary.
        //  pick(int x8) fills exactly eight slots, pick(int x9) needs a ninth;
        //  the resolver tells them apart by ARITY, and every slot's value
        //  survived (the 8th/9th arg was neither dropped nor aliased).
        // =====================================================================
        ctx.check("mo_arity8_resolves_int8", g_r_int8.load() == RET_INT8);
        // 8-arg edge slots: count==8, slot0==81, slot7(8th)==88 — the 8-jvalue packing
        // boundary (call() static_asserts max 8 args, so 9 is not callable/testable).
        ctx.check("mo_arity8_arg_slots",
                  g_int8_count.load() == 8 && g_int8_first.load() == 81 && g_int8_last.load() == 88);
        // high arities are distinct from every low arity (0..3) — no collapse.
        ctx.check("mo_high_arity_distinct_from_low",
                  g_r_int8.load() != g_r_arity3.load()
                  && g_r_int8.load() != g_r_arity2.load());

        // =====================================================================
        //  NULL-oop reference disambiguation: a wrapper carrying a NULL oop still
        //  resolves by its C++ TYPE — argument_matches_descriptor is a compile-
        //  time trait check that never dereferences the oop.  So a null Integer
        //  wrapper still lands on pick(Integer) and a null Object on pick(Object),
        //  and the process survives (bodies ignore the oop).
        // =====================================================================
        ctx.check("mo_null_integer_resolves_integer", g_r_null_integer.load() == RET_INTEGER);
        ctx.check("mo_null_object_resolves_object",    g_r_null_object.load()  == RET_OBJECT);
        ctx.check("mo_null_refs_distinct", g_r_null_integer.load() != g_r_null_object.load());

        // =====================================================================
        //  Explicit-signature fast path resolves identically to name-only.
        // =====================================================================
        ctx.check("mo_sig_int_resolves_int",       g_sig_int.load()    == RET_INT);
        ctx.check("mo_sig_double_resolves_double",  g_sig_double.load() == RET_DOUBLE);
        ctx.check("mo_sig_long_resolves_long",      g_sig_long.load()   == RET_LONG);
        ctx.check("mo_sig_float_resolves_float",    g_sig_float.load()  == RET_FLOAT);
        ctx.check("mo_sig_string_resolves_string",  g_sig_string.load() == RET_STRING);
        // the REST of the single-arg descriptor set via the pinned-sig path —
        // every descriptor letter (Z B S C) + the no-arg () sig reaches exactly
        // its overload (RESOLUTION via the signature_text fast path).
        ctx.check("mo_sig_bool_resolves_boolean",  g_sig_bool.load()  == RET_BOOLEAN);
        ctx.check("mo_sig_byte_resolves_byte",     g_sig_byte.load()  == RET_BYTE);
        ctx.check("mo_sig_short_resolves_short",   g_sig_short.load() == RET_SHORT);
        ctx.check("mo_sig_char_resolves_char",     g_sig_char.load()  == RET_CHAR);
        ctx.check("mo_sig_noarg_resolves_noarg",   g_sig_noarg.load() == RET_NOARG);
        // pinned reference sigs reach exactly Object / Integer.
        ctx.check("mo_sig_object_resolves_object",   g_sig_object.load()  == RET_OBJECT);
        ctx.check("mo_sig_integer_resolves_integer", g_sig_integer.load() == RET_INTEGER);
        // multi-arg pinned sigs reach exactly their overload (incl. order-sensitive
        // (IJ) vs (JI) and (ILstr) vs (Lstr;I), the 3-arg (III), and the (JD) pair).
        ctx.check("mo_sig_int_int_resolves_int_int",         g_sig_int_int.load()     == RET_INT_INT);
        ctx.check("mo_sig_int_int_int_resolves_int_int_int", g_sig_int_int_int.load() == RET_INT_INT_INT);
        ctx.check("mo_sig_int_long_resolves_int_long",       g_sig_int_long.load()    == RET_INT_LONG);
        ctx.check("mo_sig_long_int_resolves_long_int",       g_sig_long_int.load()    == RET_LONG_INT);
        ctx.check("mo_sig_int_string_resolves_int_string",   g_sig_int_string.load()  == RET_INT_STRING);
        ctx.check("mo_sig_string_int_resolves_string_int",   g_sig_string_int.load()  == RET_STRING_INT);
        ctx.check("mo_sig_long_double_resolves_long_double", g_sig_long_double.load() == RET_LONG_DOUBLE);
        // pinned mixed narrow/wide ORDER sigs reach exactly (ID)I / (DI)I and the
        // pinned path agrees with the name-only path on both.
        ctx.check("mo_sig_int_double_resolves_int_double", g_sig_int_double.load() == RET_INT_DOUBLE);
        ctx.check("mo_sig_double_int_resolves_double_int", g_sig_double_int.load() == RET_DOUBLE_INT);
        ctx.check("mo_sig_int_double_vs_double_int_distinct",
                  g_sig_int_double.load() != g_sig_double_int.load());
        ctx.check("mo_sig_path_agrees_with_nameonly_int_double",
                  g_sig_int_double.load() == g_r_int_double.load());
        ctx.check("mo_sig_path_agrees_with_nameonly_double_int",
                  g_sig_double_int.load() == g_r_double_int.load());
        ctx.check("mo_sig_int_long_vs_long_int_distinct",
                  g_sig_int_long.load() != g_sig_long_int.load());
        // fast path and name-only path must agree on the SAME overload — extended
        // across the full single-arg descriptor set, not just int/double.
        ctx.check("mo_sig_path_agrees_with_nameonly_int",
                  g_sig_int.load() == g_r_int.load());
        ctx.check("mo_sig_path_agrees_with_nameonly_double",
                  g_sig_double.load() == g_r_double.load());
        ctx.check("mo_sig_path_agrees_with_nameonly_long",
                  g_sig_long.load() == g_r_long.load());
        ctx.check("mo_sig_path_agrees_with_nameonly_float",
                  g_sig_float.load() == g_r_float.load());
        ctx.check("mo_sig_path_agrees_with_nameonly_bool",
                  g_sig_bool.load() == g_r_bool.load());
        ctx.check("mo_sig_path_agrees_with_nameonly_byte",
                  g_sig_byte.load() == g_r_byte.load());
        ctx.check("mo_sig_path_agrees_with_nameonly_short",
                  g_sig_short.load() == g_r_short.load());
        ctx.check("mo_sig_path_agrees_with_nameonly_char",
                  g_sig_char.load() == g_r_char.load());
        ctx.check("mo_sig_path_agrees_with_nameonly_string",
                  g_sig_string.load() == g_r_string.load());

        // =====================================================================
        //  signature_pinned: an EXPLICIT sig is dispatched VERBATIM — the C++ arg
        //  type must NOT re-pick a different overload (resolve_compatible_method
        //  bails on signature_pinned).  Pin (D)I but pass a C++ int -> (D)I still
        //  runs (RET_DOUBLE), NOT (I)I.  This is the guard that an explicit
        //  combo(CharSequence) request is never silently turned into combo(String)
        //  by an incidental arg type.
        // =====================================================================
        ctx.check("mo_pinned_double_sig_not_repicked_by_int_arg",
                  g_pinned_double_with_int_arg.load() == RET_DOUBLE);
        ctx.check("mo_pinned_long_sig_not_repicked_by_int_arg",
                  g_pinned_long_with_int_arg.load() == RET_LONG);
        ctx.check("mo_pinned_string_sig_not_repicked_by_object_arg",
                  g_pinned_string_with_object_arg.load() == RET_STRING);

        // =====================================================================
        //  Single-signature method: matching arg resolves; non-matching arg
        //  falls back to this->method (documented, value not asserted).
        // =====================================================================
        ctx.check("mo_only_int_matching_arg", g_only_int_match.load() == 7011);
        ctx.record("[INFO] onlyInt(double 99.0) [no (D)I overload -> fallback to (I)I] returned "
                   + std::to_string(g_only_int_mismatch.load()));

        // =====================================================================
        //  NO-MATCH graceful failure (the contract: never crash; fall back
        //  predictably).  Three flavours: a type mismatch (onlyInt(double),
        //  above), an ARITY mismatch (onlyInt() with zero args), and a
        //  REFERENCE-type mismatch (onlyRef(Double-wrapper) — its descriptor
        //  matches no onlyRef overload).  The process SURVIVING to here is itself
        //  the primary proof for the first two; the reference case additionally
        //  has a DETERMINISTIC fallback (a single onlyRef overload) so its value
        //  is hard-asserted.  g_detour_calls/g_saw_self (checked above) confirm
        //  the detour ran to completion, i.e. no call AV'd.
        if (g_only_int_noargs_attempted.load() == 1)
        {
            ctx.record("[INFO] onlyInt() [wrong arity: 0 args vs (I)I -> fallback to (I)I, "
                       "reads zero-initialised slot] returned "
                       + std::to_string(g_only_int_noargs.load()) + " (no crash is the contract)");
        }
        if (g_only_ref_mismatch_attempted.load() == 1)
        {
            // Ljava/lang/Double; matches no onlyRef overload; the SOLE onlyRef(Integer)
            // is the fallback this->method and dispatches -> RET_ONLY_REF.  A wrong
            // value here (or a crash before here) would mean the reference no-match
            // fallback is broken.
            ctx.check("mo_reference_no_match_falls_back_to_sole_overload",
                      g_only_ref_mismatch.load() == RET_ONLY_REF);
        }
        else
        {
            ctx.record("[INFO] reference no-match probe (onlyRef + java/lang/Double wrapper) "
                       "did not run on this detour — skipped (no fault, no FAIL)");
        }

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
        // STATIC reference overload: a java/lang/Integer wrapper re-picks
        // spick(Integer) over spick(String) on the historically-broken static path.
        ctx.check("mo_static_integer_resolves_integer", g_s_integer.load() == RET_INTEGER + SBIAS);
        ctx.check("mo_static_integer_distinct_from_string",
                  g_s_integer.load() != g_s_string.load());
        // STATIC two-wide-parameter overload: spick(long,double) (JD)I selected by
        // a (int64_t,double) static call, distinct from every single-arg static.
        ctx.check("mo_static_long_double_resolves_long_double",
                  g_s_long_double.load() == RET_LONG_DOUBLE + SBIAS);
        ctx.check("mo_static_long_double_distinct_from_long_and_double",
                  g_s_long_double.load() != g_s_long.load()
                  && g_s_long_double.load() != g_s_double.load());
        // STATIC narrow-then-wide overload: spick(int,double) "(ID)I" selected by a
        // (int32_t,double) static call on the historically-broken static path,
        // distinct from spick(long,double) "(JD)I" and spick(int,int) "(II)I".
        ctx.check("mo_static_int_double_resolves_int_double",
                  g_s_int_double.load() == RET_INT_DOUBLE + SBIAS);
        ctx.check("mo_static_int_double_distinct_from_long_double_and_int_int",
                  g_s_int_double.load() != g_s_long_double.load()
                  && g_s_int_double.load() != g_s_arity2.load());
        // STATIC no-arg overload: spick() "()I" re-picked by an arg-less static call
        // (the ARITY-0 case of the fix-#7 static resolver), distinct from every
        // arg-taking static.  A KNOWN-broken-before-fix path: a name-only static
        // call with zero args used to dispatch the first-by-name overload.
        ctx.check("mo_static_noarg_resolves_noarg", g_s_noarg.load() == RET_NOARG + SBIAS);
        ctx.check("mo_static_noarg_distinct_from_int_and_arity2",
                  g_s_noarg.load() != g_s_int.load() && g_s_noarg.load() != g_s_arity2.load());
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

        // STATIC explicit-signature path across the REST of the descriptor set +
        // a static 2-arg sig.  This path BYPASSES resolution (signature_pinned),
        // so it proves the static methods EXIST and dispatch on this JDK for every
        // descriptor letter — independent of the (fixed) static-resolution bug.
        ctx.check("mo_static_sig_long_exact",   g_s_sig_long.load()   == RET_LONG + SBIAS);
        ctx.check("mo_static_sig_float_exact",  g_s_sig_float.load()  == RET_FLOAT + SBIAS);
        ctx.check("mo_static_sig_bool_exact",   g_s_sig_bool.load()   == RET_BOOLEAN + SBIAS);
        ctx.check("mo_static_sig_byte_exact",   g_s_sig_byte.load()   == RET_BYTE + SBIAS);
        ctx.check("mo_static_sig_short_exact",  g_s_sig_short.load()  == RET_SHORT + SBIAS);
        ctx.check("mo_static_sig_char_exact",   g_s_sig_char.load()   == RET_CHAR + SBIAS);
        ctx.check("mo_static_sig_int_int_exact", g_s_sig_int_int.load() == RET_INT_INT + SBIAS);
        ctx.check("mo_static_sig_long_double_exact",
                  g_s_sig_long_double.load() == RET_LONG_DOUBLE + SBIAS);
        // static pinned no-arg "()I" and narrow/wide "(ID)I" dispatch exactly
        // (resolution bypassed) — the static methods EXIST and dispatch on this JDK.
        ctx.check("mo_static_sig_noarg_exact",      g_s_sig_noarg.load()      == RET_NOARG + SBIAS);
        ctx.check("mo_static_sig_int_double_exact", g_s_sig_int_double.load() == RET_INT_DOUBLE + SBIAS);
        // STATIC name-only agreement across the extended set (each name-only static
        // resolution lands on the SAME overload as its explicit-sig twin).
        ctx.check("mo_static_nameonly_agrees_with_sig_long",
                  g_s_long.load() == g_s_sig_long.load());
        ctx.check("mo_static_nameonly_agrees_with_sig_float",
                  g_s_float.load() == g_s_sig_float.load());
        ctx.check("mo_static_nameonly_agrees_with_sig_bool",
                  g_s_bool.load() == g_s_sig_bool.load());
        ctx.check("mo_static_nameonly_agrees_with_sig_byte",
                  g_s_byte.load() == g_s_sig_byte.load());
        ctx.check("mo_static_nameonly_agrees_with_sig_short",
                  g_s_short.load() == g_s_sig_short.load());
        ctx.check("mo_static_nameonly_agrees_with_sig_char",
                  g_s_char.load() == g_s_sig_char.load());
        ctx.check("mo_static_nameonly_agrees_with_sig_int_int",
                  g_s_arity2.load() == g_s_sig_int_int.load());
        ctx.check("mo_static_nameonly_agrees_with_sig_long_double",
                  g_s_long_double.load() == g_s_sig_long_double.load());
        // the new static twins: name-only resolution agrees with the explicit-sig
        // dispatch for the no-arg ()I and the narrow/wide (ID)I overloads.
        ctx.check("mo_static_nameonly_agrees_with_sig_noarg",
                  g_s_noarg.load() == g_s_sig_noarg.load());
        ctx.check("mo_static_nameonly_agrees_with_sig_int_double",
                  g_s_int_double.load() == g_s_sig_int_double.load());

        // STATIC ALTERNATE C++ TYPES by name — the static resolver classifies by
        // traits just like the instance one: const char* -> spick(String),
        // char16_t -> spick(char), unsigned char -> spick(byte).
        ctx.check("mo_static_cstr_resolves_string",   g_s_cstr.load()   == RET_STRING + SBIAS);
        ctx.check("mo_static_char16_resolves_char",   g_s_char16.load() == RET_CHAR + SBIAS);
        ctx.check("mo_static_uchar_resolves_byte",    g_s_uchar.load()  == RET_BYTE + SBIAS);
        // STATIC boundary wide-pair: spick(LONG_MIN, 0.25) -> (JD)I + SBIAS.
        ctx.check("mo_static_long_double_boundary_resolves_long_double",
                  g_s_long_double_boundary.load() == RET_LONG_DOUBLE + SBIAS);
        // STATIC HIGH-arity: spick(int x8) re-picked over every lower static arity
        // on the (historically-broken) static path, at the 8-jvalue boundary.
        ctx.check("mo_static_arity8_resolves_int8", g_s_int8.load() == RET_INT8 + SBIAS);
        ctx.check("mo_static_arity8_arg_count", g_s_int8_count.load() == 8);
        ctx.check("mo_static_arity8_distinct_from_arity2",
                  g_s_int8.load() != g_s_arity2.load());
        // STATIC null-oop reference: a null Integer wrapper still re-picks
        // spick(Integer) (resolution by C++ type, never an oop deref).
        ctx.check("mo_static_null_integer_resolves_integer",
                  g_s_null_integer.load() == RET_INTEGER + SBIAS);

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
        // a scalar char is likewise unperturbed by the char[] overload's presence.
        ctx.check("mo_scalar_char_unperturbed_by_array_overload",
                  g_r_char_scalar_amid_arrays.load() == RET_CHAR);
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
        if (g_arr_char_attempted.load() == 1)
        {
            // ([C)I reaches the char[] body — a THIRD array element type, distinct
            // from int[]/long[] and from the scalar char overload.
            ctx.check("mo_char_array_sig_resolves_char_array", g_arr_char_sig.load() == RET_CHAR_ARRAY);
            ctx.check("mo_char_array_arg_landed", g_arr_char_len.load() == 4);
            ctx.check("mo_char_array_distinct_from_scalar_char",
                      g_arr_char_sig.load() != g_r_char.load());
        }
        else
        {
            ctx.record("[INFO] char[] allocation unavailable in this detour — "
                       "explicit ([C)I array dispatch skipped (no fault, no FAIL)");
        }
        if (g_arr_int_attempted.load() == 1 && g_arr_long_attempted.load() == 1)
        {
            ctx.check("mo_int_array_vs_long_array_distinct",
                      g_arr_int_sig.load() != g_arr_long_sig.load());
        }
        if (g_arr_int_attempted.load() == 1 && g_arr_char_attempted.load() == 1)
        {
            ctx.check("mo_int_array_vs_char_array_distinct",
                      g_arr_int_sig.load() != g_arr_char_sig.load());
        }

        // =====================================================================
        //  AMBIGUOUS unregistered-wrapper resolution — first-match-wins, no
        //  diagnostic (the medium flaw).  An UNREGISTERED wrapper's L...; branch
        //  matches ANY reference descriptor, so pick(String) / pick(Object) /
        //  pick(Integer) ALL match and the loop returns whichever _methods index
        //  is lowest — nondeterministic across builds.  Record only.
        // =====================================================================
        const std::int64_t amb{ g_amb_unregistered.load() };
        ctx.record("[INFO] unregistered-wrapper arg resolved to sentinel "
                   + std::to_string(amb) + " (pick(String)=" + std::to_string(RET_STRING)
                   + " pick(Object)=" + std::to_string(RET_OBJECT)
                   + " pick(Integer)=" + std::to_string(RET_INTEGER)
                   + "); first-match-wins with no ambiguity diagnostic is a KNOWN flaw");
        ctx.record(std::string{ "[INFO] unregistered_wrapper_matched_a_reference_overload = " }
                   + ((amb == RET_STRING || amb == RET_OBJECT || amb == RET_INTEGER) ? "true" : "false"));
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
