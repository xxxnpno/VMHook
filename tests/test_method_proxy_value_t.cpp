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
#include <string>
#include <vector>

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

using value_t = vmhook::method_proxy::value_t;

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
        // Use static_cast (not brace-init): with a *templated* conversion
        // operator, `std::string s{ value_t }` runs overload resolution over
        // every conversion the operator could produce (const char*, string_view,
        // ...) and picks a surprising one — a known C++ gotcha unrelated to the
        // operator's logic.  static_cast<std::string> names the target exactly.
        const auto s = value_t{ std::monostate{} }.as_string();
        check("monostate_to_string_is_empty", s.empty());
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

    return failures == 0 ? 0 : 1;
}
