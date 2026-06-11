// Exhaustive, JVM-free coverage of the OS-layer memory geometry primitives and
// the address-alignment arithmetic built on top of them.
//
// WHAT THIS FILE OWNS
// -------------------
//   * vmhook::os::page_size()              (vmhook.hpp)
//   * vmhook::os::allocation_granularity() (vmhook.hpp)
//   * the align_up / align_down masking arithmetic the trampoline allocator
//     (allocate_nearby_memory) is built on — reproduced here byte-for-byte from
//     the header's private lambdas, since they are not exported symbols.
//
// DESIGN: COMPILE-TIME FIRST
// --------------------------
// The pure alignment/rounding arithmetic is deterministic and total, so the bulk
// of the coverage is `static_assert` — it costs nothing at runtime, can never
// flake, and fails the BUILD (not a test run) the instant the contract breaks.
// We drive the mirrored formula through an EXHAUSTIVE matrix:
//   * every alignment that is a power of two and fits a 64-bit uintptr_t
//     (1, 2, 4, … 2^31, plus the two real-world values 4096 / 65536), walked by
//     a recursive consteval sweep so no power is skipped;
//   * a dense per-alignment input set: 0, 1, alignment-1, alignment,
//     alignment+1, multiple boundaries, and the high end of the address space
//     one alignment below UINTPTR_MAX;
//   * the documented overflow/wrap behaviour of the masking form near
//     UINTPTR_MAX (align_up wraps to 0, align_down never wraps) — pinned so a
//     "fix" that changes it is a conscious, tested decision.
// Properties asserted for every (alignment, input): ordering
// (down <= x <= up), exact alignment of each result, idempotence, the up-down
// gap being exactly 0 or one alignment unit, and agreement with an independent
// divide/multiply reference implementation.
//
// The only RUNTIME portion is the handful of values that must be queried from
// the live OS — page_size() / allocation_granularity() — which we check are
// sane (non-zero, power of two, within plausible bounds) and satisfy the
// invariant this feature is named for (granularity is a whole multiple of, and
// >= , the page size).  These are process-invariant constants, so they are just
// as deterministic as the static_asserts — no JVM, no allocation, no flakes.
//
// PORTABILITY: page_size differs by OS/arch (4 KiB on x86, 16 KiB on Apple
// arm64, 64 KiB on some aarch64 kernels) and allocation_granularity is 64 KiB on
// Windows but == page_size on every POSIX target.  Every assertion here is
// therefore an INVARIANT that holds on all of them; the two genuinely
// platform-specific facts (Win32 4096/65536, POSIX granularity==page) are pinned
// behind the VMHOOK_OS_* macros at the end.  No JVM is involved at any point.
#include <vmhook/vmhook.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <thread>

namespace
{

// ───────────────────────────────────────────────────────────────────────────
// Byte-for-byte mirror of the trampoline allocator's alignment lambdas in
// vmhook.hpp (allocate_nearby_memory):
//
//     align_up(value, alignment)   = (value + alignment - 1) & ~(alignment - 1)
//     align_down(value, alignment) =  value                  & ~(alignment - 1)
//
// Both assume `alignment` is a power of two — the same assumption the library
// makes, because page_size() / allocation_granularity() always return one.  The
// lambdas are private to the function, so we reproduce the EXACT masks here; if
// the header ever changes the formula these mirrors must change in lock-step (a
// drift would surface as the cross-check against the divide/multiply reference
// below failing at compile time).
// ───────────────────────────────────────────────────────────────────────────
constexpr auto mirror_align_up(std::uintptr_t value, std::uintptr_t alignment) noexcept
    -> std::uintptr_t
{
    return (value + alignment - 1) & ~(alignment - 1);
}

constexpr auto mirror_align_down(std::uintptr_t value, std::uintptr_t alignment) noexcept
    -> std::uintptr_t
{
    return value & ~(alignment - 1);
}

// Independent reference using divide/multiply instead of bit masks.  For
// power-of-two alignments and inputs where `value + alignment - 1` does not
// overflow, this MUST agree with the masking form; cross-checking the two
// catches a wrong mask.  align_up here is only ever invoked on inputs the caller
// has proven non-overflowing.
constexpr auto ref_align_down(std::uintptr_t value, std::uintptr_t alignment) noexcept
    -> std::uintptr_t
{
    return (value / alignment) * alignment;
}

constexpr auto ref_align_up(std::uintptr_t value, std::uintptr_t alignment) noexcept
    -> std::uintptr_t
{
    return ((value + alignment - 1) / alignment) * alignment;
}

constexpr auto is_power_of_two(std::uintptr_t v) noexcept -> bool
{
    return v != 0 && (v & (v - 1)) == 0;
}

constexpr std::uintptr_t kUintMax{ ~static_cast<std::uintptr_t>(0) };

// ───────────────────────────────────────────────────────────────────────────
// COMPILE-TIME: every algebraic property, for ONE (alignment, value) pair, with
// the value guaranteed to be in the non-overflowing range (value + alignment-1
// does not wrap).  Returns true iff every property holds — folded into
// static_asserts by the sweep below so any violation is a hard build error that
// names the exact failing pair via the template arguments / call site.
// ───────────────────────────────────────────────────────────────────────────
constexpr auto check_pair_no_overflow(std::uintptr_t value, std::uintptr_t alignment) noexcept
    -> bool
{
    const std::uintptr_t d{ mirror_align_down(value, alignment) };
    const std::uintptr_t u{ mirror_align_up(value, alignment) };

    // Ordering: align_down(x) <= x <= align_up(x) and down <= up.
    if (!(d <= value)) { return false; }
    if (!(value <= u)) { return false; }
    if (!(d <= u)) { return false; }

    // Each result is exactly aligned (low bits clear).
    if ((d & (alignment - 1)) != 0) { return false; }
    if ((u & (alignment - 1)) != 0) { return false; }

    // Idempotence: re-aligning an aligned value is a fixed point.
    if (mirror_align_down(d, alignment) != d) { return false; }
    if (mirror_align_up(u, alignment) != u) { return false; }

    // The gap between up and down is 0 (already aligned) or one alignment unit.
    const std::uintptr_t gap{ u - d };
    if (!(gap == 0 || gap == alignment)) { return false; }

    // Agreement with the independent divide/multiply reference (safe: caller
    // guarantees no overflow for this value).
    if (d != ref_align_down(value, alignment)) { return false; }
    if (u != ref_align_up(value, alignment)) { return false; }

    // If x is already a multiple of alignment, both directions return x.
    if ((value % alignment) == 0)
    {
        if (d != value || u != value) { return false; }
    }
    return true;
}

// Drive check_pair_no_overflow over a dense input set for a single alignment.
// Every input is clamped to <= (kUintMax - alignment) so align_up never wraps
// here; the wrap boundary is asserted separately in check_overflow_boundary.
constexpr auto check_alignment_dense(std::uintptr_t alignment) noexcept -> bool
{
    const std::uintptr_t one_below_max{ kUintMax - alignment };
    const std::uintptr_t inputs[]{
        static_cast<std::uintptr_t>(0),
        static_cast<std::uintptr_t>(1),
        alignment - 1,
        alignment,
        alignment + 1,
        2u * alignment,
        2u * alignment + 1,
        3u * alignment - 1,
        4u * alignment,
        16u * alignment,
        static_cast<std::uintptr_t>(0x10000),                        // typical min app addr
        static_cast<std::uintptr_t>(vmhook::os::user_address_floor),  // 0xFFFF
        static_cast<std::uintptr_t>(vmhook::os::user_address_ceiling) & ~(alignment - 1),
        one_below_max & ~(alignment - 1),
        one_below_max,
    };
    for (const std::uintptr_t x : inputs)
    {
        if (!check_pair_no_overflow(x, alignment))
        {
            return false;
        }
    }
    return true;
}

// The documented overflow / wrap contract of the masking form near UINTPTR_MAX.
//   * align_down never wraps — it only clears low bits, so it stays <= value.
//   * align_up of the highest multiple is a fixed point.
//   * align_up of (highest multiple + 1) and of UINTPTR_MAX itself WRAP to 0
//     (this is exactly why allocate_nearby_memory clamps to search_max before
//     calling align_up — pin the wrap so that rationale stays true).
constexpr auto check_overflow_boundary(std::uintptr_t alignment) noexcept -> bool
{
    // align_down at the very top: aligned and never above the input.
    if (mirror_align_down(kUintMax, alignment) != (kUintMax & ~(alignment - 1))) { return false; }
    if ((mirror_align_down(kUintMax, alignment) & (alignment - 1)) != 0) { return false; }
    if (!(mirror_align_down(kUintMax, alignment) <= kUintMax)) { return false; }

    const std::uintptr_t top_aligned{ mirror_align_down(kUintMax, alignment) };

    // align_up of the highest representable multiple is a fixed point.
    if (mirror_align_up(top_aligned, alignment) != top_aligned) { return false; }

    if (alignment > 1u)
    {
        // One past the highest multiple wraps to 0 (masking behaviour).
        if (mirror_align_up(top_aligned + 1u, alignment) != 0u) { return false; }
        // UINTPTR_MAX has low bits set (alignment is a power of two > 1), so
        // align_up of it also wraps to 0.
        if (mirror_align_up(kUintMax, alignment) != 0u) { return false; }
    }
    else
    {
        // alignment == 1: every value is "aligned", up == down == value, no wrap.
        if (mirror_align_up(kUintMax, 1u) != kUintMax) { return false; }
        if (mirror_align_down(kUintMax, 1u) != kUintMax) { return false; }
    }
    return true;
}

// ───────────────────────────────────────────────────────────────────────────
// EXHAUSTIVE compile-time sweep over EVERY power-of-two alignment that fits a
// 64-bit uintptr_t.  `Shift` runs 0..63: alignment = 1u << Shift.  We stop the
// dense-input + reference cross-check at shift 31 (2^31) — beyond that the
// hand-built inputs like `16u * alignment` and `one_below_max & ~(alignment-1)`
// stop being interesting / start colliding with the wrap region — but we still
// assert the overflow boundary contract for the very large alignments
// (2^32 … 2^62) where the masking form is most likely to misbehave.  Every
// power is visited; nothing is skipped.
// ───────────────────────────────────────────────────────────────────────────
template <unsigned Shift>
consteval auto sweep_all_alignments() -> bool
{
    constexpr std::uintptr_t alignment{ static_cast<std::uintptr_t>(1) << Shift };
    static_assert(is_power_of_two(alignment), "alignment under test must be a power of two");

    if constexpr (Shift <= 31u)
    {
        static_assert(check_alignment_dense(alignment),
                      "dense alignment sweep failed for some input at this power-of-two alignment");
    }
    // The wrap/overflow boundary is meaningful (and most fragile) for the large
    // alignments too; assert it across the whole range up to 2^62.  Skip the top
    // bit (2^63) where `top_aligned + 1u` and the reference math get degenerate.
    if constexpr (Shift <= 62u)
    {
        static_assert(check_overflow_boundary(alignment),
                      "align_up/align_down overflow-boundary contract failed at this alignment");
    }

    if constexpr (Shift < 62u)
    {
        return sweep_all_alignments<Shift + 1u>();
    }
    else
    {
        return true;
    }
}

// Kick off the recursive sweep at compile time.  The static_asserts inside fire
// during this instantiation; the bool result is a belt-and-suspenders extra.
static_assert(sweep_all_alignments<0u>(),
              "exhaustive power-of-two alignment sweep failed");

// ───────────────────────────────────────────────────────────────────────────
// COMPILE-TIME: hand-computed spot values for the two real-world alignments
// this library actually uses (4096 page, 65536 Windows granularity), so a
// silently-wrong mask is caught against literal expectations, not just against
// the algebraic reference.
// ───────────────────────────────────────────────────────────────────────────
static_assert(mirror_align_down(0u, 4096u) == 0u);
static_assert(mirror_align_down(1u, 4096u) == 0u);
static_assert(mirror_align_down(4095u, 4096u) == 0u);
static_assert(mirror_align_down(4096u, 4096u) == 4096u);
static_assert(mirror_align_down(4097u, 4096u) == 4096u);
static_assert(mirror_align_down(8191u, 4096u) == 4096u);
static_assert(mirror_align_down(8192u, 4096u) == 8192u);
static_assert(mirror_align_up(0u, 4096u) == 0u);
static_assert(mirror_align_up(1u, 4096u) == 4096u);
static_assert(mirror_align_up(4095u, 4096u) == 4096u);
static_assert(mirror_align_up(4096u, 4096u) == 4096u);
static_assert(mirror_align_up(4097u, 4096u) == 8192u);
static_assert(mirror_align_up(8192u, 4096u) == 8192u);

static_assert(mirror_align_down(65535u, 65536u) == 0u);
static_assert(mirror_align_down(65536u, 65536u) == 65536u);
static_assert(mirror_align_up(1u, 65536u) == 65536u);
static_assert(mirror_align_up(65537u, 65536u) == 131072u);
static_assert(mirror_align_up(4096u, 65536u) == 65536u);   // page rounds up to granularity
static_assert(mirror_align_down(4096u, 65536u) == 0u);

// The library's allocate_nearby_memory pairs the two alignments: a page-aligned
// address is NOT necessarily granularity-aligned, but a granularity-aligned
// address IS always page-aligned (granularity is a multiple of page).  Pin both
// directions at compile time for the canonical Windows pair.
static_assert((65536u % 4096u) == 0u, "granularity must be a multiple of page (Win32 pair)");
static_assert(mirror_align_down(mirror_align_down(0x12345u, 65536u), 4096u)
                  == mirror_align_down(0x12345u, 65536u),
              "a granularity-aligned address is already page-aligned");

// ───────────────────────────────────────────────────────────────────────────
// RUNTIME harness for the OS-queried values.  Tiny and deterministic.
// ───────────────────────────────────────────────────────────────────────────
int failures{ 0 };

auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok)
    {
        ++failures;
    }
}

auto is_pow2_rt(std::size_t v) noexcept -> bool
{
    return v != 0 && (v & (v - 1)) == 0;
}

// ───────────────────────────────────────────────────────────────────────────
// page_size() / allocation_granularity(): the full invariant set, sanity bounds,
// the named relationship, and stability across both repeated calls and threads
// (these are process-global, unlike current_thread_id).
// ───────────────────────────────────────────────────────────────────────────
auto test_queried_values_are_sane() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    const std::size_t gran{ vmhook::os::allocation_granularity() };

    // page_size sanity.
    check("page_size_nonzero", page != 0);
    check("page_size_power_of_two", is_pow2_rt(page));
    check("page_size_at_least_4096", page >= 4096u);
    check("page_size_at_most_2MiB", page <= (2u * 1024u * 1024u));

    // allocation_granularity sanity.
    check("granularity_nonzero", gran != 0);
    check("granularity_power_of_two", is_pow2_rt(gran));
    check("granularity_at_least_4096", gran >= 4096u);
    check("granularity_at_most_64MiB", gran <= (64u * 1024u * 1024u));

    // The relationship this feature is named for.
    check("granularity_at_least_page", gran >= page);
    check("granularity_multiple_of_page", (gran % page) == 0);
    // gran and page both powers of two with gran >= page => gran/page is itself
    // a power of two; verify that derived fact directly.
    check("granularity_over_page_is_power_of_two", is_pow2_rt(gran / page));

    // Stability across repeated calls (no caching in the lib, but the values are
    // process-invariant, so two reads must agree).
    check("page_size_stable_across_calls",
          vmhook::os::page_size() == page && vmhook::os::page_size() == page);
    check("granularity_stable_across_calls",
          vmhook::os::allocation_granularity() == gran
              && vmhook::os::allocation_granularity() == gran);
}

auto test_queried_values_thread_stable() -> void
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

// ───────────────────────────────────────────────────────────────────────────
// Feed the LIVE, runtime-queried page and granularity values straight into the
// mirrored alignment helpers.  The compile-time sweep proves the arithmetic for
// every power of two in the abstract; this proves it for whatever this host's
// actual values are, closing the loop between "the formula is correct" and "the
// formula is correct for the numbers the library will really pass it".
// ───────────────────────────────────────────────────────────────────────────
auto test_alignment_on_live_values() -> void
{
    const std::uintptr_t page{ static_cast<std::uintptr_t>(vmhook::os::page_size()) };
    const std::uintptr_t gran{ static_cast<std::uintptr_t>(vmhook::os::allocation_granularity()) };

    check("live_page_dense_sweep", check_alignment_dense(page));
    check("live_granularity_dense_sweep", check_alignment_dense(gran));
    check("live_page_overflow_boundary", check_overflow_boundary(page));
    check("live_granularity_overflow_boundary", check_overflow_boundary(gran));

    // The allocate_nearby_memory invariant, on the real values: a
    // granularity-aligned address is always page-aligned.
    const std::uintptr_t probe{ 0xABCDEF0123ull & vmhook::os::user_address_ceiling };
    const std::uintptr_t g_aligned{ mirror_align_down(probe, gran) };
    check("live_granularity_aligned_is_page_aligned",
          mirror_align_down(g_aligned, page) == g_aligned);
    check("live_granularity_aligned_has_no_page_low_bits",
          (g_aligned & (page - 1)) == 0);
}

// ───────────────────────────────────────────────────────────────────────────
// Reproduce the POSIX protect() page-rounding arithmetic as pure runtime math
// against the real page size and assert it yields a page-aligned base covering
// the whole requested range.  (vmhook.hpp protect(): base &= ~(ps-1);
// size = (end - base + ps - 1) & ~(ps-1).)  This is the math that lets protect()
// accept unaligned interior addresses; verified here as arithmetic, the live
// mprotect call is exercised in test_os_protect_interaction.cpp.
// ───────────────────────────────────────────────────────────────────────────
auto test_protect_page_rounding_formula() -> void
{
    const std::uintptr_t ps{ static_cast<std::uintptr_t>(vmhook::os::page_size()) };

    struct Case { std::uintptr_t off; std::uintptr_t len; };
    const std::uintptr_t origin{ 4u * ps };               // 4 pages up: room to subtract
    const Case cases[]{
        { 0u,          1u },        // aligned base, 1 byte
        { 0u,          ps },        // aligned base, exactly one page
        { 0u,          ps + 1u },   // aligned base, just over one page
        { 1u,          1u },        // unaligned base, 1 byte
        { ps - 1u,     1u },        // last byte of a page
        { ps - 1u,     2u },        // straddles the page boundary
        { ps / 2u,     ps },        // unaligned base, page length -> 2 pages
        { ps - 4u,     8u },        // crossing range like the boundary test
        { 3u,          3u * ps },   // unaligned base spanning many pages
    };

    bool base_aligned{ true };
    bool base_le_addr{ true };
    bool covers_end{ true };
    bool size_page_multiple{ true };
    bool size_nonzero{ true };

    for (const Case& c : cases)
    {
        const std::uintptr_t addr{ origin + c.off };
        const std::uintptr_t end{ addr + c.len };
        const std::uintptr_t base{ addr & ~(ps - 1) };
        const std::uintptr_t aligned_size{ (end - base + ps - 1) & ~(ps - 1) };

        if ((base & (ps - 1)) != 0) { base_aligned = false; }
        if (!(base <= addr)) { base_le_addr = false; }
        if (!(base + aligned_size >= end)) { covers_end = false; }
        if ((aligned_size % ps) != 0) { size_page_multiple = false; }
        if (aligned_size == 0) { size_nonzero = false; }
    }

    check("protect_rounding_base_is_page_aligned", base_aligned);
    check("protect_rounding_base_le_request", base_le_addr);
    check("protect_rounding_covers_full_range", covers_end);
    check("protect_rounding_size_is_page_multiple", size_page_multiple);
    check("protect_rounding_size_nonzero", size_nonzero);
}

// ───────────────────────────────────────────────────────────────────────────
// Concrete, per-OS values.  page_size DIFFERS by platform, so each literal is
// gated behind the OS macro; the cross-platform invariants above already cover
// the unguarded contract.
// ───────────────────────────────────────────────────────────────────────────
auto test_platform_specific_values() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    const std::size_t gran{ vmhook::os::allocation_granularity() };

#if VMHOOK_OS_WINDOWS
    // Every shipping Win32 build: 4 KiB page, 64 KiB allocation granularity.
    check("windows_page_size_is_4096", page == 4096u);
    check("windows_granularity_is_65536", gran == 65536u);
#else
    // Every POSIX target: the library defines allocation_granularity() to return
    // page_size() — the identity contract.  Pin it.
    check("posix_granularity_equals_page_size", gran == page);
    (void)gran;
#endif
}

} // namespace

int main()
{
    test_queried_values_are_sane();
    test_queried_values_thread_stable();
    test_alignment_on_live_values();
    test_protect_page_rounding_formula();
    test_platform_specific_values();

    if (failures == 0)
    {
        std::printf("vmhook os page_size/granularity + alignment: OK\n");
    }
    else
    {
        std::printf("vmhook os page_size/granularity + alignment: %d FAILURE(S)\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
