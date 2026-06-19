// method_static_portability — exhaustive JVM tests for the PORTABILITY of the
// STATIC-method CALL path:
//
//     static_method("name")->call(args)            // portable on every compiler
//     static_method("name","sig")->call(args)      // explicit-overload variant
//
// across EVERY return type (void Z B C S I J F D String object) and EVERY
// argument shape (no-arg, primitive, String, object, multi-arg incl. the
// long+double TWO-SLOT boundary), driven on a LIVE JVM through genuine bytecode
// dispatch.  This is the no-receiver dispatch path: call_stub fast path
// (StubRoutines::_call_stub_entry, JDK 8-20) or the CallStatic<T>MethodA call_jni
// fallback (JDK 21+).  Both must produce the identical converted value_t.
//
// Feature lives in vmhook/ext/vmhook/vmhook.hpp:
//   * object<T>::static_method(name)               (portable factory)  : 14465-14469
//   * object<T>::static_method(name, signature)    (portable factory)  : 14474-14478
//   * object_base::get_method(type_index,name)        (static path)    : 14173-14209
//   * object_base::get_method(type_index,name,sig)    (static path)    : 14226-14269
//   * resolve_compatible_method() static klass via _pool_holder        : 13710-13733
//   * method_proxy::is_static() (reads JVM_ACC_STATIC live)            : 13352-13363
//   * method_proxy::get_compressed_oop() (receiver OOP, 0 for static)  : 13397-13407
//   * method_proxy::call()  (call_stub fast path + decode)             : 13095-13313
//   * method_proxy::call_jni()  (CallStatic<T>MethodA slots 116..143)  : 12489-12695
//   * value_t::as_string()/is_string()/is_void()                      : 12410-12455
//
// WHAT THIS MODULE PROVES (the static-portability contract), each as ctx.check():
//   1. Every RETURN TYPE decodes to the EXACT Java value via the portable
//      static_method(name)->call() — void + Z B C S I J F D at boundary values,
//      String (ASCII/UTF-8/empty/null) via as_string(), and an object reference
//      (null -> null unique_ptr on every path; full usability is call-path
//      dependent, recorded as INFO mirroring method_call_object).
//   2. Every ARGUMENT SHAPE round-trips with NO RECEIVER: no-arg, primitive
//      (I/J/D — the two-slot kinds included), String, object, and a 4-arg frame
//      mixing int+long+double+int plus a (long,double) two-slot pair.  Static
//      recorders stamp the args they actually saw; correct values prove slot-0
//      alignment (a phantom `this` would shift every arg by a slot).
//   3. static_method(name, signature) (the explicit-overload portable factory)
//      dispatches the pinned overload verbatim for representative return types.
//   4. OVERLOAD RESOLUTION on the portable static path picks the arg-MATCHING
//      overload (the recently-FIXED crash: a primitive blasted into a reference
//      slot used to AV the JVM; resolve_compatible_method() now derives the
//      static klass from the Method's ConstantPool _pool_holder and re-picks).
//      sOver(int|long|double|String|object) each return a DISTINCT sentinel.
//   5. is_static()==TRUE for every static method resolved via static_method(...)
//      and ==FALSE for instance methods resolved via the portable get_method(...)
//      receiver path, and the proxy's receiver OOP is 0 for static methods.
//   6. SAFE no-overload-match characterization: sNum has int/long/double
//      overloads only; calling with a C++ float (descriptor F) matches none.  The
//      guard that once refused such calls was removed (signature_matches_arguments
//      false-negatives on object args), so this falls back to the first-by-name
//      overload — a PRIMITIVE slot, never a reference-slot AV.  Recorded as INFO,
//      never asserted to a specific value, and chosen so it CANNOT crash.
//
// Everything runs inside ONE detour on trigger(int) — the only context where the
// current JavaThread is set so method_proxy::call() may dispatch.  No hooks are
// left armed (scoped_hook RAII); the JVM is never shut down.
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
    // Wrapper for vmhook.fixtures.MethodStaticCall.  ALL static helpers go
    // through the portable static_method(...) factory (NOT deducing-this
    // get_field/get_method) so the call sites compile + dispatch identically on
    // MSVC, Clang and GCC; every call() is made from inside the trigger() detour.
    class msc : public vmhook::object<msc>
    {
    public:
        explicit msc(vmhook::oop_t instance) noexcept
            : vmhook::object<msc>{ instance }
        {
        }

        // -- handshake / observable static fields (portable static_field) --
        static auto set_go(bool v) -> void                      { static_field("go")->set(v); }
        static auto get_done() -> bool                          { return static_field("done")->get(); }
        static auto get_static_recorder_hits() -> std::int32_t  { return static_field("staticRecorderHits")->get(); }
        static auto get_recorded_long() -> std::int64_t         { return static_field("recordedLongArg")->get(); }
        // Java double fields: read as `double` (NOT int64 — the value_t->int64
        // conversion numerically TRUNCATES a double; only the recorded values
        // here are finite + exactly representable so a direct double compare is
        // exact).
        static auto get_recorded_double() -> double             { return static_field("recordedDoubleArg")->get(); }
        static auto get_recorded_first() -> std::int32_t        { return static_field("recordedFirstOfFour")->get(); }
        static auto get_recorded_second() -> std::int64_t       { return static_field("recordedSecondOfFour")->get(); }
        static auto get_recorded_third() -> double              { return static_field("recordedThirdOfFour")->get(); }
        static auto get_recorded_fourth() -> std::int32_t       { return static_field("recordedFourthOfFour")->get(); }
        static auto get_recorded_two_slot_long() -> std::int64_t{ return static_field("recordedTwoSlotLong")->get(); }
        static auto get_recorded_two_slot_double() -> double    { return static_field("recordedTwoSlotDouble")->get(); }
        static auto get_recorded_obj_seed() -> std::int32_t     { return static_field("recordedObjSeed")->get(); }
        static auto get_recorded_bool() -> bool                 { return static_field("recordedBoolArg")->get(); }
        static auto get_recorded_float() -> float               { return static_field("recordedFloatArg")->get(); }

        // Reads this instance's own seed (proves a returned wrapper is usable).
        auto seed() const -> std::int32_t { return get_field("seed")->get(); }
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
    constexpr std::int32_t k_static_child_seed = 7373;

    // Fixture-mirrored overload sentinels (kept in lockstep with the Java file).
    constexpr std::int32_t OVER_INT    = 5001;
    constexpr std::int32_t OVER_LONG   = 5002;
    constexpr std::int32_t OVER_DOUBLE = 5003;
    constexpr std::int32_t OVER_STRING = 5004;
    constexpr std::int32_t OVER_OBJECT = 5005;
    constexpr std::int32_t NUM_INT     = 6001;
    constexpr std::int32_t NUM_LONG    = 6002;
    constexpr std::int32_t NUM_DOUBLE  = 6003;

    // ------------------------------------------------------------------
    //  Captured observations.  The detour writes; the module body reads.
    // ------------------------------------------------------------------
    std::atomic<int>  g_detour_calls{ 0 };
    std::atomic<bool> g_detour_saw_self{ false };
    std::atomic<bool> g_all_calls_ran{ false };

    // -- (1) primitive returns via static_method(name)->call() --
    std::atomic<int>  g_void_is_void{ -1 };
    std::atomic<int>  g_bool_true{ -1 };
    std::atomic<int>  g_bool_false{ -1 };
    std::atomic<std::int64_t> g_byte_max{ k_uncaptured };
    std::atomic<std::int64_t> g_byte_min{ k_uncaptured };
    std::atomic<std::int64_t> g_byte_negone{ k_uncaptured };
    std::atomic<std::int64_t> g_byte_negone_as_int{ k_uncaptured }; // sign-extension
    std::atomic<std::int64_t> g_char_a{ k_uncaptured };
    std::atomic<std::int64_t> g_char_max{ k_uncaptured };
    std::atomic<std::int64_t> g_char_max_as_int{ k_uncaptured };    // zero-extension
    std::atomic<std::int64_t> g_short_max{ k_uncaptured };
    std::atomic<std::int64_t> g_short_min{ k_uncaptured };
    std::atomic<std::int64_t> g_short_negone{ k_uncaptured };
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

    // -- (1) String returns --
    std::atomic<bool> g_str_captured{ false };
    std::string       g_str_hello;          // guarded by g_str_captured
    std::string       g_str_unicode;
    std::string       g_str_empty;
    std::string       g_str_null;
    std::atomic<int>  g_str_hello_is_string{ -1 };

    // -- (1) object returns --
    std::atomic<bool> g_obj_null_is_null_uptr{ false };   // sNullChild -> null uptr (hard)
    std::atomic<bool> g_obj_child_nonnull{ false };       // sMakeChild non-null (path-dep)
    std::atomic<std::int64_t> g_obj_child_seed{ k_uncaptured };
    std::atomic<int>  g_obj_child_is_string{ -1 };        // non-String object: is_string()?

    // -- (2) argument shapes / no receiver --
    std::atomic<std::int64_t> g_echo_int{ k_uncaptured };       // sEchoInt(v)->v
    std::atomic<std::int64_t> g_echo_long{ k_uncaptured };      // sEchoLong(v)->v (two-slot)
    std::atomic<std::uint64_t> g_echo_double_bits{ 0 };         // sEchoDouble(v)->v (two-slot)
    std::atomic<bool> g_echo_double_captured{ false };
    std::atomic<bool> g_echo_string_ok{ false };                // sEchoString("x")=="x"
    std::atomic<int>  g_echo_bool_ret{ -1 };                    // sEchoBool(true)->1
    std::atomic<int>  g_echo_bool_field{ -1 };                  // recordedBoolArg after the call
    std::atomic<std::uint32_t> g_echo_float_ret_bits{ 0 };      // sEchoFloat(0.5f) return bits
    std::atomic<std::uint32_t> g_echo_float_field_bits{ 0 };    // recordedFloatArg bits
    std::atomic<bool> g_echo_bool_float_captured{ false };
    std::atomic<std::int64_t> g_zero_arg_long{ k_uncaptured };  // no-arg long returner
    std::atomic<std::int64_t> g_record_long_ret{ k_uncaptured };
    std::atomic<std::uint64_t> g_record_double_ret_bits{ 0 };
    std::atomic<bool> g_record_double_ret_captured{ false };
    std::atomic<std::int64_t> g_record_four_ret{ k_uncaptured };
    std::atomic<std::uint64_t> g_long_double_ret_bits{ 0 };     // sLongDouble(l,d) return
    std::atomic<bool> g_long_double_ret_captured{ false };
    std::atomic<std::int64_t> g_obj_arg_seed_ret{ k_uncaptured };// sReadObjSeed(child)
    std::atomic<std::int64_t> g_obj_arg_null_ret{ k_uncaptured };// sReadObjSeed(null)
    std::atomic<int>  g_recv_oop_zero_echo{ -1 };               // proxy.get_compressed_oop()==0
    std::atomic<int>  g_recv_oop_zero_void{ -1 };

    // -- (3) static_method(name, signature) explicit-overload portable path --
    std::atomic<std::int64_t> g_sig_int_ret{ k_uncaptured };    // ("sIntFortyTwo","()I")
    std::atomic<std::int64_t> g_sig_echo_ret{ k_uncaptured };   // ("sEchoInt","(I)I")
    std::atomic<std::int64_t> g_sig_long_ret{ k_uncaptured };   // ("sLongBig","()J")
    std::atomic<std::uint64_t> g_sig_double_ret_bits{ 0 };      // ("sDoublePi","()D")
    std::atomic<bool> g_sig_double_captured{ false };
    std::string       g_sig_string_ret;                          // ("sStringHello","()Ljava/lang/String;")
    std::atomic<bool> g_sig_string_captured{ false };
    std::atomic<int>  g_sig_is_static{ -1 };

    // -- (4) overload resolution on the portable static path (the fixed crash) --
    std::atomic<std::int64_t> g_over_int{ k_uncaptured };
    std::atomic<std::int64_t> g_over_long{ k_uncaptured };
    std::atomic<std::int64_t> g_over_double{ k_uncaptured };
    std::atomic<std::int64_t> g_over_string{ k_uncaptured };
    std::atomic<std::int64_t> g_over_object{ k_uncaptured };
    // explicit-signature disambiguation of the SAME overloaded name.
    std::atomic<std::int64_t> g_over_sig_int{ k_uncaptured };
    std::atomic<std::int64_t> g_over_sig_string{ k_uncaptured };

    // -- (5) is_static() accessor + receiver OOP --
    std::atomic<int>  g_isstatic_void{ -1 };
    std::atomic<int>  g_isstatic_int{ -1 };
    std::atomic<int>  g_isstatic_long{ -1 };
    std::atomic<int>  g_isstatic_double{ -1 };
    std::atomic<int>  g_isstatic_string{ -1 };
    std::atomic<int>  g_isstatic_object{ -1 };
    std::atomic<int>  g_isstatic_echo{ -1 };
    std::atomic<int>  g_isstatic_inst_getseed{ -1 }; // instance via get_method -> false
    std::atomic<int>  g_isstatic_inst_echo{ -1 };

    // -- (6) SAFE "no overload matches" characterization (recorded, not asserted) --
    std::atomic<std::int64_t> g_num_int{ k_uncaptured };     // sNum(int)   -> NUM_INT
    std::atomic<std::int64_t> g_num_long{ k_uncaptured };    // sNum(long)  -> NUM_LONG
    std::atomic<std::int64_t> g_num_double{ k_uncaptured };  // sNum(double)-> NUM_DOUBLE
    std::atomic<std::int64_t> g_num_float_nomatch{ k_uncaptured }; // sNum(float): no (F) overload
    std::atomic<int>  g_num_float_did_not_crash{ -1 };       // reached the line after the call

    auto run_all_calls(const std::unique_ptr<msc>& self) -> void
    {
        // ============================== (1) RETURN TYPES =======================
        // void: is_void()==true; the body bumps staticRecorderHits.
        g_void_is_void.store(msc::static_method("sVoid")->call().is_void() ? 1 : 0);

        g_bool_true.store(msc::static_method("sBoolTrue")->call() ? 1 : 0);
        g_bool_false.store(msc::static_method("sBoolFalse")->call() ? 1 : 0);

        g_byte_max.store(static_cast<std::int8_t>(msc::static_method("sByteMax")->call()));
        g_byte_min.store(static_cast<std::int8_t>(msc::static_method("sByteMin")->call()));
        g_byte_negone.store(static_cast<std::int8_t>(msc::static_method("sByteNegOne")->call()));
        {
            const std::int32_t as_int = msc::static_method("sByteNegOne")->call();
            g_byte_negone_as_int.store(as_int);
        }

        g_char_a.store(static_cast<std::uint16_t>(msc::static_method("sCharA")->call()));
        g_char_max.store(static_cast<std::uint16_t>(msc::static_method("sCharMax")->call()));
        {
            const std::int32_t as_int = msc::static_method("sCharMax")->call();
            g_char_max_as_int.store(as_int);
        }

        g_short_max.store(static_cast<std::int16_t>(msc::static_method("sShortMax")->call()));
        g_short_min.store(static_cast<std::int16_t>(msc::static_method("sShortMin")->call()));
        g_short_negone.store(static_cast<std::int16_t>(msc::static_method("sShortNegOne")->call()));

        g_int_max.store(static_cast<std::int32_t>(msc::static_method("sIntMax")->call()));
        g_int_min.store(static_cast<std::int32_t>(msc::static_method("sIntMin")->call()));
        g_int_42.store(static_cast<std::int32_t>(msc::static_method("sIntFortyTwo")->call()));
        g_int_negone.store(static_cast<std::int32_t>(msc::static_method("sIntNegOne")->call()));

        g_long_max.store(static_cast<std::int64_t>(msc::static_method("sLongMax")->call()));
        g_long_min.store(static_cast<std::int64_t>(msc::static_method("sLongMin")->call()));
        g_long_big.store(static_cast<std::int64_t>(msc::static_method("sLongBig")->call()));

        g_float_half.store(f2bits(static_cast<float>(msc::static_method("sFloatHalf")->call())));
        g_float_negzero.store(f2bits(static_cast<float>(msc::static_method("sFloatNegZero")->call())));
        g_float_nan.store(f2bits(static_cast<float>(msc::static_method("sFloatNaN")->call())));
        g_float_posinf.store(f2bits(static_cast<float>(msc::static_method("sFloatPosInf")->call())));
        g_float_max.store(f2bits(static_cast<float>(msc::static_method("sFloatMax")->call())));

        g_double_pi.store(d2bits(static_cast<double>(msc::static_method("sDoublePi")->call())));
        g_double_negzero.store(d2bits(static_cast<double>(msc::static_method("sDoubleNegZero")->call())));
        g_double_nan.store(d2bits(static_cast<double>(msc::static_method("sDoubleNaN")->call())));
        g_double_neginf.store(d2bits(static_cast<double>(msc::static_method("sDoubleNegInf")->call())));
        g_double_max.store(d2bits(static_cast<double>(msc::static_method("sDoubleMax")->call())));
        g_fp_captured.store(true);

        // String returns (MSVC copy-init from ->call()).
        {
            const auto v_hello = msc::static_method("sStringHello")->call();
            g_str_hello = v_hello.as_string();
            g_str_hello_is_string.store(v_hello.is_string() ? 1 : 0);

            g_str_unicode = msc::static_method("sStringUnicode")->call().as_string();
            g_str_empty   = msc::static_method("sStringEmpty")->call().as_string();
            g_str_null    = msc::static_method("sStringNull")->call().as_string();
            g_str_captured.store(true);
        }

        // Object returns.  Null -> null unique_ptr is the hard invariant.
        {
            std::unique_ptr<msc> null_child = msc::static_method("sNullChild")->call();
            g_obj_null_is_null_uptr.store(null_child == nullptr, std::memory_order_relaxed);

            std::unique_ptr<msc> child = msc::static_method("sMakeChild")->call();
            const bool child_nonnull{ child != nullptr };
            g_obj_child_nonnull.store(child_nonnull, std::memory_order_relaxed);
            if (child_nonnull)
            {
                g_obj_child_seed.store(child->seed(), std::memory_order_relaxed);
            }

            const auto v_obj = msc::static_method("sMakeChild")->call();
            g_obj_child_is_string.store(v_obj.is_string() ? 1 : 0);
        }

        // ============================== (2) ARGUMENT SHAPES ====================
        // No-arg (already exercised above for returns); a no-arg long here too.
        g_zero_arg_long.store(static_cast<std::int64_t>(
            msc::static_method("sZeroArgLong")->call()));

        // Primitive args: int, long (two-slot), double (two-slot).
        g_echo_int.store(static_cast<std::int32_t>(
            msc::static_method("sEchoInt")->call(std::int32_t{ 13572468 })));
        g_echo_long.store(static_cast<std::int64_t>(
            msc::static_method("sEchoLong")->call(std::int64_t{ 0x0011223344556677LL })));
        {
            const double d = msc::static_method("sEchoDouble")->call(double{ 2.718281828459045 });
            g_echo_double_bits.store(d2bits(d));
            g_echo_double_captured.store(true);
        }

        // String arg shape (reference parameter).
        {
            const auto v = msc::static_method("sEchoString")->call(std::string{ "echo-arg-café" });
            g_echo_string_ok.store(v.as_string() == "echo-arg-caf\xC3\xA9");
        }

        // Bool arg shape (one slot; the JVM writes the boolean as a full int 0/1)
        // and float arg shape (one slot; IEEE-exact bits, not value-after-widening).
        // Both echo back AND stamp a recorder field so slot-0 alignment is proven
        // with no receiver in front.  0.5f is exactly representable so even a
        // float->double->float trip would be exact, but we assert raw bits anyway.
        {
            g_echo_bool_ret.store(msc::static_method("sEchoBool")->call(true) ? 1 : 0);
            g_echo_bool_field.store(msc::get_recorded_bool() ? 1 : 0);

            const float fin{ 0.5f };
            const float fout{ static_cast<float>(msc::static_method("sEchoFloat")->call(fin)) };
            g_echo_float_ret_bits.store(f2bits(fout));
            g_echo_float_field_bits.store(f2bits(msc::get_recorded_float()));
            g_echo_bool_float_captured.store(true);
        }

        // Object arg shape: pass a unique_ptr<wrapper> through to a static method.
        {
            std::unique_ptr<msc> child = msc::static_method("sMakeChild")->call();
            if (child)
            {
                g_obj_arg_seed_ret.store(static_cast<std::int32_t>(
                    msc::static_method("sReadObjSeed")->call(std::move(child))));
            }
            // Null object arg -> -1 sentinel (and must not crash).
            std::unique_ptr<msc> none{};
            g_obj_arg_null_ret.store(static_cast<std::int32_t>(
                msc::static_method("sReadObjSeed")->call(std::move(none))));
        }

        // Recorder round-trips (return + side-effect both checked in the body).
        g_record_long_ret.store(static_cast<std::int64_t>(
            msc::static_method("sRecordLong")->call(std::int64_t{ 0x7EEDFACE0BADBEEFLL })));
        {
            const double d = msc::static_method("sRecordDouble")->call(double{ 1.5 });
            g_record_double_ret_bits.store(d2bits(d));
            g_record_double_ret_captured.store(true);
        }

        // Multi-arg frame: int slot0, long slots1-2, double slots3-4, int slot5.
        g_record_four_ret.store(static_cast<std::int64_t>(
            msc::static_method("sRecordFour")->call(
                std::int32_t{ 5 },
                std::int64_t{ 0x1122334455667788LL },
                double{ 4.0 },
                std::int32_t{ -13 })));

        // long+double two-slot boundary in isolation.
        {
            const double d = msc::static_method("sLongDouble")->call(
                std::int64_t{ 0x0102030405060708LL }, double{ 9.0 });
            g_long_double_ret_bits.store(d2bits(d));
            g_long_double_ret_captured.store(true);
        }

        // Receiver OOP must be 0 for a static proxy.
        {
            auto p_echo = msc::static_method("sEchoInt");
            g_recv_oop_zero_echo.store((p_echo && p_echo->get_compressed_oop() == 0u) ? 1 : 0);
            auto p_void = msc::static_method("sVoid");
            g_recv_oop_zero_void.store((p_void && p_void->get_compressed_oop() == 0u) ? 1 : 0);
        }

        // ===================== (3) static_method(name, signature) ==============
        {
            auto p_int = msc::static_method("sIntFortyTwo", "()I");
            if (p_int)
            {
                g_sig_int_ret.store(static_cast<std::int32_t>(p_int->call()));
                g_sig_is_static.store(p_int->is_static() ? 1 : 0);
            }
            if (auto p_echo = msc::static_method("sEchoInt", "(I)I"))
            {
                g_sig_echo_ret.store(static_cast<std::int32_t>(p_echo->call(std::int32_t{ 24681357 })));
            }
            if (auto p_long = msc::static_method("sLongBig", "()J"))
            {
                g_sig_long_ret.store(static_cast<std::int64_t>(p_long->call()));
            }
            if (auto p_double = msc::static_method("sDoublePi", "()D"))
            {
                const double d = p_double->call();
                g_sig_double_ret_bits.store(d2bits(d));
                g_sig_double_captured.store(true);
            }
            if (auto p_str = msc::static_method("sStringHello", "()Ljava/lang/String;"))
            {
                g_sig_string_ret = p_str->call().as_string();
                g_sig_string_captured.store(true);
            }
        }

        // ===================== (4) OVERLOAD RESOLUTION (fixed crash) ===========
        // Name-only static_method("sOver"): resolve_compatible_method must re-pick
        // the overload matching each C++ arg TYPE.  A mis-resolution that put a
        // primitive into the (String)/(object) slot used to AV the JVM.  Distinct
        // sentinels prove WHICH overload ran.
        g_over_int.store(static_cast<std::int32_t>(
            msc::static_method("sOver")->call(std::int32_t{ 7 })));
        g_over_long.store(static_cast<std::int32_t>(
            msc::static_method("sOver")->call(std::int64_t{ 7 })));
        g_over_double.store(static_cast<std::int32_t>(
            msc::static_method("sOver")->call(double{ 7.0 })));
        g_over_string.store(static_cast<std::int32_t>(
            msc::static_method("sOver")->call(std::string{ "seven" })));
        {
            std::unique_ptr<msc> child = msc::static_method("sMakeChild")->call();
            if (child)
            {
                g_over_object.store(static_cast<std::int32_t>(
                    msc::static_method("sOver")->call(std::move(child))));
            }
        }
        // Explicit-signature disambiguation of the same overloaded name.
        if (auto p = msc::static_method("sOver", "(I)I"))
        {
            g_over_sig_int.store(static_cast<std::int32_t>(p->call(std::int32_t{ 1 })));
        }
        if (auto p = msc::static_method("sOver", "(Ljava/lang/String;)I"))
        {
            g_over_sig_string.store(static_cast<std::int32_t>(p->call(std::string{ "x" })));
        }

        // ===================== (5) is_static() + receiver OOP ==================
        g_isstatic_void.store(msc::static_method("sVoid")->is_static() ? 1 : 0);
        g_isstatic_int.store(msc::static_method("sIntFortyTwo")->is_static() ? 1 : 0);
        g_isstatic_long.store(msc::static_method("sLongBig")->is_static() ? 1 : 0);
        g_isstatic_double.store(msc::static_method("sDoublePi")->is_static() ? 1 : 0);
        g_isstatic_string.store(msc::static_method("sStringHello")->is_static() ? 1 : 0);
        g_isstatic_object.store(msc::static_method("sMakeChild")->is_static() ? 1 : 0);
        g_isstatic_echo.store(msc::static_method("sEchoInt")->is_static() ? 1 : 0);
        if (self)
        {
            g_isstatic_inst_getseed.store(self->get_method("iGetSeed")->is_static() ? 1 : 0);
            g_isstatic_inst_echo.store(self->get_method("iEcho")->is_static() ? 1 : 0);
        }

        // ===================== (6) SAFE no-overload-match characterization =====
        // sNum has int/long/double overloads ONLY.  The first three calls match
        // exactly; the float call (descriptor F) matches NONE and falls back to
        // the first-by-name overload — but every sNum overload is a PRIMITIVE
        // slot, so this can never trigger the reference-slot AV.  We record what
        // came back and prove the line AFTER the call was reached (no crash).
        g_num_int.store(static_cast<std::int32_t>(
            msc::static_method("sNum")->call(std::int32_t{ 1 })));
        g_num_long.store(static_cast<std::int32_t>(
            msc::static_method("sNum")->call(std::int64_t{ 2 })));
        g_num_double.store(static_cast<std::int32_t>(
            msc::static_method("sNum")->call(double{ 3.0 })));
        {
            const auto v = msc::static_method("sNum")->call(float{ 4.5f });
            // Whatever value_t came back (monostate or a primitive sentinel),
            // reaching here proves the no-match path did not tear down the JVM.
            g_num_float_nomatch.store(v.is_void() ? k_uncaptured : static_cast<std::int64_t>(static_cast<std::int32_t>(v)));
            g_num_float_did_not_crash.store(1);
        }

        (void)self;
        g_all_calls_ran.store(true);
    }
}

VMHOOK_JVM_MODULE(method_static_portability)
{
    vmhook::register_class<msc>("vmhook/fixtures/MethodStaticCall");

    // Record which dispatch path the live JDK uses.  Primitives + String are
    // path-independent; object-return usability is path-dependent (call_jni
    // truncates the handle on JDK 21+, a documented flaw this module records).
    const bool call_stub_present{ vmhook::detail::find_call_stub_entry() != nullptr };
    ctx.record(std::string{ "[INFO] method_static_portability dispatch path: " }
               + (call_stub_present ? "call_stub fast path (object returns are real compressed OOPs)"
                                    : "call_jni fallback (CallStatic<T>MethodA; object returns TRUNCATED/freed — KNOWN flaw)"));

    {
        auto handle{ vmhook::scoped_hook<msc>(
            "trigger",
            [](vmhook::return_value&,
               const std::unique_ptr<msc>& self,
               std::int32_t /*delta*/)
            {
                g_detour_calls.fetch_add(1, std::memory_order_relaxed);
                g_detour_saw_self.store(self != nullptr, std::memory_order_relaxed);
                run_all_calls(self);
            }) };
        ctx.check("msp_hook_installed", handle.installed());

        const bool done{ ctx.run_probe(
            [](bool v) { msc::set_go(v); },
            []() { return msc::get_done(); }) };

        ctx.check("msp_probe_completed", done);
        ctx.check("msp_detour_fired", g_detour_calls.load(std::memory_order_relaxed) >= 1);
        ctx.check("msp_detour_saw_self", g_detour_saw_self.load(std::memory_order_relaxed));
        ctx.check("msp_all_calls_ran", g_all_calls_ran.load(std::memory_order_relaxed));

        // ==================================================================
        //  (1) RETURN TYPES — exact value per type + boundaries, via the
        //      portable static_method(name)->call().
        // ==================================================================
        ctx.check("msp_void_is_void", g_void_is_void.load() == 1);
        ctx.check("msp_void_side_effect_ran", msc::get_static_recorder_hits() > 0);

        ctx.check("msp_bool_true", g_bool_true.load() == 1);
        ctx.check("msp_bool_false", g_bool_false.load() == 0);

        ctx.check("msp_byte_max_127", g_byte_max.load() == 127);
        ctx.check("msp_byte_min_neg128", g_byte_min.load() == -128);
        ctx.check("msp_byte_negone", g_byte_negone.load() == -1);
        ctx.check("msp_byte_negone_sign_extends_to_int", g_byte_negone_as_int.load() == -1);

        ctx.check("msp_char_A_65", g_char_a.load() == 65);
        ctx.check("msp_char_max_65535", g_char_max.load() == 65535);
        ctx.check("msp_char_max_zero_extends_to_int_65535", g_char_max_as_int.load() == 65535);

        ctx.check("msp_short_max_32767", g_short_max.load() == 32767);
        ctx.check("msp_short_min_neg32768", g_short_min.load() == -32768);
        ctx.check("msp_short_negone", g_short_negone.load() == -1);

        ctx.check("msp_int_max_2147483647", g_int_max.load() == 2147483647LL);
        ctx.check("msp_int_min_neg2147483648", g_int_min.load() == -2147483648LL);
        ctx.check("msp_int_42", g_int_42.load() == 42);
        ctx.check("msp_int_negone", g_int_negone.load() == -1);

        ctx.check("msp_long_max", g_long_max.load() == std::numeric_limits<std::int64_t>::max());
        ctx.check("msp_long_min", g_long_min.load() == std::numeric_limits<std::int64_t>::min());
        ctx.check("msp_long_big_pattern", g_long_big.load() == static_cast<std::int64_t>(0x0123456789ABCDEFLL));

        ctx.check("msp_fp_captured", g_fp_captured.load());
        ctx.check("msp_float_half", bits2f(g_float_half.load()) == 0.5f);
        {
            const float nz = bits2f(g_float_negzero.load());
            ctx.check("msp_float_negzero_value", nz == 0.0f);
            ctx.check("msp_float_negzero_signbit", std::signbit(nz));
        }
        ctx.check("msp_float_nan_isnan", std::isnan(bits2f(g_float_nan.load())));
        {
            const float pinf = bits2f(g_float_posinf.load());
            ctx.check("msp_float_posinf_isinf", std::isinf(pinf) && pinf > 0.0f);
        }
        ctx.check("msp_float_max", bits2f(g_float_max.load()) == std::numeric_limits<float>::max());

        ctx.check("msp_double_pi_bits", g_double_pi.load() == d2bits(3.141592653589793));
        {
            const double nz = bits2d(g_double_negzero.load());
            ctx.check("msp_double_negzero_value", nz == 0.0);
            ctx.check("msp_double_negzero_signbit", std::signbit(nz));
        }
        ctx.check("msp_double_nan_isnan", std::isnan(bits2d(g_double_nan.load())));
        {
            const double ninf = bits2d(g_double_neginf.load());
            ctx.check("msp_double_neginf_isinf", std::isinf(ninf) && ninf < 0.0);
        }
        ctx.check("msp_double_max", bits2d(g_double_max.load()) == std::numeric_limits<double>::max());

        // String returns.
        ctx.check("msp_str_captured", g_str_captured.load());
        ctx.check("msp_string_hello_exact", g_str_hello == "hello-portable");
        ctx.check("msp_string_hello_is_string", g_str_hello_is_string.load() == 1);
        ctx.check("msp_string_unicode_exact", g_str_unicode == "caf\xC3\xA9");
        ctx.check("msp_string_unicode_not_empty", !g_str_unicode.empty());
        ctx.check("msp_string_empty_is_empty", g_str_empty.empty());
        // A null String return decodes to "" via as_string() and never crashes
        // (path-independent headline guarantee).
        ctx.check("msp_string_null_as_string_empty", g_str_null.empty());

        // Object returns: null -> null uptr is the hard invariant on every path.
        ctx.check("msp_object_null_returns_null_unique_ptr",
                  g_obj_null_is_null_uptr.load(std::memory_order_relaxed));
        // Path-independent value_t routing: a non-String object is never is_string().
        ctx.check("msp_object_return_not_string", g_obj_child_is_string.load() == 0);
        if (call_stub_present)
        {
            ctx.check("msp_object_make_child_non_null",
                      g_obj_child_nonnull.load(std::memory_order_relaxed));
            ctx.check("msp_object_make_child_seed_through_wrapper",
                      g_obj_child_seed.load(std::memory_order_relaxed) == k_static_child_seed);
        }
        else
        {
            ctx.record(std::string{ "[INFO] msp_object_make_child_non_null (call_jni) = " }
                       + (g_obj_child_nonnull.load(std::memory_order_relaxed) ? "true" : "false"));
            ctx.record(std::string{ "[INFO] msp_object_make_child_seed (call_jni) = " }
                       + std::to_string(g_obj_child_seed.load(std::memory_order_relaxed))
                       + " (expected " + std::to_string(k_static_child_seed) + ")");
        }

        // ==================================================================
        //  (2) ARGUMENT SHAPES — no receiver, every kind, two-slot boundary.
        // ==================================================================
        ctx.check("msp_no_arg_long_returner", g_zero_arg_long.load() == static_cast<std::int64_t>(0x7766554433221100LL));

        ctx.check("msp_arg_echo_int_returns_arg", g_echo_int.load() == 13572468);
        ctx.check("msp_arg_echo_long_two_slot_returns_arg",
                  g_echo_long.load() == static_cast<std::int64_t>(0x0011223344556677LL));
        ctx.check("msp_arg_echo_double_captured", g_echo_double_captured.load());
        ctx.check("msp_arg_echo_double_two_slot_returns_arg",
                  g_echo_double_bits.load() == d2bits(2.718281828459045));
        ctx.check("msp_arg_echo_string_returns_arg", g_echo_string_ok.load());

        // Bool + float arg shapes: return value AND the recorded field round-trip
        // exactly (one interpreter slot each, slot-0 aligned with no receiver).
        ctx.check("msp_arg_echo_bool_float_captured", g_echo_bool_float_captured.load());
        ctx.check("msp_arg_echo_bool_returns_arg", g_echo_bool_ret.load() == 1);
        ctx.check("msp_arg_echo_bool_recorded_field", g_echo_bool_field.load() == 1);
        ctx.check("msp_arg_echo_float_returns_arg_bits", g_echo_float_ret_bits.load() == f2bits(0.5f));
        ctx.check("msp_arg_echo_float_recorded_field", g_echo_float_field_bits.load() == f2bits(0.5f));

        // Recorder returns AND side-effect fields (slot-0 alignment, no receiver).
        ctx.check("msp_record_long_return",
                  g_record_long_ret.load() == static_cast<std::int64_t>(0x7EEDFACE0BADBEEFLL));
        ctx.check("msp_record_long_field",
                  msc::get_recorded_long() == static_cast<std::int64_t>(0x7EEDFACE0BADBEEFLL));
        ctx.check("msp_record_double_captured", g_record_double_ret_captured.load());
        ctx.check("msp_record_double_return", bits2d(g_record_double_ret_bits.load()) == 1.5);
        ctx.check("msp_record_double_field", msc::get_recorded_double() == 1.5);

        // 4-arg frame: int slot0, long slots1-2, double slots3-4, int slot5.
        ctx.check("msp_record_four_return",
                  g_record_four_ret.load()
                      == static_cast<std::int64_t>(5) + 0x1122334455667788LL + static_cast<std::int64_t>(4) + (-13));
        ctx.check("msp_record_four_first_int_slot0", msc::get_recorded_first() == 5);
        ctx.check("msp_record_four_long_slot1", msc::get_recorded_second() == 0x1122334455667788LL);
        ctx.check("msp_record_four_double_slot3", msc::get_recorded_third() == 4.0);
        ctx.check("msp_record_four_trailing_int_slot5", msc::get_recorded_fourth() == -13);

        // long+double two-slot pair in isolation.
        ctx.check("msp_long_double_ret_captured", g_long_double_ret_captured.load());
        ctx.check("msp_long_double_return",
                  bits2d(g_long_double_ret_bits.load()) == static_cast<double>(0x0102030405060708LL) + 9.0);
        ctx.check("msp_long_double_long_field", msc::get_recorded_two_slot_long() == 0x0102030405060708LL);
        ctx.check("msp_long_double_double_field", msc::get_recorded_two_slot_double() == 9.0);

        // Object arg shape: child seed read back through the RETURN value (the
        // object reference reached parameter slot 0, no receiver in front of it).
        // The non-null seed proof is path-dependent: it requires sMakeChild to
        // have produced a usable wrapper to pass, which only holds on the
        // call_stub path (call_jni truncates the returned handle).
        if (call_stub_present)
        {
            ctx.check("msp_object_arg_seed_returned", g_obj_arg_seed_ret.load() == k_static_child_seed);
        }
        else
        {
            ctx.record(std::string{ "[INFO] msp_object_arg_seed_returned (call_jni) = " }
                       + std::to_string(g_obj_arg_seed_ret.load())
                       + " (expected " + std::to_string(k_static_child_seed)
                       + "; the object arg comes from sMakeChild whose handle is truncated on call_jni)");
        }
        // A null object arg ALWAYS yields the -1 sentinel and never crashes
        // (path-independent — the last sReadObjSeed call was the null one, so the
        // recorded field also reflects -1).
        ctx.check("msp_object_arg_null_returns_neg1", g_obj_arg_null_ret.load() == -1);
        ctx.check("msp_object_arg_null_recorded_field", msc::get_recorded_obj_seed() == -1);

        // Receiver OOP is 0 for static proxies.
        ctx.check("msp_receiver_oop_zero_echo", g_recv_oop_zero_echo.load() == 1);
        ctx.check("msp_receiver_oop_zero_void", g_recv_oop_zero_void.load() == 1);

        // ==================================================================
        //  (3) static_method(name, signature) — explicit-overload portable path
        // ==================================================================
        ctx.check("msp_sig_int_return", g_sig_int_ret.load() == 42);
        ctx.check("msp_sig_is_static", g_sig_is_static.load() == 1);
        ctx.check("msp_sig_echo_return", g_sig_echo_ret.load() == 24681357);
        ctx.check("msp_sig_long_return", g_sig_long_ret.load() == static_cast<std::int64_t>(0x0123456789ABCDEFLL));
        ctx.check("msp_sig_double_captured", g_sig_double_captured.load());
        ctx.check("msp_sig_double_return", g_sig_double_ret_bits.load() == d2bits(3.141592653589793));
        ctx.check("msp_sig_string_captured", g_sig_string_captured.load());
        ctx.check("msp_sig_string_return", g_sig_string_ret == "hello-portable");

        // ==================================================================
        //  (4) OVERLOAD RESOLUTION on the portable static path (the FIXED crash)
        //      Distinct sentinels prove the arg-matching overload was picked.
        //      A wrong pick into a reference slot used to AV — a correct sentinel
        //      is a direct proof the _pool_holder static-klass derivation works.
        // ==================================================================
        ctx.check("msp_over_int_resolves_int", g_over_int.load() == OVER_INT);
        ctx.check("msp_over_long_resolves_long", g_over_long.load() == OVER_LONG);
        ctx.check("msp_over_double_resolves_double", g_over_double.load() == OVER_DOUBLE);
        ctx.check("msp_over_string_resolves_string", g_over_string.load() == OVER_STRING);
        if (call_stub_present)
        {
            // Object-arg overload resolution needs a usable wrapper to pass; on
            // call_jni the sMakeChild handle is truncated so the child may be null
            // and the object-arg call is skipped (g stays k_uncaptured).
            ctx.check("msp_over_object_resolves_object", g_over_object.load() == OVER_OBJECT);
        }
        else
        {
            ctx.record(std::string{ "[INFO] msp_over_object (call_jni) = " }
                       + std::to_string(g_over_object.load())
                       + " (expected " + std::to_string(OVER_OBJECT)
                       + "; object arg may be skipped if sMakeChild handle was truncated on call_jni)");
        }
        ctx.check("msp_over_all_primitive_sentinels_distinct",
                  g_over_int.load() != g_over_long.load()
                  && g_over_long.load() != g_over_double.load()
                  && g_over_double.load() != g_over_string.load()
                  && g_over_int.load() != g_over_double.load());
        // Explicit-signature disambiguation of the SAME overloaded name.
        ctx.check("msp_over_sig_int_exact", g_over_sig_int.load() == OVER_INT);
        ctx.check("msp_over_sig_string_exact", g_over_sig_string.load() == OVER_STRING);

        // ==================================================================
        //  (5) is_static() accessor + receiver OOP
        // ==================================================================
        ctx.check("msp_is_static_true_void", g_isstatic_void.load() == 1);
        ctx.check("msp_is_static_true_int", g_isstatic_int.load() == 1);
        ctx.check("msp_is_static_true_long", g_isstatic_long.load() == 1);
        ctx.check("msp_is_static_true_double", g_isstatic_double.load() == 1);
        ctx.check("msp_is_static_true_string", g_isstatic_string.load() == 1);
        ctx.check("msp_is_static_true_object", g_isstatic_object.load() == 1);
        ctx.check("msp_is_static_true_echo_with_arg", g_isstatic_echo.load() == 1);
        ctx.check("msp_is_static_false_instance_getseed", g_isstatic_inst_getseed.load() == 0);
        ctx.check("msp_is_static_false_instance_echo", g_isstatic_inst_echo.load() == 0);

        // ==================================================================
        //  (6) SAFE no-overload-match characterization — NEVER fails CI.
        //      sNum int/long/double resolve exactly; sNum(float) matches no
        //      overload and falls back to the first-by-name PRIMITIVE overload
        //      (cannot AV).  We assert the matching ones and the no-crash fact;
        //      the no-match VALUE is recorded only.
        // ==================================================================
        ctx.check("msp_num_int_resolves_int", g_num_int.load() == NUM_INT);
        ctx.check("msp_num_long_resolves_long", g_num_long.load() == NUM_LONG);
        ctx.check("msp_num_double_resolves_double", g_num_double.load() == NUM_DOUBLE);
        // The headline safety guarantee: a float arg matching NO sNum overload
        // did NOT tear down the JVM (the line after the call was reached).
        ctx.check("msp_num_float_no_overload_did_not_crash", g_num_float_did_not_crash.load() == 1);
        ctx.record(std::string{ "[INFO] sNum(float 4.5f) [no (F) overload; resolve falls back to first-by-name "
                                "primitive overload, guard was removed] returned " }
                   + (g_num_float_nomatch.load() == k_uncaptured
                          ? std::string{ "monostate/void" }
                          : std::to_string(g_num_float_nomatch.load()))
                   + " (NUM_INT=" + std::to_string(NUM_INT)
                   + " NUM_LONG=" + std::to_string(NUM_LONG)
                   + " NUM_DOUBLE=" + std::to_string(NUM_DOUBLE)
                   + "); value not asserted — only the no-crash fact above is.");
    }
}
