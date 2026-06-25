// Standalone (no-JVM) unit test for vmhook::watch_static_field's
// cold-state install/uninstall contract — exercising the path where there
// is NO live HotSpot JVM in the process.  Source of truth lives in
// vmhook/ext/vmhook/vmhook.hpp around lines 21195-21325 (the template +
// the empty-handle fallbacks) and 9126-9209 (watch_handle).
//
// What is no-JVM-determinable here:
//
//   1. watch_static_field<W, T>() must NOT crash with no JVM attached.
//      Whether the platform supports hardware data breakpoints or not:
//        - On platforms WITHOUT support (VMHOOK_HAS_HW_DATA_BREAKPOINTS==0,
//          i.e. anything but Windows x86_64), the function unconditionally
//          returns an empty watch_handle — documented at vmhook.hpp:21317.
//        - On Windows x86_64 (==1), the resolution path runs
//          object_base::get_field which, with no live JVM, resolves the
//          klass to nullptr; get_field returns std::nullopt; the function
//          returns an empty watch_handle via the "field not found" arm.
//      Either way, the returned handle's running() is false.
//
//   2. The empty handle's stop() is a safe no-op and destroying it does
//      nothing observable — the watch_handle move-only RAII contract
//      (vmhook.hpp:9126-9209) survives the cold-state path.
//
//   3. Repeated cold-state installs are idempotent — N back-to-back
//      watch_static_field calls each return an empty handle.  No global
//      DR slot state changes (none of the four DR0-DR3 hardware slots get
//      consumed because we never reach dr_arm_one()).
//
//   4. Static-assert the public signature shape: watch_static_field is a
//      template returning vmhook::watch_handle, taking a std::string_view
//      field name and a callable of (field_type, field_type) — pinned at
//      compile time.
//
//   5. The VMHOOK_HAS_HW_DATA_BREAKPOINTS capability macro is exposed and
//      defined to exactly 0 or 1 — recorded as [INFO] (platform-variant)
//      and HARD-asserted as a known integral value.
//
// OUT OF SCOPE (needs a live JVM and Windows x86_64):
//   - The DR0-DR3 slot allocation actually firing on a putstatic write.
//   - The "all 4 slots in use" capacity refusal arm.
//   - Slot-freeing on watch_handle destruction.

#include <vmhook/vmhook.hpp>

#include <cstdio>
#include <cstdint>
#include <string>
#include <string_view>
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
static auto info_int(const char* name, long long observed) -> void
{
    std::printf("[INFO] %s = %lld\n", name, observed);
}

// ─────────────────────────────────────────────────────────────────────────
// Local unregistered wrapper.  Without register_class<>(), the internal
// resolve_klass(typeid(...)) returns nullptr, so object_base::get_field
// returns std::nullopt — driving watch_static_field into its
// "field not found" empty-handle arm on supported platforms.
// ─────────────────────────────────────────────────────────────────────────
struct cold_wrapper : public vmhook::object<cold_wrapper>
{
    using vmhook::object<cold_wrapper>::object;
};

// ─────────────────────────────────────────────────────────────────────────
// COMPILE-TIME CONTRACT (static_asserts on signature + capability macro)
// ─────────────────────────────────────────────────────────────────────────

// watch_handle traits (same lock as on_exception nojvm).
static_assert(std::is_default_constructible_v<vmhook::watch_handle>,
              "watch_handle must be default-constructible (the empty/inert handle).");
static_assert(!std::is_copy_constructible_v<vmhook::watch_handle>,
              "watch_handle must NOT be copy-constructible (move-only RAII).");
static_assert(!std::is_copy_assignable_v<vmhook::watch_handle>,
              "watch_handle must NOT be copy-assignable (move-only RAII).");
static_assert(std::is_nothrow_move_constructible_v<vmhook::watch_handle>,
              "watch_handle move ctor must be noexcept.");
static_assert(std::is_nothrow_move_assignable_v<vmhook::watch_handle>,
              "watch_handle move-assign must be noexcept.");
static_assert(std::is_nothrow_destructible_v<vmhook::watch_handle>,
              "watch_handle dtor must be noexcept.");
static_assert(noexcept(std::declval<vmhook::watch_handle&>().stop()),
              "watch_handle::stop() must be noexcept (dtor calls it).");
static_assert(noexcept(std::declval<const vmhook::watch_handle&>().running()),
              "watch_handle::running() must be noexcept.");

// Capability macro must be defined and 0 or 1.
static_assert(VMHOOK_HAS_HW_DATA_BREAKPOINTS == 0 || VMHOOK_HAS_HW_DATA_BREAKPOINTS == 1,
              "VMHOOK_HAS_HW_DATA_BREAKPOINTS must be defined as 0 or 1.");

// watch_static_field<W, T, F>(string_view, F) must return vmhook::watch_handle.
// Resolve via decltype on a never-executed expression for each primitive
// width the DR-LEN encoder selects between (1/2/4/8 bytes).
namespace
{
    auto cb_i32(std::int32_t, std::int32_t) -> void {}
    auto cb_i64(std::int64_t, std::int64_t) -> void {}
    auto cb_i16(std::int16_t, std::int16_t) -> void {}
    auto cb_i8 (std::int8_t,  std::int8_t)  -> void {}

    using wsf_i32_t = decltype(vmhook::watch_static_field<cold_wrapper, std::int32_t>(
        std::declval<std::string_view>(), &cb_i32));
    using wsf_i64_t = decltype(vmhook::watch_static_field<cold_wrapper, std::int64_t>(
        std::declval<std::string_view>(), &cb_i64));
    using wsf_i16_t = decltype(vmhook::watch_static_field<cold_wrapper, std::int16_t>(
        std::declval<std::string_view>(), &cb_i16));
    using wsf_i8_t  = decltype(vmhook::watch_static_field<cold_wrapper, std::int8_t>(
        std::declval<std::string_view>(), &cb_i8));
}
static_assert(std::is_same_v<wsf_i32_t, vmhook::watch_handle>,
              "watch_static_field<W, int32_t>(..) must return watch_handle.");
static_assert(std::is_same_v<wsf_i64_t, vmhook::watch_handle>,
              "watch_static_field<W, int64_t>(..) must return watch_handle.");
static_assert(std::is_same_v<wsf_i16_t, vmhook::watch_handle>,
              "watch_static_field<W, int16_t>(..) must return watch_handle.");
static_assert(std::is_same_v<wsf_i8_t,  vmhook::watch_handle>,
              "watch_static_field<W, int8_t>(..) must return watch_handle.");

// data_breakpoint_kind / data_breakpoint_length enum encodings — these
// drive the DR7 LEN/RW field selection in the DR trap path; they are part
// of the public os layer surface and must remain stable.
static_assert(static_cast<std::uint8_t>(
                  vmhook::os::data_breakpoint_kind::write) == 0b01,
              "data_breakpoint_kind::write must encode to DR7 RW=0b01.");
static_assert(static_cast<std::uint8_t>(
                  vmhook::os::data_breakpoint_kind::read_write) == 0b11,
              "data_breakpoint_kind::read_write must encode to DR7 RW=0b11.");
static_assert(static_cast<std::uint8_t>(
                  vmhook::os::data_breakpoint_length::one_byte) == 0b00,
              "data_breakpoint_length::one_byte must encode to DR7 LEN=0b00.");
static_assert(static_cast<std::uint8_t>(
                  vmhook::os::data_breakpoint_length::two_bytes) == 0b01,
              "data_breakpoint_length::two_bytes must encode to DR7 LEN=0b01.");
static_assert(static_cast<std::uint8_t>(
                  vmhook::os::data_breakpoint_length::eight_bytes) == 0b10,
              "data_breakpoint_length::eight_bytes must encode to DR7 LEN=0b10.");
static_assert(static_cast<std::uint8_t>(
                  vmhook::os::data_breakpoint_length::four_bytes) == 0b11,
              "data_breakpoint_length::four_bytes must encode to DR7 LEN=0b11.");

// ─────────────────────────────────────────────────────────────────────────
// RUNTIME CONTRACT
// ─────────────────────────────────────────────────────────────────────────

int main()
{
    info_int("VMHOOK_HAS_HW_DATA_BREAKPOINTS", VMHOOK_HAS_HW_DATA_BREAKPOINTS);

    // A. Default-constructed watch_handle is inert.
    {
        vmhook::watch_handle empty{};
        check("A_default_handle_not_running", !empty.running());
        empty.stop();
        check("A_stop_idempotent_on_empty", !empty.running());
        empty.stop();
        check("A_double_stop_safe", !empty.running());
    }

    // B. Cold-state watch_static_field — no JVM (and unregistered wrapper).
    //    On every platform this returns an inert handle:
    //      - HW_DATA_BREAKPOINTS==0: the function's else-branch unconditionally
    //        returns watch_handle{} after logging.
    //      - HW_DATA_BREAKPOINTS==1: object_base::get_field returns nullopt
    //        because resolve_klass on an unregistered wrapper is null,
    //        so the "field not found" arm returns watch_handle{}.
    //    No callback invocation is observable either way.
    {
        int call_count{ 0 };
        auto handle = vmhook::watch_static_field<cold_wrapper, std::int32_t>(
            std::string_view{ "value" },
            [&call_count](std::int32_t /*old_val*/, std::int32_t /*new_val*/) noexcept
            {
                ++call_count;
            });

        check("B_cold_install_handle_not_running", !handle.running());
        check("B_cold_install_callback_never_fired", call_count == 0);

        // stop() on the inert handle is safe.
        handle.stop();
        check("B_post_stop_not_running", !handle.running());
    }

    // C. Idempotency: repeated cold-state installs all yield inert handles
    //    — none of them consume a hardware DR slot (we never reach the
    //    arming path because the field never resolves).  We can hand-roll
    //    well over the 4-slot limit to prove the "5th watch is refused"
    //    documentation is not the path we end up on here.
    {
        constexpr int reinstalls{ 16 };
        std::vector<vmhook::watch_handle> handles{};
        handles.reserve(reinstalls);
        for (int i{ 0 }; i < reinstalls; ++i)
        {
            handles.emplace_back(
                vmhook::watch_static_field<cold_wrapper, std::int32_t>(
                    std::string_view{ "value" },
                    [](std::int32_t, std::int32_t) noexcept {}));
        }
        for (const auto& h : handles)
        {
            check("C_each_handle_inert", !h.running());
        }
    }

    // D. Different field widths exercise each branch of the constexpr
    //    LEN selector (sizeof==1/2/4/8).  All must return inert handles
    //    cold; in particular this proves the SFINAE-free template body
    //    instantiates cleanly for all four widths without referencing
    //    any DR machinery at runtime.
    {
        auto h8  = vmhook::watch_static_field<cold_wrapper, std::int8_t>(
            std::string_view{ "b" },
            [](std::int8_t, std::int8_t) noexcept {});
        auto h16 = vmhook::watch_static_field<cold_wrapper, std::int16_t>(
            std::string_view{ "s" },
            [](std::int16_t, std::int16_t) noexcept {});
        auto h32 = vmhook::watch_static_field<cold_wrapper, std::int32_t>(
            std::string_view{ "i" },
            [](std::int32_t, std::int32_t) noexcept {});
        auto h64 = vmhook::watch_static_field<cold_wrapper, std::int64_t>(
            std::string_view{ "j" },
            [](std::int64_t, std::int64_t) noexcept {});
        check("D_h8_inert",  !h8.running());
        check("D_h16_inert", !h16.running());
        check("D_h32_inert", !h32.running());
        check("D_h64_inert", !h64.running());
    }

    // E. Move semantics across the cold-state empty handle: move-out leaves
    //    source inert; move-in dest is inert; double-stop is safe.
    {
        auto h1 = vmhook::watch_static_field<cold_wrapper, std::int32_t>(
            std::string_view{ "v" },
            [](std::int32_t, std::int32_t) noexcept {});
        check("E_pre_move_source_inert", !h1.running());

        vmhook::watch_handle h2{ std::move(h1) };
        check("E_post_move_source_inert", !h1.running());
        check("E_post_move_dest_inert",   !h2.running());

        vmhook::watch_handle h3{};
        h3 = std::move(h2);
        check("E_move_assign_dest_inert", !h3.running());
        h3.stop();
        check("E_post_stop_inert", !h3.running());
    }

    // F. Empty field-name edge: a zero-length name string must not crash;
    //    cold-state still yields an inert handle.
    {
        auto handle = vmhook::watch_static_field<cold_wrapper, std::int32_t>(
            std::string_view{},
            [](std::int32_t, std::int32_t) noexcept {});
        check("F_empty_name_handle_inert", !handle.running());
    }

    // G. Platform characterization — [INFO] only, never a hard fail.
    {
        info("G_platform_has_hw_data_breakpoints",
             VMHOOK_HAS_HW_DATA_BREAKPOINTS != 0);
    }

    std::printf("\n%s — %d failure(s)\n",
                failures == 0 ? "[ALL PASSED]" : "[SOME FAILED]", failures);
    return failures == 0 ? 0 : 1;
}
