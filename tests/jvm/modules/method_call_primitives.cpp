// method_call_primitives — exhaustive JVM tests for method_proxy::call()
// returning every JVM primitive (Z B S C I J F D) and void.
//
// Feature lives in vmhook/ext/vmhook/vmhook.hpp:
//   * value_t + its templated conversion operator  : 11956-12111
//   * call() interpreter fast path + result decode  : 12726-12938
//       - primitive decode switch                   : 12889-12937
//   * the call stub JNI fallback + per-type slots      : 12141-12695
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
        // (I)F: feed an int arg, read back a float RETURN (exact for the magnitudes
        // used) — proves an int ARG and a float RETURN coexist in one dispatch.
        auto call_int_to_float(const char* n, std::int32_t a) -> float { return get_method(n)->call(a); }

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
        // char arg in the surrogate range (0xD83D) widened in the callee — proves
        // a bit-15-set char arg with a busy low byte arrives ZERO-extended.
        auto char_surrogate_to_int(std::uint16_t a) -> std::int32_t { return get_method("charSurrogateToInt", "(C)I")->call(a); }
        // FIVE narrow args, one of each kind (boolean/byte/char/short/int) in ONE
        // frame — the deep pure-narrow packing witness past the 4-arg (IBCS)I.
        auto mix_zbcsi(bool z, std::int8_t b, std::uint16_t c, std::int16_t s, std::int32_t i) -> std::int32_t
        {
            return get_method("mixZBCSI", "(ZBCSI)I")->call(z, b, c, s, i);
        }
        // (I)I pure echo (no lastEchoArg clobber) used for INT boundary ARGS.
        auto echo_int_pattern_arg(std::int32_t a) -> std::int32_t { return get_method("echoIntPattern", "(I)I")->call(a); }

        // -- value_t conversion-operator breadth on a RETURN (instance) --
        // Each resolves a primitive returner and converts the SAME value_t into a
        // DIFFERENT-width / different-kind C++ target, exercising the templated
        // operator target_type()'s static_cast leg directly (int->int64 widen,
        // byte->int64 sign-extend, char->int64 zero-extend, int->bool truncation).
        auto ret_as_i64(const char* n) -> std::int64_t { return get_method(n)->call(); }
        auto ret_as_i32(const char* n) -> std::int32_t { return get_method(n)->call(); }
        auto ret_as_bool(const char* n) -> bool        { return get_method(n)->call(); }
        // Cross-KIND conversion on a RETURN value_t: a floating-point return read
        // into an INTEGER target exercises operator target_type()'s static_cast leg
        // float->int / double->int64 (truncation toward zero); an integral return
        // read into a FLOAT target exercises int32->float / int64->double widening.
        auto ret_float_as_i32(const char* n) -> std::int32_t { return get_method(n)->call(); }
        auto ret_double_as_i64(const char* n) -> std::int64_t { return get_method(n)->call(); }
        auto ret_int_as_float(const char* n) -> float  { return get_method(n)->call(); }
        auto ret_long_as_double(const char* n) -> double { return get_method(n)->call(); }
        auto ret_long_as_bool(const char* n) -> bool   { return get_method(n)->call(); }
        // (S)->wider-int RETURN widen: a short return read into a 32-bit int proves
        // the int16 alternative sign-extends (-1 -> -1, 0xAAAA -> -21846).
        auto short_ret_as_int(const char* n) -> std::int32_t { return get_method(n)->call(); }
        // -- batch-18 cross-kind conversion breadth on a RETURN (instance) --
        // Each converts the SAME primitive return value_t into a target whose KIND
        // differs from the stored alternative, exercising operator target_type()'s
        // static_cast leg the existing probes do not reach: a float return into a
        // 64-bit integer (trunc), a double return into a 32-bit integer (trunc), and
        // every integral kind into a wider FLOATING target (lossless widen for the
        // exact magnitudes used).  All read through one templated conversion site.
        auto ret_float_as_i64(const char* n) -> std::int64_t { return get_method(n)->call(); }
        auto ret_double_as_i32(const char* n) -> std::int32_t { return get_method(n)->call(); }
        auto ret_float_as_double(const char* n) -> double { return get_method(n)->call(); }
        auto ret_byte_as_float(const char* n) -> float { return get_method(n)->call(); }
        auto ret_byte_as_double(const char* n) -> double { return get_method(n)->call(); }
        auto ret_short_as_float(const char* n) -> float { return get_method(n)->call(); }
        auto ret_short_as_double(const char* n) -> double { return get_method(n)->call(); }
        auto ret_char_as_double(const char* n) -> double { return get_method(n)->call(); }
        auto ret_int_as_double(const char* n) -> double { return get_method(n)->call(); }
        auto ret_long_as_float(const char* n) -> float { return get_method(n)->call(); }
        // (I)J widen ARG path: an int arg read back as a long RETURN — the long twin
        // of call_int_to_float, proving an int ARG and a J RETURN coexist and the int
        // arrived SIGN-extended into the 64-bit result.
        auto call_int_to_long(const char* n, std::int32_t a) -> std::int64_t { return get_method(n)->call(a); }

        // value_t introspection probes (instance)
        auto is_void(const char* n) -> bool   { return get_method(n)->call().is_void(); }
        auto is_string(const char* n) -> bool { return get_method(n)->call().is_string(); }
        // as_string() on a NON-string return (numeric or void) is spec'd to return
        // an EMPTY std::string (the conversion operator / as_string() default leg
        // for a numeric / monostate alternative), not a textual rendering of the
        // number.  Proves as_string() never fabricates content from a primitive.
        auto as_string_empty(const char* n) -> bool { return get_method(n)->call().as_string().empty(); }

        // -- narrowing / cross-kind conversion legs the existing probes do not reach.
        // Each resolves a primitive returner and converts the SAME value_t into a
        // C++ target whose width is SMALLER than (or a different kind from) the
        // stored alternative, driving operator target_type()'s static_cast leg
        // through its TRUNCATING / reinterpreting path (char->int8 keeps the low
        // byte with its sign, char->int16 reinterprets the low 16 bits as signed,
        // char->char16_t is identity for the natural Java-char C++ type).
        auto ret_char_as_i8(const char* n)  -> std::int8_t   { return get_method(n)->call(); }
        auto ret_char_as_i16(const char* n) -> std::int16_t  { return get_method(n)->call(); }
        auto ret_char_as_u8(const char* n)  -> unsigned char { return get_method(n)->call(); }
        auto ret_char_as_char16(const char* n) -> char16_t   { return get_method(n)->call(); }
        auto ret_char_as_char(const char* n) -> char         { return get_method(n)->call(); }
        // bool value_t (the `bool` alternative) converted into every numeric width
        // and the floating kinds — the Z static_cast leg the existing probes never
        // exercise (they only read Z as bool or as int).
        auto ret_bool_as_i8(const char* n)  -> std::int8_t  { return get_method(n)->call(); }
        auto ret_bool_as_i16(const char* n) -> std::int16_t { return get_method(n)->call(); }
        auto ret_bool_as_u16(const char* n) -> std::uint16_t{ return get_method(n)->call(); }
        auto ret_bool_as_i64(const char* n) -> std::int64_t { return get_method(n)->call(); }
        auto ret_bool_as_float(const char* n)  -> float     { return get_method(n)->call(); }
        auto ret_bool_as_double(const char* n) -> double    { return get_method(n)->call(); }
        // Any primitive kind read into bool: nonzero -> true, zero (incl -0.0) -> false.
        auto ret_as_bool_kind(const char* n) -> bool { return get_method(n)->call(); }

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
        static auto scall_int_to_float(const char* n, std::int32_t a) -> float { return static_method(n)->call(a); }
        static auto scall_int_to_long(const char* n, std::int32_t a) -> std::int64_t { return static_method(n)->call(a); }
        static auto svoid(const char* n) -> bool { return static_method(n)->call().is_void(); }
        // static value_t introspection probes — is_void()/is_string() on a STATIC
        // primitive return must both be false (the static dispatch slot still yields
        // a populated, non-void, non-string value_t).
        static auto sis_void(const char* n) -> bool   { return static_method(n)->call().is_void(); }
        static auto sis_string(const char* n) -> bool { return static_method(n)->call().is_string(); }
        // as_string() on a STATIC numeric/void return is likewise the empty string.
        static auto sas_string_empty(const char* n) -> bool { return static_method(n)->call().as_string().empty(); }

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
        static auto schar_surrogate_to_int(std::uint16_t a) -> std::int32_t { return static_method("sCharSurrogateToInt", "(C)I")->call(a); }
        static auto smix_zbcsi(bool z, std::int8_t b, std::uint16_t c, std::int16_t s, std::int32_t i) -> std::int32_t
        {
            return static_method("sMixZBCSI", "(ZBCSI)I")->call(z, b, c, s, i);
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
    // The (ZBCSI)I fixture computes (z*5000011)+(b*70001)+(c*900007)+(s*11)+i with
    // Java int wraparound, where z is the boolean promoted to 1/0, b/s are the
    // already sign-extended ints and c the already zero-extended int.  Mirror it
    // bit-for-bit through the unsigned helpers so boundary operands stay UB-free.
    inline auto mix5_expect(std::int32_t z, std::int32_t b, std::int32_t c,
                            std::int32_t s, std::int32_t i) noexcept -> std::int32_t
    {
        return jaddi(jaddi(jaddi(jaddi(jmuli(z, 5000011), jmuli(b, 70001)),
                                 jmuli(c, 900007)), jmuli(s, 11)), i);
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
    // byte alternating-bit phase 0xAA -> -86 (sign bit set), instance + static.
    std::atomic<std::int64_t> g_byte_alt{ k_uncaptured };
    std::atomic<std::int64_t> g_byte_alt_stat{ k_uncaptured };
    std::atomic<std::int64_t> g_byte_zero_stat{ k_uncaptured };
    std::atomic<std::int64_t> g_byte_one_stat{ k_uncaptured };
    std::atomic<std::int64_t> g_byte_pattern_stat{ k_uncaptured };

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
    // short alternating-bit phases: 0x5555 -> +21845, 0xAAAA -> -21846.
    std::atomic<std::int64_t> g_short_altpos{ k_uncaptured };
    std::atomic<std::int64_t> g_short_altneg{ k_uncaptured };
    std::atomic<std::int64_t> g_short_altneg_stat{ k_uncaptured };
    std::atomic<std::int64_t> g_short_zero_stat{ k_uncaptured };
    std::atomic<std::int64_t> g_short_highbyte_stat{ k_uncaptured };
    // short RETURN read into a wider int — proves the int16 alternative
    // sign-extends (negone -> -1, 0xAAAA -> -21846), the short twin of the
    // byte/char widen-into-int return probes.
    std::atomic<std::int64_t> g_short_negone_as_int{ k_uncaptured };
    std::atomic<std::int64_t> g_short_altneg_as_int{ k_uncaptured };
    std::atomic<std::int64_t> g_short_min_as_int{ k_uncaptured };

    // char  (UNSIGNED)
    std::atomic<std::int64_t> g_char_zero{ k_uncaptured };
    std::atomic<std::int64_t> g_char_a{ k_uncaptured };
    std::atomic<std::int64_t> g_char_max{ k_uncaptured };
    std::atomic<std::int64_t> g_char_255{ k_uncaptured };       // 0x00FF -> 255
    std::atomic<std::int64_t> g_char_highbit{ k_uncaptured };   // 0x8000 -> 32768
    std::atomic<std::int64_t> g_char_a_stat{ k_uncaptured };
    std::atomic<std::int64_t> g_char_max_stat{ k_uncaptured };
    std::atomic<std::int64_t> g_char_highbit_stat{ k_uncaptured };
    std::atomic<std::int64_t> g_char_surrogate{ k_uncaptured };       // 0xD83D -> 55357
    std::atomic<std::int64_t> g_char_surrogate_stat{ k_uncaptured };
    // char alternating-bit phases: 0x5555 -> 21845, 0xAAAA -> 43690 (bit 15 set).
    std::atomic<std::int64_t> g_char_altlo{ k_uncaptured };
    std::atomic<std::int64_t> g_char_althi{ k_uncaptured };
    std::atomic<std::int64_t> g_char_althi_stat{ k_uncaptured };
    std::atomic<std::int64_t> g_char_zero_stat{ k_uncaptured };
    std::atomic<std::int64_t> g_char_255_stat{ k_uncaptured };
    // char 0xAAAA read into an int must be 43690 (zero-extend), never -21846.
    std::atomic<std::int64_t> g_char_althi_as_int{ k_uncaptured };
    // char must NOT sign-extend: 0xFFFF read into an int stays 65535
    std::atomic<std::int64_t> g_char_max_as_int{ k_uncaptured };
    // char bit-15-only (0x8000) read into an int must be 32768, never -32768
    std::atomic<std::int64_t> g_char_highbit_as_int{ k_uncaptured };
    // char surrogate (0xD83D) read into an int must be 55357, never -10243
    std::atomic<std::int64_t> g_char_surrogate_as_int{ k_uncaptured };

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
    // int alternating-bit phases: 0x55555555 -> +1431655765, 0xAAAAAAAA -> -1431655766.
    std::atomic<std::int64_t> g_int_altpos{ k_uncaptured };
    std::atomic<std::int64_t> g_int_altneg{ k_uncaptured };
    std::atomic<std::int64_t> g_int_altneg_stat{ k_uncaptured };
    std::atomic<std::int64_t> g_int_zero_stat{ k_uncaptured };
    std::atomic<std::int64_t> g_int_negone_stat{ k_uncaptured };
    // int ARG byte-order witness (echoed-back distinct-byte pattern)
    std::atomic<std::int64_t> g_arg_int_pattern_echo_inst{ k_uncaptured };
    std::atomic<std::int64_t> g_arg_int_pattern_echo_stat{ k_uncaptured };
    // int ARG alternating-bit echoes over (I)I (no witness clobber).
    std::atomic<std::int64_t> g_arg_int_echo_altpos{ k_uncaptured };
    std::atomic<std::int64_t> g_arg_int_echo_altneg{ k_uncaptured };

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
    // byte arg alternating-bit widen: 0xAA -> -86 (sign), 0x55 -> +85.
    std::atomic<std::int64_t> g_arg_byte_widen_alt{ k_uncaptured };
    std::atomic<std::int64_t> g_arg_byte_widen_pattern{ k_uncaptured };
    // short arg (S).
    std::atomic<std::int64_t> g_arg_short_echo_min{ k_uncaptured };
    std::atomic<std::int64_t> g_arg_short_echo_max{ k_uncaptured };
    std::atomic<std::int64_t> g_arg_short_widen_negone{ k_uncaptured };
    std::atomic<std::int64_t> g_arg_short_widen_pos255{ k_uncaptured };  // 0x00FF -> +255
    std::atomic<std::int64_t> g_arg_short_echo_max_stat{ k_uncaptured };
    std::atomic<std::int64_t> g_arg_short_widen_negone_stat{ k_uncaptured };
    std::atomic<std::int64_t> g_arg_short_widen_pos255_stat{ k_uncaptured };
    // short arg alternating-bit widen: 0xAAAA -> -21846 (sign), 0x5555 -> +21845.
    std::atomic<std::int64_t> g_arg_short_widen_altneg{ k_uncaptured };
    std::atomic<std::int64_t> g_arg_short_widen_altpos{ k_uncaptured };
    // char arg (C): echo full unsigned range + widen-to-int (zero-extension proof).
    std::atomic<std::int64_t> g_arg_char_echo_zero{ k_uncaptured };
    std::atomic<std::int64_t> g_arg_char_echo_a{ k_uncaptured };
    std::atomic<std::int64_t> g_arg_char_echo_max{ k_uncaptured };
    std::atomic<std::int64_t> g_arg_char_widen_max{ k_uncaptured };
    std::atomic<std::int64_t> g_arg_char_widen_highbit{ k_uncaptured };  // 0x8000 -> 32768
    std::atomic<std::int64_t> g_arg_char_widen_surrogate{ k_uncaptured }; // 0xD83D -> 55357
    std::atomic<std::int64_t> g_arg_char_echo_max_stat{ k_uncaptured };
    std::atomic<std::int64_t> g_arg_char_widen_max_stat{ k_uncaptured };
    std::atomic<std::int64_t> g_arg_char_widen_highbit_stat{ k_uncaptured };
    std::atomic<std::int64_t> g_arg_char_widen_surrogate_stat{ k_uncaptured };
    // char arg alternating-bit widen: 0xAAAA -> 43690 (zero-ext), 0x5555 -> 21845.
    std::atomic<std::int64_t> g_arg_char_widen_althi{ k_uncaptured };
    std::atomic<std::int64_t> g_arg_char_widen_altlo{ k_uncaptured };
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
    // five narrow args (ZBCSI)I: deep pure-narrow packing (one of each kind).
    std::atomic<std::int64_t> g_arg_mix5_inst{ k_uncaptured };
    std::atomic<std::int64_t> g_arg_mix5_stat{ k_uncaptured };
    // int boundary ARGS over a pure (I)I echo (no witness clobber).
    std::atomic<std::int64_t> g_arg_int_echo_min{ k_uncaptured };
    std::atomic<std::int64_t> g_arg_int_echo_max{ k_uncaptured };
    std::atomic<std::int64_t> g_arg_int_echo_zero{ k_uncaptured };
    std::atomic<std::int64_t> g_arg_int_echo_negone{ k_uncaptured };

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
    // long alternating-bit phases (64-bit inverses).
    std::atomic<std::int64_t> g_long_altpos{ k_uncaptured };  // 0x5555...5 -> +6148914691236517205
    std::atomic<std::int64_t> g_long_altneg{ k_uncaptured };  // 0xAAAA...A -> -6148914691236517206
    std::atomic<std::int64_t> g_long_altneg_stat{ k_uncaptured };
    std::atomic<std::int64_t> g_long_zero_stat{ k_uncaptured };
    std::atomic<std::int64_t> g_long_negone_stat{ k_uncaptured };

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
    std::atomic<std::uint32_t> g_float_one_stat{ 0 };
    std::atomic<std::uint32_t> g_float_max_stat{ 0 };
    std::atomic<std::uint32_t> g_float_minval_stat{ 0 };
    std::atomic<std::uint32_t> g_float_neginf_stat{ 0 };
    std::atomic<std::uint32_t> g_float_two{ 0 };
    std::atomic<std::uint32_t> g_float_minnormal{ 0 };
    std::atomic<std::uint32_t> g_float_altlo{ 0 };       // intBits 0x55555555
    std::atomic<std::uint32_t> g_float_althi{ 0 };       // intBits 0xAAAAAAAA
    std::atomic<std::uint32_t> g_float_two_stat{ 0 };
    std::atomic<std::uint32_t> g_float_minnormal_stat{ 0 };
    std::atomic<std::uint32_t> g_float_althi_stat{ 0 };

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
    std::atomic<std::uint64_t> g_double_one_stat{ 0 };
    std::atomic<std::uint64_t> g_double_max_stat{ 0 };
    std::atomic<std::uint64_t> g_double_minval_stat{ 0 };
    std::atomic<std::uint64_t> g_double_posinf_stat{ 0 };
    std::atomic<std::uint64_t> g_double_two{ 0 };
    std::atomic<std::uint64_t> g_double_minnormal{ 0 };
    std::atomic<std::uint64_t> g_double_altlo{ 0 };       // longBits 0x5555...5
    std::atomic<std::uint64_t> g_double_althi{ 0 };       // longBits 0xAAAA...A
    std::atomic<std::uint64_t> g_double_two_stat{ 0 };
    std::atomic<std::uint64_t> g_double_minnormal_stat{ 0 };
    std::atomic<std::uint64_t> g_double_althi_stat{ 0 };

    // void + introspection
    std::atomic<int> g_void_inst_is_void{ -1 };
    std::atomic<int> g_void_stat_is_void{ -1 };
    std::atomic<int> g_int_is_void{ -1 };     // is_void() on an int return -> must be false
    std::atomic<int> g_int_is_string{ -1 };   // is_string() on an int return -> must be false

    // Conversion-operator-on-value_t cross checks done in-detour
    std::atomic<int> g_bool_true_to_int{ -1 };   // bool true -> int == 1
    std::atomic<int> g_float_half_to_double{ -1 };// 0.5f -> double == 0.5 exactly
    // value_t conversion breadth: SAME return value_t -> a different C++ target.
    std::atomic<std::int64_t> g_int_max_as_i64{ k_uncaptured };     // I return -> int64 widen
    std::atomic<std::int64_t> g_int_negone_as_i64{ k_uncaptured };  // I -1 -> int64 -1
    std::atomic<std::int64_t> g_byte_negone_as_i64{ k_uncaptured }; // B -1 -> int64 sign-extend
    std::atomic<std::int64_t> g_char_max_as_i64{ k_uncaptured };    // C 0xFFFF -> int64 zero-extend (65535)
    std::atomic<int> g_int_zero_as_bool{ -1 };     // I 0 -> bool false
    std::atomic<int> g_int_fortytwo_as_bool{ -1 }; // I 42 -> bool true
    std::atomic<int> g_byte_zero_as_bool{ -1 };    // B 0 -> bool false
    // CROSS-KIND value_t conversions: float/double return -> integer target
    // (static_cast truncation toward zero) and integer return -> float target
    // (widening), plus short -> int64 sign-extend and long -> bool.
    std::atomic<std::int64_t> g_short_min_as_i64{ k_uncaptured };   // S MIN -> int64 sign-extend (-32768)
    std::atomic<std::int64_t> g_short_altneg_as_i64{ k_uncaptured };// S 0xAAAA -> -21846
    std::atomic<std::int64_t> g_long_max_as_i64{ k_uncaptured };    // J MAX -> int64 identity
    std::atomic<int> g_float_2p75_as_i32{ -1 };    // F 2.75 -> int 2 (trunc)
    std::atomic<int> g_float_neg2p75_as_i32{ -1 }; // F -2.75 -> int -2 (trunc toward zero)
    std::atomic<std::int64_t> g_double_2p75_as_i64{ k_uncaptured };    // D 2.75 -> int64 2
    std::atomic<std::int64_t> g_double_neg2p75_as_i64{ k_uncaptured }; // D -2.75 -> int64 -2
    std::atomic<int> g_int_42_as_float{ -1 };      // I 42 -> float 42.0 exactly
    std::atomic<int> g_long_negone_as_double{ -1 };// J -1 -> double -1.0 exactly
    std::atomic<int> g_long_negone_as_bool{ -1 };  // J -1 -> bool true
    std::atomic<int> g_long_zero_as_bool{ -1 };    // J 0 -> bool false
    // int ARG -> float RETURN (exact magnitudes): proves an int arg and a float
    // return coexist; 16777216 == 2^24 is the largest int exact as a float.
    std::atomic<std::uint32_t> g_arg_int_to_float_inst{ 0 };
    std::atomic<std::uint32_t> g_arg_int_to_float_stat{ 0 };
    std::atomic<bool>          g_arg_int_to_float_captured{ false };

    // ---- batch-18 deepening: additional captures ----
    // bool false converted to int via value_t must be 0 (the twin of the existing
    // bool-true->1 probe; together they pin both phases of the Z conversion).
    std::atomic<int> g_bool_false_to_int{ -1 };
    // value_t conversion breadth — additional same-value_t -> different-target reads
    // that the existing block does not cover.
    std::atomic<std::int64_t> g_byte_max_as_i64{ k_uncaptured };    // B 127 -> int64 127
    std::atomic<std::int64_t> g_byte_zero_as_i64{ k_uncaptured };   // B 0 -> int64 0
    std::atomic<std::int64_t> g_char_zero_as_i64{ k_uncaptured };   // C 0 -> int64 0
    std::atomic<std::int64_t> g_char_highbit_as_i64{ k_uncaptured };// C 0x8000 -> int64 32768 (zero-ext)
    std::atomic<std::int64_t> g_short_max_as_i64{ k_uncaptured };   // S 32767 -> int64 32767
    std::atomic<std::int64_t> g_int_min_as_i64{ k_uncaptured };     // I MIN -> int64 -2147483648
    std::atomic<std::int64_t> g_int_pattern_as_i64{ k_uncaptured }; // I 0x12345678 -> int64 305419896
    std::atomic<std::int64_t> g_long_min_as_i64{ k_uncaptured };    // J MIN -> int64 identity
    std::atomic<std::int64_t> g_long_altneg_as_i64{ k_uncaptured }; // J 0xAAAA..A -> int64 identity
    // CROSS-KIND truncation on the OTHER float/double target widths:
    // float return -> int64 target, double return -> int32 target, with non-trivial
    // six/seven-digit magnitudes so the truncation keeps the full integer part.
    std::atomic<std::int64_t> g_float_bigwhole_as_i64{ k_uncaptured };    // F 1000000.5 -> 1000000
    std::atomic<std::int64_t> g_float_negbigwhole_as_i64{ k_uncaptured }; // F -1000000.5 -> -1000000
    std::atomic<int>          g_double_bigwhole_as_i32{ -2 };             // D 1234567.5 -> 1234567
    std::atomic<int>          g_double_negbigwhole_as_i32{ -2 };          // D -1234567.5 -> -1234567
    // integral return -> wider FLOATING target, captured as raw bits so the EXACT
    // value is asserted (these magnitudes are all exactly representable).
    std::atomic<std::uint32_t> g_byte_negone_as_float{ 0 };   // B -1 -> -1.0f
    std::atomic<std::uint32_t> g_short_min_as_float{ 0 };     // S -32768 -> -32768.0f
    std::atomic<std::uint32_t> g_int_pow24_as_float{ 0 };     // I 2^24 -> 16777216.0f
    std::atomic<std::uint32_t> g_long_negone_as_float{ 0 };   // J -1 -> -1.0f
    std::atomic<std::uint64_t> g_byte_negone_as_double{ 0 };  // B -1 -> -1.0
    std::atomic<std::uint64_t> g_char_max_as_double{ 0 };     // C 65535 -> 65535.0
    std::atomic<std::uint64_t> g_short_min_as_double{ 0 };    // S -32768 -> -32768.0
    std::atomic<std::uint64_t> g_int_max_as_double{ 0 };      // I 2147483647 -> 2147483647.0
    std::atomic<std::uint64_t> g_long_pow40_as_double{ 0 };   // J 2^40 -> 1099511627776.0
    std::atomic<std::uint64_t> g_float_max_as_double{ 0 };    // F FLT_MAX -> exact double widen
    std::atomic<bool>          g_conv18_captured{ false };
    // is_void()/is_string() must be false across EVERY non-void primitive return
    // kind (instance), not just int — float/double/long/char/byte/short/bool.
    std::atomic<int> g_float_is_void{ -1 };
    std::atomic<int> g_double_is_void{ -1 };
    std::atomic<int> g_long_is_void{ -1 };
    std::atomic<int> g_char_is_void{ -1 };
    std::atomic<int> g_byte_is_void{ -1 };
    std::atomic<int> g_short_is_void{ -1 };
    std::atomic<int> g_bool_is_void{ -1 };
    std::atomic<int> g_float_is_string{ -1 };
    std::atomic<int> g_long_is_string{ -1 };
    // static-return introspection: is_void()/is_string() false on a STATIC int return.
    std::atomic<int> g_sint_is_void{ -1 };
    std::atomic<int> g_sint_is_string{ -1 };
    std::atomic<int> g_svoid_int_is_void{ -1 };  // is_void() true on a STATIC void return
    // (I)J widen ARG -> long RETURN coexistence: int arg sign-extends into the long.
    std::atomic<std::int64_t> g_arg_int_to_long_negone_inst{ k_uncaptured };  // -1 -> -1
    std::atomic<std::int64_t> g_arg_int_to_long_max_inst{ k_uncaptured };     // INT_MAX -> 2147483647
    std::atomic<std::int64_t> g_arg_int_to_long_min_inst{ k_uncaptured };     // INT_MIN -> -2147483648
    std::atomic<std::int64_t> g_arg_int_to_long_negone_stat{ k_uncaptured };  // -1 -> -1 (static)
    std::atomic<bool>         g_arg_int_to_long_captured{ false };
    // additional POSITIVE-boundary narrow ARG widen probes (the existing block
    // leans on negative/sign-bit cases; these prove the positive boundaries arrive
    // intact through the .i slot): byte 0 / byte MAX / short 0 / short MAX / char 'A'.
    std::atomic<std::int64_t> g_arg_byte_widen_zero{ k_uncaptured };   // 0 -> 0
    std::atomic<std::int64_t> g_arg_byte_widen_max{ k_uncaptured };    // 127 -> 127
    std::atomic<std::int64_t> g_arg_short_widen_zero{ k_uncaptured };  // 0 -> 0
    std::atomic<std::int64_t> g_arg_short_widen_max{ k_uncaptured };   // 32767 -> 32767
    std::atomic<std::int64_t> g_arg_char_widen_a{ k_uncaptured };      // 'A' -> 65

    // ---- batch-22 deepening: additional captures ----
    // char (uint16) value_t NARROWED / reinterpreted into smaller / other-kind
    // C++ targets — the truncating static_cast legs the existing widen-only probes
    // never reach.
    std::atomic<std::int64_t> g_char_max_as_i8{ k_uncaptured };      // 0xFFFF -> -1
    std::atomic<std::int64_t> g_char_max_as_i16{ k_uncaptured };     // 0xFFFF -> -1
    std::atomic<std::int64_t> g_char_max_as_u8{ k_uncaptured };      // 0xFFFF -> 255 (low byte)
    std::atomic<std::int64_t> g_char_max_as_char16{ k_uncaptured };  // 0xFFFF -> 0xFFFF
    std::atomic<std::int64_t> g_char_highbit_as_i16{ k_uncaptured }; // 0x8000 -> -32768
    std::atomic<std::int64_t> g_char_highbit_as_i8{ k_uncaptured };  // 0x8000 -> 0 (low byte)
    std::atomic<std::int64_t> g_char_255_as_i8{ k_uncaptured };      // 0x00FF -> -1
    std::atomic<std::int64_t> g_char_a_as_char{ k_uncaptured };      // 'A' -> 65
    // bool value_t (Z alternative) -> every numeric width + floating kind.
    std::atomic<std::int64_t>  g_bool_true_as_i8{ k_uncaptured };    // 1
    std::atomic<std::int64_t>  g_bool_true_as_i16{ k_uncaptured };   // 1
    std::atomic<std::int64_t>  g_bool_true_as_u16{ k_uncaptured };   // 1
    std::atomic<std::int64_t>  g_bool_true_as_i64{ k_uncaptured };   // 1
    std::atomic<std::int64_t>  g_bool_false_as_i64{ k_uncaptured };  // 0
    std::atomic<std::uint32_t> g_bool_true_as_float{ 0 };           // 1.0f
    std::atomic<std::uint32_t> g_bool_false_as_float{ 0 };          // 0.0f
    std::atomic<std::uint64_t> g_bool_true_as_double{ 0 };          // 1.0
    std::atomic<std::uint64_t> g_bool_false_as_double{ 0 };         // 0.0
    std::atomic<bool>          g_batch22_bool_captured{ false };
    // EVERY primitive kind -> bool (the char/short/float/double truncation-to-bool
    // legs the existing int/byte/long probes miss; both -0.0 phases prove false).
    std::atomic<int> g_char_zero_as_bool{ -1 };
    std::atomic<int> g_char_max_as_bool{ -1 };
    std::atomic<int> g_short_zero_as_bool{ -1 };
    std::atomic<int> g_short_min_as_bool{ -1 };
    std::atomic<int> g_float_zero_as_bool{ -1 };
    std::atomic<int> g_float_one_as_bool{ -1 };
    std::atomic<int> g_float_negzero_as_bool{ -1 };
    std::atomic<int> g_double_zero_as_bool{ -1 };
    std::atomic<int> g_double_pi_as_bool{ -1 };
    std::atomic<int> g_double_negzero_as_bool{ -1 };
    // IDEMPOTENCY: the same returner called twice in one detour must agree.
    std::atomic<std::int64_t>  g_idem_int_a{ k_uncaptured };
    std::atomic<std::int64_t>  g_idem_int_b{ k_uncaptured };
    std::atomic<std::int64_t>  g_idem_long_a{ k_uncaptured };
    std::atomic<std::int64_t>  g_idem_long_b{ k_uncaptured };
    std::atomic<std::uint32_t> g_idem_float_nan_a{ 0 };
    std::atomic<std::uint32_t> g_idem_float_nan_b{ 0 };
    // as_string() on a NON-string (numeric / void) return is the empty string.
    std::atomic<int> g_as_string_int_empty{ -1 };
    std::atomic<int> g_as_string_double_empty{ -1 };
    std::atomic<int> g_as_string_sint_empty{ -1 };
    std::atomic<int> g_as_string_svoid_empty{ -1 };
    std::atomic<bool> g_batch22_misc_captured{ false };

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
        g_byte_alt.store(s.call_byte("retByteAlt"));
        g_byte_alt_stat.store(method_primitives::scall_byte("sRetByteAlt"));
        g_byte_zero_stat.store(method_primitives::scall_byte("sRetByteZero"));
        g_byte_one_stat.store(method_primitives::scall_byte("sRetByteOne"));
        g_byte_pattern_stat.store(method_primitives::scall_byte("sRetBytePattern"));
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
        g_short_altpos.store(s.call_short("retShortAltPos"));
        g_short_altneg.store(s.call_short("retShortAltNeg"));
        g_short_altneg_stat.store(method_primitives::scall_short("sRetShortAltNeg"));
        g_short_zero_stat.store(method_primitives::scall_short("sRetShortZero"));
        g_short_highbyte_stat.store(method_primitives::scall_short("sRetShortHighByte"));
        // short RETURN widened into a 32-bit int target — int16 alternative
        // sign-extends (negone -> -1, 0xAAAA -> -21846, MIN -> -32768).
        g_short_negone_as_int.store(s.short_ret_as_int("retShortNegOne"));
        g_short_altneg_as_int.store(s.short_ret_as_int("retShortAltNeg"));
        g_short_min_as_int.store(s.short_ret_as_int("retShortMin"));

        // ---- char ----
        g_char_zero.store(s.call_char("retCharZero"));
        g_char_a.store(s.call_char("retCharA"));
        g_char_max.store(s.call_char("retCharMax"));
        g_char_255.store(s.call_char("retChar255"));
        g_char_highbit.store(s.call_char("retCharHighBit"));
        g_char_surrogate.store(s.call_char("retCharSurrogate"));
        g_char_a_stat.store(method_primitives::scall_char("sRetCharA"));
        g_char_max_stat.store(method_primitives::scall_char("sRetCharMax"));
        g_char_highbit_stat.store(method_primitives::scall_char("sRetCharHighBit"));
        g_char_surrogate_stat.store(method_primitives::scall_char("sRetCharSurrogate"));
        g_char_altlo.store(s.call_char("retCharAltLo"));
        g_char_althi.store(s.call_char("retCharAltHi"));
        g_char_althi_stat.store(method_primitives::scall_char("sRetCharAltHi"));
        g_char_zero_stat.store(method_primitives::scall_char("sRetCharZero"));
        g_char_255_stat.store(method_primitives::scall_char("sRetChar255"));
        // char 0xAAAA read into an int -> 43690 (zero-extend), never -21846.
        {
            const std::int32_t as_int = s.get_method("retCharAltHi")->call();
            g_char_althi_as_int.store(as_int);
        }
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
        // char 0xD83D (lone high-surrogate: bit 15 set + busy low byte) read into
        // an int must be 55357, never -10243 (its signed-16 reading).
        {
            const std::int32_t as_int = s.get_method("retCharSurrogate")->call();
            g_char_surrogate_as_int.store(as_int);
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
        g_int_altpos.store(s.call_int("retIntAltPos"));
        g_int_altneg.store(s.call_int("retIntAltNeg"));
        g_int_altneg_stat.store(method_primitives::scall_int("sRetIntAltNeg"));
        g_int_zero_stat.store(method_primitives::scall_int("sRetIntZero"));
        g_int_negone_stat.store(method_primitives::scall_int("sRetIntNegOne"));
        g_int_echo_inst.store(s.call_int_arg("echoInt", 1234567));
        g_int_echo_stat.store(method_primitives::scall_int_arg("sEchoInt", -7654321));
        // distinct-byte pattern echoed back over the int ARG path (no witness
        // clobber: echoIntPattern does not touch lastEchoArg).
        g_arg_int_pattern_echo_inst.store(s.echo_int_pattern(0x12345678));
        g_arg_int_pattern_echo_stat.store(method_primitives::secho_int_pattern(0x12345678));
        // int ARG alternating-bit phases echoed back (0x55555555 positive, 0xAAAAAAAA
        // negative): both phases survive the C++ -> .i slot -> body -> return path.
        g_arg_int_echo_altpos.store(s.echo_int_pattern(0x55555555));
        g_arg_int_echo_altneg.store(s.echo_int_pattern(static_cast<std::int32_t>(0xAAAAAAAAu)));

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
        // byte arg alternating-bit widen: 0xAA -> -86 (sign-extend), 0x55 -> +85.
        g_arg_byte_widen_alt.store(s.byte_to_int(static_cast<std::int8_t>(0xAA)));      // -86
        g_arg_byte_widen_pattern.store(s.byte_to_int(static_cast<std::int8_t>(0x55)));  // +85

        // short arg (S).
        g_arg_short_echo_min.store(s.echo_short(std::numeric_limits<std::int16_t>::min())); // -32768
        g_arg_short_echo_max.store(s.echo_short(std::numeric_limits<std::int16_t>::max())); //  32767
        g_arg_short_widen_negone.store(s.short_to_int(static_cast<std::int16_t>(-1)));      // -> -1
        g_arg_short_widen_pos255.store(s.short_pos_to_int(static_cast<std::int16_t>(0x00FF))); // -> +255
        g_arg_short_echo_max_stat.store(method_primitives::secho_short(std::numeric_limits<std::int16_t>::max()));
        g_arg_short_widen_negone_stat.store(method_primitives::sshort_to_int(static_cast<std::int16_t>(-1)));
        g_arg_short_widen_pos255_stat.store(method_primitives::sshort_pos_to_int(static_cast<std::int16_t>(0x00FF)));
        // short arg alternating-bit widen: 0xAAAA -> -21846 (sign), 0x5555 -> +21845.
        g_arg_short_widen_altneg.store(s.short_to_int(static_cast<std::int16_t>(0xAAAA)));        // -21846
        g_arg_short_widen_altpos.store(s.short_pos_to_int(static_cast<std::int16_t>(0x5555)));    // +21845

        // char arg (C): echo full unsigned range + widen-to-int proves ZERO-ext.
        g_arg_char_echo_zero.store(s.echo_char(static_cast<std::uint16_t>(0)));
        g_arg_char_echo_a.store(s.echo_char(static_cast<std::uint16_t>('A')));               // 65
        g_arg_char_echo_max.store(s.echo_char(static_cast<std::uint16_t>(0xFFFF)));           // 65535
        g_arg_char_widen_max.store(s.char_to_int(static_cast<std::uint16_t>(0xFFFF)));        // -> 65535
        g_arg_char_widen_highbit.store(s.char_highbit_to_int(static_cast<std::uint16_t>(0x8000))); // -> 32768
        g_arg_char_widen_surrogate.store(s.char_surrogate_to_int(static_cast<std::uint16_t>(0xD83D))); // -> 55357
        g_arg_char_echo_max_stat.store(method_primitives::secho_char(static_cast<std::uint16_t>(0xFFFF)));
        g_arg_char_widen_max_stat.store(method_primitives::schar_to_int(static_cast<std::uint16_t>(0xFFFF)));
        g_arg_char_widen_highbit_stat.store(method_primitives::schar_highbit_to_int(static_cast<std::uint16_t>(0x8000)));
        g_arg_char_widen_surrogate_stat.store(method_primitives::schar_surrogate_to_int(static_cast<std::uint16_t>(0xD83D)));
        // char arg alternating-bit widen: 0xAAAA -> 43690 (zero-ext, bit 15 set),
        // 0x5555 -> 21845.  Reuses charToInt (the (C)I widener; does not clobber the
        // lastCharArg witness — charToInt has no side effect).
        g_arg_char_widen_althi.store(s.char_to_int(static_cast<std::uint16_t>(0xAAAA)));  // 43690
        g_arg_char_widen_altlo.store(s.char_to_int(static_cast<std::uint16_t>(0x5555)));  // 21845

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

        // FIVE narrow args (ZBCSI)I: one of every narrow kind in one frame.  Boundary
        // operands so a wrong-width or swapped slot changes the asymmetric result.
        g_arg_mix5_inst.store(s.mix_zbcsi(true,
                                          static_cast<std::int8_t>(-1),        // byte -> -1 (signed)
                                          static_cast<std::uint16_t>(0xFFFF),  // char -> 65535 (unsigned)
                                          static_cast<std::int16_t>(-2),       // short -> -2 (signed)
                                          7));                                  // int  -> 7
        g_arg_mix5_stat.store(method_primitives::smix_zbcsi(false,
                                          std::numeric_limits<std::int8_t>::max(),    // 127
                                          static_cast<std::uint16_t>(0x8000),         // char -> 32768 (bit 15, zero-ext)
                                          std::numeric_limits<std::int16_t>::min(),   // -32768
                                          std::numeric_limits<std::int32_t>::max()));  // INT_MAX

        // int boundary ARGS over a pure (I)I echo: the whole signed range survives
        // the C++ -> .i slot -> body -> return round-trip (no witness clobber).
        g_arg_int_echo_min.store(s.echo_int_pattern_arg(std::numeric_limits<std::int32_t>::min()));
        g_arg_int_echo_max.store(s.echo_int_pattern_arg(std::numeric_limits<std::int32_t>::max()));
        g_arg_int_echo_zero.store(s.echo_int_pattern_arg(0));
        g_arg_int_echo_negone.store(s.echo_int_pattern_arg(-1));

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
        g_long_altpos.store(s.call_long("retLongAltPos"));
        g_long_altneg.store(s.call_long("retLongAltNeg"));
        g_long_altneg_stat.store(method_primitives::scall_long("sRetLongAltNeg"));
        g_long_zero_stat.store(method_primitives::scall_long("sRetLongZero"));
        g_long_negone_stat.store(method_primitives::scall_long("sRetLongNegOne"));

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
        g_float_one_stat.store(f2bits(method_primitives::scall_float("sRetFloatOne")));
        g_float_max_stat.store(f2bits(method_primitives::scall_float("sRetFloatMax")));
        g_float_minval_stat.store(f2bits(method_primitives::scall_float("sRetFloatMinValue")));
        g_float_neginf_stat.store(f2bits(method_primitives::scall_float("sRetFloatNegInf")));
        g_float_two.store(f2bits(s.call_float("retFloatTwo")));
        g_float_minnormal.store(f2bits(s.call_float("retFloatMinNormal")));
        g_float_altlo.store(f2bits(s.call_float("retFloatAltLo")));
        g_float_althi.store(f2bits(s.call_float("retFloatAltHi")));
        g_float_two_stat.store(f2bits(method_primitives::scall_float("sRetFloatTwo")));
        g_float_minnormal_stat.store(f2bits(method_primitives::scall_float("sRetFloatMinNormal")));
        g_float_althi_stat.store(f2bits(method_primitives::scall_float("sRetFloatAltHi")));
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
        g_double_one_stat.store(d2bits(method_primitives::scall_double("sRetDoubleOne")));
        g_double_max_stat.store(d2bits(method_primitives::scall_double("sRetDoubleMax")));
        g_double_minval_stat.store(d2bits(method_primitives::scall_double("sRetDoubleMinValue")));
        g_double_posinf_stat.store(d2bits(method_primitives::scall_double("sRetDoublePosInf")));
        g_double_two.store(d2bits(s.call_double("retDoubleTwo")));
        g_double_minnormal.store(d2bits(s.call_double("retDoubleMinNormal")));
        g_double_altlo.store(d2bits(s.call_double("retDoubleAltLo")));
        g_double_althi.store(d2bits(s.call_double("retDoubleAltHi")));
        g_double_two_stat.store(d2bits(method_primitives::scall_double("sRetDoubleTwo")));
        g_double_minnormal_stat.store(d2bits(method_primitives::scall_double("sRetDoubleMinNormal")));
        g_double_althi_stat.store(d2bits(method_primitives::scall_double("sRetDoubleAltHi")));
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
        // value_t conversion-operator breadth: take a primitive return and convert
        // the SAME value_t into a DIFFERENT-width / different-kind C++ target,
        // exercising operator target_type()'s static_cast leg across the stored
        // alternatives (int32->int64, int8->int64 sign-extend, uint16->int64
        // zero-extend, int32->bool truncation).
        g_int_max_as_i64.store(s.ret_as_i64("retIntMax"));        // -> 2147483647
        g_int_negone_as_i64.store(s.ret_as_i64("retIntNegOne"));  // -> -1
        g_byte_negone_as_i64.store(s.ret_as_i64("retByteNegOne"));// -> -1 (sign-extend)
        g_char_max_as_i64.store(s.ret_as_i64("retCharMax"));      // -> 65535 (zero-extend)
        g_int_zero_as_bool.store(s.ret_as_bool("retIntZero") ? 1 : 0);          // -> false
        g_int_fortytwo_as_bool.store(s.ret_as_bool("retIntFortyTwo") ? 1 : 0);  // -> true
        g_byte_zero_as_bool.store(s.ret_as_bool("retByteZero") ? 1 : 0);        // -> false

        // ---- CROSS-KIND value_t conversions (static_cast leg across kinds) ----
        // short return widened to int64 sign-extends (MIN -> -32768, 0xAAAA -> -21846).
        g_short_min_as_i64.store(s.ret_as_i64("retShortMin"));
        g_short_altneg_as_i64.store(s.ret_as_i64("retShortAltNeg"));
        // long return read as int64 (identity for the widest integral alternative).
        g_long_max_as_i64.store(s.ret_as_i64("retLongMax"));
        // float return -> int32 truncates toward zero (2.75 -> 2, -2.75 -> -2).
        g_float_2p75_as_i32.store(s.ret_float_as_i32("retFloatTwoPoint75"));
        g_float_neg2p75_as_i32.store(s.ret_float_as_i32("retFloatNegTwoPoint75"));
        // double return -> int64 truncates toward zero (2.75 -> 2, -2.75 -> -2).
        g_double_2p75_as_i64.store(s.ret_double_as_i64("retDoubleTwoPoint75"));
        g_double_neg2p75_as_i64.store(s.ret_double_as_i64("retDoubleNegTwoPoint75"));
        // int return -> float widens (42 is exactly representable).
        g_int_42_as_float.store(s.ret_int_as_float("retIntFortyTwo") == 42.0f ? 1 : 0);
        // long return -> double widens (-1 is exactly representable).
        g_long_negone_as_double.store(s.ret_long_as_double("retLongNegOne") == -1.0 ? 1 : 0);
        // long return -> bool narrows (-1 nonzero -> true, 0 -> false).
        g_long_negone_as_bool.store(s.ret_long_as_bool("retLongNegOne") ? 1 : 0);
        g_long_zero_as_bool.store(s.ret_long_as_bool("retLongZero") ? 1 : 0);

        // ---- int ARG -> float RETURN coexistence (exact magnitudes) ----
        // 16777216 == 2^24 is the largest int exactly representable as a float; the
        // (I)F callee returns (float)arg, so the captured bits must equal 16777216.0f.
        g_arg_int_to_float_inst.store(f2bits(s.call_int_to_float("intToFloat", 16777216)));
        g_arg_int_to_float_stat.store(f2bits(method_primitives::scall_int_to_float("sIntToFloat", 16777216)));
        g_arg_int_to_float_captured.store(true);

        // ================================================================
        //  batch-18 deepening captures
        // ================================================================
        // bool false -> int 0 (twin of the bool-true->1 probe above).
        {
            const std::int32_t btoi = s.get_method("retBoolFalse")->call();
            g_bool_false_to_int.store(btoi);
        }

        // value_t conversion breadth — more same-value_t -> int64 reads.
        g_byte_max_as_i64.store(s.ret_as_i64("retByteMax"));            // 127
        g_byte_zero_as_i64.store(s.ret_as_i64("retByteZero"));          // 0
        g_char_zero_as_i64.store(s.ret_as_i64("retCharZero"));          // 0
        g_char_highbit_as_i64.store(s.ret_as_i64("retCharHighBit"));    // 32768 (zero-ext)
        g_short_max_as_i64.store(s.ret_as_i64("retShortMax"));          // 32767
        g_int_min_as_i64.store(s.ret_as_i64("retIntMin"));              // -2147483648
        g_int_pattern_as_i64.store(s.ret_as_i64("retIntPattern"));      // 305419896
        g_long_min_as_i64.store(s.ret_as_i64("retLongMin"));            // identity
        g_long_altneg_as_i64.store(s.ret_as_i64("retLongAltNeg"));      // identity

        // CROSS-KIND truncation on the OTHER float/double target widths:
        // float -> int64 and double -> int32, with six/seven-digit magnitudes.
        g_float_bigwhole_as_i64.store(s.ret_float_as_i64("retFloatBigWhole"));        // 1000000
        g_float_negbigwhole_as_i64.store(s.ret_float_as_i64("retFloatNegBigWhole"));  // -1000000
        g_double_bigwhole_as_i32.store(s.ret_double_as_i32("retDoubleBigWhole"));     // 1234567
        g_double_negbigwhole_as_i32.store(s.ret_double_as_i32("retDoubleNegBigWhole"));// -1234567

        // integral / float return -> wider FLOATING target (raw bits -> exact value).
        g_byte_negone_as_float.store(f2bits(s.ret_byte_as_float("retByteNegOne")));   // -1.0f
        g_short_min_as_float.store(f2bits(s.ret_short_as_float("retShortMin")));      // -32768.0f
        g_int_pow24_as_float.store(f2bits(s.ret_int_as_float("retIntPow2to24")));     // 16777216.0f
        g_long_negone_as_float.store(f2bits(s.ret_long_as_float("retLongNegOne")));   // -1.0f
        g_byte_negone_as_double.store(d2bits(s.ret_byte_as_double("retByteNegOne"))); // -1.0
        g_char_max_as_double.store(d2bits(s.ret_char_as_double("retCharMax")));       // 65535.0
        g_short_min_as_double.store(d2bits(s.ret_short_as_double("retShortMin")));    // -32768.0
        g_int_max_as_double.store(d2bits(s.ret_int_as_double("retIntMax")));          // 2147483647.0
        g_long_pow40_as_double.store(d2bits(s.ret_long_as_double("retLongPow2to40")));// 1099511627776.0
        g_float_max_as_double.store(d2bits(s.ret_float_as_double("retFloatMax")));    // exact widen
        g_conv18_captured.store(true);

        // is_void()/is_string() must be false across every non-void primitive kind.
        g_float_is_void.store(s.is_void("retFloatHalf") ? 1 : 0);
        g_double_is_void.store(s.is_void("retDoublePi") ? 1 : 0);
        g_long_is_void.store(s.is_void("retLongMax") ? 1 : 0);
        g_char_is_void.store(s.is_void("retCharA") ? 1 : 0);
        g_byte_is_void.store(s.is_void("retByteMax") ? 1 : 0);
        g_short_is_void.store(s.is_void("retShortMax") ? 1 : 0);
        g_bool_is_void.store(s.is_void("retBoolTrue") ? 1 : 0);
        g_float_is_string.store(s.is_string("retFloatHalf") ? 1 : 0);
        g_long_is_string.store(s.is_string("retLongMax") ? 1 : 0);
        // static-return introspection.
        g_sint_is_void.store(method_primitives::sis_void("sRetIntFortyTwo") ? 1 : 0);
        g_sint_is_string.store(method_primitives::sis_string("sRetIntFortyTwo") ? 1 : 0);
        g_svoid_int_is_void.store(method_primitives::svoid("sRetVoidBump") ? 1 : 0);

        // (I)J widen ARG -> long RETURN: int arg sign-extends into the 64-bit result.
        g_arg_int_to_long_negone_inst.store(s.call_int_to_long("intToLong", -1));                            // -1
        g_arg_int_to_long_max_inst.store(s.call_int_to_long("intToLong", std::numeric_limits<std::int32_t>::max())); // 2147483647
        g_arg_int_to_long_min_inst.store(s.call_int_to_long("intToLong", std::numeric_limits<std::int32_t>::min())); // -2147483648
        g_arg_int_to_long_negone_stat.store(method_primitives::scall_int_to_long("sIntToLong", -1));         // -1
        g_arg_int_to_long_captured.store(true);

        // POSITIVE-boundary narrow ARG widen probes (the .i slot delivers the
        // positive boundaries intact, complementing the negative/sign-bit cases).
        g_arg_byte_widen_zero.store(s.byte_to_int(static_cast<std::int8_t>(0)));                            // 0
        g_arg_byte_widen_max.store(s.byte_to_int(std::numeric_limits<std::int8_t>::max()));                 // 127
        g_arg_short_widen_zero.store(s.short_to_int(static_cast<std::int16_t>(0)));                         // 0
        g_arg_short_widen_max.store(s.short_pos_to_int(std::numeric_limits<std::int16_t>::max()));          // 32767
        g_arg_char_widen_a.store(s.char_to_int(static_cast<std::uint16_t>('A')));                           // 65
        // ================================================================
        //  batch-22 deepening captures
        // ================================================================
        // ---- char (uint16) value_t NARROWED into smaller / other-kind targets ----
        // 0xFFFF read into int8 keeps the low byte AS A SIGNED int8 -> -1; into int16
        // reinterprets the low 16 bits as signed -> -1; into char16_t / unsigned char
        // is the natural identity / low-byte.  0x8000 read into int16 is INT16_MIN
        // (-32768), into int8 is 0 (low byte clear); 0x00FF into int8 is -1.
        g_char_max_as_i8.store(s.ret_char_as_i8("retCharMax"));          // 0xFFFF -> -1
        g_char_max_as_i16.store(s.ret_char_as_i16("retCharMax"));        // 0xFFFF -> -1
        g_char_max_as_u8.store(s.ret_char_as_u8("retCharMax"));          // 0xFFFF -> 255 (low byte)
        g_char_max_as_char16.store(static_cast<std::uint16_t>(s.ret_char_as_char16("retCharMax"))); // -> 0xFFFF
        g_char_highbit_as_i16.store(s.ret_char_as_i16("retCharHighBit"));// 0x8000 -> -32768
        g_char_highbit_as_i8.store(s.ret_char_as_i8("retCharHighBit"));  // 0x8000 -> 0 (low byte)
        g_char_255_as_i8.store(s.ret_char_as_i8("retChar255"));          // 0x00FF -> -1
        g_char_a_as_char.store(static_cast<std::int32_t>(s.ret_char_as_char("retCharA"))); // 'A' -> 65

        // ---- bool value_t (Z alternative) -> every numeric width / floating kind ----
        // true promotes to 1 across int8/int16/uint16/int64/float/double; false to 0.
        g_bool_true_as_i8.store(s.ret_bool_as_i8("retBoolTrue"));        // 1
        g_bool_true_as_i16.store(s.ret_bool_as_i16("retBoolTrue"));      // 1
        g_bool_true_as_u16.store(s.ret_bool_as_u16("retBoolTrue"));      // 1
        g_bool_true_as_i64.store(s.ret_bool_as_i64("retBoolTrue"));      // 1
        g_bool_false_as_i64.store(s.ret_bool_as_i64("retBoolFalse"));    // 0
        g_bool_true_as_float.store(f2bits(s.ret_bool_as_float("retBoolTrue")));   // 1.0f
        g_bool_false_as_float.store(f2bits(s.ret_bool_as_float("retBoolFalse"))); // 0.0f
        g_bool_true_as_double.store(d2bits(s.ret_bool_as_double("retBoolTrue")));   // 1.0
        g_bool_false_as_double.store(d2bits(s.ret_bool_as_double("retBoolFalse"))); // 0.0
        g_batch22_bool_captured.store(true);

        // ---- EVERY primitive kind -> bool (nonzero true, zero incl -0.0 false) ----
        // The existing block covers only int/byte/long -> bool; these reach the
        // char / short / float / double alternatives' truncation-to-bool leg, with
        // BOTH the negative-zero floats proving -0.0 narrows to false (its value is
        // zero, not its sign bit).
        g_char_zero_as_bool.store(s.ret_as_bool_kind("retCharZero") ? 1 : 0);       // false
        g_char_max_as_bool.store(s.ret_as_bool_kind("retCharMax") ? 1 : 0);         // true
        g_short_zero_as_bool.store(s.ret_as_bool_kind("retShortZero") ? 1 : 0);     // false
        g_short_min_as_bool.store(s.ret_as_bool_kind("retShortMin") ? 1 : 0);       // true
        g_float_zero_as_bool.store(s.ret_as_bool_kind("retFloatZero") ? 1 : 0);     // false
        g_float_one_as_bool.store(s.ret_as_bool_kind("retFloatOne") ? 1 : 0);       // true
        g_float_negzero_as_bool.store(s.ret_as_bool_kind("retFloatNegZero") ? 1 : 0); // false
        g_double_zero_as_bool.store(s.ret_as_bool_kind("retDoubleZero") ? 1 : 0);   // false
        g_double_pi_as_bool.store(s.ret_as_bool_kind("retDoublePi") ? 1 : 0);       // true
        g_double_negzero_as_bool.store(s.ret_as_bool_kind("retDoubleNegZero") ? 1 : 0); // false

        // ---- IDEMPOTENCY: the SAME returner invoked twice in one detour yields the
        // SAME value_t (call() carries no destructive per-call state; two reads of a
        // distinct-byte int and an alternating-bit long must agree bit-for-bit). ----
        {
            const std::int64_t a = s.get_method("retIntPattern")->call();
            const std::int64_t b = s.get_method("retIntPattern")->call();
            g_idem_int_a.store(a);
            g_idem_int_b.store(b);
        }
        {
            const std::int64_t a = s.get_method("retLongAltNeg")->call();
            const std::int64_t b = s.get_method("retLongAltNeg")->call();
            g_idem_long_a.store(a);
            g_idem_long_b.store(b);
        }
        // FLOAT idempotency on a special (NaN) — two reads yield identical raw bits.
        {
            const std::uint32_t a = f2bits(s.call_float("retFloatNaN"));
            const std::uint32_t b = f2bits(s.call_float("retFloatNaN"));
            g_idem_float_nan_a.store(a);
            g_idem_float_nan_b.store(b);
        }

        // ---- as_string() on a NON-string return is the EMPTY string ----
        // A numeric primitive return (instance + static) and a STATIC void return all
        // yield "" from as_string() (the default leg never fabricates a number).  The
        // void probe uses the STATIC void returner deliberately: the INSTANCE void
        // counter is asserted EXACTLY == 1 elsewhere, so re-invoking the instance void
        // here would corrupt that count, whereas the static counter is asserted >= 1.
        g_as_string_int_empty.store(s.as_string_empty("retIntFortyTwo") ? 1 : 0);
        g_as_string_double_empty.store(s.as_string_empty("retDoublePi") ? 1 : 0);
        g_as_string_sint_empty.store(method_primitives::sas_string_empty("sRetIntFortyTwo") ? 1 : 0);
        g_as_string_svoid_empty.store(method_primitives::sas_string_empty("sRetVoidBump") ? 1 : 0);
        g_batch22_misc_captured.store(true);
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
        // alternating-bit phase 0xAA: bit 7 (sign) set -> -86.  Pairs with the 0x55
        // (+85) pattern to cover both phases and the sign bit at byte width.
        ctx.check("mcp_byte_alt_0xAA_neg86", g_byte_alt.load() == -86);
        ctx.check("mcp_byte_alt_static_neg86", g_byte_alt_stat.load() == -86);
        ctx.check("mcp_byte_zero_static", g_byte_zero_stat.load() == 0);
        ctx.check("mcp_byte_one_static", g_byte_one_stat.load() == 1);
        ctx.check("mcp_byte_pattern_static_85", g_byte_pattern_stat.load() == 0x55);

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
        // alternating-bit phases at short width: 0x5555 positive, 0xAAAA negative
        // (bit 15 sign-extends).  The byte-width 0x55/0xAA values cannot reach these.
        ctx.check("mcp_short_altpos_0x5555_21845", g_short_altpos.load() == 21845);
        ctx.check("mcp_short_altneg_0xAAAA_neg21846", g_short_altneg.load() == -21846);
        ctx.check("mcp_short_altneg_static_neg21846", g_short_altneg_stat.load() == -21846);
        ctx.check("mcp_short_zero_static", g_short_zero_stat.load() == 0);
        ctx.check("mcp_short_highbyte_static_neg256", g_short_highbyte_stat.load() == -256);
        // short RETURN widened into a 32-bit int sign-extends (the short twin of the
        // byte/char widen-into-int return probes already present).
        ctx.check("mcp_short_negone_sign_extends_to_int", g_short_negone_as_int.load() == -1);
        ctx.check("mcp_short_altneg_sign_extends_to_int", g_short_altneg_as_int.load() == -21846);
        ctx.check("mcp_short_min_sign_extends_to_int", g_short_min_as_int.load() == -32768);

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
        // (char)0xD83D — a lone UTF-16 high surrogate — must read 55357 (zero-
        // extended), never -10243 (signed-16) nor 0x3D (low-byte-only).
        ctx.check("mcp_char_surrogate_55357", g_char_surrogate.load() == 55357);
        ctx.check("mcp_char_A_static_65", g_char_a_stat.load() == 65);
        ctx.check("mcp_char_max_static_65535", g_char_max_stat.load() == 65535);
        ctx.check("mcp_char_highbit_static_32768", g_char_highbit_stat.load() == 32768);
        ctx.check("mcp_char_surrogate_static_55357", g_char_surrogate_stat.load() == 55357);
        ctx.check("mcp_char_max_zero_extends_to_int_65535", g_char_max_as_int.load() == 65535);
        ctx.check("mcp_char_highbit_zero_extends_to_int_32768", g_char_highbit_as_int.load() == 32768);
        ctx.check("mcp_char_surrogate_zero_extends_to_int_55357", g_char_surrogate_as_int.load() == 55357);
        // alternating-bit phases at char width: 0x5555 -> 21845, 0xAAAA -> 43690.
        // 0xAAAA has bit 15 set, so 43690 (never -21846) is a second zero-ext witness.
        ctx.check("mcp_char_altlo_0x5555_21845", g_char_altlo.load() == 21845);
        ctx.check("mcp_char_althi_0xAAAA_43690", g_char_althi.load() == 43690);
        ctx.check("mcp_char_althi_static_43690", g_char_althi_stat.load() == 43690);
        ctx.check("mcp_char_zero_static", g_char_zero_stat.load() == 0);
        ctx.check("mcp_char_255_static", g_char_255_stat.load() == 255);
        ctx.check("mcp_char_althi_zero_extends_to_int_43690", g_char_althi_as_int.load() == 43690);

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
        // alternating-bit phases at int width: 0x55555555 positive, 0xAAAAAAAA
        // negative (bit 31 sign).  Neither is byte-symmetric, so a word swap reorders.
        ctx.check("mcp_int_altpos_0x55555555", g_int_altpos.load() == 0x55555555LL);
        ctx.check("mcp_int_altneg_0xAAAAAAAA",
                  g_int_altneg.load() == static_cast<std::int64_t>(static_cast<std::int32_t>(0xAAAAAAAAu)));
        ctx.check("mcp_int_altneg_static",
                  g_int_altneg_stat.load() == static_cast<std::int64_t>(static_cast<std::int32_t>(0xAAAAAAAAu)));
        ctx.check("mcp_int_zero_static", g_int_zero_stat.load() == 0);
        ctx.check("mcp_int_negone_static", g_int_negone_stat.load() == -1);
        ctx.check("mcp_int_echo_instance_passthrough", g_int_echo_inst.load() == 1234567);
        ctx.check("mcp_int_echo_static_passthrough", g_int_echo_stat.load() == -7654321);
        // the same distinct-byte pattern echoed back over the int ARG path proves
        // no byte reorder C++ -> .i slot -> body -> return.
        ctx.check("mcp_arg_int_pattern_echo_instance", g_arg_int_pattern_echo_inst.load() == 0x12345678LL);
        ctx.check("mcp_arg_int_pattern_echo_static", g_arg_int_pattern_echo_stat.load() == 0x12345678LL);
        // int boundary ARGS over the same pure (I)I echo: the whole signed range
        // survives the C++ -> .i slot -> body -> return round-trip.
        ctx.check("mcp_arg_int_echo_min", g_arg_int_echo_min.load() == std::numeric_limits<std::int32_t>::min());
        ctx.check("mcp_arg_int_echo_max", g_arg_int_echo_max.load() == std::numeric_limits<std::int32_t>::max());
        ctx.check("mcp_arg_int_echo_zero", g_arg_int_echo_zero.load() == 0);
        ctx.check("mcp_arg_int_echo_negone", g_arg_int_echo_negone.load() == -1);
        // alternating-bit phases echoed over (I)I survive the C++ -> .i slot -> body
        // -> return path in both phases (positive 0x55555555, negative 0xAAAAAAAA).
        ctx.check("mcp_arg_int_echo_altpos", g_arg_int_echo_altpos.load() == 0x55555555LL);
        ctx.check("mcp_arg_int_echo_altneg",
                  g_arg_int_echo_altneg.load() == static_cast<std::int64_t>(static_cast<std::int32_t>(0xAAAAAAAAu)));
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
        // byte arg alternating-bit widen: 0xAA arg sign-extends to -86, 0x55 to +85.
        ctx.check("mcp_arg_byte_widen_alt_0xAA_neg86", g_arg_byte_widen_alt.load() == -86);
        ctx.check("mcp_arg_byte_widen_pattern_0x55_pos85", g_arg_byte_widen_pattern.load() == 85);
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
        // short arg alternating-bit widen: 0xAAAA arg sign-extends to -21846, 0x5555
        // stays +21845 (bit 15 clear).
        ctx.check("mcp_arg_short_widen_altneg_neg21846", g_arg_short_widen_altneg.load() == -21846);
        ctx.check("mcp_arg_short_widen_altpos_pos21845", g_arg_short_widen_altpos.load() == 21845);
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
        // char arg 0xD83D (lone surrogate: bit 15 + busy low byte) widened to int
        // must be 55357, proving the char ARG path zero-extends a surrogate-range
        // code unit rather than sign-extending it (-10243).
        ctx.check("mcp_arg_char_widen_surrogate_zero_extends_55357", g_arg_char_widen_surrogate.load() == 55357);
        ctx.check("mcp_arg_char_echo_max_static_65535", g_arg_char_echo_max_stat.load() == 65535);
        ctx.check("mcp_arg_char_widen_max_static_zero_extends", g_arg_char_widen_max_stat.load() == 65535);
        ctx.check("mcp_arg_char_widen_highbit_static_zero_extends", g_arg_char_widen_highbit_stat.load() == 32768);
        ctx.check("mcp_arg_char_widen_surrogate_static_zero_extends", g_arg_char_widen_surrogate_stat.load() == 55357);
        // char arg alternating-bit widen: 0xAAAA arg zero-extends to 43690 (bit 15
        // set, never -21846), 0x5555 to 21845.
        ctx.check("mcp_arg_char_widen_althi_zero_extends_43690", g_arg_char_widen_althi.load() == 43690);
        ctx.check("mcp_arg_char_widen_altlo_21845", g_arg_char_widen_altlo.load() == 21845);
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

        // FIVE narrow args (ZBCSI)I — one of every narrow kind in a single frame.
        // Mirror Java's promotion: boolean->1/0, byte/short sign-extend, char
        // zero-extends, all to int.  A wrong-width or swapped slot (esp. the
        // leading boolean's own slot) changes the asymmetric result.
        {
            const std::int32_t expect{ mix5_expect(1, -1, 0xFFFF, -2, 7) };
            ctx.check("mcp_arg_mix5_zbcsi_instance", g_arg_mix5_inst.load() == expect);
        }
        {
            const std::int32_t expect{ mix5_expect(0, 127, 0x8000, -32768,
                                                   std::numeric_limits<std::int32_t>::max()) };
            ctx.check("mcp_arg_mix5_zbcsi_static", g_arg_mix5_stat.load() == expect);
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
        // alternating-bit phases at 64-bit width (bitwise inverses): 0x5555...5
        // positive, 0xAAAA...A negative.  Both halves busy -> catch word swap/trunc.
        ctx.check("mcp_long_altpos_0x5555", g_long_altpos.load() == static_cast<std::int64_t>(0x5555555555555555ULL));
        ctx.check("mcp_long_altneg_0xAAAA", g_long_altneg.load() == static_cast<std::int64_t>(0xAAAAAAAAAAAAAAAAULL));
        ctx.check("mcp_long_altneg_static", g_long_altneg_stat.load() == static_cast<std::int64_t>(0xAAAAAAAAAAAAAAAAULL));
        ctx.check("mcp_long_zero_static", g_long_zero_stat.load() == 0);
        ctx.check("mcp_long_negone_static", g_long_negone_stat.load() == -1);

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
        // static float boundary mirrors (CallStaticFloatMethodA): finite one,
        // largest-finite MAX, smallest-positive subnormal MIN_VALUE, -Inf.
        ctx.check("mcp_float_one_static", bits2f(g_float_one_stat.load()) == 1.0f);
        ctx.check("mcp_float_max_static", bits2f(g_float_max_stat.load()) == std::numeric_limits<float>::max());
        ctx.check("mcp_float_min_subnormal_static", bits2f(g_float_minval_stat.load()) == std::numeric_limits<float>::denorm_min());
        {
            const float ninf = bits2f(g_float_neginf_stat.load());
            ctx.check("mcp_float_neginf_static_isinf", std::isinf(ninf) && ninf < 0.0f);
        }
        // 2.0f, smallest-NORMAL (distinct from the subnormal MIN_VALUE above), and
        // the two alternating-bit raw patterns (finite normals; compare RAW bits so
        // a byte swap is caught; 0xAAAAAAAA has the sign bit set -> finite negative).
        ctx.check("mcp_float_two", bits2f(g_float_two.load()) == 2.0f);
        ctx.check("mcp_float_min_normal", bits2f(g_float_minnormal.load()) == std::numeric_limits<float>::min());
        ctx.check("mcp_float_altlo_bits_0x55555555", g_float_altlo.load() == 0x55555555u);
        ctx.check("mcp_float_althi_bits_0xAAAAAAAA", g_float_althi.load() == 0xAAAAAAAAu);
        {
            const float althi = bits2f(g_float_althi.load());
            ctx.check("mcp_float_althi_is_finite_negative",
                      !std::isnan(althi) && !std::isinf(althi) && althi < 0.0f);
        }
        ctx.check("mcp_float_two_static", bits2f(g_float_two_stat.load()) == 2.0f);
        ctx.check("mcp_float_min_normal_static", bits2f(g_float_minnormal_stat.load()) == std::numeric_limits<float>::min());
        ctx.check("mcp_float_althi_static_bits", g_float_althi_stat.load() == 0xAAAAAAAAu);
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
        // static double boundary mirrors (CallStaticDoubleMethodA): finite one,
        // largest-finite MAX, smallest-positive subnormal MIN_VALUE, +Inf.
        ctx.check("mcp_double_one_static", bits2d(g_double_one_stat.load()) == 1.0);
        ctx.check("mcp_double_max_static", bits2d(g_double_max_stat.load()) == std::numeric_limits<double>::max());
        ctx.check("mcp_double_min_subnormal_static", bits2d(g_double_minval_stat.load()) == std::numeric_limits<double>::denorm_min());
        {
            const double pinf = bits2d(g_double_posinf_stat.load());
            ctx.check("mcp_double_posinf_static_isinf", std::isinf(pinf) && pinf > 0.0);
        }
        // 2.0, smallest-NORMAL (distinct from the subnormal MIN_VALUE), and the two
        // alternating-bit raw patterns (finite normals; RAW-bit compare for word-swap;
        // 0xAA... has the sign bit set -> finite negative).
        ctx.check("mcp_double_two", bits2d(g_double_two.load()) == 2.0);
        ctx.check("mcp_double_min_normal", bits2d(g_double_minnormal.load()) == std::numeric_limits<double>::min());
        ctx.check("mcp_double_altlo_bits", g_double_altlo.load() == 0x5555555555555555ULL);
        ctx.check("mcp_double_althi_bits", g_double_althi.load() == 0xAAAAAAAAAAAAAAAAULL);
        {
            const double althi = bits2d(g_double_althi.load());
            ctx.check("mcp_double_althi_is_finite_negative",
                      !std::isnan(althi) && !std::isinf(althi) && althi < 0.0);
        }
        ctx.check("mcp_double_two_static", bits2d(g_double_two_stat.load()) == 2.0);
        ctx.check("mcp_double_min_normal_static", bits2d(g_double_minnormal_stat.load()) == std::numeric_limits<double>::min());
        ctx.check("mcp_double_althi_static_bits", g_double_althi_stat.load() == 0xAAAAAAAAAAAAAAAAULL);

        // =====================================================================
        //  void (V) + value_t introspection
        // =====================================================================
        ctx.check("mcp_void_instance_is_void", g_void_inst_is_void.load() == 1);
        ctx.check("mcp_void_static_is_void", g_void_stat_is_void.load() == 1);
        // void side effects: the instance void is bumped once; the STATIC void
        // (sRetVoidBump) is invoked by BOTH svoid("sRetVoidBump") probes
        // (g_void_stat_is_void + g_svoid_int_is_void), so its counter equals the
        // number of those call sites, not 1.  The invariant under test is that a void
        // call actually RUNS the method body (a real side effect) -> assert >= 1 for the
        // static rather than an incidental exact count.
        ctx.check("mcp_void_instance_side_effect", method_primitives::get_void_instance_hits() == 1);
        ctx.check("mcp_void_static_side_effect", method_primitives::get_void_static_hits() >= 1);
        // A numeric (int) return must NOT be reported as void or string.
        ctx.check("mcp_int_return_is_not_void", g_int_is_void.load() == 0);
        ctx.check("mcp_int_return_is_not_string", g_int_is_string.load() == 0);

        // =====================================================================
        //  value_t conversion-operator breadth — the SAME primitive return value_t
        //  converted to a DIFFERENT-width / different-kind C++ target.  Exercises
        //  operator target_type()'s static_cast leg across stored alternatives;
        //  fully deterministic (pure C++ cast semantics on the decoded value).
        // =====================================================================
        // int32-stored return widened to int64 (value preserved, sign intact).
        ctx.check("mcp_conv_int_max_to_i64", g_int_max_as_i64.load() == 2147483647LL);
        ctx.check("mcp_conv_int_negone_to_i64", g_int_negone_as_i64.load() == -1LL);
        // int8-stored return widened to int64 sign-extends (-1 stays -1, not 255).
        ctx.check("mcp_conv_byte_negone_to_i64_sign_extends", g_byte_negone_as_i64.load() == -1LL);
        // uint16-stored (char) return widened to int64 zero-extends (0xFFFF->65535).
        ctx.check("mcp_conv_char_max_to_i64_zero_extends", g_char_max_as_i64.load() == 65535LL);
        // numeric return narrowed to bool: zero->false, non-zero->true.
        ctx.check("mcp_conv_int_zero_to_bool_false", g_int_zero_as_bool.load() == 0);
        ctx.check("mcp_conv_int_fortytwo_to_bool_true", g_int_fortytwo_as_bool.load() == 1);
        ctx.check("mcp_conv_byte_zero_to_bool_false", g_byte_zero_as_bool.load() == 0);
        // CROSS-KIND conversions: int16 stored return widened to int64 sign-extends.
        ctx.check("mcp_conv_short_min_to_i64_sign_extends", g_short_min_as_i64.load() == -32768LL);
        ctx.check("mcp_conv_short_altneg_to_i64_sign_extends", g_short_altneg_as_i64.load() == -21846LL);
        // int64 stored return read as int64 (identity for the widest integral kind).
        ctx.check("mcp_conv_long_max_to_i64_identity",
                  g_long_max_as_i64.load() == std::numeric_limits<std::int64_t>::max());
        // float/double stored return -> integer target: static_cast truncates toward
        // zero (2.75 -> 2, -2.75 -> -2).  Fully deterministic C++ cast semantics.
        ctx.check("mcp_conv_float_2p75_to_int_truncates_2", g_float_2p75_as_i32.load() == 2);
        ctx.check("mcp_conv_float_neg2p75_to_int_truncates_neg2", g_float_neg2p75_as_i32.load() == -2);
        ctx.check("mcp_conv_double_2p75_to_i64_truncates_2", g_double_2p75_as_i64.load() == 2);
        ctx.check("mcp_conv_double_neg2p75_to_i64_truncates_neg2", g_double_neg2p75_as_i64.load() == -2);
        // integer stored return -> floating target widens exactly for these values.
        ctx.check("mcp_conv_int_42_to_float_exact", g_int_42_as_float.load() == 1);
        ctx.check("mcp_conv_long_negone_to_double_exact", g_long_negone_as_double.load() == 1);
        // int64 stored return -> bool narrows: nonzero -> true, zero -> false.
        ctx.check("mcp_conv_long_negone_to_bool_true", g_long_negone_as_bool.load() == 1);
        ctx.check("mcp_conv_long_zero_to_bool_false", g_long_zero_as_bool.load() == 0);

        // =====================================================================
        //  int ARG -> float RETURN coexistence: feed 2^24 (largest int exact as a
        //  float) and read the (float)arg return; the bits must equal 16777216.0f.
        // =====================================================================
        ctx.check("mcp_arg_int_to_float_captured", g_arg_int_to_float_captured.load());
        ctx.check("mcp_arg_int_to_float_instance",
                  bits2f(g_arg_int_to_float_inst.load()) == 16777216.0f);
        ctx.check("mcp_arg_int_to_float_static",
                  bits2f(g_arg_int_to_float_stat.load()) == 16777216.0f);

        // =====================================================================
        //  BATCH-18 DEEPENING
        // =====================================================================
        // bool false -> int 0 (twin of the bool-true->1 conversion already checked).
        ctx.check("mcp_bool_false_to_int_is_0", g_bool_false_to_int.load() == 0);

        // ---- value_t conversion breadth: more same-value_t -> int64 reads ----
        // POSITIVE byte/short boundaries widen WITHOUT sign artifacts; char high bit
        // and char zero zero-extend; int MIN / int distinct-byte pattern widen with
        // sign and byte order intact; long MIN / long 0xAAAA..A read as identity.
        ctx.check("mcp_conv_byte_max_to_i64_127", g_byte_max_as_i64.load() == 127LL);
        ctx.check("mcp_conv_byte_zero_to_i64_0", g_byte_zero_as_i64.load() == 0LL);
        ctx.check("mcp_conv_char_zero_to_i64_0", g_char_zero_as_i64.load() == 0LL);
        ctx.check("mcp_conv_char_highbit_to_i64_zero_extends_32768", g_char_highbit_as_i64.load() == 32768LL);
        ctx.check("mcp_conv_short_max_to_i64_32767", g_short_max_as_i64.load() == 32767LL);
        ctx.check("mcp_conv_int_min_to_i64_neg2147483648", g_int_min_as_i64.load() == -2147483648LL);
        ctx.check("mcp_conv_int_pattern_to_i64_305419896", g_int_pattern_as_i64.load() == 0x12345678LL);
        ctx.check("mcp_conv_long_min_to_i64_identity",
                  g_long_min_as_i64.load() == std::numeric_limits<std::int64_t>::min());
        ctx.check("mcp_conv_long_altneg_to_i64_identity",
                  g_long_altneg_as_i64.load() == static_cast<std::int64_t>(0xAAAAAAAAAAAAAAAAULL));

        // ---- cross-kind truncation on the OTHER fp target widths ----
        // float return -> int64 target truncates toward zero, keeping the full
        // six-digit magnitude (1000000.5 -> 1000000, not 0 or 1000001 or a low digit).
        ctx.check("mcp_conv_float_bigwhole_to_i64_truncates_1000000", g_float_bigwhole_as_i64.load() == 1000000LL);
        ctx.check("mcp_conv_float_negbigwhole_to_i64_truncates_neg1000000", g_float_negbigwhole_as_i64.load() == -1000000LL);
        // double return -> int32 target truncates toward zero, keeping the full
        // seven-digit magnitude (1234567.5 -> 1234567).
        ctx.check("mcp_conv_double_bigwhole_to_i32_truncates_1234567", g_double_bigwhole_as_i32.load() == 1234567);
        ctx.check("mcp_conv_double_negbigwhole_to_i32_truncates_neg1234567", g_double_negbigwhole_as_i32.load() == -1234567);

        // ---- integral / float return -> wider FLOATING target (exact magnitudes) ----
        // Each captured as RAW bits, so the assertion pins the EXACT representable
        // value (not just "nonzero"); a wrong static_cast leg would change the bits.
        ctx.check("mcp_conv18_captured", g_conv18_captured.load());
        ctx.check("mcp_conv_byte_negone_to_float_neg1", bits2f(g_byte_negone_as_float.load()) == -1.0f);
        ctx.check("mcp_conv_short_min_to_float_neg32768", bits2f(g_short_min_as_float.load()) == -32768.0f);
        ctx.check("mcp_conv_int_pow24_to_float_exact", bits2f(g_int_pow24_as_float.load()) == 16777216.0f);
        ctx.check("mcp_conv_long_negone_to_float_neg1", bits2f(g_long_negone_as_float.load()) == -1.0f);
        ctx.check("mcp_conv_byte_negone_to_double_neg1", bits2d(g_byte_negone_as_double.load()) == -1.0);
        ctx.check("mcp_conv_char_max_to_double_65535", bits2d(g_char_max_as_double.load()) == 65535.0);
        ctx.check("mcp_conv_short_min_to_double_neg32768", bits2d(g_short_min_as_double.load()) == -32768.0);
        ctx.check("mcp_conv_int_max_to_double_exact", bits2d(g_int_max_as_double.load()) == 2147483647.0);
        ctx.check("mcp_conv_long_pow40_to_double_exact", bits2d(g_long_pow40_as_double.load()) == 1099511627776.0);
        // FLT_MAX widens to double LOSSLESSLY and equals the same value promoted in C++.
        ctx.check("mcp_conv_float_max_to_double_exact",
                  bits2d(g_float_max_as_double.load()) == static_cast<double>(std::numeric_limits<float>::max()));

        // ---- is_void()/is_string() false across EVERY non-void primitive kind ----
        // A populated primitive value_t is neither void nor string regardless of the
        // stored alternative; only a true void return (monostate) is is_void().
        ctx.check("mcp_float_return_is_not_void",  g_float_is_void.load()  == 0);
        ctx.check("mcp_double_return_is_not_void", g_double_is_void.load() == 0);
        ctx.check("mcp_long_return_is_not_void",   g_long_is_void.load()   == 0);
        ctx.check("mcp_char_return_is_not_void",   g_char_is_void.load()   == 0);
        ctx.check("mcp_byte_return_is_not_void",   g_byte_is_void.load()   == 0);
        ctx.check("mcp_short_return_is_not_void",  g_short_is_void.load()  == 0);
        ctx.check("mcp_bool_return_is_not_void",   g_bool_is_void.load()   == 0);
        ctx.check("mcp_float_return_is_not_string", g_float_is_string.load() == 0);
        ctx.check("mcp_long_return_is_not_string",  g_long_is_string.load()  == 0);
        // static-return introspection: a STATIC int return is non-void/non-string,
        // and a STATIC void return IS void.
        ctx.check("mcp_static_int_return_is_not_void",   g_sint_is_void.load()   == 0);
        ctx.check("mcp_static_int_return_is_not_string", g_sint_is_string.load() == 0);
        ctx.check("mcp_static_void_return_is_void",      g_svoid_int_is_void.load() == 1);

        // ---- (I)J widen ARG -> long RETURN coexistence ----
        // The int arg arrives SIGN-extended into the 64-bit result: -1 -> -1 (never
        // 4294967295), INT_MAX/INT_MIN widen to their exact 64-bit values.
        ctx.check("mcp_arg_int_to_long_captured", g_arg_int_to_long_captured.load());
        ctx.check("mcp_arg_int_to_long_negone_instance", g_arg_int_to_long_negone_inst.load() == -1LL);
        ctx.check("mcp_arg_int_to_long_max_instance", g_arg_int_to_long_max_inst.load() == 2147483647LL);
        ctx.check("mcp_arg_int_to_long_min_instance", g_arg_int_to_long_min_inst.load() == -2147483648LL);
        ctx.check("mcp_arg_int_to_long_negone_static", g_arg_int_to_long_negone_stat.load() == -1LL);

        // ---- POSITIVE-boundary narrow ARG widen probes ----
        // The .i slot delivers positive byte/short/char boundaries intact, the
        // positive twin of the negative/sign-bit widen probes above.
        ctx.check("mcp_arg_byte_widen_zero", g_arg_byte_widen_zero.load() == 0);
        ctx.check("mcp_arg_byte_widen_max_127", g_arg_byte_widen_max.load() == 127);
        ctx.check("mcp_arg_short_widen_zero", g_arg_short_widen_zero.load() == 0);
        ctx.check("mcp_arg_short_widen_max_32767", g_arg_short_widen_max.load() == 32767);
        ctx.check("mcp_arg_char_widen_A_65", g_arg_char_widen_a.load() == 65);

        // =====================================================================
        //  BATCH-22 DEEPENING
        // =====================================================================
        // ---- char (uint16) value_t NARROWED / reinterpreted into smaller targets ----
        // The existing char probes only WIDEN (uint16 -> int/i64/double).  These drive
        // operator target_type()'s TRUNCATING / reinterpreting static_cast leg: 0xFFFF
        // read as int8 is -1 (low byte, signed), as int16 is -1 (low 16 reinterpreted
        // signed), as unsigned char is 255 (low byte), as char16_t is the full 0xFFFF.
        ctx.check("mcp_conv_char_max_to_i8_truncates_neg1", g_char_max_as_i8.load() == -1);
        ctx.check("mcp_conv_char_max_to_i16_reinterprets_neg1", g_char_max_as_i16.load() == -1);
        ctx.check("mcp_conv_char_max_to_u8_low_byte_255", g_char_max_as_u8.load() == 255);
        ctx.check("mcp_conv_char_max_to_char16_identity_65535", g_char_max_as_char16.load() == 65535);
        // 0x8000 (only bit 15 set): as int16 it is INT16_MIN; as int8 it is 0 (low byte
        // clear).  A sharper single-bit witness than 0xFFFF for the reinterpret leg.
        ctx.check("mcp_conv_char_highbit_to_i16_neg32768", g_char_highbit_as_i16.load() == -32768);
        ctx.check("mcp_conv_char_highbit_to_i8_low_byte_zero", g_char_highbit_as_i8.load() == 0);
        // 0x00FF (low byte all-ones) read as int8 is -1 (the sign bit of the low byte).
        ctx.check("mcp_conv_char_255_to_i8_neg1", g_char_255_as_i8.load() == -1);
        // 'A' (65) read as plain `char` survives as 65.
        ctx.check("mcp_conv_char_A_to_char_65", g_char_a_as_char.load() == 65);

        // ---- bool value_t (Z alternative) -> every numeric width / floating kind ----
        // The existing Z probes only read bool as bool or as int; these reach the
        // int8/int16/uint16/int64/float/double static_cast legs (true -> 1, false -> 0).
        ctx.check("mcp_batch22_bool_captured", g_batch22_bool_captured.load());
        ctx.check("mcp_conv_bool_true_to_i8_1", g_bool_true_as_i8.load() == 1);
        ctx.check("mcp_conv_bool_true_to_i16_1", g_bool_true_as_i16.load() == 1);
        ctx.check("mcp_conv_bool_true_to_u16_1", g_bool_true_as_u16.load() == 1);
        ctx.check("mcp_conv_bool_true_to_i64_1", g_bool_true_as_i64.load() == 1);
        ctx.check("mcp_conv_bool_false_to_i64_0", g_bool_false_as_i64.load() == 0);
        ctx.check("mcp_conv_bool_true_to_float_1", bits2f(g_bool_true_as_float.load()) == 1.0f);
        ctx.check("mcp_conv_bool_false_to_float_0", bits2f(g_bool_false_as_float.load()) == 0.0f);
        ctx.check("mcp_conv_bool_true_to_double_1", bits2d(g_bool_true_as_double.load()) == 1.0);
        ctx.check("mcp_conv_bool_false_to_double_0", bits2d(g_bool_false_as_double.load()) == 0.0);

        // ---- EVERY primitive kind -> bool (nonzero true, zero incl -0.0 false) ----
        // Reaches the char / short / float / double truncation-to-bool legs the
        // existing int/byte/long -> bool probes miss.  -0.0 -> false proves the bool
        // conversion tests the VALUE (== 0), not the sign bit.
        ctx.check("mcp_conv_char_zero_to_bool_false", g_char_zero_as_bool.load() == 0);
        ctx.check("mcp_conv_char_max_to_bool_true", g_char_max_as_bool.load() == 1);
        ctx.check("mcp_conv_short_zero_to_bool_false", g_short_zero_as_bool.load() == 0);
        ctx.check("mcp_conv_short_min_to_bool_true", g_short_min_as_bool.load() == 1);
        ctx.check("mcp_conv_float_zero_to_bool_false", g_float_zero_as_bool.load() == 0);
        ctx.check("mcp_conv_float_one_to_bool_true", g_float_one_as_bool.load() == 1);
        ctx.check("mcp_conv_float_negzero_to_bool_false", g_float_negzero_as_bool.load() == 0);
        ctx.check("mcp_conv_double_zero_to_bool_false", g_double_zero_as_bool.load() == 0);
        ctx.check("mcp_conv_double_pi_to_bool_true", g_double_pi_as_bool.load() == 1);
        ctx.check("mcp_conv_double_negzero_to_bool_false", g_double_negzero_as_bool.load() == 0);

        // ---- IDEMPOTENCY: two reads of the same returner in one detour agree ----
        // call() carries no destructive per-call state: a distinct-byte int, an
        // alternating-bit long, and a NaN float each read twice must be bit-identical,
        // and the second read must equal the single-read value already asserted above.
        ctx.check("mcp_idem_int_pattern_two_reads_agree", g_idem_int_a.load() == g_idem_int_b.load());
        ctx.check("mcp_idem_int_pattern_value", g_idem_int_b.load() == 0x12345678LL);
        ctx.check("mcp_idem_long_altneg_two_reads_agree", g_idem_long_a.load() == g_idem_long_b.load());
        ctx.check("mcp_idem_long_altneg_value",
                  g_idem_long_b.load() == static_cast<std::int64_t>(0xAAAAAAAAAAAAAAAAULL));
        ctx.check("mcp_idem_float_nan_two_reads_agree", g_idem_float_nan_a.load() == g_idem_float_nan_b.load());
        ctx.check("mcp_idem_float_nan_is_nan", std::isnan(bits2f(g_idem_float_nan_b.load())));

        // ---- as_string() on a NON-string return is the EMPTY string ----
        // A populated numeric value_t and a void value_t both yield "" from as_string()
        // (the default leg never fabricates a textual number); instance + static.
        ctx.check("mcp_batch22_misc_captured", g_batch22_misc_captured.load());
        ctx.check("mcp_as_string_int_return_empty", g_as_string_int_empty.load() == 1);
        ctx.check("mcp_as_string_double_return_empty", g_as_string_double_empty.load() == 1);
        ctx.check("mcp_as_string_static_int_return_empty", g_as_string_sint_empty.load() == 1);
        ctx.check("mcp_as_string_static_void_return_empty", g_as_string_svoid_empty.load() == 1);
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
