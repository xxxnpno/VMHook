// Wave-31 deepening: return_value::frame() RAW-accessor characterization,
// no-JVM. Complements test_return_value.cpp (sections K/K2/K3) with NOVEL
// angles the existing file does not cover:
//
//   1. Multi-sentinel echo sweep — frame() is a pure echo of the ctor
//      argument across a wide pattern bank (zero, all-ones, alignment-edge
//      patterns, mid-range, INT_MIN/MAX-shaped, alternating bit patterns).
//   2. Default ctor (slot-only, frame defaulted) — frame() is nullptr AND
//      noexcept and idempotent. (Different ctor overload from K which uses
//      explicit nullptr.)
//   3. Const-callable through const-ref. frame() works on a `const return_value&`
//      without const-stripping.
//   4. Storage size sanity — return_value carries at least the two raw
//      pointers it is documented to (static_assert size lower bound).
//   5. Run-time sentinel preserved across const-binding (no const-stripping
//      mutation, no slot side-effects).
//
// None of these assertion names overlap with test_return_value.cpp.

#include <vmhook/vmhook.hpp>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <type_traits>

namespace
{
    int g_failures{ 0 };
    int g_passes{ 0 };

    void check(const char* name, bool ok)
    {
        if (ok) { ++g_passes; std::printf("[ OK ] %s\n", name); }
        else    { ++g_failures; std::printf("[FAIL] %s\n", name); }
    }
} // namespace

int main()
{
    using rv_t = vmhook::return_value;

    // -------------------------------------------------------------------
    // 1) Multi-sentinel echo sweep — frame() never inspects the pointer,
    //    so every bit pattern must round-trip byte-for-byte.
    // -------------------------------------------------------------------
    {
        const std::array<std::uintptr_t, 10> patterns{ {
            static_cast<std::uintptr_t>(0x0000000000000000ull),
            static_cast<std::uintptr_t>(0x0000000000000008ull), // 8-byte aligned low
            static_cast<std::uintptr_t>(0x0000000000001000ull), // page-aligned low
            static_cast<std::uintptr_t>(0x00007FFFFFFFFFFFull), // sysv user-space top
            static_cast<std::uintptr_t>(0x0000800000000000ull), // canonical break
            static_cast<std::uintptr_t>(0xAAAAAAAAAAAAAAA8ull), // alternating 1010
            static_cast<std::uintptr_t>(0x5555555555555550ull), // alternating 0101
            static_cast<std::uintptr_t>(0xCAFEBABEDEADBEE8ull),
            static_cast<std::uintptr_t>(0xFFFFFFFF80000008ull),
            static_cast<std::uintptr_t>(0xFFFFFFFFFFFFFFF8ull), // all-ones aligned
        } };

        vmhook::hotspot::return_slot slot{};
        bool all_echoed{ true };
        bool slot_untouched{ true };
        for (const std::uintptr_t p : patterns)
        {
            auto* const f{ reinterpret_cast<vmhook::hotspot::frame*>(p) };
            rv_t rv{ &slot, f };
            if (rv.frame() != f) { all_echoed = false; }
            // Twice must agree (no mutation, no caching of a transformed value).
            if (rv.frame() != rv.frame()) { all_echoed = false; }
            if (slot.cancel != false || slot.retval != 0) { slot_untouched = false; }
        }
        check("frame_multi_sentinel_echo_sweep_all_match", all_echoed);
        check("frame_multi_sentinel_sweep_slot_untouched", slot_untouched);
    }

    // -------------------------------------------------------------------
    // 2) Default-frame ctor overload (slot only). frame() is null,
    //    noexcept, idempotent — DIFFERENT path from explicit nullptr.
    // -------------------------------------------------------------------
    {
        vmhook::hotspot::return_slot slot{};
        rv_t rv{ &slot }; // frame defaulted

        check("frame_default_overload_is_null",       rv.frame() == nullptr);
        check("frame_default_overload_twice_is_null", rv.frame() == nullptr && rv.frame() == rv.frame());
        check("frame_default_overload_slot_clean",
              slot.cancel == false && slot.retval == 0);

        static_assert(noexcept(rv.frame()),
                      "frame() must be noexcept on the default-frame ctor path");
        // Confirm the default-frame ctor itself is noexcept (it is the path
        // the trampoline uses when no frame is captured).
        static_assert(noexcept(rv_t{ static_cast<vmhook::hotspot::return_slot*>(nullptr) }),
                      "return_value(slot) ctor must be noexcept");
        static_assert(noexcept(rv_t{ static_cast<vmhook::hotspot::return_slot*>(nullptr),
                                     static_cast<vmhook::hotspot::frame*>(nullptr) }),
                      "return_value(slot, frame) ctor must be noexcept");
    }

    // -------------------------------------------------------------------
    // 3) Const-callable through const-ref. The accessor must bind to a
    //    `const rv_t&` (the trampoline passes the return_value into user
    //    detours typically by reference; frame() must be const-qualified).
    // -------------------------------------------------------------------
    {
        vmhook::hotspot::return_slot slot{};
        auto* const sentinel{ reinterpret_cast<vmhook::hotspot::frame*>(
            static_cast<std::uintptr_t>(0x1234567890ABCDE8ull)) };
        const rv_t rv{ &slot, sentinel };

        const rv_t& cref{ rv };
        check("frame_const_ref_returns_sentinel", cref.frame() == sentinel);
        check("frame_const_ref_stable",           cref.frame() == cref.frame());
        check("frame_const_ref_no_slot_mutation",
              slot.cancel == false && slot.retval == 0);

        // Static contract: const overload exists and returns frame*.
        static_assert(
            std::is_same<decltype(std::declval<const rv_t&>().frame()),
                         vmhook::hotspot::frame*>::value,
            "const rv_t::frame() must return hotspot::frame*");
    }

    // -------------------------------------------------------------------
    // 4) Storage-size sanity — return_value must be big enough to hold the
    //    documented payload (return_slot* + frame*). On a 64-bit target that
    //    is at least 16 bytes; on hypothetical 32-bit it would be 8. We use
    //    a portable lower bound: 2 * sizeof(void*).
    // -------------------------------------------------------------------
    {
        static_assert(sizeof(rv_t) >= 2 * sizeof(void*),
                      "return_value must carry at least the slot and frame pointers");
        // The two ctor-stash pointers occupy the documented amount of space;
        // anything bigger is the implementation's prerogative but a regression
        // shrinking it below 2 ptrs would silently lose state.
        check("return_value_size_at_least_two_pointers",
              sizeof(rv_t) >= 2u * sizeof(void*));
    }

    // -------------------------------------------------------------------
    // 5) Run-time: a NON-aligned sentinel (off-by-1 etc.) — the accessor
    //    must still echo it bit-perfectly because it never dereferences.
    //    This pins "no internal alignment scrubbing".
    // -------------------------------------------------------------------
    {
        vmhook::hotspot::return_slot slot{};
        const std::array<std::uintptr_t, 4> unaligned{ {
            0x0000000000000001ull,
            0x0000000000000003ull,
            0x0000000000000007ull,
            0xDEADBEEFCAFEBABDull, // odd
        } };
        bool all_echoed_unaligned{ true };
        for (const std::uintptr_t p : unaligned)
        {
            auto* const f{ reinterpret_cast<vmhook::hotspot::frame*>(p) };
            rv_t rv{ &slot, f };
            if (rv.frame() != f) { all_echoed_unaligned = false; }
        }
        check("frame_unaligned_sentinel_echoes_bit_perfect", all_echoed_unaligned);
        check("frame_unaligned_sweep_slot_clean",
              slot.cancel == false && slot.retval == 0);
    }

    std::printf("\n[summary] passed=%d failed=%d\n", g_passes, g_failures);
    return g_failures == 0 ? 0 : 1;
}
