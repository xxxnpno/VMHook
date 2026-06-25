// Standalone (no-JVM) characterization of the method_proxy::value_t SFINAE
// return-type matrix: which target_type productions of the constrained
// `operator target_type()` (vmhook.hpp:16335-16337) survive overload resolution,
// and which are excised by detail::value_t_convertible_target_v (vmhook.hpp:1851).
//
// SCOPE (ledger gap: "SFINAE return-type matrix — call() valid for every T in
// {primitives, std::string, wrapper<W>}; static_asserts on valid/invalid
// overload distinction; compile-time only"):
//
//   * Every PRIMITIVE return target (bool, int8_t..int64_t, uint16_t, char,
//     float, double) is convertible-from `value_t`.
//   * std::string is convertible-from `value_t` (no ambiguity, see the
//     constraint commentary at vmhook.hpp:16325-16333).
//   * std::unique_ptr<W> where W derives from vmhook::object_base is
//     convertible.  (We pin via the trait — the static_assert inside the
//     conversion operator is only triggered at instantiation, so the SFINAE
//     gate is the constraint itself.)
//   * `void*` is the ONE permitted pointer (the compressed-OOP decode target).
//   * Every OTHER pointer, `std::nullptr_t`, `const char*`, `char*`, raw `W*`
//     are EXCISED — value_t_convertible_target_v<T> is false.
//
// All checks are compile-time (static_assert).  Runtime side: spot-check that a
// default value_t actually converts to a couple of these targets without
// throwing, to confirm the SFINAE gate matches dispatchable code paths.

#include <vmhook/vmhook.hpp>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <type_traits>

namespace mcrts_nojvm
{
    using value_t = vmhook::method_proxy::value_t;

    template<typename T>
    inline constexpr bool ok_v = vmhook::detail::value_t_convertible_target_v<T>;

    // ---- (A) PRIMITIVE return targets ---------------------------------------
    static_assert(ok_v<bool>,            "bool must be a valid value_t target (Java Z)");
    static_assert(ok_v<std::int8_t>,     "int8_t must be a valid value_t target (Java B)");
    static_assert(ok_v<std::int16_t>,    "int16_t must be a valid value_t target (Java S)");
    static_assert(ok_v<std::int32_t>,    "int32_t must be a valid value_t target (Java I)");
    static_assert(ok_v<std::int64_t>,    "int64_t must be a valid value_t target (Java J)");
    static_assert(ok_v<std::uint16_t>,   "uint16_t must be a valid value_t target (Java C)");
    static_assert(ok_v<float>,           "float must be a valid value_t target (Java F)");
    static_assert(ok_v<double>,          "double must be a valid value_t target (Java D)");
    // wider/narrower numeric widenings the call-site may legitimately request
    static_assert(ok_v<std::uint8_t>,    "uint8_t must be a valid value_t target");
    static_assert(ok_v<std::uint32_t>,   "uint32_t must be a valid value_t target");
    static_assert(ok_v<std::uint64_t>,   "uint64_t must be a valid value_t target");
    static_assert(ok_v<char>,            "char must be a valid value_t target");
    static_assert(ok_v<long long>,       "long long must be a valid value_t target");

    // cv-ref qualified primitives must classify by the underlying type
    // (vmhook.hpp:1848-1849).
    static_assert(ok_v<const int>,          "const int target must be valid");
    static_assert(ok_v<const std::int64_t&>,"const int64_t& target must be valid");
    static_assert(ok_v<int&&>,              "int&& target must be valid");

    // ---- (B) std::string ----------------------------------------------------
    static_assert(ok_v<std::string>,
                  "std::string must be a valid value_t target (Java L java/lang/String)");
    static_assert(ok_v<const std::string&>,
                  "const std::string& target must be valid");

    // ---- (C) std::unique_ptr<W> --------------------------------------------
    struct dummy_wrapper : public vmhook::object_base
    {
        using object_base::object_base;
    };
    static_assert(std::is_base_of_v<vmhook::object_base, dummy_wrapper>,
                  "test wrapper precondition");
    static_assert(ok_v<std::unique_ptr<dummy_wrapper>>,
                  "std::unique_ptr<W: object_base> must be a valid value_t target");

    // ---- (D) void* is the ONE permitted pointer ----------------------------
    static_assert(ok_v<void*>,        "void* must be a valid value_t target");
    static_assert(ok_v<const void*>,  "const void* must be a valid value_t target");

    // ---- (E) EXCISED targets: nullptr_t + every non-void pointer -----------
    // These are the productions value_t_convertible_target_v removes to
    // disambiguate static_cast<std::string>(call()) on MSVC /permissive- and
    // to refuse to silently fabricate a `const char*` into a String return.
    static_assert(!ok_v<std::nullptr_t>,
                  "std::nullptr_t must be EXCISED from value_t targets");
    static_assert(!ok_v<const char*>,
                  "const char* must be EXCISED (was: ambiguous with std::string)");
    static_assert(!ok_v<char*>,
                  "char* must be EXCISED");
    static_assert(!ok_v<const wchar_t*>,
                  "const wchar_t* must be EXCISED");
    static_assert(!ok_v<int*>,
                  "int* must be EXCISED");
    static_assert(!ok_v<const int*>,
                  "const int* must be EXCISED");
    static_assert(!ok_v<dummy_wrapper*>,
                  "raw W* must be EXCISED (use std::unique_ptr<W> for object returns)");
    static_assert(!ok_v<const dummy_wrapper*>,
                  "raw const W* must be EXCISED");
    static_assert(!ok_v<void**>,
                  "void** must be EXCISED (double-indirection is not a compressed-OOP target)");
    static_assert(!ok_v<std::string*>,
                  "std::string* must be EXCISED");

    // cv-ref qualified pointer targets must ALSO be classified by underlying
    // pointer-ness (the constraint strips cvref first).
    static_assert(!ok_v<const char* const>,
                  "const char* const must be EXCISED (cvref strip)");
    static_assert(!ok_v<const char*&>,
                  "const char*& must be EXCISED (cvref strip)");
    static_assert(ok_v<void* const&>,
                  "void* const& must be valid (void* underlying)");

    // ---- (F) Conversion-operator exists for valid targets ------------------
    // The constraint is necessary AND sufficient — when the trait is true the
    // conversion is well-formed (instantiable).  std::is_convertible_v sees
    // through the requires clause.
    static_assert(std::is_convertible_v<value_t, bool>,
                  "value_t -> bool must be a valid conversion");
    static_assert(std::is_convertible_v<value_t, std::int32_t>,
                  "value_t -> int32_t must be a valid conversion");
    static_assert(std::is_convertible_v<value_t, std::int64_t>,
                  "value_t -> int64_t must be a valid conversion");
    static_assert(std::is_convertible_v<value_t, float>,
                  "value_t -> float must be a valid conversion");
    static_assert(std::is_convertible_v<value_t, double>,
                  "value_t -> double must be a valid conversion");
    static_assert(std::is_convertible_v<value_t, std::string>,
                  "value_t -> std::string must be a valid conversion");
    static_assert(std::is_convertible_v<value_t, void*>,
                  "value_t -> void* must be a valid conversion");
    static_assert(std::is_convertible_v<value_t, std::unique_ptr<dummy_wrapper>>,
                  "value_t -> std::unique_ptr<W> must be a valid conversion");

    // ---- (G) Conversion DOES NOT exist for excised targets -----------------
    static_assert(!std::is_convertible_v<value_t, const char*>,
                  "value_t -> const char* MUST be excised (overload not viable)");
    static_assert(!std::is_convertible_v<value_t, char*>,
                  "value_t -> char* MUST be excised");
    static_assert(!std::is_convertible_v<value_t, int*>,
                  "value_t -> int* MUST be excised");
    static_assert(!std::is_convertible_v<value_t, dummy_wrapper*>,
                  "value_t -> raw W* MUST be excised");
    static_assert(!std::is_convertible_v<value_t, std::nullptr_t>,
                  "value_t -> std::nullptr_t MUST be excised");

    // ---- (H) noexcept-ness of the constrained conversion -------------------
    static_assert(noexcept(static_cast<std::int32_t>(std::declval<const value_t&>())),
                  "value_t -> int32_t must be noexcept");
    static_assert(noexcept(static_cast<void*>(std::declval<const value_t&>())),
                  "value_t -> void* must be noexcept");
    // NOTE: value_t -> std::string / unique_ptr<W> are noexcept on the
    // conversion-OPERATOR boundary (the operator is marked noexcept and
    // visit() returns by value into the named return), but a static_cast
    // expression also accounts for the materialized prvalue's
    // move/copy-construction at the call site — which is NOT noexcept for
    // std::string (allocating types) on every libstdc++.  We therefore do
    // NOT static_assert noexcept on those casts; the SFINAE gate above
    // already pins that the conversions are well-formed.

    // ---- (I) Runtime spot-check: monostate default actually converts -------
    // Confirms the SFINAE gate matches REAL code paths (not just trait sugar):
    // a default value_t (the void/cold-call sentinel) converts to every
    // permitted target without throwing, yielding the zero / empty value.
    static int failures{ 0 };
    static auto check(const char* name, bool ok) -> void
    {
        std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
        if (!ok) { ++failures; }
    }

    inline auto run_runtime_spotcheck() -> void
    {
        value_t v{};
        check("monostate_to_bool",         static_cast<bool>(v) == false);
        check("monostate_to_int8",         static_cast<std::int8_t>(v) == 0);
        check("monostate_to_int16",        static_cast<std::int16_t>(v) == 0);
        check("monostate_to_int32",        static_cast<std::int32_t>(v) == 0);
        check("monostate_to_int64",        static_cast<std::int64_t>(v) == 0);
        check("monostate_to_uint16",       static_cast<std::uint16_t>(v) == 0);
        check("monostate_to_float",        static_cast<float>(v) == 0.0f);
        check("monostate_to_double",       static_cast<double>(v) == 0.0);
        check("monostate_to_string_empty", static_cast<std::string>(v).empty());
        check("monostate_to_voidp_null",   static_cast<void*>(v) == nullptr);
        std::unique_ptr<dummy_wrapper> up{ static_cast<std::unique_ptr<dummy_wrapper>>(v) };
        check("monostate_to_uniqueptr_null", up.get() == nullptr);
    }
}

int main()
{
    mcrts_nojvm::run_runtime_spotcheck();
    std::printf("\n%s (%d failure%s)\n",
                mcrts_nojvm::failures == 0 ? "ALL PASS" : "FAILURES",
                mcrts_nojvm::failures,
                mcrts_nojvm::failures == 1 ? "" : "s");
    return mcrts_nojvm::failures == 0 ? 0 : 1;
}
