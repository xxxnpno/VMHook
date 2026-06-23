// Exhaustive (no-JVM) unit tests for the cross-platform fault-tolerant memory
// read primitives:
//   * vmhook::os::safe_read(dst, src, size)       — the load-bearing primitive
//   * vmhook::os::safe_read_fast(dst, src, size)  — the cheap-path twin
//   * vmhook::hotspot::is_readable_pointer(p)      — the region-based gate
//
// safe_read is the thing every HotSpot introspection helper dereferences an
// UNTRUSTED JVM pointer through; the whole "crash-proof" promise of the library
// rests on it returning `false` instead of faulting when the pointer is garbage,
// freed, or sits on a no-access page.  This file drives that primitive from every
// input/edge that is *safely* testable in-process, against REAL local memory and
// REAL OS calls — no JVM, no oop, no GC, no fixtures.
//
// WHY A DEDICATED FILE
// --------------------
// The existing OS tests already cover adjacent ground:
//   * tests/test_os_protect_interaction.cpp        — safe_read of ONE byte fully
//     inside a no_access page + the null/zero input-guard matrix.
//   * tests/test_os_layer.cpp                       — safe_read of a 2-byte valid
//     block + one bogus high-canonical pointer.
//   * tests/test_field_proxy_value_conversions.cpp  — safe_read_fast null guards,
//     byte-identical-to-safe_read across 1/2/4/8, and unmapped-read recovery.
//   * tests/test_const_method_bounds.cpp            — is_readable_pointer deeply
//     (null/floor/ceiling/poison sentinels, mapped slots, no_access, guard page).
// This file fills the gaps NONE of those exercise for the *primary* safe_read:
//   (1) a POSITIVE round-trip across many widths (0,1,2,4,7,8,9,16,page-1,page,
//       page+1, multi-page) asserting EXACT bytes copied;
//   (2) the page-BOUNDARY trio — straddle readable->no_access (false), read
//       ending exactly at the boundary (true), read starting on the no_access
//       page (false) — the off-by-one cases the 1-byte test cannot reach;
//   (3) freed/unmapped memory and the canonical known-bad sentinels (0x1, the
//       user_address_floor, 0xDEADBEEF, a non-canonical high address) returning
//       false with NO fault — the core contract;
//   (4) safe_read vs safe_read_fast byte-identical at the OS-test level;
//   (5) overlapping src/dst defined behaviour;
//   (6) CONCURRENT probes on N threads (the thread_local fault-recovery state);
//   (7) repeated fallback-path entry leaving the process healthy;
//   (8) a huge / address-space-wrapping size rejected promptly with no hang.
//
// PORTABILITY: every assertion is a cross-platform INVARIANT — the FACT of
// success/failure plus the bytes copied — never a specific address or page size.
// page_size() is queried at run time (no hard-coded 4096; Apple silicon is 16K).
// No <charconv>/float, no std::expected/syncstream, so the file builds under the
// MinGW libstdc++, MSVC STL, and libc++ CI toolchains.
//
// SAFETY: every no-access / unmapped probe goes ONLY through safe_read /
// safe_read_fast (kernel-validated on Windows/macOS/Linux, sigsetjmp-guarded on
// the Linux fallback), is gated behind `#if !VMHOOK_OS_IOS` (iOS has no
// fault-safe read API — there safe_read is a raw memcpy that WOULD fault), and
// the no_access cases additionally SKIP (print [INFO], no failure) when a
// sandboxed runner refuses PROT_NONE.  Every page we protect is one we allocated
// ourselves and we always restore a writable mapping before release().
#include <vmhook/vmhook.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

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
// Restore a writable mapping for teardown.  execute_rw is the state a page
// returns from allocate_rwx with; Apple arm64 / current iOS enforce W^X and
// refuse PROT_WRITE|PROT_EXEC without the JIT entitlement, so fall back to plain
// read_write (which is what allocate_rwx itself falls back to there).  Mirrors
// the helper in test_os_protect_interaction.cpp so teardown is identical.
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

// Deterministic fill so byte-exact comparisons have a stable reference.
static auto fill_pattern(std::uint8_t* p, std::size_t n) -> void
{
    for (std::size_t i{ 0 }; i < n; ++i)
    {
        p[i] = static_cast<std::uint8_t>((i * 31u + 7u) & 0xFFu);
    }
}

// ---------------------------------------------------------------------------
// 1) POSITIVE round-trip: a GOOD read must copy EXACTLY the requested bytes for
//    every width a caller might use, and return true.  The existing OS tests
//    only ever assert the *negative* (no_access / bogus) case plus the input
//    guards and a single 2-byte positive read; this pins that the happy path
//    actually transfers byte-for-byte at 0,1,2,4,7,8,9,16, page-1, page, page+1,
//    and a multi-page span.  size==0 is the one width that must return FALSE.
// ---------------------------------------------------------------------------
static auto test_safe_read_positive_roundtrip_all_widths() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    // Two pages so page+1 / 2*page stay inside our own allocation.
    void* const block{ vmhook::os::allocate_rwx(nullptr, page * 2) };
    if (!block)
    {
        check("safe_read_roundtrip_skipped_alloc_failed", false);
        return;
    }

    auto* const src{ static_cast<std::uint8_t*>(block) };
    fill_pattern(src, page * 2);

    const std::size_t widths[]{
        std::size_t{ 0 },  // must return false (the only width that does)
        std::size_t{ 1 },
        std::size_t{ 2 },
        std::size_t{ 4 },
        std::size_t{ 7 },
        std::size_t{ 8 },
        std::size_t{ 9 },
        std::size_t{ 16 },
        page - 1,
        page,
        page + 1,
        page * 2,
    };

    std::vector<std::uint8_t> dst(page * 2 + 16, 0u);
    bool all_ok{ true };
    bool all_bytes_match{ true };
    bool zero_size_false{ true };

    for (const std::size_t w : widths)
    {
        // Poison the destination so a partial/garbage copy is visible.
        std::memset(dst.data(), 0xCC, dst.size());

        const bool ok{ vmhook::os::safe_read(dst.data(), src, w) };

        if (w == 0)
        {
            // size==0 is the documented input-guard failure: false, no copy.
            if (ok)
            {
                zero_size_false = false;
            }
            continue;
        }

        if (!ok)
        {
            all_ok = false;
        }
        // Exactly w bytes must equal the source; verified against an independent
        // memcmp so a wrong length or torn copy is caught.
        if (std::memcmp(dst.data(), src, w) != 0)
        {
            all_bytes_match = false;
        }
        // And the byte just past the copied range must NOT have been touched
        // (still the 0xCC poison) — proves safe_read copies w bytes, not w+1.
        if (dst[w] != 0xCC)
        {
            all_bytes_match = false;
        }
    }

    check("safe_read_zero_size_returns_false", zero_size_false);
    check("safe_read_all_widths_succeed", all_ok);
    check("safe_read_all_widths_copy_exact_bytes", all_bytes_match);

    check("safe_read_roundtrip_restore_writable", make_writable(block, page * 2));
    vmhook::os::release(block, page * 2);
}

// ---------------------------------------------------------------------------
// 2) safe_read vs safe_read_fast must be BYTE-IDENTICAL on valid reads — same
//    bool, same bytes — across the same width sweep.  test_field_proxy already
//    checks this for 1/2/4/8; here we additionally cover 7/9/16/page-sized so the
//    drop-in-replacement contract holds at non-power-of-two and page-scale reads.
// ---------------------------------------------------------------------------
static auto test_safe_read_fast_byte_identical_to_safe_read() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
    if (!block)
    {
        check("safe_read_fast_parity_skipped_alloc_failed", false);
        return;
    }

    auto* const src{ static_cast<std::uint8_t*>(block) };
    fill_pattern(src, page);

    const std::size_t widths[]{
        std::size_t{ 1 }, std::size_t{ 2 }, std::size_t{ 3 }, std::size_t{ 4 },
        std::size_t{ 7 }, std::size_t{ 8 }, std::size_t{ 9 }, std::size_t{ 16 },
        std::size_t{ 64 }, page - 1, page,
    };

    bool all_bool_match{ true };
    bool all_bytes_match{ true };
    bool all_fast_correct{ true };

    std::vector<std::uint8_t> via_slow(page + 16, 0u);
    std::vector<std::uint8_t> via_fast(page + 16, 0u);

    for (const std::size_t w : widths)
    {
        std::memset(via_slow.data(), 0xA1, via_slow.size());
        std::memset(via_fast.data(), 0xB2, via_fast.size());

        const bool ok_slow{ vmhook::os::safe_read(via_slow.data(), src, w) };
        const bool ok_fast{ vmhook::os::safe_read_fast(via_fast.data(), src, w) };

        if (ok_slow != ok_fast)
        {
            all_bool_match = false;
        }
        if (!ok_fast)
        {
            all_fast_correct = false;
        }
        // The two paths must produce identical bytes, and both must equal source.
        if (std::memcmp(via_slow.data(), via_fast.data(), w) != 0)
        {
            all_bytes_match = false;
        }
        if (std::memcmp(via_fast.data(), src, w) != 0)
        {
            all_fast_correct = false;
        }
    }

    check("safe_read_fast_bool_matches_safe_read", all_bool_match);
    check("safe_read_fast_bytes_match_safe_read", all_bytes_match);
    check("safe_read_fast_copies_exact_bytes", all_fast_correct);

    check("safe_read_fast_parity_restore_writable", make_writable(block, page));
    vmhook::os::release(block, page);
}

#if !VMHOOK_OS_IOS
// ---------------------------------------------------------------------------
// 3) The page-BOUNDARY trio.  Allocate two adjacent pages, make page 0 RWX and
//    page 1 no_access, then probe three precise spans:
//      (a) [page-8, page)   — last 8 bytes of the readable page, NONE of the
//          no_access page: MUST return true and copy exactly.  Proves safe_read
//          does not over-read by even one byte into the next page.
//      (b) [page-4, page+4) — straddles the boundary: MUST return false (the
//          read crosses into the unreadable page).  Per the all-or-nothing
//          contract, dst is unspecified on a false return; we only assert the
//          bool and that no fault occurred.
//      (c) [page, page+1) and [page, page+8) — start exactly on the no_access
//          page: MUST return false.
//    Gated off iOS and SKIPPED when PROT_NONE is refused.
// ---------------------------------------------------------------------------
static auto test_safe_read_page_boundary_trio() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    void* const block{ vmhook::os::allocate_rwx(nullptr, page * 2) };
    if (!block)
    {
        check("safe_read_boundary_skipped_alloc_failed", false);
        return;
    }

    auto* const base{ static_cast<std::uint8_t*>(block) };
    // Distinct markers either side of the boundary before we lock page 1.
    fill_pattern(base, page * 2);

    // Lock page 1 to no_access; page 0 stays readable (RWX from allocate_rwx).
    const bool locked{ vmhook::os::protect(base + page, page,
                                           vmhook::os::memory_protection::no_access,
                                           nullptr) };
    if (!locked)
    {
        std::printf("[INFO] safe_read_boundary skipped: protect(no_access) refused\n");
        (void)make_writable(block, page * 2);
        vmhook::os::release(block, page * 2);
        return;
    }

    // (a) Read entirely within the readable page, ending EXACTLY at the boundary.
    {
        std::uint8_t dst[8];
        std::memset(dst, 0xCC, sizeof(dst));
        const bool ok{ vmhook::os::safe_read(dst, base + page - 8, 8u) };
        check("safe_read_ends_at_boundary_succeeds", ok);
        check("safe_read_ends_at_boundary_exact_bytes",
              ok && std::memcmp(dst, base + page - 8, 8u) == 0);
    }

    // (b) Straddle the boundary: readable prefix + unreadable tail -> false.
    {
        std::uint8_t dst[8];
        std::memset(dst, 0xCC, sizeof(dst));
        const bool ok{ vmhook::os::safe_read(dst, base + page - 4, 8u) };
        // The read cannot be fully satisfied; all-or-nothing -> false, no fault.
        check("safe_read_straddle_into_no_access_returns_false", !ok);
    }

    // (c) Start exactly on the no_access page -> false, at two widths.
    {
        std::uint8_t dst[8];
        std::memset(dst, 0xCC, sizeof(dst));
        const bool one{ vmhook::os::safe_read(dst, base + page, 1u) };
        const bool eight{ vmhook::os::safe_read(dst, base + page, 8u) };
        check("safe_read_starts_on_no_access_size1_returns_false", !one);
        check("safe_read_starts_on_no_access_size8_returns_false", !eight);
    }

    // Restore both pages writable before unmap.
    check("safe_read_boundary_restore_writable", make_writable(block, page * 2));
    vmhook::os::release(block, page * 2);
}

// ---------------------------------------------------------------------------
// 4) safe_read of a SINGLE no_access page across widths.  The canonical
//    interaction test reads 1 byte; here we additionally read 8 and a
//    page-sized span fully inside the locked page — every width must return
//    false, never fault.  Confirms the all-or-nothing refusal is width-agnostic.
// ---------------------------------------------------------------------------
static auto test_safe_read_no_access_page_all_widths() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
    if (!block)
    {
        check("safe_read_no_access_widths_skipped_alloc_failed", false);
        return;
    }

    auto* const bytes{ static_cast<std::uint8_t*>(block) };
    bytes[0] = 0xEE;

    const bool locked{ vmhook::os::protect(block, page,
                                           vmhook::os::memory_protection::no_access,
                                           nullptr) };
    if (!locked)
    {
        std::printf("[INFO] safe_read_no_access_widths skipped: PROT_NONE refused\n");
        vmhook::os::release(block, page);
        return;
    }

    const std::size_t widths[]{
        std::size_t{ 1 }, std::size_t{ 2 }, std::size_t{ 4 }, std::size_t{ 8 },
        std::size_t{ 64 }, page,
    };
    std::vector<std::uint8_t> dst(page, 0u);
    bool all_false{ true };
    for (const std::size_t w : widths)
    {
        if (vmhook::os::safe_read(dst.data(), block, w))
        {
            all_false = false;
        }
    }
    check("safe_read_no_access_page_all_widths_false", all_false);

    (void)vmhook::os::protect(block, page,
                              vmhook::os::memory_protection::read_write, nullptr);
    vmhook::os::release(block, page);
}

// ---------------------------------------------------------------------------
// 5) safe_read of FREED / unmapped memory must return false, not crash.  We
//    allocate a page, write to it, RELEASE it, then attempt to read from the
//    now-unmapped address.  Mirrors test_query_region_reports_free_for_unallocated
//    for the read path.  (Some OSes may immediately remap the address; safe_read
//    must still not fault.  We assert no crash; the read of freed memory is
//    overwhelmingly false, but a kernel that re-mapped the page could make it
//    true — so the load-bearing assertion is "process survives", encoded by
//    reaching the next line.)
// ---------------------------------------------------------------------------
static auto test_safe_read_freed_memory_no_crash() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
    if (!block)
    {
        check("safe_read_freed_skipped_alloc_failed", false);
        return;
    }
    *static_cast<volatile std::uint8_t*>(block) = 0x5A;
    vmhook::os::release(block, page);

    // Reading the freed region must NOT fault.  We capture the bool but only
    // assert the no-crash invariant strongly; the typical result is false.
    std::uint8_t dst[8]{};
    const bool ok{ vmhook::os::safe_read(dst, block, sizeof(dst)) };
    check("safe_read_freed_memory_no_crash", true);
    // On every mainstream OS the just-released reservation is gone, so the read
    // fails.  Accept either outcome but record the expected one for visibility.
    if (ok)
    {
        std::printf("[INFO] safe_read_freed_memory: kernel re-mapped the address (read succeeded)\n");
    }
    else
    {
        check("safe_read_freed_memory_returns_false", true);
    }
}

// ---------------------------------------------------------------------------
// 6) The canonical KNOWN-BAD sentinels must each return false with NO fault.
//    These bypass safe_read_pointer's pre-filters and hit safe_read DIRECTLY,
//    so this exercises the kernel/signal path, not the address filter:
//      0x1                       — minimal non-null garbage
//      user_address_floor 0xFFFF — the floor sentinel HotSpot uses for list ends
//      0xDEADBEEF                — classic debug poison
//      0xCAFEBABE                — Java magic / sentinel
//      0xFFFF'8000'0000'0000     — non-canonical / kernel-space high address
//      ~0 (all ones)             — top of the address space
//    Gated off iOS (raw memcpy would fault there).
// ---------------------------------------------------------------------------
static auto test_safe_read_known_bad_pointers_no_fault() -> void
{
    struct Bad { const char* tag; std::uintptr_t value; };
    const Bad bad[]{
        { "one",            std::uintptr_t{ 0x1 } },
        { "floor_0xFFFF",   vmhook::os::user_address_floor },
        { "deadbeef",       std::uintptr_t{ 0xDEADBEEFull } },
        { "cafebabe",       std::uintptr_t{ 0xCAFEBABEull } },
        { "noncanonical",   std::uintptr_t{ 0xFFFF800000000000ull } },
        { "all_ones",       ~std::uintptr_t{ 0 } },
    };

    bool all_false{ true };
    for (const Bad& b : bad)
    {
        std::uint8_t dst[8]{};
        const void* const src{ reinterpret_cast<const void*>(b.value) };
        // Read 1 and 8 bytes from each; both must be refused with no fault.
        if (vmhook::os::safe_read(dst, src, 1u))
        {
            all_false = false;
        }
        if (vmhook::os::safe_read(dst, src, sizeof(dst)))
        {
            all_false = false;
        }
    }
    check("safe_read_known_bad_pointers_all_false_no_fault", all_false);

    // Reaching here at all proves none of the reads faulted the process.
    check("safe_read_known_bad_pointers_process_survives", true);
}
#endif // !VMHOOK_OS_IOS

// ---------------------------------------------------------------------------
// 7) Overlapping / aliasing src and dst.  safe_read has no overlap guard; the
//    kernel paths (ReadProcessMemory / process_vm_readv / mach_vm_read_overwrite)
//    tolerate overlap, and a forward memcpy on the Linux fallback is also safe
//    for the dst-after-src / identical-region cases.  We pin that:
//      (a) dst == src (full self-read) succeeds and leaves the bytes unchanged;
//      (b) dst just AFTER src by the full width (non-overlapping but adjacent)
//          succeeds and copies exactly.
//    This documents that a self/adjacent read is a DEFINED, non-corrupting usage
//    (the only overlap we ever rely on), without asserting the UB-prone
//    dst-inside-src forward-overlap case.
// ---------------------------------------------------------------------------
static auto test_safe_read_overlapping_regions_defined() -> void
{
    // A single owned buffer; we read parts of it into other parts.
    alignas(std::uint64_t) std::uint8_t buf[64];
    fill_pattern(buf, sizeof(buf));

    // (a) Full self-read: dst == src.  Must succeed and not change the bytes.
    {
        std::uint8_t reference[16];
        std::memcpy(reference, buf, sizeof(reference));
        const bool ok{ vmhook::os::safe_read(buf, buf, sizeof(reference)) };
        check("safe_read_self_read_succeeds", ok);
        check("safe_read_self_read_bytes_unchanged",
              std::memcmp(buf, reference, sizeof(reference)) == 0);
    }

    // (b) Adjacent, non-overlapping: copy buf[0..7] into buf[8..15].
    {
        std::uint8_t reference[8];
        std::memcpy(reference, buf, sizeof(reference));   // expected dst content
        const bool ok{ vmhook::os::safe_read(buf + 8, buf, 8u) };
        check("safe_read_adjacent_copy_succeeds", ok);
        check("safe_read_adjacent_copy_exact_bytes",
              std::memcmp(buf + 8, reference, 8u) == 0);
    }
}

#if !VMHOOK_OS_IOS
// ---------------------------------------------------------------------------
// 8) CONCURRENT probes on multiple threads.  Each thread hammers safe_read
//    against a shared no_access page in a tight loop; every call must return
//    false and NO thread may fault.  This exercises the thread_local
//    fault-recovery state (Linux/Android `active_state`) — the property that two
//    threads probing simultaneously do not clobber each other's sigjmp_buf.  A
//    regression making that state process-global (not thread_local) would crash
//    or wedge here.  On Windows/macOS the kernel read is inherently thread-safe;
//    the test still verifies concurrent correctness there.  Gated off iOS, and
//    SKIPPED when PROT_NONE is refused (we cannot create the no_access page).
// ---------------------------------------------------------------------------
static auto test_safe_read_concurrent_probes() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
    if (!block)
    {
        check("safe_read_concurrent_skipped_alloc_failed", false);
        return;
    }
    *static_cast<volatile std::uint8_t*>(block) = 0x33;

    const bool locked{ vmhook::os::protect(block, page,
                                           vmhook::os::memory_protection::no_access,
                                           nullptr) };
    if (!locked)
    {
        std::printf("[INFO] safe_read_concurrent skipped: PROT_NONE refused\n");
        vmhook::os::release(block, page);
        return;
    }

    constexpr int thread_count{ 8 };
    constexpr int iterations{ 2000 };
    std::atomic<int> unexpected_true{ 0 };

    // Also give each thread a small VALID buffer it reads in the same loop, so
    // the probe path and the success path are interleaved under contention — the
    // realistic pattern where some reads land on live memory and some don't.
    std::vector<std::thread> threads;
    threads.reserve(thread_count);
    for (int t{ 0 }; t < thread_count; ++t)
    {
        threads.emplace_back([&, t]() {
            alignas(std::uint64_t) std::uint8_t local_src[16];
            for (std::size_t i{ 0 }; i < sizeof(local_src); ++i)
            {
                local_src[i] = static_cast<std::uint8_t>(t * 16 + i);
            }
            std::uint8_t dst[16];
            for (int i{ 0 }; i < iterations; ++i)
            {
                // Bad read: must be false.
                if (vmhook::os::safe_read(dst, block, 8u))
                {
                    unexpected_true.fetch_add(1, std::memory_order_relaxed);
                }
                // Good read on this thread's own buffer: must be true + exact.
                if (!vmhook::os::safe_read(dst, local_src, sizeof(local_src))
                    || std::memcmp(dst, local_src, sizeof(local_src)) != 0)
                {
                    unexpected_true.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& th : threads)
    {
        th.join();
    }

    check("safe_read_concurrent_no_access_all_false_and_valid_ok",
          unexpected_true.load() == 0);
    // Reaching here proves no thread faulted during ~16k interleaved probes.
    check("safe_read_concurrent_process_survives", true);

    (void)vmhook::os::protect(block, page,
                              vmhook::os::memory_protection::read_write, nullptr);
    vmhook::os::release(block, page);
}

// ---------------------------------------------------------------------------
// 9) Repeated fallback-path entry leaves host signal handling HEALTHY.  Force
//    hundreds of failing reads (which on Linux/Android drive the sigsetjmp
//    signal fallback after process_vm_readv refuses, and on Windows/macOS just
//    hammer the kernel read), then prove a deliberate VALID read STILL succeeds
//    and a normal in-process store STILL works.  This is the closest pure-logic
//    proxy that the handler self-disarm + re-arm (the `static` install) did not
//    wedge SIGSEGV/SIGBUS for the process.  Gated off iOS; the no_access source
//    is SKIPPED (degrading to an unmapped-sentinel source) if PROT_NONE refused.
// ---------------------------------------------------------------------------
static auto test_safe_read_repeated_fallback_keeps_process_healthy() -> void
{
    const std::size_t page{ vmhook::os::page_size() };

    // Prefer a real no_access page (drives the strongest fault path); fall back
    // to an unmapped sentinel address if the sandbox forbids PROT_NONE.
    void* fault_src{ nullptr };
    void* owned{ vmhook::os::allocate_rwx(nullptr, page) };
    bool owned_locked{ false };
    if (owned)
    {
        *static_cast<volatile std::uint8_t*>(owned) = 0x10;
        owned_locked = vmhook::os::protect(owned, page,
                                           vmhook::os::memory_protection::no_access,
                                           nullptr);
        if (owned_locked)
        {
            fault_src = owned;
        }
    }
    void* const unmapped_sentinel{ reinterpret_cast<void*>(
        static_cast<std::uintptr_t>(0xDEAD0000ull)) };
    if (!fault_src)
    {
        fault_src = unmapped_sentinel;
    }

    // Hundreds of faulting reads.
    bool all_false{ true };
    for (int i{ 0 }; i < 512; ++i)
    {
        std::uint8_t dst[8]{};
        if (vmhook::os::safe_read(dst, fault_src, sizeof(dst)))
        {
            all_false = false;
        }
    }
    check("safe_read_repeated_faults_all_false", all_false);

    // A VALID read must still work after all those faults — proves the fault
    // machinery re-armed cleanly and did not leave the handler disarmed/wedged.
    {
        alignas(std::uint64_t) const std::uint8_t good[16]{
            0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
            0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00 };
        std::uint8_t dst[16]{};
        const bool ok{ vmhook::os::safe_read(dst, good, sizeof(good)) };
        check("safe_read_valid_after_many_faults_succeeds", ok);
        check("safe_read_valid_after_many_faults_exact_bytes",
              ok && std::memcmp(dst, good, sizeof(good)) == 0);
    }

    // A normal in-process operation (a plain store + read) must still work — if
    // the process-wide handler had been wedged this would be where it shows.
    {
        volatile std::uint64_t probe{ 0 };
        probe = 0xA5A5A5A5A5A5A5A5ull;
        check("safe_read_inprocess_store_still_works",
              probe == 0xA5A5A5A5A5A5A5A5ull);
    }

    if (owned)
    {
        if (owned_locked)
        {
            (void)vmhook::os::protect(owned, page,
                                      vmhook::os::memory_protection::read_write, nullptr);
        }
        vmhook::os::release(owned, page);
    }
}
#endif // !VMHOOK_OS_IOS

// ---------------------------------------------------------------------------
// 10) Huge / address-space-wrapping size must be rejected promptly with no hang
//     and no crash.  safe_read does not bound `size`; on the kernel paths
//     (Windows/macOS/Linux process_vm_readv) the kernel validates the range and
//     fails, and on the Linux sigsetjmp fallback the eventual fault is caught.
//     We hand safe_read a SMALL valid buffer as src plus (a) SIZE_MAX and (b) a
//     size precisely tuned so src+size wraps the address space.  The contract we
//     pin is "returns false, does not hang, does not crash" — the call returns
//     here at all, and the bool is false.
//
//     SAFETY: src points at a tiny owned stack buffer; the kernel read of a wild
//     length fails fast on a region-size check (ReadProcessMemory /
//     process_vm_readv reject the cross-region span almost immediately), so this
//     does not actually scan terabytes.
// ---------------------------------------------------------------------------
static auto test_safe_read_huge_size_rejected() -> void
{
    alignas(std::uint64_t) std::uint8_t small_src[64];
    fill_pattern(small_src, sizeof(small_src));
    std::uint8_t dst[8]{};

    // (a) SIZE_MAX from a valid base: the range vastly exceeds any mapping.
    const bool size_max_ok{ vmhook::os::safe_read(dst, small_src, SIZE_MAX) };
    check("safe_read_size_max_returns_false", !size_max_ok);

    // (b) A size tuned so base + size wraps past the top of the address space.
    {
        const std::uintptr_t base_addr{ reinterpret_cast<std::uintptr_t>(small_src) };
        const std::uintptr_t max_addr{ ~static_cast<std::uintptr_t>(0) };
        // (max_addr - base_addr) is the largest non-wrapping size; +64 wraps.
        const std::size_t wrapping_size{
            static_cast<std::size_t>(max_addr - base_addr) + std::size_t{ 64 } };
        const bool wrap_ok{ vmhook::os::safe_read(dst, small_src, wrapping_size) };
        check("safe_read_wrapping_size_returns_false", !wrap_ok);
    }

    // Reaching here proves neither monstrous read hung or faulted the process.
    check("safe_read_huge_size_process_survives", true);
}

// ---------------------------------------------------------------------------
// 11) is_readable_pointer tied to the safe_read story: TRUE on a live mapped
//     8-byte-aligned buffer, FALSE on null and on an unmapped sentinel.  This is
//     covered deeply in test_const_method_bounds.cpp; here we keep a focused
//     positive/negative cross-check so this file (which owns the readability
//     primitives) stands on its own and pins the safe_read/is_readable_pointer
//     agreement: where is_readable_pointer says readable, safe_read of one
//     pointer-word from that address succeeds; where it says not, safe_read of
//     the SAME address fails (or the address is filtered before the OS call).
// ---------------------------------------------------------------------------
static auto test_is_readable_pointer_matches_safe_read() -> void
{
    using vmhook::hotspot::is_readable_pointer;

    const std::size_t page{ vmhook::os::page_size() };
    void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
    if (!block)
    {
        check("is_readable_matches_safe_read_skipped_alloc_failed", false);
        return;
    }

    // 8-byte-aligned slots inside the live page (is_readable_pointer requires
    // 8-byte alignment; allocate_rwx returns a page-aligned base).
    auto* const words{ static_cast<std::uint64_t*>(block) };
    words[0] = 0x0123456789ABCDEFull;
    words[1] = 0xFEDCBA9876543210ull;

    // Positive: readable per the gate AND safe_read of a pointer-word succeeds.
    {
        const bool readable{ is_readable_pointer(&words[0]) };
        check("is_readable_pointer_true_on_mapped_aligned", readable);

        void* read_back{ nullptr };
        const bool ok{ vmhook::os::safe_read(&read_back, &words[0], sizeof(read_back)) };
        check("safe_read_succeeds_where_readable", ok);
        check("safe_read_returns_expected_word_where_readable",
              ok && reinterpret_cast<std::uintptr_t>(read_back)
                        == static_cast<std::uintptr_t>(0x0123456789ABCDEFull));
    }

    // Negative: null is rejected by the gate, and safe_read(null, ...) is false.
    {
        check("is_readable_pointer_false_on_null", !is_readable_pointer(nullptr));
        void* read_back{ nullptr };
        check("safe_read_false_on_null_src",
              !vmhook::os::safe_read(&read_back, nullptr, sizeof(read_back)));
    }

    // Negative: an unmapped high-but-canonical, 8-byte-aligned sentinel.  Both
    // the gate and (on !iOS) safe_read must reject it.
    {
        void* const unmapped{ reinterpret_cast<void*>(
            static_cast<std::uintptr_t>(0x0000'7FFE'0000'0000ull)) };
        check("is_readable_pointer_false_on_unmapped", !is_readable_pointer(unmapped));
#if !VMHOOK_OS_IOS
        void* read_back{ nullptr };
        check("safe_read_false_on_unmapped_where_gate_rejects",
              !vmhook::os::safe_read(&read_back, unmapped, sizeof(read_back)));
#endif
    }

    // is_readable_pointer is declared noexcept; pin it (cheap compile-time fact).
    check("is_readable_pointer_is_noexcept", noexcept(is_readable_pointer(nullptr)));

    vmhook::os::release(block, page);
}

// ===========================================================================
// DEEPENING SECTION (additive only).  Pure-logic / OS-layer invariants whose
// expected values are derived DIRECTLY from vmhook.hpp:
//   * safe_read / safe_read_fast input-guard matrix          (hpp 957-960, 1140-1143)
//   * the address-space-WRAP size guard                      (hpp 968-971)
//   * safe_read_pointer's pure pre-filter + happy-path word  (hpp 2106-2130)
//   * untag_pointer's bit math                               (hpp 2092-2097)
//   * is_readable_pointer's pure pre-filter rejections       (hpp 2018-2028)
//   * the named constants                                    (hpp 515,520)
// POSIX-SAFETY: every memory READ goes through a REAL owned buffer.  The only
// fabricated high/sentinel addresses handed to a function are handed to the
// PURE-FILTER paths (safe_read_pointer / is_readable_pointer / untag_pointer)
// or to safe_read's WRAP guard -- all of which return BEFORE any dereference, so
// nothing wild is ever read.  No raw NUL / non-ASCII bytes; no rewrite of any
// existing assertion.
// ===========================================================================
namespace deepen_os_safe_read
{

// ---------------------------------------------------------------------------
// D1) safe_read input-guard matrix (hpp 957-960): a false on ANY of
//     dst==null, src==null, size==0, independent of the other args, on EVERY
//     platform (the guard is the first statement, pre-OS, pre-memcpy).  The
//     existing protect-interaction file covers four entries; here we sweep the
//     full cross-product against a real owned src/dst so non-iOS and iOS agree.
// ---------------------------------------------------------------------------
static auto test_guard_matrix_full() -> void
{
    alignas(std::uint64_t) std::uint8_t real[16];
    fill_pattern(real, sizeof(real));
    std::uint8_t out[16]{};

    // dst==null with every size class (>0) and a valid src -> false.
    bool null_dst_false{ true };
    const std::size_t sizes[]{ std::size_t{ 1 }, std::size_t{ 7 },
                               std::size_t{ 8 }, std::size_t{ 16 } };
    for (const std::size_t s : sizes)
    {
        if (vmhook::os::safe_read(nullptr, real, s))
        {
            null_dst_false = false;
        }
    }
    check("safe_read_null_dst_all_sizes_false", null_dst_false);

    // src==null with every size class and a valid dst -> false.
    bool null_src_false{ true };
    for (const std::size_t s : sizes)
    {
        if (vmhook::os::safe_read(out, nullptr, s))
        {
            null_src_false = false;
        }
    }
    check("safe_read_null_src_all_sizes_false", null_src_false);

    // size==0 with all four null/non-null dst/src combinations -> false.
    const bool z0{ vmhook::os::safe_read(out,     real,    0u) };
    const bool z1{ vmhook::os::safe_read(out,     nullptr, 0u) };
    const bool z2{ vmhook::os::safe_read(nullptr, real,    0u) };
    const bool z3{ vmhook::os::safe_read(nullptr, nullptr, 0u) };
    check("safe_read_size0_valid_both_false",     !z0);
    check("safe_read_size0_null_src_false",       !z1);
    check("safe_read_size0_null_dst_false",       !z2);
    check("safe_read_size0_both_null_false",      !z3);

    // The positive twin of the size==0 guard: size==1 with BOTH valid must
    // succeed and copy exactly one byte (proves the guard is `==0`, not `<N`).
    out[0] = 0xCC;
    out[1] = 0xCC;
    const bool one_ok{ vmhook::os::safe_read(out, real, 1u) };
    check("safe_read_size1_valid_both_true", one_ok);
    check("safe_read_size1_copies_one_byte",
          one_ok && out[0] == real[0] && out[1] == 0xCC);
}

// ---------------------------------------------------------------------------
// D2) safe_read_fast shares the IDENTICAL guard prefix (hpp 1140-1143) and is a
//     byte-identical drop-in (hpp 1135-1136).  Pin that its guard matrix matches
//     safe_read's exactly, and that the size==1 positive twin copies one byte.
//     Runs on every platform (no faulting source involved).
// ---------------------------------------------------------------------------
static auto test_fast_guard_matrix() -> void
{
    alignas(std::uint64_t) std::uint8_t real[16];
    fill_pattern(real, sizeof(real));
    std::uint8_t out[16]{};

    check("safe_read_fast_null_dst_false",  !vmhook::os::safe_read_fast(nullptr, real, 8u));
    check("safe_read_fast_null_src_false",  !vmhook::os::safe_read_fast(out, nullptr, 8u));
    check("safe_read_fast_size0_false",     !vmhook::os::safe_read_fast(out, real, 0u));
    check("safe_read_fast_both_null_size0_false",
          !vmhook::os::safe_read_fast(nullptr, nullptr, 0u));

    out[0] = 0xCC;
    out[1] = 0xCC;
    const bool one_ok{ vmhook::os::safe_read_fast(out, real, 1u) };
    check("safe_read_fast_size1_true", one_ok);
    check("safe_read_fast_size1_copies_one_byte",
          one_ok && out[0] == real[0] && out[1] == 0xCC);
}

// ---------------------------------------------------------------------------
// D3) The address-space-WRAP size guard (hpp 968-971): when
//     (uintptr)src + size < (uintptr)src the read can never name a mapped range,
//     so safe_read returns false BEFORE any OS call / memcpy.  This is pure
//     uintptr arithmetic -- identical on every platform.
//
//     POSIX-SAFE: the wrap-detected paths return at line 970 with NO read, so a
//     sentinel src is never dereferenced.  We still anchor on a REAL owned src
//     for the cases that wrap by a small amount, plus the all-ones-src case
//     whose (~0 + 1 == 0 < ~0) wrap is rejected before any access.
// ---------------------------------------------------------------------------
static auto test_wrap_size_guard() -> void
{
    alignas(std::uint64_t) std::uint8_t real[64];
    fill_pattern(real, sizeof(real));
    std::uint8_t out[8]{};

    const std::uintptr_t base{ reinterpret_cast<std::uintptr_t>(real) };
    const std::uintptr_t top{ ~static_cast<std::uintptr_t>(0) };

    // size exactly == (top - base) does NOT wrap (base + size == top, no carry),
    // size == (top - base) + 1 DOES wrap (base + size == 0 < base).  We assert
    // only the WRAPPING case here against the real base; the non-wrapping
    // enormous read is the kernel's to reject (covered by the existing
    // test_safe_read_huge_size_rejected) and we do not force a giant scan.
    const std::size_t just_wraps{
        static_cast<std::size_t>(top - base) + std::size_t{ 1 } };
    check("safe_read_size_that_wraps_by_one_false",
          !vmhook::os::safe_read(out, real, just_wraps));

    // A src of all-ones with size==1: (~0 + 1) == 0 < ~0 -> wrap guard fires at
    // hpp 968 and returns false WITHOUT touching the bogus address.  This is the
    // one place a non-owned address is passed to safe_read and it is provably
    // never read (the wrap branch returns first).
    void* const all_ones{ reinterpret_cast<void*>(top) };
    check("safe_read_all_ones_src_size1_wrap_false",
          !vmhook::os::safe_read(out, all_ones, 1u));
    // Same address via SIZE_MAX also wraps -> false, also pre-read.
    check("safe_read_all_ones_src_sizemax_false",
          !vmhook::os::safe_read(out, all_ones, SIZE_MAX));

    // safe_read_fast delegates (cl.exe SEH path also funnels through safe_read on
    // a fault, but the wrap guard lives in BOTH guard prefixes) so the wrap is
    // rejected the same way.
    check("safe_read_fast_all_ones_src_size1_wrap_false",
          !vmhook::os::safe_read_fast(out, all_ones, 1u));
}

// ---------------------------------------------------------------------------
// D4) safe_read_pointer's PURE pre-filter (hpp 2106-2130).  Every rejection is
//     decided from the address alone BEFORE any os::safe_read, so passing
//     sentinel / unmapped-shaped addresses here is POSIX-SAFE (no dereference):
//       * null                                   -> nullptr (2109-2112)
//       * addr <= user_address_floor (0xFFFF)    -> nullptr (2116)
//       * addr >= user_address_ceiling           -> nullptr (2117)
//       * (addr & 0x7) != 0  (mis-8-aligned)     -> nullptr (2118)
//     The happy path (in-range, 8-aligned, owned, mapped) reads exactly one
//     pointer-word and returns it verbatim.
// ---------------------------------------------------------------------------
static auto test_safe_read_pointer_filter_and_happy_path() -> void
{
    using vmhook::hotspot::safe_read_pointer;

    // --- pure-filter rejections (no read occurs) ---
    check("srp_null_is_null", safe_read_pointer(nullptr) == nullptr);

    // addr == floor (0xFFFF) is rejected by `<=`; addr == floor-? both below.
    check("srp_floor_exact_is_null",
          safe_read_pointer(reinterpret_cast<const void*>(vmhook::os::user_address_floor))
              == nullptr);
    check("srp_below_floor_is_null",
          safe_read_pointer(reinterpret_cast<const void*>(std::uintptr_t{ 0x8 }))
              == nullptr);
    check("srp_one_is_null",
          safe_read_pointer(reinterpret_cast<const void*>(std::uintptr_t{ 0x1 }))
              == nullptr);

    // addr == ceiling is rejected by `>=`; just below ceiling but unaligned is
    // also rejected by the alignment clause, so it never reads either.
    check("srp_ceiling_exact_is_null",
          safe_read_pointer(reinterpret_cast<const void*>(vmhook::os::user_address_ceiling))
              == nullptr);
    check("srp_above_ceiling_is_null",
          safe_read_pointer(reinterpret_cast<const void*>(
              std::uintptr_t{ 0xFFFF800000000000ull })) == nullptr);

    // Mis-aligned in-range addresses (low 3 bits set): each rejected pre-read.
    bool misaligned_all_null{ true };
    for (std::uintptr_t off{ 1 }; off <= 7; ++off)
    {
        // 0x100000 is comfortably in (floor, ceiling); +off makes it mis-8-aligned.
        const std::uintptr_t a{ std::uintptr_t{ 0x100000ull } + off };
        if (safe_read_pointer(reinterpret_cast<const void*>(a)) != nullptr)
        {
            misaligned_all_null = false;
        }
    }
    check("srp_misaligned_in_range_all_null", misaligned_all_null);

    // --- happy path: owned, in-range, 8-aligned slot holding a known word ---
    alignas(std::uint64_t) std::uint64_t slot[2];
    slot[0] = 0x0123456789ABCDEFull;
    slot[1] = 0x00007FFEDCBA9876ull;  // a canonical user-space-shaped value
    // The slot's own address is 8-aligned and in (floor, ceiling) on any real
    // allocation; reading it yields slot[0] verbatim.
    const void* const got{ safe_read_pointer(&slot[0]) };
    check("srp_reads_stored_word",
          reinterpret_cast<std::uintptr_t>(got) == 0x0123456789ABCDEFull);
    const void* const got1{ safe_read_pointer(&slot[1]) };
    check("srp_reads_second_stored_word",
          reinterpret_cast<std::uintptr_t>(got1) == 0x00007FFEDCBA9876ull);

    // safe_read_pointer is declared noexcept (cheap compile-time fact).
    check("srp_is_noexcept", noexcept(safe_read_pointer(nullptr)));
}

// ---------------------------------------------------------------------------
// D5) untag_pointer (hpp 2092-2097): result == (addr & user_address_ceiling),
//     i.e. addr & 0x00007FFFFFFFFFFF.  Pure bit math; no read.  Enumerate the
//     bit-pattern cases the masking must handle.
// ---------------------------------------------------------------------------
static auto test_untag_pointer_bit_math() -> void
{
    using vmhook::hotspot::untag_pointer;
    constexpr std::uintptr_t mask{ 0x00007FFFFFFFFFFFull };  // == user_address_ceiling

    const std::uintptr_t cases[]{
        std::uintptr_t{ 0x0 },
        std::uintptr_t{ 0x1 },
        std::uintptr_t{ 0xFFFFull },
        std::uintptr_t{ 0x100000ull },
        std::uintptr_t{ 0x00007FFFFFFFFFFFull },        // == mask: unchanged
        std::uintptr_t{ 0xFFFF800000000000ull },        // pure high tag -> 0
        std::uintptr_t{ 0xFFFF7FFFFFFFFFFFull },         // high tag + low bits kept
        ~std::uintptr_t{ 0 },                            // all ones -> mask
        std::uintptr_t{ 0x8000000000000000ull },         // top bit only -> 0
        std::uintptr_t{ 0x0000800000000000ull },         // bit 47 -> stripped (mask bit47=0)
    };

    bool all_match{ true };
    for (const std::uintptr_t a : cases)
    {
        const std::uintptr_t expected{ a & mask };
        const auto result{ reinterpret_cast<std::uintptr_t>(
            untag_pointer(reinterpret_cast<const void*>(a))) };
        if (result != expected)
        {
            all_match = false;
        }
    }
    check("untag_pointer_masks_with_ceiling_all_cases", all_match);

    // Spot the canonical examples explicitly so a regression names itself.
    check("untag_pointer_strips_pure_high_tag",
          untag_pointer(reinterpret_cast<const void*>(
              std::uintptr_t{ 0xFFFF800000000000ull })) == nullptr);
    check("untag_pointer_all_ones_is_ceiling",
          reinterpret_cast<std::uintptr_t>(
              untag_pointer(reinterpret_cast<const void*>(~std::uintptr_t{ 0 }))) == mask);
    check("untag_pointer_in_range_unchanged",
          reinterpret_cast<std::uintptr_t>(
              untag_pointer(reinterpret_cast<const void*>(
                  std::uintptr_t{ 0x100000ull }))) == std::uintptr_t{ 0x100000ull });
    check("untag_pointer_is_noexcept",
          noexcept(untag_pointer(nullptr)));
}

// ---------------------------------------------------------------------------
// D6) is_readable_pointer's PURE pre-filter rejections (hpp 2021-2028).  These
//     branches are decided from the address alone, BEFORE the os::query_region
//     call, so the sentinel addresses are never read -- POSIX-SAFE.  The POSITIVE
//     (mapped, aligned) case is already covered by
//     test_is_readable_pointer_matches_safe_read above; here we exhaustively pin
//     the FILTER side:
//       * addr <= floor (0xFFFF)        -> false
//       * addr >= ceiling               -> false
//       * (addr & 0x7) != 0             -> false (the 8-byte-aligned requirement)
//     and confirm the two named constants have their documented values.
// ---------------------------------------------------------------------------
static auto test_is_readable_pointer_pure_filter() -> void
{
    using vmhook::hotspot::is_readable_pointer;

    // The named constants (hpp 515, 520) drive every gate; pin them.
    check("user_address_floor_value",
          vmhook::os::user_address_floor == std::uintptr_t{ 0xFFFFull });
    check("user_address_ceiling_value",
          vmhook::os::user_address_ceiling == std::uintptr_t{ 0x00007FFFFFFFFFFFull });

    // <= floor: 0, 0x8, 0xFFFF (exact) all rejected by the `<=` clause.
    bool below_floor_false{ true };
    const std::uintptr_t low[]{ std::uintptr_t{ 0x0 }, std::uintptr_t{ 0x8 },
                                std::uintptr_t{ 0x1000 },
                                vmhook::os::user_address_floor };
    for (const std::uintptr_t a : low)
    {
        if (is_readable_pointer(reinterpret_cast<const void*>(a)))
        {
            below_floor_false = false;
        }
    }
    check("irp_at_or_below_floor_all_false", below_floor_false);

    // >= ceiling: exact ceiling and a high non-canonical address rejected.
    bool above_ceiling_false{ true };
    const std::uintptr_t high[]{ vmhook::os::user_address_ceiling,
                                 std::uintptr_t{ 0x0000800000000000ull },
                                 std::uintptr_t{ 0xFFFF800000000000ull },
                                 ~std::uintptr_t{ 0 } };
    for (const std::uintptr_t a : high)
    {
        if (is_readable_pointer(reinterpret_cast<const void*>(a)))
        {
            above_ceiling_false = false;
        }
    }
    check("irp_at_or_above_ceiling_all_false", above_ceiling_false);

    // Mis-8-aligned in-range: low 3 bits set -> false (the alignment clause).
    bool misaligned_false{ true };
    for (std::uintptr_t off{ 1 }; off <= 7; ++off)
    {
        const std::uintptr_t a{ std::uintptr_t{ 0x200000ull } + off };
        if (is_readable_pointer(reinterpret_cast<const void*>(a)))
        {
            misaligned_false = false;
        }
    }
    check("irp_misaligned_in_range_all_false", misaligned_false);
}

} // namespace deepen_os_safe_read

// ===========================================================================
// DEEPENING SECTION 2 (additive only).  Signal-handler install/disarm
// contracts + the safe_write twin + cross-path parity — os-layer surface the
// prior passes did not touch.  Every expected value is traced DIRECTLY from
// vmhook.hpp:
//   * detail_signal::probe_state default-init                 (hpp 907-912)
//   * detail_signal::active_state thread_local + disarm        (hpp 914, 1006-1014)
//   * detail_signal::install_once idempotent / returns true    (hpp 929-941)
//   * safe_write guard prefix + per-platform fault-safe store  (hpp 1048-1075)
//   * safe_read/safe_write round-trip agreement                (hpp 955-1075)
// POSIX-SAFETY: every dereferenced/protected/written page is one ALLOCATED IN
// THIS FILE (allocate_rwx) or a stack/array buffer.  No fabricated, unmapped,
// or high address is ever handed to a reading/writing helper; the only
// non-owned addresses go to the input-guard (null) paths, which return before
// any access.  No raw NUL / non-ASCII bytes; no existing assertion is touched.
// ===========================================================================
namespace deepen_os_signal_handler
{

#if VMHOOK_OS_LINUX || VMHOOK_OS_ANDROID
// ---------------------------------------------------------------------------
// S1) detail_signal::probe_state default-construction contract (hpp 907-912).
//     A freshly default-constructed probe_state must report active==false and
//     fault==false — the disarmed state safe_read relies on before it arms.
//     This is the struct the SIGSEGV/SIGBUS handler keys its recovery on; the
//     header comment for this file only ever proxied it indirectly.  Pure,
//     no signal raised, a stack-owned struct.
// ---------------------------------------------------------------------------
static auto test_probe_state_default_init() -> void
{
    vmhook::os::detail_signal::probe_state st{};
    check("probe_state_default_active_false", st.active == false);
    check("probe_state_default_fault_false",  st.fault == false);
}

// ---------------------------------------------------------------------------
// S2) detail_signal::install_once (hpp 929-941) is a magic-static init-once:
//     the FIRST call installs the SIGSEGV+SIGBUS sigaction and caches whether
//     BOTH succeeded; every later call returns that SAME cached bool.  On a
//     normal Linux/Android runner both sigaction calls succeed, so it returns
//     true and is idempotent.  install_once is noexcept (hpp 929).  We only
//     assert idempotence + noexcept unconditionally; the true-value assertion
//     is the expected outcome on any non-sandboxed host (a sandbox that blocks
//     sigaction would make it false, which we surface as [INFO] not [FAIL]).
// ---------------------------------------------------------------------------
static auto test_install_once_idempotent() -> void
{
    const bool a{ vmhook::os::detail_signal::install_once() };
    const bool b{ vmhook::os::detail_signal::install_once() };
    const bool c{ vmhook::os::detail_signal::install_once() };
    // The cached magic-static must yield a byte-stable answer on every call.
    check("install_once_idempotent", a == b && b == c);
    check("install_once_is_noexcept",
          noexcept(vmhook::os::detail_signal::install_once()));
    if (a)
    {
        check("install_once_succeeds_on_host", true);
    }
    else
    {
        std::printf("[INFO] install_once returned false: sandbox blocked sigaction\n");
    }
}

// ---------------------------------------------------------------------------
// S3) detail_signal::active_state is `thread_local` (hpp 914), so it is the
//     per-thread "currently inside a protected memcpy" pointer.  Outside any
//     safe_read it must be nullptr — both on the calling thread and on a fresh
//     worker thread (proving the thread_local is zero-initialised per thread,
//     the property that isolates concurrent probes).  No fault is raised.
// ---------------------------------------------------------------------------
static auto test_active_state_thread_local_null_at_rest() -> void
{
    check("active_state_null_on_main_at_rest",
          vmhook::os::detail_signal::active_state == nullptr);

    std::atomic<bool> worker_saw_null{ false };
    std::thread worker{ [&] {
        worker_saw_null.store(
            vmhook::os::detail_signal::active_state == nullptr,
            std::memory_order_relaxed);
    } };
    worker.join();
    check("active_state_null_on_fresh_thread", worker_saw_null.load());
}

#if !VMHOOK_OS_IOS
// ---------------------------------------------------------------------------
// S4) The disarm contract (hpp 1006-1014): safe_read publishes
//     active_state=&state before the probed memcpy and ALWAYS resets it to
//     nullptr afterwards (line 1014), on BOTH the success and the fault path.
//     After any safe_read returns, active_state must be nullptr again — a leak
//     here would leave a stale jmp_buf armed for the next unrelated SIGSEGV.
//     We assert it after a VALID read (owned page) and after a FAULTING read
//     (owned no_access page, which drives the sigsetjmp fallback after
//     process_vm_readv short-reads).  PROT_NONE-gated; off iOS.
// ---------------------------------------------------------------------------
static auto test_active_state_cleared_after_read() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
    if (!block)
    {
        check("active_state_disarm_skipped_alloc_failed", false);
        return;
    }
    auto* const bytes{ static_cast<std::uint8_t*>(block) };
    fill_pattern(bytes, page);

    // Valid read first: active_state must be null after it returns.
    std::uint8_t dst[8]{};
    const bool good{ vmhook::os::safe_read(dst, bytes, sizeof(dst)) };
    check("disarm_valid_read_succeeds", good);
    check("active_state_null_after_valid_read",
          vmhook::os::detail_signal::active_state == nullptr);

    // Now a faulting read through an owned no_access page.
    const bool locked{ vmhook::os::protect(block, page,
                                           vmhook::os::memory_protection::no_access,
                                           nullptr) };
    if (locked)
    {
        const bool bad{ vmhook::os::safe_read(dst, block, sizeof(dst)) };
        check("disarm_faulting_read_returns_false", !bad);
        check("active_state_null_after_faulting_read",
              vmhook::os::detail_signal::active_state == nullptr);
        (void)vmhook::os::protect(block, page,
                                  vmhook::os::memory_protection::read_write, nullptr);
    }
    else
    {
        std::printf("[INFO] disarm faulting-read case skipped: PROT_NONE refused\n");
    }
    vmhook::os::release(block, page);
}
#endif // !VMHOOK_OS_IOS
#endif // VMHOOK_OS_LINUX || VMHOOK_OS_ANDROID

// ---------------------------------------------------------------------------
// S5) safe_write input-guard prefix (hpp 1050-1053) is byte-for-byte the same
//     guard as safe_read's (dst==null || src==null || size==0 -> false), on
//     every platform, before any OS call.  Pin the full guard matrix against a
//     real owned src/dst so even iOS (where safe_write always returns false)
//     agrees on the negative entries.
// ---------------------------------------------------------------------------
static auto test_safe_write_guard_matrix() -> void
{
    alignas(std::uint64_t) std::uint8_t src[16];
    fill_pattern(src, sizeof(src));
    alignas(std::uint64_t) std::uint8_t dst[16]{};

    check("safe_write_null_dst_false",  !vmhook::os::safe_write(nullptr, src, 8u));
    check("safe_write_null_src_false",  !vmhook::os::safe_write(dst, nullptr, 8u));
    check("safe_write_size0_false",     !vmhook::os::safe_write(dst, src, 0u));
    check("safe_write_both_null_size0_false",
          !vmhook::os::safe_write(nullptr, nullptr, 0u));
}

#if !VMHOOK_OS_IOS
// ---------------------------------------------------------------------------
// S6) safe_write happy path + safe_read agreement (hpp 1048-1075).  On
//     Windows/macOS/Linux/Android safe_write performs a kernel-validated store
//     into a committed writable page; iOS/unknown refuse (return false) and so
//     are gated out.  Write a known pattern into an OWNED page via safe_write,
//     then read it back with BOTH a plain load and safe_read — every byte must
//     match.  Proves the write twin lands exactly `size` bytes and that the
//     read/write primitives round-trip through the same owned memory.
// ---------------------------------------------------------------------------
static auto test_safe_write_roundtrip_owned_page() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
    if (!block)
    {
        check("safe_write_roundtrip_skipped_alloc_failed", false);
        return;
    }
    auto* const target{ static_cast<std::uint8_t*>(block) };
    std::memset(target, 0x00, page);

    alignas(std::uint64_t) std::uint8_t pattern[32];
    fill_pattern(pattern, sizeof(pattern));

    const bool wrote{ vmhook::os::safe_write(target, pattern, sizeof(pattern)) };
    check("safe_write_owned_page_succeeds", wrote);
    // Direct load: every written byte present, and the byte just past the span
    // untouched (still 0x00) — proves exactly sizeof(pattern) bytes landed.
    check("safe_write_owned_page_exact_bytes",
          wrote && std::memcmp(target, pattern, sizeof(pattern)) == 0
              && target[sizeof(pattern)] == 0x00);

    // safe_read of the just-written region must agree byte-for-byte.
    std::uint8_t readback[32]{};
    const bool read_ok{ vmhook::os::safe_read(readback, target, sizeof(readback)) };
    check("safe_read_after_safe_write_succeeds", read_ok);
    check("safe_read_after_safe_write_matches_pattern",
          read_ok && std::memcmp(readback, pattern, sizeof(pattern)) == 0);

    vmhook::os::release(block, page);
}

// ---------------------------------------------------------------------------
// S7) safe_write of a no_access OWNED page must return false (kernel rejects
//     the store), never fault, and must NOT have altered the original bytes
//     once the page is made readable again.  The write counterpart of the
//     no_access safe_read cases.  PROT_NONE-gated; off iOS.
// ---------------------------------------------------------------------------
static auto test_safe_write_no_access_page_false() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
    if (!block)
    {
        check("safe_write_no_access_skipped_alloc_failed", false);
        return;
    }
    auto* const target{ static_cast<std::uint8_t*>(block) };
    // Known marker bytes so we can prove the refused write changed nothing.
    fill_pattern(target, page);

    const bool locked{ vmhook::os::protect(block, page,
                                           vmhook::os::memory_protection::no_access,
                                           nullptr) };
    if (!locked)
    {
        std::printf("[INFO] safe_write_no_access skipped: PROT_NONE refused\n");
        (void)make_writable(block, page);
        vmhook::os::release(block, page);
        return;
    }

    alignas(std::uint64_t) std::uint8_t intruder[16];
    std::memset(intruder, 0x5C, sizeof(intruder));
    const bool wrote{ vmhook::os::safe_write(block, intruder, sizeof(intruder)) };
    check("safe_write_no_access_returns_false", !wrote);

    // Restore RW and confirm the first bytes are still the original pattern,
    // i.e. the refused write was all-or-nothing (no partial corruption).
    const bool restored{ make_writable(block, page) };
    check("safe_write_no_access_restore_writable", restored);
    if (restored)
    {
        bool unchanged{ true };
        for (std::size_t i{ 0 }; i < sizeof(intruder); ++i)
        {
            if (target[i] != static_cast<std::uint8_t>((i * 31u + 7u) & 0xFFu))
            {
                unchanged = false;
            }
        }
        check("safe_write_no_access_left_bytes_unchanged", unchanged);
    }
    vmhook::os::release(block, page);
}

// ---------------------------------------------------------------------------
// S8) safe_read_fast vs safe_read parity on a NO_ACCESS owned page: both must
//     return false (neither faults).  The positive-parity sweep is covered
//     above; this pins that the fast twin's NEGATIVE result also matches the
//     authoritative path on an unreadable page (on cl.exe the SEH copy faults
//     then falls back to safe_read; everywhere else it delegates outright).
//     PROT_NONE-gated; off iOS.
// ---------------------------------------------------------------------------
static auto test_safe_read_fast_parity_on_no_access() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
    if (!block)
    {
        check("fast_parity_no_access_skipped_alloc_failed", false);
        return;
    }
    *static_cast<volatile std::uint8_t*>(block) = 0x7E;

    const bool locked{ vmhook::os::protect(block, page,
                                           vmhook::os::memory_protection::no_access,
                                           nullptr) };
    if (!locked)
    {
        std::printf("[INFO] fast_parity_no_access skipped: PROT_NONE refused\n");
        vmhook::os::release(block, page);
        return;
    }

    bool all_match_false{ true };
    const std::size_t widths[]{ std::size_t{ 1 }, std::size_t{ 4 },
                                std::size_t{ 8 }, std::size_t{ 64 } };
    std::uint8_t dst[64];
    for (const std::size_t w : widths)
    {
        const bool slow{ vmhook::os::safe_read(dst, block, w) };
        const bool fast{ vmhook::os::safe_read_fast(dst, block, w) };
        if (slow != fast || slow)
        {
            all_match_false = false;
        }
    }
    check("safe_read_fast_matches_safe_read_false_on_no_access", all_match_false);

    (void)vmhook::os::protect(block, page,
                              vmhook::os::memory_protection::read_write, nullptr);
    vmhook::os::release(block, page);
}
#endif // !VMHOOK_OS_IOS

} // namespace deepen_os_signal_handler

int main()
{
    // Positive / parity (run on every platform, iOS included — these are valid
    // reads that never touch a no_access or bogus page).
    test_safe_read_positive_roundtrip_all_widths();
    test_safe_read_fast_byte_identical_to_safe_read();
    test_safe_read_overlapping_regions_defined();
    test_safe_read_huge_size_rejected();
    test_is_readable_pointer_matches_safe_read();

    // Deepening (additive): pure-logic guard/filter/bit-math invariants derived
    // straight from vmhook.hpp.  All use owned buffers or pre-read filter paths,
    // so they are safe on every platform (iOS included) and never dereference a
    // fabricated address.
    deepen_os_safe_read::test_guard_matrix_full();
    deepen_os_safe_read::test_fast_guard_matrix();
    deepen_os_safe_read::test_wrap_size_guard();
    deepen_os_safe_read::test_safe_read_pointer_filter_and_happy_path();
    deepen_os_safe_read::test_untag_pointer_bit_math();
    deepen_os_safe_read::test_is_readable_pointer_pure_filter();

    // Deepening 2 (additive): signal-handler install/disarm contracts + the
    // safe_write twin + cross-path parity.  The signal-handler pieces are
    // Linux/Android-only (detail_signal only exists there); the safe_write guard
    // matrix runs on every platform; the safe_write/fast positive + no_access
    // cases need the fault-safe path and are gated off iOS.
#if VMHOOK_OS_LINUX || VMHOOK_OS_ANDROID
    deepen_os_signal_handler::test_probe_state_default_init();
    deepen_os_signal_handler::test_install_once_idempotent();
    deepen_os_signal_handler::test_active_state_thread_local_null_at_rest();
#if !VMHOOK_OS_IOS
    deepen_os_signal_handler::test_active_state_cleared_after_read();
#endif
#endif
    deepen_os_signal_handler::test_safe_write_guard_matrix();
#if !VMHOOK_OS_IOS
    deepen_os_signal_handler::test_safe_write_roundtrip_owned_page();
    deepen_os_signal_handler::test_safe_write_no_access_page_false();
    deepen_os_signal_handler::test_safe_read_fast_parity_on_no_access();
#endif

    // Fault-safety cases that require the fault-safe read path — gated off iOS,
    // where safe_read is a raw memcpy and these would fault the process.
#if !VMHOOK_OS_IOS
    test_safe_read_page_boundary_trio();
    test_safe_read_no_access_page_all_widths();
    test_safe_read_freed_memory_no_crash();
    test_safe_read_known_bad_pointers_no_fault();
    test_safe_read_concurrent_probes();
    test_safe_read_repeated_fallback_keeps_process_healthy();
#else
    std::printf("[INFO] iOS: fault-safety safe_read cases skipped (no fault-safe read API)\n");
#endif

    if (failures == 0)
    {
        std::printf("vmhook os safe_read: OK\n");
    }
    else
    {
        std::printf("vmhook os safe_read: %d FAILURE(S)\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
