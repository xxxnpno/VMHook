// Wave-30 no-JVM unit tests for method_proxy::call() in the COLD state
// (no live HotSpot in process) — focused on the std::string-returning contract.
//
// LEDGER gaps closed:
//   * cold-state call() on a String-returning proxy must return monostate (no
//     decode crash) — and as_string() on that result must be the empty string.
//   * call() is declared noexcept; static_assert the noexcept-ness on every
//     argument arity / type combo we care about (no JVM needed — pure trait).
//   * static_assert the return type of call(...) is exactly method_proxy::value_t.
//   * a method_proxy built with null `this->method` is safe to call(): the
//     guard at vmhook.hpp:17150 short-circuits to value_t{ monostate } before
//     touching any field.
//
// All assertions are deterministic and platform-invariant (no compiler/libc++
// traps): we only exercise the early-out guard arm in call() — no decode,
// no JNI, no oop walks — so the test is identical on MinGW/MSVC/clang/Linux/macOS.

#include <vmhook/vmhook.hpp>
#include <cstdio>
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

using value_t = vmhook::method_proxy::value_t;

int main()
{
    // -------------------------------------------------------------------------
    // static_assert: call() return type is exactly value_t, for every arg arity
    // we care about. Pinned at compile time so a future refactor that changes
    // the return type is caught BEFORE the runtime suite runs.
    // -------------------------------------------------------------------------
    using proxy_t = vmhook::method_proxy;
    static_assert(std::is_same_v<
                      decltype(std::declval<const proxy_t&>().call()),
                      value_t>,
                  "call() must return method_proxy::value_t (no args)");
    static_assert(std::is_same_v<
                      decltype(std::declval<const proxy_t&>().call(1)),
                      value_t>,
                  "call(int) must return method_proxy::value_t");
    static_assert(std::is_same_v<
                      decltype(std::declval<const proxy_t&>().call(1, 2.0, true)),
                      value_t>,
                  "call(int, double, bool) must return method_proxy::value_t");
    static_assert(std::is_same_v<
                      decltype(std::declval<const proxy_t&>().call(std::string{"x"})),
                      value_t>,
                  "call(std::string) must return method_proxy::value_t");
    check("static_assert_call_return_type_is_value_t", true);

    // -------------------------------------------------------------------------
    // static_assert: call() is noexcept on every plausible arg combo. This is
    // the documented contract (vmhook.hpp:17147: `auto call(...) const noexcept`)
    // and pinned here so a future de-noexcept regression fails the build.
    // -------------------------------------------------------------------------
    static_assert(noexcept(std::declval<const proxy_t&>().call()),
                  "call() must be noexcept (no args)");
    static_assert(noexcept(std::declval<const proxy_t&>().call(42)),
                  "call(int) must be noexcept");
    static_assert(noexcept(std::declval<const proxy_t&>().call(1, 2, 3, 4, 5, 6, 7, 8)),
                  "call(8 args) must be noexcept");
    static_assert(noexcept(std::declval<const proxy_t&>().call(1.5, 2.5f, true, std::int64_t{7})),
                  "call(mixed numerics) must be noexcept");
    check("static_assert_call_is_noexcept", true);

    // -------------------------------------------------------------------------
    // static_assert: as_string() is noexcept too — pinned at compile time so
    // the value_t extraction path the std::string-returning call relies on
    // never gains a throwing spec.
    // -------------------------------------------------------------------------
    static_assert(noexcept(std::declval<const value_t&>().as_string()),
                  "value_t::as_string() must be noexcept");
    static_assert(noexcept(std::declval<const value_t&>().is_void()),
                  "value_t::is_void() must be noexcept");
    static_assert(noexcept(std::declval<const value_t&>().is_string()),
                  "value_t::is_string() must be noexcept");
    static_assert(std::is_same_v<
                      decltype(std::declval<const value_t&>().as_string()),
                      std::string>,
                  "value_t::as_string() must return std::string by value");
    check("static_assert_value_t_extractors_noexcept", true);

    // -------------------------------------------------------------------------
    // COLD call() on a proxy built with a null Method* and a String return
    // signature: the guard at vmhook.hpp:17150 fires (method == nullptr ->
    // is_valid_pointer false), so call() returns value_t{ monostate } WITHOUT
    // touching the JVM and WITHOUT throwing.
    //
    // This is the regression wall for the headline truncation bug: even in
    // the cold state, the value_t we get back must NOT be an OOP handle
    // pretending to be a String, and as_string() on it must yield "".
    // -------------------------------------------------------------------------
    {
        const vmhook::method_proxy proxy{ nullptr, nullptr,
                                          std::string{ "()Ljava/lang/String;" } };
        const value_t result{ proxy.call() };
        check("null_method_call_string_return_is_void",      result.is_void());
        check("null_method_call_string_return_not_string",   !result.is_string());
        check("null_method_call_string_return_as_string_empty", result.as_string().empty());
        // Calling with arguments takes the same guarded path.
        const value_t result_args{ proxy.call(std::int32_t{ 42 }, std::string{ "x" }) };
        check("null_method_call_string_with_args_is_void",   result_args.is_void());
        check("null_method_call_string_with_args_as_string_empty",
              result_args.as_string().empty());
    }

    // -------------------------------------------------------------------------
    // COLD call() on a proxy whose signature returns an array, primitive, or
    // void all reach the same null-method early-out — pin that the guard does
    // NOT depend on the return descriptor (the field is consulted later).
    // -------------------------------------------------------------------------
    {
        const vmhook::method_proxy p_arr  { nullptr, nullptr, std::string{ "()[Ljava/lang/String;" } };
        const vmhook::method_proxy p_prim { nullptr, nullptr, std::string{ "()I" } };
        const vmhook::method_proxy p_void { nullptr, nullptr, std::string{ "()V" } };
        check("null_method_array_return_call_is_void",     p_arr.call().is_void());
        check("null_method_primitive_return_call_is_void", p_prim.call().is_void());
        check("null_method_void_return_call_is_void",      p_void.call().is_void());
        // Even on a non-string return type, as_string() of a monostate result is "".
        check("null_method_primitive_return_as_string_empty",
              p_prim.call().as_string().empty());
    }

    // -------------------------------------------------------------------------
    // Repeated cold call()s on the same proxy are stable: every invocation
    // yields the SAME monostate result (no hidden state accumulating), and
    // as_string() never changes. This is the cold-equivalent of the JVM
    // leak/stability loop in the integration module — proves the guard arm
    // is side-effect free.
    // -------------------------------------------------------------------------
    {
        const vmhook::method_proxy proxy{ nullptr, nullptr,
                                          std::string{ "()Ljava/lang/String;" } };
        std::size_t distinct_tags{ 0 };
        bool any_non_empty{ false };
        bool prev_void{ true };
        for (int i{ 0 }; i < 100; ++i)
        {
            const value_t r{ proxy.call() };
            if (!r.as_string().empty()) { any_non_empty = true; }
            const bool is_v{ r.is_void() };
            if (i == 0 || is_v != prev_void) { ++distinct_tags; }
            prev_void = is_v;
        }
        check("cold_call_loop_no_decoded_bytes_ever",  !any_non_empty);
        check("cold_call_loop_single_stable_tag",      distinct_tags == 1);
    }

    // -------------------------------------------------------------------------
    // call() handles a wide variety of argument types in the guard arm without
    // touching the args (perfect-forward then early-return). Pin runtime safety
    // for each forwarded type combo we already static_asserted noexcept on.
    // -------------------------------------------------------------------------
    {
        const vmhook::method_proxy proxy{ nullptr, nullptr,
                                          std::string{ "(IJ)Ljava/lang/String;" } };
        check("null_method_int_long_args_is_void",
              proxy.call(std::int32_t{ -1 }, std::int64_t{ 1LL << 40 }).is_void());
        check("null_method_double_arg_is_void",
              proxy.call(3.14159).is_void());
        check("null_method_eight_args_is_void",
              proxy.call(1, 2, 3, 4, 5, 6, 7, 8).is_void());
        check("null_method_string_arg_is_void",
              proxy.call(std::string{ "carry-on" }).is_void());
    }

    std::printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
