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
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <typeinfo>
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

// Mirror of the constructor-signature build inside vmhook::jni_make_unique
// (vmhook.hpp ~10989-10991): for an argument pack <args...> the JNI <init>
// descriptor is "(" + concat(jni_signature_for_arg<remove_cvref_t<args>>()...)
// + ")V".  This is the EXACT fold the library feeds to GetMethodID('<init>'),
// so reproducing it here proves the per-arg descriptors compose into the right
// whole-constructor signature (the empty pack => "()V", a long/double arg still
// contributes exactly one descriptor token, the uint16->C split is visible
// end-to-end, etc.).  No JVM is touched — it is pure string assembly over the
// compile-time jni_signature_for_arg table.
template<typename... args_t>
static auto ctor_signature_of() -> std::string
{
    std::string signature{ "(" };
    ((signature += vmhook::detail::jni_signature_for_arg<std::remove_cvref_t<args_t>>()), ...);
    signature += ")V";
    return signature;
}

// A minimal registered/registerable wrapper, following the exact pattern used by
// tests/test_object_factory.cpp and tests/test_api_surface*.cpp: derive from
// vmhook::object<T> with the required `explicit T(vmhook::oop_t)` constructor.
// jni_signature_for_arg<unique_ptr<sig_wrapper>>() resolves its descriptor from
// vmhook::type_to_class_map (an inline std::unordered_map populated by
// register_class<T>()).  register_class needs a live JVM (its find_class probe
// fails with no VM), so for this pure no-JVM test we write the map entry
// DIRECTLY — byte-for-byte what register_class does internally
// (type_to_class_map.insert_or_assign(type_index, name)) — to simulate the
// "registered" state without a JVM.
class sig_wrapper : public vmhook::object<sig_wrapper>
{
public:
    explicit sig_wrapper(vmhook::oop_t oop) noexcept
        : vmhook::object<sig_wrapper>{ oop }
    {
    }
};

// A second wrapper that is deliberately NEVER inserted into type_to_class_map,
// so jni_signature_for_arg falls back to "Ljava/lang/Object;" (flaw #5: a
// compilable-but-wrong descriptor for an unregistered wrapper).
class sig_wrapper_unregistered : public vmhook::object<sig_wrapper_unregistered>
{
public:
    explicit sig_wrapper_unregistered(vmhook::oop_t oop) noexcept
        : vmhook::object<sig_wrapper_unregistered>{ oop }
    {
    }
};

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

    // =====================================================================
    // EXHAUSTIVE PASS 2 — total byte/char sweeps, every-input boundaries,
    // the platform-sensitive integral matrix, the class-map fallback, and
    // the full constructor-signature assembly.  Every expected value is
    // derived directly from the confirmed tables in vmhook.hpp:
    //   sig_char_to_basic_type  (vmhook.hpp:12937 — Z4 C5 F6 D7 B8 S9 I10
    //                            J11 L12 [13 V14, default 12)
    //   jvm_primitive_byte_width(vmhook.hpp:12972 — size!=1 ->0; Z/B1 S/C2
    //                            I/F4 J/D8, default 0)
    //   jni_signature_for_arg   (vmhook.hpp:10530 — decay; String/Z/B/S/C/J/
    //                            F/D, generic is_integral&&sizeof==4 ->I,
    //                            unique_ptr<wrapper> & object_base -> class
    //                            map L...; (Object fallback), else assert)
    // =====================================================================

    // ---- sig_char_to_basic_type: FULL 0..255 byte sweep ---------------------
    // The single source of truth for "which bytes are recognised".  Exactly the
    // 11 descriptor chars Z C F D B S I J L [ V yield their table value; EVERY
    // other byte in the entire 0..255 range yields the T_OBJECT fallback (12).
    // The parameter is a plain `char`: on platforms where `char` is signed
    // (MSVC/MinGW) bytes 0x80..0xFF arrive as NEGATIVE ints, so this also proves
    // the switch routes high/negative bytes to `default` with no signed-char
    // surprise and no stray case label anywhere in the 256-value domain.
    {
        auto expected_basic_type = [](int byte) -> int
        {
            switch (byte)
            {
            case 'Z': return 4;
            case 'C': return 5;
            case 'F': return 6;
            case 'D': return 7;
            case 'B': return 8;
            case 'S': return 9;
            case 'I': return 10;
            case 'J': return 11;
            case 'L': return 12;
            case '[': return 13;
            case 'V': return 14;
            default:  return 12;
            }
        };
        bool whole_byte_range_matches{ true };
        for (int byte{ 0 }; byte <= 0xFF; ++byte)
        {
            const int got{ vmhook::detail::sig_char_to_basic_type(static_cast<char>(byte)) };
            if (got != expected_basic_type(byte))
            {
                whole_byte_range_matches = false;
            }
        }
        check("sig_char_full_0_to_255_byte_sweep_matches_table", whole_byte_range_matches);
        // Independent count: exactly 10 bytes in 0..255 are non-default (i.e.
        // classify as something other than 12) — Z C F D B S I J [ V — and that
        // is precisely the 8 primitives + '[' + 'V' (note 'L' is itself 12, so
        // it is NOT counted here even though it is a "recognised" char — this
        // asserts the *distinct-from-fallback* set).
        int non_object_count{ 0 };
        for (int byte{ 0 }; byte <= 0xFF; ++byte)
        {
            if (vmhook::detail::sig_char_to_basic_type(static_cast<char>(byte)) != 12)
            {
                ++non_object_count;
            }
        }
        check("sig_char_exactly_10_bytes_classify_non_object", non_object_count == 10);
    }
    // The high-byte / signed-char corners called out explicitly (these are the
    // bytes that arrive negative under a signed `char`): 0x80, 0xC0, 0xFF.
    check("sig_char_high_byte_0x80_is_object_12",
          vmhook::detail::sig_char_to_basic_type(static_cast<char>(0x80)) == 12);
    check("sig_char_high_byte_0xC0_is_object_12",
          vmhook::detail::sig_char_to_basic_type(static_cast<char>(0xC0)) == 12);
    check("sig_char_high_byte_0xFF_is_object_12",
          vmhook::detail::sig_char_to_basic_type(static_cast<char>(0xFF)) == 12);

    // ---- jvm_primitive_byte_width: FULL 0..255 single-byte sweep ------------
    // For a length-1 descriptor, exactly Z/B->1, S/C->2, I/F->4, J/D->8; every
    // other single byte (all 248 of them, including 'L','V','[' and all high /
    // negative bytes) -> 0.  Proves the inner switch has no stray width label.
    {
        auto expected_width = [](int byte) -> std::size_t
        {
            switch (byte)
            {
            case 'Z': case 'B': return 1;
            case 'S': case 'C': return 2;
            case 'I': case 'F': return 4;
            case 'J': case 'D': return 8;
            default:            return 0;
            }
        };
        bool all_single_bytes_match{ true };
        for (int byte{ 0 }; byte <= 0xFF; ++byte)
        {
            const char one[1]{ static_cast<char>(byte) };
            const std::size_t got{ vmhook::detail::jvm_primitive_byte_width(std::string_view{ one, 1 }) };
            if (got != expected_width(byte)) { all_single_bytes_match = false; }
        }
        check("byte_width_full_0_to_255_single_byte_sweep_matches_table", all_single_bytes_match);
        // Exactly 8 single bytes have a non-zero width.
        int nonzero_width_count{ 0 };
        for (int byte{ 0 }; byte <= 0xFF; ++byte)
        {
            const char one[1]{ static_cast<char>(byte) };
            if (vmhook::detail::jvm_primitive_byte_width(std::string_view{ one, 1 }) != 0)
            {
                ++nonzero_width_count;
            }
        }
        check("byte_width_exactly_8_single_bytes_have_width", nonzero_width_count == 8);
    }

    // ---- jvm_primitive_byte_width: the size()!=1 length gate, exhaustively ---
    // The gate is checked BEFORE the first byte, so length 0 and any length>=2
    // is 0 regardless of content — even content that begins with a valid
    // primitive letter.  Sweep lengths 0..16 of repeated 'I' (a real primitive
    // letter) to prove only length==1 ever yields a width.
    {
        bool only_len1_has_width{ true };
        std::string many_i;
        for (std::size_t len{ 0 }; len <= 16; ++len)
        {
            const std::size_t got{ vmhook::detail::jvm_primitive_byte_width(std::string_view{ many_i }) };
            const std::size_t want{ (len == 1) ? static_cast<std::size_t>(4) : static_cast<std::size_t>(0) };
            if (got != want) { only_len1_has_width = false; }
            many_i.push_back('I');
        }
        check("byte_width_only_length_one_of_repeated_I_has_width", only_len1_has_width);
    }
    // Adversarial widths called out individually: an embedded-NUL view of size 2
    // ("I\0") must be 0 (the NUL does not shorten a string_view; size()==2 gates
    // it), a high-byte single char is 0, and a 2-byte all-high view is 0.
    check("byte_width_I_then_nul_size2_is_0",
          vmhook::detail::jvm_primitive_byte_width(std::string_view{ "I\0", 2 }) == 0);
    check("byte_width_nul_then_I_size2_is_0",
          vmhook::detail::jvm_primitive_byte_width(std::string_view{ "\0I", 2 }) == 0);
    {
        const char hi[1]{ static_cast<char>(0xFF) };
        check("byte_width_single_high_byte_0xFF_is_0",
              vmhook::detail::jvm_primitive_byte_width(std::string_view{ hi, 1 }) == 0);
    }
    // The classic JVM primitive *and* the wide forms together (already covered
    // singly above, re-pinned here as one explicit "every primitive code" row
    // so the canonical {1,2,4,8} mapping is greppable in one place).
    check("byte_width_all_eight_primitive_codes_canonical",
             vmhook::detail::jvm_primitive_byte_width("Z") == 1
          && vmhook::detail::jvm_primitive_byte_width("B") == 1
          && vmhook::detail::jvm_primitive_byte_width("S") == 2
          && vmhook::detail::jvm_primitive_byte_width("C") == 2
          && vmhook::detail::jvm_primitive_byte_width("I") == 4
          && vmhook::detail::jvm_primitive_byte_width("F") == 4
          && vmhook::detail::jvm_primitive_byte_width("J") == 8
          && vmhook::detail::jvm_primitive_byte_width("D") == 8);

    // ---- jvm_primitive_byte_width vs return-slot count cross-check -----------
    // The JVM gives long (J) and double (D) TWO local/stack slots and every
    // other type ONE.  vmhook has no public slot-counter, but the 8-byte width
    // reported here is exactly the set {J,D} that is two-slot, so we assert the
    // partition: width==8  <=>  two-slot type, for all eight primitive codes.
    {
        struct prim_slot { const char* code; std::size_t width; int slots; };
        const prim_slot table[]{
            { "Z", 1, 1 }, { "B", 1, 1 }, { "S", 2, 1 }, { "C", 2, 1 },
            { "I", 4, 1 }, { "F", 4, 1 }, { "J", 8, 2 }, { "D", 8, 2 },
        };
        bool partition_holds{ true };
        for (const prim_slot& p : table)
        {
            const std::size_t w{ vmhook::detail::jvm_primitive_byte_width(p.code) };
            const bool two_slot{ p.slots == 2 };
            if (w != p.width) { partition_holds = false; }
            if ((w == 8) != two_slot) { partition_holds = false; }
        }
        check("byte_width_eq8_iff_two_slot_long_or_double", partition_holds);
    }

    // ---- jni_signature_for_arg<T>: the full fixed-width integral row ---------
    // Re-pinned together so the complete fixed-width contract is greppable in
    // one place and a single-row drift fails loudly.
    check("jni_sig_matrix_fixed_width_integrals",
             vmhook::detail::jni_signature_for_arg<bool>() == "Z"
          && vmhook::detail::jni_signature_for_arg<std::int8_t>() == "B"
          && vmhook::detail::jni_signature_for_arg<std::uint8_t>() == "B"
          && vmhook::detail::jni_signature_for_arg<std::int16_t>() == "S"
          && vmhook::detail::jni_signature_for_arg<std::uint16_t>() == "C"
          && vmhook::detail::jni_signature_for_arg<std::int32_t>() == "I"
          && vmhook::detail::jni_signature_for_arg<std::uint32_t>() == "I"
          && vmhook::detail::jni_signature_for_arg<std::int64_t>() == "J"
          && vmhook::detail::jni_signature_for_arg<std::uint64_t>() == "J"
          && vmhook::detail::jni_signature_for_arg<float>() == "F"
          && vmhook::detail::jni_signature_for_arg<double>() == "D");

    // ---- jni_signature_for_arg<T>: the uint16->C asymmetry, pinned hard ------
    // The single most surprising row: signed 16-bit -> Java short (S), but
    // UNSIGNED 16-bit -> Java char (C).  Round-trip self-consistency: the 'C'
    // this emits is classified back as T_CHAR(5) / width 2 by the other two
    // helpers, while 'S' is T_SHORT(9) / width 2.  Pinning all of it together
    // makes any future "unify uint16 to S" change a conscious, visible break.
    check("jni_sig_int16_S_uint16_C_are_distinct",
             vmhook::detail::jni_signature_for_arg<std::int16_t>() == "S"
          && vmhook::detail::jni_signature_for_arg<std::uint16_t>() == "C"
          && vmhook::detail::jni_signature_for_arg<std::int16_t>()
                 != vmhook::detail::jni_signature_for_arg<std::uint16_t>());
    check("jni_sig_uint16_C_roundtrips_to_T_CHAR_5",
          vmhook::detail::sig_char_to_basic_type(
              vmhook::detail::jni_signature_for_arg<std::uint16_t>()[0]) == 5);
    check("jni_sig_int16_S_roundtrips_to_T_SHORT_9",
          vmhook::detail::sig_char_to_basic_type(
              vmhook::detail::jni_signature_for_arg<std::int16_t>()[0]) == 9);

    // ---- jni_signature_for_arg<T>: every emitted primitive descriptor round-
    //      trips through sig_char_to_basic_type to a non-fallback basic type ---
    // For each C++ primitive arg type, the single-char descriptor it emits is
    // recognised by the classifier as a genuine primitive (basic type in 4..11),
    // i.e. NONE of them degrade to the T_OBJECT(12) fallback.  This couples the
    // emit side and the classify side across the whole primitive set.
    {
        struct emitted { std::string sig; int basic; };
        const emitted rows[]{
            { vmhook::detail::jni_signature_for_arg<bool>(),          4 },  // Z
            { vmhook::detail::jni_signature_for_arg<std::uint16_t>(), 5 },  // C
            { vmhook::detail::jni_signature_for_arg<float>(),         6 },  // F
            { vmhook::detail::jni_signature_for_arg<double>(),        7 },  // D
            { vmhook::detail::jni_signature_for_arg<std::int8_t>(),   8 },  // B
            { vmhook::detail::jni_signature_for_arg<std::int16_t>(),  9 },  // S
            { vmhook::detail::jni_signature_for_arg<std::int32_t>(), 10 },  // I
            { vmhook::detail::jni_signature_for_arg<std::int64_t>(), 11 },  // J
        };
        bool all_roundtrip{ true };
        for (const emitted& r : rows)
        {
            if (r.sig.size() != 1) { all_roundtrip = false; continue; }
            if (vmhook::detail::sig_char_to_basic_type(r.sig[0]) != r.basic) { all_roundtrip = false; }
        }
        check("jni_sig_every_primitive_descriptor_roundtrips_to_its_basic_type", all_roundtrip);
    }

    // ---- jni_signature_for_arg<T>: cv/ref/pointer-decay matrix --------------
    // std::decay_t strips top-level cv and reference; the four String-mapped
    // types (std::string, std::string_view, const char*, char*) and the
    // primitive rows must all survive heavy qualification unchanged.
    check("jni_sig_const_volatile_ref_double_is_D",
          vmhook::detail::jni_signature_for_arg<const volatile double&>() == "D");
    check("jni_sig_rvalue_ref_int64_is_J",
          vmhook::detail::jni_signature_for_arg<std::int64_t&&>() == "J");
    check("jni_sig_const_ref_string_view_is_String",
          vmhook::detail::jni_signature_for_arg<const std::string_view&>() == "Ljava/lang/String;");
    check("jni_sig_const_ref_uint16_is_C",
          vmhook::detail::jni_signature_for_arg<const std::uint16_t&>() == "C");
    check("jni_sig_volatile_bool_is_Z",
          vmhook::detail::jni_signature_for_arg<volatile bool>() == "Z");
    // `const char* const&` and `char* const` both decay to a String-mapped
    // pointer.  (decay removes the reference and the top-level const.)
    check("jni_sig_const_char_ptr_const_ref_is_String",
          vmhook::detail::jni_signature_for_arg<const char* const&>() == "Ljava/lang/String;");

    // ---- jni_signature_for_arg<T>: platform-sized integral routing ----------
    // `long`, `long long`, `size_t`, `ptrdiff_t`, `char32_t`, `wchar_t` have
    // platform-dependent sizes / typedef identities, and the helper has NO
    // explicit branch for them — they reach either the explicit int64 branch
    // (sizeof==8 AND the type IS int64_t/uint64_t), the generic
    // `is_integral && sizeof==4 -> I` branch, or the hard static_assert (which
    // would FAIL TO COMPILE).  We therefore gate every instantiation behind a
    // compile-time predicate that is true ONLY when the type lands on a
    // *compilable* branch, and assert the value the table dictates for THIS
    // platform.  This makes the matrix exhaustive yet green on LP64 (Linux/mac)
    // and LLP64 (Windows) alike.

    // signed/unsigned `long`: compiles iff it is int64_t/uint64_t (LP64 -> "J")
    // OR sizeof==4 (LLP64 -> generic int32 branch -> "I").
    if constexpr (std::is_same_v<long, std::int64_t>)
    {
        check("jni_sig_long_is_J_on_LP64",
              vmhook::detail::jni_signature_for_arg<long>() == "J");
    }
    else if constexpr (std::is_integral_v<long> && sizeof(long) == sizeof(std::int32_t))
    {
        check("jni_sig_long_is_I_on_LLP64",
              vmhook::detail::jni_signature_for_arg<long>() == "I");
    }
    if constexpr (std::is_same_v<unsigned long, std::uint64_t>)
    {
        check("jni_sig_ulong_is_J_on_LP64",
              vmhook::detail::jni_signature_for_arg<unsigned long>() == "J");
    }
    else if constexpr (std::is_integral_v<unsigned long> && sizeof(unsigned long) == sizeof(std::int32_t))
    {
        check("jni_sig_ulong_is_I_on_LLP64",
              vmhook::detail::jni_signature_for_arg<unsigned long>() == "I");
    }

    // `long long` / `unsigned long long`: 8 bytes everywhere, but only COMPILE
    // when they alias int64_t/uint64_t (true on LLP64; on LP64 int64_t is
    // `long`, so `long long` would hit the static_assert and is intentionally
    // NOT instantiated there).
    if constexpr (std::is_same_v<long long, std::int64_t>)
    {
        check("jni_sig_long_long_is_J",
              vmhook::detail::jni_signature_for_arg<long long>() == "J");
    }
    if constexpr (std::is_same_v<unsigned long long, std::uint64_t>)
    {
        check("jni_sig_ulong_long_is_J",
              vmhook::detail::jni_signature_for_arg<unsigned long long>() == "J");
    }

    // size_t / ptrdiff_t: route by their concrete typedef.  On 64-bit they
    // alias the 64-bit fixed-width types (-> "J"); on a 32-bit build they are
    // 4-byte integrals (-> "I").  Guard both arms.
    if constexpr (std::is_same_v<std::size_t, std::uint64_t>)
    {
        check("jni_sig_size_t_is_J_on_64bit",
              vmhook::detail::jni_signature_for_arg<std::size_t>() == "J");
    }
    else if constexpr (std::is_integral_v<std::size_t> && sizeof(std::size_t) == sizeof(std::int32_t))
    {
        check("jni_sig_size_t_is_I_on_32bit",
              vmhook::detail::jni_signature_for_arg<std::size_t>() == "I");
    }
    if constexpr (std::is_same_v<std::ptrdiff_t, std::int64_t>)
    {
        check("jni_sig_ptrdiff_t_is_J_on_64bit",
              vmhook::detail::jni_signature_for_arg<std::ptrdiff_t>() == "J");
    }
    else if constexpr (std::is_integral_v<std::ptrdiff_t> && sizeof(std::ptrdiff_t) == sizeof(std::int32_t))
    {
        check("jni_sig_ptrdiff_t_is_I_on_32bit",
              vmhook::detail::jni_signature_for_arg<std::ptrdiff_t>() == "I");
    }

    // char32_t: always a 4-byte integral, NOT uint16_t, so it falls to the
    // generic `is_integral && sizeof==4 -> I` branch (it does NOT become 'C').
    // This pins that the unsigned-16 split is keyed on the EXACT std::uint16_t
    // type, not on "any unsigned char-ish type".
    check("jni_sig_char32_t_is_I_not_C",
          vmhook::detail::jni_signature_for_arg<char32_t>() == "I");
    static_assert(!std::is_same_v<std::decay_t<char32_t>, std::uint16_t>,
                  "char32_t must not alias uint16_t (else the C-split test is meaningless)");

    // char16_t: a 2-byte integral that is a DISTINCT type from uint16_t, with
    // no explicit branch and sizeof!=4, so jni_signature_for_arg<char16_t>()
    // would hit the hard static_assert and FAIL TO COMPILE.  We cannot probe a
    // static_assert at runtime, so instead we pin the *property the helper keys
    // on*: char16_t is NOT std::uint16_t (so it does not steal the 'C' branch)
    // and its size is not 4 (so it does not reach the generic 'I' branch),
    // which is exactly why it is rejected.  (Same rationale the file header
    // gives for not instantiating unsupported types.)
    static_assert(!std::is_same_v<std::decay_t<char16_t>, std::uint16_t>,
                  "char16_t must be a distinct type from uint16_t");
    check("jni_sig_char16_t_is_distinct_from_uint16_t_property",
          (!std::is_same_v<std::decay_t<char16_t>, std::uint16_t>)
          && sizeof(char16_t) != sizeof(std::int32_t));

    // wchar_t: platform-dependent.  Where sizeof(wchar_t)==4 (Linux/macOS) it is
    // a 4-byte integral and routes to "I"; where sizeof==2 (Windows) it has no
    // branch and sizeof!=4, so it would FAIL TO COMPILE — instantiate it ONLY in
    // the 4-byte case.
    if constexpr (std::is_integral_v<wchar_t> && sizeof(wchar_t) == sizeof(std::int32_t))
    {
        check("jni_sig_wchar_t_is_I_where_4_bytes",
              vmhook::detail::jni_signature_for_arg<wchar_t>() == "I");
    }
    else
    {
        // 2-byte wchar_t (Windows): assert the rejecting property rather than
        // instantiating the static_assert branch.
        check("jni_sig_wchar_t_2byte_would_not_compile_property",
              !std::is_same_v<std::decay_t<wchar_t>, std::uint16_t>
              && sizeof(wchar_t) != sizeof(std::int32_t));
    }

    // ---- enums do NOT satisfy is_integral_v (so they hit the static_assert) --
    // A scoped `enum class : int` and an unscoped `enum : long` both have
    // integer-ish underlying types, but std::is_integral_v<enum> is FALSE, so
    // neither matches any integral branch and both reach the hard static_assert
    // (compile error) — which is precisely why instantiating
    // jni_signature_for_arg<MyEnum>() is forbidden here.  We pin the trait that
    // routes them there.  (This guards the "every wrapper/64-bit/unknown arg
    // used to silently mis-encode as I" regression the static_assert prevents.)
    {
        enum class scoped_enum_i : int { a, b };
        enum unscoped_enum_l : long { x, y };
        check("enum_class_int_is_not_integral",  !std::is_integral_v<scoped_enum_i>);
        check("enum_unscoped_long_is_not_integral", !std::is_integral_v<unscoped_enum_l>);
        // ...and they are not object_base-derived / unique_ptr either, so the
        // ONLY branch left for them is the static_assert.
        check("enum_class_is_not_object_base_derived",
              !std::is_base_of_v<vmhook::object_base, scoped_enum_i>);
        check("enum_class_is_not_unique_ptr",
              !vmhook::detail::is_unique_ptr_v<scoped_enum_i>);
    }

    // ---- public jni::signature_for_arg == detail::jni_signature_for_arg ------
    // The public re-export vmhook::jni::signature_for_arg<T> (vmhook.hpp:11219)
    // forwards verbatim to detail::jni_signature_for_arg<T>; assert byte-
    // identical output across a representative spread so the two entry points
    // can never diverge.
    check("signature_for_arg_parity_string",
          vmhook::jni::signature_for_arg<std::string>()
              == vmhook::detail::jni_signature_for_arg<std::string>());
    check("signature_for_arg_parity_bool",
          vmhook::jni::signature_for_arg<bool>()
              == vmhook::detail::jni_signature_for_arg<bool>());
    check("signature_for_arg_parity_uint16_C",
          vmhook::jni::signature_for_arg<std::uint16_t>()
              == vmhook::detail::jni_signature_for_arg<std::uint16_t>());
    check("signature_for_arg_parity_uint16_value_is_C",
          vmhook::jni::signature_for_arg<std::uint16_t>() == "C");
    check("signature_for_arg_parity_int64_J",
          vmhook::jni::signature_for_arg<std::int64_t>()
              == vmhook::detail::jni_signature_for_arg<std::int64_t>());
    check("signature_for_arg_parity_double_D",
          vmhook::jni::signature_for_arg<double>()
              == vmhook::detail::jni_signature_for_arg<double>());
    check("signature_for_arg_parity_const_char_ptr_String",
          vmhook::jni::signature_for_arg<const char*>()
              == vmhook::detail::jni_signature_for_arg<const char*>());

    // ---- jni_signature_for_arg<T>: class-map wrapper resolution (flaw #5) ----
    // Populate type_to_class_map DIRECTLY (no JVM) exactly as register_class
    // does internally, then assert the wrapper resolves to `Lcom/example/Sig;`.
    // An UNregistered wrapper falls back to the compilable-but-wrong
    // `Ljava/lang/Object;` (the honest hazard flaw #5 documents).
    {
        // Pre-condition: neither wrapper is registered yet, so BOTH currently
        // fall back to Ljava/lang/Object; .
        check("jni_sig_unregistered_wrapper_uniqueptr_falls_back_to_Object_pre",
              vmhook::detail::jni_signature_for_arg<std::unique_ptr<sig_wrapper>>()
                  == "Ljava/lang/Object;");
        check("jni_sig_unregistered_wrapper_value_falls_back_to_Object_pre",
              vmhook::detail::jni_signature_for_arg<sig_wrapper>()
                  == "Ljava/lang/Object;");

        // Register sig_wrapper by writing the map entry directly (this is the
        // precise mutation register_class<sig_wrapper>("com/example/Sig") makes
        // to type_to_class_map; the only extra thing register_class does is the
        // find_class JVM probe + the factory map, neither of which
        // jni_signature_for_arg reads).
        vmhook::type_to_class_map.insert_or_assign(
            std::type_index{ typeid(sig_wrapper) }, std::string{ "com/example/Sig" });

        // unique_ptr<wrapper> branch: "L" + class + ";".
        check("jni_sig_registered_wrapper_uniqueptr_is_Lname",
              vmhook::detail::jni_signature_for_arg<std::unique_ptr<sig_wrapper>>()
                  == "Lcom/example/Sig;");
        // by-value object_base-derived branch resolves identically.
        check("jni_sig_registered_wrapper_value_is_Lname",
              vmhook::detail::jni_signature_for_arg<sig_wrapper>()
                  == "Lcom/example/Sig;");
        // cv/ref qualification on the wrapper still resolves through decay.
        check("jni_sig_registered_wrapper_const_ref_value_is_Lname",
              vmhook::detail::jni_signature_for_arg<const sig_wrapper&>()
                  == "Lcom/example/Sig;");
        check("jni_sig_registered_wrapper_const_ref_uniqueptr_is_Lname",
              vmhook::detail::jni_signature_for_arg<const std::unique_ptr<sig_wrapper>&>()
                  == "Lcom/example/Sig;");

        // The OTHER wrapper is still unregistered -> still the Object fallback,
        // proving the lookup is keyed per-type and the registration above did
        // not accidentally satisfy an unrelated type.
        check("jni_sig_other_wrapper_uniqueptr_still_Object_fallback",
              vmhook::detail::jni_signature_for_arg<std::unique_ptr<sig_wrapper_unregistered>>()
                  == "Ljava/lang/Object;");
        check("jni_sig_other_wrapper_value_still_Object_fallback",
              vmhook::detail::jni_signature_for_arg<sig_wrapper_unregistered>()
                  == "Ljava/lang/Object;");

        // A single-segment ("default package") class name builds the L...; form
        // with no '/' — covers single-char / unqualified names end-to-end.
        vmhook::type_to_class_map.insert_or_assign(
            std::type_index{ typeid(sig_wrapper) }, std::string{ "X" });
        check("jni_sig_registered_wrapper_single_segment_name_is_LX",
              vmhook::detail::jni_signature_for_arg<sig_wrapper>() == "LX;");
        // A deeply nested name with an inner-class '$' segment round-trips
        // verbatim into the descriptor.
        vmhook::type_to_class_map.insert_or_assign(
            std::type_index{ typeid(sig_wrapper) },
            std::string{ "a/b/c/d/e/Outer$Inner" });
        check("jni_sig_registered_wrapper_nested_inner_class_name",
              vmhook::detail::jni_signature_for_arg<sig_wrapper>()
                  == "La/b/c/d/e/Outer$Inner;");

        // Clean up so we leave global state as we found it (defensive — this is
        // the last consumer, but keeps the map honest for any future addition).
        vmhook::type_to_class_map.erase(std::type_index{ typeid(sig_wrapper) });
    }

    // ---- full constructor-signature assembly (mirrors jni_make_unique) -------
    // "(" + concat(per-arg descriptors) + ")V".  Every expected value is the
    // concatenation of the already-pinned per-arg rows.

    // Empty pack -> "()V".
    check("ctor_sig_empty_pack_is_void",
          ctor_signature_of<>() == "()V");
    // One of each primitive (in BasicType order Z C F D B S I J) — note uint16
    // contributes 'C' and uint8 contributes 'B'.
    check("ctor_sig_single_bool_is_ZV",
          ctor_signature_of<bool>() == "(Z)V");
    check("ctor_sig_single_uint16_is_CV",
          ctor_signature_of<std::uint16_t>() == "(C)V");
    check("ctor_sig_single_float_is_FV",
          ctor_signature_of<float>() == "(F)V");
    check("ctor_sig_single_double_is_DV",
          ctor_signature_of<double>() == "(D)V");
    check("ctor_sig_single_int8_is_BV",
          ctor_signature_of<std::int8_t>() == "(B)V");
    check("ctor_sig_single_int16_is_SV",
          ctor_signature_of<std::int16_t>() == "(S)V");
    check("ctor_sig_single_int32_is_IV",
          ctor_signature_of<std::int32_t>() == "(I)V");
    check("ctor_sig_single_int64_is_JV",
          ctor_signature_of<std::int64_t>() == "(J)V");
    check("ctor_sig_single_string_is_StringV",
          ctor_signature_of<std::string>() == "(Ljava/lang/String;)V");
    // The canonical mixed example from the briefing: (int, double, String).
    check("ctor_sig_int_double_string_is_IDLString_V",
          ctor_signature_of<int, double, std::string>()
              == "(IDLjava/lang/String;)V");
    // Two reference (String) params adjacent — concatenated L...; tokens.
    check("ctor_sig_two_strings",
          ctor_signature_of<std::string, const char*>()
              == "(Ljava/lang/String;Ljava/lang/String;)V");
    // long/double argument-slot reality: each long/double still contributes
    // exactly ONE descriptor token (the two-slot-ness is the JVM's concern, not
    // the descriptor's).  (J then D then J).
    check("ctor_sig_long_double_long_tokens",
          ctor_signature_of<std::int64_t, double, std::int64_t>() == "(JDJ)V");
    // A wide "many args, all primitives together" pack: one token per arg, in
    // order — Z B C S I J F D  ->  ZBCSIJFD.
    check("ctor_sig_all_eight_primitives_in_order",
          ctor_signature_of<bool, std::int8_t, std::uint16_t, std::int16_t,
                            std::int32_t, std::int64_t, float, double>()
              == "(ZBCSIJFD)V");
    // cv/ref qualified args are decayed by the fold's remove_cvref_t, so a
    // const-ref / rvalue-ref pack produces the same tokens as the bare pack.
    check("ctor_sig_cvref_args_decay_like_bare",
          ctor_signature_of<const int&, double&&, const std::string&>()
              == "(IDLjava/lang/String;)V");
    // Single uint16 arg again, asserted end-to-end as the briefing requested
    // (so the unsigned-16 split is visible in a whole constructor signature).
    check("ctor_sig_uint16_arg_visible_end_to_end",
          ctor_signature_of<std::uint16_t>() == "(C)V");

    // A constructor pack containing a REGISTERED wrapper arg assembles the
    // L...; class token inline with the primitives.  Register, build, clean up.
    {
        vmhook::type_to_class_map.insert_or_assign(
            std::type_index{ typeid(sig_wrapper) }, std::string{ "com/example/Sig" });
        check("ctor_sig_with_registered_wrapper_arg",
              ctor_signature_of<int, std::unique_ptr<sig_wrapper>, double>()
                  == "(ILcom/example/Sig;D)V");
        vmhook::type_to_class_map.erase(std::type_index{ typeid(sig_wrapper) });
    }

    // ---- return-descriptor extraction: every primitive + non-primitive ------
    // One assertion per return letter routed through the exact rfind(')')+1
    // path, pinning the BasicType for a complete, well-formed signature with a
    // realistic parameter list in front.  (Z C F D B S I J already individually
    // covered above; re-pinned here as one "every well-formed return" sweep over
    // signatures whose params are themselves varied.)
    {
        struct ret_case { const char* sig; int basic; };
        const ret_case rets[]{
            { "(Ljava/lang/String;)Z", 4  },   // boolean
            { "(I)C",                   5  },   // char
            { "(J)F",                   6  },   // float
            { "([B)D",                  7  },   // double
            { "(II)B",                  8  },   // byte
            { "(D)S",                   9  },   // short
            { "(Ljava/lang/Object;)I",  10 },   // int
            { "(FF)J",                  11 },   // long
            { "(I)Ljava/lang/String;",  12 },   // object
            { "(I)[I",                  13 },   // array
            { "()V",                    14 },   // void
        };
        bool all_rets_match{ true };
        for (const ret_case& r : rets)
        {
            if (return_basic_type_of(r.sig) != r.basic) { all_rets_match = false; }
        }
        check("return_type_every_wellformed_return_letter_matches", all_rets_match);
    }
    // Array-return depth is irrelevant: [I, [[I, [[[I, and an object array all
    // classify by the FIRST post-')' char '[' -> T_ARRAY(13).
    check("return_type_1d_prim_array_is_13", return_basic_type_of("()[I")   == 13);
    check("return_type_2d_prim_array_is_13", return_basic_type_of("()[[I")  == 13);
    check("return_type_3d_prim_array_is_13", return_basic_type_of("()[[[I") == 13);
    check("return_type_obj_array_is_13",
          return_basic_type_of("()[Ljava/lang/String;") == 13);

    // ---- return-descriptor extraction: UNKNOWN-return policy (flaw #1) -------
    // PIN THE CURRENT BEHAVIOUR: a malformed / unknown return letter after the
    // last ')' is classified T_OBJECT(12) (the sig_char_to_basic_type default),
    // NOT T_VOID.  This is the documented hazard (flaw #1: the call-stub then
    // trusts a non-object return as an oop).  These assertions lock the present
    // policy so any change to a safer T_VOID(14) mapping is a conscious,
    // test-visible decision rather than a silent regression.
    check("return_type_unknown_letter_Q_is_object_12_FLAW1",
          return_basic_type_of("(I)Q") == 12);
    check("return_type_lowercase_i_after_paren_is_object_12_FLAW1",
          return_basic_type_of("(I)i") == 12);
    check("return_type_digit_after_paren_is_object_12_FLAW1",
          return_basic_type_of("()9") == 12);
    check("return_type_punct_after_paren_is_object_12_FLAW1",
          return_basic_type_of("();") == 12);
    // The 'L' return char itself classifies as T_OBJECT(12) — same value as the
    // unknown fallback, but for a VALID reason (it is a real object marker), so
    // a well-formed object return and a garbage return are indistinguishable at
    // the BasicType level.  Pin both to make that collision explicit.
    check("return_type_valid_object_L_and_garbage_Q_both_12",
          return_basic_type_of("(I)Ljava/lang/String;") == 12
          && return_basic_type_of("(I)Q") == 12);

    // ---- return-descriptor extraction: "last paren wins" stress -------------
    // rfind(')') governs even with many ')' embedded; the byte after the FINAL
    // ')' is the return.  Cover nested-paren junk and a trailing valid letter.
    check("return_type_nested_parens_then_J_is_11",
          return_basic_type_of("(((())))J") == 11);
    check("return_type_paren_soup_then_void_is_14",
          return_basic_type_of("(()())V") == 14);
    check("return_type_balanced_then_object_is_12",
          return_basic_type_of("(I(J)K)Ljava/lang/Object;") == 12);

    // NOTE on flaw #2 (the `signature[rparen+1]` boundary): a signature whose
    // FINAL character is ')' makes rparen+1 == size(), and string_view::
    // operator[](size()) is UB.  We deliberately DO NOT feed such inputs to
    // return_basic_type_of (it mirrors the library's unguarded call site
    // verbatim, so doing so would invoke the same UB here).  The boundary is
    // documented as a library hazard in the final report rather than exercised.
    // Inputs that merely *contain* ')' but do not END in it are safe and are
    // covered above; the npos (no-')') path is covered in pass 1.

    return failures == 0 ? 0 : 1;
}
