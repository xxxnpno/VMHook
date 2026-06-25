// Standalone (no-JVM) characterization of method_proxy::call() with WIDE
// (long / double — two interpreter slots each) argument packs.
//
// LEDGER GAPS closed here:
//   (1) cold-state call() with long/double args returns monostate SAFELY
//       (null method short-circuits before any HotSpot read; 2-slot args
//       don't blow past params[8] or corrupt the void sentinel).
//   (2) noexcept characterization for every wide-arg call() signature we
//       use: int64, double, all-wide mixes (J/D leading/trailing/middle),
//       all-wide 4-arg, wide-after-narrow, narrow-after-wide.
//   (3) static_asserts that the overload set ACCEPTS every wide-arg pack
//       the live JNI / call_stub dispatch advertises (J/D mixed packs,
//       boundary-arity).
//   (4) null method_ptr is safe across EVERY wide-arg pack — process must
//       survive, return must be monostate, primitive conversion T{}.
//   (5) bogus (low-VA) method pointer with a (J)J / (D)D / (JD)V sig
//       short-circuits via is_valid_pointer — never dereferenced.
//
// This pairs with tests/jvm/modules/method_call_wide_args.cpp (live wide).
// No JVM, no detour, no fabricated pointer is ever read.

#include <vmhook/vmhook.hpp>

#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

// ---------------------------------------------------------------------------
// (A) noexcept pin for every wide-arg call() signature.
// ---------------------------------------------------------------------------
namespace wide_noexcept_pin
{
    using proxy_t = const vmhook::method_proxy&;

    // single wide
    static_assert(noexcept(std::declval<proxy_t>().call(std::declval<std::int64_t>())),
                  "call(long) must be noexcept");
    static_assert(noexcept(std::declval<proxy_t>().call(std::declval<double>())),
                  "call(double) must be noexcept");

    // two wides
    static_assert(noexcept(std::declval<proxy_t>().call(
                      std::declval<std::int64_t>(), std::declval<std::int64_t>())),
                  "call(long,long) must be noexcept");
    static_assert(noexcept(std::declval<proxy_t>().call(
                      std::declval<double>(), std::declval<double>())),
                  "call(double,double) must be noexcept");
    static_assert(noexcept(std::declval<proxy_t>().call(
                      std::declval<std::int64_t>(), std::declval<double>())),
                  "call(long,double) must be noexcept");

    // wide + narrow interleavings (the slot-pack corruption class)
    static_assert(noexcept(std::declval<proxy_t>().call(
                      std::declval<std::int64_t>(), std::declval<std::int32_t>())),
                  "call(long,int) must be noexcept");
    static_assert(noexcept(std::declval<proxy_t>().call(
                      std::declval<std::int32_t>(), std::declval<std::int64_t>())),
                  "call(int,long) must be noexcept");
    static_assert(noexcept(std::declval<proxy_t>().call(
                      std::declval<double>(), std::declval<std::int32_t>())),
                  "call(double,int) must be noexcept");
    static_assert(noexcept(std::declval<proxy_t>().call(
                      std::declval<std::int32_t>(), std::declval<double>())),
                  "call(int,double) must be noexcept");

    // wide in the middle
    static_assert(noexcept(std::declval<proxy_t>().call(
                      std::declval<std::int32_t>(),
                      std::declval<std::int64_t>(),
                      std::declval<std::int32_t>())),
                  "call(int,long,int) must be noexcept");
    static_assert(noexcept(std::declval<proxy_t>().call(
                      std::declval<std::int32_t>(),
                      std::declval<double>(),
                      std::declval<std::int32_t>())),
                  "call(int,double,int) must be noexcept");

    // four wides (all-wide pack)
    static_assert(noexcept(std::declval<proxy_t>().call(
                      std::declval<std::int64_t>(),
                      std::declval<double>(),
                      std::declval<std::int64_t>(),
                      std::declval<double>())),
                  "call(long,double,long,double) must be noexcept");
}

// ---------------------------------------------------------------------------
// (B) Overload-set: every wide-arg pack we use must be well-formed.
// ---------------------------------------------------------------------------
namespace wide_overload_set
{
    template<typename, typename... args_t>
    struct callable : std::false_type {};

    template<typename... args_t>
    struct callable<
        std::void_t<decltype(std::declval<const vmhook::method_proxy&>()
                                 .call(std::declval<args_t>()...))>,
        args_t...> : std::true_type {};

    template<typename... args_t>
    inline constexpr bool callable_v{ callable<void, args_t...>::value };

    static_assert(callable_v<std::int64_t>,                       "long");
    static_assert(callable_v<double>,                             "double");
    static_assert(callable_v<std::int64_t, std::int64_t>,         "long,long");
    static_assert(callable_v<double, double>,                     "double,double");
    static_assert(callable_v<std::int64_t, double>,               "long,double");
    static_assert(callable_v<double, std::int64_t>,               "double,long");
    static_assert(callable_v<std::int64_t, std::int32_t>,         "long,int");
    static_assert(callable_v<std::int32_t, std::int64_t>,         "int,long");
    static_assert(callable_v<std::int32_t, std::int64_t, std::int32_t>,
                  "int,long,int (wide-in-middle)");
    static_assert(callable_v<std::int32_t, double, std::int32_t>,
                  "int,double,int (wide-in-middle)");
    static_assert(callable_v<std::int64_t, std::int64_t, std::int64_t, std::int64_t>,
                  "four longs");
    static_assert(callable_v<double, double, double, double>,
                  "four doubles");
    static_assert(callable_v<std::int64_t, double, std::int64_t, double>,
                  "long,double,long,double (all-wide)");
}

// ---------------------------------------------------------------------------
// (C) Cold-state call() with wide-arg packs: null method ptr returns
//     monostate sentinel for EVERY pack; T{} on conversion.
// ---------------------------------------------------------------------------
static auto run_cold_wide_call() -> void
{
    using value_t = vmhook::method_proxy::value_t;

    const vmhook::method_proxy cold{
        /*owning_object*/ nullptr,
        /*method_ptr*/    nullptr,
        /*sig*/           std::string{ "(JD)V" } };

    constexpr std::int64_t LMIN{ std::numeric_limits<std::int64_t>::min() };
    constexpr std::int64_t LMAX{ std::numeric_limits<std::int64_t>::max() };
    const double DMIN{ std::numeric_limits<double>::lowest() };
    const double DMAX{ std::numeric_limits<double>::max() };

    // single wide
    const value_t rJ{ cold.call(LMIN) };
    const value_t rD{ cold.call(DMAX) };
    check("cold_call_long_is_void",   rJ.is_void());
    check("cold_call_double_is_void", rD.is_void());
    check("cold_call_long_variant0",  rJ.data.index() == 0u);
    check("cold_call_double_variant0",rD.data.index() == 0u);
    check("cold_call_long_to_int64_zero",
          static_cast<std::int64_t>(rJ) == 0);
    check("cold_call_double_to_double_zero",
          static_cast<double>(rD) == 0.0);

    // two wides
    const value_t rJJ{ cold.call(LMIN, LMAX) };
    const value_t rDD{ cold.call(DMIN, DMAX) };
    const value_t rJD{ cold.call(LMAX, DMAX) };
    const value_t rDJ{ cold.call(DMIN, LMIN) };
    check("cold_call_long_long_is_void",     rJJ.is_void());
    check("cold_call_double_double_is_void", rDD.is_void());
    check("cold_call_long_double_is_void",   rJD.is_void());
    check("cold_call_double_long_is_void",   rDJ.is_void());

    // wide + narrow interleavings
    const value_t rJi{ cold.call(LMAX, std::int32_t{ 42 }) };
    const value_t riJ{ cold.call(std::int32_t{ 42 }, LMAX) };
    const value_t rDi{ cold.call(DMAX, std::int32_t{ 42 }) };
    const value_t riD{ cold.call(std::int32_t{ 42 }, DMAX) };
    check("cold_call_long_int_is_void",   rJi.is_void());
    check("cold_call_int_long_is_void",   riJ.is_void());
    check("cold_call_double_int_is_void", rDi.is_void());
    check("cold_call_int_double_is_void", riD.is_void());

    // wide in the middle (flanking ints must survive in the live path; here
    // we only assert process survival + sentinel)
    const value_t riJi{ cold.call(std::int32_t{ 1 }, LMAX, std::int32_t{ 99 }) };
    const value_t riDi{ cold.call(std::int32_t{ 1 }, DMAX, std::int32_t{ 99 }) };
    check("cold_call_int_long_int_is_void",   riJi.is_void());
    check("cold_call_int_double_int_is_void", riDi.is_void());

    // four wides — exactly fills the 8-slot conceptual frame (one C++ arg per
    // params[] word, see specialist notes)
    const value_t rJDJD{ cold.call(LMIN, DMIN, LMAX, DMAX) };
    const value_t rJJJJ{ cold.call(LMIN, LMIN, LMAX, LMAX) };
    const value_t rDDDD{ cold.call(DMIN, DMIN, DMAX, DMAX) };
    check("cold_call_four_mixed_wide_is_void", rJDJD.is_void());
    check("cold_call_four_long_is_void",       rJJJJ.is_void());
    check("cold_call_four_double_is_void",     rDDDD.is_void());

    // Boundary long bit patterns the live test uses as truncation witnesses
    const std::int64_t HIGH_HALF{ static_cast<std::int64_t>(0xFFFFFFFF00000000ULL) };
    const std::int64_t LOW_HALF { static_cast<std::int64_t>(0x00000000FFFFFFFFULL) };
    const std::int64_t MIXED   { static_cast<std::int64_t>(0xDEADBEEFCAFEBABEULL) };
    check("cold_call_long_high_half_is_void",  cold.call(HIGH_HALF).is_void());
    check("cold_call_long_low_half_is_void",   cold.call(LOW_HALF).is_void());
    check("cold_call_long_mixed_bits_is_void", cold.call(MIXED).is_void());
}

// ---------------------------------------------------------------------------
// (D) Bogus method pointer (low-VA cookie, is_valid_pointer rejects)
//     across wide-arg packs.  Never dereferenced.
// ---------------------------------------------------------------------------
static auto run_bogus_wide_call() -> void
{
    using value_t = vmhook::method_proxy::value_t;

    auto* const bogus{
        reinterpret_cast<vmhook::hotspot::method*>(static_cast<std::uintptr_t>(0x42)) };

    const vmhook::method_proxy pJ{ nullptr, bogus, std::string{ "(J)J" } };
    const vmhook::method_proxy pD{ nullptr, bogus, std::string{ "(D)D" } };
    const vmhook::method_proxy pJD{ nullptr, bogus, std::string{ "(JD)V" } };
    const vmhook::method_proxy pJJJJ{ nullptr, bogus, std::string{ "(JJJJ)J" } };
    const vmhook::method_proxy pDDDD{ nullptr, bogus, std::string{ "(DDDD)D" } };

    const value_t rJ{ pJ.call(std::int64_t{ -1 }) };
    const value_t rD{ pD.call(double{ 1.5 }) };
    const value_t rJD{ pJD.call(std::int64_t{ 7 }, double{ 7.0 }) };
    const value_t rJJJJ{ pJJJJ.call(std::int64_t{ 1 }, std::int64_t{ 2 },
                                     std::int64_t{ 3 }, std::int64_t{ 4 }) };
    const value_t rDDDD{ pDDDD.call(1.0, 2.0, 3.0, 4.0) };

    check("bogus_call_J_is_void",    rJ.is_void());
    check("bogus_call_D_is_void",    rD.is_void());
    check("bogus_call_JD_is_void",   rJD.is_void());
    check("bogus_call_JJJJ_is_void", rJJJJ.is_void());
    check("bogus_call_DDDD_is_void", rDDDD.is_void());

    check("bogus_call_J_to_int64_zero",   static_cast<std::int64_t>(rJ) == 0);
    check("bogus_call_D_to_double_zero",  static_cast<double>(rD) == 0.0);
    check("bogus_call_JJJJ_variant0",     rJJJJ.data.index() == 0u);
    check("bogus_call_DDDD_variant0",     rDDDD.data.index() == 0u);
}

int main()
{
    run_cold_wide_call();
    run_bogus_wide_call();

    std::printf("\n%s (%d failure%s)\n",
                failures == 0 ? "ALL PASS" : "FAILURES",
                failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
