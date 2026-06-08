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
#include <vmhook/vmhook.hpp>

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

    return failures == 0 ? 0 : 1;
}
