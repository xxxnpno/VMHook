// Standalone (no-JVM) tests for vmhook's diagnostic logging layer:
// detail::format_log formatting, the VMHOOK_LOG macro, the *_tag literals,
// detail::emit_log_line null/edge tolerance, and the never-throw guarantee.
//
// This is the ONE piece of vmhook that is pure host C++ (no JVM, no JNI): the
// formatter (std::vformat with a verbatim catch-fallback), the never-throw
// sink, the active/no-op macro split, and the bracketed tag literals.  It is
// therefore exhaustively exercisable here without a live HotSpot.
//
// Coverage philosophy: enumerate EVERY testable input class to format_log /
// emit_log_line / VMHOOK_LOG —
//   * every placeholder form  ({} in order, positional {N}, escaped {{ }},
//     specs: width/fill/align/sign/base/alt-form/precision, dynamic {:{}}),
//   * every argument type      (int family at INT/LLONG/UINT/ULLONG limits,
//     char/signed char/unsigned char, bool, float/double incl. inf/nan/-0,
//     pointers null/non-null, const char*/std::string/std::string_view incl.
//     empty/embedded-NUL/UTF-8/very-long),
//   * degenerate format strings (empty, lone { or }, more fields than args,
//     out-of-range positional) which must hit the catch and return verbatim,
//   * the std::cout sink (rdbuf-captured) so the ACTIVE vs NO-OP VMHOOK_LOG
//     divergence and the exact emitted bytes (incl. the trailing '\n') are
//     observed, plus a concurrent K*M-line interleaving test for the mutex,
//   * the *_tag literals' exact value / bracket structure.
//
// Everything that produces a *formatted* string is gated on
// VMHOOK_HAS_STD_FORMAT (GCC 13+/Clang 14+/MSVC 19.29+); on older toolchains
// format_log returns the format string verbatim, so the #else branch asserts
// that byte-for-byte guarantee for the same inputs.
#include <vmhook/vmhook.hpp>

#include <cstdio>
#include <cstdint>
#include <climits>
#include <cfloat>
#include <limits>
#include <string>
#include <string_view>
#include <sstream>
#include <streambuf>
#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <type_traits>
#include <cmath>
#include <cctype>
#include <cstdlib>      // std::strtod for the FP round-trip companion checks

// ---------------------------------------------------------------------------
// Platform-invariant floating-point helpers.
//
// std::format pins the *value* of a floating-point result but NOT every byte of
// its spelling: the standard leaves several things implementation-defined, and
// libstdc++ (the MinGW/GCC leg) and the MSVC STL legitimately differ on them.
// Known divergences this test must survive on EITHER STL:
//   * hex-float ({:a}/{:A}): libstdc++ -> "1p+0"; some MSVC STL versions ->
//     "0x1p+0" (the "0x" prefix on hexfloat is not pinned by the standard).
//   * NaN sign/spelling: libstdc++ -> "nan"/"-nan"; classic MSVC STL ->
//     "nan(ind)" / "-nan(ind)".  Only "the token contains 'nan'" is portable.
//   * the default ({}) and {:g} forms emit the SHORTEST round-tripping decimal,
//     which the standard DOES pin to a value but whose exact digits are most
//     robustly asserted by parsing them back and comparing the double.
// For each such case we assert a *property* (round-trips to the same double, or
// contains the right token, with the right sign) instead of an exact string.
// Things the standard fully pins (sign char, "inf", the 'e'/'E'/'f' fixed and
// scientific layouts with explicit precision, integer bases/alt-form) stay as
// exact-string checks elsewhere — only the genuinely implementation-defined
// spellings are relaxed here.
namespace fpinv
{
    // Lower-case a copy (ASCII) so token checks are spelling-case agnostic.
    inline auto lower(std::string s) -> std::string
    {
        for (char& c : s)
        {
            c = static_cast<char>(
                std::tolower(static_cast<unsigned char>(c)));
        }
        return s;
    }

    inline auto contains_ci(std::string_view hay, std::string_view needle) -> bool
    {
        return lower(std::string{ hay }).find(lower(std::string{ needle }))
            != std::string::npos;
    }

    // Parse a formatted floating-point string back to a double.
    //
    // We deliberately use std::strtod (<cstdlib>) rather than the float overload
    // of std::from_chars.  std::from_chars(const char*, const char*, double&) is
    // a THIRD point of STL divergence on this test's surface: libc++ ships the
    // INTEGER from_chars only and explicitly `= delete`s the floating-point
    // overloads, so any TU that names it fails to COMPILE on Apple clang/libc++
    // (the macOS + iOS CI legs) — even inside an untaken branch, because deleted
    // overloads are diagnosed at the call expression, not at link time.  strtod
    // is C-library and universally available (including libc++), so the
    // round-trip companion checks stay portable.
    //
    // Round-trip semantics are equivalent for our inputs: every value routed
    // through here is a finite decimal (1.5, 0.0, -1.5, 100000.0, 1e6, 0.1, …)
    // and strtod performs correct round-to-nearest, so the parsed double equals
    // the original BIT-FOR-BIT.  The non-finite / hex-float / NaN forms never
    // come through here — their call sites assert tokens directly via
    // is_inf_spelling / is_nan_spelling / is_hexfloat_spelling.
    inline auto parses_to(std::string_view text, double expected) -> bool
    {
        // strtod needs a NUL-terminated C-string; a std::string supplies one via
        // c_str().  The formatted FP renderings never contain an embedded NUL, so
        // the whole token is the parse subject.
        const std::string token{ text };
        char* end{ nullptr };
        const double value{ std::strtod(token.c_str(), &end) };
        // Reject: nothing parsed, or trailing junk after the number (end must
        // land on the terminating NUL == one-past the last char).
        if (end == token.c_str()
            || end != token.c_str() + token.size())
        {
            return false;
        }
        // Bit-exact: these are shortest-round-trip (or fixed-precision) spellings,
        // so the parsed value must reproduce the original double exactly (handles
        // -0.0 too, since we also check the sign bit separately at the call site).
        return value == expected;
    }

    // True when `text` is a valid rendering of +/-infinity: contains "inf"
    // (case-insensitive) and its sign matches `negative`.
    inline auto is_inf_spelling(std::string_view text, bool negative) -> bool
    {
        if (!contains_ci(text, "inf"))
        {
            return false;
        }
        const bool has_minus{ !text.empty() && text.front() == '-' };
        return has_minus == negative;
    }

    // True when `text` is a valid rendering of NaN: contains "nan"
    // (case-insensitive).  NaN has no meaningful sign requirement in the
    // standard's default form (the sign is unspecified), so we do NOT pin it.
    inline auto is_nan_spelling(std::string_view text) -> bool
    {
        return contains_ci(text, "nan");
    }

    // A hex-float ({:a}) rendering is "[sign] [0x] mantissa 'p' [sign] exp"
    // where the "0x" is OPTIONAL across STLs.  We validate the invariant
    // structure (a 'p'/'P' exponent marker is present and the value parses)
    // rather than the exact "0x" presence.  `upper` selects p vs P.
    inline auto is_hexfloat_spelling(std::string_view text, bool upper) -> bool
    {
        const char marker{ upper ? 'P' : 'p' };
        return text.find(marker) != std::string_view::npos;
    }
}

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

// ---------------------------------------------------------------------------
// std::cout capture helper.  emit_log_line (and therefore the active VMHOOK_LOG)
// writes to std::cout when VMHOOK_LOG_FILE is undefined — which it is for every
// test target (see tests/CMakeLists.txt).  We swap std::cout's streambuf for an
// in-memory one so the EXACT emitted bytes (including the trailing '\n' the sink
// appends) can be asserted, then restore it.  RAII so an assertion that throws
// can never leave std::cout pointing at a destroyed buffer.
namespace
{
    class cout_capture
    {
    public:
        cout_capture()
            : old_{ std::cout.rdbuf(buffer_.rdbuf()) }
        {
        }
        ~cout_capture()
        {
            std::cout.rdbuf(old_);
        }
        cout_capture(const cout_capture&)            = delete;
        cout_capture& operator=(const cout_capture&) = delete;

        auto str() const -> std::string { return buffer_.str(); }

    private:
        std::ostringstream  buffer_{};
        std::streambuf*     old_{ nullptr };
    };
}

int main()
{
    // ---------------------------------------------------------------------
    // error_tag / warning_tag / info_tag — stable, non-empty string literals.
    // The header declares them as `inline constexpr std::string_view`.
    // ---------------------------------------------------------------------
    check("error_tag_non_empty", !vmhook::error_tag.empty());
    check("error_tag_exact_value", vmhook::error_tag == "[VMHook ERROR]");
    check("warning_tag_non_empty", !vmhook::warning_tag.empty());
    check("warning_tag_exact_value", vmhook::warning_tag == "[VMHook WARNING]");
    check("info_tag_non_empty", !vmhook::info_tag.empty());
    check("info_tag_exact_value", vmhook::info_tag == "[VMHook INFO]");
    // Tags are distinct from one another.
    check("tags_distinct",
        vmhook::error_tag != vmhook::warning_tag &&
        vmhook::warning_tag != vmhook::info_tag &&
        vmhook::error_tag != vmhook::info_tag);
    // It is a std::string_view (its element type is char), so this comparison
    // is meaningful and the literal carries the documented bracket prefix.
    check("error_tag_starts_with_bracket",
        !vmhook::error_tag.empty() && vmhook::error_tag.front() == '[');
    // Symmetry: every tag is bracket-delimited "[VMHook ...]" — assert the
    // structure on all three, not just error_tag (front '[' and back ']').
    check("warning_tag_starts_with_bracket",
        !vmhook::warning_tag.empty() && vmhook::warning_tag.front() == '[');
    check("info_tag_starts_with_bracket",
        !vmhook::info_tag.empty() && vmhook::info_tag.front() == '[');
    check("error_tag_ends_with_bracket",
        !vmhook::error_tag.empty() && vmhook::error_tag.back() == ']');
    check("warning_tag_ends_with_bracket",
        !vmhook::warning_tag.empty() && vmhook::warning_tag.back() == ']');
    check("info_tag_ends_with_bracket",
        !vmhook::info_tag.empty() && vmhook::info_tag.back() == ']');
    // Every tag carries the common "VMHook" brand substring.
    check("error_tag_contains_brand",
        vmhook::error_tag.find("VMHook") != std::string_view::npos);
    check("warning_tag_contains_brand",
        vmhook::warning_tag.find("VMHook") != std::string_view::npos);
    check("info_tag_contains_brand",
        vmhook::info_tag.find("VMHook") != std::string_view::npos);
    // The tags are constexpr string_views — usable in a constant expression.
    {
        static_assert(!vmhook::error_tag.empty(), "error_tag must be constexpr-usable");
        static_assert(vmhook::error_tag.front() == '[', "error_tag front constexpr");
        static_assert(vmhook::warning_tag.back() == ']', "warning_tag back constexpr");
        check("tags_are_constexpr_usable", true);
    }

    // ---------------------------------------------------------------------
    // detail::format_log positive path.  These produce a formatted string only
    // when the toolchain ships std::format (VMHOOK_HAS_STD_FORMAT); otherwise
    // format_log returns the format string verbatim, so the expectations are
    // gated on that macro to stay correct on every supported compiler.
    // ---------------------------------------------------------------------
#if VMHOOK_HAS_STD_FORMAT
    // int argument.
    check("format_int",
        vmhook::detail::format_log("{}", 42) == "42");
    // string argument (std::string).
    check("format_string",
        vmhook::detail::format_log("{}", std::string{ "hello" }) == "hello");
    // string-literal argument.
    check("format_cstring",
        vmhook::detail::format_log("v={}", "x") == "v=x");
    // pointer argument — std::format renders pointers as 0x.. ; a null pointer
    // is well defined.  We only assert the value is non-empty and that a
    // non-null pointer yields a different rendering than nullptr.
    {
        int local{ 0 };
        const std::string p_null{ vmhook::detail::format_log("{}", static_cast<void*>(nullptr)) };
        const std::string p_some{ vmhook::detail::format_log("{}", static_cast<void*>(&local)) };
        check("format_pointer_non_empty", !p_null.empty() && !p_some.empty());
        check("format_pointer_distinct", p_null != p_some);
    }
    // float / double argument — 1.5 is exactly representable, so its shortest
    // round-trip spelling is the standard-pinned "1.5" on every STL.  Assert the
    // exact string AND that it round-trips back to the same double (the latter
    // is the STL-invariant property).
    check("format_double",
        vmhook::detail::format_log("{}", 1.5) == "1.5");
    check("format_double_roundtrips",
        fpinv::parses_to(vmhook::detail::format_log("{}", 1.5), 1.5));
    // Multiple heterogeneous args in one call (int, string, pointer-ish).
    check("format_multi_args",
        vmhook::detail::format_log("{}-{}-{}", 7, std::string{ "ab" }, 3) == "7-ab-3");
    // Positional / reordered indices.
    check("format_indexed",
        vmhook::detail::format_log("{1}{0}", 1, 2) == "21");
    // Width / fill spec is honoured.
    check("format_width_spec",
        vmhook::detail::format_log("{:03}", 5) == "005");
    // A format call that embeds error_tag, exactly as the library does
    // internally (e.g. VMHOOK_LOG("{} ...", vmhook::error_tag, ...)).
    check("format_with_error_tag",
        vmhook::detail::format_log("{} boom", vmhook::error_tag) == "[VMHook ERROR] boom");
    // A plain format string with no replacement fields round-trips unchanged.
    check("format_no_fields",
        vmhook::detail::format_log("literal text") == "literal text");
    // Escaped braces collapse to single braces.
    check("format_escaped_braces",
        vmhook::detail::format_log("{{{}}}", 9) == "{9}");

    // --- More escaped-brace / literal-character cases. -------------------
    // A lone escaped open and close with no field.
    check("format_escaped_open_brace_only",
        vmhook::detail::format_log("{{") == "{");
    check("format_escaped_close_brace_only",
        vmhook::detail::format_log("}}") == "}");
    check("format_double_escaped_pair",
        vmhook::detail::format_log("{{}}") == "{}");
    // A percent sign is an ORDINARY character in std::format (unlike printf):
    // it passes through verbatim with no argument consumed.
    check("format_percent_is_literal",
        vmhook::detail::format_log("100%") == "100%");
    check("format_percent_d_is_literal",
        vmhook::detail::format_log("%d and {}", 5) == "%d and 5");
    // Backslashes are ordinary too (std::format has no escape processing).
    check("format_backslash_is_literal",
        vmhook::detail::format_log("a\\b{}", 1) == "a\\b1");
    // Escaped braces immediately around / adjacent to a real field.
    check("format_escaped_then_field",
        vmhook::detail::format_log("{{}}{}", 7) == "{}7");
    check("format_field_then_escaped",
        vmhook::detail::format_log("{}{{}}", 7) == "7{}");
    check("format_escaped_braces_with_text",
        vmhook::detail::format_log("set {{key}} = {}", 9) == "set {key} = 9");

    // --- More substitution arities (0..4 fields). -----------------------
    check("format_arity_0_plain",
        vmhook::detail::format_log("none") == "none");
    check("format_arity_1",
        vmhook::detail::format_log("[{}]", 1) == "[1]");
    check("format_arity_2",
        vmhook::detail::format_log("{},{}", 1, 2) == "1,2");
    check("format_arity_4",
        vmhook::detail::format_log("{}{}{}{}", 1, 2, 3, 4) == "1234");
    // Higher arity (the internal call sites go up to ~6 args).
    check("format_arity_6",
        vmhook::detail::format_log("{}{}{}{}{}{}", 1, 2, 3, 4, 5, 6) == "123456");
    check("format_arity_8_mixed",
        vmhook::detail::format_log("{}/{}/{}/{}/{}/{}/{}/{}",
            1, "a", 2, "b", 3, "c", 4, "d") == "1/a/2/b/3/c/4/d");
    // Extra trailing arguments are simply ignored (NOT an error in std::format).
    check("format_extra_args_ignored",
        vmhook::detail::format_log("{}", 1, 2, 3) == "1");
    check("format_no_field_with_args_ignored",
        vmhook::detail::format_log("literal", 1, 2) == "literal");
    // Empty format string WITH arguments: no fields consume them -> empty.
    check("format_empty_fmt_with_args_empty",
        vmhook::detail::format_log("", 1, 2, 3).empty());

    // --- Repeated / reordered positional indices. -----------------------
    check("format_repeated_index",
        vmhook::detail::format_log("{0}{0}{0}", 7) == "777");
    check("format_full_reverse_index",
        vmhook::detail::format_log("{2}{1}{0}", 1, 2, 3) == "321");
    check("format_mixed_repeat_and_order",
        vmhook::detail::format_log("{1}{0}{1}", std::string{ "a" }, std::string{ "b" })
            == "bab");
    // Positional index with a spec attached.
    check("format_positional_with_spec",
        vmhook::detail::format_log("{0:03}-{0:#x}", 255) == "255-0xff");

    // --- Format-spec variety (width / fill / align / sign / base / prec). */
    check("format_right_align_width",
        vmhook::detail::format_log("{:>5}", 7) == "    7");
    check("format_left_align_width",
        vmhook::detail::format_log("{:<5}", 7) == "7    ");
    check("format_center_align_width",
        vmhook::detail::format_log("{:^5}", 7) == "  7  ");
    check("format_custom_fill_center",
        vmhook::detail::format_log("{:*^5}", 7) == "**7**");
    check("format_custom_fill_left",
        vmhook::detail::format_log("{:.<5}", 7) == "7....");
    check("format_custom_fill_right",
        vmhook::detail::format_log("{:0>5}", 7) == "00007");
    check("format_zero_pad_width",
        vmhook::detail::format_log("{:05}", 42) == "00042");
    check("format_explicit_plus_sign",
        vmhook::detail::format_log("{:+}", 5) == "+5");
    check("format_explicit_plus_on_negative",
        vmhook::detail::format_log("{:+}", -5) == "-5");
    // The space-flag: a leading space for non-negative, '-' for negative.
    check("format_space_sign_positive",
        vmhook::detail::format_log("{: }", 5) == " 5");
    check("format_space_sign_negative",
        vmhook::detail::format_log("{: }", -5) == "-5");
    check("format_plus_sign_on_zero",
        vmhook::detail::format_log("{:+}", 0) == "+0");
    check("format_hex_lower",
        vmhook::detail::format_log("{:x}", 255) == "ff");
    check("format_hex_alt_form",
        vmhook::detail::format_log("{:#x}", 255) == "0xff");
    check("format_hex_upper",
        vmhook::detail::format_log("{:X}", 255) == "FF");
    check("format_hex_upper_alt_form",
        vmhook::detail::format_log("{:#X}", 255) == "0XFF");
    check("format_binary",
        vmhook::detail::format_log("{:b}", 5) == "101");
    check("format_binary_alt_form",
        vmhook::detail::format_log("{:#b}", 5) == "0b101");
    check("format_octal",
        vmhook::detail::format_log("{:o}", 8) == "10");
    check("format_octal_alt_form",
        vmhook::detail::format_log("{:#o}", 8) == "010");
    check("format_decimal_explicit",
        vmhook::detail::format_log("{:d}", 42) == "42");
    // std::format renders NEGATIVE signed integers with a leading '-' even
    // under a base spec (it is NOT printf's unsigned two's-complement).  Pin it.
    check("format_hex_negative_is_signed",
        vmhook::detail::format_log("{:x}", -1) == "-1");
    check("format_hex_negative_value",
        vmhook::detail::format_log("{:x}", -255) == "-ff");
    check("format_fixed_precision",
        vmhook::detail::format_log("{:.2f}", 1.5) == "1.50");
    check("format_fixed_precision_zero",
        vmhook::detail::format_log("{:.0f}", 2.5) == "2");
    // The internal hook-logging idiom uses {:08X} on a compressed OOP; pin it.
    check("format_hex_zero_pad_8_upper",
        vmhook::detail::format_log("0x{:08X}", 0xDEADu) == "0x0000DEAD");
    // Combined sign + alt-form + zero-pad + total-width-8 on a positive: the
    // '+' and "0x" prefix COUNT toward the field width, so the zero-fill makes
    // up the remaining 5 columns: '+' "0x" "000ff" == "+0x000ff" (8 chars).
    check("format_combined_sign_altform_zeropad",
        vmhook::detail::format_log("{:+#08x}", 255) == "+0x000ff");
    // Dynamic (nested) width and precision drawn from later args.
    check("format_dynamic_width",
        vmhook::detail::format_log("{:{}}", 7, 4) == "   7");
    check("format_dynamic_width_and_fill",
        vmhook::detail::format_log("{:*>{}}", 7, 4) == "***7");
    check("format_dynamic_precision",
        vmhook::detail::format_log("{:.{}f}", 3.14159, 2) == "3.14");

    // --- Integer boundary sweep: every width / signedness limit. ---------
    check("format_int_zero",
        vmhook::detail::format_log("{}", 0) == "0");
    check("format_int_min",
        vmhook::detail::format_log("{}", INT_MIN) == "-2147483648");
    check("format_int_max",
        vmhook::detail::format_log("{}", INT_MAX) == "2147483647");
    check("format_uint_max",
        vmhook::detail::format_log("{}", UINT_MAX) == "4294967295");
    check("format_long_long_min",
        vmhook::detail::format_log("{}", LLONG_MIN) == "-9223372036854775808");
    check("format_long_long_max",
        vmhook::detail::format_log("{}", LLONG_MAX) == "9223372036854775807");
    check("format_ulong_long_max",
        vmhook::detail::format_log("{}", ULLONG_MAX) == "18446744073709551615");
    // Explicit fixed-width integer types.
    check("format_int8_min",
        vmhook::detail::format_log("{}", static_cast<std::int8_t>(-128)) == "-128");
    check("format_uint8_max",
        vmhook::detail::format_log("{}", static_cast<std::uint8_t>(255)) == "255");
    check("format_int16_min",
        vmhook::detail::format_log("{}", static_cast<std::int16_t>(-32768)) == "-32768");
    check("format_uint16_max",
        vmhook::detail::format_log("{}", static_cast<std::uint16_t>(65535)) == "65535");
    check("format_uint32_max_hex",
        vmhook::detail::format_log("{:08X}", static_cast<std::uint32_t>(0xFFFFFFFFu))
            == "FFFFFFFF");
    check("format_uint64_max_hex",
        vmhook::detail::format_log("{:016x}", static_cast<std::uint64_t>(~std::uint64_t{ 0 }))
            == "ffffffffffffffff");
    // size_t / intptr_t round-trips.
    check("format_size_t_value",
        vmhook::detail::format_log("{}", static_cast<std::size_t>(123456789u))
            == "123456789");

    // --- char / signed char / unsigned char / bool. ---------------------
    // A plain `char` formats as its character glyph...
    check("format_char_glyph",
        vmhook::detail::format_log("{}", 'A') == "A");
    // ...but with {:d} it formats as its integer code.
    check("format_char_as_int",
        vmhook::detail::format_log("{:d}", 'A') == "65");
    // signed char / unsigned char are INTEGER types in std::format (not chars):
    // they render as numbers by default.
    check("format_signed_char_is_number",
        vmhook::detail::format_log("{}", static_cast<signed char>(-1)) == "-1");
    check("format_unsigned_char_is_number",
        vmhook::detail::format_log("{}", static_cast<unsigned char>(200)) == "200");
    // The {:c} spec turns an integer back into its character.
    check("format_int_as_char_spec",
        vmhook::detail::format_log("{:c}", 66) == "B");
    // bool: textual by default, numeric with {:d}.
    check("format_bool_true",
        vmhook::detail::format_log("{}", true) == "true");
    check("format_bool_false",
        vmhook::detail::format_log("{}", false) == "false");
    check("format_bool_true_as_int",
        vmhook::detail::format_log("{:d}", true) == "1");
    check("format_bool_false_as_int",
        vmhook::detail::format_log("{:d}", false) == "0");

    // --- Floating-point: specials, signs, and the format types. ----------
    //
    // PORTABILITY NOTE: the std::format float spellings split into two classes.
    //   (1) Fully standard-pinned layouts — the 'e'/'E'/'f' forms with an
    //       explicit precision, the sign character, and the literal token "inf"
    //       — are byte-identical on libstdc++ and the MSVC STL, so they keep
    //       EXACT-string asserts.
    //   (2) Implementation-defined spellings — the SHORTEST round-trip default
    //       ({}) / {:g} digits, hex-float ({:a}/{:A}), and the NaN token (plain
    //       "nan" on libstdc++, "nan(ind)" on classic MSVC) — are asserted as
    //       PROPERTIES (round-trips to the same double / contains the right
    //       token / has the right sign) so the test is green on EITHER STL.
    //
    // The shortest default ({}) is pinned by the standard to the shortest
    // decimal that round-trips, so we assert BOTH: round-trip equality (the
    // invariant that survives any STL) AND — for the values where every
    // shortest-form implementation must agree (exact binary fractions like
    // 0.0/-0.0/-1.5/2.5) — the exact spelling.
    check("format_double_zero",
        vmhook::detail::format_log("{}", 0.0) == "0");
    check("format_double_zero_roundtrips",
        fpinv::parses_to(vmhook::detail::format_log("{}", 0.0), 0.0));
    // Negative zero keeps its sign in the default presentation.  std::format
    // pins this: the shortest form of -0.0 is "-0".  Assert exact AND that the
    // sign bit survives (round-trip of -0 also yields a value == 0.0).
    {
        const std::string nz{ vmhook::detail::format_log("{}", -0.0) };
        check("format_double_neg_zero", nz == "-0");
        check("format_double_neg_zero_has_minus",
            !nz.empty() && nz.front() == '-');
    }
    check("format_double_negative",
        vmhook::detail::format_log("{}", -1.5) == "-1.5");
    check("format_double_negative_roundtrips",
        fpinv::parses_to(vmhook::detail::format_log("{}", -1.5), -1.5));
    // inf: the standard pins the lowercase token "inf" for the default / 'f' /
    // signed forms, and both STLs comply — but we assert the INVARIANT (token
    // "inf" present, correct sign/prefix) so a future STL that spelled it
    // "infinity" would still pass.  The explicit '+'/'-' sign IS pinned, so the
    // sign half of each check is exact.
    {
        const double inf{ std::numeric_limits<double>::infinity() };
        const std::string s_inf{ vmhook::detail::format_log("{}", inf) };
        const std::string s_ninf{ vmhook::detail::format_log("{}", -inf) };
        const std::string s_inf_f{ vmhook::detail::format_log("{:f}", inf) };
        const std::string s_inf_p{ vmhook::detail::format_log("{:+}", inf) };
        check("format_double_inf", fpinv::is_inf_spelling(s_inf, false));
        check("format_double_neg_inf", fpinv::is_inf_spelling(s_ninf, true));
        check("format_double_neg_inf_has_minus",
            !s_ninf.empty() && s_ninf.front() == '-');
        check("format_double_inf_fixed", fpinv::is_inf_spelling(s_inf_f, false));
        check("format_double_inf_with_plus",
            fpinv::is_inf_spelling(s_inf_p, false)
                && !s_inf_p.empty() && s_inf_p.front() == '+');
    }
    // NaN: the exact spelling is implementation-defined — libstdc++ emits
    // "nan"/"-nan", classic MSVC STL emits "nan(ind)"/"-nan(ind)".  Assert ONLY
    // the portable invariant: a non-empty token containing "nan"
    // (case-insensitive).  The sign of a NaN is unspecified, so it is NOT
    // pinned for either the positive or the negated input.
    {
        const std::string s_nan{
            vmhook::detail::format_log("{}", std::numeric_limits<double>::quiet_NaN()) };
        const std::string s_nnan{
            vmhook::detail::format_log("{}", -std::numeric_limits<double>::quiet_NaN()) };
        const std::string s_nan_f{
            vmhook::detail::format_log("{:f}", std::numeric_limits<double>::quiet_NaN()) };
        check("format_double_nan",
            !s_nan.empty() && fpinv::is_nan_spelling(s_nan));
        check("format_double_neg_nan_contains_nan",
            !s_nnan.empty() && fpinv::is_nan_spelling(s_nnan));
        check("format_double_nan_fixed",
            !s_nan_f.empty() && fpinv::is_nan_spelling(s_nan_f));
    }
    // Presentation types.  The 'e'/'E'/'f' forms with explicit/default
    // precision are byte-pinned by the standard (the digit count is fixed, not
    // shortest), so these stay EXACT on both STLs.
    check("format_double_scientific",
        vmhook::detail::format_log("{:e}", 1.5) == "1.500000e+00");
    check("format_double_scientific_upper",
        vmhook::detail::format_log("{:E}", 1.5) == "1.500000E+00");
    check("format_double_fixed_default",
        vmhook::detail::format_log("{:f}", 1.5) == "1.500000");
    // Hex-float ({:a}/{:A}): the "0x"/"0X" prefix is NOT pinned by the standard
    // — libstdc++ emits "1p+0", some MSVC STL versions emit "0x1p+0".  Assert
    // ONLY the INVARIANT structure here (a 'p'/'P' exponent marker is present,
    // lowercase vs uppercase tracks the spec letter).  We do NOT round-trip the
    // hex-float spelling: strtod's hex-float support is itself uneven across
    // libraries, so going through parses_to here would reintroduce a portability
    // hazard — the p-marker + letter-case checks already pin the spelling.
    {
        const std::string hx{ vmhook::detail::format_log("{:a}", 1.0) };
        const std::string hX{ vmhook::detail::format_log("{:A}", 1.0) };
        check("format_double_hexfloat_has_p_marker",
            fpinv::is_hexfloat_spelling(hx, /*upper=*/false));
        check("format_double_hexfloat_upper_has_P_marker",
            fpinv::is_hexfloat_spelling(hX, /*upper=*/true));
        // Lower form must NOT contain an uppercase 'P' and vice-versa (the
        // letter case IS pinned even though the "0x" prefix is not).
        check("format_double_hexfloat_lower_no_upper_P",
            hx.find('P') == std::string::npos);
        check("format_double_hexfloat_upper_no_lower_p",
            hX.find('p') == std::string::npos);
    }
    // {:g} small value: 100000.0 is below the exponent-switch threshold so it
    // renders without an exponent.  Both STLs agree on the digits here, and the
    // value is exact, so assert exact AND round-trip.
    check("format_double_general_small",
        vmhook::detail::format_log("{:g}", 100000.0) == "100000");
    check("format_double_general_small_roundtrips",
        fpinv::parses_to(vmhook::detail::format_log("{:g}", 100000.0), 100000.0));
    // {:g} at/over the switch threshold uses scientific notation.  The exact
    // exponent spelling ("1e+06") is pinned by the standard's 'g' rules and
    // matches on both STLs; also assert it round-trips.
    check("format_double_general_switch_to_exp",
        vmhook::detail::format_log("{:g}", 1000000.0) == "1e+06");
    check("format_double_general_switch_roundtrips",
        fpinv::parses_to(vmhook::detail::format_log("{:g}", 1000000.0), 1000000.0));
    // Explicit-precision {:.17g} on 0.1 prints a FIXED 17 significant digits
    // (not shortest), so the digit string is pinned by the standard and equal
    // on both STLs.  Assert exact AND that it round-trips to the same double.
    check("format_double_full_precision",
        vmhook::detail::format_log("{:.17g}", 0.1) == "0.10000000000000001");
    check("format_double_full_precision_roundtrips",
        fpinv::parses_to(vmhook::detail::format_log("{:.17g}", 0.1), 0.1));
    check("format_double_signed_fixed",
        vmhook::detail::format_log("{:+.2f}", 3.14159) == "+3.14");
    // A float (not double) value — shortest form, pinned to "2.5"; also assert
    // round-trip.
    check("format_float_value",
        vmhook::detail::format_log("{}", 2.5f) == "2.5");
    check("format_float_value_roundtrips",
        fpinv::parses_to(vmhook::detail::format_log("{}", 2.5f), 2.5));

    // --- const char* / std::string / std::string_view payloads. ---------
    check("format_empty_string_arg",
        vmhook::detail::format_log("[{}]", std::string{}) == "[]");
    check("format_empty_cstring_arg",
        vmhook::detail::format_log("[{}]", "") == "[]");
    {
        std::string_view sv{ "view-arg" };
        check("format_string_view_arg",
            vmhook::detail::format_log("{}", sv) == "view-arg");
    }
    // A std::string containing an embedded NUL: std::format copies by SIZE, so
    // the result must be the full 5 bytes "ab\0cd" — NOT truncated at the NUL.
    {
        std::string emb{ "ab" };
        emb.push_back('\0');
        emb += "cd";
        const std::string produced{ vmhook::detail::format_log("{}", emb) };
        check("format_string_with_embedded_nul_size",
            produced.size() == 5u);
        check("format_string_with_embedded_nul_bytes",
            produced == emb);
    }
    // UTF-8 multibyte payload: bytes are preserved verbatim (std::format is
    // byte-oriented for `char`).  "héllo" is 6 bytes (the é is 2 bytes).
    {
        std::string u8{ "h\xC3\xA9llo" };
        const std::string produced{ vmhook::detail::format_log("[{}]", u8) };
        check("format_utf8_byte_preserving_size",
            produced.size() == u8.size() + 2u);
        check("format_utf8_byte_preserving_value",
            produced == "[" + u8 + "]");
    }
    // Raw control characters inside an argument pass through untouched.
    check("format_control_chars_in_arg",
        vmhook::detail::format_log("{}", std::string{ "a\tb\nc" }) == "a\tb\nc");
    // String arg with brace characters in the DATA (not the format string):
    // they are literal, never re-parsed.
    check("format_braces_in_arg_not_reparsed",
        vmhook::detail::format_log("{}", std::string{ "{not a field}" }) == "{not a field}");

    // --- Very long format string and a long substituted value. ----------
    {
        const std::string long_literal(4096u, 'L');
        check("format_very_long_literal",
            vmhook::detail::format_log(long_literal) == long_literal);
        const std::string long_arg(2048u, 'A');
        const std::string produced{ vmhook::detail::format_log("<{}>", long_arg) };
        check("format_long_substituted_value",
            produced == "<" + long_arg + ">");
        check("format_long_substituted_value_length",
            produced.size() == long_arg.size() + 2u);
    }
    // A format string with MANY fields (stress the parser/arg-store).  Use a
    // repeated single positional index so one argument drives 32 fields — the
    // same parser/arg-store stress without needing 32 distinct arguments.
    {
        std::string fmt0;
        std::string expected;
        for (int i{ 0 }; i < 32; ++i)
        {
            fmt0 += "{0}";
            expected += "7";
        }
        check("format_many_repeated_positional",
            vmhook::detail::format_log(fmt0, 7) == expected);
    }

    // --- Malformed-but-runtime-parsed format strings must NOT throw; the ---
    //     catch arm hands back the verbatim format string (vmhook.hpp:333-340).
    {
        // A lone unmatched '{' is an invalid format -> std::vformat throws ->
        // format_log returns the fmt verbatim.
        check("format_unmatched_open_brace_returns_fmt",
            vmhook::detail::format_log("{", 1) == "{");
        // A lone unmatched '{' with NO args also throws -> verbatim.
        check("format_lone_open_brace_no_args",
            vmhook::detail::format_log("{") == "{");
        // A stray unmatched '}' is also invalid -> verbatim.
        check("format_unmatched_close_brace_returns_fmt",
            vmhook::detail::format_log("a}b") == "a}b");
        // A lone '}' on its own.
        check("format_lone_close_brace",
            vmhook::detail::format_log("}") == "}");
        // An out-of-range positional index throws at runtime -> verbatim.
        check("format_index_out_of_range_returns_fmt",
            vmhook::detail::format_log("{5}", 1) == "{5}");
        // Out-of-range with ZERO args.
        check("format_field_with_no_args_returns_fmt",
            vmhook::detail::format_log("{0}") == "{0}");
        // More fields than args (the 2nd {} has no arg) -> throws -> verbatim.
        check("format_more_fields_than_args_returns_fmt",
            vmhook::detail::format_log("{} {}", 1) == "{} {}");
        // A bad format spec for the type throws -> verbatim.  (':d' on a
        // non-integer std::string is invalid.)
        check("format_bad_spec_for_type_returns_fmt",
            vmhook::detail::format_log("{:d}", std::string{ "x" }) == "{:d}");
        // ':s' (string presentation) on an int is invalid -> verbatim.
        check("format_string_spec_on_int_returns_fmt",
            vmhook::detail::format_log("{:s}", 1) == "{:s}");
        // An unterminated replacement field "{:" -> verbatim.
        check("format_unterminated_field_returns_fmt",
            vmhook::detail::format_log("{:", 1) == "{:");
        // A garbage spec body -> verbatim.
        check("format_garbage_spec_returns_fmt",
            vmhook::detail::format_log("{:ZZZ}", 1) == "{:ZZZ}");
    }
#else
    // Without std::format, format_log ignores specifiers and returns the
    // format string verbatim (documented fallback behaviour).  Mirror the
    // positive-path inputs here so the pre-std::format toolchains in CI still
    // exercise this surface — every input, formatted or malformed, must come
    // back byte-for-byte.
    check("format_fallback_verbatim",
        vmhook::detail::format_log("{}", 42) == "{}");
    check("format_fallback_plain",
        vmhook::detail::format_log("literal text") == "literal text");
    check("format_fallback_with_tag",
        vmhook::detail::format_log("{} boom", vmhook::error_tag) == "{} boom");
    // In the fallback, EVERY format string is returned byte-for-byte, ignoring
    // both arguments and specifiers — so even malformed / spec-heavy strings are
    // safe and verbatim.
    check("format_fallback_specifiers_verbatim",
        vmhook::detail::format_log("{:08X}", 0xDEADu) == "{:08X}");
    check("format_fallback_escaped_braces_verbatim",
        vmhook::detail::format_log("{{{}}}", 9) == "{{{}}}");
    check("format_fallback_unmatched_brace_verbatim",
        vmhook::detail::format_log("{", 1) == "{");
    check("format_fallback_multi_args_verbatim",
        vmhook::detail::format_log("{}-{}-{}", 1, 2, 3) == "{}-{}-{}");
    check("format_fallback_percent_verbatim",
        vmhook::detail::format_log("100%") == "100%");
    // Mirror the typed-argument and boundary inputs: in the fallback the
    // argument values never appear — only the format string survives.
    check("format_fallback_int_min_verbatim",
        vmhook::detail::format_log("{}", INT_MIN) == "{}");
    check("format_fallback_bool_verbatim",
        vmhook::detail::format_log("{}", true) == "{}");
    check("format_fallback_double_verbatim",
        vmhook::detail::format_log("{}", 1.5) == "{}");
    check("format_fallback_pointer_verbatim",
        vmhook::detail::format_log("{}", static_cast<void*>(nullptr)) == "{}");
    check("format_fallback_string_arg_verbatim",
        vmhook::detail::format_log("v={}", std::string{ "ignored" }) == "v={}");
    check("format_fallback_positional_verbatim",
        vmhook::detail::format_log("{1}{0}", 1, 2) == "{1}{0}");
    check("format_fallback_dynamic_width_verbatim",
        vmhook::detail::format_log("{:{}}", 7, 4) == "{:{}}");
    check("format_fallback_out_of_range_verbatim",
        vmhook::detail::format_log("{5}", 1) == "{5}");
    check("format_fallback_empty_with_args_empty",
        vmhook::detail::format_log("", 1, 2, 3).empty());
    // Embedded-NUL format STRING in the fallback is returned by size as well.
    {
        std::string fmt{ "ab" };
        fmt.push_back('\0');
        fmt += "cd";
        const std::string produced{ vmhook::detail::format_log(fmt, 1) };
        check("format_fallback_embedded_nul_fmt_size", produced.size() == 5u);
        check("format_fallback_embedded_nul_fmt_value", produced == fmt);
    }
    {
        const std::string long_literal(4096u, 'L');
        check("format_fallback_very_long_literal",
            vmhook::detail::format_log(long_literal, 1, 2) == long_literal);
    }
#endif

    // ---------------------------------------------------------------------
    // format_log return type is std::string, and a zero-argument call (just a
    // format string) is always valid and yields exactly that string.
    // ---------------------------------------------------------------------
    {
        auto produced = vmhook::detail::format_log("plain");
        check("format_returns_std_string",
            std::is_same_v<decltype(produced), std::string>);
        check("format_zero_args_is_fmt", produced == "plain");
    }

    // Empty format string yields an empty result on every toolchain.
    check("format_empty_fmt_empty_result",
        vmhook::detail::format_log("").empty());

    // format_log accepts a std::string_view fmt (its declared parameter type),
    // a const char* literal, and a std::string — all three call forms compile
    // and return the same thing for a no-field string on EVERY toolchain.
    {
        std::string_view fmt_sv{ "abc" };
        const std::string fmt_str{ "abc" };
        check("format_accepts_string_view_fmt",
            vmhook::detail::format_log(fmt_sv) == "abc");
        check("format_accepts_std_string_fmt",
            vmhook::detail::format_log(fmt_str) == "abc");
        check("format_accepts_cstring_fmt",
            vmhook::detail::format_log("abc") == "abc");
    }

    // ---------------------------------------------------------------------
    // Never-throw guarantee.  format_log is implemented with an internal
    // try/catch (NOTE: it is *not* marked `noexcept` in the header, so we do
    // not assert noexcept(...) here — that would be a false claim — we assert
    // the observable behaviour instead): a malformed-but-syntactically-valid
    // std::vformat call (here, a replacement field with no matching argument)
    // throws std::format_error internally, which format_log swallows and
    // returns the raw format string instead of propagating.
    // ---------------------------------------------------------------------
    {
        bool threw{ false };
        std::string result;
        try
        {
            // "{}" with no argument: std::vformat throws std::format_error at
            // runtime; format_log must catch it and hand back the fmt string.
            result = vmhook::detail::format_log("{}");
        }
        catch (...)
        {
            threw = true;
        }
        check("format_mismatched_field_does_not_throw", !threw);
        // On the catch path the verbatim format string is returned (this holds
        // identically on the fallback path, where it never even tries to parse).
        check("format_mismatched_returns_fmt", result == "{}");
    }
    {
        // A second malformed case: too few args for two fields.
        bool threw{ false };
        try
        {
            (void)vmhook::detail::format_log("{} {}", 1);
        }
        catch (...)
        {
            threw = true;
        }
        check("format_too_few_args_does_not_throw", !threw);
    }
    {
        // A battery of malformed inputs, each must return WITHOUT throwing on
        // BOTH the std::format and the fallback paths (the observable contract
        // that VMHOOK_LOG relies on at ~70 call sites).
        const char* malformed[]{
            "{", "}", "{}", "{ }", "{:", "{5}", "{0}", "{:d}", "{:ZZ}",
            "{} {} {}", "}{", "{{{", "}}}",
        };
        bool any_threw{ false };
        for (const char* m : malformed)
        {
            try
            {
                (void)vmhook::detail::format_log(m, 1);
            }
            catch (...)
            {
                any_threw = true;
            }
        }
        check("format_malformed_battery_never_throws", !any_threw);
    }

    // ---------------------------------------------------------------------
    // detail::emit_log_line is noexcept and must tolerate any std::string,
    // including empty and very long inputs, without crashing or throwing.
    // ---------------------------------------------------------------------
    check("emit_log_line_is_noexcept",
        noexcept(vmhook::detail::emit_log_line(std::string{})));
    {
        bool threw{ false };
        try
        {
            vmhook::detail::emit_log_line(std::string{});            // empty
        }
        catch (...) { threw = true; }
        check("emit_empty_string_ok", !threw);
    }
    {
        bool threw{ false };
        try
        {
            vmhook::detail::emit_log_line(std::string{ "a short line" });
        }
        catch (...) { threw = true; }
        check("emit_short_string_ok", !threw);
    }
    {
        bool threw{ false };
        try
        {
            const std::string long_line(64u * 1024u, 'x');          // 64 KiB
            vmhook::detail::emit_log_line(long_line);
        }
        catch (...) { threw = true; }
        check("emit_long_string_ok", !threw);
    }
    {
        bool threw{ false };
        try
        {
            // Embedded newlines / NUL bytes must not break emission.
            std::string weird{ "line1\nline2" };
            weird.push_back('\0');
            weird += "after-nul";
            vmhook::detail::emit_log_line(weird);
        }
        catch (...) { threw = true; }
        check("emit_embedded_control_chars_ok", !threw);
    }
    {
        // A string that is ONLY a NUL byte.
        bool threw{ false };
        try
        {
            std::string only_nul;
            only_nul.push_back('\0');
            vmhook::detail::emit_log_line(only_nul);
        }
        catch (...) { threw = true; }
        check("emit_only_nul_byte_ok", !threw);
    }
    {
        // CRLF / mixed line endings and tabs.
        bool threw{ false };
        try
        {
            vmhook::detail::emit_log_line(std::string{ "a\r\nb\tc\r\n" });
        }
        catch (...) { threw = true; }
        check("emit_crlf_and_tabs_ok", !threw);
    }
    {
        // Many sequential emissions in a tight loop (mutex re-entry, stream
        // flushing) must all return normally.
        bool threw{ false };
        try
        {
            for (int i{ 0 }; i < 64; ++i)
            {
                vmhook::detail::emit_log_line(std::string{ "loop line " } + std::to_string(i));
            }
        }
        catch (...) { threw = true; }
        check("emit_many_sequential_lines_ok", !threw);
    }
    {
        // A string built from the formatter, emitted — the exact two-step the
        // VMHOOK_LOG macro performs (format_log -> emit_log_line).
        bool threw{ false };
        try
        {
            vmhook::detail::emit_log_line(
                vmhook::detail::format_log("{} formatted-then-emitted", vmhook::info_tag));
        }
        catch (...) { threw = true; }
        check("emit_formatted_line_ok", !threw);
    }
    // format_log return type is std::string regardless of the gating branch, so
    // its result is always a valid emit_log_line argument (cross-branch contract).
    check("format_log_result_is_emittable_type",
          std::is_same_v<decltype(vmhook::detail::format_log("{}", 1)), std::string>);

    // ---------------------------------------------------------------------
    // emit_log_line OUTPUT observation (std::cout, rdbuf-captured).
    // The sink appends exactly one '\n' to the supplied string and writes the
    // payload verbatim — assert the captured bytes are exactly `payload + "\n"`.
    // ---------------------------------------------------------------------
    {
        std::string captured;
        {
            cout_capture cap;
            vmhook::detail::emit_log_line(std::string{ "hello-sink" });
            captured = cap.str();
        }
        check("emit_appends_single_newline",
            captured == "hello-sink\n");
    }
    {
        // Empty payload -> just the newline.
        std::string captured;
        {
            cout_capture cap;
            vmhook::detail::emit_log_line(std::string{});
            captured = cap.str();
        }
        check("emit_empty_payload_is_just_newline", captured == "\n");
    }
    {
        // The payload is written verbatim — interior newlines are preserved
        // (the sink only APPENDS one), so a 2-line message yields 3 LFs total.
        std::string captured;
        {
            cout_capture cap;
            vmhook::detail::emit_log_line(std::string{ "a\nb" });
            captured = cap.str();
        }
        check("emit_preserves_interior_newlines", captured == "a\nb\n");
    }
    {
        // Two emissions concatenate in order, each newline-terminated.
        std::string captured;
        {
            cout_capture cap;
            vmhook::detail::emit_log_line(std::string{ "first" });
            vmhook::detail::emit_log_line(std::string{ "second" });
            captured = cap.str();
        }
        check("emit_two_lines_concatenate_in_order",
            captured == "first\nsecond\n");
    }
    {
        // An embedded NUL is written through (length-preserving), then '\n'.
        std::string captured;
        {
            cout_capture cap;
            std::string payload{ "x" };
            payload.push_back('\0');
            payload += "y";
            vmhook::detail::emit_log_line(payload);
            captured = cap.str();
        }
        check("emit_embedded_nul_length_preserved",
            captured.size() == 4u && captured.back() == '\n');
    }

    // ---------------------------------------------------------------------
    // VMHOOK_LOG must compile and run without crashing in whatever mode this
    // translation unit is built (no-op when VMHOOK_DEBUG_LOGS == 0, active
    // otherwise).  Exercise it with the int/string/pointer/float arg mix and
    // with the library's own error_tag, mirroring real internal usage.
    // ---------------------------------------------------------------------
    {
        bool threw{ false };
        try
        {
            int sentinel{ 123 };
            VMHOOK_LOG("plain message");
            VMHOOK_LOG("{} int={}", vmhook::error_tag, 7);
            VMHOOK_LOG("{} str={} ptr={} flt={}",
                vmhook::warning_tag,
                std::string{ "s" },
                static_cast<void*>(&sentinel),
                2.25);
        }
        catch (...) { threw = true; }
        check("vmhook_log_macro_runs_without_throwing", !threw);
    }
    // VMHOOK_DEBUG_LOGS is always defined (to 0 or 1) by the header.
#if defined(VMHOOK_DEBUG_LOGS)
    check("vmhook_debug_logs_macro_defined",
        (VMHOOK_DEBUG_LOGS == 0) || (VMHOOK_DEBUG_LOGS == 1));
#else
    check("vmhook_debug_logs_macro_defined", false);
#endif

    // ---------------------------------------------------------------------
    // VMHOOK_LOG active vs no-op DIVERGENCE — observed through the captured
    // std::cout.  The macro splits on VMHOOK_DEBUG_LOGS:
    //   * active (==1): expands to emit_log_line(format_log(...)) -> the exact
    //     formatted line followed by '\n' is written.
    //   * no-op  (==0): expands to ((void)sizeof(...)) -> an UNEVALUATED
    //     expression, so NOTHING is written AND the argument is not evaluated
    //     at runtime (proving the sizeof-unevaluated-context contract).
    // ---------------------------------------------------------------------
    {
        std::string captured;
        {
            cout_capture cap;
            VMHOOK_LOG("{} probe={}", vmhook::info_tag, 99);
            captured = cap.str();
        }
#if VMHOOK_DEBUG_LOGS
    #if VMHOOK_HAS_STD_FORMAT
        // Active + std::format: exact formatted line + newline.
        check("vmhook_log_active_emits_exact_line",
            captured == "[VMHook INFO] probe=99\n");
    #else
        // Active + fallback formatter: the format string verbatim + newline.
        check("vmhook_log_active_emits_exact_line",
            captured == "{} probe={}\n");
    #endif
#else
        // No-op build: nothing is emitted at all.
        check("vmhook_log_noop_emits_nothing", captured.empty());
#endif
    }
    {
        // Side-effect probe: an argument with an observable side effect must be
        // EVALUATED in the active build (it is a real function-call argument)
        // and NOT evaluated in the no-op build (sizeof is an unevaluated
        // operand).  Either way the macro must compile and not throw.
        std::atomic<int> side_effect{ 0 };
        auto bump = [&side_effect]() -> int { side_effect.fetch_add(1); return 5; };
        {
            cout_capture cap;            // swallow any active-build output
            VMHOOK_LOG("{} value={}", vmhook::info_tag, bump());
            (void)cap;
        }
#if VMHOOK_DEBUG_LOGS
        check("vmhook_log_active_evaluates_args", side_effect.load() == 1);
#else
        check("vmhook_log_noop_does_not_evaluate_args", side_effect.load() == 0);
#endif
    }

    // ---------------------------------------------------------------------
    // Thread-safety / interleaving.  emit_log_line serialises writes under a
    // function-local static std::mutex, so concurrent emissions from K threads
    // of M distinct lines each must produce EXACTLY K*M intact, newline-
    // terminated lines in the captured buffer — no torn writes, no losses.
    // (std::cout's streambuf is swapped for an in-memory one for the duration;
    // the mutex inside emit_log_line is what protects the shared buffer.)
    // ---------------------------------------------------------------------
    {
        constexpr int K{ 8 };
        constexpr int M{ 50 };
        std::string captured;
        {
            cout_capture cap;
            std::vector<std::thread> threads;
            threads.reserve(K);
            for (int t{ 0 }; t < K; ++t)
            {
                threads.emplace_back([t]()
                {
                    for (int i{ 0 }; i < M; ++i)
                    {
                        // Each line is unique and contains no interior newline,
                        // so torn writes would be detectable as a malformed line.
                        vmhook::detail::emit_log_line(
                            "T" + std::to_string(t) + "#" + std::to_string(i));
                    }
                });
            }
            for (auto& th : threads) { th.join(); }
            captured = cap.str();
        }
        // Count newline-terminated lines and validate each is well-formed
        // ("T<digits>#<digits>") — a torn write would corrupt one of these.
        int line_count{ 0 };
        bool all_wellformed{ true };
        {
            std::istringstream iss{ captured };
            std::string line;
            while (std::getline(iss, line))
            {
                ++line_count;
                // Minimal structural check: starts with 'T', contains '#'.
                if (line.empty() || line.front() != 'T'
                    || line.find('#') == std::string::npos)
                {
                    all_wellformed = false;
                }
            }
        }
        check("emit_concurrent_line_count_exact", line_count == K * M);
        check("emit_concurrent_no_torn_writes", all_wellformed);
        // The captured buffer must end on a newline (every emit terminates one).
        check("emit_concurrent_ends_with_newline",
            !captured.empty() && captured.back() == '\n');
    }

    std::printf("\n%d checks failed\n", failures);
    return failures == 0 ? 0 : 1;
}
