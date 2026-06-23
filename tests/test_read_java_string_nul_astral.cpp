// Standalone (no-JVM) characterization of how the make_java_string ENCODE side
// and the read_java_string DECODE side handle two adversarial input classes:
//
//   (1) EMBEDDED INTERIOR NUL — e.g. "a\0b" (3 bytes, a real U+0000 in the
//       middle, NOT a C-string terminator).
//   (2) ASTRAL / SUPPLEMENTARY code points — e.g. U+1F600 (4-byte standard UTF-8
//       F0 9F 98 80), which on the Java side is carried as a UTF-16 SURROGATE
//       PAIR (high D800..DBFF + low DC00..DFFF).
//
// Both make_java_string (vmhook.hpp make_java_string / its TLAB + JNI-NewString
// fallback) and the GetString-free read_java_string decode loops funnel their
// byte<->char-unit conversion through ONE shared, pure, JVM-free function:
//     vmhook::detail::utf8_to_utf16(std::string_view) -> std::vector<uint16_t>
// (vmhook.hpp:12762).  That function is directly callable here with no live oop
// and no running JVM, so the ENCODE-side behavior is asserted against the REAL
// library code — not a reimplementation.
//
// The read_java_string DECODE side (vmhook.hpp:20416) needs a live String oop +
// VMStructs and so cannot run here; but its two decode lambdas — `append_utf8`
// (vmhook.hpp:20593) and `utf16_to_utf8` (vmhook.hpp:20621) — are pure code-unit
// math.  They are MIRRORED VERBATIM below as a decode oracle (kept byte-for-byte
// identical to the source so a future divergence is caught by code review), and
// the end-to-end round-trip  utf8_to_utf16(in) --decode--> out  is asserted to
// reproduce `in` exactly.  The live-oop end-to-end is covered by the JVM module
// tests/jvm/modules/read_java_string.cpp + make_java_string.cpp; this file pins
// the NUL/astral edge of the SHARED pure core that both rest on.
//
// ───────────────────────── CHARACTERIZED BEHAVIOR (from source) ──────────────
// Asserted as the ACTUAL documented behavior, NOT an idealized guess:
//
//   * EMBEDDED NUL IS LENGTH-PRESERVING AND NUL-CLEAN on the counted UTF-16 path.
//     utf8_to_utf16 iterates `i < value.size()` (a COUNTED loop, vmhook.hpp:12767)
//     and treats byte 0x00 as the ordinary code point U+0000 (b0 < 0x80 arm,
//     12772) — it does NOT stop at a NUL.  So "a\0b" -> EXACTLY the 3 code units
//     {0x0061, 0x0000, 0x0062}.  make_java_string therefore builds a length-3
//     Java String containing a real interior NUL (TLAB byte[]/char[] copy or the
//     counted JNI NewString slot-163 fallback, both length-counted), and the
//     read_java_string decode (LATIN1 append_utf8(0) / UTF16 chars[i]==0) emits a
//     real '\0' back.  This is the CORRECT, length-preserving behavior — there is
//     NO silent interior-NUL truncation on this path.
//
//     [INFO] The NUL-TRUNCATING path is the SEPARATE legacy JNI NewStringUTF
//     (slot 167, "modified UTF-8" C string) which the library DELIBERATELY does
//     NOT use for String construction here precisely because it truncates "a\0b"
//     to "a" and mangles astral scalars (documented at vmhook.hpp:12824-12834).
//     This test pins that the chosen path is the NUL-clean one.
//
//   * ASTRAL ROUND-TRIPS AS ONE SCALAR VIA A SURROGATE PAIR.
//     utf8_to_utf16 decodes a 4-byte UTF-8 scalar (cp >= 0x10000, 12798) into a
//     HIGH+LOW surrogate pair (12801-12802); the decode oracle recombines the
//     pair into the single astral cp (mirror of utf16_to_utf8, 20628-20636) and
//     append_utf8 re-emits the 4-byte form (20610-20616) — NOT two 3-byte CESU-8
//     halves.  So U+1F600 round-trips byte-identically.
//
//   * MALFORMED / TRUNCATED UTF-8 -> U+FFFD (one replacement char per bad lead
//     byte), per the cp=0xFFFD default + the `(i+k) < size` continuation guards
//     (12770, 12776/12782/12789).  A lone 0xFF or a truncated multibyte tail
//     becomes EF BF BD, length-preserving in code-unit terms.
//
// Out of scope (needs a live JVM / real compressed-oop heap): the actual
// String-oop construction, the +12/+16 arrayOop reads, and decode_oop_pointer —
// all covered by the JVM module tests named above.

#include <vmhook/vmhook.hpp>

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}
static auto info(const char* name, const char* detail) -> void
{
    std::printf("[INFO] %s: %s\n", name, detail);
}

// ───────────────────────── decode oracle ────────────────────────────────────
// MIRROR of read_java_string's two decode lambdas, kept BYTE-FOR-BYTE identical
// to vmhook.hpp:20593 (append_utf8) and vmhook.hpp:20621 (utf16_to_utf8) so this
// oracle has the same NUL/astral/surrogate semantics as the real decode.  If the
// library's decode changes, this copy diverging is the signal to re-characterize.

static auto append_utf8(std::string& out, std::uint32_t cp) -> void
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
}

static auto utf16_to_utf8(const std::vector<std::uint16_t>& chars) -> std::string
{
    std::string out;
    const std::int32_t count{ static_cast<std::int32_t>(chars.size()) };
    for (std::int32_t i{ 0 }; i < count; ++i)
    {
        std::uint32_t cp{ chars[static_cast<std::size_t>(i)] };
        if (cp >= 0xD800u && cp <= 0xDBFFu && (i + 1) < count)
        {
            const std::uint16_t low{ chars[static_cast<std::size_t>(i + 1)] };
            if (low >= 0xDC00u && low <= 0xDFFFu)
            {
                cp = 0x10000u + ((cp - 0xD800u) << 10) + (low - 0xDC00u);
                ++i;
            }
        }
        append_utf8(out, cp);
    }
    return out;
}

// Convenience: full library encode then oracle decode (the round trip a real
// make_java_string -> read_java_string would perform through the SAME shared
// utf8_to_utf16 core).
static auto round_trip(std::string_view in) -> std::string
{
    return utf16_to_utf8(vmhook::detail::utf8_to_utf16(in));
}

// ───────────────────────── embedded-NUL characterization ────────────────────
static auto test_interior_nul_is_length_preserving() -> void
{
    // Build "a\0b" without a raw NUL byte literal in source.
    std::string in;
    in += 'a';
    in += static_cast<char>(0x00);
    in += 'b';

    const std::vector<std::uint16_t> units{ vmhook::detail::utf8_to_utf16(in) };

    // The COUNTED loop sees all 3 bytes; the NUL is U+0000, not a terminator.
    check("interior_nul: utf8_to_utf16 emits 3 code units (no C-string cutoff)",
          units.size() == 3);
    check("interior_nul: unit[0] == U+0061 'a'",
          units.size() == 3 && units[0] == 0x0061u);
    check("interior_nul: unit[1] == U+0000 (the real interior NUL, preserved)",
          units.size() == 3 && units[1] == 0x0000u);
    check("interior_nul: unit[2] == U+0062 'b'",
          units.size() == 3 && units[2] == 0x0062u);

    // Decode oracle (LATIN1 append_utf8(0) / UTF16 chars[i]==0 both emit '\0').
    const std::string out{ utf16_to_utf8(units) };
    check("interior_nul: round-trip length is 3 (NOT truncated to 1)",
          out.size() == 3);
    check("interior_nul: round-trip is byte-identical to \"a\\0b\"",
          out == in);
    check("interior_nul: round-trip middle byte is a real NUL",
          out.size() == 3 && out[1] == '\0');

    info("interior_nul",
         out.size() == 3
             ? "make_java_string path is NUL-CLEAN / length-preserving (counted "
               "UTF-16). The truncating NewStringUTF C-string path is deliberately "
               "unused for String construction (vmhook.hpp:12824)."
             : "UNEXPECTED truncation — re-read source!");
}

static auto test_leading_and_trailing_nul() -> void
{
    std::string lead;
    lead += static_cast<char>(0x00);
    lead += 'x';
    const std::vector<std::uint16_t> lead_units{ vmhook::detail::utf8_to_utf16(lead) };
    check("leading_nul: \"\\0x\" -> 2 units {0x0000, 0x0078}",
          lead_units.size() == 2 && lead_units[0] == 0x0000u && lead_units[1] == 0x0078u);
    check("leading_nul: round-trip byte-identical",
          round_trip(lead) == lead);

    std::string trail;
    trail += 'x';
    trail += static_cast<char>(0x00);
    const std::vector<std::uint16_t> trail_units{ vmhook::detail::utf8_to_utf16(trail) };
    check("trailing_nul: \"x\\0\" -> 2 units {0x0078, 0x0000}",
          trail_units.size() == 2 && trail_units[0] == 0x0078u && trail_units[1] == 0x0000u);
    check("trailing_nul: round-trip byte-identical (trailing NUL kept)",
          round_trip(trail) == trail);

    // Multiple interior NULs + an empty-between case.
    std::string multi;
    multi += static_cast<char>(0x00);
    multi += static_cast<char>(0x00);
    multi += 'z';
    check("multi_nul: \"\\0\\0z\" -> 3 units, round-trip byte-identical",
          vmhook::detail::utf8_to_utf16(multi).size() == 3 && round_trip(multi) == multi);
}

// ───────────────────────── astral / surrogate characterization ──────────────
static auto test_astral_surrogate_pair() -> void
{
    // U+1F600 GRINNING FACE == standard UTF-8 F0 9F 98 80.  Built from bytes so
    // no raw non-ASCII literal sits in source.
    std::string in;
    in += static_cast<char>(0xF0);
    in += static_cast<char>(0x9F);
    in += static_cast<char>(0x98);
    in += static_cast<char>(0x80);

    const std::vector<std::uint16_t> units{ vmhook::detail::utf8_to_utf16(in) };
    check("astral: U+1F600 decodes to a 2-unit UTF-16 surrogate pair",
          units.size() == 2);
    check("astral: high surrogate == 0xD83D",
          units.size() == 2 && units[0] == 0xD83Du);
    check("astral: low surrogate == 0xDE00",
          units.size() == 2 && units[1] == 0xDE00u);

    // Decode recombines the pair into ONE astral scalar -> 4-byte UTF-8.
    const std::string out{ utf16_to_utf8(units) };
    check("astral: round-trip is the original 4 bytes (one scalar, not 2x CESU-8)",
          out.size() == 4 && out == in);
    check("astral: round-trip lead byte is 0xF0 (4-byte UTF-8 form)",
          out.size() == 4 && static_cast<std::uint8_t>(out[0]) == 0xF0u);

    info("astral",
         out == in
             ? "U+1F600 round-trips byte-identically as a single surrogate-paired "
               "scalar; no CESU-8 / modified-UTF-8 split."
             : "UNEXPECTED astral mangling — re-read source!");

    // Highest scalar U+10FFFF == F4 8F BF BF -> {0xDBFF, 0xDFFF}.
    std::string top;
    top += static_cast<char>(0xF4);
    top += static_cast<char>(0x8F);
    top += static_cast<char>(0xBF);
    top += static_cast<char>(0xBF);
    const std::vector<std::uint16_t> top_units{ vmhook::detail::utf8_to_utf16(top) };
    check("astral_max: U+10FFFF -> {0xDBFF, 0xDFFF}",
          top_units.size() == 2 && top_units[0] == 0xDBFFu && top_units[1] == 0xDFFFu);
    check("astral_max: round-trip byte-identical",
          round_trip(top) == top);
}

// NUL right next to an astral scalar — proves the counted loop doesn't let a
// preceding/following NUL disturb surrogate emission.
static auto test_nul_adjacent_astral() -> void
{
    std::string in;
    in += 'a';
    in += static_cast<char>(0x00);
    in += static_cast<char>(0xF0); // U+1F600
    in += static_cast<char>(0x9F);
    in += static_cast<char>(0x98);
    in += static_cast<char>(0x80);
    in += 'b';

    const std::vector<std::uint16_t> units{ vmhook::detail::utf8_to_utf16(in) };
    // a, NUL, high, low, b  == 5 units.
    check("nul+astral: 'a' \\0 U+1F600 'b' -> 5 code units",
          units.size() == 5);
    check("nul+astral: units == {0x61, 0x00, 0xD83D, 0xDE00, 0x62}",
          units.size() == 5 && units[0] == 0x0061u && units[1] == 0x0000u
              && units[2] == 0xD83Du && units[3] == 0xDE00u && units[4] == 0x0062u);
    check("nul+astral: round-trip byte-identical (7 bytes back)",
          round_trip(in) == in && in.size() == 7);
}

// ───────────────────────── malformed-input characterization ─────────────────
static auto test_malformed_yields_replacement() -> void
{
    // Lone 0xFF (not a valid lead byte) -> U+FFFD -> EF BF BD.
    std::string lone_ff;
    lone_ff += static_cast<char>(0xFF);
    const std::vector<std::uint16_t> ff_units{ vmhook::detail::utf8_to_utf16(lone_ff) };
    check("malformed: lone 0xFF -> single U+FFFD code unit",
          ff_units.size() == 1 && ff_units[0] == 0xFFFDu);
    const std::string ff_out{ utf16_to_utf8(ff_units) };
    check("malformed: U+FFFD encodes to EF BF BD",
          ff_out.size() == 3
              && static_cast<std::uint8_t>(ff_out[0]) == 0xEFu
              && static_cast<std::uint8_t>(ff_out[1]) == 0xBFu
              && static_cast<std::uint8_t>(ff_out[2]) == 0xBDu);

    // Truncated 3-byte lead with no continuation (E0 at end-of-buffer): the
    // `(i+2) < size` guard fails, so it falls through to the U+FFFD default.
    std::string trunc;
    trunc += static_cast<char>(0xE0);
    const std::vector<std::uint16_t> trunc_units{ vmhook::detail::utf8_to_utf16(trunc) };
    check("malformed: truncated 3-byte lead 0xE0 (no tail) -> U+FFFD",
          trunc_units.size() == 1 && trunc_units[0] == 0xFFFDu);

    // A valid scalar after a malformed byte is still decoded (loop continues).
    std::string mixed;
    mixed += static_cast<char>(0xFF);
    mixed += 'k';
    const std::vector<std::uint16_t> mixed_units{ vmhook::detail::utf8_to_utf16(mixed) };
    check("malformed: 0xFF then 'k' -> {0xFFFD, 0x006B}",
          mixed_units.size() == 2 && mixed_units[0] == 0xFFFDu && mixed_units[1] == 0x006Bu);
}

// ───────────────────────── ASCII / empty baseline (no regression) ───────────
static auto test_ascii_and_empty_baseline() -> void
{
    check("baseline: empty input -> 0 code units",
          vmhook::detail::utf8_to_utf16(std::string_view{}).empty());
    check("baseline: empty round-trip is empty",
          round_trip(std::string_view{}).empty());

    const std::string_view hello{ "hello" };
    const std::vector<std::uint16_t> h_units{ vmhook::detail::utf8_to_utf16(hello) };
    check("baseline: \"hello\" -> 5 units, each byte 1:1 to a BMP unit",
          h_units.size() == 5 && h_units[0] == 'h' && h_units[4] == 'o');
    check("baseline: \"hello\" round-trip byte-identical",
          round_trip(hello) == hello);

    // 2-byte UTF-8 sanity: U+00E9 'é' == C3 A9 -> single unit 0x00E9 -> back to C3 A9.
    std::string e_acute;
    e_acute += static_cast<char>(0xC3);
    e_acute += static_cast<char>(0xA9);
    const std::vector<std::uint16_t> e_units{ vmhook::detail::utf8_to_utf16(e_acute) };
    check("baseline: U+00E9 -> single unit 0x00E9, round-trip byte-identical",
          e_units.size() == 1 && e_units[0] == 0x00E9u && round_trip(e_acute) == e_acute);
}

auto main() -> int
{
    test_interior_nul_is_length_preserving();
    test_leading_and_trailing_nul();
    test_astral_surrogate_pair();
    test_nul_adjacent_astral();
    test_malformed_yields_replacement();
    test_ascii_and_empty_baseline();

    if (failures == 0)
    {
        std::printf("vmhook read_java_string NUL/astral characterization: OK\n");
    }
    else
    {
        std::printf("vmhook read_java_string NUL/astral characterization: %d FAILURE(S)\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
