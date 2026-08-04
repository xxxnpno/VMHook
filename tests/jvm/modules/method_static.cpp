// method_static — exhaustive JVM tests for the STATIC-method call surface:
//   static_method("name")->call(args)   on vmhook::object<T>.
//
// Feature lives in vmhook/ext/vmhook/vmhook.hpp.  Authoritative anchors (read &
// verified against the current file; do NOT trust older line citations):
//   * object<T>::static_method(name)             -> object_base::get_method(type_index,name)
//   * object<T>::static_method(name, signature)  -> object_base::get_method(type_index,name,sig)
//   * object_base::get_method(type_index,name)        (static path, super-chain walk,
//        first STATIC name match, builds method_proxy{nullptr,...})
//   * object_base::get_method(type_index,name,sig)    (static path, name+sig, STATIC only)
//   * object_base::static_method_only()  (the JVM_ACC_STATIC 0x0008 gate; fails CLOSED)
//   * method_proxy::is_static()          (reads JVM_ACC_STATIC from the live Method)
//   * method_proxy::get_compressed_oop() (receiver OOP; 0 when object==nullptr)
//   * method_proxy::call()      (interpreter call-stub fast path + return decode)
//   * method_proxy::call_jni()  (JNI fallback; is_static_call = object==nullptr || is_static();
//        static JNI dispatch slots 116/119/122/125/128/131/134/137/140/143)
//   * value_t::is_void() / is_string() / as_string()
//   * vmhook::array_length / get_array_element (array-return element walks)
//
// WHAT THIS MODULE PROVES (the method_static contract).  Two families of check:
//   ms_*    — the original surface (primitive/String/object returns, no-receiver,
//             is_static(), the ACC_STATIC instance-rejection gate, name+sig pin).
//   mstat_* — the EXHAUSTIVE expansion added on top:
//      A. every NON-int primitive ARGUMENT (Z B S C J F D) + String + Object echoes
//         back exactly (the C++ arg-packer maps each descriptor slot, no shift),
//      B. ARRAY returns ([I / [Ljava/lang/String; / [Ljava/lang/Object; / null) and
//         an array ARGUMENT length echo,
//      C. OVERLOADED static methods resolved by C++ arg TYPE and by explicit
//         static_method(name, signature) — distinct sentinel per overload,
//      D. a static method that MUTATES a static field (observable side effect),
//      E. an instance wrapper's get_method() of a STATIC method dispatches
//         statically (is_static()==true, no receiver) — the "vice-versa" of the
//         instance-rejection gate,
//      F. STATIC method on an INTERFACE (Java 8+), on an ABSTRACT class, on an
//         ENUM, and an INHERITED static (declared in a super, named through the
//         sub — statics are not polymorphic),
//      G. first touch of a class via a static call triggers its <clinit>
//         (characterized).
//
// Object/array USABILITY is call-path dependent (call_stub fast path vs JDK-21
// call_jni): every such assertion gates on `call_stub_present` and is recorded as
// INFO on the JNI path, exactly mirroring method_call_object / enum_singleton.
// Path-INDEPENDENT invariants (right value/overload, type routing, is_static,
// null->null uptr, no crash) are HARD-asserted on every path.
//
// Everything runs inside ONE detour on trigger(int) — the only context where
// current_java_thread is set so method_proxy::call() may dispatch.  The module
// body ends with an unconditional shutdown_hooks() OUTSIDE the try-equivalent so
// ZERO hooks stay armed for later modules (suite-safety).
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>

namespace
{
    // Wrapper for vmhook.fixtures.MethodStatic.  All static helpers go through
    // static_method(...) (NOT get_field/get_method) for GCC portability; every
    // call() is made from inside the trigger() detour.
    class method_static : public vmhook::object<method_static>
    {
    public:
        explicit method_static(vmhook::oop_t instance) noexcept
            : vmhook::object<method_static>{ instance }
        {
        }

        // -- handshake / observable static fields --
        static auto set_go(bool v) -> void                 { static_field("go")->set(v); }
        static auto get_done() -> bool                     { return static_field("done")->get(); }
        static auto get_static_recorder_hits() -> std::int32_t { return static_field("staticRecorderHits")->get(); }
        static auto get_recorded_int_arg() -> std::int32_t     { return static_field("recordedIntArg")->get(); }
        static auto get_recorded_long_arg() -> std::int64_t    { return static_field("recordedLongArg")->get(); }
        static auto get_recorded_first() -> std::int32_t       { return static_field("recordedFirstOfThree")->get(); }
        static auto get_recorded_second() -> std::int64_t      { return static_field("recordedSecondOfThree")->get(); }
        static auto get_recorded_third() -> std::int32_t       { return static_field("recordedThirdOfThree")->get(); }

        // full-width single-arg echo recorders (read back to prove the arg landed)
        static auto get_recorded_bool() -> bool                { return static_field("recordedBoolArg")->get(); }
        static auto get_recorded_byte() -> std::int8_t         { return static_field("recordedByteArg")->get(); }
        static auto get_recorded_short() -> std::int16_t       { return static_field("recordedShortArg")->get(); }
        static auto get_recorded_char() -> std::uint16_t       { return static_field("recordedCharArg")->get(); }
        static auto get_recorded_echo_long() -> std::int64_t   { return static_field("recordedEchoLong")->get(); }
        static auto get_recorded_float() -> float              { return static_field("recordedFloatArg")->get(); }
        static auto get_recorded_double() -> double            { return static_field("recordedDoubleArg")->get(); }
        static auto get_recorded_string() -> std::string       { return static_field("recordedStringArg")->get(); }
        static auto get_mutable_state() -> std::int32_t        { return static_field("mutableState")->get(); }

        // Reads this instance's own seed (proves a returned wrapper is usable).
        auto seed() const -> std::int32_t { return get_field("seed")->get(); }
    };

    // Bare carrier for an arbitrary Java oop / array oop.  call()'s arg-packer
    // sends any object_base-derived arg as a raw oop (get_instance()), so wrapping
    // an oop here lets us pass an Object / array argument into a static method.
    class oop_carrier : public vmhook::object<oop_carrier>
    {
    public:
        explicit oop_carrier(vmhook::oop_t instance) noexcept
            : vmhook::object<oop_carrier>{ instance }
        {
        }
    };

    // Wrapper for the inherited-static subclass  MethodStatic$StaticSub.
    class static_sub : public vmhook::object<static_sub>
    {
    public:
        explicit static_sub(vmhook::oop_t instance) noexcept
            : vmhook::object<static_sub>{ instance }
        {
        }
    };

    // Wrapper for the abstract class with a static  MethodStatic$AbstractWithStatic.
    class abstract_with_static : public vmhook::object<abstract_with_static>
    {
    public:
        explicit abstract_with_static(vmhook::oop_t instance) noexcept
            : vmhook::object<abstract_with_static>{ instance }
        {
        }
    };

    // Wrapper for the interface with a static  MethodStatic$StaticIface (Java 8+).
    class static_iface : public vmhook::object<static_iface>
    {
    public:
        explicit static_iface(vmhook::oop_t instance) noexcept
            : vmhook::object<static_iface>{ instance }
        {
        }
    };

    // Wrapper for the enum with a static  MethodStatic$StaticEnum.
    class static_enum : public vmhook::object<static_enum>
    {
    public:
        explicit static_enum(vmhook::oop_t instance) noexcept
            : vmhook::object<static_enum>{ instance }
        {
        }
    };

    // Wrapper for the <clinit> probe  MethodStatic$ClinitProbe.
    class clinit_probe : public vmhook::object<clinit_probe>
    {
    public:
        explicit clinit_probe(vmhook::oop_t instance) noexcept
            : vmhook::object<clinit_probe>{ instance }
        {
        }
        // Reads the <clinit>-set sentinel via a RAW static-field read, which does
        // NOT run <clinit> (so it can observe the pre-init state).
        //
        // TRI-STATE, and deliberately not the usual `static_field(n)->get()`
        // one-liner: MethodStatic$ClinitProbe is a NESTED class that nothing has
        // referenced yet, so before the first static call on it the class is not
        // loaded and static_field() legitimately yields std::nullopt.  Writing
        // `->get()` there dereferences a DISENGAGED optional -- reading a
        // field_proxy out of uninitialized storage, whose std::string member then
        // constructs from a null pointer and takes the JVM down inside the detour.
        // (-1 = the field did not resolve, 0/1 = the read value.)
        static auto get_initialized() -> std::int32_t
        {
            const std::optional<vmhook::field_proxy> field{ static_field("initialized") };
            if (!field.has_value()) { return -1; }
            return field->get() ? 1 : 0;
        }
    };

    // ---- raw-bit capture so NaN / Inf / -0.0 survive the atomic round-trip --
    inline auto f2bits(float f) noexcept -> std::uint32_t
    {
        std::uint32_t b{ 0 };
        std::memcpy(&b, &f, sizeof(b));
        return b;
    }
    inline auto bits2f(std::uint32_t b) noexcept -> float
    {
        float f{ 0.0f };
        std::memcpy(&f, &b, sizeof(f));
        return f;
    }
    inline auto d2bits(double d) noexcept -> std::uint64_t
    {
        std::uint64_t b{ 0 };
        std::memcpy(&b, &d, sizeof(b));
        return b;
    }
    inline auto bits2d(std::uint64_t b) noexcept -> double
    {
        double d{ 0.0 };
        std::memcpy(&d, &b, sizeof(d));
        return d;
    }

    // Sentinel no Java boundary value collides with.
    constexpr std::int64_t k_uncaptured = static_cast<std::int64_t>(0xDEADBEEFCAFEF00Dull);
    constexpr std::int32_t k_static_child_seed = 9090;

    // -- overloaded-static sentinels (mirror MethodStatic.POLY_*) --
    constexpr std::int32_t k_poly_int    = 7001;
    constexpr std::int32_t k_poly_long   = 7002;
    constexpr std::int32_t k_poly_double = 7003;
    constexpr std::int32_t k_poly_string = 7004;
    constexpr std::int32_t k_poly_int2   = 7005;

    // -- inherited/abstract/interface/enum/clinit sentinels (mirror the fixture) --
    constexpr std::int32_t k_base_static     = 0x5151;  // 20817
    constexpr std::int32_t k_sub_static      = 0x6262;  // 25186
    constexpr std::int32_t k_abstract_static = 0x7373;  // 29555
    constexpr std::int32_t k_iface_static    = 0x8484;  // 33924
    constexpr std::int32_t k_enum_static     = 0x9595;  // 38293
    constexpr std::int32_t k_clinit_value    = 0xC11C;  // 49436

    // -- array-return sentinels (mirror the fixture) --
    constexpr std::int32_t k_arr_int_len = 4;
    constexpr std::int32_t k_arr_int_0   = 10;
    constexpr std::int32_t k_arr_int_3   = 40;
    constexpr std::int32_t k_arr_str_len = 3;
    constexpr std::int32_t k_arr_obj_len = 2;

    // ------------------------------------------------------------------
    //  Captured observations.  The detour writes; the module body reads.
    // ------------------------------------------------------------------
    std::atomic<int>  g_detour_calls{ 0 };
    std::atomic<bool> g_detour_saw_self{ false };
    std::atomic<bool> g_all_calls_ran{ false };

    // -- primitive static returns --
    std::atomic<int>  g_bool_true{ -1 };
    std::atomic<int>  g_bool_false{ -1 };
    std::atomic<std::int64_t> g_byte_max{ k_uncaptured };
    std::atomic<std::int64_t> g_byte_min{ k_uncaptured };
    std::atomic<std::int64_t> g_byte_negone{ k_uncaptured };
    std::atomic<std::int64_t> g_byte_negone_as_int{ k_uncaptured }; // sign-extension
    std::atomic<std::int64_t> g_short_max{ k_uncaptured };
    std::atomic<std::int64_t> g_short_min{ k_uncaptured };
    std::atomic<std::int64_t> g_short_negone{ k_uncaptured };
    std::atomic<std::int64_t> g_char_a{ k_uncaptured };
    std::atomic<std::int64_t> g_char_max{ k_uncaptured };
    std::atomic<std::int64_t> g_char_max_as_int{ k_uncaptured };   // zero-extension
    std::atomic<std::int64_t> g_int_max{ k_uncaptured };
    std::atomic<std::int64_t> g_int_min{ k_uncaptured };
    std::atomic<std::int64_t> g_int_42{ k_uncaptured };
    std::atomic<std::int64_t> g_int_negone{ k_uncaptured };
    std::atomic<std::int64_t> g_long_max{ k_uncaptured };
    std::atomic<std::int64_t> g_long_min{ k_uncaptured };
    std::atomic<std::int64_t> g_long_big{ k_uncaptured };

    std::atomic<bool>          g_fp_captured{ false };
    std::atomic<std::uint32_t> g_float_half{ 0 };
    std::atomic<std::uint32_t> g_float_negzero{ 0 };
    std::atomic<std::uint32_t> g_float_nan{ 0 };
    std::atomic<std::uint32_t> g_float_posinf{ 0 };
    std::atomic<std::uint32_t> g_float_max{ 0 };
    std::atomic<std::uint64_t> g_double_pi{ 0 };
    std::atomic<std::uint64_t> g_double_negzero{ 0 };
    std::atomic<std::uint64_t> g_double_nan{ 0 };
    std::atomic<std::uint64_t> g_double_neginf{ 0 };
    std::atomic<std::uint64_t> g_double_max{ 0 };

    // -- void static return --
    std::atomic<int> g_void_is_void{ -1 };

    // -- String static returns --
    std::atomic<bool> g_str_captured{ false };
    std::string       g_str_hello;            // guarded by g_str_captured
    std::string       g_str_unicode;
    std::string       g_str_empty;
    std::string       g_str_null;
    std::atomic<int>  g_str_hello_is_string{ -1 };
    std::atomic<int>  g_str_null_is_void{ -1 }; // null String return: is_void()?

    // -- object static returns --
    std::atomic<bool> g_obj_null_is_null_uptr{ false };   // sNullChild -> null uptr (hard)
    std::atomic<int>  g_obj_null_is_void{ -1 };           // value_t.is_void() on null obj
    std::atomic<bool> g_obj_child_nonnull{ false };       // sMakeChild non-null (path-dep)
    std::atomic<std::int64_t> g_obj_child_seed{ k_uncaptured }; // seed read through wrapper (int64 to hold the k_uncaptured sentinel)
    std::atomic<int>  g_obj_child_is_string{ -1 };        // non-String object: is_string()?
    std::atomic<int>  g_obj_child_is_void{ -1 };          // non-null object: is_void()?

    // -- "no receiver" proofs --
    std::atomic<std::int64_t> g_echo_ret{ k_uncaptured };       // sEchoInt(v) -> v
    std::atomic<std::int64_t> g_record_long_ret{ k_uncaptured };
    std::atomic<std::int64_t> g_record_three_ret{ k_uncaptured };
    std::atomic<int>  g_recv_oop_zero_echo{ -1 };   // proxy.get_compressed_oop()==0
    std::atomic<int>  g_recv_oop_zero_int{ -1 };
    std::atomic<int>  g_recv_oop_zero_void{ -1 };

    // -- is_static() accessor (THE headline) --
    // static methods -> must be true; instance methods -> must be false.
    std::atomic<int>  g_isstatic_sint{ -1 };
    std::atomic<int>  g_isstatic_slong{ -1 };
    std::atomic<int>  g_isstatic_sbool{ -1 };
    std::atomic<int>  g_isstatic_sstring{ -1 };
    std::atomic<int>  g_isstatic_sobject{ -1 };
    std::atomic<int>  g_isstatic_svoid{ -1 };
    std::atomic<int>  g_isstatic_secho{ -1 };
    std::atomic<int>  g_isstatic_iget{ -1 };   // instance via get_method -> false
    std::atomic<int>  g_isstatic_ilabel{ -1 };
    std::atomic<int>  g_isstatic_iecho{ -1 };
    std::atomic<int>  g_isstatic_itouch{ -1 };
    std::atomic<int>  g_isstatic_trigger{ -1 }; // the hooked instance method -> false

    // -- static_method() must REJECT instance methods (JVM_ACC_STATIC gate). --
    std::atomic<int>  g_flaw_iget_has_value{ -1 };       // static_method("iGetSeed").has_value() -> 0
    std::atomic<int>  g_flaw_iget_is_static{ -1 };       // ...is_static() on it -> 0 (never static)
    std::atomic<int>  g_flaw_iecho_has_value{ -1 };      // static_method("iEcho").has_value()    -> 0
    std::atomic<int>  g_flaw_iecho_is_static{ -1 };
    std::atomic<int>  g_static_real_has_value{ -1 };     // static_method("sIntFortyTwo").has_value() -> 1

    // -- signature-overload accessor: static_method(name, sig) --
    std::atomic<std::int64_t> g_sig_echo_ret{ k_uncaptured };  // static_method("sEchoInt","(I)I")
    std::atomic<int>  g_sig_isstatic{ -1 };

    // ===================== mstat_* EXHAUSTIVE EXPANSION =====================

    // (A) every non-int primitive + String + object ARGUMENT echo round-trip.
    std::atomic<int>          g_arg_bool_ret{ -1 };        // sEchoBool(true)  -> 1
    std::atomic<int>          g_arg_bool_field{ -1 };
    std::atomic<std::int64_t> g_arg_byte_ret{ k_uncaptured };   // sEchoByte(-7)
    std::atomic<std::int64_t> g_arg_byte_field{ k_uncaptured };
    std::atomic<std::int64_t> g_arg_short_ret{ k_uncaptured };  // sEchoShort(-12345)
    std::atomic<std::int64_t> g_arg_short_field{ k_uncaptured };
    std::atomic<std::int64_t> g_arg_char_ret{ k_uncaptured };   // sEchoChar(0xBEEF)
    std::atomic<std::int64_t> g_arg_char_field{ k_uncaptured };
    std::atomic<std::int64_t> g_arg_long_ret{ k_uncaptured };   // sEchoLong(big)
    std::atomic<std::int64_t> g_arg_long_field{ k_uncaptured };
    std::atomic<std::uint32_t> g_arg_float_ret{ 0 };            // sEchoFloat(-0.0f) raw bits
    std::atomic<std::uint32_t> g_arg_float_field{ 0 };
    std::atomic<std::uint64_t> g_arg_double_ret{ 0 };          // sEchoDouble(pi) raw bits
    std::atomic<std::uint64_t> g_arg_double_field{ 0 };
    std::atomic<bool>         g_arg_string_captured{ false };
    std::string               g_arg_string_ret;               // sEchoString("round-trip")
    std::string               g_arg_string_field;
    std::atomic<int>          g_arg_object_identity_nonzero{ -1 }; // sArgIdentity(self) != 0
    std::atomic<int>          g_arg_object_field_matches{ -1 };    // field == returned id
    std::atomic<int>          g_arg_object_null_zero{ -1 };        // sArgIdentity(null) == 0

    // (B) array returns + array argument.
    std::atomic<int> g_arr_int_resolves{ -1 };
    std::atomic<int> g_arr_int_not_string{ -1 };   // path-independent: [I is not a String
    std::atomic<int> g_arr_int_not_void{ -1 };     // non-null array: is_void()==false (path-dep)
    std::atomic<int> g_arr_int_len_ok{ -1 };       // array_length == 4 (path-dep)
    std::atomic<int> g_arr_int_elems_ok{ -1 };     // elem[0]==10 && elem[3]==40 (path-dep)
    std::atomic<int> g_arr_str_resolves{ -1 };
    std::atomic<int> g_arr_str_len_ok{ -1 };       // length 3 (path-dep)
    std::atomic<int> g_arr_obj_resolves{ -1 };
    std::atomic<int> g_arr_obj_len_ok{ -1 };       // length 2 (path-dep)
    std::atomic<bool> g_arr_null_is_null_uptr{ false }; // sNullArray -> null uptr (hard)
    std::atomic<int>  g_arr_arg_attempted{ 0 };         // 1 once the fresh int[] alloc succeeded
    std::atomic<std::int64_t> g_arr_arg_len_ret{ k_uncaptured }; // sArrayLen(int[]) -> 4 (path-indep)

    // (C) overloaded static resolution.
    std::atomic<int> g_poly_by_int{ -1 };       // sPoly(int)    -> POLY_INT
    std::atomic<int> g_poly_by_long{ -1 };      // sPoly(int64)  -> POLY_LONG
    std::atomic<int> g_poly_by_double{ -1 };    // sPoly(double) -> POLY_DOUBLE
    std::atomic<int> g_poly_by_string{ -1 };    // sPoly(string) -> POLY_STRING
    std::atomic<int> g_poly_by_int2{ -1 };      // sPoly(int,int)-> POLY_INT2
    std::atomic<int> g_poly_sig_int{ -1 };      // static_method("sPoly","(I)I")    -> POLY_INT
    std::atomic<int> g_poly_sig_long{ -1 };     // static_method("sPoly","(J)I")    -> POLY_LONG
    std::atomic<int> g_poly_sig_string{ -1 };   // static_method("sPoly","(Ljava/lang/String;)I") -> POLY_STRING

    // (D) static-field mutation side effect.
    std::atomic<int> g_mutate_prior{ -1 };      // sMutateState(0x1357) returns prior (0 after reset)
    std::atomic<int> g_mutate_observed{ -1 };   // mutableState field now == 0x1357

    // (E) instance wrapper's get_method() of a STATIC method dispatches statically.
    // NOTE: a proxy built by instance->get_method("staticX") KEEPS the instance as
    // its stored receiver (this->object), so get_compressed_oop() is the instance's
    // (NON-zero) oop — the "no receiver" decision is made at CALL time via
    // is_static() (object==nullptr || is_static()), NOT by a nulled receiver oop.
    // So we characterize the oop as non-zero and rely on is_static()/return for the
    // real contract.  (Contrast static_method("staticX"), whose receiver IS null.)
    std::atomic<int> g_inst_static_resolves{ -1 };   // self->get_method("sIntFortyTwo").has_value()
    std::atomic<int> g_inst_static_is_static{ -1 };  // ...is_static() == true
    std::atomic<int> g_inst_static_oop_nonzero{ -1 }; // ...get_compressed_oop()!=0 (carries the wrapper oop)
    std::atomic<std::int64_t> g_inst_static_ret{ k_uncaptured }; // ...call() == 42

    // (F) static on interface / abstract / enum / inherited.
    std::atomic<int> g_iface_resolves{ -1 };
    std::atomic<int> g_iface_is_static{ -1 };
    std::atomic<std::int64_t> g_iface_ret{ k_uncaptured };       // ifaceStaticValue() -> 0x8484
    std::atomic<int> g_abstract_resolves{ -1 };
    std::atomic<int> g_abstract_is_static{ -1 };
    std::atomic<std::int64_t> g_abstract_ret{ k_uncaptured };    // abstractStaticValue() -> 0x7373
    std::atomic<int> g_enum_resolves{ -1 };
    std::atomic<int> g_enum_is_static{ -1 };
    std::atomic<std::int64_t> g_enum_ret{ k_uncaptured };        // enumStaticValue() -> 0x9595
    std::atomic<int> g_sub_own_resolves{ -1 };                   // subStaticValue() on the sub
    std::atomic<std::int64_t> g_sub_own_ret{ k_uncaptured };     // -> 0x6262
    std::atomic<int> g_inherited_resolves{ -1 };                 // baseStaticValue() VIA the sub wrapper
    std::atomic<int> g_inherited_is_static{ -1 };
    std::atomic<std::int64_t> g_inherited_ret{ k_uncaptured };   // -> 0x5151 (super's declaration)

    // (G) <clinit> characterization.
    std::atomic<int> g_clinit_pre_initialized{ -1 };  // initialized field BEFORE the static call
    std::atomic<int> g_clinit_resolves{ -1 };
    std::atomic<std::int64_t> g_clinit_ret{ k_uncaptured };  // clinitValue() -> 0xC11C (proves <clinit> ran)
    std::atomic<int> g_clinit_post_initialized{ -1 }; // initialized field AFTER the static call -> 1

    // ===================== mstat2_* DEEPER INPUT COVERAGE ====================
    // (H) more boundary VALUES through the single-arg echoes (RETURN only — a
    //     primitive return/echo is bit-identical on both dispatch paths, so all
    //     hard-assertable everywhere).  We re-use the existing sEcho* methods at
    //     additional boundaries the original module did not cover.
    std::atomic<int>          g_echo_bool_false{ -1 };       // sEchoBool(false) -> 0
    std::atomic<std::int64_t> g_echo_byte_zero{ k_uncaptured };
    std::atomic<std::int64_t> g_echo_byte_max{ k_uncaptured };   // 127
    std::atomic<std::int64_t> g_echo_byte_min{ k_uncaptured };   // -128
    std::atomic<std::int64_t> g_echo_short_max{ k_uncaptured };  // 32767
    std::atomic<std::int64_t> g_echo_short_min{ k_uncaptured };  // -32768
    std::atomic<std::int64_t> g_echo_char_zero{ k_uncaptured };  // 0
    std::atomic<std::int64_t> g_echo_char_max{ k_uncaptured };   // 65535
    std::atomic<std::int64_t> g_echo_char_a{ k_uncaptured };     // 'A' = 65
    std::atomic<std::int64_t> g_echo_int_max{ k_uncaptured };
    std::atomic<std::int64_t> g_echo_int_min{ k_uncaptured };
    std::atomic<std::int64_t> g_echo_int_zero{ k_uncaptured };
    std::atomic<std::int64_t> g_echo_int_negone{ k_uncaptured };
    std::atomic<std::int64_t> g_echo_long_max{ k_uncaptured };
    std::atomic<std::int64_t> g_echo_long_min{ k_uncaptured };
    std::atomic<std::int64_t> g_echo_long_zero{ k_uncaptured };
    std::atomic<std::uint32_t> g_echo_float_half{ 0 };          // sEchoFloat(0.5f)
    std::atomic<std::uint32_t> g_echo_float_nan{ 0 };           // NaN survives echo
    std::atomic<std::uint32_t> g_echo_float_posinf{ 0 };
    std::atomic<std::uint32_t> g_echo_float_max{ 0 };
    std::atomic<std::uint64_t> g_echo_double_negzero{ 0 };      // -0.0 signbit
    std::atomic<std::uint64_t> g_echo_double_nan{ 0 };
    std::atomic<std::uint64_t> g_echo_double_neginf{ 0 };
    std::atomic<std::uint64_t> g_echo_double_max{ 0 };
    std::atomic<bool>          g_echo_string_captured{ false };
    std::string                g_echo_string_empty;            // sEchoString("") -> ""
    std::string                g_echo_string_unicode;          // sEchoString("café") round-trip

    // (I) multi-argument slot-shape digests (RETURN only, path-independent).
    std::atomic<std::int64_t> g_sum_ii{ k_uncaptured };        // sSumII(a,b)
    std::atomic<std::int64_t> g_sum_jj{ k_uncaptured };        // sSumJJ(a,b)
    std::atomic<std::uint64_t> g_sum_dd{ 0 };                  // sSumDD(a,b) raw bits
    std::atomic<bool>          g_sum_dd_captured{ false };
    std::atomic<std::uint32_t> g_sum_ff{ 0 };                  // sSumFF(a,b) raw bits
    std::atomic<bool>          g_sum_ff_captured{ false };
    std::atomic<int>          g_bool_xor_tf{ -1 };             // sBoolXor(true,false) -> 1
    std::atomic<int>          g_bool_xor_tt{ -1 };             // sBoolXor(true,true)  -> 0
    std::atomic<std::uint64_t> g_mix_fid{ 0 };                 // sMixFID(f,i,d) raw bits
    std::atomic<bool>          g_mix_fid_captured{ false };
    std::atomic<std::int64_t> g_pack_prims{ k_uncaptured };    // sPackPrims(...) digest
    std::atomic<std::int64_t> g_seven_ints{ k_uncaptured };    // s7Ints(...) weighted sum

    // (J) extra overloads: float and (int,long), by C++ type and by signature.
    std::atomic<int> g_poly_by_float{ -1 };    // sPoly(float)    -> POLY_FLOAT
    std::atomic<int> g_poly_by_intlong{ -1 };  // sPoly(int,long) -> POLY_INTLONG
    std::atomic<int> g_poly_sig_float{ -1 };   // static_method("sPoly","(F)I")
    std::atomic<int> g_poly_sig_double{ -1 };  // static_method("sPoly","(D)I")
    std::atomic<int> g_poly_sig_intint{ -1 };  // static_method("sPoly","(II)I")
    std::atomic<int> g_poly_sig_intlong{ -1 }; // static_method("sPoly","(IJ)I")

    // (K) name+signature overload that is REJECTED for a wrong/instance match.
    std::atomic<int> g_sig_missing_overload{ -1 };  // ("sPoly","(B)I") absent -> has_value()==0
    std::atomic<int> g_sig_instance_rejected{ -1 }; // ("iEcho","(I)I") instance -> has_value()==0

    // overloaded-static sentinels for the extra overloads (mirror MethodStatic).
    constexpr std::int32_t k_poly_float   = 7006;
    constexpr std::int32_t k_poly_intlong = 7007;

    // ===================== mstat3_* BATCH-18 DEEPENING ======================
    // (L) 8-arg static call — EXACTLY the maximum both dispatch paths support
    //     (params[8] / jvalue[8], compile-time capped at 8; a static call has no
    //     receiver so all 8 slots carry real args).  Position-weighted sum, HARD.
    std::atomic<std::int64_t> g_eight_ints{ k_uncaptured };       // s8Ints(1..8)

    // (M) wide/narrow interleave + back-to-back wide pairs (RETURN only, HARD).
    std::atomic<std::int64_t>  g_wide_shape{ k_uncaptured };      // sWideShape(J,I,J,I)
    std::atomic<std::uint64_t> g_three_d{ 0 };                    // sThreeD(D,D,D) raw bits
    std::atomic<bool>          g_three_d_captured{ false };

    // (N) proxy REUSE — one resolved proxy, two call()s with distinct args.
    std::atomic<std::int64_t> g_reuse_first{ k_uncaptured };      // sEchoInt2 call #1
    std::atomic<std::int64_t> g_reuse_second{ k_uncaptured };     // sEchoInt2 call #2 (same proxy)

    // (O) object-arg ROUND-TRIP of a library-decoded child (path-dependent on the
    //     child being non-null, so the comparison is gated; the no-crash + null
    //     branch are path-independent and hard-asserted).
    std::atomic<int> g_roundtrip_attempted{ 0 };                  // 1 once a non-null child was obtained
    std::atomic<int> g_roundtrip_identity_matches{ -1 };          // id via sArgIdentity2 == id via sArgIdentity
    std::atomic<int> g_roundtrip_null_zero{ -1 };                 // sArgIdentity2(null) == 0 (path-indep)

    // (P) null + non-trivial String ARGUMENT (distinct from null String RETURN).
    std::atomic<std::int64_t> g_string_arg_len_null{ k_uncaptured };    // sStringArgLen(null) -> -1
    std::atomic<std::int64_t> g_string_arg_len_unicode{ k_uncaptured }; // sStringArgLen("café") -> 4 code units

    // (Q) interface DEFAULT (instance) method is REJECTED by the static gate.
    std::atomic<int> g_iface_default_rejected{ -1 };  // static_method("ifaceDefaultValue").has_value()==0
    std::atomic<int> g_iface_default_sig_rejected{ -1 }; // name+sig form also rejected

    // (R) INHERITED static via the SUB resolved by NAME+SIGNATURE (the pinned
    //     path through the super-chain walk; the module already covers name-only).
    std::atomic<int> g_inherited_sig_resolves{ -1 };
    std::atomic<int> g_inherited_sig_is_static{ -1 };
    std::atomic<std::int64_t> g_inherited_sig_ret{ k_uncaptured };  // -> 0x5151

    // ===================== mstat4_* DEEPER INPUT COVERAGE ====================
    // Every capture below is from a PRIMITIVE / int / String return or a
    // resolution / is_static() / get_compressed_oop() accessor — all
    // PATH-INDEPENDENT, so all HARD-assertable on every JDK.  All reuse EXISTING
    // fixture methods (no new fixture shape).

    // (S) sRecordInt — the one no-receiver recorder the module never exercised:
    //     stamps its int arg into recordedIntArg from parameter slot 0.
    std::atomic<std::int64_t> g_record_int_field{ k_uncaptured };

    // (T) sBoolXor full truth table completion (FF / FT in addition to TF / TT).
    std::atomic<int> g_bool_xor_ff{ -1 };   // sBoolXor(false,false) -> 0
    std::atomic<int> g_bool_xor_ft{ -1 };   // sBoolXor(false,true)  -> 1

    // (U) sArrayLen(null) -> -1 (path-INDEPENDENT int return; distinct from the
    //     fresh-alloc length echo, which the module already covers).
    std::atomic<std::int64_t> g_array_len_null{ k_uncaptured };

    // (V) sStringArgLen boundaries: empty -> 0, ASCII "abcde" -> 5.
    std::atomic<std::int64_t> g_string_arg_len_empty{ k_uncaptured };
    std::atomic<std::int64_t> g_string_arg_len_ascii{ k_uncaptured };

    // (W) sEchoString ASCII round-trips (longer payload incl. digits/punct).
    std::atomic<bool> g_echo_string_ascii_captured{ false };
    std::string       g_echo_string_ascii;   // "Echo-123/!?" round-trip

    // (X) sArgIdentity / sArgIdentity2 idempotency + agreement on the SAME oop.
    std::atomic<int> g_identity_idempotent{ -1 };  // sArgIdentity(self)==sArgIdentity(self)
    std::atomic<int> g_identity_two_methods_agree{ -1 }; // sArgIdentity(self)==sArgIdentity2(self)

    // (Y) sSumII / sSumJJ boundary slot-order (path-INDEPENDENT digests).
    std::atomic<std::int64_t> g_sum_ii_zero_negone{ k_uncaptured }; // sSumII(0,-1)
    std::atomic<std::int64_t> g_sum_ii_min_max{ k_uncaptured };     // sSumII(MIN,MAX)
    std::atomic<std::int64_t> g_sum_jj_zero_one{ k_uncaptured };    // sSumJJ(0,1) -> 1

    // (Z) sEchoInt / sEchoInt2 agreement + extra small-int boundaries.
    std::atomic<std::int64_t> g_echo_int_one{ k_uncaptured };       // sEchoInt(1)
    std::atomic<std::int64_t> g_echo_int2_zero{ k_uncaptured };     // sEchoInt2(0)
    std::atomic<int>          g_echo_int_methods_agree{ -1 };       // sEchoInt(7)==sEchoInt2(7)

    // (AA) is_static() true + null-receiver-oop for MORE static methods.
    std::atomic<int> g_isstatic_spoly{ -1 };
    std::atomic<int> g_isstatic_ssumii{ -1 };
    std::atomic<int> g_isstatic_sarraylen{ -1 };
    std::atomic<int> g_isstatic_s8ints{ -1 };
    std::atomic<int> g_isstatic_sargidentity{ -1 };
    std::atomic<int> g_recv_oop_zero_poly{ -1 };
    std::atomic<int> g_recv_oop_zero_sumii{ -1 };
    std::atomic<int> g_recv_oop_zero_arraylen{ -1 };

    // (AB) static_method(name, sig) is_static + null-receiver-oop for more sigs.
    std::atomic<int> g_sig_sumii_is_static{ -1 };
    std::atomic<int> g_sig_sumii_oop_zero{ -1 };
    std::atomic<std::int64_t> g_sig_sumii_ret{ k_uncaptured };   // ("sSumII","(II)I")(3,4)
    std::atomic<int> g_sig_echolong_is_static{ -1 };
    std::atomic<std::int64_t> g_sig_echolong_ret{ k_uncaptured };// ("sEchoLong","(J)J")(big)

    // (AC) sPoly by-TYPE vs by-SIGNATURE agreement cross-check (each overload).
    std::atomic<int> g_poly_int_type_eq_sig{ -1 };
    std::atomic<int> g_poly_long_type_eq_sig{ -1 };
    std::atomic<int> g_poly_string_type_eq_sig{ -1 };
    std::atomic<int> g_poly_float_type_eq_sig{ -1 };
    std::atomic<int> g_poly_double_type_eq_sig{ -1 };
    std::atomic<int> g_poly_intint_type_eq_sig{ -1 };
    std::atomic<int> g_poly_intlong_type_eq_sig{ -1 };

    // (AD) sEchoLong more boundaries (1, -1, alternating-bit pattern).
    std::atomic<std::int64_t> g_echo_long_one{ k_uncaptured };
    std::atomic<std::int64_t> g_echo_long_negone{ k_uncaptured };
    std::atomic<std::int64_t> g_echo_long_alt{ k_uncaptured };   // 0x5555555555555555

    // (AE) sEchoChar more code-unit boundaries (1, surrogate-range 0xD800).
    std::atomic<std::int64_t> g_echo_char_one{ k_uncaptured };
    std::atomic<std::int64_t> g_echo_char_surrogate{ k_uncaptured }; // 0xD800 echoes exactly

    // (AF) sEchoByte / sEchoShort extra interior boundaries (1, -1).
    std::atomic<std::int64_t> g_echo_byte_one{ k_uncaptured };
    std::atomic<std::int64_t> g_echo_byte_negone{ k_uncaptured };
    std::atomic<std::int64_t> g_echo_short_one{ k_uncaptured };
    std::atomic<std::int64_t> g_echo_short_negone{ k_uncaptured };

    auto run_all_calls(const std::unique_ptr<method_static>& self) -> void
    {
        // ============================== PRIMITIVES ==============================
        g_bool_true.store(method_static::static_method("sBoolTrue")->call() ? 1 : 0);
        g_bool_false.store(method_static::static_method("sBoolFalse")->call() ? 1 : 0);

        g_byte_max.store(static_cast<std::int8_t>(method_static::static_method("sByteMax")->call()));
        g_byte_min.store(static_cast<std::int8_t>(method_static::static_method("sByteMin")->call()));
        g_byte_negone.store(static_cast<std::int8_t>(method_static::static_method("sByteNegOne")->call()));
        {
            const std::int32_t as_int = method_static::static_method("sByteNegOne")->call();
            g_byte_negone_as_int.store(as_int);
        }

        g_short_max.store(static_cast<std::int16_t>(method_static::static_method("sShortMax")->call()));
        g_short_min.store(static_cast<std::int16_t>(method_static::static_method("sShortMin")->call()));
        g_short_negone.store(static_cast<std::int16_t>(method_static::static_method("sShortNegOne")->call()));

        g_char_a.store(static_cast<std::uint16_t>(method_static::static_method("sCharA")->call()));
        g_char_max.store(static_cast<std::uint16_t>(method_static::static_method("sCharMax")->call()));
        {
            const std::int32_t as_int = method_static::static_method("sCharMax")->call();
            g_char_max_as_int.store(as_int);
        }

        g_int_max.store(static_cast<std::int32_t>(method_static::static_method("sIntMax")->call()));
        g_int_min.store(static_cast<std::int32_t>(method_static::static_method("sIntMin")->call()));
        g_int_42.store(static_cast<std::int32_t>(method_static::static_method("sIntFortyTwo")->call()));
        g_int_negone.store(static_cast<std::int32_t>(method_static::static_method("sIntNegOne")->call()));

        g_long_max.store(static_cast<std::int64_t>(method_static::static_method("sLongMax")->call()));
        g_long_min.store(static_cast<std::int64_t>(method_static::static_method("sLongMin")->call()));
        g_long_big.store(static_cast<std::int64_t>(method_static::static_method("sLongBig")->call()));

        g_float_half.store(f2bits(static_cast<float>(method_static::static_method("sFloatHalf")->call())));
        g_float_negzero.store(f2bits(static_cast<float>(method_static::static_method("sFloatNegZero")->call())));
        g_float_nan.store(f2bits(static_cast<float>(method_static::static_method("sFloatNaN")->call())));
        g_float_posinf.store(f2bits(static_cast<float>(method_static::static_method("sFloatPosInf")->call())));
        g_float_max.store(f2bits(static_cast<float>(method_static::static_method("sFloatMax")->call())));

        g_double_pi.store(d2bits(static_cast<double>(method_static::static_method("sDoublePi")->call())));
        g_double_negzero.store(d2bits(static_cast<double>(method_static::static_method("sDoubleNegZero")->call())));
        g_double_nan.store(d2bits(static_cast<double>(method_static::static_method("sDoubleNaN")->call())));
        g_double_neginf.store(d2bits(static_cast<double>(method_static::static_method("sDoubleNegInf")->call())));
        g_double_max.store(d2bits(static_cast<double>(method_static::static_method("sDoubleMax")->call())));
        g_fp_captured.store(true);

        // void: is_void() must be true; the body bumps staticRecorderHits.
        g_void_is_void.store(method_static::static_method("sVoidBump")->call().is_void() ? 1 : 0);

        // ============================== STRINGS ================================
        {
            const auto v_hello = method_static::static_method("sStringHello")->call();
            g_str_hello = v_hello.as_string();
            g_str_hello_is_string.store(v_hello.is_string() ? 1 : 0);

            g_str_unicode = method_static::static_method("sStringUnicode")->call().as_string();
            g_str_empty   = method_static::static_method("sStringEmpty")->call().as_string();

            const auto v_null = method_static::static_method("sStringNull")->call();
            g_str_null = v_null.as_string();
            g_str_null_is_void.store(v_null.is_void() ? 1 : 0);
            g_str_captured.store(true);
        }

        // ============================== OBJECTS ================================
        {
            // Null object return MUST be a null unique_ptr (the most important
            // invariant; holds on every call path).
            std::unique_ptr<method_static> null_child = method_static::static_method("sNullChild")->call();
            g_obj_null_is_null_uptr.store(null_child == nullptr, std::memory_order_relaxed);
            g_obj_null_is_void.store(
                method_static::static_method("sNullChild")->call().is_void() ? 1 : 0);

            // Non-null object return: path-dependent usability.  On JDK-21
            // (call_jni) the handle is truncated/freed, so the wrapper may be
            // null — recorded, never hard-failed.
            std::unique_ptr<method_static> child = method_static::static_method("sMakeChild")->call();
            const bool child_nonnull{ child != nullptr };
            g_obj_child_nonnull.store(child_nonnull, std::memory_order_relaxed);
            if (child_nonnull)
            {
                g_obj_child_seed.store(child->seed(), std::memory_order_relaxed);
            }

            // value_t alternative routing (path-independent): a non-String
            // object return is never is_string(); when non-null it is never
            // is_void().
            const auto v_obj = method_static::static_method("sMakeChild")->call();
            g_obj_child_is_string.store(v_obj.is_string() ? 1 : 0);
            g_obj_child_is_void.store(v_obj.is_void() ? 1 : 0);
        }

        // ========================= NO RECEIVER PASSED ==========================
        // sEchoInt(v) returns v exactly: if a phantom `this` had occupied slot 0
        // the interpreter/JNI would mis-read the argument.
        g_echo_ret.store(static_cast<std::int32_t>(
            method_static::static_method("sEchoInt")->call(std::int32_t{ 13572468 })));

        // sRecordLong / sRecordThree stamp their args into static fields AND
        // return them; correct values prove slot-0 alignment with no receiver.
        g_record_long_ret.store(static_cast<std::int64_t>(
            method_static::static_method("sRecordLong")->call(std::int64_t{ 0x0011223344556677LL })));
        g_record_three_ret.store(static_cast<std::int64_t>(
            method_static::static_method("sRecordThree")->call(
                std::int32_t{ 5 }, std::int64_t{ 0x1122334455667788LL }, std::int32_t{ -13 })));

        // The proxy for a static method must report a NULL receiver OOP.
        {
            auto p_echo = method_static::static_method("sEchoInt");
            g_recv_oop_zero_echo.store((p_echo && p_echo->get_compressed_oop() == 0u) ? 1 : 0);
            auto p_int = method_static::static_method("sIntFortyTwo");
            g_recv_oop_zero_int.store((p_int && p_int->get_compressed_oop() == 0u) ? 1 : 0);
            auto p_void = method_static::static_method("sVoidBump");
            g_recv_oop_zero_void.store((p_void && p_void->get_compressed_oop() == 0u) ? 1 : 0);
        }

        // ========================= is_static() ACCESSOR ========================
        // Static methods -> true.
        g_isstatic_sint.store(method_static::static_method("sIntFortyTwo")->is_static() ? 1 : 0);
        g_isstatic_slong.store(method_static::static_method("sLongBig")->is_static() ? 1 : 0);
        g_isstatic_sbool.store(method_static::static_method("sBoolTrue")->is_static() ? 1 : 0);
        g_isstatic_sstring.store(method_static::static_method("sStringHello")->is_static() ? 1 : 0);
        g_isstatic_sobject.store(method_static::static_method("sMakeChild")->is_static() ? 1 : 0);
        g_isstatic_svoid.store(method_static::static_method("sVoidBump")->is_static() ? 1 : 0);
        g_isstatic_secho.store(method_static::static_method("sEchoInt")->is_static() ? 1 : 0);

        // Instance methods resolved through the instance receiver -> false.
        if (self)
        {
            g_isstatic_iget.store(self->get_method("iGetSeed")->is_static() ? 1 : 0);
            g_isstatic_ilabel.store(self->get_method("iLabel")->is_static() ? 1 : 0);
            g_isstatic_iecho.store(self->get_method("iEcho")->is_static() ? 1 : 0);
            g_isstatic_itouch.store(self->get_method("iTouch")->is_static() ? 1 : 0);
            g_isstatic_trigger.store(self->get_method("trigger")->is_static() ? 1 : 0);
        }

        // ============ static_method REJECTS instance methods (ACC_STATIC gate) ==
        {
            auto flaw_iget = method_static::static_method("iGetSeed");
            g_flaw_iget_has_value.store(flaw_iget.has_value() ? 1 : 0);
            g_flaw_iget_is_static.store((flaw_iget && flaw_iget->is_static()) ? 1 : 0);

            auto flaw_iecho = method_static::static_method("iEcho");
            g_flaw_iecho_has_value.store(flaw_iecho.has_value() ? 1 : 0);
            g_flaw_iecho_is_static.store((flaw_iecho && flaw_iecho->is_static()) ? 1 : 0);

            auto real_static = method_static::static_method("sIntFortyTwo");
            g_static_real_has_value.store(real_static.has_value() ? 1 : 0);
        }

        // ===================== static_method(name, signature) ==================
        {
            auto p = method_static::static_method("sEchoInt", "(I)I");
            if (p)
            {
                g_sig_isstatic.store(p->is_static() ? 1 : 0);
                g_sig_echo_ret.store(static_cast<std::int32_t>(p->call(std::int32_t{ 24681357 })));
            }
        }

        // ================================================================
        //  (A) EVERY non-int primitive + String + object ARGUMENT echo
        // ================================================================
        g_arg_bool_ret.store(method_static::static_method("sEchoBool")->call(true) ? 1 : 0);
        g_arg_bool_field.store(method_static::get_recorded_bool() ? 1 : 0);

        g_arg_byte_ret.store(static_cast<std::int8_t>(
            method_static::static_method("sEchoByte")->call(std::int8_t{ -7 })));
        g_arg_byte_field.store(method_static::get_recorded_byte());

        g_arg_short_ret.store(static_cast<std::int16_t>(
            method_static::static_method("sEchoShort")->call(std::int16_t{ -12345 })));
        g_arg_short_field.store(method_static::get_recorded_short());

        g_arg_char_ret.store(static_cast<std::uint16_t>(
            method_static::static_method("sEchoChar")->call(std::uint16_t{ 0xBEEF })));
        g_arg_char_field.store(method_static::get_recorded_char());

        g_arg_long_ret.store(static_cast<std::int64_t>(
            method_static::static_method("sEchoLong")->call(std::int64_t{ 0x7766554433221100LL })));
        g_arg_long_field.store(method_static::get_recorded_echo_long());

        g_arg_float_ret.store(f2bits(static_cast<float>(
            method_static::static_method("sEchoFloat")->call(-0.0f))));
        g_arg_float_field.store(f2bits(method_static::get_recorded_float()));

        g_arg_double_ret.store(d2bits(static_cast<double>(
            method_static::static_method("sEchoDouble")->call(3.141592653589793))));
        g_arg_double_field.store(d2bits(method_static::get_recorded_double()));

        {
            const auto v = method_static::static_method("sEchoString")->call(std::string{ "round-trip" });
            g_arg_string_ret = v.as_string();
            g_arg_string_field = method_static::get_recorded_string();
            g_arg_string_captured.store(true);
        }

        // Object argument: pass `self` as an oop_carrier and confirm the static
        // method saw a non-null object at slot 0 (its identity hash is non-zero
        // and matches what it stamped).  Then pass null -> identity 0.
        if (self)
        {
            auto carrier{ std::make_unique<oop_carrier>(self->get_instance()) };
            const std::int32_t id = method_static::static_method("sArgIdentity")->call(std::move(carrier));
            g_arg_object_identity_nonzero.store(id != 0 ? 1 : 0);
            const std::int32_t recorded_id{ method_static::static_field("recordedObjectIdentity")->get() };
            g_arg_object_field_matches.store(recorded_id == id ? 1 : 0);
        }
        {
            std::unique_ptr<oop_carrier> none{};   // null oop -> Java null arg
            const std::int32_t id0 = method_static::static_method("sArgIdentity")->call(std::move(none));
            g_arg_object_null_zero.store(id0 == 0 ? 1 : 0);
        }

        // ================================================================
        //  (B) ARRAY returns + array argument
        // ================================================================
        {
            auto m_int_arr = method_static::static_method("sIntArray");
            g_arr_int_resolves.store(m_int_arr.has_value() ? 1 : 0);
            if (m_int_arr)
            {
                const auto v = m_int_arr->call();
                g_arr_int_not_string.store(v.is_string() ? 0 : 1);
                g_arr_int_not_void.store(v.is_void() ? 0 : 1);
                void* const arr = v;
                if (arr != nullptr && vmhook::hotspot::is_valid_pointer(arr))
                {
                    g_arr_int_len_ok.store(vmhook::array_length(arr) == k_arr_int_len ? 1 : 0);
                    const std::int32_t e0 = vmhook::get_array_element<std::int32_t>(arr, 0);
                    const std::int32_t e3 = vmhook::get_array_element<std::int32_t>(arr, 3);
                    g_arr_int_elems_ok.store((e0 == k_arr_int_0 && e3 == k_arr_int_3) ? 1 : 0);
                }
            }

            // array ARGUMENT echo (path-INDEPENDENT): allocate a FRESH int[4]
            // through the library's make_java_array (the canonical native-array
            // path, mirrors method_overload), wrap the oop, and pass it to
            // sArrayLen(int[]).  An object/array ARGUMENT is sent as a raw oop and
            // consumed correctly on BOTH the call_stub and call_jni paths (only
            // object RETURNS are call-path dependent), so this is hard-asserted —
            // guarded only on allocation success (never faults, never false-fails).
            {
                void* const fresh_int_arr{
                    vmhook::make_java_array("[I", k_arr_int_len, sizeof(std::int32_t)) };
                if (fresh_int_arr && vmhook::hotspot::is_valid_pointer(fresh_int_arr))
                {
                    g_arr_arg_attempted.store(1);
                    auto arr_carrier{ std::make_unique<oop_carrier>(fresh_int_arr) };
                    g_arr_arg_len_ret.store(static_cast<std::int32_t>(
                        method_static::static_method("sArrayLen")->call(std::move(arr_carrier))));
                }
            }

            auto m_str_arr = method_static::static_method("sStringArray");
            g_arr_str_resolves.store(m_str_arr.has_value() ? 1 : 0);
            if (m_str_arr)
            {
                void* const arr = m_str_arr->call();
                if (arr != nullptr && vmhook::hotspot::is_valid_pointer(arr))
                {
                    g_arr_str_len_ok.store(vmhook::array_length(arr) == k_arr_str_len ? 1 : 0);
                }
            }

            auto m_obj_arr = method_static::static_method("sObjectArray");
            g_arr_obj_resolves.store(m_obj_arr.has_value() ? 1 : 0);
            if (m_obj_arr)
            {
                void* const arr = m_obj_arr->call();
                if (arr != nullptr && vmhook::hotspot::is_valid_pointer(arr))
                {
                    g_arr_obj_len_ok.store(vmhook::array_length(arr) == k_arr_obj_len ? 1 : 0);
                }
            }

            // Null array return MUST be a null unique_ptr (path-independent).
            std::unique_ptr<oop_carrier> null_arr = method_static::static_method("sNullArray")->call();
            g_arr_null_is_null_uptr.store(null_arr == nullptr, std::memory_order_relaxed);
        }

        // ================================================================
        //  (C) OVERLOADED static resolution — by arg TYPE and by signature
        // ================================================================
        g_poly_by_int.store(static_cast<std::int32_t>(
            method_static::static_method("sPoly")->call(std::int32_t{ 1 })));
        g_poly_by_long.store(static_cast<std::int32_t>(
            method_static::static_method("sPoly")->call(std::int64_t{ 2 })));
        g_poly_by_double.store(static_cast<std::int32_t>(
            method_static::static_method("sPoly")->call(3.5)));
        g_poly_by_string.store(static_cast<std::int32_t>(
            method_static::static_method("sPoly")->call(std::string{ "x" })));
        g_poly_by_int2.store(static_cast<std::int32_t>(
            method_static::static_method("sPoly")->call(std::int32_t{ 4 }, std::int32_t{ 5 })));

        g_poly_sig_int.store(static_cast<std::int32_t>(
            method_static::static_method("sPoly", "(I)I")->call(std::int32_t{ 9 })));
        g_poly_sig_long.store(static_cast<std::int32_t>(
            method_static::static_method("sPoly", "(J)I")->call(std::int64_t{ 9 })));
        g_poly_sig_string.store(static_cast<std::int32_t>(
            method_static::static_method("sPoly", "(Ljava/lang/String;)I")->call(std::string{ "y" })));

        // ================================================================
        //  (D) static-field MUTATION side effect
        // ================================================================
        // mutableState was reset to 0 by run(); sMutateState returns the prior
        // value (0) and sets it to 0x1357 — then we read the field back.
        g_mutate_prior.store(static_cast<std::int32_t>(
            method_static::static_method("sMutateState")->call(std::int32_t{ 0x1357 })));
        g_mutate_observed.store(method_static::get_mutable_state() == 0x1357 ? 1 : 0);

        // ================================================================
        //  (E) instance wrapper's get_method() of a STATIC method dispatches
        //      statically (the "vice-versa" of the instance-rejection gate)
        // ================================================================
        if (self)
        {
            auto p = self->get_method("sIntFortyTwo");   // STATIC method via INSTANCE wrapper
            g_inst_static_resolves.store(p.has_value() ? 1 : 0);
            if (p)
            {
                g_inst_static_is_static.store(p->is_static() ? 1 : 0);
                // The proxy carries the instance oop (receiver), so this is NON-zero;
                // the static dispatch still passes NO receiver because is_static()
                // is true (decided at call time).  See the global's note.
                g_inst_static_oop_nonzero.store(p->get_compressed_oop() != 0u ? 1 : 0);
                g_inst_static_ret.store(static_cast<std::int32_t>(p->call()));
            }
        }

        // ================================================================
        //  (F) STATIC on interface / abstract / enum / inherited
        // ================================================================
        {
            auto p = static_iface::static_method("ifaceStaticValue");
            g_iface_resolves.store(p.has_value() ? 1 : 0);
            if (p)
            {
                g_iface_is_static.store(p->is_static() ? 1 : 0);
                g_iface_ret.store(static_cast<std::int32_t>(p->call()));
            }
        }
        {
            auto p = abstract_with_static::static_method("abstractStaticValue");
            g_abstract_resolves.store(p.has_value() ? 1 : 0);
            if (p)
            {
                g_abstract_is_static.store(p->is_static() ? 1 : 0);
                g_abstract_ret.store(static_cast<std::int32_t>(p->call()));
            }
        }
        {
            auto p = static_enum::static_method("enumStaticValue");
            g_enum_resolves.store(p.has_value() ? 1 : 0);
            if (p)
            {
                g_enum_is_static.store(p->is_static() ? 1 : 0);
                g_enum_ret.store(static_cast<std::int32_t>(p->call()));
            }
        }
        {
            // The sub declares subStaticValue() itself...
            auto p_own = static_sub::static_method("subStaticValue");
            g_sub_own_resolves.store(p_own.has_value() ? 1 : 0);
            if (p_own)
            {
                g_sub_own_ret.store(static_cast<std::int32_t>(p_own->call()));
            }
            // ...and INHERITS baseStaticValue() from StaticBase (statics are not
            // polymorphic: naming it through the sub resolves the super's single
            // declaration, found by the static get_method super-chain walk).
            auto p_inh = static_sub::static_method("baseStaticValue");
            g_inherited_resolves.store(p_inh.has_value() ? 1 : 0);
            if (p_inh)
            {
                g_inherited_is_static.store(p_inh->is_static() ? 1 : 0);
                g_inherited_ret.store(static_cast<std::int32_t>(p_inh->call()));
            }
        }

        // ================================================================
        //  (G) <clinit> characterization — first static touch initializes
        // ================================================================
        {
            // BEFORE any static call on ClinitProbe: a raw static-field read does
            // NOT trigger <clinit>, so on most JDKs this observes the pre-init
            // (false) state.  Recorded, not hard-asserted (load-vs-init timing is
            // JDK-sensitive and a raw field read can race a concurrent init).
            g_clinit_pre_initialized.store(clinit_probe::get_initialized());

            auto p = clinit_probe::static_method("clinitValue");
            g_clinit_resolves.store(p.has_value() ? 1 : 0);
            if (p)
            {
                // Calling the static method TRIGGERS <clinit> and returns the
                // post-init sentinel — the observable "first touch initialized
                // the class" contract.
                g_clinit_ret.store(static_cast<std::int32_t>(p->call()));
            }
            g_clinit_post_initialized.store(clinit_probe::get_initialized());
        }

        // ================================================================
        //  (H) MORE boundary VALUES through the single-arg echoes (RETURN
        //      only — bit-identical on both dispatch paths).
        // ================================================================
        g_echo_bool_false.store(method_static::static_method("sEchoBool")->call(false) ? 1 : 0);

        g_echo_byte_zero.store(static_cast<std::int8_t>(
            method_static::static_method("sEchoByte")->call(std::int8_t{ 0 })));
        g_echo_byte_max.store(static_cast<std::int8_t>(
            method_static::static_method("sEchoByte")->call(std::int8_t{ 127 })));
        g_echo_byte_min.store(static_cast<std::int8_t>(
            method_static::static_method("sEchoByte")->call(std::int8_t{ -128 })));

        g_echo_short_max.store(static_cast<std::int16_t>(
            method_static::static_method("sEchoShort")->call(std::int16_t{ 32767 })));
        g_echo_short_min.store(static_cast<std::int16_t>(
            method_static::static_method("sEchoShort")->call(std::int16_t{ -32768 })));

        g_echo_char_zero.store(static_cast<std::uint16_t>(
            method_static::static_method("sEchoChar")->call(std::uint16_t{ 0 })));
        g_echo_char_max.store(static_cast<std::uint16_t>(
            method_static::static_method("sEchoChar")->call(std::uint16_t{ 65535 })));
        g_echo_char_a.store(static_cast<std::uint16_t>(
            method_static::static_method("sEchoChar")->call(std::uint16_t{ 65 })));

        g_echo_int_max.store(static_cast<std::int32_t>(
            method_static::static_method("sEchoInt")->call(std::numeric_limits<std::int32_t>::max())));
        g_echo_int_min.store(static_cast<std::int32_t>(
            method_static::static_method("sEchoInt")->call(std::numeric_limits<std::int32_t>::min())));
        g_echo_int_zero.store(static_cast<std::int32_t>(
            method_static::static_method("sEchoInt")->call(std::int32_t{ 0 })));
        g_echo_int_negone.store(static_cast<std::int32_t>(
            method_static::static_method("sEchoInt")->call(std::int32_t{ -1 })));

        g_echo_long_max.store(static_cast<std::int64_t>(
            method_static::static_method("sEchoLong")->call(std::numeric_limits<std::int64_t>::max())));
        g_echo_long_min.store(static_cast<std::int64_t>(
            method_static::static_method("sEchoLong")->call(std::numeric_limits<std::int64_t>::min())));
        g_echo_long_zero.store(static_cast<std::int64_t>(
            method_static::static_method("sEchoLong")->call(std::int64_t{ 0 })));

        g_echo_float_half.store(f2bits(static_cast<float>(
            method_static::static_method("sEchoFloat")->call(0.5f))));
        g_echo_float_nan.store(f2bits(static_cast<float>(
            method_static::static_method("sEchoFloat")->call(std::numeric_limits<float>::quiet_NaN()))));
        g_echo_float_posinf.store(f2bits(static_cast<float>(
            method_static::static_method("sEchoFloat")->call(std::numeric_limits<float>::infinity()))));
        g_echo_float_max.store(f2bits(static_cast<float>(
            method_static::static_method("sEchoFloat")->call(std::numeric_limits<float>::max()))));

        g_echo_double_negzero.store(d2bits(static_cast<double>(
            method_static::static_method("sEchoDouble")->call(-0.0))));
        g_echo_double_nan.store(d2bits(static_cast<double>(
            method_static::static_method("sEchoDouble")->call(std::numeric_limits<double>::quiet_NaN()))));
        g_echo_double_neginf.store(d2bits(static_cast<double>(
            method_static::static_method("sEchoDouble")->call(-std::numeric_limits<double>::infinity()))));
        g_echo_double_max.store(d2bits(static_cast<double>(
            method_static::static_method("sEchoDouble")->call(std::numeric_limits<double>::max()))));

        {
            g_echo_string_empty   = method_static::static_method("sEchoString")->call(std::string{ "" }).as_string();
            g_echo_string_unicode = method_static::static_method("sEchoString")->call(std::string{ "caf\xC3\xA9" }).as_string();
            g_echo_string_captured.store(true);
        }

        // ================================================================
        //  (I) MULTI-ARGUMENT slot-shape digests (RETURN only, path-indep)
        // ================================================================
        g_sum_ii.store(static_cast<std::int32_t>(
            method_static::static_method("sSumII")->call(std::int32_t{ 11 }, std::int32_t{ 22 })));
        g_sum_jj.store(static_cast<std::int64_t>(
            method_static::static_method("sSumJJ")->call(
                std::int64_t{ 0x0011223344556677LL }, std::int64_t{ 0x7766554433221100LL })));

        g_sum_dd.store(d2bits(static_cast<double>(
            method_static::static_method("sSumDD")->call(2.5, 1.25))));
        g_sum_dd_captured.store(true);
        g_sum_ff.store(f2bits(static_cast<float>(
            method_static::static_method("sSumFF")->call(2.5f, 1.25f))));
        g_sum_ff_captured.store(true);

        g_bool_xor_tf.store(method_static::static_method("sBoolXor")->call(true, false) ? 1 : 0);
        g_bool_xor_tt.store(method_static::static_method("sBoolXor")->call(true, true) ? 1 : 0);

        g_mix_fid.store(d2bits(static_cast<double>(
            method_static::static_method("sMixFID")->call(1.5f, std::int32_t{ 9 }, 0.125))));
        g_mix_fid_captured.store(true);

        g_pack_prims.store(static_cast<std::int64_t>(
            method_static::static_method("sPackPrims")->call(
                true, std::int8_t{ -7 }, std::int16_t{ -12345 }, std::uint16_t{ 0xBEEF },
                std::int32_t{ 0x01020304 }, std::int64_t{ 0x0011223344556677LL })));

        g_seven_ints.store(static_cast<std::int32_t>(
            method_static::static_method("s7Ints")->call(
                std::int32_t{ 1 }, std::int32_t{ 2 }, std::int32_t{ 3 }, std::int32_t{ 4 },
                std::int32_t{ 5 }, std::int32_t{ 6 }, std::int32_t{ 7 })));

        // ================================================================
        //  (J) EXTRA overloads — float and (int,long), by type and signature
        // ================================================================
        g_poly_by_float.store(static_cast<std::int32_t>(
            method_static::static_method("sPoly")->call(2.5f)));
        g_poly_by_intlong.store(static_cast<std::int32_t>(
            method_static::static_method("sPoly")->call(std::int32_t{ 1 }, std::int64_t{ 2 })));

        g_poly_sig_float.store(static_cast<std::int32_t>(
            method_static::static_method("sPoly", "(F)I")->call(9.0f)));
        g_poly_sig_double.store(static_cast<std::int32_t>(
            method_static::static_method("sPoly", "(D)I")->call(9.0)));
        g_poly_sig_intint.store(static_cast<std::int32_t>(
            method_static::static_method("sPoly", "(II)I")->call(std::int32_t{ 8 }, std::int32_t{ 9 })));
        g_poly_sig_intlong.store(static_cast<std::int32_t>(
            method_static::static_method("sPoly", "(IJ)I")->call(std::int32_t{ 8 }, std::int64_t{ 9 })));

        // ================================================================
        //  (K) name+signature overload REJECTION (absent overload / instance)
        // ================================================================
        {
            // (B)I is not a declared sPoly overload -> nullopt (static path fails
            // closed for a non-existent signature).
            auto missing = method_static::static_method("sPoly", "(B)I");
            g_sig_missing_overload.store(missing.has_value() ? 1 : 0);

            // iEcho IS (I)I but it is an INSTANCE method -> the JVM_ACC_STATIC gate
            // on the static name+signature path must reject it.
            auto inst_sig = method_static::static_method("iEcho", "(I)I");
            g_sig_instance_rejected.store(inst_sig.has_value() ? 1 : 0);
        }

        // ##################################################################
        //  mstat3_* — BATCH-18 DEEPENING (new inputs; all primitive returns are
        //  path-independent + HARD unless noted).
        // ##################################################################

        // ---- (L) 8-arg static call — the maximum arity (no receiver) ----
        // params[8]/jvalue[8] are both compile-time capped at 8 (a 9-arg call()
        // would fail to COMPILE on either path — so 8 is the tested ceiling and
        // a runtime 9-arg overflow case is impossible by construction).
        g_eight_ints.store(static_cast<std::int32_t>(
            method_static::static_method("s8Ints")->call(
                std::int32_t{ 1 }, std::int32_t{ 2 }, std::int32_t{ 3 }, std::int32_t{ 4 },
                std::int32_t{ 5 }, std::int32_t{ 6 }, std::int32_t{ 7 }, std::int32_t{ 8 })));

        // ---- (M) wide/narrow interleave + back-to-back wide pairs ----
        g_wide_shape.store(static_cast<std::int64_t>(
            method_static::static_method("sWideShape")->call(
                std::int64_t{ 0x0011223344556677LL }, std::int32_t{ 3 },
                std::int64_t{ 0x7766554433221100LL }, std::int32_t{ -5 })));
        g_three_d.store(d2bits(static_cast<double>(
            method_static::static_method("sThreeD")->call(1.5, 0.25, 0.125))));
        g_three_d_captured.store(true);

        // ---- (N) proxy REUSE — one proxy, two call()s ----
        {
            auto p = method_static::static_method("sEchoInt2");
            if (p)
            {
                g_reuse_first.store(static_cast<std::int32_t>(p->call(std::int32_t{ 111 })));
                g_reuse_second.store(static_cast<std::int32_t>(p->call(std::int32_t{ -222 })));
            }
        }

        // ---- (O) object-arg ROUND-TRIP of a library-decoded child ----
        // sMakeChild returns a real child whose usability is call-path dependent;
        // when it is non-null we feed its oop BACK in as the argument of
        // sArgIdentity2 and confirm the identity hash matches what sArgIdentity
        // reports for the same oop.  The null branch is path-independent.
        {
            std::unique_ptr<method_static> child = method_static::static_method("sMakeChild")->call();
            if (child != nullptr)
            {
                const vmhook::oop_t child_oop{ child->get_instance() };
                if (child_oop != nullptr)
                {
                    g_roundtrip_attempted.store(1);
                    auto c1{ std::make_unique<oop_carrier>(child_oop) };
                    const std::int32_t id_a = method_static::static_method("sArgIdentity2")->call(std::move(c1));
                    auto c2{ std::make_unique<oop_carrier>(child_oop) };
                    const std::int32_t id_b = method_static::static_method("sArgIdentity")->call(std::move(c2));
                    g_roundtrip_identity_matches.store((id_a != 0 && id_a == id_b) ? 1 : 0);
                }
            }
            std::unique_ptr<oop_carrier> none{};
            g_roundtrip_null_zero.store(
                static_cast<std::int64_t>(
                    method_static::static_method("sArgIdentity2")->call(std::move(none))) == 0 ? 1 : 0);
        }

        // ---- (P) null + non-trivial String ARGUMENT ----
        {
            std::unique_ptr<oop_carrier> none{};
            g_string_arg_len_null.store(static_cast<std::int32_t>(
                method_static::static_method("sStringArgLen")->call(std::move(none))));
        }
        g_string_arg_len_unicode.store(static_cast<std::int32_t>(
            method_static::static_method("sStringArgLen")->call(std::string{ "caf\xC3\xA9" })));

        // ---- (Q) interface DEFAULT (instance) method rejected by static gate ---
        {
            auto p = static_iface::static_method("ifaceDefaultValue");
            g_iface_default_rejected.store(p.has_value() ? 1 : 0);
            auto p_sig = static_iface::static_method("ifaceDefaultValue", "()I");
            g_iface_default_sig_rejected.store(p_sig.has_value() ? 1 : 0);
        }

        // ---- (R) inherited static via SUB, resolved by NAME+SIGNATURE ----
        {
            auto p = static_sub::static_method("baseStaticValue", "()I");
            g_inherited_sig_resolves.store(p.has_value() ? 1 : 0);
            if (p)
            {
                g_inherited_sig_is_static.store(p->is_static() ? 1 : 0);
                g_inherited_sig_ret.store(static_cast<std::int32_t>(p->call()));
            }
        }

        // ##################################################################
        //  mstat4_* — DEEPER INPUT COVERAGE (all path-INDEPENDENT, HARD).
        //  Every call below returns a primitive / int / String or only probes a
        //  resolution / is_static() / get_compressed_oop() accessor, so the
        //  capture is bit-identical on the call_stub and call_jni paths.  Every
        //  fixture method used here already exists.
        // ##################################################################

        // ---- (S) sRecordInt: int arg lands at parameter slot 0, no receiver ----
        method_static::static_method("sRecordInt")->call(std::int32_t{ 0x1234ABCD });
        g_record_int_field.store(method_static::get_recorded_int_arg());

        // ---- (T) sBoolXor full truth table (FF / FT) ----
        g_bool_xor_ff.store(method_static::static_method("sBoolXor")->call(false, false) ? 1 : 0);
        g_bool_xor_ft.store(method_static::static_method("sBoolXor")->call(false, true) ? 1 : 0);

        // ---- (U) sArrayLen(null) -> -1 (path-independent int return) ----
        {
            std::unique_ptr<oop_carrier> none{};
            g_array_len_null.store(static_cast<std::int32_t>(
                method_static::static_method("sArrayLen")->call(std::move(none))));
        }

        // ---- (V) sStringArgLen empty / ASCII boundaries ----
        g_string_arg_len_empty.store(static_cast<std::int32_t>(
            method_static::static_method("sStringArgLen")->call(std::string{ "" })));
        g_string_arg_len_ascii.store(static_cast<std::int32_t>(
            method_static::static_method("sStringArgLen")->call(std::string{ "abcde" })));

        // ---- (W) sEchoString longer ASCII round-trip (digits + punctuation) ----
        {
            g_echo_string_ascii =
                method_static::static_method("sEchoString")->call(std::string{ "Echo-123/!?" }).as_string();
            g_echo_string_ascii_captured.store(true);
        }

        // ---- (X) sArgIdentity / sArgIdentity2 idempotency + agreement ----
        if (self)
        {
            const vmhook::oop_t self_oop{ self->get_instance() };
            if (self_oop != nullptr)
            {
                auto c1{ std::make_unique<oop_carrier>(self_oop) };
                const std::int32_t id_a = method_static::static_method("sArgIdentity")->call(std::move(c1));
                auto c2{ std::make_unique<oop_carrier>(self_oop) };
                const std::int32_t id_b = method_static::static_method("sArgIdentity")->call(std::move(c2));
                g_identity_idempotent.store((id_a != 0 && id_a == id_b) ? 1 : 0);
                auto c3{ std::make_unique<oop_carrier>(self_oop) };
                const std::int32_t id_c = method_static::static_method("sArgIdentity2")->call(std::move(c3));
                g_identity_two_methods_agree.store((id_a != 0 && id_a == id_c) ? 1 : 0);
            }
        }

        // ---- (Y) sSumII / sSumJJ boundary slot-order digests ----
        g_sum_ii_zero_negone.store(static_cast<std::int32_t>(
            method_static::static_method("sSumII")->call(std::int32_t{ 0 }, std::int32_t{ -1 })));
        g_sum_ii_min_max.store(static_cast<std::int32_t>(
            method_static::static_method("sSumII")->call(
                std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max())));
        g_sum_jj_zero_one.store(static_cast<std::int64_t>(
            method_static::static_method("sSumJJ")->call(std::int64_t{ 0 }, std::int64_t{ 1 })));

        // ---- (Z) sEchoInt / sEchoInt2 agreement + small-int boundaries ----
        g_echo_int_one.store(static_cast<std::int32_t>(
            method_static::static_method("sEchoInt")->call(std::int32_t{ 1 })));
        g_echo_int2_zero.store(static_cast<std::int32_t>(
            method_static::static_method("sEchoInt2")->call(std::int32_t{ 0 })));
        {
            const std::int32_t a = method_static::static_method("sEchoInt")->call(std::int32_t{ 7 });
            const std::int32_t b = method_static::static_method("sEchoInt2")->call(std::int32_t{ 7 });
            g_echo_int_methods_agree.store((a == 7 && a == b) ? 1 : 0);
        }

        // ---- (AA) is_static() true + null-receiver-oop for more statics ----
        g_isstatic_spoly.store(method_static::static_method("sPoly", "(I)I")->is_static() ? 1 : 0);
        g_isstatic_ssumii.store(method_static::static_method("sSumII")->is_static() ? 1 : 0);
        g_isstatic_sarraylen.store(method_static::static_method("sArrayLen")->is_static() ? 1 : 0);
        g_isstatic_s8ints.store(method_static::static_method("s8Ints")->is_static() ? 1 : 0);
        g_isstatic_sargidentity.store(method_static::static_method("sArgIdentity")->is_static() ? 1 : 0);
        {
            auto p_poly = method_static::static_method("sPoly", "(I)I");
            g_recv_oop_zero_poly.store((p_poly && p_poly->get_compressed_oop() == 0u) ? 1 : 0);
            auto p_sum = method_static::static_method("sSumII");
            g_recv_oop_zero_sumii.store((p_sum && p_sum->get_compressed_oop() == 0u) ? 1 : 0);
            auto p_arr = method_static::static_method("sArrayLen");
            g_recv_oop_zero_arraylen.store((p_arr && p_arr->get_compressed_oop() == 0u) ? 1 : 0);
        }

        // ---- (AB) static_method(name, sig) is_static + null-oop for more sigs ----
        {
            auto p = method_static::static_method("sSumII", "(II)I");
            if (p)
            {
                g_sig_sumii_is_static.store(p->is_static() ? 1 : 0);
                g_sig_sumii_oop_zero.store(p->get_compressed_oop() == 0u ? 1 : 0);
                g_sig_sumii_ret.store(static_cast<std::int32_t>(
                    p->call(std::int32_t{ 3 }, std::int32_t{ 4 })));
            }
        }
        {
            auto p = method_static::static_method("sEchoLong", "(J)J");
            if (p)
            {
                g_sig_echolong_is_static.store(p->is_static() ? 1 : 0);
                g_sig_echolong_ret.store(static_cast<std::int64_t>(
                    p->call(std::int64_t{ 0x0011223344556677LL })));
            }
        }

        // ---- (AC) sPoly by-TYPE vs by-SIGNATURE agreement cross-check ----
        {
            const std::int32_t t_int = method_static::static_method("sPoly")->call(std::int32_t{ 1 });
            const std::int32_t s_int = method_static::static_method("sPoly", "(I)I")->call(std::int32_t{ 1 });
            g_poly_int_type_eq_sig.store((t_int == k_poly_int && t_int == s_int) ? 1 : 0);

            const std::int32_t t_long = method_static::static_method("sPoly")->call(std::int64_t{ 2 });
            const std::int32_t s_long = method_static::static_method("sPoly", "(J)I")->call(std::int64_t{ 2 });
            g_poly_long_type_eq_sig.store((t_long == k_poly_long && t_long == s_long) ? 1 : 0);

            const std::int32_t t_str = method_static::static_method("sPoly")->call(std::string{ "z" });
            const std::int32_t s_str = method_static::static_method("sPoly", "(Ljava/lang/String;)I")->call(std::string{ "z" });
            g_poly_string_type_eq_sig.store((t_str == k_poly_string && t_str == s_str) ? 1 : 0);

            const std::int32_t t_flt = method_static::static_method("sPoly")->call(2.5f);
            const std::int32_t s_flt = method_static::static_method("sPoly", "(F)I")->call(2.5f);
            g_poly_float_type_eq_sig.store((t_flt == k_poly_float && t_flt == s_flt) ? 1 : 0);

            const std::int32_t t_dbl = method_static::static_method("sPoly")->call(3.5);
            const std::int32_t s_dbl = method_static::static_method("sPoly", "(D)I")->call(3.5);
            g_poly_double_type_eq_sig.store((t_dbl == k_poly_double && t_dbl == s_dbl) ? 1 : 0);

            const std::int32_t t_ii = method_static::static_method("sPoly")->call(std::int32_t{ 4 }, std::int32_t{ 5 });
            const std::int32_t s_ii = method_static::static_method("sPoly", "(II)I")->call(std::int32_t{ 4 }, std::int32_t{ 5 });
            g_poly_intint_type_eq_sig.store((t_ii == k_poly_int2 && t_ii == s_ii) ? 1 : 0);

            const std::int32_t t_il = method_static::static_method("sPoly")->call(std::int32_t{ 1 }, std::int64_t{ 2 });
            const std::int32_t s_il = method_static::static_method("sPoly", "(IJ)I")->call(std::int32_t{ 1 }, std::int64_t{ 2 });
            g_poly_intlong_type_eq_sig.store((t_il == k_poly_intlong && t_il == s_il) ? 1 : 0);
        }

        // ---- (AD) sEchoLong more boundaries ----
        g_echo_long_one.store(static_cast<std::int64_t>(
            method_static::static_method("sEchoLong")->call(std::int64_t{ 1 })));
        g_echo_long_negone.store(static_cast<std::int64_t>(
            method_static::static_method("sEchoLong")->call(std::int64_t{ -1 })));
        g_echo_long_alt.store(static_cast<std::int64_t>(
            method_static::static_method("sEchoLong")->call(std::int64_t{ 0x5555555555555555LL })));

        // ---- (AE) sEchoChar more code-unit boundaries (echo is exact) ----
        g_echo_char_one.store(static_cast<std::uint16_t>(
            method_static::static_method("sEchoChar")->call(std::uint16_t{ 1 })));
        g_echo_char_surrogate.store(static_cast<std::uint16_t>(
            method_static::static_method("sEchoChar")->call(std::uint16_t{ 0xD800 })));

        // ---- (AF) sEchoByte / sEchoShort interior boundaries (1, -1) ----
        g_echo_byte_one.store(static_cast<std::int8_t>(
            method_static::static_method("sEchoByte")->call(std::int8_t{ 1 })));
        g_echo_byte_negone.store(static_cast<std::int8_t>(
            method_static::static_method("sEchoByte")->call(std::int8_t{ -1 })));
        g_echo_short_one.store(static_cast<std::int16_t>(
            method_static::static_method("sEchoShort")->call(std::int16_t{ 1 })));
        g_echo_short_negone.store(static_cast<std::int16_t>(
            method_static::static_method("sEchoShort")->call(std::int16_t{ -1 })));

        (void)self;
        g_all_calls_ran.store(true);
    }
}

VMHOOK_JVM_MODULE(method_static)
{
    vmhook::register_class<method_static>("vmhook/fixtures/MethodStatic");
    vmhook::register_class<static_sub>("vmhook/fixtures/MethodStatic$StaticSub");
    vmhook::register_class<abstract_with_static>("vmhook/fixtures/MethodStatic$AbstractWithStatic");
    vmhook::register_class<static_iface>("vmhook/fixtures/MethodStatic$StaticIface");
    vmhook::register_class<static_enum>("vmhook/fixtures/MethodStatic$StaticEnum");
    vmhook::register_class<clinit_probe>("vmhook/fixtures/MethodStatic$ClinitProbe");
    // oop_carrier is a bare oop holder; it is NEVER register_class<>'d — its
    // instances are only ever passed AS ARGUMENTS (raw oop), never resolved.

    // Record which dispatch path the live JDK uses (object/array-return usability
    // is path-dependent; primitives + String are path-independent).
    const bool call_stub_present{ vmhook::detail::find_call_stub_entry() != nullptr };
    ctx.record(std::string{ "[INFO] method_static dispatch path: " }
               + (call_stub_present ? "call_stub fast path (object/array returns are real compressed OOPs)"
                                    : "JNI fallback (object/array returns TRUNCATED/freed — KNOWN call_jni flaw)"));

    {
        auto handle{ vmhook::scoped_hook<method_static>(
            "trigger",
            [](vmhook::return_value&,
               const std::unique_ptr<method_static>& self,
               std::int32_t /*delta*/)
            {
                g_detour_calls.fetch_add(1, std::memory_order_relaxed);
                g_detour_saw_self.store(self != nullptr, std::memory_order_relaxed);
                run_all_calls(self);
            }) };
        ctx.check("ms_hook_installed", handle.installed());

        const bool done{ ctx.run_probe(
            [](bool v) { method_static::set_go(v); },
            []() { return method_static::get_done(); }) };

        ctx.check("ms_probe_completed", done);
        ctx.check("ms_detour_fired", g_detour_calls.load(std::memory_order_relaxed) >= 1);
        ctx.check("ms_detour_saw_self", g_detour_saw_self.load(std::memory_order_relaxed));
        ctx.check("ms_all_calls_ran", g_all_calls_ran.load(std::memory_order_relaxed));

        // ==================================================================
        //  1) PRIMITIVE static returns — exact value per type + boundaries
        // ==================================================================
        ctx.check("ms_bool_true_static", g_bool_true.load() == 1);
        ctx.check("ms_bool_false_static", g_bool_false.load() == 0);

        ctx.check("ms_byte_max_127", g_byte_max.load() == 127);
        ctx.check("ms_byte_min_neg128", g_byte_min.load() == -128);
        ctx.check("ms_byte_negone", g_byte_negone.load() == -1);
        ctx.check("ms_byte_negone_sign_extends_to_int", g_byte_negone_as_int.load() == -1);

        ctx.check("ms_short_max_32767", g_short_max.load() == 32767);
        ctx.check("ms_short_min_neg32768", g_short_min.load() == -32768);
        ctx.check("ms_short_negone", g_short_negone.load() == -1);

        ctx.check("ms_char_A_65", g_char_a.load() == 65);
        ctx.check("ms_char_max_65535", g_char_max.load() == 65535);
        ctx.check("ms_char_max_zero_extends_to_int_65535", g_char_max_as_int.load() == 65535);

        ctx.check("ms_int_max_2147483647", g_int_max.load() == 2147483647LL);
        ctx.check("ms_int_min_neg2147483648", g_int_min.load() == -2147483648LL);
        ctx.check("ms_int_42", g_int_42.load() == 42);
        ctx.check("ms_int_negone", g_int_negone.load() == -1);

        ctx.check("ms_long_max", g_long_max.load() == std::numeric_limits<std::int64_t>::max());
        ctx.check("ms_long_min", g_long_min.load() == std::numeric_limits<std::int64_t>::min());
        ctx.check("ms_long_big_pattern", g_long_big.load() == static_cast<std::int64_t>(0x0123456789ABCDEFLL));

        ctx.check("ms_fp_captured", g_fp_captured.load());
        ctx.check("ms_float_half", bits2f(g_float_half.load()) == 0.5f);
        {
            const float nz = bits2f(g_float_negzero.load());
            ctx.check("ms_float_negzero_value", nz == 0.0f);
            ctx.check("ms_float_negzero_signbit", std::signbit(nz));
        }
        ctx.check("ms_float_nan_isnan", std::isnan(bits2f(g_float_nan.load())));
        {
            const float pinf = bits2f(g_float_posinf.load());
            ctx.check("ms_float_posinf_isinf", std::isinf(pinf) && pinf > 0.0f);
        }
        ctx.check("ms_float_max", bits2f(g_float_max.load()) == std::numeric_limits<float>::max());

        ctx.check("ms_double_pi_bits", g_double_pi.load() == d2bits(3.141592653589793));
        {
            const double nz = bits2d(g_double_negzero.load());
            ctx.check("ms_double_negzero_value", nz == 0.0);
            ctx.check("ms_double_negzero_signbit", std::signbit(nz));
        }
        ctx.check("ms_double_nan_isnan", std::isnan(bits2d(g_double_nan.load())));
        {
            const double ninf = bits2d(g_double_neginf.load());
            ctx.check("ms_double_neginf_isinf", std::isinf(ninf) && ninf < 0.0);
        }
        ctx.check("ms_double_max", bits2d(g_double_max.load()) == std::numeric_limits<double>::max());

        // void: is_void()==true AND the body's side effect ran.  The static
        // recorder hit counter aggregates sVoidBump + sRecord*-family bumps;
        // we assert it is > 0 (every recorder that ran bumped it).
        ctx.check("ms_void_static_is_void", g_void_is_void.load() == 1);
        ctx.check("ms_static_void_side_effect_ran",
                  method_static::get_static_recorder_hits() > 0);

        // ==================================================================
        //  2) STRING static returns — exact UTF-8, empty vs null distinct
        // ==================================================================
        ctx.check("ms_str_captured", g_str_captured.load());
        ctx.check("ms_string_hello_exact", g_str_hello == "hello-static");
        ctx.check("ms_string_hello_is_string", g_str_hello_is_string.load() == 1);
        // café in modified UTF-8 / UTF-8: 'c','a','f',0xC3,0xA9.
        ctx.check("ms_string_unicode_exact", g_str_unicode == "caf\xC3\xA9");
        ctx.check("ms_string_unicode_not_empty", !g_str_unicode.empty());
        ctx.check("ms_string_empty_is_empty", g_str_empty.empty());
        // A null String return decodes to "" via as_string() and never crashes
        // (path-independent — the headline guarantee for null String returns).
        ctx.check("ms_string_null_as_string_empty", g_str_null.empty());
        // is_void() on a null String return is PATH-DEPENDENT: the call_stub path
        // yields monostate (is_void()==true); the call_jni path eagerly builds an
        // empty std::string (is_void()==false, is_string()==true).  Assert only on
        // the call_stub path; record on call_jni.
        if (call_stub_present)
        {
            ctx.check("ms_string_null_is_void", g_str_null_is_void.load() == 1);
        }
        else
        {
            ctx.record(std::string{ "[INFO] ms_string_null_is_void (call_jni) = " }
                       + std::to_string(g_str_null_is_void.load())
                       + " (call_jni decodes null String to empty std::string, not monostate)");
        }

        // ==================================================================
        //  3) OBJECT static returns — null is hard, non-null is path-dep
        // ==================================================================
        ctx.check("ms_object_null_returns_null_unique_ptr",
                  g_obj_null_is_null_uptr.load(std::memory_order_relaxed));
        ctx.check("ms_object_return_not_string", g_obj_child_is_string.load() == 0);
        if (call_stub_present)
        {
            ctx.check("ms_object_null_is_void", g_obj_null_is_void.load() == 1);
        }
        else
        {
            ctx.record(std::string{ "[INFO] ms_object_null_is_void (call_jni) = " }
                       + std::to_string(g_obj_null_is_void.load())
                       + " (call_jni null object -> uint32{0} alternative, decodes to null uptr)");
        }

        if (call_stub_present)
        {
            ctx.check("ms_object_make_child_non_null",
                      g_obj_child_nonnull.load(std::memory_order_relaxed));
            ctx.check("ms_object_make_child_seed_through_wrapper",
                      g_obj_child_seed.load(std::memory_order_relaxed) == k_static_child_seed);
            ctx.check("ms_object_non_null_not_void", g_obj_child_is_void.load() == 0);
        }
        else
        {
            ctx.record(std::string{ "[INFO] ms_object_make_child_non_null (call_jni) = " }
                       + (g_obj_child_nonnull.load(std::memory_order_relaxed) ? "true" : "false"));
            ctx.record(std::string{ "[INFO] ms_object_make_child_seed (call_jni) = " }
                       + std::to_string(g_obj_child_seed.load(std::memory_order_relaxed))
                       + " (expected " + std::to_string(k_static_child_seed) + ")");
            ctx.record(std::string{ "[INFO] ms_object_non_null_is_void (call_jni) = " }
                       + std::to_string(g_obj_child_is_void.load()));
        }

        // ==================================================================
        //  4) NO RECEIVER PASSED to a static method
        // ==================================================================
        ctx.check("ms_no_receiver_echo_returns_arg", g_echo_ret.load() == 13572468);
        ctx.check("ms_no_receiver_recorded_long_return",
                  g_record_long_ret.load() == static_cast<std::int64_t>(0x0011223344556677LL));
        ctx.check("ms_no_receiver_recorded_long_field",
                  method_static::get_recorded_long_arg() == static_cast<std::int64_t>(0x0011223344556677LL));
        ctx.check("ms_no_receiver_three_return",
                  g_record_three_ret.load()
                      == static_cast<std::int64_t>(5) + 0x1122334455667788LL + (-13));
        ctx.check("ms_no_receiver_three_first_int_slot0",
                  method_static::get_recorded_first() == 5);
        ctx.check("ms_no_receiver_three_long_slot1",
                  method_static::get_recorded_second() == 0x1122334455667788LL);
        ctx.check("ms_no_receiver_three_trailing_int_after_long",
                  method_static::get_recorded_third() == -13);
        ctx.check("ms_no_receiver_oop_zero_echo", g_recv_oop_zero_echo.load() == 1);
        ctx.check("ms_no_receiver_oop_zero_int", g_recv_oop_zero_int.load() == 1);
        ctx.check("ms_no_receiver_oop_zero_void", g_recv_oop_zero_void.load() == 1);

        // ==================================================================
        //  5) is_static() ACCESSOR — true for static, false for instance
        // ==================================================================
        ctx.check("ms_is_static_true_int_returner", g_isstatic_sint.load() == 1);
        ctx.check("ms_is_static_true_long_returner", g_isstatic_slong.load() == 1);
        ctx.check("ms_is_static_true_bool_returner", g_isstatic_sbool.load() == 1);
        ctx.check("ms_is_static_true_string_returner", g_isstatic_sstring.load() == 1);
        ctx.check("ms_is_static_true_object_returner", g_isstatic_sobject.load() == 1);
        ctx.check("ms_is_static_true_void", g_isstatic_svoid.load() == 1);
        ctx.check("ms_is_static_true_echo_with_arg", g_isstatic_secho.load() == 1);

        ctx.check("ms_is_static_false_instance_getter", g_isstatic_iget.load() == 0);
        ctx.check("ms_is_static_false_instance_string", g_isstatic_ilabel.load() == 0);
        ctx.check("ms_is_static_false_instance_echo", g_isstatic_iecho.load() == 0);
        ctx.check("ms_is_static_false_instance_void", g_isstatic_itouch.load() == 0);
        ctx.check("ms_is_static_false_instance_trigger", g_isstatic_trigger.load() == 0);

        // ==================================================================
        //  5b) static_method(name, signature) overload — also static + works
        // ==================================================================
        ctx.check("ms_sig_overload_is_static", g_sig_isstatic.load() == 1);
        ctx.check("ms_sig_overload_echo_returns_arg", g_sig_echo_ret.load() == 24681357);

        // ==================================================================
        //  6) static_method() REJECTS instance methods (JVM_ACC_STATIC gate)
        // ==================================================================
        ctx.check("ms_static_method_rejects_instance_iget",
                  g_flaw_iget_has_value.load() == 0);
        ctx.check("ms_static_method_rejects_instance_iecho",
                  g_flaw_iecho_has_value.load() == 0);
        ctx.check("ms_static_method_resolves_real_static",
                  g_static_real_has_value.load() == 1);
        ctx.check("ms_static_method_rejected_instance_not_static_iget",
                  g_flaw_iget_is_static.load() == 0);
        ctx.check("ms_static_method_rejected_instance_not_static_iecho",
                  g_flaw_iecho_is_static.load() == 0);

        // ##################################################################
        //  mstat_* — EXHAUSTIVE EXPANSION
        // ##################################################################

        // ---- (A) every non-int primitive + String + object ARGUMENT echo ----
        ctx.check("mstat_arg_bool_echo_return", g_arg_bool_ret.load() == 1);
        ctx.check("mstat_arg_bool_echo_field", g_arg_bool_field.load() == 1);
        ctx.check("mstat_arg_byte_echo_return", g_arg_byte_ret.load() == -7);
        ctx.check("mstat_arg_byte_echo_field", g_arg_byte_field.load() == -7);
        ctx.check("mstat_arg_short_echo_return", g_arg_short_ret.load() == -12345);
        ctx.check("mstat_arg_short_echo_field", g_arg_short_field.load() == -12345);
        // 0xBEEF as a Java char is 48879 (zero-extended, unsigned 16-bit).
        ctx.check("mstat_arg_char_echo_return", g_arg_char_ret.load() == 0xBEEF);
        ctx.check("mstat_arg_char_echo_field", g_arg_char_field.load() == 0xBEEF);
        ctx.check("mstat_arg_long_echo_return",
                  g_arg_long_ret.load() == static_cast<std::int64_t>(0x7766554433221100LL));
        ctx.check("mstat_arg_long_echo_field",
                  g_arg_long_field.load() == static_cast<std::int64_t>(0x7766554433221100LL));
        // -0.0f survives bit-exact through the arg packer (signbit preserved).
        ctx.check("mstat_arg_float_echo_return_bits", g_arg_float_ret.load() == f2bits(-0.0f));
        ctx.check("mstat_arg_float_echo_field_bits", g_arg_float_field.load() == f2bits(-0.0f));
        ctx.check("mstat_arg_double_echo_return_bits", g_arg_double_ret.load() == d2bits(3.141592653589793));
        ctx.check("mstat_arg_double_echo_field_bits", g_arg_double_field.load() == d2bits(3.141592653589793));
        ctx.check("mstat_arg_string_captured", g_arg_string_captured.load());
        ctx.check("mstat_arg_string_echo_return", g_arg_string_ret == "round-trip");
        ctx.check("mstat_arg_string_echo_field", g_arg_string_field == "round-trip");
        // Object argument: a non-null oop reached slot 0 (its identity is non-zero
        // and matches what the static method stamped), and a null oop -> 0.
        ctx.check("mstat_arg_object_identity_nonzero", g_arg_object_identity_nonzero.load() == 1);
        ctx.check("mstat_arg_object_field_matches_return", g_arg_object_field_matches.load() == 1);
        ctx.check("mstat_arg_object_null_identity_zero", g_arg_object_null_zero.load() == 1);

        // ---- (B) array returns + array argument ----
        ctx.check("mstat_array_int_resolves", g_arr_int_resolves.load() == 1);
        ctx.check("mstat_array_int_return_not_string", g_arr_int_not_string.load() == 1);
        ctx.check("mstat_array_str_resolves", g_arr_str_resolves.load() == 1);
        ctx.check("mstat_array_obj_resolves", g_arr_obj_resolves.load() == 1);
        // Null array return -> null unique_ptr (path-INDEPENDENT, hard assert).
        ctx.check("mstat_array_null_returns_null_unique_ptr",
                  g_arr_null_is_null_uptr.load(std::memory_order_relaxed));
        // Array-RETURN element/length walks depend on a usable returned oop, which
        // is call-path dependent (call_jni truncates the handle): gate on
        // call_stub_present, record on call_jni — mirrors the object-return policy.
        if (call_stub_present)
        {
            ctx.check("mstat_array_int_non_null_not_void", g_arr_int_not_void.load() == 1);
            ctx.check("mstat_array_int_length_4", g_arr_int_len_ok.load() == 1);
            ctx.check("mstat_array_int_elements_10_40", g_arr_int_elems_ok.load() == 1);
            ctx.check("mstat_array_str_length_3", g_arr_str_len_ok.load() == 1);
            ctx.check("mstat_array_obj_length_2", g_arr_obj_len_ok.load() == 1);
        }
        else
        {
            ctx.record(std::string{ "[INFO] mstat_array_int (call_jni) not_void=" }
                       + std::to_string(g_arr_int_not_void.load())
                       + " len_ok=" + std::to_string(g_arr_int_len_ok.load())
                       + " elems_ok=" + std::to_string(g_arr_int_elems_ok.load())
                       + " (call_jni truncates the returned array handle)");
            ctx.record(std::string{ "[INFO] mstat_array_str_length_ok (call_jni) = " }
                       + std::to_string(g_arr_str_len_ok.load()));
            ctx.record(std::string{ "[INFO] mstat_array_obj_length_ok (call_jni) = " }
                       + std::to_string(g_arr_obj_len_ok.load()));
        }
        // Array ARGUMENT pass-through is path-INDEPENDENT (a raw oop arg is
        // consumed correctly on both paths): hard-assert the echoed length when the
        // fresh allocation succeeded.
        if (g_arr_arg_attempted.load() == 1)
        {
            ctx.check("mstat_array_arg_length_echo_4", g_arr_arg_len_ret.load() == k_arr_int_len);
        }
        else
        {
            ctx.record("[INFO] mstat_array_arg_length_echo: int[] allocation unavailable "
                       "on this run (TLAB/GC) — array-argument echo skipped (no crash).");
        }

        // ---- (C) overloaded static resolution — by arg type and by signature --
        ctx.check("mstat_overload_by_int", g_poly_by_int.load() == k_poly_int);
        ctx.check("mstat_overload_by_long", g_poly_by_long.load() == k_poly_long);
        ctx.check("mstat_overload_by_double", g_poly_by_double.load() == k_poly_double);
        ctx.check("mstat_overload_by_string", g_poly_by_string.load() == k_poly_string);
        ctx.check("mstat_overload_by_int_int", g_poly_by_int2.load() == k_poly_int2);
        ctx.check("mstat_overload_sig_int", g_poly_sig_int.load() == k_poly_int);
        ctx.check("mstat_overload_sig_long", g_poly_sig_long.load() == k_poly_long);
        ctx.check("mstat_overload_sig_string", g_poly_sig_string.load() == k_poly_string);

        // ---- (D) static-field mutation side effect ----
        ctx.check("mstat_mutate_returns_prior_zero", g_mutate_prior.load() == 0);
        ctx.check("mstat_mutate_side_effect_observed", g_mutate_observed.load() == 1);

        // ---- (E) STATIC method via INSTANCE wrapper dispatches statically ----
        ctx.check("mstat_instance_wrapper_static_resolves", g_inst_static_resolves.load() == 1);
        // The headline contracts: is_static() is true (so the call passes NO
        // receiver), and the static method returns its constant.  The proxy's
        // stored receiver oop is the wrapper's (non-zero) — see the global note;
        // the static-ness is keyed on is_static(), not a nulled oop.
        ctx.check("mstat_instance_wrapper_static_is_static", g_inst_static_is_static.load() == 1);
        ctx.check("mstat_instance_wrapper_static_carries_wrapper_oop", g_inst_static_oop_nonzero.load() == 1);
        ctx.check("mstat_instance_wrapper_static_returns_42", g_inst_static_ret.load() == 42);

        // ---- (F) STATIC on interface / abstract / enum / inherited ----
        ctx.check("mstat_iface_static_resolves", g_iface_resolves.load() == 1);
        ctx.check("mstat_iface_static_is_static", g_iface_is_static.load() == 1);
        ctx.check("mstat_iface_static_returns_value", g_iface_ret.load() == k_iface_static);

        ctx.check("mstat_abstract_static_resolves", g_abstract_resolves.load() == 1);
        ctx.check("mstat_abstract_static_is_static", g_abstract_is_static.load() == 1);
        ctx.check("mstat_abstract_static_returns_value", g_abstract_ret.load() == k_abstract_static);

        ctx.check("mstat_enum_static_resolves", g_enum_resolves.load() == 1);
        ctx.check("mstat_enum_static_is_static", g_enum_is_static.load() == 1);
        ctx.check("mstat_enum_static_returns_value", g_enum_ret.load() == k_enum_static);

        ctx.check("mstat_sub_own_static_resolves", g_sub_own_resolves.load() == 1);
        ctx.check("mstat_sub_own_static_returns_value", g_sub_own_ret.load() == k_sub_static);
        // Inherited static: declared on StaticBase, named through the StaticSub
        // wrapper.  Statics are NOT polymorphic — the super's declaration is what
        // resolves (super-chain walk on the static get_method path).
        ctx.check("mstat_inherited_static_resolves", g_inherited_resolves.load() == 1);
        ctx.check("mstat_inherited_static_is_static", g_inherited_is_static.load() == 1);
        ctx.check("mstat_inherited_static_returns_super_value", g_inherited_ret.load() == k_base_static);

        // ---- (G) <clinit> characterization ----
        ctx.check("mstat_clinit_static_resolves", g_clinit_resolves.load() == 1);
        // The static call returns the post-<clinit> sentinel: first touch through
        // a static call initialized the class (the observable contract; HARD).
        ctx.check("mstat_clinit_call_returns_post_init_value",
                  g_clinit_ret.load() == k_clinit_value);
        // After the static call the class is definitely initialized.
        ctx.check("mstat_clinit_initialized_after_call", g_clinit_post_initialized.load() == 1);
        // The pre-call initialized state is JDK/timing-sensitive (a raw static
        // field read does not itself force <clinit>, but load-vs-init ordering and
        // any concurrent init can vary) — recorded, never asserted.
        ctx.record(std::string{ "[INFO] mstat_clinit initialized BEFORE first static call = " }
                   + std::to_string(g_clinit_pre_initialized.load())
                   + " (0 expected on a JDK where the class was loaded-but-not-initialized; "
                     "first static call then forces <clinit>)");

        // ##################################################################
        //  mstat2_* — DEEPER INPUT COVERAGE (all path-INDEPENDENT, HARD)
        // ##################################################################

        // ---- (H) more boundary VALUES through the single-arg echoes ----
        ctx.check("mstat2_echo_bool_false", g_echo_bool_false.load() == 0);
        ctx.check("mstat2_echo_byte_zero", g_echo_byte_zero.load() == 0);
        ctx.check("mstat2_echo_byte_max_127", g_echo_byte_max.load() == 127);
        ctx.check("mstat2_echo_byte_min_neg128", g_echo_byte_min.load() == -128);
        ctx.check("mstat2_echo_short_max_32767", g_echo_short_max.load() == 32767);
        ctx.check("mstat2_echo_short_min_neg32768", g_echo_short_min.load() == -32768);
        ctx.check("mstat2_echo_char_zero", g_echo_char_zero.load() == 0);
        ctx.check("mstat2_echo_char_max_65535", g_echo_char_max.load() == 65535);
        ctx.check("mstat2_echo_char_A_65", g_echo_char_a.load() == 65);
        ctx.check("mstat2_echo_int_max", g_echo_int_max.load() == 2147483647LL);
        ctx.check("mstat2_echo_int_min", g_echo_int_min.load() == -2147483648LL);
        ctx.check("mstat2_echo_int_zero", g_echo_int_zero.load() == 0);
        ctx.check("mstat2_echo_int_negone", g_echo_int_negone.load() == -1);
        ctx.check("mstat2_echo_long_max", g_echo_long_max.load() == std::numeric_limits<std::int64_t>::max());
        ctx.check("mstat2_echo_long_min", g_echo_long_min.load() == std::numeric_limits<std::int64_t>::min());
        ctx.check("mstat2_echo_long_zero", g_echo_long_zero.load() == 0);
        ctx.check("mstat2_echo_float_half", bits2f(g_echo_float_half.load()) == 0.5f);
        ctx.check("mstat2_echo_float_nan_isnan", std::isnan(bits2f(g_echo_float_nan.load())));
        {
            const float pinf = bits2f(g_echo_float_posinf.load());
            ctx.check("mstat2_echo_float_posinf_isinf", std::isinf(pinf) && pinf > 0.0f);
        }
        ctx.check("mstat2_echo_float_max", bits2f(g_echo_float_max.load()) == std::numeric_limits<float>::max());
        {
            const double nz = bits2d(g_echo_double_negzero.load());
            ctx.check("mstat2_echo_double_negzero_value", nz == 0.0);
            ctx.check("mstat2_echo_double_negzero_signbit", std::signbit(nz));
        }
        ctx.check("mstat2_echo_double_nan_isnan", std::isnan(bits2d(g_echo_double_nan.load())));
        {
            const double ninf = bits2d(g_echo_double_neginf.load());
            ctx.check("mstat2_echo_double_neginf_isinf", std::isinf(ninf) && ninf < 0.0);
        }
        ctx.check("mstat2_echo_double_max", bits2d(g_echo_double_max.load()) == std::numeric_limits<double>::max());
        ctx.check("mstat2_echo_string_captured", g_echo_string_captured.load());
        ctx.check("mstat2_echo_string_empty_round_trip", g_echo_string_empty.empty());
        ctx.check("mstat2_echo_string_unicode_round_trip", g_echo_string_unicode == "caf\xC3\xA9");

        // ---- (I) multi-argument slot-shape digests ----
        ctx.check("mstat2_sum_ii", g_sum_ii.load() == (static_cast<std::int64_t>(11) * 1000003 + 22));
        {
            // Recompute a*1000003 + b with unsigned 64-bit wrapping (matches Java
            // long arithmetic; avoids signed-overflow UB in the expectation).
            const std::uint64_t a{ static_cast<std::uint64_t>(0x0011223344556677ULL) };
            const std::uint64_t bb{ static_cast<std::uint64_t>(0x7766554433221100ULL) };
            const std::int64_t expected_jj{ static_cast<std::int64_t>(a * 1000003ULL + bb) };
            ctx.check("mstat2_sum_jj", g_sum_jj.load() == expected_jj);
        }
        ctx.check("mstat2_sum_dd_captured", g_sum_dd_captured.load());
        ctx.check("mstat2_sum_dd_bits", bits2d(g_sum_dd.load()) == 11.25);
        ctx.check("mstat2_sum_ff_captured", g_sum_ff_captured.load());
        ctx.check("mstat2_sum_ff_bits", bits2f(g_sum_ff.load()) == 11.25f);
        ctx.check("mstat2_bool_xor_true_false", g_bool_xor_tf.load() == 1);
        ctx.check("mstat2_bool_xor_true_true", g_bool_xor_tt.load() == 0);
        ctx.check("mstat2_mix_fid_captured", g_mix_fid_captured.load());
        // 1.5*1000 + 9*7 + 0.125 = 1563.125 (all exactly representable).
        ctx.check("mstat2_mix_fid_bits", bits2d(g_mix_fid.load()) == 1563.125);
        // sPackPrims digest — recomputed with the SAME wrapping arithmetic Java
        // uses (unsigned 64-bit to avoid signed-shift UB; bit-identical result).
        {
            const std::uint64_t z{ 1u };
            const std::uint64_t b{ static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int8_t>(-7))) };
            const std::uint64_t s{ static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int16_t>(-12345))) };
            const std::uint64_t c{ static_cast<std::uint64_t>(std::uint16_t{ 0xBEEF }) };
            const std::uint64_t i{ static_cast<std::uint64_t>(static_cast<std::int64_t>(std::int32_t{ 0x01020304 })) };
            const std::uint64_t j{ static_cast<std::uint64_t>(static_cast<std::int64_t>(0x0011223344556677LL)) };
            std::uint64_t acc{ 0u };
            acc += z;
            acc += b << 1;
            acc += s << 9;
            acc += c << 25;
            acc += i << 41;
            acc ^= j;
            const std::int64_t expected_pack{ static_cast<std::int64_t>(acc) };
            ctx.check("mstat2_pack_prims_digest", g_pack_prims.load() == expected_pack);
        }
        ctx.check("mstat2_seven_ints_weighted_sum", g_seven_ints.load() == 140);

        // ---- (J) extra overloads — float and (int,long) ----
        ctx.check("mstat2_overload_by_float", g_poly_by_float.load() == k_poly_float);
        ctx.check("mstat2_overload_by_int_long", g_poly_by_intlong.load() == k_poly_intlong);
        ctx.check("mstat2_overload_sig_float", g_poly_sig_float.load() == k_poly_float);
        ctx.check("mstat2_overload_sig_double", g_poly_sig_double.load() == k_poly_double);
        ctx.check("mstat2_overload_sig_int_int", g_poly_sig_intint.load() == k_poly_int2);
        ctx.check("mstat2_overload_sig_int_long", g_poly_sig_intlong.load() == k_poly_intlong);

        // ---- (K) name+signature overload REJECTION ----
        // An absent signature resolves to nullopt (the static path fails closed).
        ctx.check("mstat2_sig_absent_overload_rejected", g_sig_missing_overload.load() == 0);
        // An instance method matched by exact (I)I is still rejected by the
        // JVM_ACC_STATIC gate on the static name+signature path.
        ctx.check("mstat2_sig_instance_overload_rejected", g_sig_instance_rejected.load() == 0);

        // ##################################################################
        //  mstat3_* — BATCH-18 DEEPENING (all path-INDEPENDENT, HARD)
        // ##################################################################

        // ---- (L) 8-arg static call — the maximum supported arity ----
        // 1 + 2*2 + 3*3 + 4*4 + 5*5 + 6*6 + 7*7 + 8*8 = 1+4+9+16+25+36+49+64 = 204.
        ctx.check("mstat3_eight_ints_weighted_sum", g_eight_ints.load() == 204);

        // ---- (M) wide/narrow interleave + back-to-back wide pairs ----
        {
            // sWideShape(p,q,r,s) = p*7 + (q<<3) + r*31 + (s<<50), Java long math
            // (recomputed with unsigned 64-bit wrapping to avoid signed-overflow UB).
            const std::uint64_t p{ static_cast<std::uint64_t>(0x0011223344556677ULL) };
            const std::uint64_t q{ static_cast<std::uint64_t>(static_cast<std::int64_t>(std::int32_t{ 3 })) };
            const std::uint64_t r{ static_cast<std::uint64_t>(0x7766554433221100ULL) };
            const std::uint64_t s{ static_cast<std::uint64_t>(static_cast<std::int64_t>(std::int32_t{ -5 })) };
            const std::int64_t expected_wide{ static_cast<std::int64_t>(
                p * 7ULL + (q << 3) + r * 31ULL + (s << 50)) };
            ctx.check("mstat3_wide_shape_digest", g_wide_shape.load() == expected_wide);
        }
        ctx.check("mstat3_three_d_captured", g_three_d_captured.load());
        // 1.5*64 + 0.25*8 + 0.125 = 96 + 2 + 0.125 = 98.125 (exactly representable).
        ctx.check("mstat3_three_d_bits", bits2d(g_three_d.load()) == 98.125);

        // ---- (N) proxy REUSE — one proxy serves two distinct calls ----
        ctx.check("mstat3_proxy_reuse_first", g_reuse_first.load() == 111);
        ctx.check("mstat3_proxy_reuse_second", g_reuse_second.load() == -222);

        // ---- (O) object-arg ROUND-TRIP of a library-decoded child ----
        // The null branch is path-independent and HARD; the non-null identity
        // match needs a usable returned child (call-path dependent) so it is
        // asserted only when the round-trip was attempted, recorded otherwise.
        ctx.check("mstat3_object_arg_roundtrip_null_zero", g_roundtrip_null_zero.load() == 1);
        if (g_roundtrip_attempted.load() == 1)
        {
            ctx.check("mstat3_object_arg_roundtrip_identity_matches",
                      g_roundtrip_identity_matches.load() == 1);
        }
        else
        {
            ctx.record(std::string{ "[INFO] mstat3_object_arg_roundtrip: sMakeChild returned no "
                                    "usable oop on this path (call_jni) — identity round-trip "
                                    "characterized only (no crash)." });
        }

        // ---- (P) null + non-trivial String ARGUMENT ----
        // A null String argument must reach the static method as Java null -> -1
        // (path-independent; distinct from the null String RETURN cases above).
        ctx.check("mstat3_string_arg_null_len_neg1", g_string_arg_len_null.load() == -1);
        // "café" is 4 UTF-16 code units (é is a single BMP code point) -> length 4.
        ctx.check("mstat3_string_arg_unicode_len_4", g_string_arg_len_unicode.load() == 4);

        // ---- (Q) interface DEFAULT method rejected by the static gate ----
        // A default method is a NON-static instance method on the interface's own
        // _methods array — the static name(+sig) resolution must skip it.
        ctx.check("mstat3_iface_default_rejected", g_iface_default_rejected.load() == 0);
        ctx.check("mstat3_iface_default_sig_rejected", g_iface_default_sig_rejected.load() == 0);

        // ---- (R) inherited static via SUB, resolved by NAME+SIGNATURE ----
        ctx.check("mstat3_inherited_sig_resolves", g_inherited_sig_resolves.load() == 1);
        ctx.check("mstat3_inherited_sig_is_static", g_inherited_sig_is_static.load() == 1);
        ctx.check("mstat3_inherited_sig_returns_super_value",
                  g_inherited_sig_ret.load() == k_base_static);

        // ##################################################################
        //  mstat4_* — DEEPER INPUT COVERAGE (all path-INDEPENDENT, HARD)
        // ##################################################################

        // ---- (S) sRecordInt: int arg landed intact at slot 0 (no receiver) ----
        ctx.check("mstat4_record_int_slot0",
                  g_record_int_field.load() == static_cast<std::int64_t>(0x1234ABCD));

        // ---- (T) sBoolXor full truth table (FF -> 0, FT -> 1) ----
        ctx.check("mstat4_bool_xor_false_false", g_bool_xor_ff.load() == 0);
        ctx.check("mstat4_bool_xor_false_true", g_bool_xor_ft.load() == 1);

        // ---- (U) sArrayLen(null) -> -1 (path-independent) ----
        ctx.check("mstat4_array_len_null_neg1", g_array_len_null.load() == -1);

        // ---- (V) sStringArgLen empty / ASCII boundaries ----
        ctx.check("mstat4_string_arg_len_empty_zero", g_string_arg_len_empty.load() == 0);
        ctx.check("mstat4_string_arg_len_ascii_5", g_string_arg_len_ascii.load() == 5);

        // ---- (W) sEchoString longer ASCII round-trip ----
        ctx.check("mstat4_echo_string_ascii_captured", g_echo_string_ascii_captured.load());
        ctx.check("mstat4_echo_string_ascii_round_trip", g_echo_string_ascii == "Echo-123/!?");

        // ---- (X) sArgIdentity idempotency + cross-method agreement ----
        // Both probes need a live `self` oop; assert only when captured, record
        // otherwise (no false-fail when the detour saw no receiver).
        if (g_identity_idempotent.load() != -1)
        {
            ctx.check("mstat4_arg_identity_idempotent", g_identity_idempotent.load() == 1);
            ctx.check("mstat4_arg_identity_two_methods_agree",
                      g_identity_two_methods_agree.load() == 1);
        }
        else
        {
            ctx.record("[INFO] mstat4_arg_identity: no self oop captured this run — "
                       "idempotency/agreement cross-check skipped (no crash).");
        }

        // ---- (Y) sSumII / sSumJJ boundary slot-order digests ----
        // sSumII(0,-1)  = 0*1000003 + (-1)        = -1
        ctx.check("mstat4_sum_ii_zero_negone", g_sum_ii_zero_negone.load() == -1);
        {
            // sSumII(MIN,MAX) in 32-bit Java int wraparound arithmetic
            // (MIN*1000003 + MAX), recomputed with unsigned 32-bit wrapping then
            // sign-cast — avoids signed-overflow UB in the expectation.
            const std::uint32_t mn{ static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::min()) };
            const std::uint32_t mx{ static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) };
            const std::int32_t expected_minmax{ static_cast<std::int32_t>(mn * 1000003u + mx) };
            ctx.check("mstat4_sum_ii_min_max", g_sum_ii_min_max.load() == expected_minmax);
        }
        ctx.check("mstat4_sum_jj_zero_one", g_sum_jj_zero_one.load() == 1);

        // ---- (Z) sEchoInt / sEchoInt2 agreement + small-int boundaries ----
        ctx.check("mstat4_echo_int_one", g_echo_int_one.load() == 1);
        ctx.check("mstat4_echo_int2_zero", g_echo_int2_zero.load() == 0);
        ctx.check("mstat4_echo_int_methods_agree", g_echo_int_methods_agree.load() == 1);

        // ---- (AA) is_static() true + null-receiver-oop for more statics ----
        ctx.check("mstat4_is_static_poly", g_isstatic_spoly.load() == 1);
        ctx.check("mstat4_is_static_sumii", g_isstatic_ssumii.load() == 1);
        ctx.check("mstat4_is_static_arraylen", g_isstatic_sarraylen.load() == 1);
        ctx.check("mstat4_is_static_eight_ints", g_isstatic_s8ints.load() == 1);
        ctx.check("mstat4_is_static_arg_identity", g_isstatic_sargidentity.load() == 1);
        ctx.check("mstat4_recv_oop_zero_poly", g_recv_oop_zero_poly.load() == 1);
        ctx.check("mstat4_recv_oop_zero_sumii", g_recv_oop_zero_sumii.load() == 1);
        ctx.check("mstat4_recv_oop_zero_arraylen", g_recv_oop_zero_arraylen.load() == 1);

        // ---- (AB) static_method(name, sig) is_static + null-oop for more sigs ---
        ctx.check("mstat4_sig_sumii_is_static", g_sig_sumii_is_static.load() == 1);
        ctx.check("mstat4_sig_sumii_oop_zero", g_sig_sumii_oop_zero.load() == 1);
        // sSumII(3,4) = 3*1000003 + 4 = 3000013.
        ctx.check("mstat4_sig_sumii_returns_digest", g_sig_sumii_ret.load() == 3000013);
        ctx.check("mstat4_sig_echolong_is_static", g_sig_echolong_is_static.load() == 1);
        ctx.check("mstat4_sig_echolong_echoes_arg",
                  g_sig_echolong_ret.load() == static_cast<std::int64_t>(0x0011223344556677LL));

        // ---- (AC) sPoly by-TYPE vs by-SIGNATURE agreement cross-check ----
        ctx.check("mstat4_poly_int_type_eq_sig", g_poly_int_type_eq_sig.load() == 1);
        ctx.check("mstat4_poly_long_type_eq_sig", g_poly_long_type_eq_sig.load() == 1);
        ctx.check("mstat4_poly_string_type_eq_sig", g_poly_string_type_eq_sig.load() == 1);
        ctx.check("mstat4_poly_float_type_eq_sig", g_poly_float_type_eq_sig.load() == 1);
        ctx.check("mstat4_poly_double_type_eq_sig", g_poly_double_type_eq_sig.load() == 1);
        ctx.check("mstat4_poly_intint_type_eq_sig", g_poly_intint_type_eq_sig.load() == 1);
        ctx.check("mstat4_poly_intlong_type_eq_sig", g_poly_intlong_type_eq_sig.load() == 1);

        // ---- (AD) sEchoLong more boundaries ----
        ctx.check("mstat4_echo_long_one", g_echo_long_one.load() == 1);
        ctx.check("mstat4_echo_long_negone", g_echo_long_negone.load() == -1);
        ctx.check("mstat4_echo_long_alt_pattern",
                  g_echo_long_alt.load() == static_cast<std::int64_t>(0x5555555555555555LL));

        // ---- (AE) sEchoChar more code-unit boundaries (echo is exact) ----
        ctx.check("mstat4_echo_char_one", g_echo_char_one.load() == 1);
        // 0xD800 is a lone-surrogate code unit; a char echo preserves it verbatim
        // (zero-extended unsigned 16-bit, no interpretation as a code point).
        ctx.check("mstat4_echo_char_surrogate_exact", g_echo_char_surrogate.load() == 0xD800);

        // ---- (AF) sEchoByte / sEchoShort interior boundaries (1, -1) ----
        ctx.check("mstat4_echo_byte_one", g_echo_byte_one.load() == 1);
        ctx.check("mstat4_echo_byte_negone", g_echo_byte_negone.load() == -1);
        ctx.check("mstat4_echo_short_one", g_echo_short_one.load() == 1);
        ctx.check("mstat4_echo_short_negone", g_echo_short_negone.load() == -1);
    }

    // Suite-safety: tear down any hook this module armed so ZERO stay installed
    // for later modules.  Idempotent + safe when nothing is armed; mirrors
    // method_overload / collection_list.  The scoped_hook above already uninstalls
    // on scope exit on the normal path — this is the belt-and-braces guarantee
    // for the no-SEH Windows recovery path (a contained crash longjmps past C++
    // destructors).
    if (ctx.reset)
    {
        ctx.reset();
    }
}
