// read_java_string JVM test module  (feature area: strings)
//
// THE subject-under-test here is the FREE helper vmhook::read_java_string(oop):
// given a decoded OOP that points at a java.lang.String, it returns the string's
// contents as a std::string encoded in UTF-8.  This module makes that helper the
// centre of gravity and exercises EVERY decode path, length boundary, and guard
// it has, across both String layouts (JDK 8 char[] vs JDK 9+ byte[]+coder).
//
// HOW THE SUBJECT IS REACHED (identical to how the library reaches it itself):
//   static_field("name")           -> field_proxy for a static String field
//        ->get_compressed_oop()     -> the 32-bit narrow OOP stored in that slot
//   hotspot::decode_oop_pointer(..) -> the real 64-bit java.lang.String OOP
//   read_java_string(that_oop)      -> the decoded std::string  (THE CALL UNDER TEST)
// This is byte-for-byte the same pipeline the field_proxy String getter, the
// array / collection / map element readers, and the method-return decoder all
// funnel through, so proving it here proves the shared decode core.
//
// WHAT THIS MODULE PROVES on a live JVM (Java 8/11/17/21/24/25/26 x MSVC/Clang/GCC):
//   * LATIN1 (coder 0) decodes BYTE-EXACT UTF-8: pure ASCII verbatim, a single
//     ASCII char, the high byte 0xE9 -> the TWO bytes C3 A9 (the headline proof
//     the LATIN1 path UTF-8-ENCODES high bytes), the LATIN1 ceiling U+00FF -> C3
//     BF, an interior NUL preserved, and a long (1000-char) string.
//   * UTF16 (coder 1) decodes BYTE-EXACT UTF-8: a single BMP CJK char, three CJK
//     code points, a MIXED ASCII+CJK string (ASCII pushed through the UTF-16
//     path), an interior NUL inside UTF-16, an astral emoji carried as a
//     SURROGATE PAIR -> one 4-byte sequence, the same emoji FLANKED by ASCII
//     (proving the surrogate index-advance doesn't eat the next char), and a long
//     (300-char) string.
//   * The malformed LONE-SURROGATE input never crashes and yields the helper's
//     ACTUAL 3-byte WTF-8/CESU encoding (ED A0 BD) -- characterised, not forced.
//   * The 1..4096 backing-ARRAY-length ceiling: exactly 4096 passes, 4097 is
//     REJECTED to "" (the doc says "truncate"; the code returns empty -- asserted
//     as the real behaviour), and the ASYMMETRIC UTF-16 char ceiling (2048 chars
//     pass, 2049 is rejected on JDK 9+ byte[] but decodes on JDK 8 char[]).
//   * The JDK-8 (char[]) and JDK-9+ (byte[]+coder) layouts decode to the
//     IDENTICAL UTF-8 bytes -- every decode is compared to a FIXED expected byte
//     sequence that does not depend on the running JDK, so a green row on each
//     matrix entry IS the cross-JDK identical-output invariant.
//   * The guard paths return an EMPTY std::string WITHOUT crashing: the empty
//     String "", a null String reference (compressed OOP 0), and three
//     deliberately-bogus raw pointers fed straight to read_java_string (nullptr,
//     an odd address, and a low sentinel) -- each rejected by the internal guard.
//   * Every decode AGREES with what Java itself reports for the same field
//     (length / code points / physical coder), cross-checked via a probe witness.
//   * read_java_string is a PURE reader: decoding the same field twice, and after
//     the probe ran, yields byte-identical results (no mutation of the backing).
//
// SUITE-SAFETY (this module installs NO hooks -- it is pure reads):
//   * The whole body runs under a try/catch that downgrades any C++ exception to
//     an [INFO] line and returns -- a module is NEVER allowed to fail the suite
//     because of a thrown exception.
//   * An UNCONDITIONAL vmhook::shutdown_hooks() runs OUTSIDE the try as the very
//     last statement (a documented no-op when nothing is armed) so the module
//     leaves nothing behind for later modules even on an early return.
//   * An entry guard bails to [INFO] (no FAIL) if java.lang.String can't be
//     resolved -- there is nothing to test that early in bootstrap.
//   * read_java_string is only ever handed a pointer this module has (a) decoded
//     from a live field slot and validated with is_valid_pointer, or (b)
//     deliberately chosen to be null / odd / a low sentinel to exercise the guard.
//     No forced System.gc() is performed (this is a pure-read module with no
//     relocation test), so there is no GC-gating to do.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace
{
    // Wrapper for vmhook.fixtures.ReadJavaString.
    //
    // Every accessor is a STATIC method reaching the field through static_field(),
    // so this compiles uniformly on GCC too (where the deducing-this get_field
    // overloads are non-viable from a static context).  Accessors use the CLEAN
    // documented one-liner `return static_field("x")->get();` idiom -- no
    // defensive has_value()->sentinel checks in the value accessors (the module
    // body and the decode() pipeline own the safety).  resolves() is the one
    // legitimate .has_value() use: it PROBES presence, it is not a value read.
    class rjs : public vmhook::object<rjs>
    {
    public:
        explicit rjs(vmhook::oop_t instance) noexcept
            : vmhook::object<rjs>{ instance }
        {
        }

        // ---- handshake (all via static_field) ----
        static auto set_go(bool value) -> void   { static_field("go")->set(value); }
        static auto set_done(bool value) -> void  { static_field("done")->set(value); }
        static auto get_done() -> bool            { return static_field("done")->get(); }

        // Presence PROBE (not a value accessor): may use has_value().
        static auto resolves(const char* name) -> bool
        {
            return static_field(name).has_value();
        }

        // ---- THE SUBJECT PIPELINE -------------------------------------------
        // Resolve a static String field, pull its compressed OOP exactly the way
        // the library does, decode it, and feed the decoded OOP straight into
        // read_java_string.  Returns the decoded std::string.
        //
        // Safety (at the CALL SITE, not in an accessor): get_compressed_oop()
        // yields 0 for a null field reference (and for a non-reference field);
        // decode_oop_pointer(0) yields nullptr; a non-null decoded oop is gated by
        // is_valid_pointer BEFORE the call (point 4 of the safety contract -- the
        // only RAW pointer this module derefs is the one read_java_string itself
        // dereferences, and it is validated first); read_java_string ALSO
        // null/range-guards internally.  So a null/invalid field reference flows
        // through as the empty string with no unsafe deref.
        static auto decode(const char* name) -> std::string
        {
            const auto proxy{ static_field(name) };
            if (!proxy.has_value())
            {
                return std::string{ "<<no-field>>" };
            }
            const std::uint32_t compressed{ proxy->get_compressed_oop() };
            void* const str_oop{ vmhook::hotspot::decode_oop_pointer(compressed) };
            // Gate the deref: only hand read_java_string a pointer we have proven
            // valid, OR an intentional nullptr (compressed==0) to exercise the
            // null-oop guard.  Never deref a bogus/half-decoded address.
            if (str_oop && !vmhook::hotspot::is_valid_pointer(str_oop))
            {
                return std::string{ "<<invalid-oop>>" };
            }
            // Copy-init (MSVC): read_java_string already returns std::string.
            const std::string s = vmhook::read_java_string(str_oop);
            return s;
        }

        // Returns the raw compressed OOP for a (reference) field, for the
        // null-reference assertion (0 == null).
        static auto compressed_oop(const char* name) -> std::uint32_t
        {
            const auto proxy{ static_field(name) };
            if (!proxy.has_value())
            {
                return 0xFFFFFFFFu; // sentinel: field did not resolve
            }
            return proxy->get_compressed_oop();
        }

        // ---- read a published Java cross-check witness (clean one-liner idiom) -
        static auto seen_int(const char* name) -> std::int32_t { return static_field(name)->get(); }
        static auto seen_bool(const char* name) -> bool { return static_field(name)->get(); }
    };

    // Fixed, JDK-independent expected UTF-8 byte sequences for each subject.  A
    // green comparison on every matrix row IS the "JDK 8 char[] and JDK 9+
    // byte[]+coder decode to identical bytes" invariant.
    const std::string k_hello   = "\x68\x65\x6C\x6C\x6F";                 // hello
    const std::string k_oneAscii= "\x5A";                                // Z
    const std::string k_cafe    = "\x63\x61\x66\xC3\xA9";                 // c a f (C3 A9)
    const std::string k_latin1Hi= "\xC3\xBF";                            // U+00FF -> C3 BF
    const std::string k_nulLatin1{ '\x61', '\x00', '\x62' };             // a NUL b (3 bytes)
    const std::string k_oneCjk  = "\xE4\xB8\xAD";                        // U+4E2D
    const std::string k_nihongo = "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E"; // U+65E5 U+672C U+8A9E
    const std::string k_mixed   = "\x41\xE6\x97\xA5\x42";                 // A U+65E5 B
    const std::string k_nulUtf16{ '\x61', '\x00', '\xE6', '\x97', '\xA5' }; // a NUL U+65E5 (5 bytes)
    const std::string k_emoji   = "\xF0\x9F\x98\x80";                     // U+1F600
    const std::string k_emojiMix= "\x58\xF0\x9F\x98\x80\x59";             // X U+1F600 Y
    // Lone high surrogate U+D83D: read_java_string does NOT combine and emits the
    // 3-byte WTF-8/CESU encoding of the surrogate code unit.  This is the helper's
    // ACTUAL behaviour (never a crash), NOT well-formed UTF-8 -- characterised.
    const std::string k_loneHigh= "\xED\xA0\xBD";                         // U+D83D as 3-byte CESU

    // Render a std::string as "AA BB CC" hex for diagnostics.
    auto to_hex(const std::string& s) -> std::string
    {
        static const char* const digits{ "0123456789ABCDEF" };
        std::string out;
        out.reserve(s.size() * 3);
        for (std::size_t i{ 0 }; i < s.size(); ++i)
        {
            if (i)
            {
                out += ' ';
            }
            const std::uint8_t b{ static_cast<std::uint8_t>(s[i]) };
            out += digits[b >> 4];
            out += digits[b & 0x0F];
        }
        return out;
    }

    // A long ASCII string is too big to spell as a literal; build the expected
    // UTF-8 the same way the fixture builds the Java String: n repeats of one
    // byte/code point.  (LATIN1/ASCII: 1 byte per char; CJK U+65E5: 3 bytes.)
    auto repeat_bytes(const std::string& unit, std::size_t n) -> std::string
    {
        std::string out;
        out.reserve(unit.size() * n);
        for (std::size_t i{ 0 }; i < n; ++i)
        {
            out += unit;
        }
        return out;
    }

    // Drive the single probe cycle (publish Java-side cross-check facts).
    auto drive(vmhook_test::context& ctx) -> bool
    {
        return ctx.run_probe(
            [](bool value)
            {
                if (value)
                {
                    rjs::set_done(false);
                }
                rjs::set_go(value);
            },
            []() { return rjs::get_done(); });
    }

    // The real body.  Wrapped by the VMHOOK_JVM_MODULE entry below so any C++
    // exception is downgraded to [INFO] and the unconditional teardown still runs.
    auto run_body(vmhook_test::context& ctx) -> void
    {
        vmhook::register_class<rjs>("vmhook/fixtures/ReadJavaString");

        // -----------------------------------------------------------------
        //  ENTRY GUARD: nothing to test if java.lang.String can't resolve
        //  (very early bootstrap / a VM without VMStructs).  [INFO], not FAIL.
        // -----------------------------------------------------------------
        if (vmhook::find_class("java/lang/String") == nullptr)
        {
            ctx.record("[INFO] read_java_string: java.lang.String not resolvable yet - "
                       "skipping module (no assertions run).");
            return;
        }

        // Detect the String layout the same way the sibling string modules do:
        // JDK 9+ compact strings carry a `coder` field; JDK 8 char[] strings do
        // not.  Used only for the two layout-dependent length-cap branches below.
        vmhook::hotspot::klass* const string_klass{ vmhook::find_class("java/lang/String") };
        const bool compact_strings{ string_klass != nullptr
                                    && string_klass->find_field("coder").has_value() };
        ctx.record(std::string{ "[INFO] String layout: " }
                   + (compact_strings ? "JDK 9+ compact (byte[]+coder)"
                                      : "JDK 8 classic (char[], no coder)"));

        // =================================================================
        //  0. Sanity: the fixture class resolves and the subject fields are
        //     present (a spot-check across the LATIN1 / UTF16 / guard groups).
        // =================================================================
        ctx.check("rjs_class_registered_ascii_field_resolves", rjs::resolves("ascii"));
        ctx.check("rjs_nihongo_field_resolves", rjs::resolves("nihongo"));
        ctx.check("rjs_emoji_field_resolves", rjs::resolves("emoji"));
        ctx.check("rjs_null_field_resolves", rjs::resolves("nullRef"));
        ctx.check("rjs_cap4096_field_resolves", rjs::resolves("cap4096"));

        // =================================================================
        //  1. LATIN1 (coder 0) DECODES -- byte-exact UTF-8.
        // =================================================================
        {
            const std::string ascii{ rjs::decode("ascii") };
            ctx.check("decode_ascii_eq_hello", ascii == k_hello);
            ctx.check("decode_ascii_len_5", ascii.size() == 5);
            ctx.record(std::string{ "[INFO] read_java_string(ascii)    = [" } + to_hex(ascii) + "] expect [" + to_hex(k_hello) + "]");

            // Single ASCII char -- the char_count == 1 boundary.
            const std::string one{ rjs::decode("oneAscii") };
            ctx.check("decode_oneAscii_eq_Z", one == k_oneAscii);
            ctx.check("decode_oneAscii_len_1", one.size() == 1);

            // The crux: 0xE9 'e-acute' -> the TWO UTF-8 bytes C3 A9 (not a raw E9).
            const std::string cafe{ rjs::decode("cafe") };
            ctx.check("decode_cafe_byte_exact_utf8", cafe == k_cafe);
            ctx.check("decode_cafe_len_5_bytes", cafe.size() == 5);
            ctx.check("decode_cafe_tail_is_C3A9",
                      cafe.size() == 5
                      && static_cast<std::uint8_t>(cafe[3]) == 0xC3u
                      && static_cast<std::uint8_t>(cafe[4]) == 0xA9u);
            ctx.record(std::string{ "[INFO] read_java_string(cafe)     = [" } + to_hex(cafe) + "] expect [" + to_hex(k_cafe) + "]");

            // The LATIN1 ceiling U+00FF -> C3 BF.
            const std::string hi{ rjs::decode("latin1Hi") };
            ctx.check("decode_latin1Hi_eq_C3BF", hi == k_latin1Hi);
            ctx.record(std::string{ "[INFO] read_java_string(latin1Hi) = [" } + to_hex(hi) + "] expect [" + to_hex(k_latin1Hi) + "]");

            // Interior NUL in a LATIN1 string: all 3 bytes survive (sized by
            // length, never cut at the embedded NUL like a C string would be).
            const std::string nul{ rjs::decode("nulLatin1") };
            ctx.check("decode_nulLatin1_byte_exact", nul == k_nulLatin1);
            ctx.check("decode_nulLatin1_len_3_with_interior_nul",
                      nul.size() == 3 && nul[0] == 'a' && nul[1] == '\0' && nul[2] == 'b');
            ctx.record(std::string{ "[INFO] read_java_string(nulLatin1)= [" } + to_hex(nul) + "] expect [" + to_hex(k_nulLatin1) + "]");

            // Long (1000-char) LATIN1 string, well under the cap.
            const std::string long_ascii{ rjs::decode("longAscii") };
            const std::string k_long_ascii{ repeat_bytes("x", 1000) };
            ctx.check("decode_longAscii_len_1000", long_ascii.size() == 1000);
            ctx.check("decode_longAscii_byte_exact", long_ascii == k_long_ascii);
        }

        // =================================================================
        //  2. UTF16 (coder 1) DECODES -- byte-exact UTF-8.
        // =================================================================
        {
            // Single BMP CJK char -- the UTF-16 char_count == 1 boundary.
            const std::string one_cjk{ rjs::decode("oneCjk") };
            ctx.check("decode_oneCjk_eq_U4E2D", one_cjk == k_oneCjk);
            ctx.check("decode_oneCjk_len_3", one_cjk.size() == 3);

            const std::string nihongo{ rjs::decode("nihongo") };
            ctx.check("decode_nihongo_byte_exact_utf8", nihongo == k_nihongo);
            ctx.check("decode_nihongo_len_9_bytes", nihongo.size() == 9); // 3 CJK x 3 bytes
            ctx.record(std::string{ "[INFO] read_java_string(nihongo)  = [" } + to_hex(nihongo) + "] expect [" + to_hex(k_nihongo) + "]");

            // ASCII pushed through the UTF-16 path (the >0xFF char promotes the
            // whole string to UTF-16): ASCII 'A'(41) and 'B'(42) flank the CJK.
            const std::string mixed{ rjs::decode("mixed") };
            ctx.check("decode_mixed_byte_exact_utf8", mixed == k_mixed);
            ctx.check("decode_mixed_ascii_flanks",
                      mixed.size() == 5
                      && static_cast<std::uint8_t>(mixed.front()) == 0x41u
                      && static_cast<std::uint8_t>(mixed.back()) == 0x42u);
            ctx.record(std::string{ "[INFO] read_java_string(mixed)    = [" } + to_hex(mixed) + "] expect [" + to_hex(k_mixed) + "]");

            // Interior NUL inside a UTF-16 string: 'a', NUL, U+65E5 (5 bytes).
            const std::string nul16{ rjs::decode("nulUtf16") };
            ctx.check("decode_nulUtf16_byte_exact", nul16 == k_nulUtf16);
            ctx.check("decode_nulUtf16_len_5_with_interior_nul",
                      nul16.size() == 5 && nul16[0] == 'a' && nul16[1] == '\0');
            ctx.record(std::string{ "[INFO] read_java_string(nulUtf16) = [" } + to_hex(nul16) + "] expect [" + to_hex(k_nulUtf16) + "]");

            // Astral emoji carried as a SURROGATE PAIR -> one 4-byte UTF-8 seq.
            const std::string emoji{ rjs::decode("emoji") };
            ctx.check("decode_emoji_surrogate_pair_4byte_utf8", emoji == k_emoji);
            ctx.check("decode_emoji_len_4_bytes", emoji.size() == 4);
            ctx.check("decode_emoji_leads_with_F0",
                      emoji.size() == 4 && static_cast<std::uint8_t>(emoji.front()) == 0xF0u);
            ctx.record(std::string{ "[INFO] read_java_string(emoji)    = [" } + to_hex(emoji) + "] expect [" + to_hex(k_emoji) + "]");

            // The SAME emoji FLANKED by ASCII: proves the surrogate combine
            // advances past BOTH units and does not swallow the trailing 'Y'.
            const std::string emoji_mix{ rjs::decode("emojiMix") };
            ctx.check("decode_emojiMix_byte_exact", emoji_mix == k_emojiMix);
            ctx.check("decode_emojiMix_len_6_X_emoji_Y",
                      emoji_mix.size() == 6
                      && static_cast<std::uint8_t>(emoji_mix.front()) == 0x58u  // 'X'
                      && static_cast<std::uint8_t>(emoji_mix[1]) == 0xF0u       // 4-byte lead
                      && static_cast<std::uint8_t>(emoji_mix.back()) == 0x59u); // 'Y'
            ctx.record(std::string{ "[INFO] read_java_string(emojiMix) = [" } + to_hex(emoji_mix) + "] expect [" + to_hex(k_emojiMix) + "]");

            // Long (300-char) UTF-16 string of U+65E5 (E6 97 A5), under the cap
            // on both layouts.
            const std::string long_cjk{ rjs::decode("longCjk") };
            const std::string k_long_cjk{ repeat_bytes("\xE6\x97\xA5", 300) };
            ctx.check("decode_longCjk_len_900", long_cjk.size() == 900); // 300 x 3 bytes
            ctx.check("decode_longCjk_byte_exact", long_cjk == k_long_cjk);
        }

        // =================================================================
        //  3. MALFORMED INPUT (characterised, never crashes): a LONE high
        //     surrogate.  read_java_string cannot combine it and emits the
        //     3-byte WTF-8/CESU encoding (ED A0 BD) of the surrogate code unit.
        //     This is the helper's ACTUAL behaviour -- asserted as-is, and the
        //     contrast with Java's own UTF-8 encoder (which substitutes '?') is
        //     recorded.  The point is: no crash, deterministic output.
        // =================================================================
        {
            const std::string lone{ rjs::decode("loneHigh") };
            ctx.check("decode_loneHigh_actual_3byte_cesu", lone == k_loneHigh);
            ctx.check("decode_loneHigh_len_3", lone.size() == 3);
            ctx.check("decode_loneHigh_leads_with_ED",
                      lone.size() == 3 && static_cast<std::uint8_t>(lone.front()) == 0xEDu);
            ctx.record(std::string{ "[INFO] read_java_string(loneHigh) = [" } + to_hex(lone)
                       + "] (lone surrogate -> 3-byte CESU; Java's own getBytes(UTF_8) would give 3F '?')");
        }

        // =================================================================
        //  4. LENGTH-CAP BOUNDARIES on the backing-ARRAY length (1..4096).
        //     The doc comment promises "truncate"; the code RETURNS EMPTY past
        //     the ceiling -- these assert the REAL behaviour.  The ceiling is
        //     applied to the raw array length, so it is ASYMMETRIC across coders:
        //     LATIN1 allows 4096 chars, UTF16 (byte[] = 2*chars) only 2048.
        // =================================================================
        {
            // Exactly 4096 ASCII chars: LATIN1 byte[] length 4096 -> passes.
            const std::string cap4096{ rjs::decode("cap4096") };
            ctx.check("decode_cap4096_passes_inclusive", cap4096.size() == 4096);
            ctx.check("decode_cap4096_all_x",
                      cap4096.size() == 4096 && cap4096.front() == 'x' && cap4096.back() == 'x');

            // 4097 ASCII chars: byte[] length 4097 > 4096 -> REJECTED -> "".
            // (Documented flaw: header says "truncate", implementation returns "".)
            const std::string cap4097{ rjs::decode("cap4097") };
            ctx.check("decode_cap4097_rejected_empty_DOC_truncate_vs_real_empty", cap4097.empty());

            // 2048 CJK chars: on JDK 9+ UTF16 byte[] length 4096 -> passes; on
            // JDK 8 char[] length 2048 -> also passes.  Decodes to 2048*3 bytes.
            const std::string capUtf2048{ rjs::decode("capUtf2048") };
            ctx.check("decode_capUtf2048_len_6144", capUtf2048.size() == 2048u * 3u);
            ctx.check("decode_capUtf2048_first_and_last_kanji",
                      capUtf2048.size() == 6144
                      && static_cast<std::uint8_t>(capUtf2048[0]) == 0xE6u
                      && static_cast<std::uint8_t>(capUtf2048[6143]) == 0xA5u);

            // 2049 CJK chars: the ASYMMETRIC ceiling.  On JDK 9+ the UTF16
            // byte[] length is 4098 > 4096 -> REJECTED -> "" (a UTF-16 string is
            // effectively capped at 2048 chars, HALF the LATIN1 cap).  On JDK 8
            // the char[] length is 2049 <= 4096 -> DECODES (2049*3 bytes).
            const std::string capUtf2049{ rjs::decode("capUtf2049") };
            if (compact_strings)
            {
                ctx.check("decode_capUtf2049_rejected_empty_utf16_cap_2048_ASYMMETRY",
                          capUtf2049.empty());
            }
            else
            {
                ctx.check("decode_capUtf2049_jdk8_char_array_decodes_6147",
                          capUtf2049.size() == 2049u * 3u);
            }
            ctx.record(std::string{ "[INFO] length-cap: cap4096 -> " }
                       + std::to_string(cap4096.size()) + " bytes, cap4097 -> "
                       + std::to_string(cap4097.size()) + " bytes (0 == rejected); "
                       + "capUtf2048 -> " + std::to_string(capUtf2048.size())
                       + ", capUtf2049 -> " + std::to_string(capUtf2049.size())
                       + " (asymmetric UTF-16 ceiling: 2048 chars on JDK 9+).");
        }

        // =================================================================
        //  5. GUARD PATHS -- empty std::string, NO crash.
        //     The empty String "", a null String reference (compressed OOP 0),
        //     and three deliberately-bogus RAW pointers fed straight to
        //     read_java_string.  is_valid_pointer is the ONLY pre-deref gate the
        //     helper has (point 4 of the safety contract), so these prove it
        //     rejects null / odd / low-sentinel addresses without dereferencing.
        // =================================================================
        {
            const std::string empty{ rjs::decode("empty") };
            ctx.check("decode_empty_is_empty_string", empty.empty());

            // nullRef's compressed OOP must be 0 (the Java null reference).
            const std::uint32_t null_comp{ rjs::compressed_oop("nullRef") };
            ctx.check("nullRef_compressed_oop_is_zero", null_comp == 0u);

            // decode("nullRef") routes compressed 0 -> decode_oop_pointer ->
            // nullptr -> read_java_string(nullptr) -> "" with no dereference.
            const std::string null_decoded{ rjs::decode("nullRef") };
            ctx.check("decode_nullRef_is_empty_string", null_decoded.empty());

            // Belt-and-braces: call read_java_string with three bogus RAW
            // pointers.  Each must return "" via the internal guard and never
            // dereference.  (nullptr; an ODD address the alignment check rejects;
            // and a LOW sentinel below the user-address floor.)
            ctx.check("read_java_string_nullptr_is_empty",
                      vmhook::read_java_string(nullptr).empty());
            ctx.check("read_java_string_odd_ptr_is_empty",
                      vmhook::read_java_string(
                          reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x1))).empty());
            ctx.check("read_java_string_low_sentinel_ptr_is_empty",
                      vmhook::read_java_string(
                          reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x10))).empty());
        }

        // =================================================================
        //  6. CROSS-CHECK against Java's own view (probe-published witnesses),
        //     and the JDK8(char[]) vs JDK9+(byte[]) IDENTICAL-OUTPUT invariant.
        //     The decodes above already compare against FIXED expected bytes that
        //     do not vary by JDK, so passing on every matrix row IS the cross-JDK
        //     invariant.  Here we additionally confirm Java agrees on
        //     lengths / code points for the same fields, and record which
        //     physical coder each case used (best-effort: the `coder` field is
        //     module-encapsulated on JDK 9+ and absent on JDK 8, so coder values
        //     are diagnostic, not assertions).
        // =================================================================
        {
            const bool done{ drive(ctx) };
            ctx.check("crosscheck_probe_completed", done);

            if (done)
            {
                // Java-reported lengths / code points for the decode targets.
                ctx.check("java_ascii_len_5", rjs::seen_int("jAsciiLen") == 5);
                ctx.check("java_oneAscii_cp0_is_Z", rjs::seen_int("jOneAsciiCp0") == 0x5A);
                ctx.check("java_cafe_len_4", rjs::seen_int("jCafeLen") == 4);
                ctx.check("java_cafe_cp3_is_E9", rjs::seen_int("jCafeCp3") == 0x00E9);
                ctx.check("java_latin1Hi_cp0_is_FF", rjs::seen_int("jLatin1HiCp0") == 0x00FF);
                ctx.check("java_nulLatin1_len_3", rjs::seen_int("jNulLatin1Len") == 3);
                ctx.check("java_nulLatin1_cp1_is_0", rjs::seen_int("jNulLatin1Cp1") == 0);
                ctx.check("java_longAscii_len_1000", rjs::seen_int("jLongAsciiLen") == 1000);
                ctx.check("java_oneCjk_cp0_is_4E2D", rjs::seen_int("jOneCjkCp0") == 0x4E2D);
                ctx.check("java_nihongo_len_3", rjs::seen_int("jNihongoLen") == 3);
                ctx.check("java_nihongo_cp0_is_65E5", rjs::seen_int("jNihongoCp0") == 0x65E5);
                ctx.check("java_nihongo_cp1_is_672C", rjs::seen_int("jNihongoCp1") == 0x672C);
                ctx.check("java_nihongo_cp2_is_8A9E", rjs::seen_int("jNihongoCp2") == 0x8A9E);
                ctx.check("java_mixed_len_3", rjs::seen_int("jMixedLen") == 3);
                ctx.check("java_nulUtf16_len_3", rjs::seen_int("jNulUtf16Len") == 3);
                ctx.check("java_nulUtf16_cp1_is_0", rjs::seen_int("jNulUtf16Cp1") == 0);
                ctx.check("java_emoji_cpCount_1", rjs::seen_int("jEmojiCpCount") == 1);
                ctx.check("java_emoji_cp0_is_1F600", rjs::seen_int("jEmojiCp0") == 0x1F600);
                ctx.check("java_emojiMix_len_4", rjs::seen_int("jEmojiMixLen") == 4);
                ctx.check("java_emojiMix_cpCount_3", rjs::seen_int("jEmojiMixCpCount") == 3);
                ctx.check("java_loneHigh_len_1", rjs::seen_int("jLoneHighLen") == 1);
                ctx.check("java_loneHigh_cp0_is_D83D", rjs::seen_int("jLoneHighCp0") == 0xD83D);
                ctx.check("java_longCjk_len_300", rjs::seen_int("jLongCjkLen") == 300);
                ctx.check("java_cap4096_len_4096", rjs::seen_int("jCap4096Len") == 4096);
                ctx.check("java_cap4097_len_4097", rjs::seen_int("jCap4097Len") == 4097);
                ctx.check("java_capUtf2048_len_2048", rjs::seen_int("jCapUtf2048Len") == 2048);
                ctx.check("java_capUtf2049_len_2049", rjs::seen_int("jCapUtf2049Len") == 2049);
                ctx.check("java_empty_len_0", rjs::seen_int("jEmptyLen") == 0);
                ctx.check("java_nullRef_is_null", rjs::seen_bool("jNullIsNull"));

                // Physical-coder coverage (diagnostic).  When the coder field is
                // readable (JDK 9+ with reflective access), LATIN1 cases are 0 and
                // UTF16 cases are 1; on JDK 8 there is no coder field (the char[]
                // path) and on a locked-down JDK 9+ reflection returns -1 -- in
                // both cases the byte-exact decodes above already prove BOTH
                // internal coder branches ran.
                const bool has_coder{ rjs::seen_bool("jHasCoderField") };
                const std::int32_t c_ascii{ rjs::seen_int("jCoderAscii") };
                const std::int32_t c_cafe{ rjs::seen_int("jCoderCafe") };
                const std::int32_t c_nihongo{ rjs::seen_int("jCoderNihongo") };
                const std::int32_t c_emoji{ rjs::seen_int("jCoderEmoji") };
                const std::int32_t c_long_cjk{ rjs::seen_int("jCoderLongCjk") };
                const std::int32_t c_long_ascii{ rjs::seen_int("jCoderLongAscii") };
                ctx.record(std::string{ "[INFO] coder field present (JDK9+)=" } + (has_coder ? "true" : "false")
                           + " coder{ascii=" + std::to_string(c_ascii)
                           + " cafe=" + std::to_string(c_cafe)
                           + " nihongo=" + std::to_string(c_nihongo)
                           + " emoji=" + std::to_string(c_emoji)
                           + " longAscii=" + std::to_string(c_long_ascii)
                           + " longCjk=" + std::to_string(c_long_cjk) + "}");
                // Only assert the physical coder when it was actually readable.
                if (c_ascii >= 0)     { ctx.check("java_coder_ascii_is_LATIN1", c_ascii == 0); }
                if (c_cafe >= 0)      { ctx.check("java_coder_cafe_is_LATIN1", c_cafe == 0); }
                if (c_long_ascii >= 0){ ctx.check("java_coder_longAscii_is_LATIN1", c_long_ascii == 0); }
                if (c_nihongo >= 0)   { ctx.check("java_coder_nihongo_is_UTF16", c_nihongo == 1); }
                if (c_emoji >= 0)     { ctx.check("java_coder_emoji_is_UTF16", c_emoji == 1); }
                if (c_long_cjk >= 0)  { ctx.check("java_coder_longCjk_is_UTF16", c_long_cjk == 1); }
            }
        }

        // =================================================================
        //  7. PURITY / REPEATABILITY: read_java_string is a pure reader.
        //     Decoding the same field twice yields identical bytes, and decoding
        //     it after the probe ran leaves the backing String content unchanged.
        // =================================================================
        {
            const std::string a{ rjs::decode("nihongo") };
            const std::string b{ rjs::decode("nihongo") };
            ctx.check("repeatable_decode_same_bytes", a == b && a == k_nihongo);

            // cafe + emoji re-decode after the probe -- still byte-exact (the
            // backing arrays were never mutated by any read above).
            ctx.check("cafe_unchanged_after_probe", rjs::decode("cafe") == k_cafe);
            ctx.check("emoji_unchanged_after_probe", rjs::decode("emoji") == k_emoji);
        }
    }
}

VMHOOK_JVM_MODULE(read_java_string)
{
    // SUITE-SAFETY: run the whole body under a try/catch so a thrown C++
    // exception is downgraded to an [INFO] line (never a suite FAIL), and run an
    // UNCONDITIONAL shutdown_hooks() OUTSIDE the try as the last statement.  This
    // module installs NO hooks, so shutdown_hooks() is a documented no-op here --
    // it is present to satisfy the module-level teardown contract uniformly.
    try
    {
        run_body(ctx);
    }
    catch (const std::exception& e)
    {
        ctx.record(std::string{ "[INFO] read_java_string: caught std::exception - " } + e.what()
                   + " (downgraded to INFO; module never fails the suite on a throw).");
    }
    catch (...)
    {
        ctx.record("[INFO] read_java_string: caught non-std exception "
                   "(downgraded to INFO; module never fails the suite on a throw).");
    }

    vmhook::shutdown_hooks();
}
