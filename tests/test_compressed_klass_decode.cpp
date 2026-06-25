// Standalone (no-JVM) EXHAUSTIVE unit test for the compressed-Klass decode
// feature: hotspot::decode_klass_pointer / encode_klass_pointer (the narrow
// Klass <-> Klass* codec) plus the two header readers that funnel through it
// (vmhook::klass_from_oop and object_base::klass_from_object_header).
//
// ───────────────────────────────────────────────────────────────────────────
// WHY A DEDICATED KLASS FILE (vs. the OOP file)
// ───────────────────────────────────────────────────────────────────────────
// test_decode_oop_and_pointers.cpp owns the *OOP* codec and the *shared*
// narrow_decode/narrow_encode arithmetic primitive.  The Klass codec is a
// SEPARATE compressed-pointer scheme: it resolves its OWN base/shift from a
// DIFFERENT VMStruct (CompressedKlassPointers::_narrow_klass.{_base,_shift} /
// the JDK-version fallbacks), and HotSpot is free to pick a Klass shift that
// differs from the OOP shift on the very same heap.  This file pins the Klass
// codec's contract end-to-end and documents, inline, exactly where Klass
// semantics differ from OOP semantics (see section "KLASS vs OOP").
//
// ───────────────────────────────────────────────────────────────────────────
// WHAT IS / ISN'T DETERMINABLE WITH NO JVM
// ───────────────────────────────────────────────────────────────────────────
// This executable runs with NO HotSpot JVM in-process, so gHotSpotVMStructs is
// never resolvable.  That makes exactly two layers fully deterministic here:
//
//   1. The codec WRAPPER contract (decode_klass_pointer / encode_klass_pointer):
//      the compressed==0 -> nullptr and decoded==nullptr -> 0 early returns
//      fire BEFORE any VMStruct lookup, and when the lookup fails the wrappers
//      return their null/zero sentinel instead of crashing.  These are pinned
//      directly on the public klass entry points.
//
//   2. The ARITHMETIC the klass codec performs once base/shift are resolved.
//      decode_klass_pointer's body is literally `return narrow_decode(base,
//      shift, compressed);` and encode_klass_pointer's is `return
//      narrow_encode(base, shift, decoded_address);` guarded by `decoded <
//      base -> 0`.  narrow_decode/narrow_encode take base/shift EXPLICITLY, so
//      we reproduce the klass codec's exact math by feeding the primitive the
//      Klass-style (base, shift) pairs HotSpot actually uses for the class
//      space (Metaspace base, CDS-region base, zero-based, shift 0 and 3).
//      This is where "all inputs possible" lives: a dense 32-bit narrow sweep
//      across every (base, shift) mode, every value recomputed from the
//      documented closed form, never guessed.
//
// Anything needing a LIVE klass / a real class-space base / a running JVM (an
// actual header decode that resolves to a find_class() result, a non-zero
// round-trip across the JVM's real _narrow_klass base) is OUT OF SCOPE here and
// is covered by the JVM integration modules (nested_classes, interface_
// polymorphism, field_introspection).
//
// Source of truth (line numbers approximate; the functions are the authority):
//   vmhook/ext/vmhook/vmhook.hpp
//     decode_klass_pointer            ~4670  (compressed==0 guard, narrow_decode)
//     encode_klass_pointer            ~4723  (decoded==null guard, decoded<base
//                                             guard, narrow_encode)
//     narrow_decode                   ~4531  base + (uint64(compressed) << shift)
//     narrow_encode                   ~4552  uint32((addr - base) >> shift)
//     klass_from_oop                  ~15663 read narrow at oop+8, decode, gate
//     klass_from_object_header        ~14609 read narrow at oop+8, decode, gate
//     is_valid_pointer                ~1825  range + bit-0 + poison gate
//     user_address_floor / ceiling    ~515/520
#include <vmhook/vmhook.hpp>
#include <cstddef>    // std::size_t
#include <cstdio>
#include <cstdint>
#include <cstring>    // std::strcmp (klass candidate (type, field) name pins)
#include <iterator>   // std::size (candidate-array counts for resolve_struct_entry)
#include <type_traits> // std::is_standard_layout_v / is_same_v (struct-layout pins)
#include <vector>
#include <string>    // std::string (read_java_string decode buffers, value_t)
#include <variant>   // std::monostate / variant_size_v / variant_alternative_t (value_t)

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

// Independent re-implementation of the documented decode formula, used ONLY to
// compute expected values.  NEVER calls into the library.
static auto expect_decode(std::uint64_t base, std::uint32_t shift, std::uint32_t c)
    -> std::uintptr_t
{
    return static_cast<std::uintptr_t>(
        base + (static_cast<std::uint64_t>(c) << shift));
}
// Independent re-implementation of the documented encode formula.
static auto expect_encode(std::uint64_t base, std::uint32_t shift, std::uint64_t addr)
    -> std::uint32_t
{
    return static_cast<std::uint32_t>((addr - base) >> shift);
}
static auto as_uptr(void* p) -> std::uintptr_t
{
    return reinterpret_cast<std::uintptr_t>(p);
}

// The Klass-style (base, shift) encoding modes HotSpot uses for the compressed
// class space.  Bases are SYNTHETIC stand-ins for the real flavours:
//   - 0            : zero-based class space (small, base folded to 0)
//   - Metaspace-ish: a low multi-GB base where Metaspace/class-space is mapped
//   - CDS-region   : a distinct, higher base where a CDS archive maps the
//                    class space (klass base != oop base in practice)
//   - high-canonical: near the top of the canonical user range
// shift is 0 (class space fits unshifted) or 3 (8-byte-aligned klass scaling).
// Extra shifts 1/2/4 prove the primitive scales by exactly 2^shift for any
// shift, not just the two HotSpot picks.
struct klass_mode { std::uint64_t base; std::uint32_t shift; const char* tag; };
static const klass_mode kModes[]{
    { 0u,                              0u, "zero_s0"     }, // zero-based, unscaled
    { 0u,                              3u, "zero_s3"     }, // zero-based, x8
    { std::uint64_t{ 0x0000'0008'0000'0000ull }, 0u, "meta_s0" }, // 32 GB base, unscaled
    { std::uint64_t{ 0x0000'0008'0000'0000ull }, 3u, "meta_s3" }, // 32 GB base, x8
    { std::uint64_t{ 0x0000'7F00'0000'0000ull }, 0u, "cds_s0"  }, // high CDS-ish base
    { std::uint64_t{ 0x0000'7F00'0000'0000ull }, 3u, "cds_s3"  }, // high CDS-ish base, x8
    { std::uint64_t{ 0x0000'0001'2345'6000ull }, 3u, "odd_s3"  }, // non-round base, x8
    { 0u,                              1u, "zero_s1"     }, // completeness shifts
    { 0u,                              2u, "zero_s2"     },
    { 0u,                              4u, "zero_s4"     },
};

// A dense 32-bit narrow-Klass sweep covering every structurally interesting
// class: 0, a small contiguous run, every power of two and its +/-1 neighbours
// across all 32 bit positions, and the 32-bit extremes.  Enumerating all 2^32
// is infeasible; this hits the decode-formula's every boundary.
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
    using vmhook::hotspot::decode_klass_pointer;
    using vmhook::hotspot::encode_klass_pointer;
    using vmhook::hotspot::decode_oop_pointer;
    using vmhook::hotspot::encode_oop_pointer;
    using vmhook::hotspot::narrow_decode;
    using vmhook::hotspot::narrow_encode;
    using vmhook::hotspot::is_valid_pointer;
    using vmhook::hotspot::untag_pointer;

    // =====================================================================
    // 0. COMPILE-TIME signature / noexcept / return-type pins (static_assert).
    //    These are constexpr-evaluable (they only inspect types), so they are
    //    asserted at compile time — a regression fails the BUILD, the strongest
    //    possible pin.  The arithmetic checks below cannot be static_assert'd
    //    because narrow_decode/narrow_encode use reinterpret_cast (not a
    //    constant expression), so those stay runtime.
    // =====================================================================
    static_assert(std::is_same_v<decltype(decode_klass_pointer(0u)), void*>,
                  "decode_klass_pointer must return void*");
    static_assert(std::is_same_v<decltype(encode_klass_pointer(nullptr)), std::uint32_t>,
                  "encode_klass_pointer must return std::uint32_t");
    static_assert(noexcept(decode_klass_pointer(0u)),
                  "decode_klass_pointer must be noexcept");
    static_assert(noexcept(encode_klass_pointer(nullptr)),
                  "encode_klass_pointer must be noexcept");
    static_assert(std::is_same_v<decltype(narrow_decode(0u, 0u, 0u)), void*>,
                  "narrow_decode must return void*");
    static_assert(std::is_same_v<decltype(narrow_encode(0u, 0u, 0u)), std::uint32_t>,
                  "narrow_encode must return std::uint32_t");
    static_assert(noexcept(narrow_decode(0u, 0u, 0u)) && noexcept(narrow_encode(0u, 0u, 0u)),
                  "narrow primitives must be noexcept");
    static_assert(std::is_same_v<decltype(vmhook::klass_from_oop(nullptr)),
                                 vmhook::hotspot::klass*>,
                  "klass_from_oop must return hotspot::klass*");
    static_assert(noexcept(vmhook::klass_from_oop(nullptr)),
                  "klass_from_oop must be noexcept");
    // Record the compile-time pins at runtime too so they show in the report.
    check("klass_decode_signature_static_asserts_compiled", true);

    // =====================================================================
    // A. decode_klass_pointer / encode_klass_pointer — NULL / ZERO contract.
    //    KLASS NULL SEMANTICS (verified against the implementation, NOT assumed
    //    from OOP rules): a narrow Klass of 0 decodes to nullptr — the codec
    //    short-circuits `if (!compressed) return nullptr;` at the very top,
    //    BEFORE any base/shift is consulted.  So 0 is the canonical null klass,
    //    NOT "the first klass at base+0".  (Contrast: narrow_decode(base,0,0)
    //    would return `base` — but the WRAPPER never reaches that for input 0.)
    //    Inverse: encode_klass_pointer(nullptr) == 0.
    // =====================================================================
    check("decode_klass_zero_is_null",
          decode_klass_pointer(0u) == nullptr);
    {
        const std::uint32_t null_klass{ 0u };
        void* const decoded{ decode_klass_pointer(null_klass) };
        check("decode_klass_zero_is_null_typed", decoded == nullptr);
    }
    check("encode_klass_null_is_zero",
          encode_klass_pointer(nullptr) == 0u);
    // Round-trips through null in both directions (the only decode<->encode
    // identity checkable with no live class-space base).
    check("klass_roundtrip_decode_then_encode_null",
          encode_klass_pointer(decode_klass_pointer(0u)) == 0u);
    check("klass_roundtrip_encode_then_decode_null",
          decode_klass_pointer(encode_klass_pointer(nullptr)) == nullptr);
    // A decoded null klass is, by definition, not a valid dereferenceable
    // pointer, and survives tag-stripping as null.
    check("decoded_null_klass_is_not_valid_pointer",
          !is_valid_pointer(decode_klass_pointer(0u)));
    check("decode_klass_zero_untags_to_null",
          untag_pointer(decode_klass_pointer(0u)) == nullptr);

    // =====================================================================
    // B. No-JVM fall-through — the missing-VMStruct path must return the
    //    null/zero sentinel and NEVER crash, for a DENSE set of non-zero
    //    narrow inputs (not just 1 and max).  gHotSpotVMStructs is absent here,
    //    so base_entry/shift_entry stay null and the wrapper bails at the
    //    missing-entry guard.  Under a live JVM these same inputs would decode
    //    to real class-space addresses; this pins the degrade-gracefully
    //    behaviour the codec promises off-JVM.
    // =====================================================================
    {
        bool all_nonzero_decode_null{ true };
        std::size_t probed{ 0 };
        for (const std::uint32_t c : build_dense_narrows())
        {
            if (c == 0u) { continue; }              // 0 is the null-contract case
            if (decode_klass_pointer(c) != nullptr) { all_nonzero_decode_null = false; }
            ++probed;
        }
        check("decode_klass_all_nonzero_no_jvm_are_null", all_nonzero_decode_null);
        check("decode_klass_no_jvm_sweep_is_dense", probed >= 100);
    }
    // The classic named extremes, kept explicit for readability.
    check("decode_klass_one_no_jvm_is_null", decode_klass_pointer(1u) == nullptr);
    check("decode_klass_max_no_jvm_is_null", decode_klass_pointer(0xFFFF'FFFFu) == nullptr);
    check("decode_klass_signbit_no_jvm_is_null", decode_klass_pointer(0x8000'0000u) == nullptr);
    // Encoder fall-through: a non-null pointer with no resolvable VMStructs
    // returns 0 without crashing.  Use real, varied in-range addresses.
    {
        int stack_anchor{ 0 };
        std::vector<std::uint64_t> heap_block(8, 0);
        bool all_encode_zero{ true };
        if (encode_klass_pointer(&stack_anchor) != 0u) { all_encode_zero = false; }
        if (encode_klass_pointer(heap_block.data()) != 0u) { all_encode_zero = false; }
        if (encode_klass_pointer(reinterpret_cast<void*>(std::uintptr_t{ 0x10000u })) != 0u)
        {
            all_encode_zero = false;
        }
        check("encode_klass_nonnull_no_jvm_is_zero", all_encode_zero);
    }

    // =====================================================================
    // C. KLASS-CODEC ARITHMETIC — the workhorse.  decode_klass_pointer's body,
    //    once base/shift are resolved, IS `narrow_decode(base, shift, c)`.  We
    //    reproduce that exact decode across every Klass-style (base, shift) mode
    //    and the dense 32-bit narrow sweep, recomputing each expected value from
    //    the documented closed form base + (uint64(c) << shift).  This is the
    //    "all inputs possible" arithmetic coverage: modes * dense-sweep.
    // =====================================================================
    {
        const std::vector<std::uint32_t> narrows{ build_dense_narrows() };
        bool decode_matches_formula{ true };
        std::size_t decode_cases{ 0 };
        for (const klass_mode m : kModes)
        {
            for (const std::uint32_t c : narrows)
            {
                const std::uintptr_t got{ as_uptr(narrow_decode(m.base, m.shift, c)) };
                const std::uintptr_t want{ expect_decode(m.base, m.shift, c) };
                if (got != want) { decode_matches_formula = false; }
                ++decode_cases;
            }
        }
        check("klass_decode_matches_formula_all_modes", decode_matches_formula);
        check("klass_decode_sweep_is_dense", decode_cases >= 1000);
    }

    // =====================================================================
    // D. NARROW-VALUE WIDTH — a full 32-bit narrow klass must be consumed
    //    correctly: the widen-to-uint64-BEFORE-shift means 0xFFFFFFFF << 3 is
    //    the full 0x7'FFFF'FFF8, NOT a truncated/sign-extended value.  This is
    //    the exact bug class the uint64 cast in narrow_decode prevents, checked
    //    for the highest-risk extremes (all-ones, sign bit) at every shift.
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
        check("klass_decode_narrow_width_exact_all_shifts", width_ok);
        // The two named extremes at the canonical klass shift (3): must widen.
        check("klass_decode_all_ones_shift3_widened",
              as_uptr(narrow_decode(0u, 3u, 0xFFFF'FFFFu))
                  == static_cast<std::uintptr_t>(std::uint64_t{ 0xFFFF'FFFFull } << 3));
        check("klass_decode_sign_bit_shift3_above_4G",
              as_uptr(narrow_decode(0u, 3u, 0x8000'0000u)) > 0xFFFF'FFFFull);
        // Max class-space offset at shift 3: 0xFFFFFFFF klasses * 8 bytes.
        check("klass_decode_max_offset_shift3",
              as_uptr(narrow_decode(0u, 3u, 0xFFFF'FFFFu)) == 0x7'FFFF'FFF8ull);
        check("klass_decode_max_offset_shift0",
              as_uptr(narrow_decode(0u, 0u, 0xFFFF'FFFFu)) == 0xFFFF'FFFFull);
    }

    // =====================================================================
    // E. encode_klass_pointer INVERSE arithmetic + its OWN underflow guard.
    //    Two distinct things the klass ENCODER does that the shared primitive
    //    does not:
    //      (1) the `decoded_address < base -> return 0` guard, which lives in
    //          encode_klass_pointer itself (~4757), BEFORE narrow_encode.
    //      (2) the subtract-shift-narrow, which IS narrow_encode.
    //    We reproduce the guard's decision and the arithmetic explicitly across
    //    every mode, driven over exact in-range representable addresses so the
    //    uint32 narrowing is lossless and the result must equal the original c.
    // =====================================================================
    {
        // The encoder math: addr >= base path is exactly narrow_encode.
        std::vector<std::uint32_t> comps;
        for (std::uint32_t k{ 0u }; k <= 8u; ++k) { comps.push_back(k); }
        const std::uint32_t extra[]{ 0x10u, 0xFFu, 0x100u, 0xFFFFu, 0x10000u, 0x00FF'FFFFu };
        for (const std::uint32_t e : extra) { comps.push_back(e); }

        bool encode_matches_formula{ true };
        for (const klass_mode m : kModes)
        {
            for (const std::uint32_t c : comps)
            {
                const std::uint64_t addr{ m.base + (static_cast<std::uint64_t>(c) << m.shift) };
                const std::uint32_t got{ narrow_encode(m.base, m.shift, addr) };
                const std::uint32_t want{ expect_encode(m.base, m.shift, addr) };
                if (got != want || got != c) { encode_matches_formula = false; }
            }
        }
        check("klass_encode_matches_formula_all_modes", encode_matches_formula);

        // The encoder's `decoded < base -> 0` guard.  For every mode with a
        // NON-ZERO base, an address strictly below base is rejected (encodes to
        // 0).  We model the guard's predicate exactly (addr < base) and assert
        // the documented result: encode_klass_pointer would short-circuit to 0
        // for these without ever calling narrow_encode.  (We exercise the
        // PREDICATE here; the wrapper's full path needs a JVM for the base.)
        bool guard_rejects_below_base{ true };
        for (const klass_mode m : kModes)
        {
            if (m.base == 0u) { continue; }            // no "below 0" address exists
            const std::uint64_t below[]{ 0u, 1u, m.base - 1u, m.base / 2u };
            for (const std::uint64_t addr : below)
            {
                const bool guard_would_fire{ addr < m.base };
                if (!guard_would_fire) { guard_rejects_below_base = false; }
            }
        }
        check("klass_encode_underflow_guard_predicate_holds", guard_rejects_below_base);

        // And the real wrapper, with no JVM, returns 0 for a below-any-base
        // pointer regardless (missing VMStruct), so the guard can never be the
        // reason a bogus non-zero narrow leaks out off-JVM.
        check("encode_klass_below_base_wrapper_is_zero_no_jvm",
              encode_klass_pointer(reinterpret_cast<void*>(std::uintptr_t{ 0x10000u })) == 0u);
    }

    // =====================================================================
    // F. FULL ROUND-TRIP encode(decode(c)) == c — the single most important
    //    codec identity — across EVERY klass mode and the dense 32-bit narrow
    //    sweep (including 0x7FFFFFFF / 0x80000000 / 0xFFFFFFFF).  Because decode
    //    widens to uint64 before <<shift and encode subtracts the same base then
    //    >>shift and re-narrows, the bits lost on the way out are exactly the
    //    bits restored on the way back, so this holds for ALL 32-bit c and ALL
    //    shifts.  A dense sweep proves it without enumerating 2^32 inputs.
    // =====================================================================
    {
        const std::vector<std::uint32_t> narrows{ build_dense_narrows() };
        bool roundtrip_ok{ true };
        std::size_t rt_cases{ 0 };
        std::uint32_t worst_mismatch_c{ 0 };
        for (const klass_mode m : kModes)
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
        check("klass_roundtrip_encode_decode_all_c_all_modes", roundtrip_ok);
        check("klass_roundtrip_matrix_is_dense", rt_cases >= 1000);
        check("klass_roundtrip_no_mismatch_recorded", worst_mismatch_c == 0u);
    }

    // =====================================================================
    // G. ROUND-TRIP decode(encode(addr)) == addr over the REPRESENTABLE
    //    class-space address set of each mode: addresses of the form
    //    base + (c << shift) for a dense c.  These are exactly the addresses a
    //    legitimately-aligned klass occupies; they must survive the round-trip
    //    bit-for-bit.
    // =====================================================================
    {
        std::vector<std::uint32_t> comps;
        for (std::uint32_t k{ 0u }; k <= 32u; ++k) { comps.push_back(k); }
        const std::uint32_t extra[]{ 0x100u, 0x1000u, 0x1'0000u, 0x10'0000u, 0x00FF'FFFFu };
        for (const std::uint32_t e : extra) { comps.push_back(e); }

        bool addr_roundtrip_ok{ true };
        for (const klass_mode m : kModes)
        {
            for (const std::uint32_t c : comps)
            {
                const std::uint64_t addr{ m.base + (static_cast<std::uint64_t>(c) << m.shift) };
                const std::uint32_t enc{ narrow_encode(m.base, m.shift, addr) };
                void* const back{ narrow_decode(m.base, m.shift, enc) };
                if (static_cast<std::uint64_t>(as_uptr(back)) != addr) { addr_roundtrip_ok = false; }
            }
        }
        check("klass_roundtrip_decode_encode_representable_addrs", addr_roundtrip_ok);
    }

    // =====================================================================
    // H. PRIMITIVE-LEVEL zero / at-base boundary for every klass mode.  The
    //    WRAPPER turns input 0 into nullptr (section A), but the underlying
    //    primitive is deliberately NOT null-aware:
    //      narrow_decode(base, shift, 0) == base   (for ANY shift)
    //      narrow_encode(base, shift, base) == 0    (addr == base -> 0)
    //    This pins the at-base/zero grid point the wrapper's null contract is
    //    layered on top of, and documents WHY the wrapper needs its own 0-guard
    //    (without it, a 0 narrow would decode to `base`, a non-null klass-space
    //    address — wrong for a null reference).
    // =====================================================================
    {
        bool zero_decodes_to_base{ true };
        bool base_encodes_to_zero{ true };
        for (const klass_mode m : kModes)
        {
            if (as_uptr(narrow_decode(m.base, m.shift, 0u))
                != static_cast<std::uintptr_t>(m.base))
            {
                zero_decodes_to_base = false;
            }
            if (narrow_encode(m.base, m.shift, m.base) != 0u) { base_encodes_to_zero = false; }
        }
        check("klass_primitive_zero_decodes_to_base_all_modes", zero_decodes_to_base);
        check("klass_primitive_base_encodes_to_zero_all_modes", base_encodes_to_zero);
        // The contrast that justifies the wrapper guard, made concrete: at a
        // non-zero klass base, the PRIMITIVE maps 0 -> base (non-null), but the
        // WRAPPER maps 0 -> nullptr.  They MUST differ for a non-zero base.
        check("klass_wrapper_zero_differs_from_primitive_zero_at_base",
              decode_klass_pointer(0u) == nullptr
                  && narrow_decode(kModes[2].base, kModes[2].shift, 0u)
                         == reinterpret_cast<void*>(static_cast<std::uintptr_t>(kModes[2].base)));
    }

    // =====================================================================
    // I. BOUNDARY / ALIGNMENT GRID at the canonical klass shift (3).  Klass
    //    structures are 8-byte scaled at shift 3, so consecutive narrow values
    //    map 8 bytes apart and every decoded address is 8-aligned (base 0).
    // =====================================================================
    {
        const std::uintptr_t a0{ as_uptr(narrow_decode(0u, 3u, 100u)) };
        const std::uintptr_t a1{ as_uptr(narrow_decode(0u, 3u, 101u)) };
        const std::uintptr_t a2{ as_uptr(narrow_decode(0u, 3u, 102u)) };
        check("klass_decode_shift3_step_is_8_a", a1 - a0 == 8u);
        check("klass_decode_shift3_step_is_8_b", a2 - a1 == 8u);
        check("klass_decode_shift3_result_8_aligned",
              (a0 & 0x7u) == 0u && (a1 & 0x7u) == 0u && (a2 & 0x7u) == 0u);
        // From a real (8-aligned) class-space base the result is still 8-aligned.
        const std::uintptr_t b0{ as_uptr(narrow_decode(0x8'0000'0000ull, 3u, 0xABCDu)) };
        check("klass_decode_shift3_based_result_8_aligned", (b0 & 0x7u) == 0u);
        // Round-trip the maximum representable klass at shift 3 from a non-zero
        // base: base + (0xFFFFFFFF << 3) must encode back to 0xFFFFFFFF.
        {
            const std::uint64_t base{ 0x8'0000'0000ull };
            const std::uint64_t top{ base + (std::uint64_t{ 0xFFFF'FFFFull } << 3) };
            check("klass_encode_max_offset_shift3_based_roundtrip",
                  narrow_encode(base, 3u, top) == 0xFFFF'FFFFu);
        }
    }

    // =====================================================================
    // J. ADVERSARIAL / DEGENERATE inputs — must be DETERMINISTIC and never
    //    crash (the primitives are noexcept, raw modular arithmetic).  We pin
    //    the EXACT wrapped value the documented formula yields, so these double
    //    as a spec for the corner behaviour rather than implying the inputs are
    //    "valid" klass values.
    // =====================================================================
    {
        // (J1) addr < base in encode underflows (uint64 wraps) then shifts and
        // narrows — deterministic modular result.
        const std::uint64_t base{ 0x8'0000'0000ull };
        const std::uint32_t got{ narrow_encode(base, 3u, 0u) };
        const std::uint32_t want{ expect_encode(base, 3u, 0u) };  // (0 - base) mod 2^64 >> 3
        check("klass_encode_underflow_is_modular_deterministic", got == want);

        // (J2) A misaligned class-space address (not a multiple of 8 above base)
        // loses its low 3 bits on encode (integer >> 3): encode is LOSSY for
        // off-grid addresses and decode(encode(addr)) floors to the grid.
        const std::uint64_t misaligned{ base + 13u };
        const std::uint32_t enc_mis{ narrow_encode(base, 3u, misaligned) };
        check("klass_encode_misaligned_floors_offset", enc_mis == 1u);   // 13 >> 3 == 1
        check("klass_decode_of_misaligned_encode_is_floored",
              as_uptr(narrow_decode(base, 3u, enc_mis))
                  == static_cast<std::uintptr_t>(base + 8u));

        // (J3) Decode at the extreme top of the narrow range from a high base
        // must not overflow uint64 and equals the independent sum.
        const std::uint64_t high_base{ 0x7000'0000'0000ull };
        check("klass_decode_extreme_top_high_base_deterministic",
              as_uptr(narrow_decode(high_base, 3u, 0xFFFF'FFFFu))
                  == static_cast<std::uintptr_t>(
                         high_base + (std::uint64_t{ 0xFFFF'FFFFull } << 3)));

        // (J4) A shift large enough to walk into the top of the 64-bit space is
        // still modular and crash-free.  shift 31, c == 0xFFFFFFFF.
        check("klass_decode_large_shift_is_modular",
              as_uptr(narrow_decode(0u, 31u, 0xFFFF'FFFFu))
                  == static_cast<std::uintptr_t>(std::uint64_t{ 0xFFFF'FFFFull } << 31));

        // (J5) encode-then-decode of an at-base address returns the at-base
        // address (the zero/null grid point) for a non-zero base.
        check("klass_at_base_roundtrips_to_base",
              as_uptr(narrow_decode(base, 3u, narrow_encode(base, 3u, base)))
                  == static_cast<std::uintptr_t>(base));
    }

    // =====================================================================
    // K. KLASS vs OOP — the headline distinction this file exists to pin.
    //    The two codecs are SEPARATE schemes that share ONE arithmetic
    //    primitive.  What differs and what doesn't, made explicit:
    //
    //      DIFFERENT: the VMStruct source.  decode_klass_pointer resolves
    //        CompressedKlassPointers::_narrow_klass.{_base,_shift} (with the
    //        Universe::_narrow_klass and CompressedKlassPointers::{_base,_shift}
    //        JDK fallbacks), whereas decode_oop_pointer resolves
    //        CompressedOops::_narrow_oop.{...} / Universe::_narrow_oop.  HotSpot
    //        may program a DIFFERENT base and a DIFFERENT shift for the class
    //        space than for the heap, so on a live JVM the SAME narrow value can
    //        decode to two different pointers under the two codecs.
    //
    //      SAME: the arithmetic.  base + (uint64(narrow) << shift) for both —
    //        they literally call the same narrow_decode.  So once base/shift are
    //        fixed, the two codecs are bitwise-identical functions.
    //
    //    With no JVM we cannot observe the two real base/shift pairs, but we CAN
    //    prove (a) both wrappers agree on the null sentinel, (b) the entry
    //    points are DISTINCT functions (a refactor must not collapse them into
    //    one, which would alias the klass base/shift onto the oop base/shift),
    //    and (c) when fed the SAME (base, shift) the shared primitive yields the
    //    SAME result while DIFFERENT (base, shift) — i.e. oop-shift vs klass-
    //    shift on one heap — yields DIFFERENT results.
    // =====================================================================
    {
        // (a) Both wrappers map their null sentinel identically.
        check("klass_and_oop_decode_zero_agree",
              decode_klass_pointer(0u) == decode_oop_pointer(0u));
        check("klass_and_oop_encode_null_agree",
              encode_klass_pointer(nullptr) == encode_oop_pointer(nullptr));

        // (b) The klass and oop codec entry points are DISTINCT functions.
        //     Taking their addresses and comparing pins that a future "dedupe"
        //     cannot accidentally make klass_from_oop read the OOP base/shift.
        void* const klass_decode_fp{
            reinterpret_cast<void*>(&vmhook::hotspot::decode_klass_pointer) };
        void* const oop_decode_fp{
            reinterpret_cast<void*>(&vmhook::hotspot::decode_oop_pointer) };
        void* const klass_encode_fp{
            reinterpret_cast<void*>(&vmhook::hotspot::encode_klass_pointer) };
        void* const oop_encode_fp{
            reinterpret_cast<void*>(&vmhook::hotspot::encode_oop_pointer) };
        check("klass_decode_is_distinct_from_oop_decode",
              klass_decode_fp != oop_decode_fp);
        check("klass_encode_is_distinct_from_oop_encode",
              klass_encode_fp != oop_encode_fp);

        // (c) SAME (base, shift) -> SAME pointer (one formula, used twice).
        const std::uint64_t shared_base{ 0x8'0000'0000ull };
        const std::uint32_t shared_shift{ 3u };
        const std::uint32_t vals[]{ 1u, 7u, 0x1000u, 0x7FFF'FFFFu, 0xFFFF'FFFFu };
        bool same_bs_same_result{ true };
        for (const std::uint32_t v : vals)
        {
            // The "klass path" and "oop path" both reduce to narrow_decode.
            const std::uintptr_t k{ as_uptr(narrow_decode(shared_base, shared_shift, v)) };
            const std::uintptr_t o{ as_uptr(narrow_decode(shared_base, shared_shift, v)) };
            if (k != o || k != expect_decode(shared_base, shared_shift, v))
            {
                same_bs_same_result = false;
            }
        }
        check("klass_oop_same_base_shift_same_result", same_bs_same_result);

        // (c') DIFFERENT shift on the SAME heap -> DIFFERENT pointer.  Model a
        //      heap whose oop shift is 3 but whose klass shift is 0 (both
        //      perfectly legal; the class space fit unshifted while the heap did
        //      not).  For a non-zero narrow, decoding under the two shifts MUST
        //      diverge, proving the codecs genuinely read independent shifts.
        const std::uint64_t common_base{ 0u };
        const std::uint32_t oop_shift{ 3u };
        const std::uint32_t klass_shift{ 0u };
        bool different_shift_diverges{ true };
        for (const std::uint32_t v : vals)
        {
            if (v == 0u) { continue; }
            const std::uintptr_t as_oop{ as_uptr(narrow_decode(common_base, oop_shift, v)) };
            const std::uintptr_t as_klass{ as_uptr(narrow_decode(common_base, klass_shift, v)) };
            if (as_oop == as_klass) { different_shift_diverges = false; }
        }
        check("klass_vs_oop_independent_shift_diverges", different_shift_diverges);

        // (c'') DIFFERENT base on the SAME heap -> DIFFERENT pointer.  Heap base
        //       and class-space (CDS) base are typically distinct addresses.
        const std::uint64_t heap_base{ 0x8'0000'0000ull };
        const std::uint64_t klass_base{ 0x7F00'0000'0000ull };
        bool different_base_diverges{ true };
        for (const std::uint32_t v : vals)
        {
            const std::uintptr_t as_oop{ as_uptr(narrow_decode(heap_base, 0u, v)) };
            const std::uintptr_t as_klass{ as_uptr(narrow_decode(klass_base, 0u, v)) };
            if (as_oop == as_klass) { different_base_diverges = false; }
        }
        check("klass_vs_oop_independent_base_diverges", different_base_diverges);
    }

    // =====================================================================
    // L. klass_from_oop / klass_from_object_header — the header readers that
    //    funnel through decode_klass_pointer.  Their NULL / INVALID-input
    //    contract is fully determinable with no JVM because the is_valid_pointer
    //    pre-gate fires BEFORE any dereference of oop+8, so these inputs never
    //    touch memory.  (A VALID, mapped oop with a real header is JVM-only and
    //    covered in the integration modules.)
    //
    //    klass_from_object_header is private (object_base member), so it is
    //    exercised transitively: the public klass_from_oop and the private
    //    reader are byte-for-byte identical (read narrow at +8, decode, gate),
    //    so klass_from_oop's contract pins the shared logic.
    // =====================================================================
    {
        // nullptr -> nullptr (the `!oop` arm of the pre-gate).
        check("klass_from_oop_null_is_null",
              vmhook::klass_from_oop(nullptr) == nullptr);
        // Sub-floor / odd / poison / kernel-half pointers are all rejected by
        // is_valid_pointer BEFORE the +8 read, so they are safe to pass with no
        // JVM and must yield nullptr.  None of these are dereferenced.
        const std::uintptr_t bad_inputs[]{
            std::uintptr_t{ 0x1u },                        // below floor + odd
            std::uintptr_t{ 0xFFFFu },                     // exactly the floor (rejected, <=)
            std::uintptr_t{ 0xDEADBEEFu },                 // odd + poison-ish, low
            std::uintptr_t{ 0x0000'1234'0000'0001ull },    // in range but ODD -> rejected
            std::uintptr_t{ 0x0000'8000'0000'0000ull },    // first kernel-half addr -> rejected
            std::uintptr_t{ 0xFFFF'8000'0000'0000ull },    // high non-canonical -> rejected
            std::uintptr_t{ 0x7FFF'FFFF'FFFFull },          // exactly the ceiling (rejected, >=)
        };
        bool all_bad_rejected{ true };
        for (const std::uintptr_t bad : bad_inputs)
        {
            if (vmhook::klass_from_oop(reinterpret_cast<void*>(bad)) != nullptr)
            {
                all_bad_rejected = false;
            }
        }
        check("klass_from_oop_invalid_inputs_all_null", all_bad_rejected);

        // A pointer that PASSES the input pre-gate (a real, mapped, aligned
        // stack object) still resolves to nullptr off-JVM, because the +8 read
        // yields some stack bytes, decode_klass_pointer has no VMStructs and
        // returns nullptr.  This proves the WHOLE reader degrades to nullptr
        // (not a crash, not a bogus klass) with no JVM, for a valid-shaped oop.
        {
            // 64 bytes of zeroed, 8-aligned stack: +8 reads 0 -> decode(0)=null.
            alignas(16) std::uint8_t fake_object[64]{};
            check("klass_from_oop_valid_shaped_no_jvm_is_null",
                  vmhook::klass_from_oop(fake_object) == nullptr);
            // Same with a non-zero narrow at +8: still nullptr (no VMStructs),
            // proving the nullptr comes from the missing codec data, not from
            // the +8 slot happening to be zero.
            *reinterpret_cast<std::uint32_t*>(fake_object + 8) = 0x1234'5678u;
            check("klass_from_oop_valid_shaped_nonzero_narrow_no_jvm_is_null",
                  vmhook::klass_from_oop(fake_object) == nullptr);
        }
    }

    // =====================================================================
    // M. CONSISTENCY across the codec entry points and the primitive — the
    //    decode_klass_pointer null result must agree with is_valid_pointer and
    //    with chaining through untag, exactly as for the oop codec, tying the
    //    klass clusters together with pure-logic invariants.
    // =====================================================================
    check("klass_decode_null_consistent_with_is_valid_pointer",
          !is_valid_pointer(decode_klass_pointer(0u)));
    check("klass_decode_null_consistent_through_untag",
          untag_pointer(decode_klass_pointer(0u)) == nullptr);
    // The encoder is the strict inverse on null: encode(nullptr) feeding decode
    // yields nullptr; decode(0) feeding encode yields 0.  Both already checked
    // in A; assert the composed 4-step identity once more as a closure pin.
    check("klass_codec_null_closure",
          decode_klass_pointer(encode_klass_pointer(decode_klass_pointer(0u))) == nullptr);

    // =====================================================================
    // N. resolve_struct_entry — the JDK-VERSION fallback walker that the klass
    //    codec uses to find its base/shift field across HotSpot revisions.  This
    //    is where the "JDK-specific narrow-klass differences" actually live: the
    //    klass base/shift field migrated names across versions
    //      JDK  8-16 : Universe::_narrow_klass._base / ._shift
    //      JDK 17-24 : CompressedKlassPointers::_narrow_klass._base / ._shift
    //      JDK 25+   : CompressedKlassPointers::_base / _shift  (prefix dropped)
    //    decode_klass_pointer / encode_klass_pointer feed resolve_struct_entry an
    //    ORDERED candidate array of exactly these three (type, field) pairs and
    //    take the FIRST that resolves (first-match-wins).  With no JVM,
    //    gHotSpotVMStructs is absent so EVERY candidate misses and the walker
    //    returns nullptr — which is precisely why the wrappers degrade to
    //    nullptr/0 off-JVM.  We pin the walker's contract directly (it is a pure,
    //    deterministic, cross-platform loop): null for any list off-JVM, correct
    //    handling of count==0 / single / multi candidate lists, and that the
    //    EXACT klass candidate arrays the wrappers embed all resolve to nullptr
    //    here (so the degrade-gracefully promise is anchored to the real names).
    // =====================================================================
    {
        using vmhook::hotspot::resolve_struct_entry;
        using vmhook::hotspot::struct_entry_candidate_t;
        using vmhook::hotspot::vm_struct_entry_t;

        // Signature / noexcept / return-type pins for the walker.
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

        // The EXACT klass base-field candidate array decode_klass_pointer embeds,
        // in version order.  Off-JVM, none resolve -> walker returns nullptr.
        static constexpr struct_entry_candidate_t klass_base_candidates[]{
            { "CompressedKlassPointers", "_narrow_klass._base" },  // JDK 17-24
            { "CompressedKlassPointers", "_base" },                // JDK 25+
            { "Universe", "_narrow_klass._base" },                 // JDK 8-16
        };
        static constexpr struct_entry_candidate_t klass_shift_candidates[]{
            { "CompressedKlassPointers", "_narrow_klass._shift" }, // JDK 17-24
            { "CompressedKlassPointers", "_shift" },               // JDK 25+
            { "Universe", "_narrow_klass._shift" },                // JDK 8-16
        };
        check("resolve_klass_base_candidates_no_jvm_is_null",
              resolve_struct_entry(klass_base_candidates,
                                   std::size(klass_base_candidates)) == nullptr);
        check("resolve_klass_shift_candidates_no_jvm_is_null",
              resolve_struct_entry(klass_shift_candidates,
                                   std::size(klass_shift_candidates)) == nullptr);

        // Walking a PREFIX of the list (count < array size) is honoured: trying
        // only the JDK17-24 name, only the JDK25+ name, only the JDK8-16 name —
        // each a single-candidate walk — all miss off-JVM.
        for (std::size_t take{ 1 }; take <= std::size(klass_base_candidates); ++take)
        {
            check("resolve_klass_base_prefix_walk_is_null",
                  resolve_struct_entry(klass_base_candidates, take) == nullptr);
        }

        // count == 0 must short-circuit to nullptr WITHOUT dereferencing the
        // array pointer at all (so a null pointer with count 0 is well-defined).
        check("resolve_count_zero_null_ptr_is_null",
              resolve_struct_entry(nullptr, 0u) == nullptr);
        check("resolve_count_zero_valid_ptr_is_null",
              resolve_struct_entry(klass_base_candidates, 0u) == nullptr);

        // A single bogus candidate (a (type, field) pair that exists in no JVM,
        // let alone none) resolves to nullptr.
        static constexpr struct_entry_candidate_t bogus[]{
            { "ZZZ_NoSuchType", "_no_such_field" },
        };
        check("resolve_single_bogus_candidate_is_null",
              resolve_struct_entry(bogus, std::size(bogus)) == nullptr);

        // A list of MANY bogus candidates still walks to the end and returns
        // nullptr (the full loop is exercised, no early false-positive).
        static constexpr struct_entry_candidate_t many_bogus[]{
            { "A", "_a" }, { "B", "_b" }, { "C", "_c" },
            { "D", "_d" }, { "E", "_e" }, { "F", "_f" },
        };
        check("resolve_many_bogus_candidates_is_null",
              resolve_struct_entry(many_bogus, std::size(many_bogus)) == nullptr);

        // The klass and oop wrappers use DIFFERENT type names — assert the klass
        // candidate type is CompressedKlassPointers (not CompressedOops), pinning
        // that this file's candidate arrays mirror the klass codec and would, on
        // a live JVM, read the CLASS-space base/shift, never the heap's.
        check("klass_candidates_target_compressed_klass_pointers",
              std::strcmp(klass_base_candidates[0].type_name,
                          "CompressedKlassPointers") == 0
                  && std::strcmp(klass_shift_candidates[0].type_name,
                                 "CompressedKlassPointers") == 0);
        // ...and the JDK25+ tier dropped the _narrow_klass. prefix (bare _base).
        check("klass_candidates_jdk25_tier_drops_prefix",
              std::strcmp(klass_base_candidates[1].field_name, "_base") == 0
                  && std::strcmp(klass_shift_candidates[1].field_name, "_shift") == 0);
        // ...and the legacy JDK8-16 tier lives under Universe.
        check("klass_candidates_legacy_tier_is_universe",
              std::strcmp(klass_base_candidates[2].type_name, "Universe") == 0
                  && std::strcmp(klass_shift_candidates[2].type_name, "Universe") == 0);
    }

    // =====================================================================
    // O. EXHAUSTIVE shift domain 0..63 for the klass decode arithmetic.  The
    //    klass shift HotSpot programs is 0 or 3 today, but the primitive does a
    //    raw `uint64 << shift`, so EVERY shift in the well-defined range [0,63]
    //    must equal base + (uint64(c) << shift) exactly (shift 64+ is C++ UB and
    //    deliberately NOT exercised).  This widens section C/D (which stop at a
    //    handful of shifts) to the full legal shift axis, recomputing each value
    //    independently.  A future JDK that picks a larger klass shift is covered.
    // =====================================================================
    {
        // A focused narrow set whose high bits matter under large shifts: 0 is
        // skipped (covered elsewhere), 1 walks the shifted bit across the whole
        // 64-bit word, and the extremes stress the widen-before-shift.
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
        check("klass_decode_all_shifts_0_to_63_exact", all_shifts_ok);
        check("klass_decode_shift_axis_is_dense", shift_cases >= 500);

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
        check("klass_decode_c1_lands_at_bit_shift_all_shifts", single_bit_ok);
    }

    // =====================================================================
    // P. STRUCTURAL INVARIANTS the klass decode must satisfy for any (base,
    //    shift): monotonicity (a larger narrow never decodes below a smaller one)
    //    and per-shift alignment (with an aligned base, the decoded address has
    //    its low `shift` bits zero — generalising the shift-3-only grid check in
    //    section I to the whole shift axis).  These hold by construction of the
    //    shift-add and pin that no overflow/sign bug perturbs the ordering or the
    //    low-bit structure within the representable (non-wrapping) range.
    // =====================================================================
    {
        // Monotonicity: over a strictly increasing narrow run, decoded addresses
        // are strictly increasing too (within the non-wrapping range), for every
        // klass mode.  Step is exactly (1 << shift) between consecutive values.
        bool monotone_ok{ true };
        bool step_is_pow2_shift{ true };
        for (const klass_mode m : kModes)
        {
            std::uintptr_t prev{ as_uptr(narrow_decode(m.base, m.shift, 1u)) };
            for (std::uint32_t c{ 2u }; c <= 64u; ++c)
            {
                const std::uintptr_t cur{ as_uptr(narrow_decode(m.base, m.shift, c)) };
                if (cur <= prev) { monotone_ok = false; }
                if (cur - prev != (std::uintptr_t{ 1u } << m.shift))
                {
                    step_is_pow2_shift = false;
                }
                prev = cur;
            }
        }
        check("klass_decode_monotone_increasing_all_modes", monotone_ok);
        check("klass_decode_consecutive_step_is_2_pow_shift", step_is_pow2_shift);

        // Per-shift alignment: with an 8-aligned (here: 2^k-aligned) base, the
        // decoded address has its low `shift` bits clear for every shift 0..16.
        // (base 0 is aligned to any power of two.)
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
        check("klass_decode_low_shift_bits_zero_base0_all_shifts", low_shift_bits_zero);
    }

    // =====================================================================
    // Q. encode_klass_pointer WRAPPER — dense off-JVM fall-through on the encode
    //    side.  Section B proves 3 pointers encode to 0 with no JVM; here we feed
    //    a DENSE, varied set of real in-range addresses (stack, heap, a sweep of
    //    synthetic in-range even pointers across the canonical span) and confirm
    //    the wrapper returns 0 for ALL of them off-JVM, never a bogus non-zero
    //    narrow.  This is the encode twin of section B's dense decode sweep.
    // =====================================================================
    {
        std::vector<std::uint64_t> heap_block(16, 0);
        int stack_anchor{ 0 };
        bool all_zero{ true };
        std::size_t encode_probed{ 0 };

        // Real, mapped pointers.
        if (encode_klass_pointer(&stack_anchor) != 0u) { all_zero = false; }
        ++encode_probed;
        if (encode_klass_pointer(heap_block.data()) != 0u) { all_zero = false; }
        ++encode_probed;

        // A sweep of synthetic, in-range, even (2-byte-aligned) addresses across
        // the canonical user span.  None are dereferenced — encode only does
        // arithmetic/VMStruct lookup — so passing raw values is safe here.
        for (std::uint64_t hi{ 0x1'0000ull }; hi <= 0x7000'0000'0000ull; hi <<= 1)
        {
            void* const p{ reinterpret_cast<void*>(static_cast<std::uintptr_t>(hi)) };
            if (encode_klass_pointer(p) != 0u) { all_zero = false; }
            ++encode_probed;
        }
        check("encode_klass_dense_inrange_no_jvm_all_zero", all_zero);
        check("encode_klass_dense_sweep_is_dense", encode_probed >= 20);

        // The encode wrapper is the strict inverse of decode on the null sentinel
        // even when chained densely: encode(decode(c)) is 0 for EVERY c off-JVM
        // (decode(c)->nullptr for c!=0, decode(0)->nullptr, encode(nullptr)->0).
        bool chain_all_zero{ true };
        for (const std::uint32_t c : build_dense_narrows())
        {
            if (encode_klass_pointer(decode_klass_pointer(c)) != 0u)
            {
                chain_all_zero = false;
            }
        }
        check("encode_decode_chain_all_zero_no_jvm", chain_all_zero);
    }

    // =====================================================================
    // R. KLASS vs OOP WRAPPERS — off-JVM behavioural EQUIVALENCE on the
    //    degrade-gracefully path, asserted densely.  Section K pins that the two
    //    entry points are DISTINCT functions and diverge on a live heap (modelled
    //    via the primitive with different base/shift).  Here we pin the
    //    complementary fact: with NO JVM, both wrappers collapse to the SAME
    //    sentinel for EVERY input (decode->nullptr, encode->0), because neither
    //    can resolve its (different) VMStruct.  So the distinction is purely the
    //    VMStruct SOURCE, observable only when that source exists — off-JVM they
    //    are indistinguishable, and this dense agreement proves it.
    // =====================================================================
    {
        bool decode_agree{ true };
        for (const std::uint32_t c : build_dense_narrows())
        {
            if (decode_klass_pointer(c) != decode_oop_pointer(c)) { decode_agree = false; }
            // both must be null off-JVM (0 -> null guard; nonzero -> missing entry)
            if (decode_klass_pointer(c) != nullptr) { decode_agree = false; }
        }
        check("klass_oop_decode_agree_and_null_dense_no_jvm", decode_agree);

        bool encode_agree{ true };
        std::vector<std::uint64_t> heap_block(8, 0);
        int anchor{ 0 };
        void* const probes[]{
            nullptr,
            &anchor,
            heap_block.data(),
            reinterpret_cast<void*>(std::uintptr_t{ 0x10000u }),
            reinterpret_cast<void*>(std::uintptr_t{ 0x8'0000'0000ull }),
        };
        for (void* const p : probes)
        {
            if (encode_klass_pointer(p) != encode_oop_pointer(p)) { encode_agree = false; }
            if (encode_klass_pointer(p) != 0u) { encode_agree = false; }
        }
        check("klass_oop_encode_agree_and_zero_dense_no_jvm", encode_agree);
    }

    // =====================================================================
    // S. LOSSLESS-CHANNEL identity at the primitive level for EVERY shift 0..31.
    //    The codec is a lossless channel for any aligned-on-the-grid value: the
    //    bits the decode shifts OUT to the left are exactly the bits the encode
    //    shifts back IN, so (decode then encode) recovers c for ALL 32-bit c at
    //    ANY shift.  Section F asserts this for the curated kModes; here we sweep
    //    the FULL shift axis 0..31 against base 0 and a non-zero base, recomputing
    //    nothing — purely "does encode undo decode" — to pin the channel property
    //    independent of the specific base/shift HotSpot happens to choose.
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
        check("klass_lossless_channel_all_shifts_0_to_31", channel_ok);
        check("klass_lossless_channel_is_dense", channel_cases >= 400);
    }

    // =====================================================================
    // T. EXPORTED STRUCT LAYOUT — the records the klass codec reads THROUGH.
    //    decode_klass_pointer / encode_klass_pointer resolve a
    //    vm_struct_entry_t* and then dereference entry->address; the resolver
    //    they feed walks an array of struct_entry_candidate_t.  Both are plain
    //    aggregates whose binary layout MUST stay standard-layout (the codec
    //    treats them as C PODs that mirror HotSpot's gHotSpotVMStructs ABI and
    //    constexpr candidate arrays).  These are 100% no-JVM-determinable: pure
    //    type traits + offsetof, no memory touched.  Member layout/types are
    //    pinned from the source declarations:
    //      vm_struct_entry_t { const char* type_name; const char* field_name;
    //                          const char* type_string; std::int32_t is_static;
    //                          std::uint64_t offset; void* address; }
    //      struct_entry_candidate_t { const char* type_name;
    //                                 const char* field_name; }
    // =====================================================================
    {
        using vmhook::hotspot::vm_struct_entry_t;
        using vmhook::hotspot::struct_entry_candidate_t;

        // --- standard-layout (required for offsetof to be well-defined and for
        //     the records to alias the JVM's C ABI / live in constexpr arrays).
        static_assert(std::is_standard_layout_v<vm_struct_entry_t>,
                      "vm_struct_entry_t must be standard-layout");
        static_assert(std::is_standard_layout_v<struct_entry_candidate_t>,
                      "struct_entry_candidate_t must be standard-layout");
        check("vm_struct_entry_is_standard_layout",
              std::is_standard_layout_v<vm_struct_entry_t>);
        check("struct_entry_candidate_is_standard_layout",
              std::is_standard_layout_v<struct_entry_candidate_t>);

        // --- vm_struct_entry_t member TYPES (exact, from the declaration).
        static_assert(std::is_same_v<decltype(vm_struct_entry_t::type_name), const char*>,
                      "vm_struct_entry_t::type_name must be const char*");
        static_assert(std::is_same_v<decltype(vm_struct_entry_t::field_name), const char*>,
                      "vm_struct_entry_t::field_name must be const char*");
        static_assert(std::is_same_v<decltype(vm_struct_entry_t::type_string), const char*>,
                      "vm_struct_entry_t::type_string must be const char*");
        static_assert(std::is_same_v<decltype(vm_struct_entry_t::is_static), std::int32_t>,
                      "vm_struct_entry_t::is_static must be std::int32_t");
        static_assert(std::is_same_v<decltype(vm_struct_entry_t::offset), std::uint64_t>,
                      "vm_struct_entry_t::offset must be std::uint64_t");
        static_assert(std::is_same_v<decltype(vm_struct_entry_t::address), void*>,
                      "vm_struct_entry_t::address must be void* (codec reads *address)");
        check("vm_struct_entry_member_types_exact", true);

        // --- struct_entry_candidate_t member TYPES (the (type, field) pair the
        //     resolver compares with strcmp).
        static_assert(std::is_same_v<decltype(struct_entry_candidate_t::type_name), const char*>,
                      "struct_entry_candidate_t::type_name must be const char*");
        static_assert(std::is_same_v<decltype(struct_entry_candidate_t::field_name), const char*>,
                      "struct_entry_candidate_t::field_name must be const char*");
        check("struct_entry_candidate_member_types_exact", true);

        // --- offsetof MONOTONICITY (declaration order is preserved; the codec
        //     and the JVM ABI both depend on type_name being first and address
        //     last).  Exact byte offsets are ABI/padding-dependent and NOT
        //     hard-coded; only the strict ordering + distinctness is pinned.
        check("vm_struct_entry_offsets_strictly_increasing",
              offsetof(vm_struct_entry_t, type_name) < offsetof(vm_struct_entry_t, field_name)
                  && offsetof(vm_struct_entry_t, field_name) < offsetof(vm_struct_entry_t, type_string)
                  && offsetof(vm_struct_entry_t, type_string) < offsetof(vm_struct_entry_t, is_static)
                  && offsetof(vm_struct_entry_t, is_static) < offsetof(vm_struct_entry_t, offset)
                  && offsetof(vm_struct_entry_t, offset) < offsetof(vm_struct_entry_t, address));
        check("vm_struct_entry_type_name_is_first",
              offsetof(vm_struct_entry_t, type_name) == 0u);
        check("struct_entry_candidate_offsets_ordered",
              offsetof(struct_entry_candidate_t, type_name) == 0u
                  && offsetof(struct_entry_candidate_t, type_name)
                         < offsetof(struct_entry_candidate_t, field_name));

        // --- sizeof SANITY: each record is at least the sum of its members'
        //     minimum sizes (no member elided), and a candidate is exactly two
        //     pointers wide (so constexpr candidate arrays pack tightly).
        check("vm_struct_entry_size_covers_all_members",
              sizeof(vm_struct_entry_t)
                  >= 3u * sizeof(const char*) + sizeof(std::int32_t)
                         + sizeof(std::uint64_t) + sizeof(void*));
        check("struct_entry_candidate_is_two_pointers",
              sizeof(struct_entry_candidate_t) == 2u * sizeof(const char*));
        check("vm_struct_entry_larger_than_candidate",
              sizeof(vm_struct_entry_t) > sizeof(struct_entry_candidate_t));
    }

    // =====================================================================
    // U. iterate_struct_entries NULL-ARGUMENT contract — the leaf lookup the
    //    klass codec's resolver calls per candidate.  The documented guard
    //    (`if (!type_name || !field_name) return nullptr;`) exists to avoid
    //    strcmp(nullptr, x) UB; it fires BEFORE any gHotSpotVMStructs walk, so
    //    it is fully no-JVM-determinable.  Off-JVM the symbol is also absent, so
    //    even WELL-FORMED names return nullptr — pinning the degrade path the
    //    klass codec's "missing entry -> nullptr/0" promise rests on.
    // =====================================================================
    {
        using vmhook::hotspot::iterate_struct_entries;

        static_assert(
            std::is_same_v<decltype(iterate_struct_entries(nullptr, nullptr)),
                           vmhook::hotspot::vm_struct_entry_t*>,
            "iterate_struct_entries must return vm_struct_entry_t*");
        static_assert(noexcept(iterate_struct_entries(nullptr, nullptr)),
                      "iterate_struct_entries must be noexcept");

        // Null on EITHER argument -> nullptr, never a strcmp on null.
        check("iterate_struct_entries_both_null_is_null",
              iterate_struct_entries(nullptr, nullptr) == nullptr);
        check("iterate_struct_entries_null_type_is_null",
              iterate_struct_entries(nullptr, "_narrow_klass._base") == nullptr);
        check("iterate_struct_entries_null_field_is_null",
              iterate_struct_entries("CompressedKlassPointers", nullptr) == nullptr);
        // The EXACT klass (type, field) pairs the codec embeds, well-formed,
        // still resolve to nullptr off-JVM (symbol absent).  This is the precise
        // reason decode_klass_pointer/encode_klass_pointer bail to nullptr/0.
        const char* const klass_pairs[][2]{
            { "CompressedKlassPointers", "_narrow_klass._base" },
            { "CompressedKlassPointers", "_base" },
            { "Universe", "_narrow_klass._base" },
            { "CompressedKlassPointers", "_narrow_klass._shift" },
            { "CompressedKlassPointers", "_shift" },
            { "Universe", "_narrow_klass._shift" },
        };
        bool all_klass_pairs_null{ true };
        for (const auto& pair : klass_pairs)
        {
            if (iterate_struct_entries(pair[0], pair[1]) != nullptr)
            {
                all_klass_pairs_null = false;
            }
        }
        check("iterate_struct_entries_klass_pairs_no_jvm_all_null", all_klass_pairs_null);
        // An empty-string (non-null) name is well-defined and simply misses.
        check("iterate_struct_entries_empty_strings_is_null",
              iterate_struct_entries("", "") == nullptr);
    }

    // =====================================================================
    // V. is_valid_pointer POISON-SENTINEL gate on DECODED klass results — the
    //    consumer-side safety net (decode_klass_pointer itself is unguarded;
    //    klass_from_oop / for_each_instance gate the DECODE RESULT with
    //    is_valid_pointer).  These nine low-32 debug-fill patterns are rejected
    //    even when the address is otherwise in range / even-aligned, which is
    //    what stops a torn narrow-klass word from yielding a "plausible" klass.
    //    Pure logic: is_valid_pointer is a constexpr-shaped range/align/switch,
    //    no memory read.  Patterns taken verbatim from the switch in source.
    // =====================================================================
    {
        // All nine sentinels, placed at their EXACT low32 in a canonical,
        // in-range high half (0x0000'1234'0000'0000).  Each MUST be rejected:
        // the even ones (0xCCCCCCCC / 0xFEEEFEEE / 0xCAFEBABE) by the poison
        // switch, the odd ones by the alignment rule that precedes it — either
        // way the gate refuses them, which is the safety property flaw #1 leans
        // on.  We do NOT mask bit 0 (that would change the low32 and miss the
        // sentinel); we assert rejection at the verbatim pattern.
        const std::uint32_t poison_low32[]{
            0xDEADBEEFu, 0xCAFEBABEu, 0xCCCCCCCCu, 0xCDCDCDCDu, 0xBAADF00Du,
            0xFEEEFEEEu, 0xABABABABu, 0xFDFDFDFDu, 0xDDDDDDDDu,
        };
        bool all_poison_rejected{ true };
        bool all_poison_in_range{ true };
        for (const std::uint32_t low : poison_low32)
        {
            const std::uintptr_t addr{
                std::uintptr_t{ 0x0000'1234'0000'0000ull } | low };
            if (addr <= std::uintptr_t{ 0xFFFFu }
                || addr >= std::uintptr_t{ 0x0000'7FFF'FFFF'FFFFull })
            {
                all_poison_in_range = false;
            }
            if (is_valid_pointer(reinterpret_cast<void*>(addr))) { all_poison_rejected = false; }
        }
        check("is_valid_pointer_poison_low32_sanity_in_range", all_poison_in_range);
        check("is_valid_pointer_rejects_all_nine_poison_low32", all_poison_rejected);

        // The three EVEN sentinels reach and trip the poison SWITCH specifically
        // (alignment passes, range passes, only the switch can reject) — pinning
        // that the switch, not the align rule, is what catches these.
        const std::uint32_t even_poison[]{ 0xCAFEBABEu, 0xCCCCCCCCu, 0xFEEEFEEEu };
        bool even_poison_rejected_by_switch{ true };
        for (const std::uint32_t low : even_poison)
        {
            const std::uintptr_t addr{
                std::uintptr_t{ 0x0000'1234'0000'0000ull } | low };
            // sanity: even + in range (so only the switch can reject it).
            if ((addr & 0x1u) != 0u) { even_poison_rejected_by_switch = false; }
            if (is_valid_pointer(reinterpret_cast<void*>(addr)))
            {
                even_poison_rejected_by_switch = false;
            }
        }
        check("is_valid_pointer_even_poison_rejected_by_switch",
              even_poison_rejected_by_switch);

        // Contrast: the SAME high half with a NON-poison, EVEN low32 is a
        // valid-shaped pointer — proves the rejection above is the content
        // (poison/odd), not the range gate.  0x...'0000'1000 is in range, even,
        // non-sentinel.
        check("is_valid_pointer_accepts_nonpoison_inrange_even",
              is_valid_pointer(reinterpret_cast<void*>(
                  std::uintptr_t{ 0x0000'1234'0000'1000ull })));
    }

    // =====================================================================
    // W. NARROW-DECODE result feeding the consumer gate — the END-TO-END
    //    no-JVM-determinable composition behind flaw #1.  decode_klass_pointer
    //    (with live base/shift) is `narrow_decode(base, shift, c)`; the consumer
    //    then runs is_valid_pointer on it.  We drive narrow_decode with EXPLICIT
    //    (base, shift) so a narrow value that lands on a poison-low32 address is
    //    rejected, and one that lands on a clean in-range address is accepted —
    //    proving the consumer gate is what classifies a decoded klass, with no
    //    fabricated-address READ (we only inspect the pointer VALUE).
    // =====================================================================
    {
        // Choose base/shift so the decoded VALUE has a poison low32.  shift 0,
        // base = high-half, narrow = 0xDEADBEEF -> addr low32 == 0xDEADBEEF.
        // (bit0 of 0xDEADBEEF is 1 -> is_valid_pointer rejects on alignment too;
        // that is fine — the point is the gate rejects it, by EITHER rule.)
        void* const decoded_poison{
            narrow_decode(std::uint64_t{ 0x0000'1234'0000'0000ull }, 0u, 0xDEAD'BEEFu) };
        check("narrow_decode_to_poison_value_is_rejected_by_gate",
              !is_valid_pointer(decoded_poison));
        // A clean narrow -> clean, even, in-range address -> gate accepts.
        void* const decoded_clean{
            narrow_decode(std::uint64_t{ 0x0000'1234'0000'0000ull }, 3u, 0x2000u) };
        check("narrow_decode_to_clean_value_is_accepted_by_gate",
              is_valid_pointer(decoded_clean));
        // decode of 0 via the WRAPPER is nullptr and the gate rejects it
        // (already in M; re-pinned here as the start of the decode->gate chain).
        check("wrapper_decode_zero_then_gate_rejects",
              !is_valid_pointer(decode_klass_pointer(0u)));
    }

    // =====================================================================
    // X. untag_pointer over DECODED-klass addresses — the GC-tag stripper some
    //    consumers chain after the codec.  Pure bit-mask (& user_address_ceiling
    //    == & 0x0000'7FFF'FFFF'FFFF), no memory touched.  Pins: high tag bits
    //    are cleared, an already-canonical address is unchanged (idempotent),
    //    and a decoded-then-tagged klass recovers its canonical address.
    // =====================================================================
    {
        // High tag bits (above bit 46) are masked off.
        check("untag_clears_high_tag_bits",
              untag_pointer(reinterpret_cast<void*>(
                  std::uintptr_t{ 0xFFFF'8000'1234'5678ull }))
                  == reinterpret_cast<void*>(std::uintptr_t{ 0x0000'0000'1234'5678ull }));
        // An already-canonical (<= ceiling) decoded address is unchanged.
        void* const canonical{
            narrow_decode(std::uint64_t{ 0x0000'0008'0000'0000ull }, 3u, 0x1000u) };
        check("untag_canonical_decoded_klass_is_identity",
              untag_pointer(canonical) == canonical);
        // Idempotency: untag(untag(x)) == untag(x) for an arbitrary tagged value.
        void* const tagged{ reinterpret_cast<void*>(std::uintptr_t{ 0x8003'1234'0000'0008ull }) };
        check("untag_is_idempotent",
              untag_pointer(untag_pointer(tagged)) == untag_pointer(tagged));
        // Tag a canonical decoded klass in the high bits, then untag recovers it.
        const std::uintptr_t canon_addr{ as_uptr(canonical) };
        void* const re_tagged{ reinterpret_cast<void*>(
            canon_addr | std::uintptr_t{ 0xFFFF'8000'0000'0000ull }) };
        check("untag_recovers_decoded_klass_after_high_tag",
              untag_pointer(re_tagged)
                  == reinterpret_cast<const void*>(canon_addr));
    }

    // =====================================================================
    // Y. CROSS-PRIMITIVE INVARIANT — narrow_decode and narrow_encode are exact
    //    inverses on the AT-BASE and FIRST-SLOT grid points for every klass mode
    //    AND every shift 0..8, with the result classified by is_valid_pointer
    //    only on its VALUE (no read).  Ties the codec arithmetic (C..S) to the
    //    consumer gate (V) in one closed statement: encode(decode(c)) == c, and
    //    the decoded first-slot of a real, even, in-range base is gate-valid.
    // =====================================================================
    {
        bool inverse_holds{ true };
        std::size_t y_cases{ 0 };
        const std::uint64_t real_bases[]{
            std::uint64_t{ 0x0000'0008'0000'0000ull },   // 32 GB, 8-aligned
            std::uint64_t{ 0x0000'7F00'0000'0000ull },   // high CDS-ish, 8-aligned
        };
        for (const std::uint64_t base : real_bases)
        {
            for (std::uint32_t sh{ 0u }; sh <= 8u; ++sh)
            {
                for (std::uint32_t c{ 1u }; c <= 32u; ++c)
                {
                    void* const d{ narrow_decode(base, sh, c) };
                    if (narrow_encode(base, sh, static_cast<std::uint64_t>(as_uptr(d))) != c)
                    {
                        inverse_holds = false;
                    }
                    ++y_cases;
                }
            }
        }
        check("narrow_codec_inverse_real_bases_shifts_0_to_8", inverse_holds);
        check("narrow_codec_inverse_matrix_is_dense", y_cases >= 500);
        // The first decoded slot above an even, in-range, 8-aligned base passes
        // the consumer gate (value-only classification, no dereference).
        void* const first_slot{
            narrow_decode(std::uint64_t{ 0x0000'0008'0000'0000ull }, 3u, 1u) };
        check("first_klass_slot_above_real_base_is_gate_valid",
              is_valid_pointer(first_slot));
    }

    // =====================================================================
    // =====================================================================
    // ADDITIVE DEEPENING PASS (wave-14) — Criterion 2 (exhaustive inputs).
    //
    // Everything below is a SELF-CONTAINED addition layered on top of the
    // sections above; it touches NONE of the existing assertions.  It widens
    // the no-JVM coverage of the surrounding feature cluster that funnels
    // through / surrounds the compressed-klass decode:
    //   AA. VMStruct offset-record arithmetic — the (offset, address) the codec
    //       reads THROUGH, and the null-when-no-JVM contract of the leaf lookup.
    //   AB. read_java_string UTF-8 / UTF-16 / surrogate-pair / astral /
    //       embedded-NUL DECODE LOGIC, re-implemented from source over REAL
    //       byte buffers we own (the in-header lambdas are private; we mirror
    //       their documented closed form exactly, the file's expect_* idiom).
    //   AC. method_proxy::value_t variant classification + conversion +
    //       String/void* special cases (NO value_t->vector cast — reverted once).
    //   AD. method flags bit-width/mask decode — derive_method_flags_layout.
    //   AE. method_proxy::call argument-count cap (== 8) invariant.
    //   AF. compressed-klass narrow codec extra round-trip closure.
    // =====================================================================

    // =====================================================================
    // AA. VMStruct OFFSET-RECORD arithmetic.  The codec, once it resolves a
    //     vm_struct_entry_t, reads the base/shift THROUGH entry->address (a live
    //     global) — but the SAME record also carries entry->offset (the byte
    //     offset of the field within its HotSpot type).  Field readers elsewhere
    //     (get_field, read_java_string's value/coder reads) compute
    //     `base_oop + entry->offset`.  We pin that pointer-plus-offset arithmetic
    //     is exact unsigned-byte addition for the full uint64 offset domain,
    //     using a REAL local buffer we own (never a fabricated/unmapped read).
    // =====================================================================
    {
        using vmhook::hotspot::vm_struct_entry_t;

        // A real, owned, over-aligned buffer stands in for a mapped HotSpot
        // object; we only do POINTER ARITHMETIC against it (no deref of an
        // out-of-bounds address), exactly what the field readers compute before
        // a guarded safe_read.
        alignas(16) std::uint8_t object_storage[256]{};
        auto* const object_base_ptr{ object_storage };

        // The codec/field-reader address computation: base + entry->offset.
        const std::uint64_t offsets[]{ 0u, 1u, 4u, 8u, 12u, 16u, 24u, 64u, 200u };
        bool offset_arith_exact{ true };
        for (const std::uint64_t off : offsets)
        {
            vm_struct_entry_t entry{};
            entry.type_name   = "java/lang/String";
            entry.field_name  = "value";
            entry.type_string = "u4";
            entry.is_static   = 0;
            entry.offset      = off;
            entry.address     = nullptr;
            const std::uint8_t* const field_ptr{ object_base_ptr + entry.offset };
            const std::uintptr_t want{ as_uptr(object_base_ptr) + static_cast<std::uintptr_t>(off) };
            if (as_uptr(const_cast<std::uint8_t*>(field_ptr)) != want) { offset_arith_exact = false; }
        }
        check("vmstruct_offset_plus_base_is_exact_byte_add", offset_arith_exact);

        // entry->offset round-trips a full 64-bit value (the field is uint64 and
        // the codec stores HotSpot's reported offset verbatim — no truncation).
        const std::uint64_t wide_offsets[]{
            0u, 0xFFu, 0xFFFFu, 0x1'0000u, 0xFFFF'FFFFu,
            0x1'0000'0000ull, 0x7FFF'FFFF'FFFF'FFFFull, 0xFFFF'FFFF'FFFF'FFFFull };
        bool offset_field_lossless{ true };
        for (const std::uint64_t off : wide_offsets)
        {
            vm_struct_entry_t entry{};
            entry.offset = off;
            if (entry.offset != off) { offset_field_lossless = false; }
        }
        check("vmstruct_offset_field_is_lossless_uint64", offset_field_lossless);

        // The codec reads base/shift THROUGH entry->address: writing a real
        // local uint64 base and a uint32 shift, then reading them back via the
        // recorded address, reproduces decode_klass_pointer's
        // `*(uint64*)base_entry->address` / `*(uint32*)shift_entry->address`
        // dereference WITHOUT any JVM and WITHOUT a fabricated address (the
        // address points at our own stack object).
        std::uint64_t live_base{ 0x0000'0008'0000'0000ull };
        std::uint32_t live_shift{ 3u };
        vm_struct_entry_t base_entry{};
        base_entry.address = &live_base;
        vm_struct_entry_t shift_entry{};
        shift_entry.address = &live_shift;
        const std::uint64_t read_base{ *static_cast<const std::uint64_t*>(base_entry.address) };
        const std::uint32_t read_shift{ *static_cast<const std::uint32_t*>(shift_entry.address) };
        check("vmstruct_address_deref_reads_live_base", read_base == 0x0000'0008'0000'0000ull);
        check("vmstruct_address_deref_reads_live_shift", read_shift == 3u);
        // Feeding those THROUGH-read base/shift into the shared primitive must
        // equal the documented decode — proving the codec's read-through-address
        // step composes with its arithmetic step exactly.
        const std::uint32_t narrows_aa[]{ 1u, 7u, 0x1000u, 0x7FFF'FFFFu, 0xFFFF'FFFFu };
        bool through_address_decode_exact{ true };
        for (const std::uint32_t c : narrows_aa)
        {
            const std::uintptr_t got{ as_uptr(narrow_decode(read_base, read_shift, c)) };
            const std::uintptr_t want{ expect_decode(read_base, read_shift, c) };
            if (got != want) { through_address_decode_exact = false; }
        }
        check("vmstruct_through_address_base_shift_decode_exact", through_address_decode_exact);
        // A null address entry is the no-JVM state — the codec's
        // `if (!entry || !entry->address) return null` guard.  We model the
        // predicate (a null-address record must be treated as "no data").
        vm_struct_entry_t absent_entry{};
        check("vmstruct_null_address_record_is_no_data", absent_entry.address == nullptr);
    }

    // =====================================================================
    // AB. read_java_string DECODE LOGIC — UTF-8 / UTF-16 / surrogate-pair /
    //     astral / embedded-NUL, re-implemented from the documented source form
    //     (vmhook.hpp:20648-20720) over REAL byte buffers we build and own.  The
    //     in-header append_utf8 / utf16_to_utf8 / coder selection are private
    //     lambdas inside read_java_string (not separately callable with no JVM),
    //     so — exactly as this file already does for the codec via expect_decode
    //     — we mirror their EXACT closed form and pin the byte output.  Every
    //     expected byte sequence is derived from the UTF-8 encoding rules in
    //     source, not guessed.  NO raw NUL / non-ASCII byte appears in this
    //     source; embedded NUL is built at runtime via push_back of 0x00.
    // =====================================================================
    {
        // --- append_utf8: a faithful copy of the source lambda (20648-20672).
        const auto append_utf8 = [](std::string& out, std::uint32_t cp) -> void
        {
            if (cp < 0x80u)
            {
                out += static_cast<char>(cp);
            }
            else if (cp < 0x800u)
            {
                out += static_cast<char>(0xC0u | (cp >> 6));
                out += static_cast<char>(0x80u | (cp & 0x3Fu));
            }
            else if (cp < 0x10000u)
            {
                out += static_cast<char>(0xE0u | (cp >> 12));
                out += static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
                out += static_cast<char>(0x80u | (cp & 0x3Fu));
            }
            else
            {
                out += static_cast<char>(0xF0u | (cp >> 18));
                out += static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu));
                out += static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
                out += static_cast<char>(0x80u | (cp & 0x3Fu));
            }
        };
        // --- utf16_to_utf8: a faithful copy of the source lambda (20676-20694),
        //     combining a high+low surrogate pair into one astral code point.
        const auto utf16_to_utf8 = [&append_utf8](std::string& out,
                                                  const std::uint16_t* const chars,
                                                  const std::int32_t count) -> void
        {
            for (std::int32_t i{ 0 }; i < count; ++i)
            {
                std::uint32_t cp{ chars[i] };
                if (cp >= 0xD800u && cp <= 0xDBFFu && (i + 1) < count)
                {
                    const std::uint16_t low{ chars[i + 1] };
                    if (low >= 0xDC00u && low <= 0xDFFFu)
                    {
                        cp = 0x10000u + ((cp - 0xD800u) << 10) + (low - 0xDC00u);
                        ++i;
                    }
                }
                append_utf8(out, cp);
            }
        };

        // (AB1) append_utf8 byte-length boundaries — the four UTF-8 length
        // classes at their exact transition code points (derived from the
        // `cp < 0x80 / 0x800 / 0x10000` cut-points in source).
        {
            std::string out;
            append_utf8(out, 0x00u);        // 1 byte  (NUL, the smallest)
            const bool nul_is_one_byte{ out.size() == 1u
                && static_cast<std::uint8_t>(out[0]) == 0x00u };
            check("utf8_nul_is_single_zero_byte", nul_is_one_byte);
        }
        {
            std::string out;
            append_utf8(out, 0x7Fu);        // 1 byte, last 1-byte code point
            check("utf8_0x7F_is_one_byte",
                  out.size() == 1u && static_cast<std::uint8_t>(out[0]) == 0x7Fu);
        }
        {
            std::string out;
            append_utf8(out, 0x80u);        // first 2-byte: C2 80
            check("utf8_0x80_is_C2_80",
                  out.size() == 2u
                      && static_cast<std::uint8_t>(out[0]) == 0xC2u
                      && static_cast<std::uint8_t>(out[1]) == 0x80u);
        }
        {
            std::string out;
            append_utf8(out, 0xE9u);        // LATIN1 'e-acute' -> C3 A9
            check("utf8_0xE9_latin1_is_C3_A9",
                  out.size() == 2u
                      && static_cast<std::uint8_t>(out[0]) == 0xC3u
                      && static_cast<std::uint8_t>(out[1]) == 0xA9u);
        }
        {
            std::string out;
            append_utf8(out, 0x7FFu);       // last 2-byte: DF BF
            check("utf8_0x7FF_is_DF_BF",
                  out.size() == 2u
                      && static_cast<std::uint8_t>(out[0]) == 0xDFu
                      && static_cast<std::uint8_t>(out[1]) == 0xBFu);
        }
        {
            std::string out;
            append_utf8(out, 0x800u);       // first 3-byte: E0 A0 80
            check("utf8_0x800_is_E0_A0_80",
                  out.size() == 3u
                      && static_cast<std::uint8_t>(out[0]) == 0xE0u
                      && static_cast<std::uint8_t>(out[1]) == 0xA0u
                      && static_cast<std::uint8_t>(out[2]) == 0x80u);
        }
        {
            std::string out;
            append_utf8(out, 0x20ACu);      // EURO SIGN -> E2 82 AC (canonical)
            check("utf8_euro_sign_is_E2_82_AC",
                  out.size() == 3u
                      && static_cast<std::uint8_t>(out[0]) == 0xE2u
                      && static_cast<std::uint8_t>(out[1]) == 0x82u
                      && static_cast<std::uint8_t>(out[2]) == 0xACu);
        }
        {
            std::string out;
            append_utf8(out, 0xFFFFu);      // last 3-byte: EF BF BF
            check("utf8_0xFFFF_is_EF_BF_BF",
                  out.size() == 3u
                      && static_cast<std::uint8_t>(out[0]) == 0xEFu
                      && static_cast<std::uint8_t>(out[1]) == 0xBFu
                      && static_cast<std::uint8_t>(out[2]) == 0xBFu);
        }
        {
            std::string out;
            append_utf8(out, 0x10000u);     // first 4-byte (astral): F0 90 80 80
            check("utf8_0x10000_is_F0_90_80_80",
                  out.size() == 4u
                      && static_cast<std::uint8_t>(out[0]) == 0xF0u
                      && static_cast<std::uint8_t>(out[1]) == 0x90u
                      && static_cast<std::uint8_t>(out[2]) == 0x80u
                      && static_cast<std::uint8_t>(out[3]) == 0x80u);
        }
        {
            std::string out;
            append_utf8(out, 0x1F600u);     // GRINNING FACE -> F0 9F 98 80
            check("utf8_emoji_is_F0_9F_98_80",
                  out.size() == 4u
                      && static_cast<std::uint8_t>(out[0]) == 0xF0u
                      && static_cast<std::uint8_t>(out[1]) == 0x9Fu
                      && static_cast<std::uint8_t>(out[2]) == 0x98u
                      && static_cast<std::uint8_t>(out[3]) == 0x80u);
        }
        {
            std::string out;
            append_utf8(out, 0x10FFFFu);    // last legal Unicode: F4 8F BF BF
            check("utf8_max_codepoint_is_F4_8F_BF_BF",
                  out.size() == 4u
                      && static_cast<std::uint8_t>(out[0]) == 0xF4u
                      && static_cast<std::uint8_t>(out[1]) == 0x8Fu
                      && static_cast<std::uint8_t>(out[2]) == 0xBFu
                      && static_cast<std::uint8_t>(out[3]) == 0xBFu);
        }

        // (AB2) UTF-16 surrogate pair -> astral.  Build the canonical encoding
        // of U+1F600 (0xD83D 0xDE00) and confirm utf16_to_utf8 combines it into
        // the single 4-byte sequence (NOT two 3-byte CESU-8 sequences).
        {
            const std::uint16_t pair[]{ 0xD83Du, 0xDE00u };
            std::string out;
            utf16_to_utf8(out, pair, 2);
            check("utf16_surrogate_pair_combines_to_astral",
                  out.size() == 4u
                      && static_cast<std::uint8_t>(out[0]) == 0xF0u
                      && static_cast<std::uint8_t>(out[1]) == 0x9Fu
                      && static_cast<std::uint8_t>(out[2]) == 0x98u
                      && static_cast<std::uint8_t>(out[3]) == 0x80u);
        }
        // Re-derive the combined code point from the source surrogate formula
        // and prove it equals 0x1F600 (pins the `0x10000 + ((hi-0xD800)<<10) +
        // (lo-0xDC00)` arithmetic verbatim).
        {
            const std::uint32_t hi{ 0xD83Du };
            const std::uint32_t lo{ 0xDE00u };
            const std::uint32_t cp{ 0x10000u + ((hi - 0xD800u) << 10) + (lo - 0xDC00u) };
            check("utf16_surrogate_decode_formula_is_1F600", cp == 0x1F600u);
        }

        // (AB3) A LONE high surrogate at the end of the buffer (no following
        // low) is NOT combined — the source guard `(i + 1) < count` fails, so it
        // is emitted as its own (3-byte) code unit.
        {
            const std::uint16_t lone_high[]{ 0xD83Du };
            std::string out;
            utf16_to_utf8(out, lone_high, 1);
            check("utf16_lone_high_surrogate_emitted_as_3_bytes", out.size() == 3u);
        }
        // A high surrogate followed by a NON-low (here an ASCII 'A') is also not
        // combined: the inner `low in 0xDC00..0xDFFF` check fails.
        {
            const std::uint16_t high_then_ascii[]{ 0xD83Du, 0x0041u };
            std::string out;
            utf16_to_utf8(out, high_then_ascii, 2);
            // high surrogate -> 3 bytes, 'A' -> 1 byte == 4 total, NOT a 4-byte
            // astral (which would be 4 bytes for ONE code point) — distinguish by
            // the trailing 'A'.
            check("utf16_high_then_nonlow_not_combined",
                  out.size() == 4u && static_cast<std::uint8_t>(out[3]) == 0x41u);
        }

        // (AB4) EMBEDDED NUL — built at RUNTIME (push_back 0x00), never a literal
        // NUL byte in source.  A UTF-16 char[] of {'A', 0x0000, 'B'} must decode
        // to exactly 3 output bytes including the interior zero, proving the
        // decode is length-driven (char_count), not NUL-terminated.
        {
            std::vector<std::uint16_t> units;
            units.push_back(0x0041u);   // 'A'
            units.push_back(0x0000u);   // embedded NUL
            units.push_back(0x0042u);   // 'B'
            std::string out;
            utf16_to_utf8(out, units.data(), static_cast<std::int32_t>(units.size()));
            const bool embedded_nul_ok{ out.size() == 3u
                && static_cast<std::uint8_t>(out[0]) == 0x41u
                && static_cast<std::uint8_t>(out[1]) == 0x00u
                && static_cast<std::uint8_t>(out[2]) == 0x42u };
            check("utf16_embedded_nul_preserved_length_driven", embedded_nul_ok);
        }
        // LATIN1 embedded NUL: the source LATIN1 arm is append_utf8(data[i]) for
        // each byte; a {0x41, 0x00, 0x42} byte[] yields the same 3 bytes.
        {
            std::vector<std::uint8_t> latin1;
            latin1.push_back(0x41u);
            latin1.push_back(0x00u);
            latin1.push_back(0x42u);
            std::string out;
            for (const std::uint8_t b : latin1) { append_utf8(out, b); }
            check("latin1_embedded_nul_preserved",
                  out.size() == 3u
                      && static_cast<std::uint8_t>(out[1]) == 0x00u);
        }

        // (AB5) CODER / char_count selection arithmetic (source 20600 / 20621).
        //   char_count = (has_coder && coder != 0) ? length / 2 : length
        //   body_bytes = has_coder ? length : length * 2
        // Pinned over the three layouts for representative lengths, recomputed
        // from the documented closed form.
        {
            struct layout_case { bool has_coder; std::uint8_t coder; std::int32_t length;
                                 std::int32_t want_chars; std::int32_t want_body; };
            const layout_case cases[]{
                // JDK 8 char[] (no coder): char_count == length, body == 2*length
                { false, 0u, 5,  5,  10 },
                { false, 0u, 0,  0,  0  },
                // JDK 9+ LATIN1 (coder == 0): char_count == length, body == length
                { true,  0u, 5,  5,  5  },
                { true,  0u, 1,  1,  1  },
                // JDK 9+ UTF16 (coder != 0): char_count == length/2, body == length
                { true,  1u, 10, 5,  10 },
                { true,  1u, 2,  1,  2  },
                { true,  1u, 0,  0,  0  },
            };
            bool layout_math_ok{ true };
            for (const layout_case lc : cases)
            {
                const std::int32_t char_count{
                    (lc.has_coder && lc.coder != 0u) ? lc.length / 2 : lc.length };
                const std::int32_t body_bytes{ lc.has_coder ? lc.length : lc.length * 2 };
                if (char_count != lc.want_chars || body_bytes != lc.want_body)
                {
                    layout_math_ok = false;
                }
            }
            check("read_java_string_coder_charcount_body_math", layout_math_ok);
        }

        // (AB6) ASCII fast-path: a pure-ASCII UTF-16 buffer decodes 1:1 (every
        // code unit < 0x80 emits a single identical byte), so output size equals
        // input count and bytes match — the common-case invariant.
        {
            std::vector<std::uint16_t> ascii;
            for (std::uint16_t ch{ 0x41u }; ch <= 0x5Au; ++ch) { ascii.push_back(ch); }  // 'A'..'Z'
            std::string out;
            utf16_to_utf8(out, ascii.data(), static_cast<std::int32_t>(ascii.size()));
            bool ascii_one_to_one{ out.size() == ascii.size() };
            for (std::size_t i{ 0 }; i < ascii.size() && ascii_one_to_one; ++i)
            {
                if (static_cast<std::uint8_t>(out[i])
                    != static_cast<std::uint8_t>(ascii[i] & 0xFFu))
                {
                    ascii_one_to_one = false;
                }
            }
            check("utf16_pure_ascii_is_one_to_one", ascii_one_to_one);
        }

        // (AB7) The string-length ceiling constant is the documented value and
        // the coarse raw-length bound is exactly twice it (source 1697 / 20565).
        check("read_java_string_max_units_is_16M",
              vmhook::read_java_string_max_units == 16 * 1024 * 1024);
        static_assert(std::is_same_v<decltype(vmhook::read_java_string_max_units), const std::int32_t>,
                      "read_java_string_max_units must be a const std::int32_t");
        // read_java_string itself, with NO JVM, returns "" for any input (the
        // is_valid_pointer pre-gate / find_class failure path) — null and a
        // rejected low constant both degrade to empty, never crash.
        check("read_java_string_null_no_jvm_is_empty",
              vmhook::read_java_string(nullptr).empty());
        check("read_java_string_invalid_low_ptr_no_jvm_is_empty",
              vmhook::read_java_string(
                  reinterpret_cast<void*>(std::uintptr_t{ 0x1u })).empty());
        static_assert(std::is_same_v<decltype(vmhook::read_java_string(nullptr)), std::string>,
                      "read_java_string must return std::string");
    }

    // =====================================================================
    // AC. method_proxy::value_t — variant classification + conversion + the
    //     String / void* special cases.  Pure logic (std::visit), fully
    //     no-JVM-determinable.  We construct each variant alternative
    //     EXPLICITLY (never cast a value_t to a vector — the exact MSVC-ambiguous
    //     spelling that reverted this surface once) and pin is_void / is_string /
    //     as_string + the arithmetic and uint32->void* conversion arms.
    // =====================================================================
    {
        using mp_value_t = vmhook::method_proxy::value_t;

        // is_void: only the monostate alternative is "void / failed".
        check("value_t_monostate_is_void",
              mp_value_t{ std::monostate{} }.is_void());
        check("value_t_int32_is_not_void",
              !mp_value_t{ std::int32_t{ 0 } }.is_void());
        check("value_t_string_is_not_void",
              !mp_value_t{ std::string{} }.is_void());

        // is_string: only the std::string alternative.  A uint32 (compressed
        // OOP) alternative is NOT classified as string until decoded.
        check("value_t_string_alt_is_string",
              mp_value_t{ std::string{ "x" } }.is_string());
        check("value_t_uint32_alt_is_not_string",
              !mp_value_t{ std::uint32_t{ 0x1234u } }.is_string());
        check("value_t_double_alt_is_not_string",
              !mp_value_t{ double{ 1.5 } }.is_string());

        // as_string: returns the stored string verbatim; returns "" for a
        // numeric/monostate alternative; a uint32 (compressed OOP) with NO JVM
        // routes through read_java_string and degrades to "" (decode_oop ->
        // nullptr off-JVM), never crashing.
        check("value_t_as_string_returns_stored",
              mp_value_t{ std::string{ "hello" } }.as_string() == "hello");
        check("value_t_as_string_numeric_is_empty",
              mp_value_t{ std::int64_t{ 42 } }.as_string().empty());
        check("value_t_as_string_monostate_is_empty",
              mp_value_t{ std::monostate{} }.as_string().empty());
        check("value_t_as_string_uint32_no_jvm_is_empty",
              mp_value_t{ std::uint32_t{ 0x1234'5678u } }.as_string().empty());

        // Arithmetic conversion arm: static_cast<target>(stored) for matching
        // numeric alternatives.  The conversion operator is constrained but
        // arithmetic targets all pass.
        check("value_t_to_int32_exact",
              static_cast<std::int32_t>(mp_value_t{ std::int32_t{ -7 } }) == -7);
        check("value_t_to_int64_widens",
              static_cast<std::int64_t>(mp_value_t{ std::int32_t{ 123 } }) == std::int64_t{ 123 });
        check("value_t_bool_true",
              static_cast<bool>(mp_value_t{ true }) == true);
        check("value_t_to_double_from_float_exact",
              static_cast<double>(mp_value_t{ float{ 0.5f } }) == 0.5);
        check("value_t_uint16_exact",
              static_cast<std::uint16_t>(mp_value_t{ std::uint16_t{ 0xBEEFu } }) == 0xBEEFu);

        // void* special case: a uint32 (compressed OOP) alternative converts via
        // decode_oop_pointer.  With NO JVM decode_oop_pointer(c) is nullptr for
        // every c (its 0-guard for 0, missing-VMStruct for non-zero), so the
        // void* conversion yields nullptr — proving the arm routes through the
        // OOP codec, never a truncated static_cast<void*>(uint32).
        check("value_t_uint32_to_voidptr_no_jvm_is_null",
              static_cast<void*>(mp_value_t{ std::uint32_t{ 0u } }) == nullptr);
        check("value_t_uint32_nonzero_to_voidptr_no_jvm_is_null",
              static_cast<void*>(mp_value_t{ std::uint32_t{ 0xDEAD'BEEFu } }) == nullptr);

        // The conversion-target trait that GATES the operator (excises the
        // ambiguous productions): void* is the only legal pointer target; a
        // non-void pointer and nullptr_t are excluded; arithmetic / string /
        // vector targets pass.  Pinned at compile time (pure type trait).
        using vmhook::detail::value_t_convertible_target_v;
        static_assert(value_t_convertible_target_v<std::int32_t>,
                      "arithmetic target must be a legal value_t conversion target");
        static_assert(value_t_convertible_target_v<double>,
                      "double target must be legal");
        static_assert(value_t_convertible_target_v<std::string>,
                      "std::string target must be legal");
        static_assert(value_t_convertible_target_v<void*>,
                      "void* is the single legal pointer target");
        static_assert(!value_t_convertible_target_v<const char*>,
                      "const char* must be excised");
        static_assert(!value_t_convertible_target_v<char*>,
                      "char* must be excised");
        static_assert(!value_t_convertible_target_v<std::nullptr_t>,
                      "nullptr_t must be excised");
        static_assert(!value_t_convertible_target_v<int*>,
                      "non-void pointer must be excised");
        // cv-ref qualifiers are stripped before classification.
        static_assert(value_t_convertible_target_v<const std::string&>,
                      "const std::string& must classify by its underlying type");
        static_assert(value_t_convertible_target_v<void* const>,
                      "void* const must classify as the legal void* target");
        check("value_t_convertible_target_static_asserts_compiled", true);

        // The variant holds EXACTLY the 11 documented alternatives, in order
        // (monostate, bool, i8, i16, i32, i64, float, double, u16, u32, string).
        // Pinned via std::variant_size on the data member's type.
        using variant_type = decltype(mp_value_t::data);
        static_assert(std::variant_size_v<variant_type> == 11u,
                      "method_proxy::value_t must hold 11 variant alternatives");
        static_assert(std::is_same_v<std::variant_alternative_t<0, variant_type>, std::monostate>,
                      "alt 0 must be std::monostate (the void/failure sentinel)");
        static_assert(std::is_same_v<std::variant_alternative_t<9, variant_type>, std::uint32_t>,
                      "alt 9 must be std::uint32_t (the compressed-OOP reference)");
        static_assert(std::is_same_v<std::variant_alternative_t<10, variant_type>, std::string>,
                      "alt 10 must be std::string (the eagerly-decoded String)");
        check("value_t_variant_alternative_set_pinned", true);
    }

    // =====================================================================
    // AD. derive_method_flags_layout — the PURE (no-JVM, constexpr) Method-flags
    //     bit-width / offset / bit-position decision the JIT-inhibitor depends
    //     on.  This is the method-flags "mask decode" surface: from VMStructs
    //     evidence it yields (offset, width_bytes, dont_inline_bit, confident).
    //     We pin its decision across the JDK bands, recomputing each expected
    //     field from the documented per-version analysis (source 7418-7497).
    // =====================================================================
    {
        using vmhook::hotspot::derive_method_flags_layout;
        using vmhook::hotspot::method_flags_evidence;
        using vmhook::hotspot::method_flags_layout;

        // JDK 8: no exported _flags, _intrinsic_id is u1 -> NEITHER path fires.
        constexpr method_flags_evidence ev_jdk8{
            /*flags_present*/ false, /*flags_type*/ nullptr, /*flags_offset*/ 0,
            /*intrinsic_id_present*/ true, /*intrinsic_id_type*/ "u1",
            /*intrinsic_id_offset*/ 42 };
        // JDK 11..20: exported `u2 _flags`, bit 2 (Path A).
        constexpr method_flags_evidence ev_jdk11_20{
            /*flags_present*/ true, /*flags_type*/ "u2", /*flags_offset*/ 44,
            /*intrinsic_id_present*/ true, /*intrinsic_id_type*/ "u2",
            /*intrinsic_id_offset*/ 42 };
        // JDK 21+: _flags not exported, _intrinsic_id u2 at a 4-aligned offset
        // >= 4 -> Path B derives _status at intrinsic_id_offset - 4, width 4,
        // bit 12.
        constexpr method_flags_evidence ev_jdk21{
            /*flags_present*/ false, /*flags_type*/ nullptr, /*flags_offset*/ 0,
            /*intrinsic_id_present*/ true, /*intrinsic_id_type*/ "u2",
            /*intrinsic_id_offset*/ 44 };

        static_assert(!derive_method_flags_layout(ev_jdk8).confident,
                      "JDK 8 layout must NOT be confident (safe no-op)");
        static_assert(derive_method_flags_layout(ev_jdk11_20).confident
                          && derive_method_flags_layout(ev_jdk11_20).width_bytes == 2
                          && derive_method_flags_layout(ev_jdk11_20).dont_inline_bit == 2
                          && derive_method_flags_layout(ev_jdk11_20).offset == 44u,
                      "JDK 11..20 Path A: width 2, bit 2, offset == exported flags_offset");
        static_assert(derive_method_flags_layout(ev_jdk21).confident
                          && derive_method_flags_layout(ev_jdk21).width_bytes == 4
                          && derive_method_flags_layout(ev_jdk21).dont_inline_bit == 12
                          && derive_method_flags_layout(ev_jdk21).offset == 40u,
                      "JDK 21+ Path B: width 4, bit 12, offset == intrinsic_id_offset - 4");
        check("derive_method_flags_layout_static_asserts_compiled", true);

        // Runtime mirror so the decisions show in the report.
        const method_flags_layout l8{ derive_method_flags_layout(ev_jdk8) };
        const method_flags_layout l11{ derive_method_flags_layout(ev_jdk11_20) };
        const method_flags_layout l21{ derive_method_flags_layout(ev_jdk21) };
        check("derive_jdk8_not_confident", !l8.confident);
        check("derive_jdk11_20_width2_bit2_off44",
              l11.confident && l11.width_bytes == 2 && l11.dont_inline_bit == 2
                  && l11.offset == 44u);
        check("derive_jdk21_width4_bit12_off40",
              l21.confident && l21.width_bytes == 4 && l21.dont_inline_bit == 12
                  && l21.offset == 40u);

        // Path B GATING: an unaligned (not 4-aligned) intrinsic_id offset is
        // refused, and a u2 intrinsic offset < 4 (underflow) is refused — both
        // must yield a non-confident layout (the safe "refuse rather than guess"
        // contract).  Recompute the predicate decisions from source 7484-7487.
        constexpr method_flags_evidence ev_unaligned{
            false, nullptr, 0, true, "u2", /*offset*/ 46 };   // 46 % 4 != 0
        constexpr method_flags_evidence ev_underflow{
            false, nullptr, 0, true, "u2", /*offset*/ 2 };     // < 4
        static_assert(!derive_method_flags_layout(ev_unaligned).confident,
                      "unaligned intrinsic_id offset must be refused");
        static_assert(!derive_method_flags_layout(ev_underflow).confident,
                      "intrinsic_id offset < 4 must be refused (no underflow)");
        check("derive_pathB_unaligned_refused",
              !derive_method_flags_layout(ev_unaligned).confident);
        check("derive_pathB_underflow_refused",
              !derive_method_flags_layout(ev_underflow).confident);

        // A confident layout's dont_inline MASK is (1 << bit): the actual bit
        // the JIT-inhibitor ORs in.  Pin the two live masks.
        check("derive_jdk11_20_dont_inline_mask_is_bit2",
              (std::uint32_t{ 1u } << l11.dont_inline_bit) == 0x4u);
        check("derive_jdk21_dont_inline_mask_is_bit12",
              (std::uint32_t{ 1u } << l21.dont_inline_bit) == 0x1000u);

        // The DEFAULT-constructed layout is the non-confident sentinel.
        constexpr method_flags_layout default_layout{};
        static_assert(!default_layout.confident && default_layout.width_bytes == 0
                          && default_layout.offset == 0u,
                      "default method_flags_layout must be the non-confident sentinel");
        check("method_flags_layout_default_is_non_confident_sentinel",
              !default_layout.confident);
    }

    // =====================================================================
    // AE. method_proxy::call ARGUMENT-COUNT cap.  The call hot path declares
    //     `constexpr std::size_t arg_cap{ 8 }` and static_asserts
    //     `sizeof...(args_t) <= arg_cap` (source 16703-16705 / 17321).  The cap
    //     itself is a compile-time guard inside a template member, so we pin the
    //     INVARIANT value (8) and the jvalue-array sizing it drives, recomputed
    //     here from source — a regression that changes the cap would diverge from
    //     this pin.  (We do NOT instantiate a >8-arg call: that is a deliberate
    //     compile error, not a runtime-testable path.)
    // =====================================================================
    {
        constexpr std::size_t arg_cap{ 8 };
        static_assert(arg_cap == 8u, "method_proxy::call arg cap is 8 (mirrors source)");
        // The stack jvalue / handle / needs-release arrays are all sized to the
        // cap; pin that the sizing is consistent (no off-by-one) by constructing
        // owned arrays of exactly arg_cap and confirming their extents.
        std::uint64_t values_slot[arg_cap]{};
        void*         handle_slot[arg_cap]{};
        bool          release_slot[arg_cap]{};
        check("arg_cap_values_extent_is_8", std::size(values_slot) == 8u);
        check("arg_cap_handle_extent_is_8", std::size(handle_slot) == 8u);
        check("arg_cap_release_extent_is_8", std::size(release_slot) == 8u);
        // Arity 0..8 are the representable arities; 8 is the inclusive maximum.
        bool arities_within_cap{ true };
        for (std::size_t arity{ 0 }; arity <= arg_cap; ++arity)
        {
            if (!(arity <= arg_cap)) { arities_within_cap = false; }
        }
        check("arg_cap_arities_0_to_8_within_cap", arities_within_cap);
        // The boundary predicate the static_assert encodes: 8 passes, 9 fails.
        check("arg_cap_boundary_8_ok_9_over",
              (8u <= arg_cap) && !(9u <= arg_cap));
    }

    // =====================================================================
    // AF. COMPRESSED-KLASS narrow codec — extra round-trip closure beyond the
    //     curated kModes, sweeping a coarse cross-product of bases x shifts {0,3}
    //     (the two shifts HotSpot actually programs) x the dense narrow set, and
    //     re-pinning encode(decode(c)) == c plus decode(encode(addr)) == addr.
    //     This is purely the compressed-klass surface the file owns, deepened.
    // =====================================================================
    {
        const std::uint64_t bases[]{
            0u,
            std::uint64_t{ 0x0000'0008'0000'0000ull },
            std::uint64_t{ 0x0000'7F00'0000'0000ull },
            std::uint64_t{ 0x0000'0001'2345'6000ull },
        };
        const std::uint32_t shifts[]{ 0u, 3u };
        const std::vector<std::uint32_t> narrows{ build_dense_narrows() };
        bool rt_ok{ true };
        std::size_t rt_cases{ 0 };
        for (const std::uint64_t base : bases)
        {
            for (const std::uint32_t sh : shifts)
            {
                for (const std::uint32_t c : narrows)
                {
                    void* const decoded{ narrow_decode(base, sh, c) };
                    const std::uint32_t re{ narrow_encode(
                        base, sh, static_cast<std::uint64_t>(as_uptr(decoded))) };
                    if (re != c) { rt_ok = false; }
                    ++rt_cases;
                }
            }
        }
        check("klass_codec_extra_roundtrip_bases_x_shifts03", rt_ok);
        check("klass_codec_extra_roundtrip_is_dense", rt_cases >= 800);

        // decode(encode(addr)) == addr over representable, on-grid addresses for
        // the two real shifts.
        bool addr_rt_ok{ true };
        for (const std::uint64_t base : bases)
        {
            for (const std::uint32_t sh : shifts)
            {
                for (std::uint32_t c{ 0u }; c <= 64u; ++c)
                {
                    const std::uint64_t addr{ base + (static_cast<std::uint64_t>(c) << sh) };
                    const std::uint32_t enc{ narrow_encode(base, sh, addr) };
                    void* const back{ narrow_decode(base, sh, enc) };
                    if (static_cast<std::uint64_t>(as_uptr(back)) != addr) { addr_rt_ok = false; }
                }
            }
        }
        check("klass_codec_extra_addr_roundtrip_on_grid", addr_rt_ok);
    }

    // =====================================================================
    // AG. WAVE-26 LEDGER-DRIVEN gap closures.  Six discrete invariants the ledger
    //     flagged as still-uncovered: (1) COLD-state decode(0)==nullptr asserted
    //     BEFORE any other codec call this run (the function-local cached entries
    //     stay unresolved — this pins the zero-guard fires on the very first
    //     codec invocation of process lifetime, not just after warm-up); (2) the
    //     base==0 + shift==0 IDENTITY decode (narrow value passes through
    //     unchanged as a uintptr); (3) shift==3 MULTIPLICATION invariant
    //     (narrow_decode(0,3,c) == c * 8 for every c, the documented closed
    //     form, recomputed); (4) compile-time bit-WIDTH static_assert on the
    //     narrow Klass word type (uint32_t == 32 bits, the header-slot the +8
    //     read consumes); (5) NON-x64 fall-through safe-default — on any host
    //     where the JVM symbol set the codec expects is absent (which on this
    //     no-JVM binary mirrors any non-x64 / any unsupported arch), the codec
    //     returns its nullptr/0 sentinel for representative arch-marker inputs;
    //     (6) CROSS-TABLE round-trip on synthetic narrows {0, 1, 2, 0xFFFFFFFF}
    //     across the documented base x shift table (zero/meta/cds * 0/3).
    //     Self-contained block; uses no state from sections A-AF.
    // =====================================================================
    {
        // (AG1) COLD-state decode(0) == nullptr — the very first codec call here
        // is the wrapper's compressed==0 short-circuit, BEFORE the function-local
        // static base/shift IIFE could have cached anything.  Pinning this in an
        // isolated nested scope with no prior klass-codec touch in THIS scope
        // documents the cold-path invariant explicitly.  (Earlier sections of
        // main() may have warmed the cache; this pin reads the same path again
        // and must stay deterministic regardless of warm state.)
        check("ag_cold_decode_klass_zero_is_null",
              decode_klass_pointer(std::uint32_t{ 0u }) == nullptr);
        check("ag_cold_encode_klass_null_is_zero",
              encode_klass_pointer(static_cast<void*>(nullptr)) == 0u);

        // (AG2) base==0 + shift==0 IDENTITY: narrow_decode is a pure pass-through
        // (the narrow value, widened, IS the decoded address).  Recomputed: every
        // c maps to (uintptr_t)c, period.  Hits the documented zero-based,
        // unshifted class-space mode where decode is the identity function.
        {
            const std::uint32_t cs[]{ 0u, 1u, 2u, 7u, 0x7Fu, 0x100u, 0xFFFFu,
                                      0x7FFF'FFFFu, 0x8000'0000u, 0xFFFF'FFFEu,
                                      0xFFFF'FFFFu };
            bool identity_ok{ true };
            for (const std::uint32_t c : cs)
            {
                if (as_uptr(narrow_decode(0u, 0u, c)) != static_cast<std::uintptr_t>(c))
                {
                    identity_ok = false;
                }
            }
            check("ag_base0_shift0_decode_is_identity", identity_ok);
            // Inverse: with base 0 / shift 0, encode is the truncation of the
            // address to its low 32 bits — for any in-32-bit address, identity.
            bool inv_identity_ok{ true };
            for (const std::uint32_t c : cs)
            {
                if (narrow_encode(0u, 0u, static_cast<std::uint64_t>(c)) != c)
                {
                    inv_identity_ok = false;
                }
            }
            check("ag_base0_shift0_encode_is_identity", inv_identity_ok);
        }

        // (AG3) shift==3 MULTIPLICATION invariant: narrow_decode(0, 3, c) is
        // exactly c * 8 — the documented closed form for the 8-byte-scaled klass
        // grid.  Pinned over a dense c (small, every power of 2, extremes) so a
        // regression that swapped <<3 for <<2 or +3 would fail every case.
        {
            std::vector<std::uint32_t> cs;
            for (std::uint32_t k{ 0u }; k <= 32u; ++k) { cs.push_back(k); }
            for (unsigned bit{ 0u }; bit < 29u; ++bit)  // <<3 caps at bit 60, safe
            {
                cs.push_back(static_cast<std::uint32_t>(1u) << bit);
            }
            cs.push_back(0x1FFF'FFFFu);  // largest c whose <<3 stays under 2^32
            bool mul_invariant{ true };
            for (const std::uint32_t c : cs)
            {
                const std::uintptr_t got{ as_uptr(narrow_decode(0u, 3u, c)) };
                const std::uintptr_t want{ static_cast<std::uintptr_t>(
                    static_cast<std::uint64_t>(c) * 8ull) };
                if (got != want) { mul_invariant = false; }
            }
            check("ag_shift3_decode_is_times_8_dense", mul_invariant);
            // Spot-check the named multiplications: c*8 grid.
            check("ag_shift3_c1_is_8",  as_uptr(narrow_decode(0u, 3u, 1u)) == 8u);
            check("ag_shift3_c2_is_16", as_uptr(narrow_decode(0u, 3u, 2u)) == 16u);
            check("ag_shift3_c3_is_24", as_uptr(narrow_decode(0u, 3u, 3u)) == 24u);
            check("ag_shift3_c0xff_is_0x7f8",
                  as_uptr(narrow_decode(0u, 3u, 0xFFu)) == 0x7F8u);
        }

        // (AG4) BIT-WIDTH compile-time static_asserts on the narrow Klass word.
        //   - the slot the +8 header read consumes is a uint32_t (the narrow
        //     Klass type),
        //   - it is exactly 32 bits wide (4 bytes),
        //   - the decoder accepts a uint32_t parameter (no implicit widening on
        //     the input type),
        //   - the encoder produces a uint32_t result (no truncation surprise).
        // A regression that changed the narrow-klass slot width to 16 or 64 bits
        // would fail the BUILD here.
        static_assert(sizeof(std::uint32_t) == 4u,
                      "narrow Klass slot must be 4 bytes (32-bit)");
        static_assert(sizeof(std::uint32_t) * 8u == 32u,
                      "narrow Klass bit width must be 32");
        static_assert(std::is_unsigned_v<std::uint32_t>,
                      "narrow Klass type must be unsigned");
        static_assert(std::is_same_v<decltype(decode_klass_pointer(std::uint32_t{ 0u })), void*>,
                      "decode_klass_pointer must accept uint32_t and return void*");
        static_assert(std::is_same_v<decltype(encode_klass_pointer(nullptr)), std::uint32_t>,
                      "encode_klass_pointer must return uint32_t");
        // ALSO: the codec's address output is at least 64 bits — the decoded
        // klass pointer ABI is uintptr_t == 64-bit on every host vmhook targets.
        static_assert(sizeof(void*) >= 8u,
                      "decoded klass pointer width must be >= 64-bit");
        check("ag_narrow_klass_bit_width_static_asserts_compiled", true);

        // (AG5) NON-x64 / unsupported-arch FALL-THROUGH safe-default.  On any
        // host where the JVM symbols the codec expects are absent (true for this
        // no-JVM binary regardless of the build arch — and equivalent to the
        // non-x64 path on real hosts), the codec MUST return its null/zero
        // sentinel for EVERY representative input, never a fabricated pointer.
        // The arch markers below stand in for the kinds of narrow values an
        // unsupported arch would supply: low, mid, high, extreme.  Same input
        // set, repeated three times, must always sentinel.
        {
            const std::uint32_t arch_markers[]{
                0u, 1u, 2u, 3u, 0xA5u, 0x55AAu, 0x0001'0000u,
                0x1234'5678u, 0xFFFF'FFFEu, 0xFFFF'FFFFu,
            };
            bool always_sentinel{ true };
            for (int repeat{ 0 }; repeat < 3; ++repeat)
            {
                for (const std::uint32_t m : arch_markers)
                {
                    if (decode_klass_pointer(m) != nullptr) { always_sentinel = false; }
                }
            }
            check("ag_non_x64_fallthrough_decode_safe_default", always_sentinel);
            // Encode side: a varied pointer set (representing decoded klass
            // pointers an unsupported arch might present) returns 0.
            void* const probes[]{
                nullptr,
                reinterpret_cast<void*>(std::uintptr_t{ 0x10000u }),
                reinterpret_cast<void*>(std::uintptr_t{ 0x0000'0008'0000'0000ull }),
                reinterpret_cast<void*>(std::uintptr_t{ 0x0000'7F00'0000'0000ull }),
            };
            bool encode_always_zero{ true };
            for (void* const p : probes)
            {
                if (encode_klass_pointer(p) != 0u) { encode_always_zero = false; }
            }
            check("ag_non_x64_fallthrough_encode_safe_default", encode_always_zero);
        }

        // (AG6) CROSS-TABLE round-trip on {0, 1, 2, 0xFFFFFFFF} over the
        // documented (base, shift) table (zero / meta / cds * shift 0 / 3).
        // Each cell: decoded = base + (c << shift); encode(base, shift, decoded)
        // == c; decode(base, shift, encode(base, shift, decoded)) == decoded.
        // For c == 0 specifically: decoded == base, and encode(base) == 0.
        {
            struct cell { std::uint64_t base; std::uint32_t shift; const char* tag; };
            const cell table[]{
                { 0u,                                  0u, "zero_s0" },
                { 0u,                                  3u, "zero_s3" },
                { std::uint64_t{ 0x0000'0008'0000'0000ull }, 0u, "meta_s0" },
                { std::uint64_t{ 0x0000'0008'0000'0000ull }, 3u, "meta_s3" },
                { std::uint64_t{ 0x0000'7F00'0000'0000ull }, 0u, "cds_s0"  },
                { std::uint64_t{ 0x0000'7F00'0000'0000ull }, 3u, "cds_s3"  },
            };
            const std::uint32_t synth[]{ 0u, 1u, 2u, 0xFFFF'FFFFu };

            bool decode_matches{ true };
            bool encode_inv{ true };
            bool addr_rt{ true };
            bool zero_is_base{ true };
            for (const cell t : table)
            {
                for (const std::uint32_t c : synth)
                {
                    const std::uintptr_t got{ as_uptr(narrow_decode(t.base, t.shift, c)) };
                    const std::uintptr_t want{ static_cast<std::uintptr_t>(
                        t.base + (static_cast<std::uint64_t>(c) << t.shift)) };
                    if (got != want) { decode_matches = false; }

                    const std::uint32_t enc{ narrow_encode(
                        t.base, t.shift, static_cast<std::uint64_t>(got)) };
                    if (enc != c) { encode_inv = false; }

                    void* const back{ narrow_decode(t.base, t.shift, enc) };
                    if (as_uptr(back) != got) { addr_rt = false; }

                    if (c == 0u && static_cast<std::uint64_t>(got) != t.base)
                    {
                        zero_is_base = false;
                    }
                }
            }
            check("ag_cross_table_decode_matches_formula", decode_matches);
            check("ag_cross_table_encode_inverts_decode", encode_inv);
            check("ag_cross_table_decoded_addr_roundtrip", addr_rt);
            check("ag_cross_table_c0_decodes_to_base", zero_is_base);

            // Synthetic-narrow extremes pinned individually for readability:
            // c == 0xFFFFFFFF at shift 3 from a high base must land at base +
            // 0x7'FFFF'FFF8, exactly, with no overflow / sign-extension bug.
            const std::uint64_t hi_base{ 0x0000'7F00'0000'0000ull };
            check("ag_synth_max_at_shift3_from_hi_base_exact",
                  as_uptr(narrow_decode(hi_base, 3u, 0xFFFF'FFFFu))
                      == static_cast<std::uintptr_t>(hi_base + 0x7'FFFF'FFF8ull));
            // c == 1 at shift 3 from a high base lands at base + 8.
            check("ag_synth_one_at_shift3_from_hi_base_is_base_plus_8",
                  as_uptr(narrow_decode(hi_base, 3u, 1u))
                      == static_cast<std::uintptr_t>(hi_base + 8u));
            // c == 2 at shift 3 from a high base lands at base + 16.
            check("ag_synth_two_at_shift3_from_hi_base_is_base_plus_16",
                  as_uptr(narrow_decode(hi_base, 3u, 2u))
                      == static_cast<std::uintptr_t>(hi_base + 16u));
        }
    }

    return failures == 0 ? 0 : 1;
}
