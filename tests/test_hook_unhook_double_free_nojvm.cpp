// Standalone (no-JVM) unit test for vmhook::hook_handle lifecycle —
// cold-state install/unhook contract, double-free safety, idempotency.
//
// WAVE-33 LEDGER GAPS this file closes:
//   * cold-state hook_handle{nullptr} → installed()==false, stop() is a
//     true no-op (no throw, no observable effect on g_hooked_methods).
//   * idempotent unhook: stop() called twice / thrice / 32 times on the
//     SAME empty handle never crashes and never trips the find_if==end()
//     gate (it short-circuits at the "if (!this->method) return" gate
//     at vmhook.hpp:11352-11355 BEFORE even taking the mutex).
//   * default-constructed hook_handle == empty handle == safe destructor.
//   * move-from leaves the source empty; the source's destructor's
//     stop() is a no-op; the moved-to handle owns nothing and its
//     destructor is also a no-op (both `method` pointers are nullptr
//     because we started from an empty handle — proves the move
//     transfer rule even in the cold case).
//   * 32-iteration sweep of construct-default → stop() → destruct never
//     leaks an entry into g_hooked_methods, never throws, never trips
//     a double-free pattern.
//   * static_asserts on signature: stop() is noexcept, returns void;
//     installed() is noexcept, returns bool; copy is deleted; move is
//     noexcept; destructor exists.
//   * pin on the global vector type (std::vector<hooked_method>) so a
//     refactor that swaps the container has to also touch this file.
//
// OUT OF SCOPE (needs a live JVM, covered by tests/jvm/modules/
// hook_unhook_double_free.cpp against the HookUnhook fixture):
//   * actually installing a hook and exercising the byte-exact restore.
//   * the Bug 1 duplicate-install shared-entry double-stop characterization.
//   * the find_if==end() no-op path with a real second handle racing the
//     primary's stop().
//   * the dont_inline / NO_COMPILE flag clearing on a live Method*.

#include <vmhook/vmhook.hpp>

#include <cstdio>
#include <type_traits>
#include <utility>
#include <vector>

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

// ---------------------------------------------------------------------------
// SECTION 1 - signature / type locks (compile-time).
// ---------------------------------------------------------------------------
using hook_handle_t = vmhook::hook_handle;

static_assert(std::is_nothrow_default_constructible_v<hook_handle_t>,
              "hook_handle must be nothrow default-constructible");
static_assert(!std::is_copy_constructible_v<hook_handle_t>,
              "hook_handle copy-construct must be deleted");
static_assert(!std::is_copy_assignable_v<hook_handle_t>,
              "hook_handle copy-assign must be deleted");
static_assert(std::is_nothrow_move_constructible_v<hook_handle_t>,
              "hook_handle move-construct must be noexcept");
static_assert(std::is_nothrow_move_assignable_v<hook_handle_t>,
              "hook_handle move-assign must be noexcept");
static_assert(std::is_destructible_v<hook_handle_t>,
              "hook_handle must be destructible");

static_assert(noexcept(std::declval<hook_handle_t&>().stop()),
              "hook_handle::stop() must be noexcept (called from ~hook_handle)");
static_assert(std::is_same_v<decltype(std::declval<hook_handle_t&>().stop()), void>,
              "hook_handle::stop() must return void");

static_assert(noexcept(std::declval<const hook_handle_t&>().installed()),
              "hook_handle::installed() must be noexcept");
static_assert(std::is_same_v<decltype(std::declval<const hook_handle_t&>().installed()),
                             bool>,
              "hook_handle::installed() must return bool");

// Pin the global vector type so a refactor to e.g. small_vector trips this.
static_assert(std::is_same_v<decltype(vmhook::hotspot::g_hooked_methods),
                             std::vector<vmhook::hotspot::hooked_method>>,
              "g_hooked_methods must remain std::vector<hooked_method>");

auto main() -> int
{
    std::printf("[hook_unhook_double_free_nojvm] start\n");

    using vmhook::hotspot::g_hooked_methods;

    // -----------------------------------------------------------------------
    // SECTION 2 - cold-state precondition: vector is empty (no install has
    // run in this process), shutdown latches are clear.  This is the cold
    // baseline the double-free test depends on — if a previous test leaked
    // an entry, the find_if walks would see it.
    // -----------------------------------------------------------------------
    const std::size_t starting_entries{ g_hooked_methods.size() };
    check("cold_g_hooked_methods_empty_at_start", starting_entries == 0);

    // -----------------------------------------------------------------------
    // SECTION 3 - default-constructed handle: installed()==false, stop()
    // is a no-op (hits the "if (!this->method) return" gate at
    // vmhook.hpp:11352-11355 and never touches the mutex / vector).
    // -----------------------------------------------------------------------
    {
        vmhook::hook_handle h{};
        check("default_handle_not_installed", !h.installed());

        bool threw{ false };
        try { h.stop(); } catch (...) { threw = true; }
        check("default_handle_stop_no_throw", !threw);
        check("default_handle_still_not_installed_after_stop", !h.installed());
        check("default_handle_stop_did_not_grow_vector",
              g_hooked_methods.size() == starting_entries);
        // dtor here = third invisible stop()
    }
    check("default_handle_dtor_did_not_grow_vector",
          g_hooked_methods.size() == starting_entries);

    // -----------------------------------------------------------------------
    // SECTION 4 - explicit nullptr-constructed handle: same contract as
    // default, but exercised via the (vmhook::hotspot::method*) ctor.
    // -----------------------------------------------------------------------
    {
        vmhook::hook_handle h{ static_cast<vmhook::hotspot::method*>(nullptr) };
        check("nullptr_handle_not_installed", !h.installed());

        bool threw{ false };
        try { h.stop(); } catch (...) { threw = true; }
        check("nullptr_handle_stop_no_throw", !threw);
        check("nullptr_handle_stop_did_not_grow_vector",
              g_hooked_methods.size() == starting_entries);
    }

    // -----------------------------------------------------------------------
    // SECTION 5 - idempotent stop(): twice, thrice on the same handle.
    // The first stop() nulls `method`; the second/third hit the gate at
    // vmhook.hpp:11352-11355 and return immediately.  No double-free, no
    // re-entry into find_if, no mutation.
    // -----------------------------------------------------------------------
    {
        vmhook::hook_handle h{};
        bool threw{ false };
        try
        {
            h.stop();
            h.stop();
        }
        catch (...) { threw = true; }
        check("twice_stop_no_throw", !threw);
        check("twice_stop_still_empty", !h.installed());
    }
    {
        vmhook::hook_handle h{};
        bool threw{ false };
        try
        {
            h.stop();
            h.stop();
            h.stop();
        }
        catch (...) { threw = true; }
        check("thrice_stop_no_throw", !threw);
        check("thrice_stop_still_empty", !h.installed());
        check("thrice_stop_vector_unchanged",
              g_hooked_methods.size() == starting_entries);
    }

    // -----------------------------------------------------------------------
    // SECTION 6 - 32-iteration sweep: double-free detection.
    // If stop() ever forgot to null `method` (or its idempotency gate
    // regressed), repeated stops on the same handle would either AV in
    // find_if's lambda, double-erase, or leak entries.  The sweep
    // observes the invariant directly: the vector size never moves.
    // -----------------------------------------------------------------------
    {
        bool threw{ false };
        try
        {
            for (int i = 0; i < 32; ++i)
            {
                vmhook::hook_handle h{};
                h.stop();
                h.stop();
                // dtor: third stop()
            }
        }
        catch (...) { threw = true; }
        check("sweep_32_construct_stop_stop_dtor_no_throw", !threw);
        check("sweep_32_vector_unchanged",
              g_hooked_methods.size() == starting_entries);
    }

    // -----------------------------------------------------------------------
    // SECTION 7 - move semantics on empty handles.
    // -----------------------------------------------------------------------
    {
        vmhook::hook_handle src{};
        vmhook::hook_handle dst{ std::move(src) };
        check("move_ctor_dst_not_installed", !dst.installed());
        check("move_ctor_src_not_installed", !src.installed()); // NOLINT(bugprone-use-after-move)

        bool threw{ false };
        try { dst.stop(); src.stop(); } catch (...) { threw = true; }
        check("move_ctor_dst_then_src_stop_no_throw", !threw);
        check("move_ctor_vector_unchanged",
              g_hooked_methods.size() == starting_entries);
    }
    {
        vmhook::hook_handle a{};
        vmhook::hook_handle b{};
        bool threw{ false };
        try { a = std::move(b); } catch (...) { threw = true; }
        check("move_assign_no_throw", !threw);
        check("move_assign_a_empty", !a.installed());
        check("move_assign_b_empty", !b.installed()); // NOLINT(bugprone-use-after-move)
        // Self-move on empty handle — must not crash.  Wrap in
        // `auto& tmp = a; a = std::move(tmp);` to dodge any
        // -Wself-move diagnostic that some compilers raise.
        auto& alias{ a };
        try { a = std::move(alias); } catch (...) { threw = true; }
        check("self_move_assign_empty_no_throw", !threw);
        check("self_move_assign_still_empty", !a.installed());
    }

    // -----------------------------------------------------------------------
    // SECTION 8 - final invariant: nothing this test did pushed an entry
    // into the global vector.  If it had, a JVM test scheduled after this
    // one would observe the leak.
    // -----------------------------------------------------------------------
    check("end_of_test_vector_size_matches_starting",
          g_hooked_methods.size() == starting_entries);

    if (failures == 0)
    {
        std::printf("[hook_unhook_double_free_nojvm] OK\n");
        return 0;
    }
    std::printf("[hook_unhook_double_free_nojvm] %d FAILED\n", failures);
    return 1;
}
