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

    return failures == 0 ? 0 : 1;
}
