// method_static — exhaustive JVM tests for the STATIC-method call surface:
//   static_method("name")->call(args)   on vmhook::object<T>.
//
// Feature lives in vmhook/ext/vmhook/vmhook.hpp:
//   * object<T>::static_method(name)            : 14026-14030
//   * object<T>::static_method(name, signature) : 14035-14039
//   * object_base::get_method(type_index,name)        (static path) : 13735-13771
//   * object_base::get_method(type_index,name,sig)    (static path) : 13788-13830
//   * method_proxy::is_static()  (reads JVM_ACC_STATIC)             : 12977-12988
//   * method_proxy::get_compressed_oop() (receiver OOP, 0 if null)  : 13022-13032
//   * method_proxy::call()      (interpreter fast path + decode)    : 12726-12938
//   * method_proxy::call_jni()  (JNI fallback; static dispatch slots
//                                116/119/122/125/128/131/134/137/140/143) : 12141-12695
//   * value_t::as_string() / is_string() / is_void()                : 12066-12110
//
// WHAT THIS MODULE PROVES (the method_static contract), each as ctx.check():
//   1. static_method("m")->call() returns the EXACT Java value for every
//      primitive return type (Z B S C I J F D) at boundary values, plus void.
//   2. static String returns decode to exact UTF-8 (as_string()); empty and
//      null are distinguished and never crash.
//   3. static object returns route through the oop/uint32 value_t alternative;
//      a null object return is ALWAYS a null unique_ptr (hard assert), while the
//      full usable-wrapper contract is call-path dependent (recorded as INFO on
//      the JDK-21 call_jni path, mirroring method_call_object).
//   4. NO RECEIVER is passed to a static method: the first declared argument
//      lands at parameter slot 0 (a `this` would shift it), the proxy's receiver
//      OOP is null (get_compressed_oop()==0), and static recorders observe the
//      exact args at the right slots across the J/D two-slot boundary.
//   5. method_proxy::is_static() == TRUE for every static method and == FALSE
//      for every instance method (the recently-fixed accessor that reads
//      JVM_ACC_STATIC from the live Method's _access_flags, NOT the dead
//      constructor member).
//   6. AUDIT FLAW (still open): the static get_method path has no JVM_ACC_STATIC
//      filter, so static_method("instanceMethod") wrongly returns a non-empty
//      optional.  The fixed is_static() accessor is what lets us DETECT it
//      (is_static()==false on the wrongly-accepted proxy).  Recorded as INFO.
//
// Everything runs inside ONE detour on trigger(int) — the only context where
// current_java_thread is set so method_proxy::call() may dispatch.
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
    constexpr std::int32_t k_static_child_seed = 9090;

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

    // -- Bug #2 flaw probe: static_method() wrongly accepts instance methods --
    std::atomic<int>  g_flaw_iget_has_value{ -1 };       // static_method("iGetSeed").has_value()
    std::atomic<int>  g_flaw_iget_is_static{ -1 };       // ...is_static() on it -> false (detect)
    std::atomic<int>  g_flaw_iecho_has_value{ -1 };
    std::atomic<int>  g_flaw_iecho_is_static{ -1 };

    // -- signature-overload accessor: static_method(name, sig) --
    std::atomic<std::int64_t> g_sig_echo_ret{ k_uncaptured };  // static_method("sEchoInt","(I)I")
    std::atomic<int>  g_sig_isstatic{ -1 };

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

        // ===================== Bug #2: static_method accepts an instance =======
        // The static get_method path has NO JVM_ACC_STATIC filter, so these
        // return non-empty optionals for INSTANCE methods (the flaw).  The fixed
        // is_static() accessor reports them as non-static (the detection).
        {
            auto flaw_iget = method_static::static_method("iGetSeed");
            g_flaw_iget_has_value.store(flaw_iget.has_value() ? 1 : 0);
            g_flaw_iget_is_static.store((flaw_iget && flaw_iget->is_static()) ? 1 : 0);

            auto flaw_iecho = method_static::static_method("iEcho");
            g_flaw_iecho_has_value.store(flaw_iecho.has_value() ? 1 : 0);
            g_flaw_iecho_is_static.store((flaw_iecho && flaw_iecho->is_static()) ? 1 : 0);
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

        (void)self;
        g_all_calls_ran.store(true);
    }
}

VMHOOK_JVM_MODULE(method_static)
{
    vmhook::register_class<method_static>("vmhook/fixtures/MethodStatic");

    // Record which dispatch path the live JDK uses (object-return usability is
    // path-dependent; primitives + String are path-independent).
    const bool call_stub_present{ vmhook::detail::find_call_stub_entry() != nullptr };
    ctx.record(std::string{ "[INFO] method_static dispatch path: " }
               + (call_stub_present ? "call_stub fast path (object returns are real compressed OOPs)"
                                    : "JNI fallback (object returns TRUNCATED/freed — KNOWN call_jni flaw)"));

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
        // recorder hit counter aggregates sVoidBump + sRecordInt-family bumps;
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
        // Hard invariant (every path): a null object return is a null uptr.
        // This is the single most important object-return guarantee — a null
        // Java return must never fabricate a wrapper.
        ctx.check("ms_object_null_returns_null_unique_ptr",
                  g_obj_null_is_null_uptr.load(std::memory_order_relaxed));
        // Path-independent value_t routing: a non-String object is never
        // is_string().
        ctx.check("ms_object_return_not_string", g_obj_child_is_string.load() == 0);
        // is_void() on a null OBJECT return is PATH-DEPENDENT: call_stub yields
        // monostate (is_void()==true); call_jni yields uint32{0} (is_void()==false,
        // but the unique_ptr decode still correctly produces null above).
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
            // Full usable-wrapper contract holds only when the call stub is live.
            ctx.check("ms_object_make_child_non_null",
                      g_obj_child_nonnull.load(std::memory_order_relaxed));
            ctx.check("ms_object_make_child_seed_through_wrapper",
                      g_obj_child_seed.load(std::memory_order_relaxed) == k_static_child_seed);
            ctx.check("ms_object_non_null_not_void", g_obj_child_is_void.load() == 0);
        }
        else
        {
            // JDK-21 call_jni: object handle is truncated/freed.  Record what
            // happened; do NOT fail CI for a documented flaw this module cannot
            // fix (mirrors method_call_object's policy).
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
        // sEchoInt(v) returns v: arg landed at slot 0, no phantom receiver.
        ctx.check("ms_no_receiver_echo_returns_arg", g_echo_ret.load() == 13572468);
        // The recorded fields confirm each arg arrived intact at the right slot.
        ctx.check("ms_no_receiver_recorded_long_return",
                  g_record_long_ret.load() == static_cast<std::int64_t>(0x0011223344556677LL));
        ctx.check("ms_no_receiver_recorded_long_field",
                  method_static::get_recorded_long_arg() == static_cast<std::int64_t>(0x0011223344556677LL));
        ctx.check("ms_no_receiver_three_return",
                  g_record_three_ret.load()
                      == static_cast<std::int64_t>(5) + 0x1122334455667788LL + (-13));
        // First int at slot 0, long across 1-2, trailing int at slot 3 — all
        // correct ONLY if no receiver shifted them.
        ctx.check("ms_no_receiver_three_first_int_slot0",
                  method_static::get_recorded_first() == 5);
        ctx.check("ms_no_receiver_three_long_slot1",
                  method_static::get_recorded_second() == 0x1122334455667788LL);
        ctx.check("ms_no_receiver_three_trailing_int_after_long",
                  method_static::get_recorded_third() == -13);
        // The proxy reports a NULL receiver OOP for static methods.
        ctx.check("ms_no_receiver_oop_zero_echo", g_recv_oop_zero_echo.load() == 1);
        ctx.check("ms_no_receiver_oop_zero_int", g_recv_oop_zero_int.load() == 1);
        ctx.check("ms_no_receiver_oop_zero_void", g_recv_oop_zero_void.load() == 1);

        // ==================================================================
        //  5) is_static() ACCESSOR — true for static, false for instance
        //     (the recently-fixed accessor: reads JVM_ACC_STATIC live)
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
        //  6) AUDIT FLAW (still open): static_method() wrongly accepts an
        //     instance method.  The fixed is_static() DETECTS it.  Recorded as
        //     INFO; the detection assertion is hard (is_static()==false).
        // ==================================================================
        ctx.record(std::string{ "[INFO] FLAW vmhook.hpp:13735-13830 — static get_method has no "
                                "JVM_ACC_STATIC filter; static_method(\"iGetSeed\").has_value() = " }
                   + (g_flaw_iget_has_value.load() == 1 ? "true (WRONGLY ACCEPTED an instance method)"
                                                        : "false (correctly rejected)"));
        ctx.record(std::string{ "[INFO] FLAW static_method(\"iEcho\").has_value() = " }
                   + (g_flaw_iecho_has_value.load() == 1 ? "true (WRONGLY ACCEPTED)"
                                                         : "false (correctly rejected)"));
        // Whichever way the flaw resolves, the fixed accessor must never report
        // an instance method as static: if the proxy was (wrongly) created, its
        // is_static() reads the real JVM_ACC_STATIC bit and returns false.
        ctx.check("ms_flaw_wrongly_accepted_instance_is_not_static_iget",
                  g_flaw_iget_is_static.load() == 0);
        ctx.check("ms_flaw_wrongly_accepted_instance_is_not_static_iecho",
                  g_flaw_iecho_is_static.load() == 0);
    }
}
