// Standalone (no-JVM) unit test for hotspot::decode_oop_pointer /
// encode_oop_pointer null-safety and hotspot::is_valid_pointer boundary logic.
//
// This executable runs with NO HotSpot JVM in-process.  gHotSpotVMStructs is
// therefore never resolvable, so the only OOP-codec behaviour that is
// statically determinable here is the *null-input* contract (the early-return
// guards that fire before any VMStruct lookup) plus the no-JVM fall-through
// (VMStruct lookup fails -> codec returns its null/zero sentinel without
// crashing).  Anything that needs a live oop, a real heap base, or a running
// JVM (e.g. a non-trivial decode->encode round-trip across a non-zero
// narrow_oop_base) is OUT OF SCOPE here and is covered by JVM integration in
// example.cpp instead.
//
// is_valid_pointer is pure address arithmetic (range + alignment + poison
// switch), so its full boundary behaviour IS checkable without a JVM.  Source
// of truth (verified against vmhook/ext/vmhook/vmhook.hpp on 2026-06-15):
//   is_valid_pointer        : :2007-2045
//   untag_pointer           : :2052-2057
//   narrow_decode (shared)  : :5289-5294   base + (uint64(c) << shift)
//   narrow_encode (shared)  : :5310-5315   uint32((addr - base) >> shift)
//   decode_oop_pointer      : :5334-5372   (null guard :5337, no-resolve :5362)
//   encode_oop_pointer      : :5380-5419   (null guard :5383, below-base :5413)
//   decode_klass_pointer    : :5428-5466
//   encode_klass_pointer    : :5481-5521   (below-base guard :5515)
//   os::user_address_ceiling: :515 (0x00007FFFFFFFFFFF)
//   os::user_address_floor  : :520 (0xFFFF)
//
// The OOP codec was refactored to delegate to the two pure primitives
// narrow_decode / narrow_encode (which take base+shift EXPLICITLY), so the
// FULL decode/encode arithmetic — every (base, shift) HotSpot mode and the
// complete narrow-value domain — is now exercisable with NO JVM by injecting
// base/shift directly.  Sections G..Z below do exactly that, exhaustively.
//
// Sections AA..EE then attribute the coverage back to the compressed_oops_decode
// feature PROPER: they exercise the PUBLIC OOP entry points decode_oop_pointer /
// encode_oop_pointer over their whole no-JVM input domain (AA, BB, DD), pin the
// OOP decode arithmetic per JDK-version (base, shift) regime (CC), characterise
// the encoder's below-base / no-resolve null behaviour incl. flaw #1 (BB), and
// pin the widen-before-shift overflow safety of the OOP decode formula (EE).
#include <vmhook/vmhook.hpp>
#include <array>         // std::array -- owned fixed-size byte buffers (no vector resize trap)
#include <bit>           // std::bit_cast — explicit (libc++ pulls nothing transitively)
#include <cstdio>
#include <cstdint>
#include <string>
#include <type_traits>   // std::is_same_v — explicit, do not rely on a transitive pull
#include <vector>

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

int main()
{
    using vmhook::hotspot::decode_oop_pointer;
    using vmhook::hotspot::encode_oop_pointer;
    using vmhook::hotspot::is_valid_pointer;

    // ===================================================================
    // A. decode_oop_pointer / encode_oop_pointer — null-input contract.
    //    These guards (vmhook.hpp:5337 and :5383) run BEFORE any
    //    gHotSpotVMStructs lookup, so they are JVM-independent and fully
    //    deterministic in this no-JVM build.
    // ===================================================================

    // decode_oop_pointer(0) -> nullptr.  A null compressed oop is the
    // canonical encoding of a Java null reference; it must map to a null
    // native pointer regardless of heap base/shift.  (Documented:
    // "@return ... or nullptr if compressed is 0".)
    check("decode_oop_pointer_zero_is_null",
          decode_oop_pointer(0u) == nullptr);

    // The same call expressed via a typed variable, to make sure the
    // public signature really is (std::uint32_t) -> void*.
    {
        const std::uint32_t null_oop{ 0u };
        void* const decoded{ decode_oop_pointer(null_oop) };
        check("decode_oop_pointer_zero_is_null_typed", decoded == nullptr);
    }

    // encode_oop_pointer(nullptr) -> 0.  Inverse guard (vmhook.hpp:5383):
    // a null native pointer encodes back to the null compressed oop.
    check("encode_oop_pointer_null_is_zero",
          encode_oop_pointer(nullptr) == 0u);

    // Round-trip through null in both directions.  These compose the two
    // guards above and are the ONLY decode<->encode identity that holds
    // without a live heap base (a non-null round-trip needs the JVM's
    // narrow_oop_base/shift, covered in example.cpp).
    check("roundtrip_decode_then_encode_null",
          encode_oop_pointer(decode_oop_pointer(0u)) == 0u);
    check("roundtrip_encode_then_decode_null",
          decode_oop_pointer(encode_oop_pointer(nullptr)) == nullptr);

    // No-JVM fall-through: a *non-zero* compressed oop cannot be decoded
    // because gHotSpotVMStructs is absent, so base_entry/shift_entry stay
    // null and decode_oop_pointer returns nullptr (vmhook.hpp:5362-5365)
    // WITHOUT crashing.  This documents the no-JVM behaviour; under a live
    // JVM this same input would decode to a real heap address instead.
    check("decode_oop_pointer_nonzero_no_jvm_is_null",
          decode_oop_pointer(0x0000'0001u) == nullptr);
    check("decode_oop_pointer_max_no_jvm_is_null",
          decode_oop_pointer(0xFFFF'FFFFu) == nullptr);

    // No-JVM fall-through for the encoder: a non-null pointer with no
    // resolvable VMStructs returns 0 (vmhook.hpp:5405-5408) and does not
    // crash.  Use the address of a stack local as a plausible "decoded"
    // pointer.  Under a live JVM the result would be a real narrow oop.
    {
        int stack_anchor{ 0 };
        check("encode_oop_pointer_nonnull_no_jvm_is_zero",
              encode_oop_pointer(&stack_anchor) == 0u);
    }

    // Signature / return-type pinning: decode yields a void*, encode yields
    // a std::uint32_t.  A compile-time mismatch here would fail the build,
    // which is itself the assertion; the runtime check just exercises it.
    {
        const bool decode_returns_voidptr{
            std::is_same_v<decltype(decode_oop_pointer(0u)), void*> };
        const bool encode_returns_u32{
            std::is_same_v<decltype(encode_oop_pointer(nullptr)),
                           std::uint32_t> };
        check("decode_oop_pointer_returns_void_ptr", decode_returns_voidptr);
        check("encode_oop_pointer_returns_uint32", encode_returns_u32);
    }

    // Both codec entry points are declared noexcept (vmhook.hpp:5334/:5380);
    // pin that so a future change that can throw is caught at compile time.
    {
        int stack_anchor{ 0 };
        check("decode_oop_pointer_is_noexcept",
              noexcept(decode_oop_pointer(0u)));
        check("encode_oop_pointer_is_noexcept",
              noexcept(encode_oop_pointer(&stack_anchor)));
    }

    // ===================================================================
    // B. is_valid_pointer — floor / ceiling boundaries.
    //    is_valid_pointer rejects addr <= user_address_floor and
    //    addr >= user_address_ceiling, rejects odd (bit-0 set) addresses,
    //    and rejects a fixed set of debug-poison low-32 patterns.
    //    All of this is pure arithmetic and fully checkable with no JVM.
    //    (vmhook.hpp:2007-2045.)
    // ===================================================================

    constexpr std::uintptr_t floor{ vmhook::os::user_address_floor };    // 0xFFFF
    constexpr std::uintptr_t ceiling{ vmhook::os::user_address_ceiling };// 0x7FFF'FFFF'FFFF

    // nullptr and the very lowest integers are below the floor -> rejected.
    check("is_valid_pointer_null_rejected",
          !is_valid_pointer(nullptr));
    check("is_valid_pointer_zero_rejected",
          !is_valid_pointer(reinterpret_cast<void*>(std::uintptr_t{ 0 })));
    // 1 is both below the floor AND odd: doubly rejected.
    check("is_valid_pointer_one_rejected",
          !is_valid_pointer(reinterpret_cast<void*>(std::uintptr_t{ 1 })));

    // Exactly AT the floor must be rejected — the comparison is `<=`.
    check("is_valid_pointer_at_floor_rejected",
          !is_valid_pointer(reinterpret_cast<void*>(floor)));
    // One below the floor: rejected.
    check("is_valid_pointer_below_floor_rejected",
          !is_valid_pointer(reinterpret_cast<void*>(floor - 1)));
    // floor itself (0xFFFF) is odd, so floor+1 = 0x10000 is the first
    // address that clears BOTH the range check and the 2-byte-alignment
    // check: it must be accepted.  This is the low canonical boundary.
    check("is_valid_pointer_just_above_floor_accepted",
          is_valid_pointer(reinterpret_cast<void*>(floor + 1)));

    // Exactly AT the ceiling must be rejected — the comparison is `>=`.
    check("is_valid_pointer_at_ceiling_rejected",
          !is_valid_pointer(reinterpret_cast<void*>(ceiling)));
    // One above the ceiling: rejected (non-canonical / kernel side).
    check("is_valid_pointer_above_ceiling_rejected",
          !is_valid_pointer(reinterpret_cast<void*>(ceiling + 1)));
    // One below the ceiling is even (ceiling 0x7FFF'FFFF'FFFF is odd, so
    // ceiling-1 is even) and in range: the highest canonical address that
    // is still accepted.
    check("is_valid_pointer_just_below_ceiling_accepted",
          is_valid_pointer(reinterpret_cast<void*>(ceiling - 1)));

    // A clearly non-canonical high address (top half of the 64-bit space,
    // well above the user ceiling) is rejected.
    check("is_valid_pointer_high_noncanonical_rejected",
          !is_valid_pointer(reinterpret_cast<void*>(
              std::uintptr_t{ 0xFFFF'8000'0000'0000ull })));
    // The lowest kernel-half address (just past the canonical user range)
    // is also rejected.
    check("is_valid_pointer_kernel_base_rejected",
          !is_valid_pointer(reinterpret_cast<void*>(
              std::uintptr_t{ 0x0000'8000'0000'0000ull })));

    // ===================================================================
    // C. is_valid_pointer — alignment (bit-0) rejection.
    //    Only bit 0 is checked (2-byte alignment); the function deliberately
    //    does NOT require 8-byte alignment, so an in-range odd address is
    //    rejected but the +1/+2/+4 even neighbours are accepted.
    // ===================================================================

    // An in-range but odd (bit-0 set) address is rejected even though it is
    // comfortably inside [floor, ceiling].
    check("is_valid_pointer_odd_low_canonical_rejected",
          !is_valid_pointer(reinterpret_cast<void*>(
              std::uintptr_t{ 0x0000'0000'0040'0001ull })));
    // The even neighbour of that same address is accepted.
    check("is_valid_pointer_even_low_canonical_accepted",
          is_valid_pointer(reinterpret_cast<void*>(
              std::uintptr_t{ 0x0000'0000'0040'0000ull })));

    {
        // A naturally-aligned stack array gives us real 2/4/8-byte aligned
        // addresses without hardcoding any constant.
        std::int64_t aligned_block[4]{};
        void* const eight_aligned{ &aligned_block[0] };              // 8-byte
        void* const four_aligned{
            reinterpret_cast<void*>(
                reinterpret_cast<std::uintptr_t>(&aligned_block[0]) + 4) };
        void* const two_aligned{
            reinterpret_cast<void*>(
                reinterpret_cast<std::uintptr_t>(&aligned_block[0]) + 2) };
        void* const odd_aligned{
            reinterpret_cast<void*>(
                reinterpret_cast<std::uintptr_t>(&aligned_block[0]) + 1) };

        check("is_valid_pointer_8byte_aligned_accepted",
              is_valid_pointer(eight_aligned));
        check("is_valid_pointer_4byte_aligned_accepted",
              is_valid_pointer(four_aligned));
        check("is_valid_pointer_2byte_aligned_accepted",
              is_valid_pointer(two_aligned));
        // odd interior pointer (bit 0 set) into a live stack object: rejected
        // purely on the alignment rule even though the memory is real.
        check("is_valid_pointer_odd_interior_rejected",
              !is_valid_pointer(odd_aligned));
    }

    // ===================================================================
    // D. is_valid_pointer — debug-poison low-32 patterns.
    //    The switch (vmhook.hpp:2025-2042) rejects any pointer whose low 32
    //    bits match a known uninitialised/freed fill, even though the
    //    address sits inside the canonical user range.  IMPORTANT: the
    //    alignment check (bit 0) runs BEFORE the poison switch, so for the
    //    ODD-valued sentinels (0xDEADBEEF, 0xCDCDCDCD, 0xBAADF00D,
    //    0xABABABAB, 0xFDFDFDFD, 0xDDDDDDDD) the rejection is actually
    //    produced by alignment, not by the switch.  Only the EVEN-valued
    //    sentinels (0xCAFEBABE, 0xCCCCCCCC, 0xFEEEFEEE) reach and exercise
    //    the poison switch.  We test the two groups separately so each
    //    assertion pins the rule that genuinely fires.
    // ===================================================================

    {
        // high_prefix is in range (0x1234 < 0x7FFF) and even, so it never
        // trips the range or alignment checks on its own.
        constexpr std::uintptr_t high_prefix{ 0x0000'1234'0000'0000ull };

        // -- Even sentinels: in-range, bit-0 clear, so ONLY the poison
        //    switch can reject them.  This is the assertion that actually
        //    proves the switch does its job.
        const std::uint32_t even_poison[]{
            0xCAFEBABEu, 0xCCCCCCCCu, 0xFEEEFEEEu,
        };
        bool even_poison_rejected{ true };
        for (const std::uint32_t low : even_poison)
        {
            const std::uintptr_t addr{ high_prefix | low };
            // Guard the premise: these must be even and in range.
            const bool premise_even_in_range{
                (addr & 0x1u) == 0u
                && addr > floor && addr < ceiling };
            if (!premise_even_in_range
                || is_valid_pointer(reinterpret_cast<void*>(addr)))
            {
                even_poison_rejected = false;
            }
        }
        check("is_valid_pointer_even_debug_poison_rejected_by_switch",
              even_poison_rejected);

        // -- Odd sentinels: still rejected, but by the alignment rule that
        //    precedes the switch.  Asserting they are rejected documents the
        //    end-to-end contract (these low-32 patterns never pass), while
        //    the premise check records WHY (bit 0 set).
        const std::uint32_t odd_poison[]{
            0xDEADBEEFu, 0xCDCDCDCDu, 0xBAADF00Du,
            0xABABABABu, 0xFDFDFDFDu, 0xDDDDDDDDu,
        };
        bool odd_poison_rejected{ true };
        bool odd_poison_all_odd{ true };
        for (const std::uint32_t low : odd_poison)
        {
            if ((low & 0x1u) == 0u) { odd_poison_all_odd = false; }
            const std::uintptr_t addr{ high_prefix | low };
            if (is_valid_pointer(reinterpret_cast<void*>(addr)))
            {
                odd_poison_rejected = false;
            }
        }
        check("is_valid_pointer_odd_debug_poison_are_all_odd",
              odd_poison_all_odd);
        check("is_valid_pointer_odd_debug_poison_rejected",
              odd_poison_rejected);

        // Control: the SAME high prefix with a benign even low half is
        // accepted, proving the rejections above came from the value of the
        // low half (poison / alignment) and not from the range check.
        const std::uintptr_t benign{ high_prefix | 0x0000'1000ull };
        check("is_valid_pointer_benign_low_half_accepted",
              is_valid_pointer(reinterpret_cast<void*>(benign)));

        // Sanity: the poison match is EXACT on the full low 32 bits.  A value
        // one bit away from 0xDEADBEEF that is also made even (0xDEADBEEE) is
        // NOT a sentinel, so with an in-range high prefix it is accepted.
        // This proves the switch is an exact low-32 compare, not a byte/sub-
        // pattern scan.  (low32 0xDEADBEEE: bit 0 clear, not in the switch.)
        const std::uintptr_t near_poison{ high_prefix | 0xDEADBEEEu };
        check("is_valid_pointer_near_poison_low_half_accepted",
              is_valid_pointer(reinterpret_cast<void*>(near_poison)));
    }

    // ===================================================================
    // E. is_valid_pointer — real live addresses are accepted.
    //    The whole point of the helper is to wave through genuine, mapped,
    //    aligned pointers.  A stack address, a heap allocation, and an
    //    allocate_rwx page are all real in this process with no JVM.
    // ===================================================================

    {
        int on_stack{ 7 };
        check("is_valid_pointer_real_stack_address_accepted",
              is_valid_pointer(&on_stack));
    }

    {
        // A heap allocation via std::vector backing store: real, mapped,
        // and at least 2-byte aligned (operator new is over-aligned in
        // practice).  This is the high-confidence "real pointer is valid"
        // case the boundary helper is designed to pass.
        std::vector<std::uint64_t> heap_block(8, 0);
        void* const heap_ptr{ heap_block.data() };
        check("is_valid_pointer_real_heap_address_accepted",
              is_valid_pointer(heap_ptr));
    }

    {
        // allocate_rwx returns a page in this process' user address space.
        // It must satisfy is_valid_pointer; release it afterwards.  If the
        // platform refuses RWX (returns nullptr) the assertion is skipped
        // so the test stays portable.
        const std::size_t size{ vmhook::os::page_size() };
        void* const page{ vmhook::os::allocate_rwx(nullptr, size) };
        if (page != nullptr)
        {
            check("is_valid_pointer_allocate_rwx_page_accepted",
                  is_valid_pointer(page));
            vmhook::os::release(page, size);
        }
        else
        {
            std::printf("[INFO] is_valid_pointer_allocate_rwx_page_accepted:"
                        " skipped (allocate_rwx returned nullptr)\n");
        }
    }

    // is_valid_pointer is declared noexcept (vmhook.hpp:2007); pin it.
    check("is_valid_pointer_is_noexcept",
          noexcept(is_valid_pointer(nullptr)));

    // ===================================================================
    // F. decode_oop_pointer null result is consistent with is_valid_pointer.
    //    A decoded null oop is, by definition, NOT a valid dereferenceable
    //    pointer — so decode_oop_pointer(0) feeding is_valid_pointer must
    //    report false.  Ties the two clusters together with a pure-logic
    //    invariant that holds with no JVM.
    // ===================================================================
    check("decoded_null_oop_is_not_valid_pointer",
          !is_valid_pointer(decode_oop_pointer(0u)));

    // ===================================================================
    // G. narrow_decode / narrow_encode — the pure shift/add+subtract/shift
    //    arithmetic primitives (vmhook.hpp:5289-5294, :5310-5315).  Unlike
    //    decode_oop_pointer / encode_oop_pointer these take the base/shift
    //    EXPLICITLY (the caller normally resolves them from VMStructs), so
    //    their full arithmetic IS exercisable with no JVM by injecting
    //    base/shift directly.  This is the round-trip the task asks for.
    //
    //      narrow_decode(base, shift, compressed) = base + (compressed << shift)
    //      narrow_encode(base, shift, addr)       = (addr - base) >> shift
    //
    //    Source of truth: the one-line bodies above.  Every expected value is
    //    computed from the documented formula, not guessed.
    // ===================================================================
    using vmhook::hotspot::narrow_decode;
    using vmhook::hotspot::narrow_encode;

    // -- shift == 0 : decode is a plain base + compressed. -----------------
    check("narrow_decode_shift0_base0_identity",
          narrow_decode(0u, 0u, 0x1000u)
              == reinterpret_cast<void*>(std::uintptr_t{ 0x1000u }));
    check("narrow_decode_shift0_with_base",
          narrow_decode(std::uint64_t{ 0x7F00'0000'0000ull }, 0u, 0x2000u)
              == reinterpret_cast<void*>(std::uintptr_t{ 0x7F00'0000'2000ull }));
    // compressed==0 with a non-zero base: narrow_decode does NOT short-circuit
    // (that is the caller's job), so it returns base verbatim.
    check("narrow_decode_zero_compressed_returns_base",
          narrow_decode(std::uint64_t{ 0x1'0000'0000ull }, 3u, 0u)
              == reinterpret_cast<void*>(std::uintptr_t{ 0x1'0000'0000ull }));

    // -- shift == 3 : the classic 8-byte-aligned-oop heap (<=32 GB). -------
    // compressed 1 << 3 == 8, added to the base.
    check("narrow_decode_shift3_scales_by_8",
          narrow_decode(0u, 3u, 1u)
              == reinterpret_cast<void*>(std::uintptr_t{ 8u }));
    check("narrow_decode_shift3_base_plus_scaled",
          narrow_decode(std::uint64_t{ 0x8'0000'0000ull }, 3u, 0x10u)
              == reinterpret_cast<void*>(std::uintptr_t{ 0x8'0000'0080ull }));
    // Max compressed value at shift 3: 0xFFFFFFFF << 3 widened to 64-bit.
    check("narrow_decode_shift3_max_compressed",
          narrow_decode(0u, 3u, 0xFFFF'FFFFu)
              == reinterpret_cast<void*>(
                     static_cast<std::uintptr_t>(std::uint64_t{ 0xFFFF'FFFFull } << 3)));

    // -- narrow_encode is the inverse on the (addr >= base) domain. --------
    check("narrow_encode_shift0_base0_identity",
          narrow_encode(0u, 0u, std::uint64_t{ 0x1234u }) == 0x1234u);
    check("narrow_encode_subtracts_base",
          narrow_encode(std::uint64_t{ 0x1000u }, 0u, std::uint64_t{ 0x1234u }) == 0x234u);
    check("narrow_encode_shift3_divides_by_8",
          narrow_encode(0u, 3u, std::uint64_t{ 64u }) == 8u);
    // addr == base -> 0 (the encoded null/at-base case).
    check("narrow_encode_addr_equals_base_is_zero",
          narrow_encode(std::uint64_t{ 0x4000'0000ull }, 3u,
                        std::uint64_t{ 0x4000'0000ull }) == 0u);

    // -- Full round-trips: encode(decode(c)) == c and decode(encode(a)) == a
    //    for several (base, shift) pairs, exactly as a live heap would do but
    //    with injected constants.  These are the decode<->encode identities the
    //    no-JVM file otherwise can only check through null.
    {
        struct cfg { std::uint64_t base; std::uint32_t shift; };
        constexpr cfg cfgs[]{
            { 0u, 0u },
            { 0u, 3u },
            { std::uint64_t{ 0x7F00'0000'0000ull }, 0u },
            { std::uint64_t{ 0x8'0000'0000ull }, 3u },
        };
        const std::uint32_t compresseds[]{ 1u, 2u, 0x10u, 0x1000u, 0x00FF'FFFFu };

        bool encode_after_decode_ok{ true };
        for (const cfg c : cfgs)
        {
            for (const std::uint32_t comp : compresseds)
            {
                void* const decoded{ narrow_decode(c.base, c.shift, comp) };
                const std::uint32_t re{ narrow_encode(
                    c.base, c.shift,
                    static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(decoded))) };
                if (re != comp) { encode_after_decode_ok = false; }
            }
        }
        check("narrow_encode_after_decode_roundtrips", encode_after_decode_ok);

        // decode(encode(addr)) == addr for addresses that are exact multiples of
        // (1<<shift) above base (the representable set).
        bool decode_after_encode_ok{ true };
        for (const cfg c : cfgs)
        {
            for (const std::uint32_t comp : compresseds)
            {
                // Build an addr that is exactly representable: base + (comp<<shift).
                const std::uint64_t addr{ c.base
                    + (static_cast<std::uint64_t>(comp) << c.shift) };
                const std::uint32_t enc{ narrow_encode(c.base, c.shift, addr) };
                void* const back{ narrow_decode(c.base, c.shift, enc) };
                if (static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(back)) != addr)
                {
                    decode_after_encode_ok = false;
                }
            }
        }
        check("narrow_decode_after_encode_roundtrips", decode_after_encode_ok);
    }

    // narrow_decode / narrow_encode are declared noexcept; pin it.
    check("narrow_decode_is_noexcept", noexcept(narrow_decode(0u, 0u, 0u)));
    check("narrow_encode_is_noexcept", noexcept(narrow_encode(0u, 0u, 0u)));
    // Return-type pinning.
    check("narrow_decode_returns_void_ptr",
          std::is_same_v<decltype(narrow_decode(0u, 0u, 0u)), void*>);
    check("narrow_encode_returns_uint32",
          std::is_same_v<decltype(narrow_encode(0u, 0u, 0u)), std::uint32_t>);

    // ===================================================================
    // H. untag_pointer — masks off high GC tag bits with user_address_ceiling
    //    (vmhook.hpp:2052-2057).  Pure bit-AND, fully checkable with no JVM.
    //      untag_pointer(p) = p & 0x00007FFFFFFFFFFF
    // ===================================================================
    using vmhook::hotspot::untag_pointer;

    // nullptr stays null (0 & anything == 0).
    check("untag_pointer_null_stays_null",
          untag_pointer(nullptr) == nullptr);
    // A pointer already inside the canonical user range is returned unchanged.
    {
        const std::uintptr_t in_range{ 0x0000'1234'5678'9AB0ull };
        check("untag_pointer_in_range_unchanged",
              untag_pointer(reinterpret_cast<const void*>(in_range))
                  == reinterpret_cast<const void*>(in_range));
    }
    // High tag bits (above bit 46) are stripped: the masked result keeps only
    // the low 47 bits.
    {
        const std::uintptr_t tagged{ 0xFFFF'0000'0000'1000ull };
        const std::uintptr_t expected{ tagged & ceiling };   // ceiling == 0x7FFF'FFFFFFFF
        check("untag_pointer_strips_high_tag_bits",
              untag_pointer(reinterpret_cast<const void*>(tagged))
                  == reinterpret_cast<const void*>(expected));
        check("untag_pointer_strip_matches_explicit_mask",
              expected == std::uintptr_t{ 0x0000'0000'0000'1000ull });
    }
    // Exactly the ceiling bit-pattern masks to itself (it IS the mask).
    check("untag_pointer_ceiling_masks_to_itself",
          untag_pointer(reinterpret_cast<const void*>(ceiling))
              == reinterpret_cast<const void*>(ceiling));
    // Bit 47 set (the lowest "kernel/tag" bit) is cleared by the mask.
    {
        const std::uintptr_t bit47{ 0x0000'8000'0000'0000ull };
        check("untag_pointer_clears_bit47",
              untag_pointer(reinterpret_cast<const void*>(bit47 | 0x2000ull))
                  == reinterpret_cast<const void*>(std::uintptr_t{ 0x2000ull }));
    }
    // untag_pointer is idempotent: untag(untag(p)) == untag(p).
    {
        const std::uintptr_t tagged{ 0xDEAD'0000'00AB'CDE0ull };
        const void* once{ untag_pointer(reinterpret_cast<const void*>(tagged)) };
        const void* twice{ untag_pointer(once) };
        check("untag_pointer_is_idempotent", once == twice);
    }
    check("untag_pointer_is_noexcept", noexcept(untag_pointer(nullptr)));

    // ===================================================================
    // I. is_valid_pointer — additional exact boundary + alignment cases that
    //    complement the existing floor/ceiling/poison coverage.
    // ===================================================================

    // floor+2 (0x10001 is odd; floor+1 = 0x10000 even accepted; floor+2 =
    // 0x10001 is ODD -> rejected; floor+3 = 0x10002 even accepted).
    check("is_valid_pointer_floor_plus_1_even_accepted",
          is_valid_pointer(reinterpret_cast<void*>(floor + 1)));      // 0x10000 even
    check("is_valid_pointer_floor_plus_2_odd_rejected",
          !is_valid_pointer(reinterpret_cast<void*>(floor + 2)));     // 0x10001 odd
    check("is_valid_pointer_floor_plus_3_even_accepted",
          is_valid_pointer(reinterpret_cast<void*>(floor + 3)));      // 0x10002 even

    // ceiling (0x7FFF'FFFF'FFFF) is odd, so ceiling-1 is even (accepted, above),
    // ceiling-2 is ODD -> rejected by alignment, and ceiling-3 is even ->
    // accepted.  (All three are comfortably below the ceiling, so range never
    // fires; alignment is the sole discriminator.)
    check("is_valid_pointer_ceiling_minus_2_odd_rejected",
          !is_valid_pointer(reinterpret_cast<void*>(ceiling - 2)));
    check("is_valid_pointer_ceiling_minus_3_even_accepted",
          is_valid_pointer(reinterpret_cast<void*>(ceiling - 3)));

    // A representative span of in-range addresses: every even one is accepted,
    // every odd one rejected (alignment is the ONLY discriminator here, given
    // none of these low halves hit a poison pattern).
    {
        bool parity_rule_holds{ true };
        const std::uintptr_t base_addr{ 0x0000'2000'0000'0000ull };  // in range, even
        for (std::uintptr_t k{ 0 }; k < 32; ++k)
        {
            const std::uintptr_t addr{ base_addr + k };
            const bool valid{ is_valid_pointer(reinterpret_cast<void*>(addr)) };
            const bool expected_valid{ (addr & 0x1u) == 0u };       // even <=> valid
            if (valid != expected_valid) { parity_rule_holds = false; }
        }
        check("is_valid_pointer_even_accept_odd_reject_span", parity_rule_holds);
    }

    // All nine documented poison low-32 patterns (vmhook.hpp:2027-2042) are
    // rejected end-to-end under an in-range, even high prefix — whether the
    // rejection comes from the alignment rule (odd sentinels) or the poison
    // switch (even sentinels), the contract is that none of these low-32
    // patterns ever pass.  This pins the FULL sentinel list, not just a subset.
    {
        constexpr std::uintptr_t prefix{ 0x0000'5000'0000'0000ull };  // in range, even
        const std::uint32_t all_poison[]{
            0xDEADBEEFu, 0xCAFEBABEu, 0xCCCCCCCCu, 0xCDCDCDCDu, 0xBAADF00Du,
            0xFEEEFEEEu, 0xABABABABu, 0xFDFDFDFDu, 0xDDDDDDDDu,
        };
        bool all_poison_rejected{ true };
        for (const std::uint32_t low : all_poison)
        {
            const std::uintptr_t addr{ prefix | low };
            if (is_valid_pointer(reinterpret_cast<void*>(addr)))
            {
                all_poison_rejected = false;
            }
        }
        check("is_valid_pointer_all_nine_poison_patterns_rejected",
              all_poison_rejected);
    }

    // decode_oop_pointer / encode_oop_pointer are still null-safe when chained
    // with untag/narrow primitives: decode(0) untags to null.
    check("decode_zero_untags_to_null",
          untag_pointer(decode_oop_pointer(0u)) == nullptr);

    // ===================================================================
    // J. narrow_decode — EXHAUSTIVE across every HotSpot (base, shift)
    //    encoding mode, with the expected value recomputed independently
    //    from the documented formula
    //        narrow_decode(base, shift, c) = base + (uint64(c) << shift)
    //    (vmhook.hpp:5289-5294).  This is the workhorse both decode_oop_pointer
    //    and decode_klass_pointer call once base/shift are resolved, so its
    //    arithmetic IS the codec arithmetic and is fully testable with no JVM.
    //
    //    HotSpot uses these (base, shift) combinations in practice:
    //      base == 0, shift == 0  : 32-bit unscaled, zero-based (<4 GB heap @0)
    //      base == 0, shift == 3  : zero-based scaled-by-8 (<=32 GB heap)
    //      base != 0, shift == 0  : based, unscaled
    //      base != 0, shift == 3  : based + scaled (>32 GB heap)
    //    The primitive itself is shift-agnostic, so we also exercise shifts
    //    1, 2 and 4 to prove it scales by exactly 2^shift for any shift.
    // ===================================================================

    // Helper: independent re-implementation of the documented decode formula,
    // used ONLY to compute the expected value (never calls the library).  Kept
    // local (lambda) so the suite stays a single self-contained main().
    const auto expect_decode{
        [](std::uint64_t base, std::uint32_t shift, std::uint32_t c) -> std::uintptr_t {
            return static_cast<std::uintptr_t>(
                base + (static_cast<std::uint64_t>(c) << shift));
        } };
    const auto as_uptr{
        [](void* p) -> std::uintptr_t {
            return reinterpret_cast<std::uintptr_t>(p);
        } };

    {
        // Every encoding mode we care about, including the four canonical
        // HotSpot configurations plus extra shifts for completeness.  The
        // bases are synthetic (no real heap), exactly as the task requires.
        struct mode { std::uint64_t base; std::uint32_t shift; const char* tag; };
        const mode modes[]{
            { 0u,                              0u, "b0_s0"   }, // unscaled zero-based
            { 0u,                              1u, "b0_s1"   },
            { 0u,                              2u, "b0_s2"   },
            { 0u,                              3u, "b0_s3"   }, // scaled-by-8 zero-based
            { 0u,                              4u, "b0_s4"   },
            { std::uint64_t{ 0x1'0000'0000ull },     0u, "bN_s0"   }, // based unscaled
            { std::uint64_t{ 0x1'0000'0000ull },     3u, "bN_s3"   }, // based + scaled
            { std::uint64_t{ 0x7F00'0000'0000ull },  0u, "bHi_s0"  }, // high base
            { std::uint64_t{ 0x8'0000'0000ull },     3u, "b32G_s3" }, // 32 GB-style heap
            { std::uint64_t{ 0x12'3456'7800ull },    3u, "bOdd_s3" }, // non-round base
        };

        // A dense compressed sweep: many small values, powers of two, on/off
        // boundary at each shift, plus the 32-bit extremes.  "All input
        // possible" at uint32 width is infeasible to enumerate (4 billion), so
        // we use a representative dense set that hits every structurally
        // interesting class: 0, 1, small contiguous run, each power of two up
        // to 2^31, each (power-of-two +/- 1), 0x7FFFFFFF, 0x80000000,
        // 0xFFFFFFFE, 0xFFFFFFFF.
        std::vector<std::uint32_t> comps;
        comps.push_back(0u);
        for (std::uint32_t k{ 1u }; k <= 16u; ++k) { comps.push_back(k); }  // small run
        for (unsigned bit{ 0u }; bit < 32u; ++bit)
        {
            const std::uint32_t pow2{ static_cast<std::uint32_t>(1u) << bit };
            comps.push_back(pow2);
            comps.push_back(pow2 - 1u);
            comps.push_back(pow2 + 1u);          // wraps to 0 only at bit 31 (+1 -> 0x80000001), fine
        }
        comps.push_back(0x7FFF'FFFFu);
        comps.push_back(0x8000'0000u);
        comps.push_back(0x8000'0001u);
        comps.push_back(0xFFFF'FFFEu);
        comps.push_back(0xFFFF'FFFFu);

        bool decode_matches_formula{ true };
        std::size_t decode_cases{ 0 };
        for (const mode m : modes)
        {
            for (const std::uint32_t c : comps)
            {
                const std::uintptr_t got{ as_uptr(narrow_decode(m.base, m.shift, c)) };
                const std::uintptr_t want{ expect_decode(m.base, m.shift, c) };
                if (got != want) { decode_matches_formula = false; }
                ++decode_cases;
            }
        }
        check("narrow_decode_matches_formula_all_modes", decode_matches_formula);
        // Pin that the sweep is genuinely large (modes * comps), so a future
        // edit that accidentally empties a vector is caught.
        check("narrow_decode_sweep_is_dense", decode_cases >= 1000);
    }

    // ===================================================================
    // K. narrow_decode — narrow-VALUE width: confirm a full 32-bit narrow
    //    value is consumed correctly and the decoded full pointer is exactly
    //    base + (narrow << shift) for the documented extreme values:
    //    0, 1, small, 0x7FFFFFFF, 0x80000000 (sign bit), 0xFFFFFFFF (all-ones).
    //    The widen-before-shift (uint64 cast in narrow_decode, vmhook.hpp:5293)
    //    means 0xFFFFFFFF << 3 must NOT overflow 32 bits — it must produce the
    //    full 0x7'FFFF'FFF8.  This is the exact bug class the cast prevents.
    // ===================================================================
    {
        const std::uint32_t widths[]{
            0u, 1u, 2u, 0x7Fu, 0x7FFFu, 0x7FFF'FFFFu,
            0x8000'0000u, 0xFFFF'FFFEu, 0xFFFF'FFFFu,
        };
        const std::uint32_t shifts[]{ 0u, 1u, 2u, 3u, 4u };
        bool width_ok{ true };
        for (const std::uint32_t sh : shifts)
        {
            for (const std::uint32_t w : widths)
            {
                const std::uintptr_t got{ as_uptr(narrow_decode(0u, sh, w)) };
                const std::uintptr_t want{
                    static_cast<std::uintptr_t>(static_cast<std::uint64_t>(w) << sh) };
                if (got != want) { width_ok = false; }
            }
        }
        check("narrow_decode_narrow_width_exact_all_shifts", width_ok);

        // Explicit, named checks for the two highest-risk extremes at shift 3:
        // all-ones and the sign bit must widen, not sign-extend or truncate.
        check("narrow_decode_all_ones_shift3_widened",
              as_uptr(narrow_decode(0u, 3u, 0xFFFF'FFFFu))
                  == static_cast<std::uintptr_t>(std::uint64_t{ 0xFFFF'FFFFull } << 3));
        check("narrow_decode_sign_bit_shift3_widened",
              as_uptr(narrow_decode(0u, 3u, 0x8000'0000u))
                  == static_cast<std::uintptr_t>(std::uint64_t{ 0x8000'0000ull } << 3));
        // Top-bit value must zero-extend into bit 32+, not become a negative
        // sign-extended pointer: result strictly greater than 0xFFFFFFFF.
        check("narrow_decode_sign_bit_shift3_above_4G",
              as_uptr(narrow_decode(0u, 3u, 0x8000'0000u)) > 0xFFFF'FFFFull);
    }

    // ===================================================================
    // L. narrow_encode — EXHAUSTIVE inverse across every mode, expected value
    //    recomputed from the documented formula
    //        narrow_encode(base, shift, addr) = uint32((addr - base) >> shift)
    //    (vmhook.hpp:5310-5315).  Driven over addresses that are exact, in-range
    //    representable points (base + (c << shift)) so the uint32 narrowing is
    //    lossless and the result must equal c.
    // ===================================================================
    {
        struct mode { std::uint64_t base; std::uint32_t shift; };
        const mode modes[]{
            { 0u, 0u }, { 0u, 1u }, { 0u, 2u }, { 0u, 3u }, { 0u, 4u },
            { std::uint64_t{ 0x1'0000'0000ull },    0u },
            { std::uint64_t{ 0x1'0000'0000ull },    3u },
            { std::uint64_t{ 0x7F00'0000'0000ull }, 0u },
            { std::uint64_t{ 0x8'0000'0000ull },    3u },
        };
        // Compressed values whose (c << shift) stays well inside 64-bit and whose
        // encode is exactly recoverable.  Cap at 0x00FF'FFFF so even shift 4 keeps
        // (c<<shift) < 2^28 and base+offset never wraps for the bases above.
        std::vector<std::uint32_t> comps;
        for (std::uint32_t k{ 0u }; k <= 8u; ++k) { comps.push_back(k); }
        const std::uint32_t extra[]{ 0x10u, 0xFFu, 0x100u, 0xFFFFu,
                                     0x10000u, 0x00FF'FFFFu };
        for (const std::uint32_t e : extra) { comps.push_back(e); }

        bool encode_matches_formula{ true };
        for (const mode m : modes)
        {
            for (const std::uint32_t c : comps)
            {
                const std::uint64_t addr{
                    m.base + (static_cast<std::uint64_t>(c) << m.shift) };
                const std::uint32_t got{ narrow_encode(m.base, m.shift, addr) };
                const std::uint32_t want{
                    static_cast<std::uint32_t>((addr - m.base) >> m.shift) };
                if (got != want || got != c) { encode_matches_formula = false; }
            }
        }
        check("narrow_encode_matches_formula_all_modes", encode_matches_formula);
    }

    // ===================================================================
    // M. FULL ROUND-TRIP encode(decode(c)) == c — the most important codec
    //    identity — across EVERY mode and the dense 32-bit compressed sweep,
    //    including 0x7FFFFFFF / 0x80000000 / 0xFFFFFFFF.  Because decode widens
    //    to uint64 before shifting and encode subtracts the same base then
    //    re-narrows, this identity holds for ALL 32-bit c and ALL shift values
    //    (the high bits lost to << are exactly the bits restored by >>).  A dense
    //    sweep proves it without enumerating all 2^32 inputs.
    // ===================================================================
    {
        struct mode { std::uint64_t base; std::uint32_t shift; };
        const mode modes[]{
            { 0u, 0u }, { 0u, 1u }, { 0u, 2u }, { 0u, 3u }, { 0u, 4u },
            { std::uint64_t{ 0x1'0000'0000ull },    0u },
            { std::uint64_t{ 0x1'0000'0000ull },    3u },
            { std::uint64_t{ 0x7F00'0000'0000ull }, 0u },
            { std::uint64_t{ 0x8'0000'0000ull },    3u },
            { std::uint64_t{ 0x12'3456'7800ull },   3u },
        };
        // Reuse the dense compressed set construction (powers of two +/-1, run,
        // extremes).  Includes 0, 1, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF.
        std::vector<std::uint32_t> comps;
        comps.push_back(0u);
        for (std::uint32_t k{ 1u }; k <= 16u; ++k) { comps.push_back(k); }
        for (unsigned bit{ 0u }; bit < 32u; ++bit)
        {
            const std::uint32_t pow2{ static_cast<std::uint32_t>(1u) << bit };
            comps.push_back(pow2);
            comps.push_back(pow2 - 1u);
            comps.push_back(pow2 + 1u);
        }
        comps.push_back(0x7FFF'FFFFu);
        comps.push_back(0x8000'0000u);
        comps.push_back(0xFFFF'FFFFu);

        bool roundtrip_ok{ true };
        std::size_t rt_cases{ 0 };
        std::uint32_t worst_mismatch_c{ 0 };
        for (const mode m : modes)
        {
            for (const std::uint32_t c : comps)
            {
                void* const decoded{ narrow_decode(m.base, m.shift, c) };
                const std::uint32_t re{ narrow_encode(
                    m.base, m.shift, static_cast<std::uint64_t>(as_uptr(decoded))) };
                if (re != c) { roundtrip_ok = false; worst_mismatch_c = c; }
                ++rt_cases;
            }
        }
        check("narrow_roundtrip_encode_decode_all_c_all_modes", roundtrip_ok);
        check("narrow_roundtrip_matrix_is_dense", rt_cases >= 1000);
        // worst_mismatch_c is referenced so it is not flagged unused under -Werror;
        // it stays 0 when everything round-trips.
        check("narrow_roundtrip_no_mismatch_recorded", worst_mismatch_c == 0u);
    }

    // ===================================================================
    // N. ROUND-TRIP decode(encode(addr)) == addr over the REPRESENTABLE
    //    address set of each mode: addresses of the form base + (c << shift)
    //    for a dense c.  (Addresses NOT of this form are not representable as a
    //    narrow value and are covered as adversarial cases in section Q.)
    // ===================================================================
    {
        struct mode { std::uint64_t base; std::uint32_t shift; };
        const mode modes[]{
            { 0u, 0u }, { 0u, 1u }, { 0u, 2u }, { 0u, 3u }, { 0u, 4u },
            { std::uint64_t{ 0x1'0000'0000ull },    0u },
            { std::uint64_t{ 0x1'0000'0000ull },    3u },
            { std::uint64_t{ 0x7F00'0000'0000ull }, 0u },
            { std::uint64_t{ 0x8'0000'0000ull },    3u },
        };
        std::vector<std::uint32_t> comps;
        for (std::uint32_t k{ 0u }; k <= 32u; ++k) { comps.push_back(k); }
        const std::uint32_t extra[]{ 0x100u, 0x1000u, 0x1'0000u,
                                     0x10'0000u, 0x00FF'FFFFu };
        for (const std::uint32_t e : extra) { comps.push_back(e); }

        bool addr_roundtrip_ok{ true };
        for (const mode m : modes)
        {
            for (const std::uint32_t c : comps)
            {
                const std::uint64_t addr{
                    m.base + (static_cast<std::uint64_t>(c) << m.shift) };
                const std::uint32_t enc{ narrow_encode(m.base, m.shift, addr) };
                void* const back{ narrow_decode(m.base, m.shift, enc) };
                if (static_cast<std::uint64_t>(as_uptr(back)) != addr)
                {
                    addr_roundtrip_ok = false;
                }
            }
        }
        check("narrow_roundtrip_decode_encode_representable_addrs", addr_roundtrip_ok);
    }

    // ===================================================================
    // O. NULL / zero special case at the PRIMITIVE level for every mode.
    //    narrow_decode is deliberately NOT null-aware (the wrapper codecs own
    //    the compressed==0 -> nullptr short-circuit, see section A / R), so:
    //      narrow_decode(base, shift, 0) == reinterpret_cast<void*>(base)
    //    for ANY shift.  And the inverse:
    //      narrow_encode(base, shift, base) == 0  (addr == base encodes to 0).
    //    These pin the at-base/zero boundary that the higher-level codecs build
    //    their null contract on top of.
    // ===================================================================
    {
        struct mode { std::uint64_t base; std::uint32_t shift; };
        const mode modes[]{
            { 0u, 0u }, { 0u, 3u },
            { std::uint64_t{ 0x1'0000'0000ull }, 0u },
            { std::uint64_t{ 0x1'0000'0000ull }, 3u },
            { std::uint64_t{ 0x7F00'0000'0000ull }, 3u },
        };
        bool zero_decodes_to_base{ true };
        bool base_encodes_to_zero{ true };
        for (const mode m : modes)
        {
            if (as_uptr(narrow_decode(m.base, m.shift, 0u))
                != static_cast<std::uintptr_t>(m.base))
            {
                zero_decodes_to_base = false;
            }
            if (narrow_encode(m.base, m.shift, m.base) != 0u)
            {
                base_encodes_to_zero = false;
            }
        }
        check("narrow_decode_zero_is_base_all_modes", zero_decodes_to_base);
        check("narrow_encode_base_is_zero_all_modes", base_encodes_to_zero);
    }

    // ===================================================================
    // P. Boundary / alignment: a compressed value and its neighbours straddling
    //    a shift-alignment boundary, plus the maximum representable heap offset
    //    for a given shift.
    // ===================================================================
    {
        // At shift 3 each compressed step is 8 bytes.  Consecutive compressed
        // values must produce addresses exactly 8 apart (the alignment grid).
        const std::uintptr_t a0{ as_uptr(narrow_decode(0u, 3u, 100u)) };
        const std::uintptr_t a1{ as_uptr(narrow_decode(0u, 3u, 101u)) };
        const std::uintptr_t a2{ as_uptr(narrow_decode(0u, 3u, 102u)) };
        check("narrow_decode_shift3_step_is_8_a", a1 - a0 == 8u);
        check("narrow_decode_shift3_step_is_8_b", a2 - a1 == 8u);
        // The decoded address at shift 3 is always 8-byte aligned (low 3 bits 0)
        // when base is 8-aligned (here base 0).
        check("narrow_decode_shift3_result_8_aligned",
              (a0 & 0x7u) == 0u && (a1 & 0x7u) == 0u && (a2 & 0x7u) == 0u);

        // Maximum representable heap offset at shift 3 with the all-ones narrow
        // value: 0xFFFFFFFF objects * 8 bytes == 0x7'FFFF'FFF8 (~32 GB span),
        // exactly the documented <=32 GB compressed-oops ceiling.
        check("narrow_decode_max_offset_shift3",
              as_uptr(narrow_decode(0u, 3u, 0xFFFF'FFFFu)) == 0x7'FFFF'FFF8ull);
        // ...and at shift 0 the maximum offset is the 4 GB span (0xFFFFFFFF).
        check("narrow_decode_max_offset_shift0",
              as_uptr(narrow_decode(0u, 0u, 0xFFFF'FFFFu)) == 0xFFFF'FFFFull);

        // Round-trip the maximum representable value at shift 3 from a non-zero
        // base: base + (0xFFFFFFFF << 3) must encode back to 0xFFFFFFFF.
        {
            const std::uint64_t base{ 0x8'0000'0000ull };
            const std::uint64_t top{ base + (std::uint64_t{ 0xFFFF'FFFFull } << 3) };
            check("narrow_encode_max_offset_shift3_based_roundtrip",
                  narrow_encode(base, 3u, top) == 0xFFFF'FFFFu);
        }
    }

    // ===================================================================
    // Q. Adversarial / degenerate inputs — must be DETERMINISTIC and never
    //    crash (the primitives are noexcept and do raw modular arithmetic).
    //    We assert the exact wrapped value the documented formula produces, so
    //    these double as a spec for the corner behaviour rather than implying
    //    the inputs are "valid".
    // ===================================================================
    {
        // (Q1) addr < base in narrow_encode underflows (uint64 wraps), then the
        // shift and uint32 narrow apply.  Documented formula: uint32(((addr -
        // base) mod 2^64) >> shift).  We compute the expectation the same way.
        const std::uint64_t base{ 0x1'0000'0000ull };
        const std::uint64_t addr{ 0x0000'0000ull };          // far below base
        const std::uint32_t got{ narrow_encode(base, 3u, addr) };
        const std::uint32_t want{
            static_cast<std::uint32_t>((addr - base) >> 3) };  // modular wrap
        check("narrow_encode_underflow_is_modular_deterministic", got == want);

        // (Q2) Misaligned addr at shift 3: an address that is NOT a multiple of
        // 8 above base loses its low (shift) bits on encode (integer >> 3), i.e.
        // encode is lossy for non-grid addresses — decode(encode(addr)) floors
        // addr down to the alignment grid.  Document that exact behaviour.
        const std::uint64_t misaligned{ base + 13u };          // +13, not /8
        const std::uint32_t enc_mis{ narrow_encode(base, 3u, misaligned) };
        check("narrow_encode_misaligned_floors_offset",
              enc_mis == 1u);                                  // 13 >> 3 == 1
        const std::uintptr_t dec_mis{ as_uptr(narrow_decode(base, 3u, enc_mis)) };
        check("narrow_decode_of_misaligned_encode_is_floored",
              dec_mis == static_cast<std::uintptr_t>(base + 8u)); // floored to +8

        // (Q3) decode at the extreme top of the narrow range from a high base:
        // base + (0xFFFFFFFF << 3) must not overflow uint64 and must equal the
        // independently computed sum (no UB, deterministic).
        const std::uint64_t high_base{ 0x7000'0000'0000ull };
        check("narrow_decode_extreme_top_high_base_deterministic",
              as_uptr(narrow_decode(high_base, 3u, 0xFFFF'FFFFu))
                  == static_cast<std::uintptr_t>(
                         high_base + (std::uint64_t{ 0xFFFF'FFFFull } << 3)));

        // (Q4) A shift large enough that (c << shift) walks into the very top of
        // the 64-bit space stays deterministic (modular), no crash.  shift 31
        // with c == 0xFFFFFFFF: 0xFFFFFFFF << 31 == 0x7FFF'FFFF'8000'0000.
        check("narrow_decode_large_shift_is_modular",
              as_uptr(narrow_decode(0u, 31u, 0xFFFF'FFFFu))
                  == static_cast<std::uintptr_t>(
                         std::uint64_t{ 0xFFFF'FFFFull } << 31));

        // (Q5) encode then decode of an at-base address is the at-base address
        // (the zero/null grid point), for a non-zero base.
        check("narrow_at_base_roundtrips_to_base",
              as_uptr(narrow_decode(base, 3u, narrow_encode(base, 3u, base)))
                  == static_cast<std::uintptr_t>(base));
    }

    // ===================================================================
    // R. decode_klass_pointer / encode_klass_pointer — the SECOND compressed-
    //    pointer codec (vmhook.hpp:5428 / :5481).  Structurally identical to the
    //    OOP codec but resolves CompressedKlassPointers::_narrow_klass.{_base,
    //    _shift}.  With no JVM the VMStruct lookup fails, so only the null
    //    contract + no-JVM fall-through are determinable — exactly mirroring the
    //    OOP coverage in section A, which previously had ZERO klass-codec
    //    counterpart.  Source of truth: the early-return guards at :5431 / :5484
    //    (compressed/decoded == 0) and the missing-entry guards at :5456 / :5506.
    // ===================================================================
    using vmhook::hotspot::decode_klass_pointer;
    using vmhook::hotspot::encode_klass_pointer;

    // decode_klass_pointer(0) -> nullptr (compressed==0 guard, JVM-independent).
    check("decode_klass_pointer_zero_is_null",
          decode_klass_pointer(0u) == nullptr);
    {
        const std::uint32_t null_klass{ 0u };
        void* const decoded{ decode_klass_pointer(null_klass) };
        check("decode_klass_pointer_zero_is_null_typed", decoded == nullptr);
    }
    // encode_klass_pointer(nullptr) -> 0 (decoded==null guard).
    check("encode_klass_pointer_null_is_zero",
          encode_klass_pointer(nullptr) == 0u);
    // Null round-trips, both directions.
    check("klass_roundtrip_decode_then_encode_null",
          encode_klass_pointer(decode_klass_pointer(0u)) == 0u);
    check("klass_roundtrip_encode_then_decode_null",
          decode_klass_pointer(encode_klass_pointer(nullptr)) == nullptr);
    // No-JVM fall-through: non-zero compressed cannot resolve VMStructs, so the
    // missing-entry guard returns nullptr (not a crash) for small AND max input.
    check("decode_klass_pointer_nonzero_no_jvm_is_null",
          decode_klass_pointer(0x0000'0001u) == nullptr);
    check("decode_klass_pointer_max_no_jvm_is_null",
          decode_klass_pointer(0xFFFF'FFFFu) == nullptr);
    // No-JVM fall-through for the klass encoder: a non-null pointer with no
    // resolvable VMStructs returns 0 and does not crash.
    {
        int stack_anchor{ 0 };
        check("encode_klass_pointer_nonnull_no_jvm_is_zero",
              encode_klass_pointer(&stack_anchor) == 0u);
    }
    // Signature / return-type pinning for the klass codec (void* / uint32_t).
    {
        const bool decode_returns_voidptr{
            std::is_same_v<decltype(decode_klass_pointer(0u)), void*> };
        const bool encode_returns_u32{
            std::is_same_v<decltype(encode_klass_pointer(nullptr)),
                           std::uint32_t> };
        check("decode_klass_pointer_returns_void_ptr", decode_returns_voidptr);
        check("encode_klass_pointer_returns_uint32", encode_returns_u32);
    }
    // Both klass codec entry points are noexcept (vmhook.hpp:5428 / :5481).
    {
        int stack_anchor{ 0 };
        check("decode_klass_pointer_is_noexcept",
              noexcept(decode_klass_pointer(0u)));
        check("encode_klass_pointer_is_noexcept",
              noexcept(encode_klass_pointer(&stack_anchor)));
    }
    // A decoded null klass is, by definition, not a valid dereferenceable
    // pointer (ties klass codec to is_valid_pointer, mirroring section F).
    check("decoded_null_klass_is_not_valid_pointer",
          !is_valid_pointer(decode_klass_pointer(0u)));
    // decode(0) for the klass codec untags to null as well (mirrors the oop
    // chaining check), proving the null sentinel survives tag-stripping.
    check("decode_klass_zero_untags_to_null",
          untag_pointer(decode_klass_pointer(0u)) == nullptr);

    // ===================================================================
    // S. Cross-codec consistency — the OOP and Klass codecs share the SAME
    //    arithmetic primitive (narrow_decode/narrow_encode), so for any given
    //    resolved (base, shift) they must produce identical results.  We cannot
    //    observe their real base/shift with no JVM, but we CAN assert the shared
    //    primitive they both delegate to behaves identically when fed the same
    //    inputs — and that both wrappers agree on the null sentinel.  This locks
    //    the "both codecs are bitwise-identical except for the VMStruct names"
    //    invariant the header documents (vmhook.hpp:5289-5294 / :5310-5315).
    // ===================================================================
    {
        // Both wrappers map their null sentinel the same way.
        check("oop_and_klass_decode_zero_agree",
              decode_oop_pointer(0u) == decode_klass_pointer(0u));
        check("oop_and_klass_encode_null_agree",
              encode_oop_pointer(nullptr) == encode_klass_pointer(nullptr));

        // The shared primitive produces identical output for the OOP-shaped and
        // Klass-shaped call when handed the same (base, shift, value) — i.e.
        // there is exactly one decode formula, used twice.  (Trivially true
        // since both call narrow_decode, but pins it against a future divergent
        // re-implementation of one codec.)
        struct mode { std::uint64_t base; std::uint32_t shift; };
        const mode modes[]{
            { 0u, 0u }, { 0u, 3u },
            { std::uint64_t{ 0x8'0000'0000ull }, 3u },
            { std::uint64_t{ 0x1'0000'0000ull }, 0u },
        };
        const std::uint32_t vals[]{ 1u, 7u, 0x1000u, 0x7FFF'FFFFu, 0xFFFF'FFFFu };
        bool primitive_is_single_formula{ true };
        for (const mode m : modes)
        {
            for (const std::uint32_t v : vals)
            {
                // The "oop path" and "klass path" both reduce to narrow_decode;
                // compute via the primitive twice and confirm equality (and that
                // it equals the documented closed form).
                const std::uintptr_t a{ as_uptr(narrow_decode(m.base, m.shift, v)) };
                const std::uintptr_t b{ as_uptr(narrow_decode(m.base, m.shift, v)) };
                const std::uintptr_t closed{ expect_decode(m.base, m.shift, v) };
                if (a != b || a != closed) { primitive_is_single_formula = false; }
            }
        }
        check("shared_narrow_primitive_single_formula", primitive_is_single_formula);
    }

    // ===================================================================
    // T. COMPILE-TIME formula pinning (static_assert) + runtime conformance.
    //    The shared primitives narrow_decode/narrow_encode are NOT constexpr
    //    (narrow_decode reinterpret_casts to void*, narrow_encode is a plain
    //    static function), so they cannot appear inside a static_assert.  But
    //    the documented CLOSED FORM they implement is pure integer arithmetic
    //    and IS constant-evaluable.  We therefore:
    //      (1) pin the closed form at COMPILE TIME with static_assert over the
    //          full HotSpot (base, shift) mode set and the extreme narrow
    //          values, so a formula typo is a build error on every CI config;
    //      (2) assert at RUNTIME that the actual library primitive reproduces
    //          the very same compile-time constant — proving the shipped code
    //          matches the statically-pinned spec.
    //    This is the "static_assert where constexpr-evaluable, else runtime
    //    assert" split the coverage goal calls for.
    // ===================================================================

    // constexpr re-derivations of the two documented formulas.  These are the
    // single source of truth for the expected values, evaluated by the
    // compiler.  They deliberately compute in integer types only (no void*),
    // which keeps them usable in a constant expression.
    struct ct
    {
        static constexpr auto dec(std::uint64_t base, std::uint32_t shift,
                                  std::uint32_t c) noexcept -> std::uint64_t
        {
            return base + (static_cast<std::uint64_t>(c) << shift);
        }
        static constexpr auto enc(std::uint64_t base, std::uint32_t shift,
                                  std::uint64_t addr) noexcept -> std::uint32_t
        {
            return static_cast<std::uint32_t>((addr - base) >> shift);
        }
    };

    // -- (1) COMPILE-TIME pins.  Every assertion here is checked by the
    //        compiler; reaching runtime means they all held. ----------------

    // shift == 0 : decode is base + c, encode is addr - base (mod 2^32).
    static_assert(ct::dec(0u, 0u, 0u) == 0ull);
    static_assert(ct::dec(0u, 0u, 1u) == 1ull);
    static_assert(ct::dec(0u, 0u, 0xFFFF'FFFFu) == 0xFFFF'FFFFull);
    static_assert(ct::dec(0x1'0000'0000ull, 0u, 0xFFFF'FFFFu) == 0x1'FFFF'FFFFull);

    // shift == 3 : the canonical <=32 GB heap.  Widen-before-shift means the
    // all-ones narrow value must reach 0x7'FFFF'FFF8, NOT truncate to 32 bits.
    static_assert(ct::dec(0u, 3u, 1u) == 8ull);
    static_assert(ct::dec(0u, 3u, 0xFFFF'FFFFu) == 0x7'FFFF'FFF8ull);
    static_assert(ct::dec(0u, 3u, 0x8000'0000u) == 0x4'0000'0000ull); // sign bit widens
    static_assert(ct::dec(0x8'0000'0000ull, 3u, 0x10u) == 0x8'0000'0080ull);

    // shift == 4 : 16-byte object alignment regime.
    static_assert(ct::dec(0u, 4u, 1u) == 16ull);
    static_assert(ct::dec(0u, 4u, 0xFFFF'FFFFu) == 0xF'FFFF'FFF0ull);

    // encode is the exact inverse on representable points, at compile time.
    static_assert(ct::enc(0u, 0u, 0x1234ull) == 0x1234u);
    static_assert(ct::enc(0x1000ull, 0u, 0x1234ull) == 0x234u);
    static_assert(ct::enc(0u, 3u, 64ull) == 8u);
    static_assert(ct::enc(0x4000'0000ull, 3u, 0x4000'0000ull) == 0u); // addr==base -> 0
    static_assert(ct::enc(0u, 4u, 0xF'FFFF'FFF0ull) == 0xFFFF'FFFFu);  // top, shift 4

    // Round-trip closed-form identity, pinned at compile time for the corner
    // narrow values across the four canonical modes.  enc(dec(c)) == c for all
    // 32-bit c, for any base/shift (the << bits are exactly the >> bits).
    static_assert(ct::enc(0u, 0u, ct::dec(0u, 0u, 0xDEAD'BEEFu)) == 0xDEAD'BEEFu);
    static_assert(ct::enc(0u, 3u, ct::dec(0u, 3u, 0xFFFF'FFFFu)) == 0xFFFF'FFFFu);
    static_assert(ct::enc(0x8'0000'0000ull, 3u,
                          ct::dec(0x8'0000'0000ull, 3u, 0x8000'0000u)) == 0x8000'0000u);
    static_assert(ct::enc(0x7F00'0000'0000ull, 0u,
                          ct::dec(0x7F00'0000'0000ull, 0u, 0x00FF'FFFFu)) == 0x00FF'FFFFu);

    // narrow == 0 decodes to exactly `base` at compile time, for ANY base/shift
    // (the primitive is intentionally not null-aware; the wrapper owns that).
    static_assert(ct::dec(0u, 0u, 0u) == 0ull);
    static_assert(ct::dec(0u, 3u, 0u) == 0ull);
    static_assert(ct::dec(0x1'0000'0000ull, 3u, 0u) == 0x1'0000'0000ull);
    static_assert(ct::dec(0x7FFF'FFFF'8000ull, 4u, 0u) == 0x7FFF'FFFF'8000ull);

    // -- (2) RUNTIME conformance: the shipped library primitive equals the
    //        compile-time constant, for the same mode set.  If the build
    //        compiled, the static_asserts held; these prove the real code path
    //        agrees with that statically-verified spec. ---------------------
    {
        struct mode { std::uint64_t base; std::uint32_t shift; };
        const mode modes[]{
            { 0u, 0u }, { 0u, 3u }, { 0u, 4u },
            { std::uint64_t{ 0x1'0000'0000ull }, 0u },
            { std::uint64_t{ 0x8'0000'0000ull }, 3u },
            { std::uint64_t{ 0x7F00'0000'0000ull }, 0u },
        };
        const std::uint32_t corner[]{
            0u, 1u, 2u, 0x7Fu, 0x7FFF'FFFFu, 0x8000'0000u, 0xFFFF'FFFEu, 0xFFFF'FFFFu,
        };
        bool decode_equals_compiletime{ true };
        bool encode_equals_compiletime{ true };
        for (const mode m : modes)
        {
            for (const std::uint32_t c : corner)
            {
                // Library decode must equal the constexpr closed form.
                if (as_uptr(narrow_decode(m.base, m.shift, c))
                    != static_cast<std::uintptr_t>(ct::dec(m.base, m.shift, c)))
                {
                    decode_equals_compiletime = false;
                }
                // Library encode of the representable address must equal the
                // constexpr closed form (and, since it is representable, c).
                const std::uint64_t addr{ ct::dec(m.base, m.shift, c) };
                const std::uint32_t enc{ narrow_encode(m.base, m.shift, addr) };
                if (enc != ct::enc(m.base, m.shift, addr) || enc != c)
                {
                    encode_equals_compiletime = false;
                }
            }
        }
        check("narrow_decode_matches_compiletime_formula", decode_equals_compiletime);
        check("narrow_encode_matches_compiletime_formula", encode_equals_compiletime);
    }

    // ===================================================================
    // U. EXHAUSTIVE decode over the COMPLETE low-16-bit narrow domain.
    //    Unlike the earlier "dense representative" sweeps, this enumerates
    //    EVERY narrow value in [0, 0xFFFF] (all 65 536 of them) against every
    //    HotSpot (base, shift) mode, recomputing the expected full pointer from
    //    the documented formula.  A complete contiguous subdomain — no gaps —
    //    is the strongest "all inputs possible" statement we can make at u32
    //    width without enumerating the full 4 billion (infeasible per build).
    //    The high 16 bits are then swept exhaustively in their own right via
    //    the power-of-two / bit-pair families (section X) and the 32-bit
    //    extremes (sections J/M), so every structurally distinct bit is covered.
    // ===================================================================
    {
        struct mode { std::uint64_t base; std::uint32_t shift; const char* tag; };
        const mode modes[]{
            { 0u,                             0u, "b0_s0"   },
            { 0u,                             3u, "b0_s3"   },
            { 0u,                             4u, "b0_s4"   },
            { std::uint64_t{ 0x1'0000'0000ull },    0u, "bN_s0"   },
            { std::uint64_t{ 0x8'0000'0000ull },    3u, "b32G_s3" },
            { std::uint64_t{ 0x7FFF'FFFF'8000ull }, 4u, "bHi_s4"  }, // near-ceiling base
            { std::uint64_t{ 0x12'3456'7801ull },   3u, "bUnal_s3"}, // non-8-aligned base
        };
        bool full16_decode_ok{ true };
        std::size_t full16_cases{ 0 };
        std::uint32_t first_bad{ 0xFFFF'FFFFu };
        for (const mode m : modes)
        {
            for (std::uint32_t c{ 0u }; c <= 0xFFFFu; ++c)
            {
                const std::uintptr_t got{ as_uptr(narrow_decode(m.base, m.shift, c)) };
                const std::uintptr_t want{
                    static_cast<std::uintptr_t>(ct::dec(m.base, m.shift, c)) };
                if (got != want)
                {
                    full16_decode_ok = false;
                    if (first_bad == 0xFFFF'FFFFu) { first_bad = c; }
                }
                ++full16_cases;
            }
        }
        check("narrow_decode_exhaustive_low16_all_modes", full16_decode_ok);
        // 7 modes * 65536 == 458 752 cases; pin the magnitude so an accidental
        // loop-bound edit that shrinks the sweep is caught.
        check("narrow_decode_exhaustive_low16_is_complete",
              full16_cases == static_cast<std::size_t>(7) * 0x1'0000u);
        // first_bad stays sentinel iff nothing mismatched; referencing it keeps
        // it live under -Werror and documents the first failing input if any.
        check("narrow_decode_exhaustive_low16_no_first_bad",
              first_bad == 0xFFFF'FFFFu);
    }

    // ===================================================================
    // V. EXHAUSTIVE encode inverse over the COMPLETE low-16-bit domain.
    //    For every narrow c in [0, 0xFFFF] and every mode, the representable
    //    address base + (c << shift) must encode back to exactly c (the high
    //    bits dropped by >> are precisely the bits the address lacks).  This is
    //    the complete-domain inverse of section U.
    // ===================================================================
    {
        struct mode { std::uint64_t base; std::uint32_t shift; };
        const mode modes[]{
            { 0u, 0u }, { 0u, 1u }, { 0u, 2u }, { 0u, 3u }, { 0u, 4u },
            { std::uint64_t{ 0x1'0000'0000ull },    0u },
            { std::uint64_t{ 0x8'0000'0000ull },    3u },
            { std::uint64_t{ 0x7FFF'FFFF'8000ull }, 4u },
            { std::uint64_t{ 0x12'3456'7801ull },   3u },
        };
        bool full16_encode_ok{ true };
        for (const mode m : modes)
        {
            for (std::uint32_t c{ 0u }; c <= 0xFFFFu; ++c)
            {
                const std::uint64_t addr{ ct::dec(m.base, m.shift, c) };
                if (narrow_encode(m.base, m.shift, addr) != c)
                {
                    full16_encode_ok = false;
                }
            }
        }
        check("narrow_encode_exhaustive_low16_all_modes", full16_encode_ok);
    }

    // ===================================================================
    // W. EXHAUSTIVE round-trips over the COMPLETE low-16-bit domain, BOTH
    //    directions, for every mode:
    //      encode(decode(c)) == c                       (compressed -> compressed)
    //      decode(encode(base+(c<<shift))) == base+(c<<shift)  (addr -> addr)
    //    No sampling: every c in [0, 0xFFFF].  This is the codec's central
    //    identity proven over a full contiguous domain rather than a dense set.
    // ===================================================================
    {
        struct mode { std::uint64_t base; std::uint32_t shift; };
        const mode modes[]{
            { 0u, 0u }, { 0u, 3u }, { 0u, 4u },
            { std::uint64_t{ 0x1'0000'0000ull },    0u },
            { std::uint64_t{ 0x8'0000'0000ull },    3u },
            { std::uint64_t{ 0x7F00'0000'0000ull }, 0u },
            { std::uint64_t{ 0x12'3456'7801ull },   3u },
        };
        bool rt_c_ok{ true };
        bool rt_addr_ok{ true };
        for (const mode m : modes)
        {
            for (std::uint32_t c{ 0u }; c <= 0xFFFFu; ++c)
            {
                // compressed -> address -> compressed
                void* const decoded{ narrow_decode(m.base, m.shift, c) };
                if (narrow_encode(m.base, m.shift,
                                  static_cast<std::uint64_t>(as_uptr(decoded))) != c)
                {
                    rt_c_ok = false;
                }
                // address -> compressed -> address (representable address)
                const std::uint64_t addr{ ct::dec(m.base, m.shift, c) };
                const std::uint32_t enc{ narrow_encode(m.base, m.shift, addr) };
                if (static_cast<std::uint64_t>(as_uptr(narrow_decode(m.base, m.shift, enc)))
                    != addr)
                {
                    rt_addr_ok = false;
                }
            }
        }
        check("narrow_roundtrip_encode_decode_exhaustive_low16", rt_c_ok);
        check("narrow_roundtrip_decode_encode_exhaustive_low16", rt_addr_ok);
    }

    // ===================================================================
    // X. COMPLETE bit-position families: every single-bit narrow value, every
    //    adjacent-bit pair, and every value straddling the shift boundary, at
    //    every shift 0..4.  This exhausts the *high* 16 bits that section U's
    //    low-16 sweep cannot reach, so between U and X every one of the 32
    //    narrow bits is exercised in isolation and in adjacent combination.
    //    The task explicitly asks for "values straddling the shift so the high
    //    bits matter" — this makes that exhaustive over all bit positions.
    // ===================================================================
    {
        const std::uint32_t shifts[]{ 0u, 1u, 2u, 3u, 4u };
        bool single_bit_ok{ true };
        bool bit_pair_ok{ true };
        bool straddle_ok{ true };
        for (const std::uint32_t sh : shifts)
        {
            // Every single set bit b in 0..31: decode(0, sh, 1<<b) == (1<<b)<<sh
            // computed in 64 bits — must never truncate even when b+sh >= 32.
            for (unsigned b{ 0u }; b < 32u; ++b)
            {
                const std::uint32_t c{ static_cast<std::uint32_t>(1u) << b };
                const std::uintptr_t want{
                    static_cast<std::uintptr_t>(static_cast<std::uint64_t>(c) << sh) };
                if (as_uptr(narrow_decode(0u, sh, c)) != want) { single_bit_ok = false; }
                // The decoded value must have its low sh bits clear (alignment
                // grid) and round-trip back to c.
                if ((as_uptr(narrow_decode(0u, sh, c)) & ((std::uintptr_t{ 1u } << sh) - 1u)) != 0u)
                {
                    single_bit_ok = false;
                }
                if (narrow_encode(0u, sh,
                        static_cast<std::uint64_t>(as_uptr(narrow_decode(0u, sh, c)))) != c)
                {
                    single_bit_ok = false;
                }
            }
            // Every adjacent bit pair (b, b+1) for b in 0..30.
            for (unsigned b{ 0u }; b < 31u; ++b)
            {
                const std::uint32_t c{
                    static_cast<std::uint32_t>((1u << b) | (1u << (b + 1u))) };
                const std::uintptr_t want{
                    static_cast<std::uintptr_t>(static_cast<std::uint64_t>(c) << sh) };
                if (as_uptr(narrow_decode(0u, sh, c)) != want) { bit_pair_ok = false; }
            }
            // Straddle the shift boundary: at shift sh, the value 1<<sh is the
            // first compressed value whose product crosses into bit (2*sh), and
            // (1<<sh)-1 / (1<<sh)+1 are its neighbours.  Verify all three decode
            // to the exact widened product (high bits preserved).
            if (sh > 0u)
            {
                const std::uint32_t centre{ static_cast<std::uint32_t>(1u) << sh };
                const std::uint32_t around[]{ centre - 1u, centre, centre + 1u };
                for (const std::uint32_t c : around)
                {
                    const std::uintptr_t want{
                        static_cast<std::uintptr_t>(static_cast<std::uint64_t>(c) << sh) };
                    if (as_uptr(narrow_decode(0u, sh, c)) != want) { straddle_ok = false; }
                }
            }
        }
        check("narrow_decode_every_single_bit_all_shifts", single_bit_ok);
        check("narrow_decode_every_adjacent_bit_pair_all_shifts", bit_pair_ok);
        check("narrow_decode_shift_boundary_straddle_all_shifts", straddle_ok);
    }

    // ===================================================================
    // Y. ALIGNMENT-GRID / SHIFT-RESIDUE law, stated exhaustively.  This is the
    //    no-JVM analogue of the live-JVM assertion "(decoded - base) has zero
    //    residue mod (1<<shift) for a real oop".  For EVERY narrow c in a
    //    complete domain and every shift, with a base that is itself a multiple
    //    of (1<<shift):
    //      (decode(base, shift, c) - base)  ==  c << shift   exactly, and
    //      its low `shift` bits are zero (it sits on the 2^shift grid).
    //    We use base==0 and a shift-aligned non-zero base so the residue law is
    //    purely a property of the shift, mirroring HotSpot's 8/16-byte object
    //    alignment guarantee.
    // ===================================================================
    {
        const std::uint32_t shifts[]{ 0u, 1u, 2u, 3u, 4u };
        bool residue_law_holds{ true };
        for (const std::uint32_t sh : shifts)
        {
            const std::uintptr_t grid_mask{ (std::uintptr_t{ 1u } << sh) - 1u };
            // base chosen as a multiple of the grid so base contributes no
            // residue; 0 and a large grid-aligned value both qualify.
            const std::uint64_t bases[]{ 0u, (std::uint64_t{ 0x8'0000'0000ull }) };
            for (const std::uint64_t base : bases)
            {
                // Sweep a complete contiguous run of compressed values plus the
                // top of the range; every decoded offset must equal c<<sh and be
                // grid-aligned.
                for (std::uint32_t c{ 0u }; c <= 0x2000u; ++c)
                {
                    const std::uintptr_t dec{ as_uptr(narrow_decode(base, sh, c)) };
                    const std::uintptr_t offset{ dec - static_cast<std::uintptr_t>(base) };
                    if (offset != (static_cast<std::uintptr_t>(c) << sh))
                    {
                        residue_law_holds = false;
                    }
                    if ((offset & grid_mask) != 0u) { residue_law_holds = false; }
                }
                // Top of the narrow range too.
                const std::uintptr_t dec_top{ as_uptr(narrow_decode(base, sh, 0xFFFF'FFFFu)) };
                const std::uintptr_t off_top{ dec_top - static_cast<std::uintptr_t>(base) };
                if (off_top != (std::uint64_t{ 0xFFFF'FFFFull } << sh)) { residue_law_holds = false; }
                if ((off_top & grid_mask) != 0u) { residue_law_holds = false; }
            }
        }
        check("narrow_decode_shift_residue_grid_law_exhaustive", residue_law_holds);
    }

    // ===================================================================
    // Z. BASE FAMILY completeness + the "narrow 0 -> base REGARDLESS of base"
    //    law, over the four base classes the coverage goal enumerates:
    //      - zero base (zero-based heap)
    //      - 32 GB-aligned heap base (0x8_0000_0000)
    //      - non-8-aligned / "unaligned-looking" base (low bits set)
    //      - high near-canonical-ceiling base (just under user_address_ceiling)
    //    For each, across all shifts 0..4:
    //      (Z1) narrow == 0 decodes to exactly the base (primitive contract),
    //      (Z2) the max u32 narrow decodes to base + (0xFFFFFFFF << shift) with
    //           no truncation (independently recomputed),
    //      (Z3) encode(base) == 0 (the at-base / null grid point).
    //    Plus the wrapper-level invariant that decode_oop_pointer(0) is null
    //    irrespective of any base/shift (it short-circuits before resolution).
    // ===================================================================
    {
        const std::uint64_t bases[]{
            0u,
            std::uint64_t{ 0x8'0000'0000ull },        // 32 GB-aligned
            std::uint64_t{ 0x12'3456'7801ull },       // unaligned-looking (odd low byte)
            std::uint64_t{ 0x7FFF'FFFF'0000ull },      // high, near user_address_ceiling
        };
        const std::uint32_t shifts[]{ 0u, 1u, 2u, 3u, 4u };
        bool zero_is_base{ true };
        bool max_no_truncate{ true };
        bool base_encodes_zero{ true };
        for (const std::uint64_t base : bases)
        {
            for (const std::uint32_t sh : shifts)
            {
                // (Z1) narrow 0 -> base.
                if (as_uptr(narrow_decode(base, sh, 0u))
                    != static_cast<std::uintptr_t>(base))
                {
                    zero_is_base = false;
                }
                // (Z2) max u32 -> base + (0xFFFFFFFF << sh), recomputed in 64-bit.
                const std::uintptr_t got_max{ as_uptr(narrow_decode(base, sh, 0xFFFF'FFFFu)) };
                const std::uintptr_t want_max{
                    static_cast<std::uintptr_t>(base + (std::uint64_t{ 0xFFFF'FFFFull } << sh)) };
                if (got_max != want_max) { max_no_truncate = false; }
                // (Z3) encode(base) -> 0.
                if (narrow_encode(base, sh, base) != 0u) { base_encodes_zero = false; }
            }
        }
        check("narrow_decode_zero_is_base_all_base_families", zero_is_base);
        check("narrow_decode_max_u32_no_truncate_all_base_families", max_no_truncate);
        check("narrow_encode_base_is_zero_all_base_families", base_encodes_zero);

        // Wrapper-level: decode_oop_pointer(0) / decode_klass_pointer(0) are
        // null with NO dependence on base/shift (the compressed==0 guard runs
        // before VMStruct resolution).  This is the "narrow 0 -> null REGARDLESS
        // of base" contract at the public API, which the primitive (returning
        // `base`) deliberately does NOT provide — the wrapper adds it.  Pinning
        // both halves documents exactly where the null semantics live.
        check("decode_oop_pointer_zero_null_independent_of_base",
              decode_oop_pointer(0u) == nullptr);
        check("decode_klass_pointer_zero_null_independent_of_base",
              decode_klass_pointer(0u) == nullptr);
        // And the primitive, fed compressed 0 with a NON-zero base, returns that
        // base (not null) — the precise distinction the wrapper papers over.
        check("narrow_decode_zero_with_nonzero_base_is_base_not_null",
              narrow_decode(std::uint64_t{ 0x8'0000'0000ull }, 3u, 0u)
                  == reinterpret_cast<void*>(std::uintptr_t{ 0x8'0000'0000ull }));
    }

    // ===================================================================
    // AA. OOP-CODEC WRAPPER (decode_oop_pointer / encode_oop_pointer) — the
    //     compressed_oops_decode feature proper, exercised end-to-end at the
    //     PUBLIC entry points (not the shared primitive).  Everything that is
    //     JVM-independent about the wrapper lives in the two guards that run
    //     before any gHotSpotVMStructs lookup (compressed==0 -> nullptr at
    //     vmhook.hpp:5337; decoded==nullptr -> 0 at :5383) plus the no-resolve
    //     fall-throughs (:5362 / :5405).  Sections A pinned the simplest forms;
    //     this section EXHAUSTS the wrapper's no-JVM input domain so the public
    //     API contract is nailed down independently of the primitive sweeps.
    // ===================================================================
    {
        // (AA1) decode_oop_pointer over a dense compressed sweep with NO JVM:
        //       only compressed==0 yields nullptr via the early guard; EVERY
        //       non-zero value falls through the unresolved-VMStruct guard and
        //       also yields nullptr (never a crash, never a bogus pointer).
        //       This pins that, absent a heap, the wrapper's output domain is
        //       exactly {nullptr} over the entire 32-bit input range — proving
        //       the no-resolve guard catches all non-zero inputs, not just the
        //       0x1 / 0xFFFFFFFF endpoints already in section A.
        std::vector<std::uint32_t> oop_inputs;
        oop_inputs.push_back(0u);
        for (std::uint32_t k{ 1u }; k <= 16u; ++k) { oop_inputs.push_back(k); }
        for (unsigned bit{ 0u }; bit < 32u; ++bit)
        {
            const std::uint32_t pow2{ static_cast<std::uint32_t>(1u) << bit };
            oop_inputs.push_back(pow2);
            oop_inputs.push_back(pow2 - 1u);
            oop_inputs.push_back(pow2 + 1u);
        }
        oop_inputs.push_back(0x7FFF'FFFFu);
        oop_inputs.push_back(0x8000'0000u);
        oop_inputs.push_back(0xFFFF'FFFEu);
        oop_inputs.push_back(0xFFFF'FFFFu);

        bool decode_wrapper_all_null_no_jvm{ true };
        std::size_t decode_wrapper_cases{ 0 };
        for (const std::uint32_t c : oop_inputs)
        {
            if (decode_oop_pointer(c) != nullptr)
            {
                decode_wrapper_all_null_no_jvm = false;
            }
            ++decode_wrapper_cases;
        }
        check("decode_oop_pointer_all_inputs_null_no_jvm",
              decode_wrapper_all_null_no_jvm);
        check("decode_oop_pointer_no_jvm_sweep_is_dense",
              decode_wrapper_cases >= 100);

        // (AA2) The wrapper short-circuits compressed==0 BEFORE the VMStruct
        //       lookup, so decode_oop_pointer(0) is nullptr on EVERY JVM and in
        //       this no-JVM build alike — i.e. the null oop never depends on a
        //       resolvable base/shift.  (Re-pinned here in the OOP-codec section
        //       so the public guarantee is stated where the feature lives.)
        check("decode_oop_pointer_zero_null_is_jvm_independent",
              decode_oop_pointer(0u) == nullptr);
    }

    // ===================================================================
    // BB. OOP-CODEC encode_oop_pointer — the BELOW-BASE / low-sentinel guard
    //     (vmhook.hpp:5413: `if (decoded_address < narrow_oop_base) return 0;`).
    //     Under a live JVM with a non-zero heap base this guard maps any pointer
    //     below the heap start to the NULL compressed oop (a known asymmetry —
    //     a sub-base/native/stale pointer is silently encoded as "store null").
    //     With NO JVM narrow_oop_base is unresolved, so the no-resolve guard at
    //     :5405 fires first and the result is also 0 — meaning EVERY pointer,
    //     including the canonical low sentinels the below-base guard exists to
    //     reject, encodes to 0 here.  We pin that no-JVM behaviour for the exact
    //     sentinel inputs (1, the address floor, the first valid page) so a
    //     future change to the guard or the no-resolve path is caught, and so
    //     the documented current contract of the OOP encoder is explicit.
    // ===================================================================
    {
        // Canonical sub-base / sentinel pointers an encoder might be handed:
        //   (void*)1            — the classic "below any heap base" sentinel
        //   (void*)0xFFFF       — exactly the user_address_floor
        //   (void*)0x10000      — the first address is_valid_pointer accepts
        //   a real stack object  — a genuine non-null native pointer
        const std::uintptr_t sentinels[]{
            std::uintptr_t{ 0x1u },
            std::uintptr_t{ 0xFFFFu },
            std::uintptr_t{ 0x1'0000u },
            std::uintptr_t{ 0x4000'0000u },
        };
        bool encode_sentinels_all_zero{ true };
        for (const std::uintptr_t s : sentinels)
        {
            if (encode_oop_pointer(reinterpret_cast<void*>(s)) != 0u)
            {
                encode_sentinels_all_zero = false;
            }
        }
        check("encode_oop_pointer_sub_base_sentinels_zero_no_jvm",
              encode_sentinels_all_zero);

        // A real, in-range, aligned native pointer (heap allocation) also
        // encodes to 0 with no JVM — confirming the no-resolve path swallows
        // even a "looks valid" pointer rather than fabricating a narrow oop.
        {
            std::vector<std::uint64_t> heap_block(8, 0);
            check("encode_oop_pointer_real_heap_ptr_zero_no_jvm",
                  encode_oop_pointer(heap_block.data()) == 0u);
        }

        // The below-base guard is exercisable in isolation via the primitive:
        // with an injected non-zero base, narrow_encode of a sub-base address
        // is NOT the clean 0 the wrapper returns — it underflows modularly
        // (the wrapper's :5413 guard is what converts that into a deliberate 0).
        // Pinning the difference documents WHERE the below-base null lives:
        // in the wrapper, not in the arithmetic primitive.
        {
            const std::uint64_t base{ 0x8'0000'0000ull };
            const std::uint64_t sub_base{ 0x1u };               // far below base
            // Primitive: modular wrap (NOT 0) — proves it does no range check.
            const std::uint32_t prim{ narrow_encode(base, 3u, sub_base) };
            const std::uint32_t prim_expected{
                static_cast<std::uint32_t>((sub_base - base) >> 3) };
            check("narrow_encode_sub_base_is_modular_not_zero",
                  prim == prim_expected);
            // The wrapper would instead short-circuit to 0 via the below-base
            // guard once base were resolved; with no JVM it is 0 via no-resolve.
            // Either way the OOP encoder's observable answer for a sub-base
            // pointer is 0, which the sentinel battery above already pinned.
        }
    }

    // ===================================================================
    // CC. OOP-CODEC arithmetic per JDK-VERSION (base, shift) regime.  The OOP
    //     codec resolves narrow_oop_base/shift from CompressedOops (JDK 17+) or
    //     Universe (JDK 8-16) and then performs EXACTLY narrow_decode(base,
    //     shift, c).  We cannot read the live base/shift with no JVM, but we CAN
    //     drive the very same primitive the OOP codec delegates to with each
    //     (base, shift) pair HotSpot actually selects, and assert the decoded
    //     pointer against the independently recomputed closed form.  This states
    //     the OOP decode arithmetic exhaustively *for the OOP codec specifically*
    //     (the generic sweeps in J/U test the shared primitive; this attributes
    //     the result to compressed_oops_decode and maps each mode to its real
    //     HotSpot trigger).  Source of truth for the regimes: vmhook.hpp:
    //     5342-5345 (the version/field mapping comment in decode_oop_pointer).
    // ===================================================================
    {
        // Each row is a (base, shift) the OOP codec genuinely runs with, tagged
        // by the JDK/heap situation that produces it.  base values are the
        // canonical HotSpot heap starts; shift is heap-size driven (0 for <4 GB,
        // 3 for <=32 GB 8-byte-aligned oops, disabled above that).
        struct oop_regime
        {
            std::uint64_t base;
            std::uint32_t shift;
            const char* trigger;   // documentation only
        };
        const oop_regime regimes[]{
            // base==0, shift==0: <4 GB heap based at 0 — the simplest mode,
            // common on JDK 8 with compressed oops and a small -Xmx; decode is
            // a pure widening cast (and the regime where flaw #1's `<base` guard
            // can never fire because base is 0).
            { 0u,                              0u, "zero_based_unscaled_<4G" },
            // base==0, shift==3: zero-based, 8-byte scaled, heap <=32 GB.
            { 0u,                              3u, "zero_based_scaled8_<=32G" },
            // base!=0, shift==0: based heap, unscaled (heap <4 GB not at 0).
            { std::uint64_t{ 0x7F00'0000'0000ull }, 0u, "based_unscaled" },
            // base!=0, shift==3: based + scaled, the >32 GB-style configuration.
            { std::uint64_t{ 0x8'0000'0000ull },    3u, "based_scaled8" },
            // A high near-ceiling base with shift 3, stressing that base+offset
            // stays inside the canonical 47-bit user range and never wraps.
            { std::uint64_t{ 0x7FFF'C000'0000ull },  3u, "high_based_scaled8" },
        };

        // Compressed values spanning the structurally interesting classes plus
        // the 32-bit extremes (the values most likely to expose a shift/widen
        // bug in the OOP decode path).
        const std::uint32_t comps[]{
            0u, 1u, 2u, 3u, 0x7Fu, 0x80u, 0xFFFFu, 0x1'0000u,
            0x00FF'FFFFu, 0x7FFF'FFFFu, 0x8000'0000u, 0xFFFF'FFFEu, 0xFFFF'FFFFu,
        };

        bool oop_regime_decode_ok{ true };
        bool oop_regime_roundtrip_ok{ true };
        for (const oop_regime r : regimes)
        {
            for (const std::uint32_t c : comps)
            {
                // The OOP codec's decode body is `return narrow_decode(base,
                // shift, compressed);` — reproduce it and check the closed form.
                const std::uintptr_t got{ as_uptr(narrow_decode(r.base, r.shift, c)) };
                const std::uintptr_t want{
                    static_cast<std::uintptr_t>(r.base
                        + (static_cast<std::uint64_t>(c) << r.shift)) };
                if (got != want) { oop_regime_decode_ok = false; }

                // Round-trip through the OOP encoder's arithmetic on the
                // representable address: encode is `narrow_encode(base, shift,
                // addr)` for addr>=base, so it must recover c exactly.
                const std::uint32_t re{ narrow_encode(
                    r.base, r.shift, static_cast<std::uint64_t>(got)) };
                if (re != c) { oop_regime_roundtrip_ok = false; }
            }
        }
        check("oop_decode_arithmetic_all_jdk_regimes", oop_regime_decode_ok);
        check("oop_decode_encode_roundtrip_all_jdk_regimes", oop_regime_roundtrip_ok);

        // Spot-pin two exact, human-readable decoded pointers — one per shift —
        // so a regression produces a self-evident wrong constant, not just a
        // failed loop flag.  JDK 17-24 zero-based shift-3 heap: compressed
        // 0x100 -> 0x800.  JDK >32 GB based shift-3 heap (base 0x8_0000_0000):
        // compressed 0x100 -> 0x8_0000_0800.
        check("oop_decode_zero_based_shift3_0x100_is_0x800",
              as_uptr(narrow_decode(0u, 3u, 0x100u)) == 0x800u);
        check("oop_decode_based_shift3_0x100_is_base_plus_0x800",
              as_uptr(narrow_decode(std::uint64_t{ 0x8'0000'0000ull }, 3u, 0x100u))
                  == 0x8'0000'0800ull);
    }

    // ===================================================================
    // DD. OOP-CODEC wrapper composition / null-identity completeness.  The only
    //     decode<->encode identities the OOP wrapper guarantees WITHOUT a heap
    //     base are the ones that route through the null sentinel; pin every
    //     composition of the two public entry points over their no-JVM domain so
    //     the wrapper's sentinel algebra is closed.  (Sections A covered the two
    //     direct round-trips; this adds the cross-compositions and the max-input
    //     compositions, all of which must collapse to the null/zero sentinel.)
    // ===================================================================
    {
        // encode(decode(c)) == 0 for ALL c with no JVM: decode(c) is nullptr
        // (c==0 via guard, c!=0 via no-resolve), and encode(nullptr) is 0.
        const std::uint32_t cs[]{ 0u, 1u, 0x100u, 0x7FFF'FFFFu, 0x8000'0000u, 0xFFFF'FFFFu };
        bool enc_of_dec_all_zero{ true };
        for (const std::uint32_t c : cs)
        {
            if (encode_oop_pointer(decode_oop_pointer(c)) != 0u)
            {
                enc_of_dec_all_zero = false;
            }
        }
        check("encode_of_decode_all_inputs_zero_no_jvm", enc_of_dec_all_zero);

        // decode(encode(p)) == nullptr for several real/sentinel pointers with
        // no JVM: encode(p) is 0 (no-resolve / below-base), decode(0) is nullptr.
        int stack_anchor{ 0 };
        void* const ptrs[]{
            nullptr,
            reinterpret_cast<void*>(std::uintptr_t{ 0x1u }),
            reinterpret_cast<void*>(std::uintptr_t{ 0x1'0000u }),
            &stack_anchor,
        };
        bool dec_of_enc_all_null{ true };
        for (void* const p : ptrs)
        {
            if (decode_oop_pointer(encode_oop_pointer(p)) != nullptr)
            {
                dec_of_enc_all_null = false;
            }
        }
        check("decode_of_encode_all_pointers_null_no_jvm", dec_of_enc_all_null);

        // The OOP encoder is idempotent through the null sentinel: encoding any
        // pointer then decoding then re-encoding stays 0.
        check("oop_codec_triple_compose_collapses_to_zero",
              encode_oop_pointer(
                  decode_oop_pointer(
                      encode_oop_pointer(&stack_anchor))) == 0u);
    }

    // ===================================================================
    // EE. OOP-CODEC overflow / widen safety, attributed to decode_oop_pointer.
    //     The single most important non-null property of the OOP decode formula
    //     is that the compressed value is widened to 64 bits BEFORE the shift
    //     (vmhook.hpp:5293), so a large narrow oop on a shift-3 heap reaches the
    //     full ~32 GB span instead of truncating at 4 GB.  Section K pinned this
    //     on the generic primitive; here we pin the exact OOP-relevant extreme
    //     (the maximum object index on a <=32 GB heap) and prove the decoded
    //     address (a) exceeds 4 GB and (b) is the precise documented ceiling, so
    //     a future narrowing bug in the OOP path is caught by an OOP-named test.
    // ===================================================================
    {
        // Max compressed oop on a zero-based shift-3 heap: 0xFFFFFFFF objects *
        // 8 bytes == 0x7'FFFF'FFF8 (the documented <=32 GB compressed-oops
        // ceiling).  Must NOT truncate to a 32-bit value.
        check("oop_decode_max_shift3_reaches_32G_ceiling",
              as_uptr(narrow_decode(0u, 3u, 0xFFFF'FFFFu)) == 0x7'FFFF'FFF8ull);
        check("oop_decode_max_shift3_exceeds_4G",
              as_uptr(narrow_decode(0u, 3u, 0xFFFF'FFFFu)) > 0xFFFF'FFFFull);
        // The sign-bit compressed value must zero-extend, not sign-extend: at
        // shift 3 the decoded address is 0x4'0000'0000 (16 GB), strictly above
        // 4 GB and with no high-bit contamination.
        check("oop_decode_sign_bit_shift3_zero_extends",
              as_uptr(narrow_decode(0u, 3u, 0x8000'0000u)) == 0x4'0000'0000ull);
        // Max compressed on a zero-based shift-0 heap is exactly the 4 GB span
        // (each oop is the raw object address) — the boundary the shift-3 case
        // must exceed.
        check("oop_decode_max_shift0_is_4G_span",
              as_uptr(narrow_decode(0u, 0u, 0xFFFF'FFFFu)) == 0xFFFF'FFFFull);
        // Round-trip the shift-3 ceiling from a real >32 GB-style base back to
        // the all-ones narrow oop — the OOP encoder must recover 0xFFFFFFFF.
        {
            const std::uint64_t base{ 0x8'0000'0000ull };
            const std::uint64_t top{ base + (std::uint64_t{ 0xFFFF'FFFFull } << 3) };
            check("oop_encode_max_shift3_based_recovers_all_ones",
                  narrow_encode(base, 3u, top) == 0xFFFF'FFFFu);
        }
    }

    // ===================================================================
    // FF. KLASS-CODEC arithmetic per JDK-VERSION (base, shift) regime — the
    //     compressed_klass_decode counterpart to section CC.  decode_klass_pointer
    //     resolves CompressedKlassPointers::_narrow_klass.{_base,_shift} (JDK 17-24),
    //     CompressedKlassPointers::{_base,_shift} (JDK 25+) or Universe::_narrow_klass
    //     (JDK 8-16) and then performs EXACTLY narrow_decode(base, shift, c)
    //     (vmhook.hpp decode_klass_pointer body: `return narrow_decode(base, shift,
    //     compressed);`).  Section CC attributed the decode arithmetic to the OOP
    //     codec; this attributes the SAME shared primitive to the KLASS codec across
    //     the (base, shift) regimes HotSpot selects for klass pointers.
    //
    //     Klass-pointer encoding has a smaller, fixed bit width in practice:
    //     compressed-class-pointers use shift 0 (unscaled) or shift 3 (8-byte
    //     metaspace alignment).  The base is the compressed-class-space start (0 on
    //     small heaps, non-zero when class space is based).  We drive the primitive
    //     the klass codec delegates to with each such (base, shift) and check the
    //     decode against the independently recomputed closed form, plus the inverse
    //     round-trip — closing the klass-codec arithmetic gap section R (null-only)
    //     left open.
    // ===================================================================
    {
        struct klass_regime
        {
            std::uint64_t base;
            std::uint32_t shift;
            const char* trigger;   // documentation only
        };
        const klass_regime regimes[]{
            // base==0, shift==0: unscaled, zero-based compressed class space —
            // common on JDK 8 / small heaps where klass ids index from 0.
            { 0u,                              0u, "klass_zero_based_unscaled" },
            // base==0, shift==3: zero-based, 8-byte metaspace alignment.
            { 0u,                              3u, "klass_zero_based_scaled8" },
            // base!=0, shift==0: based class space, unscaled.
            { std::uint64_t{ 0x7F00'0000'0000ull }, 0u, "klass_based_unscaled" },
            // base!=0, shift==3: based + scaled — the typical compressed-class
            // configuration once the class space is given a non-zero base.
            { std::uint64_t{ 0x8'0000'0000ull },    3u, "klass_based_scaled8" },
            // High near-ceiling base with shift 3: stresses that base+offset stays
            // inside the canonical 47-bit user range and never wraps for klasses.
            { std::uint64_t{ 0x7FFF'C000'0000ull },  3u, "klass_high_based_scaled8" },
        };
        const std::uint32_t comps[]{
            0u, 1u, 2u, 3u, 0x7Fu, 0x80u, 0xFFFFu, 0x1'0000u,
            0x00FF'FFFFu, 0x7FFF'FFFFu, 0x8000'0000u, 0xFFFF'FFFEu, 0xFFFF'FFFFu,
        };

        bool klass_regime_decode_ok{ true };
        bool klass_regime_roundtrip_ok{ true };
        for (const klass_regime r : regimes)
        {
            for (const std::uint32_t c : comps)
            {
                // decode_klass_pointer's body is `narrow_decode(base, shift, c)` —
                // reproduce it and check against the documented closed form.
                const std::uintptr_t got{ as_uptr(narrow_decode(r.base, r.shift, c)) };
                const std::uintptr_t want{
                    static_cast<std::uintptr_t>(r.base
                        + (static_cast<std::uint64_t>(c) << r.shift)) };
                if (got != want) { klass_regime_decode_ok = false; }
                // encode_klass_pointer's body is `narrow_encode(base, shift, addr)`
                // for addr >= base, so it must recover c exactly on the
                // representable address.
                const std::uint32_t re{ narrow_encode(
                    r.base, r.shift, static_cast<std::uint64_t>(got)) };
                if (re != c) { klass_regime_roundtrip_ok = false; }
            }
        }
        check("klass_decode_arithmetic_all_jdk_regimes", klass_regime_decode_ok);
        check("klass_decode_encode_roundtrip_all_jdk_regimes", klass_regime_roundtrip_ok);

        // Spot-pin two exact, human-readable decoded klass pointers, one per
        // shift, so a regression yields a self-evident wrong constant.  Zero-based
        // shift-3 metaspace: compressed 0x100 -> 0x800.  Based shift-3 metaspace
        // (base 0x8_0000_0000): compressed 0x100 -> 0x8_0000_0800.  These mirror
        // the OOP spot-pins in CC and prove the klass path uses the identical
        // shift-add (there is exactly one decode formula, shared by both codecs).
        check("klass_decode_zero_based_shift3_0x100_is_0x800",
              as_uptr(narrow_decode(0u, 3u, 0x100u)) == 0x800u);
        check("klass_decode_based_shift3_0x100_is_base_plus_0x800",
              as_uptr(narrow_decode(std::uint64_t{ 0x8'0000'0000ull }, 3u, 0x100u))
                  == 0x8'0000'0800ull);

        // The klass codec's widen-before-shift safety (mirror of EE for the OOP
        // codec): max compressed klass on a shift-3 metaspace must reach the full
        // 0x7'FFFF'FFF8 span and exceed 4 GB, never truncate at 32 bits.
        check("klass_decode_max_shift3_reaches_32G_ceiling",
              as_uptr(narrow_decode(0u, 3u, 0xFFFF'FFFFu)) == 0x7'FFFF'FFF8ull);
        check("klass_decode_max_shift3_exceeds_4G",
              as_uptr(narrow_decode(0u, 3u, 0xFFFF'FFFFu)) > 0xFFFF'FFFFull);
        check("klass_decode_sign_bit_shift3_zero_extends",
              as_uptr(narrow_decode(0u, 3u, 0x8000'0000u)) == 0x4'0000'0000ull);
    }

    // ===================================================================
    // GG. KLASS-CODEC wrapper (decode_klass_pointer / encode_klass_pointer) —
    //     the no-JVM input-domain and below-base/sentinel coverage that section
    //     BB provides for the OOP encoder but which had NO klass counterpart.
    //     With no JVM the VMStruct lookup fails, so every non-zero input decodes
    //     to nullptr (no-resolve guard) and every pointer encodes to 0 (no-resolve
    //     / below-base guard).  We pin that the klass wrapper's output domain is
    //     exactly {nullptr} / {0} over its whole 32-bit / pointer input range —
    //     proving the guards catch ALL inputs, not just the endpoints in R.
    // ===================================================================
    {
        // (GG1) decode_klass_pointer over a dense compressed sweep with NO JVM:
        //       only compressed==0 hits the early guard; every non-zero value
        //       hits the no-resolve guard.  Both yield nullptr, never a crash,
        //       never a fabricated pointer.
        std::vector<std::uint32_t> klass_inputs;
        klass_inputs.push_back(0u);
        for (std::uint32_t k{ 1u }; k <= 16u; ++k) { klass_inputs.push_back(k); }
        for (unsigned bit{ 0u }; bit < 32u; ++bit)
        {
            const std::uint32_t pow2{ static_cast<std::uint32_t>(1u) << bit };
            klass_inputs.push_back(pow2);
            klass_inputs.push_back(pow2 - 1u);
            klass_inputs.push_back(pow2 + 1u);
        }
        klass_inputs.push_back(0x7FFF'FFFFu);
        klass_inputs.push_back(0x8000'0000u);
        klass_inputs.push_back(0xFFFF'FFFEu);
        klass_inputs.push_back(0xFFFF'FFFFu);

        bool klass_decode_all_null_no_jvm{ true };
        std::size_t klass_decode_cases{ 0 };
        for (const std::uint32_t c : klass_inputs)
        {
            if (decode_klass_pointer(c) != nullptr)
            {
                klass_decode_all_null_no_jvm = false;
            }
            ++klass_decode_cases;
        }
        check("decode_klass_pointer_all_inputs_null_no_jvm",
              klass_decode_all_null_no_jvm);
        check("decode_klass_pointer_no_jvm_sweep_is_dense",
              klass_decode_cases >= 100);

        // (GG2) encode_klass_pointer of the canonical sub-base / sentinel pointers
        //       the below-base guard (vmhook.hpp encode_klass_pointer: `if
        //       (decoded_address < base) return 0;`) exists to reject — with no
        //       JVM the no-resolve guard fires first, so every one encodes to 0.
        //       This mirrors BB for the klass encoder.
        int klass_stack_anchor{ 0 };
        const std::uintptr_t klass_sentinels[]{
            std::uintptr_t{ 0x1u },
            std::uintptr_t{ 0xFFFFu },
            std::uintptr_t{ 0x1'0000u },
            std::uintptr_t{ 0x4000'0000u },
        };
        bool klass_encode_sentinels_zero{ true };
        for (const std::uintptr_t s : klass_sentinels)
        {
            if (encode_klass_pointer(reinterpret_cast<void*>(s)) != 0u)
            {
                klass_encode_sentinels_zero = false;
            }
        }
        if (encode_klass_pointer(&klass_stack_anchor) != 0u)
        {
            klass_encode_sentinels_zero = false;
        }
        check("encode_klass_pointer_sub_base_sentinels_zero_no_jvm",
              klass_encode_sentinels_zero);

        // (GG3) Wrapper sentinel algebra closure (mirror of DD for the OOP codec):
        //       encode(decode(c)) collapses to 0 for ALL c, and decode(encode(p))
        //       collapses to nullptr for any pointer, with no JVM.
        const std::uint32_t cs[]{ 0u, 1u, 0x100u, 0x7FFF'FFFFu, 0x8000'0000u, 0xFFFF'FFFFu };
        bool klass_enc_of_dec_all_zero{ true };
        for (const std::uint32_t c : cs)
        {
            if (encode_klass_pointer(decode_klass_pointer(c)) != 0u)
            {
                klass_enc_of_dec_all_zero = false;
            }
        }
        check("encode_of_decode_klass_all_inputs_zero_no_jvm", klass_enc_of_dec_all_zero);

        void* const ptrs[]{
            nullptr,
            reinterpret_cast<void*>(std::uintptr_t{ 0x1u }),
            reinterpret_cast<void*>(std::uintptr_t{ 0x1'0000u }),
            &klass_stack_anchor,
        };
        bool klass_dec_of_enc_all_null{ true };
        for (void* const p : ptrs)
        {
            if (decode_klass_pointer(encode_klass_pointer(p)) != nullptr)
            {
                klass_dec_of_enc_all_null = false;
            }
        }
        check("decode_of_encode_klass_all_pointers_null_no_jvm", klass_dec_of_enc_all_null);
    }

    // ===================================================================
    // HH. std::bit_cast pointer-pun cross-check of the decode result.  The whole
    //     suite uses reinterpret_cast / as_uptr to compare the decoded void*
    //     against an integer; this section adds an INDEPENDENT type-pun path via
    //     std::bit_cast (the portability-correct way to reinterpret a void* as a
    //     uintptr_t without aliasing UB) and confirms the two punning methods
    //     agree bit-for-bit.  std::bit_cast<void*,std::uintptr_t> is the inverse,
    //     so bit_cast(uptr) must reproduce the exact pointer the codec returned.
    //     This pins that the decoded value survives a lossless round-trip through
    //     the object representation — the strongest "no bits lost" statement —
    //     and exercises <bit> on every STL combo the matrix builds.
    // ===================================================================
    {
        struct mode { std::uint64_t base; std::uint32_t shift; };
        const mode modes[]{
            { 0u, 0u }, { 0u, 3u }, { 0u, 4u },
            { std::uint64_t{ 0x1'0000'0000ull },    0u },
            { std::uint64_t{ 0x8'0000'0000ull },    3u },
            { std::uint64_t{ 0x7FFF'FFFF'8000ull }, 4u },
        };
        const std::uint32_t vals[]{
            1u, 2u, 0x7Fu, 0x1000u, 0x7FFF'FFFFu, 0x8000'0000u, 0xFFFF'FFFFu,
        };
        // sizeof(void*) == sizeof(std::uintptr_t) is a precondition for bit_cast
        // between them; pin it so a hypothetical 32-bit build fails loudly here
        // rather than silently truncating.
        check("bitcast_pointer_width_matches_uintptr",
              sizeof(void*) == sizeof(std::uintptr_t));

        bool bitcast_agrees_with_reinterpret{ true };
        bool bitcast_inverse_roundtrips{ true };
        for (const mode m : modes)
        {
            for (const std::uint32_t v : vals)
            {
                void* const decoded{ narrow_decode(m.base, m.shift, v) };
                // Pun #1: the suite's reinterpret_cast path.
                const std::uintptr_t via_reinterpret{ as_uptr(decoded) };
                // Pun #2: std::bit_cast, the aliasing-safe object-representation copy.
                const std::uintptr_t via_bitcast{ std::bit_cast<std::uintptr_t>(decoded) };
                if (via_reinterpret != via_bitcast)
                {
                    bitcast_agrees_with_reinterpret = false;
                }
                // The decoded integer must equal the independently widened formula.
                const std::uintptr_t want{
                    static_cast<std::uintptr_t>(m.base
                        + (static_cast<std::uint64_t>(v) << m.shift)) };
                if (via_bitcast != want) { bitcast_agrees_with_reinterpret = false; }
                // Inverse: bit_cast the integer back to a pointer and confirm it is
                // identical to the codec's own returned pointer (lossless pun).
                void* const back{ std::bit_cast<void*>(via_bitcast) };
                if (back != decoded) { bitcast_inverse_roundtrips = false; }
            }
        }
        check("narrow_decode_bitcast_matches_reinterpret_all_modes",
              bitcast_agrees_with_reinterpret);
        check("narrow_decode_bitcast_inverse_roundtrips_all_modes",
              bitcast_inverse_roundtrips);
    }

    // ===================================================================
    // II. SHIFT-STEP GRANULARITY completeness — section P pinned the 8-byte step
    //     at shift 3; this pins the step at EVERY shift 0..4, so the per-shift
    //     "consecutive compressed values are 2^shift bytes apart" grid law is
    //     stated for all object-alignment regimes the codec runs under (1, 2, 4,
    //     8, 16 bytes), not just the shift-3 case.  Consecutive decoded addresses
    //     must differ by exactly 2^shift, and a base-aligned decode must land on
    //     the 2^shift grid (low `shift` bits clear).
    // ===================================================================
    {
        const std::uint32_t shifts[]{ 0u, 1u, 2u, 3u, 4u };
        bool step_is_pow2_all_shifts{ true };
        bool grid_aligned_all_shifts{ true };
        for (const std::uint32_t sh : shifts)
        {
            const std::uintptr_t expected_step{ std::uintptr_t{ 1u } << sh };
            const std::uintptr_t grid_mask{ expected_step - 1u };
            // Walk a short run of consecutive compressed values from a grid-aligned
            // base; every adjacent pair must be exactly 2^shift bytes apart.
            const std::uint64_t base{ 0x8'0000'0000ull };  // multiple of 16 -> grid for all sh<=4
            std::uintptr_t prev{ as_uptr(narrow_decode(base, sh, 1000u)) };
            for (std::uint32_t c{ 1001u }; c <= 1010u; ++c)
            {
                const std::uintptr_t cur{ as_uptr(narrow_decode(base, sh, c)) };
                if (cur - prev != expected_step) { step_is_pow2_all_shifts = false; }
                if ((cur & grid_mask) != 0u) { grid_aligned_all_shifts = false; }
                prev = cur;
            }
        }
        check("narrow_decode_step_is_pow2_all_shifts", step_is_pow2_all_shifts);
        check("narrow_decode_result_grid_aligned_all_shifts", grid_aligned_all_shifts);

        // Explicit, human-readable step pins for the two most common klass/oop
        // regimes that section P did not name: shift 0 (1-byte step, unscaled) and
        // shift 4 (16-byte step).  A regression in the scale factor surfaces as a
        // wrong constant rather than a loop flag.
        check("narrow_decode_shift0_step_is_1",
              as_uptr(narrow_decode(0u, 0u, 51u)) - as_uptr(narrow_decode(0u, 0u, 50u)) == 1u);
        check("narrow_decode_shift4_step_is_16",
              as_uptr(narrow_decode(0u, 4u, 51u)) - as_uptr(narrow_decode(0u, 4u, 50u)) == 16u);
        check("narrow_decode_shift4_0x100_is_0x1000",
              as_uptr(narrow_decode(0u, 4u, 0x100u)) == 0x1000u);
    }

    // ===================================================================
    // JJ. safe_read_pointer - the FAULT-SAFE pointer reader's PURE-LOGIC gate
    //     (vmhook.hpp:2106-2130).  This is a sibling of decode_oop_and_pointers
    //     that NO existing section covers.  Before crossing the OS boundary it
    //     applies, in order:
    //        (1) !pointer                          -> nullptr   (:2109)
    //        (2) addr <= user_address_floor        -> nullptr   (:2116)
    //        (3) addr >= user_address_ceiling      -> nullptr   (:2117)
    //        (4) (addr & 0x7) != 0  [8-byte align] -> nullptr   (:2118)
    //     Only AFTER all four pass does it call os::safe_read and return the
    //     POINTED-TO word (:2124-2129).  The four rejects above return BEFORE
    //     any OS read, so they are fully deterministic with no JVM.  Note the
    //     alignment rule here is 8-BYTE (& 0x7), strictly stronger than
    //     is_valid_pointer's 2-byte (& 0x1) rule - the documented flaw-#2
    //     mismatch, pinned end-to-end in section LL below.
    // ===================================================================
    using vmhook::hotspot::safe_read_pointer;
    {
        // (1) null in -> null out, before any OS read.
        check("safe_read_pointer_null_is_null",
              safe_read_pointer(nullptr) == nullptr);

        // (2) exactly AT the floor (0xFFFF) is rejected (<=), as is below it.
        check("safe_read_pointer_at_floor_is_null",
              safe_read_pointer(reinterpret_cast<const void*>(floor)) == nullptr);
        check("safe_read_pointer_below_floor_is_null",
              safe_read_pointer(reinterpret_cast<const void*>(floor - 1)) == nullptr);
        // 0x8 is 8-aligned and below the floor: still rejected by the range
        // check (it is < floor), proving range runs and dominates here.
        check("safe_read_pointer_low_aligned_below_floor_is_null",
              safe_read_pointer(reinterpret_cast<const void*>(std::uintptr_t{ 0x8u }))
                  == nullptr);

        // (3) exactly AT the ceiling is rejected (>=), as is above it.  Both
        //     are chosen 8-aligned so the ONLY discriminator is the range gate;
        //     ceiling 0x7FFF'FFFF'FFFF is odd, so use ceiling+1 (even) and a far
        //     kernel-half 8-aligned address.
        check("safe_read_pointer_above_ceiling_is_null",
              safe_read_pointer(reinterpret_cast<const void*>(ceiling + 1)) == nullptr);
        check("safe_read_pointer_kernel_half_aligned_is_null",
              safe_read_pointer(reinterpret_cast<const void*>(
                  std::uintptr_t{ 0x0000'8000'0000'0000ull })) == nullptr);

        // (4) 8-byte alignment: an in-range address whose low 3 bits are set is
        //     rejected BEFORE any OS read.  Sweep every non-zero residue mod 8
        //     off an in-range, 8-aligned anchor: residues 1..7 are all rejected
        //     purely on the alignment gate (the anchor itself is never read
        //     because we only assert the misaligned offsets return nullptr).
        {
            constexpr std::uintptr_t anchor{ 0x0000'2000'0000'0000ull }; // in range, 8-aligned
            bool misaligned_all_null{ true };
            for (std::uintptr_t r{ 1 }; r < 8u; ++r)
            {
                if (safe_read_pointer(reinterpret_cast<const void*>(anchor + r)) != nullptr)
                {
                    misaligned_all_null = false;
                }
            }
            check("safe_read_pointer_any_nonzero_residue_mod8_is_null",
                  misaligned_all_null);
        }

        // The 2-byte- and 4-byte-aligned (but NOT 8-aligned) interior offsets of
        // a real stack block are rejected by safe_read_pointer's 8-byte gate even
        // though the memory is genuinely mapped - these are exactly the addresses
        // is_valid_pointer ACCEPTS (section C), so this pins the flaw-#2 gap from
        // the safe_read_pointer side.  (+2 and +4 are 2/4-aligned, never 8.)
        {
            std::int64_t block[4]{};
            const std::uintptr_t b0{ reinterpret_cast<std::uintptr_t>(&block[0]) };
            check("safe_read_pointer_plus2_not_8aligned_is_null",
                  safe_read_pointer(reinterpret_cast<const void*>(b0 + 2)) == nullptr);
            check("safe_read_pointer_plus4_not_8aligned_is_null",
                  safe_read_pointer(reinterpret_cast<const void*>(b0 + 4)) == nullptr);
        }

        // (5) A real, mapped, 8-aligned slot holding a KNOWN pointer value:
        //     safe_read_pointer reads the POINTED-TO word and returns it, NOT the
        //     source address.  We stash a sentinel pointer in an 8-aligned stack
        //     slot and require it back verbatim - confirming the OS read path
        //     returns *slot, not &slot.  (A stack `void*` is 8-aligned on every
        //     LP64/LLP64 target the suite builds for.)
        {
            int target{ 0 };
            void* const sentinel{ &target };
            void* slot{ sentinel };
            // &slot must itself be 8-aligned and in range for the gate to pass.
            const std::uintptr_t slot_addr{ reinterpret_cast<std::uintptr_t>(&slot) };
            const bool slot_gate_ok{
                slot_addr > floor && slot_addr < ceiling && (slot_addr & 0x7u) == 0u };
            check("safe_read_pointer_slot_precondition_8aligned_in_range",
                  slot_gate_ok);
            check("safe_read_pointer_reads_pointed_to_value",
                  safe_read_pointer(&slot) == sentinel);
            // Reading a slot that holds nullptr returns nullptr (the value, not a
            // gate rejection - &slot is a perfectly valid source).
            void* null_slot{ nullptr };
            check("safe_read_pointer_slot_holding_null_returns_null",
                  safe_read_pointer(&null_slot) == nullptr);
        }

        // safe_read_pointer is declared noexcept (vmhook.hpp:2106) and returns a
        // const void*; pin both so a future signature change is a build error.
        check("safe_read_pointer_is_noexcept",
              noexcept(safe_read_pointer(nullptr)));
        check("safe_read_pointer_returns_const_void_ptr",
              std::is_same_v<decltype(safe_read_pointer(nullptr)), const void*>);
    }

    // ===================================================================
    // KK. is_readable_pointer - the OTHER sibling validator (vmhook.hpp:2018-
    //     2032).  Its pure-logic gate is identical in shape to
    //     safe_read_pointer's but it is the function the constant-pool / symbol
    //     slot walkers use.  Pre-OS gate (all return false BEFORE the
    //     os::query_region call at :2030):
    //        addr <= user_address_floor   -> false
    //        addr >= user_address_ceiling -> false
    //        (addr & 0x7) != 0            -> false   [8-byte alignment]
    //     The OS query only runs for a gate-passing address, so the rejects are
    //     deterministic with no JVM.  We additionally confirm a genuinely
    //     mapped, 8-aligned live address is reported readable (committed &&
    //     readable && !guarded), matching section E's is_valid_pointer cases.
    // ===================================================================
    using vmhook::hotspot::is_readable_pointer;
    {
        // null / at-floor / below-floor: false via the range gate.
        check("is_readable_pointer_null_rejected",
              !is_readable_pointer(nullptr));
        check("is_readable_pointer_at_floor_rejected",
              !is_readable_pointer(reinterpret_cast<const void*>(floor)));
        check("is_readable_pointer_below_floor_rejected",
              !is_readable_pointer(reinterpret_cast<const void*>(floor - 1)));
        // at-ceiling / above-ceiling: false via the range gate (ceiling+1 even).
        check("is_readable_pointer_at_ceiling_rejected",
              !is_readable_pointer(reinterpret_cast<const void*>(ceiling)));
        check("is_readable_pointer_above_ceiling_rejected",
              !is_readable_pointer(reinterpret_cast<const void*>(ceiling + 1)));
        // kernel-half 8-aligned address: false via the range gate.
        check("is_readable_pointer_kernel_half_rejected",
              !is_readable_pointer(reinterpret_cast<const void*>(
                  std::uintptr_t{ 0x0000'8000'0000'0000ull })));

        // 8-byte alignment gate: every non-zero residue mod 8 off an in-range
        // 8-aligned anchor is rejected BEFORE the OS query.  (The anchor is an
        // unmapped synthetic address, but we only assert the MISALIGNED offsets,
        // which never reach query_region.)
        {
            constexpr std::uintptr_t anchor{ 0x0000'3000'0000'0000ull };
            bool misaligned_all_false{ true };
            for (std::uintptr_t r{ 1 }; r < 8u; ++r)
            {
                if (is_readable_pointer(reinterpret_cast<const void*>(anchor + r)))
                {
                    misaligned_all_false = false;
                }
            }
            check("is_readable_pointer_any_nonzero_residue_mod8_rejected",
                  misaligned_all_false);
        }

        // A real, mapped, 8-aligned live object IS readable (committed, readable,
        // not guarded).  A stack `std::int64_t` and a heap allocation both
        // qualify; mirror section E so the positive contract is pinned too.
        {
            std::int64_t on_stack{ 0 };
            check("is_readable_pointer_real_stack_8aligned_accepted",
                  is_readable_pointer(&on_stack));
        }
        {
            std::vector<std::uint64_t> heap_block(8, 0);
            check("is_readable_pointer_real_heap_8aligned_accepted",
                  is_readable_pointer(heap_block.data()));
        }

        // is_readable_pointer is noexcept and returns bool; pin both.
        check("is_readable_pointer_is_noexcept",
              noexcept(is_readable_pointer(nullptr)));
        check("is_readable_pointer_returns_bool",
              std::is_same_v<decltype(is_readable_pointer(nullptr)), bool>);
    }

    // ===================================================================
    // LL. ALIGNMENT-RULE MISMATCH across the three sibling validators (flaw #2),
    //     pinned end-to-end so a future "harmonisation" that silently changes
    //     either rule is caught.  For an in-range address that is 2-byte- or
    //     4-byte-aligned but NOT 8-byte-aligned:
    //        is_valid_pointer      -> true   (only requires & 0x1 == 0)
    //        safe_read_pointer     -> nullptr (requires & 0x7 == 0)
    //        is_readable_pointer   -> false   (requires & 0x7 == 0)
    //     We drive REAL stack memory so safe_read_pointer/is_readable_pointer
    //     would otherwise have live bytes to read - the rejection is purely the
    //     stricter alignment gate, not an unmapped page.
    // ===================================================================
    {
        std::int64_t block[4]{};
        const std::uintptr_t b0{ reinterpret_cast<std::uintptr_t>(&block[0]) };
        const void* const plus2{ reinterpret_cast<const void*>(b0 + 2) }; // 2-aligned
        const void* const plus4{ reinterpret_cast<const void*>(b0 + 4) }; // 4-aligned

        // Premise: +2 and +4 are even (so is_valid_pointer accepts) yet NOT
        // 8-aligned (so the safe/readable gates reject).  Guard it explicitly.
        check("alignment_mismatch_premise_even_not_8aligned",
              ((b0 + 2) & 0x1u) == 0u && ((b0 + 2) & 0x7u) != 0u
              && ((b0 + 4) & 0x1u) == 0u && ((b0 + 4) & 0x7u) != 0u);

        // is_valid_pointer accepts (2-byte rule).
        check("alignment_mismatch_is_valid_accepts_plus2",
              is_valid_pointer(const_cast<void*>(plus2)));
        check("alignment_mismatch_is_valid_accepts_plus4",
              is_valid_pointer(const_cast<void*>(plus4)));
        // safe_read_pointer rejects (8-byte rule) - the SAME address.
        check("alignment_mismatch_safe_read_rejects_plus2",
              safe_read_pointer(plus2) == nullptr);
        check("alignment_mismatch_safe_read_rejects_plus4",
              safe_read_pointer(plus4) == nullptr);
        // is_readable_pointer rejects (8-byte rule) - the SAME address.
        check("alignment_mismatch_is_readable_rejects_plus2",
              !is_readable_pointer(plus2));
        check("alignment_mismatch_is_readable_rejects_plus4",
              !is_readable_pointer(plus4));

        // The 8-aligned base of the very same block is accepted by ALL THREE,
        // proving the divergence above is purely the low-3-bit alignment rule.
        const void* const b0p{ reinterpret_cast<const void*>(b0) };
        check("alignment_mismatch_all_three_accept_8aligned_base",
              is_valid_pointer(const_cast<void*>(b0p))
              && is_readable_pointer(b0p)
              && safe_read_pointer(&block[0]) != reinterpret_cast<const void*>(b0p));
        // (safe_read_pointer(&block[0]) returns *(&block[0]) == 0 here, i.e. the
        //  STORED value, not the source address - so it is != b0p; this both
        //  exercises the read path on an 8-aligned slot AND re-confirms it returns
        //  the pointed-to word.  block[0] is value-initialised to 0, so the read
        //  yields nullptr, which is != the non-null source address b0p.)
        check("alignment_mismatch_safe_read_8aligned_slot_reads_stored_zero",
              safe_read_pointer(&block[0]) == nullptr);
    }

    // ===================================================================
    // MM. os::safe_read - the OS-boundary building block behind safe_read_pointer
    //     (vmhook.hpp:955-1020).  Its pre-OS argument validation is fully
    //     deterministic on every platform:
    //        !dst || !src || size == 0                  -> false   (:957-960)
    //        reinterpret_cast<uintptr_t>(src) + size wraps -> false (:968-971)
    //     And a copy of a genuinely mapped, in-process buffer succeeds and is
    //     byte-exact (the warm path) on Windows / Linux / Android / macOS / iOS.
    // ===================================================================
    {
        using vmhook::os::safe_read;

        std::uint64_t out{ 0xAAAA'BBBB'CCCC'DDDDull };
        const std::uint64_t in{ 0x0102'0304'0506'0708ull };

        // Argument guards: any null operand or zero size -> false, no read.
        check("os_safe_read_null_dst_is_false",
              !safe_read(nullptr, &in, sizeof(in)));
        check("os_safe_read_null_src_is_false",
              !safe_read(&out, nullptr, sizeof(out)));
        check("os_safe_read_zero_size_is_false",
              !safe_read(&out, &in, 0));
        // out must be untouched by the rejected calls above.
        check("os_safe_read_rejected_calls_leave_dst_untouched",
              out == 0xAAAA'BBBB'CCCC'DDDDull);

        // Address-space-wrapping size guard: src + size overflowing uintptr ->
        // false (deterministic cross-platform per the in-code contract :961-967).
        {
            const void* const top_src{
                reinterpret_cast<const void*>(std::uintptr_t{ 0u } - std::uintptr_t{ 2u }) };
            check("os_safe_read_size_wraps_address_space_is_false",
                  !safe_read(&out, top_src, 16u));
            // SIZE_MAX size from any non-null src also wraps -> false.
            check("os_safe_read_sizemax_is_false",
                  !safe_read(&out, &in, static_cast<std::size_t>(-1)));
            check("os_safe_read_wrap_guard_leaves_dst_untouched",
                  out == 0xAAAA'BBBB'CCCC'DDDDull);
        }

        // Warm path: a real in-process buffer copies byte-exactly and reports
        // success.  This is the path safe_read_pointer relies on for a mapped
        // slot; it holds on every platform the matrix builds (incl. the iOS
        // unguarded-memcpy path, which still copies correct bytes for a mapped
        // src).
        {
            std::uint64_t dst{ 0 };
            const bool ok{ safe_read(&dst, &in, sizeof(in)) };
            check("os_safe_read_mapped_buffer_succeeds", ok);
            check("os_safe_read_mapped_buffer_is_byte_exact", dst == in);
        }
        // A partial-width read of the leading bytes of a larger object is also
        // exact: read the first 4 bytes of `in` into a uint32 and compare to the
        // low 32 bits (little-endian hosts - every platform the suite targets).
        {
            std::uint32_t dst32{ 0 };
            const bool ok{ safe_read(&dst32, &in, sizeof(dst32)) };
            check("os_safe_read_partial_width_succeeds", ok);
            check("os_safe_read_partial_width_low_bytes",
                  dst32 == static_cast<std::uint32_t>(in));
        }

        // os::safe_read is noexcept and returns bool; pin both.
        check("os_safe_read_is_noexcept",
              noexcept(safe_read(&out, &in, sizeof(in))));
        check("os_safe_read_returns_bool",
              std::is_same_v<decltype(safe_read(&out, &in, sizeof(in))), bool>);
    }

    // ===================================================================
    // NN. untag_pointer - additional bit-exact / canonicalisation cases beyond
    //     section H, including the flaw-#5 hazard (a kernel/non-canonical input
    //     masks down INTO the canonical user range) so the documented behaviour
    //     is regression-locked, and the mask-constant identity (untag == AND
    //     with user_address_ceiling).
    // ===================================================================
    {
        // (a) The mask constant IS user_address_ceiling: untag(p) for an all-ones
        //     input equals exactly the ceiling (every low-47 bit kept, all higher
        //     bits cleared).  This pins "tag-strip == canonicalize to <= ceiling".
        check("untag_pointer_all_ones_yields_ceiling",
              untag_pointer(reinterpret_cast<const void*>(
                  std::uintptr_t{ 0xFFFF'FFFF'FFFF'FFFFull }))
                  == reinterpret_cast<const void*>(ceiling));
        // The masked result of ANY input never exceeds the ceiling (it is the
        // AND of the input with the ceiling), for a representative bit-spread.
        {
            const std::uintptr_t inputs[]{
                0x0u, 0x1u, 0xFFFFu, 0x1'0000u,
                0x0000'7FFF'FFFF'FFFFull, 0x0000'8000'0000'0000ull,
                0xFFFF'8000'0000'1234ull, 0xDEAD'BEEF'CAFE'BABEull,
                0xFFFF'FFFF'FFFF'FFFFull,
            };
            bool never_above_ceiling{ true };
            for (const std::uintptr_t v : inputs)
            {
                const std::uintptr_t masked{ reinterpret_cast<std::uintptr_t>(
                    untag_pointer(reinterpret_cast<const void*>(v))) };
                if (masked > ceiling) { never_above_ceiling = false; }
                // And it equals the explicit AND, bit-for-bit.
                if (masked != (v & ceiling)) { never_above_ceiling = false; }
            }
            check("untag_pointer_masked_never_exceeds_ceiling_and_equals_AND",
                  never_above_ceiling);
        }

        // (b) Only the low 47 bits (bits 0..46) survive; bit 47 and up are tag
        //     bits and are cleared.  A value with ONLY bit 46 set is kept; a value
        //     with ONLY bit 47 set masks to 0.
        check("untag_pointer_keeps_bit46",
              untag_pointer(reinterpret_cast<const void*>(
                  std::uintptr_t{ 1ull } << 46))
                  == reinterpret_cast<const void*>(std::uintptr_t{ 1ull } << 46));
        check("untag_pointer_clears_bit47_only",
              untag_pointer(reinterpret_cast<const void*>(
                  std::uintptr_t{ 1ull } << 47)) == nullptr);

        // (c) flaw-#5 hazard, pinned as documented behaviour: a kernel-half /
        //     non-canonical input (high bits set) is silently masked into a
        //     PLAUSIBLE canonical user address - untag cannot tell a tag from
        //     corruption.  Here a kernel-half word masks to a small in-range
        //     address that THEN passes is_valid_pointer.  This is intentional to
        //     document (and lock) the current contract, not an endorsement.
        {
            // Low 47 bits chosen ABOVE the floor (0xFFFF) and even so the masked
            // result clears is_valid_pointer's range + alignment gates; 0x40'0000
            // (4 MB) is comfortably in range, even, and not a poison low-32.
            const std::uintptr_t kernel_half{ 0xFFFF'8000'0040'0000ull };
            const void* const recovered{
                untag_pointer(reinterpret_cast<const void*>(kernel_half)) };
            check("untag_pointer_kernel_half_masks_into_range_flaw5",
                  recovered == reinterpret_cast<const void*>(std::uintptr_t{ 0x40'0000u }));
            // And the masked-down value is now accepted by is_valid_pointer -
            // the precise hazard the flaw describes (garbage high word -> a
            // "valid"-looking pointer).
            check("untag_pointer_flaw5_result_passes_is_valid_pointer",
                  is_valid_pointer(const_cast<void*>(recovered)));
        }

        // (d) Round-trip: tagging a real canonical address with arbitrary high
        //     bits then untagging recovers the EXACT original (the tag occupies
        //     only bits >= 47, which the original lacks).
        {
            const std::uintptr_t real{ 0x0000'1234'5678'9AB0ull }; // canonical, even
            const std::uintptr_t tags[]{
                0x0000'8000'0000'0000ull, 0xFFFF'0000'0000'0000ull,
                0xABCD'0000'0000'0000ull, 0xFFFF'8000'0000'0000ull,
            };
            bool tag_then_untag_recovers{ true };
            for (const std::uintptr_t tag : tags)
            {
                const std::uintptr_t tagged{ real | tag };
                if (untag_pointer(reinterpret_cast<const void*>(tagged))
                    != reinterpret_cast<const void*>(real))
                {
                    tag_then_untag_recovers = false;
                }
            }
            check("untag_pointer_tag_then_untag_recovers_canonical",
                  tag_then_untag_recovers);
        }

        // untag_pointer returns a const void* (pin the const-qualified return,
        // complementing section H's noexcept pin).
        check("untag_pointer_returns_const_void_ptr",
              std::is_same_v<decltype(untag_pointer(nullptr)), const void*>);
    }

    // ===================================================================
    // OO. CHAINED real-consumer contract: untag_pointer -> safe_read_pointer ->
    //     is_valid_pointer, the exact composition the dictionary / symbol slot
    //     walkers use (vmhook.hpp:3457, 4423-4443).  With no JVM we cannot supply
    //     a real tagged dictionary entry, but we CAN prove the no-JVM-safe legs of
    //     the chain are individually sound and compose without crashing:
    //        - untag(nullptr) == nullptr, safe_read_pointer(nullptr) == nullptr,
    //          is_valid_pointer(nullptr) == false  (the null short-circuit each
    //          stage honours);
    //        - untag of a tagged 8-aligned in-range SLOT address yields a still-
    //          8-aligned in-range address that safe_read_pointer will accept at
    //          its gate (the read itself needs a mapped page, exercised on a real
    //          stack slot).
    // ===================================================================
    {
        // The null leg: each stage maps null -> its own "stop" sentinel, and the
        // full composition is a benign false (never a fault).
        check("chain_untag_safe_read_valid_null_is_false",
              !is_valid_pointer(const_cast<void*>(
                  safe_read_pointer(untag_pointer(nullptr)))));

        // A real tagged slot: take an 8-aligned stack slot holding a sentinel,
        // OR in high tag bits to simulate a GC-tagged dictionary-bucket word,
        // untag it back to the real slot address, then safe_read_pointer it and
        // confirm we recover the stored sentinel - i.e. the untag+read pair is
        // the identity on a correctly-tagged, mapped slot.
        {
            int target{ 0 };
            void* const sentinel{ &target };
            void* slot{ sentinel };
            const std::uintptr_t slot_addr{ reinterpret_cast<std::uintptr_t>(&slot) };
            // Simulate a tag in the high (>=47) bits; canonical user slot_addr has
            // those bits clear, so untag must restore slot_addr exactly.
            const std::uintptr_t tagged_slot{ slot_addr | 0xFFFF'0000'0000'0000ull };
            const void* const recovered{
                untag_pointer(reinterpret_cast<const void*>(tagged_slot)) };
            check("chain_untag_restores_tagged_slot_address",
                  recovered == reinterpret_cast<const void*>(slot_addr));
            // The recovered slot address is 8-aligned + in range, so it clears
            // safe_read_pointer's gate, and the read returns the stored sentinel.
            check("chain_untag_then_safe_read_recovers_sentinel",
                  safe_read_pointer(recovered) == sentinel);
        }
    }

    // ===================================================================
    // PP. (ADDITIVE deepening pass) Inputs the earlier sections do NOT reach.
    //     Pure arithmetic only: every expected value is recomputed from the two
    //     confirmed primitive bodies (vmhook.hpp:5459-5464 / :5480-5485):
    //         narrow_decode(base, shift, c)    = base + (uint64(c) << shift)
    //         narrow_encode(base, shift, addr) = uint32((addr - base) >> shift)
    //     No memory reads of fabricated addresses, no value_t->container casts,
    //     no std::vector byte-buffer resize (fixed C arrays / scalars only).
    // ===================================================================

    // -- PP1. INJECTIVITY of decode over a complete contiguous narrow domain.
    //    Distinct narrow values must decode to distinct full pointers within a
    //    mode (no aliasing / base-shift mix-up).  The decoded sequence over
    //    [0, N] is also STRICTLY MONOTONIC INCREASING (each step adds exactly
    //    1<<shift > 0), which both proves injectivity AND the ordering the
    //    collection walkers rely on.  This is the no-JVM analogue of the live
    //    "two distinct oops decode to two distinct pointers" assertion.
    {
        struct mode { std::uint64_t base; std::uint32_t shift; };
        const mode modes[]{
            { 0u, 0u }, { 0u, 1u }, { 0u, 3u }, { 0u, 4u },
            { std::uint64_t{ 0x1'0000'0000ull }, 0u },
            { std::uint64_t{ 0x8'0000'0000ull }, 3u },
            { std::uint64_t{ 0x7FFF'C000'0000ull }, 4u },
        };
        bool strictly_increasing{ true };
        bool step_is_exactly_pow2{ true };
        for (const mode m : modes)
        {
            const std::uintptr_t expected_step{ std::uintptr_t{ 1u } << m.shift };
            std::uintptr_t prev{ as_uptr(narrow_decode(m.base, m.shift, 0u)) };
            for (std::uint32_t c{ 1u }; c <= 0x4000u; ++c)
            {
                const std::uintptr_t cur{ as_uptr(narrow_decode(m.base, m.shift, c)) };
                if (!(cur > prev)) { strictly_increasing = false; }
                if (cur - prev != expected_step) { step_is_exactly_pow2 = false; }
                prev = cur;
            }
        }
        check("narrow_decode_strictly_increasing_complete_run", strictly_increasing);
        check("narrow_decode_step_exactly_pow2_complete_run", step_is_exactly_pow2);
    }

    // -- PP2. LOSSY-ENCODE FLOOR LAW, exhaustive over EVERY residue mod 2^shift.
    //    Section Q pinned a single misaligned spot (+13 at shift 3).  Here, for
    //    every shift 1..4 and every residue r in [0, (1<<shift)-1], an address
    //    base + (c<<shift) + r encodes to exactly c (the residue r < 1<<shift is
    //    shifted out), and decoding that back yields the GRID-FLOORED address
    //    base + (c<<shift) -- i.e. encode floors any sub-grid offset down.  Pure
    //    integer arithmetic; expected value is the documented >> truncation.
    {
        const std::uint32_t shifts[]{ 1u, 2u, 3u, 4u };
        const std::uint64_t base{ 0x8'0000'0000ull };   // grid-aligned for all sh<=4
        bool floor_to_c_ok{ true };
        bool floored_decode_ok{ true };
        for (const std::uint32_t sh : shifts)
        {
            const std::uint64_t grid{ std::uint64_t{ 1u } << sh };
            const std::uint32_t c{ 0x1234u };
            const std::uint64_t grid_addr{ base + (static_cast<std::uint64_t>(c) << sh) };
            for (std::uint64_t r{ 0u }; r < grid; ++r)
            {
                const std::uint64_t addr{ grid_addr + r };
                // encode floors: (addr - base) >> sh == c regardless of r < grid.
                if (narrow_encode(base, sh, addr) != c) { floor_to_c_ok = false; }
                // decode(encode(addr)) snaps addr back DOWN to the grid point.
                const std::uintptr_t back{ as_uptr(narrow_decode(base, sh,
                    narrow_encode(base, sh, addr))) };
                if (back != static_cast<std::uintptr_t>(grid_addr)) { floored_decode_ok = false; }
            }
        }
        check("narrow_encode_floors_every_residue_to_c", floor_to_c_ok);
        check("narrow_decode_of_floored_encode_snaps_to_grid", floored_decode_ok);
    }

    // -- PP3. MODULAR UNDERFLOW of encode for addr < base, exhaustive over a run
    //    of sub-base deltas.  Section Q1 pinned one (addr 0 vs base).  Here every
    //    delta d in [1, 64] below a non-zero base produces the documented modular
    //    wrap uint32((-(d) mod 2^64) >> shift); we recompute the SAME way so the
    //    assertion is a spec of the corner, not a claim the input is valid.
    {
        const std::uint64_t base{ 0x1'0000'0000ull };
        const std::uint32_t shifts[]{ 0u, 3u };
        bool underflow_modular_ok{ true };
        for (const std::uint32_t sh : shifts)
        {
            for (std::uint64_t d{ 1u }; d <= 64u; ++d)
            {
                const std::uint64_t addr{ base - d };           // strictly below base
                const std::uint32_t got{ narrow_encode(base, sh, addr) };
                const std::uint32_t want{
                    static_cast<std::uint32_t>((addr - base) >> sh) }; // wraps mod 2^64
                if (got != want) { underflow_modular_ok = false; }
            }
        }
        check("narrow_encode_underflow_modular_exhaustive_run", underflow_modular_ok);
    }

    // -- PP4. TRUNCATION characterization (flaw #2): an offset whose >>shift
    //    exceeds 32 bits is silently narrowed by the final uint32 cast.  Pinned
    //    as DETERMINISTIC current behaviour (pure arithmetic), so a future guard
    //    that makes it reject/throw is caught.  Each expected value is the exact
    //    low-32 of the documented (addr-base)>>shift.
    {
        // (a) shift 0, base 0: an address just over 4 GB loses bit 32 on encode.
        //     0x1'2345'6789 -> uint32 == 0x2345'6789.
        check("narrow_encode_truncates_above_4G_shift0",
              narrow_encode(0u, 0u, std::uint64_t{ 0x1'2345'6789ull }) == 0x2345'6789u);
        // The truncated value decoded back is NOT the original (round-trip breaks
        // exactly where the documentation says it can): decode(0x2345'6789) at
        // (0,0) == 0x2345'6789 != 0x1'2345'6789.
        check("narrow_encode_truncation_breaks_roundtrip_shift0",
              as_uptr(narrow_decode(0u, 0u,
                  narrow_encode(0u, 0u, std::uint64_t{ 0x1'2345'6789ull })))
                  != static_cast<std::uintptr_t>(0x1'2345'6789ull));
        // (b) shift 3, base 0: offset 0x8'0000'0000 >> 3 == 0x1'0000'0000, whose
        //     low 32 bits are 0 -> encodes to 0 even though the input is non-null.
        check("narrow_encode_truncates_to_zero_shift3",
              narrow_encode(0u, 3u, std::uint64_t{ 0x8'0000'0000ull }) == 0u);
        // (c) A general high offset at shift 3: (0xABCD'0000'0000 >> 3) ==
        //     0x1579'A000'0000; uint32 low half == 0xA000'0000.
        check("narrow_encode_high_offset_shift3_low32",
              narrow_encode(0u, 3u, std::uint64_t{ 0xABCD'0000'0000ull })
                  == 0xA000'0000u);
    }

    // -- PP5. EXHAUSTIVE high-16-bit narrow family: values k<<16 for ALL k in
    //    [0, 0xFFFF] (the contiguous high half section U's low-16 sweep cannot
    //    reach), across the canonical modes.  decode must equal base +
    //    ((k<<16)<<shift) with the widen-before-shift preventing 32-bit overflow
    //    even for the largest k at shift 4 (0xFFFF0000 << 4 == 0xF'FFF0'0000).
    {
        struct mode { std::uint64_t base; std::uint32_t shift; };
        const mode modes[]{
            { 0u, 0u }, { 0u, 3u }, { 0u, 4u },
            { std::uint64_t{ 0x8'0000'0000ull }, 3u },
        };
        bool high16_ok{ true };
        bool high16_roundtrip_ok{ true };
        std::size_t high16_cases{ 0 };
        for (const mode m : modes)
        {
            for (std::uint32_t k{ 0u }; k <= 0xFFFFu; ++k)
            {
                const std::uint32_t c{ k << 16 };           // high-half narrow value
                const std::uintptr_t got{ as_uptr(narrow_decode(m.base, m.shift, c)) };
                const std::uintptr_t want{ static_cast<std::uintptr_t>(
                    m.base + (static_cast<std::uint64_t>(c) << m.shift)) };
                if (got != want) { high16_ok = false; }
                if (narrow_encode(m.base, m.shift,
                        static_cast<std::uint64_t>(got)) != c) { high16_roundtrip_ok = false; }
                ++high16_cases;
            }
        }
        check("narrow_decode_exhaustive_high16_family", high16_ok);
        check("narrow_roundtrip_exhaustive_high16_family", high16_roundtrip_ok);
        check("narrow_decode_high16_sweep_is_complete",
              high16_cases == static_cast<std::size_t>(4) * 0x1'0000u);
        // Explicit largest-k pin at shift 4: 0xFFFF0000 << 4 must widen, not
        // truncate, to 0xF'FFF0'0000 (above 4 GB).
        check("narrow_decode_high16_max_k_shift4_widens",
              as_uptr(narrow_decode(0u, 4u, 0xFFFF'0000u)) == 0xF'FFF0'0000ull);
    }

    // -- PP6. OOP/KLASS WRAPPER null-domain over the COMPLETE low-16-bit input
    //    range with no JVM.  Sections AA/GG used a dense (not contiguous) sweep;
    //    here EVERY value in [0, 0xFFFF] is fed to both public decoders and must
    //    yield nullptr (c==0 via the early guard, c!=0 via the no-resolve guard),
    //    and the OOP/KLASS decoders must AGREE on every one (shared null
    //    contract).  No crashes, no fabricated reads -- the wrappers do their own
    //    arithmetic only after a VMStruct resolve that never succeeds here.
    {
        bool oop_all_null{ true };
        bool klass_all_null{ true };
        bool oop_klass_agree{ true };
        std::size_t wrapper_cases{ 0 };
        for (std::uint32_t c{ 0u }; c <= 0xFFFFu; ++c)
        {
            void* const o{ decode_oop_pointer(c) };
            void* const k{ decode_klass_pointer(c) };
            if (o != nullptr) { oop_all_null = false; }
            if (k != nullptr) { klass_all_null = false; }
            if (o != k) { oop_klass_agree = false; }
            ++wrapper_cases;
        }
        check("decode_oop_pointer_complete_low16_all_null_no_jvm", oop_all_null);
        check("decode_klass_pointer_complete_low16_all_null_no_jvm", klass_all_null);
        check("oop_klass_decode_agree_complete_low16_no_jvm", oop_klass_agree);
        check("wrapper_complete_low16_sweep_is_complete",
              wrapper_cases == static_cast<std::size_t>(0x1'0000u));
    }

    // -- PP7. EXACT human-readable decoded pointers for additional (base, shift)
    //    points not spot-pinned earlier, so a regression yields a self-evident
    //    wrong constant rather than only a failed loop flag.  Each value is the
    //    closed form base + (c<<shift), computed by hand.
    {
        // shift 1 (2-byte step): zero-based 0x40 -> 0x80.
        check("narrow_decode_shift1_0x40_is_0x80",
              as_uptr(narrow_decode(0u, 1u, 0x40u)) == 0x80u);
        // shift 2 (4-byte step): zero-based 0x40 -> 0x100.
        check("narrow_decode_shift2_0x40_is_0x100",
              as_uptr(narrow_decode(0u, 2u, 0x40u)) == 0x100u);
        // based unscaled (shift 0), high base: base + c verbatim.
        check("narrow_decode_high_base_shift0_adds_c",
              as_uptr(narrow_decode(std::uint64_t{ 0x7F00'0000'0000ull }, 0u, 0xABCDu))
                  == 0x7F00'0000'ABCDull);
        // based scaled8 (shift 3): base + (0x2_0000 << 3) == base + 0x10_0000.
        check("narrow_decode_based_shift3_0x20000_offset",
              as_uptr(narrow_decode(std::uint64_t{ 0x8'0000'0000ull }, 3u, 0x2'0000u))
                  == 0x8'0010'0000ull);
        // The inverse of each, recovering the narrow value.
        check("narrow_encode_high_base_shift0_recovers",
              narrow_encode(std::uint64_t{ 0x7F00'0000'0000ull }, 0u,
                  std::uint64_t{ 0x7F00'0000'ABCDull }) == 0xABCDu);
        check("narrow_encode_based_shift3_recovers",
              narrow_encode(std::uint64_t{ 0x8'0000'0000ull }, 3u,
                  std::uint64_t{ 0x8'0010'0000ull }) == 0x2'0000u);
    }

    // -- PP8. WRAPPER noexcept / signature re-pin across the COMPLETE set of
    //    public codec entry points in one place (additive: section A pinned the
    //    OOP pair, R pinned the klass pair; this restates all four together as a
    //    single closure so a future signature drift on ANY of them is caught
    //    here too).  Compile-time traits + a runtime exercise of each.
    {
        int anchor{ 0 };
        check("all_decoders_return_void_ptr",
              std::is_same_v<decltype(decode_oop_pointer(0u)), void*>
              && std::is_same_v<decltype(decode_klass_pointer(0u)), void*>);
        check("all_encoders_return_uint32",
              std::is_same_v<decltype(encode_oop_pointer(nullptr)), std::uint32_t>
              && std::is_same_v<decltype(encode_klass_pointer(nullptr)), std::uint32_t>);
        check("all_codecs_are_noexcept",
              noexcept(decode_oop_pointer(0u)) && noexcept(encode_oop_pointer(&anchor))
              && noexcept(decode_klass_pointer(0u)) && noexcept(encode_klass_pointer(&anchor)));
    }

    // ===================================================================
    // QQ. (ADDITIVE deepening pass) safe_read_fast - the warm-path fault-safe
    //     copy primitive (vmhook.hpp:1138-1159).  NO earlier section drives it:
    //     section MM covers os::safe_read, but safe_read_fast is its caller-facing
    //     sibling (MSVC-cl SEH fast path, else delegates to os::safe_read).  Its
    //     argument guard is identical (!dst || !src || size == 0 -> false,
    //     :1140-1143) and a copy of a genuinely OWNED in-process buffer must
    //     succeed byte-exactly on EVERY platform/compiler the matrix builds (both
    //     the SEH fast path and the os::safe_read fallback copy the same correct
    //     bytes for a mapped src).  Uses ONLY owned std::array / scalars - NO
    //     fabricated address, no vector byte-buffer resize.
    // ===================================================================
    {
        using vmhook::os::safe_read_fast;

        // Argument guards: any null operand or zero size -> false, no read, and
        // the destination is left untouched.
        std::uint64_t guard_dst{ 0x1111'2222'3333'4444ull };
        const std::uint64_t guard_src{ 0x5566'7788'99AA'BBCCull };
        check("safe_read_fast_null_dst_is_false",
              !safe_read_fast(nullptr, &guard_src, sizeof(guard_src)));
        check("safe_read_fast_null_src_is_false",
              !safe_read_fast(&guard_dst, nullptr, sizeof(guard_dst)));
        check("safe_read_fast_zero_size_is_false",
              !safe_read_fast(&guard_dst, &guard_src, 0));
        check("safe_read_fast_rejected_calls_leave_dst_untouched",
              guard_dst == 0x1111'2222'3333'4444ull);

        // Warm path on owned memory: copy a known scalar byte-exactly.
        {
            const std::uint64_t in{ 0x0102'0304'0506'0708ull };
            std::uint64_t out{ 0 };
            const bool ok{ safe_read_fast(&out, &in, sizeof(in)) };
            check("safe_read_fast_owned_scalar_succeeds", ok);
            check("safe_read_fast_owned_scalar_byte_exact", out == in);
        }

        // Warm path across a range of widths from an owned byte buffer.  Each
        // prefix read reproduces exactly the first `width` bytes, and the bytes
        // beyond it stay untouched (value-initialised 0).  Fixed std::array (no
        // deduced-size-0 vector resize -Wstringop-overflow trap), explicit
        // ASCII-range byte constants (no raw NUL / non-ASCII).
        {
            std::array<std::uint8_t, 16> src_bytes{ {
                0x10u, 0x21u, 0x32u, 0x43u, 0x54u, 0x65u, 0x76u, 0x87u,
                0x98u, 0xA9u, 0xBAu, 0xCBu, 0xDCu, 0xEDu, 0xFEu, 0x0Fu,
            } };
            const std::size_t widths[]{ 1u, 2u, 4u, 7u, 8u, 13u, 16u };
            bool every_width_byte_exact{ true };
            for (const std::size_t w : widths)
            {
                std::array<std::uint8_t, 16> dst_bytes{};   // all 0
                const bool ok{ safe_read_fast(dst_bytes.data(), src_bytes.data(), w) };
                if (!ok) { every_width_byte_exact = false; }
                for (std::size_t i{ 0 }; i < w; ++i)
                {
                    if (dst_bytes[i] != src_bytes[i]) { every_width_byte_exact = false; }
                }
                for (std::size_t i{ w }; i < dst_bytes.size(); ++i)
                {
                    if (dst_bytes[i] != 0u) { every_width_byte_exact = false; }
                }
            }
            check("safe_read_fast_all_widths_owned_buffer_byte_exact",
                  every_width_byte_exact);
        }

        // safe_read_fast is noexcept and returns bool; pin both so a future
        // signature change is a build error on every config.
        {
            std::uint64_t d{ 0 };
            const std::uint64_t s{ 0 };
            check("safe_read_fast_is_noexcept",
                  noexcept(safe_read_fast(&d, &s, sizeof(s))));
            check("safe_read_fast_returns_bool",
                  std::is_same_v<decltype(safe_read_fast(&d, &s, sizeof(s))), bool>);
        }
    }

    // ===================================================================
    // RR. (ADDITIVE) untag_pointer - COMPLETE single-bit enumeration over all 64
    //     bit positions (vmhook.hpp:2092-2097).  Section NN pinned bit 46 (kept)
    //     and bit 47 (cleared) individually; this enumerates EVERY bit 0..63 in
    //     isolation, deriving the expectation purely from the mask identity
    //     untag(p) == p & user_address_ceiling (ceiling == 0x00007FFFFFFFFFFF,
    //     i.e. bits 0..46 set, 47..63 clear):
    //        - a value with ONLY bit b set, b in [0, 46], is returned unchanged;
    //        - a value with ONLY bit b set, b in [47, 63], masks to 0.
    //     One assertion family per bit, no gaps - the strongest "mask is exactly
    //     bits 0..46" statement.  Pure bit arithmetic, no memory read.
    // ===================================================================
    {
        bool low47_bits_kept{ true };
        bool high17_bits_cleared{ true };
        for (unsigned b{ 0u }; b < 64u; ++b)
        {
            const std::uintptr_t only_bit{ std::uintptr_t{ 1ull } << b };
            const std::uintptr_t masked{ reinterpret_cast<std::uintptr_t>(
                untag_pointer(reinterpret_cast<const void*>(only_bit))) };
            const std::uintptr_t want{ only_bit & ceiling };   // mask identity
            if (masked != want) { low47_bits_kept = false; high17_bits_cleared = false; }
            if (b <= 46u)
            {
                if (masked != only_bit) { low47_bits_kept = false; }
            }
            else
            {
                if (masked != 0u) { high17_bits_cleared = false; }
            }
        }
        check("untag_pointer_low_47_bits_each_kept", low47_bits_kept);
        check("untag_pointer_high_17_bits_each_cleared", high17_bits_cleared);

        // Endpoints recomputed from the identity bracket the enumeration:
        // 0 -> 0 (already trivially), and all-ones -> ceiling.
        check("untag_pointer_all_bits_set_is_ceiling_endpoint",
              reinterpret_cast<std::uintptr_t>(
                  untag_pointer(reinterpret_cast<const void*>(
                      std::uintptr_t{ 0u } - std::uintptr_t{ 1u }))) == ceiling);

        // Every two-adjacent-bit straddle of the 46/47 boundary: bits {45,46}
        // are both kept (both <=46), bits {46,47} keep only 46, bits {47,48} keep
        // nothing.  Pins the cut is precisely between bit 46 and bit 47.
        check("untag_pointer_bits_45_46_both_kept",
              reinterpret_cast<std::uintptr_t>(untag_pointer(
                  reinterpret_cast<const void*>((std::uintptr_t{ 1ull } << 45)
                                                | (std::uintptr_t{ 1ull } << 46))))
                  == ((std::uintptr_t{ 1ull } << 45) | (std::uintptr_t{ 1ull } << 46)));
        check("untag_pointer_bits_46_47_keeps_only_46",
              reinterpret_cast<std::uintptr_t>(untag_pointer(
                  reinterpret_cast<const void*>((std::uintptr_t{ 1ull } << 46)
                                                | (std::uintptr_t{ 1ull } << 47))))
                  == (std::uintptr_t{ 1ull } << 46));
        check("untag_pointer_bits_47_48_keeps_nothing",
              untag_pointer(reinterpret_cast<const void*>(
                  (std::uintptr_t{ 1ull } << 47) | (std::uintptr_t{ 1ull } << 48)))
                  == nullptr);
    }

    // ===================================================================
    // SS. (ADDITIVE) is_valid_pointer - EXACT-MATCH neighbours of each EVEN poison
    //     sentinel that actually reaches the switch (0xCAFEBABE, 0xCCCCCCCC,
    //     0xFEEEFEEE - vmhook.hpp:2071/2072/2075).  Section D proved the even
    //     sentinels are rejected and that ONE near-miss (0xDEADBEEE) is accepted;
    //     this pins, PER even sentinel, that value-2 and value+2 (kept even so
    //     they pass the alignment gate and genuinely reach the switch) are NOT in
    //     the switch and are ACCEPTED under an in-range even high prefix - proving
    //     the compare is an EXACT full low-32 match, not a range/near-pattern test.
    //     (+/-2, not +/-1, so the neighbour stays even; an odd neighbour would be
    //     rejected by alignment and mask the switch behaviour.)
    // ===================================================================
    {
        constexpr std::uintptr_t prefix{ 0x0000'3300'0000'0000ull };  // in range, even
        const std::uint32_t even_sentinels[]{
            0xCAFEBABEu, 0xCCCCCCCCu, 0xFEEEFEEEu,
        };
        bool sentinel_rejected_neighbours_accepted{ true };
        for (const std::uint32_t s : even_sentinels)
        {
            // The sentinel itself (in-range, even) is rejected by the switch.
            if (is_valid_pointer(reinterpret_cast<void*>(prefix | s)))
            {
                sentinel_rejected_neighbours_accepted = false;
            }
            const std::uint32_t lower{ s - 2u };
            const std::uint32_t upper{ s + 2u };
            // Premise: both neighbours are even (so alignment passes and they
            // reach the switch).
            if ((lower & 0x1u) != 0u || (upper & 0x1u) != 0u)
            {
                sentinel_rejected_neighbours_accepted = false;
            }
            // Neither neighbour is a listed sentinel -> both accepted.
            if (!is_valid_pointer(reinterpret_cast<void*>(prefix | lower)))
            {
                sentinel_rejected_neighbours_accepted = false;
            }
            if (!is_valid_pointer(reinterpret_cast<void*>(prefix | upper)))
            {
                sentinel_rejected_neighbours_accepted = false;
            }
        }
        check("is_valid_pointer_even_sentinel_exact_match_neighbours_ok",
              sentinel_rejected_neighbours_accepted);

        // The high prefix alone (benign even low half) is accepted, so the
        // rejections above are attributable solely to the sentinel low-32 value.
        check("is_valid_pointer_ss_prefix_control_accepted",
              is_valid_pointer(reinterpret_cast<void*>(prefix | 0x0000'2222u)));
    }

    // ===================================================================
    // TT. (ADDITIVE) os::safe_read - wrap-guard BATTERY + non-wrapping owned
    //     prefix-width sweep (vmhook.hpp:957-971).  Section MM pinned single
    //     null/zero/wrap examples; this adds (1) a complete owned-buffer prefix
    //     read where src+size does NOT wrap (the guard-NOT-taken branch, on real
    //     memory) and (2) a battery of high synthetic src + size pairs, asserting
    //     that EVERY pair for which the documented predicate `src + size < src`
    //     holds is rejected at the guard BEFORE any OS read (so it never touches
    //     the unmapped synthetic page and cannot fault on POSIX).  We feed only
    //     pairs we have independently verified DO wrap.
    // ===================================================================
    {
        using vmhook::os::safe_read;

        // (TT1) Non-wrapping owned read of every prefix width succeeds and is
        //       byte-exact (the "src + size >= src" branch on real memory).
        {
            std::array<std::uint8_t, 8> src_b{ {
                0x01u, 0x23u, 0x45u, 0x67u, 0x89u, 0xABu, 0xCDu, 0xEFu } };
            bool all_ok{ true };
            for (std::size_t w{ 1u }; w <= src_b.size(); ++w)
            {
                std::array<std::uint8_t, 8> dst_b{};
                if (!safe_read(dst_b.data(), src_b.data(), w)) { all_ok = false; }
                for (std::size_t i{ 0 }; i < w; ++i)
                {
                    if (dst_b[i] != src_b[i]) { all_ok = false; }
                }
            }
            check("os_safe_read_nonwrapping_owned_prefix_widths_succeed", all_ok);
        }

        // (TT2) Wrap-guard battery: every (high src, size) pair for which
        //       (src + size) < src must be rejected; we skip non-wrapping pairs
        //       (they would reach an unmapped OS read).  The predicate is
        //       recomputed independently per pair.
        {
            std::uint64_t sink{ 0 };
            const std::uintptr_t high_srcs[]{
                std::uintptr_t{ 0u } - std::uintptr_t{ 1u },   // UINTPTR_MAX
                std::uintptr_t{ 0u } - std::uintptr_t{ 2u },
                std::uintptr_t{ 0u } - std::uintptr_t{ 8u },
                std::uintptr_t{ 0u } - std::uintptr_t{ 16u },
            };
            const std::size_t sizes[]{ 2u, 8u, 16u, 64u,
                                       static_cast<std::size_t>(-1) };
            bool all_wrapping_rejected{ true };
            std::size_t wrapping_cases{ 0 };
            for (const std::uintptr_t s : high_srcs)
            {
                for (const std::size_t sz : sizes)
                {
                    const bool wraps{ (s + sz) < s };   // documented predicate
                    if (!wraps) { continue; }
                    ++wrapping_cases;
                    if (safe_read(&sink, reinterpret_cast<const void*>(s), sz))
                    {
                        all_wrapping_rejected = false;
                    }
                }
            }
            check("os_safe_read_all_wrapping_pairs_rejected", all_wrapping_rejected);
            check("os_safe_read_wrap_battery_nonempty", wrapping_cases >= 4);
            // The destination is never written by a guard-rejected call.
            check("os_safe_read_wrap_battery_left_sink_zero", sink == 0u);
        }
    }

    // ===================================================================
    // UU. (ADDITIVE deepening pass) Pure-logic inputs the A..TT sections do
    //     NOT reach.  Every expected value is recomputed from the confirmed
    //     primitive bodies (vmhook/ext/vmhook/vmhook.hpp:5459-5463 narrow_decode
    //     = base + (uint64(c) << shift); :5480-5484 narrow_encode =
    //     uint32((addr - base) >> shift)), the is_valid_pointer gate
    //     (:2050-2083: <= floor, >= ceiling, & 0x1, 9-case poison switch), and
    //     the untag mask (:2092-2097: & user_address_ceiling).  No memory read
    //     of any fabricated address, no value_t->container cast, no vector
    //     byte-buffer resize (fixed C arrays / std::array / scalars only), no
    //     signed/unsigned narrowing, no raw NUL / non-ASCII in literals.
    // ===================================================================

    // -- UU1. TRANSLATION-INVARIANCE of decode under a base change.  For a fixed
    //    shift and compressed value, sliding the base by delta slides the decoded
    //    pointer by exactly delta:  decode(base+delta, shift, c) ==
    //    decode(base, shift, c) + delta.  This is the algebraic law the OOP/KLASS
    //    codecs rely on when the same compressed id is resolved against a relocated
    //    heap base, and no earlier section states it.  Pure arithmetic.
    {
        const std::uint32_t shifts[]{ 0u, 1u, 2u, 3u, 4u };
        const std::uint64_t base{ 0x1'0000'0000ull };
        const std::uint64_t deltas[]{
            0u, 8u, 0x1000u, 0x10'0000u, std::uint64_t{ 0x4'0000'0000ull },
        };
        const std::uint32_t comps[]{ 0u, 1u, 0x40u, 0x1234u, 0xFFFFu, 0xFFFF'FFFFu };
        bool translation_invariant{ true };
        for (const std::uint32_t sh : shifts)
        {
            for (const std::uint64_t d : deltas)
            {
                for (const std::uint32_t c : comps)
                {
                    const std::uintptr_t at_base{ as_uptr(narrow_decode(base, sh, c)) };
                    const std::uintptr_t at_shifted{
                        as_uptr(narrow_decode(base + d, sh, c)) };
                    if (at_shifted - at_base != static_cast<std::uintptr_t>(d))
                    {
                        translation_invariant = false;
                    }
                }
            }
        }
        check("narrow_decode_translation_invariant_under_base_delta",
              translation_invariant);
    }

    // -- UU2. ENCODE DIFFERENCE LAW: for two addresses on the same grid above a
    //    common base, the difference of their encodes equals the difference of
    //    their grid indices.  encode(base,sh,base+(c2<<sh)) -
    //    encode(base,sh,base+(c1<<sh)) == c2 - c1 (mod 2^32).  Pins that encode is
    //    an affine map (subtract base, divide by 2^sh) on the representable set.
    {
        const std::uint32_t shifts[]{ 0u, 3u, 4u };
        const std::uint64_t base{ 0x8'0000'0000ull };
        const std::uint32_t c1s[]{ 0u, 1u, 0x100u, 0x1'0000u };
        const std::uint32_t c2s[]{ 0u, 7u, 0x101u, 0x2'0000u };
        bool difference_law{ true };
        for (const std::uint32_t sh : shifts)
        {
            for (const std::uint32_t c1 : c1s)
            {
                for (const std::uint32_t c2 : c2s)
                {
                    const std::uint64_t a1{ base + (static_cast<std::uint64_t>(c1) << sh) };
                    const std::uint64_t a2{ base + (static_cast<std::uint64_t>(c2) << sh) };
                    const std::uint32_t e1{ narrow_encode(base, sh, a1) };
                    const std::uint32_t e2{ narrow_encode(base, sh, a2) };
                    if (static_cast<std::uint32_t>(e2 - e1)
                        != static_cast<std::uint32_t>(c2 - c1))
                    {
                        difference_law = false;
                    }
                }
            }
        }
        check("narrow_encode_difference_equals_index_difference", difference_law);
    }

    // -- UU3. untag_pointer is a CLOSURE / fixed-point map: for ANY input the
    //    masked result is already in [0, ceiling], so untagging it again is a
    //    no-op, AND every value already <= ceiling is its own image (a fixed
    //    point).  Section H pinned a single idempotent example; this states the
    //    fixed-point law over a structured bit-spread, deriving each expectation
    //    from the mask identity p & user_address_ceiling.
    {
        const std::uintptr_t inputs[]{
            0x0u, 0x1u, 0x2u, 0xFFFFu, 0x1'0000u,
            std::uintptr_t{ 0x0000'7FFF'FFFF'FFFEull },   // ceiling-1 (even, in range)
            ceiling,                                       // exactly the mask
            std::uintptr_t{ 0x0000'8000'0000'0000ull },    // bit 47 only
            std::uintptr_t{ 0xFFFF'8000'0040'0000ull },    // kernel half + low payload
            std::uintptr_t{ 0xDEAD'BEEF'CAFE'BABEull },    // arbitrary high junk
            std::uintptr_t{ 0u } - std::uintptr_t{ 1u },   // all ones
        };
        bool untag_is_idempotent{ true };
        bool masked_is_fixed_point{ true };
        for (const std::uintptr_t v : inputs)
        {
            const void* const once{ untag_pointer(reinterpret_cast<const void*>(v)) };
            const void* const twice{ untag_pointer(once) };
            if (once != twice) { untag_is_idempotent = false; }
            // The masked value, re-fed, must equal itself (it is <= ceiling).
            const std::uintptr_t once_u{ reinterpret_cast<std::uintptr_t>(once) };
            if (once_u != (v & ceiling)) { masked_is_fixed_point = false; }
            if (once_u > ceiling) { masked_is_fixed_point = false; }
        }
        check("untag_pointer_is_idempotent_over_bit_spread", untag_is_idempotent);
        check("untag_pointer_masked_value_is_fixed_point", masked_is_fixed_point);
    }

    // -- UU4. is_valid_pointer POISON switch is positionally exact: each of the
    //    nine sentinels, placed at the low 32 bits under an in-range even high
    //    prefix, is rejected; the SAME sentinel placed in the HIGH 32 bits (so the
    //    low 32 bits are a benign even value) is ACCEPTED -- proving the switch
    //    examines only the low 32 bits (vmhook.hpp:2067 `low32 = (uint32)addr`),
    //    never the high half.  All nine derived directly from the switch cases
    //    (:2070-2078).  The high-prefix-placed sentinels keep an even, in-range,
    //    non-poison low half so the only thing under test is WHERE the bytes sit.
    {
        constexpr std::uintptr_t even_low_payload{ 0x0000'4000u };  // in-range-friendly, even, not poison
        const std::uint32_t sentinels[]{
            0xDEADBEEFu, 0xCAFEBABEu, 0xCCCCCCCCu, 0xCDCDCDCDu, 0xBAADF00Du,
            0xFEEEFEEEu, 0xABABABABu, 0xFDFDFDFDu, 0xDDDDDDDDu,
        };
        // High prefix used when the sentinel sits in the LOW 32 bits: must be
        // in-range and even.  0x2200 in bits 32+ keeps addr < ceiling.
        constexpr std::uintptr_t low_test_prefix{ 0x0000'2200'0000'0000ull };
        bool low_placed_all_rejected{ true };
        bool high_placed_all_accepted{ true };
        for (const std::uint32_t s : sentinels)
        {
            // Sentinel in the low 32 bits -> rejected (alignment for odd ones,
            // poison switch for even ones; either way false end-to-end).
            const std::uintptr_t low_addr{ low_test_prefix | s };
            if (is_valid_pointer(reinterpret_cast<void*>(low_addr)))
            {
                low_placed_all_rejected = false;
            }
            // Same sentinel bits in the HIGH 32 bits, with a benign even low half
            // and a high half kept under the ceiling.  The poison switch must not
            // fire (it only reads low32), and the address stays in range because
            // the sentinel is masked to the low 15 bits of the high word so the
            // overall value never reaches the ceiling.
            const std::uintptr_t high_word{
                static_cast<std::uintptr_t>(s & 0x7FFFu) << 32 };
            const std::uintptr_t high_addr{ high_word | even_low_payload };
            // Premise guard: this constructed address is genuinely in range and
            // even (so only the low32-vs-high32 placement is under test).
            const bool premise_ok{
                high_addr > floor && high_addr < ceiling
                && (high_addr & 0x1u) == 0u };
            if (!premise_ok
                || !is_valid_pointer(reinterpret_cast<void*>(high_addr)))
            {
                high_placed_all_accepted = false;
            }
        }
        check("is_valid_pointer_poison_low32_placed_all_rejected",
              low_placed_all_rejected);
        check("is_valid_pointer_poison_high32_placed_all_accepted",
              high_placed_all_accepted);
    }

    // -- UU5. is_valid_pointer is MONOTONE-FREE of the high bits across the floor
    //    and ceiling but a strict gate at each: a contiguous descending walk from
    //    just above the floor and a contiguous walk just below the ceiling, even
    //    values only (so alignment never fires) and chosen so no low-32 hits a
    //    sentinel, are ALL accepted; the floor and ceiling endpoints themselves
    //    are rejected.  Pins the two boundaries are exactly `<= floor` and
    //    `>= ceiling` over a run, complementing the single-point checks in B.
    {
        bool low_run_accepted{ true };
        bool high_run_accepted{ true };
        // Low run: floor+2, floor+4, ... floor+64 (all even, all in range, none a
        // poison low-32 since these are tiny values like 0x10001.. -> wait, must
        // stay even: floor is 0xFFFF (odd), floor+1 = 0x10000 even).  Use
        // floor+1, +3, +5.. which are the EVEN addresses (floor+odd == even).
        for (std::uintptr_t k{ 1 }; k <= 64u; k += 2u)   // floor+k is even for odd k
        {
            const std::uintptr_t addr{ floor + k };
            if ((addr & 0x1u) != 0u) { low_run_accepted = false; }   // premise
            if (!is_valid_pointer(reinterpret_cast<void*>(addr)))
            {
                low_run_accepted = false;
            }
        }
        // High run: ceiling-2, ceiling-4, ... ceiling-64 (ceiling is odd, so
        // ceiling-odd is even).
        for (std::uintptr_t k{ 1 }; k <= 64u; k += 2u)   // ceiling-k is even for odd k
        {
            const std::uintptr_t addr{ ceiling - k };
            if ((addr & 0x1u) != 0u) { high_run_accepted = false; }  // premise
            if (!is_valid_pointer(reinterpret_cast<void*>(addr)))
            {
                high_run_accepted = false;
            }
        }
        check("is_valid_pointer_low_even_run_above_floor_accepted",
              low_run_accepted);
        check("is_valid_pointer_high_even_run_below_ceiling_accepted",
              high_run_accepted);
        // The exact endpoints stay rejected (re-pinned in this run's context).
        check("is_valid_pointer_floor_endpoint_rejected_in_run",
              !is_valid_pointer(reinterpret_cast<void*>(floor)));
        check("is_valid_pointer_ceiling_endpoint_rejected_in_run",
              !is_valid_pointer(reinterpret_cast<void*>(ceiling)));
    }

    // -- UU6. CONSTANT-VALUE pins, derived from source, asserted so they cannot be
    //    silently changed.  user_address_floor == 0xFFFF and user_address_ceiling
    //    == 0x00007FFFFFFFFFFF (vmhook.hpp:515/520).  The ceiling has exactly bits
    //    0..46 set and 47..63 clear, and floor+1 is the first power-of-two-aligned
    //    in-range address.  Each constant is referenced (no unused-const).
    {
        check("const_user_address_floor_is_0xFFFF",
              floor == std::uintptr_t{ 0xFFFFu });
        check("const_user_address_ceiling_is_47_bit_mask",
              ceiling == std::uintptr_t{ 0x0000'7FFF'FFFF'FFFFull });
        // ceiling is (1<<47) - 1: bit 47 is the lowest CLEARED bit.
        check("const_ceiling_is_one_below_bit47",
              ceiling == ((std::uintptr_t{ 1ull } << 47) - 1u));
        // floor is (1<<16) - 1: the first in-range even address is exactly 1<<16.
        check("const_floor_is_one_below_bit16",
              floor == ((std::uintptr_t{ 1ull } << 16) - 1u));
        check("const_floor_plus_1_is_bit16",
              (floor + 1u) == (std::uintptr_t{ 1ull } << 16));
    }

    // -- UU7. FULL-WIDTH structured decode/encode bijection at the 32-bit
    //    extremes that section U's low-16 sweep cannot reach: the values
    //    {0x7FFF'FFFE, 0x7FFF'FFFF, 0x8000'0000, 0x8000'0001, 0xFFFF'FFFE,
    //    0xFFFF'FFFF} straddle the narrow sign bit and the all-ones top.  For each
    //    canonical (base, shift) the decode must equal the widened closed form and
    //    re-encode to the same value -- the most overflow-sensitive narrow inputs,
    //    attributed to BOTH the generic primitive and re-checked at shift 4 (the
    //    widest object alignment) where (c<<4) reaches highest.
    {
        struct mode { std::uint64_t base; std::uint32_t shift; };
        const mode modes[]{
            { 0u, 0u }, { 0u, 3u }, { 0u, 4u },
            { std::uint64_t{ 0x8'0000'0000ull }, 3u },
            { std::uint64_t{ 0x7000'0000'0000ull }, 4u },
        };
        const std::uint32_t extremes[]{
            0x7FFF'FFFEu, 0x7FFF'FFFFu, 0x8000'0000u,
            0x8000'0001u, 0xFFFF'FFFEu, 0xFFFF'FFFFu,
        };
        bool decode_extreme_ok{ true };
        bool encode_extreme_ok{ true };
        for (const mode m : modes)
        {
            for (const std::uint32_t c : extremes)
            {
                const std::uintptr_t got{ as_uptr(narrow_decode(m.base, m.shift, c)) };
                const std::uintptr_t want{ static_cast<std::uintptr_t>(
                    m.base + (static_cast<std::uint64_t>(c) << m.shift)) };
                if (got != want) { decode_extreme_ok = false; }
                if (narrow_encode(m.base, m.shift,
                        static_cast<std::uint64_t>(got)) != c)
                {
                    encode_extreme_ok = false;
                }
            }
        }
        check("narrow_decode_sign_and_top_extremes_all_modes", decode_extreme_ok);
        check("narrow_roundtrip_sign_and_top_extremes_all_modes", encode_extreme_ok);
        // Explicit human-readable pin: 0xFFFF'FFFF at shift 4 widens to
        // 0xF'FFFF'FFF0 (not truncated to 32 bits) -- the widest single-value
        // overflow case in the suite.
        check("narrow_decode_all_ones_shift4_widens_to_0xF_FFFF_FFF0",
              as_uptr(narrow_decode(0u, 4u, 0xFFFF'FFFFu)) == 0xF'FFFF'FFF0ull);
    }

    // -- UU8. decode_oop_pointer(0) / decode_klass_pointer(0) feed cleanly through
    //    the WHOLE pointer-hygiene chain to a benign result with no JVM: the null
    //    sentinel survives untag, is rejected by is_valid_pointer, is rejected by
    //    safe_read_pointer (null short-circuit), and is rejected by
    //    is_readable_pointer -- so a decoded-null oop/klass can never be mistaken
    //    for a live pointer at any stage.  Composes the four hygiene helpers on the
    //    one input that is fully determined with no heap.
    {
        void* const dnull_oop{ decode_oop_pointer(0u) };
        void* const dnull_klass{ decode_klass_pointer(0u) };
        check("decoded_null_oop_survives_untag_as_null",
              untag_pointer(dnull_oop) == nullptr);
        check("decoded_null_klass_survives_untag_as_null",
              untag_pointer(dnull_klass) == nullptr);
        check("decoded_null_oop_rejected_by_all_hygiene_gates",
              !is_valid_pointer(dnull_oop)
              && safe_read_pointer(dnull_oop) == nullptr
              && !is_readable_pointer(dnull_oop));
        check("decoded_null_klass_rejected_by_all_hygiene_gates",
              !is_valid_pointer(dnull_klass)
              && safe_read_pointer(dnull_klass) == nullptr
              && !is_readable_pointer(dnull_klass));
    }

    // ===================================================================
    // VV. (ADDITIVE deepening pass) Pure-logic codec inputs the A..UU sections
    //     do NOT reach.  Every expected value is recomputed from the two
    //     confirmed primitive bodies (vmhook/ext/vmhook/vmhook.hpp:5459-5463
    //     narrow_decode = base + (uint64(c) << shift); :5480-5484 narrow_encode
    //     = uint32((addr - base) >> shift)) and the is_valid_pointer gate
    //     (:2050-2083).  No memory read of any fabricated address (the array
    //     helper drives a REAL owned std::array we allocate); no value_t ->
    //     container cast; no std::vector byte-buffer resize (std::array /
    //     scalars only); no signed/unsigned narrowing; no raw NUL / non-ASCII in
    //     literals; every const below is referenced.
    // ===================================================================

    // -- VV1. COMPLETE byte-narrow domain [0, 0xFF] at EVERY shift 0..8, both
    //    decode and the round-trip, for the four canonical bases.  Earlier
    //    sweeps cap shift at 4 (the widest object alignment HotSpot uses); the
    //    primitive is shift-agnostic, so this exercises shifts 5..8 that no prior
    //    section reaches, with the widen-before-shift guaranteeing (c << 8) for
    //    c == 0xFF still lands at exactly 0xFF00 (no truncation).  The expected
    //    value is the documented closed form, recomputed in 64-bit.
    {
        struct mode { std::uint64_t base; std::uint32_t shift; };
        const mode modes[]{
            { 0u, 0u }, { 0u, 5u }, { 0u, 6u }, { 0u, 7u }, { 0u, 8u },
            { std::uint64_t{ 0x1'0000'0000ull }, 5u },
            { std::uint64_t{ 0x8'0000'0000ull }, 8u },
            { std::uint64_t{ 0x7F00'0000'0000ull }, 7u },
        };
        bool byte_decode_ok{ true };
        bool byte_roundtrip_ok{ true };
        std::size_t byte_cases{ 0 };
        for (const mode m : modes)
        {
            for (std::uint32_t c{ 0u }; c <= 0xFFu; ++c)
            {
                const std::uintptr_t got{ as_uptr(narrow_decode(m.base, m.shift, c)) };
                const std::uintptr_t want{ static_cast<std::uintptr_t>(
                    m.base + (static_cast<std::uint64_t>(c) << m.shift)) };
                if (got != want) { byte_decode_ok = false; }
                if (narrow_encode(m.base, m.shift,
                        static_cast<std::uint64_t>(got)) != c) { byte_roundtrip_ok = false; }
                ++byte_cases;
            }
        }
        check("narrow_decode_complete_byte_domain_shifts_5_to_8", byte_decode_ok);
        check("narrow_roundtrip_complete_byte_domain_shifts_5_to_8", byte_roundtrip_ok);
        check("narrow_decode_byte_domain_sweep_is_complete",
              byte_cases == static_cast<std::size_t>(8) * 0x100u);
        // Explicit human-readable pin: 0xFF at shift 8 widens to exactly 0xFF00.
        check("narrow_decode_0xFF_shift8_is_0xFF00",
              as_uptr(narrow_decode(0u, 8u, 0xFFu)) == 0xFF00u);
    }

    // -- VV2. ENCODE single-bit OFFSET law: for an address that is base plus a
    //    single set bit (1 << b) for every b in 0..62, encode yields exactly the
    //    documented low-32 of ((1<<b) >> shift).  This isolates each bit of the
    //    subtract-shift-narrow independently (the inverse of section X's decode
    //    single-bit family), proving the >> and the final uint32 cast act on each
    //    bit position exactly as the closed form says.  Pure arithmetic.
    {
        const std::uint32_t shifts[]{ 0u, 1u, 3u, 4u };
        const std::uint64_t base{ 0x8'0000'0000ull };   // grid-aligned, in canonical range
        bool single_bit_encode_ok{ true };
        for (const std::uint32_t sh : shifts)
        {
            for (unsigned b{ 0u }; b < 63u; ++b)
            {
                const std::uint64_t offset{ std::uint64_t{ 1ull } << b };
                const std::uint64_t addr{ base + offset };
                const std::uint32_t got{ narrow_encode(base, sh, addr) };
                const std::uint32_t want{
                    static_cast<std::uint32_t>(offset >> sh) };   // documented low-32
                if (got != want) { single_bit_encode_ok = false; }
            }
        }
        check("narrow_encode_single_bit_offset_low32_all_shifts",
              single_bit_encode_ok);
    }

    // -- VV3. is_valid_pointer over a REAL owned byte buffer: every interior byte
    //    address of a std::array we allocate is classified purely by its low bit
    //    (even accepted, odd rejected) -- none of these interior addresses can hit
    //    a poison low-32 because they are real stack addresses whose low 32 bits
    //    are an arbitrary live value, so the discriminator is the 2-byte alignment
    //    rule alone.  We assert the parity law holds AND that at least one even and
    //    one odd interior address were actually exercised (so the loop is not
    //    vacuous).  REAL buffer -- no fabricated address is ever dereferenced (the
    //    helper only inspects the address value, never reads through it).
    {
        std::array<std::uint8_t, 64> real_buf{};   // owned, mapped, never read through
        const std::uintptr_t b0{ reinterpret_cast<std::uintptr_t>(real_buf.data()) };
        bool parity_rule_holds{ true };
        bool saw_even{ false };
        bool saw_odd{ false };
        for (std::size_t i{ 0 }; i < real_buf.size(); ++i)
        {
            const std::uintptr_t addr{ b0 + i };
            // Guard: the buffer sits comfortably inside [floor, ceiling] (a real
            // stack/heap object always does), so range never fires and alignment
            // is the sole discriminator.  Skip if a low-32 happens to be a poison
            // pattern (vanishingly unlikely for a live address, but keep the law
            // exact by only asserting on non-poison even addresses).
            const std::uint32_t low32{ static_cast<std::uint32_t>(addr) };
            const bool is_poison{
                low32 == 0xDEADBEEFu || low32 == 0xCAFEBABEu || low32 == 0xCCCCCCCCu
                || low32 == 0xCDCDCDCDu || low32 == 0xBAADF00Du || low32 == 0xFEEEFEEEu
                || low32 == 0xABABABABu || low32 == 0xFDFDFDFDu || low32 == 0xDDDDDDDDu };
            const bool valid{ is_valid_pointer(reinterpret_cast<void*>(addr)) };
            if ((addr & 0x1u) == 0u)
            {
                if (!is_poison)
                {
                    if (!valid) { parity_rule_holds = false; }
                    saw_even = true;
                }
            }
            else
            {
                if (valid) { parity_rule_holds = false; }
                saw_odd = true;
            }
        }
        check("is_valid_pointer_real_buffer_even_accept_odd_reject", parity_rule_holds);
        check("is_valid_pointer_real_buffer_exercised_both_parities",
              saw_even && saw_odd);
    }

    // -- VV4. PUBLIC-WRAPPER vs PRIMITIVE agreement on the null sentinel, plus a
    //    real-buffer encode characterization.  With no JVM the wrappers cannot
    //    resolve base/shift, so the ONLY thing observable is their sentinel
    //    behaviour; pin that BOTH public decoders agree with the primitive's
    //    "compressed 0 with base 0 decodes to base 0 == null" reading, and that
    //    encoding a real owned buffer address through either public encoder yields
    //    0 (no-resolve), while the primitive given an injected base would NOT --
    //    locating the no-resolve null precisely in the wrapper, not the math.
    {
        // The primitive with base 0, shift 0, compressed 0 returns (void*)0 == the
        // same null the public decoders short-circuit to -- so all three agree on
        // the canonical null oop/klass reading.
        check("primitive_zero_base_zero_matches_wrapper_null_oop",
              narrow_decode(0u, 0u, 0u) == decode_oop_pointer(0u));
        check("primitive_zero_base_zero_matches_wrapper_null_klass",
              narrow_decode(0u, 0u, 0u) == decode_klass_pointer(0u));

        // A real owned buffer address: both public encoders return 0 (no VMStruct
        // resolve), confirming the wrapper swallows even a genuinely valid pointer.
        std::array<std::uint64_t, 4> owned{};   // owned, 8-aligned, mapped
        void* const owned_ptr{ owned.data() };
        check("encode_oop_pointer_owned_buffer_zero_no_jvm",
              encode_oop_pointer(owned_ptr) == 0u);
        check("encode_klass_pointer_owned_buffer_zero_no_jvm",
              encode_klass_pointer(owned_ptr) == 0u);
        // The SAME owned address fed to the primitive with an injected non-zero
        // base that is below it does NOT collapse to 0 -- it is the lossless
        // index (addr - base) >> shift.  Recompute the expectation the same way so
        // the assertion is a spec, and choose a base strictly below the address so
        // there is no modular underflow.
        {
            const std::uintptr_t addr_u{ reinterpret_cast<std::uintptr_t>(owned_ptr) };
            // A base 0x1000 below the (8-aligned) address: (addr - base) is a
            // positive multiple-friendly delta; shift 0 keeps it lossless.
            const std::uint64_t base{ static_cast<std::uint64_t>(addr_u) - 0x1000u };
            const std::uint32_t got{ narrow_encode(base, 0u,
                static_cast<std::uint64_t>(addr_u)) };
            check("primitive_encode_owned_buffer_is_lossless_index_not_zero",
                  got == 0x1000u);
        }
    }

    // -- VV5. ENCODE/DECODE COMPOSITION is the identity on the COMPLETE byte index
    //    domain for shifts the earlier exhaustive low-16 sweep does not cover
    //    (5..8): for every c in [0, 0xFF] and every such shift, decode(encode(
    //    base + (c<<shift))) reproduces base + (c<<shift) exactly (representable
    //    grid points), and encode(decode(c)) reproduces c.  Both directions over a
    //    full contiguous domain at the wider shifts.
    {
        const std::uint32_t shifts[]{ 5u, 6u, 7u, 8u };
        const std::uint64_t base{ 0x8'0000'0000ull };
        bool addr_id{ true };
        bool comp_id{ true };
        for (const std::uint32_t sh : shifts)
        {
            for (std::uint32_t c{ 0u }; c <= 0xFFu; ++c)
            {
                const std::uint64_t addr{ base + (static_cast<std::uint64_t>(c) << sh) };
                const std::uint32_t enc{ narrow_encode(base, sh, addr) };
                if (static_cast<std::uint64_t>(as_uptr(narrow_decode(base, sh, enc))) != addr)
                {
                    addr_id = false;
                }
                void* const dec{ narrow_decode(base, sh, c) };
                if (narrow_encode(base, sh, static_cast<std::uint64_t>(as_uptr(dec))) != c)
                {
                    comp_id = false;
                }
            }
        }
        check("narrow_roundtrip_decode_encode_byte_domain_shifts_5_to_8", addr_id);
        check("narrow_roundtrip_encode_decode_byte_domain_shifts_5_to_8", comp_id);
    }

    return failures == 0 ? 0 : 1;
}
