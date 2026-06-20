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

// RAW-CLASSIFIER mirror of return-descriptor extraction: find the last ')',
// take the single char immediately after it, and feed it STRAIGHT into
// sig_char_to_basic_type with no validity filtering; when there is no ')' the
// fallback is void ('V').  This reproduces the *historical* (pre-guard)
// method_proxy::call call site and exists to characterise sig_char_to_basic_type
// through that lens — in particular it preserves the OLD policy where an
// unrecognised return letter degrades to T_OBJECT(12) (see the FLAW1 block).
// It is NOT the current library behaviour: the live call site now filters the
// return char through an allow-list and degrades unknowns to T_VOID — that is
// reproduced separately by call_site_result_type() below.  Because this mirror
// is unguarded, callers MUST NOT pass a signature whose final char is ')'
// (signature[rparen+1] would index size(), which is UB on string_view).
static auto return_basic_type_of(std::string_view signature) -> int
{
    const std::size_t rparen{ signature.rfind(')') };
    const char        ret_char{ rparen != std::string_view::npos ? signature[rparen + 1] : 'V' };
    return vmhook::detail::sig_char_to_basic_type(ret_char);
}

// FAITHFUL mirror of the CURRENT return-descriptor decode in method_proxy::call
// (vmhook.hpp:14287-14313) AND method_proxy::call_jni (vmhook.hpp:13646-13660).
// The live code hardens the raw lookup on two fronts, both reproduced verbatim:
//   * BOUNDS: the return char is read only when `rparen + 1 < size()`, so a
//     signature ending in ')' (e.g. "()" / "(I)" / "(Lj/l/String;)") — where
//     rparen+1 == size() — is treated as void with NO out-of-bounds read.  (On
//     string_view, operator[](size()) is UB; std::string's NUL-at-size() does
//     NOT carry through the view, which is exactly why the guard exists.)
//   * VALIDITY: only a real JVM return descriptor letter
//     (Z B C S I J F D | L | [ | V) is forwarded to sig_char_to_basic_type;
//     anything else degrades to T_VOID(14) — a safe no-op return — rather than
//     falling through sig_char_to_basic_type's T_OBJECT(12) default, which would
//     make the call stub decode an arbitrary return register as an oop pointer.
// This is byte-for-byte the `ret_char` / `valid_ret_char` / `result_type` ladder
// the library runs; reproducing it lets the exhaustive sweep below exercise the
// real, guarded policy (the boundary "ends in ')'" cases and the unknown->VOID
// mapping) that the raw mirror above deliberately cannot reach.
static auto call_site_result_type(std::string_view sig) -> int
{
    const std::size_t rparen{ sig.rfind(')') };
    const char ret_char{
        (rparen != std::string_view::npos && rparen + 1 < sig.size())
            ? sig[rparen + 1]
            : 'V' };
    const bool valid_ret_char{
        ret_char == 'Z' || ret_char == 'B' || ret_char == 'C'
        || ret_char == 'S' || ret_char == 'I' || ret_char == 'J'
        || ret_char == 'F' || ret_char == 'D' || ret_char == 'L'
        || ret_char == '[' || ret_char == 'V' };
    return valid_ret_char ? vmhook::detail::sig_char_to_basic_type(ret_char)
                          : 14 /* T_VOID */;
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

// =====================================================================
// WHOLE-DESCRIPTOR REFERENCE WALK (JVMS § 4.3.2 / § 4.3.3).
//
// vmhook does NOT ship a whole-signature parser: its descriptor surface is
// exactly the three table helpers above plus the inline rfind(')')+1 return
// extraction reproduced by return_basic_type_of / call_site_result_type.
// There is no library function that counts arguments, counts JVM local-variable
// slots (with the long/double = 2 rule), or peels an array's dimension from its
// component — so those behaviours cannot be asserted against library code that
// does not exist.  To still make this module "every JVM method/type descriptor
// parse" exhaustive, the helpers below are a SELF-CONTAINED reference walk built
// ONLY on the one library primitive that classifies a leading descriptor byte
// (vmhook::detail::sig_char_to_basic_type).  They encode the canonical JVMS
// grammar so the suite pins the full-parse semantics the task enumerates and so
// any future library parser can be diffed against this reference.
//
// Slot-count note: the JVMS gives `long` (J) and `double` (D) TWO local-variable
// slots and every other type ONE; that is a property of the DESCRIPTOR grammar.
// It is deliberately distinct from how method_proxy::call happens to marshal
// arguments (vmhook.hpp:15877 packs ONE intptr_t params[] entry per C++ value,
// regardless of width) — these helpers report the JVMS slot count, not the
// library's marshalling-slot usage.  Every index read here is bounds-checked
// (`pos < size()`), so a malformed descriptor returns a clean failure and never
// reads past the end.

// Classification of a single (non-method) field descriptor.
struct field_descriptor_parse
{
    bool        ok{ false };       // well-formed and fully consumed
    int         basic_type{ -1 };  // sig_char_to_basic_type of the FIRST byte
    int         array_dims{ 0 };   // count of leading '[' (0 for non-array)
    int         component_basic{ -1 }; // basic type of the innermost component
    std::size_t consumed{ 0 };     // bytes consumed (== whole length when ok)
};

// Walk ONE field descriptor starting at sig[pos].  Recognises:
//   primitive  : Z B C S I J F D                      (one byte)
//   object     : L <name with no ';'> ;               (consume through ';')
//   array      : [+ <one element descriptor>          (any dimension)
// On any malformation (truncated 'L', stray '[' at end, unknown leading byte,
// empty) returns ok=false with consumed = bytes scanned before giving up.  Never
// indexes past size().
static auto parse_one_field_descriptor(std::string_view sig, std::size_t pos)
    -> field_descriptor_parse
{
    field_descriptor_parse out{};
    const std::size_t start{ pos };
    int dims{ 0 };
    while (pos < sig.size() && sig[pos] == '[')
    {
        ++dims;
        ++pos;
    }
    if (pos >= sig.size())
    {
        // '[' with nothing following it -> malformed; report bytes scanned.
        out.array_dims = dims;
        out.consumed   = pos - start;
        return out;
    }
    const char lead{ sig[pos] };
    const int  lead_basic{ vmhook::detail::sig_char_to_basic_type(lead) };
    if (lead == 'L')
    {
        // Object: scan to the terminating ';'.  Unterminated 'L' is malformed.
        std::size_t scan{ pos + 1 };
        while (scan < sig.size() && sig[scan] != ';')
        {
            ++scan;
        }
        if (scan >= sig.size())
        {
            // No ';' before end -> unterminated object descriptor.
            out.array_dims = dims;
            out.consumed   = sig.size() - start;
            return out;
        }
        out.ok              = true;
        out.basic_type      = dims > 0 ? 13 /* T_ARRAY */ : 12 /* T_OBJECT */;
        out.array_dims      = dims;
        out.component_basic = 12; // object component
        out.consumed        = (scan + 1) - start; // include the ';'
        return out;
    }
    // Primitive component?  Exactly the eight letters whose width != 0, i.e. the
    // basic types in [4..11] EXCEPT 'L'(12)/'['(13)/'V'(14).  We reuse the width
    // helper as the authoritative "is this a primitive letter" oracle so the
    // reference walk stays coupled to the library tables.
    {
        const char one[1]{ lead };
        const bool is_primitive{
            vmhook::detail::jvm_primitive_byte_width(std::string_view{ one, 1 }) != 0 };
        if (is_primitive)
        {
            out.ok              = true;
            out.basic_type      = dims > 0 ? 13 /* T_ARRAY */ : lead_basic;
            out.array_dims      = dims;
            out.component_basic = lead_basic;
            out.consumed        = (pos + 1) - start;
            return out;
        }
    }
    // Anything else (V, unknown byte, ']' etc.) is not a valid field descriptor
    // leading byte.  Report failure with the bytes scanned so far.
    out.array_dims = dims;
    out.consumed   = (pos + 1) - start;
    return out;
}

// Classification of a whole method descriptor "( params ) ret".
struct method_descriptor_parse
{
    bool ok{ false };       // well-formed: balanced '(' ')' + valid params + ret
    int  arg_count{ 0 };    // number of parameter descriptors
    int  arg_slots{ 0 };    // JVM local slots: long/double = 2, else 1
    int  return_basic{ -1 }; // basic type of the return descriptor
    int  return_dims{ 0 };  // array dimension of the return (0 if not array)
};

// Walk a complete method descriptor.  Grammar: '(' field* ')' (field | 'V').
// Counts parameters and JVM slots (J/D contribute 2), and classifies the return.
// Any structural fault — missing '(', missing ')', a malformed parameter, a
// missing/garbled return — yields ok=false.  Fully bounds-checked.
static auto parse_method_descriptor(std::string_view sig) -> method_descriptor_parse
{
    method_descriptor_parse out{};
    if (sig.empty() || sig[0] != '(')
    {
        return out;
    }
    std::size_t pos{ 1 };
    int         args{ 0 };
    int         slots{ 0 };
    while (pos < sig.size() && sig[pos] != ')')
    {
        const field_descriptor_parse f{ parse_one_field_descriptor(sig, pos) };
        if (!f.ok || f.consumed == 0)
        {
            return out; // malformed parameter -> whole descriptor invalid
        }
        ++args;
        // long (J=11) and double (D=7) occupy two slots; everything else one.
        slots += (f.basic_type == 11 || f.basic_type == 7) ? 2 : 1;
        pos += f.consumed;
    }
    if (pos >= sig.size() || sig[pos] != ')')
    {
        return out; // no closing ')'
    }
    ++pos; // consume ')'
    if (pos >= sig.size())
    {
        return out; // nothing after ')': missing return descriptor
    }
    if (sig[pos] == 'V')
    {
        // void return must be the final byte.
        if (pos + 1 != sig.size())
        {
            return out;
        }
        out.ok           = true;
        out.arg_count    = args;
        out.arg_slots    = slots;
        out.return_basic = 14; // T_VOID
        out.return_dims  = 0;
        return out;
    }
    const field_descriptor_parse r{ parse_one_field_descriptor(sig, pos) };
    if (!r.ok || pos + r.consumed != sig.size())
    {
        return out; // trailing garbage after the return, or malformed return
    }
    out.ok           = true;
    out.arg_count    = args;
    out.arg_slots    = slots;
    out.return_basic = r.basic_type;
    out.return_dims  = r.array_dims;
    return out;
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

    // NOTE on the `signature[rparen+1]` boundary: a signature whose FINAL char
    // is ')' makes rparen+1 == size(), and string_view::operator[](size()) is
    // UB.  The RAW mirror (return_basic_type_of) reproduces the library's
    // *historical* unguarded read, so we never feed it an ends-in-')' input.
    // The CURRENT library guards that read (`rparen + 1 < size()`), and that
    // guarded behaviour — plus the unknown->VOID validity filter — is what the
    // call_site_result_type() mirror reproduces and PASS 5 below exercises in
    // full, including the previously-unprobed ends-in-')' boundary.

    // =====================================================================
    // EXHAUSTIVE PASS 5 — the CURRENT, GUARDED call-site return-decode.
    //
    // PASSES 1-4 above pin the raw sig_char_to_basic_type lookup as reached by
    // the *historical* unguarded call site (return_basic_type_of).  Since that
    // test was written the live method_proxy::call / call_jni sites gained two
    // guards (vmhook.hpp:14287-14313 / 13646-13660): a bounds check that makes a
    // signature ending in ')' decode as void instead of reading one-past-end,
    // and a return-char allow-list that degrades an UNKNOWN return descriptor to
    // T_VOID(14) instead of sig_char_to_basic_type's T_OBJECT(12) default.  This
    // pass drives call_site_result_type() — the faithful mirror of that guarded
    // ladder — so the suite tracks what the library actually does today, and so
    // the boundary + unknown-policy cases the older passes could not reach (the
    // raw mirror would UB on ends-in-')'; PASS-4's FLAW1 block pins the *old*
    // T_OBJECT policy) are now covered against the real, fixed behaviour.
    // =====================================================================

    // ---- guarded decode: every well-formed return letter is UNCHANGED -------
    // For a real, well-formed signature the guard is transparent: the allow-list
    // admits every genuine descriptor letter, so each routes to the SAME
    // BasicType the raw lookup gives.  Pin the full set so the guard can never
    // accidentally start rejecting a valid return.
    check("callsite_return_void_is_14",    call_site_result_type("()V")                    == 14);
    check("callsite_return_boolean_is_4",  call_site_result_type("(Ljava/lang/Object;)Z")  == 4);
    check("callsite_return_char_is_5",     call_site_result_type("(I)C")                    == 5);
    check("callsite_return_float_is_6",    call_site_result_type("(J)F")                    == 6);
    check("callsite_return_double_is_7",   call_site_result_type("([B)D")                   == 7);
    check("callsite_return_byte_is_8",     call_site_result_type("(II)B")                   == 8);
    check("callsite_return_short_is_9",    call_site_result_type("(D)S")                    == 9);
    check("callsite_return_int_is_10",     call_site_result_type("(I)I")                    == 10);
    check("callsite_return_long_is_11",    call_site_result_type("(FF)J")                   == 11);
    check("callsite_return_object_is_12",  call_site_result_type("(I)Ljava/lang/String;")   == 12);
    check("callsite_return_array_is_13",   call_site_result_type("(I)[I")                   == 13);
    // Multi-dimensional and object arrays classify by the leading '[' -> 13.
    check("callsite_return_2d_array_is_13",      call_site_result_type("()[[I")             == 13);
    check("callsite_return_obj_array_is_13",     call_site_result_type("()[Ljava/lang/String;") == 13);
    // "Last paren wins" survives the guard unchanged.
    check("callsite_return_many_params_last_paren_is_10",
          call_site_result_type("(IJLjava/lang/Object;)I") == 10);
    check("callsite_return_nested_parens_then_J_is_11",
          call_site_result_type("(((())))J") == 11);

    // ---- guarded decode: the ends-in-')' BOUNDARY (the unprobed gap) ---------
    // These are the inputs the raw mirror could NOT take: rparen is the LAST
    // index, so rparen+1 == size().  The live guard `rparen + 1 < size()` makes
    // the read fall to the 'V' branch -> T_VOID(14), with no one-past-end access.
    // Drive a representative spread of "chopped return" signatures and assert the
    // safe void result for every one.
    check("callsite_boundary_empty_parens_is_void_14",
          call_site_result_type("()") == 14);
    check("callsite_boundary_one_arg_no_ret_is_void_14",
          call_site_result_type("(I)") == 14);
    check("callsite_boundary_two_args_no_ret_is_void_14",
          call_site_result_type("(IJ)") == 14);
    check("callsite_boundary_object_arg_no_ret_is_void_14",
          call_site_result_type("(Ljava/lang/String;)") == 14);
    check("callsite_boundary_array_arg_no_ret_is_void_14",
          call_site_result_type("([I)") == 14);
    check("callsite_boundary_paren_soup_ending_in_rparen_is_void_14",
          call_site_result_type("(()())") == 14);
    check("callsite_boundary_single_rparen_is_void_14",
          call_site_result_type(")") == 14);
    // A lone ')' as the entire 1-char signature: rparen==0, size()==1, 0+1==1
    // is NOT < 1 -> void, no read of index 1.
    {
        bool every_chopped_is_void{ true };
        const char* chopped[]{
            "()", "(I)", "(II)", "(IJ)", "(JD)", "(Z)", "([[I)",
            "(Ljava/lang/Object;)", "([Ljava/lang/String;)", "(()())", ")",
        };
        for (const char* s : chopped)
        {
            if (call_site_result_type(s) != 14) { every_chopped_is_void = false; }
        }
        check("callsite_boundary_every_ends_in_rparen_is_void_14", every_chopped_is_void);
    }

    // ---- guarded decode: UNKNOWN return char -> T_VOID(14), not T_OBJECT -----
    // This is the policy FLIP from PASS-4's FLAW1 block.  The raw lookup maps an
    // unrecognised post-')' letter to T_OBJECT(12); the live allow-list rejects
    // it and yields T_VOID(14).  Pin the new, safe policy against the SAME inputs
    // FLAW1 pinned to 12, so the divergence between "raw classifier" and "live
    // call site" is explicit and any regression in either direction fails loudly.
    check("callsite_unknown_Q_is_void_14_not_object",
          call_site_result_type("(I)Q") == 14);
    check("callsite_unknown_lowercase_i_is_void_14",
          call_site_result_type("(I)i") == 14);
    check("callsite_unknown_digit_is_void_14",
          call_site_result_type("()9") == 14);
    check("callsite_unknown_semicolon_is_void_14",
          call_site_result_type("();") == 14);
    check("callsite_unknown_slash_is_void_14",
          call_site_result_type("()/") == 14);
    // Direct contrast with the raw classifier on one input: raw -> 12, guarded
    // -> 14.  This single line makes the remediation of flaw #1 unmistakable.
    check("callsite_vs_raw_unknown_return_diverge_14_vs_12",
          call_site_result_type("(I)Q") == 14 && return_basic_type_of("(I)Q") == 12);
    // The 'L' object marker is on the allow-list, so a genuine object return is
    // STILL 12 under the guard — only *unknown* letters move to 14.  Pin that a
    // valid object return is not collateral damage of the unknown->void policy.
    check("callsite_valid_object_L_still_12_under_guard",
          call_site_result_type("(I)Ljava/lang/String;") == 12);

    // ---- guarded decode: no-')' (npos) path -> void, same as raw ------------
    // When there is no ')' at all the guard's first conjunct (rparen != npos) is
    // false, so it takes the 'V' branch -> 14, identical to the raw mirror's npos
    // fallback.  Pin parity on the npos path so both mirrors agree where they can.
    check("callsite_no_paren_is_void_14",      call_site_result_type("garbage") == 14);
    check("callsite_empty_signature_is_void_14", call_site_result_type("")       == 14);
    check("callsite_single_letter_no_paren_is_void_14", call_site_result_type("I") == 14);

    // ---- guarded decode: FULL 0..255 post-')' byte sweep --------------------
    // Build "()X" for every byte X and assert the guarded result matches the
    // reference policy: the 11 allow-listed descriptor letters (Z C F D B S I J
    // L [ V) keep their BasicType; EVERY other byte (all 245 of them, including
    // all high/negative bytes under a signed `char`) degrades to T_VOID(14).
    // This is the call-site analogue of the raw 0..255 sweep in pass 2 and is the
    // single source of truth for "which return bytes the live decode trusts".
    {
        auto reference_callsite = [](int byte) -> int
        {
            switch (byte)
            {
            case 'Z': return 4;   case 'C': return 5;   case 'F': return 6;
            case 'D': return 7;   case 'B': return 8;   case 'S': return 9;
            case 'I': return 10;  case 'J': return 11;  case 'L': return 12;
            case '[': return 13;  case 'V': return 14;
            default:  return 14;  // unknown return -> T_VOID under the allow-list
            }
        };
        bool whole_post_paren_range_matches{ true };
        for (int byte{ 0 }; byte <= 0xFF; ++byte)
        {
            const char three[3]{ '(', ')', static_cast<char>(byte) };
            const int got{ call_site_result_type(std::string_view{ three, 3 }) };
            if (got != reference_callsite(byte)) { whole_post_paren_range_matches = false; }
        }
        check("callsite_full_0_to_255_post_paren_sweep_matches_allowlist",
              whole_post_paren_range_matches);
        // Independent count: exactly 10 bytes classify as something OTHER than
        // T_VOID(14) — Z C F D B S I J L [ minus the ones equal to 14.  'V' maps
        // to 14 (so not counted) and unknowns map to 14, leaving precisely the 8
        // primitives + 'L'(12) + '['(13) = 10 distinct-from-void results.
        int non_void_count{ 0 };
        for (int byte{ 0 }; byte <= 0xFF; ++byte)
        {
            const char three[3]{ '(', ')', static_cast<char>(byte) };
            if (call_site_result_type(std::string_view{ three, 3 }) != 14) { ++non_void_count; }
        }
        check("callsite_exactly_10_post_paren_bytes_classify_non_void", non_void_count == 10);
    }
    // High / signed-char corners after ')': bytes that arrive negative under a
    // signed `char` are not allow-listed -> 14, with no signed-char surprise.
    check("callsite_high_byte_0x80_after_paren_is_void_14",
          call_site_result_type(std::string_view{ "()\x80", 3 }) == 14);
    check("callsite_high_byte_0xFF_after_paren_is_void_14",
          call_site_result_type(std::string_view{ "()\xFF", 3 }) == 14);

    // =====================================================================
    // EXHAUSTIVE PASS 6 — whole method/type descriptor WALK (JVMS § 4.3).
    //
    // PASSES 1-5 pin the library's per-byte helpers and the return-char
    // extraction.  This pass covers the *whole-descriptor* parse axis the task
    // enumerates — argument enumeration, arg COUNT vs JVM SLOT count (long/double
    // = 2), array dimension + component, full return classification, and graceful
    // handling of every malformation — via the self-contained reference walk
    // (parse_one_field_descriptor / parse_method_descriptor) defined above, which
    // is built only on vmhook::detail::sig_char_to_basic_type +
    // jvm_primitive_byte_width.  No library whole-signature parser exists, so this
    // canonical walk also serves as the diff target for any future one.
    // =====================================================================

    // ---- single field descriptor: every primitive char ----------------------
    // Each primitive descriptor is a one-byte field that parses to its basic type
    // with zero array dimension and consumes exactly one byte.
    {
        struct prim_field { const char* d; int basic; };
        const prim_field prims[]{
            { "Z", 4 }, { "C", 5 }, { "F", 6 }, { "D", 7 },
            { "B", 8 }, { "S", 9 }, { "I", 10 }, { "J", 11 },
        };
        bool all_ok{ true };
        for (const prim_field& p : prims)
        {
            const field_descriptor_parse f{ parse_one_field_descriptor(p.d, 0) };
            if (!f.ok || f.basic_type != p.basic || f.array_dims != 0
                || f.component_basic != p.basic || f.consumed != 1)
            {
                all_ok = false;
            }
        }
        check("fielddesc_every_primitive_parses_one_byte_to_basic_type", all_ok);
    }
    // 'V' is NOT a valid field descriptor (only a method RETURN), so it fails to
    // parse as a field even though sig_char_to_basic_type('V')==14.
    check("fielddesc_void_is_not_a_valid_field",
          !parse_one_field_descriptor("V", 0).ok);

    // ---- single field descriptor: object Lpkg/Class; ------------------------
    // Deep package, with digits / underscores, parses as T_OBJECT(12), dims 0,
    // consuming the whole descriptor INCLUDING the terminating ';'.
    {
        const field_descriptor_parse f{
            parse_one_field_descriptor("Lcom/example/My_Class9;", 0) };
        check("fielddesc_object_deeppkg_digits_underscore_is_object_12",
              f.ok && f.basic_type == 12 && f.array_dims == 0
              && f.consumed == std::string_view{ "Lcom/example/My_Class9;" }.size());
    }
    // Nested ('$') inner-class name round-trips and terminates at the first ';'.
    {
        const field_descriptor_parse f{
            parse_one_field_descriptor("La/b/Outer$Inner$Deep;", 0) };
        check("fielddesc_object_nested_inner_class_is_object_12",
              f.ok && f.basic_type == 12
              && f.consumed == std::string_view{ "La/b/Outer$Inner$Deep;" }.size());
    }
    // Single-segment ("default package") object name.
    check("fielddesc_object_single_segment_ok",
          parse_one_field_descriptor("LX;", 0).ok);
    // java/lang/String, the canonical reference type.
    {
        const field_descriptor_parse f{
            parse_one_field_descriptor("Ljava/lang/String;", 0) };
        check("fielddesc_object_string_is_object_12_full_consume",
              f.ok && f.basic_type == 12
              && f.consumed == std::string_view{ "Ljava/lang/String;" }.size());
    }

    // ---- single field descriptor: arrays [I [[I [Ljava/lang/String; [[[D -----
    // The whole array descriptor classifies as T_ARRAY(13); array_dims counts the
    // leading '[', component_basic is the element's basic type, and consumed spans
    // the full descriptor (brackets + element, incl. any object ';').
    {
        struct arr_field { const char* d; int dims; int component; std::size_t len; };
        const arr_field arrays[]{
            { "[I",                    1, 10, 2  },                 // int[]
            { "[[I",                   2, 10, 3  },                 // int[][]
            { "[[[D",                  3, 7,  4  },                 // double[][][]
            { "[J",                    1, 11, 2  },                 // long[]
            { "[Z",                    1, 4,  2  },                 // boolean[]
            { "[Ljava/lang/String;",   1, 12, 19 },                 // String[]
            { "[[Ljava/lang/Object;",  2, 12, 20 },                 // Object[][]
        };
        bool all_ok{ true };
        for (const arr_field& a : arrays)
        {
            const field_descriptor_parse f{ parse_one_field_descriptor(a.d, 0) };
            if (!f.ok || f.basic_type != 13 || f.array_dims != a.dims
                || f.component_basic != a.component || f.consumed != a.len)
            {
                all_ok = false;
            }
        }
        check("fielddesc_arrays_dimension_and_component_parsed", all_ok);
    }

    // ---- method descriptor: ()V — zero args, void return --------------------
    {
        const method_descriptor_parse m{ parse_method_descriptor("()V") };
        check("methoddesc_void_noargs_is_0args_0slots_voidret",
              m.ok && m.arg_count == 0 && m.arg_slots == 0
              && m.return_basic == 14 && m.return_dims == 0);
    }
    // ---- method descriptor: (I)I — one int arg, int return ------------------
    {
        const method_descriptor_parse m{ parse_method_descriptor("(I)I") };
        check("methoddesc_int_to_int_is_1arg_1slot_intret",
              m.ok && m.arg_count == 1 && m.arg_slots == 1
              && m.return_basic == 10 && m.return_dims == 0);
    }
    // ---- method descriptor: (JD)V — two WIDE args -> 2 args, 4 slots --------
    // The crux of the slot rule: long + double = two parameters but FOUR JVM
    // local slots (2 each), with a void return.
    {
        const method_descriptor_parse m{ parse_method_descriptor("(JD)V") };
        check("methoddesc_long_double_void_is_2args_4slots",
              m.ok && m.arg_count == 2 && m.arg_slots == 4
              && m.return_basic == 14);
    }
    // ---- method descriptor: (Ljava/lang/String;[IJ)Z — mixed ----------------
    // String (1 slot) + int[] (1 slot, arrays are single-slot references) + long
    // (2 slots) = 3 args, 4 slots; boolean return.
    {
        const method_descriptor_parse m{
            parse_method_descriptor("(Ljava/lang/String;[IJ)Z") };
        check("methoddesc_mixed_string_intarray_long_is_3args_4slots_boolret",
              m.ok && m.arg_count == 3 && m.arg_slots == 4
              && m.return_basic == 4 && m.return_dims == 0);
    }
    // ---- method descriptor: ([[Ljava/lang/Object;)[I — array arg + array ret -
    // One parameter (Object[][], a single reference slot) and an int[] return
    // classified T_ARRAY(13) with return_dims==1.
    {
        const method_descriptor_parse m{
            parse_method_descriptor("([[Ljava/lang/Object;)[I") };
        check("methoddesc_objarray_arg_intarray_ret_is_1arg_1slot_arrayret",
              m.ok && m.arg_count == 1 && m.arg_slots == 1
              && m.return_basic == 13 && m.return_dims == 1);
    }
    // ---- method descriptor: return-type shape sweep -------------------------
    // Every return shape (void / each primitive / object / 1-D & multi-D array /
    // object array) classified through the FULL parse (not just the post-')'
    // byte), with the parameter list varied to prove the walk consumes params
    // correctly before reaching the return.
    {
        struct ret_shape { const char* d; int basic; int dims; };
        const ret_shape rets[]{
            { "()V",                       14, 0 },  // void
            { "(I)Z",                      4,  0 },  // boolean
            { "(J)C",                      5,  0 },  // char
            { "(D)F",                      6,  0 },  // float
            { "(B)D",                      7,  0 },  // double
            { "(S)B",                      8,  0 },  // byte
            { "(C)S",                      9,  0 },  // short
            { "(F)I",                      10, 0 },  // int
            { "(Z)J",                      11, 0 },  // long
            { "(I)Ljava/lang/String;",     12, 0 },  // object
            { "(I)[I",                     13, 1 },  // int[]
            { "(I)[[I",                    13, 2 },  // int[][]
            { "(I)[[[D",                   13, 3 },  // double[][][]
            { "(I)[Ljava/lang/String;",    13, 1 },  // String[]
        };
        bool all_ok{ true };
        for (const ret_shape& r : rets)
        {
            const method_descriptor_parse m{ parse_method_descriptor(r.d) };
            if (!m.ok || m.return_basic != r.basic || m.return_dims != r.dims)
            {
                all_ok = false;
            }
        }
        check("methoddesc_return_shape_sweep_all_classified", all_ok);
    }

    // ---- arg COUNT vs arg SLOT count: the wide-type partition, exhaustively --
    // For each single-parameter method, arg_count is always 1 but arg_slots is 2
    // exactly when the parameter is long (J) or double (D), and 1 otherwise
    // (including arrays-of-long / arrays-of-double, which are single-slot refs).
    {
        struct one_param { const char* d; int slots; };
        const one_param cases[]{
            { "(Z)V", 1 }, { "(B)V", 1 }, { "(C)V", 1 }, { "(S)V", 1 },
            { "(I)V", 1 }, { "(F)V", 1 }, { "(J)V", 2 }, { "(D)V", 2 },
            { "(Ljava/lang/String;)V", 1 },  // reference = 1 slot
            { "([J)V", 1 },                  // long[] is a reference = 1 slot
            { "([D)V", 1 },                  // double[] is a reference = 1 slot
            { "([[I)V", 1 },                 // int[][] reference = 1 slot
        };
        bool all_ok{ true };
        for (const one_param& c : cases)
        {
            const method_descriptor_parse m{ parse_method_descriptor(c.d) };
            if (!m.ok || m.arg_count != 1 || m.arg_slots != c.slots) { all_ok = false; }
        }
        check("methoddesc_single_param_slot_is_2_iff_long_or_double", all_ok);
    }
    // A handful of multi-arg packs with mixed widths, asserting BOTH the argument
    // count and the slot count independently so an off-by-one in either is caught.
    {
        struct pack_case { const char* d; int args; int slots; };
        const pack_case packs[]{
            { "(II)V",                              2, 2 },   // int,int
            { "(JJ)V",                              2, 4 },   // long,long
            { "(IJ)V",                              2, 3 },   // int,long
            { "(JIDI)V",                            4, 6 },   // long,int,double,int
            { "(DDDD)V",                            4, 8 },   // 4 doubles
            { "(Ljava/lang/String;Ljava/lang/Object;)V", 2, 2 }, // 2 refs
            { "([I[J[D)V",                          3, 3 },   // 3 arrays = 3 slots
            { "(IJLjava/lang/Object;DF)V",          5, 7 },   // mixed
        };
        bool all_ok{ true };
        for (const pack_case& p : packs)
        {
            const method_descriptor_parse m{ parse_method_descriptor(p.d) };
            if (!m.ok || m.arg_count != p.args || m.arg_slots != p.slots)
            {
                all_ok = false;
            }
        }
        check("methoddesc_mixed_packs_arg_and_slot_counts_correct", all_ok);
    }

    // ---- boundary: very long argument list ----------------------------------
    // 64 int parameters -> 64 args, 64 slots; and 64 long parameters -> 64 args,
    // 128 slots.  Proves the walk has no fixed-arity ceiling and the slot math
    // accumulates correctly across a long list (unlike method_proxy's params[8]).
    {
        std::string many_ints{ "(" };
        std::string many_longs{ "(" };
        for (int i{ 0 }; i < 64; ++i)
        {
            many_ints  += "I";
            many_longs += "J";
        }
        many_ints  += ")V";
        many_longs += ")V";
        const method_descriptor_parse mi{ parse_method_descriptor(many_ints) };
        const method_descriptor_parse ml{ parse_method_descriptor(many_longs) };
        check("methoddesc_64_int_params_is_64args_64slots",
              mi.ok && mi.arg_count == 64 && mi.arg_slots == 64);
        check("methoddesc_64_long_params_is_64args_128slots",
              ml.ok && ml.arg_count == 64 && ml.arg_slots == 128);
    }
    // ---- boundary: a single deeply-nested array parameter -------------------
    // 32 '[' before the element 'I': one argument (a reference, 1 slot) whose
    // component is int and whose dimension is 32.
    {
        std::string deep{ "(" };
        deep.append(32, '[');
        deep += "I)V";
        const method_descriptor_parse m{ parse_method_descriptor(deep) };
        check("methoddesc_32d_array_param_is_1arg_1slot", m.ok && m.arg_count == 1 && m.arg_slots == 1);
        // And the field-level walk of just that 32-D array reports dims==32.
        std::string deep_field;
        deep_field.append(32, '[');
        deep_field += "I";
        const field_descriptor_parse f{ parse_one_field_descriptor(deep_field, 0) };
        check("fielddesc_32d_array_component_int_dims_32",
              f.ok && f.basic_type == 13 && f.array_dims == 32 && f.component_basic == 10);
    }

    // ---- MALFORMED descriptors: graceful rejection, NO overrun --------------
    // Each of these is structurally invalid; the walk must return ok=false and —
    // critically — must not read past the end.  (Bounds safety is enforced by the
    // `pos < size()` guards in the helpers; reaching here at all without a crash
    // under -fsanitize / normal execution is itself the overrun check.)
    {
        const char* bad_methods[]{
            "",                       // empty
            "I",                      // no parens at all
            "(",                      // just open paren, never closed
            "()",                     // closed but no return descriptor
            "(I",                     // unterminated param list (no ')')
            "(II",                    // unterminated, two params
            ")V",                     // missing open paren
            "(I)",                    // params + ')' but no return
            "(I)V extra",             // trailing garbage after a void return
            "(I)IJ",                  // trailing byte after a primitive return
            "(L)V",                   // 'L' with no class name and no ';'
            "(Ljava/lang/String)V",   // object param missing terminating ';'
            "(Q)V",                   // unknown primitive-ish letter as a param
            "([)V",                   // '[' with no element before ')'
            "(V)V",                   // 'V' is not a valid PARAMETER descriptor
            "(I)Q",                   // unknown return letter (not in grammar)
            "(I)[",                   // return '[' with no element
            "(I)L",                   // return 'L' with no class / ';'
            "garbage",                // pure junk, no structure
            "(((",                    // only open parens
            "()VV",                   // void must be the final byte
        };
        bool all_rejected{ true };
        for (const char* d : bad_methods)
        {
            if (parse_method_descriptor(d).ok) { all_rejected = false; }
        }
        check("methoddesc_all_malformed_descriptors_rejected_no_overrun", all_rejected);
    }
    // Malformed FIELD descriptors likewise reject without overrun.
    {
        const char* bad_fields[]{
            "",                       // empty
            "L",                      // 'L' alone, unterminated
            "Ljava/lang/String",      // object missing ';'
            "[",                      // '[' alone, no element
            "[[",                     // brackets with no element
            "[L",                     // array of unterminated object
            "[Lfoo",                  // array of object missing ';'
            "V",                      // void is not a field descriptor
            "Q",                      // unknown leading byte
            ";",                      // stray terminator
            ")",                      // stray close paren
        };
        bool all_rejected{ true };
        for (const char* d : bad_fields)
        {
            if (parse_one_field_descriptor(d, 0).ok) { all_rejected = false; }
        }
        check("fielddesc_all_malformed_field_descriptors_rejected_no_overrun", all_rejected);
    }
    // Targeted unterminated-'L' boundary: "L" of length 1 must NOT read sig[1].
    // The helper's `scan < sig.size()` guard makes this safe; assert it rejects.
    check("fielddesc_lone_L_unterminated_rejected",
          !parse_one_field_descriptor("L", 0).ok);
    // A '[' as the entire 1-char field descriptor: dims becomes 1 then pos hits
    // end -> rejected, no read of sig[1].
    check("fielddesc_lone_bracket_rejected",
          !parse_one_field_descriptor("[", 0).ok);
    // Method "(" of length 1: pos=1 == size() immediately -> loop body never
    // runs, the `pos >= size()` no-')' branch rejects, sig[1] never read.
    check("methoddesc_lone_open_paren_rejected_no_read",
          !parse_method_descriptor("(").ok);

    // ---- well-formed round-trip: re-pin the exact briefing examples ---------
    // The descriptors named in the task, asserted ok with their full (count,
    // slots, return) tuple in one greppable block.
    check("methoddesc_briefing_II_V",
          [] { const auto m = parse_method_descriptor("(II)V");
               return m.ok && m.arg_count == 2 && m.arg_slots == 2 && m.return_basic == 14; }());
    check("methoddesc_briefing_String_to_I",
          [] { const auto m = parse_method_descriptor("(Ljava/lang/String;)I");
               return m.ok && m.arg_count == 1 && m.arg_slots == 1 && m.return_basic == 10; }());
    check("methoddesc_briefing_JD_V_wide",
          [] { const auto m = parse_method_descriptor("(JD)V");
               return m.ok && m.arg_count == 2 && m.arg_slots == 4 && m.return_basic == 14; }());
    check("methoddesc_briefing_mixed_String_intarray_long_Z",
          [] { const auto m = parse_method_descriptor("(Ljava/lang/String;[IJ)Z");
               return m.ok && m.arg_count == 3 && m.arg_slots == 4 && m.return_basic == 4; }());
    check("methoddesc_briefing_objarray_to_intarray",
          [] { const auto m = parse_method_descriptor("([[Ljava/lang/Object;)[I");
               return m.ok && m.arg_count == 1 && m.arg_slots == 1
                      && m.return_basic == 13 && m.return_dims == 1; }());

    // =====================================================================
    // EXHAUSTIVE PASS 7 — close the remaining whole-descriptor shape gaps the
    // task enumerates that earlier passes touched only partially:
    //   * arrays of EVERY primitive component (passes 6's array sweep covered
    //     int/double/long/boolean/object but skipped char/float/byte/short),
    //   * the parse_one_field_descriptor `pos` offset contract (always called
    //     with pos==0 above; here driven mid-string so the offset arithmetic —
    //     consumed measured from `start`, not 0 — is exercised directly),
    //   * '$' inner-class and java/lang/Object object shapes at the METHOD
    //     level (only field-level above),
    //   * every-primitive-array METHOD parameters end-to-end.
    // Every expected value still derives only from the confirmed
    // sig_char_to_basic_type / jvm_primitive_byte_width tables.
    // =====================================================================

    // ---- arrays of EVERY primitive component (the four previously skipped) ----
    // [C / [F / [B / [S complete the "array of every primitive" matrix: each is
    // T_ARRAY(13), dims 1, with component_basic = that primitive's basic type,
    // consuming exactly 2 bytes ('[' + element).
    {
        struct arr_prim { const char* d; int component; };
        const arr_prim rows[]{
            { "[Z", 4 },  // boolean[]
            { "[C", 5 },  // char[]
            { "[F", 6 },  // float[]
            { "[D", 7 },  // double[]
            { "[B", 8 },  // byte[]
            { "[S", 9 },  // short[]
            { "[I", 10 }, // int[]
            { "[J", 11 }, // long[]
        };
        bool all_ok{ true };
        for (const arr_prim& a : rows)
        {
            const field_descriptor_parse f{ parse_one_field_descriptor(a.d, 0) };
            if (!f.ok || f.basic_type != 13 || f.array_dims != 1
                || f.component_basic != a.component || f.consumed != 2)
            {
                all_ok = false;
            }
        }
        check("fielddesc_array_of_every_primitive_component_parsed", all_ok);
    }
    // The same four char/float/byte/short components at 2-D and 3-D depth, so the
    // dimension counter is exercised for the previously-uncovered element types.
    {
        struct arr_prim_depth { const char* d; int dims; int component; std::size_t len; };
        const arr_prim_depth rows[]{
            { "[[C",  2, 5, 3 }, { "[[[C", 3, 5, 4 },   // char[][], char[][][]
            { "[[F",  2, 6, 3 }, { "[[[F", 3, 6, 4 },   // float[][], float[][][]
            { "[[B",  2, 8, 3 }, { "[[[B", 3, 8, 4 },   // byte[][], byte[][][]
            { "[[S",  2, 9, 3 }, { "[[[S", 3, 9, 4 },   // short[][], short[][][]
        };
        bool all_ok{ true };
        for (const arr_prim_depth& a : rows)
        {
            const field_descriptor_parse f{ parse_one_field_descriptor(a.d, 0) };
            if (!f.ok || f.basic_type != 13 || f.array_dims != a.dims
                || f.component_basic != a.component || f.consumed != a.len)
            {
                all_ok = false;
            }
        }
        check("fielddesc_multidim_char_float_byte_short_arrays_parsed", all_ok);
    }

    // ---- parse_one_field_descriptor: the `pos` offset contract ---------------
    // Every call above passes pos==0.  parse_method_descriptor relies on the
    // helper parsing a field that STARTS partway through the string and reporting
    // `consumed` relative to that start (not to index 0).  Drive that path
    // directly: scan the second field of a two-field string and assert the parse
    // describes the field AT the offset, with consumed measured from `pos`.
    {
        // "IJ": parse the 'J' at pos==1 -> long, 1 byte consumed.
        const field_descriptor_parse f{ parse_one_field_descriptor("IJ", 1) };
        check("fielddesc_pos_offset_parses_field_at_index_not_zero",
              f.ok && f.basic_type == 11 && f.array_dims == 0 && f.consumed == 1);
    }
    {
        // "I[Ljava/lang/String;": parse the array object starting at pos==1.
        // consumed counts from pos==1 to the end (the whole "[Ljava/lang/String;").
        const std::string_view sig{ "I[Ljava/lang/String;" };
        const field_descriptor_parse f{ parse_one_field_descriptor(sig, 1) };
        check("fielddesc_pos_offset_parses_array_object_midstring",
              f.ok && f.basic_type == 13 && f.array_dims == 1
              && f.component_basic == 12
              && f.consumed == sig.size() - 1);
    }
    {
        // Offset landing exactly on the trailing object of "[BLcom/x/Y;": at
        // pos==2 the 'L' object is parsed alone (the leading "[B" is skipped by
        // the caller-provided offset), consuming "Lcom/x/Y;".
        const std::string_view sig{ "[BLcom/x/Y;" };
        const field_descriptor_parse f{ parse_one_field_descriptor(sig, 2) };
        check("fielddesc_pos_offset_object_after_array_prefix",
              f.ok && f.basic_type == 12 && f.array_dims == 0
              && f.consumed == std::string_view{ "Lcom/x/Y;" }.size());
    }

    // ---- object SHAPES at the method level: java/lang/Object + '$' inner ------
    // Field-level object parses are covered above; pin the same canonical object
    // names as METHOD params/returns so the whole-descriptor walk consumes a
    // back-to-back run of ';'-terminated objects (including an inner-class '$'
    // name) correctly and classifies the object return.
    {
        // java/lang/Object as the single param AND a $-inner-class object return.
        const method_descriptor_parse m{
            parse_method_descriptor("(Ljava/lang/Object;)La/b/Outer$Inner;") };
        check("methoddesc_object_param_inner_class_object_return",
              m.ok && m.arg_count == 1 && m.arg_slots == 1
              && m.return_basic == 12 && m.return_dims == 0);
    }
    {
        // Three consecutive object params with mixed name shapes (deep package,
        // inner class, single segment) -> 3 args, 3 slots, void return.  Proves
        // the walk re-synchronises on each ';' with no separator between objects.
        const method_descriptor_parse m{ parse_method_descriptor(
            "(Lcom/example/Deep$Inner;Ljava/lang/Object;LX;)V") };
        check("methoddesc_three_back_to_back_objects_3args_3slots",
              m.ok && m.arg_count == 3 && m.arg_slots == 3
              && m.return_basic == 14);
    }

    // ---- every-primitive-array as a METHOD PARAMETER -------------------------
    // ([Z) ([C) ([F) ([D) ([B) ([S) ([I) ([J): each is ONE parameter and ONE
    // slot (arrays are single-slot references regardless of element width — even
    // [J and [D), with a void return.  This pairs the field-level array sweep
    // above with its method-descriptor counterpart for the full primitive set.
    {
        const char* array_params[]{
            "([Z)V", "([C)V", "([F)V", "([D)V",
            "([B)V", "([S)V", "([I)V", "([J)V",
        };
        bool all_ok{ true };
        for (const char* d : array_params)
        {
            const method_descriptor_parse m{ parse_method_descriptor(d) };
            if (!m.ok || m.arg_count != 1 || m.arg_slots != 1
                || m.return_basic != 14)
            {
                all_ok = false;
            }
        }
        check("methoddesc_every_primitive_array_param_is_1arg_1slot", all_ok);
    }
    // ...and every-primitive-array as the RETURN: each classifies T_ARRAY(13)
    // with return_dims==1, regardless of element type.
    {
        const char* array_returns[]{
            "()[Z", "()[C", "()[F", "()[D",
            "()[B", "()[S", "()[I", "()[J",
        };
        bool all_ok{ true };
        for (const char* d : array_returns)
        {
            const method_descriptor_parse m{ parse_method_descriptor(d) };
            if (!m.ok || m.return_basic != 13 || m.return_dims != 1)
            {
                all_ok = false;
            }
        }
        check("methoddesc_every_primitive_array_return_is_array_dim1", all_ok);
    }

    // ---- java/lang/Object as a standalone FIELD descriptor ------------------
    // The other canonical reference type (String is covered above): parses to
    // T_OBJECT(12), dims 0, consuming the whole "Ljava/lang/Object;".
    {
        const field_descriptor_parse f{
            parse_one_field_descriptor("Ljava/lang/Object;", 0) };
        check("fielddesc_object_java_lang_Object_is_object_12_full_consume",
              f.ok && f.basic_type == 12 && f.array_dims == 0
              && f.consumed == std::string_view{ "Ljava/lang/Object;" }.size());
    }

    // ---- ctor signature: every-primitive-array + object-array arg tokens ------
    // jni_signature_for_arg has no C++ branch for array types (those go through
    // wrappers), so array arguments are not expressible via the ctor fold; this
    // is the descriptor-walk's job, asserted above.  Here, re-pin via the fold
    // the one constructor shape the task names that mixes a wide primitive run
    // with a String — a long pack proving token order is preserved across many
    // args (S C I J F D then String).
    check("ctor_sig_long_mixed_pack_order_preserved",
          ctor_signature_of<std::int16_t, std::uint16_t, std::int32_t,
                            std::int64_t, float, double, std::string>()
              == "(SCIJFDLjava/lang/String;)V");

    // =====================================================================
    // EXHAUSTIVE PASS 8 — the GENERIC sizeof integral ladder + remaining
    // whole-descriptor adversarial shapes.
    //
    // The live jni_signature_for_arg (vmhook.hpp:12772) is NOT a fixed-width
    // typedef ladder — it is `bool -> Z`, then `char16_t|uint16_t -> C`, then a
    // GENERIC `is_integral && sizeof==N` ladder (N=1->B, 2->S, 4->I, 8->J),
    // then float/double, then the wrapper/object class-map branches.  Earlier
    // passes only instantiated the fixed-width std::intNN_t aliases; this pass
    // drives the *character* integral types (`char`, `char8_t`, `char16_t`,
    // `signed char`, `unsigned char`) that route through that generic ladder and
    // that the older comments wrongly claimed "fail to compile".  It also closes
    // the last whole-descriptor parse gaps: branch-ORDER invariants, object names
    // whose bytes collide with structural tokens, and a fresh malformed batch.
    // Every expected value derives only from the confirmed sig_char_to_basic_type
    // / jvm_primitive_byte_width / jni_signature_for_arg tables in vmhook.hpp.
    // =====================================================================

    // ---- generic sizeof==1 integral branch: every 1-byte char type -> "B" ----
    // `char`, `signed char`, `unsigned char`, and `char8_t` are all 1-byte
    // integrals that are NOT bool and NOT uint16_t/char16_t, so they fall to the
    // generic sizeof==1 branch -> "B".  (signed char == int8_t is already pinned;
    // plain `char` and `char8_t` are the genuinely new rows the generic ladder
    // admits — the OLD fixed-width ladder static_asserted on them.)  Java has no
    // unsigned byte, so every 8-bit integral collapses to the one byte tag 'B'.
    check("jni_sig_plain_char_is_B",
          vmhook::detail::jni_signature_for_arg<char>() == "B");
    check("jni_sig_unsigned_char_is_B",
          vmhook::detail::jni_signature_for_arg<unsigned char>() == "B");
    check("jni_sig_char8_t_is_B",
          vmhook::detail::jni_signature_for_arg<char8_t>() == "B");
    // sizeof(char)==1 is mandated by the standard, so this branch is reached on
    // every target — pin the property the routing keys on.
    check("jni_sig_char_routes_via_sizeof1_branch_property",
          std::is_integral_v<char> && sizeof(char) == 1
          && !std::is_same_v<std::decay_t<char>, bool>
          && !std::is_same_v<std::decay_t<char>, std::uint16_t>);

    // ---- char16_t now maps to "C" (the corrected ladder) ---------------------
    // The unsigned-16 / Java-char branch claims `char16_t || uint16_t` BEFORE the
    // generic sizeof==2 branch, so char16_t -> "C" (NOT the static_assert the old
    // comments described, and NOT "S").  This is the symmetric partner to
    // uint16_t->C and is what makes a C++ UTF-16 unit a Java `char` arg.
    check("jni_sig_char16_t_is_C",
          vmhook::detail::jni_signature_for_arg<char16_t>() == "C");
    // char16_t and uint16_t agree on "C"; int16_t (a DISTINCT, signed 2-byte
    // integral with no special branch) takes the generic sizeof==2 -> "S".  Pin
    // all three together so the "which 16-bit types are Java char" set is fixed.
    check("jni_sig_char16_uint16_both_C_int16_S",
             vmhook::detail::jni_signature_for_arg<char16_t>() == "C"
          && vmhook::detail::jni_signature_for_arg<std::uint16_t>() == "C"
          && vmhook::detail::jni_signature_for_arg<std::int16_t>() == "S");
    // cv/ref qualified char16_t still decays to "C".
    check("jni_sig_const_ref_char16_t_is_C",
          vmhook::detail::jni_signature_for_arg<const char16_t&>() == "C");

    // ---- branch-ORDER invariants (the ordering is load-bearing) --------------
    // bool is a 1-byte integral; if the generic sizeof==1 branch ran first it
    // would mis-encode bool as "B".  The bool branch is claimed FIRST, so bool ->
    // "Z" while every OTHER 1-byte integral -> "B".  Pin the discriminating pair.
    check("jni_sig_bool_is_Z_not_B_branch_order",
             vmhook::detail::jni_signature_for_arg<bool>() == "Z"
          && vmhook::detail::jni_signature_for_arg<char>() == "B"
          && vmhook::detail::jni_signature_for_arg<bool>()
                 != vmhook::detail::jni_signature_for_arg<char>());
    // uint16_t is a 2-byte integral; the char-branch claims it BEFORE the generic
    // sizeof==2 branch, so it is "C" not "S".  int16_t (no special branch) is the
    // sizeof==2 fall-through -> "S".  The order is what separates them.
    check("jni_sig_uint16_C_before_generic_sizeof2_S_branch_order",
             vmhook::detail::jni_signature_for_arg<std::uint16_t>() == "C"
          && vmhook::detail::jni_signature_for_arg<std::int16_t>() == "S"
          && vmhook::detail::jni_signature_for_arg<std::uint16_t>()
                 != vmhook::detail::jni_signature_for_arg<std::int16_t>());

    // ---- ctor signature: the new char-type rows compose end-to-end -----------
    // The character integral tags must compose into a whole <init> descriptor
    // exactly like the fixed-width aliases.  A mixed pack with `char` ('B') and
    // `char16_t` ('C') proves the generic-ladder tags survive the fold.
    check("ctor_sig_char_and_char16_pack",
          ctor_signature_of<char, char16_t, char8_t>() == "(BCB)V");
    check("ctor_sig_char_with_string_and_int",
          ctor_signature_of<char, std::string, int>()
              == "(BLjava/lang/String;I)V");

    // ---- public re-export parity on the new char rows ------------------------
    // vmhook::jni::signature_for_arg forwards verbatim, so it must agree on the
    // character types too — pin parity on the genuinely-new branches.
    check("signature_for_arg_parity_char_B",
          vmhook::jni::signature_for_arg<char>()
              == vmhook::detail::jni_signature_for_arg<char>());
    check("signature_for_arg_parity_char16_C",
          vmhook::jni::signature_for_arg<char16_t>()
              == vmhook::detail::jni_signature_for_arg<char16_t>());
    check("signature_for_arg_parity_char16_value_is_C",
          vmhook::jni::signature_for_arg<char16_t>() == "C");

    // ---- field descriptor: object names whose BYTES collide with tokens ------
    // The object-name scanner consumes everything up to the FIRST ';' verbatim,
    // so a class name byte-stream containing '(' ')' '[' 'V' 'I' etc. is parsed
    // as NAME content, not as structural descriptor tokens.  These are not legal
    // Java identifiers, but the walk is a byte scanner: it must terminate at the
    // first ';' and consume exactly that span.  Proves no structural byte inside
    // an L...; is special.
    {
        const std::string_view weird{ "Lweird(name)[V/Type;" };
        const field_descriptor_parse f{ parse_one_field_descriptor(weird, 0) };
        check("fielddesc_object_name_with_token_bytes_consumed_to_semicolon",
              f.ok && f.basic_type == 12 && f.array_dims == 0
              && f.consumed == weird.size());
    }
    // The name scanner stops at the FIRST ';': a second ';' later is NOT consumed
    // by this field — "La;b;" parses the object "La;" (consume 3) and leaves "b;".
    {
        const field_descriptor_parse f{ parse_one_field_descriptor("La;b;", 0) };
        check("fielddesc_object_stops_at_first_semicolon",
              f.ok && f.basic_type == 12 && f.consumed == 3);
    }
    // A high-byte / "unicode-ish" sequence inside the class name is just name
    // bytes; the scanner terminates at ';' and consumes the whole span.  (Built
    // as a named array so the string_view never dangles.)
    {
        const char unicode_name[]{ 'L', 'p', '/', static_cast<char>(0xC3),
                                   static_cast<char>(0xA9), 'X', ';', '\0' };
        const std::string_view sv{ unicode_name, 7 };
        const field_descriptor_parse f{ parse_one_field_descriptor(sv, 0) };
        check("fielddesc_object_high_byte_name_consumed_to_semicolon",
              f.ok && f.basic_type == 12 && f.consumed == 7);
    }
    // Empty class name "L;": the scanner finds ';' at index 1 immediately, so the
    // walk ACCEPTS it as a 2-byte object (this reference walk is permissive about
    // the empty name; JVMS forbids it, but sig_char_to_basic_type-based walking
    // does not validate name content).  Pin the walk's actual behaviour so a
    // future stricter parser is a visible change.
    {
        const field_descriptor_parse f{ parse_one_field_descriptor("L;", 0) };
        check("fielddesc_empty_object_name_L_semicolon_accepted_consume_2",
              f.ok && f.basic_type == 12 && f.consumed == 2);
    }

    // ---- method descriptor: object-name token bytes do not desync the walk ----
    // A parameter object whose name contains ')' must NOT be mistaken for the
    // method's closing paren — the walk consumes the object through its ';'
    // first, then meets the real ')'.  One arg, one slot, int return.
    {
        const method_descriptor_parse m{
            parse_method_descriptor("(Lhas)paren;)I") };
        check("methoddesc_param_object_name_with_rparen_byte_one_arg_int_ret",
              m.ok && m.arg_count == 1 && m.arg_slots == 1
              && m.return_basic == 10 && m.return_dims == 0);
    }
    // A RETURN object whose name contains '(' and ')' bytes: rfind-free full walk
    // still classifies it T_OBJECT after consuming the param list.
    {
        const method_descriptor_parse m{
            parse_method_descriptor("(I)Lret(with)parens;") };
        check("methoddesc_return_object_name_with_paren_bytes_is_object_12",
              m.ok && m.arg_count == 1 && m.arg_slots == 1
              && m.return_basic == 12 && m.return_dims == 0);
    }

    // ---- method descriptor: maximal-width slot accumulation ------------------
    // 8 doubles + 8 longs interleaved: 16 args, 32 slots (every one is two-slot).
    // Proves the slot accumulator does not saturate or wrap on an all-wide pack.
    {
        std::string all_wide{ "(" };
        for (int i{ 0 }; i < 8; ++i) { all_wide += "DJ"; }
        all_wide += ")V";
        const method_descriptor_parse m{ parse_method_descriptor(all_wide) };
        check("methoddesc_8double_8long_interleaved_is_16args_32slots",
              m.ok && m.arg_count == 16 && m.arg_slots == 32
              && m.return_basic == 14);
    }
    // A pack alternating single- and two-slot types so arg_count and arg_slots
    // diverge by a known amount: (I J I J I J) -> 6 args, 9 slots.
    {
        const method_descriptor_parse m{ parse_method_descriptor("(IJIJIJ)V") };
        check("methoddesc_alternating_int_long_is_6args_9slots",
              m.ok && m.arg_count == 6 && m.arg_slots == 9);
    }

    // ---- method descriptor: a wide return after a wide param -----------------
    // (J)D — one long param (2 slots), double return (T_DOUBLE 7).  The return's
    // own width is irrelevant to arg_slots; only PARAMS count toward slots.
    {
        const method_descriptor_parse m{ parse_method_descriptor("(J)D") };
        check("methoddesc_long_param_double_return_2slots_doubleret",
              m.ok && m.arg_count == 1 && m.arg_slots == 2
              && m.return_basic == 7 && m.return_dims == 0);
    }

    // ---- fresh MALFORMED batch (shapes not in the pass-6 list) ---------------
    // More structural faults, each must reject (ok=false) with no overrun.
    {
        const char* more_bad_methods[]{
            "()I extra",              // trailing space+letter after primitive ret
            "(I)Ljava/lang/String",   // return object missing terminating ';'
            "([V)V",                  // array of 'V' (V is not a valid element)
            "(II))V",                 // doubled close paren before return
            "((I)V",                  // doubled open paren (param 'I' then stray)
            "(I;)V",                  // stray ';' where a descriptor is expected
            "(Ljava/lang/String;",    // object param ok but no ')' and no return
            "()[",                    // array return marker with no element
            "()L;extra",              // empty object return then trailing junk
            "(D)Vx",                  // void return not the final byte
            "( )V",                   // space is not a valid parameter descriptor
        };
        bool all_rejected{ true };
        for (const char* d : more_bad_methods)
        {
            if (parse_method_descriptor(d).ok) { all_rejected = false; }
        }
        check("methoddesc_fresh_malformed_batch_all_rejected_no_overrun", all_rejected);
    }
    // A method descriptor containing an embedded NUL inside the param region:
    // the NUL is an unknown field byte -> the param walk fails -> whole reject,
    // and the string_view's size (not the NUL) bounds the scan.
    {
        const char with_nul[]{ '(', 'I', '\0', 'J', ')', 'V', '\0' };
        const std::string_view sv{ with_nul, 6 };
        check("methoddesc_embedded_nul_param_byte_rejected",
              !parse_method_descriptor(sv).ok);
    }
    // ...whereas the SAME bytes with the NUL as the WHOLE element are rejected at
    // field level too (NUL is sig_char_to_basic_type==12 but width==0, so it is
    // not a primitive and not 'L'/'[' -> not a valid field lead).
    {
        const char nul_field[]{ '\0', '\0' };
        check("fielddesc_lone_nul_byte_rejected",
              !parse_one_field_descriptor(std::string_view{ nul_field, 1 }, 0).ok);
    }

    // ---- fresh malformed FIELD batch -----------------------------------------
    {
        const char* more_bad_fields[]{
            "[V",                     // array of void
            "[[V",                    // 2-D array of void
            "[;",                     // array of stray ';'
            "II",                     // two primitives is not ONE field descriptor
                                      //   (parse_one consumes only the first 'I';
                                      //   ok=true with consumed==1 — so this is
                                      //   asserted via consumed, NOT via !ok, below)
            "L/no/leading/semicolon", // object missing ';'
        };
        // The first three and the last are outright invalid (ok=false); "II"
        // is the special case where parse_one SUCCEEDS on the first byte only.
        check("fielddesc_array_of_void_1d_rejected",
              !parse_one_field_descriptor(more_bad_fields[0], 0).ok);
        check("fielddesc_array_of_void_2d_rejected",
              !parse_one_field_descriptor(more_bad_fields[1], 0).ok);
        check("fielddesc_array_of_stray_semicolon_rejected",
              !parse_one_field_descriptor(more_bad_fields[2], 0).ok);
        check("fielddesc_object_no_terminator_rejected",
              !parse_one_field_descriptor(more_bad_fields[4], 0).ok);
        // "II": ONE field descriptor parse consumes exactly the first 'I' and
        // STOPS — it does not greedily swallow the trailing 'I'.  This is the
        // contract parse_method_descriptor relies on to advance arg-by-arg.
        {
            const field_descriptor_parse f{ parse_one_field_descriptor("II", 0) };
            check("fielddesc_two_primitives_parses_only_first_consume_1",
                  f.ok && f.basic_type == 10 && f.consumed == 1);
        }
    }

    // ---- whole-descriptor round-trip: param-count == sum over fields ----------
    // For a well-formed descriptor, walking each parameter field one at a time
    // (using parse_one_field_descriptor + consumed to advance) must reproduce the
    // same arg_count and arg_slots parse_method_descriptor reports.  This pins the
    // two code paths (single-field walk vs whole-method walk) agree, exercising
    // the `consumed`-driven advance directly on a non-trivial mixed signature.
    {
        const std::string_view sig{ "(IJLjava/lang/Object;[DF)Ljava/lang/String;" };
        // Manual field-by-field walk of the parameter region.
        int    manual_args{ 0 };
        int    manual_slots{ 0 };
        bool   manual_ok{ true };
        std::size_t pos{ 1 }; // skip '('
        while (pos < sig.size() && sig[pos] != ')')
        {
            const field_descriptor_parse f{ parse_one_field_descriptor(sig, pos) };
            if (!f.ok || f.consumed == 0) { manual_ok = false; break; }
            ++manual_args;
            manual_slots += (f.basic_type == 11 || f.basic_type == 7) ? 2 : 1;
            pos += f.consumed;
        }
        const method_descriptor_parse m{ parse_method_descriptor(sig) };
        check("methoddesc_manual_field_walk_matches_whole_parse",
              manual_ok && m.ok
              && m.arg_count == manual_args && m.arg_slots == manual_slots
              && m.arg_count == 5 && m.arg_slots == 6
              && m.return_basic == 12);
    }

    return failures == 0 ? 0 : 1;
}
