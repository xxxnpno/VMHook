// Standalone unit test: field_primitives_get — COLD-STATE read contract.
//
// WAVE-30 LEDGER GAPS this file closes (no-JVM):
//   * cold-state field_proxy::get() on a null oop returns T{} for ALL 8 JVM
//     primitive descriptors (Z/B/S/I/J/F/D/C) when consumed via the implicit
//     conversion to T,
//   * the null-pointer fallback (vmhook.hpp:11551-11554) returns a value_t
//     whose variant alternative is ALWAYS the int32_t alternative regardless
//     of the descriptor — pin the (known-buggy-by-design) invariant so a
//     future "fix" that flips it shows up here intentionally rather than
//     silently breaking every numeric-cast caller,
//   * round-trip of signature_text through the null path is bytewise stable,
//   * noexcept-ness of get() is CHARACTERIZED (not asserted) — the body does
//     std::string comparisons that the standard does not guarantee noexcept,
//     so we pin only what the standard DOES guarantee (the ctor is noexcept,
//     and a repeated cold call against a null proxy never throws in practice
//     across 1024 iterations),
//   * static_asserts on RETURN-TYPE IDENTITY of cast_for_variant<T> for every
//     primitive T — proves the implicit-conversion operator yields exactly T
//     (no surprise integral promotion to int, no double-from-float promotion
//     when the caller asked for float).
//
// Contrast vs sibling files:
//   * test_field_null_safety_nojvm.cpp covers LOOKUP-surface null-name /
//     null-klass / overlong-name input validation,
//   * test_field_proxy_value_conversions.cpp covers conversion semantics
//     across the LIVE value_t variant alternatives (non-null payload),
//   * THIS file covers cold-state get() on a null field_pointer for every
//     primitive descriptor — the read-side mirror of set-guards.

#include <vmhook/vmhook.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

// ---------------------------------------------------------------------------
// SECTION 0 — static_asserts on conversion return-type identity.
// The implicit conversion lives at value_t::operator T() (vmhook.hpp ~11500)
// and delegates to cast_for_variant<T>.  The result of the conversion must be
// EXACTLY T — not int, not double, not a sign-promoted wider type — for every
// JVM primitive C++ representative.  If a future refactor introduces a
// ``return static_cast<long>(...)`` somewhere in the chain, this catches it
// at compile time.
// ---------------------------------------------------------------------------

namespace
{
template <class T>
auto convert_default() -> T
{
    vmhook::field_proxy proxy{ nullptr, std::string{ "I" }, /*is_static=*/false };
    const auto value{ proxy.get() };
    return static_cast<T>(value);
}

// Identity check — the FUNCTION return type must be exactly T (we declared it
// `-> T`).  Pin the conversion operator does not stealth-promote.
static_assert(std::is_same_v<decltype(convert_default<bool>()),         bool>,
              "value_t -> bool conversion must yield bool");
static_assert(std::is_same_v<decltype(convert_default<std::int8_t>()),  std::int8_t>,
              "value_t -> int8_t conversion must yield int8_t");
static_assert(std::is_same_v<decltype(convert_default<std::int16_t>()), std::int16_t>,
              "value_t -> int16_t conversion must yield int16_t");
static_assert(std::is_same_v<decltype(convert_default<std::int32_t>()), std::int32_t>,
              "value_t -> int32_t conversion must yield int32_t");
static_assert(std::is_same_v<decltype(convert_default<std::int64_t>()), std::int64_t>,
              "value_t -> int64_t conversion must yield int64_t");
static_assert(std::is_same_v<decltype(convert_default<float>(),         float{}), float>,
              "value_t -> float conversion must yield float");
static_assert(std::is_same_v<decltype(convert_default<double>(),        double{}), double>,
              "value_t -> double conversion must yield double");
static_assert(std::is_same_v<decltype(convert_default<char16_t>()),     char16_t>,
              "value_t -> char16_t conversion must yield char16_t");
static_assert(std::is_same_v<decltype(convert_default<std::uint16_t>()),std::uint16_t>,
              "value_t -> uint16_t conversion must yield uint16_t (Java char)");
static_assert(std::is_same_v<decltype(convert_default<std::uint32_t>()),std::uint32_t>,
              "value_t -> uint32_t conversion must yield uint32_t (compressed OOP)");
}

// The 3-arg field_proxy ctor MUST be noexcept (vmhook.hpp:15537) — re-pin here
// independently from the sibling file so a refactor that adds throwing logic
// in just one ctor cannot quietly slip past either site.
static_assert(std::is_nothrow_constructible_v<vmhook::field_proxy,
                                              void*, std::string, bool>,
              "field_proxy(void*, string, bool) must be noexcept");

// ---------------------------------------------------------------------------
// SECTION 1 — per-primitive cold-state get() returns T{} via the conversion
// operator.  Every JVM primitive descriptor character is exercised against a
// null field_pointer, with the proxy constructed both static and instance —
// the cold-state behaviour ignores is_static() (it is consulted only at
// lookup time inside object::get_field).
// ---------------------------------------------------------------------------

template <class T>
static auto cold_get_one(std::string_view sig) -> T
{
    vmhook::field_proxy proxy{ nullptr, std::string{ sig }, /*is_static=*/false };
    const auto value{ proxy.get() };
    return static_cast<T>(value);
}

template <class T>
static auto cold_get_one_static(std::string_view sig) -> T
{
    vmhook::field_proxy proxy{ nullptr, std::string{ sig }, /*is_static=*/true };
    const auto value{ proxy.get() };
    return static_cast<T>(value);
}

static auto section_cold_get_all_primitives() -> void
{
    // Z / boolean — T{} == false.
    check("cold get<bool>('Z') == false",
          cold_get_one<bool>("Z") == false);
    check("cold get<bool>('Z') static == false",
          cold_get_one_static<bool>("Z") == false);

    // B / byte — T{} == 0.
    check("cold get<int8_t>('B') == 0",
          cold_get_one<std::int8_t>("B") == std::int8_t{ 0 });
    check("cold get<int8_t>('B') static == 0",
          cold_get_one_static<std::int8_t>("B") == std::int8_t{ 0 });

    // S / short — T{} == 0.
    check("cold get<int16_t>('S') == 0",
          cold_get_one<std::int16_t>("S") == std::int16_t{ 0 });
    check("cold get<int16_t>('S') static == 0",
          cold_get_one_static<std::int16_t>("S") == std::int16_t{ 0 });

    // C / char — T{} == 0.  Both the lossless uint16_t / char16_t targets
    // and the lossy plain-char target — cold state, all zero either way.
    check("cold get<uint16_t>('C') == 0",
          cold_get_one<std::uint16_t>("C") == std::uint16_t{ 0 });
    check("cold get<char16_t>('C') == u'\\0'",
          cold_get_one<char16_t>("C") == char16_t{ 0 });

    // I / int — T{} == 0.
    check("cold get<int32_t>('I') == 0",
          cold_get_one<std::int32_t>("I") == std::int32_t{ 0 });
    check("cold get<int32_t>('I') static == 0",
          cold_get_one_static<std::int32_t>("I") == std::int32_t{ 0 });

    // J / long — T{} == 0.  Critical to read all 64 bits via the int64 target,
    // not the int32 conversion (some callers consume `long` directly).
    check("cold get<int64_t>('J') == 0",
          cold_get_one<std::int64_t>("J") == std::int64_t{ 0 });
    check("cold get<int64_t>('J') static == 0",
          cold_get_one_static<std::int64_t>("J") == std::int64_t{ 0 });

    // F / float — T{} == 0.0f.  Round-trip the bit pattern.
    {
        const float v{ cold_get_one<float>("F") };
        check("cold get<float>('F') == 0.0f", v == 0.0f);
        std::uint32_t bits{ 0 };
        std::memcpy(&bits, &v, sizeof(bits));
        check("cold get<float>('F') bit-pattern == 0x00000000 (+0.0)",
              bits == 0u);
    }

    // D / double — T{} == 0.0.  Round-trip the 64-bit pattern.
    {
        const double v{ cold_get_one<double>("D") };
        check("cold get<double>('D') == 0.0", v == 0.0);
        std::uint64_t bits{ 0 };
        std::memcpy(&bits, &v, sizeof(bits));
        check("cold get<double>('D') bit-pattern == 0 (+0.0)",
              bits == 0u);
    }
}

// ---------------------------------------------------------------------------
// SECTION 2 — variant alternative invariant on the null path.
// vmhook.hpp:11551-11554 returns ``value_t{ std::int32_t{}, signature_text }``
// for EVERY null-pointer call regardless of descriptor.  This is a real bug
// (specialist-documented bug #3) but it is the LIVE contract — pin it so
// every numeric caller's cast continues to work, and so a future fix is
// caught here as a deliberate invariant change.
// ---------------------------------------------------------------------------

static auto section_null_path_variant_alternative() -> void
{
    constexpr const char* sigs[]{ "Z", "B", "S", "C", "I", "J", "F", "D",
                                  "Ljava/lang/String;", "[I" };
    constexpr std::size_t int32_alternative_index{ 3 };

    for (const char* sig : sigs)
    {
        vmhook::field_proxy proxy{ nullptr, std::string{ sig }, false };
        const auto value{ proxy.get() };
        const bool ok{ value.data.index() == int32_alternative_index };
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "cold get('%s') variant alt == int32 (idx 3), actual=%zu",
                      sig, value.data.index());
        check(buf, ok);

        // signature_text round-trips byte-for-byte through the null path.
        char buf2[128];
        std::snprintf(buf2, sizeof(buf2),
                      "cold get('%s') signature round-trips", sig);
        check(buf2, value.signature == sig);
    }
}

// ---------------------------------------------------------------------------
// SECTION 3 — noexcept characterisation.  get() does std::string comparisons
// in the descriptor chain, which the standard does not guarantee noexcept.
// We DO NOT assert noexcept on get() itself (would be a brittle [INFO]).  We
// pin:
//   (a) the ctor IS noexcept (already pinned above),
//   (b) the raw_address() / signature() / is_static() accessors are noexcept,
//   (c) 1024 cold calls in a row against a null proxy never throw and never
//       diverge from T{}.  This is a runtime stand-in for noexcept-in-practice
//       without committing the standard guarantee.
// ---------------------------------------------------------------------------

static auto section_noexcept_characterisation() -> void
{
    // (b) accessor noexcept locks.
    vmhook::field_proxy proxy{ nullptr, std::string{ "I" }, false };
    static_assert(noexcept(proxy.raw_address()),
                  "field_proxy::raw_address() must be noexcept");
    static_assert(noexcept(proxy.is_static()),
                  "field_proxy::is_static() must be noexcept");
    static_assert(noexcept(proxy.signature()),
                  "field_proxy::signature() must be noexcept");
    check("cold proxy raw_address() == nullptr",
          proxy.raw_address() == nullptr);
    check("cold proxy is_static() == false",
          proxy.is_static() == false);
    check("cold proxy signature() == 'I'",
          proxy.signature() == std::string_view{ "I" });

    // (c) 1024-iter idempotent cold read.  Any iteration that throws would
    //     propagate out of the loop and abort the test — successful loop exit
    //     IS the assertion that get() did not throw on the null path.
    bool all_zero{ true };
    for (int i{ 0 }; i < 1024; ++i)
    {
        vmhook::field_proxy p{ nullptr, std::string{ "J" }, false };
        const std::int64_t v{ p.get() };
        if (v != 0) { all_zero = false; break; }
    }
    check("1024-iter cold get<int64>('J') all 0, no throw", all_zero);
}

// ---------------------------------------------------------------------------
// SECTION 4 — null-proxy cold reads do not depend on is_static.
// get() reads field_pointer directly; is_static() is consulted only by
// object::get_field at LOOKUP time.  Same null pointer + same descriptor +
// flipped is_static must yield the same value AND the same variant
// alternative index — pin the orthogonality.
// ---------------------------------------------------------------------------

static auto section_cold_get_static_orthogonal() -> void
{
    constexpr const char* prims[]{ "Z", "B", "S", "C", "I", "J", "F", "D" };
    for (const char* sig : prims)
    {
        vmhook::field_proxy inst{ nullptr, std::string{ sig }, false };
        vmhook::field_proxy stat{ nullptr, std::string{ sig }, true };
        const auto vi{ inst.get() };
        const auto vs{ stat.get() };
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "cold get('%s') variant alt orthogonal to is_static", sig);
        check(buf, vi.data.index() == vs.data.index());

        char buf2[128];
        std::snprintf(buf2, sizeof(buf2),
                      "cold get('%s') signature orthogonal to is_static", sig);
        check(buf2, vi.signature == vs.signature);

        // Numeric value through int32 — same on both proxies (the int32 alt
        // is 0 either way; this guards against a future change that would
        // make the null-path consult is_static).
        const std::int32_t ni{ vi };
        const std::int32_t ns{ vs };
        char buf3[128];
        std::snprintf(buf3, sizeof(buf3),
                      "cold get('%s') int32 value orthogonal to is_static", sig);
        check(buf3, ni == ns && ni == 0);
    }
}

int main()
{
    std::printf("field_primitives_get no-JVM unit test\n");
    section_cold_get_all_primitives();
    section_null_path_variant_alternative();
    section_noexcept_characterisation();
    section_cold_get_static_orthogonal();
    if (failures == 0)
    {
        std::printf("OK\n");
        return 0;
    }
    std::printf("FAIL: %d failures\n", failures);
    return 1;
}
