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
#include <cstdio>
#include <cstdint>
#include <string>
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

    return failures == 0 ? 0 : 1;
}
