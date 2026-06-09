// method_call_string JVM test module.
//
// FEATURE: method_proxy::call() returning java.lang.String.
//
// The whole point of this module is the critical "call-stub truncation" bug
// the audit flagged: a Ljava/lang/String;-returning call() used to hand back a
// truncated 32-bit OOP handle instead of decoding UTF-8, so the C++ side saw ""
// on the very JDKs where StubRoutines::_call_stub_entry is exposed.  Both
// dispatch paths are now expected to decode the String to a real std::string:
//   * call_jni() path (JNI fallback, the path CI actually takes on JDK 8..25,
//     where _call_stub_entry is absent from VMStructs): uses GetStringUTFChars,
//     so the bytes are MODIFIED UTF-8 (NUL -> C0 80, supplementary planes ->
//     CESU-8 surrogate pairs).
//   * call_stub path (when _call_stub_entry IS present): uses read_java_string,
//     which walks the heap byte[]/char[] directly and substitutes '?' for every
//     non-ASCII (>= 0x80) code unit.
//
// Those two decoders DISAGREE on every non-ASCII string, so this module detects
// which path is live at runtime (find_call_stub_entry()) and asserts the exact,
// correct result for THAT path.  ASCII / empty / structural angles are
// path-independent and asserted unconditionally.  Everything is extracted with
// value_t::as_string() (the unambiguous accessor) per the feature contract.
//
// call() must run where current_java_thread is set, i.e. from inside a hook
// detour.  So we hook MethodString.trigger() (a no-arg instance method the
// probe calls on a real bytecode dispatch) and do ALL the call()-returns-String
// work from inside that detour against the live receiver + the static methods.
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
    // Wrapper for vmhook.fixtures.MethodString.
    class method_string_fixture : public vmhook::object<method_string_fixture>
    {
    public:
        explicit method_string_fixture(vmhook::oop_t instance) noexcept
            : vmhook::object<method_string_fixture>{ instance }
        {
        }

        static auto set_go(bool value) -> void { static_field("go")->set(value); }
        static auto get_done() -> bool          { return static_field("done")->get(); }
        static auto get_trigger_count() -> std::int32_t { return static_field("triggerCount")->get(); }
    };

    // One captured call() result: the decoded std::string plus the value_t's
    // own predicates, so we can assert both the payload AND the variant tag.
    struct observation
    {
        std::string  value{};
        std::size_t  byte_len{ 0 };
        bool         is_string{ false };
        bool         is_void{ false };
        bool         captured{ false };
    };

    std::mutex                          g_mutex;
    std::map<std::string, observation>  g_obs;            // method-name -> result
    std::atomic<int>                    g_detour_calls{ 0 };
    std::atomic<bool>                   g_call_stub_path{ false }; // true => read_java_string decoder
    std::atomic<bool>                   g_self_was_valid{ false };
    std::atomic<int>                    g_loop_distinct{ -1 };     // distinct results across the leak loop
    std::atomic<int>                    g_loop_iterations{ 0 };

    // Record one no-arg instance call.
    auto capture(const method_string_fixture& self, const char* method) -> void
    {
        auto proxy{ self.get_method(method) };
        observation obs{};
        if (proxy.has_value())
        {
            const vmhook::method_proxy::value_t v{ proxy->call() };
            obs.value     = v.as_string();
            obs.byte_len  = obs.value.size();
            obs.is_string = v.is_string();
            obs.is_void   = v.is_void();
            obs.captured  = true;
        }
        std::lock_guard<std::mutex> lock{ g_mutex };
        g_obs[method] = obs;
    }

    // Record one no-arg STATIC call.
    auto capture_static(const char* method) -> void
    {
        auto proxy{ method_string_fixture::static_method(method) };
        observation obs{};
        if (proxy.has_value())
        {
            const vmhook::method_proxy::value_t v{ proxy->call() };
            obs.value     = v.as_string();
            obs.byte_len  = obs.value.size();
            obs.is_string = v.is_string();
            obs.is_void   = v.is_void();
            obs.captured  = true;
        }
        std::lock_guard<std::mutex> lock{ g_mutex };
        g_obs[std::string{ "static:" } + method] = obs;
    }

    // Record a one-arg instance call (String -> String), keyed by an explicit tag.
    auto capture_echo(const method_string_fixture& self,
                      const char*                  tag,
                      const std::string&           arg) -> void
    {
        auto proxy{ self.get_method("echo") };
        observation obs{};
        if (proxy.has_value())
        {
            const vmhook::method_proxy::value_t v{ proxy->call(arg) };
            obs.value     = v.as_string();
            obs.byte_len  = obs.value.size();
            obs.is_string = v.is_string();
            obs.is_void   = v.is_void();
            obs.captured  = true;
        }
        std::lock_guard<std::mutex> lock{ g_mutex };
        g_obs[std::string{ "echo:" } + tag] = obs;
    }

    // Read back a recorded observation (default-constructed if missing).
    auto get(const std::string& key) -> observation
    {
        std::lock_guard<std::mutex> lock{ g_mutex };
        const auto it{ g_obs.find(key) };
        return (it != g_obs.end()) ? it->second : observation{};
    }
}

VMHOOK_JVM_MODULE(method_call_string)
{
    vmhook::register_class<method_string_fixture>("vmhook/fixtures/MethodString");

    {
        // Hook trigger(); inside the detour current_java_thread is live, so
        // every call() below dispatches a real Java method and decodes its
        // String return.  scoped_hook uninstalls when the handle leaves scope.
        auto handle{ vmhook::scoped_hook<method_string_fixture>(
            "trigger",
            [](vmhook::return_value&,
               const std::unique_ptr<method_string_fixture>& self)
            {
                g_detour_calls.fetch_add(1, std::memory_order_relaxed);
                g_self_was_valid.store(self != nullptr, std::memory_order_relaxed);
                if (!self)
                {
                    return;
                }

                // Which decoder will call() use?  Record it so the assertions
                // pick the right expectation for non-ASCII strings.
                g_call_stub_path.store(
                    vmhook::detail::find_call_stub_entry() != nullptr,
                    std::memory_order_relaxed);

                // ---- instance String returns, every category ----
                capture(*self, "regular");
                capture(*self, "empty");
                capture(*self, "single");
                capture(*self, "whitespace");
                capture(*self, "punctuation");
                capture(*self, "asciiHigh");
                capture(*self, "returnNull");
                capture(*self, "cafe");
                capture(*self, "resume");
                capture(*self, "accents");
                capture(*self, "mixed");
                capture(*self, "cjk");
                capture(*self, "greek");
                capture(*self, "emoji");
                capture(*self, "interiorNul");
                capture(*self, "fieldValue");
                capture(*self, "dynamic");
                capture(*self, "substringResult");

                // ---- static String returns ----
                capture_static("staticRegular");
                capture_static("staticEmpty");
                capture_static("staticNull");
                capture_static("staticUnicode");
                capture_static("staticCjk");
                capture_static("staticDynamic");

                // ---- argument round-trips (String arg -> String return) ----
                capture_echo(*self, "ascii",   std::string{ "round-trip-123" });
                capture_echo(*self, "empty",   std::string{});
                // Pure-ASCII high-byte free unicode round trip is path-sensitive
                // on the way OUT too (make_java_string copies raw bytes), so keep
                // the echoed unicode to a Latin-1 char the JNI path round-trips.
                capture_echo(*self, "unicode", std::string{ "caf\xC3\xA9" });

                {
                    // repeatA(64): a medium fresh string of 64 'A's.
                    auto proxy{ self->get_method("repeatA") };
                    observation obs{};
                    if (proxy.has_value())
                    {
                        const vmhook::method_proxy::value_t v{ proxy->call(static_cast<std::int32_t>(64)) };
                        obs.value     = v.as_string();
                        obs.byte_len  = obs.value.size();
                        obs.is_string = v.is_string();
                        obs.is_void   = v.is_void();
                        obs.captured  = true;
                    }
                    std::lock_guard<std::mutex> lock{ g_mutex };
                    g_obs["repeatA64"] = obs;
                }

                {
                    // repeatA(5000): asks for 5000 chars; read_java_string caps
                    // its decode at 4096 and the call_jni GetStringUTFChars path
                    // returns the full length.  Capture both behaviours.
                    auto proxy{ self->get_method("repeatA") };
                    observation obs{};
                    if (proxy.has_value())
                    {
                        const vmhook::method_proxy::value_t v{ proxy->call(static_cast<std::int32_t>(5000)) };
                        obs.value     = v.as_string();
                        obs.byte_len  = obs.value.size();
                        obs.is_string = v.is_string();
                        obs.is_void   = v.is_void();
                        obs.captured  = true;
                    }
                    std::lock_guard<std::mutex> lock{ g_mutex };
                    g_obs["repeatA5000"] = obs;
                }

                // ---- leak / stability loop: call the same String-returning
                // method many times and assert the result never changes and the
                // JNI local-ref table never starves (would manifest as later
                // calls returning "").  Directly exercises the local-ref release
                // fix in both decode paths.
                {
                    constexpr int iterations{ 250 };
                    std::string  first{};
                    int          distinct{ 0 };
                    bool         have_first{ false };
                    for (int i{ 0 }; i < iterations; ++i)
                    {
                        auto proxy{ self->get_method("regular") };
                        if (!proxy.has_value())
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
                   + (stub_path ? "call_stub (read_java_string)" : "call_jni (GetStringUTFChars / modified-UTF-8)"));

        // ================= PATH-INDEPENDENT (ASCII / structure) =============

        const observation regular{ get("regular") };
        ctx.check("regular_captured",        regular.captured);
        ctx.check("regular_value_exact",     regular.value == "hello world");
        ctx.check("regular_is_string",       regular.is_string);
        ctx.check("regular_not_void",        !regular.is_void);
        ctx.check("regular_len_11",          regular.byte_len == 11);

        const observation empty{ get("empty") };
        ctx.check("empty_captured",          empty.captured);
        ctx.check("empty_value_is_empty",    empty.value.empty());
        ctx.check("empty_len_0",             empty.byte_len == 0);
        // Empty string is still a String object on the JNI path (decoded ""),
        // so as_string() yields "".  is_string()/is_void() differ by path
        // (call_stub maps the empty-array decode to monostate), so we only
        // hard-assert the payload and record the tags below.
        ctx.record(std::string{ "[INFO] empty.is_string=" }
                   + (empty.is_string ? "1" : "0")
                   + " empty.is_void=" + (empty.is_void ? "1" : "0"));

        const observation single{ get("single") };
        ctx.check("single_value_exact",      single.value == "X");
        ctx.check("single_len_1",            single.byte_len == 1);

        const observation whitespace{ get("whitespace") };
        ctx.check("whitespace_value_exact",  whitespace.value == " \t\n\r ");
        ctx.check("whitespace_len_5",        whitespace.byte_len == 5);

        const observation punctuation{ get("punctuation") };
        ctx.check("punctuation_value_exact", punctuation.value == "\"\\/{}[]:,");

        const observation ascii_high{ get("asciiHigh") };
        ctx.check("asciiHigh_value_exact",   ascii_high.value == "~}|{`_^]");

        const observation field_value{ get("fieldValue") };
        ctx.check("fieldValue_exact",        field_value.value == "instance-field-value");
        ctx.check("fieldValue_is_string",    field_value.is_string);

        const observation dynamic{ get("dynamic") };
        ctx.check("dynamic_value_exact",     dynamic.value == "dyn-42");
        ctx.check("dynamic_is_string",       dynamic.is_string);

        const observation substr{ get("substringResult") };
        ctx.check("substring_value_exact",   substr.value == "456789");

        // ---- null return: as_string() collapses to "" on both paths ----
        const observation ret_null{ get("returnNull") };
        ctx.check("returnNull_as_string_empty", ret_null.value.empty());
        // The variant tag differs by path: call_jni stores std::string{""}
        // (is_string), call_stub stores monostate (is_void).  Assert the
        // path-correct tag so the divergence is genuinely covered, not skipped.
        if (stub_path)
        {
            ctx.check("returnNull_tag_void_on_call_stub", ret_null.is_void);
        }
        else
        {
            ctx.check("returnNull_tag_string_on_call_jni", ret_null.is_string && !ret_null.is_void);
        }

        // ================= STATIC dispatch (FindClass branch) ================

        const observation s_regular{ get("static:staticRegular") };
        ctx.check("staticRegular_captured",  s_regular.captured);
        ctx.check("staticRegular_value_exact", s_regular.value == "static-hello");
        ctx.check("staticRegular_is_string", s_regular.is_string);

        const observation s_empty{ get("static:staticEmpty") };
        ctx.check("staticEmpty_value_empty", s_empty.value.empty());

        const observation s_null{ get("static:staticNull") };
        ctx.check("staticNull_as_string_empty", s_null.value.empty());

        const observation s_dyn{ get("static:staticDynamic") };
        ctx.check("staticDynamic_value_exact", s_dyn.value == "static-dyn-99");

        // ================= ARGUMENT round-trips ==============================

        const observation echo_ascii{ get("echo:ascii") };
        ctx.check("echo_ascii_exact",        echo_ascii.value == "round-trip-123");
        ctx.check("echo_ascii_is_string",    echo_ascii.is_string);

        const observation echo_empty{ get("echo:empty") };
        ctx.check("echo_empty_value_empty",  echo_empty.value.empty());

        // ================= LENGTH / CAP angles ===============================

        const observation r64{ get("repeatA64") };
        ctx.check("repeatA64_len_64",        r64.byte_len == 64);
        ctx.check("repeatA64_all_A",
                  r64.value == std::string(64, 'A'));

        const observation r5000{ get("repeatA5000") };
        // On the call_jni path GetStringUTFChars returns the full 5000 ASCII
        // bytes.  On the call_stub path read_java_string rejects length > 4096
        // and returns "".  Both are the documented behaviour of their decoder.
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

        // ================= LEAK / STABILITY loop =============================

        ctx.check("leak_loop_ran",
                  g_loop_iterations.load(std::memory_order_relaxed) == 250);
        // Every one of the 250 calls must return the identical "hello world";
        // a starved local-ref table would make later calls return "" -> a 2nd
        // distinct value.  distinct == 1 proves stable, leak-free decoding.
        ctx.check("leak_loop_single_distinct_value",
                  g_loop_distinct.load(std::memory_order_relaxed) == 1);

        // ================= UNICODE: path-correct exact bytes =================
        // JNI path: modified UTF-8 (verified against DataOutputStream.writeUTF).
        // call_stub path: read_java_string substitutes '?' for every code unit
        // >= 0x80, so a café decodes to "caf?" etc.  Assert exactly per path.

        const observation cafe{ get("cafe") };
        const observation resume{ get("resume") };
        const observation accents{ get("accents") };
        const observation mixed{ get("mixed") };
        const observation cjk{ get("cjk") };
        const observation greek{ get("greek") };
        const observation emoji{ get("emoji") };
        const observation inul{ get("interiorNul") };
        const observation s_unicode{ get("static:staticUnicode") };
        const observation s_cjk{ get("static:staticCjk") };
        const observation echo_unicode{ get("echo:unicode") };

        ctx.check("cafe_captured",   cafe.captured);
        ctx.check("cjk_captured",    cjk.captured);
        ctx.check("emoji_captured",  emoji.captured);

        if (stub_path)
        {
            // read_java_string '?'-replacement contract.
            ctx.check("cafe_call_stub_q",     cafe.value    == "caf?");
            ctx.check("resume_call_stub_q",   resume.value  == "r?sum?");
            ctx.check("accents_call_stub_q",  accents.value == "????");
            ctx.check("mixed_call_stub_q",    mixed.value   == "???");
            ctx.check("cjk_call_stub_q",      cjk.value     == "???");
            ctx.check("greek_call_stub_q",    greek.value   == "??");
            // Emoji is one supplementary scalar = two UTF-16 code units, both
            // >= 0x80, so read_java_string yields "??".
            ctx.check("emoji_call_stub_qq",   emoji.value   == "??");
            // Interior NUL: code units are 'a', 0x0000, 'b'.  0x0000 < 0x80 so
            // it stays a real NUL; 'a'/'b' pass through.  -> "a\0b" (len 3).
            ctx.check("interiorNul_call_stub_len3", inul.byte_len == 3);
            ctx.check("interiorNul_call_stub_bytes",
                      inul.value == std::string("a\0b", 3));
            ctx.check("staticUnicode_call_stub_q", s_unicode.value == "caf?");
            ctx.check("staticCjk_call_stub_q",     s_cjk.value     == "???");
            // On the call_stub path the OUTGOING arg also diverges: make_java_string
            // copies the modified-UTF-8 bytes "caf\xC3\xA9" verbatim into a LATIN1
            // byte[], so the Java String is 5 LATIN1 chars (c,a,f,0xC3,0xA9) and
            // read_java_string maps the two high bytes to '?' -> "caf??".
            ctx.check("echo_unicode_call_stub_qq", echo_unicode.value == "caf??");
        }
        else
        {
            // call_jni GetStringUTFChars modified-UTF-8 contract (exact bytes).
            ctx.check("cafe_call_jni_mutf8",
                      cafe.value == "caf\xC3\xA9");
            ctx.check("cafe_call_jni_len5",   cafe.byte_len == 5);

            ctx.check("resume_call_jni_mutf8",
                      resume.value == "r\xC3\xA9sum\xC3\xA9");
            ctx.check("resume_call_jni_len8", resume.byte_len == 8);

            ctx.check("accents_call_jni_mutf8",
                      accents.value == "\xC3\xA9\xC3\xA8\xC3\xAA\xC3\xAB");
            ctx.check("accents_call_jni_len8", accents.byte_len == 8);

            ctx.check("mixed_call_jni_mutf8",
                      mixed.value == "\xC3\xBC\xC3\xB1\xE2\x82\xAC");
            ctx.check("mixed_call_jni_len7",  mixed.byte_len == 7);

            ctx.check("cjk_call_jni_mutf8",
                      cjk.value == "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E");
            ctx.check("cjk_call_jni_len9",    cjk.byte_len == 9);

            ctx.check("greek_call_jni_mutf8",
                      greek.value == "\xCE\xB1\xCE\xB2");
            ctx.check("greek_call_jni_len4",  greek.byte_len == 4);

            // Supplementary plane: modified UTF-8 emits the surrogate pair as
            // two 3-byte CESU-8 sequences (NOT the 4-byte standard-UTF-8 form).
            ctx.check("emoji_call_jni_cesu8",
                      emoji.value == "\xED\xA0\xBD\xED\xB8\x80");
            ctx.check("emoji_call_jni_len6", emoji.byte_len == 6);

            // Interior NUL: modified UTF-8 encodes U+0000 as 0xC0 0x80, so the
            // C string does NOT terminate early -> all 4 bytes survive.
            ctx.check("interiorNul_call_jni_mutf8",
                      inul.value == "\x61\xC0\x80\x62");
            ctx.check("interiorNul_call_jni_len4", inul.byte_len == 4);
            // Critical regression guard: the truncation bug would have made
            // this "" (or "a"); assert it is neither.
            ctx.check("interiorNul_call_jni_not_empty", !inul.value.empty());

            ctx.check("staticUnicode_call_jni_mutf8", s_unicode.value == "caf\xC3\xA9");
            ctx.check("staticCjk_call_jni_mutf8",
                      s_cjk.value == "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E");
            // Round-trip a Latin-1 string through a String arg and back.
            ctx.check("echo_unicode_call_jni_mutf8", echo_unicode.value == "caf\xC3\xA9");
        }

        // ================= REGRESSION GUARD (the headline bug) ===============
        // The call-stub truncation bug made EVERY non-empty String return ""
        // on JDKs with the call stub.  Pin that it does not happen: a known
        // non-empty ASCII String must never come back empty on either path.
        ctx.check("no_truncation_regular_nonempty", !regular.value.empty());
        ctx.check("no_truncation_static_nonempty",  !s_regular.value.empty());
        ctx.check("no_truncation_field_nonempty",   !field_value.value.empty());
        ctx.check("no_truncation_dynamic_nonempty", !dynamic.value.empty());
        // And the non-ASCII strings must never be empty either (the bug hit
        // those hardest); their exact contents are path-checked above.
        ctx.check("no_truncation_cafe_nonempty",    !cafe.value.empty());
        ctx.check("no_truncation_cjk_nonempty",     !cjk.value.empty());
        ctx.check("no_truncation_emoji_nonempty",   !emoji.value.empty());
        ctx.check("no_truncation_interiorNul_nonempty", !inul.value.empty());
    }
}
