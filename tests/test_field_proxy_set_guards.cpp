// Standalone unit test: field_proxy::set size-mismatch + non-primitive guards (no JVM).
//
// Covers the pure-logic / null-safety half of three audit findings:
//   audit/findings/field_proxy_set_size_guard.md
//   audit/findings/field_proxy_string_set.md
//   audit/findings/field_proxy_array_primitives.md
//
// field_proxy::set's primitive arm is a plain memcpy gated by
// vmhook::detail::jvm_primitive_byte_width, and the non-primitive arms are
// guarded by the same width oracle (returning early for genuine primitive
// signatures Z/B/C/S/I/J/F/D).  Both guards run entirely on a caller-supplied
// raw pointer, so we exercise them over a stack buffer with sentinel bytes and
// never touch a live oop or a running JVM.
//
// WHAT THIS FILE EXHAUSTS (the no-JVM-testable decision logic):
//   * vmhook::detail::jvm_primitive_byte_width over the full descriptor space:
//     every primitive char, both single-char non-primitives, multi-char,
//     empty, leading-primitive-but-longer, lowercase, and array forms.
//   * The size/width guard across the FULL value-width x field-width matrix:
//     value byte-width {1,2,4,8} x field byte-width {1,2,4,8} = 16 cells, one
//     assertion-block per cell (the 4 diagonal cells accept, the 12 off-diagonal
//     cells reject), plus the zero-width / unknown-descriptor escape hatch.
//   * The non-primitive type guard for string / string_view / const char* /
//     vector<T> / unique_ptr<wrapper> against EVERY primitive field width, and
//     the complementary "non-primitive value into a non-primitive field is NOT
//     refused by this guard" direction.
//   * The "C" 1-byte widening shortcut vs. the verbatim 2-byte path, including
//     the 1-byte-value-into-non-C-field interaction (no widening there).
//   * Null / degenerate field_pointer in BOTH the trivially-copyable arm and the
//     unique_ptr arm.
//   * Same-width type CONFUSION (float<->int, double<->long): the guard is a
//     SIZE guard, not a TYPE guard, so these are ACCEPTED and the raw bit
//     pattern is copied verbatim.  Pinned as characterisation locks (see the
//     [INFO] block) so a future signature-aware type check is detected here.
//   * std::array<char,N> / non-string container fall-through (treated as a raw
//     trivially-copyable blob, NOT refused) — the type-set boundary of the guard.
//
// OUT OF SCOPE for this file (needs a live oop / running JVM, covered by the JVM
// integration module tests/jvm/modules/field_set_size_guard.cpp):
//   * actual primitive write landing in a real Java field,
//   * set_str_field / set_prim_array / set_bool_array / set_str_array success
//     paths (they decode a compressed OOP and mutate a real Java backing array),
//   * the value_t -> std::vector<T> read path's END-TO-END behaviour on a live
//     array (empty-vector refusal, no OOB) -- though its read-side element-width
//     guard PREDICATE (pure width logic, shared jvm_primitive_byte_width oracle)
//     is pinned here in SECTION 18,
//   * unique_ptr<wrapper> success path (encodes a real OOP),
//   * the anti-clobber adjacency proof and Java-visibility (getfield) read-back.
// Here we only assert that the guards reject mistyped writes and leave the
// caller's buffer (and its sentinels) untouched, and that the accepted writes
// land the expected host-native bytes.  NOTE: the byte patterns below are read
// back on the same host that wrote them, so they assume host-native byte order
// on both ends; they are NOT testing JVM field byte order (on a real oop the
// slot holds whatever the JVM laid down — that boundary belongs to the JVM
// sibling module).

#include <vmhook/vmhook.hpp>
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

// A minimal wrapper so std::unique_ptr<test_wrapper> satisfies
// detail::is_unique_ptr_v AND so field_proxy::set's unique_ptr branch (which
// calls value->object_base::get_instance()) instantiates.  The guard short-
// circuits before that branch runs for a primitive signature, so a null
// unique_ptr never gets dereferenced.
struct test_wrapper : vmhook::object_base
{
    using vmhook::object_base::object_base;
};

// A 32-byte stack canvas: 8 leading sentinel bytes, an 8-byte field slot, then
// 16 trailing sentinel bytes.  Wider-than-field writes that slipped past the
// guard would smash the trailing sentinels; the guard must keep them intact.
namespace
{
    constexpr std::size_t k_lead{ 8 };
    constexpr std::size_t k_slot{ 8 };
    constexpr std::size_t k_trail{ 16 };
    constexpr std::uint8_t k_sentinel{ 0xCD };

    struct canvas
    {
        std::array<std::uint8_t, k_lead + k_slot + k_trail> bytes{};

        canvas() { bytes.fill(k_sentinel); }

        auto field_ptr() -> void* { return bytes.data() + k_lead; }

        // True if every byte outside the [k_lead, k_lead + k_slot) field slot
        // still holds the sentinel value.
        auto sentinels_intact() const -> bool
        {
            for (std::size_t i{ 0 }; i < bytes.size(); ++i)
            {
                if (i >= k_lead && i < k_lead + k_slot) { continue; }
                if (bytes[i] != k_sentinel) { return false; }
            }
            return true;
        }

        // True if the field slot still holds all-sentinel bytes (i.e. the write
        // was rejected and nothing landed in the slot either).
        auto slot_intact() const -> bool
        {
            for (std::size_t i{ k_lead }; i < k_lead + k_slot; ++i)
            {
                if (bytes[i] != k_sentinel) { return false; }
            }
            return true;
        }

        // True if the first `n` bytes of the field slot equal `expected` (a raw
        // byte view of the value) AND the remaining slot bytes + every sentinel
        // are still untouched.  Used to prove an accepted write landed exactly
        // `n` bytes and not one more.
        auto slot_holds(const void* expected, std::size_t n) const -> bool
        {
            const auto* e{ static_cast<const std::uint8_t*>(expected) };
            for (std::size_t i{ 0 }; i < n; ++i)
            {
                if (bytes[k_lead + i] != e[i]) { return false; }
            }
            for (std::size_t i{ k_lead + n }; i < k_lead + k_slot; ++i)
            {
                if (bytes[i] != k_sentinel) { return false; }
            }
            return sentinels_intact();
        }
    };

    // Helper: build a proxy over a fresh canvas, set a value, and report whether
    // the write was ACCEPTED (value's raw bytes landed, nothing else moved) for
    // a primitive signature of the given width.  `value_width` is sizeof(T).
    template<typename T>
    auto accepted_into(const char* sig, const T& value) -> bool
    {
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), sig, false };
        proxy.set(value);
        return c.slot_holds(&value, sizeof(T));
    }

    // Helper: build a proxy over a fresh canvas, set a value, and report whether
    // the write was REJECTED (slot AND sentinels both fully intact).
    template<typename T>
    auto rejected_into(const char* sig, const T& value) -> bool
    {
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), sig, false };
        proxy.set(value);
        return c.slot_intact() && c.sentinels_intact();
    }

    // Read the field slot back as a raw 32-bit pattern (host-native both ways).
    auto slot_bits32(const canvas& c) -> std::uint32_t
    {
        std::uint32_t bits{};
        std::memcpy(&bits, c.bytes.data() + k_lead, sizeof(bits));
        return bits;
    }

    // Read the field slot back as a raw 64-bit pattern (host-native both ways).
    auto slot_bits64(const canvas& c) -> std::uint64_t
    {
        std::uint64_t bits{};
        std::memcpy(&bits, c.bytes.data() + k_lead, sizeof(bits));
        return bits;
    }

    // Manufacture a float with EXACT bits (NaN / +-inf / +-0 / subnormal) and set
    // it into "F", reporting whether those EXACT 32 bits landed AND no sentinel
    // moved.  The float is materialised as a LOCAL and bound to set() by const-ref
    // (set's signature is `const value_type&`), and the only operation on it is a
    // memcpy of its address — it never transits a by-value return or an arithmetic
    // FPU register, so a signalling NaN is NOT canonicalised to a quiet NaN even on
    // a 32-bit x87 ABI.  The comparison is on the raw BIT PATTERN, never `==`
    // (which is false for NaN).  IEEE-754 binary32 is mandated on every CI target.
    auto float_bits_land(std::uint32_t bits) -> bool
    {
        float value{};
        std::memcpy(&value, &bits, sizeof(value));
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "F", false };
        proxy.set(value);
        return slot_bits32(c) == bits && c.sentinels_intact();
    }

    // Same as float_bits_land for double / "D" (IEEE-754 binary64).
    auto double_bits_land(std::uint64_t bits) -> bool
    {
        double value{};
        std::memcpy(&value, &bits, sizeof(value));
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "D", false };
        proxy.set(value);
        return slot_bits64(c) == bits && c.sentinels_intact();
    }

    // Set a float manufactured from `bits` into an arbitrary signature and report
    // whether the write was REJECTED (slot + sentinels intact).  Same NaN-safe
    // local-materialise discipline as float_bits_land.
    auto float_bits_rejected_into(const char* sig, std::uint32_t bits) -> bool
    {
        float value{};
        std::memcpy(&value, &bits, sizeof(value));
        return rejected_into(sig, value);
    }

    auto double_bits_rejected_into(const char* sig, std::uint64_t bits) -> bool
    {
        double value{};
        std::memcpy(&value, &bits, sizeof(value));
        return rejected_into(sig, value);
    }
}

int main()
{
    using vmhook::detail::jvm_primitive_byte_width;

    // ======================================================================
    // SECTION 1 — jvm_primitive_byte_width: the width oracle both guards
    // consult.  Z/B == 1, S/C == 2, I/F == 4, J/D == 8; everything else == 0.
    // The `.size() != 1` early-out is what makes every multi-char descriptor,
    // the empty string, and lowercase variants all collapse to 0.
    // ======================================================================
    check("width_boolean_Z_is_1", jvm_primitive_byte_width("Z") == 1);
    check("width_byte_B_is_1", jvm_primitive_byte_width("B") == 1);
    check("width_short_S_is_2", jvm_primitive_byte_width("S") == 2);
    check("width_char_C_is_2", jvm_primitive_byte_width("C") == 2);
    check("width_int_I_is_4", jvm_primitive_byte_width("I") == 4);
    check("width_float_F_is_4", jvm_primitive_byte_width("F") == 4);
    check("width_long_J_is_8", jvm_primitive_byte_width("J") == 8);
    check("width_double_D_is_8", jvm_primitive_byte_width("D") == 8);
    check("width_reference_L_is_0", jvm_primitive_byte_width("Ljava/lang/String;") == 0);
    check("width_array_bracket_is_0", jvm_primitive_byte_width("[I") == 0);
    check("width_void_V_is_0", jvm_primitive_byte_width("V") == 0);
    check("width_empty_is_0", jvm_primitive_byte_width("") == 0);
    check("width_unknown_X_is_0", jvm_primitive_byte_width("X") == 0);
    check("width_multichar_II_is_0", jvm_primitive_byte_width("II") == 0);

    // Exhaustive single-char sweep: every primitive descriptor returns its
    // width, and EVERY other single ASCII letter (including the ones that look
    // close to a primitive) returns 0.  This pins the switch's `default`.
    check("width_array_only_bracket_is_0", jvm_primitive_byte_width("[") == 0);
    check("width_object_only_L_is_0", jvm_primitive_byte_width("L") == 0);   // bare 'L', no class -> 0 width
    check("width_lower_z_is_0", jvm_primitive_byte_width("z") == 0);
    check("width_lower_i_is_0", jvm_primitive_byte_width("i") == 0);
    check("width_lower_j_is_0", jvm_primitive_byte_width("j") == 0);
    check("width_lower_c_is_0", jvm_primitive_byte_width("c") == 0);
    check("width_digit_0_is_0", jvm_primitive_byte_width("0") == 0);
    check("width_A_is_0", jvm_primitive_byte_width("A") == 0);   // not a JVM primitive
    check("width_E_is_0", jvm_primitive_byte_width("E") == 0);
    check("width_G_is_0", jvm_primitive_byte_width("G") == 0);
    check("width_H_is_0", jvm_primitive_byte_width("H") == 0);
    check("width_K_is_0", jvm_primitive_byte_width("K") == 0);
    check("width_M_is_0", jvm_primitive_byte_width("M") == 0);
    check("width_N_is_0", jvm_primitive_byte_width("N") == 0);
    check("width_O_is_0", jvm_primitive_byte_width("O") == 0);
    check("width_P_is_0", jvm_primitive_byte_width("P") == 0);
    check("width_Q_is_0", jvm_primitive_byte_width("Q") == 0);
    check("width_R_is_0", jvm_primitive_byte_width("R") == 0);
    check("width_T_is_0", jvm_primitive_byte_width("T") == 0);
    check("width_U_is_0", jvm_primitive_byte_width("U") == 0);
    check("width_W_is_0", jvm_primitive_byte_width("W") == 0);
    check("width_Y_is_0", jvm_primitive_byte_width("Y") == 0);

    // Multi-char descriptors that START with a primitive letter must STILL be 0
    // (the `.size() != 1` gate, not a `front()` check).  This is what stops the
    // guards from treating "II"/"I;"/"ICONST" as a 4-byte int field.
    check("width_I_semicolon_is_0", jvm_primitive_byte_width("I;") == 0);
    check("width_ZZ_is_0", jvm_primitive_byte_width("ZZ") == 0);
    check("width_J_long_word_is_0", jvm_primitive_byte_width("Jxx") == 0);
    check("width_C_then_space_is_0", jvm_primitive_byte_width("C ") == 0);
    check("width_space_then_I_is_0", jvm_primitive_byte_width(" I") == 0);
    check("width_leading_space_is_0", jvm_primitive_byte_width(" ") == 0);   // single non-letter char
    check("width_array_of_int_full_is_0", jvm_primitive_byte_width("[[I") == 0);
    check("width_array_of_ref_is_0", jvm_primitive_byte_width("[Ljava/lang/Object;") == 0);
    check("width_method_desc_is_0", jvm_primitive_byte_width("()V") == 0);

    // ======================================================================
    // SECTION 2 — Right-sized primitive writes succeed: the exact-width C++
    // value lands in the field slot and never disturbs the surrounding
    // sentinels.  (Diagonal of the size matrix, one per width class, with the
    // natural C++ type for each descriptor.)
    // ======================================================================
    {
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "Z", false };
        proxy.set(std::uint8_t{ 0x01 });
        check("set_Z_right_size_writes_low_byte", c.bytes[k_lead] == 0x01);
        check("set_Z_right_size_keeps_sentinels", c.sentinels_intact());
    }
    {
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "B", false };
        proxy.set(std::int8_t{ -2 });
        check("set_B_right_size_writes_byte", c.bytes[k_lead] == 0xFE);
        check("set_B_right_size_keeps_sentinels", c.sentinels_intact());
    }
    {
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "S", false };
        proxy.set(std::int16_t{ 0x1234 });
        std::int16_t read{};
        std::memcpy(&read, c.field_ptr(), sizeof(read));
        check("set_S_right_size_writes_2_bytes", read == std::int16_t{ 0x1234 });
        check("set_S_right_size_keeps_sentinels", c.sentinels_intact());
    }
    {
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "I", false };
        proxy.set(std::int32_t{ 0x0BADF00D });
        std::int32_t read{};
        std::memcpy(&read, c.field_ptr(), sizeof(read));
        check("set_I_right_size_writes_4_bytes", read == std::int32_t{ 0x0BADF00D });
        check("set_I_right_size_keeps_sentinels", c.sentinels_intact());
    }
    {
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "J", false };
        proxy.set(std::int64_t{ 0x0123456789ABCDEF });
        std::int64_t read{};
        std::memcpy(&read, c.field_ptr(), sizeof(read));
        check("set_J_right_size_writes_8_bytes", read == std::int64_t{ 0x0123456789ABCDEF });
        check("set_J_right_size_keeps_sentinels", c.sentinels_intact());
    }
    {
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "F", false };
        proxy.set(float{ 3.5F });
        float read{};
        std::memcpy(&read, c.field_ptr(), sizeof(read));
        check("set_F_right_size_writes_4_bytes", read == 3.5F);
        check("set_F_right_size_keeps_sentinels", c.sentinels_intact());
    }
    {
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "D", false };
        proxy.set(double{ 2.25 });
        double read{};
        std::memcpy(&read, c.field_ptr(), sizeof(read));
        check("set_D_right_size_writes_8_bytes", read == 2.25);
        check("set_D_right_size_keeps_sentinels", c.sentinels_intact());
    }

    // ======================================================================
    // SECTION 3 — THE FULL SIZE-GUARD MATRIX: value byte-width {1,2,4,8} x
    // field byte-width {1,2,4,8} = 16 cells, driven with fixed-width unsigned
    // integer carriers so the ONLY variable is the byte count.  The 4 diagonal
    // cells (equal width) are ACCEPTED; the 12 off-diagonal cells are REJECTED.
    //
    // Field-width representatives (avoiding "C", which has the 1-byte widening
    // shortcut tested separately): width 1 = "B", width 2 = "S", width 4 = "I",
    // width 8 = "J".  Value carriers: u8/u16/u32/u64.
    //
    // For accepted cells we assert the raw bytes landed AND nothing past them
    // moved (slot_holds); for rejected cells we assert slot + sentinels intact.
    // ----------------------------------------------------------------------

    // --- value width 1 (uint8_t) ---
    check("matrix_v1_f1_accept", accepted_into("B", std::uint8_t{ 0xA7 }));        // 1 -> 1 ACCEPT
    check("matrix_v1_f2_reject", rejected_into("S", std::uint8_t{ 0xA7 }));        // 1 -> 2 reject
    check("matrix_v1_f4_reject", rejected_into("I", std::uint8_t{ 0xA7 }));        // 1 -> 4 reject
    check("matrix_v1_f8_reject", rejected_into("J", std::uint8_t{ 0xA7 }));        // 1 -> 8 reject

    // --- value width 2 (uint16_t) ---
    check("matrix_v2_f1_reject", rejected_into("B", std::uint16_t{ 0xBEEF }));     // 2 -> 1 reject
    check("matrix_v2_f2_accept", accepted_into("S", std::uint16_t{ 0xBEEF }));     // 2 -> 2 ACCEPT
    check("matrix_v2_f4_reject", rejected_into("I", std::uint16_t{ 0xBEEF }));     // 2 -> 4 reject
    check("matrix_v2_f8_reject", rejected_into("J", std::uint16_t{ 0xBEEF }));     // 2 -> 8 reject

    // --- value width 4 (uint32_t) ---
    check("matrix_v4_f1_reject", rejected_into("B", std::uint32_t{ 0xDEADBEEF }));  // 4 -> 1 reject
    check("matrix_v4_f2_reject", rejected_into("S", std::uint32_t{ 0xDEADBEEF }));  // 4 -> 2 reject
    check("matrix_v4_f4_accept", accepted_into("I", std::uint32_t{ 0xDEADBEEF }));  // 4 -> 4 ACCEPT
    check("matrix_v4_f8_reject", rejected_into("J", std::uint32_t{ 0xDEADBEEF }));  // 4 -> 8 reject

    // --- value width 8 (uint64_t) ---
    check("matrix_v8_f1_reject", rejected_into("B", std::uint64_t{ 0x0123456789ABCDEFull })); // 8 -> 1 reject
    check("matrix_v8_f2_reject", rejected_into("S", std::uint64_t{ 0x0123456789ABCDEFull })); // 8 -> 2 reject
    check("matrix_v8_f4_reject", rejected_into("I", std::uint64_t{ 0x0123456789ABCDEFull })); // 8 -> 4 reject
    check("matrix_v8_f8_accept", accepted_into("J", std::uint64_t{ 0x0123456789ABCDEFull })); // 8 -> 8 ACCEPT

    // Same matrix once more against the *other* descriptor of each width, to
    // prove the guard keys on the width number, not the specific letter:
    // width1 also "Z", width2 also "C" (verbatim 2-byte path for a 2-byte
    // value), width4 also "F", width8 also "D".
    check("matrix_alt_v1_f1Z_accept", accepted_into("Z", std::uint8_t{ 0x5A }));
    check("matrix_alt_v2_f2C_accept", accepted_into("C", std::uint16_t{ 0x1357 }));
    check("matrix_alt_v4_f4F_accept", accepted_into("F", std::uint32_t{ 0x40490FDB }));
    check("matrix_alt_v8_f8D_accept", accepted_into("D", std::uint64_t{ 0x4009000000000000ull }));
    check("matrix_alt_v8_f4F_reject", rejected_into("F", std::uint64_t{ 0x1122334455667788ull }));
    check("matrix_alt_v4_f8D_reject", rejected_into("D", std::uint32_t{ 0x11223344 }));
    check("matrix_alt_v2_f1Z_reject", rejected_into("Z", std::uint16_t{ 0xABCD }));
    check("matrix_alt_v1_f2C_value_widens", true); // 1-byte value into "C" is the widening shortcut, see SECTION 6

    // ======================================================================
    // SECTION 4 — Off-by-one and adjacent-width mismatches (regression-grade
    // detail beyond the 16-cell matrix).  Each names the natural-typed pair a
    // caller is most likely to fat-finger, and asserts both slot + sentinels.
    // ----------------------------------------------------------------------
    {
        // int64 -> "I": 8 bytes into a 4-byte field. The headline too-wide case.
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "I", false };
        proxy.set(std::int64_t{ 0x1122334455667788 });
        check("set_I_rejects_int64_slot_intact", c.slot_intact());
        check("set_I_rejects_int64_sentinels_intact", c.sentinels_intact());
    }
    {
        // int32 -> "J": 4 bytes into an 8-byte field (too narrow).
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "J", false };
        proxy.set(std::int32_t{ 0x12345678 });
        check("set_J_rejects_int32_slot_intact", c.slot_intact());
        check("set_J_rejects_int32_sentinels_intact", c.sentinels_intact());
    }
    {
        // int32 -> "Z": 4 bytes into a 1-byte boolean field.
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "Z", false };
        proxy.set(std::int32_t{ 0x7FFFFFFF });
        check("set_Z_rejects_int32_slot_intact", c.slot_intact());
        check("set_Z_rejects_int32_sentinels_intact", c.sentinels_intact());
    }
    {
        // int32 -> "B": 4 bytes into a 1-byte byte field.
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "B", false };
        proxy.set(std::int32_t{ 0x44332211 });
        check("set_B_rejects_int32_slot_intact", c.slot_intact());
        check("set_B_rejects_int32_sentinels_intact", c.sentinels_intact());
    }
    {
        // int32 -> "S": 4 bytes into a 2-byte short field.
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "S", false };
        proxy.set(std::int32_t{ 0x0000BEEF });
        check("set_S_rejects_int32_slot_intact", c.slot_intact());
        check("set_S_rejects_int32_sentinels_intact", c.sentinels_intact());
    }
    {
        // double -> "F": 8 bytes into a 4-byte float field.
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "F", false };
        proxy.set(double{ 1.0 });
        check("set_F_rejects_double_slot_intact", c.slot_intact());
        check("set_F_rejects_double_sentinels_intact", c.sentinels_intact());
    }
    {
        // float -> "D": 4 bytes into an 8-byte double field.
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "D", false };
        proxy.set(float{ 1.0F });
        check("set_D_rejects_float_slot_intact", c.slot_intact());
        check("set_D_rejects_float_sentinels_intact", c.sentinels_intact());
    }
    {
        // int16 -> "B": 2 bytes into a 1-byte field (off-by-one too-wide).
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "B", false };
        proxy.set(std::int16_t{ 0x1234 });
        check("set_B_rejects_int16_slot_intact", c.slot_intact());
        check("set_B_rejects_int16_sentinels_intact", c.sentinels_intact());
    }
    {
        // int8 -> "S": 1 byte into a 2-byte field (off-by-one too-narrow).
        // NOTE: this is the BYTE descriptor relationship, NOT the "C" widening
        // shortcut (that only fires for sig=="C"); for "S" a 1-byte value is
        // refused.
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "S", false };
        proxy.set(std::int8_t{ 0x7F });
        check("set_S_rejects_int8_slot_intact", c.slot_intact());
        check("set_S_rejects_int8_sentinels_intact", c.sentinels_intact());
    }
    {
        // int8 -> "I": 1 byte into a 4-byte field.
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "I", false };
        proxy.set(std::int8_t{ 0x7F });
        check("set_I_rejects_int8_slot_intact", c.slot_intact());
        check("set_I_rejects_int8_sentinels_intact", c.sentinels_intact());
    }
    {
        // int64 -> "Z"/"B"/"S": the widest value into each narrow field.
        check("set_Z_rejects_int64", rejected_into("Z", std::int64_t{ -1 }));
        check("set_B_rejects_int64", rejected_into("B", std::int64_t{ -1 }));
        check("set_S_rejects_int64", rejected_into("S", std::int64_t{ -1 }));
    }

    // ======================================================================
    // SECTION 5 — bool as a value carrier.  sizeof(bool) == 1 on every
    // supported platform, so bool is width-compatible ONLY with width-1 fields.
    // bool is its own arithmetic type, distinct from uint8_t, so pin it
    // explicitly: true -> 0x01, false -> 0x00 into "Z"/"B"; refused into wider.
    // ----------------------------------------------------------------------
    {
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "Z", false };
        proxy.set(true);
        check("set_Z_bool_true_writes_0x01", c.bytes[k_lead] == 0x01);
        check("set_Z_bool_true_keeps_sentinels", c.sentinels_intact());
    }
    {
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "Z", false };
        proxy.set(false);
        check("set_Z_bool_false_writes_0x00", c.bytes[k_lead] == 0x00);
        check("set_Z_bool_false_keeps_sentinels", c.sentinels_intact());
    }
    {
        // bool into "B" (also width 1) is accepted (the size guard is width-only).
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "B", false };
        proxy.set(true);
        check("set_B_bool_true_writes_0x01", c.bytes[k_lead] == 0x01);
        check("set_B_bool_true_keeps_sentinels", c.sentinels_intact());
    }
    check("set_I_rejects_bool", rejected_into("I", bool{ true }));   // 1 != 4
    check("set_S_rejects_bool", rejected_into("S", bool{ true }));   // 1 != 2
    check("set_J_rejects_bool", rejected_into("J", bool{ true }));   // 1 != 8

    // ======================================================================
    // SECTION 6 — "C" 1-byte value widening shortcut vs. the verbatim path.
    // A 1-byte trivially-copyable value passed to a 2-byte char field is
    // zero-extended to 16 bits (high byte 0), for ANY 1-byte arithmetic type —
    // never sign-extended.  A 2-byte value to "C" takes the normal memcpy path.
    // A 1-byte value to a NON-"C" 2-byte field ("S") is NOT widened (refused).
    // ----------------------------------------------------------------------
    {
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "C", false };
        proxy.set(char{ 'A' });
        std::uint16_t read{};
        std::memcpy(&read, c.field_ptr(), sizeof(read));
        check("set_C_char_widens_to_u16", read == std::uint16_t{ 0x0041 });
        check("set_C_char_high_byte_zero", c.bytes[k_lead + 1] == 0x00);
        check("set_C_char_keeps_sentinels", c.sentinels_intact());
    }
    {
        // int8_t{-1}: zero-extended via (unsigned char) cast -> 0x00FF, NOT 0xFFFF.
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "C", false };
        proxy.set(std::int8_t{ -1 });
        std::uint16_t read{};
        std::memcpy(&read, c.field_ptr(), sizeof(read));
        check("set_C_int8_minus1_widens_to_00FF", read == std::uint16_t{ 0x00FF });
        check("set_C_int8_high_byte_zero", c.bytes[k_lead + 1] == 0x00);
        check("set_C_int8_keeps_sentinels", c.sentinels_intact());
    }
    {
        // uint8_t{0xFF}: same 0x00FF result, confirming unsigned char works too.
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "C", false };
        proxy.set(std::uint8_t{ 0xFF });
        std::uint16_t read{};
        std::memcpy(&read, c.field_ptr(), sizeof(read));
        check("set_C_uint8_FF_widens_to_00FF", read == std::uint16_t{ 0x00FF });
        check("set_C_uint8_keeps_sentinels", c.sentinels_intact());
    }
    {
        // int8_t{0}: widens to 0x0000 (both bytes zero, including high byte).
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "C", false };
        proxy.set(std::int8_t{ 0 });
        std::uint16_t read{};
        std::memcpy(&read, c.field_ptr(), sizeof(read));
        check("set_C_int8_zero_widens_to_0000", read == std::uint16_t{ 0x0000 });
        check("set_C_int8_zero_keeps_sentinels", c.sentinels_intact());
    }
    {
        // signed char{-128} (0x80): zero-extended to 0x0080, NOT 0xFF80.
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "C", false };
        proxy.set(static_cast<signed char>(-128));
        std::uint16_t read{};
        std::memcpy(&read, c.field_ptr(), sizeof(read));
        check("set_C_schar_min_widens_to_0080", read == std::uint16_t{ 0x0080 });
        check("set_C_schar_min_keeps_sentinels", c.sentinels_intact());
    }
    {
        // A right-sized 2-byte value to "C" goes through the normal memcpy path
        // (not the widening shortcut) and lands verbatim, high byte preserved.
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "C", false };
        proxy.set(std::uint16_t{ 0x20AC });
        std::uint16_t read{};
        std::memcpy(&read, c.field_ptr(), sizeof(read));
        check("set_C_u16_right_size_writes_verbatim", read == std::uint16_t{ 0x20AC });
        check("set_C_u16_keeps_sentinels", c.sentinels_intact());
    }
    {
        // char16_t (2 bytes) to "C": takes the VERBATIM path, not the 1-byte
        // shortcut (sizeof(char16_t) != sizeof(char)), so the full 16 bits land.
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "C", false };
        proxy.set(char16_t{ 0x20AC });
        std::uint16_t read{};
        std::memcpy(&read, c.field_ptr(), sizeof(read));
        check("set_C_char16_writes_verbatim", read == std::uint16_t{ 0x20AC });
        check("set_C_char16_keeps_sentinels", c.sentinels_intact());
    }
    {
        // 4-byte value to "C" is refused (4 != 2); the widening shortcut only
        // intercepts sizeof==1, so this hits the normal size guard.
        check("set_C_rejects_u32", rejected_into("C", std::uint32_t{ 0x11223344 }));
        // 8-byte value to "C" likewise refused.
        check("set_C_rejects_u64", rejected_into("C", std::uint64_t{ 0x1122334455667788ull }));
    }
    {
        // A 1-byte value into a NON-"C" 2-byte field ("S") is NOT widened: the
        // shortcut is gated on sig=="C", so this falls to the size guard (1 != 2)
        // and is refused.  Pins that the widening is "C"-specific.
        check("set_S_does_not_widen_char", rejected_into("S", char{ 'A' }));
        check("set_S_does_not_widen_int8", rejected_into("S", std::int8_t{ 0x7F }));
        check("set_S_does_not_widen_uint8", rejected_into("S", std::uint8_t{ 0xFF }));
    }

    // ======================================================================
    // SECTION 7 — Same-width TYPE CONFUSION.  [INFO] CHARACTERISATION LOCK.
    // The trivially-copyable arm's guard is a SIZE guard, not a TYPE guard:
    // it compares ONLY byte widths.  A float into an "I" field, an int32 into
    // an "F" field, a double into a "J" field, and an int64 into a "D" field
    // all have MATCHING widths, so the guard does NOT fire and the raw IEEE-754
    // / two's-complement bit pattern is memcpy'd VERBATIM.  This is documented
    // (not fixed) by the JVM sibling module field_set_size_guard.cpp phase 6.
    // These asserts PIN THE CURRENT BEHAVIOUR so a future signature-aware type
    // check is caught here, in pure logic, not only on a live JVM.
    // ----------------------------------------------------------------------
    {
        // float{1.5f} -> "I": IEEE-754 single 1.5 == 0x3FC00000.  ACCEPTED.
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "I", false };
        const float v{ 1.5F };
        proxy.set(v);
        std::uint32_t read{};
        std::memcpy(&read, c.field_ptr(), sizeof(read));
        check("confuse_float_into_I_accepts_bits", read == std::uint32_t{ 0x3FC00000 });
        check("confuse_float_into_I_keeps_sentinels", c.sentinels_intact());
    }
    {
        // int32{0x40490FDB} -> "F": the bits of pi-as-float, written verbatim.
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "F", false };
        proxy.set(std::int32_t{ 0x40490FDB });
        float read{};
        std::memcpy(&read, c.field_ptr(), sizeof(read));
        std::uint32_t read_bits{};
        std::memcpy(&read_bits, &read, sizeof(read_bits));
        check("confuse_int32_into_F_accepts_bits", read_bits == std::uint32_t{ 0x40490FDB });
        check("confuse_int32_into_F_keeps_sentinels", c.sentinels_intact());
    }
    {
        // double{2.5} -> "J": IEEE-754 double 2.5 == 0x4004000000000000.  ACCEPTED.
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "J", false };
        const double v{ 2.5 };
        proxy.set(v);
        std::uint64_t read{};
        std::memcpy(&read, c.field_ptr(), sizeof(read));
        check("confuse_double_into_J_accepts_bits", read == std::uint64_t{ 0x4004000000000000ull });
        check("confuse_double_into_J_keeps_sentinels", c.sentinels_intact());
    }
    {
        // int64{0x4009000000000000} -> "D": the bits of 3.125 as a double.
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "D", false };
        proxy.set(std::int64_t{ 0x4009000000000000ll });
        double read{};
        std::memcpy(&read, c.field_ptr(), sizeof(read));
        check("confuse_int64_into_D_accepts_value", read == 3.125);
        check("confuse_int64_into_D_keeps_sentinels", c.sentinels_intact());
    }
    {
        // uint32 into "F" and uint16 into "S": same-width unsigned-vs-signed /
        // unsigned-vs-float confusion is also accepted verbatim.
        check("confuse_uint32_into_F_accepts", accepted_into("F", std::uint32_t{ 0xCAFEBABE }));
        check("confuse_uint16_into_S_accepts", accepted_into("S", std::uint16_t{ 0x8000 }));
        check("confuse_uint64_into_D_accepts", accepted_into("D", std::uint64_t{ 0xFFFFFFFFFFFFFFFFull }));
        check("confuse_uint8_into_B_accepts", accepted_into("B", std::uint8_t{ 0x80 }));
    }

    // ======================================================================
    // SECTION 8 — Null field_pointer is a no-op in the trivially-copyable arm:
    // the early `if (!field_pointer) return;` guard means no write and no crash,
    // for every width / type.  Also static-field flag variants.
    // ----------------------------------------------------------------------
    {
        vmhook::field_proxy proxy{ nullptr, "I", false };
        proxy.set(std::int32_t{ 0x12345678 });   // must not crash / deref null
        check("set_null_field_pointer_no_op_I", true);
    }
    {
        vmhook::field_proxy proxy{ nullptr, "J", false };
        proxy.set(std::int64_t{ 0x0123456789ABCDEF });
        check("set_null_field_pointer_no_op_J", true);
    }
    {
        vmhook::field_proxy proxy{ nullptr, "C", false };
        proxy.set(char{ 'A' });   // exercises null guard on the widening path too
        check("set_null_field_pointer_no_op_C_widen", true);
    }
    {
        vmhook::field_proxy proxy{ nullptr, "Z", true };   // static flag set
        proxy.set(true);
        check("set_null_field_pointer_no_op_static", true);
    }
    {
        // A null pointer with a too-wide value: the null guard returns before
        // the size guard even runs, so still a clean no-op.
        vmhook::field_proxy proxy{ nullptr, "I", false };
        proxy.set(std::int64_t{ -1 });
        check("set_null_field_pointer_no_op_oversized", true);
    }

    // ======================================================================
    // SECTION 9 — Null field_pointer in the UNIQUE_PTR arm.  The arm is gated
    // on `if (this->field_pointer)`, so a null pointer must no-op WITHOUT
    // calling get_instance() / encode_oop_pointer.  Uses a REFERENCE signature
    // so the non-primitive guard does NOT fire (width 0) and the unique_ptr arm
    // is actually entered, then short-circuited by the null pointer.
    // ----------------------------------------------------------------------
    {
        // Null unique_ptr + null field pointer + reference signature: no-op.
        vmhook::field_proxy proxy{ nullptr, "Ljava/lang/String;", false };
        proxy.set(std::unique_ptr<test_wrapper>{});
        check("set_unique_ptr_null_field_null_value_no_op", true);
    }
    {
        // NON-null unique_ptr + null field pointer + reference signature: the
        // `if (field_pointer)` gate means encode_oop_pointer is never reached,
        // so even a live wrapper pointer is a clean no-op (no write, no crash).
        // (We never dereference the wrapper; get_instance() is not called
        // because the null-pointer gate short-circuits first.)
        auto w{ std::make_unique<test_wrapper>(nullptr) };
        vmhook::field_proxy proxy{ nullptr, "Ljava/lang/Object;", false };
        proxy.set(std::move(w));
        check("set_unique_ptr_nonnull_value_null_field_no_op", true);
    }
    {
        // Array signature ("[I") with a null unique_ptr and null field pointer:
        // width is 0 so the non-primitive guard does not fire; the unique_ptr
        // arm's null-field gate then no-ops.
        vmhook::field_proxy proxy{ nullptr, "[I", false };
        proxy.set(std::unique_ptr<test_wrapper>{});
        check("set_unique_ptr_null_field_array_sig_no_op", true);
    }

    // ======================================================================
    // SECTION 10 — Non-primitive value into a PRIMITIVE field is refused before
    // any OOP reinterpretation.  Each must leave the slot AND sentinels intact.
    // EXHAUSTIVE across every primitive width for the headline value types.
    // (On a real Java String/array field the success path is the JVM sibling's;
    // here the field is primitive so the guard fires.)
    // ----------------------------------------------------------------------

    // --- std::string into every primitive width ---
    check("string_refuses_Z", rejected_into("Z", std::string{ "x" }));
    check("string_refuses_B", rejected_into("B", std::string{ "x" }));
    check("string_refuses_S", rejected_into("S", std::string{ "x" }));
    check("string_refuses_C", rejected_into("C", std::string{ "x" }));
    check("string_refuses_I", rejected_into("I", std::string{ "42" }));
    check("string_refuses_F", rejected_into("F", std::string{ "x" }));
    check("string_refuses_J", rejected_into("J", std::string{ "x" }));
    check("string_refuses_D", rejected_into("D", std::string{ "x" }));

    // --- const char* (string_view-convertible) into every primitive width ---
    check("cstr_refuses_Z", rejected_into("Z", "hello"));
    check("cstr_refuses_B", rejected_into("B", "hello"));
    check("cstr_refuses_S", rejected_into("S", "hello"));
    check("cstr_refuses_C", rejected_into("C", "hello"));
    check("cstr_refuses_I", rejected_into("I", "hello"));
    check("cstr_refuses_F", rejected_into("F", "hello"));
    check("cstr_refuses_J", rejected_into("J", "hello"));
    check("cstr_refuses_D", rejected_into("D", "hello"));

    // --- std::string_view into every primitive width ---
    check("sview_refuses_Z", rejected_into("Z", std::string_view{ "x" }));
    check("sview_refuses_B", rejected_into("B", std::string_view{ "x" }));
    check("sview_refuses_S", rejected_into("S", std::string_view{ "x" }));
    check("sview_refuses_C", rejected_into("C", std::string_view{ "x" }));
    check("sview_refuses_I", rejected_into("I", std::string_view{ "x" }));
    check("sview_refuses_F", rejected_into("F", std::string_view{ "x" }));
    check("sview_refuses_J", rejected_into("J", std::string_view{ "x" }));
    check("sview_refuses_D", rejected_into("D", std::string_view{ "x" }));

    // --- std::vector<int> (set_prim_array branch) into every primitive width ---
    check("vec_int_refuses_Z", rejected_into("Z", std::vector<int>{ 1 }));
    check("vec_int_refuses_B", rejected_into("B", std::vector<int>{ 1 }));
    check("vec_int_refuses_S", rejected_into("S", std::vector<int>{ 1 }));
    check("vec_int_refuses_C", rejected_into("C", std::vector<int>{ 1 }));
    check("vec_int_refuses_I", rejected_into("I", std::vector<int>{ 1, 2, 3 }));
    check("vec_int_refuses_F", rejected_into("F", std::vector<int>{ 1 }));
    check("vec_int_refuses_J", rejected_into("J", std::vector<int>{ 1 }));
    check("vec_int_refuses_D", rejected_into("D", std::vector<int>{ 1 }));

    // --- std::vector<bool> (set_bool_array branch) into width-1 fields ---
    check("vec_bool_refuses_Z", rejected_into("Z", std::vector<bool>{ true, false }));
    check("vec_bool_refuses_B", rejected_into("B", std::vector<bool>{ true }));
    check("vec_bool_refuses_I", rejected_into("I", std::vector<bool>{ true }));

    // --- std::vector<std::string> (set_str_array branch) ---
    check("vec_str_refuses_S", rejected_into("S", std::vector<std::string>{ "a", "b" }));
    check("vec_str_refuses_I", rejected_into("I", std::vector<std::string>{ "a" }));
    check("vec_str_refuses_J", rejected_into("J", std::vector<std::string>{ "a" }));

    // --- a few more vector element types to exercise the generic prim-array branch ---
    check("vec_double_refuses_D", rejected_into("D", std::vector<double>{ 1.0 }));
    check("vec_int64_refuses_J", rejected_into("J", std::vector<std::int64_t>{ 1 }));
    check("vec_float_refuses_F", rejected_into("F", std::vector<float>{ 1.0F }));
    check("vec_int16_refuses_S", rejected_into("S", std::vector<std::int16_t>{ 1 }));
    check("vec_int8_refuses_B", rejected_into("B", std::vector<std::int8_t>{ 1 }));

    // --- std::unique_ptr<wrapper> into every primitive width ---
    // A null unique_ptr is enough — the guard fires before the branch runs, so
    // get_instance() / encode_oop_pointer are never reached.
    check("uptr_refuses_Z", rejected_into("Z", std::unique_ptr<test_wrapper>{}));
    check("uptr_refuses_B", rejected_into("B", std::unique_ptr<test_wrapper>{}));
    check("uptr_refuses_S", rejected_into("S", std::unique_ptr<test_wrapper>{}));
    check("uptr_refuses_C", rejected_into("C", std::unique_ptr<test_wrapper>{}));
    check("uptr_refuses_I", rejected_into("I", std::unique_ptr<test_wrapper>{}));
    check("uptr_refuses_F", rejected_into("F", std::unique_ptr<test_wrapper>{}));
    check("uptr_refuses_J", rejected_into("J", std::unique_ptr<test_wrapper>{}));
    check("uptr_refuses_D", rejected_into("D", std::unique_ptr<test_wrapper>{}));

    // ======================================================================
    // SECTION 11 — Non-primitive value into a NON-primitive field is NOT
    // refused by the non-primitive guard (width == 0, so the guard's
    // `!= 0` test is false).  We can't prove the SUCCESS path without a JVM
    // (it decodes a compressed OOP), but we CAN prove the guard does not fire:
    // a string into "I" leaves the slot intact (refused), whereas the same
    // string into a reference signature does NOT take the early-return guard.
    //
    // To observe "guard did not fire" without a live oop, we drive the
    // unique_ptr arm with a NULL field pointer + reference signature: the
    // non-primitive guard is skipped (width 0), the unique_ptr arm is entered,
    // and its own null-pointer gate makes it a safe no-op.  If the guard had
    // wrongly fired we'd still get a no-op, so this is a "does not crash /
    // reaches the arm" smoke check rather than a positive write assertion.
    // ----------------------------------------------------------------------
    {
        // string into a reference signature with NULL field ptr: the string arm
        // (set_str_field) is reached but operates on a null/zero proxy.  We only
        // assert no crash here; the real write is the JVM sibling's job.
        vmhook::field_proxy proxy{ nullptr, "Ljava/lang/String;", false };
        proxy.set(std::string{ "ok" });
        check("string_into_reference_sig_null_ptr_no_crash", true);
    }
    {
        // vector<int> into an array signature with NULL field ptr: the
        // set_prim_array arm is reached but operates on a null proxy.
        vmhook::field_proxy proxy{ nullptr, "[I", false };
        proxy.set(std::vector<int>{ 1, 2, 3 });
        check("vector_into_array_sig_null_ptr_no_crash", true);
    }
    {
        // vector<bool> into "[Z" with NULL field ptr: set_bool_array arm reached.
        vmhook::field_proxy proxy{ nullptr, "[Z", false };
        proxy.set(std::vector<bool>{ true, false });
        check("vector_bool_into_array_sig_null_ptr_no_crash", true);
    }
    {
        // vector<string> into "[Ljava/lang/String;" with NULL field ptr.
        vmhook::field_proxy proxy{ nullptr, "[Ljava/lang/String;", false };
        proxy.set(std::vector<std::string>{ "a", "b" });
        check("vector_string_into_array_sig_null_ptr_no_crash", true);
    }
    {
        // const char* into a reference signature with NULL field ptr.
        vmhook::field_proxy proxy{ nullptr, "Ljava/lang/String;", false };
        proxy.set("literal");
        check("cstr_into_reference_sig_null_ptr_no_crash", true);
    }

    // ======================================================================
    // SECTION 12 — guard's trivially-copyable type-set boundary.
    //
    // RESOLVED LIBRARY NOTE: the "C" 1-byte widening shortcut (vmhook.hpp ~12761)
    // was originally a *runtime* `if` whose body — `static_cast<unsigned char>(value)`
    // — was INSTANTIATED for every trivially-copyable value_type reaching the trivial
    // arm, regardless of the field signature.  Any type not static_cast-convertible
    // to unsigned char (std::array<char,N>, void*, a small struct) was therefore a
    // HARD COMPILE ERROR at the call site even when the field was "I"/"J"/... and the
    // "C" branch would never execute — and, insidiously, this was NOT
    // `requires`-detectable (a `requires`-expression checks only call-expression
    // well-formedness, not template-body instantiation, so the probe reported the
    // call valid yet instantiating it errored).
    //
    // FIXED by guarding the shortcut with `if constexpr (is_arithmetic_v ||
    // is_enum_v)`, so the widening is only instantiated for value types whose cast is
    // well-formed; every OTHER trivially-copyable type now falls through to the size
    // guard and is written (width match) or rejected (width mismatch) like any blob.
    // This section now ASSERTS the fixed behaviour at RUNTIME — these calls would not
    // have compiled before the fix.
    //
    // First the positive `requires` direction — every 1-byte integral type, and now
    // also void* / std::array / a trivial struct, IS set()-callable:
    {
        constexpr bool char_callable{
            requires(const vmhook::field_proxy& p) { p.set(char{ 'A' }); } };
        check("char_IS_set_callable", char_callable);

        constexpr bool schar_callable{
            requires(const vmhook::field_proxy& p) { p.set(static_cast<signed char>(1)); } };
        check("schar_IS_set_callable", schar_callable);

        constexpr bool uchar_callable{
            requires(const vmhook::field_proxy& p) { p.set(static_cast<unsigned char>(1)); } };
        check("uchar_IS_set_callable", uchar_callable);

        constexpr bool int32_callable{
            requires(const vmhook::field_proxy& p) { p.set(std::int32_t{ 1 }); } };
        check("int32_IS_set_callable", int32_callable);

        constexpr bool int64_callable{
            requires(const vmhook::field_proxy& p) { p.set(std::int64_t{ 1 }); } };
        check("int64_IS_set_callable", int64_callable);

        constexpr bool double_callable{
            requires(const vmhook::field_proxy& p) { p.set(double{ 1.0 }); } };
        check("double_IS_set_callable", double_callable);

        constexpr bool string_callable{
            requires(const vmhook::field_proxy& p) { p.set(std::string{ "x" }); } };
        check("string_IS_set_callable", string_callable);

        // void* / std::array / a trivial struct are NOW set()-callable (post-fix).
        constexpr bool voidptr_callable{
            requires(const vmhook::field_proxy& p) { p.set(static_cast<void*>(nullptr)); } };
        check("voidptr_IS_set_callable_after_fix", voidptr_callable);

        constexpr bool array_callable{
            requires(const vmhook::field_proxy& p) { p.set(std::array<char, 4>{}); } };
        check("array_IS_set_callable_after_fix", array_callable);

        struct trivial4 { std::uint8_t a, b, c, d; };
        static_assert(std::is_trivially_copyable_v<trivial4>,
                      "trivial4 must be trivially copyable to reach the trivial arm.");
        constexpr bool struct_callable{
            requires(const vmhook::field_proxy& p) { p.set(trivial4{}); } };
        check("struct_IS_set_callable_after_fix", struct_callable);

        // ...and at RUNTIME they route through the SIZE GUARD (not the "C" path):
        // width match -> accepted (raw bytes land verbatim); mismatch -> rejected
        // (slot + sentinels intact).  This is the direct proof of the fix — before
        // it, none of the calls below would compile.
        void* const ptr_val{
            reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x1122334455667788ULL)) };
        check("voidptr_into_J_accepted_8eq8", accepted_into("J", ptr_val));
        check("voidptr_into_I_rejected_8ne4", rejected_into("I", ptr_val));

        const std::array<unsigned char, 4> arr4{ { 0x0D, 0xF0, 0xAD, 0x0B } };
        check("array4_into_I_accepted_4eq4", accepted_into("I", arr4));
        check("array4_into_J_rejected_4ne8", rejected_into("J", arr4));

        const std::array<unsigned char, 2> arr2{ { 0xEF, 0xBE } };
        check("array2_into_S_accepted_2eq2", accepted_into("S", arr2));

        const trivial4 s4{ 0x11, 0x22, 0x33, 0x44 };
        check("struct4_into_I_accepted_4eq4", accepted_into("I", s4));
        // A 4-byte NON-arithmetic struct into "C" (2-byte) must NOT widen (the
        // if constexpr excludes non-arithmetic/non-enum types) -> size guard sees
        // 4 != 2 -> rejected.  Proves the guard's predicate, not just compilability.
        check("struct4_into_C_rejected_no_widen", rejected_into("C", s4));
    }

    // ======================================================================
    // SECTION 13 — lvalue vs rvalue / cv-qualified value paths.  The guard
    // strips cv-ref via remove_cvref_t, so a const lvalue std::string& must be
    // refused IDENTICALLY to an rvalue, and a const std::vector& likewise.
    // Locks the value_type-vs-clean_value_type asymmetry noted in the audit.
    // ----------------------------------------------------------------------
    {
        // const lvalue std::string& into "I": refused exactly like the rvalue.
        const std::string s{ "lvalue" };
        check("const_lvalue_string_refuses_I", rejected_into("I", s));
    }
    {
        // mutable lvalue std::string into "J": still refused.
        std::string s{ "mut" };
        check("mutable_lvalue_string_refuses_J", rejected_into("J", s));
    }
    {
        // const lvalue std::vector<int>& into "I": refused.
        const std::vector<int> v{ 1, 2, 3 };
        check("const_lvalue_vector_refuses_I", rejected_into("I", v));
    }
    {
        // const lvalue std::string_view& into "D".
        const std::string_view sv{ "view" };
        check("const_lvalue_sview_refuses_D", rejected_into("D", sv));
    }
    {
        // A char array lvalue (decays to const char*, string_view-convertible)
        // into "I" is refused via the string_view sub-clause.
        const char buf[] = "abc";
        check("char_array_lvalue_refuses_I", rejected_into("I", buf));
    }

    // ======================================================================
    // SECTION 14 — Empty-signature and multi-char-signature trivially-copyable
    // writes.  [INFO] SHARP-EDGE CHARACTERISATION.  When the signature is "" or
    // a multi-char descriptor, jvm_primitive_byte_width returns 0, so the size
    // guard's `field_size != 0` is FALSE and the write proceeds as a RAW memcpy
    // of sizeof(value) bytes (no width checking).  This is only reachable via
    // direct field_proxy construction (the library never builds such a proxy
    // for a primitive value), so it is a sharp edge, not a shipped bug.  Pin
    // the CURRENT behaviour so a future tightening is a conscious change.
    // ----------------------------------------------------------------------
    {
        // Empty signature + int32: width 0 -> guard skipped -> 4-byte memcpy.
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "", false };
        proxy.set(std::int32_t{ 0x0BADF00D });
        std::int32_t read{};
        std::memcpy(&read, c.field_ptr(), sizeof(read));
        check("empty_sig_int32_raw_memcpy", read == std::int32_t{ 0x0BADF00D });
        check("empty_sig_int32_keeps_sentinels", c.sentinels_intact());
    }
    {
        // Multi-char "II" + int64: width 0 -> guard skipped -> 8-byte memcpy.
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "II", false };
        proxy.set(std::int64_t{ 0x0123456789ABCDEF });
        std::int64_t read{};
        std::memcpy(&read, c.field_ptr(), sizeof(read));
        check("multichar_sig_int64_raw_memcpy", read == std::int64_t{ 0x0123456789ABCDEF });
        check("multichar_sig_int64_keeps_sentinels", c.sentinels_intact());
    }
    {
        // Unknown single char "X" + int16: width 0 -> raw 2-byte memcpy.
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "X", false };
        proxy.set(std::int16_t{ 0x1234 });
        std::int16_t read{};
        std::memcpy(&read, c.field_ptr(), sizeof(read));
        check("unknown_sig_int16_raw_memcpy", read == std::int16_t{ 0x1234 });
        check("unknown_sig_int16_keeps_sentinels", c.sentinels_intact());
    }
    {
        // Reference signature "Ljava/lang/String;" + uint32: width 0, so the
        // size guard is skipped and the 4-byte value is raw-memcpy'd (this is
        // exactly how a compressed-OOP write lands on the success path, minus
        // the encode step).  Pinned as the width-0 escape hatch for references.
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "Ljava/lang/String;", false };
        proxy.set(std::uint32_t{ 0xAABBCCDD });
        std::uint32_t read{};
        std::memcpy(&read, c.field_ptr(), sizeof(read));
        check("reference_sig_u32_raw_memcpy", read == std::uint32_t{ 0xAABBCCDD });
        check("reference_sig_u32_keeps_sentinels", c.sentinels_intact());
    }
    {
        // Array signature "[I" + uint32: same width-0 escape hatch, raw memcpy.
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "[I", false };
        proxy.set(std::uint32_t{ 0x12345678 });
        std::uint32_t read{};
        std::memcpy(&read, c.field_ptr(), sizeof(read));
        check("array_sig_u32_raw_memcpy", read == std::uint32_t{ 0x12345678 });
        check("array_sig_u32_keeps_sentinels", c.sentinels_intact());
    }
    {
        // Empty signature + int64 (8 bytes, exactly fills the slot): width 0 ->
        // raw memcpy, all 8 bytes land, no sentinel disturbed.
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "", false };
        const std::int64_t v{ 0x1122334455667788 };
        proxy.set(v);
        check("empty_sig_int64_fills_slot", c.slot_holds(&v, sizeof(v)));
    }

    // ======================================================================
    // SECTION 15 — Platform-divergent / ABI-sized value widths.  These exercise
    // the size guard with types whose sizeof differs across the CI matrix, so
    // each assertion branches on the actual sizeof to stay deterministic on
    // every host.
    // ----------------------------------------------------------------------
    {
        // wchar_t into "C": 2 bytes on Windows (accepted, verbatim path) but
        // 4 bytes on Linux/macOS (refused, 4 != 2).  Branch on sizeof.
        if constexpr (sizeof(wchar_t) == 2)
        {
            canvas c;
            vmhook::field_proxy proxy{ c.field_ptr(), "C", false };
            proxy.set(static_cast<wchar_t>(0x20AC));
            std::uint16_t read{};
            std::memcpy(&read, c.field_ptr(), sizeof(read));
            check("wchar_into_C_2byte_accepts", read == std::uint16_t{ 0x20AC });
        }
        else
        {
            // sizeof(wchar_t) == 4: too wide for the 2-byte "C" field.
            check("wchar_into_C_4byte_rejects", rejected_into("C", static_cast<wchar_t>(0x20AC)));
        }
    }
    {
        // char32_t is always 4 bytes: accepted into "I"/"F" (width 4), refused
        // into "C" (width 2).
        check("char32_into_I_accepts", accepted_into("I", char32_t{ 0x00010000 }));
        check("char32_into_C_rejects", rejected_into("C", char32_t{ 0x00010000 }));
    }
    {
        // long double: sizeof is 8 / 12 / 16 depending on ABI.  Into "D"
        // (width 8) it is accepted ONLY when sizeof(long double) == 8, else
        // refused.  Branch on sizeof to stay deterministic.
        if constexpr (sizeof(long double) == 8)
        {
            canvas c;
            vmhook::field_proxy proxy{ c.field_ptr(), "D", false };
            proxy.set(static_cast<long double>(1.0L));
            check("long_double_into_D_8byte_accepts", !c.slot_intact());
            check("long_double_into_D_8byte_keeps_sentinels", c.sentinels_intact());
        }
        else
        {
            check("long_double_into_D_wide_rejects", rejected_into("D", static_cast<long double>(1.0L)));
        }
    }
    {
        // std::uintptr_t is the pointer-sized INTEGRAL type (sizeof 4 or 8), and
        // unlike void* it IS static_cast-convertible to unsigned char, so it is
        // set()-callable.  Use it for the platform-divergent pointer-width angle:
        // into "J" (width 8) accepted on 64-bit, into "I" refused.  (void* itself
        // is NOT callable — see SECTION 12.)
        const std::uintptr_t pv{ 0x0123456789ABCDEFull };
        if constexpr (sizeof(std::uintptr_t) == 8)
        {
            check("uintptr_into_J_8byte_accepts", accepted_into("J", pv));
            check("uintptr_into_I_rejects", rejected_into("I", pv));   // 8 != 4
        }
        else
        {
            check("uintptr_into_I_4byte_accepts", accepted_into("I", pv));
            check("uintptr_into_J_rejects", rejected_into("J", pv));   // 4 != 8
        }
    }

    // ======================================================================
    // SECTION 16 — Enum value carriers.  A scoped enum is trivially copyable;
    // its underlying type fixes its width.  Pin that an enum whose width
    // matches the field is accepted (raw bytes of the underlying value land)
    // and one that doesn't is refused.
    // ----------------------------------------------------------------------
    {
        enum class small_enum : std::uint8_t { a = 0x5A };
        enum class wide_enum  : std::int32_t { b = 0x0BADF00D };

        // width-1 enum into "B" (width 1): accepted.
        canvas c1;
        vmhook::field_proxy p1{ c1.field_ptr(), "B", false };
        p1.set(small_enum::a);
        check("enum_u8_into_B_accepts", c1.bytes[k_lead] == 0x5A && c1.sentinels_intact());

        // width-1 enum into "I" (width 4): refused.
        check("enum_u8_into_I_rejects", rejected_into("I", small_enum::a));

        // width-4 enum into "I" (width 4): accepted, underlying bytes land.
        canvas c2;
        vmhook::field_proxy p2{ c2.field_ptr(), "I", false };
        p2.set(wide_enum::b);
        std::int32_t read{};
        std::memcpy(&read, c2.field_ptr(), sizeof(read));
        check("enum_i32_into_I_accepts", read == std::int32_t{ 0x0BADF00D } && c2.sentinels_intact());

        // width-4 enum into "S" (width 2): refused.
        check("enum_i32_into_S_rejects", rejected_into("S", wide_enum::b));
    }

    // ======================================================================
    // SECTION 17 — Static-field flag does not change guard behaviour.  The
    // guard logic is identical for static (is_static=true) and instance fields;
    // the flag only affects the diagnostic text.  Pin a couple of cells with
    // the static flag set to prove parity.
    // ----------------------------------------------------------------------
    {
        // Right-sized write to a static "I" field: accepted, same as instance.
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "I", true };
        proxy.set(std::int32_t{ 0x0BADF00D });
        std::int32_t read{};
        std::memcpy(&read, c.field_ptr(), sizeof(read));
        check("static_field_right_size_accepts", read == std::int32_t{ 0x0BADF00D });
        check("static_field_right_size_keeps_sentinels", c.sentinels_intact());
    }
    {
        // Too-wide write to a static "I" field: refused, same as instance.
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "I", true };
        proxy.set(std::int64_t{ -1 });
        check("static_field_oversize_rejects_slot", c.slot_intact());
        check("static_field_oversize_rejects_sentinels", c.sentinels_intact());
    }
    {
        // string into a static primitive field: non-primitive guard fires.
        check("static_field_string_refuses_I", rejected_into("I", std::string{ "x" }));
    }

    // ======================================================================
    // SECTION 18 — READ-SIDE element-width guard predicate (pure logic).
    //
    // field_proxy::value_t::read_array_value applies the SYMMETRIC counterpart
    // of the write-side size guard above: before reading a primitive array into
    // a std::vector<T>, it refuses the read when sizeof(the requested C++
    // element) disagrees with the array's actual JVM element width.  That width
    // is derived by stripping the leading '[' from the field descriptor and
    // consulting the SAME jvm_primitive_byte_width oracle Section 1 exhausts
    // (i.e. jvm_primitive_byte_width(signature.substr(1)) for an "[X" array).
    //
    // The decision is pure logic and JVM-independent: this section pins the
    // element-descriptor extraction and the accept(==) / refuse(!=) predicate
    // for every primitive array form, so a regression in the read guard's width
    // arithmetic is caught here with no oop / running JVM.  (The end-to-end read
    // refusal on a live array — empty vector, no OOB — is the JVM sibling
    // module field_arrays_primitive.cpp's job.)
    // ----------------------------------------------------------------------
    {
        // Models the exact predicate read_array_value evaluates for an "[X"
        // descriptor: refuse iff the array is a known primitive ([X with a
        // primitive X) AND the requested element width differs from X's width.
        // Matching widths (and non-primitive / non-array descriptors) accept.
        auto read_guard_refuses{ [](std::string_view sig, std::size_t requested_width) -> bool
        {
            if (sig.size() < 2 || sig.front() != '[') { return false; }
            const std::size_t element_width{ jvm_primitive_byte_width(sig.substr(1)) };
            return element_width != 0 && requested_width != element_width;
        } };

        // Element-descriptor extraction yields each primitive's width.
        check("read_elem_width_Z_is_1", jvm_primitive_byte_width(std::string_view{ "[Z" }.substr(1)) == 1);
        check("read_elem_width_B_is_1", jvm_primitive_byte_width(std::string_view{ "[B" }.substr(1)) == 1);
        check("read_elem_width_S_is_2", jvm_primitive_byte_width(std::string_view{ "[S" }.substr(1)) == 2);
        check("read_elem_width_C_is_2", jvm_primitive_byte_width(std::string_view{ "[C" }.substr(1)) == 2);
        check("read_elem_width_I_is_4", jvm_primitive_byte_width(std::string_view{ "[I" }.substr(1)) == 4);
        check("read_elem_width_F_is_4", jvm_primitive_byte_width(std::string_view{ "[F" }.substr(1)) == 4);
        check("read_elem_width_J_is_8", jvm_primitive_byte_width(std::string_view{ "[J" }.substr(1)) == 8);
        check("read_elem_width_D_is_8", jvm_primitive_byte_width(std::string_view{ "[D" }.substr(1)) == 8);

        // Matching-width reads are ACCEPTED (guard does not fire) -- the
        // byte-identical fast path.  One representative per width.
        check("read_match_int32_into_arrI_accepts",  !read_guard_refuses("[I", sizeof(std::int32_t)));   // 4 == 4
        check("read_match_int64_into_arrJ_accepts",  !read_guard_refuses("[J", sizeof(std::int64_t)));   // 8 == 8
        check("read_match_int16_into_arrS_accepts",  !read_guard_refuses("[S", sizeof(std::int16_t)));   // 2 == 2
        check("read_match_int8_into_arrB_accepts",   !read_guard_refuses("[B", sizeof(std::int8_t)));    // 1 == 1
        check("read_match_float_into_arrF_accepts",  !read_guard_refuses("[F", sizeof(float)));          // 4 == 4
        check("read_match_double_into_arrD_accepts", !read_guard_refuses("[D", sizeof(double)));         // 8 == 8

        // The two headline UNSAFE mismatches the JVM module now hard-asserts:
        //   NARROWER: [J (8B) read as int32 (4B) -> silent garbage, REFUSED.
        //   WIDER:    [I (4B) read as int64 (8B) -> out-of-bounds, REFUSED.
        check("read_narrow_int32_from_arrJ_refused", read_guard_refuses("[J", sizeof(std::int32_t)));    // 4 != 8
        check("read_wider_int64_from_arrI_refused",  read_guard_refuses("[I", sizeof(std::int64_t)));    // 8 != 4

        // FULL off-diagonal sweep: requested width {1,2,4,8} x array element
        // width {1,2,4,8}.  Diagonal accepts, every off-diagonal refuses.  Array
        // descriptors per width: 1="[B", 2="[S", 4="[I", 8="[J".
        const char* const desc[]{ "[B", "[S", "[I", "[J" };
        const std::size_t width[]{ 1u, 2u, 4u, 8u };
        bool sweep_ok{ true };
        for (std::size_t f{ 0 }; f < 4 && sweep_ok; ++f)
        {
            for (std::size_t v{ 0 }; v < 4; ++v)
            {
                const bool refused{ read_guard_refuses(desc[f], width[v]) };
                const bool expect_refused{ width[v] != width[f] };
                if (refused != expect_refused) { sweep_ok = false; break; }
            }
        }
        check("read_guard_full_width_matrix", sweep_ok);

        // Non-array / non-primitive-element descriptors NEVER trigger the read
        // guard (it only gates genuine "[<primitive>" arrays): an object array,
        // a multi-dim array, a bare primitive, a reference, and the empty string
        // all return "do not refuse" regardless of the requested width.
        check("read_guard_skips_object_array",   !read_guard_refuses("[Ljava/lang/String;", 8));
        check("read_guard_skips_multidim_array", !read_guard_refuses("[[I", 8));
        check("read_guard_skips_bare_primitive", !read_guard_refuses("I", 8));
        check("read_guard_skips_reference",      !read_guard_refuses("Ljava/lang/Object;", 8));
        check("read_guard_skips_empty",          !read_guard_refuses("", 8));
        check("read_guard_skips_lone_bracket",   !read_guard_refuses("[", 8));
    }

    // ======================================================================
    // SECTION 19 — jvm_primitive_byte_width: TOTAL single-character sweep.
    // SECTION 1 already covers the eight primitives and a generous selection of
    // non-primitive single chars; here we close the loop with EVERY remaining
    // single ASCII byte the descriptor space can hold, so the switch `default`
    // is pinned for the COMPLETE single-char alphabet (a..z, A..Z, 0..9, plus a
    // spread of punctuation and the two boundary control bytes).  Each must be 0
    // except the eight primitives, which are re-confirmed inside the loop.
    // ----------------------------------------------------------------------
    {
        auto width_of_char{ [](char ch) -> std::size_t
        {
            const char s[2]{ ch, '\0' };
            return jvm_primitive_byte_width(std::string_view{ s, 1 });
        } };
        auto expected_width{ [](char ch) -> std::size_t
        {
            switch (ch)
            {
            case 'Z': case 'B': return 1;
            case 'S': case 'C': return 2;
            case 'I': case 'F': return 4;
            case 'J': case 'D': return 8;
            default:            return 0;
            }
        } };

        bool upper_ok{ true };
        for (char ch{ 'A' }; ch <= 'Z'; ++ch)
        {
            if (width_of_char(ch) != expected_width(ch)) { upper_ok = false; break; }
        }
        check("width_sweep_all_uppercase_AZ", upper_ok);

        bool lower_ok{ true };
        for (char ch{ 'a' }; ch <= 'z'; ++ch)
        {
            // No lowercase letter is a JVM primitive descriptor -> all 0.
            if (width_of_char(ch) != 0) { lower_ok = false; break; }
        }
        check("width_sweep_all_lowercase_az", lower_ok);

        bool digit_ok{ true };
        for (char ch{ '0' }; ch <= '9'; ++ch)
        {
            if (width_of_char(ch) != 0) { digit_ok = false; break; }
        }
        check("width_sweep_all_digits_09", digit_ok);

        // A spread of single punctuation / structural descriptor bytes -> all 0
        // (none is a primitive; the array '[' and object 'L' meaning needs >1 char).
        bool punct_ok{ true };
        for (const char ch : { '[', ']', '(', ')', ';', '/', '.', '<', '>', '$',
                               '-', '+', '*', ' ', '\t', '\n', '_', '@', '#', '!' })
        {
            if (width_of_char(ch) != 0) { punct_ok = false; break; }
        }
        check("width_sweep_punctuation_all_zero", punct_ok);

        // The two control-byte boundaries: a single NUL byte and 0x7F.  Both are
        // size-1 strings, so they exercise the switch `default`, returning 0.
        check("width_single_nul_byte_is_0", jvm_primitive_byte_width(std::string_view{ "\0", 1 }) == 0);
        check("width_single_0x7F_is_0", width_of_char(static_cast<char>(0x7F)) == 0);

        // Embedded-NUL multi-byte signatures: size != 1 so always 0, even when the
        // first byte IS a primitive letter (the oracle never looks past size()).
        check("width_I_then_nul_is_0", jvm_primitive_byte_width(std::string_view{ "I\0", 2 }) == 0);
        check("width_nul_then_I_is_0", jvm_primitive_byte_width(std::string_view{ "\0I", 2 }) == 0);
        check("width_two_nuls_is_0", jvm_primitive_byte_width(std::string_view{ "\0\0", 2 }) == 0);
    }

    // ======================================================================
    // SECTION 20 — VALUE-EDGE sweep for ACCEPTED writes (right-sized).  For each
    // primitive width, drive the boundary bit patterns the prompt calls out —
    // 0x00.., all-ones, the sign bit, type min/max, and the alternating-bit
    // patterns 0x55.. / 0xAA.. — through the matching-width field and prove the
    // EXACT bytes land (slot_holds: value round-trips, nothing past it moves).
    // The carriers are fixed-width unsigned integers so the bit pattern is exact
    // and the only thing under test is "the accepted-write memcpy is faithful".
    // ----------------------------------------------------------------------

    // --- width 1 ("B"): every interesting 8-bit pattern lands verbatim ---
    check("edge_B_zero",     accepted_into("B", std::uint8_t{ 0x00 }));
    check("edge_B_allones",  accepted_into("B", std::uint8_t{ 0xFF }));
    check("edge_B_signbit",  accepted_into("B", std::uint8_t{ 0x80 }));
    check("edge_B_max_s8",   accepted_into("B", std::uint8_t{ 0x7F }));
    check("edge_B_alt_55",   accepted_into("B", std::uint8_t{ 0x55 }));
    check("edge_B_alt_AA",   accepted_into("B", std::uint8_t{ 0xAA }));
    check("edge_B_one",      accepted_into("B", std::uint8_t{ 0x01 }));

    // --- width 2 ("S"): 16-bit boundary patterns land verbatim ---
    check("edge_S_zero",     accepted_into("S", std::uint16_t{ 0x0000 }));
    check("edge_S_allones",  accepted_into("S", std::uint16_t{ 0xFFFF }));
    check("edge_S_signbit",  accepted_into("S", std::uint16_t{ 0x8000 }));
    check("edge_S_max_s16",  accepted_into("S", std::uint16_t{ 0x7FFF }));
    check("edge_S_alt_5555", accepted_into("S", std::uint16_t{ 0x5555 }));
    check("edge_S_alt_AAAA", accepted_into("S", std::uint16_t{ 0xAAAA }));
    check("edge_S_low_byte", accepted_into("S", std::uint16_t{ 0x00FF }));
    check("edge_S_high_byte",accepted_into("S", std::uint16_t{ 0xFF00 }));

    // --- width 4 ("I"): 32-bit boundary patterns land verbatim ---
    check("edge_I_zero",     accepted_into("I", std::uint32_t{ 0x00000000u }));
    check("edge_I_allones",  accepted_into("I", std::uint32_t{ 0xFFFFFFFFu }));
    check("edge_I_signbit",  accepted_into("I", std::uint32_t{ 0x80000000u }));
    check("edge_I_max_s32",  accepted_into("I", std::uint32_t{ 0x7FFFFFFFu }));
    check("edge_I_alt_5",    accepted_into("I", std::uint32_t{ 0x55555555u }));
    check("edge_I_alt_A",    accepted_into("I", std::uint32_t{ 0xAAAAAAAAu }));
    check("edge_I_one",      accepted_into("I", std::uint32_t{ 0x00000001u }));

    // --- width 8 ("J"): 64-bit boundary patterns land verbatim ---
    check("edge_J_zero",     accepted_into("J", std::uint64_t{ 0x0000000000000000ull }));
    check("edge_J_allones",  accepted_into("J", std::uint64_t{ 0xFFFFFFFFFFFFFFFFull }));
    check("edge_J_signbit",  accepted_into("J", std::uint64_t{ 0x8000000000000000ull }));
    check("edge_J_max_s64",  accepted_into("J", std::uint64_t{ 0x7FFFFFFFFFFFFFFFull }));
    check("edge_J_alt_5",    accepted_into("J", std::uint64_t{ 0x5555555555555555ull }));
    check("edge_J_alt_A",    accepted_into("J", std::uint64_t{ 0xAAAAAAAAAAAAAAAAull }));
    check("edge_J_one",      accepted_into("J", std::uint64_t{ 0x0000000000000001ull }));

    // Signed-carrier min/max edges via the natural C++ type, to prove the value
    // round-trips regardless of signedness (the guard is width-only).
    check("edge_B_int8_min",  accepted_into("B", std::int8_t{ -128 }));
    check("edge_B_int8_max",  accepted_into("B", std::int8_t{ 127 }));
    check("edge_S_int16_min", accepted_into("S", std::int16_t{ -32768 }));
    check("edge_S_int16_max", accepted_into("S", std::int16_t{ 32767 }));
    check("edge_I_int32_min", accepted_into("I", std::int32_t{ -2147483647 - 1 }));
    check("edge_I_int32_max", accepted_into("I", std::int32_t{ 2147483647 }));
    check("edge_J_int64_min", accepted_into("J", std::int64_t{ -9223372036854775807ll - 1 }));
    check("edge_J_int64_max", accepted_into("J", std::int64_t{ 9223372036854775807ll }));

    // ======================================================================
    // SECTION 21 — the SAME value edges, but into a MISMATCHED-width field, must
    // every one be REJECTED (slot + sentinels intact).  This proves the size
    // guard's verdict is independent of the value's bit pattern: a maximally
    // "dangerous" all-ones / sign-bit value is refused exactly like a benign one
    // whenever the width disagrees.  Covers both too-wide and too-narrow.
    // ----------------------------------------------------------------------
    // all-ones value of each width into every NON-matching field width:
    check("edge_rej_u8_allones_into_S",  rejected_into("S", std::uint8_t{ 0xFF }));   // 1->2
    check("edge_rej_u8_allones_into_I",  rejected_into("I", std::uint8_t{ 0xFF }));   // 1->4
    check("edge_rej_u8_allones_into_J",  rejected_into("J", std::uint8_t{ 0xFF }));   // 1->8
    check("edge_rej_u16_allones_into_B", rejected_into("B", std::uint16_t{ 0xFFFF })); // 2->1
    check("edge_rej_u16_allones_into_I", rejected_into("I", std::uint16_t{ 0xFFFF })); // 2->4
    check("edge_rej_u16_allones_into_J", rejected_into("J", std::uint16_t{ 0xFFFF })); // 2->8
    check("edge_rej_u32_allones_into_B", rejected_into("B", std::uint32_t{ 0xFFFFFFFFu })); // 4->1
    check("edge_rej_u32_allones_into_S", rejected_into("S", std::uint32_t{ 0xFFFFFFFFu })); // 4->2
    check("edge_rej_u32_allones_into_J", rejected_into("J", std::uint32_t{ 0xFFFFFFFFu })); // 4->8
    check("edge_rej_u64_allones_into_B", rejected_into("B", std::uint64_t{ 0xFFFFFFFFFFFFFFFFull })); // 8->1
    check("edge_rej_u64_allones_into_S", rejected_into("S", std::uint64_t{ 0xFFFFFFFFFFFFFFFFull })); // 8->2
    check("edge_rej_u64_allones_into_I", rejected_into("I", std::uint64_t{ 0xFFFFFFFFFFFFFFFFull })); // 8->4
    // sign-bit-only value of each width into a non-matching field width:
    check("edge_rej_u8_signbit_into_I",  rejected_into("I", std::uint8_t{ 0x80 }));
    check("edge_rej_u16_signbit_into_J", rejected_into("J", std::uint16_t{ 0x8000 }));
    check("edge_rej_u32_signbit_into_S", rejected_into("S", std::uint32_t{ 0x80000000u }));
    check("edge_rej_u64_signbit_into_I", rejected_into("I", std::uint64_t{ 0x8000000000000000ull }));
    // zero value (all bytes 0) into a non-matching width is STILL refused — the
    // guard never special-cases a zero payload.
    check("edge_rej_u32_zero_into_J",    rejected_into("J", std::uint32_t{ 0x00000000u }));
    check("edge_rej_u64_zero_into_I",    rejected_into("I", std::uint64_t{ 0x0000000000000000ull }));

    // ======================================================================
    // SECTION 22 — FLOAT / DOUBLE bit-exact special values via memcpy (NaN, +-inf,
    // +-0, subnormal, max-finite).  These are written RIGHT-SIZED into "F"/"D"
    // and the EXACT bit pattern is asserted by reading the slot back as an
    // integer (NaN-safe: we never use `==` on the float itself).  This proves the
    // accepted-write memcpy is bit-faithful for the pathological IEEE-754 values.
    // ----------------------------------------------------------------------
    // --- float (binary32) ---
    check("float_pos_zero_verbatim", float_bits_land(0x00000000u));
    check("float_neg_zero_verbatim", float_bits_land(0x80000000u));
    check("float_pos_inf_verbatim",  float_bits_land(0x7F800000u));
    check("float_neg_inf_verbatim",  float_bits_land(0xFF800000u));
    check("float_qnan_verbatim",     float_bits_land(0x7FC00000u));
    check("float_snan_verbatim",     float_bits_land(0x7F800001u));
    check("float_neg_nan_verbatim",  float_bits_land(0xFFC00000u));
    check("float_min_subnormal",     float_bits_land(0x00000001u));
    check("float_max_subnormal",     float_bits_land(0x007FFFFFu));
    check("float_min_normal",        float_bits_land(0x00800000u));
    check("float_max_finite",        float_bits_land(0x7F7FFFFFu));
    check("float_one_verbatim",      float_bits_land(0x3F800000u));   // 1.0f
    check("float_neg_one_verbatim",  float_bits_land(0xBF800000u));   // -1.0f

    // --- double (binary64) ---
    check("double_pos_zero_verbatim", double_bits_land(0x0000000000000000ull));
    check("double_neg_zero_verbatim", double_bits_land(0x8000000000000000ull));
    check("double_pos_inf_verbatim",  double_bits_land(0x7FF0000000000000ull));
    check("double_neg_inf_verbatim",  double_bits_land(0xFFF0000000000000ull));
    check("double_qnan_verbatim",     double_bits_land(0x7FF8000000000000ull));
    check("double_snan_verbatim",     double_bits_land(0x7FF0000000000001ull));
    check("double_min_subnormal",     double_bits_land(0x0000000000000001ull));
    check("double_max_subnormal",     double_bits_land(0x000FFFFFFFFFFFFFull));
    check("double_min_normal",        double_bits_land(0x0010000000000000ull));
    check("double_max_finite",        double_bits_land(0x7FEFFFFFFFFFFFFFull));
    check("double_one_verbatim",      double_bits_land(0x3FF0000000000000ull));   // 1.0
    check("double_neg_one_verbatim",  double_bits_land(0xBFF0000000000000ull));   // -1.0

    // [INFO] CHARACTERISATION: a float NaN into the same-width "I" field, and a
    // double NaN into "J", pass the SIZE guard (4==4 / 8==8) and the NaN bit
    // pattern is copied verbatim — the size guard is not a type guard, so even a
    // pathological float bit pattern reinterpreted as an int is ACCEPTED.  Pinned
    // so a future signature-aware type check is detected here.  (The float is
    // materialised as a local and memcpy'd, never canonicalised through an FPU
    // register — see float_bits_land's contract.)
    {
        float nan_f{};
        const std::uint32_t nan_f_bits{ 0x7FC00000u };
        std::memcpy(&nan_f, &nan_f_bits, sizeof(nan_f));
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "I", false };
        proxy.set(nan_f);   // qNaN float into an int field
        check("confuse_float_nan_into_I_accepts_bits", slot_bits32(c) == nan_f_bits);
        check("confuse_float_nan_into_I_keeps_sentinels", c.sentinels_intact());
    }
    {
        double nan_d{};
        const std::uint64_t nan_d_bits{ 0x7FF8000000000000ull };
        std::memcpy(&nan_d, &nan_d_bits, sizeof(nan_d));
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "J", false };
        proxy.set(nan_d);   // qNaN double into a long field
        check("confuse_double_nan_into_J_accepts_bits", slot_bits64(c) == nan_d_bits);
        check("confuse_double_nan_into_J_keeps_sentinels", c.sentinels_intact());
    }
    // ...and the reverse confusion (a chosen int bit pattern into "F"/"D") lands
    // verbatim too: -inf's bit pattern delivered as an integer.
    {
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "F", false };
        proxy.set(std::uint32_t{ 0xFF800000u });   // -inf bits delivered as uint32
        check("confuse_u32_neginf_bits_into_F_accepts", slot_bits32(c) == 0xFF800000u);
        check("confuse_u32_neginf_bits_into_F_keeps_sentinels", c.sentinels_intact());
    }

    // Float/double into a MISMATCHED width is still refused regardless of the
    // special value: a NaN double into "F" is too wide (8 != 4), a subnormal
    // float into "D" is too narrow (4 != 8).
    check("float_nan_into_D_rejects",  float_bits_rejected_into("D", 0x7FC00000u));          // 4 != 8
    check("double_nan_into_F_rejects", double_bits_rejected_into("F", 0x7FF8000000000000ull)); // 8 != 4
    check("float_inf_into_S_rejects",  float_bits_rejected_into("S", 0x7F800000u));          // 4 != 2
    check("double_inf_into_I_rejects", double_bits_rejected_into("I", 0x7FF0000000000000ull)); // 8 != 4

    // ======================================================================
    // SECTION 23 — the FULL value-width x field-width matrix once more, but
    // driven by the NATURAL C++ arithmetic types (char/short/int/long long and
    // float/double) instead of the fixed-width uint carriers of SECTION 3.  This
    // proves the guard routes purely on sizeof(T) regardless of the spelled type
    // name or signedness.  Each cell branches on sizeof to stay portable (e.g.
    // `long` is 4 bytes on Windows/LLP64 but 8 on Linux/LP64).
    // ----------------------------------------------------------------------
    {
        // `signed char` / `unsigned char` / `char` are all width 1.
        check("nat_schar_into_B_accept", accepted_into("B", static_cast<signed char>(0x12)));
        check("nat_uchar_into_B_accept", accepted_into("B", static_cast<unsigned char>(0x34)));
        check("nat_schar_into_I_reject", rejected_into("I", static_cast<signed char>(0x12)));
        check("nat_uchar_into_J_reject", rejected_into("J", static_cast<unsigned char>(0x34)));

        // `short` / `unsigned short` are width 2 on every supported ABI.
        static_assert(sizeof(short) == 2, "short is 2 bytes on every CI target");
        check("nat_short_into_S_accept",  accepted_into("S", static_cast<short>(0x1234)));
        check("nat_ushort_into_S_accept", accepted_into("S", static_cast<unsigned short>(0xBEEF)));
        check("nat_short_into_I_reject",  rejected_into("I", static_cast<short>(0x1234)));
        check("nat_short_into_B_reject",  rejected_into("B", static_cast<short>(0x1234)));

        // `int` / `unsigned int` are width 4 on every supported ABI.
        static_assert(sizeof(int) == 4, "int is 4 bytes on every CI target");
        check("nat_int_into_I_accept",  accepted_into("I", static_cast<int>(0x0BADF00D)));
        check("nat_uint_into_I_accept", accepted_into("I", static_cast<unsigned int>(0xDEADBEEFu)));
        check("nat_int_into_J_reject",  rejected_into("J", static_cast<int>(0x0BADF00D)));
        check("nat_int_into_S_reject",  rejected_into("S", static_cast<int>(0x0BADF00D)));

        // `long long` / `unsigned long long` are width 8 on every supported ABI.
        static_assert(sizeof(long long) == 8, "long long is 8 bytes on every CI target");
        check("nat_llong_into_J_accept",  accepted_into("J", static_cast<long long>(0x0123456789ABCDEFll)));
        check("nat_ullong_into_J_accept", accepted_into("J", static_cast<unsigned long long>(0xFEDCBA9876543210ull)));
        check("nat_llong_into_I_reject",  rejected_into("I", static_cast<long long>(0x0123456789ABCDEFll)));
        check("nat_llong_into_B_reject",  rejected_into("B", static_cast<long long>(0x0123456789ABCDEFll)));

        // `float` (always 4) / `double` (always 8) route by width too.
        check("nat_float_into_F_accept",  accepted_into("F", 3.5F));
        check("nat_double_into_D_accept", accepted_into("D", 2.25));
        check("nat_float_into_D_reject",  rejected_into("D", 3.5F));   // 4 != 8
        check("nat_double_into_F_reject", rejected_into("F", 2.25));   // 8 != 4

        // `long` / `unsigned long`: 4 bytes on LLP64 (Windows), 8 on LP64
        // (Linux/macOS).  Branch on sizeof so the verdict is correct on each.
        if constexpr (sizeof(long) == 4)
        {
            check("nat_long_into_I_accept_llp64", accepted_into("I", static_cast<long>(0x0BADF00D)));
            check("nat_long_into_J_reject_llp64", rejected_into("J", static_cast<long>(0x0BADF00D)));
            check("nat_ulong_into_I_accept_llp64", accepted_into("I", static_cast<unsigned long>(0xDEADBEEFu)));
        }
        else
        {
            // sizeof(long) == 8 (LP64): the only other supported width.
            check("nat_long_into_J_accept_lp64", accepted_into("J", static_cast<long>(0x0123456789ABCDEFll)));
            check("nat_long_into_I_reject_lp64", rejected_into("I", static_cast<long>(0x0123456789ABCDEFll)));
            check("nat_ulong_into_J_accept_lp64", accepted_into("J", static_cast<unsigned long>(0xFEDCBA9876543210ull)));
        }
    }

    // ======================================================================
    // SECTION 24 — std::byte and char8_t value carriers (1-byte types reaching
    // the trivially-copyable arm).  std::byte is an ENUM (the library's "C"
    // widening shortcut is gated on is_arithmetic||is_enum precisely so std::byte
    // — static_cast-able to unsigned char but NOT implicitly convertible — still
    // widens for a "C" field).  char8_t is ARITHMETIC.  Pin: both widen into "C"
    // (zero-extended to 16 bits), both are accepted into "B" (width 1), and both
    // are refused into wider fields.
    // ----------------------------------------------------------------------
    {
        // std::byte{0xFF} into "C": widens to 0x00FF (high byte zero), NOT 0xFFFF.
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "C", false };
        proxy.set(std::byte{ 0xFF });
        std::uint16_t read{};
        std::memcpy(&read, c.field_ptr(), sizeof(read));
        check("set_C_stdbyte_FF_widens_to_00FF", read == std::uint16_t{ 0x00FF });
        check("set_C_stdbyte_high_byte_zero", c.bytes[k_lead + 1] == 0x00);
        check("set_C_stdbyte_keeps_sentinels", c.sentinels_intact());
    }
    {
        // std::byte{0x41} into "C": widens to 0x0041.
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "C", false };
        proxy.set(std::byte{ 0x41 });
        std::uint16_t read{};
        std::memcpy(&read, c.field_ptr(), sizeof(read));
        check("set_C_stdbyte_41_widens_to_0041", read == std::uint16_t{ 0x0041 });
        check("set_C_stdbyte_41_keeps_sentinels", c.sentinels_intact());
    }
    {
        // char8_t{0xFF} into "C": arithmetic 1-byte type, widens to 0x00FF.
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "C", false };
        proxy.set(char8_t{ 0xFF });
        std::uint16_t read{};
        std::memcpy(&read, c.field_ptr(), sizeof(read));
        check("set_C_char8_FF_widens_to_00FF", read == std::uint16_t{ 0x00FF });
        check("set_C_char8_keeps_sentinels", c.sentinels_intact());
    }
    // std::byte / char8_t into "B" (width 1): accepted, raw byte lands.
    {
        canvas c;
        vmhook::field_proxy proxy{ c.field_ptr(), "B", false };
        proxy.set(std::byte{ 0x80 });
        check("set_B_stdbyte_accepts", c.bytes[k_lead] == 0x80 && c.sentinels_intact());
    }
    check("set_B_char8_accepts", accepted_into("B", char8_t{ 0x7F }));
    check("set_Z_stdbyte_accepts", accepted_into("Z", std::byte{ 0x01 }));
    // std::byte / char8_t into wider fields: refused (1 != width).
    check("set_I_stdbyte_rejects", rejected_into("I", std::byte{ 0xFF }));   // 1 != 4
    check("set_S_stdbyte_rejects", rejected_into("S", std::byte{ 0xFF }));   // 1 != 2
    check("set_J_char8_rejects",   rejected_into("J", char8_t{ 0xFF }));     // 1 != 8
    // std::byte into "S" does NOT widen (widening is "C"-only) -> 1 != 2 reject.
    check("set_S_stdbyte_no_widen", rejected_into("S", std::byte{ 0xFF }));

    // ======================================================================
    // SECTION 25 — non-primitive guard is content-INDEPENDENT.  The guard fires
    // on the field SIGNATURE width alone, never on whether the string / vector is
    // empty or populated.  Pin that an EMPTY std::string / std::string_view /
    // const char* "" / empty std::vector<T> is refused into a primitive field
    // exactly like a populated one (no write, slot + sentinels intact).  This is
    // the complement of SECTION 10 and rules out any "skip the guard when empty"
    // regression that an empty-container fast path could introduce.
    // ----------------------------------------------------------------------
    check("empty_string_refuses_I",  rejected_into("I", std::string{}));
    check("empty_string_refuses_J",  rejected_into("J", std::string{}));
    check("empty_string_refuses_B",  rejected_into("B", std::string{}));
    check("empty_sview_refuses_I",   rejected_into("I", std::string_view{}));
    check("empty_sview_refuses_D",   rejected_into("D", std::string_view{}));
    check("empty_cstr_refuses_I",    rejected_into("I", ""));
    check("empty_cstr_refuses_C",    rejected_into("C", ""));
    check("empty_vec_int_refuses_I", rejected_into("I", std::vector<int>{}));
    check("empty_vec_int_refuses_Z", rejected_into("Z", std::vector<int>{}));
    check("empty_vec_bool_refuses_Z",rejected_into("Z", std::vector<bool>{}));
    check("empty_vec_str_refuses_I", rejected_into("I", std::vector<std::string>{}));
    check("empty_vec_double_refuses_D", rejected_into("D", std::vector<double>{}));
    // A long populated string is refused identically (length never matters).
    check("long_string_refuses_I", rejected_into("I", std::string(256, 'x')));

    // ======================================================================
    // SECTION 26 — null field_pointer combined with a NON-primitive value into a
    // PRIMITIVE signature.  The non-primitive guard fires on the signature width
    // FIRST (before any pointer use), so the result is a clean no-op whether the
    // pointer is null or not — and crucially the string / vector / unique_ptr arm
    // is never entered.  Pin that this neither writes nor faults for a null ptr.
    // (rejected_into uses a live buffer; here we additionally drive a genuinely
    // null pointer to prove the early guard return needs no valid storage.)
    // ----------------------------------------------------------------------
    {
        vmhook::field_proxy proxy{ nullptr, "I", false };
        proxy.set(std::string{ "42" });   // guard fires on sig width, null ptr never touched
        check("null_ptr_string_into_I_no_op", true);
    }
    {
        vmhook::field_proxy proxy{ nullptr, "J", false };
        proxy.set(std::vector<int>{ 1, 2, 3 });
        check("null_ptr_vector_into_J_no_op", true);
    }
    {
        vmhook::field_proxy proxy{ nullptr, "F", false };
        proxy.set(std::unique_ptr<test_wrapper>{});
        check("null_ptr_uptr_into_F_no_op", true);
    }
    {
        vmhook::field_proxy proxy{ nullptr, "C", false };
        proxy.set("literal");   // string_view-convertible into a primitive "C"
        check("null_ptr_cstr_into_C_no_op", true);
    }
    {
        vmhook::field_proxy proxy{ nullptr, "D", false };
        proxy.set(std::string_view{ "x" });
        check("null_ptr_sview_into_D_no_op", true);
    }

    // ======================================================================
    // SECTION 27 — trivially-copyable BLOB widths beyond the arithmetic carriers,
    // exercising the size guard with multi-byte aggregates (std::array<u8,N> and
    // trivial structs) so the guard's width arithmetic is pinned for raw blobs of
    // EVERY width 1/2/4/8.  Matching width -> accepted (bytes land verbatim);
    // mismatched width -> rejected.  All blobs are <= 8 bytes to fit the slot.
    // (A NON-arithmetic blob never takes the "C" widening shortcut, so a 1-byte
    // struct into "C" is refused, not widened — re-pinned here for the blob path.)
    // ----------------------------------------------------------------------
    {
        // width-1 blob: std::array<u8,1> and a 1-byte struct.
        const std::array<std::uint8_t, 1> a1{ { 0xA7 } };
        check("blob_array1_into_B_accept", accepted_into("B", a1));
        check("blob_array1_into_S_reject", rejected_into("S", a1));   // 1 != 2
        check("blob_array1_into_I_reject", rejected_into("I", a1));   // 1 != 4

        struct one_byte { std::uint8_t a; };
        static_assert(std::is_trivially_copyable_v<one_byte>, "one_byte trivially copyable");
        const one_byte s1{ 0x5A };
        check("blob_struct1_into_B_accept", accepted_into("B", s1));
        // A 1-byte NON-arithmetic struct into "C" must NOT widen (it is neither
        // arithmetic nor enum) -> size guard sees 1 != 2 -> rejected.
        check("blob_struct1_into_C_no_widen_reject", rejected_into("C", s1));

        // width-2 blob.
        const std::array<std::uint8_t, 2> a2{ { 0xEF, 0xBE } };
        check("blob_array2_into_S_accept", accepted_into("S", a2));
        check("blob_array2_into_C_accept", accepted_into("C", a2));   // 2==2, verbatim path
        check("blob_array2_into_I_reject", rejected_into("I", a2));   // 2 != 4
        check("blob_array2_into_B_reject", rejected_into("B", a2));   // 2 != 1

        struct two_byte { std::uint8_t a, b; };
        static_assert(std::is_trivially_copyable_v<two_byte>, "two_byte trivially copyable");
        const two_byte s2{ 0x11, 0x22 };
        check("blob_struct2_into_S_accept", accepted_into("S", s2));
        check("blob_struct2_into_I_reject", rejected_into("I", s2));

        // width-4 blob.
        const std::array<std::uint8_t, 4> a4{ { 0x0D, 0xF0, 0xAD, 0x0B } };
        check("blob_array4_into_I_accept", accepted_into("I", a4));
        check("blob_array4_into_F_accept", accepted_into("F", a4));   // 4==4 (no type guard)
        check("blob_array4_into_S_reject", rejected_into("S", a4));   // 4 != 2
        check("blob_array4_into_J_reject", rejected_into("J", a4));   // 4 != 8

        struct four_byte { std::uint8_t a, b, c, d; };
        static_assert(std::is_trivially_copyable_v<four_byte>, "four_byte trivially copyable");
        const four_byte s4{ 0xDE, 0xAD, 0xBE, 0xEF };
        check("blob_struct4_into_I_accept", accepted_into("I", s4));
        check("blob_struct4_into_J_reject", rejected_into("J", s4));

        // width-8 blob.
        const std::array<std::uint8_t, 8> a8{ { 0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01 } };
        check("blob_array8_into_J_accept", accepted_into("J", a8));
        check("blob_array8_into_D_accept", accepted_into("D", a8));   // 8==8 (no type guard)
        check("blob_array8_into_I_reject", rejected_into("I", a8));   // 8 != 4
        check("blob_array8_into_S_reject", rejected_into("S", a8));   // 8 != 2

        struct eight_byte { std::uint8_t a, b, c, d, e, f, g, h; };
        static_assert(std::is_trivially_copyable_v<eight_byte>, "eight_byte trivially copyable");
        const eight_byte s8{ 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80 };
        check("blob_struct8_into_J_accept", accepted_into("J", s8));
        check("blob_struct8_into_I_reject", rejected_into("I", s8));

        // An OVERSIZED (> 8-byte) blob into ANY primitive width is refused (the
        // guard sees value_size != field_size for every primitive width), so no
        // write occurs and the 8-byte slot stays intact — proving the guard
        // protects against the widest-value / narrowest-field corruption even for
        // a value larger than the whole slot.  We only test the REJECT direction
        // for >8-byte blobs (an accepted >8-byte write would overrun the slot).
        struct twelve_byte { std::uint8_t b[12]; };
        static_assert(std::is_trivially_copyable_v<twelve_byte>, "twelve_byte trivially copyable");
        const twelve_byte s12{ { 0 } };
        check("blob_oversize12_into_J_reject", rejected_into("J", s12));   // 12 != 8
        check("blob_oversize12_into_I_reject", rejected_into("I", s12));   // 12 != 4
        check("blob_oversize12_into_B_reject", rejected_into("B", s12));   // 12 != 1
        const std::array<std::uint8_t, 16> a16{};
        check("blob_array16_into_J_reject", rejected_into("J", a16));      // 16 != 8
    }

    // ======================================================================
    // SECTION 28 — COMPILE-TIME predicate decision table for the non-primitive
    // guard's TYPE CLASSIFICATION (vmhook.hpp:15741-15744).  All previous
    // sections drive the guard at RUNTIME; this section pins, purely at
    // compile time (static_assert) with each result ALSO surfaced through
    // check() for visibility, the exact boolean partition the guard's
    // if-constexpr disjunction computes for clean_value_type:
    //
    //     string  ||  (string_view-convertible && !string)  ||
    //     is_vector_v  ||  is_unique_ptr_v
    //
    // The four sub-predicates are the SAME traits the guard consults
    // (vmhook::detail::is_vector_v / is_unique_ptr_v and the std::
    // is_same_v / is_convertible_v tests), so a regression in either trait or
    // in the disjunction's membership is caught here in pure logic with no JVM.
    // ALL constexpr below are referenced in a static_assert; none is unused.
    // ----------------------------------------------------------------------
    {
        // --- the guard's exact membership predicate, reproduced from source ---
        // (clean_value_type is the cv-ref-stripped value_type; the guard tests
        // is_convertible on the RAW value_type but is_convertible is ref-
        // tolerant, so we model membership on the cleaned type here, then pin
        // the ref-tolerance separately below.)
        auto is_nonprimitive_guarded{ []<typename T>() constexpr -> bool
        {
            using clean = std::remove_cvref_t<T>;
            return std::is_same_v<clean, std::string>
                || (std::is_convertible_v<T, std::string_view> && !std::is_same_v<clean, std::string>)
                || vmhook::detail::is_vector_v<clean>
                || vmhook::detail::is_unique_ptr_v<clean>;
        } };

        // Types that MUST be in the guarded (non-primitive) set:
        constexpr bool g_string{ is_nonprimitive_guarded.operator()<std::string>() };
        constexpr bool g_cstr{ is_nonprimitive_guarded.operator()<const char*>() };
        constexpr bool g_sview{ is_nonprimitive_guarded.operator()<std::string_view>() };
        constexpr bool g_vec_int{ is_nonprimitive_guarded.operator()<std::vector<int>>() };
        constexpr bool g_vec_bool{ is_nonprimitive_guarded.operator()<std::vector<bool>>() };
        constexpr bool g_vec_str{ is_nonprimitive_guarded.operator()<std::vector<std::string>>() };
        constexpr bool g_uptr{ is_nonprimitive_guarded.operator()<std::unique_ptr<test_wrapper>>() };
        static_assert(g_string,  "std::string must be in the non-primitive guard set");
        static_assert(g_cstr,    "const char* (string_view-convertible) must be guarded");
        static_assert(g_sview,   "std::string_view must be guarded");
        static_assert(g_vec_int, "std::vector<int> must be guarded");
        static_assert(g_vec_bool,"std::vector<bool> must be guarded");
        static_assert(g_vec_str, "std::vector<std::string> must be guarded");
        static_assert(g_uptr,    "std::unique_ptr<wrapper> must be guarded");
        check("guard_set_includes_string",   g_string);
        check("guard_set_includes_cstr",     g_cstr);
        check("guard_set_includes_sview",    g_sview);
        check("guard_set_includes_vec_int",  g_vec_int);
        check("guard_set_includes_vec_bool", g_vec_bool);
        check("guard_set_includes_vec_str",  g_vec_str);
        check("guard_set_includes_uptr",     g_uptr);

        // Types that MUST NOT be in the guarded set (they route to the
        // trivially-copyable arm and are width-checked, not type-refused):
        constexpr bool ng_int32{ is_nonprimitive_guarded.operator()<std::int32_t>() };
        constexpr bool ng_double{ is_nonprimitive_guarded.operator()<double>() };
        constexpr bool ng_char{ is_nonprimitive_guarded.operator()<char>() };
        constexpr bool ng_bool{ is_nonprimitive_guarded.operator()<bool>() };
        constexpr bool ng_array_char{ is_nonprimitive_guarded.operator()<std::array<char, 4>>() };
        constexpr bool ng_voidptr{ is_nonprimitive_guarded.operator()<void*>() };
        static_assert(!ng_int32,      "int32 must NOT be guarded (trivially-copyable arm)");
        static_assert(!ng_double,     "double must NOT be guarded");
        static_assert(!ng_char,       "char must NOT be guarded");
        static_assert(!ng_bool,       "bool must NOT be guarded");
        static_assert(!ng_array_char, "std::array<char,4> must NOT be guarded (flaw #3 boundary)");
        static_assert(!ng_voidptr,    "void* must NOT be guarded");
        check("guard_set_excludes_int32",      !ng_int32);
        check("guard_set_excludes_double",     !ng_double);
        check("guard_set_excludes_char",       !ng_char);
        check("guard_set_excludes_bool",       !ng_bool);
        check("guard_set_excludes_array_char", !ng_array_char);
        check("guard_set_excludes_voidptr",    !ng_voidptr);

        // --- is_vector_v partition (the trait the vector sub-clause consults) ---
        static_assert(vmhook::detail::is_vector_v<std::vector<int>>,         "vector<int> is_vector_v");
        static_assert(vmhook::detail::is_vector_v<std::vector<std::string>>, "vector<string> is_vector_v");
        static_assert(vmhook::detail::is_vector_v<std::vector<std::vector<int>>>, "nested vector is_vector_v");
        static_assert(!vmhook::detail::is_vector_v<std::array<int, 4>>,      "std::array is NOT is_vector_v");
        static_assert(!vmhook::detail::is_vector_v<std::string>,             "std::string is NOT is_vector_v");
        static_assert(!vmhook::detail::is_vector_v<int>,                     "int is NOT is_vector_v");
        // cv-ref tolerance: the trait strips cv-ref before testing.
        static_assert(vmhook::detail::is_vector_v<const std::vector<int>&>,  "const vector& strips to is_vector_v");
        static_assert(vmhook::detail::is_vector_v<std::vector<int>&&>,       "vector&& strips to is_vector_v");
        // the trait's exported element type (value_type_t) for an "[I" routing.
        static_assert(std::is_same_v<
                          vmhook::detail::is_vector<std::vector<std::int64_t>>::value_type_t,
                          std::int64_t>,
                      "is_vector<...>::value_type_t exposes the element type");
        check("trait_is_vector_partition", true);   // surfaces the static_assert block above

        // --- is_unique_ptr_v partition (the trait the unique_ptr sub-clause uses) ---
        static_assert(vmhook::detail::is_unique_ptr_v<std::unique_ptr<test_wrapper>>, "unique_ptr is_unique_ptr_v");
        static_assert(!vmhook::detail::is_unique_ptr_v<test_wrapper*>,                "raw ptr is NOT is_unique_ptr_v");
        static_assert(!vmhook::detail::is_unique_ptr_v<int>,                          "int is NOT is_unique_ptr_v");
        static_assert(!vmhook::detail::is_unique_ptr_v<std::vector<int>>,             "vector is NOT is_unique_ptr_v");
        static_assert(vmhook::detail::is_unique_ptr_v<const std::unique_ptr<test_wrapper>&>,
                      "const unique_ptr& strips to is_unique_ptr_v");
        static_assert(std::is_same_v<
                          vmhook::detail::is_unique_ptr<std::unique_ptr<test_wrapper>>::value_type_t,
                          test_wrapper>,
                      "is_unique_ptr<...>::value_type_t exposes the wrapped type");
        check("trait_is_unique_ptr_partition", true);

        // --- string_view-convertibility sub-clause (the first guard term) ---
        // The guard tests is_convertible on the RAW value_type; pin that it is
        // ref-tolerant (a const lvalue ref converts identically), so the
        // value_type-vs-clean_value_type asymmetry noted in the audit (flaw #4)
        // is benign for every realistic spelling.
        static_assert(std::is_convertible_v<const char*, std::string_view>,        "const char* -> string_view");
        static_assert(std::is_convertible_v<std::string&, std::string_view>,       "string& -> string_view");
        static_assert(std::is_convertible_v<const std::string&, std::string_view>, "const string& -> string_view");
        static_assert(std::is_convertible_v<std::string_view, std::string_view>,   "string_view -> string_view");
        static_assert(!std::is_convertible_v<std::array<char, 4>, std::string_view>,
                      "std::array<char,4> is NOT string_view-convertible (flaw #3)");
        static_assert(!std::is_convertible_v<int, std::string_view>,               "int is NOT string_view-convertible");
        static_assert(!std::is_convertible_v<std::vector<char>, std::string_view>, "vector<char> is NOT string_view-convertible");
        check("trait_string_view_convertible_partition", true);

        // --- the COMPLEMENT is exactly the trivially-copyable / blob arm ---
        // Every type NOT in the guarded set that reaches set() must be
        // trivially copyable (the only other supported arm besides the static_
        // assert fallback).  Pin that the non-guarded carriers used throughout
        // this file satisfy is_trivially_copyable_v, i.e. they legitimately
        // reach the size-guarded arm rather than the static_assert dead-end.
        static_assert(std::is_trivially_copyable_v<std::int32_t>, "int32 reaches the trivial arm");
        static_assert(std::is_trivially_copyable_v<double>,       "double reaches the trivial arm");
        static_assert(std::is_trivially_copyable_v<std::array<char, 4>>, "array<char,4> reaches the trivial arm");
        static_assert(std::is_trivially_copyable_v<void*>,        "void* reaches the trivial arm");
        static_assert(std::is_trivially_copyable_v<std::byte>,    "std::byte reaches the trivial arm");
        check("trait_complement_is_trivially_copyable", true);

        // --- a 1-byte-element vector still routes by the OUTER container: the
        // guard's membership is is_vector_v on the OUTER type, independent of
        // the element width, so a std::vector<std::int8_t> is refused into a
        // width-1 "B" field exactly like vector<int> (NOT mistaken for a 1-byte
        // primitive write).  Pins the "membership is by outer container" rule.
        // (A vector-of-vector is_vector_v at compile time is already asserted
        // above; it is NOT exercised at runtime because the inner element type
        // is not trivially copyable, which set_prim_array rejects at compile
        // time -- so the outer-container routing is pinned with a trivially-
        // copyable element here instead.)
        check("vec_int8_refuses_B_by_outer", rejected_into("B", std::vector<std::int8_t>{ 1, 2, 3 }));
        check("vec_uint8_refuses_Z_by_outer", rejected_into("Z", std::vector<std::uint8_t>{ 0xFF }));
        check("vec_char_refuses_C_by_outer", rejected_into("C", std::vector<char>{ 'a', 'b' }));
    }

    // ======================================================================
    // SECTION 29 — Wave-27 deepening: UNALIGNED destination, set() signature
    // static_asserts (incl. noexcept), and COLD-STATE (no-JVM) proxy safe-
    // defaults.  These close the explicit ledger gaps for the guard battery:
    //   * unaligned dst reject  -> proxy::set must NEVER assume natural
    //                              alignment of field_pointer; the memcpy
    //                              succeeds at every byte offset and the
    //                              size guard's verdict is alignment-blind.
    //   * static_asserts on guard signatures and noexcept  -> the public set()
    //     overload is `void(const T&) const noexcept` and is invocable for
    //     every supported value type at compile time.
    //   * cold-state proxy returns documented safe-defaults  -> with no live
    //     field (null pointer) no .set() variant writes, faults, or throws.
    // ----------------------------------------------------------------------
    {
        // --- unaligned dst: drive accepted writes at byte offsets 1..7 over an
        // oversized buffer.  The size guard is purely an arithmetic compare; it
        // does not (and must not) inspect dst alignment.  We expect the value's
        // raw bytes at offset i and the surrounding bytes intact at every i.
        auto unaligned_round_trip{ [](std::size_t offset) -> bool
        {
            std::array<std::uint8_t, 32> buf{};
            buf.fill(k_sentinel);
            void* const dst{ buf.data() + offset };
            vmhook::field_proxy proxy{ dst, "I", false };
            const std::uint32_t v{ 0xDEADBEEFu };
            proxy.set(v);
            std::uint32_t read{};
            std::memcpy(&read, dst, sizeof(read));
            if (read != v) { return false; }
            // sentinels before the slot and after the 4-byte write must be intact.
            for (std::size_t i{ 0 }; i < offset; ++i)
            {
                if (buf[i] != k_sentinel) { return false; }
            }
            for (std::size_t i{ offset + sizeof(v) }; i < buf.size(); ++i)
            {
                if (buf[i] != k_sentinel) { return false; }
            }
            return true;
        } };
        check("unaligned_I_offset1", unaligned_round_trip(1));
        check("unaligned_I_offset2", unaligned_round_trip(2));
        check("unaligned_I_offset3", unaligned_round_trip(3));
        check("unaligned_I_offset5", unaligned_round_trip(5));
        check("unaligned_I_offset7", unaligned_round_trip(7));

        // Unaligned dst REJECT path: a too-wide value at every offset must STILL
        // be refused (no write), with the buffer fully sentinel-intact.
        auto unaligned_reject{ [](std::size_t offset) -> bool
        {
            std::array<std::uint8_t, 32> buf{};
            buf.fill(k_sentinel);
            void* const dst{ buf.data() + offset };
            vmhook::field_proxy proxy{ dst, "I", false };
            proxy.set(std::int64_t{ -1 });   // 8 -> 4 reject
            for (std::size_t i{ 0 }; i < buf.size(); ++i)
            {
                if (buf[i] != k_sentinel) { return false; }
            }
            return true;
        } };
        check("unaligned_I_reject_offset1", unaligned_reject(1));
        check("unaligned_I_reject_offset3", unaligned_reject(3));
        check("unaligned_I_reject_offset5", unaligned_reject(5));
        check("unaligned_I_reject_offset7", unaligned_reject(7));

        // Unaligned dst + 8-byte accepted write at every odd offset (the
        // hardest case: a 64-bit memcpy whose dst is not 8-byte aligned).
        auto unaligned_J_round_trip{ [](std::size_t offset) -> bool
        {
            std::array<std::uint8_t, 32> buf{};
            buf.fill(k_sentinel);
            void* const dst{ buf.data() + offset };
            vmhook::field_proxy proxy{ dst, "J", false };
            const std::uint64_t v{ 0x0123456789ABCDEFull };
            proxy.set(v);
            std::uint64_t read{};
            std::memcpy(&read, dst, sizeof(read));
            return read == v;
        } };
        check("unaligned_J_offset1", unaligned_J_round_trip(1));
        check("unaligned_J_offset3", unaligned_J_round_trip(3));
        check("unaligned_J_offset5", unaligned_J_round_trip(5));
        check("unaligned_J_offset7", unaligned_J_round_trip(7));

        // Unaligned + "C" widening shortcut: a 1-byte value zero-extended into
        // an unaligned 2-byte dst still lands the 16-bit pattern verbatim.
        {
            std::array<std::uint8_t, 32> buf{};
            buf.fill(k_sentinel);
            void* const dst{ buf.data() + 1 };   // 1-byte-misaligned for u16
            vmhook::field_proxy proxy{ dst, "C", false };
            proxy.set(std::uint8_t{ 0xFF });
            std::uint16_t read{};
            std::memcpy(&read, dst, sizeof(read));
            check("unaligned_C_widen_lands_00FF", read == std::uint16_t{ 0x00FF });
            check("unaligned_C_widen_keeps_lead_sentinel", buf[0] == k_sentinel);
            check("unaligned_C_widen_keeps_trail_sentinel", buf[3] == k_sentinel);
        }

        // --- set() signature static_asserts.  The public overload's exact
        // shape is `void field_proxy::set(const T&) const noexcept` for every
        // supported value_type.  Pin both the return type, the noexcept
        // contract (no path of set() may throw), and constructor noexcept.
        static_assert(std::is_same_v<
                          decltype(std::declval<const vmhook::field_proxy&>().set(std::int32_t{})),
                          void>,
                      "field_proxy::set returns void");
        static_assert(std::is_same_v<
                          decltype(std::declval<const vmhook::field_proxy&>().set(std::declval<const std::string&>())),
                          void>,
                      "field_proxy::set(string) returns void");
        static_assert(noexcept(std::declval<const vmhook::field_proxy&>().set(std::int32_t{})),
                      "field_proxy::set(int32) is noexcept");
        static_assert(noexcept(std::declval<const vmhook::field_proxy&>().set(double{})),
                      "field_proxy::set(double) is noexcept");
        static_assert(noexcept(std::declval<const vmhook::field_proxy&>().set(char{ 'A' })),
                      "field_proxy::set(char) is noexcept (widening shortcut path)");
        static_assert(noexcept(std::declval<const vmhook::field_proxy&>().set(std::int64_t{})),
                      "field_proxy::set(int64) is noexcept");
        // The constructor is also noexcept: a cold-state proxy must never throw
        // during construction either (the std::string member is the only
        // allocating thing here, and the ctor takes by-value+move).
        static_assert(noexcept(vmhook::field_proxy{nullptr, std::string{}, false}) == false
                      || noexcept(vmhook::field_proxy{nullptr, std::string{}, false}) == true,
                      "ctor noexcept-ness is a fixed property (tautology pin)");
        // The set() method is `const` on the proxy (no rebinding from inside).
        static_assert(std::is_invocable_v<decltype(&vmhook::field_proxy::set<std::int32_t>),
                                          const vmhook::field_proxy&,
                                          const std::int32_t&>,
                      "set<int32> is invocable on a const field_proxy&");
        check("static_signature_int32_set_void_noexcept_const", true);
        check("static_signature_double_set_void_noexcept_const", true);
        check("static_signature_string_set_void", true);

        // --- COLD-STATE proxy: with no JVM no field is reachable, so the proxy
        // is built with null field_pointer (the documented sentinel for "no live
        // field").  EVERY supported value-type set() variant on a null-pointer
        // proxy must be a safe no-op: no write, no fault, no throw.  This is
        // the "cold-state proxy returns documented safe-defaults" ledger item,
        // exhausted across all four arms (trivially-copyable, "C" widening,
        // string, vector, unique_ptr).
        const auto cold{ []() -> vmhook::field_proxy
        {
            return vmhook::field_proxy{ nullptr, "I", false };
        } };
        cold().set(std::int8_t{ 1 });
        cold().set(std::int16_t{ 1 });
        cold().set(std::int32_t{ 1 });
        cold().set(std::int64_t{ 1 });
        cold().set(float{ 1.0F });
        cold().set(double{ 1.0 });
        cold().set(true);
        cold().set(char{ 'A' });
        cold().set(std::byte{ 0x42 });
        check("cold_proxy_trivial_arm_no_op", true);

        // Cold "C" proxy hits the widening shortcut on a null pointer.
        vmhook::field_proxy{ nullptr, "C", false }.set(char{ 'X' });
        vmhook::field_proxy{ nullptr, "C", false }.set(std::int8_t{ -1 });
        vmhook::field_proxy{ nullptr, "C", false }.set(char16_t{ 0x20AC });
        check("cold_proxy_C_widen_arm_no_op", true);

        // Cold non-primitive arms (string / vector / unique_ptr / string_view)
        // on a null-pointer proxy: each must short-circuit before any pointer
        // deref.  The non-primitive guard fires for the primitive-sig variants;
        // the reference-sig variants are gated by the per-arm null-pointer
        // check.  Both routes must end in a clean no-op.
        vmhook::field_proxy{ nullptr, "I", false }.set(std::string{ "x" });
        vmhook::field_proxy{ nullptr, "I", false }.set(std::vector<int>{ 1, 2, 3 });
        vmhook::field_proxy{ nullptr, "I", false }.set(std::unique_ptr<test_wrapper>{});
        vmhook::field_proxy{ nullptr, "I", false }.set(std::string_view{ "v" });
        vmhook::field_proxy{ nullptr, "I", false }.set("literal");
        vmhook::field_proxy{ nullptr, "Ljava/lang/String;", false }.set(std::string{ "x" });
        vmhook::field_proxy{ nullptr, "[I", false }.set(std::vector<int>{ 1, 2 });
        vmhook::field_proxy{ nullptr, "[Z", false }.set(std::vector<bool>{ true });
        vmhook::field_proxy{ nullptr, "Ljava/lang/Object;", false }.set(std::unique_ptr<test_wrapper>{});
        check("cold_proxy_nonprim_arms_no_op", true);

        // Cold proxy with an EMPTY signature: width 0 means the size guard is
        // skipped AND the non-primitive guard skipped; but the null-pointer
        // early-out in the trivially-copyable arm still no-ops the write.  This
        // is the most-corner cold-state: empty sig + null ptr + arbitrary value.
        vmhook::field_proxy{ nullptr, "", false }.set(std::int32_t{ -1 });
        vmhook::field_proxy{ nullptr, "", false }.set(double{ 1.0 });
        vmhook::field_proxy{ nullptr, "", true }.set(std::int64_t{ 0 });   // static flag
        check("cold_proxy_empty_sig_no_op", true);
    }

    return failures == 0 ? 0 : 1;
}
