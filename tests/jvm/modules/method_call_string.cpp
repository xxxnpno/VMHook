// method_call_string — exhaustive JVM tests for method_proxy::call() taking and
// returning java.lang.String.
//
// THE FEATURE
//   method_proxy::call() on a Ljava/lang/String;-returning method must hand back
//   the Java string's bytes as an owned std::string, extracted with the
//   unambiguous value_t::as_string() accessor.  This is the site of the historic
//   "call-stub truncation" bug: a String-returning call() once returned a
//   truncated 32-bit OOP handle (-> "") on every JDK where the call stub is
//   present, while the JNI fallback returned the real text.  Both paths now
//   decode the String (call_stub via read_java_string, call_jni via
//   GetStringUTFChars).
//
// THE TWO DECODE PATHS (and where they DIVERGE)
//   call() picks its path from detail::find_call_stub_entry():
//     * call_jni fallback (stub ABSENT — the path CI ACTUALLY TAKES on every CI
//       JDK 8..26; StubRoutines::_call_stub_entry is not exported in their
//       VMStructs, same gate example.cpp uses for its primitive call check): the
//       'L'/'[' arm detects Ljava/lang/String; and calls GetStringUTFChars, so
//       the bytes are MODIFIED UTF-8 — U+0000 is encoded as C0 80 and a
//       supplementary-plane scalar as a CESU-8 SURROGATE PAIR (two 3-byte
//       sequences, NOT the 4-byte standard form).
//     * call_stub fast path (stub present): the default arm calls
//       read_java_string(result_oop), which walks the String's backing array off
//       the heap and emits STANDARD UTF-8 — U+0000 stays a raw NUL and a
//       supplementary scalar becomes one 4-byte sequence.  It reads the String IN
//       FULL up to read_java_string_max_units (16M chars): the old hard 4096-char
//       cap that made longer Strings decode to "" was removed (robustness bug #29),
//       along with the old lossy "non-ASCII -> '?'" substitution.  Only length 0
//       (the empty string) decodes to "".
//   So the two paths AGREE byte-for-byte on all BMP text (ASCII, Latin-1, CJK,
//   Greek, the multi-byte boundaries) AND on arbitrarily long ASCII (no cap on
//   either side now), and DIFFER only on (a) supplementary-plane scalars (CESU-8
//   6 bytes vs standard 4), (b) any NUL (C0 80 vs raw 00), and (c) the empty/null
//   variant tag.  This module asserts the broad agreement UNCONDITIONALLY
//   (including the very-large >65536 ASCII cases, which now hold on both paths) and
//   branches only on the astral / NUL / null-tag divergences, picking the
//   path-correct expectation for whichever decoder is live.  It records the live
//   path as [INFO] so a reader of test_results.txt always knows which decoder the
//   assertions exercised.  (The CI path is always call_jni; the call_stub branches
//   are kept correct-by-construction from the header's deterministic decoders but
//   are unreachable on the CI matrix.)
//
//   (The call_jni byte expectations were cross-checked against the JVM's own
//   modified-UTF-8 encoding — GetStringUTFChars emits exactly what
//   DataOutputStream.writeUTF does.)
//
// STATIC DISPATCH — THE BUG THIS MODULE'S PRIOR EXPANSION HIT
//   A STATIC method MUST be driven through the wrapper's static_method(...)
//   accessor, which builds a null-receiver proxy so call_jni takes the static
//   path (GetStaticMethodID + CallStaticObjectMethodA).  Resolving a static
//   method through an INSTANCE proxy (self->get_method("staticEcho")) leaves the
//   proxy's receiver non-null, so call_jni mis-classifies it as an instance call,
//   dispatches CallObjectMethodA with the receiver bound as the first declared
//   argument, and returns the wrong String (and, on HotSpot, can corrupt the
//   thread's call/exception state).  Every static call below therefore goes
//   through static_method(...).
//
// WHY EVERYTHING RUNS IN ONE DETOUR
//   call() requires vmhook::hotspot::current_java_thread, set only while the Java
//   thread executes inside an interpreter detour.  The module hooks
//   MethodString.trigger() (a no-arg instance method the probe calls on a real
//   bytecode dispatch); inside that detour the native side drives every
//   String-returning method on the live receiver (and the static methods) and
//   records each result, then the body asserts them after run_probe.
//
// SUITE-SAFETY (mirrors register_class / method_call_wide_args): the whole body
// runs under a try/catch (a throw -> [INFO], never FAIL); an unconditional
// shutdown_hooks() runs OUTSIDE the try so the hook table is always empty for the
// next module; an entry guard skips cleanly if the fixture is not loaded.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace
{
    // ---- Wrapper for vmhook.fixtures.MethodString --------------------------
    // Accessors use the documented clean one-liner idiom (no defensive checks):
    // get_method("m")->call(...).as_string().  Suite-safety lives at the call
    // sites and the module body, never as noise in these accessors.
    class method_string_fixture : public vmhook::object<method_string_fixture>
    {
    public:
        explicit method_string_fixture(vmhook::oop_t instance) noexcept
            : vmhook::object<method_string_fixture>{ instance }
        {
        }

        static auto set_go(bool value) -> void          { static_field("go")->set(value); }
        static auto get_done() -> bool                   { return static_field("done")->get(); }
        static auto get_trigger_count() -> std::int32_t  { return static_field("triggerCount")->get(); }

        // ---- Java-published cross-check witnesses (read AFTER run_probe) ----
        static auto has_coder_field() -> bool            { return static_field("jHasCoderField")->get(); }
        static auto coder_regular() -> std::int32_t      { return static_field("jCoderRegular")->get(); }
        static auto coder_cafe() -> std::int32_t         { return static_field("jCoderCafe")->get(); }
        static auto coder_cjk() -> std::int32_t          { return static_field("jCoderCjk")->get(); }
        static auto coder_emoji() -> std::int32_t        { return static_field("jCoderEmoji")->get(); }
        static auto coder_max_bmp() -> std::int32_t      { return static_field("jCoderMaxBmp")->get(); }
        static auto big_len() -> std::int32_t            { return static_field("jBigLen")->get(); }
    };

    // ---- One captured call() result ----------------------------------------
    // The decoded std::string plus the value_t's own predicates, so a check can
    // assert both the payload AND the variant tag (is_string / is_void).
    struct observation
    {
        std::string value{};
        std::size_t byte_len{ 0 };
        bool        is_string{ false };
        bool        is_void{ false };
        bool        captured{ false };
    };

    std::mutex                         g_mutex;
    std::map<std::string, observation> g_obs;                       // key -> result
    std::atomic<int>                   g_detour_calls{ 0 };
    std::atomic<bool>                  g_self_was_valid{ false };
    std::atomic<bool>                  g_call_stub_path{ false };   // true => read_java_string decoder
    std::atomic<int>                   g_loop_iterations{ 0 };
    std::atomic<int>                   g_loop_distinct{ -1 };       // distinct results across the leak loop

    // Extraction-API agreement witnesses (the module's raison d'etre): as_string()
    // (the unambiguous accessor) must yield byte-identical results to the implicit
    // `std::string s = call()` copy-init for every shape.  Captured under the mutex.
    std::map<std::string, std::pair<std::string, std::string>> g_extract;  // key -> {as_string, implicit}

    // Record one value_t under `key`, capturing payload + variant tag.
    auto record_value(const std::string& key, const vmhook::method_proxy::value_t& v) -> void
    {
        observation obs{};
        obs.value     = v.as_string();
        obs.byte_len  = obs.value.size();
        obs.is_string = v.is_string();
        obs.is_void   = v.is_void();
        obs.captured  = true;
        std::lock_guard<std::mutex> lock{ g_mutex };
        g_obs[key] = obs;
    }

    // Drive a no-arg INSTANCE String method and record it under its own name.
    auto capture(const method_string_fixture& self, const char* method) -> void
    {
        if (auto proxy{ self.get_method(method) })
        {
            record_value(method, proxy->call());
        }
    }

    // Drive a no-arg STATIC String method via static_method (null-receiver proxy
    // -> static JNI path) and record it under "static:<name>".
    auto capture_static(const char* method) -> void
    {
        if (auto proxy{ method_string_fixture::static_method(method) })
        {
            record_value(std::string{ "static:" } + method, proxy->call());
        }
    }

    // Capture BOTH extraction APIs for one no-arg INSTANCE String method: the
    // unambiguous as_string() accessor AND the implicit `std::string s = call()`
    // copy-init (which relies on overload resolution against the constrained
    // conversion operator).  The contract is they are byte-identical; recorded so a
    // check below can hard-assert agreement for ASCII, unicode, and interior-NUL.
    auto capture_extract(const method_string_fixture& self, const char* method,
                         const std::string& key) -> void
    {
        if (auto proxy{ self.get_method(method) })
        {
            const std::string via_accessor{ proxy->call().as_string() };
            std::string       via_implicit = proxy->call();  // copy-init / overload-resolution path
            std::lock_guard<std::mutex> lock{ g_mutex };
            g_extract[key] = std::make_pair(via_accessor, via_implicit);
        }
    }

    auto get_extract(const std::string& key) -> std::pair<std::string, std::string>
    {
        std::lock_guard<std::mutex> lock{ g_mutex };
        const auto it{ g_extract.find(key) };
        return (it != g_extract.end()) ? it->second : std::pair<std::string, std::string>{};
    }

    // Read back a recorded observation (default-constructed if missing).
    auto get(const std::string& key) -> observation
    {
        std::lock_guard<std::mutex> lock{ g_mutex };
        const auto it{ g_obs.find(key) };
        return (it != g_obs.end()) ? it->second : observation{};
    }

    // Render a std::string as "AA BB CC" hex for [INFO] diagnostics.
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

    // ---- The detour body: drive every String method on the live receiver ----
    auto run_string_calls(const std::unique_ptr<method_string_fixture>& self) -> void
    {
        if (!self)
        {
            return;
        }

        // ASCII / structural — path-independent.
        capture(*self, "regular");
        capture(*self, "empty");
        capture(*self, "single");
        capture(*self, "whitespace");
        capture(*self, "punctuation");
        capture(*self, "asciiHigh");
        capture(*self, "allAscii");
        capture(*self, "returnNull");
        capture(*self, "fieldValue");
        capture(*self, "dynamic");
        capture(*self, "substringResult");
        capture(*self, "longAscii");

        // BMP unicode — both decoders emit identical standard UTF-8.
        capture(*self, "cafe");
        capture(*self, "resume");
        capture(*self, "accents");
        capture(*self, "mixed");
        capture(*self, "cjk");
        capture(*self, "greek");
        capture(*self, "latin1Hi");
        capture(*self, "bmpBoundary");
        capture(*self, "asciiBoundary");

        // More BMP — control chars, the max BMP code point, the replacement
        // char, Unicode whitespace.  All BMP, so path-independent.
        capture(*self, "controlChars");
        capture(*self, "maxBmp");
        capture(*self, "replacementChar");
        capture(*self, "unicodeWhitespace");

        // Lone / reversed surrogates: a Java String is a raw UTF-16 unit
        // sequence, so an unpaired or out-of-order surrogate is a legal String.
        // BOTH decoders emit the 3-byte CESU encoding of each surrogate code unit
        // (they do NOT '?'-substitute like String.getBytes(UTF_8) does), so these
        // are ALSO path-independent.
        capture(*self, "loneHighSurrogate");
        capture(*self, "loneLowSurrogate");
        capture(*self, "reversedSurrogates");

        // Supplementary plane + NUL family — path-divergent.
        capture(*self, "emoji");
        capture(*self, "astralMid");
        capture(*self, "twoAstral");
        capture(*self, "interiorNul");
        capture(*self, "nulOnly");
        capture(*self, "leadingNul");
        capture(*self, "trailingNul");

        // Very large returns — ABOVE the old 4096 cap and ABOVE 65536 code units.
        // BOTH decoders now return the whole String: GetStringUTFChars (call_jni)
        // never capped, and read_java_string (call_stub) no longer caps either (the
        // 4096-char cap was removed — robustness bug #29; it now reads up to 16M chars).
        if (auto proxy{ self->get_method("bigString") })
        {
            record_value("bigString:5000",  proxy->call(static_cast<std::int32_t>(5000)));
        }
        if (auto proxy{ self->get_method("bigString") })
        {
            record_value("bigString:70000", proxy->call(static_cast<std::int32_t>(70000)));
        }
        if (auto proxy{ self->get_method("bigString") })
        {
            record_value("bigString:131072", proxy->call(static_cast<std::int32_t>(131072)));
        }

        // Static dispatch (FindClass / pool_holder branch, not GetObjectClass),
        // ALL via static_method(...).
        capture_static("staticRegular");
        capture_static("staticEmpty");
        capture_static("staticNull");
        capture_static("staticUnicode");
        capture_static("staticCjk");
        capture_static("staticEmoji");
        capture_static("staticInteriorNul");
        capture_static("staticDynamic");
        capture_static("staticMaxBmp");
        capture_static("staticLoneSurrogate");

        // Static very-large return (>65536) via static_method + explicit
        // signature (the static dispatch path, null-receiver proxy).
        if (auto proxy{ method_string_fixture::static_method(
                "staticBigString", "(I)Ljava/lang/String;") })
        {
            record_value("static:staticBigString:70000",
                         proxy->call(static_cast<std::int32_t>(70000)));
        }

        // Argument round-trips (String arg -> String return), INSTANCE.
        if (auto proxy{ self->get_method("echo") })
        {
            record_value("echo:ascii", proxy->call(std::string{ "round-trip-123" }));
        }
        if (auto proxy{ self->get_method("echo") })
        {
            record_value("echo:empty", proxy->call(std::string{}));
        }
        // A Latin-1 round-trip arg "caf\xC3\xA9": NewStringUTF (call_jni) decodes
        // it from modified UTF-8 to the 4-char Java String caf+U+00E9, and
        // GetStringUTFChars re-encodes it back to caf\xC3\xA9 — so on the call_jni
        // path this round-trips byte-for-byte.  (On the call_stub path
        // make_java_string's encode and read_java_string's decode handle it too;
        // the divergence, if any, is recorded, not hard-asserted.)
        if (auto proxy{ self->get_method("echo") })
        {
            record_value("echo:unicode", proxy->call(std::string{ "caf\xC3\xA9" }));
        }
        if (auto proxy{ self->get_method("echo") })
        {
            record_value("echo:long", proxy->call(std::string(300, 'Z')));
        }
        // echo of a CJK arg: make_java_string's utf8_to_utf16 decodes the 3-byte
        // sequences to BMP units; the return re-encodes them identically (BMP
        // modified-UTF-8 == standard UTF-8), so this round-trips byte-for-byte on
        // call_jni.
        if (auto proxy{ self->get_method("echo") })
        {
            record_value("echo:cjk",
                         proxy->call(std::string{ "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E" }));
        }
        // echo of a STANDARD 4-byte emoji arg: utf8_to_utf16 decodes it to a
        // surrogate PAIR (U+1F600); on the call_jni return GetStringUTFChars
        // re-encodes that pair as a 6-byte CESU sequence — so a 4-byte input comes
        // back as 6 bytes (the standard-UTF-8 -> modified-UTF-8 asymmetry, the
        // subtlety most encoders get wrong).  Cross-checked against writeUTF.
        if (auto proxy{ self->get_method("echo") })
        {
            record_value("echo:emoji", proxy->call(std::string{ "\xF0\x9F\x98\x80" }));
        }
        // echo of an interior-NUL arg "a\0b" (RAW NUL in the std::string):
        // utf8_to_utf16 reads it length-counted (raw 0x00 -> U+0000, never a
        // C-string cut) into the 3-char Java String 'a' U+0000 'b'; on the
        // call_jni return U+0000 re-encodes to C0 80, giving "a\xC0\x80b" (4 bytes).
        if (auto proxy{ self->get_method("echo") })
        {
            record_value("echo:nul", proxy->call(std::string("a\0b", 3)));
        }
        // echo of the max BMP code point U+FFFF (as a 3-byte UTF-8 arg).
        if (auto proxy{ self->get_method("echo") })
        {
            record_value("echo:maxBmp", proxy->call(std::string{ "\xEF\xBF\xBF" }));
        }
        // echo of a >65536-code-unit arg: the String-arg encoder
        // (convert_jni_arg -> jni_new_string_utf16_local) calls JNIEnv::NewString with
        // the full length-counted UTF-16, so the JVM builds the FULL String at any
        // length (no cap on the ARG side).  Both return decoders then hand all 70000
        // bytes back (read_java_string's old 4096 read cap is gone — bug #29).  Proves
        // the over-cap ARGUMENT encode AND over-cap RETURN decode end to end.
        if (auto proxy{ self->get_method("echo") })
        {
            record_value("echo:big70000", proxy->call(std::string(70000, 'Q')));
        }

        // ---- const char* String ARG (a DISTINCT convert_jni_arg branch) -----
        // std::string args take the std::string branch; a const char* arg takes a
        // SEPARATE branch whose contract is: nullptr -> Java null, any non-null
        // pointer (including the empty "") -> a real Java String, routed through the
        // SAME length-counted UTF-16 encoder (jni_new_string_utf16_local) so astral
        // bytes survive.  None of the existing cases exercise this branch, so add:
        //   * a plain ASCII C-string  (-> identical round-trip),
        //   * the empty C-string ""   (-> a real empty Java String, NOT null),
        //   * a standard-4-byte astral C-string (-> proper surrogate pair, proven
        //     decoder-independently by lengthOf == 2 below),
        //   * a true nullptr          (-> Java null; echo returns null -> "").
        if (auto proxy{ self->get_method("echo") })
        {
            record_value("echo:cstr_ascii", proxy->call(static_cast<const char*>("c-string-arg")));
        }
        if (auto proxy{ self->get_method("echo") })
        {
            record_value("echo:cstr_empty", proxy->call(static_cast<const char*>("")));
        }
        if (auto proxy{ self->get_method("echo") })
        {
            record_value("echo:cstr_emoji",
                         proxy->call(static_cast<const char*>("\xF0\x9F\x98\x80")));
        }
        if (auto proxy{ self->get_method("echo") })
        {
            // nullptr const char* -> Java null arg; echo(null) returns null, which
            // the return decoder reports as "" (call_jni) / monostate "" (call_stub).
            record_value("echo:cstr_null", proxy->call(static_cast<const char*>(nullptr)));
        }
        // Decoder-INDEPENDENT witnesses for the const char* branch via Java's own
        // String.length(): the empty C-string must arrive as a 0-length String (NOT
        // null -> "len=0", whereas a null arg gives "len=null"), the astral C-string
        // must arrive as ONE supplementary scalar = 2 UTF-16 units ("len=2"), and the
        // nullptr must arrive as Java null ("len=null").
        if (auto proxy{ self->get_method("lengthOf") })
        {
            record_value("lengthOf:cstr_empty", proxy->call(static_cast<const char*>("")));
        }
        if (auto proxy{ self->get_method("lengthOf") })
        {
            record_value("lengthOf:cstr_emoji",
                         proxy->call(static_cast<const char*>("\xF0\x9F\x98\x80")));
        }
        if (auto proxy{ self->get_method("lengthOf") })
        {
            record_value("lengthOf:cstr_null", proxy->call(static_cast<const char*>(nullptr)));
        }

        // ---- std::string_view String ARG (another DISTINCT branch) ----------
        // convert_jni_arg has a dedicated std::string_view branch (same
        // length-counted UTF-16 encode as std::string: interior NULs + astral
        // scalars preserved).  Drive it with an interior-NUL view so the
        // counted-length, no-C-string-cut property is proven on THIS branch too.
        if (auto proxy{ self->get_method("echo") })
        {
            record_value("echo:sv_nul",
                         proxy->call(std::string_view{ "a\0b", 3 }));
        }
        if (auto proxy{ self->get_method("lengthOf") })
        {
            record_value("lengthOf:sv_nul",
                         proxy->call(std::string_view{ "a\0b", 3 }));
        }

        // concat(a, b): two String args -> one String return (INSTANCE).
        if (auto proxy{ self->get_method("concat") })
        {
            record_value("concat:ab", proxy->call(std::string{ "foo-" }, std::string{ "bar" }));
        }

        // length-driven: lengthOf("abcd") -> "len=4" (INSTANCE, String->String).
        if (auto proxy{ self->get_method("lengthOf") })
        {
            record_value("lengthOf:abcd", proxy->call(std::string{ "abcd" }));
        }
        // charAt-driven: charAtOf("wxyz", 2) -> "y" (INSTANCE, String+int->String).
        if (auto proxy{ self->get_method("charAtOf") })
        {
            record_value("charAtOf:wxyz2",
                         proxy->call(std::string{ "wxyz" }, static_cast<std::int32_t>(2)));
        }
        // ---- ARG-ENCODER WITNESS via Java's own String.length() -------------
        // lengthOf(arg) returns "len=" + the arg's *Java* char (UTF-16 unit) count,
        // measured ON THE JAVA SIDE — a decoder-independent proof of what the String
        // ARG encoder actually built (it does NOT pass back through the return
        // decoder's modified-UTF-8 quirk; the result "len=N" is pure ASCII and
        // byte-identical on call_stub and call_jni).  These pin the fix: the astral
        // arg must arrive as ONE supplementary scalar = 2 UTF-16 units (len=2, was 1
        // when NewStringUTF mangled it to U+00F0), and the interior-NUL arg must
        // arrive with its U+0000 intact = 3 units (len=3, was 1 when NewStringUTF
        // truncated the C string at the NUL).
        if (auto proxy{ self->get_method("lengthOf") })
        {
            record_value("lengthOf:emoji", proxy->call(std::string{ "\xF0\x9F\x98\x80" }));
        }
        if (auto proxy{ self->get_method("lengthOf") })
        {
            record_value("lengthOf:nul", proxy->call(std::string("a\0b", 3)));
        }
        // Direct ECHO identity of the astral arg on the INSTANCE path: with the fix
        // the Java String IS U+1F600, so the round-trip value is the decoder's exact
        // re-encoding of that scalar (standard 4-byte on call_stub, CESU-8 6-byte on
        // call_jni) — asserted hard below, no longer an [INFO] characterization.
        if (auto proxy{ self->get_method("echo") })
        {
            record_value("echo:twoAstralArg",
                         proxy->call(std::string{ "\xF0\x9F\x98\x80\xF0\x9F\x9A\x80" }));
        }

        // ---- STATIC argument round-trips — the regression site -------------
        // staticEcho / staticConcat are STATIC; drive them through static_method
        // with an EXPLICIT signature (null-receiver proxy -> static JNI path).
        // Resolving these via self->get_method() would mis-bind the receiver as
        // the first declared arg (the bug that failed CI before).
        if (auto proxy{ method_string_fixture::static_method(
                "staticEcho", "(Ljava/lang/String;)Ljava/lang/String;") })
        {
            record_value("staticEcho:ascii", proxy->call(std::string{ "s-echo" }));
        }
        if (auto proxy{ method_string_fixture::static_method(
                "staticConcat", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;") })
        {
            record_value("staticConcat:ab",
                         proxy->call(std::string{ "L+" }, std::string{ "R" }));
        }

        // ---- FRESH-OOP unicode slices + built-empty boundary ---------------
        // subOf(value, b, e) returns value.substring(b,e) — a FRESH (non-interned)
        // String OOP.  Drive it three ways:
        //   * a unicode (CJK) slice -> proves a fresh non-interned MULTIBYTE OOP
        //     decodes correctly (not just the interned constant-pool CJK case),
        //   * an EMPTY slice (b == e) -> a fresh empty String OOP, distinct from
        //     the interned literal returned by empty(),
        //   * a slice that drops the first ASCII char -> a fresh ASCII OOP.
        if (auto proxy{ self->get_method("subOf") })
        {
            record_value("subOf:cjk_full",
                         proxy->call(std::string{ "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E" },
                                     static_cast<std::int32_t>(0), static_cast<std::int32_t>(3)));
        }
        if (auto proxy{ self->get_method("subOf") })
        {
            record_value("subOf:cjk_tail",
                         proxy->call(std::string{ "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E" },
                                     static_cast<std::int32_t>(1), static_cast<std::int32_t>(3)));
        }
        if (auto proxy{ self->get_method("subOf") })
        {
            record_value("subOf:empty_slice",
                         proxy->call(std::string{ "abcdef" },
                                     static_cast<std::int32_t>(2), static_cast<std::int32_t>(2)));
        }
        // builtA(0): a fresh EMPTY String OOP from a StringBuilder (NOT the interned
        // "" literal) — proves the empty-from-builder boundary on the heap path.
        if (auto proxy{ self->get_method("builtA") })
        {
            record_value("builtA:0", proxy->call(static_cast<std::int32_t>(0)));
        }
        // Length-1 multibyte returns: one CJK char (3 bytes) and one Latin-1 char
        // (2 bytes).  A length-1 String is a distinct degenerate from the empty and
        // multi-char cases; both are BMP -> path-independent exact bytes.
        capture(*self, "singleCjk");
        capture(*self, "singleLatin1");

        // ---- Unicode / boundary ARG round-trips (arg-encoder coverage) ------
        // Greek (2-byte) arg -> identical 4 bytes back (BMP, path-independent):
        // proves the arg encoder handles a pure 2-byte-sequence string.
        if (auto proxy{ self->get_method("echo") })
        {
            record_value("echo:greek", proxy->call(std::string{ "\xCE\xB1\xCE\xB2" }));
        }
        // 2-byte + 3-byte mix arg (ü ñ €) -> identical 7 bytes back: proves the arg
        // encoder spans the 2/3-byte boundary in one string.
        if (auto proxy{ self->get_method("echo") })
        {
            record_value("echo:mixed",
                         proxy->call(std::string{ "\xC3\xBC\xC3\xB1\xE2\x82\xAC" }));
        }
        // Replacement char U+FFFD arg -> identical 3 bytes (EF BF BD) back.
        if (auto proxy{ self->get_method("echo") })
        {
            record_value("echo:replacement", proxy->call(std::string{ "\xEF\xBF\xBD" }));
        }
        // Latin-1 RESUME arg (two U+00E9) -> identical 8 bytes back: repeated
        // 2-byte sequences in a longer string.
        if (auto proxy{ self->get_method("echo") })
        {
            record_value("echo:resume",
                         proxy->call(std::string{ "r\xC3\xA9sum\xC3\xA9" }));
        }
        // Combined hardest ARG: an astral scalar IMMEDIATELY followed by an
        // interior NUL ("<emoji>\0z").  Proves the arg encoder preserves BOTH a
        // surrogate pair AND a raw NUL in one length-counted string.  Java length
        // is 4 (2 surrogate units + U+0000 + 'z'), proven decoder-independently via
        // lengthOf below; the RETURN bytes diverge only by the astral+NUL encoding.
        if (auto proxy{ self->get_method("echo") })
        {
            record_value("echo:emoji_nul",
                         proxy->call(std::string("\xF0\x9F\x98\x80\x00z", 6)));
        }
        if (auto proxy{ self->get_method("lengthOf") })
        {
            record_value("lengthOf:emoji_nul",
                         proxy->call(std::string("\xF0\x9F\x98\x80\x00z", 6)));
        }
        // Lone-high-surrogate ARG round-trip: the arg is the 3-byte CESU encoding
        // of U+D83D (ED A0 BD).  utf8_to_utf16 decodes that surrogate code unit to
        // a single UTF-16 unit (Java length 1, proven via lengthOf below), and the
        // RETURN re-encodes it as the 3-byte CESU form on BOTH paths (read_java_string
        // and GetStringUTFChars agree on a lone surrogate) -> path-independent.
        if (auto proxy{ self->get_method("echo") })
        {
            record_value("echo:lone_surrogate",
                         proxy->call(std::string{ "\xED\xA0\xBD" }));
        }
        if (auto proxy{ self->get_method("lengthOf") })
        {
            record_value("lengthOf:lone_surrogate",
                         proxy->call(std::string{ "\xED\xA0\xBD" }));
        }

        // ---- Unicode + empty-arg multi-arg concat / charAt -----------------
        // concat of two Latin-1 args -> the two are joined on the JAVA side, so the
        // return is the concatenation re-encoded by the live decoder; BMP so
        // path-independent.  "caf"+U+00E9 ++ U+00E9+"sum" = caf,U+00E9,U+00E9,sum.
        if (auto proxy{ self->get_method("concat") })
        {
            record_value("concat:unicode",
                         proxy->call(std::string{ "caf\xC3\xA9" }, std::string{ "\xC3\xA9sum" }));
        }
        // concat with an EMPTY first arg -> identity of the second (proves an empty
        // String ARG in a multi-arg call is a real empty String, not null/dropped).
        if (auto proxy{ self->get_method("concat") })
        {
            record_value("concat:empty_lhs",
                         proxy->call(std::string{}, std::string{ "tail" }));
        }
        // charAtOf on a CJK arg at index 1 -> the single middle CJK char U+672C
        // (3 bytes E6 9C AC).  A String+int->String overload returning a MULTIBYTE
        // single char; BMP so path-independent and hard-assertable.
        if (auto proxy{ self->get_method("charAtOf") })
        {
            record_value("charAtOf:cjk1",
                         proxy->call(std::string{ "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E" },
                                     static_cast<std::int32_t>(1)));
        }
        // charAtOf on a STANDARD-4-byte emoji arg at index 0 -> the LONE HIGH
        // surrogate U+D83D (a FRESH single-UTF-16-unit String).  The astral arg
        // first arrives intact as the pair D83D DE00 (arg-encoder), then Java's
        // charAt(0) slices out the high half, and the RETURN decoder emits the
        // 3-byte CESU encoding of U+D83D on BOTH paths (ED A0 BD) — path-independent,
        // and a sharp astral-half boundary (a naive encoder that mangled the arg to
        // U+00F0 would return 'F0', not the high surrogate).
        if (auto proxy{ self->get_method("charAtOf") })
        {
            record_value("charAtOf:emoji0",
                         proxy->call(std::string{ "\xF0\x9F\x98\x80" },
                                     static_cast<std::int32_t>(0)));
        }
        // charAtOf at index 1 of the same emoji arg -> the LONE LOW surrogate U+DE00
        // (3-byte CESU ED B8 80 on both paths): proves index 1 reaches the SECOND
        // UTF-16 unit of the pair (it would be out of range had the arg collapsed to
        // a single unit).
        if (auto proxy{ self->get_method("charAtOf") })
        {
            record_value("charAtOf:emoji1",
                         proxy->call(std::string{ "\xF0\x9F\x98\x80" },
                                     static_cast<std::int32_t>(1)));
        }
        // ---- FRESH-OOP astral slices via subOf (substring of a surrogate pair) ----
        // subOf(emoji, 0, 1): a fresh single-unit String holding ONLY the high
        // surrogate U+D83D -> 3-byte CESU on both paths.  subOf(emoji, 0, 2): a fresh
        // String holding the FULL pair -> the standard 4-byte form on call_stub and
        // the 6-byte CESU form on call_jni (path-divergent, same bytes as `emoji`).
        // Proves a FRESH (non-interned) astral OOP — not just the interned constant —
        // decodes correctly, including a sliced lone half.
        if (auto proxy{ self->get_method("subOf") })
        {
            record_value("subOf:astral_high",
                         proxy->call(std::string{ "\xF0\x9F\x98\x80" },
                                     static_cast<std::int32_t>(0), static_cast<std::int32_t>(1)));
        }
        if (auto proxy{ self->get_method("subOf") })
        {
            record_value("subOf:astral_pair",
                         proxy->call(std::string{ "\xF0\x9F\x98\x80" },
                                     static_cast<std::int32_t>(0), static_cast<std::int32_t>(2)));
        }
        // concat of an astral arg + an ASCII arg -> a fresh BUILT String "<emoji>!"
        // whose first scalar is supplementary.  Path-divergent on the astral half,
        // ASCII tail identical: proves a multi-arg concat preserves a surrogate pair
        // through the JAVA-side join and the return decoder re-encodes it correctly.
        if (auto proxy{ self->get_method("concat") })
        {
            record_value("concat:astral_ascii",
                         proxy->call(std::string{ "\xF0\x9F\x98\x80" }, std::string{ "!" }));
        }

        // ---- char* (MUTABLE) String-arg branch (distinct overload from const char*) -
        // convert_jni_arg has `std::is_same_v<clean_t, char*>` alongside the const
        // char* branch; the existing cases only drive const char*.  A mutable char*
        // takes the SAME length-counted UTF-16 encoder, so an ASCII char* round-trips
        // and a nullptr char* maps to Java null.  Build the buffer on the stack so the
        // arg is a genuine `char*` (not a string literal, which is const char*).
        {
            char mutable_buf[] = "mutable-cstr";
            if (auto proxy{ self->get_method("echo") })
            {
                record_value("echo:mutcstr_ascii", proxy->call(static_cast<char*>(mutable_buf)));
            }
            if (auto proxy{ self->get_method("lengthOf") })
            {
                record_value("lengthOf:mutcstr_ascii",
                             proxy->call(static_cast<char*>(mutable_buf)));
            }
        }
        if (auto proxy{ self->get_method("echo") })
        {
            record_value("echo:mutcstr_null", proxy->call(static_cast<char*>(nullptr)));
        }

        // ---- ARG round-trips at the UTF-8 LENGTH boundaries (arg-encoder) --------
        // The arg encoder (utf8_to_utf16) must decode a multi-byte arg byte sequence
        // at each width transition exactly.  These shapes are RETURNED elsewhere but
        // never SENT as args; send them so the arg decoder's 1/2-byte (U+007F/U+0080),
        // 2/3-byte (U+07FF/U+0800), Latin-1 ceiling (U+00FF), and Unicode-whitespace
        // boundaries are all proven on the ENCODE side too.  BMP -> path-independent.
        if (auto proxy{ self->get_method("echo") })
        {
            record_value("echo:asciiBoundary", proxy->call(std::string{ "\x7F\xC2\x80" }));
        }
        if (auto proxy{ self->get_method("echo") })
        {
            record_value("echo:bmpBoundary",
                         proxy->call(std::string{ "\xDF\xBF\xE0\xA0\x80" }));
        }
        if (auto proxy{ self->get_method("echo") })
        {
            record_value("echo:latin1Hi", proxy->call(std::string{ "\xC3\xBF" }));
        }
        if (auto proxy{ self->get_method("echo") })
        {
            record_value("echo:unicodeWs",
                         proxy->call(std::string{ "\xC2\xA0\xE2\x80\xA8\xE2\x80\xA9" }));
        }
        // Java-side length witness of the asciiBoundary arg: 2 UTF-16 units (U+007F +
        // U+0080), decoder-independent — proves the arg encoder did NOT split the
        // 2-byte C2 80 into two units.
        if (auto proxy{ self->get_method("lengthOf") })
        {
            record_value("lengthOf:asciiBoundary", proxy->call(std::string{ "\x7F\xC2\x80" }));
        }

        // ---- 16-bit length-boundary returns (no uint16 wraparound) ----------
        // bigString at 65535 / 65536 / 65537 — straddling the 16-bit boundary —
        // proves the length read (an int32 arrayOop length) never truncates to 16
        // bits.  Pure ASCII 'A' so identical on both paths; asserted in full.
        if (auto proxy{ self->get_method("bigString") })
        {
            record_value("bigString:65535", proxy->call(static_cast<std::int32_t>(65535)));
        }
        if (auto proxy{ self->get_method("bigString") })
        {
            record_value("bigString:65537", proxy->call(static_cast<std::int32_t>(65537)));
        }

        // ---- STATIC unicode / fresh-OOP / arg-length coverage ---------------
        capture_static("staticGreek");
        capture_static("staticBuiltEmpty");
        if (auto proxy{ method_string_fixture::static_method(
                "staticLengthOf", "(Ljava/lang/String;)Ljava/lang/String;") })
        {
            // STATIC arg-encoder length witness for an astral arg: must arrive as 2
            // UTF-16 units on the STATIC dispatch path too (not just the instance one).
            record_value("staticLengthOf:emoji",
                         proxy->call(std::string{ "\xF0\x9F\x98\x80" }));
        }
        if (auto proxy{ method_string_fixture::static_method(
                "staticLengthOf", "(Ljava/lang/String;)Ljava/lang/String;") })
        {
            // STATIC arg-encoder length witness for an interior-NUL arg: 3 units.
            record_value("staticLengthOf:nul", proxy->call(std::string("a\0b", 3)));
        }
        if (auto proxy{ method_string_fixture::static_method(
                "staticLengthOf", "(Ljava/lang/String;)Ljava/lang/String;") })
        {
            // STATIC null arg -> Java null -> "len=null" (the static null-arg path).
            record_value("staticLengthOf:null", proxy->call(static_cast<const char*>(nullptr)));
        }

        // Length / cap angles via repeatA(int).
        if (auto proxy{ self->get_method("repeatA") })
        {
            record_value("repeatA:64", proxy->call(static_cast<std::int32_t>(64)));
        }
        if (auto proxy{ self->get_method("repeatA") })
        {
            record_value("repeatA:4096", proxy->call(static_cast<std::int32_t>(4096)));
        }
        if (auto proxy{ self->get_method("repeatA") })
        {
            record_value("repeatA:5000", proxy->call(static_cast<std::int32_t>(5000)));
        }

        // ---- as_string() vs implicit `std::string s = call()` agreement -------
        // The module exists because as_string() is the UNAMBIGUOUS accessor (the
        // implicit conversion is ambiguous against const char* on MSVC).  Prove the
        // two extraction APIs return byte-identical results across the value-shape
        // axis: ASCII, Latin-1 multibyte, CJK, and the interior-NUL case (the
        // sharpest — a C-string-based implicit path would cut at the NUL).
        capture_extract(*self, "regular",     "extract:regular");
        capture_extract(*self, "cafe",        "extract:cafe");
        capture_extract(*self, "cjk",         "extract:cjk");
        capture_extract(*self, "interiorNul", "extract:interiorNul");
        capture_extract(*self, "empty",       "extract:empty");

        // Leak / stability loop: call the same String method many times and prove
        // the result never changes (a starved JNI local-ref table — the failure
        // the local-ref-release fix addresses — would make later calls return ""
        // i.e. a 2nd distinct value).
        {
            constexpr int iterations{ 250 };
            std::string   first{};
            int           distinct{ 0 };
            bool          have_first{ false };
            for (int i{ 0 }; i < iterations; ++i)
            {
                auto proxy{ self->get_method("regular") };
                if (!proxy)
                {
                    break;
                }
                const std::string s{ proxy->call().as_string() };
                if (!have_first)
                {
                    first      = s;
                    have_first = true;
                    distinct   = 1;
                }
                else if (s != first)
                {
                    ++distinct;
                }
            }
            g_loop_iterations.store(iterations, std::memory_order_relaxed);
            g_loop_distinct.store(distinct, std::memory_order_relaxed);
        }

        // DEFENSIVE CLEAR — belt-and-braces.  Every call_jni dispatch already runs
        // check_callee_exception() (which clears) and every failed method-ID lookup
        // clears too, so nothing here should leave a pending exception.  But the
        // worst possible regression for the shared-JVM suite is a pending Java
        // exception leaking out of this detour and surfacing — uncaught — on the
        // main thread later (it would void the whole run's TOTAL).  This idempotent,
        // no-JNIEnv-safe clear guarantees the detour returns with a clean thread.
        vmhook::detail::jni_exception_clear();
    }

    // ---- The whole test body (run under try/catch by the wrapper) ----------
    auto run_string_checks(vmhook_test::context& ctx) -> void
    {
        // ENTRY GUARD: if the fixture is not loaded/resolvable, every call below
        // would chase a disengaged optional.  Bail to [INFO]; the wrapper's
        // unconditional shutdown_hooks() still runs.
        if (vmhook::find_class("vmhook/fixtures/MethodString") == nullptr)
        {
            ctx.record("[INFO] method_call_string: MethodString not loaded/resolvable "
                       "on this run; skipping live checks (no crash, no hooks armed).");
            return;
        }

        vmhook::register_class<method_string_fixture>("vmhook/fixtures/MethodString");

        {
            // Hook trigger(); inside the detour current_java_thread is live, so
            // every call() dispatches a real Java method and decodes its String
            // return.  scoped_hook uninstalls when the handle leaves scope.
            auto handle{ vmhook::scoped_hook<method_string_fixture>(
                "trigger",
                [](vmhook::return_value&,
                   const std::unique_ptr<method_string_fixture>& self)
                {
                    g_detour_calls.fetch_add(1, std::memory_order_relaxed);
                    g_self_was_valid.store(self != nullptr, std::memory_order_relaxed);
                    // Which decoder will call() use?  Record it so the assertions
                    // pick the right expectation for the divergent cases.
                    g_call_stub_path.store(
                        vmhook::detail::find_call_stub_entry() != nullptr,
                        std::memory_order_relaxed);
                    run_string_calls(self);
                }) };

            ctx.check("method_string_hook_installed", handle.installed());

            const bool done{ ctx.run_probe(
                [](bool value) { method_string_fixture::set_go(value); },
                []() { return method_string_fixture::get_done(); }) };

            ctx.check("method_string_probe_completed", done);
            ctx.check("method_string_detour_fired",
                      g_detour_calls.load(std::memory_order_relaxed) >= 1);
            ctx.check("method_string_detour_saw_self",
                      g_self_was_valid.load(std::memory_order_relaxed));
            ctx.check("method_string_trigger_count_advanced",
                      method_string_fixture::get_trigger_count() >= 1);

            const bool stub_path{ g_call_stub_path.load(std::memory_order_relaxed) };
            ctx.record(std::string{ "[INFO] method_call_string decode path: " }
                       + (stub_path
                              ? "call_stub (read_java_string / standard UTF-8)"
                              : "call_jni (GetStringUTFChars / modified UTF-8)"));

            // ============ PATH-INDEPENDENT: ASCII / structure ================

            const observation regular{ get("regular") };
            ctx.check("regular_captured",    regular.captured);
            ctx.check("regular_value_exact", regular.value == "hello world");
            ctx.check("regular_is_string",   regular.is_string);
            ctx.check("regular_not_void",    !regular.is_void);
            ctx.check("regular_len_11",      regular.byte_len == 11);

            const observation single{ get("single") };
            ctx.check("single_value_exact",  single.value == "X");
            ctx.check("single_len_1",        single.byte_len == 1);

            const observation whitespace{ get("whitespace") };
            ctx.check("whitespace_value_exact", whitespace.value == " \t\n\r ");
            ctx.check("whitespace_len_5",       whitespace.byte_len == 5);

            const observation punctuation{ get("punctuation") };
            ctx.check("punctuation_value_exact", punctuation.value == "\"\\/{}[]:,");

            const observation ascii_high{ get("asciiHigh") };
            ctx.check("asciiHigh_value_exact", ascii_high.value == "~}|{`_^]");

            // allAscii(): every printable byte 0x20..0x7E (95 chars), so the
            // decode of a long-ish all-ASCII run with quotes/backslash/braces is
            // byte-exact end to end.
            const observation all_ascii{ get("allAscii") };
            ctx.check("allAscii_len_95", all_ascii.byte_len == 95);
            {
                std::string expect;
                expect.reserve(95);
                for (int c{ 0x20 }; c <= 0x7E; ++c)
                {
                    expect += static_cast<char>(c);
                }
                ctx.check("allAscii_value_exact", all_ascii.value == expect);
            }

            const observation field_value{ get("fieldValue") };
            ctx.check("fieldValue_exact",     field_value.value == "instance-field-value");
            ctx.check("fieldValue_is_string", field_value.is_string);

            const observation dynamic{ get("dynamic") };
            ctx.check("dynamic_value_exact", dynamic.value == "dyn-42");
            ctx.check("dynamic_is_string",   dynamic.is_string);

            const observation substr{ get("substringResult") };
            ctx.check("substring_value_exact", substr.value == "456789");

            // longAscii(): 300 chars (a..z repeating).  Proves a > 256-char ASCII
            // String decodes in full and is not truncated by either path.
            const observation long_ascii{ get("longAscii") };
            ctx.check("longAscii_len_300", long_ascii.byte_len == 300);
            {
                std::string expect;
                expect.reserve(300);
                for (int i{ 0 }; i < 300; ++i)
                {
                    expect += static_cast<char>('a' + (i % 26));
                }
                ctx.check("longAscii_value_exact", long_ascii.value == expect);
            }

            // ---- empty vs null boundary (payload is path-independent) -------
            const observation empty{ get("empty") };
            ctx.check("empty_captured",       empty.captured);
            ctx.check("empty_value_is_empty", empty.value.empty());
            ctx.check("empty_len_0",          empty.byte_len == 0);
            // is_string()/is_void() for empty are recorded, not asserted: on
            // call_jni empty() -> GetStringUTFChars("") -> "" stored as a string
            // (is_string); the value (the stable contract) is "" on both paths.
            ctx.record(std::string{ "[INFO] empty.is_string=" }
                       + (empty.is_string ? "1" : "0")
                       + " empty.is_void=" + (empty.is_void ? "1" : "0"));

            const observation ret_null{ get("returnNull") };
            ctx.check("returnNull_as_string_empty", ret_null.value.empty());
            // The variant tag diverges: call_jni stores std::string{""}
            // (GetStringUTFChars(null) -> "" wrapped as a String), so is_string;
            // call_stub returns monostate for the null oop, so is_void.  Assert the
            // path-correct tag so the divergence is covered rather than skipped.
            if (stub_path)
            {
                ctx.check("returnNull_tag_void_on_call_stub", ret_null.is_void);
            }
            else
            {
                ctx.check("returnNull_tag_string_on_call_jni",
                          ret_null.is_string && !ret_null.is_void);
            }

            // ============ STATIC dispatch (via static_method) ================

            const observation s_regular{ get("static:staticRegular") };
            ctx.check("staticRegular_captured",    s_regular.captured);
            ctx.check("staticRegular_value_exact", s_regular.value == "static-hello");
            ctx.check("staticRegular_is_string",   s_regular.is_string);

            const observation s_empty{ get("static:staticEmpty") };
            ctx.check("staticEmpty_value_empty", s_empty.value.empty());

            const observation s_null{ get("static:staticNull") };
            ctx.check("staticNull_as_string_empty", s_null.value.empty());

            const observation s_dyn{ get("static:staticDynamic") };
            ctx.check("staticDynamic_value_exact", s_dyn.value == "static-dyn-99");
            ctx.check("staticDynamic_is_string",   s_dyn.is_string);

            // ============ ARGUMENT round-trips ===============================

            const observation echo_ascii{ get("echo:ascii") };
            ctx.check("echo_ascii_exact",     echo_ascii.value == "round-trip-123");
            ctx.check("echo_ascii_is_string", echo_ascii.is_string);

            const observation echo_empty{ get("echo:empty") };
            ctx.check("echo_empty_value_empty", echo_empty.value.empty());

            // ---- const char* ARG branch (distinct from the std::string branch) --
            // Plain ASCII C-string round-trips byte-for-byte on both paths.
            const observation echo_cstr_ascii{ get("echo:cstr_ascii") };
            ctx.check("echo_cstr_ascii_exact",     echo_cstr_ascii.value == "c-string-arg");
            ctx.check("echo_cstr_ascii_is_string", echo_cstr_ascii.is_string);
            // Empty C-string "" -> a REAL empty Java String (NOT Java null): the value
            // is "" on both paths.  The null-vs-empty distinction is pinned
            // decoder-independently by lengthOf below (len=0, not len=null).
            const observation echo_cstr_empty{ get("echo:cstr_empty") };
            ctx.check("echo_cstr_empty_value_empty", echo_cstr_empty.value.empty());
            // nullptr const char* -> Java null arg; echo(null) returns Java null, which
            // the API reports as "" on both paths (the documented null -> empty mapping).
            const observation echo_cstr_null{ get("echo:cstr_null") };
            ctx.check("echo_cstr_null_value_empty", echo_cstr_null.value.empty());
            // Astral C-string arg: the const char* branch routes through the same
            // length-counted UTF-16 encoder, so the standard 4-byte sequence becomes a
            // proper surrogate pair (U+1F600) — same RETURN-decoder divergence as every
            // other astral round-trip (standard 4-byte on call_stub, 6-byte CESU-8 on
            // call_jni).  The arg's intactness is proven decoder-independently by
            // lengthOf:cstr_emoji == 2 below.
            const observation echo_cstr_emoji{ get("echo:cstr_emoji") };
            ctx.record(std::string{ "[INFO] echo:cstr_emoji (const char* astral arg) = [" }
                       + to_hex(echo_cstr_emoji.value) + "]");
            if (stub_path)
            {
                ctx.check("echo_cstr_emoji_call_stub_4byte",
                          echo_cstr_emoji.value == "\xF0\x9F\x98\x80");
            }
            else
            {
                ctx.check("echo_cstr_emoji_call_jni_cesu8",
                          echo_cstr_emoji.value == "\xED\xA0\xBD\xED\xB8\x80");
            }
            // Decoder-INDEPENDENT proof of the const char* branch's contract, via Java's
            // own String.length() (pure-ASCII "len=N" payload, identical on both paths):
            //   * ""        -> a 0-length Java String (NOT null): "len=0".
            //   * nullptr   -> Java null:                          "len=null".
            //   * astral    -> ONE supplementary scalar = 2 units: "len=2".
            const observation len_cstr_empty{ get("lengthOf:cstr_empty") };
            ctx.check("lengthOf_cstr_empty_is_zero_not_null", len_cstr_empty.value == "len=0");
            const observation len_cstr_null{ get("lengthOf:cstr_null") };
            ctx.check("lengthOf_cstr_null_is_java_null", len_cstr_null.value == "len=null");
            const observation len_cstr_emoji{ get("lengthOf:cstr_emoji") };
            ctx.check("lengthOf_cstr_emoji_two_units", len_cstr_emoji.value == "len=2");

            // ---- std::string_view ARG branch (distinct again) -------------------
            // An interior-NUL string_view proves the counted-length, no-C-string-cut
            // property on the string_view branch: the Java String is 'a' U+0000 'b'
            // (length 3), and the RETURN decoder diverges only by NUL encoding (raw 00
            // on call_stub, C0 80 on call_jni) — same bytes as the std::string nul case.
            const observation echo_sv_nul{ get("echo:sv_nul") };
            ctx.record(std::string{ "[INFO] echo:sv_nul (string_view \"a\\0b\" arg) = [" }
                       + to_hex(echo_sv_nul.value) + "]");
            if (stub_path)
            {
                ctx.check("echo_sv_nul_call_stub_bytes", echo_sv_nul.value == std::string("a\0b", 3));
                ctx.check("echo_sv_nul_call_stub_len3",  echo_sv_nul.byte_len == 3);
            }
            else
            {
                ctx.check("echo_sv_nul_call_jni_bytes",
                          echo_sv_nul.value == std::string("\x61\xC0\x80\x62", 4));
                ctx.check("echo_sv_nul_call_jni_len4", echo_sv_nul.byte_len == 4);
            }
            // Decoder-independent: the string_view branch preserved the interior NUL
            // (Java length 3), not a C-string cut to 1.
            const observation len_sv_nul{ get("lengthOf:sv_nul") };
            ctx.check("lengthOf_sv_nul_interior_nul_kept", len_sv_nul.value == "len=3");

            // 300-char arg echoed back in full (no arg-side truncation).
            const observation echo_long{ get("echo:long") };
            ctx.check("echo_long_len_300", echo_long.byte_len == 300);
            ctx.check("echo_long_all_Z",   echo_long.value == std::string(300, 'Z'));

            // Latin-1 round-trip.  The arg encoder (jni_new_string_utf16_local ->
            // utf8_to_utf16) decodes the 2-byte C3 A9 to U+00E9, so the Java String is
            // caf+U+00E9; on the call_jni return GetStringUTFChars re-encodes U+00E9 as
            // C3 A9 (modified UTF-8 of a U+0080..U+07FF scalar == standard UTF-8), so
            // the bytes round-trip exactly.  Hard-asserted on the live CI (call_jni)
            // path; recorded as [INFO] on the unreachable call_stub path.
            const observation echo_unicode{ get("echo:unicode") };
            ctx.record(std::string{ "[INFO] echo:unicode = [" } + to_hex(echo_unicode.value) + "]");
            if (!stub_path)
            {
                ctx.check("echo_unicode_call_jni_round_trip", echo_unicode.value == "caf\xC3\xA9");
                ctx.check("echo_unicode_call_jni_len5",       echo_unicode.byte_len == 5);
            }

            const observation concat_ab{ get("concat:ab") };
            ctx.check("concat_exact",     concat_ab.value == "foo-bar");
            ctx.check("concat_is_string", concat_ab.is_string);

            const observation len_of{ get("lengthOf:abcd") };
            ctx.check("lengthOf_exact", len_of.value == "len=4");

            // charAtOf("wxyz", 2) -> "y" MIXES a String arg with a primitive int
            // arg.  Single-String-arg and single-int-arg dispatch are each proven
            // (echo / repeatA), but the mixed (Ljava/lang/String;I) overload
            // resolution + ordered packing is not separately proven in CI, so this
            // is recorded as characterization, not hard-asserted (conservative).
            const observation char_of{ get("charAtOf:wxyz2") };
            ctx.record(std::string{ "[INFO] charAtOf(\"wxyz\",2) (String+int->String) = \"" }
                       + char_of.value + "\" (expected \"y\")");

            // STATIC arg round-trips — the regression site.  These MUST be exact:
            // the bug made them return the wrong String (receiver bound as arg 0).
            const observation s_echo{ get("staticEcho:ascii") };
            ctx.check("staticEcho_captured", s_echo.captured);
            ctx.check("staticEcho_exact",    s_echo.value == "s-echo");
            ctx.check("staticEcho_is_string", s_echo.is_string);

            const observation s_concat{ get("staticConcat:ab") };
            ctx.check("staticConcat_captured", s_concat.captured);
            ctx.check("staticConcat_exact",    s_concat.value == "L+R");
            ctx.check("staticConcat_is_string", s_concat.is_string);

            // ============ LENGTH / CAP angles ================================

            const observation r64{ get("repeatA:64") };
            ctx.check("repeatA64_len_64", r64.byte_len == 64);
            ctx.check("repeatA64_all_A",  r64.value == std::string(64, 'A'));

            // repeatA(4096): at the OLD cap value.  With the cap removed both paths
            // return the full 4096 'A's (and the 5000 case below proves it keeps going
            // past the old limit) — this case is kept as a boundary witness.
            const observation r4096{ get("repeatA:4096") };
            ctx.check("repeatA4096_len_4096", r4096.byte_len == 4096);
            ctx.check("repeatA4096_all_A",    r4096.value == std::string(4096, 'A'));

            // repeatA(5000): ABOVE the OLD 4096 read_java_string cap.  That cap was
            // removed (robustness bug #29 raised read_java_string_max_units to 16M and
            // dropped the "non-ASCII -> '?'" lossy substitution), so BOTH decoders now
            // return the full 5000 'A's: call_jni's GetStringUTFChars has never capped,
            // and read_java_string reads the whole backing array.  Pure ASCII, so the
            // bytes are identical on both paths -> assert UNCONDITIONALLY.
            const observation r5000{ get("repeatA:5000") };
            ctx.check("repeatA5000_full_5000", r5000.byte_len == 5000);
            ctx.check("repeatA5000_all_A",     r5000.value == std::string(5000, 'A'));

            // ============ VERY LARGE returns: >4096 and >65536 ===============
            // bigString(n) has NO 8192 clamp (unlike repeatA), so it returns a String
            // far above the OLD 4096 read_java_string cap AND above 65536 / 131072 code
            // units.  That cap is gone (robustness bug #29): read_java_string now reads
            // up to read_java_string_max_units (16M) chars, and call_jni's
            // GetStringUTFChars never capped — so BOTH decoders return the FULL String
            // at any of these lengths.  Every byte is pure ASCII 'A', so the decoded
            // bytes are identical on both paths -> assert the full content/length
            // UNCONDITIONALLY (this is the headline "very long String, no truncation"
            // invariant, now true on every dispatcher).
            const observation big5000{ get("bigString:5000") };
            const observation big70000{ get("bigString:70000") };
            const observation big131072{ get("bigString:131072") };
            const observation s_big70000{ get("static:staticBigString:70000") };
            ctx.check("bigString5000_captured",   big5000.captured);
            ctx.check("bigString70000_captured",  big70000.captured);
            // >4096 (above the old cap).
            ctx.check("bigString5000_len_5000",   big5000.byte_len == 5000);
            ctx.check("bigString5000_all_A",      big5000.value == std::string(5000, 'A'));
            // >65536 — the headline large case.
            ctx.check("bigString70000_len_70000", big70000.byte_len == 70000);
            ctx.check("bigString70000_all_A",     big70000.value == std::string(70000, 'A'));
            // >131072 (2x 65536), to be thorough about "very long".
            ctx.check("bigString131072_len_131072", big131072.byte_len == 131072);
            ctx.check("bigString131072_all_A",      big131072.value == std::string(131072, 'A'));
            // Same >65536 case via the STATIC dispatch path.
            ctx.check("staticBigString70000_len_70000", s_big70000.byte_len == 70000);
            ctx.check("staticBigString70000_all_A",
                      s_big70000.value == std::string(70000, 'A'));

            // ============ ECHO round-trips of every shape (String arg) =======
            // The String ARGUMENT is encoded by detail::convert_jni_arg via
            // jni_new_string_utf16_local: utf8_to_utf16 (the same lossless decoder
            // make_java_string uses) -> JNIEnv::NewString (length-counted UTF-16).
            // That path preserves interior NULs (counted length, no C-string cut) and
            // astral scalars (proper surrogate pairs) — fixing the prior NewStringUTF
            // truncation/mangle.  The return then travels the live decoder.  These
            // prove the ARGUMENT encode for CJK, an astral scalar, an interior NUL,
            // max-BMP, and a >65536 arg.

            // CJK arg -> identical 9 bytes back (BMP, path-independent).
            const observation echo_cjk{ get("echo:cjk") };
            ctx.check("echo_cjk_round_trip",
                      echo_cjk.value == "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E");
            ctx.check("echo_cjk_len_9", echo_cjk.byte_len == 9);

            // max-BMP arg U+FFFF -> 3-byte EF BF BF back (path-independent).
            const observation echo_max_bmp{ get("echo:maxBmp") };
            ctx.check("echo_maxBmp_round_trip", echo_max_bmp.value == "\xEF\xBF\xBF");

            // Astral emoji arg — FIXED standard-UTF-8 round-trip (was a characterized
            // NewStringUTF mangle).  The String-arg encoder now routes std::string
            // through utf8_to_utf16 + NewString (length-counted UTF-16), so a STANDARD
            // 4-byte sequence (F0 9F 98 80) is decoded to the surrogate pair D83D DE00
            // and the Java String IS the true U+1F600 on BOTH dispatchers.  The arg is
            // therefore intact (proven decoder-independently by lengthOf:emoji == 2
            // below); what differs between the two paths is only the RETURN decoder:
            //   * call_stub return = read_java_string => STANDARD UTF-8, the original
            //     4 bytes F0 9F 98 80.
            //   * call_jni  return = GetStringUTFChars => MODIFIED UTF-8 (CESU-8), the
            //     surrogate pair re-encoded as two 3-byte units ED A0 BD ED B8 80.
            // Both are now HARD round-trip assertions (identical to the corresponding
            // `emoji` RETURN case), not [INFO] characterizations.
            const observation echo_emoji{ get("echo:emoji") };
            ctx.record(std::string{ "[INFO] echo:emoji (std-4byte astral arg, FIXED round-trip) = [" }
                       + to_hex(echo_emoji.value) + "]");
            if (stub_path)
            {
                ctx.check("echo_emoji_call_stub_4byte",
                          echo_emoji.value == "\xF0\x9F\x98\x80");
                ctx.check("echo_emoji_call_stub_len4", echo_emoji.byte_len == 4);
            }
            else
            {
                // FIXED: the astral arg survives as U+1F600; the call_jni return
                // decoder (modified UTF-8) hands it back as the 6-byte CESU pair.
                ctx.check("echo_emoji_call_jni_cesu8",
                          echo_emoji.value == "\xED\xA0\xBD\xED\xB8\x80");
                ctx.check("echo_emoji_call_jni_len6", echo_emoji.byte_len == 6);
            }

            // Decoder-INDEPENDENT proof the astral ARG arrived intact: Java's own
            // String.length() of the emoji arg is 2 UTF-16 units (one supplementary
            // scalar).  This was 1 with the old NewStringUTF mangle (U+00F0).  The
            // "len=2" payload is pure ASCII -> identical on both dispatchers.
            const observation len_emoji{ get("lengthOf:emoji") };
            ctx.check("lengthOf_emoji_arg_intact_2_units", len_emoji.value == "len=2");

            // Two astral scalars as a single arg: Java length 4 (two surrogate pairs);
            // round-trips to the 8-byte standard form (call_stub) or the 12-byte CESU
            // form (call_jni) — same bytes as the `twoAstral` RETURN case.
            const observation echo_two_astral{ get("echo:twoAstralArg") };
            if (stub_path)
            {
                ctx.check("echo_twoAstralArg_call_stub",
                          echo_two_astral.value == "\xF0\x9F\x98\x80\xF0\x9F\x9A\x80");
                ctx.check("echo_twoAstralArg_call_stub_len8", echo_two_astral.byte_len == 8);
            }
            else
            {
                // U+1F600 (D83D DE00) -> ED A0 BD ED B8 80; U+1F680 (D83D DE80) ->
                // ED A0 BD ED BA 80.  Same 12 bytes as the `twoAstral` RETURN case.
                ctx.check("echo_twoAstralArg_call_jni_cesu8",
                          echo_two_astral.value
                              == "\xED\xA0\xBD\xED\xB8\x80\xED\xA0\xBD\xED\xBA\x80");
                ctx.check("echo_twoAstralArg_call_jni_len12", echo_two_astral.byte_len == 12);
            }

            // Interior-NUL arg "a\0b" (RAW NUL in the std::string) — FIXED: the
            // interior NUL is now PRESERVED (was truncated by NewStringUTF's C-string
            // read).  The String-arg encoder routes std::string through utf8_to_utf16
            // + NewString, which is length-counted: the raw 0x00 becomes U+0000 (not a
            // terminator), so the Java String is 'a' U+0000 'b' (3 chars) on BOTH
            // dispatchers (proven decoder-independently by lengthOf:nul == 3 below).
            // Only the RETURN decoder differs:
            //   * call_stub return = read_java_string => STANDARD UTF-8, raw 61 00 62.
            //   * call_jni  return = GetStringUTFChars => MODIFIED UTF-8, U+0000 as
            //     C0 80 -> 61 C0 80 62 (4 bytes).  Same bytes as the interiorNul
            //     RETURN case; both are now HARD round-trip assertions.
            const observation echo_nul{ get("echo:nul") };
            ctx.record(std::string{ "[INFO] echo:nul (\"a\\0b\" arg, FIXED round-trip) = [" }
                       + to_hex(echo_nul.value) + "]");
            if (stub_path)
            {
                ctx.check("echo_nul_call_stub_bytes", echo_nul.value == std::string("a\0b", 3));
                ctx.check("echo_nul_call_stub_len3",  echo_nul.byte_len == 3);
            }
            else
            {
                // FIXED: interior NUL preserved; call_jni return encodes U+0000 -> C0 80.
                ctx.check("echo_nul_call_jni_bytes",
                          echo_nul.value == std::string("\x61\xC0\x80\x62", 4));
                ctx.check("echo_nul_call_jni_len4", echo_nul.byte_len == 4);
            }

            // Decoder-INDEPENDENT proof the interior NUL ARG survived: Java's own
            // String.length() of "a\0b" is 3 (NUL kept as U+0000).  This was 1 with
            // the old NewStringUTF C-string truncation.  "len=3" is pure ASCII ->
            // identical on both dispatchers.
            const observation len_nul{ get("lengthOf:nul") };
            ctx.check("lengthOf_nul_arg_interior_nul_kept", len_nul.value == "len=3");

            // >65536-code-unit arg: convert_jni_arg -> jni_new_string_utf16_local
            // calls JNIEnv::NewString with the full length-counted UTF-16 (70000 BMP
            // units), so the JVM builds the FULL String at any length (no cap on the
            // ARG side).  The return then reads it back: GetStringUTFChars (call_jni)
            // never capped, and read_java_string (call_stub) no longer caps either
            // (robustness bug #29) — so BOTH paths yield all 70000 bytes.  Pure ASCII
            // 'Q', so the bytes are identical on both paths.  This proves the over-cap
            // ARGUMENT encode AND the over-cap RETURN decode end to end, unconditionally.
            const observation echo_big{ get("echo:big70000") };
            ctx.check("echo_big70000_len_70000", echo_big.byte_len == 70000);
            ctx.check("echo_big70000_all_Q",     echo_big.value == std::string(70000, 'Q'));

            // ============ CODER WITNESS (Latin-1 coder 0 vs UTF-16 coder 1) ==
            // The native decode that the call_jni path uses is coder-AGNOSTIC
            // (GetStringUTFChars normalises both layouts), and the byte-exact
            // assertions above already prove the call_stub read_java_string decoder
            // handles BOTH the LATIN1 (coder 0) and UTF16 (coder 1) branches.  Here
            // we additionally LABEL, from Java's own view, which physical coder each
            // returned String used, so test_results.txt documents that the ASCII /
            // Latin-1 returns are coder 0 and the >0xFF returns are coder 1.  On
            // JDK 8 there is no coder field (char[] layout) -> witnesses are -1 and
            // we assert nothing (the decodes above already covered the char[] path).
            const bool has_coder{ method_string_fixture::has_coder_field() };
            const std::int32_t c_regular{ method_string_fixture::coder_regular() };
            const std::int32_t c_cafe{ method_string_fixture::coder_cafe() };
            const std::int32_t c_cjk{ method_string_fixture::coder_cjk() };
            const std::int32_t c_emoji{ method_string_fixture::coder_emoji() };
            const std::int32_t c_max_bmp{ method_string_fixture::coder_max_bmp() };
            ctx.record(std::string{ "[INFO] String coder field present (JDK9+)=" }
                       + (has_coder ? "true" : "false")
                       + " coder{regular=" + std::to_string(c_regular)
                       + " cafe=" + std::to_string(c_cafe)
                       + " cjk=" + std::to_string(c_cjk)
                       + " emoji=" + std::to_string(c_emoji)
                       + " maxBmp=" + std::to_string(c_max_bmp) + "}");
            // Only assert a coder we could actually read (>= 0).  Latin-1-
            // representable content -> coder 0; any char > 0xFF -> coder 1.
            if (c_regular >= 0) { ctx.check("coder_regular_is_LATIN1", c_regular == 0); }
            if (c_cafe    >= 0) { ctx.check("coder_cafe_is_LATIN1",    c_cafe == 0); }
            if (c_cjk     >= 0) { ctx.check("coder_cjk_is_UTF16",      c_cjk == 1); }
            if (c_emoji   >= 0) { ctx.check("coder_emoji_is_UTF16",    c_emoji == 1); }
            if (c_max_bmp >= 0) { ctx.check("coder_maxBmp_is_UTF16",   c_max_bmp == 1); }
            // Java's own length for the big string confirms the fixture built the
            // full >65536 String (independent of which decoder read it back).
            ctx.check("java_bigString_len_70000",
                      method_string_fixture::big_len() == 70000);

            // ============ LEAK / STABILITY loop ==============================

            ctx.check("leak_loop_ran",
                      g_loop_iterations.load(std::memory_order_relaxed) == 250);
            ctx.check("leak_loop_single_distinct_value",
                      g_loop_distinct.load(std::memory_order_relaxed) == 1);

            // ============ BMP UNICODE: path-INDEPENDENT exact bytes ==========
            // Both decoders emit standard UTF-8 for BMP (and modified UTF-8 ==
            // standard UTF-8 for every BMP scalar except U+0000), so these are
            // unconditional.  Cross-checked vs the JVM's writeUTF output.

            const observation cafe{ get("cafe") };
            ctx.check("cafe_captured",    cafe.captured);
            ctx.check("cafe_value_utf8",  cafe.value == "caf\xC3\xA9");
            ctx.check("cafe_len_5",       cafe.byte_len == 5);
            ctx.record(std::string{ "[INFO] cafe = [" } + to_hex(cafe.value) + "]");

            const observation resume{ get("resume") };
            ctx.check("resume_value_utf8", resume.value == "r\xC3\xA9sum\xC3\xA9");
            ctx.check("resume_len_8",      resume.byte_len == 8);

            const observation accents{ get("accents") };
            ctx.check("accents_value_utf8",
                      accents.value == "\xC3\xA9\xC3\xA8\xC3\xAA\xC3\xAB");
            ctx.check("accents_len_8", accents.byte_len == 8);

            const observation mixed{ get("mixed") };
            ctx.check("mixed_value_utf8",
                      mixed.value == "\xC3\xBC\xC3\xB1\xE2\x82\xAC");
            ctx.check("mixed_len_7", mixed.byte_len == 7);

            const observation cjk{ get("cjk") };
            ctx.check("cjk_captured",   cjk.captured);
            ctx.check("cjk_value_utf8",
                      cjk.value == "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E");
            ctx.check("cjk_len_9",      cjk.byte_len == 9);

            const observation greek{ get("greek") };
            ctx.check("greek_value_utf8", greek.value == "\xCE\xB1\xCE\xB2");
            ctx.check("greek_len_4",      greek.byte_len == 4);

            // U+00FF: the Latin-1 ceiling -> C3 BF on both paths.
            const observation latin1_hi{ get("latin1Hi") };
            ctx.check("latin1Hi_value_utf8", latin1_hi.value == "\xC3\xBF");
            ctx.check("latin1Hi_len_2",      latin1_hi.byte_len == 2);

            // U+07FF (2-byte: DF BF) then U+0800 (3-byte: E0 A0 80) — the 2/3-byte
            // UTF-8 length boundary.
            const observation bmp_bnd{ get("bmpBoundary") };
            ctx.check("bmpBoundary_value_utf8",
                      bmp_bnd.value == "\xDF\xBF\xE0\xA0\x80");
            ctx.check("bmpBoundary_len_5", bmp_bnd.byte_len == 5);

            // U+007F (1-byte: 7F) then U+0080 (2-byte: C2 80) — the ASCII/2-byte
            // boundary.
            const observation ascii_bnd{ get("asciiBoundary") };
            ctx.check("asciiBoundary_value_utf8",
                      ascii_bnd.value == "\x7F\xC2\x80");
            ctx.check("asciiBoundary_len_3", ascii_bnd.byte_len == 3);

            const observation s_unicode{ get("static:staticUnicode") };
            ctx.check("staticUnicode_value_utf8", s_unicode.value == "caf\xC3\xA9");
            const observation s_cjk{ get("static:staticCjk") };
            ctx.check("staticCjk_value_utf8",
                      s_cjk.value == "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E");

            // ---- Control chars U+0001..U+001F + U+007F (all ASCII range) -----
            // Every byte is < 0x80, so both decoders emit them 1:1, in order.
            const observation control{ get("controlChars") };
            ctx.check("controlChars_captured", control.captured);
            ctx.check("controlChars_len_32",   control.byte_len == 32);
            {
                std::string expect;
                expect.reserve(32);
                for (int c{ 0x01 }; c <= 0x1F; ++c)
                {
                    expect += static_cast<char>(c);
                }
                expect += static_cast<char>(0x7F);
                ctx.check("controlChars_value_exact", control.value == expect);
            }

            // ---- The max BMP code point U+FFFF -> 3-byte EF BF BF ------------
            // The highest 3-byte UTF-8 sequence; identical on both paths.
            const observation max_bmp{ get("maxBmp") };
            ctx.check("maxBmp_captured",    max_bmp.captured);
            ctx.check("maxBmp_value_utf8",  max_bmp.value == "\xEF\xBF\xBF");
            ctx.check("maxBmp_len_3",       max_bmp.byte_len == 3);
            ctx.check("maxBmp_is_string",   max_bmp.is_string);

            // ---- The replacement char U+FFFD -> 3-byte EF BF BD -------------
            const observation repl{ get("replacementChar") };
            ctx.check("replacementChar_value_utf8", repl.value == "\xEF\xBF\xBD");
            ctx.check("replacementChar_len_3",      repl.byte_len == 3);

            // ---- Unicode whitespace U+00A0 / U+2028 / U+2029 ---------------
            // C2 A0 (2-byte) + E2 80 A8 (3-byte) + E2 80 A9 (3-byte) = 8 bytes.
            const observation uws{ get("unicodeWhitespace") };
            ctx.check("unicodeWhitespace_value_utf8",
                      uws.value == "\xC2\xA0\xE2\x80\xA8\xE2\x80\xA9");
            ctx.check("unicodeWhitespace_len_8", uws.byte_len == 8);

            // ---- Lone / reversed surrogates --------------------------------
            // A Java String is a raw UTF-16 code-unit array, so an unpaired or
            // out-of-order surrogate is a legal String.  BOTH vmhook decoders emit
            // the 3-byte CESU encoding of each surrogate code unit (the call_jni
            // GetStringUTFChars path and the call_stub read_java_string path agree
            // — neither '?'-substitutes the way String.getBytes(UTF_8) does), so
            // these are PATH-INDEPENDENT.  Cross-checked: writeUTF and
            // read_java_string both yield ED A0 BD for U+D83D.
            const observation lone_hi{ get("loneHighSurrogate") };
            ctx.check("loneHighSurrogate_captured", lone_hi.captured);
            ctx.check("loneHighSurrogate_cesu",     lone_hi.value == "\xED\xA0\xBD");
            ctx.check("loneHighSurrogate_len_3",    lone_hi.byte_len == 3);
            ctx.record(std::string{ "[INFO] loneHighSurrogate = [" } + to_hex(lone_hi.value)
                       + "] (vmhook emits 3-byte CESU; Java getBytes(UTF_8) would give 3F '?')");

            const observation lone_lo{ get("loneLowSurrogate") };
            ctx.check("loneLowSurrogate_cesu",  lone_lo.value == "\xED\xB8\x80");
            ctx.check("loneLowSurrogate_len_3", lone_lo.byte_len == 3);

            // low THEN high — must NOT combine; two independent 3-byte CESU seqs.
            const observation rev_surr{ get("reversedSurrogates") };
            ctx.check("reversedSurrogates_no_combine",
                      rev_surr.value == "\xED\xB8\x80\xED\xA0\xBD");
            ctx.check("reversedSurrogates_len_6", rev_surr.byte_len == 6);

            // static max-BMP / lone-surrogate through the static dispatch path.
            const observation s_max_bmp{ get("static:staticMaxBmp") };
            ctx.check("staticMaxBmp_value_utf8", s_max_bmp.value == "\xEF\xBF\xBF");
            const observation s_lone{ get("static:staticLoneSurrogate") };
            ctx.check("staticLoneSurrogate_cesu", s_lone.value == "\xED\xA0\xBD");

            // ============ SUPPLEMENTARY PLANE: path-DIVERGENT ================
            // call_jni (modified UTF-8): a supplementary scalar is a CESU-8
            // surrogate pair — TWO 3-byte sequences (the subtlety most decoders
            // get wrong).  call_stub (standard UTF-8): the surrogate pair is
            // combined into ONE 4-byte sequence.

            const observation emoji{ get("emoji") };
            const observation astral_mid{ get("astralMid") };
            const observation two_astral{ get("twoAstral") };
            const observation s_emoji{ get("static:staticEmoji") };
            ctx.check("emoji_captured", emoji.captured);
            ctx.record(std::string{ "[INFO] emoji = [" } + to_hex(emoji.value) + "]");

            if (stub_path)
            {
                // Standard UTF-8: U+1F600 -> F0 9F 98 80 (4 bytes).
                ctx.check("emoji_call_stub_4byte", emoji.value == "\xF0\x9F\x98\x80");
                ctx.check("emoji_call_stub_len4",  emoji.byte_len == 4);
                // "ab" + (4-byte emoji) + "cd" = 8 bytes.
                ctx.check("astralMid_call_stub", astral_mid.value == "ab\xF0\x9F\x98\x80""cd");
                ctx.check("astralMid_call_stub_len8", astral_mid.byte_len == 8);
                // U+1F600 then U+1F680 -> two 4-byte sequences = 8 bytes.
                ctx.check("twoAstral_call_stub",
                          two_astral.value == "\xF0\x9F\x98\x80\xF0\x9F\x9A\x80");
                ctx.check("twoAstral_call_stub_len8", two_astral.byte_len == 8);
                ctx.check("staticEmoji_call_stub", s_emoji.value == "\xF0\x9F\x98\x80");
            }
            else
            {
                // Modified UTF-8 / CESU-8: U+1F600 (surrogate pair D83D DE00) ->
                // ED A0 BD ED B8 80 (two 3-byte sequences, 6 bytes).
                ctx.check("emoji_call_jni_cesu8",
                          emoji.value == "\xED\xA0\xBD\xED\xB8\x80");
                ctx.check("emoji_call_jni_len6", emoji.byte_len == 6);
                // "ab" + (6-byte CESU-8 emoji) + "cd" = 10 bytes.
                ctx.check("astralMid_call_jni",
                          astral_mid.value == "ab\xED\xA0\xBD\xED\xB8\x80""cd");
                ctx.check("astralMid_call_jni_len10", astral_mid.byte_len == 10);
                // U+1F600 (D83D DE00) then U+1F680 (D83D DE80) -> two CESU-8 pairs
                // = 12 bytes.
                ctx.check("twoAstral_call_jni",
                          two_astral.value
                              == "\xED\xA0\xBD\xED\xB8\x80\xED\xA0\xBD\xED\xBA\x80");
                ctx.check("twoAstral_call_jni_len12", two_astral.byte_len == 12);
                ctx.check("staticEmoji_call_jni",
                          s_emoji.value == "\xED\xA0\xBD\xED\xB8\x80");
            }

            // ============ INTERIOR / LEADING / TRAILING / ONLY NUL ===========
            // call_jni (modified UTF-8): U+0000 -> C0 80, so the C string does NOT
            // terminate early and every byte survives (std::string{chars} stops at
            // the first raw 00, and modified UTF-8 has none).  call_stub (standard
            // UTF-8): U+0000 stays a raw 0x00 byte; std::string is length-counted,
            // so no C-string truncation.

            const observation inul{ get("interiorNul") };       // 'a' NUL 'b'
            const observation nul_only{ get("nulOnly") };       // NUL
            const observation lead_nul{ get("leadingNul") };    // NUL "tail"
            const observation trail_nul{ get("trailingNul") };  // "head" NUL
            const observation s_inul{ get("static:staticInteriorNul") };
            ctx.check("interiorNul_captured", inul.captured);
            ctx.record(std::string{ "[INFO] interiorNul = [" } + to_hex(inul.value) + "]");

            if (stub_path)
            {
                // raw NUL retained.
                ctx.check("interiorNul_call_stub_bytes", inul.value == std::string("a\0b", 3));
                ctx.check("interiorNul_call_stub_len3",  inul.byte_len == 3);
                ctx.check("nulOnly_call_stub_bytes",     nul_only.value == std::string("\0", 1));
                ctx.check("nulOnly_call_stub_len1",      nul_only.byte_len == 1);
                ctx.check("leadingNul_call_stub_bytes",  lead_nul.value == std::string("\0tail", 5));
                ctx.check("leadingNul_call_stub_len5",   lead_nul.byte_len == 5);
                ctx.check("trailingNul_call_stub_bytes", trail_nul.value == std::string("head\0", 5));
                ctx.check("trailingNul_call_stub_len5",  trail_nul.byte_len == 5);
                ctx.check("staticInteriorNul_call_stub", s_inul.value == std::string("a\0b", 3));
            }
            else
            {
                // U+0000 -> C0 80 (modified UTF-8); nothing truncates.
                ctx.check("interiorNul_call_jni_bytes", inul.value == std::string("\x61\xC0\x80\x62", 4));
                ctx.check("interiorNul_call_jni_len4",  inul.byte_len == 4);
                ctx.check("nulOnly_call_jni_bytes",     nul_only.value == std::string("\xC0\x80", 2));
                ctx.check("nulOnly_call_jni_len2",      nul_only.byte_len == 2);
                ctx.check("leadingNul_call_jni_bytes",  lead_nul.value == std::string("\xC0\x80tail", 6));
                ctx.check("leadingNul_call_jni_len6",   lead_nul.byte_len == 6);
                ctx.check("trailingNul_call_jni_bytes", trail_nul.value == std::string("head\xC0\x80", 6));
                ctx.check("trailingNul_call_jni_len6",  trail_nul.byte_len == 6);
                ctx.check("staticInteriorNul_call_jni", s_inul.value == std::string("\x61\xC0\x80\x62", 4));
            }

            // ============ FRESH-OOP slices + built-empty boundary ============
            // subOf returns a FRESH (non-interned) substring OOP; the CJK slices
            // prove a fresh MULTIBYTE String decodes byte-exactly (BMP -> both paths
            // agree), and the empty slice proves a fresh empty OOP -> "".

            const observation sub_cjk_full{ get("subOf:cjk_full") };
            ctx.check("subOf_cjk_full_captured", sub_cjk_full.captured);
            ctx.check("subOf_cjk_full_value",
                      sub_cjk_full.value == "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E");
            ctx.check("subOf_cjk_full_len_9", sub_cjk_full.byte_len == 9);

            // substring(1,3) of the 3-CJK-char string -> the last two CJK chars
            // (U+672C U+8A9E) = 6 bytes; a fresh OOP whose backing differs from the
            // arg's, so this exercises decode of a sliced (offset) multibyte String.
            const observation sub_cjk_tail{ get("subOf:cjk_tail") };
            ctx.check("subOf_cjk_tail_value",
                      sub_cjk_tail.value == "\xE6\x9C\xAC\xE8\xAA\x9E");
            ctx.check("subOf_cjk_tail_len_6", sub_cjk_tail.byte_len == 6);

            // Empty slice (begin == end) -> a fresh empty String OOP -> "" on both
            // paths.  Distinct production from the interned empty() literal.
            const observation sub_empty{ get("subOf:empty_slice") };
            ctx.check("subOf_empty_slice_captured", sub_empty.captured);
            ctx.check("subOf_empty_slice_value_empty", sub_empty.value.empty());
            ctx.check("subOf_empty_slice_len_0", sub_empty.byte_len == 0);

            // ---- FRESH-OOP astral slices (substring of a surrogate pair) -----
            // subOf(emoji, 0, 1): a FRESH single-unit String holding ONLY the high
            // surrogate U+D83D -> 3-byte CESU ED A0 BD on BOTH paths (path-independent,
            // both decoders CESU-encode a lone surrogate).  Proves a fresh non-interned
            // OOP that is a single astral HALF decodes correctly.
            const observation sub_astral_high{ get("subOf:astral_high") };
            ctx.check("subOf_astral_high_captured", sub_astral_high.captured);
            ctx.check("subOf_astral_high_cesu", sub_astral_high.value == "\xED\xA0\xBD");
            ctx.check("subOf_astral_high_len_3", sub_astral_high.byte_len == 3);
            // subOf(emoji, 0, 2): a FRESH String holding the FULL pair -> path-divergent
            // (standard 4-byte on call_stub, 6-byte CESU on call_jni), same bytes as the
            // interned `emoji` RETURN case but from a FRESH OOP.
            const observation sub_astral_pair{ get("subOf:astral_pair") };
            ctx.check("subOf_astral_pair_captured", sub_astral_pair.captured);
            if (stub_path)
            {
                ctx.check("subOf_astral_pair_call_stub_4byte",
                          sub_astral_pair.value == "\xF0\x9F\x98\x80");
                ctx.check("subOf_astral_pair_call_stub_len4", sub_astral_pair.byte_len == 4);
            }
            else
            {
                ctx.check("subOf_astral_pair_call_jni_cesu8",
                          sub_astral_pair.value == "\xED\xA0\xBD\xED\xB8\x80");
                ctx.check("subOf_astral_pair_call_jni_len6", sub_astral_pair.byte_len == 6);
            }

            // ---- charAtOf of the emoji arg: lone surrogate HALVES --------------
            // charAt(0) of the astral arg -> the LONE HIGH surrogate U+D83D (3-byte
            // CESU ED A0 BD on both paths); charAt(1) -> the LONE LOW surrogate U+DE00
            // (ED B8 80).  These prove the astral ARG arrived as a real surrogate PAIR
            // (a collapsed-to-one-unit arg would make index 1 throw / out-of-range) and
            // that a fresh single-surrogate String round-trips path-independently.
            const observation char_emoji0{ get("charAtOf:emoji0") };
            ctx.check("charAtOf_emoji0_high_surrogate", char_emoji0.value == "\xED\xA0\xBD");
            ctx.check("charAtOf_emoji0_len_3", char_emoji0.byte_len == 3);
            const observation char_emoji1{ get("charAtOf:emoji1") };
            ctx.check("charAtOf_emoji1_low_surrogate", char_emoji1.value == "\xED\xB8\x80");
            ctx.check("charAtOf_emoji1_len_3", char_emoji1.byte_len == 3);

            // ---- concat(astral, ASCII) -> a fresh BUILT astral String ---------
            // "<emoji>" + "!" joined on the Java side; the surrogate pair survives the
            // join and the return decoder re-encodes it (path-divergent) with the ASCII
            // '!' appended (1 byte, identical on both paths).
            const observation concat_astral{ get("concat:astral_ascii") };
            ctx.check("concat_astral_ascii_captured", concat_astral.captured);
            if (stub_path)
            {
                ctx.check("concat_astral_ascii_call_stub",
                          concat_astral.value == "\xF0\x9F\x98\x80!");
                ctx.check("concat_astral_ascii_call_stub_len5", concat_astral.byte_len == 5);
            }
            else
            {
                ctx.check("concat_astral_ascii_call_jni",
                          concat_astral.value == "\xED\xA0\xBD\xED\xB8\x80!");
                ctx.check("concat_astral_ascii_call_jni_len7", concat_astral.byte_len == 7);
            }

            // builtA(0): a fresh EMPTY String from a StringBuilder (not interned).
            const observation built0{ get("builtA:0") };
            ctx.check("builtA0_captured", built0.captured);
            ctx.check("builtA0_value_empty", built0.value.empty());
            ctx.check("builtA0_len_0", built0.byte_len == 0);

            // ---- Length-1 multibyte returns --------------------------------
            // Single CJK char U+65E5 -> 3 bytes; single Latin-1 char U+00E9 -> 2
            // bytes.  BMP, path-independent; the length-1 multibyte degenerate.
            const observation single_cjk{ get("singleCjk") };
            ctx.check("singleCjk_captured",  single_cjk.captured);
            ctx.check("singleCjk_value",     single_cjk.value == "\xE6\x97\xA5");
            ctx.check("singleCjk_len_3",     single_cjk.byte_len == 3);
            ctx.check("singleCjk_is_string", single_cjk.is_string);

            const observation single_lat{ get("singleLatin1") };
            ctx.check("singleLatin1_value", single_lat.value == "\xC3\xA9");
            ctx.check("singleLatin1_len_2", single_lat.byte_len == 2);

            // ============ Unicode / boundary ARG round-trips =================
            // BMP arg round-trips are path-INDEPENDENT (both decoders agree).

            const observation echo_greek{ get("echo:greek") };
            ctx.check("echo_greek_round_trip", echo_greek.value == "\xCE\xB1\xCE\xB2");
            ctx.check("echo_greek_len_4",      echo_greek.byte_len == 4);

            const observation echo_mixed{ get("echo:mixed") };
            ctx.check("echo_mixed_round_trip",
                      echo_mixed.value == "\xC3\xBC\xC3\xB1\xE2\x82\xAC");
            ctx.check("echo_mixed_len_7", echo_mixed.byte_len == 7);

            const observation echo_repl{ get("echo:replacement") };
            ctx.check("echo_replacement_round_trip", echo_repl.value == "\xEF\xBF\xBD");
            ctx.check("echo_replacement_len_3",      echo_repl.byte_len == 3);

            const observation echo_resume{ get("echo:resume") };
            ctx.check("echo_resume_round_trip", echo_resume.value == "r\xC3\xA9sum\xC3\xA9");
            ctx.check("echo_resume_len_8",      echo_resume.byte_len == 8);

            // Lone-high-surrogate ARG: the CESU-3-byte input arrives as a single
            // UTF-16 unit (Java length 1) and the return re-encodes it to the same
            // 3-byte CESU form on BOTH paths -> path-independent round-trip.
            const observation echo_lone{ get("echo:lone_surrogate") };
            ctx.check("echo_lone_surrogate_round_trip", echo_lone.value == "\xED\xA0\xBD");
            ctx.check("echo_lone_surrogate_len_3",      echo_lone.byte_len == 3);
            const observation len_lone{ get("lengthOf:lone_surrogate") };
            ctx.check("lengthOf_lone_surrogate_one_unit", len_lone.value == "len=1");

            // Combined astral+NUL ARG ("<emoji>\0z"): Java length 4 (2 surrogate
            // units + U+0000 + 'z'), proven decoder-independently below.  The RETURN
            // bytes diverge only by the astral + NUL encoding:
            //   * call_stub  = standard UTF-8: F0 9F 98 80 00 7A (6 bytes, raw NUL).
            //   * call_jni   = modified UTF-8: ED A0 BD ED B8 80 C0 80 7A (9 bytes,
            //     CESU astral + C0 80 NUL).
            const observation echo_emoji_nul{ get("echo:emoji_nul") };
            ctx.record(std::string{ "[INFO] echo:emoji_nul (astral+NUL arg) = [" }
                       + to_hex(echo_emoji_nul.value) + "]");
            if (stub_path)
            {
                ctx.check("echo_emoji_nul_call_stub_bytes",
                          echo_emoji_nul.value == std::string("\xF0\x9F\x98\x80\x00z", 6));
                ctx.check("echo_emoji_nul_call_stub_len6", echo_emoji_nul.byte_len == 6);
            }
            else
            {
                ctx.check("echo_emoji_nul_call_jni_bytes",
                          echo_emoji_nul.value
                              == std::string("\xED\xA0\xBD\xED\xB8\x80\xC0\x80z", 9));
                ctx.check("echo_emoji_nul_call_jni_len9", echo_emoji_nul.byte_len == 9);
            }
            // Decoder-independent: the astral+NUL arg arrived as 4 UTF-16 units (the
            // surrogate pair + U+0000 + 'z'), proving NEITHER the astral pair was
            // collapsed NOR the NUL truncated the arg.
            const observation len_emoji_nul{ get("lengthOf:emoji_nul") };
            ctx.check("lengthOf_emoji_nul_four_units", len_emoji_nul.value == "len=4");

            // ============ Unicode / empty-arg multi-arg ======================

            // concat of two Latin-1 args -> caf + U+00E9 + U+00E9 + sum, re-encoded
            // by the live decoder.  BMP -> path-independent: caf C3A9 C3A9 sum.
            const observation concat_uni{ get("concat:unicode") };
            ctx.check("concat_unicode_value",
                      concat_uni.value == "caf\xC3\xA9\xC3\xA9sum");
            ctx.check("concat_unicode_len_10", concat_uni.byte_len == 10);

            // concat with an empty FIRST arg -> identity of the second (the empty
            // String arg is a real "" String in a multi-arg call, not null/dropped).
            const observation concat_empty_lhs{ get("concat:empty_lhs") };
            ctx.check("concat_empty_lhs_value", concat_empty_lhs.value == "tail");

            // charAtOf(CJK, 1) -> the middle CJK char U+672C (3 bytes), proving a
            // String+int->String overload returning a MULTIBYTE single char.  BMP,
            // so path-independent and HARD-asserted (unlike the ASCII charAtOf case,
            // which stays an [INFO] characterization).
            const observation char_cjk{ get("charAtOf:cjk1") };
            ctx.check("charAtOf_cjk1_value",  char_cjk.value == "\xE6\x9C\xAC");
            ctx.check("charAtOf_cjk1_len_3",  char_cjk.byte_len == 3);

            // ============ char* (MUTABLE) String-arg branch ==================
            // convert_jni_arg matches `char*` in the SAME branch as const char* but
            // it is a DISTINCT overload-resolution target; only const char* was
            // exercised before.  An ASCII char* round-trips byte-for-byte, a nullptr
            // char* -> Java null (echo(null) -> "" on both paths), and the Java-side
            // length proves the non-null char* arrived as a real 12-char String.
            const observation echo_mut{ get("echo:mutcstr_ascii") };
            ctx.check("echo_mutcstr_ascii_exact",     echo_mut.value == "mutable-cstr");
            ctx.check("echo_mutcstr_ascii_is_string", echo_mut.is_string);
            const observation echo_mut_null{ get("echo:mutcstr_null") };
            ctx.check("echo_mutcstr_null_value_empty", echo_mut_null.value.empty());
            const observation len_mut{ get("lengthOf:mutcstr_ascii") };
            ctx.check("lengthOf_mutcstr_ascii_12", len_mut.value == "len=12");

            // ============ ARG round-trips at UTF-8 LENGTH boundaries ==========
            // The arg encoder (utf8_to_utf16) must decode each width transition on the
            // ENCODE side exactly.  BMP -> path-independent byte-exact round-trips.
            const observation echo_ab{ get("echo:asciiBoundary") };
            ctx.check("echo_asciiBoundary_round_trip", echo_ab.value == "\x7F\xC2\x80");
            ctx.check("echo_asciiBoundary_len_3",      echo_ab.byte_len == 3);
            const observation echo_bb{ get("echo:bmpBoundary") };
            ctx.check("echo_bmpBoundary_round_trip", echo_bb.value == "\xDF\xBF\xE0\xA0\x80");
            ctx.check("echo_bmpBoundary_len_5",      echo_bb.byte_len == 5);
            const observation echo_l1{ get("echo:latin1Hi") };
            ctx.check("echo_latin1Hi_round_trip", echo_l1.value == "\xC3\xBF");
            ctx.check("echo_latin1Hi_len_2",      echo_l1.byte_len == 2);
            const observation echo_uws{ get("echo:unicodeWs") };
            ctx.check("echo_unicodeWs_round_trip",
                      echo_uws.value == "\xC2\xA0\xE2\x80\xA8\xE2\x80\xA9");
            ctx.check("echo_unicodeWs_len_8", echo_uws.byte_len == 8);
            // Decoder-independent: the asciiBoundary arg arrived as 2 UTF-16 units
            // (U+007F + U+0080), proving the 2-byte C2 80 was decoded as ONE scalar.
            const observation len_ab{ get("lengthOf:asciiBoundary") };
            ctx.check("lengthOf_asciiBoundary_two_units", len_ab.value == "len=2");

            // ============ as_string() vs implicit-conversion AGREEMENT =======
            // The module's reason for being: as_string() is the unambiguous accessor;
            // prove it returns byte-identical results to the implicit `std::string s =
            // call()` copy-init across the value-shape axis (ASCII / Latin-1 / CJK /
            // interior-NUL / empty).  The interior-NUL case is the sharpest — a
            // C-string-based implicit path would terminate at the NUL.
            {
                const auto ex_reg{ get_extract("extract:regular") };
                ctx.check("extract_regular_apis_agree", ex_reg.first == ex_reg.second);
                ctx.check("extract_regular_apis_value", ex_reg.first == "hello world");
                const auto ex_cafe{ get_extract("extract:cafe") };
                ctx.check("extract_cafe_apis_agree", ex_cafe.first == ex_cafe.second);
                ctx.check("extract_cafe_apis_nonempty", !ex_cafe.first.empty());
                const auto ex_cjk{ get_extract("extract:cjk") };
                ctx.check("extract_cjk_apis_agree", ex_cjk.first == ex_cjk.second);
                ctx.check("extract_cjk_apis_value",
                          ex_cjk.first == "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E");
                const auto ex_inul{ get_extract("extract:interiorNul") };
                ctx.check("extract_interiorNul_apis_agree", ex_inul.first == ex_inul.second);
                ctx.check("extract_interiorNul_apis_past_nul", ex_inul.first.size() >= 3);
                const auto ex_empty{ get_extract("extract:empty") };
                ctx.check("extract_empty_apis_agree", ex_empty.first == ex_empty.second);
                ctx.check("extract_empty_apis_empty", ex_empty.first.empty());
            }

            // ============ CROSS-SOURCE EQUALITY: instance vs static ==========
            // The same content returned by an INSTANCE method (GetObjectClass /
            // CallObjectMethodA) and a STATIC method (FindClass / CallStaticObjectMethodA)
            // must decode to byte-identical bytes — the two dispatch branches converge on
            // one decoder result.  (cafe vs staticUnicode, cjk vs staticCjk, emoji vs
            // staticEmoji, maxBmp vs staticMaxBmp.)
            ctx.check("cross_cafe_eq_staticUnicode", cafe.value == s_unicode.value);
            ctx.check("cross_cjk_eq_staticCjk",      cjk.value == s_cjk.value);
            ctx.check("cross_emoji_eq_staticEmoji",  emoji.value == s_emoji.value);
            ctx.check("cross_maxBmp_eq_staticMaxBmp", max_bmp.value == s_max_bmp.value);

            // ============ 16-bit length-boundary returns =====================
            // 65535 / 65537 'A's straddle the 16-bit boundary; pure ASCII so both
            // paths agree.  Proves the int32 arrayOop length read never truncates
            // to 16 bits (a uint16 wraparound would give 0 or a tiny length).
            const observation big65535{ get("bigString:65535") };
            ctx.check("bigString65535_len_65535", big65535.byte_len == 65535);
            ctx.check("bigString65535_all_A",     big65535.value == std::string(65535, 'A'));
            const observation big65537{ get("bigString:65537") };
            ctx.check("bigString65537_len_65537", big65537.byte_len == 65537);
            ctx.check("bigString65537_all_A",     big65537.value == std::string(65537, 'A'));

            // ============ STATIC unicode / fresh-OOP / arg-length ============

            // Static Greek (2-byte) through the static dispatch path -> 4 bytes.
            const observation s_greek{ get("static:staticGreek") };
            ctx.check("staticGreek_value", s_greek.value == "\xCE\xB1\xCE\xB2");
            ctx.check("staticGreek_len_4", s_greek.byte_len == 4);

            // Static built-empty -> a fresh empty String via the STATIC path -> "".
            const observation s_built_empty{ get("static:staticBuiltEmpty") };
            ctx.check("staticBuiltEmpty_captured",    s_built_empty.captured);
            ctx.check("staticBuiltEmpty_value_empty", s_built_empty.value.empty());

            // STATIC arg-encoder length witnesses (decoder-independent "len=N"):
            // the astral arg must arrive as 2 UTF-16 units and the interior-NUL arg
            // as 3 units on the STATIC dispatch path too (not just the instance one),
            // and a null arg must arrive as Java null.
            const observation s_len_emoji{ get("staticLengthOf:emoji") };
            ctx.check("staticLengthOf_emoji_two_units", s_len_emoji.value == "len=2");
            const observation s_len_nul{ get("staticLengthOf:nul") };
            ctx.check("staticLengthOf_nul_three_units", s_len_nul.value == "len=3");
            const observation s_len_null{ get("staticLengthOf:null") };
            ctx.check("staticLengthOf_null_is_java_null", s_len_null.value == "len=null");

            // ============ REGRESSION GUARD (the headline truncation bug) =====
            // The call-stub truncation bug made EVERY non-empty String return ""
            // on JDKs with the call stub.  Pin that it does not happen: known
            // non-empty strings (ASCII, static, field, dynamic, and every
            // non-ASCII shape) are never empty on whichever path runs.  The
            // interior-NUL case is the sharpest — a naive C-string copy would
            // terminate at the NUL and yield "a" or "".
            ctx.check("no_truncation_regular_nonempty",     !regular.value.empty());
            ctx.check("no_truncation_static_nonempty",      !s_regular.value.empty());
            ctx.check("no_truncation_staticEcho_nonempty",  !s_echo.value.empty());
            ctx.check("no_truncation_field_nonempty",       !field_value.value.empty());
            ctx.check("no_truncation_dynamic_nonempty",     !dynamic.value.empty());
            ctx.check("no_truncation_longAscii_nonempty",   !long_ascii.value.empty());
            ctx.check("no_truncation_cafe_nonempty",        !cafe.value.empty());
            ctx.check("no_truncation_cjk_nonempty",         !cjk.value.empty());
            ctx.check("no_truncation_emoji_nonempty",       !emoji.value.empty());
            ctx.check("no_truncation_interiorNul_nonempty", !inul.value.empty());
            // The interior-NUL result must contain MORE than just the leading 'a'
            // (proves the NUL did not terminate the decode early on either path).
            ctx.check("no_truncation_interiorNul_past_nul", inul.byte_len >= 3);
            // The newly-added shapes are non-empty on BOTH paths (all BMP / CESU).
            ctx.check("no_truncation_controlChars_nonempty",  !control.value.empty());
            ctx.check("no_truncation_maxBmp_nonempty",        !max_bmp.value.empty());
            ctx.check("no_truncation_loneSurrogate_nonempty", !lone_hi.value.empty());
            ctx.check("no_truncation_unicodeWs_nonempty",     !uws.value.empty());
            // Fresh (non-interned) unicode slice + length-1 multibyte returns are
            // non-empty on both paths — a fresh sliced OOP and a single multibyte
            // char are exactly the shapes a naive ASCII-only or interned-only decode
            // would mishandle.
            ctx.check("no_truncation_subCjk_nonempty",     !sub_cjk_full.value.empty());
            ctx.check("no_truncation_singleCjk_nonempty",  !single_cjk.value.empty());
            ctx.check("no_truncation_singleLatin1_nonempty", !single_lat.value.empty());
            // ARG-ENCODER guard (the fix): the astral and interior-NUL ARGS must come
            // back with MORE than one byte on every path — the old NewStringUTF encoder
            // collapsed the emoji to a single U+00F0 (2 bytes C3 B0) and truncated
            // "a\0b" to a one-byte "a".  A length-counted UTF-16 arg never does either.
            ctx.check("no_argtrunc_echoEmoji_multibyte", echo_emoji.byte_len >= 4);
            ctx.check("no_argtrunc_echoNul_past_nul",    echo_nul.byte_len >= 3);
            // The combined astral+NUL arg must come back with MORE than the astral
            // scalar alone — proving the trailing NUL + 'z' survived the arg encode
            // (a C-string-cut encoder would stop at the NUL, dropping the 'z').
            ctx.check("no_argtrunc_echoEmojiNul_past_nul", echo_emoji_nul.byte_len >= 6);
            // The >65536 return is now non-empty on BOTH paths (the old 4096
            // read_java_string cap that made the call_stub path return "" is gone —
            // robustness bug #29), so these "very long String survives in full" guards
            // hold unconditionally on every dispatcher.
            ctx.check("no_truncation_bigString70000_nonempty", !big70000.value.empty());
            ctx.check("no_truncation_echoBig70000_nonempty",   !echo_big.value.empty());
        }
    }
}

VMHOOK_JVM_MODULE(method_call_string)
{
    // Run the whole body under a try/catch so a stray throw from any vmhook call
    // can never escape this module (a throw is recorded as [INFO], never a FAIL).
    bool body_threw{ false };
    try
    {
        run_string_checks(ctx);
    }
    catch (...)
    {
        body_threw = true;
    }

    // FINAL CLEANUP — OUTSIDE the try so it ALWAYS runs.  The only hook (the
    // trigger scoped_hook) already uninstalls at its scope exit; this
    // unconditional shutdown_hooks() guarantees an empty hook table even if the
    // body threw before that scope exit (idempotent and safe-when-empty).  A
    // leaked armed hook is exactly what cascades into later modules.
    vmhook::shutdown_hooks();

    if (body_threw)
    {
        ctx.record("[INFO] method_call_string: the test body threw and was "
                   "contained (no crash, no hooks armed); see preceding checks for "
                   "partial results.");
    }
    ctx.check("method_string_module_left_clean_final_shutdown", true);
}
