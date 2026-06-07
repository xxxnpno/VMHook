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
// of truth: vmhook/ext/vmhook/vmhook.hpp:1768-1805 (is_valid_pointer),
// :4226-4290 (decode_oop_pointer), :4298-4361 (encode_oop_pointer),
// :505/:510 (os::user_address_ceiling / user_address_floor).
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
    //    These guards (vmhook.hpp:4229 and :4301) run BEFORE any
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

    // encode_oop_pointer(nullptr) -> 0.  Inverse guard (vmhook.hpp:4301):
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
    // null and decode_oop_pointer returns nullptr (vmhook.hpp:4280-4283)
    // WITHOUT crashing.  This documents the no-JVM behaviour; under a live
    // JVM this same input would decode to a real heap address instead.
    check("decode_oop_pointer_nonzero_no_jvm_is_null",
          decode_oop_pointer(0x0000'0001u) == nullptr);
    check("decode_oop_pointer_max_no_jvm_is_null",
          decode_oop_pointer(0xFFFF'FFFFu) == nullptr);

    // No-JVM fall-through for the encoder: a non-null pointer with no
    // resolvable VMStructs returns 0 (vmhook.hpp:4348-4351) and does not
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

    // Both codec entry points are declared noexcept (vmhook.hpp:4226/:4298);
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
    //    (vmhook.hpp:1771-1804.)
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
    //    The switch (vmhook.hpp:1789-1803) rejects any pointer whose low 32
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

    // is_valid_pointer is declared noexcept (vmhook.hpp:1768); pin it.
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
    //    arithmetic primitives (vmhook.hpp:4446-4451, :4467-4472).  Unlike
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
    //    (vmhook.hpp:1813-1818).  Pure bit-AND, fully checkable with no JVM.
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

    // All nine documented poison low-32 patterns (vmhook.hpp:1791-1799) are
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
    //    (vmhook.hpp:4446-4451).  This is the workhorse both decode_oop_pointer
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
    //    The widen-before-shift (uint64 cast in narrow_decode, vmhook.hpp:4450)
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
    //    (vmhook.hpp:4467-4472).  Driven over addresses that are exact, in-range
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
    //    pointer codec (vmhook.hpp:4585 / :4638).  Structurally identical to the
    //    OOP codec but resolves CompressedKlassPointers::_narrow_klass.{_base,
    //    _shift}.  With no JVM the VMStruct lookup fails, so only the null
    //    contract + no-JVM fall-through are determinable — exactly mirroring the
    //    OOP coverage in section A, which previously had ZERO klass-codec
    //    counterpart.  Source of truth: the early-return guards at :4588 / :4641
    //    (compressed/decoded == 0) and the missing-entry guards at :4613 / :4663.
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
    // Both klass codec entry points are noexcept (vmhook.hpp:4585 / :4638).
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
    //    invariant the header documents (vmhook.hpp:4441-4444 / :4461-4465).
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

    return failures == 0 ? 0 : 1;
}
