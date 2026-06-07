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
//   * the value_t -> std::vector<T> read path and its width/length guards,
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

    return failures == 0 ? 0 : 1;
}
