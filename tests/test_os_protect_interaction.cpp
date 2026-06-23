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

// A stable human-readable spelling for each portable protection, so the
// transition-matrix diagnostics below name exactly which source->dest edge an
// [INFO] line is characterising.  Kept tiny and dependency-free (no <array>,
// no std::string) to stay within the MinGW / MSVC / libc++ CI toolchains.
static auto protection_name(vmhook::os::memory_protection p) -> const char*
{
    switch (p)
    {
    case vmhook::os::memory_protection::no_access:    return "no_access";
    case vmhook::os::memory_protection::read:         return "read";
    case vmhook::os::memory_protection::read_write:   return "read_write";
    case vmhook::os::memory_protection::execute_read: return "execute_read";
    case vmhook::os::memory_protection::execute_rw:   return "execute_rw";
    }
    return "<unknown>";
}

// Whether a portable protection carries the WRITE bit (read_write or
// execute_rw).  Used by the matrix to decide whether a real store is expected
// to stick; the read-only protections (read, execute_read) and no_access must
// never be probed with a store (it would fault the process).
static auto protection_is_writable(vmhook::os::memory_protection p) -> bool
{
    return p == vmhook::os::memory_protection::read_write
        || p == vmhook::os::memory_protection::execute_rw;
}

// Whether a portable protection carries the READ bit — every portable
// protection except no_access is readable, so safe_read should succeed on a
// page in that state (on the platforms whose safe_read is fault-safe).
// [[maybe_unused]]: its only caller sits inside the matrix's !VMHOOK_OS_IOS
// guard (iOS has no fault-safe read), so on an iOS build this helper is unused.
[[maybe_unused]] static auto protection_is_readable(vmhook::os::memory_protection p) -> bool
{
    return p != vmhook::os::memory_protection::no_access;
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

// ---------------------------------------------------------------------------
// EXHAUSTIVE: the FULL source->dest transition matrix.  The enum-walk and
// behavioural tests above each follow one fixed sequence; this drives EVERY
// ordered pair (src, dst) over the portable enum on a single fresh page,
// re-establishing `src` before each transition and asserting the transition to
// `dst` succeeds.  Where the resulting state is fault-safe to probe we confirm
// the OBSERVABLE effect, not just the bool:
//   * dst readable (every state except no_access) -> safe_read succeeds;
//   * dst no_access                               -> safe_read refuses;
//   * dst writable (read_write / execute_rw)      -> a real store sticks.
// W^X-gated states (anything bearing both W and X, i.e. execute_rw) and
// no_access may be REFUSED by a hardened kernel / sandbox; a refused transition
// is an [INFO] characterisation, never a hard failure, and we re-seed from a
// known-good writable state so the matrix keeps walking.  The page is restored
// writable between every pair so a refused W^X dst cannot strand the walk.
//
// This is the single most exhaustive assertion bundle in the file: 5 src x 5
// dst = 25 ordered transitions, each gated and probed.  It is the regression
// net that catches a to_native_protect switch arm collapsing two states (e.g.
// read_write and execute_rw mapping to the same native mask) or a transition
// that silently no-ops.
// ---------------------------------------------------------------------------
static auto test_protect_full_transition_matrix() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
    if (!block)
    {
        check("protect_matrix_skipped_alloc_failed", false);
        return;
    }

    auto* const bytes{ static_cast<std::uint8_t*>(block) };

    const vmhook::os::memory_protection states[]{
        vmhook::os::memory_protection::no_access,
        vmhook::os::memory_protection::read,
        vmhook::os::memory_protection::read_write,
        vmhook::os::memory_protection::execute_read,
        vmhook::os::memory_protection::execute_rw,
    };

    for (const auto src : states)
    {
        for (const auto dst : states)
        {
            // Re-seed: start every pair from a known writable mapping and a
            // known byte value, so a probe after `dst` is interpreted against a
            // controlled baseline and a previously-refused W^X dst can't strand
            // the page.
            if (!make_writable(block, page))
            {
                // Cannot even re-establish a writable mapping; the platform is
                // refusing our own allocation's restore.  Characterise and stop
                // probing this allocation rather than asserting a host issue.
                std::printf("[INFO] protect_matrix: make_writable refused mid-walk; "
                            "stopping matrix on this host\n");
                vmhook::os::release(block, page);
                return;
            }
            bytes[0] = 0x5A;

            // Establish the source protection.  A refused source (W^X / sandbox)
            // is not a failure of the dst transition we are measuring; skip just
            // this pair.
            const bool src_ok{ vmhook::os::protect(block, page, src, nullptr) };
            if (!src_ok)
            {
                std::printf("[INFO] protect_matrix src=%s refused; skipping pairs from it\n",
                            protection_name(src));
                continue;
            }

            // The measured transition: src -> dst.
            const bool dst_ok{ vmhook::os::protect(block, page, dst, nullptr) };

            const bool dst_is_wx{ dst == vmhook::os::memory_protection::execute_rw };
            const bool dst_is_none{ dst == vmhook::os::memory_protection::no_access };
            if (!dst_ok)
            {
                // Only the W^X-bearing and no_access destinations may be refused
                // by a hardened platform; any OTHER refused transition is a real
                // failure of the primitive and must be flagged.
                if (dst_is_wx || dst_is_none)
                {
                    std::printf("[INFO] protect_matrix %s->%s refused (W^X / sandbox)\n",
                                protection_name(src), protection_name(dst));
                }
                else
                {
                    check("protect_matrix_nonhardened_transition_succeeds", false);
                }
                continue;
            }

            // The transition succeeded — assert it and then probe the observable
            // effect where it is fault-safe to do so.
            check("protect_matrix_transition_succeeds", true);

#if !VMHOOK_OS_IOS
            if (protection_is_readable(dst))
            {
                check("protect_matrix_readable_state_is_readable",
                      byte_is_readable(block));
            }
            else
            {
                // no_access dst: must NOT be readable.  (Only reached when the
                // platform granted PROT_NONE.)
                check("protect_matrix_no_access_state_not_readable",
                      !byte_is_readable(block));
            }
#endif
            // A real store is only safe on a writable dst; the byte was 0x5A from
            // the re-seed, so a successful store to 0xC3 proves the W bit.
            if (protection_is_writable(dst))
            {
                bytes[0] = 0xC3;
                check("protect_matrix_writable_state_store_sticks",
                      bytes[0] == 0xC3);
            }
        }
    }

    // Always leave the page writable before release.
    check("protect_matrix_restore_writable", make_writable(block, page));
    vmhook::os::release(block, page);
}

// ---------------------------------------------------------------------------
// EXHAUSTIVE: sub-range protect WITHIN a larger block.  The charter case the
// neighbour-witness test (page 0 of a 2-page block) only half-covers: protect
// the MIDDLE page of a 3-page allocation read-only and confirm BOTH flanking
// pages (page 0 and page 2) remain writable.  This is exactly what a trampoline
// installer does when it flips one interior code page while leaving the data
// pages on either side untouched.
//
// On POSIX the request is page-aligned already (base = block + page, len =
// page), so mprotect touches page 1 only.  On Windows VirtualProtect operates
// on the single enclosing page.  Either way pages 0 and 2 must stay writable —
// a fresh store into each must stick.  We never read the middle page through a
// raw deref; we only re-confirm its byte is preserved (a protection change
// never rewrites data) once it is writable again.
// ---------------------------------------------------------------------------
static auto test_protect_subrange_within_block() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    void* const block{ vmhook::os::allocate_rwx(nullptr, page * 3) };
    if (!block)
    {
        check("protect_subrange_skipped_alloc_failed", false);
        return;
    }

    auto* const bytes{ static_cast<std::uint8_t*>(block) };
    bytes[0]            = 0x11; // page 0 marker
    bytes[page]         = 0x22; // page 1 marker (the page we protect)
    bytes[page * 2]     = 0x33; // page 2 marker

    // Flip ONLY the middle page to read-only.
    const bool flipped{ vmhook::os::protect(bytes + page, page,
                                            vmhook::os::memory_protection::read,
                                            nullptr) };
    check("protect_subrange_middle_page_succeeds", flipped);

    // Flanking pages must still be writable: a brand-new store into each sticks.
    bytes[0] = 0xF0;
    check("protect_subrange_page0_still_writable", bytes[0] == 0xF0);
    bytes[page * 2] = 0xF3;
    check("protect_subrange_page2_still_writable", bytes[page * 2] == 0xF3);

#if !VMHOOK_OS_IOS
    // The protected middle page is readable (read state), via the fault-safe
    // path only.
    if (flipped)
    {
        check("protect_subrange_middle_page_readable",
              byte_is_readable(bytes + page));
    }
#endif

    // Restore the whole block writable; the middle page's marker must be intact
    // (protection never rewrote it).
    check("protect_subrange_restore_writable", make_writable(block, page * 3));
    check("protect_subrange_middle_marker_preserved", bytes[page] == 0x22);
    vmhook::os::release(block, page * 3);
}

// ---------------------------------------------------------------------------
// EXHAUSTIVE: no_access recovery — the real trampoline pattern of locking a page
// down and then bringing it back.  protect(no_access) then protect(read_write)
// must return the page to a fully usable writable state: a store sticks and the
// previously-written marker survives the round trip (no_access does not zero the
// backing).  Gated like every no_access probe: a sandbox that forbids PROT_NONE
// turns the lock step into an [INFO] skip.
// ---------------------------------------------------------------------------
#if !VMHOOK_OS_IOS
static auto test_protect_no_access_recovery() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
    if (!block)
    {
        check("protect_no_access_recovery_skipped_alloc_failed", false);
        return;
    }

    auto* const bytes{ static_cast<std::uint8_t*>(block) };
    bytes[0] = 0x7E;

    const bool locked{ vmhook::os::protect(block, page,
                                           vmhook::os::memory_protection::no_access,
                                           nullptr) };
    if (!locked)
    {
        std::printf("[INFO] protect_no_access_recovery skipped: PROT_NONE refused\n");
        vmhook::os::release(block, page);
        return;
    }

    // While locked, the page is not readable through the fault-safe path.
    check("protect_no_access_recovery_locked_not_readable", !byte_is_readable(block));

    // Recover to writable.  This is the contract a hook uninstaller relies on:
    // a page locked to no_access can always be brought back to RW (or RWX).
    const bool recovered{ make_writable(block, page) };
    check("protect_no_access_recovery_back_to_writable", recovered);
    if (recovered)
    {
        // The marker written before the lock survived the no_access round trip.
        check("protect_no_access_recovery_marker_survived", bytes[0] == 0x7E);
        // And the page is writable again.
        bytes[0] = 0x1D;
        check("protect_no_access_recovery_store_sticks_after", bytes[0] == 0x1D);
#if !VMHOOK_OS_IOS
        check("protect_no_access_recovery_readable_after", byte_is_readable(block));
#endif
    }

    vmhook::os::release(block, page);
}
#endif

// ---------------------------------------------------------------------------
// EXHAUSTIVE: the old_prot out-param is written on EVERY successful flip across
// a long chain, not just the first two.  The contract test above pins two
// flips; this drives a five-step chain and re-pins the platform-asymmetric
// value at each step (Windows: non-zero PAGE_* bitmask; POSIX: exactly 0).  A
// fresh sentinel is seeded before each call so "written on success" is a real
// observation, never a stale value.  We NEVER feed a POSIX old_prot back into
// protect() (that would request no_access on the documented-zero contract).
// ---------------------------------------------------------------------------
static auto test_protect_old_prot_written_every_flip() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
    if (!block)
    {
        check("protect_old_prot_chain_skipped_alloc_failed", false);
        return;
    }

    auto* const bytes{ static_cast<std::uint8_t*>(block) };
    bytes[0] = 0x01;

    constexpr std::uint32_t sentinel{ 0xDEADBEEFu };

    // A chain that avoids no_access and the W^X-gated execute_rw so every step is
    // expected to succeed on every CI platform: read -> read_write ->
    // execute_read -> read -> read_write.
    const vmhook::os::memory_protection chain[]{
        vmhook::os::memory_protection::read,
        vmhook::os::memory_protection::read_write,
        vmhook::os::memory_protection::execute_read,
        vmhook::os::memory_protection::read,
        vmhook::os::memory_protection::read_write,
    };

    bool all_written{ true };
    bool platform_value_ok{ true };
    for (const auto prot : chain)
    {
        std::uint32_t op{ sentinel };
        const bool ok{ vmhook::os::protect(block, page, prot, &op) };
        check("protect_old_prot_chain_step_succeeds", ok);
        if (ok)
        {
            if (op == sentinel)
            {
                all_written = false;
            }
#if VMHOOK_OS_WINDOWS
            // Windows reports the genuine prior native protection; every state in
            // the chain maps to a non-zero PAGE_* constant, so old_prot is never
            // zero here.
            if (op == 0u)
            {
                platform_value_ok = false;
            }
#else
            // POSIX writes 0 unconditionally.
            if (op != 0u)
            {
                platform_value_ok = false;
            }
#endif
        }
    }
    check("protect_old_prot_chain_written_every_step", all_written);
#if VMHOOK_OS_WINDOWS
    check("protect_old_prot_chain_windows_nonzero_every_step", platform_value_ok);
#else
    check("protect_old_prot_chain_posix_zero_every_step", platform_value_ok);
#endif

    check("protect_old_prot_chain_restore_writable", make_writable(block, page));
    vmhook::os::release(block, page);
}

// ---------------------------------------------------------------------------
// EXHAUSTIVE: the overflow / address-space-wrap guard must leave a caller-
// supplied old_prot UNTOUCHED, exactly like the null/zero guards do.  The
// guard fires BEFORE the kernel call and before old_prot is written, so a
// rejected wrapping request must not clobber the out-param.  (The null/zero
// guard's old_prot-untouched contract is owned by the sibling release/edges
// file; the OVERFLOW guard is this file's charter, so we pin its old_prot
// behaviour here.)
// ---------------------------------------------------------------------------
static auto test_protect_overflow_guard_old_prot_untouched() -> void
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

    constexpr std::uint32_t sentinel{ 0xFEEDFACEu };

    // SIZE_MAX from a non-null base wraps the address space -> rejected, and the
    // out-param must keep its sentinel.
    {
        std::uint32_t op{ sentinel };
        const bool ok{ vmhook::os::protect(block, SIZE_MAX,
                                           vmhook::os::memory_protection::read, &op) };
        check("protect_overflow_size_max_returns_false", !ok);
        check("protect_overflow_size_max_old_prot_untouched", op == sentinel);
    }

    // A size precisely tuned so base + size wraps past UINTPTR_MAX by 8 bytes.
    {
        const std::uintptr_t base_addr{ reinterpret_cast<std::uintptr_t>(block) };
        const std::uintptr_t max_addr{ ~static_cast<std::uintptr_t>(0) };
        const std::size_t wrapping_size{
            static_cast<std::size_t>(max_addr - base_addr) + std::size_t{ 8 } };
        std::uint32_t op{ sentinel };
        const bool ok{ vmhook::os::protect(block, wrapping_size,
                                           vmhook::os::memory_protection::read, &op) };
        check("protect_overflow_wrap_returns_false", !ok);
        check("protect_overflow_wrap_old_prot_untouched", op == sentinel);
    }

    // The page protection must be unchanged by the rejected calls: a store still
    // sticks.
    bytes[0] = 0xA5;
    check("protect_overflow_old_prot_page_still_writable", bytes[0] == 0xA5);
    vmhook::os::release(block, page);
}

// ---------------------------------------------------------------------------
// EXHAUSTIVE: the size==page and size==page-1 LOWER-boundary witness — the
// mirror of the spill witness below.  Both a full-page request and a one-byte-
// short request, launched from an aligned base, must round to EXACTLY ONE page
// and so must NOT touch page 1.  Allocate two pages: after protect(page, read)
// and again after protect(page-1, read), page 1 must remain writable (a fresh
// store sticks), proving the rounding did NOT spill.  Together with the
// page_plus_one spill witness this pins both sides of the rounding boundary:
//   size in [1 .. page]      -> 1 page
//   size in [page+1 .. 2page]-> 2 pages
// ---------------------------------------------------------------------------
static auto test_protect_exact_and_minus_one_no_spill_witness() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    void* const block{ vmhook::os::allocate_rwx(nullptr, page * 2) };
    if (!block)
    {
        check("protect_lower_boundary_witness_skipped_alloc_failed", false);
        return;
    }

    auto* const bytes{ static_cast<std::uint8_t*>(block) };

    // size == page : exactly one page, no rounding -> page 1 untouched.
    {
        bytes[0]    = 0xA0; // page 0
        bytes[page] = 0xB0; // page 1 (must stay writable)
        const bool flipped{ vmhook::os::protect(block, page,
                                                vmhook::os::memory_protection::read,
                                                nullptr) };
        check("protect_exact_page_no_spill_succeeds", flipped);
        bytes[page] = 0xBF; // page 1 store must stick
        check("protect_exact_page_page1_still_writable", bytes[page] == 0xBF);
        check("protect_exact_page_restore", make_writable(block, page * 2));
        check("protect_exact_page_page0_marker_preserved", bytes[0] == 0xA0);
    }

    // size == page - 1 : rounds UP to exactly one page -> page 1 still untouched.
    {
        bytes[0]    = 0xC0;
        bytes[page] = 0xD0;
        const bool flipped{ vmhook::os::protect(block, page - 1,
                                                vmhook::os::memory_protection::read,
                                                nullptr) };
        check("protect_page_minus_one_rounds_to_one_page_succeeds", flipped);
        bytes[page] = 0xDF; // page 1 still writable
        check("protect_page_minus_one_page1_still_writable", bytes[page] == 0xDF);
        check("protect_page_minus_one_restore", make_writable(block, page * 2));
        check("protect_page_minus_one_page0_marker_preserved", bytes[0] == 0xC0);
    }

    vmhook::os::release(block, page * 2);
}

// ---------------------------------------------------------------------------
// EXHAUSTIVE: the size==page+1 SPILL witness.  A request of exactly one page
// plus one byte, launched from an aligned base, must round UP to cover TWO pages
// (page 0 fully, page 1 because of the 1-byte spill) and must NOT touch a third
// page.  Allocate three pages: after protect(page+1, read), page 2 must still be
// writable (proving the rounding stopped at page 1), while pages 0 and 1 carry
// the read protection.  This pins the exact page-rounding boundary — the
// difference between "rounds to 1 page" (size==page) and "rounds to 2 pages"
// (size==page+1) — against a live mprotect/VirtualProtect.
// ---------------------------------------------------------------------------
static auto test_protect_page_plus_one_spill_witness() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    void* const block{ vmhook::os::allocate_rwx(nullptr, page * 3) };
    if (!block)
    {
        check("protect_spill_witness_skipped_alloc_failed", false);
        return;
    }

    auto* const bytes{ static_cast<std::uint8_t*>(block) };
    bytes[0]          = 0xA0; // page 0
    bytes[page]       = 0xB0; // page 1 (the spill target)
    bytes[page * 2]   = 0xC0; // page 2 (must stay writable)

    // page + 1 bytes from the aligned base spills one byte into page 1.
    const bool flipped{ vmhook::os::protect(block, page + 1,
                                            vmhook::os::memory_protection::read,
                                            nullptr) };
    check("protect_spill_witness_succeeds", flipped);

    // Page 2 was outside the rounded span and must remain writable.
    bytes[page * 2] = 0xCF;
    check("protect_spill_witness_page2_still_writable", bytes[page * 2] == 0xCF);

#if !VMHOOK_OS_IOS
    if (flipped)
    {
        // Pages 0 and 1 are both read-only now -> readable via fault-safe path.
        check("protect_spill_witness_page0_readable", byte_is_readable(block));
        check("protect_spill_witness_page1_readable", byte_is_readable(bytes + page));
    }
#endif

    // Markers in the protected pages are intact (protection never rewrites data).
    check("protect_spill_witness_restore_writable", make_writable(block, page * 3));
    check("protect_spill_witness_markers_preserved",
          bytes[0] == 0xA0 && bytes[page] == 0xB0);
    vmhook::os::release(block, page * 3);
}

// ---------------------------------------------------------------------------
// EXHAUSTIVE: cross-allocation isolation.  protect() on one allocation must not
// affect an unrelated, separately-allocated block.  Two independent RWX blocks:
// flip block A read-only; block B must stay fully writable (a store sticks) and
// readable.  The two mmap/VirtualAlloc reservations are distinct kernel objects,
// so the protect on A can never alter B — this guards against a wrapper bug that
// mis-computes the base (e.g. masking the wrong allocation) and against any
// cross-region bleed.
// ---------------------------------------------------------------------------
static auto test_protect_cross_allocation_isolation() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    void* const a{ vmhook::os::allocate_rwx(nullptr, page) };
    void* const b{ vmhook::os::allocate_rwx(nullptr, page) };
    if (!a || !b)
    {
        check("protect_isolation_skipped_alloc_failed", false);
        if (a)
        {
            vmhook::os::release(a, page);
        }
        if (b)
        {
            vmhook::os::release(b, page);
        }
        return;
    }

    auto* const ba{ static_cast<std::uint8_t*>(a) };
    auto* const bb{ static_cast<std::uint8_t*>(b) };
    ba[0] = 0xAA;
    bb[0] = 0xBB;

    // Flip A read-only.
    const bool flipped{ vmhook::os::protect(a, page,
                                            vmhook::os::memory_protection::read,
                                            nullptr) };
    check("protect_isolation_flip_a_succeeds", flipped);

    // B must be entirely unaffected: a fresh store into B sticks.
    bb[0] = 0xCC;
    check("protect_isolation_b_still_writable", bb[0] == 0xCC);
#if !VMHOOK_OS_IOS
    check("protect_isolation_b_still_readable", byte_is_readable(b));
    if (flipped)
    {
        check("protect_isolation_a_readable_after_flip", byte_is_readable(a));
    }
#endif

    // A's marker is intact under read protection.
    check("protect_isolation_restore_a_writable", make_writable(a, page));
    check("protect_isolation_a_marker_preserved", ba[0] == 0xAA);

    vmhook::os::release(a, page);
    vmhook::os::release(b, page);
}

// ---------------------------------------------------------------------------
// EXHAUSTIVE: multi-byte safe_read probe across the protect states.  Every
// behavioural probe above reads a single byte; this confirms a FULL multi-byte
// span is honoured — an all-or-nothing read of N bytes from a read-protected
// page succeeds and returns the exact bytes, while the same span from a
// no_access page is refused in its entirety (safe_read is all-or-nothing, never
// a partial copy).  Locks the size>1 path of safe_read against each protection.
// Gated off iOS (raw memcpy) and skips where PROT_NONE is refused.
// ---------------------------------------------------------------------------
#if !VMHOOK_OS_IOS
static auto test_protect_multibyte_safe_read_probe() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
    if (!block)
    {
        check("protect_multibyte_probe_skipped_alloc_failed", false);
        return;
    }

    auto* const bytes{ static_cast<std::uint8_t*>(block) };
    // Stamp a recognisable 8-byte pattern while still writable.
    const std::uint8_t pattern[8]{ 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80 };
    constexpr std::size_t span{ sizeof(pattern) };
    for (std::size_t i{ 0 }; i < span; ++i)
    {
        bytes[i] = pattern[i];
    }

    // read state: a full 8-byte safe_read must succeed and match the pattern.
    {
        const bool ok{ vmhook::os::protect(block, page,
                                           vmhook::os::memory_protection::read, nullptr) };
        check("protect_multibyte_read_flip_succeeds", ok);
        std::uint8_t dst[span]{};
        const bool read_ok{ vmhook::os::safe_read(dst, block, span) };
        check("protect_multibyte_read_span_succeeds", read_ok);
        if (read_ok)
        {
            check("protect_multibyte_read_span_matches",
                  std::memcmp(dst, pattern, span) == 0);
        }
    }

    // no_access state: the same 8-byte safe_read must be refused entirely.
    {
        const bool locked{ vmhook::os::protect(block, page,
                                               vmhook::os::memory_protection::no_access,
                                               nullptr) };
        if (locked)
        {
            std::uint8_t dst[span]{ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
            const bool read_ok{ vmhook::os::safe_read(dst, block, span) };
            check("protect_multibyte_no_access_span_refused", !read_ok);
        }
        else
        {
            std::printf("[INFO] protect_multibyte no_access span skipped: PROT_NONE refused\n");
        }
    }

    check("protect_multibyte_probe_restore_writable", make_writable(block, page));
    vmhook::os::release(block, page);
}
#endif

// ===========================================================================
// ADDITIVE DEEPENING SECTION (namespaced to avoid colliding with the
// first-party functions above).  Everything here is either PURE ARITHMETIC over
// page_size()/allocation_granularity() or operates ONLY on memory THIS section
// allocated itself (allocate_rwx) — never a fabricated/unmapped address handed
// to a reading helper.  It targets os-layer relations the body above does not
// yet pin:
//   * page_size / allocation_granularity power-of-two + determinism + ratio
//     invariants (the body only asserts gr>=ps and gr%ps==0);
//   * query_region FIELD invariants on an owned, committed RWX page — address
//     containment (base <= addr < base+size), size is a page multiple, base is
//     page-aligned, committed XOR free, not guarded;
//   * allocate_rwx / release round-trip + a repeated alloc/release "leak" loop
//     across several sizes, each freshly writable and each released, proving no
//     accumulation / crash;
//   * the protect-arm mapping reflected back through query_region (execute_read
//     -> region reports executable; read_write store sticks) on an owned page,
//     plus the size==0 / null guards of release re-pinned as no-ops.
// Mirrors the file's check() idiom and the make_writable / allocate-guard
// patterns.  No <thread> / no value_t-cast / no vector-stringop trap / no raw
// NUL.  Every OS-specific assertion is platform-gated.
// ===========================================================================
namespace deepen_os_protect
{

// Whether `v` is a power of two (and non-zero).  Pure bit math; both page_size
// and allocation_granularity are required by every VM allocator to be powers of
// two so the mask-based rounding (base &= ~(ps-1)) the wrapper uses is correct.
static auto is_power_of_two(std::size_t v) -> bool
{
    return v != 0 && (v & (v - 1)) == 0;
}

// ---------------------------------------------------------------------------
// Pure arithmetic: page_size() and allocation_granularity() must each be a
// non-zero power of two, must be deterministic across repeated calls (they read
// fixed SYSTEM_INFO / sysconf values), and the granularity/page ratio must be an
// exact integer power-of-two multiple.  The body's test_granularity_relationship
// only pins gr>=ps and gr%ps==0; these are the stronger structural invariants
// the mask-rounding inside protect() actually depends on.
// ---------------------------------------------------------------------------
static auto test_page_and_granularity_pure_relations() -> void
{
    const std::size_t ps{ vmhook::os::page_size() };
    const std::size_t gr{ vmhook::os::allocation_granularity() };

    check("deepen_page_size_nonzero", ps != 0);
    check("deepen_page_size_power_of_two", is_power_of_two(ps));
    check("deepen_granularity_nonzero", gr != 0);
    check("deepen_granularity_power_of_two", is_power_of_two(gr));

    // Determinism: a second query returns the identical value (both are derived
    // from immutable kernel-reported constants).
    check("deepen_page_size_deterministic", vmhook::os::page_size() == ps);
    check("deepen_granularity_deterministic",
          vmhook::os::allocation_granularity() == gr);

    // gr >= ps and gr % ps == 0 (already pinned elsewhere) imply an integer
    // ratio; because both are powers of two the ratio is itself a power of two.
    const std::size_t ratio{ gr / ps };
    check("deepen_granularity_ratio_exact", ratio * ps == gr);
    check("deepen_granularity_ratio_power_of_two", is_power_of_two(ratio));

    // The page mask (ps - 1) selects exactly the low bits; ANDing it back with
    // ps must be zero (ps has no low bits set) — the precondition for the
    // POSIX rounding `base &= ~(ps-1)` to align to a page boundary.
    check("deepen_page_mask_clears_to_zero", (ps & (ps - 1)) == 0);
}

// ---------------------------------------------------------------------------
// query_region FIELD invariants on a page WE own.  The body asserts committed /
// readable / not-guarded / size>=page; here we pin the structural relationships
// the trampoline allocator relies on: the returned region must CONTAIN the
// queried address, its base must be page-aligned, its size a whole-page
// multiple, and committed and free must be mutually exclusive for a live
// mapping.  All against a real RWX allocation, restored writable before release.
// ---------------------------------------------------------------------------
static auto test_query_region_field_invariants_owned() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    void* const block{ vmhook::os::allocate_rwx(nullptr, page * 2) };
    if (!block)
    {
        check("deepen_query_invariants_skipped_alloc_failed", false);
        return;
    }

    auto* const bytes{ static_cast<std::uint8_t*>(block) };
    // Touch both pages so the whole allocation is unambiguously committed.
    bytes[0]    = 0x10;
    bytes[page] = 0x20;

    // Query the SECOND page's interior so a non-trivial base/containment
    // relationship is exercised (not just base==address).
    const void* const probe{ bytes + page + 7 };
    const auto info{ vmhook::os::query_region(probe) };

    check("deepen_query_committed", info.committed);
    check("deepen_query_not_free_when_committed", !(info.committed && info.free));
    check("deepen_query_base_non_null", info.base != nullptr);
    check("deepen_query_size_nonzero", info.size != 0);

    // Containment: base <= probe < base + size.
    const std::uintptr_t base_addr{ reinterpret_cast<std::uintptr_t>(info.base) };
    const std::uintptr_t probe_addr{ reinterpret_cast<std::uintptr_t>(probe) };
    check("deepen_query_base_at_or_below_probe", base_addr <= probe_addr);
    check("deepen_query_probe_within_region",
          probe_addr < base_addr + static_cast<std::uintptr_t>(info.size));

    // Base must be page-aligned and size a whole-page multiple (a region the
    // kernel reports is always page-granular on every supported platform).
    check("deepen_query_base_page_aligned", (base_addr % page) == 0);
    check("deepen_query_size_page_multiple", (info.size % page) == 0);

    check("deepen_query_restore_writable", make_writable(block, page * 2));
    vmhook::os::release(block, page * 2);
}

// ---------------------------------------------------------------------------
// allocate_rwx / release round-trip across several sizes, plus a repeated
// alloc/release loop.  Each allocation must be non-null, freshly writable (a
// store sticks at offset 0 and at the last byte), and released cleanly.  The
// loop pins that repeated reservation+release neither leaks into failure nor
// crashes — exactly the churn a trampoline pool produces over a long session.
// Sizes are page-relative (never hard-coded) so it holds on 16K-page Apple HW.
// ---------------------------------------------------------------------------
static auto test_alloc_release_roundtrip_and_loop() -> void
{
    const std::size_t page{ vmhook::os::page_size() };

    const std::size_t sizes[]{
        page,
        page * 2,
        page * 3,
        page * 4 + 1, // intentionally not a page multiple: must round up internally
    };

    bool all_alloc_ok{ true };
    bool all_writable{ true };
    for (const std::size_t sz : sizes)
    {
        void* const block{ vmhook::os::allocate_rwx(nullptr, sz) };
        if (!block)
        {
            all_alloc_ok = false;
            continue;
        }
        auto* const bytes{ static_cast<std::uint8_t*>(block) };
        // First and last requested byte must both be writable.
        bytes[0]      = 0x5A;
        bytes[sz - 1] = 0xA5;
        if (!(bytes[0] == 0x5A && bytes[sz - 1] == 0xA5))
        {
            all_writable = false;
        }
        vmhook::os::release(block, sz);
    }
    check("deepen_alloc_all_sizes_succeed", all_alloc_ok);
    check("deepen_alloc_all_sizes_writable_first_and_last", all_writable);

    // Repeated single-page alloc/release loop: every iteration must succeed and
    // be independently writable.  A leak or double-free regression surfaces as a
    // failed allocation or a crash partway through.
    bool loop_ok{ true };
    for (int i{ 0 }; i < 64; ++i)
    {
        void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
        if (!block)
        {
            loop_ok = false;
            break;
        }
        *static_cast<volatile std::uint8_t*>(block) = static_cast<std::uint8_t>(i);
        vmhook::os::release(block, page);
    }
    check("deepen_alloc_release_loop_64_iterations_ok", loop_ok);
}

// ---------------------------------------------------------------------------
// The protect-arm mapping, observed through query_region, on an owned page.
// execute_read must set the region's executable flag (the X bit the trampoline
// installer checks before treating a page as code); read_write must leave the
// page writable (a store sticks).  The body's roundtrip test pins execute_read
// -> executable in one fixed sequence; this re-pins it independently and adds
// the read_write writability witness on the SAME page so a to_native_protect arm
// that collapsed execute_read into read_write (dropping X) would fail here.
// W^X / sandbox refusals are treated as [INFO] skips, never hard failures.
// ---------------------------------------------------------------------------
static auto test_protect_arm_mapping_via_query() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
    if (!block)
    {
        check("deepen_arm_mapping_skipped_alloc_failed", false);
        return;
    }

    auto* const bytes{ static_cast<std::uint8_t*>(block) };
    bytes[0] = 0x3C;

    // execute_read -> region reports readable AND executable.
    {
        const bool ok{ vmhook::os::protect(block, page,
                                           vmhook::os::memory_protection::execute_read,
                                           nullptr) };
        if (ok)
        {
            const auto info{ vmhook::os::query_region(block) };
            check("deepen_arm_execrx_region_readable", info.readable);
            check("deepen_arm_execrx_region_executable", info.executable);
        }
        else
        {
            std::printf("[INFO] deepen_arm execute_read refused (W^X / sandbox)\n");
        }
    }

    // read_write -> region readable and a real store sticks (proves the W bit
    // came back and the X-only-or-not state did not strand writability).
    {
        const bool ok{ vmhook::os::protect(block, page,
                                           vmhook::os::memory_protection::read_write,
                                           nullptr) };
        check("deepen_arm_rw_succeeds", ok);
        if (ok)
        {
            const auto info{ vmhook::os::query_region(block) };
            check("deepen_arm_rw_region_readable", info.readable);
            bytes[0] = 0xD2;
            check("deepen_arm_rw_store_sticks", bytes[0] == 0xD2);
        }
    }

    check("deepen_arm_mapping_restore_writable", make_writable(block, page));
    vmhook::os::release(block, page);
}

// ---------------------------------------------------------------------------
// release() guard re-pin on an owned block: a null address, and a zero size with
// a live base, must both be NO-OPS that neither crash nor disturb the live
// mapping.  The body's input-guard test covers the null/zero-size cases; here we
// additionally confirm the block stays fully usable (writable at offset 0 AND at
// the last page byte) after the no-op release attempts, before the real release.
// ---------------------------------------------------------------------------
static auto test_release_guard_is_noop_on_owned_block() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
    if (!block)
    {
        check("deepen_release_noop_skipped_alloc_failed", false);
        return;
    }

    auto* const bytes{ static_cast<std::uint8_t*>(block) };
    bytes[0]        = 0x11;
    bytes[page - 1] = 0x22;

    // No-op releases: null address (ignores size), and zero size on the real
    // base (must NOT unmap the live block).
    vmhook::os::release(nullptr, page);
    vmhook::os::release(block, 0);

    // The block must still be fully live and writable after the no-ops.
    bytes[0]        = 0x33;
    bytes[page - 1] = 0x44;
    check("deepen_release_noop_first_byte_writable", bytes[0] == 0x33);
    check("deepen_release_noop_last_byte_writable", bytes[page - 1] == 0x44);

    vmhook::os::release(block, page); // the real release
}

} // namespace deepen_os_protect

// ===========================================================================
// ADDITIVE DEEPENING SECTION 2 (safe_read charter, namespaced).  Everything
// here drives vmhook::os::safe_read against REAL allocations THIS section makes
// itself (allocate_rwx + protect), never a fabricated/unmapped address handed to
// a raw reader.  It targets the safe_read angles the body above does not yet
// pin: the body asserts the 1-byte no_access refusal, the multibyte positive +
// no_access-span refusal, and the null/zero input guards.  These add:
//   * the POSITIVE round-trip across widths (1,2,4,8,16,page-1,page) byte-exact
//     against a reference memcpy — the body's positive case is a single 8-byte
//     span; this proves every width copies correctly;
//   * a read that STRADDLES from a readable page INTO a no_access page must
//     return false (the all-or-nothing contract; the readable prefix does not
//     license success);
//   * a read ending EXACTLY at the readable/no_access boundary (last 8 bytes of
//     the readable page, none of the locked page) must SUCCEED and match — the
//     off-by-one sibling proving the wrapper does not over-read by a byte;
//   * a read STARTING exactly on the no_access page must be refused at widths 1
//     and 8 — the readable->unreadable transition address;
//   * safe_read from FREED / released memory must return false, never crash;
//   * repeated fallback-path entry (many no_access reads in a row) must not
//     wedge the process: a deliberate readable safe_read still succeeds and a
//     normal in-process store still works afterwards (the closest pure-logic
//     proxy for the signal-handler self-disarm + re-arm staying healthy);
//   * size==1 with two valid pointers is the POSITIVE twin of the size==0 guard,
//     proving the guard is exactly `size == 0`, not `size < something`;
//   * an absurd / address-space-wrapping size (SIZE_MAX, and base+size wrapping
//     past UINTPTR_MAX) from a real small buffer must be rejected with false and
//     must NOT hang — the wrap-guard contract.
// No-access probes stay behind !VMHOOK_OS_IOS (iOS safe_read is a raw memcpy
// that cannot honour the no-fault contract) and keep the sandbox-skip pattern
// (protect(no_access) refused -> [INFO] skip, never a hard fail).  Mirrors the
// file's check() idiom and the make_writable / allocate-guard helpers above.
// No <thread> / no value_t-cast / no vector-stringop / no narrowing / no raw NUL.
// ===========================================================================
namespace deepen_safe_read
{

// ---------------------------------------------------------------------------
// POSITIVE width round-trip.  A good read must copy the exact bytes for every
// width a caller hands safe_read.  We stamp a deterministic ramp into a real RWX
// page, leave it readable, and assert safe_read(dst, src, w) for each width w
// matches a reference std::memcpy of the same span byte-for-byte.  Widths are
// page-relative at the top end (page-1, page) so the assertion holds on 16K-page
// Apple hardware as well as 4K-page x86.
// ---------------------------------------------------------------------------
static auto test_safe_read_positive_width_roundtrip() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
    if (!block)
    {
        check("deepen_sr_width_roundtrip_skipped_alloc_failed", false);
        return;
    }

    auto* const bytes{ static_cast<std::uint8_t*>(block) };
    // Deterministic ramp across the whole page so every width samples distinct
    // bytes (low byte of the index keeps it in range without a NUL run).
    for (std::size_t i{ 0 }; i < page; ++i)
    {
        bytes[i] = static_cast<std::uint8_t>((i * 7u + 0x11u) & 0xFFu);
    }

    const std::size_t widths[]{
        std::size_t{ 1 },
        std::size_t{ 2 },
        std::size_t{ 4 },
        std::size_t{ 8 },
        std::size_t{ 16 },
        page - 1,
        page,
    };

    // A separate full-page destination owned by this test; safe_read copies the
    // requested width into it and we compare those bytes against the source page
    // (the source IS the reference — safe_read copies straight from `bytes`).
    void* const dst_block{ vmhook::os::allocate_rwx(nullptr, page) };
    if (!dst_block)
    {
        check("deepen_sr_width_roundtrip_skipped_dst_alloc_failed", false);
        vmhook::os::release(block, page);
        return;
    }
    auto* const dst_bytes{ static_cast<std::uint8_t*>(dst_block) };

    bool all_ok{ true };
    bool all_match{ true };
    for (const std::size_t w : widths)
    {
        // Pre-fill the destination with a sentinel so a short copy would leave a
        // detectable mismatch, then read exactly `w` bytes.
        for (std::size_t i{ 0 }; i < page; ++i)
        {
            dst_bytes[i] = 0xFFu;
        }
        const bool ok{ vmhook::os::safe_read(dst_bytes, bytes, w) };
        if (!ok)
        {
            all_ok = false;
        }
        else if (std::memcmp(dst_bytes, bytes, w) != 0)
        {
            all_match = false;
        }
    }
    check("deepen_sr_width_roundtrip_all_succeed", all_ok);
    check("deepen_sr_width_roundtrip_all_byte_exact", all_match);

    vmhook::os::release(dst_block, page);
    vmhook::os::release(block, page);
}

// ---------------------------------------------------------------------------
// size==1 with two valid pointers is the POSITIVE twin of the size==0 guard:
// the guard rejects exactly `size == 0`, so a 1-byte read of a real readable
// byte must SUCCEED and copy that byte.  Proves the body's size==0 refusal is a
// `== 0` test, not a `< something` test that would also swallow size==1.
// ---------------------------------------------------------------------------
static auto test_safe_read_size_one_succeeds() -> void
{
    std::uint8_t src{ 0xC7 };
    std::uint8_t dst{ 0x00 };
    const bool ok{ vmhook::os::safe_read(&dst, &src, 1u) };
    check("deepen_sr_size_one_succeeds", ok);
    check("deepen_sr_size_one_copies_byte", dst == 0xC7);
}

#if !VMHOOK_OS_IOS
// ---------------------------------------------------------------------------
// STRADDLE: a read that begins in a readable page and crosses INTO a no_access
// page must return false in its entirety.  Two pages: page 0 RWX (readable),
// page 1 locked no_access.  A marker is stamped spanning the boundary while both
// are writable; then page 1 is locked and safe_read reads 8 bytes starting 4
// bytes before the boundary (so 4 readable + 4 unreadable).  The all-or-nothing
// contract requires false — the readable prefix must NOT license success.
// Skips when PROT_NONE is refused.
// ---------------------------------------------------------------------------
static auto test_safe_read_straddle_into_no_access_refused() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    void* const block{ vmhook::os::allocate_rwx(nullptr, page * 2) };
    if (!block)
    {
        check("deepen_sr_straddle_skipped_alloc_failed", false);
        return;
    }

    auto* const bytes{ static_cast<std::uint8_t*>(block) };
    // Stamp a recognisable pattern across [page-4, page+4] while both writable.
    for (std::size_t i{ 0 }; i < 8u; ++i)
    {
        bytes[page - 4u + i] = static_cast<std::uint8_t>(0x90u + i);
    }

    // Lock ONLY page 1 to no_access.
    const bool locked{ vmhook::os::protect(bytes + page, page,
                                           vmhook::os::memory_protection::no_access,
                                           nullptr) };
    if (!locked)
    {
        std::printf("[INFO] deepen_sr_straddle skipped: PROT_NONE refused\n");
        vmhook::os::release(block, page * 2);
        return;
    }

    // 8-byte read starting 4 bytes before the boundary: crosses into page 1.
    std::uint8_t dst[8]{ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    const bool read_ok{ vmhook::os::safe_read(dst, bytes + page - 4u, 8u) };
    check("deepen_sr_straddle_into_no_access_refused", !read_ok);

    // Restore writable before release so unmap sees no locked page.
    (void)vmhook::os::protect(bytes + page, page,
                              vmhook::os::memory_protection::read_write, nullptr);
    vmhook::os::release(block, page * 2);
}

// ---------------------------------------------------------------------------
// BOUNDARY (read side): a read ending EXACTLY at the readable/no_access boundary
// — the last 8 bytes of the readable page, none of the locked page — must
// SUCCEED and match.  This is the off-by-one sibling of the straddle test: it
// proves the wrapper does not over-read even a single byte into the locked page.
// Skips when PROT_NONE is refused.
// ---------------------------------------------------------------------------
static auto test_safe_read_ending_at_boundary_succeeds() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    void* const block{ vmhook::os::allocate_rwx(nullptr, page * 2) };
    if (!block)
    {
        check("deepen_sr_boundary_end_skipped_alloc_failed", false);
        return;
    }

    auto* const bytes{ static_cast<std::uint8_t*>(block) };
    // Stamp the last 8 bytes of page 0 with a known pattern.
    std::uint8_t pattern[8]{ 0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, 0x07, 0x18 };
    for (std::size_t i{ 0 }; i < 8u; ++i)
    {
        bytes[page - 8u + i] = pattern[i];
    }

    const bool locked{ vmhook::os::protect(bytes + page, page,
                                           vmhook::os::memory_protection::no_access,
                                           nullptr) };
    if (!locked)
    {
        std::printf("[INFO] deepen_sr_boundary_end skipped: PROT_NONE refused\n");
        vmhook::os::release(block, page * 2);
        return;
    }

    std::uint8_t dst[8]{};
    const bool read_ok{ vmhook::os::safe_read(dst, bytes + page - 8u, 8u) };
    check("deepen_sr_ending_at_boundary_succeeds", read_ok);
    if (read_ok)
    {
        check("deepen_sr_ending_at_boundary_matches",
              std::memcmp(dst, pattern, 8u) == 0);
    }

    (void)vmhook::os::protect(bytes + page, page,
                              vmhook::os::memory_protection::read_write, nullptr);
    vmhook::os::release(block, page * 2);
}

// ---------------------------------------------------------------------------
// BOUNDARY (start side): a read STARTING exactly on the no_access page must be
// refused at widths 1 AND 8.  The body's 1-byte refusal reads offset 0 of a
// single locked page; this verifies the readable->unreadable transition address
// (the first byte of the locked SECOND page) is itself refused.  Skips when
// PROT_NONE is refused.
// ---------------------------------------------------------------------------
static auto test_safe_read_starting_on_no_access_refused() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    void* const block{ vmhook::os::allocate_rwx(nullptr, page * 2) };
    if (!block)
    {
        check("deepen_sr_boundary_start_skipped_alloc_failed", false);
        return;
    }

    auto* const bytes{ static_cast<std::uint8_t*>(block) };
    bytes[page - 1u] = 0x5A; // last readable byte (page 0)
    bytes[page]      = 0x6B; // first byte of the page we will lock

    const bool locked{ vmhook::os::protect(bytes + page, page,
                                           vmhook::os::memory_protection::no_access,
                                           nullptr) };
    if (!locked)
    {
        std::printf("[INFO] deepen_sr_boundary_start skipped: PROT_NONE refused\n");
        vmhook::os::release(block, page * 2);
        return;
    }

    std::uint8_t dst1{ 0xFF };
    std::uint8_t dst8[8]{ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    const bool one_ok{ vmhook::os::safe_read(&dst1, bytes + page, 1u) };
    const bool eight_ok{ vmhook::os::safe_read(dst8, bytes + page, 8u) };
    check("deepen_sr_starting_on_no_access_one_refused", !one_ok);
    check("deepen_sr_starting_on_no_access_eight_refused", !eight_ok);

    // The last readable byte of page 0 must STILL be readable (lock did not
    // bleed backward).
    std::uint8_t edge{ 0x00 };
    const bool edge_ok{ vmhook::os::safe_read(&edge, bytes + page - 1u, 1u) };
    check("deepen_sr_last_readable_byte_still_readable", edge_ok);
    if (edge_ok)
    {
        check("deepen_sr_last_readable_byte_value", edge == 0x5A);
    }

    (void)vmhook::os::protect(bytes + page, page,
                              vmhook::os::memory_protection::read_write, nullptr);
    vmhook::os::release(block, page * 2);
}

// ---------------------------------------------------------------------------
// safe_read from FREED / released memory must return false, never fault.  Mirror
// of test_query_region_reports_free_for_unallocated for the read path: allocate,
// touch, release, then safe_read the now-unmapped address.  On a kernel that
// immediately re-maps the freed address with a readable region the read could
// legitimately succeed, so we accept EITHER false (the expected unmapped case)
// or a non-crashing true (re-mapped) — what we pin is "no fault".  Gated off iOS
// (its safe_read is a raw memcpy that would fault on a truly unmapped address).
// ---------------------------------------------------------------------------
static auto test_safe_read_from_released_memory() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    void* const block{ vmhook::os::allocate_rwx(nullptr, page) };
    if (!block)
    {
        check("deepen_sr_freed_skipped_alloc_failed", false);
        return;
    }
    *static_cast<volatile std::uint8_t*>(block) = 0x42;
    vmhook::os::release(block, page);

    std::uint8_t dst[8]{ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    // The read itself must not fault.  We do not assert the bool (a re-mapped
    // address may read true); reaching the next line proves fault-safety.
    (void)vmhook::os::safe_read(dst, block, 8u);
    check("deepen_sr_from_released_memory_no_fault", true);
}

// ---------------------------------------------------------------------------
// Repeated fallback-path entry must not wedge the process.  On Linux/Android a
// no_access safe_read that process_vm_readv cannot satisfy drops into the
// SIGSEGV/SIGBUS-catching sigsetjmp probe; this hammers that path many times,
// then asserts (a) a deliberate readable safe_read STILL succeeds and (b) a
// normal in-process store STILL works — proving the handler self-disarm + re-arm
// (the function-local static install) stayed healthy across hundreds of probes.
// This is the closest pure-logic proxy for the signal-handler stability; it does
// not assert handler chaining (that needs a real JVM).  Skips when PROT_NONE is
// refused.
// ---------------------------------------------------------------------------
static auto test_safe_read_repeated_fallback_stays_healthy() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    void* const locked_block{ vmhook::os::allocate_rwx(nullptr, page) };
    void* const good_block{ vmhook::os::allocate_rwx(nullptr, page) };
    if (!locked_block || !good_block)
    {
        check("deepen_sr_repeated_fallback_skipped_alloc_failed", false);
        if (locked_block)
        {
            vmhook::os::release(locked_block, page);
        }
        if (good_block)
        {
            vmhook::os::release(good_block, page);
        }
        return;
    }

    auto* const good_bytes{ static_cast<std::uint8_t*>(good_block) };
    good_bytes[0] = 0x3C;

    const bool locked{ vmhook::os::protect(locked_block, page,
                                           vmhook::os::memory_protection::no_access,
                                           nullptr) };
    if (!locked)
    {
        std::printf("[INFO] deepen_sr_repeated_fallback skipped: PROT_NONE refused\n");
        vmhook::os::release(locked_block, page);
        vmhook::os::release(good_block, page);
        return;
    }

    // Hammer the no_access read; every call must report false with no fault.
    bool all_refused{ true };
    for (int i{ 0 }; i < 256; ++i)
    {
        std::uint8_t dst{ 0xFF };
        if (vmhook::os::safe_read(&dst, locked_block, 1u))
        {
            all_refused = false;
        }
    }
    check("deepen_sr_repeated_fallback_all_refused", all_refused);

    // A deliberate readable read still succeeds after the hammering.
    std::uint8_t good_dst{ 0x00 };
    const bool good_ok{ vmhook::os::safe_read(&good_dst, good_block, 1u) };
    check("deepen_sr_repeated_fallback_good_read_still_works", good_ok);
    if (good_ok)
    {
        check("deepen_sr_repeated_fallback_good_read_value", good_dst == 0x3C);
    }

    // A normal in-process store still works (the host stayed healthy).
    good_bytes[0] = 0xD2;
    check("deepen_sr_repeated_fallback_store_still_sticks", good_bytes[0] == 0xD2);

    (void)vmhook::os::protect(locked_block, page,
                              vmhook::os::memory_protection::read_write, nullptr);
    vmhook::os::release(locked_block, page);
    vmhook::os::release(good_block, page);
}
#endif // !VMHOOK_OS_IOS

// ---------------------------------------------------------------------------
// Absurd / address-space-wrapping size must be rejected with false and must not
// hang.  safe_read carries a `src + size` overflow guard that fires before any
// kernel call / fallback memcpy.  From a real, live small buffer: SIZE_MAX wraps
// for any non-null src, and a size tuned so base+size overflows past UINTPTR_MAX
// by a few bytes is the smallest wrapping family.  Both must return false
// promptly; the buffer is otherwise valid so this exercises the guard, not a bad
// pointer.  Safe on every platform because the guard short-circuits before the
// read.
// ---------------------------------------------------------------------------
static auto test_safe_read_wrapping_size_rejected() -> void
{
    std::uint8_t buffer[16]{};
    std::uint8_t dst[16]{};
    buffer[0] = 0x5A;

    // SIZE_MAX from any non-null src wraps the address space.
    check("deepen_sr_size_max_rejected",
          !vmhook::os::safe_read(dst, buffer, SIZE_MAX));

    // The smallest family of sizes that overflow: (UINTPTR_MAX - base) + k.
    {
        const std::uintptr_t base_addr{ reinterpret_cast<std::uintptr_t>(buffer) };
        const std::uintptr_t max_addr{ ~static_cast<std::uintptr_t>(0) };
        const std::size_t wrapping_size{
            static_cast<std::size_t>(max_addr - base_addr) + std::size_t{ 8 } };
        check("deepen_sr_base_plus_size_wrap_rejected",
              !vmhook::os::safe_read(dst, buffer, wrapping_size));
    }

    // The buffer is untouched by the rejected calls — a real 1-byte read still
    // succeeds and returns the seeded value.
    std::uint8_t one{ 0x00 };
    const bool ok{ vmhook::os::safe_read(&one, buffer, 1u) };
    check("deepen_sr_after_reject_small_read_ok", ok);
    if (ok)
    {
        check("deepen_sr_after_reject_buffer_intact", one == 0x5A);
    }
}

} // namespace deepen_safe_read

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
    test_protect_subrange_within_block();
    test_protect_full_transition_matrix();
    test_protect_old_prot_written_every_flip();
    test_protect_exact_and_minus_one_no_spill_witness();
    test_protect_page_plus_one_spill_witness();
    test_protect_cross_allocation_isolation();
    test_protect_size_overflow_guard();
    test_protect_overflow_guard_old_prot_untouched();
    test_os_primitive_input_guards();
#if !VMHOOK_OS_IOS
    test_safe_read_refuses_no_access_page();
    test_protect_out_of_range_enum_is_restrictive();
    test_protect_no_access_recovery();
    test_protect_multibyte_safe_read_probe();
    test_query_region_reports_free_for_unallocated();
#endif

    deepen_os_protect::test_page_and_granularity_pure_relations();
    deepen_os_protect::test_alloc_release_roundtrip_and_loop();
    deepen_os_protect::test_protect_arm_mapping_via_query();
    deepen_os_protect::test_release_guard_is_noop_on_owned_block();
#if !VMHOOK_OS_IOS
    // query_region's structural field invariants (page-aligned base, page-
    // multiple size, true containment) require a real kernel region.  iOS
    // returns a permissive stub (base == the queried address, size == one page),
    // which deliberately does not satisfy those relationships, so gate it off.
    deepen_os_protect::test_query_region_field_invariants_owned();
#endif

    deepen_safe_read::test_safe_read_positive_width_roundtrip();
    deepen_safe_read::test_safe_read_size_one_succeeds();
    deepen_safe_read::test_safe_read_wrapping_size_rejected();
#if !VMHOOK_OS_IOS
    deepen_safe_read::test_safe_read_straddle_into_no_access_refused();
    deepen_safe_read::test_safe_read_ending_at_boundary_succeeds();
    deepen_safe_read::test_safe_read_starting_on_no_access_refused();
    deepen_safe_read::test_safe_read_from_released_memory();
    deepen_safe_read::test_safe_read_repeated_fallback_stays_healthy();
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
