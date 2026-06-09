// read_java_string JVM test module  (feature area: strings)
//
// THE subject-under-test here is the FREE helper vmhook::read_java_string(oop):
// given a decoded OOP that points at a java.lang.String, it returns the string's
// contents as a std::string encoded in UTF-8.  This module migrates the legacy
// example.cpp test_read_java_string into the modular harness and makes the helper
// the centre of gravity, exhaustively.
//
// HOW THE SUBJECT IS REACHED (identical to how the library reaches it itself):
//   static_field("name")           -> field_proxy for a static String field
//        ->get_compressed_oop()     -> the 32-bit narrow OOP stored in that slot
//   hotspot::decode_oop_pointer(..) -> the real 64-bit java.lang.String OOP
//   read_java_string(that_oop)      -> the decoded std::string  (THE CALL UNDER TEST)
// This is byte-for-byte the same pipeline the field_proxy String getter, array
// readers, and method-return decoders all funnel through (see vmhook.hpp ~13308,
// ~11592, ~12446), so proving it here proves the shared decode core.
//
// WHAT THIS MODULE PROVES on a live JVM (Java 8/11/17/21/24/25 x MSVC/Clang/GCC):
//   * BOTH backing-storage coders decode to BYTE-EXACT UTF-8:
//       - LATIN1 (coder 0): "hello" (pure ASCII, verbatim) and "cafe" (the LATIN1
//         single byte 0xE9 must come out as the TWO UTF-8 bytes C3 A9 -- proving
//         the LATIN1 path UTF-8-ENCODES high bytes rather than copying them raw),
//         plus the LATIN1 ceiling char U+00FF -> C3 BF.
//       - UTF16 (coder 1): "U+65E5 U+672C U+8A9E" (3 BMP CJK code points), a MIXED ASCII+CJK
//         string forcing the whole thing through the UTF-16 decode path, and an
//         astral emoji U+1F600 carried as a SURROGATE PAIR -> 4-byte UTF-8
//         (F0 9F 98 80), exercising read_java_string's surrogate-combining branch.
//   * the Java-8 layout (String.value is char[], always UTF-16, no `coder` field)
//     and the Java-9+ layout (byte[] + coder) decode to the IDENTICAL UTF-8 bytes
//     -- asserted by comparing every decode against a FIXED expected byte sequence
//     that does not depend on the running JDK.
//   * the guard paths return an EMPTY std::string WITHOUT crashing:
//       - the empty String "" (backing length 0 -> length<=0 guard),
//       - a null String reference (compressed OOP 0 -> null-oop guard); the null
//         oop is fed to read_java_string directly to prove it tolerates nullptr.
//   * every decode AGREES with what Java itself reports for the same field
//     (length / code points), cross-checked via a probe-published witness.
//
// SAFETY: this module installs NO hooks (pure reads) so there is nothing to tear
// down.  Every OOP dereference is gated through vmhook::hotspot::is_valid_pointer
// before use, and read_java_string is never handed a pointer this module has not
// either (a) decoded from a live field slot and validated, or (b) deliberately
// chosen to be nullptr to exercise the null guard.
//
// Harness shape mirrors field_static: register_class, a single go/done probe that
// publishes Java-side cross-check facts, and a dense battery of ctx.check()s.
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
    // overloads are non-viable from a static context).  Mirrors field_static::fs.
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

        static auto resolves(const char* name) -> bool
        {
            return static_field(name).has_value();
        }

        // ---- THE SUBJECT PIPELINE -------------------------------------------
        // Resolve a static String field, pull its compressed OOP exactly the way
        // the library does, decode it, and feed the decoded OOP straight into
        // read_java_string.  Returns the decoded std::string.
        //
        // Safety: get_compressed_oop() yields 0 for a null field reference (and
        // for a non-reference field); decode_oop_pointer(0) yields nullptr; the
        // resulting oop is validated with is_valid_pointer before the call, and
        // read_java_string ALSO null/range-guards internally -- so a null/invalid
        // field reference flows through as the empty string with no deref.
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

        // ---- read a published Java cross-check int witness ----
        static auto seen_int(const char* name) -> std::int32_t { return static_field(name)->get(); }
        static auto seen_bool(const char* name) -> bool { return static_field(name)->get(); }
    };

    // Fixed, JDK-independent expected UTF-8 byte sequences for each subject.
    // (Verified against the live fixture: hello / cafe / y / U+65E5 U+672C U+8A9E / AU+65E5B / U+1F600.)
    const std::string k_hello   = "\x68\x65\x6C\x6C\x6F";                          // hello
    const std::string k_cafe    = "\x63\x61\x66\xC3\xA9";                          // c a f e(C3 A9)
    const std::string k_latin1Hi= "\xC3\xBF";                                      // y  U+00FF
    const std::string k_nihongo = "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E";          // U+65E5 U+672C U+8A9E
    const std::string k_mixed   = "\x41\xE6\x97\xA5\x42";                          // A U+65E5 B
    const std::string k_emoji   = "\xF0\x9F\x98\x80";                              // U+1F600 U+1F600

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
}

VMHOOK_JVM_MODULE(read_java_string)
{
    vmhook::register_class<rjs>("vmhook/fixtures/ReadJavaString");

    // =====================================================================
    //  0. Sanity: the fixture class resolves and the subject fields are there.
    // =====================================================================
    ctx.check("rjs_class_registered_ascii_field_resolves", rjs::resolves("ascii"));
    ctx.check("rjs_nihongo_field_resolves", rjs::resolves("nihongo"));
    ctx.check("rjs_null_field_resolves", rjs::resolves("nullRef"));

    // =====================================================================
    //  1. LATIN1 (coder 0) DECODES -- byte-exact UTF-8.
    //     "hello" is pure ASCII (UTF-8 == backing verbatim).  "cafe" carries the
    //     LATIN1 single byte 0xE9, which MUST decode to the two UTF-8 bytes
    //     C3 A9 (NOT a raw 0xE9): the headline proof that the LATIN1 path
    //     UTF-8-encodes high bytes.  U+00FF is the LATIN1 ceiling.
    // =====================================================================
    {
        const std::string ascii{ rjs::decode("ascii") };
        ctx.check("decode_ascii_eq_hello", ascii == k_hello);
        ctx.check("decode_ascii_len_5", ascii.size() == 5);
        ctx.record(std::string{ "[INFO] read_java_string(ascii)   = [" } + to_hex(ascii) + "] expect [" + to_hex(k_hello) + "]");

        const std::string cafe{ rjs::decode("cafe") };
        ctx.check("decode_cafe_byte_exact_utf8", cafe == k_cafe);
        // The crux: 5 UTF-8 bytes for 4 chars, ending C3 A9 (not 4 bytes ending E9).
        ctx.check("decode_cafe_len_5_bytes", cafe.size() == 5);
        ctx.check("decode_cafe_tail_is_C3A9",
                  cafe.size() == 5
                  && static_cast<std::uint8_t>(cafe[3]) == 0xC3u
                  && static_cast<std::uint8_t>(cafe[4]) == 0xA9u);
        ctx.record(std::string{ "[INFO] read_java_string(cafe)    = [" } + to_hex(cafe) + "] expect [" + to_hex(k_cafe) + "]");

        const std::string hi{ rjs::decode("latin1Hi") };
        ctx.check("decode_latin1Hi_eq_C3BF", hi == k_latin1Hi);
        ctx.record(std::string{ "[INFO] read_java_string(latin1Hi)= [" } + to_hex(hi) + "] expect [" + to_hex(k_latin1Hi) + "]");
    }

    // =====================================================================
    //  2. UTF16 (coder 1) DECODES -- byte-exact UTF-8.
    //     "U+65E5 U+672C U+8A9E" (3 BMP CJK), a MIXED ASCII+CJK string (whole string promoted
    //     to UTF-16 so the ASCII chars also traverse the UTF-16 path), and an
    //     astral emoji carried as a SURROGATE PAIR (the surrogate-combining
    //     branch -> a single 4-byte UTF-8 sequence).
    // =====================================================================
    {
        const std::string nihongo{ rjs::decode("nihongo") };
        ctx.check("decode_nihongo_byte_exact_utf8", nihongo == k_nihongo);
        ctx.check("decode_nihongo_len_9_bytes", nihongo.size() == 9); // 3 CJK x 3 bytes
        ctx.record(std::string{ "[INFO] read_java_string(nihongo) = [" } + to_hex(nihongo) + "] expect [" + to_hex(k_nihongo) + "]");

        const std::string mixed{ rjs::decode("mixed") };
        ctx.check("decode_mixed_byte_exact_utf8", mixed == k_mixed);
        // ASCII 'A'(41) and 'B'(42) flank the 3-byte CJK; proves UTF-16-path ASCII.
        ctx.check("decode_mixed_ascii_flanks",
                  mixed.size() == 5
                  && static_cast<std::uint8_t>(mixed.front()) == 0x41u
                  && static_cast<std::uint8_t>(mixed.back()) == 0x42u);
        ctx.record(std::string{ "[INFO] read_java_string(mixed)   = [" } + to_hex(mixed) + "] expect [" + to_hex(k_mixed) + "]");

        const std::string emoji{ rjs::decode("emoji") };
        ctx.check("decode_emoji_surrogate_pair_4byte_utf8", emoji == k_emoji);
        ctx.check("decode_emoji_len_4_bytes", emoji.size() == 4);
        // First byte F0 marks a 4-byte sequence (astral plane) -- the surrogate
        // pair was combined into one code point, not emitted as two 3-byte CESU.
        ctx.check("decode_emoji_leads_with_F0",
                  emoji.size() == 4 && static_cast<std::uint8_t>(emoji.front()) == 0xF0u);
        ctx.record(std::string{ "[INFO] read_java_string(emoji)   = [" } + to_hex(emoji) + "] expect [" + to_hex(k_emoji) + "]");
    }

    // =====================================================================
    //  3. GUARD PATHS -- empty std::string, no crash.
    //     The empty String "" (backing length 0 -> length<=0 guard) and a null
    //     String reference (compressed OOP 0 -> null-oop guard).  The null oop
    //     is fed to read_java_string DIRECTLY (nullptr) to prove tolerance.
    // =====================================================================
    {
        const std::string empty{ rjs::decode("empty") };
        ctx.check("decode_empty_is_empty_string", empty.empty());

        // nullRef's compressed OOP must be 0 (the Java null reference).
        const std::uint32_t null_comp{ rjs::compressed_oop("nullRef") };
        ctx.check("nullRef_compressed_oop_is_zero", null_comp == 0u);

        // decode("nullRef") routes compressed 0 -> decode_oop_pointer -> nullptr
        // -> read_java_string(nullptr) -> "" with no dereference.
        const std::string null_decoded{ rjs::decode("nullRef") };
        ctx.check("decode_nullRef_is_empty_string", null_decoded.empty());

        // Belt-and-braces: call read_java_string(nullptr) explicitly -- the
        // documented null-oop guard must return "" and never dereference.
        const std::string direct_null = vmhook::read_java_string(nullptr);
        ctx.check("read_java_string_nullptr_is_empty", direct_null.empty());

        // And an OBVIOUSLY-invalid (non-null but unaligned/low) pointer: the
        // is_valid_pointer guard inside read_java_string must reject it as ""
        // (we pass an odd address which is_valid_pointer rejects outright).
        const std::string direct_bogus =
            vmhook::read_java_string(reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x1)));
        ctx.check("read_java_string_bogus_ptr_is_empty", direct_bogus.empty());
    }

    // =====================================================================
    //  4. CROSS-CHECK against Java's own view (probe-published witnesses), and
    //     the JDK8(char[]) vs JDK9+(byte[]) IDENTICAL-OUTPUT invariant.
    //     The decodes above already compare against FIXED expected bytes that do
    //     not vary by JDK, so passing on every matrix row IS the cross-JDK
    //     invariant.  Here we additionally confirm Java agrees on lengths/code
    //     points for the same fields, and record which physical coder each case
    //     used (best-effort; the `coder` field is module-encapsulated on JDK 9+
    //     and absent on JDK 8, so coder values are diagnostic, not assertions).
    // =====================================================================
    {
        const bool done{ drive(ctx) };
        ctx.check("crosscheck_probe_completed", done);

        if (done)
        {
            // Java-reported lengths / code points for the GET targets.
            ctx.check("java_ascii_len_5", rjs::seen_int("jAsciiLen") == 5);
            ctx.check("java_cafe_len_4", rjs::seen_int("jCafeLen") == 4);
            ctx.check("java_cafe_cp3_is_E9", rjs::seen_int("jCafeCp3") == 0x00E9);
            ctx.check("java_latin1Hi_cp0_is_FF", rjs::seen_int("jLatin1HiCp0") == 0x00FF);
            ctx.check("java_nihongo_len_3", rjs::seen_int("jNihongoLen") == 3);
            ctx.check("java_nihongo_cp0_is_65E5", rjs::seen_int("jNihongoCp0") == 0x65E5);
            ctx.check("java_nihongo_cp1_is_672C", rjs::seen_int("jNihongoCp1") == 0x672C);
            ctx.check("java_nihongo_cp2_is_8A9E", rjs::seen_int("jNihongoCp2") == 0x8A9E);
            ctx.check("java_mixed_len_3", rjs::seen_int("jMixedLen") == 3);
            ctx.check("java_emoji_cpCount_1", rjs::seen_int("jEmojiCpCount") == 1);
            ctx.check("java_emoji_cp0_is_1F600", rjs::seen_int("jEmojiCp0") == 0x1F600);
            ctx.check("java_empty_len_0", rjs::seen_int("jEmptyLen") == 0);
            ctx.check("java_nullRef_is_null", rjs::seen_bool("jNullIsNull"));

            // Physical-coder coverage (diagnostic).  When the coder field is
            // readable (JDK 9+ launched with --add-opens java.base/java.lang),
            // ASCII/cafe are LATIN1 (0) and CJK/emoji are UTF16 (1); on JDK 8
            // there is no coder field (the char[] path) and on a locked-down
            // JDK 9+ reflection returns -1 -- in both cases the byte-exact
            // decodes above already prove BOTH internal coder branches ran.
            const bool has_coder{ rjs::seen_bool("jHasCoderField") };
            const std::int32_t c_ascii{ rjs::seen_int("jCoderAscii") };
            const std::int32_t c_cafe{ rjs::seen_int("jCoderCafe") };
            const std::int32_t c_nihongo{ rjs::seen_int("jCoderNihongo") };
            const std::int32_t c_emoji{ rjs::seen_int("jCoderEmoji") };
            ctx.record(std::string{ "[INFO] coder field present (JDK9+)=" } + (has_coder ? "true" : "false")
                       + " coder{ascii=" + std::to_string(c_ascii)
                       + " cafe=" + std::to_string(c_cafe)
                       + " nihongo=" + std::to_string(c_nihongo)
                       + " emoji=" + std::to_string(c_emoji) + "}");
            // Only assert the physical coder when it was actually readable.
            if (c_ascii >= 0)
            {
                ctx.check("java_coder_ascii_is_LATIN1", c_ascii == 0);
            }
            if (c_cafe >= 0)
            {
                ctx.check("java_coder_cafe_is_LATIN1", c_cafe == 0);
            }
            if (c_nihongo >= 0)
            {
                ctx.check("java_coder_nihongo_is_UTF16", c_nihongo == 1);
            }
            if (c_emoji >= 0)
            {
                ctx.check("java_coder_emoji_is_UTF16", c_emoji == 1);
            }
        }
    }

    // =====================================================================
    //  5. PURITY / REPEATABILITY: read_java_string is a pure reader.  Decoding
    //     the same field twice yields identical bytes, and decoding it after the
    //     probe ran leaves the underlying String content unchanged (no mutation).
    // =====================================================================
    {
        const std::string a{ rjs::decode("nihongo") };
        const std::string b{ rjs::decode("nihongo") };
        ctx.check("repeatable_decode_same_bytes", a == b && a == k_nihongo);

        // cafe re-decode after the probe -- still byte-exact (pure read).
        const std::string cafe_again{ rjs::decode("cafe") };
        ctx.check("cafe_unchanged_after_probe", cafe_again == k_cafe);
    }

    // No hooks were installed (pure reads), so there is nothing to tear down.
}
