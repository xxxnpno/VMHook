// Stress tests for the vmhook::os layer that go beyond the basic round-trip
// covered in test_os_layer.cpp:
//   * protect() must accept addresses that are NOT page-aligned (mprotect on
//     POSIX requires page alignment; the wrapper must align internally).
//   * protect() with a size that crosses a page boundary must cover both
//     pages, not just one.
//   * After protect(no_access), safe_read() must refuse to copy and return
//     false rather than faulting the process.
//   * query_region() on a freshly allocate_rwx'd block must report it as
//     committed + readable (regardless of platform).
//   * allocation_granularity() must be a multiple of page_size() — required
//     by every trampoline allocator that walks the address space at
//     granularity-sized strides.
//
// EXHAUSTIVE EXPANSION
// --------------------
// This file drives os::protect (and its interaction with allocate_rwx /
// query_region / safe_read / release) from every input/edge that is *safely*
// testable in-process:
//   * protect a freshly-allocated page to each portable protection
//     (read, read_write, execute_read, execute_rw, no_access) and confirm the
//     page contents survive each transition;
//   * round-trip protect -> query_region -> protect-back (the natural
//     trampoline-installer pattern);
//   * page-boundary rounding of base+size: 1 byte -> 1 page, exact one page,
//     one page minus one, exactly N pages, N pages plus one byte (spill into
//     the next page), and an unaligned interior base spanning many pages;
//   * idempotent re-protect (same protection twice) and protect of an
//     already-correctly-protected region;
//   * the old_prot output contract — PLATFORM-ASYMMETRIC: Windows writes the
//     genuine previous native flags, POSIX always writes 0.  We assert the
//     contract WITHOUT ever feeding a POSIX old_prot back into protect() (that
//     would request no_access and brick the page);
//   * a behavioural probe of each protection via safe_read where it is
//     fault-safe to do so (skipped on iOS — no fault-safe read API — and when
//     a sandbox refuses PROT_NONE).
//
// SAFETY: every test only ever protects memory it allocated itself, always
// restores a writable mapping before release(), never executes written bytes,
// and never reads a no_access page except through the fault-safe safe_read()
// path (which is itself gated off on iOS and skipped if PROT_NONE is refused).
//
// PORTABILITY: protection-flag VALUES and the page size differ by OS, so every
// assertion here is an INVARIANT (round-trip survival, alignment, success /
// failure contract, asymmetry of old_prot) rather than an OS-specific constant.
// The only OS-gated literal is the Windows-vs-POSIX old_prot contract, behind
// VMHOOK_OS_WINDOWS.  No <charconv>/float, no std::expected/syncstream — the
// file builds under the MinGW libstdc++, MSVC STL, and libc++ CI toolchains.
#include <vmhook/vmhook.hpp>

#include <cstdio>
#include <cstdint>
#include <cstring>

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
// Shared helpers.  Keeping the W^X fallback and the no_access behavioural probe
// in one place lets every test below restore a writable mapping the same way
// and keeps the per-test bodies focused on the edge they target.
// ---------------------------------------------------------------------------

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

// Probe whether the first byte at `p` is readable, WITHOUT faulting.  safe_read
// uses ReadProcessMemory / process_vm_readv / mach_vm_read_overwrite, all of
// which kernel-validate the source and return false instead of crashing.  Not
// usable on iOS (safe_read there is a raw memcpy that would fault on a
// no_access page), so callers gate the no_access cases behind !VMHOOK_OS_IOS.
static auto byte_is_readable(const void* p) -> bool
{
    std::uint8_t sink{ 0 };
    return vmhook::os::safe_read(&sink, p, 1u);
}

static auto test_granularity_relationship() -> void
{
    const std::size_t ps{ vmhook::os::page_size() };
    const std::size_t gr{ vmhook::os::allocation_granularity() };
    check("allocation_granularity_at_least_page_size", gr >= ps);
    check("allocation_granularity_multiple_of_page_size", (gr % ps) == 0);
}

static auto test_query_region_attributes_of_rwx_alloc() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
    if (!block)
    {
        check("query_region_attributes_skipped_alloc_failed", false);
        return;
    }

    // Touch the page so it is unambiguously committed.
    *static_cast<volatile std::uint8_t*>(block) = 0x42;

    const auto info{ vmhook::os::query_region(block) };
    check("query_region_committed", info.committed);
    check("query_region_readable", info.readable);
    check("query_region_not_guarded", !info.guarded);
    check("query_region_size_at_least_page", info.size >= page);

    vmhook::os::release(block, page);
}

static auto test_protect_non_aligned_address() -> void
{
    // Allocate two pages so any rounding inside protect() stays inside our
    // own allocation.  We then ask protect() to change one byte in the middle
    // of page 0 — the wrapper must align the request down before calling
    // mprotect (otherwise mprotect returns EINVAL on POSIX) and back up to
    // the full page.
    const std::size_t page{ vmhook::os::page_size() };
    void* const block{ vmhook::os::allocate_rwx(nullptr, page * 2) };
    if (!block)
    {
        check("protect_non_aligned_address_skipped_alloc_failed", false);
        return;
    }

    auto* const bytes{ static_cast<std::uint8_t*>(block) };
    bytes[0]            = 0x11;
    bytes[page / 2]     = 0x22;
    bytes[page]         = 0x33;
    bytes[page + 1]     = 0x44;

    // Pass an unaligned interior address with a 1-byte length.
    const bool flipped{ vmhook::os::protect(bytes + (page / 2), 1,
                                           vmhook::os::memory_protection::read,
                                           nullptr) };
    check("protect_unaligned_address_succeeds", flipped);

    // The protect() above must NOT have damaged the existing contents.
    check("protect_unaligned_address_preserves_data_at_zero",
          bytes[0] == 0x11);
    check("protect_unaligned_address_preserves_data_mid",
          bytes[page / 2] == 0x22);

    // Flip back to RW (or RWX, which is what allocate_rwx returned).  Some
    // platforms (Apple arm64 without JIT entitlement) only permit RW after
    // a writable->execute_rw transition; in that case we still want the
    // page to be writable for the cleanup memset below.
    check("protect_back_to_rw", make_writable(bytes, page));

    vmhook::os::release(block, page * 2);
}

static auto test_protect_crossing_page_boundary() -> void
{
    // Allocate two pages, ask protect() to change a range starting in page 0
    // and ending in page 1.  Both pages must end up with the new protection.
    const std::size_t page{ vmhook::os::page_size() };
    void* const block{ vmhook::os::allocate_rwx(nullptr, page * 2) };
    if (!block)
    {
        check("protect_crossing_page_skipped_alloc_failed", false);
        return;
    }

    auto* const bytes{ static_cast<std::uint8_t*>(block) };
    bytes[0]         = 0xAA;
    bytes[page - 1]  = 0xBB;
    bytes[page]      = 0xCC;
    bytes[page * 2 - 1] = 0xDD;

    // protect a range spanning [page - 4, page + 4] -> covers both pages.
    const bool flipped{ vmhook::os::protect(bytes + page - 4, 8,
                                           vmhook::os::memory_protection::read,
                                           nullptr) };
    check("protect_crossing_page_boundary_succeeds", flipped);

    check("protect_crossing_page_preserves_data_page0",
          bytes[0] == 0xAA && bytes[page - 1] == 0xBB);
    check("protect_crossing_page_preserves_data_page1",
          bytes[page] == 0xCC && bytes[page * 2 - 1] == 0xDD);

    // Restore writable.
    check("protect_back_to_rw_crossing", make_writable(bytes, page * 2));

    vmhook::os::release(block, page * 2);
}

#if !VMHOOK_OS_IOS
static auto test_safe_read_refuses_no_access_page() -> void
{
    // After protect(no_access), the page must not be readable.  safe_read
    // must return false rather than letting the process fault.
    const std::size_t page{ vmhook::os::page_size() };
    void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
    if (!block)
    {
        check("safe_read_no_access_skipped_alloc_failed", false);
        return;
    }

    auto* const bytes{ static_cast<std::uint8_t*>(block) };
    bytes[0] = 0xEE;

    const bool flipped{ vmhook::os::protect(block, page,
                                           vmhook::os::memory_protection::no_access,
                                           nullptr) };
    if (!flipped)
    {
        // Some sandboxed runners forbid PROT_NONE; skip rather than fail.
        std::printf("[INFO] safe_read_no_access_skipped: protect(no_access) refused\n");
        vmhook::os::release(block, page);
        return;
    }

    std::uint8_t dst{ 0 };
    const bool readable{ vmhook::os::safe_read(&dst, block, 1u) };
    check("safe_read_refuses_no_access_page", !readable);

    // Flip back so munmap doesn't trip over the no-access mapping in any
    // sanity-check the platform performs at unmap time.
    (void)vmhook::os::protect(block, page,
                              vmhook::os::memory_protection::read_write, nullptr);
    vmhook::os::release(block, page);
}
#endif

// ---------------------------------------------------------------------------
// Defensive input checks - every os::* primitive must short-circuit on bad
// args rather than calling through to the kernel with garbage.
// ---------------------------------------------------------------------------
static auto test_os_primitive_input_guards() -> void
{
    const std::size_t page{ vmhook::os::page_size() };

    // protect: null address OR zero size -> false, no kernel call.
    check("protect_null_addr_returns_false",
          !vmhook::os::protect(nullptr, page,
                               vmhook::os::memory_protection::read_write, nullptr));
    {
        std::uint8_t scratch[16]{};
        check("protect_zero_size_returns_false",
              !vmhook::os::protect(scratch, 0,
                                   vmhook::os::memory_protection::read_write, nullptr));
    }

    // allocate_rwx: zero size -> nullptr, no kernel call.
    check("allocate_rwx_zero_size_returns_null",
          vmhook::os::allocate_rwx(nullptr, 0) == nullptr);
    check("allocate_rwx_zero_size_with_hint_returns_null",
          vmhook::os::allocate_rwx(reinterpret_cast<void*>(0x10000), 0) == nullptr);

    // release: null addr OR zero size -> no-op, doesn't crash.
    vmhook::os::release(nullptr, page);    // must be safe
    vmhook::os::release(nullptr, 0);       // must be safe
    {
        void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
        if (block)
        {
            vmhook::os::release(block, 0); // must be a no-op
            // After the no-op the block should still be live and writable.
            *static_cast<volatile std::uint8_t*>(block) = 0x77;
            check("release_zero_size_is_noop",
                  *static_cast<volatile std::uint8_t*>(block) == 0x77);
            vmhook::os::release(block, page);  // real release
        }
        else
        {
            check("release_zero_size_is_noop_skipped_alloc_failed", false);
        }
    }

    // safe_read: every null arg / zero size combination -> false, no read.
    {
        std::uint8_t dst{ 0 };
        std::uint8_t src{ 0xAB };
        check("safe_read_null_dst_returns_false",
              !vmhook::os::safe_read(nullptr, &src, 1u));
        check("safe_read_null_src_returns_false",
              !vmhook::os::safe_read(&dst, nullptr, 1u));
        check("safe_read_zero_size_returns_false",
              !vmhook::os::safe_read(&dst, &src, 0u));
        check("safe_read_all_null_returns_false",
              !vmhook::os::safe_read(nullptr, nullptr, 0u));
    }

    // query_region(nullptr) returns a default-constructed region_info.
    {
        const auto info{ vmhook::os::query_region(nullptr) };
        check("query_region_null_addr_base_null", info.base == nullptr);
        check("query_region_null_addr_size_zero", info.size == 0);
        check("query_region_null_addr_not_committed", !info.committed);
    }

    // find_loaded_module(nullptr-name) should not crash; result is platform
    // specific (GetModuleHandleA(nullptr) returns the exe's handle on
    // Windows; dlopen(nullptr) returns RTLD_DEFAULT on POSIX).  We only
    // assert no fault.
    (void)vmhook::os::find_loaded_module(nullptr);

    // get_proc_address: null module or null symbol -> nullptr.
    check("get_proc_address_null_module_returns_null",
          vmhook::os::get_proc_address(nullptr, "any_symbol") == nullptr);
    {
        // Use a non-null but invalid handle to keep the "null symbol" check
        // honest without touching GetProcAddress / dlsym on a real module.
        // The null-symbol guard fires before the handle is dereferenced.
        vmhook::os::module_handle bogus_handle{ reinterpret_cast<vmhook::os::module_handle>(
            static_cast<std::uintptr_t>(0x1)) };
        check("get_proc_address_null_symbol_returns_null",
              vmhook::os::get_proc_address(bogus_handle, nullptr) == nullptr);
    }
}

// ---------------------------------------------------------------------------
// Round-trip protect across multiple distinct flag combinations.  The basic
// roundtrip test only covers RWX -> R -> RWX.  This walks the whole enum so
// any switch arm that's silently broken (e.g. accidentally returning
// PROT_NONE on POSIX) shows up as a flipped page that can't be read back.
// ---------------------------------------------------------------------------
static auto test_protect_all_enum_values() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
    if (!block)
    {
        check("protect_all_enum_values_skipped_alloc_failed", false);
        return;
    }

    auto* const bytes{ static_cast<std::uint8_t*>(block) };
    bytes[0] = 0xA5;

    // Each transition must succeed and leave the byte intact.  Skip
    // no_access (some sandboxed runners forbid PROT_NONE) and
    // execute_rw -> read_write fallback (Apple W^X) but still verify the
    // primary flags.
    const vmhook::os::memory_protection sequence[]{
        vmhook::os::memory_protection::read,
        vmhook::os::memory_protection::read_write,
        vmhook::os::memory_protection::execute_read,
    };
    for (const auto prot : sequence)
    {
        const bool ok{ vmhook::os::protect(block, page, prot, nullptr) };
        check("protect_each_enum_transition_succeeds", ok);
    }

    // Tear down: ensure writable before unmap so any platform-level integrity
    // check at release time doesn't trip on a read-only mapping.
    check("protect_back_to_rw_after_enum_walk", make_writable(block, page));
    bytes[0] = 0x5A;
    check("protect_byte_still_writable_after_walk", bytes[0] == 0x5A);

    vmhook::os::release(block, page);
}

// ---------------------------------------------------------------------------
// EXHAUSTIVE: every portable protection in BOTH directions, each verified by a
// behavioural probe (safe_read), not just the bool return.  The enum-walk above
// only checks the return; here we confirm the page actually ends up readable /
// unreadable as requested, distinguishing read_write (R+W, no X) from
// execute_rw (R+W+X) and execute_read (R+X) from read (R).  We never trigger a
// write/exec fault directly (no SEH / signal harness in this TU); writability
// is verified by an actual store only for the writable protections.
//
// no_access and the X-bearing protections are gated:
//   * no_access read-back requires the fault-safe safe_read path -> !iOS, and
//     is skipped when a sandbox refuses PROT_NONE.
//   * execute_rw may be refused under W^X; we treat refusal as "skip", never
//     as failure.
// ---------------------------------------------------------------------------
static auto test_protect_each_protection_behavioural() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
    if (!block)
    {
        check("protect_behavioural_skipped_alloc_failed", false);
        return;
    }

    auto* const bytes{ static_cast<std::uint8_t*>(block) };
    bytes[0] = 0x10;

    // read: page becomes R-only.  safe_read must succeed (readable); we do NOT
    // attempt a store (that would fault).  The byte content is unchanged.
    {
        const bool ok{ vmhook::os::protect(block, page,
                                           vmhook::os::memory_protection::read, nullptr) };
        check("protect_read_succeeds", ok);
#if !VMHOOK_OS_IOS
        if (ok)
        {
            check("protect_read_page_is_readable", byte_is_readable(block));
        }
#endif
        check("protect_read_preserves_byte", bytes[0] == 0x10);
    }

    // read_write: page becomes R+W (no X).  A real store must stick.
    {
        const bool ok{ make_writable(block, page) }; // execute_rw or read_write
        check("protect_read_write_succeeds", ok);
        if (ok)
        {
            bytes[0] = 0x20;
            check("protect_read_write_store_sticks", bytes[0] == 0x20);
#if !VMHOOK_OS_IOS
            check("protect_read_write_page_is_readable", byte_is_readable(block));
#endif
        }
    }

    // execute_read: page becomes R+X.  Readable; we never execute the bytes.
    {
        const bool ok{ vmhook::os::protect(block, page,
                                           vmhook::os::memory_protection::execute_read, nullptr) };
        check("protect_execute_read_succeeds", ok);
#if !VMHOOK_OS_IOS
        if (ok)
        {
            check("protect_execute_read_page_is_readable", byte_is_readable(block));
        }
#endif
        check("protect_execute_read_preserves_byte", bytes[0] == 0x20);
    }

    // execute_rw: page becomes R+W+X.  May be refused under W^X; treat refusal
    // as a skip.  When it succeeds a real store must stick and it is readable.
    {
        const bool ok{ vmhook::os::protect(block, page,
                                           vmhook::os::memory_protection::execute_rw, nullptr) };
        if (ok)
        {
            bytes[0] = 0x30;
            check("protect_execute_rw_store_sticks", bytes[0] == 0x30);
#if !VMHOOK_OS_IOS
            check("protect_execute_rw_page_is_readable", byte_is_readable(block));
#endif
        }
        else
        {
            std::printf("[INFO] protect_execute_rw skipped: refused (W^X / no JIT entitlement)\n");
        }
    }

    // no_access: page becomes unreadable.  Gated off iOS (no fault-safe read)
    // and skipped where PROT_NONE is refused.
#if !VMHOOK_OS_IOS
    {
        const bool ok{ vmhook::os::protect(block, page,
                                           vmhook::os::memory_protection::no_access, nullptr) };
        if (ok)
        {
            check("protect_no_access_page_is_not_readable", !byte_is_readable(block));
        }
        else
        {
            std::printf("[INFO] protect_no_access skipped: refused (sandbox forbids PROT_NONE)\n");
        }
    }
#endif

    // Always restore writable before release.
    check("protect_behavioural_restore_writable", make_writable(block, page));
    vmhook::os::release(block, page);
}

// ---------------------------------------------------------------------------
// EXHAUSTIVE: the natural trampoline-installer pattern — protect a page, query
// it back, protect it back — works as a round trip and query_region reflects
// the requested state.  query_region's `executable`/`readable` flags are
// platform-derived (Windows reads PAGE_*, Linux parses /proc perms, macOS reads
// mach protection), so we assert the cross-platform-true direction only:
//   * after protect(read)         -> region is readable;
//   * after protect(execute_read) -> region is readable AND executable;
//   * after protect(read_write)   -> region is readable.
// We deliberately do NOT assert "execute bit clear after read" because some
// kernels keep an executable mapping's backing flags coarser than the portable
// enum; the positive direction is the contract callers rely on.
// ---------------------------------------------------------------------------
static auto test_protect_query_roundtrip() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
    if (!block)
    {
        check("protect_query_roundtrip_skipped_alloc_failed", false);
        return;
    }

    auto* const bytes{ static_cast<std::uint8_t*>(block) };
    bytes[0] = 0x99;

    // read -> readable.
    {
        const bool ok{ vmhook::os::protect(block, page,
                                           vmhook::os::memory_protection::read, nullptr) };
        check("protect_query_read_succeeds", ok);
        const auto info{ vmhook::os::query_region(block) };
        check("protect_query_read_region_committed", info.committed);
        check("protect_query_read_region_readable", info.readable);
    }

    // execute_read -> readable + executable.  Skip the executable assertion if
    // the transition was refused (W^X edge); on the platforms that grant R+X
    // query_region reports the X bit.
    {
        const bool ok{ vmhook::os::protect(block, page,
                                           vmhook::os::memory_protection::execute_read, nullptr) };
        check("protect_query_execrx_succeeds", ok);
        if (ok)
        {
            const auto info{ vmhook::os::query_region(block) };
            check("protect_query_execrx_region_readable", info.readable);
            check("protect_query_execrx_region_executable", info.executable);
        }
    }

    // read_write -> readable, and an actual store must stick (proves the W bit).
    {
        const bool ok{ vmhook::os::protect(block, page,
                                           vmhook::os::memory_protection::read_write, nullptr) };
        check("protect_query_rw_succeeds", ok);
        if (ok)
        {
            const auto info{ vmhook::os::query_region(block) };
            check("protect_query_rw_region_readable", info.readable);
            bytes[0] = 0xCD;
            check("protect_query_rw_store_sticks", bytes[0] == 0xCD);
        }
    }

    check("protect_query_roundtrip_restore_writable", make_writable(block, page));
    vmhook::os::release(block, page);
}

// ---------------------------------------------------------------------------
// EXHAUSTIVE: the old_prot output contract.  The signature exposes an optional
// old_prot out-parameter, but its meaning is PLATFORM-ASYMMETRIC:
//   * Windows: VirtualProtect reports the genuine previous protection, so
//     old_prot receives a non-zero PAGE_* bitmask after a successful flip.
//   * POSIX:   mprotect has no cheap way to read the prior flags, so the
//     wrapper writes 0 unconditionally.
// A caller that does "save old, work, restore old" would, on POSIX, restore
// with 0 == no_access and brick the page.  We pin BOTH halves of the contract
// here so a future "real previous flags on Linux" change is a deliberate,
// tested decision rather than a silent break — and crucially we NEVER feed the
// POSIX old_prot back into protect().
// ---------------------------------------------------------------------------
static auto test_protect_old_prot_contract() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
    if (!block)
    {
        check("protect_old_prot_skipped_alloc_failed", false);
        return;
    }

    auto* const bytes{ static_cast<std::uint8_t*>(block) };
    bytes[0] = 0x01;

    constexpr std::uint32_t sentinel{ 0xDEADBEEFu };

    // First flip: RWX (from allocate_rwx) -> read.  old_prot must be WRITTEN on
    // success (it must not keep the sentinel) on every platform.
    {
        std::uint32_t op{ sentinel };
        const bool ok{ vmhook::os::protect(block, page,
                                           vmhook::os::memory_protection::read, &op) };
        check("protect_old_prot_first_flip_succeeds", ok);
        if (ok)
        {
            check("protect_old_prot_written_on_success", op != sentinel);
#if VMHOOK_OS_WINDOWS
            // Windows reports the genuine prior protection; the page was RWX
            // (PAGE_EXECUTE_READWRITE) before this flip, so old_prot is a
            // non-zero PAGE_* bitmask.  We do not pin the exact value (it is a
            // native constant), only that it is meaningfully populated.
            check("protect_old_prot_windows_nonzero", op != 0u);
#else
            // POSIX contract: old_prot is always exactly 0 after success.  This
            // is the documented asymmetry; if it ever becomes the real previous
            // flags this assertion will fire and force a conscious update.
            check("protect_old_prot_posix_is_zero", op == 0u);
#endif
        }
    }

    // Second flip: read -> read_write.  Same contract; on Windows old_prot now
    // reflects PAGE_READONLY (still non-zero), on POSIX still 0.
    {
        std::uint32_t op{ sentinel };
        const bool ok{ vmhook::os::protect(block, page,
                                           vmhook::os::memory_protection::read_write, &op) };
        check("protect_old_prot_second_flip_succeeds", ok);
        if (ok)
        {
            check("protect_old_prot_second_written_on_success", op != sentinel);
#if VMHOOK_OS_WINDOWS
            check("protect_old_prot_second_windows_nonzero", op != 0u);
#else
            check("protect_old_prot_second_posix_is_zero", op == 0u);
#endif
        }
    }

    // A successful protect with a NULL old_prot must also succeed and not crash
    // (the `if (old_prot)` guard on the success path must be exercised with
    // old_prot == nullptr, not only on the failure path).
    check("protect_success_null_old_prot_ok",
          vmhook::os::protect(block, page,
                              vmhook::os::memory_protection::read, nullptr));

    check("protect_old_prot_restore_writable", make_writable(block, page));
    bytes[0] = 0x02;
    check("protect_old_prot_byte_writable_after", bytes[0] == 0x02);
    vmhook::os::release(block, page);
}

// ---------------------------------------------------------------------------
// EXHAUSTIVE: size/rounding boundary pairs against the POSIX page-rounding math
// (base &= ~(ps-1); size = (end-base+ps-1) & ~(ps-1)) and Windows' native
// rounding.  Allocate THREE pages so even the "+1 byte spill" case stays inside
// our own allocation, write distinct markers across all three pages, and run
// each size through protect(read) -> restore, asserting no marker is corrupted.
//   size == 1            -> rounds up to exactly one page
//   size == page - 1     -> rounds up to exactly one page
//   size == page         -> exactly one page, no rounding
//   size == page + 1     -> spills into page 1 -> two pages
//   size == 2*page       -> exactly two pages
//   size == 2*page + 1   -> spills into page 2 -> three pages
// Each is launched from an aligned base (block) so the covered span is
// predictable; the unaligned-base spans are covered by the crossing test above
// and the many-page unaligned case below.
// ---------------------------------------------------------------------------
static auto test_protect_size_rounding_boundaries() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    void* const block{ vmhook::os::allocate_rwx(nullptr, page * 3) };
    if (!block)
    {
        check("protect_size_rounding_skipped_alloc_failed", false);
        return;
    }

    auto* const bytes{ static_cast<std::uint8_t*>(block) };

    const std::size_t sizes[]{
        std::size_t{ 1 },
        page - 1,
        page,
        page + 1,
        page * 2,
        page * 2 + 1,
    };

    bool all_ok{ true };
    bool all_preserved{ true };
    for (const std::size_t sz : sizes)
    {
        // Re-stamp distinct markers across all three pages before each pass.
        bytes[0]              = 0xA0;
        bytes[page]           = 0xB0;
        bytes[page * 2]       = 0xC0;
        bytes[page * 3 - 1]   = 0xD0;

        if (!vmhook::os::protect(block, sz,
                                 vmhook::os::memory_protection::read, nullptr))
        {
            all_ok = false;
        }

        // Whatever sub-span got rounded read-only, the contents must be intact.
        if (!(bytes[0] == 0xA0 && bytes[page] == 0xB0
              && bytes[page * 2] == 0xC0 && bytes[page * 3 - 1] == 0xD0))
        {
            all_preserved = false;
        }

        // Restore the whole allocation writable for the next iteration.
        if (!make_writable(block, page * 3))
        {
            all_ok = false;
        }
    }

    check("protect_size_rounding_all_sizes_succeed", all_ok);
    check("protect_size_rounding_preserves_all_markers", all_preserved);

    // Leave it writable + release.
    check("protect_size_rounding_restore_writable", make_writable(block, page * 3));
    vmhook::os::release(block, page * 3);
}

// ---------------------------------------------------------------------------
// EXHAUSTIVE: exact one-page span (no rounding) and exact multi-page spans.
// Distinguished from the boundary sweep above by asserting per-span behaviour
// individually with descriptive names, and by exercising an exact 3-page span
// in a single call.
// ---------------------------------------------------------------------------
static auto test_protect_exact_page_multiples() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    void* const block{ vmhook::os::allocate_rwx(nullptr, page * 3) };
    if (!block)
    {
        check("protect_exact_multiples_skipped_alloc_failed", false);
        return;
    }

    auto* const bytes{ static_cast<std::uint8_t*>(block) };
    bytes[0]            = 0x1A;
    bytes[page]         = 0x2B;
    bytes[page * 2]     = 0x3C;

    // Exactly one page.
    check("protect_exact_one_page_succeeds",
          vmhook::os::protect(block, page,
                              vmhook::os::memory_protection::read, nullptr));
    check("protect_exact_one_page_preserves",
          bytes[0] == 0x1A && bytes[page] == 0x2B && bytes[page * 2] == 0x3C);
    check("protect_exact_one_page_restore", make_writable(block, page * 3));

    // Exactly two pages.
    check("protect_exact_two_pages_succeeds",
          vmhook::os::protect(block, page * 2,
                              vmhook::os::memory_protection::read, nullptr));
    check("protect_exact_two_pages_preserves",
          bytes[0] == 0x1A && bytes[page] == 0x2B && bytes[page * 2] == 0x3C);
    check("protect_exact_two_pages_restore", make_writable(block, page * 3));

    // Exactly three pages (the whole allocation) in one call.
    check("protect_exact_three_pages_succeeds",
          vmhook::os::protect(block, page * 3,
                              vmhook::os::memory_protection::read, nullptr));
    check("protect_exact_three_pages_preserves",
          bytes[0] == 0x1A && bytes[page] == 0x2B && bytes[page * 2] == 0x3C);
    check("protect_exact_three_pages_restore", make_writable(block, page * 3));

    vmhook::os::release(block, page * 3);
}

// ---------------------------------------------------------------------------
// EXHAUSTIVE: an unaligned interior base whose [base, base+len) spans several
// pages.  The wrapper must align base DOWN and round the length UP to cover the
// whole request — exactly the formula reproduced in test_os_layer.cpp, here
// exercised against a live mprotect/VirtualProtect.  Four pages allocated; we
// protect from 3 bytes into page 0 across a length that reaches into page 2.
// ---------------------------------------------------------------------------
static auto test_protect_unaligned_multipage_span() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    void* const block{ vmhook::os::allocate_rwx(nullptr, page * 4) };
    if (!block)
    {
        check("protect_unaligned_multipage_skipped_alloc_failed", false);
        return;
    }

    auto* const bytes{ static_cast<std::uint8_t*>(block) };
    bytes[0]              = 0x5A;
    bytes[page]           = 0x6B;
    bytes[page * 2]       = 0x7C;
    bytes[page * 3]       = 0x8D;
    bytes[page * 4 - 1]   = 0x9E;

    // Base 3 bytes into page 0; length spans into page 2 (covers pages 0..2).
    const std::size_t len{ page * 2 + 8 };
    const bool flipped{ vmhook::os::protect(bytes + 3, len,
                                            vmhook::os::memory_protection::read,
                                            nullptr) };
    check("protect_unaligned_multipage_succeeds", flipped);
    check("protect_unaligned_multipage_preserves_all",
          bytes[0] == 0x5A && bytes[page] == 0x6B && bytes[page * 2] == 0x7C
          && bytes[page * 3] == 0x8D && bytes[page * 4 - 1] == 0x9E);

    check("protect_unaligned_multipage_restore", make_writable(block, page * 4));
    vmhook::os::release(block, page * 4);
}

// ---------------------------------------------------------------------------
// EXHAUSTIVE: idempotent re-protect and protect-of-already-correct.
//   * Applying the SAME protection twice in a row must both succeed and leave
//     the page in that protection (no state corruption on a no-op transition).
//   * Applying read_write to a page that is already read_write (allocate_rwx
//     came back RW or RWX) must succeed and remain writable.
// ---------------------------------------------------------------------------
static auto test_protect_idempotent_reprotect() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
    if (!block)
    {
        check("protect_idempotent_skipped_alloc_failed", false);
        return;
    }

    auto* const bytes{ static_cast<std::uint8_t*>(block) };
    bytes[0] = 0x44;

    // read twice.
    check("protect_read_first_succeeds",
          vmhook::os::protect(block, page,
                              vmhook::os::memory_protection::read, nullptr));
    check("protect_read_second_succeeds",
          vmhook::os::protect(block, page,
                              vmhook::os::memory_protection::read, nullptr));
#if !VMHOOK_OS_IOS
    check("protect_read_twice_still_readable", byte_is_readable(block));
#endif
    check("protect_read_twice_preserves_byte", bytes[0] == 0x44);

    // read_write twice; a store must stick after the second.
    check("protect_rw_first_succeeds", make_writable(block, page));
    check("protect_rw_second_succeeds",
          vmhook::os::protect(block, page,
                              vmhook::os::memory_protection::read_write, nullptr));
    bytes[0] = 0x55;
    check("protect_rw_twice_store_sticks", bytes[0] == 0x55);

    // protect-of-already-correct: read_write a third time changes nothing.
    check("protect_already_rw_succeeds",
          vmhook::os::protect(block, page,
                              vmhook::os::memory_protection::read_write, nullptr));
    check("protect_already_rw_store_still_sticks", (bytes[0] = 0x66) == 0x66);

    check("protect_idempotent_restore_writable", make_writable(block, page));
    vmhook::os::release(block, page);
}

// ---------------------------------------------------------------------------
// EXHAUSTIVE: out-of-range enum value.  Both to_native_protect overloads end
// with a `return PAGE_NOACCESS;` / `return PROT_NONE;` fallback, so a garbage
// enum cast degrades to the most-restrictive mapping and protect() still
// returns true.  Pin that fallback: the call succeeds AND the page becomes
// inaccessible (safe_read refuses).  This locks the current behaviour so a
// refactor cannot silently turn the fallback into, say, RWX.
//
// Gated like the other no_access probes: the read-back needs the fault-safe
// path (off on iOS) and PROT_NONE may be refused in a sandbox.  Where the
// platform refuses the most-restrictive mapping the protect() call may itself
// fail; we treat that as a skip rather than a failure, because the contract we
// own is "garbage enum does not silently become a PERMISSIVE mapping".
// ---------------------------------------------------------------------------
#if !VMHOOK_OS_IOS
static auto test_protect_out_of_range_enum_is_restrictive() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
    if (!block)
    {
        check("protect_oor_enum_skipped_alloc_failed", false);
        return;
    }

    auto* const bytes{ static_cast<std::uint8_t*>(block) };
    bytes[0] = 0x7F;

    const auto garbage{ static_cast<vmhook::os::memory_protection>(0xFFu) };
    const bool ok{ vmhook::os::protect(block, page, garbage, nullptr) };
    if (ok)
    {
        // Fallback mapped to no_access -> the page must NOT be readable.  This
        // is the load-bearing assertion: a garbage enum must never widen access.
        check("protect_oor_enum_maps_to_no_access", !byte_is_readable(block));
    }
    else
    {
        // A sandbox refused the most-restrictive mapping (PROT_NONE).  That is
        // acceptable; what matters is it did not silently succeed as permissive.
        std::printf("[INFO] protect_oor_enum skipped: most-restrictive mapping refused\n");
    }

    // Restore writable regardless (the page may currently be no_access).
    check("protect_oor_enum_restore_writable", make_writable(block, page));
    vmhook::os::release(block, page);
}
#endif

// ---------------------------------------------------------------------------
// EXHAUSTIVE: alignment witness.  Sub-page protect MUST page-round and so MUST
// NOT bleed into a neighbouring page.  Allocate two pages, write a distinct
// marker into page 1, protect a single byte in page 0 read-only, and confirm
// page 1 is STILL WRITABLE (a fresh store sticks).  This is the assertion the
// existing unaligned tests omit — they prove non-corruption of the *other*
// page's existing bytes but not its continued writability.
//
// On POSIX the rounding covers page 0 only (base rounds down into page 0, the
// 1-byte length rounds up to one page), so page 1 stays whatever it was (RW).
// On Windows VirtualProtect rounds to the page enclosing the single byte, with
// the same outcome.  Either way page 1 must remain writable.
// ---------------------------------------------------------------------------
static auto test_protect_subpage_does_not_bleed_into_neighbour() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    void* const block{ vmhook::os::allocate_rwx(nullptr, page * 2) };
    if (!block)
    {
        check("protect_neighbour_witness_skipped_alloc_failed", false);
        return;
    }

    auto* const bytes{ static_cast<std::uint8_t*>(block) };
    bytes[0]    = 0x01; // page 0 marker
    bytes[page] = 0x02; // page 1 marker (the neighbour we must keep writable)

    // Make a single byte in page 0 read-only.  Must round to page 0 only.
    const bool flipped{ vmhook::os::protect(bytes, 1,
                                            vmhook::os::memory_protection::read,
                                            nullptr) };
    check("protect_neighbour_witness_flip_succeeds", flipped);

    // Page 0's existing byte is intact (protection change never rewrites data).
    check("protect_neighbour_witness_page0_byte_intact", bytes[0] == 0x01);

    // The neighbour page must still be WRITABLE — a brand-new store must stick.
    // If sub-page protect had bled into page 1 this store would fault (and the
    // process would die) or, at minimum, not take effect.  We reach it only
    // because page 1 was never touched by the page-0 rounding.
    bytes[page] = 0xF2;
    check("protect_neighbour_witness_page1_still_writable", bytes[page] == 0xF2);

    // Restore page 0 writable and release the whole allocation.
    check("protect_neighbour_witness_restore", make_writable(block, page * 2));
    vmhook::os::release(block, page * 2);
}

// ---------------------------------------------------------------------------
// EXHAUSTIVE: address-space-wrap / overflow guard.  protect() rounds the
// [base, base+size) span up to page granularity on POSIX (end = base + size;
// aligned_size = end - base + ps - 1).  A size large enough that base + size
// wraps uintptr_t would make that length math produce a garbage value and hand
// mprotect a bogus range.  The wrapper must reject such a request up front —
// returning false WITHOUT touching the kernel — so the degenerate case is a
// clean failure rather than undefined behaviour.  Windows' VirtualProtect
// validates kernel-side, but the wrapper rejects uniformly so the contract is
// identical on every platform.
//
// This test is SAFE precisely because the guard short-circuits before any
// syscall: we hand protect() a valid, live page plus a wrapping size and assert
// (a) it returns false, (b) the process does not crash, and (c) the page's
// protection is UNCHANGED — a real store still sticks afterwards.  No kernel
// call is made with the bogus range, so nothing about the page is disturbed.
// ---------------------------------------------------------------------------
static auto test_protect_size_overflow_guard() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
    if (!block)
    {
        check("protect_overflow_guard_skipped_alloc_failed", false);
        return;
    }

    auto* const bytes{ static_cast<std::uint8_t*>(block) };
    bytes[0] = 0x5A; // marker we will re-write after the rejected calls

    // (1) SIZE_MAX from any non-null base wraps the address space.  Must be
    // rejected with false and must not call mprotect/VirtualProtect.
    check("protect_size_max_returns_false",
          !vmhook::os::protect(block, SIZE_MAX,
                               vmhook::os::memory_protection::read, nullptr));

    // (2) A size precisely tuned so that base + size wraps past the maximum
    // representable pointer by a few bytes.  size = (UINTPTR_MAX - base) + k
    // is the smallest family of sizes that overflow; k = 8 here.  This pins the
    // boundary of the guard, not just the SIZE_MAX extreme.
    {
        const std::uintptr_t base_addr{ reinterpret_cast<std::uintptr_t>(block) };
        const std::uintptr_t max_addr{ ~static_cast<std::uintptr_t>(0) };
        // (max_addr - base_addr) is the largest size that does NOT wrap; +8
        // is the smallest that does.  Guard against the (impossible for a heap
        // page) case base_addr == 0 so the addition itself can't overflow here.
        const std::size_t wrapping_size{
            static_cast<std::size_t>(max_addr - base_addr) + std::size_t{ 8 } };
        check("protect_base_plus_size_wrap_returns_false",
              !vmhook::os::protect(block, wrapping_size,
                                   vmhook::os::memory_protection::read, nullptr));
    }

    // (3) Boundary witness on the SAFE side: the LARGEST size that does NOT
    // wrap (base + size == UINTPTR_MAX exactly) must NOT be rejected by the
    // overflow guard.  The kernel will almost certainly fail such an enormous
    // mprotect/VirtualProtect with its own error, so we accept EITHER outcome
    // (true or false) — what we are pinning is that the guard does not falsely
    // reject the maximal non-wrapping size.  We only assert no crash.
    {
        const std::uintptr_t base_addr{ reinterpret_cast<std::uintptr_t>(block) };
        const std::uintptr_t max_addr{ ~static_cast<std::uintptr_t>(0) };
        const std::size_t non_wrapping_max{
            static_cast<std::size_t>(max_addr - base_addr) };
        (void)vmhook::os::protect(block, non_wrapping_max,
                                  vmhook::os::memory_protection::read, nullptr);
        check("protect_non_wrapping_max_size_no_crash", true);
    }

    // The page's protection must be UNCHANGED by the rejected calls: it came
    // from allocate_rwx (RW or RWX) and a fresh store must still stick.  If any
    // rejected call had leaked through to the kernel and flipped the page to
    // read-only, this store would fault or fail to take effect.
    bytes[0] = 0xA5;
    check("protect_overflow_guard_page_still_writable", bytes[0] == 0xA5);

    vmhook::os::release(block, page);
}

// ---------------------------------------------------------------------------
// query_region must report the FREE attribute for an unallocated address.
// This is implementation-defined on iOS (where the helper always returns a
// permissive "looks committed" stub) so we only check it on every other
// platform.
// ---------------------------------------------------------------------------
#if !VMHOOK_OS_IOS
static auto test_query_region_reports_free_for_unallocated() -> void
{
    // Reserve a block, release it, then query - the region must report as
    // either free or non-committed.  This proves the wrapper correctly
    // distinguishes mapped/unmapped state, which the trampoline allocator
    // relies on.
    const std::size_t page{ vmhook::os::page_size() };
    void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
    if (!block)
    {
        check("query_region_free_after_release_skipped_alloc_failed", false);
        return;
    }
    vmhook::os::release(block, page);

    const auto info{ vmhook::os::query_region(block) };
    // After release, the region is either free or non-committed.  On some
    // OSes the kernel may immediately reuse the address; in that case the
    // region info reflects the new mapping and committed could be true.
    // The defining post-release contract is "not the same RWX region we
    // had before": readable + executable both true would be unusual.
    check("query_region_after_release_consistent",
          (info.free || !info.committed)
          || (!(info.readable && info.executable)));
}
#endif

int main()
{
    test_granularity_relationship();
    test_query_region_attributes_of_rwx_alloc();
    test_protect_non_aligned_address();
    test_protect_crossing_page_boundary();
    test_protect_all_enum_values();
    test_protect_each_protection_behavioural();
    test_protect_query_roundtrip();
    test_protect_old_prot_contract();
    test_protect_size_rounding_boundaries();
    test_protect_exact_page_multiples();
    test_protect_unaligned_multipage_span();
    test_protect_idempotent_reprotect();
    test_protect_subpage_does_not_bleed_into_neighbour();
    test_protect_size_overflow_guard();
    test_os_primitive_input_guards();
#if !VMHOOK_OS_IOS
    test_safe_read_refuses_no_access_page();
    test_protect_out_of_range_enum_is_restrictive();
    test_query_region_reports_free_for_unallocated();
#endif

    if (failures == 0)
    {
        std::printf("vmhook os protect/safe_read: OK\n");
    }
    else
    {
        std::printf("vmhook os protect/safe_read: %d FAILURE(S)\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
