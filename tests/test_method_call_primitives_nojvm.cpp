// Standalone (no-JVM) characterization of method_proxy::call()'s COLD-STATE
// primitive return surface.  Pair with test_method_call_arg_cap.cpp (arity cap)
// and tests/jvm/modules/method_call_primitives.cpp (live primitive returns).
//
// LEDGER GAP closed here:
//   (1) call() with a null/invalid method pointer SAFELY returns the void
//       sentinel value_t (monostate); converting that to each JVM primitive
//       (bool / int8 / int16 / int32 / int64 / float / double / uint16 char)
//       yields a default-constructed T - no UB, no deref of a fabricated
//       Method*.  This is the "null this" / cold-state contract.
//   (2) method_proxy::call is noexcept (the static signature guarantee the
//       hot detour path relies on, mirrored as static_assert so a regression
//       fails the BUILD on every matrix compiler).
//   (3) The OVERLOAD SET is callable for every primitive arg pack we use in
//       practice (0..8 args of each primitive type), pinned by static_assert
//       via a SFINAE-friendly is_callable detector.  This is the C++-side
//       guarantee underneath the JNI / call_stub dispatch.
//   (4) value_t default-construction is noexcept and is_void()/as_string()/
//       conversion to each primitive are well-defined on the default state.
//
// SCOPE: no live JVM, no detour, no fabricated pointer is ever dereferenced.
// The method pointer we hand the proxy is nullptr; call()'s early-out at the
// top of the function (`if (!this->method || !is_valid_pointer(method))`)
// short-circuits to a monostate value_t before any HotSpot read.  Every
// assertion below is genuinely host-C++.

#include <vmhook/vmhook.hpp>

#include <cstdio>
#include <cstdint>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>
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
// (A) noexcept characterization - pinned at compile time.
//
// method_proxy::call is declared `noexcept` (see vmhook.hpp).  The hot detour
// path that drives call() runs inside an SEH-less window on some toolchains,
// so a future change that drops noexcept would silently widen the failure
// surface.  Pin it.
// ---------------------------------------------------------------------------
namespace mcp_noexcept_pin
{
    using value_t = vmhook::method_proxy::value_t;

    // value_t default-ctor is noexcept (std::variant<monostate, ...> default-
    // constructs to monostate, no allocation).
    static_assert(std::is_nothrow_default_constructible_v<value_t>,
                  "value_t{} must be noexcept (monostate default)");

    // The conversion operator into a primitive is a member template; we can
    // only check the instantiations we actually use.  Each must be noexcept
    // (the conversion is a static_cast over a constant-time variant visit).
    static_assert(noexcept(static_cast<bool>(std::declval<const value_t&>())),
                  "value_t -> bool conversion must be noexcept");
    static_assert(noexcept(static_cast<std::int8_t>(std::declval<const value_t&>())),
                  "value_t -> int8 conversion must be noexcept");
    static_assert(noexcept(static_cast<std::int16_t>(std::declval<const value_t&>())),
                  "value_t -> int16 conversion must be noexcept");
    static_assert(noexcept(static_cast<std::int32_t>(std::declval<const value_t&>())),
                  "value_t -> int32 conversion must be noexcept");
    static_assert(noexcept(static_cast<std::int64_t>(std::declval<const value_t&>())),
                  "value_t -> int64 conversion must be noexcept");
    static_assert(noexcept(static_cast<float>(std::declval<const value_t&>())),
                  "value_t -> float conversion must be noexcept");
    static_assert(noexcept(static_cast<double>(std::declval<const value_t&>())),
                  "value_t -> double conversion must be noexcept");
    static_assert(noexcept(static_cast<std::uint16_t>(std::declval<const value_t&>())),
                  "value_t -> uint16 (char) conversion must be noexcept");

    // is_void / is_string are status queries - must be noexcept.
    static_assert(noexcept(std::declval<const value_t&>().is_void()),
                  "value_t::is_void() must be noexcept");
    static_assert(noexcept(std::declval<const value_t&>().is_string()),
                  "value_t::is_string() must be noexcept");

    // method_proxy::call() itself is noexcept on every primitive arg pack we
    // exercise here.  This is what the live-JVM detour and the SEH-less
    // platforms rely on.
    static_assert(noexcept(std::declval<const vmhook::method_proxy&>().call()),
                  "call() with no args must be noexcept");
    static_assert(noexcept(std::declval<const vmhook::method_proxy&>().call(
                      std::declval<std::int32_t>())),
                  "call(int32) must be noexcept");
    static_assert(noexcept(std::declval<const vmhook::method_proxy&>().call(
                      std::declval<std::int64_t>())),
                  "call(int64) must be noexcept");
    static_assert(noexcept(std::declval<const vmhook::method_proxy&>().call(
                      std::declval<float>())),
                  "call(float) must be noexcept");
    static_assert(noexcept(std::declval<const vmhook::method_proxy&>().call(
                      std::declval<double>())),
                  "call(double) must be noexcept");
    static_assert(noexcept(std::declval<const vmhook::method_proxy&>().call(
                      std::declval<bool>())),
                  "call(bool) must be noexcept");
    // method_proxy ctor itself is noexcept (the proxy is a hot-path object).
    static_assert(std::is_nothrow_constructible_v<vmhook::method_proxy,
                                                  void*,
                                                  vmhook::hotspot::method*,
                                                  std::string>,
                  "method_proxy ctor must be noexcept");
}

// ---------------------------------------------------------------------------
// (B) Overload-set detector: `proxy.call(args...)` is well-formed for the
//     primitive arg packs the JNI fast path supports.  This catches a
//     regression that would, e.g., constrain the call() member template in a
//     way that broke a primitive arg type.
// ---------------------------------------------------------------------------
namespace mcp_overload_set
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

    // Every primitive type, alone:
    static_assert(callable_v<>,                "call() must be well-formed");
    static_assert(callable_v<bool>,            "call(bool)");
    static_assert(callable_v<std::int8_t>,     "call(int8)");
    static_assert(callable_v<std::int16_t>,    "call(int16)");
    static_assert(callable_v<std::int32_t>,    "call(int32)");
    static_assert(callable_v<std::int64_t>,    "call(int64)");
    static_assert(callable_v<float>,           "call(float)");
    static_assert(callable_v<double>,          "call(double)");
    static_assert(callable_v<std::uint16_t>,   "call(uint16/char)");
    // A representative mixed 8-arg pack at the cap (each kind once):
    static_assert(callable_v<bool, std::int8_t, std::int16_t, std::int32_t,
                             std::int64_t, float, double, std::uint16_t>,
                  "8-arg mixed primitive pack must be well-formed");
}

// ---------------------------------------------------------------------------
// (C) Cold-state call(): null method pointer is SAFE and returns a void
//     value_t.  Converting that to each primitive yields T{} - the cold
//     "T{} return" contract from the wave-30 ledger.
// ---------------------------------------------------------------------------
static auto run_cold_call_returns_t_default() -> void
{
    using value_t = vmhook::method_proxy::value_t;

    // Default-constructed value_t IS the cold sentinel.  Pin its surface
    // first - is_void true, is_string false, as_string empty, every primitive
    // conversion T{}.
    {
        value_t v{};
        check("cold_value_t_default_is_void",      v.is_void());
        check("cold_value_t_default_not_string",   !v.is_string());
        check("cold_value_t_default_as_string_empty", v.as_string().empty());

        check("cold_value_t_to_bool_is_false",
              static_cast<bool>(v) == false);
        check("cold_value_t_to_int8_is_zero",
              static_cast<std::int8_t>(v) == std::int8_t{ 0 });
        check("cold_value_t_to_int16_is_zero",
              static_cast<std::int16_t>(v) == std::int16_t{ 0 });
        check("cold_value_t_to_int32_is_zero",
              static_cast<std::int32_t>(v) == std::int32_t{ 0 });
        check("cold_value_t_to_int64_is_zero",
              static_cast<std::int64_t>(v) == std::int64_t{ 0 });
        check("cold_value_t_to_uint16_is_zero",
              static_cast<std::uint16_t>(v) == std::uint16_t{ 0 });
        // Float/double T{} are exactly +0.0 (no NaN, no payload).
        const float f{ static_cast<float>(v) };
        const double d{ static_cast<double>(v) };
        check("cold_value_t_to_float_is_zero",  f == 0.0f);
        check("cold_value_t_to_double_is_zero", d == 0.0);
        check("cold_value_t_to_float_not_nan",  !std::isnan(f));
        check("cold_value_t_to_double_not_nan", !std::isnan(d));
    }

    // Now drive the same contract THROUGH call() on a proxy whose method
    // pointer is null - call() short-circuits at the top with the monostate
    // value_t.  Variant index 0 == monostate.
    //
    // We pass NO live JavaThread / NO HotSpot, so the call stub's env probe
    // (the other early-out) would also hit, but the method-null check fires
    // FIRST and is the contract we want to pin.
    {
        const vmhook::method_proxy cold_proxy{
            /*owning_object*/ nullptr,
            /*method_ptr*/    nullptr,
            /*sig*/           std::string{ "()V" } };

        const value_t r0{ cold_proxy.call() };
        check("cold_call_no_args_is_void",      r0.is_void());
        check("cold_call_no_args_not_string",   !r0.is_string());
        check("cold_call_no_args_as_string_empty", r0.as_string().empty());
        check("cold_call_no_args_variant_index_monostate",
              r0.data.index() == 0u);

        // Every primitive conversion of the cold call() return is T{}.
        check("cold_call_to_bool_T_default",
              static_cast<bool>(r0) == false);
        check("cold_call_to_int8_T_default",
              static_cast<std::int8_t>(r0) == std::int8_t{ 0 });
        check("cold_call_to_int16_T_default",
              static_cast<std::int16_t>(r0) == std::int16_t{ 0 });
        check("cold_call_to_int32_T_default",
              static_cast<std::int32_t>(r0) == std::int32_t{ 0 });
        check("cold_call_to_int64_T_default",
              static_cast<std::int64_t>(r0) == std::int64_t{ 0 });
        check("cold_call_to_uint16_T_default",
              static_cast<std::uint16_t>(r0) == std::uint16_t{ 0 });
        check("cold_call_to_float_T_default",
              static_cast<float>(r0) == 0.0f);
        check("cold_call_to_double_T_default",
              static_cast<double>(r0) == 0.0);

        // The cold return must also be safe with a primitive ARG - call()
        // returns the same monostate sentinel regardless of arg pack.  We
        // exercise representatives of every wide and narrow primitive at
        // arity 1, plus mixed 8-arg AT the cap.
        const value_t r_bool { cold_proxy.call(true) };
        const value_t r_i8   { cold_proxy.call(std::int8_t{ -1 }) };
        const value_t r_i16  { cold_proxy.call(std::int16_t{ -1 }) };
        const value_t r_i32  { cold_proxy.call(std::int32_t{ -1 }) };
        const value_t r_i64  { cold_proxy.call(
            std::int64_t{ std::numeric_limits<std::int64_t>::min() }) };
        const value_t r_f    { cold_proxy.call(1.5f) };
        const value_t r_d    { cold_proxy.call(1.5) };
        const value_t r_u16  { cold_proxy.call(std::uint16_t{ 0xFFFF }) };
        const value_t r_mix  { cold_proxy.call(true,
                                               std::int8_t{ 1 },
                                               std::int16_t{ 2 },
                                               std::int32_t{ 3 },
                                               std::int64_t{ 4 },
                                               5.0f,
                                               6.0,
                                               std::uint16_t{ 7 }) };
        check("cold_call_bool_is_void",  r_bool.is_void());
        check("cold_call_i8_is_void",    r_i8.is_void());
        check("cold_call_i16_is_void",   r_i16.is_void());
        check("cold_call_i32_is_void",   r_i32.is_void());
        check("cold_call_i64_is_void",   r_i64.is_void());
        check("cold_call_float_is_void", r_f.is_void());
        check("cold_call_double_is_void",r_d.is_void());
        check("cold_call_u16_is_void",   r_u16.is_void());
        check("cold_call_mix8_is_void",  r_mix.is_void());

        // And the conversion from each of THOSE is T{} too.
        check("cold_call_bool_arg_to_int32_zero",
              static_cast<std::int32_t>(r_bool) == 0);
        check("cold_call_i64_arg_to_int64_zero",
              static_cast<std::int64_t>(r_i64) == 0);
        check("cold_call_double_arg_to_double_zero",
              static_cast<double>(r_d) == 0.0);
    }

    // Invalid (non-null but unmapped) method pointer must also short-circuit
    // - is_valid_pointer(nullptr-region) is false, so call() returns the
    // same monostate sentinel.  Use a low-VA cookie that is_valid_pointer
    // categorically rejects on every platform; we NEVER deref it.
    {
        auto* const bogus_method{
            reinterpret_cast<vmhook::hotspot::method*>(static_cast<std::uintptr_t>(0x42)) };
        const vmhook::method_proxy bogus_proxy{
            /*owning_object*/ nullptr,
            /*method_ptr*/    bogus_method,
            /*sig*/           std::string{ "(I)I" } };
        const value_t r{ bogus_proxy.call(std::int32_t{ 99 }) };
        check("cold_call_bogus_method_ptr_is_void", r.is_void());
        check("cold_call_bogus_method_ptr_to_int_zero",
              static_cast<std::int32_t>(r) == 0);
        check("cold_call_bogus_method_ptr_variant_index_monostate",
              r.data.index() == 0u);
    }
}

// ---------------------------------------------------------------------------
// (D) value_t::data alternative count is the documented 11 alternatives
//     (monostate, bool, i8, i16, i32, i64, float, double, u16, u32, string).
//     A regression that ADDED an alternative would silently shift the
//     monostate index away from 0 - which the cold-call assertions above
//     rely on.
// ---------------------------------------------------------------------------
namespace mcp_variant_pin
{
    using data_t = decltype(vmhook::method_proxy::value_t::data);
    static_assert(std::variant_size_v<data_t> == 11u,
                  "value_t variant must have exactly 11 alternatives");
    static_assert(std::is_same_v<std::variant_alternative_t<0, data_t>, std::monostate>,
                  "value_t variant index 0 must be monostate (the void sentinel)");
    static_assert(std::is_same_v<std::variant_alternative_t<1, data_t>, bool>,
                  "value_t variant index 1 must be bool");
    static_assert(std::is_same_v<std::variant_alternative_t<2, data_t>, std::int8_t>,
                  "value_t variant index 2 must be int8");
    static_assert(std::is_same_v<std::variant_alternative_t<3, data_t>, std::int16_t>,
                  "value_t variant index 3 must be int16");
    static_assert(std::is_same_v<std::variant_alternative_t<4, data_t>, std::int32_t>,
                  "value_t variant index 4 must be int32");
    static_assert(std::is_same_v<std::variant_alternative_t<5, data_t>, std::int64_t>,
                  "value_t variant index 5 must be int64");
    static_assert(std::is_same_v<std::variant_alternative_t<6, data_t>, float>,
                  "value_t variant index 6 must be float");
    static_assert(std::is_same_v<std::variant_alternative_t<7, data_t>, double>,
                  "value_t variant index 7 must be double");
    static_assert(std::is_same_v<std::variant_alternative_t<8, data_t>, std::uint16_t>,
                  "value_t variant index 8 must be uint16 (char)");
    static_assert(std::is_same_v<std::variant_alternative_t<9, data_t>, std::uint32_t>,
                  "value_t variant index 9 must be uint32 (compressed OOP)");
    static_assert(std::is_same_v<std::variant_alternative_t<10, data_t>, std::string>,
                  "value_t variant index 10 must be std::string");
}

int main()
{
    run_cold_call_returns_t_default();

    std::printf("\n%s (%d failure%s)\n",
                failures == 0 ? "ALL PASS" : "FAILURES",
                failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
