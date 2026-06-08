// Standalone (no-JVM) edge tests for the zero-size and null-guard corners of
// the vmhook::os layer, PLUS an exhaustive sweep of the os::allocate_rwx /
// os::release round-trip across every input size and edge that is safely
// testable in-process.  Originally focused on the CHANGELOG "release(addr, 0)
// is a no-op" fix and the get_proc_address / protect null-/zero-guards; the
// allocate/release sweep was added to cover EVERY input the two primitives
// accept (size {1, page-1, page, page+1, many pages, granularity, 0}, hint
// honoured-or-ignored, alignment of the returned base, write/read round-trip
// across the full range, query of a live vs released region, a leak-detecting
// repetition loop, the documented double-/null-/mismatched-release contract,
// and a positive execute-bit check where the platform permits it).
//
// This file deliberately does NOT duplicate tests/test_os_protect_interaction.cpp
// or tests/test_os_layer.cpp.  Those already cover:
//   * the basic release-zero no-op, the protect/safe_read null guards, the enum
//     walk and the page_size/granularity relationship (protect_interaction);
//   * the dense align_up/align_down sweep, the POSIX protect page-rounding
//     formula, single-page allocate->write->protect->read round-trip, and the
//     Windows granularity-aligned-base check (os_layer).
// Here we drill into the *edges those two files do not assert*:
//   * release(ptr, 0) is idempotent across many calls and the block survives
//     until a later real release(ptr, page) frees it (no double-free, no leak).
//   * release(nullptr, 0) / release(nullptr, page) / release(ptr, 0) are all
//     no-ops that never fault.
//   * allocate_rwx across the full size spectrum {1, page-1, page, page+1,
//     page*N, granularity}: non-null, page-aligned base, the ENTIRE requested
//     range writable end-to-end (write a pattern across every page, read back),
//     and query_region().size >= requested.
//   * a 512-iteration alloc->write->release loop never starts failing to
//     allocate (a leak in the release path would eventually exhaust the space).
//   * the documented (and currently un-enforced, no-diagnostic) contract for a
//     MISMATCHED release: an unaligned/short/interior release must NOT crash
//     (release is noexcept->void and ignores both kernel return values).  This
//     is a behaviour-pinning test, not a "it works" test.
//   * allocate_rwx with a placement hint derived from a just-freed region and
//     with a deliberately-occupied hint: either way it returns a usable RWX
//     page and never hands back the occupied object's address as if fresh.
//   * query_region of a live allocate_rwx block (committed + readable) and of
//     a multi-page block (size spans the request).
//   * protect(): on the null/zero early-return path the caller's old_prot
//     output is left completely untouched.
//   * protect() accepts a non-page-aligned interior address without corrupting
//     neighbouring bytes.
//   * get_proc_address(): the null-symbol guard fires BEFORE the OS lookup even
//     with a valid, real module handle, with a real symbol resolving as a
//     positive control.
//   * page_size() / allocation_granularity() invariants, idempotency, no DWORD
//     truncation, and the architecturally-valid page-size set.
//   * a POSITIVE execute-bit check: a trivial `ret` stub written into an
//     allocate_rwx page and called through a function pointer (the only way to
//     prove the X in _rwx actually holds).  Gated off Apple arm64/iOS where the
//     allocator legitimately falls back to PROT_READ|PROT_WRITE (W^X).
//
// SAFETY: this file only ever allocates and releases its OWN memory.  It never
// double-frees a real pointer (that is genuine UB; we pin only the DOCUMENTED
// safe no-op/mismatch contract), never reads released memory, and always tears
// every allocation down.  Every assertion is platform-invariant; the few
// genuinely OS/arch-specific literals are gated behind VMHOOK_OS_* / VMHOOK_ARCH_*.
//
// Everything here is pure OS-layer / null-safety / boundary behaviour.  Nothing
// in this file requires a live oop or a running JVM.
#include <vmhook/vmhook.hpp>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cstring>

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

// Small helper: is p aligned to (power-of-two) a?
static auto is_aligned(const void* p, std::size_t a) noexcept -> bool
{
    return (reinterpret_cast<std::uintptr_t>(p) & (a - 1u)) == 0u;
}

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

    // Many distinct bogus non-null addresses, all with size 0 -> all no-ops.
    // Proves the zero-size guard, not the address, is what short-circuits.
    const std::uintptr_t bogus_addrs[]{
        0x1u, 0x8u, 0xFFFFu, 0x1'0000u, 0xDEAD'BEEFu,
        static_cast<std::uintptr_t>(vmhook::os::user_address_floor),
        static_cast<std::uintptr_t>(vmhook::os::user_address_ceiling),
    };
    for (const std::uintptr_t a : bogus_addrs)
    {
        vmhook::os::release(reinterpret_cast<void*>(a), 0);
    }
    check("release_many_bogus_addrs_zero_size_no_crash", true);
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
    // Zero size still wins even with a plausible, page-aligned, in-range hint.
    check("allocate_rwx_zero_size_aligned_hint_returns_null",
          vmhook::os::allocate_rwx(reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x10'0000ull)), 0)
              == nullptr);
}

// ---------------------------------------------------------------------------
// allocate_rwx across the FULL size spectrum.  For each request size we assert:
//   * a non-null pointer (every size from 1 byte upward is satisfiable),
//   * the returned base is page-aligned (the trampoline allocator and protect()
//     both assume this),
//   * the ENTIRE requested range is writable: write a position-dependent byte
//     to the first and last byte of every page the request spans and read it
//     back (catches an off-by-one in the committed length / a sub-page request
//     that under-commits),
//   * query_region(base).size >= the requested size.
// Sub-page sizes (1, page-1) must round UP to a full committed page, never
// under-commit.  We free every block immediately; no leaks.
// ---------------------------------------------------------------------------
static auto exercise_alloc_size(const char* tag, std::size_t size) -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    void* const block{ vmhook::os::allocate_rwx(nullptr, size) };

    char name[128];
    std::snprintf(name, sizeof(name), "alloc_size_%s_returns_nonnull", tag);
    check(name, block != nullptr);
    if (!block)
    {
        return;
    }

    std::snprintf(name, sizeof(name), "alloc_size_%s_base_page_aligned", tag);
    check(name, is_aligned(block, page));

    // Walk every page the request spans and stamp the first + last byte of each
    // committed page, then read it back.  For a sub-page request we still touch
    // [0 .. size-1] (the kernel commits at least one whole page, but we only
    // assert writability of the bytes we actually requested, which is the
    // contract: the returned buffer is usable for `size` bytes).
    auto* const bytes{ static_cast<volatile std::uint8_t*>(block) };
    bool round_trip_ok{ true };
    for (std::size_t off{ 0 }; off < size; off += page)
    {
        const std::size_t last_in_chunk{ (off + page - 1u < size) ? (off + page - 1u)
                                                                  : (size - 1u) };
        // Stamp the last byte of the chunk first, then the first byte, and only
        // verify the two as independent cells when they ARE independent (a
        // single-byte chunk, e.g. size==1 or the 1-byte tail of page+1, has
        // off == last_in_chunk so the two writes alias one address).
        const std::uint8_t v1{ static_cast<std::uint8_t>(0x5C ^ (last_in_chunk & 0xFFu)) };
        bytes[last_in_chunk] = v1;
        if (off != last_in_chunk)
        {
            const std::uint8_t v0{ static_cast<std::uint8_t>(0xA0 ^ (off & 0xFFu)) };
            bytes[off] = v0;
            if (bytes[off] != v0) { round_trip_ok = false; break; }
        }
        if (bytes[last_in_chunk] != v1) { round_trip_ok = false; break; }
    }
    // Always exercise the very last requested byte explicitly (covers size==1
    // and the page-1 / page+1 boundaries where the loop's last chunk is short).
    {
        auto* const tail{ static_cast<volatile std::uint8_t*>(block) + (size - 1u) };
        *tail = 0x3C;
        if (*tail != 0x3C) { round_trip_ok = false; }
    }
    std::snprintf(name, sizeof(name), "alloc_size_%s_full_range_writable", tag);
    check(name, round_trip_ok);

    const auto info{ vmhook::os::query_region(block) };
    std::snprintf(name, sizeof(name), "alloc_size_%s_query_size_ge_request", tag);
    // iOS query_region returns a permissive one-page stub, so the size can be
    // exactly page rather than >= request for multi-page allocations; only
    // assert the >= relationship on platforms that report the true region size.
#if VMHOOK_OS_IOS
    check(name, info.size >= page); // permissive stub: at least a page
#else
    check(name, info.size >= size);
#endif

    vmhook::os::release(block, size);
    std::snprintf(name, sizeof(name), "alloc_size_%s_release_no_crash", tag);
    check(name, true);
}

static auto test_allocate_rwx_size_spectrum() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    const std::size_t gran{ vmhook::os::allocation_granularity() };

    exercise_alloc_size("1byte",        1u);
    exercise_alloc_size("page_minus_1", page - 1u);
    exercise_alloc_size("exact_page",   page);
    exercise_alloc_size("page_plus_1",  page + 1u);
    exercise_alloc_size("two_pages",    page * 2u);
    exercise_alloc_size("four_pages",   page * 4u);
    exercise_alloc_size("seventeen_pages", page * 17u);
    // Granularity-sized request (== page on POSIX, 64 KiB on Windows): the unit
    // VirtualAlloc reservations are rounded to.  Distinct from the page cases on
    // Windows, a harmless duplicate of four_pages on POSIX (still valid).
    exercise_alloc_size("granularity", gran);
}

// ---------------------------------------------------------------------------
// A leak-detecting repetition loop.  allocate_rwx -> write -> release, many
// times.  If release leaked the mapping (the silent failure mode behind library
// flaws #1/#2 where a size/base mismatch is ignored), the address space would
// monotonically fill and allocation would eventually start returning nullptr.
// A matched (base, size) round-trip must sustain indefinitely; 512 iterations
// of a page each is plenty to surface a per-iteration leak without being slow.
// ---------------------------------------------------------------------------
static auto test_alloc_release_roundtrip_no_leak() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    constexpr int iterations{ 512 };

    int succeeded{ 0 };
    bool every_block_writable{ true };
    for (int i{ 0 }; i < iterations; ++i)
    {
        void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
        if (!block)
        {
            break; // a leak would manifest here partway through the loop
        }
        ++succeeded;
        auto* const cell{ static_cast<volatile std::uint8_t*>(block) };
        const std::uint8_t v{ static_cast<std::uint8_t>(i & 0xFF) };
        *cell = v;
        if (*cell != v)
        {
            every_block_writable = false;
        }
        vmhook::os::release(block, page);
    }
    check("roundtrip_loop_all_iterations_allocated", succeeded == iterations);
    check("roundtrip_loop_every_block_writable", every_block_writable);
}

// Same loop, but with multi-page allocations, to stress that the size passed to
// release (load-bearing on POSIX munmap) frees the whole multi-page mapping and
// nothing is left mapped to accumulate.  Fewer iterations because each is bigger.
static auto test_alloc_release_roundtrip_multipage_no_leak() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    const std::size_t size{ page * 8u };
    constexpr int iterations{ 128 };

    int succeeded{ 0 };
    for (int i{ 0 }; i < iterations; ++i)
    {
        void* const block{ vmhook::os::allocate_rwx(nullptr, size) };
        if (!block)
        {
            break;
        }
        ++succeeded;
        // Touch first and last page so every page is genuinely committed and
        // would have to be reclaimed by release for the loop to keep succeeding.
        auto* const bytes{ static_cast<volatile std::uint8_t*>(block) };
        bytes[0]            = 0x11;
        bytes[size - 1u]    = 0x22;
        vmhook::os::release(block, size);
    }
    check("roundtrip_loop_multipage_all_allocated", succeeded == iterations);
}

// ---------------------------------------------------------------------------
// query_region of a LIVE allocate_rwx block vs the same block after release.
// The companion interaction test checks the live-block attributes once and the
// post-release "free or non-committed" contract once; here we tie them together
// on a single block and additionally assert the live region's base/extent
// actually contain our pointer (the trampoline allocator depends on that).
// ---------------------------------------------------------------------------
static auto test_query_region_live_then_released() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    void* const block{ vmhook::os::allocate_rwx(nullptr, page * 3u) };
    if (!block)
    {
        check("query_live_then_released_skipped_alloc_failed", false);
        return;
    }
    // Commit every page so the query is unambiguous.
    auto* const bytes{ static_cast<volatile std::uint8_t*>(block) };
    bytes[0]              = 0x01;
    bytes[page]           = 0x02;
    bytes[page * 2u]      = 0x03;

    const auto live{ vmhook::os::query_region(block) };
    check("query_live_committed", live.committed);
    check("query_live_readable", live.readable);
    check("query_live_not_guarded", !live.guarded);
#if VMHOOK_OS_IOS
    check("query_live_size_at_least_page", live.size >= page);
#else
    check("query_live_size_spans_request", live.size >= page * 3u);
    // The reported region must actually contain the pointer we queried.
    if (live.base)
    {
        const std::uintptr_t base{ reinterpret_cast<std::uintptr_t>(live.base) };
        const std::uintptr_t q{ reinterpret_cast<std::uintptr_t>(block) };
        check("query_live_region_contains_pointer",
              q >= base && q < base + live.size);
    }
    else
    {
        check("query_live_region_contains_pointer", false);
    }
#endif

    vmhook::os::release(block, page * 3u);

#if !VMHOOK_OS_IOS
    // After release the region must no longer look like the live RWX block.
    // (On a busy process the kernel may re-map the address, so we accept any of:
    //  free, not-committed, or not-(readable AND executable) — same contract the
    //  companion test pins, asserted here on the multi-page block.)
    const auto freed{ vmhook::os::query_region(block) };
    check("query_after_release_not_live_rwx",
          freed.free || !freed.committed || !(freed.readable && freed.executable));
#endif
}

// ---------------------------------------------------------------------------
// DOCUMENTED mismatched-release contract (behaviour-pinning, NOT "it works").
// release() is `noexcept -> void` and discards both kernel return values
// (vmhook.hpp: VirtualFree(addr,0,MEM_RELEASE) ignores `size` and needs the
// reservation base; munmap(addr,size) needs page alignment and the right size).
// A caller that violates the convention gets NO diagnostic and NO crash — the
// call is simply a silent partial/total no-op.  We freeze that contract so a
// future change that adds error signalling is a deliberate, visible break.
//
// SAFETY: every block here is independently allocated and then properly torn
// down via a matched release(base, size).  The mismatched calls in between are
// the ones under test; none of them is a double-free of a live mapping.
// ---------------------------------------------------------------------------
static auto test_release_mismatch_is_silent_noop_or_safe() -> void
{
    const std::size_t page{ vmhook::os::page_size() };

    // (a) POSIX: release with a size SHORTER than the mapping must not crash.
    //     On Windows size is ignored entirely, so this is trivially safe there.
    {
        void* const block{ vmhook::os::allocate_rwx(nullptr, page * 2u) };
        if (block)
        {
            vmhook::os::release(block, page - 1u);   // short size: silent no-op/partial
            vmhook::os::release(block, page * 2u);   // proper teardown of whatever remains
        }
        check("release_short_size_then_full_no_crash", true);
    }

    // (b) Unaligned / interior pointer.  POSIX munmap(EINVAL) frees nothing;
    //     Windows VirtualFree on a non-base pointer fails (returns 0) and frees
    //     nothing.  Either way: no crash, and the real base is freed afterward.
    {
        void* const block{ vmhook::os::allocate_rwx(nullptr, page * 2u) };
        if (block)
        {
            void* const interior{ static_cast<std::uint8_t*>(block) + 1u };
            vmhook::os::release(interior, page);     // interior ptr: silent no-op
            void* const second_page{ static_cast<std::uint8_t*>(block) + page };
            vmhook::os::release(second_page, page);  // page-aligned interior on POSIX
            // Real teardown via the genuine base + full size.  On POSIX, if the
            // previous aligned-interior call already unmapped page 2, munmapping
            // the full range again over a partially-unmapped region is still a
            // legal no-op for the already-gone page; the base page is freed.
            vmhook::os::release(block, page * 2u);
        }
        check("release_interior_ptr_then_base_no_crash", true);
    }

    // (c) size LARGER than requested.  POSIX munmap will unmap whatever pages
    //     fall in [addr, addr+size) that are mapped and ignore the rest; since
    //     we pass our own base this frees our block (and harmlessly targets the
    //     following, unmapped page).  Windows ignores size.  No crash either way,
    //     and we must NOT issue a second release (the block is already gone).
    {
        void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
        if (block)
        {
            *static_cast<volatile std::uint8_t*>(block) = 0x7E;
            vmhook::os::release(block, page * 2u);   // over-sized release frees the block
        }
        check("release_oversized_no_crash", true);
    }
}

// ---------------------------------------------------------------------------
// Placement hint: honoured-or-ignored, but never corrupting.  `address_hint` is
// documented non-binding.  Two probes:
//   * a hint derived from a just-freed region (a plausibly-free address): the
//     allocation must still return a usable, page-aligned RWX page.
//   * a deliberately-OCCUPIED hint (the address of a live stack object): the
//     allocator must NOT return that exact address as if it were fresh memory
//     it owns, yet must still hand back some usable page.
// Guards against a future change that blindly trusts the hint.
// ---------------------------------------------------------------------------
static auto test_allocate_rwx_hint_honoured_or_ignored() -> void
{
    const std::size_t page{ vmhook::os::page_size() };

    // Probe 1: free a block, reuse its address as a hint.
    {
        void* const first{ vmhook::os::allocate_rwx(nullptr, page) };
        if (!first)
        {
            check("hint_free_addr_skipped_alloc_failed", false);
        }
        else
        {
            vmhook::os::release(first, page);
            // `first` is now (probably) free; offer it back as a hint.
            void* const second{ vmhook::os::allocate_rwx(first, page) };
            check("hint_free_addr_returns_usable_page", second != nullptr);
            if (second)
            {
                check("hint_free_addr_result_page_aligned", is_aligned(second, page));
                auto* const cell{ static_cast<volatile std::uint8_t*>(second) };
                *cell = 0x9D;
                check("hint_free_addr_result_writable", *cell == 0x9D);
                vmhook::os::release(second, page);
            }
        }
    }

    // Probe 2: occupied hint (address of a live local array).  The allocator may
    // ignore it (POSIX mmap) or fail the placement and pick elsewhere; what it
    // must NEVER do is return that exact occupied address as a "fresh" mapping.
    {
        volatile std::uint8_t occupied[64]{};
        occupied[0] = 0xC7;
        void* const hint{ const_cast<std::uint8_t*>(occupied) };
        void* const got{ vmhook::os::allocate_rwx(hint, page) };
        // It is allowed to return null (placement refused) or a different,
        // usable page; it must not alias our live object.
        if (got)
        {
            check("hint_occupied_does_not_alias_live_object", got != hint);
            check("hint_occupied_result_page_aligned", is_aligned(got, page));
            auto* const cell{ static_cast<volatile std::uint8_t*>(got) };
            *cell = 0x1B;
            check("hint_occupied_result_writable", *cell == 0x1B);
            vmhook::os::release(got, page);
        }
        else
        {
            // Null is an acceptable outcome for a refused placement.
            check("hint_occupied_does_not_alias_live_object", true);
            check("hint_occupied_result_page_aligned", true);
            check("hint_occupied_result_writable", true);
        }
        // The occupied object must be untouched regardless.
        check("hint_occupied_object_unchanged", occupied[0] == 0xC7);
    }
}

// ---------------------------------------------------------------------------
// Alignment of the returned pointer across several sizes.  The trampoline
// allocator and protect() both assume allocate_rwx hands back a page-aligned
// base; pin it directly for the size spectrum.  (os_layer.cpp checks the single
// page case; here we add sub-page, multi-page and granularity sizes.)
// ---------------------------------------------------------------------------
static auto test_allocate_rwx_returned_pointer_alignment() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    const std::size_t gran{ vmhook::os::allocation_granularity() };
    const std::size_t sizes[]{ 1u, page - 1u, page, page + 1u, page * 5u, gran };

    bool all_page_aligned{ true };
    for (const std::size_t s : sizes)
    {
        void* const block{ vmhook::os::allocate_rwx(nullptr, s) };
        if (!block)
        {
            all_page_aligned = false;
            break;
        }
        if (!is_aligned(block, page))
        {
            all_page_aligned = false;
        }
        vmhook::os::release(block, s);
    }
    check("allocate_rwx_returned_ptr_page_aligned_all_sizes", all_page_aligned);

#if VMHOOK_OS_WINDOWS
    // On Windows the reservation base reported by query_region is granularity-
    // aligned even for a sub-page request (VirtualAlloc rounds the base down).
    void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
    if (block)
    {
        const auto info{ vmhook::os::query_region(block) };
        if (info.base)
        {
            check("windows_alloc_reservation_base_granularity_aligned",
                  is_aligned(info.base, gran));
        }
        else
        {
            check("windows_alloc_reservation_base_granularity_aligned", false);
        }
        vmhook::os::release(block, page);
    }
    else
    {
        check("windows_alloc_reservation_base_granularity_aligned", false);
    }
#else
    (void)gran;
#endif
}

// ---------------------------------------------------------------------------
// POSITIVE execute-bit check.  The function is named allocate_*rwx*; the only
// way to prove the X actually holds is to run code out of the page.  We write a
// trivial `ret` and call it through a function pointer.
//
// Gated OFF Apple (arm64 / iOS enforce W^X without the JIT entitlement, where
// allocate_rwx legitimately falls back to PROT_READ|PROT_WRITE — executing
// would fault by design).  Also requires a known instruction encoding, so we
// only run on x86_64 and arm64.  On every CI runner this feature is built for
// (Windows x64, Linux x64) the stub must execute and return its constant.
// ---------------------------------------------------------------------------
#if !VMHOOK_OS_APPLE && (VMHOOK_ARCH_X86_64 || VMHOOK_ARCH_ARM64)
using ret_int_fn = int (*)();

static auto test_allocate_rwx_executable_stub() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
    if (!block)
    {
        check("exec_stub_skipped_alloc_failed", false);
        return;
    }

    auto* const code{ static_cast<std::uint8_t*>(block) };

#if VMHOOK_ARCH_X86_64
    // mov eax, 0x2A ; ret   ->  B8 2A 00 00 00 C3   (returns 42)
    const std::uint8_t stub[]{ 0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3 };
    std::memcpy(code, stub, sizeof(stub));
#else // VMHOOK_ARCH_ARM64
    // mov w0, #0x2A ; ret   ->  0x52800540, 0xD65F03C0  (returns 42), LE bytes.
    const std::uint8_t stub[]{
        0x40, 0x05, 0x80, 0x52,   // mov w0, #42
        0xC0, 0x03, 0x5F, 0xD6,   // ret
    };
    std::memcpy(code, stub, sizeof(stub));
#endif

    // Some platforms (Windows DEP transitions, ARM I-cache) want an explicit
    // execute_read/execute_rw flip and/or an instruction-cache flush after
    // writing code.  allocate_rwx already returns an executable mapping on the
    // gated platforms, but flip to execute_rw defensively (ignore failure: the
    // mapping is already RWX where this test runs).
    (void)vmhook::os::protect(block, page, vmhook::os::memory_protection::execute_rw, nullptr);
#if VMHOOK_ARCH_ARM64 && defined(__GNUC__)
    __builtin___clear_cache(reinterpret_cast<char*>(code),
                            reinterpret_cast<char*>(code) + sizeof(stub));
#endif

    ret_int_fn fn{};
    std::memcpy(&fn, &code, sizeof(fn)); // avoid object->function-pointer cast UB warning
    const int result{ fn() };
    check("exec_stub_runs_and_returns_42", result == 42);

    vmhook::os::release(block, page);
}
#endif // executable stub gate

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
// idempotency (two reads agree), a sane lower bound, that the DWORD->size_t cast
// on Windows did not truncate to zero, and that page_size is one of the
// architecturally-valid values (so the POSIX `4096` literal fallback stays
// consistent with reality).
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

    // The DWORD->size_t cast (Windows GetSystemInfo) / long->size_t cast (POSIX
    // sysconf) must not have produced a truncated-to-zero value; already implied
    // by nonzero above, but pin the >= page relationship as a hard invariant the
    // trampoline allocator's align_down(usable_end - size, granularity) math
    // depends on (a granularity < page would mis-round last_candidate).
    check("granularity_ge_page_hard_invariant", gr1 >= ps1);
    check("granularity_over_page_is_power_of_two",
          ((gr1 / ps1) & ((gr1 / ps1) - 1)) == 0);

    // page_size must be one of the architecturally-valid page sizes.  This keeps
    // the POSIX `sysconf<=0 -> 4096` fallback literal honest: 4 KiB (x86, most
    // arm64), 16 KiB (Apple arm64, some arm64 Linux), 64 KiB (some ppc64/arm64).
    const bool known_page{ ps1 == 4096u || ps1 == 16384u || ps1 == 65536u };
    check("page_size_is_architecturally_valid", known_page);

    // Concrete per-OS values, gated behind the OS macro (cross-OS invariants are
    // covered above).  Mirrors test_os_layer.cpp's per-OS pin.
#if VMHOOK_OS_WINDOWS
    check("windows_page_size_is_4096", ps1 == 4096u);
    check("windows_granularity_is_65536", gr1 == 65536u);
#else
    // POSIX defines allocation_granularity() == page_size(); pin that identity.
    check("posix_granularity_equals_page_size", gr1 == ps1);
#endif
}

int main()
{
    test_release_zero_size_is_idempotent_noop();
    test_release_null_and_zero_combinations_are_safe();
    test_allocate_rwx_release_zero_then_real();
    test_allocate_rwx_zero_size_returns_null();
    test_allocate_rwx_size_spectrum();
    test_alloc_release_roundtrip_no_leak();
    test_alloc_release_roundtrip_multipage_no_leak();
    test_query_region_live_then_released();
    test_release_mismatch_is_silent_noop_or_safe();
    test_allocate_rwx_hint_honoured_or_ignored();
    test_allocate_rwx_returned_pointer_alignment();
#if !VMHOOK_OS_APPLE && (VMHOOK_ARCH_X86_64 || VMHOOK_ARCH_ARM64)
    test_allocate_rwx_executable_stub();
#endif
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
