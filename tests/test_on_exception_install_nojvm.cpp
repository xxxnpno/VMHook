// Standalone (no-JVM) unit test for vmhook::on_exception's cold-state
// install/uninstall contract — exercising the path where there is NO live
// HotSpot JVM in the process.  Source of truth lives in
// vmhook/ext/vmhook/vmhook.hpp around lines 21494-21688 (the registry
// declarations + the on_exception template + reset_watcher_latches()).
//
// What is no-JVM-determinable here:
//
//   1. on_exception() must NOT crash when called from a process with no JVM
//      attached.  The internal vmhook::hook<throwable_wrapper>(...) install
//      goes through register_class -> find_class on java/lang/Throwable, and
//      with no live JVM that resolution returns null, hook install returns
//      false, the optimistically-pushed callback is erased, and the function
//      hands back an empty watch_handle (running() == false).  This is the
//      "inert-watcher parity" branch documented at vmhook.hpp:21625-21638.
//
//   2. The empty handle's running() is false, stop() is a safe no-op, and
//      destroying it does nothing observable — the move-only watch_handle
//      contract (vmhook.hpp:9126-9209) survives the cold-state path.
//
//   3. Repeated cold-state installs are idempotent: N back-to-back
//      on_exception() calls each return an empty handle, none of them flip
//      the exception_hook_installed latch, and the callback list does NOT
//      grow without bound (the inert path erases its own push).  This is the
//      load-bearing guarantee that lets the live-JVM path eventually arm
//      cleanly: a stale "installed=true" + stale callback list left over
//      from cold-state attempts would make the FIRST live install skip the
//      hook<>() call and silently lose every callback.
//
//   4. Static-assert the public signature shape: on_exception is a template
//      taking a callable of one std::string-compatible argument, returning a
//      vmhook::watch_handle; watch_handle is move-only (no copy ctor / copy
//      assign), default-constructible, and its move ops + stop() + dtor are
//      noexcept (vmhook.hpp:9135-9209).  These are pinned at compile time so
//      that any future ABI refactor that accidentally adds a copy ctor or
//      drops noexcept off stop() fails this test instead of CI surface-wide.
//
//   5. noexcept characterization: on_exception itself is NOT noexcept (it
//      allocates a shared_ptr + pushes to a vector under a mutex), but the
//      handle's lifecycle (stop, dtor, move) is.  We record the observed
//      noexcept-ness as INFO so a future tightening that marks on_exception
//      noexcept can be detected without hard-asserting the current relaxed
//      contract.
//
// OUT OF SCOPE (needs a live JVM; the modular harness covers them):
//   - The fillInStackTrace detour actually firing on a Throwable construction.
//   - Multiple callbacks fan-in / snapshot semantics on a live install.
//   - reset_watcher_latches() observable across a shutdown_hooks() round trip
//     (requires the hook detour to have been live).

#include <vmhook/vmhook.hpp>

#include <cstdio>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}
static auto info(const char* name, bool observed) -> void
{
    std::printf("[INFO] %s = %s\n", name, observed ? "true" : "false");
}

// ─────────────────────────────────────────────────────────────────────────
//  COMPILE-TIME CONTRACT (static_asserts on signature + watch_handle traits)
// ─────────────────────────────────────────────────────────────────────────

// watch_handle is move-only, default-constructible, and its lifecycle is
// noexcept.  These are the contract clients of on_exception() rely on to
// stash handles in containers / class members without surprise.
static_assert(std::is_default_constructible_v<vmhook::watch_handle>,
              "watch_handle must be default-constructible (the empty/inert handle).");
static_assert(!std::is_copy_constructible_v<vmhook::watch_handle>,
              "watch_handle must NOT be copy-constructible (move-only RAII).");
static_assert(!std::is_copy_assignable_v<vmhook::watch_handle>,
              "watch_handle must NOT be copy-assignable (move-only RAII).");
static_assert(std::is_nothrow_move_constructible_v<vmhook::watch_handle>,
              "watch_handle move ctor must be noexcept (containers rely on it).");
static_assert(std::is_nothrow_move_assignable_v<vmhook::watch_handle>,
              "watch_handle move-assign must be noexcept.");
static_assert(std::is_nothrow_destructible_v<vmhook::watch_handle>,
              "watch_handle dtor must be noexcept (it runs on_stop swallowing throws).");

// A callable shaped like the documented public callback signature must be
// accepted by on_exception<>.  We don't *call* the template here — we only
// pin the type that the registry stores: std::function<void(const std::string&)>.
static_assert(std::is_same_v<vmhook::detail::exception_callback_t,
                             std::function<void(const std::string&)>>,
              "exception_callback_t signature is the public contract: "
              "void(const std::string&) — internal class name as '/'-separated.");

// The return type of on_exception<>(F) MUST be watch_handle for any
// reasonable callable.  We resolve via decltype on a value-category-correct
// dummy invocation expression — never executed.
namespace
{
    auto sample_callback(const std::string&) -> void {}
    using sample_cb_t = decltype(&sample_callback);

    using on_exception_return_t =
        decltype(vmhook::on_exception(std::declval<sample_cb_t>()));
}
static_assert(std::is_same_v<on_exception_return_t, vmhook::watch_handle>,
              "on_exception<>(F) must return vmhook::watch_handle.");

// ─────────────────────────────────────────────────────────────────────────
//  RUNTIME CONTRACT (cold-state install / uninstall idempotency)
// ─────────────────────────────────────────────────────────────────────────

int main()
{
    // A. Default-constructed watch_handle is inert.
    {
        vmhook::watch_handle empty{};
        check("A_default_handle_not_running", !empty.running());
        // stop() on an empty handle is a documented no-op.
        empty.stop();
        check("A_stop_idempotent_on_empty", !empty.running());
        // Second stop is still safe.
        empty.stop();
        check("A_double_stop_safe", !empty.running());
    }

    // B. Cold-state on_exception() — no JVM present.  Must NOT crash; the
    //    install path's hook<>() call returns false, the inert-watcher
    //    branch erases the optimistic callback push, and the returned
    //    handle is empty.
    {
        int call_count{ 0 };
        auto handle = vmhook::on_exception(
            [&call_count](const std::string& /*name*/) noexcept
            {
                ++call_count;
            });

        // The cold-state path returns the empty handle.  We cannot
        // hard-assert exception_callbacks.empty() because the global
        // mutex/vector are shared across the process — another test in
        // the same binary could in principle leave state behind — but in
        // this single-TU test that never installs anything else, the
        // inert branch's erase is what must run.
        check("B_cold_install_handle_not_running", !handle.running());
        check("B_cold_install_callback_never_fired", call_count == 0);

        // Capture whether the install latched (it must NOT have, because
        // there is no JVM to resolve java/lang/Throwable).
        check("B_hook_install_latch_still_false",
              !vmhook::detail::exception_hook_installed);
    }

    // C. Idempotency: repeated cold-state installs do NOT grow the
    //    callback registry or flip the install latch.
    {
        const std::size_t before_size = vmhook::detail::exception_callbacks.size();
        const bool        before_latch = vmhook::detail::exception_hook_installed;

        constexpr int reinstalls{ 32 };
        std::vector<vmhook::watch_handle> handles{};
        handles.reserve(reinstalls);
        for (int i{ 0 }; i < reinstalls; ++i)
        {
            handles.emplace_back(vmhook::on_exception(
                [](const std::string&) noexcept {}));
        }
        for (const auto& h : handles)
        {
            check("C_each_handle_inert", !h.running());
        }

        const std::size_t after_size = vmhook::detail::exception_callbacks.size();
        const bool        after_latch = vmhook::detail::exception_hook_installed;

        check("C_callback_registry_did_not_grow", after_size == before_size);
        check("C_install_latch_unchanged", after_latch == before_latch);
        check("C_install_latch_still_false", !after_latch);
    }

    // D. Move-out leaves the source inert; destroying the moved-in handle
    //    is safe even from the cold-state empty branch.
    {
        auto h1 = vmhook::on_exception([](const std::string&) noexcept {});
        check("D_pre_move_source_inert", !h1.running());

        vmhook::watch_handle h2{ std::move(h1) };
        check("D_post_move_source_inert", !h1.running());  // moved-from
        check("D_post_move_dest_inert",   !h2.running());

        // Move-assign chain: empty <- empty stays inert and stop() is safe.
        vmhook::watch_handle h3{};
        h3 = std::move(h2);
        check("D_move_assign_dest_inert", !h3.running());
        h3.stop();
        check("D_post_stop_inert", !h3.running());
    }

    // E. Reset-latches helper: documented to clear both the install bool
    //    AND the callback vector.  Cold-state, both are already false/empty
    //    so this is observably a no-op but must remain safe to call.
    {
        vmhook::detail::reset_watcher_latches();
        check("E_reset_clears_latch",
              !vmhook::detail::exception_hook_installed);
        check("E_reset_clears_callbacks",
              vmhook::detail::exception_callbacks.empty());

        // Idempotent: a second reset must also be safe.
        vmhook::detail::reset_watcher_latches();
        check("E_reset_idempotent_latch",
              !vmhook::detail::exception_hook_installed);
        check("E_reset_idempotent_callbacks",
              vmhook::detail::exception_callbacks.empty());
    }

    // F. noexcept characterization — recorded as [INFO] not asserted, so a
    //    future tightening that marks on_exception noexcept doesn't reverse
    //    the meaning of a hard assertion here.
    {
        using cb_t = void(*)(const std::string&);
        constexpr bool on_exception_is_noexcept =
            noexcept(vmhook::on_exception(std::declval<cb_t>()));
        info("F_on_exception_is_noexcept", on_exception_is_noexcept);

        constexpr bool watch_stop_is_noexcept =
            noexcept(std::declval<vmhook::watch_handle&>().stop());
        info("F_watch_handle_stop_is_noexcept", watch_stop_is_noexcept);
        // stop() noexcept IS load-bearing — assert it hard via static_assert
        // mirror (the dtor calls it from a noexcept context).
        static_assert(noexcept(std::declval<vmhook::watch_handle&>().stop()),
                      "watch_handle::stop() must be noexcept.");

        constexpr bool watch_running_is_noexcept =
            noexcept(std::declval<const vmhook::watch_handle&>().running());
        info("F_watch_handle_running_is_noexcept", watch_running_is_noexcept);
        static_assert(noexcept(std::declval<const vmhook::watch_handle&>().running()),
                      "watch_handle::running() must be noexcept.");
    }

    std::printf("\n%s — %d failure(s)\n",
                failures == 0 ? "[ALL PASSED]" : "[SOME FAILED]", failures);
    return failures == 0 ? 0 : 1;
}
