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
