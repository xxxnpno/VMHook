// Standalone (no-JVM) edge tests for the zero-size and null-guard corners of
// the vmhook::os layer.  These focus on the CHANGELOG "release(addr, 0) is a
// no-op" fix and the get_proc_address / protect null-/zero-guards.
//
// This file deliberately does NOT duplicate tests/test_os_protect_interaction.cpp
// (which already covers the basic release-zero no-op, the protect/safe_read
// null guards, the enum walk and the granularity relationship).  Here we drill
// into the *edges* that file does not assert:
//   * release(ptr, 0) is idempotent across many calls and the block survives
//     until a later real release(ptr, page) frees it (no double-free, no leak).
//   * release(nullptr, 0) / release(nullptr, page) / release(ptr, 0) are all
//     no-ops that never fault.
//   * protect(): on the null/zero early-return path the caller's old_prot
//     output is left completely untouched (finding
//     test_protect_writes_old_prot_only_on_success).
//   * protect() accepts a non-page-aligned interior address (the wrapper aligns
//     internally) and does not corrupt neighbouring bytes.
//   * get_proc_address(): the null-symbol guard fires BEFORE the OS lookup even
//     when a *valid, real* module handle is supplied, and a real symbol still
//     resolves through the same path (positive control).
//   * page_size() / allocation_granularity() invariants and idempotency.
//
// Everything here is pure OS-layer / null-safety / boundary behaviour.  Nothing
// in this file requires a live oop or a running JVM; the oop/JVM-dependent paths
// of vmhook are covered by JVM integration in example.cpp.
#include <vmhook/vmhook.hpp>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

// ---------------------------------------------------------------------------
// Compile-time sanity on the portable memory_protection enum.  If anyone
// renumbers these, the protect() native-mapping switch and every caller that
// stores a raw old_prot break silently; pin the contract here.
// ---------------------------------------------------------------------------
static_assert(static_cast<std::uint32_t>(vmhook::os::memory_protection::no_access) == 0u);
static_assert(static_cast<std::uint32_t>(vmhook::os::memory_protection::read) == 1u);
static_assert(static_cast<std::uint32_t>(vmhook::os::memory_protection::read_write) == 2u);
static_assert(static_cast<std::uint32_t>(vmhook::os::memory_protection::execute_read) == 3u);
static_assert(static_cast<std::uint32_t>(vmhook::os::memory_protection::execute_rw) == 4u);

// ---------------------------------------------------------------------------
// release(addr, 0) is the CHANGELOG fix: a guaranteed no-op.  We push harder
// than the single check in test_os_protect_interaction.cpp by calling it many
// times in a row and confirming the block is still live and writable after
// each, then performing one real release(ptr, page) at the end.
// ---------------------------------------------------------------------------
static auto test_release_zero_size_is_idempotent_noop() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
    if (!block)
    {
        check("release_zero_idempotent_skipped_alloc_failed", false);
        return;
    }

    auto* const cell{ static_cast<volatile std::uint8_t*>(block) };
    *cell = 0x10;

    // A whole run of zero-size releases must NOT unmap the block.  If the guard
    // were missing, munmap(addr, 0) on POSIX returns EINVAL (harmless) but
    // VirtualFree(addr, 0, MEM_RELEASE) on Windows would actually free the
    // reservation, and the next write would fault.  Either way the byte must
    // survive every iteration.
    bool survived{ true };
    for (int i{ 0 }; i < 8; ++i)
    {
        vmhook::os::release(block, 0);
        *cell = static_cast<std::uint8_t>(0x20 + i);
        if (*cell != static_cast<std::uint8_t>(0x20 + i))
        {
            survived = false;
            break;
        }
    }
    check("release_zero_size_repeated_keeps_block_live", survived);

    // The block is still ours: one real release frees it cleanly (no crash).
    vmhook::os::release(block, page);
    check("release_zero_then_real_release_no_crash", true);
}

// ---------------------------------------------------------------------------
// release() short-circuits on a null address and/or zero size and never calls
// the kernel.  Exercise every null/zero combination; the assert is simply
// "we got here without faulting".
// ---------------------------------------------------------------------------
static auto test_release_null_and_zero_combinations_are_safe() -> void
{
    const std::size_t page{ vmhook::os::page_size() };

    vmhook::os::release(nullptr, 0);       // null addr + zero size
    vmhook::os::release(nullptr, page);    // null addr + non-zero size
    check("release_null_addr_variants_no_crash", true);

    // A bogus but non-null address paired with size 0 must also be a no-op:
    // the size==0 arm of the guard fires before the address is ever passed to
    // VirtualFree / munmap, so the garbage pointer is never dereferenced.
    void* const bogus{ reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x1234)) };
    vmhook::os::release(bogus, 0);
    check("release_bogus_addr_zero_size_no_crash", true);
}

// ---------------------------------------------------------------------------
// allocate_rwx then release(ptr, 0) then real release: the exact sequence
// called out in the cluster.  Confirms the zero-size release does not consume
// the mapping and the subsequent full release succeeds.
// ---------------------------------------------------------------------------
static auto test_allocate_rwx_release_zero_then_real() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
    check("allocate_rwx_nonzero_size_returns_ptr", block != nullptr);
    if (!block)
    {
        return;
    }

    auto* const cell{ static_cast<volatile std::uint8_t*>(block) };
    *cell = 0x5A;

    vmhook::os::release(block, 0);  // no-op
    check("release_zero_size_preserves_written_byte", *cell == 0x5A);

    // Mutate again after the no-op release to prove the page is still mapped RW.
    *cell = 0xA5;
    check("release_zero_size_block_still_writable", *cell == 0xA5);

    vmhook::os::release(block, page);  // real release
    check("real_release_after_zero_size_no_crash", true);
}

// ---------------------------------------------------------------------------
// allocate_rwx(size==0) must return nullptr without calling VirtualAlloc/mmap,
// regardless of the placement hint.  (The interaction test asserts two hints;
// here we add the null + high-canonical + low pointers to be thorough.)
// ---------------------------------------------------------------------------
static auto test_allocate_rwx_zero_size_returns_null() -> void
{
    check("allocate_rwx_zero_size_null_hint_returns_null",
          vmhook::os::allocate_rwx(nullptr, 0) == nullptr);
    check("allocate_rwx_zero_size_low_hint_returns_null",
          vmhook::os::allocate_rwx(reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x1000)), 0)
              == nullptr);
    check("allocate_rwx_zero_size_high_hint_returns_null",
          vmhook::os::allocate_rwx(reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x7FFF'0000'0000ull)), 0)
              == nullptr);
}

// ---------------------------------------------------------------------------
// protect() early-returns false on null address / zero size and, crucially,
// must NOT write through the old_prot pointer on that path.  A caller that
// passes a pre-seeded old_prot and ignores the false return should still find
// its sentinel intact (finding test_protect_writes_old_prot_only_on_success).
// ---------------------------------------------------------------------------
static auto test_protect_null_zero_guards_and_old_prot_untouched() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    constexpr std::uint32_t sentinel{ 0xDEADBEEFu };

    // null address -> false, old_prot untouched.
    {
        std::uint32_t op{ sentinel };
        const bool ok{ vmhook::os::protect(nullptr, page,
                                           vmhook::os::memory_protection::read_write, &op) };
        check("protect_null_addr_returns_false", !ok);
        check("protect_null_addr_leaves_old_prot_untouched", op == sentinel);
    }

    // zero size on a real-but-stack pointer -> false, old_prot untouched.
    {
        std::uint8_t scratch[32]{};
        std::uint32_t op{ sentinel };
        const bool ok{ vmhook::os::protect(scratch, 0,
                                           vmhook::os::memory_protection::read_write, &op) };
        check("protect_zero_size_returns_false", !ok);
        check("protect_zero_size_leaves_old_prot_untouched", op == sentinel);
    }

    // both null address AND zero size -> false, old_prot untouched.
    {
        std::uint32_t op{ sentinel };
        const bool ok{ vmhook::os::protect(nullptr, 0,
                                           vmhook::os::memory_protection::read, &op) };
        check("protect_null_addr_zero_size_returns_false", !ok);
        check("protect_null_addr_zero_size_leaves_old_prot_untouched", op == sentinel);
    }

    // The guard must also tolerate a null old_prot on the failure path (no
    // attempt to dereference it).
    check("protect_null_addr_null_old_prot_returns_false",
          !vmhook::os::protect(nullptr, page,
                               vmhook::os::memory_protection::read, nullptr));
    check("protect_zero_size_null_old_prot_returns_false",
          !vmhook::os::protect(reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x4000)), 0,
                               vmhook::os::memory_protection::read, nullptr));
}

// ---------------------------------------------------------------------------
// protect() must accept a non-page-aligned interior address: the POSIX path
// aligns the base down and the length up internally, and Windows VirtualProtect
// aligns natively.  Here we focus on the *zero-damage* contract on a single
// page (the crossing-page case lives in test_os_protect_interaction.cpp): the
// byte we changed protection around, and its page-0 neighbours, survive.
// iOS has no fault-safe story for some of this; the protect/flip itself is
// fine, so we only guard the no_access pieces elsewhere.
// ---------------------------------------------------------------------------
static auto test_protect_non_page_aligned_addr_single_page() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    // One page is enough: an unaligned interior address still rounds down into
    // the same page, so the wrapper never touches memory we don't own.
    void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
    if (!block)
    {
        check("protect_unaligned_single_page_skipped_alloc_failed", false);
        return;
    }

    auto* const bytes{ static_cast<std::uint8_t*>(block) };
    bytes[0]          = 0xC1;
    bytes[page / 3]   = 0xC2;
    bytes[page - 1]   = 0xC3;

    // Deliberately unaligned base (page/3 is not a multiple of page_size) with
    // a tiny 1-byte length.  Must succeed: the wrapper aligns the request.
    const bool flipped{ vmhook::os::protect(bytes + (page / 3), 1,
                                            vmhook::os::memory_protection::read,
                                            nullptr) };
    check("protect_unaligned_interior_addr_succeeds", flipped);

    // Read-back of the now-read-only page must still show the original bytes:
    // changing protection must never rewrite the contents.
    check("protect_unaligned_preserves_byte_at_zero", bytes[0] == 0xC1);
    check("protect_unaligned_preserves_byte_mid", bytes[page / 3] == 0xC2);
    check("protect_unaligned_preserves_byte_last", bytes[page - 1] == 0xC3);

    // Restore a writable mapping for teardown.  execute_rw is the natural state
    // from allocate_rwx; fall back to read_write where W^X forbids it (Apple).
    bool restored{ vmhook::os::protect(bytes, page,
                                       vmhook::os::memory_protection::execute_rw, nullptr) };
    if (!restored)
    {
        restored = vmhook::os::protect(bytes, page,
                                       vmhook::os::memory_protection::read_write, nullptr);
    }
    check("protect_restore_writable_single_page", restored);

    vmhook::os::release(block, page);
}

// ---------------------------------------------------------------------------
// get_proc_address() null-name guard.  The header guards `if (!module ||
// !symbol) return nullptr;` BEFORE calling GetProcAddress/dlsym, so the guard
// must fire even with a *valid, real* module handle.  We obtain a real handle
// (the host process / a guaranteed-loaded module) and confirm:
//   * a null symbol on that real handle -> nullptr (guard fired, no OS call)
//   * a null module with a non-null symbol -> nullptr
//   * both null -> nullptr
// Then, as a positive control, we resolve a real exported symbol through the
// same handle to prove the guard is the ONLY thing rejecting the null-name case
// (i.e. the path otherwise works).
// ---------------------------------------------------------------------------
static auto test_get_proc_address_null_name_guard() -> void
{
    // find_loaded_module(nullptr): on Windows GetModuleHandleA(nullptr) returns
    // the .exe handle; on POSIX dlopen(nullptr, ...) returns a handle to the
    // global symbol scope.  Either way it is a valid, non-null handle suitable
    // for exercising the null-symbol guard.
    const vmhook::os::module_handle self{ vmhook::os::find_loaded_module(nullptr) };
    check("find_loaded_module_null_name_returns_handle", self != nullptr);

    if (self)
    {
        // Real handle + null symbol must be rejected by the guard, NOT by the
        // OS (GetProcAddress(h, nullptr) is undefined; dlsym(h, nullptr) is a
        // segfault risk).  The guard prevents both.
        check("get_proc_address_real_handle_null_symbol_returns_null",
              vmhook::os::get_proc_address(self, nullptr) == nullptr);
    }

    // null module + valid symbol -> nullptr (module half of the guard).
    check("get_proc_address_null_module_valid_symbol_returns_null",
          vmhook::os::get_proc_address(nullptr, "memcpy") == nullptr);

    // both null -> nullptr.
    check("get_proc_address_both_null_returns_null",
          vmhook::os::get_proc_address(nullptr, nullptr) == nullptr);

    // A non-null but bogus handle with a null symbol still hits the symbol half
    // of the guard before the handle is ever dereferenced.
    {
        const vmhook::os::module_handle bogus{
            reinterpret_cast<vmhook::os::module_handle>(static_cast<std::uintptr_t>(0x1)) };
        check("get_proc_address_bogus_handle_null_symbol_returns_null",
              vmhook::os::get_proc_address(bogus, nullptr) == nullptr);
    }

    // Positive control: a guaranteed-exported C runtime symbol resolves through
    // the same real handle.  This proves the null-name rejections above are the
    // guard talking, not a generally-dead lookup path.  We try a few names that
    // exist in the process's global scope on each platform.
    if (self)
    {
        const char* const names[]{
#if VMHOOK_OS_WINDOWS
            // On Windows, find_loaded_module(nullptr) is the .exe; its exports
            // are usually empty.  Use a module guaranteed to export symbols.
            "memcpy", "malloc",
#else
            "malloc", "free", "memcpy",
#endif
        };

        vmhook::os::module_handle lookup_handle{ self };
#if VMHOOK_OS_WINDOWS
        // The .exe rarely re-exports CRT symbols; resolve against a module that
        // definitely does.  kernel32 is always loaded in a Win32 process.
        if (const vmhook::os::module_handle k32{ vmhook::os::find_loaded_module("kernel32.dll") })
        {
            lookup_handle = k32;
        }
        const char* const win_names[]{ "GetProcAddress", "LoadLibraryA", "VirtualProtect" };
        bool resolved{ false };
        for (const char* n : win_names)
        {
            if (vmhook::os::get_proc_address(lookup_handle, n) != nullptr)
            {
                resolved = true;
                break;
            }
        }
        check("get_proc_address_resolves_real_symbol_positive_control", resolved);
        (void)names;
#else
        bool resolved{ false };
        for (const char* n : names)
        {
            if (vmhook::os::get_proc_address(lookup_handle, n) != nullptr)
            {
                resolved = true;
                break;
            }
        }
        check("get_proc_address_resolves_real_symbol_positive_control", resolved);
#endif

        // And the null-name guard still wins on the very handle that just
        // resolved a real symbol.
        check("get_proc_address_null_symbol_on_resolving_handle_returns_null",
              vmhook::os::get_proc_address(lookup_handle, nullptr) == nullptr);
    }
}

// ---------------------------------------------------------------------------
// page_size() / allocation_granularity() invariants.  test_os_layer.cpp checks
// non-zero + power-of-two for page_size and non-zero for granularity; the
// interaction test checks gr >= ps and gr % ps == 0.  Here we additionally pin
// idempotency (two reads agree) and a sane lower bound, which the trampoline
// allocator's stride math depends on.
// ---------------------------------------------------------------------------
static auto test_page_size_and_granularity_relationship() -> void
{
    const std::size_t ps1{ vmhook::os::page_size() };
    const std::size_t ps2{ vmhook::os::page_size() };
    const std::size_t gr1{ vmhook::os::allocation_granularity() };
    const std::size_t gr2{ vmhook::os::allocation_granularity() };

    check("page_size_nonzero", ps1 != 0);
    check("page_size_power_of_two", (ps1 & (ps1 - 1)) == 0);
    check("page_size_at_least_4096", ps1 >= 4096);
    check("page_size_idempotent", ps1 == ps2);

    check("allocation_granularity_nonzero", gr1 != 0);
    check("allocation_granularity_power_of_two", (gr1 & (gr1 - 1)) == 0);
    check("allocation_granularity_idempotent", gr1 == gr2);

    // The two invariants the allocator relies on: granularity is a whole
    // multiple of the page size and never smaller than it.
    check("allocation_granularity_at_least_page_size", gr1 >= ps1);
    check("allocation_granularity_multiple_of_page_size", (gr1 % ps1) == 0);
}

int main()
{
    test_release_zero_size_is_idempotent_noop();
    test_release_null_and_zero_combinations_are_safe();
    test_allocate_rwx_release_zero_then_real();
    test_allocate_rwx_zero_size_returns_null();
    test_protect_null_zero_guards_and_old_prot_untouched();
    test_protect_non_page_aligned_addr_single_page();
    test_get_proc_address_null_name_guard();
    test_page_size_and_granularity_relationship();

    if (failures == 0)
    {
        std::printf("vmhook os release/protect edges: OK\n");
    }
    else
    {
        std::printf("vmhook os release/protect edges: %d FAILURE(S)\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
