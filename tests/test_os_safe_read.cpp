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

int main()
{
    // Positive / parity (run on every platform, iOS included — these are valid
    // reads that never touch a no_access or bogus page).
    test_safe_read_positive_roundtrip_all_widths();
    test_safe_read_fast_byte_identical_to_safe_read();
    test_safe_read_overlapping_regions_defined();
    test_safe_read_huge_size_rejected();
    test_is_readable_pointer_matches_safe_read();

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
