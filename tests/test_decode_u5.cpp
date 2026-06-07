// Standalone unit tests for the HotSpot UNSIGNED5 decoder
// (vmhook::hotspot::klass::decode_u5) used to parse the JDK 21+
// InstanceKlass FieldInfoStream. No JVM is required: decode_u5 is a pure
// byte-buffer -> integer function, so every case here runs in-process by
// feeding it hand-built byte buffers and asserting both the decoded value
// and the number of bytes consumed (the in/out stream_pos).
//
// Decoder contract (verified against vmhook.hpp, mirroring
// src/hotspot/share/utilities/unsigned5.hpp):
//   auto decode_u5(const std::uint8_t* data, int& stream_pos) noexcept
//       -> std::uint32_t
//   * value = sum_i (b_i - 1) * 64^i  for i = 0, 1, 2, ...   (base-64, "excess-1")
//     i.e. each byte contributes (b_i - 1) << (6 * i).
//   * The sequence terminates at the first "low" byte b_i < 192.
//   * A "high"/continuation byte is b_i >= 192.
//   * Byte value 0 is never emitted by the encoder; it is the stream End
//     marker: decode_u5 returns ~0u and leaves stream_pos UNCHANGED (rewinds
//     the one byte it peeked).
//   * At most 5 bytes are read; after 5 continuation bytes the partial sum is
//     returned and stream_pos has advanced by exactly 5.
//
// Anything requiring a live oop / running JVM (find_field_in_stream walking a
// real InstanceKlass) is OUT OF SCOPE here and is covered by JVM integration
// in example.cpp.

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

// Decode one value from a fresh buffer starting at position 0.
// The buffer is padded so the decoder can always peek up to 5 bytes safely.
// On return, pos_out holds the bytes-consumed count.
static auto decode_at0(std::vector<std::uint8_t> bytes, int& pos_out) -> std::uint32_t
{
    bytes.resize(bytes.size() + 8, 0u);  // trailing padding (0 = harmless End)
    pos_out = 0;
    return vmhook::hotspot::klass::decode_u5(bytes.data(), pos_out);
}

// Convenience: decode one value, discarding the consumed-count.
static auto decode_value(std::vector<std::uint8_t> bytes) -> std::uint32_t
{
    int pos{ 0 };
    return decode_at0(std::move(bytes), pos);
}

int main()
{
    // -----------------------------------------------------------------
    // 1-byte encodings: value in [0 .. 190] maps to the single byte
    // (value + 1), because b_0 - 1 == value and b_0 < 192 terminates.
    // The smallest emittable byte is 1 (byte 0 is the End marker), so the
    // smallest representable 1-byte value is 0.
    // -----------------------------------------------------------------
    check("one_byte_zero",  decode_value({ 1 })   == 0u);    // 1   - 1
    check("one_byte_one",   decode_value({ 2 })   == 1u);    // 2   - 1
    check("one_byte_64",    decode_value({ 65 })  == 64u);   // 65  - 1
    check("one_byte_127",   decode_value({ 128 }) == 127u);  // 128 - 1
    check("one_byte_190",   decode_value({ 191 }) == 190u);  // 191 - 1 (last 1-byte value)

    // The boundary byte 191 is still a single "low" byte (191 < 192), so it
    // must consume exactly one byte, never two.
    {
        int pos{ 0 };
        const std::uint32_t v{ decode_at0({ 191 }, pos) };
        check("one_byte_191_value", v == 190u);
        check("one_byte_191_consumes_one", pos == 1);
    }

    // Every 1-byte decode advances stream_pos by exactly 1.
    {
        int pos{ 0 };
        (void)decode_at0({ 1 }, pos);
        check("one_byte_min_consumes_one", pos == 1);
    }

    // -----------------------------------------------------------------
    // 2-byte boundary: 191 is the first value needing two bytes.
    // b_0 = 192 is the lowest continuation byte ((192-1) == 191 contributed
    // at position 0), b_1 supplies the high base-64 digit:
    //   value = (192 - 1) + (b_1 - 1) * 64 = 191 + (b_1 - 1) * 64
    //   b_1 = 1 -> 191,  b_1 = 2 -> 255,  b_1 = 62 -> 4096
    // -----------------------------------------------------------------
    check("two_byte_191",  decode_value({ 192, 1 })  == 191u);   // 191 + 0*64
    check("two_byte_255",  decode_value({ 192, 2 })  == 255u);   // 191 + 1*64
    check("two_byte_4096", decode_value({ 193, 62 }) == 4096u);  // 192 + 61*64

    // A continuation byte (>= 192) in position 0 followed by a low byte must
    // consume exactly two bytes.
    {
        int pos{ 0 };
        const std::uint32_t v{ decode_at0({ 192, 1 }, pos) };
        check("two_byte_value", v == 191u);
        check("two_byte_consumes_two", pos == 2);
    }

    // Largest 2-byte value: both digits maxed without a third continuation.
    // b_0 = 255 (max low contribution 254 at pos 0 would terminate), so to
    // stay 2 bytes the high byte must be a low byte. Use b_0=255 (continuation,
    // 255 >= 192) and b_1=191 (low): value = 254 + 190*64 = 12414.
    {
        int pos{ 0 };
        const std::uint32_t v{ decode_at0({ 255, 191 }, pos) };
        check("two_byte_max_value", v == (254u + 190u * 64u));  // 12414
        check("two_byte_max_consumes_two", pos == 2);
    }

    // -----------------------------------------------------------------
    // 3-byte boundary: the first value needing three bytes is
    //   191 + 191*64 = 12415  ->  bytes [192, 192, 1]
    // First two bytes are continuation (>= 192), the third (1) is low.
    //   value = (192-1) + (192-1)*64 + (1-1)*4096 = 191 + 12224 + 0
    // -----------------------------------------------------------------
    {
        int pos{ 0 };
        const std::uint32_t v{ decode_at0({ 192, 192, 1 }, pos) };
        check("three_byte_boundary_value", v == 12415u);
        check("three_byte_boundary_value_math",
              v == (191u + 191u * 64u + 0u * 64u * 64u));  // proves 64^i layout
        check("three_byte_consumes_three", pos == 3);
    }

    // A non-trivial 3-byte value exercising the middle and high digits:
    //   [192, 200, 3] = 191 + (200-1)*64 + (3-1)*4096
    //                  = 191 + 12736 + 8192 = 21119
    {
        int pos{ 0 };
        const std::uint32_t v{ decode_at0({ 192, 200, 3 }, pos) };
        check("three_byte_mixed_value",
              v == (191u + 199u * 64u + 2u * 64u * 64u));  // 21119
        check("three_byte_mixed_consumes_three", pos == 3);
    }

    // -----------------------------------------------------------------
    // 4-byte boundary / stress: three continuation bytes then a low byte.
    //   [192, 192, 192, 2]
    //   = (192-1) + (192-1)*64 + (192-1)*64^2 + (2-1)*64^3
    //   = 191 + 12224 + 782336 + 262144 = 1056895
    // Every byte position must contribute, proving the 6*position shift.
    // -----------------------------------------------------------------
    {
        int pos{ 0 };
        const std::uint32_t v{ decode_at0({ 192, 192, 192, 2 }, pos) };
        check("four_byte_value",
              v == (191u + 191u * 64u + 191u * 64u * 64u + 1u * 64u * 64u * 64u));
        check("four_byte_value_literal", v == 1056895u);
        check("four_byte_consumes_four", pos == 4);
    }

    // -----------------------------------------------------------------
    // 5-byte values: four continuation bytes then a terminating low byte.
    //   [192, 192, 192, 192, 2]
    //   = 191 + 191*64 + 191*64^2 + 191*64^3 + (2-1)*64^4
    // 64^4 = 16777216, so this exceeds the 4-byte range and proves the
    // decoder reaches position 4. Consumes exactly 5 bytes.
    // -----------------------------------------------------------------
    {
        int pos{ 0 };
        const std::uint32_t v{ decode_at0({ 192, 192, 192, 192, 2 }, pos) };
        const std::uint32_t expected{
            191u
            + 191u * 64u
            + 191u * 64u * 64u
            + 191u * 64u * 64u * 64u
            + 1u * 64u * 64u * 64u * 64u };
        check("five_byte_value", v == expected);
        check("five_byte_consumes_five", pos == 5);
    }

    // 5-byte hard cap: when all five bytes are continuation bytes (>= 192),
    // the loop runs out at position 5 and returns the accumulated partial
    // sum WITHOUT reading a sixth byte. stream_pos advances by exactly 5.
    // This documents the bounded-read safety property of the decoder.
    {
        int pos{ 0 };
        const std::uint32_t v{ decode_at0({ 192, 192, 192, 192, 192 }, pos) };
        const std::uint32_t expected{
            191u
            + 191u * 64u
            + 191u * 64u * 64u
            + 191u * 64u * 64u * 64u
            + 191u * 64u * 64u * 64u * 64u };
        check("five_byte_all_continuation_value", v == expected);
        check("five_byte_all_continuation_caps_at_five", pos == 5);
    }

    // A 6th continuation byte beyond the 5-byte window must NOT be consumed:
    // place a sentinel low byte (1) at index 5 and confirm the value matches
    // the 5-continuation-byte result and pos stops at 5 (sentinel untouched).
    {
        int pos{ 0 };
        const std::uint32_t v{ decode_at0({ 192, 192, 192, 192, 192, 1 }, pos) };
        const std::uint32_t expected{
            191u
            + 191u * 64u
            + 191u * 64u * 64u
            + 191u * 64u * 64u * 64u
            + 191u * 64u * 64u * 64u * 64u };
        check("sixth_byte_not_consumed_value", v == expected);
        check("sixth_byte_not_consumed_pos", pos == 5);
    }

    // -----------------------------------------------------------------
    // End-of-stream marker: a leading 0 byte returns ~0u and rewinds, so
    // stream_pos is left UNCHANGED (the peeked byte is given back). This is
    // how the FieldInfoStream walker detects the trailing End(0) sentinel.
    // -----------------------------------------------------------------
    {
        int pos{ 0 };
        const std::uint32_t v{ decode_at0({ 0 }, pos) };
        check("end_marker_returns_all_ones", v == ~0u);
        check("end_marker_does_not_advance_pos", pos == 0);
    }

    // The End marker is distinct from the smallest real value (0 -> byte 1):
    // byte 1 decodes to 0 and advances, byte 0 decodes to ~0u and does not.
    {
        int pos_zero{ 0 };
        const std::uint32_t real_zero{ decode_at0({ 1 }, pos_zero) };
        int pos_end{ 0 };
        const std::uint32_t end{ decode_at0({ 0 }, pos_end) };
        check("zero_value_distinct_from_end_marker", real_zero != end);
        check("zero_value_advances_but_end_does_not",
              pos_zero == 1 && pos_end == 0);
    }

    // -----------------------------------------------------------------
    // Sequential decode: stream_pos is threaded across calls, so successive
    // values are read back-to-back from one buffer with no reset. This is the
    // exact usage pattern inside find_field_in_stream.
    //   [65, 192, 1, 3] -> 64, then 191 (2 bytes), then 2
    // -----------------------------------------------------------------
    {
        std::vector<std::uint8_t> stream{ 65, 192, 1, 3, 0, 0, 0, 0 };
        int pos{ 0 };
        const std::uint32_t v0{ vmhook::hotspot::klass::decode_u5(stream.data(), pos) };
        const std::uint32_t v1{ vmhook::hotspot::klass::decode_u5(stream.data(), pos) };
        const std::uint32_t v2{ vmhook::hotspot::klass::decode_u5(stream.data(), pos) };
        check("sequence_first_value",  v0 == 64u);
        check("sequence_second_value", v1 == 191u);
        check("sequence_third_value",  v2 == 2u);
        check("sequence_total_pos",    pos == 4);
    }

    // Sequential decode that ends on the End marker: after consuming two
    // values the third call hits the 0 sentinel, returns ~0u, and leaves
    // stream_pos parked at the sentinel (so a caller can stop the loop).
    {
        std::vector<std::uint8_t> stream{ 2, 192, 2, 0, 0, 0, 0, 0 };  // 1, 255, End
        int pos{ 0 };
        const std::uint32_t a{ vmhook::hotspot::klass::decode_u5(stream.data(), pos) };
        const std::uint32_t b{ vmhook::hotspot::klass::decode_u5(stream.data(), pos) };
        const int pos_before_end{ pos };
        const std::uint32_t end{ vmhook::hotspot::klass::decode_u5(stream.data(), pos) };
        check("sequence_then_end_first",  a == 1u);
        check("sequence_then_end_second", b == 255u);
        check("sequence_then_end_marker", end == ~0u);
        check("sequence_then_end_pos_parked", pos == pos_before_end);
        check("sequence_then_end_pos_value", pos == 3);
    }

    // =====================================================================
    // EXPANDED COVERAGE (additive — every expected value derived from the
    // decode_u5 loop body in vmhook.hpp:
    //   sum += (byte-1) << (6*pos); low byte (<192) terminates; byte 0 at ANY
    //   position is the End marker (returns ~0u and rewinds one byte); the loop
    //   reads at most 5 bytes).  decode_at0 pads the buffer with trailing zeros,
    //   which act as harmless End markers for the at-most-5-byte window.)
    // =====================================================================

    // ---- The 191/192 low-vs-continuation boundary, both directions ----------
    // 191 is the LARGEST low byte (191 < 192): a lone 191 is a complete 1-byte
    // value (190) consuming one byte.  192 is the SMALLEST continuation byte
    // (NOT < 192): a lone 192 followed by the trailing-zero padding makes the
    // decoder read the 0 at position 1 and treat it as the End marker -> ~0u,
    // with stream_pos rewound to 1 (the 192 was consumed, the 0 was given back).
    // This pins that 192 alone is an INCOMPLETE sequence, not the value 191.
    {
        int pos{ 0 };
        const std::uint32_t v{ decode_at0({ 192 }, pos) };
        check("lone_192_then_padding_is_end_marker", v == ~0u);
        check("lone_192_consumes_one_then_rewinds_to_one", pos == 1);
    }
    // 190 and 191 are adjacent low bytes -> values 189 and 190, each one byte.
    {
        int pos{ 0 };
        const std::uint32_t v{ decode_at0({ 190 }, pos) };
        check("one_byte_189_value", v == 189u);
        check("one_byte_189_consumes_one", pos == 1);
    }

    // ---- Interior 1-byte values across the whole 0..190 range ----------------
    // value == byte - 1 for every low byte; sample several interior points to
    // cover the full single-byte span between the already-tested endpoints.
    check("one_byte_2",   decode_value({ 3 })   == 2u);
    check("one_byte_31",  decode_value({ 32 })  == 31u);
    check("one_byte_63",  decode_value({ 64 })  == 63u);   // 64^1 boundary - 1
    check("one_byte_100", decode_value({ 101 }) == 100u);
    check("one_byte_128", decode_value({ 129 }) == 128u);
    check("one_byte_189", decode_value({ 190 }) == 189u);

    // ---- 2-byte: every "high digit" boundary value --------------------------
    // value = 191 + (b1 - 1) * 64 for a leading 192 (the minimum continuation,
    // contributing 191 at position 0).  Walk b1 = 1,2,3,...; each step adds 64.
    check("two_byte_b1_3_is_319",  decode_value({ 192, 3 })  == 191u + 2u * 64u);  // 319
    check("two_byte_b1_10_is_767", decode_value({ 192, 10 }) == 191u + 9u * 64u);  // 767
    // Leading 193 contributes 192 at position 0: value = 192 + (b1-1)*64.
    check("two_byte_lead_193_b1_1_is_192", decode_value({ 193, 1 }) == 192u);
    check("two_byte_lead_193_b1_2_is_256", decode_value({ 193, 2 }) == 256u);
    // The smallest 2-byte value (191) requires a continuation FIRST byte; the
    // SAME numeric value 191 also has a 1-byte-impossible form — pin that the
    // canonical 2-byte encoding {192,1} round-trips to 191 and the 1-byte
    // maximum {191} is the different value 190 (no aliasing across lengths).
    check("two_byte_min_191_distinct_from_one_byte_max_190",
          decode_value({ 192, 1 }) == 191u && decode_value({ 191 }) == 190u);

    // ---- 3-byte maximum (both leading digits maxed, final low byte 191) -----
    //   {255, 255, 191} = 254 + 254*64 + 190*4096
    {
        int pos{ 0 };
        const std::uint32_t v{ decode_at0({ 255, 255, 191 }, pos) };
        check("three_byte_max_value", v == (254u + 254u * 64u + 190u * 64u * 64u));  // 794750
        check("three_byte_max_literal", v == 794750u);
        check("three_byte_max_consumes_three", pos == 3);
    }

    // ---- 4-byte maximum -----------------------------------------------------
    //   {255, 255, 255, 191} = 254 + 254*64 + 254*4096 + 190*262144
    {
        int pos{ 0 };
        const std::uint32_t v{ decode_at0({ 255, 255, 255, 191 }, pos) };
        const std::uint32_t expected{
            254u + 254u * 64u + 254u * 64u * 64u + 190u * 64u * 64u * 64u };
        check("four_byte_max_value", v == expected);
        check("four_byte_max_literal", v == 50864254u);
        check("four_byte_max_consumes_four", pos == 4);
    }

    // ---- 5-byte maximum with a terminating low byte -------------------------
    //   {255, 255, 255, 255, 191}
    //   = 254 + 254*64 + 254*64^2 + 254*64^3 + 190*64^4
    // Still within uint32 range (3,255,312,510 < 2^32).  Consumes exactly 5.
    {
        int pos{ 0 };
        const std::uint32_t v{ decode_at0({ 255, 255, 255, 255, 191 }, pos) };
        const std::uint32_t expected{
            254u
            + 254u * 64u
            + 254u * 64u * 64u
            + 254u * 64u * 64u * 64u
            + 190u * 64u * 64u * 64u * 64u };
        check("five_byte_max_low_terminated_value", v == expected);
        check("five_byte_max_low_terminated_literal", v == 3255312510u);
        check("five_byte_max_low_terminated_consumes_five", pos == 5);
    }

    // ---- 5-byte all-0xFF: every byte a continuation, no terminating low ------
    // All five bytes are 255 (>= 192), so the loop runs to position 5 and
    // returns the accumulated sum WITHOUT reading a sixth byte.  The true sum
    // 254*(1+64+4096+262144+16777216) overflows uint32 and wraps; we build the
    // expected value with the SAME uint32_t shift arithmetic the decoder uses,
    // so the assertion pins the decoder's actual (wrapping) result rather than
    // an idealised big-integer value.
    {
        int pos{ 0 };
        const std::uint32_t v{ decode_at0({ 255, 255, 255, 255, 255 }, pos) };
        const std::uint32_t expected{
            (static_cast<std::uint32_t>(254u) << 0)
            + (static_cast<std::uint32_t>(254u) << 6)
            + (static_cast<std::uint32_t>(254u) << 12)
            + (static_cast<std::uint32_t>(254u) << 18)
            + (static_cast<std::uint32_t>(254u) << 24) };
        check("five_byte_all_0xFF_matches_uint32_shift_sum", v == expected);
        check("five_byte_all_0xFF_consumes_five", pos == 5);
    }

    // ---- End marker at EVERY interior position (0 byte after continuations) --
    // A 0 byte at ANY position (not just position 0) is the End marker: the
    // decoder rewinds the single peeked 0 and returns ~0u, leaving stream_pos
    // at the index of that 0.  So N leading continuation bytes (192) followed
    // by a 0 yields ~0u with stream_pos == N.  Covers N = 1, 2, 3, 4.
    {
        int pos{ 0 };
        const std::uint32_t v{ decode_at0({ 192, 0 }, pos) };
        check("end_marker_after_1_continuation_value", v == ~0u);
        check("end_marker_after_1_continuation_pos", pos == 1);
    }
    {
        int pos{ 0 };
        const std::uint32_t v{ decode_at0({ 192, 192, 0 }, pos) };
        check("end_marker_after_2_continuations_value", v == ~0u);
        check("end_marker_after_2_continuations_pos", pos == 2);
    }
    {
        int pos{ 0 };
        const std::uint32_t v{ decode_at0({ 192, 192, 192, 0 }, pos) };
        check("end_marker_after_3_continuations_value", v == ~0u);
        check("end_marker_after_3_continuations_pos", pos == 3);
    }
    {
        int pos{ 0 };
        const std::uint32_t v{ decode_at0({ 192, 192, 192, 192, 0 }, pos) };
        check("end_marker_after_4_continuations_value", v == ~0u);
        check("end_marker_after_4_continuations_pos", pos == 4);
    }

    // ---- Truncated input: continuation bytes that run into trailing zeros ----
    // decode_at0 pads with zeros, so a buffer that is "all continuation" but
    // shorter than 5 real bytes hits a padding 0 and returns the End marker.
    // This documents that an UNTERMINATED short sequence is reported as End,
    // not as a partial value (the partial-value path only triggers at the
    // 5-byte cap, tested above).  {193, 200} (no low terminator) -> pos 2 reads
    // the padding 0 -> ~0u, pos == 2.
    {
        int pos{ 0 };
        const std::uint32_t v{ decode_at0({ 193, 200 }, pos) };
        check("truncated_two_continuations_hits_padding_end", v == ~0u);
        check("truncated_two_continuations_pos_at_padding", pos == 2);
    }

    // ---- Low byte immediately after a maxed continuation digit --------------
    // {255, 1}: position 0 byte 255 (continuation, contributes 254), position 1
    // byte 1 is the SMALLEST low byte and contributes (1-1)<<6 = 0, terminating.
    // So {255,1} decodes to exactly 254 in two bytes — the minimum 2-byte value
    // reachable with a maxed first digit.
    {
        int pos{ 0 };
        const std::uint32_t v{ decode_at0({ 255, 1 }, pos) };
        check("maxed_first_digit_then_min_low_is_254", v == 254u);
        check("maxed_first_digit_then_min_low_consumes_two", pos == 2);
    }

    // ---- Position-2 digit weight, isolated ----------------------------------
    // Each position's contribution is (byte-1)<<(6*pos).  To REACH position 2
    // the bytes at positions 0 and 1 must both be continuation bytes (>= 192);
    // a low byte there would terminate early.  With two leading 192s (each
    // contributing 191) and a terminating low byte 2 at position 2:
    //   {192, 192, 2} = 191 + 191*64 + (2-1)*64^2 = 191 + 12224 + 4096 = 16511.
    // This pins the position-2 weight (4096) of a byte whose value is 2.
    {
        int pos{ 0 };
        const std::uint32_t v{ decode_at0({ 192, 192, 2 }, pos) };
        check("position_2_weight_is_4096",
              v == (191u + 191u * 64u + 1u * 64u * 64u));  // 16511
        check("position_2_weight_literal", v == 16511u);
        check("position_2_weight_consumes_three", pos == 3);
    }

    // ---- Sequential decode mixing 1-, 2- and 3-byte values + End -------------
    // Thread stream_pos across four reads from one buffer to confirm the
    // back-to-back usage pattern with varied lengths lands on the right cursor.
    //   [65, 193,62, 192,192,1, 0, ...]
    //     -> 64 (1B), 4096 (2B), 12415 (3B), then End (parks at the 0)
    {
        std::vector<std::uint8_t> stream{ 65, 193, 62, 192, 192, 1, 0, 0, 0, 0 };
        int pos{ 0 };
        const std::uint32_t v0{ vmhook::hotspot::klass::decode_u5(stream.data(), pos) };
        const std::uint32_t v1{ vmhook::hotspot::klass::decode_u5(stream.data(), pos) };
        const std::uint32_t v2{ vmhook::hotspot::klass::decode_u5(stream.data(), pos) };
        const int pos_before_end{ pos };
        const std::uint32_t end{ vmhook::hotspot::klass::decode_u5(stream.data(), pos) };
        check("mixed_seq_v0_is_64",     v0 == 64u);
        check("mixed_seq_v1_is_4096",   v1 == 4096u);
        check("mixed_seq_v2_is_12415",  v2 == 12415u);
        check("mixed_seq_total_pos_6",  pos_before_end == 6);
        check("mixed_seq_end_marker",   end == ~0u);
        check("mixed_seq_end_parks",    pos == 6);
    }

    return failures == 0 ? 0 : 1;
}
