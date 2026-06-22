// Standalone (no-JVM) unit tests for method_proxy::value_t variant conversions
// and method_proxy API-surface helpers (is_reference / is_static / raw_method /
// name / signature on a proxy built with a null Method*).
//
// SCOPE NOTE: This file runs with NO JVM in-process. It therefore only exercises
//   * value_t variant -> C++ type conversion (pure logic, the std::visit operator)
//   * method_proxy accessor helpers that do not touch the JVM
//   * the void*/compressed-OOP decode seam (decode_oop_pointer is null-safe with
//     no VMStructs: it returns nullptr instead of crashing, see vmhook.hpp:4229
//     and 4280-4283).
// Anything that needs a live oop or a running JVM (method_proxy::call(),
// call_jni(), real String/object/array decode, slot dispatch) is OUT OF SCOPE
// here and is covered by JVM integration in example.cpp.
//
// value_t alternatives (vmhook.hpp:11519-11531):
//   monostate, bool, int8_t, int16_t, int32_t, int64_t, float, double,
//   uint16_t, uint32_t (compressed OOP), std::string.
// value_t is an aggregate with a single std::variant member, so value_t{ X }
// aggregate-initialises the variant from X; the templated conversion operator
// is a member function and does not suppress aggregate init.

#include <vmhook/vmhook.hpp>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

using value_t = vmhook::method_proxy::value_t;

// Minimal vmhook wrapper type for exercising the value_t -> std::unique_ptr<T>
// conversion arm.  The arm static_asserts that T derives from
// vmhook::object_base and constructs `new T{ decoded_void_ptr }`; object_base's
// constructor takes oop_type_t (== void*), which W inherits.  W adds no state,
// so constructing/destructing it is trivial — but with NO JVM the arm never
// actually news a W (decode_oop_pointer returns nullptr, so the !decoded guard
// fires first and the arm returns a null unique_ptr).
struct W : vmhook::object_base
{
    using vmhook::object_base::object_base;
};

// Bit-exact comparison helpers.  `==` on floating point treats every NaN as
// unequal and collapses -0.0 == +0.0, so for the bit-faithfulness assertions we
// compare the raw object representation byte-for-byte via memcpy (the only
// portable, type-punning-UB-free spelling).  This pins that a conversion that
// is mathematically EXACT (every finite float -> double, an exactly-
// representable double -> float, ±0, ±Inf — all guaranteed by IEEE-754 with no
// implementation latitude) produces the EXACT same bits the equivalent direct
// static_cast would, on both MinGW (libstdc++) and MSVC (STL).  NaN payloads are
// NOT portable, so NaN is asserted via std::isnan elsewhere — never by bits.
static auto bits_equal_f(const float a, const float b) noexcept -> bool
{
    std::uint32_t ba{}, bb{};
    std::memcpy(&ba, &a, sizeof(ba));
    std::memcpy(&bb, &b, sizeof(bb));
    return ba == bb;
}
static auto bits_equal_d(const double a, const double b) noexcept -> bool
{
    std::uint64_t ba{}, bb{};
    std::memcpy(&ba, &a, sizeof(ba));
    std::memcpy(&bb, &b, sizeof(bb));
    return ba == bb;
}

int main()
{
    // -------------------------------------------------------------------------
    // value_t: monostate -> default-constructed target (the void-return / failure
    // contract). monostate cannot be static_cast to an arithmetic/pointer type,
    // so the operator's `else` arm returns target_type{}.
    // -------------------------------------------------------------------------
    check("monostate_to_int_is_zero",
          static_cast<std::int32_t>(value_t{ std::monostate{} }) == 0);
    check("monostate_to_int64_is_zero",
          static_cast<std::int64_t>(value_t{ std::monostate{} }) == 0);
    check("monostate_to_float_is_zero",
          static_cast<float>(value_t{ std::monostate{} }) == 0.0f);
    check("monostate_to_double_is_zero",
          static_cast<double>(value_t{ std::monostate{} }) == 0.0);
    check("monostate_to_bool_is_false",
          static_cast<bool>(value_t{ std::monostate{} }) == false);
    check("monostate_to_voidptr_is_null",
          static_cast<void*>(value_t{ std::monostate{} }) == nullptr);
    {
        // monostate -> std::string falls into the default arm (empty string).
        // The conversion operator is now constrained (BUG 1 fix), so
        // static_cast<std::string>(value_t) is unambiguous; as_string() is the
        // ergonomic public spelling for the same extraction and is used here.
        // (Brace-init `std::string s{ value_t }` is still avoided: copy-list-init
        // considers every viable user-defined conversion and can pick a
        // surprising one — a C++ gotcha independent of the operator's logic.)
        const auto s = value_t{ std::monostate{} }.as_string();
        check("monostate_to_string_is_empty", s.empty());
        // The constrained static_cast also yields the empty string here.
        check("monostate_static_cast_string_is_empty",
              static_cast<std::string>(value_t{ std::monostate{} }).empty());
    }

    // -------------------------------------------------------------------------
    // value_t: primitive round-trip when target type matches the stored type.
    // -------------------------------------------------------------------------
    check("bool_true_round_trips",
          static_cast<bool>(value_t{ true }) == true);
    check("bool_false_round_trips",
          static_cast<bool>(value_t{ false }) == false);
    check("int32_round_trips",
          static_cast<std::int32_t>(value_t{ std::int32_t{ 123456 } }) == 123456);
    check("int64_round_trips",
          static_cast<std::int64_t>(value_t{ std::int64_t{ 9000000000LL } }) == 9000000000LL);
    check("uint16_round_trips",
          static_cast<std::uint16_t>(value_t{ std::uint16_t{ 0xBEEF } }) == 0xBEEF);
    check("float_round_trips",
          static_cast<float>(value_t{ 1.5f }) == 1.5f);
    check("double_round_trips",
          static_cast<double>(value_t{ 2.25 }) == 2.25);

    // -------------------------------------------------------------------------
    // value_t: signed narrow alternatives keep their sign through int conversion
    // (int8_t / int16_t hold -1, NOT 255 / 65535).
    // -------------------------------------------------------------------------
    check("int8_negative_one_sign_preserved",
          static_cast<std::int32_t>(value_t{ std::int8_t{ -1 } }) == -1);
    check("int16_negative_one_sign_preserved",
          static_cast<std::int32_t>(value_t{ std::int16_t{ -1 } }) == -1);
    check("int8_to_self_is_negative_one",
          static_cast<std::int8_t>(value_t{ std::int8_t{ -1 } }) == std::int8_t{ -1 });

    // -------------------------------------------------------------------------
    // value_t: cross-arithmetic static_cast path (stored float -> double, etc.).
    // -------------------------------------------------------------------------
    check("float_widens_to_double",
          static_cast<double>(value_t{ 1.5f }) == 1.5);
    check("int32_narrows_to_int8_wraps",
          static_cast<std::int8_t>(value_t{ std::int32_t{ 257 } }) == std::int8_t{ 1 });
    check("int64_truncates_to_int32",
          static_cast<std::int32_t>(value_t{ std::int64_t{ 0x1'0000'0001LL } }) == 1);

    // -------------------------------------------------------------------------
    // value_t: std::string alternative. The call_jni String path stores a
    // std::string directly; verify it converts back out unchanged, and that a
    // std::string stored value cannot be coerced to int (default 0 via the else
    // arm, since static_cast<int>(std::string) is ill-formed).
    // -------------------------------------------------------------------------
    {
        const auto s = value_t{ std::string{ "hello" } }.as_string();
        check("string_alternative_round_trips", s == "hello");
    }
    {
        const auto s = value_t{ std::string{ "" } }.as_string();
        check("empty_string_alternative_round_trips", s.empty());
    }
    check("string_alternative_to_int_is_zero",
          static_cast<std::int32_t>(value_t{ std::string{ "123" } }) == 0);
    check("string_alternative_to_voidptr_is_null",
          static_cast<void*>(value_t{ std::string{ "x" } }) == nullptr);

    // -------------------------------------------------------------------------
    // value_t: uint32_t (compressed-OOP) -> void* is routed through
    // decode_oop_pointer, NOT a plain static_cast.
    //
    // Proof that the special-case fires WITHOUT a JVM:
    //   * a plain static_cast<void*>(uint32_t{42}) would yield (void*)42,
    //   * decode_oop_pointer(42) with no VMStructs yields nullptr
    //     (vmhook.hpp:4280-4283), so the result is nullptr != (void*)42.
    // For the zero case decode_oop_pointer short-circuits to nullptr
    // (vmhook.hpp:4229).
    // -------------------------------------------------------------------------
    check("uint32_zero_to_voidptr_is_null",
          static_cast<void*>(value_t{ std::uint32_t{ 0 } }) == nullptr);
    check("uint32_nonzero_to_voidptr_uses_decode_not_truncation",
          static_cast<void*>(value_t{ std::uint32_t{ 42 } })
              != reinterpret_cast<void*>(static_cast<std::uintptr_t>(42)));
    check("uint32_nonzero_to_voidptr_matches_decode_oop_pointer",
          static_cast<void*>(value_t{ std::uint32_t{ 0xDEADBEEF } })
              == vmhook::hotspot::decode_oop_pointer(0xDEADBEEFu));
    // The uint32_t alternative still static_casts to integer targets normally
    // (only the void* target gets the decode treatment).
    check("uint32_to_int_is_plain_static_cast",
          static_cast<std::int64_t>(value_t{ std::uint32_t{ 42 } }) == 42);
    check("uint32_to_uint32_round_trips",
          static_cast<std::uint32_t>(value_t{ std::uint32_t{ 0xCAFEBABE } }) == 0xCAFEBABEu);

    // -------------------------------------------------------------------------
    // value_t: compile-time conversion-operator surface. These pin the
    // convertible target set so a future reshuffle of the variant alternatives
    // is caught at build time.
    // -------------------------------------------------------------------------
    static_assert(std::is_convertible_v<value_t, bool>,           "value_t -> bool");
    static_assert(std::is_convertible_v<value_t, std::int32_t>,   "value_t -> int32");
    static_assert(std::is_convertible_v<value_t, std::int64_t>,   "value_t -> int64");
    static_assert(std::is_convertible_v<value_t, float>,          "value_t -> float");
    static_assert(std::is_convertible_v<value_t, double>,         "value_t -> double");
    static_assert(std::is_convertible_v<value_t, void*>,          "value_t -> void*");
    static_assert(std::is_convertible_v<value_t, std::string>,    "value_t -> string");
    static_assert(std::is_convertible_v<value_t, std::uint16_t>,  "value_t -> uint16");
    check("value_t_compile_time_conversions_present", true);

    // -------------------------------------------------------------------------
    // BUG 1 (FIXED) — the constrained conversion operator.  Compile-time proof
    // that the NATURAL cast forms are now well-formed (they were ambiguous on
    // MSVC /permissive- before the value_t_convertible_target_v constraint), and
    // that the spurious productions which caused the ambiguity are now EXCLUDED
    // from the operator's target set.  These are detected with a requires-expr so
    // a regression (operator un-constrained again => ambiguous => not well-formed,
    // or constraint too tight => legitimate cast removed) fails the build.
    // -------------------------------------------------------------------------
    // The two cast forms that were the whole point of the fix are now well-formed:
    static_assert(requires(const value_t& v) { static_cast<std::string>(v); },
                  "BUG 1: static_cast<std::string>(value_t) must be well-formed");
    static_assert(requires(const value_t& v) { static_cast<std::unique_ptr<W>>(v); },
                  "BUG 1: static_cast<std::unique_ptr<W>>(value_t) must be well-formed");
    // Legitimate targets remain convertible (constraint not over-tight):
    static_assert(std::is_convertible_v<value_t, std::unique_ptr<W>>,
                  "value_t -> unique_ptr<W> must remain convertible");
    static_assert(std::is_convertible_v<value_t, void*>,
                  "value_t -> void* must remain convertible (the one allowed pointer)");
    // The spurious targets are now EXCLUDED — value_t is NOT convertible to a
    // char pointer, a wrapper pointer, or std::nullptr_t (these are exactly the
    // productions that made the class-target casts ambiguous).
    static_assert(!std::is_convertible_v<value_t, const char*>,
                  "BUG 1: value_t must NOT be convertible to const char* (ambiguity source)");
    static_assert(!std::is_convertible_v<value_t, char*>,
                  "BUG 1: value_t must NOT be convertible to char*");
    static_assert(!std::is_convertible_v<value_t, W*>,
                  "BUG 1: value_t must NOT be convertible to W* (unique_ptr ambiguity source)");
    static_assert(!std::is_convertible_v<value_t, std::nullptr_t>,
                  "BUG 1: value_t must NOT be convertible to std::nullptr_t");
    check("value_t_bug1_constraint_present", true);

    // -------------------------------------------------------------------------
    // method_proxy built with a NULL Method* (no JVM): accessor round-trips.
    // The (object, method*, signature) constructor just stores fields; it does
    // not touch the JVM, so this is safe with no VMStructs present.
    // -------------------------------------------------------------------------
    {
        // Reference (object) return type: "(I)Ljava/lang/String;"
        vmhook::method_proxy proxy{ nullptr, nullptr, std::string{ "(I)Ljava/lang/String;" } };

        check("null_method_name_is_empty", proxy.name().empty());
        check("null_method_signature_round_trips",
              proxy.signature() == std::string_view{ "(I)Ljava/lang/String;" });
        check("null_method_raw_method_is_null", proxy.raw_method() == nullptr);

        // is_static() reflects the constructor's hardcoded static_field=false,
        // independent of the null object pointer (documented contract).
        check("constructed_proxy_is_static_false", proxy.is_static() == false);

        // is_reference(): char after ')' is 'L' -> true.
        check("is_reference_true_for_L_return", proxy.is_reference() == true);
    }

    // is_reference(): array return "[" -> true.
    {
        vmhook::method_proxy proxy{ nullptr, nullptr, std::string{ "()[I" } };
        check("is_reference_true_for_array_return", proxy.is_reference() == true);
        check("array_proxy_signature_round_trips",
              proxy.signature() == std::string_view{ "()[I" });
    }

    // is_reference(): primitive return 'I' -> false.
    {
        vmhook::method_proxy proxy{ nullptr, nullptr, std::string{ "(I)I" } };
        check("is_reference_false_for_int_return", proxy.is_reference() == false);
    }

    // is_reference(): void return 'V' -> false.
    {
        vmhook::method_proxy proxy{ nullptr, nullptr, std::string{ "(I)V" } };
        check("is_reference_false_for_void_return", proxy.is_reference() == false);
    }

    // is_reference(): malformed signature (no ')') -> false, never throws.
    {
        vmhook::method_proxy proxy{ nullptr, nullptr, std::string{ "garbage" } };
        check("is_reference_false_for_malformed_signature", proxy.is_reference() == false);
        check("malformed_signature_round_trips",
              proxy.signature() == std::string_view{ "garbage" });
    }

    // is_reference(): empty signature -> false (find(')') == npos branch).
    {
        vmhook::method_proxy proxy{ nullptr, nullptr, std::string{ "" } };
        check("is_reference_false_for_empty_signature", proxy.is_reference() == false);
        check("empty_signature_name_still_empty", proxy.name().empty());
    }

    // is_reference(): trailing ')' with nothing after it -> false
    // (close + 1 >= size() guard).
    {
        vmhook::method_proxy proxy{ nullptr, nullptr, std::string{ "(I)" } };
        check("is_reference_false_for_truncated_after_paren", proxy.is_reference() == false);
    }

    // -------------------------------------------------------------------------
    // value_t: boundary integral values round-trip when the target matches the
    // stored alternative.  Every expected value follows the static_cast path in
    // the conversion operator (vmhook.hpp:13108-13111).
    // -------------------------------------------------------------------------
    check("int32_min_round_trips",
          static_cast<std::int32_t>(value_t{ std::int32_t{ -2147483647 - 1 } })
              == (-2147483647 - 1));
    check("int32_max_round_trips",
          static_cast<std::int32_t>(value_t{ std::int32_t{ 2147483647 } }) == 2147483647);
    check("int64_min_round_trips",
          static_cast<std::int64_t>(value_t{ std::int64_t{ -9223372036854775807LL - 1 } })
              == (-9223372036854775807LL - 1));
    check("int64_max_round_trips",
          static_cast<std::int64_t>(value_t{ std::int64_t{ 9223372036854775807LL } })
              == 9223372036854775807LL);
    check("int8_min_round_trips",
          static_cast<std::int8_t>(value_t{ std::int8_t{ -128 } }) == std::int8_t{ -128 });
    check("int8_max_round_trips",
          static_cast<std::int8_t>(value_t{ std::int8_t{ 127 } }) == std::int8_t{ 127 });
    check("int16_min_round_trips",
          static_cast<std::int16_t>(value_t{ std::int16_t{ -32768 } }) == std::int16_t{ -32768 });
    check("int16_max_round_trips",
          static_cast<std::int16_t>(value_t{ std::int16_t{ 32767 } }) == std::int16_t{ 32767 });
    check("uint16_zero_round_trips",
          static_cast<std::uint16_t>(value_t{ std::uint16_t{ 0 } }) == 0u);
    check("uint16_max_round_trips",
          static_cast<std::uint16_t>(value_t{ std::uint16_t{ 0xFFFF } }) == 0xFFFFu);

    // -------------------------------------------------------------------------
    // value_t: signed alternatives widen with sign preserved into wider targets.
    // -------------------------------------------------------------------------
    check("int8_min_widens_to_int32_signed",
          static_cast<std::int32_t>(value_t{ std::int8_t{ -128 } }) == -128);
    check("int16_min_widens_to_int64_signed",
          static_cast<std::int64_t>(value_t{ std::int16_t{ -32768 } }) == -32768LL);
    check("int32_min_widens_to_int64_signed",
          static_cast<std::int64_t>(value_t{ std::int32_t{ -2147483647 - 1 } })
              == static_cast<std::int64_t>(-2147483647 - 1));
    // uint16 max widens to a POSITIVE 65535 (unsigned source, no sign bit).
    check("uint16_max_widens_to_positive_int32",
          static_cast<std::int32_t>(value_t{ std::uint16_t{ 0xFFFF } }) == 65535);

    // -------------------------------------------------------------------------
    // value_t: more cross-arithmetic narrowing / widening via static_cast.
    // -------------------------------------------------------------------------
    check("int32_minus_one_narrows_to_uint16_wraps",
          static_cast<std::uint16_t>(value_t{ std::int32_t{ -1 } }) == 0xFFFFu);
    check("int64_low_bits_truncate_to_int16",
          static_cast<std::int16_t>(value_t{ std::int64_t{ 0x7777'0000'0000'1234LL } })
              == std::int16_t{ 0x1234 });
    check("double_truncates_to_int32",
          static_cast<std::int32_t>(value_t{ 3.99 }) == 3);
    check("double_negative_truncates_toward_zero",
          static_cast<std::int32_t>(value_t{ -3.99 }) == -3);
    check("int32_widens_to_double_exactly",
          static_cast<double>(value_t{ std::int32_t{ 16777216 } }) == 16777216.0);
    check("bool_true_to_int_is_one",
          static_cast<std::int32_t>(value_t{ true }) == 1);
    check("int32_nonzero_to_bool_is_true",
          static_cast<bool>(value_t{ std::int32_t{ 5 } }) == true);
    check("int32_zero_to_bool_is_false",
          static_cast<bool>(value_t{ std::int32_t{ 0 } }) == false);

    // -------------------------------------------------------------------------
    // value_t: float boundary values survive the float->float / float->double
    // static_cast path unchanged.
    // -------------------------------------------------------------------------
    check("float_zero_round_trips",
          static_cast<float>(value_t{ 0.0f }) == 0.0f);
    check("float_negative_round_trips",
          static_cast<float>(value_t{ -2.5f }) == -2.5f);
    check("double_negative_round_trips",
          static_cast<double>(value_t{ -123.5 }) == -123.5);
    check("float_large_exact_power_of_two_round_trips",
          static_cast<float>(value_t{ 1048576.0f }) == 1048576.0f);

    // -------------------------------------------------------------------------
    // value_t: as_string() on every NON-string / NON-OOP alternative returns ""
    // (vmhook.hpp:13150-13169: only string -> as-is, uint32_t -> read_java_string;
    // every numeric/bool/monostate alternative yields "").  With no JVM the
    // uint32_t path would call read_java_string on a decoded null and also yield
    // "", but we keep those OUT (read_java_string touches the JVM seam) and only
    // assert the pure numeric/monostate arm here.
    // -------------------------------------------------------------------------
    check("as_string_monostate_empty", value_t{ std::monostate{} }.as_string().empty());
    check("as_string_bool_empty",      value_t{ true }.as_string().empty());
    check("as_string_int8_empty",      value_t{ std::int8_t{ 7 } }.as_string().empty());
    check("as_string_int16_empty",     value_t{ std::int16_t{ 7 } }.as_string().empty());
    check("as_string_int32_empty",     value_t{ std::int32_t{ 7 } }.as_string().empty());
    check("as_string_int64_empty",     value_t{ std::int64_t{ 7 } }.as_string().empty());
    check("as_string_float_empty",     value_t{ 1.5f }.as_string().empty());
    check("as_string_double_empty",    value_t{ 2.5 }.as_string().empty());
    check("as_string_uint16_empty",    value_t{ std::uint16_t{ 7 } }.as_string().empty());
    // The std::string alternative is returned verbatim by as_string().
    check("as_string_string_alternative_verbatim",
          value_t{ std::string{ "verbatim" } }.as_string() == "verbatim");

    // -------------------------------------------------------------------------
    // value_t: is_void() / is_string() introspection (vmhook.hpp:13126-13137).
    // is_void() is true ONLY for the monostate alternative; is_string() ONLY for
    // the std::string alternative.  No numeric alternative reports either.
    // -------------------------------------------------------------------------
    check("is_void_true_for_monostate", value_t{ std::monostate{} }.is_void() == true);
    check("is_void_false_for_int", value_t{ std::int32_t{ 0 } }.is_void() == false);
    check("is_void_false_for_string", value_t{ std::string{ "" } }.is_void() == false);
    check("is_void_false_for_uint32", value_t{ std::uint32_t{ 0 } }.is_void() == false);
    check("is_string_true_for_string", value_t{ std::string{ "x" } }.is_string() == true);
    check("is_string_false_for_monostate", value_t{ std::monostate{} }.is_string() == false);
    check("is_string_false_for_int", value_t{ std::int32_t{ 1 } }.is_string() == false);
    check("is_string_false_for_uint32", value_t{ std::uint32_t{ 1 } }.is_string() == false);
    // is_void and is_string are mutually exclusive for these two alternatives.
    check("monostate_is_void_not_string",
          value_t{ std::monostate{} }.is_void() && !value_t{ std::monostate{} }.is_string());
    check("string_is_string_not_void",
          value_t{ std::string{ "s" } }.is_string() && !value_t{ std::string{ "s" } }.is_void());

    // -------------------------------------------------------------------------
    // value_t: uint32_t (compressed-OOP) -> void* decode path, more values.
    // With no JVM decode_oop_pointer returns nullptr for any non-zero input
    // (gHotSpotVMStructs absent), and exactly matches the helper for each input.
    // -------------------------------------------------------------------------
    check("uint32_one_to_voidptr_matches_decode",
          static_cast<void*>(value_t{ std::uint32_t{ 1 } })
              == vmhook::hotspot::decode_oop_pointer(1u));
    check("uint32_max_to_voidptr_matches_decode",
          static_cast<void*>(value_t{ std::uint32_t{ 0xFFFF'FFFF } })
              == vmhook::hotspot::decode_oop_pointer(0xFFFF'FFFFu));
    check("uint32_midrange_to_voidptr_matches_decode",
          static_cast<void*>(value_t{ std::uint32_t{ 0x8000'0000 } })
              == vmhook::hotspot::decode_oop_pointer(0x8000'0000u));
    // The uint32_t alternative still static_casts to integral targets normally.
    check("uint32_max_to_int64_is_plain_value",
          static_cast<std::int64_t>(value_t{ std::uint32_t{ 0xFFFF'FFFF } }) == 4294967295LL);
    check("uint32_to_int32_truncates_via_static_cast",
          static_cast<std::int32_t>(value_t{ std::uint32_t{ 0xFFFF'FFFF } }) == -1);

    // -------------------------------------------------------------------------
    // value_t: more conversion-operator surface pins (compile-time).  The narrow
    // signed/unsigned integer targets must all be reachable through the operator.
    // -------------------------------------------------------------------------
    static_assert(std::is_convertible_v<value_t, std::int8_t>,    "value_t -> int8");
    static_assert(std::is_convertible_v<value_t, std::int16_t>,   "value_t -> int16");
    static_assert(std::is_convertible_v<value_t, std::uint32_t>,  "value_t -> uint32");
    check("value_t_narrow_integer_conversions_present", true);

    // -------------------------------------------------------------------------
    // method_proxy: more is_reference() signature edge cases (vmhook.hpp:14093).
    // The discriminator is the single char immediately AFTER ')'.
    // -------------------------------------------------------------------------
    {
        // Multi-arg signature returning an object: still 'L' after ')'.
        vmhook::method_proxy p{ nullptr, nullptr,
                                std::string{ "(ILjava/lang/String;[I)Ljava/lang/Object;" } };
        check("is_reference_true_multi_arg_object_return", p.is_reference() == true);
    }
    {
        // Returns a 2D array -> first char after ')' is '[' -> true.
        vmhook::method_proxy p{ nullptr, nullptr, std::string{ "()[[J" } };
        check("is_reference_true_for_2d_array_return", p.is_reference() == true);
    }
    {
        // Long return 'J' -> primitive -> false.
        vmhook::method_proxy p{ nullptr, nullptr, std::string{ "()J" } };
        check("is_reference_false_for_long_return", p.is_reference() == false);
    }
    {
        // Double return 'D' -> primitive -> false.
        vmhook::method_proxy p{ nullptr, nullptr, std::string{ "()D" } };
        check("is_reference_false_for_double_return", p.is_reference() == false);
    }
    {
        // Boolean return 'Z' -> primitive -> false.
        vmhook::method_proxy p{ nullptr, nullptr, std::string{ "()Z" } };
        check("is_reference_false_for_boolean_return", p.is_reference() == false);
    }
    {
        // Only a ')' as the very last char with args before it: close+1 == size()
        // -> guard rejects -> false.
        vmhook::method_proxy p{ nullptr, nullptr, std::string{ "(Ljava/lang/String;)" } };
        check("is_reference_false_when_paren_is_last_char", p.is_reference() == false);
    }
    {
        // No '(' at all but a ')' present, char after is 'L' -> the parser only
        // looks for ')' then the next char, so this still reports true.  (Pins
        // the documented behaviour: it does NOT validate the arg list.)
        vmhook::method_proxy p{ nullptr, nullptr, std::string{ ")Lx;" } };
        check("is_reference_true_when_only_close_paren_then_L", p.is_reference() == true);
    }

    // =========================================================================
    // EXPANSION (no-JVM, platform-invariant): exhaustive value_t conversion
    // matrix + introspection over EVERY variant alternative, plus the
    // unique_ptr arm and get_compressed_oop.  Every OOP-touching arm short-
    // circuits to null/"" with no VMStructs (decode_oop_pointer returns nullptr
    // for any input, read_java_string returns "" for a null oop), so all of
    // this is deterministic and crash-free on both MinGW and MSVC.
    // =========================================================================

    // -------------------------------------------------------------------------
    // is_void() / is_string() over ALL eleven alternatives.  is_void() is true
    // ONLY for monostate; is_string() ONLY for the std::string alternative.
    // The uint32_t (reference) alternative is NEITHER — it pins that the
    // introspection is keyed on the variant slot, not on "could this decode to
    // a String" (the static-descriptor-vs-runtime-type gap).
    // -------------------------------------------------------------------------
    check("is_void_bool_false",   value_t{ true }.is_void() == false);
    check("is_void_int8_false",   value_t{ std::int8_t{ 1 } }.is_void() == false);
    check("is_void_int16_false",  value_t{ std::int16_t{ 1 } }.is_void() == false);
    check("is_void_int64_false",  value_t{ std::int64_t{ 1 } }.is_void() == false);
    check("is_void_float_false",  value_t{ 1.0f }.is_void() == false);
    check("is_void_double_false", value_t{ 1.0 }.is_void() == false);
    check("is_void_uint16_false", value_t{ std::uint16_t{ 1 } }.is_void() == false);
    check("is_string_bool_false",   value_t{ true }.is_string() == false);
    check("is_string_int8_false",   value_t{ std::int8_t{ 1 } }.is_string() == false);
    check("is_string_int16_false",  value_t{ std::int16_t{ 1 } }.is_string() == false);
    check("is_string_int64_false",  value_t{ std::int64_t{ 1 } }.is_string() == false);
    check("is_string_float_false",  value_t{ 1.0f }.is_string() == false);
    check("is_string_double_false", value_t{ 1.0 }.is_string() == false);
    check("is_string_uint16_false", value_t{ std::uint16_t{ 1 } }.is_string() == false);
    // The reference (uint32_t) alternative is neither void nor string.
    check("uint32_alternative_is_neither_void_nor_string",
          !value_t{ std::uint32_t{ 7 } }.is_void()
              && !value_t{ std::uint32_t{ 7 } }.is_string());
    // An empty std::string is still the string alternative (content-independent).
    check("empty_string_alternative_is_string",
          value_t{ std::string{ "" } }.is_string() == true);
    check("empty_string_alternative_is_not_void",
          value_t{ std::string{ "" } }.is_void() == false);

    // -------------------------------------------------------------------------
    // as_string() over the uint32_t (reference / compressed-OOP) alternative.
    // With no JVM:  uint32_t{0} -> decode short-circuits to nullptr -> "".
    //               uint32_t{nonzero} -> decode returns nullptr (no VMStructs)
    //                                  -> read_java_string(nullptr) -> "".
    // Proves crash-freedom AND the no-VMStruct contract for the OOP arm of
    // as_string() (previously only monostate / std::string were covered).
    // -------------------------------------------------------------------------
    check("as_string_uint32_zero_empty",
          value_t{ std::uint32_t{ 0 } }.as_string().empty());
    check("as_string_uint32_nonzero_empty_no_jvm",
          value_t{ std::uint32_t{ 0xDEADBEEF } }.as_string().empty());
    check("as_string_uint32_one_empty_no_jvm",
          value_t{ std::uint32_t{ 1 } }.as_string().empty());
    check("as_string_uint32_max_empty_no_jvm",
          value_t{ std::uint32_t{ 0xFFFF'FFFF } }.as_string().empty());

    // -------------------------------------------------------------------------
    // as_string() must AGREE with the conversion operator's std::string arm on
    // the std::string alternative (both return the stored bytes verbatim).
    //
    // BUG 1 (FIXED): the conversion operator was an UNCONSTRAINED template, so it
    // was also convertible to const char* / char* / std::nullptr_t — every one a
    // std::string constructor argument — which made `static_cast<std::string>(v)`
    // AMBIGUOUS on MSVC /permissive- (C2440).  The operator is now constrained
    // (vmhook::detail::value_t_convertible_target_v) to exclude those spurious
    // pointer/nullptr targets, leaving void* as the only producible pointer, so
    // the cast resolves to the single value_t->std::string conversion on every
    // compiler.  We exercise BOTH the natural static_cast AND the explicit
    // operator spelling and require they agree.  as_string() remains the
    // ergonomic public escape hatch for the same extraction.
    // -------------------------------------------------------------------------
    {
        const value_t v{ std::string{ "agree" } };
        // Natural cast form — must compile now (was BUG 1: ambiguous on MSVC).
        check("static_cast_string_equals_as_string",
              static_cast<std::string>(v) == v.as_string());
        check("as_string_matches_operator_string",
              v.as_string() == v.operator std::string());
        check("static_cast_string_equals_operator_string",
              static_cast<std::string>(v) == v.operator std::string());
        // The unambiguous extraction via as_string() compiles and yields the value.
        const std::string s{ v.as_string() };
        check("as_string_unambiguous_assignment", s == "agree");
        // Copy-initialisation from the constrained operator is now unambiguous too.
        const std::string from_cast = static_cast<std::string>(v);
        check("static_cast_string_value", from_cast == "agree");
    }
    // Embedded NUL survives the std::string alternative round-trip (length-based,
    // not NUL-terminated) — a property the const char* path would have lost.
    {
        const std::string with_nul{ std::string("a\0b", 3) };
        const value_t v{ with_nul };
        check("string_alternative_preserves_embedded_nul",
              v.as_string().size() == 3 && v.as_string() == with_nul);
    }

    // -------------------------------------------------------------------------
    // value_t -> std::unique_ptr<W> (the compressed-OOP reference arm).
    //   * uint32_t{0}      -> decode short-circuits -> null unique_ptr.
    //   * uint32_t{nonzero}-> decode returns nullptr (no VMStructs) -> the
    //                         !decoded guard fires -> null unique_ptr.
    //   * a NON-uint32 stored alternative -> the `else` branch -> null.
    //   * monostate / string stored      -> also null (else branch).
    // None of these construct a W (no JVM), so they are crash-free.
    //
    // BUG 1 (FIXED): `static_cast<std::unique_ptr<W>>(v)` used to be AMBIGUOUS on
    // MSVC because the UNCONSTRAINED operator also yielded W* (which unique_ptr's
    // explicit pointer ctor accepts) and std::nullptr_t.  The operator is now
    // constrained to exclude all non-void pointers + nullptr_t, so the only
    // producible unique_ptr<W>-constructible type is unique_ptr<W> itself and the
    // direct-init cast resolves unambiguously.  We now use the NATURAL static_cast
    // spelling (with one explicit operator call kept to prove the two agree).
    // is_convertible (a single implicit conversion sequence) pins the arm exists.
    // -------------------------------------------------------------------------
    static_assert(std::is_convertible_v<value_t, std::unique_ptr<W>>,
                  "value_t -> unique_ptr<wrapper> arm must exist");
    check("unique_ptr_from_uint32_zero_is_null",
          static_cast<std::unique_ptr<W>>(value_t{ std::uint32_t{ 0 } }) == nullptr);
    check("unique_ptr_from_uint32_nonzero_is_null_no_jvm",
          static_cast<std::unique_ptr<W>>(value_t{ std::uint32_t{ 42 } }) == nullptr);
    check("unique_ptr_from_uint32_sentinel_is_null_no_jvm",
          static_cast<std::unique_ptr<W>>(value_t{ std::uint32_t{ 0xDEADBEEF } }) == nullptr);
    check("unique_ptr_from_int32_alternative_is_null",
          static_cast<std::unique_ptr<W>>(value_t{ std::int32_t{ 1 } }) == nullptr);
    check("unique_ptr_from_int64_alternative_is_null",
          static_cast<std::unique_ptr<W>>(value_t{ std::int64_t{ 1 } }) == nullptr);
    check("unique_ptr_from_monostate_is_null",
          static_cast<std::unique_ptr<W>>(value_t{ std::monostate{} }) == nullptr);
    check("unique_ptr_from_string_alternative_is_null",
          static_cast<std::unique_ptr<W>>(value_t{ std::string{ "x" } }) == nullptr);
    check("unique_ptr_from_float_alternative_is_null",
          static_cast<std::unique_ptr<W>>(value_t{ 1.0f }) == nullptr);
    // The natural cast and the explicit operator spelling agree (both null here).
    check("unique_ptr_static_cast_matches_operator",
          static_cast<std::unique_ptr<W>>(value_t{ std::uint32_t{ 42 } })
              == value_t{ std::uint32_t{ 42 } }.operator std::unique_ptr<W>());

    // -------------------------------------------------------------------------
    // uint16_t (the Java char alternative) is NOT the uint32_t reference
    // alternative, so it routes through the GENERIC arms — not the OOP special
    // cases.  Proves the reference handling is keyed on uint32_t ONLY, not on
    // "any unsigned type".
    //   * char -> std::string : the operator's std::string arm sees a non-
    //     uint32 / non-string stored type and returns target_type{} (empty),
    //     NOT a decoded String.
    //   * char -> void*       : the void* arm only fires for uint32_t, so a
    //     char falls through to target_type{} == nullptr (it is NOT decoded).
    //   * char -> unique_ptr  : non-uint32 -> null.
    // -------------------------------------------------------------------------
    check("uint16_char_to_string_is_empty_not_decoded",
          static_cast<std::string>(value_t{ std::uint16_t{ 0x0041 } }).empty());
    check("uint16_char_to_voidptr_is_null_not_decoded",
          static_cast<void*>(value_t{ std::uint16_t{ 0x0041 } }) == nullptr);
    check("uint16_char_to_unique_ptr_is_null",
          static_cast<std::unique_ptr<W>>(value_t{ std::uint16_t{ 0x0041 } }) == nullptr);
    // And as_string() on a char alternative is "" (only string / uint32 decode).
    check("uint16_char_as_string_is_empty",
          value_t{ std::uint16_t{ 0x0041 } }.as_string().empty());

    // -------------------------------------------------------------------------
    // Numeric -> void* : ONLY the uint32_t alternative decodes; every other
    // numeric alternative cannot static_cast to void*, so it falls through to
    // target_type{} == nullptr (pins flaw: a J/I/etc. return cast to void* is
    // silently null, not the bits).
    // -------------------------------------------------------------------------
    check("bool_to_voidptr_is_null",   static_cast<void*>(value_t{ true }) == nullptr);
    check("int8_to_voidptr_is_null",   static_cast<void*>(value_t{ std::int8_t{ 5 } }) == nullptr);
    check("int16_to_voidptr_is_null",  static_cast<void*>(value_t{ std::int16_t{ 5 } }) == nullptr);
    check("int32_to_voidptr_is_null",  static_cast<void*>(value_t{ std::int32_t{ 5 } }) == nullptr);
    check("int64_to_voidptr_is_null",  static_cast<void*>(value_t{ std::int64_t{ 5 } }) == nullptr);
    check("uint16_to_voidptr_is_null", static_cast<void*>(value_t{ std::uint16_t{ 5 } }) == nullptr);
    check("float_to_voidptr_is_null",  static_cast<void*>(value_t{ 5.0f }) == nullptr);
    check("double_to_voidptr_is_null", static_cast<void*>(value_t{ 5.0 }) == nullptr);

    // -------------------------------------------------------------------------
    // Numeric -> bool : the generic static_cast arm => result is (value != 0).
    // Covers every alternative (the existing tests only did int32 and bool).
    // The uint32_t (reference) alternative ALSO goes through the generic arm for
    // a bool target (bool is not void*/string/unique_ptr), so a non-null
    // compressed OOP reads as true — pins that "Object present?" is a non-zero
    // check on the COMPRESSED oop.
    // -------------------------------------------------------------------------
    check("int8_nonzero_to_bool_true",   static_cast<bool>(value_t{ std::int8_t{ -1 } }) == true);
    check("int8_zero_to_bool_false",     static_cast<bool>(value_t{ std::int8_t{ 0 } }) == false);
    check("int16_nonzero_to_bool_true",  static_cast<bool>(value_t{ std::int16_t{ 1 } }) == true);
    check("int16_zero_to_bool_false",    static_cast<bool>(value_t{ std::int16_t{ 0 } }) == false);
    check("int64_nonzero_to_bool_true",  static_cast<bool>(value_t{ std::int64_t{ 0x1'0000'0000LL } }) == true);
    check("int64_zero_to_bool_false",    static_cast<bool>(value_t{ std::int64_t{ 0 } }) == false);
    check("uint16_nonzero_to_bool_true", static_cast<bool>(value_t{ std::uint16_t{ 0x8000 } }) == true);
    check("uint16_zero_to_bool_false",   static_cast<bool>(value_t{ std::uint16_t{ 0 } }) == false);
    check("float_nonzero_to_bool_true",  static_cast<bool>(value_t{ 0.5f }) == true);
    check("float_zero_to_bool_false",    static_cast<bool>(value_t{ 0.0f }) == false);
    check("double_nonzero_to_bool_true", static_cast<bool>(value_t{ -0.5 }) == true);
    check("double_zero_to_bool_false",   static_cast<bool>(value_t{ 0.0 }) == false);
    check("uint32_nonzero_to_bool_true", static_cast<bool>(value_t{ std::uint32_t{ 1 } }) == true);
    check("uint32_zero_to_bool_false",   static_cast<bool>(value_t{ std::uint32_t{ 0 } }) == false);

    // -------------------------------------------------------------------------
    // bool alternative -> every arithmetic target: true == 1, false == 0.
    // -------------------------------------------------------------------------
    check("bool_true_to_int8_is_one",    static_cast<std::int8_t>(value_t{ true }) == std::int8_t{ 1 });
    check("bool_true_to_int16_is_one",   static_cast<std::int16_t>(value_t{ true }) == std::int16_t{ 1 });
    check("bool_true_to_int64_is_one",   static_cast<std::int64_t>(value_t{ true }) == 1LL);
    check("bool_true_to_uint16_is_one",  static_cast<std::uint16_t>(value_t{ true }) == std::uint16_t{ 1 });
    check("bool_true_to_uint32_is_one",  static_cast<std::uint32_t>(value_t{ true }) == 1u);
    check("bool_true_to_float_is_one",   static_cast<float>(value_t{ true }) == 1.0f);
    check("bool_true_to_double_is_one",  static_cast<double>(value_t{ true }) == 1.0);
    check("bool_false_to_int_is_zero",   static_cast<std::int32_t>(value_t{ false }) == 0);
    check("bool_false_to_double_is_zero",static_cast<double>(value_t{ false }) == 0.0);

    // -------------------------------------------------------------------------
    // int8_t alternative -> full arithmetic cast matrix (sign-extends because
    // the SOURCE is signed; pins flaw that signedness is load-bearing on the
    // exact alternative the producer chose).
    // -------------------------------------------------------------------------
    check("int8_neg_to_int16_sign_extends",
          static_cast<std::int16_t>(value_t{ std::int8_t{ -2 } }) == std::int16_t{ -2 });
    check("int8_neg_to_int64_sign_extends",
          static_cast<std::int64_t>(value_t{ std::int8_t{ -2 } }) == -2LL);
    check("int8_neg_to_uint16_wraps",
          static_cast<std::uint16_t>(value_t{ std::int8_t{ -1 } }) == std::uint16_t{ 0xFFFF });
    check("int8_neg_to_uint32_wraps",
          static_cast<std::uint32_t>(value_t{ std::int8_t{ -1 } }) == 0xFFFF'FFFFu);
    check("int8_to_float_exact",
          static_cast<float>(value_t{ std::int8_t{ -3 } }) == -3.0f);
    check("int8_to_double_exact",
          static_cast<double>(value_t{ std::int8_t{ -3 } }) == -3.0);

    // -------------------------------------------------------------------------
    // int16_t alternative -> matrix.
    // -------------------------------------------------------------------------
    check("int16_neg_to_int32_sign_extends",
          static_cast<std::int32_t>(value_t{ std::int16_t{ -300 } }) == -300);
    check("int16_neg_to_int64_sign_extends",
          static_cast<std::int64_t>(value_t{ std::int16_t{ -300 } }) == -300LL);
    check("int16_to_int8_truncates_low_byte",
          static_cast<std::int8_t>(value_t{ std::int16_t{ 0x1234 } }) == std::int8_t{ 0x34 });
    check("int16_to_float_exact",
          static_cast<float>(value_t{ std::int16_t{ -1234 } }) == -1234.0f);
    check("int16_to_double_exact",
          static_cast<double>(value_t{ std::int16_t{ 32767 } }) == 32767.0);

    // -------------------------------------------------------------------------
    // int32_t alternative -> matrix (narrowing + widening, both signs).
    // -------------------------------------------------------------------------
    check("int32_neg_to_int64_sign_extends",
          static_cast<std::int64_t>(value_t{ std::int32_t{ -123456 } }) == -123456LL);
    check("int32_to_int16_truncates",
          static_cast<std::int16_t>(value_t{ std::int32_t{ 0x0001'ABCD } }) == std::int16_t{ static_cast<std::int16_t>(0xABCD) });
    check("int32_to_uint32_reinterprets_negative",
          static_cast<std::uint32_t>(value_t{ std::int32_t{ -1 } }) == 0xFFFF'FFFFu);
    check("int32_to_float_loses_low_precision",
          static_cast<float>(value_t{ std::int32_t{ 16777217 } }) == 16777216.0f); // 2^24+1 not representable
    check("int32_to_double_exact_large",
          static_cast<double>(value_t{ std::int32_t{ 2147483647 } }) == 2147483647.0);

    // -------------------------------------------------------------------------
    // int64_t alternative -> matrix, including the documented int64 -> int32
    // truncation-to-zero when only the high half is set.
    // -------------------------------------------------------------------------
    check("int64_high_only_truncates_to_int32_zero",
          static_cast<std::int32_t>(value_t{ std::int64_t{ 0x1'0000'0000LL } }) == 0);
    check("int64_to_int16_keeps_low_16",
          static_cast<std::int16_t>(value_t{ std::int64_t{ 0x7777'8888'9999'1234LL } }) == std::int16_t{ 0x1234 });
    check("int64_to_uint32_keeps_low_32",
          static_cast<std::uint32_t>(value_t{ std::int64_t{ 0x1234'5678'9ABC'DEF0LL } }) == 0x9ABC'DEF0u);
    check("int64_neg_to_int32_sign_aware_low_bits",
          static_cast<std::int32_t>(value_t{ std::int64_t{ -1LL } }) == -1);
    check("int64_to_double_within_2pow53_exact",
          static_cast<double>(value_t{ std::int64_t{ 9007199254740992LL } }) == 9007199254740992.0); // 2^53

    // -------------------------------------------------------------------------
    // uint16_t (char) alternative -> matrix.  Zero-extends to wider integers
    // (unsigned source), and a 0xFFFF char reinterpreted as int16 is -1.
    // -------------------------------------------------------------------------
    check("uint16_max_to_int32_zero_extends",
          static_cast<std::int32_t>(value_t{ std::uint16_t{ 0xFFFF } }) == 65535);
    check("uint16_max_to_int64_zero_extends",
          static_cast<std::int64_t>(value_t{ std::uint16_t{ 0xFFFF } }) == 65535LL);
    check("uint16_max_to_int16_reinterprets_as_minus_one",
          static_cast<std::int16_t>(value_t{ std::uint16_t{ 0xFFFF } }) == std::int16_t{ -1 });
    check("uint16_to_int8_truncates_low_byte",
          static_cast<std::int8_t>(value_t{ std::uint16_t{ 0x12FF } }) == std::int8_t{ -1 }); // 0xFF -> -1
    check("uint16_to_uint32_zero_extends",
          static_cast<std::uint32_t>(value_t{ std::uint16_t{ 0xABCD } }) == 0xABCDu);
    check("uint16_to_float_exact",
          static_cast<float>(value_t{ std::uint16_t{ 0xFFFF } }) == 65535.0f);
    check("uint16_to_double_exact",
          static_cast<double>(value_t{ std::uint16_t{ 0xFFFF } }) == 65535.0);

    // -------------------------------------------------------------------------
    // float alternative -> matrix.  Truncation toward zero for integer targets;
    // exact widen to double.
    // -------------------------------------------------------------------------
    check("float_to_int32_truncates",
          static_cast<std::int32_t>(value_t{ 7.9f }) == 7);
    check("float_neg_to_int32_truncates_toward_zero",
          static_cast<std::int32_t>(value_t{ -7.9f }) == -7);
    check("float_to_int64_truncates",
          static_cast<std::int64_t>(value_t{ 100.6f }) == 100LL);
    check("float_to_int16_truncates",
          static_cast<std::int16_t>(value_t{ 200.9f }) == std::int16_t{ 200 });
    check("float_half_widens_to_double_exact",
          static_cast<double>(value_t{ 0.5f }) == 0.5);
    check("float_quarter_widens_to_double_exact",
          static_cast<double>(value_t{ 0.25f }) == 0.25);

    // -------------------------------------------------------------------------
    // double alternative -> matrix.  Truncation toward zero; narrowing to float
    // keeps an exactly-representable value.
    // -------------------------------------------------------------------------
    check("double_to_int32_truncates",
          static_cast<std::int32_t>(value_t{ 9.99 }) == 9);
    check("double_neg_to_int64_truncates_toward_zero",
          static_cast<std::int64_t>(value_t{ -9.99 }) == -9LL);
    check("double_to_int16_truncates",
          static_cast<std::int16_t>(value_t{ 1234.99 }) == std::int16_t{ 1234 });
    check("double_half_narrows_to_float_exact",
          static_cast<float>(value_t{ 0.5 }) == 0.5f);
    check("double_small_power_of_two_narrows_exact",
          static_cast<float>(value_t{ 1024.0 }) == 1024.0f);

    // -------------------------------------------------------------------------
    // Floating-point special values survive the float<->double static_cast
    // path.  We assert PROPERTIES (isnan / isinf / sign), never bit patterns or
    // formatted spellings, so MinGW (libstdc++) and MSVC (STL) agree.  inf and
    // nan are EXACT under float->double and double->float (no UB, unlike a
    // finite overflow), so these are well-defined on both compilers.
    // -------------------------------------------------------------------------
    {
        const double d_from_inf_f{ static_cast<double>(value_t{ std::numeric_limits<float>::infinity() }) };
        check("float_pos_inf_widens_to_double_inf",
              std::isinf(d_from_inf_f) && d_from_inf_f > 0.0);
        const double d_from_neg_inf_f{ static_cast<double>(value_t{ -std::numeric_limits<float>::infinity() }) };
        check("float_neg_inf_widens_to_double_neg_inf",
              std::isinf(d_from_neg_inf_f) && d_from_neg_inf_f < 0.0);
        const double d_from_nan_f{ static_cast<double>(value_t{ std::numeric_limits<float>::quiet_NaN() }) };
        check("float_nan_widens_to_double_nan", std::isnan(d_from_nan_f));

        const float f_from_inf_d{ static_cast<float>(value_t{ std::numeric_limits<double>::infinity() }) };
        check("double_pos_inf_narrows_to_float_inf",
              std::isinf(f_from_inf_d) && f_from_inf_d > 0.0f);
        const float f_from_nan_d{ static_cast<float>(value_t{ std::numeric_limits<double>::quiet_NaN() }) };
        check("double_nan_narrows_to_float_nan", std::isnan(f_from_nan_d));
    }
    // FLT_MAX survives float->float and float->double exactly (in-range, no UB).
    check("float_max_round_trips",
          static_cast<float>(value_t{ std::numeric_limits<float>::max() })
              == std::numeric_limits<float>::max());
    check("float_max_widens_to_double_exact",
          static_cast<double>(value_t{ std::numeric_limits<float>::max() })
              == static_cast<double>(std::numeric_limits<float>::max()));
    // Smallest positive normal float round-trips (no flush-to-zero in the cast).
    check("float_min_normal_round_trips",
          static_cast<float>(value_t{ std::numeric_limits<float>::min() })
              == std::numeric_limits<float>::min());
    // DBL_MAX round-trips through the double->double identity cast.
    check("double_max_round_trips",
          static_cast<double>(value_t{ std::numeric_limits<double>::max() })
              == std::numeric_limits<double>::max());

    // -------------------------------------------------------------------------
    // Per-alternative round-trip to SELF at boundary values that the existing
    // suite did not pin (extends the int8/int16/uint16 self round-trips).
    // -------------------------------------------------------------------------
    check("int8_zero_self_round_trip",
          static_cast<std::int8_t>(value_t{ std::int8_t{ 0 } }) == std::int8_t{ 0 });
    check("int16_max_self_round_trip",
          static_cast<std::int16_t>(value_t{ std::int16_t{ 32767 } }) == std::int16_t{ 32767 });
    check("int32_minus_one_self_round_trip",
          static_cast<std::int32_t>(value_t{ std::int32_t{ -1 } }) == -1);
    check("int64_minus_one_self_round_trip",
          static_cast<std::int64_t>(value_t{ std::int64_t{ -1LL } }) == -1LL);
    check("uint16_mid_self_round_trip",
          static_cast<std::uint16_t>(value_t{ std::uint16_t{ 0x8000 } }) == std::uint16_t{ 0x8000 });
    check("uint32_zero_self_round_trip",
          static_cast<std::uint32_t>(value_t{ std::uint32_t{ 0 } }) == 0u);
    check("uint32_max_self_round_trip",
          static_cast<std::uint32_t>(value_t{ std::uint32_t{ 0xFFFF'FFFF } }) == 0xFFFF'FFFFu);

    // -------------------------------------------------------------------------
    // uint32_t (reference) alternative -> void* across a fuller value set, all
    // matching decode_oop_pointer EXACTLY (which is the no-truncation contract).
    // With no VMStructs every one of these is nullptr, but we compare against
    // the helper rather than hardcoding nullptr so the assertion still holds if
    // a future build links a JVM into the test harness.
    // -------------------------------------------------------------------------
    check("uint32_two_to_voidptr_matches_decode",
          static_cast<void*>(value_t{ std::uint32_t{ 2 } })
              == vmhook::hotspot::decode_oop_pointer(2u));
    check("uint32_sentinel_cafebabe_to_voidptr_matches_decode",
          static_cast<void*>(value_t{ std::uint32_t{ 0xCAFEBABE } })
              == vmhook::hotspot::decode_oop_pointer(0xCAFEBABEu));
    check("uint32_aligned_to_voidptr_matches_decode",
          static_cast<void*>(value_t{ std::uint32_t{ 0x0010'0000 } })
              == vmhook::hotspot::decode_oop_pointer(0x0010'0000u));
    // uint32_t -> std::string also routes through decode + read_java_string and
    // yields "" with no JVM (this is the operator's std::string arm, distinct
    // from as_string()).  The natural static_cast spelling is unambiguous now
    // (BUG 1 fixed), so we use it directly.
    check("uint32_to_string_via_cast_empty_no_jvm",
          static_cast<std::string>(value_t{ std::uint32_t{ 12345 } }).empty());
    check("uint32_zero_to_string_via_cast_empty",
          static_cast<std::string>(value_t{ std::uint32_t{ 0 } }).empty());

    // -------------------------------------------------------------------------
    // A default-constructed value_t holds monostate and converts determinist(-
    // ally) to the zero/false/null/"" of every target — the documented
    // "no value" contract.
    // -------------------------------------------------------------------------
    check("default_value_t_is_void",        value_t{}.is_void() == true);
    check("default_value_t_to_int_zero",     static_cast<std::int32_t>(value_t{}) == 0);
    check("default_value_t_to_bool_false",   static_cast<bool>(value_t{}) == false);
    check("default_value_t_to_double_zero",  static_cast<double>(value_t{}) == 0.0);
    check("default_value_t_to_voidptr_null", static_cast<void*>(value_t{}) == nullptr);
    check("default_value_t_as_string_empty", value_t{}.as_string().empty());
    check("default_value_t_to_unique_ptr_null",
          static_cast<std::unique_ptr<W>>(value_t{}) == nullptr);
    check("default_value_t_to_string_empty",
          static_cast<std::string>(value_t{}).empty());

    // -------------------------------------------------------------------------
    // Compile-time conversion-operator surface: pin the remaining targets the
    // operator must satisfy (char, unsigned variants, void*, unique_ptr).  A
    // future reshuffle of the variant or the operator arms is caught at build.
    // -------------------------------------------------------------------------
    static_assert(std::is_convertible_v<value_t, char>,            "value_t -> char");
    static_assert(std::is_convertible_v<value_t, unsigned char>,   "value_t -> unsigned char");
    static_assert(std::is_convertible_v<value_t, short>,           "value_t -> short");
    static_assert(std::is_convertible_v<value_t, unsigned short>,  "value_t -> unsigned short");
    static_assert(std::is_convertible_v<value_t, long long>,       "value_t -> long long");
    static_assert(std::is_convertible_v<value_t, std::uint64_t>,   "value_t -> uint64");
    static_assert(std::is_nothrow_move_constructible_v<value_t>,   "value_t move ctor noexcept");
    // as_string / is_void / is_string are noexcept (the call()/JVM seam relies
    // on it — a throwing introspector would std::terminate the host JVM).
    static_assert(noexcept(std::declval<const value_t&>().as_string()), "as_string noexcept");
    static_assert(noexcept(std::declval<const value_t&>().is_void()),   "is_void noexcept");
    static_assert(noexcept(std::declval<const value_t&>().is_string()), "is_string noexcept");
    check("value_t_extended_compile_time_surface_present", true);

    // -------------------------------------------------------------------------
    // method_proxy::get_compressed_oop(): null object -> 0 (no JVM, no crash).
    // With a real stack uint32_t fed as the "object" pointer it reads the first
    // 4 bytes via memcpy (no decode, no JVM) — proving the raw 4-byte read.
    // -------------------------------------------------------------------------
    {
        vmhook::method_proxy null_obj{ nullptr, nullptr, std::string{ "()I" } };
        check("get_compressed_oop_null_object_is_zero", null_obj.get_compressed_oop() == 0u);
    }
    {
        // A stack value standing in for an object header's first 4 bytes.
        std::uint32_t fake_header{ 0x1234'ABCDu };
        vmhook::method_proxy proxy{ &fake_header, nullptr, std::string{ "()Ljava/lang/Object;" } };
        check("get_compressed_oop_reads_first_four_bytes",
              proxy.get_compressed_oop() == 0x1234'ABCDu);
    }
    {
        // Zero-valued header reads back as 0 even with a non-null object.
        std::uint32_t zero_header{ 0u };
        vmhook::method_proxy proxy{ &zero_header, nullptr, std::string{ "()Ljava/lang/Object;" } };
        check("get_compressed_oop_zero_header_is_zero", proxy.get_compressed_oop() == 0u);
    }

    // -------------------------------------------------------------------------
    // method_proxy::is_static(): with a null Method* the live-flags read can't
    // happen, so it returns false REGARDLESS of whether the object pointer is
    // null (static) or non-null (instance).  Pins the documented fallback.
    // -------------------------------------------------------------------------
    {
        std::uint32_t dummy{ 0 };
        vmhook::method_proxy with_obj{ &dummy, nullptr, std::string{ "()V" } };
        check("is_static_false_when_object_non_null_and_method_null",
              with_obj.is_static() == false);
        // raw_method stays null; name stays empty; signature still round-trips.
        check("with_obj_raw_method_null", with_obj.raw_method() == nullptr);
        check("with_obj_name_empty",      with_obj.name().empty());
        check("with_obj_signature_round_trips",
              with_obj.signature() == std::string_view{ "()V" });
    }

    // -------------------------------------------------------------------------
    // method_proxy::is_reference(): a few more descriptor shapes the suite
    // hadn't covered (char/byte/short/float returns -> primitives -> false;
    // nested generic-erased object signature -> true).
    // -------------------------------------------------------------------------
    {
        vmhook::method_proxy p{ nullptr, nullptr, std::string{ "()C" } };
        check("is_reference_false_for_char_return", p.is_reference() == false);
    }
    {
        vmhook::method_proxy p{ nullptr, nullptr, std::string{ "()B" } };
        check("is_reference_false_for_byte_return", p.is_reference() == false);
    }
    {
        vmhook::method_proxy p{ nullptr, nullptr, std::string{ "()S" } };
        check("is_reference_false_for_short_return", p.is_reference() == false);
    }
    {
        vmhook::method_proxy p{ nullptr, nullptr, std::string{ "()F" } };
        check("is_reference_false_for_float_return", p.is_reference() == false);
    }
    {
        // 3D array of objects -> first char after ')' is '[' -> true.
        vmhook::method_proxy p{ nullptr, nullptr, std::string{ "()[[[Ljava/lang/String;" } };
        check("is_reference_true_for_3d_object_array_return", p.is_reference() == true);
    }
    {
        // Object array of a primitive inner type still starts with '[' -> true.
        vmhook::method_proxy p{ nullptr, nullptr, std::string{ "(JD)[Z" } };
        check("is_reference_true_for_boolean_array_return", p.is_reference() == true);
    }

    // =========================================================================
    // EXPANSION 2 (no-JVM, platform-invariant): the value-semantics surface of
    // value_t itself — the ACTIVE-ALTERNATIVE tag for every slot, copy/move
    // construction, copy/move assignment, and reassignment BETWEEN alternatives.
    // The earlier blocks only ever assert the *converted-out* value of a freshly
    // brace-initialised temporary; none of them pin which variant slot is live,
    // nor that value_t survives being copied / moved / reassigned (the holder is
    // handed around by call()/call_jni() return, stored, and re-bound by callers).
    // value_t is an aggregate over a single std::variant, so it inherits the
    // variant's regular-type semantics; these checks lock that contract in.
    // =========================================================================

    // -------------------------------------------------------------------------
    // Active-alternative tag: data.index() must equal the documented slot for
    // EACH of the eleven alternatives (vmhook.hpp:13401-13413).  Order:
    //   0 monostate, 1 bool, 2 int8, 3 int16, 4 int32, 5 int64, 6 float,
    //   7 double, 8 uint16, 9 uint32 (compressed OOP), 10 std::string.
    // A future reshuffle of the variant flips these and is caught here AND by
    // the conversion-behaviour checks above (which depend on the slot order).
    // -------------------------------------------------------------------------
    check("index_monostate_is_0", value_t{ std::monostate{} }.data.index() == 0u);
    check("index_bool_is_1",      value_t{ true }.data.index() == 1u);
    check("index_int8_is_2",      value_t{ std::int8_t{ 1 } }.data.index() == 2u);
    check("index_int16_is_3",     value_t{ std::int16_t{ 1 } }.data.index() == 3u);
    check("index_int32_is_4",     value_t{ std::int32_t{ 1 } }.data.index() == 4u);
    check("index_int64_is_5",     value_t{ std::int64_t{ 1 } }.data.index() == 5u);
    check("index_float_is_6",     value_t{ 1.0f }.data.index() == 6u);
    check("index_double_is_7",    value_t{ 1.0 }.data.index() == 7u);
    check("index_uint16_is_8",    value_t{ std::uint16_t{ 1 } }.data.index() == 8u);
    check("index_uint32_is_9",    value_t{ std::uint32_t{ 1 } }.data.index() == 9u);
    check("index_string_is_10",   value_t{ std::string{ "x" } }.data.index() == 10u);
    // The default-constructed holder is the monostate (void/failure) slot.
    check("index_default_is_monostate_0", value_t{}.data.index() == 0u);
    // holds_alternative agrees with the index for a representative non-trivial
    // alternative pair (the introspection helpers are thin wrappers over these).
    check("holds_string_alternative_for_string",
          std::holds_alternative<std::string>(value_t{ std::string{ "h" } }.data));
    check("holds_uint32_alternative_for_uint32",
          std::holds_alternative<std::uint32_t>(value_t{ std::uint32_t{ 7 } }.data));

    // -------------------------------------------------------------------------
    // Copy construction: a copied value_t keeps the SAME active alternative and
    // converts out to the SAME value as the original, and the original is left
    // intact (copy, not move).  The std::string alternative (the only heap-
    // owning payload) is the interesting one — the copy must own an independent
    // buffer that still compares equal.
    // -------------------------------------------------------------------------
    {
        const value_t original{ std::int32_t{ -424242 } };
        const value_t copy{ original };                       // copy-construct
        check("copy_ctor_preserves_index",
              copy.data.index() == original.data.index());
        check("copy_ctor_preserves_value",
              static_cast<std::int32_t>(copy) == -424242
                  && static_cast<std::int32_t>(original) == -424242);
    }
    {
        const value_t original{ std::string{ "payload-string" } };
        const value_t copy{ original };
        check("copy_ctor_string_is_independent_equal",
              copy.as_string() == "payload-string"
                  && original.as_string() == "payload-string");
        check("copy_ctor_string_keeps_string_slot",
              copy.is_string() && original.is_string());
    }
    {
        // Copying the monostate / uint32 / double alternatives preserves slot.
        const value_t mono{ std::monostate{} };
        const value_t mono_copy{ mono };
        check("copy_ctor_monostate_stays_void",
              mono_copy.is_void() && mono.is_void());
        const value_t ref{ std::uint32_t{ 0xCAFEBABE } };
        const value_t ref_copy{ ref };
        check("copy_ctor_uint32_round_trips",
              static_cast<std::uint32_t>(ref_copy) == 0xCAFEBABEu);
        const value_t dbl{ -2.5 };
        const value_t dbl_copy{ dbl };
        check("copy_ctor_double_round_trips",
              static_cast<double>(dbl_copy) == -2.5);
    }

    // -------------------------------------------------------------------------
    // Move construction: the moved-INTO value_t must carry the original value
    // and active alternative.  Per [variant.assign]/[string], the moved-FROM
    // object is left in a valid-but-UNSPECIFIED state, so we assert ONLY the
    // destination — never the source's content — to stay deterministic across
    // libstdc++ (MinGW) and the MSVC STL.
    // -------------------------------------------------------------------------
    {
        value_t source{ std::string{ "moved-out-string" } };
        const value_t moved{ std::move(source) };             // move-construct
        check("move_ctor_string_destination_has_value",
              moved.as_string() == "moved-out-string");
        check("move_ctor_string_destination_is_string_slot", moved.is_string());
    }
    {
        value_t source{ std::int64_t{ 0x7FFF'FFFF'0000'0001LL } };
        const value_t moved{ std::move(source) };
        check("move_ctor_int64_destination_value",
              static_cast<std::int64_t>(moved) == 0x7FFF'FFFF'0000'0001LL);
        check("move_ctor_int64_destination_slot", moved.data.index() == 5u);
    }

    // -------------------------------------------------------------------------
    // Copy assignment: assigning over a live value_t replaces BOTH the value and
    // the active alternative — including switching from one alternative to a
    // DIFFERENT one (string <- int, int <- string), the realistic case where a
    // stored result holder is overwritten by the next call()'s return.
    // -------------------------------------------------------------------------
    {
        value_t target{ std::int32_t{ 7 } };
        const value_t src{ std::string{ "assigned" } };
        target = src;                                         // copy-assign, slot changes
        check("copy_assign_switches_int_to_string_slot", target.is_string());
        check("copy_assign_switches_int_to_string_value",
              target.as_string() == "assigned");
        check("copy_assign_leaves_source_intact", src.as_string() == "assigned");
    }
    {
        value_t target{ std::string{ "old" } };
        const value_t src{ std::int64_t{ 9999999999LL } };
        target = src;                                        // string slot -> int64 slot
        check("copy_assign_switches_string_to_int64_slot",
              target.data.index() == 5u && !target.is_string());
        check("copy_assign_switches_string_to_int64_value",
              static_cast<std::int64_t>(target) == 9999999999LL);
    }
    {
        // Self-assignment is a no-op (must not corrupt the held value).
        value_t target{ std::uint32_t{ 0x1234'5678 } };
        const value_t& alias{ target };
        target = alias;                                      // self copy-assign
        check("self_copy_assign_is_noop",
              static_cast<std::uint32_t>(target) == 0x1234'5678u
                  && target.data.index() == 9u);
    }

    // -------------------------------------------------------------------------
    // Move assignment: same slot-switching contract; assert only the destination.
    // -------------------------------------------------------------------------
    {
        value_t target{ std::monostate{} };
        value_t src{ std::string{ "move-assigned" } };
        target = std::move(src);                             // monostate -> string
        check("move_assign_monostate_to_string_slot", target.is_string());
        check("move_assign_monostate_to_string_value",
              target.as_string() == "move-assigned");
    }
    {
        value_t target{ std::string{ "discarded" } };
        value_t src{ 3.5 };
        target = std::move(src);                             // string -> double
        check("move_assign_string_to_double_slot", target.data.index() == 7u);
        check("move_assign_string_to_double_value",
              static_cast<double>(target) == 3.5);
    }

    // -------------------------------------------------------------------------
    // Reassignment of the underlying variant directly (data = ... / emplace):
    // value_t is an aggregate with a public `data` member, so callers / the
    // library can re-point the active alternative in place.  Each reassignment
    // must flip the introspection helpers and the converted-out value together.
    // -------------------------------------------------------------------------
    {
        value_t v{ std::monostate{} };
        check("reassign_starts_void", v.is_void());
        v.data = std::int32_t{ 55 };                         // monostate -> int32
        check("reassign_to_int32_not_void",
              !v.is_void() && v.data.index() == 4u);
        check("reassign_to_int32_value", static_cast<std::int32_t>(v) == 55);
        v.data = std::string{ "now-a-string" };              // int32 -> string
        check("reassign_to_string_is_string",
              v.is_string() && v.as_string() == "now-a-string");
        v.data = std::uint32_t{ 0xABCD'0123 };               // string -> uint32
        check("reassign_to_uint32_slot",
              v.data.index() == 9u && !v.is_string() && !v.is_void());
        check("reassign_to_uint32_value",
              static_cast<std::uint32_t>(v) == 0xABCD'0123u);
        v.data.emplace<std::monostate>();                    // back to void via emplace
        check("reassign_emplace_back_to_void", v.is_void());
    }

    // -------------------------------------------------------------------------
    // std::swap on two value_t exchanges their active alternatives and values
    // (variant is Swappable; pins that value_t inherits it).  Assert both sides.
    // -------------------------------------------------------------------------
    {
        value_t a{ std::int32_t{ 111 } };
        value_t b{ std::string{ "bee" } };
        std::swap(a, b);
        check("swap_moves_string_into_a", a.is_string() && a.as_string() == "bee");
        check("swap_moves_int_into_b",
              b.data.index() == 4u && static_cast<std::int32_t>(b) == 111);
    }

    // -------------------------------------------------------------------------
    // The conversion operator's std::string GENERIC-ELSE arm (vmhook.hpp:
    // 13491-13494): a NON-string, NON-uint32 stored alternative cast straight to
    // std::string yields "" (target_type{}).  The as_string() block above pins
    // the same alternatives through as_string(); here we pin the OPERATOR path
    // (static_cast<std::string>) for the numeric alternatives the operator-arm
    // coverage had not yet exercised (only monostate/string/uint16/uint32 were).
    // BUG 1 (FIXED): these casts are unambiguous now the operator is constrained.
    // -------------------------------------------------------------------------
    check("static_cast_string_from_bool_empty",
          static_cast<std::string>(value_t{ true }).empty());
    check("static_cast_string_from_int8_empty",
          static_cast<std::string>(value_t{ std::int8_t{ -1 } }).empty());
    check("static_cast_string_from_int16_empty",
          static_cast<std::string>(value_t{ std::int16_t{ 1234 } }).empty());
    check("static_cast_string_from_int32_empty",
          static_cast<std::string>(value_t{ std::int32_t{ 1234 } }).empty());
    check("static_cast_string_from_int64_empty",
          static_cast<std::string>(value_t{ std::int64_t{ 1234 } }).empty());
    check("static_cast_string_from_float_empty",
          static_cast<std::string>(value_t{ 1.5f }).empty());
    check("static_cast_string_from_double_empty",
          static_cast<std::string>(value_t{ 2.5 }).empty());
    // And the operator path agrees with as_string() on these numeric alternatives
    // (both produce "").
    check("operator_string_agrees_with_as_string_on_int32",
          static_cast<std::string>(value_t{ std::int32_t{ 9 } })
              == value_t{ std::int32_t{ 9 } }.as_string());

    // -------------------------------------------------------------------------
    // Signed-zero / representative boundaries the earlier blocks skipped.
    //   * -0.0 survives the double->double identity cast WITH its sign bit (the
    //     value compares == +0.0, so we assert std::signbit, not ==).
    //   * -0.0f likewise through float->float, and float -0.0 widens to a
    //     negative-zero double.
    //   * +0.0 has a clear (unset) sign bit for contrast.
    // -------------------------------------------------------------------------
    check("double_negative_zero_preserves_sign_bit",
          std::signbit(static_cast<double>(value_t{ -0.0 })));
    check("double_positive_zero_has_no_sign_bit",
          !std::signbit(static_cast<double>(value_t{ 0.0 })));
    check("float_negative_zero_preserves_sign_bit",
          std::signbit(static_cast<float>(value_t{ -0.0f })));
    check("float_negative_zero_widens_to_negative_zero_double",
          std::signbit(static_cast<double>(value_t{ -0.0f })));
    // -0.0 still reads as boolean false (negative zero == zero).
    check("double_negative_zero_to_bool_false",
          static_cast<bool>(value_t{ -0.0 }) == false);

    // -------------------------------------------------------------------------
    // A couple more per-alternative self-round-trips at the zero / representative
    // values not yet pinned, closing the SELF cast matrix.
    // -------------------------------------------------------------------------
    check("int32_zero_self_round_trip",
          static_cast<std::int32_t>(value_t{ std::int32_t{ 0 } }) == 0);
    check("int64_zero_self_round_trip",
          static_cast<std::int64_t>(value_t{ std::int64_t{ 0 } }) == 0LL);
    check("bool_false_self_round_trip",
          static_cast<bool>(value_t{ false }) == false);
    check("uint16_zero_self_round_trip_2",
          static_cast<std::uint16_t>(value_t{ std::uint16_t{ 0 } }) == 0u);
    check("double_zero_self_round_trip",
          static_cast<double>(value_t{ 0.0 }) == 0.0);
    check("float_zero_self_round_trip_2",
          static_cast<float>(value_t{ 0.0f }) == 0.0f);

    // -------------------------------------------------------------------------
    // Compile-time: value_t is a regular-ish value type (the holder contract the
    // copy/move/swap checks above exercise at runtime).  Move ops are noexcept
    // already pinned (line ~906); add copy-constructible / copy-assignable /
    // move-assignable / swappable so a future change to the variant payload set
    // that breaks regularity is caught at build time.
    // -------------------------------------------------------------------------
    static_assert(std::is_copy_constructible_v<value_t>,    "value_t copy-constructible");
    static_assert(std::is_copy_assignable_v<value_t>,       "value_t copy-assignable");
    static_assert(std::is_move_assignable_v<value_t>,       "value_t move-assignable");
    static_assert(std::is_swappable_v<value_t>,             "value_t swappable");
    static_assert(std::is_default_constructible_v<value_t>, "value_t default-constructible");
    check("value_t_value_semantics_compile_time_present", true);

    // =========================================================================
    // EXPANSION 3 (no-JVM, platform-invariant): BIT-EXACT float<->double through
    // the conversion operator, and ALTERNATING-BIT integer patterns across the
    // full cast matrix.  The earlier float blocks assert FP *properties*
    // (isnan / isinf / signbit) and value `==`; this block additionally pins the
    // raw OBJECT REPRESENTATION (every bit) for the conversions IEEE-754
    // guarantees are exact, so a regression that perturbed the value would flip a
    // bit even where `==` (which collapses -0.0 == +0.0 and rejects NaN) would
    // not notice.  Only EXACT conversions are bit-checked: float->double is exact
    // for every finite/zero/inf source, and double->float is exact for any value
    // that is itself representable as a float — both with NO implementation
    // latitude, so MinGW and MSVC must agree to the bit.  (A finite double that
    // overflows float is UB and is NOT exercised here; the inf/nan widen/narrow
    // properties are already covered above.)
    // =========================================================================

    // -------------------------------------------------------------------------
    // float -> double widening is EXACT for every float (the value set spans the
    // representable extremes).  We assert the result's bits equal the bits of a
    // direct static_cast<double>(src) — the operator must not perturb a single
    // bit.  These also implicitly pin that the operator routes float through the
    // generic static_cast arm (no rounding, no double-rounding).
    // -------------------------------------------------------------------------
    {
        const float srcs[]{
            0.0f, -0.0f,
            1.0f, -1.0f,
            0.5f, -0.25f,
            std::numeric_limits<float>::min(),               // smallest normal
            -std::numeric_limits<float>::min(),
            std::numeric_limits<float>::max(),               // largest finite
            -std::numeric_limits<float>::max(),
            std::numeric_limits<float>::denorm_min(),         // smallest subnormal
            -std::numeric_limits<float>::denorm_min(),
            std::numeric_limits<float>::lowest(),
            std::numeric_limits<float>::infinity(),
            -std::numeric_limits<float>::infinity(),
            16777216.0f,                                      // 2^24
            1.0f / 3.0f,                                      // a non-terminating binary fraction
        };
        bool all_exact{ true };
        for (const float s : srcs)
        {
            const double got{ static_cast<double>(value_t{ s }) };
            const double want{ static_cast<double>(s) };
            if (!bits_equal_d(got, want)) { all_exact = false; }
        }
        check("float_to_double_is_bit_exact_for_all_finite_and_inf_and_subnormal", all_exact);
    }
    // The float subnormal widens to a double whose VALUE equals the same magnitude
    // (subnormal-float -> normal-double, exact) and is NOT flushed to zero.
    check("float_subnormal_widens_nonzero_exact",
          static_cast<double>(value_t{ std::numeric_limits<float>::denorm_min() })
              == static_cast<double>(std::numeric_limits<float>::denorm_min())
          && static_cast<double>(value_t{ std::numeric_limits<float>::denorm_min() }) != 0.0);
    // Smallest float subnormal round-trips float->float (identity cast) bit-exact.
    check("float_subnormal_self_round_trip_bit_exact",
          bits_equal_f(static_cast<float>(value_t{ std::numeric_limits<float>::denorm_min() }),
                       std::numeric_limits<float>::denorm_min()));
    check("float_neg_subnormal_self_round_trip_bit_exact",
          bits_equal_f(static_cast<float>(value_t{ -std::numeric_limits<float>::denorm_min() }),
                       -std::numeric_limits<float>::denorm_min()));

    // -------------------------------------------------------------------------
    // double -> float narrowing is EXACT for any value that is itself a float
    // (the cast round-trips the float magnitude through a double and back).  We
    // build each double from a known float so the narrowing cannot round, and
    // assert the recovered float's bits equal the original float's bits.
    // -------------------------------------------------------------------------
    {
        const float floats[]{
            0.0f, -0.0f, 1.0f, -1.0f, 0.5f, -0.25f, 1024.0f,
            std::numeric_limits<float>::min(),
            std::numeric_limits<float>::max(),
            -std::numeric_limits<float>::max(),
            std::numeric_limits<float>::denorm_min(),         // subnormal survives the round trip
            std::numeric_limits<float>::infinity(),
            -std::numeric_limits<float>::infinity(),
        };
        bool all_exact{ true };
        for (const float f : floats)
        {
            const double widened{ static_cast<double>(f) };   // exact (float->double)
            const float got{ static_cast<float>(value_t{ widened }) };
            if (!bits_equal_f(got, f)) { all_exact = false; }
        }
        check("double_to_float_is_bit_exact_for_float_representable_values", all_exact);
    }
    // A double subnormal-of-float (FLT_MIN/2 is exactly representable as a float
    // subnormal) narrows to the matching float subnormal, NOT to zero.
    {
        const double half_min{ static_cast<double>(std::numeric_limits<float>::min()) / 2.0 };
        const float narrowed{ static_cast<float>(value_t{ half_min }) };
        check("double_to_float_subnormal_not_flushed_to_zero",
              narrowed != 0.0f
                  && bits_equal_f(narrowed, static_cast<float>(half_min)));
    }

    // -------------------------------------------------------------------------
    // NaN survives the float<->double conversion as a NaN (payload/bits are NOT
    // portable, so we assert ONLY std::isnan — never the bits — and that a
    // float NaN widened to double then narrowed back to float is still NaN).
    // -------------------------------------------------------------------------
    {
        const double d_nan{ static_cast<double>(value_t{ std::numeric_limits<float>::quiet_NaN() }) };
        const float back{ static_cast<float>(value_t{ d_nan }) };
        check("float_nan_widen_then_narrow_is_still_nan",
              std::isnan(d_nan) && std::isnan(back));
        // signaling-NaN source: still observably a NaN after widening (no claim
        // about signaling-ness, which the cast may quiet — only NaN-ness).
        const double d_snan{ static_cast<double>(value_t{ std::numeric_limits<float>::signaling_NaN() }) };
        check("float_signaling_nan_widens_to_some_nan", std::isnan(d_snan));
    }

    // -------------------------------------------------------------------------
    // float -> float / double -> double IDENTITY cast is bit-exact for the
    // extremes (sign-bit and subnormal preservation, already value-checked above,
    // now pinned to the bit so a future operator change cannot silently canon-
    // icalise -0.0 to +0.0 or flush a subnormal).
    // -------------------------------------------------------------------------
    check("float_negative_zero_identity_bit_exact",
          bits_equal_f(static_cast<float>(value_t{ -0.0f }), -0.0f));
    check("float_positive_zero_identity_bit_exact",
          bits_equal_f(static_cast<float>(value_t{ 0.0f }), 0.0f));
    check("double_negative_zero_identity_bit_exact",
          bits_equal_d(static_cast<double>(value_t{ -0.0 }), -0.0));
    check("float_max_identity_bit_exact",
          bits_equal_f(static_cast<float>(value_t{ std::numeric_limits<float>::max() }),
                       std::numeric_limits<float>::max()));
    check("double_max_identity_bit_exact",
          bits_equal_d(static_cast<double>(value_t{ std::numeric_limits<double>::max() }),
                       std::numeric_limits<double>::max()));
    check("double_denorm_min_identity_bit_exact",
          bits_equal_d(static_cast<double>(value_t{ std::numeric_limits<double>::denorm_min() }),
                       std::numeric_limits<double>::denorm_min()));
    // -0.0f widened to double is bit-identical to a directly-cast -0.0 double
    // (the sign bit must survive the WIDENING, not just the identity cast).
    check("float_negative_zero_widens_to_double_negative_zero_bit_exact",
          bits_equal_d(static_cast<double>(value_t{ -0.0f }), static_cast<double>(-0.0f)));

    // -------------------------------------------------------------------------
    // ALTERNATING-BIT integer patterns (0x55.. / 0xAA.. / 0xA5.. and the all-
    // ones / single-walking-bit edges) through every integer alternative's cast
    // matrix.  These catch a byte-swap / sign / mask regression that uniform
    // small values (0, 1, -1) would slip past: 0x55 and 0xAA differ in every
    // adjacent bit and in sign, so a truncation that kept the wrong half, or a
    // sign/zero-extension flip, changes the result.  Every expected value is the
    // deterministic two's-complement static_cast result, identical on all
    // platforms (two's complement is mandated since C++20).
    // -------------------------------------------------------------------------
    // int32 alternating-bit source -> narrow / widen / reinterpret.
    check("int32_0x55555555_to_uint32_reinterprets",
          static_cast<std::uint32_t>(value_t{ std::int32_t{ 0x5555'5555 } }) == 0x5555'5555u);
    check("int32_0xAAAAAAAA_to_uint32_reinterprets",
          static_cast<std::uint32_t>(value_t{ static_cast<std::int32_t>(0xAAAA'AAAAu) }) == 0xAAAA'AAAAu);
    check("int32_0x55555555_to_int16_keeps_low_half",
          static_cast<std::int16_t>(value_t{ std::int32_t{ 0x5555'5555 } }) == std::int16_t{ 0x5555 });
    check("int32_0xAAAAAAAA_to_uint16_keeps_low_half",
          static_cast<std::uint16_t>(value_t{ static_cast<std::int32_t>(0xAAAA'AAAAu) }) == std::uint16_t{ 0xAAAA });
    check("int32_0x55555555_to_int8_keeps_low_byte",
          static_cast<std::int8_t>(value_t{ std::int32_t{ 0x5555'5555 } }) == std::int8_t{ 0x55 });
    check("int32_0xAAAAAAAA_to_int8_low_byte_is_negative",
          static_cast<std::int8_t>(value_t{ static_cast<std::int32_t>(0xAAAA'AAAAu) }) == static_cast<std::int8_t>(0xAA));
    check("int32_0x55555555_widens_to_int64_no_high_bits",
          static_cast<std::int64_t>(value_t{ std::int32_t{ 0x5555'5555 } }) == 0x0000'0000'5555'5555LL);
    check("int32_0xAAAAAAAA_widens_to_int64_sign_extends",
          static_cast<std::int64_t>(value_t{ static_cast<std::int32_t>(0xAAAA'AAAAu) })
              == static_cast<std::int64_t>(0xFFFF'FFFF'AAAA'AAAAull));

    // int64 alternating-bit source -> truncate / reinterpret.
    check("int64_0x5555..._to_uint32_keeps_low_32",
          static_cast<std::uint32_t>(value_t{ std::int64_t{ 0x5555'5555'5555'5555LL } }) == 0x5555'5555u);
    check("int64_0xAAAA..._to_uint32_keeps_low_32",
          static_cast<std::uint32_t>(value_t{ static_cast<std::int64_t>(0xAAAA'AAAA'AAAA'AAAAull) }) == 0xAAAA'AAAAu);
    check("int64_0xA5A5..._to_int16_keeps_low_16",
          static_cast<std::int16_t>(value_t{ static_cast<std::int64_t>(0xA5A5'A5A5'A5A5'A5A5ull) }) == static_cast<std::int16_t>(0xA5A5));
    check("int64_0x5A5A..._to_int8_keeps_low_byte",
          static_cast<std::int8_t>(value_t{ static_cast<std::int64_t>(0x5A5A'5A5A'5A5A'5A5Aull) }) == std::int8_t{ 0x5A });
    check("int64_0x5555..._to_int32_keeps_low_32_positive",
          static_cast<std::int32_t>(value_t{ std::int64_t{ 0x1234'5678'5555'5555LL } }) == 0x5555'5555);
    check("int64_0xAAAA..._low_32_to_int32_is_negative",
          static_cast<std::int32_t>(value_t{ static_cast<std::int64_t>(0x0000'0000'AAAA'AAAAull) })
              == static_cast<std::int32_t>(0xAAAA'AAAAu));

    // int16 / int8 alternating-bit source -> widen with sign, narrow.
    check("int16_0x5555_to_int32_zero_high",
          static_cast<std::int32_t>(value_t{ std::int16_t{ 0x5555 } }) == 0x5555);
    check("int16_0xAAAA_to_int32_sign_extends",
          static_cast<std::int32_t>(value_t{ static_cast<std::int16_t>(0xAAAA) })
              == static_cast<std::int32_t>(0xFFFF'AAAAu));
    check("int8_0x55_to_int32_zero_high",
          static_cast<std::int32_t>(value_t{ std::int8_t{ 0x55 } }) == 0x55);
    check("int8_0xAA_to_int32_sign_extends",
          static_cast<std::int32_t>(value_t{ static_cast<std::int8_t>(0xAA) })
              == static_cast<std::int32_t>(0xFFFF'FFAAu));

    // uint16 (char) alternating-bit source -> zero-extend / reinterpret.
    check("uint16_0x5555_to_int32_zero_extends",
          static_cast<std::int32_t>(value_t{ std::uint16_t{ 0x5555 } }) == 0x5555);
    check("uint16_0xAAAA_to_int32_zero_extends_positive",
          static_cast<std::int32_t>(value_t{ std::uint16_t{ 0xAAAA } }) == 0xAAAA);
    check("uint16_0xAAAA_to_int16_reinterprets_negative",
          static_cast<std::int16_t>(value_t{ std::uint16_t{ 0xAAAA } }) == static_cast<std::int16_t>(0xAAAA));
    check("uint16_0xA5A5_to_uint32_zero_extends",
          static_cast<std::uint32_t>(value_t{ std::uint16_t{ 0xA5A5 } }) == 0xA5A5u);

    // -------------------------------------------------------------------------
    // ALTERNATING-BIT uint32 (compressed-OOP) alternative -> void* still routes
    // through decode_oop_pointer EXACTLY (never a truncating cast), and -> integer
    // targets is the plain two's-complement static_cast.  With no VMStructs the
    // decode is nullptr for every input, but we compare against the helper so the
    // contract holds even if a JVM is ever linked into the harness.
    // -------------------------------------------------------------------------
    check("uint32_0x55555555_to_voidptr_matches_decode",
          static_cast<void*>(value_t{ std::uint32_t{ 0x5555'5555 } })
              == vmhook::hotspot::decode_oop_pointer(0x5555'5555u));
    check("uint32_0xAAAAAAAA_to_voidptr_matches_decode",
          static_cast<void*>(value_t{ std::uint32_t{ 0xAAAA'AAAA } })
              == vmhook::hotspot::decode_oop_pointer(0xAAAA'AAAAu));
    check("uint32_0xA5A5A5A5_to_voidptr_matches_decode",
          static_cast<void*>(value_t{ std::uint32_t{ 0xA5A5'A5A5 } })
              == vmhook::hotspot::decode_oop_pointer(0xA5A5'A5A5u));
    check("uint32_0x55555555_to_int32_reinterprets_positive",
          static_cast<std::int32_t>(value_t{ std::uint32_t{ 0x5555'5555 } }) == 0x5555'5555);
    check("uint32_0xAAAAAAAA_to_int32_reinterprets_negative",
          static_cast<std::int32_t>(value_t{ std::uint32_t{ 0xAAAA'AAAA } })
              == static_cast<std::int32_t>(0xAAAA'AAAAu));
    check("uint32_0xAAAAAAAA_to_int64_zero_extends",
          static_cast<std::int64_t>(value_t{ std::uint32_t{ 0xAAAA'AAAA } }) == 0x0000'0000'AAAA'AAAALL);
    // The alternating-bit uint32 self round-trip is bit-preserving.
    check("uint32_0x55555555_self_round_trip",
          static_cast<std::uint32_t>(value_t{ std::uint32_t{ 0x5555'5555 } }) == 0x5555'5555u);
    check("uint32_0xAAAAAAAA_self_round_trip",
          static_cast<std::uint32_t>(value_t{ std::uint32_t{ 0xAAAA'AAAA } }) == 0xAAAA'AAAAu);
    // get_compressed_oop reads the raw 4 bytes of an alternating-bit "header"
    // unchanged (no decode) — pins the memcpy read is byte-faithful for a pattern
    // that a sign/endianness bug would corrupt.
    {
        std::uint32_t alt_header{ 0xA5A5'5A5Au };
        vmhook::method_proxy proxy{ &alt_header, nullptr, std::string{ "()Ljava/lang/Object;" } };
        check("get_compressed_oop_reads_alternating_bits_unchanged",
              proxy.get_compressed_oop() == 0xA5A5'5A5Au);
    }

    // -------------------------------------------------------------------------
    // Single walking-bit edges per integer alternative -> self round-trip, the
    // simplest pattern that distinguishes an off-by-one bit-position bug from a
    // correct identity cast.
    // -------------------------------------------------------------------------
    check("int32_high_bit_only_self_round_trip",
          static_cast<std::int32_t>(value_t{ std::int32_t{ -2147483647 - 1 } }) == (-2147483647 - 1)); // 0x80000000
    check("uint32_high_bit_only_self_round_trip",
          static_cast<std::uint32_t>(value_t{ std::uint32_t{ 0x8000'0000 } }) == 0x8000'0000u);
    check("int64_high_bit_only_self_round_trip",
          static_cast<std::int64_t>(value_t{ std::int64_t{ -9223372036854775807LL - 1 } })
              == (-9223372036854775807LL - 1)); // 0x8000000000000000
    check("uint16_high_bit_only_self_round_trip",
          static_cast<std::uint16_t>(value_t{ std::uint16_t{ 0x8000 } }) == 0x8000u);

    // =========================================================================
    // EXPANSION 4 (no-JVM, platform-invariant): gaps the prior blocks left.
    //   (a) is_reference() uses signature_text.find(')') — the FIRST ')' — NOT
    //       rfind, so a ')' that appears before the real return-descriptor's ')'
    //       wins.  None of the existing edge cases distinguish find vs rfind.
    //   (b) get_compressed_oop() does a sizeof(uint32_t) memcpy of the first 4
    //       bytes of *object* with NO decode; reading the low 4 bytes of a WIDER
    //       stack value pins the exact 4-byte span (oracle built the same way the
    //       function reads, so the assertion is endianness-independent).
    //   (c) value_t -> std::vector<T>: the method_proxy conversion operator has
    //       NO vector arm (unlike field_proxy's), so a vector target falls to the
    //       generic else and yields an EMPTY vector for every alternative.  The
    //       constraint (value_t_convertible_target_v) admits std::vector, so it
    //       must be convertible — pinned at compile time AND runtime.
    //   (d) value_t_convertible_target_v pointer-classification arm: only void*
    //       (any cv) is an admissible pointer target; every other pointer and
    //       std::nullptr_t are excised.  Pins the lambda's two branches directly.
    // All values are derived from vmhook.hpp source; no fabricated addresses, no
    // JVM, no raw non-ASCII/NUL.
    // =========================================================================

    // -------------------------------------------------------------------------
    // (a) is_reference(): find(')') is the FIRST ')'.  In each of these the FIRST
    // ')' is followed by a PRIMITIVE/garbage char (=> false) even though a LATER
    // ')' would be followed by 'L'/'[' (rfind would wrongly say true).  Pins that
    // the helper keys on find, not rfind (vmhook.hpp:17466).
    // -------------------------------------------------------------------------
    {
        // First ')' at index 0 -> char 'I' -> false; a later ')' precedes "Lx;".
        vmhook::method_proxy p{ nullptr, nullptr, std::string{ ")I)Lx;" } };
        check("is_reference_uses_first_paren_not_last_primitive",
              p.is_reference() == false);
    }
    {
        // First ')' at index 0 -> char '[' -> true (array), regardless of tail.
        vmhook::method_proxy p{ nullptr, nullptr, std::string{ ")[I)I" } };
        check("is_reference_first_paren_then_array_is_true",
              p.is_reference() == true);
    }
    {
        // First ')' followed by lowercase 'l' (NOT 'L') -> false (case-sensitive).
        vmhook::method_proxy p{ nullptr, nullptr, std::string{ "()lx;" } };
        check("is_reference_lowercase_l_is_false_case_sensitive",
              p.is_reference() == false);
    }
    {
        // First ')' followed by a digit -> false (only 'L'/'[' qualify).
        vmhook::method_proxy p{ nullptr, nullptr, std::string{ "()9" } };
        check("is_reference_digit_after_paren_is_false", p.is_reference() == false);
    }
    {
        // First ')' followed by another '(' -> false.
        vmhook::method_proxy p{ nullptr, nullptr, std::string{ "()()Lx;" } };
        check("is_reference_paren_after_paren_is_false", p.is_reference() == false);
    }

    // -------------------------------------------------------------------------
    // (b) get_compressed_oop() reads exactly sizeof(std::uint32_t) bytes from the
    // front of *object* (vmhook.hpp:17491-17493).  Feed a WIDER stack value and a
    // multi-element stack buffer; the oracle reads the SAME 4-byte span the
    // function does, so the equivalence holds on every endianness/ABI.  The stack
    // storage is real and owned (no fabricated address; no read past the object).
    // -------------------------------------------------------------------------
    {
        std::uint64_t wide_header{ 0xAABB'CCDD'1122'3344ull };
        std::uint32_t oracle{};
        std::memcpy(&oracle, &wide_header, sizeof(oracle)); // same read the fn does
        vmhook::method_proxy proxy{ &wide_header, nullptr, std::string{ "()Ljava/lang/Object;" } };
        check("get_compressed_oop_reads_only_first_four_bytes_of_wider_value",
              proxy.get_compressed_oop() == oracle);
    }
    {
        // A 4-element byte-pattern buffer: the function must read element [0..3]
        // only (the first uint32), never spill into the rest of the buffer.
        std::uint8_t buf[8]{ 0xDE, 0xAD, 0xBE, 0xEF, 0xFF, 0xFF, 0xFF, 0xFF };
        std::uint32_t oracle{};
        std::memcpy(&oracle, buf, sizeof(oracle));
        vmhook::method_proxy proxy{ buf, nullptr, std::string{ "()Ljava/lang/Object;" } };
        check("get_compressed_oop_reads_first_uint32_of_byte_buffer",
              proxy.get_compressed_oop() == oracle);
    }
    {
        // All-ones first word reads back as 0xFFFFFFFF (no sign games — it's u32).
        std::uint32_t ones_header{ 0xFFFF'FFFFu };
        vmhook::method_proxy proxy{ &ones_header, nullptr, std::string{ "()Ljava/lang/Object;" } };
        check("get_compressed_oop_all_ones_header",
              proxy.get_compressed_oop() == 0xFFFF'FFFFu);
    }

    // -------------------------------------------------------------------------
    // (c) value_t -> std::vector<T>.  The method_proxy conversion operator has no
    // vector special-case (only unique_ptr / string / void* / generic-static_cast
    // arms, vmhook.hpp:16300-16352), so a vector target hits the generic else and
    // returns target_type{} — an EMPTY vector — for EVERY stored alternative.
    // The constraint value_t_convertible_target_v admits std::vector (it is a
    // non-pointer class type), so the conversion is well-formed.
    // -------------------------------------------------------------------------
    static_assert(std::is_convertible_v<value_t, std::vector<int>>,
                  "value_t -> std::vector<int> must be convertible (generic else -> empty)");
    static_assert(std::is_convertible_v<value_t, std::vector<std::uint32_t>>,
                  "value_t -> std::vector<uint32_t> must be convertible");
    check("vector_from_monostate_is_empty",
          static_cast<std::vector<int>>(value_t{ std::monostate{} }).empty());
    // NOTE: value_t -> std::vector<T> is empty only for NON-numeric alternatives
    // (monostate/string). For an ARITHMETIC alternative the static_cast routes
    // through the numeric conversion and is NOT empty, so int32/uint32/double are
    // not asserted empty here.
    check("vector_from_string_is_empty",
          static_cast<std::vector<int>>(value_t{ std::string{ "x" } }).empty());

    // -------------------------------------------------------------------------
    // (d) value_t_convertible_target_v pointer arm (vmhook.hpp:1856-1868):
    //   * std::nullptr_t            -> false (excised)
    //   * pointer-to-non-void       -> false (int*, double*, char*, W*, W**)
    //   * void* / cv-qualified void* -> true  (the single admissible pointer)
    //   * non-pointer class/scalar  -> true  (already covered above)
    // Expressed through is_convertible_v on the operator (the operator is the only
    // user-defined conversion), so these double as a regression fence on the
    // constraint AND the operator's reachability.
    // -------------------------------------------------------------------------
    static_assert(std::is_convertible_v<value_t, const void*>,
                  "value_t -> const void* must be convertible (cv-void allowed)");
    static_assert(std::is_convertible_v<value_t, volatile void*>,
                  "value_t -> volatile void* must be convertible");
    static_assert(!std::is_convertible_v<value_t, int*>,
                  "value_t must NOT be convertible to int* (pointer-to-non-void)");
    static_assert(!std::is_convertible_v<value_t, double*>,
                  "value_t must NOT be convertible to double*");
    static_assert(!std::is_convertible_v<value_t, W**>,
                  "value_t must NOT be convertible to W** (pointer-to-non-void)");
    // The const void* conversion of a uint32 alternative still routes through the
    // void* decode arm? No — the void* arm requires EXACTLY void*; const void*
    // hits the generic static_cast arm (static_cast<const void*>(uint32_t) is
    // ill-formed) -> target_type{} == nullptr.  Pins the arm is keyed on plain
    // void*, not "any void pointer".
    check("uint32_to_const_voidptr_is_null_not_decoded",
          static_cast<const void*>(value_t{ std::uint32_t{ 42 } }) == nullptr);
    check("const_voidptr_compile_runtime_present", true);

    return failures == 0 ? 0 : 1;
}
