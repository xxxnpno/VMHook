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
    // Symmetry: the warning_tag and info_tag substitute identically, including
    // alongside a trailing argument (the exact two-field idiom most call sites
    // use: VMHOOK_LOG("{} ...: {}", vmhook::X_tag, value)).
    check("format_with_warning_tag",
        vmhook::detail::format_log("{} note", vmhook::warning_tag) == "[VMHook WARNING] note");
    check("format_with_info_tag",
        vmhook::detail::format_log("{} ready", vmhook::info_tag) == "[VMHook INFO] ready");
    check("format_tag_with_trailing_arg",
        vmhook::detail::format_log("{} code={}", vmhook::info_tag, 7) == "[VMHook INFO] code=7");
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
    // bool honours width / alignment too (textual presentation, padded).
    check("format_bool_right_align_width",
        vmhook::detail::format_log("{:>6}", true) == "  true");
    check("format_bool_left_align_width",
        vmhook::detail::format_log("{:<6}", false) == "false ");

    // --- String-payload format specs (width / align / fill / precision). --
    //
    // The integer spec sweep above covers width/fill/align on NUMBERS; strings
    // exercise a DIFFERENT formatter (the std-string/string_view/const char*
    // formatter), where `precision` means "truncate to N code units" rather than
    // fractional digits.  All spellings here are standard-pinned (no shortest-
    // round-trip or locale involvement), so they are byte-identical on every STL.
    //
    // Precision on a std::string truncates to N leading bytes.
    check("format_string_precision_truncates",
        vmhook::detail::format_log("{:.3}", std::string{ "hello" }) == "hel");
    // Precision longer than the string is a no-op (no padding, no error).
    check("format_string_precision_longer_is_noop",
        vmhook::detail::format_log("{:.5}", std::string{ "hi" }) == "hi");
    // Precision of zero yields the empty string.
    check("format_string_precision_zero_empty",
        vmhook::detail::format_log("{:.0}", std::string{ "abc" }).empty());
    // Width + right alignment pads a short string on the left.
    check("format_string_width_right_align",
        vmhook::detail::format_log("{:>5}", std::string{ "ab" }) == "   ab");
    // Width + left alignment + custom fill.
    check("format_string_width_left_fill",
        vmhook::detail::format_log("{:.<6}", std::string{ "text" }) == "text..");
    // Combined custom-fill + width + precision: truncate to 3, then pad to 8
    // using '*' as the fill (left-aligned) -> "hel*****".
    check("format_string_fill_width_precision",
        vmhook::detail::format_log("{:*<8.3}", std::string{ "hello" }) == "hel*****");
    // A width smaller than the content is a no-op (content is never clipped by
    // width alone — only `precision` clips).
    check("format_string_width_smaller_is_noop",
        vmhook::detail::format_log("{:2}", std::string{ "abcdef" }) == "abcdef");
    // The same specs apply to a std::string_view payload...
    {
        std::string_view sv{ "vmhook" };
        check("format_string_view_precision",
            vmhook::detail::format_log("{:.3}", sv) == "vmh");
    }
    // ...and to a const char* payload.
    check("format_cstring_width_right_align",
        vmhook::detail::format_log("{:>5}", "ab") == "   ab");

    // --- `char` payload with a spec (distinct from int / signed char). ----
    // A plain `char` is a CHARACTER type in std::format, so width / fill / align
    // apply to its single glyph (it is NOT promoted to a number unless {:d}).
    check("format_char_width_right_align",
        vmhook::detail::format_log("{:>3}", 'A') == "  A");
    check("format_char_fill_align",
        vmhook::detail::format_log("{:->4}", 'x') == "---x");
    // An explicit {:c} on an actual char value renders the glyph unchanged.
    check("format_char_explicit_c_spec",
        vmhook::detail::format_log("{:c}", 'Z') == "Z");

    // --- Alt-form (#) on a ZERO value: the prefix rules are subtle. --------
    // Hex/binary of zero keep their "0x"/"0b" prefix, but OCTAL of zero has NO
    // "0" prefix added (alt-form octal of 0 is just "0", per the standard).
    check("format_alt_hex_zero",
        vmhook::detail::format_log("{:#x}", 0) == "0x0");
    check("format_alt_binary_zero",
        vmhook::detail::format_log("{:#b}", 0) == "0b0");
    check("format_alt_octal_zero_no_prefix",
        vmhook::detail::format_log("{:#o}", 0) == "0");

    // --- Deeper nested escaped braces around a real field. ----------------
    // Four leading '{{' collapse to two '{', the field expands, four trailing
    // '}}' collapse to two '}' -> "{{5}}".
    check("format_deep_nested_escaped_braces",
        vmhook::detail::format_log("{{{{{}}}}}", 5) == "{{5}}");

    // --- Dynamic width / precision drawn from POSITIONAL args. ------------
    // The auto-indexed dynamic forms ({:{}}, {:.{}f}) are covered above; here the
    // nested width/precision are taken from EXPLICIT positional indices, and a
    // single call combines two dynamic fields.
    check("format_dynamic_width_positional",
        vmhook::detail::format_log("{1:{0}}", 4, 7) == "   7");
    check("format_dynamic_precision_positional",
        vmhook::detail::format_log("{0:.{1}f}", 3.14159, 3) == "3.142");
    // Two dynamic fields (width AND precision) auto-indexed from later args.
    check("format_dynamic_width_and_precision",
        vmhook::detail::format_log("{:{}.{}f}", 3.14159, 8, 2) == "    3.14");
    // A dynamic width of zero is a no-op (renders the value unpadded).
    check("format_dynamic_width_zero_is_noop",
        vmhook::detail::format_log("{:{}}", 9, 0) == "9");

    // --- long double payload (a distinct arithmetic type). ---------------
    // 1.5 is exactly representable, so its shortest form is the standard-pinned
    // "1.5" on every STL; also assert the fixed-precision layout (byte-pinned).
    check("format_long_double_shortest",
        vmhook::detail::format_log("{}", 1.5L) == "1.5");
    check("format_long_double_fixed",
        vmhook::detail::format_log("{:.2f}", 1.5L) == "1.50");

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
    // Scientific with an EXPLICIT precision is byte-pinned (fixed digit count),
    // including the two-digit exponent with a sign — identical on both STLs.
    check("format_double_scientific_precision",
        vmhook::detail::format_log("{:.3e}", 0.000123) == "1.230e-04");
    check("format_double_scientific_plus_sign",
        vmhook::detail::format_log("{:+.2e}", 1.5) == "+1.50e+00");
    // The space-flag on a fixed-precision float: leading space for non-negative,
    // '-' for negative.  Standard-pinned layout.
    check("format_double_space_sign_fixed_positive",
        vmhook::detail::format_log("{: .2f}", 1.5) == " 1.50");
    check("format_double_space_sign_fixed_negative",
        vmhook::detail::format_log("{: .2f}", -1.5) == "-1.50");
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
    // Mirror the std::format-path spec sweep added above: on the fallback the
    // ENTIRE format string (specs and all) is returned byte-for-byte, regardless
    // of payload type — so a reviewer on a pre-std::format MinGW/Clang leg
    // exercises the exact same input strings and gets the verbatim guarantee.
    check("format_fallback_string_precision_verbatim",
        vmhook::detail::format_log("{:.3}", std::string{ "hello" }) == "{:.3}");
    check("format_fallback_string_width_verbatim",
        vmhook::detail::format_log("{:>5}", std::string{ "ab" }) == "{:>5}");
    check("format_fallback_string_fill_width_prec_verbatim",
        vmhook::detail::format_log("{:*<8.3}", std::string{ "hello" }) == "{:*<8.3}");
    check("format_fallback_char_width_verbatim",
        vmhook::detail::format_log("{:>3}", 'A') == "{:>3}");
    check("format_fallback_char_c_spec_verbatim",
        vmhook::detail::format_log("{:c}", 'Z') == "{:c}");
    check("format_fallback_bool_width_verbatim",
        vmhook::detail::format_log("{:>6}", true) == "{:>6}");
    check("format_fallback_alt_hex_zero_verbatim",
        vmhook::detail::format_log("{:#x}", 0) == "{:#x}");
    check("format_fallback_alt_octal_zero_verbatim",
        vmhook::detail::format_log("{:#o}", 0) == "{:#o}");
    check("format_fallback_deep_nested_braces_verbatim",
        vmhook::detail::format_log("{{{{{}}}}}", 5) == "{{{{{}}}}}");
    check("format_fallback_dynamic_positional_verbatim",
        vmhook::detail::format_log("{1:{0}}", 4, 7) == "{1:{0}}");
    check("format_fallback_dynamic_width_and_prec_verbatim",
        vmhook::detail::format_log("{:{}.{}f}", 3.14159, 8, 2) == "{:{}.{}f}");
    check("format_fallback_long_double_verbatim",
        vmhook::detail::format_log("{}", 1.5L) == "{}");
    check("format_fallback_scientific_precision_verbatim",
        vmhook::detail::format_log("{:.3e}", 0.000123) == "{:.3e}");
    check("format_fallback_warning_tag_verbatim",
        vmhook::detail::format_log("{} note", vmhook::warning_tag) == "{} note");
    check("format_fallback_info_tag_verbatim",
        vmhook::detail::format_log("{} ready", vmhook::info_tag) == "{} ready");
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
    {
        // Characterize the sink's NO-normalization contract: it appends exactly
        // one '\n' UNCONDITIONALLY, so a payload that ALREADY ends in '\n' yields
        // a trailing BLANK line ("x\n" + "\n").  (This is by design — the sink
        // does not de-duplicate or trim — documented here so a future change to
        // that behaviour trips a test rather than silently shipping.)
        std::string captured;
        {
            cout_capture cap;
            vmhook::detail::emit_log_line(std::string{ "x\n" });
            captured = cap.str();
        }
        check("emit_trailing_newline_not_deduplicated", captured == "x\n\n");
    }
    {
        // A payload that IS one of the library tags passes through the sink
        // verbatim with a single appended newline — the exact shape of a real
        // (argument-less) VMHOOK_LOG(vmhook::error_tag) emission.
        std::string captured;
        {
            cout_capture cap;
            vmhook::detail::emit_log_line(std::string{ vmhook::error_tag });
            captured = cap.str();
        }
        check("emit_tag_payload_verbatim_plus_newline",
            captured == "[VMHook ERROR]\n");
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

    // =====================================================================
    // DEEPENING WAVE (additive) — exhaustive bit-pattern / sign-boundary /
    // spec-edge sweeps for detail::format_log, plus argument-forwarding and
    // sink-byte invariants not yet covered above.  Every expected value is
    // derived directly from the library source:
    //   * format_log (VMHOOK_HAS_STD_FORMAT==1) == std::vformat(fmt, args)  ->
    //     spellings here are all STANDARD-PINNED (no shortest-round-trip, no
    //     locale, no implementation-defined NaN/hexfloat token), so they are
    //     byte-identical on libstdc++ AND the MSVC STL.
    //   * format_log (VMHOOK_HAS_STD_FORMAT==0) == std::string{ fmt }        ->
    //     the verbatim fallback, asserted byte-for-byte for the same inputs.
    // This section is namespaced by a distinct check-name prefix ("dw_") so it
    // does not collide with any assertion above.
    // =====================================================================
#if VMHOOK_HAS_STD_FORMAT
    // --- Signed-integer sign boundaries under a BASE spec. ----------------
    // std::format renders a signed negative as '-' + magnitude-in-base (NOT
    // printf two's-complement).  INT_MIN magnitude is 0x80000000.
    check("dw_int_min_hex_signed_magnitude",
        vmhook::detail::format_log("{:x}", INT_MIN) == "-80000000");
    check("dw_int_max_hex",
        vmhook::detail::format_log("{:x}", INT_MAX) == "7fffffff");
    check("dw_int_min_octal_signed",
        vmhook::detail::format_log("{:o}", INT_MIN) == "-20000000000");
    check("dw_int_min_binary_signed",
        vmhook::detail::format_log("{:b}", INT_MIN)
            == "-10000000000000000000000000000000");
    // 0x7F / 0x80 sign boundary at int8 width: 0x7F == 127, (signed char)0x80
    // == -128, (unsigned char)0x80 == 128.
    check("dw_int8_0x7F_is_127",
        vmhook::detail::format_log("{}", static_cast<std::int8_t>(0x7F)) == "127");
    check("dw_int8_0x80_is_minus_128",
        vmhook::detail::format_log("{}", static_cast<std::int8_t>(
            static_cast<std::int8_t>(-128))) == "-128");
    check("dw_uint8_0x7F_is_127",
        vmhook::detail::format_log("{}", static_cast<std::uint8_t>(0x7F)) == "127");
    check("dw_uint8_0x80_is_128",
        vmhook::detail::format_log("{}", static_cast<std::uint8_t>(0x80)) == "128");
    // 0x7FFF / 0x8000 sign boundary at int16 width.
    check("dw_int16_0x7FFF_is_32767",
        vmhook::detail::format_log("{}", static_cast<std::int16_t>(0x7FFF)) == "32767");
    check("dw_uint16_0x8000_is_32768",
        vmhook::detail::format_log("{}", static_cast<std::uint16_t>(0x8000)) == "32768");
    // 0x7FFFFFFF / 0x80000000 at uint32 width (unsigned -> no sign char).
    check("dw_uint32_0x80000000_is_2147483648",
        vmhook::detail::format_log("{}", static_cast<std::uint32_t>(0x80000000u))
            == "2147483648");
    check("dw_uint32_0x7FFFFFFF_is_2147483647",
        vmhook::detail::format_log("{}", static_cast<std::uint32_t>(0x7FFFFFFFu))
            == "2147483647");
    // 0x8000000000000000 at uint64 width.
    check("dw_uint64_high_bit_is_9223372036854775808",
        vmhook::detail::format_log("{}",
            static_cast<std::uint64_t>(0x8000000000000000ull))
            == "9223372036854775808");
    // -1 across signed widths: always "-1" by default (decimal magnitude).
    check("dw_int8_minus1",
        vmhook::detail::format_log("{}", static_cast<std::int8_t>(-1)) == "-1");
    check("dw_int16_minus1",
        vmhook::detail::format_log("{}", static_cast<std::int16_t>(-1)) == "-1");
    check("dw_int64_minus1",
        vmhook::detail::format_log("{}", static_cast<std::int64_t>(-1)) == "-1");
    // -1 as an UNSIGNED width is the all-ones value (no sign).
    check("dw_uint16_all_ones",
        vmhook::detail::format_log("{}", static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(-1))) == "65535");
    // +1 across widths.
    check("dw_int_plus1",
        vmhook::detail::format_log("{}", 1) == "1");

    // --- Center / left / right alignment padding-split exactness. ---------
    // Even padding splits evenly; ODD padding floors the LEFT side (pad 3 on a
    // 2-char string in width 5 -> 1 left, 2 right).
    check("dw_center_even_pad",
        vmhook::detail::format_log("{:^6}", std::string{ "ab" }) == "  ab  ");
    check("dw_center_odd_pad_floor_left",
        vmhook::detail::format_log("{:^5}", std::string{ "ab" }) == " ab  ");
    check("dw_center_pad_two",
        vmhook::detail::format_log("{:^4}", std::string{ "ab" }) == " ab ");
    // A large width emits exactly (width - len) fill chars.
    check("dw_large_right_align_width",
        vmhook::detail::format_log("{:>20}", 7)
            == "                   7");          // 19 spaces + '7'
    check("dw_large_left_align_width",
        vmhook::detail::format_log("{:<20}", 7)
            == "7                   ");          // '7' + 19 spaces

    // --- Alt-form + zero-pad width COUNTING (prefix occupies columns). ----
    // {:#010x} on 255: "0x" (2) + zero-fill + "ff" (2) == width 10 -> 6 zeros.
    check("dw_alt_hex_zeropad_width_counts_prefix",
        vmhook::detail::format_log("{:#010x}", 255) == "0x000000ff");
    // {:08b} on 5: "101" zero-padded to width 8.
    check("dw_binary_zeropad8",
        vmhook::detail::format_log("{:08b}", 5) == "00000101");
    // {:#06b} on 1: "0b" (2) + zero-fill + "1" -> width 6 -> 3 zeros.
    check("dw_alt_binary_zeropad_prefix_counts",
        vmhook::detail::format_log("{:#06b}", 1) == "0b0001");

    // --- char value boundaries (glyph vs integer code). -------------------
    // A char with value 0 formats as integer 0 under {:d}.
    check("dw_char_nul_as_int",
        vmhook::detail::format_log("{:d}", '\x00') == "0");
    // The digit glyph '0' is code 48: glyph by default, 48 under {:d}.
    check("dw_char_zero_glyph",
        vmhook::detail::format_log("{}", '0') == "0");
    check("dw_char_zero_as_int",
        vmhook::detail::format_log("{:d}", '0') == "48");
    // {:c} on code 32 yields a single space.
    check("dw_int_as_char_space",
        vmhook::detail::format_log("{:c}", 32) == " ");
    // {:c} on code 126 yields '~' (last printable ASCII).
    check("dw_int_as_char_tilde",
        vmhook::detail::format_log("{:c}", 126) == "~");

    // --- Scientific with zero precision (no half-rounding ambiguity). -----
    check("dw_scientific_zero_precision",
        vmhook::detail::format_log("{:.0e}", 1.0) == "1e+00");
    check("dw_scientific_zero_precision_plus",
        vmhook::detail::format_log("{:+.0e}", 1.0) == "+1e+00");
    // Fixed with zero precision on an exact integer-valued double.
    check("dw_fixed_zero_precision_exact",
        vmhook::detail::format_log("{:.0f}", 3.0) == "3");

    // --- Argument forwarding: rvalue and lvalue produce identical output. -
    // format_log takes args_t&&...; make_format_args binds named lvalues.  An
    // rvalue temporary and a named lvalue of the same value must format the
    // same — guards the documented refactor hazard around the && forwarding.
    {
        const int lv{ 5 };
        const std::string lv_s{ "fwd" };
        check("dw_forward_rvalue_eq_lvalue_int",
            vmhook::detail::format_log("{}", 5)
                == vmhook::detail::format_log("{}", lv));
        check("dw_forward_rvalue_eq_lvalue_string",
            vmhook::detail::format_log("{}", std::string{ "fwd" })
                == vmhook::detail::format_log("{}", lv_s));
        check("dw_forward_lvalue_int_value", vmhook::detail::format_log("{}", lv) == "5");
        check("dw_forward_lvalue_string_value",
            vmhook::detail::format_log("{}", lv_s) == "fwd");
        // A const lvalue (the common call-site shape: a const std::string&)
        // also binds and formats correctly.
        const std::string& cref{ lv_s };
        check("dw_forward_const_ref_string",
            vmhook::detail::format_log("[{}]", cref) == "[fwd]");
    }

    // --- Two-field tag idiom with EACH boundary integer (real call shape). -
    // The ~70 internal call sites are "{} ...: {}", tag, value.  Exercise that
    // exact two-field shape across the integer boundaries with each tag.
    check("dw_tag_idiom_error_int_min",
        vmhook::detail::format_log("{} v={}", vmhook::error_tag, INT_MIN)
            == "[VMHook ERROR] v=-2147483648");
    check("dw_tag_idiom_warning_ullong_max",
        vmhook::detail::format_log("{} v={}", vmhook::warning_tag, ULLONG_MAX)
            == "[VMHook WARNING] v=18446744073709551615");
    check("dw_tag_idiom_info_hex",
        vmhook::detail::format_log("{} addr=0x{:08X}", vmhook::info_tag, 0xBEEFu)
            == "[VMHook INFO] addr=0x0000BEEF");

    // --- Width applied to a NEGATIVE number: sign char counts toward width. -
    // {:5} on -7: '-' + '7' is 2 chars, right-aligned (default for numbers) in
    // width 5 -> 3 leading spaces.
    check("dw_negative_width_right_default",
        vmhook::detail::format_log("{:5}", -7) == "   -7");
    // {:05} on -7: zero-pad places the sign FIRST, then zeros -> "-0007".
    check("dw_negative_zeropad_sign_first",
        vmhook::detail::format_log("{:05}", -7) == "-0007");
    // {:+06} on 42: '+' first, then zero-fill to width 6 -> "+00042".
    check("dw_positive_plus_zeropad",
        vmhook::detail::format_log("{:+06}", 42) == "+00042");

    // --- size_t / ptrdiff_t boundaries (logged at several call sites). -----
    check("dw_size_t_zero",
        vmhook::detail::format_log("{}", static_cast<std::size_t>(0)) == "0");
    check("dw_size_t_max",
        vmhook::detail::format_log("{}", (std::numeric_limits<std::size_t>::max)())
            == (sizeof(std::size_t) == 8
                ? "18446744073709551615"
                : "4294967295"));
    check("dw_ptrdiff_min",
        vmhook::detail::format_log("{}", (std::numeric_limits<std::ptrdiff_t>::min)())
            == (sizeof(std::ptrdiff_t) == 8
                ? "-9223372036854775808"
                : "-2147483648"));
#else
    // Fallback leg: every input above returns the format string verbatim,
    // ignoring all arguments and specifiers.  Mirror the spec-bearing strings
    // so the pre-std::format MinGW/Clang CI legs exercise the same surface.
    check("dw_fb_int_min_hex_verbatim",
        vmhook::detail::format_log("{:x}", INT_MIN) == "{:x}");
    check("dw_fb_int_min_octal_verbatim",
        vmhook::detail::format_log("{:o}", INT_MIN) == "{:o}");
    check("dw_fb_int_min_binary_verbatim",
        vmhook::detail::format_log("{:b}", INT_MIN) == "{:b}");
    check("dw_fb_center_verbatim",
        vmhook::detail::format_log("{:^5}", std::string{ "ab" }) == "{:^5}");
    check("dw_fb_large_width_verbatim",
        vmhook::detail::format_log("{:>20}", 7) == "{:>20}");
    check("dw_fb_alt_hex_zeropad_verbatim",
        vmhook::detail::format_log("{:#010x}", 255) == "{:#010x}");
    check("dw_fb_binary_zeropad8_verbatim",
        vmhook::detail::format_log("{:08b}", 5) == "{:08b}");
    check("dw_fb_char_as_int_verbatim",
        vmhook::detail::format_log("{:d}", '\x00') == "{:d}");
    check("dw_fb_int_as_char_verbatim",
        vmhook::detail::format_log("{:c}", 32) == "{:c}");
    check("dw_fb_scientific_zero_prec_verbatim",
        vmhook::detail::format_log("{:.0e}", 1.0) == "{:.0e}");
    // Forwarding: even in the fallback, rvalue and lvalue calls yield the same
    // verbatim format string (arguments never appear).
    {
        const int lv{ 5 };
        check("dw_fb_forward_rvalue_eq_lvalue",
            vmhook::detail::format_log("{}", 5)
                == vmhook::detail::format_log("{}", lv));
        check("dw_fb_forward_value_is_fmt",
            vmhook::detail::format_log("{}", lv) == "{}");
    }
    check("dw_fb_tag_idiom_verbatim",
        vmhook::detail::format_log("{} v={}", vmhook::error_tag, INT_MIN) == "{} v={}");
    check("dw_fb_negative_zeropad_verbatim",
        vmhook::detail::format_log("{:05}", -7) == "{:05}");
    check("dw_fb_positive_plus_zeropad_verbatim",
        vmhook::detail::format_log("{:+06}", 42) == "{:+06}");
    check("dw_fb_size_t_max_verbatim",
        vmhook::detail::format_log("{}",
            (std::numeric_limits<std::size_t>::max)()) == "{}");
#endif

    // --- Cross-branch (toolchain-independent) sink + emit invariants. ------
    // A payload containing ONLY a carriage return is written through verbatim
    // with the appended '\n' (sink does no CR/LF normalization on cout).
    {
        std::string captured;
        {
            cout_capture cap;
            vmhook::detail::emit_log_line(std::string{ "a\rb" });
            captured = cap.str();
        }
        check("dw_emit_carriage_return_verbatim", captured == "a\rb\n");
    }
    // A payload that already contains the appended-newline shape: emit appends
    // exactly one '\n', so "x\ny" -> "x\ny\n" (3 LF-delimited segments).
    {
        std::string captured;
        {
            cout_capture cap;
            vmhook::detail::emit_log_line(std::string{ "x\ny" });
            captured = cap.str();
        }
        check("dw_emit_interior_newline_then_appended",
            captured == "x\ny\n");
    }
    // A long line (one byte under and at a power-of-two) emits byte-for-byte
    // plus the single trailing '\n'.
    {
        std::string captured;
        const std::string payload(1023u, 'Z');
        {
            cout_capture cap;
            vmhook::detail::emit_log_line(payload);
            captured = cap.str();
        }
        check("dw_emit_1023_byte_line_size",
            captured.size() == payload.size() + 1u);
        check("dw_emit_1023_byte_line_terminated",
            !captured.empty() && captured.back() == '\n');
        check("dw_emit_1023_byte_line_body",
            captured.compare(0, payload.size(), payload) == 0);
    }
    // The full VMHOOK_LOG two-step (format_log -> emit_log_line) reproduced by
    // hand through the captured sink, asserting the EXACT emitted bytes on the
    // std::format leg and the verbatim-fmt leg respectively.
    {
        std::string captured;
        {
            cout_capture cap;
            vmhook::detail::emit_log_line(
                vmhook::detail::format_log("{} step={}", vmhook::warning_tag, 3));
            captured = cap.str();
        }
#if VMHOOK_HAS_STD_FORMAT
        check("dw_format_then_emit_exact_bytes",
            captured == "[VMHook WARNING] step=3\n");
#else
        check("dw_format_then_emit_exact_bytes",
            captured == "{} step={}\n");
#endif
    }

    // =====================================================================
    // DEEPENING WAVE 2 (additive) — a SECOND namespaced section ("dw2_")
    // covering format_log / emit_log_line inputs the first wave ("dw_") did
    // NOT touch: the 64-bit signed base boundaries, bool under base specs,
    // the '-' sign flag, dynamic alignment/width/precision drawn together,
    // '{:g}' trailing-zero trimming, deeper string-precision/width
    // interactions, additional {:c} glyph boundaries, multi-tag composition,
    // and further sink-byte invariants.  PURE LOGIC ONLY — no memory reads,
    // no pointer fabrication, no value_t casts.  Every expected value is
    // derived directly from the source contract:
    //   * VMHOOK_HAS_STD_FORMAT==1  -> format_log == std::vformat(fmt, args);
    //     every spelling chosen here is STANDARD-PINNED (no shortest-round-
    //     trip, no locale, no impl-defined NaN/hexfloat token), hence
    //     byte-identical on libstdc++ AND the MSVC STL.
    //   * VMHOOK_HAS_STD_FORMAT==0  -> format_log == std::string{ fmt }, the
    //     verbatim fallback, asserted byte-for-byte for the same inputs.
    // =====================================================================
#if VMHOOK_HAS_STD_FORMAT
    // --- 64-bit signed base boundaries (signed magnitude, not two's-comp). -
    // LLONG_MIN magnitude is 0x8000000000000000; std::format prints '-' + the
    // base-N magnitude.
    check("dw2_llong_min_hex_signed",
        vmhook::detail::format_log("{:x}", LLONG_MIN) == "-8000000000000000");
    check("dw2_llong_max_hex",
        vmhook::detail::format_log("{:x}", LLONG_MAX) == "7fffffffffffffff");
    check("dw2_llong_min_octal_signed",
        vmhook::detail::format_log("{:o}", LLONG_MIN) == "-1000000000000000000000");
    check("dw2_llong_max_octal",
        vmhook::detail::format_log("{:o}", LLONG_MAX) == "777777777777777777777");
    check("dw2_ullong_max_hex",
        vmhook::detail::format_log("{:x}", ULLONG_MAX) == "ffffffffffffffff");
    check("dw2_ullong_max_octal",
        vmhook::detail::format_log("{:o}", ULLONG_MAX) == "1777777777777777777777");
    // 64-bit binary of LLONG_MAX is 63 set bits; alt-form upper-hex of ULLONG.
    check("dw2_llong_max_binary",
        vmhook::detail::format_log("{:b}", LLONG_MAX)
            == "111111111111111111111111111111111111111111111111111111111111111");
    check("dw2_ullong_max_hex_alt_upper",
        vmhook::detail::format_log("{:#X}", ULLONG_MAX) == "0XFFFFFFFFFFFFFFFF");

    // --- The '-' sign flag (only negatives get a sign; default behaviour). -
    // '{:-}' is the explicit spelling of the default: no '+' on non-negatives.
    check("dw2_minus_flag_positive_no_sign",
        vmhook::detail::format_log("{:-}", 5) == "5");
    check("dw2_minus_flag_negative_has_sign",
        vmhook::detail::format_log("{:-}", -5) == "-5");
    check("dw2_minus_flag_zero_no_sign",
        vmhook::detail::format_log("{:-}", 0) == "0");

    // --- bool under integer base / sign presentation types. ---------------
    // bool accepts integer presentation: true==1, false==0 across bases.
    check("dw2_bool_true_hex",
        vmhook::detail::format_log("{:x}", true) == "1");
    check("dw2_bool_false_hex",
        vmhook::detail::format_log("{:x}", false) == "0");
    check("dw2_bool_true_binary",
        vmhook::detail::format_log("{:b}", true) == "1");
    check("dw2_bool_true_octal",
        vmhook::detail::format_log("{:o}", true) == "1");
    check("dw2_bool_true_alt_hex",
        vmhook::detail::format_log("{:#x}", true) == "0x1");
    check("dw2_bool_true_plus_dec",
        vmhook::detail::format_log("{:+d}", true) == "+1");

    // --- Dynamic alignment + width + precision combinations. --------------
    // Right-align (default for non-strings) into a dynamic width.
    check("dw2_dynamic_width_right_default",
        vmhook::detail::format_log("{:{}}", 7, 5) == "    7");
    // Explicit right-align glyph with a dynamic width and custom fill.
    check("dw2_dynamic_fill_right_width",
        vmhook::detail::format_log("{:0>{}}", 7, 5) == "00007");
    // Left-align glyph with a dynamic width on a string.
    check("dw2_dynamic_left_width_string",
        vmhook::detail::format_log("{:<{}}", std::string{ "ab" }, 5) == "ab   ");
    // Dynamic width on a STRING with a dynamic precision (truncate then pad):
    // precision 3 truncates "hello" -> "hel", width 6 pads right -> "hel   ".
    check("dw2_dynamic_string_width_and_precision",
        vmhook::detail::format_log("{:{}.{}}", std::string{ "hello" }, 6, 3)
            == "hel   ");
    // Dynamic precision alone on a fixed float (auto-indexed).
    check("dw2_dynamic_fixed_precision_only",
        vmhook::detail::format_log("{:.{}f}", 2.0, 4) == "2.0000");

    // --- {:g} trailing-zero trimming (general format removes them). -------
    // 'g' strips trailing zeros, so 1.0 -> "1", 2.50 -> "2.5".
    check("dw2_general_trims_integer",
        vmhook::detail::format_log("{:g}", 1.0) == "1");
    check("dw2_general_trims_trailing_zero",
        vmhook::detail::format_log("{:g}", 2.5) == "2.5");
    // Fixed format KEEPS trailing zeros at the requested precision.
    check("dw2_fixed_keeps_trailing_zeros",
        vmhook::detail::format_log("{:.4f}", 2.5) == "2.5000");
    // High fixed precision on an exactly representable value is byte-pinned.
    check("dw2_fixed_precision_10_exact",
        vmhook::detail::format_log("{:.10f}", 0.5) == "0.5000000000");
    // Fixed precision on a negative exact value.
    check("dw2_fixed_precision_negative_exact",
        vmhook::detail::format_log("{:.3f}", -0.25) == "-0.250");

    // --- More {:c} glyph boundaries (int code -> single character). -------
    check("dw2_int_as_char_A",
        vmhook::detail::format_log("{:c}", 65) == "A");
    check("dw2_int_as_char_zero_digit",
        vmhook::detail::format_log("{:c}", 48) == "0");
    check("dw2_int_as_char_backtick",
        vmhook::detail::format_log("{:c}", 96) == "`");
    // {:c} honours width + fill on the produced glyph.
    check("dw2_int_as_char_width_fill",
        vmhook::detail::format_log("{:*>4c}", 65) == "***A");

    // --- Multi-tag composition in one message (e.g. tag + nested context). -
    // Two distinct tags substituted into one format string keep their bytes.
    check("dw2_two_tags_in_one_message",
        vmhook::detail::format_log("{} then {}", vmhook::error_tag, vmhook::info_tag)
            == "[VMHook ERROR] then [VMHook INFO]");
    // A tag re-used via a positional index appears twice.
    check("dw2_repeated_tag_positional",
        vmhook::detail::format_log("{0} / {0}", vmhook::warning_tag)
            == "[VMHook WARNING] / [VMHook WARNING]");
    // Tag right-aligned into a width wider than itself: the bracketed token is
    // padded on the left (error_tag is 14 chars, width 16 -> 2 leading spaces).
    check("dw2_tag_right_aligned_width",
        vmhook::detail::format_log("{:>16}", vmhook::error_tag)
            == "  [VMHook ERROR]");

    // --- ASCII string precision interacting with width (byte-pinned). ------
    // For pure-ASCII payloads the code-unit count equals the display width on
    // every STL, so precision/width spellings are byte-identical (the UTF-8
    // width-estimation divergence between libstdc++ and the MSVC STL is avoided
    // by staying ASCII).  Precision 4 truncates "abcdef" -> "abcd", then width
    // 7 right-aligns it with 3 leading spaces.
    check("dw2_ascii_precision_then_width_right",
        vmhook::detail::format_log("{:>7.4}", std::string{ "abcdef" })
            == "   abcd");
    // Precision 4 truncates, width 7 LEFT-aligns with a custom fill.
    check("dw2_ascii_precision_then_width_left_fill",
        vmhook::detail::format_log("{:-<7.4}", std::string{ "abcdef" })
            == "abcd---");
    // Precision equal to the length is a no-op; width still pads.
    check("dw2_ascii_precision_eq_len_width_pads",
        vmhook::detail::format_log("{:>5.3}", std::string{ "abc" })
            == "  abc");

    // --- Empty format string with a TAG argument still drops it (no field). -
    check("dw2_empty_fmt_with_tag_empty",
        vmhook::detail::format_log("", vmhook::error_tag).empty());

    // --- Fixed-width hex on each pointer-sized boundary value (as integer). -
    // These are the compressed-OOP / address idioms ({:016x}) on the 64-bit
    // word boundaries; integer inputs only, no real address is dereferenced.
    check("dw2_hex16_zero",
        vmhook::detail::format_log("{:016x}", static_cast<std::uint64_t>(0))
            == "0000000000000000");
    check("dw2_hex16_one",
        vmhook::detail::format_log("{:016x}", static_cast<std::uint64_t>(1))
            == "0000000000000001");
    check("dw2_hex16_high_nibble",
        vmhook::detail::format_log("{:016x}",
            static_cast<std::uint64_t>(0xF000000000000000ull))
            == "f000000000000000");

    // --- Sign flag interaction with alt-form and base on a tiny value. ----
    // {:+#x} on 1 -> '+' then "0x" then "1".
    check("dw2_plus_altform_hex_one",
        vmhook::detail::format_log("{:+#x}", 1) == "+0x1");
    // {: #o} on 8 -> space sign then alt-octal "010".
    check("dw2_space_altform_octal_eight",
        vmhook::detail::format_log("{: #o}", 8) == " 010");
#else
    // Fallback leg: every dw2_ input above returns the format string verbatim,
    // ignoring all arguments and specifiers.  Mirror the spec-bearing strings
    // so the pre-std::format MinGW/Clang CI legs exercise the same surface.
    check("dw2_fb_llong_min_hex_verbatim",
        vmhook::detail::format_log("{:x}", LLONG_MIN) == "{:x}");
    check("dw2_fb_ullong_max_octal_verbatim",
        vmhook::detail::format_log("{:o}", ULLONG_MAX) == "{:o}");
    check("dw2_fb_minus_flag_verbatim",
        vmhook::detail::format_log("{:-}", -5) == "{:-}");
    check("dw2_fb_bool_hex_verbatim",
        vmhook::detail::format_log("{:x}", true) == "{:x}");
    check("dw2_fb_dynamic_width_right_verbatim",
        vmhook::detail::format_log("{:{}}", 7, 5) == "{:{}}");
    check("dw2_fb_dynamic_string_wp_verbatim",
        vmhook::detail::format_log("{:{}.{}}", std::string{ "hello" }, 6, 3)
            == "{:{}.{}}");
    check("dw2_fb_general_trims_verbatim",
        vmhook::detail::format_log("{:g}", 1.0) == "{:g}");
    check("dw2_fb_fixed_prec10_verbatim",
        vmhook::detail::format_log("{:.10f}", 0.5) == "{:.10f}");
    check("dw2_fb_int_as_char_verbatim",
        vmhook::detail::format_log("{:c}", 65) == "{:c}");
    check("dw2_fb_int_as_char_width_verbatim",
        vmhook::detail::format_log("{:*>4c}", 65) == "{:*>4c}");
    check("dw2_fb_two_tags_verbatim",
        vmhook::detail::format_log("{} then {}", vmhook::error_tag, vmhook::info_tag)
            == "{} then {}");
    check("dw2_fb_tag_width_verbatim",
        vmhook::detail::format_log("{:>16}", vmhook::error_tag) == "{:>16}");
    check("dw2_fb_ascii_precision_width_verbatim",
        vmhook::detail::format_log("{:>7.4}", std::string{ "abcdef" }) == "{:>7.4}");
    check("dw2_fb_empty_fmt_with_tag_empty",
        vmhook::detail::format_log("", vmhook::error_tag).empty());
    check("dw2_fb_hex16_verbatim",
        vmhook::detail::format_log("{:016x}", static_cast<std::uint64_t>(1))
            == "{:016x}");
    check("dw2_fb_plus_altform_hex_verbatim",
        vmhook::detail::format_log("{:+#x}", 1) == "{:+#x}");
    check("dw2_fb_space_altform_octal_verbatim",
        vmhook::detail::format_log("{: #o}", 8) == "{: #o}");
#endif

    // --- Cross-branch sink-byte invariants not covered above (dw2_). -------
    // A payload of multiple interior newlines: emit appends exactly one '\n',
    // so "a\nb\nc" -> "a\nb\nc\n" (4 LF-delimited segments, last is empty-term).
    {
        std::string captured;
        {
            cout_capture cap;
            vmhook::detail::emit_log_line(std::string{ "a\nb\nc" });
            captured = cap.str();
        }
        check("dw2_emit_multi_interior_newlines", captured == "a\nb\nc\n");
    }
    // A payload that is exactly a single newline -> emit yields two newlines.
    {
        std::string captured;
        {
            cout_capture cap;
            vmhook::detail::emit_log_line(std::string{ "\n" });
            captured = cap.str();
        }
        check("dw2_emit_lone_newline_doubles", captured == "\n\n");
    }
    // A tab-only payload passes through verbatim plus the appended newline.
    {
        std::string captured;
        {
            cout_capture cap;
            vmhook::detail::emit_log_line(std::string{ "\t" });
            captured = cap.str();
        }
        check("dw2_emit_tab_verbatim", captured == "\t\n");
    }
    // Three sequential emissions concatenate in strict order, each terminated.
    {
        std::string captured;
        {
            cout_capture cap;
            vmhook::detail::emit_log_line(std::string{ "one" });
            vmhook::detail::emit_log_line(std::string{ "two" });
            vmhook::detail::emit_log_line(std::string{ "three" });
            captured = cap.str();
        }
        check("dw2_emit_three_lines_in_order",
            captured == "one\ntwo\nthree\n");
    }
    // A payload that is itself a full bracketed tag with a trailing field,
    // pre-formatted by hand, is emitted verbatim + newline (the exact shape of
    // a real VMHOOK_LOG("{} ...", tag) emission, asserted through the sink).
    {
        std::string captured;
        {
            cout_capture cap;
            vmhook::detail::emit_log_line(std::string{ "[VMHook INFO] ok" });
            captured = cap.str();
        }
        check("dw2_emit_preformatted_tag_line",
            captured == "[VMHook INFO] ok\n");
    }
    // emit_log_line on the result of a TAG-only format_log call: cross-branch,
    // the tag substitutes (std::format leg) or is dropped (fallback leg).  Both
    // legs append exactly one '\n'; assert the leg-specific exact bytes.
    {
        std::string captured;
        {
            cout_capture cap;
            vmhook::detail::emit_log_line(
                vmhook::detail::format_log("{}", vmhook::error_tag));
            captured = cap.str();
        }
#if VMHOOK_HAS_STD_FORMAT
        check("dw2_emit_tag_format_then_emit",
            captured == "[VMHook ERROR]\n");
#else
        check("dw2_emit_tag_format_then_emit",
            captured == "{}\n");
#endif
    }

    // =====================================================================
    // DEEPENING WAVE 3 (additive) — a THIRD namespaced section ("dw3_")
    // covering inputs the first two waves did NOT touch:
    //   * tag *.size() / exact-length invariants derived from the source
    //     literals (error/warning/info_tag at vmhook.hpp:408-410),
    //   * std::string_view ARGUMENT (not fmt) carrying width/precision specs,
    //   * alt-form (#) sign-placement on NEGATIVE base values,
    //   * sign flags ('+'/' ') on floating ZERO / negative-zero,
    //   * {:c} on the remaining control / boundary code points (tab 9, DEL 127),
    //   * '0' fill that is NOT the zero-pad form (explicit-align '0' fill),
    //   * empty / multi-NUL emit payloads and a third concurrent shape,
    //   * the format_log result element-type (remove_cvref_t -> char string),
    //   * the no-op/active VMHOOK_LOG seam via a second arg-evaluation probe.
    // PURE LOGIC ONLY — no memory reads, no pointer fabrication, no value_t
    // casts, no narrowing.  Every expected value is derived from source:
    //   * VMHOOK_HAS_STD_FORMAT==1 -> format_log == std::vformat(fmt, args);
    //     all spellings here are STANDARD-PINNED (no shortest-round-trip, no
    //     locale, no impl-defined NaN/hexfloat token), byte-identical on
    //     libstdc++ AND the MSVC STL.
    //   * VMHOOK_HAS_STD_FORMAT==0 -> format_log == std::string{ fmt }.
    // =====================================================================

    // --- Tag exact sizes (derived from the literals at vmhook.hpp:408-410). -
    // "[VMHook ERROR]" = 14 bytes, "[VMHook WARNING]" = 16, "[VMHook INFO]" = 13.
    check("dw3_error_tag_size_14", vmhook::error_tag.size() == 14u);
    check("dw3_warning_tag_size_16", vmhook::warning_tag.size() == 16u);
    check("dw3_info_tag_size_13", vmhook::info_tag.size() == 13u);
    // The bracketed body (strip '[' and ']') begins with the brand "VMHook ".
    check("dw3_error_tag_body_after_bracket",
        vmhook::error_tag.substr(1, 7) == "VMHook ");
    check("dw3_warning_tag_body_after_bracket",
        vmhook::warning_tag.substr(1, 7) == "VMHook ");
    check("dw3_info_tag_body_after_bracket",
        vmhook::info_tag.substr(1, 7) == "VMHook ");
    // The severity word is exactly what the literal spells.
    check("dw3_error_tag_severity_word",
        vmhook::error_tag.substr(8, 5) == "ERROR");
    check("dw3_warning_tag_severity_word",
        vmhook::warning_tag.substr(8, 7) == "WARNING");
    check("dw3_info_tag_severity_word",
        vmhook::info_tag.substr(8, 4) == "INFO");
    // Tags are constexpr string_views whose element type is `char` (so they
    // compose with format_log's std::string output).  remove_cvref_t strips the
    // reference/const that decltype(view[i]) would otherwise carry.
    {
        using elem_t =
            std::remove_cvref_t<decltype(vmhook::error_tag[0])>;
        check("dw3_tag_element_type_is_char",
            std::is_same_v<elem_t, char>);
    }

#if VMHOOK_HAS_STD_FORMAT
    // --- std::string_view as the ARGUMENT (not the fmt) with specs. --------
    // The sv-formatter honours precision (truncate to N code units) and width,
    // exactly like std::string/const char*.
    {
        std::string_view sv{ "abcdef" };
        check("dw3_sv_arg_precision_truncates",
            vmhook::detail::format_log("{:.4}", sv) == "abcd");
        check("dw3_sv_arg_width_right_align",
            vmhook::detail::format_log("{:>8}", sv) == "  abcdef");
        check("dw3_sv_arg_precision_then_width",
            vmhook::detail::format_log("{:>6.3}", sv) == "   abc");
    }
    // An EMPTY string_view argument renders as nothing; width still pads.
    {
        std::string_view empty_sv{};
        check("dw3_sv_empty_arg_is_empty",
            vmhook::detail::format_log("[{}]", empty_sv) == "[]");
        check("dw3_sv_empty_arg_width_pads",
            vmhook::detail::format_log("[{:>3}]", empty_sv) == "[   ]");
    }

    // --- Alt-form (#) sign placement on NEGATIVE base values. --------------
    // The sign precedes the alt-form prefix: '-' then "0x" then magnitude.
    check("dw3_neg_alt_hex_sign_first",
        vmhook::detail::format_log("{:#x}", -255) == "-0xff");
    check("dw3_neg_alt_binary_sign_first",
        vmhook::detail::format_log("{:#b}", -5) == "-0b101");
    check("dw3_neg_alt_octal_sign_first",
        vmhook::detail::format_log("{:#o}", -8) == "-010");
    // Negative alt-form hex with zero-pad: '-' "0x" zero-fill magnitude, the
    // sign + prefix counting toward the field width (width 8 -> 3 fill zeros).
    check("dw3_neg_alt_hex_zeropad_sign_prefix_count",
        vmhook::detail::format_log("{:#08x}", -255) == "-0x000ff");

    // --- Sign flags on floating ZERO / negative-zero. ---------------------
    // '+' forces a leading '+' on +0.0; '-0.0' keeps its '-'.
    check("dw3_plus_flag_pos_zero_fixed",
        vmhook::detail::format_log("{:+.1f}", 0.0) == "+0.0");
    check("dw3_minus_zero_fixed_keeps_sign",
        vmhook::detail::format_log("{:.1f}", -0.0) == "-0.0");
    // The space flag on +0.0 emits a leading space (not '+'); -0.0 keeps '-'.
    check("dw3_space_flag_pos_zero_fixed",
        vmhook::detail::format_log("{: .1f}", 0.0) == " 0.0");
    check("dw3_space_flag_neg_zero_fixed",
        vmhook::detail::format_log("{: .1f}", -0.0) == "-0.0");

    // --- {:c} on the remaining boundary code points. ----------------------
    // Code 9 is a horizontal tab; {:c} produces a single '\t' byte.
    check("dw3_int_as_char_tab",
        vmhook::detail::format_log("{:c}", 9) == "\t");
    // Code 127 (DEL) is a single byte 0x7F.
    {
        const std::string del{ vmhook::detail::format_log("{:c}", 127) };
        check("dw3_int_as_char_del_size", del.size() == 1u);
        check("dw3_int_as_char_del_byte",
            del.size() == 1u && static_cast<unsigned char>(del[0]) == 0x7Fu);
    }
    // Code 65 ('A') with a precision is rejected for a char presentation? No —
    // {:c} ignores precision; width still applies (covered in dw2).  Here pin
    // the plain glyph at the upper printable boundary code 126 again via {}.
    check("dw3_char_value_126_glyph",
        vmhook::detail::format_log("{}", static_cast<char>(126)) == "~");

    // --- '0' as an explicit FILL char (distinct from the zero-pad form). ---
    // "{:0>5}" uses '0' as the alignment fill (right-align), which for a
    // NEGATIVE value pads with '0' AFTER the sign position is NOT special — the
    // fill form treats the whole "-7" token as the content -> "000-7".  This is
    // distinct from "{:05}" (zero-pad) which would yield "-0007".
    check("dw3_zero_fill_align_negative_is_not_zeropad",
        vmhook::detail::format_log("{:0>5}", -7) == "000-7");
    check("dw3_zeropad_negative_for_contrast",
        vmhook::detail::format_log("{:05}", -7) == "-0007");

    // --- Width on a bracketed tag with LEFT align (pads on the right). -----
    // info_tag is 13 chars; width 16 left-aligned -> 3 trailing spaces.
    check("dw3_tag_left_aligned_width",
        vmhook::detail::format_log("{:<16}", vmhook::info_tag)
            == "[VMHook INFO]   ");
    // Tag center-aligned in width 18 (13 chars -> 5 pad, floor-left 2 / right 3).
    check("dw3_tag_center_aligned_width",
        vmhook::detail::format_log("{:^18}", vmhook::info_tag)
            == "  [VMHook INFO]   ");

    // --- A char-typed ARGUMENT that is itself a brace is literal data. -----
    // The arg value '{' is NOT a format field — it is emitted as the glyph.
    check("dw3_char_arg_open_brace_is_glyph",
        vmhook::detail::format_log("{}", '{') == "{");
    check("dw3_char_arg_close_brace_is_glyph",
        vmhook::detail::format_log("{}", '}') == "}");
#else
    // Fallback leg: every dw3_ std-format input above returns the fmt verbatim.
    check("dw3_fb_sv_precision_verbatim",
        vmhook::detail::format_log("{:.4}", std::string_view{ "abcdef" }) == "{:.4}");
    check("dw3_fb_neg_alt_hex_verbatim",
        vmhook::detail::format_log("{:#x}", -255) == "{:#x}");
    check("dw3_fb_neg_alt_hex_zeropad_verbatim",
        vmhook::detail::format_log("{:#08x}", -255) == "{:#08x}");
    check("dw3_fb_plus_zero_fixed_verbatim",
        vmhook::detail::format_log("{:+.1f}", 0.0) == "{:+.1f}");
    check("dw3_fb_space_zero_fixed_verbatim",
        vmhook::detail::format_log("{: .1f}", 0.0) == "{: .1f}");
    check("dw3_fb_int_as_char_tab_verbatim",
        vmhook::detail::format_log("{:c}", 9) == "{:c}");
    check("dw3_fb_zero_fill_align_verbatim",
        vmhook::detail::format_log("{:0>5}", -7) == "{:0>5}");
    check("dw3_fb_tag_left_width_verbatim",
        vmhook::detail::format_log("{:<16}", vmhook::info_tag) == "{:<16}");
    check("dw3_fb_char_arg_brace_verbatim",
        vmhook::detail::format_log("{}", '{') == "{}");
#endif

    // --- Cross-branch: format_log result element type is char (std::string).
    // remove_cvref_t strips the reference decltype(str[i]) carries, so the
    // element type is exactly `char` on either branch.
    {
        const std::string produced{ vmhook::detail::format_log("e", 1) };
        using elem_t = std::remove_cvref_t<decltype(produced[0])>;
        check("dw3_format_result_element_is_char",
            std::is_same_v<elem_t, char> && !produced.empty());
    }

    // --- Cross-branch emit invariants not covered above. -------------------
    // A payload of several NUL bytes is length-preserving; emit appends '\n'.
    {
        std::string captured;
        std::string payload;
        payload.push_back('\0');
        payload.push_back('\0');
        payload.push_back('\0');
        {
            cout_capture cap;
            vmhook::detail::emit_log_line(payload);
            captured = cap.str();
        }
        check("dw3_emit_multi_nul_length_preserved",
            captured.size() == 4u && captured.back() == '\n');
    }
    // A single-space payload passes through verbatim plus the appended newline.
    {
        std::string captured;
        {
            cout_capture cap;
            vmhook::detail::emit_log_line(std::string{ " " });
            captured = cap.str();
        }
        check("dw3_emit_single_space_verbatim", captured == " \n");
    }
    // Interleaved emit + format_log-then-emit in one capture preserves order.
    {
        std::string captured;
        {
            cout_capture cap;
            vmhook::detail::emit_log_line(std::string{ "raw" });
            vmhook::detail::emit_log_line(
                vmhook::detail::format_log("fmt-noarg"));
            captured = cap.str();
        }
        // "fmt-noarg" has no fields, so it round-trips verbatim on BOTH legs.
        check("dw3_emit_raw_then_formatted_order",
            captured == "raw\nfmt-noarg\n");
    }
    // A third concurrent interleaving shape: each line is "<tag-letter><i>" with
    // a distinct per-thread letter, K*M intact newline-terminated lines result.
    {
        constexpr int K{ 4 };
        constexpr int M{ 40 };
        std::string captured;
        {
            cout_capture cap;
            std::vector<std::thread> threads;
            threads.reserve(K);
            for (int t{ 0 }; t < K; ++t)
            {
                threads.emplace_back([t]()
                {
                    const char letter{ static_cast<char>('A' + t) };
                    for (int i{ 0 }; i < M; ++i)
                    {
                        vmhook::detail::emit_log_line(
                            std::string{ letter } + std::to_string(i));
                    }
                });
            }
            for (auto& th : threads) { th.join(); }
            captured = cap.str();
        }
        int line_count{ 0 };
        bool all_wellformed{ true };
        {
            std::istringstream iss{ captured };
            std::string line;
            while (std::getline(iss, line))
            {
                ++line_count;
                // Each line starts with one of the per-thread letters [A..D]
                // and is followed by at least one digit (no torn write).
                if (line.size() < 2u
                    || line.front() < 'A' || line.front() > 'D'
                    || !std::isdigit(static_cast<unsigned char>(line[1])))
                {
                    all_wellformed = false;
                }
            }
        }
        check("dw3_emit_concurrent_letter_count_exact", line_count == K * M);
        check("dw3_emit_concurrent_letter_no_torn_writes", all_wellformed);
    }
    // Second arg-evaluation probe through VMHOOK_LOG: an incrementing counter
    // passed as the formatted value is evaluated exactly once in the active
    // build and never in the no-op build (the sizeof-unevaluated seam).
    {
        std::atomic<int> calls{ 0 };
        auto next = [&calls]() -> int { return calls.fetch_add(1) + 1; };
        {
            cout_capture cap;            // swallow any active-build output
            VMHOOK_LOG("{} n={}", vmhook::warning_tag, next());
            (void)cap;
        }
#if VMHOOK_DEBUG_LOGS
        check("dw3_vmhook_log_active_evaluates_once", calls.load() == 1);
#else
        check("dw3_vmhook_log_noop_no_eval", calls.load() == 0);
#endif
    }

    // =====================================================================
    // DEEPENING WAVE 4 (additive) — a FOURTH namespaced section ("dw4_")
    // closing the LAST proven-OPEN ledger gaps the first three waves did NOT
    // touch (audit/COVERAGE_LEDGER.md, logging_format row):
    //   * the SUBNORMAL double (denorm_min) through {:.17g} / {:e} / {:f}
    //     (the one float-special the ledger lists that dw_/dw2_/dw3_ omit),
    //   * the sign flags '+' and ' ' combined with a BASE spec on INT_MIN
    //     (the sign flag only affects NON-negatives, so a negative under
    //     {:+x}/{: x} still renders '-' + magnitude — pin that),
    //   * the space flag on a positive DECIMAL ({: d}),
    //   * alt-form {:#x} at the UINT_MAX and INT_MIN boundaries (the two
    //     boundary values the int sweep printed in plain decimal but not in
    //     alt-form hex).
    // PURE LOGIC ONLY — no memory reads, no pointer fabrication, no value_t
    // casts, no narrowing.  Every expected value is derived from source and
    // was confirmed against libstdc++ std::format:
    //   * VMHOOK_HAS_STD_FORMAT==1 -> format_log == std::vformat(fmt, args);
    //     the {:e}/{:.17g} digit counts are FIXED (not shortest-round-trip),
    //     so they are byte-identical on libstdc++ AND the MSVC STL, and the
    //     subnormal {:f} expansion (implementation-length) is asserted as a
    //     round-trip PROPERTY rather than a byte string.
    //   * VMHOOK_HAS_STD_FORMAT==0 -> format_log == std::string{ fmt }.
    // =====================================================================
#if VMHOOK_HAS_STD_FORMAT
    // --- Subnormal double: denorm_min == 2^-1074, the smallest positive value.
    // {:.17g} prints a FIXED 17 significant digits (standard-pinned, not
    // shortest), so assert the exact spelling AND that it round-trips back to
    // the same subnormal double (the STL-invariant property).
    check("dw4_subnormal_full_precision_g17",
        vmhook::detail::format_log(
            "{:.17g}", std::numeric_limits<double>::denorm_min())
            == "4.9406564584124654e-324");
    check("dw4_subnormal_full_precision_roundtrips",
        fpinv::parses_to(
            vmhook::detail::format_log(
                "{:.17g}", std::numeric_limits<double>::denorm_min()),
            std::numeric_limits<double>::denorm_min()));
    // {:.2e} on the subnormal: a FIXED 2-fraction-digit scientific layout with
    // the three-digit exponent — byte-pinned by the standard.
    check("dw4_subnormal_scientific_precision",
        vmhook::detail::format_log(
            "{:.2e}", std::numeric_limits<double>::denorm_min())
            == "4.94e-324");
    // {:.0e} rounds to a single significant digit (still byte-pinned).
    check("dw4_subnormal_scientific_zero_precision",
        vmhook::detail::format_log(
            "{:.0e}", std::numeric_limits<double>::denorm_min())
            == "5e-324");
    // {:f} on the subnormal yields a (very long) all-decimal expansion whose
    // exact length is implementation-defined; assert only that it is non-empty,
    // contains a decimal point, and is sign-free (positive value).
    {
        const std::string sub_fixed{
            vmhook::detail::format_log(
                "{:f}", std::numeric_limits<double>::denorm_min()) };
        check("dw4_subnormal_fixed_nonempty_pointed",
            !sub_fixed.empty()
                && sub_fixed.front() == '0'
                && sub_fixed.find('.') != std::string::npos);
    }

    // --- Sign flags ('+' / ' ') COMBINED with a base spec on INT_MIN. ---------
    // The sign flag governs only NON-negative values, so a negative under
    // {:+x}/{: x} still renders '-' + base-magnitude (NOT '+'/' ').  INT_MIN's
    // magnitude in hex is 0x80000000.  Pin that the sign flag does NOT alter the
    // negative form.
    check("dw4_int_min_plus_flag_hex_still_minus",
        vmhook::detail::format_log("{:+x}", INT_MIN) == "-80000000");
    check("dw4_int_min_space_flag_hex_still_minus",
        vmhook::detail::format_log("{: x}", INT_MIN) == "-80000000");
    // The space flag on a POSITIVE decimal emits a single leading space.
    check("dw4_space_flag_positive_decimal",
        vmhook::detail::format_log("{: d}", 7) == " 7");
    // The space flag on a NEGATIVE decimal still uses '-' (never a space).
    check("dw4_space_flag_negative_decimal",
        vmhook::detail::format_log("{: d}", -7) == "-7");
    // The '+' flag on a positive decimal forces a leading '+'.
    check("dw4_plus_flag_positive_decimal",
        vmhook::detail::format_log("{:+d}", 7) == "+7");

    // --- Alt-form {:#x} at the UINT_MAX / INT_MIN boundaries. -----------------
    // UINT_MAX is unsigned -> no sign, "0x" + all-f's.
    check("dw4_uint_max_alt_hex",
        vmhook::detail::format_log("{:#x}", UINT_MAX) == "0xffffffff");
    // INT_MIN is signed -> '-' precedes the "0x" prefix, then the magnitude.
    check("dw4_int_min_alt_hex_sign_before_prefix",
        vmhook::detail::format_log("{:#x}", INT_MIN) == "-0x80000000");
#else
    // Fallback leg: every dw4_ std-format input above returns the fmt verbatim,
    // ignoring all arguments and specifiers — mirror the spec-bearing strings so
    // the pre-std::format MinGW/Clang CI legs exercise the same surface.  The
    // subnormal value is still referenced (passed as the ignored argument) so no
    // const is unused on this leg either.
    check("dw4_fb_subnormal_g17_verbatim",
        vmhook::detail::format_log(
            "{:.17g}", std::numeric_limits<double>::denorm_min()) == "{:.17g}");
    check("dw4_fb_subnormal_scientific_verbatim",
        vmhook::detail::format_log(
            "{:.2e}", std::numeric_limits<double>::denorm_min()) == "{:.2e}");
    check("dw4_fb_subnormal_fixed_verbatim",
        vmhook::detail::format_log(
            "{:f}", std::numeric_limits<double>::denorm_min()) == "{:f}");
    check("dw4_fb_int_min_plus_hex_verbatim",
        vmhook::detail::format_log("{:+x}", INT_MIN) == "{:+x}");
    check("dw4_fb_int_min_space_hex_verbatim",
        vmhook::detail::format_log("{: x}", INT_MIN) == "{: x}");
    check("dw4_fb_space_decimal_verbatim",
        vmhook::detail::format_log("{: d}", 7) == "{: d}");
    check("dw4_fb_uint_max_alt_hex_verbatim",
        vmhook::detail::format_log("{:#x}", UINT_MAX) == "{:#x}");
    check("dw4_fb_int_min_alt_hex_verbatim",
        vmhook::detail::format_log("{:#x}", INT_MIN) == "{:#x}");
#endif

    std::printf("\n%d checks failed\n", failures);
    return failures == 0 ? 0 : 1;
}
