// Standalone (no-JVM) EXHAUSTIVE unit test for the compressed-OOP decode
// feature: hotspot::decode_oop_pointer / encode_oop_pointer — the narrow-OOP
// codec that turns HotSpot's 32-bit compressed object reference into a real
// 64-bit heap pointer and back.  This is the bottom of every reference-decoding
// path in the library (typed hook args, read_java_string, object-type field
// reads, and every collection walker funnel through decode_oop_pointer), so a
// defect here silently corrupts every higher-level feature.
//
// ───────────────────────────────────────────────────────────────────────────
// WHY A DEDICATED compressed_oops FILE (vs. the OOP-and-pointers / klass files)
// ───────────────────────────────────────────────────────────────────────────
// The suite already has:
//   - test_decode_oop_and_pointers.cpp : owns the OOP null contract, the SHARED
//        narrow_decode/narrow_encode arithmetic primitive, untag_pointer, and
//        the is_valid_pointer boundary battery.
//   - test_compressed_klass_decode.cpp : owns the SEPARATE Klass codec, the
//        resolve_struct_entry walker, and the klass-vs-oop distinction.
//
// What was MISSING — and is this file's charter — is exhaustive coverage of the
// compressed-OOP decode feature PROPER as exercised through its PUBLIC entry
// points (decode_oop_pointer / encode_oop_pointer), pinned to the OOP-SPECIFIC
// machinery: the CompressedOops-typed VMStruct candidate arrays (the three
// JDK-version tiers), the OOP encoder's below-base guard, and the OOP decode /
// encode arithmetic re-pinned through the primitive over the (base, shift)
// regimes HotSpot programs for the HEAP (not the class space).  Every section
// here is framed around the OOP codec and recomputes its expectation from the
// documented closed form, never guessing.
//
// ───────────────────────────────────────────────────────────────────────────
// WHAT IS / ISN'T DETERMINABLE WITH NO JVM  (honest scope)
// ───────────────────────────────────────────────────────────────────────────
// This executable runs with NO HotSpot JVM in-process, so gHotSpotVMStructs is
// never resolvable (get_vm_structs() returns nullptr).  That makes exactly two
// layers of the PUBLIC OOP codec fully deterministic here:
//
//   1. The codec WRAPPER contract.  decode_oop_pointer's `if (!compressed)
//      return nullptr;` and encode_oop_pointer's `if (!decoded) return 0;`
//      early returns fire BEFORE any VMStruct lookup, and when the lookup fails
//      (no JVM) both wrappers return their null/zero sentinel instead of
//      crashing or dereferencing a bogus VMStruct address.  These are pinned
//      directly on the public OOP entry points, over the WHOLE 32-bit input
//      domain (dense sweep), not just 0 / 1 / max.
//
//   2. The OOP-codec ARITHMETIC.  decode_oop_pointer's body, once base/shift
//      are resolved, is literally `return narrow_decode(narrow_oop_base,
//      narrow_oop_shift, compressed);` and encode_oop_pointer's is `return
//      narrow_encode(narrow_oop_base, narrow_oop_shift, decoded_address);`
//      guarded by `decoded_address < narrow_oop_base -> return 0`.  Because
//      narrow_decode/narrow_encode take base/shift EXPLICITLY, we reproduce the
//      OOP codec's exact math by feeding the primitive the HEAP-style (base,
//      shift) pairs HotSpot actually programs for compressed oops (zero-based /
//      based, shift 0 / 3, plus completeness shifts).  This is where "every
//      possible input" lives: a dense 32-bit narrow sweep across every (base,
//      shift) mode, every value recomputed from the documented formula.
//
// Anything needing a LIVE oop / a real heap base / a running JVM — an actual
// String.value oop round-trip, a non-zero decode<->encode identity across the
// JVM's real narrow_oop_base, the encoder below-base guard FIRING on a real
// non-zero base, the consumer <=0xFFFFFFFF heuristic not double-decoding a
// small zero-based address — is OUT OF SCOPE here and is covered by the JVM
// integration modules.  Each such gap is called out inline with an [INFO] line.
//
// Source of truth (verified against vmhook/ext/vmhook/vmhook.hpp on 2026-06-17):
//   decode_oop_pointer            :5374-5412 (null guard :5377, no-resolve :5402,
//                                             base reads :5407-5408, decode :5411)
//   encode_oop_pointer            :5420-5459 (null guard :5423, no-resolve :5445,
//                                             below-base guard :5453, encode :5458)
//   OOP base candidates           :5386-5390 { CompressedOops,_narrow_oop._base /
//                                             CompressedOops,_base / Universe,
//                                             _narrow_oop._base }
//   OOP shift candidates          :5391-5395 (same tiers, _shift)
//   narrow_decode                 :5329-5334  base + (uint64(compressed) << shift)
//   narrow_encode                 :5350-5355  uint32((addr - base) >> shift)
//   resolve_struct_entry          :5301-5315  first-match-wins over candidates
//   iterate_struct_entries        :1990-2009  null get_vm_structs -> nullptr off-JVM
//   is_valid_pointer              :2047-2084  range + bit-0 + poison gate
//   untag_pointer                 :2092-2097  & user_address_ceiling
//   user_address_floor / ceiling  :520 / 515
#include <vmhook/vmhook.hpp>
#include <cstddef>    // std::size_t
#include <cstdio>
#include <cstdint>
#include <cstring>    // std::strcmp (candidate (type, field) name pins)
#include <iterator>   // std::size (candidate-array counts)
#include <type_traits>
#include <vector>

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

// Independent re-implementation of the documented OOP decode formula
//   real_address = narrow_oop_base + (uint64(compressed) << narrow_oop_shift)
// used ONLY to compute expected values; NEVER calls into the library.
static auto expect_decode(std::uint64_t base, std::uint32_t shift, std::uint32_t c)
    -> std::uintptr_t
{
    return static_cast<std::uintptr_t>(
        base + (static_cast<std::uint64_t>(c) << shift));
}
// Independent re-implementation of the documented OOP encode formula
//   compressed = uint32((decoded_address - narrow_oop_base) >> narrow_oop_shift)
static auto expect_encode(std::uint64_t base, std::uint32_t shift, std::uint64_t addr)
    -> std::uint32_t
{
    return static_cast<std::uint32_t>((addr - base) >> shift);
}
static auto as_uptr(void* p) -> std::uintptr_t
{
    return reinterpret_cast<std::uintptr_t>(p);
}

// The HEAP-style (base, shift) encoding modes HotSpot programs for compressed
// OOPs.  Bases are SYNTHETIC stand-ins (no real heap exists off-JVM):
//   - base 0, shift 0 : 32-bit unscaled zero-based heap (<4 GB starting at 0).
//                       Decode is a plain widening cast; this is the JDK8 /
//                       -XX:-UseCompressedOops-off / tiny-heap regime where the
//                       encoder's below-base guard (base 0) can never fire.
//   - base 0, shift 3 : zero-based, 8-byte-aligned-oop heap (<=32 GB).
//   - base !=0, shift 0 : "based" unscaled heap.
//   - base !=0, shift 3 : "based" + scaled, the >4 GB classic.
// Extra shifts 1/2/4 prove the codec scales by exactly 2^shift for ANY shift,
// not just the two HotSpot picks.
struct oop_mode { std::uint64_t base; std::uint32_t shift; const char* tag; };
static const oop_mode kModes[]{
    { 0u,                                    0u, "zerobased_unscaled" }, // JDK8 / <4GB @0
    { 0u,                                    3u, "zerobased_x8"       }, // <=32 GB zero-based
    { std::uint64_t{ 0x0000'0001'0000'0000ull }, 0u, "based_4G_unscaled" },
    { std::uint64_t{ 0x0000'0008'0000'0000ull }, 3u, "based_32G_x8"      }, // 32 GB-style
    { std::uint64_t{ 0x0000'7F00'0000'0000ull }, 0u, "based_high_s0"     }, // high heap base
    { std::uint64_t{ 0x0000'7F00'0000'0000ull }, 3u, "based_high_s3"     },
    { std::uint64_t{ 0x0000'0001'2345'6000ull }, 3u, "based_nonround_s3" }, // non-round base
    { 0u,                                    1u, "zerobased_s1"       }, // completeness shifts
    { 0u,                                    2u, "zerobased_s2"       },
    { 0u,                                    4u, "zerobased_s4"       },
};

// A dense 32-bit narrow-OOP sweep covering every structurally interesting
// class: 0, a small contiguous run, every power of two and its +/-1 neighbours
// across all 32 bit positions, and the 32-bit extremes.  Enumerating all 2^32
// is infeasible; this hits every boundary of the decode formula.
static auto build_dense_narrows() -> std::vector<std::uint32_t>
{
    std::vector<std::uint32_t> v;
    v.push_back(0u);
    for (std::uint32_t k{ 1u }; k <= 16u; ++k) { v.push_back(k); }
    for (unsigned bit{ 0u }; bit < 32u; ++bit)
    {
        const std::uint32_t pow2{ static_cast<std::uint32_t>(1u) << bit };
        v.push_back(pow2);
        v.push_back(pow2 - 1u);
        v.push_back(pow2 + 1u);   // wraps to 0x80000001 at bit 31, intentional
    }
    v.push_back(0x7FFF'FFFFu);
    v.push_back(0x8000'0000u);
    v.push_back(0x8000'0001u);
    v.push_back(0xFFFF'FFFEu);
    v.push_back(0xFFFF'FFFFu);
    return v;
}

int main()
{
    using vmhook::hotspot::decode_oop_pointer;
    using vmhook::hotspot::encode_oop_pointer;
    using vmhook::hotspot::narrow_decode;
    using vmhook::hotspot::narrow_encode;
    using vmhook::hotspot::is_valid_pointer;
    using vmhook::hotspot::untag_pointer;

    // =====================================================================
    // 0. COMPILE-TIME signature / noexcept / return-type pins (static_assert).
    //    These only inspect types, so they are constant expressions and a
    //    regression fails the BUILD — the strongest possible pin.  The
    //    arithmetic checks below stay runtime because narrow_decode uses
    //    reinterpret_cast (not a constant expression).
    // =====================================================================
    static_assert(std::is_same_v<decltype(decode_oop_pointer(0u)), void*>,
                  "decode_oop_pointer must return void*");
    static_assert(std::is_same_v<decltype(encode_oop_pointer(nullptr)), std::uint32_t>,
                  "encode_oop_pointer must return std::uint32_t");
    static_assert(noexcept(decode_oop_pointer(0u)),
                  "decode_oop_pointer must be noexcept");
    static_assert(noexcept(encode_oop_pointer(nullptr)),
                  "encode_oop_pointer must be noexcept");
    static_assert(std::is_same_v<decltype(narrow_decode(0u, 0u, 0u)), void*>,
                  "narrow_decode must return void*");
    static_assert(std::is_same_v<decltype(narrow_encode(0u, 0u, 0u)), std::uint32_t>,
                  "narrow_encode must return std::uint32_t");
    static_assert(noexcept(narrow_decode(0u, 0u, 0u)) && noexcept(narrow_encode(0u, 0u, 0u)),
                  "narrow primitives must be noexcept");
    // The public entry point takes a uint32 and a void* respectively — pin the
    // exact parameter type so a future widening to uint64 is caught.
    static_assert(std::is_invocable_r_v<void*, decltype(decode_oop_pointer), std::uint32_t>,
                  "decode_oop_pointer must accept exactly std::uint32_t");
    static_assert(std::is_invocable_r_v<std::uint32_t, decltype(encode_oop_pointer), void*>,
                  "encode_oop_pointer must accept exactly void*");
    check("oop_decode_signature_static_asserts_compiled", true);

    // =====================================================================
    // A. decode_oop_pointer / encode_oop_pointer — NULL / ZERO contract.
    //    OOP NULL SEMANTICS: a narrow oop of 0 is the canonical encoding of a
    //    Java null reference and decodes to nullptr — the codec short-circuits
    //    `if (!compressed) return nullptr;` (vmhook.hpp:5377) at the very top,
    //    BEFORE base/shift is consulted.  So 0 is null, NOT "the first oop at
    //    base+0".  Inverse: encode_oop_pointer(nullptr) == 0 (:5423).  These
    //    guards are the ONLY fully JVM-independent paths of the codec.
    // =====================================================================
    check("decode_oop_zero_is_null",
          decode_oop_pointer(0u) == nullptr);
    {
        const std::uint32_t null_oop{ 0u };
        void* const decoded{ decode_oop_pointer(null_oop) };
        check("decode_oop_zero_is_null_typed", decoded == nullptr);
    }
    check("encode_oop_null_is_zero",
          encode_oop_pointer(nullptr) == 0u);
    // Round-trips through null in both directions — the ONLY decode<->encode
    // identity that holds with no live heap base.
    check("oop_roundtrip_decode_then_encode_null",
          encode_oop_pointer(decode_oop_pointer(0u)) == 0u);
    check("oop_roundtrip_encode_then_decode_null",
          decode_oop_pointer(encode_oop_pointer(nullptr)) == nullptr);
    // A decoded null oop is, by definition, not a valid dereferenceable pointer,
    // and survives tag-stripping as null.  Ties the codec to the gating helper.
    check("decoded_null_oop_is_not_valid_pointer",
          !is_valid_pointer(decode_oop_pointer(0u)));
    check("decode_oop_zero_untags_to_null",
          untag_pointer(decode_oop_pointer(0u)) == nullptr);
    // Four-step null closure: decode(encode(decode(0))) == nullptr.
    check("oop_codec_null_closure",
          decode_oop_pointer(encode_oop_pointer(decode_oop_pointer(0u))) == nullptr);

    // =====================================================================
    // B. No-JVM fall-through (DECODE) — the missing-VMStruct path must return
    //    nullptr and NEVER crash, for a DENSE set of non-zero narrow inputs (not
    //    just 1 and max).  Off-JVM gHotSpotVMStructs is absent, so
    //    resolve_struct_entry returns nullptr for both base and shift, and the
    //    wrapper bails at the `!base_entry || !shift_entry` guard (vmhook.hpp:
    //    5402) WITHOUT ever dereferencing a VMStruct address.  Under a live JVM
    //    these same inputs decode to real heap addresses; this pins the
    //    degrade-gracefully promise the codec makes off-JVM.
    // =====================================================================
    {
        bool all_nonzero_decode_null{ true };
        std::size_t probed{ 0 };
        for (const std::uint32_t c : build_dense_narrows())
        {
            if (c == 0u) { continue; }              // 0 is the null-contract case
            if (decode_oop_pointer(c) != nullptr) { all_nonzero_decode_null = false; }
            ++probed;
        }
        check("decode_oop_all_nonzero_no_jvm_are_null", all_nonzero_decode_null);
        check("decode_oop_no_jvm_sweep_is_dense", probed >= 100);
    }
    // The classic named extremes, kept explicit for readability.
    check("decode_oop_one_no_jvm_is_null", decode_oop_pointer(1u) == nullptr);
    check("decode_oop_max_no_jvm_is_null", decode_oop_pointer(0xFFFF'FFFFu) == nullptr);
    check("decode_oop_signbit_no_jvm_is_null", decode_oop_pointer(0x8000'0000u) == nullptr);

    // =====================================================================
    // C. No-JVM fall-through (ENCODE) — a non-null pointer with no resolvable
    //    VMStructs returns 0 (vmhook.hpp:5445) and never crashes.  Feed a DENSE,
    //    varied set of REAL in-range addresses (stack, heap, allocate_rwx page,
    //    and a sweep of synthetic in-range even pointers across the canonical
    //    span).  None are dereferenced — encode only does VMStruct lookup +
    //    arithmetic — so passing raw addresses is safe.  This is the encode twin
    //    of section B's dense decode sweep.
    // =====================================================================
    {
        std::vector<std::uint64_t> heap_block(16, 0);
        int stack_anchor{ 0 };
        bool all_zero{ true };
        std::size_t encode_probed{ 0 };

        if (encode_oop_pointer(&stack_anchor) != 0u) { all_zero = false; }
        ++encode_probed;
        if (encode_oop_pointer(heap_block.data()) != 0u) { all_zero = false; }
        ++encode_probed;

        // A real RWX page (skipped if the platform refuses RWX, to stay portable).
        const std::size_t page_size{ vmhook::os::page_size() };
        void* const page{ vmhook::os::allocate_rwx(nullptr, page_size) };
        if (page != nullptr)
        {
            if (encode_oop_pointer(page) != 0u) { all_zero = false; }
            ++encode_probed;
            vmhook::os::release(page, page_size);
        }

        // A sweep of synthetic, in-range, even (2-byte-aligned) addresses across
        // the canonical user span — these stand in for "decoded" oop pointers.
        for (std::uint64_t hi{ 0x1'0000ull }; hi <= 0x7000'0000'0000ull; hi <<= 1)
        {
            void* const p{ reinterpret_cast<void*>(static_cast<std::uintptr_t>(hi)) };
            if (encode_oop_pointer(p) != 0u) { all_zero = false; }
            ++encode_probed;
        }
        check("encode_oop_dense_inrange_no_jvm_all_zero", all_zero);
        check("encode_oop_dense_sweep_is_dense", encode_probed >= 20);
    }
    // The encode wrapper is the strict inverse of decode on the null sentinel
    // even when chained densely: encode(decode(c)) is 0 for EVERY c off-JVM
    // (decode(c)->nullptr for c!=0, decode(0)->nullptr, encode(nullptr)->0).
    {
        bool chain_all_zero{ true };
        for (const std::uint32_t c : build_dense_narrows())
        {
            if (encode_oop_pointer(decode_oop_pointer(c)) != 0u) { chain_all_zero = false; }
        }
        check("encode_decode_oop_chain_all_zero_no_jvm", chain_all_zero);
    }

    // =====================================================================
    // D. OOP-CODEC DECODE ARITHMETIC — the workhorse.  decode_oop_pointer's
    //    body, once base/shift are resolved from CompressedOops, IS
    //    `narrow_decode(narrow_oop_base, narrow_oop_shift, compressed)`
    //    (vmhook.hpp:5411).  We reproduce that exact decode across every
    //    HEAP-style (base, shift) mode and the dense 32-bit narrow sweep,
    //    recomputing each expected value from the documented closed form
    //    base + (uint64(compressed) << shift).  This is the "every possible
    //    input" arithmetic coverage: modes * dense-sweep.
    // =====================================================================
    {
        const std::vector<std::uint32_t> narrows{ build_dense_narrows() };
        bool decode_matches_formula{ true };
        std::size_t decode_cases{ 0 };
        for (const oop_mode m : kModes)
        {
            for (const std::uint32_t c : narrows)
            {
                const std::uintptr_t got{ as_uptr(narrow_decode(m.base, m.shift, c)) };
                const std::uintptr_t want{ expect_decode(m.base, m.shift, c) };
                if (got != want) { decode_matches_formula = false; }
                ++decode_cases;
            }
        }
        check("oop_decode_matches_formula_all_modes", decode_matches_formula);
        check("oop_decode_sweep_is_dense", decode_cases >= 1000);
    }

    // =====================================================================
    // E. NARROW-VALUE WIDTH — a full 32-bit narrow oop must be consumed
    //    correctly: the widen-to-uint64-BEFORE-shift (the uint64 cast in
    //    narrow_decode, vmhook.hpp:5333) means 0xFFFFFFFF << 3 is the full
    //    0x7'FFFF'FFF8, NOT a 32-bit-truncated / sign-extended value.  This is
    //    the exact bug class the cast prevents, checked for the highest-risk
    //    extremes (all-ones, sign bit) at every shift the codec might program.
    // =====================================================================
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
        check("oop_decode_narrow_width_exact_all_shifts", width_ok);
        // The two named extremes at the canonical heap shift (3): must widen,
        // not sign-extend or truncate.
        check("oop_decode_all_ones_shift3_widened",
              as_uptr(narrow_decode(0u, 3u, 0xFFFF'FFFFu))
                  == static_cast<std::uintptr_t>(std::uint64_t{ 0xFFFF'FFFFull } << 3));
        check("oop_decode_sign_bit_shift3_widened",
              as_uptr(narrow_decode(0u, 3u, 0x8000'0000u))
                  == static_cast<std::uintptr_t>(std::uint64_t{ 0x8000'0000ull } << 3));
        // Top-bit value zero-extends into bit 32+, not a negative pointer.
        check("oop_decode_sign_bit_shift3_above_4G",
              as_uptr(narrow_decode(0u, 3u, 0x8000'0000u)) > 0xFFFF'FFFFull);
        // Max heap offset at shift 3 with the all-ones narrow oop:
        // 0xFFFFFFFF objects * 8 bytes == 0x7'FFFF'FFF8 (~32 GB span), exactly
        // the documented <=32 GB compressed-oops ceiling.
        check("oop_decode_max_offset_shift3",
              as_uptr(narrow_decode(0u, 3u, 0xFFFF'FFFFu)) == 0x7'FFFF'FFF8ull);
        // ...and at shift 0 the maximum offset is the 4 GB span (0xFFFFFFFF).
        check("oop_decode_max_offset_shift0",
              as_uptr(narrow_decode(0u, 0u, 0xFFFF'FFFFu)) == 0xFFFF'FFFFull);
    }

    // =====================================================================
    // F. OOP-CODEC ENCODE arithmetic + its OWN below-base guard.  Two distinct
    //    things encode_oop_pointer does beyond the shared primitive:
    //      (1) the `decoded_address < narrow_oop_base -> return 0` guard
    //          (vmhook.hpp:5453), unique to the encoder, BEFORE narrow_encode.
    //      (2) the subtract-shift-narrow, which IS narrow_encode (:5458).
    //    We reproduce the guard's decision and the arithmetic explicitly across
    //    every mode, driven over exact in-range representable addresses so the
    //    uint32 narrowing is lossless and the result must equal the original c.
    // =====================================================================
    {
        std::vector<std::uint32_t> comps;
        for (std::uint32_t k{ 0u }; k <= 8u; ++k) { comps.push_back(k); }
        const std::uint32_t extra[]{ 0x10u, 0xFFu, 0x100u, 0xFFFFu, 0x10000u, 0x00FF'FFFFu };
        for (const std::uint32_t e : extra) { comps.push_back(e); }

        bool encode_matches_formula{ true };
        for (const oop_mode m : kModes)
        {
            for (const std::uint32_t c : comps)
            {
                const std::uint64_t addr{ m.base + (static_cast<std::uint64_t>(c) << m.shift) };
                const std::uint32_t got{ narrow_encode(m.base, m.shift, addr) };
                const std::uint32_t want{ expect_encode(m.base, m.shift, addr) };
                if (got != want || got != c) { encode_matches_formula = false; }
            }
        }
        check("oop_encode_matches_formula_all_modes", encode_matches_formula);

        // The encoder's below-base guard predicate, modelled exactly.  For every
        // mode with a NON-ZERO base, an address strictly below base satisfies the
        // guard (addr < base), so encode_oop_pointer would short-circuit to 0 for
        // these without ever calling narrow_encode.  We exercise the PREDICATE
        // here; the wrapper's full path needs a JVM for the real base (see G).
        bool guard_rejects_below_base{ true };
        for (const oop_mode m : kModes)
        {
            if (m.base == 0u) { continue; }          // no "below 0" address exists
            const std::uint64_t below[]{ 0u, 1u, m.base - 1u, m.base / 2u };
            for (const std::uint64_t addr : below)
            {
                const bool guard_would_fire{ addr < m.base };
                if (!guard_would_fire) { guard_rejects_below_base = false; }
            }
        }
        check("oop_encode_below_base_guard_predicate_holds", guard_rejects_below_base);

        // And the real wrapper, with no JVM, returns 0 for a below-any-base
        // pointer regardless (missing VMStruct), so off-JVM the guard can never
        // be the reason a bogus non-zero narrow leaks out.
        check("encode_oop_below_base_wrapper_is_zero_no_jvm",
              encode_oop_pointer(reinterpret_cast<void*>(std::uintptr_t{ 0x10000u })) == 0u);
    }

    // =====================================================================
    // G. [INFO] The encoder below-base guard FIRING (flaw #1) is JVM-only.
    //    flaw #1: with a NON-ZERO narrow_oop_base, encode_oop_pointer of ANY
    //    pointer below the heap start (a foreign/native pointer, a stale wrapper
    //    address) returns 0 — the NULL compressed oop — silently mapping a
    //    non-null input to "store null into this Java field" with no distinct
    //    error sentinel.  Off-JVM the base is unresolvable so this returns 0 for
    //    the missing-VMStruct reason, NOT the below-base reason; the two are
    //    indistinguishable here.  Observing the guard genuinely fire requires a
    //    live JVM with a non-zero base (~4-32 GB heap) — see JVM module
    //    compressed_oops_decode.cpp, assertion "encode((void*)1) == 0 with a
    //    non-zero base".  We pin the off-JVM behaviour and DOCUMENT the gap.
    // =====================================================================
    check("encode_oop_subbase_sentinel_no_jvm_is_zero",
          encode_oop_pointer(reinterpret_cast<void*>(std::uintptr_t{ 1u })) == 0u);
    std::printf("[INFO] encode_oop below-base guard (flaw #1) FIRING needs a live "
                "non-zero-base JVM; off-JVM it is masked by the missing-VMStruct "
                "return-0 path.  Covered by JVM module compressed_oops_decode.cpp.\n");

    // =====================================================================
    // H. FULL ROUND-TRIP encode(decode(c)) == c — the single most important OOP
    //    codec identity — across EVERY mode and the dense 32-bit narrow sweep
    //    (incl. 0x7FFFFFFF / 0x80000000 / 0xFFFFFFFF).  Because decode widens to
    //    uint64 before <<shift and encode subtracts the same base then >>shift
    //    and re-narrows, the bits lost on the way out are exactly the bits
    //    restored on the way back, so this holds for ALL 32-bit c and ALL
    //    shifts.  A dense sweep proves it without enumerating 2^32 inputs.
    // =====================================================================
    {
        const std::vector<std::uint32_t> narrows{ build_dense_narrows() };
        bool roundtrip_ok{ true };
        std::size_t rt_cases{ 0 };
        std::uint32_t worst_mismatch_c{ 0 };
        for (const oop_mode m : kModes)
        {
            for (const std::uint32_t c : narrows)
            {
                void* const decoded{ narrow_decode(m.base, m.shift, c) };
                const std::uint32_t re{ narrow_encode(
                    m.base, m.shift, static_cast<std::uint64_t>(as_uptr(decoded))) };
                if (re != c) { roundtrip_ok = false; worst_mismatch_c = c; }
                ++rt_cases;
            }
        }
        check("oop_roundtrip_encode_decode_all_c_all_modes", roundtrip_ok);
        check("oop_roundtrip_matrix_is_dense", rt_cases >= 1000);
        // worst_mismatch_c is referenced so it is not unused under -Werror; it
        // stays 0 when everything round-trips.
        check("oop_roundtrip_no_mismatch_recorded", worst_mismatch_c == 0u);
    }

    // =====================================================================
    // I. ROUND-TRIP decode(encode(addr)) == addr over the REPRESENTABLE heap
    //    address set of each mode: addresses of the form base + (c << shift) for
    //    a dense c.  These are exactly the addresses a legitimately-aligned oop
    //    occupies; they must survive the round-trip bit-for-bit.  (Addresses NOT
    //    of this form are not representable as a narrow oop and are pinned as
    //    adversarial/lossy cases in section L.)
    // =====================================================================
    {
        std::vector<std::uint32_t> comps;
        for (std::uint32_t k{ 0u }; k <= 32u; ++k) { comps.push_back(k); }
        const std::uint32_t extra[]{ 0x100u, 0x1000u, 0x1'0000u, 0x10'0000u, 0x00FF'FFFFu };
        for (const std::uint32_t e : extra) { comps.push_back(e); }

        bool addr_roundtrip_ok{ true };
        for (const oop_mode m : kModes)
        {
            for (const std::uint32_t c : comps)
            {
                const std::uint64_t addr{ m.base + (static_cast<std::uint64_t>(c) << m.shift) };
                const std::uint32_t enc{ narrow_encode(m.base, m.shift, addr) };
                void* const back{ narrow_decode(m.base, m.shift, enc) };
                if (static_cast<std::uint64_t>(as_uptr(back)) != addr) { addr_roundtrip_ok = false; }
            }
        }
        check("oop_roundtrip_decode_encode_representable_addrs", addr_roundtrip_ok);
    }

    // =====================================================================
    // J. PRIMITIVE-LEVEL zero / at-base boundary for every OOP mode, and WHY the
    //    wrapper needs its own 0-guard.  The WRAPPER turns input 0 into nullptr
    //    (section A), but the underlying primitive is deliberately NOT null-aware:
    //      narrow_decode(base, shift, 0) == base   (for ANY shift)
    //      narrow_encode(base, shift, base) == 0    (addr == base -> 0)
    //    This pins the at-base/zero grid point the wrapper's null contract is
    //    layered on top of, and documents that WITHOUT the wrapper guard, a 0
    //    narrow would decode to `base` — a non-null heap address (the heap base
    //    itself), which is WRONG for a Java null reference.
    // =====================================================================
    {
        bool zero_decodes_to_base{ true };
        bool base_encodes_to_zero{ true };
        for (const oop_mode m : kModes)
        {
            if (as_uptr(narrow_decode(m.base, m.shift, 0u))
                != static_cast<std::uintptr_t>(m.base))
            {
                zero_decodes_to_base = false;
            }
            if (narrow_encode(m.base, m.shift, m.base) != 0u) { base_encodes_to_zero = false; }
        }
        check("oop_primitive_zero_decodes_to_base_all_modes", zero_decodes_to_base);
        check("oop_primitive_base_encodes_to_zero_all_modes", base_encodes_to_zero);
        // The contrast that justifies the wrapper guard, made concrete: at a
        // non-zero heap base, the PRIMITIVE maps 0 -> base (non-null), but the
        // WRAPPER maps 0 -> nullptr.  They MUST differ for a non-zero base.
        // kModes[2] is the first non-zero-base mode (based_4G_unscaled).
        check("oop_wrapper_zero_differs_from_primitive_zero_at_base",
              decode_oop_pointer(0u) == nullptr
                  && narrow_decode(kModes[2].base, kModes[2].shift, 0u)
                         == reinterpret_cast<void*>(static_cast<std::uintptr_t>(kModes[2].base)));
    }

    // =====================================================================
    // K. BOUNDARY / ALIGNMENT GRID at the canonical heap shift (3).  Compressed
    //    oops at shift 3 scale by 8 (8-byte object alignment), so consecutive
    //    narrow values map exactly 8 bytes apart and every decoded address is
    //    8-aligned when the base is 8-aligned (here base 0).
    // =====================================================================
    {
        const std::uintptr_t a0{ as_uptr(narrow_decode(0u, 3u, 100u)) };
        const std::uintptr_t a1{ as_uptr(narrow_decode(0u, 3u, 101u)) };
        const std::uintptr_t a2{ as_uptr(narrow_decode(0u, 3u, 102u)) };
        check("oop_decode_shift3_step_is_8_a", a1 - a0 == 8u);
        check("oop_decode_shift3_step_is_8_b", a2 - a1 == 8u);
        check("oop_decode_shift3_result_8_aligned",
              (a0 & 0x7u) == 0u && (a1 & 0x7u) == 0u && (a2 & 0x7u) == 0u);
        // From a real (8-aligned) heap base the result is still 8-aligned.
        const std::uintptr_t b0{ as_uptr(narrow_decode(0x8'0000'0000ull, 3u, 0xABCDu)) };
        check("oop_decode_shift3_based_result_8_aligned", (b0 & 0x7u) == 0u);
        // Round-trip the maximum representable oop at shift 3 from a non-zero
        // base: base + (0xFFFFFFFF << 3) must encode back to 0xFFFFFFFF.
        {
            const std::uint64_t base{ 0x8'0000'0000ull };
            const std::uint64_t top{ base + (std::uint64_t{ 0xFFFF'FFFFull } << 3) };
            check("oop_encode_max_offset_shift3_based_roundtrip",
                  narrow_encode(base, 3u, top) == 0xFFFF'FFFFu);
        }
    }

    // =====================================================================
    // L. ADVERSARIAL / DEGENERATE inputs — must be DETERMINISTIC and never crash
    //    (the primitives are noexcept, raw modular arithmetic).  We pin the EXACT
    //    wrapped value the documented formula yields, so these double as a spec
    //    for the corner behaviour rather than implying the inputs are "valid"
    //    oop values.  This is where flaw #2 (no upper-bound / shift-residue
    //    check; the final uint32 cast truncates) is CHARACTERISED off-JVM.
    // =====================================================================
    {
        // (L1) addr < base in encode underflows (uint64 wraps) then shifts and
        // narrows — deterministic modular result, no UB, no crash.
        const std::uint64_t base{ 0x1'0000'0000ull };
        const std::uint32_t got{ narrow_encode(base, 3u, 0u) };
        const std::uint32_t want{ expect_encode(base, 3u, 0u) };  // (0 - base) mod 2^64 >> 3
        check("oop_encode_underflow_is_modular_deterministic", got == want);

        // (L2) A misaligned heap address (not a multiple of 8 above base) loses
        // its low 3 bits on encode (integer >> 3): encode is LOSSY for off-grid
        // addresses, so decode(encode(addr)) FLOORS addr down to the alignment
        // grid.  This is the (addr-base) not-a-multiple-of-(1<<shift) half of
        // flaw #2 — encode(decode(x)) is NOT guaranteed to equal x for a
        // shift-misaligned x.  We pin the exact floored value.
        const std::uint64_t misaligned{ base + 13u };          // +13, not /8
        const std::uint32_t enc_mis{ narrow_encode(base, 3u, misaligned) };
        check("oop_encode_misaligned_floors_offset", enc_mis == 1u);   // 13 >> 3 == 1
        check("oop_decode_of_misaligned_encode_is_floored",
              as_uptr(narrow_decode(base, 3u, enc_mis))
                  == static_cast<std::uintptr_t>(base + 8u));           // floored to +8

        // (L3) TRUNCATION half of flaw #2: an offset (addr - base) whose value
        // exceeds the 32-bit-representable narrow range at the given shift is
        // SILENTLY truncated by the final static_cast<uint32_t>, producing a
        // valid-looking-but-WRONG narrow oop rather than a detectable failure.
        // We pin the truncation arithmetic EXACTLY (it is the documented current
        // behaviour) so a future change that adds an upper-bound check is caught.
        // offset = (0x1'0000'0001 << 0) needs 33 bits; >> 0 then uint32-cast
        // drops bit 32, yielding 0x0000'0001 instead of a >32-bit value.
        {
            const std::uint64_t base0{ 0u };
            const std::uint64_t over_range_addr{ base0 + 0x1'0000'0001ull };  // 33-bit offset
            const std::uint32_t truncated{ narrow_encode(base0, 0u, over_range_addr) };
            // Documented current (buggy) behaviour: high bit dropped.
            check("oop_encode_over_range_truncates_to_low32_flaw2",
                  truncated == 0x0000'0001u);
            // Decoding the truncated narrow does NOT recover the original addr —
            // the round-trip is broken for an out-of-range offset (flaw #2).
            check("oop_encode_over_range_roundtrip_is_broken_flaw2",
                  as_uptr(narrow_decode(base0, 0u, truncated)) != over_range_addr);
        }

        // (L4) Decode at the extreme top of the narrow range from a high heap
        // base must not overflow uint64 and equals the independent sum.  This is
        // the "cannot overflow for any real base+compressed+shift" honest-non-bug
        // from the brief: max base + (0xFFFFFFFF << shift) is far below 2^63.
        const std::uint64_t high_base{ 0x7000'0000'0000ull };
        check("oop_decode_extreme_top_high_base_deterministic",
              as_uptr(narrow_decode(high_base, 3u, 0xFFFF'FFFFu))
                  == static_cast<std::uintptr_t>(
                         high_base + (std::uint64_t{ 0xFFFF'FFFFull } << 3)));

        // (L5) encode-then-decode of an at-base address returns the at-base
        // address (the zero/null grid point) for a non-zero base.
        check("oop_at_base_roundtrips_to_base",
              as_uptr(narrow_decode(base, 3u, narrow_encode(base, 3u, base)))
                  == static_cast<std::uintptr_t>(base));
    }

    // =====================================================================
    // M. EXHAUSTIVE shift domain 0..63 for the OOP decode arithmetic.  HotSpot
    //    programs an oop shift of 0 or 3 today, but narrow_decode does a raw
    //    `uint64 << shift`, so EVERY shift in the well-defined range [0,63] must
    //    equal base + (uint64(c) << shift) exactly (shift 64+ is C++ UB and
    //    deliberately NOT exercised).  This widens sections D/E (which stop at a
    //    handful of shifts) to the full legal shift axis, recomputing each value
    //    independently.  A future JDK that picks a larger oop shift is covered.
    // =====================================================================
    {
        const std::uint32_t cs[]{ 1u, 2u, 3u, 0x7Fu, 0x8000u, 0x7FFF'FFFFu,
                                  0x8000'0000u, 0xFFFF'FFFFu };
        const std::uint64_t bases[]{ 0u, std::uint64_t{ 0x8'0000'0000ull } };
        bool all_shifts_ok{ true };
        std::size_t shift_cases{ 0 };
        for (const std::uint64_t base : bases)
        {
            for (std::uint32_t sh{ 0u }; sh <= 63u; ++sh)
            {
                for (const std::uint32_t c : cs)
                {
                    const std::uintptr_t got{ as_uptr(narrow_decode(base, sh, c)) };
                    const std::uintptr_t want{ static_cast<std::uintptr_t>(
                        base + (static_cast<std::uint64_t>(c) << sh)) };
                    if (got != want) { all_shifts_ok = false; }
                    ++shift_cases;
                }
            }
        }
        check("oop_decode_all_shifts_0_to_63_exact", all_shifts_ok);
        check("oop_decode_shift_axis_is_dense", shift_cases >= 500);

        // The single-bit narrow (c==1) lands at exactly bit `shift` for every
        // shift, base 0: the cleanest proof the shift is applied verbatim.
        bool single_bit_ok{ true };
        for (std::uint32_t sh{ 0u }; sh <= 63u; ++sh)
        {
            if (as_uptr(narrow_decode(0u, sh, 1u))
                != static_cast<std::uintptr_t>(std::uint64_t{ 1u } << sh))
            {
                single_bit_ok = false;
            }
        }
        check("oop_decode_c1_lands_at_bit_shift_all_shifts", single_bit_ok);
    }

    // =====================================================================
    // N. STRUCTURAL INVARIANTS the OOP decode must satisfy for any (base, shift):
    //    monotonicity (a larger narrow oop never decodes below a smaller one) and
    //    per-shift alignment (with an aligned base, the decoded address has its
    //    low `shift` bits zero — generalising the shift-3-only grid check in
    //    section K to the whole shift axis).  These hold by construction of the
    //    shift-add and pin that no overflow/sign bug perturbs the ordering or the
    //    low-bit structure within the representable (non-wrapping) range.
    // =====================================================================
    {
        bool monotone_ok{ true };
        bool step_is_pow2_shift{ true };
        for (const oop_mode m : kModes)
        {
            std::uintptr_t prev{ as_uptr(narrow_decode(m.base, m.shift, 1u)) };
            for (std::uint32_t c{ 2u }; c <= 64u; ++c)
            {
                const std::uintptr_t cur{ as_uptr(narrow_decode(m.base, m.shift, c)) };
                if (cur <= prev) { monotone_ok = false; }
                if (cur - prev != (std::uintptr_t{ 1u } << m.shift)) { step_is_pow2_shift = false; }
                prev = cur;
            }
        }
        check("oop_decode_monotone_increasing_all_modes", monotone_ok);
        check("oop_decode_consecutive_step_is_2_pow_shift", step_is_pow2_shift);

        // Per-shift alignment: with a 2^k-aligned base (base 0 is aligned to any
        // power of two), the decoded address has its low `shift` bits clear for
        // every shift 0..16.
        bool low_shift_bits_zero{ true };
        for (std::uint32_t sh{ 0u }; sh <= 16u; ++sh)
        {
            const std::uintptr_t mask{ (std::uintptr_t{ 1u } << sh) - 1u };
            const std::uint32_t probe[]{ 1u, 2u, 7u, 100u, 0xABCDu, 0x7FFF'FFFFu };
            for (const std::uint32_t c : probe)
            {
                const std::uintptr_t a{ as_uptr(narrow_decode(0u, sh, c)) };
                if ((a & mask) != 0u) { low_shift_bits_zero = false; }
            }
        }
        check("oop_decode_low_shift_bits_zero_base0_all_shifts", low_shift_bits_zero);
    }

    // =====================================================================
    // O. LOSSLESS-CHANNEL identity at the primitive level for EVERY shift 0..31.
    //    The codec is a lossless channel for any on-grid value: the bits the
    //    decode shifts OUT to the left are exactly the bits the encode shifts
    //    back IN, so (decode then encode) recovers c for ALL 32-bit c at ANY
    //    shift.  Section H asserts this for the curated kModes; here we sweep the
    //    FULL shift axis 0..31 against base 0 and a non-zero base, recomputing
    //    nothing — purely "does encode undo decode" — to pin the channel property
    //    independent of the specific (base, shift) HotSpot chooses.
    // =====================================================================
    {
        const std::uint32_t cs[]{ 1u, 2u, 0x7Fu, 0x1234u, 0x7FFF'FFFFu,
                                  0x8000'0000u, 0xFFFF'FFFFu };
        const std::uint64_t bases[]{ 0u, std::uint64_t{ 0x8'0000'0000ull } };
        bool channel_ok{ true };
        std::size_t channel_cases{ 0 };
        for (const std::uint64_t base : bases)
        {
            for (std::uint32_t sh{ 0u }; sh <= 31u; ++sh)
            {
                for (const std::uint32_t c : cs)
                {
                    void* const decoded{ narrow_decode(base, sh, c) };
                    const std::uint32_t re{ narrow_encode(
                        base, sh, static_cast<std::uint64_t>(as_uptr(decoded))) };
                    if (re != c) { channel_ok = false; }
                    ++channel_cases;
                }
            }
        }
        check("oop_lossless_channel_all_shifts_0_to_31", channel_ok);
        check("oop_lossless_channel_is_dense", channel_cases >= 400);
    }

    // =====================================================================
    // P. OOP VMStruct CANDIDATE ARRAYS — the JDK-version (type, field) fallback
    //    the OOP codec embeds, pinned through resolve_struct_entry.  This is the
    //    OOP-SPECIFIC half of the codec (the klass codec uses a DIFFERENT type
    //    name).  decode_oop_pointer / encode_oop_pointer feed resolve_struct_entry
    //    an ORDERED array of exactly these three (type, field) pairs and take the
    //    FIRST that resolves (first-match-wins):
    //      JDK 17-24 : CompressedOops::_narrow_oop._base / ._shift  (1st, happy path)
    //      JDK 25+   : CompressedOops::_base / _shift  (2nd, prefix dropped)
    //      JDK  8-16 : Universe::_narrow_oop._base / ._shift  (3rd, last fallback)
    //    With no JVM gHotSpotVMStructs is absent, so EVERY candidate misses and
    //    the walker returns nullptr — exactly why the wrappers degrade to
    //    nullptr/0 off-JVM.  We mirror the EXACT arrays the OOP wrappers embed and
    //    pin (a) they all resolve to nullptr here, (b) prefix-walk honours count,
    //    (c) the type is CompressedOops (NOT CompressedKlassPointers), anchoring
    //    the degrade-gracefully promise to the real OOP field names.
    //
    //    [INFO re flaw #3] The brief flagged a doc/lookup-order mismatch.  The
    //    current header comment (vmhook.hpp:5383-5385) lists JDK8-16/17-24/25+ in
    //    VERSION order while the candidate ARRAY is ordered 17-24, 25+, 8-16
    //    (happy-path first).  On a real JDK 8-16 the Universe entry is the THIRD
    //    (last) candidate, so a cold decode does two failed iterate walks first —
    //    a latent first-call cost, cached thereafter.  Not a correctness bug and
    //    not observable off-JVM; recorded here, not "fixed".
    // =====================================================================
    {
        using vmhook::hotspot::resolve_struct_entry;
        using vmhook::hotspot::struct_entry_candidate_t;
        using vmhook::hotspot::vm_struct_entry_t;

        static_assert(
            std::is_same_v<decltype(resolve_struct_entry(
                               static_cast<const struct_entry_candidate_t*>(nullptr),
                               std::size_t{ 0 })),
                           const vm_struct_entry_t*>,
            "resolve_struct_entry must return const vm_struct_entry_t*");
        static_assert(
            noexcept(resolve_struct_entry(
                static_cast<const struct_entry_candidate_t*>(nullptr), std::size_t{ 0 })),
            "resolve_struct_entry must be noexcept");

        // The EXACT OOP base/shift candidate arrays decode_oop_pointer embeds, in
        // the same array order (vmhook.hpp:5386-5395).
        static constexpr struct_entry_candidate_t oop_base_candidates[]{
            { "CompressedOops", "_narrow_oop._base" },  // JDK 17-24 (happy path)
            { "CompressedOops", "_base" },              // JDK 25+
            { "Universe", "_narrow_oop._base" },        // JDK 8-16 (last fallback)
        };
        static constexpr struct_entry_candidate_t oop_shift_candidates[]{
            { "CompressedOops", "_narrow_oop._shift" }, // JDK 17-24
            { "CompressedOops", "_shift" },             // JDK 25+
            { "Universe", "_narrow_oop._shift" },       // JDK 8-16
        };
        check("resolve_oop_base_candidates_no_jvm_is_null",
              resolve_struct_entry(oop_base_candidates,
                                   std::size(oop_base_candidates)) == nullptr);
        check("resolve_oop_shift_candidates_no_jvm_is_null",
              resolve_struct_entry(oop_shift_candidates,
                                   std::size(oop_shift_candidates)) == nullptr);

        // Prefix walk: trying only the JDK17-24 name, then +25, then +8-16 — each
        // a growing-prefix walk — all miss off-JVM.
        for (std::size_t take{ 1 }; take <= std::size(oop_base_candidates); ++take)
        {
            check("resolve_oop_base_prefix_walk_is_null",
                  resolve_struct_entry(oop_base_candidates, take) == nullptr);
        }
        // count == 0 short-circuits to nullptr WITHOUT dereferencing the array
        // pointer, so a null pointer with count 0 is well-defined.
        check("resolve_oop_count_zero_null_ptr_is_null",
              resolve_struct_entry(nullptr, 0u) == nullptr);
        check("resolve_oop_count_zero_valid_ptr_is_null",
              resolve_struct_entry(oop_base_candidates, 0u) == nullptr);

        // The OOP wrapper's candidate TYPE is CompressedOops (the heap), NOT
        // CompressedKlassPointers (the class space) — this pins that on a live
        // JVM the OOP codec would read the HEAP base/shift, never the klass's.
        check("oop_candidates_target_compressed_oops",
              std::strcmp(oop_base_candidates[0].type_name, "CompressedOops") == 0
                  && std::strcmp(oop_shift_candidates[0].type_name, "CompressedOops") == 0);
        // The JDK17-24 happy-path tier keeps the _narrow_oop. prefix.
        check("oop_candidates_happy_path_keeps_prefix",
              std::strcmp(oop_base_candidates[0].field_name, "_narrow_oop._base") == 0
                  && std::strcmp(oop_shift_candidates[0].field_name, "_narrow_oop._shift") == 0);
        // The JDK25+ tier dropped the prefix (bare _base / _shift).
        check("oop_candidates_jdk25_tier_drops_prefix",
              std::strcmp(oop_base_candidates[1].field_name, "_base") == 0
                  && std::strcmp(oop_shift_candidates[1].field_name, "_shift") == 0);
        // The legacy JDK8-16 tier lives under Universe (the last fallback).
        check("oop_candidates_legacy_tier_is_universe",
              std::strcmp(oop_base_candidates[2].type_name, "Universe") == 0
                  && std::strcmp(oop_shift_candidates[2].type_name, "Universe") == 0);

        // A bogus (type, field) pair that exists in no JVM resolves to nullptr,
        // and a long all-bogus list walks fully to the end (no false positive).
        static constexpr struct_entry_candidate_t bogus[]{
            { "ZZZ_NoSuchType", "_no_such_field" },
        };
        static constexpr struct_entry_candidate_t many_bogus[]{
            { "A", "_a" }, { "B", "_b" }, { "C", "_c" },
            { "D", "_d" }, { "E", "_e" }, { "F", "_f" },
        };
        check("resolve_single_bogus_candidate_is_null",
              resolve_struct_entry(bogus, std::size(bogus)) == nullptr);
        check("resolve_many_bogus_candidates_is_null",
              resolve_struct_entry(many_bogus, std::size(many_bogus)) == nullptr);
    }

    // =====================================================================
    // Q. CONSUMER HEURISTIC DOMAIN — the <=0xFFFFFFFF discriminator (vmhook.hpp
    //    ~6272 / ~9401 region) decides whether a raw interpreter slot holds a
    //    compressed oop (<= 4 GB, decode it) vs. an already-decoded full pointer
    //    (> 4 GB, use verbatim).  This is a CONSUMER concern, not a codec defect
    //    (honest non-bug from the brief).  What IS this feature's concern is the
    //    OUTPUT DOMAIN: which decoded addresses fall on which side of the 4 GB
    //    line.  We pin, purely arithmetically, that:
    //      - a zero-based small heap (base 0, shift 0/3) CAN produce a decoded
    //        address <= 0xFFFFFFFF (so the consumer must not re-decode it — the
    //        double-decode risk the JVM test must confirm doesn't bite), and
    //      - a based / high-shift heap produces addresses > 0xFFFFFFFF.
    //    [INFO] Whether a wrapper read actually double-decodes a small zero-based
    //    address is JVM-only (needs a live small heap); pinned here is only the
    //    output-domain arithmetic that frames that JVM test.
    // =====================================================================
    {
        // Zero-based shift 0: decode(c) == c, so small c stays <= 4 GB.
        check("oop_decode_zerobased_small_below_4G",
              as_uptr(narrow_decode(0u, 0u, 0x1000u)) <= 0xFFFF'FFFFull);
        // Zero-based shift 0, the 4 GB-1 boundary: still <= 4 GB by definition.
        check("oop_decode_zerobased_max_at_4G_minus_1",
              as_uptr(narrow_decode(0u, 0u, 0xFFFF'FFFFu)) == 0xFFFF'FFFFull);
        // Zero-based shift 3 with a large oop crosses ABOVE 4 GB (the consumer
        // would NOT decode such a slot — but the codec output proves the >4 GB
        // half of the domain is reachable from a zero-based heap too).
        check("oop_decode_zerobased_shift3_large_above_4G",
              as_uptr(narrow_decode(0u, 3u, 0x4000'0000u)) > 0xFFFF'FFFFull);
        // A based heap: every decoded address is above the base, which is itself
        // above 4 GB here, so all outputs are > 4 GB (consumer uses verbatim).
        check("oop_decode_based_4G_output_above_4G",
              as_uptr(narrow_decode(0x1'0000'0000ull, 0u, 1u)) > 0xFFFF'FFFFull);
        std::printf("[INFO] consumer <=0xFFFFFFFF heuristic (codec output-domain) "
                    "double-decode safety on a small zero-based heap is JVM-only; "
                    "see JVM module compressed_oops_decode.cpp.\n");
    }

    // =====================================================================
    // R. is_valid_pointer gating that FOLLOWS a decode (no-JVM-testable).  Every
    //    consumer that decodes an oop then gates the result through
    //    is_valid_pointer before dereferencing.  decode_oop_pointer(0) yields
    //    nullptr, which is_valid_pointer rejects; and the pure boundary battery
    //    (floor/ceiling/alignment/poison) decides which DECODED addresses would
    //    survive the gate.  The full is_valid_pointer matrix is owned by
    //    test_decode_oop_and_pointers.cpp; here we pin only the codec-adjacent
    //    invariants that tie a decode RESULT to the gate.
    // =====================================================================
    {
        constexpr std::uintptr_t floor{ vmhook::os::user_address_floor };    // 0xFFFF
        constexpr std::uintptr_t ceiling{ vmhook::os::user_address_ceiling };// 0x7FFF'FFFF'FFFF

        // decode(0) -> nullptr is rejected by the gate (already in A; repinned
        // here as the codec->gate hand-off invariant).
        check("decoded_null_oop_rejected_by_gate",
              !is_valid_pointer(decode_oop_pointer(0u)));

        // A decoded zero-based oop landing exactly on the floor (0xFFFF) is
        // rejected (<=), and the next 8-aligned grid point above it is accepted.
        // narrow_decode(0, 0, 0xFFFF) == 0xFFFF == floor.
        check("decoded_oop_at_floor_rejected",
              !is_valid_pointer(narrow_decode(0u, 0u,
                                static_cast<std::uint32_t>(floor))));
        // narrow_decode(0, 3, 0x2000) == 0x10000, comfortably above the floor,
        // 8-aligned, not poison -> accepted.
        check("decoded_oop_above_floor_8aligned_accepted",
              is_valid_pointer(narrow_decode(0u, 3u, 0x2000u)));

        // A decoded address that lands on an odd byte (only possible with shift 0
        // and an odd narrow value) is rejected by the bit-0 alignment rule even
        // though it is in range.  narrow_decode(0, 0, 0x40'0001) == 0x40'0001 (odd).
        check("decoded_oop_odd_shift0_rejected_by_alignment",
              !is_valid_pointer(narrow_decode(0u, 0u, 0x0040'0001u)));
        // Its even neighbour decodes to an accepted address.
        check("decoded_oop_even_shift0_accepted",
              is_valid_pointer(narrow_decode(0u, 0u, 0x0040'0000u)));

        // A decoded address whose low 32 bits match a debug-poison pattern is
        // rejected by the poison switch.  With a high in-range even base prefix
        // and a CAFEBABE low half via shift 0, the decode lands exactly on a
        // poison value -> gate rejects.  base 0x0000'1234'0000'0000 + 0xCAFEBABE.
        {
            const std::uint64_t poison_base{ 0x0000'1234'0000'0000ull };
            void* const decoded{ narrow_decode(poison_base, 0u, 0xCAFEBABEu) };
            // Premise: even & in range, so ONLY the poison switch can reject it.
            const std::uintptr_t a{ as_uptr(decoded) };
            const bool premise{ (a & 0x1u) == 0u && a > floor && a < ceiling };
            check("decoded_oop_poison_low32_rejected_by_gate",
                  premise && !is_valid_pointer(decoded));
        }
    }

    // =====================================================================
    // S. [INFO] Genuinely JVM-only OOP surface this no-JVM file CANNOT cover.
    //    Recorded explicitly so the report and the next maintainer know the
    //    boundary of what is proven here vs. what the JVM module must own.
    // =====================================================================
    std::printf("[INFO] JVM-only OOP surface (covered by JVM module "
                "compressed_oops_decode.cpp, NOT here): (1) decode<->encode "
                "round-trip on a REAL String.value oop; (2) shift==0 vs shift==3 "
                "AND base==0 vs base!=0 against a live heap (control -Xmx / "
                "-XX:-UseCompressedOops); (3) the encoder below-base guard (flaw #1) "
                "FIRING on a non-zero base; (4) two distinct live objects -> two "
                "distinct narrow oops; (5) codec vs. typed-wrapper/field-proxy "
                "agreement; (6) the VMStruct field-width ABI read (flaw #4) and the "
                "address-non-null assumption (flaw #5), which only matter when a "
                "real VMStruct address is dereferenced.\n");

    return failures == 0 ? 0 : 1;
}
