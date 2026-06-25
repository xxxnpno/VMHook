// Wave-31 no-JVM unit tests for method_proxy::call() in the COLD state
// (no live HotSpot), focused on the *Object-returning* contract:
// value_t -> std::unique_ptr<wrapper> conversion.
//
// LEDGER gaps closed:
//   * cold-state call() on an Object-returning proxy with a null Method*
//     short-circuits to value_t{ monostate }; conversion to unique_ptr<W>
//     of that monostate result is a NULL unique_ptr (never a fake wrapper).
//   * call() noexcept characterized for the unique_ptr return path.
//   * static_assert pinning the unique_ptr conversion return type.
//   * null `this->method` is safe to call() across all Object signature
//     shapes (L..;, [L..;, [[L..;).
//
// All checks are pure-trait / pure-guard-arm: no JVM, no decode, no JNI,
// platform-invariant on every (MinGW / MSVC / clang / Linux / macOS) cell.

#include <vmhook/vmhook.hpp>
#include <cstdio>
#include <memory>
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

// Minimal registered-style wrapper: derives from object_base so the
// unique_ptr branch's static_assert is satisfied.  No JVM contact — we
// only ever construct it via the cold guard-arm null path, which never
// reaches `new wrapper_type{ decoded }`.
struct test_object final : vmhook::object_base
{
    using vmhook::object_base::object_base;
};

int main()
{
    using proxy_t = vmhook::method_proxy;

    // -------------------------------------------------------------------------
    // static_assert: the unique_ptr<W> conversion of value_t is well-formed
    // and yields exactly std::unique_ptr<W>.  Pins the method-vs-field parity
    // header at compile time.
    // -------------------------------------------------------------------------
    static_assert(std::is_convertible_v<value_t, std::unique_ptr<test_object>>,
                  "value_t must be convertible to std::unique_ptr<W : object_base>");
    static_assert(std::is_same_v<
                      decltype(static_cast<std::unique_ptr<test_object>>(std::declval<value_t>())),
                      std::unique_ptr<test_object>>,
                  "static_cast<unique_ptr<W>>(value_t) must produce std::unique_ptr<W>");
    static_assert(noexcept(static_cast<std::unique_ptr<test_object>>(std::declval<value_t>())),
                  "value_t -> unique_ptr<W> conversion must be noexcept");
    check("static_assert_value_t_to_unique_ptr_wrapper", true);

    // static_assert: same checks for the void* alternative (the other
    // reference-decoding branch — used for arrays in production).
    static_assert(std::is_convertible_v<value_t, void*>,
                  "value_t must be convertible to void* (compressed-OOP decode path)");
    static_assert(noexcept(static_cast<void*>(std::declval<value_t>())),
                  "value_t -> void* conversion must be noexcept");
    check("static_assert_value_t_to_void_pointer", true);

    // call() return type and noexcept on Object-shaped arg/return combos.
    static_assert(std::is_same_v<
                      decltype(std::declval<const proxy_t&>().call()),
                      value_t>,
                  "call() must return method_proxy::value_t");
    static_assert(noexcept(std::declval<const proxy_t&>().call()),
                  "call() must be noexcept");
    static_assert(noexcept(std::declval<const proxy_t&>().call(std::int32_t{ 1 })),
                  "call(int) must be noexcept");
    check("static_assert_call_object_signatures", true);

    // -------------------------------------------------------------------------
    // COLD call() on a null Method* with an Object return signature: guard
    // fires before any decode, so the result is monostate.  Converting that
    // monostate to unique_ptr<W> must yield a *null* unique_ptr — never a
    // wrapper around a garbage instance.
    //
    // This is the regression wall for the call_jni truncated-handle flaw:
    // even on the buggy path, a null Method* never reaches the truncate, so
    // the wrapper conversion stays honest (null in, null out).
    // -------------------------------------------------------------------------
    {
        const vmhook::method_proxy proxy{ nullptr, nullptr,
                                          std::string{ "()Lvmhook/Fixture;" } };
        const value_t result{ proxy.call() };
        check("null_method_object_return_is_void", result.is_void());
        check("null_method_object_return_not_string", !result.is_string());

        std::unique_ptr<test_object> wrapper{ result };
        check("null_method_object_return_unique_ptr_null", wrapper == nullptr);

        // void* alternative on the same monostate is nullptr (not a truncated handle).
        void* const raw{ result };
        check("null_method_object_return_void_pointer_null", raw == nullptr);
    }

    // -------------------------------------------------------------------------
    // Array return descriptors (one-dim and two-dim) take the same guard
    // arm — the guard does not inspect the descriptor.
    // -------------------------------------------------------------------------
    {
        const vmhook::method_proxy p_arr1{ nullptr, nullptr,
                                           std::string{ "()[Lvmhook/Fixture;" } };
        const vmhook::method_proxy p_arr2{ nullptr, nullptr,
                                           std::string{ "()[[Lvmhook/Fixture;" } };
        const value_t r1{ p_arr1.call() };
        const value_t r2{ p_arr2.call() };
        check("null_method_array1_return_is_void", r1.is_void());
        check("null_method_array2_return_is_void", r2.is_void());

        std::unique_ptr<test_object> w1{ r1 };
        std::unique_ptr<test_object> w2{ r2 };
        check("null_method_array1_unique_ptr_null", w1 == nullptr);
        check("null_method_array2_unique_ptr_null", w2 == nullptr);
    }

    // -------------------------------------------------------------------------
    // Argument-bearing Object calls: arguments are perfect-forwarded and
    // never touched in the guard arm — confirm a representative cross-section
    // of arg types still produces a null unique_ptr without throwing.
    // -------------------------------------------------------------------------
    {
        const vmhook::method_proxy proxy{ nullptr, nullptr,
                                          std::string{ "(IJDLjava/lang/String;)Lvmhook/Fixture;" } };
        const value_t r{ proxy.call(std::int32_t{ -7 },
                                    std::int64_t{ 1LL << 40 },
                                    3.14,
                                    std::string{ "carry" }) };
        check("null_method_object_with_args_is_void", r.is_void());
        std::unique_ptr<test_object> wrapper{ r };
        check("null_method_object_with_args_unique_ptr_null", wrapper == nullptr);
    }

    // -------------------------------------------------------------------------
    // Stability: repeated cold calls on the same Object-returning proxy
    // always produce a null unique_ptr — never leaks a stale pointer through.
    // -------------------------------------------------------------------------
    {
        const vmhook::method_proxy proxy{ nullptr, nullptr,
                                          std::string{ "()Lvmhook/Fixture;" } };
        bool any_non_null{ false };
        for (int i{ 0 }; i < 64; ++i)
        {
            std::unique_ptr<test_object> w{ proxy.call() };
            if (w) { any_non_null = true; }
        }
        check("cold_call_loop_object_unique_ptr_always_null", !any_non_null);
    }

    // -------------------------------------------------------------------------
    // A directly-constructed monostate value_t (no call() involved) also
    // converts to a null unique_ptr — pins the variant-arm branch in
    // isolation from the call() guard.
    // -------------------------------------------------------------------------
    {
        const value_t direct{ };  // default-constructed -> monostate
        check("direct_monostate_value_t_is_void", direct.is_void());
        std::unique_ptr<test_object> wrapper{ direct };
        check("direct_monostate_to_unique_ptr_null", wrapper == nullptr);
    }

    // -------------------------------------------------------------------------
    // value_t holding a non-uint32 alternative (e.g. an int32) must NOT
    // synthesize a wrapper.  Pins the `else { return target_type{}; }` arm
    // of the unique_ptr branch.
    // -------------------------------------------------------------------------
    {
        value_t numeric{ };
        numeric.data = std::int32_t{ 42 };
        std::unique_ptr<test_object> wrapper{ numeric };
        check("numeric_value_t_to_unique_ptr_null", wrapper == nullptr);
        check("numeric_value_t_not_void", !numeric.is_void());
        check("numeric_value_t_not_string", !numeric.is_string());
    }

    std::printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
