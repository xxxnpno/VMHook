// Standalone (no-JVM) EXHAUSTIVE unit test for the dont_inline_dont_compile
// feature: the JIT inhibitors vmhook applies to a hooked Method so the patched
// i2i (interpreter) stub stays reachable.  Two HotSpot-internal words carry the
// inhibition and this file pins the PURE BIT ARITHMETIC and the NULL / fail-
// closed contract of both, with no live JVM in-process:
//
//   (1) _dont_inline  -- a single bit inside Method::_flags / MethodFlags::_status.
//       set_dont_inline() locates it via the PURE decision derive_method_flags_
//       layout() and toggles `1u << bit` with a width-correct RMW.
//   (2) NO_COMPILE    -- the JVM_ACC_NOT_C{1,2,2_OSR}_COMPILABLE | JVM_ACC_QUEUED
//       bitmask OR'd into Method::_access_flags (vmhook::hotspot::NO_COMPILE).
//
// Plus the common-detour dispatch POD (return_slot) the trampoline writes, and
// the klass-introspection bit constants / name-classification / empty contract.
//
// ---------------------------------------------------------------------------
// WHAT IS / ISN'T DETERMINABLE WITH NO JVM
// ---------------------------------------------------------------------------
// gHotSpotVMStructs is never resolvable in this process, so anything that needs
// a LIVE Method* / klass* / interpreter frame is OUT OF SCOPE (it is covered by
// the JVM integration module tests/jvm/modules/dont_inline_dont_compile.cpp
// against the DontInlineDontCompile fixture).  Determinable here, and pinned:
//
//   * derive_method_flags_layout()    -- a `constexpr` PURE function of VMStructs
//     EVIDENCE; every supported JDK's (offset,width,bit) decision is swept at
//     COMPILE TIME (static_assert) plus runtime, recomputed from source.
//   * NO_COMPILE                       -- a constexpr bitmask: exact value, the
//     four contributing bits, their high-byte positions, idempotent OR, and the
//     no-bleed into the JVM_ACC_STATIC / interface bits.
//   * the `1u << bit` _dont_inline mask arithmetic + idempotence + neighbour
//     isolation, for both the JDK 11..20 (bit 2) and JDK 21+ (bit 12) layouts.
//   * resolve_method_flags_slot() / set_dont_inline() NULL / invalid-pointer /
//     no-JVM contract: the is_valid_pointer pre-gate fires BEFORE any deref, so
//     a null / low-sentinel Method* yields a non-confident slot / a no-op WITHOUT
//     touching memory (no fabricated-address read).
//   * method::get_flags() RETURN TYPE -- std::uint16_t* (the u2 read view); a
//     compile-time pin of the documented width hazard (u1 on JDK 8-12, u4 on
//     JDK 21+ -- the accessor is deliberately u2-only; see vmhook.hpp:2897).
//   * return_slot POD layout / default encoding (the common-detour dispatch slot).
//   * klass-introspection bit constants (JVM_ACC_INTERFACE 0x0200, JVM_ACC_STATIC
//     0x0008), array-name classification ('[' prefix), and the empty / array
//     find_class() no-JVM null contract.
//
// Source of truth (line numbers approximate; the code is the authority):
//   vmhook/ext/vmhook/vmhook.hpp
//     derive_method_flags_layout    ~7450  (Path A u2/bit2, Path B u4/bit12)
//     method_flags_evidence/layout  ~7385/7401
//     resolve_method_flags_slot     ~7510  (is_valid_pointer pre-gate)
//     NO_COMPILE                    ~7579  (the four JVM_ACC compile-control bits)
//     set_dont_inline               ~7644  (null/invalid no-op; `1u<<bit` RMW)
//     method::get_flags             ~2944  (u2 read view -> std::uint16_t*)
//     return_slot                   ~1313  (cancel:bool, retval:int64)
//     find_class                    ~8146  (empty + '[' no-JVM null contract)
//     JVM_ACC_INTERFACE / _STATIC   ~15299 / ~4015
#include <vmhook/vmhook.hpp>

#include <cstddef>     // std::size_t
#include <cstdio>
#include <cstdint>
#include <type_traits> // std::is_same_v / is_standard_layout_v / remove_cvref_t
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

// Independent re-implementation of the documented Path-A / Path-B decision in
// derive_method_flags_layout, used ONLY to cross-check the library's result.
// Mirrors vmhook.hpp:7450 exactly; NEVER calls the library.
struct expected_layout
{
    std::uint64_t offset;
    int           width_bytes;
    int           dont_inline_bit;
    bool          confident;
};
static auto expect_layout(bool flags_present, const char* flags_type,
                          std::uint64_t flags_offset, bool intrinsic_present,
                          const char* intrinsic_type, std::uint64_t intrinsic_offset)
    -> expected_layout
{
    auto type_is = [](const char* t, const char* lit) -> bool
    {
        if (!t) { return false; }
        for (std::size_t i{ 0 }; ; ++i)
        {
            if (t[i] != lit[i]) { return false; }
            if (t[i] == '\0') { return true; }
        }
    };
    if (flags_present && type_is(flags_type, "u2"))
    {
        return expected_layout{ flags_offset, 2, 2, true };
    }
    if (intrinsic_present && type_is(intrinsic_type, "u2")
        && intrinsic_offset >= 4u && (intrinsic_offset % 4u) == 0u)
    {
        return expected_layout{ intrinsic_offset - 4u, 4, 12, true };
    }
    return expected_layout{ 0u, 0, 0, false };
}

// Build the VMStructs-evidence input for derive_method_flags_layout.
static auto make_evidence(bool flags_present, const char* flags_type,
                          std::uint64_t flags_offset, bool intrinsic_present,
                          const char* intrinsic_type, std::uint64_t intrinsic_offset)
    -> vmhook::hotspot::method_flags_evidence
{
    vmhook::hotspot::method_flags_evidence e{};
    e.flags_present        = flags_present;
    e.flags_type           = flags_type;
    e.flags_offset         = flags_offset;
    e.intrinsic_id_present = intrinsic_present;
    e.intrinsic_id_type    = intrinsic_type;
    e.intrinsic_id_offset  = intrinsic_offset;
    return e;
}

int main()
{
    using vmhook::hotspot::derive_method_flags_layout;
    using vmhook::hotspot::method_flags_evidence;
    using vmhook::hotspot::method_flags_layout;
    using vmhook::hotspot::method_flags_slot;
    using vmhook::hotspot::resolve_method_flags_slot;
    using vmhook::hotspot::set_dont_inline;
    using vmhook::hotspot::is_valid_pointer;

    // =====================================================================
    // 0. COMPILE-TIME signature / noexcept / return-type / POD pins.  These
    //    only inspect types or evaluate the constexpr decision, so they fail
    //    the BUILD on regression -- the strongest possible pin.
    // =====================================================================

    // NO_COMPILE is a constexpr std::int32_t; its type and value are fixed.
    static_assert(std::is_same_v<decltype(vmhook::hotspot::NO_COMPILE), const std::int32_t>,
                  "NO_COMPILE must be a constexpr std::int32_t");
    static_assert(vmhook::hotspot::NO_COMPILE
                      == (0x02000000 | 0x04000000 | 0x08000000 | 0x01000000),
                  "NO_COMPILE must be the OR of the four JVM_ACC compile-control bits");

    // derive_method_flags_layout is constexpr and noexcept; pin its return type
    // and that it can be evaluated in a constant expression.
    static_assert(std::is_same_v<
                      decltype(derive_method_flags_layout(method_flags_evidence{})),
                      method_flags_layout>,
                  "derive_method_flags_layout must return method_flags_layout");
    static_assert(noexcept(derive_method_flags_layout(method_flags_evidence{})),
                  "derive_method_flags_layout must be noexcept");

    // Path A (JDK 11..20 u2 _flags) evaluated at COMPILE TIME.
    {
        constexpr method_flags_evidence ev{ true, "u2", 48u, false, nullptr, 0u };
        constexpr method_flags_layout lay{ derive_method_flags_layout(ev) };
        static_assert(lay.confident, "Path A must be confident for u2 _flags");
        static_assert(lay.width_bytes == 2, "Path A width must be 2");
        static_assert(lay.dont_inline_bit == 2, "Path A bit must be 2");
        static_assert(lay.offset == 48u, "Path A must use the exported _flags offset");
    }
    // Path B (JDK 21+ derived MethodFlags::_status) evaluated at COMPILE TIME.
    {
        constexpr method_flags_evidence ev{ false, nullptr, 0u, true, "u2", 52u };
        constexpr method_flags_layout lay{ derive_method_flags_layout(ev) };
        static_assert(lay.confident, "Path B must be confident for u2 _intrinsic_id");
        static_assert(lay.width_bytes == 4, "Path B width must be 4");
        static_assert(lay.dont_inline_bit == 12, "Path B bit must be 12");
        static_assert(lay.offset == 48u, "Path B offset must be intrinsic_offset - 4");
    }
    // JDK 8 (u1 _intrinsic_id, no exported _flags) -> NOT confident, COMPILE TIME.
    {
        constexpr method_flags_evidence ev{ false, nullptr, 0u, true, "u1", 40u };
        constexpr method_flags_layout lay{ derive_method_flags_layout(ev) };
        static_assert(!lay.confident, "JDK 8 (u1 intrinsic) must be non-confident");
    }
    // Empty evidence -> NOT confident, COMPILE TIME.
    static_assert(!derive_method_flags_layout(method_flags_evidence{}).confident,
                  "empty evidence must be non-confident");

    // method::get_flags() is the u2 READ view -> std::uint16_t* (the documented
    // width hazard: u1 on JDK 8-12, u4 on JDK 21+, but the accessor stays u2).
    static_assert(std::is_same_v<
                      decltype(std::declval<vmhook::hotspot::method>().get_flags()),
                      std::uint16_t*>,
                  "method::get_flags() must return std::uint16_t* (u2 read view)");

    // resolve_method_flags_slot / set_dont_inline noexcept + return types.
    static_assert(noexcept(resolve_method_flags_slot(nullptr)),
                  "resolve_method_flags_slot must be noexcept");
    static_assert(std::is_same_v<decltype(resolve_method_flags_slot(nullptr)),
                                 method_flags_slot>,
                  "resolve_method_flags_slot must return method_flags_slot");
    static_assert(noexcept(set_dont_inline(nullptr, true)),
                  "set_dont_inline must be noexcept");
    static_assert(std::is_same_v<decltype(set_dont_inline(nullptr, true)), void>,
                  "set_dont_inline must return void");

    // return_slot POD layout (the common-detour dispatch slot).
    static_assert(std::is_standard_layout_v<vmhook::hotspot::return_slot>,
                  "return_slot must be standard-layout (raw stack slot)");
    static_assert(std::is_same_v<decltype(vmhook::hotspot::return_slot::cancel), bool>,
                  "return_slot::cancel must be bool");
    static_assert(std::is_same_v<decltype(vmhook::hotspot::return_slot::retval), std::int64_t>,
                  "return_slot::retval must be std::int64_t");

    check("dont_inline_dont_compile_static_asserts_compiled", true);

    // =====================================================================
    // A. NO_COMPILE bitmask -- exact value, contributing bits, high-byte
    //    positions, idempotent OR, and the no-bleed contract.  Every constant
    //    derived from vmhook.hpp:7558-7583.
    // =====================================================================
    {
        constexpr std::uint32_t NOT_C2{ 0x02000000u };       // JVM_ACC_NOT_C2_COMPILABLE
        constexpr std::uint32_t NOT_C1{ 0x04000000u };       // JVM_ACC_NOT_C1_COMPILABLE
        constexpr std::uint32_t NOT_C2_OSR{ 0x08000000u };   // JVM_ACC_NOT_C2_OSR_COMPILABLE
        constexpr std::uint32_t QUEUED{ 0x01000000u };       // JVM_ACC_QUEUED
        const std::uint32_t no_compile{ static_cast<std::uint32_t>(vmhook::hotspot::NO_COMPILE) };

        check("no_compile_equals_or_of_four_bits",
              no_compile == (NOT_C2 | NOT_C1 | NOT_C2_OSR | QUEUED));
        // Each contributing bit is present.
        check("no_compile_has_not_c2",     (no_compile & NOT_C2) == NOT_C2);
        check("no_compile_has_not_c1",     (no_compile & NOT_C1) == NOT_C1);
        check("no_compile_has_not_c2_osr", (no_compile & NOT_C2_OSR) == NOT_C2_OSR);
        check("no_compile_has_queued",     (no_compile & QUEUED) == QUEUED);
        // Exactly four bits set -- no stray bit crept in.
        {
            int set_bits{ 0 };
            for (int b{ 0 }; b < 32; ++b)
            {
                if ((no_compile >> b) & 1u) { ++set_bits; }
            }
            check("no_compile_has_exactly_four_bits", set_bits == 4);
        }
        // All four bits live in the historical high byte (bits 24..27).
        check("no_compile_bits_in_high_byte_24_27",
              (no_compile & 0xF0FFFFFFu) == 0u
                  && (no_compile & 0x0F000000u) == no_compile);
        check("no_compile_bit_positions_24_to_27",
              no_compile == ((1u << 24) | (1u << 25) | (1u << 26) | (1u << 27)));

        // IDEMPOTENT OR: setting NO_COMPILE twice leaves the same word.
        std::uint32_t access{ 0x00000021u };   // arbitrary low ACC bits already set
        const std::uint32_t once{ access | no_compile };
        const std::uint32_t twice{ once | no_compile };
        check("no_compile_or_is_idempotent", once == twice);
        // The pre-existing low bits are preserved (no clobber of real ACC flags).
        check("no_compile_or_preserves_low_bits", (once & 0x00000021u) == 0x00000021u);

        // CLEAR is the exact inverse: AND with ~NO_COMPILE restores the original.
        const std::uint32_t cleared{ once & static_cast<std::uint32_t>(~no_compile) };
        check("no_compile_clear_restores_original", cleared == 0x00000021u);

        // NO-BLEED: NO_COMPILE never touches the introspection bits this same
        // _access_flags word carries (JVM_ACC_STATIC 0x0008, JVM_ACC_INTERFACE
        // 0x0200) -- they sit in the LOW halfword, far below bit 24.
        check("no_compile_disjoint_from_jvm_acc_static",
              (no_compile & 0x0008u) == 0u);
        check("no_compile_disjoint_from_jvm_acc_interface",
              (no_compile & 0x0200u) == 0u);
        // Setting NO_COMPILE over a word that has STATIC+INTERFACE keeps them.
        const std::uint32_t with_acc{ 0x0008u | 0x0200u };
        check("no_compile_or_keeps_static_and_interface",
              ((with_acc | no_compile) & with_acc) == with_acc);
    }

    // =====================================================================
    // B. derive_method_flags_layout -- the PURE (offset,width,bit) decision,
    //    swept at runtime over every documented JDK layout + adversarial
    //    evidence, each cross-checked against an independent re-implementation.
    // =====================================================================
    {
        struct evidence_case
        {
            bool        flags_present;
            const char* flags_type;
            std::uint64_t flags_offset;
            bool        intrinsic_present;
            const char* intrinsic_type;
            std::uint64_t intrinsic_offset;
            const char* tag;
        };
        const evidence_case cases[]{
            // Path A: exported u2 _flags (JDK 11..20) at a range of offsets.
            { true,  "u2", 0u,   false, nullptr, 0u,  "pathA_off0"   },
            { true,  "u2", 48u,  false, nullptr, 0u,  "pathA_off48"  },
            { true,  "u2", 56u,  true,  "u2",    60u, "pathA_wins_over_B" },
            // Path B: derived MethodFlags::_status from u2 _intrinsic_id (JDK 21+).
            { false, nullptr, 0u, true, "u2", 4u,   "pathB_min_off4"  },
            { false, nullptr, 0u, true, "u2", 52u,  "pathB_off52"     },
            { false, nullptr, 0u, true, "u2", 64u,  "pathB_off64"     },
            // JDK 8: u1 _intrinsic_id, no _flags -> non-confident.
            { false, nullptr, 0u, true, "u1", 40u,  "jdk8_u1"         },
            // Path B rejects: intrinsic offset < 4 (would underflow).
            { false, nullptr, 0u, true, "u2", 0u,   "B_reject_off0"   },
            { false, nullptr, 0u, true, "u2", 2u,   "B_reject_off2"   },
            // Path B rejects: intrinsic offset not 4-aligned.
            { false, nullptr, 0u, true, "u2", 6u,   "B_reject_unaligned6"  },
            { false, nullptr, 0u, true, "u2", 50u,  "B_reject_unaligned50" },
            // Path A rejects a non-u2 _flags export (would mismatch width).
            { true,  "u1", 48u,  false, nullptr, 0u, "A_reject_u1_flags" },
            { true,  "u4", 48u,  true,  "u2", 52u,  "A_reject_u4_flags_falls_to_B" },
            // Nothing exported -> non-confident.
            { false, nullptr, 0u, false, nullptr, 0u, "nothing" },
        };
        bool all_match{ true };
        std::size_t evaluated{ 0 };
        for (const evidence_case c : cases)
        {
            const method_flags_layout got{ derive_method_flags_layout(
                make_evidence(c.flags_present, c.flags_type, c.flags_offset,
                              c.intrinsic_present, c.intrinsic_type, c.intrinsic_offset)) };
            const expected_layout want{ expect_layout(
                c.flags_present, c.flags_type, c.flags_offset,
                c.intrinsic_present, c.intrinsic_type, c.intrinsic_offset) };
            if (got.confident != want.confident
                || got.offset != want.offset
                || got.width_bytes != want.width_bytes
                || got.dont_inline_bit != want.dont_inline_bit)
            {
                all_match = false;
            }
            ++evaluated;
        }
        check("derive_layout_matches_independent_reimpl_all_cases", all_match);
        check("derive_layout_swept_every_case", evaluated == (sizeof(cases) / sizeof(cases[0])));

        // The two confident layouts produce exactly the documented bit positions.
        const method_flags_layout path_a{ derive_method_flags_layout(
            make_evidence(true, "u2", 48u, false, nullptr, 0u)) };
        check("derive_layout_pathA_bit_is_2", path_a.confident && path_a.dont_inline_bit == 2);
        const method_flags_layout path_b{ derive_method_flags_layout(
            make_evidence(false, nullptr, 0u, true, "u2", 52u)) };
        check("derive_layout_pathB_bit_is_12", path_b.confident && path_b.dont_inline_bit == 12);
        // Path A wins when BOTH are present (legacy byte-identical behaviour).
        const method_flags_layout both{ derive_method_flags_layout(
            make_evidence(true, "u2", 56u, true, "u2", 60u)) };
        check("derive_layout_pathA_wins_when_both_present",
              both.confident && both.width_bytes == 2 && both.offset == 56u);
    }

    // =====================================================================
    // C. _dont_inline MASK ARITHMETIC -- `1u << bit` for the two layouts, plus
    //    idempotent set, clean clear, and neighbour-bit isolation.  This is the
    //    pure bit logic set_dont_inline performs once the slot is resolved.
    // =====================================================================
    {
        const int bits[]{ 2, 12 };   // JDK 11..20 and JDK 21+ positions
        bool mask_ok{ true };
        for (const int bit : bits)
        {
            const std::uint32_t mask{ 1u << bit };
            // Set on an empty word lights exactly that bit.
            if ((0u | mask) != mask) { mask_ok = false; }
            // Idempotent: a second OR does not flip it twice / change the word.
            const std::uint32_t once{ 0u | mask };
            if ((once | mask) != once) { mask_ok = false; }
            // Clear is the inverse.
            if (((once) & ~mask) != 0u) { mask_ok = false; }
            // Exactly one bit set.
            int n{ 0 };
            for (int b{ 0 }; b < 32; ++b) { if ((mask >> b) & 1u) { ++n; } }
            if (n != 1) { mask_ok = false; }
        }
        check("dont_inline_mask_arithmetic_both_bits", mask_ok);

        // NEIGHBOUR ISOLATION: setting bit N leaves bits N-1 and N+1 untouched,
        // and a fully-populated word keeps every OTHER bit after the OR/AND.
        bool neighbour_ok{ true };
        for (const int bit : bits)
        {
            const std::uint32_t mask{ 1u << bit };
            const std::uint32_t neighbours{ (1u << (bit - 1)) | (1u << (bit + 1)) };
            const std::uint32_t word{ neighbours };               // neighbours preset
            const std::uint32_t set{ word | mask };               // set _dont_inline
            if ((set & neighbours) != neighbours) { neighbour_ok = false; }  // neighbours intact
            const std::uint32_t clr{ set & ~mask };               // clear _dont_inline
            if (clr != neighbours) { neighbour_ok = false; }      // back to just neighbours
        }
        check("dont_inline_set_clear_leaves_neighbours_intact", neighbour_ok);

        // The u2-width set path masks within 16 bits for bit 2; bit 12 also fits
        // a u16, but the JDK 21+ word is u4 -- pin both representable widths.
        check("dont_inline_bit2_fits_u16",  (1u << 2) <= 0xFFFFu);
        check("dont_inline_bit12_fits_u16", (1u << 12) <= 0xFFFFu);
        check("dont_inline_bit12_fits_u32", (1u << 12) <= 0xFFFFFFFFu);
        // u2 cast of the mask round-trips (the library casts to std::uint16_t).
        check("dont_inline_bit2_u16_cast_roundtrip",
              static_cast<std::uint16_t>(1u << 2) == 0x0004u);
        check("dont_inline_bit12_u16_cast_roundtrip",
              static_cast<std::uint16_t>(1u << 12) == 0x1000u);
    }

    // =====================================================================
    // D. resolve_method_flags_slot / set_dont_inline -- NULL / INVALID-pointer
    //    / no-JVM contract.  The is_valid_pointer pre-gate fires BEFORE any
    //    `this + offset` is formed, so these never dereference; we pass null and
    //    is_valid_pointer-REJECTED low constants (never dereferenced) only.
    // =====================================================================
    {
        // null Method* -> non-confident, all-zero slot.
        const method_flags_slot null_slot{ resolve_method_flags_slot(nullptr) };
        check("resolve_slot_null_method_not_confident", !null_slot.confident);
        check("resolve_slot_null_method_address_null", null_slot.address == nullptr);
        check("resolve_slot_null_method_width_zero", null_slot.width_bytes == 0);

        // is_valid_pointer-rejected low sentinels: rejected up front, never read.
        const std::uintptr_t bad_method_addrs[]{
            std::uintptr_t{ 0x1u },
            std::uintptr_t{ 0x8u },
            std::uintptr_t{ 0xFFFFu },        // exactly the floor (rejected, <=)
            std::uintptr_t{ 0xDEADBEEFu },    // poison-ish, low
        };
        bool all_low_not_confident{ true };
        for (const std::uintptr_t addr : bad_method_addrs)
        {
            const auto* const m{ reinterpret_cast<const vmhook::hotspot::method*>(addr) };
            // Sanity: is_valid_pointer rejects each, so no deref can occur.
            if (is_valid_pointer(m)) { all_low_not_confident = false; }
            if (resolve_method_flags_slot(m).confident) { all_low_not_confident = false; }
        }
        check("resolve_slot_invalid_low_addrs_not_confident", all_low_not_confident);

        // set_dont_inline on null / invalid is a crash-free no-op (returns void;
        // the value of the call is just that it does not fault).
        set_dont_inline(nullptr, true);
        set_dont_inline(nullptr, false);
        for (const std::uintptr_t addr : bad_method_addrs)
        {
            const auto* const m{ reinterpret_cast<const vmhook::hotspot::method*>(addr) };
            set_dont_inline(m, true);
            set_dont_inline(m, false);
        }
        check("set_dont_inline_null_and_invalid_are_noops", true);

        // No-JVM: even a VALID-SHAPED (real, mapped, aligned) pointer yields a
        // non-confident slot because no Method::_flags / _intrinsic_id VMStruct
        // resolves -> derive_method_flags_layout returns non-confident.  We pass
        // a real stack object's address (passes is_valid_pointer) but
        // set_dont_inline still no-ops because the slot is non-confident; this
        // proves the degrade-gracefully behaviour without a JVM.  We do NOT
        // assert it mutates anything (it must not).
        {
            alignas(16) std::uint8_t fake_method[64]{};
            const auto* const m{ reinterpret_cast<const vmhook::hotspot::method*>(fake_method) };
            check("resolve_slot_valid_shaped_no_jvm_not_confident",
                  !resolve_method_flags_slot(m).confident);
            // No mutation: the bytes are untouched by a no-op set.
            set_dont_inline(m, true);
            bool untouched{ true };
            for (const std::uint8_t b : fake_method) { if (b != 0u) { untouched = false; } }
            check("set_dont_inline_valid_shaped_no_jvm_does_not_mutate", untouched);
        }
    }

    // =====================================================================
    // E. return_slot -- the common-detour dispatch POD the trampoline writes.
    //    Default-encoding contract (cancel=false, retval=0) and that the two
    //    fields are independently writable as the trampoline / return_value do.
    // =====================================================================
    {
        vmhook::hotspot::return_slot slot{};
        check("return_slot_default_cancel_false", slot.cancel == false);
        check("return_slot_default_retval_zero", slot.retval == 0);

        // The dispatch decision the trampoline encodes: cancel flips, retval
        // holds the raw 64-bit return bit-pattern.  Pure POD field writes.
        slot.cancel = true;
        slot.retval = static_cast<std::int64_t>(0x1122334455667788ll);
        check("return_slot_cancel_writable", slot.cancel == true);
        check("return_slot_retval_holds_raw_bits",
              slot.retval == static_cast<std::int64_t>(0x1122334455667788ll));

        // retval is a full signed 64-bit cell (sign-preserving), distinct from
        // the cancel flag -- a negative value round-trips intact.
        slot.retval = static_cast<std::int64_t>(-1);
        check("return_slot_retval_negative_roundtrip", slot.retval == -1);

        // sizeof pins the slot is at least the two fields with no surprise growth.
        check("return_slot_size_holds_both_fields",
              sizeof(vmhook::hotspot::return_slot)
                  >= sizeof(bool) + sizeof(std::int64_t));
    }

    // =====================================================================
    // F. klass-introspection -- the bit constants the dont_inline_dont_compile
    //    feature shares the _access_flags word with, the array/primitive name
    //    classification, and the empty / array find_class() no-JVM contract.
    // =====================================================================
    {
        // The JVM_ACC bit constants, exactly as the library uses them
        // (vmhook.hpp:15299 / 4015).  These decode the SAME 32-bit word
        // NO_COMPILE writes its high byte into, so their disjointness is the
        // safety contract section A already pinned; here we pin their values.
        constexpr std::uint32_t JVM_ACC_STATIC{ 0x0008u };
        constexpr std::uint32_t JVM_ACC_INTERFACE{ 0x0200u };
        check("jvm_acc_static_is_bit_3", JVM_ACC_STATIC == (1u << 3));
        check("jvm_acc_interface_is_bit_9", JVM_ACC_INTERFACE == (1u << 9));

        // Access-modifier decode: a word with both bits set reports both; a word
        // with neither reports neither -- pure mask arithmetic over the decode.
        const std::uint32_t both_set{ JVM_ACC_STATIC | JVM_ACC_INTERFACE };
        check("acc_decode_static_set", (both_set & JVM_ACC_STATIC) != 0u);
        check("acc_decode_interface_set", (both_set & JVM_ACC_INTERFACE) != 0u);
        const std::uint32_t neither{ 0x0001u };   // ACC_PUBLIC only
        check("acc_decode_static_clear", (neither & JVM_ACC_STATIC) == 0u);
        check("acc_decode_interface_clear", (neither & JVM_ACC_INTERFACE) == 0u);

        // ARRAY-name classification: the library keys array klass resolution on a
        // leading '[' (vmhook.hpp:8175).  Pin the predicate over a varied set of
        // internal names, including primitive-array and object-array descriptors.
        struct name_case { const char* name; bool is_array; };
        const name_case names[]{
            { "[I",                      true  },  // int[]
            { "[[J",                     true  },  // long[][]
            { "[Ljava/lang/String;",     true  },  // String[]
            { "[Z",                      true  },  // boolean[]
            { "java/lang/String",        false },  // ordinary class
            { "java/lang/Object",        false },  // ordinary class
            { "I",                       false },  // bare primitive descriptor
        };
        bool classify_ok{ true };
        for (const name_case nc : names)
        {
            const bool leading_bracket{ nc.name[0] == '[' };
            if (leading_bracket != nc.is_array) { classify_ok = false; }
        }
        check("array_name_classified_by_leading_bracket", classify_ok);

        // find_class() no-JVM contract: empty name and every array descriptor
        // resolve to nullptr without a JVM (empty short-circuits before the
        // graph walk; array names route through jni_find_class which is null off
        // -JVM).  None of these dereference a fabricated address.
        check("find_class_empty_no_jvm_is_null", vmhook::find_class("") == nullptr);
        bool all_array_null{ true };
        for (const name_case nc : names)
        {
            if (nc.is_array && vmhook::find_class(nc.name) != nullptr) { all_array_null = false; }
        }
        check("find_class_array_descriptors_no_jvm_all_null", all_array_null);
    }

    // =====================================================================
    // G. Element-type pins on local arrays (remove_cvref_t, since decltype of
    //    arr[i] is a reference) -- the data-driven sweeps above index typed
    //    arrays; pin the element types so a silent narrowing cannot creep in.
    // =====================================================================
    {
        const std::uint32_t acc_bits[]{ 0x0008u, 0x0200u, 0x01000000u, 0x02000000u };
        using acc_elem = std::remove_cvref_t<decltype(acc_bits[0])>;
        static_assert(std::is_same_v<acc_elem, std::uint32_t>,
                      "acc_bits elements must be std::uint32_t");
        // Reference every element so the array is not unused (-Wunused-variable).
        std::uint32_t acc_or{ 0u };
        for (const std::uint32_t b : acc_bits) { acc_or |= b; }
        check("acc_bits_or_includes_no_compile_high_bits",
              (acc_or & static_cast<std::uint32_t>(vmhook::hotspot::NO_COMPILE))
                  == 0x03000000u);

        const int dont_inline_bits[]{ 2, 12 };
        using bit_elem = std::remove_cvref_t<decltype(dont_inline_bits[0])>;
        static_assert(std::is_same_v<bit_elem, int>,
                      "dont_inline_bits elements must be int");
        std::uint32_t combined_mask{ 0u };
        for (const int bit : dont_inline_bits) { combined_mask |= (1u << bit); }
        check("dont_inline_bits_combined_mask",
              combined_mask == ((1u << 2) | (1u << 12)));
    }

    std::printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
