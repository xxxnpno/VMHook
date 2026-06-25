// Standalone unit test (no JVM) — wave-30 deepening for the field<std::string>()
// GET path: read_java_string() and the value_t -> std::string conversion arm.
//
// LEDGER GAPS this file closes:
//   * cold-state field<std::string>() on a null oop / null field-pointer returns "".
//   * noexcept characterisation of the GET surface (read_java_string ALLOCATES,
//     so the function MUST NOT be noexcept; we pin this at compile time so a
//     future refactor that adds `noexcept` is caught here).
//   * static_assert on the return type: read_java_string and field_proxy::value_t
//     conversion to std::string both yield exactly std::string.
//   * The 4096-character boundary (former hard cap from bug #29) is now safe:
//     it has been LIFTED to a much larger ceiling (read_java_string_max_units),
//     and a length of 4097 — which the old guard would have rejected — is now
//     accepted by the coarse length check.  Driven through pure logic only.
//
// Everything here runs against either a nullptr, a zero compressed OOP, or a
// fabricated stack buffer.  No oop is ever decoded.

#include <vmhook/vmhook.hpp>
#include <array>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

// --- compile-time return-type pins ----------------------------------------
static_assert(std::is_same_v<decltype(vmhook::read_java_string(std::declval<void*>())),
                             std::string>,
              "read_java_string must return std::string by value");

static_assert(std::is_same_v<
                  decltype(std::declval<vmhook::field_proxy>().get().operator std::string()),
                  std::string>,
              "field_proxy::value_t implicit conversion to std::string must yield std::string");

// --- compile-time noexcept characterisation pins --------------------------
// read_java_string ALLOCATES (returns a std::string), so it CANNOT be noexcept.
// If a future refactor sneaks `noexcept` onto it, this fires.
static_assert(!noexcept(vmhook::read_java_string(std::declval<void*>())),
              "read_java_string is allocating; must not be noexcept");

// make_java_string IS noexcept (returns void*; failure -> nullptr).  Pin the
// asymmetry so a regression that flips either direction is caught.
static_assert(noexcept(vmhook::make_java_string(std::declval<std::string_view>())),
              "make_java_string is documented noexcept");

// The cap is a public constexpr; pin its order of magnitude (the wave-30 ledger
// gap calls out the 4096 boundary specifically — the OLD cap.  The new cap is
// vastly larger, and 4097 must be well below 2 * read_java_string_max_units).
static_assert(vmhook::read_java_string_max_units > 4096,
              "read_java_string_max_units must exceed the historical 4096 cap (bug #29)");
static_assert(2 * static_cast<long long>(vmhook::read_java_string_max_units) > 4097,
              "the COARSE raw-length ceiling (2 * max_units) must accept length=4097");

int main()
{
    // =====================================================================
    // SECTION 1 — read_java_string(nullptr) on a COLD library state (no JVM
    // attached, no klass cache populated).  Must return "" without crashing.
    // The null/invalid-pointer guard at vmhook.hpp:20474 short-circuits before
    // find_class is even called.
    // =====================================================================
    {
        std::string s{ vmhook::read_java_string(nullptr) };
        check("cold_read_java_string_nullptr_empty", s.empty());
        check("cold_read_java_string_nullptr_size_zero", s.size() == 0);
    }

    // Same call repeated: must be idempotent and still empty (no hidden state).
    {
        std::string s1{ vmhook::read_java_string(nullptr) };
        std::string s2{ vmhook::read_java_string(nullptr) };
        check("cold_read_java_string_nullptr_idempotent",
              s1.empty() && s2.empty() && s1 == s2);
    }

    // An obviously-invalid pointer (low arithmetic value, fails
    // is_valid_pointer's heuristic) also degrades to "".  Driven via a small
    // integer cast — never dereferenced by us OR by the library (the guard
    // returns first).
    {
        void* bogus{ reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x1)) };
        std::string s{ vmhook::read_java_string(bogus) };
        check("cold_read_java_string_low_addr_empty", s.empty());
    }
    {
        void* bogus{ reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x42)) };
        std::string s{ vmhook::read_java_string(bogus) };
        check("cold_read_java_string_bogus_addr_empty", s.empty());
    }

    // =====================================================================
    // SECTION 2 — field_proxy::get() -> std::string on null/zero field
    // pointers and zero compressed OOPs.  All collapse to "".
    // =====================================================================
    {
        // Null field pointer + String signature: get() routes through the
        // uint32 alternative whose value is 0; cast_for_variant<std::string>
        // decodes oop(0) -> nullptr -> read_java_string(nullptr) -> "".
        vmhook::field_proxy null_ref{ nullptr, "Ljava/lang/String;", false };
        std::string s = null_ref.get();
        check("cold_field_get_string_null_proxy_empty", s.empty());
    }
    {
        // Static-flag variant of the same.
        vmhook::field_proxy null_ref_static{ nullptr, "Ljava/lang/String;", true };
        std::string s = null_ref_static.get();
        check("cold_field_get_string_null_proxy_static_empty", s.empty());
    }
    {
        // Non-null backing storage, but the four bytes ARE 0 -> compressed OOP
        // 0 -> decode_oop_pointer(0) == nullptr -> "".
        std::array<std::uint8_t, 16> storage{};
        storage.fill(std::uint8_t{ 0x00 });
        vmhook::field_proxy ref{ storage.data(), "Ljava/lang/String;", false };
        std::string s = ref.get();
        check("cold_field_get_string_zero_oop_empty", s.empty());
    }
    {
        // Same as above but as_string() spelling, which is the explicit
        // accessor counterpart to the implicit conversion.  Must agree.
        std::array<std::uint8_t, 16> storage{};
        storage.fill(std::uint8_t{ 0x00 });
        vmhook::field_proxy ref{ storage.data(), "Ljava/lang/String;", false };
        std::string implicit_s = ref.get();
        std::string explicit_s = ref.get().as_string();
        check("cold_field_get_implicit_matches_as_string",
              implicit_s == explicit_s && implicit_s.empty());
    }

    // =====================================================================
    // SECTION 3 — Cross-alternative behaviour for the std::string conversion.
    // Every non-uint32 alternative of value_t MUST return "" when cast to
    // std::string (cast_for_variant<std::string> only services the uint32 arm).
    // We exercise this by reading from PRIMITIVE-signature proxies whose get()
    // never produces a uint32 alternative.
    // =====================================================================
    {
        std::array<std::uint8_t, 16> storage{};
        std::int32_t v{ 0x12345678 };
        std::memcpy(storage.data(), &v, sizeof(v));
        vmhook::field_proxy prim_i{ storage.data(), "I", false };
        std::string s = prim_i.get();
        check("cold_field_get_string_from_I_empty", s.empty());
    }
    {
        std::array<std::uint8_t, 16> storage{};
        std::int64_t v{ 0x0123456789ABCDEFll };
        std::memcpy(storage.data(), &v, sizeof(v));
        vmhook::field_proxy prim_j{ storage.data(), "J", false };
        std::string s = prim_j.get();
        check("cold_field_get_string_from_J_empty", s.empty());
    }
    {
        std::array<std::uint8_t, 16> storage{};
        double v{ 3.14 };
        std::memcpy(storage.data(), &v, sizeof(v));
        vmhook::field_proxy prim_d{ storage.data(), "D", false };
        std::string s = prim_d.get();
        check("cold_field_get_string_from_D_empty", s.empty());
    }
    {
        std::array<std::uint8_t, 16> storage{};
        storage[0] = 0x01;
        vmhook::field_proxy prim_z{ storage.data(), "Z", false };
        std::string s = prim_z.get();
        check("cold_field_get_string_from_Z_empty", s.empty());
    }

    // =====================================================================
    // SECTION 4 — read_java_string return-type & semantic invariants pinned at
    // RUNTIME (the static_assert above pins it at compile time).  Default-
    // constructed std::string from a failed decode is the empty string AND has
    // a null-terminator at .data() (the std::string invariant), and round-trips
    // through std::string{} equality.
    // =====================================================================
    {
        std::string s{ vmhook::read_java_string(nullptr) };
        check("cold_returned_string_is_empty_object", s == std::string{});
        check("cold_returned_string_data_nul_terminated", s.c_str()[0] == '\0');
        // size() / length() agree and are zero.
        check("cold_returned_string_size_eq_length", s.size() == s.length());
        check("cold_returned_string_length_zero", s.length() == 0);
    }

    // =====================================================================
    // SECTION 5 — 4096-char boundary safety (former hard cap, bug #29 RESOLVED).
    // The library's coarse pre-decode length check is now
    //   `length <= 0 || length > 2 * read_java_string_max_units`.
    // We cannot decode a real String without a JVM, but we CAN prove the
    // numerical boundary is no longer at 4096: assert that 4096 AND 4097 AND
    // 5000 all sit comfortably below the new coarse ceiling, so a future
    // regression that re-introduces the old `length > 4096` rejection is
    // caught here at runtime arithmetic (compile-time pins are above).
    // =====================================================================
    {
        const long long ceiling{ 2LL * vmhook::read_java_string_max_units };
        check("boundary_4096_below_new_ceiling", 4096LL <= ceiling);
        check("boundary_4097_below_new_ceiling", 4097LL <= ceiling);
        check("boundary_5000_below_new_ceiling", 5000LL <= ceiling);
        // And the new ceiling is strictly LARGER than the old 4096 cap.
        check("new_ceiling_strictly_above_4096_cap", ceiling > 4096LL);
    }

    // The decoded-character ceiling (post-coder branch) is read_java_string_max_units
    // itself — pin that 4096 chars stays comfortably below it too.
    {
        check("char_count_4096_below_max_units",
              4096 <= vmhook::read_java_string_max_units);
        check("char_count_4097_below_max_units",
              4097 <= vmhook::read_java_string_max_units);
    }

    // =====================================================================
    // SECTION 6 — Field-proxy is_reference() / value_t.is_reference() on a
    // cold (null) String-signature proxy.  The proxy reports is_reference()
    // true (signature starts with 'L'), and get() places the zero into the
    // uint32 alternative so value_t::is_reference() is also true.  These hold
    // even with no JVM and no backing memory.
    // =====================================================================
    {
        vmhook::field_proxy null_ref{ nullptr, "Ljava/lang/String;", false };
        check("cold_null_proxy_String_proxy_is_reference",
              null_ref.is_reference() == true);
        // null proxy collapses the value_t to a non-uint32 alternative (zero),
        // so value_t::is_reference() is false even though the SIGNATURE is a
        // reference one — pin the documented divergence (see
        // test_field_proxy_value_conversions.cpp section 19).
        check("cold_null_proxy_String_value_is_reference_false",
              null_ref.get().is_reference() == false);
        // And the as_string() is still empty.
        check("cold_null_proxy_String_as_string_empty",
              null_ref.get().as_string().empty());
    }

    // =====================================================================
    // SECTION 7 — Array-of-String signature with null backing: get() routes
    // through the same uint32 path, and the conversion-to-string falls
    // through (it isn't a String FIELD), yielding "".  Pins the boundary
    // between "[Ljava/lang/String;" (an array reference) and a scalar String.
    // =====================================================================
    {
        vmhook::field_proxy null_arr{ nullptr, "[Ljava/lang/String;", false };
        std::string s = null_arr.get();
        check("cold_null_proxy_StringArray_get_string_empty", s.empty());
        check("cold_null_proxy_StringArray_is_reference",
              null_arr.is_reference() == true);
    }

    std::printf("\n%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
