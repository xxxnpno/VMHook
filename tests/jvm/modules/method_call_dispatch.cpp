// method_call_dispatch — exhaustive JVM tests for vmhook::method_proxy::call().
//
// HISTORY, because it explains the shape of this file and the mcj_ prefix.  The
// module was written as method_call_jni_fallback, to cover a SECOND dispatcher:
// call() probed detail::find_call_stub_entry(), and when the entry was absent it
// short-circuited into the call stub, which marshalled args into a jvalue[] and
// dispatched via Call(Static)?<Type>MethodA.  The module was built on the belief
// that the entry is absent on JDK 21+ and so CI naturally exercised the fallback.
//
// Both halves of that were wrong.  StubRoutines::_call_stub_entry was never
// published through VMStructs on ANY JDK, so find_call_stub_entry() returned null
// everywhere and call() was a silent no-op — the "fallback" was not a fallback,
// it was the only thing that ever ran.  The entry is now DERIVED from
// StubRoutines::_call_stub_return_address, resolves on 8/21/26 alike, and the JNI
// dispatcher has been deleted with it.  There is exactly one dispatch path.
//
// So this module is now what it was always really worth: the deepest single
// exercise of call() semantics in the suite.  Assertion names keep the mcj_
// prefix deliberately — several thousand of them are directly comparable against
// historical CI runs, which is how regressions in this area get bisected.
//
// WHAT IT STRESSES:
//   * every return type    : void Z B C S I J F D String Object (instance+static)
//   * every arg shape      : no-arg, single primitive, String, Object,
//                            multi-arg incl. long(J) + double(D) two-slot args
//   * repeat-call stability: tight loops of String-RETURN, String-ARG, and
//                            long+double MULTI-ARG primitive calls, asserting the
//                            result is STABLE across all 256 iterations.  These
//                            began as local-ref-leak guards for the JNI
//                            dispatcher; they are kept because they are the
//                            sharpest available characterization of "call()
//                            accumulates no per-call state", which the call-stub
//                            path has to satisfy just as much.
//   * exception discipline : a throwing callee is REPORTED through value_t
//                            (threw() + exception_class), its pending exception
//                            is cleared off the JavaThread, and the result is
//                            value-initialised rather than garbage.
//   * cache warm-up        : repeated calls on the SAME proxy reuse the resolved
//                            method with no state corruption.
//   * instance vs static   : both dispatch kinds, interleaved.
//
// call() must run where current_java_thread is set, i.e. inside a hook detour.
// So we hook MethodCallDispatch.trigger(int); the probe calls trigger() on a real
// bytecode dispatch, and the detour performs every call() below on the live
// receiver + the static methods, recording observations into file-scope atomics.
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
    // Wrapper for vmhook.fixtures.MethodCallDispatch.  Instance helpers convert each
    // returned value_t into the exact matching C++ type (so the conversion
    // operator is exercised at the right target type); static helpers exercise
    // the CallStatic*MethodA slots / the FindClass-based static jclass path.
    class mcd_fixture : public vmhook::object<mcd_fixture>
    {
    public:
        explicit mcd_fixture(vmhook::oop_t instance) noexcept
            : vmhook::object<mcd_fixture>{ instance }
        {
        }

        // -- go/done handshake + side-effect readback --
        static auto set_go(bool v) -> void              { static_field("go")->set(v); }
        static auto get_done() -> bool                  { return static_field("done")->get(); }
        static auto get_trigger_count() -> std::int32_t { return static_field("triggerCount")->get(); }
        static auto void_instance_hits() -> std::int32_t { return static_field("voidInstanceHits")->get(); }
        static auto void_static_hits()   -> std::int32_t { return static_field("voidStaticHits")->get(); }

        static auto multi_prim_called() -> bool        { return static_field("multiPrimCalled")->get(); }
        static auto multi_arg_int()     -> std::int32_t { return static_field("multiArgInt")->get(); }
        static auto multi_arg_long()    -> std::int64_t { return static_field("multiArgLong")->get(); }
        static auto multi_arg_double()  -> double       { return static_field("multiArgDouble")->get(); }

        static auto two_slot_called()     -> bool        { return static_field("twoSlotLoopCalled")->get(); }
        static auto two_slot_last_long()  -> std::int64_t { return static_field("twoSlotLastLong")->get(); }
        static auto two_slot_last_double()-> double       { return static_field("twoSlotLastDouble")->get(); }

        static auto string_arg_called() -> bool        { return static_field("stringArgCalled")->get(); }
        static auto string_arg_value()  -> std::string  { return static_field("stringArgValue")->get(); }
        static auto string_arg_len()    -> std::int32_t { return static_field("stringArgLen")->get(); }

        static auto object_arg_called()   -> bool        { return static_field("objectArgCalled")->get(); }
        static auto object_arg_non_null() -> bool        { return static_field("objectArgNonNull")->get(); }
        static auto object_arg_identity() -> std::int32_t { return static_field("objectArgIdentity")->get(); }
        static auto self_identity()       -> std::int32_t { return static_field("selfIdentity")->get(); }

        static auto last_echo_arg() -> std::int32_t { return static_field("lastEchoArg")->get(); }

        static auto record_string_called()   -> bool        { return static_field("recordStringCalled")->get(); }
        static auto record_string_char_len() -> std::int32_t { return static_field("recordStringCharLen")->get(); }
        static auto record_string_cp_count() -> std::int32_t { return static_field("recordStringCpCount")->get(); }
        static auto record_string_first_cp() -> std::int32_t { return static_field("recordStringFirstCp")->get(); }
        static auto record_string_last_cp()  -> std::int32_t { return static_field("recordStringLastCp")->get(); }
        static auto record_string_hash()     -> std::int32_t { return static_field("recordStringHash")->get(); }

        static auto ctor_calls() -> std::int32_t { return static_field("ctorCalls")->get(); }

        // -- narrow / wide primitive ARG recorders (one per marshaller arm) --
        static auto arg_bool_called()  -> bool        { return static_field("argBoolCalled")->get(); }
        static auto arg_bool_value()   -> bool        { return static_field("argBoolValue")->get(); }
        static auto arg_byte_called()  -> bool        { return static_field("argByteCalled")->get(); }
        static auto arg_byte_value()   -> std::int32_t { return static_field("argByteValue")->get(); }
        static auto arg_char_called()  -> bool        { return static_field("argCharCalled")->get(); }
        static auto arg_char_value()   -> std::int32_t { return static_field("argCharValue")->get(); }
        static auto arg_short_called() -> bool        { return static_field("argShortCalled")->get(); }
        static auto arg_short_value()  -> std::int32_t { return static_field("argShortValue")->get(); }
        static auto arg_float_called() -> bool        { return static_field("argFloatCalled")->get(); }
        static auto arg_float_bits()   -> std::int32_t { return static_field("argFloatBits")->get(); }
        static auto arg_double_called()-> bool        { return static_field("argDoubleCalled")->get(); }
        static auto arg_double_bits()  -> std::int64_t { return static_field("argDoubleBits")->get(); }

        // -- many-arg / all-two-slot shape recorders --
        static auto six_arg_called()   -> bool        { return static_field("sixArgCalled")->get(); }
        static auto six_arg_packed()   -> std::int64_t { return static_field("sixArgPacked")->get(); }
        static auto four_wide_called() -> bool        { return static_field("fourWideCalled")->get(); }
        static auto four_wide_result() -> std::int64_t { return static_field("fourWideResult")->get(); }

        // -- null-reference ARG contract recorders --
        static auto null_str_called()  -> bool        { return static_field("nullStrArgCalled")->get(); }
        static auto null_str_was_null()-> bool        { return static_field("nullStrArgWasNull")->get(); }
        static auto null_obj_called()  -> bool        { return static_field("nullObjArgCalled")->get(); }
        static auto null_obj_was_null()-> bool        { return static_field("nullObjArgWasNull")->get(); }

        // -- instance primitive returners (convert at the exact target type) --
        auto call_bool(const char* n) -> bool          { return get_method(n)->call(); }
        auto call_byte(const char* n) -> std::int8_t   { return get_method(n)->call(); }
        auto call_char(const char* n) -> std::uint16_t { return get_method(n)->call(); }
        auto call_short(const char* n) -> std::int16_t { return get_method(n)->call(); }
        auto call_int(const char* n) -> std::int32_t   { return get_method(n)->call(); }
        auto call_long(const char* n) -> std::int64_t  { return get_method(n)->call(); }
        auto call_float(const char* n) -> float        { return get_method(n)->call(); }
        auto call_double(const char* n) -> double      { return get_method(n)->call(); }

        // -- static primitive returners --
        static auto scall_bool(const char* n) -> bool          { return static_method(n)->call(); }
        static auto scall_byte(const char* n) -> std::int8_t   { return static_method(n)->call(); }
        static auto scall_char(const char* n) -> std::uint16_t { return static_method(n)->call(); }
        static auto scall_short(const char* n) -> std::int16_t { return static_method(n)->call(); }
        static auto scall_int(const char* n) -> std::int32_t   { return static_method(n)->call(); }
        static auto scall_long(const char* n) -> std::int64_t  { return static_method(n)->call(); }
        static auto scall_float(const char* n) -> float        { return static_method(n)->call(); }
        static auto scall_double(const char* n) -> double      { return static_method(n)->call(); }
    };

    // ---- raw-bit float/double capture so special values survive the atomic ----
    inline auto f2bits(float f) noexcept -> std::uint32_t
    {
        std::uint32_t b{ 0 };
        std::memcpy(&b, &f, sizeof(b));
        return b;
    }
    inline auto d2bits(double d) noexcept -> std::uint64_t
    {
        std::uint64_t b{ 0 };
        std::memcpy(&b, &d, sizeof(b));
        return b;
    }

    constexpr std::int64_t k_uncaptured = static_cast<std::int64_t>(0xDEADBEEFCAFEF00Dull);

    // ------------------------------------------------------------------
    //  Captured observations.  The detour writes; the module body reads.
    // ------------------------------------------------------------------
    std::atomic<int>  g_detour_calls{ 0 };
    std::atomic<bool> g_detour_saw_self{ false };
    std::atomic<bool> g_call_stub_present{ false };

    // instance primitive + void returns
    std::atomic<int>          g_bool_true{ -1 };
    std::atomic<int>          g_bool_false{ -1 };
    std::atomic<std::int64_t> g_byte{ k_uncaptured };
    std::atomic<std::int64_t> g_char{ k_uncaptured };
    std::atomic<std::int64_t> g_char_max{ k_uncaptured };
    std::atomic<std::int64_t> g_char_max_as_int{ k_uncaptured }; // zero-extend proof
    std::atomic<std::int64_t> g_short{ k_uncaptured };
    std::atomic<std::int64_t> g_int{ k_uncaptured };
    std::atomic<std::int64_t> g_long{ k_uncaptured };
    std::atomic<bool>         g_float_captured{ false };
    std::atomic<std::uint32_t> g_float_bits{ 0 };
    std::atomic<bool>         g_double_captured{ false };
    std::atomic<std::uint64_t> g_double_bits{ 0 };
    std::atomic<int>          g_void_is_void{ -1 };

    // static primitive returns
    std::atomic<int>          g_s_bool_true{ -1 };
    std::atomic<std::int64_t> g_s_byte{ k_uncaptured };
    std::atomic<std::int64_t> g_s_char{ k_uncaptured };
    std::atomic<std::int64_t> g_s_short{ k_uncaptured };
    std::atomic<std::int64_t> g_s_int{ k_uncaptured };
    std::atomic<std::int64_t> g_s_long{ k_uncaptured };
    std::atomic<bool>         g_s_float_captured{ false };
    std::atomic<std::uint32_t> g_s_float_bits{ 0 };
    std::atomic<bool>         g_s_double_captured{ false };
    std::atomic<std::uint64_t> g_s_double_bits{ 0 };
    std::atomic<int>          g_s_void_is_void{ -1 };

    // single-arg primitive echo (instance + static)
    std::atomic<std::int64_t> g_echo_int{ k_uncaptured };
    std::atomic<std::int64_t> g_echo_long{ k_uncaptured };
    std::atomic<std::int64_t> g_s_echo_int{ k_uncaptured };

    // call_jni cache mis-keying regression (library #3): ONE held name-only proxy
    // reused across two overloads (combo(int)->int, combo(String)->String).
    std::atomic<std::int64_t> g_combo_int_first{ k_uncaptured };      // combo(5) FIRST -> 105
    std::atomic<int>          g_combo_str_after_int{ -1 };            // combo("hi") after int -> "hi!"
    std::atomic<std::int64_t> g_combo_int_after_str{ k_uncaptured };  // combo(7) after String -> 107
    std::atomic<int>          g_combo_str_first{ -1 };                // combo("yo") FIRST -> "yo!"

    // multi-arg (int, long, double) returns + side effects (instance + static)
    std::atomic<std::int64_t> g_sum_ild{ k_uncaptured };
    std::atomic<std::int64_t> g_s_sum_ild{ k_uncaptured };

    // String returns (instance + static)
    std::string  g_str_inst{};
    std::string  g_str_static{};
    std::atomic<bool> g_str_inst_is_string{ false };
    std::atomic<bool> g_str_static_is_string{ false };
    std::atomic<bool> g_str_captured{ false };

    // String arg round-trips
    std::string  g_echo_str_ascii{};
    std::string  g_echo_str_empty{};
    std::string  g_echo_str_unicode{};
    std::atomic<bool> g_echo_str_captured{ false };

    // String arg -> void body
    std::atomic<int> g_consume_str_is_void{ -1 };

    // Object arg -> void body
    std::atomic<int> g_consume_obj_is_void{ -1 };

    // Object returns (instance + static): identity + null contract
    std::atomic<bool>           g_self_nonnull{ false };
    std::atomic<std::uintptr_t> g_self_instance{ 0 };
    std::atomic<std::uintptr_t> g_receiver_instance{ 0 };
    std::atomic<int>            g_self_is_void{ -1 };
    std::atomic<bool>           g_null_obj_is_null{ false };
    std::atomic<int>            g_null_obj_is_void{ -1 };
    std::atomic<bool>           g_static_self_nonnull{ false };
    std::atomic<std::uintptr_t> g_static_self_instance{ 0 };
    std::atomic<bool>           g_static_null_is_null{ false };
    std::atomic<bool>           g_array_nonnull{ false };

    // -- TIGHT LOOPS (local-ref-leak characterization) --
    // String-RETURN loop: every call creates+releases a JNI local ref for the
    // returned String.  Distinct==1 over the whole loop proves no leak/starve.
    std::atomic<int> g_ret_loop_iters{ 0 };
    std::atomic<int> g_ret_loop_distinct{ -1 };
    // String-ARG loop: every call creates+releases a NewStringUTF local ref.
    std::atomic<int> g_arg_loop_iters{ 0 };
    std::atomic<int> g_arg_loop_mismatches{ -1 };
    // long+double MULTI-ARG primitive loop: union-aliasing footgun guard.
    std::atomic<int> g_two_loop_iters{ 0 };
    std::atomic<int> g_two_loop_mismatches{ -1 };

    // Non-corruption: a value-returning call AFTER all the loops.
    std::atomic<std::int64_t> g_post_loop_echo{ k_uncaptured };

    // ── STATIC-METHOD-VIA-INSTANCE-PROXY regression (the CI-failure bug) ──
    // A static method resolved through an INSTANCE wrapper's get_method("..")
    // yields a proxy whose this->object is the live receiver but whose Method*
    // is ACC_STATIC.  Before the fix, call()/call_jni keyed static-ness solely
    // on `object == nullptr`, so it mis-classified these as INSTANCE calls:
    //   * call_jni -> GetObjectClass + GetMethodID (which does NOT resolve
    //     static methods) -> null id -> monostate (wrong result), and on JVMs
    //     where it did resolve, the live receiver was bound as the static
    //     method's FIRST declared arg -> corrupted result + poisoned JNI
    //     exception state (which let a sibling module's uncaught exception
    //     escape and kill the JVM).
    //   * call (call_stub) -> prepends the receiver as locals[0], shifting
    //     every real argument down one slot -> wrong arithmetic.
    // The fix treats the call as static when object==nullptr OR is_static().
    // These sentinels are recognizable and INDEPENDENT of the receiver, so a
    // correct value can only come from a true static dispatch.
    std::atomic<std::int64_t> g_svia_ret_int{ k_uncaptured };      // sRetInt() == INT_MIN
    std::atomic<std::int64_t> g_svia_echo_int{ k_uncaptured };     // sEchoInt(v) == v
    std::atomic<std::int64_t> g_svia_sum_ild{ k_uncaptured };      // sSumILD(i,j,d)
    std::atomic<std::int64_t> g_svia_echo_sig{ k_uncaptured };     // via (name,sig) overload
    std::atomic<int>          g_svia_is_static{ -1 };              // proxy.is_static() == true
    std::atomic<int>          g_svia_has_receiver{ -1 };           // proxy receiver OOP != 0
    // Proof the receiver survives a static-via-instance dispatch unharmed: an
    // ordinary INSTANCE call performed immediately AFTER must still be correct.
    // NOTE: we deliberately use echoLong (NOT echoInt) for this follow-up — an
    // instance method that does NOT write the shared `lastEchoArg` field that
    // the unrelated `mcj_echo_int_side_effect` assertion reads.  Using echoInt
    // here would overwrite lastEchoArg after the post-loop echo and make that
    // sibling assertion fail (the regression the first re-land attempt hit was
    // this self-inflicted field clobber, NOT an is_static() defect).
    std::atomic<std::int64_t> g_svia_followup_inst_echo{ k_uncaptured };
    // Sentinels for the static-via-instance regression.
    constexpr std::int64_t k_svia_echo   = static_cast<std::int64_t>(0x6E5D4C3BLL); // 1851877947
    constexpr std::int64_t k_svia_follow = static_cast<std::int64_t>(0x2BCD16E0ABCDEF01LL);

    // ── ARRAY-RETURN value-correctness (int[] / long[] / String[]) ──────────
    // Decoded array oop -> read length + elements via the crash-safe library
    // readers (vmhook::array_length / vmhook::get_array_element), so the '['
    // dispatch arm is proven to land on the REAL array, not merely "non-null".
    std::atomic<std::int64_t> g_int_arr_len{ k_uncaptured };
    std::atomic<std::int64_t> g_int_arr_e0{ k_uncaptured };
    std::atomic<std::int64_t> g_int_arr_e1{ k_uncaptured };
    std::atomic<std::int64_t> g_int_arr_e2{ k_uncaptured };
    std::atomic<std::int64_t> g_long_arr_len{ k_uncaptured };
    std::atomic<std::int64_t> g_long_arr_e0{ k_uncaptured };
    std::atomic<std::int64_t> g_long_arr_e3{ k_uncaptured };
    std::atomic<bool>         g_str_arr_nonnull{ false };
    std::atomic<std::int64_t> g_str_arr_len{ k_uncaptured };

    // ── String-ARG NUL / astral round-trip into the JVM (the #27 fix) ───────
    // Pure-int observations the fixture body publishes, immune to string decode.
    std::atomic<int> g_nul_arg_captured{ -1 };
    std::atomic<int> g_nul_arg_char_len{ -1 };
    std::atomic<int> g_nul_arg_cp_count{ -1 };
    std::atomic<int> g_nul_arg_hash{ 0 };
    std::atomic<int> g_astral_arg_captured{ -1 };
    std::atomic<int> g_astral_arg_char_len{ -1 };
    std::atomic<int> g_astral_arg_cp_count{ -1 };
    std::atomic<int> g_astral_arg_first_cp{ 0 };
    std::atomic<int> g_astral_arg_last_cp{ 0 };

    // ── NONVIRTUAL <init> via CallNonvirtualVoidMethodA (slot 93) ───────────
    std::atomic<int> g_ctor_proxy_found{ -1 };
    std::atomic<int> g_ctor_calls_before{ -1 };
    std::atomic<int> g_ctor_calls_after{ -1 };
    std::atomic<int> g_ctor_call_is_void{ -1 };
    std::atomic<int> g_ctor_no_pending_exc{ -1 };

    // ── EXCEPTION discipline (callee throws -> reported + cleared, no escape) ─
    // call() detects the callee's throw, clears ThreadShadow::_pending_exception
    // with a pure-VM write (no JNI), and reports the throw back through the
    // returned value_t: threw() is set and exception_class carries the internal
    // class name.  So the observation is the RETURN VALUE, not a thread probe —
    // we record threw()/exception_class per throwing call.  The library having
    // cleared the exception is what lets the follow-up recovery call succeed.
    std::atomic<int>          g_exc_void_threw{ -1 };        // throwVoid() -> threw()
    std::atomic<int>          g_exc_int_threw{ -1 };         // throwReturningInt() -> threw()
    std::atomic<int>          g_exc_static_threw{ -1 };      // sThrowVoid() -> threw()
    std::string               g_exc_void_class{};            // internal name of each throw
    std::string               g_exc_int_class{};
    std::string               g_exc_static_class{};
    // The documented contract for a value-returning callee that throws: the
    // result is VALUE-INITIALISED for the declared return type, never the stub's
    // garbage result slot.  throwReturningInt is int -> exactly 0.
    std::atomic<int>          g_exc_int_result_is_zero{ -1 };
    std::atomic<int>          g_exc_recovery_ok{ -1 };       // echoLong after throws == sentinel
    std::atomic<int>          g_exc_seen_at_least_one{ -1 }; // at least one throw WAS reported
    constexpr std::int32_t    k_exc_recovery = 0x33EC0DE5;   // recovery echo sentinel

    // ── float / double SPECIAL-VALUE RETURNS (IEEE-754 fidelity decode) ─────
    // Raw bits captured so NaN / inf / -0.0 / denormal survive the atomic.
    std::atomic<bool>          g_fret_special_captured{ false };
    std::atomic<std::uint32_t> g_fret_nan_bits{ 0 };
    std::atomic<std::uint32_t> g_fret_posinf_bits{ 0 };
    std::atomic<std::uint32_t> g_fret_neginf_bits{ 0 };
    std::atomic<std::uint32_t> g_fret_negzero_bits{ 0 };
    std::atomic<std::uint32_t> g_fret_denorm_bits{ 0 };
    std::atomic<bool>          g_dret_special_captured{ false };
    std::atomic<std::uint64_t> g_dret_nan_bits{ 0 };
    std::atomic<std::uint64_t> g_dret_posinf_bits{ 0 };
    std::atomic<std::uint64_t> g_dret_negzero_bits{ 0 };
    std::atomic<std::uint64_t> g_dret_denorm_bits{ 0 };
    // Static special-value returns (CallStatic<F|D>MethodA arm).
    std::atomic<bool>          g_s_special_captured{ false };
    std::atomic<std::uint32_t> g_s_fret_nan_bits{ 0 };
    std::atomic<std::uint64_t> g_s_dret_posinf_bits{ 0 };
    std::atomic<std::uint64_t> g_s_dret_negzero_bits{ 0 };

    // ── float / double ARG round-trips (F/D marshaller arm + return decode) ──
    std::atomic<bool>          g_fd_arg_captured{ false };
    std::atomic<std::uint32_t> g_echo_float_bits{ 0 };       // echoFloat(-7.25f)
    std::atomic<std::uint64_t> g_echo_double_bits{ 0 };      // echoDouble(123.456)
    std::atomic<std::uint32_t> g_echo_float_nan_bits{ 0 };   // echoFloat(NaN)
    std::atomic<std::uint32_t> g_s_echo_float_bits{ 0 };     // sEchoFloat(-7.25f)
    std::atomic<std::uint64_t> g_s_echo_double_bits{ 0 };    // sEchoDouble(123.456)

    // ── boundary primitive ARG echoes (int/long edge values round-trip) ─────
    std::atomic<std::int64_t> g_echo_int_min{ k_uncaptured };
    std::atomic<std::int64_t> g_echo_int_max{ k_uncaptured };
    std::atomic<std::int64_t> g_echo_int_neg1{ k_uncaptured };
    std::atomic<std::int64_t> g_echo_int_zero{ k_uncaptured };
    std::atomic<std::int64_t> g_echo_long_min{ k_uncaptured };
    std::atomic<std::int64_t> g_echo_long_max{ k_uncaptured };
    std::atomic<std::int64_t> g_echo_long_neg1{ k_uncaptured };

    // ── many-arg / all-two-slot ARG shapes (slot-array layout proof) ────────
    std::atomic<std::int64_t> g_six_arg_ret{ k_uncaptured };
    std::atomic<std::int64_t> g_s_six_arg_ret{ k_uncaptured };
    std::atomic<std::int64_t> g_four_wide_ret{ k_uncaptured };

    // ── narrow/wide primitive ARG round-trips into the JVM (per arm) ─────────
    std::atomic<int> g_arg_runs{ 0 };   // count of arg-record calls performed

    // ── null-reference ARG contract ─────────────────────────────────────────
    std::atomic<int> g_null_str_ran{ -1 };
    std::atomic<int> g_null_obj_ran{ -1 };

    // ── additional primitive-array returns (per-stride '[' arm) ─────────────
    std::atomic<std::int64_t> g_byte_arr_len{ k_uncaptured };
    std::atomic<std::int64_t> g_byte_arr_e0{ k_uncaptured };
    std::atomic<std::int64_t> g_byte_arr_e2{ k_uncaptured };
    std::atomic<std::int64_t> g_byte_arr_e3{ k_uncaptured };
    std::atomic<std::int64_t> g_bool_arr_len{ k_uncaptured };
    std::atomic<std::int64_t> g_bool_arr_e0{ k_uncaptured };
    std::atomic<std::int64_t> g_bool_arr_e1{ k_uncaptured };
    std::atomic<std::int64_t> g_short_arr_len{ k_uncaptured };
    std::atomic<std::int64_t> g_short_arr_e0{ k_uncaptured };
    std::atomic<std::int64_t> g_short_arr_e2{ k_uncaptured };
    std::atomic<std::int64_t> g_char_arr_len{ k_uncaptured };
    std::atomic<std::int64_t> g_char_arr_e0{ k_uncaptured };
    std::atomic<std::int64_t> g_char_arr_e1{ k_uncaptured };
    std::atomic<std::int64_t> g_char_arr_e2{ k_uncaptured };
    std::atomic<std::int64_t> g_dbl_arr_len{ k_uncaptured };
    std::atomic<bool>         g_dbl_arr_captured{ false };
    std::atomic<std::uint64_t> g_dbl_arr_e0_bits{ 0 };
    std::atomic<std::uint64_t> g_dbl_arr_e2_bits{ 0 };
    std::atomic<std::int64_t> g_flt_arr_len{ k_uncaptured };
    std::atomic<bool>         g_flt_arr_captured{ false };
    std::atomic<std::uint32_t> g_flt_arr_e0_bits{ 0 };
    std::atomic<std::uint32_t> g_flt_arr_e1_bits{ 0 };

    // ═══════════════════════════════════════════════════════════════════════
    //  DEEPENING WAVE: additive cross-checks / value-semantics / type-tag /
    //  decode-width-agreement / idempotency observations.  Every value below is
    //  GROUNDED in behavior the existing passing checks already establish (the
    //  same call, decoded again at a different width or queried for its variant
    //  tag) — no new fixture method, no inferred edge behavior.
    // ═══════════════════════════════════════════════════════════════════════

    // (A) value_t TYPE-TAG discipline: a PRIMITIVE return is neither void nor a
    //     String; a String return IS a string and is NOT void.  These pin the
    //     variant-alternative classification the value_t conversion relies on.
    std::atomic<int> g_int_not_void{ -1 };       // retInt()    -> !is_void()
    std::atomic<int> g_int_not_string{ -1 };     // retInt()    -> !is_string()
    std::atomic<int> g_bool_not_void{ -1 };      // retBoolTrue()-> !is_void()
    std::atomic<int> g_bool_not_string{ -1 };    // retBoolTrue()-> !is_string()
    std::atomic<int> g_long_not_void{ -1 };      // retLong()   -> !is_void()
    std::atomic<int> g_double_not_string{ -1 };  // retDouble() -> !is_string()
    std::atomic<int> g_str_inst_not_void{ -1 };  // retString() -> !is_void()
    std::atomic<int> g_str_static_not_void{ -1 };// sRetString()-> !is_void()
    std::atomic<int> g_loopstr_is_string{ -1 };  // loopString()-> is_string()

    // (B) DECODE-WIDTH AGREEMENT: the SAME no-arg call decoded at two C++ widths
    //     must agree under the documented sign/zero-extension rules.
    //     retByte()  : as int8 == -7  and as int32 (sign-extended) == -7.
    //     retChar()  : as uint16 == 90 and as int32 (zero-extended) == 90.
    //     retCharMax(): as uint16 == 0xFFFF and as int32 == 65535 (zero-extend).
    //     retShort() : as int16 == -12345 and as int32 (sign-extended) == -12345.
    //     retLong()  : as int64 stable across two independent decodes.
    std::atomic<std::int64_t> g_byte_as_int32{ k_uncaptured };
    std::atomic<std::int64_t> g_short_as_int32{ k_uncaptured };
    std::atomic<std::int64_t> g_char_as_int32{ k_uncaptured };
    std::atomic<std::int64_t> g_long_decode_a{ k_uncaptured };
    std::atomic<std::int64_t> g_long_decode_b{ k_uncaptured };

    // (C) IDEMPOTENCY: a deterministic value-returning method called twice on the
    //     same fresh proxy yields the identical value (no per-call drift / cache
    //     poisoning between two reads).
    std::atomic<std::int64_t> g_int_idem_a{ k_uncaptured };
    std::atomic<std::int64_t> g_int_idem_b{ k_uncaptured };
    std::atomic<int>          g_retstr_idem_equal{ -1 };  // retString()==retString()
    std::atomic<int>          g_retstr_idem_nonempty{ -1 };

    // (D) value_t VALUE-SEMANTICS: copy and move of a captured value_t both
    //     preserve the decoded result (the variant is regular).  Captured from
    //     retString() (String alt) and echoInt(sentinel) (int32 alt).
    std::atomic<int>          g_vt_copy_str_ok{ -1 };
    std::atomic<int>          g_vt_move_str_ok{ -1 };
    std::atomic<std::int64_t> g_vt_copy_int{ k_uncaptured };

    // (E) String CROSS-CHECK: as_string() of retString() has the exact expected
    //     length, and the empty-arg echo decodes to size()==0 (count==size).
    std::atomic<std::int64_t> g_str_inst_len{ k_uncaptured };
    std::atomic<std::int64_t> g_echo_empty_len{ k_uncaptured };

    // (F) ZERO / ONE / -1 boundary RETURN of a single-arg echo: echoIntId(0)==0,
    //     echoIntId(1)==1 round-trip exactly (smallest magnitudes through out.i).
    std::atomic<std::int64_t> g_echo_one{ k_uncaptured };
    std::atomic<std::int64_t> g_echo_long_one{ k_uncaptured };

    // (G) SINGLE-element & boundary ARRAY-length agreement: array_length of every
    //     captured array equals the fixture's literal element count (count==size
    //     cross-checked against the per-element reads already asserted).  We also
    //     re-read each array's length a SECOND time to prove array_length is a
    //     pure read (idempotent, no mutation of the header).
    std::atomic<std::int64_t> g_int_arr_len2{ k_uncaptured };
    std::atomic<std::int64_t> g_long_arr_len2{ k_uncaptured };

    // Sentinels (mirror the fixture's boundary values).
    constexpr std::int64_t k_int_ret    = 0x0BADF00DLL;            // 195948557
    constexpr std::int64_t k_long_ret   = static_cast<std::int64_t>(0x0123456789ABCDEFLL);
    constexpr std::int32_t k_echo_int   = 0x5A5A5A5A;              // 1515870810
    constexpr std::int64_t k_echo_long  = static_cast<std::int64_t>(0x7FEEDDCCBBAA9988LL);
    constexpr std::int32_t k_sum_i      = 1000;
    constexpr std::int64_t k_sum_j      = 0x0000000100000000LL;    // 4294967296 (high dword set)
    constexpr double       k_sum_d      = 250.0;                   // exact (long)d == 250
    constexpr std::int32_t k_post_echo  = 0x1357ACE0;

    // Boundary echo sentinels.
    constexpr std::int32_t k_int_min    = static_cast<std::int32_t>(0x80000000);   // INT_MIN
    constexpr std::int32_t k_int_max    = 0x7FFFFFFF;                              // INT_MAX
    constexpr std::int64_t k_long_min   = static_cast<std::int64_t>(0x8000000000000000LL);
    constexpr std::int64_t k_long_max   = 0x7FFFFFFFFFFFFFFFLL;

    // float/double ARG sentinels (exactly representable -> bit-exact round trip).
    constexpr float        k_echo_float  = -7.25f;          // exact in binary
    constexpr double       k_echo_double = 123.456;         // not exact, but bit-stable round trip

    // Six-arg / four-wide sentinels (mixed single/two-slot).
    constexpr std::int32_t k_six_a = 7;
    constexpr std::int64_t k_six_b = 0x0000000200000000LL;  // 8589934592 (high dword set)
    constexpr std::int32_t k_six_c = -3;
    constexpr double       k_six_d = 64.0;                  // exact (long)d == 64
    constexpr std::int64_t k_six_e = static_cast<std::int64_t>(0x00000000F0000000LL);
    constexpr std::int32_t k_six_f = 11;
    // four-wide: (J,D,J,D) -> a + c + (long)b - (long)d
    constexpr std::int64_t k_fw_a = static_cast<std::int64_t>(0x1111000022220000LL);
    constexpr double       k_fw_b = 500.0;
    constexpr std::int64_t k_fw_c = static_cast<std::int64_t>(0x0000333300004444LL);
    constexpr double       k_fw_d = 125.0;

    // Narrow primitive ARG sentinels.
    constexpr std::int32_t k_arg_byte_int  = -7;            // (byte)-7 sign-extends to -7
    constexpr std::int32_t k_arg_char_int  = 65535;         // (char)0xFFFF zero-extends
    constexpr std::int32_t k_arg_short_int = -12345;        // sign-extends
    constexpr float        k_arg_float     = 2.5f;          // exact
    constexpr double       k_arg_double    = -8.5;          // exact

    auto run_all(const std::unique_ptr<mcd_fixture>& self) -> void
    {
        if (!self)
        {
            return;
        }
        mcd_fixture& s = *self;
        g_receiver_instance.store(
            reinterpret_cast<std::uintptr_t>(s.get_instance()),
            std::memory_order_relaxed);

        // ───────── instance primitive returns ─────────
        g_bool_true.store(s.call_bool("retBoolTrue") ? 1 : 0);
        g_bool_false.store(s.call_bool("retBoolFalse") ? 1 : 0);
        g_byte.store(s.call_byte("retByte"));
        g_char.store(s.call_char("retChar"));
        g_char_max.store(s.call_char("retCharMax"));
        {
            const std::int32_t as_int = s.get_method("retCharMax")->call();
            g_char_max_as_int.store(as_int);
        }
        g_short.store(s.call_short("retShort"));
        g_int.store(s.call_int("retInt"));
        g_long.store(s.call_long("retLong"));
        g_float_bits.store(f2bits(s.call_float("retFloat")));
        g_float_captured.store(true);
        g_double_bits.store(d2bits(s.call_double("retDouble")));
        g_double_captured.store(true);
        {
            auto p{ s.get_method("retVoid") };
            if (p.has_value())
            {
                g_void_is_void.store(p->call().is_void() ? 1 : 0);
            }
        }

        // ───────── DEEPEN (A): value_t type-tag discipline ─────────
        // A primitive return is neither void nor a String; a String return IS a
        // string and is NOT void.  Each query is a fresh call so the variant tag
        // is read off a genuine dispatch, not a cached one.
        {
            auto p{ s.get_method("retInt") };
            if (p.has_value())
            {
                const auto v{ p->call() };
                g_int_not_void.store(v.is_void() ? 0 : 1);
                g_int_not_string.store(v.is_string() ? 0 : 1);
            }
        }
        {
            auto p{ s.get_method("retBoolTrue") };
            if (p.has_value())
            {
                const auto v{ p->call() };
                g_bool_not_void.store(v.is_void() ? 0 : 1);
                g_bool_not_string.store(v.is_string() ? 0 : 1);
            }
        }
        {
            auto p{ s.get_method("retLong") };
            if (p.has_value()) { g_long_not_void.store(p->call().is_void() ? 0 : 1); }
        }
        {
            auto p{ s.get_method("retDouble") };
            if (p.has_value()) { g_double_not_string.store(p->call().is_string() ? 0 : 1); }
        }

        // ───────── DEEPEN (B): decode-width agreement (same call, two widths) ─────────
        // retByte() decoded as int32 must sign-extend to the same -7 the int8
        // decode produced; retChar()/retCharMax() zero-extend; retShort() sign-
        // extends; retLong() is stable across two independent decodes.
        {
            auto p{ s.get_method("retByte") };
            if (p.has_value())
            {
                const std::int32_t as_int = p->call();
                g_byte_as_int32.store(as_int);
            }
        }
        {
            auto p{ s.get_method("retShort") };
            if (p.has_value())
            {
                const std::int32_t as_int = p->call();
                g_short_as_int32.store(as_int);
            }
        }
        {
            auto p{ s.get_method("retChar") };
            if (p.has_value())
            {
                const std::int32_t as_int = p->call();
                g_char_as_int32.store(as_int);
            }
        }
        {
            auto p{ s.get_method("retLong") };
            if (p.has_value())
            {
                g_long_decode_a.store(static_cast<std::int64_t>(p->call()));
                g_long_decode_b.store(static_cast<std::int64_t>(p->call()));
            }
        }

        // ───────── DEEPEN (C): idempotency of a deterministic value return ─────────
        {
            auto p{ s.get_method("retInt") };
            if (p.has_value())
            {
                g_int_idem_a.store(static_cast<std::int32_t>(p->call()));
                g_int_idem_b.store(static_cast<std::int32_t>(p->call()));
            }
        }
        {
            auto p{ s.get_method("retString") };
            if (p.has_value())
            {
                const std::string a{ p->call().as_string() };
                const std::string b{ p->call().as_string() };
                g_retstr_idem_equal.store(a == b ? 1 : 0);
                g_retstr_idem_nonempty.store(!a.empty() ? 1 : 0);
            }
        }

        // ───────── DEEPEN (D): value_t copy/move value-semantics ─────────
        // A captured value_t copies and moves while preserving the decode.
        {
            auto p{ s.get_method("retString") };
            if (p.has_value())
            {
                const auto original{ p->call() };
                const auto copied{ original };               // copy-construct
                auto to_move{ original };                    // copy then move-from
                const auto moved{ std::move(to_move) };      // move-construct
                g_vt_copy_str_ok.store(copied.as_string() == original.as_string() ? 1 : 0);
                g_vt_move_str_ok.store(moved.as_string() == original.as_string() ? 1 : 0);
            }
        }
        {
            auto p{ s.get_method("echoIntId") };
            if (p.has_value())
            {
                const auto original{ p->call(k_echo_int) };
                const auto copied{ original };
                g_vt_copy_int.store(static_cast<std::int32_t>(copied));
            }
        }

        // ───────── DEEPEN (E): String length cross-check (count == size) ─────────
        {
            auto p{ s.get_method("retString") };
            if (p.has_value())
            {
                g_str_inst_len.store(static_cast<std::int64_t>(p->call().as_string().size()));
            }
        }
        {
            auto p{ s.get_method("echoString") };
            if (p.has_value())
            {
                g_echo_empty_len.store(
                    static_cast<std::int64_t>(p->call(std::string{}).as_string().size()));
            }
        }

        // ───────── DEEPEN (F): smallest-magnitude echo boundaries (0/1) ─────────
        {
            auto p{ s.get_method("echoIntId") };
            if (p.has_value())
            {
                g_echo_one.store(static_cast<std::int32_t>(p->call(static_cast<std::int32_t>(1))));
            }
        }
        {
            auto p{ s.get_method("echoLongId") };
            if (p.has_value())
            {
                g_echo_long_one.store(static_cast<std::int64_t>(p->call(static_cast<std::int64_t>(1))));
            }
        }

        // ───────── DEEPEN (G): loopString() is a String + re-read array lengths ─────────
        {
            auto p{ s.get_method("loopString") };
            if (p.has_value()) { g_loopstr_is_string.store(p->call().is_string() ? 1 : 0); }
        }
        {
            auto p{ s.get_method("retIntArray") };
            if (p.has_value())
            {
                void* const arr{ static_cast<void*>(p->call()) };
                if (arr != nullptr) { g_int_arr_len2.store(vmhook::array_length(arr)); }
            }
        }
        {
            auto p{ s.get_method("retLongArray") };
            if (p.has_value())
            {
                void* const arr{ static_cast<void*>(p->call()) };
                if (arr != nullptr) { g_long_arr_len2.store(vmhook::array_length(arr)); }
            }
        }

        // ───────── static primitive returns ─────────
        g_s_bool_true.store(mcd_fixture::scall_bool("sRetBoolTrue") ? 1 : 0);
        g_s_byte.store(mcd_fixture::scall_byte("sRetByte"));
        g_s_char.store(mcd_fixture::scall_char("sRetChar"));
        g_s_short.store(mcd_fixture::scall_short("sRetShort"));
        g_s_int.store(mcd_fixture::scall_int("sRetInt"));
        g_s_long.store(mcd_fixture::scall_long("sRetLong"));
        g_s_float_bits.store(f2bits(mcd_fixture::scall_float("sRetFloat")));
        g_s_float_captured.store(true);
        g_s_double_bits.store(d2bits(mcd_fixture::scall_double("sRetDouble")));
        g_s_double_captured.store(true);
        {
            auto p{ mcd_fixture::static_method("sRetVoid") };
            if (p.has_value())
            {
                g_s_void_is_void.store(p->call().is_void() ? 1 : 0);
            }
        }

        // ───────── single-primitive-arg echoes ─────────
        g_echo_int.store(static_cast<std::int32_t>(s.get_method("echoInt")->call(k_echo_int)));
        g_echo_long.store(static_cast<std::int64_t>(s.get_method("echoLong")->call(k_echo_long)));
        g_s_echo_int.store(static_cast<std::int32_t>(
            mcd_fixture::static_method("sEchoInt")->call(k_echo_int)));

        // ───────── call_jni cache mis-keying (library #3) ─────────
        // ONE held name-only proxy reused across two overloads with DIFFERENT
        // return types (combo(int)->int, combo(String)->String).  Pre-fix, the
        // 2nd call reused the 1st overload's cached jmethodID + return-type char,
        // invoking the wrong method and decoding its return as the wrong type.
        {
            auto p{ s.get_method("combo") };
            if (p.has_value())
            {
                g_combo_int_first.store(static_cast<std::int64_t>(p->call(static_cast<std::int32_t>(5))));
                g_combo_str_after_int.store(p->call(std::string{ "hi" }).as_string() == "hi!" ? 1 : 0);
            }
        }
        {
            auto p{ s.get_method("combo") };
            if (p.has_value())
            {
                g_combo_str_first.store(p->call(std::string{ "yo" }).as_string() == "yo!" ? 1 : 0);
                g_combo_int_after_str.store(static_cast<std::int64_t>(p->call(static_cast<std::int32_t>(7))));
            }
        }

        // ───────── boundary primitive-arg echoes (int/long edge values) ─────────
        // echoIntId / echoLongId return their arg WITHOUT touching lastEchoArg, so
        // these extra echoes cannot clobber the mcj_echo_int_side_effect breadcrumb.
        {
            auto p{ s.get_method("echoIntId") };
            if (p.has_value())
            {
                g_echo_int_min.store(static_cast<std::int32_t>(p->call(k_int_min)));
                g_echo_int_max.store(static_cast<std::int32_t>(p->call(k_int_max)));
                g_echo_int_neg1.store(static_cast<std::int32_t>(p->call(static_cast<std::int32_t>(-1))));
                g_echo_int_zero.store(static_cast<std::int32_t>(p->call(static_cast<std::int32_t>(0))));
            }
        }
        {
            auto p{ s.get_method("echoLongId") };
            if (p.has_value())
            {
                g_echo_long_min.store(static_cast<std::int64_t>(p->call(k_long_min)));
                g_echo_long_max.store(static_cast<std::int64_t>(p->call(k_long_max)));
                g_echo_long_neg1.store(static_cast<std::int64_t>(p->call(static_cast<std::int64_t>(-1))));
            }
        }

        // ───────── float / double ARG round-trips (F/D marshaller arm) ──────────
        // echoFloat/echoDouble round-trip an F/D arg AND decode the F/D return in
        // one call.  Captured as raw bits so a NaN arg survives.
        {
            auto p{ s.get_method("echoFloat") };
            if (p.has_value())
            {
                g_echo_float_bits.store(f2bits(static_cast<float>(p->call(k_echo_float))));
                g_echo_float_nan_bits.store(
                    f2bits(static_cast<float>(p->call(std::numeric_limits<float>::quiet_NaN()))));
            }
        }
        {
            auto p{ s.get_method("echoDouble") };
            if (p.has_value())
            {
                g_echo_double_bits.store(d2bits(static_cast<double>(p->call(k_echo_double))));
            }
            g_fd_arg_captured.store(true);
        }
        {
            auto p{ mcd_fixture::static_method("sEchoFloat") };
            if (p.has_value())
            {
                g_s_echo_float_bits.store(f2bits(static_cast<float>(p->call(k_echo_float))));
            }
        }
        {
            auto p{ mcd_fixture::static_method("sEchoDouble") };
            if (p.has_value())
            {
                g_s_echo_double_bits.store(d2bits(static_cast<double>(p->call(k_echo_double))));
            }
        }

        // ───────── narrow / wide primitive ARG recorders (per marshaller arm) ───
        // bool(Z), byte(B narrow out.i), char(C narrow out.i zero-ext), short(S
        // narrow out.i sign-ext), float(F), double(D) — each body publishes a
        // pure-int / raw-bit measurement of the JVM-side value.
        {
            auto p{ s.get_method("recordBool") };
            if (p.has_value()) { p->call(true); ++g_arg_runs; }
        }
        {
            auto p{ s.get_method("recordByte") };
            if (p.has_value())
            {
                p->call(static_cast<std::int8_t>(k_arg_byte_int));
                ++g_arg_runs;
            }
        }
        {
            auto p{ s.get_method("recordChar") };
            if (p.has_value())
            {
                p->call(static_cast<std::uint16_t>(k_arg_char_int)); // 0xFFFF
                ++g_arg_runs;
            }
        }
        {
            auto p{ s.get_method("recordShort") };
            if (p.has_value())
            {
                p->call(static_cast<std::int16_t>(k_arg_short_int));
                ++g_arg_runs;
            }
        }
        {
            auto p{ s.get_method("recordFloat") };
            if (p.has_value()) { p->call(k_arg_float); ++g_arg_runs; }
        }
        {
            auto p{ s.get_method("recordDouble") };
            if (p.has_value()) { p->call(k_arg_double); ++g_arg_runs; }
        }

        // ───────── many-arg / all-two-slot ARG shapes (slot-array layout) ───────
        // result = a + c + f + b + e + (long)d ; six args mixing single+two-slot.
        {
            auto p{ s.get_method("sixArg") };
            if (p.has_value())
            {
                g_six_arg_ret.store(static_cast<std::int64_t>(
                    p->call(k_six_a, k_six_b, k_six_c, k_six_d, k_six_e, k_six_f)));
            }
        }
        {
            auto p{ mcd_fixture::static_method("sSixArg") };
            if (p.has_value())
            {
                g_s_six_arg_ret.store(static_cast<std::int64_t>(
                    p->call(k_six_a, k_six_b, k_six_c, k_six_d, k_six_e, k_six_f)));
            }
        }
        // result = a + c + (long)b - (long)d ; four CONSECUTIVE two-slot args.
        {
            auto p{ s.get_method("fourWide") };
            if (p.has_value())
            {
                g_four_wide_ret.store(static_cast<std::int64_t>(
                    p->call(k_fw_a, k_fw_b, k_fw_c, k_fw_d)));
            }
        }

        // ───────── null-reference ARG contract (null -> Java null) ──────────────
        // A null const char* and a null unique_ptr must reach the JVM as a genuine
        // null reference.  We pass a default-constructed null unique_ptr and a
        // null C-string and assert the body observed null.
        {
            auto p{ s.get_method("consumeNullableString") };
            if (p.has_value())
            {
                const char* const null_cstr{ nullptr };
                p->call(null_cstr);
                g_null_str_ran.store(1);
            }
        }
        {
            auto p{ s.get_method("consumeNullableObject") };
            if (p.has_value())
            {
                std::unique_ptr<mcd_fixture> null_obj{};
                p->call(null_obj);
                g_null_obj_ran.store(1);
            }
        }

        // ───────── float / double SPECIAL-VALUE returns (IEEE-754 fidelity) ─────
        {
            auto pn{ s.get_method("retFloatNaN") };
            auto pi{ s.get_method("retFloatPosInf") };
            auto pj{ s.get_method("retFloatNegInf") };
            auto pz{ s.get_method("retFloatNegZero") };
            auto pd{ s.get_method("retFloatDenormal") };
            if (pn.has_value()) { g_fret_nan_bits.store(f2bits(static_cast<float>(pn->call()))); }
            if (pi.has_value()) { g_fret_posinf_bits.store(f2bits(static_cast<float>(pi->call()))); }
            if (pj.has_value()) { g_fret_neginf_bits.store(f2bits(static_cast<float>(pj->call()))); }
            if (pz.has_value()) { g_fret_negzero_bits.store(f2bits(static_cast<float>(pz->call()))); }
            if (pd.has_value()) { g_fret_denorm_bits.store(f2bits(static_cast<float>(pd->call()))); }
            g_fret_special_captured.store(true);
        }
        {
            auto pn{ s.get_method("retDoubleNaN") };
            auto pi{ s.get_method("retDoublePosInf") };
            auto pz{ s.get_method("retDoubleNegZero") };
            auto pd{ s.get_method("retDoubleDenormal") };
            if (pn.has_value()) { g_dret_nan_bits.store(d2bits(static_cast<double>(pn->call()))); }
            if (pi.has_value()) { g_dret_posinf_bits.store(d2bits(static_cast<double>(pi->call()))); }
            if (pz.has_value()) { g_dret_negzero_bits.store(d2bits(static_cast<double>(pz->call()))); }
            if (pd.has_value()) { g_dret_denorm_bits.store(d2bits(static_cast<double>(pd->call()))); }
            g_dret_special_captured.store(true);
        }
        {
            auto pn{ mcd_fixture::static_method("sRetFloatNaN") };
            auto pi{ mcd_fixture::static_method("sRetDoublePosInf") };
            auto pz{ mcd_fixture::static_method("sRetDoubleNegZero") };
            if (pn.has_value()) { g_s_fret_nan_bits.store(f2bits(static_cast<float>(pn->call()))); }
            if (pi.has_value()) { g_s_dret_posinf_bits.store(d2bits(static_cast<double>(pi->call()))); }
            if (pz.has_value()) { g_s_dret_negzero_bits.store(d2bits(static_cast<double>(pz->call()))); }
            g_s_special_captured.store(true);
        }

        // ───────── additional primitive-array returns (per-stride '[' arm) ──────
        // byte[] { -1, 0, 127, -128 } — 1-byte stride, sign-extending read.
        {
            auto p{ s.get_method("retByteArray") };
            if (p.has_value())
            {
                void* const arr{ static_cast<void*>(p->call()) };
                if (arr != nullptr)
                {
                    g_byte_arr_len.store(vmhook::array_length(arr));
                    g_byte_arr_e0.store(vmhook::get_array_element<std::int8_t>(arr, 0));
                    g_byte_arr_e2.store(vmhook::get_array_element<std::int8_t>(arr, 2));
                    g_byte_arr_e3.store(vmhook::get_array_element<std::int8_t>(arr, 3));
                }
            }
        }
        // boolean[] { true, false, true } — 1-byte stride (0/1).
        {
            auto p{ s.get_method("retBoolArray") };
            if (p.has_value())
            {
                void* const arr{ static_cast<void*>(p->call()) };
                if (arr != nullptr)
                {
                    g_bool_arr_len.store(vmhook::array_length(arr));
                    g_bool_arr_e0.store(vmhook::get_array_element<std::uint8_t>(arr, 0) != 0 ? 1 : 0);
                    g_bool_arr_e1.store(vmhook::get_array_element<std::uint8_t>(arr, 1) != 0 ? 1 : 0);
                }
            }
        }
        // short[] { -32768, 0, 32767 } — 2-byte stride, sign.
        {
            auto p{ s.get_method("retShortArray") };
            if (p.has_value())
            {
                void* const arr{ static_cast<void*>(p->call()) };
                if (arr != nullptr)
                {
                    g_short_arr_len.store(vmhook::array_length(arr));
                    g_short_arr_e0.store(vmhook::get_array_element<std::int16_t>(arr, 0));
                    g_short_arr_e2.store(vmhook::get_array_element<std::int16_t>(arr, 2));
                }
            }
        }
        // char[] { 'A', 0xFFFF, '0' } — 2-byte stride, zero-extend.
        {
            auto p{ s.get_method("retCharArray") };
            if (p.has_value())
            {
                void* const arr{ static_cast<void*>(p->call()) };
                if (arr != nullptr)
                {
                    g_char_arr_len.store(vmhook::array_length(arr));
                    g_char_arr_e0.store(vmhook::get_array_element<std::uint16_t>(arr, 0));
                    g_char_arr_e1.store(vmhook::get_array_element<std::uint16_t>(arr, 1));
                    g_char_arr_e2.store(vmhook::get_array_element<std::uint16_t>(arr, 2));
                }
            }
        }
        // double[] { 1.5, -2.5, 1024.0 } — 8-byte stride, IEEE bits.
        {
            auto p{ s.get_method("retDoubleArray") };
            if (p.has_value())
            {
                void* const arr{ static_cast<void*>(p->call()) };
                if (arr != nullptr)
                {
                    g_dbl_arr_len.store(vmhook::array_length(arr));
                    g_dbl_arr_e0_bits.store(d2bits(vmhook::get_array_element<double>(arr, 0)));
                    g_dbl_arr_e2_bits.store(d2bits(vmhook::get_array_element<double>(arr, 2)));
                    g_dbl_arr_captured.store(true);
                }
            }
        }
        // float[] { 0.5, -0.5, 3.5, -3.5 } — 4-byte stride, IEEE bits.
        {
            auto p{ s.get_method("retFloatArray") };
            if (p.has_value())
            {
                void* const arr{ static_cast<void*>(p->call()) };
                if (arr != nullptr)
                {
                    g_flt_arr_len.store(vmhook::array_length(arr));
                    g_flt_arr_e0_bits.store(f2bits(vmhook::get_array_element<float>(arr, 0)));
                    g_flt_arr_e1_bits.store(f2bits(vmhook::get_array_element<float>(arr, 1)));
                    g_flt_arr_captured.store(true);
                }
            }
        }

        // ───────── multi-arg (int, long, double) — two-slot args ─────────
        // Expected return = i + j + (long)d.
        g_sum_ild.store(static_cast<std::int64_t>(
            s.get_method("sumILD")->call(k_sum_i, k_sum_j, k_sum_d)));
        g_s_sum_ild.store(static_cast<std::int64_t>(
            mcd_fixture::static_method("sSumILD")->call(k_sum_i, k_sum_j, k_sum_d)));

        // ───────── String returns ─────────
        {
            auto p{ s.get_method("retString") };
            if (p.has_value())
            {
                const auto v{ p->call() };
                g_str_inst           = v.as_string();
                g_str_inst_is_string.store(v.is_string());
            }
        }
        {
            auto p{ mcd_fixture::static_method("sRetString") };
            if (p.has_value())
            {
                const auto v{ p->call() };
                g_str_static           = v.as_string();
                g_str_static_is_string.store(v.is_string());
            }
        }
        g_str_captured.store(true);

        // ───────── DEEPEN (A cont.): String returns are NOT void ─────────
        {
            auto p{ s.get_method("retString") };
            if (p.has_value()) { g_str_inst_not_void.store(p->call().is_void() ? 0 : 1); }
        }
        {
            auto p{ mcd_fixture::static_method("sRetString") };
            if (p.has_value()) { g_str_static_not_void.store(p->call().is_void() ? 0 : 1); }
        }

        // ───────── String-arg round-trips (echoString) ─────────
        {
            auto p{ s.get_method("echoString") };
            if (p.has_value())
            {
                g_echo_str_ascii   = p->call(std::string{ "round-trip-jni-987" }).as_string();
                g_echo_str_empty   = p->call(std::string{}).as_string();
                // Latin-1 char café: round-trips through NewStringUTF (modified
                // UTF-8 out) and GetStringUTFChars (modified UTF-8 back) on the
                // call_jni path, so the bytes come back identical there.
                g_echo_str_unicode = p->call(std::string{ "caf\xC3\xA9" }).as_string();
            }
        }
        g_echo_str_captured.store(true);

        // ───────── String arg -> void body ─────────
        {
            auto p{ s.get_method("consumeString") };
            if (p.has_value())
            {
                const auto v{ p->call(std::string{ "void-string-arg-jni" }) };
                g_consume_str_is_void.store(v.is_void() ? 1 : 0);
            }
        }

        // ───────── Object arg -> void body (pass the live receiver) ─────────
        {
            auto p{ s.get_method("consumeObject") };
            if (p.has_value())
            {
                const auto v{ p->call(*self) };
                g_consume_obj_is_void.store(v.is_void() ? 1 : 0);
            }
        }

        // ───────── Object returns (identity + null contract) ─────────
        {
            // copy-init (=), NOT brace-init: value_t's templated conversion
            // operator makes std::unique_ptr<T>{ value_t } ambiguous under MSVC
            // (C2440).  Copy-init resolves the user-defined conversion cleanly.
            std::unique_ptr<mcd_fixture> sp = s.get_method("retSelf")->call();
            g_self_nonnull.store(sp != nullptr);
            if (sp)
            {
                g_self_instance.store(
                    reinterpret_cast<std::uintptr_t>(sp->get_instance()));
            }
            auto p{ s.get_method("retSelf") };
            if (p.has_value())
            {
                g_self_is_void.store(p->call().is_void() ? 1 : 0);
            }
        }
        {
            std::unique_ptr<mcd_fixture> np = s.get_method("retNullObject")->call();  // copy-init (MSVC C2440)
            g_null_obj_is_null.store(np == nullptr);
            auto p{ s.get_method("retNullObject") };
            if (p.has_value())
            {
                g_null_obj_is_void.store(p->call().is_void() ? 1 : 0);
            }
        }
        {
            auto sm{ mcd_fixture::static_method("sRetSingleton") };
            if (sm.has_value())
            {
                std::unique_ptr<mcd_fixture> sp = sm->call();
                g_static_self_nonnull.store(sp != nullptr);
                if (sp)
                {
                    g_static_self_instance.store(
                        reinterpret_cast<std::uintptr_t>(sp->get_instance()));
                }
            }
            auto sn{ mcd_fixture::static_method("sRetNullObject") };
            if (sn.has_value())
            {
                std::unique_ptr<mcd_fixture> sp = sn->call();
                g_static_null_is_null.store(sp == nullptr);
            }
        }
        // Array reference return ('[' descriptor): decode to a non-null oop via
        // the value_t void* conversion (decode_oop_pointer), without walking it.
        // Then read its length + every element value-correctly via the crash-safe
        // library array readers (no new raw deref) to prove the '[' arm landed on
        // the REAL int[] { 11, 22, 33 }.
        {
            auto p{ s.get_method("retIntArray") };
            if (p.has_value())
            {
                const auto v{ p->call() };
                void* const arr{ static_cast<void*>(v) };
                g_array_nonnull.store(arr != nullptr);
                if (arr != nullptr)
                {
                    g_int_arr_len.store(vmhook::array_length(arr));
                    g_int_arr_e0.store(vmhook::get_array_element<std::int32_t>(arr, 0));
                    g_int_arr_e1.store(vmhook::get_array_element<std::int32_t>(arr, 1));
                    g_int_arr_e2.store(vmhook::get_array_element<std::int32_t>(arr, 2));
                }
            }
        }
        // long[] return — 8-byte stride; value-correct read of the boundary
        // elements proves get_array_element<int64_t> stride math + the '[' arm.
        {
            auto p{ s.get_method("retLongArray") };
            if (p.has_value())
            {
                const auto v{ p->call() };
                void* const arr{ static_cast<void*>(v) };
                if (arr != nullptr)
                {
                    g_long_arr_len.store(vmhook::array_length(arr));
                    g_long_arr_e0.store(vmhook::get_array_element<std::int64_t>(arr, 0));
                    g_long_arr_e3.store(vmhook::get_array_element<std::int64_t>(arr, 3));
                }
            }
        }
        // String[] return ('[Ljava/lang/String;') — object-element descriptor on
        // the '[' arm.  Assert non-null decode + exact length (object elements
        // are not walked; the header length read proves the decode landed right).
        {
            auto p{ s.get_method("retStringArray") };
            if (p.has_value())
            {
                const auto v{ p->call() };
                void* const arr{ static_cast<void*>(v) };
                g_str_arr_nonnull.store(arr != nullptr);
                if (arr != nullptr)
                {
                    g_str_arr_len.store(vmhook::array_length(arr));
                }
            }
        }

        // ═════════ String-ARG NUL / astral round-trip INTO the JVM (#27) ════════
        // The arg path uses length-counted UTF-16 (NewString), so an interior NUL
        // and an astral scalar must arrive in the JVM verbatim — proven by the
        // fixture's pure-int measurements, which no string DECODE can distort.
        // (The String-RETURN decode via GetStringUTFChars cannot itself round-trip
        // these, so a plain echo+compare would be a false negative for the arg
        // path; these int observations are the correct, decode-independent proof.)
        {
            auto p{ s.get_method("recordString") };
            if (p.has_value())
            {
                // "a\0b\0c" — three interior NULs; char len 5, code-point count 5.
                const std::string nul_payload{ std::string("a\0b\0c", 5) };
                p->call(nul_payload);
                g_nul_arg_captured.store(mcd_fixture::record_string_called() ? 1 : 0);
                g_nul_arg_char_len.store(mcd_fixture::record_string_char_len());
                g_nul_arg_cp_count.store(mcd_fixture::record_string_cp_count());
                g_nul_arg_hash.store(mcd_fixture::record_string_hash());
            }
        }
        {
            auto p{ s.get_method("recordString") };
            if (p.has_value())
            {
                // U+1F600 GRINNING FACE, standard UTF-8 F0 9F 98 80 — one astral
                // scalar = TWO UTF-16 units (surrogate pair).  Sandwiched between
                // ASCII so a truncation/mangle is unmistakable.
                const std::string astral_payload{ "X\xF0\x9F\x98\x80Y" };
                p->call(astral_payload);
                g_astral_arg_captured.store(mcd_fixture::record_string_called() ? 1 : 0);
                g_astral_arg_char_len.store(mcd_fixture::record_string_char_len());
                g_astral_arg_cp_count.store(mcd_fixture::record_string_cp_count());
                g_astral_arg_first_cp.store(mcd_fixture::record_string_first_cp());
                g_astral_arg_last_cp.store(mcd_fixture::record_string_last_cp());
            }
        }

        // ═════════ TIGHT LOOP 1: String-RETURN local-ref discipline ═════════
        // Every loopString() return is a JNI local ref that call_jni decodes to
        // UTF-8 and releases.  A leak would starve the 16-entry table within
        // ~16 iterations; once starved, later calls return "" -> a 2nd distinct
        // value.  distinct == 1 proves stable, leak-free String-return decoding.
        {
            constexpr int iters{ 256 };
            std::string first{};
            bool have_first{ false };
            int distinct{ 0 };
            auto proxy{ s.get_method("loopString") };
            for (int i{ 0 }; i < iters; ++i)
            {
                if (!proxy.has_value())
                {
                    break;
                }
                const std::string r{ proxy->call().as_string() };
                if (!have_first)
                {
                    first = r;
                    have_first = true;
                    distinct = 1;
                }
                else if (r != first)
                {
                    ++distinct;
                }
            }
            g_ret_loop_iters.store(iters);
            g_ret_loop_distinct.store(distinct);
        }

        // ═════════ TIGHT LOOP 2: String-ARG local-ref discipline ═════════
        // Every echoString(s) marshals a NewStringUTF local ref (released by
        // the arg-cleanup RAII) and returns a String local ref (released by the
        // String-return decode).  Two refs per iteration -> the table starves
        // twice as fast if either release is missing.  We assert every echo
        // round-trips to the exact input; a starved table yields "" mismatches.
        {
            constexpr int iters{ 256 };
            const std::string payload{ "arg-loop-payload-42" };
            int mismatches{ 0 };
            auto proxy{ s.get_method("echoString") };
            for (int i{ 0 }; i < iters; ++i)
            {
                if (!proxy.has_value())
                {
                    mismatches = iters;
                    break;
                }
                const std::string r{ proxy->call(payload).as_string() };
                if (r != payload)
                {
                    ++mismatches;
                }
            }
            g_arg_loop_iters.store(iters);
            g_arg_loop_mismatches.store(mismatches);
        }

        // ═════════ TIGHT LOOP 3: long+double MULTI-ARG primitive calls ═════════
        // twoSlot(long a, double b, long c) -> a + c + (long)b.  This is the
        // union-aliasing footgun: each jvalue cell holds a primitive (jlong /
        // jdouble) whose .l alias is a non-null garbage pointer; the arg-cleanup
        // MUST NOT hand it to DeleteLocalRef.  A regression there corrupts the
        // thread's local-ref state and later iterations diverge (or crash).  We
        // assert every iteration returns the exact arithmetic result and the
        // recorded args match — stable across 256 iterations proves the
        // primitive cells are never released and state never corrupts.
        {
            constexpr int iters{ 256 };
            const std::int64_t a{ static_cast<std::int64_t>(0x4242424242424242LL) };
            const double       b{ 1024.0 };
            const std::int64_t c{ static_cast<std::int64_t>(0x0000111122223333LL) };
            const std::int64_t expected{ a + c + static_cast<std::int64_t>(b) };
            int mismatches{ 0 };
            auto proxy{ s.get_method("twoSlot") };
            for (int i{ 0 }; i < iters; ++i)
            {
                if (!proxy.has_value())
                {
                    mismatches = iters;
                    break;
                }
                const std::int64_t r{ proxy->call(a, b, c) };
                if (r != expected)
                {
                    ++mismatches;
                }
            }
            g_two_loop_iters.store(iters);
            g_two_loop_mismatches.store(mismatches);
        }

        // ═════════ NON-CORRUPTION: value-returning call after all loops ═════════
        {
            auto p{ s.get_method("echoInt") };
            if (p.has_value())
            {
                const std::int32_t r{ p->call(k_post_echo) };
                g_post_loop_echo.store(r);
            }
        }

        // ═════════ STATIC METHOD resolved through the INSTANCE wrapper ═════════
        // get_method("sX") on the instance `s` returns a proxy bound to the live
        // receiver (this->object != null) whose Method* is ACC_STATIC.  These
        // calls reproduce the CI-failure bug exactly; the asserted results below
        // would be wrong (or monostate, and the JVM possibly torn down) before
        // the call()/call_jni static-detection fix.
        {
            // No-arg static via instance proxy: sRetInt() == Integer.MIN_VALUE.
            // Under the bug this took the instance path (GetMethodID can't see a
            // static -> monostate -> 0), so a correct -2147483648 proves static
            // dispatch fired off the Method's declaring class, not the receiver.
            auto p_ret{ s.get_method("sRetInt") };
            if (p_ret.has_value())
            {
                g_svia_is_static.store(p_ret->is_static() ? 1 : 0);
                // The proxy MUST still carry the receiver OOP (that's the whole
                // point — it came from an instance wrapper); the fix must make it
                // dispatch static *despite* a non-null receiver.
                g_svia_has_receiver.store(p_ret->get_compressed_oop() != 0u ? 1 : 0);
                g_svia_ret_int.store(static_cast<std::int32_t>(p_ret->call()));
            }

            // Single-arg static via instance proxy: sEchoInt(v) == v.  This is the
            // sharpest discriminator for the "receiver bound as first arg" mis-
            // dispatch: if the live instance were pinned into slot 0 / arg 0, the
            // echoed value would be the receiver's bits, never our sentinel.
            auto p_echo{ s.get_method("sEchoInt") };
            if (p_echo.has_value())
            {
                g_svia_echo_int.store(static_cast<std::int32_t>(p_echo->call(
                    static_cast<std::int32_t>(k_svia_echo))));
            }

            // Multi-arg (I,J,D) static via instance proxy: a receiver shift would
            // corrupt every two-slot argument, so the exact arithmetic sum is a
            // strong proof the argument block was laid out with NO receiver.
            auto p_sum{ s.get_method("sSumILD") };
            if (p_sum.has_value())
            {
                g_svia_sum_ild.store(static_cast<std::int64_t>(
                    p_sum->call(k_sum_i, k_sum_j, k_sum_d)));
            }

            // The (name, signature) instance overload must behave identically.
            auto p_sig{ s.get_method("sEchoInt", "(I)I") };
            if (p_sig.has_value())
            {
                g_svia_echo_sig.store(static_cast<std::int32_t>(p_sig->call(
                    static_cast<std::int32_t>(k_svia_echo))));
            }

            // Non-corruption: an ordinary INSTANCE call AFTER the static-via-
            // instance dispatches must still deliver its argument intact (proves
            // the receiver/JNI state was not poisoned by the static calls).  We
            // use echoLong here (returns its long arg, NO lastEchoArg side effect)
            // so this follow-up cannot clobber the field that the unrelated
            // mcj_echo_int_side_effect assertion reads.
            auto p_follow{ s.get_method("echoLong") };
            if (p_follow.has_value())
            {
                g_svia_followup_inst_echo.store(
                    static_cast<std::int64_t>(p_follow->call(k_svia_follow)));
            }
        }

        // ═════════ NONVIRTUAL dispatch: re-invoke <init> via slot 93 ═══════════
        // The only CallNonvirtualVoidMethodA (invokespecial) path in the whole
        // library is call_jni's 'V' arm for a void <init>/<clinit> on an INSTANCE
        // proxy.  We resolve the no-arg constructor on the live receiver and
        // call() it: that re-runs MethodCallDispatch() (which only bumps ctorCalls) via
        // the nonvirtual slot, so the counter advancing by exactly one — with no
        // pending exception and a void result — proves the nonvirtual constructor
        // dispatch fired correctly and left the thread clean.
        {
            auto p_ctor{ s.get_method("<init>", "()V") };
            g_ctor_proxy_found.store(p_ctor.has_value() ? 1 : 0);
            if (p_ctor.has_value())
            {
                g_ctor_calls_before.store(mcd_fixture::ctor_calls());
                const auto v{ p_ctor->call() };
                g_ctor_call_is_void.store(v.is_void() ? 1 : 0);
                g_ctor_calls_after.store(mcd_fixture::ctor_calls());
                // call() reports a callee throw through the returned value_t, so
                // "the constructor dispatch left the thread clean" is a real
                // readback of that flag — not an assumption.
                g_ctor_no_pending_exc.store(v.threw() ? 0 : 1);
            }
        }

        // ═════════ EXCEPTION DISCIPLINE: callee throws -> reported + cleared ════
        // A Java method that throws must NOT leave the exception parked on the
        // JavaThread, where the next Java code to run observes it and it
        // propagates out of a frame that never made the call (that is what let a
        // sibling module's exception crash the JVM).  call() reads
        // ThreadShadow::_pending_exception, names the exception klass, clears the
        // slot with a plain pure-VM write, and hands the throw back through the
        // returned value_t: threw() set, exception_class populated, and the
        // result VALUE-INITIALISED for the declared return type rather than the
        // stub's garbage result slot.
        //
        // So the whole discipline is observable from the RETURN VALUE — no
        // ExceptionCheck, no thread probe, no defensive clear of our own.  We
        // record it for instance-void, instance-int (the value path, where the
        // zeroed result is the interesting part) and static-void throwers.  Run
        // LAST so a mis-handled throw cannot affect any earlier assertion.
        {
            int seen_thrown{ 0 };

            auto p_tv{ s.get_method("throwVoid") };
            if (p_tv.has_value())
            {
                const auto v{ p_tv->call() };
                g_exc_void_threw.store(v.threw() ? 1 : 0);
                g_exc_void_class = v.exception_class;
                if (v.threw()) { ++seen_thrown; }
            }

            auto p_ti{ s.get_method("throwReturningInt") };
            if (p_ti.has_value())
            {
                const auto v{ p_ti->call() };
                g_exc_int_threw.store(v.threw() ? 1 : 0);
                g_exc_int_class = v.exception_class;
                // The value-initialised-on-throw contract: int -> exactly 0.
                g_exc_int_result_is_zero.store(
                    static_cast<std::int32_t>(v) == 0 ? 1 : 0);
                if (v.threw()) { ++seen_thrown; }
            }

            auto p_ts{ mcd_fixture::static_method("sThrowVoid") };
            if (p_ts.has_value())
            {
                const auto v{ p_ts->call() };
                g_exc_static_threw.store(v.threw() ? 1 : 0);
                g_exc_static_class = v.exception_class;
                if (v.threw()) { ++seen_thrown; }
            }

            g_exc_seen_at_least_one.store(seen_thrown > 0 ? 1 : 0);

            // Hard recovery proof: a normal value-returning call after all the
            // throws must still deliver its argument intact — the throwing calls
            // did not corrupt the thread's JNI state.  We deliberately use
            // echoLong (returns its long arg, NO lastEchoArg side effect) so this
            // recovery call cannot clobber the field the sibling
            // mcj_echo_int_side_effect assertion reads (same discipline as the
            // static-via-instance follow-up above).
            auto p_rec{ s.get_method("echoLong") };
            if (p_rec.has_value())
            {
                // copy-init via static_cast (NOT brace-init from value_t): the
                // deducing-this/MSVC conversion is unambiguous this way.
                const std::int64_t r =
                    static_cast<std::int64_t>(p_rec->call(static_cast<std::int64_t>(k_exc_recovery)));
                g_exc_recovery_ok.store(r == static_cast<std::int64_t>(k_exc_recovery) ? 1 : 0);
            }
            ((void)0);
        }
    }
}

VMHOOK_JVM_MODULE(method_call_dispatch)
{
    vmhook::register_class<mcd_fixture>("vmhook/fixtures/MethodCallDispatch");

    // Record which dispatch path the live JDK uses.  On JDK 21+ (and any JDK
    // that does not export StubRoutines::_call_stub_entry via VMStructs) this is
    // the JNI fallback — the path this module targets.
    g_call_stub_present.store(vmhook::detail::find_call_stub_entry() != nullptr,
                              std::memory_order_relaxed);

    {
        auto handle{ vmhook::scoped_hook<mcd_fixture>(
            "trigger",
            [](vmhook::return_value&,
               const std::unique_ptr<mcd_fixture>& self,
               std::int32_t /*delta*/)
            {
                g_detour_calls.fetch_add(1, std::memory_order_relaxed);
                g_detour_saw_self.store(self != nullptr, std::memory_order_relaxed);
                run_all(self);
            }) };

        ctx.check("mcj_hook_installed", handle.installed());

        const bool done{ ctx.run_probe(
            [](bool v) { mcd_fixture::set_go(v); },
            []() { return mcd_fixture::get_done(); }) };

        ctx.check("mcj_probe_completed", done);
        ctx.check("mcj_detour_fired", g_detour_calls.load(std::memory_order_relaxed) >= 1);
        ctx.check("mcj_detour_saw_self", g_detour_saw_self.load(std::memory_order_relaxed));
        ctx.check("mcj_trigger_count_advanced", mcd_fixture::get_trigger_count() >= 1);

        const bool stub{ g_call_stub_present.load(std::memory_order_relaxed) };
        ctx.record(std::string{ "[INFO] method_call_dispatch path: " }
                   + (stub ? "call_stub (find_call_stub_entry resolved)"
                           : "NONE - find_call_stub_entry returned null, call() cannot dispatch"));

        // HARD assertion that call() has a live dispatcher.  This check used to
        // assert the OPPOSITE (`!stub`) on the premise that no JDK exports
        // StubRoutines::_call_stub_entry via VMStructs, so call() must be running
        // the JNI fallback.  The premise was wrong in both halves: _call_stub_entry
        // was never published on ANY JDK, and the entry is now DERIVED from
        // StubRoutines::_call_stub_return_address instead — so it resolves on 8,
        // 21 and 26 alike.  The JNI fallback it was guarding no longer exists.
        // What is worth asserting HARD is the same thing it was really protecting:
        // that every value assertion below ran through a real dispatcher rather
        // than a silently no-op call().
        ctx.check("mcj_call_stub_is_the_live_path", stub);

        // ═════════════════════ INSTANCE primitive returns ═════════════════════
        ctx.check("mcj_bool_true_instance",  g_bool_true.load()  == 1);
        ctx.check("mcj_bool_false_instance", g_bool_false.load() == 0);
        ctx.check("mcj_byte_neg7_sign_extends", g_byte.load() == -7);
        ctx.check("mcj_char_Z_90",   g_char.load() == 90);
        ctx.check("mcj_char_max_65535", g_char_max.load() == 65535);
        ctx.check("mcj_char_max_zero_extends_to_int_65535", g_char_max_as_int.load() == 65535);
        ctx.check("mcj_short_neg12345_sign_extends", g_short.load() == -12345);
        ctx.check("mcj_int_badf00d", g_int.load() == k_int_ret);
        ctx.check("mcj_long_pattern", g_long.load() == k_long_ret);
        ctx.check("mcj_float_captured", g_float_captured.load());
        {
            float f{ 0.0f };
            const std::uint32_t b{ g_float_bits.load() };
            std::memcpy(&f, &b, sizeof(f));
            ctx.check("mcj_float_3_5_exact", f == 3.5f);
        }
        ctx.check("mcj_double_captured", g_double_captured.load());
        {
            double d{ 0.0 };
            const std::uint64_t b{ g_double_bits.load() };
            std::memcpy(&d, &b, sizeof(d));
            ctx.check("mcj_double_e_exact", d == 2.718281828459045);
        }
        ctx.check("mcj_void_instance_is_void", g_void_is_void.load() == 1);
        ctx.check("mcj_void_instance_side_effect", mcd_fixture::void_instance_hits() == 1);

        // ═════════════════════ STATIC primitive returns ═══════════════════════
        ctx.check("mcj_static_bool_true",  g_s_bool_true.load() == 1);
        ctx.check("mcj_static_byte_99",    g_s_byte.load() == 99);
        ctx.check("mcj_static_char_k_107", g_s_char.load() == 107);
        ctx.check("mcj_static_short_20000", g_s_short.load() == 20000);
        ctx.check("mcj_static_int_min", g_s_int.load() == -2147483648LL);
        ctx.check("mcj_static_long_max", g_s_long.load() == 0x7FFFFFFFFFFFFFFFLL);
        ctx.check("mcj_static_float_captured", g_s_float_captured.load());
        {
            float f{ 0.0f };
            const std::uint32_t b{ g_s_float_bits.load() };
            std::memcpy(&f, &b, sizeof(f));
            ctx.check("mcj_static_float_neg_half", f == -0.5f && std::signbit(f));
        }
        ctx.check("mcj_static_double_captured", g_s_double_captured.load());
        {
            double d{ 0.0 };
            const std::uint64_t b{ g_s_double_bits.load() };
            std::memcpy(&d, &b, sizeof(d));
            ctx.check("mcj_static_double_neg_1_5", d == -1.5);
        }
        ctx.check("mcj_static_void_is_void", g_s_void_is_void.load() == 1);
        ctx.check("mcj_static_void_side_effect", mcd_fixture::void_static_hits() == 1);

        // ═════════════════════ single-arg primitive echoes ════════════════════
        ctx.check("mcj_echo_int_passthrough", g_echo_int.load() == k_echo_int);
        ctx.check("mcj_echo_int_side_effect", mcd_fixture::last_echo_arg() == k_post_echo); // last echo was the post-loop one
        ctx.check("mcj_echo_long_passthrough", g_echo_long.load() == k_echo_long);
        ctx.check("mcj_static_echo_int_passthrough", g_s_echo_int.load() == k_echo_int);

        // ───── call_jni cache mis-keying (library #3): a reused name-only proxy
        // must re-resolve per overload.  combo(5)=105 then combo("hi")="hi!" through
        // ONE proxy, and the reverse order through another — pre-fix the 2nd call in
        // each pair returned the wrong type/value from the 1st overload's cache.
        ctx.check("mcj_combo_int_first_returns_105", g_combo_int_first.load() == 105);
        ctx.check("mcj_combo_str_after_int_returns_hibang", g_combo_str_after_int.load() == 1);
        ctx.check("mcj_combo_str_first_returns_yobang", g_combo_str_first.load() == 1);
        ctx.check("mcj_combo_int_after_str_returns_107", g_combo_int_after_str.load() == 107);

        // ═════════════════════ multi-arg (I,J,D) two-slot args ════════════════
        // result = i + j + (long)d ; proves long + double both landed correctly.
        const std::int64_t expected_sum{ static_cast<std::int64_t>(k_sum_i)
                                         + k_sum_j + static_cast<std::int64_t>(k_sum_d) };
        ctx.check("mcj_multi_arg_return_correct", g_sum_ild.load() == expected_sum);
        ctx.check("mcj_static_multi_arg_return_correct", g_s_sum_ild.load() == expected_sum);
        // And each argument arrived verbatim at the (instance) body.
        ctx.check("mcj_multi_arg_called", mcd_fixture::multi_prim_called());
        ctx.check("mcj_multi_arg_int", mcd_fixture::multi_arg_int() == k_sum_i);
        ctx.check("mcj_multi_arg_long", mcd_fixture::multi_arg_long() == k_sum_j);
        ctx.check("mcj_multi_arg_double", mcd_fixture::multi_arg_double() == k_sum_d);

        // ═════════════════════ boundary primitive-arg echoes ══════════════════
        // INT_MIN / INT_MAX / -1 / 0 and LONG_MIN / LONG_MAX / -1 round-trip
        // exactly through the narrow (out.i) and wide (out.j) marshaller arms.
        ctx.check("mcj_echo_int_min", g_echo_int_min.load() == k_int_min);
        ctx.check("mcj_echo_int_max", g_echo_int_max.load() == k_int_max);
        ctx.check("mcj_echo_int_neg1", g_echo_int_neg1.load() == -1);
        ctx.check("mcj_echo_int_zero", g_echo_int_zero.load() == 0);
        ctx.check("mcj_echo_long_min", g_echo_long_min.load() == k_long_min);
        ctx.check("mcj_echo_long_max", g_echo_long_max.load() == k_long_max);
        ctx.check("mcj_echo_long_neg1", g_echo_long_neg1.load() == -1);

        // ═════════════════════ float / double ARG round-trips ═════════════════
        // The F / D marshaller arm AND the F / D return decode in one call each.
        ctx.check("mcj_fd_arg_captured", g_fd_arg_captured.load());
        {
            float f{ 0.0f };
            const std::uint32_t b{ g_echo_float_bits.load() };
            std::memcpy(&f, &b, sizeof(f));
            ctx.check("mcj_echo_float_arg_exact", f == k_echo_float);
        }
        // A NaN float arg must round-trip as a NaN (payload may be canonicalized
        // by the JVM, so assert NaN-ness via the bit pattern's exponent/mantissa,
        // not exact bits — exponent all ones AND non-zero mantissa).
        {
            const std::uint32_t b{ g_echo_float_nan_bits.load() };
            const bool is_nan{ (b & 0x7F800000u) == 0x7F800000u && (b & 0x007FFFFFu) != 0u };
            ctx.check("mcj_echo_float_arg_nan_is_nan", is_nan);
        }
        {
            double d{ 0.0 };
            const std::uint64_t b{ g_echo_double_bits.load() };
            std::memcpy(&d, &b, sizeof(d));
            ctx.check("mcj_echo_double_arg_exact", d == k_echo_double);
        }
        {
            float f{ 0.0f };
            const std::uint32_t b{ g_s_echo_float_bits.load() };
            std::memcpy(&f, &b, sizeof(f));
            ctx.check("mcj_static_echo_float_arg_exact", f == k_echo_float);
        }
        {
            double d{ 0.0 };
            const std::uint64_t b{ g_s_echo_double_bits.load() };
            std::memcpy(&d, &b, sizeof(d));
            ctx.check("mcj_static_echo_double_arg_exact", d == k_echo_double);
        }

        // ═════════════════════ narrow / wide primitive ARG recorders ══════════
        // Each marshaller arm (Z / B / C / S / F / D) delivered its arg verbatim.
        ctx.check("mcj_arg_record_runs", g_arg_runs.load() == 6);
        ctx.check("mcj_arg_bool_body_ran", mcd_fixture::arg_bool_called());
        ctx.check("mcj_arg_bool_true", mcd_fixture::arg_bool_value());
        ctx.check("mcj_arg_byte_body_ran", mcd_fixture::arg_byte_called());
        ctx.check("mcj_arg_byte_sign_extends", mcd_fixture::arg_byte_value() == k_arg_byte_int);
        ctx.check("mcj_arg_char_body_ran", mcd_fixture::arg_char_called());
        ctx.check("mcj_arg_char_zero_extends_65535", mcd_fixture::arg_char_value() == k_arg_char_int);
        ctx.check("mcj_arg_short_body_ran", mcd_fixture::arg_short_called());
        ctx.check("mcj_arg_short_sign_extends", mcd_fixture::arg_short_value() == k_arg_short_int);
        ctx.check("mcj_arg_float_body_ran", mcd_fixture::arg_float_called());
        {
            float f{ 0.0f };
            const std::int32_t raw{ mcd_fixture::arg_float_bits() };
            std::uint32_t b{ 0 };
            std::memcpy(&b, &raw, sizeof(b));
            std::memcpy(&f, &b, sizeof(f));
            ctx.check("mcj_arg_float_value_2_5", f == k_arg_float);
        }
        ctx.check("mcj_arg_double_body_ran", mcd_fixture::arg_double_called());
        {
            double d{ 0.0 };
            const std::int64_t raw{ mcd_fixture::arg_double_bits() };
            std::uint64_t b{ 0 };
            std::memcpy(&b, &raw, sizeof(b));
            std::memcpy(&d, &b, sizeof(d));
            ctx.check("mcj_arg_double_value_neg_8_5", d == k_arg_double);
        }

        // ═════════════════════ many-arg / all-two-slot ARG shapes ═════════════
        // result = a + c + f + b + e + (long)d ; six args, mixed single/two-slot.
        const std::int64_t expected_six{ static_cast<std::int64_t>(k_six_a)
                                         + k_six_b + static_cast<std::int64_t>(k_six_c)
                                         + static_cast<std::int64_t>(k_six_d)
                                         + k_six_e + static_cast<std::int64_t>(k_six_f) };
        ctx.check("mcj_six_arg_instance_return_correct", g_six_arg_ret.load() == expected_six);
        ctx.check("mcj_six_arg_static_return_correct", g_s_six_arg_ret.load() == expected_six);
        ctx.check("mcj_six_arg_body_ran", mcd_fixture::six_arg_called());
        ctx.check("mcj_six_arg_body_packed_matches", mcd_fixture::six_arg_packed() == expected_six);
        // result = a + c + (long)b - (long)d ; four consecutive two-slot args.
        const std::int64_t expected_fw{ k_fw_a + k_fw_c
                                        + static_cast<std::int64_t>(k_fw_b)
                                        - static_cast<std::int64_t>(k_fw_d) };
        ctx.check("mcj_four_wide_return_correct", g_four_wide_ret.load() == expected_fw);
        ctx.check("mcj_four_wide_body_ran", mcd_fixture::four_wide_called());
        ctx.check("mcj_four_wide_body_result_matches", mcd_fixture::four_wide_result() == expected_fw);

        // ═════════════════════ null-reference ARG contract ════════════════════
        // A null const char* and a null unique_ptr reached the JVM as Java null.
        ctx.check("mcj_null_string_arg_ran", g_null_str_ran.load() == 1);
        ctx.check("mcj_null_string_arg_body_ran", mcd_fixture::null_str_called());
        ctx.check("mcj_null_string_arg_was_null", mcd_fixture::null_str_was_null());
        ctx.check("mcj_null_object_arg_ran", g_null_obj_ran.load() == 1);
        ctx.check("mcj_null_object_arg_body_ran", mcd_fixture::null_obj_called());
        ctx.check("mcj_null_object_arg_was_null", mcd_fixture::null_obj_was_null());

        // ═════════════════════ float / double SPECIAL-VALUE returns ═══════════
        // IEEE-754 fidelity of the 'F'/'D' return arm for NaN / inf / -0.0 /
        // denormal — proven via exact bit patterns (NaN tested as NaN-ness, since
        // the JVM may canonicalize the payload).
        ctx.check("mcj_float_special_captured", g_fret_special_captured.load());
        {
            const std::uint32_t b{ g_fret_nan_bits.load() };
            const bool is_nan{ (b & 0x7F800000u) == 0x7F800000u && (b & 0x007FFFFFu) != 0u };
            ctx.check("mcj_float_ret_nan_is_nan", is_nan);
        }
        ctx.check("mcj_float_ret_pos_inf_bits", g_fret_posinf_bits.load() == 0x7F800000u);
        ctx.check("mcj_float_ret_neg_inf_bits", g_fret_neginf_bits.load() == 0xFF800000u);
        ctx.check("mcj_float_ret_neg_zero_bits", g_fret_negzero_bits.load() == 0x80000000u);
        ctx.check("mcj_float_ret_denormal_bits", g_fret_denorm_bits.load() == 0x00000001u); // Float.MIN_VALUE
        ctx.check("mcj_double_special_captured", g_dret_special_captured.load());
        {
            const std::uint64_t b{ g_dret_nan_bits.load() };
            const bool is_nan{ (b & 0x7FF0000000000000ull) == 0x7FF0000000000000ull
                               && (b & 0x000FFFFFFFFFFFFFull) != 0ull };
            ctx.check("mcj_double_ret_nan_is_nan", is_nan);
        }
        ctx.check("mcj_double_ret_pos_inf_bits", g_dret_posinf_bits.load() == 0x7FF0000000000000ull);
        ctx.check("mcj_double_ret_neg_zero_bits", g_dret_negzero_bits.load() == 0x8000000000000000ull);
        ctx.check("mcj_double_ret_denormal_bits", g_dret_denorm_bits.load() == 0x0000000000000001ull); // Double.MIN_VALUE
        // Static special-value returns (CallStatic<F|D>MethodA arm).
        ctx.check("mcj_static_special_captured", g_s_special_captured.load());
        {
            const std::uint32_t b{ g_s_fret_nan_bits.load() };
            const bool is_nan{ (b & 0x7F800000u) == 0x7F800000u && (b & 0x007FFFFFu) != 0u };
            ctx.check("mcj_static_float_ret_nan_is_nan", is_nan);
        }
        ctx.check("mcj_static_double_ret_pos_inf_bits", g_s_dret_posinf_bits.load() == 0x7FF0000000000000ull);
        ctx.check("mcj_static_double_ret_neg_zero_bits", g_s_dret_negzero_bits.load() == 0x8000000000000000ull);

        // ═════════════════════ additional primitive-array returns ═════════════
        // Every element stride the '[' arm + get_array_element must handle.
        // byte[] { -1, 0, 127, -128 } — 1-byte, sign-extending.
        ctx.check("mcj_byte_array_length_4", g_byte_arr_len.load() == 4);
        ctx.check("mcj_byte_array_elem0_neg1", g_byte_arr_e0.load() == -1);
        ctx.check("mcj_byte_array_elem2_127", g_byte_arr_e2.load() == 127);
        ctx.check("mcj_byte_array_elem3_neg128", g_byte_arr_e3.load() == -128);
        // boolean[] { true, false, true } — 1-byte 0/1.
        ctx.check("mcj_bool_array_length_3", g_bool_arr_len.load() == 3);
        ctx.check("mcj_bool_array_elem0_true", g_bool_arr_e0.load() == 1);
        ctx.check("mcj_bool_array_elem1_false", g_bool_arr_e1.load() == 0);
        // short[] { -32768, 0, 32767 } — 2-byte, sign.
        ctx.check("mcj_short_array_length_3", g_short_arr_len.load() == 3);
        ctx.check("mcj_short_array_elem0_min", g_short_arr_e0.load() == -32768);
        ctx.check("mcj_short_array_elem2_max", g_short_arr_e2.load() == 32767);
        // char[] { 'A', 0xFFFF, '0' } — 2-byte, zero-extend.
        ctx.check("mcj_char_array_length_3", g_char_arr_len.load() == 3);
        ctx.check("mcj_char_array_elem0_65", g_char_arr_e0.load() == 65);
        ctx.check("mcj_char_array_elem1_65535", g_char_arr_e1.load() == 65535);
        ctx.check("mcj_char_array_elem2_48", g_char_arr_e2.load() == 48);
        // double[] { 1.5, -2.5, 1024.0 } — 8-byte, IEEE bits.
        ctx.check("mcj_double_array_length_3", g_dbl_arr_len.load() == 3);
        ctx.check("mcj_double_array_captured", g_dbl_arr_captured.load());
        {
            double d{ 0.0 };
            const std::uint64_t b{ g_dbl_arr_e0_bits.load() };
            std::memcpy(&d, &b, sizeof(d));
            ctx.check("mcj_double_array_elem0_1_5", d == 1.5);
        }
        {
            double d{ 0.0 };
            const std::uint64_t b{ g_dbl_arr_e2_bits.load() };
            std::memcpy(&d, &b, sizeof(d));
            ctx.check("mcj_double_array_elem2_1024", d == 1024.0);
        }
        // float[] { 0.5, -0.5, 3.5, -3.5 } — 4-byte, IEEE bits.
        ctx.check("mcj_float_array_length_4", g_flt_arr_len.load() == 4);
        ctx.check("mcj_float_array_captured", g_flt_arr_captured.load());
        {
            float f{ 0.0f };
            const std::uint32_t b{ g_flt_arr_e0_bits.load() };
            std::memcpy(&f, &b, sizeof(f));
            ctx.check("mcj_float_array_elem0_0_5", f == 0.5f);
        }
        {
            float f{ 0.0f };
            const std::uint32_t b{ g_flt_arr_e1_bits.load() };
            std::memcpy(&f, &b, sizeof(f));
            ctx.check("mcj_float_array_elem1_neg_0_5", f == -0.5f && std::signbit(f));
        }

        // ═════════════════════ String returns ═════════════════════════════════
        ctx.check("mcj_str_captured", g_str_captured.load());
        ctx.check("mcj_str_instance_exact", g_str_inst == "jni-instance-hello");
        ctx.check("mcj_str_instance_is_string", g_str_inst_is_string.load());
        ctx.check("mcj_str_instance_nonempty", !g_str_inst.empty());
        ctx.check("mcj_str_static_exact", g_str_static == "jni-static-hello");
        ctx.check("mcj_str_static_is_string", g_str_static_is_string.load());
        ctx.check("mcj_str_static_nonempty", !g_str_static.empty());

        // ═════════════════════ String-arg round-trips ═════════════════════════
        ctx.check("mcj_echo_str_captured", g_echo_str_captured.load());
        ctx.check("mcj_echo_str_ascii_exact", g_echo_str_ascii == "round-trip-jni-987");
        ctx.check("mcj_echo_str_empty_exact", g_echo_str_empty.empty());
        // Latin-1 café round-trips BYTE-FOR-BYTE, and on every layout.  This was
        // previously asserted per dispatch path — "caf??" on the call_stub path —
        // against two lossy behaviours that no longer exist: make_java_string used
        // to copy the raw UTF-8 bytes into a LATIN1 array (turning U+00E9 into the
        // two chars U+00C3 U+00A9), and read_java_string used to substitute '?'
        // for every char >= 0x80.  Both were fixed: make_java_string UTF-8-decodes
        // to UTF-16 code units and picks the LATIN1/UTF16 coder from the units,
        // and read_java_string re-encodes code points as real UTF-8.  So "café"
        // survives the round trip intact — one assertion, no path split.
        ctx.check("mcj_echo_str_unicode_round_trips", g_echo_str_unicode == "caf\xC3\xA9");

        // ═════════════════════ String/Object arg -> void body ═════════════════
        ctx.check("mcj_consume_string_is_void", g_consume_str_is_void.load() == 1);
        ctx.check("mcj_consume_string_called", mcd_fixture::string_arg_called());
        ctx.check("mcj_consume_string_len_exact",
                  mcd_fixture::string_arg_len() == static_cast<std::int32_t>(std::string{ "void-string-arg-jni" }.size()));
        ctx.check("mcj_consume_string_value_exact",
                  mcd_fixture::string_arg_value() == "void-string-arg-jni");

        ctx.check("mcj_consume_object_is_void", g_consume_obj_is_void.load() == 1);
        ctx.check("mcj_consume_object_called", mcd_fixture::object_arg_called());
        ctx.check("mcj_consume_object_non_null", mcd_fixture::object_arg_non_null());
        // The body's identityHashCode of the received object equals the receiver's
        // published identity -> the EXACT receiver object reached the void body.
        ctx.check("mcj_consume_object_identity_matches_receiver",
                  mcd_fixture::object_arg_identity() != 0
                  && mcd_fixture::object_arg_identity() == mcd_fixture::self_identity());

        // ═════════════════════ Object returns: null contract (BOTH paths) ══════
        // The most important reference-return invariant: a null Java return must
        // never fabricate a wrapper, and must be is_void().  Holds on every path.
        ctx.check("mcj_null_object_returns_null_unique_ptr", g_null_obj_is_null.load());
        ctx.check("mcj_null_object_is_void", g_null_obj_is_void.load() == 1);
        ctx.check("mcj_static_null_object_returns_null_unique_ptr", g_static_null_is_null.load());

        // ═════════════════════ Object returns: identity ═══════════════════════
        // The current call_jni 'L'/'[' arm decodes the JNI handle to the real
        // heap OOP (jni_decode_object) and re-encodes it (encode_oop_pointer),
        // so a non-null reference return round-trips into a usable wrapper on
        // BOTH paths.  retSelf() must therefore yield the RECEIVER's OOP.
        ctx.check("mcj_retself_non_null_wrapper", g_self_nonnull.load());
        ctx.check("mcj_retself_instance_equals_receiver",
                  g_self_instance.load() != 0
                  && g_self_instance.load() == g_receiver_instance.load());
        // A non-null Object (non-String) return is NOT is_void().
        ctx.check("mcj_retself_not_void", g_self_is_void.load() == 0);
        ctx.check("mcj_static_singleton_non_null_wrapper", g_static_self_nonnull.load());
        ctx.check("mcj_static_singleton_instance_equals_receiver",
                  g_static_self_instance.load() != 0
                  && g_static_self_instance.load() == g_receiver_instance.load());
        // Array reference return decoded to a non-null oop.
        ctx.check("mcj_array_reference_decoded_non_null", g_array_nonnull.load());

        // ═════════════════════ Array returns: VALUE-correct ═══════════════════
        // int[] { 11, 22, 33 } — length + every element via the crash-safe
        // library readers prove the '[' arm decoded the REAL array.
        ctx.check("mcj_int_array_length_3", g_int_arr_len.load() == 3);
        ctx.check("mcj_int_array_elem0_11", g_int_arr_e0.load() == 11);
        ctx.check("mcj_int_array_elem1_22", g_int_arr_e1.load() == 22);
        ctx.check("mcj_int_array_elem2_33", g_int_arr_e2.load() == 33);
        // long[] — 8-byte stride; boundary elements exact.
        ctx.check("mcj_long_array_length_4", g_long_arr_len.load() == 4);
        ctx.check("mcj_long_array_elem0_pattern",
                  g_long_arr_e0.load() == static_cast<std::int64_t>(0x1111111111111111LL));
        ctx.check("mcj_long_array_elem3_pattern",
                  g_long_arr_e3.load() == static_cast<std::int64_t>(0x4444444444444444LL));
        // String[] — object-element '[' arm: non-null decode + exact length.
        ctx.check("mcj_string_array_decoded_non_null", g_str_arr_nonnull.load());
        ctx.check("mcj_string_array_length_3", g_str_arr_len.load() == 3);

        // ═════════════════ String-ARG NUL / astral into the JVM (#27) ══════════
        // The length-counted UTF-16 arg path must deliver every code unit verbatim.
        // "a\0b\0c": 5 UTF-16 units, 5 code points (interior NULs preserved — a
        // NewStringUTF marshaller would truncate to "a", giving char_len 1).
        ctx.check("mcj_nul_arg_body_ran", g_nul_arg_captured.load() == 1);
        ctx.check("mcj_nul_arg_char_len_5", g_nul_arg_char_len.load() == 5);
        ctx.check("mcj_nul_arg_cp_count_5", g_nul_arg_cp_count.load() == 5);
        // String.hashCode() of "a\0b\0c" is content-defined; a truncated arg would
        // hash differently.  Java: 31^4*'a' + 31^3*0 + 31^2*'b' + 31*0 + 'c'.
        {
            const std::string nul_payload{ std::string("a\0b\0c", 5) };
            std::int32_t expect_hash{ 0 };
            for (char ch : nul_payload)
            {
                expect_hash = expect_hash * 31 + static_cast<std::int32_t>(static_cast<unsigned char>(ch));
            }
            ctx.check("mcj_nul_arg_hashcode_content_exact",
                      g_nul_arg_hash.load() == expect_hash);
        }
        // "X<U+1F600>Y": one astral scalar = 2 UTF-16 units, so char_len 4 but
        // code-point count 3 (X, U+1F600, Y).  first cp 'X', last cp 'Y'; a
        // mangled marshal would change the count or drop the surrogate pair.
        ctx.check("mcj_astral_arg_body_ran", g_astral_arg_captured.load() == 1);
        ctx.check("mcj_astral_arg_char_len_4", g_astral_arg_char_len.load() == 4);
        ctx.check("mcj_astral_arg_cp_count_3", g_astral_arg_cp_count.load() == 3);
        ctx.check("mcj_astral_arg_first_cp_X", g_astral_arg_first_cp.load() == static_cast<int>('X'));
        ctx.check("mcj_astral_arg_last_cp_Y", g_astral_arg_last_cp.load() == static_cast<int>('Y'));

        // ═════════════════════ NONVIRTUAL <init> via slot 93 ══════════════════
        // The library's only CallNonvirtualVoidMethodA path: re-invoking the no-arg
        // constructor on the live receiver bumps ctorCalls by exactly one, returns
        // void, and leaves no pending exception.
        ctx.check("mcj_nonvirtual_ctor_proxy_found", g_ctor_proxy_found.load() == 1);
        ctx.check("mcj_nonvirtual_ctor_call_is_void", g_ctor_call_is_void.load() == 1);
        ctx.check("mcj_nonvirtual_ctor_advanced_count_by_one",
                  g_ctor_calls_before.load() >= 1
                  && g_ctor_calls_after.load() == g_ctor_calls_before.load() + 1);
        ctx.check("mcj_nonvirtual_ctor_no_pending_exception",
                  g_ctor_no_pending_exc.load() == 1);

        // ═════════════════════ EXCEPTION discipline ═══════════════════════════
        // call() must REPORT a callee's throw and CLEAR it off the JavaThread, so
        // nothing escapes to poison a sibling module and a follow-up call still
        // works.  Every assertion here reads the returned value_t, so all of them
        // hold unconditionally — there is one dispatcher now, and it reports the
        // throw itself rather than leaving the module to probe the thread.
        //
        // (This block used to be split by dispatch path and to read observations
        // hard-coded to `false` — the de-JNI removal took out the ExceptionCheck
        // that produced them and left the literal behind, which made the liveness
        // check unsatisfiable.  Reading threw() is both pure-VM and stronger: it
        // pins WHICH exception each call reported.)
        ctx.check("mcj_exc_instance_void_reported", g_exc_void_threw.load() == 1);
        ctx.check("mcj_exc_instance_int_reported", g_exc_int_threw.load() == 1);
        ctx.check("mcj_exc_static_void_reported", g_exc_static_threw.load() == 1);

        // The reported class is the exact one the fixture threw (internal form).
        ctx.check("mcj_exc_instance_void_class_is_illegal_state",
                  g_exc_void_class == "java/lang/IllegalStateException");
        ctx.check("mcj_exc_instance_int_class_is_arithmetic",
                  g_exc_int_class == "java/lang/ArithmeticException");
        ctx.check("mcj_exc_static_void_class_is_illegal_state",
                  g_exc_static_class == "java/lang/IllegalStateException");

        // A throwing value-returning callee yields a VALUE-INITIALISED result for
        // the declared return type, never the stub's garbage result slot.
        ctx.check("mcj_exc_int_result_value_initialised",
                  g_exc_int_result_is_zero.load() == 1);

        // Recovery + no-escape: a normal value-returning call after the throws
        // delivered its argument intact, so the throwing calls left the thread
        // clean and nothing escaped to poison a sibling module.
        ctx.check("mcj_exc_recovery_call_intact", g_exc_recovery_ok.load() == 1);
        // Liveness: at least one throw was genuinely reported, so the discipline
        // checks above are not vacuous.
        ctx.check("mcj_exc_throw_mechanism_fired", g_exc_seen_at_least_one.load() == 1);

        // ═════════════════════ TIGHT LOOP characterization ════════════════════
        // Repeat-call stability guards: a dispatcher that accumulated per-call
        // state (the JNI fallback's un-released local refs were the original
        // concern) shows up as values drifting or emptying out across iterations.

        // String-RETURN loop: stable single value across 256 iterations.
        ctx.check("mcj_string_return_loop_ran", g_ret_loop_iters.load() == 256);
        ctx.check("mcj_string_return_loop_no_leak_single_distinct",
                  g_ret_loop_distinct.load() == 1);

        // String-ARG loop: every echo round-trips exactly (zero mismatches).
        ctx.check("mcj_string_arg_loop_ran", g_arg_loop_iters.load() == 256);
        ctx.check("mcj_string_arg_loop_no_leak_zero_mismatches",
                  g_arg_loop_mismatches.load() == 0);

        // long+double MULTI-ARG primitive loop: zero mismatches => primitive
        // jvalue cells are never handed to DeleteLocalRef (union-aliasing safe)
        // and cached state is reused without corruption.
        ctx.check("mcj_two_slot_loop_ran", g_two_loop_iters.load() == 256);
        ctx.check("mcj_two_slot_loop_no_corruption_zero_mismatches",
                  g_two_loop_mismatches.load() == 0);
        // The loop body's recorded args (from the last iteration) confirm the
        // two-slot long + double survived marshalling.
        ctx.check("mcj_two_slot_loop_called", mcd_fixture::two_slot_called());
        ctx.check("mcj_two_slot_last_long",
                  mcd_fixture::two_slot_last_long() == static_cast<std::int64_t>(0x4242424242424242LL));
        ctx.check("mcj_two_slot_last_double",
                  mcd_fixture::two_slot_last_double() == 1024.0);

        // ═════════════════════ NON-CORRUPTION after the loops ═════════════════
        // A value-returning call after hundreds of JNI dispatches still works.
        ctx.check("mcj_post_loop_echo_value", g_post_loop_echo.load() == k_post_echo);

        // ═════════ STATIC METHOD via INSTANCE PROXY (CI-failure regression) ════
        // The bug: call()/call_jni decided static-vs-instance from
        // `this->object == nullptr` alone, so a static Method resolved through an
        // instance wrapper's get_method("..") (non-null receiver) was dispatched
        // as an instance call — wrong result AND corrupted JNI exception state.
        // The fix dispatches static whenever object==nullptr OR is_static().
        // These assertions FAIL on the pre-fix header (monostate/0 on the JNI
        // path, receiver-shifted arithmetic on the call_stub path) and PASS now,
        // on BOTH dispatch paths.  (The first re-land of this fix regressed only
        // the sibling mcj_echo_int_side_effect check — a self-inflicted field
        // clobber from a now-removed echoInt follow-up — not an is_static defect;
        // the follow-up below uses echoLong so lastEchoArg is left untouched.)

        // The proxy is genuinely a static method that nevertheless carries the
        // receiver (so the fix has to override a non-null `object`).
        ctx.check("mcj_svia_proxy_is_static", g_svia_is_static.load() == 1);
        ctx.check("mcj_svia_proxy_has_receiver", g_svia_has_receiver.load() == 1);

        // No-arg static via instance proxy returns the real static value.
        ctx.check("mcj_svia_no_arg_static_returns_int_min",
                  g_svia_ret_int.load() == -2147483648LL);
        // Single-arg static via instance proxy echoes the sentinel — proves the
        // receiver was NOT bound as the static method's first argument.
        ctx.check("mcj_svia_single_arg_static_echo_passthrough",
                  g_svia_echo_int.load() == k_svia_echo);
        // Multi-arg (I,J,D) static via instance proxy: exact sum proves the arg
        // block had no phantom receiver slot.
        ctx.check("mcj_svia_multi_arg_static_return_correct",
                  g_svia_sum_ild.load() == expected_sum);
        // The (name, signature) instance overload behaves identically.
        ctx.check("mcj_svia_sig_overload_static_echo_passthrough",
                  g_svia_echo_sig.load() == k_svia_echo);
        // And an ordinary INSTANCE call performed right after the static-via-
        // instance dispatches still works — the receiver / JNI state survived.
        // (echoLong returns its argument; no lastEchoArg side effect.)
        ctx.check("mcj_svia_followup_instance_call_intact",
                  g_svia_followup_inst_echo.load() == k_svia_follow);

        // ═════════════════════ DEEPEN (A): value_t type-tag discipline ════════
        // A primitive return is neither void nor a String; a String return IS a
        // string and is NOT void.  This pins the variant-alternative the value_t
        // conversion + as_string()/is_void()/is_string() introspection depend on.
        ctx.check("mcj_int_return_not_void", g_int_not_void.load() == 1);
        ctx.check("mcj_int_return_not_string", g_int_not_string.load() == 1);
        ctx.check("mcj_bool_return_not_void", g_bool_not_void.load() == 1);
        ctx.check("mcj_bool_return_not_string", g_bool_not_string.load() == 1);
        ctx.check("mcj_long_return_not_void", g_long_not_void.load() == 1);
        ctx.check("mcj_double_return_not_string", g_double_not_string.load() == 1);
        ctx.check("mcj_str_instance_not_void", g_str_inst_not_void.load() == 1);
        ctx.check("mcj_str_static_not_void", g_str_static_not_void.load() == 1);
        ctx.check("mcj_loopstring_is_string", g_loopstr_is_string.load() == 1);

        // ═════════════════════ DEEPEN (B): decode-width agreement ═════════════
        // retByte() decoded as int32 sign-extends to the SAME -7 the int8 decode
        // (g_byte) produced; retChar()/retCharMax() zero-extend; retShort() sign-
        // extends; retLong() is stable across two independent decodes.  Each new
        // value is cross-checked against the already-passing narrow decode.
        ctx.check("mcj_byte_as_int32_sign_extends", g_byte_as_int32.load() == -7);
        ctx.check("mcj_byte_width_decode_agrees", g_byte_as_int32.load() == g_byte.load());
        ctx.check("mcj_short_as_int32_sign_extends", g_short_as_int32.load() == -12345);
        ctx.check("mcj_short_width_decode_agrees", g_short_as_int32.load() == g_short.load());
        ctx.check("mcj_char_as_int32_zero_extends", g_char_as_int32.load() == 90);
        ctx.check("mcj_char_width_decode_agrees", g_char_as_int32.load() == g_char.load());
        ctx.check("mcj_long_decode_stable_a", g_long_decode_a.load() == k_long_ret);
        ctx.check("mcj_long_decode_stable_b", g_long_decode_b.load() == k_long_ret);
        ctx.check("mcj_long_decode_two_reads_agree",
                  g_long_decode_a.load() == g_long_decode_b.load());

        // ═════════════════════ DEEPEN (C): idempotency ════════════════════════
        // A deterministic value-returning method called twice on one fresh proxy
        // yields the identical value — no per-call drift / cache poisoning.
        ctx.check("mcj_int_idempotent_first_correct", g_int_idem_a.load() == k_int_ret);
        ctx.check("mcj_int_idempotent_two_reads_agree",
                  g_int_idem_a.load() == g_int_idem_b.load());
        ctx.check("mcj_retstring_idempotent_equal", g_retstr_idem_equal.load() == 1);
        ctx.check("mcj_retstring_idempotent_nonempty", g_retstr_idem_nonempty.load() == 1);

        // ═════════════════════ DEEPEN (D): value_t copy/move semantics ════════
        // Copy- and move-construction of a captured value_t preserve the decode.
        ctx.check("mcj_value_t_copy_preserves_string", g_vt_copy_str_ok.load() == 1);
        ctx.check("mcj_value_t_move_preserves_string", g_vt_move_str_ok.load() == 1);
        ctx.check("mcj_value_t_copy_preserves_int", g_vt_copy_int.load() == k_echo_int);

        // ═════════════════════ DEEPEN (E): String length cross-check ══════════
        // as_string() length equals the literal "jni-instance-hello" size, and an
        // empty-string echo decodes to size()==0 (count == size).
        ctx.check("mcj_str_instance_length_matches_literal",
                  g_str_inst_len.load()
                  == static_cast<std::int64_t>(std::string{ "jni-instance-hello" }.size()));
        ctx.check("mcj_echo_empty_string_length_zero", g_echo_empty_len.load() == 0);

        // ═════════════════════ DEEPEN (F): smallest-magnitude echo ════════════
        ctx.check("mcj_echo_int_one", g_echo_one.load() == 1);
        ctx.check("mcj_echo_long_one", g_echo_long_one.load() == 1);

        // ═════════════════════ DEEPEN (G): array_length idempotency / count==size
        // Re-reading each array's length yields the SAME literal element count the
        // per-element value checks already proved (array_length is a pure read).
        ctx.check("mcj_int_array_length_reread_3", g_int_arr_len2.load() == 3);
        ctx.check("mcj_int_array_length_reread_agrees",
                  g_int_arr_len2.load() == g_int_arr_len.load());
        ctx.check("mcj_long_array_length_reread_4", g_long_arr_len2.load() == 4);
        ctx.check("mcj_long_array_length_reread_agrees",
                  g_long_arr_len2.load() == g_long_arr_len.load());
    }

    // UNCONDITIONAL teardown: the scoped_hook above is RAII, but on the no-SEH
    // Windows path (mingw/clang) a faulting body longjmps PAST its destructor,
    // leaving the hook armed for the NEXT module (poisons on_class_loaded's
    // multi_cb/rearm).  Force a full hook reset so the table is empty regardless
    // of how control left the block.
    if (ctx.reset)
    {
        ctx.reset();
    }
}
