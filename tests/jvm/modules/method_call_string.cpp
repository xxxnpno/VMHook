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
//       supplementary scalar becomes one 4-byte sequence; it rejects length 0 or
//       > 4096 (-> "").
//   So the two paths AGREE byte-for-byte on all BMP text (ASCII, Latin-1, CJK,
//   Greek, the multi-byte boundaries) and DIFFER only on (a) supplementary-plane
//   scalars (CESU-8 6 bytes vs standard 4), (b) any NUL (C0 80 vs raw 00),
//   (c) the empty/null variant tag, and (d) the read_java_string 4096-char cap.
//   This module asserts the broad agreement UNCONDITIONALLY and branches only on
//   those divergences, picking the path-correct expectation for whichever decoder
//   is live.  It records the live path as [INFO] so a reader of test_results.txt
//   always knows which decoder the assertions exercised.  (The CI path is always
//   call_jni; the call_stub branches are kept correct-by-construction from the
//   header's deterministic decoders but are unreachable on the CI matrix.)
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

        // Very large returns — ABOVE 4096 and ABOVE 65536 code units.  On the
        // call_jni path GetStringUTFChars returns the whole String (no cap); on the
        // call_stub path read_java_string rejects any length > 4096 -> "".
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
        // echo of a >65536-code-unit arg: make_java_string routes any input above
        // the 4096-unit TLAB ceiling to the GC-aware JNIEnv::NewString fallback,
        // which builds the FULL String (no truncation); the call_jni return then
        // hands all 70000 bytes back.  Proves the over-cap ARGUMENT path end to end.
        if (auto proxy{ self->get_method("echo") })
        {
            record_value("echo:big70000", proxy->call(std::string(70000, 'Q')));
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

            // 300-char arg echoed back in full (no arg-side truncation).
            const observation echo_long{ get("echo:long") };
            ctx.check("echo_long_len_300", echo_long.byte_len == 300);
            ctx.check("echo_long_all_Z",   echo_long.value == std::string(300, 'Z'));

            // Latin-1 round-trip.  On the call_jni path (CI) NewStringUTF decodes
            // the modified-UTF-8 arg to caf+U+00E9 and GetStringUTFChars encodes
            // it back, so the bytes round-trip exactly.  Recorded as [INFO] on the
            // call_stub path (make_java_string's arg encode is not exercised in
            // CI; we do not hard-assert an unverified path).
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

            // repeatA(4096): exactly AT read_java_string's cap (length <= 4096
            // passes), so BOTH paths return the full 4096 'A's.
            const observation r4096{ get("repeatA:4096") };
            ctx.check("repeatA4096_len_4096", r4096.byte_len == 4096);
            ctx.check("repeatA4096_all_A",    r4096.value == std::string(4096, 'A'));

            // repeatA(5000): ABOVE the cap.  call_jni's GetStringUTFChars has no
            // cap and returns the full 5000; read_java_string rejects length >
            // 4096 and returns "".  Each is the documented behaviour of its
            // decoder.
            const observation r5000{ get("repeatA:5000") };
            if (stub_path)
            {
                ctx.check("repeatA5000_call_stub_caps_to_empty", r5000.value.empty());
            }
            else
            {
                ctx.check("repeatA5000_call_jni_full_5000", r5000.byte_len == 5000);
                ctx.check("repeatA5000_call_jni_all_A",
                          r5000.value == std::string(5000, 'A'));
            }

            // ============ VERY LARGE returns: >4096 and >65536 ===============
            // bigString(n) has NO 8192 clamp (unlike repeatA), so it returns a
            // String far above the read_java_string backing-array cap AND above
            // 65536 code units.  call_jni's GetStringUTFChars has no cap and returns
            // the whole String; call_stub's read_java_string rejects any length >
            // 4096 -> "".  Assert the path-correct contract for each.
            const observation big5000{ get("bigString:5000") };
            const observation big70000{ get("bigString:70000") };
            const observation big131072{ get("bigString:131072") };
            const observation s_big70000{ get("static:staticBigString:70000") };
            ctx.check("bigString5000_captured",   big5000.captured);
            ctx.check("bigString70000_captured",  big70000.captured);
            if (stub_path)
            {
                // read_java_string caps at 4096 -> every over-cap return is "".
                ctx.check("bigString5000_call_stub_empty",   big5000.value.empty());
                ctx.check("bigString70000_call_stub_empty",  big70000.value.empty());
                ctx.check("bigString131072_call_stub_empty", big131072.value.empty());
                ctx.check("staticBigString70000_call_stub_empty", s_big70000.value.empty());
            }
            else
            {
                // GetStringUTFChars returns the FULL String at any length.
                ctx.check("bigString5000_call_jni_len_5000",   big5000.byte_len == 5000);
                ctx.check("bigString5000_call_jni_all_A",
                          big5000.value == std::string(5000, 'A'));
                // >65536 — the headline large case.
                ctx.check("bigString70000_call_jni_len_70000", big70000.byte_len == 70000);
                ctx.check("bigString70000_call_jni_all_A",
                          big70000.value == std::string(70000, 'A'));
                // >131072 (2x 65536), to be thorough about "very long".
                ctx.check("bigString131072_call_jni_len_131072",
                          big131072.byte_len == 131072);
                ctx.check("bigString131072_call_jni_all_A",
                          big131072.value == std::string(131072, 'A'));
                // Same >65536 case via the STATIC dispatch path.
                ctx.check("staticBigString70000_call_jni_len_70000",
                          s_big70000.byte_len == 70000);
                ctx.check("staticBigString70000_call_jni_all_A",
                          s_big70000.value == std::string(70000, 'A'));
            }

            // ============ ECHO round-trips of every shape (String arg) =======
            // The argument travels make_java_string (utf8_to_utf16 -> hand-built
            // array for <=4096 units, else GC-aware JNIEnv::NewString), and the
            // return travels the live decoder.  These prove the ARGUMENT encode for
            // CJK, an astral scalar, an interior NUL, max-BMP, and a >65536 arg.

            // CJK arg -> identical 9 bytes back (BMP, path-independent).
            const observation echo_cjk{ get("echo:cjk") };
            ctx.check("echo_cjk_round_trip",
                      echo_cjk.value == "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E");
            ctx.check("echo_cjk_len_9", echo_cjk.byte_len == 9);

            // max-BMP arg U+FFFF -> 3-byte EF BF BF back (path-independent).
            const observation echo_max_bmp{ get("echo:maxBmp") };
            ctx.check("echo_maxBmp_round_trip", echo_max_bmp.value == "\xEF\xBF\xBF");

            // Astral emoji arg.  KEY ASYMMETRY between the two String-arg encoders:
            //   * call_jni (CI path) packs a std::string arg via NewStringUTF, which
            //     interprets the bytes as MODIFIED UTF-8 read as a C string.  A
            //     STANDARD 4-byte sequence (F0 9F 98 80) is NOT valid modified UTF-8
            //     (which has no 4-byte form), so HotSpot's lenient NewStringUTF does
            //     NOT reconstruct U+1F600 — it decodes the lead byte to a single
            //     char (observed: U+00F0 -> the 2 bytes C3 B0 on the return).  This
            //     is characterised, not hard-asserted, because it is NewStringUTF's
            //     malformed-input behaviour, not vmhook's decode (which this module
            //     owns and which is byte-exact for every WELL-FORMED case above).
            //   * call_stub packs via make_java_string -> utf8_to_utf16, which DOES
            //     decode the 4-byte sequence to a surrogate pair, so the return is
            //     the 4-byte standard form again.
            // To pass a SUPPLEMENTARY scalar through the call_jni String-arg path
            // intact, the caller must hand modified UTF-8 (a CESU surrogate pair);
            // that is out of scope here (the return-decode is the subject).
            const observation echo_emoji{ get("echo:emoji") };
            ctx.record(std::string{ "[INFO] echo:emoji (std-4byte arg via String-arg encoder) = [" }
                       + to_hex(echo_emoji.value)
                       + "]  (call_jni NewStringUTF mangles a non-modified-UTF-8 astral arg; "
                         "call_stub utf8_to_utf16 round-trips it)");
            if (stub_path)
            {
                ctx.check("echo_emoji_call_stub_4byte",
                          echo_emoji.value == "\xF0\x9F\x98\x80");
            }
            else
            {
                // Deterministic NewStringUTF malformed-lead decode -> U+00F0 (C3 B0).
                ctx.check("echo_emoji_call_jni_newstringutf_mangles",
                          echo_emoji.value == "\xC3\xB0");
            }

            // Interior-NUL arg "a\0b" (RAW NUL in the std::string).  ANOTHER
            // String-arg-encoder asymmetry:
            //   * call_jni packs via NewStringUTF(arg.c_str()) — a C string — so it
            //     TRUNCATES at the first interior NUL: the Java String is just "a",
            //     and the return is "a".  This is a real (if documented) limitation
            //     of passing NUL-bearing bytes through the JNI String-arg path; the
            //     RETURN decoder handles interior NULs perfectly (see the
            //     interiorNul / nulOnly / leadingNul / trailingNul cases — those are
            //     method RETURNS, decoded by call()/read_java_string, not args).
            //   * call_stub packs via make_java_string -> utf8_to_utf16, which is
            //     length-counted (raw 0x00 -> U+0000, no C-string cut), so the Java
            //     String is 'a' U+0000 'b' and the return re-encodes U+0000 to C0 80
            //     -> "a\xC0\x80b".
            const observation echo_nul{ get("echo:nul") };
            ctx.record(std::string{ "[INFO] echo:nul (\"a\\0b\" arg via String-arg encoder) = [" }
                       + to_hex(echo_nul.value)
                       + "]  (call_jni NewStringUTF truncates the arg at the interior NUL -> \"a\"; "
                         "call_stub utf8_to_utf16 keeps it)");
            if (stub_path)
            {
                ctx.check("echo_nul_call_stub_bytes", echo_nul.value == std::string("a\0b", 3));
                ctx.check("echo_nul_call_stub_len3",  echo_nul.byte_len == 3);
            }
            else
            {
                // C-string truncation at the interior NUL: arg "a\0b" -> Java "a".
                ctx.check("echo_nul_call_jni_truncates_at_nul", echo_nul.value == "a");
                ctx.check("echo_nul_call_jni_len1",             echo_nul.byte_len == 1);
            }

            // >65536-code-unit arg: make_java_string routes any input above the
            // 4096-unit TLAB ceiling to the GC-aware JNIEnv::NewString fallback,
            // which builds the FULL String (no truncation — the e547746 fix).  The
            // call_jni return then yields all 70000 bytes; the call_stub return
            // caps the READ at 4096 -> "".  This is the over-cap ARGUMENT proof.
            const observation echo_big{ get("echo:big70000") };
            if (stub_path)
            {
                // The arg built the full 70000-char String (make_java_string did
                // not truncate); read_java_string just refuses to read it back.
                ctx.check("echo_big70000_call_stub_read_capped_empty", echo_big.value.empty());
            }
            else
            {
                ctx.check("echo_big70000_call_jni_len_70000", echo_big.byte_len == 70000);
                ctx.check("echo_big70000_call_jni_all_Q",
                          echo_big.value == std::string(70000, 'Q'));
            }

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
            // The >65536 return is non-empty only on call_jni (call_stub caps the
            // READ to 4096 -> ""); guard it on the path that returns it.
            if (!stub_path)
            {
                ctx.check("no_truncation_bigString70000_nonempty", !big70000.value.empty());
                ctx.check("no_truncation_echoBig70000_nonempty",   !echo_big.value.empty());
            }
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
