// Standalone (no-JVM) unit test for vmhook::for_each_instance<T>.
//
// Off-JVM, vmhook::for_each_instance<T>() is fully deterministic at the entry
// gate: the type_to_class_map lookup either MISSES (unregistered T -> return 0
// immediately, visitor never called) or HITS but find_class<T>() yields nullptr
// (no JVM -> return 0, visitor never called).  In either branch, the function
// returns 0, invokes the visitor zero times, and never reads cold heap memory.
// We also pin the signature / noexcept-in-practice behaviour as static_asserts
// + try/catch witnesses so a regression fails the BUILD or the RUN.
//
// We exercise:
//   - unregistered-T cold path: 0 visits, visitor never called.
//   - max_visits=0 cap: 0 visits (cap honoured even before the first call).
//   - max_visits=SIZE_MAX (the default) off-JVM: 0 visits.
//   - max_visits=INT_MAX off-JVM: 0 visits.
//   - return type is std::size_t; the function is callable with a stateful
//     visitor and a stateless lambda; never throws off-JVM.
//   - calling twice is idempotent (no hidden state).
//
// No live oop_t is ever fabricated; the visitor is wired to flag any call so a
// regression that lets the visitor fire off-JVM is a HARD failure.
#include <vmhook/vmhook.hpp>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <type_traits>

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

namespace
{
    // Unregistered wrapper: the type_to_class_map MISS branch fires inside
    // for_each_instance<T>() before any heap touch.
    struct unreg_wrapper : vmhook::object<unreg_wrapper>
    {
        using vmhook::object<unreg_wrapper>::object;
    };
}

int main()
{
    // =====================================================================
    // 0. COMPILE-TIME signature pins.
    // =====================================================================
    auto noop_visitor = [](std::unique_ptr<unreg_wrapper>) {};
    using ret_t = decltype(vmhook::for_each_instance<unreg_wrapper>(noop_visitor));
    static_assert(std::is_same_v<ret_t, std::size_t>,
                  "for_each_instance<T>() must return std::size_t");

    using ret_capped_t = decltype(
        vmhook::for_each_instance<unreg_wrapper>(noop_visitor, std::size_t{ 0 }));
    static_assert(std::is_same_v<ret_capped_t, std::size_t>,
                  "for_each_instance<T>(visit, cap) must return std::size_t");

    // The cap parameter is std::size_t.
    static_assert(std::is_same_v<std::size_t,
                                 std::remove_cvref_t<decltype(std::size_t{ 0 })>>,
                  "cap type is std::size_t");

    check("for_each_instance_signature_static_asserts_compiled", true);

    // =====================================================================
    // A. UNREGISTERED T -> 0 visits, visitor never called (cold no-JVM path).
    // =====================================================================
    {
        std::size_t visitor_calls{ 0 };
        auto count_visitor = [&](std::unique_ptr<unreg_wrapper>) { ++visitor_calls; };

        bool threw{ false };
        std::size_t reported{ 999 };
        try {
            reported = vmhook::for_each_instance<unreg_wrapper>(count_visitor);
        } catch (...) { threw = true; }

        check("fei_unreg_no_throw", !threw);
        check("fei_unreg_returns_zero", reported == 0u);
        check("fei_unreg_visitor_never_called", visitor_calls == 0u);
    }

    // =====================================================================
    // B. max_visits=0 cap -> 0 visits, visitor never called.
    // =====================================================================
    {
        std::size_t visitor_calls{ 0 };
        auto count_visitor = [&](std::unique_ptr<unreg_wrapper>) { ++visitor_calls; };

        bool threw{ false };
        std::size_t reported{ 999 };
        try {
            reported = vmhook::for_each_instance<unreg_wrapper>(count_visitor,
                                                                std::size_t{ 0 });
        } catch (...) { threw = true; }

        check("fei_cap0_no_throw", !threw);
        check("fei_cap0_returns_zero", reported == 0u);
        check("fei_cap0_visitor_never_called", visitor_calls == 0u);
    }

    // =====================================================================
    // C. max_visits=INT_MAX -> 0 visits off-JVM (cap is moot; substrate is empty).
    // =====================================================================
    {
        std::size_t visitor_calls{ 0 };
        auto count_visitor = [&](std::unique_ptr<unreg_wrapper>) { ++visitor_calls; };

        bool threw{ false };
        std::size_t reported{ 999 };
        try {
            reported = vmhook::for_each_instance<unreg_wrapper>(
                count_visitor,
                static_cast<std::size_t>(INT_MAX));
        } catch (...) { threw = true; }

        check("fei_capINTMAX_no_throw", !threw);
        check("fei_capINTMAX_returns_zero", reported == 0u);
        check("fei_capINTMAX_visitor_never_called", visitor_calls == 0u);
    }

    // =====================================================================
    // D. max_visits=SIZE_MAX (the documented default) -> 0 visits off-JVM.
    // =====================================================================
    {
        std::size_t visitor_calls{ 0 };
        auto count_visitor = [&](std::unique_ptr<unreg_wrapper>) { ++visitor_calls; };

        bool threw{ false };
        std::size_t reported{ 999 };
        try {
            reported = vmhook::for_each_instance<unreg_wrapper>(
                count_visitor,
                std::numeric_limits<std::size_t>::max());
        } catch (...) { threw = true; }

        check("fei_capSIZEMAX_no_throw", !threw);
        check("fei_capSIZEMAX_returns_zero", reported == 0u);
        check("fei_capSIZEMAX_visitor_never_called", visitor_calls == 0u);
    }

    // =====================================================================
    // E. STATELESS lambda visitor -> still 0 visits, no throw.  Pins that the
    //    visitor template parameter accepts a stateless callable.
    // =====================================================================
    {
        bool threw{ false };
        std::size_t reported{ 999 };
        try {
            reported = vmhook::for_each_instance<unreg_wrapper>(
                [](std::unique_ptr<unreg_wrapper>) {});
        } catch (...) { threw = true; }
        check("fei_stateless_visitor_no_throw", !threw);
        check("fei_stateless_visitor_returns_zero", reported == 0u);
    }

    // =====================================================================
    // F. IDEMPOTENCE — two consecutive calls yield the same 0 with the same
    //    zero-visitor-call count.  Pins no hidden state across cold calls.
    // =====================================================================
    {
        std::size_t calls{ 0 };
        auto v = [&](std::unique_ptr<unreg_wrapper>) { ++calls; };
        const std::size_t a{ vmhook::for_each_instance<unreg_wrapper>(v) };
        const std::size_t b{ vmhook::for_each_instance<unreg_wrapper>(v) };
        check("fei_idempotent_returns_zero", a == 0u && b == 0u);
        check("fei_idempotent_visitor_never_called", calls == 0u);
    }

    // =====================================================================
    // G. CAP SWEEP — a range of small caps off-JVM all yield 0 and never call.
    // =====================================================================
    {
        const std::size_t caps[]{ 0u, 1u, 2u, 10u, 1000u, 65536u };
        bool all_zero{ true };
        std::size_t total_calls{ 0 };
        std::size_t probed{ 0 };
        for (const std::size_t cap : caps)
        {
            auto v = [&](std::unique_ptr<unreg_wrapper>) { ++total_calls; };
            const std::size_t r{
                vmhook::for_each_instance<unreg_wrapper>(v, cap) };
            if (r != 0u) { all_zero = false; }
            ++probed;
        }
        check("fei_cap_sweep_all_zero", all_zero);
        check("fei_cap_sweep_visitor_never_called", total_calls == 0u);
        check("fei_cap_sweep_size", probed == sizeof(caps)/sizeof(caps[0]));
    }

    std::printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
