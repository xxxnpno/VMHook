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
#include <array>
#include <cstdio>
#include <cstdint>
#include <cstring>
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

// Minimal vmhook wrapper type for exercising the value_t -> std::unique_ptr<W>
// conversion arm AND the value_t_convertible_target_v constraint exclusions
// (W* must NOT be a producible target).  cast_for_variant's unique_ptr arm
// does `new W{ decoded_void_ptr }`; object_base's constructor takes oop_type_t
// (== void*), which W inherits.  W adds no state.  With NO JVM the arm never
// actually news a W for any case we drive here: every unique_ptr case below
// either hits the FLAW-B signature guard (non-'L' signature -> nullptr before
// any decode) or the non-uint32 alternative guard (-> nullptr), so no live oop
// is ever dereferenced.  Mirrors test_method_proxy_value_t.cpp's W.
struct W : vmhook::object_base
{
    using vmhook::object_base::object_base;
};

// A non-pointer, NON-AGGREGATE class used to exercise cast_for_variant's final
// `else { return target_type{}; }` fallback arm (section 41).  It is a legitimate
// value_t conversion target (not a pointer, not nullptr_t -> passes
// value_t_convertible_target_v, so the operator is enabled), yet it has NO
// constructor accepting any arithmetic / uint32 alternative, so
// `static_cast<NonCastable>(scalar)` is ill-formed and the operator can ONLY take
// the value-initialising fallback.  The user-declared default ctor (which makes it
// a non-aggregate) plants a sentinel that the fallback `NonCastable{}` re-runs; an
// aggregate would instead be reachable via brace-init static_cast and overwrite it.
// It lives at file scope (not inside main) so the requires-expressions below
// classify cleanly on GCC — a local class named in a static_assert requires-expr
// makes GCC hard-error on the failed ctor candidate rather than report the
// requirement unsatisfied.
struct NonCastable
{
    int sentinel;
    NonCastable() noexcept : sentinel{ 0xABCD } {}
};
static_assert(vmhook::detail::value_t_convertible_target_v<NonCastable>,
              "NonCastable must be an allowed (non-pointer) conversion target");
// static_cast<NonCastable>(scalar) is well-formed iff NonCastable is
// direct-constructible from that scalar; it is not, so the operator's
// cast_for_variant takes the value-initialising fallback arm.  Expressed via
// is_constructible (SFINAE-clean on every compiler) rather than a
// `requires { static_cast<...>; }` — GCC hard-errors on a failed class-target
// static_cast inside a static_assert requires-expression instead of reporting it
// as an unsatisfied requirement.
static_assert(!std::is_constructible_v<NonCastable, std::int32_t>,
              "NonCastable must NOT be constructible from an int "
              "(otherwise the fallback arm is not exercised)");
static_assert(!std::is_constructible_v<NonCastable, double>,
              "NonCastable must NOT be constructible from a double");
static_assert(!std::is_constructible_v<NonCastable, std::uint32_t>,
              "NonCastable must NOT be constructible from a uint32");
static_assert(!std::is_constructible_v<NonCastable, bool>,
              "NonCastable must NOT be constructible from a bool");

using value_t = vmhook::field_proxy::value_t;

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

    // =====================================================================
    // STATIC_CAST / CONSTRAINT EXPANSION.  The conversion operator is now
    // gated on vmhook::detail::value_t_convertible_target_v (vmhook.hpp
    // ~1630-1650), so `static_cast<std::string>(v)` and
    // `static_cast<std::unique_ptr<W>>(v)` COMPILE unambiguously on MSVC
    // /permissive- (they previously hard-errored C2440).  These sections
    // (a) prove the constraint shape at compile time, (b) drive the now-
    // legal static_cast forms at runtime, and (c) cross-check static_cast
    // against the implicit conversion so the two spellings always agree.
    // All remain no-JVM: only zero compressed OOPs and pure-arithmetic
    // alternatives are exercised; the FLAW-B unique_ptr guard and the
    // non-uint32 guards mean no live oop is ever decoded.
    // =====================================================================

    // ---------------------------------------------------------------------
    // 23. COMPILE-TIME constraint proof for field_proxy::value_t.  Mirrors
    //     the method_proxy::value_t BUG-1 block: the two class-target casts
    //     that the constraint exists to disambiguate must be well-formed,
    //     every legitimate target stays convertible, and the four spurious
    //     productions (const char*, char*, W*, std::nullptr_t) that caused
    //     the MSVC ambiguity must be EXCLUDED.  A regression in either
    //     direction (operator un-constrained again -> ambiguous -> casts
    //     ill-formed; or constraint over-tight -> a real target removed)
    //     fails the build, not just the run.
    // ---------------------------------------------------------------------
    {
        // The two cast forms that are the entire point of the constraint fix
        // are now well-formed (were ambiguous C2440 on MSVC /permissive-):
        static_assert(requires(const value_t& v) { static_cast<std::string>(v); },
                      "static_cast<std::string>(field value_t) must be well-formed");
        static_assert(requires(const value_t& v) { static_cast<std::unique_ptr<W>>(v); },
                      "static_cast<std::unique_ptr<W>>(field value_t) must be well-formed");

        // Legitimate targets remain convertible (constraint not over-tight):
        static_assert(std::is_convertible_v<value_t, bool>,
                      "value_t -> bool must remain convertible");
        static_assert(std::is_convertible_v<value_t, std::int8_t>,
                      "value_t -> int8_t must remain convertible");
        static_assert(std::is_convertible_v<value_t, std::int16_t>,
                      "value_t -> int16_t must remain convertible");
        static_assert(std::is_convertible_v<value_t, std::int32_t>,
                      "value_t -> int32_t must remain convertible");
        static_assert(std::is_convertible_v<value_t, std::int64_t>,
                      "value_t -> int64_t must remain convertible");
        static_assert(std::is_convertible_v<value_t, std::uint16_t>,
                      "value_t -> uint16_t must remain convertible");
        static_assert(std::is_convertible_v<value_t, std::uint32_t>,
                      "value_t -> uint32_t must remain convertible");
        static_assert(std::is_convertible_v<value_t, float>,
                      "value_t -> float must remain convertible");
        static_assert(std::is_convertible_v<value_t, double>,
                      "value_t -> double must remain convertible");
        static_assert(std::is_convertible_v<value_t, std::string>,
                      "value_t -> std::string must remain convertible");
        // NB: std::vector<T> is NOT asserted via std::is_convertible_v here.
        // is_convertible tests a copy-init/return context whose candidate set
        // (operator->vector<T> vs operator->size_type matching vector's
        // (size_type) ctor) is ambiguous on some STLs (MSVC /permissive-).
        // The constraint's acceptance of std::vector<T> as a legitimate target
        // is asserted at the trait level instead (value_t_convertible_target_v
        // below), which is STL-independent; the runtime copy-init reject path
        // is exercised in section 26.
        static_assert(std::is_convertible_v<value_t, std::unique_ptr<W>>,
                      "value_t -> unique_ptr<W> must remain convertible");
        static_assert(std::is_convertible_v<value_t, void*>,
                      "value_t -> void* must remain convertible (the one allowed pointer)");
        // Any cv-qualified void* is a legitimate pointer target (the trait
        // strips the pointee's cv and checks is_void_v); cast_for_variant only
        // decodes through the exact `void*` arm, so `const void*` rides the
        // generic static_cast arm (decode result implicitly converts).
        static_assert(std::is_convertible_v<value_t, const void*>,
                      "value_t -> const void* must remain convertible (cv void* allowed)");

        // The spurious targets are EXCLUDED — these are exactly the
        // productions that an UNCONSTRAINED operator would also offer and
        // that collided with std::string / unique_ptr constructors:
        static_assert(!std::is_convertible_v<value_t, const char*>,
                      "value_t must NOT be convertible to const char* (ambiguity source)");
        static_assert(!std::is_convertible_v<value_t, char*>,
                      "value_t must NOT be convertible to char*");
        static_assert(!std::is_convertible_v<value_t, W*>,
                      "value_t must NOT be convertible to W* (unique_ptr ambiguity source)");
        static_assert(!std::is_convertible_v<value_t, int*>,
                      "value_t must NOT be convertible to int*");
        static_assert(!std::is_convertible_v<value_t, std::nullptr_t>,
                      "value_t must NOT be convertible to std::nullptr_t");

        // The trait itself, exercised directly on representative targets.
        static_assert(vmhook::detail::value_t_convertible_target_v<int>);
        static_assert(vmhook::detail::value_t_convertible_target_v<std::string>);
        static_assert(vmhook::detail::value_t_convertible_target_v<void*>);
        static_assert(vmhook::detail::value_t_convertible_target_v<const void* volatile>);
        static_assert(vmhook::detail::value_t_convertible_target_v<std::unique_ptr<W>>);
        static_assert(vmhook::detail::value_t_convertible_target_v<std::vector<int>>);
        static_assert(!vmhook::detail::value_t_convertible_target_v<const char*>);
        static_assert(!vmhook::detail::value_t_convertible_target_v<char*>);
        static_assert(!vmhook::detail::value_t_convertible_target_v<W*>);
        static_assert(!vmhook::detail::value_t_convertible_target_v<std::nullptr_t>);
        // cv-ref qualifiers are stripped before classification.
        static_assert(vmhook::detail::value_t_convertible_target_v<const std::string&>);
        static_assert(vmhook::detail::value_t_convertible_target_v<std::unique_ptr<W>&&>);
        static_assert(!vmhook::detail::value_t_convertible_target_v<const char* const&>);

        // operator-as-noexcept: the conversion operator is declared noexcept.
        static_assert(noexcept(static_cast<int>(std::declval<value_t>())),
                      "value_t conversion operator must be noexcept");

        check("value_t_constraint_compile_proof_present", true);
    }

    // ---------------------------------------------------------------------
    // 24. static_cast<std::string>(value_t) RUNTIME path (the fixed form).
    //     For a numeric / null alternative cast_for_variant's std::string arm
    //     returns "" (the source is not uint32_t); for a uint32 alternative
    //     it decodes via read_java_string(decode_oop_pointer(v)), and a ZERO
    //     OOP decodes to nullptr -> "".  static_cast and as_string() must
    //     agree on every case (both name the same extraction).
    // ---------------------------------------------------------------------
    {
        auto vi = read_back<std::int32_t>("I", 999);
        check("static_cast_string_empty_for_int", static_cast<std::string>(vi).empty());
        check("static_cast_string_matches_as_string_int",
              static_cast<std::string>(vi) == vi.as_string());

        auto vj = read_back<std::int64_t>("J", std::int64_t{ 123456789 });
        check("static_cast_string_empty_for_long", static_cast<std::string>(vj).empty());

        auto vd = read_back<double>("D", 2.5);
        check("static_cast_string_empty_for_double", static_cast<std::string>(vd).empty());

        auto vz = read_back<std::uint8_t>("Z", std::uint8_t{ 1 });
        check("static_cast_string_empty_for_bool", static_cast<std::string>(vz).empty());

        auto vc = read_back<std::uint16_t>("C", std::uint16_t{ 0x41 });
        check("static_cast_string_empty_for_char", static_cast<std::string>(vc).empty());

        // Reference field with a ZERO compressed OOP -> "" via null decode.
        std::array<std::uint8_t, 16> storage{};
        storage.fill(std::uint8_t{ 0x00 });
        vmhook::field_proxy ref{ storage.data(), "Ljava/lang/String;", false };
        auto vr = ref.get();
        check("static_cast_string_empty_for_zero_oop_ref", static_cast<std::string>(vr).empty());
        check("static_cast_string_matches_as_string_zero_oop",
              static_cast<std::string>(vr) == vr.as_string());

        // Null proxy -> int32 alternative -> "".
        vmhook::field_proxy null_ref{ nullptr, "Ljava/lang/String;", false };
        check("static_cast_string_empty_for_null_proxy",
              static_cast<std::string>(null_ref.get()).empty());
    }

    // ---------------------------------------------------------------------
    // 25. static_cast<std::unique_ptr<W>>(value_t) RUNTIME path (the fixed
    //     form) — the FLAW-B reject arms, all JVM-free because they return
    //     nullptr BEFORE any decode_oop_pointer / is_valid_pointer call:
    //       * uint32 alternative + array signature "[L..."  -> nullptr
    //       * uint32 alternative + primitive-looking sig "I" -> nullptr
    //       * uint32 alternative + EMPTY signature           -> nullptr
    //       * uint32 alternative + unknown sig "X"           -> nullptr
    //       * non-uint32 alternative (null-proxy int32)      -> nullptr
    //     (A valid 'L...;' field with a LIVE oop is JVM-only and out of
    //     scope here.)
    // ---------------------------------------------------------------------
    {
        // uint32 alternative reached via the reference/array fall-through of
        // get(), but the signature's first char is not 'L' -> guard fires.
        std::array<std::uint8_t, 16> storage{};
        storage.fill(std::uint8_t{ 0x00 });

        vmhook::field_proxy arr{ storage.data(), "[Ljava/lang/Object;", false };
        auto va = arr.get();
        check("unique_ptr_null_for_array_signature",
              static_cast<std::unique_ptr<W>>(va) == nullptr);
        check("unique_ptr_array_alt_is_uint32", va.data.index() == idx::k_u32);

        vmhook::field_proxy arr_prim{ storage.data(), "[I", false };
        check("unique_ptr_null_for_primitive_array_signature",
              static_cast<std::unique_ptr<W>>(arr_prim.get()) == nullptr);

        // Unknown single char "X": get() -> uint32 alt, sig.front() != 'L'.
        vmhook::field_proxy unknown_x{ storage.data(), "X", false };
        auto vx = unknown_x.get();
        check("unique_ptr_null_for_unknown_signature",
              static_cast<std::unique_ptr<W>>(vx) == nullptr);
        check("unique_ptr_unknown_alt_is_uint32", vx.data.index() == idx::k_u32);

        // Empty signature + non-null pointer: get() -> uint32 alt, sig empty.
        vmhook::field_proxy empty_sig{ storage.data(), "", false };
        check("unique_ptr_null_for_empty_signature",
              static_cast<std::unique_ptr<W>>(empty_sig.get()) == nullptr);

        // Non-uint32 alternative (null proxy -> int32) -> nullptr arm.
        vmhook::field_proxy null_ref{ nullptr, "Ljava/lang/String;", false };
        auto vn = null_ref.get();
        check("unique_ptr_null_for_non_uint32_alternative",
              static_cast<std::unique_ptr<W>>(vn) == nullptr);
        check("unique_ptr_null_proxy_alt_is_int32", vn.data.index() == idx::k_i32);

        // A primitive value_t (int alt) -> unique_ptr is the non-uint32 arm.
        check("unique_ptr_null_for_int_alternative",
              static_cast<std::unique_ptr<W>>(read_back<std::int32_t>("I", 7)) == nullptr);

        // The implicit conversion agrees with the static_cast spelling.
        std::unique_ptr<W> implicit_form = read_back<std::int32_t>("I", 7);
        check("unique_ptr_implicit_matches_static_cast_null", implicit_form == nullptr);
    }

    // ---------------------------------------------------------------------
    // 26. value_t -> std::vector<T> reject path (via COPY-INITIALISATION, the
    //     documented spelling): a numeric or null alternative is not uint32_t,
    //     so cast_for_variant's vector arm returns an empty vector (the
    //     populated array path needs a live JVM and is out of scope).  Also a
    //     uint32 alternative whose array OOP is ZERO yields an empty vector
    //     (decode_array_oop(0) == nullptr).
    //
    //     NB: `std::vector<T>` is driven through copy-init (`std::vector<T> x =
    //     v;`) and NOT `static_cast<std::vector<T>>(v)`.  Unlike std::string /
    //     std::unique_ptr (both fixed by value_t_convertible_target_v), a
    //     std::vector<T> static_cast is STILL ambiguous on MSVC /permissive-:
    //     value_t -> vector<T> (the operator's vector arm) competes with
    //     value_t -> size_type (an arithmetic arm) against vector's
    //     (size_type) constructor.  The constraint cannot excise that without
    //     dropping arithmetic conversions, so to_vector<T>() / copy-init is the
    //     portable spelling.  See the bug note in the agent report.
    // ---------------------------------------------------------------------
    {
        std::vector<int> from_int = read_back<std::int32_t>("I", 5);
        check("vector_int_empty_for_int_alt", from_int.empty());
        std::vector<int> from_double = read_back<double>("D", 1.0);
        check("vector_int_empty_for_double_alt", from_double.empty());
        std::vector<int> from_bool = read_back<std::uint8_t>("Z", std::uint8_t{ 1 });
        check("vector_int_empty_for_bool_alt", from_bool.empty());

        // Null proxy -> int32 alt -> empty vector.
        vmhook::field_proxy null_arr{ nullptr, "[I", false };
        std::vector<int> from_null = null_arr.get();
        check("vector_int_empty_for_null_proxy", from_null.empty());

        // Array field with ZERO compressed OOP -> empty (decode_array_oop(0)).
        std::array<std::uint8_t, 16> storage{};
        storage.fill(std::uint8_t{ 0x00 });
        vmhook::field_proxy arr{ storage.data(), "[I", false };
        std::vector<int> from_zero_oop = arr.get();
        check("vector_int_empty_for_zero_array_oop", from_zero_oop.empty());

        // std::vector<bool> and std::vector<std::string> reject arms likewise.
        std::vector<bool> bool_vec = read_back<std::int32_t>("I", 5);
        check("vector_bool_empty_for_int_alt", bool_vec.empty());
        std::vector<std::string> str_vec = read_back<std::int32_t>("I", 5);
        check("vector_string_empty_for_int_alt", str_vec.empty());

        // The constraint nonetheless keeps value_t -> std::vector<T> a valid
        // (copy-init) conversion target — proven at compile time in section 23.
        check("vector_int_copy_init_present", true);
    }

    // ---------------------------------------------------------------------
    // 27. Every primitive alternative -> every compatible numeric target,
    //     driven through BOTH the implicit conversion AND static_cast, with
    //     the two spellings REQUIRED to agree.  cast_for_variant routes both
    //     through the same static_cast<target>(value) arm, so a divergence
    //     would mean the operator and an explicit cast disagree.
    // ---------------------------------------------------------------------
    {
        // Helper-free explicit dual-path assertions per source alternative.
        // int32 source.
        {
            auto v = read_back<std::int32_t>("I", std::int32_t{ -12345 });
            const int implicit_i = v;            const int cast_i = static_cast<int>(v);
            const std::int64_t implicit_l = v;   const std::int64_t cast_l = static_cast<std::int64_t>(v);
            const double implicit_d = v;         const double cast_d = static_cast<double>(v);
            check("i32_implicit_eq_static_cast_int", implicit_i == cast_i && cast_i == -12345);
            check("i32_implicit_eq_static_cast_int64", implicit_l == cast_l && cast_l == -12345);
            check("i32_implicit_eq_static_cast_double", implicit_d == cast_d && cast_d == -12345.0);
        }
        // int64 source.
        {
            auto v = read_back<std::int64_t>("J", std::int64_t{ 5000000000LL });
            const std::int64_t implicit_l = v;   const std::int64_t cast_l = static_cast<std::int64_t>(v);
            const double implicit_d = v;         const double cast_d = static_cast<double>(v);
            check("i64_implicit_eq_static_cast_int64", implicit_l == cast_l && cast_l == 5000000000LL);
            check("i64_implicit_eq_static_cast_double", implicit_d == cast_d && cast_d == 5000000000.0);
        }
        // int8 source.
        {
            auto v = read_back<std::int8_t>("B", std::int8_t{ -42 });
            check("i8_implicit_eq_static_cast_int",
                  static_cast<int>(v) == static_cast<int>(static_cast<std::int8_t>(-42))
                  && (int{ v } == static_cast<int>(v)));
        }
        // int16 source.
        {
            auto v = read_back<std::int16_t>("S", std::int16_t{ -3000 });
            check("i16_implicit_eq_static_cast_int",
                  int{ v } == static_cast<int>(v) && static_cast<int>(v) == -3000);
        }
        // uint16 (char) source.
        {
            auto v = read_back<std::uint16_t>("C", std::uint16_t{ 50000 });
            const int implicit_i = v;
            check("u16_implicit_eq_static_cast_int",
                  implicit_i == static_cast<int>(v) && static_cast<int>(v) == 50000);
        }
        // float source.
        {
            auto v = read_back<float>("F", 12.0f);
            const double implicit_d = v;
            check("f32_implicit_eq_static_cast_double",
                  implicit_d == static_cast<double>(v) && static_cast<double>(v) == 12.0);
            check("f32_implicit_eq_static_cast_int",
                  int{ v } == static_cast<int>(v) && static_cast<int>(v) == 12);
        }
        // double source.
        {
            auto v = read_back<double>("D", 99.0);
            const float implicit_f = v;
            check("f64_implicit_eq_static_cast_float",
                  implicit_f == static_cast<float>(v) && static_cast<float>(v) == 99.0f);
            check("f64_implicit_eq_static_cast_int64",
                  std::int64_t{ v } == static_cast<std::int64_t>(v)
                  && static_cast<std::int64_t>(v) == 99);
        }
        // bool source.
        {
            auto v = read_back<std::uint8_t>("Z", std::uint8_t{ 1 });
            const int implicit_i = v;
            check("bool_implicit_eq_static_cast_int",
                  implicit_i == static_cast<int>(v) && static_cast<int>(v) == 1);
        }
        // uint32 (reference) source — pure arithmetic, no decode.
        {
            auto v = read_back<std::uint32_t>("Ljava/lang/Object;", std::uint32_t{ 305419896u });
            const std::int64_t implicit_l = v;
            check("u32_implicit_eq_static_cast_int64",
                  implicit_l == static_cast<std::int64_t>(v)
                  && static_cast<std::int64_t>(v) == 305419896LL);
            const std::uint32_t implicit_u = v;
            check("u32_implicit_eq_static_cast_uint32",
                  implicit_u == static_cast<std::uint32_t>(v)
                  && static_cast<std::uint32_t>(v) == 305419896u);
        }
    }

    // ---------------------------------------------------------------------
    // 28. void* is the ONLY legitimate pointer target.  static_cast<void*>
    //     and the implicit conversion both route a uint32 alternative through
    //     decode_oop_pointer (zero OOP -> nullptr, JVM-free) and yield
    //     nullptr for every non-uint32 alternative.  The two spellings agree.
    // ---------------------------------------------------------------------
    {
        std::array<std::uint8_t, 16> storage{};
        storage.fill(std::uint8_t{ 0x00 });
        vmhook::field_proxy ref{ storage.data(), "Ljava/lang/String;", false };
        auto vr = ref.get();
        void* implicit_p = vr;
        check("void_ptr_implicit_eq_static_cast_zero_oop",
              implicit_p == static_cast<void*>(vr) && static_cast<void*>(vr) == nullptr);

        // Non-uint32 alternatives -> nullptr through the void* "else" arm.
        check("void_ptr_null_for_int_alt",
              static_cast<void*>(read_back<std::int32_t>("I", 1)) == nullptr);
        check("void_ptr_null_for_double_alt",
              static_cast<void*>(read_back<double>("D", 1.0)) == nullptr);
        check("void_ptr_null_for_bool_alt",
              static_cast<void*>(read_back<std::uint8_t>("Z", std::uint8_t{ 1 })) == nullptr);
        check("void_ptr_null_for_char_alt",
              static_cast<void*>(read_back<std::uint16_t>("C", std::uint16_t{ 65 })) == nullptr);
    }

    // ---------------------------------------------------------------------
    // 29. Additional narrowing / sign edges through static_cast not already
    //     pinned: uint16 boundaries, int64 just-past-2^53, and int64 ->
    //     float rounding direction.  Pure arithmetic.
    // ---------------------------------------------------------------------
    {
        auto v0 = read_back<std::uint16_t>("C", std::uint16_t{ 0 });
        check("C_zero_casts_to_int_0", static_cast<int>(v0) == 0);
        check("C_zero_casts_to_uint32_0", static_cast<std::uint32_t>(v0) == 0u);

        auto vmax = read_back<std::uint16_t>("C", std::uint16_t{ 0xFFFF });
        check("C_max_casts_to_uint32_65535", static_cast<std::uint32_t>(vmax) == 65535u);
        check("C_max_no_sign_extension_in_int64",
              static_cast<std::int64_t>(vmax) == 65535);

        // int16 max/min through static_cast widen with sign.
        auto smax = read_back<std::int16_t>("S", std::int16_t{ 32767 });
        check("S_max_widens_to_int_32767", static_cast<int>(smax) == 32767);
        auto smin = read_back<std::int16_t>("S", std::int16_t{ -32768 });
        check("S_min_widens_to_int_minus_32768", static_cast<int>(smin) == -32768);

        // int64 value one past exact double integer range (2^53 + 1) cast to
        // double: cannot be represented, rounds to 2^53.  Pin the documented
        // static_cast rounding (round-to-nearest-even on every IEEE target).
        const std::int64_t past_exact{ 9007199254740993LL };  // 2^53 + 1
        auto vp = read_back<std::int64_t>("J", past_exact);
        check("J_2pow53_plus_1_rounds_to_2pow53_as_double",
              static_cast<double>(vp) == 9007199254740992.0);

        // uint32 0x80000000 -> int is the minimum int (bit reinterpret), ->
        // int64 zero-extends.  Pure arithmetic, no decode.
        auto vh = read_back<std::uint32_t>("[I", std::uint32_t{ 0x80000000u });
        check("u32_high_bit_casts_to_int_min",
              static_cast<int>(vh) == (-2147483647 - 1));
        check("u32_high_bit_casts_to_int64_2147483648",
              static_cast<std::int64_t>(vh) == 2147483648LL);
    }

    // ---------------------------------------------------------------------
    // 30. float / double special values survive the variant bit-exactly.
    //     get() does a raw memcpy and cast_for_variant<float>/<double> is an
    //     identity static_cast, so +0.0/-0.0 stay distinct, +-inf round-trip,
    //     NaN stays NaN, and a subnormal keeps every bit.  NaN != NaN, so we
    //     compare BITS via memcmp (libc++-safe: no <charconv>, no float
    //     from_chars/to_chars).
    // ---------------------------------------------------------------------
    {
        // +0.0 vs -0.0: distinct bit patterns must both survive.
        const double pos_zero{ 0.0 };
        const double neg_zero{ -0.0 };
        const double got_pos{ static_cast<double>(read_back<double>("D", pos_zero)) };
        const double got_neg{ static_cast<double>(read_back<double>("D", neg_zero)) };
        std::uint64_t bits_pos{}, bits_neg{};
        std::memcpy(&bits_pos, &got_pos, sizeof(bits_pos));
        std::memcpy(&bits_neg, &got_neg, sizeof(bits_neg));
        std::uint64_t ref_pos{}, ref_neg{};
        std::memcpy(&ref_pos, &pos_zero, sizeof(ref_pos));
        std::memcpy(&ref_neg, &neg_zero, sizeof(ref_neg));
        check("D_pos_zero_bits_preserved", bits_pos == ref_pos);
        check("D_neg_zero_bits_preserved", bits_neg == ref_neg);
        check("D_pos_and_neg_zero_have_distinct_bits", bits_pos != bits_neg);
        check("D_pos_zero_equals_neg_zero_numerically", got_pos == got_neg); // 0.0 == -0.0

        // +inf / -inf via bit construction (no <limits> dependence for the
        // pattern itself; we build the IEEE-754 double bit patterns directly).
        std::uint64_t inf_bits{ 0x7FF0000000000000ULL };
        double pos_inf{};
        std::memcpy(&pos_inf, &inf_bits, sizeof(pos_inf));
        auto vi = read_back<double>("D", pos_inf);
        double got_inf{ static_cast<double>(vi) };
        std::uint64_t got_inf_bits{};
        std::memcpy(&got_inf_bits, &got_inf, sizeof(got_inf_bits));
        check("D_pos_inf_bits_preserved", got_inf_bits == inf_bits);

        std::uint64_t ninf_bits{ 0xFFF0000000000000ULL };
        double neg_inf{};
        std::memcpy(&neg_inf, &ninf_bits, sizeof(neg_inf));
        double got_ninf{ static_cast<double>(read_back<double>("D", neg_inf)) };
        std::uint64_t got_ninf_bits{};
        std::memcpy(&got_ninf_bits, &got_ninf, sizeof(got_ninf_bits));
        check("D_neg_inf_bits_preserved", got_ninf_bits == ninf_bits);

        // Quiet NaN: bits must survive exactly (NaN != NaN so compare bits).
        std::uint64_t nan_bits{ 0x7FF8000000000000ULL };
        double the_nan{};
        std::memcpy(&the_nan, &nan_bits, sizeof(the_nan));
        double got_nan{ static_cast<double>(read_back<double>("D", the_nan)) };
        std::uint64_t got_nan_bits{};
        std::memcpy(&got_nan_bits, &got_nan, sizeof(got_nan_bits));
        check("D_quiet_nan_bits_preserved", got_nan_bits == nan_bits);

        // Smallest positive subnormal double (bit pattern 0x1): every bit kept.
        std::uint64_t subnormal_bits{ 0x0000000000000001ULL };
        double subnormal{};
        std::memcpy(&subnormal, &subnormal_bits, sizeof(subnormal));
        double got_sub{ static_cast<double>(read_back<double>("D", subnormal)) };
        std::uint64_t got_sub_bits{};
        std::memcpy(&got_sub_bits, &got_sub, sizeof(got_sub_bits));
        check("D_subnormal_bits_preserved", got_sub_bits == subnormal_bits);

        // float special values: +inf and NaN via 32-bit patterns.
        std::uint32_t f_inf_bits{ 0x7F800000u };
        float f_inf{};
        std::memcpy(&f_inf, &f_inf_bits, sizeof(f_inf));
        float got_finf{ static_cast<float>(read_back<float>("F", f_inf)) };
        std::uint32_t got_finf_bits{};
        std::memcpy(&got_finf_bits, &got_finf, sizeof(got_finf_bits));
        check("F_pos_inf_bits_preserved", got_finf_bits == f_inf_bits);

        std::uint32_t f_nan_bits{ 0x7FC00000u };
        float f_nan{};
        std::memcpy(&f_nan, &f_nan_bits, sizeof(f_nan));
        float got_fnan{ static_cast<float>(read_back<float>("F", f_nan)) };
        std::uint32_t got_fnan_bits{};
        std::memcpy(&got_fnan_bits, &got_fnan, sizeof(got_fnan_bits));
        check("F_quiet_nan_bits_preserved", got_fnan_bits == f_nan_bits);

        std::uint32_t f_negzero_bits{ 0x80000000u };
        float f_negzero{};
        std::memcpy(&f_negzero, &f_negzero_bits, sizeof(f_negzero));
        float got_fnz{ static_cast<float>(read_back<float>("F", f_negzero)) };
        std::uint32_t got_fnz_bits{};
        std::memcpy(&got_fnz_bits, &got_fnz, sizeof(got_fnz_bits));
        check("F_neg_zero_bits_preserved", got_fnz_bits == f_negzero_bits);
    }

    // ---------------------------------------------------------------------
    // 31. char ("C") full BMP range: U+0000, U+007F (ASCII edge), U+0080
    //     (Latin-1 supplement start), U+07FF, U+FFFF (max code unit) all
    //     round-trip through the uint16 alternative with NO sign extension
    //     (the source is unsigned), via both std::get and static_cast<int>.
    // ---------------------------------------------------------------------
    {
        const std::uint16_t code_units[]{ 0x0000, 0x007F, 0x0080, 0x07FF, 0x4E2D, 0xFFFF };
        bool all_ok{ true };
        for (const std::uint16_t cu : code_units)
        {
            auto v = read_back<std::uint16_t>("C", cu);
            if (v.data.index() != idx::k_u16) { all_ok = false; }
            if (std::get<std::uint16_t>(v.data) != cu) { all_ok = false; }
            // No sign extension: int value equals the unsigned code unit.
            if (static_cast<int>(v) != static_cast<int>(cu)) { all_ok = false; }
            if (static_cast<char16_t>(v) != static_cast<char16_t>(cu)) { all_ok = false; }
        }
        check("C_full_bmp_range_round_trips_no_sign_extension", all_ok);
    }

    // ---------------------------------------------------------------------
    // 32. bool ("Z") non-canonical backing byte (documented get() gap): a
    //     0x02 byte is not a valid bool object representation, so OBSERVING
    //     its value would be UB.  We assert only that get() (a) does not
    //     crash and (b) still selects the bool alternative (index check, no
    //     value read).  This pins the dispatch without depending on the
    //     trap representation — libc++-safe (no bool trap read).
    // ---------------------------------------------------------------------
    {
        std::array<std::uint8_t, 16> storage{};
        storage.fill(std::uint8_t{ 0xAB });
        storage[0] = std::uint8_t{ 0x02 };  // non-canonical bool byte
        vmhook::field_proxy proxy{ storage.data(), "Z", false };
        auto v = proxy.get();
        check("Z_non_canonical_byte_selects_bool_alternative", v.data.index() == idx::k_bool);
    }

    // ---------------------------------------------------------------------
    // 33. Direct value_t aggregate construction (no proxy / no get()): the
    //     conversion operator and static_cast work identically on a value_t
    //     built straight from an alternative, including monostate-free
    //     defaults.  This isolates the operator from the get() dispatch and
    //     mirrors test_method_proxy_value_t's direct-construction style.
    //     value_t{ alt } leaves signature default "" — fine for numeric
    //     casts (signature is only consulted by the unique_ptr/vector/string
    //     arms, which a numeric alt never enters).
    // ---------------------------------------------------------------------
    {
        // Numeric alternatives constructed directly.
        check("direct_int32_static_cast", static_cast<int>(value_t{ std::int32_t{ 77 } }) == 77);
        check("direct_int32_implicit", []{ int i = value_t{ std::int32_t{ 77 } }; return i; }() == 77);
        check("direct_int64_static_cast",
              static_cast<std::int64_t>(value_t{ std::int64_t{ -9000000000LL } }) == -9000000000LL);
        check("direct_double_static_cast",
              static_cast<double>(value_t{ double{ 1.25 } }) == 1.25);
        check("direct_float_static_cast",
              static_cast<float>(value_t{ float{ -2.5f } }) == -2.5f);
        check("direct_bool_true_static_cast", static_cast<bool>(value_t{ true }) == true);
        check("direct_bool_false_static_cast", static_cast<bool>(value_t{ false }) == false);
        check("direct_uint16_static_cast",
              static_cast<std::uint16_t>(value_t{ std::uint16_t{ 0xBEEF } }) == std::uint16_t{ 0xBEEF });
        check("direct_int8_static_cast",
              static_cast<std::int8_t>(value_t{ std::int8_t{ -7 } }) == std::int8_t{ -7 });
        check("direct_int16_static_cast",
              static_cast<std::int16_t>(value_t{ std::int16_t{ 4242 } }) == std::int16_t{ 4242 });

        // uint32 alternative built directly, with a default ("") signature:
        // numeric casts are pure; string/void* arms behave as zero-OOP.
        value_t ref_default{ std::uint32_t{ 0u } };
        check("direct_uint32_is_reference_true", ref_default.is_reference() == true);
        check("direct_uint32_zero_as_string_empty", ref_default.as_string().empty());
        check("direct_uint32_zero_to_void_ptr_null", static_cast<void*>(ref_default) == nullptr);
        check("direct_uint32_value_static_cast",
              static_cast<std::uint32_t>(value_t{ std::uint32_t{ 123u } }) == 123u);

        // unique_ptr<W> on a directly-built uint32 alt with EMPTY signature ->
        // FLAW-B guard (signature.empty()) -> nullptr, no decode.
        check("direct_uint32_empty_sig_unique_ptr_null",
              static_cast<std::unique_ptr<W>>(value_t{ std::uint32_t{ 0u } }) == nullptr);

        // Explicit signature on a directly-built value_t flows into the
        // unique_ptr signature guard: "[I" (array) -> nullptr.
        value_t arr_alt{ std::uint32_t{ 0u }, std::string{ "[I" } };
        check("direct_value_t_carries_signature", arr_alt.signature == "[I");
        check("direct_uint32_array_sig_unique_ptr_null",
              static_cast<std::unique_ptr<W>>(arr_alt) == nullptr);
    }

    // ---------------------------------------------------------------------
    // 34. GC-safety / os::safe_read fallback: field_proxy::get() reads the
    //     field bytes through vmhook::os::safe_read (ReadProcessMemory on
    //     Windows / process_vm_readv + signal-guarded fallback on POSIX), so a
    //     field_pointer aimed at an UNMAPPED address must NOT fault — get()
    //     recovers with the same zero/empty result the null-pointer guard
    //     returns.  This is the no-JVM stand-in for the relocating-GC race
    //     where a static field's class mirror moves between resolve and read,
    //     leaving the cached field_pointer pointing at a stale/unmapped page.
    //     Before the fix get() raw-memcpy'd that pointer and crashed.
    //
    //     0xDEAD0000 is reliably unmapped in this process on every supported
    //     platform; safe_read returns false there (never faults).  We first
    //     confirm safe_read itself rejects the address, then drive get() for
    //     every signature and assert the recovered value collapses to zero.
    // ---------------------------------------------------------------------
    {
        void* const unmapped{ reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xDEAD0000ULL)) };

        // Precondition: the address really is unreadable (so the cases below
        // exercise the recovery arm, not an accidentally-mapped page).
        std::uint64_t scratch{ 0 };
        const bool readable{ vmhook::os::safe_read(&scratch, unmapped, sizeof(scratch)) };
        check("safe_read_rejects_unmapped_address", readable == false);

        // Each primitive signature: get() must return without faulting and the
        // value must read back as zero (failed safe_read leaves the zero-init).
        {
            vmhook::field_proxy p{ unmapped, "Z", false };
            check("unmapped_Z_recovers_false", static_cast<bool>(p.get()) == false);
        }
        {
            vmhook::field_proxy p{ unmapped, "B", false };
            auto v = p.get();
            check("unmapped_B_selects_int8_alternative", v.data.index() == idx::k_i8);
            check("unmapped_B_recovers_zero", static_cast<std::int8_t>(v) == 0);
        }
        {
            vmhook::field_proxy p{ unmapped, "S", false };
            check("unmapped_S_recovers_zero", static_cast<std::int16_t>(p.get()) == 0);
        }
        {
            vmhook::field_proxy p{ unmapped, "I", false };
            auto v = p.get();
            check("unmapped_I_selects_int32_alternative", v.data.index() == idx::k_i32);
            check("unmapped_I_recovers_zero", static_cast<std::int32_t>(v) == 0);
        }
        {
            vmhook::field_proxy p{ unmapped, "J", false };
            check("unmapped_J_recovers_zero", static_cast<std::int64_t>(p.get()) == 0);
        }
        {
            vmhook::field_proxy p{ unmapped, "F", false };
            check("unmapped_F_recovers_zero", static_cast<float>(p.get()) == 0.0f);
        }
        {
            vmhook::field_proxy p{ unmapped, "D", false };
            check("unmapped_D_recovers_zero", static_cast<double>(p.get()) == 0.0);
        }
        {
            vmhook::field_proxy p{ unmapped, "C", false };
            auto v = p.get();
            check("unmapped_C_selects_uint16_alternative", v.data.index() == idx::k_u16);
            check("unmapped_C_recovers_zero", static_cast<std::uint16_t>(v) == 0u);
        }
        {
            // Reference field over an unmapped slot: get() recovers a zero
            // compressed OOP (the uint32 alternative), which routes to a null
            // void* and an empty string — no fault, no wild decode.
            vmhook::field_proxy p{ unmapped, "Ljava/lang/String;", false };
            auto v = p.get();
            check("unmapped_ref_selects_uint32_alternative", v.data.index() == idx::k_u32);
            check("unmapped_ref_recovers_zero_oop", std::get<std::uint32_t>(v.data) == 0u);
            check("unmapped_ref_routes_to_null_void_ptr", static_cast<void*>(v) == nullptr);
            check("unmapped_ref_as_string_empty", v.as_string().empty());
        }
        {
            vmhook::field_proxy p{ unmapped, "[I", false };
            check("unmapped_array_recovers_zero_oop",
                  std::get<std::uint32_t>(p.get().data) == 0u);
        }

        // get_compressed_oop() over the same unmapped reference slot must also
        // route through safe_read and recover 0 rather than faulting.
        {
            vmhook::field_proxy p{ unmapped, "Ljava/lang/String;", false };
            check("unmapped_get_compressed_oop_recovers_zero",
                  p.get_compressed_oop() == 0u);
        }

        // A static-flagged proxy with NO mirror_klass (legacy 3-arg ctor) takes
        // the same recovery path: get() never re-resolves (mirror_klass is
        // null) and safe_read protects the stale field_pointer.
        {
            vmhook::field_proxy p{ unmapped, "I", true };
            check("unmapped_static_legacy_recovers_zero",
                  static_cast<std::int32_t>(p.get()) == 0);
        }
    }

    // ---------------------------------------------------------------------
    // 35. os::safe_read_fast — the cheap-path fault-safe read that
    //     field_proxy::get()/get_compressed_oop() now use on the hot path
    //     (PERF #21).  It must be a DROP-IN for os::safe_read: identical
    //     all-or-nothing bool contract, identical bytes on success, and it
    //     must NEVER fault on a bad pointer.
    //
    //     SAFE BY CONSTRUCTION across every toolchain: safe_read_fast only
    //     takes a cheap guarded path where that guard is proven (SEH __try
    //     on real MSVC cl.exe); on EVERY other config — clang-cl /
    //     clang-on-windows, MinGW, Linux, Android, macOS, iOS — it is just a
    //     call to os::safe_read.  So this fault-injection section can never
    //     reach an unguarded faulting read on ANY platform: the worst case is
    //     it exercises os::safe_read, which is itself fault-safe.  These cases
    //     run with no JVM — they drive the read primitive directly over
    //     buffers we own plus the same 0xDEAD0000 unmapped sentinel section 34
    //     uses.
    // ---------------------------------------------------------------------
    {
        // Input-guard parity with safe_read: null dst / null src / zero size
        // all return false on BOTH primitives (no fault, no copy).
        {
            std::uint64_t scratch{ 0 };
            std::uint64_t src{ 0x1122334455667788ULL };
            check("fast_null_dst_false",
                  vmhook::os::safe_read_fast(nullptr, &src, sizeof(src)) == false);
            check("fast_null_src_false",
                  vmhook::os::safe_read_fast(&scratch, nullptr, sizeof(scratch)) == false);
            check("fast_zero_size_false",
                  vmhook::os::safe_read_fast(&scratch, &src, 0) == false);
        }

        // Cheap-path SUCCESS on a valid buffer: every width field_proxy reads
        // (1,2,4,8 bytes) copies the exact bytes and returns true, and the
        // result is byte-identical to os::safe_read over the same source.
        {
            alignas(std::uint64_t) const std::uint8_t source[8]{
                0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x23, 0x45, 0x67 };
            const std::size_t widths[]{ 1, 2, 4, 8 };
            for (const std::size_t w : widths)
            {
                std::uint8_t via_fast[8]{};
                std::uint8_t via_slow[8]{};
                const bool ok_fast{ vmhook::os::safe_read_fast(via_fast, source, w) };
                const bool ok_slow{ vmhook::os::safe_read(via_slow, source, w) };
                check("fast_valid_returns_true", ok_fast == true);
                check("fast_matches_safe_read_bool", ok_fast == ok_slow);
                check("fast_copies_exact_bytes", std::memcmp(via_fast, source, w) == 0);
                check("fast_matches_safe_read_bytes",
                      std::memcmp(via_fast, via_slow, w) == 0);
            }
        }

        // Cheap-path FAULT recovery: an unmapped source must NOT fault — on
        // MSVC the SEH guard traps the AV and falls back to safe_read; on every
        // other config the call IS safe_read.  Either way it reports false
        // (same as safe_read).  This is the load-bearing fault-safety case: if
        // any config reached an unguarded read here, it would crash the process.
        {
            void* const unmapped{ reinterpret_cast<void*>(
                static_cast<std::uintptr_t>(0xDEAD0000ULL)) };
            const std::size_t widths[]{ 1, 2, 4, 8 };
            for (const std::size_t w : widths)
            {
                std::uint64_t dst{ 0 };
                const bool ok_fast{ vmhook::os::safe_read_fast(&dst, unmapped, w) };
                const bool ok_slow{ vmhook::os::safe_read(&dst, unmapped, w) };
                check("fast_unmapped_returns_false", ok_fast == false);
                check("fast_unmapped_matches_safe_read", ok_fast == ok_slow);
            }
        }

        // Stress the fault path: many consecutive faulting reads followed by a
        // valid read must all behave (no wedged SEH filter, no fault).  Closest
        // no-JVM proxy that the guarded path stays healthy across repeated trips.
        {
            void* const unmapped{ reinterpret_cast<void*>(
                static_cast<std::uintptr_t>(0xDEAD0000ULL)) };
            bool all_false{ true };
            for (int i = 0; i < 256; ++i)
            {
                std::uint32_t dst{ 0xFFFFFFFFu };
                if (vmhook::os::safe_read_fast(&dst, unmapped, sizeof(dst)))
                {
                    all_false = false;
                }
            }
            check("fast_repeated_faults_all_false", all_false);

            const std::uint32_t source{ 0xCAFED00Du };
            std::uint32_t dst{ 0 };
            check("fast_valid_after_faults_true",
                  vmhook::os::safe_read_fast(&dst, &source, sizeof(dst)) == true);
            check("fast_valid_after_faults_bytes", dst == 0xCAFED00Du);
        }
    }

    // ---------------------------------------------------------------------
    // 36. RAW-ECHO SAFETY (robustness #1).  The 3-arg ctor is a documented
    //     escape hatch that stores ANY caller pointer with no validation
    //     (raw_address() echoes it verbatim).  get()/get_compressed_oop() now
    //     gate the DEREF behind vmhook::hotspot::is_valid_pointer, so a proxy
    //     built over a genuinely-BOGUS address returns the SAME zero/empty default
    //     the null guard yields, WITHOUT dereferencing the wild pointer.  This is
    //     the load-bearing safety case: before the gate, get() handed the address
    //     straight to the read, which on a config whose fault guard is weakest
    //     (clang-cl SEH does not trap AVs; iOS safe_read raw-memcpys) is an access
    //     violation.  No JVM: pure pointer-classification + the null-guard default;
    //     no live oop is decoded.
    //
    //     The gate accepts an address when is_valid_pointer(addr) OR
    //     is_valid_pointer(addr & ~1) holds — the second arm keeps a LEGITIMATE
    //     1-byte (byte/boolean) field at an ODD offset readable (is_valid_pointer
    //     demands 2-byte alignment, which such a field does not satisfy), while a
    //     truly-wild pointer fails BOTH.  Outcome-wise every case below collapses
    //     to the documented default: out-of-range / kernel addresses are rejected
    //     by the gate; an address that PASSES the gate but is unmapped (e.g. the
    //     poison pattern, whose even neighbour is in-range) is caught one layer
    //     down by safe_read_fast — both yield the zero/empty default, no crash.
    // ---------------------------------------------------------------------
    {
        // A spread of addresses that all drive get() to the safe default:
        //   0x1 / 0x3      -> below user_address_floor (0xFFFF): rejected by the
        //                     gate (both the address AND its even base are out of
        //                     range), so get() never reads them.
        //   0xDEADBEEF     -> a poison pattern; is_valid_pointer(addr) is false but
        //                     is_valid_pointer(addr & ~1)==0xDEADBEEE is in range,
        //                     so the gate lets it through and safe_read_fast (the
        //                     backstop) recovers 0 from the unmapped page.  This
        //                     mirrors section 34's 0xDEAD0000 safe-read recovery.
        //   0xFFFF...FFFF  -> kernel-space, above user_address_ceiling: rejected by
        //                     the gate.
        struct BogusAddr { const char* tag; std::uintptr_t value; };
        const BogusAddr bogus_addrs[]{
            { "one",          std::uintptr_t{ 0x1 } },
            { "three",        std::uintptr_t{ 0x3 } },
            { "poison",       std::uintptr_t{ 0xDEADBEEFull } },
            { "kernel_space", ~std::uintptr_t{ 0 } },
        };

        // Sanity: every address above fails is_valid_pointer itself (the building
        // block of the gate), so the get() cases below exercise the safe path and
        // not an accidentally-valid pointer.
        for (const BogusAddr& b : bogus_addrs)
        {
            check((std::string{ "bogus_addr_fails_is_valid_pointer_" } + b.tag).c_str(),
                  vmhook::hotspot::is_valid_pointer(
                      reinterpret_cast<void*>(b.value)) == false);
        }

        // get() over each bogus address returns the documented default for the
        // signature: the int32 alternative, value 0, signature preserved — the
        // identical result the null-pointer guard produces.  No AV, no crash.
        for (const BogusAddr& b : bogus_addrs)
        {
            void* const addr{ reinterpret_cast<void*>(b.value) };

            vmhook::field_proxy pi{ addr, "I", false };
            auto vi = pi.get();
            check((std::string{ "bogus_get_int_is_int32_alt_" } + b.tag).c_str(),
                  vi.data.index() == idx::k_i32);
            check((std::string{ "bogus_get_int_is_zero_" } + b.tag).c_str(),
                  static_cast<std::int32_t>(vi) == 0);
            check((std::string{ "bogus_get_int_signature_preserved_" } + b.tag).c_str(),
                  vi.signature == "I");

            // Wider primitive: still the zero default, never an 8-byte wild read.
            vmhook::field_proxy pj{ addr, "J", true };
            check((std::string{ "bogus_get_long_is_zero_" } + b.tag).c_str(),
                  static_cast<std::int64_t>(pj.get()) == 0);

            // Reference field: get() yields the int32 zero default (NOT a uint32
            // OOP read), so it routes to a null void* and an empty string with no
            // wild decode; get_compressed_oop() likewise recovers 0.
            vmhook::field_proxy pr{ addr, "Ljava/lang/String;", true };
            auto vr = pr.get();
            check((std::string{ "bogus_get_ref_routes_to_null_void_ptr_" } + b.tag).c_str(),
                  static_cast<void*>(vr) == nullptr);
            check((std::string{ "bogus_get_ref_as_string_empty_" } + b.tag).c_str(),
                  vr.as_string().empty());
            check((std::string{ "bogus_get_compressed_oop_is_zero_" } + b.tag).c_str(),
                  pr.get_compressed_oop() == 0u);
        }

        // raw_address() is UNCHANGED by the fix: it still echoes the bogus
        // pointer verbatim (the gate is on the DEREF in get(), not on the ctor
        // or the accessor).  This pins that the escape hatch remains an escape
        // hatch — only the *read* is now safe-by-default.
        {
            void* const addr{ reinterpret_cast<void*>(std::uintptr_t{ 0x1 }) };
            vmhook::field_proxy p{ addr, "Ljava/lang/String;", true };
            check("bogus_raw_address_still_echoes_pointer",
                  p.raw_address() == addr);
        }
    }

    // ---------------------------------------------------------------------
    // 37. ALTERNATING-BIT boundary patterns (0xAA.../0x55...) for EVERY width.
    //     The MEGA-GOAL boundary list calls out alternating-bits explicitly; the
    //     earlier sections pin MIN/MAX/0/-1 but never the 0b1010.../0b0101...
    //     patterns that catch a byte-swap, a wrong-width memcpy, or a sign-bit
    //     handling slip in get()'s per-descriptor read.  Each value is planted
    //     little-endian into the buffer by read_back and must come back BIT-EXACT
    //     through the matching variant alternative.  Pure logic, no JVM.
    // ---------------------------------------------------------------------
    {
        // int8 "B": 0xAA == -86, 0x55 == 85 (signed alternative).
        {
            auto va = read_back<std::int8_t>("B", static_cast<std::int8_t>(0xAA));
            check("B_0xAA_selects_int8_alt", va.data.index() == idx::k_i8);
            check("B_0xAA_is_minus_86", std::get<std::int8_t>(va.data) == static_cast<std::int8_t>(0xAA));
            check("B_0xAA_value_is_minus_86_numeric", std::get<std::int8_t>(va.data) == std::int8_t{ -86 });
            auto v5 = read_back<std::int8_t>("B", std::int8_t{ 0x55 });
            check("B_0x55_is_85", std::get<std::int8_t>(v5.data) == std::int8_t{ 85 });
        }
        // int16 "S": 0xAAAA / 0x5555.
        {
            auto va = read_back<std::int16_t>("S", static_cast<std::int16_t>(0xAAAA));
            check("S_0xAAAA_selects_int16_alt", va.data.index() == idx::k_i16);
            check("S_0xAAAA_bit_exact",
                  std::get<std::int16_t>(va.data) == static_cast<std::int16_t>(0xAAAA));
            auto v5 = read_back<std::int16_t>("S", std::int16_t{ 0x5555 });
            check("S_0x5555_is_21845", std::get<std::int16_t>(v5.data) == std::int16_t{ 0x5555 });
        }
        // uint16 "C": 0xAAAA / 0x5555 (unsigned — exact, no sign extension).
        {
            auto va = read_back<std::uint16_t>("C", std::uint16_t{ 0xAAAA });
            check("C_0xAAAA_selects_uint16_alt", va.data.index() == idx::k_u16);
            check("C_0xAAAA_bit_exact", std::get<std::uint16_t>(va.data) == std::uint16_t{ 0xAAAA });
            check("C_0xAAAA_to_int_no_sign_extension", static_cast<int>(va) == 0xAAAA);
            auto v5 = read_back<std::uint16_t>("C", std::uint16_t{ 0x5555 });
            check("C_0x5555_bit_exact", std::get<std::uint16_t>(v5.data) == std::uint16_t{ 0x5555 });
        }
        // int32 "I": 0xAAAAAAAA / 0x55555555.
        {
            auto va = read_back<std::int32_t>("I", static_cast<std::int32_t>(0xAAAAAAAAu));
            check("I_0xAAAAAAAA_selects_int32_alt", va.data.index() == idx::k_i32);
            check("I_0xAAAAAAAA_bit_exact",
                  std::get<std::int32_t>(va.data) == static_cast<std::int32_t>(0xAAAAAAAAu));
            auto v5 = read_back<std::int32_t>("I", std::int32_t{ 0x55555555 });
            check("I_0x55555555_is_positive",
                  std::get<std::int32_t>(v5.data) == std::int32_t{ 0x55555555 });
        }
        // int64 "J": 0xAAAAAAAAAAAAAAAA / 0x5555555555555555.
        {
            auto va = read_back<std::int64_t>("J", static_cast<std::int64_t>(0xAAAAAAAAAAAAAAAAull));
            check("J_0xAAAA_pattern_selects_int64_alt", va.data.index() == idx::k_i64);
            check("J_0xAAAA_pattern_bit_exact",
                  std::get<std::int64_t>(va.data) == static_cast<std::int64_t>(0xAAAAAAAAAAAAAAAAull));
            auto v5 = read_back<std::int64_t>("J", static_cast<std::int64_t>(0x5555555555555555ull));
            check("J_0x5555_pattern_bit_exact",
                  std::get<std::int64_t>(v5.data) == static_cast<std::int64_t>(0x5555555555555555ull));
        }
        // uint32 (reference) alt: 0xAAAAAAAA / 0x55555555 round-trip exactly.
        {
            auto va = read_back<std::uint32_t>("Ljava/lang/Object;", std::uint32_t{ 0xAAAAAAAAu });
            check("u32_0xAAAAAAAA_selects_uint32_alt", va.data.index() == idx::k_u32);
            check("u32_0xAAAAAAAA_bit_exact", std::get<std::uint32_t>(va.data) == 0xAAAAAAAAu);
            auto v5 = read_back<std::uint32_t>("[I", std::uint32_t{ 0x55555555u });
            check("u32_0x55555555_bit_exact", std::get<std::uint32_t>(v5.data) == 0x55555555u);
        }
        // float "F" / double "D": alternating-bit PATTERNS as raw bits — get()'s
        // memcpy must preserve them verbatim (regardless of what value they denote).
        {
            std::uint32_t f_bits{ 0xAAAAAAAAu };
            float f_in{};
            std::memcpy(&f_in, &f_bits, sizeof(f_in));
            float f_got{ static_cast<float>(read_back<float>("F", f_in)) };
            std::uint32_t f_got_bits{};
            std::memcpy(&f_got_bits, &f_got, sizeof(f_got_bits));
            check("F_alternating_bits_preserved", f_got_bits == f_bits);

            std::uint64_t d_bits{ 0x5555555555555555ull };
            double d_in{};
            std::memcpy(&d_in, &d_bits, sizeof(d_in));
            double d_got{ static_cast<double>(read_back<double>("D", d_in)) };
            std::uint64_t d_got_bits{};
            std::memcpy(&d_got_bits, &d_got, sizeof(d_got_bits));
            check("D_alternating_bits_preserved", d_got_bits == d_bits);
        }
    }

    // ---------------------------------------------------------------------
    // 38. NARROW-READ OVER-READ SYMMETRY on get() itself.  read_back fills all
    //     16 buffer bytes with the 0xAB sentinel and then overwrites only the
    //     leading sizeof(value) bytes with the planted value, so any byte get()
    //     reads PAST the field width would surface as 0xAB contamination in the
    //     decoded value.  Section 11 only checked over-read for
    //     get_compressed_oop(); here we pin that "B" reads EXACTLY 1 byte, "S"/"C"
    //     EXACTLY 2, and "I"/"F" EXACTLY 4 — never picking up the trailing
    //     sentinel.  We plant a value whose own bytes are all-zero in the low
    //     positions so a too-wide read (pulling in 0xAB) would be unmistakable.
    // ---------------------------------------------------------------------
    {
        // "B": plant 0x00 in byte[0], leave 0xAB in [1..]; a 1-byte read yields 0.
        {
            std::array<std::uint8_t, 16> storage{};
            storage.fill(std::uint8_t{ 0xAB });
            storage[0] = std::uint8_t{ 0x00 };
            vmhook::field_proxy p{ storage.data(), "B", false };
            check("B_reads_exactly_one_byte_no_over_read",
                  std::get<std::int8_t>(p.get().data) == std::int8_t{ 0 });
        }
        // "S": plant 0x0000 in [0..1], 0xAB in [2..]; a 2-byte read yields 0,
        // a 4-byte over-read would yield 0xABAB0000 -> nonzero int16 truncation.
        {
            std::array<std::uint8_t, 16> storage{};
            storage.fill(std::uint8_t{ 0xAB });
            storage[0] = std::uint8_t{ 0x00 };
            storage[1] = std::uint8_t{ 0x00 };
            vmhook::field_proxy p{ storage.data(), "S", false };
            check("S_reads_exactly_two_bytes_no_over_read",
                  std::get<std::int16_t>(p.get().data) == std::int16_t{ 0 });
        }
        // "C": same 2-byte exactness for the unsigned char alternative.
        {
            std::array<std::uint8_t, 16> storage{};
            storage.fill(std::uint8_t{ 0xAB });
            storage[0] = std::uint8_t{ 0x00 };
            storage[1] = std::uint8_t{ 0x00 };
            vmhook::field_proxy p{ storage.data(), "C", false };
            check("C_reads_exactly_two_bytes_no_over_read",
                  std::get<std::uint16_t>(p.get().data) == std::uint16_t{ 0 });
        }
        // "I": plant 0x00000000 in [0..3], 0xAB in [4..]; a 4-byte read yields 0,
        // an 8-byte over-read (reading as if "J") would pull in 0xAB bytes.
        {
            std::array<std::uint8_t, 16> storage{};
            storage.fill(std::uint8_t{ 0xAB });
            for (int i = 0; i < 4; ++i) { storage[static_cast<std::size_t>(i)] = std::uint8_t{ 0x00 }; }
            vmhook::field_proxy p{ storage.data(), "I", false };
            check("I_reads_exactly_four_bytes_no_over_read",
                  std::get<std::int32_t>(p.get().data) == std::int32_t{ 0 });
        }
        // "F": 4-byte exactness for the float alternative (bit-level).
        {
            std::array<std::uint8_t, 16> storage{};
            storage.fill(std::uint8_t{ 0xAB });
            for (int i = 0; i < 4; ++i) { storage[static_cast<std::size_t>(i)] = std::uint8_t{ 0x00 }; }
            vmhook::field_proxy p{ storage.data(), "F", false };
            const float got{ std::get<float>(p.get().data) };
            std::uint32_t got_bits{};
            std::memcpy(&got_bits, &got, sizeof(got_bits));
            check("F_reads_exactly_four_bytes_no_over_read", got_bits == 0u);
        }
        // Positive symmetry: a NON-zero low value with sentinel tail still reads
        // back EXACTLY the planted value (the tail never bleeds in).  "S" planted
        // with 0x1234 over a 0xAB tail must read 0x1234, not 0xAB-contaminated.
        {
            std::array<std::uint8_t, 16> storage{};
            storage.fill(std::uint8_t{ 0xAB });
            const std::int16_t planted{ 0x1234 };
            std::memcpy(storage.data(), &planted, sizeof(planted));
            vmhook::field_proxy p{ storage.data(), "S", false };
            check("S_planted_value_not_tail_contaminated",
                  std::get<std::int16_t>(p.get().data) == std::int16_t{ 0x1234 });
            check("S_tail_sentinel_intact", storage[2] == 0xAB && storage[3] == 0xAB);
        }
    }

    // ---------------------------------------------------------------------
    // 39. value_t::to_vector<W>() no-JVM paths.  to_vector() first does
    //     static_cast<uint32_t>(*this) then decode_oop_pointer(that); for a ZERO
    //     compressed OOP decode_oop_pointer(0)==nullptr (no VMStruct access), and
    //     for a NON-uint32 alternative the static_cast yields 0 -> same null
    //     decode.  Both early-return an EMPTY vector before any heap walk, so this
    //     is fully JVM-free.  (A live, non-zero collection OOP needs a JVM and is
    //     out of scope — covered by example.cpp.)  This is the first exercise of
    //     to_vector() at all; previously it was only declared/defined, never run.
    // ---------------------------------------------------------------------
    {
        // Reference field with a ZERO compressed OOP -> empty wrapper vector.
        std::array<std::uint8_t, 16> storage{};
        storage.fill(std::uint8_t{ 0x00 });
        vmhook::field_proxy ref{ storage.data(), "Ljava/util/ArrayList;", false };
        check("to_vector_empty_for_zero_oop_collection",
              ref.get().to_vector<W>().empty());

        // Object-array signature "[Ljava/lang/Object;" with a ZERO oop -> the
        // direct-array branch also returns empty (decode_oop_pointer(0)==nullptr
        // is rejected before array_length is ever read).
        vmhook::field_proxy obj_arr{ storage.data(), "[Ljava/lang/Object;", false };
        check("to_vector_empty_for_zero_oop_object_array",
              obj_arr.get().to_vector<W>().empty());

        // Null-proxy reference -> int32 alternative -> static_cast<uint32>(*this)
        // is 0 -> empty vector, no decode.
        vmhook::field_proxy null_ref{ nullptr, "Ljava/util/ArrayList;", false };
        check("to_vector_empty_for_null_proxy",
              null_ref.get().to_vector<W>().empty());

        // A directly-built uint32 zero alternative with an ArrayList signature.
        value_t direct_zero{ std::uint32_t{ 0u }, std::string{ "Ljava/util/ArrayList;" } };
        check("to_vector_empty_for_direct_zero_uint32",
              direct_zero.to_vector<W>().empty());

        // A primitive (int) alternative routed through to_vector: static_cast<
        // uint32>(int-alt) is a pure arithmetic value; if it happens to be 0 the
        // decode is null, and a non-zero would still need a JVM to walk — here we
        // use a zero-valued int so the path is deterministically empty + JVM-free.
        check("to_vector_empty_for_zero_int_alternative",
              read_back<std::int32_t>("Ljava/util/ArrayList;", 0).to_vector<W>().empty());
    }

    // ---------------------------------------------------------------------
    // 40. value_t::to_entries<K,V>() no-JVM paths.  Same shape as to_vector:
    //     decode_oop_pointer(static_cast<uint32>(*this)) is the first step, so a
    //     ZERO compressed OOP and a non-uint32 alternative both early-return an
    //     EMPTY entries vector with no map walk.  First exercise of to_entries()
    //     at all.
    // ---------------------------------------------------------------------
    {
        std::array<std::uint8_t, 16> storage{};
        storage.fill(std::uint8_t{ 0x00 });
        vmhook::field_proxy map_ref{ storage.data(), "Ljava/util/HashMap;", false };
        check("to_entries_empty_for_zero_oop_map",
              (map_ref.get().to_entries<W, W>().empty()));

        vmhook::field_proxy null_map{ nullptr, "Ljava/util/HashMap;", false };
        check("to_entries_empty_for_null_proxy",
              (null_map.get().to_entries<W, W>().empty()));

        // Directly-built uint32 zero alternative -> empty entries.
        value_t direct_zero{ std::uint32_t{ 0u }, std::string{ "Ljava/util/HashMap;" } };
        check("to_entries_empty_for_direct_zero_uint32",
              (direct_zero.to_entries<W, W>().empty()));

        // Mixed key/value wrapper types compile and still early-return empty.
        check("to_entries_mixed_types_empty_for_zero_oop",
              (read_back<std::int32_t>("Ljava/util/HashMap;", 0).to_entries<W, W>().empty()));
    }

    // ---------------------------------------------------------------------
    // 41. operator target_type() fallback arm (cast_for_variant's final
    //     `return target_type{};`).  When a target is a value_t_convertible_target
    //     (so the operator is enabled) but NO variant alternative has a viable
    //     static_cast to it, cast_for_variant takes the `else` branch and returns
    //     a VALUE-INITIALISED target.  A class type with a defaulted constructor
    //     and no constructor/conversion from any arithmetic alternative is exactly
    //     such a target.  We assert the result equals a value-initialised instance
    //     for representative primitive alternatives.  Pure logic, no JVM.
    // ---------------------------------------------------------------------
    {
        // NonCastable (file scope) is a non-pointer class with a sentinel member
        // set by its default ctor.  It is value_t_convertible_target (not a pointer,
        // not nullptr_t), so the operator is enabled, but there is NO static_cast
        // from bool/int/double/... to it -> cast_for_variant's
        // `else { return target_type{}; }` arm fires and value-initialises it
        // (running the default ctor -> sentinel == 0xABCD).  The file-scope
        // static_asserts (next to W) pin that the scalar cast is genuinely
        // ill-formed, so the fallback is the only path the operator can take here.
        // (NonCastable lives at namespace scope, not inside main(), because a LOCAL
        // class named inside a requires-expression in a static_assert makes GCC
        // hard-error on the failed-overload candidate instead of reporting the
        // requirement as unsatisfied.)
        check("fallback_value_init_from_int_alt",
              static_cast<NonCastable>(read_back<std::int32_t>("I", 12345)).sentinel == 0xABCD);
        check("fallback_value_init_from_double_alt",
              static_cast<NonCastable>(read_back<double>("D", 3.14)).sentinel == 0xABCD);
        check("fallback_value_init_from_bool_alt",
              static_cast<NonCastable>(read_back<std::uint8_t>("Z", std::uint8_t{ 1 })).sentinel == 0xABCD);
        check("fallback_value_init_from_uint32_alt",
              static_cast<NonCastable>(
                  read_back<std::uint32_t>("Ljava/lang/Object;", std::uint32_t{ 7u })).sentinel == 0xABCD);
        // The implicit-conversion spelling takes the same fallback arm.
        NonCastable implicit_form = read_back<std::int64_t>("J", std::int64_t{ 99 });
        check("fallback_value_init_implicit_matches_static_cast",
              implicit_form.sentinel == 0xABCD);
    }

    // ---------------------------------------------------------------------
    // 42. float subnormal + float<->double widening/narrowing exactness.
    //     Section 30 pins double subnormal; here we add the FLOAT subnormal (bit
    //     pattern 0x00000001), confirm a finite float widens to double EXACTLY
    //     (every float is representable as double), and pin a double->float
    //     narrowing that rounds a value just past float's mantissa.  memcpy in
    //     get() + identity static_cast in cast_for_variant must keep all bits on
    //     the same-type round-trip.  Pure IEEE arithmetic, no JVM.
    // ---------------------------------------------------------------------
    {
        // Smallest positive subnormal float (bit pattern 0x1): all bits kept.
        std::uint32_t f_sub_bits{ 0x00000001u };
        float f_sub{};
        std::memcpy(&f_sub, &f_sub_bits, sizeof(f_sub));
        float f_got{ static_cast<float>(read_back<float>("F", f_sub)) };
        std::uint32_t f_got_bits{};
        std::memcpy(&f_got_bits, &f_got, sizeof(f_got_bits));
        check("F_subnormal_bits_preserved", f_got_bits == f_sub_bits);

        // A finite float widens to double exactly: 0.5f (1/2, exactly
        // representable in both) and 0.1f (whose exact value differs from 0.1
        // but is preserved as that exact float promoted to double).
        {
            auto v = read_back<float>("F", 0.5f);
            check("F_half_widens_to_double_exactly", static_cast<double>(v) == 0.5);
        }
        {
            const float tenth{ 0.1f };
            auto v = read_back<float>("F", tenth);
            check("F_tenth_widens_to_double_equals_promoted_float",
                  static_cast<double>(v) == static_cast<double>(tenth));
        }

        // A double that is NOT representable as float narrows via static_cast with
        // round-to-nearest-EVEN.  The float ULP at 1.0 is 2^-23, so the EXACT
        // midpoint between 1.0f and the next float (1.0f + 2^-23) is 1.0 + 2^-24.
        // 1.0f has an even mantissa LSB (0) and 1.0f+2^-23 has an odd LSB (1), so
        // the midpoint rounds DOWN to 1.0f.  All terms are powers of two, exact in
        // double, so this midpoint is represented precisely — the rounding is fully
        // deterministic on every IEEE target.
        {
            const double midpoint_above_one{ 1.0 + (1.0 / 16777216.0) };  // 1 + 2^-24
            auto v = read_back<double>("D", midpoint_above_one);
            check("D_midpoint_above_one_narrows_to_float_round_to_even",
                  static_cast<float>(v) == 1.0f);
        }
        // The matching midpoint between 1.0f+2^-23 (odd LSB) and 1.0f+2^-22 (even
        // LSB) is 1.0 + 2^-23 + 2^-24; round-to-even there goes UP to 1.0f+2^-22.
        // Pins that the rounding chooses EVEN, not a fixed direction.
        {
            const double midpoint_next{ 1.0 + (1.0 / 8388608.0) + (1.0 / 16777216.0) };
            const float expected{ 1.0f + (1.0f / 4194304.0f) };  // 1 + 2^-22
            auto v = read_back<double>("D", midpoint_next);
            check("D_next_midpoint_narrows_up_to_even_float",
                  static_cast<float>(v) == expected);
        }

        // double +inf narrows to float +inf (bit-exact 0x7F800000); a same-type
        // double->double identity already covered, this pins the cross-type
        // overflow-to-inf rule through cast_for_variant's static_cast arm.
        {
            std::uint64_t d_inf_bits{ 0x7FF0000000000000ull };
            double d_inf{};
            std::memcpy(&d_inf, &d_inf_bits, sizeof(d_inf));
            float f_from_d{ static_cast<float>(read_back<double>("D", d_inf)) };
            std::uint32_t f_from_d_bits{};
            std::memcpy(&f_from_d_bits, &f_from_d, sizeof(f_from_d_bits));
            check("D_pos_inf_narrows_to_float_pos_inf", f_from_d_bits == 0x7F800000u);
        }
    }

    // =====================================================================
    // WAVE-5 DEEPENING (additive — every expected value traced from
    // field_proxy::value_t::cast_for_variant<> (vmhook.hpp ~15276-15368),
    // get()'s per-descriptor dispatch (~15525-15652), as_string()
    // (~15424-15439), and value_t::is_reference() (~15445-15448)).  Pure
    // logic / no live JVM: only the compressed value 0 (decode_oop_pointer(0)
    // -> nullptr, no VMStruct access) and pure-arithmetic alternatives are
    // exercised; no fabricated unmapped address is decoded, no live oop is
    // dereferenced.  Sections are numbered 43+ to avoid clashing with the
    // existing 1-42.
    // =====================================================================

    // ---------------------------------------------------------------------
    // 43. EXTRA-WIDTH integer conversion targets through cast_for_variant's
    //     generic numeric arm (the `requires { static_cast<target>(value); }`
    //     branch at ~15360-15363).  Earlier sections pin the fixed-width
    //     std::intNN_t targets; here we drive the C++ builtin spellings
    //     (long long / unsigned long long / unsigned int / short / signed
    //     char / unsigned char) so a regression that narrowed the generic arm
    //     to only the std::intNN_t set would be caught.  Every result is the
    //     standard static_cast value from the stored alternative.
    // ---------------------------------------------------------------------
    {
        // int32 source -12345 fans out across the builtin integer widths.
        auto vi = read_back<std::int32_t>("I", std::int32_t{ -12345 });
        check("i32_to_long_long_is_minus_12345", static_cast<long long>(vi) == -12345LL);
        check("i32_to_short_is_minus_12345", static_cast<short>(vi) == static_cast<short>(-12345));
        check("i32_to_signed_char_truncates",
              static_cast<signed char>(vi) == static_cast<signed char>(static_cast<std::int32_t>(-12345)));
        // -12345 as uint32 is 0xFFFFCFC7; as unsigned long long zero-extends THAT.
        check("i32_minus_12345_to_unsigned_int_is_0xFFFFCFC7",
              static_cast<unsigned int>(vi) == 0xFFFFCFC7u);

        // int64 source large positive narrows / widens per static_cast.
        auto vj = read_back<std::int64_t>("J", std::int64_t{ 5000000000LL });
        check("i64_to_unsigned_long_long_is_5e9",
              static_cast<unsigned long long>(vj) == 5000000000ULL);
        // 5000000000 mod 2^32 == 705032704 (0x2A05F200) as a 32-bit unsigned.
        check("i64_to_unsigned_int_wraps_to_705032704",
              static_cast<unsigned int>(vj) == 705032704u);

        // uint32 (reference) source -> builtin unsigned widths, pure arithmetic.
        auto vu = read_back<std::uint32_t>("Ljava/lang/Object;", std::uint32_t{ 0xFFFFFFFFu });
        check("u32_max_to_unsigned_long_long_is_4294967295",
              static_cast<unsigned long long>(vu) == 4294967295ULL);
        check("u32_max_to_long_long_zero_extends",
              static_cast<long long>(vu) == 4294967295LL);

        // bool source -> builtin widths: true == 1 everywhere.
        auto vz = read_back<std::uint8_t>("Z", std::uint8_t{ 1 });
        check("bool_true_to_long_long_is_1", static_cast<long long>(vz) == 1LL);
        check("bool_true_to_unsigned_char_is_1",
              static_cast<unsigned char>(vz) == static_cast<unsigned char>(1));
    }

    // ---------------------------------------------------------------------
    // 44. std::vector<T> reject arm (~15294-15303) across MORE element types.
    //     For any NON-uint32 source the vector arm returns `{}` (empty)
    //     regardless of element_type; section 26 covered int/bool/string, here
    //     we add float / double / int64_t / char / int16_t / uint32_t element
    //     vectors, all driven through copy-init (the portable spelling — a
    //     static_cast<vector<T>> stays ambiguous on MSVC, see section 26 note).
    //     A zero compressed-OOP array source also yields empty (decode_array_oop
    //     (0) == nullptr, no heap walk), which is JVM-free.
    // ---------------------------------------------------------------------
    {
        std::vector<float> vf = read_back<std::int32_t>("I", 5);
        check("vector_float_empty_for_int_alt", vf.empty());
        std::vector<double> vd = read_back<double>("D", 2.5);
        check("vector_double_empty_for_double_alt", vd.empty());
        std::vector<std::int64_t> vl = read_back<std::int64_t>("J", std::int64_t{ 9 });
        check("vector_int64_empty_for_long_alt", vl.empty());
        std::vector<char> vc = read_back<std::uint16_t>("C", std::uint16_t{ 65 });
        check("vector_char_empty_for_char_alt", vc.empty());
        std::vector<std::int16_t> vs = read_back<std::int16_t>("S", std::int16_t{ 7 });
        check("vector_int16_empty_for_short_alt", vs.empty());
        std::vector<std::uint32_t> vu = read_back<std::uint8_t>("Z", std::uint8_t{ 1 });
        check("vector_uint32_empty_for_bool_alt", vu.empty());

        // Zero compressed-OOP array (uint32 alt) -> empty for each element type.
        std::array<std::uint8_t, 16> storage{};
        storage.fill(std::uint8_t{ 0x00 });
        vmhook::field_proxy arr_f{ storage.data(), "[F", false };
        std::vector<float> zf = arr_f.get();
        check("vector_float_empty_for_zero_oop_array", zf.empty());
        vmhook::field_proxy arr_d{ storage.data(), "[D", false };
        std::vector<double> zd = arr_d.get();
        check("vector_double_empty_for_zero_oop_array", zd.empty());
        vmhook::field_proxy arr_j{ storage.data(), "[J", false };
        std::vector<std::int64_t> zl = arr_j.get();
        check("vector_int64_empty_for_zero_oop_array", zl.empty());
    }

    // ---------------------------------------------------------------------
    // 45. value_t::is_reference() (~15445-15448 == holds_alternative<uint32_t>)
    //     enumerated over EVERY directly-built primitive alternative -> false,
    //     and the uint32 alternative -> true.  Section 19/33 cover this via
    //     get() and one direct uint32; this isolates the predicate from get()
    //     dispatch across all eight primitive alternatives via direct
    //     construction (no proxy, no buffer).  Exhaustive small-domain sweep.
    // ---------------------------------------------------------------------
    {
        check("direct_bool_is_reference_false", value_t{ true }.is_reference() == false);
        check("direct_int8_is_reference_false", value_t{ std::int8_t{ -1 } }.is_reference() == false);
        check("direct_int16_is_reference_false", value_t{ std::int16_t{ 1 } }.is_reference() == false);
        check("direct_int32_is_reference_false", value_t{ std::int32_t{ 1 } }.is_reference() == false);
        check("direct_int64_is_reference_false", value_t{ std::int64_t{ 1 } }.is_reference() == false);
        check("direct_float_is_reference_false", value_t{ float{ 1.0f } }.is_reference() == false);
        check("direct_double_is_reference_false", value_t{ double{ 1.0 } }.is_reference() == false);
        check("direct_uint16_is_reference_false", value_t{ std::uint16_t{ 1 } }.is_reference() == false);
        check("direct_uint32_zero_is_reference_true", value_t{ std::uint32_t{ 0u } }.is_reference() == true);
        check("direct_uint32_nonzero_is_reference_true", value_t{ std::uint32_t{ 42u } }.is_reference() == true);

        // The alternative index and is_reference() must agree for every primitive.
        check("direct_bool_index_matches_alt", value_t{ true }.data.index() == idx::k_bool);
        check("direct_int8_index_matches_alt", value_t{ std::int8_t{ 0 } }.data.index() == idx::k_i8);
        check("direct_int16_index_matches_alt", value_t{ std::int16_t{ 0 } }.data.index() == idx::k_i16);
        check("direct_int32_index_matches_alt", value_t{ std::int32_t{ 0 } }.data.index() == idx::k_i32);
        check("direct_int64_index_matches_alt", value_t{ std::int64_t{ 0 } }.data.index() == idx::k_i64);
        check("direct_float_index_matches_alt", value_t{ float{ 0 } }.data.index() == idx::k_float);
        check("direct_double_index_matches_alt", value_t{ double{ 0 } }.data.index() == idx::k_double);
        check("direct_uint16_index_matches_alt", value_t{ std::uint16_t{ 0 } }.data.index() == idx::k_u16);
        check("direct_uint32_index_matches_alt", value_t{ std::uint32_t{ 0 } }.data.index() == idx::k_u32);
    }

    // ---------------------------------------------------------------------
    // 46. as_string() == static_cast<std::string>() agreement across EVERY
    //     alternative (both name the same uint32-only decode arm; ~15283-15293
    //     and ~15424-15439).  All eight primitive alternatives -> "" on BOTH
    //     spellings; a zero-OOP uint32 -> "" on both (decode_oop_pointer(0) ->
    //     read_java_string(nullptr) -> "").  Driven via direct construction so
    //     the agreement is pinned independently of get().  A non-zero uint32
    //     would call decode_oop_pointer(nonzero) (JVM-only) so is NOT exercised.
    // ---------------------------------------------------------------------
    {
        const value_t prims[]{
            value_t{ true },
            value_t{ std::int8_t{ -7 } },
            value_t{ std::int16_t{ 1234 } },
            value_t{ std::int32_t{ 999999 } },
            value_t{ std::int64_t{ -5LL } },
            value_t{ float{ 3.5f } },
            value_t{ double{ 2.5 } },
            value_t{ std::uint16_t{ 0x41 } },
        };
        bool all_empty_and_agree{ true };
        for (const value_t& v : prims)
        {
            if (!v.as_string().empty()) { all_empty_and_agree = false; }
            if (!static_cast<std::string>(v).empty()) { all_empty_and_agree = false; }
            if (v.as_string() != static_cast<std::string>(v)) { all_empty_and_agree = false; }
        }
        check("as_string_eq_static_cast_empty_all_primitives", all_empty_and_agree);

        // Zero-OOP uint32 (with a String signature): both yield "" and agree.
        value_t zero_oop{ std::uint32_t{ 0u }, std::string{ "Ljava/lang/String;" } };
        check("as_string_zero_oop_empty", zero_oop.as_string().empty());
        check("static_cast_string_zero_oop_empty", static_cast<std::string>(zero_oop).empty());
        check("as_string_eq_static_cast_zero_oop",
              zero_oop.as_string() == static_cast<std::string>(zero_oop));

        // Zero-OOP uint32 with an EMPTY signature still decodes through the
        // uint32 arm (the arm keys on the ALTERNATIVE, not the signature) -> "".
        value_t zero_oop_empty_sig{ std::uint32_t{ 0u } };
        check("as_string_zero_oop_empty_sig_empty", zero_oop_empty_sig.as_string().empty());
    }

    // ---------------------------------------------------------------------
    // 47. void* decode keys on the ALTERNATIVE not the signature, mirrored for
    //     the directly-built value_t.  cast_for_variant<void*> (~15348-15359)
    //     decodes only when the stored alternative is uint32_t; a zero OOP ->
    //     nullptr (decode_oop_pointer(0)).  A uint32 alt built with NO signature
    //     still routes through the decode arm and yields nullptr for a zero OOP,
    //     and every non-uint32 direct alternative -> nullptr via the else arm.
    //     Pins that the signature does not gate the void* path.
    // ---------------------------------------------------------------------
    {
        check("direct_uint32_zero_no_sig_to_void_ptr_null",
              static_cast<void*>(value_t{ std::uint32_t{ 0u } }) == nullptr);
        check("direct_int32_to_void_ptr_null",
              static_cast<void*>(value_t{ std::int32_t{ 12345 } }) == nullptr);
        check("direct_int64_to_void_ptr_null",
              static_cast<void*>(value_t{ std::int64_t{ -1LL } }) == nullptr);
        check("direct_double_to_void_ptr_null",
              static_cast<void*>(value_t{ double{ 1.0 } }) == nullptr);
        check("direct_bool_to_void_ptr_null",
              static_cast<void*>(value_t{ true }) == nullptr);
        check("direct_uint16_to_void_ptr_null",
              static_cast<void*>(value_t{ std::uint16_t{ 0xFFFF } }) == nullptr);

        // const void* (cv void* is the allowed cv-pointer target per the trait):
        // a zero-OOP uint32 alt rides the generic static_cast arm and is nullptr.
        const void* cvp = value_t{ std::uint32_t{ 0u } };
        check("direct_uint32_zero_to_const_void_ptr_null", cvp == nullptr);
    }

    // ---------------------------------------------------------------------
    // 48. unique_ptr<W> FLAW-B signature guard (~15317-15320) enumerated over a
    //     directly-built uint32 ZERO alternative carrying each non-'L' signature
    //     class: array "[I", array-of-objects "[Ljava/lang/Object;", primitive
    //     "I", void "V", unknown "X", empty "", and a lone "[".  EVERY one
    //     returns nullptr BEFORE any decode (the guard fires on
    //     signature.empty() || front() != 'L'), so no JVM.  A 'L...;' signature
    //     with a ZERO oop is ALSO nullptr but via the later `!decoded` arm
    //     (~15322-15326) since decode_oop_pointer(0) == nullptr — still JVM-free.
    // ---------------------------------------------------------------------
    {
        const char* const non_L_sigs[]{ "[I", "[Ljava/lang/Object;", "I", "V", "X", "", "[" };
        bool all_null{ true };
        for (const char* s : non_L_sigs)
        {
            value_t v{ std::uint32_t{ 0u }, std::string{ s } };
            if (static_cast<std::unique_ptr<W>>(v) != nullptr) { all_null = false; }
            // The alternative is genuinely uint32 (so the guard, not the
            // non-uint32 else-arm, is what produced the nullptr).
            if (v.data.index() != idx::k_u32) { all_null = false; }
        }
        check("unique_ptr_null_for_all_non_L_signatures_direct", all_null);

        // 'L...;' signature with a ZERO oop -> nullptr via the !decoded arm
        // (decode_oop_pointer(0) == nullptr), reached AFTER the front()=='L'
        // guard passes — proves the guard is first-char only, JVM-free here.
        value_t l_zero{ std::uint32_t{ 0u }, std::string{ "Ljava/lang/String;" } };
        check("unique_ptr_null_for_L_sig_zero_oop",
              static_cast<std::unique_ptr<W>>(l_zero) == nullptr);
        check("unique_ptr_L_sig_zero_oop_alt_is_uint32",
              l_zero.data.index() == idx::k_u32);
    }

    // ---------------------------------------------------------------------
    // 49. get() dispatch: the eight primitive descriptors are CASE-SENSITIVE
    //     and EXACT-LENGTH.  Each lowercase twin ("z","b","s","i","j","f","d",
    //     "c") and each doubled form falls through to the uint32 reference arm
    //     (~15648-15651), NOT the matching primitive, because the comparison is
    //     `signature_text == "Z"` (a full std::string equality).  Exhaustive
    //     over all eight primitive letters; pure logic, non-null buffer.
    // ---------------------------------------------------------------------
    {
        const char* const lowercase[]{ "z", "b", "s", "i", "j", "f", "d", "c" };
        bool all_uint32{ true };
        for (const char* s : lowercase)
        {
            auto v = read_back<std::uint32_t>(s, std::uint32_t{ 0x01020304u });
            if (v.data.index() != idx::k_u32) { all_uint32 = false; }
            if (std::get<std::uint32_t>(v.data) != 0x01020304u) { all_uint32 = false; }
        }
        check("lowercase_primitive_letters_all_route_to_uint32", all_uint32);

        // A primitive letter followed by a NUL-equivalent extra char (e.g.
        // "I ") is a different std::string -> uint32 fall-through.
        auto vspace = read_back<std::uint32_t>("I ", std::uint32_t{ 0xCAFEBABEu });
        check("primitive_letter_with_trailing_space_routes_to_uint32",
              vspace.data.index() == idx::k_u32);
        check("primitive_letter_with_trailing_space_value_round_trips",
              std::get<std::uint32_t>(vspace.data) == 0xCAFEBABEu);
    }

    // ---------------------------------------------------------------------
    // 50. signature round-trips through value_t for EVERY descriptor get()
    //     dispatches, and the carried signature equals the proxy's own.
    //     get() builds value_t{ value, this->signature_text } on every arm
    //     (~15603-15651), so value.signature must always equal the descriptor —
    //     including the uint32 fall-through ("X", "", "[I").  Section 7 checks
    //     proxy.signature(); this checks the value_t.signature member that the
    //     unique_ptr/string/vector arms consult.
    // ---------------------------------------------------------------------
    {
        const char* const descriptors[]{
            "Z", "B", "S", "I", "J", "F", "D", "C",
            "Ljava/lang/String;", "[I", "[Ljava/lang/Object;", "X", ""
        };
        bool all_match{ true };
        for (const char* d : descriptors)
        {
            std::array<std::uint8_t, 16> storage{};
            storage.fill(std::uint8_t{ 0x00 });
            vmhook::field_proxy proxy{ storage.data(), d, false };
            if (proxy.get().signature != std::string{ d }) { all_match = false; }
        }
        check("value_t_signature_round_trips_all_dispatched_descriptors", all_match);

        // Null-proxy get() also preserves the signature on the int32 fallback
        // arm (~15549) for a reference descriptor.
        vmhook::field_proxy null_ref{ nullptr, "[Ljava/lang/Object;", false };
        check("null_proxy_value_t_signature_preserved",
              null_ref.get().signature == "[Ljava/lang/Object;");
    }

    // ---------------------------------------------------------------------
    // 51. int64 ("J") full cross-cast sweep at the extremes through the generic
    //     numeric arm: INT64_MIN/MAX narrowing to int32 keeps the LOW 32 bits
    //     (static_cast truncation), and the documented widening/identity holds.
    //     All pure two's-complement arithmetic, fully deterministic.
    // ---------------------------------------------------------------------
    {
        // INT64_MIN low 32 bits are 0 -> int32 truncation == 0.
        auto vmin = read_back<std::int64_t>("J", std::int64_t{ -9223372036854775807LL - 1 });
        check("J_min_narrows_to_int32_zero", static_cast<std::int32_t>(vmin) == 0);
        check("J_min_to_uint32_zero", static_cast<std::uint32_t>(vmin) == 0u);
        check("J_min_identity_int64",
              static_cast<std::int64_t>(vmin) == (std::int64_t{ -9223372036854775807LL } - 1));

        // INT64_MAX low 32 bits are 0xFFFFFFFF -> int32 truncation == -1.
        auto vmax = read_back<std::int64_t>("J", std::int64_t{ 9223372036854775807LL });
        check("J_max_narrows_to_int32_minus_one", static_cast<std::int32_t>(vmax) == -1);
        check("J_max_to_uint32_all_ones", static_cast<std::uint32_t>(vmax) == 0xFFFFFFFFu);

        // A value with a known low/high split: 0x1122334455667788.
        auto vsplit = read_back<std::int64_t>("J", static_cast<std::int64_t>(0x1122334455667788LL));
        check("J_split_narrows_to_int32_low_word",
              static_cast<std::int32_t>(vsplit) == static_cast<std::int32_t>(0x55667788u));
        check("J_split_to_uint32_low_word",
              static_cast<std::uint32_t>(vsplit) == 0x55667788u);
        check("J_split_identity_int64",
              static_cast<std::int64_t>(vsplit) == static_cast<std::int64_t>(0x1122334455667788LL));
    }

    // ---------------------------------------------------------------------
    // 52. Implicit operator vs static_cast agreement for the EXTRA-WIDTH and
    //     special targets added above (the two spellings route through the same
    //     cast_for_variant, so they must never diverge).  Covers long long,
    //     unsigned int, void* (zero OOP), and std::string (zero OOP) — the four
    //     arm families (generic numeric, void* decode, string decode).
    // ---------------------------------------------------------------------
    {
        auto vi = read_back<std::int32_t>("I", std::int32_t{ 123456 });
        const long long imp_ll = vi;
        check("i32_implicit_eq_static_cast_long_long",
              imp_ll == static_cast<long long>(vi) && imp_ll == 123456LL);
        const unsigned int imp_ui = vi;
        check("i32_implicit_eq_static_cast_unsigned_int",
              imp_ui == static_cast<unsigned int>(vi) && imp_ui == 123456u);

        // void* zero-OOP: implicit and static_cast both nullptr.
        std::array<std::uint8_t, 16> storage{};
        storage.fill(std::uint8_t{ 0x00 });
        vmhook::field_proxy ref{ storage.data(), "Ljava/lang/Object;", false };
        auto vr = ref.get();
        void* imp_vp = vr;
        check("void_ptr_implicit_eq_static_cast_zero_oop_obj",
              imp_vp == static_cast<void*>(vr) && imp_vp == nullptr);

        // std::string zero-OOP: as_string drives the same decode; static_cast
        // and as_string agree and are both empty.
        std::string imp_s = vr.as_string();
        check("string_as_string_eq_static_cast_zero_oop_obj",
              imp_s == static_cast<std::string>(vr) && imp_s.empty());
    }

    // ---------------------------------------------------------------------
    // 53. COMPILE-TIME variant structural contract.  The runtime index
    //     checks (sections 1/45) pin which ORDINAL each get() arm lands on,
    //     but never the TYPE identity of each ordinal nor the alternative
    //     COUNT.  std::variant_alternative_t<N, ...> and std::variant_size_v
    //     are pure compile-time queries over field_proxy::value_t::data's
    //     declared variant (vmhook.hpp ~14931-14941); a reordering or an
    //     added/removed alternative (FLAW #4 latent hazard) would break BOTH
    //     these static_asserts AND the positional value_t{ alt, sig } call
    //     sites at once.  Nothing here touches memory.
    // ---------------------------------------------------------------------
    {
        using data_t = decltype(std::declval<value_t>().data);
        static_assert(std::variant_size_v<data_t> == 9,
                      "value_t variant must hold exactly 9 alternatives "
                      "(bool,i8,i16,i32,i64,float,double,u16,u32)");
        static_assert(std::is_same_v<std::variant_alternative_t<idx::k_bool, data_t>, bool>,
                      "alternative 0 must be bool");
        static_assert(std::is_same_v<std::variant_alternative_t<idx::k_i8, data_t>, std::int8_t>,
                      "alternative 1 must be int8_t");
        static_assert(std::is_same_v<std::variant_alternative_t<idx::k_i16, data_t>, std::int16_t>,
                      "alternative 2 must be int16_t");
        static_assert(std::is_same_v<std::variant_alternative_t<idx::k_i32, data_t>, std::int32_t>,
                      "alternative 3 must be int32_t");
        static_assert(std::is_same_v<std::variant_alternative_t<idx::k_i64, data_t>, std::int64_t>,
                      "alternative 4 must be int64_t");
        static_assert(std::is_same_v<std::variant_alternative_t<idx::k_float, data_t>, float>,
                      "alternative 5 must be float");
        static_assert(std::is_same_v<std::variant_alternative_t<idx::k_double, data_t>, double>,
                      "alternative 6 must be double");
        static_assert(std::is_same_v<std::variant_alternative_t<idx::k_u16, data_t>, std::uint16_t>,
                      "alternative 7 must be uint16_t");
        static_assert(std::is_same_v<std::variant_alternative_t<idx::k_u32, data_t>, std::uint32_t>,
                      "alternative 8 must be uint32_t (compressed OOP)");
        // signature member is std::string and the default-constructed value_t
        // leaves it empty (consumed by get()'s value_t{ value, sig } builds).
        static_assert(std::is_same_v<decltype(value_t{}.signature), std::string>,
                      "value_t::signature must be std::string");
        check("default_value_t_signature_empty", value_t{}.signature.empty());
        // A default-constructed variant value-initialises its FIRST alternative
        // (bool) per [variant.ctor]; pins that the leading alternative is the
        // monostate-free bool, not a reference alt.
        check("default_value_t_alt_is_bool", value_t{}.data.index() == idx::k_bool);
        check("default_value_t_not_reference", value_t{}.is_reference() == false);
    }

    // ---------------------------------------------------------------------
    // 54. noexcept contract of the whole conversion surface.  get(),
    //     get_compressed_oop(), as_string(), value_t::is_reference(), and the
    //     templated operator target_type() are all declared noexcept
    //     (vmhook.hpp get() ~15577, get_compressed_oop ~15953, as_string
    //     ~15476, is_reference ~15497, operator ~15451).  Pure type-trait
    //     queries — no buffer, no read.  A regression that drops noexcept (and
    //     so could throw out of a hot read path) trips these at compile time.
    // ---------------------------------------------------------------------
    {
        const value_t cv{ std::int32_t{ 0 } };
        static_assert(noexcept(cv.as_string()), "as_string() must be noexcept");
        static_assert(noexcept(cv.is_reference()), "value_t::is_reference() must be noexcept");
        static_assert(noexcept(static_cast<int>(cv)),
                      "operator target_type() (numeric) must be noexcept");
        static_assert(noexcept(static_cast<void*>(cv)),
                      "operator target_type() (void*) must be noexcept");
        vmhook::field_proxy probe{ nullptr, "I", false };
        static_assert(noexcept(probe.get()), "field_proxy::get() must be noexcept");
        static_assert(noexcept(probe.get_compressed_oop()),
                      "field_proxy::get_compressed_oop() must be noexcept");
        static_assert(noexcept(probe.is_reference()),
                      "field_proxy::is_reference() must be noexcept");
        static_assert(noexcept(probe.signature()), "field_proxy::signature() must be noexcept");
        static_assert(noexcept(probe.raw_address()), "field_proxy::raw_address() must be noexcept");
        static_assert(noexcept(probe.is_static()), "field_proxy::is_static() must be noexcept");
        check("noexcept_section_reached", true);
    }

    // ---------------------------------------------------------------------
    // 55. get() is a PURE read: calling it twice on the same proxy yields
    //     value_t copies that are byte-identical in alternative index, carried
    //     signature, and decoded value.  get() reads the SAME bytes each call
    //     (no caching, no mutation of field_pointer); section 34+ exercise the
    //     safe-read path but never the call-to-call stability.  Stack buffer
    //     (valid address), pure logic.
    // ---------------------------------------------------------------------
    {
        std::array<std::uint8_t, 16> storage{};
        storage.fill(std::uint8_t{ 0xAB });
        const std::int32_t planted{ 0x12345678 };
        std::memcpy(storage.data(), &planted, sizeof(planted));
        vmhook::field_proxy proxy{ storage.data(), "I", false };
        const value_t a = proxy.get();
        const value_t b = proxy.get();
        check("get_idempotent_same_index", a.data.index() == b.data.index());
        check("get_idempotent_same_signature", a.signature == b.signature);
        check("get_idempotent_same_value",
              std::get<std::int32_t>(a.data) == std::get<std::int32_t>(b.data));
        check("get_idempotent_value_correct",
              std::get<std::int32_t>(a.data) == planted);

        // Null proxy: get() twice both land on the int32 zero contract with the
        // signature preserved, identically.
        vmhook::field_proxy null_proxy{ nullptr, "Ljava/lang/String;", false };
        const value_t na = null_proxy.get();
        const value_t nb = null_proxy.get();
        check("null_get_idempotent_index",
              na.data.index() == idx::k_i32 && nb.data.index() == idx::k_i32);
        check("null_get_idempotent_signature",
              na.signature == "Ljava/lang/String;" && nb.signature == na.signature);
    }

    // ---------------------------------------------------------------------
    // 56. value_t COPY and MOVE preserve the alternative, signature, and the
    //     decoded value, and conversions on the copy match the original.  The
    //     struct is a plain {variant, std::string} aggregate, so this pins that
    //     no slicing / alternative-collapse occurs across a copy/move and that
    //     a copied value_t is independently convertible.  Direct construction,
    //     pure logic.
    // ---------------------------------------------------------------------
    {
        const value_t original{ std::int64_t{ -9223372036854775807LL - 1 },
                                std::string{ "J" } };
        const value_t copy{ original };           // copy ctor
        check("value_t_copy_preserves_index", copy.data.index() == original.data.index());
        check("value_t_copy_preserves_signature", copy.signature == original.signature);
        check("value_t_copy_preserves_value",
              std::get<std::int64_t>(copy.data) == std::get<std::int64_t>(original.data));
        check("value_t_copy_convertible_matches",
              static_cast<std::int64_t>(copy) == static_cast<std::int64_t>(original));

        value_t source{ std::uint32_t{ 0u }, std::string{ "Ljava/lang/String;" } };
        const std::size_t src_index{ source.data.index() };
        const value_t moved{ std::move(source) }; // move ctor
        check("value_t_move_preserves_index", moved.data.index() == src_index);
        check("value_t_move_preserves_signature", moved.signature == "Ljava/lang/String;");
        check("value_t_move_is_reference_true", moved.is_reference() == true);
        // Moved-from uint32 zero OOP still decodes to "" / nullptr safely.
        check("value_t_move_as_string_empty", moved.as_string().empty());
        check("value_t_move_void_ptr_null", static_cast<void*>(moved) == nullptr);

        // Copy-assignment likewise re-points the alternative + signature.
        value_t assign_target{ double{ 3.5 }, std::string{ "D" } };
        assign_target = original;
        check("value_t_copy_assign_index", assign_target.data.index() == idx::k_i64);
        check("value_t_copy_assign_signature", assign_target.signature == "J");
    }

    // ---------------------------------------------------------------------
    // 57. char ("C", uint16 alternative) NEVER sign-extends into the widest
    //     SIGNED targets.  Section 31 pins operator int at full BMP range;
    //     this extends the no-sign-extension guarantee to int64_t / long long
    //     / unsigned long long at U+FFFF (0xFFFF), the value most likely to
    //     surface a stray sign-extend.  The source is an unsigned 16-bit
    //     alternative, so every widening must zero-extend to exactly 65535.
    //     Pure two's-complement arithmetic, direct construction.
    // ---------------------------------------------------------------------
    {
        const value_t max_unit{ std::uint16_t{ 0xFFFF } };
        check("C_FFFF_to_int64_zero_extends",
              static_cast<std::int64_t>(max_unit) == std::int64_t{ 65535 });
        check("C_FFFF_to_long_long_zero_extends",
              static_cast<long long>(max_unit) == 65535LL);
        check("C_FFFF_to_unsigned_long_long_zero_extends",
              static_cast<unsigned long long>(max_unit) == 65535ULL);
        check("C_FFFF_to_uint32_zero_extends",
              static_cast<std::uint32_t>(max_unit) == 0xFFFFu);
        check("C_FFFF_to_int32_zero_extends",
              static_cast<std::int32_t>(max_unit) == std::int32_t{ 65535 });
        // Round-trip identity on the uint16 target itself.
        check("C_FFFF_to_uint16_identity",
              static_cast<std::uint16_t>(max_unit) == std::uint16_t{ 0xFFFF });
        // Lowest BMP unit (U+0000) widens to exactly 0 on every target.
        const value_t zero_unit{ std::uint16_t{ 0x0000 } };
        check("C_0000_to_int64_zero",
              static_cast<std::int64_t>(zero_unit) == std::int64_t{ 0 });
        check("C_0000_to_unsigned_long_long_zero",
              static_cast<unsigned long long>(zero_unit) == 0ULL);
    }

    // ---------------------------------------------------------------------
    // 58. WAVE-27 LEDGER: explicit static_assert variant index map.  The 9
    //     alternative ORDER is the load-bearing contract every test in this
    //     file pins via `idx::k_*`.  Mirror that contract as static_asserts
    //     directly on the variant type so a future reorder of value_t::data
    //     hard-errors at compile time INSTEAD of silently re-pointing
    //     idx::k_* (which would then quietly pass with the wrong meaning).
    // ---------------------------------------------------------------------
    {
        using variant_t = decltype(std::declval<value_t&>().data);
        static_assert(std::variant_size_v<variant_t> == 9,
                      "value_t::data must hold exactly 9 alternatives");
        static_assert(std::is_same_v<std::variant_alternative_t<0, variant_t>, bool>,
                      "idx 0 = bool (Z)");
        static_assert(std::is_same_v<std::variant_alternative_t<1, variant_t>, std::int8_t>,
                      "idx 1 = int8_t (B)");
        static_assert(std::is_same_v<std::variant_alternative_t<2, variant_t>, std::int16_t>,
                      "idx 2 = int16_t (S)");
        static_assert(std::is_same_v<std::variant_alternative_t<3, variant_t>, std::int32_t>,
                      "idx 3 = int32_t (I)");
        static_assert(std::is_same_v<std::variant_alternative_t<4, variant_t>, std::int64_t>,
                      "idx 4 = int64_t (J)");
        static_assert(std::is_same_v<std::variant_alternative_t<5, variant_t>, float>,
                      "idx 5 = float (F)");
        static_assert(std::is_same_v<std::variant_alternative_t<6, variant_t>, double>,
                      "idx 6 = double (D)");
        static_assert(std::is_same_v<std::variant_alternative_t<7, variant_t>, std::uint16_t>,
                      "idx 7 = uint16_t (C)");
        static_assert(std::is_same_v<std::variant_alternative_t<8, variant_t>, std::uint32_t>,
                      "idx 8 = uint32_t (compressed OOP)");
        check("variant_index_map_static_asserts_compiled", true);
    }

    // ---------------------------------------------------------------------
    // 59. WAVE-27 LEDGER: holds_alternative<T> per-type sweep.  For each of
    //     the 9 alternatives, constructing a value_t with that alternative
    //     must make holds_alternative<T> true for exactly that T and false
    //     for every other.  This is the runtime mirror of section 58.
    // ---------------------------------------------------------------------
    {
        const value_t b{ bool{ true } };
        check("holds_bool_for_bool", std::holds_alternative<bool>(b.data));
        check("holds_not_i32_for_bool", !std::holds_alternative<std::int32_t>(b.data));
        check("holds_not_u32_for_bool", !std::holds_alternative<std::uint32_t>(b.data));

        const value_t i8{ std::int8_t{ -1 } };
        check("holds_i8_for_i8", std::holds_alternative<std::int8_t>(i8.data));
        check("holds_not_i16_for_i8", !std::holds_alternative<std::int16_t>(i8.data));

        const value_t i16{ std::int16_t{ -1 } };
        check("holds_i16_for_i16", std::holds_alternative<std::int16_t>(i16.data));
        check("holds_not_i8_for_i16", !std::holds_alternative<std::int8_t>(i16.data));
        check("holds_not_u16_for_i16", !std::holds_alternative<std::uint16_t>(i16.data));

        const value_t i32{ std::int32_t{ 0 } };
        check("holds_i32_for_i32", std::holds_alternative<std::int32_t>(i32.data));
        check("holds_not_i64_for_i32", !std::holds_alternative<std::int64_t>(i32.data));
        check("holds_not_u32_for_i32", !std::holds_alternative<std::uint32_t>(i32.data));

        const value_t i64{ std::int64_t{ 0 } };
        check("holds_i64_for_i64", std::holds_alternative<std::int64_t>(i64.data));
        check("holds_not_i32_for_i64", !std::holds_alternative<std::int32_t>(i64.data));

        const value_t f{ float{ 0.0f } };
        check("holds_float_for_float", std::holds_alternative<float>(f.data));
        check("holds_not_double_for_float", !std::holds_alternative<double>(f.data));

        const value_t d{ double{ 0.0 } };
        check("holds_double_for_double", std::holds_alternative<double>(d.data));
        check("holds_not_float_for_double", !std::holds_alternative<float>(d.data));

        const value_t u16{ std::uint16_t{ 0 } };
        check("holds_u16_for_u16", std::holds_alternative<std::uint16_t>(u16.data));
        check("holds_not_i16_for_u16", !std::holds_alternative<std::int16_t>(u16.data));

        const value_t u32{ std::uint32_t{ 0 } };
        check("holds_u32_for_u32", std::holds_alternative<std::uint32_t>(u32.data));
        check("holds_not_i32_for_u32", !std::holds_alternative<std::int32_t>(u32.data));
    }

    // ---------------------------------------------------------------------
    // 60. WAVE-27 LEDGER: std::get<WrongT> throws std::bad_variant_access.
    //     Pin the variant's documented exception contract per alternative.
    //     std::get<CorrectT> must succeed, std::get<OtherT> must throw.
    // ---------------------------------------------------------------------
    {
        const value_t v{ std::int32_t{ 42 } };
        bool got_correct{ false };
        try { got_correct = (std::get<std::int32_t>(v.data) == 42); }
        catch (const std::bad_variant_access&) { got_correct = false; }
        check("get_correct_T_returns_value", got_correct);

        bool threw_wrong{ false };
        try { (void)std::get<std::int64_t>(v.data); }
        catch (const std::bad_variant_access&) { threw_wrong = true; }
        check("get_wrong_T_throws_bad_variant_access", threw_wrong);

        bool threw_bool{ false };
        try { (void)std::get<bool>(v.data); }
        catch (const std::bad_variant_access&) { threw_bool = true; }
        check("get_bool_on_i32_throws", threw_bool);

        bool threw_u32{ false };
        try { (void)std::get<std::uint32_t>(v.data); }
        catch (const std::bad_variant_access&) { threw_u32 = true; }
        check("get_u32_on_i32_throws", threw_u32);

        // Float alt: get<double> throws (different alternative even though
        // both are floating-point), get<float> returns the value.
        const value_t fv{ float{ 1.5f } };
        bool threw_double_on_float{ false };
        try { (void)std::get<double>(fv.data); }
        catch (const std::bad_variant_access&) { threw_double_on_float = true; }
        check("get_double_on_float_throws", threw_double_on_float);

        bool threw_float_on_double{ false };
        const value_t dv{ double{ 1.5 } };
        try { (void)std::get<float>(dv.data); }
        catch (const std::bad_variant_access&) { threw_float_on_double = true; }
        check("get_float_on_double_throws", threw_float_on_double);

        // uint32 (OOP alt) vs int32: distinct alternatives despite same width.
        const value_t oop{ std::uint32_t{ 0u } };
        bool threw_i32_on_u32{ false };
        try { (void)std::get<std::int32_t>(oop.data); }
        catch (const std::bad_variant_access&) { threw_i32_on_u32 = true; }
        check("get_i32_on_u32_throws", threw_i32_on_u32);
    }

    // ---------------------------------------------------------------------
    // 61. WAVE-27 LEDGER: default-constructed value_t.  std::variant's
    //     default ctor value-initialises the FIRST alternative.  For
    //     value_t::data that is bool -> false.  signature is an empty
    //     std::string.  Pin this so a future reorder of the variant (which
    //     would break section 58 at compile time) is also pinned at runtime
    //     via its default state.
    // ---------------------------------------------------------------------
    {
        const value_t def{};
        check("default_value_t_index_is_bool", def.data.index() == idx::k_bool);
        check("default_value_t_bool_is_false", std::get<bool>(def.data) == false);
        check("default_value_t_holds_bool", std::holds_alternative<bool>(def.data));
        check("default_value_t_signature_empty", def.signature.empty());
        // is_reference() asks holds_alternative<uint32_t>, so default is false.
        check("default_value_t_is_reference_false", def.is_reference() == false);
        // as_string() on a non-uint32 alternative returns "" by contract.
        check("default_value_t_as_string_empty", def.as_string().empty());
        // void* fallback on non-uint32 alternative is nullptr.
        check("default_value_t_void_ptr_null", static_cast<void*>(def) == nullptr);
        // Implicit bool conversion routes through static_cast<bool>(false).
        check("default_value_t_bool_cast_false", static_cast<bool>(def) == false);
        // Implicit int conversion routes through static_cast<int>(false) == 0.
        check("default_value_t_int_cast_zero", static_cast<int>(def) == 0);
    }

    // ---------------------------------------------------------------------
    // 62. WAVE-27 LEDGER: round-trip every one of the 9 alternatives via
    //     direct value_t{alt} construction (mirroring get()'s producer
    //     contract) and std::get<T> readback.  Covers the boundary values
    //     for each carrier in a single compact sweep — complementary to
    //     section 1 which goes through read_back<>().
    // ---------------------------------------------------------------------
    {
        const value_t vb{ bool{ true } };
        check("rt_bool_true", std::get<bool>(vb.data) == true);
        const value_t vbf{ bool{ false } };
        check("rt_bool_false", std::get<bool>(vbf.data) == false);

        const value_t vi8_min{ std::int8_t{ -128 } };
        check("rt_i8_min", std::get<std::int8_t>(vi8_min.data) == std::int8_t{ -128 });
        const value_t vi8_max{ std::int8_t{ 127 } };
        check("rt_i8_max", std::get<std::int8_t>(vi8_max.data) == std::int8_t{ 127 });

        const value_t vi16_min{ std::int16_t{ -32768 } };
        check("rt_i16_min", std::get<std::int16_t>(vi16_min.data) == std::int16_t{ -32768 });
        const value_t vi16_max{ std::int16_t{ 32767 } };
        check("rt_i16_max", std::get<std::int16_t>(vi16_max.data) == std::int16_t{ 32767 });

        const value_t vi32_min{ std::int32_t{ -2147483647 - 1 } };
        check("rt_i32_min", std::get<std::int32_t>(vi32_min.data) == (-2147483647 - 1));
        const value_t vi32_max{ std::int32_t{ 2147483647 } };
        check("rt_i32_max", std::get<std::int32_t>(vi32_max.data) == 2147483647);

        const value_t vi64_min{ std::int64_t{ -9223372036854775807LL - 1 } };
        check("rt_i64_min", std::get<std::int64_t>(vi64_min.data)
              == (std::int64_t{ -9223372036854775807LL - 1 }));
        const value_t vi64_max{ std::int64_t{ 9223372036854775807LL } };
        check("rt_i64_max", std::get<std::int64_t>(vi64_max.data) == 9223372036854775807LL);

        const value_t vf{ float{ 3.5f } };
        check("rt_float", std::get<float>(vf.data) == 3.5f);
        const value_t vd{ double{ 3.5 } };
        check("rt_double", std::get<double>(vd.data) == 3.5);

        const value_t vu16_max{ std::uint16_t{ 0xFFFF } };
        check("rt_u16_max", std::get<std::uint16_t>(vu16_max.data) == std::uint16_t{ 0xFFFF });
        const value_t vu32_max{ std::uint32_t{ 0xFFFFFFFFu } };
        check("rt_u32_max", std::get<std::uint32_t>(vu32_max.data) == 0xFFFFFFFFu);
    }

    return failures == 0 ? 0 : 1;
}
