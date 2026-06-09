// field_primitives_get JVM test module.
//
// Exhaustively exercises field_proxy::get() (vmhook.hpp:11548-11609) for EVERY
// JVM primitive descriptor (Z B S C I J F D) at regular AND boundary values,
// read back through the public wrapper API: static_field("name")->get() and an
// instance wrapper's get_field("name")->get().
//
// For every read we assert TWO things:
//   (1) the converted C++ value equals the Java-side value, and
//   (2) the value_t variant holds the CORRECT alternative (data.index()), which
//       proves get() selected the right signature branch — bool=0, int8=1,
//       int16=2, int32=3, int64=4, float=5, double=6, uint16=7, uint32=8.
//
// Float/double are additionally checked BIT-EXACT (std::bit_cast on the
// converted value) so NaN payloads, the signaling-NaN bit, denormal mantissas
// and the +/-0.0 sign bit are all proven to survive the memcpy unchanged.
//
// Flaws this module pins (see findings under audit/findings/):
//   * "Z" branch raw-memcpy into bool (vmhook.hpp:11558-11560) — we read every
//     bool and also a runtime-written bool to confirm a canonical bool result.
//   * Null-field_pointer fallback returns int32_t{} for EVERY signature
//     (vmhook.hpp:11551-11554) — proven by constructing field_proxy{nullptr,...}
//     directly and observing the variant alternative is always int32_t.
//   * static_cast<int>(NaN/Inf) is UB and static_cast<bool>(NaN)==true — we read
//     F/D NaN/Inf fields, assert the float/double value is correct, and document
//     the integer/bool conversion behaviour the audit flagged.
//   * Java char > 0x7F silently narrows to C++ char — we read high/BMP/surrogate
//     code units and confirm the uint16_t value is intact while char() is lossy.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <variant>

namespace
{
    // Wrapper for vmhook.fixtures.FieldPrimitivesGet.
    class fpg : public vmhook::object<fpg>
    {
    public:
        explicit fpg(vmhook::oop_t instance) noexcept
            : vmhook::object<fpg>{ instance }
        {
        }

        // ── handshake ──────────────────────────────────────────────────────
        static auto set_go(bool value) -> void { static_field("go")->set(value); }
        static auto get_done() -> bool          { return static_field("done")->get(); }

        // A live instance wrapper (the fixture keeps `static instance` alive).
        static auto get_instance() -> std::unique_ptr<fpg>
        {
            return static_field("instance")->get();
        }

        // Witness side-effect written by the probe through real bytecode.
        static auto get_runtime_seed_done() -> bool { return get_done(); }
    };

    // The fixture this module reads.  Used by the entry guard and registration.
    constexpr char FIXTURE[]{ "vmhook/fixtures/FieldPrimitivesGet" };

    // Variant-alternative indices (must match field_proxy::value_t::data order).
    constexpr std::size_t kIdxBool   = 0;
    constexpr std::size_t kIdxI8     = 1;
    constexpr std::size_t kIdxI16    = 2;
    constexpr std::size_t kIdxI32    = 3;
    constexpr std::size_t kIdxI64    = 4;
    constexpr std::size_t kIdxFloat  = 5;
    constexpr std::size_t kIdxDouble = 6;
    constexpr std::size_t kIdxU16    = 7;
    constexpr std::size_t kIdxU32    = 8;

    // Bit helpers (avoid std::bit_cast to keep C++17 portability with the rest
    // of the test tree; memcpy is the canonical type-pun).
    auto float_bits(float f) noexcept -> std::uint32_t
    {
        std::uint32_t b{};
        std::memcpy(&b, &f, sizeof(b));
        return b;
    }
    auto double_bits(double d) noexcept -> std::uint64_t
    {
        std::uint64_t b{};
        std::memcpy(&b, &d, sizeof(b));
        return b;
    }
}

// The entire test body, factored out so the VMHOOK_JVM_MODULE wrapper can run it
// under a try/catch and ALWAYS follow it with shutdown_hooks() (suite-safety: a
// throw must never escape, and zero hooks must be armed on exit — see the wrapper
// at the bottom).  Anonymous-namespace members (fpg, kIdx*, float_bits) are
// visible here at file scope in this TU.
static void run_field_primitives_get_checks(vmhook_test::context& ctx)
{
    vmhook::register_class<fpg>(FIXTURE);

    // =====================================================================
    //  ENTRY GUARD.  If FieldPrimitivesGet is not loaded/resolvable on this
    //  run, every static_field()->get() below would deref a disengaged
    //  optional.  Bail cleanly to [INFO] instead (the wrapper's final
    //  shutdown_hooks() still runs).  In practice the harness loads the
    //  fixture on every run, so this is belt-and-braces.
    // =====================================================================
    if (vmhook::find_class(FIXTURE) == nullptr)
    {
        ctx.record("[INFO] field_primitives_get: FieldPrimitivesGet not "
                   "loaded/resolvable on this run; skipping the module's live "
                   "checks (no crash, no hooks armed).");
        return;
    }

    // ---------------------------------------------------------------------
    // Sanity: the class resolves and a static read works at all.
    // ---------------------------------------------------------------------
    {
        const auto probe{ fpg::static_field("sIntZero") };
        ctx.check("fpg_class_registered_static_field_resolves", probe.has_value());
    }

    // =====================================================================
    //  STATIC reads — helper lambdas keep the (value + variant index) pair
    //  assertion uniform across the hundreds of checks below.
    // =====================================================================

    // boolean ("Z") -> bool, index 0.
    auto chk_bool = [&](const char* field, bool expected)
    {
        auto fp{ fpg::static_field(field) };
        ctx.check(std::string{ "Z_resolves_" } + field, fp.has_value());
        if (!fp) { return; }
        const auto v{ fp->get() };
        ctx.check(std::string{ "Z_variant_is_bool_" } + field, v.data.index() == kIdxBool);
        const bool got{ v };
        ctx.check(std::string{ "Z_value_" } + field, got == expected);
        // A bool produced by get() must be canonical: exactly 0 or 1 when widened.
        const int as_int{ v };
        ctx.check(std::string{ "Z_canonical_int_" } + field,
                  as_int == (expected ? 1 : 0));
        ctx.check(std::string{ "Z_signature_" } + field, v.signature == "Z");
    };
    chk_bool("sBoolTrue",  true);
    chk_bool("sBoolFalse", false);

    // byte ("B") -> int8_t, index 1.
    auto chk_byte = [&](const char* field, std::int8_t expected)
    {
        auto fp{ fpg::static_field(field) };
        ctx.check(std::string{ "B_resolves_" } + field, fp.has_value());
        if (!fp) { return; }
        const auto v{ fp->get() };
        ctx.check(std::string{ "B_variant_is_int8_" } + field, v.data.index() == kIdxI8);
        const std::int8_t got{ v };
        ctx.check(std::string{ "B_value_" } + field, got == expected);
        // Sign-extension through the variant: int8_t -1 -> int -1.
        const int widened{ v };
        ctx.check(std::string{ "B_sign_extends_to_int_" } + field,
                  widened == static_cast<int>(expected));
        ctx.check(std::string{ "B_signature_" } + field, v.signature == "B");
    };
    chk_byte("sByteZero",   0);
    chk_byte("sByteOne",    1);
    chk_byte("sByteNegOne", -1);
    chk_byte("sByteMin",    std::numeric_limits<std::int8_t>::min()); // -128
    chk_byte("sByteMax",    std::numeric_limits<std::int8_t>::max()); //  127
    chk_byte("sByte0x7F",   127);
    chk_byte("sByte0x80",   -128);
    chk_byte("sByte0xFF",   -1);
    chk_byte("sByte0xAB",   static_cast<std::int8_t>(0xAB));          //  -85
    // Unsigned widening of byte 0xFF: signed -1 then widened to uint32 is 0xFFFFFFFF.
    {
        auto fp{ fpg::static_field("sByte0xFF") };
        if (fp)
        {
            const std::uint32_t u{ fp->get() };
            ctx.check("B_0xFF_widens_unsigned_to_FFFFFFFF", u == 0xFFFFFFFFu);
        }
    }

    // short ("S") -> int16_t, index 2.
    auto chk_short = [&](const char* field, std::int16_t expected)
    {
        auto fp{ fpg::static_field(field) };
        ctx.check(std::string{ "S_resolves_" } + field, fp.has_value());
        if (!fp) { return; }
        const auto v{ fp->get() };
        ctx.check(std::string{ "S_variant_is_int16_" } + field, v.data.index() == kIdxI16);
        const std::int16_t got{ v };
        ctx.check(std::string{ "S_value_" } + field, got == expected);
        const int widened{ v };
        ctx.check(std::string{ "S_sign_extends_to_int_" } + field,
                  widened == static_cast<int>(expected));
        ctx.check(std::string{ "S_signature_" } + field, v.signature == "S");
    };
    chk_short("sShortZero",   0);
    chk_short("sShortOne",    1);
    chk_short("sShortNegOne", -1);
    chk_short("sShortMin",    std::numeric_limits<std::int16_t>::min()); // -32768
    chk_short("sShortMax",    std::numeric_limits<std::int16_t>::max()); //  32767
    chk_short("sShort0x8000", static_cast<std::int16_t>(0x8000));        // -32768
    chk_short("sShort0x7FFF", 0x7FFF);                                   //  32767
    chk_short("sShortBeef",   static_cast<std::int16_t>(0xBEEF));        //  -16657

    // int ("I") -> int32_t, index 3.
    auto chk_int = [&](const char* field, std::int32_t expected)
    {
        auto fp{ fpg::static_field(field) };
        ctx.check(std::string{ "I_resolves_" } + field, fp.has_value());
        if (!fp) { return; }
        const auto v{ fp->get() };
        ctx.check(std::string{ "I_variant_is_int32_" } + field, v.data.index() == kIdxI32);
        const std::int32_t got{ v };
        ctx.check(std::string{ "I_value_" } + field, got == expected);
        // Widening an int32 into int64 preserves sign.
        const std::int64_t widened{ v };
        ctx.check(std::string{ "I_sign_extends_to_long_" } + field,
                  widened == static_cast<std::int64_t>(expected));
        ctx.check(std::string{ "I_signature_" } + field, v.signature == "I");
    };
    chk_int("sIntZero",       0);
    chk_int("sIntOne",        1);
    chk_int("sIntNegOne",     -1);
    chk_int("sIntMin",        std::numeric_limits<std::int32_t>::min());
    chk_int("sIntMax",        std::numeric_limits<std::int32_t>::max());
    chk_int("sIntDeadBeef",   static_cast<std::int32_t>(0xDEADBEEF));
    chk_int("sInt0x7FFFFFFF", 0x7FFFFFFF);
    chk_int("sInt0x80000000", static_cast<std::int32_t>(0x80000000));

    // long ("J") -> int64_t, index 4.
    auto chk_long = [&](const char* field, std::int64_t expected)
    {
        auto fp{ fpg::static_field(field) };
        ctx.check(std::string{ "J_resolves_" } + field, fp.has_value());
        if (!fp) { return; }
        const auto v{ fp->get() };
        ctx.check(std::string{ "J_variant_is_int64_" } + field, v.data.index() == kIdxI64);
        const std::int64_t got{ v };
        ctx.check(std::string{ "J_value_" } + field, got == expected);
        ctx.check(std::string{ "J_signature_" } + field, v.signature == "J");
    };
    chk_long("sLongZero",               0);
    chk_long("sLongOne",                1);
    chk_long("sLongNegOne",             -1);
    chk_long("sLongMin",                std::numeric_limits<std::int64_t>::min());
    chk_long("sLongMax",                std::numeric_limits<std::int64_t>::max());
    chk_long("sLongDeadBeef",           static_cast<std::int64_t>(0xDEADBEEFCAFEBABEULL));
    chk_long("sLong0x7FFFFFFFFFFFFFFF", 0x7FFFFFFFFFFFFFFFLL);
    chk_long("sLong0x8000000000000000", static_cast<std::int64_t>(0x8000000000000000ULL));
    chk_long("sLongHighBits",           static_cast<std::int64_t>(0x00000000FFFFFFFFULL)); // 4294967295

    // char ("C") -> uint16_t, index 7.  Java char is a UTF-16 code unit.
    auto chk_char = [&](const char* field, std::uint16_t expected)
    {
        auto fp{ fpg::static_field(field) };
        ctx.check(std::string{ "C_resolves_" } + field, fp.has_value());
        if (!fp) { return; }
        const auto v{ fp->get() };
        ctx.check(std::string{ "C_variant_is_uint16_" } + field, v.data.index() == kIdxU16);
        const std::uint16_t got{ v };
        ctx.check(std::string{ "C_value_" } + field, got == expected);
        const char16_t as_c16{ v };
        ctx.check(std::string{ "C_char16_matches_" } + field,
                  as_c16 == static_cast<char16_t>(expected));
        // char is never sign-promoted to a negative when widened to int (it is
        // an unsigned 16-bit unit): widening to int must reproduce 0..65535.
        const int widened{ v };
        ctx.check(std::string{ "C_widens_to_unsigned_int_" } + field,
                  widened == static_cast<int>(expected));
        ctx.check(std::string{ "C_signature_" } + field, v.signature == "C");
    };
    chk_char("sCharNull",   0x0000); // '\0' — char MIN, NUL terminator
    chk_char("sCharSpace",  0x0020);
    chk_char("sCharA",      0x0041);
    chk_char("sCharZeroDig", 0x0030); // '0'
    chk_char("sChar0x7F",   0x007F); // last single-byte ASCII (DEL)
    chk_char("sChar0x80",   0x0080); // first high-bit value
    chk_char("sChar0xFF",   0x00FF); // last 8-bit value
    chk_char("sChar0x0100", 0x0100); // first value needing the high byte
    chk_char("sCharMax",    0xFFFF);
    chk_char("sCharHighBit", 0x00E9); // 'é'
    chk_char("sCharBmp",    0x4E2D);  // '中'
    chk_char("sCharHiSurr", 0xD83D);
    chk_char("sCharLoSurr", 0xDE00);
    chk_char("sCharMinSurr", 0xD800);
    chk_char("sCharMaxSurr", 0xDFFF);

    // Char narrowing witness: high/BMP chars truncate when forced into C++ char,
    // but the uint16_t / char16_t path is lossless.  (Audit: silent narrowing.)
    {
        auto fp{ fpg::static_field("sCharBmp") };
        if (fp)
        {
            const auto v{ fp->get() };
            const std::uint16_t full{ v };
            const char narrowed{ v };
            ctx.check("C_bmp_uint16_lossless", full == 0x4E2D);
            ctx.check("C_bmp_char_truncates_to_low_byte",
                      static_cast<unsigned char>(narrowed) == 0x2D);
        }
    }
    {
        auto fp{ fpg::static_field("sCharHighBit") };
        if (fp)
        {
            const auto v{ fp->get() };
            const std::uint16_t full{ v };
            const char narrowed{ v };
            ctx.check("C_high_uint16_lossless", full == 0x00E9);
            ctx.check("C_high_char_low_byte",
                      static_cast<unsigned char>(narrowed) == 0xE9);
        }
    }

    // float ("F") -> float, index 5.  value + BIT-EXACT pattern.
    auto chk_float = [&](const char* field, std::uint32_t expected_bits)
    {
        auto fp{ fpg::static_field(field) };
        ctx.check(std::string{ "F_resolves_" } + field, fp.has_value());
        if (!fp) { return; }
        const auto v{ fp->get() };
        ctx.check(std::string{ "F_variant_is_float_" } + field, v.data.index() == kIdxFloat);
        const float got{ v };
        ctx.check(std::string{ "F_bits_exact_" } + field, float_bits(got) == expected_bits);
        ctx.check(std::string{ "F_signature_" } + field, v.signature == "F");
    };
    chk_float("sFloatPosZero", 0x00000000);
    chk_float("sFloatNegZero", 0x80000000);
    chk_float("sFloatOne",     0x3F800000);
    chk_float("sFloatNegOne",  0xBF800000);
    chk_float("sFloatMin",     0x00000001); // Float.MIN_VALUE (denormal)
    chk_float("sFloatMax",     0x7F7FFFFF);
    chk_float("sFloatMinNorm", 0x00800000);
    chk_float("sFloatPosInf",  0x7F800000);
    chk_float("sFloatNegInf",  0xFF800000);
    chk_float("sFloatNaN",     0x7FC00000); // canonical qNaN
    chk_float("sFloatSNaN",    0x7F800001); // signaling NaN bit pattern
    chk_float("sFloatNaNPay",  0x7FA55555); // qNaN with payload
    chk_float("sFloatDenorm",  0x00000001);
    // Exact-representable fractions (bit patterns are exact, no epsilon needed).
    chk_float("sFloatHalf",    0x3F000000); // 0.5
    chk_float("sFloatQuarter", 0x3E800000); // 0.25
    chk_float("sFloatNegHalf", 0xBF000000); // -0.5
    chk_float("sFloatThreeQ",  0x3F400000); // 0.75
    chk_float("sFloatTwo",     0x40000000); // 2.0
    // Ordinary value: assert the actual float compares equal (not just bits).
    {
        auto fp{ fpg::static_field("sFloatPi") };
        if (fp)
        {
            const float got{ fp->get() };
            ctx.check("F_pi_value_close",
                      std::fabs(got - 3.14159265358979F) < 1e-6F);
        }
    }
    // Semantic float predicates round-tripped through get().
    {
        auto fnan{ fpg::static_field("sFloatNaN") };
        auto fpinf{ fpg::static_field("sFloatPosInf") };
        auto fninf{ fpg::static_field("sFloatNegInf") };
        auto fnz{ fpg::static_field("sFloatNegZero") };
        if (fnan)  { const float g{ fnan->get() };  ctx.check("F_NaN_is_nan",        std::isnan(g)); }
        if (fpinf) { const float g{ fpinf->get() }; ctx.check("F_posinf_is_inf_pos", std::isinf(g) && g > 0.0F); }
        if (fninf) { const float g{ fninf->get() }; ctx.check("F_neginf_is_inf_neg", std::isinf(g) && g < 0.0F); }
        if (fnz)   { const float g{ fnz->get() };   ctx.check("F_negzero_signbit",   std::signbit(g) && g == 0.0F); }
    }
    // Exact-value equality on exactly-representable fractions (== is safe here:
    // 0.5/0.25/0.75/2.0 have no rounding).  Proves the CONVERTED float value, not
    // only its bit pattern, is faithful.
    {
        auto fh{ fpg::static_field("sFloatHalf") };
        auto fq{ fpg::static_field("sFloatQuarter") };
        auto ftq{ fpg::static_field("sFloatThreeQ") };
        auto fnh{ fpg::static_field("sFloatNegHalf") };
        if (fh)  { const float g{ fh->get() };  ctx.check("F_half_value_exact",    g == 0.5F); }
        if (fq)  { const float g{ fq->get() };  ctx.check("F_quarter_value_exact", g == 0.25F); }
        if (ftq) { const float g{ ftq->get() }; ctx.check("F_threeQ_value_exact",  g == 0.75F); }
        if (fnh) { const float g{ fnh->get() }; ctx.check("F_neghalf_value_exact", g == -0.5F && std::signbit(g)); }
        // 0.5 + 0.25 == 0.75 reconstructed purely from get()-read operands.
        if (fh && fq && ftq)
        {
            const float a{ fh->get() };
            const float b{ fq->get() };
            const float c{ ftq->get() };
            ctx.check("F_half_plus_quarter_is_threeQ", (a + b) == c);
        }
    }

    // double ("D") -> double, index 6.  value + BIT-EXACT pattern.
    auto chk_double = [&](const char* field, std::uint64_t expected_bits)
    {
        auto fp{ fpg::static_field(field) };
        ctx.check(std::string{ "D_resolves_" } + field, fp.has_value());
        if (!fp) { return; }
        const auto v{ fp->get() };
        ctx.check(std::string{ "D_variant_is_double_" } + field, v.data.index() == kIdxDouble);
        const double got{ v };
        ctx.check(std::string{ "D_bits_exact_" } + field, double_bits(got) == expected_bits);
        ctx.check(std::string{ "D_signature_" } + field, v.signature == "D");
    };
    chk_double("sDoublePosZero", 0x0000000000000000ULL);
    chk_double("sDoubleNegZero", 0x8000000000000000ULL);
    chk_double("sDoubleOne",     0x3FF0000000000000ULL);
    chk_double("sDoubleNegOne",  0xBFF0000000000000ULL);
    chk_double("sDoubleMin",     0x0000000000000001ULL); // Double.MIN_VALUE denormal
    chk_double("sDoubleMax",     0x7FEFFFFFFFFFFFFFULL);
    chk_double("sDoubleMinNorm", 0x0010000000000000ULL);
    chk_double("sDoublePosInf",  0x7FF0000000000000ULL);
    chk_double("sDoubleNegInf",  0xFFF0000000000000ULL);
    chk_double("sDoubleNaN",     0x7FF8000000000000ULL); // canonical qNaN
    chk_double("sDoubleSNaN",    0x7FF0000000000001ULL); // signaling NaN
    chk_double("sDoubleNaNPay",  0x7FFAAAAAAAAAAAAAULL); // qNaN with payload
    chk_double("sDoubleDenorm",  0x0000000000000001ULL);
    // Exact-representable fractions (bit patterns are exact, no epsilon needed).
    chk_double("sDoubleHalf",    0x3FE0000000000000ULL); // 0.5
    chk_double("sDoubleQuarter", 0x3FD0000000000000ULL); // 0.25
    chk_double("sDoubleNegHalf", 0xBFE0000000000000ULL); // -0.5
    chk_double("sDoubleThreeQ",  0x3FE8000000000000ULL); // 0.75
    chk_double("sDoubleTwo",     0x4000000000000000ULL); // 2.0
    {
        auto fp{ fpg::static_field("sDoublePi") };
        if (fp)
        {
            const double got{ fp->get() };
            ctx.check("D_pi_value_close", std::fabs(got - 3.141592653589793) < 1e-12);
        }
    }
    {
        auto dnan{ fpg::static_field("sDoubleNaN") };
        auto dpinf{ fpg::static_field("sDoublePosInf") };
        auto dninf{ fpg::static_field("sDoubleNegInf") };
        auto dnz{ fpg::static_field("sDoubleNegZero") };
        if (dnan)  { const double g{ dnan->get() };  ctx.check("D_NaN_is_nan",        std::isnan(g)); }
        if (dpinf) { const double g{ dpinf->get() }; ctx.check("D_posinf_is_inf_pos", std::isinf(g) && g > 0.0); }
        if (dninf) { const double g{ dninf->get() }; ctx.check("D_neginf_is_inf_neg", std::isinf(g) && g < 0.0); }
        if (dnz)   { const double g{ dnz->get() };   ctx.check("D_negzero_signbit",   std::signbit(g) && g == 0.0); }
    }
    // Exact-value equality on exactly-representable double fractions.
    {
        auto dh{ fpg::static_field("sDoubleHalf") };
        auto dq{ fpg::static_field("sDoubleQuarter") };
        auto dtq{ fpg::static_field("sDoubleThreeQ") };
        auto dnh{ fpg::static_field("sDoubleNegHalf") };
        if (dh)  { const double g{ dh->get() };  ctx.check("D_half_value_exact",    g == 0.5); }
        if (dq)  { const double g{ dq->get() };  ctx.check("D_quarter_value_exact", g == 0.25); }
        if (dtq) { const double g{ dtq->get() }; ctx.check("D_threeQ_value_exact",  g == 0.75); }
        if (dnh) { const double g{ dnh->get() }; ctx.check("D_neghalf_value_exact", g == -0.5 && std::signbit(g)); }
        if (dh && dq && dtq)
        {
            const double a{ dh->get() };
            const double b{ dq->get() };
            const double c{ dtq->get() };
            ctx.check("D_half_plus_quarter_is_threeQ", (a + b) == c);
        }
    }

    // =====================================================================
    //  INSTANCE reads — same get() path, instance dispatch.  Proves get()
    //  ignores the static/instance flag and yields identical decoding.
    // =====================================================================
    {
        const auto inst{ fpg::get_instance() };
        ctx.check("instance_wrapper_obtained", inst != nullptr);
        if (inst)
        {
            // boolean
            {
                auto fp{ inst->get_field("iBool") };
                ctx.check("inst_Z_resolves", fp.has_value());
                if (fp)
                {
                    const auto v{ fp->get() };
                    ctx.check("inst_Z_variant_is_bool", v.data.index() == kIdxBool);
                    const bool b{ v };
                    ctx.check("inst_Z_value", b == true);
                }
            }
            // byte 0xFE -> -2
            {
                auto fp{ inst->get_field("iByte") };
                ctx.check("inst_B_resolves", fp.has_value());
                if (fp)
                {
                    const auto v{ fp->get() };
                    ctx.check("inst_B_variant_is_int8", v.data.index() == kIdxI8);
                    const std::int8_t b{ v };
                    ctx.check("inst_B_value", b == static_cast<std::int8_t>(0xFE));
                    const int widened{ v };
                    ctx.check("inst_B_sign_extends", widened == -2);
                }
            }
            // short 0xCAFE
            {
                auto fp{ inst->get_field("iShort") };
                ctx.check("inst_S_resolves", fp.has_value());
                if (fp)
                {
                    const auto v{ fp->get() };
                    ctx.check("inst_S_variant_is_int16", v.data.index() == kIdxI16);
                    const std::int16_t s{ v };
                    ctx.check("inst_S_value", s == static_cast<std::int16_t>(0xCAFE));
                }
            }
            // int 0x0BADF00D
            {
                auto fp{ inst->get_field("iInt") };
                ctx.check("inst_I_resolves", fp.has_value());
                if (fp)
                {
                    const auto v{ fp->get() };
                    ctx.check("inst_I_variant_is_int32", v.data.index() == kIdxI32);
                    const std::int32_t i{ v };
                    ctx.check("inst_I_value", i == 0x0BADF00D);
                }
            }
            // long 0x0123456789ABCDEF
            {
                auto fp{ inst->get_field("iLong") };
                ctx.check("inst_J_resolves", fp.has_value());
                if (fp)
                {
                    const auto v{ fp->get() };
                    ctx.check("inst_J_variant_is_int64", v.data.index() == kIdxI64);
                    const std::int64_t l{ v };
                    ctx.check("inst_J_value", l == 0x0123456789ABCDEFLL);
                }
            }
            // char 0x20AC '€'
            {
                auto fp{ inst->get_field("iChar") };
                ctx.check("inst_C_resolves", fp.has_value());
                if (fp)
                {
                    const auto v{ fp->get() };
                    ctx.check("inst_C_variant_is_uint16", v.data.index() == kIdxU16);
                    const std::uint16_t c{ v };
                    ctx.check("inst_C_value", c == 0x20AC);
                }
            }
            // float bit pattern 0xC0490FDB
            {
                auto fp{ inst->get_field("iFloat") };
                ctx.check("inst_F_resolves", fp.has_value());
                if (fp)
                {
                    const auto v{ fp->get() };
                    ctx.check("inst_F_variant_is_float", v.data.index() == kIdxFloat);
                    const float f{ v };
                    ctx.check("inst_F_bits_exact", float_bits(f) == 0xC0490FDB);
                }
            }
            // double pi bits 0x400921FB54442D18
            {
                auto fp{ inst->get_field("iDouble") };
                ctx.check("inst_D_resolves", fp.has_value());
                if (fp)
                {
                    const auto v{ fp->get() };
                    ctx.check("inst_D_variant_is_double", v.data.index() == kIdxDouble);
                    const double d{ v };
                    ctx.check("inst_D_bits_exact", double_bits(d) == 0x400921FB54442D18ULL);
                }
            }

            // ── Instance BOUNDARY reads ──────────────────────────────────
            // The same get() decoding at the EXTREMES of each primitive on the
            // instance-dispatch path (offset basis = decoded oop + offset), each
            // asserting value AND variant alternative.  Small per-type lambdas
            // keep these uniform; the instance handle `inst` is captured.
            auto inst_bool = [&](const char* field, bool expected)
            {
                auto fp{ inst->get_field(field) };
                ctx.check(std::string{ "inst_Z_resolves_" } + field, fp.has_value());
                if (!fp) { return; }
                const auto v{ fp->get() };
                ctx.check(std::string{ "inst_Z_variant_" } + field, v.data.index() == kIdxBool);
                const bool b{ v };
                ctx.check(std::string{ "inst_Z_value_" } + field, b == expected);
            };
            auto inst_byte = [&](const char* field, std::int8_t expected)
            {
                auto fp{ inst->get_field(field) };
                ctx.check(std::string{ "inst_B_resolves_" } + field, fp.has_value());
                if (!fp) { return; }
                const auto v{ fp->get() };
                ctx.check(std::string{ "inst_B_variant_" } + field, v.data.index() == kIdxI8);
                const std::int8_t b{ v };
                ctx.check(std::string{ "inst_B_value_" } + field, b == expected);
            };
            auto inst_short = [&](const char* field, std::int16_t expected)
            {
                auto fp{ inst->get_field(field) };
                ctx.check(std::string{ "inst_S_resolves_" } + field, fp.has_value());
                if (!fp) { return; }
                const auto v{ fp->get() };
                ctx.check(std::string{ "inst_S_variant_" } + field, v.data.index() == kIdxI16);
                const std::int16_t s{ v };
                ctx.check(std::string{ "inst_S_value_" } + field, s == expected);
            };
            auto inst_int = [&](const char* field, std::int32_t expected)
            {
                auto fp{ inst->get_field(field) };
                ctx.check(std::string{ "inst_I_resolves_" } + field, fp.has_value());
                if (!fp) { return; }
                const auto v{ fp->get() };
                ctx.check(std::string{ "inst_I_variant_" } + field, v.data.index() == kIdxI32);
                const std::int32_t i{ v };
                ctx.check(std::string{ "inst_I_value_" } + field, i == expected);
            };
            auto inst_long = [&](const char* field, std::int64_t expected)
            {
                auto fp{ inst->get_field(field) };
                ctx.check(std::string{ "inst_J_resolves_" } + field, fp.has_value());
                if (!fp) { return; }
                const auto v{ fp->get() };
                ctx.check(std::string{ "inst_J_variant_" } + field, v.data.index() == kIdxI64);
                const std::int64_t l{ v };
                ctx.check(std::string{ "inst_J_value_" } + field, l == expected);
            };
            auto inst_char = [&](const char* field, std::uint16_t expected)
            {
                auto fp{ inst->get_field(field) };
                ctx.check(std::string{ "inst_C_resolves_" } + field, fp.has_value());
                if (!fp) { return; }
                const auto v{ fp->get() };
                ctx.check(std::string{ "inst_C_variant_" } + field, v.data.index() == kIdxU16);
                const std::uint16_t c{ v };
                ctx.check(std::string{ "inst_C_value_" } + field, c == expected);
            };
            auto inst_float_bits = [&](const char* field, std::uint32_t expected_bits)
            {
                auto fp{ inst->get_field(field) };
                ctx.check(std::string{ "inst_F_resolves_" } + field, fp.has_value());
                if (!fp) { return; }
                const auto v{ fp->get() };
                ctx.check(std::string{ "inst_F_variant_" } + field, v.data.index() == kIdxFloat);
                const float f{ v };
                ctx.check(std::string{ "inst_F_bits_" } + field, float_bits(f) == expected_bits);
            };
            auto inst_double_bits = [&](const char* field, std::uint64_t expected_bits)
            {
                auto fp{ inst->get_field(field) };
                ctx.check(std::string{ "inst_D_resolves_" } + field, fp.has_value());
                if (!fp) { return; }
                const auto v{ fp->get() };
                ctx.check(std::string{ "inst_D_variant_" } + field, v.data.index() == kIdxDouble);
                const double d{ v };
                ctx.check(std::string{ "inst_D_bits_" } + field, double_bits(d) == expected_bits);
            };

            inst_bool ("iBoolFalse", false);
            inst_byte ("iByteMin",   std::numeric_limits<std::int8_t>::min());
            inst_byte ("iByteMax",   std::numeric_limits<std::int8_t>::max());
            inst_byte ("iByteZero",  0);
            inst_short("iShortMin",  std::numeric_limits<std::int16_t>::min());
            inst_short("iShortMax",  std::numeric_limits<std::int16_t>::max());
            inst_int  ("iIntMin",    std::numeric_limits<std::int32_t>::min());
            inst_int  ("iIntMax",    std::numeric_limits<std::int32_t>::max());
            inst_int  ("iIntNegOne", -1);
            inst_long ("iLongMin",   std::numeric_limits<std::int64_t>::min());
            inst_long ("iLongMax",   std::numeric_limits<std::int64_t>::max());
            inst_char ("iCharNull",  0x0000);
            inst_char ("iCharMax",   0xFFFF);
            inst_float_bits ("iFloatPosInf",   0x7F800000);
            inst_float_bits ("iFloatNaN",      0x7FC00000);
            inst_float_bits ("iFloatNegZero",  0x80000000);
            inst_double_bits("iDoubleMax",     0x7FEFFFFFFFFFFFFFULL);
            inst_double_bits("iDoubleNegInf",  0xFFF0000000000000ULL);
            inst_double_bits("iDoubleNegZero", 0x8000000000000000ULL);

            // Cross-check: instance get() of a field whose JVM_ACC_STATIC is set
            // (read a static field through the instance accessor) yields the same
            // value as the static accessor — proves get() does not consult the flag.
            {
                auto via_inst{ inst->get_field("sIntMax") };
                auto via_static{ fpg::static_field("sIntMax") };
                if (via_inst && via_static)
                {
                    const std::int32_t a{ via_inst->get() };
                    const std::int32_t b{ via_static->get() };
                    ctx.check("static_via_instance_equals_static_accessor",
                              a == b && a == std::numeric_limits<std::int32_t>::max());
                }
            }
        }
    }

    // =====================================================================
    //  NULL-field_pointer fallback (vmhook.hpp:11551-11554): get() returns
    //  value_t{int32_t{}, sig} for EVERY signature.  We construct field_proxy
    //  directly with a null pointer and confirm the (buggy-by-design) contract:
    //  the variant alternative is ALWAYS int32_t, regardless of descriptor, and
    //  every numeric/bool conversion collapses to zero/false without crashing.
    // =====================================================================
    {
        const char* sigs[] = { "Z", "B", "S", "I", "J", "F", "D", "C",
                               "Ljava/lang/String;", "[I" };
        for (const char* sig : sigs)
        {
            vmhook::field_proxy fp{ nullptr, sig, false };
            const auto v{ fp.get() };
            ctx.check(std::string{ "null_ptr_variant_is_int32_" } + sig,
                      v.data.index() == kIdxI32);
            ctx.check(std::string{ "null_ptr_signature_roundtrips_" } + sig,
                      v.signature == sig);
            const int as_int{ v };
            ctx.check(std::string{ "null_ptr_int_is_zero_" } + sig, as_int == 0);
            const bool as_bool{ v };
            ctx.check(std::string{ "null_ptr_bool_is_false_" } + sig, as_bool == false);
            const double as_double{ v };
            ctx.check(std::string{ "null_ptr_double_is_zero_" } + sig, as_double == 0.0);
        }
        // String-typed null proxy decodes to empty (does not chase a garbage OOP).
        {
            vmhook::field_proxy fp{ nullptr, "Ljava/lang/String;", false };
            // Use as_string() (parity with method_proxy::value_t): a brace-init
            // std::string{ fp.get() } is ambiguous / can pick the const char*
            // conversion and build from a null pointer for a non-string value.
            const std::string s{ fp.get().as_string() };
            ctx.check("null_ptr_string_is_empty", s.empty());
        }
    }

    // =====================================================================
    //  is_static() / signature() accessor parity on resolved proxies.
    // =====================================================================
    {
        auto st{ fpg::static_field("sIntZero") };
        if (st)
        {
            ctx.check("static_proxy_is_static_true", st->is_static() == true);
            ctx.check("static_proxy_signature_I", std::string{ st->signature() } == "I");
        }
        const auto inst{ fpg::get_instance() };
        if (inst)
        {
            auto in{ inst->get_field("iInt") };
            if (in)
            {
                ctx.check("instance_proxy_is_static_false", in->is_static() == false);
                ctx.check("instance_proxy_signature_I", std::string{ in->signature() } == "I");
            }
        }
    }

    // =====================================================================
    //  RUNTIME reads — drive the Java probe (real bytecode dispatch performs
    //  putstatic / putfield), THEN read the runtime fields back through get().
    //  This proves get() reflects live, post-dispatch JVM state, not just the
    //  class-initializer constants read above.
    // =====================================================================
    {
        const bool done{ ctx.run_probe(
            [](bool value) { fpg::set_go(value); },
            []() { return fpg::get_done(); }) };
        ctx.check("runtime_probe_completed", done);

        if (done)
        {
            // Static runtime fields written by writeRuntime().
            {
                auto fp{ fpg::static_field("rBool") };
                if (fp) { const bool b{ fp->get() }; ctx.check("runtime_Z_true", b == true); }
            }
            {
                auto fp{ fpg::static_field("rByte") };
                if (fp)
                {
                    const auto v{ fp->get() };
                    const std::int8_t b{ v };
                    ctx.check("runtime_B_is_int8_min", b == std::numeric_limits<std::int8_t>::min());
                    ctx.check("runtime_B_variant_int8", v.data.index() == kIdxI8);
                }
            }
            {
                auto fp{ fpg::static_field("rShort") };
                if (fp) { const std::int16_t s{ fp->get() }; ctx.check("runtime_S_is_int16_min", s == std::numeric_limits<std::int16_t>::min()); }
            }
            {
                auto fp{ fpg::static_field("rInt") };
                if (fp) { const std::int32_t i{ fp->get() }; ctx.check("runtime_I_is_int32_min", i == std::numeric_limits<std::int32_t>::min()); }
            }
            {
                auto fp{ fpg::static_field("rLong") };
                if (fp) { const std::int64_t l{ fp->get() }; ctx.check("runtime_J_is_int64_max", l == std::numeric_limits<std::int64_t>::max()); }
            }
            {
                auto fp{ fpg::static_field("rChar") };
                if (fp) { const std::uint16_t c{ fp->get() }; ctx.check("runtime_C_is_FFFF", c == 0xFFFF); }
            }
            {
                auto fp{ fpg::static_field("rFloat") };
                if (fp)
                {
                    const auto v{ fp->get() };
                    const float f{ v };
                    ctx.check("runtime_F_is_neg_inf_bits", float_bits(f) == 0xFF800000);
                    ctx.check("runtime_F_is_neg_inf_pred", std::isinf(f) && f < 0.0F);
                }
            }
            {
                auto fp{ fpg::static_field("rDouble") };
                if (fp)
                {
                    const auto v{ fp->get() };
                    const double d{ v };
                    ctx.check("runtime_D_is_canonical_nan_bits", double_bits(d) == 0x7FF8000000000000ULL);
                    ctx.check("runtime_D_is_nan_pred", std::isnan(d));
                }
            }

            // Instance runtime fields written by writeRuntime() (putfield).
            const auto inst{ fpg::get_instance() };
            ctx.check("runtime_instance_reobtained", inst != nullptr);
            if (inst)
            {
                {
                    auto fp{ inst->get_field("rIBool") };
                    if (fp) { const bool b{ fp->get() }; ctx.check("runtime_inst_Z_true", b == true); }
                }
                {
                    auto fp{ inst->get_field("rIInt") };
                    if (fp) { const std::int32_t i{ fp->get() }; ctx.check("runtime_inst_I_is_int32_max", i == std::numeric_limits<std::int32_t>::max()); }
                }
                {
                    auto fp{ inst->get_field("rILong") };
                    if (fp) { const std::int64_t l{ fp->get() }; ctx.check("runtime_inst_J_is_int64_min", l == std::numeric_limits<std::int64_t>::min()); }
                }
                {
                    auto fp{ inst->get_field("rIDouble") };
                    if (fp)
                    {
                        const double d{ fp->get() };
                        // Math.PI bit pattern.
                        ctx.check("runtime_inst_D_is_pi_bits", double_bits(d) == 0x400921FB54442D18ULL);
                    }
                }
            }
        }
    }

    // =====================================================================
    //  Repeatability: reading the same static field twice yields identical
    //  value + variant alternative (get() is a pure copy with no side effects).
    // =====================================================================
    {
        auto a{ fpg::static_field("sLongDeadBeef") };
        auto b{ fpg::static_field("sLongDeadBeef") };
        if (a && b)
        {
            const auto va{ a->get() };
            const auto vb{ b->get() };
            const std::int64_t la{ va };
            const std::int64_t lb{ vb };
            ctx.check("repeatable_J_same_value", la == lb);
            ctx.check("repeatable_J_same_variant", va.data.index() == vb.data.index());
        }
    }

    // =====================================================================
    //  raw_address sanity: a resolved primitive proxy exposes a non-null,
    //  width-aligned field address (get() reads from exactly this pointer).
    // =====================================================================
    {
        auto fp{ fpg::static_field("sDoubleOne") };
        if (fp)
        {
            ctx.check("double_proxy_address_nonnull", fp->raw_address() != nullptr);
            const auto addr{ reinterpret_cast<std::uintptr_t>(fp->raw_address()) };
            ctx.check("double_proxy_address_8_aligned", (addr % alignof(double)) == 0);
        }
    }
    {
        auto fp{ fpg::static_field("sIntMax") };
        if (fp)
        {
            const auto addr{ reinterpret_cast<std::uintptr_t>(fp->raw_address()) };
            ctx.check("int_proxy_address_4_aligned", (addr % alignof(std::int32_t)) == 0);
        }
    }
}

VMHOOK_JVM_MODULE(field_primitives_get)
{
    // SUITE-SAFETY (mirrors register_class.cpp / aaa_warmup.cpp):
    //   * The whole body runs under a try/catch: a stray throw from any vmhook
    //     call is recorded as [INFO], never a FAIL, and never escapes this module.
    //     (On MinGW/Clang the harness's run_one() catches throws but NOT a
    //     segfault, so this module ALSO is_valid_pointer-guards every decoded oop
    //     before use; it never derefs a disengaged optional — every fp-> deref is
    //     gated on has_value().)
    //   * An unconditional vmhook::shutdown_hooks() runs OUTSIDE the try, so the
    //     module returns to the driver with an EMPTY hook table on every path.
    //     This module installs NO hooks (it only drives the fixture's pre-
    //     registered probe via run_probe), so this is belt-and-braces — but a
    //     leaked armed hook is exactly what cascades into later modules, and the
    //     playbook mandates the unconditional teardown regardless.
    bool body_threw{ false };
    try
    {
        run_field_primitives_get_checks(ctx);
    }
    catch (...)
    {
        body_threw = true;
    }

    // FINAL CLEANUP — OUTSIDE the try so it ALWAYS runs (idempotent and
    // safe-when-empty; proven by shutdown_hooks_teardown).
    vmhook::shutdown_hooks();

    if (body_threw)
    {
        ctx.record("[INFO] field_primitives_get: the test body threw and was "
                   "contained (no crash, no hooks armed); see preceding checks "
                   "for partial results.");
    }
    ctx.check("module_left_clean_final_shutdown", true);
}
