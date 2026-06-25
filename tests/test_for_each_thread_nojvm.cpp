// Standalone unit test: for_each_thread cold-state safety + type/contract locks
// (no JVM).
//
// WAVE-29 LEDGER GAPS this file closes:
//   * cold-state visitor never invoked - without a bootstrapped JVM, neither
//     find_any_java_thread() (Path 1) nor the SMR ThreadsList entries (Path 2)
//     resolve, so vmhook::for_each_thread MUST return without calling the
//     visitor even once.
//   * thread_info struct field types pinned via static_asserts: thread is a
//     vmhook::hotspot::java_thread*, state is the java_thread_state enum (int8),
//     os_thread_id is vmhook::os::thread_id_t (DWORD on Windows, uint64_t on
//     POSIX).  Default-constructed values are nullptr / _thread_uninitialized
//     / 0.
//   * visitor noexcept/non-noexcept characterized: for_each_thread itself is
//     NOT noexcept (the docstring guarantees "callback exceptions propagate;
//     iteration stops at the throwing visit") - lock this on the signature.
//   * 32 cold calls stability - repeated invocation never invokes the visitor
//     and never throws; idempotent on a non-bootstrapped process.
//
// OUT OF SCOPE (needs a live JVM, covered by tests/jvm/modules/for_each_thread):
//   * actual enumeration of live JavaThreads, pointer dedup, lifecycle delta
//     of a freshly-spawned worker thread, OSThread decode.

#include <vmhook/vmhook.hpp>

#include <cstdio>
#include <cstdint>
#include <type_traits>
#include <utility>

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

// ---------------------------------------------------------------------------
// SECTION 1 - thread_info field type locks (compile-time).
// The struct is the public contract handed to every visitor; future refactors
// must not silently widen / rename / re-type these fields.
// ---------------------------------------------------------------------------

static_assert(std::is_same_v<decltype(vmhook::thread_info::thread),
                             vmhook::hotspot::java_thread*>,
              "thread_info::thread must be vmhook::hotspot::java_thread*");
static_assert(std::is_same_v<decltype(vmhook::thread_info::state),
                             vmhook::hotspot::java_thread_state>,
              "thread_info::state must be vmhook::hotspot::java_thread_state");
static_assert(std::is_same_v<decltype(vmhook::thread_info::os_thread_id),
                             vmhook::os::thread_id_t>,
              "thread_info::os_thread_id must be vmhook::os::thread_id_t");

// java_thread_state is an int8 enum class - the layout that get_thread_state()
// writes through a reinterpret_cast<java_thread_state*>.  Widening it would
// corrupt the next adjacent JavaThread field.
static_assert(std::is_same_v<std::underlying_type_t<vmhook::hotspot::java_thread_state>,
                             std::int8_t>,
              "java_thread_state underlying type must be int8_t");

// thread_info must be trivially constructible / copyable - the for_each_thread
// implementation default-constructs one per visit (`thread_info info{};`) and
// passes it by const ref, so any non-trivial init would silently regress hot
// loops.
static_assert(std::is_default_constructible_v<vmhook::thread_info>,
              "thread_info must be default-constructible");
static_assert(std::is_trivially_copyable_v<vmhook::thread_info>,
              "thread_info must be trivially copyable");

// ---------------------------------------------------------------------------
// SECTION 2 - for_each_thread is NOT noexcept.
// The contract explicitly allows visitor exceptions to propagate ("iteration
// stops at the throwing visit").  A future refactor that adds a try/catch
// around the visit call would silently swallow that exception - lock the
// noexcept(false) signature.
// ---------------------------------------------------------------------------

static_assert(!noexcept(vmhook::for_each_thread([](const vmhook::thread_info&) noexcept {})),
              "for_each_thread must NOT be noexcept - visitor exceptions propagate");

// ---------------------------------------------------------------------------
// SECTION 3 - default-constructed thread_info values.
// ---------------------------------------------------------------------------

static auto test_default_thread_info() -> void
{
    vmhook::thread_info info{};
    check("default thread_info.thread is nullptr", info.thread == nullptr);
    check("default thread_info.state is _thread_uninitialized",
          info.state == vmhook::hotspot::java_thread_state::_thread_uninitialized);
    check("default thread_info.os_thread_id is 0",
          info.os_thread_id == vmhook::os::thread_id_t{ 0 });
}

// ---------------------------------------------------------------------------
// SECTION 4 - cold-state visitor never invoked.
// Without a bootstrapped JVM, both walk paths short-circuit:
//   Path 1: find_any_java_thread() returns nullptr (no Threads::_thread_list
//           vm_struct entry resolved).
//   Path 2: iterate_struct_entries("ThreadsSMRSupport", ...) returns nullptr.
// The visitor MUST NOT fire even once.
// ---------------------------------------------------------------------------

static auto test_cold_visitor_never_invoked() -> void
{
    int visit_count{ 0 };
    vmhook::for_each_thread([&](const vmhook::thread_info&) noexcept
    {
        ++visit_count;
    });
    check("cold for_each_thread does not invoke visitor", visit_count == 0);
}

// ---------------------------------------------------------------------------
// SECTION 5 - 32 cold calls stability (repeated invocation is idempotent,
// never throws, never invokes the visitor).
// ---------------------------------------------------------------------------

static auto test_32_cold_calls_stable() -> void
{
    int total_visits{ 0 };
    bool threw{ false };
    try
    {
        for (int i{ 0 }; i < 32; ++i)
        {
            vmhook::for_each_thread([&](const vmhook::thread_info&) noexcept
            {
                ++total_visits;
            });
        }
    }
    catch (...)
    {
        threw = true;
    }
    check("32 cold for_each_thread calls do not throw", !threw);
    check("32 cold for_each_thread calls invoke visitor zero times",
          total_visits == 0);
}

// ---------------------------------------------------------------------------
// SECTION 6 - non-noexcept visitor accepted (template deduces fine).
// for_each_thread accepts ANY callable - a throwing visitor must compile and
// (cold-state) is simply never called, so nothing throws.
// ---------------------------------------------------------------------------

static auto test_throwing_visitor_compiles() -> void
{
    bool threw{ false };
    try
    {
        vmhook::for_each_thread([&](const vmhook::thread_info&) -> void
        {
            // Cold state - never reached - but the body is non-noexcept and
            // could throw, which the template must accept.
            throw 42;
        });
    }
    catch (...)
    {
        threw = true;
    }
    check("throwing visitor compiles and does not fire cold", !threw);
}

auto main() -> int
{
    test_default_thread_info();
    test_cold_visitor_never_invoked();
    test_32_cold_calls_stable();
    test_throwing_visitor_compiles();

    std::printf("\n%s: %d failure(s)\n",
                failures == 0 ? "OK" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
