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
