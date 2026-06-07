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

// ---------------------------------------------------------------------------
// Independent reference (oracle) implementations of the HotSpot UNSIGNED5
// codec, transcribed DIRECTLY from src/hotspot/share/utilities/unsigned5.hpp
// (the upstream source vmhook.hpp mirrors).  These are deliberately written
// from the SPEC, not from vmhook's loop, so that comparing vmhook's decode_u5
// against them is a genuine cross-check rather than a tautology.
//
//   Constants:  X (excess) = 1,  L (low-byte count) = 191,  H = 64, lg_H = 6,
//               MAX_LENGTH = 5.
//   A "low" byte is  b < X + L == 192  (terminates the sequence).
//   A "high"/continuation byte is  b >= 192.
//   value = sum_i (b_i - X) * H^i   for i = 0..MAX_LENGTH-1.
// ---------------------------------------------------------------------------
namespace u5_oracle
{
    constexpr std::uint32_t kExcess{ 1u };     // X
    constexpr std::uint32_t kLow{ 191u };      // L
    constexpr std::uint32_t kBase{ 64u };      // H
    constexpr int           kShift{ 6 };       // lg_H
    constexpr int           kMaxLen{ 5 };      // MAX_LENGTH
    constexpr std::uint32_t kContinuation{ kExcess + kLow };  // 192: first high byte

    // Canonical encoder: produce the exact minimal byte sequence the HotSpot
    // UNSIGNED5 writer emits for `value` (write_uint).  Never emits a 0 byte,
    // so the result never aliases the decoder's End marker.
    static auto encode(std::uint32_t value) -> std::vector<std::uint8_t>
    {
        std::vector<std::uint8_t> out;
        if (value < kLow)
        {
            out.push_back(static_cast<std::uint8_t>(kExcess + value));
            return out;
        }
        std::uint32_t sum{ value };
        for (int i{ 0 }; ; ++i)
        {
            if (sum < kLow || i == kMaxLen - 1)
            {
                out.push_back(static_cast<std::uint8_t>(kExcess + sum));
                return out;
            }
            sum -= kLow;
            out.push_back(static_cast<std::uint8_t>(kExcess + kLow + (sum % kBase)));
            sum >>= kShift;
        }
    }

    // Reference decoder (read_uint), independent of vmhook.  Mirrors the same
    // at-most-5-byte window and low/high termination.  Does NOT model the
    // End(0) marker — feed it only encoder output (no 0 bytes).
    static auto decode(const std::uint8_t* data, int& pos) -> std::uint32_t
    {
        std::uint32_t sum{ 0 };
        for (int i{ 0 }; i < kMaxLen; ++i)
        {
            const std::uint32_t b{ data[pos++] };
            sum += (b - kExcess) << (kShift * i);
            if (b < kContinuation)
            {
                return sum;
            }
        }
        return sum;
    }

    // Convenience: number of bytes the canonical encoding of `value` occupies.
    static auto encoded_length(std::uint32_t value) -> std::size_t
    {
        return encode(value).size();
    }
}  // namespace u5_oracle

// Run vmhook's decode_u5 on the canonical encoding of `value` and report both
// the decoded result and the bytes consumed.  Pads with trailing zeros so the
// 5-byte peek window is always in-bounds.
static auto roundtrip_decode(std::uint32_t value, int& pos_out) -> std::uint32_t
{
    return decode_at0(u5_oracle::encode(value), pos_out);
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

    // #####################################################################
    // ##  EXHAUSTIVE WAVE 2 — every possible input class to decode_u5     ##
    // ##                                                                  ##
    // ##  Methodology: a spec-faithful reference codec (namespace         ##
    // ##  u5_oracle), transcribed independently from HotSpot's            ##
    // ##  unsigned5.hpp (X=1, L=191, H=64, MAX_LENGTH=5), provides the    ##
    // ##  canonical encoder and an independent reference decoder.  Every  ##
    // ##  sweep below compares vmhook's decode_u5 against this oracle so   ##
    // ##  the assertions are real cross-checks, not self-referential.     ##
    // ##  All expected literals are first pinned by hand against the      ##
    // ##  spec, THEN used to validate the oracle, THEN the oracle drives  ##
    // ##  the dense sweeps.                                               ##
    // #####################################################################

    // =====================================================================
    // (A) ORACLE SELF-VALIDATION — pin the oracle to hand-derived spec
    //     literals before trusting it as the sweep comparator.  Each of
    //     these byte sequences and values is independently verifiable from
    //     the unsigned5.hpp algorithm (value = sum (b_i - 1) * 64^i).
    // =====================================================================
    {
        // Canonical encodings the existing hand-written tests already use,
        // now reproduced by the oracle encoder (proves encode() is correct).
        const bool enc_ok{
            u5_oracle::encode(0u)          == std::vector<std::uint8_t>{ 1 }
         && u5_oracle::encode(1u)          == std::vector<std::uint8_t>{ 2 }
         && u5_oracle::encode(64u)         == std::vector<std::uint8_t>{ 65 }
         && u5_oracle::encode(127u)        == std::vector<std::uint8_t>{ 128 }
         && u5_oracle::encode(190u)        == std::vector<std::uint8_t>{ 191 }
         && u5_oracle::encode(191u)        == (std::vector<std::uint8_t>{ 192, 1 })
         && u5_oracle::encode(255u)        == (std::vector<std::uint8_t>{ 192, 2 })
         && u5_oracle::encode(4096u)       == (std::vector<std::uint8_t>{ 193, 62 })
         && u5_oracle::encode(12414u)      == (std::vector<std::uint8_t>{ 255, 191 })
         && u5_oracle::encode(12415u)      == (std::vector<std::uint8_t>{ 192, 192, 1 })
         && u5_oracle::encode(21119u)      == (std::vector<std::uint8_t>{ 192, 200, 3 })
         && u5_oracle::encode(794750u)     == (std::vector<std::uint8_t>{ 255, 255, 191 })
         && u5_oracle::encode(794751u)     == (std::vector<std::uint8_t>{ 192, 192, 192, 1 })
         && u5_oracle::encode(1056895u)    == (std::vector<std::uint8_t>{ 192, 192, 192, 2 })
         && u5_oracle::encode(50864254u)   == (std::vector<std::uint8_t>{ 255, 255, 255, 191 })
         && u5_oracle::encode(50864255u)   == (std::vector<std::uint8_t>{ 192, 192, 192, 192, 1 })
         && u5_oracle::encode(3255312510u) == (std::vector<std::uint8_t>{ 255, 255, 255, 255, 191 })
         && u5_oracle::encode(0xFFFFFFFFu) == (std::vector<std::uint8_t>{ 192, 254, 253, 253, 253 }) };
        check("oracle_encoder_matches_spec_literals", enc_ok);
    }
    {
        // The oracle decoder reproduces the same hand-derived values the
        // existing tests assert against vmhook (proves decode() is correct).
        const std::vector<std::uint8_t> a{ 192, 200, 3, 0, 0, 0, 0, 0 };
        const std::vector<std::uint8_t> b{ 255, 255, 255, 255, 191, 0, 0, 0 };
        int pa{ 0 }, pb{ 0 };
        const bool dec_ok{
            u5_oracle::decode(a.data(), pa) == 21119u && pa == 3
         && u5_oracle::decode(b.data(), pb) == 3255312510u && pb == 5 };
        check("oracle_decoder_matches_spec_literals", dec_ok);
    }

    // =====================================================================
    // (B) EXACT LENGTH-TRANSITION BOUNDARIES — for every N in 1..5, the
    //     largest value that fits in N bytes and the smallest that needs
    //     N+1.  These pin the precise base-64 "excess-191" carry points.
    //       len1: [0 .. 190]            (190 -> {191},          191 -> {192,1})
    //       len2: [191 .. 12414]        (12414 -> {255,191},    12415 -> {192,192,1})
    //       len3: [12415 .. 794750]     (794750 -> {255,255,191}, 794751 -> {192,192,192,1})
    //       len4: [794751 .. 50864254]  (50864254 -> {...,191},  50864255 -> {192,192,192,192,1})
    //       len5: [50864255 .. 2^32-1]  (entire remaining range encodes in 5 bytes)
    // =====================================================================
    {
        struct Boundary { const char* name; std::uint32_t value; std::size_t len; };
        const Boundary boundaries[]{
            { "len1_max_190",          190u,        1 },
            { "len2_min_191",          191u,        2 },
            { "len2_max_12414",        12414u,      2 },
            { "len3_min_12415",        12415u,      3 },
            { "len3_max_794750",       794750u,     3 },
            { "len4_min_794751",       794751u,     4 },
            { "len4_max_50864254",     50864254u,   4 },
            { "len5_min_50864255",     50864255u,   5 },
            { "len5_max_uint32",       0xFFFFFFFFu, 5 },
        };
        for (const auto& bnd : boundaries)
        {
            int pos{ 0 };
            const std::uint32_t decoded{ roundtrip_decode(bnd.value, pos) };
            std::string label_v{ std::string{ "boundary_" } + bnd.name + "_roundtrips" };
            std::string label_l{ std::string{ "boundary_" } + bnd.name + "_byte_length" };
            check(label_v.c_str(), decoded == bnd.value);
            check(label_l.c_str(),
                  static_cast<std::size_t>(pos) == bnd.len
               && u5_oracle::encoded_length(bnd.value) == bnd.len);
        }
        // Adjacency: each "max at len N" and "min at len N+1" differ by exactly 1
        // AND straddle the byte-length step.  This is the load-bearing carry test.
        check("transition_190_191_is_len1_to_len2",
              u5_oracle::encoded_length(190u) == 1 && u5_oracle::encoded_length(191u) == 2);
        check("transition_12414_12415_is_len2_to_len3",
              u5_oracle::encoded_length(12414u) == 2 && u5_oracle::encoded_length(12415u) == 3);
        check("transition_794750_794751_is_len3_to_len4",
              u5_oracle::encoded_length(794750u) == 3 && u5_oracle::encoded_length(794751u) == 4);
        check("transition_50864254_50864255_is_len4_to_len5",
              u5_oracle::encoded_length(50864254u) == 4 && u5_oracle::encoded_length(50864255u) == 5);
    }

    // =====================================================================
    // (C) COMPLETE 1-BYTE SPACE — every low byte 1..191 as a single byte.
    //     For all 191 valid single low bytes, decode({b}) == b - 1 and the
    //     cursor advances by exactly 1.  This closes the entire 1-byte input
    //     domain with zero gaps (byte 0 is End, tested separately; bytes
    //     192..255 are continuation, tested in the multi-byte sweeps).
    // =====================================================================
    {
        bool all_values_ok{ true };
        bool all_pos_ok{ true };
        int  first_bad{ -1 };
        for (int b{ 1 }; b <= 191; ++b)
        {
            int pos{ 0 };
            const std::uint32_t v{ decode_at0({ static_cast<std::uint8_t>(b) }, pos) };
            if (v != static_cast<std::uint32_t>(b - 1)) { all_values_ok = false; if (first_bad < 0) first_bad = b; }
            if (pos != 1)                               { all_pos_ok = false;    if (first_bad < 0) first_bad = b; }
        }
        if (!all_values_ok || !all_pos_ok)
        {
            std::printf("       (first failing single byte = %d)\n", first_bad);
        }
        check("every_single_low_byte_1_to_191_decodes_to_b_minus_1", all_values_ok);
        check("every_single_low_byte_1_to_191_consumes_exactly_one", all_pos_ok);
    }

    // =====================================================================
    // (D) DENSE ROUND-TRIP SWEEP — decode(encode(x)) == x for a contiguous
    //     block of small values 0..49999.  This crosses the len1->len2 and
    //     len2->len3 transitions thousands of times and exercises every
    //     low-digit/high-digit combination in those ranges.  Also asserts
    //     the consumed byte count equals the canonical encoded length for
    //     each x, and that vmhook agrees with the independent oracle decoder
    //     on every byte buffer.
    // =====================================================================
    {
        bool value_ok{ true };
        bool length_ok{ true };
        bool oracle_agrees{ true };
        std::uint32_t first_bad{ 0 };
        bool have_bad{ false };
        for (std::uint32_t x{ 0 }; x <= 49999u; ++x)
        {
            const std::vector<std::uint8_t> enc{ u5_oracle::encode(x) };
            // vmhook decode
            int vpos{ 0 };
            const std::uint32_t vdec{ roundtrip_decode(x, vpos) };
            // independent oracle decode on the same padded buffer
            std::vector<std::uint8_t> padded{ enc };
            padded.resize(padded.size() + 8, 0u);
            int opos{ 0 };
            const std::uint32_t odec{ u5_oracle::decode(padded.data(), opos) };

            if (vdec != x)                                          { value_ok = false;      if (!have_bad) { first_bad = x; have_bad = true; } }
            if (static_cast<std::size_t>(vpos) != enc.size())       { length_ok = false;     if (!have_bad) { first_bad = x; have_bad = true; } }
            if (vdec != odec || vpos != opos)                      { oracle_agrees = false; if (!have_bad) { first_bad = x; have_bad = true; } }
        }
        if (have_bad)
        {
            std::printf("       (first failing round-trip value = %u)\n", first_bad);
        }
        check("roundtrip_0_to_49999_decode_equals_value", value_ok);
        check("roundtrip_0_to_49999_consumes_canonical_length", length_ok);
        check("roundtrip_0_to_49999_vmhook_matches_oracle_decoder", oracle_agrees);
    }

    // =====================================================================
    // (E) STRIDED ROUND-TRIP OVER THE ENTIRE UINT32 RANGE — a coprime
    //     stride sweep visiting ~84k points spread across [0, 2^32) so that
    //     all five byte-lengths (including the full 5-byte high range) are
    //     covered, and decode(encode(x)) == x holds end to end.  This is the
    //     critical lossless-codec property: UNSIGNED5/5-byte covers every
    //     32-bit value with no truncation for CANONICAL encodings.
    //     Stride 51217 is coprime with 2^32 (it is odd), so the walk does
    //     not repeat until it wraps the whole space.
    // =====================================================================
    {
        bool value_ok{ true };
        bool oracle_agrees{ true };
        bool length_ok{ true };
        std::uint32_t first_bad{ 0 };
        bool have_bad{ false };
        std::uint32_t x{ 0 };
        constexpr std::uint32_t stride{ 51217u };
        for (long iter{ 0 }; iter < 84000; ++iter, x += stride)
        {
            const std::vector<std::uint8_t> enc{ u5_oracle::encode(x) };
            int vpos{ 0 };
            const std::uint32_t vdec{ roundtrip_decode(x, vpos) };
            std::vector<std::uint8_t> padded{ enc };
            padded.resize(padded.size() + 8, 0u);
            int opos{ 0 };
            const std::uint32_t odec{ u5_oracle::decode(padded.data(), opos) };

            if (vdec != x)                                    { value_ok = false;      if (!have_bad) { first_bad = x; have_bad = true; } }
            if (static_cast<std::size_t>(vpos) != enc.size()) { length_ok = false;     if (!have_bad) { first_bad = x; have_bad = true; } }
            if (vdec != odec || vpos != opos)                { oracle_agrees = false; if (!have_bad) { first_bad = x; have_bad = true; } }
        }
        if (have_bad)
        {
            std::printf("       (first failing strided value = %u)\n", first_bad);
        }
        check("strided_uint32_roundtrip_decode_equals_value", value_ok);
        check("strided_uint32_roundtrip_consumes_canonical_length", length_ok);
        check("strided_uint32_roundtrip_vmhook_matches_oracle", oracle_agrees);
    }

    // =====================================================================
    // (F) BOUNDARY-NEIGHBOURHOOD SWEEPS — dense +/- window around every
    //     length-transition point, where off-by-one carry bugs would hide.
    //     For each boundary B we sweep [B-4 .. B+4] and assert exact
    //     round-trip plus the expected encoded length on each side.
    // =====================================================================
    {
        const std::uint32_t boundary_points[]{
            190u, 191u,            // len1/len2
            12414u, 12415u,        // len2/len3
            794750u, 794751u,      // len3/len4
            50864254u, 50864255u,  // len4/len5
        };
        bool ok{ true };
        std::uint32_t first_bad{ 0 };
        bool have_bad{ false };
        for (const std::uint32_t bp : boundary_points)
        {
            const std::uint32_t lo{ bp >= 4u ? bp - 4u : 0u };
            for (std::uint32_t x{ lo }; x <= bp + 4u; ++x)
            {
                int pos{ 0 };
                const std::uint32_t v{ roundtrip_decode(x, pos) };
                if (v != x || static_cast<std::size_t>(pos) != u5_oracle::encoded_length(x))
                {
                    ok = false;
                    if (!have_bad) { first_bad = x; have_bad = true; }
                }
            }
        }
        if (have_bad)
        {
            std::printf("       (first failing boundary-neighbourhood value = %u)\n", first_bad);
        }
        check("boundary_neighbourhood_sweeps_all_roundtrip", ok);
    }

    // =====================================================================
    // (G) CONTINUATION-BYTE MECHANICS — each successive byte contributes
    //     its digit shifted by exactly 6*position bits.  We isolate every
    //     position 0..4 by holding all lower positions at their minimal
    //     continuation byte (192, which contributes 191 at that position)
    //     and placing a single terminating low byte (value 2 -> digit 1) at
    //     the position under test.  The decoded delta vs. the all-191-prefix
    //     baseline must equal exactly 1 << (6 * position) == 64^position.
    // =====================================================================
    {
        // Baseline contribution of K leading 192 bytes = sum_{i<K} 191 * 64^i.
        auto prefix_sum = [](int k) -> std::uint32_t {
            std::uint32_t s{ 0 };
            for (int i{ 0 }; i < k; ++i) { s += static_cast<std::uint32_t>(191u) << (6 * i); }
            return s;
        };
        // position 0: a single low byte 2 -> digit 1 at weight 64^0 == 1.
        {
            int pos{ 0 };
            const std::uint32_t v{ decode_at0({ 2 }, pos) };
            check("digit_weight_position_0_is_1", v == 1u && pos == 1);
        }
        // positions 1..4: (position) leading 192s then a terminating 2.
        for (int p{ 1 }; p <= 4; ++p)
        {
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(p), 192u);
            bytes.push_back(2u);  // digit 1 at position p
            int pos{ 0 };
            const std::uint32_t v{ decode_at0(bytes, pos) };
            const std::uint32_t expected{ prefix_sum(p) + (static_cast<std::uint32_t>(1u) << (6 * p)) };
            std::string lbl_v{ std::string{ "digit_weight_position_" } + std::to_string(p) + "_value" };
            std::string lbl_p{ std::string{ "digit_weight_position_" } + std::to_string(p) + "_consumes" };
            check(lbl_v.c_str(), v == expected);
            check(lbl_p.c_str(), pos == p + 1);
            // And the delta from the pure-prefix baseline equals exactly 64^p.
            std::vector<std::uint8_t> base_bytes(static_cast<std::size_t>(p), 192u);
            base_bytes.push_back(1u);  // digit 0 at position p -> contributes 0
            int bpos{ 0 };
            const std::uint32_t base_v{ decode_at0(base_bytes, bpos) };
            std::string lbl_d{ std::string{ "digit_weight_position_" } + std::to_string(p) + "_delta_is_64_pow_p" };
            check(lbl_d.c_str(), (v - base_v) == (static_cast<std::uint32_t>(1u) << (6 * p)));
        }
        // Sweep the position-1 digit fully (low byte 1..191 after one 192):
        // value must be 191 + (b1 - 1) * 64 for every b1, consuming 2 bytes.
        {
            bool ok{ true };
            int first_bad{ -1 };
            for (int b1{ 1 }; b1 <= 191; ++b1)
            {
                int pos{ 0 };
                const std::uint32_t v{ decode_at0({ 192u, static_cast<std::uint8_t>(b1) }, pos) };
                const std::uint32_t expected{ 191u + static_cast<std::uint32_t>(b1 - 1) * 64u };
                if (v != expected || pos != 2) { ok = false; if (first_bad < 0) first_bad = b1; }
            }
            if (!ok) { std::printf("       (first failing position-1 digit b1 = %d)\n", first_bad); }
            check("position_1_digit_sweep_1_to_191_all_correct", ok);
        }
    }

    // =====================================================================
    // (H) NON-ZERO START POSITION — every existing case starts at pos 0.
    //     Decode the SAME value from a mid-buffer offset and confirm the
    //     result is independent of start position and the cursor advances
    //     by the encoded length from wherever it began.  We prepend a
    //     variable run of filler low bytes, then decode at that offset.
    // =====================================================================
    {
        const std::uint32_t probe_values[]{ 0u, 1u, 190u, 191u, 4096u, 12415u, 794751u, 50864255u, 0xFFFFFFFFu };
        bool ok{ true };
        for (int start{ 0 }; start <= 7; ++start)
        {
            for (const std::uint32_t val : probe_values)
            {
                std::vector<std::uint8_t> buf(static_cast<std::size_t>(start), 1u);  // `start` filler bytes (each decodes to 0)
                const std::vector<std::uint8_t> enc{ u5_oracle::encode(val) };
                buf.insert(buf.end(), enc.begin(), enc.end());
                buf.resize(buf.size() + 8, 0u);  // peek padding
                int pos{ start };
                const std::uint32_t v{ vmhook::hotspot::klass::decode_u5(buf.data(), pos) };
                if (v != val || static_cast<std::size_t>(pos) != static_cast<std::size_t>(start) + enc.size())
                {
                    ok = false;
                }
            }
        }
        check("decode_from_nonzero_start_position_independent_of_offset", ok);
    }
    // Explicit single example mirroring the brief's "pos == 3 entry" request.
    {
        // buffer: [filler,filler,filler, <enc(4096)>, padding...], decode at pos 3.
        std::vector<std::uint8_t> buf{ 9, 9, 9, 193, 62, 0, 0, 0, 0, 0 };  // enc(4096) = {193,62}
        int pos{ 3 };
        const std::uint32_t v{ vmhook::hotspot::klass::decode_u5(buf.data(), pos) };
        check("decode_at_pos_3_value_is_4096", v == 4096u);
        check("decode_at_pos_3_advances_to_5", pos == 5);
    }

    // =====================================================================
    // (I) 32-BIT BOUNDARY + SENTINEL-ALIASING OBSERVATIONS.
    //     The canonical 5-byte encoding losslessly covers the ENTIRE uint32
    //     range.  In particular the literal value 0xFFFFFFFF encodes to
    //     {192,254,253,253,253} (a NON-zero leading byte, so NOT the End
    //     marker) and decodes back to 0xFFFFFFFF — which numerically equals
    //     the decoder's ~0u End sentinel.  This documents that the End
    //     marker is signalled by the literal 0 BYTE, never by a decoded
    //     value; the two are distinguished by the byte stream, not the
    //     numeric result.  (Real field-stream values never reach 0xFFFFFFFF,
    //     so there is no practical collision — see feature flaw #4.)
    // =====================================================================
    {
        const std::vector<std::uint8_t> enc_max{ u5_oracle::encode(0xFFFFFFFFu) };
        check("uint32_max_encodes_to_five_nonzero_bytes",
              enc_max.size() == 5 && enc_max[0] != 0u);
        int pos{ 0 };
        const std::uint32_t v{ roundtrip_decode(0xFFFFFFFFu, pos) };
        check("uint32_max_roundtrips_exactly", v == 0xFFFFFFFFu && pos == 5);
        check("uint32_max_decoded_value_numerically_equals_end_sentinel",
              v == ~0u);  // ~0u == 0xFFFFFFFF; aliasing is numeric only
        // But a real End marker (leading 0 byte) is reached with pos UNCHANGED,
        // whereas decoding 0xFFFFFFFF consumes 5 bytes — distinguishable.
        int end_pos{ 0 };
        const std::uint32_t end_v{ decode_at0({ 0 }, end_pos) };
        check("end_sentinel_distinguished_from_uint32_max_by_cursor",
              end_v == ~0u && end_pos == 0 && pos == 5);
    }
    // The largest value whose CANONICAL encoding is all-continuation-then-low
    // is {255,255,255,255,191} = 3255312510; just above it the encodings
    // start using the 5th byte as a high digit but still round-trip.
    {
        int p1{ 0 }, p2{ 0 };
        const std::uint32_t a{ roundtrip_decode(3255312510u, p1) };
        const std::uint32_t b{ roundtrip_decode(3255312511u, p2) };
        check("canonical_5byte_low_terminated_max_roundtrips", a == 3255312510u && p1 == 5);
        check("value_above_5byte_low_max_still_roundtrips", b == 3255312511u && p2 == 5);
    }

    // =====================================================================
    // (J) ADVERSARIAL / DEGENERATE BUFFERS.
    //     These are NON-canonical inputs (a real encoder never produces
    //     them) used to pin the decoder's documented behaviour on corrupt
    //     or truncated streams.  decode_at0 pads with trailing zeros, so a
    //     short all-continuation run hits a padding 0 and reports End.
    // =====================================================================
    {
        // All-zero buffer: the very first byte is the End marker; pos unchanged.
        int pos{ 0 };
        const std::uint32_t v{ decode_at0({ 0, 0, 0, 0, 0 }, pos) };
        check("all_zero_buffer_is_immediate_end", v == ~0u && pos == 0);
    }
    {
        // All-0xFF buffer (>= 5 bytes): never terminates within the window,
        // so the 5-byte cap returns the wrapping uint32 shift-sum and consumes
        // exactly 5.  Built with the same arithmetic the decoder uses.
        int pos{ 0 };
        const std::uint32_t v{ decode_at0({ 255, 255, 255, 255, 255, 255, 255 }, pos) };
        const std::uint32_t expected{
            (static_cast<std::uint32_t>(254u) << 0)
          + (static_cast<std::uint32_t>(254u) << 6)
          + (static_cast<std::uint32_t>(254u) << 12)
          + (static_cast<std::uint32_t>(254u) << 18)
          + (static_cast<std::uint32_t>(254u) << 24) };
        check("all_0xFF_buffer_caps_at_five_with_wrapping_sum", v == expected && pos == 5);
    }
    {
        // Truncated multi-byte sequence at the buffer tail: N continuation
        // bytes (N < 5) followed immediately by the End(0) padding.  The
        // decoder reads the padding 0 at position N and reports End, with the
        // cursor parked at the 0 (index N).  Covers N = 1..4.  This documents
        // that an UNTERMINATED short sequence is reported as End, not as a
        // partial value (the partial-value path only fires at the 5-byte cap).
        bool ok{ true };
        int first_bad{ -1 };
        for (int n{ 1 }; n <= 4; ++n)
        {
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(n), 192u);  // n continuations, no low terminator
            int pos{ 0 };
            const std::uint32_t v{ decode_at0(bytes, pos) };
            if (v != ~0u || pos != n) { ok = false; if (first_bad < 0) first_bad = n; }
        }
        if (!ok) { std::printf("       (first failing truncated-N = %d)\n", first_bad); }
        check("truncated_continuation_runs_report_end_at_padding", ok);
    }
    {
        // Maximal truncation: exactly 4 continuation bytes then End padding.
        // (5 would hit the cap instead — distinct, tested above.)
        int pos{ 0 };
        const std::uint32_t v{ decode_at0({ 255, 255, 255, 255 }, pos) };
        check("four_continuations_then_padding_is_end", v == ~0u && pos == 4);
    }

    // =====================================================================
    // (K) FULL CALLER GRAMMAR AGAINST A BYTE BUFFER (no JVM) — reproduce the
    //     find_field_in_stream decode pattern: a header (num_java,
    //     num_injected) followed by N field records, each 5 mandatory
    //     UNSIGNED5 values plus flag-gated optionals (0x01 initval, 0x04
    //     generic-sig, 0x10 contended-group), then the trailing End(0).
    //     Built entirely from the oracle encoder; we thread one cursor
    //     through every value exactly as the library does and assert the
    //     cursor lands precisely on the trailing 0 sentinel.
    // =====================================================================
    {
        // Three synthetic fields exercising every optional-presence combination.
        struct Field {
            std::uint32_t name_idx, sig_idx, offset, access_flags, field_flags;
            std::uint32_t initval, gsig, group;  // only read when the matching flag bit is set
        };
        const Field fields[]{
            // flags 0x00: no optionals
            { 3u,   5u,   16u,  0x0001u, 0x00u,  0u,    0u,   0u },
            // flags 0x01 (initval) | 0x04 (generic sig): two optionals
            { 7u,   9u,   24u,  0x0008u, 0x05u,  42u,   11u,  0u },
            // flags 0x10 (contended group) only: one optional, plus a big offset
            { 13u,  17u,  4096u, 0x0002u, 0x10u, 0u,    0u,   3u },
        };
        const std::uint32_t num_java{ 2u };
        const std::uint32_t num_injected{ 1u };

        std::vector<std::uint8_t> stream;
        auto emit = [&stream](std::uint32_t v) {
            const std::vector<std::uint8_t> e{ u5_oracle::encode(v) };
            stream.insert(stream.end(), e.begin(), e.end());
        };
        emit(num_java);
        emit(num_injected);
        for (const Field& f : fields)
        {
            emit(f.name_idx);
            emit(f.sig_idx);
            emit(f.offset);
            emit(f.access_flags);
            emit(f.field_flags);
            if (f.field_flags & 0x01u) { emit(f.initval); }
            if (f.field_flags & 0x04u) { emit(f.gsig); }
            if (f.field_flags & 0x10u) { emit(f.group); }
        }
        const std::size_t end_index{ stream.size() };  // where the End(0) will sit
        stream.push_back(0u);                           // End marker
        stream.resize(stream.size() + 8, 0u);           // peek padding

        // Now walk it exactly like find_field_in_stream does.
        int pos{ 0 };
        bool ok{ true };
        const std::uint32_t dec_num_java{ vmhook::hotspot::klass::decode_u5(stream.data(), pos) };
        const std::uint32_t dec_num_injected{ vmhook::hotspot::klass::decode_u5(stream.data(), pos) };
        if (dec_num_java != num_java || dec_num_injected != num_injected) { ok = false; }
        for (const Field& f : fields)
        {
            const std::uint32_t name_idx{ vmhook::hotspot::klass::decode_u5(stream.data(), pos) };
            const std::uint32_t sig_idx{ vmhook::hotspot::klass::decode_u5(stream.data(), pos) };
            const std::uint32_t offset{ vmhook::hotspot::klass::decode_u5(stream.data(), pos) };
            const std::uint32_t access_flags{ vmhook::hotspot::klass::decode_u5(stream.data(), pos) };
            const std::uint32_t field_flags{ vmhook::hotspot::klass::decode_u5(stream.data(), pos) };
            if (name_idx != f.name_idx || sig_idx != f.sig_idx || offset != f.offset
             || access_flags != f.access_flags || field_flags != f.field_flags) { ok = false; }
            if (field_flags & 0x01u)
            {
                if (vmhook::hotspot::klass::decode_u5(stream.data(), pos) != f.initval) { ok = false; }
            }
            if (field_flags & 0x04u)
            {
                if (vmhook::hotspot::klass::decode_u5(stream.data(), pos) != f.gsig) { ok = false; }
            }
            if (field_flags & 0x10u)
            {
                if (vmhook::hotspot::klass::decode_u5(stream.data(), pos) != f.group) { ok = false; }
            }
        }
        check("caller_grammar_header_and_fields_decode_correctly", ok);
        check("caller_grammar_cursor_lands_on_trailing_end_index",
              static_cast<std::size_t>(pos) == end_index);
        // The next decode hits the End(0) and parks the cursor (loop-stop).
        const int pos_before_end{ pos };
        const std::uint32_t end{ vmhook::hotspot::klass::decode_u5(stream.data(), pos) };
        check("caller_grammar_next_decode_is_end_marker", end == ~0u);
        check("caller_grammar_end_marker_parks_cursor", pos == pos_before_end);
    }

    // =====================================================================
    // (L) EMPTY STREAM — header (0 java, 0 injected fields) immediately
    //     followed by End(0).  After reading both zero counts the cursor
    //     reaches the End marker on the third decode.  (Note: a 0 count
    //     itself encodes as byte 1, NOT byte 0 — byte 0 is reserved for
    //     End — so the two count reads consume one byte each.)
    // =====================================================================
    {
        std::vector<std::uint8_t> stream{ 1, 1, 0, 0, 0, 0, 0, 0 };  // num_java=0, num_injected=0, End
        int pos{ 0 };
        const std::uint32_t j{ vmhook::hotspot::klass::decode_u5(stream.data(), pos) };
        const std::uint32_t k{ vmhook::hotspot::klass::decode_u5(stream.data(), pos) };
        const int pos_after_header{ pos };
        const std::uint32_t end{ vmhook::hotspot::klass::decode_u5(stream.data(), pos) };
        check("empty_stream_num_java_is_zero", j == 0u);
        check("empty_stream_num_injected_is_zero", k == 0u);
        check("empty_stream_header_consumes_two_bytes", pos_after_header == 2);
        check("empty_stream_then_end_marker", end == ~0u);
        check("empty_stream_end_parks_at_two", pos == 2);
    }

    return failures == 0 ? 0 : 1;
}
