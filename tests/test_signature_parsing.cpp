// Standalone (no-JVM) unit test for vmhook's JVM-descriptor parsing helpers:
// detail::sig_char_to_basic_type, detail::jvm_primitive_byte_width,
// detail::jni_signature_for_arg<T>, and the inline return-descriptor extraction
// (the char after the close-paren) that method_proxy::call feeds into
// sig_char_to_basic_type.  These are all pure compile-time / table-lookup
// helpers: they touch no oop and no running JVM, so they are exhaustively
// testable here.  Anything that needs a live oop or interpreter (the actual
// resolve_compatible_method hierarchy walk, get_method(name, sig) against a
// real klass, method_proxy::call dispatch) is OUT OF SCOPE for this file and is
// covered by JVM integration in example.cpp.
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

// Mirror of the inline return-type extraction in method_proxy::call
// (vmhook.hpp ~12241-12245): find the last ')', the return descriptor is the
// single char immediately after it; when there is no ')' the contract is to
// treat the return type as void ('V').  The library has no *named* helper for
// this step — the BasicType lookup it performs IS detail::sig_char_to_basic_type
// — so we reproduce the one-line extraction and assert the composed result,
// exercising sig_char_to_basic_type through the exact code path call() uses.
static auto return_basic_type_of(std::string_view signature) -> int
{
    const std::size_t rparen{ signature.rfind(')') };
    const char        ret_char{ rparen != std::string_view::npos ? signature[rparen + 1] : 'V' };
    return vmhook::detail::sig_char_to_basic_type(ret_char);
}

int main()
{
    // ---- detail::sig_char_to_basic_type: HotSpot BasicType ints --------------
    // Values are the stable HotSpot BasicType enum integers and must not drift.
    check("sig_char_basic_type_boolean_Z_is_4", vmhook::detail::sig_char_to_basic_type('Z') == 4);
    check("sig_char_basic_type_char_C_is_5", vmhook::detail::sig_char_to_basic_type('C') == 5);
    check("sig_char_basic_type_float_F_is_6", vmhook::detail::sig_char_to_basic_type('F') == 6);
    check("sig_char_basic_type_double_D_is_7", vmhook::detail::sig_char_to_basic_type('D') == 7);
    check("sig_char_basic_type_byte_B_is_8", vmhook::detail::sig_char_to_basic_type('B') == 8);
    check("sig_char_basic_type_short_S_is_9", vmhook::detail::sig_char_to_basic_type('S') == 9);
    check("sig_char_basic_type_int_I_is_10", vmhook::detail::sig_char_to_basic_type('I') == 10);
    check("sig_char_basic_type_long_J_is_11", vmhook::detail::sig_char_to_basic_type('J') == 11);
    check("sig_char_basic_type_object_L_is_12", vmhook::detail::sig_char_to_basic_type('L') == 12);
    check("sig_char_basic_type_array_bracket_is_13", vmhook::detail::sig_char_to_basic_type('[') == 13);
    check("sig_char_basic_type_void_V_is_14", vmhook::detail::sig_char_to_basic_type('V') == 14);
    // Unknown / unexpected chars fall back to T_OBJECT (12) by documented design,
    // so a malformed return descriptor degrades to "treat as object" rather than
    // tripping an assert.
    check("sig_char_basic_type_unknown_falls_back_to_object_12", vmhook::detail::sig_char_to_basic_type('Q') == 12);
    check("sig_char_basic_type_nul_falls_back_to_object_12", vmhook::detail::sig_char_to_basic_type('\0') == 12);
    check("sig_char_basic_type_lowercase_is_not_primitive_12", vmhook::detail::sig_char_to_basic_type('i') == 12);

    // ---- detail::jvm_primitive_byte_width: in-heap primitive widths ----------
    // Single-char primitive descriptors map to their JVM spec widths.
    check("byte_width_boolean_Z_is_1", vmhook::detail::jvm_primitive_byte_width("Z") == 1);
    check("byte_width_byte_B_is_1", vmhook::detail::jvm_primitive_byte_width("B") == 1);
    check("byte_width_short_S_is_2", vmhook::detail::jvm_primitive_byte_width("S") == 2);
    check("byte_width_char_C_is_2", vmhook::detail::jvm_primitive_byte_width("C") == 2);
    check("byte_width_int_I_is_4", vmhook::detail::jvm_primitive_byte_width("I") == 4);
    check("byte_width_float_F_is_4", vmhook::detail::jvm_primitive_byte_width("F") == 4);
    check("byte_width_long_J_is_8", vmhook::detail::jvm_primitive_byte_width("J") == 8);
    check("byte_width_double_D_is_8", vmhook::detail::jvm_primitive_byte_width("D") == 8);
    // Reference / array / void / unknown / non-single-char all return 0 so the
    // caller (field_proxy::set) skips its width validation rather than rejecting.
    check("byte_width_void_V_is_0", vmhook::detail::jvm_primitive_byte_width("V") == 0);
    check("byte_width_object_descriptor_is_0", vmhook::detail::jvm_primitive_byte_width("Ljava/lang/String;") == 0);
    check("byte_width_array_descriptor_is_0", vmhook::detail::jvm_primitive_byte_width("[I") == 0);
    check("byte_width_bare_L_single_char_is_0", vmhook::detail::jvm_primitive_byte_width("L") == 0);
    check("byte_width_empty_string_is_0", vmhook::detail::jvm_primitive_byte_width("") == 0);
    check("byte_width_unknown_single_char_is_0", vmhook::detail::jvm_primitive_byte_width("Q") == 0);
    // The size==1 guard means even a valid primitive letter padded to length>1
    // is rejected (no leading/trailing-byte tolerance).
    check("byte_width_multichar_primitive_is_0", vmhook::detail::jvm_primitive_byte_width("II") == 0);

    // ---- detail::jni_signature_for_arg<T>: C++ type -> JNI descriptor --------
    // Only the supported types are instantiated here; the unsupported-type branch
    // is a hard static_assert(dependent_false_v) by design, so e.g.
    // jni_signature_for_arg<void*> / <char> would fail to COMPILE — that compile
    // -time rejection is the contract and cannot be probed at runtime.
    check("jni_sig_std_string_is_String", vmhook::detail::jni_signature_for_arg<std::string>() == "Ljava/lang/String;");
    check("jni_sig_string_view_is_String", vmhook::detail::jni_signature_for_arg<std::string_view>() == "Ljava/lang/String;");
    check("jni_sig_const_char_ptr_is_String", vmhook::detail::jni_signature_for_arg<const char*>() == "Ljava/lang/String;");
    check("jni_sig_char_ptr_is_String", vmhook::detail::jni_signature_for_arg<char*>() == "Ljava/lang/String;");
    check("jni_sig_bool_is_Z", vmhook::detail::jni_signature_for_arg<bool>() == "Z");
    check("jni_sig_int8_is_B", vmhook::detail::jni_signature_for_arg<std::int8_t>() == "B");
    check("jni_sig_uint8_is_B", vmhook::detail::jni_signature_for_arg<std::uint8_t>() == "B");
    check("jni_sig_int16_is_S", vmhook::detail::jni_signature_for_arg<std::int16_t>() == "S");
    // uint16_t maps to Java char ('C'), NOT short — this is the unsigned-16 split.
    check("jni_sig_uint16_is_C", vmhook::detail::jni_signature_for_arg<std::uint16_t>() == "C");
    check("jni_sig_int32_is_I", vmhook::detail::jni_signature_for_arg<std::int32_t>() == "I");
    check("jni_sig_uint32_is_I", vmhook::detail::jni_signature_for_arg<std::uint32_t>() == "I");
    check("jni_sig_int64_is_J", vmhook::detail::jni_signature_for_arg<std::int64_t>() == "J");
    check("jni_sig_uint64_is_J", vmhook::detail::jni_signature_for_arg<std::uint64_t>() == "J");
    check("jni_sig_float_is_F", vmhook::detail::jni_signature_for_arg<float>() == "F");
    check("jni_sig_double_is_D", vmhook::detail::jni_signature_for_arg<double>() == "D");
    // Plain `int` is a 4-byte integral on every supported target, so it routes
    // through the generic sizeof==int32 branch to "I".
    check("jni_sig_plain_int_is_I", vmhook::detail::jni_signature_for_arg<int>() == "I");
    // cv / ref qualifiers are stripped via std::decay_t before dispatch.
    check("jni_sig_strips_const_ref_on_string", vmhook::detail::jni_signature_for_arg<const std::string&>() == "Ljava/lang/String;");
    check("jni_sig_strips_const_ref_on_double", vmhook::detail::jni_signature_for_arg<const double&>() == "D");

    // ---- return-descriptor extraction (char after the close paren) ----------
    // Reproduces method_proxy::call's inline rfind(')')+1 lookup feeding
    // sig_char_to_basic_type, asserting the composed BasicType for each return.
    check("return_type_void_method_is_14", return_basic_type_of("()V") == 14);
    check("return_type_int_method_is_10", return_basic_type_of("(I)I") == 10);
    check("return_type_long_method_is_11", return_basic_type_of("(II)J") == 11);
    check("return_type_object_method_is_12", return_basic_type_of("(I)Ljava/lang/String;") == 12);
    check("return_type_array_method_is_13", return_basic_type_of("(I)[B") == 13);
    check("return_type_boolean_method_is_4", return_basic_type_of("(Ljava/lang/Object;)Z") == 4);
    // rfind(')') correctly picks the LAST paren even when a nested object
    // descriptor in the param list contains characters — here a method whose
    // single param is itself a method-typed... (synthetic) — we only assert that
    // the final ')' governs: trailing 'D' after the last ')' is the return.
    check("return_type_uses_last_paren_double_is_7", return_basic_type_of("(Ljava/lang/Object;)D") == 7);
    // Degenerate / malformed signature with no ')' is treated as void ('V' -> 14)
    // by the call-site fallback, never an out-of-bounds read.
    check("return_type_no_paren_defaults_void_14", return_basic_type_of("garbage") == 14);
    check("return_type_empty_signature_defaults_void_14", return_basic_type_of("") == 14);

    // =====================================================================
    // EXPANDED COVERAGE (additive — every expected value derived from the
    // sig_char_to_basic_type / jvm_primitive_byte_width tables in vmhook.hpp).
    // =====================================================================

    // ---- sig_char_to_basic_type: EVERY ASCII control char (0x01..0x1F) ------
    // None of these is a recognised descriptor letter, so each falls through to
    // the default branch -> T_OBJECT (12).  (0x00 is already asserted above.)
    {
        bool all_controls_object{ true };
        for (int c{ 0x01 }; c <= 0x1F; ++c)
        {
            if (vmhook::detail::sig_char_to_basic_type(static_cast<char>(c)) != 12)
            {
                all_controls_object = false;
            }
        }
        check("sig_char_all_control_chars_fall_back_to_object_12", all_controls_object);
    }
    // DEL (0x7F) and the space char are likewise unrecognised -> 12.
    check("sig_char_del_0x7F_falls_back_to_object_12",
          vmhook::detail::sig_char_to_basic_type(static_cast<char>(0x7F)) == 12);
    check("sig_char_space_falls_back_to_object_12",
          vmhook::detail::sig_char_to_basic_type(' ') == 12);

    // ---- sig_char_to_basic_type: EVERY uppercase letter A..Z ----------------
    // Exactly Z,C,F,D,B,S,I,J,L,V are recognised; every OTHER uppercase letter
    // (A E G H K M N O P Q R T U W X Y) maps to the T_OBJECT fallback (12).
    // 'L' itself maps to T_OBJECT (12) by design, and 'A' (T_ARRAY is '[', not
    // 'A') is NOT special — it is a plain fallback.
    {
        struct upper_case { char c; int expected; };
        const upper_case uppers[]{
            { 'A', 12 }, { 'B', 8 },  { 'C', 5 },  { 'D', 7 },  { 'E', 12 },
            { 'F', 6 },  { 'G', 12 }, { 'H', 12 }, { 'I', 10 }, { 'J', 11 },
            { 'K', 12 }, { 'L', 12 }, { 'M', 12 }, { 'N', 12 }, { 'O', 12 },
            { 'P', 12 }, { 'Q', 12 }, { 'R', 12 }, { 'S', 9 },  { 'T', 12 },
            { 'U', 12 }, { 'V', 14 }, { 'W', 12 }, { 'X', 12 }, { 'Y', 12 },
            { 'Z', 4 },
        };
        bool all_match{ true };
        for (const upper_case& u : uppers)
        {
            if (vmhook::detail::sig_char_to_basic_type(u.c) != u.expected)
            {
                all_match = false;
            }
        }
        check("sig_char_every_uppercase_letter_maps_per_table", all_match);
    }

    // ---- sig_char_to_basic_type: EVERY lowercase letter a..z ----------------
    // The switch is case-sensitive, so no lowercase letter is a primitive; all
    // 26 map to the T_OBJECT fallback (12).  This pins that e.g. 'z'/'i'/'j'
    // are NOT silently accepted as their uppercase counterparts.
    {
        bool all_lower_object{ true };
        for (char c{ 'a' }; c <= 'z'; ++c)
        {
            if (vmhook::detail::sig_char_to_basic_type(c) != 12)
            {
                all_lower_object = false;
            }
        }
        check("sig_char_every_lowercase_letter_falls_back_to_object_12", all_lower_object);
    }

    // ---- sig_char_to_basic_type: EVERY decimal digit 0..9 -------------------
    // Digits are not descriptor letters -> 12.
    {
        bool all_digits_object{ true };
        for (char c{ '0' }; c <= '9'; ++c)
        {
            if (vmhook::detail::sig_char_to_basic_type(c) != 12)
            {
                all_digits_object = false;
            }
        }
        check("sig_char_every_digit_falls_back_to_object_12", all_digits_object);
    }

    // ---- sig_char_to_basic_type: assorted punctuation / symbols -------------
    // The descriptor grammar uses ';' (terminator), '/' (separator) and '$'
    // (inner-class) in NAMES, but sig_char_to_basic_type is a single-CHAR
    // classifier: fed these in isolation it returns the T_OBJECT fallback (12).
    // ']' is not the array marker ('[' is) and also falls back.
    {
        const char symbols[]{ ';', '/', '$', '_', '-', '.', '(', ')', ']', '*', '?', '+' };
        bool all_symbols_object{ true };
        for (const char c : symbols)
        {
            if (vmhook::detail::sig_char_to_basic_type(c) != 12)
            {
                all_symbols_object = false;
            }
        }
        check("sig_char_assorted_symbols_fall_back_to_object_12", all_symbols_object);
    }
    // The array marker '[' is the ONE bracket that is special (T_ARRAY = 13);
    // re-pin it alongside the negative ']' case above to make the asymmetry
    // explicit.
    check("sig_char_open_bracket_is_array_13",
          vmhook::detail::sig_char_to_basic_type('[') == 13);
    check("sig_char_close_bracket_is_object_12",
          vmhook::detail::sig_char_to_basic_type(']') == 12);

    // ---- jvm_primitive_byte_width: multi-bracket array descriptors ----------
    // size() != 1 for any of these, so ALL return 0 (arrays have no fixed
    // in-heap primitive width here — handled by the compressed-OOP path).
    check("byte_width_double_bracket_int_is_0",
          vmhook::detail::jvm_primitive_byte_width("[[I") == 0);
    check("byte_width_triple_bracket_int_is_0",
          vmhook::detail::jvm_primitive_byte_width("[[[I") == 0);
    check("byte_width_array_of_long_is_0",
          vmhook::detail::jvm_primitive_byte_width("[J") == 0);
    check("byte_width_array_of_object_is_0",
          vmhook::detail::jvm_primitive_byte_width("[Ljava/lang/Object;") == 0);

    // ---- jvm_primitive_byte_width: nested / malformed object descriptors ----
    // Any length>1 string is rejected by the size==1 guard up front, before the
    // first char is even inspected — so even a string that STARTS with a valid
    // primitive letter (e.g. "Ifoo") returns 0.
    check("byte_width_object_no_semicolon_is_0",
          vmhook::detail::jvm_primitive_byte_width("Ljava/lang/String") == 0);
    check("byte_width_object_only_L_and_semicolon_is_0",
          vmhook::detail::jvm_primitive_byte_width("L;") == 0);
    check("byte_width_primitive_letter_with_trailing_junk_is_0",
          vmhook::detail::jvm_primitive_byte_width("Ifoo") == 0);
    check("byte_width_primitive_letter_with_leading_junk_is_0",
          vmhook::detail::jvm_primitive_byte_width("xI") == 0);

    // ---- jvm_primitive_byte_width: whitespace and control single chars ------
    // A single space / tab / NUL is size()==1 but not a primitive letter -> 0.
    check("byte_width_single_space_is_0",
          vmhook::detail::jvm_primitive_byte_width(" ") == 0);
    check("byte_width_single_tab_is_0",
          vmhook::detail::jvm_primitive_byte_width("\t") == 0);
    check("byte_width_single_nul_is_0",
          vmhook::detail::jvm_primitive_byte_width(std::string_view{ "\0", 1 }) == 0);
    // A leading/trailing space around a valid letter makes size()==2 -> 0.
    check("byte_width_space_padded_int_is_0",
          vmhook::detail::jvm_primitive_byte_width(" I") == 0);
    check("byte_width_int_trailing_space_is_0",
          vmhook::detail::jvm_primitive_byte_width("I ") == 0);

    // ---- jvm_primitive_byte_width: EVERY single uppercase letter ------------
    // Exactly Z,B (1), S,C (2), I,F (4), J,D (8) have a width; every other
    // single uppercase letter (including 'L', 'V', and 'A') returns 0.
    {
        struct width_case { char c; std::size_t expected; };
        const width_case widths[]{
            { 'A', 0 }, { 'B', 1 }, { 'C', 2 }, { 'D', 8 }, { 'E', 0 },
            { 'F', 4 }, { 'G', 0 }, { 'H', 0 }, { 'I', 4 }, { 'J', 8 },
            { 'K', 0 }, { 'L', 0 }, { 'M', 0 }, { 'N', 0 }, { 'O', 0 },
            { 'P', 0 }, { 'Q', 0 }, { 'R', 0 }, { 'S', 2 }, { 'T', 0 },
            { 'U', 0 }, { 'V', 0 }, { 'W', 0 }, { 'X', 0 }, { 'Y', 0 },
            { 'Z', 1 },
        };
        bool all_match{ true };
        for (const width_case& w : widths)
        {
            const char buf[2]{ w.c, '\0' };
            if (vmhook::detail::jvm_primitive_byte_width(std::string_view{ buf, 1 }) != w.expected)
            {
                all_match = false;
            }
        }
        check("byte_width_every_single_uppercase_letter_per_table", all_match);
    }

    // ---- jvm_primitive_byte_width: lowercase primitives are NOT accepted -----
    // Case-sensitive: 'i','j','z','d',... all return 0.
    {
        const char lowers[]{ 'i', 'j', 'z', 'b', 's', 'c', 'f', 'd' };
        bool all_zero{ true };
        for (const char c : lowers)
        {
            const char buf[2]{ c, '\0' };
            if (vmhook::detail::jvm_primitive_byte_width(std::string_view{ buf, 1 }) != 0)
            {
                all_zero = false;
            }
        }
        check("byte_width_lowercase_primitive_letters_are_0", all_zero);
    }

    // ---- jvm_primitive_byte_width vs sig_char_to_basic_type consistency ------
    // For every single-char primitive descriptor, a non-zero width implies the
    // basic-type classifier recognises the same letter as a primitive (basic
    // type in [4..11], i.e. NOT the T_OBJECT(12)/T_ARRAY(13)/T_VOID(14) bucket).
    {
        const char prims[]{ 'Z', 'B', 'S', 'C', 'I', 'F', 'J', 'D' };
        bool consistent{ true };
        for (const char c : prims)
        {
            const char buf[2]{ c, '\0' };
            const std::size_t w{ vmhook::detail::jvm_primitive_byte_width(std::string_view{ buf, 1 }) };
            const int bt{ vmhook::detail::sig_char_to_basic_type(c) };
            if (!(w != 0 && bt >= 4 && bt <= 11)) { consistent = false; }
        }
        check("byte_width_and_basic_type_agree_on_primitives", consistent);
    }

    // ---- jni_signature_for_arg<T>: more integral-width boundary types --------
    // `short`/`unsigned short` are exactly the 16-bit fixed-width typedefs on
    // every CI target (int16_t==short, uint16_t==unsigned short with no padding
    // bits), so they take the named 16-bit branches: signed short -> "S",
    // unsigned short -> "C" (the unsigned-16 = Java char split).  (Deliberately
    // NOT testing `long`/`long long` here: whether int64_t aliases `long`
    // (LP64) or `long long` (LLP64) is platform-dependent, so `long long`'s
    // mapping is not portable enough to pin a fixed expected value.)
    check("jni_sig_short_is_S",
          vmhook::detail::jni_signature_for_arg<short>() == "S");
    check("jni_sig_unsigned_short_is_C",
          vmhook::detail::jni_signature_for_arg<unsigned short>() == "C");
    // signed char is the int8_t typedef on every CI target -> "B".
    check("jni_sig_signed_char_is_B",
          vmhook::detail::jni_signature_for_arg<signed char>() == "B");
    // cv/ref stripping also applies to volatile and rvalue refs.
    check("jni_sig_strips_volatile_on_int",
          vmhook::detail::jni_signature_for_arg<volatile int>() == "I");
    check("jni_sig_strips_rvalue_ref_on_float",
          vmhook::detail::jni_signature_for_arg<float&&>() == "F");
    check("jni_sig_strips_const_on_int64",
          vmhook::detail::jni_signature_for_arg<const std::int64_t>() == "J");

    // ---- return-descriptor extraction: more well-formed return types --------
    // Every primitive return letter, plus the recognised non-primitive markers,
    // routed through the exact rfind(')')+1 path call() uses.  (We never feed a
    // signature whose ')' is the final char: signature[rparen+1] would index
    // string_view::size(), which is UB on operator[] for string_view.)
    check("return_type_char_method_is_5",   return_basic_type_of("()C")  == 5);
    check("return_type_float_method_is_6",  return_basic_type_of("()F")  == 6);
    check("return_type_double_method_is_7", return_basic_type_of("()D")  == 7);
    check("return_type_byte_method_is_8",   return_basic_type_of("()B")  == 8);
    check("return_type_short_method_is_9",  return_basic_type_of("()S")  == 9);
    // A multi-dimensional array return is still classified by its FIRST char
    // after ')', which is '[' -> T_ARRAY (13).
    check("return_type_2d_array_method_is_13",
          return_basic_type_of("()[[I") == 13);
    check("return_type_object_array_method_is_13",
          return_basic_type_of("(I)[Ljava/lang/String;") == 13);
    // rfind picks the LAST ')': an object-typed parameter list with many ')'
    // is irrelevant; the governing close-paren is the final one, and the byte
    // after it ('I') is the return.
    check("return_type_many_params_uses_last_paren_is_10",
          return_basic_type_of("(IJLjava/lang/Object;)I") == 10);
    // A return whose letter after ')' is unrecognised (here lowercase 'i')
    // degrades to T_OBJECT (12) rather than asserting.
    check("return_type_unrecognised_letter_after_paren_is_object_12",
          return_basic_type_of("(I)i") == 12);
    // A bare ')' followed immediately by another ')' then a real letter: the
    // LAST ')' governs, return letter 'V' -> 14.
    check("return_type_double_paren_then_void_is_14",
          return_basic_type_of("(()))V") == 14);
    // No paren at all, in various junk shapes -> void fallback (14).
    check("return_type_single_letter_no_paren_is_void_14",
          return_basic_type_of("I") == 14);
    check("return_type_whitespace_no_paren_is_void_14",
          return_basic_type_of("   ") == 14);

    return failures == 0 ? 0 : 1;
}
