// Standalone (no-JVM) characterization of method_proxy::call()'s VOID-return
// contract and the value_t::is_void()/is_string()/monostate-default safety net.
//
// SCOPE (ledger gap): cold-state void-return call() safe no-op; static_assert
// void return; noexcept characterized; idempotent.  All assertions execute
// without a live JVM (gHotSpotVMStructs is null in this binary): the bits we
// pin are the variant classification + the cold-entry guard at the top of
// method_proxy::call() that returns monostate when this->method is null /
// invalid, BEFORE any HotSpot pointer is dereferenced.

#include <vmhook/vmhook.hpp>

#include <cstdio>
#include <cstdint>
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

namespace mcrv_nojvm
{
    using value_t = vmhook::method_proxy::value_t;

    // ---- (A) static_assert: value_t's VOID alternative is std::monostate ----
    // The variant's first alternative (index 0) is monostate and a
    // default-constructed value_t holds it -> is_void() true at compile time
    // in spirit (the body is constexpr-friendly but is_void() itself is not
    // marked constexpr, so we pin via the variant traits instead).
    static_assert(std::is_same_v<
                      std::variant_alternative_t<0, decltype(std::declval<value_t&>().data)>,
                      std::monostate>,
                  "value_t's first alternative must be std::monostate (the void/failure sentinel)");
    static_assert(std::is_default_constructible_v<value_t>,
                  "value_t must be default-constructible (the void/failure sentinel)");
    static_assert(std::is_nothrow_default_constructible_v<value_t>,
                  "value_t default-ctor must be noexcept");

    // ---- (B) is_void() / is_string() / as_string() are noexcept --------------
    // The void-return contract demands these never throw on any alternative;
    // the type-system pin here means a regression (someone adds a throwing
    // path) breaks the build.
    static_assert(noexcept(std::declval<const value_t&>().is_void()),
                  "value_t::is_void() must be noexcept");
    static_assert(noexcept(std::declval<const value_t&>().is_string()),
                  "value_t::is_string() must be noexcept");
    static_assert(noexcept(std::declval<const value_t&>().as_string()),
                  "value_t::as_string() must be noexcept");

    // ---- (C) method_proxy::call() itself is noexcept on every arity ---------
    // The top-of-call cold guard `if (!this->method ...) return monostate;`
    // only honours its safety contract if call() is noexcept; pin it.
    static_assert(noexcept(std::declval<const vmhook::method_proxy&>().call()),
                  "method_proxy::call() must be noexcept (0 args)");
    static_assert(noexcept(std::declval<const vmhook::method_proxy&>().call(1)),
                  "method_proxy::call(int) must be noexcept");
    static_assert(noexcept(std::declval<const vmhook::method_proxy&>().call(
                      std::int64_t{}, double{}, 1, 2, 3, 4, 5, 6)),
                  "method_proxy::call(8 args incl wide) must be noexcept");

    // ---- (D) Runtime: default value_t is the VOID alternative ---------------
    inline auto run_default_is_void() -> void
    {
        value_t v{};
        check("default_v_is_void", v.is_void());
        check("default_v_not_string", !v.is_string());
        check("default_v_as_string_empty", v.as_string().empty());
        // Conversion of a monostate to a numeric target returns a
        // default-constructed value (0).  This is the documented behaviour
        // that motivates the is_void() introspection (you cannot distinguish
        // a real 0 from a void return without it).
        const std::int32_t as_i{ static_cast<std::int32_t>(v) };
        check("monostate_to_int_is_zero", as_i == 0);
        const double as_d{ static_cast<double>(v) };
        check("monostate_to_double_is_zero", as_d == 0.0);
        const bool as_b{ static_cast<bool>(v) };
        check("monostate_to_bool_is_false", as_b == false);
    }

    // ---- (E) Cold-state call() on a method-less proxy is a safe no-op -------
    // Construct a proxy with method=nullptr; call()'s very first check is
    // `if (!this->method ...) return value_t{monostate};` -> no HotSpot
    // pointer is dereferenced.  The probe is the safest possible witness of
    // the void-return cold-guard.
    inline auto run_cold_call_is_safe_noop() -> void
    {
        vmhook::method_proxy proxy{
            /*owning_object*/ nullptr,
            /*method_pointer*/ nullptr,
            /*signature*/ std::string{ "()V" }
        };
        const value_t r0{ proxy.call() };
        check("cold_call_no_args_is_void", r0.is_void());
        check("cold_call_no_args_not_string", !r0.is_string());
        check("cold_call_no_args_as_string_empty", r0.as_string().empty());
        check("cold_call_no_args_to_int_zero",
              static_cast<std::int32_t>(r0) == 0);

        // IDEMPOTENT: every invocation must yield the same monostate result -
        // no per-call state corruption, no transition out of the cold guard.
        for (int i{ 0 }; i < 8; ++i)
        {
            const value_t ri{ proxy.call() };
            check("cold_call_idempotent_void", ri.is_void());
            check("cold_call_idempotent_not_string", !ri.is_string());
        }

        // Cold guard fires regardless of arity (every overload of call()
        // shares the same `if (!this->method ...)` head).
        const value_t r1{ proxy.call(123) };
        check("cold_call_1_arg_is_void", r1.is_void());
        const value_t r8{ proxy.call(std::int64_t{ 1 }, double{ 2.0 },
                                     3, 4, 5, 6, 7, 8) };
        check("cold_call_8_args_is_void", r8.is_void());

        // String arg path: even the at-cap String marshalling must short-
        // circuit at the cold guard without touching a JVM.
        const value_t rs{ proxy.call(std::string{ "x" }) };
        check("cold_call_string_arg_is_void", rs.is_void());
    }

    // ---- (F) Explicit monostate construction round-trips through is_void ---
    // Pins the documented constructor used internally by call() when it
    // returns a void result: `value_t{ std::monostate{} }`.
    inline auto run_explicit_monostate_ctor() -> void
    {
        value_t v{ std::monostate{} };
        check("explicit_monostate_is_void", v.is_void());
        check("explicit_monostate_not_string", !v.is_string());
        check("explicit_monostate_as_string_empty", v.as_string().empty());
    }

    // ---- (G) value_t with a NON-monostate alternative is NOT void -----------
    // Contrast: an int64-bearing value_t (the encoding call() would use for
    // a J-returning method) must report is_void()==false, otherwise a caller
    // cannot distinguish 0 from void.  Same for string and bool.
    inline auto run_contrast_not_void() -> void
    {
        {
            value_t v{};
            v.data = static_cast<std::int64_t>(0);
            check("int64_zero_not_void", !v.is_void());
            check("int64_zero_to_int_zero",
                  static_cast<std::int64_t>(v) == 0);
        }
        {
            value_t v{};
            v.data = std::string{};
            check("empty_string_not_void", !v.is_void());
            check("empty_string_is_string", v.is_string());
            check("empty_string_as_string_empty", v.as_string().empty());
        }
        {
            value_t v{};
            v.data = false;
            check("bool_false_not_void", !v.is_void());
            check("bool_false_convert", static_cast<bool>(v) == false);
        }
    }

    inline auto run_all() -> void
    {
        run_default_is_void();
        run_cold_call_is_safe_noop();
        run_explicit_monostate_ctor();
        run_contrast_not_void();
    }
}

int main()
{
    mcrv_nojvm::run_all();
    std::printf("\n%s (%d failure%s)\n",
                failures == 0 ? "ALL PASS" : "FAILURES",
                failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
