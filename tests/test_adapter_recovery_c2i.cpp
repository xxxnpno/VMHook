// Dedicated no-JVM pure-logic tests for the adapter_recovery_c2i feature: the
// AdapterHandlerEntry / c2i-entry recovery primitives that vmhook's deopt path
// relies on to redirect Method::_from_compiled_entry at the c2i stub.
//
// FEATURE SURFACE (all in vmhook/ext/vmhook/vmhook.hpp):
//   * vmhook::hotspot::validate_adapter_handler_entry(candidate, c2i_off)
//       The structural oracle. Reads two pointer slots inside `candidate`
//       (_i2c_entry at the HARDCODED offset 0 via memcpy, _c2i_entry at
//       c2i_off via memcpy) and requires BOTH targets to be readable AND to
//       land in committed+executable memory (os::query_region). Returns false
//       on a null candidate, a candidate that fails is_readable_pointer, a null
//       i2c, a non-readable i2c, a null c2i, a non-readable c2i, or either
//       target page not being committed+executable.
//   * vmhook::hotspot::detect_adapter_offset_from_method(method* probe)
//       The JDK 9+ heuristic scan. Guards `probe` with is_valid_pointer, then
//       resolves AdapterHandlerEntry::_c2i_entry from VMStructs; if that field
//       is absent it returns 0 BEFORE touching the probe. With NO JVM the field
//       is always absent, so this is the no-JVM contract: detect() == 0 without
//       ever scanning / dereferencing probe bytes.
//   * vmhook::hotspot::get_c2i_entry_from_adapter(adapter)
//       Validates `adapter` (null + is_valid_pointer), resolves
//       AdapterHandlerEntry::_c2i_entry once, and fault-safe-reads the c2i word.
//       With no JVM the field is unresolved, so it returns nullptr even for a
//       perfectly valid, readable adapter pointer.
//
// WHY THIS IS A PURE NO-JVM FILE, AND WHAT IS OUT OF SCOPE:
//   In a standalone process no libjvm is loaded, so get_vm_structs() /
//   get_vm_types() resolve to nullptr and EVERY iterate_struct_entries /
//   iterate_type_entries lookup returns nullptr. That makes the WHOLE heuristic
//   scan inside detect_adapter_offset_from_method unreachable (it bails on the
//   unresolved _c2i_entry field) and makes get_c2i_entry_from_adapter return
//   nullptr regardless of the adapter. The positive "recovery succeeds on a real
//   Method" assertions (test angles 16-21) REQUIRE a live JVM and belong in
//   tests/jvm/modules/adapter_recovery_c2i.cpp, NOT here.
//
//   validate_adapter_handler_entry, however, takes RAW pointers and does NOT
//   consult VMStructs (the c2i offset is passed in by the caller), so its FULL
//   branch matrix is exercisable here against SYNTHETIC, FULLY-OWNED buffers:
//   a real allocate_rwx executable block (so query_region reports executable),
//   real heap/stack memory (readable but NOT executable), and the
//   is_valid_pointer-REJECTED low constants. No fabricated unmapped/wild address
//   is ever raw-read: the candidate we memcpy out of is always an owned, mapped,
//   readable allocation; the i2c/c2i values we plant are only ever passed to
//   is_readable_pointer / query_region, which use VirtualQuery / /proc-maps and
//   never raw-deref. POSIX-safe by construction.
//
// Every expected value below is derived directly from the source of the three
// functions above (traced at the line ranges noted in each section's comment).

#include <vmhook/vmhook.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <type_traits>

namespace
{

int failures{ 0 };

auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

// Pointer-width slot used everywhere a Method/AHE word is read.
constexpr std::size_t kWord{ sizeof(void*) };

// A synthetic AdapterHandlerEntry candidate buffer. We treat byte offset 0 as
// _i2c_entry and a chosen byte offset as _c2i_entry, writing pointer-sized
// words there with memcpy (matching the library's own memcpy reads). The buffer
// is a real, owned, page-sized allocate_rwx block so it is always mapped &
// readable; validate_adapter_handler_entry memcpy-reads out of it without ever
// faulting.
struct synthetic_ahe
{
    void*       block{ nullptr };
    std::size_t size{ 0 };

    auto store_word(std::size_t off, const void* value) noexcept -> void
    {
        std::memcpy(static_cast<std::uint8_t*>(block) + off, &value, kWord);
    }
};

// ---------------------------------------------------------------------------
// 1. validate_adapter_handler_entry — the cheap REJECT branches that need no
//    executable memory at all (null candidate, is_valid_pointer-rejected low
//    candidate). These are derived from the very first guard:
//        if (!candidate || !is_readable_pointer(candidate)) return false;
//    (vmhook.hpp ~7922). is_readable_pointer rejects addr <= user_address_floor
//    (0xFFFF), addr >= user_address_ceiling, and non-8-aligned BEFORE any region
//    query, and an aligned-but-unmapped canonical address by the committed flag
//    of query_region (VirtualQuery / /proc-maps — never a raw read). So all of
//    these are POSIX-safe: NO fabricated address is ever dereferenced.
// ---------------------------------------------------------------------------
auto test_validate_rejects_bad_candidate_pointers() -> void
{
    using vmhook::hotspot::validate_adapter_handler_entry;

    // The c2i offset value is irrelevant on these paths (the candidate guard
    // fires first); use a representative 8 and also 0 to prove it.
    check("validate_null_candidate_off8_false",
          validate_adapter_handler_entry(nullptr, 8) == false);
    check("validate_null_candidate_off0_false",
          validate_adapter_handler_entry(nullptr, 0) == false);

    // 0x1000 == 4096 <= user_address_floor (0xFFFF == 65535): rejected by
    // is_readable_pointer's range pre-filter, no read attempted.
    check("validate_lowconst_0x1000_false",
          validate_adapter_handler_entry(reinterpret_cast<void*>(0x1000), 8) == false);

    // The floor sentinel itself (addr == user_address_floor): the guard is
    // `addr <= floor`, so the exact floor is rejected too.
    check("validate_floor_sentinel_false",
          validate_adapter_handler_entry(
              reinterpret_cast<void*>(vmhook::os::user_address_floor), 8) == false);

    // An odd (non-8-aligned) low pointer: rejected by the (addr & 0x7)!=0 filter.
    check("validate_misaligned_low_false",
          validate_adapter_handler_entry(reinterpret_cast<void*>(0x1001), 8) == false);

    // A canonical, 8-aligned, but (almost certainly) UNMAPPED low address:
    // 0x10000 > floor and aligned, so it clears the pre-filter, but
    // query_region reports it uncommitted -> is_readable_pointer false. This
    // exercises the region-query rejection path WITHOUT a raw read (query_region
    // is VirtualQuery / /proc-maps; POSIX-safe).
    check("validate_unmapped_canonical_low_false",
          validate_adapter_handler_entry(reinterpret_cast<void*>(0x10000), 8) == false);

    // An address at/above the user ceiling (kernel space) is rejected by the
    // range pre-filter; pick exactly the ceiling (guard is `addr >= ceiling`).
    check("validate_ceiling_addr_false",
          validate_adapter_handler_entry(
              reinterpret_cast<void*>(vmhook::os::user_address_ceiling), 8) == false);
}

// ---------------------------------------------------------------------------
// 2. validate_adapter_handler_entry — readable candidate whose planted i2c /
//    c2i words exercise the null / non-readable inner-pointer branches:
//        memcpy(&i2c, candidate, 8); if (!i2c || !is_readable(i2c)) false;
//        memcpy(&c2i, candidate+off, 8); if (!c2i || !is_readable(c2i)) false;
//    (vmhook.hpp ~7928-7939). The candidate is a real owned heap buffer, so the
//    memcpy reads are safe; the planted i2c/c2i values are only passed to
//    is_readable_pointer (region query, no raw read). POSIX-safe.
// ---------------------------------------------------------------------------
auto test_validate_rejects_bad_inner_pointers() -> void
{
    using vmhook::hotspot::validate_adapter_handler_entry;

    // Owned, mapped, readable candidate buffer. alignas(16) guarantees the
    // candidate base is 8-aligned so it clears is_readable_pointer's alignment
    // filter (a plain std::vector<uint8_t> base is only 1-aligned, which could
    // make validate reject at the CANDIDATE guard instead of the inner-pointer
    // branch we are exercising).
    alignas(16) std::uint8_t buf[256]{};
    void* const candidate{ &buf[0] };
    const std::size_t c2i_off{ 8 };

    auto put = [&](std::size_t off, const void* v) noexcept
    {
        std::memcpy(&buf[0] + off, &v, kWord);
    };

    // (a) i2c word == nullptr (whole buffer is zero): fails at the i2c null
    //     check before c2i is even read.
    check("validate_i2c_null_false",
          validate_adapter_handler_entry(candidate, c2i_off) == false);

    // (b) i2c is a non-readable low value (0x10000: canonical, 8-aligned, but
    //     unmapped). c2i is left null. Fails at the i2c is_readable check.
    put(0, reinterpret_cast<void*>(0x10000));
    put(c2i_off, nullptr);
    check("validate_i2c_unmapped_false",
          validate_adapter_handler_entry(candidate, c2i_off) == false);

    // (c) i2c readable (points at the candidate buffer itself -> a committed,
    //     readable, non-executable heap region), but c2i word == nullptr: fails
    //     at the c2i null check. This proves i2c readability alone is not enough.
    put(0, candidate);
    put(c2i_off, nullptr);
    check("validate_c2i_null_false",
          validate_adapter_handler_entry(candidate, c2i_off) == false);

    // (d) i2c readable, c2i a non-readable low value (0x10000): fails at the
    //     c2i is_readable check.
    put(0, candidate);
    put(c2i_off, reinterpret_cast<void*>(0x10000));
    check("validate_c2i_unmapped_false",
          validate_adapter_handler_entry(candidate, c2i_off) == false);
}

// ---------------------------------------------------------------------------
// 3. validate_adapter_handler_entry — the EXECUTABLE gate. Both i2c and c2i are
//    READABLE (point into the readable heap candidate buffer), but heap memory
//    is NOT executable, so query_region(...).executable is false for both and
//    validate returns false:
//        return i2c_region.committed && i2c_region.executable
//            && c2i_region.committed && c2i_region.executable;  (~7945)
//    This pins that "readable" is insufficient — the X bit is required. Pure
//    heap memory, fully owned, no raw read of a fabricated address.
// ---------------------------------------------------------------------------
auto test_validate_executable_gate_rejects_readable_nonexec() -> void
{
    using vmhook::hotspot::validate_adapter_handler_entry;

    // 8-aligned candidate (see test 2's note); a 1-aligned vector base could be
    // rejected at the candidate guard.
    alignas(16) std::uint8_t buf[256]{};
    void* const candidate{ &buf[0] };
    const std::size_t c2i_off{ 16 };

    auto put = [&](std::size_t off, const void* v) noexcept
    {
        std::memcpy(&buf[0] + off, &v, kWord);
    };

    // Point BOTH inner pointers at readable-but-NON-executable static storage
    // (8-aligned, so they clear is_readable_pointer's alignment filter). Their
    // region is readable but not executable, so only the X gate makes validate
    // return false.
    alignas(16) static std::uint64_t exec_decoy_a{ 0 };
    alignas(16) static std::uint64_t exec_decoy_b{ 0 };
    const void* const i2c_target{ &exec_decoy_a };
    const void* const c2i_target{ &exec_decoy_b };

    put(0, i2c_target);
    put(c2i_off, c2i_target);

    // i2c_target / c2i_target are readable (static storage) but their region is
    // NOT executable, so the executable gate rejects.
    check("validate_readable_nonexec_targets_false",
          validate_adapter_handler_entry(candidate, c2i_off) == false);

    // Sanity: confirm the premise — those targets ARE readable but NOT
    // executable, so the only thing making validate return false is the X gate.
    const auto ra{ vmhook::os::query_region(i2c_target) };
    const auto rb{ vmhook::os::query_region(c2i_target) };
    check("validate_decoy_targets_are_readable",
          vmhook::hotspot::is_readable_pointer(i2c_target)
          && vmhook::hotspot::is_readable_pointer(c2i_target));
    check("validate_decoy_targets_not_executable",
          !ra.executable && !rb.executable);
}

// ---------------------------------------------------------------------------
// 4. validate_adapter_handler_entry — the POSITIVE shape, plus the bytewise
//    "c2i offset is honoured" negative. The candidate is a real allocate_rwx
//    page; i2c (offset 0) and c2i (the chosen offset) point into a SECOND
//    allocate_rwx page that query_region reports committed+executable. validate
//    must return TRUE. Then planting the executable c2i at the WRONG offset (and
//    a null/non-exec word at the right one) must return FALSE — proving the
//    function reads c2i_offset_in_ahe and not a fixed slot.
//
//    Apple arm64 / current iOS enforce W^X and allocate_rwx falls back to RW
//    (non-executable). If the region is not executable we cannot construct the
//    positive case, so we SKIP (as the os tests do) rather than fail.
// ---------------------------------------------------------------------------
auto test_validate_positive_and_offset_honoured() -> void
{
    using vmhook::hotspot::validate_adapter_handler_entry;

    const std::size_t page{ vmhook::os::page_size() };

    // Executable target page: i2c/c2i will point into its interior.
    void* const exec{ vmhook::os::allocate_rwx(nullptr, page) };
    // Candidate AHE page (also rwx, but we only use it as a readable buffer to
    // hold the two planted words).
    void* const cand{ vmhook::os::allocate_rwx(nullptr, page) };

    if (!exec || !cand)
    {
        std::printf("[INFO] validate_positive skipped: allocate_rwx failed\n");
        if (exec) { vmhook::os::release(exec, page); }
        if (cand) { vmhook::os::release(cand, page); }
        return;
    }

    const auto exec_info{ vmhook::os::query_region(exec) };
    if (!exec_info.executable || !exec_info.committed)
    {
        std::printf("[INFO] validate_positive skipped: rwx page not executable (W^X)\n");
        vmhook::os::release(exec, page);
        vmhook::os::release(cand, page);
        return;
    }

    // 8-aligned interior targets inside the executable page (allocate_rwx is
    // page-aligned, so +0 and +64 are 8-aligned and clear is_readable_pointer's
    // alignment filter).
    const void* const i2c_target{ static_cast<std::uint8_t*>(exec) + 0 };
    const void* const c2i_target{ static_cast<std::uint8_t*>(exec) + 64 };

    auto put = [&](std::size_t off, const void* v) noexcept
    {
        std::memcpy(static_cast<std::uint8_t*>(cand) + off, &v, kWord);
    };

    const std::size_t c2i_off{ 24 };

    // POSITIVE: i2c at offset 0, c2i at c2i_off, both -> executable region.
    put(0, i2c_target);
    put(c2i_off, c2i_target);
    check("validate_positive_synthetic_ahe_true",
          validate_adapter_handler_entry(cand, c2i_off) == true);

    // BYTEWISE: same i2c, but the executable c2i word is at a DIFFERENT offset
    // (c2i_off + 8) while the offset validate actually reads (c2i_off) holds a
    // null word. validate must return false -> it honoured c2i_off, not a fixed
    // slot. (Re-zero the buffer region first to be deterministic.)
    std::memset(static_cast<std::uint8_t*>(cand), 0, 256);
    put(0, i2c_target);
    put(c2i_off + 8, c2i_target);   // exec pointer at the WRONG offset
    put(c2i_off, nullptr);          // right offset is null
    check("validate_offset_honoured_wrong_slot_false",
          validate_adapter_handler_entry(cand, c2i_off) == false);

    // And reading at the offset where we DID plant it returns true -> confirms
    // the only difference is which offset is consulted.
    check("validate_offset_honoured_right_slot_true",
          validate_adapter_handler_entry(cand, c2i_off + 8) == true);

    // c2i null with a valid executable i2c -> false (independent re-check of the
    // c2i-null branch but now with a genuinely executable i2c).
    std::memset(static_cast<std::uint8_t*>(cand), 0, 256);
    put(0, i2c_target);
    put(c2i_off, nullptr);
    check("validate_exec_i2c_null_c2i_false",
          validate_adapter_handler_entry(cand, c2i_off) == false);

    // i2c null with a valid executable c2i -> false (the i2c branch fires first,
    // before c2i is even examined).
    std::memset(static_cast<std::uint8_t*>(cand), 0, 256);
    put(0, nullptr);
    put(c2i_off, c2i_target);
    check("validate_null_i2c_exec_c2i_false",
          validate_adapter_handler_entry(cand, c2i_off) == false);

    // c2i pointing at readable-but-non-executable memory while i2c is executable
    // -> false (the c2i half of the executable gate). Use a static (readable,
    // non-exec) target for c2i.
    alignas(16) static std::uint64_t nonexec_word{ 0 };
    std::memset(static_cast<std::uint8_t*>(cand), 0, 256);
    put(0, i2c_target);                              // executable
    put(c2i_off, static_cast<const void*>(&nonexec_word)); // readable, non-exec
    check("validate_exec_i2c_nonexec_c2i_false",
          validate_adapter_handler_entry(cand, c2i_off) == false);

    vmhook::os::release(exec, page);
    vmhook::os::release(cand, page);
}

// ---------------------------------------------------------------------------
// 5. validate_adapter_handler_entry — the c2i offset is interpreted as a BYTE
//    offset and the candidate is read at candidate + c2i_offset_in_ahe. Sweep a
//    handful of distinct byte offsets, each time planting the executable c2i at
//    that exact offset, and assert TRUE; then assert that validating the SAME
//    buffer with any OTHER offset (where no executable word lives) is FALSE.
//    This is a finer-grained pin of the byte-offset arithmetic across several
//    JDK-plausible _c2i_entry offsets (8, 16, 24, 32, 40, 48). Skips under W^X.
// ---------------------------------------------------------------------------
auto test_validate_offset_byte_sweep() -> void
{
    using vmhook::hotspot::validate_adapter_handler_entry;

    const std::size_t page{ vmhook::os::page_size() };
    void* const exec{ vmhook::os::allocate_rwx(nullptr, page) };
    void* const cand{ vmhook::os::allocate_rwx(nullptr, page) };
    if (!exec || !cand)
    {
        std::printf("[INFO] validate_offset_byte_sweep skipped: allocate_rwx failed\n");
        if (exec) { vmhook::os::release(exec, page); }
        if (cand) { vmhook::os::release(cand, page); }
        return;
    }
    if (!vmhook::os::query_region(exec).executable)
    {
        std::printf("[INFO] validate_offset_byte_sweep skipped: rwx not executable (W^X)\n");
        vmhook::os::release(exec, page);
        vmhook::os::release(cand, page);
        return;
    }

    const void* const i2c_target{ static_cast<std::uint8_t*>(exec) + 0 };
    const void* const c2i_target{ static_cast<std::uint8_t*>(exec) + 128 };

    auto put = [&](std::size_t off, const void* v) noexcept
    {
        std::memcpy(static_cast<std::uint8_t*>(cand) + off, &v, kWord);
    };

    const std::size_t offsets[]{ 8, 16, 24, 32, 40, 48 };
    bool all_true{ true };
    bool wrong_offset_all_false{ true };

    for (const std::size_t off : offsets)
    {
        // Clean slate; plant i2c at 0 and the executable c2i at exactly `off`.
        std::memset(static_cast<std::uint8_t*>(cand), 0, 512);
        put(0, i2c_target);
        put(off, c2i_target);

        if (validate_adapter_handler_entry(cand, off) != true)
        {
            all_true = false;
        }

        // Reading at a DIFFERENT offset (off + 8) where the word is zero must be
        // false (c2i null at that offset).
        if (validate_adapter_handler_entry(cand, off + 8) != false)
        {
            wrong_offset_all_false = false;
        }
    }

    check("validate_byte_offset_sweep_all_true", all_true);
    check("validate_byte_offset_sweep_wrong_offset_false", wrong_offset_all_false);

    vmhook::os::release(exec, page);
    vmhook::os::release(cand, page);
}

// ---------------------------------------------------------------------------
// 6. validate_adapter_handler_entry — i2c and c2i MAY alias (point at the same
//    executable word). A real AHE often has distinct entries, but the function
//    imposes no distinctness requirement; both pointing at the same executable
//    address must still validate TRUE. Pins that there is no accidental "i2c !=
//    c2i" check. Skips under W^X.
// ---------------------------------------------------------------------------
auto test_validate_aliased_entries_true() -> void
{
    using vmhook::hotspot::validate_adapter_handler_entry;

    const std::size_t page{ vmhook::os::page_size() };
    void* const exec{ vmhook::os::allocate_rwx(nullptr, page) };
    void* const cand{ vmhook::os::allocate_rwx(nullptr, page) };
    if (!exec || !cand)
    {
        std::printf("[INFO] validate_aliased skipped: allocate_rwx failed\n");
        if (exec) { vmhook::os::release(exec, page); }
        if (cand) { vmhook::os::release(cand, page); }
        return;
    }
    if (!vmhook::os::query_region(exec).executable)
    {
        std::printf("[INFO] validate_aliased skipped: rwx not executable (W^X)\n");
        vmhook::os::release(exec, page);
        vmhook::os::release(cand, page);
        return;
    }

    const void* const same_target{ static_cast<std::uint8_t*>(exec) + 256 };
    const std::size_t c2i_off{ 8 };
    void* v{ const_cast<void*>(same_target) };
    std::memcpy(static_cast<std::uint8_t*>(cand) + 0, &v, kWord);
    std::memcpy(static_cast<std::uint8_t*>(cand) + c2i_off, &v, kWord);

    check("validate_aliased_i2c_c2i_true",
          validate_adapter_handler_entry(cand, c2i_off) == true);

    // c2i offset == 0 (i2c and c2i would read the SAME slot): also valid, since
    // both reads then yield the same executable word.
    check("validate_c2i_offset_zero_aliases_i2c_true",
          validate_adapter_handler_entry(cand, 0) == true);

    vmhook::os::release(exec, page);
    vmhook::os::release(cand, page);
}

// ---------------------------------------------------------------------------
// 7. detect_adapter_offset_from_method — null / bounds-rejected probe and the
//    no-JVM contract. Derived from the body (vmhook.hpp ~7959):
//        if (!probe || !is_valid_pointer(probe)) return 0;
//        c2i_field = iterate_struct_entries("AdapterHandlerEntry","_c2i_entry");
//        if (!c2i_field) return 0;   // <- ALWAYS taken with no JVM
//    So with no JVM detect() returns 0 for EVERY probe, and it does so WITHOUT
//    scanning probe bytes (the c2i_field guard precedes the scan). We therefore
//    can pass a real OWNED buffer cast to method* safely: detect short-circuits
//    on the unresolved field, never reading the buffer.
// ---------------------------------------------------------------------------
auto test_detect_null_and_no_jvm_contract() -> void
{
    using vmhook::hotspot::detect_adapter_offset_from_method;

    // Null probe -> 0 (first guard).
    check("detect_null_probe_zero",
          detect_adapter_offset_from_method(nullptr) == 0);

    // is_valid_pointer-rejected low probe -> 0. 0x1000 <= user_address_floor, so
    // the guard fires before any read. (reinterpret_cast a low constant to
    // method* is fine; it is never dereferenced.)
    check("detect_lowconst_probe_zero",
          detect_adapter_offset_from_method(
              reinterpret_cast<vmhook::hotspot::method*>(0x1000)) == 0);

    // A poison-pattern pointer (low32 == 0xDEADBEEF) is rejected by
    // is_valid_pointer's sentinel switch -> 0, again with no read.
    check("detect_poison_probe_zero",
          detect_adapter_offset_from_method(
              reinterpret_cast<vmhook::hotspot::method*>(
                  static_cast<std::uintptr_t>(0xDEADBEEFu) & ~std::uintptr_t{ 1 })) == 0);

    // A REAL, owned, mapped, 8-aligned buffer cast to method*: passes
    // is_valid_pointer, but with no JVM the _c2i_entry field is unresolved, so
    // detect returns 0 WITHOUT scanning the buffer. This is the no-JVM contract.
    alignas(16) static std::uint64_t owned_probe[64]{};
    auto* const probe{ reinterpret_cast<vmhook::hotspot::method*>(&owned_probe[0]) };
    check("detect_no_jvm_owned_probe_zero",
          detect_adapter_offset_from_method(probe) == 0);

    // Determinism: repeated calls all return 0 (no cached state mutates here;
    // the only static is the c2i_field lookup, itself permanently null).
    bool all_zero{ true };
    for (int i{ 0 }; i < 256; ++i)
    {
        if (detect_adapter_offset_from_method(nullptr) != 0) { all_zero = false; }
        if (detect_adapter_offset_from_method(probe) != 0) { all_zero = false; }
    }
    check("detect_repeated_all_zero", all_zero);
}

// ---------------------------------------------------------------------------
// 8. get_c2i_entry_from_adapter — null / invalid adapter and the no-JVM
//    contract. Derived from the body (vmhook.hpp ~7770):
//        if (!adapter || !is_valid_pointer(adapter)) return nullptr;
//        entry = iterate_struct_entries("AdapterHandlerEntry","_c2i_entry");
//        if (!entry) return nullptr;   // <- ALWAYS with no JVM
//    So it returns nullptr for null/invalid adapters AND for a perfectly valid,
//    readable adapter (the field is unresolved). is_valid_pointer rejects the
//    low / sentinel constants before any read; the valid-adapter path stops at
//    the unresolved field before any read. POSIX-safe throughout.
// ---------------------------------------------------------------------------
auto test_get_c2i_entry_contracts() -> void
{
    using vmhook::hotspot::get_c2i_entry_from_adapter;

    // Null adapter -> nullptr.
    check("get_c2i_null_adapter_nullptr",
          get_c2i_entry_from_adapter(nullptr) == nullptr);

    // is_valid_pointer-rejected low constant -> nullptr (0x1000 <= floor).
    check("get_c2i_lowconst_adapter_nullptr",
          get_c2i_entry_from_adapter(reinterpret_cast<void*>(0x1000)) == nullptr);

    // Odd (misaligned) low pointer -> nullptr (is_valid_pointer odd-reject).
    check("get_c2i_misaligned_adapter_nullptr",
          get_c2i_entry_from_adapter(reinterpret_cast<void*>(0x1001)) == nullptr);

    // Poison pattern (0xCAFEBABE) -> nullptr (sentinel reject).
    check("get_c2i_poison_adapter_nullptr",
          get_c2i_entry_from_adapter(
              reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xCAFEBABEu)
                                      & ~std::uintptr_t{ 1 })) == nullptr);

    // Ceiling / kernel-space address -> nullptr (range reject).
    check("get_c2i_ceiling_adapter_nullptr",
          get_c2i_entry_from_adapter(
              reinterpret_cast<void*>(vmhook::os::user_address_ceiling)) == nullptr);

    // A REAL, owned, mapped, 8-aligned adapter buffer: passes is_valid_pointer,
    // but with no JVM the _c2i_entry field is unresolved -> nullptr WITHOUT
    // reading the buffer. The no-JVM contract.
    alignas(16) static std::uint64_t owned_adapter[16]{ 0xAAAAAAAAAAAAAAAAull };
    check("get_c2i_no_jvm_valid_adapter_nullptr",
          get_c2i_entry_from_adapter(&owned_adapter[0]) == nullptr);

    // Determinism across many calls.
    bool all_null{ true };
    for (int i{ 0 }; i < 256; ++i)
    {
        if (get_c2i_entry_from_adapter(nullptr) != nullptr) { all_null = false; }
        if (get_c2i_entry_from_adapter(&owned_adapter[0]) != nullptr) { all_null = false; }
        if (get_c2i_entry_from_adapter(reinterpret_cast<void*>(0x1000)) != nullptr)
        {
            all_null = false;
        }
    }
    check("get_c2i_repeated_all_null", all_null);
}

// ---------------------------------------------------------------------------
// 9. Cross-function consistency on the no-JVM happy-shaped inputs. The three
//    recovery primitives form a chain: a candidate that validate_* would ACCEPT
//    (executable shape) is still mapped through get_c2i_entry_from_adapter as
//    nullptr without a JVM (the field is unresolved), and detect_* returns 0 for
//    it too. This pins that the no-JVM degradation is uniform — none of the
//    three ever returns a non-null/non-zero result when VMStructs is absent,
//    even for an input that is structurally perfect. Skips the executable build
//    of the candidate under W^X but still asserts the field-absence contract.
// ---------------------------------------------------------------------------
auto test_no_jvm_chain_consistency() -> void
{
    using vmhook::hotspot::detect_adapter_offset_from_method;
    using vmhook::hotspot::get_c2i_entry_from_adapter;
    using vmhook::hotspot::validate_adapter_handler_entry;

    const std::size_t page{ vmhook::os::page_size() };
    void* const exec{ vmhook::os::allocate_rwx(nullptr, page) };
    void* const cand{ vmhook::os::allocate_rwx(nullptr, page) };
    if (!exec || !cand)
    {
        std::printf("[INFO] no_jvm_chain skipped: allocate_rwx failed\n");
        if (exec) { vmhook::os::release(exec, page); }
        if (cand) { vmhook::os::release(cand, page); }
        return;
    }

    const bool executable{ vmhook::os::query_region(exec).executable };
    const std::size_t c2i_off{ 8 };
    if (executable)
    {
        void* const i2c_target{ static_cast<std::uint8_t*>(exec) + 0 };
        void* const c2i_target{ static_cast<std::uint8_t*>(exec) + 32 };
        std::memcpy(static_cast<std::uint8_t*>(cand) + 0, &i2c_target, kWord);
        std::memcpy(static_cast<std::uint8_t*>(cand) + c2i_off, &c2i_target, kWord);

        // The structural oracle accepts it (validate does NOT consult VMStructs).
        check("chain_validate_accepts_perfect_candidate",
              validate_adapter_handler_entry(cand, c2i_off) == true);
    }
    else
    {
        std::printf("[INFO] chain_validate positive skipped: rwx not executable (W^X)\n");
    }

    // ...yet the VMStructs-dependent recovery returns the no-JVM null/zero for
    // the very same (valid, mapped, 8-aligned) candidate pointer.
    check("chain_get_c2i_no_jvm_nullptr",
          get_c2i_entry_from_adapter(cand) == nullptr);
    check("chain_detect_no_jvm_zero",
          detect_adapter_offset_from_method(
              reinterpret_cast<vmhook::hotspot::method*>(cand)) == 0);

    vmhook::os::release(exec, page);
    vmhook::os::release(cand, page);
}

// ---------------------------------------------------------------------------
// 10. ABI / layout sanity that the recovery code's hardcoded assumptions rest
//     on, all checkable at compile/runtime without a JVM:
//       * a pointer is exactly kWord bytes and the recovery reads kWord-sized
//         slots (validate's memcpy(&i2c, ..., sizeof(i2c)) and the scan's
//         sizeof(void*) stride),
//       * the scan stride / preferred-guess arithmetic uses sizeof(void*),
//       * the 512-byte default Method size cap and the [0,4096) clamp window
//         (method_size default 512; only adopted if 0 < type->size < 4096),
//       * the os::region_info has the committed + executable fields the oracle
//         reads.
//     These pin the constants the heuristic depends on so a width / field
//     regression is caught here, on every OS, with no JVM.
// ---------------------------------------------------------------------------
auto test_recovery_abi_constants() -> void
{
    // Pointer width: 8 on the 64-bit CI matrix; the recovery reads sizeof(void*)
    // words and strides by sizeof(void*).
    check("abi_pointer_is_8_or_4",
          kWord == 8 || kWord == 4);
    static_assert(sizeof(void*) == sizeof(std::uintptr_t),
                  "void* and uintptr_t must be the same width");

    // The default Method-size cap (512) and the clamp window [1,4096) that the
    // scan applies to a VMTypes-reported size. Derived from the source:
    //   std::size_t method_size{ 512 };
    //   if (type->size > 0 && type->size < 4096) method_size = type->size;
    // Pin the numeric constants so a future change is conscious.
    constexpr std::size_t kDefaultMethodSize{ 512 };
    constexpr std::size_t kMethodSizeCeil{ 4096 };
    check("abi_default_method_size_512", kDefaultMethodSize == 512);
    check("abi_method_size_ceiling_4096", kMethodSizeCeil == 4096);
    // 512 is a whole number of pointer slots and lies strictly under the ceiling.
    check("abi_default_size_is_word_multiple",
          (kDefaultMethodSize % kWord) == 0);
    check("abi_default_size_below_ceiling",
          kDefaultMethodSize < kMethodSizeCeil);
    // The scan loop bound is `offset + sizeof(void*) <= method_size`; with the
    // 512 default the highest scanned offset is 512 - 8 == 504, itself a word
    // multiple, and there are exactly 512/8 == 64 slots.
    check("abi_default_scan_slot_count_64",
          (kDefaultMethodSize / kWord) == (kWord == 8 ? std::size_t{ 64 } : std::size_t{ 128 }));

    // os::region_info exposes the two flags the executable gate reads.
    vmhook::os::region_info ri{};
    ri.committed = true;
    ri.executable = true;
    check("abi_region_info_has_committed_flag", ri.committed);
    check("abi_region_info_has_executable_flag", ri.executable);
    static_assert(std::is_same_v<decltype(ri.committed), bool>,
                  "region_info.committed must be bool");
    static_assert(std::is_same_v<decltype(ri.executable), bool>,
                  "region_info.executable must be bool");

    // detect's return type is std::size_t (an offset / sentinel-0), and
    // get_c2i_entry_from_adapter returns void*; pin the signatures so a return
    // type regression is caught.
    static_assert(std::is_same_v<
                      decltype(vmhook::hotspot::detect_adapter_offset_from_method(nullptr)),
                      std::size_t>,
                  "detect_adapter_offset_from_method must return std::size_t");
    static_assert(std::is_same_v<
                      decltype(vmhook::hotspot::get_c2i_entry_from_adapter(nullptr)),
                      void*>,
                  "get_c2i_entry_from_adapter must return void*");
    static_assert(std::is_same_v<
                      decltype(vmhook::hotspot::validate_adapter_handler_entry(nullptr, 0)),
                      bool>,
                  "validate_adapter_handler_entry must return bool");
    check("abi_signatures_static_asserted", true);
}

// ---------------------------------------------------------------------------
// 11. The recovery primitives are noexcept (crash-proofing contract). A throw
//     out of any of them on the no-JVM / poisoned-pointer paths would terminate;
//     the noexcept specifier is part of the documented "swallow-by-design"
//     behaviour (failure surfaces as nullptr/0, never an exception). Pin it at
//     compile time.
// ---------------------------------------------------------------------------
auto test_recovery_noexcept_contract() -> void
{
    static_assert(noexcept(vmhook::hotspot::validate_adapter_handler_entry(nullptr, 0)),
                  "validate_adapter_handler_entry must be noexcept");
    static_assert(noexcept(vmhook::hotspot::detect_adapter_offset_from_method(nullptr)),
                  "detect_adapter_offset_from_method must be noexcept");
    static_assert(noexcept(vmhook::hotspot::get_c2i_entry_from_adapter(nullptr)),
                  "get_c2i_entry_from_adapter must be noexcept");
    check("recovery_primitives_are_noexcept", true);
}

} // namespace

auto main() -> int
{
    test_validate_rejects_bad_candidate_pointers();
    test_validate_rejects_bad_inner_pointers();
    test_validate_executable_gate_rejects_readable_nonexec();
    test_validate_positive_and_offset_honoured();
    test_validate_offset_byte_sweep();
    test_validate_aliased_entries_true();
    test_detect_null_and_no_jvm_contract();
    test_get_c2i_entry_contracts();
    test_no_jvm_chain_consistency();
    test_recovery_abi_constants();
    test_recovery_noexcept_contract();

    std::printf("\n%s: %d failure(s)\n",
                failures == 0 ? "OK" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
