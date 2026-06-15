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
#include <array>
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

// ---------------------------------------------------------------------------
// COMPILE-TIME spec mirror (namespace u5_ct).  vmhook's real decode_u5 is
// `inline static` but NOT constexpr, so it cannot be evaluated inside a
// static_assert.  To still get genuine compile-time coverage of the UNSIGNED5
// *contract* (the base-64 excess-1 math, the 5-byte cap, the low/high split,
// the End(0) rewind), we transcribe the SAME algorithm here as constexpr
// functions over a fixed-size buffer (std::array is constexpr in C++17; the
// oracle above uses std::vector, which is not).  The static_assert wall below
// pins the exact byte<->value mapping for every length-transition boundary and
// every landmark value at COMPILE time.  The identical landmarks are then
// re-checked against the REAL runtime decode_u5 in section (M), so the
// compile-time math and the shipped symbol are cross-validated, not merely
// asserted against each other.
// ---------------------------------------------------------------------------
namespace u5_ct
{
    constexpr std::uint32_t kExcess{ 1u };
    constexpr std::uint32_t kLow{ 191u };
    constexpr std::uint32_t kBase{ 64u };
    constexpr int           kShift{ 6 };
    constexpr int           kMaxLen{ 5 };
    constexpr std::uint32_t kContinuation{ kExcess + kLow };  // 192

    // A fixed 5-byte buffer plus the count of bytes actually written; this is
    // the constexpr-friendly stand-in for the oracle's std::vector encoding.
    struct Encoded
    {
        std::array<std::uint8_t, 5> bytes{};
        std::size_t                 len{ 0 };
    };

    // Canonical encoder (constexpr): minimal byte sequence HotSpot's writer
    // emits for `value`.  Never emits a 0 byte.  Mirrors u5_oracle::encode.
    constexpr auto encode(std::uint32_t value) -> Encoded
    {
        Encoded e{};
        if (value < kLow)
        {
            e.bytes[e.len++] = static_cast<std::uint8_t>(kExcess + value);
            return e;
        }
        std::uint32_t sum{ value };
        for (int i{ 0 }; ; ++i)
        {
            if (sum < kLow || i == kMaxLen - 1)
            {
                e.bytes[e.len++] = static_cast<std::uint8_t>(kExcess + sum);
                return e;
            }
            sum -= kLow;
            e.bytes[e.len++] = static_cast<std::uint8_t>(kExcess + kLow + (sum % kBase));
            sum >>= kShift;
        }
    }

    // Decoder (constexpr) modelling vmhook::decode_u5 EXACTLY, including the
    // End(0) rewind semantics: a 0 byte at any position returns ~0u and leaves
    // the cursor unchanged.  `data` must hold at least 5 readable bytes.
    constexpr auto decode(const std::uint8_t* data, int& pos) -> std::uint32_t
    {
        std::uint32_t sum{ 0 };
        for (int i{ 0 }; i < kMaxLen; ++i)
        {
            const std::uint8_t b{ data[pos++] };
            if (b == 0u)
            {
                --pos;
                return ~0u;
            }
            sum += static_cast<std::uint32_t>(b - 1u) << (kShift * i);
            if (b < kContinuation)
            {
                return sum;
            }
        }
        return sum;
    }

    // decode(encode(value)) over the fixed buffer; reports value + bytes used.
    struct Decoded { std::uint32_t value; int pos; };
    constexpr auto roundtrip(std::uint32_t value) -> Decoded
    {
        const Encoded e{ encode(value) };
        std::array<std::uint8_t, 8> buf{};   // 5 payload + 3 zero padding
        for (std::size_t i{ 0 }; i < e.len; ++i) { buf[i] = e.bytes[i]; }
        int pos{ 0 };
        const std::uint32_t v{ decode(buf.data(), pos) };
        return { v, pos };
    }

    // Decode a single low byte (constexpr) — the 1-byte-domain helper.
    constexpr auto decode_one(std::uint8_t b) -> Decoded
    {
        const std::array<std::uint8_t, 8> buf{ b, 0, 0, 0, 0, 0, 0, 0 };
        int pos{ 0 };
        const std::uint32_t v{ decode(buf.data(), pos) };
        return { v, pos };
    }
}  // namespace u5_ct

// ===========================================================================
// COMPILE-TIME ASSERTION WALL — every check here is evaluated by the compiler.
// If any fails the translation unit does not build (a far stronger guarantee
// than a runtime check).  These pin the codec contract; the runtime section
// (M) mirrors the same landmarks against the shipped decode_u5.
// ===========================================================================

// --- 1-byte domain: decode({b}) == b-1, consumes 1, for the endpoints and a
//     spread of interior points (full 1..191 sweep is done at runtime in (C)).
static_assert(u5_ct::decode_one(1).value == 0u   && u5_ct::decode_one(1).pos == 1, "1->0");
static_assert(u5_ct::decode_one(2).value == 1u   && u5_ct::decode_one(2).pos == 1, "2->1");
static_assert(u5_ct::decode_one(128).value == 127u, "128->127 (0x7F)");
static_assert(u5_ct::decode_one(129).value == 128u, "129->128 (0x80)");
static_assert(u5_ct::decode_one(191).value == 190u && u5_ct::decode_one(191).pos == 1, "191->190 (1-byte max)");

// --- End(0) marker: returns ~0u and does NOT advance the cursor (rewind).
static_assert(u5_ct::decode_one(0).value == ~0u && u5_ct::decode_one(0).pos == 0, "End(0) rewinds");

// --- Exact length-transition boundaries (the load-bearing base-64 carries).
//     max-at-len-N and min-at-len-(N+1) for every N in 1..5.
static_assert(u5_ct::encode(190u).len == 1 && u5_ct::encode(191u).len == 2, "len1->len2 @ 190/191");
static_assert(u5_ct::encode(12414u).len == 2 && u5_ct::encode(12415u).len == 3, "len2->len3 @ 12414/12415");
static_assert(u5_ct::encode(794750u).len == 3 && u5_ct::encode(794751u).len == 4, "len3->len4 @ 794750/794751");
static_assert(u5_ct::encode(50864254u).len == 4 && u5_ct::encode(50864255u).len == 5, "len4->len5 @ 50864254/50864255");

// --- Every boundary value round-trips through the constexpr codec with the
//     canonical byte length.
static_assert(u5_ct::roundtrip(190u).value == 190u        && u5_ct::roundtrip(190u).pos == 1, "rt 190");
static_assert(u5_ct::roundtrip(191u).value == 191u        && u5_ct::roundtrip(191u).pos == 2, "rt 191");
static_assert(u5_ct::roundtrip(12414u).value == 12414u    && u5_ct::roundtrip(12414u).pos == 2, "rt 12414");
static_assert(u5_ct::roundtrip(12415u).value == 12415u    && u5_ct::roundtrip(12415u).pos == 3, "rt 12415");
static_assert(u5_ct::roundtrip(794750u).value == 794750u  && u5_ct::roundtrip(794750u).pos == 3, "rt 794750");
static_assert(u5_ct::roundtrip(794751u).value == 794751u  && u5_ct::roundtrip(794751u).pos == 4, "rt 794751");
static_assert(u5_ct::roundtrip(50864254u).value == 50864254u && u5_ct::roundtrip(50864254u).pos == 4, "rt 50864254");
static_assert(u5_ct::roundtrip(50864255u).value == 50864255u && u5_ct::roundtrip(50864255u).pos == 5, "rt 50864255");

// --- Canonical encodings pinned BYTE-FOR-BYTE against the spec (these literals
//     are independently derived from value = sum (b_i-1)*64^i, not from the
//     encoder, so they cross-check the constexpr encoder itself).
static_assert(u5_ct::encode(0u).bytes[0] == 1u, "enc(0)={1}");
static_assert(u5_ct::encode(191u).bytes[0] == 192u && u5_ct::encode(191u).bytes[1] == 1u, "enc(191)={192,1}");
static_assert(u5_ct::encode(4096u).bytes[0] == 193u && u5_ct::encode(4096u).bytes[1] == 62u, "enc(4096)={193,62}");
static_assert(u5_ct::encode(65535u).bytes[0] == 192u && u5_ct::encode(65535u).bytes[1] == 254u
           && u5_ct::encode(65535u).bytes[2] == 13u && u5_ct::encode(65535u).len == 3, "enc(0xFFFF)={192,254,13}");
static_assert(u5_ct::encode(16777215u).bytes[0] == 192u && u5_ct::encode(16777215u).bytes[1] == 254u
           && u5_ct::encode(16777215u).bytes[2] == 253u && u5_ct::encode(16777215u).bytes[3] == 61u
           && u5_ct::encode(16777215u).len == 4, "enc(0xFFFFFF)={192,254,253,61}");
static_assert(u5_ct::encode(0xFFFFFFFFu).bytes[0] == 192u && u5_ct::encode(0xFFFFFFFFu).bytes[1] == 254u
           && u5_ct::encode(0xFFFFFFFFu).bytes[2] == 253u && u5_ct::encode(0xFFFFFFFFu).bytes[3] == 253u
           && u5_ct::encode(0xFFFFFFFFu).bytes[4] == 253u && u5_ct::encode(0xFFFFFFFFu).len == 5,
           "enc(UINT32_MAX)={192,254,253,253,253}");

// --- Landmark values round-trip losslessly through the constexpr codec, all
//     five byte-lengths represented.  (The runtime mirror in (M) re-verifies
//     these against the shipped decode_u5.)
static_assert(u5_ct::roundtrip(127u).value == 127u, "rt 0x7F");
static_assert(u5_ct::roundtrip(128u).value == 128u, "rt 0x80");
static_assert(u5_ct::roundtrip(255u).value == 255u, "rt 0xFF");
static_assert(u5_ct::roundtrip(65535u).value == 65535u && u5_ct::roundtrip(65535u).pos == 3, "rt 0xFFFF");
static_assert(u5_ct::roundtrip(65536u).value == 65536u, "rt 0x10000");
static_assert(u5_ct::roundtrip(16777215u).value == 16777215u && u5_ct::roundtrip(16777215u).pos == 4, "rt 0xFFFFFF");
static_assert(u5_ct::roundtrip(16777216u).value == 16777216u, "rt 0x1000000");
static_assert(u5_ct::roundtrip(2147483647u).value == 2147483647u && u5_ct::roundtrip(2147483647u).pos == 5, "rt 2^31-1");
static_assert(u5_ct::roundtrip(2147483648u).value == 2147483648u, "rt 2^31");
static_assert(u5_ct::roundtrip(2147483649u).value == 2147483649u, "rt 2^31+1");
static_assert(u5_ct::roundtrip(0xFFFFFFFFu).value == 0xFFFFFFFFu && u5_ct::roundtrip(0xFFFFFFFFu).pos == 5, "rt UINT32_MAX");

// --- Power-of-two ±1 ladder, EVERY exponent 0..31, all three of (2^k-1, 2^k,
//     2^k+1) round-tripping at compile time.  A single recursive constexpr fold
//     collapses the whole ladder into one assertion: any off-by-one carry bug
//     at any bit position breaks the build.
namespace u5_ct
{
    constexpr bool pow2_neighbourhood_roundtrips(int k)
    {
        if (k > 31) { return true; }
        const std::uint32_t p{ static_cast<std::uint32_t>(1u) << k };
        const std::uint32_t lo{ p == 0u ? 0u : p - 1u };
        const std::uint32_t hi{ p + 1u };  // wraps harmlessly at k==31 (-> 2^31+1)
        const bool here{
               roundtrip(lo).value == lo
            && roundtrip(p).value  == p
            && roundtrip(hi).value == hi };
        return here && pow2_neighbourhood_roundtrips(k + 1);
    }
}  // namespace u5_ct
static_assert(u5_ct::pow2_neighbourhood_roundtrips(0),
              "every power-of-two +/-1 (2^0..2^31) round-trips through UNSIGNED5");

// --- Dense compile-time fold over a contiguous low block: decode(encode(x))==x
//     for all x in [0..4500].  This crosses the len1->len2 and len2->len3
//     transitions hundreds of times entirely at compile time.  (Kept modest so
//     constexpr step limits are never an issue on any compiler in the matrix.)
namespace u5_ct
{
    constexpr bool dense_roundtrips(std::uint32_t x, std::uint32_t end)
    {
        for (; x <= end; ++x)
        {
            const Decoded d{ roundtrip(x) };
            if (d.value != x) { return false; }
            if (static_cast<std::size_t>(d.pos) != encode(x).len) { return false; }
        }
        return true;
    }
}  // namespace u5_ct
static_assert(u5_ct::dense_roundtrips(0u, 4500u),
              "decode(encode(x))==x with canonical length for all x in [0,4500]");

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

    // #####################################################################
    // ##  EXHAUSTIVE WAVE 3 — TARGETED LANDMARK VALUES against the REAL   ##
    // ##  decode_u5.  The strided uint32 sweep (E) walks ~84k coprime     ##
    // ##  points but lands on NONE of the classic landmark constants      ##
    // ##  (verified: 0x7F, 0x80, 0xFFFF, 0xFFFFFF, every power-of-two +-1,##
    // ##  2^31, 2^31+-1 are all missed by stride 51217).  The low dense    ##
    // ##  block (D) covers only x <= 49999.  This wave pins each landmark  ##
    // ##  EXPLICITLY: the shipped decode_u5 must reproduce the exact       ##
    // ##  spec value, the canonical byte length, and agree with the        ##
    // ##  independent oracle decoder, for every one.                       ##
    // #####################################################################

    // =====================================================================
    // (M) LANDMARK TABLE — 0x7F/0x80, 0xFF, 0xFFFF, 0xFFFFFF, the byte-width
    //     round constants, 2^31 +/- 1, and UINT32_MAX.  Each `expect_len` is
    //     the hand-derived canonical UNSIGNED5 length (see the boundary map in
    //     section (B)).  decode_u5 is fed the canonical encoding and must
    //     return (value, expect_len); the oracle decoder must agree byte-for-
    //     byte on the cursor too.
    // =====================================================================
    {
        struct Landmark { const char* name; std::uint32_t value; std::size_t expect_len; };
        const Landmark landmarks[]{
            { "0x7F_127",            127u,         1 },  // last value below the 0x80 transition
            { "0x80_128",            128u,         1 },  // 0x80 itself (still 1 byte: 128 < 191)
            { "0xFF_255",            255u,         2 },  // first 0xFF; spills to 2 bytes
            { "0x100_256",           256u,         2 },
            { "0x1FF_511",           511u,         2 },
            { "0x200_512",           512u,         2 },
            { "0xFFF_4095",          4095u,        2 },  // last 2-byte value before 0x1000 region
            { "0x1000_4096",         4096u,        2 },
            { "0xFFFF_65535",        65535u,       3 },  // 16-bit all-ones
            { "0x10000_65536",       65536u,       3 },
            { "0xFFFFF_1048575",     1048575u,     4 },
            { "0x100000_1048576",    1048576u,     4 },
            { "0xFFFFFF_16777215",   16777215u,    4 },  // 24-bit all-ones
            { "0x1000000_16777216",  16777216u,    4 },  // 2^24; still 4 bytes (5-byte region starts at 50864255)
            { "0x7FFFFFFF_2147483647", 2147483647u, 5 }, // 2^31 - 1
            { "0x80000000_2147483648", 2147483648u, 5 }, // 2^31 (bit 31 set)
            { "0x80000001_2147483649", 2147483649u, 5 }, // 2^31 + 1
            { "0xFFFFFFFF_max",      0xFFFFFFFFu,  5 },  // UINT32_MAX
        };
        bool value_ok{ true };
        bool length_ok{ true };
        bool oracle_ok{ true };
        const char* first_bad{ nullptr };
        for (const Landmark& lm : landmarks)
        {
            // real decode_u5 on the canonical encoding
            int vpos{ 0 };
            const std::uint32_t vdec{ roundtrip_decode(lm.value, vpos) };
            // independent oracle decode on the same padded buffer
            std::vector<std::uint8_t> padded{ u5_oracle::encode(lm.value) };
            padded.resize(padded.size() + 8, 0u);
            int opos{ 0 };
            const std::uint32_t odec{ u5_oracle::decode(padded.data(), opos) };

            if (vdec != lm.value)                                { value_ok = false;  if (!first_bad) first_bad = lm.name; }
            if (static_cast<std::size_t>(vpos) != lm.expect_len) { length_ok = false; if (!first_bad) first_bad = lm.name; }
            if (vdec != odec || vpos != opos)                    { oracle_ok = false; if (!first_bad) first_bad = lm.name; }
        }
        if (first_bad) { std::printf("       (first failing landmark = %s)\n", first_bad); }
        check("landmarks_decode_to_exact_value", value_ok);
        check("landmarks_consume_canonical_length", length_ok);
        check("landmarks_vmhook_matches_oracle_decoder", oracle_ok);
    }

    // =====================================================================
    // (N) HARDCODED-BYTE LANDMARK PINS — decode the canonical byte sequences
    //     for 0xFFFF and 0xFFFFFF written out as LITERAL bytes (independently
    //     derived from value = sum (b_i-1)*64^i), so these two checks do not
    //     depend on the in-test encoder at all.  If both the encoder and the
    //     decoder shared a bug, these byte-literal pins would still catch it.
    //       0xFFFF   = 65535    -> {192,254,13}     : 191 + 253*64 + 12*4096
    //       0xFFFFFF = 16777215 -> {192,254,253,61} : 191 + 253*64 + 252*4096 + 60*262144
    // =====================================================================
    {
        int pos{ 0 };
        const std::uint32_t v{ decode_at0({ 192, 254, 13 }, pos) };
        check("hardcoded_0xFFFF_bytes_decode_to_65535",
              v == 65535u && v == (191u + 253u * 64u + 12u * 64u * 64u));
        check("hardcoded_0xFFFF_consumes_three", pos == 3);
    }
    {
        int pos{ 0 };
        const std::uint32_t v{ decode_at0({ 192, 254, 253, 61 }, pos) };
        check("hardcoded_0xFFFFFF_bytes_decode_to_16777215",
              v == 16777215u
           && v == (191u + 253u * 64u + 252u * 64u * 64u + 60u * 64u * 64u * 64u));
        check("hardcoded_0xFFFFFF_consumes_four", pos == 4);
    }
    // 2^31 (bit 31 set) decoded from its literal canonical bytes {193,254,253,253,125}.
    //   = 192 + 253*64 + 252*4096 + 252*262144 + 124*16777216 = 2147483648
    {
        int pos{ 0 };
        const std::uint32_t v{ decode_at0({ 193, 254, 253, 253, 125 }, pos) };
        check("hardcoded_2pow31_bytes_decode_to_2147483648",
              v == 2147483648u && (v & 0x80000000u) != 0u);
        check("hardcoded_2pow31_consumes_five", pos == 5);
    }

    // =====================================================================
    // (O) POWER-OF-TWO +/-1 LADDER against the REAL decode_u5 — for every
    //     exponent k in 0..31, decode_u5 on the canonical encodings of
    //     (2^k - 1), 2^k, and (2^k + 1) must round-trip exactly and consume
    //     the canonical length.  The compile-time wall already proved the
    //     spec math; this proves the SHIPPED symbol agrees across all 32 bit
    //     positions (96 values), where any per-bit carry mishandling would
    //     surface.
    // =====================================================================
    {
        bool value_ok{ true };
        bool length_ok{ true };
        int  first_bad_k{ -1 };
        for (int k{ 0 }; k <= 31; ++k)
        {
            const std::uint32_t p{ static_cast<std::uint32_t>(1u) << k };
            const std::uint32_t probes[]{ (p == 0u ? 0u : p - 1u), p, p + 1u };
            for (const std::uint32_t x : probes)
            {
                int vpos{ 0 };
                const std::uint32_t vdec{ roundtrip_decode(x, vpos) };
                if (vdec != x)                                                  { value_ok = false;  if (first_bad_k < 0) first_bad_k = k; }
                if (static_cast<std::size_t>(vpos) != u5_oracle::encoded_length(x)) { length_ok = false; if (first_bad_k < 0) first_bad_k = k; }
            }
        }
        if (first_bad_k >= 0) { std::printf("       (first failing power-of-two exponent k = %d)\n", first_bad_k); }
        check("pow2_pm1_ladder_real_decode_roundtrips", value_ok);
        check("pow2_pm1_ladder_real_decode_canonical_length", length_ok);
    }

    // =====================================================================
    // (P) HIGH-RANGE DENSE BLOCK around UINT32_MAX — the strided sweep is
    //     sparse near the very top; pin a contiguous run of the largest 4096
    //     uint32 values [2^32-4096 .. 2^32-1] so the top of the encoding
    //     space (where the 5th byte carries its maximum digit) has zero gaps.
    // =====================================================================
    {
        bool ok{ true };
        std::uint32_t first_bad{ 0 };
        bool have_bad{ false };
        for (std::uint32_t x{ 0xFFFFF000u }; ; ++x)
        {
            int vpos{ 0 };
            const std::uint32_t vdec{ roundtrip_decode(x, vpos) };
            if (vdec != x || static_cast<std::size_t>(vpos) != 5u)
            {
                ok = false;
                if (!have_bad) { first_bad = x; have_bad = true; }
            }
            if (x == 0xFFFFFFFFu) { break; }  // inclusive of UINT32_MAX, then stop (no wrap)
        }
        if (have_bad) { std::printf("       (first failing top-range value = %u)\n", first_bad); }
        check("top_4096_uint32_values_roundtrip_in_five_bytes", ok);
    }

    // #####################################################################
    // ##  EXHAUSTIVE WAVE 4 — remaining per-position digit sweeps, the    ##
    // ##  full leading-byte domain, and explicit pins of the documented   ##
    // ##  32-bit-truncation (flaw #3) and End-sentinel-aliasing (flaw #4)  ##
    // ##  boundaries.  Wave 2's section (G) swept the position-1 digit      ##
    // ##  fully (1..191) but left positions 2/3/4 only spot-checked, and    ##
    // ##  the position-0 LEADING byte was sampled (192,193,255) rather than ##
    // ##  swept across its whole 192..255 continuation range.  Every        ##
    // ##  expected value below is hand-derived from value = sum (b_i-1)*    ##
    // ##  64^i and uses the SAME uint32 shift arithmetic the decoder uses,  ##
    // ##  so the wrapping cases pin the decoder's actual result, not an     ##
    // ##  idealised big-integer one.  All assertions are deterministic and  ##
    // ##  platform-independent (uint32 wraparound is well-defined in C++).  ##
    // #####################################################################

    // =====================================================================
    // (Q) COMPLETE LEADING-BYTE (position-0) DOMAIN — every byte 1..255 as
    //     the FIRST byte, with a guaranteed terminator behind it.
    //       * A low first byte 1..191 terminates immediately: value == b0-1,
    //         consumes 1 (this re-pins the 1-byte domain from the leading
    //         position with a NON-zero follow byte present, proving the
    //         follow byte is never read once a low byte terminates).
    //       * A continuation first byte 192..255 followed by the smallest low
    //         byte (1, digit 0) yields value == b0-1 in two bytes, isolating
    //         the position-0 weight (64^0 == 1) across the whole high range.
    //     Closes bytes 192..255 at position 0, which (C) explicitly deferred.
    // =====================================================================
    {
        bool low_ok{ true };
        bool high_ok{ true };
        int  first_bad{ -1 };
        for (int b0{ 1 }; b0 <= 255; ++b0)
        {
            if (b0 < 192)
            {
                // Low leading byte: terminates at position 0. Put a non-zero
                // continuation byte (200) behind it that must NOT be consumed.
                int pos{ 0 };
                const std::uint32_t v{ decode_at0({ static_cast<std::uint8_t>(b0), 200u }, pos) };
                if (v != static_cast<std::uint32_t>(b0 - 1) || pos != 1)
                {
                    low_ok = false; if (first_bad < 0) first_bad = b0;
                }
            }
            else
            {
                // Continuation leading byte + minimal low terminator (1 -> digit 0):
                // value == (b0 - 1) at weight 64^0, consumes exactly 2 bytes.
                int pos{ 0 };
                const std::uint32_t v{ decode_at0({ static_cast<std::uint8_t>(b0), 1u }, pos) };
                if (v != static_cast<std::uint32_t>(b0 - 1) || pos != 2)
                {
                    high_ok = false; if (first_bad < 0) first_bad = b0;
                }
            }
        }
        if (!low_ok || !high_ok) { std::printf("       (first failing leading byte = %d)\n", first_bad); }
        check("leading_low_byte_1_to_191_terminates_and_ignores_follow", low_ok);
        check("leading_continuation_byte_192_to_255_plus_min_low_is_b0_minus_1", high_ok);
    }

    // =====================================================================
    // (R) FULL TERMINAL-DIGIT SWEEPS AT POSITIONS 2, 3 AND 4 — the partner
    //     of (G)'s position-1 sweep.  Hold every lower position at the
    //     minimal continuation byte 192 (digit 191) and sweep the
    //     terminating low byte 1..191 at the position under test.  The
    //     expected value is the all-191 prefix plus (digit) << (6*pos), built
    //     with the decoder's own uint32 shift arithmetic so the position-4
    //     high digits (which overflow 32 bits and wrap) are pinned exactly.
    //     Each decode consumes pos+1 bytes.
    // =====================================================================
    {
        auto prefix_sum = [](int k) -> std::uint32_t {
            std::uint32_t s{ 0 };
            for (int i{ 0 }; i < k; ++i) { s += static_cast<std::uint32_t>(191u) << (6 * i); }
            return s;
        };
        for (int p{ 2 }; p <= 4; ++p)
        {
            bool value_ok{ true };
            bool pos_ok{ true };
            int  first_bad{ -1 };
            const std::uint32_t base{ prefix_sum(p) };
            for (int digit_byte{ 1 }; digit_byte <= 191; ++digit_byte)
            {
                std::vector<std::uint8_t> bytes(static_cast<std::size_t>(p), 192u);  // p leading continuations
                bytes.push_back(static_cast<std::uint8_t>(digit_byte));               // terminating low byte
                int pos{ 0 };
                const std::uint32_t v{ decode_at0(bytes, pos) };
                const std::uint32_t expected{
                    base + (static_cast<std::uint32_t>(digit_byte - 1) << (6 * p)) };
                if (v != expected) { value_ok = false; if (first_bad < 0) first_bad = digit_byte; }
                if (pos != p + 1)  { pos_ok = false;   if (first_bad < 0) first_bad = digit_byte; }
            }
            if (!value_ok || !pos_ok)
            {
                std::printf("       (first failing position-%d terminal digit = %d)\n", p, first_bad);
            }
            std::string lbl_v{ std::string{ "position_" } + std::to_string(p) + "_terminal_digit_sweep_1_to_191_value" };
            std::string lbl_p{ std::string{ "position_" } + std::to_string(p) + "_terminal_digit_sweep_1_to_191_consumes" };
            check(lbl_v.c_str(), value_ok);
            check(lbl_p.c_str(), pos_ok);
        }
    }

    // =====================================================================
    // (S) THE 191/192 LOW-vs-CONTINUATION SPLIT AT A NON-LEADING POSITION —
    //     the `current_byte < 192` test fires at EVERY byte position, not
    //     only position 0.  Place the boundary bytes at position 1 of a value
    //     whose position-0 byte is a fixed continuation (192, digit 191):
    //       * b1 == 191 (LOW): terminates a 2-byte value == 191 + 190*64
    //         == 12351, consuming 2 — the position-1 byte does NOT continue.
    //       * b1 == 192 (CONTINUATION): does NOT terminate; a third low byte
    //         (1, digit 0) is then read, giving 191 + 191*64 == 12415 in 3
    //         bytes.  The single-step change 191->192 at position 1 flips the
    //         length from 2 to 3, isolating the split at a middle byte.
    //     Also pin the same split at position 2 (two leading 192s).
    // =====================================================================
    {
        // position-1 boundary
        int pos_low{ 0 };
        const std::uint32_t v_low{ decode_at0({ 192u, 191u, 200u }, pos_low) };  // 200 must be ignored
        check("split_at_pos1_byte_191_is_low_terminator",
              v_low == (191u + 190u * 64u) && v_low == 12351u && pos_low == 2);
        int pos_high{ 0 };
        const std::uint32_t v_high{ decode_at0({ 192u, 192u, 1u }, pos_high) };
        check("split_at_pos1_byte_192_continues_to_third_byte",
              v_high == (191u + 191u * 64u) && v_high == 12415u && pos_high == 3);
        check("split_at_pos1_191_vs_192_flips_length_2_to_3",
              pos_low == 2 && pos_high == 3);

        // position-2 boundary (two leading continuations, then 191 vs 192 at pos 2)
        int pos2_low{ 0 };
        const std::uint32_t v2_low{ decode_at0({ 192u, 192u, 191u, 200u }, pos2_low) };
        check("split_at_pos2_byte_191_is_low_terminator",
              v2_low == (191u + 191u * 64u + 190u * 64u * 64u) && pos2_low == 3);
        int pos2_high{ 0 };
        const std::uint32_t v2_high{ decode_at0({ 192u, 192u, 192u, 1u }, pos2_high) };
        check("split_at_pos2_byte_192_continues_to_fourth_byte",
              v2_high == (191u + 191u * 64u + 191u * 64u * 64u) && pos2_high == 4);
    }

    // =====================================================================
    // (T) POSITION-4 HIGH-DIGIT TRUNCATION (feature flaw #3, documented &
    //     intended).  At byte_position == 4 the contribution is
    //     (digit) << 24, so a position-4 digit >= 0x40 (== 64) places bits
    //     above 31 that fall off uint32 — the decoder returns a deterministic
    //     WRAPPED value, never UB (unsigned shift/overflow is well-defined).
    //     We pin a value that SETS bit 31 and one whose top bits wrap, both
    //     built with the decoder's exact uint32 shift math.  Prefix is four
    //     192s (digit 191 at positions 0..3); the 5th byte is the position-4
    //     terminal digit.
    // =====================================================================
    {
        // 5th byte 192 -> digit 191 at position 4: 191<<24 = 0xBF000000 (bit 31 set).
        int pos{ 0 };
        const std::uint32_t v{ decode_at0({ 192u, 192u, 192u, 192u, 192u }, pos) };
        const std::uint32_t expected{
            (static_cast<std::uint32_t>(191u) << 0)
          + (static_cast<std::uint32_t>(191u) << 6)
          + (static_cast<std::uint32_t>(191u) << 12)
          + (static_cast<std::uint32_t>(191u) << 18)
          + (static_cast<std::uint32_t>(191u) << 24) };
        check("pos4_digit_191_matches_uint32_shift_sum", v == expected && pos == 5);
        check("pos4_digit_191_sets_bit_31", (v & 0x80000000u) != 0u);

        // Sweep EVERY position-4 terminal digit 1..191 and confirm the decoder
        // result equals the wrapping uint32 shift sum for each — i.e. the
        // documented truncation is uniform and deterministic across the whole
        // 5th-digit range, not just at the endpoints.
        bool sweep_ok{ true };
        int  first_bad{ -1 };
        const std::uint32_t base4{
            (static_cast<std::uint32_t>(191u) << 0)
          + (static_cast<std::uint32_t>(191u) << 6)
          + (static_cast<std::uint32_t>(191u) << 12)
          + (static_cast<std::uint32_t>(191u) << 18) };
        for (int d{ 1 }; d <= 191; ++d)
        {
            int sp{ 0 };
            const std::uint32_t sv{ decode_at0({ 192u, 192u, 192u, 192u, static_cast<std::uint8_t>(d) }, sp) };
            const std::uint32_t se{ base4 + (static_cast<std::uint32_t>(d - 1) << 24) };
            if (sv != se || sp != 5) { sweep_ok = false; if (first_bad < 0) first_bad = d; }
        }
        if (!sweep_ok) { std::printf("       (first failing pos4 terminal digit = %d)\n", first_bad); }
        check("pos4_terminal_digit_sweep_1_to_191_is_wrapping_shift_sum", sweep_ok);
    }

    // =====================================================================
    // (U) END-SENTINEL ANTI-ALIASING (feature flaw #4).  ~0u is returned for
    //     BOTH the End(0) marker AND a genuine UINT32_MAX decode; the two are
    //     distinguished only by the cursor delta.  Beyond UINT32_MAX itself
    //     (pinned in section I), confirm that the LARGEST low-terminated
    //     5-byte value reachable by the encoder (3255312510 == {255,255,255,
    //     255,191}) is NOT ~0u — a non-degenerate real value never collides
    //     with the End return value via the normal (low-terminated) path.
    //     Then prove the cursor is the sole discriminator: End advances 0,
    //     a real UINT32_MAX decode advances 5, on otherwise-identical calls.
    // =====================================================================
    {
        int p_lowmax{ 0 };
        const std::uint32_t lowmax{ decode_at0({ 255u, 255u, 255u, 255u, 191u }, p_lowmax) };
        check("low_terminated_5byte_max_is_3255312510", lowmax == 3255312510u && p_lowmax == 5);
        check("low_terminated_5byte_max_is_not_end_sentinel", lowmax != ~0u);

        // Cursor is the discriminator: feed UINT32_MAX bytes vs a single 0.
        int p_max{ 0 };
        const std::uint32_t vmax{ decode_at0({ 192u, 254u, 253u, 253u, 253u }, p_max) };
        int p_end{ 0 };
        const std::uint32_t vend{ decode_at0({ 0u }, p_end) };
        check("uint32_max_and_end_share_return_value", vmax == ~0u && vend == ~0u);
        check("uint32_max_and_end_differ_only_by_cursor", p_max == 5 && p_end == 0);
    }

    // =====================================================================
    // (V) DECODE RESULT IS BITWISE-IDENTICAL REGARDLESS OF START OFFSET — a
    //     stronger form of (H): for a spread of values, decoding the SAME
    //     canonical bytes placed at offsets 0..16 yields the EXACT same value
    //     and the cursor always advances by exactly the encoded length.  This
    //     pins that decode_u5 is a pure function of the bytes at [stream_pos,
    //     stream_pos+len) and the start offset only shifts the window.
    // =====================================================================
    {
        const std::uint32_t probes[]{ 0u, 1u, 63u, 64u, 190u, 191u, 255u, 4096u,
                                      65535u, 16777215u, 50864255u, 3255312510u, 0xFFFFFFFFu };
        bool ok{ true };
        std::uint32_t first_bad{ 0 };
        bool have_bad{ false };
        for (const std::uint32_t val : probes)
        {
            const std::vector<std::uint8_t> enc{ u5_oracle::encode(val) };
            std::uint32_t ref{ 0 };
            for (int start{ 0 }; start <= 16; ++start)
            {
                std::vector<std::uint8_t> buf(static_cast<std::size_t>(start), 1u);  // filler (each is value 0)
                buf.insert(buf.end(), enc.begin(), enc.end());
                buf.resize(buf.size() + 8, 0u);  // peek padding
                int pos{ start };
                const std::uint32_t v{ vmhook::hotspot::klass::decode_u5(buf.data(), pos) };
                if (start == 0) { ref = v; }
                if (v != val || v != ref
                 || static_cast<std::size_t>(pos) != static_cast<std::size_t>(start) + enc.size())
                {
                    ok = false; if (!have_bad) { first_bad = val; have_bad = true; }
                }
            }
        }
        if (have_bad) { std::printf("       (first failing offset-invariance value = %u)\n", first_bad); }
        check("decode_value_is_bitwise_identical_across_start_offsets_0_to_16", ok);
    }

    // =====================================================================
    // (W) SEQUENTIAL THREADING OF MAXIMAL-LENGTH VALUES — back-to-back decode
    //     of three 5-byte values from one buffer, confirming the cursor
    //     threads correctly across the longest records (the costliest case
    //     for the per-field walk in find_field_in_stream) and lands exactly
    //     on the trailing End(0).
    // =====================================================================
    {
        const std::uint32_t a{ 50864255u };   // smallest 5-byte value
        const std::uint32_t b{ 0xFFFFFFFFu };  // UINT32_MAX (5 bytes)
        const std::uint32_t c{ 3255312510u };  // largest low-terminated 5-byte value
        std::vector<std::uint8_t> stream;
        for (std::uint32_t v : { a, b, c })
        {
            const std::vector<std::uint8_t> e{ u5_oracle::encode(v) };
            stream.insert(stream.end(), e.begin(), e.end());
        }
        const std::size_t end_index{ stream.size() };
        stream.push_back(0u);                  // End marker
        stream.resize(stream.size() + 8, 0u);  // peek padding

        int pos{ 0 };
        const std::uint32_t d0{ vmhook::hotspot::klass::decode_u5(stream.data(), pos) };
        const std::uint32_t d1{ vmhook::hotspot::klass::decode_u5(stream.data(), pos) };
        const std::uint32_t d2{ vmhook::hotspot::klass::decode_u5(stream.data(), pos) };
        const int pos_before_end{ pos };
        const std::uint32_t end{ vmhook::hotspot::klass::decode_u5(stream.data(), pos) };
        check("seq_three_5byte_values_decode_correctly", d0 == a && d1 == b && d2 == c);
        check("seq_three_5byte_values_consume_fifteen_bytes", pos_before_end == 15);
        check("seq_three_5byte_values_cursor_lands_on_end_index",
              static_cast<std::size_t>(pos_before_end) == end_index);
        check("seq_three_5byte_values_then_end_marker", end == ~0u && pos == pos_before_end);
    }

    return failures == 0 ? 0 : 1;
}
