// method_explicit_signature — exhaustive JVM tests for the EXPLICIT-SIGNATURE
// method lookup:  object::get_method(name, signature) selecting ONE overload by
// EXACT JVM descriptor, and yielding NO method (safe no-op call) for a
// wrong/absent/empty signature.
//
// Feature lives in vmhook/ext/vmhook/vmhook.hpp:
//   * instance overload   get_method(name, sig)              : 13678-13720
//       - exact compare  current_signature == method_signature : 13709
//       - returns method_proxy{ this->instance, ... }          : 13711  (see FLAW 1)
//   * static  overload    get_method(type_index, name, sig)  : 13788-13830
//       - exact compare                                         : 13819
//       - returns method_proxy{ nullptr, ... }                  : 13821
//   * deducing-this forwarder  get_method(self, name, sig)    : 13968-13972
//   * portable alias          static_method(name, sig)        : 14035-14039
//   * name-only siblings (latch FIRST by-name)                 : 13626 / 13735
//
// Strategy: explicit-signature selection is proven THREE independent ways so a
// single coincidence can't pass a check:
//   (a) the proxy's signature() accessor equals the requested descriptor,
//   (b) the per-overload Java side effect of the EXPECTED overload fires (and
//       the OTHER overloads' side effects do NOT),
//   (c) the call's return value is the expected overload's result.
//
// IMPORTANT NUANCE (combo block): (a) the LOOKUP is signature-exact, but (b)/(c)
// hold only when the C++ argument type maps to the SAME descriptor as the
// explicit signature.  call() re-runs resolve_compatible_method<args_t...>, and
// when the C++ arg type resolves to a DIFFERENT overload (e.g. std::string vs an
// explicit CharSequence signature), the dispatch follows the ARG TYPE, not the
// explicit signature.  The combo(CharSequence) vs combo(String) case below
// CHARACTERIZES that real vmhook behavior (both probes dispatch combo(String)).
//
// Every get_method(name,sig)->call() runs INSIDE the trigger() detour, where
// vmhook::hotspot::current_java_thread is set (the only context call() works in).
// The module body then reads back the recorded observations and asserts each.
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
    // ---- exact JVM descriptors of every overload (single source of truth) ----
    constexpr const char* SIG_PROC_I   = "(I)I";
    constexpr const char* SIG_PROC_II  = "(II)I";
    constexpr const char* SIG_PROC_J   = "(J)J";
    constexpr const char* SIG_PROC_S   = "(Ljava/lang/String;)Ljava/lang/String;";
    constexpr const char* SIG_PROC_SI  = "(Ljava/lang/String;I)Ljava/lang/String;";
    constexpr const char* SIG_PROC_V   = "()V";
    constexpr const char* SIG_COMBO_CS = "(Ljava/lang/CharSequence;)Ljava/lang/String;";
    constexpr const char* SIG_COMBO_ST = "(Ljava/lang/String;)Ljava/lang/String;";
    constexpr const char* SIG_SMAP_I   = "(I)I";
    constexpr const char* SIG_SMAP_S   = "(Ljava/lang/String;)Ljava/lang/String;";
    constexpr const char* SIG_BASE_I   = "(I)I";
    constexpr const char* SIG_BASE_II  = "(II)I";

    // ---- constants mirrored from MethodExplicitSig.java ----------------------
    constexpr std::int32_t PROC_I_ARG{ 41 };
    constexpr std::int32_t PROC_II_A{ 3 };
    constexpr std::int32_t PROC_II_B{ 9 };
    constexpr std::int64_t PROC_J_ARG{ 5 };
    constexpr std::int32_t PROC_SI_N{ 7 };
    constexpr std::int32_t SMAP_I_ARG{ 21 };
    constexpr std::int32_t BASE_I_ARG{ 100 };
    constexpr std::int32_t BASE_II_A{ 50 };
    constexpr std::int32_t BASE_II_B{ 8 };

    // Wrapper for vmhook.fixtures.MethodExplicitSig.
    class method_explicit_sig : public vmhook::object<method_explicit_sig>
    {
    public:
        explicit method_explicit_sig(vmhook::oop_t instance) noexcept
            : vmhook::object<method_explicit_sig>{ instance }
        {
        }

        // -- handshake --
        static auto set_go(bool v) -> void { static_field("go")->set(v); }
        static auto get_done() -> bool      { return static_field("done")->get(); }
        static auto get_trigger_count() -> std::int32_t { return static_field("triggerCount")->get(); }

        // -- per-overload side-effect tallies (proof of WHICH overload ran) --
        static auto procIntArg() -> std::int32_t   { return static_field("processIntArg")->get(); }
        static auto procIntIntA() -> std::int32_t  { return static_field("processIntIntA")->get(); }
        static auto procIntIntB() -> std::int32_t  { return static_field("processIntIntB")->get(); }
        static auto procLongArg() -> std::int64_t  { return static_field("processLongArg")->get(); }
        static auto procStrArg() -> std::string    { return static_field("processStrArg")->get(); }
        static auto procStrIntS() -> std::string   { return static_field("processStrIntS")->get(); }
        static auto procStrIntN() -> std::int32_t  { return static_field("processStrIntN")->get(); }
        static auto procVoidHits() -> std::int32_t { return static_field("processVoidHits")->get(); }
        static auto comboCsHits() -> std::int32_t  { return static_field("comboCsHits")->get(); }
        static auto comboStHits() -> std::int32_t  { return static_field("comboStHits")->get(); }
        static auto smapIntHits() -> std::int32_t  { return static_field("smapIntHits")->get(); }
        static auto smapStrHits() -> std::int32_t  { return static_field("smapStrHits")->get(); }
    };

    // Reads the inherited-overload tallies off the SUPERCLASS mirror.  The base
    // class is registered under its own wrapper so static_field resolves to the
    // base klass's java.lang.Class mirror (where its statics live).
    class method_explicit_sig_base : public vmhook::object<method_explicit_sig_base>
    {
    public:
        explicit method_explicit_sig_base(vmhook::oop_t instance) noexcept
            : vmhook::object<method_explicit_sig_base>{ instance }
        {
        }
        static auto baseIntSeen() -> std::int32_t    { return static_field("baseIntSeen")->get(); }
        static auto baseIntIntSeen() -> std::int32_t { return static_field("baseIntIntSeen")->get(); }
    };

    // ---------------------------------------------------------------------
    //  Observations recorded inside the detour, read back in the body.
    // ---------------------------------------------------------------------
    std::atomic<int>  g_detour_calls{ 0 };
    std::atomic<bool> g_detour_saw_self{ false };
    std::atomic<bool> g_call_stub_path{ false };

    // For every overload we capture: did get_method(name,sig) RESOLVE (has_value),
    // the proxy's signature() text, the call's numeric/string result.
    struct probe_result
    {
        bool        resolved{ false };
        std::string sig_text{};      // proxy->signature()
        bool        is_void{ false };
        bool        is_string{ false };
        std::int64_t ival{ 0 };      // numeric result (int/long)
        std::string  sval{};         // string result (as_string())
    };

    std::mutex                            g_mutex;
    std::map<std::string, probe_result>   g_res;

    auto put(const std::string& key, const probe_result& r) -> void
    {
        std::lock_guard<std::mutex> lock{ g_mutex };
        g_res[key] = r;
    }
    auto get(const std::string& key) -> probe_result
    {
        std::lock_guard<std::mutex> lock{ g_mutex };
        const auto it{ g_res.find(key) };
        return (it != g_res.end()) ? it->second : probe_result{};
    }

    // --- helpers that resolve by EXACT signature and capture the proxy state --

    // Instance, numeric result.
    auto cap_inst_num(const method_explicit_sig& self,
                      const std::string&         key,
                      const char*                name,
                      const char*                sig) -> void
    {
        probe_result r{};
        auto proxy{ self.get_method(name, sig) };
        if (proxy.has_value())
        {
            r.resolved = true;
            r.sig_text = std::string{ proxy->signature() };
            const vmhook::method_proxy::value_t v{ proxy->call() };
            r.is_void   = v.is_void();
            r.is_string = v.is_string();
            r.ival      = static_cast<std::int64_t>(v);
        }
        put(key, r);
    }

    // Instance, numeric result, ONE int arg.
    auto cap_inst_num_i(const method_explicit_sig& self,
                        const std::string&         key,
                        const char*                name,
                        const char*                sig,
                        std::int32_t               a) -> void
    {
        probe_result r{};
        auto proxy{ self.get_method(name, sig) };
        if (proxy.has_value())
        {
            r.resolved = true;
            r.sig_text = std::string{ proxy->signature() };
            const vmhook::method_proxy::value_t v{ proxy->call(a) };
            r.is_void = v.is_void();
            r.ival    = static_cast<std::int64_t>(v);
        }
        put(key, r);
    }

    // Instance, numeric result, TWO int args.
    auto cap_inst_num_ii(const method_explicit_sig& self,
                         const std::string&         key,
                         const char*                name,
                         const char*                sig,
                         std::int32_t a, std::int32_t b) -> void
    {
        probe_result r{};
        auto proxy{ self.get_method(name, sig) };
        if (proxy.has_value())
        {
            r.resolved = true;
            r.sig_text = std::string{ proxy->signature() };
            const vmhook::method_proxy::value_t v{ proxy->call(a, b) };
            r.is_void = v.is_void();
            r.ival    = static_cast<std::int64_t>(v);
        }
        put(key, r);
    }

    // Instance, numeric result, ONE long arg.
    auto cap_inst_num_j(const method_explicit_sig& self,
                        const std::string&         key,
                        const char*                name,
                        const char*                sig,
                        std::int64_t               a) -> void
    {
        probe_result r{};
        auto proxy{ self.get_method(name, sig) };
        if (proxy.has_value())
        {
            r.resolved = true;
            r.sig_text = std::string{ proxy->signature() };
            const vmhook::method_proxy::value_t v{ proxy->call(a) };
            r.is_void = v.is_void();
            r.ival    = static_cast<std::int64_t>(v);
        }
        put(key, r);
    }

    // Instance, String result, ONE String arg.
    auto cap_inst_str_s(const method_explicit_sig& self,
                        const std::string&         key,
                        const char*                name,
                        const char*                sig,
                        const std::string&         a) -> void
    {
        probe_result r{};
        auto proxy{ self.get_method(name, sig) };
        if (proxy.has_value())
        {
            r.resolved = true;
            r.sig_text = std::string{ proxy->signature() };
            const vmhook::method_proxy::value_t v{ proxy->call(a) };
            r.is_void   = v.is_void();
            r.is_string = v.is_string();
            r.sval      = v.as_string();
        }
        put(key, r);
    }

    // Instance, String result, (String, int) args.
    auto cap_inst_str_si(const method_explicit_sig& self,
                         const std::string&         key,
                         const char*                name,
                         const char*                sig,
                         const std::string&         a, std::int32_t n) -> void
    {
        probe_result r{};
        auto proxy{ self.get_method(name, sig) };
        if (proxy.has_value())
        {
            r.resolved = true;
            r.sig_text = std::string{ proxy->signature() };
            const vmhook::method_proxy::value_t v{ proxy->call(a, n) };
            r.is_void   = v.is_void();
            r.is_string = v.is_string();
            r.sval      = v.as_string();
        }
        put(key, r);
    }

    // Instance, void result, no args.
    auto cap_inst_void(const method_explicit_sig& self,
                       const std::string&         key,
                       const char*                name,
                       const char*                sig) -> void
    {
        probe_result r{};
        auto proxy{ self.get_method(name, sig) };
        if (proxy.has_value())
        {
            r.resolved = true;
            r.sig_text = std::string{ proxy->signature() };
            const vmhook::method_proxy::value_t v{ proxy->call() };
            r.is_void   = v.is_void();
            r.is_string = v.is_string();
        }
        put(key, r);
    }

    // STATIC (via static_method(name,sig)), numeric, one int arg.
    auto cap_stat_num_i(const std::string& key,
                        const char*        name,
                        const char*        sig,
                        std::int32_t       a) -> void
    {
        probe_result r{};
        auto proxy{ method_explicit_sig::static_method(name, sig) };
        if (proxy.has_value())
        {
            r.resolved = true;
            r.sig_text = std::string{ proxy->signature() };
            const vmhook::method_proxy::value_t v{ proxy->call(a) };
            r.is_void = v.is_void();
            r.ival    = static_cast<std::int64_t>(v);
        }
        put(key, r);
    }

    // STATIC, String, one String arg.
    auto cap_stat_str_s(const std::string& key,
                        const char*        name,
                        const char*        sig,
                        const std::string& a) -> void
    {
        probe_result r{};
        auto proxy{ method_explicit_sig::static_method(name, sig) };
        if (proxy.has_value())
        {
            r.resolved = true;
            r.sig_text = std::string{ proxy->signature() };
            const vmhook::method_proxy::value_t v{ proxy->call(a) };
            r.is_void   = v.is_void();
            r.is_string = v.is_string();
            r.sval      = v.as_string();
        }
        put(key, r);
    }

    // A "miss" probe: resolve only, record whether it returned a proxy.
    auto cap_miss(const method_explicit_sig& self,
                  const std::string&         key,
                  const char*                name,
                  const char*                sig) -> void
    {
        probe_result r{};
        auto proxy{ self.get_method(name, sig) };
        r.resolved = proxy.has_value();
        if (proxy.has_value())
        {
            r.sig_text = std::string{ proxy->signature() };
        }
        put(key, r);
    }

    // A static "miss" probe.
    auto cap_miss_static(const std::string& key,
                         const char*        name,
                         const char*        sig) -> void
    {
        probe_result r{};
        auto proxy{ method_explicit_sig::static_method(name, sig) };
        r.resolved = proxy.has_value();
        if (proxy.has_value())
        {
            r.sig_text = std::string{ proxy->signature() };
        }
        put(key, r);
    }

    // A "no-op safety" miss probe: resolve a wrong signature and CALL on the
    // optional only when present.  Proves a missing method never dispatches.
    // (We never deref a nullopt; the point is has_value()==false stays a no-op.)
    auto cap_miss_then_guarded_call(const method_explicit_sig& self,
                                    const std::string&         key,
                                    const char*                name,
                                    const char*                sig,
                                    std::int32_t               a) -> void
    {
        probe_result r{};
        auto proxy{ self.get_method(name, sig) };
        r.resolved = proxy.has_value();
        if (proxy.has_value())
        {
            // Should NOT be reached for a wrong signature; if it ever is, this
            // records the (unexpected) dispatch so the body check fails loudly.
            const vmhook::method_proxy::value_t v{ proxy->call(a) };
            r.ival = static_cast<std::int64_t>(v);
        }
        put(key, r);
    }

    // Run EVERY capture inside the detour against the live receiver.
    auto run_all(const std::unique_ptr<method_explicit_sig>& self) -> void
    {
        if (!self)
        {
            return;
        }
        const method_explicit_sig& s = *self;

        // ============================================================
        //  EXACT-MATCH selection across the whole process(...) family.
        //  Each exact signature must pick its own overload.
        // ============================================================
        cap_inst_num_i (s, "proc_I",  "process", SIG_PROC_I,  PROC_I_ARG);
        cap_inst_num_ii(s, "proc_II", "process", SIG_PROC_II, PROC_II_A, PROC_II_B);
        cap_inst_num_j (s, "proc_J",  "process", SIG_PROC_J,  PROC_J_ARG);
        cap_inst_str_s (s, "proc_S",  "process", SIG_PROC_S,  std::string{ "abc" });
        cap_inst_str_si(s, "proc_SI", "process", SIG_PROC_SI, std::string{ "k" }, PROC_SI_N);
        cap_inst_void  (s, "proc_V",  "process", SIG_PROC_V);

        // ============================================================
        //  combo(CharSequence) vs combo(String): SAME Java String arg can go to
        //  either.  Both proxies are resolved by EXACT descriptor and each keeps
        //  its own Method* (signature() proves it).  BUT call(std::string) re-runs
        //  resolve_compatible_method, and std::string maps to Ljava/lang/String;
        //  only — so at DISPATCH time both proxies land on combo(String).  The
        //  body block below characterizes this (the explicit signature pins the
        //  LOOKUP but not the call-time overload).  See REPORTED bug there.
        // ============================================================
        cap_inst_str_s(s, "combo_CS", "combo", SIG_COMBO_CS, std::string{ "Z" });
        cap_inst_str_s(s, "combo_ST", "combo", SIG_COMBO_ST, std::string{ "Z" });

        // ============================================================
        //  STATIC explicit-signature overloads (type_index path) via the
        //  portable static_method(name, sig) alias.
        // ============================================================
        cap_stat_num_i(    "smap_I", "smap", SIG_SMAP_I, SMAP_I_ARG);
        cap_stat_str_s(    "smap_S", "smap", SIG_SMAP_S, std::string{ "qq" });

        // ============================================================
        //  INHERITED overloads declared on the superclass: the hierarchy walk
        //  in BOTH overloads must find them.
        // ============================================================
        cap_inst_num_i (s, "base_I",  "base", SIG_BASE_I,  BASE_I_ARG);
        cap_inst_num_ii(s, "base_II", "base", SIG_BASE_II, BASE_II_A, BASE_II_B);

        // ============================================================
        //  WRONG-SIGNATURE / ABSENT-SIGNATURE: every one must MISS (nullopt) and
        //  be a safe no-op.  Cover many shapes of "close but wrong".
        // ============================================================
        // right name, signature of a DIFFERENT (nonexistent-on-this-name) shape
        cap_miss(s, "miss_proc_wrong_ret",  "process", "(I)J");                 // (I) exists but returns I, not J
        cap_miss(s, "miss_proc_wrong_arg",  "process", "(D)I");                 // no (D) overload
        cap_miss(s, "miss_proc_swapped",    "process", "(ILjava/lang/String;)Ljava/lang/String;"); // args reversed vs (String,int)
        cap_miss(s, "miss_proc_extra_arg",  "process", "(III)I");              // no 3-int overload
        cap_miss(s, "miss_proc_obj_ret",    "process", "(I)Ljava/lang/Object;"); // (I) returns I not Object
        cap_miss(s, "miss_proc_cs_arg",     "process", "(Ljava/lang/CharSequence;)Ljava/lang/String;"); // process has no CS overload
        // right name, EMPTY signature -> strict miss (NOT a wildcard; see FLAW 2)
        cap_miss(s, "miss_proc_empty_sig",  "process", "");
        // right signature SHAPE but WRONG (nonexistent) name -> must miss.
        // NOTE: the name here must be a genuine typo; "process" itself is a real
        // method and (I)I is one of its real overloads, so using "process" would
        // RESOLVE (that is the proc_I case above), not miss.  Use a name that no
        // method in the hierarchy has.
        cap_miss(s, "miss_wrong_name",      "procezz",  SIG_PROC_I);            // typo'd name (no such method)
        // a signature missing the leading '(' (malformed) -> miss
        cap_miss(s, "miss_malformed_noparen", "process", "I)I");
        // a signature with trailing junk after a real descriptor -> miss
        cap_miss(s, "miss_trailing_junk",   "process", "(I)IX");
        // combo with a signature that does not exist (Object param)
        cap_miss(s, "miss_combo_obj",       "combo", "(Ljava/lang/Object;)Ljava/lang/String;");
        // base with the leaf-only sig family that doesn't exist on base
        cap_miss(s, "miss_base_long",       "base", "(J)J");
        // STATIC miss: smap has (I) and (String); (J) is absent
        cap_miss_static("miss_smap_long",   "smap", "(J)J");
        cap_miss_static("miss_smap_empty",  "smap", "");
        // STATIC miss: asking for an instance-only name via the static overload
        cap_miss_static("miss_static_name_is_instance", "process", SIG_PROC_I);

        // SAFE NO-OP proof: a wrong signature must not dispatch ANY method.  We
        // pick "(I)J" (wrong return) on process; if it wrongly matched process(I)
        // it would set processIntArg.  The body asserts processIntArg stayed at
        // the value left by proc_I above (its own legitimate call), i.e. the miss
        // added no extra dispatch.  Also record resolved==false.
        cap_miss_then_guarded_call(s, "noop_proc_wrong_ret", "process", "(I)J", 999999);
    }
}

VMHOOK_JVM_MODULE(method_explicit_signature)
{
    vmhook::register_class<method_explicit_sig>("vmhook/fixtures/MethodExplicitSig");
    vmhook::register_class<method_explicit_sig_base>("vmhook/fixtures/MethodExplicitSigBase");

    {
        auto handle{ vmhook::scoped_hook<method_explicit_sig>(
            "trigger",
            [](vmhook::return_value&,
               const std::unique_ptr<method_explicit_sig>& self)
            {
                g_detour_calls.fetch_add(1, std::memory_order_relaxed);
                g_detour_saw_self.store(self != nullptr, std::memory_order_relaxed);
                g_call_stub_path.store(
                    vmhook::detail::find_call_stub_entry() != nullptr,
                    std::memory_order_relaxed);
                run_all(self);
            }) };

        ctx.check("mes_hook_installed", handle.installed());

        const bool done{ ctx.run_probe(
            [](bool v) { method_explicit_sig::set_go(v); },
            []() { return method_explicit_sig::get_done(); }) };

        ctx.check("mes_probe_completed", done);
        ctx.check("mes_detour_fired", g_detour_calls.load(std::memory_order_relaxed) >= 1);
        ctx.check("mes_detour_saw_self", g_detour_saw_self.load(std::memory_order_relaxed));
        ctx.check("mes_trigger_count_advanced", method_explicit_sig::get_trigger_count() >= 1);

        const bool stub_path{ g_call_stub_path.load(std::memory_order_relaxed) };
        ctx.record(std::string{ "[INFO] method_explicit_signature dispatch path: " }
                   + (stub_path ? "call_stub (resolve_compatible_method active)"
                                : "call_jni (resolve_compatible_method active)"));

        // ===================================================================
        //  process(I)I  — exact (I)I selection
        // ===================================================================
        {
            const probe_result r{ get("proc_I") };
            ctx.check("proc_I_resolved", r.resolved);
            ctx.check("proc_I_sig_is_exact", r.sig_text == SIG_PROC_I);
            ctx.check("proc_I_not_void", !r.is_void);
            ctx.check("proc_I_returns_arg_plus_1", r.ival == (PROC_I_ARG + 1));
            // side effect: process(I) recorded its arg, the OTHER overloads did not.
            ctx.check("proc_I_side_effect_arg", method_explicit_sig::procIntArg() == PROC_I_ARG);
        }

        // ===================================================================
        //  process(II)I — exact (II)I selection (NOT (I)I)
        // ===================================================================
        {
            const probe_result r{ get("proc_II") };
            ctx.check("proc_II_resolved", r.resolved);
            ctx.check("proc_II_sig_is_exact", r.sig_text == SIG_PROC_II);
            ctx.check("proc_II_returns_a100_plus_b", r.ival == (PROC_II_A * 100 + PROC_II_B));
            ctx.check("proc_II_side_effect_a", method_explicit_sig::procIntIntA() == PROC_II_A);
            ctx.check("proc_II_side_effect_b", method_explicit_sig::procIntIntB() == PROC_II_B);
        }

        // ===================================================================
        //  process(J)J — exact (J)J selection (a long, NOT the int overload)
        // ===================================================================
        {
            const probe_result r{ get("proc_J") };
            ctx.check("proc_J_resolved", r.resolved);
            ctx.check("proc_J_sig_is_exact", r.sig_text == SIG_PROC_J);
            ctx.check("proc_J_returns_arg_plus_1000", r.ival == (PROC_J_ARG + 1000));
            ctx.check("proc_J_side_effect", method_explicit_sig::procLongArg() == PROC_J_ARG);
        }

        // ===================================================================
        //  process(String)String — exact reference-arg selection
        // ===================================================================
        {
            const probe_result r{ get("proc_S") };
            ctx.check("proc_S_resolved", r.resolved);
            ctx.check("proc_S_sig_is_exact", r.sig_text == SIG_PROC_S);
            ctx.check("proc_S_is_string", r.is_string);
            ctx.check("proc_S_returns_prefixed", r.sval == "S:abc");
            ctx.check("proc_S_side_effect", method_explicit_sig::procStrArg() == "abc");
        }

        // ===================================================================
        //  process(String,int)String — exact (String,int) selection
        // ===================================================================
        {
            const probe_result r{ get("proc_SI") };
            ctx.check("proc_SI_resolved", r.resolved);
            ctx.check("proc_SI_sig_is_exact", r.sig_text == SIG_PROC_SI);
            ctx.check("proc_SI_is_string", r.is_string);
            ctx.check("proc_SI_returns_joined", r.sval == "k#7");
            ctx.check("proc_SI_side_effect_s", method_explicit_sig::procStrIntS() == "k");
            ctx.check("proc_SI_side_effect_n", method_explicit_sig::procStrIntN() == PROC_SI_N);
        }

        // ===================================================================
        //  process()V — exact no-arg void selection
        // ===================================================================
        {
            const probe_result r{ get("proc_V") };
            ctx.check("proc_V_resolved", r.resolved);
            ctx.check("proc_V_sig_is_exact", r.sig_text == SIG_PROC_V);
            ctx.check("proc_V_is_void", r.is_void);
            ctx.check("proc_V_is_not_string", !r.is_string);
            ctx.check("proc_V_side_effect_one_hit", method_explicit_sig::procVoidHits() == 1);
        }

        // ===================================================================
        //  combo(CharSequence) vs combo(String): the discriminating case.
        //
        //  LOOKUP is exact and correct: get_method("combo", <CS sig>) and
        //  get_method("combo", <String sig>) each return a proxy carrying its
        //  OWN signature (proven by signature() and by the two differing).
        //
        //  DISPATCH, however, is NOT governed by the explicit signature.  Both
        //  probes call(std::string{"Z"}); on EVERY dispatch path call() runs
        //  resolve_compatible_method<std::string>() (call_jni: vmhook.hpp:12493,
        //  call_stub: vmhook.hpp:13095).  std::string maps to Ljava/lang/String;
        //  ONLY (argument_matches_descriptor, vmhook.hpp:13458-13460); it does
        //  NOT match Ljava/lang/CharSequence;.  So for the CS proxy the fast-path
        //  (signature_matches_arguments on this->signature_text) returns false,
        //  the hierarchy walk finds combo(Ljava/lang/String;), and the CS proxy
        //  DISPATCHES combo(String) anyway.  Net effect: BOTH probes land on
        //  combo(String) -> comboStHits == 2, comboCsHits == 0, and the CS probe
        //  returns "ST:Z" not "CS:Z".
        //
        //  This is a REAL vmhook behavior, NOT a test artifact: explicit-signature
        //  get_method(name,sig) does NOT pin the overload at call() time when the
        //  C++ argument type re-resolves to a different overload.  To actually
        //  invoke combo(CharSequence) a caller must pass a C++ arg whose mapped
        //  descriptor is Ljava/lang/CharSequence; (e.g. a wrapper registered as
        //  java/lang/CharSequence), which std::string is not.  See REPORTED bug
        //  in audit notes (resolve_compatible_method overrides this->signature_text).
        // ===================================================================
        {
            const probe_result cs{ get("combo_CS") };
            const probe_result st{ get("combo_ST") };
            // -- LOOKUP correctness: each proxy carries its OWN exact signature.
            ctx.check("combo_CS_resolved", cs.resolved);
            ctx.check("combo_ST_resolved", st.resolved);
            ctx.check("combo_CS_sig_is_charsequence", cs.sig_text == SIG_COMBO_CS);
            ctx.check("combo_ST_sig_is_string", st.sig_text == SIG_COMBO_ST);
            ctx.check("combo_two_proxies_differ_in_sig", cs.sig_text != st.sig_text);

            ctx.record("[INFO] combo: explicit-signature get_method() LOOKUP is exact "
                       "(CS proxy sig=" + cs.sig_text + ", ST proxy sig=" + st.sig_text
                       + ") and is now PINNED at call() time (FIXED #5): a pinned proxy's "
                       "overload is honoured verbatim, so call(std::string) on the CS proxy "
                       "dispatches combo(CharSequence) (String IS-A CharSequence), not "
                       "combo(String).");
            ctx.record("[INFO] combo observed: comboCsHits=" + std::to_string(method_explicit_sig::comboCsHits())
                       + " comboStHits=" + std::to_string(method_explicit_sig::comboStHits())
                       + " cs.sval=\"" + cs.sval + "\" st.sval=\"" + st.sval + "\"");

            // -- DISPATCH (FIXED #5): the explicit signature is PINNED, so each
            //    proxy dispatches ITS exact overload — the CS proxy reaches
            //    combo(CharSequence) and the ST proxy reaches combo(String).
            ctx.check("combo_CS_dispatch_pinned_to_charsequence",
                      method_explicit_sig::comboCsHits() == 1);
            ctx.check("combo_ST_dispatch_to_string",
                      method_explicit_sig::comboStHits() == 1);
            // Each probe returns ITS OWN overload's result.
            ctx.check("combo_CS_returns_cs_prefixed", cs.sval == "CS:Z");
            ctx.check("combo_ST_returns_st_prefixed", st.sval == "ST:Z");
            // Total dispatches across both combo probes is still exactly two.
            ctx.check("combo_total_two_dispatches",
                      method_explicit_sig::comboCsHits() + method_explicit_sig::comboStHits() == 2);
        }

        // ===================================================================
        //  STATIC smap(I)I and smap(String)String via static_method(name,sig).
        //  The returned proxy carries a NULL owning object (static overload),
        //  so the dispatch must still work and pick the exact overload.
        // ===================================================================
        {
            const probe_result ri{ get("smap_I") };
            ctx.check("smap_I_resolved", ri.resolved);
            ctx.check("smap_I_sig_is_exact", ri.sig_text == SIG_SMAP_I);
            ctx.check("smap_I_returns_double", ri.ival == (SMAP_I_ARG * 2));
            ctx.check("smap_I_side_effect_once", method_explicit_sig::smapIntHits() == 1);

            const probe_result rs{ get("smap_S") };
            ctx.check("smap_S_resolved", rs.resolved);
            ctx.check("smap_S_sig_is_exact", rs.sig_text == SIG_SMAP_S);
            ctx.check("smap_S_is_string", rs.is_string);
            ctx.check("smap_S_returns_prefixed", rs.sval == "M:qq");
            ctx.check("smap_S_side_effect_once", method_explicit_sig::smapStrHits() == 1);
        }

        // ===================================================================
        //  INHERITED base(I)I and base(II)I — hierarchy walk found them, exact
        //  descriptor picked the right arity.
        // ===================================================================
        {
            const probe_result ri{ get("base_I") };
            ctx.check("base_I_resolved", ri.resolved);
            ctx.check("base_I_sig_is_exact", ri.sig_text == SIG_BASE_I);
            ctx.check("base_I_returns_arg_plus_7", ri.ival == (BASE_I_ARG + 7));
            ctx.check("base_I_side_effect", method_explicit_sig_base::baseIntSeen() == BASE_I_ARG);

            const probe_result rii{ get("base_II") };
            ctx.check("base_II_resolved", rii.resolved);
            ctx.check("base_II_sig_is_exact", rii.sig_text == SIG_BASE_II);
            ctx.check("base_II_returns_a_minus_b", rii.ival == (BASE_II_A - BASE_II_B));
            ctx.check("base_II_side_effect",
                      method_explicit_sig_base::baseIntIntSeen() == (BASE_II_A * 1000 + BASE_II_B));
        }

        // ===================================================================
        //  WRONG / ABSENT SIGNATURE -> nullopt (no method), and any guarded call
        //  is a safe no-op (never dispatches).  This is the core "absent
        //  signature yields no method" guarantee.
        // ===================================================================
        ctx.check("miss_proc_wrong_ret_is_nullopt",   !get("miss_proc_wrong_ret").resolved);
        ctx.check("miss_proc_wrong_arg_is_nullopt",   !get("miss_proc_wrong_arg").resolved);
        ctx.check("miss_proc_swapped_is_nullopt",     !get("miss_proc_swapped").resolved);
        ctx.check("miss_proc_extra_arg_is_nullopt",   !get("miss_proc_extra_arg").resolved);
        ctx.check("miss_proc_obj_ret_is_nullopt",     !get("miss_proc_obj_ret").resolved);
        ctx.check("miss_proc_cs_arg_is_nullopt",      !get("miss_proc_cs_arg").resolved);
        ctx.check("miss_proc_empty_sig_is_nullopt",   !get("miss_proc_empty_sig").resolved);
        ctx.check("miss_wrong_name_is_nullopt",       !get("miss_wrong_name").resolved);
        ctx.check("miss_malformed_noparen_is_nullopt", !get("miss_malformed_noparen").resolved);
        ctx.check("miss_trailing_junk_is_nullopt",    !get("miss_trailing_junk").resolved);
        ctx.check("miss_combo_obj_is_nullopt",        !get("miss_combo_obj").resolved);
        ctx.check("miss_base_long_is_nullopt",        !get("miss_base_long").resolved);
        ctx.check("miss_smap_long_is_nullopt",        !get("miss_smap_long").resolved);
        ctx.check("miss_smap_empty_is_nullopt",       !get("miss_smap_empty").resolved);
        // The static overload must NOT find an instance method just by name+sig.
        // process(I)I is an INSTANCE method; the static-by-type_index lookup now
        // filters on JVM_ACC_STATIC (fix: static get_method ACC_STATIC guard), so
        // it correctly REJECTS process(I)I by name+signature.  Resolving an
        // instance method via the static path was always wrong (it yielded a proxy
        // with a null owning object that misbehaves if called static); the lookup
        // now refuses it outright rather than handing back that footgun proxy.
        ctx.check("static_lookup_of_instance_name_rejected",
                  !get("miss_static_name_is_instance").resolved);

        // ===================================================================
        //  SAFE NO-OP: the wrong-signature guarded call never dispatched, so it
        //  left process(I)'s side-effect field exactly where proc_I put it.
        // ===================================================================
        {
            const probe_result r{ get("noop_proc_wrong_ret") };
            ctx.check("noop_proc_wrong_ret_is_nullopt", !r.resolved);
            // process(I) was legitimately called once (proc_I) with PROC_I_ARG.
            // The wrong-(I)J miss must NOT have re-dispatched process(I) with
            // 999999; processIntArg therefore stays at PROC_I_ARG.
            ctx.check("noop_did_not_dispatch_process_I",
                      method_explicit_sig::procIntArg() == PROC_I_ARG);
        }

        // ===================================================================
        //  GLOBAL ISOLATION INVARIANTS across the whole run:
        //  every overload's side effect fired EXACTLY the expected number of
        //  times — no wrong-signature miss leaked into a real dispatch, and no
        //  exact selection picked a sibling.
        // ===================================================================
        // process(II) ran once (proc_II); process(I) ran once (proc_I).  Their
        // recorded args are still the legitimate ones.
        ctx.check("isolation_proc_I_arg_intact", method_explicit_sig::procIntArg() == PROC_I_ARG);
        ctx.check("isolation_proc_II_a_intact",  method_explicit_sig::procIntIntA() == PROC_II_A);
        ctx.check("isolation_proc_II_b_intact",  method_explicit_sig::procIntIntB() == PROC_II_B);
        ctx.check("isolation_proc_J_intact",     method_explicit_sig::procLongArg() == PROC_J_ARG);
        ctx.check("isolation_proc_V_one_hit",    method_explicit_sig::procVoidHits() == 1);
        // combo (FIXED #5): the explicit signature is now PINNED, so each probe
        // dispatched ITS overload exactly once — combo(CharSequence) once and
        // combo(String) once (total two dispatches).
        ctx.check("isolation_combo_cs_one", method_explicit_sig::comboCsHits() == 1);
        ctx.check("isolation_combo_st_one", method_explicit_sig::comboStHits() == 1);
        ctx.check("isolation_combo_sum_two",
                  method_explicit_sig::comboCsHits() + method_explicit_sig::comboStHits() == 2);
        // static smap each exactly once.
        ctx.check("isolation_smap_i_one",        method_explicit_sig::smapIntHits() == 1);
        ctx.check("isolation_smap_s_one",        method_explicit_sig::smapStrHits() == 1);
    }
}
