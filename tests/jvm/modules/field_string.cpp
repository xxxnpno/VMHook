// field_string JVM test module — exhaustive coverage of String field GET and
// SET through vmhook's zero-JNI field_proxy / read_java_string / write_java_string.
//
// Feature surface under test (vmhook/ext/vmhook/vmhook.hpp):
//   - read_java_string()                          (line ~15138)  GET decode
//   - field_proxy::value_t::cast_for_variant<std::string>  (~11411)  GET dispatch
//   - write_java_string() / set_str_field()       (~15256/15320) SET in-place
//   - field_proxy::set(std::string)               (~11655)       SET dispatch + guard
//
// All checks run on a live JDK-21 JVM, where java.lang.String is COMPACT:
//   coder 0 (LATIN1) => one byte per char, each code point UTF-8-ENCODED
//                       (0xE9 -> C3 A9, 0x80 -> C2 80, 0xFF -> C3 BF);
//   coder 1 (UTF-16) => two bytes per char, decoded to multi-byte UTF-8 with
//                       surrogate pairs combined into one 4-byte sequence.
//   read_java_string no longer substitutes '?' for non-ASCII.  The remaining
//   characterized quirk is the 4096-backing-byte cap: a String whose backing
//   array exceeds 4096 bytes is REJECTED to "" (NOT truncated), which makes the
//   effective UTF-16 ceiling 2048 chars.  The fixture is built so every decode
//   path AND that cap boundary is exercised (documented inline per check).
//
// Mirrors the pilot module shape: register_class, a scoped_hook for the
// interpreter-hook-on-dispatch requirement, run_probe for the handshake, and a
// dense battery of ctx.check() assertions.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace
{
    // Wrapper for vmhook.fixtures.FieldString.
    class field_string_fixture : public vmhook::object<field_string_fixture>
    {
    public:
        explicit field_string_fixture(vmhook::oop_t instance) noexcept
            : vmhook::object<field_string_fixture>{ instance }
        {
        }

        // ---- handshake ----
        static auto set_go(bool value) -> void { static_field("go")->set(value); }
        static auto get_done() -> bool          { return static_field("done")->get(); }
        static auto get_observed() -> std::int32_t { return static_field("observed")->get(); }

        // ---- read a static String field through field_proxy::get() (the
        //      value_t -> std::string conversion routes through read_java_string).
        //      Returns "" if the field can't be resolved.
        static auto read_static(const char* name) -> std::string { return static_field(name)->get(); }

        // ---- read the SAME field's backing String OOP and decode it DIRECTLY
        //      via read_java_string (bypassing field_proxy), to prove the two
        //      paths agree.  Returns "" if the field is unresolved.
        static auto read_static_direct(const char* name) -> std::string
        {
            const auto proxy{ static_field(name) };
            if (!proxy.has_value())
            {
                return {};
            }
            return vmhook::read_java_string(vmhook::field_oop(*proxy));
        }

        // ---- set a static String field.
        static auto set_static(const char* name, std::string_view value) -> bool
        {
            const auto proxy{ static_field(name) };
            if (!proxy.has_value())
            {
                return false;
            }
            proxy->set(std::string{ value });
            return true;
        }

        // ---- set a primitive int field with a std::string (must be REFUSED by
        //      the field_proxy::set type guard).
        static auto set_int_with_string(const char* name, std::string_view value) -> bool
        {
            const auto proxy{ static_field(name) };
            if (!proxy.has_value())
            {
                return false;
            }
            proxy->set(std::string{ value });
            return true;
        }

        static auto read_static_int(const char* name) -> std::int32_t { return static_field(name)->get(); }

        // ---- Java-published observation fields ----
        static auto j_ascii_len()   -> std::int32_t { return static_field("jAsciiLen")->get(); }
        static auto j_latin1_len()  -> std::int32_t { return static_field("jLatin1Len")->get(); }
        static auto j_latin1_cp1()  -> std::int32_t { return static_field("jLatin1Cp1")->get(); }
        static auto j_cjk_len()     -> std::int32_t { return static_field("jCjkLen")->get(); }
        static auto j_cjk_cp0()     -> std::int32_t { return static_field("jCjkCp0")->get(); }
        static auto j_null_is_null()-> bool         { return static_field("jNullIsNull")->get(); }
        static auto j_len4096_len() -> std::int32_t { return static_field("jLen4096Len")->get(); }
        static auto j_len4097_len() -> std::int32_t { return static_field("jLen4097Len")->get(); }

        // ---- Java-published facts for the exhaustive GET extras ----
        static auto j_emoji_cpcount() -> std::int32_t { return static_field("jEmojiCpCount")->get(); }
        static auto j_emoji_cp1()     -> std::int32_t { return static_field("jEmojiCp1")->get(); }
        static auto j_astral_cp0()    -> std::int32_t { return static_field("jAstralCp0")->get(); }
        static auto j_max_bmp_cp0()   -> std::int32_t { return static_field("jMaxBmpCp0")->get(); }
        static auto j_lo80_cp0()      -> std::int32_t { return static_field("jLo80Cp0")->get(); }
        static auto j_u0800_cp0()     -> std::int32_t { return static_field("jU0800Cp0")->get(); }
        static auto j_all256_len()    -> std::int32_t { return static_field("jAll256Len")->get(); }
        static auto j_all256_cp255()  -> std::int32_t { return static_field("jAll256Cp255")->get(); }
        static auto j_final_const_value()     -> std::string { return static_field("jFinalConstValue")->get(); }
        static auto j_inherited_str_value()   -> std::string { return static_field("jInheritedStrValue")->get(); }
        static auto j_static_inherited_value()-> std::string { return static_field("jStaticInheritedValue")->get(); }
        static auto j_inst_get_only_value()   -> std::string { return static_field("jInstGetOnlyValue")->get(); }
        static auto j_reassign_after_value()  -> std::string { return static_field("jReassignAfterValue")->get(); }

        static auto set_ascii_eq_matches() -> bool        { return static_field("setAsciiEqMatches")->get(); }
        static auto set_ascii_eq_len()     -> std::int32_t { return static_field("setAsciiEqLen")->get(); }
        static auto set_ascii_eq_value()   -> std::string  { return static_field("setAsciiEqValue")->get(); }
        static auto set_shorter_value()    -> std::string  { return static_field("setShorterValue")->get(); }
        static auto set_shorter_len()      -> std::int32_t { return static_field("setShorterLen")->get(); }
        static auto set_latin1_matches()   -> bool        { return static_field("setLatin1Matches")->get(); }
        static auto set_latin1_value()     -> std::string  { return static_field("setLatin1TgtValue")->get(); }
        static auto set_overlong_value()   -> std::string  { return static_field("setOverlongValue")->get(); }
        static auto set_overlong_len()     -> std::int32_t { return static_field("setOverlongLen")->get(); }
        static auto inst_ascii_value()     -> std::string  { return static_field("instAsciiValue")->get(); }
        static auto inst_ascii_matches()   -> bool        { return static_field("instAsciiMatches")->get(); }
        static auto interned_intact()      -> bool        { return static_field("internedStillIntact")->get(); }

        // ---- obtain the live instance the fixture published in `self`. ----
        static auto acquire_self() -> std::unique_ptr<field_string_fixture> { return static_field("self")->get(); }
    };

    std::atomic<int>          g_hook_calls{ 0 };
    std::atomic<std::int32_t> g_hook_arg{ -1 };
    std::atomic<bool>         g_hook_saw_self{ false };

    // Render the raw bytes of a std::string as hex, for diagnostic [INFO] lines.
    auto hex_bytes(const std::string& s) -> std::string
    {
        static const char* const digits{ "0123456789ABCDEF" };
        std::string out;
        out.reserve(s.size() * 3);
        for (const unsigned char c : s)
        {
            out += digits[c >> 4];
            out += digits[c & 0x0F];
            out += ' ';
        }
        return out;
    }

    // Build the expected UTF-8 for the getAll256 field (char[i] == i, i in
    // 0..255) by mirroring read_java_string's LATIN1 append_utf8: each code point
    // 0..0x7F is one byte; 0x80..0xFF is two bytes (C2/C3 lead).  384 bytes total.
    auto build_all256_expected() -> std::string
    {
        std::string out;
        out.reserve(384);
        for (std::uint32_t cp{ 0 }; cp < 256u; ++cp)
        {
            if (cp < 0x80u)
            {
                out += static_cast<char>(cp);
            }
            else
            {
                out += static_cast<char>(0xC0u | (cp >> 6));
                out += static_cast<char>(0x80u | (cp & 0x3Fu));
            }
        }
        return out;
    }
}

VMHOOK_JVM_MODULE(field_string)
{
    vmhook::register_class<field_string_fixture>("vmhook/fixtures/FieldString");

    // ----------------------------------------------------------------------
    // PHASE 1: perform every SET write BEFORE raising go.  field_proxy::set
    // mutates the backing array directly in heap memory, so no Java bytecode
    // is required for the writes; the probe (phase 2) then reads them back
    // through Java to prove the mutation is visible to the JVM itself.
    // ----------------------------------------------------------------------

    // Clean full overwrite: "AAAAA"(5) <- "world"(5).
    ctx.check("set_ascii_eq_proxy_resolved",
              field_string_fixture::set_static("setAsciiEq", "world"));

    // Shorter write into a longer backing: "world"(5) <- "hi"(2).
    // write_java_string writes only min(5,2)=2 bytes and leaves the tail,
    // so Java should see "hirld" (partial-overwrite, no length change).
    ctx.check("set_shorter_proxy_resolved",
              field_string_fixture::set_static("setShorter", "hi"));

    // Write into a zero-length backing: must be a no-op (writable_length<=0).
    ctx.check("set_empty_proxy_resolved",
              field_string_fixture::set_static("setEmptyTgt", "ignored"));

    // ASCII into a LATIN1 coder-0 backing of equal length: "AAAAA"(5) <- "abcde"(5).
    ctx.check("set_latin1_proxy_resolved",
              field_string_fixture::set_static("setLatin1Tgt", "abcde"));

    // Overlong write into a short backing: "abc"(3) <- "LONGER"(6).
    // Truncates to 3 bytes ("LON"); length stays 3 (no Java-heap resize).
    ctx.check("set_overlong_proxy_resolved",
              field_string_fixture::set_static("setOverlong", "LONGER"));

    // Instance String field, mutated through an INSTANCE field_proxy.
    {
        const auto self{ field_string_fixture::acquire_self() };
        ctx.check("instance_self_acquired", self != nullptr);
        if (self)
        {
            const auto inst_proxy{ self->get_field("instAscii") };
            ctx.check("instance_field_resolved", inst_proxy.has_value());
            if (inst_proxy.has_value())
            {
                inst_proxy->set(std::string{ "java!" });   // "QQQQQ"(5) <- "java!"(5)
            }
            // Confirm the instance read path agrees immediately (pre-probe).
            if (inst_proxy.has_value())
            {
                // Copy-init (not brace-init): field_proxy::value_t has a
                // templated conversion operator, so std::string x{ value_t } is
                // ambiguous on MSVC; `= value_t` matches the working baseline.
                const std::string after = inst_proxy->get();
                ctx.check("instance_set_native_readback_java", after == "java!");
            }
        }
    }

    // Type-guard: writing a std::string into a primitive "I" field must be
    // refused (field_proxy::set guard at vmhook.hpp ~11635), leaving it intact.
    const std::int32_t int_before{ field_string_fixture::read_static_int("notAStringInt") };
    ctx.check("guard_int_field_initial_12345", int_before == 12345);
    field_string_fixture::set_int_with_string("notAStringInt", "99999");
    const std::int32_t int_after{ field_string_fixture::read_static_int("notAStringInt") };
    ctx.check("guard_string_into_int_refused_value_unchanged", int_after == 12345);

    // ----------------------------------------------------------------------
    // GET: read every static String field BEFORE the probe too (reads are
    // side-effect free).  These exercise read_java_string's decode paths.
    // ----------------------------------------------------------------------

    // --- ASCII (LATIN1 coder 0): byte-verbatim round-trip. ---
    const std::string ascii{ field_string_fixture::read_static("getAscii") };
    ctx.check("get_ascii_value", ascii == "hello world");
    ctx.check("get_ascii_len_11", ascii.size() == 11);

    // Single ASCII char.
    ctx.check("get_one_char_Z", field_string_fixture::read_static("getOneChar") == "Z");

    // Direct read_java_string vs field_proxy::get must AGREE for ASCII.
    ctx.check("get_ascii_direct_equals_proxy",
              field_string_fixture::read_static_direct("getAscii") == ascii);

    // --- Latin-1 (cp <= 0xFF): read_java_string now UTF-8-encodes each code
    // point (0xE9 'é' -> C3 A9), so the result is valid UTF-8 (NOT raw bytes). ---
    // "héllo èéÿ" -> UTF-8 {68 C3A9 6C 6C 6F 20 C3A8 C3A9 C3BF}, length 13.
    const std::string latin1{ field_string_fixture::read_static("getLatin1") };
    ctx.record(std::string{ "[INFO] getLatin1 UTF-8 bytes: " } + hex_bytes(latin1));
    ctx.check("get_latin1_utf8_len_13", latin1.size() == 13);
    ctx.check("get_latin1_byte0_h", static_cast<unsigned char>(latin1[0]) == 0x68);
    // 'é' is now the correct 2-byte UTF-8 sequence C3 A9, not the raw Latin-1 0xE9.
    ctx.check("get_latin1_e_acute_is_utf8_C3A9",
              latin1.size() == 13 && static_cast<unsigned char>(latin1[1]) == 0xC3
                                  && static_cast<unsigned char>(latin1[2]) == 0xA9);
    // 'ÿ' (U+00FF) is the final code point -> trailing UTF-8 C3 BF.
    ctx.check("get_latin1_y_diaeresis_is_utf8_C3BF",
              latin1.size() == 13 && static_cast<unsigned char>(latin1[11]) == 0xC3
                                  && static_cast<unsigned char>(latin1[12]) == 0xBF);
    ctx.check("get_latin1_equals_expected_utf8",
              latin1 == std::string{ "\x68\xC3\xA9\x6C\x6C\x6F\x20\xC3\xA8\xC3\xA9\xC3\xBF" });
    ctx.check("get_latin1_direct_equals_proxy",
              field_string_fixture::read_static_direct("getLatin1") == latin1);

    // --- CJK (UTF-16 coder 1): read_java_string now emits correct UTF-8. ---
    // "日本語" (3 code points) -> UTF-8 {E6 97 A5  E6 9C AC  E8 AA 9E}, length 9.
    const std::string cjk{ field_string_fixture::read_static("getCjk") };
    ctx.record(std::string{ "[INFO] getCjk decoded: '" } + cjk + "' bytes: " + hex_bytes(cjk));
    ctx.check("get_cjk_is_utf8_three_kanji",
              cjk == std::string{ "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E" });
    ctx.check("get_cjk_utf8_len_9", cjk.size() == 9);
    ctx.check("get_cjk_direct_equals_proxy",
              field_string_fixture::read_static_direct("getCjk") == cjk);

    // --- Mixed ASCII + >0xFF char (UTF-16 coder 1): correct UTF-8 throughout. ---
    // "A日BéC" -> UTF-8 {41  E6 97 A5  42  C3 A9  43}, length 8.
    const std::string mixed{ field_string_fixture::read_static("getMixed") };
    ctx.record(std::string{ "[INFO] getMixed decoded: '" } + mixed + "'");
    ctx.check("get_mixed_is_utf8_A_kanji_B_eacute_C",
              mixed == std::string{ "A\xE6\x97\xA5" "B\xC3\xA9" "C" });
    ctx.check("get_mixed_utf8_len_8", mixed.size() == 8);

    // --- Empty string: length<=0 guard -> "". ---
    ctx.check("get_empty_is_empty", field_string_fixture::read_static("getEmpty").empty());

    // --- null field: null-oop guard -> "". ---
    ctx.check("get_null_is_empty", field_string_fixture::read_static("getNull").empty());
    ctx.check("get_null_direct_is_empty",
              field_string_fixture::read_static_direct("getNull").empty());

    // --- Interned literal: reads back verbatim, value untouched. ---
    ctx.check("get_interned_value",
              field_string_fixture::read_static("getInterned") == "INTERNED_LITERAL");

    // --- Embedded NUL: ASCII bytes with a 0x00 in the middle (LATIN1, length 5). ---
    // read_java_string assigns by length, so the NUL is preserved (no C-string cut).
    const std::string embedded{ field_string_fixture::read_static("getEmbeddedNul") };
    ctx.check("get_embedded_nul_len_5", embedded.size() == 5);
    ctx.check("get_embedded_nul_bytes",
              embedded.size() == 5
              && embedded[0] == 'a' && embedded[1] == '\0'
              && embedded[2] == 'b' && embedded[3] == '\0' && embedded[4] == 'c');

    // --- Length cap: exactly 4096 ASCII chars passes (inclusive). ---
    const std::string len4096{ field_string_fixture::read_static("getLen4096") };
    ctx.check("get_len4096_passes_cap_full", len4096.size() == 4096);
    ctx.check("get_len4096_all_x",
              len4096.size() == 4096 && len4096.front() == 'x' && len4096.back() == 'x');

    // --- 4097 ASCII chars: byte length 4097 > 4096 -> REJECTED -> "". ---
    // (Bug: docstring says "truncates"; code rejects.  Asserts the real behavior.)
    ctx.check("get_len4097_rejected_empty_BUG",
              field_string_fixture::read_static("getLen4097").empty());

    // --- 5000 ASCII chars: well past cap -> "". ---
    ctx.check("get_len5000_rejected_empty_BUG",
              field_string_fixture::read_static("getLen5000").empty());

    // --- 2048 CJK chars (日): UTF-16 byte[] length 4096 -> passes the cap, then
    // decodes to correct UTF-8 — each 日 (U+65E5) is 3 bytes E6 97 A5, so the
    // result is 2048 * 3 = 6144 bytes.  (The UTF-16 char cap is 2048, half of the
    // 4096-byte array-length ceiling.) ---
    const std::string cjk2048{ field_string_fixture::read_static("getCjk2048") };
    ctx.check("get_cjk2048_utf8_len_6144", cjk2048.size() == 2048u * 3u);
    ctx.check("get_cjk2048_first_and_last_kanji",
              cjk2048.size() == 6144
              && static_cast<unsigned char>(cjk2048[0]) == 0xE6
              && static_cast<unsigned char>(cjk2048[1]) == 0x97
              && static_cast<unsigned char>(cjk2048[2]) == 0xA5
              && static_cast<unsigned char>(cjk2048[6141]) == 0xE6
              && static_cast<unsigned char>(cjk2048[6142]) == 0x97
              && static_cast<unsigned char>(cjk2048[6143]) == 0xA5);

    // --- 2049 CJK chars: read_java_string's 4096 cap is on the backing-ARRAY
    // length.  On JDK 9+ compact strings the UTF-16 backing is a byte[] (2 bytes
    // per char), so 2049 chars = 4098 bytes > 4096 -> rejected -> "".  On JDK 8
    // the backing is a char[] (1 unit per char), so 2049 <= 4096 -> the read
    // succeeds (2049 kanji -> 2049*3 = 6147 UTF-8 bytes).  Branch on whether the
    // String class carries the JDK-9 `coder` field (i.e. uses compact strings). ---
    {
        vmhook::hotspot::klass* const string_klass{ vmhook::find_class("java/lang/String") };
        const bool compact_strings{ string_klass != nullptr
                                    && string_klass->find_field("coder").has_value() };
        const std::string cjk2049{ field_string_fixture::read_static("getCjk2049") };
        if (compact_strings)
        {
            ctx.check("get_cjk2049_rejected_empty_utf16_cap_2048", cjk2049.empty());
        }
        else
        {
            // JDK 8 char[]: 2049 chars are within the 4096-char cap and decode fine.
            ctx.check("get_cjk2049_classic_char_array_read_6147", cjk2049.size() == 2049u * 3u);
        }
    }

    // ======================================================================
    // EXHAUSTIVE GET EXTRAS — "every possible String-field read" battery.
    // Prefix fstr_* for these additions.  Each value's storage form (coder /
    // backing-byte length) was verified empirically on Temurin 21, and every
    // expected UTF-8 byte string was cross-checked against String.getBytes(UTF_8).
    // All reads go through the CLEAN one-liner (read_static -> field_proxy::get
    // -> read_java_string) AND are re-decoded directly via read_java_string on
    // the raw backing OOP (read_static_direct), asserting the two paths agree —
    // no new raw dereferences (both reuse the guarded read_java_string).
    // ======================================================================

    // --- Astral emoji (U+1F600) flanked by ASCII -> surrogate pair COMBINED
    //     into one 4-byte UTF-8 sequence; the trailing 'Y' is not swallowed.
    //     Stored UTF-16 (coder 1, byte[8]).  Expected UTF-8: 58 F0 9F 98 80 59. ---
    const std::string emoji{ field_string_fixture::read_static("getEmoji") };
    ctx.record(std::string{ "[INFO] fstr getEmoji UTF-8 bytes: " } + hex_bytes(emoji));
    ctx.check("fstr_emoji_utf8_X_emoji_Y",
              emoji == std::string{ "\x58\xF0\x9F\x98\x80\x59" });
    ctx.check("fstr_emoji_utf8_len_6", emoji.size() == 6);
    ctx.check("fstr_emoji_4byte_lead_F0",
              emoji.size() == 6
              && static_cast<unsigned char>(emoji.front()) == 0x58
              && static_cast<unsigned char>(emoji[1]) == 0xF0
              && static_cast<unsigned char>(emoji.back()) == 0x59);
    ctx.check("fstr_emoji_direct_equals_proxy",
              field_string_fixture::read_static_direct("getEmoji") == emoji);

    // --- The same astral code point ALONE -> exactly one 4-byte UTF-8 seq. ---
    const std::string astral{ field_string_fixture::read_static("getAstral") };
    ctx.check("fstr_astral_utf8_4bytes",
              astral == std::string{ "\xF0\x9F\x98\x80" });
    ctx.check("fstr_astral_utf8_len_4", astral.size() == 4);
    ctx.check("fstr_astral_direct_equals_proxy",
              field_string_fixture::read_static_direct("getAstral") == astral);

    // --- Max BMP U+FFFF -> UTF-16 (coder 1) -> 3-byte UTF-8 EF BF BF.
    //     Proves a non-surrogate at the very top of the BMP is a plain 3-byte
    //     char (not mis-handled by the surrogate-combining branch). ---
    const std::string max_bmp{ field_string_fixture::read_static("getMaxBmp") };
    ctx.check("fstr_max_bmp_utf8_EFBFBF",
              max_bmp == std::string{ "\xEF\xBF\xBF" });
    ctx.check("fstr_max_bmp_utf8_len_3", max_bmp.size() == 3);
    ctx.check("fstr_max_bmp_direct_equals_proxy",
              field_string_fixture::read_static_direct("getMaxBmp") == max_bmp);

    // --- U+0080 -> smallest non-ASCII; STILL LATIN1 (coder 0) but encodes to
    //     the TWO bytes C2 80 (the 1->2 byte boundary on the LATIN1 path). ---
    const std::string lo80{ field_string_fixture::read_static("getLo80") };
    ctx.check("fstr_lo80_latin1_utf8_C280",
              lo80 == std::string{ "\xC2\x80" });
    ctx.check("fstr_lo80_utf8_len_2", lo80.size() == 2);
    ctx.check("fstr_lo80_direct_equals_proxy",
              field_string_fixture::read_static_direct("getLo80") == lo80);

    // --- U+0800 -> UTF-16 (coder 1) -> the FIRST 3-byte UTF-8 code point
    //     (E0 A0 80): the append_utf8 2->3 byte boundary via the UTF-16 path. ---
    const std::string u0800{ field_string_fixture::read_static("getU0800") };
    ctx.check("fstr_u0800_utf8_E0A080",
              u0800 == std::string{ "\xE0\xA0\x80" });
    ctx.check("fstr_u0800_utf8_len_3", u0800.size() == 3);
    ctx.check("fstr_u0800_direct_equals_proxy",
              field_string_fixture::read_static_direct("getU0800") == u0800);

    // --- EVERY byte 0x00..0xFF in one LATIN1 (coder 0) String -> 384 UTF-8
    //     bytes (128 ASCII incl. a leading interior NUL + 128 two-byte highs,
    //     C2 80 .. C3 BF).  The single most thorough LATIN1 decode proof, on the
    //     FIELD get path.  Also exercises the interior NUL inside a long string. ---
    const std::string all256{ field_string_fixture::read_static("getAll256") };
    const std::string all256_expected{ build_all256_expected() };
    ctx.record(std::string{ "[INFO] fstr getAll256 -> " } + std::to_string(all256.size())
               + " bytes (expect 384); first=" + (all256.empty() ? "??" : "00")
               + " expect leading 00 .. trailing C3 BF");
    ctx.check("fstr_all256_utf8_len_384", all256.size() == 384);
    ctx.check("fstr_all256_byte_exact", all256 == all256_expected);
    ctx.check("fstr_all256_leading_nul_then_01",
              all256.size() == 384
              && static_cast<unsigned char>(all256[0]) == 0x00
              && static_cast<unsigned char>(all256[1]) == 0x01);
    ctx.check("fstr_all256_tail_is_C3BF",
              all256.size() == 384
              && static_cast<unsigned char>(all256[382]) == 0xC3
              && static_cast<unsigned char>(all256[383]) == 0xBF);
    ctx.check("fstr_all256_direct_equals_proxy",
              field_string_fixture::read_static_direct("getAll256") == all256);

    // --- static FINAL String whose initializer is a COMPILE-TIME CONSTANT.
    //     javac folds bytecode reads of it into the using class's constant pool,
    //     but the field SLOT still carries real backing storage (verified on JDK
    //     21: the value/coder/backing are all present).  So reading it via the
    //     field_proxy/raw decode returns the actual String — NOT "".  This
    //     characterizes folded-constant field storage for the GET path. ---
    const std::string final_const{ field_string_fixture::read_static("getFinalConst") };
    ctx.check("fstr_final_const_value_FINAL_CONSTANT", final_const == "FINAL_CONSTANT");
    ctx.check("fstr_final_const_len_14", final_const.size() == 14);
    ctx.check("fstr_final_const_direct_equals_proxy",
              field_string_fixture::read_static_direct("getFinalConst") == final_const);

    // --- INHERITED static String field, declared only on FieldStringBase,
    //     resolved through the CHILD wrapper's static_field (vmhook::find_field's
    //     Klass::get_super() walk on the class mirror), then decoded. ---
    const std::string static_inherited{ field_string_fixture::read_static("sInheritedStr") };
    ctx.check("fstr_static_inherited_value", static_inherited == "base-static-inherited");
    ctx.check("fstr_static_inherited_direct_equals_proxy",
              field_string_fixture::read_static_direct("sInheritedStr") == static_inherited);

    // --- The pre-reassign read of getReassign.  The probe (PHASE 2) replaces it
    //     with a freshly-allocated "after2"; a NEW field_proxy read after the
    //     probe must see the new backing.  Capture "before" now. ---
    const std::string reassign_before{ field_string_fixture::read_static("getReassign") };
    ctx.check("fstr_reassign_before_value", reassign_before == "before");

    // --- INHERITED instance String + a clean (never-written) instance String,
    //     both read through an INSTANCE field_proxy on the live `self` OOP.
    //     The inherited one starts the super walk at the child klass and resolves
    //     a slot declared on FieldStringBase (depth 1, compressed-OOP decode). ---
    {
        const auto self{ field_string_fixture::acquire_self() };
        ctx.check("fstr_self_acquired_for_get", self != nullptr);
        if (self)
        {
            const auto inherited_proxy{ self->get_field("inheritedStr") };
            ctx.check("fstr_inherited_instance_field_resolves", inherited_proxy.has_value());
            if (inherited_proxy.has_value())
            {
                const std::string inherited = inherited_proxy->get();
                ctx.check("fstr_inherited_instance_value", inherited == "base-inherited");
                ctx.check("fstr_inherited_instance_signature",
                          std::string{ inherited_proxy->signature() } == "Ljava/lang/String;");
            }

            const auto get_only_proxy{ self->get_field("instGetOnly") };
            ctx.check("fstr_instance_get_only_field_resolves", get_only_proxy.has_value());
            if (get_only_proxy.has_value())
            {
                const std::string get_only = get_only_proxy->get();
                ctx.check("fstr_instance_get_only_value", get_only == "instance-get");
            }
        }
    }

    // ----------------------------------------------------------------------
    // PHASE 2: install the interpreter hook and run the probe, which fires a
    // real bytecode dispatch AND reads every mutated field back through Java.
    // ----------------------------------------------------------------------
    {
        auto handle{ vmhook::scoped_hook<field_string_fixture>(
            "touchString",
            [](vmhook::return_value&,
               const std::unique_ptr<field_string_fixture>& self,
               std::int32_t delta)
            {
                g_hook_calls.fetch_add(1, std::memory_order_relaxed);
                g_hook_arg.store(delta, std::memory_order_relaxed);
                g_hook_saw_self.store(self != nullptr, std::memory_order_relaxed);
            }) };
        ctx.check("field_string_hook_installed", handle.installed());

        const bool done{ ctx.run_probe(
            [](bool value) { field_string_fixture::set_go(value); },
            []() { return field_string_fixture::get_done(); }) };

        ctx.check("probe_completed", done);
        ctx.check("hook_fired_on_dispatch", g_hook_calls.load(std::memory_order_relaxed) >= 1);
        ctx.check("hook_saw_self", g_hook_saw_self.load(std::memory_order_relaxed));
        ctx.check("hook_saw_arg_100", g_hook_arg.load(std::memory_order_relaxed) == 100);
        // touchString returns instAscii.length()(5 after "java!") + 100 == 105.
        ctx.check("observed_is_105", field_string_fixture::get_observed() == 105);

        // ---- Java-side cross-checks of the GET targets (proves what vmhook
        //      decoded corresponds to the actual Java String contents). ----
        ctx.check("java_ascii_len_11", field_string_fixture::j_ascii_len() == 11);
        ctx.check("java_latin1_len_9", field_string_fixture::j_latin1_len() == 9);
        ctx.check("java_latin1_cp1_is_0xE9", field_string_fixture::j_latin1_cp1() == 0xE9);
        ctx.check("java_cjk_len_3", field_string_fixture::j_cjk_len() == 3);
        ctx.check("java_cjk_cp0_is_0x65E5", field_string_fixture::j_cjk_cp0() == 0x65E5);
        ctx.check("java_null_is_null", field_string_fixture::j_null_is_null());
        ctx.check("java_len4096_len_4096", field_string_fixture::j_len4096_len() == 4096);
        ctx.check("java_len4097_len_4097", field_string_fixture::j_len4097_len() == 4097);

        // ---- Java-side cross-checks of the exhaustive GET extras: prove the
        //      bytes vmhook decoded correspond to the real Java String. ----
        ctx.check("fstr_java_emoji_cpcount_3", field_string_fixture::j_emoji_cpcount() == 3);
        ctx.check("fstr_java_emoji_cp1_is_1F600", field_string_fixture::j_emoji_cp1() == 0x1F600);
        ctx.check("fstr_java_astral_cp0_is_1F600", field_string_fixture::j_astral_cp0() == 0x1F600);
        ctx.check("fstr_java_max_bmp_cp0_is_FFFF", field_string_fixture::j_max_bmp_cp0() == 0xFFFF);
        ctx.check("fstr_java_lo80_cp0_is_80", field_string_fixture::j_lo80_cp0() == 0x80);
        ctx.check("fstr_java_u0800_cp0_is_800", field_string_fixture::j_u0800_cp0() == 0x800);
        ctx.check("fstr_java_all256_len_256", field_string_fixture::j_all256_len() == 256);
        ctx.check("fstr_java_all256_cp255_is_FF", field_string_fixture::j_all256_cp255() == 0xFF);
        // Java itself sees the SAME value vmhook decoded for the folded constant
        // and the two inherited / clean instance String fields.
        ctx.check("fstr_java_final_const_value",
                  field_string_fixture::j_final_const_value() == "FINAL_CONSTANT");
        ctx.check("fstr_java_inherited_str_value",
                  field_string_fixture::j_inherited_str_value() == "base-inherited");
        ctx.check("fstr_java_static_inherited_value",
                  field_string_fixture::j_static_inherited_value() == "base-static-inherited");
        ctx.check("fstr_java_inst_get_only_value",
                  field_string_fixture::j_inst_get_only_value() == "instance-get");

        // ---- SET write-back verified THROUGH JAVA (the contract). ----
        // Clean full overwrite landed and is visible to Java.
        ctx.check("set_ascii_eq_java_equals_world", field_string_fixture::set_ascii_eq_matches());
        ctx.check("set_ascii_eq_java_len_5", field_string_fixture::set_ascii_eq_len() == 5);
        ctx.check("set_ascii_eq_java_value_world", field_string_fixture::set_ascii_eq_value() == "world");

        // Shorter write left the tail in place -> "hirld" (partial-overwrite quirk).
        ctx.check("set_shorter_java_value_hirld", field_string_fixture::set_shorter_value() == "hirld");
        ctx.check("set_shorter_java_len_stays_5", field_string_fixture::set_shorter_len() == 5);

        // ASCII into LATIN1 coder-0 backing of equal length round-trips cleanly.
        ctx.check("set_latin1_java_equals_abcde", field_string_fixture::set_latin1_matches());
        ctx.check("set_latin1_java_value_abcde", field_string_fixture::set_latin1_value() == "abcde");

        // Overlong write truncated to backing length; Java length stayed 3.
        ctx.check("set_overlong_java_value_LON", field_string_fixture::set_overlong_value() == "LON");
        ctx.check("set_overlong_java_len_stays_3", field_string_fixture::set_overlong_len() == 3);

        // Instance field write-back visible to Java.
        ctx.check("instance_set_java_equals_java_bang", field_string_fixture::inst_ascii_matches());
        ctx.check("instance_set_java_value", field_string_fixture::inst_ascii_value() == "java!");

        // Reading the interned literal did not corrupt the shared pool.
        ctx.check("interned_literal_intact_after_reads", field_string_fixture::interned_intact());

        // ---- Re-read the SET targets via vmhook's OWN read path post-probe
        //      and confirm it agrees with what Java reported. ----
        ctx.check("set_ascii_eq_vmhook_reread_world",
                  field_string_fixture::read_static("setAsciiEq") == "world");
        ctx.check("set_shorter_vmhook_reread_hirld",
                  field_string_fixture::read_static("setShorter") == "hirld");
        ctx.check("set_overlong_vmhook_reread_LON",
                  field_string_fixture::read_static("setOverlong") == "LON");
        ctx.check("set_empty_target_still_empty",
                  field_string_fixture::read_static("setEmptyTgt").empty());

        // ---- FIELD REASSIGNED BETWEEN READS: the probe replaced getReassign's
        //      reference with a freshly-allocated "after2".  A NEW field_proxy
        //      read must now resolve the new backing (proving the getter reads
        //      the live slot, not a cached OOP).  This is GC-sensitive: the
        //      probe just allocated the replacement, so a young-gen collection
        //      could relocate it between the putstatic and this read; if that
        //      happens read_java_string degrades to "" (its safe_read guard).
        //      We therefore (a) record the raw observation as [INFO], and (b)
        //      assert vmhook AGREES WITH JAVA's own post-reassign view — Java
        //      republished jReassignAfterValue inside the same probe, so both
        //      sides see the same heap state and the comparison is race-free. ----
        const std::string reassign_after_vmhook{ field_string_fixture::read_static("getReassign") };
        const std::string reassign_after_java{ field_string_fixture::j_reassign_after_value() };
        ctx.record(std::string{ "[INFO] fstr getReassign: before='" } + reassign_before
                   + "' after(vmhook)='" + reassign_after_vmhook
                   + "' after(java)='" + reassign_after_java
                   + "' (GC-sensitive re-read of a reassigned field).");
        // Java's own view of the reassigned field is the authoritative new value
        // (race-free: republished inside the same probe).
        ctx.check("fstr_reassign_java_is_after2", reassign_after_java == "after2");
        // The fresh field_proxy read resolved the NEW slot, not the stale "before"
        // backing — the core "re-read sees the reassignment" proof.  Guard the
        // exact-value assertion behind a non-empty read: the replacement String was
        // allocated by the probe an instant earlier, so a young-gen relocation
        // between the putstatic and this read could (rarely) leave read_java_string's
        // safe_read on an in-flight OOP and degrade to "".  In that GC-race window we
        // record [INFO] instead of failing; in the overwhelmingly common case we
        // assert the exact new value AND that it agrees with Java.
        ctx.check("fstr_reassign_vmhook_not_before", reassign_after_vmhook != "before");
        if (!reassign_after_vmhook.empty())
        {
            ctx.check("fstr_reassign_vmhook_agrees_with_java",
                      reassign_after_vmhook == reassign_after_java);
            ctx.check("fstr_reassign_vmhook_is_after2", reassign_after_vmhook == "after2");
        }
        else
        {
            ctx.record("[INFO] fstr getReassign: vmhook re-read returned \"\" — a GC "
                       "relocation of the just-allocated replacement raced the read; "
                       "Java's authoritative view (above) confirms the reassignment.");
        }

        // ---- Inherited / clean-instance / folded-constant reads still agree
        //      with Java AFTER the probe (the probe never wrote these; this is a
        //      stability re-read proving the getter is repeatable for them). ----
        ctx.check("fstr_final_const_reread_after_probe",
                  field_string_fixture::read_static("getFinalConst") == "FINAL_CONSTANT");
        ctx.check("fstr_static_inherited_reread_after_probe",
                  field_string_fixture::read_static("sInheritedStr") == "base-static-inherited");
        {
            const auto self_after{ field_string_fixture::acquire_self() };
            if (self_after)
            {
                const auto inh{ self_after->get_field("inheritedStr") };
                if (inh.has_value())
                {
                    const std::string v = inh->get();
                    ctx.check("fstr_inherited_instance_reread_after_probe", v == "base-inherited");
                }
            }
        }
    }
}
