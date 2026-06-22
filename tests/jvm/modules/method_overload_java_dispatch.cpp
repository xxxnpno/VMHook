// method_overload_java_dispatch — area: methods.
//
// THE Java-side READBACK authority for overload dispatch.  This is the companion
// to method_overload.cpp: that module proves WHICH overload the resolver SELECTS
// (each overload returns a distinct sentinel, checked inside the detour); THIS
// module proves the selected overload's REAL EFFECT — its actual computed return
// value AND a per-overload side effect Java itself records — flows back correctly
// through method_proxy::call().  It is the modular re-host of the legacy
// example.cpp test_overloaded_methods (overloadProbe*), generalized to drive each
// overload from native code TWO independent ways and read each result back.
//
// The legacy contract (vmhook/src/example.cpp test_overloaded_methods,
// Example.overload(...)) was: Java calls each overload, native reads the stored
// results back and asserts 130 / "[foo]" / 5.  Here the NATIVE side does the
// calling — via vmhook overload resolution — and asserts the same legacy values,
// proving descriptor-aware resolution reaches the right Java body for the
// PRIMITIVE (f(int)->130), the STRING (f(String)->"[foo]"), and the MULTI-ARG
// (f(int,int)->5) forms.
//
// Two dispatch paths per overload, each asserted to land the SAME value + side
// effect:
//   (1) C++-TYPED call():       get_method("f")->call(<typed arg>)
//         DISPATCH follows the C++ argument TYPE
//         (int->I, std::string->Ljava/lang/String;, (int,int)->(II)).
//         NOTE: get_method("f") resolves by NAME ONLY and latches the FIRST-by-
//         name overload's descriptor into the proxy; proxy->signature() therefore
//         reports that first-by-name descriptor (deterministically "(II)I" for
//         this fixture's `f`), NOT the typed-dispatch target — call() re-resolves
//         the dispatch overload separately and never rewrites signature_text
//         (vmhook.hpp:12534-12540).  Proof of WHICH body ran is the result value +
//         the Java-side readback, never the name-only signature.
//   (2) EXPLICIT-SIGNATURE:      get_method("f", "(I)I")->call(...)
//         the proxy is built WITH the requested descriptor (signature_pinned), so
//         here proxy->signature() IS the exact descriptor and is asserted as such.
// Java-side proof that the intended body ran (and no sibling did) comes from the
// fixture's recorders: each overload writes its arg(s)/result into a distinct
// static field and bumps its own hit counter; the module reads these back AFTER
// the probe and asserts each counter / echo.
//
// Plus the documented NO-MATCH fallback, characterized SAFELY: calling a name
// whose overloads match NONE of the C++ argument types makes
// resolve_compatible_method() walk the hierarchy, find no descriptor match, and
// return the FIRST-by-name overload (NOT monostate — the final
// `return this->method` at vmhook.hpp:13765; the call paths carry no fail-safe
// refusal, see the notes at 13128-13131 / 12521-12528).  To avoid the
// primitive-into-reference-slot access violation that a no-match dispatch into a
// reference parameter would cause, the no-match family `h` is PRIMITIVE-ONLY
// (h(int) and h(long)); whichever HotSpot orders first is a safe primitive
// dispatch.  We pass a C++ double (no (D) overload exists), assert ACTUAL
// behaviour (a valid primitive result came back, never monostate) and record the
// observed first-by-name choice as [INFO] — its identity is HotSpot
// Symbol-ordering arbitrary across JDK/compiler builds, so it is characterized,
// not pinned.
//
// SAFETY: every call() runs INSIDE the tick() detour (current_java_thread live);
// object/string oop derefs are gated with vmhook::hotspot::is_valid_pointer;
// String results are read with value_t::as_string() (NOT a cast / brace-init —
// MSVC-ambiguous); numeric results use copy-init via static_cast<std::int64_t>.
// The hook is a scoped_hook that disarms at end of scope, so NO hook stays armed
// and shutdown_hooks() is unnecessary here (and deliberately not called, matching
// the other method_* modules that share the JVM).
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace
{
    // ── Legacy-mirrored constants (kept in lockstep with OverloadDispatch.java) ─
    constexpr std::int32_t F_INT_ARG     = 30;
    constexpr std::int32_t F_INT_EXPECT  = 130;     // 30 + 100
    constexpr const char*  F_STR_ARG     = "foo";
    constexpr const char*  F_STR_EXPECT  = "[foo]";
    constexpr std::int32_t F_DUAL_A      = 2;
    constexpr std::int32_t F_DUAL_B      = 3;
    constexpr std::int32_t F_DUAL_EXPECT = 5;       // 2 + 3

    constexpr std::int32_t H_INT_ARG     = 4;
    constexpr std::int32_t H_INT_EXPECT  = 44;      // 4 + 40
    constexpr std::int64_t H_LONG_ARG    = 7;
    constexpr std::int64_t H_LONG_EXPECT = 7007;    // 7 + 7000

    // Matched widen-only family `w` (w(long)->+60000, w(double)->+70000.0).
    constexpr std::int64_t W_LONG_ARG      = 13;
    constexpr std::int64_t W_LONG_EXPECT   = 60013;     // 13 + 60000
    constexpr double       W_DOUBLE_ARG    = 2.5;
    constexpr double       W_DOUBLE_EXPECT = 70002.5;   // 2.5 + 70000.0

    // `g` family: distinct single-slot primitive descriptors (S/B/C/Z/F/I).
    constexpr std::int16_t  G_SHORT_ARG    = 12;
    constexpr std::int32_t  G_SHORT_EXPECT = 1012;  // 12 + 1000
    constexpr std::int8_t   G_BYTE_ARG     = 5;
    constexpr std::int32_t  G_BYTE_EXPECT  = 2005;  // 5 + 2000
    constexpr char16_t      G_CHAR_ARG     = u'A';  // 65
    constexpr std::int32_t  G_CHAR_EXPECT  = 3065;  // 65 + 3000
    constexpr bool          G_BOOL_ARG     = true;
    constexpr std::int32_t  G_BOOL_EXPECT  = 1;
    constexpr float         G_FLOAT_ARG    = 2.5f;
    constexpr float         G_FLOAT_EXPECT = 4002.75f; // 2.5 + 4000.25
    constexpr std::int32_t  G_INT_ARG      = 9;
    constexpr std::int32_t  G_INT_EXPECT   = 5009;  // 9 + 5000

    // `p` family: position-dependent multi-arg.
    constexpr std::int32_t  P_IS_INT    = 7;
    constexpr const char*   P_IS_STR    = "x";
    constexpr const char*   P_IS_EXPECT = "IS:7:x";
    constexpr std::int32_t  P_SI_INT    = 8;
    constexpr const char*   P_SI_STR    = "y";
    constexpr const char*   P_SI_EXPECT = "SI:y:8";

    // `sf` single-String-overload: one Java body, three C++ spellings reach it.
    constexpr const char*   SF_ARG    = "bar";
    constexpr const char*   SF_EXPECT = "{bar}";   // "{" + s + "}"

    // Exact JVM descriptors of every `f` overload (single source of truth).
    constexpr const char* SIG_F_I  = "(I)I";
    constexpr const char* SIG_F_S  = "(Ljava/lang/String;)Ljava/lang/String;";
    constexpr const char* SIG_F_II = "(II)I";

    // Exact JVM descriptors of the `g` overloads (explicit-signature path).
    constexpr const char* SIG_G_S = "(S)I";
    constexpr const char* SIG_G_B = "(B)I";
    constexpr const char* SIG_G_C = "(C)I";
    constexpr const char* SIG_G_Z = "(Z)I";
    constexpr const char* SIG_G_F = "(F)F";
    constexpr const char* SIG_G_I = "(I)I";

    // Exact JVM descriptors of the `p` overloads.
    constexpr const char* SIG_P_IS = "(ILjava/lang/String;)Ljava/lang/String;";
    constexpr const char* SIG_P_SI = "(Ljava/lang/String;I)Ljava/lang/String;";

    // Exact JVM descriptor of the `sf` single-String overload.
    constexpr const char* SIG_SF = "(Ljava/lang/String;)Ljava/lang/String;";

    // Exact JVM descriptors of the primitive `h` overloads (matched wide-arg path).
    constexpr const char* SIG_H_I = "(I)I";
    constexpr const char* SIG_H_J = "(J)J";

    // Exact JVM descriptors of the widen-only `w` overloads (matched path).
    constexpr const char* SIG_W_J = "(J)J";
    constexpr const char* SIG_W_D = "(D)D";

    // Exact JVM descriptors of the boundary identity families.
    constexpr const char* SIG_BI = "(I)I";
    constexpr const char* SIG_BL = "(J)J";
    constexpr const char* SIG_ES = "(Ljava/lang/String;)Ljava/lang/String;";

    // Sentinel for "this capture slot was never written" — distinct from any real
    // result so a body assertion can tell "detour did not run that call" apart
    // from a genuine 0 / empty value.
    constexpr std::int64_t k_unset = static_cast<std::int64_t>(0xDEADBEEFCAFEF00Dull);

    // Wrapper for vmhook.fixtures.OverloadDispatch.
    class overload_dispatch : public vmhook::object<overload_dispatch>
    {
    public:
        explicit overload_dispatch(vmhook::oop_t instance) noexcept
            : vmhook::object<overload_dispatch>{ instance }
        {
        }

        // ── go/done handshake ──────────────────────────────────────────────
        static auto set_go(bool v) -> void { static_field("go")->set(v); }
        static auto get_done() -> bool      { return static_field("done")->get(); }
        static auto get_tick_count() -> std::int32_t { return static_field("tickCount")->get(); }
        static auto last_tick_nonce() -> std::int32_t { return static_field("lastTickNonce")->get(); }

        // ── Java-recorded side effects (proof of WHICH overload body ran) ──
        static auto last_int_arg()    -> std::int32_t { return static_field("lastIntArg")->get(); }
        static auto last_int_result() -> std::int32_t { return static_field("lastIntResult")->get(); }
        static auto last_str_arg()    -> std::string  { return static_field("lastStrArg")->get(); }
        static auto last_str_result() -> std::string  { return static_field("lastStrResult")->get(); }
        static auto last_dual_a()     -> std::int32_t { return static_field("lastDualA")->get(); }
        static auto last_dual_b()     -> std::int32_t { return static_field("lastDualB")->get(); }
        static auto last_dual_sum()   -> std::int32_t { return static_field("lastDualSum")->get(); }

        static auto f_int_hits()  -> std::int32_t { return static_field("fIntHits")->get(); }
        static auto f_str_hits()  -> std::int32_t { return static_field("fStrHits")->get(); }
        static auto f_dual_hits() -> std::int32_t { return static_field("fDualHits")->get(); }

        static auto last_h_arg()    -> std::int64_t { return static_field("lastHArg")->get(); }
        static auto last_h_result() -> std::int64_t { return static_field("lastHResult")->get(); }
        static auto h_int_hits()  -> std::int32_t { return static_field("hIntHits")->get(); }
        static auto h_long_hits() -> std::int32_t { return static_field("hLongHits")->get(); }

        // ── `g` family (distinct single-slot primitive descriptors) ───────────
        static auto last_g_arg()    -> std::int64_t { return static_field("lastGArg")->get(); }
        static auto last_g_result() -> std::int64_t { return static_field("lastGResult")->get(); }
        static auto g_short_hits() -> std::int32_t { return static_field("gShortHits")->get(); }
        static auto g_byte_hits()  -> std::int32_t { return static_field("gByteHits")->get(); }
        static auto g_char_hits()  -> std::int32_t { return static_field("gCharHits")->get(); }
        static auto g_bool_hits()  -> std::int32_t { return static_field("gBoolHits")->get(); }
        static auto g_float_hits() -> std::int32_t { return static_field("gFloatHits")->get(); }
        static auto g_int_hits()   -> std::int32_t { return static_field("gIntHits")->get(); }

        // ── `p` family (position-dependent multi-arg) ─────────────────────────
        static auto last_p_result() -> std::string  { return static_field("lastPResult")->get(); }
        static auto p_is_hits() -> std::int32_t { return static_field("pIsHits")->get(); }
        static auto p_si_hits() -> std::int32_t { return static_field("pSiHits")->get(); }

        // ── `sf` single-String-overload family ────────────────────────────────
        static auto last_sf_arg() -> std::string { return static_field("lastSfArg")->get(); }
        static auto sf_hits()     -> std::int32_t { return static_field("sfHits")->get(); }

        // ── `w` widen-only family ─────────────────────────────────────────────
        static auto last_w_arg()    -> std::int64_t { return static_field("lastWArg")->get(); }
        static auto w_long_hits()   -> std::int32_t { return static_field("wLongHits")->get(); }
        static auto w_double_hits() -> std::int32_t { return static_field("wDoubleHits")->get(); }

        // ── boundary identity families (every-possible-input edge coverage) ────
        static auto last_bi_arg() -> std::int32_t { return static_field("lastBiArg")->get(); }
        static auto bi_hits()     -> std::int32_t { return static_field("biHits")->get(); }
        static auto last_bl_arg() -> std::int64_t { return static_field("lastBlArg")->get(); }
        static auto bl_hits()     -> std::int32_t { return static_field("blHits")->get(); }
        static auto last_es_arg() -> std::string  { return static_field("lastEsArg")->get(); }
        static auto last_es_len() -> std::int32_t { return static_field("lastEsLen")->get(); }
        static auto es_hits()     -> std::int32_t { return static_field("esHits")->get(); }
    };

    // ── One captured dispatch result, recorded inside the detour ──────────────
    // call() only works inside the detour (current_java_thread live), so each
    // overload call captures its outcome here for the body to read back.
    struct dispatch_result
    {
        bool         resolved{ false };  // get_method(...) returned a proxy
        std::string  sig_text{};         // proxy->signature()
        bool         is_void{ false };
        bool         is_string{ false };
        std::int64_t ival{ k_unset };    // numeric result (copy-init via static_cast)
        double       dval{ 0.0 };        // floating result (static_cast<double>)
        std::string  sval{};             // string result (value_t::as_string())
    };

    std::mutex                                g_mutex;
    std::map<std::string, dispatch_result>    g_res;

    auto put(const std::string& key, const dispatch_result& r) -> void
    {
        std::lock_guard<std::mutex> lock{ g_mutex };
        g_res[key] = r;
    }
    auto got(const std::string& key) -> dispatch_result
    {
        std::lock_guard<std::mutex> lock{ g_mutex };
        const auto it{ g_res.find(key) };
        return (it != g_res.end()) ? it->second : dispatch_result{};
    }

    // ── handshake / path observations ─────────────────────────────────────────
    std::atomic<int>  g_detour_calls{ 0 };
    std::atomic<bool> g_detour_saw_self{ false };
    std::atomic<bool> g_call_stub_path{ false };

    // The nonce value the detour observed off the hooked tick(int) frame, plus a
    // sentinel meaning "never written".  The probe drives tick(NONCE) exactly once
    // so this is a single deterministic observation (NOT a fire count) — proof the
    // detour read the right ARGUMENT, cross-checkable against Java's lastTickNonce.
    constexpr std::int32_t k_nonce_unset = static_cast<std::int32_t>(0x7FFFFFFF);
    constexpr std::int32_t k_tick_nonce  = 11;   // matches the fixture probe's SINGLETON.tick(11)
    std::atomic<std::int32_t> g_detour_nonce{ k_nonce_unset };

    // ── capture helpers: resolve, dispatch, record (all run in the detour) ─────

    // C++-typed name-only call, numeric result, ONE int arg.
    auto cap_named_num_i(const overload_dispatch& self,
                         const std::string&       key,
                         std::int32_t             a) -> void
    {
        dispatch_result r{};
        auto proxy{ self.get_method("f") };
        if (proxy.has_value())
        {
            r.resolved  = true;
            r.sig_text  = std::string{ proxy->signature() };
            const vmhook::method_proxy::value_t v{ proxy->call(a) };
            r.is_void   = v.is_void();
            r.is_string = v.is_string();
            r.ival      = static_cast<std::int64_t>(v);
        }
        put(key, r);
    }

    // C++-typed name-only call, String result, ONE std::string arg.
    auto cap_named_str_s(const overload_dispatch& self,
                         const std::string&       key,
                         const std::string&       a) -> void
    {
        dispatch_result r{};
        auto proxy{ self.get_method("f") };
        if (proxy.has_value())
        {
            r.resolved  = true;
            r.sig_text  = std::string{ proxy->signature() };
            const vmhook::method_proxy::value_t v{ proxy->call(a) };
            r.is_void   = v.is_void();
            r.is_string = v.is_string();
            r.sval      = v.as_string();   // MSVC-safe extraction (NOT a cast)
        }
        put(key, r);
    }

    // C++-typed name-only call, numeric result, TWO int args.
    auto cap_named_num_ii(const overload_dispatch& self,
                          const std::string&       key,
                          std::int32_t a, std::int32_t b) -> void
    {
        dispatch_result r{};
        auto proxy{ self.get_method("f") };
        if (proxy.has_value())
        {
            r.resolved  = true;
            r.sig_text  = std::string{ proxy->signature() };
            const vmhook::method_proxy::value_t v{ proxy->call(a, b) };
            r.is_void   = v.is_void();
            r.is_string = v.is_string();
            r.ival      = static_cast<std::int64_t>(v);
        }
        put(key, r);
    }

    // Explicit-signature call, numeric result, ONE int arg.
    auto cap_sig_num_i(const overload_dispatch& self,
                       const std::string&       key,
                       const char*              sig,
                       std::int32_t             a) -> void
    {
        dispatch_result r{};
        auto proxy{ self.get_method("f", sig) };
        if (proxy.has_value())
        {
            r.resolved  = true;
            r.sig_text  = std::string{ proxy->signature() };
            const vmhook::method_proxy::value_t v{ proxy->call(a) };
            r.is_void   = v.is_void();
            r.is_string = v.is_string();
            r.ival      = static_cast<std::int64_t>(v);
        }
        put(key, r);
    }

    // Explicit-signature call, String result, ONE std::string arg.
    auto cap_sig_str_s(const overload_dispatch& self,
                       const std::string&       key,
                       const char*              sig,
                       const std::string&       a) -> void
    {
        dispatch_result r{};
        auto proxy{ self.get_method("f", sig) };
        if (proxy.has_value())
        {
            r.resolved  = true;
            r.sig_text  = std::string{ proxy->signature() };
            const vmhook::method_proxy::value_t v{ proxy->call(a) };
            r.is_void   = v.is_void();
            r.is_string = v.is_string();
            r.sval      = v.as_string();
        }
        put(key, r);
    }

    // Explicit-signature call, numeric result, TWO int args.
    auto cap_sig_num_ii(const overload_dispatch& self,
                        const std::string&       key,
                        const char*              sig,
                        std::int32_t a, std::int32_t b) -> void
    {
        dispatch_result r{};
        auto proxy{ self.get_method("f", sig) };
        if (proxy.has_value())
        {
            r.resolved  = true;
            r.sig_text  = std::string{ proxy->signature() };
            const vmhook::method_proxy::value_t v{ proxy->call(a, b) };
            r.is_void   = v.is_void();
            r.is_string = v.is_string();
            r.ival      = static_cast<std::int64_t>(v);
        }
        put(key, r);
    }

    // No-match probe: PRIMITIVE-ONLY family `h`, called with a C++ double for
    // which NO (D) overload exists.  resolve_compatible_method() falls back to the
    // first-by-name `h` overload (h(int) or h(long), BOTH primitive — no reference
    // slot, so no AV).  Capture whatever numeric result came back.
    auto cap_nomatch_h_double(const overload_dispatch& self,
                              const std::string&       key,
                              double                   a) -> void
    {
        dispatch_result r{};
        auto proxy{ self.get_method("h") };
        if (proxy.has_value())
        {
            r.resolved  = true;
            r.sig_text  = std::string{ proxy->signature() };
            const vmhook::method_proxy::value_t v{ proxy->call(a) };
            r.is_void   = v.is_void();
            r.is_string = v.is_string();
            r.ival      = static_cast<std::int64_t>(v);
        }
        put(key, r);
    }

    // ── MATCHED wide-arg captures (`h` family) ────────────────────────────────
    // The companion to the no-match double probe: drive each `h` overload by the
    // EXACT C++ type that matches its descriptor.  h(int) is a single I slot;
    // h(long) is a TWO-slot J wide arg — proving the wide-arg packing reaches the
    // right body and the (widened-to-J) return decodes back.  Templated over the
    // C++ arg type, name-only and explicit-signature spellings both covered.
    template<typename arg_t>
    auto cap_named_h_num(const overload_dispatch& self,
                         const std::string&       key,
                         arg_t                    a) -> void
    {
        dispatch_result r{};
        auto proxy{ self.get_method("h") };
        if (proxy.has_value())
        {
            r.resolved  = true;
            r.sig_text  = std::string{ proxy->signature() };
            const vmhook::method_proxy::value_t v{ proxy->call(a) };
            r.is_void   = v.is_void();
            r.is_string = v.is_string();
            r.ival      = static_cast<std::int64_t>(v);
        }
        put(key, r);
    }

    template<typename arg_t>
    auto cap_sig_h_num(const overload_dispatch& self,
                       const std::string&       key,
                       const char*              sig,
                       arg_t                    a) -> void
    {
        dispatch_result r{};
        auto proxy{ self.get_method("h", sig) };
        if (proxy.has_value())
        {
            r.resolved  = true;
            r.sig_text  = std::string{ proxy->signature() };
            const vmhook::method_proxy::value_t v{ proxy->call(a) };
            r.is_void   = v.is_void();
            r.is_string = v.is_string();
            r.ival      = static_cast<std::int64_t>(v);
        }
        put(key, r);
    }

    // ── MATCHED widen-only captures (`w` family) ──────────────────────────────
    // The companion to the no-match int probe: drive w(long) with a C++ long and
    // w(double) with a C++ double — the EXACT descriptors (J / D), proving both
    // two-slot wide kinds dispatch their own body.  w(long) numeric result and
    // w(double) floating result are both read back.
    auto cap_named_w_long(const overload_dispatch& self,
                          const std::string&       key,
                          std::int64_t             a) -> void
    {
        dispatch_result r{};
        auto proxy{ self.get_method("w") };
        if (proxy.has_value())
        {
            r.resolved  = true;
            r.sig_text  = std::string{ proxy->signature() };
            const vmhook::method_proxy::value_t v{ proxy->call(a) };
            r.is_void   = v.is_void();
            r.is_string = v.is_string();
            r.ival      = static_cast<std::int64_t>(v);
        }
        put(key, r);
    }

    auto cap_named_w_double(const overload_dispatch& self,
                            const std::string&       key,
                            double                   a) -> void
    {
        dispatch_result r{};
        auto proxy{ self.get_method("w") };
        if (proxy.has_value())
        {
            r.resolved  = true;
            r.sig_text  = std::string{ proxy->signature() };
            const vmhook::method_proxy::value_t v{ proxy->call(a) };
            r.is_void   = v.is_void();
            r.is_string = v.is_string();
            r.dval      = static_cast<double>(v);
        }
        put(key, r);
    }

    // ── EXPLICIT-SIGNATURE positional captures (`p` family) ───────────────────
    // The pinned-descriptor companion to the typed positional calls: the proxy is
    // built WITH (ILjava/lang/String;) / (Ljava/lang/String;I), so signature() IS
    // the exact descriptor and the call honours it verbatim.
    auto cap_sig_p_is(const overload_dispatch& self,
                      const std::string&       key,
                      const char*              sig,
                      std::int32_t             n,
                      const std::string&       s) -> void
    {
        dispatch_result r{};
        auto proxy{ self.get_method("p", sig) };
        if (proxy.has_value())
        {
            r.resolved  = true;
            r.sig_text  = std::string{ proxy->signature() };
            const vmhook::method_proxy::value_t v{ proxy->call(n, s) };
            r.is_void   = v.is_void();
            r.is_string = v.is_string();
            r.sval      = v.as_string();
        }
        put(key, r);
    }

    auto cap_sig_p_si(const overload_dispatch& self,
                      const std::string&       key,
                      const char*              sig,
                      const std::string&       s,
                      std::int32_t             n) -> void
    {
        dispatch_result r{};
        auto proxy{ self.get_method("p", sig) };
        if (proxy.has_value())
        {
            r.resolved  = true;
            r.sig_text  = std::string{ proxy->signature() };
            const vmhook::method_proxy::value_t v{ proxy->call(s, n) };
            r.is_void   = v.is_void();
            r.is_string = v.is_string();
            r.sval      = v.as_string();
        }
        put(key, r);
    }

    // ── `g` family captures: distinct single-slot primitive descriptors ───────
    // C++-typed name-only call(): resolution must follow the C++ argument TYPE's
    // EXACT descriptor (no widening between sibling primitives).  Templated over
    // the C++ arg type so one helper exercises short/byte/char/bool/int.
    template<typename arg_t>
    auto cap_named_g_num(const overload_dispatch& self,
                         const std::string&       key,
                         arg_t                    a) -> void
    {
        dispatch_result r{};
        auto proxy{ self.get_method("g") };
        if (proxy.has_value())
        {
            r.resolved  = true;
            r.sig_text  = std::string{ proxy->signature() };
            const vmhook::method_proxy::value_t v{ proxy->call(a) };
            r.is_void   = v.is_void();
            r.is_string = v.is_string();
            r.ival      = static_cast<std::int64_t>(v);
        }
        put(key, r);
    }

    // C++-typed name-only call() for the FLOAT overload (g(F)F) — captures the
    // floating result via static_cast<double> (the value_t arithmetic operator).
    auto cap_named_g_float(const overload_dispatch& self,
                           const std::string&       key,
                           float                    a) -> void
    {
        dispatch_result r{};
        auto proxy{ self.get_method("g") };
        if (proxy.has_value())
        {
            r.resolved  = true;
            r.sig_text  = std::string{ proxy->signature() };
            const vmhook::method_proxy::value_t v{ proxy->call(a) };
            r.is_void   = v.is_void();
            r.is_string = v.is_string();
            r.dval      = static_cast<double>(v);
        }
        put(key, r);
    }

    // Explicit-signature call() for a `g` numeric overload (pinned descriptor),
    // templated over the C++ arg type passed at the call site.
    template<typename arg_t>
    auto cap_sig_g_num(const overload_dispatch& self,
                       const std::string&       key,
                       const char*              sig,
                       arg_t                    a) -> void
    {
        dispatch_result r{};
        auto proxy{ self.get_method("g", sig) };
        if (proxy.has_value())
        {
            r.resolved  = true;
            r.sig_text  = std::string{ proxy->signature() };
            const vmhook::method_proxy::value_t v{ proxy->call(a) };
            r.is_void   = v.is_void();
            r.is_string = v.is_string();
            r.ival      = static_cast<std::int64_t>(v);
        }
        put(key, r);
    }

    // Explicit-signature call() for the FLOAT overload.
    auto cap_sig_g_float(const overload_dispatch& self,
                         const std::string&       key,
                         const char*              sig,
                         float                    a) -> void
    {
        dispatch_result r{};
        auto proxy{ self.get_method("g", sig) };
        if (proxy.has_value())
        {
            r.resolved  = true;
            r.sig_text  = std::string{ proxy->signature() };
            const vmhook::method_proxy::value_t v{ proxy->call(a) };
            r.is_void   = v.is_void();
            r.is_string = v.is_string();
            r.dval      = static_cast<double>(v);
        }
        put(key, r);
    }

    // Single-String-overload `sf` calls driven by each C++ String spelling.
    // All of std::string / const char* / std::string_view map to
    // Ljava/lang/String; and must reach the SAME (and ONLY) sf overload.
    auto cap_sf_cstr(const overload_dispatch& self,
                     const std::string&       key,
                     const char*              a) -> void
    {
        dispatch_result r{};
        auto proxy{ self.get_method("sf") };
        if (proxy.has_value())
        {
            r.resolved  = true;
            r.sig_text  = std::string{ proxy->signature() };
            const vmhook::method_proxy::value_t v{ proxy->call(a) };
            r.is_void   = v.is_void();
            r.is_string = v.is_string();
            r.sval      = v.as_string();
        }
        put(key, r);
    }

    auto cap_sf_sview(const overload_dispatch& self,
                      const std::string&       key,
                      std::string_view         a) -> void
    {
        dispatch_result r{};
        auto proxy{ self.get_method("sf") };
        if (proxy.has_value())
        {
            r.resolved  = true;
            r.sig_text  = std::string{ proxy->signature() };
            const vmhook::method_proxy::value_t v{ proxy->call(a) };
            r.is_void   = v.is_void();
            r.is_string = v.is_string();
            r.sval      = v.as_string();
        }
        put(key, r);
    }

    auto cap_sf_string(const overload_dispatch& self,
                       const std::string&       key,
                       const std::string&       a) -> void
    {
        dispatch_result r{};
        auto proxy{ self.get_method("sf") };
        if (proxy.has_value())
        {
            r.resolved  = true;
            r.sig_text  = std::string{ proxy->signature() };
            const vmhook::method_proxy::value_t v{ proxy->call(a) };
            r.is_void   = v.is_void();
            r.is_string = v.is_string();
            r.sval      = v.as_string();
        }
        put(key, r);
    }

    // Explicit-signature `sf` call (pinned String descriptor) via const char*.
    auto cap_sf_sig(const overload_dispatch& self,
                    const std::string&       key,
                    const char*              a) -> void
    {
        dispatch_result r{};
        auto proxy{ self.get_method("sf", SIG_SF) };
        if (proxy.has_value())
        {
            r.resolved  = true;
            r.sig_text  = std::string{ proxy->signature() };
            const vmhook::method_proxy::value_t v{ proxy->call(a) };
            r.is_void   = v.is_void();
            r.is_string = v.is_string();
            r.sval      = v.as_string();
        }
        put(key, r);
    }

    // C++-typed name-only call for the position-dependent `p(int,String)` form.
    auto cap_named_p_is(const overload_dispatch& self,
                        const std::string&       key,
                        std::int32_t             n,
                        const std::string&       s) -> void
    {
        dispatch_result r{};
        auto proxy{ self.get_method("p") };
        if (proxy.has_value())
        {
            r.resolved  = true;
            r.sig_text  = std::string{ proxy->signature() };
            const vmhook::method_proxy::value_t v{ proxy->call(n, s) };
            r.is_void   = v.is_void();
            r.is_string = v.is_string();
            r.sval      = v.as_string();
        }
        put(key, r);
    }

    // C++-typed name-only call for the position-dependent `p(String,int)` form.
    auto cap_named_p_si(const overload_dispatch& self,
                        const std::string&       key,
                        const std::string&       s,
                        std::int32_t             n) -> void
    {
        dispatch_result r{};
        auto proxy{ self.get_method("p") };
        if (proxy.has_value())
        {
            r.resolved  = true;
            r.sig_text  = std::string{ proxy->signature() };
            const vmhook::method_proxy::value_t v{ proxy->call(s, n) };
            r.is_void   = v.is_void();
            r.is_string = v.is_string();
            r.sval      = v.as_string();
        }
        put(key, r);
    }

    // No-widening probe: widen-only family `w` (w(long), w(double) — NO w(int)),
    // called with a C++ int.  argument_matches_descriptor<int> -> "I" matches
    // NEITHER overload, so resolve_compatible_method falls back to first-by-name
    // (vmhook does NOT widen int->long/double).  Both fallbacks are primitive.
    auto cap_nomatch_w_int(const overload_dispatch& self,
                           const std::string&       key,
                           std::int32_t             a) -> void
    {
        dispatch_result r{};
        auto proxy{ self.get_method("w") };
        if (proxy.has_value())
        {
            r.resolved  = true;
            r.sig_text  = std::string{ proxy->signature() };
            const vmhook::method_proxy::value_t v{ proxy->call(a) };
            r.is_void   = v.is_void();
            r.is_string = v.is_string();
            r.ival      = static_cast<std::int64_t>(v);
        }
        put(key, r);
    }

    // ── BOUNDARY identity captures (every-possible-input edge coverage) ───────
    // bi(int) / bl(long) / es(String) return their argument unchanged.  Each
    // capture records BOTH the native round-tripped value and (read back later)
    // Java's per-call echo, so the module can cross-check native==Java==input for
    // boundary / overflow / degenerate inputs.  Keyed per input so every edge
    // value gets its own dispatch_result.

    // C++-typed name-only bi(int): numeric round-trip of one I-slot arg.
    auto cap_bi(const overload_dispatch& self,
                const std::string&       key,
                std::int32_t             a) -> void
    {
        dispatch_result r{};
        auto proxy{ self.get_method("bi") };
        if (proxy.has_value())
        {
            r.resolved  = true;
            r.sig_text  = std::string{ proxy->signature() };
            const vmhook::method_proxy::value_t v{ proxy->call(a) };
            r.is_void   = v.is_void();
            r.is_string = v.is_string();
            r.ival      = static_cast<std::int64_t>(v);
        }
        put(key, r);
    }

    // Explicit-signature bi(int) — pinned (I)I, same round-trip.
    auto cap_bi_sig(const overload_dispatch& self,
                    const std::string&       key,
                    std::int32_t             a) -> void
    {
        dispatch_result r{};
        auto proxy{ self.get_method("bi", SIG_BI) };
        if (proxy.has_value())
        {
            r.resolved  = true;
            r.sig_text  = std::string{ proxy->signature() };
            const vmhook::method_proxy::value_t v{ proxy->call(a) };
            r.is_void   = v.is_void();
            r.is_string = v.is_string();
            r.ival      = static_cast<std::int64_t>(v);
        }
        put(key, r);
    }

    // C++-typed name-only bl(long): numeric round-trip of one J wide arg.
    auto cap_bl(const overload_dispatch& self,
                const std::string&       key,
                std::int64_t             a) -> void
    {
        dispatch_result r{};
        auto proxy{ self.get_method("bl") };
        if (proxy.has_value())
        {
            r.resolved  = true;
            r.sig_text  = std::string{ proxy->signature() };
            const vmhook::method_proxy::value_t v{ proxy->call(a) };
            r.is_void   = v.is_void();
            r.is_string = v.is_string();
            r.ival      = static_cast<std::int64_t>(v);
        }
        put(key, r);
    }

    // C++-typed name-only es(String): String round-trip (identity).
    auto cap_es(const overload_dispatch& self,
                const std::string&       key,
                const std::string&       a) -> void
    {
        dispatch_result r{};
        auto proxy{ self.get_method("es") };
        if (proxy.has_value())
        {
            r.resolved  = true;
            r.sig_text  = std::string{ proxy->signature() };
            const vmhook::method_proxy::value_t v{ proxy->call(a) };
            r.is_void   = v.is_void();
            r.is_string = v.is_string();
            r.sval      = v.as_string();
        }
        put(key, r);
    }

    // Run every capture inside the detour against the live receiver.
    auto run_all(const std::unique_ptr<overload_dispatch>& self) -> void
    {
        if (!self)
        {
            return;
        }
        const overload_dispatch& s = *self;

        // ============================================================
        //  (1) C++-TYPED call(): resolution follows the C++ arg TYPE.
        //  Each overload's legacy result must come back.
        // ============================================================
        cap_named_num_i (s, "named_int",  F_INT_ARG);                       // f(int 30)   -> 130
        cap_named_str_s (s, "named_str",  std::string{ F_STR_ARG });        // f("foo")    -> "[foo]"
        cap_named_num_ii(s, "named_dual", F_DUAL_A, F_DUAL_B);              // f(2,3)      -> 5

        // ============================================================
        //  (2) EXPLICIT-SIGNATURE call(): resolution pinned to the descriptor.
        //  Must dispatch the SAME overload and return the SAME legacy result.
        // ============================================================
        cap_sig_num_i (s, "sig_int",  SIG_F_I,  F_INT_ARG);                 // (I)I  -> 130
        cap_sig_str_s (s, "sig_str",  SIG_F_S,  std::string{ F_STR_ARG });  // (Lstring)Lstring -> "[foo]"
        cap_sig_num_ii(s, "sig_dual", SIG_F_II, F_DUAL_A, F_DUAL_B);        // (II)I -> 5

        // ============================================================
        //  MATCHED wide-arg dispatch (`h` family): int->(I)I, long->(J)J.
        //  Driven BEFORE the no-match probe so the no-match fallback's
        //  contribution to the `h` counters is a known +1 over the matched
        //  baseline (4 matched dispatches: 2 per overload, typed + explicit-sig).
        // ============================================================
        cap_named_h_num<std::int32_t>(s, "h_int",  H_INT_ARG);              // h(int 4)  -> 44
        cap_named_h_num<std::int64_t>(s, "h_long", H_LONG_ARG);            // h(long 7) -> 7007
        cap_sig_h_num<std::int32_t>  (s, "h_int_sig",  SIG_H_I, H_INT_ARG);
        cap_sig_h_num<std::int64_t>  (s, "h_long_sig", SIG_H_J, H_LONG_ARG);

        // ============================================================
        //  No-match fallback (primitive-only `h`, called with a double).
        //  Runs AFTER the matched `h` calls; it adds exactly +1 to whichever
        //  primitive overload HotSpot orders first.
        // ============================================================
        cap_nomatch_h_double(s, "nomatch_h", 9.5);

        // ============================================================
        //  (3) DISTINCT SINGLE-SLOT PRIMITIVE DESCRIPTORS (`g` family).
        //  Each C++ arg type must resolve its EXACT descriptor sibling
        //  (no widening between primitives).  Two paths each (typed +
        //  explicit-signature), so every overload's counter reads 2.
        // ============================================================
        cap_named_g_num<std::int16_t>(s, "g_short", G_SHORT_ARG);   // short  -> (S)I
        cap_named_g_num<std::int8_t> (s, "g_byte",  G_BYTE_ARG);    // byte   -> (B)I
        cap_named_g_num<char16_t>    (s, "g_char",  G_CHAR_ARG);    // char   -> (C)I
        cap_named_g_num<bool>        (s, "g_bool",  G_BOOL_ARG);    // bool   -> (Z)I
        cap_named_g_num<std::int32_t>(s, "g_int",   G_INT_ARG);     // int    -> (I)I
        cap_named_g_float            (s, "g_float", G_FLOAT_ARG);   // float  -> (F)F

        cap_sig_g_num<std::int16_t>(s, "g_short_sig", SIG_G_S, G_SHORT_ARG);
        cap_sig_g_num<std::int8_t> (s, "g_byte_sig",  SIG_G_B, G_BYTE_ARG);
        cap_sig_g_num<char16_t>    (s, "g_char_sig",  SIG_G_C, G_CHAR_ARG);
        cap_sig_g_num<bool>        (s, "g_bool_sig",  SIG_G_Z, G_BOOL_ARG);
        cap_sig_g_num<std::int32_t>(s, "g_int_sig",   SIG_G_I, G_INT_ARG);
        cap_sig_g_float            (s, "g_float_sig", SIG_G_F, G_FLOAT_ARG);

        // ============================================================
        //  (4) STRING DESCRIPTOR from every C++ String spelling, against the
        //  single-overload `sf` family (std::string / const char* / string_view
        //  all map to Ljava/lang/String; and must reach the SAME body).
        // ============================================================
        cap_sf_string(s, "sf_string", std::string{ SF_ARG });       // std::string -> String
        cap_sf_cstr  (s, "sf_cstr",   SF_ARG);                      // const char* -> String
        cap_sf_sview (s, "sf_sview",  std::string_view{ SF_ARG });  // string_view -> String
        cap_sf_sig   (s, "sf_sig",    SF_ARG);                      // explicit-sig -> String

        // ============================================================
        //  (5) POSITION-DEPENDENT MULTI-ARG (`p` family).
        //  Arg ORDER drives the descriptor: (ILjava/lang/String;) vs
        //  (Ljava/lang/String;I).
        // ============================================================
        cap_named_p_is(s, "p_is", P_IS_INT, std::string{ P_IS_STR });
        cap_named_p_si(s, "p_si", std::string{ P_SI_STR }, P_SI_INT);

        // (5b) EXPLICIT-SIGNATURE positional dispatch — the pinned descriptor must
        // dispatch the SAME positional overload as the typed pack order.  Distinct
        // args (so the explicit-sig echoes do not alias the typed ones) keep the
        // per-overload hit counts unambiguous (each positional form: typed + sig).
        cap_sig_p_is(s, "p_is_sig", SIG_P_IS, P_IS_INT, std::string{ P_IS_STR });
        cap_sig_p_si(s, "p_si_sig", SIG_P_SI, std::string{ P_SI_STR }, P_SI_INT);

        // ============================================================
        //  (6) NO-WIDENING fallback (widen-only `w`, called with a C++ int).
        //  Driven BEFORE the matched `w` calls so the no-match contribution is a
        //  known +1 over the matched baseline (one matched dispatch per overload).
        // ============================================================
        cap_nomatch_w_int(s, "nomatch_w", 100);

        // ============================================================
        //  (6b) MATCHED widen-only dispatch (`w` family): long->(J)J, double->(D)D.
        //  Proves the EXACT-width descriptors dispatch their own body (no widening
        //  needed when the C++ type already matches) and both two-slot wide kinds
        //  marshal + decode correctly (J numeric, D floating).
        // ============================================================
        cap_named_w_long  (s, "w_long",   W_LONG_ARG);     // w(long 13)  -> 60013
        cap_named_w_double(s, "w_double", W_DOUBLE_ARG);   // w(double 2.5) -> 70002.5

        // ============================================================
        //  (7) BOUNDARY / OVERFLOW / DEGENERATE inputs through identity
        //  overloads.  Each value must round-trip EXACTLY: native return ==
        //  Java echo == input.  Driven LAST so the per-call recorders end on a
        //  known final value; the hit counts are deterministic (see below).
        //
        //  bi(int): 7 typed inputs + 1 explicit-sig => 8 bi dispatches.
        //  bl(long): 6 typed inputs => 6 bl dispatches.
        //  es(String): 3 typed inputs (empty, single, large) => 3 es dispatches.
        // ============================================================
        cap_bi(s, "bi_zero",  0);
        cap_bi(s, "bi_one",   1);
        cap_bi(s, "bi_neg1", -1);
        cap_bi(s, "bi_min",   static_cast<std::int32_t>(INT32_MIN));
        cap_bi(s, "bi_max",   static_cast<std::int32_t>(INT32_MAX));
        cap_bi(s, "bi_42",    42);
        cap_bi(s, "bi_neg42",-42);
        cap_bi_sig(s, "bi_max_sig", static_cast<std::int32_t>(INT32_MAX));

        cap_bl(s, "bl_zero",  static_cast<std::int64_t>(0));
        cap_bl(s, "bl_one",   static_cast<std::int64_t>(1));
        cap_bl(s, "bl_neg1",  static_cast<std::int64_t>(-1));
        cap_bl(s, "bl_min",   static_cast<std::int64_t>(INT64_MIN));
        cap_bl(s, "bl_max",   static_cast<std::int64_t>(INT64_MAX));
        // A value larger than INT32_MAX proves the J wide path is not truncated
        // to a single 32-bit slot.
        cap_bl(s, "bl_wide",  static_cast<std::int64_t>(0x0000'0001'0000'0000LL));

        cap_es(s, "es_empty",  std::string{});                 // ""  (degenerate)
        cap_es(s, "es_single", std::string{ "Q" });            // single char
        cap_es(s, "es_large",  std::string(1024, 'z'));        // large (1024 chars)
    }
}

VMHOOK_JVM_MODULE(method_overload_java_dispatch)
{
    vmhook::register_class<overload_dispatch>("vmhook/fixtures/OverloadDispatch");

    {
        auto handle{ vmhook::scoped_hook<overload_dispatch>(
            "tick",
            [](vmhook::return_value&,
               const std::unique_ptr<overload_dispatch>& self,
               std::int32_t nonce)
            {
                g_detour_calls.fetch_add(1, std::memory_order_relaxed);
                g_detour_saw_self.store(self != nullptr, std::memory_order_relaxed);
                // Record the nonce the detour observed off the hooked frame (only
                // on the FIRST fire, so a JIT-inlined re-fire can't overwrite the
                // cold-interpreter observation we assert on).
                std::int32_t expected_unset{ k_nonce_unset };
                g_detour_nonce.compare_exchange_strong(
                    expected_unset, nonce, std::memory_order_relaxed);
                g_call_stub_path.store(
                    vmhook::detail::find_call_stub_entry() != nullptr,
                    std::memory_order_relaxed);
                run_all(self);
            }) };

        ctx.check("mojd_hook_installed", handle.installed());

        const bool done{ ctx.run_probe(
            [](bool v) { overload_dispatch::set_go(v); },
            []() { return overload_dispatch::get_done(); }) };

        ctx.check("mojd_probe_completed", done);
        ctx.check("mojd_detour_fired", g_detour_calls.load(std::memory_order_relaxed) >= 1);
        ctx.check("mojd_detour_saw_self", g_detour_saw_self.load(std::memory_order_relaxed));
        ctx.check("mojd_tick_count_advanced", overload_dispatch::get_tick_count() >= 1);

        const bool stub_path{ g_call_stub_path.load(std::memory_order_relaxed) };
        ctx.record(std::string{ "[INFO] method_overload_java_dispatch dispatch path: " }
                   + (stub_path ? "call_stub fast path (resolve_compatible_method active)"
                                : "call_jni fallback (resolve_compatible_method active)"));

        // ===================================================================
        //  DETOUR OBSERVED THE RIGHT ARGUMENT.  The probe drove SINGLETON.tick(11)
        //  exactly once; the detour read the nonce off the hooked interpreter
        //  frame (latched on the FIRST fire via compare_exchange, so a later
        //  JIT-inlined re-fire cannot perturb it).  This is a single deterministic
        //  ARGUMENT observation — NOT a fire count — so it is HARD-asserted:
        //    * the detour saw a real (non-sentinel) nonce, AND it equals 11;
        //    * Java's own tick() recorded the SAME nonce (cross-side agreement),
        //  proving the hooked call site delivered the right argument both to the
        //  detour's frame read and to the original Java body.
        // ===================================================================
        const std::int32_t observed_nonce{ g_detour_nonce.load(std::memory_order_relaxed) };
        ctx.check("mojd_detour_nonce_observed", observed_nonce != k_nonce_unset);
        ctx.check("mojd_detour_nonce_is_eleven", observed_nonce == k_tick_nonce);
        ctx.check("mojd_java_recorded_same_nonce",
                  overload_dispatch::last_tick_nonce() == k_tick_nonce);
        ctx.check("mojd_detour_and_java_nonce_agree",
                  observed_nonce == overload_dispatch::last_tick_nonce());

        // ===================================================================
        //  NAME-ONLY proxy signature() is the FIRST-BY-NAME overload — NOT the
        //  typed-dispatch target.
        //
        //  CRITICAL DISTINCTION (was the source of two deterministic CI FAILs):
        //  cap_named_* resolves the overload by NAME ONLY via get_method("f")
        //  (vmhook.hpp object::get_method(name), ~14084).  That walks _methods and
        //  returns a proxy for the FIRST method whose NAME is "f", latching THAT
        //  overload's descriptor into the proxy's signature_text.  The subsequent
        //  call(<typed arg>) re-resolves the actual overload to DISPATCH via
        //  resolve_compatible_method() (it reads effective_signature locally and
        //  never writes signature_text back, vmhook.hpp:12534-12540), so
        //  proxy->signature() keeps reporting the first-by-name descriptor, which
        //  is INDEPENDENT of the C++ arg type.
        //
        //  HotSpot sorts InstanceKlass::_methods by (name Symbol, signature Symbol)
        //  identity; for this fixture's `f` family the signature symbol "(II)I"
        //  orders first, so get_method("f") deterministically returns the f(II)I
        //  proxy on every JDK/compiler build (verified on JDK 8 and JDK 21 via
        //  getDeclaredMethods, which reads the same array).  Hence ALL THREE
        //  name-only captures share ONE signature_text (the first-by-name f), and
        //  the previous per-arg-descriptor assertions
        //      named_int_sig_is_I  (expected "(I)I")   — FAILED (got "(II)I")
        //      named_str_sig_is_string (expected (Lstring)Lstring) — FAILED ("(II)I")
        //      named_dual_sig_is_II (expected "(II)I")  — passed BY COINCIDENCE
        //  conflated the name-only proxy's signature with the dispatch target.  The
        //  WHICH-overload-ran proof is the result value + the Java-side readback
        //  (hit counters / arg echoes) below — not the name-only signature.
        //
        //  So: assert the LIBRARY-FAITHFUL invariant (all three name-only proxies
        //  carry the identical first-by-name descriptor), record it as [INFO], and
        //  prove dispatch via the value/kind checks.
        // ===================================================================
        const std::string first_by_name_f{ got("named_int").sig_text };
        ctx.record(std::string{ "[INFO] get_method(\"f\") name-only resolves the FIRST-by-name overload; "
                                "its signature() = '" } + first_by_name_f
                   + "' (HotSpot _methods Symbol-ordering; deterministically (II)I on every build). "
                     "call(<typed arg>) re-resolves the DISPATCH overload independently — "
                     "proxy->signature() is NOT the dispatch target.");

        // (1) C++-TYPED call() — f(int 30) -> 130 (dispatch re-resolves to (I)I).
        // Proves: int arg dispatches the PRIMITIVE overload AND its real value
        // flows back; the Java side recorded exactly this body's effect.
        {
            const dispatch_result r{ got("named_int") };
            ctx.check("named_int_resolved", r.resolved);
            // Name-only signature() is the first-by-name f, NOT the (I)I target.
            ctx.check("named_int_sig_is_first_by_name", r.sig_text == first_by_name_f);
            ctx.check("named_int_not_void", !r.is_void);
            ctx.check("named_int_not_string", !r.is_string);
            ctx.check("named_int_result_is_130", r.ival == F_INT_EXPECT);
        }

        // (1) C++-TYPED call() — f("foo") -> "[foo]" (dispatch re-resolves to the
        // Ljava/lang/String; overload).  Proves: std::string arg dispatches the
        // STRING overload AND the decoded String result reads back as "[foo]".
        {
            const dispatch_result r{ got("named_str") };
            ctx.check("named_str_resolved", r.resolved);
            // Same name-only proxy descriptor as the int capture (first-by-name f).
            ctx.check("named_str_sig_is_first_by_name", r.sig_text == first_by_name_f);
            ctx.check("named_str_is_string", r.is_string);
            ctx.check("named_str_not_void", !r.is_void);
            ctx.check("named_str_result_is_bracketed_foo", r.sval == F_STR_EXPECT);
        }

        // (1) C++-TYPED call() — f(2,3) -> 5 (dispatch re-resolves to (II)I).
        // Proves: a two-int arg pack dispatches the multi-arg overload (NOT the
        // single-int one) AND returns the legacy sum.
        {
            const dispatch_result r{ got("named_dual") };
            ctx.check("named_dual_resolved", r.resolved);
            // Name-only first-by-name f IS (II)I here, but assert it as the shared
            // first-by-name descriptor (not a per-arg target) to stay correct
            // regardless of HotSpot ordering.
            ctx.check("named_dual_sig_is_first_by_name", r.sig_text == first_by_name_f);
            ctx.check("named_dual_not_void", !r.is_void);
            ctx.check("named_dual_result_is_5", r.ival == F_DUAL_EXPECT);
        }

        // All three NAME-ONLY captures used get_method("f"), so they MUST carry the
        // identical first-by-name signature() — the library contract under test.
        ctx.check("named_all_share_first_by_name_signature",
                  got("named_int").sig_text == got("named_str").sig_text
                  && got("named_str").sig_text == got("named_dual").sig_text);

        // ===================================================================
        //  (2) EXPLICIT-SIGNATURE call() — each pinned descriptor dispatches the
        //  SAME overload and returns the SAME legacy value as the typed path.
        // ===================================================================
        {
            const dispatch_result r{ got("sig_int") };
            ctx.check("sig_int_resolved", r.resolved);
            ctx.check("sig_int_sig_is_I", r.sig_text == SIG_F_I);
            ctx.check("sig_int_result_is_130", r.ival == F_INT_EXPECT);
        }
        {
            const dispatch_result r{ got("sig_str") };
            ctx.check("sig_str_resolved", r.resolved);
            ctx.check("sig_str_sig_is_string", r.sig_text == SIG_F_S);
            ctx.check("sig_str_is_string", r.is_string);
            ctx.check("sig_str_result_is_bracketed_foo", r.sval == F_STR_EXPECT);
        }
        {
            const dispatch_result r{ got("sig_dual") };
            ctx.check("sig_dual_resolved", r.resolved);
            ctx.check("sig_dual_sig_is_II", r.sig_text == SIG_F_II);
            ctx.check("sig_dual_result_is_5", r.ival == F_DUAL_EXPECT);
        }

        // ===================================================================
        //  Cross-path agreement: the C++-typed and explicit-signature paths
        //  produced identical results for all three overloads.
        // ===================================================================
        ctx.check("paths_agree_int",  got("named_int").ival  == got("sig_int").ival);
        ctx.check("paths_agree_str",   got("named_str").sval == got("sig_str").sval);
        ctx.check("paths_agree_dual",  got("named_dual").ival == got("sig_dual").ival);

        // ===================================================================
        //  JAVA-SIDE READBACK of each overload's side effect (the headline:
        //  prove from Java's OWN recorded state that the intended body ran).
        //
        //  Each overload was dispatched TWICE (typed + explicit-signature), so:
        //    * its hit counter reads exactly 2,
        //    * its recorded arg(s)/result equal the legacy inputs/outputs.
        //  This is the modular re-host of legacy test_overloaded_methods'
        //  overloadProbeIntResult / overloadProbeStrResult / overloadProbeDualResult
        //  readback — but driven entirely from native overload resolution.
        // ===================================================================
        // f(int): arg 30 echoed, result 130 recorded, body ran exactly twice.
        ctx.check("java_f_int_hits_two",     overload_dispatch::f_int_hits()  == 2);
        ctx.check("java_f_int_arg_echo_30",  overload_dispatch::last_int_arg() == F_INT_ARG);
        ctx.check("java_f_int_result_130",   overload_dispatch::last_int_result() == F_INT_EXPECT);
        // f(String): arg "foo" echoed, result "[foo]" recorded, body ran twice.
        ctx.check("java_f_str_hits_two",     overload_dispatch::f_str_hits()  == 2);
        ctx.check("java_f_str_arg_echo_foo", overload_dispatch::last_str_arg() == F_STR_ARG);
        ctx.check("java_f_str_result_foo",   overload_dispatch::last_str_result() == F_STR_EXPECT);
        // f(int,int): args 2,3 echoed into the right slots, sum 5 recorded, twice.
        ctx.check("java_f_dual_hits_two",    overload_dispatch::f_dual_hits() == 2);
        ctx.check("java_f_dual_a_echo_2",    overload_dispatch::last_dual_a() == F_DUAL_A);
        ctx.check("java_f_dual_b_echo_3",    overload_dispatch::last_dual_b() == F_DUAL_B);
        ctx.check("java_f_dual_sum_5",       overload_dispatch::last_dual_sum() == F_DUAL_EXPECT);

        // ===================================================================
        //  ISOLATION: the THREE `f` overloads are mutually exclusive — each ran
        //  ONLY its own body.  Total f dispatches across both paths is exactly 6
        //  (2 per overload), and no overload's hit count leaked into another.
        // ===================================================================
        ctx.check("isolation_f_total_six",
                  overload_dispatch::f_int_hits()
                      + overload_dispatch::f_str_hits()
                      + overload_dispatch::f_dual_hits() == 6);

        // ===================================================================
        //  MATCHED WIDE-ARG DISPATCH — `h` family (h(int)->(I)I, h(long)->(J)J).
        //
        //  The companion to the no-match probe: each `h` overload is driven by the
        //  EXACT C++ type that matches its descriptor.  h(int) packs ONE I slot;
        //  h(long) packs a TWO-slot J wide arg.  Proving wide-arg marshalling
        //  reaches the right body AND the (widened-to-J) return decodes back, via
        //  BOTH the returned value and Java's per-overload echo.
        // ===================================================================
        {
            const dispatch_result ri{ got("h_int") };
            ctx.check("h_int_resolved", ri.resolved);
            ctx.check("h_int_not_void", !ri.is_void);
            ctx.check("h_int_not_string", !ri.is_string);
            ctx.check("h_int_result_44", ri.ival == H_INT_EXPECT);

            const dispatch_result rj{ got("h_long") };
            ctx.check("h_long_resolved", rj.resolved);
            ctx.check("h_long_not_void", !rj.is_void);
            ctx.check("h_long_not_string", !rj.is_string);
            ctx.check("h_long_result_7007", rj.ival == H_LONG_EXPECT);

            // Explicit-signature spellings dispatch the SAME bodies with the same
            // legacy values, and report the pinned descriptor verbatim.
            const dispatch_result ris{ got("h_int_sig") };
            ctx.check("h_int_sig_resolved", ris.resolved);
            ctx.check("h_int_sig_is_I", ris.sig_text == SIG_H_I);
            ctx.check("h_int_sig_result_44", ris.ival == H_INT_EXPECT);

            const dispatch_result rjs{ got("h_long_sig") };
            ctx.check("h_long_sig_resolved", rjs.resolved);
            ctx.check("h_long_sig_is_J", rjs.sig_text == SIG_H_J);
            ctx.check("h_long_sig_result_7007", rjs.ival == H_LONG_EXPECT);

            // Typed and explicit-signature paths agree per overload.
            ctx.check("h_int_paths_agree",  ri.ival == ris.ival);
            ctx.check("h_long_paths_agree", rj.ival == rjs.ival);
        }

        // ===================================================================
        //  NO-MATCH FALLBACK (primitive-only `h` called with a C++ double).
        //
        //  resolve_compatible_method() walks the hierarchy, finds NO (D) overload,
        //  and returns the FIRST-by-name `h` (NOT monostate — the final
        //  `return this->method`; no fail-safe refusal exists on either call
        //  path).  Both `h` overloads are primitive (no reference slot), so this
        //  is a SAFE dispatch.
        //
        //  Which overload is "first" is HotSpot Symbol-ordering arbitrary across
        //  JDK/compiler builds, so we CHARACTERIZE the ACTUAL behaviour:
        //    * the call resolved + returned a real (non-monostate) value, proving
        //      the documented fall-back-to-first-by-name (NOT a refused no-op);
        //    * the no-match call added EXACTLY ONE dispatch on top of the 4 matched
        //      `h` dispatches (2 per overload above), landing on first-by-name;
        //  and record the observed first-by-name choice as [INFO].
        // ===================================================================
        {
            const dispatch_result r{ got("nomatch_h") };
            ctx.check("nomatch_h_resolved", r.resolved);
            // The documented behaviour: a no-match call does NOT yield monostate;
            // it dispatches the first-by-name overload, so a real value came back.
            ctx.check("nomatch_h_not_void_falls_back_to_first_by_name", !r.is_void);
            ctx.check("nomatch_h_not_string_primitive_family", !r.is_string);

            const std::int32_t h_i{ overload_dispatch::h_int_hits() };
            const std::int32_t h_j{ overload_dispatch::h_long_hits() };
            // Each matched overload fired EXACTLY twice (typed + explicit-sig); the
            // no-match probe added one more to whichever HotSpot orders first.
            ctx.check("matched_h_int_hits_two_floor",  h_i >= 2);
            ctx.check("matched_h_long_hits_two_floor", h_j >= 2);
            // 4 matched + 1 no-match fallback = 5 total `h` dispatches, exactly.
            ctx.check("h_total_dispatches_five", (h_i + h_j) == 5);
            // The no-match added its +1 to exactly ONE overload (not both, not
            // neither): the over-2 surplus is exactly 1.
            ctx.check("nomatch_h_added_exactly_one",
                      ((h_i - 2) + (h_j - 2)) == 1);

            const std::string which{
                (h_i == 3) ? "h(int) [(I)I]"
                           : (h_j == 3) ? "h(long) [(J)J]"
                                        : "<<none — UNEXPECTED>>" };
            ctx.record(std::string{ "[INFO] no-match h(double 9.5): falls back to first-by-name overload = " }
                       + which
                       + " (HotSpot _methods Symbol-ordering arbitrary across builds); returned value = "
                       + std::to_string(r.ival)
                       + ", is_void=" + (r.is_void ? "true" : "false")
                       + ".  resolve_compatible_method() returns this->method on no descriptor match "
                         "— NOT monostate; no call-path fail-safe refusal exists.");
            ctx.record(std::string{ "[INFO] h hits (matched x4 + no-match x1): h(int)=" } + std::to_string(h_i)
                       + " h(long)=" + std::to_string(h_j)
                       + "; lastHArg=" + std::to_string(overload_dispatch::last_h_arg())
                       + " lastHResult=" + std::to_string(overload_dispatch::last_h_result()));
        }

        // ===================================================================
        //  (3) DISTINCT SINGLE-SLOT PRIMITIVE DESCRIPTORS — `g` family.
        //
        //  argument_matches_descriptor maps EACH C++ scalar to ONE exact JVM
        //  descriptor with NO widening between sibling primitives:
        //      int16_t  -> S      int8_t  -> B      char16_t -> C
        //      bool     -> Z      float   -> F      int32_t  -> I
        //  So a C++-typed call("g", <arg>) must dispatch the matching descriptor
        //  overload and NO other.  Each overload is category-1 (single
        //  interpreter slot) so it dispatches identically on the call_stub and
        //  call_jni paths across every JDK.  We prove WHICH ran two ways: the
        //  returned VALUE, and Java's per-overload hit counter + arg echo.
        // ===================================================================
        {
            const dispatch_result rs{ got("g_short") };
            ctx.check("g_short_resolved", rs.resolved);
            ctx.check("g_short_not_void", !rs.is_void);
            ctx.check("g_short_not_string", !rs.is_string);
            ctx.check("g_short_result", rs.ival == G_SHORT_EXPECT);

            const dispatch_result rb{ got("g_byte") };
            ctx.check("g_byte_resolved", rb.resolved);
            ctx.check("g_byte_not_void", !rb.is_void);
            ctx.check("g_byte_result", rb.ival == G_BYTE_EXPECT);

            const dispatch_result rc{ got("g_char") };
            ctx.check("g_char_resolved", rc.resolved);
            ctx.check("g_char_not_void", !rc.is_void);
            ctx.check("g_char_result", rc.ival == G_CHAR_EXPECT);

            const dispatch_result rz{ got("g_bool") };
            ctx.check("g_bool_resolved", rz.resolved);
            ctx.check("g_bool_not_void", !rz.is_void);
            ctx.check("g_bool_result", rz.ival == G_BOOL_EXPECT);

            const dispatch_result ri{ got("g_int") };
            ctx.check("g_int_resolved", ri.resolved);
            ctx.check("g_int_not_void", !ri.is_void);
            ctx.check("g_int_result", ri.ival == G_INT_EXPECT);

            const dispatch_result rf{ got("g_float") };
            ctx.check("g_float_resolved", rf.resolved);
            ctx.check("g_float_not_void", !rf.is_void);
            ctx.check("g_float_not_string", !rf.is_string);
            // float result decoded as the F alternative -> double; tolerate tiny
            // float-rounding (G_FLOAT_EXPECT is exactly representable, but compare
            // with an epsilon to stay portable across FPU rounding modes).
            ctx.check("g_float_result",
                      rf.dval > (static_cast<double>(G_FLOAT_EXPECT) - 0.01)
                      && rf.dval < (static_cast<double>(G_FLOAT_EXPECT) + 0.01));
        }

        // (3b) EXPLICIT-SIGNATURE path for the `g` overloads — pinned descriptor
        // is reported verbatim by signature() and dispatches the same overload.
        {
            const dispatch_result rs{ got("g_short_sig") };
            ctx.check("g_short_sig_resolved", rs.resolved);
            ctx.check("g_short_sig_is_S", rs.sig_text == SIG_G_S);
            ctx.check("g_short_sig_result", rs.ival == G_SHORT_EXPECT);

            const dispatch_result rb{ got("g_byte_sig") };
            ctx.check("g_byte_sig_resolved", rb.resolved);
            ctx.check("g_byte_sig_is_B", rb.sig_text == SIG_G_B);
            ctx.check("g_byte_sig_result", rb.ival == G_BYTE_EXPECT);

            const dispatch_result rc{ got("g_char_sig") };
            ctx.check("g_char_sig_resolved", rc.resolved);
            ctx.check("g_char_sig_is_C", rc.sig_text == SIG_G_C);
            ctx.check("g_char_sig_result", rc.ival == G_CHAR_EXPECT);

            const dispatch_result rz{ got("g_bool_sig") };
            ctx.check("g_bool_sig_resolved", rz.resolved);
            ctx.check("g_bool_sig_is_Z", rz.sig_text == SIG_G_Z);
            ctx.check("g_bool_sig_result", rz.ival == G_BOOL_EXPECT);

            const dispatch_result ri{ got("g_int_sig") };
            ctx.check("g_int_sig_resolved", ri.resolved);
            ctx.check("g_int_sig_is_I", ri.sig_text == SIG_G_I);
            ctx.check("g_int_sig_result", ri.ival == G_INT_EXPECT);

            const dispatch_result rf{ got("g_float_sig") };
            ctx.check("g_float_sig_resolved", rf.resolved);
            ctx.check("g_float_sig_is_F", rf.sig_text == SIG_G_F);
            ctx.check("g_float_sig_result",
                      rf.dval > (static_cast<double>(G_FLOAT_EXPECT) - 0.01)
                      && rf.dval < (static_cast<double>(G_FLOAT_EXPECT) + 0.01));
        }

        // (3c) Cross-path agreement for the `g` family (typed vs explicit-sig).
        ctx.check("g_paths_agree_short", got("g_short").ival == got("g_short_sig").ival);
        ctx.check("g_paths_agree_byte",  got("g_byte").ival  == got("g_byte_sig").ival);
        ctx.check("g_paths_agree_char",  got("g_char").ival  == got("g_char_sig").ival);
        ctx.check("g_paths_agree_bool",  got("g_bool").ival  == got("g_bool_sig").ival);
        ctx.check("g_paths_agree_int",   got("g_int").ival   == got("g_int_sig").ival);

        // (3d) JAVA-SIDE READBACK of the `g` family: each overload fired EXACTLY
        // twice (typed + explicit-sig) and recorded its own arg.  This is the
        // descriptor-disambiguation proof — char16_t did NOT cross into S, int16_t
        // did NOT cross into C, etc.  Each counter == 2 means no sibling leaked.
        ctx.check("java_g_short_hits_two", overload_dispatch::g_short_hits() == 2);
        ctx.check("java_g_byte_hits_two",  overload_dispatch::g_byte_hits()  == 2);
        ctx.check("java_g_char_hits_two",  overload_dispatch::g_char_hits()  == 2);
        ctx.check("java_g_bool_hits_two",  overload_dispatch::g_bool_hits()  == 2);
        ctx.check("java_g_float_hits_two", overload_dispatch::g_float_hits() == 2);
        ctx.check("java_g_int_hits_two",   overload_dispatch::g_int_hits()   == 2);

        // (3e) ISOLATION across the WHOLE `g` family: six overloads, each twice,
        // total = 12 dispatches, none leaked into a sibling descriptor.
        ctx.check("isolation_g_total_twelve",
                  overload_dispatch::g_short_hits()
                      + overload_dispatch::g_byte_hits()
                      + overload_dispatch::g_char_hits()
                      + overload_dispatch::g_bool_hits()
                      + overload_dispatch::g_float_hits()
                      + overload_dispatch::g_int_hits() == 12);

        // ===================================================================
        //  (4) STRING DESCRIPTOR from EVERY C++ String spelling — `sf` family.
        //  std::string, const char*, and std::string_view all map to
        //  Ljava/lang/String; (argument_matches_descriptor's three String forms),
        //  so each dispatches the SAME (and ONLY) sf overload and returns "{bar}".
        //  A fourth call pins the descriptor explicitly.  Proves all spellings
        //  resolve identically and the value flows back on every path.
        // ===================================================================
        {
            const dispatch_result rstr{ got("sf_string") };
            ctx.check("sf_string_resolved", rstr.resolved);
            ctx.check("sf_string_is_string", rstr.is_string);
            ctx.check("sf_string_result", rstr.sval == SF_EXPECT);

            const dispatch_result rc{ got("sf_cstr") };
            ctx.check("sf_cstr_resolved", rc.resolved);
            ctx.check("sf_cstr_is_string", rc.is_string);
            ctx.check("sf_cstr_result", rc.sval == SF_EXPECT);

            const dispatch_result rv{ got("sf_sview") };
            ctx.check("sf_sview_resolved", rv.resolved);
            ctx.check("sf_sview_is_string", rv.is_string);
            ctx.check("sf_sview_result", rv.sval == SF_EXPECT);

            const dispatch_result rsig{ got("sf_sig") };
            ctx.check("sf_sig_resolved", rsig.resolved);
            ctx.check("sf_sig_is_string_sig", rsig.sig_text == SIG_SF);
            ctx.check("sf_sig_is_string", rsig.is_string);
            ctx.check("sf_sig_result", rsig.sval == SF_EXPECT);

            // All FOUR forms agree on the returned value.
            ctx.check("sf_string_forms_agree",
                      rstr.sval == rc.sval && rc.sval == rv.sval && rv.sval == rsig.sval);
        }
        // Java-side readback: the SINGLE sf overload absorbed all FOUR String-typed
        // dispatches (3 typed spellings + 1 explicit-sig) and recorded the arg.
        ctx.check("java_sf_hits_four", overload_dispatch::sf_hits() == 4);
        ctx.check("java_sf_arg_echo",  overload_dispatch::last_sf_arg() == SF_ARG);

        // ===================================================================
        //  (5) POSITION-DEPENDENT MULTI-ARG — `p` family.
        //  p(int,String) and p(String,int) differ ONLY by argument ORDER.  The
        //  C++ pack order drives the descriptor the resolver builds:
        //      call(int, std::string) -> (ILjava/lang/String;)
        //      call(std::string, int) -> (Ljava/lang/String;I)
        //  Proving the resolver respects positional descriptor matching, not just
        //  the SET of argument types.
        // ===================================================================
        {
            const dispatch_result ris{ got("p_is") };
            ctx.check("p_is_resolved", ris.resolved);
            ctx.check("p_is_is_string", ris.is_string);
            ctx.check("p_is_result", ris.sval == P_IS_EXPECT);

            const dispatch_result rsi{ got("p_si") };
            ctx.check("p_si_resolved", rsi.resolved);
            ctx.check("p_si_is_string", rsi.is_string);
            ctx.check("p_si_result", rsi.sval == P_SI_EXPECT);

            // The two results are DISTINCT — the ordering genuinely mattered.
            ctx.check("p_order_distinct", ris.sval != rsi.sval);

            // EXPLICIT-SIGNATURE positional path: the pinned descriptor IS reported
            // verbatim and dispatches the SAME positional overload as the typed pack
            // order, returning the same value.
            const dispatch_result risS{ got("p_is_sig") };
            ctx.check("p_is_sig_resolved", risS.resolved);
            ctx.check("p_is_sig_is_IS", risS.sig_text == SIG_P_IS);
            ctx.check("p_is_sig_is_string", risS.is_string);
            ctx.check("p_is_sig_result", risS.sval == P_IS_EXPECT);

            const dispatch_result rsiS{ got("p_si_sig") };
            ctx.check("p_si_sig_resolved", rsiS.resolved);
            ctx.check("p_si_sig_is_SI", rsiS.sig_text == SIG_P_SI);
            ctx.check("p_si_sig_is_string", rsiS.is_string);
            ctx.check("p_si_sig_result", rsiS.sval == P_SI_EXPECT);

            // Typed and explicit-signature positional paths agree per form, and the
            // two forms remain distinct even on the pinned path.
            ctx.check("p_is_paths_agree", ris.sval == risS.sval);
            ctx.check("p_si_paths_agree", rsi.sval == rsiS.sval);
            ctx.check("p_sig_order_distinct", risS.sval != rsiS.sval);
        }
        // Java-side readback: each positional form fired EXACTLY twice (typed +
        // explicit-sig) and no cross-firing (the int-first calls did NOT hit
        // p(String,int) etc.).  Total positional dispatches = 4, none leaked.
        ctx.check("java_p_is_hits_two", overload_dispatch::p_is_hits() == 2);
        ctx.check("java_p_si_hits_two", overload_dispatch::p_si_hits() == 2);
        ctx.check("isolation_p_total_four",
                  overload_dispatch::p_is_hits() + overload_dispatch::p_si_hits() == 4);
        // The LAST `p` dispatch in run_all was the explicit-sig p(String,int), so
        // Java's shared lastPResult ends as the SI form's formatted output.
        ctx.check("java_p_last_result_si",
                  overload_dispatch::last_p_result() == P_SI_EXPECT);

        // ===================================================================
        //  (6) NO-WIDENING fallback — widen-only `w` (no narrow overload).
        //  argument_matches_descriptor is EXACT-WIDTH: a C++ int (descriptor I)
        //  matches NEITHER w(long)/J NOR w(double)/D.  vmhook does NOT apply Java
        //  widening (int->long / int->double), so resolve_compatible_method finds
        //  no match and falls back to the first-by-name w overload — exactly the
        //  documented no-match behaviour, NOT monostate.  Both fallbacks are
        //  primitive (no reference slot), so this is a SAFE dispatch.  Which
        //  overload is "first" is HotSpot Symbol-ordering arbitrary, so we
        //  characterize ACTUAL behaviour and record the choice as [INFO].
        // ===================================================================
        {
            const dispatch_result r{ got("nomatch_w") };
            ctx.check("nomatch_w_resolved", r.resolved);
            // Real value came back -> fell back to first-by-name (NOT refused).
            ctx.check("nomatch_w_not_void_falls_back", !r.is_void);
            ctx.check("nomatch_w_not_string_primitive_family", !r.is_string);

            const std::int32_t w_l{ overload_dispatch::w_long_hits() };
            const std::int32_t w_d{ overload_dispatch::w_double_hits() };
            // Each matched widen-only overload fired EXACTLY once (matched block
            // below); the no-match int probe added one more to whichever HotSpot
            // orders first.  So the total is 3 and the no-match surplus over the
            // matched baseline of 1-each is exactly 1 — proving NO Java widening
            // (no I overload to match) AND no double-dispatch.
            ctx.check("w_total_dispatches_three", (w_l + w_d) == 3);
            ctx.check("nomatch_w_added_exactly_one",
                      ((w_l - 1) + (w_d - 1)) == 1);
            const std::string which{
                (w_l == 2) ? "w(long) [(J)J]"
                           : (w_d == 2) ? "w(double) [(D)D]"
                                        : "<<none — UNEXPECTED>>" };
            ctx.record(std::string{ "[INFO] no-widening w(int 100): C++ int (I) matched NO widen-only "
                                    "overload (no Java int->long/double widening); fell back to "
                                    "first-by-name = " } + which
                       + " (HotSpot _methods Symbol-ordering arbitrary); returned value = "
                       + std::to_string(r.ival)
                       + ", lastWArg=" + std::to_string(overload_dispatch::last_w_arg()) + ".");
        }

        // ===================================================================
        //  (6b) MATCHED widen-only dispatch — `w` family (long->(J)J, double->(D)D).
        //  When the C++ type already matches the descriptor EXACTLY, no widening is
        //  needed and resolution dispatches the own body.  Proves both TWO-slot
        //  wide kinds marshal + decode: w(long) numeric (J) and w(double) floating
        //  (D, via the value_t arithmetic operator -> double).
        // ===================================================================
        {
            const dispatch_result rl{ got("w_long") };
            ctx.check("w_long_resolved", rl.resolved);
            ctx.check("w_long_not_void", !rl.is_void);
            ctx.check("w_long_not_string", !rl.is_string);
            ctx.check("w_long_result_60013", rl.ival == W_LONG_EXPECT);

            const dispatch_result rd{ got("w_double") };
            ctx.check("w_double_resolved", rd.resolved);
            ctx.check("w_double_not_void", !rd.is_void);
            ctx.check("w_double_not_string", !rd.is_string);
            // D return decodes to double via the value_t arithmetic operator;
            // W_DOUBLE_EXPECT (70002.5) is exactly representable, compare with a
            // tiny epsilon to stay portable across FPU rounding modes.
            ctx.check("w_double_result_70002_5",
                      rd.dval > (W_DOUBLE_EXPECT - 0.01)
                      && rd.dval < (W_DOUBLE_EXPECT + 0.01));

            // Java-side readback: each matched widen-only overload fired at least
            // once (its matched call) and at most twice (matched + the no-match +1).
            // Both firing proves neither matched call leaked into the sibling
            // descriptor; the exact total (3) and surplus (1) are pinned above.
            const std::int32_t wl{ overload_dispatch::w_long_hits() };
            const std::int32_t wd{ overload_dispatch::w_double_hits() };
            ctx.check("java_w_long_matched_fired",  wl >= 1 && wl <= 2);
            ctx.check("java_w_double_matched_fired", wd >= 1 && wd <= 2);
        }

        // ===================================================================
        //  EXTRA CROSS-CHECKS on the EXISTING families (deterministic last-write
        //  + native/Java agreement the prior blocks did not pin).
        // ===================================================================
        // The probe drove SINGLETON.tick(11) EXACTLY once, so the Java tick() body
        // (which bumps tickCount) ran exactly once — a deterministic Java-side
        // count, robust across i2i-vs-JIT (the body executes once per real call).
        ctx.check("mojd_tick_count_exactly_one", overload_dispatch::get_tick_count() == 1);
        // The NATIVE detour fire count is i2i-vs-JIT sensitive (a JIT-recompiled
        // re-fire of the same single call could perturb it), so characterize it as
        // [INFO] rather than hard-asserting an exact native fire count (HARD RULE:
        // exact fire-count asserts are HARD only when robustly observed).
        ctx.record(std::string{ "[INFO] native detour fire count for the single tick(11) call: " }
                   + std::to_string(g_detour_calls.load(std::memory_order_relaxed)));

        // `w` family: typed long/double round-trips agree with the int64 view of
        // each result, and the LAST `w` dispatch in run_all was w(double 2.5), so
        // Java's shared lastWArg ended as (long)2.5 == 2 (deterministic last-write).
        {
            const dispatch_result rl{ got("w_long") };
            const dispatch_result rd{ got("w_double") };
            // w(long) numeric result is ALSO reachable as int64 (same alternative).
            ctx.check("w_long_ival_60013", rl.ival == W_LONG_EXPECT);
            // w(double) double result truncates to int64 70002 via value_t's
            // static_cast conversion (70002.5 -> 70002).
            const std::int64_t wd_trunc{ static_cast<std::int64_t>(got("w_double").dval) };
            ctx.check("w_double_truncates_to_70002", wd_trunc == 70002);
            ctx.check("w_double_dval_positive_matches", rd.dval > 70002.0 && rd.dval < 70003.0);
            // Last w body was w(double 2.5): lastWArg == (long)2.5 == 2.
            ctx.check("java_last_w_arg_is_two", overload_dispatch::last_w_arg() == 2);
        }

        // `g` family: the LAST g dispatch in run_all was the explicit-sig g(float
        // 2.5), whose body records lastGArg=(long)2.5==2 and lastGResult=
        // (long)4002.75==4002 (deterministic last-write).
        ctx.check("java_last_g_arg_is_two",     overload_dispatch::last_g_arg() == 2);
        ctx.check("java_last_g_result_is_4002", overload_dispatch::last_g_result() == 4002);

        // ===================================================================
        //  (7) BOUNDARY / OVERFLOW / DEGENERATE inputs — identity overloads.
        //  Each input must round-trip EXACTLY through native dispatch: the
        //  returned value equals the input, and (cross-check) Java's recorded
        //  echo equals the input too.  bi(int) covers 0/1/-1/INT_MIN/INT_MAX;
        //  bl(long) covers 0/1/-1/LONG_MIN/LONG_MAX + a >32-bit value; es(String)
        //  covers empty/single/large.  These are the "every possible input" edge
        //  cases the legacy-value blocks (fixed inputs) could not reach.
        // ===================================================================
        {
            // bi(int) boundary round-trips — native return == input.
            const dispatch_result bz{ got("bi_zero") };
            ctx.check("bi_zero_resolved", bz.resolved);
            ctx.check("bi_zero_not_void", !bz.is_void);
            ctx.check("bi_zero_not_string", !bz.is_string);
            ctx.check("bi_zero_roundtrip", bz.ival == 0);

            ctx.check("bi_one_roundtrip",  got("bi_one").ival  == 1);
            ctx.check("bi_neg1_roundtrip", got("bi_neg1").ival == -1);
            ctx.check("bi_min_roundtrip",
                      got("bi_min").ival == static_cast<std::int64_t>(INT32_MIN));
            ctx.check("bi_max_roundtrip",
                      got("bi_max").ival == static_cast<std::int64_t>(INT32_MAX));
            ctx.check("bi_42_roundtrip",    got("bi_42").ival    == 42);
            ctx.check("bi_neg42_roundtrip", got("bi_neg42").ival == -42);

            // Explicit-signature path reports the pinned (I)I and round-trips MAX.
            const dispatch_result bms{ got("bi_max_sig") };
            ctx.check("bi_max_sig_resolved", bms.resolved);
            ctx.check("bi_max_sig_is_I", bms.sig_text == SIG_BI);
            ctx.check("bi_max_sig_roundtrip",
                      bms.ival == static_cast<std::int64_t>(INT32_MAX));
            // Typed and explicit-sig agree on the MAX round-trip.
            ctx.check("bi_max_paths_agree", got("bi_max").ival == bms.ival);

            // bl(long) boundary round-trips — native return == input (J wide).
            ctx.check("bl_zero_roundtrip", got("bl_zero").ival == 0);
            ctx.check("bl_one_roundtrip",  got("bl_one").ival  == 1);
            ctx.check("bl_neg1_roundtrip", got("bl_neg1").ival == -1);
            ctx.check("bl_min_roundtrip",  got("bl_min").ival  == INT64_MIN);
            ctx.check("bl_max_roundtrip",  got("bl_max").ival  == INT64_MAX);
            // A value above INT32_MAX proves the wide arg is not truncated to 32 bits.
            ctx.check("bl_wide_roundtrip",
                      got("bl_wide").ival == static_cast<std::int64_t>(0x0000'0001'0000'0000LL));
            ctx.check("bl_wide_not_void", !got("bl_wide").is_void);

            // es(String) degenerate / large round-trips — identity returns the arg.
            const dispatch_result ee{ got("es_empty") };
            ctx.check("es_empty_resolved", ee.resolved);
            ctx.check("es_empty_is_string", ee.is_string);
            ctx.check("es_empty_roundtrip", ee.sval.empty());

            const dispatch_result es1{ got("es_single") };
            ctx.check("es_single_is_string", es1.is_string);
            ctx.check("es_single_roundtrip", es1.sval == "Q");

            const dispatch_result el{ got("es_large") };
            ctx.check("es_large_is_string", el.is_string);
            ctx.check("es_large_len_1024", el.sval.size() == 1024u);
            ctx.check("es_large_roundtrip", el.sval == std::string(1024, 'z'));
        }

        // ── Boundary families: Java-side echo CROSS-CHECKS + exact hit counts ──
        // Hit counts are deterministic: bi fired 8x (7 typed + 1 sig), bl 6x,
        // es 3x.  The LAST call of each family pins Java's shared recorder.
        ctx.check("java_bi_hits_eight", overload_dispatch::bi_hits() == 8);
        ctx.check("java_bl_hits_six",   overload_dispatch::bl_hits() == 6);
        ctx.check("java_es_hits_three", overload_dispatch::es_hits() == 3);
        // Last bi dispatch was bi(INT32_MAX) (the explicit-sig call); last bl was
        // bl(0x1_0000_0000); last es was es(1024 'z').  Java's echo must agree.
        ctx.check("java_last_bi_arg_is_max",
                  overload_dispatch::last_bi_arg() == static_cast<std::int32_t>(INT32_MAX));
        ctx.check("java_last_bl_arg_is_wide",
                  overload_dispatch::last_bl_arg() == static_cast<std::int64_t>(0x0000'0001'0000'0000LL));
        ctx.check("java_last_es_len_1024", overload_dispatch::last_es_len() == 1024);
        ctx.check("java_last_es_arg_large",
                  overload_dispatch::last_es_arg() == std::string(1024, 'z'));
        // Native round-tripped value AGREES with Java's recorded echo for the MAX
        // boundary (native==Java==input — the full cross-check loop).
        ctx.check("bi_max_native_equals_java_echo",
                  got("bi_max").ival == static_cast<std::int64_t>(overload_dispatch::last_bi_arg()));
        ctx.check("bl_wide_native_equals_java_echo",
                  got("bl_wide").ival == overload_dispatch::last_bl_arg());
    }
    // scoped_hook `handle` disarms here — NO hook left armed; shutdown_hooks()
    // intentionally NOT called (other method_* modules share this JVM).
}
