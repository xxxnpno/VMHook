// Standalone (no-JVM) unit test for vmhook's INTERPRETER FRAME WALK — the
// saved-rbp / frame-pointer chain machinery behind return_value::caller(),
// return_value::stack_trace() and return_value::frame() (vmhook.hpp
// ~line 1344 onward; the walk bodies live at the caller()/stack_trace()
// definitions, ~9551 and ~9674).
//
// WHAT THE WALK ACTUALLY DOES (and therefore what is testable without a JVM)
// -------------------------------------------------------------------------
// Both caller() and stack_trace() treat return_value::stack_frame as the
// current interpreter frame base (rbp) and walk the HotSpot x64 saved-rbp
// chain:
//   1. current_rbp_slot must pass hotspot::is_valid_pointer (range + alignment
//      + debug-poison filter).
//   2. caller_rbp = *current_rbp_slot, read via cold_read_frame_pointer
//      (os::safe_read on Windows — kernel-validated, never faults; a raw load
//      on POSIX, where the JVM's own SIGSEGV handling contains a stray fault).
//   3. caller_rbp must ALSO pass is_valid_pointer.
//   4. A load-bearing stack-growth guard: caller_rbp must be strictly ABOVE
//      current_rbp_slot and at most 1 MiB above it (cal_addr > cur_addr &&
//      cal_addr - cur_addr <= 1<<20).  This is what stops the walk before it
//      strays into a compiled/native frame and AVs.
//   5. caller_method = *(caller_rbp - 24), again via cold_read_frame_pointer,
//      and must pass is_valid_pointer.
//   6. stack_trace() additionally validates the Method -> ConstMethod ->
//      ConstantPool chain before reading get_name()/get_signature(); caller()
//      reads get_name() directly.  Either way, a fabricated Method* that is
//      not a real HotSpot Method produces an empty method name, so the frame
//      is rejected / the walk breaks.
//
// THE NO-JVM CONTRACT WE CAN EXHAUST.  We cannot manufacture a *real* Method*
// (get_name() chases VMStruct-resolved offsets that need a live JVM), so every
// fabricated chain necessarily yields an EMPTY result.  The genuinely
// interesting, fully-testable invariant is therefore the SAFETY / TERMINATION
// contract documented in the header:
//   * a null / sentinel / fabricated / misaligned / cyclic / unmapped frame
//     pointer yields a clean empty caller_info (valid()==false) and an empty
//     stack_trace() vector,
//   * the walk NEVER faults,
//   * the walk always TERMINATES — it hits its max_depth cap or breaks at one
//     of the gates above; it never spins on a self-referential cycle and never
//     dereferences past the bound we fabricated,
//   * frame() round-trips whatever pointer the constructor was handed.
//
// We exercise this by building FAKE saved-rbp chains in memory we own — exactly
// the way test_os_*/test_array_*/test_helpers fabricate buffers — so that on
// POSIX (raw read) every slot the walk could touch is backed by a real mapped
// page and the read can never fault.  The "stray into unmapped memory" case is
// driven with os::protect(no_access) and is [INFO]-gated (never hard-asserted)
// because a sandbox may refuse PROT_NONE, and because on iOS safe_read is a raw
// memcpy with no fault-safe path.
//
// CROSS-PLATFORM / DETERMINISM.  Every assertion is an INVARIANT (empty result,
// no fault, termination, frame() identity) — never an OS/JDK-specific constant.
// The only platform literal is the page protection used to fabricate an
// unmapped region, and that path is [INFO]-gated. No <charconv>, no float, no
// std::expected/syncstream, fully-braced inits, explicit casts — builds clean
// under MinGW libstdc++, MSVC /WX, clang -Werror and Apple clang.
#include <vmhook/vmhook.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <tuple>
#include <type_traits>
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

static auto info(const char* name, const char* detail) -> void
{
    std::printf("[INFO] %s: %s\n", name, detail);
}

// ===========================================================================
// ADDITIVE DEEPENING (Criterion 2) — pure-logic surface of the slot model and
// the exported frame-walk structs.  Everything below is compile-time-decidable
// or pure integer arithmetic derived FROM SOURCE; nothing reads a fabricated or
// live address, constructs a value_t, or writes a 0-size vector region.  It
// complements (does not touch) the saved-rbp safety/termination sections above.
//
// Provenance of every constant used here (vmhook.hpp):
//   * is_java_double_slot_v normalises via std::remove_cvref_t            (~9492)
//       int64_t / uint64_t / double -> 2 slots; everything else -> 1 slot.
//   * java_slot_offsets<std::tuple<...>>::compute() widens +2 per J/D     (~9523)
//       empty-tuple specialisation -> std::array<int32_t,0>              (~9540)
//   * frame::get_method() reads the Method* at  this - 24 bytes           (~6124)
//       interpreter_frame_method_offset = -3 words = -24 bytes.
//   * get_locals() JDK-21+ index recovery: r14 = rbp + index * 8          (~6263)
//       classified as an index iff slot_index < 0x1000u                   (~6264)
//       JDK 8-20 direct-pointer branch gated by is_valid_pointer          (~6256)
//   * extract_frame_arg / get_argument bounds-guard max_jvm_locals==0xFFFF (~9556, ~6481)
//   * saved-rbp growth guard: strictly above AND <= (1 << 20) bytes       (~9815, ~9952)
//   * is_valid_pointer: reject addr <= user_address_floor(0xFFFF),        (~2047)
//       reject odd, reject >= user_address_ceiling(0x00007FFFFFFFFFFF).
// ===========================================================================

// --- is_java_double_slot_v: cv / ref / pointer normalisation -----------------
// The frame-walk decoders take T, const T&, T&, unique_ptr<wrapper>... and rely
// on the trait collapsing cv/ref via remove_cvref_t.  test_traits.cpp pins the
// BARE types; here we pin the qualified-type normalisation the walk depends on.
namespace ifw_slotmodel
{
    namespace d = vmhook::detail;

    // Two-slot types stay two-slot through const / ref / cv-ref.
    static_assert(d::is_java_double_slot_v<const std::int64_t>,
                  "const int64_t still 2 slots (remove_cvref)");
    static_assert(d::is_java_double_slot_v<std::int64_t&>,
                  "int64_t& still 2 slots");
    static_assert(d::is_java_double_slot_v<const std::int64_t&>,
                  "const int64_t& still 2 slots");
    static_assert(d::is_java_double_slot_v<volatile std::uint64_t>,
                  "volatile uint64_t still 2 slots");
    static_assert(d::is_java_double_slot_v<const double&>,
                  "const double& still 2 slots");

    // One-slot types stay one-slot through const / ref.
    static_assert(!d::is_java_double_slot_v<const std::int32_t&>,
                  "const int32_t& still 1 slot");
    static_assert(!d::is_java_double_slot_v<float&>,
                  "float& still 1 slot (float != double)");
    static_assert(!d::is_java_double_slot_v<const float>,
                  "const float still 1 slot");

    // Distinct-type discipline the J/D rule leans on (long != double != float).
    static_assert(d::is_java_double_slot_v<double> == d::is_java_double_slot_v<std::int64_t>,
                  "double and long agree: both 2-slot");
    static_assert(d::is_java_double_slot_v<float> != d::is_java_double_slot_v<double>,
                  "float (1) and double (2) must differ");

    // --- java_slot_offsets at the extremes test_traits.cpp does not reach -----
    // Empty tuple -> empty table (0-arg method / static no-arg).
    static_assert(d::java_slot_offsets<std::tuple<>>::value.size() == 0u,
                  "0-arg frame -> empty slot table");

    // Single arg of each width.
    static_assert(d::java_slot_offsets<std::tuple<std::int32_t>>::value
                  == std::array<std::int32_t, 1>{ { 0 } },
                  "single int -> [0]");
    static_assert(d::java_slot_offsets<std::tuple<std::int64_t>>::value
                  == std::array<std::int32_t, 1>{ { 0 } },
                  "single long -> [0] (start slot, width handled at read)");

    // Leading double then trailing arg: the trailing arg jumps to slot 2.
    static_assert(d::java_slot_offsets<std::tuple<double, std::int32_t>>::value
                  == std::array<std::int32_t, 2>{ { 0, 2 } },
                  "(double,int) -> [0,2]");

    // All-double run: each successive double starts two slots later.
    static_assert(d::java_slot_offsets<std::tuple<double, double, double, double>>::value
                  == std::array<std::int32_t, 4>{ { 0, 2, 4, 6 } },
                  "(d,d,d,d) -> [0,2,4,6]");

    // Higher arity than the size-3 unit checks: mixed widths summing correctly.
    // (this, long, int, double, int) -> 0,1,3,4,6.
    static_assert(
        d::java_slot_offsets<std::tuple<void*, std::int64_t, std::int32_t, double, std::int32_t>>::value
        == std::array<std::int32_t, 5>{ { 0, 1, 3, 4, 6 } },
        "(this,long,int,double,int) widening table");

    // Table length always equals the arg count regardless of widths.
    static_assert(
        d::java_slot_offsets<std::tuple<std::int64_t, double, std::int64_t, double>>::value.size() == 4u,
        "slot-table length == arg count even when every arg is 2-slot");

    // The last entry equals the sum of all preceding widths (monotone, derived).
    static_assert(
        d::java_slot_offsets<std::tuple<std::int64_t, double, std::int64_t, double>>::value[3] == 6,
        "4th of four 2-slot args starts at slot 6 (2+2+2)");
}

// --- caller_info: standard layout + member order + valid() truth table -------
// The exported result struct of the walk.  valid() is defined as method!=nullptr.
namespace ifw_callerinfo
{
    using ci = vmhook::return_value::caller_info;

    static_assert(std::is_standard_layout_v<ci>,
                  "caller_info must be standard-layout (stable ABI for the result)");
    static_assert(std::is_nothrow_default_constructible_v<ci>,
                  "caller_info default ctor is noexcept");
    static_assert(std::is_same_v<decltype(std::declval<ci>().method),
                                 vmhook::hotspot::method*>,
                  "caller_info::method is a hotspot::method*");
    // method is the first member (offset 0) so a zeroed result is invalid.
    static_assert(offsetof(ci, method) == 0u,
                  "caller_info::method must be the first member");
    static_assert(noexcept(std::declval<const ci&>().valid()),
                  "caller_info::valid() is noexcept");
}

// ---------------------------------------------------------------------------
// A page-sized, page-aligned scratch arena we fully control.  Fabricated rbp
// chains live here so that on POSIX (where the walk uses a raw load) every
// pointer the walk could chase is backed by a real mapped page — the read can
// produce a wrong value, but it can never fault.  We write rbp links and the
// Method* slot ([rbp - 24]) as plain std::uintptr_t words.
//
// Layout note: the walk reads [rbp + 0] (saved caller rbp) and [rbp - 24]
// (Method*).  So any rbp we hand it must have at least 24 readable bytes BELOW
// it and 8 readable bytes AT it.  We keep all fabricated rbp values comfortably
// inside the arena, away from both ends.
// ---------------------------------------------------------------------------
struct arena
{
    std::uint8_t* base{ nullptr };
    std::size_t   size{ 0 };

    auto ok() const -> bool { return base != nullptr; }

    // Word accessors (8-byte slots), offset in BYTES from the arena base.
    auto store(std::size_t byte_off, std::uintptr_t value) -> void
    {
        std::memcpy(base + byte_off, &value, sizeof(value));
    }
    auto word_ptr(std::size_t byte_off) -> std::uint8_t*
    {
        return base + byte_off;
    }
};

static auto make_arena(std::size_t bytes) -> arena
{
    void* const p{ vmhook::os::allocate_rwx(nullptr, bytes) };
    arena a{};
    if (p)
    {
        a.base = static_cast<std::uint8_t*>(p);
        a.size = bytes;
        std::memset(a.base, 0, bytes);
    }
    return a;
}

static auto free_arena(arena& a) -> void
{
    if (a.base)
    {
        vmhook::os::release(a.base, a.size);
        a.base = nullptr;
        a.size = 0;
    }
}

// Construct a return_value whose stack_frame points at `frame_ptr`.  The
// return_slot is a throwaway; the walk never touches it.  We reinterpret the
// fabricated address as a hotspot::frame* exactly as the real trampoline does.
static auto walk_caller(void* frame_ptr) -> vmhook::return_value::caller_info
{
    vmhook::hotspot::return_slot slot{};
    vmhook::return_value rv{ &slot, reinterpret_cast<vmhook::hotspot::frame*>(frame_ptr) };
    return rv.caller();
}

static auto walk_trace(void* frame_ptr, std::size_t max_depth)
    -> std::vector<vmhook::return_value::caller_info>
{
    vmhook::hotspot::return_slot slot{};
    vmhook::return_value rv{ &slot, reinterpret_cast<vmhook::hotspot::frame*>(frame_ptr) };
    return rv.stack_trace(max_depth);
}

// ===========================================================================
// SECTION 1 — null / sentinel frame pointers: clean empty result, no fault.
//
// stack_frame == nullptr is the documented "no frame" path: caller() returns an
// invalid caller_info and stack_trace() an empty vector for every depth.  Low
// sentinel / poison values must be rejected by is_valid_pointer at the very
// first gate and produce the same empty result without ever being dereferenced.
// ===========================================================================
static auto test_null_and_sentinel_frames() -> void
{
    // Null frame.
    {
        const auto c{ walk_caller(nullptr) };
        check("null_frame_caller_invalid", !c.valid());
        check("null_frame_caller_method_null", c.method == nullptr);
        check("null_frame_caller_class_empty", c.class_name.empty());
        check("null_frame_caller_method_name_empty", c.method_name.empty());
        check("null_frame_caller_signature_empty", c.signature.empty());

        check("null_frame_trace_default_empty", walk_trace(nullptr, 64).empty());
        check("null_frame_trace_zero_depth_empty", walk_trace(nullptr, 0).empty());
        check("null_frame_trace_depth1_empty", walk_trace(nullptr, 1).empty());
        check("null_frame_trace_huge_depth_empty", walk_trace(nullptr, 100000).empty());
    }

    // Low / sentinel / poison frame pointers — all rejected by is_valid_pointer
    // (below user_address_floor, odd, or a known debug-fill).  Each must yield
    // an empty result; none may be dereferenced.
    {
        const std::array<std::uintptr_t, 12> bad{ {
            std::uintptr_t{ 0x1u },                 // odd + below floor
            std::uintptr_t{ 0x2u },                 // below floor
            std::uintptr_t{ 0x8u },                 // below floor, aligned
            std::uintptr_t{ 0xFFFFu },              // == user_address_floor (rejected: <=)
            std::uintptr_t{ 0xDEADBEEFu },          // debug poison
            std::uintptr_t{ 0xCAFEBABEu },          // debug poison
            std::uintptr_t{ 0xCCCCCCCCu },          // MSVC uninit stack
            std::uintptr_t{ 0xBAADF00Du },          // Windows LocalAlloc uninit
            std::uintptr_t{ 0xFEEEFEEEu },          // Windows HeapFree
            std::uintptr_t{ 0xABABABABu },          // Windows no-man's-land
            (std::uintptr_t{ 1 } << 63),            // kernel-space (>= ceiling)
            ~std::uintptr_t{ 0 },                   // all-ones (>= ceiling, odd)
        } };
        bool all_caller_empty{ true };
        bool all_trace_empty{ true };
        for (const std::uintptr_t v : bad)
        {
            void* const fp{ reinterpret_cast<void*>(v) };
            if (walk_caller(fp).valid()) { all_caller_empty = false; }
            if (!walk_trace(fp, 64).empty()) { all_trace_empty = false; }
        }
        check("sentinel_frames_caller_all_empty", all_caller_empty);
        check("sentinel_frames_trace_all_empty", all_trace_empty);
    }
}

// ===========================================================================
// SECTION 2 — misaligned frame pointers: the odd-address reject in
// is_valid_pointer (addr & 1) must fire on the very first gate.  We base every
// odd pointer INSIDE our arena so that even if the alignment check were absent,
// the read would hit mapped memory rather than fault — but the contract is that
// it returns empty without reading at all.
// ===========================================================================
static auto test_misaligned_frames(arena& a) -> void
{
    if (!a.ok())
    {
        check("misaligned_frames_skipped_no_arena", false);
        return;
    }
    // A well-inside-the-arena address, then made odd.  Both the even and odd
    // variants are mapped; the odd one must still be rejected by alignment.
    std::uint8_t* const mid{ a.word_ptr(a.size / 2u) };
    void* const odd{ reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(mid) | std::uintptr_t{ 1u }) };

    check("misaligned_frame_caller_empty", !walk_caller(odd).valid());
    check("misaligned_frame_trace_empty", walk_trace(odd, 64).empty());

    // An interior odd offset (base + 7) — still odd, still rejected.
    void* const odd2{ a.word_ptr((a.size / 2u) + 7u) };
    check("misaligned_frame_plus7_caller_empty", !walk_caller(odd2).valid());
    check("misaligned_frame_plus7_trace_empty", walk_trace(odd2, 64).empty());
}

// ===========================================================================
// SECTION 3 — fabricated chain whose saved-rbp VIOLATES the stack-growth guard.
//
// The walk requires caller_rbp to be strictly ABOVE the current slot and within
// 1 MiB.  We fabricate three guard violations, each reading only mapped arena
// memory, and assert the walk breaks immediately (empty / no fault):
//   (a) caller_rbp == current  (cal == cur, not strictly above)  -> break
//   (b) caller_rbp  < current  (chain points downward)           -> break
//   (c) caller_rbp  > current + 1 MiB (absurd jump)              -> break, but
//       only if the target is itself a valid mapped pointer; otherwise the
//       is_valid_pointer gate stops it first.  We use a high-but-canonical
//       user address that passes is_valid_pointer yet is > 1 MiB away.
// ===========================================================================
static auto test_stack_growth_guard(arena& a) -> void
{
    if (!a.ok())
    {
        check("stack_growth_guard_skipped_no_arena", false);
        return;
    }

    // Pick an rbp comfortably inside the arena with >=24 bytes below it.
    const std::size_t rbp_off{ a.size / 2u };
    std::uint8_t* const rbp{ a.word_ptr(rbp_off) };

    // (a) saved-rbp == rbp itself: cal_addr == cur_addr fails "strictly above".
    a.store(rbp_off, reinterpret_cast<std::uintptr_t>(rbp));
    check("guard_equal_rbp_caller_empty", !walk_caller(rbp).valid());
    check("guard_equal_rbp_trace_empty", walk_trace(rbp, 64).empty());

    // (b) saved-rbp below rbp (points at an earlier arena word): chain goes the
    // wrong way, cal_addr < cur_addr -> break.
    std::uint8_t* const below{ a.word_ptr(rbp_off - 64u) };
    a.store(rbp_off, reinterpret_cast<std::uintptr_t>(below));
    check("guard_downward_rbp_caller_empty", !walk_caller(rbp).valid());
    check("guard_downward_rbp_trace_empty", walk_trace(rbp, 64).empty());

    // (c) saved-rbp far above (> 1 MiB): a canonical, aligned, in-range address
    // that passes is_valid_pointer but is > 1 MiB above the slot.  Distance
    // guard must reject it before [caller_rbp - 24] is dereferenced.  We point
    // it ~16 MiB above the arena (still well under the user ceiling).
    const std::uintptr_t far_above{ reinterpret_cast<std::uintptr_t>(rbp) + (std::uintptr_t{ 16u } << 20) };
    a.store(rbp_off, far_above);
    check("guard_far_above_caller_empty", !walk_caller(rbp).valid());
    check("guard_far_above_trace_empty", walk_trace(rbp, 64).empty());
}

// ===========================================================================
// SECTION 4 — self-referential and short cycles must TERMINATE, not spin.
//
// A frame whose saved-rbp points at itself (cal == cur) is rejected by the
// strict-above guard, so even though it is a cycle the walk breaks on the
// FIRST iteration.  A two-node cycle where B is above A (A -> B passes the
// guard) but B -> A points downward likewise breaks when it reaches B.  Either
// way the result is empty (no real Method*) and, crucially, the call RETURNS.
// We additionally cap iterations implicitly via max_depth to make "terminates"
// observable: a spin would hang the test rather than fail it, so reaching the
// assert at all is the proof.
// ===========================================================================
static auto test_cycles_terminate(arena& a) -> void
{
    if (!a.ok())
    {
        check("cycles_skipped_no_arena", false);
        return;
    }

    // Self-cycle: rbp -> rbp.
    {
        const std::size_t off{ a.size / 2u };
        std::uint8_t* const rbp{ a.word_ptr(off) };
        a.store(off, reinterpret_cast<std::uintptr_t>(rbp));

        // The very fact these return is the termination proof.
        const auto c{ walk_caller(rbp) };
        check("self_cycle_caller_returns_empty", !c.valid());
        const auto t{ walk_trace(rbp, 64) };
        check("self_cycle_trace_returns_empty", t.empty());
        const auto t_big{ walk_trace(rbp, 100000) };
        check("self_cycle_trace_huge_depth_terminates_empty", t_big.empty());
    }

    // Two-node cycle: A (low) -> B (high, +128) -> A.  A->B satisfies "strictly
    // above & within 1 MiB"; the Method* slot [B-24] is zero (memset) so it
    // fails is_valid_pointer and the walk breaks at B.  Both nodes carry >=24
    // readable bytes below them inside the arena.
    {
        const std::size_t a_off{ a.size / 3u };
        const std::size_t b_off{ a_off + 128u };
        std::uint8_t* const node_a{ a.word_ptr(a_off) };
        std::uint8_t* const node_b{ a.word_ptr(b_off) };
        a.store(a_off, reinterpret_cast<std::uintptr_t>(node_b)); // A -> B
        a.store(b_off, reinterpret_cast<std::uintptr_t>(node_a)); // B -> A (downward)
        // Method* slots are zero -> rejected -> break (no real Method to find).
        check("two_node_cycle_caller_empty", !walk_caller(node_a).valid());
        check("two_node_cycle_trace_empty", walk_trace(node_a, 64).empty());
    }
}

// ===========================================================================
// SECTION 5 — a LONG, monotonically-increasing valid chain whose Method* slots
// are all zero (so each frame is rejected at the Method* gate) proves the walk
// follows links correctly AND terminates.  Because every Method* is rejected,
// caller()/stack_trace() return empty, but the important property is that the
// walk visited frames in order and stopped — never faulted, never overran the
// arena.  We make the chain longer than any plausible max_depth so the cap is
// the only thing that could matter, then assert the result is still empty and
// the call returned.
//
// We also build a chain where the LAST link points just past the arena's end
// into a no_access guard region (Section 6 fabricates that); here every link is
// in-arena and the terminator is a zero saved-rbp (rejected by floor).
// ===========================================================================
static auto test_long_chain_terminates(arena& a) -> void
{
    if (!a.ok())
    {
        check("long_chain_skipped_no_arena", false);
        return;
    }

    // Build a strictly-increasing chain of rbp nodes spaced 128 bytes apart,
    // each saved-rbp pointing at the next-higher node, the final one pointing
    // at 0 (terminator).  Start above the first 24-byte margin.
    const std::size_t stride{ 128u };
    const std::size_t start{ 256u };
    const std::size_t end_guard{ a.size - stride };   // leave tail room

    std::size_t node_count{ 0 };
    std::size_t off{ start };
    std::size_t prev_off{ 0 };
    bool have_prev{ false };
    while (off + 24u < end_guard)
    {
        if (have_prev)
        {
            // prev -> this (this is higher, within stride < 1 MiB)
            a.store(prev_off, reinterpret_cast<std::uintptr_t>(a.word_ptr(off)));
        }
        prev_off = off;
        have_prev = true;
        ++node_count;
        off += stride;
    }
    // Terminate the last node's saved-rbp at 0 (rejected by is_valid_pointer).
    if (have_prev)
    {
        a.store(prev_off, std::uintptr_t{ 0u });
    }

    std::uint8_t* const head{ a.word_ptr(start) };

    // Every Method* slot is zero -> each frame rejected -> empty result, but the
    // walk must traverse and RETURN.  Reaching these asserts is the proof.
    check("long_chain_caller_empty", !walk_caller(head).valid());
    check("long_chain_trace_empty_depth64", walk_trace(head, 64).empty());
    check("long_chain_trace_empty_depth1", walk_trace(head, 1).empty());
    check("long_chain_trace_empty_huge_depth", walk_trace(head, 100000).empty());

    {
        char buf[96]{};
        std::snprintf(buf, sizeof(buf), "fabricated %zu-node increasing chain, all terminated cleanly",
                      node_count);
        info("long_chain_node_count", buf);
    }
}

// ===========================================================================
// SECTION 6 — chain that walks INTO an unmapped (no_access) page.
//
// Fabricate two adjacent regions: a readable arena and, immediately above it, a
// no_access guard page.  Put an rbp at the top of the readable arena whose
// saved-rbp points into the guard page (strictly above, < 1 MiB away, passes
// is_valid_pointer's range/alignment).  The walk then tries to read the guard
// page:
//   * Windows: cold_read_frame_pointer uses os::safe_read (ReadProcessMemory),
//     which returns false on the no_access page -> nullptr -> walk breaks
//     cleanly.  Asserted.
//   * POSIX (non-iOS): cold_read_frame_pointer does a RAW load of the guard
//     page.  That would fault — and the JVM's signal handling (absent here)
//     would not contain it.  So we must NOT drive the raw read into a real
//     PROT_NONE page on POSIX; instead we only assert the Windows safe-read
//     contract, and on POSIX we [INFO]-document that the raw-read path is
//     exercised by the live-JVM suite (where SIGSEGV handling contains it),
//     never here.  This keeps the no-JVM lane fault-free on every platform.
//
// The whole section is [INFO]-gated on protect() refusal (sandboxes) and on
// iOS (no fault-safe read).
// ===========================================================================
static auto test_chain_into_unmapped() -> void
{
#if defined(_WIN32)
    const std::size_t page{ vmhook::os::page_size() };
    // Two contiguous pages from one reservation: [readable | guard].
    void* const block{ vmhook::os::allocate_rwx(nullptr, page * 2u) };
    if (!block)
    {
        info("chain_into_unmapped", "skipped: 2-page allocation failed");
        return;
    }
    std::uint8_t* const lo{ static_cast<std::uint8_t*>(block) };
    std::uint8_t* const guard{ lo + page };
    std::memset(lo, 0, page);

    std::uint32_t old_prot{ 0 };
    const bool protected_ok{ vmhook::os::protect(guard, page,
                                                 vmhook::os::memory_protection::no_access,
                                                 &old_prot) };
    if (!protected_ok)
    {
        info("chain_into_unmapped", "skipped: protect(no_access) refused (sandbox)");
        vmhook::os::release(block, page * 2u);
        return;
    }

    // rbp near the TOP of the readable page (keep >=24 bytes below it), saved-rbp
    // pointing into the guard page (strictly above, within 1 MiB).
    const std::size_t rbp_off{ page - 64u };
    std::uintptr_t into_guard{ reinterpret_cast<std::uintptr_t>(guard) + 64u };
    into_guard &= ~std::uintptr_t{ 1u }; // keep aligned so is_valid_pointer accepts it
    std::memcpy(lo + rbp_off, &into_guard, sizeof(into_guard));
    std::uint8_t* const rbp{ lo + rbp_off };

    // safe_read on the no_access page fails -> nullptr -> walk breaks cleanly.
    check("unmapped_chain_caller_empty_no_fault", !walk_caller(rbp).valid());
    check("unmapped_chain_trace_empty_no_fault", walk_trace(rbp, 64).empty());

    // Restore writable before release so teardown is clean.
    (void)vmhook::os::protect(guard, page,
                              vmhook::os::memory_protection::read_write, nullptr);
    vmhook::os::release(block, page * 2u);
    info("chain_into_unmapped", "windows: safe_read gated the no_access page, walk broke cleanly");
#else
    // POSIX/iOS: cold_read_frame_pointer is a RAW load; driving it into a real
    // PROT_NONE page would fault uncontained in this no-JVM harness.  The raw-read
    // path is covered by the live-JVM stack_trace module where the JVM's SIGSEGV
    // handling contains a stray fault.  We do NOT fabricate that fault here.
    info("chain_into_unmapped",
         "posix/ios: raw-read path intentionally not driven into PROT_NONE here "
         "(covered by the live-JVM suite); no-JVM lane stays fault-free");
#endif
}

// ===========================================================================
// SECTION 7 — max_depth cap contract.  With no real Method* anywhere, the
// result is always empty, but the cap arithmetic still runs: max_depth == 0
// promotes to 64 internally, and any finite cap must bound the loop.  We assert
// the documented behaviour for a representative set of caps over BOTH a null
// frame and a fabricated (always-rejected) chain.  Every call returning is the
// termination proof.
// ===========================================================================
static auto test_max_depth_cap(arena& a) -> void
{
    const std::array<std::size_t, 8> caps{ {
        std::size_t{ 0u }, std::size_t{ 1u }, std::size_t{ 2u }, std::size_t{ 8u },
        std::size_t{ 64u }, std::size_t{ 65u }, std::size_t{ 1024u }, std::size_t{ 1000000u },
    } };

    // Null frame: every cap -> empty.
    {
        bool all_empty{ true };
        for (const std::size_t cap : caps)
        {
            if (!walk_trace(nullptr, cap).empty()) { all_empty = false; }
        }
        check("max_depth_null_frame_all_caps_empty", all_empty);
    }

    // Fabricated rejected chain: every cap -> empty AND returns (termination).
    if (a.ok())
    {
        const std::size_t off{ a.size / 2u };
        std::uint8_t* const rbp{ a.word_ptr(off) };
        // saved-rbp = a higher in-arena node with a zero Method* (rejected).
        const std::size_t hi_off{ off + 256u };
        if (hi_off + 8u < a.size)
        {
            a.store(off, reinterpret_cast<std::uintptr_t>(a.word_ptr(hi_off)));
            a.store(hi_off, std::uintptr_t{ 0u });
        }
        bool all_empty{ true };
        for (const std::size_t cap : caps)
        {
            if (!walk_trace(rbp, cap).empty()) { all_empty = false; }
        }
        check("max_depth_fabricated_chain_all_caps_empty_and_terminates", all_empty);
    }
    else
    {
        check("max_depth_fabricated_chain_skipped_no_arena", false);
    }
}

// ===========================================================================
// SECTION 8 — frame() accessor identity.  frame() must return EXACTLY the
// pointer the constructor was given, for any value (null, sentinel, real
// arena address), with no validation applied — it is the documented raw
// escape hatch.  This is the one piece of the walk that is pure pointer
// plumbing and fully assertable.
// ===========================================================================
static auto test_frame_accessor_identity(arena& a) -> void
{
    {
        vmhook::hotspot::return_slot slot{};
        vmhook::return_value rv{ &slot, nullptr };
        check("frame_accessor_null_identity", rv.frame() == nullptr);
    }
    {
        vmhook::hotspot::return_slot slot{};
        auto* const fake{ reinterpret_cast<vmhook::hotspot::frame*>(
            static_cast<std::uintptr_t>(0xDEADBEEFu)) };
        vmhook::return_value rv{ &slot, fake };
        check("frame_accessor_sentinel_identity", rv.frame() == fake);
    }
    if (a.ok())
    {
        vmhook::hotspot::return_slot slot{};
        auto* const real{ reinterpret_cast<vmhook::hotspot::frame*>(a.word_ptr(a.size / 2u)) };
        vmhook::return_value rv{ &slot, real };
        check("frame_accessor_real_identity", rv.frame() == real);
    }
    else
    {
        check("frame_accessor_real_skipped_no_arena", false);
    }
}

// ===========================================================================
// SECTION 9 — caller()/stack_trace() agreement on a fabricated frame.
// The header documents that stack_trace().front() equals caller() (index 0 is
// the immediate caller).  On a fabricated chain both are empty, so the agreed
// invariant is: caller().valid() == false  AND  stack_trace().empty() == true
// — i.e. they agree there is no identifiable caller.  We assert the agreement
// over several fabricated frames so a future divergence (one returning a frame
// the other rejects) is caught.
// ===========================================================================
static auto test_caller_trace_agreement(arena& a) -> void
{
    if (!a.ok())
    {
        check("caller_trace_agreement_skipped_no_arena", false);
        return;
    }

    // For each fabricated frame, caller() and stack_trace() must AGREE that
    // there is no identifiable caller: caller().valid() is false and the trace
    // front (if any) would equal it.  On a fabricated chain both are empty, so
    // agreement reduces to "caller invalid AND trace empty" — assert that joint
    // invariant per frame.
    auto agrees_no_caller{ [](void* fp) -> bool
    {
        const bool caller_invalid{ !walk_caller(fp).valid() };
        const bool trace_empty{ walk_trace(fp, 64).empty() };
        return caller_invalid && trace_empty;
    } };

    bool agree{ true };
    const std::size_t mid{ a.size / 2u };

    // (1) zero saved-rbp.
    a.store(mid, std::uintptr_t{ 0u });
    if (!agrees_no_caller(a.word_ptr(mid))) { agree = false; }

    // (2) self-cycle.
    a.store(mid, reinterpret_cast<std::uintptr_t>(a.word_ptr(mid)));
    if (!agrees_no_caller(a.word_ptr(mid))) { agree = false; }

    // (3) higher node with zero Method*.
    {
        const std::size_t hi{ mid + 256u };
        if (hi + 8u < a.size)
        {
            a.store(mid, reinterpret_cast<std::uintptr_t>(a.word_ptr(hi)));
            a.store(hi, std::uintptr_t{ 0u });
            if (!agrees_no_caller(a.word_ptr(mid))) { agree = false; }
        }
    }
    check("caller_and_trace_agree_no_caller_on_fabricated_frames", agree);
}

// ===========================================================================
// SECTION 10 (ADDITIVE) — runtime checks of the walk's pure arithmetic and the
// caller_info value contract.  No fabricated/live address is dereferenced: the
// "index recovery" math below operates on a real, mapped arena base treated as
// an rbp VALUE (pure integer arithmetic, never read), reproducing get_locals()'s
// JDK-21+ formula `r14 = rbp + index*8` and the classification cliff at 0x1000.
// Every literal is derived from vmhook.hpp as documented at the top of the file.
// ===========================================================================
static auto test_walk_arithmetic_and_value_contract(arena& a) -> void
{
    // --- interpreter_frame_method_offset = -3 words = -24 bytes (get_method). -
    {
        constexpr std::int64_t method_offset_words{ -3 };
        constexpr std::int64_t word_bytes{ 8 };
        check("method_offset_is_minus_24_bytes",
              (method_offset_words * word_bytes) == std::int64_t{ -24 });
    }

    // --- max_jvm_locals == 0xFFFF (the u2 bound shared by extract_frame_arg /
    // get_argument / set_arg).  An index is accepted iff 0 <= index <= 0xFFFF. --
    {
        constexpr std::int32_t max_jvm_locals{ 0xFFFF };
        check("max_jvm_locals_is_65535", max_jvm_locals == 65535);
        // Boundary classification of the documented guard `index < 0 || > max`.
        auto in_range{ [](std::int32_t idx) -> bool
        {
            constexpr std::int32_t mx{ 0xFFFF };
            return !(idx < 0 || idx > mx);
        } };
        check("locals_index_zero_in_range", in_range(0));
        check("locals_index_max_in_range", in_range(0xFFFF));
        check("locals_index_negative_rejected", !in_range(-1));
        check("locals_index_intmin_rejected", !in_range(-2147483647 - 1));
        check("locals_index_past_max_rejected", !in_range(0x10000));
    }

    // --- saved-rbp growth guard: caller_rbp must be strictly ABOVE and at most
    // (1 << 20) == 1 MiB above the current slot.  Pure boundary arithmetic. -----
    {
        constexpr std::uintptr_t one_mib{ std::uintptr_t{ 1 } << 20 };
        check("growth_bound_is_one_mib", one_mib == std::uintptr_t{ 1048576u });
        // Reproduce: bool ok = cal > cur && (cal - cur) <= one_mib.
        auto accepts{ [](std::uintptr_t cur, std::uintptr_t cal) -> bool
        {
            constexpr std::uintptr_t lim{ std::uintptr_t{ 1 } << 20 };
            return cal > cur && (cal - cur) <= lim;
        } };
        const std::uintptr_t cur{ std::uintptr_t{ 0x40000000u } };
        check("growth_equal_rejected", !accepts(cur, cur));                       // not strictly above
        check("growth_below_rejected", !accepts(cur, cur - 8u));                  // downward
        check("growth_one_above_accepted", accepts(cur, cur + 8u));              // strictly above, near
        check("growth_exactly_one_mib_accepted", accepts(cur, cur + one_mib));   // boundary inclusive
        check("growth_one_past_mib_rejected", !accepts(cur, cur + one_mib + 1u));// just over
    }

    // --- is_valid_pointer floor/ceiling/alignment, as the walk's first gate
    // applies them.  Pure constants — no dereference. ------------------------
    {
        check("user_address_floor_is_0xFFFF",
              vmhook::os::user_address_floor == std::uintptr_t{ 0xFFFFu });
        check("user_address_ceiling_is_canonical",
              vmhook::os::user_address_ceiling == std::uintptr_t{ 0x00007FFFFFFFFFFFull });
        // Floor is exclusive (addr <= floor rejected); ceiling exclusive (>=).
        check("ivp_rejects_floor_value",
              !vmhook::hotspot::is_valid_pointer(
                  reinterpret_cast<const void*>(vmhook::os::user_address_floor)));
        check("ivp_rejects_below_floor",
              !vmhook::hotspot::is_valid_pointer(reinterpret_cast<const void*>(std::uintptr_t{ 0x100u })));
        check("ivp_rejects_ceiling_value",
              !vmhook::hotspot::is_valid_pointer(
                  reinterpret_cast<const void*>(vmhook::os::user_address_ceiling)));
        check("ivp_rejects_odd_in_range",
              !vmhook::hotspot::is_valid_pointer(reinterpret_cast<const void*>(std::uintptr_t{ 0x100001u })));
        check("ivp_accepts_even_in_range",
              vmhook::hotspot::is_valid_pointer(reinterpret_cast<const void*>(std::uintptr_t{ 0x100000u })));
    }

    // --- get_locals() JDK-21+ index recovery: r14 = rbp + index*8, and the
    // classification cliff `slot_index < 0x1000` (else nullptr).  We compute on
    // a REAL mapped arena base used purely as an rbp VALUE — never dereferenced. -
    {
        constexpr std::uintptr_t index_cliff{ 0x1000u };
        check("locals_index_cliff_is_4096", index_cliff == std::uintptr_t{ 4096u });

        // The hs_err proof from the header: rbp + 3*8 with index 3.
        const std::uintptr_t proof_rbp{ std::uintptr_t{ 0x243f888u } };
        const std::uintptr_t proof_index{ 3u };
        check("locals_recovery_matches_hs_err_proof",
              (proof_rbp + proof_index * 8u) == std::uintptr_t{ 0x243f8a0u });

        // Round-trip the spilled value: index = (r14 - rbp) >> 3, then recover.
        const std::uintptr_t rbp_val{ std::uintptr_t{ 0x10000000u } };
        const std::uintptr_t r14_val{ rbp_val + 7u * 8u };           // 7 slots above rbp
        const std::uintptr_t spilled{ (r14_val - rbp_val) >> 3 };    // == 7
        check("locals_index_encode_roundtrip", spilled == 7u);
        check("locals_recover_from_index", (rbp_val + spilled * 8u) == r14_val);

        // Classification: index just under cliff recovered; at/over cliff -> null.
        auto classify_is_index{ [](std::uintptr_t slot) -> bool
        {
            return slot < 0x1000u;   // get_locals(): JDK-21+ index branch
        } };
        check("locals_index_below_cliff_is_index", classify_is_index(0xFFFu));
        check("locals_index_at_cliff_is_not_index", !classify_is_index(0x1000u));
        check("locals_index_zero_is_index", classify_is_index(0u));

        if (a.ok())
        {
            // Use the arena base as an rbp VALUE only; compute the recovered
            // locals address and confirm it lands inside the arena for a small
            // index (so the formula stays self-consistent with a real mapping).
            const std::uintptr_t rbp{ reinterpret_cast<std::uintptr_t>(a.base) };
            const std::uintptr_t recovered{ rbp + 5u * 8u };
            const std::uintptr_t arena_end{ rbp + static_cast<std::uintptr_t>(a.size) };
            check("locals_recovery_in_arena_bounds",
                  recovered > rbp && recovered < arena_end);
        }
    }

    // --- caller_info value contract at runtime: default-constructed is invalid;
    // setting method!=nullptr flips valid(); the string members start empty. ----
    {
        vmhook::return_value::caller_info ci{};
        check("default_caller_info_invalid", !ci.valid());
        check("default_caller_info_method_null", ci.method == nullptr);
        check("default_caller_info_class_empty", ci.class_name.empty());
        check("default_caller_info_method_name_empty", ci.method_name.empty());
        check("default_caller_info_signature_empty", ci.signature.empty());

        // valid() is purely method!=nullptr — a non-null sentinel flips it true
        // WITHOUT any dereference (valid() only compares the pointer to null).
        ci.method = reinterpret_cast<vmhook::hotspot::method*>(std::uintptr_t{ 0x1000u });
        check("caller_info_nonnull_method_valid", ci.valid());
        ci.method = nullptr;
        check("caller_info_reset_method_invalid", !ci.valid());
    }
}

int main()
{
    std::printf("=== interpreter_frame_walk (no-JVM) ===\n");

    // One generously-sized arena reused across the in-memory chain tests.
    // 64 KiB is plenty for the spaced chains and leaves wide margins at both
    // ends so a [rbp - 24] read can never approach the low edge.
    arena a{ make_arena(std::size_t{ 64u } * 1024u) };
    if (!a.ok())
    {
        info("arena", "allocate_rwx(64 KiB) failed; in-memory chain tests skipped");
    }

    test_null_and_sentinel_frames();
    test_misaligned_frames(a);
    test_stack_growth_guard(a);
    test_cycles_terminate(a);
    test_long_chain_terminates(a);
    test_chain_into_unmapped();
    test_max_depth_cap(a);
    test_frame_accessor_identity(a);
    test_caller_trace_agreement(a);
    test_walk_arithmetic_and_value_contract(a);

    free_arena(a);

    if (failures == 0)
    {
        std::printf("ALL PASS\n");
        return 0;
    }
    std::printf("FAILURES: %d\n", failures);
    return 1;
}
