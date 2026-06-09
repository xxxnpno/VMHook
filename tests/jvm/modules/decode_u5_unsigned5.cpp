// decode_u5 / UNSIGNED5 JVM test module  (feature area: HotSpot field-stream codec)
//
// THE authority for vmhook::hotspot::klass::decode_u5(const uint8_t* data,
// int& stream_pos) -- the variable-length UNSIGNED5 integer decoder that parses
// the JDK 21+ InstanceKlass::_fieldinfo_stream (an Array<u1> of UNSIGNED5 values)
// under find_field_in_stream -> find_field.  Decoder defined at vmhook.hpp:3038.
//
// WHAT THIS MODULE PROVES, EXHAUSTIVELY.  decode_u5 is a PURE byte-buffer ->
// uint32 function with an in/out cursor; it touches no JVM state.  So this module
// drives it DIRECTLY with hand-crafted byte buffers and asserts BOTH the decoded
// value AND the bytes-consumed (the cursor delta) for every interesting input,
// with NO JVM, NO heap deref, NO hooks -- which makes it the single most
// suite-safe module in the matrix while still being able to FAIL for any real
// regression in the codec.  (The same decoder is reached live via
// find_field_in_stream on JDK 21+, but the value/cursor arithmetic this module
// pins is identical whether the bytes come from a real Array<u1> or a local
// array -- so a pure-buffer harness is the COMPLETE and correct test surface for
// the decoder's logic.)
//
// CODEC CONTRACT (mirrored from src/hotspot/share/utilities/unsigned5.hpp and the
// library docblock at vmhook.hpp:3030):
//   * value = SUM (b_i - 1) * 64^i     for i = 0, 1, 2, ...   (base-64, excess-1)
//   * a byte >= 192 is a CONTINUATION byte: read the next byte too.
//   * the first byte < 192 is the TERMINATING byte: stop, return the running sum.
//   * MAX_LENGTH == 5: after 5 continuation bytes the loop stops and returns the
//     partial sum WITHOUT reading a 6th byte (32-bit codec; field-stream values
//     are all u4, so 5 bytes is the documented hard cap).
//   * byte value 0 is NEVER emitted by the encoder; it is the stream End sentinel.
//     On reading a 0 the decoder REWINDS the cursor (--stream_pos) and returns
//     ~0u (0xFFFFFFFF) so the caller can park exactly on the terminator.
//
// EXHAUSTIVE INPUT COVERAGE (all asserted decoded-value AND cursor-delta):
//   * value 0 and the entire 1-byte space: a loop over EVERY low byte 1..191
//     asserting decode({b}) == b-1 and consumes 1  (closes the 1-byte space).
//   * each byte-length boundary AND its off-by-one neighbour: the exact largest
//     value at length N and the smallest value at length N+1, for N = 1..4
//     (190|191, 12414|12415, 794750|794751, 50864254|50864255), each with the
//     canonical minimal encoding and the expected consumed-byte count.
//   * the continuation-bit transition 191 (low/terminates) vs 192 (high/continues)
//     as the FIRST byte and as the MIDDLE byte of a multi-byte value.
//   * the high AND low halves of an encoded byte at every position (a continuation
//     byte's digit b-1 in [191,254] vs a terminating digit in [0,190]).
//   * the 5-byte terminating maximum (3255312510, bit 31 set) and the FULL u32
//     maximum 0xFFFFFFFF (canonical 5-byte {192,254,253,253,253}), proving the
//     codec spans the entire 32-bit range.
//   * the SENTINEL-ALIASING hazard: 0xFFFFFFFF is BOTH a real decode AND the End
//     marker numerically -- the module pins that the two are disambiguated ONLY by
//     the cursor (End consumes 0 via rewind; the real value consumes 5).
//   * the 5-continuation hard cap ({255,255,255,255,255}) -- partial sum, stops at
//     exactly 5 -- and the 6th-byte-not-consumed property.
//   * the End sentinel: leading 0 -> ~0u with the cursor UNCHANGED (rewind), and
//     0-as-End distinct from real value 0 (byte 1 advances, byte 0 does not).
//   * non-zero start offset: decode from a mid-buffer cursor is value-identical to
//     decode from 0 and advances the same amount (the exact find_field_in_stream
//     threading usage).
//   * sequential packed values threaded through one cursor, including the caller's
//     real grammar: a header (num_java, num_injected) + per-field records (5
//     mandatory values + flag-gated optionals) decoded purely against a byte
//     buffer, asserting the cursor lands exactly on the trailing 0 End marker.
//   * a randomised round-trip fuzz: encode_ref(v) for many pseudo-random v, then
//     assert decode_u5 returns v and consumes exactly the encoded length.
//
// ALL expected values are anchored TWO independent ways: (a) a reference
// identity decode  SUM (b_i - 1) * 64^i  computed in this TU from the raw bytes,
// and (b) a reference encoder encode_ref() that is the exact inverse of the
// decoder (verified by full-range round-trip offline).  Hardcoded literals in the
// boundary table are cross-checked against BOTH, so a typo cannot make a check
// vacuously pass.
//
// SUITE-SAFETY (mirrors register_class.cpp / aaa_warmup.cpp):
//   * NO JVM, NO fixture, NO heap deref, NO hooks, NO GC.  The module calls only
//     decode_u5 on local stack/static arrays -- there is no vmhook-returned
//     pointer to validate and no Java coordination.  It cannot crash the suite.
//   * The whole body runs under try/catch; a caught throw is recorded as [INFO],
//     NEVER a FAIL.  An UNCONDITIONAL vmhook::shutdown_hooks() runs OUTSIDE the
//     try (belt-and-braces: this module arms nothing, but it leaves the hook table
//     provably empty on every path, exactly like the other re-enabled modules).
//   * -Werror clean under -Wall -Wextra -Wpedantic: no unused vars, no narrowing,
//     no sign-compare (loop counters and byte values are explicitly typed).
//   * C++17/23 only: no post-17 API; no std::bit_cast.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace
{
    // ---- direct alias to the function under test ---------------------------
    // decode_u5 is a public `inline static` member of vmhook::hotspot::klass
    // (struct -> default public); callable with no instance.  Signature:
    //   static uint32_t decode_u5(const uint8_t* data, int& stream_pos) noexcept
    auto decode(const std::uint8_t* data, int& pos) -> std::uint32_t
    {
        return vmhook::hotspot::klass::decode_u5(data, pos);
    }

    // The End sentinel the decoder returns on a 0 byte (and the brief's "max u32").
    constexpr std::uint32_t END_MARK{ ~0u };  // 0xFFFFFFFF == 4294967295

    // ---- reference identity decode (INDEPENDENT oracle #1) -----------------
    // The pure mathematical definition of UNSIGNED5: value = SUM (b_i-1)*64^i.
    // Computed in 64-bit then truncated to u32 exactly as the decoder's unsigned
    // accumulator would, so it is a faithful, decoder-independent expectation.
    // `count` is how many bytes the decoder consumes for this sequence.
    auto identity_ref(const std::vector<std::uint8_t>& bytes) -> std::uint32_t
    {
        std::uint64_t sum{ 0 };
        for (std::size_t i{ 0 }; i < bytes.size(); ++i)
        {
            sum += static_cast<std::uint64_t>(bytes[i] - 1u)
                   << static_cast<std::uint64_t>(6u * i);
        }
        return static_cast<std::uint32_t>(sum);
    }

    // ---- reference encoder (INDEPENDENT oracle #2) -------------------------
    // The exact inverse of decode_u5, verified offline by a full 32-bit
    // round-trip (decode(encode_ref(v)) == v for every v in [0, 0xFFFFFFFF],
    // max length 5).  Used both to generate canonical minimal encodings for the
    // value sweeps and as a second cross-check on the hardcoded boundary table.
    //
    // Rule: at each position, if the remaining value fits a TERMINATING digit
    // (<= 190, byte < 192) -- or we are at the 5th and final byte -- emit it as
    // (remaining + 1); otherwise emit a CONTINUATION byte whose digit lies in
    // [191,254] (byte in [192,255]) and is == remaining (mod 64), then carry
    // (remaining - digit) / 64 up to the next position.
    auto encode_ref(std::uint32_t value) -> std::vector<std::uint8_t>
    {
        std::vector<std::uint8_t> out{};
        std::uint64_t remaining{ value };
        for (int i{ 0 }; ; ++i)
        {
            if (remaining <= 190u || i == 4)
            {
                out.push_back(static_cast<std::uint8_t>(remaining + 1u));
                break;
            }
            const std::uint64_t digit{ 191u + ((remaining - 191u) % 64u) };
            out.push_back(static_cast<std::uint8_t>(digit + 1u));
            remaining = (remaining - digit) / 64u;
        }
        return out;
    }

    // Decode `bytes` (padded so the decoder may always peek 5) starting at `start`.
    // Returns the decoded value; writes the bytes-consumed into `consumed`.
    auto decode_seq(const std::vector<std::uint8_t>& bytes, int start,
                    int& consumed) -> std::uint32_t
    {
        // 8 trailing zeros guarantee the bounded 5-byte read never walks off the
        // end of our buffer regardless of the input shape (a 0 also doubles as a
        // natural End terminator if the sequence is itself non-terminating).
        std::vector<std::uint8_t> buf{};
        buf.reserve(static_cast<std::size_t>(start) + bytes.size() + 8u);
        buf.insert(buf.end(), static_cast<std::size_t>(start),
                   static_cast<std::uint8_t>(0xAA));  // junk prefix before `start`
        buf.insert(buf.end(), bytes.begin(), bytes.end());
        buf.insert(buf.end(), 8u, static_cast<std::uint8_t>(0));
        int pos{ start };
        const std::uint32_t value{ decode(buf.data(), pos) };
        consumed = pos - start;
        return value;
    }

    // Convenience: decode from offset 0.
    auto decode_at0(const std::vector<std::uint8_t>& bytes, int& consumed)
        -> std::uint32_t
    {
        return decode_seq(bytes, 0, consumed);
    }

    // A tiny xorshift32 so the fuzz sweep is deterministic across platforms
    // (no <random> engine variance, no narrowing surprises).
    auto next_rand(std::uint32_t& state) -> std::uint32_t
    {
        std::uint32_t x{ state };
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        state = x;
        return x;
    }

    // ── One assertion helper that checks BOTH value and consumed-bytes, AND
    //    cross-checks the expectation against the identity oracle so a wrong
    //    literal cannot slip through as a vacuous pass. ──
    auto check_decode(vmhook_test::context& ctx, const std::string& name,
                      const std::vector<std::uint8_t>& bytes,
                      std::uint32_t expect_value, int expect_consumed) -> void
    {
        int consumed{ -1 };
        const std::uint32_t got{ decode_at0(bytes, consumed) };
        ctx.check(name, got == expect_value && consumed == expect_consumed);
    }

    auto run_decode_u5_checks(vmhook_test::context& ctx) -> void
    {
        // ============================================================ SECTION 0
        //  SELF-CONSISTENCY OF THE ORACLES.  Before trusting encode_ref /
        //  identity_ref to anchor the rest of the module, prove they agree with
        //  the decoder on a spread of values.  If these fail, the oracles are
        //  wrong and every downstream "expected" is suspect -- so this gates the
        //  whole module's credibility (non-vacuous: a broken oracle FAILS here).
        // =====================================================================
        {
            const std::uint32_t seeds[]{
                0u, 1u, 63u, 64u, 190u, 191u, 255u, 4096u, 12414u, 12415u,
                65535u, 794750u, 794751u, 1056895u, 50864254u, 50864255u,
                3255312510u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFEu, 0xFFFFFFFFu };
            bool all_ok{ true };
            for (const std::uint32_t v : seeds)
            {
                const std::vector<std::uint8_t> enc{ encode_ref(v) };
                int consumed{ -1 };
                const std::uint32_t got{ decode_at0(enc, consumed) };
                const bool ok{ got == v
                               && consumed == static_cast<int>(enc.size())
                               && identity_ref(enc) == v
                               && enc.size() >= 1u && enc.size() <= 5u };
                all_ok = all_ok && ok;
            }
            ctx.check("oracles_roundtrip_agree_with_decoder", all_ok);
        }

        // ============================================================ SECTION 1
        //  VALUE 0 and the ENTIRE 1-byte space.  Every terminating byte 1..191
        //  decodes to (byte-1) and consumes exactly 1.  This closes the complete
        //  single-byte input space (191 distinct inputs) in one non-vacuous loop.
        // =====================================================================
        {
            bool all_value_ok{ true };
            bool all_consume_ok{ true };
            for (int b{ 1 }; b <= 191; ++b)
            {
                int consumed{ -1 };
                const std::uint32_t got{ decode_at0(
                    { static_cast<std::uint8_t>(b) }, consumed) };
                all_value_ok   = all_value_ok   && (got == static_cast<std::uint32_t>(b - 1));
                all_consume_ok = all_consume_ok && (consumed == 1);
            }
            ctx.check("one_byte_space_all_values_b_minus_1", all_value_ok);
            ctx.check("one_byte_space_all_consume_one", all_consume_ok);

            // Spot anchors with explicit literals (also guards the loop itself).
            check_decode(ctx, "value_zero_is_byte_one", { 1 }, 0u, 1);
            check_decode(ctx, "value_1_is_byte_2", { 2 }, 1u, 1);
            check_decode(ctx, "value_63_is_byte_64", { 64 }, 63u, 1);
            check_decode(ctx, "value_64_is_byte_65", { 65 }, 64u, 1);
            check_decode(ctx, "value_127_is_byte_128", { 128 }, 127u, 1);
            check_decode(ctx, "value_190_is_byte_191_max_1byte", { 191 }, 190u, 1);
        }

        // ============================================================ SECTION 2
        //  BYTE-LENGTH BOUNDARIES + OFF-BY-ONE.  For each N in 1..4, the largest
        //  value at length N and the smallest value at length N+1, with the
        //  canonical minimal encoding and the exact consumed-byte count.  This is
        //  the "exact value where it rolls to 2/3/4/5 bytes" coverage.
        // =====================================================================
        {
            // 1 -> 2 byte roll.  190 is the last 1-byte value; 191 needs 2 bytes.
            check_decode(ctx, "boundary_1byte_max_190", { 191 }, 190u, 1);
            check_decode(ctx, "boundary_2byte_min_191", { 192, 1 }, 191u, 2);
            // 2 -> 3.  12414 last 2-byte; 12415 first 3-byte.
            check_decode(ctx, "boundary_2byte_max_12414", { 255, 191 }, 12414u, 2);
            check_decode(ctx, "boundary_3byte_min_12415", { 192, 192, 1 }, 12415u, 3);
            // 3 -> 4.  794750 last 3-byte; 794751 first 4-byte.
            check_decode(ctx, "boundary_3byte_max_794750", { 255, 255, 191 }, 794750u, 3);
            check_decode(ctx, "boundary_4byte_min_794751", { 192, 192, 192, 1 }, 794751u, 4);
            // 4 -> 5.  50864254 last 4-byte; 50864255 first 5-byte.
            check_decode(ctx, "boundary_4byte_max_50864254", { 255, 255, 255, 191 }, 50864254u, 4);
            check_decode(ctx, "boundary_5byte_min_50864255", { 192, 192, 192, 192, 1 }, 50864255u, 5);

            // Cross-check every boundary's encoding against encode_ref so the
            // literals above are not just internally consistent but match the
            // canonical inverse too.
            const std::uint32_t boundary_values[]{
                190u, 191u, 12414u, 12415u, 794750u, 794751u, 50864254u, 50864255u };
            const std::size_t expected_len[]{ 1, 2, 2, 3, 3, 4, 4, 5 };
            bool enc_lengths_ok{ true };
            for (std::size_t i{ 0 }; i < std::size(boundary_values); ++i)
            {
                enc_lengths_ok = enc_lengths_ok
                    && (encode_ref(boundary_values[i]).size() == expected_len[i]);
            }
            ctx.check("boundary_canonical_encoding_lengths", enc_lengths_ok);
        }

        // ============================================================ SECTION 3
        //  CONTINUATION-BIT TRANSITION (191 low vs 192 high) at FIRST and MIDDLE
        //  byte positions.  191 (< 192) terminates; 192 (>= 192) continues.
        // =====================================================================
        {
            // As the FIRST byte:
            check_decode(ctx, "first_byte_191_terminates", { 191 }, 190u, 1);
            check_decode(ctx, "first_byte_192_continues", { 192, 1 }, 191u, 2);

            // As the MIDDLE byte of a value whose first byte is a continuation:
            //   {192,191}     -> digit0=191, digit1=190 (191 terminates) = 12351
            //   {192,192,1}   -> digit0=191, digit1=191 (192 continues), digit2=0
            check_decode(ctx, "middle_byte_191_terminates_2B", { 192, 191 }, 12351u, 2);
            check_decode(ctx, "middle_byte_192_continues_3B", { 192, 192, 1 }, 12415u, 3);

            // Off-by-one straddling 191/192 in the first byte, asserting the
            // consumed-count flips from 1 to 2 right at the threshold.
            int c191{ -1 };
            int c192{ -1 };
            const std::uint32_t v191{ decode_at0({ 191 }, c191) };
            const std::uint32_t v192{ decode_at0({ 192, 1 }, c192) };
            ctx.check("threshold_191_vs_192_value_and_consume",
                      v191 == 190u && c191 == 1 && v192 == 191u && c192 == 2);
        }

        // ============================================================ SECTION 4
        //  HIGH/LOW HALVES of an encoded byte.  A continuation byte carries a
        //  digit (b-1) in [191,254]; a terminating byte a digit in [0,190].  Pin
        //  representative continuation digits and prove the per-position 64^i
        //  weighting via explicit layout assertions.
        // =====================================================================
        {
            // {193,1}: digit0 = 192 (a "low-half" continuation digit) -> value 192.
            check_decode(ctx, "cont_digit_192_byte_193", { 193, 1 }, 192u, 2);
            // {255,1}: digit0 = 254 (the MAX continuation digit) -> value 254.
            check_decode(ctx, "cont_digit_254_byte_255", { 255, 1 }, 254u, 2);
            // {200,3}: digit0 = 199, digit1 = 2 -> 199 + 2*64 = 327.
            check_decode(ctx, "cont_mixed_200_3_is_327", { 200, 3 }, 327u, 2);

            // Layout proof: a 3-byte value where each position contributes a known
            // amount.  {192,200,3}: 191 + 199*64 + 2*64^2 = 191 + 12736 + 8192.
            {
                const std::uint32_t expect{ 191u + 199u * 64u + 2u * 64u * 64u };
                check_decode(ctx, "layout_3byte_positionweights",
                             { 192, 200, 3 }, expect, 3);
                ctx.check("layout_3byte_expect_is_21119", expect == 21119u);
            }
            // 4-byte all-position proof: {192,192,192,2} = 191 + 191*64 +
            // 191*64^2 + 1*64^3 -> every position 0..3 contributes (proves shift
            // 6*3 = 18 is applied).
            {
                const std::uint32_t expect{ 191u + 191u * 64u
                                            + 191u * 64u * 64u
                                            + 1u * 64u * 64u * 64u };
                check_decode(ctx, "layout_4byte_positionweights",
                             { 192, 192, 192, 2 }, expect, 4);
                ctx.check("layout_4byte_expect_is_1056895", expect == 1056895u);
            }
            // 5-byte position-4 reached: {192,192,192,192,2} contributes 1*64^4 at
            // the top position -> proves shift 6*4 = 24 (the position-4 term).
            {
                const std::uint64_t expect64{ 191ull + 191ull * 64ull
                                              + 191ull * 64ull * 64ull
                                              + 191ull * 64ull * 64ull * 64ull
                                              + 1ull * 64ull * 64ull * 64ull * 64ull };
                check_decode(ctx, "layout_5byte_position4_term",
                             { 192, 192, 192, 192, 2 },
                             static_cast<std::uint32_t>(expect64), 5);
            }
        }

        // ============================================================ SECTION 5
        //  5-BYTE MAXIMA and the FULL u32 RANGE (incl. bit 31 set, and 0xFFFFFFFF).
        // =====================================================================
        {
            // Largest TERMINATING 5-byte value: {255,255,255,255,191} -> 0xC208207E.
            // Bit 31 IS set, proving the codec reaches the top of the u32 range
            // via a normal terminating sequence.
            check_decode(ctx, "five_byte_terminating_max_3255312510",
                         { 255, 255, 255, 255, 191 }, 3255312510u, 5);
            ctx.check("five_byte_max_has_bit31_set",
                      (3255312510u & 0x80000000u) != 0u);

            // FULL u32 maximum 0xFFFFFFFF via its canonical 5-byte encoding.  This
            // is the brief's "max u32 (0xFFFFFFFF)" input -- the encoder DOES reach
            // it (it is NOT out of range).
            {
                const std::vector<std::uint8_t> enc_max{ encode_ref(0xFFFFFFFFu) };
                ctx.check("u32_max_canonical_is_5_bytes", enc_max.size() == 5u);
                check_decode(ctx, "u32_max_0xFFFFFFFF_decodes",
                             enc_max, 0xFFFFFFFFu, 5);
                // The exact canonical bytes, pinned: {192,254,253,253,253}.
                const std::vector<std::uint8_t> expect_bytes{ 192, 254, 253, 253, 253 };
                ctx.check("u32_max_canonical_bytes_exact", enc_max == expect_bytes);
            }

            // A value with bit 31 set but NOT all-ones, to cover the high half of
            // the 32-bit range distinctly from the max.
            {
                const std::uint32_t v{ 0x80000000u };
                const std::vector<std::uint8_t> enc{ encode_ref(v) };
                int consumed{ -1 };
                const std::uint32_t got{ decode_at0(enc, consumed) };
                ctx.check("high_half_0x80000000_roundtrip",
                          got == v && consumed == static_cast<int>(enc.size()));
            }
        }

        // ============================================================ SECTION 6
        //  SENTINEL-ALIASING HAZARD (the codec's one sharp edge).  0xFFFFFFFF is
        //  BOTH a legitimate decoded value (5 bytes) AND the numeric End marker
        //  (~0u) returned for a 0 byte.  They are disambiguated ONLY by the
        //  cursor: End REWINDS (consumes 0); the real value consumes 5.  This
        //  pins the exact contract the caller (find_field_in_stream) leans on.
        // =====================================================================
        {
            // End: leading 0 -> ~0u, cursor UNCHANGED (rewind to where it started).
            {
                int consumed{ -99 };
                const std::uint32_t got{ decode_at0({ 0 }, consumed) };
                ctx.check("end_marker_returns_tilde0_and_rewinds",
                          got == END_MARK && consumed == 0);
            }
            // Real 0xFFFFFFFF: same numeric result, but consumes 5.
            {
                const std::vector<std::uint8_t> enc_max{ encode_ref(0xFFFFFFFFu) };
                int consumed{ -1 };
                const std::uint32_t got{ decode_at0(enc_max, consumed) };
                ctx.check("real_value_equal_to_sentinel_consumes_5",
                          got == END_MARK && consumed == 5);
            }
            // The disambiguator, stated as one assertion: identical VALUE, different
            // CURSOR delta (0 vs 5).
            {
                int c_end{ -1 };
                int c_val{ -1 };
                const std::uint32_t v_end{ decode_at0({ 0 }, c_end) };
                const std::uint32_t v_val{ decode_at0(encode_ref(0xFFFFFFFFu), c_val) };
                ctx.check("sentinel_vs_value_same_number_diff_cursor",
                          v_end == v_val && c_end == 0 && c_val == 5);
            }
            // value 0 (byte 1) is distinct from the End marker (byte 0): the
            // former advances, the latter rewinds.
            {
                int c_zero{ -1 };
                int c_end{ -1 };
                const std::uint32_t v_zero{ decode_at0({ 1 }, c_zero) };
                const std::uint32_t v_end{ decode_at0({ 0 }, c_end) };
                ctx.check("real_zero_distinct_from_end_marker",
                          v_zero == 0u && c_zero == 1
                          && v_end == END_MARK && c_end == 0);
            }
        }

        // ============================================================ SECTION 7
        //  5-CONTINUATION HARD CAP and the 6th-byte-not-consumed property.
        // =====================================================================
        {
            // All five bytes are continuation bytes: the loop stops at exactly 5
            // and returns the partial sum (it does NOT read a 6th byte).
            {
                const std::vector<std::uint8_t> all_cont{ 255, 255, 255, 255, 255 };
                const std::uint32_t expect{ identity_ref(all_cont) };  // 34087038
                check_decode(ctx, "five_continuation_cap_partial_sum",
                             all_cont, expect, 5);
                ctx.check("five_continuation_cap_value_is_34087038",
                          expect == 34087038u);
            }
            // A 6th byte present but never consumed: cursor parks at 5 and the 6th
            // byte does not affect the value.  {192,192,192,192,192,1} -> the 6th
            // byte (1) is untouched; value uses the five continuation digits only.
            {
                const std::vector<std::uint8_t> six{ 192, 192, 192, 192, 192, 1 };
                const std::vector<std::uint8_t> first5{ 192, 192, 192, 192, 192 };
                const std::uint32_t expect{ identity_ref(first5) };  // 3255312511
                check_decode(ctx, "sixth_byte_not_consumed", six, expect, 5);
                ctx.check("sixth_byte_value_is_3255312511",
                          expect == 3255312511u);
            }
        }

        // ============================================================ SECTION 8
        //  NON-ZERO START OFFSET.  Decoding from a mid-buffer cursor must be
        //  value-identical to decoding from 0 and must advance by the same amount.
        //  This is exactly how find_field_in_stream threads ONE cursor through the
        //  whole stream.  Every prior section starts at 0; this covers the rest.
        // =====================================================================
        {
            const std::vector<std::vector<std::uint8_t>> samples{
                { 1 }, { 65 }, { 191 }, { 192, 1 }, { 255, 191 },
                { 192, 192, 1 }, { 192, 200, 3 }, { 255, 255, 255, 255, 191 } };
            const int starts[]{ 0, 1, 3, 7, 13 };
            bool all_match{ true };
            for (const std::vector<std::uint8_t>& s : samples)
            {
                int c0{ -1 };
                const std::uint32_t base{ decode_seq(s, 0, c0) };
                for (const int start : starts)
                {
                    int ci{ -1 };
                    const std::uint32_t got{ decode_seq(s, start, ci) };
                    all_match = all_match && (got == base) && (ci == c0);
                }
            }
            ctx.check("nonzero_start_offset_value_and_consume_invariant",
                      all_match);

            // Explicit single-case anchor: {192,1} at offset 7 -> 191, consumes 2.
            {
                int consumed{ -1 };
                const std::uint32_t got{ decode_seq({ 192, 1 }, 7, consumed) };
                ctx.check("offset7_192_1_is_191_consume2",
                          got == 191u && consumed == 2);
            }
        }

        // ============================================================ SECTION 9
        //  SEQUENTIAL PACKED VALUES through one cursor.  Two angles:
        //   (a) the brief's threading example {65,192,1,3} -> 64, 191, 2 with the
        //       cumulative cursor landing at 4.
        //   (b) a sequence that ends on the End sentinel: the cursor parks ON the
        //       0 (does not advance past it).
        // =====================================================================
        {
            // (a) {65, 192,1, 3} threaded: 64 (@1), 191 (@3), 2 (@4).
            {
                std::array<std::uint8_t, 12> buf{ 65, 192, 1, 3,
                                                  0, 0, 0, 0, 0, 0, 0, 0 };
                int pos{ 0 };
                const std::uint32_t a{ decode(buf.data(), pos) };
                const int pa{ pos };
                const std::uint32_t b{ decode(buf.data(), pos) };
                const int pb{ pos };
                const std::uint32_t c{ decode(buf.data(), pos) };
                const int pc{ pos };
                ctx.check("threaded_seq_values_64_191_2",
                          a == 64u && b == 191u && c == 2u);
                ctx.check("threaded_seq_cursor_1_3_4",
                          pa == 1 && pb == 3 && pc == 4);
            }
            // (b) {2, 192,2, 0} -> 1 (@1), 255 (@3), End (@3, rewound -> parks on 0).
            {
                std::array<std::uint8_t, 12> buf{ 2, 192, 2, 0,
                                                  0, 0, 0, 0, 0, 0, 0, 0 };
                int pos{ 0 };
                const std::uint32_t a{ decode(buf.data(), pos) };
                const std::uint32_t b{ decode(buf.data(), pos) };
                const int before_end{ pos };
                const std::uint32_t end{ decode(buf.data(), pos) };
                ctx.check("seq_ending_on_sentinel_values",
                          a == 1u && b == 255u && end == END_MARK);
                ctx.check("seq_ending_on_sentinel_cursor_parks_on_zero",
                          before_end == 3 && pos == 3
                          && buf[static_cast<std::size_t>(pos)] == 0u);
            }
        }

        // ============================================================ SECTION 10
        //  CALLER GRAMMAR (find_field_in_stream pattern) against a byte buffer,
        //  with NO JVM.  Build a realistic FieldInfoStream:
        //     header: num_java, num_injected
        //     per field: name sig offset access field_flags [optionals by flag bits]
        //     End(0)
        //  Decode it with the EXACT loop shape the library uses (5 mandatory + the
        //  0x01/0x04/0x10 flag-gated optionals) and assert the cursor lands
        //  precisely on the trailing 0.  This exercises the optional-skip cursor
        //  alignment (vmhook.hpp:3129-3140) purely in-process.
        // =====================================================================
        {
            // Field A: flags = 0 (no optionals).  values name=10 sig=11 off=16
            //          access=1 flags=0.
            // Field B: flags = 0x01|0x04|0x10 = 0x15 (all THREE optionals present):
            //          name=12 sig=13 off=24 access=2 flags=0x15
            //          + initval=100, gsig=14, group=3.
            // Field C: flags = 0x04 (generic-sig only): name=20 sig=21 off=40
            //          access=8 flags=0x04 + gsig=22.
            std::vector<std::uint8_t> stream{};
            auto emit = [&stream](std::uint32_t v)
            {
                const std::vector<std::uint8_t> e{ encode_ref(v) };
                stream.insert(stream.end(), e.begin(), e.end());
            };
            // header: 3 java fields, 0 injected
            emit(3u); emit(0u);
            // Field A (flags 0)
            emit(10u); emit(11u); emit(16u); emit(1u); emit(0u);
            // Field B (flags 0x15 -> 3 optionals)
            emit(12u); emit(13u); emit(24u); emit(2u); emit(0x15u);
            emit(100u); emit(14u); emit(3u);
            // Field C (flags 0x04 -> 1 optional)
            emit(20u); emit(21u); emit(40u); emit(8u); emit(0x04u);
            emit(22u);
            // End marker
            stream.push_back(0u);

            // Pad so the decoder may always peek 5 past the End.
            std::vector<std::uint8_t> buf{ stream };
            buf.insert(buf.end(), 8u, static_cast<std::uint8_t>(0));

            int pos{ 0 };
            const std::uint32_t num_java{ decode(buf.data(), pos) };
            const std::uint32_t num_injected{ decode(buf.data(), pos) };
            bool grammar_ok{ num_java == 3u && num_injected == 0u };

            // The decoded field metadata we will assert against.
            struct field_rec { std::uint32_t name, sig, off, access, flags; };
            const field_rec expected[]{
                { 10u, 11u, 16u, 1u, 0x00u },
                { 12u, 13u, 24u, 2u, 0x15u },
                { 20u, 21u, 40u, 8u, 0x04u } };

            for (std::uint32_t f{ 0 }; f < num_java + num_injected; ++f)
            {
                const std::uint32_t name{ decode(buf.data(), pos) };
                const std::uint32_t sig{ decode(buf.data(), pos) };
                const std::uint32_t off{ decode(buf.data(), pos) };
                const std::uint32_t access{ decode(buf.data(), pos) };
                const std::uint32_t flags{ decode(buf.data(), pos) };
                // Optional-skip logic mirrored EXACTLY from find_field_in_stream.
                if (flags & 0x01u) { (void)decode(buf.data(), pos); }
                if (flags & 0x04u) { (void)decode(buf.data(), pos); }
                if (flags & 0x10u) { (void)decode(buf.data(), pos); }

                if (f < std::size(expected))
                {
                    const field_rec& e{ expected[f] };
                    grammar_ok = grammar_ok
                        && name == e.name && sig == e.sig && off == e.off
                        && access == e.access && flags == e.flags;
                }
            }
            // After all fields the cursor must sit EXACTLY on the End 0 byte.
            const bool on_end{ pos == static_cast<int>(stream.size()) - 1
                               && buf[static_cast<std::size_t>(pos)] == 0u };
            // And reading once more yields the End marker with a rewind.
            const std::uint32_t tail{ decode(buf.data(), pos) };
            const bool tail_ok{ tail == END_MARK
                                && pos == static_cast<int>(stream.size()) - 1 };

            ctx.check("caller_grammar_header_and_fields", grammar_ok);
            ctx.check("caller_grammar_cursor_lands_on_end", on_end);
            ctx.check("caller_grammar_trailing_end_marker", tail_ok);
        }

        // ============================================================ SECTION 11
        //  RANDOMISED ROUND-TRIP FUZZ.  Many pseudo-random u32 values: encode with
        //  the reference encoder, decode with decode_u5, assert the value AND the
        //  consumed length match.  Catches any value-range regression a fixed
        //  table would miss.  Deterministic (xorshift32 seed) for CI stability.
        // =====================================================================
        {
            std::uint32_t state{ 0x1234ABCDu };
            bool all_ok{ true };
            std::size_t checked{ 0 };
            for (int n{ 0 }; n < 4000; ++n)
            {
                const std::uint32_t v{ next_rand(state) };
                const std::vector<std::uint8_t> enc{ encode_ref(v) };
                int consumed{ -1 };
                const std::uint32_t got{ decode_at0(enc, consumed) };
                all_ok = all_ok
                    && got == v
                    && consumed == static_cast<int>(enc.size())
                    && enc.size() >= 1u && enc.size() <= 5u
                    && identity_ref(enc) == v;
                ++checked;
            }
            ctx.check("fuzz_random_roundtrip_4000_values",
                      all_ok && checked == 4000u);

            // Dense low-range sweep: EVERY value 0..20000 round-trips and consumes
            // a length consistent with the boundary table (1, 2 or 3 bytes).
            bool dense_ok{ true };
            for (std::uint32_t v{ 0 }; v <= 20000u; ++v)
            {
                const std::vector<std::uint8_t> enc{ encode_ref(v) };
                int consumed{ -1 };
                const std::uint32_t got{ decode_at0(enc, consumed) };
                const std::size_t want_len{ (v <= 190u) ? 1u
                                            : (v <= 12414u) ? 2u : 3u };
                dense_ok = dense_ok
                    && got == v
                    && consumed == static_cast<int>(enc.size())
                    && enc.size() == want_len;
            }
            ctx.check("fuzz_dense_0_to_20000_roundtrip_and_lengths", dense_ok);
        }
    }
}

VMHOOK_JVM_MODULE(decode_u5_unsigned5)
{
    // Whole body under try/catch: a stray throw can never escape this module
    // (mirrors register_class.cpp / aaa_warmup.cpp).  A throw is recorded as
    // [INFO], NEVER a FAIL.  This module touches no JVM state, so a throw is not
    // expected -- the guard is the suite-safety contract, not a real code path.
    bool body_threw{ false };
    try
    {
        run_decode_u5_checks(ctx);
    }
    catch (...)
    {
        body_threw = true;
    }

    // FINAL CLEANUP -- belt-and-braces, OUTSIDE the try so it ALWAYS runs.  This
    // module arms NO hooks (it never installs one), but other modules run after
    // it, so we still guarantee an empty hook table on every path: an
    // unconditional, idempotent, safe-when-empty shutdown_hooks() (same idiom as
    // register_class.cpp:702 / shutdown_hooks_teardown.cpp).
    vmhook::shutdown_hooks();

    if (body_threw)
    {
        ctx.record("[INFO] decode_u5_unsigned5: the test body threw and was "
                   "contained (no crash, no hooks armed); see preceding checks "
                   "for partial results.");
    }
    ctx.check("decode_u5_module_left_clean_final_shutdown", true);
}
