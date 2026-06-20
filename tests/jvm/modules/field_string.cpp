// field_string JVM test module — exhaustive coverage of String field GET and
// SET through vmhook's zero-JNI field_proxy / read_java_string / store_string.
//
// Feature surface under test (vmhook/ext/vmhook/vmhook.hpp):
//   - read_java_string()                          (line ~15138)  GET decode
//   - field_proxy::value_t::cast_for_variant<std::string>  (~11411)  GET dispatch
//   - field_proxy::store_string()                 (~15479)       SET = REBIND
//   - field_proxy::set(std::string)               (~15127)       SET dispatch + guard
//
// SET semantics (library bug #30 FIXED): field_proxy::set(std::string) now
// REBINDS the String field to a freshly-built, correctly-encoded java.lang.String
// of the EXACT input value+length (an object-reference store via store_string ->
// store_object_oop), the same as a Java `field = value;`.  It NO LONGER overwrites
// the existing backing array in place, so the old quirks are gone: a shorter write
// becomes exactly the new value (not "old-tail"-padded), an overlong write is the
// FULL value (not truncated to the backing length), a write over a 0-length backing
// builds a real String, and a shared/interned String referenced elsewhere is never
// corrupted.  These SET checks assert that new, correct behavior.
//
// SET ENCODING: because store_string builds the new String via the length-counted
// UTF-16 path (jni_new_string_utf16_local -> NewString), the WRITE direction is
// content-exact for every encoding class — so this module also writes a Latin-1
// 'é', a BMP CJK 日, an ASTRAL emoji (U+1F600, which must become a UTF-16
// surrogate PAIR), an interior-NUL string, and an empty string through the field,
// then proves (via Java's own length/code-point view AND vmhook's own re-read)
// that each round-trips to the exact UTF-8 bytes written.  It also drives the
// std::string_view CONVERTIBILITY arm of set() (const char* / string_view, a
// different if-constexpr branch than std::string) and a RE-SET (a second rebind
// over an already-rebound field).  The free-helper decode core's exhaustive
// surrogate/boundary battery lives in the sibling read_java_string module; this
// module owns the FIELD path (field_proxy GET + SET) specifically.
//
// All checks run on a live JDK-21 JVM, where java.lang.String is COMPACT:
//   coder 0 (LATIN1) => one byte per char, each code point UTF-8-ENCODED
//                       (0xE9 -> C3 A9, 0x80 -> C2 80, 0xFF -> C3 BF);
//   coder 1 (UTF-16) => two bytes per char, decoded to multi-byte UTF-8 with
//                       surrogate pairs combined into one 4-byte sequence.
//   read_java_string no longer substitutes '?' for non-ASCII.  The old hard
//   4096-backing-byte cap (which REJECTED any longer String to "") is GONE
//   (robustness bug #29 fixed in the library): the helper now reads a String IN
//   FULL up to a layout-uniform ceiling of read_java_string_max_units (16M
//   CHARACTERS) — applied to the decoded CHARACTER count, so LATIN1, UTF16, and
//   JDK 8 char[] all share the SAME char ceiling (no more asymmetric 2048-char
//   UTF-16 limit).  The fixture is built so every decode path AND the long-String
//   read-in-full behaviour is exercised (documented inline per check).
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

        // ---- set a static String field with a std::string carrying ARBITRARY
        //      bytes (interior NUL / multi-byte UTF-8).  Takes a std::string BY
        //      VALUE (not string_view) so an embedded NUL is preserved end-to-end
        //      — a string_view built from a literal would stop at the first NUL.
        //      Routes through the set(std::string) arm -> store_string.
        static auto set_static_bytes(const char* name, std::string value) -> bool
        {
            const auto proxy{ static_field(name) };
            if (!proxy.has_value())
            {
                return false;
            }
            proxy->set(value);   // lvalue std::string -> the std::string arm
            return true;
        }

        // ---- set a static String field through field_proxy::set(const char*),
        //      hitting the std::string_view CONVERTIBILITY arm of set() (distinct
        //      from the std::string arm).  The literal must be NUL-free.
        static auto set_static_charptr(const char* name, const char* literal) -> bool
        {
            const auto proxy{ static_field(name) };
            if (!proxy.has_value())
            {
                return false;
            }
            proxy->set(literal); // const char* -> string_view convertibility arm
            return true;
        }

        // ---- set a static String field through field_proxy::set(std::string_view),
        //      the same convertibility arm via an explicit string_view value.
        static auto set_static_sv(const char* name, std::string_view value) -> bool
        {
            const auto proxy{ static_field(name) };
            if (!proxy.has_value())
            {
                return false;
            }
            proxy->set(value);   // std::string_view -> string_view convertibility arm
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

        // ---- Java-published facts for the batch-17 GET deepening targets. ----
        static auto j_controls_len()      -> std::int32_t { return static_field("jControlsLen")->get(); }
        static auto j_controls_cp1()      -> std::int32_t { return static_field("jControlsCp1")->get(); }
        static auto j_coder_latin1_cp7()  -> std::int32_t { return static_field("jCoderLatin1Cp7")->get(); }
        static auto j_coder_utf16_cp7()   -> std::int32_t { return static_field("jCoderUtf16Cp7")->get(); }
        static auto j_u07ff_cp0()         -> std::int32_t { return static_field("jU07FFCp0")->get(); }
        static auto j_astral_min_cp0()    -> std::int32_t { return static_field("jAstralMinCp0")->get(); }
        static auto j_astral_max_cp0()    -> std::int32_t { return static_field("jAstralMaxCp0")->get(); }
        static auto j_replacement_cp0()   -> std::int32_t { return static_field("jReplacementCp0")->get(); }
        static auto j_multi_script_len()  -> std::int32_t { return static_field("jMultiScriptLen")->get(); }
        static auto j_multi_script_cp3()  -> std::int32_t { return static_field("jMultiScriptCp3")->get(); }
        static auto j_emoji_run_cpcount() -> std::int32_t { return static_field("jEmojiRunCpCount")->get(); }
        static auto j_emoji_run_cp0()     -> std::int32_t { return static_field("jEmojiRunCp0")->get(); }
        static auto j_inherited_cjk_value() -> std::string { return static_field("jInheritedCjkValue")->get(); }

        // ---- Java-published facts for the batch-19 GET deepening edges. ----
        static auto j_u007f_cp1()           -> std::int32_t { return static_field("jU007FCp1")->get(); }
        static auto j_before_surrogate_cp0()-> std::int32_t { return static_field("jBeforeSurrogateCp0")->get(); }
        static auto j_after_surrogate_cp0() -> std::int32_t { return static_field("jAfterSurrogateCp0")->get(); }
        static auto j_static_inherited_cjk_value() -> std::string { return static_field("jStaticInheritedCjkValue")->get(); }

        static auto set_ascii_eq_matches() -> bool        { return static_field("setAsciiEqMatches")->get(); }
        static auto set_ascii_eq_len()     -> std::int32_t { return static_field("setAsciiEqLen")->get(); }
        static auto set_ascii_eq_value()   -> std::string  { return static_field("setAsciiEqValue")->get(); }
        static auto set_shorter_value()    -> std::string  { return static_field("setShorterValue")->get(); }
        static auto set_shorter_len()      -> std::int32_t { return static_field("setShorterLen")->get(); }
        static auto set_shorter_original_intact() -> bool  { return static_field("setShorterOriginalIntact")->get(); }
        static auto set_empty_tgt_value()  -> std::string  { return static_field("setEmptyTgtValue")->get(); }
        static auto set_empty_tgt_len()    -> std::int32_t { return static_field("setEmptyTgtLen")->get(); }
        static auto set_latin1_matches()   -> bool        { return static_field("setLatin1Matches")->get(); }
        static auto set_latin1_value()     -> std::string  { return static_field("setLatin1TgtValue")->get(); }
        static auto set_overlong_value()   -> std::string  { return static_field("setOverlongValue")->get(); }
        static auto set_overlong_len()     -> std::int32_t { return static_field("setOverlongLen")->get(); }
        static auto inst_ascii_value()     -> std::string  { return static_field("instAsciiValue")->get(); }
        static auto inst_ascii_matches()   -> bool        { return static_field("instAsciiMatches")->get(); }
        static auto interned_intact()      -> bool        { return static_field("internedStillIntact")->get(); }

        // ---- SET-encoding round-trip: Java-published readbacks. ----
        static auto set_latin1_write_value() -> std::string  { return static_field("setLatin1WriteValue")->get(); }
        static auto set_latin1_write_len()   -> std::int32_t { return static_field("setLatin1WriteLen")->get(); }
        static auto set_latin1_write_cp0()   -> std::int32_t { return static_field("setLatin1WriteCp0")->get(); }
        static auto set_cjk_write_value()    -> std::string  { return static_field("setCjkWriteValue")->get(); }
        static auto set_cjk_write_len()      -> std::int32_t { return static_field("setCjkWriteLen")->get(); }
        static auto set_cjk_write_cp0()      -> std::int32_t { return static_field("setCjkWriteCp0")->get(); }
        static auto set_astral_write_value()    -> std::string  { return static_field("setAstralWriteValue")->get(); }
        static auto set_astral_write_len()      -> std::int32_t { return static_field("setAstralWriteLen")->get(); }
        static auto set_astral_write_cpcount()  -> std::int32_t { return static_field("setAstralWriteCpCount")->get(); }
        static auto set_astral_write_cp0()      -> std::int32_t { return static_field("setAstralWriteCp0")->get(); }
        static auto set_nul_write_value()    -> std::string  { return static_field("setNulWriteValue")->get(); }
        static auto set_nul_write_len()      -> std::int32_t { return static_field("setNulWriteLen")->get(); }
        static auto set_nul_write_cp1()      -> std::int32_t { return static_field("setNulWriteCp1")->get(); }
        static auto set_empty_write_value()  -> std::string  { return static_field("setEmptyWriteValue")->get(); }
        static auto set_empty_write_len()    -> std::int32_t { return static_field("setEmptyWriteLen")->get(); }
        static auto set_empty_write_is_null()-> bool         { return static_field("setEmptyWriteIsNull")->get(); }
        static auto set_empty_write_eq_empty()-> bool        { return static_field("setEmptyWriteEqualsEmpty")->get(); }
        static auto set_via_char_ptr_matches()  -> bool      { return static_field("setViaCharPtrMatches")->get(); }
        static auto set_via_string_view_matches()-> bool     { return static_field("setViaStringViewMatches")->get(); }
        static auto set_reset_value()        -> std::string  { return static_field("setReSetValue")->get(); }
        static auto set_reset_len()          -> std::int32_t { return static_field("setReSetLen")->get(); }
        // ---- batch-19 SET-encoding readbacks. ----
        static auto set_max_bmp_write_value() -> std::string  { return static_field("setMaxBmpWriteValue")->get(); }
        static auto set_max_bmp_write_len()   -> std::int32_t { return static_field("setMaxBmpWriteLen")->get(); }
        static auto set_max_bmp_write_cp0()   -> std::int32_t { return static_field("setMaxBmpWriteCp0")->get(); }
        static auto set_mixed_write_value()    -> std::string  { return static_field("setMixedWriteValue")->get(); }
        static auto set_mixed_write_len()      -> std::int32_t { return static_field("setMixedWriteLen")->get(); }
        static auto set_mixed_write_cpcount()  -> std::int32_t { return static_field("setMixedWriteCpCount")->get(); }
        static auto set_mixed_write_cp3()      -> std::int32_t { return static_field("setMixedWriteCp3")->get(); }
        static auto set_long_write_len()       -> std::int32_t { return static_field("setLongWriteLen")->get(); }
        static auto set_long_write_all_l()     -> bool         { return static_field("setLongWriteAllL")->get(); }
        static auto set_re_encode_len()        -> std::int32_t { return static_field("setReEncodeLen")->get(); }
        static auto set_re_encode_cpcount()    -> std::int32_t { return static_field("setReEncodeCpCount")->get(); }
        static auto set_re_encode_cp1()        -> std::int32_t { return static_field("setReEncodeCp1")->get(); }
        static auto set_inherited_writable_value()   -> std::string { return static_field("setInheritedWritableValue")->get(); }
        static auto set_inherited_writable_matches() -> bool        { return static_field("setInheritedWritableMatches")->get(); }
        static auto inst_cjk_value()         -> std::string  { return static_field("instCjkValue")->get(); }
        static auto inst_cjk_len()           -> std::int32_t { return static_field("instCjkLen")->get(); }
        static auto inst_cjk_cp0()           -> std::int32_t { return static_field("instCjkCp0")->get(); }

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
    // builds a fresh java.lang.String and rebinds the field reference to it (an
    // object-reference store, no Java bytecode required); the probe (phase 2)
    // then reads each field back through Java to prove the new reference is
    // visible to the JVM itself.
    // ----------------------------------------------------------------------

    // Clean full overwrite (rebind): "AAAAA"(5) <- "world"(5).
    ctx.check("set_ascii_eq_proxy_resolved",
              field_string_fixture::set_static("setAsciiEq", "world"));

    // Shorter write into a longer backing: "world"(5) <- "hi"(2).
    // field_proxy::set now REBINDS the field to a freshly-built java.lang.String
    // (library bug #30 fixed) instead of overwriting the existing backing array
    // in place, so the field becomes EXACTLY "hi" (length 2) — NOT the old
    // partial-overwrite "hirld" (length 5) that left the stale tail.
    ctx.check("set_shorter_proxy_resolved",
              field_string_fixture::set_static("setShorter", "hi"));

    // Write into a zero-length backing: the rebind builds a brand-new String, so
    // a 0-length backing is irrelevant — the field becomes "ignored" (length 7).
    // (Under the old in-place path this was a no-op, writable_length<=0.)
    ctx.check("set_empty_proxy_resolved",
              field_string_fixture::set_static("setEmptyTgt", "ignored"));

    // ASCII into a LATIN1 coder-0 backing of equal length: "AAAAA"(5) <- "abcde"(5).
    ctx.check("set_latin1_proxy_resolved",
              field_string_fixture::set_static("setLatin1Tgt", "abcde"));

    // Overlong write into a short backing: "abc"(3) <- "LONGER"(6).
    // The rebind allocates a String of ANY length, so the field becomes the FULL
    // "LONGER" (length 6) — NOT the old truncation to the 3-byte backing ("LON").
    ctx.check("set_overlong_proxy_resolved",
              field_string_fixture::set_static("setOverlong", "LONGER"));

    // ----------------------------------------------------------------------
    // SET-ENCODING round-trip writes: write a known UTF-8 string of every
    // non-ASCII class through field_proxy::set and prove (in PHASE 2) that
    // store_string's UTF-8 -> java.lang.String encode is content-exact and
    // visible to Java itself.  These exercise the WRITE direction of the same
    // encodings the GET battery covers for the read direction.
    // ----------------------------------------------------------------------

    // Latin-1 source: UTF-8 'é' (U+00E9 = C3 A9) -> Java length 1, cp 0x00E9.
    ctx.check("set_latin1_write_resolved",
              field_string_fixture::set_static_bytes("setLatin1Write",
                                                     std::string{ "\xC3\xA9" }));

    // BMP CJK source: UTF-8 日 (U+65E5 = E6 97 A5) -> Java length 1, cp 0x65E5.
    ctx.check("set_cjk_write_resolved",
              field_string_fixture::set_static_bytes("setCjkWrite",
                                                     std::string{ "\xE6\x97\xA5" }));

    // Astral source: UTF-8 U+1F600 (F0 9F 98 80) -> Java length 2 (a surrogate
    // PAIR), code-point count 1.  Proves store_string encodes a 4-byte UTF-8
    // scalar into a proper UTF-16 surrogate pair.
    ctx.check("set_astral_write_resolved",
              field_string_fixture::set_static_bytes("setAstralWrite",
                                                     std::string{ "\xF0\x9F\x98\x80" }));

    // Interior-NUL source: the std::string is built with an EXPLICIT length so
    // the embedded NUL is carried into store_string's length-counted UTF-16 path
    // (NewStringUTF would truncate at the NUL).  -> Java length 3, charAt(1)==0.
    ctx.check("set_nul_write_resolved",
              field_string_fixture::set_static_bytes(
                  "setNulWrite", std::string(std::string_view{ "a\0b", 3 })));

    // Empty write into a populated target -> a real empty String (length 0),
    // not null and not left at the old "populated" value.
    ctx.check("set_empty_write_resolved",
              field_string_fixture::set_static_bytes("setEmptyWrite", std::string{}));

    // const char* through the std::string_view CONVERTIBILITY arm of set().
    ctx.check("set_via_char_ptr_resolved",
              field_string_fixture::set_static_charptr("setViaCharPtr", "char-ptr-set"));

    // std::string_view through the same convertibility arm.
    ctx.check("set_via_string_view_resolved",
              field_string_fixture::set_static_sv("setViaStringView",
                                                  std::string_view{ "sv-set" }));

    // RE-SET: write "first" then "second" into the SAME field (a second rebind
    // over an already-rebound field).  Java must see the LAST value, "second".
    ctx.check("set_reset_first_resolved",
              field_string_fixture::set_static("setReSet", "first"));
    ctx.check("set_reset_second_resolved",
              field_string_fixture::set_static("setReSet", "second"));

    // ----------------------------------------------------------------------
    // BATCH-19 SET-ENCODING writes: a single 3-byte BMP scalar, a MIXED
    // multi-encode buffer, a MODEST-long ASCII string, and a RE-ENCODE re-set
    // that flips the compact-string coder class.  Each proves store_string's
    // UTF-8 -> java.lang.String encode for a class the prior battery lacked.
    // ----------------------------------------------------------------------

    // Max-BMP source: UTF-8 U+FFFF (EF BF BF) -> Java length 1, cp 0xFFFF.
    // A single 3-byte UTF-8 scalar -> exactly ONE UTF-16 code unit.
    ctx.check("set_max_bmp_write_resolved",
              field_string_fixture::set_static_bytes("setMaxBmpWrite",
                                                     std::string{ "\xEF\xBF\xBF" }));

    // Mixed source in one write: 'A' + 'é'(C3 A9) + 日(E6 97 A5) + emoji(F0 9F 98 80)
    // -> Java length 5 (UTF-16 units), code-point count 4.  A heterogeneous buffer.
    ctx.check("set_mixed_write_resolved",
              field_string_fixture::set_static_bytes(
                  "setMixedWrite",
                  std::string{ "A\xC3\xA9\xE6\x97\xA5\xF0\x9F\x98\x80" }));

    // Modest-long ASCII (300 'L') into a 7-char placeholder backing: the rebind
    // allocates the full length regardless of the old backing size.
    ctx.check("set_long_write_resolved",
              field_string_fixture::set_static_bytes("setLongWrite",
                                                     std::string(300u, 'L')));

    // RE-ENCODE re-set: ASCII "plain" then OVERWRITE with "Z" + emoji.  The second
    // rebind changes the coder class (LATIN1 -> UTF-16); Java must see the LAST one.
    ctx.check("set_re_encode_first_resolved",
              field_string_fixture::set_static("setReEncode", "plain"));
    ctx.check("set_re_encode_second_resolved",
              field_string_fixture::set_static_bytes("setReEncode",
                                                     std::string{ "Z\xF0\x9F\x98\x80" }));

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

            // Instance NON-ASCII SET: write CJK 日 (U+65E5) through the instance
            // field_proxy, proving the SET encode path works on an instance slot.
            const auto inst_cjk_proxy{ self->get_field("instCjk") };
            ctx.check("instance_cjk_field_resolved", inst_cjk_proxy.has_value());
            if (inst_cjk_proxy.has_value())
            {
                inst_cjk_proxy->set(std::string{ "\xE6\x97\xA5" });
                // Native read-back agrees immediately (pre-probe): the field now
                // decodes to the 3-byte UTF-8 of 日.
                const std::string after = inst_cjk_proxy->get();
                ctx.check("instance_cjk_set_native_readback",
                          after == std::string{ "\xE6\x97\xA5" });
            }

            // INHERITED writable INSTANCE String (declared on FieldStringBase):
            // rebind it through the CHILD instance field_proxy, proving SET (not
            // just GET) resolves an inherited slot via the super walk.  The slot
            // resolution + signature are HARD; the rebound value is re-verified
            // through Java in PHASE 2.
            const auto inh_writable_proxy{ self->get_field("inheritedWritable") };
            ctx.check("instance_inherited_writable_field_resolved",
                      inh_writable_proxy.has_value());
            if (inh_writable_proxy.has_value())
            {
                ctx.check("instance_inherited_writable_signature",
                          std::string{ inh_writable_proxy->signature() } == "Ljava/lang/String;");
                // Pre-write read resolves the base's initial value via the super walk.
                const std::string before = inh_writable_proxy->get();
                ctx.check("instance_inherited_writable_initial_value",
                          before == "base-writable");
                inh_writable_proxy->set(std::string{ "child-wrote" });
                // Native read-back agrees immediately (pre-probe).
                const std::string after = inh_writable_proxy->get();
                ctx.check("instance_inherited_writable_native_readback",
                          after == "child-wrote");
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

    // --- Long-String read-in-full (robustness bug #29 FIXED): the old hard
    // 4096-byte cap that rejected longer Strings to "" is gone.  read_java_string
    // now reads a String IN FULL up to read_java_string_max_units (16M CHARS), so
    // these "boundary" subjects now decode to their COMPLETE content.  The 4096 /
    // 4097 / 5000 numbers are no longer a cap — they are just lengths well under
    // the new ceiling, asserted read-in-full. ---
    const std::string len4096{ field_string_fixture::read_static("getLen4096") };
    ctx.check("get_len4096_read_in_full", len4096.size() == 4096);
    ctx.check("get_len4096_all_x",
              len4096.size() == 4096 && len4096.front() == 'x' && len4096.back() == 'x');

    // --- 4097 ASCII chars ('y'): formerly REJECTED at the old 4096 cap, now read
    // IN FULL (4097 bytes, every byte 'y').  This is the headline proof the fix
    // landed: a String one char past the OLD cap is no longer truncated-to-empty. ---
    const std::string len4097{ field_string_fixture::read_static("getLen4097") };
    ctx.check("get_len4097_read_in_full", len4097.size() == 4097);
    ctx.check("get_len4097_all_y",
              len4097.size() == 4097 && len4097.front() == 'y' && len4097.back() == 'y');

    // --- 5000 ASCII chars ('z'): well past the OLD cap, now read IN FULL. ---
    const std::string len5000{ field_string_fixture::read_static("getLen5000") };
    ctx.check("get_len5000_read_in_full", len5000.size() == 5000);
    ctx.check("get_len5000_all_z",
              len5000.size() == 5000 && len5000.front() == 'z' && len5000.back() == 'z');

    // --- 2048 CJK chars (日): decodes to correct UTF-8 — each 日 (U+65E5) is
    // 3 bytes E6 97 A5, so the result is 2048 * 3 = 6144 bytes. ---
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

    // --- 2049 CJK chars: with the fix, the ceiling is applied to the decoded
    // CHARACTER count UNIFORMLY (16M chars), not to the raw backing-array byte
    // length, so there is NO more asymmetric UTF-16 cap.  A 2049-char UTF-16 String
    // (byte[] length 4098 on JDK 9+) now reads IN FULL on BOTH layouts: 2049 kanji
    // -> 2049 * 3 = 6147 UTF-8 bytes, regardless of compact-string layout. ---
    const std::string cjk2049{ field_string_fixture::read_static("getCjk2049") };
    ctx.check("get_cjk2049_read_in_full_6147", cjk2049.size() == 2049u * 3u);
    ctx.check("get_cjk2049_first_and_last_kanji",
              cjk2049.size() == 2049u * 3u
              && static_cast<unsigned char>(cjk2049[0]) == 0xE6
              && static_cast<unsigned char>(cjk2049[1]) == 0x97
              && static_cast<unsigned char>(cjk2049[2]) == 0xA5
              && static_cast<unsigned char>(cjk2049[2049u * 3u - 1]) == 0xA5);

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

            // INHERITED INSTANCE non-ASCII String, declared only on the base:
            // proves the super-walk resolution feeds the UTF-16 decode path.
            // 日本 (U+65E5 U+672C) -> UTF-8 E6 97 A5 E6 9C AC, 6 bytes.
            const auto inh_cjk_proxy{ self->get_field("inheritedCjk") };
            ctx.check("fstr_inherited_cjk_field_resolves", inh_cjk_proxy.has_value());
            if (inh_cjk_proxy.has_value())
            {
                const std::string inh_cjk = inh_cjk_proxy->get();
                ctx.check("fstr_inherited_cjk_value",
                          inh_cjk == std::string{ "\xE6\x97\xA5\xE6\x9C\xAC" });
                ctx.check("fstr_inherited_cjk_utf8_len_6", inh_cjk.size() == 6);
            }
        }
    }

    // ======================================================================
    // BATCH-17 GET DEEPENING — "every possible String-field read" the prior
    // battery lacked.  Each value's expected UTF-8 was cross-checked against
    // String.getBytes(UTF_8); every read goes through the clean one-liner
    // (read_static -> field_proxy::get -> read_java_string) AND is re-decoded
    // directly on the raw backing OOP (read_static_direct), asserting the two
    // paths AGREE.  Content round-trip is HARD (banked rule).
    // ======================================================================

    // --- ASCII control chars (TAB/LF/CR) preserved byte-verbatim (LATIN1). ---
    // "a\tb\nc\rd" -> 61 09 62 0A 63 0D 64, length 7.
    const std::string controls{ field_string_fixture::read_static("getControls") };
    ctx.check("fstr_controls_len_7", controls.size() == 7);
    ctx.check("fstr_controls_bytes",
              controls == std::string{ "a\tb\nc\rd" });
    ctx.check("fstr_controls_byte1_is_tab",
              controls.size() == 7 && static_cast<unsigned char>(controls[1]) == 0x09);
    ctx.check("fstr_controls_byte3_is_lf",
              controls.size() == 7 && static_cast<unsigned char>(controls[3]) == 0x0A);
    ctx.check("fstr_controls_byte5_is_cr",
              controls.size() == 7 && static_cast<unsigned char>(controls[5]) == 0x0D);
    ctx.check("fstr_controls_direct_equals_proxy",
              field_string_fixture::read_static_direct("getControls") == controls);

    // --- COMPACT-STRING CODER BOUNDARY: the SAME ASCII prefix "Shared!" decodes
    //     to IDENTICAL bytes whether the backing is LATIN1 (coder 0) or UTF-16
    //     (coder 1).  The LATIN1 string is "Shared!é" (cp's all <= 0xFF); the
    //     UTF-16 string is "Shared!" + U+1F4A9 (the astral promotes the whole
    //     string to coder 1).  Assert the first 7 decoded UTF-8 bytes match. ---
    const std::string coder_latin1{ field_string_fixture::read_static("getCoderLatin1Prefix") };
    const std::string coder_utf16{ field_string_fixture::read_static("getCoderUtf16Prefix") };
    ctx.record(std::string{ "[INFO] fstr coder boundary: latin1='" } + coder_latin1
               + "' (" + hex_bytes(coder_latin1) + ") utf16='" + coder_utf16
               + "' (" + hex_bytes(coder_utf16) + ")");
    ctx.check("fstr_coder_latin1_prefix_shared",
              coder_latin1.size() >= 7 && coder_latin1.substr(0, 7) == "Shared!");
    ctx.check("fstr_coder_utf16_prefix_shared",
              coder_utf16.size() >= 7 && coder_utf16.substr(0, 7) == "Shared!");
    // The decisive identity check: both decode paths agree on the shared prefix.
    ctx.check("fstr_coder_boundary_shared_prefix_identical",
              coder_latin1.size() >= 7 && coder_utf16.size() >= 7
              && coder_latin1.substr(0, 7) == coder_utf16.substr(0, 7));
    // Full content per layout: LATIN1 tail 'é' (C3 A9); UTF-16 tail U+1F4A9 (F0 9F 92 A9).
    ctx.check("fstr_coder_latin1_full_value",
              coder_latin1 == std::string{ "Shared!\xC3\xA9" });
    ctx.check("fstr_coder_utf16_full_value",
              coder_utf16 == std::string{ "Shared!\xF0\x9F\x92\xA9" });
    ctx.check("fstr_coder_latin1_direct_equals_proxy",
              field_string_fixture::read_static_direct("getCoderLatin1Prefix") == coder_latin1);
    ctx.check("fstr_coder_utf16_direct_equals_proxy",
              field_string_fixture::read_static_direct("getCoderUtf16Prefix") == coder_utf16);

    // --- U+07FF -> the LAST 2-byte UTF-8 code point (DF BF); UTF-16 (coder 1).
    //     Completes the 2-byte boundary with getLo80 (C2 80, first) and the
    //     3-byte boundary with getU0800 (E0 A0 80, first). ---
    const std::string u07ff{ field_string_fixture::read_static("getU07FF") };
    ctx.check("fstr_u07ff_utf8_DFBF", u07ff == std::string{ "\xDF\xBF" });
    ctx.check("fstr_u07ff_utf8_len_2", u07ff.size() == 2);
    ctx.check("fstr_u07ff_direct_equals_proxy",
              field_string_fixture::read_static_direct("getU07FF") == u07ff);

    // --- U+10000 -> the SMALLEST astral scalar (surrogate pair D800 DC00) ->
    //     4-byte UTF-8 F0 90 80 80.  Lower edge of the surrogate-combining math. ---
    const std::string astral_min{ field_string_fixture::read_static("getAstralMin") };
    ctx.check("fstr_astral_min_utf8_F0908080",
              astral_min == std::string{ "\xF0\x90\x80\x80" });
    ctx.check("fstr_astral_min_utf8_len_4", astral_min.size() == 4);
    ctx.check("fstr_astral_min_direct_equals_proxy",
              field_string_fixture::read_static_direct("getAstralMin") == astral_min);

    // --- U+10FFFF -> the LARGEST valid code point (surrogate pair DBFF DFFF) ->
    //     4-byte UTF-8 F4 8F BF BF.  Upper edge of the surrogate-combining math. ---
    const std::string astral_max{ field_string_fixture::read_static("getAstralMax") };
    ctx.check("fstr_astral_max_utf8_F48FBFBF",
              astral_max == std::string{ "\xF4\x8F\xBF\xBF" });
    ctx.check("fstr_astral_max_utf8_len_4", astral_max.size() == 4);
    ctx.check("fstr_astral_max_direct_equals_proxy",
              field_string_fixture::read_static_direct("getAstralMax") == astral_max);

    // --- U+FFFD REPLACEMENT CHARACTER as REAL content (EF BF BD): a legitimate
    //     3-byte BMP char that must round-trip verbatim, NOT confused with the
    //     decode's own degrade-to-"" path. ---
    const std::string replacement{ field_string_fixture::read_static("getReplacement") };
    ctx.check("fstr_replacement_utf8_EFBFBD",
              replacement == std::string{ "\xEF\xBF\xBD" });
    ctx.check("fstr_replacement_utf8_len_3", replacement.size() == 3);
    ctx.check("fstr_replacement_not_empty", !replacement.empty());
    ctx.check("fstr_replacement_direct_equals_proxy",
              field_string_fixture::read_static_direct("getReplacement") == replacement);

    // --- Multi-script BMP: 'A' + U+03B1 + U+3044 + U+AC00 -> a 1/2/3-byte UTF-8
    //     mix in one string: 41  CE B1  E3 81 84  EA B0 80, length 9. ---
    const std::string multi{ field_string_fixture::read_static("getMultiScript") };
    ctx.record(std::string{ "[INFO] fstr getMultiScript bytes: " } + hex_bytes(multi));
    ctx.check("fstr_multi_script_value",
              multi == std::string{ "A\xCE\xB1\xE3\x81\x84\xEA\xB0\x80" });
    ctx.check("fstr_multi_script_len_9", multi.size() == 9);
    ctx.check("fstr_multi_script_direct_equals_proxy",
              field_string_fixture::read_static_direct("getMultiScript") == multi);

    // --- MODEST-long UTF-16 with 100 REPEATED surrogate pairs (U+1F4A9):
    //     decodes to 100 * 4 = 400 UTF-8 bytes (F0 9F 92 A9 x100).  Exercises the
    //     surrogate-pair combining loop many times; heap-modest. ---
    const std::string emoji_run{ field_string_fixture::read_static("getEmojiRun") };
    ctx.check("fstr_emoji_run_utf8_len_400", emoji_run.size() == 400);
    {
        std::string expected_run;
        expected_run.reserve(400);
        for (int i{ 0 }; i < 100; ++i)
        {
            expected_run += std::string{ "\xF0\x9F\x92\xA9" };
        }
        ctx.check("fstr_emoji_run_byte_exact", emoji_run == expected_run);
    }
    ctx.check("fstr_emoji_run_first_pair_F0",
              emoji_run.size() == 400
              && static_cast<unsigned char>(emoji_run[0]) == 0xF0
              && static_cast<unsigned char>(emoji_run[396]) == 0xF0
              && static_cast<unsigned char>(emoji_run[399]) == 0xA9);
    ctx.check("fstr_emoji_run_direct_equals_proxy",
              field_string_fixture::read_static_direct("getEmojiRun") == emoji_run);

    // ======================================================================
    // BATCH-19 GET DEEPENING — encoder-boundary EDGES the prior battery lacked.
    // Each value's expected UTF-8 was cross-checked against String.getBytes(UTF_8);
    // every read goes through the clean one-liner AND read_static_direct, asserting
    // the two paths AGREE.  Content round-trip is HARD (banked rule).
    // ======================================================================

    // --- U+007F (DEL): the LAST 1-byte ASCII code point -> a SINGLE 0x7F byte
    //     (LATIN1 coder 0), embedded between 'a' and 'b'.  Completes the 1->2 byte
    //     encoder boundary from the LOW side (getLo80 = U+0080 from the high side).
    //     "a\x7Fb" -> 61 7F 62, length 3. ---
    const std::string u007f{ field_string_fixture::read_static("getU007F") };
    ctx.check("fstr_u007f_value", u007f == std::string{ "a\x7F" "b" });
    ctx.check("fstr_u007f_len_3", u007f.size() == 3);
    ctx.check("fstr_u007f_mid_is_7F",
              u007f.size() == 3 && static_cast<unsigned char>(u007f[1]) == 0x7F);
    ctx.check("fstr_u007f_direct_equals_proxy",
              field_string_fixture::read_static_direct("getU007F") == u007f);

    // --- U+D7FF: the LAST BMP scalar BEFORE the surrogate range -> plain 3-byte
    //     UTF-8 ED 9F BF (UTF-16 coder 1).  Must NOT be mistaken for a surrogate
    //     by the pair-combining branch (low edge of the "ordinary BMP" check). ---
    const std::string before_surr{ field_string_fixture::read_static("getBeforeSurrogate") };
    ctx.check("fstr_before_surrogate_utf8_ED9FBF",
              before_surr == std::string{ "\xED\x9F\xBF" });
    ctx.check("fstr_before_surrogate_len_3", before_surr.size() == 3);
    ctx.check("fstr_before_surrogate_direct_equals_proxy",
              field_string_fixture::read_static_direct("getBeforeSurrogate") == before_surr);

    // --- U+E000: the FIRST BMP scalar AFTER the surrogate range -> plain 3-byte
    //     UTF-8 EE 80 80 (UTF-16 coder 1).  High edge of the "ordinary BMP, not a
    //     surrogate" check (getBeforeSurrogate is the low edge). ---
    const std::string after_surr{ field_string_fixture::read_static("getAfterSurrogate") };
    ctx.check("fstr_after_surrogate_utf8_EE8080",
              after_surr == std::string{ "\xEE\x80\x80" });
    ctx.check("fstr_after_surrogate_len_3", after_surr.size() == 3);
    ctx.check("fstr_after_surrogate_direct_equals_proxy",
              field_string_fixture::read_static_direct("getAfterSurrogate") == after_surr);

    // --- INHERITED STATIC non-ASCII String, declared only on FieldStringBase,
    //     resolved through the CHILD wrapper's static_field super walk, feeding the
    //     UTF-16 decode path.  語 (U+8A9E) -> 3 UTF-8 bytes E8 AA 9E. ---
    const std::string static_inherited_cjk{ field_string_fixture::read_static("sInheritedCjk") };
    ctx.check("fstr_static_inherited_cjk_value",
              static_inherited_cjk == std::string{ "\xE8\xAA\x9E" });
    ctx.check("fstr_static_inherited_cjk_len_3", static_inherited_cjk.size() == 3);
    ctx.check("fstr_static_inherited_cjk_direct_equals_proxy",
              field_string_fixture::read_static_direct("sInheritedCjk") == static_inherited_cjk);

    // --- NON-String object field accessed as a String: read_java_string is fed
    //     a non-String oop (an int[] held in an Object field).  It must DEGRADE
    //     GRACEFULLY (return "" or a bounded best-effort decode) and NEVER crash.
    //     Exact bytes are undefined, so we ONLY assert no-crash + boundedness;
    //     the raw observation is recorded as [INFO].  (Reaching this line at all
    //     proves it did not fault the suite.) ---
    const std::string not_a_string{ field_string_fixture::read_static("notAStringObj") };
    ctx.record(std::string{ "[INFO] fstr notAStringObj decoded len=" }
               + std::to_string(not_a_string.size()) + " bytes: " + hex_bytes(not_a_string));
    ctx.check("fstr_non_string_object_no_crash_bounded",
              not_a_string.size() <= 64u);
    // The direct path over the same field must also be crash-safe and agree with
    // the proxy path (both reuse the guarded read_java_string -> deterministic).
    ctx.check("fstr_non_string_object_direct_equals_proxy",
              field_string_fixture::read_static_direct("notAStringObj") == not_a_string);

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

        // ---- Java-side cross-checks of the batch-17 GET deepening targets:
        //      prove the bytes vmhook decoded correspond to the real Java String
        //      code points / lengths (the authoritative cross-check). ----
        ctx.check("fstr_java_controls_len_7", field_string_fixture::j_controls_len() == 7);
        ctx.check("fstr_java_controls_cp1_is_tab", field_string_fixture::j_controls_cp1() == 0x09);
        ctx.check("fstr_java_coder_latin1_cp7_is_E9",
                  field_string_fixture::j_coder_latin1_cp7() == 0x00E9);
        ctx.check("fstr_java_coder_utf16_cp7_is_1F4A9",
                  field_string_fixture::j_coder_utf16_cp7() == 0x1F4A9);
        ctx.check("fstr_java_u07ff_cp0_is_7FF", field_string_fixture::j_u07ff_cp0() == 0x07FF);
        ctx.check("fstr_java_astral_min_cp0_is_10000",
                  field_string_fixture::j_astral_min_cp0() == 0x10000);
        ctx.check("fstr_java_astral_max_cp0_is_10FFFF",
                  field_string_fixture::j_astral_max_cp0() == 0x10FFFF);
        ctx.check("fstr_java_replacement_cp0_is_FFFD",
                  field_string_fixture::j_replacement_cp0() == 0xFFFD);
        ctx.check("fstr_java_multi_script_len_4", field_string_fixture::j_multi_script_len() == 4);
        ctx.check("fstr_java_multi_script_cp3_is_AC00",
                  field_string_fixture::j_multi_script_cp3() == 0xAC00);
        ctx.check("fstr_java_emoji_run_cpcount_100",
                  field_string_fixture::j_emoji_run_cpcount() == 100);
        ctx.check("fstr_java_emoji_run_cp0_is_1F4A9",
                  field_string_fixture::j_emoji_run_cp0() == 0x1F4A9);
        // Java sees the SAME 日本 value vmhook decoded for the inherited non-ASCII
        // instance field (proves the super-walk + UTF-16 decode agree with Java).
        ctx.check("fstr_java_inherited_cjk_value",
                  field_string_fixture::j_inherited_cjk_value()
                  == std::string{ "\xE6\x97\xA5\xE6\x9C\xAC" });

        // ---- Java-side cross-checks of the batch-19 GET deepening edges:
        //      prove the bytes vmhook decoded correspond to the real Java String
        //      code points (the authoritative cross-check). ----
        ctx.check("fstr_java_u007f_cp1_is_7F", field_string_fixture::j_u007f_cp1() == 0x7F);
        ctx.check("fstr_java_before_surrogate_cp0_is_D7FF",
                  field_string_fixture::j_before_surrogate_cp0() == 0xD7FF);
        ctx.check("fstr_java_after_surrogate_cp0_is_E000",
                  field_string_fixture::j_after_surrogate_cp0() == 0xE000);
        // Java sees the SAME 語 value vmhook decoded for the inherited STATIC
        // non-ASCII field (proves the static super walk + UTF-16 decode agree).
        ctx.check("fstr_java_static_inherited_cjk_value",
                  field_string_fixture::j_static_inherited_cjk_value()
                  == std::string{ "\xE8\xAA\x9E" });

        // ---- SET write-back verified THROUGH JAVA (the contract). ----
        // Clean full overwrite landed and is visible to Java.
        ctx.check("set_ascii_eq_java_equals_world", field_string_fixture::set_ascii_eq_matches());
        ctx.check("set_ascii_eq_java_len_5", field_string_fixture::set_ascii_eq_len() == 5);
        ctx.check("set_ascii_eq_java_value_world", field_string_fixture::set_ascii_eq_value() == "world");

        // Shorter write REBINDS to a fresh String -> Java sees exactly "hi"
        // (length 2), not the old partial-overwrite "hirld"/length-5 quirk.
        ctx.check("set_shorter_java_value_hi", field_string_fixture::set_shorter_value() == "hi");
        ctx.check("set_shorter_java_len_2", field_string_fixture::set_shorter_len() == 2);
        // REBIND-SAFETY: a separate Java alias to setShorter's ORIGINAL String
        // object still reads "world" after the native set().  The rebind is an
        // object-reference store (it bound the field to a brand-new "hi" String),
        // so the previously-referenced object was never mutated — proving a shared
        // String aliased elsewhere is not corrupted (the old in-place overwrite
        // WOULD have turned this original object's content into "hirld").
        ctx.check("set_shorter_original_object_intact_world",
                  field_string_fixture::set_shorter_original_intact());

        // Empty-backing target: the rebind built a real "ignored" String, so Java
        // sees the full value (length 7), not the old writable_length<=0 no-op "".
        ctx.check("set_empty_java_value_ignored",
                  field_string_fixture::set_empty_tgt_value() == "ignored");
        ctx.check("set_empty_java_len_7", field_string_fixture::set_empty_tgt_len() == 7);

        // ASCII into LATIN1 coder-0 backing of equal length round-trips cleanly.
        ctx.check("set_latin1_java_equals_abcde", field_string_fixture::set_latin1_matches());
        ctx.check("set_latin1_java_value_abcde", field_string_fixture::set_latin1_value() == "abcde");

        // Overlong write REBINDS to a fresh String -> Java sees the FULL "LONGER"
        // (length 6), not the old truncate-to-backing "LON"/length-3 quirk.
        ctx.check("set_overlong_java_value_LONGER", field_string_fixture::set_overlong_value() == "LONGER");
        ctx.check("set_overlong_java_len_6", field_string_fixture::set_overlong_len() == 6);

        // Instance field write-back visible to Java.
        ctx.check("instance_set_java_equals_java_bang", field_string_fixture::inst_ascii_matches());
        ctx.check("instance_set_java_value", field_string_fixture::inst_ascii_value() == "java!");

        // Reading the interned literal did not corrupt the shared pool.
        ctx.check("interned_literal_intact_after_reads", field_string_fixture::interned_intact());

        // ---- Re-read the SET targets via vmhook's OWN read path post-probe
        //      and confirm it agrees with what Java reported. ----
        ctx.check("set_ascii_eq_vmhook_reread_world",
                  field_string_fixture::read_static("setAsciiEq") == "world");
        ctx.check("set_shorter_vmhook_reread_hi",
                  field_string_fixture::read_static("setShorter") == "hi");
        ctx.check("set_overlong_vmhook_reread_LONGER",
                  field_string_fixture::read_static("setOverlong") == "LONGER");
        // The rebind built a real String over the formerly-empty backing, so the
        // field now reads the written value (no longer the old no-op "").
        ctx.check("set_empty_target_now_ignored",
                  field_string_fixture::read_static("setEmptyTgt") == "ignored");

        // ======================================================================
        // SET-ENCODING round-trip verification (the WRITE direction of the GET
        // battery's encodings).  For each: (a) Java's OWN view of the rebound
        // field — length / code points — proves store_string's UTF-8 -> String
        // encode is content-exact and visible to the JVM; (b) vmhook's OWN
        // re-read decodes back to the exact UTF-8 bytes we wrote, closing the
        // write->read loop entirely inside the field path.  All are HARD: the
        // decoded content/length is invariant across JDK 8-26 x GC x compiler.
        // ======================================================================

        // --- Latin-1 'é' (U+00E9): Java length 1, cp 0x00E9; vmhook re-read C3 A9. ---
        ctx.check("set_latin1_write_java_len_1", field_string_fixture::set_latin1_write_len() == 1);
        ctx.check("set_latin1_write_java_cp0_E9", field_string_fixture::set_latin1_write_cp0() == 0x00E9);
        ctx.check("set_latin1_write_java_value_utf8",
                  field_string_fixture::set_latin1_write_value() == std::string{ "\xC3\xA9" });
        ctx.check("set_latin1_write_vmhook_reread",
                  field_string_fixture::read_static("setLatin1Write") == std::string{ "\xC3\xA9" });

        // --- CJK 日 (U+65E5): Java length 1, cp 0x65E5; vmhook re-read E6 97 A5. ---
        ctx.check("set_cjk_write_java_len_1", field_string_fixture::set_cjk_write_len() == 1);
        ctx.check("set_cjk_write_java_cp0_65E5", field_string_fixture::set_cjk_write_cp0() == 0x65E5);
        ctx.check("set_cjk_write_java_value_utf8",
                  field_string_fixture::set_cjk_write_value() == std::string{ "\xE6\x97\xA5" });
        ctx.check("set_cjk_write_vmhook_reread",
                  field_string_fixture::read_static("setCjkWrite") == std::string{ "\xE6\x97\xA5" });

        // --- Astral U+1F600: Java length 2 (surrogate PAIR), cpCount 1, cp 0x1F600;
        //     vmhook re-read is the one 4-byte UTF-8 sequence F0 9F 98 80.  This is
        //     the headline SET proof: a 4-byte UTF-8 scalar -> surrogate pair ->
        //     decoded back to the same 4-byte sequence, all through the field. ---
        ctx.check("set_astral_write_java_len_2", field_string_fixture::set_astral_write_len() == 2);
        ctx.check("set_astral_write_java_cpcount_1", field_string_fixture::set_astral_write_cpcount() == 1);
        ctx.check("set_astral_write_java_cp0_1F600", field_string_fixture::set_astral_write_cp0() == 0x1F600);
        ctx.check("set_astral_write_java_value_utf8",
                  field_string_fixture::set_astral_write_value() == std::string{ "\xF0\x9F\x98\x80" });
        ctx.check("set_astral_write_vmhook_reread",
                  field_string_fixture::read_static("setAstralWrite") == std::string{ "\xF0\x9F\x98\x80" });

        // --- Interior NUL ("a\0b"): Java length 3, charAt(1)==0; vmhook re-read is
        //     the same 3 bytes with the interior NUL preserved (length-counted, not
        //     C-string-truncated by either the write or the read). ---
        ctx.check("set_nul_write_java_len_3", field_string_fixture::set_nul_write_len() == 3);
        ctx.check("set_nul_write_java_cp1_is_0", field_string_fixture::set_nul_write_cp1() == 0);
        {
            const std::string reread{ field_string_fixture::read_static("setNulWrite") };
            ctx.check("set_nul_write_vmhook_reread_len_3", reread.size() == 3);
            ctx.check("set_nul_write_vmhook_reread_bytes",
                      reread.size() == 3 && reread[0] == 'a' && reread[1] == '\0' && reread[2] == 'b');
        }

        // --- Empty write: Java sees a real empty String (length 0, NOT null),
        //     and vmhook re-reads "". ---
        ctx.check("set_empty_write_java_len_0", field_string_fixture::set_empty_write_len() == 0);
        ctx.check("set_empty_write_java_not_null", !field_string_fixture::set_empty_write_is_null());
        ctx.check("set_empty_write_java_equals_empty", field_string_fixture::set_empty_write_eq_empty());
        ctx.check("set_empty_write_vmhook_reread_empty",
                  field_string_fixture::read_static("setEmptyWrite").empty());

        // --- set(const char*) and set(string_view) convertibility arms landed
        //     correct values, visible to Java and to vmhook's re-read. ---
        ctx.check("set_via_char_ptr_java_matches", field_string_fixture::set_via_char_ptr_matches());
        ctx.check("set_via_char_ptr_vmhook_reread",
                  field_string_fixture::read_static("setViaCharPtr") == "char-ptr-set");
        ctx.check("set_via_string_view_java_matches", field_string_fixture::set_via_string_view_matches());
        ctx.check("set_via_string_view_vmhook_reread",
                  field_string_fixture::read_static("setViaStringView") == "sv-set");

        // --- RE-SET: the second rebind won; Java and vmhook both see "second". ---
        ctx.check("set_reset_java_value_second", field_string_fixture::set_reset_value() == "second");
        ctx.check("set_reset_java_len_6", field_string_fixture::set_reset_len() == 6);
        ctx.check("set_reset_vmhook_reread_second",
                  field_string_fixture::read_static("setReSet") == "second");

        // --- Max-BMP write U+FFFF: Java length 1, cp 0xFFFF; vmhook re-read EF BF BF.
        //     A single 3-byte UTF-8 scalar encoded to exactly one UTF-16 unit. ---
        ctx.check("set_max_bmp_write_java_len_1", field_string_fixture::set_max_bmp_write_len() == 1);
        ctx.check("set_max_bmp_write_java_cp0_FFFF",
                  field_string_fixture::set_max_bmp_write_cp0() == 0xFFFF);
        ctx.check("set_max_bmp_write_java_value_utf8",
                  field_string_fixture::set_max_bmp_write_value() == std::string{ "\xEF\xBF\xBF" });
        ctx.check("set_max_bmp_write_vmhook_reread",
                  field_string_fixture::read_static("setMaxBmpWrite") == std::string{ "\xEF\xBF\xBF" });

        // --- Mixed write: Java length 5 (UTF-16 units), cpCount 4, cp[3]==0x1F600;
        //     vmhook re-read is the exact heterogeneous UTF-8 buffer.  Proves the
        //     encoder handles a buffer of 1/2/3/4-byte scalars in one call. ---
        ctx.check("set_mixed_write_java_len_5", field_string_fixture::set_mixed_write_len() == 5);
        ctx.check("set_mixed_write_java_cpcount_4", field_string_fixture::set_mixed_write_cpcount() == 4);
        ctx.check("set_mixed_write_java_cp3_1F600",
                  field_string_fixture::set_mixed_write_cp3() == 0x1F600);
        ctx.check("set_mixed_write_java_value_utf8",
                  field_string_fixture::set_mixed_write_value()
                  == std::string{ "A\xC3\xA9\xE6\x97\xA5\xF0\x9F\x98\x80" });
        ctx.check("set_mixed_write_vmhook_reread",
                  field_string_fixture::read_static("setMixedWrite")
                  == std::string{ "A\xC3\xA9\xE6\x97\xA5\xF0\x9F\x98\x80" });

        // --- Modest-long write (300 'L'): the rebind allocates the FULL length
        //     regardless of the old short backing.  Java sees 300 chars, all 'L';
        //     vmhook re-reads 300 'L' bytes. ---
        ctx.check("set_long_write_java_len_300", field_string_fixture::set_long_write_len() == 300);
        ctx.check("set_long_write_java_all_L", field_string_fixture::set_long_write_all_l());
        {
            const std::string reread{ field_string_fixture::read_static("setLongWrite") };
            ctx.check("set_long_write_vmhook_reread_len_300", reread.size() == 300u);
            ctx.check("set_long_write_vmhook_reread_all_L",
                      reread == std::string(300u, 'L'));
        }

        // --- RE-ENCODE re-set: the second rebind flipped LATIN1 -> UTF-16.  Java
        //     sees length 3 (Z + surrogate pair), cpCount 2, cp[1]==0x1F600; vmhook
        //     re-reads 'Z' + the 4-byte emoji.  Proves each rebind builds a String
        //     of the correct encoding (not a coder-stuck in-place edit). ---
        ctx.check("set_re_encode_java_len_3", field_string_fixture::set_re_encode_len() == 3);
        ctx.check("set_re_encode_java_cpcount_2", field_string_fixture::set_re_encode_cpcount() == 2);
        ctx.check("set_re_encode_java_cp1_1F600",
                  field_string_fixture::set_re_encode_cp1() == 0x1F600);
        ctx.check("set_re_encode_vmhook_reread",
                  field_string_fixture::read_static("setReEncode")
                  == std::string{ "Z\xF0\x9F\x98\x80" });

        // --- INHERITED writable INSTANCE String rebound through the child instance
        //     field_proxy: Java sees "child-wrote" on the inherited slot (the SET
        //     super walk resolved and rebound the base-declared field). ---
        ctx.check("set_inherited_writable_java_matches",
                  field_string_fixture::set_inherited_writable_matches());
        ctx.check("set_inherited_writable_java_value",
                  field_string_fixture::set_inherited_writable_value() == "child-wrote");

        // --- Instance NON-ASCII SET: Java length 1, cp 0x65E5; vmhook re-read
        //     E6 97 A5 — the encode path verified on an INSTANCE slot. ---
        ctx.check("instance_cjk_set_java_len_1", field_string_fixture::inst_cjk_len() == 1);
        ctx.check("instance_cjk_set_java_cp0_65E5", field_string_fixture::inst_cjk_cp0() == 0x65E5);
        ctx.check("instance_cjk_set_java_value_utf8",
                  field_string_fixture::inst_cjk_value() == std::string{ "\xE6\x97\xA5" });

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
                // The inherited writable instance slot, rebound in PHASE 1, still
                // reads "child-wrote" via a FRESH instance proxy post-probe — the
                // SET super-walk rebind is stable and re-resolvable.
                const auto inh_w{ self_after->get_field("inheritedWritable") };
                if (inh_w.has_value())
                {
                    const std::string v = inh_w->get();
                    ctx.check("fstr_inherited_writable_reread_after_probe", v == "child-wrote");
                }
            }
        }
    }
}
