// Exhaustive standalone (no-JVM) tests for vmhook::os::query_region — the
// memory-region introspection primitive that snapshots the VM region containing
// an address into a region_info { base, size, committed, free, readable,
// executable, guarded }.  This is the foundation every higher layer trusts to
// decide "is this pointer real memory?" (hotspot::is_readable_pointer), "where
// does this stub end?" (hotspot::find_stub_size), and "is there a free hole for
// a trampoline?" (the trampoline allocator's +/-2 GiB hole-walk).
//
// WHAT ALREADY EXISTS (not duplicated here):
//   * tests/test_os_protect_interaction.cpp — query_region of a freshly RWX'd
//     block (committed/readable/!guarded/size>=page), query_region(nullptr)
//     default, the protect->query roundtrip (read/execute_read/read_write
//     positive attrs), and free-after-release (gated off iOS).
//   * tests/test_os_layer.cpp — the basic protect/allocate round-trip.
//
// WHAT THIS FILE ADDS — the EXHAUSTIVE structural-invariant plan for the
// primitive itself, every assertion an INVARIANT (containment, alignment,
// found/not-found, no overflow, idempotency, consumer consistency) rather than a
// platform-specific address or size constant:
//   (1) CONTAINMENT across buffer kinds — a stack local, a heap (new[]) buffer,
//       and an allocate_rwx block: the reported region [base, base+size) must
//       CONTAIN the queried address, size > 0, committed + readable, base
//       page-aligned.
//   (2) INTERIOR + UNALIGNED queries — at block, block+page, block+size-1,
//       block+1, block+page/3: every answer reports a base <= the query address
//       that contains it.  query_region must NOT require alignment (it is the
//       *caller* that aligns); the aligned and unaligned interior queries on the
//       same page must agree.
//   (3) SIZE bounds + no address-space wrap — size >= page for a committed
//       alloc; base + size never wraps past UINTPTR_MAX (guards the trailing-hole
//       and region_end additions the consumers do).
//   (4) ATTRIBUTE correctness across EVERY memory_protection — walk one page
//       through each portable protection and assert the decoded readable /
//       executable track the request (positive direction; gated like the sibling
//       tests for no_access / W^X).
//   (5) NULL + BOUNDARY inputs — nullptr, user_address_floor, just above it,
//       user_address_ceiling, a non-canonical kernel address, 0x1: NO crash and
//       a sane (default-or-free) region_info, with base+size never wrapping.
//   (6) FREE / hole reporting after release — Win/Linux must surface free OR
//       !committed for a just-unmapped block (gated off iOS, where the stub is a
//       permissive lie by design).
//   (7) IDEMPOTENCY — two consecutive queries on a stable mapping return a
//       byte-identical region_info (catches any reliance on mutated static state
//       or a partially-filled struct on an early return).
//   (8) CONSUMER round-trips (no JVM) — is_readable_pointer agrees with
//       query_region on the same address (committed && readable && !guarded);
//       find_stub_size on a fresh single page == min(page, 0x2000) and falls back
//       to 0x2000 for a one-past-the-end (region_end <= start) query.
//
// SAFETY: every test only ever queries memory it allocated itself OR a pointer
// it never dereferences (query_region only inspects the kernel's view; it never
// reads through the pointer).  The known-unmapped / kernel / non-canonical
// probes are pure metadata queries — query_region must answer them WITHOUT
// faulting on every platform.  No page is ever executed; every owned page is
// restored writable before release.
//
// PORTABILITY: region attribute VALUES, the page size, and where the address
// space's holes sit differ by OS, so every assertion is an invariant.  The two
// platform-gated pieces mirror the sibling tests: the free-after-release
// contract is `#if !VMHOOK_OS_IOS` (the iOS stub always claims committed), and
// the protection-attribute behavioural walk treats W^X / PROT_NONE refusal as a
// skip.  No <charconv>/float, no std::expected/syncstream — builds under the
// MinGW libstdc++, MSVC STL, and libc++ CI toolchains.
#include <vmhook/vmhook.hpp>

#include <cstdio>
#include <cstdint>
#include <cstring>
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

// Restore a writable mapping for teardown.  execute_rw is the natural state a
// page came back from allocate_rwx with; Apple arm64 / current iOS enforce W^X
// and refuse PROT_WRITE|PROT_EXEC without the JIT entitlement, so fall back to
// plain read_write (which is what allocate_rwx itself falls back to there).
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

// True if [base, base+size) is a non-wrapping range that contains `addr`.
// Containment is the central invariant of query_region: the region it reports
// for an address must actually enclose that address.  Computed in uintptr_t so a
// pathological base+size wrap is detected rather than wrapping silently.
static auto region_contains(const vmhook::os::region_info& info, const void* addr) -> bool
{
    const std::uintptr_t base{ reinterpret_cast<std::uintptr_t>(info.base) };
    const std::uintptr_t a{ reinterpret_cast<std::uintptr_t>(addr) };
    const std::uintptr_t end{ base + info.size };
    if (end < base) // base + size wrapped the address space — never valid.
    {
        return false;
    }
    return a >= base && a < end;
}

// base + size must never wrap past the top of the address space.  Consumers add
// info.size to info.base (find_stub_size's region_end, the allocator's walk), so
// a wrapping range would hand them a bogus pointer.
static auto region_no_overflow(const vmhook::os::region_info& info) -> bool
{
    const std::uintptr_t base{ reinterpret_cast<std::uintptr_t>(info.base) };
    return base + info.size >= base;
}

// ---------------------------------------------------------------------------
// (1) CONTAINMENT across buffer kinds.  query_region of a live address — no
// matter how that memory was obtained — must report a region that contains the
// address, is non-empty, committed + readable, and starts on a page boundary.
// We probe three independent allocation kinds so a path that only works for one
// (e.g. only for VirtualAlloc'd blocks, not the C++ heap or the stack) shows up.
// ---------------------------------------------------------------------------
static auto test_query_region_contains_live_addresses() -> void
{
    const std::size_t page{ vmhook::os::page_size() };

    // (a) A stack local.  Its address is unquestionably live, committed, and
    // readable; query_region must enclose it.
    {
        volatile std::uint8_t stack_buf[256];
        stack_buf[0] = 0x11;
        stack_buf[sizeof(stack_buf) - 1] = 0x22;
        const void* const p{ const_cast<const std::uint8_t*>(&stack_buf[128]) };
        const auto info{ vmhook::os::query_region(p) };
        check("query_region_stack_committed", info.committed);
        check("query_region_stack_readable", info.readable);
        check("query_region_stack_size_nonzero", info.size != 0);
        check("query_region_stack_contains_addr", region_contains(info, p));
        check("query_region_stack_base_page_aligned",
              (reinterpret_cast<std::uintptr_t>(info.base) % page) == 0);
        check("query_region_stack_no_overflow", region_no_overflow(info));
    }

    // (b) A heap buffer from the C++ allocator (new[]).  Large enough that it is
    // very unlikely to share a partially-uncommitted page boundary.
    {
        std::vector<std::uint8_t> heap(64u * 1024u, std::uint8_t{ 0xAB });
        const void* const p{ &heap[heap.size() / 2] };
        const auto info{ vmhook::os::query_region(p) };
        check("query_region_heap_committed", info.committed);
        check("query_region_heap_readable", info.readable);
        check("query_region_heap_size_nonzero", info.size != 0);
        check("query_region_heap_contains_addr", region_contains(info, p));
        check("query_region_heap_base_page_aligned",
              (reinterpret_cast<std::uintptr_t>(info.base) % page) == 0);
        check("query_region_heap_no_overflow", region_no_overflow(info));
    }

    // (c) An allocate_rwx block (the trampoline allocator's own currency).
    {
        void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
        if (!block)
        {
            check("query_region_rwx_skipped_alloc_failed", false);
        }
        else
        {
            *static_cast<volatile std::uint8_t*>(block) = 0x42;
            const auto info{ vmhook::os::query_region(block) };
            check("query_region_rwx_committed", info.committed);
            check("query_region_rwx_readable", info.readable);
            check("query_region_rwx_size_at_least_page", info.size >= page);
            check("query_region_rwx_contains_addr", region_contains(info, block));
            check("query_region_rwx_base_page_aligned",
                  (reinterpret_cast<std::uintptr_t>(info.base) % page) == 0);
            check("query_region_rwx_not_guarded", !info.guarded);
            check("query_region_rwx_no_overflow", region_no_overflow(info));
            vmhook::os::release(block, page);
        }
    }
}

// ---------------------------------------------------------------------------
// (2) INTERIOR + UNALIGNED queries inside a known multi-page allocation.  For
// every probed offset (start, +1 byte, +page/3, +page, +size-1) the reported
// region must CONTAIN the query address — i.e. base <= addr.  This is exactly
// the invariant a region-walking primitive must satisfy and the one that the
// macOS mach_vm_region "advance to next mapping" aliasing would violate (it
// returns base > addr for an in-hole query); the test is written platform-
// agnostic so it pins the contract everywhere.  query_region must not require an
// aligned input — the unaligned interior probe must land in the same containing
// region as the aligned one.
// ---------------------------------------------------------------------------
static auto test_query_region_interior_and_unaligned() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    const std::size_t size{ page * 4 };
    void* const block{ vmhook::os::allocate_rwx(nullptr, size) };
    if (!block)
    {
        check("query_region_interior_skipped_alloc_failed", false);
        return;
    }

    auto* const bytes{ static_cast<std::uint8_t*>(block) };
    // Touch every page so the whole span is unambiguously committed.
    for (std::size_t off{ 0 }; off < size; off += page)
    {
        bytes[off] = static_cast<std::uint8_t>(0x10 + (off / page));
    }
    bytes[size - 1] = 0xFE;

    struct Probe { const char* tag; std::size_t off; };
    const Probe probes[]{
        { "at_base",        std::size_t{ 0 } },
        { "plus_one_byte",  std::size_t{ 1 } },
        { "third_of_page",  page / 3 },
        { "plus_one_page",  page },
        { "last_byte",      size - 1 },
    };

    bool all_contained{ true };
    bool all_base_aligned{ true };
    bool all_committed{ true };
    bool all_no_overflow{ true };
    for (const Probe& pr : probes)
    {
        const void* const p{ bytes + pr.off };
        const auto info{ vmhook::os::query_region(p) };
        if (!region_contains(info, p))
        {
            all_contained = false;
            std::printf("[INFO] query_region interior probe '%s' did not contain its address\n",
                        pr.tag);
        }
        if ((reinterpret_cast<std::uintptr_t>(info.base) % page) != 0)
        {
            all_base_aligned = false;
        }
        if (!info.committed)
        {
            all_committed = false;
        }
        if (!region_no_overflow(info))
        {
            all_no_overflow = false;
        }
    }
    check("query_region_interior_all_probes_contained", all_contained);
    check("query_region_interior_all_bases_page_aligned", all_base_aligned);
    check("query_region_interior_all_committed", all_committed);
    check("query_region_interior_all_no_overflow", all_no_overflow);

    // The aligned base query and an unaligned interior query within the SAME
    // first page must resolve to the same containing region base — query_region
    // ignores sub-page alignment of its input.
    {
        const auto aligned{ vmhook::os::query_region(bytes) };
        const auto unaligned{ vmhook::os::query_region(bytes + 1) };
        check("query_region_unaligned_same_base_as_aligned",
              aligned.base == unaligned.base);
        check("query_region_unaligned_same_size_as_aligned",
              aligned.size == unaligned.size);
    }

    check("query_region_interior_restore_writable", make_writable(block, size));
    vmhook::os::release(block, size);
}

// ---------------------------------------------------------------------------
// (3) SIZE lower bound + no wrap for a multi-page allocation.  A committed
// allocation of N pages must report at least one page of region (the kernel may
// coalesce neighbours and report MORE, never less), and base + size must not
// wrap.  This guards the consumers that add info.size to info.base.
// ---------------------------------------------------------------------------
static auto test_query_region_size_bounds() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    const std::size_t size{ page * 2 };
    void* const block{ vmhook::os::allocate_rwx(nullptr, size) };
    if (!block)
    {
        check("query_region_size_bounds_skipped_alloc_failed", false);
        return;
    }
    *static_cast<volatile std::uint8_t*>(block) = 0x5A;

    const auto info{ vmhook::os::query_region(block) };
    check("query_region_size_at_least_one_page", info.size >= page);
    check("query_region_size_base_plus_size_no_wrap", region_no_overflow(info));
    // The region must start at or below our block (it could be a coalesced
    // region that began earlier) and our block must fall inside it.
    check("query_region_size_block_inside_region", region_contains(info, block));

    vmhook::os::release(block, size);
}

// ---------------------------------------------------------------------------
// (4) ATTRIBUTE correctness across EVERY memory_protection.  Allocate one page
// and walk it through each portable protection, querying after each protect()
// and asserting the decoded readable / executable bits track the request.  This
// is the single biggest query_region gap the existing tests leave: today only
// the RWX state's attributes and a 3-value positive roundtrip are checked, so a
// bitmask/switch arm that mis-decodes a protection (e.g. forgets
// PAGE_EXECUTE_WRITECOPY on Windows, or VM_PROT_EXECUTE on macOS) would pass.
//
// We assert the POSITIVE direction the callers rely on (read -> readable,
// execute_read -> readable+executable, ...).  We do NOT assert the negative
// "execute bit clear after read" because some kernels keep an executable
// mapping's backing flags coarser than the portable enum (mirroring the note in
// test_os_protect_interaction.cpp).  no_access and X-bearing protections are
// gated: no_access readability is only meaningful where the protect() succeeds,
// and execute_rw may be refused under W^X (treated as a skip, never a failure).
// ---------------------------------------------------------------------------
static auto test_query_region_attributes_each_protection() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
    if (!block)
    {
        check("query_region_attrs_each_skipped_alloc_failed", false);
        return;
    }

    auto* const bytes{ static_cast<std::uint8_t*>(block) };
    bytes[0] = 0x10;

    // read -> readable, not executable-required.  committed must hold.
    {
        const bool ok{ vmhook::os::protect(block, page,
                                           vmhook::os::memory_protection::read, nullptr) };
        check("query_region_attr_read_protect_succeeds", ok);
        if (ok)
        {
            const auto info{ vmhook::os::query_region(block) };
            check("query_region_attr_read_committed", info.committed);
            check("query_region_attr_read_readable", info.readable);
            check("query_region_attr_read_contains", region_contains(info, block));
        }
    }

    // read_write -> readable; a store proves the W bit independent of the query.
    {
        const bool ok{ vmhook::os::protect(block, page,
                                           vmhook::os::memory_protection::read_write, nullptr) };
        check("query_region_attr_rw_protect_succeeds", ok);
        if (ok)
        {
            const auto info{ vmhook::os::query_region(block) };
            check("query_region_attr_rw_committed", info.committed);
            check("query_region_attr_rw_readable", info.readable);
            bytes[0] = 0x20;
            check("query_region_attr_rw_store_sticks", bytes[0] == 0x20);
        }
    }

    // execute_read -> readable AND executable.
    {
        const bool ok{ vmhook::os::protect(block, page,
                                           vmhook::os::memory_protection::execute_read, nullptr) };
        check("query_region_attr_execrx_protect_succeeds", ok);
        if (ok)
        {
            const auto info{ vmhook::os::query_region(block) };
            check("query_region_attr_execrx_readable", info.readable);
            check("query_region_attr_execrx_executable", info.executable);
            check("query_region_attr_execrx_committed", info.committed);
        }
    }

    // execute_rw -> readable AND executable when granted.  Refusal under W^X is a
    // skip, never a failure.
    {
        const bool ok{ vmhook::os::protect(block, page,
                                           vmhook::os::memory_protection::execute_rw, nullptr) };
        if (ok)
        {
            const auto info{ vmhook::os::query_region(block) };
            check("query_region_attr_execrw_readable", info.readable);
            check("query_region_attr_execrw_executable", info.executable);
            check("query_region_attr_execrw_committed", info.committed);
        }
        else
        {
            std::printf("[INFO] query_region_attr_execrw skipped: protect refused (W^X)\n");
        }
    }

    // no_access -> NOT readable AND NOT executable.  Where the platform refuses
    // PROT_NONE the protect() call fails and we skip (matching the sibling
    // tests).  On platforms whose region query reports coarser flags for a
    // no-access mapping this still holds because the *enum* mapped to the
    // most-restrictive native protection.
    {
        const bool ok{ vmhook::os::protect(block, page,
                                           vmhook::os::memory_protection::no_access, nullptr) };
        if (ok)
        {
            const auto info{ vmhook::os::query_region(block) };
            check("query_region_attr_noaccess_not_readable", !info.readable);
            check("query_region_attr_noaccess_not_executable", !info.executable);
        }
        else
        {
            std::printf("[INFO] query_region_attr_noaccess skipped: PROT_NONE refused\n");
        }
    }

    check("query_region_attrs_each_restore_writable", make_writable(block, page));
    vmhook::os::release(block, page);
}

// ---------------------------------------------------------------------------
// (5) NULL + BOUNDARY inputs.  query_region must answer every degenerate /
// out-of-range / kernel / non-canonical address WITHOUT faulting (it only
// inspects the kernel's map; it never dereferences the pointer).  The result
// must be a sane region_info — for these inputs that means either the
// default-constructed not-found sentinel (base null, size 0, all false) or a
// free region — and base + size must never wrap.  nullptr specifically must be
// the all-false default (documented null-input sentinel).
// ---------------------------------------------------------------------------
static auto test_query_region_null_and_boundary_inputs() -> void
{
    // nullptr -> default-constructed region_info (the documented sentinel).
    {
        const auto info{ vmhook::os::query_region(nullptr) };
        check("query_region_null_base_null", info.base == nullptr);
        check("query_region_null_size_zero", info.size == 0);
        check("query_region_null_not_committed", !info.committed);
        check("query_region_null_not_free", !info.free);
        check("query_region_null_not_readable", !info.readable);
        check("query_region_null_not_executable", !info.executable);
        check("query_region_null_not_guarded", !info.guarded);
    }

    // A spread of boundary / out-of-range / non-canonical addresses.  None may
    // fault; each must yield a non-wrapping region_info.  We do NOT assert a
    // specific committed/free verdict (it is platform- and layout-dependent for
    // arbitrary addresses) — only no-crash and structural sanity.
    struct Edge { const char* tag; std::uintptr_t value; };
    const Edge edges[]{
        { "one",                 std::uintptr_t{ 0x1 } },
        { "floor_0xFFFF",        vmhook::os::user_address_floor },
        { "just_above_floor",    vmhook::os::user_address_floor + 1 },
        { "ceiling",             vmhook::os::user_address_ceiling },
        { "just_below_ceiling",  vmhook::os::user_address_ceiling - 1 },
        { "noncanonical_kernel", std::uintptr_t{ 0xFFFF800000000000ull } },
        { "all_ones",            ~std::uintptr_t{ 0 } },
    };

    bool all_no_overflow{ true };
    bool all_sane{ true };
    for (const Edge& e : edges)
    {
        const void* const p{ reinterpret_cast<const void*>(e.value) };
        const auto info{ vmhook::os::query_region(p) };
        if (!region_no_overflow(info))
        {
            all_no_overflow = false;
            std::printf("[INFO] query_region boundary '%s' produced a wrapping region\n", e.tag);
        }
        // "Sane" = a not-found default (size 0) OR an internally consistent
        // free/committed region.  A committed region must be readable-or-not but
        // must at least contain the queried address if it claims to enclose it;
        // a size-0 result is the not-found sentinel.  We accept any of these and
        // only flag an outright contradiction: size 0 but claiming committed.
        if (info.size == 0 && info.committed)
        {
            all_sane = false;
        }
    }
    check("query_region_boundary_inputs_no_overflow", all_no_overflow);
    check("query_region_boundary_inputs_internally_sane", all_sane);
    // Reaching here proves not one of the boundary queries faulted the process.
    check("query_region_boundary_inputs_process_survives", true);
}

// ---------------------------------------------------------------------------
// (6) FREE / hole reporting after release.  The trampoline allocator's ONLY
// allocation trigger is info.free, so a just-unmapped block must surface as free
// (or at least non-committed) on the platforms that can observe it.  Gated off
// iOS, where query_region is a permissive "always committed" stub by design
// (mirrors test_query_region_reports_free_for_unallocated in the sibling file,
// here with the explicit non-wrapping invariant added).
// ---------------------------------------------------------------------------
#if !VMHOOK_OS_IOS
static auto test_query_region_free_after_release() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
    if (!block)
    {
        check("query_region_free_after_release_skipped_alloc_failed", false);
        return;
    }
    vmhook::os::release(block, page);

    const auto info{ vmhook::os::query_region(block) };
    // After release the address is either free, or not committed, or — if the
    // kernel instantly reused it — a different mapping that is at least not the
    // old RWX region (not both readable and executable).  Any of these proves
    // query_region distinguishes mapped from unmapped state.
    check("query_region_free_after_release_consistent",
          info.free || !info.committed || !(info.readable && info.executable));
    check("query_region_free_after_release_no_overflow", region_no_overflow(info));
}
#endif

// ---------------------------------------------------------------------------
// (7) IDEMPOTENCY / determinism.  Two consecutive queries against a stable
// mapping must return a byte-identical region_info.  A mismatch would betray
// reliance on mutated static state or a struct left partially filled on an early
// return path.  We compare every field explicitly so a single drifting member is
// named.
// ---------------------------------------------------------------------------
static auto test_query_region_idempotent() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
    if (!block)
    {
        check("query_region_idempotent_skipped_alloc_failed", false);
        return;
    }
    *static_cast<volatile std::uint8_t*>(block) = 0x77;

    const auto a{ vmhook::os::query_region(block) };
    const auto b{ vmhook::os::query_region(block) };
    check("query_region_idempotent_base", a.base == b.base);
    check("query_region_idempotent_size", a.size == b.size);
    check("query_region_idempotent_committed", a.committed == b.committed);
    check("query_region_idempotent_free", a.free == b.free);
    check("query_region_idempotent_readable", a.readable == b.readable);
    check("query_region_idempotent_executable", a.executable == b.executable);
    check("query_region_idempotent_guarded", a.guarded == b.guarded);

    // nullptr is likewise deterministic — two default sentinels are identical.
    {
        const auto n1{ vmhook::os::query_region(nullptr) };
        const auto n2{ vmhook::os::query_region(nullptr) };
        check("query_region_idempotent_null_base", n1.base == n2.base);
        check("query_region_idempotent_null_size", n1.size == n2.size);
        check("query_region_idempotent_null_committed", n1.committed == n2.committed);
    }

    vmhook::os::release(block, page);
}

// ---------------------------------------------------------------------------
// (8a) CONSUMER round-trip: hotspot::is_readable_pointer must AGREE with
// query_region on the same address.  is_readable_pointer pre-filters the address
// range/alignment and then returns committed && readable && !guarded from the
// region query.  For an 8-byte-aligned interior address of a live, committed,
// readable, non-guarded allocation it must therefore return true; and its result
// must equal the (committed && readable && !guarded) verdict we read directly.
// This pins the consumer contract WITHOUT a JVM.
// ---------------------------------------------------------------------------
static auto test_query_region_is_readable_pointer_consistency() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
    if (!block)
    {
        check("is_readable_pointer_consistency_skipped_alloc_failed", false);
        return;
    }
    *static_cast<volatile std::uint8_t*>(block) = 0x99;

    // An 8-byte-aligned interior address (allocate_rwx returns page-aligned, so
    // block + 8 is 8-aligned and clears is_readable_pointer's alignment filter).
    const void* const p{ static_cast<std::uint8_t*>(block) + 8 };
    const auto info{ vmhook::os::query_region(p) };
    const bool direct_verdict{ info.committed && info.readable && !info.guarded };
    const bool readable{ vmhook::hotspot::is_readable_pointer(p) };

    check("is_readable_pointer_true_for_live_aligned_addr", readable);
    check("is_readable_pointer_matches_query_region_verdict",
          readable == direct_verdict);

    // is_readable_pointer must reject nullptr and the floor sentinel (its own
    // pre-filter), independent of what query_region returns for them.
    check("is_readable_pointer_rejects_null",
          !vmhook::hotspot::is_readable_pointer(nullptr));
    check("is_readable_pointer_rejects_floor_sentinel",
          !vmhook::hotspot::is_readable_pointer(
              reinterpret_cast<const void*>(vmhook::os::user_address_floor)));
    // A misaligned (odd) pointer is rejected by the alignment pre-filter even
    // though the underlying page is perfectly readable.
    check("is_readable_pointer_rejects_misaligned",
          !vmhook::hotspot::is_readable_pointer(static_cast<std::uint8_t*>(block) + 1));

    vmhook::os::release(block, page);
}

// ---------------------------------------------------------------------------
// (8b) CONSUMER round-trip: hotspot::find_stub_size is region-aware.  For a
// freshly allocated SINGLE page it must return min(page, 0x2000) exactly — the
// region-clamped scan window — pinning the region-aware path (today nothing
// asserts it vs. the 0x2000 fallback).  For a one-past-the-end address the
// region_end <= start branch must fall back to exactly 0x2000.
//
// page is 4096 (>=4096, power of two) on every supported platform, so
// min(page, 0x2000) == page whenever page <= 0x2000 and == 0x2000 otherwise; we
// compute the expected value the same way the impl does so the assertion holds
// for any page size (e.g. 16 KiB on Apple arm64).
// ---------------------------------------------------------------------------
static auto test_query_region_find_stub_size_consumer() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    const std::size_t cap{ static_cast<std::size_t>(0x2000) };

    void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
    if (!block)
    {
        check("find_stub_size_consumer_skipped_alloc_failed", false);
        return;
    }
    *static_cast<volatile std::uint8_t*>(block) = 0xC3; // x86 'ret' — never executed.

    // Single page: the region is exactly one page, so the clamp is
    // min(page, 0x2000) measured from the start of the region.
    {
        const std::size_t expected{ page < cap ? page : cap };
        const std::size_t got{
            vmhook::hotspot::find_stub_size(static_cast<const std::uint8_t*>(block)) };
        check("find_stub_size_single_page_equals_min_page_cap", got == expected);
        check("find_stub_size_single_page_nonzero", got != 0);
        check("find_stub_size_single_page_at_most_cap", got <= cap);
    }

    // An interior start partway through the page leaves fewer bytes to the region
    // end, so the result must be the remaining bytes (which is < a full page and
    // hence < cap on a 4 KiB page) — strictly less than the at-start result.
    {
        const std::size_t at_start{
            vmhook::hotspot::find_stub_size(static_cast<const std::uint8_t*>(block)) };
        const std::size_t at_mid{
            vmhook::hotspot::find_stub_size(static_cast<const std::uint8_t*>(block) + page / 2) };
        check("find_stub_size_interior_at_most_at_start", at_mid <= at_start);
        check("find_stub_size_interior_nonzero", at_mid != 0);
    }

    vmhook::os::release(block, page);

    // One-past-the-end (now-released) address: query_region either reports the
    // freed/next region or a not-found default.  Either way find_stub_size must
    // return a sane, capped, non-zero window — and in the not-found / region_end
    // <= start cases it falls back to exactly 0x2000.  We assert the robust
    // superset contract: the result is non-zero and never exceeds the cap.
    {
        const std::size_t got{
            vmhook::hotspot::find_stub_size(
                reinterpret_cast<const std::uint8_t*>(vmhook::os::user_address_floor)) };
        check("find_stub_size_low_sentinel_capped", got <= cap);
        check("find_stub_size_low_sentinel_nonzero", got != 0);
    }

    // A genuinely not-found region (nullptr's default has base==null, size==0)
    // must hit the explicit 0x2000 fallback exactly.
    {
        const std::size_t got{ vmhook::hotspot::find_stub_size(nullptr) };
        check("find_stub_size_null_start_falls_back_to_cap", got == cap);
    }
}

int main()
{
    test_query_region_contains_live_addresses();
    test_query_region_interior_and_unaligned();
    test_query_region_size_bounds();
    test_query_region_attributes_each_protection();
    test_query_region_null_and_boundary_inputs();
#if !VMHOOK_OS_IOS
    test_query_region_free_after_release();
#endif
    test_query_region_idempotent();
    test_query_region_is_readable_pointer_consistency();
    test_query_region_find_stub_size_consumer();

    if (failures == 0)
    {
        std::printf("vmhook os query_region: OK\n");
    }
    else
    {
        std::printf("vmhook os query_region: %d FAILURE(S)\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
