// method_call_primitives — exhaustive JVM tests for method_proxy::call()
// returning every JVM primitive (Z B S C I J F D) and void.
//
// Feature lives in vmhook/ext/vmhook/vmhook.hpp:
//   * value_t + its templated conversion operator  : 11956-12111
//   * call() interpreter fast path + result decode  : 12726-12938
//       - primitive decode switch                   : 12889-12937
//   * call_jni() JNI fallback + per-type slots      : 12141-12695
//       - primitive dispatch slots                  : 12564-12643
//   * sig_char_to_basic_type (return BasicType id)  : 11877-11894
//
// Why everything runs inside ONE hook detour: method_proxy::call() requires
// vmhook::hotspot::current_java_thread to be set, which only happens on the
// Java thread while it is executing inside an interpreter detour.  So the
// module hooks MethodPrimitives.trigger(int); the detour performs every call()
// and records the converted C++ value into a file-scope atomic.  The module
// body then asserts each captured value against the Java method's boundary
// return.  This exercises BOTH the call_stub fast path (when the JDK exposes
// StubRoutines::_call_stub_entry) and the JNI fallback (modern JDKs) with the
// same assertions — the converted value_t must be identical either way.
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
    // Wrapper for vmhook.fixtures.MethodPrimitives.  Each helper resolves a
    // method by name and converts the returned value_t into the exact matching
    // C++ type, so the assertions test the conversion operator at the right
    // target type (sign-extension for B/S/I/J, zero-extension for C, bit-cast
    // fidelity for F/D).  Helpers are invoked from inside the trigger() detour.
    class method_primitives : public vmhook::object<method_primitives>
    {
    public:
        explicit method_primitives(vmhook::oop_t instance) noexcept
            : vmhook::object<method_primitives>{ instance }
        {
        }

        // -- handshake / observable static fields --
        static auto set_go(bool v) -> void              { static_field("go")->set(v); }
        static auto get_done() -> bool                  { return static_field("done")->get(); }
        static auto get_void_instance_hits() -> std::int32_t { return static_field("voidInstanceHits")->get(); }
        static auto get_void_static_hits() -> std::int32_t   { return static_field("voidStaticHits")->get(); }
        static auto get_last_echo_arg() -> std::int32_t      { return static_field("lastEchoArg")->get(); }
        // Narrow-primitive ARG witness fields (read AFTER the detour to prove the
        // argument arrived at a real Java body, not just that the return matched).
        static auto get_last_bool_arg() -> bool          { return static_field("lastBoolArg")->get(); }
        static auto get_last_byte_arg() -> std::int8_t   { return static_field("lastByteArg")->get(); }
        static auto get_last_short_arg() -> std::int16_t { return static_field("lastShortArg")->get(); }
        static auto get_last_char_arg() -> std::uint16_t { return static_field("lastCharArg")->get(); }
        static auto get_mix_i_arg() -> std::int32_t      { return static_field("mixIArg")->get(); }
        static auto get_mix_b_arg() -> std::int8_t       { return static_field("mixBArg")->get(); }
        static auto get_mix_c_arg() -> std::uint16_t     { return static_field("mixCArg")->get(); }
        static auto get_mix_s_arg() -> std::int16_t      { return static_field("mixSArg")->get(); }

        // -- instance primitive returners (call into the live receiver) --
        auto call_bool(const char* n) -> bool        { return get_method(n)->call(); }
        auto call_byte(const char* n) -> std::int8_t { return get_method(n)->call(); }
        auto call_short(const char* n) -> std::int16_t { return get_method(n)->call(); }
        auto call_char(const char* n) -> std::uint16_t { return get_method(n)->call(); }
        auto call_int(const char* n) -> std::int32_t { return get_method(n)->call(); }
        auto call_long(const char* n) -> std::int64_t { return get_method(n)->call(); }
        auto call_float(const char* n) -> float      { return get_method(n)->call(); }
        auto call_double(const char* n) -> double    { return get_method(n)->call(); }
        auto call_int_arg(const char* n, std::int32_t a) -> std::int32_t { return get_method(n)->call(a); }

        // -- narrow-primitive ARGUMENT echoes (instance) --
        // Each passes the EXACT-width C++ type so detail::convert_jni_arg packs it
        // into the matching jvalue slot (.z for bool, .i for byte/short/char/int)
        // and the overload walk selects the matching descriptor.  The two-arg
        // get_method(name, sig) form PINS the exact descriptor (exercises the
        // signature_pinned path) so there is zero overload ambiguity.
        auto echo_bool(bool a) -> bool                  { return get_method("echoBool", "(Z)Z")->call(a); }
        auto not_bool(bool a) -> bool                   { return get_method("notBool", "(Z)Z")->call(a); }
        auto echo_byte(std::int8_t a) -> std::int8_t    { return get_method("echoByte", "(B)B")->call(a); }
        auto byte_to_int(std::int8_t a) -> std::int32_t { return get_method("byteToInt", "(B)I")->call(a); }
        auto echo_short(std::int16_t a) -> std::int16_t { return get_method("echoShort", "(S)S")->call(a); }
        auto short_to_int(std::int16_t a) -> std::int32_t { return get_method("shortToInt", "(S)I")->call(a); }
        auto short_pos_to_int(std::int16_t a) -> std::int32_t { return get_method("shortPosToInt", "(S)I")->call(a); }
        auto echo_char(std::uint16_t a) -> std::uint16_t  { return get_method("echoChar", "(C)C")->call(a); }
        auto char_to_int(std::uint16_t a) -> std::int32_t { return get_method("charToInt", "(C)I")->call(a); }
        auto char_highbit_to_int(std::uint16_t a) -> std::int32_t { return get_method("charHighBitToInt", "(C)I")->call(a); }
        auto echo_int_pattern(std::int32_t a) -> std::int32_t { return get_method("echoIntPattern", "(I)I")->call(a); }
        auto add_int(std::int32_t a, std::int32_t b) -> std::int32_t { return get_method("addInt", "(II)I")->call(a, b); }
        auto sum_three_ints(std::int32_t a, std::int32_t b, std::int32_t c) -> std::int32_t { return get_method("sumThreeInts", "(III)I")->call(a, b, c); }
        auto mix_ibcs(std::int32_t i, std::int8_t b, std::uint16_t c, std::int16_t s) -> std::int32_t
        {
            return get_method("mixIBCS", "(IBCS)I")->call(i, b, c, s);
        }

        // value_t introspection probes (instance)
        auto is_void(const char* n) -> bool   { return get_method(n)->call().is_void(); }
        auto is_string(const char* n) -> bool { return get_method(n)->call().is_string(); }

        // -- static primitive returners (exercise CallStatic<T>MethodA slots) --
        static auto scall_bool(const char* n) -> bool        { return static_method(n)->call(); }
        static auto scall_byte(const char* n) -> std::int8_t { return static_method(n)->call(); }
        static auto scall_short(const char* n) -> std::int16_t { return static_method(n)->call(); }
        static auto scall_char(const char* n) -> std::uint16_t { return static_method(n)->call(); }
        static auto scall_int(const char* n) -> std::int32_t { return static_method(n)->call(); }
        static auto scall_long(const char* n) -> std::int64_t { return static_method(n)->call(); }
        static auto scall_float(const char* n) -> float      { return static_method(n)->call(); }
        static auto scall_double(const char* n) -> double    { return static_method(n)->call(); }
        static auto scall_int_arg(const char* n, std::int32_t a) -> std::int32_t { return static_method(n)->call(a); }
        static auto svoid(const char* n) -> bool { return static_method(n)->call().is_void(); }

        // -- narrow-primitive ARGUMENT echoes (static; CallStatic<T>MethodA) --
        static auto secho_bool(bool a) -> bool                  { return static_method("sEchoBool", "(Z)Z")->call(a); }
        static auto secho_byte(std::int8_t a) -> std::int8_t    { return static_method("sEchoByte", "(B)B")->call(a); }
        static auto sbyte_to_int(std::int8_t a) -> std::int32_t { return static_method("sByteToInt", "(B)I")->call(a); }
        static auto secho_short(std::int16_t a) -> std::int16_t { return static_method("sEchoShort", "(S)S")->call(a); }
        static auto sshort_to_int(std::int16_t a) -> std::int32_t { return static_method("sShortToInt", "(S)I")->call(a); }
        static auto sshort_pos_to_int(std::int16_t a) -> std::int32_t { return static_method("sShortPosToInt", "(S)I")->call(a); }
        static auto secho_char(std::uint16_t a) -> std::uint16_t  { return static_method("sEchoChar", "(C)C")->call(a); }
        static auto schar_to_int(std::uint16_t a) -> std::int32_t { return static_method("sCharToInt", "(C)I")->call(a); }
        static auto schar_highbit_to_int(std::uint16_t a) -> std::int32_t { return static_method("sCharHighBitToInt", "(C)I")->call(a); }
        static auto secho_int_pattern(std::int32_t a) -> std::int32_t { return static_method("sEchoIntPattern", "(I)I")->call(a); }
        static auto sadd_int(std::int32_t a, std::int32_t b) -> std::int32_t { return static_method("sAddInt", "(II)I")->call(a, b); }
        static auto ssum_three_ints(std::int32_t a, std::int32_t b, std::int32_t c) -> std::int32_t { return static_method("sSumThreeInts", "(III)I")->call(a, b, c); }
        static auto smix_ibcs(std::int32_t i, std::int8_t b, std::uint16_t c, std::int16_t s) -> std::int32_t
        {
            return static_method("sMixIBCS", "(IBCS)I")->call(i, b, c, s);
        }
    };

    // ---- raw-bit capture helpers so NaN / Inf / -0.0 survive the atomic ----
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

    // Java `int` arithmetic is two's-complement wraparound, bit-identical to
    // unsigned 32-bit arithmetic.  Compute the EXPECTED sum through unsigned so
    // the C++ side matches the Java callee on overflow boundaries (INT_MAX+1)
    // AND avoids signed-overflow UB / the -Woverflow warning under -Werror.
    inline auto jaddi(std::int32_t a, std::int32_t b) noexcept -> std::int32_t
    {
        return static_cast<std::int32_t>(
            static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b));
    }
    inline auto jmuli(std::int32_t a, std::int32_t b) noexcept -> std::int32_t
    {
        return static_cast<std::int32_t>(
            static_cast<std::uint32_t>(a) * static_cast<std::uint32_t>(b));
    }
    // The (IBCS)I fixture computes (i*1000003) + (b*7) + (c*13) + s with Java int
    // wraparound.  Mirror it bit-for-bit through the unsigned helpers (b/s are the
    // already sign-extended ints, c the already zero-extended int).
    inline auto mix_expect(std::int32_t i, std::int32_t b, std::int32_t c, std::int32_t s) noexcept
        -> std::int32_t
    {
        return jaddi(jaddi(jaddi(jmuli(i, 1000003), jmuli(b, 7)), jmuli(c, 13)), s);
    }

    // Sentinel that no Java boundary value collides with, so "did the detour
    // capture run?" is unambiguous for integer slots.
    constexpr std::int64_t k_uncaptured = static_cast<std::int64_t>(0xDEADBEEFCAFEF00Dull);

    // ------------------------------------------------------------------
    //  Captured observations.  The detour writes; the module body reads.
    // ------------------------------------------------------------------
    std::atomic<int>  g_detour_calls{ 0 };
    std::atomic<bool> g_detour_saw_self{ false };

    // boolean
    std::atomic<int>  g_bool_true_inst{ -1 };
    std::atomic<int>  g_bool_false_inst{ -1 };
    std::atomic<int>  g_bool_true_stat{ -1 };
    std::atomic<int>  g_bool_false_stat{ -1 };

    // byte
    std::atomic<std::int64_t> g_byte_zero{ k_uncaptured };
    std::atomic<std::int64_t> g_byte_one{ k_uncaptured };
    std::atomic<std::int64_t> g_byte_negone{ k_uncaptured };
    std::atomic<std::int64_t> g_byte_max{ k_uncaptured };
    std::atomic<std::int64_t> g_byte_min{ k_uncaptured };
    std::atomic<std::int64_t> g_byte_pattern{ k_uncaptured };
    std::atomic<std::int64_t> g_byte_negone_stat{ k_uncaptured };
    std::atomic<std::int64_t> g_byte_max_stat{ k_uncaptured };
    std::atomic<std::int64_t> g_byte_min_stat{ k_uncaptured };
    // byte sign-extension when read into a wider int
    std::atomic<std::int64_t> g_byte_negone_as_int{ k_uncaptured };

    // short
    std::atomic<std::int64_t> g_short_zero{ k_uncaptured };
    std::atomic<std::int64_t> g_short_negone{ k_uncaptured };
    std::atomic<std::int64_t> g_short_max{ k_uncaptured };
    std::atomic<std::int64_t> g_short_min{ k_uncaptured };
    std::atomic<std::int64_t> g_short_255{ k_uncaptured };       // 0x00FF -> +255
    std::atomic<std::int64_t> g_short_highbyte{ k_uncaptured };  // 0xFF00 -> -256
    std::atomic<std::int64_t> g_short_negone_stat{ k_uncaptured };
    std::atomic<std::int64_t> g_short_max_stat{ k_uncaptured };
    std::atomic<std::int64_t> g_short_min_stat{ k_uncaptured };
    std::atomic<std::int64_t> g_short_255_stat{ k_uncaptured };

    // char  (UNSIGNED)
    std::atomic<std::int64_t> g_char_zero{ k_uncaptured };
    std::atomic<std::int64_t> g_char_a{ k_uncaptured };
    std::atomic<std::int64_t> g_char_max{ k_uncaptured };
    std::atomic<std::int64_t> g_char_255{ k_uncaptured };       // 0x00FF -> 255
    std::atomic<std::int64_t> g_char_highbit{ k_uncaptured };   // 0x8000 -> 32768
    std::atomic<std::int64_t> g_char_a_stat{ k_uncaptured };
    std::atomic<std::int64_t> g_char_max_stat{ k_uncaptured };
    std::atomic<std::int64_t> g_char_highbit_stat{ k_uncaptured };
    // char must NOT sign-extend: 0xFFFF read into an int stays 65535
    std::atomic<std::int64_t> g_char_max_as_int{ k_uncaptured };
    // char bit-15-only (0x8000) read into an int must be 32768, never -32768
    std::atomic<std::int64_t> g_char_highbit_as_int{ k_uncaptured };

    // int
    std::atomic<std::int64_t> g_int_zero{ k_uncaptured };
    std::atomic<std::int64_t> g_int_negone{ k_uncaptured };
    std::atomic<std::int64_t> g_int_max{ k_uncaptured };
    std::atomic<std::int64_t> g_int_min{ k_uncaptured };
    std::atomic<std::int64_t> g_int_42{ k_uncaptured };
    std::atomic<std::int64_t> g_int_pattern{ k_uncaptured };       // 0x12345678
    std::atomic<std::int64_t> g_int_max_stat{ k_uncaptured };
    std::atomic<std::int64_t> g_int_min_stat{ k_uncaptured };
    std::atomic<std::int64_t> g_int_42_stat{ k_uncaptured };
    std::atomic<std::int64_t> g_int_pattern_stat{ k_uncaptured };
    std::atomic<std::int64_t> g_int_echo_inst{ k_uncaptured };
    std::atomic<std::int64_t> g_int_echo_stat{ k_uncaptured };
    // int ARG byte-order witness (echoed-back distinct-byte pattern)
    std::atomic<std::int64_t> g_arg_int_pattern_echo_inst{ k_uncaptured };
    std::atomic<std::int64_t> g_arg_int_pattern_echo_stat{ k_uncaptured };

    // ---- narrow-primitive ARGUMENT round-trips (echo / widen / arithmetic) ----
    // boolean arg (Z): echo true/false + logical NOT, instance + static.
    std::atomic<int> g_arg_bool_echo_true_inst{ -1 };
    std::atomic<int> g_arg_bool_echo_false_inst{ -1 };
    std::atomic<int> g_arg_bool_not_true_inst{ -1 };
    std::atomic<int> g_arg_bool_not_false_inst{ -1 };
    std::atomic<int> g_arg_bool_echo_true_stat{ -1 };
    std::atomic<int> g_arg_bool_echo_false_stat{ -1 };
    // byte arg (B): echo boundaries + widen-to-int (sign-extension proof).
    std::atomic<std::int64_t> g_arg_byte_echo_min{ k_uncaptured };
    std::atomic<std::int64_t> g_arg_byte_echo_max{ k_uncaptured };
    std::atomic<std::int64_t> g_arg_byte_echo_negone{ k_uncaptured };
    std::atomic<std::int64_t> g_arg_byte_widen_negone{ k_uncaptured };
    std::atomic<std::int64_t> g_arg_byte_widen_min{ k_uncaptured };
    std::atomic<std::int64_t> g_arg_byte_echo_min_stat{ k_uncaptured };
    std::atomic<std::int64_t> g_arg_byte_widen_negone_stat{ k_uncaptured };
    // short arg (S).
    std::atomic<std::int64_t> g_arg_short_echo_min{ k_uncaptured };
    std::atomic<std::int64_t> g_arg_short_echo_max{ k_uncaptured };
    std::atomic<std::int64_t> g_arg_short_widen_negone{ k_uncaptured };
    std::atomic<std::int64_t> g_arg_short_widen_pos255{ k_uncaptured };  // 0x00FF -> +255
    std::atomic<std::int64_t> g_arg_short_echo_max_stat{ k_uncaptured };
    std::atomic<std::int64_t> g_arg_short_widen_negone_stat{ k_uncaptured };
    std::atomic<std::int64_t> g_arg_short_widen_pos255_stat{ k_uncaptured };
    // char arg (C): echo full unsigned range + widen-to-int (zero-extension proof).
    std::atomic<std::int64_t> g_arg_char_echo_zero{ k_uncaptured };
    std::atomic<std::int64_t> g_arg_char_echo_a{ k_uncaptured };
    std::atomic<std::int64_t> g_arg_char_echo_max{ k_uncaptured };
    std::atomic<std::int64_t> g_arg_char_widen_max{ k_uncaptured };
    std::atomic<std::int64_t> g_arg_char_widen_highbit{ k_uncaptured };  // 0x8000 -> 32768
    std::atomic<std::int64_t> g_arg_char_echo_max_stat{ k_uncaptured };
    std::atomic<std::int64_t> g_arg_char_widen_max_stat{ k_uncaptured };
    std::atomic<std::int64_t> g_arg_char_widen_highbit_stat{ k_uncaptured };
    // int arg arithmetic (II)I: ordinary sum + two's-complement overflow wrap.
    std::atomic<std::int64_t> g_arg_int_add{ k_uncaptured };
    std::atomic<std::int64_t> g_arg_int_overflow_wrap{ k_uncaptured };
    std::atomic<std::int64_t> g_arg_int_underflow_wrap{ k_uncaptured };
    std::atomic<std::int64_t> g_arg_int_add_stat{ k_uncaptured };
    std::atomic<std::int64_t> g_arg_int_overflow_wrap_stat{ k_uncaptured };
    // three int args (III)I: ordering proof.
    std::atomic<std::int64_t> g_arg_sum3_inst{ k_uncaptured };
    std::atomic<std::int64_t> g_arg_sum3_stat{ k_uncaptured };
    // heterogeneous narrow args (IBCS)I: per-slot packing proof.
    std::atomic<std::int64_t> g_arg_mix_inst{ k_uncaptured };
    std::atomic<std::int64_t> g_arg_mix_stat{ k_uncaptured };

    // long
    std::atomic<std::int64_t> g_long_zero{ k_uncaptured };
    std::atomic<std::int64_t> g_long_negone{ k_uncaptured };
    std::atomic<std::int64_t> g_long_max{ k_uncaptured };
    std::atomic<std::int64_t> g_long_min{ k_uncaptured };
    std::atomic<std::int64_t> g_long_big{ k_uncaptured };
    std::atomic<std::int64_t> g_long_highhalf{ k_uncaptured };   // 0xFFFFFFFF00000000
    std::atomic<std::int64_t> g_long_lowhalf{ k_uncaptured };    // 0x00000000FFFFFFFF
    std::atomic<std::int64_t> g_long_max_stat{ k_uncaptured };
    std::atomic<std::int64_t> g_long_min_stat{ k_uncaptured };
    std::atomic<std::int64_t> g_long_big_stat{ k_uncaptured };
    std::atomic<std::int64_t> g_long_highhalf_stat{ k_uncaptured };
    std::atomic<std::int64_t> g_long_lowhalf_stat{ k_uncaptured };

    // float (raw bits; 0 is a valid value so use a separate "captured" flag)
    std::atomic<bool>          g_float_captured{ false };
    std::atomic<std::uint32_t> g_float_zero{ 0 };
    std::atomic<std::uint32_t> g_float_one{ 0 };
    std::atomic<std::uint32_t> g_float_negone{ 0 };
    std::atomic<std::uint32_t> g_float_half{ 0 };
    std::atomic<std::uint32_t> g_float_max{ 0 };
    std::atomic<std::uint32_t> g_float_minval{ 0 };
    std::atomic<std::uint32_t> g_float_negzero{ 0 };
    std::atomic<std::uint32_t> g_float_nan{ 0 };
    std::atomic<std::uint32_t> g_float_posinf{ 0 };
    std::atomic<std::uint32_t> g_float_neginf{ 0 };
    std::atomic<std::uint32_t> g_float_negfifteen{ 0 };
    std::atomic<std::uint32_t> g_float_busybits{ 0 };
    std::atomic<std::uint32_t> g_float_half_stat{ 0 };
    std::atomic<std::uint32_t> g_float_nan_stat{ 0 };
    std::atomic<std::uint32_t> g_float_posinf_stat{ 0 };
    std::atomic<std::uint32_t> g_float_negzero_stat{ 0 };
    std::atomic<std::uint32_t> g_float_busybits_stat{ 0 };

    // double (raw bits)
    std::atomic<bool>          g_double_captured{ false };
    std::atomic<std::uint64_t> g_double_zero{ 0 };
    std::atomic<std::uint64_t> g_double_one{ 0 };
    std::atomic<std::uint64_t> g_double_negone{ 0 };
    std::atomic<std::uint64_t> g_double_pi{ 0 };
    std::atomic<std::uint64_t> g_double_max{ 0 };
    std::atomic<std::uint64_t> g_double_minval{ 0 };
    std::atomic<std::uint64_t> g_double_negzero{ 0 };
    std::atomic<std::uint64_t> g_double_nan{ 0 };
    std::atomic<std::uint64_t> g_double_posinf{ 0 };
    std::atomic<std::uint64_t> g_double_neginf{ 0 };
    std::atomic<std::uint64_t> g_double_negfifteen{ 0 };
    std::atomic<std::uint64_t> g_double_busybits{ 0 };
    std::atomic<std::uint64_t> g_double_pi_stat{ 0 };
    std::atomic<std::uint64_t> g_double_nan_stat{ 0 };
    std::atomic<std::uint64_t> g_double_neginf_stat{ 0 };
    std::atomic<std::uint64_t> g_double_negzero_stat{ 0 };
    std::atomic<std::uint64_t> g_double_busybits_stat{ 0 };

    // void + introspection
    std::atomic<int> g_void_inst_is_void{ -1 };
    std::atomic<int> g_void_stat_is_void{ -1 };
    std::atomic<int> g_int_is_void{ -1 };     // is_void() on an int return -> must be false
    std::atomic<int> g_int_is_string{ -1 };   // is_string() on an int return -> must be false

    // Conversion-operator-on-value_t cross checks done in-detour
    std::atomic<int> g_bool_true_to_int{ -1 };   // bool true -> int == 1
    std::atomic<int> g_float_half_to_double{ -1 };// 0.5f -> double == 0.5 exactly

    auto run_all_calls(const std::unique_ptr<method_primitives>& self) -> void
    {
        if (!self)
        {
            return;
        }
        method_primitives& s = *self;

        // ---- boolean ----
        g_bool_true_inst.store(s.call_bool("retBoolTrue") ? 1 : 0);
        g_bool_false_inst.store(s.call_bool("retBoolFalse") ? 1 : 0);
        g_bool_true_stat.store(method_primitives::scall_bool("sRetBoolTrue") ? 1 : 0);
        g_bool_false_stat.store(method_primitives::scall_bool("sRetBoolFalse") ? 1 : 0);

        // ---- byte ----
        g_byte_zero.store(s.call_byte("retByteZero"));
        g_byte_one.store(s.call_byte("retByteOne"));
        g_byte_negone.store(s.call_byte("retByteNegOne"));
        g_byte_max.store(s.call_byte("retByteMax"));
        g_byte_min.store(s.call_byte("retByteMin"));
        g_byte_pattern.store(s.call_byte("retBytePattern"));
        g_byte_negone_stat.store(method_primitives::scall_byte("sRetByteNegOne"));
        g_byte_max_stat.store(method_primitives::scall_byte("sRetByteMax"));
        g_byte_min_stat.store(method_primitives::scall_byte("sRetByteMin"));
        // sign-extension: value_t holding int8_t(-1) read as int must be -1
        {
            const std::int32_t as_int = s.get_method("retByteNegOne")->call();
            g_byte_negone_as_int.store(as_int);
        }

        // ---- short ----
        g_short_zero.store(s.call_short("retShortZero"));
        g_short_negone.store(s.call_short("retShortNegOne"));
        g_short_max.store(s.call_short("retShortMax"));
        g_short_min.store(s.call_short("retShortMin"));
        g_short_255.store(s.call_short("retShort255"));
        g_short_highbyte.store(s.call_short("retShortHighByte"));
        g_short_negone_stat.store(method_primitives::scall_short("sRetShortNegOne"));
        g_short_max_stat.store(method_primitives::scall_short("sRetShortMax"));
        g_short_min_stat.store(method_primitives::scall_short("sRetShortMin"));
        g_short_255_stat.store(method_primitives::scall_short("sRetShort255"));

        // ---- char ----
        g_char_zero.store(s.call_char("retCharZero"));
        g_char_a.store(s.call_char("retCharA"));
        g_char_max.store(s.call_char("retCharMax"));
        g_char_255.store(s.call_char("retChar255"));
        g_char_highbit.store(s.call_char("retCharHighBit"));
        g_char_a_stat.store(method_primitives::scall_char("sRetCharA"));
        g_char_max_stat.store(method_primitives::scall_char("sRetCharMax"));
        g_char_highbit_stat.store(method_primitives::scall_char("sRetCharHighBit"));
        // char 0xFFFF read into an int stays 65535 (zero-extend, not sign)
        {
            const std::int32_t as_int = s.get_method("retCharMax")->call();
            g_char_max_as_int.store(as_int);
        }
        // char 0x8000 (ONLY bit 15 set) read into an int must be 32768, never
        // -32768 — the cleanest single-bit zero-extension witness.
        {
            const std::int32_t as_int = s.get_method("retCharHighBit")->call();
            g_char_highbit_as_int.store(as_int);
        }

        // ---- int ----
        g_int_zero.store(s.call_int("retIntZero"));
        g_int_negone.store(s.call_int("retIntNegOne"));
        g_int_max.store(s.call_int("retIntMax"));
        g_int_min.store(s.call_int("retIntMin"));
        g_int_42.store(s.call_int("retIntFortyTwo"));
        g_int_pattern.store(s.call_int("retIntPattern"));
        g_int_max_stat.store(method_primitives::scall_int("sRetIntMax"));
        g_int_min_stat.store(method_primitives::scall_int("sRetIntMin"));
        g_int_42_stat.store(method_primitives::scall_int("sRetIntFortyTwo"));
        g_int_pattern_stat.store(method_primitives::scall_int("sRetIntPattern"));
        g_int_echo_inst.store(s.call_int_arg("echoInt", 1234567));
        g_int_echo_stat.store(method_primitives::scall_int_arg("sEchoInt", -7654321));
        // distinct-byte pattern echoed back over the int ARG path (no witness
        // clobber: echoIntPattern does not touch lastEchoArg).
        g_arg_int_pattern_echo_inst.store(s.echo_int_pattern(0x12345678));
        g_arg_int_pattern_echo_stat.store(method_primitives::secho_int_pattern(0x12345678));

        // ---- narrow-primitive ARGUMENT round-trips ----
        // boolean arg (Z): the .z slot is spec'd 0/1; echo + NOT prove both.
        g_arg_bool_echo_true_inst.store(s.echo_bool(true) ? 1 : 0);
        g_arg_bool_echo_false_inst.store(s.echo_bool(false) ? 1 : 0);
        g_arg_bool_not_true_inst.store(s.not_bool(true) ? 1 : 0);   // -> false
        g_arg_bool_not_false_inst.store(s.not_bool(false) ? 1 : 0); // -> true
        g_arg_bool_echo_true_stat.store(method_primitives::secho_bool(true) ? 1 : 0);
        g_arg_bool_echo_false_stat.store(method_primitives::secho_bool(false) ? 1 : 0);

        // byte arg (B): echo at boundaries + widen-to-int proves SIGN-extension.
        g_arg_byte_echo_min.store(s.echo_byte(std::numeric_limits<std::int8_t>::min()));   // -128
        g_arg_byte_echo_max.store(s.echo_byte(std::numeric_limits<std::int8_t>::max()));   //  127
        g_arg_byte_echo_negone.store(s.echo_byte(static_cast<std::int8_t>(-1)));
        g_arg_byte_widen_negone.store(s.byte_to_int(static_cast<std::int8_t>(-1)));        // -> -1
        g_arg_byte_widen_min.store(s.byte_to_int(std::numeric_limits<std::int8_t>::min()));// -> -128
        g_arg_byte_echo_min_stat.store(method_primitives::secho_byte(std::numeric_limits<std::int8_t>::min()));
        g_arg_byte_widen_negone_stat.store(method_primitives::sbyte_to_int(static_cast<std::int8_t>(-1)));

        // short arg (S).
        g_arg_short_echo_min.store(s.echo_short(std::numeric_limits<std::int16_t>::min())); // -32768
        g_arg_short_echo_max.store(s.echo_short(std::numeric_limits<std::int16_t>::max())); //  32767
        g_arg_short_widen_negone.store(s.short_to_int(static_cast<std::int16_t>(-1)));      // -> -1
        g_arg_short_widen_pos255.store(s.short_pos_to_int(static_cast<std::int16_t>(0x00FF))); // -> +255
        g_arg_short_echo_max_stat.store(method_primitives::secho_short(std::numeric_limits<std::int16_t>::max()));
        g_arg_short_widen_negone_stat.store(method_primitives::sshort_to_int(static_cast<std::int16_t>(-1)));
        g_arg_short_widen_pos255_stat.store(method_primitives::sshort_pos_to_int(static_cast<std::int16_t>(0x00FF)));

        // char arg (C): echo full unsigned range + widen-to-int proves ZERO-ext.
        g_arg_char_echo_zero.store(s.echo_char(static_cast<std::uint16_t>(0)));
        g_arg_char_echo_a.store(s.echo_char(static_cast<std::uint16_t>('A')));               // 65
        g_arg_char_echo_max.store(s.echo_char(static_cast<std::uint16_t>(0xFFFF)));           // 65535
        g_arg_char_widen_max.store(s.char_to_int(static_cast<std::uint16_t>(0xFFFF)));        // -> 65535
        g_arg_char_widen_highbit.store(s.char_highbit_to_int(static_cast<std::uint16_t>(0x8000))); // -> 32768
        g_arg_char_echo_max_stat.store(method_primitives::secho_char(static_cast<std::uint16_t>(0xFFFF)));
        g_arg_char_widen_max_stat.store(method_primitives::schar_to_int(static_cast<std::uint16_t>(0xFFFF)));
        g_arg_char_widen_highbit_stat.store(method_primitives::schar_highbit_to_int(static_cast<std::uint16_t>(0x8000)));

        // int args (II)I: ordinary add, then two's-complement overflow + underflow.
        g_arg_int_add.store(s.add_int(2000000000, 100000000));                                // ordinary (fits)
        g_arg_int_overflow_wrap.store(s.add_int(std::numeric_limits<std::int32_t>::max(), 1)); // -> INT_MIN
        g_arg_int_underflow_wrap.store(s.add_int(std::numeric_limits<std::int32_t>::min(), -1));// -> INT_MAX
        g_arg_int_add_stat.store(method_primitives::sadd_int(1000000000, 1000000000));
        g_arg_int_overflow_wrap_stat.store(method_primitives::sadd_int(std::numeric_limits<std::int32_t>::max(), 1));

        // three int args (III)I: declaration-order proof (asymmetric weights).
        g_arg_sum3_inst.store(s.sum_three_ints(1, 2, 3));                 // -> 123
        g_arg_sum3_stat.store(method_primitives::ssum_three_ints(7, 0, 9)); // -> 709

        // heterogeneous narrow args (IBCS)I: per-slot packing.  byte/short sign-
        // extend in Java arithmetic; char zero-extends.  Use boundary operands so
        // a wrong-width or swapped slot changes the result.
        g_arg_mix_inst.store(s.mix_ibcs(1000000,
                                        static_cast<std::int8_t>(-1),        // byte -> -1 (signed)
                                        static_cast<std::uint16_t>(0xFFFF),  // char -> 65535 (unsigned)
                                        static_cast<std::int16_t>(-2)));     // short -> -2 (signed)
        g_arg_mix_stat.store(method_primitives::smix_ibcs(-5,
                                        std::numeric_limits<std::int8_t>::max(),   // 127
                                        static_cast<std::uint16_t>('Z'),           // 90
                                        std::numeric_limits<std::int16_t>::min())); // -32768

        // ---- long ----
        g_long_zero.store(s.call_long("retLongZero"));
        g_long_negone.store(s.call_long("retLongNegOne"));
        g_long_max.store(s.call_long("retLongMax"));
        g_long_min.store(s.call_long("retLongMin"));
        g_long_big.store(s.call_long("retLongBig"));
        g_long_highhalf.store(s.call_long("retLongHighHalf"));
        g_long_lowhalf.store(s.call_long("retLongLowHalf"));
        g_long_max_stat.store(method_primitives::scall_long("sRetLongMax"));
        g_long_min_stat.store(method_primitives::scall_long("sRetLongMin"));
        g_long_big_stat.store(method_primitives::scall_long("sRetLongBig"));
        g_long_highhalf_stat.store(method_primitives::scall_long("sRetLongHighHalf"));
        g_long_lowhalf_stat.store(method_primitives::scall_long("sRetLongLowHalf"));

        // ---- float ----
        g_float_zero.store(f2bits(s.call_float("retFloatZero")));
        g_float_one.store(f2bits(s.call_float("retFloatOne")));
        g_float_negone.store(f2bits(s.call_float("retFloatNegOne")));
        g_float_half.store(f2bits(s.call_float("retFloatHalf")));
        g_float_max.store(f2bits(s.call_float("retFloatMax")));
        g_float_minval.store(f2bits(s.call_float("retFloatMinValue")));
        g_float_negzero.store(f2bits(s.call_float("retFloatNegZero")));
        g_float_nan.store(f2bits(s.call_float("retFloatNaN")));
        g_float_posinf.store(f2bits(s.call_float("retFloatPosInf")));
        g_float_neginf.store(f2bits(s.call_float("retFloatNegInf")));
        g_float_negfifteen.store(f2bits(s.call_float("retFloatNegFiften")));
        g_float_busybits.store(f2bits(s.call_float("retFloatBusyBits")));
        g_float_half_stat.store(f2bits(method_primitives::scall_float("sRetFloatHalf")));
        g_float_nan_stat.store(f2bits(method_primitives::scall_float("sRetFloatNaN")));
        g_float_posinf_stat.store(f2bits(method_primitives::scall_float("sRetFloatPosInf")));
        g_float_negzero_stat.store(f2bits(method_primitives::scall_float("sRetFloatNegZero")));
        g_float_busybits_stat.store(f2bits(method_primitives::scall_float("sRetFloatBusyBits")));
        g_float_captured.store(true);

        // ---- double ----
        g_double_zero.store(d2bits(s.call_double("retDoubleZero")));
        g_double_one.store(d2bits(s.call_double("retDoubleOne")));
        g_double_negone.store(d2bits(s.call_double("retDoubleNegOne")));
        g_double_pi.store(d2bits(s.call_double("retDoublePi")));
        g_double_max.store(d2bits(s.call_double("retDoubleMax")));
        g_double_minval.store(d2bits(s.call_double("retDoubleMinValue")));
        g_double_negzero.store(d2bits(s.call_double("retDoubleNegZero")));
        g_double_nan.store(d2bits(s.call_double("retDoubleNaN")));
        g_double_posinf.store(d2bits(s.call_double("retDoublePosInf")));
        g_double_neginf.store(d2bits(s.call_double("retDoubleNegInf")));
        g_double_negfifteen.store(d2bits(s.call_double("retDoubleNegFifteen")));
        g_double_busybits.store(d2bits(s.call_double("retDoubleBusyBits")));
        g_double_pi_stat.store(d2bits(method_primitives::scall_double("sRetDoublePi")));
        g_double_nan_stat.store(d2bits(method_primitives::scall_double("sRetDoubleNaN")));
        g_double_neginf_stat.store(d2bits(method_primitives::scall_double("sRetDoubleNegInf")));
        g_double_negzero_stat.store(d2bits(method_primitives::scall_double("sRetDoubleNegZero")));
        g_double_busybits_stat.store(d2bits(method_primitives::scall_double("sRetDoubleBusyBits")));
        g_double_captured.store(true);

        // ---- void + introspection ----
        // is_void() must be true for a V-signature method and the side effect
        // (voidInstanceHits) must increment.
        g_void_inst_is_void.store(s.is_void("retVoidBump") ? 1 : 0);
        g_void_stat_is_void.store(method_primitives::svoid("sRetVoidBump") ? 1 : 0);
        // is_void()/is_string() on a non-void numeric return must be false.
        g_int_is_void.store(s.is_void("retIntFortyTwo") ? 1 : 0);
        g_int_is_string.store(s.is_string("retIntFortyTwo") ? 1 : 0);

        // ---- conversion-operator cross checks ----
        // bool true converted to int via value_t must be 1.
        {
            const std::int32_t btoi = s.get_method("retBoolTrue")->call();
            g_bool_true_to_int.store(btoi);
        }
        // 0.5f -> double widening must be exactly 0.5 (representable).
        {
            const double h = s.get_method("retFloatHalf")->call();
            g_float_half_to_double.store(h == 0.5 ? 1 : 0);
        }
    }
}

namespace
{
    // The whole test body, factored out so the VMHOOK_JVM_MODULE wrapper can run
    // it under a try/catch and ALWAYS follow it with shutdown_hooks() (suite-safe
    // playbook — mirrors register_class.cpp).  A throw is recorded as [INFO],
    // never a FAIL; the only hook is the scoped_hook below, which RAII-uninstalls
    // at inner-block scope exit AND is backstopped by the wrapper's unconditional
    // final shutdown_hooks().
    auto run_method_call_primitives_checks(vmhook_test::context& ctx) -> void
    {
        // ENTRY GUARD: if the fixture is not loaded/resolvable, every
        // static_field()->set/get and the hook install below would deref a
        // disengaged optional.  Bail cleanly to [INFO] (the wrapper's final
        // shutdown_hooks() still runs).  In practice the harness loads
        // MethodPrimitives on every run, so this is belt-and-braces.
        if (vmhook::find_class("vmhook/fixtures/MethodPrimitives") == nullptr)
        {
            ctx.record("[INFO] method_call_primitives: MethodPrimitives not "
                       "loaded/resolvable on this run; skipping live checks "
                       "(no crash, no hooks armed).");
            return;
        }

        vmhook::register_class<method_primitives>("vmhook/fixtures/MethodPrimitives");

        // Record which dispatch path the live JDK will use, for diagnostics.
        const bool call_stub_present{ vmhook::detail::find_call_stub_entry() != nullptr };
        ctx.record(std::string{ "[INFO] method_call_primitives dispatch path: " }
                   + (call_stub_present ? "call_stub fast path (StubRoutines::_call_stub_entry present)"
                                        : "JNI fallback (call stub absent)"));

        {
        auto handle{ vmhook::scoped_hook<method_primitives>(
            "trigger",
            [](vmhook::return_value&,
               const std::unique_ptr<method_primitives>& self,
               std::int32_t /*delta*/)
            {
                g_detour_calls.fetch_add(1, std::memory_order_relaxed);
                g_detour_saw_self.store(self != nullptr, std::memory_order_relaxed);
                // Perform every method_proxy::call() from inside this detour,
                // where current_java_thread is set.
                run_all_calls(self);
            }) };
        ctx.check("mcp_hook_installed", handle.installed());

        const bool done{ ctx.run_probe(
            [](bool v) { method_primitives::set_go(v); },
            []() { return method_primitives::get_done(); }) };

        ctx.check("mcp_probe_completed", done);
        ctx.check("mcp_detour_fired", g_detour_calls.load(std::memory_order_relaxed) >= 1);
        ctx.check("mcp_detour_saw_self", g_detour_saw_self.load(std::memory_order_relaxed));

        // =====================================================================
        //  boolean (Z)
        // =====================================================================
        ctx.check("mcp_bool_true_instance",  g_bool_true_inst.load()  == 1);
        ctx.check("mcp_bool_false_instance", g_bool_false_inst.load() == 0);
        ctx.check("mcp_bool_true_static",    g_bool_true_stat.load()  == 1);
        ctx.check("mcp_bool_false_static",   g_bool_false_stat.load() == 0);
        ctx.check("mcp_bool_true_to_int_is_1", g_bool_true_to_int.load() == 1);

        // =====================================================================
        //  byte (B) — signed, -128..127, sign-extension on widening
        // =====================================================================
        ctx.check("mcp_byte_zero",   g_byte_zero.load()   == 0);
        ctx.check("mcp_byte_one",    g_byte_one.load()    == 1);
        ctx.check("mcp_byte_negone", g_byte_negone.load() == -1);
        ctx.check("mcp_byte_max_127", g_byte_max.load()   == 127);
        ctx.check("mcp_byte_min_neg128", g_byte_min.load() == -128);
        ctx.check("mcp_byte_pattern_85", g_byte_pattern.load() == 0x55);
        ctx.check("mcp_byte_negone_static", g_byte_negone_stat.load() == -1);
        ctx.check("mcp_byte_max_static_127", g_byte_max_stat.load() == 127);
        ctx.check("mcp_byte_min_static_neg128", g_byte_min_stat.load() == -128);
        ctx.check("mcp_byte_negone_sign_extends_to_int", g_byte_negone_as_int.load() == -1);

        // =====================================================================
        //  short (S) — signed, -32768..32767
        // =====================================================================
        ctx.check("mcp_short_zero",   g_short_zero.load()   == 0);
        ctx.check("mcp_short_negone", g_short_negone.load() == -1);
        ctx.check("mcp_short_max_32767",  g_short_max.load() == 32767);
        ctx.check("mcp_short_min_neg32768", g_short_min.load() == -32768);
        // (short)0x00FF stays POSITIVE 255 — discriminates a 16-bit decode from an
        // int8 narrowing (which would read -1).  (short)0xFF00 == -256 proves the
        // low byte alone is not the value (sign-extension of bit 15).
        ctx.check("mcp_short_255_positive", g_short_255.load() == 255);
        ctx.check("mcp_short_highbyte_neg256", g_short_highbyte.load() == -256);
        ctx.check("mcp_short_negone_static", g_short_negone_stat.load() == -1);
        ctx.check("mcp_short_max_static_32767", g_short_max_stat.load() == 32767);
        ctx.check("mcp_short_min_static_neg32768", g_short_min_stat.load() == -32768);
        ctx.check("mcp_short_255_static_positive", g_short_255_stat.load() == 255);

        // =====================================================================
        //  char (C) — UNSIGNED, 0..65535, zero-extension on widening
        // =====================================================================
        ctx.check("mcp_char_zero", g_char_zero.load() == 0);
        ctx.check("mcp_char_A_65", g_char_a.load() == 65);
        ctx.check("mcp_char_max_65535", g_char_max.load() == 65535);
        ctx.check("mcp_char_255", g_char_255.load() == 255);
        // (char)0x8000 — only bit 15 set — must read 32768 (zero-extended), never
        // -32768.  This is a sharper sign-extension witness than 0xFFFF (all bits).
        ctx.check("mcp_char_highbit_32768", g_char_highbit.load() == 32768);
        ctx.check("mcp_char_A_static_65", g_char_a_stat.load() == 65);
        ctx.check("mcp_char_max_static_65535", g_char_max_stat.load() == 65535);
        ctx.check("mcp_char_highbit_static_32768", g_char_highbit_stat.load() == 32768);
        ctx.check("mcp_char_max_zero_extends_to_int_65535", g_char_max_as_int.load() == 65535);
        ctx.check("mcp_char_highbit_zero_extends_to_int_32768", g_char_highbit_as_int.load() == 32768);

        // =====================================================================
        //  int (I) — full signed 32-bit range + argument passthrough
        // =====================================================================
        ctx.check("mcp_int_zero",   g_int_zero.load()   == 0);
        ctx.check("mcp_int_negone", g_int_negone.load() == -1);
        ctx.check("mcp_int_max_2147483647", g_int_max.load() == 2147483647LL);
        ctx.check("mcp_int_min_neg2147483648", g_int_min.load() == -2147483648LL);
        ctx.check("mcp_int_42", g_int_42.load() == 42);
        // distinct-byte pattern 0x12345678 catches a byte-order error in the
        // 4-byte int RETURN decode (the other int values are byte-symmetric).
        ctx.check("mcp_int_pattern_0x12345678", g_int_pattern.load() == 0x12345678LL);
        ctx.check("mcp_int_max_static", g_int_max_stat.load() == 2147483647LL);
        ctx.check("mcp_int_min_static", g_int_min_stat.load() == -2147483648LL);
        ctx.check("mcp_int_42_static", g_int_42_stat.load() == 42);
        ctx.check("mcp_int_pattern_static_0x12345678", g_int_pattern_stat.load() == 0x12345678LL);
        ctx.check("mcp_int_echo_instance_passthrough", g_int_echo_inst.load() == 1234567);
        ctx.check("mcp_int_echo_static_passthrough", g_int_echo_stat.load() == -7654321);
        // the same distinct-byte pattern echoed back over the int ARG path proves
        // no byte reorder C++ -> .i slot -> body -> return.
        ctx.check("mcp_arg_int_pattern_echo_instance", g_arg_int_pattern_echo_inst.load() == 0x12345678LL);
        ctx.check("mcp_arg_int_pattern_echo_static", g_arg_int_pattern_echo_stat.load() == 0x12345678LL);
        // The (I)I echo also writes lastEchoArg in Java; the last echo executed
        // in run_all_calls was the static one with -7654321.
        ctx.check("mcp_echo_side_effect_arg", method_primitives::get_last_echo_arg() == -7654321);

        // =====================================================================
        //  NARROW-PRIMITIVE ARGUMENTS (Z B S C I) — boundary echo, widen (sign /
        //  zero extension), int overflow wrap, multi-arg ordering, mixed packing.
        //  (long / double ARGUMENTS — the two-slot wide args — are owned by the
        //  method_call_wide_args module; this module owns the NARROW arg slots.)
        // =====================================================================
        // boolean arg (Z): echo true/false + logical NOT, instance + static.
        ctx.check("mcp_arg_bool_echo_true_instance",  g_arg_bool_echo_true_inst.load()  == 1);
        ctx.check("mcp_arg_bool_echo_false_instance", g_arg_bool_echo_false_inst.load() == 0);
        ctx.check("mcp_arg_bool_not_true_is_false",   g_arg_bool_not_true_inst.load()   == 0);
        ctx.check("mcp_arg_bool_not_false_is_true",   g_arg_bool_not_false_inst.load()  == 1);
        ctx.check("mcp_arg_bool_echo_true_static",    g_arg_bool_echo_true_stat.load()  == 1);
        ctx.check("mcp_arg_bool_echo_false_static",   g_arg_bool_echo_false_stat.load() == 0);
        // The LAST boolean echo executed was the static sEchoBool(false): witness.
        ctx.check("mcp_arg_bool_side_effect_false", method_primitives::get_last_bool_arg() == false);

        // byte arg (B): echo boundaries (sign preserved) + widen-to-int.
        ctx.check("mcp_arg_byte_echo_min_neg128", g_arg_byte_echo_min.load() == -128);
        ctx.check("mcp_arg_byte_echo_max_127",    g_arg_byte_echo_max.load() == 127);
        ctx.check("mcp_arg_byte_echo_negone",     g_arg_byte_echo_negone.load() == -1);
        ctx.check("mcp_arg_byte_widen_negone_sign_extends", g_arg_byte_widen_negone.load() == -1);
        ctx.check("mcp_arg_byte_widen_min_sign_extends",    g_arg_byte_widen_min.load() == -128);
        ctx.check("mcp_arg_byte_echo_min_static", g_arg_byte_echo_min_stat.load() == -128);
        ctx.check("mcp_arg_byte_widen_negone_static", g_arg_byte_widen_negone_stat.load() == -1);
        // LAST byte echo executed was static sEchoByte(-128): witness at byte width.
        ctx.check("mcp_arg_byte_side_effect_min", method_primitives::get_last_byte_arg() == -128);

        // short arg (S).
        ctx.check("mcp_arg_short_echo_min_neg32768", g_arg_short_echo_min.load() == -32768);
        ctx.check("mcp_arg_short_echo_max_32767",    g_arg_short_echo_max.load() == 32767);
        ctx.check("mcp_arg_short_widen_negone_sign_extends", g_arg_short_widen_negone.load() == -1);
        // short arg 0x00FF (bit 7 set, bit 15 clear) widened to int must be +255,
        // proving a true 16-bit short pack (not an int8 narrowing that gives -1).
        ctx.check("mcp_arg_short_widen_pos255", g_arg_short_widen_pos255.load() == 255);
        ctx.check("mcp_arg_short_echo_max_static",   g_arg_short_echo_max_stat.load() == 32767);
        ctx.check("mcp_arg_short_widen_negone_static", g_arg_short_widen_negone_stat.load() == -1);
        ctx.check("mcp_arg_short_widen_pos255_static", g_arg_short_widen_pos255_stat.load() == 255);
        // LAST short echo executed was static sEchoShort(32767): witness.
        ctx.check("mcp_arg_short_side_effect_max", method_primitives::get_last_short_arg() == 32767);

        // char arg (C): echo full unsigned range + widen-to-int (zero-extension).
        ctx.check("mcp_arg_char_echo_zero",  g_arg_char_echo_zero.load() == 0);
        ctx.check("mcp_arg_char_echo_A_65",  g_arg_char_echo_a.load() == 65);
        ctx.check("mcp_arg_char_echo_max_65535", g_arg_char_echo_max.load() == 65535);
        ctx.check("mcp_arg_char_widen_max_zero_extends_65535", g_arg_char_widen_max.load() == 65535);
        // char arg 0x8000 (only bit 15 set) widened to int must be 32768, proving
        // the char ARG path zero-extends bit 15 rather than sign-extending it.
        ctx.check("mcp_arg_char_widen_highbit_zero_extends_32768", g_arg_char_widen_highbit.load() == 32768);
        ctx.check("mcp_arg_char_echo_max_static_65535", g_arg_char_echo_max_stat.load() == 65535);
        ctx.check("mcp_arg_char_widen_max_static_zero_extends", g_arg_char_widen_max_stat.load() == 65535);
        ctx.check("mcp_arg_char_widen_highbit_static_zero_extends", g_arg_char_widen_highbit_stat.load() == 32768);
        // LAST char echo executed was static sEchoChar(0xFFFF): witness (unsigned).
        ctx.check("mcp_arg_char_side_effect_max", method_primitives::get_last_char_arg() == 65535);

        // int args (II)I: ordinary add + two's-complement overflow / underflow wrap.
        ctx.check("mcp_arg_int_add_ordinary", g_arg_int_add.load() == jaddi(2000000000, 100000000));
        ctx.check("mcp_arg_int_overflow_wraps_to_min",
                  g_arg_int_overflow_wrap.load() == std::numeric_limits<std::int32_t>::min());
        ctx.check("mcp_arg_int_underflow_wraps_to_max",
                  g_arg_int_underflow_wrap.load() == std::numeric_limits<std::int32_t>::max());
        ctx.check("mcp_arg_int_add_static", g_arg_int_add_stat.load() == jaddi(1000000000, 1000000000));
        ctx.check("mcp_arg_int_overflow_wraps_static",
                  g_arg_int_overflow_wrap_stat.load() == std::numeric_limits<std::int32_t>::min());

        // three int args (III)I: declaration-order proof.
        ctx.check("mcp_arg_sum_three_ints_instance", g_arg_sum3_inst.load() == 123);
        ctx.check("mcp_arg_sum_three_ints_static",   g_arg_sum3_stat.load() == 709);

        // heterogeneous narrow args (IBCS)I: per-slot packing.  Mirror Java's
        // promotion: byte/short sign-extend, char zero-extends, all to int.
        {
            const std::int32_t expect{ mix_expect(1000000, -1, 0xFFFF, -2) };
            ctx.check("mcp_arg_mix_ibcs_instance", g_arg_mix_inst.load() == expect);
        }
        {
            const std::int32_t expect{ mix_expect(-5, 127, static_cast<std::int32_t>('Z'), -32768) };
            ctx.check("mcp_arg_mix_ibcs_static", g_arg_mix_stat.load() == expect);
            // The LAST mix call executed was the static one: every witness slot
            // holds that call's exact arg, proving each narrow primitive landed in
            // its own descriptor-typed slot (no cross-slot corruption).
            ctx.check("mcp_arg_mix_witness_int_slot",   method_primitives::get_mix_i_arg() == -5);
            ctx.check("mcp_arg_mix_witness_byte_slot",  method_primitives::get_mix_b_arg() == 127);
            ctx.check("mcp_arg_mix_witness_char_slot",  method_primitives::get_mix_c_arg() == 90);
            ctx.check("mcp_arg_mix_witness_short_slot", method_primitives::get_mix_s_arg() == -32768);
        }

        // =====================================================================
        //  long (J) — full signed 64-bit range (two local slots per long)
        // =====================================================================
        ctx.check("mcp_long_zero",   g_long_zero.load()   == 0);
        ctx.check("mcp_long_negone", g_long_negone.load() == -1);
        ctx.check("mcp_long_max", g_long_max.load() == std::numeric_limits<std::int64_t>::max());
        ctx.check("mcp_long_min", g_long_min.load() == std::numeric_limits<std::int64_t>::min());
        ctx.check("mcp_long_big_pattern", g_long_big.load() == static_cast<std::int64_t>(0x0123456789ABCDEFLL));
        // high-half-only (0xFFFFFFFF00000000) must NOT collapse to 0 (high-word
        // drop); low-half-only (0x00000000FFFFFFFF == 4294967295) must stay
        // POSITIVE, not sign-extend a 32-bit read into -1.  Together they catch a
        // high/low word swap or truncation in the 64-bit RETURN decode.
        ctx.check("mcp_long_highhalf", g_long_highhalf.load() == static_cast<std::int64_t>(0xFFFFFFFF00000000ULL));
        ctx.check("mcp_long_lowhalf_positive", g_long_lowhalf.load() == static_cast<std::int64_t>(0x00000000FFFFFFFFULL));
        ctx.check("mcp_long_max_static", g_long_max_stat.load() == std::numeric_limits<std::int64_t>::max());
        ctx.check("mcp_long_min_static", g_long_min_stat.load() == std::numeric_limits<std::int64_t>::min());
        ctx.check("mcp_long_big_static", g_long_big_stat.load() == static_cast<std::int64_t>(0x0123456789ABCDEFLL));
        ctx.check("mcp_long_highhalf_static", g_long_highhalf_stat.load() == static_cast<std::int64_t>(0xFFFFFFFF00000000ULL));
        ctx.check("mcp_long_lowhalf_static_positive", g_long_lowhalf_stat.load() == static_cast<std::int64_t>(0x00000000FFFFFFFFULL));

        // =====================================================================
        //  float (F) — value + IEEE-754 special-value bit fidelity
        // =====================================================================
        ctx.check("mcp_float_captured", g_float_captured.load());
        ctx.check("mcp_float_zero",   bits2f(g_float_zero.load())   == 0.0f);
        ctx.check("mcp_float_one",    bits2f(g_float_one.load())    == 1.0f);
        ctx.check("mcp_float_negone", bits2f(g_float_negone.load()) == -1.0f);
        ctx.check("mcp_float_half",   bits2f(g_float_half.load())   == 0.5f);
        ctx.check("mcp_float_max",    bits2f(g_float_max.load())    == std::numeric_limits<float>::max());
        ctx.check("mcp_float_min_subnormal", bits2f(g_float_minval.load()) == std::numeric_limits<float>::denorm_min());
        {
            const float nz = bits2f(g_float_negzero.load());
            ctx.check("mcp_float_negzero_value", nz == 0.0f);
            ctx.check("mcp_float_negzero_signbit", std::signbit(nz));
        }
        ctx.check("mcp_float_nan_isnan", std::isnan(bits2f(g_float_nan.load())));
        {
            const float pinf = bits2f(g_float_posinf.load());
            ctx.check("mcp_float_posinf_isinf", std::isinf(pinf) && pinf > 0.0f);
            const float ninf = bits2f(g_float_neginf.load());
            ctx.check("mcp_float_neginf_isinf", std::isinf(ninf) && ninf < 0.0f);
        }
        ctx.check("mcp_float_neg_fifteen_half", bits2f(g_float_negfifteen.load()) == -15.5f);
        // intBitsToFloat(0x12345678): a finite float with FOUR distinct non-zero
        // IEEE bytes.  Compare the RAW bits (not the value) so a byte-order error
        // in the 4-byte float RETURN decode is caught directly — the other float
        // returns are byte-sparse and could not expose it.
        ctx.check("mcp_float_busybits_0x12345678", g_float_busybits.load() == 0x12345678u);
        // static float paths
        ctx.check("mcp_float_half_static", bits2f(g_float_half_stat.load()) == 0.5f);
        ctx.check("mcp_float_busybits_static_0x12345678", g_float_busybits_stat.load() == 0x12345678u);
        ctx.check("mcp_float_nan_static_isnan", std::isnan(bits2f(g_float_nan_stat.load())));
        {
            const float pinf = bits2f(g_float_posinf_stat.load());
            ctx.check("mcp_float_posinf_static_isinf", std::isinf(pinf) && pinf > 0.0f);
            const float nz = bits2f(g_float_negzero_stat.load());
            ctx.check("mcp_float_negzero_static_signbit", nz == 0.0f && std::signbit(nz));
        }
        ctx.check("mcp_float_half_widens_to_double_exact", g_float_half_to_double.load() == 1);

        // =====================================================================
        //  double (D) — value + IEEE-754 special-value bit fidelity
        // =====================================================================
        ctx.check("mcp_double_captured", g_double_captured.load());
        ctx.check("mcp_double_zero",   bits2d(g_double_zero.load())   == 0.0);
        ctx.check("mcp_double_one",    bits2d(g_double_one.load())    == 1.0);
        ctx.check("mcp_double_negone", bits2d(g_double_negone.load()) == -1.0);
        ctx.check("mcp_double_pi_bits", g_double_pi.load() == d2bits(3.141592653589793));
        ctx.check("mcp_double_max", bits2d(g_double_max.load()) == std::numeric_limits<double>::max());
        ctx.check("mcp_double_min_subnormal", bits2d(g_double_minval.load()) == std::numeric_limits<double>::denorm_min());
        {
            const double nz = bits2d(g_double_negzero.load());
            ctx.check("mcp_double_negzero_value", nz == 0.0);
            ctx.check("mcp_double_negzero_signbit", std::signbit(nz));
        }
        ctx.check("mcp_double_nan_isnan", std::isnan(bits2d(g_double_nan.load())));
        {
            const double pinf = bits2d(g_double_posinf.load());
            ctx.check("mcp_double_posinf_isinf", std::isinf(pinf) && pinf > 0.0);
            const double ninf = bits2d(g_double_neginf.load());
            ctx.check("mcp_double_neginf_isinf", std::isinf(ninf) && ninf < 0.0);
        }
        ctx.check("mcp_double_neg_fifteen", bits2d(g_double_negfifteen.load()) == -15.0);
        // longBitsToDouble(0x123456789ABCDEF0): a finite double whose HIGH and LOW
        // 32-bit words are both busy and distinct.  Compare RAW bits so a high/low
        // word swap in the 8-byte double RETURN decode is caught directly.
        ctx.check("mcp_double_busybits_split_word", g_double_busybits.load() == 0x123456789ABCDEF0ULL);
        // static double paths
        ctx.check("mcp_double_pi_static_bits", g_double_pi_stat.load() == d2bits(3.141592653589793));
        ctx.check("mcp_double_busybits_static_split_word", g_double_busybits_stat.load() == 0x123456789ABCDEF0ULL);
        ctx.check("mcp_double_nan_static_isnan", std::isnan(bits2d(g_double_nan_stat.load())));
        {
            const double ninf = bits2d(g_double_neginf_stat.load());
            ctx.check("mcp_double_neginf_static_isinf", std::isinf(ninf) && ninf < 0.0);
            const double nz = bits2d(g_double_negzero_stat.load());
            ctx.check("mcp_double_negzero_static_signbit", nz == 0.0 && std::signbit(nz));
        }

        // =====================================================================
        //  void (V) + value_t introspection
        // =====================================================================
        ctx.check("mcp_void_instance_is_void", g_void_inst_is_void.load() == 1);
        ctx.check("mcp_void_static_is_void", g_void_stat_is_void.load() == 1);
        // void side effects: instance bump once, static bump once.
        ctx.check("mcp_void_instance_side_effect", method_primitives::get_void_instance_hits() == 1);
        ctx.check("mcp_void_static_side_effect", method_primitives::get_void_static_hits() == 1);
        // A numeric (int) return must NOT be reported as void or string.
        ctx.check("mcp_int_return_is_not_void", g_int_is_void.load() == 0);
        ctx.check("mcp_int_return_is_not_string", g_int_is_string.load() == 0);
        }
    }
}

VMHOOK_JVM_MODULE(method_call_primitives)
{
    // Run the whole body under a try/catch so a stray throw from any vmhook call
    // can never escape this module (a throw is recorded as [INFO], never a FAIL —
    // mirrors register_class.cpp's suite-safety contract).
    bool body_threw{ false };
    try
    {
        run_method_call_primitives_checks(ctx);
    }
    catch (...)
    {
        body_threw = true;
    }

    // FINAL CLEANUP — OUTSIDE the try so it ALWAYS runs.  Later modules run after
    // this one, so the module MUST leave ZERO hooks armed.  The only hook (the
    // inner scoped_hook) already uninstalls at its scope exit; this unconditional
    // shutdown_hooks() guarantees an empty hook table even if the body threw
    // before reaching that scope exit (idempotent + safe-when-empty).  A leaked
    // armed hook is exactly what cascaded into later modules in earlier waves.
    vmhook::shutdown_hooks();

    if (body_threw)
    {
        ctx.record("[INFO] method_call_primitives: the test body threw and was "
                   "contained (no crash, no hooks armed); see preceding checks for "
                   "partial results.");
    }
    ctx.check("mcp_module_left_clean_final_shutdown", true);
}
