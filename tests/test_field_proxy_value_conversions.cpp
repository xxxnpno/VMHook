// Standalone (no-JVM) unit tests for field_proxy::value_t variant -> C++ conversions,
// implicit operator target_type, compressed-OOP-to-void* routing, signature() round-trip,
// is_reference()/is_static()/raw_address()/get_compressed_oop() surface behaviour.
//
// All cases run WITHOUT a live JVM in-process. Every field_proxy below is built over a
// stack buffer (or a null pointer) so get()/set() touch only the bytes we own. The variant
// alternatives, the implicit conversion operator, and the boundary helpers are all pure
// logic and null-safe; anything that needs a live oop (decoding a NON-zero compressed OOP
// via VMStructs, reading a real Java String, etc.) is OUT OF SCOPE here and is covered by
// JVM integration in example.cpp.
//
// JVM-safety notes that gate which cases are exercised here:
//   * field_proxy::get() with a null field_pointer early-returns value_t{int32_t{}, sig}
//     for EVERY signature (no VMStruct access) — see vmhook.hpp get() null guard.
//   * decode_oop_pointer(0) returns nullptr directly without touching VMStructs, so the
//     compressed-OOP-to-void* path is only safe to assert for the compressed value 0.
//     A non-zero compressed OOP would call iterate_struct_entries() and needs a JVM.
//   * cast_for_variant<void*, T> returns nullptr unless the stored alternative is uint32_t.

#include <vmhook/vmhook.hpp>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <variant>
#include <vector>

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

// Variant alternative order in field_proxy::value_t::data (must mirror vmhook.hpp):
//   0 bool, 1 int8_t, 2 int16_t, 3 int32_t, 4 int64_t, 5 float, 6 double,
//   7 uint16_t, 8 uint32_t (reference / array compressed OOP).
namespace idx
{
    constexpr std::size_t k_bool{ 0 };
    constexpr std::size_t k_i8{ 1 };
    constexpr std::size_t k_i16{ 2 };
    constexpr std::size_t k_i32{ 3 };
    constexpr std::size_t k_i64{ 4 };
    constexpr std::size_t k_float{ 5 };
    constexpr std::size_t k_double{ 6 };
    constexpr std::size_t k_u16{ 7 };
    constexpr std::size_t k_u32{ 8 };
}

// Build a field_proxy over a small stack buffer whose first sizeof(T) bytes hold `value`,
// then return its get() result. Helper keeps the per-signature cases terse.
template<typename T>
static auto read_back(const char* sig, T value) -> vmhook::field_proxy::value_t
{
    std::array<std::uint8_t, 16> storage{};
    storage.fill(std::uint8_t{ 0xAB });
    std::memcpy(storage.data(), &value, sizeof(value));
    vmhook::field_proxy proxy{ storage.data(), sig, false };
    return proxy.get();
}

int main()
{
    // ---------------------------------------------------------------------
    // 1. Each primitive signature selects the documented variant alternative
    //    and round-trips its value through the matching std::get<T>.
    // ---------------------------------------------------------------------
    {
        auto v = read_back<std::int8_t>("B", std::int8_t{ -1 });
        check("B_selects_int8_alternative", v.data.index() == idx::k_i8);
        check("B_value_is_minus_one", std::get<std::int8_t>(v.data) == std::int8_t{ -1 });
    }
    {
        // 0x80 byte -> INT8_MIN (-128) via the signed int8_t alternative.
        auto v = read_back<std::int8_t>("B", std::int8_t{ -128 });
        check("B_byte_0x80_is_int8_min", std::get<std::int8_t>(v.data) == std::int8_t{ -128 });
    }
    {
        auto v = read_back<std::int8_t>("B", std::int8_t{ 127 });
        check("B_byte_0x7F_is_int8_max", std::get<std::int8_t>(v.data) == std::int8_t{ 127 });
    }
    {
        auto v = read_back<std::int16_t>("S", std::int16_t{ -1 });
        check("S_selects_int16_alternative", v.data.index() == idx::k_i16);
        check("S_value_is_minus_one", std::get<std::int16_t>(v.data) == std::int16_t{ -1 });
    }
    {
        auto v = read_back<std::int16_t>("S", std::int16_t{ -32768 });
        check("S_value_is_int16_min", std::get<std::int16_t>(v.data) == std::int16_t{ -32768 });
    }
    {
        auto v = read_back<std::int32_t>("I", std::int32_t{ -2147483647 - 1 });
        check("I_selects_int32_alternative", v.data.index() == idx::k_i32);
        check("I_value_is_int32_min",
              std::get<std::int32_t>(v.data) == (std::int32_t{ -2147483647 } - 1));
    }
    {
        auto v = read_back<std::int32_t>("I", std::int32_t{ 2147483647 });
        check("I_value_is_int32_max", std::get<std::int32_t>(v.data) == std::int32_t{ 2147483647 });
    }
    {
        auto v = read_back<std::int64_t>("J", std::int64_t{ -9223372036854775807LL - 1 });
        check("J_selects_int64_alternative", v.data.index() == idx::k_i64);
        check("J_value_is_int64_min",
              std::get<std::int64_t>(v.data) == (std::int64_t{ -9223372036854775807LL } - 1));
    }
    {
        auto v = read_back<std::int64_t>("J", std::int64_t{ 9223372036854775807LL });
        check("J_value_is_int64_max",
              std::get<std::int64_t>(v.data) == std::int64_t{ 9223372036854775807LL });
    }
    {
        auto v = read_back<std::uint16_t>("C", std::uint16_t{ 0x0041 });
        check("C_selects_uint16_alternative", v.data.index() == idx::k_u16);
        check("C_value_is_uppercase_A", std::get<std::uint16_t>(v.data) == std::uint16_t{ 0x41 });
    }

    // ---------------------------------------------------------------------
    // 2. bool ("Z") canonical reads. Non-canonical backing bytes are the
    //    documented get() bug (raw memcpy into bool is UB to observe), so we
    //    only assert the canonical 0x00 / 0x01 contract here.
    // ---------------------------------------------------------------------
    {
        auto v = read_back<std::uint8_t>("Z", std::uint8_t{ 0x00 });
        check("Z_selects_bool_alternative", v.data.index() == idx::k_bool);
        check("Z_byte_zero_is_false", std::get<bool>(v.data) == false);
        check("Z_byte_zero_operator_bool_false", static_cast<bool>(v) == false);
    }
    {
        auto v = read_back<std::uint8_t>("Z", std::uint8_t{ 0x01 });
        check("Z_byte_one_is_true", std::get<bool>(v.data) == true);
        check("Z_byte_one_operator_bool_true", static_cast<bool>(v) == true);
        check("Z_byte_one_operator_int_is_1", static_cast<int>(v) == 1);
    }

    // ---------------------------------------------------------------------
    // 3. float / double ("F" / "D") bit-exact preservation through the
    //    variant. memcpy in get() preserves all bits; we assert finite values
    //    survive exactly and select the right alternative.
    // ---------------------------------------------------------------------
    {
        const float f{ -1.5f };
        auto v = read_back<float>("F", f);
        check("F_selects_float_alternative", v.data.index() == idx::k_float);
        check("F_value_bit_exact", std::get<float>(v.data) == f);
        check("F_operator_float_round_trips", static_cast<float>(v) == f);
    }
    {
        const double d{ 3.141592653589793 };
        auto v = read_back<double>("D", d);
        check("D_selects_double_alternative", v.data.index() == idx::k_double);
        check("D_value_bit_exact", std::get<double>(v.data) == d);
        check("D_operator_double_round_trips", static_cast<double>(v) == d);
    }

    // ---------------------------------------------------------------------
    // 4. Implicit operator target_type() cross-casts from the stored
    //    alternative. Pins the documented sign-extension / widening behaviour.
    // ---------------------------------------------------------------------
    {
        // "I" holding INT32_MIN, widened to int64_t, must sign-extend.
        auto v = read_back<std::int32_t>("I", std::int32_t{ -2147483647 - 1 });
        const std::int64_t widened{ v };
        check("I_int32_min_widens_to_int64_with_sign_extension",
              widened == static_cast<std::int64_t>(std::int32_t{ -2147483647 } - 1));
    }
    {
        // "B" holding 0xFF (int8_t -1) -> uint32_t is signed-then-widened-then-unsigned.
        auto v = read_back<std::int8_t>("B", std::int8_t{ -1 });
        const std::uint32_t u{ v };
        check("B_minus_one_casts_to_uint32_all_ones", u == 0xFFFFFFFFu);
    }
    {
        // "C" code unit converts losslessly to char16_t and to int.
        auto v = read_back<std::uint16_t>("C", std::uint16_t{ 0x4E2D }); // U+4E2D
        check("C_casts_to_char16_losslessly",
              static_cast<char16_t>(v) == static_cast<char16_t>(0x4E2D));
        check("C_casts_to_int_is_full_code_unit", static_cast<int>(v) == 0x4E2D);
    }

    // ---------------------------------------------------------------------
    // 5. Null field_pointer: get() must not crash and returns int32_t{} for
    //    every signature (documented null-guard contract). Numeric/bool casts
    //    collapse to zero/false.
    // ---------------------------------------------------------------------
    {
        vmhook::field_proxy fp{ nullptr, "J", false };
        auto v = fp.get();
        check("null_ptr_get_returns_int32_alternative", v.data.index() == idx::k_i32);
        check("null_ptr_get_signature_round_trips", v.signature == "J");
        check("null_ptr_get_casts_to_zero_int64", static_cast<std::int64_t>(v) == 0);
    }
    {
        vmhook::field_proxy fp{ nullptr, "Z", false };
        auto v = fp.get();
        check("null_ptr_Z_casts_to_false", static_cast<bool>(v) == false);
        check("null_ptr_Z_signature_round_trips", v.signature == "Z");
    }
    {
        vmhook::field_proxy fp{ nullptr, "D", false };
        check("null_ptr_D_casts_to_zero_double", static_cast<double>(fp.get()) == 0.0);
    }

    // ---------------------------------------------------------------------
    // 6. Compressed-OOP-to-void* routing via decode_oop_pointer.
    //    Only the compressed value 0 is JVM-safe (decode_oop_pointer(0) ==
    //    nullptr without VMStruct access). A reference field holding 0 routes
    //    to a null void*. A null-pointer proxy is int32_t{} (not uint32_t), so
    //    cast_for_variant<void*> takes the "not a compressed OOP" branch and
    //    also yields nullptr.
    // ---------------------------------------------------------------------
    {
        std::array<std::uint8_t, 16> storage{};
        storage.fill(std::uint8_t{ 0x00 }); // compressed OOP == 0
        vmhook::field_proxy proxy{ storage.data(), "Ljava/lang/String;", false };
        auto v = proxy.get();
        check("ref_field_selects_uint32_alternative", v.data.index() == idx::k_u32);
        check("ref_field_zero_oop_value_is_zero", std::get<std::uint32_t>(v.data) == 0u);
        check("ref_field_zero_oop_routes_to_null_void_ptr",
              static_cast<void*>(v) == nullptr);
    }
    {
        // Array signature behaves the same as a plain reference for routing.
        std::array<std::uint8_t, 16> storage{};
        storage.fill(std::uint8_t{ 0x00 });
        vmhook::field_proxy proxy{ storage.data(), "[I", false };
        auto v = proxy.get();
        check("array_field_selects_uint32_alternative", v.data.index() == idx::k_u32);
        check("array_field_zero_oop_routes_to_null_void_ptr",
              static_cast<void*>(v) == nullptr);
    }
    {
        // Non-uint32 alternative (here int32 from the null guard) -> void* must
        // be nullptr, exercising the "only convert from uint32_t" guard.
        vmhook::field_proxy fp{ nullptr, "Ljava/lang/String;", false };
        check("non_compressed_oop_alternative_casts_to_null_void_ptr",
              static_cast<void*>(fp.get()) == nullptr);
    }

    // ---------------------------------------------------------------------
    // 7. signature() round-trips the exact descriptor bytes for primitives,
    //    references, and arrays.
    // ---------------------------------------------------------------------
    {
        const char* const descriptors[]{
            "Z", "B", "S", "I", "J", "F", "D", "C",
            "Ljava/lang/String;", "[I", "[Ljava/lang/Object;"
        };
        bool all_match{ true };
        for (const char* d : descriptors)
        {
            vmhook::field_proxy proxy{ nullptr, d, true };
            if (proxy.signature() != std::string_view{ d }) { all_match = false; }
        }
        check("signature_round_trips_all_descriptors", all_match);
    }
    {
        // signature() view aliases the proxy's own storage (no copy).
        vmhook::field_proxy proxy{ nullptr, "Ljava/lang/String;", false };
        const std::string_view sig{ proxy.signature() };
        check("signature_view_has_expected_size", sig.size() == std::strlen("Ljava/lang/String;"));
        check("signature_view_equals_descriptor", sig == "Ljava/lang/String;");
    }

    // ---------------------------------------------------------------------
    // 8. is_reference(): true for L / [ descriptors, false for primitives and
    //    the empty descriptor.
    // ---------------------------------------------------------------------
    {
        vmhook::field_proxy ref{ nullptr, "Ljava/lang/String;", false };
        vmhook::field_proxy arr{ nullptr, "[I", false };
        vmhook::field_proxy obj_arr{ nullptr, "[Ljava/lang/Object;", false };
        vmhook::field_proxy prim_i{ nullptr, "I", false };
        vmhook::field_proxy prim_z{ nullptr, "Z", false };
        vmhook::field_proxy empty{ nullptr, "", false };
        check("is_reference_true_for_L_descriptor", ref.is_reference() == true);
        check("is_reference_true_for_array_primitive", arr.is_reference() == true);
        check("is_reference_true_for_array_of_objects", obj_arr.is_reference() == true);
        check("is_reference_false_for_int", prim_i.is_reference() == false);
        check("is_reference_false_for_bool", prim_z.is_reference() == false);
        check("is_reference_false_for_empty_signature", empty.is_reference() == false);
    }

    // ---------------------------------------------------------------------
    // 9. is_static() echoes the constructor flag verbatim, including on a
    //    null-pointer proxy.
    // ---------------------------------------------------------------------
    {
        vmhook::field_proxy static_proxy{ nullptr, "I", true };
        vmhook::field_proxy instance_proxy{ nullptr, "I", false };
        check("is_static_true_when_constructed_static", static_proxy.is_static() == true);
        check("is_static_false_when_constructed_instance", instance_proxy.is_static() == false);
    }
    {
        // Static and instance proxies over the same bytes read identical values:
        // get() does not consult the static flag.
        std::array<std::uint8_t, 16> storage{};
        storage.fill(std::uint8_t{ 0xAB });
        const std::int32_t marker{ 0x0BADF00D };
        std::memcpy(storage.data(), &marker, sizeof(marker));
        vmhook::field_proxy as_static{ storage.data(), "I", true };
        vmhook::field_proxy as_instance{ storage.data(), "I", false };
        check("static_and_instance_get_agree",
              static_cast<std::int32_t>(as_static.get())
                  == static_cast<std::int32_t>(as_instance.get()));
        check("static_flag_does_not_change_value",
              static_cast<std::int32_t>(as_static.get()) == marker);
    }

    // ---------------------------------------------------------------------
    // 10. raw_address() echoes the constructor pointer exactly, applying no
    //     internal adjustment, for primitives, references, null, and a
    //     deliberately bogus pointer. Also a compile-time noexcept guarantee.
    // ---------------------------------------------------------------------
    {
        std::array<std::uint8_t, 16> storage{};
        void* const base{ storage.data() };
        vmhook::field_proxy prim{ base, "I", false };
        vmhook::field_proxy ref{ base, "Ljava/lang/String;", true };
        vmhook::field_proxy arr{ base, "[I", false };
        check("raw_address_echoes_pointer_primitive", prim.raw_address() == base);
        check("raw_address_echoes_pointer_reference", ref.raw_address() == base);
        check("raw_address_echoes_pointer_array", arr.raw_address() == base);
        check("raw_address_two_proxies_same_pointer_agree",
              prim.raw_address() == ref.raw_address());
    }
    {
        vmhook::field_proxy null_proxy{ nullptr, "I", false };
        check("raw_address_null_base_is_null", null_proxy.raw_address() == nullptr);

        void* const bogus{ reinterpret_cast<void*>(static_cast<std::uintptr_t>(1)) };
        vmhook::field_proxy bogus_proxy{ bogus, "I", false };
        check("raw_address_passes_bogus_pointer_unchanged",
              bogus_proxy.raw_address() == bogus);
    }
    {
        static_assert(noexcept(std::declval<vmhook::field_proxy>().raw_address()),
                      "raw_address must be noexcept");
        check("raw_address_is_noexcept", true);
    }
    {
        // raw_address() points at the same bytes get() reads from.
        std::array<std::uint8_t, 16> storage{};
        storage.fill(std::uint8_t{ 0xAB });
        const std::int32_t planted{ 0x12345678 };
        std::memcpy(storage.data(), &planted, sizeof(planted));
        vmhook::field_proxy proxy{ storage.data(), "I", false };
        const std::int32_t via_raw{ *static_cast<std::int32_t*>(proxy.raw_address()) };
        check("raw_address_offset_matches_get",
              via_raw == static_cast<std::int32_t>(proxy.get()));
    }

    // ---------------------------------------------------------------------
    // 11. get_compressed_oop(): null field_pointer returns 0; a non-null
    //     pointer returns exactly the first 4 bytes (little-endian) with no
    //     over-read of adjacent bytes.
    // ---------------------------------------------------------------------
    {
        vmhook::field_proxy null_ref{ nullptr, "Ljava/lang/String;", false };
        check("get_compressed_oop_null_returns_zero", null_ref.get_compressed_oop() == 0u);
    }
    {
        std::array<std::uint8_t, 16> storage{};
        storage.fill(std::uint8_t{ 0xAB });
        const std::uint32_t sentinel{ 0xDEADBEEFu };
        std::memcpy(storage.data(), &sentinel, sizeof(sentinel));
        vmhook::field_proxy proxy{ storage.data(), "Ljava/lang/String;", false };
        check("get_compressed_oop_reads_first_4_bytes",
              proxy.get_compressed_oop() == sentinel);
        check("get_compressed_oop_does_not_over_read",
              storage[4] == 0xAB && storage[5] == 0xAB
              && storage[6] == 0xAB && storage[7] == 0xAB);
    }

    // =====================================================================
    // EXPANDED COVERAGE (additive — every expected value derived from the
    // field_proxy::get() dispatch, cast_for_variant<>, get_compressed_oop()
    // FLAW-C guard, and value_t::is_reference()/as_string() in vmhook.hpp).
    // No JVM: only the compressed value 0 is safe for the void*/string decode
    // paths; numeric static_casts of a NON-zero uint32 alternative are pure and
    // JVM-free, so those are exercised here too.
    // =====================================================================

    // ---------------------------------------------------------------------
    // 12. get() dispatch matches the signature EXACTLY.  Any descriptor that
    //     is NOT one of the eight primitive single-chars falls through to the
    //     reference/array branch and reads 4 bytes as the uint32 alternative —
    //     INCLUDING an empty signature, an unknown single char, and a 2-char
    //     "primitive-looking" string.  (With a non-null pointer; the null-proxy
    //     path is the separate int32 fallback already covered above.)
    // ---------------------------------------------------------------------
    {
        // Unknown single char "X": get() -> uint32 alternative (not a primitive
        // match), value == the 4 little-endian bytes.
        auto v = read_back<std::uint32_t>("X", std::uint32_t{ 0x0000002Au });
        check("unknown_sig_X_selects_uint32_alternative", v.data.index() == idx::k_u32);
        check("unknown_sig_X_value_is_42", std::get<std::uint32_t>(v.data) == 42u);
    }
    {
        // Empty signature with a NON-null pointer also routes to uint32.
        auto v = read_back<std::uint32_t>("", std::uint32_t{ 0xDEADBEEFu });
        check("empty_sig_nonnull_selects_uint32_alternative", v.data.index() == idx::k_u32);
        check("empty_sig_nonnull_value_round_trips",
              std::get<std::uint32_t>(v.data) == 0xDEADBEEFu);
    }
    {
        // A 2-char "II" is != "I", so it is NOT the int branch -> uint32.
        auto v = read_back<std::uint32_t>("II", std::uint32_t{ 0x12345678u });
        check("two_char_II_selects_uint32_alternative", v.data.index() == idx::k_u32);
        check("two_char_II_value_round_trips",
              std::get<std::uint32_t>(v.data) == 0x12345678u);
    }
    {
        // Lowercase "i" is not the uppercase "I" primitive -> uint32 fallback.
        auto v = read_back<std::uint32_t>("i", std::uint32_t{ 7u });
        check("lowercase_i_selects_uint32_alternative", v.data.index() == idx::k_u32);
        check("lowercase_i_value_is_7", std::get<std::uint32_t>(v.data) == 7u);
    }

    // ---------------------------------------------------------------------
    // 13. Numeric static_cast conversions of a NON-zero uint32 alternative.
    //     cast_for_variant routes a non-string/vector/unique_ptr/void* target
    //     through static_cast<target>(uint32).  These are pure arithmetic and
    //     need no JVM (unlike the void*/string decode of a non-zero OOP).
    // ---------------------------------------------------------------------
    {
        auto v = read_back<std::uint32_t>("Ljava/lang/String;", std::uint32_t{ 42u });
        check("uint32_alt_casts_to_int_42", static_cast<int>(v) == 42);
        check("uint32_alt_casts_to_int64_42", static_cast<std::int64_t>(v) == 42);
        check("uint32_alt_casts_to_uint32_42", static_cast<std::uint32_t>(v) == 42u);
        check("uint32_alt_value_is_reference_true", v.is_reference() == true);
    }
    {
        // uint32 0xFFFFFFFF -> int is a bit-reinterpret (-1); -> int64 zero-
        // extends to 4294967295 (uint32 widened through the value, NOT sign
        // extended, because the stored alternative is unsigned).
        auto v = read_back<std::uint32_t>("[I", std::uint32_t{ 0xFFFFFFFFu });
        check("uint32_max_alt_casts_to_int_minus_one", static_cast<int>(v) == -1);
        check("uint32_max_alt_casts_to_int64_4294967295",
              static_cast<std::int64_t>(v) == static_cast<std::int64_t>(4294967295LL));
        check("uint32_max_alt_casts_to_uint32_all_ones",
              static_cast<std::uint32_t>(v) == 0xFFFFFFFFu);
    }

    // ---------------------------------------------------------------------
    // 14. int8 ("B") cross-casts: -1 fans out to every numeric target with the
    //     documented sign/zero extension via static_cast from int8_t.
    // ---------------------------------------------------------------------
    {
        auto v = read_back<std::int8_t>("B", std::int8_t{ -1 });
        check("B_minus_one_to_int16_is_minus_one", static_cast<std::int16_t>(v) == -1);
        check("B_minus_one_to_int32_is_minus_one", static_cast<std::int32_t>(v) == -1);
        check("B_minus_one_to_int64_is_minus_one", static_cast<std::int64_t>(v) == -1);
        check("B_minus_one_to_uint8_is_0xFF",
              static_cast<std::uint8_t>(v) == std::uint8_t{ 0xFF });
        check("B_minus_one_to_uint16_is_0xFFFF",
              static_cast<std::uint16_t>(v) == std::uint16_t{ 0xFFFF });
        check("B_minus_one_to_float_is_minus_one", static_cast<float>(v) == -1.0f);
        check("B_minus_one_to_double_is_minus_one", static_cast<double>(v) == -1.0);
    }

    // ---------------------------------------------------------------------
    // 15. char ("C", uint16) cross-casts: 0xFFFF wraps to -1 as int16, widens
    //     to 65535 as int/int64, narrows to 0xFF as uint8, and converts exactly
    //     to float/double.
    // ---------------------------------------------------------------------
    {
        auto v = read_back<std::uint16_t>("C", std::uint16_t{ 0xFFFF });
        check("C_0xFFFF_to_int16_is_minus_one", static_cast<std::int16_t>(v) == -1);
        check("C_0xFFFF_to_int_is_65535", static_cast<int>(v) == 65535);
        check("C_0xFFFF_to_int64_is_65535", static_cast<std::int64_t>(v) == 65535);
        check("C_0xFFFF_to_uint8_is_0xFF",
              static_cast<std::uint8_t>(v) == std::uint8_t{ 0xFF });
        check("C_0xFFFF_to_float_is_65535", static_cast<float>(v) == 65535.0f);
        check("C_0xFFFF_to_double_is_65535", static_cast<double>(v) == 65535.0);
    }

    // ---------------------------------------------------------------------
    // 16. Floating-point -> integral truncation (toward zero) and float->int64.
    //     static_cast<int>(3.9f) == 3; static_cast<int>(-3.9f) == -3.
    // ---------------------------------------------------------------------
    {
        auto v = read_back<float>("F", 3.9f);
        check("F_3p9_truncates_to_int_3", static_cast<int>(v) == 3);
        check("F_3p9_truncates_to_int64_3", static_cast<std::int64_t>(v) == 3);
    }
    {
        auto v = read_back<float>("F", -3.9f);
        check("F_minus_3p9_truncates_to_int_minus_3", static_cast<int>(v) == -3);
    }
    {
        auto v = read_back<double>("D", 2.999);
        check("D_2p999_truncates_to_int_2", static_cast<int>(v) == 2);
        check("D_2p999_truncates_to_int64_2", static_cast<std::int64_t>(v) == 2);
    }

    // ---------------------------------------------------------------------
    // 17. bool ("Z") cross-casts: true -> 1 across numeric targets, false -> 0.
    // ---------------------------------------------------------------------
    {
        auto t = read_back<std::uint8_t>("Z", std::uint8_t{ 0x01 });
        check("Z_true_to_int_1", static_cast<int>(t) == 1);
        check("Z_true_to_int64_1", static_cast<std::int64_t>(t) == 1);
        check("Z_true_to_uint32_1", static_cast<std::uint32_t>(t) == 1u);
        check("Z_true_to_float_1", static_cast<float>(t) == 1.0f);
        check("Z_true_to_double_1", static_cast<double>(t) == 1.0);

        auto f = read_back<std::uint8_t>("Z", std::uint8_t{ 0x00 });
        check("Z_false_to_int_0", static_cast<int>(f) == 0);
        check("Z_false_to_double_0", static_cast<double>(f) == 0.0);
    }

    // ---------------------------------------------------------------------
    // 18. int32 / int64 widening & narrowing boundaries through the operator.
    // ---------------------------------------------------------------------
    {
        // INT32_MAX widened to int64 (no sign issue, positive).
        auto v = read_back<std::int32_t>("I", std::int32_t{ 2147483647 });
        check("I_int32_max_widens_to_int64", static_cast<std::int64_t>(v) == 2147483647LL);
    }
    {
        // int64 value that is exactly representable as double (2^53) round-trips
        // through static_cast<double> with no precision loss.
        const std::int64_t exact{ 9007199254740992LL };  // 2^53
        auto v = read_back<std::int64_t>("J", exact);
        check("J_2pow53_casts_to_double_exactly",
              static_cast<double>(v) == 9007199254740992.0);
    }
    {
        // A small int64 -> int32 narrowing keeps the low value.
        auto v = read_back<std::int64_t>("J", std::int64_t{ 1000 });
        check("J_small_value_narrows_to_int32_1000", static_cast<std::int32_t>(v) == 1000);
    }

    // ---------------------------------------------------------------------
    // 19. value_t::is_reference(): true ONLY for the uint32 alternative, false
    //     for every primitive alternative and for the null-proxy int32 fallback.
    //     (This is the value_t-level holds_alternative<uint32> test, distinct
    //     from the proxy-level signature-first-char is_reference() below.)
    // ---------------------------------------------------------------------
    {
        check("value_is_reference_false_for_int", read_back<std::int32_t>("I", 1).is_reference() == false);
        check("value_is_reference_false_for_bool", read_back<std::uint8_t>("Z", std::uint8_t{ 1 }).is_reference() == false);
        check("value_is_reference_false_for_double", read_back<double>("D", 1.0).is_reference() == false);
        check("value_is_reference_false_for_char", read_back<std::uint16_t>("C", std::uint16_t{ 65 }).is_reference() == false);

        // Reference field -> uint32 alternative -> value_t::is_reference() true.
        std::array<std::uint8_t, 16> storage{};
        storage.fill(std::uint8_t{ 0x00 });
        vmhook::field_proxy ref{ storage.data(), "Ljava/lang/String;", false };
        check("value_is_reference_true_for_ref_field", ref.get().is_reference() == true);

        // Null proxy -> int32 alternative -> value_t::is_reference() false.
        vmhook::field_proxy null_ref{ nullptr, "Ljava/lang/String;", false };
        check("value_is_reference_false_for_null_proxy", null_ref.get().is_reference() == false);
    }

    // ---------------------------------------------------------------------
    // 20. value_t::as_string(): "" for every primitive alternative and for a
    //     uint32 alternative whose OOP is 0 (decode_oop_pointer(0)==nullptr ->
    //     read_java_string(nullptr)==""), with no JVM.  Never decodes a non-zero
    //     OOP here (that needs a live heap).
    // ---------------------------------------------------------------------
    {
        check("as_string_empty_for_int", read_back<std::int32_t>("I", 12345).as_string().empty());
        check("as_string_empty_for_double", read_back<double>("D", 3.14).as_string().empty());
        check("as_string_empty_for_bool", read_back<std::uint8_t>("Z", std::uint8_t{ 1 }).as_string().empty());

        // Reference field with a ZERO compressed OOP -> "" (null decode).
        std::array<std::uint8_t, 16> storage{};
        storage.fill(std::uint8_t{ 0x00 });
        vmhook::field_proxy ref{ storage.data(), "Ljava/lang/String;", false };
        check("as_string_empty_for_zero_oop_ref", ref.get().as_string().empty());

        // Null proxy -> int32 alternative -> "".
        vmhook::field_proxy null_ref{ nullptr, "Ljava/lang/String;", false };
        check("as_string_empty_for_null_proxy", null_ref.get().as_string().empty());
    }

    // ---------------------------------------------------------------------
    // 21. get_compressed_oop() FLAW-C guard: returns 0 on a PRIMITIVE field
    //     even with a non-null pointer and non-zero bytes (because the proxy's
    //     is_reference() is false for a primitive descriptor), and ALSO 0 for an
    //     unknown single-char signature like "X" (first char not 'L'/'[').  Only
    //     genuine reference/array descriptors expose the raw 4 bytes.
    // ---------------------------------------------------------------------
    {
        std::array<std::uint8_t, 16> storage{};
        storage.fill(std::uint8_t{ 0xAB });
        const std::uint32_t sentinel{ 0xCAFEBABEu };
        std::memcpy(storage.data(), &sentinel, sizeof(sentinel));

        // Primitive "I": is_reference() false -> guard fires -> 0.
        vmhook::field_proxy prim_i{ storage.data(), "I", false };
        check("get_compressed_oop_primitive_int_is_guarded_zero",
              prim_i.get_compressed_oop() == 0u);
        check("get_compressed_oop_primitive_int_is_reference_false",
              prim_i.is_reference() == false);

        // Primitive "J": same guard.
        vmhook::field_proxy prim_j{ storage.data(), "J", false };
        check("get_compressed_oop_primitive_long_is_guarded_zero",
              prim_j.get_compressed_oop() == 0u);

        // Unknown "X": proxy is_reference() false (first char not L/[) -> 0,
        // even though get() would put the bytes in the uint32 alternative.
        vmhook::field_proxy unknown_x{ storage.data(), "X", false };
        check("get_compressed_oop_unknown_X_is_guarded_zero",
              unknown_x.get_compressed_oop() == 0u);
        check("get_compressed_oop_unknown_X_proxy_is_reference_false",
              unknown_x.is_reference() == false);

        // Genuine reference "[I": guard passes -> raw 4 bytes exposed.
        vmhook::field_proxy arr{ storage.data(), "[I", false };
        check("get_compressed_oop_array_field_exposes_bytes",
              arr.get_compressed_oop() == sentinel);
    }

    // ---------------------------------------------------------------------
    // 22. Proxy-level is_reference() vs value_t-level is_reference() DIVERGE on
    //     an unknown single-char signature: get() routes "X" into the uint32
    //     alternative (value_t::is_reference() == true), while the proxy checks
    //     the first descriptor char and returns false.  Pin this contrast.
    // ---------------------------------------------------------------------
    {
        std::array<std::uint8_t, 16> storage{};
        storage.fill(std::uint8_t{ 0x11 });
        vmhook::field_proxy unknown_x{ storage.data(), "X", false };
        check("proxy_is_reference_false_for_unknown_X",
              unknown_x.is_reference() == false);
        check("value_is_reference_true_for_unknown_X_get",
              unknown_x.get().is_reference() == true);
    }

    return failures == 0 ? 0 : 1;
}
