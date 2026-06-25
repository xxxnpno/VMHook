// Standalone (no-JVM) unit test for vmhook::on_class_loaded's cold-state
// install/uninstall contract — the path where there is NO live HotSpot JVM
// in the process.  Source of truth lives in vmhook/ext/vmhook/vmhook.hpp
// around lines 21329-21492 (the registry declarations + the on_class_loaded
// template) and 21675-21687 (detail::reset_watcher_latches()).
//
// What is no-JVM-determinable here:
//
//   1. on_class_loaded() must NOT crash when called from a process with no
//      JVM attached.  The internal install path goes through
//      register_class<class_loader_wrapper>("java/lang/ClassLoader") +
//      vmhook::hook<>(...) on defineClass; with no live JVM that resolution
//      returns false, the optimistically-pushed callback is erased, and the
//      function hands back an empty watch_handle (running() == false).  This
//      is the inert-watcher branch documented at vmhook.hpp:21449-21466.
//
//   2. The empty handle's running() is false, stop() is a safe no-op, and
//      destroying / re-destroying it is safe — the move-only watch_handle
//      contract (vmhook.hpp:9126-9209) survives the cold-state path.
//
//   3. Repeated cold-state installs are idempotent: N back-to-back
//      on_class_loaded() calls each return an empty handle, none flip the
//      class_load_hook_installed latch, and the callback vector does NOT
//      grow without bound.  This is the load-bearing guarantee that lets
//      the live-JVM path eventually arm cleanly: a stale "installed=true" +
//      stale callback list left over from cold attempts would make the
//      FIRST live install skip the hook<>() call and silently lose every
//      callback.
//
//   4. Static-assert the public signature shape: on_class_loaded is a
//      template taking a callable of one std::string-compatible argument,
//      returning vmhook::watch_handle; watch_handle is move-only,
//      default-constructible, and its move ops + stop() + dtor are
//      noexcept.  Pinned at compile time so a future ABI refactor that
//      accidentally adds a copy ctor or drops noexcept fails this test
//      instead of CI surface-wide.
//
//   5. detail::reset_watcher_latches() is idempotent and clears BOTH the
//      class_load_hook_installed bool AND the callback vector (it also
//      touches the on_exception registry, which we observe).  Cold-state
//      these are already false/empty, so reset must remain a safe no-op.
//
// OUT OF SCOPE (needs a live JVM; the modular harness covers them):
//   - The ClassLoader.defineClass detour actually firing on a fresh class
//     definition.  See tests/jvm/modules/on_class_loaded.cpp.
//   - Multiple callbacks fan-in / snapshot semantics on a live install.
//   - The [HIGH] characterized flaw where class_load_hook_installed stays
//     true after shutdown_hooks() — that needs the detour to have armed.

#include <vmhook/vmhook.hpp>

#include <cstdio>
#include <functional>
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

// watch_handle is move-only, default-constructible, lifecycle is noexcept.
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

// The registry stores std::function<void(const std::string&)> — pin that.
// This is the public contract: the JVM-internal '/'-separated class name.
static_assert(std::is_same_v<vmhook::detail::class_load_callback_t,
                             std::function<void(const std::string&)>>,
              "class_load_callback_t signature is the public contract: "
              "void(const std::string&) — internal class name as '/'-separated.");

// The return type of on_class_loaded<>(F) MUST be watch_handle for any
// reasonable callable.  Resolve via decltype on a never-executed expression.
namespace
{
    auto sample_callback(const std::string&) -> void {}
    using sample_cb_t = decltype(&sample_callback);

    using on_class_loaded_return_t =
        decltype(vmhook::on_class_loaded(std::declval<sample_cb_t>()));
}
static_assert(std::is_same_v<on_class_loaded_return_t, vmhook::watch_handle>,
              "on_class_loaded<>(F) must return vmhook::watch_handle.");

// A stateless lambda matching the documented signature must be invocable —
// pins documentation against the implementation.
static_assert(std::is_invocable_v<
                  decltype([](const std::string&) noexcept {}),
                  const std::string&>,
              "documented stateless lambda must be invocable with (const std::string&)");

// ─────────────────────────────────────────────────────────────────────────
//  RUNTIME CONTRACT (cold-state install / uninstall idempotency)
// ─────────────────────────────────────────────────────────────────────────

int main()
{
    // A. Default-constructed watch_handle is inert.
    {
        vmhook::watch_handle empty{};
        check("A_default_handle_not_running", !empty.running());
        empty.stop();
        check("A_stop_idempotent_on_empty", !empty.running());
        empty.stop();
        check("A_double_stop_safe", !empty.running());
    }

    // B. Cold-state on_class_loaded() — no JVM present.  Must NOT crash;
    //    the install path's hook<>() returns false, the inert-watcher
    //    branch erases the optimistic callback push, and the returned
    //    handle is empty.
    {
        int call_count{ 0 };
        auto handle = vmhook::on_class_loaded(
            [&call_count](const std::string& /*name*/) noexcept
            {
                ++call_count;
            });

        check("B_cold_install_handle_not_running", !handle.running());
        check("B_cold_install_callback_never_fired", call_count == 0);
        // The install latch MUST NOT have flipped — no JVM means
        // java/lang/ClassLoader.defineClass cannot resolve.
        check("B_hook_install_latch_still_false",
              !vmhook::detail::class_load_hook_installed);
    }

    // C. Idempotency: 32 back-to-back cold-state installs do NOT grow the
    //    callback registry, do NOT flip the install latch, and every
    //    returned handle is inert.
    {
        const std::size_t before_size = vmhook::detail::class_load_callbacks.size();
        const bool        before_latch = vmhook::detail::class_load_hook_installed;

        constexpr int reinstalls{ 32 };
        std::vector<vmhook::watch_handle> handles{};
        handles.reserve(reinstalls);
        for (int i{ 0 }; i < reinstalls; ++i)
        {
            handles.emplace_back(vmhook::on_class_loaded(
                [](const std::string&) noexcept {}));
        }
        for (const auto& h : handles)
        {
            check("C_each_handle_inert", !h.running());
        }

        const std::size_t after_size = vmhook::detail::class_load_callbacks.size();
        const bool        after_latch = vmhook::detail::class_load_hook_installed;

        check("C_callback_registry_did_not_grow", after_size == before_size);
        check("C_install_latch_unchanged", after_latch == before_latch);
        check("C_install_latch_still_false", !after_latch);
    }

    // D. Move-out leaves the source inert; destroying the moved-in handle
    //    is safe even from the cold-state empty branch.
    {
        auto h1 = vmhook::on_class_loaded([](const std::string&) noexcept {});
        check("D_pre_move_source_inert", !h1.running());

        vmhook::watch_handle h2{ std::move(h1) };
        check("D_post_move_source_inert", !h1.running());  // moved-from
        check("D_post_move_dest_inert",   !h2.running());

        vmhook::watch_handle h3{};
        h3 = std::move(h2);
        check("D_move_assign_dest_inert", !h3.running());
        h3.stop();
        check("D_post_stop_inert", !h3.running());
    }

    // E. reset_watcher_latches() clears BOTH watcher registries (class-load
    //    and exception).  Cold-state both are already false/empty so this
    //    is observably a no-op, but must remain safe to call repeatedly.
    {
        vmhook::detail::reset_watcher_latches();
        check("E_reset_clears_class_load_latch",
              !vmhook::detail::class_load_hook_installed);
        check("E_reset_clears_class_load_callbacks",
              vmhook::detail::class_load_callbacks.empty());
        check("E_reset_clears_exception_latch",
              !vmhook::detail::exception_hook_installed);
        check("E_reset_clears_exception_callbacks",
              vmhook::detail::exception_callbacks.empty());

        // Idempotent: a second reset must also be safe.
        vmhook::detail::reset_watcher_latches();
        check("E_reset_idempotent_class_load_latch",
              !vmhook::detail::class_load_hook_installed);
        check("E_reset_idempotent_class_load_callbacks",
              vmhook::detail::class_load_callbacks.empty());

        // A third reset, just to be sure the helper is truly idempotent and
        // doesn't accumulate any per-call side effect.
        vmhook::detail::reset_watcher_latches();
        check("E_reset_third_call_still_clean",
              !vmhook::detail::class_load_hook_installed
              && vmhook::detail::class_load_callbacks.empty());
    }

    // F. After reset, a fresh cold-state on_class_loaded() still returns an
    //    inert handle — the install latch behaviour is reproducible across
    //    a reset boundary.
    {
        vmhook::detail::reset_watcher_latches();
        auto h = vmhook::on_class_loaded([](const std::string&) noexcept {});
        check("F_post_reset_handle_inert", !h.running());
        check("F_post_reset_latch_still_false",
              !vmhook::detail::class_load_hook_installed);
        // Cleanup branch ran — registry empty again.
        check("F_post_reset_callbacks_empty",
              vmhook::detail::class_load_callbacks.empty());
    }

    // G. noexcept characterization.  on_class_loaded itself is NOT noexcept
    //    (allocates a shared_ptr + pushes to a vector under a mutex); the
    //    handle lifecycle (stop, running) IS noexcept and is load-bearing.
    {
        using cb_t = void(*)(const std::string&);
        constexpr bool on_class_loaded_is_noexcept =
            noexcept(vmhook::on_class_loaded(std::declval<cb_t>()));
        info("G_on_class_loaded_is_noexcept", on_class_loaded_is_noexcept);

        constexpr bool watch_stop_is_noexcept =
            noexcept(std::declval<vmhook::watch_handle&>().stop());
        info("G_watch_handle_stop_is_noexcept", watch_stop_is_noexcept);
        static_assert(noexcept(std::declval<vmhook::watch_handle&>().stop()),
                      "watch_handle::stop() must be noexcept.");

        constexpr bool watch_running_is_noexcept =
            noexcept(std::declval<const vmhook::watch_handle&>().running());
        info("G_watch_handle_running_is_noexcept", watch_running_is_noexcept);
        static_assert(noexcept(std::declval<const vmhook::watch_handle&>().running()),
                      "watch_handle::running() must be noexcept.");

        // reset_watcher_latches is declared noexcept — assert hard.
        static_assert(noexcept(vmhook::detail::reset_watcher_latches()),
                      "reset_watcher_latches() must be noexcept (forward-decl says so).");
    }

    std::printf("\n%s — %d failure(s)\n",
                failures == 0 ? "[ALL PASSED]" : "[SOME FAILED]", failures);
    return failures == 0 ? 0 : 1;
}
