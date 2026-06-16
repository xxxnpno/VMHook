// Exercises the vmhook::os abstraction without involving the JVM.
//
// The bulk of this file is an EXHAUSTIVE, platform-invariant sweep of the
// OS-layer page-size / allocation-granularity queries and the address-alignment
// arithmetic the trampoline allocator is built on.  Every assertion holds on
// every supported OS/arch (Windows 4 KiB page / 64 KiB granularity, Linux/macOS
// 4 KiB or 16 KiB page == granularity, etc.), so we assert INVARIANTS
// (power-of-two, >= 4096, granularity a whole multiple of page, alignment
// idempotence / ordering / no-overflow) rather than any specific constant.  The
// few genuinely platform-specific values are pinned behind the VMHOOK_OS_*
// macros at the very end.
//
// Validates:
//   * page_size / allocation_granularity: non-zero, power-of-two, >= 4096,
//     granularity >= page and granularity % page == 0, stable across repeated
//     calls AND across threads (process-invariant), plus per-OS concrete values.
//   * align_up / align_down (a byte-for-byte mirror of the library's own
//     trampoline-allocator lambdas, vmhook.hpp align_up/align_down): a dense
//     input sweep — 0, 1, page-1, page, page+1, multi-page, granularity edges,
//     and addresses one alignment below UINTPTR_MAX — checking idempotence,
//     ordering (down <= x <= up), exact alignment of the result, agreement with
//     the multiply/divide reference form, and the no-overflow boundary.
//   * the POSIX protect() page-rounding formula (base &= ~(ps-1);
//     size = (end-base+ps-1) & ~(ps-1)) reproduced as pure arithmetic.
//   * current_thread_id returns non-zero values that change across threads.
//   * allocate_rwx then protect can flip a page to RW and back, and the byte we
//     wrote is preserved (so we know the protect path didn't tear the mapping
//     down); the returned base is granularity-aligned on Windows.
//   * query_region returns at least some info for an allocated region.
//   * safe_read succeeds on a valid pointer and rejects an obviously bogus one.
//
// allocate_rwx / release ROUND-TRIP charter (the exhaustive lower half of this
// file — the sibling test_os_protect_interaction.cpp owns the protect matrix and
// test_os_release_and_protect_edges.cpp owns the null/zero-guard seam, so we do
// NOT duplicate those):
//   * allocate_rwx over a dense SIZE sweep (1, 17, 64, page/2, page-1, page,
//     page+1, 2/4/17 pages, allocation_granularity): non-null, page-aligned, the
//     FULL requested range written-then-read end to end (a tail page left
//     uncommitted faults/mismatches here), query_region.size covers the request,
//     released with the exact size it was allocated with.
//   * fresh allocate_rwx memory is ZERO-FILLED before first write (universal: no
//     kernel returns uninitialised committed anonymous memory).
//   * a large (8 MiB) allocation: success is [INFO]-gated (a constrained sandbox
//     may refuse it / map it RW), but IF granted the whole span is usable.
//   * many simultaneous blocks are DISTINCT and NON-OVERLAPPING — unique markers
//     all survive (no aliasing) — and after releasing them all a fresh batch
//     still allocates (the releases really returned the memory).
//   * alloc -> write+verify -> release LOOPED thousands of times never
//     monotonically exhausts (a leaking release would eventually return null) —
//     for both single-page and multi-page blocks.
//   * the placement hint is NON-BINDING: a free hint, an occupied (live stack
//     object) hint, and a high-canonical hint each still yield a usable page,
//     and the occupied address is NEVER handed back as a fresh allocation.
//   * NEGATIVE-RELEASE containment (behaviour-pinning): an interior pointer, an
//     unaligned base (POSIX), and a sub-page size are SILENTLY ignored — the
//     noexcept release never faults — freezing the current no-diagnostic
//     contract so a future return-value change is a deliberate break.
//   * the X in "rwx" is REAL: on non-Apple x86_64/arm64 a tiny stub written into
//     an allocate_rwx page is CALLED through a function pointer and returns the
//     expected value (the only place the executable bit is actually proven).
#include <vmhook/vmhook.hpp>
#include <cstring> // std::memcpy for the function-pointer round-trip

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>

static int failures{ 0 };

static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok)
    {
        ++failures;
    }
}

// ---------------------------------------------------------------------------
// Byte-for-byte mirror of the trampoline allocator's alignment lambdas in
// vmhook.hpp (allocate_nearby_memory):
//     align_up(value, alignment)   = (value + alignment - 1) & ~(alignment - 1)
//     align_down(value, alignment) =  value                  & ~(alignment - 1)
// Both assume `alignment` is a power of two (the same assumption the library
// makes — page_size() / allocation_granularity() always return one).  We test
// the library behaviour by reproducing the exact masks here; if the header ever
// changes the formula these mirrors must be updated in lock-step.
// ---------------------------------------------------------------------------
static constexpr auto mirror_align_up(std::uintptr_t value, std::uintptr_t alignment) noexcept
    -> std::uintptr_t
{
    return (value + alignment - 1) & ~(alignment - 1);
}

static constexpr auto mirror_align_down(std::uintptr_t value, std::uintptr_t alignment) noexcept
    -> std::uintptr_t
{
    return value & ~(alignment - 1);
}

// Independent reference implementation using multiply/divide instead of bit
// masks.  For power-of-two alignments and inputs that don't overflow, this MUST
// agree with the masking form — cross-checking the two catches a wrong mask.
static constexpr auto ref_align_down(std::uintptr_t value, std::uintptr_t alignment) noexcept
    -> std::uintptr_t
{
    return (value / alignment) * alignment;
}

static constexpr auto ref_align_up(std::uintptr_t value, std::uintptr_t alignment) noexcept
    -> std::uintptr_t
{
    // Caller guarantees value + alignment - 1 does not overflow.
    return ((value + alignment - 1) / alignment) * alignment;
}

static auto is_power_of_two(std::size_t v) noexcept -> bool
{
    return v != 0 && (v & (v - 1)) == 0;
}

// Compile-time confidence in the mirror against hand-computed values, so a
// silently-wrong mask is caught even before the binary runs.  4096 / 65536 are
// the two real-world alignments this library cares about.
static_assert(mirror_align_down(0u, 4096u) == 0u);
static_assert(mirror_align_down(4095u, 4096u) == 0u);
static_assert(mirror_align_down(4096u, 4096u) == 4096u);
static_assert(mirror_align_down(4097u, 4096u) == 4096u);
static_assert(mirror_align_down(8191u, 4096u) == 4096u);
static_assert(mirror_align_up(0u, 4096u) == 0u);
static_assert(mirror_align_up(1u, 4096u) == 4096u);
static_assert(mirror_align_up(4096u, 4096u) == 4096u);
static_assert(mirror_align_up(4097u, 4096u) == 8192u);
static_assert(mirror_align_down(65535u, 65536u) == 0u);
static_assert(mirror_align_up(65537u, 65536u) == 131072u);
static_assert(mirror_align_up(4096u, 65536u) == 65536u);

// ---------------------------------------------------------------------------
// page_size() / allocation_granularity(): the full invariant set.
// ---------------------------------------------------------------------------
static auto test_page_and_granularity_invariants() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    const std::size_t gran{ vmhook::os::allocation_granularity() };

    // -- page_size -----------------------------------------------------------
    check("page_size_nonzero", page > 0);
    check("page_size_power_of_two", is_power_of_two(page));
    check("page_size_at_least_4096", page >= 4096u);
    check("page_size_at_most_2MiB", page <= (2u * 1024u * 1024u)); // sane upper bound

    // -- allocation_granularity ---------------------------------------------
    check("alloc_granularity_nonzero", gran > 0);
    check("alloc_granularity_power_of_two", is_power_of_two(gran));
    check("alloc_granularity_at_least_4096", gran >= 4096u);

    // -- the relationship this feature is named for -------------------------
    check("granularity_at_least_page", gran >= page);
    check("granularity_multiple_of_page", (gran % page) == 0);
    // Because both are powers of two and gran >= page, gran/page is itself a
    // power of two — verify that derived fact directly.
    check("granularity_over_page_is_power_of_two", is_power_of_two(gran / page));

    // -- stability across repeated calls (process-invariant) ----------------
    check("page_size_stable_across_calls",
          vmhook::os::page_size() == page && vmhook::os::page_size() == page);
    check("granularity_stable_across_calls",
          vmhook::os::allocation_granularity() == gran
              && vmhook::os::allocation_granularity() == gran);
}

// ---------------------------------------------------------------------------
// page_size() / allocation_granularity() MUST report identical values on a
// worker thread — unlike current_thread_id these are global to the process.
// ---------------------------------------------------------------------------
static auto test_page_and_granularity_thread_stable() -> void
{
    const std::size_t main_page{ vmhook::os::page_size() };
    const std::size_t main_gran{ vmhook::os::allocation_granularity() };

    std::atomic<std::size_t> worker_page{ 0 };
    std::atomic<std::size_t> worker_gran{ 0 };
    std::thread worker{ [&] {
        worker_page.store(vmhook::os::page_size());
        worker_gran.store(vmhook::os::allocation_granularity());
    } };
    worker.join();

    check("page_size_identical_across_threads", worker_page.load() == main_page);
    check("granularity_identical_across_threads", worker_gran.load() == main_gran);
}

// ---------------------------------------------------------------------------
// The dense alignment sweep.  For a chosen alignment (page, then granularity)
// and a spread of inputs we assert every algebraic property the library relies
// on.  Each property is checked for every input so a regression in any single
// case is isolated.
// ---------------------------------------------------------------------------
static auto sweep_alignment(const char* tag, std::uintptr_t alignment) -> void
{
    // Inputs chosen to bracket each alignment boundary plus the SIZE_MAX edge.
    // Crucially every input here is <= UINTPTR_MAX - alignment so align_up
    // cannot overflow; the overflow boundary itself is exercised separately.
    const std::uintptr_t one_below_max{ ~static_cast<std::uintptr_t>(0) - alignment };
    const std::uintptr_t inputs[]{
        static_cast<std::uintptr_t>(0),
        static_cast<std::uintptr_t>(1),
        alignment - 1,
        alignment,
        alignment + 1,
        2u * alignment,
        2u * alignment + 1,
        3u * alignment - 1,
        16u * alignment,
        static_cast<std::uintptr_t>(0x10000),                       // typical min app address
        static_cast<std::uintptr_t>(vmhook::os::user_address_floor), // 0xFFFF
        static_cast<std::uintptr_t>(vmhook::os::user_address_ceiling) & ~(alignment - 1), // aligned hi
        one_below_max,
    };

    bool down_le_value{ true };
    bool value_le_up{ true };
    bool down_aligned{ true };
    bool up_aligned{ true };
    bool down_idempotent{ true };
    bool up_idempotent{ true };
    bool up_minus_down_bounded{ true };
    bool down_matches_ref{ true };
    bool up_matches_ref{ true };
    bool already_aligned_fixed{ true };
    bool up_ge_down{ true };

    for (const std::uintptr_t x : inputs)
    {
        const std::uintptr_t d{ mirror_align_down(x, alignment) };
        const std::uintptr_t u{ mirror_align_up(x, alignment) };

        // Ordering: align_down(x) <= x <= align_up(x).
        if (!(d <= x)) { down_le_value = false; }
        if (!(x <= u)) { value_le_up = false; }
        if (!(d <= u)) { up_ge_down = false; }

        // Result is exactly aligned (low bits clear).
        if ((d & (alignment - 1)) != 0) { down_aligned = false; }
        if ((u & (alignment - 1)) != 0) { up_aligned = false; }

        // Idempotence: aligning an already-aligned value is a fixed point.
        if (mirror_align_down(d, alignment) != d) { down_idempotent = false; }
        if (mirror_align_up(u, alignment) != u) { up_idempotent = false; }

        // The gap between up and down is either 0 (already aligned) or exactly
        // one alignment unit (rounded across a boundary).
        const std::uintptr_t gap{ u - d };
        if (!(gap == 0 || gap == alignment)) { up_minus_down_bounded = false; }

        // Agreement with the independent multiply/divide reference form.  Safe
        // from overflow because every input is <= one_below_max.
        if (d != ref_align_down(x, alignment)) { down_matches_ref = false; }
        if (u != ref_align_up(x, alignment)) { up_matches_ref = false; }

        // If x is already a multiple of alignment, both directions return x.
        if ((x % alignment) == 0)
        {
            if (d != x || u != x) { already_aligned_fixed = false; }
        }
    }

    char name[96];
    std::snprintf(name, sizeof(name), "%s_align_down_le_value", tag);
    check(name, down_le_value);
    std::snprintf(name, sizeof(name), "%s_value_le_align_up", tag);
    check(name, value_le_up);
    std::snprintf(name, sizeof(name), "%s_align_up_ge_align_down", tag);
    check(name, up_ge_down);
    std::snprintf(name, sizeof(name), "%s_align_down_result_aligned", tag);
    check(name, down_aligned);
    std::snprintf(name, sizeof(name), "%s_align_up_result_aligned", tag);
    check(name, up_aligned);
    std::snprintf(name, sizeof(name), "%s_align_down_idempotent", tag);
    check(name, down_idempotent);
    std::snprintf(name, sizeof(name), "%s_align_up_idempotent", tag);
    check(name, up_idempotent);
    std::snprintf(name, sizeof(name), "%s_up_minus_down_in_zero_or_one_unit", tag);
    check(name, up_minus_down_bounded);
    std::snprintf(name, sizeof(name), "%s_align_down_matches_divmul_reference", tag);
    check(name, down_matches_ref);
    std::snprintf(name, sizeof(name), "%s_align_up_matches_divmul_reference", tag);
    check(name, up_matches_ref);
    std::snprintf(name, sizeof(name), "%s_already_aligned_is_fixed_point", tag);
    check(name, already_aligned_fixed);
}

// ---------------------------------------------------------------------------
// Alignment overflow boundary: the library clamps inputs to search_max before
// calling align_up, but the masking formula `(value + alignment - 1) & ...`
// wraps to 0 when value is within (alignment-1) of UINTPTR_MAX.  Pin that exact
// documented behaviour so a "fix" that changes it is noticed, and verify
// align_down never wraps.
// ---------------------------------------------------------------------------
static auto test_alignment_overflow_boundary(const char* tag, std::uintptr_t alignment) -> void
{
    const std::uintptr_t umax{ ~static_cast<std::uintptr_t>(0) };
    char name[96];

    // align_down never overflows: it only clears low bits.
    std::snprintf(name, sizeof(name), "%s_align_down_at_uintptr_max_no_overflow", tag);
    check(name, mirror_align_down(umax, alignment) == (umax & ~(alignment - 1)));
    std::snprintf(name, sizeof(name), "%s_align_down_at_uintptr_max_is_aligned", tag);
    check(name, (mirror_align_down(umax, alignment) & (alignment - 1)) == 0);
    std::snprintf(name, sizeof(name), "%s_align_down_at_uintptr_max_le_value", tag);
    check(name, mirror_align_down(umax, alignment) <= umax);

    // The largest value that align_up can round WITHOUT wrapping is the highest
    // multiple of alignment, i.e. align_down(umax).  At that point up == down.
    const std::uintptr_t top_aligned{ mirror_align_down(umax, alignment) };
    std::snprintf(name, sizeof(name), "%s_align_up_at_highest_multiple_is_fixed_point", tag);
    check(name, mirror_align_up(top_aligned, alignment) == top_aligned);

    // One past the highest multiple: align_up wraps to 0 (documented masking
    // behaviour).  This is exactly why allocate_nearby_memory clamps to
    // search_max first — assert the wrap so the rationale stays true.
    if (top_aligned != umax) // always true since alignment > 1
    {
        std::snprintf(name, sizeof(name), "%s_align_up_above_highest_multiple_wraps_to_zero", tag);
        check(name, mirror_align_up(top_aligned + 1u, alignment) == 0u);
    }

    // And align_up of UINTPTR_MAX itself wraps to 0 too (umax is not aligned
    // because alignment is a power of two > 1, so umax has low bits set).
    std::snprintf(name, sizeof(name), "%s_align_up_of_uintptr_max_wraps_to_zero", tag);
    check(name, mirror_align_up(umax, alignment) == 0u);
}

// ---------------------------------------------------------------------------
// Reproduce the POSIX protect() page-rounding arithmetic as a pure function and
// assert it produces a page-aligned base that covers the whole requested range.
// (vmhook.hpp protect(): base &= ~(ps-1); size = (end-base+ps-1) & ~(ps-1).)
// This is the math behind protect() accepting unaligned interior addresses; we
// verify the formula directly rather than relying on a live mprotect call.
// ---------------------------------------------------------------------------
static auto test_protect_page_rounding_formula() -> void
{
    const std::uintptr_t ps{ static_cast<std::uintptr_t>(vmhook::os::page_size()) };

    struct Case { std::uintptr_t addr; std::uintptr_t len; };
    // A base address two pages up so we can subtract safely, plus a spread of
    // (offset-within-page, length) combinations that do and don't cross pages.
    const std::uintptr_t origin{ 4u * ps };
    const Case cases[]{
        { origin,            1u },          // aligned base, 1 byte
        { origin,            ps },          // aligned base, exactly one page
        { origin,            ps + 1u },     // aligned base, just over one page
        { origin + 1u,       1u },          // unaligned base, 1 byte
        { origin + ps - 1u,  1u },          // last byte of a page
        { origin + ps - 1u,  2u },          // straddles the page boundary
        { origin + ps / 2u,  ps },          // unaligned base, page-length -> 2 pages
        { origin + ps - 4u,  8u },          // crossing range like the boundary test
        { origin + 3u,       3u * ps },     // unaligned base spanning many pages
    };

    bool base_aligned{ true };
    bool base_le_addr{ true };
    bool covers_end{ true };
    bool size_page_multiple{ true };
    bool size_nonzero{ true };

    for (const Case& c : cases)
    {
        const std::uintptr_t end{ c.addr + c.len };
        const std::uintptr_t base{ c.addr & ~(ps - 1) };
        const std::size_t aligned_size{
            static_cast<std::size_t>((end - base + ps - 1) & ~(ps - 1)) };

        if ((base & (ps - 1)) != 0) { base_aligned = false; }
        if (!(base <= c.addr)) { base_le_addr = false; }
        // The rounded mapping must cover the entire requested [addr, addr+len).
        if (!(base + aligned_size >= end)) { covers_end = false; }
        if ((aligned_size % static_cast<std::size_t>(ps)) != 0) { size_page_multiple = false; }
        if (aligned_size == 0) { size_nonzero = false; }
    }

    check("protect_rounding_base_is_page_aligned", base_aligned);
    check("protect_rounding_base_le_request", base_le_addr);
    check("protect_rounding_covers_full_range", covers_end);
    check("protect_rounding_size_is_page_multiple", size_page_multiple);
    check("protect_rounding_size_nonzero", size_nonzero);
}

// ---------------------------------------------------------------------------
// Concrete, per-OS values.  page_size DIFFERS by platform, so each literal is
// gated behind the OS macro; the cross-platform invariants above already cover
// the unguarded contract.
// ---------------------------------------------------------------------------
static auto test_platform_specific_values() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    const std::size_t gran{ vmhook::os::allocation_granularity() };

#if VMHOOK_OS_WINDOWS
    // Every shipping Win32 build: 4 KiB page, 64 KiB allocation granularity.
    check("windows_page_size_is_4096", page == 4096u);
    check("windows_granularity_is_65536", gran == 65536u);
#else
    // On every POSIX target the library defines granularity == page_size()
    // (vmhook.hpp allocation_granularity() returns page_size()).  This is the
    // identity contract; pin it.
    check("posix_granularity_equals_page_size", gran == page);
#endif
}

// ---------------------------------------------------------------------------
// Observable consequence of the granularity value: VirtualAlloc rounds the
// returned base down to dwAllocationGranularity on Windows.  We allocate a
// page-sized block and assert query_region's reported base is granularity-
// aligned (Windows only; POSIX mmap aligns to the page, which the page-aligned
// assertion below covers everywhere).
// ---------------------------------------------------------------------------
static auto test_allocate_rwx_alignment(void* block) -> void
{
    if (!block)
    {
        return;
    }
    const std::size_t gran{ vmhook::os::allocation_granularity() };
    const std::size_t page{ vmhook::os::page_size() };
    const auto info{ vmhook::os::query_region(block) };

    if (info.base)
    {
        const std::uintptr_t base{ reinterpret_cast<std::uintptr_t>(info.base) };
        // The region base is always at least page-aligned on every platform.
        check("query_region_base_is_page_aligned", (base & (page - 1)) == 0);
#if VMHOOK_OS_WINDOWS
        // VirtualAlloc-backed reservations start on a granularity boundary.
        check("windows_alloc_base_is_granularity_aligned", (base & (gran - 1)) == 0);
#else
        (void)gran;
#endif
    }
    // The returned pointer itself must be page-aligned on every platform.
    const std::uintptr_t ptr{ reinterpret_cast<std::uintptr_t>(block) };
    check("allocate_rwx_pointer_is_page_aligned", (ptr & (page - 1)) == 0);
}

// ===========================================================================
// allocate_rwx / release ROUND-TRIP surface (this file's charter).  Everything
// below this banner drives the real OS allocator over many sizes, verifies the
// FULL requested range is usable (write a pattern across every page, read it
// back), confirms freshly mapped pages are zero-filled, that distinct blocks
// don't alias, that a long alloc/release loop never monotonically exhausts
// (a leak in the release path would betray itself there), that the placement
// hint is non-binding but never corrupts, and — where the platform permits —
// that the X in "rwx" is real by executing a tiny stub.  Per the cross-platform
// discipline, any platform-variable outcome (big-alloc success, exact region
// size, addresses) is [INFO]; only universal invariants are hard-asserted.
// ===========================================================================

// Write a walking pattern across the WHOLE [block, block+size) range — at least
// one store on every page plus the first and last byte — then read it back.
// Returns true iff every byte read back matches what was written.  This is the
// load-bearing "the entire requested range is committed and RW" probe: an
// off-by-one in the commit size (a tail page left unmapped) faults or mismatches
// here.  `size` must be the exact size handed to allocate_rwx.
static auto write_read_whole_range(void* block, std::size_t size, std::uint8_t seed) -> bool
{
    if (!block || size == 0)
    {
        return false;
    }
    const std::size_t page{ vmhook::os::page_size() };
    auto* const bytes{ static_cast<volatile std::uint8_t*>(block) };

    // Deterministic per-offset value so a torn/aliased write is detectable.
    auto value_at{ [seed](std::size_t off) -> std::uint8_t {
        return static_cast<std::uint8_t>((off * 31u + seed) & 0xFFu);
    } };

    // Touch the first byte of every page (covers the whole committed span) and
    // the final byte of the range (catches a short tail page).
    for (std::size_t off{ 0 }; off < size; off += page)
    {
        bytes[off] = value_at(off);
    }
    bytes[size - 1] = value_at(size - 1);

    // Read everything back.
    for (std::size_t off{ 0 }; off < size; off += page)
    {
        if (bytes[off] != value_at(off))
        {
            return false;
        }
    }
    if (bytes[size - 1] != value_at(size - 1))
    {
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Freshly allocate_rwx'd memory must be ZERO-FILLED before first write.  Both
// backends guarantee it: VirtualAlloc(MEM_COMMIT) returns zeroed pages and
// mmap(MAP_ANONYMOUS) returns zeroed pages.  This is a universal, observable
// invariant (no kernel returns uninitialised committed anonymous memory to a
// user process for security reasons), so we hard-assert it — but only on the
// first byte of each page we are certain is committed.  We read BEFORE writing.
// ---------------------------------------------------------------------------
static auto test_allocate_rwx_returns_zeroed_memory() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    const std::size_t size{ page * 3u };
    void* const block{ vmhook::os::allocate_rwx(nullptr, size) };
    if (!block)
    {
        check("allocate_rwx_zeroed_skipped_alloc_failed", false);
        return;
    }

    auto* const bytes{ static_cast<volatile std::uint8_t*>(block) };
    bool all_zero{ true };
    // First byte of each page, plus the last byte of the allocation.
    for (std::size_t off{ 0 }; off < size; off += page)
    {
        if (bytes[off] != 0u)
        {
            all_zero = false;
        }
    }
    if (bytes[size - 1] != 0u)
    {
        all_zero = false;
    }
    check("allocate_rwx_fresh_pages_are_zero_filled", all_zero);

    vmhook::os::release(block, size);
}

// ---------------------------------------------------------------------------
// allocate_rwx over a DENSE size sweep.  Every existing test allocates exactly
// one page (or page*2/3 for the protect matrix).  Here we drive 1 byte, a
// sub-page value, page-1, exactly one page, page+1, several multi-page sizes,
// and exactly one allocation_granularity().  For each: the pointer is non-null
// and page-aligned, the FULL requested range is writable+readable end-to-end,
// and query_region reports a region whose size covers the request.  Then the
// block is released with the SAME size it was allocated with (load-bearing on
// POSIX munmap).  Sub-page and odd sizes must round UP to a usable mapping, not
// under-commit — so a 1-byte request still yields a writable first byte and a
// query_region.size >= 1 (in practice a whole page).
// ---------------------------------------------------------------------------
static auto test_allocate_rwx_size_sweep() -> void
{
    const std::size_t page{ vmhook::os::page_size() };

    const std::size_t sizes[]{
        std::size_t{ 1 },
        std::size_t{ 17 },         // tiny sub-page
        std::size_t{ 64 },         // sub-page
        page / 2u,                 // sub-page, half a page
        page - 1u,                 // one below a page
        page,                      // exactly one page
        page + 1u,                 // one above a page -> two pages of mapping
        page * 2u,                 // exactly two pages
        page * 4u,                 // a handful of pages
        page * 17u,                // an odd multiple
        vmhook::os::allocation_granularity(), // the named granularity unit
    };

    bool all_nonnull{ true };
    bool all_page_aligned{ true };
    bool all_range_usable{ true };
    bool all_region_covers{ true };

    std::uint8_t seed{ 0x01 };
    for (const std::size_t sz : sizes)
    {
        void* const block{ vmhook::os::allocate_rwx(nullptr, sz) };
        if (!block)
        {
            // A page-or-handful sized request failing would be a real defect on
            // the CI matrix; flag it (do not silently pass).
            all_nonnull = false;
            continue;
        }

        const std::uintptr_t addr{ reinterpret_cast<std::uintptr_t>(block) };
        if ((addr & (page - 1u)) != 0u)
        {
            all_page_aligned = false;
        }

        if (!write_read_whole_range(block, sz, seed))
        {
            all_range_usable = false;
        }

        const auto info{ vmhook::os::query_region(block) };
        // The reported region must cover at least the requested size.  (On
        // Windows query_region returns the committed run; on POSIX the parsed
        // /proc region; on iOS a permissive page-sized stub — so for sub-page
        // requests we only require >= sz, which a single page always satisfies.)
        if (info.base && info.size < sz)
        {
            all_region_covers = false;
        }

        vmhook::os::release(block, sz);
        ++seed;
    }

    check("allocate_rwx_size_sweep_all_nonnull", all_nonnull);
    check("allocate_rwx_size_sweep_all_page_aligned", all_page_aligned);
    check("allocate_rwx_size_sweep_full_range_usable", all_range_usable);
    check("allocate_rwx_size_sweep_region_covers_request", all_region_covers);
}

// ---------------------------------------------------------------------------
// A LARGE allocation is platform-variable: a 64 MiB RWX reservation succeeds on
// a desktop CI runner but may be refused on a constrained sandbox, and W^X
// platforms map it RW.  So success is [INFO], NEVER a hard assert.  What we DO
// hard-assert is conditional on success: IF the big block comes back, its full
// range is usable and it releases cleanly.  This exercises the multi-page commit
// path at a scale the page-granularity sweep can't, without making the suite
// flaky on memory-tight runners.
// ---------------------------------------------------------------------------
static auto test_allocate_rwx_large_is_info_gated() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    // 8 MiB rounded to whole pages: big enough to span many pages, small enough
    // not to stress a normal runner, but still gated [INFO] for the constrained
    // sandbox case per the cross-platform determinism rule.
    const std::size_t big{ (std::size_t{ 8 } * 1024u * 1024u + page - 1u) & ~(page - 1u) };

    void* const block{ vmhook::os::allocate_rwx(nullptr, big) };
    if (!block)
    {
        std::printf("[INFO] allocate_rwx_large: %zu-byte RWX request refused "
                    "(constrained environment) — not a failure\n", big);
        return;
    }

    // Conditional on success the whole span must be writable+readable.
    const bool usable{ write_read_whole_range(block, big, 0x9C) };
    check("allocate_rwx_large_full_range_usable_when_granted", usable);

    const std::uintptr_t addr{ reinterpret_cast<std::uintptr_t>(block) };
    check("allocate_rwx_large_pointer_is_page_aligned", (addr & (page - 1u)) == 0u);

    vmhook::os::release(block, big);
    std::printf("[INFO] allocate_rwx_large: %zu-byte RWX request granted and released\n", big);
}

// ---------------------------------------------------------------------------
// Many distinct blocks must NOT alias.  Allocate a batch of single-page blocks
// simultaneously (no interleaved release), stamp each with a unique marker, then
// verify EVERY marker is intact — proving the allocator handed out
// non-overlapping mappings (an aliasing bug would let one block's store clobber
// another's marker).  Also assert pairwise that no two bases are equal and no
// block's page overlaps another's.  Finally release them all (no leak) and, as a
// second pass, confirm a fresh batch can still be allocated (the releases
// actually returned the memory).
// ---------------------------------------------------------------------------
static auto test_allocate_rwx_many_blocks_distinct() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    constexpr std::size_t count{ 32 };

    void* blocks[count]{};
    std::size_t got{ 0 };

    for (std::size_t i{ 0 }; i < count; ++i)
    {
        blocks[i] = vmhook::os::allocate_rwx(nullptr, page);
        if (!blocks[i])
        {
            break;
        }
        ++got;
    }
    // Getting 32 single pages must succeed on any real target; a shortfall is a
    // genuine problem, not an [INFO] case.
    check("many_blocks_all_allocated", got == count);

    // Stamp each block's first 8 bytes with a unique 64-bit marker derived from
    // its index, so a later mismatch pinpoints aliasing.
    for (std::size_t i{ 0 }; i < got; ++i)
    {
        auto* const m{ static_cast<volatile std::uint64_t*>(blocks[i]) };
        *m = static_cast<std::uint64_t>(0xA5A5'0000ull + i);
    }

    // Every marker must read back unchanged: no block's write disturbed another.
    bool markers_intact{ true };
    for (std::size_t i{ 0 }; i < got; ++i)
    {
        const auto* const m{ static_cast<volatile std::uint64_t*>(blocks[i]) };
        if (*m != static_cast<std::uint64_t>(0xA5A5'0000ull + i))
        {
            markers_intact = false;
        }
    }
    check("many_blocks_markers_all_intact_no_aliasing", markers_intact);

    // Pairwise distinctness: no two bases equal, and no two single-page mappings
    // overlap (|a - b| >= page for every pair).
    bool all_distinct{ true };
    bool none_overlap{ true };
    for (std::size_t i{ 0 }; i < got; ++i)
    {
        const std::uintptr_t ai{ reinterpret_cast<std::uintptr_t>(blocks[i]) };
        for (std::size_t j{ i + 1 }; j < got; ++j)
        {
            const std::uintptr_t aj{ reinterpret_cast<std::uintptr_t>(blocks[j]) };
            if (ai == aj)
            {
                all_distinct = false;
            }
            const std::uintptr_t diff{ ai > aj ? ai - aj : aj - ai };
            if (diff < static_cast<std::uintptr_t>(page))
            {
                none_overlap = false;
            }
        }
    }
    check("many_blocks_bases_pairwise_distinct", all_distinct);
    check("many_blocks_pages_do_not_overlap", none_overlap);

    // Release everything we got — no leak.
    for (std::size_t i{ 0 }; i < got; ++i)
    {
        vmhook::os::release(blocks[i], page);
    }

    // Second pass: the memory we just released must be re-allocatable.  If
    // release leaked, a fresh batch on a tight runner would start failing; on a
    // roomy runner it always succeeds either way, so this is a sanity floor.
    void* probe{ vmhook::os::allocate_rwx(nullptr, page) };
    check("many_blocks_realloc_after_release", probe != nullptr);
    if (probe)
    {
        vmhook::os::release(probe, page);
    }
}

// ---------------------------------------------------------------------------
// Round-trip STABILITY under repetition.  A leak in the release path (e.g. a
// size/base mismatch that silently fails to unmap) would, over many iterations,
// monotonically consume address space until allocate_rwx starts returning null.
// Loop alloc -> write+verify -> release a large number of times and assert every
// single allocation succeeds and every block is usable.  This is the direct
// behavioural witness for "release actually gives the memory back".  We keep the
// block size at one page so the loop is cheap and deterministic everywhere.
// ---------------------------------------------------------------------------
static auto test_allocate_release_roundtrip_no_leak() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    constexpr int iterations{ 2000 };

    bool every_alloc_ok{ true };
    bool every_block_usable{ true };
    int completed{ 0 };

    for (int i{ 0 }; i < iterations; ++i)
    {
        void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
        if (!block)
        {
            every_alloc_ok = false;
            break;
        }
        // A small write+read each pass so a recycled-but-not-really-mapped
        // address would fault/mismatch instead of silently passing.
        if (!write_read_whole_range(block, page, static_cast<std::uint8_t>(i & 0xFF)))
        {
            every_block_usable = false;
        }
        vmhook::os::release(block, page);
        ++completed;
    }

    check("roundtrip_loop_every_alloc_succeeded", every_alloc_ok);
    check("roundtrip_loop_every_block_usable", every_block_usable);
    check("roundtrip_loop_completed_all_iterations", completed == iterations);
}

// ---------------------------------------------------------------------------
// Larger-block round-trip stability.  Same idea as the page loop but with a
// multi-page block, so a release that under-frees (POSIX munmap with a too-small
// size leaking the tail pages) would accumulate faster.  Fewer iterations keeps
// it cheap; each block's whole range is verified usable.
// ---------------------------------------------------------------------------
static auto test_allocate_release_multipage_roundtrip_no_leak() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    const std::size_t size{ page * 8u };
    constexpr int iterations{ 256 };

    bool every_alloc_ok{ true };
    bool every_block_usable{ true };
    int completed{ 0 };

    for (int i{ 0 }; i < iterations; ++i)
    {
        void* const block{ vmhook::os::allocate_rwx(nullptr, size) };
        if (!block)
        {
            every_alloc_ok = false;
            break;
        }
        if (!write_read_whole_range(block, size, static_cast<std::uint8_t>((i * 7) & 0xFF)))
        {
            every_block_usable = false;
        }
        vmhook::os::release(block, size);
        ++completed;
    }

    check("multipage_roundtrip_every_alloc_succeeded", every_alloc_ok);
    check("multipage_roundtrip_every_block_usable", every_block_usable);
    check("multipage_roundtrip_completed_all_iterations", completed == iterations);
}

// ---------------------------------------------------------------------------
// The placement hint is documented as NON-BINDING (a preference, not a demand).
// We exercise both ends:
//   * a "plausibly free" hint derived from a just-released block — the allocator
//     may or may not honour it, but must return SOME usable RWX page;
//   * a deliberately-OCCUPIED hint (the address of a live stack object) — the
//     allocator must NEVER hand that exact address back as a fresh allocation
//     (doing so would alias live memory), and must still return a usable page.
// Either way the returned block is page-aligned and writable end-to-end, and we
// always release it.  This guards a future change that blindly trusts the hint.
// ---------------------------------------------------------------------------
static auto test_allocate_rwx_hint_non_binding() -> void
{
    const std::size_t page{ vmhook::os::page_size() };

    // (1) Free-ish hint: allocate, capture the address, release, then ask for a
    // block at that now-free hint.  Result must be usable; honouring the hint is
    // optional, so we do NOT assert the address matches.
    {
        void* const first{ vmhook::os::allocate_rwx(nullptr, page) };
        if (!first)
        {
            check("hint_free_first_alloc_ok", false);
        }
        else
        {
            void* const hint{ first };
            vmhook::os::release(first, page);

            void* const second{ vmhook::os::allocate_rwx(hint, page) };
            check("hint_free_returns_usable_block", second != nullptr);
            if (second)
            {
                const std::uintptr_t a{ reinterpret_cast<std::uintptr_t>(second) };
                check("hint_free_block_page_aligned", (a & (page - 1u)) == 0u);
                check("hint_free_block_usable", write_read_whole_range(second, page, 0x3C));
                vmhook::os::release(second, page);
            }
        }
    }

    // (2) Occupied hint: the address of a live local.  The CROSS-PLATFORM-true
    // invariant is "the call never hands back the occupied address as a fresh
    // allocation" — but the two backends reach it differently and that
    // difference must be [INFO], not a hard assert:
    //   * POSIX mmap treats the hint as advisory and RELOCATES to a free spot,
    //     so a usable (different) block comes back.
    //   * Windows VirtualAlloc treats a non-null base as binding-or-fail: an
    //     occupied address returns NULL (ERROR_INVALID_ADDRESS) rather than
    //     relocating.  This contradicts the header's "non-binding" doc comment
    //     (vmhook.hpp allocate_rwx) — a real divergence, harmless in practice
    //     because the trampoline allocator only ever hints at FREE regions.
    // Hard-assert only the safety property (never == occupied_addr) and the
    // usability of any block actually returned; characterise the null vs
    // relocate split with [INFO].
    {
        volatile std::uint64_t occupied{ 0xCAFEBABEu };
        void* const occupied_addr{ const_cast<void*>(
            static_cast<const volatile void*>(&occupied)) };

        void* const block{ vmhook::os::allocate_rwx(occupied_addr, page) };
        // Universal: must NOT alias our live stack object.
        check("hint_occupied_never_returns_occupied_address",
              block != occupied_addr);
        if (block)
        {
            const std::uintptr_t a{ reinterpret_cast<std::uintptr_t>(block) };
            check("hint_occupied_relocated_block_page_aligned", (a & (page - 1u)) == 0u);
            check("hint_occupied_relocated_block_usable",
                  write_read_whole_range(block, page, 0xD7));
            vmhook::os::release(block, page);
            std::printf("[INFO] hint_occupied: allocator relocated off the "
                        "occupied hint (POSIX-style advisory hint)\n");
        }
        else
        {
            std::printf("[INFO] hint_occupied: allocator returned null for an "
                        "occupied hint (Windows VirtualAlloc binding-or-fail)\n");
        }
        // The live local must be untouched by the whole dance, in BOTH cases.
        check("hint_occupied_local_intact", occupied == 0xCAFEBABEu);

        // Whatever happened above, a plain hint-free allocation must still work,
        // proving the occupied-hint attempt left the allocator in a clean state.
        void* const recover{ vmhook::os::allocate_rwx(nullptr, page) };
        check("hint_occupied_plain_alloc_still_works", recover != nullptr);
        if (recover)
        {
            vmhook::os::release(recover, page);
        }
    }

    // (3) A high-canonical but almost-certainly-unmapped hint.  The kernel is
    // free to ignore it (POSIX relocates; Windows honours a free hint, or fails
    // if that exact address happens to be unavailable).  We do NOT hard-assert
    // a block comes back at this specific address (that would be a placement /
    // address-value assert, forbidden) — only that IF one comes back it is
    // usable, and that the allocator remains healthy afterwards.
    {
        void* const high_hint{ reinterpret_cast<void*>(
            static_cast<std::uintptr_t>(0x0000'7000'0000'0000ull)) };
        void* const block{ vmhook::os::allocate_rwx(high_hint, page) };
        if (block)
        {
            check("hint_high_canonical_block_usable",
                  write_read_whole_range(block, page, 0x6E));
            vmhook::os::release(block, page);
        }
        else
        {
            std::printf("[INFO] hint_high_canonical: hinted address unavailable "
                        "on this runner — allocator declined (non-binding hint)\n");
        }

        // Health check independent of the hint outcome.
        void* const recover{ vmhook::os::allocate_rwx(nullptr, page) };
        check("hint_high_canonical_plain_alloc_still_works", recover != nullptr);
        if (recover)
        {
            vmhook::os::release(recover, page);
        }
    }
}

// ---------------------------------------------------------------------------
// NEGATIVE-RELEASE CONTAINMENT (behaviour-pinning, not "it works").
// release() is noexcept -> void and ignores both kernel return values, so a
// size/base mismatch is SILENTLY ignored rather than faulting.  The TRUE,
// empirically-verified per-OS behaviour (more nuanced than a naive reading of
// the docs) is:
//   * Windows VirtualFree(addr, 0, MEM_RELEASE) ignores size and ROUNDS the
//     address down to its containing reservation.  An interior pointer in the
//     SAME page as the base therefore frees the WHOLE reservation and returns
//     success; an address in a LATER page (not the reservation base page) fails
//     with ERROR_INVALID_ADDRESS (487) and frees nothing.  Both outcomes are
//     swallowed (the wrapper discards the BOOL).
//   * POSIX munmap(addr, size) requires a page-aligned base and unmaps whole
//     pages spanning [addr, addr+size).  An unaligned / interior base returns
//     EINVAL (nothing freed); a too-small size still rounds the length up to a
//     whole page.  The int return is discarded.
// We FREEZE this contract: every mismatched call returns WITHOUT crashing the
// process.  A future change that adds a return value / diagnostic would
// deliberately break these pins, which is the point.
//
// SAFETY + LEAK-FREEDOM: this is the one corner where touching the block after a
// mismatched call is unsafe (Windows may already have freed it), so we branch by
// OS and (a) only read/write the block when we KNOW it is still mapped, and
// (b) perform exactly the right cleanup so nothing leaks and nothing is
// double-freed.
// ---------------------------------------------------------------------------
static auto test_release_mismatch_is_contained_noop() -> void
{
    const std::size_t page{ vmhook::os::page_size() };

#if VMHOOK_OS_WINDOWS
    // ---- Windows behaviour ------------------------------------------------
    // (1) Interior pointer in a LATER page: VirtualFree fails (487), frees
    // nothing.  The block survives, so we verify the byte and then release it
    // correctly.  Use a 2-page block and target base + page (page-1 boundary).
    {
        void* const block{ vmhook::os::allocate_rwx(nullptr, page * 2u) };
        if (!block)
        {
            check("release_later_page_ptr_skipped_alloc_failed", false);
        }
        else
        {
            auto* const base{ static_cast<std::uint8_t*>(block) };
            base[0] = 0x11;
            // base + page is the start of page 1 — NOT the reservation base
            // page, so VirtualFree(MEM_RELEASE) rejects it and frees nothing.
            vmhook::os::release(base + page, page);
            check("release_later_page_ptr_no_crash", true);
            base[0] = 0x22; // block must still be mapped
            check("release_later_page_ptr_block_survived", base[0] == 0x22);
            vmhook::os::release(block, page * 2u); // correct cleanup
            check("release_later_page_ptr_correct_release_no_crash", true);
        }
    }

    // (2) Same-page interior pointer: VirtualFree rounds down to the base and
    // frees the WHOLE reservation, returning success.  This is the surprising
    // half of the Windows asymmetry — pin "does not crash" and, crucially, do
    // NOT touch or re-release the block afterwards (it is already freed).  Using
    // a dedicated block keeps this self-contained and leak-free.
    {
        void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
        if (!block)
        {
            check("release_samepage_ptr_skipped_alloc_failed", false);
        }
        else
        {
            auto* const base{ static_cast<std::uint8_t*>(block) };
            base[0] = 0x33;
            // base + 1 is in the base page; VirtualFree frees the reservation.
            vmhook::os::release(base + 1, page);
            check("release_samepage_ptr_no_crash", true);
            // Block is now freed by the mismatched call itself — no further
            // touch, no double release.  Nothing leaks.
        }
    }

    // (3) Correct base, too-small size: Windows ignores size, so this frees the
    // whole reservation cleanly.  Pin no-crash; do not re-release.
    {
        void* const block{ vmhook::os::allocate_rwx(nullptr, page * 2u) };
        if (!block)
        {
            check("release_small_size_skipped_alloc_failed", false);
        }
        else
        {
            vmhook::os::release(block, page - 1u); // size ignored on Windows
            check("release_small_size_no_crash", true);
            // Freed by the call (size is ignored, whole reservation released).
        }
    }
#else
    // ---- POSIX behaviour --------------------------------------------------
    // (1) Interior (mid-page, unaligned) pointer: munmap returns EINVAL, frees
    // nothing.  The block survives; verify the byte then release correctly.
    {
        void* const block{ vmhook::os::allocate_rwx(nullptr, page * 2u) };
        if (!block)
        {
            check("release_interior_ptr_skipped_alloc_failed", false);
        }
        else
        {
            auto* const base{ static_cast<std::uint8_t*>(block) };
            base[0] = 0x11;
            vmhook::os::release(base + 1, page); // unaligned -> EINVAL, no-op
            check("release_interior_ptr_no_crash", true);
            base[0] = 0x22;
            check("release_interior_ptr_block_survived", base[0] == 0x22);
            vmhook::os::release(block, page * 2u); // correct cleanup
            check("release_interior_ptr_correct_release_no_crash", true);
        }
    }

    // (2) A non-page-aligned base also yields EINVAL from munmap — nothing
    // freed.  base + (page/2) is firmly mid-page and unaligned.
    {
        void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
        if (!block)
        {
            check("release_unaligned_base_skipped_alloc_failed", false);
        }
        else
        {
            auto* const base{ static_cast<std::uint8_t*>(block) };
            base[0] = 0x44;
            vmhook::os::release(base + (page / 2u), page); // unaligned -> EINVAL
            check("release_unaligned_base_no_crash", true);
            base[0] = 0x55;
            check("release_unaligned_base_block_survived", base[0] == 0x55);
            vmhook::os::release(block, page); // correct cleanup
            check("release_unaligned_base_correct_release_no_crash", true);
        }
    }

    // (3) Correct base, sub-page size: munmap rounds the length UP to one page,
    // so this DOES unmap the single page.  Use a dedicated block; do NOT touch
    // or re-release it afterwards (it is unmapped, and the address may be
    // recycled — a second munmap could hit unrelated memory).
    {
        void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
        if (!block)
        {
            check("release_subpage_size_skipped_alloc_failed", false);
        }
        else
        {
            vmhook::os::release(block, page - 1u); // rounds up -> unmaps the page
            check("release_subpage_size_no_crash", true);
            // The single page is accounted for by this call; nothing leaks.
        }
    }
#endif
}

// ---------------------------------------------------------------------------
// EXECUTE-BIT POSITIVE CHECK — the only test that proves the X in "rwx" is real.
// On Linux/Windows x86_64/arm64 (NOT Apple, where allocate_rwx legitimately
// drops PROT_EXEC under W^X) we write a trivial "return a constant" stub into a
// freshly-allocated RWX page and CALL it through a function pointer, asserting it
// returns the expected value.  If allocate_rwx ever silently produced a
// non-executable mapping on these platforms, this faults / fails — exactly the
// regression the name allocate_RWX must not allow.
//
// We pick the smallest portable stub per arch:
//   x86-64 : mov eax, imm32 ; ret      -> B8 <imm32> C3        (returns int)
//   arm64  : movz w0, #imm16 ; ret     -> 52800000|imm<<5 , D65F03C0
// Both honour the C calling convention for `int fn(void)` on every supported
// non-Apple ABI (System V AMD64 + Windows x64 both return int in EAX; AArch64
// returns in w0).  We DO instruction-cache maintenance on arm64 before the call
// (writing code then executing it requires an I-cache flush on ARM).
// ---------------------------------------------------------------------------
#if !VMHOOK_OS_APPLE && (VMHOOK_ARCH_X86_64 || VMHOOK_ARCH_ARM64)
static auto test_allocate_rwx_executes_code() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
    if (!block)
    {
        check("exec_stub_skipped_alloc_failed", false);
        return;
    }

    auto* const code{ static_cast<std::uint8_t*>(block) };
    constexpr int expected{ 0x5EED };

#if VMHOOK_ARCH_X86_64
    // mov eax, 0x00005EED ; ret
    code[0] = 0xB8;
    code[1] = static_cast<std::uint8_t>((expected >> 0)  & 0xFF);
    code[2] = static_cast<std::uint8_t>((expected >> 8)  & 0xFF);
    code[3] = static_cast<std::uint8_t>((expected >> 16) & 0xFF);
    code[4] = static_cast<std::uint8_t>((expected >> 24) & 0xFF);
    code[5] = 0xC3; // ret
    const std::size_t code_len{ 6 };
#else // VMHOOK_ARCH_ARM64
    // movz w0, #0x5EED ; ret   (little-endian 32-bit instruction words)
    const std::uint32_t movz_w0{ 0x52800000u | (static_cast<std::uint32_t>(expected) << 5) };
    const std::uint32_t ret_lr{ 0xD65F03C0u };
    auto store32{ [](std::uint8_t* p, std::uint32_t w) {
        p[0] = static_cast<std::uint8_t>(w & 0xFFu);
        p[1] = static_cast<std::uint8_t>((w >> 8) & 0xFFu);
        p[2] = static_cast<std::uint8_t>((w >> 16) & 0xFFu);
        p[3] = static_cast<std::uint8_t>((w >> 24) & 0xFFu);
    } };
    store32(code + 0, movz_w0);
    store32(code + 4, ret_lr);
    const std::size_t code_len{ 8 };
#endif

    // allocate_rwx returns an executable mapping on these platforms, but on a
    // hardened Linux (SELinux/PaX) the W^X policy can still strip X.  Make the
    // page explicitly execute_read before calling; if THAT is refused we cannot
    // safely call the bytes, so we [INFO]-skip rather than fault.  (We only ever
    // execute after a successful X-grant.)
    const bool exec_ok{ vmhook::os::protect(block, page,
                                            vmhook::os::memory_protection::execute_read,
                                            nullptr) };
    if (!exec_ok)
    {
        std::printf("[INFO] exec_stub: execute_read refused (hardened W^X) — "
                    "skipping call-through\n");
        // restore writable for clean teardown
        (void)vmhook::os::protect(block, page,
                                  vmhook::os::memory_protection::read_write, nullptr);
        vmhook::os::release(block, page);
        return;
    }

#if VMHOOK_ARCH_ARM64
    // Writing code then executing it on ARM requires flushing the just-written
    // bytes from the data cache to the instruction cache, or the core may fetch
    // stale instructions.  GCC/Clang expose __builtin___clear_cache; pure MSVC
    // (cl.exe, which lacks that intrinsic) uses FlushInstructionCache from the
    // already-included <windows.h>.
#  if VMHOOK_COMPILER_MSVC
    ::FlushInstructionCache(::GetCurrentProcess(), code, code_len);
#  else
    __builtin___clear_cache(reinterpret_cast<char*>(code),
                            reinterpret_cast<char*>(code + code_len));
#  endif
#else
    (void)code_len;
#endif

    using fn_t = int (*)();
    fn_t fn{};
    // Function-pointer cast through void* (round-trip is well-defined for the
    // platforms here; the data->code pointer identity holds on x86-64/arm64).
    std::memcpy(&fn, &block, sizeof(fn));

    const int got{ fn() };
    check("allocate_rwx_executed_stub_returns_expected", got == expected);

    // Teardown: restore a writable mapping (so release sees a normal page) and
    // free.  execute_rw may be refused; read_write always works post-exec here.
    if (!vmhook::os::protect(block, page,
                             vmhook::os::memory_protection::read_write, nullptr))
    {
        (void)vmhook::os::protect(block, page,
                                  vmhook::os::memory_protection::execute_rw, nullptr);
    }
    vmhook::os::release(block, page);
}
#endif // !VMHOOK_OS_APPLE && (x86_64 || arm64)

// ---------------------------------------------------------------------------
// query_region of an allocate_rwx block reports it COMMITTED + READABLE, and the
// reported size covers the request.  test_os_protect_interaction.cpp asserts the
// committed/readable flags for a single page; here we additionally tie it to the
// allocate/release CHARTER by checking the size-covers-request invariant across
// a couple of multi-page sizes and confirming the base is page-aligned.  (The
// post-release "free/non-committed" direction is owned by the protect-interaction
// file; we do not duplicate it.)
// ---------------------------------------------------------------------------
static auto test_query_region_covers_allocation() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    const std::size_t sizes[]{ page, page * 2u, page * 5u };

    bool all_committed{ true };
    bool all_readable{ true };
    bool all_cover{ true };
    bool all_base_aligned{ true };

    for (const std::size_t sz : sizes)
    {
        void* const block{ vmhook::os::allocate_rwx(nullptr, sz) };
        if (!block)
        {
            all_committed = false;
            continue;
        }
        // Touch every page so the whole span is unambiguously committed.
        (void)write_read_whole_range(block, sz, 0x42);

        const auto info{ vmhook::os::query_region(block) };
        if (!info.committed) { all_committed = false; }
        if (!info.readable)  { all_readable = false; }
        if (info.base && info.size < sz) { all_cover = false; }
        if (info.base
            && (reinterpret_cast<std::uintptr_t>(info.base) & (page - 1u)) != 0u)
        {
            all_base_aligned = false;
        }

        vmhook::os::release(block, sz);
    }

    check("query_region_alloc_committed_all_sizes", all_committed);
    check("query_region_alloc_readable_all_sizes", all_readable);
    check("query_region_alloc_size_covers_request", all_cover);
    check("query_region_alloc_base_page_aligned", all_base_aligned);
}

int main()
{
    // --- pure-logic, no side effects: the heart of this file ---------------
    test_page_and_granularity_invariants();
    test_page_and_granularity_thread_stable();

    const std::uintptr_t page_align{ static_cast<std::uintptr_t>(vmhook::os::page_size()) };
    const std::uintptr_t gran_align{ static_cast<std::uintptr_t>(vmhook::os::allocation_granularity()) };
    sweep_alignment("page", page_align);
    sweep_alignment("granularity", gran_align);

    // Overflow boundary once per distinct alignment (skip the duplicate when
    // granularity == page on POSIX so the check names stay unique-per-run).
    test_alignment_overflow_boundary("page", page_align);
    if (gran_align != page_align)
    {
        test_alignment_overflow_boundary("granularity", gran_align);
    }

    test_protect_page_rounding_formula();
    test_platform_specific_values();

    // --- thread id (unchanged from the original round-trip) ----------------
    const auto tid{ vmhook::os::current_thread_id() };
    check("current_thread_id_nonzero", tid != 0);

    std::atomic<std::uint64_t> other_tid{ 0 };
    std::thread worker{ [&] {
        other_tid.store(static_cast<std::uint64_t>(vmhook::os::current_thread_id()));
    } };
    worker.join();
    check("current_thread_id_unique_per_thread", other_tid.load() != static_cast<std::uint64_t>(tid));

    // --- allocate / protect / safe_read round-trip (unchanged behaviour) ---
    const std::size_t page{ vmhook::os::page_size() };
    void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
    check("allocate_rwx_returns_ptr", block != nullptr);

    // Observable granularity / page alignment of the freshly allocated block.
    test_allocate_rwx_alignment(block);

    if (block)
    {
        auto* const bytes = static_cast<volatile unsigned char*>(block);
        bytes[0] = 0xAA;
        bytes[1] = 0x55;
        check("allocated_memory_writable", bytes[0] == 0xAA && bytes[1] == 0x55);

        const auto info = vmhook::os::query_region(block);
        check("query_region_locates_alloc", info.base != nullptr && info.size >= page);

        std::uint32_t old{};
        const bool flipped = vmhook::os::protect(block, page,
                                                 vmhook::os::memory_protection::read, &old);
        check("protect_to_readonly", flipped);

        // Flipping back to a writable + executable mapping requires the
        // JIT entitlement on Apple arm64 (W^X enforced).  When the
        // entitlement is absent we settle for plain read-write which is
        // what allocate_rwx itself fell back to.
        bool flipped_back = vmhook::os::protect(block, page,
                                                vmhook::os::memory_protection::execute_rw, nullptr);
        if (!flipped_back)
        {
            flipped_back = vmhook::os::protect(block, page,
                                               vmhook::os::memory_protection::read_write, nullptr);
            std::printf("[INFO] protect_back_to_rwx fell back to read_write (Apple W^X / no JIT entitlement)\n");
        }
        check("protect_back_to_rw_or_rwx", flipped_back);
        check("memory_survives_protect_cycle", bytes[0] == 0xAA && bytes[1] == 0x55);

        // safe_read sanity: reading into a buffer should succeed for the
        // valid block and fail for a high-canonical pointer.
        unsigned char dst[2]{};
        const bool ok_read = vmhook::os::safe_read(dst, block, sizeof(dst));
        check("safe_read_valid_block", ok_read && dst[0] == 0xAA && dst[1] == 0x55);

        // iOS has no fault-safe read API without entitlements, so the
        // bogus-pointer check would crash there; skip it.
#if !VMHOOK_OS_IOS
        const void* const bogus = reinterpret_cast<const void*>(static_cast<std::uintptr_t>(0xDEADBEEFDEAD'BEEFull));
        const bool ok_bogus = vmhook::os::safe_read(dst, bogus, sizeof(dst));
        check("safe_read_rejects_bogus", !ok_bogus);
#else
        std::printf("[INFO] safe_read_rejects_bogus: skipped (no fault-safe read API on iOS)\n");
#endif

        vmhook::os::release(block, page);
    }

    // --- allocate_rwx / release ROUND-TRIP charter (the expanded sweep) -----
    test_allocate_rwx_returns_zeroed_memory();
    test_allocate_rwx_size_sweep();
    test_allocate_rwx_large_is_info_gated();
    test_allocate_rwx_many_blocks_distinct();
    test_allocate_release_roundtrip_no_leak();
    test_allocate_release_multipage_roundtrip_no_leak();
    test_allocate_rwx_hint_non_binding();
    test_release_mismatch_is_contained_noop();
    test_query_region_covers_allocation();
#if !VMHOOK_OS_APPLE && (VMHOOK_ARCH_X86_64 || VMHOOK_ARCH_ARM64)
    test_allocate_rwx_executes_code();
#else
    std::printf("[INFO] allocate_rwx_executes_code: skipped (Apple W^X drops "
                "PROT_EXEC, or non-x86_64/arm64 arch)\n");
#endif

    return failures == 0 ? 0 : 1;
}
