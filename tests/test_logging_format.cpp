// Standalone (no-JVM) tests for vmhook's diagnostic logging layer:
// detail::format_log formatting, the VMHOOK_LOG macro, the *_tag literals,
// detail::emit_log_line null/edge tolerance, and the never-throw guarantee.
#include <vmhook/vmhook.hpp>

#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
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
    // float / double argument — exact integral-valued double is portable.
    check("format_double",
        vmhook::detail::format_log("{}", 1.5) == "1.5");
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

    // --- More substitution arities (0..4 fields). -----------------------
    check("format_arity_0_plain",
        vmhook::detail::format_log("none") == "none");
    check("format_arity_1",
        vmhook::detail::format_log("[{}]", 1) == "[1]");
    check("format_arity_2",
        vmhook::detail::format_log("{},{}", 1, 2) == "1,2");
    check("format_arity_4",
        vmhook::detail::format_log("{}{}{}{}", 1, 2, 3, 4) == "1234");
    // Extra trailing arguments are simply ignored (NOT an error in std::format).
    check("format_extra_args_ignored",
        vmhook::detail::format_log("{}", 1, 2, 3) == "1");
    check("format_no_field_with_args_ignored",
        vmhook::detail::format_log("literal", 1, 2) == "literal");

    // --- Repeated / reordered positional indices. -----------------------
    check("format_repeated_index",
        vmhook::detail::format_log("{0}{0}{0}", 7) == "777");
    check("format_full_reverse_index",
        vmhook::detail::format_log("{2}{1}{0}", 1, 2, 3) == "321");
    check("format_mixed_repeat_and_order",
        vmhook::detail::format_log("{1}{0}{1}", std::string{ "a" }, std::string{ "b" })
            == "bab");

    // --- Format-spec variety (width / fill / align / sign / base / prec). */
    check("format_right_align_width",
        vmhook::detail::format_log("{:>5}", 7) == "    7");
    check("format_left_align_width",
        vmhook::detail::format_log("{:<5}", 7) == "7    ");
    check("format_center_align_width",
        vmhook::detail::format_log("{:^5}", 7) == "  7  ");
    check("format_custom_fill_center",
        vmhook::detail::format_log("{:*^5}", 7) == "**7**");
    check("format_zero_pad_width",
        vmhook::detail::format_log("{:05}", 42) == "00042");
    check("format_explicit_plus_sign",
        vmhook::detail::format_log("{:+}", 5) == "+5");
    check("format_explicit_plus_on_negative",
        vmhook::detail::format_log("{:+}", -5) == "-5");
    check("format_hex_lower",
        vmhook::detail::format_log("{:x}", 255) == "ff");
    check("format_hex_alt_form",
        vmhook::detail::format_log("{:#x}", 255) == "0xff");
    check("format_hex_upper",
        vmhook::detail::format_log("{:X}", 255) == "FF");
    check("format_binary",
        vmhook::detail::format_log("{:b}", 5) == "101");
    check("format_octal",
        vmhook::detail::format_log("{:o}", 8) == "10");
    check("format_fixed_precision",
        vmhook::detail::format_log("{:.2f}", 1.5) == "1.50");
    check("format_fixed_precision_zero",
        vmhook::detail::format_log("{:.0f}", 2.5) == "2");
    // The internal hook-logging idiom uses {:08X} on a compressed OOP; pin it.
    check("format_hex_zero_pad_8_upper",
        vmhook::detail::format_log("0x{:08X}", 0xDEADu) == "0x0000DEAD");

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

    // --- Malformed-but-runtime-parsed format strings must NOT throw; the ---
    //     catch arm hands back the verbatim format string (vmhook.hpp:333-340).
    {
        // A lone unmatched '{' is an invalid format -> std::vformat throws ->
        // format_log returns the fmt verbatim.
        check("format_unmatched_open_brace_returns_fmt",
            vmhook::detail::format_log("{", 1) == "{");
        // A stray unmatched '}' is also invalid -> verbatim.
        check("format_unmatched_close_brace_returns_fmt",
            vmhook::detail::format_log("a}b") == "a}b");
        // An out-of-range positional index throws at runtime -> verbatim.
        check("format_index_out_of_range_returns_fmt",
            vmhook::detail::format_log("{5}", 1) == "{5}");
        // A bad format spec for the type throws -> verbatim.  (':d' on a
        // non-integer std::string is invalid.)
        check("format_bad_spec_for_type_returns_fmt",
            vmhook::detail::format_log("{:d}", std::string{ "x" }) == "{:d}");
    }
#else
    // Without std::format, format_log ignores specifiers and returns the
    // format string verbatim (documented fallback behaviour).
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
#if VMHOOK_HAS_STD_FORMAT
        // On the catch path the verbatim format string is returned.
        check("format_mismatched_returns_fmt", result == "{}");
#else
        check("format_mismatched_returns_fmt", result == "{}");
#endif
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

    // ---------------------------------------------------------------------
    // detail::emit_log_line is noexcept and must tolerate any std::string,
    // including empty and very long inputs, without crashing or throwing.
    // (Output goes to std::cout / VMHOOK_LOG_FILE; we only assert it returns
    // normally.)
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

    std::printf("\n%d checks failed\n", failures);
    return failures == 0 ? 0 : 1;
}
