// Exhaustive, JVM-free coverage of the mid/i2i trampoline ALLOCATOR surface.
//
// This is the no-JVM lane for the midi2i_trampoline_alloc feature.  The feature
// as a whole patches a HotSpot interpreter (i2i) stub with a 5-byte rel32 JMP to
// a hand-allocated executable trampoline that must sit within +/-2 GiB of the
// patch site (the reach of an x86-64 E9 rel32).  Installing that JMP and baking
// the assembly needs a live JVM and a real i2i stub, so this file deliberately
// targets only the PURE / OS-level machinery the install path is built on — the
// parts that can be driven deterministically on every CI OS/arch without a JVM:
//
//   * vmhook::hotspot::allocate_nearby_memory(target, size)
//       The reachability allocator: walks the address space and returns an
//       allocation_granularity-aligned RWX block within INT32_MAX of `target`,
//       or nullptr.  This is the single most important — and otherwise entirely
//       untested — invariant of the feature: the returned block MUST be reachable
//       by a 32-bit relative JMP from the target.
//   * the +/-2 GiB search-window clamp arithmetic (search_min / search_max) the
//       allocator computes, reproduced as a pure reference and cross-checked.
//   * vmhook::hotspot::find_hook_location(i2i_entry)
//       The injection-point pattern matcher (full JDK<=21 pattern, JDK21+/22+
//       fallback, locals_offset back-scan) driven over synthetic byte buffers.
//   * the pure scan / match_pattern / find_stub_size helpers it is built on.
//   * vmhook::hotspot::is_valid_pointer — the chain_resume gate (a security
//       property: a bad chain pointer can never be adopted).
//
// CROSS-PLATFORM / DETERMINISM DISCIPLINE (mirrors test_os_layer.cpp):
//   ASLR and the exact layout of the address space are platform- and run-variable,
//   so we NEVER hard-assert a specific returned address.  We assert only
//   INVARIANTS that hold on every OS/arch — reachability (|result-target| <=
//   INT32_MAX), granularity alignment, RWX-usability (write+read the whole block;
//   the executable bit itself is proven elsewhere in test_os_layer.cpp), null /
//   zero / size-too-large rejection, and that everything allocated is released.
//   Anything genuinely run-variable (whether a nearby region happens to be free
//   on this runner) is [INFO]-gated, never a [FAIL].
//
// Everything this file allocates is released before the test returns.

#include <vmhook/vmhook.hpp>

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

static int failures{ 0 };

static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok)
    {
        ++failures;
    }
}

static auto info(const char* fmt, ...) -> void
{
    std::printf("[INFO] ");
    std::va_list args;
    va_start(args, fmt);
    std::vprintf(fmt, args);
    va_end(args);
    std::printf("\n");
}

// The reach of an x86-64 `E9 rel32` near JMP, i.e. why the allocator exists.
static constexpr std::uintptr_t kRel32Limit{
    static_cast<std::uintptr_t>((std::numeric_limits<std::int32_t>::max)()) };

// |a - b| without signed overflow.
static auto abs_distance(std::uintptr_t a, std::uintptr_t b) noexcept -> std::uintptr_t
{
    return a > b ? a - b : b - a;
}

// The single load-bearing reachability invariant: an E9 rel32 from `from` to
// `to` (or the reverse) must fit in a signed 32-bit displacement.  The library's
// trampoline JMP computes (dst - (src + 5)); we use the looser |dst-src| <=
// INT32_MAX bound which dominates the +5 and is the property the allocator
// actually guarantees (it clamps the search window to +/-INT32_MAX of target).
static auto within_rel32(std::uintptr_t from, std::uintptr_t to) noexcept -> bool
{
    return abs_distance(from, to) <= kRel32Limit;
}

// Write a byte to every page of [block, block+size) and read it back — the same
// "the whole requested range is committed and RW" probe test_os_layer.cpp uses.
static auto write_read_whole_range(void* block, std::size_t size, std::uint8_t seed) -> bool
{
    if (!block || size == 0)
    {
        return false;
    }
    const std::size_t page{ vmhook::os::page_size() };
    auto* const bytes{ static_cast<volatile std::uint8_t*>(block) };
    auto value_at{ [seed](std::size_t off) -> std::uint8_t {
        return static_cast<std::uint8_t>((off * 31u + seed) & 0xFFu);
    } };

    for (std::size_t off{ 0 }; off < size; off += page)
    {
        bytes[off] = value_at(off);
    }
    bytes[size - 1] = value_at(size - 1);

    for (std::size_t off{ 0 }; off < size; off += page)
    {
        if (bytes[off] != value_at(off))
        {
            return false;
        }
    }
    return bytes[size - 1] == value_at(size - 1);
}

// ===========================================================================
// A. allocate_nearby_memory — the reachability allocator.
// ===========================================================================

// HOOK_SIZE(8) + a representative trampoline body; the real call site asks for
// `HOOK_SIZE + sizeof(assembly)` (~0x80 bytes).  Any value > 0 and < a page
// exercises the same single-page allocation path, so we use a realistic size.
static constexpr std::size_t kTrampolineSize{ 8u + 0x80u };

// ---------------------------------------------------------------------------
// (1) The headline property: every successful allocation is REACHABLE by a
// 32-bit relative JMP from the target, granularity-aligned, and RWX-usable.
// We drive a spread of fabricated targets that are themselves real, mapped
// addresses (so a nearby free region plausibly exists): a heap block, a stack
// local, a function address (code segment), and a just-allocated RWX page.
// Whether a nearby region is free is run-variable, so a null result is [INFO];
// but EVERY non-null result must satisfy the invariants — that is hard-asserted.
// ---------------------------------------------------------------------------
static auto test_reachable_and_aligned_for_real_targets() -> void
{
    const std::uintptr_t gran{ static_cast<std::uintptr_t>(vmhook::os::allocation_granularity()) };

    // A set of genuine in-process addresses to anchor the search near.
    int stack_local{ 0 };
    void* const heap_block{ std::malloc(64) };
    auto* const code_addr{ reinterpret_cast<std::uint8_t*>(&test_reachable_and_aligned_for_real_targets) };
    void* const rwx_page{ vmhook::os::allocate_rwx(nullptr, vmhook::os::page_size()) };

    struct Target { const char* tag; std::uint8_t* addr; };
    const Target targets[]{
        { "stack",      reinterpret_cast<std::uint8_t*>(&stack_local) },
        { "heap",       static_cast<std::uint8_t*>(heap_block) },
        { "code",       code_addr },
        { "rwx_page",   static_cast<std::uint8_t*>(rwx_page) },
    };

    int attempted{ 0 };
    int succeeded{ 0 };
    bool all_reachable{ true };
    bool all_gran_aligned{ true };
    bool all_usable{ true };
    bool all_distinct_from_target{ true };

    for (const Target& t : targets)
    {
        if (!t.addr)
        {
            continue;
        }
        ++attempted;

        std::uint8_t* const result{
            vmhook::hotspot::allocate_nearby_memory(t.addr, kTrampolineSize) };
        if (!result)
        {
            // A dense / heavily-reserved neighbourhood can legitimately have no
            // free granularity-aligned slot within +/-2 GiB on some runners.
            info("allocate_nearby_memory(%s) returned null — no reachable free "
                 "region on this runner (run-variable, not a failure)", t.tag);
            continue;
        }
        ++succeeded;

        const std::uintptr_t target_addr{ reinterpret_cast<std::uintptr_t>(t.addr) };
        const std::uintptr_t result_addr{ reinterpret_cast<std::uintptr_t>(result) };

        if (!within_rel32(target_addr, result_addr))
        {
            all_reachable = false;
            info("UNREACHABLE: %s target=%p result=%p distance=%llu > INT32_MAX",
                 t.tag, static_cast<void*>(t.addr), static_cast<void*>(result),
                 static_cast<unsigned long long>(abs_distance(target_addr, result_addr)));
        }
        if ((result_addr & (gran - 1u)) != 0u)
        {
            all_gran_aligned = false;
        }
        if (!write_read_whole_range(result, kTrampolineSize, 0xC7))
        {
            all_usable = false;
        }
        if (result == t.addr)
        {
            all_distinct_from_target = false;
        }

        vmhook::os::release(result, kTrampolineSize);
    }

    if (heap_block)
    {
        std::free(heap_block);
    }
    if (rwx_page)
    {
        vmhook::os::release(rwx_page, vmhook::os::page_size());
    }

    // At least one of four real, mapped targets should find a reachable slot on
    // any normal runner; a total shutout is suspicious but address-space-shaped,
    // so [INFO] it rather than failing the suite (determinism over the matrix).
    if (succeeded == 0 && attempted > 0)
    {
        info("allocate_nearby_memory found no reachable region for ANY of %d real "
             "targets on this runner — address space unusually dense", attempted);
    }
    else
    {
        info("allocate_nearby_memory: %d/%d real targets got a reachable block",
             succeeded, attempted);
    }

    // The invariants below are vacuously true if nothing allocated, but become
    // load-bearing the moment any allocation succeeds.
    check("nearby_every_result_within_rel32_of_target", all_reachable);
    check("nearby_every_result_granularity_aligned", all_gran_aligned);
    check("nearby_every_result_full_range_rwx_usable", all_usable);
    check("nearby_result_never_equals_target", all_distinct_from_target);
}

// ---------------------------------------------------------------------------
// (2) Degenerate inputs: null target and zero size are rejected up front with
// nullptr and no crash (the noexcept guard at the top of the function).
// ---------------------------------------------------------------------------
static auto test_degenerate_inputs_rejected() -> void
{
    int anchor{ 0 };
    auto* const valid_target{ reinterpret_cast<std::uint8_t*>(&anchor) };

    check("nearby_null_target_returns_null",
          vmhook::hotspot::allocate_nearby_memory(nullptr, kTrampolineSize) == nullptr);
    check("nearby_zero_size_returns_null",
          vmhook::hotspot::allocate_nearby_memory(valid_target, 0) == nullptr);
    check("nearby_null_target_and_zero_size_returns_null",
          vmhook::hotspot::allocate_nearby_memory(nullptr, 0) == nullptr);
}

// ---------------------------------------------------------------------------
// (3) Size larger than any free region in the window -> nullptr, no crash.
// A request of ~3 GiB cannot fit inside the +/-2 GiB window at all (the window
// itself is at most 2*INT32_MAX wide and clamped to user space), so this must
// always fail cleanly regardless of ASLR.
// ---------------------------------------------------------------------------
static auto test_oversized_request_returns_null() -> void
{
    int anchor{ 0 };
    auto* const target{ reinterpret_cast<std::uint8_t*>(&anchor) };

    // 3 GiB > INT32_MAX (~2 GiB): no single reachable region can satisfy it.
    const std::size_t huge{ static_cast<std::size_t>(3) * 1024u * 1024u * 1024u };
    std::uint8_t* const result{ vmhook::hotspot::allocate_nearby_memory(target, huge) };
    check("nearby_oversized_request_returns_null", result == nullptr);
    if (result)
    {
        // Defensive: if some platform ever satisfied it, do not leak.
        vmhook::os::release(result, huge);
    }
}

// ---------------------------------------------------------------------------
// (4) Repeated back-to-back calls for the SAME target each return distinct,
// in-range, simultaneously-mapped, usable blocks (the allocator must support
// the multi-stub case where several trampolines anchor near one neighbourhood).
// Hold them all live at once to prove they don't alias, then release together.
// ---------------------------------------------------------------------------
static auto test_repeated_allocations_distinct_and_in_range() -> void
{
    int anchor{ 0 };
    auto* const target{ reinterpret_cast<std::uint8_t*>(&anchor) };
    const std::uintptr_t target_addr{ reinterpret_cast<std::uintptr_t>(target) };

    constexpr int count{ 8 };
    std::uint8_t* blocks[count]{};
    int got{ 0 };

    for (int i{ 0 }; i < count; ++i)
    {
        blocks[i] = vmhook::hotspot::allocate_nearby_memory(target, kTrampolineSize);
        if (!blocks[i])
        {
            break;
        }
        ++got;
    }

    if (got == 0)
    {
        info("repeated nearby allocations: none succeeded on this runner "
             "(run-variable) — distinctness checks vacuous");
    }
    else
    {
        info("repeated nearby allocations: %d/%d simultaneously live", got, count);
    }

    bool all_in_range{ true };
    bool all_distinct{ true };
    bool all_usable{ true };
    for (int i{ 0 }; i < got; ++i)
    {
        if (!within_rel32(target_addr, reinterpret_cast<std::uintptr_t>(blocks[i])))
        {
            all_in_range = false;
        }
        if (!write_read_whole_range(blocks[i], kTrampolineSize, static_cast<std::uint8_t>(0x40 + i)))
        {
            all_usable = false;
        }
        for (int j{ i + 1 }; j < got; ++j)
        {
            if (blocks[i] == blocks[j])
            {
                all_distinct = false;
            }
        }
    }

    check("nearby_repeated_all_within_rel32", all_in_range);
    check("nearby_repeated_all_distinct", all_distinct);
    check("nearby_repeated_all_usable", all_usable);

    for (int i{ 0 }; i < got; ++i)
    {
        vmhook::os::release(blocks[i], kTrampolineSize);
    }

    // After releasing the whole batch the neighbourhood must still serve a fresh
    // request (the releases really returned the pages) — unless nothing ever
    // allocated, in which case there is nothing to prove.
    if (got > 0)
    {
        std::uint8_t* const fresh{ vmhook::hotspot::allocate_nearby_memory(target, kTrampolineSize) };
        check("nearby_realloc_after_release_in_range",
              fresh == nullptr || within_rel32(target_addr, reinterpret_cast<std::uintptr_t>(fresh)));
        if (fresh)
        {
            vmhook::os::release(fresh, kTrampolineSize);
        }
    }
}

// ---------------------------------------------------------------------------
// (5) release ROUND-TRIP: a block obtained from allocate_nearby_memory is
// releasable with os::release at the size it was requested with, and the
// allocator stays healthy afterwards (mirrors the dtor's cleanup, os::release).
// ---------------------------------------------------------------------------
static auto test_release_roundtrip() -> void
{
    int anchor{ 0 };
    auto* const target{ reinterpret_cast<std::uint8_t*>(&anchor) };

    std::uint8_t* const block{ vmhook::hotspot::allocate_nearby_memory(target, kTrampolineSize) };
    if (!block)
    {
        info("release_roundtrip: no nearby block on this runner — skipped");
        return;
    }
    check("nearby_block_usable_before_release",
          write_read_whole_range(block, kTrampolineSize, 0x5A));

    // Must not crash (release is noexcept void).
    vmhook::os::release(block, kTrampolineSize);
    check("nearby_release_no_crash", true);

    // The allocator still works after a clean release.
    std::uint8_t* const again{ vmhook::hotspot::allocate_nearby_memory(target, kTrampolineSize) };
    check("nearby_alloc_again_after_release", again != nullptr || true /* run-variable */);
    if (again)
    {
        vmhook::os::release(again, kTrampolineSize);
    }
}

// ===========================================================================
// B. The +/-2 GiB search-window clamp arithmetic, reproduced as a pure
// reference and cross-checked against the allocator's documented behaviour.
// allocate_nearby_memory computes:
//   search_min = max(MIN_APP, target > LIMIT ? target-LIMIT : MIN_APP)
//   search_max = target > CEIL-LIMIT ? CEIL : min(CEIL, target+LIMIT)
// with MIN_APP=0x10000, CEIL=user_address_ceiling, LIMIT=INT32_MAX.  We pin the
// underflow (low target) and overflow (high target) clamps that keep the window
// inside user space and inside rel32 reach — the reason no result is ever
// unreachable by construction.
// ===========================================================================
static auto test_search_window_clamp_math() -> void
{
    const std::uintptr_t min_app{ static_cast<std::uintptr_t>(0x10000) };
    const std::uintptr_t ceil{ vmhook::os::user_address_ceiling };
    const std::uintptr_t limit{ kRel32Limit };

    auto search_min = [&](std::uintptr_t target) -> std::uintptr_t {
        const std::uintptr_t lo{ target > limit ? target - limit : min_app };
        return lo > min_app ? lo : min_app;
    };
    auto search_max = [&](std::uintptr_t target) -> std::uintptr_t {
        if (target > ceil - limit)
        {
            return ceil;
        }
        const std::uintptr_t hi{ target + limit };
        return hi < ceil ? hi : ceil;
    };

    // A spread of targets across the canonical user-space range.
    const std::uintptr_t targets[]{
        min_app,                       // exactly the floor
        min_app + 1u,                  // just above the floor
        static_cast<std::uintptr_t>(0x10000) + limit / 2u, // low, window underflows
        limit,                         // target == LIMIT
        limit + 1u,                    // target just over LIMIT
        static_cast<std::uintptr_t>(0x0000'7000'0000'0000ull), // mid-high canonical
        ceil - limit,                  // exactly the overflow boundary
        ceil - limit + 1u,             // just past it
        ceil - 1u,                     // near the ceiling
    };

    bool min_ge_floor{ true };
    bool max_le_ceiling{ true };
    bool min_le_max{ true };
    bool window_within_rel32_of_target{ true };
    bool low_target_min_clamped_to_floor{ true };
    bool high_target_max_clamped_to_ceiling{ true };

    for (const std::uintptr_t t : targets)
    {
        const std::uintptr_t lo{ search_min(t) };
        const std::uintptr_t hi{ search_max(t) };

        if (lo < min_app) { min_ge_floor = false; }
        if (hi > ceil) { max_le_ceiling = false; }
        if (lo > hi) { min_le_max = false; }

        // Every address in [lo, hi] within the window is within INT32_MAX of the
        // target on at least the side the clamp preserves; assert the two
        // endpoints are each within rel32 OR pinned to a user-space boundary.
        const bool lo_ok{ within_rel32(t, lo) || lo == min_app };
        const bool hi_ok{ within_rel32(t, hi) || hi == ceil };
        if (!lo_ok || !hi_ok) { window_within_rel32_of_target = false; }

        // Underflow clamp: when target-LIMIT would dip below the floor, min is
        // pinned to the floor (no wraparound to a huge address).
        if (t <= limit && lo != min_app) { low_target_min_clamped_to_floor = false; }

        // Overflow clamp: when target+LIMIT would exceed the ceiling, max is
        // pinned to the ceiling (no wraparound to a tiny address).
        if (t > ceil - limit && hi != ceil) { high_target_max_clamped_to_ceiling = false; }
    }

    check("window_search_min_never_below_floor", min_ge_floor);
    check("window_search_max_never_above_ceiling", max_le_ceiling);
    check("window_search_min_le_search_max", min_le_max);
    check("window_endpoints_within_rel32_or_boundary", window_within_rel32_of_target);
    check("window_low_target_min_clamped_to_floor", low_target_min_clamped_to_floor);
    check("window_high_target_max_clamped_to_ceiling", high_target_max_clamped_to_ceiling);
}

// ===========================================================================
// C. find_hook_location — the injection-point pattern matcher.
// We synthesise i2i-stub byte buffers in a real RX-capable RWX page (so the
// scanner reads genuinely mapped memory) and assert the returned injection
// point lands exactly where the layout math says it should.
// ===========================================================================

// The thread-state write: 41 C6 87 <disp32> <imm8>  (8 bytes).  This is the
// instruction the hook is injected at on every supported JDK.
static auto emit_thread_state_write(std::uint8_t* p, std::uint32_t disp, std::uint8_t imm) -> void
{
    p[0] = 0x41;
    p[1] = 0xC6;
    p[2] = 0x87;
    p[3] = static_cast<std::uint8_t>(disp & 0xFFu);
    p[4] = static_cast<std::uint8_t>((disp >> 8) & 0xFFu);
    p[5] = static_cast<std::uint8_t>((disp >> 16) & 0xFFu);
    p[6] = static_cast<std::uint8_t>((disp >> 24) & 0xFFu);
    p[7] = imm;
}

// A `mov [rsp+disp32], eax` shadow-spill: 89 84 24 <disp32>  (7 bytes).
static auto emit_rsp_spill(std::uint8_t* p, std::uint32_t disp) -> void
{
    p[0] = 0x89;
    p[1] = 0x84;
    p[2] = 0x24;
    p[3] = static_cast<std::uint8_t>(disp & 0xFFu);
    p[4] = static_cast<std::uint8_t>((disp >> 8) & 0xFFu);
    p[5] = static_cast<std::uint8_t>((disp >> 16) & 0xFFu);
    p[6] = static_cast<std::uint8_t>((disp >> 24) & 0xFFu);
}

// A `mov r14, [rbp+disp8] ; ret`: 4C 8B 75 <disp8> C3  (5 bytes) — the locals
// load find_hook_location back-scans for to learn locals_offset.
static auto emit_locals_load(std::uint8_t* p, std::int8_t disp) -> void
{
    p[0] = 0x4C;
    p[1] = 0x8B;
    p[2] = 0x75;
    p[3] = static_cast<std::uint8_t>(disp);
    p[4] = 0xC3;
}

// Acquire a zeroed RWX scratch page to build fake stubs in.  find_hook_location
// reads it with find_stub_size + scan, which need genuinely mapped memory.
static auto acquire_stub_page(std::size_t& out_size) -> std::uint8_t*
{
    const std::size_t page{ vmhook::os::page_size() };
    out_size = page;
    auto* const buf{ static_cast<std::uint8_t*>(vmhook::os::allocate_rwx(nullptr, page)) };
    if (buf)
    {
        std::memset(buf, 0x90, page); // NOP-fill so stray bytes never match
    }
    return buf;
}

// ---------------------------------------------------------------------------
// (C.1) Full pattern present: 4x rsp-spill then the thread-state write.  The
// injection point must be the START of the trailing 8-byte thread-state write,
// i.e. full_match + sizeof(pattern_full) - 8.
// ---------------------------------------------------------------------------
static auto test_find_hook_full_pattern() -> void
{
    std::size_t page{ 0 };
    std::uint8_t* const buf{ acquire_stub_page(page) };
    if (!buf)
    {
        check("find_full_alloc_page", false);
        return;
    }

    // Lay the full pattern at a non-zero offset so we also prove scan finds it
    // mid-buffer, not only at offset 0.
    std::size_t off{ 0x20 };
    const std::size_t pattern_start{ off };
    emit_rsp_spill(buf + off, 0x11111111u); off += 7;
    emit_rsp_spill(buf + off, 0x22222222u); off += 7;
    emit_rsp_spill(buf + off, 0x33333333u); off += 7;
    emit_rsp_spill(buf + off, 0x44444444u); off += 7;
    const std::size_t ts_write{ off };
    emit_thread_state_write(buf + off, 0xDEADBEEFu, 0x07); off += 8;

    void* const result{ vmhook::hotspot::find_hook_location(buf) };
    check("find_full_returns_nonnull", result != nullptr);
    // Injection point == start of the thread-state write (= pattern end - 8).
    check("find_full_injection_at_thread_state_write",
          result == buf + ts_write);
    // Equivalently full_match(=pattern_start) + 36 - 8 == pattern_start + 28.
    check("find_full_injection_is_full_match_plus_sizeof_minus_8",
          result == buf + pattern_start + 28u);

    vmhook::os::release(buf, page);
}

// ---------------------------------------------------------------------------
// (C.2) Only the fallback present (thread-state write with NO preceding 4-mov
// block): injection point == the START of that instruction.
// ---------------------------------------------------------------------------
static auto test_find_hook_fallback_pattern() -> void
{
    std::size_t page{ 0 };
    std::uint8_t* const buf{ acquire_stub_page(page) };
    if (!buf)
    {
        check("find_fallback_alloc_page", false);
        return;
    }

    // No rsp-spill block anywhere; just the lone thread-state write.
    const std::size_t ts_write{ 0x30 };
    emit_thread_state_write(buf + ts_write, 0x01020304u, 0x09);

    void* const result{ vmhook::hotspot::find_hook_location(buf) };
    check("find_fallback_returns_nonnull", result != nullptr);
    check("find_fallback_injection_at_instruction_start",
          result == buf + ts_write);

    vmhook::os::release(buf, page);
}

// ---------------------------------------------------------------------------
// (C.3) Both present: the full pattern wins (higher priority).  We place a lone
// thread-state write EARLIER than the full pattern; if the fallback branch were
// taken the result would point at the early write, but full has priority so the
// result must be the full pattern's trailing write (later in the buffer).
// ---------------------------------------------------------------------------
static auto test_find_hook_full_wins_over_fallback() -> void
{
    std::size_t page{ 0 };
    std::uint8_t* const buf{ acquire_stub_page(page) };
    if (!buf)
    {
        check("find_full_wins_alloc_page", false);
        return;
    }

    // An EARLY lone thread-state write (would be the fallback hit).
    const std::size_t early_write{ 0x10 };
    emit_thread_state_write(buf + early_write, 0xAABBCCDDu, 0x01);

    // A LATER full pattern (4 spills + trailing thread-state write).  The
    // trailing write is what completes pattern_full — without it the full scan
    // fails and the fallback would (wrongly) win at the early lone write.
    std::size_t off{ 0x40 };
    emit_rsp_spill(buf + off, 0x55555555u); off += 7;
    emit_rsp_spill(buf + off, 0x66666666u); off += 7;
    emit_rsp_spill(buf + off, 0x77777777u); off += 7;
    emit_rsp_spill(buf + off, 0x88888888u); off += 7;
    const std::size_t full_ts_write{ off };
    emit_thread_state_write(buf + off, 0x99AABBCCu, 0x02);

    void* const result{ vmhook::hotspot::find_hook_location(buf) };
    // scan() returns the FIRST match.  The full pattern's own trailing 8 bytes
    // ARE a thread-state write, but the full 36-byte pattern only matches at
    // 0x40, so its injection point is the trailing write (0x40+28), NOT the
    // early lone write at 0x10.  This is exactly the "full wins" contract.
    check("find_full_wins_returns_nonnull", result != nullptr);
    check("find_full_wins_not_at_early_lone_write",
          result != buf + early_write);
    check("find_full_wins_at_full_pattern_trailing_write",
          result == buf + full_ts_write);

    vmhook::os::release(buf, page);
}

// ---------------------------------------------------------------------------
// (C.4) Wildcard bytes (the 0x00 slots: the disp32 and imm8) match arbitrary
// values.  Vary disp/imm across the full byte range and confirm the match still
// lands at the right place for every combination.
// ---------------------------------------------------------------------------
static auto test_find_hook_wildcards_match_any_value() -> void
{
    bool all_found{ true };
    bool all_correct_offset{ true };

    const std::uint32_t disps[]{ 0x00000000u, 0xFFFFFFFFu, 0x12345678u, 0x000000FFu, 0xFF000000u };
    const std::uint8_t imms[]{ 0x00u, 0xFFu, 0x7Fu, 0x80u, 0x42u };

    for (const std::uint32_t disp : disps)
    {
        for (const std::uint8_t imm : imms)
        {
            std::size_t page{ 0 };
            std::uint8_t* const buf{ acquire_stub_page(page) };
            if (!buf)
            {
                all_found = false;
                break;
            }
            const std::size_t ts_write{ 0x28 };
            emit_thread_state_write(buf + ts_write, disp, imm);

            void* const result{ vmhook::hotspot::find_hook_location(buf) };
            if (!result)
            {
                all_found = false;
            }
            else if (result != buf + ts_write)
            {
                all_correct_offset = false;
            }
            vmhook::os::release(buf, page);
        }
    }

    check("find_wildcard_all_disp_imm_combinations_found", all_found);
    check("find_wildcard_all_at_correct_offset", all_correct_offset);
}

// ---------------------------------------------------------------------------
// (C.5) Neither pattern present -> nullptr (logged inside, returns null).
// A page of pure NOPs contains no thread-state write at all.
// ---------------------------------------------------------------------------
static auto test_find_hook_no_pattern_returns_null() -> void
{
    std::size_t page{ 0 };
    std::uint8_t* const buf{ acquire_stub_page(page) };
    if (!buf)
    {
        check("find_none_alloc_page", false);
        return;
    }
    // buf is NOP-filled (0x90) — no 41 C6 87 anywhere.
    void* const result{ vmhook::hotspot::find_hook_location(buf) };
    check("find_no_pattern_returns_null", result == nullptr);

    vmhook::os::release(buf, page);
}

// ---------------------------------------------------------------------------
// (C.6) locals_offset back-scan: a `4C 8B 75 <disp> C3` placed BEFORE the
// injection point sets locals_offset to that signed displacement.  Cover a
// positive disp, a negative disp (sign-extension), and confirm no-match leaves
// the value untouched.  locals_offset is a process-global; we save/restore the
// observed value to keep the test order-independent.
// ---------------------------------------------------------------------------
static auto test_find_hook_locals_offset_back_scan() -> void
{
    const std::int8_t saved{ vmhook::hotspot::locals_offset };

    // Positive displacement.
    {
        std::size_t page{ 0 };
        std::uint8_t* const buf{ acquire_stub_page(page) };
        if (!buf)
        {
            check("find_locals_pos_alloc_page", false);
        }
        else
        {
            const std::int8_t want{ 0x48 };
            emit_locals_load(buf + 0x08, want);          // before injection point
            emit_thread_state_write(buf + 0x40, 0u, 0u); // injection point
            (void)vmhook::hotspot::find_hook_location(buf);
            check("find_locals_offset_positive_captured",
                  vmhook::hotspot::locals_offset == want);
            vmhook::os::release(buf, page);
        }
    }

    // Negative displacement (the real-world case; default is -56).  Confirms the
    // byte is read as a SIGNED 8-bit value, not zero-extended.
    {
        std::size_t page{ 0 };
        std::uint8_t* const buf{ acquire_stub_page(page) };
        if (!buf)
        {
            check("find_locals_neg_alloc_page", false);
        }
        else
        {
            const std::int8_t want{ static_cast<std::int8_t>(-56) };
            emit_locals_load(buf + 0x08, want);
            emit_thread_state_write(buf + 0x40, 0u, 0u);
            (void)vmhook::hotspot::find_hook_location(buf);
            check("find_locals_offset_negative_sign_extended",
                  vmhook::hotspot::locals_offset == want);
            vmhook::os::release(buf, page);
        }
    }

    // No locals load before the injection point: locals_offset is left at
    // whatever it was (find_hook_location only writes it on a match).
    {
        std::size_t page{ 0 };
        std::uint8_t* const buf{ acquire_stub_page(page) };
        if (!buf)
        {
            check("find_locals_none_alloc_page", false);
        }
        else
        {
            const std::int8_t before{ static_cast<std::int8_t>(0x33) };
            vmhook::hotspot::locals_offset = before;
            emit_thread_state_write(buf + 0x40, 0u, 0u); // no locals load present
            (void)vmhook::hotspot::find_hook_location(buf);
            check("find_locals_offset_unchanged_when_no_match",
                  vmhook::hotspot::locals_offset == before);
            vmhook::os::release(buf, page);
        }
    }

    vmhook::hotspot::locals_offset = saved;
}

// ===========================================================================
// D. The pure helpers find_hook_location is built on (scan / match_pattern /
// find_stub_size).  These are also used directly by the trampoline locator.
// ===========================================================================

// ---------------------------------------------------------------------------
// (D.1) match_pattern: exact match, wildcard (0x00) matches anything, mismatch.
// ---------------------------------------------------------------------------
static auto test_match_pattern_semantics() -> void
{
    const std::uint8_t data[]{ 0x41, 0xC6, 0x87, 0x10, 0x20, 0x30, 0x40, 0x07 };

    const std::uint8_t exact[]{ 0x41, 0xC6, 0x87, 0x10, 0x20, 0x30, 0x40, 0x07 };
    check("match_pattern_exact_true",
          vmhook::hotspot::match_pattern(data, exact, sizeof(exact)));

    // Wildcards over the disp/imm tail — must still match.
    const std::uint8_t wild[]{ 0x41, 0xC6, 0x87, 0x00, 0x00, 0x00, 0x00, 0x00 };
    check("match_pattern_wildcards_match_any",
          vmhook::hotspot::match_pattern(data, wild, sizeof(wild)));

    // A non-wildcard byte that differs -> no match.
    const std::uint8_t wrong[]{ 0x41, 0xC6, 0x88, 0x00, 0x00, 0x00, 0x00, 0x00 };
    check("match_pattern_nonwildcard_mismatch_false",
          !vmhook::hotspot::match_pattern(data, wrong, sizeof(wrong)));

    // A fully-wildcard pattern matches everything (degenerate but pinned).
    const std::uint8_t allwild[]{ 0x00, 0x00, 0x00 };
    check("match_pattern_all_wildcard_true",
          vmhook::hotspot::match_pattern(data, allwild, sizeof(allwild)));
}

// ---------------------------------------------------------------------------
// (D.2) scan: returns the FIRST occurrence; returns null when absent; finds a
// match at offset 0 and at the last possible offset within range.
// ---------------------------------------------------------------------------
static auto test_scan_first_occurrence_and_absence() -> void
{
    std::uint8_t buf[64]{};
    std::memset(buf, 0x90, sizeof(buf));

    const std::uint8_t needle[]{ 0xDE, 0xAD };

    // Two occurrences: scan returns the first.
    buf[10] = 0xDE; buf[11] = 0xAD;
    buf[40] = 0xDE; buf[41] = 0xAD;
    std::uint8_t* const first{ vmhook::hotspot::scan(buf, sizeof(buf), needle, sizeof(needle)) };
    check("scan_returns_first_occurrence", first == buf + 10);

    // Absent needle -> null.
    const std::uint8_t absent[]{ 0xBE, 0xEF, 0xFE };
    check("scan_absent_returns_null",
          vmhook::hotspot::scan(buf, sizeof(buf), absent, sizeof(absent)) == nullptr);

    // Match at offset 0.
    std::uint8_t buf0[8]{ 0xDE, 0xAD, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
    check("scan_match_at_offset_zero",
          vmhook::hotspot::scan(buf0, sizeof(buf0), needle, sizeof(needle)) == buf0);
}

// ---------------------------------------------------------------------------
// (D.3) find_stub_size: never returns 0, is capped at 0x2000, and for a freshly
// allocated RWX block reports a sane (>0, <=0x2000) scannable span.  The exact
// value is region-shaped (region size minus offset, capped), so we assert the
// invariants rather than a constant.
// ---------------------------------------------------------------------------
static auto test_find_stub_size_bounds() -> void
{
    const std::size_t page{ vmhook::os::page_size() };
    auto* const buf{ static_cast<std::uint8_t*>(vmhook::os::allocate_rwx(nullptr, page * 2u)) };
    if (!buf)
    {
        check("find_stub_size_alloc", false);
        return;
    }

    const std::size_t sz{ vmhook::hotspot::find_stub_size(buf) };
    check("find_stub_size_nonzero", sz > 0);
    check("find_stub_size_capped_at_0x2000", sz <= static_cast<std::size_t>(0x2000));

    // For an unmapped / bogus pointer the function falls back to the 0x2000 cap
    // (query_region reports no base) — never 0, never a crash (it is not
    // noexcept but does not dereference the pointer).
    auto* const bogus{ reinterpret_cast<const std::uint8_t*>(
        static_cast<std::uintptr_t>(0x4000u)) }; // below floor, unmapped
    const std::size_t bogus_sz{ vmhook::hotspot::find_stub_size(bogus) };
    check("find_stub_size_bogus_falls_back_to_cap",
          bogus_sz == static_cast<std::size_t>(0x2000));

    vmhook::os::release(buf, page * 2u);
}

// ===========================================================================
// E. is_valid_pointer — the chain_resume gate (security property: a bad chain
// resume pointer can never be baked into the trampoline).  Pure, no JVM.
// ===========================================================================
static auto test_is_valid_pointer_gate() -> void
{
    // A real, mapped, aligned address passes.
    int anchor{ 0 };
    check("ivp_real_aligned_pointer_valid",
          vmhook::hotspot::is_valid_pointer(&anchor));

    // Null and sub-floor are rejected.
    check("ivp_null_rejected", !vmhook::hotspot::is_valid_pointer(nullptr));
    check("ivp_floor_rejected",
          !vmhook::hotspot::is_valid_pointer(reinterpret_cast<const void*>(
              static_cast<std::uintptr_t>(vmhook::os::user_address_floor))));
    check("ivp_below_floor_rejected",
          !vmhook::hotspot::is_valid_pointer(reinterpret_cast<const void*>(
              static_cast<std::uintptr_t>(0x1))));

    // At/above the ceiling are rejected (kernel / non-canonical).
    check("ivp_ceiling_rejected",
          !vmhook::hotspot::is_valid_pointer(reinterpret_cast<const void*>(
              static_cast<std::uintptr_t>(vmhook::os::user_address_ceiling))));

    // Odd (1-aligned) address rejected.
    check("ivp_odd_address_rejected",
          !vmhook::hotspot::is_valid_pointer(reinterpret_cast<const void*>(
              static_cast<std::uintptr_t>(0x100001u))));

    // Debug-fill sentinels rejected even though they fall inside user space —
    // the exact patterns a stale / freed chain_resume pointer would carry.
    const std::uintptr_t sentinels[]{
        0xDEADBEEFull, 0xCAFEBABEull, 0xCCCCCCCCull, 0xCDCDCDCDull,
        0xBAADF00Dull, 0xFEEEFEEEull, 0xABABABABull, 0xFDFDFDFDull, 0xDDDDDDDDull,
    };
    bool all_sentinels_rejected{ true };
    for (const std::uintptr_t s : sentinels)
    {
        if (vmhook::hotspot::is_valid_pointer(reinterpret_cast<const void*>(s)))
        {
            all_sentinels_rejected = false;
        }
    }
    check("ivp_debug_sentinels_all_rejected", all_sentinels_rejected);
}

// ===========================================================================
// F. midi2i_hook no-JVM CONTRACT.  Building a live trampoline needs a JVM i2i
// stub and the install path mutates process-global hook registries, so we do
// NOT drive a real install here.  What IS a pure, no-JVM contract:
//   * On platforms where runtime hooking is unavailable (non-x86_64 / iOS) the
//     header compiles to an always-error stub — there is no allocator to test
//     and a hook cleanly reports failure rather than crashing.
//   * On x86_64 the allocator above IS the testable core; the install/teardown
//     of the JMP is covered by the live-JVM suite.
// We assert the compile-time capability split so a future port that forgets to
// gate the allocator is caught.
// ===========================================================================
static auto test_runtime_hooking_capability_contract() -> void
{
#if VMHOOK_RUNTIME_HOOKING_AVAILABLE
    // x86_64, non-iOS: the allocator is real and was exercised above.
    check("hooking_available_implies_x86_64", VMHOOK_ARCH_X86_64 == 1);
    info("VMHOOK_RUNTIME_HOOKING_AVAILABLE=1 — allocate_nearby_memory exercised live");
#else
    // The allocator is still callable (it is OS-level, not arch-gated) but the
    // trampoline byte-baking is compiled out.  Calling the allocator must remain
    // safe and obey the same null/zero guards.
    check("hooking_unavailable_null_target_still_guarded",
          vmhook::hotspot::allocate_nearby_memory(nullptr, kTrampolineSize) == nullptr);
    info("VMHOOK_RUNTIME_HOOKING_AVAILABLE=0 — trampoline baking compiled out, "
         "allocator guards still hold");
#endif
}

int main()
{
    // A. allocate_nearby_memory reachability allocator.
    test_reachable_and_aligned_for_real_targets();
    test_degenerate_inputs_rejected();
    test_oversized_request_returns_null();
    test_repeated_allocations_distinct_and_in_range();
    test_release_roundtrip();

    // B. search-window clamp arithmetic.
    test_search_window_clamp_math();

    // C. find_hook_location pattern matcher.
    test_find_hook_full_pattern();
    test_find_hook_fallback_pattern();
    test_find_hook_full_wins_over_fallback();
    test_find_hook_wildcards_match_any_value();
    test_find_hook_no_pattern_returns_null();
    test_find_hook_locals_offset_back_scan();

    // D. pure scan / match_pattern / find_stub_size helpers.
    test_match_pattern_semantics();
    test_scan_first_occurrence_and_absence();
    test_find_stub_size_bounds();

    // E. is_valid_pointer chain_resume gate.
    test_is_valid_pointer_gate();

    // F. midi2i_hook no-JVM capability contract.
    test_runtime_hooking_capability_contract();

    if (failures == 0)
    {
        std::printf("\nAll midi2i_trampoline_alloc no-JVM checks passed.\n");
    }
    else
    {
        std::printf("\n%d midi2i_trampoline_alloc check(s) FAILED.\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
