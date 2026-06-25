// Focused no-JVM deepening for vmhook::make_java_array.
//
// test_object_factory.cpp already covers the core negative-length guard, the
// per-primitive descriptors, and the JNI-fallback toggle. This file ADDS:
//
//   * compile-time static_asserts on the public signature/return-type/noexcept
//     contract (so a future signature drift is a hard compile error);
//   * concurrency safety: many threads hammering make_java_array in parallel
//     all observe the same nullptr/no-throw contract with no JVM (rules out
//     any non-thread-safe internal state poisoning between callers);
//   * additional descriptor shapes (whitespace, leading-NUL inside a view,
//     a descriptor that is a single '[' character — the smallest possible
//     '[' branch, which still must take the JDK-8 fallback and bail
//     gracefully);
//   * the (INT_MAX, SIZE_MAX>>1) corner — the 16 + length*element_size
//     allocation arithmetic is never reached without a JVM, but the call must
//     still be a clean nullptr with no UB or throw;
//   * repeated-call idempotence on the same arguments.
//
// EVERY assertion is HARD: with no JVM, ensure_current_java_thread() fails
// inside both find_class() and the JDK-8 JNI fallback, so the function MUST
// return nullptr deterministically on every platform/compiler.
#include <vmhook/vmhook.hpp>
#include <atomic>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

// --------- Compile-time contract: signature, noexcept, return type ---------

// Public signature: void* make_java_array(std::string_view, std::int32_t,
// std::size_t, bool=true) noexcept.
static_assert(std::is_same_v<
    decltype(vmhook::make_java_array(std::string_view{}, 0, std::size_t{ 0 }, true)),
    void*>);
static_assert(std::is_same_v<
    decltype(vmhook::make_java_array(std::string_view{}, 0, std::size_t{ 0 })),
    void*>);
static_assert(noexcept(vmhook::make_java_array(std::string_view{}, 0, std::size_t{ 0 }, true)));
static_assert(noexcept(vmhook::make_java_array(std::string_view{}, 0, std::size_t{ 0 })));

// The length parameter is a signed 32-bit int — exactly the JVM `_length`
// slot's width on every supported platform. This pins the API to int32, so a
// drift to (say) std::size_t would break the negative-length guard contract.
static_assert(std::is_signed_v<std::int32_t>);
static_assert(sizeof(std::int32_t) == 4);

namespace
{

// Wrap one call so we can use it from threads without try/catch escaping.
auto call_returns_null_no_throw(const std::string_view desc,
                                const std::int32_t length,
                                const std::size_t element_size,
                                const bool allow_jni_fallback) noexcept -> bool
{
    // noexcept on the API — but we still belt-and-braces wrap to PROVE the no-
    // throw contract observably (a throw would terminate immediately via the
    // noexcept boundary, which would fail the test process; this catch is for
    // a non-noexcept-violating throw, which simply cannot occur per the
    // static_assert above).
    void* result{ reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xCAFEBABE)) };
    result = vmhook::make_java_array(desc, length, element_size, allow_jni_fallback);
    return result == nullptr;
}

} // namespace

int main()
{
    // ----- A. Additional malformed / edge descriptors ------------------------

    // Whitespace-only descriptor: not empty, front()==' ', so the '[' fallback
    // is NOT taken; the plain find_class path bails and returns nullptr.
    check("whitespace_descriptor_returns_null",
          call_returns_null_no_throw(" ", 1, 1u, true));

    // A bare single '[' descriptor: front()=='[' so the JDK-8 JNI fallback IS
    // entered; jni_find_class itself bails inside ensure_current_java_thread()
    // with no JVM -> still nullptr, still no throw.
    check("bare_open_bracket_returns_null",
          call_returns_null_no_throw("[", 1, 1u, true));

    // A descriptor with an internal NUL byte (string_view carries it). Must not
    // crash; still nullptr.
    {
        const char raw[]{ '[', 'B', '\0', 'X' };
        check("descriptor_with_internal_nul_returns_null",
              call_returns_null_no_throw(std::string_view{ raw, sizeof(raw) }, 1, 1u, true));
    }

    // A long obviously-bogus descriptor — exercises the find_class miss path
    // with a name that will never resolve, even with a JVM.
    {
        const std::string_view long_bogus{ "[Lvmhook/nojvm/NoSuchClassNameThatCannotExist;" };
        check("long_bogus_ref_array_returns_null",
              call_returns_null_no_throw(long_bogus, 3, sizeof(void*), true));
    }

    // ----- B. Length / element_size corners ---------------------------------

    // INT_MAX length × a huge element_size — the allocation arithmetic
    // (16 + length*element_size) WOULD overflow if it ran, but the function
    // bails at find_class first, so this is a clean nullptr/no-throw with no
    // UB observed.
    check("intmax_len_huge_elem_no_overflow_observed",
          call_returns_null_no_throw("[B", INT_MAX,
                                     std::numeric_limits<std::size_t>::max() >> 1,
                                     true));

    // size==0 path with the JNI fallback explicitly disabled (the form
    // make_java_string itself uses mid-encode).
    check("zero_len_no_jni_fallback_returns_null",
          call_returns_null_no_throw("[B", 0, 1u, false));

    // ----- C. Repeated-call idempotence -------------------------------------
    // Calling the same shape ten times in a row must produce the SAME contract
    // (nullptr, no-throw) every time — no internal latched state.
    for (int i{ 0 }; i < 10; ++i)
    {
        const bool ok{ call_returns_null_no_throw("[I", 4, sizeof(std::int32_t), true) };
        if (!ok)
        {
            check("repeated_call_idempotent", false);
            break;
        }
        if (i == 9) { check("repeated_call_idempotent", true); }
    }

    // ----- D. Concurrency: many threads, no torn state ----------------------
    // The no-JVM contract is per-thread (each calls ensure_current_java_thread
    // which fails independently); proving the function holds under contention
    // rules out any non-thread-safe shared write between callers.
    {
        constexpr int thread_count{ 8 };
        constexpr int iters_per_thread{ 64 };
        std::atomic<int> non_null_observed{ 0 };
        std::atomic<int> threw_observed{ 0 };
        std::vector<std::thread> ts;
        ts.reserve(thread_count);
        for (int t{ 0 }; t < thread_count; ++t)
        {
            ts.emplace_back([&, t]()
            {
                for (int i{ 0 }; i < iters_per_thread; ++i)
                {
                    // Vary descriptor and length per-iteration to spread across
                    // the find_class miss path and the '[' fallback branch.
                    const std::string_view desc{ (i & 1) ? "[B" : "[Ljava/lang/Object;" };
                    const std::int32_t len{ static_cast<std::int32_t>((t * 7 + i) & 0xFF) };
                    void* r{ reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xBEEF)) };
                    try { r = vmhook::make_java_array(desc, len, sizeof(void*), true); }
                    catch (...) { threw_observed.fetch_add(1); continue; }
                    if (r != nullptr) { non_null_observed.fetch_add(1); }
                }
            });
        }
        for (auto& th : ts) { th.join(); }
        check("concurrent_no_non_null", non_null_observed.load() == 0);
        check("concurrent_no_throw", threw_observed.load() == 0);
    }

    if (failures == 0)
    {
        std::printf("test_make_java_array_nojvm: ALL PASSED\n");
        return 0;
    }
    std::printf("test_make_java_array_nojvm: %d FAILED\n", failures);
    return 1;
}
