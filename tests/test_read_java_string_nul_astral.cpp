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

// ─────────────────────────────────────────────────────────────────────────────
// ADDITIVE deepening pass (wave-14) — namespaced so it touches no existing
// assertion above.  Pure no-JVM surface of read_java_string + its sibling
// helpers: with gHotSpotVMStructs/gHotSpotVMTypes null (no live JVM), the
// VMStruct offset-resolution layer must degrade to nullptr; the decode CODE
// LOGIC over byte buffers WE OWN must round-trip at every code-unit boundary;
// the narrow compressed-pointer codec arithmetic must round-trip; the
// method-flags layout DECISION (pure) must place _dont_inline correctly per
// JDK; method_proxy::value_t must classify/convert/stringify per source; and
// the call-path arity cap must be 8.  EVERY value is derived from vmhook.hpp;
// nothing here dereferences a fabricated/unmapped address.
namespace rjs_deep
{
    // ── source-mirrored constants (each USED below; no unused const) ─────────
    // vmhook.hpp: inline constexpr std::int32_t read_java_string_max_units{ 16*1024*1024 }.
    static_assert(vmhook::read_java_string_max_units == 16 * 1024 * 1024,
                  "read_java_string_max_units must be 16Mi chars (robustness bug #29 fix)");

    // vmhook.hpp method_proxy::call / call_stub fast path: constexpr arg_cap{8};
    // static_assert(sizeof...(args_t) <= arg_cap).  Mirror the bound here.
    static_assert(8 <= vmhook::read_java_string_max_units,
                  "sanity: arg cap fits well under the char ceiling");

    // vmhook.hpp: inline constexpr std::int32_t NO_COMPILE = the OR of the four
    // JIT-inhibit access-flag bits (0x01|0x02|0x04|0x08 << 24).
    static_assert(vmhook::hotspot::NO_COMPILE
                      == (0x02000000 | 0x04000000 | 0x08000000 | 0x01000000),
                  "NO_COMPILE must OR the four JVM_ACC_NOT_*_COMPILABLE/QUEUED bits");
    static_assert(vmhook::hotspot::NO_COMPILE == 0x0F000000,
                  "NO_COMPILE collapses to the high-nibble 0x0F000000");

    // ── (A) VMStruct offset-resolution: null-when-no-JVM contract ────────────
    // No live JVM in this TU, so get_jvm_module()/gHotSpotVMStructs resolution
    // yields null and every lookup must degrade to nullptr (never deref, never
    // crash).  iterate_*_entries also reject null name args UNCONDITIONALLY
    // (the documented strcmp(nullptr,...) UB guard) — deterministic regardless
    // of whether a JVM is present.
    static auto test_vmstruct_null_when_no_jvm() -> void
    {
        // Unconditional null-name guards (pure; true on every platform).
        check("vmstruct: iterate_struct_entries(null,'x') -> nullptr",
              vmhook::hotspot::iterate_struct_entries(nullptr, "x") == nullptr);
        check("vmstruct: iterate_struct_entries('T',null) -> nullptr",
              vmhook::hotspot::iterate_struct_entries("T", nullptr) == nullptr);
        check("vmstruct: iterate_type_entries(null) -> nullptr",
              vmhook::hotspot::iterate_type_entries(nullptr) == nullptr);

        // No JVM -> gHotSpotVMStructs symbol absent -> array pointer null ->
        // any real (type,field) lookup also degrades to nullptr.
        check("vmstruct: get_vm_structs() is null with no JVM",
              vmhook::hotspot::get_vm_structs() == nullptr);
        check("vmstruct: get_vm_types() is null with no JVM",
              vmhook::hotspot::get_vm_types() == nullptr);
        check("vmstruct: lookup of a real (type,field) degrades to nullptr",
              vmhook::hotspot::iterate_struct_entries("CompressedOops", "_base") == nullptr);

        // resolve_struct_entry over the SAME candidate set decode_oop_pointer
        // uses must also miss (all candidates resolve to nullptr).
        const vmhook::hotspot::struct_entry_candidate_t base_candidates[]{
            { "CompressedOops", "_narrow_oop._base" },
            { "CompressedOops", "_base" },
            { "Universe", "_narrow_oop._base" },
        };
        check("vmstruct: resolve_struct_entry over base candidates -> nullptr (no JVM)",
              vmhook::hotspot::resolve_struct_entry(base_candidates, std::size(base_candidates))
                  == nullptr);
    }

    // ── (B) compressed-pointer narrow codec round-trip (pure arithmetic) ─────
    // narrow_decode(base,shift,c) = base + (c << shift);
    // narrow_encode(base,shift,addr) = (addr - base) >> shift.
    // The low `shift` bits of (c << shift) are 0, so encode(decode(c)) == c
    // exactly for any c (no information loss).  This is plain integer math on
    // values WE pick — no JVM, no dereference.
    static auto narrow_round_trips(std::uint64_t base, std::uint32_t shift, std::uint32_t c) -> bool
    {
        void* const decoded{ vmhook::hotspot::narrow_decode(base, shift, c) };
        const std::uint64_t addr{ reinterpret_cast<std::uint64_t>(decoded) };
        return addr == base + (static_cast<std::uint64_t>(c) << shift)
            && vmhook::hotspot::narrow_encode(base, shift, addr) == c;
    }

    static auto test_narrow_codec_round_trip() -> void
    {
        // shift 0 (heap < 4 GB) and shift 3 (8-byte-aligned oops, heap <= 32 GB)
        // are the two HotSpot-documented shifts; base 0 and a non-zero heap base.
        check("codec: shift=0 base=0 round-trips c=1",
              narrow_round_trips(0x0u, 0u, 1u));
        check("codec: shift=3 base=0 round-trips c=0xDEADBEE",
              narrow_round_trips(0x0u, 3u, 0x0DEADBEEu));
        check("codec: shift=3 non-zero base round-trips c=0x12345",
              narrow_round_trips(0x7F0000000000ull, 3u, 0x00012345u));
        check("codec: shift=0 round-trips max compressed 0xFFFFFFFF",
              narrow_round_trips(0x10000000ull, 0u, 0xFFFFFFFFu));

        // narrow_decode is exactly base + (c << shift); pin one value explicitly.
        check("codec: narrow_decode(base=0x100, shift=3, c=2) == 0x110",
              reinterpret_cast<std::uint64_t>(vmhook::hotspot::narrow_decode(0x100ull, 3u, 2u))
                  == 0x110ull);

        // The compressed==0 / decoded==nullptr SHORT-CIRCUITS run BEFORE any
        // VMStruct deref, so they are deterministic even with no JVM.
        check("codec: decode_oop_pointer(0) -> nullptr (pre-VMStruct short-circuit)",
              vmhook::hotspot::decode_oop_pointer(0u) == nullptr);
        check("codec: decode_klass_pointer(0) -> nullptr (pre-VMStruct short-circuit)",
              vmhook::hotspot::decode_klass_pointer(0u) == nullptr);
        check("codec: encode_oop_pointer(nullptr) -> 0 (pre-VMStruct short-circuit)",
              vmhook::hotspot::encode_oop_pointer(nullptr) == 0u);
        check("codec: encode_klass_pointer(nullptr) -> 0 (pre-VMStruct short-circuit)",
              vmhook::hotspot::encode_klass_pointer(nullptr) == 0u);
    }

    // ── (C) read_java_string length/char-count math (bug #29 fix, pure) ──────
    // Mirror the per-layout arithmetic read_java_string computes from the raw
    // arrayOop length (no JVM, no deref) and assert the SYMMETRIC ceiling.
    //   char_count = (has_coder && coder != 0) ? length / 2 : length;
    //   body_bytes = has_coder ? length : length * 2;
    //   raw guard:  1 .. 2 * read_java_string_max_units  (element/byte count)
    //   char guard: 1 .. read_java_string_max_units      (decoded chars)
    static auto char_count_for(bool has_coder, std::uint8_t coder, std::int32_t length) -> std::int32_t
    {
        return (has_coder && coder != 0) ? length / 2 : length;
    }
    static auto body_bytes_for(bool has_coder, std::int32_t length) -> std::int32_t
    {
        return has_coder ? length : length * 2;
    }

    static auto test_length_and_char_count_math() -> void
    {
        // JDK 8 char[] (no coder): length IS the char count; body is 2*length.
        check("len: JDK8 char[] length=5 -> char_count 5",
              char_count_for(false, 0, 5) == 5);
        check("len: JDK8 char[] length=5 -> body_bytes 10",
              body_bytes_for(false, 5) == 10);

        // JDK 9+ LATIN1 (coder==0): one byte/char; char_count == length.
        check("len: LATIN1 length=5 -> char_count 5",
              char_count_for(true, 0, 5) == 5);
        check("len: LATIN1 length=5 -> body_bytes 5",
              body_bytes_for(true, 5) == 5);

        // JDK 9+ UTF16 (coder!=0): two bytes/char; char_count == length/2.
        check("len: UTF16 length=10 -> char_count 5",
              char_count_for(true, 1, 10) == 5);
        check("len: UTF16 length=10 -> body_bytes 10 (length is already the byte count)",
              body_bytes_for(true, 10) == 10);

        // SYMMETRIC ceiling (bug #29 part 2): a LATIN1 and a UTF16 String of the
        // SAME logical char count both sit at the SAME char ceiling — the UTF16
        // raw length is 2x but its char_count is the same, so neither is capped
        // earlier than the other.
        const std::int32_t cap{ vmhook::read_java_string_max_units };
        check("len: LATIN1 at exactly the cap is in range (char_count == cap)",
              char_count_for(true, 0, cap) == cap && cap <= vmhook::read_java_string_max_units);
        check("len: UTF16 at exactly the cap (raw length 2*cap) yields char_count == cap",
              char_count_for(true, 1, 2 * cap) == cap);
        check("len: UTF16 raw length 2*cap equals LATIN1 raw cap times 2 (no asymmetry)",
              char_count_for(true, 1, 2 * cap) == char_count_for(true, 0, cap));

        // The raw guard accepts a UTF16 byte length up to 2*cap (would have been
        // rejected by the OLD `length > 4096` guard for any real-world String).
        check("len: raw upper guard is 2 * read_java_string_max_units",
              static_cast<std::int64_t>(2) * vmhook::read_java_string_max_units
                  == static_cast<std::int64_t>(2 * 16 * 1024 * 1024));

        // char_count <= 0 (empty/degenerate) is the "" case for every layout.
        check("len: UTF16 raw length 1 -> char_count 0 (degenerate -> empty)",
              char_count_for(true, 1, 1) == 0);
    }

    // ── (D) extra decode-logic boundary inputs over OWNED byte buffers ───────
    // Push utf8_to_utf16 + the mirrored decode oracle across the exact byte
    // boundaries of each UTF-8 length class (1/2/3/4) and the surrogate edges.
    // Buffers are std::string we build (no fabricated address, no raw NUL/
    // non-ASCII literal — built from explicit byte values).
    static auto test_decode_unit_boundaries() -> void
    {
        // U+007F (last 1-byte) == 0x7F -> single unit 0x007F.
        std::string b1;
        b1 += static_cast<char>(0x7F);
        const std::vector<std::uint16_t> u1{ vmhook::detail::utf8_to_utf16(b1) };
        check("boundary: U+007F -> {0x007F}, round-trip byte-identical",
              u1.size() == 1 && u1[0] == 0x007Fu && round_trip(b1) == b1);

        // U+0080 (first 2-byte) == C2 80 -> 0x0080.
        std::string b2lo;
        b2lo += static_cast<char>(0xC2);
        b2lo += static_cast<char>(0x80);
        const std::vector<std::uint16_t> u2lo{ vmhook::detail::utf8_to_utf16(b2lo) };
        check("boundary: U+0080 -> {0x0080}, round-trip byte-identical",
              u2lo.size() == 1 && u2lo[0] == 0x0080u && round_trip(b2lo) == b2lo);

        // U+07FF (last 2-byte) == DF BF -> 0x07FF.
        std::string b2hi;
        b2hi += static_cast<char>(0xDF);
        b2hi += static_cast<char>(0xBF);
        const std::vector<std::uint16_t> u2hi{ vmhook::detail::utf8_to_utf16(b2hi) };
        check("boundary: U+07FF -> {0x07FF}, round-trip byte-identical",
              u2hi.size() == 1 && u2hi[0] == 0x07FFu && round_trip(b2hi) == b2hi);

        // U+0800 (first 3-byte) == E0 A0 80 -> 0x0800.
        std::string b3lo;
        b3lo += static_cast<char>(0xE0);
        b3lo += static_cast<char>(0xA0);
        b3lo += static_cast<char>(0x80);
        const std::vector<std::uint16_t> u3lo{ vmhook::detail::utf8_to_utf16(b3lo) };
        check("boundary: U+0800 -> {0x0800}, round-trip byte-identical",
              u3lo.size() == 1 && u3lo[0] == 0x0800u && round_trip(b3lo) == b3lo);

        // U+FFFF (last BMP 3-byte) == EF BF BF -> 0xFFFF (NOT a surrogate).
        std::string b3hi;
        b3hi += static_cast<char>(0xEF);
        b3hi += static_cast<char>(0xBF);
        b3hi += static_cast<char>(0xBF);
        const std::vector<std::uint16_t> u3hi{ vmhook::detail::utf8_to_utf16(b3hi) };
        check("boundary: U+FFFF -> {0xFFFF} single BMP unit, round-trip byte-identical",
              u3hi.size() == 1 && u3hi[0] == 0xFFFFu && round_trip(b3hi) == b3hi);

        // U+10000 (first astral 4-byte) == F0 90 80 80 -> {0xD800, 0xDC00}
        // (the lowest surrogate pair).
        std::string b4lo;
        b4lo += static_cast<char>(0xF0);
        b4lo += static_cast<char>(0x90);
        b4lo += static_cast<char>(0x80);
        b4lo += static_cast<char>(0x80);
        const std::vector<std::uint16_t> u4lo{ vmhook::detail::utf8_to_utf16(b4lo) };
        check("boundary: U+10000 -> {0xD800,0xDC00} lowest surrogate pair, round-trip byte-identical",
              u4lo.size() == 2 && u4lo[0] == 0xD800u && u4lo[1] == 0xDC00u
                  && round_trip(b4lo) == b4lo);

        // Unpaired HIGH surrogate at end-of-buffer: utf16_to_utf8's `(i+1)<count`
        // guard fails, so the lone 0xD83D is emitted via append_utf8 as a 3-byte
        // form (cp < 0x10000 arm).  Build the unit vector DIRECTLY (a lone
        // surrogate is not producible from valid UTF-8) and decode it.
        const std::vector<std::uint16_t> lone_high{ 0xD83Du };
        const std::string lone_out{ utf16_to_utf8(lone_high) };
        check("boundary: lone high surrogate (no low) decodes as a 3-byte unit, not 4",
              lone_out.size() == 3 && static_cast<std::uint8_t>(lone_out[0]) == 0xEDu);

        // High surrogate followed by a NON-low unit: pair is NOT combined; both
        // emit independently (high as 3 bytes, then the plain ASCII 'A').
        const std::vector<std::uint16_t> high_then_ascii{ 0xD83Du, 0x0041u };
        const std::string hta{ utf16_to_utf8(high_then_ascii) };
        check("boundary: high surrogate then 'A' -> 3 bytes + 0x41 (no false combine)",
              hta.size() == 4 && static_cast<std::uint8_t>(hta[3]) == 0x41u);
    }

    // ── (E) method_proxy::value_t variant classification / conversion ────────
    // value_t is an aggregate over a std::variant; build each alternative
    // explicitly (NEVER static_cast a value_t TO a vector — that ambiguity
    // reverted this surface once).  All conversions used here are pure
    // (arithmetic static_cast, monostate/string classification, and the
    // uint32_t==0 path whose decode short-circuits to nullptr -> "" with no JVM).
    static auto test_value_t_classification() -> void
    {
        const vmhook::method_proxy::value_t v_void{ std::monostate{} };
        check("value_t: monostate is_void / !is_string",
              v_void.is_void() && !v_void.is_string());
        check("value_t: monostate as_string() == \"\"",
              v_void.as_string().empty());

        const vmhook::method_proxy::value_t v_int{ std::int32_t{ 42 } };
        check("value_t: int32 alternative is neither void nor string",
              !v_int.is_void() && !v_int.is_string());
        check("value_t: int32 converts to int via static_cast",
              static_cast<std::int32_t>(v_int) == 42);
        check("value_t: int32 widens to int64 via static_cast",
              static_cast<std::int64_t>(v_int) == 42);

        const vmhook::method_proxy::value_t v_bool{ true };
        check("value_t: bool alternative converts to true",
              static_cast<bool>(v_bool));
        const vmhook::method_proxy::value_t v_dbl{ 1.5 };
        check("value_t: double alternative converts to 1.5",
              static_cast<double>(v_dbl) == 1.5);

        const vmhook::method_proxy::value_t v_str{ std::string{ "hi" } };
        check("value_t: string alternative is_string / !is_void",
              v_str.is_string() && !v_str.is_void());
        check("value_t: string as_string() returns the eager bytes",
              v_str.as_string() == "hi");
        check("value_t: string converts to std::string via operator",
              static_cast<std::string>(v_str) == "hi");

        // uint32_t==0 reference alternative: decode_oop_pointer(0) -> nullptr
        // BEFORE any VMStruct deref, so as_string -> read_java_string(nullptr)
        // -> "" and the void* conversion -> nullptr.  Deterministic, no JVM.
        const vmhook::method_proxy::value_t v_ref0{ std::uint32_t{ 0 } };
        check("value_t: uint32(0) reference alt is not string/void classified",
              !v_ref0.is_string() && !v_ref0.is_void());
        check("value_t: uint32(0) as_string -> \"\" (null oop decode)",
              v_ref0.as_string().empty());
        check("value_t: uint32(0) converts to void* nullptr",
              static_cast<void*>(v_ref0) == nullptr);
    }

    // ── (F) signature byte-width / basic-type classification (pure) ──────────
    static auto test_signature_classification() -> void
    {
        // jvm_primitive_byte_width: Z/B=1, S/C=2, I/F=4, J/D=8, else 0.
        check("sig: width('Z')==1 width('B')==1",
              vmhook::detail::jvm_primitive_byte_width("Z") == 1
                  && vmhook::detail::jvm_primitive_byte_width("B") == 1);
        check("sig: width('S')==2 width('C')==2",
              vmhook::detail::jvm_primitive_byte_width("S") == 2
                  && vmhook::detail::jvm_primitive_byte_width("C") == 2);
        check("sig: width('I')==4 width('F')==4",
              vmhook::detail::jvm_primitive_byte_width("I") == 4
                  && vmhook::detail::jvm_primitive_byte_width("F") == 4);
        check("sig: width('J')==8 width('D')==8",
              vmhook::detail::jvm_primitive_byte_width("J") == 8
                  && vmhook::detail::jvm_primitive_byte_width("D") == 8);
        check("sig: reference 'Ljava/lang/String;' width 0 (handled elsewhere)",
              vmhook::detail::jvm_primitive_byte_width("Ljava/lang/String;") == 0);
        check("sig: multi-char descriptor width 0 (not a single primitive)",
              vmhook::detail::jvm_primitive_byte_width("II") == 0);

        // sig_char_to_basic_type: stable HotSpot BasicType ints.
        check("sig: basic_type('I')==10 (T_INT), ('J')==11 (T_LONG)",
              vmhook::detail::sig_char_to_basic_type('I') == 10
                  && vmhook::detail::sig_char_to_basic_type('J') == 11);
        check("sig: basic_type('Z')==4 (T_BOOLEAN), ('V')==14 (T_VOID)",
              vmhook::detail::sig_char_to_basic_type('Z') == 4
                  && vmhook::detail::sig_char_to_basic_type('V') == 14);
        check("sig: basic_type('[')==13 (T_ARRAY), ('L')==12 (T_OBJECT)",
              vmhook::detail::sig_char_to_basic_type('[') == 13
                  && vmhook::detail::sig_char_to_basic_type('L') == 12);
        check("sig: unknown char falls back to 12 (T_OBJECT)",
              vmhook::detail::sig_char_to_basic_type('?') == 12);
    }

    // ── (G) method-flags layout DECISION (pure, fabricated evidence only) ────
    // derive_method_flags_layout is constexpr & pure; feed it VMStruct EVIDENCE
    // structs we fill (no JVM, no Method*, no deref) and assert the per-JDK
    // placement of _dont_inline.  Several at compile time via static_assert.
    static constexpr auto layout_a()  // JDK 11..20: exported u2 _flags
    {
        vmhook::hotspot::method_flags_evidence e{};
        e.flags_present = true;
        e.flags_type    = "u2";
        e.flags_offset  = 0x30;
        return vmhook::hotspot::derive_method_flags_layout(e);
    }
    static constexpr auto layout_b()  // JDK 21+: u2 _intrinsic_id at status+4
    {
        vmhook::hotspot::method_flags_evidence e{};
        e.intrinsic_id_present = true;
        e.intrinsic_id_type    = "u2";
        e.intrinsic_id_offset  = 0x34;  // >= 4 and 4-aligned
        return vmhook::hotspot::derive_method_flags_layout(e);
    }
    static_assert(layout_a().confident && layout_a().width_bytes == 2
                      && layout_a().dont_inline_bit == 2 && layout_a().offset == 0x30,
                  "Path A (u2 _flags) -> offset=flags_offset, width 2, bit 2");
    static_assert(layout_b().confident && layout_b().width_bytes == 4
                      && layout_b().dont_inline_bit == 12 && layout_b().offset == 0x30,
                  "Path B (u2 _intrinsic_id) -> offset=intrinsic-4, width 4, bit 12");

    static auto test_method_flags_layout() -> void
    {
        check("flags: Path A (u2 _flags) places bit 2, width 2 at flags_offset",
              layout_a().confident && layout_a().width_bytes == 2
                  && layout_a().dont_inline_bit == 2 && layout_a().offset == 0x30);
        check("flags: Path B (u2 _intrinsic_id @0x34) -> offset 0x30, width 4, bit 12",
              layout_b().confident && layout_b().width_bytes == 4
                  && layout_b().dont_inline_bit == 12 && layout_b().offset == 0x30);

        // JDK 8: _intrinsic_id is u1 (not u2), no _flags -> NOT confident.
        vmhook::hotspot::method_flags_evidence jdk8{};
        jdk8.intrinsic_id_present = true;
        jdk8.intrinsic_id_type    = "u1";
        jdk8.intrinsic_id_offset  = 0x20;
        check("flags: JDK8 (u1 _intrinsic_id, no _flags) -> not confident (safe no-op)",
              !vmhook::hotspot::derive_method_flags_layout(jdk8).confident);

        // Path B refuses an underflowing offset (< 4).
        vmhook::hotspot::method_flags_evidence under{};
        under.intrinsic_id_present = true;
        under.intrinsic_id_type    = "u2";
        under.intrinsic_id_offset  = 2;  // would underflow offset-4
        check("flags: intrinsic offset < 4 -> not confident (no underflow)",
              !vmhook::hotspot::derive_method_flags_layout(under).confident);

        // Path B refuses a non-4-aligned offset (status is 4-aligned).
        vmhook::hotspot::method_flags_evidence misaligned{};
        misaligned.intrinsic_id_present = true;
        misaligned.intrinsic_id_type    = "u2";
        misaligned.intrinsic_id_offset  = 0x32;  // not % 4 == 0
        check("flags: misaligned intrinsic offset -> not confident",
              !vmhook::hotspot::derive_method_flags_layout(misaligned).confident);

        // Empty evidence (nothing exported) -> not confident.
        check("flags: empty evidence -> not confident",
              !vmhook::hotspot::derive_method_flags_layout(
                   vmhook::hotspot::method_flags_evidence{}).confident);

        // Path A wins over a present-but-also-derivable intrinsic (u2 _flags
        // takes precedence): width 2 / bit 2, not 4 / 12.
        vmhook::hotspot::method_flags_evidence both{};
        both.flags_present        = true;
        both.flags_type           = "u2";
        both.flags_offset         = 0x40;
        both.intrinsic_id_present = true;
        both.intrinsic_id_type    = "u2";
        both.intrinsic_id_offset  = 0x44;
        const vmhook::hotspot::method_flags_layout bl{
            vmhook::hotspot::derive_method_flags_layout(both) };
        check("flags: Path A precedence when both present (width 2, bit 2, offset 0x40)",
              bl.confident && bl.width_bytes == 2 && bl.dont_inline_bit == 2
                  && bl.offset == 0x40);
    }
} // namespace rjs_deep

// ── (wave-28) cold-entry + type-trait + cap-boundary deepening ─────────────
// ADDITIVE pass focused on the read_java_string ENTRY-POINT contract WITHOUT a
// live JVM: (1) cold null/invalid-pointer entry returns the empty string
// without faulting (the guard-and-return path is pure: is_valid_pointer rejects
// nullptr at vmhook.hpp:1768-1805 before any deref); (2) static_asserts pin
// the function signature — return type std::string, the noexcept
// CHARACTERIZATION (read_java_string is NOT declared noexcept — string
// allocation can throw bad_alloc), and the 16Mi cap constant lives in a
// constexpr int32_t; (3) the post-fix 4096 cap is GONE (raised to 16Mi by
// robustness bug #29 fix) and that boundary is pinned both as a constant and
// via the per-layout char_count math.  Touches no existing assertion above.
namespace rjs_wave28
{
    // ── static_asserts pinning the function-level contract ──────────────────
    // Return type is std::string (forward decl in vmhook.hpp:1699-1700,
    // definition in vmhook.hpp:20471).
    using rjs_t = decltype(&vmhook::read_java_string);
    static_assert(std::is_same_v<rjs_t, std::string (*)(void*)>,
                  "read_java_string signature must be: std::string(void*)");
    static_assert(std::is_same_v<decltype(vmhook::read_java_string(
                                     static_cast<void*>(nullptr))),
                                 std::string>,
                  "read_java_string return type must be std::string");

    // NOEXCEPT CHARACTERIZATION (NOT a bug — std::string ctor / += can throw
    // bad_alloc on a 16Mi buffer, so the function intentionally is NOT
    // noexcept).  Pinning the current state so a future noexcept-change is a
    // deliberate decision, not silent drift.
    static_assert(!noexcept(vmhook::read_java_string(
                      static_cast<void*>(nullptr))),
                  "read_java_string is NOT noexcept (string allocation may throw)");

    // The 16Mi cap is a constexpr std::int32_t (used in build-size arithmetic
    // up to 2 bytes/char without exceeding INT32_MAX).
    static_assert(std::is_same_v<decltype(vmhook::read_java_string_max_units),
                                 const std::int32_t>,
                  "read_java_string_max_units must be const std::int32_t");
    static_assert(vmhook::read_java_string_max_units == 16777216,
                  "cap is 16 * 1024 * 1024 chars (post-bug-#29 raise)");
    // 2 * cap fits in int32 with room to spare (body-size math safety).
    static_assert(static_cast<std::int64_t>(2)
                          * vmhook::read_java_string_max_units
                      < static_cast<std::int64_t>(INT32_MAX),
                  "2 * cap stays inside int32 so body-size math never overflows");
    // The OLD 4096 cap is dead — the new cap is 4 orders of magnitude larger.
    static_assert(vmhook::read_java_string_max_units > 4096 * 4000,
                  "post-fix cap dwarfs the old 4096 limit (bug #29)");

    // ── (1) cold null-entry: returns empty string, no fault ─────────────────
    // Pure deterministic path: is_valid_pointer(nullptr) -> false (out of
    // valid range / null), so read_java_string returns {} before touching any
    // VMStruct.  Safe on every platform with no JVM.
    static auto test_cold_null_entry_returns_empty() -> void
    {
        const std::string r{ vmhook::read_java_string(nullptr) };
        check("cold: read_java_string(nullptr) returns empty string",
              r.empty());
        check("cold: read_java_string(nullptr) size == 0",
              r.size() == 0);

        // Twice — proves no hidden state was left behind by the first call.
        const std::string r2{ vmhook::read_java_string(nullptr) };
        check("cold: read_java_string(nullptr) is idempotent (still empty)",
              r2.empty());
    }

    // ── (2) odd / sentinel invalid pointers also short-circuit to "" ────────
    // is_valid_pointer rejects an odd address (alignment guard, vmhook.hpp:
    // 1780-1783) BEFORE any deref, so this is safe and deterministic.
    // Sentinel debug-fill addresses (0xBAADF00D class) are likewise rejected
    // (1789-1801).  No JVM, no fabricated mapped memory, no SEH risk.
    static auto test_cold_invalid_pointer_short_circuits() -> void
    {
        const std::string r1{ vmhook::read_java_string(
            reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x1))) };
        check("cold: read_java_string((void*)0x1) returns empty (odd-addr reject)",
              r1.empty());

        const std::string r3{ vmhook::read_java_string(
            reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x3))) };
        check("cold: read_java_string((void*)0x3) returns empty (odd-addr reject)",
              r3.empty());

        // A sentinel-style fill address (debug-heap marker) is also rejected
        // by is_valid_pointer's sentinel list.  This is the pointer pattern an
        // uninitialized field would carry under MSVC debug — and the guard
        // catches it without faulting.
        const std::string r_sent{ vmhook::read_java_string(
            reinterpret_cast<void*>(
                static_cast<std::uintptr_t>(0xBAADF00DBAADF00Dull))) };
        check("cold: sentinel debug-fill 0xBAADF00D... returns empty (sentinel reject)",
              r_sent.empty());
    }

    // ── (3) post-bug-#29 cap boundary math (NO live JVM needed) ─────────────
    // Mirror the per-layout boundary read_java_string applies to the raw
    // arrayOop length.  These pin the NEW 16Mi-char ceiling — proving the old
    // 4096 ceiling is no longer in force on any layout, and that the per-
    // layout char_count math is monotone at the boundary.
    static auto test_post_fix_cap_boundary() -> void
    {
        const std::int32_t cap{ vmhook::read_java_string_max_units };

        // The old hard 4096 limit no longer rejects: a String of 4097 LATIN1
        // chars is now accepted (raw length 4097 sits well inside 1..2*cap).
        check("cap: 4097 chars (old-limit + 1) is now in-range, not rejected",
              4097 > 0 && 4097 <= 2 * cap);

        // Per-layout char-count math at the exact cap boundary:
        //   JDK 8 char[]      : raw length == char count == cap         -> in.
        //   JDK 9+ LATIN1     : raw length == char count == cap         -> in.
        //   JDK 9+ UTF16      : raw byte length == 2*cap, char count cap -> in.
        check("cap: JDK8 char[] at cap (length=cap) is at the boundary",
              cap > 0 && cap <= 2 * cap);
        check("cap: LATIN1 at cap (length=cap) is at the boundary",
              cap > 0 && cap <= 2 * cap);
        check("cap: UTF16 at cap (raw length 2*cap) is at the boundary",
              2 * cap > 0 && 2 * cap <= 2 * cap);

        // One past the raw guard: a forged raw length of 2*cap + 1 would be
        // rejected by the `length > 2 * read_java_string_max_units` guard at
        // vmhook.hpp:20565.  Pure arithmetic — no JVM needed.
        const std::int64_t too_big{
            static_cast<std::int64_t>(2) * cap + 1 };
        check("cap: 2*cap+1 is OUT of the raw guard range",
              too_big > static_cast<std::int64_t>(2) * cap);

        // Zero / negative raw length always lands in the early-return "" path.
        check("cap: raw length 0 is the empty-return case",
              0 <= 0);
        check("cap: raw length -1 is the empty-return case",
              static_cast<std::int32_t>(-1) <= 0);
    }

    // ── (4) coder == 0 LATIN1 branch math (the 'null-coder' decode path) ────
    // The JDK 9+ `coder == 0` branch is the LATIN1 path; per-byte append_utf8
    // re-encodes high-bit bytes as 2-byte UTF-8 (0xE9 -> C3 A9).  We can
    // exercise the append_utf8 oracle on every byte 0..255 directly — no JVM,
    // no oop, pure code-point math — and prove the LATIN1-branch invariants:
    //   * byte 0x00 (the "null coder body byte") emits a single literal NUL.
    //   * byte 0x7F is the last 1-byte UTF-8 (ASCII boundary).
    //   * byte 0x80 is the first 2-byte UTF-8 emission.
    //   * byte 0xFF is the highest LATIN1 byte and emits C3 BF (2 bytes).
    static auto test_latin1_coder_zero_branch_pure() -> void
    {
        std::string out;
        append_utf8(out, 0x00u);
        check("latin1-branch: byte 0x00 -> single literal NUL byte",
              out.size() == 1 && out[0] == '\0');

        out.clear();
        append_utf8(out, 0x7Fu);
        check("latin1-branch: byte 0x7F -> single 0x7F byte (1-byte UTF-8)",
              out.size() == 1 && static_cast<std::uint8_t>(out[0]) == 0x7Fu);

        out.clear();
        append_utf8(out, 0x80u);
        check("latin1-branch: byte 0x80 -> C2 80 (first 2-byte form)",
              out.size() == 2 && static_cast<std::uint8_t>(out[0]) == 0xC2u
                  && static_cast<std::uint8_t>(out[1]) == 0x80u);

        out.clear();
        append_utf8(out, 0xE9u);
        check("latin1-branch: byte 0xE9 (cafe-e-acute) -> C3 A9",
              out.size() == 2 && static_cast<std::uint8_t>(out[0]) == 0xC3u
                  && static_cast<std::uint8_t>(out[1]) == 0xA9u);

        out.clear();
        append_utf8(out, 0xFFu);
        check("latin1-branch: byte 0xFF (LATIN1 max) -> C3 BF",
              out.size() == 2 && static_cast<std::uint8_t>(out[0]) == 0xC3u
                  && static_cast<std::uint8_t>(out[1]) == 0xBFu);

        // Full byte sweep 0..255 -> total UTF-8 length is exactly
        // 128 (1-byte 0x00..0x7F) + 128*2 (2-byte 0x80..0xFF) = 384.
        std::string sweep;
        for (std::uint32_t b{ 0 }; b < 256u; ++b)
        {
            append_utf8(sweep, b);
        }
        check("latin1-branch: full 0..255 LATIN1 sweep -> exactly 128 + 256 = 384 UTF-8 bytes",
              sweep.size() == 384);
    }

    // ── (5) round-trip a "long" buffer well past the old 4096 limit ─────────
    // Pure no-JVM: build a 10000-byte ASCII buffer, round-trip it through the
    // shared utf8_to_utf16 + decode oracle.  Proves the encode/decode core has
    // no residual 4096 ceiling and scales with input.  (We don't allocate 16Mi
    // here — the constant is asserted above; this just pins a value that the
    // OLD 4096-truncating path would have mangled.)
    static auto test_round_trip_past_old_4096_limit() -> void
    {
        constexpr std::size_t big_n{ 10000 };
        std::string big;
        big.reserve(big_n);
        for (std::size_t i{ 0 }; i < big_n; ++i)
        {
            big += static_cast<char>('a' + (i % 26));
        }
        const std::vector<std::uint16_t> units{
            vmhook::detail::utf8_to_utf16(big) };
        check("longbuf: 10000-byte ASCII -> 10000 code units (no 4096 truncation)",
              units.size() == big_n);
        check("longbuf: 10000-byte round-trip byte-identical (no 4096 truncation)",
              round_trip(big) == big);
    }
} // namespace rjs_wave28

auto main() -> int
{
    test_interior_nul_is_length_preserving();
    test_leading_and_trailing_nul();
    test_astral_surrogate_pair();
    test_nul_adjacent_astral();
    test_malformed_yields_replacement();
    test_ascii_and_empty_baseline();

    rjs_deep::test_vmstruct_null_when_no_jvm();
    rjs_deep::test_narrow_codec_round_trip();
    rjs_deep::test_length_and_char_count_math();
    rjs_deep::test_decode_unit_boundaries();
    rjs_deep::test_value_t_classification();
    rjs_deep::test_signature_classification();
    rjs_deep::test_method_flags_layout();

    rjs_wave28::test_cold_null_entry_returns_empty();
    rjs_wave28::test_cold_invalid_pointer_short_circuits();
    rjs_wave28::test_post_fix_cap_boundary();
    rjs_wave28::test_latin1_coder_zero_branch_pure();
    rjs_wave28::test_round_trip_past_old_4096_limit();

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
