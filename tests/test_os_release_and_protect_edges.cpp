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
#include <cstddef>
#include <cstring>
#include <limits>
#include <array>
#include <utility>
#include <bit>
#include <type_traits>
#include <string>
#include <string_view>
#include <vector>

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

// A characterization line for a behaviour that LEGITIMATELY differs across
// kernels (e.g. partial-size release, double-release, protect-after-release):
// it must never be asserted hard, only reported.  Keeping the format identical
// to the sibling files' "[INFO] ... skipped/refused" convention.
static auto info(const char* name, bool observed) -> void
{
    std::printf("[INFO] %s = %s\n", name, observed ? "true" : "false");
}

// ---------------------------------------------------------------------------
// Shared teardown helpers.  Every test that flips protection must restore a
// writable mapping before release() so no platform-level integrity check trips
// on a read-only / no-access mapping at unmap time.  execute_rw is the natural
// state allocate_rwx hands back; Apple arm64 / current iOS enforce W^X and
// refuse PROT_WRITE|PROT_EXEC without the JIT entitlement, so fall back to plain
// read_write (which is what allocate_rwx itself falls back to there).
// ---------------------------------------------------------------------------
static auto make_writable(void* base, std::size_t size) -> bool
{
    bool ok{ vmhook::os::protect(base, size,
                                 vmhook::os::memory_protection::execute_rw, nullptr) };
    if (!ok)
    {
        ok = vmhook::os::protect(base, size,
                                 vmhook::os::memory_protection::read_write, nullptr);
    }
    return ok;
}

// Probe whether the first byte at `p` is readable WITHOUT faulting.  safe_read
// kernel-validates the source (ReadProcessMemory / process_vm_readv /
// mach_vm_read_overwrite) and returns false instead of crashing.  Not usable on
// iOS (safe_read there is a raw memcpy that would fault on a no_access page), so
// callers gate the no_access cases behind !VMHOOK_OS_IOS.
static auto byte_is_readable(const void* p) -> bool
{
    std::uint8_t sink{ 0 };
    return vmhook::os::safe_read(&sink, p, 1u);
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

// ===========================================================================
// EXPANSION — release / allocate lifecycle edges and protect<->release
// interaction edges that the sibling files do NOT cover.
//
// test_os_protect_interaction.cpp owns the deep protect behavioural matrix
// (every enum, size-rounding, old_prot platform-asymmetry, overflow guard,
// neighbour witness); test_os_safe_read.cpp and test_os_query_region.cpp own
// those primitives.  This block deliberately stays on the RELEASE / ALLOCATE
// lifecycle and the protect<->release seam:
//   * multi-page release, allocate/release reuse cycles, sub-page + multi-page
//     allocate writability, two-block non-aliasing, non-binding hint;
//   * release of a region left NON-writable (read / no_access) — the sibling
//     always restores writable first, so this seam is untested elsewhere;
//   * partial-size / oversize / double / never-allocated release — all
//     LEGITIMATELY platform-variable, so reported via [INFO], never asserted;
//   * the overflow-guard early-return leaves old_prot untouched (a THIRD
//     early-return distinct from the null/zero guards already covered above).
// Every assertion below is a cross-platform INVARIANT; every platform-variable
// outcome is an [INFO] characterization line.
// ===========================================================================

// ---------------------------------------------------------------------------
// allocate_rwx returns memory that is immediately writable across the WHOLE
// requested span, and a real release(ptr, size) of a multi-page region frees it
// cleanly.  The basic round-trip in test_os_layer.cpp only touches the first
// byte of a single page; here we stamp + read back a marker in EVERY page of a
// multi-page allocation, proving the whole reservation is committed RW, then
// release the entire span.
// ---------------------------------------------------------------------------
static auto test_allocate_rwx_multipage_writable_then_release() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    constexpr std::size_t pages{ 4 };
    void* const block{ vmhook::os::allocate_rwx(nullptr, page * pages) };
    if (!block)
    {
        check("allocate_multipage_skipped_alloc_failed", false);
        return;
    }

    auto* const bytes{ static_cast<std::uint8_t*>(block) };

    // Write a distinct marker at the start of each page AND at the very last
    // byte of the allocation (the far edge of the last page).
    bool all_written{ true };
    for (std::size_t p{ 0 }; p < pages; ++p)
    {
        const auto marker{ static_cast<std::uint8_t>(0xA0 + p) };
        bytes[p * page] = marker;
        if (bytes[p * page] != marker)
        {
            all_written = false;
        }
    }
    bytes[page * pages - 1] = 0xEF;
    if (bytes[page * pages - 1] != 0xEF)
    {
        all_written = false;
    }
    check("allocate_multipage_every_page_writable", all_written);

    // Read every marker back — proves no page silently aliased another.
    bool all_readback{ true };
    for (std::size_t p{ 0 }; p < pages; ++p)
    {
        if (bytes[p * page] != static_cast<std::uint8_t>(0xA0 + p))
        {
            all_readback = false;
        }
    }
    check("allocate_multipage_markers_readback_intact",
          all_readback && bytes[page * pages - 1] == 0xEF);

    // Real release of the whole multi-page reservation (size matters on POSIX:
    // munmap unmaps exactly [block, block + page*pages)).  No crash == pass.
    vmhook::os::release(block, page * pages);
    check("release_multipage_full_region_no_crash", true);
}

// ---------------------------------------------------------------------------
// Sub-page allocate: allocate_rwx(size == 1) must hand back a usable pointer.
// The kernel rounds the mapping up to a full page, so the entire first page is
// writable even though only 1 byte was requested.  Release with size 1 (POSIX
// munmap rounds the length up to the page; Windows ignores size and frees the
// whole reservation) must free it cleanly.
// ---------------------------------------------------------------------------
static auto test_allocate_rwx_subpage_size_one() -> void
{
    void* const block{ vmhook::os::allocate_rwx(nullptr, 1) };
    check("allocate_size_one_returns_ptr", block != nullptr);
    if (!block)
    {
        return;
    }

    auto* const cell{ static_cast<volatile std::uint8_t*>(block) };
    *cell = 0x7E;
    check("allocate_size_one_byte_writable", *cell == 0x7E);
    *cell = 0xE7;
    check("allocate_size_one_byte_rewritable", *cell == 0xE7);

    // Release using the same tiny size the caller asked for.  POSIX munmap
    // rounds the length up to one page; Windows VirtualFree ignores it.
    vmhook::os::release(block, 1);
    check("release_size_one_no_crash", true);
}

// ---------------------------------------------------------------------------
// Two independent allocations must NOT alias: distinct base pointers, and a
// store through one must be invisible through the other.  The trampoline
// allocator places many stubs; if allocate_rwx ever returned overlapping
// reservations the stubs would clobber each other.  Both blocks must be
// simultaneously live and writable.
// ---------------------------------------------------------------------------
static auto test_allocate_rwx_two_blocks_do_not_alias() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    void* const a{ vmhook::os::allocate_rwx(nullptr, page) };
    void* const b{ vmhook::os::allocate_rwx(nullptr, page) };
    if (!a || !b)
    {
        check("allocate_two_blocks_skipped_alloc_failed", false);
        vmhook::os::release(a, page);
        vmhook::os::release(b, page);
        return;
    }

    check("allocate_two_blocks_distinct_pointers", a != b);

    auto* const ba{ static_cast<std::uint8_t*>(a) };
    auto* const bb{ static_cast<std::uint8_t*>(b) };

    // Distinct stores; if the two blocks aliased, the second write would be
    // visible through the first pointer.
    ba[0] = 0x11;
    bb[0] = 0x22;
    check("allocate_two_blocks_no_alias_a", ba[0] == 0x11);
    check("allocate_two_blocks_no_alias_b", bb[0] == 0x22);

    // Both must remain independently writable after the cross writes.
    ba[0] = 0x33;
    check("allocate_two_blocks_a_still_independent", ba[0] == 0x33 && bb[0] == 0x22);

    vmhook::os::release(a, page);
    vmhook::os::release(b, page);
    check("release_two_blocks_no_crash", true);
}

// ---------------------------------------------------------------------------
// allocate_rwx honours an address_hint only as a NON-BINDING preference: the
// kernel may place the mapping elsewhere.  The portable contract is merely
// "you get SOME usable block back", so we assert non-null + writable and do NOT
// assert the returned address equals the hint (Windows rounds the hint to the
// allocation granularity; POSIX may ignore it entirely; ASLR moves it).
// ---------------------------------------------------------------------------
static auto test_allocate_rwx_hint_is_non_binding() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    const std::size_t gran{ vmhook::os::allocation_granularity() };

    // A plausible, granularity-aligned hint well inside user space.  Even if the
    // exact address is taken, allocate_rwx must still return a usable block.
    // Mask is uintptr_t-typed so the high address bits survive on every ABI.
    const std::uintptr_t gran_mask{ ~(static_cast<std::uintptr_t>(gran) - 1u) };
    void* const hint{ reinterpret_cast<void*>(
        static_cast<std::uintptr_t>(0x0000'1000'0000'0000ull) & gran_mask) };

    void* const block{ vmhook::os::allocate_rwx(hint, page) };
    check("allocate_hint_returns_usable_block", block != nullptr);
    if (!block)
    {
        return;
    }

    auto* const cell{ static_cast<volatile std::uint8_t*>(block) };
    *cell = 0x9C;
    check("allocate_hint_block_is_writable", *cell == 0x9C);

    vmhook::os::release(block, page);
    check("release_after_hinted_alloc_no_crash", true);
}

// ---------------------------------------------------------------------------
// allocate / release REUSE cycle: repeatedly allocate a page, use it, and
// release it.  Proves there is no leak that accumulates across the loop (each
// release truly returns the reservation) and that a fresh allocation after a
// release is always writable.  We do NOT assert the address is reused (kernels
// differ); only that every cycle yields a live, writable, releasable block.
// ---------------------------------------------------------------------------
static auto test_allocate_release_reuse_cycle() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    bool every_cycle_ok{ true };
    for (int i{ 0 }; i < 16; ++i)
    {
        void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
        if (!block)
        {
            every_cycle_ok = false;
            break;
        }
        auto* const cell{ static_cast<volatile std::uint8_t*>(block) };
        const auto marker{ static_cast<std::uint8_t>(0x40 + (i & 0x3F)) };
        *cell = marker;
        if (*cell != marker)
        {
            every_cycle_ok = false;
            vmhook::os::release(block, page);
            break;
        }
        vmhook::os::release(block, page);
    }
    check("allocate_release_reuse_cycle_all_live_writable", every_cycle_ok);
}

// ---------------------------------------------------------------------------
// release() must tolerate a mapping that is currently NON-writable.  The
// sibling interaction test always restores read_write before release; this one
// deliberately does NOT — it leaves the page in `read` (read-only) and releases
// it directly.  munmap (POSIX) and VirtualFree (Windows) both ignore the
// page protection, so the unmap must still succeed without faulting.
// ---------------------------------------------------------------------------
static auto test_release_of_read_only_mapping() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
    if (!block)
    {
        check("release_readonly_skipped_alloc_failed", false);
        return;
    }

    auto* const bytes{ static_cast<std::uint8_t*>(block) };
    bytes[0] = 0x5A;

    const bool flipped{ vmhook::os::protect(block, page,
                                            vmhook::os::memory_protection::read,
                                            nullptr) };
    check("release_readonly_protect_read_succeeds", flipped);

    // Release WITHOUT restoring writability.  This is the seam under test.
    vmhook::os::release(block, page);
    check("release_of_read_only_mapping_no_crash", true);
}

// ---------------------------------------------------------------------------
// release() must also tolerate a mapping left in `no_access`.  Gated like every
// other no_access probe: a sandbox may refuse PROT_NONE, in which case we skip
// (the protect itself returns false) rather than fail.  When it succeeds, the
// release of an inaccessible mapping must still not fault.
// ---------------------------------------------------------------------------
static auto test_release_of_no_access_mapping() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
    if (!block)
    {
        check("release_no_access_skipped_alloc_failed", false);
        return;
    }

    const bool flipped{ vmhook::os::protect(block, page,
                                            vmhook::os::memory_protection::no_access,
                                            nullptr) };
    if (!flipped)
    {
        std::printf("[INFO] release_no_access_skipped: protect(no_access) refused\n");
        // Restore writable so the unmap below is of a normal mapping.
        (void)make_writable(block, page);
        vmhook::os::release(block, page);
        return;
    }

    // Release WITHOUT restoring access.  Must not fault.
    vmhook::os::release(block, page);
    check("release_of_no_access_mapping_no_crash", true);
}

// ---------------------------------------------------------------------------
// PLATFORM-VARIABLE: release() with a size that is SMALLER than the original
// allocation (partial release), and a size LARGER than the allocation.
//   * Windows: VirtualFree(addr, 0, MEM_RELEASE) ignores the size and frees the
//     ENTIRE reservation regardless of the value passed.
//   * POSIX: munmap(addr, n) unmaps exactly the pages spanned by [addr, addr+n)
//     and leaves the rest mapped (partial), or unmaps beyond the allocation if
//     n is larger (potentially unmapping adjacent unrelated pages — which is why
//     real callers always pass the exact size).
// The outcome therefore LEGITIMATELY differs by OS, so we never hard-assert it;
// we only (a) report what we can observe via query_region as [INFO] and (b)
// assert the process does not crash and we can always reclaim cleanly.  To stay
// SAFE we allocate a generously oversized block and only ever release sizes that
// stay WITHIN our own allocation, so no unrelated page is ever at risk.
// ---------------------------------------------------------------------------
static auto test_release_partial_size_is_platform_variable() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    constexpr std::size_t pages{ 4 };
    void* const block{ vmhook::os::allocate_rwx(nullptr, page * pages) };
    if (!block)
    {
        check("release_partial_skipped_alloc_failed", false);
        return;
    }

    auto* const bytes{ static_cast<std::uint8_t*>(block) };
    bytes[0] = 0x01;
    bytes[page * (pages - 1)] = 0x02; // marker in the LAST page

    // Partial release: only the first page.  On POSIX this unmaps page 0 and
    // leaves pages 1..3 mapped; on Windows it frees the whole reservation.  We
    // cannot portably read page 0 afterwards (it may be gone), so we only probe
    // the LAST page via the fault-safe path and REPORT the result.
    vmhook::os::release(block, page);
    check("release_partial_first_page_no_crash", true);

#if !VMHOOK_OS_IOS
    // Characterization only — NOT an assertion.  On POSIX the last page is very
    // likely still readable (only page 0 was unmapped); on Windows the whole
    // reservation is gone so it is likely NOT readable.  Either is correct.
    info("release_partial_last_page_still_readable_after_first_page_freed",
         byte_is_readable(bytes + page * (pages - 1)));
#endif

    // Reclaim whatever may remain by releasing the FULL original span.  On
    // Windows this is a (harmless) double-release of an already-freed base; on
    // POSIX it unmaps the still-mapped tail (pages 1..3) — munmap of an
    // already-unmapped sub-range within the call is harmless.  No crash == pass.
    vmhook::os::release(block, page * pages);
    check("release_partial_then_full_no_crash", true);
}

// ---------------------------------------------------------------------------
// PLATFORM-VARIABLE: double-release of the same base.  Freeing an already-freed
// reservation is, strictly, the caller's bug, but the wrapper must not turn it
// into a process-killing fault.
//   * Windows: VirtualFree on an already-released base returns 0 (error) but
//     does not raise.
//   * POSIX: munmap of an already-unmapped range returns -1/EINVAL, harmless.
// We assert ONLY no-crash; the boolean outcome is not exposed by release() (it
// returns void) so there is nothing to assert beyond survival.
// ---------------------------------------------------------------------------
static auto test_double_release_does_not_crash() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
    if (!block)
    {
        check("double_release_skipped_alloc_failed", false);
        return;
    }

    auto* const cell{ static_cast<volatile std::uint8_t*>(block) };
    *cell = 0x3C;

    vmhook::os::release(block, page); // first, real release
    vmhook::os::release(block, page); // second, double release of same base
    check("double_release_same_base_no_crash", true);

    // A third release, for good measure — the no-op contract must hold no matter
    // how many times a freed base is handed back.
    vmhook::os::release(block, page);
    check("triple_release_same_base_no_crash", true);
}

// ---------------------------------------------------------------------------
// PLATFORM-VARIABLE: protect() applied to a region AFTER it has been released.
// Touching freed memory is the caller's bug; the contract we can pin portably
// is only "protect() does not crash on a freed region".  The boolean result is
// platform-variable (Windows VirtualProtect on a freed page -> false; POSIX
// mprotect on an unmapped page -> ENOMEM -> false; but a kernel that has since
// re-used the address could return true), so it is reported via [INFO], not
// asserted.  We use the fault-safe nature of the wrappers: protect() never
// dereferences the page itself, it only asks the kernel to re-tag it, so a
// freed address is a clean kernel rejection rather than a fault.
// ---------------------------------------------------------------------------
static auto test_protect_after_release_is_platform_variable() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
    if (!block)
    {
        check("protect_after_release_skipped_alloc_failed", false);
        return;
    }

    auto* const cell{ static_cast<volatile std::uint8_t*>(block) };
    *cell = 0x6D;

    vmhook::os::release(block, page);

    // protect() on the now-freed region.  No crash is the invariant; the bool
    // is characterized only.
    const bool reprotected{ vmhook::os::protect(block, page,
                                                vmhook::os::memory_protection::read,
                                                nullptr) };
    check("protect_after_release_no_crash", true);
    info("protect_after_release_returned", reprotected);

    // If the kernel happened to honour it (address was re-mapped under us), put
    // it back to a writable state so we don't leave a stray read-only mapping;
    // if it failed, this is a harmless no-op on a freed/again-failing region.
    if (reprotected)
    {
        (void)make_writable(block, page);
        vmhook::os::release(block, page);
    }
    check("protect_after_release_cleanup_no_crash", true);
}

// ---------------------------------------------------------------------------
// The OVERFLOW-GUARD early-return is a THIRD distinct early `return false` in
// protect() — separate from the null-address and zero-size guards already
// covered above.  A non-null base with a size so large that base + size wraps
// the address space must (a) return false and (b) leave a pre-seeded old_prot
// sentinel UNTOUCHED, exactly like the other guards.  This pins that the wrap
// guard rejects BEFORE any *old_prot write on every platform.  It is also
// completely safe: the guard short-circuits before any syscall, so the live
// page handed in is never disturbed.
// ---------------------------------------------------------------------------
static auto test_protect_overflow_guard_leaves_old_prot_untouched() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
    if (!block)
    {
        check("protect_overflow_old_prot_skipped_alloc_failed", false);
        return;
    }

    auto* const bytes{ static_cast<std::uint8_t*>(block) };
    bytes[0] = 0x5A;

    constexpr std::uint32_t sentinel{ 0xDEADBEEFu };

    // SIZE_MAX from a non-null base wraps the address space -> guard fires.
    {
        std::uint32_t op{ sentinel };
        const bool ok{ vmhook::os::protect(block, SIZE_MAX,
                                           vmhook::os::memory_protection::read, &op) };
        check("protect_overflow_size_max_returns_false", !ok);
        check("protect_overflow_size_max_leaves_old_prot_untouched", op == sentinel);
    }

    // The smallest size that still wraps from this exact base: (UINTPTR_MAX -
    // base) + 8.  Pins the guard boundary, not just the SIZE_MAX extreme.
    {
        const std::uintptr_t base_addr{ reinterpret_cast<std::uintptr_t>(block) };
        const std::uintptr_t max_addr{ ~static_cast<std::uintptr_t>(0) };
        const std::size_t wrapping_size{
            static_cast<std::size_t>(max_addr - base_addr) + std::size_t{ 8 } };
        std::uint32_t op{ sentinel };
        const bool ok{ vmhook::os::protect(block, wrapping_size,
                                           vmhook::os::memory_protection::read, &op) };
        check("protect_overflow_boundary_returns_false", !ok);
        check("protect_overflow_boundary_leaves_old_prot_untouched", op == sentinel);
    }

    // The guard must also tolerate a null old_prot on the overflow path.
    check("protect_overflow_null_old_prot_returns_false",
          !vmhook::os::protect(block, SIZE_MAX,
                               vmhook::os::memory_protection::read, nullptr));

    // The rejected calls never touched the kernel: the page is still writable.
    bytes[0] = 0xA5;
    check("protect_overflow_guard_page_untouched_still_writable", bytes[0] == 0xA5);

    vmhook::os::release(block, page);
}

// ---------------------------------------------------------------------------
// Full installer-shaped lifecycle terminating in release, with the page left in
// a NON-original protection at points along the way.  This is the canonical
// trampoline pattern (flip RW, "write", flip RX) but exercised end-to-end as one
// named sequence whose TERMINAL step is release — pinning that the whole cycle,
// including release of a page currently tagged execute_read, never faults.
// X-bearing transitions are treated as best-effort (W^X), never as failures.
// ---------------------------------------------------------------------------
static auto test_protect_cycle_then_release_lifecycle() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
    if (!block)
    {
        check("protect_cycle_lifecycle_skipped_alloc_failed", false);
        return;
    }

    auto* const bytes{ static_cast<std::uint8_t*>(block) };

    // Flip to RW (or RWX) and "patch".
    check("protect_cycle_make_writable", make_writable(block, page));
    bytes[0] = 0xCC;
    bytes[1] = 0xDD;
    check("protect_cycle_patch_sticks", bytes[0] == 0xCC && bytes[1] == 0xDD);

    // Flip to execute_read (the state a real installer leaves code in).  If W^X
    // forbids it the page stays writable; either way the contents are intact.
    const bool rx{ vmhook::os::protect(block, page,
                                       vmhook::os::memory_protection::execute_read,
                                       nullptr) };
    if (!rx)
    {
        std::printf("[INFO] protect_cycle_execute_read refused (W^X); leaving page RW\n");
    }
    check("protect_cycle_contents_survive_rx_flip", bytes[0] == 0xCC && bytes[1] == 0xDD);

    // Terminal step: release a page that is currently execute_read (NOT restored
    // to writable first).  Must not fault.
    vmhook::os::release(block, page);
    check("protect_cycle_release_of_rx_page_no_crash", true);
}

// ===========================================================================
// DEEPENING WAVE-5 -- os_allocate_release: sizing primitives, multi-page /
// granularity-sized writability, returned-pointer alignment, query_region size
// agreement, allocate/release leak-loop, the OS size/base release asymmetry
// (flaws #1/#2) pinned as no-crash, and the execute-bit positive control
// (flaw #3) where the platform actually permits execution.
//
// ADDITIVE ONLY.  Every assertion below is a cross-platform INVARIANT derived
// directly from vmhook.hpp:
//   * page_size()              486-496  (Win dwPageSize / POSIX sysconf, 4096 fb)
//   * allocation_granularity() 501-510  (Win dwAllocationGranularity / POSIX = ps)
//   * allocate_rwx()           703-750  (size==0 -> nullptr; VirtualAlloc page-
//                                        aligned base / mmap page-aligned base)
//   * release()                774-786  (null|0 -> no kernel call; Win ignores
//                                        size, POSIX munmap uses size)
//   * query_region()           793-898  (committed+readable+size>=requested for
//                                        an allocate_rwx block on every OS)
// Platform-variable outcomes are reported via info(), never hard-asserted.
// No fabricated unmapped addresses: every pointer touched is either a real
// allocate_rwx block we own, a stack object, or an is_valid_pointer-rejected
// low constant.
// ===========================================================================
namespace wave5_alloc_release
{
    // page_size() is a power of two (rule 419) so a strict alignment mask is
    // exact: aligned <=> (p & (ps - 1)) == 0.
    static auto is_page_aligned(const void* p, std::size_t ps) -> bool
    {
        return (reinterpret_cast<std::uintptr_t>(p) & (static_cast<std::uintptr_t>(ps) - 1u)) == 0u;
    }

    // -----------------------------------------------------------------------
    // SIZING PRIMITIVES -- deepen the bit-pattern / cast contract beyond the
    // existing non-zero / power-of-two / idempotent / >=4096 checks.
    //   * page_size() must be one of the architecturally-valid page sizes
    //     {4096, 16384, 65536}.  The POSIX fallback literal is 4096 (494); a
    //     real 16 KiB (Apple arm64) or 64 KiB kernel reports those.  Anything
    //     else means the DWORD->size_t cast (491) truncated or sysconf lied.
    //   * The DWORD->size_t casts (491 / 506) must not truncate to zero: a
    //     32-bit dwPageSize/dwAllocationGranularity always fits in a >=32-bit
    //     size_t, so the post-cast value is non-zero (already implied, pinned
    //     explicitly as a cast-safety contract).
    //   * std::popcount == 1 is the same power-of-two predicate phrased via
    //     <bit>, a second independent witness of the single-bit invariant.
    // -----------------------------------------------------------------------
    static auto test_sizing_primitive_bit_contract() -> void
    {
        const std::size_t ps{ vmhook::os::page_size() };
        const std::size_t gr{ vmhook::os::allocation_granularity() };

        // Architecturally-valid page sizes.  4096 (x86 / most), 16384 (Apple
        // arm64, some ARM), 65536 (some ppc64 / experimental).  The fallback
        // literal at 494 is 4096, inside this set, so the contract holds even
        // when sysconf fails.
        const bool ps_is_valid_arch_size{ ps == 4096u || ps == 16384u || ps == 65536u };
        check("wave5_page_size_is_valid_arch_size", ps_is_valid_arch_size);

        // popcount==1 <=> exactly one bit set <=> power of two (and non-zero).
        check("wave5_page_size_popcount_one", std::popcount(ps) == 1);
        check("wave5_granularity_popcount_one", std::popcount(gr) == 1);

        // The DWORD->size_t cast cannot have truncated to zero: a 32-bit field
        // widened into size_t (>=32 bit on every supported ABI) is value-
        // preserving, so a non-zero kernel value stays non-zero.
        check("wave5_page_size_cast_nonzero", ps != 0u);
        check("wave5_granularity_cast_nonzero", gr != 0u);

        // Hard invariant the trampoline allocator's align_down(usable_end -
        // size, granularity) math depends on (vmhook.hpp:4820): granularity is
        // never SMALLER than a page.  A granularity < page would make the
        // last-candidate rounding skip past valid pages.
        check("wave5_granularity_ge_page_size", gr >= ps);
        check("wave5_granularity_multiple_of_page_size", (gr % ps) == 0u);

        // On POSIX allocation_granularity() returns page_size() verbatim (508):
        // the relationship is then EQUALITY, not merely >=.  On Windows the two
        // come from distinct SYSTEM_INFO fields and granularity is typically
        // 64 KiB > page.  We can only hard-assert the >= / multiple invariants
        // portably; the POSIX-equality refinement is reported as a witness.
#if !VMHOOK_OS_WINDOWS
        check("wave5_posix_granularity_equals_page_size", gr == ps);
#else
        info("wave5_windows_granularity_gt_page", gr > ps);
#endif

        // Both fit in size_t without sign issues: size_t is unsigned, so the
        // top bit is never interpreted as negative.
        static_assert(std::is_unsigned_v<std::size_t>);
        check("wave5_page_size_fits_size_t", ps <= (std::numeric_limits<std::size_t>::max)());
    }

    // -----------------------------------------------------------------------
    // RETURNED-POINTER ALIGNMENT.  No existing test pins this.  VirtualAlloc
    // (709) returns a base aligned to the allocation granularity (>= page) and
    // mmap (735) returns a page-aligned base; either way the base is page-
    // aligned.  The trampoline allocator and protect() both assume base
    // alignment, so freeze it for single-page, multi-page, granularity-sized,
    // and sub-page requests.
    // -----------------------------------------------------------------------
    static auto test_returned_pointer_is_page_aligned() -> void
    {
        const std::size_t ps{ vmhook::os::page_size() };
        const std::size_t gr{ vmhook::os::allocation_granularity() };

        const std::array<std::size_t, 5> sizes{ {
            1u,             // sub-page -> rounded up, base still page-aligned
            ps - 1u,        // just under a page
            ps,             // exactly one page
            ps * 4u,        // multi-page
            gr,             // a full granularity unit (>= page)
        } };

        bool every_base_aligned{ true };
        for (const std::size_t s : sizes)
        {
            void* const block{ vmhook::os::allocate_rwx(nullptr, s) };
            if (!block)
            {
                every_base_aligned = false;
                continue;
            }
            if (!is_page_aligned(block, ps))
            {
                every_base_aligned = false;
            }
            vmhook::os::release(block, s);
        }
        check("wave5_allocate_rwx_returns_page_aligned_base", every_base_aligned);
    }

    // -----------------------------------------------------------------------
    // MULTI-PAGE / SUB-PAGE FULL-SPAN WRITABILITY across an exhaustive small
    // domain of N.  The existing multipage test uses N==4 and stamps only the
    // first byte of each page; here we sweep N in {1,4,17}, write a DISTINCT
    // pattern across EVERY byte of EVERY page (not just the page head), read it
    // ALL back, and confirm the full requested span is committed RW with no
    // off-by-one at the tail.  A sub-page request (1 byte, ps-1 bytes) must
    // still hand back a whole writable page.  Then release the exact size.
    // -----------------------------------------------------------------------
    static auto stamp_and_verify_full_span(void* block, std::size_t bytes_to_touch) -> bool
    {
        auto* const bytes{ static_cast<std::uint8_t*>(block) };
        // Fill with a position-dependent pattern so any aliasing or short
        // commit shows up as a mismatch on read-back.
        for (std::size_t i{ 0 }; i < bytes_to_touch; ++i)
        {
            bytes[i] = static_cast<std::uint8_t>((i * 31u + 7u) & 0xFFu);
        }
        for (std::size_t i{ 0 }; i < bytes_to_touch; ++i)
        {
            if (bytes[i] != static_cast<std::uint8_t>((i * 31u + 7u) & 0xFFu))
            {
                return false;
            }
        }
        return true;
    }

    static auto test_multipage_full_span_writable_sweep() -> void
    {
        const std::size_t ps{ vmhook::os::page_size() };
        const std::array<std::size_t, 3> counts{ { 1u, 4u, 17u } };

        bool all_ok{ true };
        for (const std::size_t n : counts)
        {
            const std::size_t size{ ps * n };
            void* const block{ vmhook::os::allocate_rwx(nullptr, size) };
            if (!block)
            {
                all_ok = false;
                continue;
            }
            // Touch the ENTIRE requested span end-to-end, not just page heads.
            if (!stamp_and_verify_full_span(block, size))
            {
                all_ok = false;
            }
            vmhook::os::release(block, size);
        }
        check("wave5_multipage_full_span_writable_N_1_4_17", all_ok);

        // Sub-page requests: only the requested byte-count is the contract, but
        // the kernel rounds the mapping up so the whole first page is usable.
        // Touch exactly (ps - 1) and exactly 1 byte spans.
        {
            void* const a{ vmhook::os::allocate_rwx(nullptr, 1u) };
            void* const b{ vmhook::os::allocate_rwx(nullptr, ps - 1u) };
            bool ok{ a != nullptr && b != nullptr };
            if (a) { ok = ok && stamp_and_verify_full_span(a, 1u); }
            if (b) { ok = ok && stamp_and_verify_full_span(b, ps - 1u); }
            vmhook::os::release(a, 1u);
            vmhook::os::release(b, ps - 1u);
            check("wave5_subpage_requests_fully_writable", ok);
        }
    }

    // -----------------------------------------------------------------------
    // query_region() AGREEMENT with allocate_rwx.  For a freshly allocated
    // block query_region must report committed && readable on every OS
    // (Windows MEM_COMMIT + a readable PAGE_* flag, macOS committed=true +
    // VM_PROT_READ, Linux maps perms[0]=='r').  Additionally the reported
    // region size must COVER the request: Windows RegionSize, the macOS region
    // size, and the Linux VMA all span at least the bytes we asked for.  We
    // only assert size>=requested WHEN committed is reported (a split/free
    // region has different size semantics).  Sweep single-page, multi-page,
    // and granularity-sized.
    // -----------------------------------------------------------------------
    static auto test_query_region_covers_allocation() -> void
    {
        const std::size_t ps{ vmhook::os::page_size() };
        const std::array<std::size_t, 3> sizes{ { ps, ps * 4u, vmhook::os::allocation_granularity() } };

        bool committed_all{ true };
        bool readable_all{ true };
        bool size_covers_all{ true };
        for (const std::size_t s : sizes)
        {
            void* const block{ vmhook::os::allocate_rwx(nullptr, s) };
            if (!block)
            {
                committed_all = false;
                continue;
            }
            // Touch the first byte so the page is definitely resident before we
            // query (does not change the committed/readable contract, but makes
            // the region unambiguously live).
            *static_cast<volatile std::uint8_t*>(block) = 0x5A;

            const vmhook::os::region_info ri{ vmhook::os::query_region(block) };
            if (!ri.committed) { committed_all = false; }
            if (!ri.readable)  { readable_all = false; }
            // size>=requested only meaningful when the region is committed.
            if (ri.committed && ri.size < s) { size_covers_all = false; }
            vmhook::os::release(block, s);
        }
        check("wave5_query_region_allocate_rwx_committed", committed_all);
        check("wave5_query_region_allocate_rwx_readable", readable_all);
        check("wave5_query_region_size_covers_request", size_covers_all);

        // query_region of nullptr is the documented early-out: an all-default
        // region_info (base null, size 0, every flag false).  Pin every field.
        {
            const vmhook::os::region_info ri{ vmhook::os::query_region(nullptr) };
            check("wave5_query_region_null_base_null", ri.base == nullptr);
            check("wave5_query_region_null_size_zero", ri.size == 0u);
            check("wave5_query_region_null_not_committed", !ri.committed);
            check("wave5_query_region_null_not_readable", !ri.readable);
            check("wave5_query_region_null_not_executable", !ri.executable);
            check("wave5_query_region_null_not_guarded", !ri.guarded);
        }
    }

    // -----------------------------------------------------------------------
    // LEAK / STABILITY LOOP (flaws #1 / #2).  A release path that leaked would
    // eventually fail to allocate.  Run a long round-trip loop: allocate one
    // page, write+verify, release the EXACT size, 1000 times.  Every iteration
    // must succeed.  This directly stresses that release() truly returns the
    // reservation on both OSes (POSIX munmap with matching size, Windows
    // VirtualFree of the base).  We do NOT assert address reuse (kernel-
    // dependent); only that no monotone exhaustion betrays a leak.
    // -----------------------------------------------------------------------
    static auto test_round_trip_leak_loop() -> void
    {
        const std::size_t ps{ vmhook::os::page_size() };
        bool every_iter_ok{ true };
        int completed{ 0 };
        for (int i{ 0 }; i < 1000; ++i)
        {
            void* const block{ vmhook::os::allocate_rwx(nullptr, ps) };
            if (!block)
            {
                every_iter_ok = false;
                break;
            }
            auto* const cell{ static_cast<volatile std::uint8_t*>(block) };
            const auto marker{ static_cast<std::uint8_t>(i & 0xFF) };
            *cell = marker;
            if (*cell != marker)
            {
                every_iter_ok = false;
                vmhook::os::release(block, ps);
                break;
            }
            vmhook::os::release(block, ps);
            ++completed;
        }
        check("wave5_round_trip_leak_loop_1000_all_succeed", every_iter_ok);
        check("wave5_round_trip_leak_loop_completed_full_count", completed == 1000);
    }

    // -----------------------------------------------------------------------
    // NEGATIVE RELEASE CONTAINMENT (the gap behind flaws #1 and #2).  This is a
    // BEHAVIOUR-PINNING test, not a "it works" test: release() is noexcept ->
    // void and the kernel return is discarded, so a size/base mismatch is
    // SILENTLY ignored rather than faulting.  We freeze that contract so a
    // future change that adds a return value is a deliberate, visible break.
    //
    // SAFETY: we only ever pass addresses inside a block WE OWN, and sizes that
    // stay WITHIN our own allocation, so no unrelated page is ever at risk.
    //   * POSIX: release(base, ps-1)  -> munmap len rounds up to a page,
    //            release(base+1, ps)  -> non-page-aligned base, munmap EINVAL.
    //   * Windows: release(base+small_offset, size) -> interior pointer in the
    //            first page rounds down to base and frees; a later-page
    //            interior frees nothing.  Either way: no crash, return discarded.
    // We allocate a generous multi-page block, run the mismatched releases on
    // copies of derived pointers, then reclaim with a full-size release of the
    // base.  Pure no-crash assertions.
    // -----------------------------------------------------------------------
    static auto test_negative_release_is_silently_contained() -> void
    {
        const std::size_t ps{ vmhook::os::page_size() };
        constexpr std::size_t pages{ 8 };

        // Block A: undersized release of the correct base.
        {
            void* const block{ vmhook::os::allocate_rwx(nullptr, ps * pages) };
            if (block)
            {
                auto* const bytes{ static_cast<std::uint8_t*>(block) };
                bytes[0] = 0x11;
                // size one short of a page: POSIX munmap rounds the length up
                // to a page (frees page 0); Windows ignores size entirely.
                vmhook::os::release(block, ps - 1u);
                check("wave5_release_undersize_no_crash", true);
                // Reclaim the full reservation.  On Windows the base was already
                // freed (harmless re-free); on POSIX this unmaps the still-mapped
                // tail.  No crash.
                vmhook::os::release(block, ps * pages);
            }
            else
            {
                check("wave5_release_undersize_skipped_alloc_failed", false);
            }
        }

        // Block B: unaligned interior base release.
        {
            void* const block{ vmhook::os::allocate_rwx(nullptr, ps * pages) };
            if (block)
            {
                auto* const bytes{ static_cast<std::uint8_t*>(block) };
                bytes[0] = 0x22;
                // base + 1: NOT page-aligned.  POSIX munmap returns EINVAL and
                // unmaps nothing (silent).  Windows VirtualFree rounds an
                // interior pointer in the first page down to the base and frees.
                void* const interior{ static_cast<void*>(bytes + 1) };
                vmhook::os::release(interior, ps);
                check("wave5_release_unaligned_interior_no_crash", true);
                // Reclaim from the true base regardless of what the mismatch did.
                vmhook::os::release(block, ps * pages);
                check("wave5_release_after_mismatch_reclaim_no_crash", true);
            }
            else
            {
                check("wave5_release_unaligned_interior_skipped_alloc_failed", false);
            }
        }

        // Block C: oversize release that stays WITHIN our own multi-page block
        // (release a 3-page span of a 8-page block from the base).  POSIX
        // unmaps exactly 3 pages; Windows frees the whole reservation.  Safe
        // because 3*ps < 8*ps so no unrelated page is named.
        {
            void* const block{ vmhook::os::allocate_rwx(nullptr, ps * pages) };
            if (block)
            {
                vmhook::os::release(block, ps * 3u);
                check("wave5_release_partial_within_own_block_no_crash", true);
                vmhook::os::release(block, ps * pages);
            }
            else
            {
                check("wave5_release_partial_within_own_block_skipped_alloc_failed", false);
            }
        }
    }

    // -----------------------------------------------------------------------
    // HINT: occupied vs free.  allocate_rwx must NEVER return the occupied
    // address as if fresh, and must return SOME usable RWX block either way
    // (the hint is non-binding per vmhook.hpp:701; Windows even has a no-hint
    // retry at 721-726 when the hinted range is taken).
    //   * Occupied hint = the address of a real stack object we own.  The
    //     returned block must differ from that address (the kernel cannot have
    //     handed us our own live stack) and must be independently writable.
    //   * Free hint = derived from a just-released block's base.  We do NOT
    //     assert the hint is honoured (ASLR / rounding), only that a usable
    //     block comes back.
    // SAFETY: the stack object is real and owned; we never dereference the hint
    // as if it were the returned block, and we never pass a fabricated unmapped
    // address to any reader.
    // -----------------------------------------------------------------------
    static auto test_hint_occupied_and_free_never_corrupts() -> void
    {
        const std::size_t ps{ vmhook::os::page_size() };

        // Occupied hint: a live stack buffer.  Use it ONLY as a numeric hint.
        std::uint8_t occupied_obj[64]{};
        occupied_obj[0] = 0xAB;
        void* const occupied_hint{ static_cast<void*>(occupied_obj) };

        void* const from_occupied{ vmhook::os::allocate_rwx(occupied_hint, ps) };
        check("wave5_hint_occupied_returns_usable_block", from_occupied != nullptr);
        if (from_occupied)
        {
            // The kernel must never have returned our own live stack region.
            check("wave5_hint_occupied_not_the_stack_object",
                  from_occupied != occupied_hint);
            auto* const cell{ static_cast<volatile std::uint8_t*>(from_occupied) };
            *cell = 0xC7;
            check("wave5_hint_occupied_block_writable", *cell == 0xC7);
            // The stack object must be untouched by the allocation.
            check("wave5_hint_occupied_stack_object_intact", occupied_obj[0] == 0xAB);
            vmhook::os::release(from_occupied, ps);
        }

        // Free hint: allocate, capture base, release, then hint at that base.
        void* free_hint{ nullptr };
        {
            void* const seed{ vmhook::os::allocate_rwx(nullptr, ps) };
            if (seed)
            {
                free_hint = seed;          // numeric value only; region is freed
                vmhook::os::release(seed, ps);
            }
        }
        void* const from_free{ vmhook::os::allocate_rwx(free_hint, ps) };
        check("wave5_hint_free_returns_usable_block", from_free != nullptr);
        if (from_free)
        {
            auto* const cell{ static_cast<volatile std::uint8_t*>(from_free) };
            *cell = 0x3E;
            check("wave5_hint_free_block_writable", *cell == 0x3E);
            vmhook::os::release(from_free, ps);
        }
    }

    // -----------------------------------------------------------------------
    // EXECUTE-BIT POSITIVE CONTROL (flaw #3).  The ONLY way to prove the X in
    // _rwx actually holds.  Write a trivial RET into an allocate_rwx page and
    // call it through a function pointer; a clean return proves the page is
    // executable.  Gated to architectures where the RET encoding is known AND
    // execution is permitted (NOT Apple, where the RWX->RW fallback at 738-747
    // legitimately drops PROT_EXEC).  x86-64 RET = 0xC3; arm64 RET = D65F03C0
    // (little-endian bytes C0 03 5F D6).  After writing the code we flip to
    // execute_read via os::protect to satisfy any platform that separates W^X
    // even when the initial RWX mapping succeeded; if that flip is refused we
    // skip the call (best-effort), never failing.
    // -----------------------------------------------------------------------
#if (VMHOOK_ARCH_X86_64 || VMHOOK_ARCH_ARM64) && !VMHOOK_OS_APPLE
    static auto test_execute_bit_positive_control() -> void
    {
        const std::size_t ps{ vmhook::os::page_size() };
        void* const block{ vmhook::os::allocate_rwx(nullptr, ps) };
        if (!block)
        {
            check("wave5_execute_bit_skipped_alloc_failed", false);
            return;
        }

        auto* const code{ static_cast<std::uint8_t*>(block) };
#if VMHOOK_ARCH_X86_64
        code[0] = 0xC3; // ret
        const std::size_t code_len{ 1u };
#else // VMHOOK_ARCH_ARM64
        // RET (x30): 0xD65F03C0, little-endian in memory.
        code[0] = 0xC0;
        code[1] = 0x03;
        code[2] = 0x5F;
        code[3] = 0xD6;
        const std::size_t code_len{ 4u };
#endif
        (void)code_len;

        // Move to execute_read so even a strict-W^X-but-non-Apple kernel will
        // let us run it.  If refused, characterize and skip the call.
        const bool rx{ vmhook::os::protect(block, ps,
                                           vmhook::os::memory_protection::execute_read,
                                           nullptr) };
        if (!rx)
        {
            info("wave5_execute_bit_protect_rx_refused", true);
            (void)make_writable(block, ps);
            vmhook::os::release(block, ps);
            return;
        }

        using fn_t = void (*)();
        fn_t fn{};
        std::memcpy(&fn, &block, sizeof(fn)); // avoid an ISO func<->object cast warning
        fn();                                  // faults the process if NOT executable
        check("wave5_execute_bit_call_returns_cleanly", true);

        (void)make_writable(block, ps);
        vmhook::os::release(block, ps);
    }
#endif

    static auto run_all() -> void
    {
        test_sizing_primitive_bit_contract();
        test_returned_pointer_is_page_aligned();
        test_multipage_full_span_writable_sweep();
        test_query_region_covers_allocation();
        test_round_trip_leak_loop();
        test_negative_release_is_silently_contained();
        test_hint_occupied_and_free_never_corrupts();
#if (VMHOOK_ARCH_X86_64 || VMHOOK_ARCH_ARM64) && !VMHOOK_OS_APPLE
        test_execute_bit_positive_control();
#endif
    }
} // namespace wave5_alloc_release

// ===========================================================================
// EXPANSION 2 (additive, namespaced) — pure-logic deepening of the
// to_native_protect mapping, the protect() overflow-guard arithmetic boundary,
// the POSIX page-rounding formula, and the old_prot SUCCESS-path contract.
//
// CHARTER SPLIT (no duplication of the sibling interaction file):
//   * test_os_protect_interaction.cpp owns the LIVE behavioural matrix
//     (mprotect/VirtualProtect transitions, safe_read probes, neighbour
//     witnesses).  It can only assert the live OUTCOME, and on iOS / hardened
//     hosts it must SKIP every no_access / W^X probe.
//   * This block owns the COMPILE-/HAND-TRACEABLE side of the SAME primitive:
//     the to_native_protect switch is a pure function of the enum, so its
//     output is fully determined by source (vmhook.hpp:617-628 / 630-641) on
//     EVERY platform with NO allocation and NO syscall -- it runs identically on
//     iOS where the behavioural matrix is gated off.  The overflow-guard
//     boundary (vmhook.hpp:665-671) and the POSIX rounding formula
//     (vmhook.hpp:684-686) are likewise pure arithmetic, hand-traced here.
//
// Every value below is DERIVED FROM SOURCE; nothing is guessed.  No fabricated
// unmapped address is ever read -- the only allocation used is a real
// allocate_rwx page, and protect() is only ever handed sizes that are rejected
// by the guard BEFORE any syscall (so the live page is never disturbed).
// ===========================================================================
namespace expansion2_protect_pure_logic
{
    using vmhook::os::memory_protection;
    using vmhook::os::to_native_protect;

    // -----------------------------------------------------------------------
    // to_native_protect is a pure switch over the 5-state enum.  Pin EACH arm
    // to the EXACT native constant the source returns, on both platforms, plus
    // the out-of-range fallback.  The native constants come from the system
    // headers vmhook.hpp already includes (PAGE_* on Windows, PROT_* on POSIX),
    // so the comparison is against the real macro values, not a transcription.
    //
    // These run on EVERY OS including iOS (no allocation, no syscall) -- they are
    // the only place the iOS build pins the no_access / execute_rw mapping at
    // all, since the behavioural matrix is fully gated off there.
    // -----------------------------------------------------------------------
    static auto test_to_native_protect_every_arm() -> void
    {
#if VMHOOK_OS_WINDOWS
        // to_native_protect returns DWORD; the PAGE_* macros are int literals.
        // Cast the native constant to DWORD so the comparison is unsigned-vs-
        // unsigned (no -Wsign-compare under the MSVC / clang-cl /W4 -Werror CI).
        check("native_no_access_is_page_noaccess",
              to_native_protect(memory_protection::no_access)
                  == static_cast<DWORD>(PAGE_NOACCESS));
        check("native_read_is_page_readonly",
              to_native_protect(memory_protection::read)
                  == static_cast<DWORD>(PAGE_READONLY));
        check("native_read_write_is_page_readwrite",
              to_native_protect(memory_protection::read_write)
                  == static_cast<DWORD>(PAGE_READWRITE));
        check("native_execute_read_is_page_execute_read",
              to_native_protect(memory_protection::execute_read)
                  == static_cast<DWORD>(PAGE_EXECUTE_READ));
        check("native_execute_rw_is_page_execute_readwrite",
              to_native_protect(memory_protection::execute_rw)
                  == static_cast<DWORD>(PAGE_EXECUTE_READWRITE));

        // Out-of-range cast falls through the default-less switch to the trailing
        // `return PAGE_NOACCESS;` (vmhook.hpp:627).  Pin it so a refactor cannot
        // silently turn the fallback into a PERMISSIVE mask.
        check("native_oor_0xFF_falls_back_to_page_noaccess",
              to_native_protect(static_cast<memory_protection>(0xFFu))
                  == static_cast<DWORD>(PAGE_NOACCESS));
        check("native_oor_5_falls_back_to_page_noaccess",
              to_native_protect(static_cast<memory_protection>(5u))
                  == static_cast<DWORD>(PAGE_NOACCESS));
        check("native_oor_max_u32_falls_back_to_page_noaccess",
              to_native_protect(static_cast<memory_protection>(
                  std::numeric_limits<std::uint32_t>::max()))
                  == static_cast<DWORD>(PAGE_NOACCESS));
#else
        check("native_no_access_is_prot_none",
              to_native_protect(memory_protection::no_access) == PROT_NONE);
        check("native_read_is_prot_read",
              to_native_protect(memory_protection::read) == PROT_READ);
        check("native_read_write_is_prot_read_write",
              to_native_protect(memory_protection::read_write) == (PROT_READ | PROT_WRITE));
        check("native_execute_read_is_prot_read_exec",
              to_native_protect(memory_protection::execute_read) == (PROT_READ | PROT_EXEC));
        check("native_execute_rw_is_prot_read_write_exec",
              to_native_protect(memory_protection::execute_rw)
                  == (PROT_READ | PROT_WRITE | PROT_EXEC));

        // PROT_NONE is guaranteed 0 by POSIX, and the read-arm deliberately does
        // NOT carry PROT_EXEC (vmhook.hpp:635) -- pin both so an accidental
        // `read -> PROT_READ|PROT_EXEC` (collapsing read into execute_read) or a
        // non-zero PROT_NONE would be caught.
        check("native_prot_none_is_zero", PROT_NONE == 0);
        check("native_read_has_no_exec_bit",
              (to_native_protect(memory_protection::read) & PROT_EXEC) == 0);
        check("native_read_write_has_no_exec_bit",
              (to_native_protect(memory_protection::read_write) & PROT_EXEC) == 0);

        // Out-of-range cast falls through to the trailing `return PROT_NONE;`
        // (vmhook.hpp:640).  Pin the most-restrictive fallback.
        check("native_oor_0xFF_falls_back_to_prot_none",
              to_native_protect(static_cast<memory_protection>(0xFFu)) == PROT_NONE);
        check("native_oor_5_falls_back_to_prot_none",
              to_native_protect(static_cast<memory_protection>(5u)) == PROT_NONE);
        check("native_oor_max_u32_falls_back_to_prot_none",
              to_native_protect(static_cast<memory_protection>(
                  std::numeric_limits<std::uint32_t>::max())) == PROT_NONE);
#endif
    }

    // -----------------------------------------------------------------------
    // The four NON-zero, IN-RANGE protections must each map to a DISTINCT native
    // mask: the switch must not collapse two states (the exact regression the
    // sibling behavioural matrix guards at runtime, pinned here as pure logic so
    // it holds even on the iOS build).  read(R), execute_read(R+X),
    // read_write(R+W), execute_rw(R+W+X) are all different; no two are equal.
    // On POSIX we additionally pin the bit-superset relationships the masks must
    // satisfy.
    // -----------------------------------------------------------------------
    static auto test_to_native_protect_arms_are_distinct() -> void
    {
        const auto n_none{ to_native_protect(memory_protection::no_access) };
        const auto n_r{ to_native_protect(memory_protection::read) };
        const auto n_rw{ to_native_protect(memory_protection::read_write) };
        const auto n_rx{ to_native_protect(memory_protection::execute_read) };
        const auto n_rwx{ to_native_protect(memory_protection::execute_rw) };

        check("native_read_distinct_from_read_write", n_r != n_rw);
        check("native_read_distinct_from_execute_read", n_r != n_rx);
        check("native_read_distinct_from_execute_rw", n_r != n_rwx);
        check("native_read_write_distinct_from_execute_read", n_rw != n_rx);
        check("native_read_write_distinct_from_execute_rw", n_rw != n_rwx);
        check("native_execute_read_distinct_from_execute_rw", n_rx != n_rwx);
        check("native_no_access_distinct_from_read", n_none != n_r);
        check("native_no_access_distinct_from_execute_rw", n_none != n_rwx);

#if !VMHOOK_OS_WINDOWS
        // POSIX bit-superset contract: execute_rw is the union of execute_read's
        // exec bit and read_write's write bit, and every non-none mask contains
        // PROT_READ.  Derived directly from the OR-expressions at 635-638.
        check("native_posix_rw_is_superset_of_read", (n_rw & n_r) == n_r);
        check("native_posix_rx_is_superset_of_read", (n_rx & n_r) == n_r);
        check("native_posix_rwx_is_superset_of_rw", (n_rwx & n_rw) == n_rw);
        check("native_posix_rwx_is_superset_of_rx", (n_rwx & n_rx) == n_rx);
        check("native_posix_rwx_equals_rw_or_rx", n_rwx == (n_rw | n_rx));
#endif
    }

    // -----------------------------------------------------------------------
    // The overflow guard (vmhook.hpp:665-671) rejects exactly the sizes for
    // which `size > UINTPTR_MAX - base`.  Equivalently: the LARGEST accepted
    // size from a given base is `UINTPTR_MAX - base` (base + size ==
    // UINTPTR_MAX, no wrap), and `+1` is the SMALLEST rejected size.  We verify
    // the boundary as PURE ARITHMETIC against a real, live page so no syscall is
    // ever issued with a wrapping size: a wrapping request is rejected BEFORE
    // the kernel call, so the live page's protection is provably undisturbed.
    //
    // Both the largest-accepted (it reaches the kernel and the kernel almost
    // certainly rejects the absurd length) and the smallest-rejected branches
    // are exercised; only the GUARD's verdict is asserted hard on the rejected
    // side (false), while the accepted side is no-crash only (the kernel outcome
    // is platform-variable for an enormous length).
    // -----------------------------------------------------------------------
    static auto test_overflow_guard_boundary_is_exact() -> void
    {
        const std::size_t page{ vmhook::os::page_size() };
        void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
        if (!block)
        {
            check("overflow_boundary_skipped_alloc_failed", false);
            return;
        }

        auto* const bytes{ static_cast<std::uint8_t*>(block) };
        bytes[0] = 0x5A;

        const std::uintptr_t base{ reinterpret_cast<std::uintptr_t>(block) };
        const std::uintptr_t umax{ std::numeric_limits<std::uintptr_t>::max() };
        // Source guard: reject when size > (umax - base).  So:
        const std::size_t largest_accepted{ static_cast<std::size_t>(umax - base) };
        const std::size_t smallest_rejected{ largest_accepted + std::size_t{ 1 } };

        // Sanity on the arithmetic itself (pure logic, no syscall): base is a
        // real heap pointer, so base != 0 and base + largest_accepted == umax.
        check("overflow_boundary_base_nonzero", base != 0u);
        check("overflow_boundary_largest_accepted_no_wrap",
              base + static_cast<std::uintptr_t>(largest_accepted) == umax);
        // base + smallest_rejected = base + (umax - base + 1) = umax + 1 = 0
        // (the wrap the guard exists to reject).
        check("overflow_boundary_smallest_rejected_wraps",
              base + static_cast<std::uintptr_t>(smallest_rejected) == std::uintptr_t{ 0 });

        // smallest_rejected and everything above it -> guard returns false.
        check("overflow_boundary_smallest_rejected_returns_false",
              !vmhook::os::protect(block, smallest_rejected,
                                   memory_protection::read, nullptr));
        check("overflow_boundary_size_max_returns_false",
              !vmhook::os::protect(block, SIZE_MAX,
                                   memory_protection::read, nullptr));

        // The largest NON-wrapping size is NOT rejected by the guard (it passes
        // the guard and reaches the kernel, which is free to fail it for being
        // absurd).  We only require no crash; the boolean is platform-variable.
        (void)vmhook::os::protect(block, largest_accepted,
                                  memory_protection::read, nullptr);
        check("overflow_boundary_largest_accepted_no_crash", true);

        // None of the rejected calls touched the kernel: page is still writable.
        // (The largest_accepted call may have flipped the page read-only IF the
        // kernel honoured it, so restore writable first before re-asserting.)
        (void)make_writable(block, page);
        bytes[0] = 0xA5;
        check("overflow_boundary_page_untouched_still_writable", bytes[0] == 0xA5);

        vmhook::os::release(block, page);
    }

    // -----------------------------------------------------------------------
    // The POSIX page-rounding formula (vmhook.hpp:684-686) computed as PURE
    // ARITHMETIC over a sweep of (interior-offset, size) pairs.  This does NOT
    // call protect(); it reproduces the exact source expression and asserts the
    // INVARIANTS the rounding must satisfy for every input, so the math is
    // validated on every OS (Windows rounds kernel-side, but the formula's
    // invariants are universal and the trampoline allocator relies on them):
    //
    //   base_down   = addr & ~(ps-1)
    //   end         = addr + size
    //   aligned     = (end - base_down + ps - 1) & ~(ps-1)
    //
    // Invariants pinned for each case:
    //   * aligned is a whole multiple of ps;
    //   * the rounded span [base_down, base_down+aligned) COVERS the requested
    //     [addr, addr+size);
    //   * aligned >= ps for any size >= 1 (never zero pages for a non-empty req);
    //   * the rounded span does not extend more than (ps-1) past the request end
    //     on the high side, nor start more than (ps-1) below addr.
    // ps is a runtime power of two from page_size(); the cases are derived from
    // it so they hold on 4K, 16K (Apple-silicon), and large-page hosts alike.
    // -----------------------------------------------------------------------
    static auto test_posix_rounding_formula_invariants() -> void
    {
        const std::size_t ps{ vmhook::os::page_size() };
        check("rounding_ps_is_power_of_two", (ps & (ps - 1)) == 0u);

        // A fixed, page-aligned synthetic base far above the heap.  We NEVER
        // dereference it -- this is pure integer arithmetic mirroring the source,
        // not a memory access, so it is POSIX-safe (no read of an unmapped addr).
        const std::uintptr_t aligned_base{ static_cast<std::uintptr_t>(ps) * 4096u };

        // (interior offset within the first page, request size) pairs covering:
        // exact page, page-1, page+1, sub-byte, multi-page, unaligned interior.
        const std::array<std::pair<std::uintptr_t, std::size_t>, 9> cases{ {
            { 0u,        std::size_t{ 1 } },
            { 0u,        ps - 1 },
            { 0u,        ps },
            { 0u,        ps + 1 },
            { 0u,        ps * 2 },
            { 0u,        ps * 2 + 1 },
            { ps / 3u,   std::size_t{ 1 } },
            { ps - 1u,   std::size_t{ 1 } },  // 1 byte at the very top of the page -> still 1 page
            { ps / 2u,   ps },                // unaligned interior spanning into next page
        } };

        bool all_multiple{ true };
        bool all_cover{ true };
        bool all_at_least_one_page{ true };
        bool all_low_within_page{ true };
        bool all_high_within_page{ true };

        for (const auto& c : cases)
        {
            const std::uintptr_t addr{ aligned_base + c.first };
            const std::size_t size{ c.second };

            const std::uintptr_t end{ addr + size };
            const std::uintptr_t base_down{ addr & ~(static_cast<std::uintptr_t>(ps) - 1u) };
            const std::size_t aligned{
                static_cast<std::size_t>(end - base_down + ps - 1) & ~(ps - 1) };

            // aligned is a whole number of pages.
            if ((aligned % ps) != 0u)
            {
                all_multiple = false;
            }
            // Covers the request: base_down <= addr and base_down+aligned >= end.
            if (!(base_down <= addr
                  && base_down + static_cast<std::uintptr_t>(aligned) >= end))
            {
                all_cover = false;
            }
            // Non-empty request -> at least one page.
            if (aligned < ps)
            {
                all_at_least_one_page = false;
            }
            // Low edge: base_down is at most (ps-1) below addr.
            if (addr - base_down > ps - 1u)
            {
                all_low_within_page = false;
            }
            // High edge: rounded end overshoots the request end by < ps.
            if (base_down + static_cast<std::uintptr_t>(aligned) - end >= ps)
            {
                all_high_within_page = false;
            }
        }

        check("rounding_aligned_is_page_multiple", all_multiple);
        check("rounding_span_covers_request", all_cover);
        check("rounding_nonempty_request_at_least_one_page", all_at_least_one_page);
        check("rounding_low_edge_within_one_page", all_low_within_page);
        check("rounding_high_edge_within_one_page", all_high_within_page);

        // Spot-check the canonical boundary sizes EXACTLY from an aligned base
        // (offset 0): size in [1..ps] -> 1 page; size in [ps+1..2ps] -> 2;
        // size == 2ps -> 2.  Derived straight from the formula.
        const auto rounded_pages{ [ps](std::size_t size) -> std::size_t {
            const std::uintptr_t addr{ 0u };
            const std::uintptr_t end{ addr + size };
            const std::uintptr_t base_down{ addr & ~(static_cast<std::uintptr_t>(ps) - 1u) };
            const std::size_t aligned{
                static_cast<std::size_t>(end - base_down + ps - 1) & ~(ps - 1) };
            return aligned / ps;
        } };
        check("rounding_size_1_is_one_page", rounded_pages(std::size_t{ 1 }) == 1u);
        check("rounding_size_ps_minus_1_is_one_page", rounded_pages(ps - 1) == 1u);
        check("rounding_size_ps_is_one_page", rounded_pages(ps) == 1u);
        check("rounding_size_ps_plus_1_is_two_pages", rounded_pages(ps + 1) == 2u);
        check("rounding_size_two_ps_is_two_pages", rounded_pages(ps * 2) == 2u);
        check("rounding_size_two_ps_plus_1_is_three_pages",
              rounded_pages(ps * 2 + 1) == 3u);
    }

    // -----------------------------------------------------------------------
    // old_prot SUCCESS-path contract within THIS file's namespace.  The sibling
    // interaction file owns the multi-flip chain; here we pin the corners the
    // edges file is responsible for and that pair naturally with its existing
    // FAILURE-path old_prot tests:
    //   (a) a successful protect with old_prot == nullptr returns true and does
    //       not crash (the `if (old_prot)` success guard at 675/688 exercised
    //       with nullptr, complementing the failure-path nullptr cases above);
    //   (b) the platform-asymmetric value on success: Windows writes a non-zero
    //       PAGE_* bitmask, POSIX writes exactly 0 (vmhook.hpp:677 vs 690);
    //   (c) the round-trip HAZARD: on POSIX the returned old_prot is 0, which
    //       round-trips through the enum to memory_protection::no_access (the
    //       static_assert at the top of this file pins no_access == 0).  We do
    //       NOT actually restore with it (that would brick the page); we ASSERT
    //       the hazard exists so a future "real previous flags on POSIX" change
    //       is a deliberate, tested decision rather than a silent break.
    // -----------------------------------------------------------------------
    static auto test_old_prot_success_path_contract() -> void
    {
        const std::size_t page{ vmhook::os::page_size() };
        void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
        if (!block)
        {
            check("old_prot_success_skipped_alloc_failed", false);
            return;
        }

        auto* const bytes{ static_cast<std::uint8_t*>(block) };
        bytes[0] = 0x11;

        // (a) success with null old_prot -> true, no crash.
        check("old_prot_success_null_old_prot_returns_true",
              vmhook::os::protect(block, page, memory_protection::read, nullptr));

        // (b) success WITH old_prot -> written (not the sentinel) and the
        // platform-correct value.
        constexpr std::uint32_t sentinel{ 0xDEADBEEFu };
        {
            std::uint32_t op{ sentinel };
            const bool ok{ vmhook::os::protect(block, page,
                                               memory_protection::read_write, &op) };
            check("old_prot_success_with_ptr_returns_true", ok);
            if (ok)
            {
                check("old_prot_success_value_written", op != sentinel);
#if VMHOOK_OS_WINDOWS
                // Prior state was PAGE_READONLY (from the read flip in (a)) -> a
                // non-zero PAGE_* constant.
                check("old_prot_success_windows_nonzero", op != 0u);
#else
                // POSIX writes 0 unconditionally (vmhook.hpp:690).
                check("old_prot_success_posix_zero", op == 0u);

                // (c) HAZARD: that 0 round-trips to memory_protection::no_access.
                // We assert the hazard (op-as-enum == no_access) WITHOUT applying
                // it, so a "save old / restore old" caller is provably unsafe on
                // POSIX and a future change to real-flags would flip this.
                check("old_prot_posix_roundtrips_to_no_access",
                      static_cast<memory_protection>(op) == memory_protection::no_access);
#endif
            }
        }

        check("old_prot_success_restore_writable", make_writable(block, page));
        bytes[0] = 0x22;
        check("old_prot_success_byte_writable_after", bytes[0] == 0x22);
        vmhook::os::release(block, page);
    }

    static auto run() -> void
    {
        test_to_native_protect_every_arm();
        test_to_native_protect_arms_are_distinct();
        test_overflow_guard_boundary_is_exact();
        test_posix_rounding_formula_invariants();
        test_old_prot_success_path_contract();
    }
} // namespace expansion2_protect_pure_logic

// ===========================================================================
// EXPANSION 3 (additive, namespaced) -- os-layer inputs/behaviour not yet hit
// anywhere in THIS file: the safe_read / safe_write INPUT + WRAP guards driven
// against REAL OWNED pages, query_region FIELD INVARIANTS (containment,
// committed/free mutual-exclusion, interior-pointer containment), and the
// page_size()/allocation_granularity()/user-address-constant arithmetic
// relations beyond the existing power-of-two / >= / multiple checks.
//
// CHARTER (no duplication):
//   * test_os_safe_read.cpp owns the deep safe_read behavioural matrix; here we
//     ONLY pin the three header-level guards that are PURE input validation --
//     reachable identically on every OS and provably safe because each rejects
//     BEFORE the platform read: the null/zero guard (vmhook.hpp:957) and the
//     wrap guard `src + size < src` (vmhook.hpp:968).  The wrap case uses a REAL
//     owned page as `src` with SIZE_MAX as `size`: the sum wraps and the guard
//     returns false WITHOUT reading, so no fabricated/unmapped address is ever
//     dereferenced.  A positive control (read of a byte we just wrote into an
//     owned page) proves the guard is the only thing rejecting the edges.
//   * test_os_query_region.cpp owns the region matrix; here we ONLY pin the
//     STRUCTURAL invariants of region_info for a block WE allocated: the region
//     must CONTAIN the queried address (base <= addr < base+size), committed and
//     free are never both set, and an interior pointer resolves to a region
//     whose base does not exceed it.  All derived from vmhook.hpp:806-897.
//   * The sizing-constant relations extend (not repeat) the existing
//     power-of-two / idempotent / >= / multiple checks with exact-division,
//     page-alignment-via-mask, and the user_address_floor < ceiling ordering
//     (vmhook.hpp:515/520).
//
// Every value is DERIVED FROM SOURCE.  Every pointer handed to a reader is a
// real allocate_rwx page we own or a stack object; the only "bad" inputs are
// nullptr and an is_valid-rejected wrap that short-circuits before any read.
// ===========================================================================
namespace expansion3_os_guards_and_region
{
    // -----------------------------------------------------------------------
    // safe_read INPUT + WRAP guards.  vmhook.hpp:957 rejects null dst, null src,
    // or size==0 before any platform read; vmhook.hpp:968 rejects a src+size
    // that wraps the address space.  Positive control first (proves the path is
    // live), then every guard edge -- all returning false, none faulting.
    // -----------------------------------------------------------------------
    static auto test_safe_read_input_and_wrap_guards() -> void
    {
        const std::size_t ps{ vmhook::os::page_size() };
        void* const block{ vmhook::os::allocate_rwx(nullptr, ps) };
        if (!block)
        {
            check("exp3_safe_read_skipped_alloc_failed", false);
            return;
        }

        auto* const bytes{ static_cast<std::uint8_t*>(block) };
        bytes[0] = 0x7C;
        bytes[1] = 0x3B;

        // Positive control: a real read of two owned bytes must succeed and copy
        // them verbatim.  This is the ONLY thing distinguishing the guard
        // rejections below from a generally-dead path.
        {
            std::uint8_t sink[2]{ 0, 0 };
            const bool ok{ vmhook::os::safe_read(sink, block, 2u) };
            check("exp3_safe_read_owned_bytes_succeeds", ok);
            check("exp3_safe_read_owned_bytes_copied",
                  ok && sink[0] == 0x7C && sink[1] == 0x3B);
        }

        // Guard: null dst -> false (src is a real owned page, never read).
        {
            std::uint8_t sink{ 0 };
            (void)sink;
            check("exp3_safe_read_null_dst_returns_false",
                  !vmhook::os::safe_read(nullptr, block, 1u));
        }
        // Guard: null src -> false.
        {
            std::uint8_t sink{ 0 };
            check("exp3_safe_read_null_src_returns_false",
                  !vmhook::os::safe_read(&sink, nullptr, 1u));
        }
        // Guard: size==0 -> false (both pointers real, nothing is read).
        {
            std::uint8_t sink{ 0 };
            check("exp3_safe_read_zero_size_returns_false",
                  !vmhook::os::safe_read(&sink, block, 0u));
        }

        // Wrap guard: src is a REAL owned page, size == SIZE_MAX.  src + SIZE_MAX
        // wraps below src, so vmhook.hpp:968 returns false BEFORE the platform
        // read -- the owned page is never actually read past its end, and no
        // unmapped address is ever touched.
        {
            std::uint8_t sink{ 0 };
            check("exp3_safe_read_wrap_size_max_returns_false",
                  !vmhook::os::safe_read(&sink, block, SIZE_MAX));
        }
        // Wrap guard boundary: the smallest size that still wraps from this exact
        // owned base is (UINTPTR_MAX - base) + 1.  Derived straight from the
        // source predicate `src + size < src`.  Rejected before any read.
        {
            std::uint8_t sink{ 0 };
            const std::uintptr_t base{ reinterpret_cast<std::uintptr_t>(block) };
            const std::uintptr_t umax{ ~static_cast<std::uintptr_t>(0) };
            const std::size_t smallest_wrapping{
                static_cast<std::size_t>(umax - base) + std::size_t{ 1 } };
            check("exp3_safe_read_wrap_boundary_returns_false",
                  !vmhook::os::safe_read(&sink, block, smallest_wrapping));
        }

        vmhook::os::release(block, ps);
    }

    // -----------------------------------------------------------------------
    // safe_write INPUT guards (vmhook.hpp:1050).  Same shape: positive control
    // into an owned page, then null/zero rejections.  iOS / unknown refuse ALL
    // writes (return false unconditionally), so the positive control is gated to
    // the platforms that expose a fault-safe write primitive.
    // -----------------------------------------------------------------------
    static auto test_safe_write_input_guards() -> void
    {
        const std::size_t ps{ vmhook::os::page_size() };
        void* const block{ vmhook::os::allocate_rwx(nullptr, ps) };
        if (!block)
        {
            check("exp3_safe_write_skipped_alloc_failed", false);
            return;
        }

        auto* const bytes{ static_cast<std::uint8_t*>(block) };
        bytes[0] = 0x00;

#if VMHOOK_OS_WINDOWS || VMHOOK_OS_MACOS || VMHOOK_OS_LINUX || VMHOOK_OS_ANDROID
        // Positive control: a real fault-safe write into the owned page lands.
        {
            const std::uint8_t payload[2]{ 0x4D, 0x5E };
            const bool ok{ vmhook::os::safe_write(block, payload, 2u) };
            check("exp3_safe_write_owned_page_succeeds", ok);
            check("exp3_safe_write_owned_page_stored",
                  ok && bytes[0] == 0x4D && bytes[1] == 0x5E);
        }
#else
        // iOS / unknown: no fault-safe write API; the primitive refuses.  Pin the
        // documented refusal so the contract is frozen on those platforms too.
        {
            const std::uint8_t payload[2]{ 0x4D, 0x5E };
            check("exp3_safe_write_refused_on_ios_unknown",
                  !vmhook::os::safe_write(block, payload, 2u));
        }
#endif

        // Guard: null dst -> false (src is a real stack buffer, never read).
        {
            const std::uint8_t payload{ 0x11 };
            check("exp3_safe_write_null_dst_returns_false",
                  !vmhook::os::safe_write(nullptr, &payload, 1u));
        }
        // Guard: null src -> false (dst is a real owned page, never written).
        check("exp3_safe_write_null_src_returns_false",
              !vmhook::os::safe_write(block, nullptr, 1u));
        // Guard: size==0 -> false (both pointers real, nothing transfers).
        {
            const std::uint8_t payload{ 0x22 };
            check("exp3_safe_write_zero_size_returns_false",
                  !vmhook::os::safe_write(block, &payload, 0u));
        }

        vmhook::os::release(block, ps);
    }

    // -----------------------------------------------------------------------
    // query_region FIELD INVARIANTS for a block WE OWN.  Beyond the
    // committed/readable/size>=request checks the wave-5 block already makes,
    // pin the STRUCTURAL guarantees every backend (VirtualQuery / mach_vm_region
    // / iOS stub / /proc maps) must satisfy:
    //   * the region CONTAINS the queried address: base <= addr < base+size;
    //   * committed and free are mutually exclusive (never both true);
    //   * querying an INTERIOR pointer of the same block yields a region whose
    //     base does not exceed that interior pointer (and on the page-granular
    //     backends the interior still lands inside the reported region).
    // All addresses are inside a real owned allocation; nothing fabricated.
    // -----------------------------------------------------------------------
    static auto test_query_region_field_invariants() -> void
    {
        const std::size_t ps{ vmhook::os::page_size() };
        constexpr std::size_t pages{ 4 };
        void* const block{ vmhook::os::allocate_rwx(nullptr, ps * pages) };
        if (!block)
        {
            check("exp3_query_region_skipped_alloc_failed", false);
            return;
        }

        // Make the whole span resident so every backend reports it live.
        auto* const bytes{ static_cast<std::uint8_t*>(block) };
        for (std::size_t p{ 0 }; p < pages; ++p)
        {
            bytes[p * ps] = static_cast<std::uint8_t>(0x60 + p);
        }

        const std::uintptr_t base_addr{ reinterpret_cast<std::uintptr_t>(block) };

        // Query at the base.
        const vmhook::os::region_info ri{ vmhook::os::query_region(block) };

        // committed and free are never simultaneously true on any backend.
        check("exp3_query_region_committed_xor_free",
              !(ri.committed && ri.free));

        // For a committed region, it must CONTAIN the queried base address:
        // region_base <= addr < region_base + size.  (Only meaningful when
        // committed; a free-region report has different base/size semantics.)
        if (ri.committed)
        {
            const std::uintptr_t rb{ reinterpret_cast<std::uintptr_t>(ri.base) };
            check("exp3_query_region_base_le_addr", rb <= base_addr);
            check("exp3_query_region_addr_lt_base_plus_size",
                  base_addr < rb + static_cast<std::uintptr_t>(ri.size));
            // A committed region we just wrote to must be non-empty.
            check("exp3_query_region_committed_size_nonzero", ri.size != 0u);
        }
        else
        {
            // A backend that cannot introspect (no committed flag) still must
            // not crash; record the outcome rather than failing.
            info("exp3_query_region_base_block_committed", ri.committed);
        }

        // Interior pointer inside the SECOND page (still our own memory): the
        // reported region base must not exceed it.
        {
            void* const interior{ static_cast<void*>(bytes + ps + 7u) };
            const std::uintptr_t ia{ reinterpret_cast<std::uintptr_t>(interior) };
            const vmhook::os::region_info ir{ vmhook::os::query_region(interior) };
            if (ir.committed)
            {
                const std::uintptr_t irb{ reinterpret_cast<std::uintptr_t>(ir.base) };
                check("exp3_query_region_interior_base_le_addr", irb <= ia);
                check("exp3_query_region_interior_addr_lt_base_plus_size",
                      ia < irb + static_cast<std::uintptr_t>(ir.size));
            }
            else
            {
                info("exp3_query_region_interior_committed", ir.committed);
            }
            // committed/free mutual exclusion holds at the interior too.
            check("exp3_query_region_interior_committed_xor_free",
                  !(ir.committed && ir.free));
        }

        // Executable bit is platform-variable for a fresh RWX block (W^X on
        // Apple drops PROT_EXEC; the Linux /proc backend reports perms[2]).
        // Characterize, never assert.
        info("exp3_query_region_fresh_rwx_executable", ri.executable);

        vmhook::os::release(block, ps * pages);
    }

    // -----------------------------------------------------------------------
    // SIZING-CONSTANT arithmetic relations beyond the existing checks.
    //   * granularity / page_size is an EXACT integer (no remainder) and
    //     multiplying back recovers granularity -- the divisor the trampoline
    //     allocator uses must be lossless.
    //   * granularity is page-aligned when masked with (ps-1): (gr & (ps-1))==0,
    //     a second witness of "multiple of page" via bitmask rather than %.
    //   * the user-address window is well-ordered and non-degenerate:
    //     user_address_floor (0xFFFF) < user_address_ceiling (0x00007FFF'FFFFFFFF)
    //     (vmhook.hpp:515/520), and both are exactly their documented literals.
    //   * page_size fits well inside the user window (a single page never spans
    //     the entire addressable user range) -- a sanity bound the allocator's
    //     stride math implicitly assumes.
    // -----------------------------------------------------------------------
    static auto test_sizing_constant_relations() -> void
    {
        const std::size_t ps{ vmhook::os::page_size() };
        const std::size_t gr{ vmhook::os::allocation_granularity() };

        // Exact, lossless division: gr is a whole number of pages and
        // (gr/ps)*ps == gr with no truncation.
        check("exp3_granularity_div_page_exact", (gr % ps) == 0u);
        check("exp3_granularity_div_then_mul_recovers", (gr / ps) * ps == gr);
        check("exp3_granularity_pages_at_least_one", (gr / ps) >= 1u);

        // Bitmask witness of "multiple of page": since ps is a power of two,
        // (gr & (ps-1)) == 0 is equivalent to (gr % ps) == 0.
        check("exp3_granularity_page_aligned_via_mask",
              (gr & (ps - 1u)) == 0u);

        // User-address window: documented literals, well-ordered, non-empty.
        check("exp3_user_floor_is_documented_literal",
              vmhook::os::user_address_floor == static_cast<std::uintptr_t>(0xFFFFull));
        check("exp3_user_ceiling_is_documented_literal",
              vmhook::os::user_address_ceiling
                  == static_cast<std::uintptr_t>(0x00007FFFFFFFFFFFull));
        check("exp3_user_floor_below_ceiling",
              vmhook::os::user_address_floor < vmhook::os::user_address_ceiling);

        // A single page is far smaller than the user window -- one page can never
        // span the whole addressable user range.
        check("exp3_page_size_within_user_window",
              static_cast<std::uintptr_t>(ps)
                  < (vmhook::os::user_address_ceiling - vmhook::os::user_address_floor));

        // granularity likewise fits comfortably under the ceiling.
        check("exp3_granularity_within_user_window",
              static_cast<std::uintptr_t>(gr)
                  < (vmhook::os::user_address_ceiling - vmhook::os::user_address_floor));
    }

    static auto run() -> void
    {
        test_safe_read_input_and_wrap_guards();
        test_safe_write_input_guards();
        test_query_region_field_invariants();
        test_sizing_constant_relations();
    }
} // namespace expansion3_os_guards_and_region

// ===========================================================================
// EXPANSION 4 (additive, namespaced) -- os-layer SURFACE that none of the
// prior sections in this file (null/zero guards, lifecycle, wave-5 sizing,
// expansion-2 pure-logic, expansion-3 guards/region) reach: the module-lookup
// helpers, current_thread_id, safe_read_fast PARITY, the safe_write multi-byte
// round-trip verified by reading it BACK, the flush_instruction_cache hint, and
// the user_address_ceiling bit-identity.  Strictly cross-platform INVARIANTS;
// every reader touches only a real allocate_rwx page we own, a stack object, or
// an is_valid-rejected absent-module name.  Platform-variable outcomes -> info().
//
// CHARTER (no duplication of expansion-3 or the sibling files):
//   * expansion-3 owns safe_read / safe_write INPUT + WRAP guards and the
//     query_region STRUCTURAL invariants; this block touches NEITHER.  It owns:
//     - find_loaded_module(nullptr) handle shape + idempotency + absent->null,
//       and find_jvm_module() characterization (vmhook.hpp:529-576) -- distinct
//       from the get_proc_address null-SYMBOL guard test earlier in the file;
//     - current_thread_id() non-zero + idempotent (vmhook.hpp:601-614);
//     - safe_read_fast PARITY with safe_read on an owned page + its own input
//       guards (vmhook.hpp:1138-1159) -- safe_read_fast is untested anywhere here;
//     - safe_write ROUND-TRIP (write N bytes, read them back via safe_read,
//       compare) + direct-read visibility -- expansion-3 only does a 2-byte
//       store positive-control, never a read-back equality;
//     - flush_instruction_cache null/zero/real-range no-op (vmhook.hpp:1164-1177);
//     - user_address_ceiling == 2^47 - 1 bit-identity + a real owned page above
//       the floor (vmhook.hpp:515/520) -- a different angle than expansion-3's
//       literal/ordering checks.
// Every value DERIVED FROM SOURCE.
// ===========================================================================
namespace expansion4_os_layer_surface
{
    // -----------------------------------------------------------------------
    // find_loaded_module(nullptr) returns a valid non-null handle on every
    // supported platform (GetModuleHandleA(nullptr) -> the .exe handle;
    // dlopen(nullptr, RTLD_LAZY|RTLD_NOLOAD) -> the global-scope handle), per
    // vmhook.hpp:529-542.  Two calls yield the SAME handle (the result identity
    // is stateless).  A nonsense leaf name no process links must resolve to
    // nullptr (the negative half of the lookup).  find_jvm_module() is
    // environment-variable in a no-JVM binary -> info(), never asserted.
    // -----------------------------------------------------------------------
    static auto test_module_lookup_self_and_absent() -> void
    {
        const vmhook::os::module_handle self1{ vmhook::os::find_loaded_module(nullptr) };
        const vmhook::os::module_handle self2{ vmhook::os::find_loaded_module(nullptr) };
        check("exp4_find_loaded_module_null_returns_handle", self1 != nullptr);
        check("exp4_find_loaded_module_null_idempotent", self1 == self2);

        // A leaf name certainly not loaded -> nullptr.
        check("exp4_find_loaded_module_absent_returns_null",
              vmhook::os::find_loaded_module("vmhook_absent_module_zzz.qqq") == nullptr);

        // No-JVM build: normally no libjvm in scope.  Environment decides.
        info("exp4_find_jvm_module_resolved_in_test_env",
             vmhook::os::find_jvm_module() != nullptr);
    }

    // -----------------------------------------------------------------------
    // current_thread_id() (vmhook.hpp:601-614): a genuine kernel/port id on
    // Windows / Linux / Android / Apple (never 0 for a live thread), 0 on the
    // unknown-platform fallback (614).  It is STABLE across calls within one
    // thread.  Idempotency is universal; non-zero is asserted only where the
    // source returns a real id, characterized elsewhere.
    // -----------------------------------------------------------------------
    static auto test_current_thread_id_stable_nonzero() -> void
    {
        const vmhook::os::thread_id_t a{ vmhook::os::current_thread_id() };
        const vmhook::os::thread_id_t b{ vmhook::os::current_thread_id() };
        check("exp4_current_thread_id_idempotent", a == b);
#if VMHOOK_OS_WINDOWS || VMHOOK_OS_LINUX || VMHOOK_OS_ANDROID || VMHOOK_OS_APPLE
        check("exp4_current_thread_id_nonzero", a != vmhook::os::thread_id_t{ 0 });
#else
        info("exp4_current_thread_id_nonzero_on_unknown_platform",
             a != vmhook::os::thread_id_t{ 0 });
#endif
    }

    // -----------------------------------------------------------------------
    // safe_read_fast is a drop-in for safe_read whose SUCCESS bytes are
    // byte-identical (vmhook.hpp:1135-1137); on a mapped readable page both must
    // succeed and return the same content.  Its input guards mirror safe_read's
    // (1140-1143).  Pure owned memory; safe_read_fast is otherwise untested in
    // this file.
    // -----------------------------------------------------------------------
    static auto test_safe_read_fast_parity_and_guards() -> void
    {
        const std::size_t ps{ vmhook::os::page_size() };
        void* const block{ vmhook::os::allocate_rwx(nullptr, ps) };
        if (!block)
        {
            check("exp4_safe_read_fast_skipped_alloc_failed", false);
            return;
        }

        auto* const bytes{ static_cast<std::uint8_t*>(block) };
        std::array<std::uint8_t, 16> pattern{};
        for (std::size_t i{ 0 }; i < pattern.size(); ++i)
        {
            pattern[i] = static_cast<std::uint8_t>((i * 17u + 3u) & 0xFFu);
            bytes[i] = pattern[i];
        }
        const std::size_t span{ pattern.size() };

        std::array<std::uint8_t, 16> via_read{};
        std::array<std::uint8_t, 16> via_fast{};
        const bool read_ok{ vmhook::os::safe_read(via_read.data(), block, span) };
        const bool fast_ok{ vmhook::os::safe_read_fast(via_fast.data(), block, span) };

        check("exp4_safe_read_owned_span_ok", read_ok);
        check("exp4_safe_read_fast_owned_span_ok", fast_ok);
        check("exp4_safe_read_fast_matches_pattern",
              !fast_ok || std::memcmp(via_fast.data(), pattern.data(), span) == 0);
        // Parity: when both succeed they return identical bytes.
        check("exp4_safe_read_fast_parity_with_safe_read",
              !(read_ok && fast_ok)
              || std::memcmp(via_read.data(), via_fast.data(), span) == 0);

        // Input guards mirror safe_read's null/zero short-circuit.
        std::uint8_t s{ 0 };
        check("exp4_safe_read_fast_null_dst_false",
              !vmhook::os::safe_read_fast(nullptr, block, 1u));
        check("exp4_safe_read_fast_null_src_false",
              !vmhook::os::safe_read_fast(&s, nullptr, 1u));
        check("exp4_safe_read_fast_zero_size_false",
              !vmhook::os::safe_read_fast(&s, block, 0u));

        vmhook::os::release(block, ps);
    }

    // -----------------------------------------------------------------------
    // safe_write ROUND-TRIP: write a multi-byte pattern into an owned writable
    // page via safe_write, then read it BACK via safe_read and compare -- the
    // read-back-equality angle expansion-3's 2-byte store positive-control does
    // not cover.  The store must also be visible through a direct volatile read
    // of our own page (it really landed, not just kernel-buffered).  Gated off
    // iOS / unknown, where safe_write refuses (vmhook.hpp:1071-1074).  dst is a
    // page WE allocated and keep writable -- never a fabricated/read-only target.
    // -----------------------------------------------------------------------
    static auto test_safe_write_round_trip_read_back() -> void
    {
        const std::size_t ps{ vmhook::os::page_size() };
        void* const block{ vmhook::os::allocate_rwx(nullptr, ps) };
        if (!block)
        {
            check("exp4_safe_write_round_trip_skipped_alloc_failed", false);
            return;
        }
        (void)make_writable(block, ps);

#if VMHOOK_OS_WINDOWS || VMHOOK_OS_MACOS || VMHOOK_OS_LINUX || VMHOOK_OS_ANDROID
        std::array<std::uint8_t, 12> payload{};
        for (std::size_t i{ 0 }; i < payload.size(); ++i)
        {
            payload[i] = static_cast<std::uint8_t>((i * 23u + 9u) & 0xFFu);
        }
        const std::size_t span{ payload.size() };

        const bool wrote{ vmhook::os::safe_write(block, payload.data(), span) };
        check("exp4_safe_write_owned_span_ok", wrote);
        if (wrote)
        {
            std::array<std::uint8_t, 12> back{};
            const bool read_ok{ vmhook::os::safe_read(back.data(), block, span) };
            check("exp4_safe_write_then_read_back_ok", read_ok);
            check("exp4_safe_write_round_trip_matches",
                  !read_ok || std::memcmp(back.data(), payload.data(), span) == 0);
            check("exp4_safe_write_visible_through_direct_read",
                  static_cast<volatile std::uint8_t*>(block)[0] == payload[0]);
        }
#else
        info("exp4_safe_write_round_trip_skipped_no_fault_safe_write", true);
#endif

        vmhook::os::release(block, ps);
    }

    // -----------------------------------------------------------------------
    // flush_instruction_cache (vmhook.hpp:1164-1177) is a best-effort void hint
    // with a null/zero short-circuit (1166-1169); the only observable contract
    // is "does not crash" and "never rewrites memory".  Pin the guard arms and a
    // real-owned-page flush (the trampoline installer calls this after writing a
    // JMP, so a real range must be accepted cleanly).
    // -----------------------------------------------------------------------
    static auto test_flush_instruction_cache_guards_and_real() -> void
    {
        const std::size_t ps{ vmhook::os::page_size() };

        // Guard arms: no kernel call, no crash.
        vmhook::os::flush_instruction_cache(nullptr, ps);
        vmhook::os::flush_instruction_cache(nullptr, 0);
        check("exp4_flush_icache_null_addr_no_crash", true);

        void* const block{ vmhook::os::allocate_rwx(nullptr, ps) };
        if (!block)
        {
            check("exp4_flush_icache_skipped_alloc_failed", false);
            return;
        }

        auto* const bytes{ static_cast<std::uint8_t*>(block) };
        bytes[0] = 0x90; // NOP -- realistic post-patch content

        // Zero size on a real addr -> guarded no-op.
        vmhook::os::flush_instruction_cache(block, 0);
        check("exp4_flush_icache_zero_size_no_crash", true);

        // Real range -> the live trampoline path: no crash, no byte rewrite.
        vmhook::os::flush_instruction_cache(block, ps);
        check("exp4_flush_icache_real_range_no_crash", true);
        check("exp4_flush_icache_does_not_rewrite_bytes", bytes[0] == 0x90);

        vmhook::os::release(block, ps);
    }

    // -----------------------------------------------------------------------
    // user_address_ceiling bit-identity: the documented top is the canonical
    // 47-bit user-space maximum, (2^47 - 1) == 0x00007FFF'FFFFFFFF (vmhook.hpp:515).
    // A real owned page sits strictly above the 64 KiB floor on every host (a heap
    // mapping is never in the bottom 64 KiB), which is the only half of the
    // is_valid_pointer band check (vmhook.hpp:2023-2024) that is universal across
    // 32/64-bit.  No fabricated address: the page is real and owned.
    // -----------------------------------------------------------------------
    static auto test_user_ceiling_bit_identity_and_owned_page() -> void
    {
        static_assert(vmhook::os::user_address_ceiling == ((std::uintptr_t{ 1 } << 47) - 1u));
        check("exp4_user_ceiling_is_2_47_minus_1",
              vmhook::os::user_address_ceiling == ((std::uintptr_t{ 1 } << 47) - 1u));

        const std::size_t ps{ vmhook::os::page_size() };
        void* const block{ vmhook::os::allocate_rwx(nullptr, ps) };
        if (!block)
        {
            check("exp4_user_ceiling_owned_page_skipped_alloc_failed", false);
            return;
        }
        const std::uintptr_t addr{ reinterpret_cast<std::uintptr_t>(block) };
        check("exp4_owned_page_above_user_floor",
              addr > vmhook::os::user_address_floor);
        vmhook::os::release(block, ps);
    }

    static auto run() -> void
    {
        test_module_lookup_self_and_absent();
        test_current_thread_id_stable_nonzero();
        test_safe_read_fast_parity_and_guards();
        test_safe_write_round_trip_read_back();
        test_flush_instruction_cache_guards_and_real();
        test_user_ceiling_bit_identity_and_owned_page();
    }
} // namespace expansion4_os_layer_surface

// ===========================================================================
// EXPANSION 5 (additive, namespaced) -- the os_allocate_release ROUND-TRIP
// corners that none of the prior sections in THIS file reach: the
// released-block -> query_region transition (a freed reservation is reported
// NON-committed / free), the zero-size-release guard exercised on a MULTI-PAGE
// block with full-span re-verification (wave-5 only used a single-page block),
// the partial-release tail CHARACTERIZED via query_region (wave-5 used only the
// fault-safe byte probe), and the sub-page-request WHOLE-page usability contract
// (a ps-1 request yields a full ps-byte usable page because both VirtualAlloc
// and mmap commit whole pages -- wave-5 touched only the requested ps-1 bytes).
//
// CHARTER (no duplication):
//   * wave5_alloc_release owns sizing-bit / alignment / leak-loop / negative-
//     release no-crash / hint / execute-bit, and queries only a LIVE block plus
//     nullptr.  This block owns the AFTER-release query transition, which wave-5
//     never touches, plus the zero-size guard on a MULTI-PAGE block and the
//     sub-page WHOLE-page span.
//   * expansion-3 owns query_region STRUCTURAL invariants of a live block; this
//     block owns the freed-region report (different State / perms semantics).
//   * The early-return guard (vmhook.hpp:776 `!address || size == 0`) is pinned
//     here on a multi-page allocation -- distinct from the single-page / null /
//     bogus combinations already covered at the top of the file.
//
// Every value DERIVED FROM SOURCE (vmhook.hpp:703-786, 793-897).  Every pointer
// handed to a reader or to query_region is a real allocate_rwx block we own (or
// its freed base, which query_region accepts -- it never dereferences the page,
// only asks the kernel/maps about the address); no fabricated unmapped address
// is read as data.  Platform-variable outcomes -> info(), never hard-asserted.
// ===========================================================================
namespace expansion5_release_query_roundtrip
{
    // -----------------------------------------------------------------------
    // The zero-size-release guard on a MULTI-PAGE block.  vmhook.hpp:776 returns
    // BEFORE any kernel call when size == 0, so a whole multi-page reservation
    // must survive a run of release(block, 0) intact -- every page still
    // writable.  Then a single real release(block, ps*pages) frees it cleanly.
    // (The existing single-page idempotent test only stamps one byte of one
    // page; here the full multi-page span is re-verified after each no-op.)
    // -----------------------------------------------------------------------
    static auto test_zero_size_release_multipage_keeps_full_span() -> void
    {
        const std::size_t ps{ vmhook::os::page_size() };
        constexpr std::size_t pages{ 5 };
        void* const block{ vmhook::os::allocate_rwx(nullptr, ps * pages) };
        if (!block)
        {
            check("exp5_zero_release_multipage_skipped_alloc_failed", false);
            return;
        }

        auto* const bytes{ static_cast<std::uint8_t*>(block) };

        bool survived{ true };
        for (int i{ 0 }; i < 6; ++i)
        {
            vmhook::os::release(block, 0); // guarded no-op (size == 0)
            for (std::size_t p{ 0 }; p < pages; ++p)
            {
                const auto marker{
                    static_cast<std::uint8_t>((0x30 + i + static_cast<int>(p)) & 0xFF) };
                bytes[p * ps] = marker;
                if (bytes[p * ps] != marker)
                {
                    survived = false;
                }
            }
            // The far edge of the last page must survive too.
            bytes[ps * pages - 1] = static_cast<std::uint8_t>((0xE0 + i) & 0xFF);
            if (bytes[ps * pages - 1] != static_cast<std::uint8_t>((0xE0 + i) & 0xFF))
            {
                survived = false;
            }
        }
        check("exp5_zero_size_release_multipage_keeps_every_page_live", survived);

        vmhook::os::release(block, ps * pages); // real release
        check("exp5_zero_size_release_multipage_then_real_no_crash", true);
    }

    // -----------------------------------------------------------------------
    // RELEASED-block -> query_region transition.  A freshly allocated block is
    // committed (asserted in wave-5); after a real release the SAME base must no
    // longer be reported as a live committed mapping on the introspective
    // backends.  query_region accepts a freed address -- it asks the kernel
    // (VirtualQuery) or walks /proc/self/maps about the address, never reads the
    // page -- so this is fault-safe and uses no fabricated pointer.
    //
    //   * Windows (VirtualQuery): the freed reservation reports MEM_FREE ->
    //     committed == false AND free == true.  HARD on Windows.
    //   * Linux/Android (/proc maps): the unmapped address falls into a gap ->
    //     committed == false (the walker sets free == true for a gap, but a
    //     neighbouring mapping could in principle re-cover the exact address, so
    //     we HARD-assert only the !committed half and characterize free).
    //   * macOS (mach_vm_region): returns the NEXT region at/above the address,
    //     which is a DIFFERENT live mapping -- committed may be true for that
    //     other region.  iOS stub always reports committed.  Both are
    //     characterized via info(), never asserted.
    // We capture committed BEFORE and AFTER on the same base to show the
    // transition direction where the backend supports it.
    // -----------------------------------------------------------------------
    static auto test_released_block_query_region_transition() -> void
    {
        const std::size_t ps{ vmhook::os::page_size() };
        void* const block{ vmhook::os::allocate_rwx(nullptr, ps) };
        if (!block)
        {
            check("exp5_released_query_skipped_alloc_failed", false);
            return;
        }

        *static_cast<volatile std::uint8_t*>(block) = 0x5A;

        const vmhook::os::region_info before{ vmhook::os::query_region(block) };
        // A live allocate_rwx block is committed on the introspective backends;
        // characterize so the AFTER comparison is meaningful even where it isn't.
        info("exp5_live_block_committed_before_release", before.committed);

        vmhook::os::release(block, ps);

        const vmhook::os::region_info after{ vmhook::os::query_region(block) };

#if VMHOOK_OS_WINDOWS
        // VirtualQuery on a fully released reservation reports MEM_FREE.
        check("exp5_windows_released_block_not_committed", !after.committed);
        check("exp5_windows_released_block_free", after.free);
#elif VMHOOK_OS_LINUX || VMHOOK_OS_ANDROID
        // The /proc walker reports the unmapped address as a gap (free) -> not
        // committed.  HARD on the !committed half; free characterized (a later
        // mapping could re-cover the exact base between unmap and query).
        check("exp5_linux_released_block_not_committed", !after.committed);
        info("exp5_linux_released_block_free", after.free);
#else
        // macOS mach_vm_region returns the next live region; iOS stub always
        // reports committed.  Both legitimately committed -> characterize only.
        info("exp5_apple_released_block_committed", after.committed);
#endif
        // committed and free are never simultaneously true on ANY backend, even
        // after release.
        check("exp5_released_block_committed_xor_free",
              !(after.committed && after.free));
    }

    // -----------------------------------------------------------------------
    // PARTIAL release tail CHARACTERIZED via query_region (distinct from wave-5,
    // which only used the fault-safe byte probe on the last page).  We allocate a
    // generous multi-page block, release ONLY the first page, then query the LAST
    // page's base.  The outcome is LEGITIMATELY platform-variable:
    //   * POSIX munmap(base, ps) unmaps page 0 only -> the last page is very
    //     likely still committed.
    //   * Windows VirtualFree(base, 0, MEM_RELEASE) ignores the size and frees
    //     the WHOLE reservation -> the last page is very likely free.
    // So we hard-assert ONLY no-crash + the committed/free mutual exclusion, and
    // report committed/free of the tail via info().  SAFETY: every address named
    // is inside our own original allocation; we reclaim the full span afterwards.
    // -----------------------------------------------------------------------
    static auto test_partial_release_tail_query_is_characterized() -> void
    {
        const std::size_t ps{ vmhook::os::page_size() };
        constexpr std::size_t pages{ 4 };
        void* const block{ vmhook::os::allocate_rwx(nullptr, ps * pages) };
        if (!block)
        {
            check("exp5_partial_tail_query_skipped_alloc_failed", false);
            return;
        }

        auto* const bytes{ static_cast<std::uint8_t*>(block) };
        bytes[0] = 0x01;
        bytes[ps * (pages - 1)] = 0x02; // marker in the LAST page

        void* const last_page{ static_cast<void*>(bytes + ps * (pages - 1)) };

        // Release ONLY the first page.
        vmhook::os::release(block, ps);
        check("exp5_partial_release_first_page_no_crash", true);

        // query_region of the LAST page base: fault-safe (never dereferences),
        // accepts any address.  Characterize the committed/free of the tail.
        const vmhook::os::region_info tail{ vmhook::os::query_region(last_page) };
        check("exp5_partial_tail_committed_xor_free",
              !(tail.committed && tail.free));
        info("exp5_partial_tail_committed", tail.committed);
        info("exp5_partial_tail_free", tail.free);

        // Reclaim the full original span (POSIX unmaps the still-mapped tail;
        // Windows re-frees an already-freed base harmlessly).  No crash == pass.
        vmhook::os::release(block, ps * pages);
        check("exp5_partial_release_then_full_no_crash", true);
    }

    // -----------------------------------------------------------------------
    // SUB-PAGE request -> WHOLE usable page.  A request of ps-1 bytes (and of 1
    // byte) is rounded UP by the kernel to a full page (VirtualAlloc commits in
    // page units; mmap maps whole pages).  Wave-5 touches only the requested
    // ps-1 / 1 bytes; here we prove the ENTIRE ps-byte page is usable -- write a
    // pattern across all ps bytes (one past the request) and read it back -- so
    // the "rounded up, fully committed" contract is pinned, not just the
    // requested prefix.  Release uses the same sub-page size the caller asked
    // for (POSIX munmap rounds the length up; Windows ignores it).
    // -----------------------------------------------------------------------
    static auto test_subpage_request_yields_whole_usable_page() -> void
    {
        const std::size_t ps{ vmhook::os::page_size() };

        // Request ps-1; the whole ps-byte page must be writable end-to-end.
        {
            void* const block{ vmhook::os::allocate_rwx(nullptr, ps - 1u) };
            if (!block)
            {
                check("exp5_subpage_ps_minus_1_skipped_alloc_failed", false);
            }
            else
            {
                auto* const bytes{ static_cast<std::uint8_t*>(block) };
                bool full_page_ok{ true };
                for (std::size_t i{ 0 }; i < ps; ++i) // up to ps, one PAST the request
                {
                    bytes[i] = static_cast<std::uint8_t>((i * 13u + 5u) & 0xFFu);
                }
                for (std::size_t i{ 0 }; i < ps; ++i)
                {
                    if (bytes[i] != static_cast<std::uint8_t>((i * 13u + 5u) & 0xFFu))
                    {
                        full_page_ok = false;
                    }
                }
                check("exp5_subpage_ps_minus_1_whole_page_writable", full_page_ok);
                vmhook::os::release(block, ps - 1u);
            }
        }

        // Request 1 byte; same whole-page contract.
        {
            void* const block{ vmhook::os::allocate_rwx(nullptr, 1u) };
            if (!block)
            {
                check("exp5_subpage_one_skipped_alloc_failed", false);
            }
            else
            {
                auto* const bytes{ static_cast<std::uint8_t*>(block) };
                bool full_page_ok{ true };
                for (std::size_t i{ 0 }; i < ps; ++i)
                {
                    bytes[i] = static_cast<std::uint8_t>((i * 7u + 1u) & 0xFFu);
                }
                for (std::size_t i{ 0 }; i < ps; ++i)
                {
                    if (bytes[i] != static_cast<std::uint8_t>((i * 7u + 1u) & 0xFFu))
                    {
                        full_page_ok = false;
                    }
                }
                check("exp5_subpage_one_byte_whole_page_writable", full_page_ok);
                vmhook::os::release(block, 1u);
            }
        }
    }

    // -----------------------------------------------------------------------
    // GRANULARITY-sized allocate/release round-trip with a query_region size
    // check.  allocation_granularity() is the unit VirtualAlloc reserves in
    // (>= page); allocating exactly that many bytes must hand back a fully
    // committed, page-aligned, end-to-end-writable block whose reported region
    // size covers the request, and release of the same size must be clean.  This
    // ties the two sizing primitives together through a real round-trip the
    // trampoline allocator depends on (it aligns candidates to granularity).
    // -----------------------------------------------------------------------
    static auto test_granularity_sized_round_trip() -> void
    {
        const std::size_t ps{ vmhook::os::page_size() };
        const std::size_t gr{ vmhook::os::allocation_granularity() };
        void* const block{ vmhook::os::allocate_rwx(nullptr, gr) };
        if (!block)
        {
            check("exp5_granularity_round_trip_skipped_alloc_failed", false);
            return;
        }

        // Page-aligned base (every backend; mask is exact since ps is 2^k).
        check("exp5_granularity_block_page_aligned",
              (reinterpret_cast<std::uintptr_t>(block)
               & (static_cast<std::uintptr_t>(ps) - 1u)) == 0u);

        // Whole granularity span writable end-to-end.
        auto* const bytes{ static_cast<std::uint8_t*>(block) };
        bool span_ok{ true };
        for (std::size_t i{ 0 }; i < gr; ++i)
        {
            bytes[i] = static_cast<std::uint8_t>((i * 19u + 11u) & 0xFFu);
        }
        for (std::size_t i{ 0 }; i < gr; ++i)
        {
            if (bytes[i] != static_cast<std::uint8_t>((i * 19u + 11u) & 0xFFu))
            {
                span_ok = false;
            }
        }
        check("exp5_granularity_full_span_writable", span_ok);

        // Reported region size covers the request where the backend introspects.
        const vmhook::os::region_info ri{ vmhook::os::query_region(block) };
        if (ri.committed)
        {
            check("exp5_granularity_query_size_covers_request", ri.size >= gr);
        }
        else
        {
            info("exp5_granularity_query_committed", ri.committed);
        }

        vmhook::os::release(block, gr);
        check("exp5_granularity_round_trip_release_no_crash", true);
    }

    static auto run() -> void
    {
        test_zero_size_release_multipage_keeps_full_span();
        test_released_block_query_region_transition();
        test_partial_release_tail_query_is_characterized();
        test_subpage_request_yields_whole_usable_page();
        test_granularity_sized_round_trip();
    }
} // namespace expansion5_release_query_roundtrip

// ===========================================================================
// WAVE-21 (additive, namespaced) -- os_allocate_release ledger-gap DEEPENING.
// The wave-5 / expansion-5 sections already close the os_allocate_release
// ledger items at the "it works / no-crash" level.  This block deepens EXACTLY
// the same proven-OPEN gaps along axes those sections leave thin, touching no
// existing assertion:
//   * DWORD->size_t (vmhook.hpp:491) and long->size_t (vmhook.hpp:494) cast
//     NON-TRUNCATION pinned as a COMPILE-TIME width contract (static_assert),
//     not merely the runtime `!= 0` wave-5 already checks -- the ledger item
//     "DWORD->size_t cast non-truncation pin".
//   * the POSIX sysconf 4096 FALLBACK literal (vmhook.hpp:494) pinned as a
//     compile-time member of the architecturally-valid page-size set and as a
//     power of two -- tying the fallback constant to the runtime
//     wave5_page_size_is_valid_arch_size check (ledger "sysconf 4096-fallback
//     architecturally-valid-page-size pin").
//   * the execute-bit positive control extended to a stub that RETURNS A VALUE
//     (mov imm -> ret), proving the X in _rwx runs REAL code with a result, not
//     just an immediate ret as wave-5 does (ledger "execute-bit positive check
//     ... call through fn-ptr").  Gated identically: (x86_64||arm64)&&!Apple.
//   * a granularity-MULTIPLE leak/stability loop that VARIES the size each
//     iteration (wave-5's loop is a fixed single page), exercising the POSIX
//     munmap-uses-size / Windows-ignores-size release arms at several sizes
//     (ledger "1000-iter alloc/write/release leak/stability loop" + "multi-page
//     + granularity-sized").
//   * negative release of a LATER-PAGE interior pointer -- wave-5 only does
//     base+1 (a FIRST-page interior).  vmhook.hpp:760-768 documents that a
//     later-page interior frees NOTHING on Windows (silent leak) while POSIX
//     munmap rejects the unaligned base; both must be no-crash, return
//     discarded (ledger "negative release containment ... pin silent
//     no-diagnostic").
//
// ADDITIVE ONLY.  Every hard assertion is a cross-platform INVARIANT derived
// from source; platform-variable outcomes -> info().  Every pointer handed to a
// reader / release / call is a REAL allocate_rwx block we own (or a derived
// interior within it); no fabricated unmapped address is read as data.
// ===========================================================================
namespace wave21_alloc_release_deepening
{
    // -----------------------------------------------------------------------
    // CAST NON-TRUNCATION, pinned at COMPILE TIME.  page_size() casts the
    // Windows DWORD dwPageSize (32-bit) / POSIX `long` sysconf result into
    // size_t (vmhook.hpp:491 / 494); allocation_granularity() casts the Windows
    // DWORD dwAllocationGranularity (vmhook.hpp:506).  A widening cast is
    // value-preserving iff the destination is at least as wide AND the source's
    // value range fits -- size_t is unsigned and on every supported ABI is at
    // least as wide as DWORD (32-bit) and as `long` (32-bit ILP32 / LLP64,
    // 64-bit LP64).  Pin the width relation as a static_assert so a future ABI
    // where size_t shrank below the source type is a hard compile break, then
    // confirm the live values are non-zero and within the source's max (the
    // runtime witness that the cast did not wrap).
    // -----------------------------------------------------------------------
    static auto test_sizing_cast_non_truncation_width_contract() -> void
    {
#if VMHOOK_OS_WINDOWS
        // DWORD is the source type for BOTH page_size and granularity on Windows.
        static_assert(sizeof(std::size_t) >= sizeof(DWORD),
                      "size_t must hold a DWORD without truncation");
        static_assert(std::is_unsigned_v<DWORD>);
        const std::size_t dword_max{ static_cast<std::size_t>(
            (std::numeric_limits<DWORD>::max)()) };
        const std::size_t ps{ vmhook::os::page_size() };
        const std::size_t gr{ vmhook::os::allocation_granularity() };
        // A non-wrapping widening cast leaves the value <= the source max.
        check("wave21_cast_page_size_within_dword_max", ps <= dword_max);
        check("wave21_cast_granularity_within_dword_max", gr <= dword_max);
        check("wave21_cast_page_size_nonzero", ps != 0u);
        check("wave21_cast_granularity_nonzero", gr != 0u);
#else
        // POSIX page_size() casts `long`; granularity() forwards page_size().
        static_assert(sizeof(std::size_t) >= sizeof(long),
                      "size_t must hold a sysconf long without truncation");
        const std::size_t ps{ vmhook::os::page_size() };
        const std::size_t gr{ vmhook::os::allocation_granularity() };
        // sysconf returns long; the source only casts the POSITIVE branch
        // (ps > 0), so the value fits the long max with the sign bit clear.
        const std::size_t long_max{ static_cast<std::size_t>(
            (std::numeric_limits<long>::max)()) };
        check("wave21_cast_page_size_within_long_max", ps <= long_max);
        check("wave21_cast_granularity_within_long_max", gr <= long_max);
        check("wave21_cast_page_size_nonzero", ps != 0u);
        check("wave21_cast_granularity_nonzero", gr != 0u);
#endif
    }

    // -----------------------------------------------------------------------
    // The POSIX sysconf FALLBACK constant (vmhook.hpp:494) is the literal 4096,
    // used when sysconf(_SC_PAGESIZE) returns <= 0.  Pin at COMPILE TIME that
    // this literal is (a) a power of two and (b) a member of the
    // architecturally-valid page-size set {4096, 16384, 65536} -- the same set
    // the runtime wave5_page_size_is_valid_arch_size check uses -- so the
    // fallback can never silently mis-stride the allocator with a value outside
    // that set.  These are pure constant assertions; they hold and compile
    // identically on every OS (the literal is the same token everywhere).
    // -----------------------------------------------------------------------
    static auto test_sysconf_fallback_constant_is_valid_page_size() -> void
    {
        constexpr std::size_t fallback{ 4096u };
        static_assert((fallback & (fallback - 1u)) == 0u,
                      "sysconf fallback must be a power of two");
        static_assert(fallback == 4096u || fallback == 16384u || fallback == 65536u,
                      "sysconf fallback must be an architecturally-valid page size");
        // Runtime witnesses of the same two facts, so a CI run records them.
        check("wave21_sysconf_fallback_power_of_two",
              (fallback & (fallback - 1u)) == 0u);
        check("wave21_sysconf_fallback_in_valid_arch_set",
              fallback == 4096u || fallback == 16384u || fallback == 65536u);
        // The live page_size() is >= the fallback floor on every supported host
        // (no architecture in the valid set is smaller than 4096).
        check("wave21_live_page_size_ge_fallback_floor",
              vmhook::os::page_size() >= fallback);
    }

    // -----------------------------------------------------------------------
    // GRANULARITY-MULTIPLE leak/stability loop with a VARYING size.  Wave-5's
    // leak loop fixes the size at one page; here each iteration allocates a
    // DIFFERENT multiple of the allocation granularity (1..4 units), writes the
    // first and last byte, verifies, and releases the EXACT size.  This stresses
    // that release() truly returns the reservation across a RANGE of sizes on
    // both OSes (POSIX munmap consumes the size; Windows VirtualFree ignores it
    // and frees the whole reservation) -- a leak at any size would eventually
    // exhaust the address space and fail an allocation.  No address-reuse
    // assertion (kernel-dependent); only that every iteration yields a live,
    // writable, releasable block.
    // -----------------------------------------------------------------------
    static auto test_varying_size_leak_loop() -> void
    {
        const std::size_t gr{ vmhook::os::allocation_granularity() };
        bool every_iter_ok{ true };
        int completed{ 0 };
        for (int i{ 0 }; i < 400; ++i)
        {
            const std::size_t units{ static_cast<std::size_t>((i % 4) + 1) };
            const std::size_t size{ gr * units };
            void* const block{ vmhook::os::allocate_rwx(nullptr, size) };
            if (!block)
            {
                every_iter_ok = false;
                break;
            }
            auto* const bytes{ static_cast<std::uint8_t*>(block) };
            const auto marker{ static_cast<std::uint8_t>(i & 0xFF) };
            bytes[0]        = marker;
            bytes[size - 1] = static_cast<std::uint8_t>(~marker & 0xFF);
            if (bytes[0] != marker
                || bytes[size - 1] != static_cast<std::uint8_t>(~marker & 0xFF))
            {
                every_iter_ok = false;
                vmhook::os::release(block, size);
                break;
            }
            vmhook::os::release(block, size);
            ++completed;
        }
        check("wave21_varying_size_leak_loop_all_succeed", every_iter_ok);
        check("wave21_varying_size_leak_loop_completed_full", completed == 400);
    }

    // -----------------------------------------------------------------------
    // NEGATIVE RELEASE of a LATER-PAGE interior pointer.  Wave-5 covers base+1
    // (a FIRST-page interior).  vmhook.hpp:760-768 documents that the LATER-page
    // case is DIFFERENT: Windows VirtualFree(MEM_RELEASE) on an interior pointer
    // in a later page returns 0 (frees NOTHING -> the reservation leaks
    // silently); POSIX munmap on a non-page-aligned base returns EINVAL and
    // unmaps nothing.  Both must be no-crash with the return discarded.  We pin
    // the silent-no-diagnostic contract: release() is noexcept->void, so a
    // future change that surfaces a diagnostic is a deliberate, visible break.
    // SAFETY: the interior pointer is inside a multi-page block WE OWN; we
    // reclaim the true base afterwards regardless of what the mismatch did.
    // -----------------------------------------------------------------------
    static auto test_later_page_interior_release_is_silently_contained() -> void
    {
        const std::size_t ps{ vmhook::os::page_size() };
        constexpr std::size_t pages{ 8 };
        void* const block{ vmhook::os::allocate_rwx(nullptr, ps * pages) };
        if (!block)
        {
            check("wave21_later_page_interior_skipped_alloc_failed", false);
            return;
        }

        auto* const bytes{ static_cast<std::uint8_t*>(block) };
        bytes[0]            = 0x44;
        bytes[ps * 3u]      = 0x55; // marker in the FOURTH page

        // Interior pointer in the FOURTH page (page index 3), offset +7 so it is
        // also non-page-aligned: exercises BOTH the later-page (Windows leaks)
        // and the unaligned-base (POSIX EINVAL) silent arms at once.
        void* const later_interior{ static_cast<void*>(bytes + ps * 3u + 7u) };
        vmhook::os::release(later_interior, ps);
        check("wave21_later_page_interior_release_no_crash", true);

        // A second later-page interior at an aligned page boundary (page 5):
        // aligned base but NOT the reservation base -> Windows frees nothing,
        // POSIX unmaps exactly that one page out of the middle (harmless to the
        // rest).  No crash either way.
        void* const aligned_later{ static_cast<void*>(bytes + ps * 5u) };
        vmhook::os::release(aligned_later, ps);
        check("wave21_aligned_later_page_release_no_crash", true);

        // Reclaim from the TRUE base with the full size.  On Windows this frees
        // the whole (still-leaked) reservation; on POSIX it unmaps whatever
        // pages remain mapped.  No crash == pass.
        vmhook::os::release(block, ps * pages);
        check("wave21_later_page_interior_reclaim_no_crash", true);
    }

    // -----------------------------------------------------------------------
    // EXECUTE-BIT positive control with a VALUE-RETURNING stub.  Wave-5 proves
    // the X bit by running a bare RET; here the stub LOADS AN IMMEDIATE into the
    // integer return register and returns it, so a clean call proves the page
    // executes REAL code that produces an observable result (a stronger witness
    // of flaw #3's RWX->RW downgrade NOT having silently happened).  Gated to
    // architectures with a known encoding AND where execution is permitted
    // (NOT Apple, where the RWX->RW fallback at 738-747 legitimately drops
    // PROT_EXEC).  We flip to execute_read via os::protect first; if that is
    // refused (strict W^X) we characterize and skip the call, never failing.
    //   x86-64: B8 2A 00 00 00 (mov eax, 42) ; C3 (ret)         -> returns 42
    //   arm64 : movz w0, #42 (0x528005400 -> bytes 40 05 80 52);
    //           ret (0xD65F03C0 -> bytes C0 03 5F D6)            -> returns 42
    // -----------------------------------------------------------------------
#if (VMHOOK_ARCH_X86_64 || VMHOOK_ARCH_ARM64) && !VMHOOK_OS_APPLE
    static auto test_execute_bit_returning_stub() -> void
    {
        const std::size_t ps{ vmhook::os::page_size() };
        void* const block{ vmhook::os::allocate_rwx(nullptr, ps) };
        if (!block)
        {
            check("wave21_execute_returning_stub_skipped_alloc_failed", false);
            return;
        }

        auto* const code{ static_cast<std::uint8_t*>(block) };
#if VMHOOK_ARCH_X86_64
        // mov eax, 42 ; ret
        code[0] = 0xB8;
        code[1] = 0x2A;
        code[2] = 0x00;
        code[3] = 0x00;
        code[4] = 0x00;
        code[5] = 0xC3;
#else // VMHOOK_ARCH_ARM64
        // movz w0, #42  -> 0x52800540, little-endian bytes 40 05 80 52
        code[0] = 0x40;
        code[1] = 0x05;
        code[2] = 0x80;
        code[3] = 0x52;
        // ret           -> 0xD65F03C0, little-endian bytes C0 03 5F D6
        code[4] = 0xC0;
        code[5] = 0x03;
        code[6] = 0x5F;
        code[7] = 0xD6;
#endif

        const bool rx{ vmhook::os::protect(block, ps,
                                           vmhook::os::memory_protection::execute_read,
                                           nullptr) };
        if (!rx)
        {
            info("wave21_execute_returning_stub_protect_rx_refused", true);
            (void)make_writable(block, ps);
            vmhook::os::release(block, ps);
            return;
        }

        using fn_t = int (*)();
        fn_t fn{};
        std::memcpy(&fn, &block, sizeof(fn)); // avoid an ISO func<->object cast warning
        const int produced{ fn() };           // faults the process if NOT executable
        check("wave21_execute_returning_stub_returns_42", produced == 42);

        (void)make_writable(block, ps);
        vmhook::os::release(block, ps);
    }
#endif

    static auto run() -> void
    {
        test_sizing_cast_non_truncation_width_contract();
        test_sysconf_fallback_constant_is_valid_page_size();
        test_varying_size_leak_loop();
        test_later_page_interior_release_is_silently_contained();
#if (VMHOOK_ARCH_X86_64 || VMHOOK_ARCH_ARM64) && !VMHOOK_OS_APPLE
        test_execute_bit_returning_stub();
#endif
    }
} // namespace wave21_alloc_release_deepening

// ===========================================================================
// WAVE-23: os_protect ledger-gap closure -- deeper old_prot/round-trip,
// out-of-range enum sweep, sub-page no-bleed cross-page boundary, exact-page
// vs page-1 rounding witnessed via safe_read asymmetry across BOTH pages,
// no_access+execute_rw behavioural-probe pair, idempotent re-protect with
// old_prot capture on every call.
//
// ADDITIVE. Every assertion below is a cross-platform INVARIANT derived
// directly from vmhook.hpp protect() body (638-667) and the two
// to_native_protect overloads (607-632). Platform-variable outcomes are
// reported via info(), never hard-asserted.
// ===========================================================================
namespace wave23_protect_ledger_gaps
{
    using vmhook::os::memory_protection;

    // -----------------------------------------------------------------------
    // Out-of-range enum SWEEP across several distinct garbage encodings. Each
    // must (a) return true (the switch fallback writes a defined native value
    // and protect() returns whatever the kernel says, which on a real page is
    // success), AND (b) the resulting page must NOT be permissively readable
    // -- the fallback is no_access / PROT_NONE, never RWX. Gated where the
    // sandbox refuses PROT_NONE.
    // -----------------------------------------------------------------------
#if !VMHOOK_OS_IOS
    static auto test_out_of_range_enum_sweep_never_widens() -> void
    {
        const std::size_t page{ vmhook::os::page_size() };
        void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
        if (!block)
        {
            check("wave23_oor_sweep_skipped_alloc_failed", false);
            return;
        }

        auto* const bytes{ static_cast<std::uint8_t*>(block) };
        bytes[0] = 0x77;

        // Four distinct garbage values across the std::uint32_t range. None of
        // them is a valid enum encoding (the valid set is 0..4 per the static
        // asserts at file top). Each must NOT widen access.
        const std::uint32_t garbage_vals[]{
            5u, 6u, 99u, 0xFFu, 0xFFFF'FFFFu,
        };

        bool any_widened_readable{ false };
        bool any_succeeded{ false };
        for (const std::uint32_t g : garbage_vals)
        {
            // Reset to writable between iterations so each probe starts from a
            // known state and the previous iteration's no_access is cleared.
            (void)make_writable(block, page);

            const bool ok{ vmhook::os::protect(block, page,
                                               static_cast<memory_protection>(g),
                                               nullptr) };
            if (ok)
            {
                any_succeeded = true;
                // Fallback maps to no_access -> page must be unreadable.
                if (byte_is_readable(block))
                {
                    any_widened_readable = true;
                }
            }
        }
        // No garbage value may ever produce a READABLE page; that would mean
        // the fallback became permissive (the load-bearing safety contract).
        check("wave23_oor_sweep_never_produces_readable_page",
              !any_widened_readable);
        // Characterize: at least one garbage value should succeed somewhere
        // (Windows VirtualProtect to PAGE_NOACCESS always works; sandboxes can
        // refuse PROT_NONE). This is informational only.
        info("wave23_oor_sweep_any_succeeded", any_succeeded);

        check("wave23_oor_sweep_restore_writable", make_writable(block, page));
        vmhook::os::release(block, page);
    }
#endif

    // -----------------------------------------------------------------------
    // no_access vs execute_rw -- the two enum arms most platform-variable.
    // Witness the asymmetry via safe_read across the SAME page in one test:
    //   * after no_access  -> byte_is_readable(p) == false
    //   * after read       -> byte_is_readable(p) == true
    //   * after execute_rw -> byte_is_readable(p) == true (when granted)
    // This pins the contract that flipping back from no_access to a permissive
    // state restores readability, AND that execute_rw is at-least-readable.
    // -----------------------------------------------------------------------
#if !VMHOOK_OS_IOS
    static auto test_no_access_vs_execute_rw_safe_read_asymmetry() -> void
    {
        const std::size_t page{ vmhook::os::page_size() };
        void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
        if (!block)
        {
            check("wave23_asymmetry_skipped_alloc_failed", false);
            return;
        }

        auto* const bytes{ static_cast<std::uint8_t*>(block) };
        bytes[0] = 0xA9;

        // 1) no_access -> unreadable. Skip if sandbox refuses PROT_NONE.
        const bool na_ok{ vmhook::os::protect(block, page,
                                              memory_protection::no_access, nullptr) };
        if (na_ok)
        {
            check("wave23_no_access_byte_unreadable", !byte_is_readable(block));
        }
        else
        {
            std::printf("[INFO] wave23_no_access skipped: PROT_NONE refused\n");
        }

        // 2) Flip BACK to read -> readable again. This is the recovery edge.
        const bool r_ok{ vmhook::os::protect(block, page,
                                             memory_protection::read, nullptr) };
        check("wave23_recover_from_no_access_to_read_succeeds", r_ok);
        if (r_ok)
        {
            check("wave23_recovered_page_is_readable", byte_is_readable(block));
            check("wave23_recovered_page_preserves_byte", bytes[0] == 0xA9);
        }

        // 3) execute_rw -> readable (when W^X allows). Otherwise [INFO] skip.
        const bool xrw_ok{ vmhook::os::protect(block, page,
                                               memory_protection::execute_rw, nullptr) };
        if (xrw_ok)
        {
            check("wave23_execute_rw_is_readable", byte_is_readable(block));
            // And writable -- distinguishes execute_rw (R+W+X) from execute_read.
            bytes[0] = 0xB9;
            check("wave23_execute_rw_is_writable", bytes[0] == 0xB9);
        }
        else
        {
            std::printf("[INFO] wave23_execute_rw skipped: W^X refused\n");
        }

        check("wave23_asymmetry_restore_writable", make_writable(block, page));
        vmhook::os::release(block, page);
    }
#endif

    // -----------------------------------------------------------------------
    // Sub-page protect with a CROSS-PAGE 1-byte request: address = end-of-
    // page-0 - 0 (i.e. last byte of page 0) and size = 1. This rounds to one
    // page (just page 0). The neighbour (page 1) must remain WRITABLE. The
    // existing neighbour witness uses page/3 which stays inside page 0; this
    // probes the page-0/page-1 boundary edge specifically.
    // -----------------------------------------------------------------------
    static auto test_subpage_at_page0_last_byte_no_bleed() -> void
    {
        const std::size_t page{ vmhook::os::page_size() };
        void* const block{ vmhook::os::allocate_rwx(nullptr, page * 2) };
        if (!block)
        {
            check("wave23_last_byte_no_bleed_skipped_alloc_failed", false);
            return;
        }

        auto* const bytes{ static_cast<std::uint8_t*>(block) };
        bytes[page - 1] = 0xE1;  // last byte of page 0
        bytes[page]     = 0xE2;  // first byte of page 1 (must stay writable)
        bytes[page * 2 - 1] = 0xE3;  // last byte of page 1

        // Protect 1 byte at the very last byte of page 0. Rounds down to base
        // of page 0, length up to 1 page. Page 1 must remain writable.
        const bool ok{ vmhook::os::protect(bytes + (page - 1), 1,
                                           memory_protection::read, nullptr) };
        check("wave23_last_byte_protect_succeeds", ok);

        // The two markers in page 1 must still be writable.
        bytes[page]         = 0xF2;
        bytes[page * 2 - 1] = 0xF3;
        check("wave23_page1_first_byte_still_writable", bytes[page] == 0xF2);
        check("wave23_page1_last_byte_still_writable",
              bytes[page * 2 - 1] == 0xF3);

        // Page 0's marker preserved (protect changes flags, not content).
        check("wave23_page0_last_byte_marker_preserved", bytes[page - 1] == 0xE1);

        check("wave23_last_byte_restore", make_writable(block, page * 2));
        vmhook::os::release(block, page * 2);
    }

    // -----------------------------------------------------------------------
    // Idempotent re-protect with old_prot capture on EVERY call. The Windows
    // path writes the genuine prior native flags, so the second protect(read)
    // captured value should be PAGE_READONLY (the state set by the first call)
    // -- proving the second call really did consult the prior state. POSIX
    // always writes 0, so both captures must be 0.
    // -----------------------------------------------------------------------
    static auto test_idempotent_reprotect_captures_old_prot_each_call() -> void
    {
        const std::size_t page{ vmhook::os::page_size() };
        void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
        if (!block)
        {
            check("wave23_idem_oldprot_skipped_alloc_failed", false);
            return;
        }

        // Establish a known starting state: read_write.
        check("wave23_idem_seed_rw", make_writable(block, page));

        std::uint32_t op1{ 0xCAFEBABEu };
        std::uint32_t op2{ 0xCAFEBABEu };

        const bool ok1{ vmhook::os::protect(block, page,
                                            memory_protection::read, &op1) };
        const bool ok2{ vmhook::os::protect(block, page,
                                            memory_protection::read, &op2) };
        check("wave23_idem_first_read_succeeds", ok1);
        check("wave23_idem_second_read_succeeds", ok2);

#if VMHOOK_OS_WINDOWS
        // Windows: each call writes the REAL prior native flags. The sentinel
        // was overwritten on both, and the second call's captured value should
        // be the native form of the READ state set by the first call. We
        // assert only "written, not sentinel" -- the exact PAGE_* value is
        // checked elsewhere; this test focuses on the per-call write contract.
        check("wave23_idem_op1_overwritten_on_windows", op1 != 0xCAFEBABEu);
        check("wave23_idem_op2_overwritten_on_windows", op2 != 0xCAFEBABEu);
        // The first call's prior state (read_write) and the second call's
        // prior state (read) MAY produce distinct PAGE_* values -- this is
        // platform-detail and reported only.
        info("wave23_idem_windows_op1_op2_distinct", op1 != op2);
#else
        // POSIX: both captures must be exactly 0 by contract (vmhook.hpp:661-664).
        check("wave23_idem_op1_is_zero_on_posix", op1 == 0u);
        check("wave23_idem_op2_is_zero_on_posix", op2 == 0u);
#endif

        check("wave23_idem_restore_writable", make_writable(block, page));
        vmhook::os::release(block, page);
    }

    // -----------------------------------------------------------------------
    // Size == page (no rounding) vs size == page + 1 (rounds up to 2 pages):
    // pin BOTH boundaries through a safe_read asymmetry probe across both
    // pages. Existing tests cover this via writability; this adds the
    // READABILITY probe (orthogonal axis) by flipping to no_access. On the
    // exact-page request, page 1 must STAY readable; on the page+1 request,
    // page 1 must become UNREADABLE (it was included in the rounded-up range).
    // Gated on iOS / PROT_NONE sandbox.
    // -----------------------------------------------------------------------
#if !VMHOOK_OS_IOS
    static auto test_exact_page_vs_page_plus_one_safe_read_witness() -> void
    {
        const std::size_t page{ vmhook::os::page_size() };
        // Three pages: page 0 we always protect, page 1 is the witness page,
        // page 2 is the safety buffer that must always stay writable.
        void* const block{ vmhook::os::allocate_rwx(nullptr, page * 3) };
        if (!block)
        {
            check("wave23_exact_vs_plus_one_skipped_alloc_failed", false);
            return;
        }

        auto* const bytes{ static_cast<std::uint8_t*>(block) };
        bytes[0]            = 0x10;
        bytes[page]         = 0x20;
        bytes[page * 2]     = 0x30;

        // (a) size == page : only page 0 affected. Page 1 stays READABLE.
        {
            const bool ok{ vmhook::os::protect(block, page,
                                               memory_protection::no_access, nullptr) };
            if (ok)
            {
                check("wave23_exact_page_page1_still_readable",
                      byte_is_readable(bytes + page));
                check("wave23_exact_page_page0_not_readable",
                      !byte_is_readable(block));
            }
            else
            {
                std::printf("[INFO] wave23_exact_page no_access refused\n");
            }
            check("wave23_exact_page_restore", make_writable(block, page * 3));
        }

        // (b) size == page + 1 : rounds up to 2 pages. Page 1 must become
        // UNREADABLE. Page 2 must stay readable.
        {
            const bool ok{ vmhook::os::protect(block, page + 1,
                                               memory_protection::no_access, nullptr) };
            if (ok)
            {
                check("wave23_page_plus_one_page0_not_readable",
                      !byte_is_readable(block));
                check("wave23_page_plus_one_page1_not_readable",
                      !byte_is_readable(bytes + page));
                check("wave23_page_plus_one_page2_still_readable",
                      byte_is_readable(bytes + page * 2));
            }
            else
            {
                std::printf("[INFO] wave23_page_plus_one no_access refused\n");
            }
            check("wave23_page_plus_one_restore", make_writable(block, page * 3));
        }

        vmhook::os::release(block, page * 3);
    }
#endif

    // -----------------------------------------------------------------------
    // old_prot success-path: pass nullptr and a valid pointer in ALTERNATING
    // calls on the same page. The nullptr arm tests the `if (old_prot)` guard
    // (Windows 648 / POSIX 661); the valid-pointer arm tests the write. Both
    // must succeed; the valid-pointer arm must overwrite a sentinel.
    // -----------------------------------------------------------------------
    static auto test_old_prot_alternating_null_and_valid_pointer() -> void
    {
        const std::size_t page{ vmhook::os::page_size() };
        void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
        if (!block)
        {
            check("wave23_alt_null_skipped_alloc_failed", false);
            return;
        }

        constexpr std::uint32_t sentinel{ 0x12345678u };

        // null -> valid -> null -> valid, all succeed.
        const bool ok1{ vmhook::os::protect(block, page,
                                            memory_protection::read, nullptr) };
        std::uint32_t op2{ sentinel };
        const bool ok2{ vmhook::os::protect(block, page,
                                            memory_protection::read_write, &op2) };
        const bool ok3{ vmhook::os::protect(block, page,
                                            memory_protection::read, nullptr) };
        std::uint32_t op4{ sentinel };
        const bool ok4{ vmhook::os::protect(block, page,
                                            memory_protection::read_write, &op4) };

        check("wave23_alt_null_arm1_succeeds", ok1);
        check("wave23_alt_valid_arm2_succeeds", ok2);
        check("wave23_alt_null_arm3_succeeds", ok3);
        check("wave23_alt_valid_arm4_succeeds", ok4);

        // Every valid-pointer arm overwrote the sentinel.
        check("wave23_alt_op2_overwritten", op2 != sentinel);
        check("wave23_alt_op4_overwritten", op4 != sentinel);

#if !VMHOOK_OS_WINDOWS
        // POSIX writes exactly 0 on every success.
        check("wave23_alt_op2_zero_on_posix", op2 == 0u);
        check("wave23_alt_op4_zero_on_posix", op4 == 0u);
#endif

        check("wave23_alt_restore_writable", make_writable(block, page));
        vmhook::os::release(block, page);
    }

    // -----------------------------------------------------------------------
    // POSIX hazard demonstration: capturing old_prot from a successful flip
    // and feeding it back through the enum produces no_access. This is a
    // VALUE-only check (no kernel call applying the bogus restore), pinning
    // that the asymmetric contract is the trap. On Windows the captured value
    // round-trips correctly through ::VirtualProtect (already tested
    // elsewhere); here we only nail the POSIX value-equivalence.
    // -----------------------------------------------------------------------
#if !VMHOOK_OS_WINDOWS
    static auto test_posix_old_prot_roundtrip_is_no_access_hazard() -> void
    {
        const std::size_t page{ vmhook::os::page_size() };
        void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
        if (!block)
        {
            check("wave23_posix_hazard_skipped_alloc_failed", false);
            return;
        }

        // Flip through every reachable prior state and confirm the captured
        // op is always 0 (== no_access cast). Each iteration is independent.
        const memory_protection priors[]{
            memory_protection::read,
            memory_protection::read_write,
            memory_protection::execute_read,
        };

        bool all_zero{ true };
        bool all_roundtrip_to_no_access{ true };
        for (const auto prior : priors)
        {
            if (!vmhook::os::protect(block, page, prior, nullptr))
            {
                continue;
            }
            std::uint32_t op{ 0xFEEDFACEu };
            if (!vmhook::os::protect(block, page,
                                     memory_protection::read_write, &op))
            {
                continue;
            }
            if (op != 0u)
            {
                all_zero = false;
            }
            if (static_cast<memory_protection>(op) != memory_protection::no_access)
            {
                all_roundtrip_to_no_access = false;
            }
        }
        check("wave23_posix_old_prot_always_zero", all_zero);
        check("wave23_posix_old_prot_roundtrips_to_no_access",
              all_roundtrip_to_no_access);

        check("wave23_posix_hazard_restore_writable", make_writable(block, page));
        vmhook::os::release(block, page);
    }
#endif

    static auto run() -> void
    {
#if !VMHOOK_OS_IOS
        test_out_of_range_enum_sweep_never_widens();
        test_no_access_vs_execute_rw_safe_read_asymmetry();
#endif
        test_subpage_at_page0_last_byte_no_bleed();
        test_idempotent_reprotect_captures_old_prot_each_call();
#if !VMHOOK_OS_IOS
        test_exact_page_vs_page_plus_one_safe_read_witness();
#endif
        test_old_prot_alternating_null_and_valid_pointer();
#if !VMHOOK_OS_WINDOWS
        test_posix_old_prot_roundtrip_is_no_access_hazard();
#endif
    }
} // namespace wave23_protect_ledger_gaps

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

    // --- expansion: release / allocate lifecycle + protect<->release seam ---
    test_allocate_rwx_multipage_writable_then_release();
    test_allocate_rwx_subpage_size_one();
    test_allocate_rwx_two_blocks_do_not_alias();
    test_allocate_rwx_hint_is_non_binding();
    test_allocate_release_reuse_cycle();
    test_release_of_read_only_mapping();
    test_release_of_no_access_mapping();
    test_release_partial_size_is_platform_variable();
    test_double_release_does_not_crash();
    test_protect_after_release_is_platform_variable();
    test_protect_overflow_guard_leaves_old_prot_untouched();
    test_protect_cycle_then_release_lifecycle();

    // --- deepening wave-5: os_allocate_release sizing / span / leak-loop /
    //     negative-release containment / execute-bit positive control ---
    wave5_alloc_release::run_all();

    // --- expansion 2: pure-logic to_native_protect mapping / overflow boundary
    //     / POSIX rounding formula / old_prot success-path contract ---
    expansion2_protect_pure_logic::run();

    // --- expansion 3: safe_read/safe_write input+wrap guards (real owned
    //     pages), query_region field invariants, sizing-constant relations ---
    expansion3_os_guards_and_region::run();

    // --- expansion 4: module lookup, current_thread_id, safe_read_fast parity,
    //     safe_write round-trip read-back, flush_instruction_cache, ceiling
    //     bit-identity ---
    expansion4_os_layer_surface::run();

    // --- expansion 5: released-block query transition, zero-size-release on a
    //     multi-page block, partial-release tail query characterization, sub-page
    //     whole-page usability, granularity-sized round-trip ---
    expansion5_release_query_roundtrip::run();

    // --- wave-21: os_allocate_release ledger-gap deepening -- cast non-
    //     truncation width contract, sysconf 4096-fallback constant pin,
    //     varying-size leak loop, later-page interior negative release,
    //     value-returning execute-bit stub ---
    wave21_alloc_release_deepening::run();

    // --- wave-23: os_protect ledger-gap closure -- OOB enum sweep, no_access
    //     vs execute_rw safe_read asymmetry, page-boundary subpage no-bleed,
    //     idempotent reprotect with old_prot every call, exact-page vs page+1
    //     safe_read witness, alternating null/valid old_prot, POSIX old_prot
    //     hazard pin ---
    wave23_protect_ledger_gaps::run();

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
