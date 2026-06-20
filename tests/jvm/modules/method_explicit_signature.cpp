// method_explicit_signature — exhaustive JVM tests for the EXPLICIT-SIGNATURE
// method lookup:  object::get_method(name, signature) selecting ONE overload by
// EXACT JVM descriptor, and yielding NO method (safe no-op call) for a
// wrong/absent/empty signature.
//
// Feature lives in vmhook/ext/vmhook/vmhook.hpp (line numbers as of v0.5.3):
//   * instance overload   get_method(name, sig)              : 16855
//       - exact compare  current_signature == method_signature : 16886
//       - returns method_proxy{ this->instance, ..., pinned } : 16888  (see FLAW 1)
//       - SECOND-CHANCE interface-DEFAULT fallback             : 16898
//         (find_interface_default_method, 17425)
//   * static  overload    get_method(type_index, name, sig)  : 16988
//       - exact compare + static_method_only ACC_STATIC gate   : 17019 / 17125
//       - returns method_proxy{ nullptr, ..., pinned }         : 17022
//   * deducing-this forwarder  get_method(self, name, sig)    : 17587
//   * portable alias          static_method(name, sig)        : 17654
//   * name-only siblings (latch FIRST by-name; NOT pinned)     : 16790 / 16925
//
// Strategy: explicit-signature selection is proven THREE independent ways so a
// single coincidence can't pass a check:
//   (a) the proxy's signature() accessor equals the requested descriptor,
//   (b) the per-overload Java side effect of the EXPECTED overload fires (and
//       the OTHER overloads' side effects do NOT),
//   (c) the call's return value is the expected overload's result.
//
// SIGNATURE PINNING (combo block): an explicit-signature proxy is now PINNED
// (method_proxy.signature_pinned == true), so resolve_compatible_method<args_t...>
// returns the latched Method* VERBATIM and does NOT re-pick a different overload
// from the C++ argument types.  Net: get_method("combo", <CharSequence sig>)->call(
// std::string) dispatches combo(CharSequence), and get_method("combo", <String
// sig>)->call(std::string) dispatches combo(String) — each probe lands on ITS OWN
// overload (comboCsHits == comboStHits == 1).  This is the validated current
// behavior; the pre-pinning code mis-dispatched both to combo(String).
//
// COVERAGE (this module is the exhaustive "every get_method(name,sig) overload
// selection"):
//   * process(...) family — (I)I (II)I (J)J (String)String (String,int)String ()V
//   * combo(CharSequence) vs combo(String) — the discriminating same-arg case
//   * descriptor SHAPES as selector — primitives F D Z B S C, many-arg (IIII)I,
//     and arrays [I [J [Ljava/lang/String; (arrays LOOKUP-ONLY: C++ array args
//     have no JNI marshalling, so the COMPARE is proven without a dispatch)
//   * RETURN TYPE is part of the descriptor — shapes(F)F resolves, (F)V/(F)D/(D)F
//     all MISS (the compare is over the FULL descriptor, return char included)
//   * STATIC smap(I)I / smap(String)String via static_method(name,sig)
//   * ACC_STATIC orthogonality — dupStatic(I)I vs dupInstance(I)I (same descriptor,
//     differ only in kind): static_method gates on ACC_STATIC, instance does NOT
//   * INHERITED base(I)I / base(II)I — superclass-chain walk
//   * INTERFACE DEFAULT ifaceDefault(I)I / (J)J — second-chance interface fallback;
//     ifaceAbstract(I)I concrete override found by the ordinary walk
//   * CONSTRUCTOR <init> ()V / (I)V / (String)V selected by descriptor (lookup-only)
//   * NAME-ONLY vs EXPLICIT contrast — get_method("process") latches the FIRST
//     _methods-order overload; explicit-sig reaches any specific one by descriptor
//   * ~25 wrong/absent/empty/malformed-signature MISS angles (safe no-op)
//   * GLOBAL ISOLATION — every overload's side effect fired EXACTLY its expected
//     count across the whole run (no miss leaked into a dispatch, no sibling picked)
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

    // descriptor-shape selectors (shapes family)
    constexpr const char* SIG_SHAPE_F   = "(F)F";
    constexpr const char* SIG_SHAPE_D   = "(D)D";
    constexpr const char* SIG_SHAPE_Z   = "(Z)Z";
    constexpr const char* SIG_SHAPE_B   = "(B)B";
    constexpr const char* SIG_SHAPE_S   = "(S)S";
    constexpr const char* SIG_SHAPE_C   = "(C)C";
    constexpr const char* SIG_SHAPE_IARR = "([I)I";
    constexpr const char* SIG_SHAPE_JARR = "([J)J";
    constexpr const char* SIG_SHAPE_SARR = "([Ljava/lang/String;)I";
    constexpr const char* SIG_SHAPE_4I   = "(IIII)I";

    // ACC_STATIC orthogonality (dup* family) — identical descriptor, differ in kind
    constexpr const char* SIG_DUP_I    = "(I)I";

    // interface DEFAULT method (iface family)
    constexpr const char* SIG_IFACE_DEF_I = "(I)I";
    constexpr const char* SIG_IFACE_DEF_J = "(J)J";
    constexpr const char* SIG_IFACE_ABS_I = "(I)I";

    // constructor (<init>) descriptors
    constexpr const char* SIG_INIT_V   = "()V";
    constexpr const char* SIG_INIT_I   = "(I)V";
    constexpr const char* SIG_INIT_S   = "(Ljava/lang/String;)V";

    // covariant-return cov() — leaf override + synthetic bridge differ ONLY by ret
    constexpr const char* SIG_COV_STR  = "()Ljava/lang/String;";       // real override
    constexpr const char* SIG_COV_CS   = "()Ljava/lang/CharSequence;"; // synthetic bridge

    // wide-mixed-arg family (multi-slot interleave); base(JI)J inherited
    constexpr const char* SIG_WIDE_JI  = "(JI)J";
    constexpr const char* SIG_WIDE_DI  = "(DI)D";
    constexpr const char* SIG_WIDE_JJ  = "(JJ)J";
    constexpr const char* SIG_BASE_JI  = "(JI)J";

    // void-with-args + object-return descriptors
    constexpr const char* SIG_SINK_IIV = "(II)V";
    constexpr const char* SIG_MAKEOBJ  = "()Ljava/lang/Object;";
    constexpr const char* SIG_MAKENUM  = "()Ljava/lang/Number;";

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

    // shapes(...) args (mirror MethodExplicitSig.SHAPE_*)
    constexpr float        SHAPE_F_ARG{ 1.5f };
    constexpr double       SHAPE_D_ARG{ 2.25 };
    constexpr std::int8_t  SHAPE_B_ARG{ 7 };
    constexpr std::int16_t SHAPE_S_ARG{ 11 };
    constexpr char16_t     SHAPE_C_ARG{ u'Q' };
    constexpr std::int32_t SHAPE_4A{ 1 };
    constexpr std::int32_t SHAPE_4B{ 2 };
    constexpr std::int32_t SHAPE_4C{ 3 };
    constexpr std::int32_t SHAPE_4D{ 4 };

    // dup* args
    constexpr std::int32_t DUP_INST_ARG{ 60 };
    constexpr std::int32_t DUP_STAT_ARG{ 70 };

    // iface args
    constexpr std::int32_t IFACE_DEF_I_ARG{ 19 };
    constexpr std::int64_t IFACE_DEF_J_ARG{ 4 };
    constexpr std::int32_t IFACE_ABS_ARG{ 80 };

    // wide-mixed-arg + void-with-args + inherited-wide args (mirror Java)
    constexpr std::int64_t WIDE_JI_J_ARG{ 6 };
    constexpr std::int32_t WIDE_JI_I_ARG{ 5 };
    constexpr double       WIDE_DI_D_ARG{ 2.5 };
    constexpr std::int32_t WIDE_DI_I_ARG{ 4 };
    constexpr std::int64_t WIDE_JJ_A_ARG{ 40 };
    constexpr std::int64_t WIDE_JJ_B_ARG{ 9 };
    constexpr std::int32_t SINK_II_A{ 12 };
    constexpr std::int32_t SINK_II_B{ 34 };
    constexpr std::int64_t BASE_JI_A{ 8 };
    constexpr std::int32_t BASE_JI_B{ 3 };

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
        static auto covStrSeen() -> std::string    { return static_field("covStrSeen")->get(); }
        static auto wideJiSeen() -> std::int64_t   { return static_field("wideJiSeen")->get(); }
        static auto wideDiSeen() -> double         { return static_field("wideDiSeen")->get(); }
        static auto wideJjSeen() -> std::int64_t   { return static_field("wideJjSeen")->get(); }
        static auto sinkIiSeen() -> std::int32_t   { return static_field("sinkIiSeen")->get(); }
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
        static auto baseLongIntSeen() -> std::int64_t { return static_field("baseLongIntSeen")->get(); }
        static auto covBaseSeen() -> std::int32_t    { return static_field("covBaseSeen")->get(); }
    };

    // Reads side-effect tallies written from places that cannot host mutable
    // static state directly (interface DEFAULT methods, etc.).  Resolves to the
    // MethodExplicitSigCounters mirror.
    class method_explicit_sig_counters : public vmhook::object<method_explicit_sig_counters>
    {
    public:
        explicit method_explicit_sig_counters(vmhook::oop_t instance) noexcept
            : vmhook::object<method_explicit_sig_counters>{ instance }
        {
        }
        static auto ifaceDefaultIntSeen() -> std::int32_t  { return static_field("ifaceDefaultIntSeen")->get(); }
        static auto ifaceDefaultLongSeen() -> std::int64_t { return static_field("ifaceDefaultLongSeen")->get(); }
        static auto ifaceAbstractSeen() -> std::int32_t    { return static_field("ifaceAbstractSeen")->get(); }
        static auto dupInstanceSeen() -> std::int32_t      { return static_field("dupInstanceSeen")->get(); }
        static auto dupStaticSeen() -> std::int32_t        { return static_field("dupStaticSeen")->get(); }
        static auto shapeFloatSeen() -> float              { return static_field("shapeFloatSeen")->get(); }
        static auto shapeDoubleSeen() -> double            { return static_field("shapeDoubleSeen")->get(); }
        static auto shapeBoolSeen() -> std::int32_t        { return static_field("shapeBoolSeen")->get(); }
        static auto shapeByteSeen() -> std::int32_t        { return static_field("shapeByteSeen")->get(); }
        static auto shapeShortSeen() -> std::int32_t       { return static_field("shapeShortSeen")->get(); }
        static auto shapeCharSeen() -> std::int32_t        { return static_field("shapeCharSeen")->get(); }
        static auto shapeIntArrSeen() -> std::int32_t      { return static_field("shapeIntArrSeen")->get(); }
        static auto shapeLongArrSeen() -> std::int64_t     { return static_field("shapeLongArrSeen")->get(); }
        static auto shapeStrArrSeen() -> std::int32_t      { return static_field("shapeStrArrSeen")->get(); }
        static auto shapeFourArgSeen() -> std::int32_t     { return static_field("shapeFourArgSeen")->get(); }
        static auto initIntSeen() -> std::int32_t          { return static_field("initIntSeen")->get(); }
        static auto initStrSeen() -> std::string           { return static_field("initStrSeen")->get(); }
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
        std::string name_text{};     // proxy->name()  (orthogonal latched-Method* proof)
        bool        is_static{ false };  // proxy->is_static() (JVM_ACC_STATIC truth)
        bool        is_void{ false };
        bool        is_string{ false };
        std::int64_t ival{ 0 };      // numeric result (int/long/byte/short/char/bool)
        double       dval{ 0.0 };    // floating result (float/double)
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

    // Snapshot helpers: store a single canonical scalar/string under `key` so the
    // body can read the value AS IT WAS at the snapshot point, independent of any
    // later (intentional) re-dispatch that overwrites the live Java field.
    auto put_ival(const std::string& key, std::int64_t v) -> void
    {
        probe_result r{};
        r.ival = v;
        put(key, r);
    }
    auto put_dval(const std::string& key, double v) -> void
    {
        probe_result r{};
        r.dval = v;
        put(key, r);
    }
    auto put_sval(const std::string& key, const std::string& v) -> void
    {
        probe_result r{};
        r.sval = v;
        put(key, r);
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
            r.name_text = proxy->name();
            r.is_static = proxy->is_static();
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
            r.name_text = proxy->name();
            r.is_static = proxy->is_static();
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
            r.name_text = proxy->name();
            r.is_static = proxy->is_static();
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
            r.name_text = proxy->name();
            r.is_static = proxy->is_static();
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
            r.name_text = proxy->name();
            r.is_static = proxy->is_static();
            const vmhook::method_proxy::value_t v{ proxy->call(a) };
            r.is_void   = v.is_void();
            r.is_string = v.is_string();
            r.sval      = v.as_string();
        }
        put(key, r);
    }

    // Instance, String result, NO args (e.g. covariant cov()Ljava/lang/String;).
    auto cap_inst_str_s_noarg(const method_explicit_sig& self,
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
            r.name_text = proxy->name();
            r.is_static = proxy->is_static();
            const vmhook::method_proxy::value_t v{ proxy->call() };
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
            r.name_text = proxy->name();
            r.is_static = proxy->is_static();
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
            r.name_text = proxy->name();
            r.is_static = proxy->is_static();
            const vmhook::method_proxy::value_t v{ proxy->call() };
            r.is_void   = v.is_void();
            r.is_string = v.is_string();
        }
        put(key, r);
    }

    // Instance, FLOAT result, one float arg.
    auto cap_inst_flt_f(const method_explicit_sig& self,
                        const std::string&         key,
                        const char*                name,
                        const char*                sig,
                        float                      a) -> void
    {
        probe_result r{};
        auto proxy{ self.get_method(name, sig) };
        if (proxy.has_value())
        {
            r.resolved = true;
            r.sig_text = std::string{ proxy->signature() };
            r.name_text = proxy->name();
            r.is_static = proxy->is_static();
            const vmhook::method_proxy::value_t v{ proxy->call(a) };
            r.is_void = v.is_void();
            r.dval    = static_cast<double>(static_cast<float>(v));
        }
        put(key, r);
    }

    // Instance, DOUBLE result, one double arg.
    auto cap_inst_dbl_d(const method_explicit_sig& self,
                        const std::string&         key,
                        const char*                name,
                        const char*                sig,
                        double                     a) -> void
    {
        probe_result r{};
        auto proxy{ self.get_method(name, sig) };
        if (proxy.has_value())
        {
            r.resolved = true;
            r.sig_text = std::string{ proxy->signature() };
            r.name_text = proxy->name();
            r.is_static = proxy->is_static();
            const vmhook::method_proxy::value_t v{ proxy->call(a) };
            r.is_void = v.is_void();
            r.dval    = static_cast<double>(v);
        }
        put(key, r);
    }

    // Instance, BOOLEAN result, one bool arg.
    auto cap_inst_bool_z(const method_explicit_sig& self,
                         const std::string&         key,
                         const char*                name,
                         const char*                sig,
                         bool                       a) -> void
    {
        probe_result r{};
        auto proxy{ self.get_method(name, sig) };
        if (proxy.has_value())
        {
            r.resolved = true;
            r.sig_text = std::string{ proxy->signature() };
            r.name_text = proxy->name();
            r.is_static = proxy->is_static();
            const vmhook::method_proxy::value_t v{ proxy->call(a) };
            r.is_void = v.is_void();
            r.ival    = static_cast<std::int64_t>(static_cast<bool>(v) ? 1 : 0);
        }
        put(key, r);
    }

    // Instance, numeric result, one BYTE arg (descriptor B).
    auto cap_inst_byte_b(const method_explicit_sig& self,
                         const std::string&         key,
                         const char*                name,
                         const char*                sig,
                         std::int8_t                a) -> void
    {
        probe_result r{};
        auto proxy{ self.get_method(name, sig) };
        if (proxy.has_value())
        {
            r.resolved = true;
            r.sig_text = std::string{ proxy->signature() };
            r.name_text = proxy->name();
            r.is_static = proxy->is_static();
            const vmhook::method_proxy::value_t v{ proxy->call(a) };
            r.is_void = v.is_void();
            r.ival    = static_cast<std::int64_t>(v);
        }
        put(key, r);
    }

    // Instance, numeric result, one SHORT arg (descriptor S).
    auto cap_inst_short_s(const method_explicit_sig& self,
                          const std::string&         key,
                          const char*                name,
                          const char*                sig,
                          std::int16_t               a) -> void
    {
        probe_result r{};
        auto proxy{ self.get_method(name, sig) };
        if (proxy.has_value())
        {
            r.resolved = true;
            r.sig_text = std::string{ proxy->signature() };
            r.name_text = proxy->name();
            r.is_static = proxy->is_static();
            const vmhook::method_proxy::value_t v{ proxy->call(a) };
            r.is_void = v.is_void();
            r.ival    = static_cast<std::int64_t>(v);
        }
        put(key, r);
    }

    // Instance, numeric result, one CHAR arg (descriptor C; char16_t).
    auto cap_inst_char_c(const method_explicit_sig& self,
                         const std::string&         key,
                         const char*                name,
                         const char*                sig,
                         char16_t                   a) -> void
    {
        probe_result r{};
        auto proxy{ self.get_method(name, sig) };
        if (proxy.has_value())
        {
            r.resolved = true;
            r.sig_text = std::string{ proxy->signature() };
            r.name_text = proxy->name();
            r.is_static = proxy->is_static();
            const vmhook::method_proxy::value_t v{ proxy->call(a) };
            r.is_void = v.is_void();
            r.ival    = static_cast<std::int64_t>(v);
        }
        put(key, r);
    }

    // Instance, numeric result, FOUR int args (many-arg).
    auto cap_inst_num_4i(const method_explicit_sig& self,
                         const std::string&         key,
                         const char*                name,
                         const char*                sig,
                         std::int32_t a, std::int32_t b,
                         std::int32_t c, std::int32_t d) -> void
    {
        probe_result r{};
        auto proxy{ self.get_method(name, sig) };
        if (proxy.has_value())
        {
            r.resolved = true;
            r.sig_text = std::string{ proxy->signature() };
            r.name_text = proxy->name();
            r.is_static = proxy->is_static();
            const vmhook::method_proxy::value_t v{ proxy->call(a, b, c, d) };
            r.is_void = v.is_void();
            r.ival    = static_cast<std::int64_t>(v);
        }
        put(key, r);
    }

    // Instance, numeric result, (long, int) args — wide+narrow interleave.
    auto cap_inst_num_ji(const method_explicit_sig& self,
                         const std::string&         key,
                         const char*                name,
                         const char*                sig,
                         std::int64_t a, std::int32_t b) -> void
    {
        probe_result r{};
        auto proxy{ self.get_method(name, sig) };
        if (proxy.has_value())
        {
            r.resolved = true;
            r.sig_text = std::string{ proxy->signature() };
            r.name_text = proxy->name();
            r.is_static = proxy->is_static();
            const vmhook::method_proxy::value_t v{ proxy->call(a, b) };
            r.is_void = v.is_void();
            r.ival    = static_cast<std::int64_t>(v);
        }
        put(key, r);
    }

    // Instance, DOUBLE result, (double, int) args — wide+narrow interleave.
    auto cap_inst_dbl_di(const method_explicit_sig& self,
                         const std::string&         key,
                         const char*                name,
                         const char*                sig,
                         double a, std::int32_t b) -> void
    {
        probe_result r{};
        auto proxy{ self.get_method(name, sig) };
        if (proxy.has_value())
        {
            r.resolved = true;
            r.sig_text = std::string{ proxy->signature() };
            r.name_text = proxy->name();
            r.is_static = proxy->is_static();
            const vmhook::method_proxy::value_t v{ proxy->call(a, b) };
            r.is_void = v.is_void();
            r.dval    = static_cast<double>(v);
        }
        put(key, r);
    }

    // Instance, numeric result, (long, long) args — two wide slots.
    auto cap_inst_num_jj(const method_explicit_sig& self,
                         const std::string&         key,
                         const char*                name,
                         const char*                sig,
                         std::int64_t a, std::int64_t b) -> void
    {
        probe_result r{};
        auto proxy{ self.get_method(name, sig) };
        if (proxy.has_value())
        {
            r.resolved = true;
            r.sig_text = std::string{ proxy->signature() };
            r.name_text = proxy->name();
            r.is_static = proxy->is_static();
            const vmhook::method_proxy::value_t v{ proxy->call(a, b) };
            r.is_void = v.is_void();
            r.ival    = static_cast<std::int64_t>(v);
        }
        put(key, r);
    }

    // Instance, VOID result, (int, int) args — void-with-args dispatch.
    auto cap_inst_void_ii(const method_explicit_sig& self,
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
            r.name_text = proxy->name();
            r.is_static = proxy->is_static();
            const vmhook::method_proxy::value_t v{ proxy->call(a, b) };
            r.is_void   = v.is_void();
            r.is_string = v.is_string();
        }
        put(key, r);
    }

    // LOOKUP-ONLY instance probe: resolve by exact signature, record resolved +
    // sig_text, do NOT call().  Used for shapes whose C++ arg type has no JNI
    // marshalling (arrays) and for <init> (calling a constructor on a live
    // instance is not a meaningful dispatch — selection-by-descriptor is the
    // feature being proven).
    auto cap_lookup_only(const method_explicit_sig& self,
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
            r.name_text = proxy->name();
            r.is_static = proxy->is_static();
        }
        put(key, r);
    }

    // NAME-ONLY probe: resolve get_method(name) WITHOUT a signature.  Records
    // the signature() of whichever overload the name-only path latched FIRST
    // (the proxy is NOT signature_pinned).  Used to CONTRAST with the
    // explicit-signature path: name-only picks the first _methods-array match;
    // explicit-signature picks a SPECIFIC (often non-first) overload by descriptor.
    auto cap_name_only(const method_explicit_sig& self,
                       const std::string&         key,
                       const char*                name) -> void
    {
        probe_result r{};
        auto proxy{ self.get_method(name) };
        r.resolved = proxy.has_value();
        if (proxy.has_value())
        {
            r.sig_text = std::string{ proxy->signature() };
            r.name_text = proxy->name();
            r.is_static = proxy->is_static();
        }
        put(key, r);
    }

    // NAME-ONLY probe that also CALLS with one int arg.  resolve_compatible_method
    // re-resolves the overload from the C++ arg type (name-only proxies are NOT
    // pinned), so call(int) on a name-only "process" proxy dispatches process(I)I
    // regardless of which overload was latched first.  Records the dispatched
    // result + the latched signature.
    auto cap_name_only_call_i(const method_explicit_sig& self,
                              const std::string&         key,
                              const char*                name,
                              std::int32_t               a) -> void
    {
        probe_result r{};
        auto proxy{ self.get_method(name) };
        r.resolved = proxy.has_value();
        if (proxy.has_value())
        {
            r.sig_text = std::string{ proxy->signature() };
            r.name_text = proxy->name();
            r.is_static = proxy->is_static();
            const vmhook::method_proxy::value_t v{ proxy->call(a) };
            r.is_void = v.is_void();
            r.ival    = static_cast<std::int64_t>(v);
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
            r.name_text = proxy->name();
            r.is_static = proxy->is_static();
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
            r.name_text = proxy->name();
            r.is_static = proxy->is_static();
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
            r.name_text = proxy->name();
            r.is_static = proxy->is_static();
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
            r.name_text = proxy->name();
            r.is_static = proxy->is_static();
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
        //  either.  Both proxies resolve by EXACT descriptor and each keeps its
        //  own Method* (signature() proves it).  Because the proxy is PINNED,
        //  call(std::string) honours the latched overload VERBATIM — the CS proxy
        //  dispatches combo(CharSequence) and the ST proxy dispatches combo(String)
        //  (comboCsHits == comboStHits == 1).  The body block asserts that pinned
        //  dispatch end-to-end.
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
        // INHERITED wide-mixed-arg overload base(JI)J — the hierarchy walk must
        // pick this multi-slot (long+int) descriptor distinctly from base(I)I /
        // base(II)I, and the call must pack the two interpreter slots correctly.
        cap_inst_num_ji(s, "base_JI", "base", SIG_BASE_JI, BASE_JI_A, BASE_JI_B);

        // ============================================================
        //  COVARIANT-RETURN selection: cov() is declared CharSequence on the base
        //  and overridden to String on the leaf, so javac emits BOTH the real
        //  override ()Ljava/lang/String; AND a synthetic bridge
        //  ()Ljava/lang/CharSequence; onto the leaf.  The two entries differ ONLY
        //  by return descriptor — the exact-signature compare keys on the FULL
        //  descriptor and selects each independently.  We call() the real override
        //  (String return) and prove its side effect + return; the bridge is
        //  LOOKUP-ONLY (dispatching the bridge re-enters the override, so calling
        //  it is not an independent observation — its EXISTENCE by exact descriptor
        //  is the feature).  On a JDK that does not synthesize the bridge the
        //  lookup simply MISSES, which we characterize as [INFO] rather than red.
        // ============================================================
        cap_inst_str_s_noarg(s, "cov_str", "cov", SIG_COV_STR);
        cap_lookup_only(s, "cov_bridge", "cov", SIG_COV_CS);

        // ============================================================
        //  WIDE-MIXED-ARG family (wide): multi-slot descriptors interleaving a
        //  long/double (two interpreter slots) with a narrow arg.  Each exact
        //  descriptor selects its own overload and the dispatch packs the slots.
        // ============================================================
        cap_inst_num_ji(s, "wide_JI", "wide", SIG_WIDE_JI, WIDE_JI_J_ARG, WIDE_JI_I_ARG);
        cap_inst_dbl_di(s, "wide_DI", "wide", SIG_WIDE_DI, WIDE_DI_D_ARG, WIDE_DI_I_ARG);
        cap_inst_num_jj(s, "wide_JJ", "wide", SIG_WIDE_JJ, WIDE_JJ_A_ARG, WIDE_JJ_B_ARG);

        // ============================================================
        //  VOID-WITH-ARGS: sink(II)V — the 'V' return descriptor selected
        //  alongside a NON-empty parameter list (distinct from process()V).
        // ============================================================
        cap_inst_void_ii(s, "sink_IIV", "sink", SIG_SINK_IIV, SINK_II_A, SINK_II_B);

        // ============================================================
        //  OBJECT-RETURN descriptors (non-String): makeObj()Object and
        //  makeNum()Number selected by exact descriptor.  LOOKUP-ONLY (calling a
        //  reference-returning method yields a wrapper, not the feature here); the
        //  point is the exact compare distinguishes ()Ljava/lang/Object; from
        //  ()Ljava/lang/Number; and rejects a wrong object-return descriptor.
        // ============================================================
        cap_lookup_only(s, "make_obj", "makeObj", SIG_MAKEOBJ);
        cap_lookup_only(s, "make_num", "makeNum", SIG_MAKENUM);
        // wrong object-return descriptor on makeObj must MISS (it returns Object,
        // not String/Number); and makeNum with the Object descriptor must MISS.
        cap_miss(s, "miss_makeobj_as_string", "makeObj", "()Ljava/lang/String;");
        cap_miss(s, "miss_makenum_as_object", "makeNum", SIG_MAKEOBJ);

        // wide family wrong / malformed descriptors -> MISS:
        cap_miss(s, "miss_wide_ij",  "wide", "(IJ)J");   // slot order swapped (int then long)
        cap_miss(s, "miss_wide_id",  "wide", "(ID)D");   // swapped int/double
        cap_miss(s, "miss_wide_ji_wrong_ret", "wide", "(JI)I"); // (JI) returns J not I
        cap_miss(s, "miss_sink_ret", "sink", "(II)I");   // sink returns V not I
        cap_miss(s, "miss_sink_arity", "sink", "(I)V");  // no single-int sink overload
        // covariant: a third return descriptor that does NOT exist for cov().
        cap_miss(s, "miss_cov_object_ret", "cov", SIG_MAKEOBJ); // no ()Object cov

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

        // ============================================================
        //  DESCRIPTOR-SHAPE SELECTORS (shapes family): every JVM primitive
        //  descriptor (F D Z B S C) selected + dispatched, plus a many-arg
        //  (IIII).  Array shapes ([I [J [Ljava/lang/String;) are LOOKUP-ONLY
        //  (no JNI marshalling for C++ array args), proving the descriptor
        //  COMPARE handles array tokens even though the call path can't pack them.
        // ============================================================
        cap_inst_flt_f  (s, "shape_F", "shapes", SIG_SHAPE_F, SHAPE_F_ARG);
        cap_inst_dbl_d  (s, "shape_D", "shapes", SIG_SHAPE_D, SHAPE_D_ARG);
        cap_inst_bool_z (s, "shape_Z", "shapes", SIG_SHAPE_Z, true);
        cap_inst_byte_b (s, "shape_B", "shapes", SIG_SHAPE_B, SHAPE_B_ARG);
        cap_inst_short_s(s, "shape_S", "shapes", SIG_SHAPE_S, SHAPE_S_ARG);
        cap_inst_char_c (s, "shape_C", "shapes", SIG_SHAPE_C, SHAPE_C_ARG);
        cap_inst_num_4i (s, "shape_4I", "shapes", SIG_SHAPE_4I, SHAPE_4A, SHAPE_4B, SHAPE_4C, SHAPE_4D);
        cap_lookup_only (s, "shape_IARR", "shapes", SIG_SHAPE_IARR);
        cap_lookup_only (s, "shape_JARR", "shapes", SIG_SHAPE_JARR);
        cap_lookup_only (s, "shape_SARR", "shapes", SIG_SHAPE_SARR);
        // Wrong array descriptors must MISS (e.g. char[] when only int[] exists).
        cap_miss(s, "miss_shape_carr", "shapes", "([C)I");                 // no char[] overload
        cap_miss(s, "miss_shape_2d",   "shapes", "([[I)I");                // no int[][] overload
        cap_miss(s, "miss_shape_iarr_wrong_ret", "shapes", "([I)J");       // int[] returns I not J

        // ============================================================
        //  RETURN TYPE IS PART OF THE DESCRIPTOR (characterization).  The
        //  exact compare in get_method(name,sig) matches the WHOLE descriptor
        //  including the return.  Positive: asking shapes(F) with the EXACT
        //  "(F)F" return resolves; negative twins (same params, wrong return)
        //  MISS.  Java forbids overload-by-return-type-only, so the negative
        //  can never accidentally hit a sibling — it proves the return char is
        //  load-bearing in the compare, not merely the parameter list.
        // ============================================================
        cap_miss(s, "ret_shape_F_wrong_V", "shapes", "(F)V");             // (F) returns F not void
        cap_miss(s, "ret_shape_F_wrong_D", "shapes", "(F)D");             // (F) returns F not D
        cap_miss(s, "ret_shape_D_wrong_F", "shapes", "(D)F");             // (D) returns D not F
        cap_miss(s, "ret_shape_Z_wrong_I", "shapes", "(Z)I");            // (Z) returns Z not I

        // ============================================================
        //  ACC_STATIC ORTHOGONALITY (dup* family).  Same descriptor (I)I,
        //  differ only in KIND.  The static overload filters on JVM_ACC_STATIC;
        //  the instance overload does NOT (FLAW 1 — characterized).
        // ============================================================
        // static path picks the STATIC method (null owner, correct dispatch).
        cap_stat_num_i(    "dup_stat_via_static", "dupStatic",   SIG_DUP_I, DUP_STAT_ARG);
        // static path REJECTS the instance-only method (ACC_STATIC gate).
        cap_miss_static(   "dup_inst_via_static", "dupInstance", SIG_DUP_I);
        // instance path picks the INSTANCE method (real receiver, correct).
        cap_inst_num_i (s, "dup_inst_via_inst", "dupInstance",   SIG_DUP_I, DUP_INST_ARG);
        // instance path ALSO resolves the STATIC method (no ACC_STATIC gate):
        // lookup succeeds and carries the exact descriptor.  We do NOT call() it
        // (FLAW 1: it would push a phantom receiver); LOOKUP characterization only.
        cap_lookup_only(s, "dup_stat_via_inst", "dupStatic",     SIG_DUP_I);

        // ============================================================
        //  INTERFACE DEFAULT method by signature (second-chance fallback inside
        //  the instance get_method(name,sig) — find_interface_default_method).
        //  The superclass walk can't see it; only the interface fallback can.
        //  Two default overloads share the name, so the exact descriptor selects.
        // ============================================================
        cap_inst_num_i (s, "iface_def_I", "ifaceDefault", SIG_IFACE_DEF_I, IFACE_DEF_I_ARG);
        cap_inst_num_j (s, "iface_def_J", "ifaceDefault", SIG_IFACE_DEF_J, IFACE_DEF_J_ARG);
        // ifaceAbstract has a CONCRETE override on the class: the ordinary
        // superclass/own-method walk finds it (NOT the interface fallback, which
        // skips abstract declarations).  It resolves + dispatches the override.
        cap_inst_num_i (s, "iface_abs_I", "ifaceAbstract", SIG_IFACE_ABS_I, IFACE_ABS_ARG);
        // A default-method name with a descriptor that no overload has -> miss.
        cap_miss(s, "miss_iface_def_str", "ifaceDefault", "(Ljava/lang/String;)Ljava/lang/String;");

        // ============================================================
        //  CONSTRUCTOR (<init>) selection by signature.  LOOKUP-ONLY: resolving
        //  a constructor proxy by its exact descriptor proves <init> overloads
        //  are disambiguated by descriptor like any other name.  We never call()
        //  a ctor on a live instance (re-running <init> is not a valid dispatch).
        // ============================================================
        cap_lookup_only(s, "init_V", "<init>", SIG_INIT_V);
        cap_lookup_only(s, "init_I", "<init>", SIG_INIT_I);
        cap_lookup_only(s, "init_S", "<init>", SIG_INIT_S);
        // wrong <init> descriptors miss (no (J)V ctor; non-void return is invalid)
        cap_miss(s, "miss_init_long", "<init>", "(J)V");
        cap_miss(s, "miss_init_ret",  "<init>", "(I)I");                  // ctors are always ()V-returning

        // ============================================================
        //  NAME-ONLY vs EXPLICIT-SIGNATURE contrast.  get_method("process")
        //  with NO signature latches the FIRST process overload in _methods-array
        //  order (not signature_pinned); the body records WHICH descriptor that
        //  is, and the explicit probes above prove a SPECIFIC (e.g. (II)I or (J)J)
        //  overload is reachable that the name-only path would not have chosen.
        //  Calling the name-only proxy with one int re-resolves to process(I)I via
        //  resolve_compatible_method.
        // ============================================================
        cap_name_only(s, "nameonly_process", "process");
        cap_name_only_call_i(s, "nameonly_process_call_i", "process", PROC_I_ARG);

        // ----------------------------------------------------------------
        //  SNAPSHOT the canonical exactly-once side-effect state HERE, before
        //  the boundary block below re-dispatches several overloads with extreme
        //  args (which would overwrite last-write-wins fields and bump counters).
        //  The body's "exactly-once / canonical value" isolation checks read this
        //  snapshot, so they stay valid regardless of the extra boundary calls.
        // ----------------------------------------------------------------
        {
            put_ival("snap_proc_I_arg",  method_explicit_sig::procIntArg());
            put_ival("snap_proc_II_a",   method_explicit_sig::procIntIntA());
            put_ival("snap_proc_II_b",   method_explicit_sig::procIntIntB());
            put_ival("snap_proc_J",      method_explicit_sig::procLongArg());
            put_sval("snap_proc_str",    method_explicit_sig::procStrArg());
            put_ival("snap_smap_i_hits", method_explicit_sig::smapIntHits());
            put_ival("snap_shape_B",     method_explicit_sig_counters::shapeByteSeen());
            put_ival("snap_shape_S",     method_explicit_sig_counters::shapeShortSeen());
            put_ival("snap_shape_C",     method_explicit_sig_counters::shapeCharSeen());
            put_ival("snap_shape_Z",     method_explicit_sig_counters::shapeBoolSeen());
            put_dval("snap_shape_F",     static_cast<double>(method_explicit_sig_counters::shapeFloatSeen()));
            put_dval("snap_shape_D",     method_explicit_sig_counters::shapeDoubleSeen());
        }

        // ============================================================
        //  BOUNDARY / DEGENERATE ARGUMENT VALUES through the exact-selected
        //  overload.  The selection is descriptor-exact; here we prove the
        //  end-to-end dispatch survives the FULL value range of each primitive
        //  descriptor (min/max/zero/negative/special-float), not just the small
        //  positive constants the family above used.  Each probe re-selects the
        //  SAME overload by the SAME exact signature, just with an extreme arg.
        // ============================================================
        // process(I)I with INT_MIN / INT_MAX (the +1 transform overflows by Java
        // 2's-complement wrap, which is well-defined: INT_MAX+1 == INT_MIN).
        cap_inst_num_i (s, "bnd_proc_I_min",  "process", SIG_PROC_I,  -2147483647 - 1);
        cap_inst_num_i (s, "bnd_proc_I_max",  "process", SIG_PROC_I,  2147483647);
        cap_inst_num_i (s, "bnd_proc_I_zero", "process", SIG_PROC_I,  0);
        cap_inst_num_i (s, "bnd_proc_I_neg",  "process", SIG_PROC_I,  -1);
        // process(J)J with LONG_MIN / LONG_MAX (transform +1000, wraps cleanly).
        cap_inst_num_j (s, "bnd_proc_J_min",  "process", SIG_PROC_J,  (-9223372036854775807LL - 1));
        cap_inst_num_j (s, "bnd_proc_J_max",  "process", SIG_PROC_J,  9223372036854775807LL);
        // process(II)I with negatives and a*100 overflow at the boundary.
        cap_inst_num_ii(s, "bnd_proc_II_neg", "process", SIG_PROC_II, -7, -3);
        // process(String)String with the EMPTY string and with a long string.
        cap_inst_str_s (s, "bnd_proc_S_empty", "process", SIG_PROC_S, std::string{});
        cap_inst_str_s (s, "bnd_proc_S_long",  "process", SIG_PROC_S,
                        std::string{ "abcdefghijklmnopqrstuvwxyz0123456789" });
        // shapes(B)B with BYTE_MIN/MAX (the *2 transform wraps in the byte range).
        cap_inst_byte_b (s, "bnd_shape_B_min", "shapes", SIG_SHAPE_B, static_cast<std::int8_t>(-128));
        cap_inst_byte_b (s, "bnd_shape_B_max", "shapes", SIG_SHAPE_B, static_cast<std::int8_t>(127));
        // shapes(S)S with SHORT_MIN/MAX (the *3 transform wraps in the short range).
        cap_inst_short_s(s, "bnd_shape_S_min", "shapes", SIG_SHAPE_S, static_cast<std::int16_t>(-32768));
        cap_inst_short_s(s, "bnd_shape_S_max", "shapes", SIG_SHAPE_S, static_cast<std::int16_t>(32767));
        // shapes(C)C with the LOW ('\0') and HIGH (0xFFFF) char boundaries; the +1
        // transform wraps 0xFFFF -> 0 (a clean unsigned-16 wrap proof).
        cap_inst_char_c (s, "bnd_shape_C_lo", "shapes", SIG_SHAPE_C, static_cast<char16_t>(0x0000));
        cap_inst_char_c (s, "bnd_shape_C_hi", "shapes", SIG_SHAPE_C, static_cast<char16_t>(0xFFFF));
        // shapes(Z)Z with false (the family above used true; the negation flips it).
        cap_inst_bool_z (s, "bnd_shape_Z_false", "shapes", SIG_SHAPE_Z, false);
        // shapes(F)F with a negative and with zero (the *2 transform preserves sign).
        cap_inst_flt_f  (s, "bnd_shape_F_neg",  "shapes", SIG_SHAPE_F, -3.5f);
        cap_inst_flt_f  (s, "bnd_shape_F_zero", "shapes", SIG_SHAPE_F, 0.0f);
        // shapes(D)D with a large magnitude (the +3.25 transform is exact here).
        cap_inst_dbl_d  (s, "bnd_shape_D_big",  "shapes", SIG_SHAPE_D, 1.0e15);

        // ============================================================
        //  MORE WRONG / MALFORMED descriptors -> MISS.  Widen the strict-compare
        //  coverage beyond the family above: degenerate parens, internal
        //  whitespace, lowercase return token, a bare descriptor with no return,
        //  a doubled parameter list, and cross-name confusion (a descriptor that
        //  is REAL for a DIFFERENT method name must not resolve under this name).
        // ============================================================
        cap_miss(s, "miss_only_open_paren",   "process", "(");                  // just '('
        cap_miss(s, "miss_only_close_paren",  "process", ")");                  // just ')'
        cap_miss(s, "miss_empty_parens",      "process", "()");                 // no return token
        cap_miss(s, "miss_doubled_parens",    "process", "((I))I");             // nested parens
        cap_miss(s, "miss_internal_space",    "process", "(I) I");              // space before ret
        cap_miss(s, "miss_leading_space",     "process", " (I)I");              // leading space
        cap_miss(s, "miss_trailing_space",    "process", "(I)I ");              // trailing space
        cap_miss(s, "miss_lowercase_void",    "process", "()v");                // lowercase 'v'
        cap_miss(s, "miss_no_return_token",   "process", "(I)");                // params, no return
        cap_miss(s, "miss_bad_ref_unterminated", "process",
                 "(Ljava/lang/String)Ljava/lang/String;");                      // missing ';' on arg
        // cross-name confusion: "(I)I" is a REAL descriptor of base/dupInstance,
        // but "trigger" only has "()V"; asking trigger with (I)I must MISS.
        cap_miss(s, "miss_crossname_trigger_I", "trigger", SIG_PROC_I);
        // process has no zero-return-token form; "(I)Z" (bool return) is absent.
        cap_miss(s, "miss_proc_bool_ret",     "process", "(Ljava/lang/String;)Z");
        // combo with an extra trailing int arg (no such overload).
        cap_miss(s, "miss_combo_extra_int",   "combo",
                 "(Ljava/lang/CharSequence;I)Ljava/lang/String;");

        // ============================================================
        //  STATIC-PATH boundary + miss angles (static_method(name,sig)).
        //  The static overload gates on JVM_ACC_STATIC and has NO interface
        //  fallback, so several instance-only lookups must MISS on it.
        // ============================================================
        // smap(I)I with boundary int (static dispatch through null owner).
        cap_stat_num_i ("bnd_smap_I_max", "smap", SIG_SMAP_I, 2147483647);
        cap_stat_num_i ("bnd_smap_I_min", "smap", SIG_SMAP_I, -2147483647 - 1);
        // static return-type twins: smap(I) returns I, NOT J / V -> both MISS.
        cap_miss_static("miss_smap_I_ret_J", "smap", "(I)J");
        cap_miss_static("miss_smap_I_ret_V", "smap", "(I)V");
        // static malformed: no leading paren / trailing junk.
        cap_miss_static("miss_smap_malformed", "smap", "I)I");
        cap_miss_static("miss_smap_trailing",  "smap", "(I)IZ");
        // an INHERITED INSTANCE method (base) via the STATIC path -> MISS
        // (ACC_STATIC gate rejects it; base is not static).
        cap_miss_static("miss_base_via_static", "base", SIG_BASE_I);
        // an INTERFACE DEFAULT method via the STATIC path -> MISS (defaults are
        // non-static instance methods; the static path has NO interface fallback).
        cap_miss_static("miss_ifacedef_via_static", "ifaceDefault", SIG_IFACE_DEF_I);
        // an INSTANCE dupInstance via static -> MISS (already covered above as
        // dup_inst_via_static; here a smap-family wrong NAME on the static path).
        cap_miss_static("miss_static_wrong_name", "smapz", SIG_SMAP_I);
    }
}

VMHOOK_JVM_MODULE(method_explicit_signature)
{
    vmhook::register_class<method_explicit_sig>("vmhook/fixtures/MethodExplicitSig");
    vmhook::register_class<method_explicit_sig_base>("vmhook/fixtures/MethodExplicitSigBase");
    vmhook::register_class<method_explicit_sig_counters>("vmhook/fixtures/MethodExplicitSigCounters");

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
            // ORTHOGONAL latched-Method* proof: the proxy's name() equals the
            // requested name, and it is NOT static (an instance overload).
            ctx.check("proc_I_name_is_process", r.name_text == "process");
            ctx.check("proc_I_not_static", !r.is_static);
            ctx.check("proc_I_not_void", !r.is_void);
            ctx.check("proc_I_returns_arg_plus_1", r.ival == (PROC_I_ARG + 1));
            // side effect: process(I) recorded its arg, the OTHER overloads did not.
            // Read the SNAPSHOT (taken before the boundary block re-dispatched
            // process(I) with extreme args, which is last-write-wins on this field).
            ctx.check("proc_I_side_effect_arg", get("snap_proc_I_arg").ival == PROC_I_ARG);
        }

        // ===================================================================
        //  process(II)I — exact (II)I selection (NOT (I)I)
        // ===================================================================
        {
            const probe_result r{ get("proc_II") };
            ctx.check("proc_II_resolved", r.resolved);
            ctx.check("proc_II_sig_is_exact", r.sig_text == SIG_PROC_II);
            ctx.check("proc_II_returns_a100_plus_b", r.ival == (PROC_II_A * 100 + PROC_II_B));
            ctx.check("proc_II_side_effect_a", get("snap_proc_II_a").ival == PROC_II_A);
            ctx.check("proc_II_side_effect_b", get("snap_proc_II_b").ival == PROC_II_B);
        }

        // ===================================================================
        //  process(J)J — exact (J)J selection (a long, NOT the int overload)
        // ===================================================================
        {
            const probe_result r{ get("proc_J") };
            ctx.check("proc_J_resolved", r.resolved);
            ctx.check("proc_J_sig_is_exact", r.sig_text == SIG_PROC_J);
            ctx.check("proc_J_returns_arg_plus_1000", r.ival == (PROC_J_ARG + 1000));
            ctx.check("proc_J_side_effect", get("snap_proc_J").ival == PROC_J_ARG);
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
            ctx.check("proc_S_side_effect", get("snap_proc_str").sval == "abc");
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
            ctx.check("smap_I_name_is_smap", ri.name_text == "smap");
            // The static overload resolves a STATIC method: is_static() is JVM truth.
            ctx.check("smap_I_is_static", ri.is_static);
            ctx.check("smap_I_returns_double", ri.ival == (SMAP_I_ARG * 2));
            // smapIntHits is a COUNTER; the boundary block calls smap(I) twice more,
            // so read the snapshot (==1) taken before those extra dispatches.
            ctx.check("smap_I_side_effect_once", get("snap_smap_i_hits").ival == 1);

            const probe_result rs{ get("smap_S") };
            ctx.check("smap_S_resolved", rs.resolved);
            ctx.check("smap_S_sig_is_exact", rs.sig_text == SIG_SMAP_S);
            ctx.check("smap_S_name_is_smap", rs.name_text == "smap");
            ctx.check("smap_S_is_static", rs.is_static);
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

            // INHERITED wide-mixed-arg base(JI)J: hierarchy walk picked the
            // multi-slot descriptor, and the two interpreter slots (long+int)
            // packed correctly -> a*1000 + b.
            const probe_result rji{ get("base_JI") };
            ctx.check("base_JI_resolved", rji.resolved);
            ctx.check("base_JI_sig_is_exact", rji.sig_text == SIG_BASE_JI);
            ctx.check("base_JI_name_is_base", rji.name_text == "base");
            ctx.check("base_JI_not_static", !rji.is_static);
            ctx.check("base_JI_returns_packed",
                      rji.ival == (BASE_JI_A * 1000 + BASE_JI_B));
            ctx.check("base_JI_side_effect",
                      method_explicit_sig_base::baseLongIntSeen() == (BASE_JI_A * 1000 + BASE_JI_B));
        }

        // ===================================================================
        //  COVARIANT RETURN: cov() declared CharSequence on the base, overridden
        //  to String on the leaf.  javac normally synthesizes a bridge
        //  cov()Ljava/lang/CharSequence; alongside the real cov()Ljava/lang/String;
        //  so the leaf _methods array holds TWO cov() entries differing ONLY by
        //  the return descriptor.  The exact-signature lookup keys on the FULL
        //  descriptor and selects each independently.
        // ===================================================================
        {
            const probe_result cstr{ get("cov_str") };
            ctx.check("cov_str_resolved", cstr.resolved);
            ctx.check("cov_str_sig_is_string_ret", cstr.sig_text == SIG_COV_STR);
            ctx.check("cov_str_name_is_cov", cstr.name_text == "cov");
            ctx.check("cov_str_is_string", cstr.is_string);
            // The leaf override ran (returns "leaf-cov"); the un-overridden base
            // body NEVER ran (covBaseSeen stays 0) — virtual dispatch reached the
            // leaf, not the supertype declaration.
            ctx.check("cov_str_returns_leaf", cstr.sval == "leaf-cov");
            ctx.check("cov_str_side_effect", method_explicit_sig::covStrSeen() == "leaf-cov");
            ctx.check("cov_base_body_never_ran", method_explicit_sig_base::covBaseSeen() == 0);

            // The synthetic bridge cov()Ljava/lang/CharSequence; — LOOKUP-ONLY.
            // Its presence is JDK/javac-dependent (a bridge is emitted for the
            // covariant override on every mainstream javac, but we do not red the
            // matrix if a given build elides it): PASS when found, [INFO] otherwise.
            const probe_result cbr{ get("cov_bridge") };
            if (cbr.resolved)
            {
                ctx.check("cov_bridge_sig_is_charsequence_ret", cbr.sig_text == SIG_COV_CS);
                ctx.check("cov_bridge_name_is_cov", cbr.name_text == "cov");
                // The two cov() proxies differ ONLY by return descriptor.
                ctx.check("cov_two_proxies_differ_only_by_return",
                          cbr.sig_text != cstr.sig_text);
                ctx.record("[INFO] covariant cov(): leaf holds BOTH the real override "
                           + cstr.sig_text + " and the synthetic bridge " + cbr.sig_text
                           + " — exact-signature lookup selected each by return descriptor.");
            }
            else
            {
                ctx.record("[INFO] covariant cov(): synthetic bridge ()Ljava/lang/"
                           "CharSequence; not present on this build (javac/JDK variant); "
                           "only the real override ()Ljava/lang/String; resolved.");
            }
            // A cov() descriptor that does not exist for either entry misses.
            ctx.check("miss_cov_object_ret_nullopt", !get("miss_cov_object_ret").resolved);
        }

        // ===================================================================
        //  WIDE-MIXED-ARG family (wide): multi-slot descriptors (long/double take
        //  two interpreter slots).  Each exact descriptor selects its overload and
        //  the dispatch packs the interleaved slots correctly.
        // ===================================================================
        {
            const probe_result wji{ get("wide_JI") };
            ctx.check("wide_JI_resolved", wji.resolved);
            ctx.check("wide_JI_sig_is_exact", wji.sig_text == SIG_WIDE_JI);
            ctx.check("wide_JI_name_is_wide", wji.name_text == "wide");
            ctx.check("wide_JI_returns_packed",
                      wji.ival == (WIDE_JI_J_ARG * 1000 + WIDE_JI_I_ARG));
            ctx.check("wide_JI_side_effect",
                      method_explicit_sig::wideJiSeen() == (WIDE_JI_J_ARG * 1000 + WIDE_JI_I_ARG));

            const probe_result wdi{ get("wide_DI") };
            ctx.check("wide_DI_resolved", wdi.resolved);
            ctx.check("wide_DI_sig_is_exact", wdi.sig_text == SIG_WIDE_DI);
            ctx.check("wide_DI_returns_sum",
                      wdi.dval == (WIDE_DI_D_ARG + static_cast<double>(WIDE_DI_I_ARG)));
            ctx.check("wide_DI_side_effect",
                      method_explicit_sig::wideDiSeen()
                          == (WIDE_DI_D_ARG + static_cast<double>(WIDE_DI_I_ARG)));

            const probe_result wjj{ get("wide_JJ") };
            ctx.check("wide_JJ_resolved", wjj.resolved);
            ctx.check("wide_JJ_sig_is_exact", wjj.sig_text == SIG_WIDE_JJ);
            ctx.check("wide_JJ_returns_diff", wjj.ival == (WIDE_JJ_A_ARG - WIDE_JJ_B_ARG));
            ctx.check("wide_JJ_side_effect",
                      method_explicit_sig::wideJjSeen() == (WIDE_JJ_A_ARG - WIDE_JJ_B_ARG));

            // The three wide proxies carry three distinct descriptors.
            ctx.check("wide_three_distinct",
                      wji.sig_text != wdi.sig_text && wdi.sig_text != wjj.sig_text
                          && wji.sig_text != wjj.sig_text);

            // wrong / swapped-slot-order / wrong-return wide descriptors MISS.
            ctx.check("miss_wide_ij_nullopt", !get("miss_wide_ij").resolved);
            ctx.check("miss_wide_id_nullopt", !get("miss_wide_id").resolved);
            ctx.check("miss_wide_ji_wrong_ret_nullopt", !get("miss_wide_ji_wrong_ret").resolved);
            ctx.record("[INFO] wide-mixed-arg: (JI)J / (DI)D / (JJ)J each selected by exact "
                       "descriptor and dispatched with correct multi-slot packing; the "
                       "slot-order-swapped (IJ)J / (ID)D and wrong-return (JI)I all MISS.");
        }

        // ===================================================================
        //  VOID-WITH-ARGS: sink(II)V — the 'V' return descriptor selected with a
        //  NON-empty parameter list (distinct from process()V).
        // ===================================================================
        {
            const probe_result sk{ get("sink_IIV") };
            ctx.check("sink_IIV_resolved", sk.resolved);
            ctx.check("sink_IIV_sig_is_exact", sk.sig_text == SIG_SINK_IIV);
            ctx.check("sink_IIV_name_is_sink", sk.name_text == "sink");
            ctx.check("sink_IIV_is_void", sk.is_void);
            ctx.check("sink_IIV_is_not_string", !sk.is_string);
            ctx.check("sink_IIV_side_effect",
                      method_explicit_sig::sinkIiSeen() == (SINK_II_A * 1000 + SINK_II_B));
            // wrong-return / wrong-arity sink descriptors miss.
            ctx.check("miss_sink_ret_nullopt",   !get("miss_sink_ret").resolved);
            ctx.check("miss_sink_arity_nullopt", !get("miss_sink_arity").resolved);
        }

        // ===================================================================
        //  OBJECT-RETURN descriptors (non-String): makeObj()Object, makeNum()Number
        //  selected by exact descriptor (LOOKUP-ONLY).  A wrong object-return
        //  descriptor misses — the reference return type is part of the descriptor
        //  just like a primitive one.
        // ===================================================================
        {
            const probe_result mo{ get("make_obj") };
            ctx.check("make_obj_resolved", mo.resolved);
            ctx.check("make_obj_sig_is_object_ret", mo.sig_text == SIG_MAKEOBJ);
            ctx.check("make_obj_name", mo.name_text == "makeObj");
            const probe_result mn{ get("make_num") };
            ctx.check("make_num_resolved", mn.resolved);
            ctx.check("make_num_sig_is_number_ret", mn.sig_text == SIG_MAKENUM);
            ctx.check("make_num_name", mn.name_text == "makeNum");
            // The two object-return descriptors differ.
            ctx.check("obj_returns_distinct", mo.sig_text != mn.sig_text);
            // Wrong object-return descriptors miss.
            ctx.check("miss_makeobj_as_string_nullopt", !get("miss_makeobj_as_string").resolved);
            ctx.check("miss_makenum_as_object_nullopt", !get("miss_makenum_as_object").resolved);
            ctx.record("[INFO] object-return descriptor is load-bearing: makeObj()Object and "
                       "makeNum()Number resolve, but makeObj()String / makeNum()Object MISS.");
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
            // 999999; the SNAPSHOT (taken after the canonical family, before any
            // boundary re-dispatch) therefore still reads PROC_I_ARG — proving the
            // miss added no dispatch.  (The live field is later overwritten by the
            // boundary block, which is a separate, intentional dispatch.)
            ctx.check("noop_did_not_dispatch_process_I",
                      get("snap_proc_I_arg").ival == PROC_I_ARG);
        }

        // ===================================================================
        //  DESCRIPTOR-SHAPE SELECTORS: every primitive descriptor (F D Z B S C)
        //  resolved by its exact descriptor, dispatched the right body, returned
        //  the right value, and fired its own side effect.  Arrays lookup-only.
        // ===================================================================
        {
            const probe_result f{ get("shape_F") };
            ctx.check("shape_F_resolved", f.resolved);
            ctx.check("shape_F_sig_is_exact", f.sig_text == SIG_SHAPE_F);
            ctx.check("shape_F_not_void", !f.is_void);
            ctx.check("shape_F_returns_double_arg", f.dval == static_cast<double>(SHAPE_F_ARG * 2.0f));
            // Side effects are last-write-wins fields; the boundary block re-dispatches
            // these shapes with extreme args, so read the canonical SNAPSHOT.
            ctx.check("shape_F_side_effect",
                      get("snap_shape_F").dval == static_cast<double>(SHAPE_F_ARG));

            const probe_result d{ get("shape_D") };
            ctx.check("shape_D_resolved", d.resolved);
            ctx.check("shape_D_sig_is_exact", d.sig_text == SIG_SHAPE_D);
            ctx.check("shape_D_returns_arg_plus", d.dval == (SHAPE_D_ARG + 3.25));
            ctx.check("shape_D_side_effect", get("snap_shape_D").dval == SHAPE_D_ARG);

            const probe_result z{ get("shape_Z") };
            ctx.check("shape_Z_resolved", z.resolved);
            ctx.check("shape_Z_sig_is_exact", z.sig_text == SIG_SHAPE_Z);
            ctx.check("shape_Z_returns_negation", z.ival == 0);   // !true == false == 0
            ctx.check("shape_Z_side_effect", get("snap_shape_Z").ival == 1);

            const probe_result b{ get("shape_B") };
            ctx.check("shape_B_resolved", b.resolved);
            ctx.check("shape_B_sig_is_exact", b.sig_text == SIG_SHAPE_B);
            ctx.check("shape_B_returns_double_arg", b.ival == (SHAPE_B_ARG * 2));
            ctx.check("shape_B_side_effect", get("snap_shape_B").ival == SHAPE_B_ARG);

            const probe_result sh{ get("shape_S") };
            ctx.check("shape_S_resolved", sh.resolved);
            ctx.check("shape_S_sig_is_exact", sh.sig_text == SIG_SHAPE_S);
            ctx.check("shape_S_returns_triple_arg", sh.ival == (SHAPE_S_ARG * 3));
            ctx.check("shape_S_side_effect", get("snap_shape_S").ival == SHAPE_S_ARG);

            const probe_result c{ get("shape_C") };
            ctx.check("shape_C_resolved", c.resolved);
            ctx.check("shape_C_sig_is_exact", c.sig_text == SIG_SHAPE_C);
            ctx.check("shape_C_returns_arg_plus_1",
                      c.ival == static_cast<std::int64_t>(static_cast<std::uint16_t>(SHAPE_C_ARG) + 1));
            ctx.check("shape_C_side_effect",
                      get("snap_shape_C").ival
                          == static_cast<std::int64_t>(static_cast<std::uint16_t>(SHAPE_C_ARG)));

            const probe_result q{ get("shape_4I") };
            ctx.check("shape_4I_resolved", q.resolved);
            ctx.check("shape_4I_sig_is_exact", q.sig_text == SIG_SHAPE_4I);
            ctx.check("shape_4I_returns_sum", q.ival == (SHAPE_4A + SHAPE_4B + SHAPE_4C + SHAPE_4D));
            ctx.check("shape_4I_side_effect",
                      method_explicit_sig_counters::shapeFourArgSeen()
                          == (SHAPE_4A + SHAPE_4B + SHAPE_4C + SHAPE_4D));

            // Array shapes: LOOKUP-ONLY (resolve + exact descriptor; no dispatch
            // because C++ array args have no JNI marshalling).  Proves the
            // descriptor compare handles '[' array tokens and the embedded 'L...;'.
            const probe_result ia{ get("shape_IARR") };
            ctx.check("shape_IARR_resolved", ia.resolved);
            ctx.check("shape_IARR_sig_is_exact", ia.sig_text == SIG_SHAPE_IARR);
            const probe_result ja{ get("shape_JARR") };
            ctx.check("shape_JARR_resolved", ja.resolved);
            ctx.check("shape_JARR_sig_is_exact", ja.sig_text == SIG_SHAPE_JARR);
            const probe_result sa{ get("shape_SARR") };
            ctx.check("shape_SARR_resolved", sa.resolved);
            ctx.check("shape_SARR_sig_is_exact", sa.sig_text == SIG_SHAPE_SARR);

            // Array shapes never dispatched (lookup-only), so their side effects
            // stayed zero.
            ctx.check("shape_arr_no_dispatch_iarr",
                      method_explicit_sig_counters::shapeIntArrSeen() == 0);
            ctx.check("shape_arr_no_dispatch_jarr",
                      method_explicit_sig_counters::shapeLongArrSeen() == 0);
            ctx.check("shape_arr_no_dispatch_sarr",
                      method_explicit_sig_counters::shapeStrArrSeen() == 0);

            // Wrong array descriptors miss.
            ctx.check("miss_shape_carr_nullopt", !get("miss_shape_carr").resolved);
            ctx.check("miss_shape_2d_nullopt",   !get("miss_shape_2d").resolved);
            ctx.check("miss_shape_iarr_wrong_ret_nullopt", !get("miss_shape_iarr_wrong_ret").resolved);
        }

        // ===================================================================
        //  RETURN TYPE IS PART OF THE DESCRIPTOR.  The exact compare matches the
        //  WHOLE descriptor including the return char; same-params/wrong-return
        //  always misses (Java forbids return-type-only overloads, so this can
        //  never alias a sibling — it isolates the return char's contribution).
        // ===================================================================
        ctx.check("ret_shape_F_wrong_V_nullopt", !get("ret_shape_F_wrong_V").resolved);
        ctx.check("ret_shape_F_wrong_D_nullopt", !get("ret_shape_F_wrong_D").resolved);
        ctx.check("ret_shape_D_wrong_F_nullopt", !get("ret_shape_D_wrong_F").resolved);
        ctx.check("ret_shape_Z_wrong_I_nullopt", !get("ret_shape_Z_wrong_I").resolved);
        ctx.record("[INFO] return type is part of the JVM descriptor: shapes(F)F "
                   "resolves but shapes(F)V / (F)D / and shapes(D)F all MISS — the "
                   "compare is over the FULL descriptor, return char included.");

        // ===================================================================
        //  ACC_STATIC ORTHOGONALITY (dup* family): same descriptor (I)I, the
        //  resolution KIND (static vs instance overload) decides which is found.
        // ===================================================================
        {
            // static_method picks the STATIC dupStatic (null owner; dispatches).
            const probe_result ds{ get("dup_stat_via_static") };
            ctx.check("dup_stat_via_static_resolved", ds.resolved);
            ctx.check("dup_stat_via_static_sig_is_exact", ds.sig_text == SIG_DUP_I);
            ctx.check("dup_stat_via_static_name", ds.name_text == "dupStatic");
            ctx.check("dup_stat_via_static_is_static", ds.is_static);
            ctx.check("dup_stat_via_static_returns_double", ds.ival == (DUP_STAT_ARG * 2));
            ctx.check("dup_stat_via_static_side_effect",
                      method_explicit_sig_counters::dupStaticSeen() == DUP_STAT_ARG);

            // static_method REJECTS the instance-only dupInstance (ACC_STATIC gate).
            ctx.check("dup_inst_via_static_rejected", !get("dup_inst_via_static").resolved);

            // get_method (instance) picks the INSTANCE dupInstance (real receiver).
            const probe_result di{ get("dup_inst_via_inst") };
            ctx.check("dup_inst_via_inst_resolved", di.resolved);
            ctx.check("dup_inst_via_inst_sig_is_exact", di.sig_text == SIG_DUP_I);
            ctx.check("dup_inst_via_inst_name", di.name_text == "dupInstance");
            ctx.check("dup_inst_via_inst_not_static", !di.is_static);
            ctx.check("dup_inst_via_inst_returns_arg_plus_1", di.ival == (DUP_INST_ARG + 1));
            ctx.check("dup_inst_via_inst_side_effect",
                      method_explicit_sig_counters::dupInstanceSeen() == DUP_INST_ARG);

            // get_method (instance) ALSO resolves the STATIC dupStatic — the
            // instance overload has NO ACC_STATIC gate (FLAW 1).  We assert only
            // that the LOOKUP succeeds with the exact descriptor (characterizing
            // the flaw); we did NOT call() it (a phantom receiver would be pushed).
            const probe_result dsi{ get("dup_stat_via_inst") };
            ctx.check("dup_stat_via_inst_resolves_flaw1", dsi.resolved);
            ctx.check("dup_stat_via_inst_sig_is_exact", dsi.sig_text == SIG_DUP_I);
            ctx.check("dup_stat_via_inst_name", dsi.name_text == "dupStatic");
            // is_static() reads JVM_ACC_STATIC off the Method directly: even though
            // the INSTANCE overload handed back a proxy carrying a phantom receiver
            // (FLAW 1), the underlying Method IS static, so is_static() reports true.
            // This is the crisp JVM-truth witness of the flaw (we still do not call
            // the proxy — a static dispatch through a non-null owner misbehaves).
            ctx.check("dup_stat_via_inst_is_static_truth", dsi.is_static);
            ctx.record("[INFO] ACC_STATIC orthogonality: static_method() filters on "
                       "JVM_ACC_STATIC (dupInstance rejected), but the INSTANCE "
                       "get_method(name,sig) does NOT (dupStatic still resolves with a "
                       "phantom receiver) — documented FLAW 1, lookup-only here.");
        }

        // ===================================================================
        //  INTERFACE DEFAULT method by signature — reached ONLY via the
        //  find_interface_default_method second-chance fallback (the superclass
        //  walk can't see an interface-declared default).  Exact descriptor picks
        //  between the two ifaceDefault overloads.
        // ===================================================================
        {
            const probe_result di{ get("iface_def_I") };
            ctx.check("iface_def_I_resolved", di.resolved);
            ctx.check("iface_def_I_sig_is_exact", di.sig_text == SIG_IFACE_DEF_I);
            ctx.check("iface_def_I_returns_arg_plus_3", di.ival == (IFACE_DEF_I_ARG + 3));
            ctx.check("iface_def_I_side_effect",
                      method_explicit_sig_counters::ifaceDefaultIntSeen() == IFACE_DEF_I_ARG);

            const probe_result dj{ get("iface_def_J") };
            ctx.check("iface_def_J_resolved", dj.resolved);
            ctx.check("iface_def_J_sig_is_exact", dj.sig_text == SIG_IFACE_DEF_J);
            ctx.check("iface_def_J_returns_arg_plus_30", dj.ival == (IFACE_DEF_J_ARG + 30));
            ctx.check("iface_def_J_side_effect",
                      method_explicit_sig_counters::ifaceDefaultLongSeen() == IFACE_DEF_J_ARG);

            // The abstract interface method has a concrete class override; the
            // ordinary walk finds + dispatches it (the fallback skips abstracts).
            const probe_result ab{ get("iface_abs_I") };
            ctx.check("iface_abs_I_resolved", ab.resolved);
            ctx.check("iface_abs_I_sig_is_exact", ab.sig_text == SIG_IFACE_ABS_I);
            ctx.check("iface_abs_I_returns_arg_plus_1", ab.ival == (IFACE_ABS_ARG + 1));
            ctx.check("iface_abs_I_side_effect",
                      method_explicit_sig_counters::ifaceAbstractSeen() == IFACE_ABS_ARG);

            // A nonexistent ifaceDefault descriptor misses (interface walk too).
            ctx.check("miss_iface_def_str_nullopt", !get("miss_iface_def_str").resolved);
        }

        // ===================================================================
        //  CONSTRUCTOR (<init>) selection by signature (lookup-only).  Each
        //  <init> overload is selected by its exact descriptor; wrong ctor
        //  descriptors miss.  We never dispatch a ctor on a live instance.
        // ===================================================================
        {
            const probe_result iv{ get("init_V") };
            ctx.check("init_V_resolved", iv.resolved);
            ctx.check("init_V_sig_is_exact", iv.sig_text == SIG_INIT_V);
            const probe_result ii{ get("init_I") };
            ctx.check("init_I_resolved", ii.resolved);
            ctx.check("init_I_sig_is_exact", ii.sig_text == SIG_INIT_I);
            const probe_result is{ get("init_S") };
            ctx.check("init_S_resolved", is.resolved);
            ctx.check("init_S_sig_is_exact", is.sig_text == SIG_INIT_S);
            // The three <init> proxies carry THREE distinct descriptors.
            ctx.check("init_three_distinct",
                      iv.sig_text != ii.sig_text && ii.sig_text != is.sig_text
                          && iv.sig_text != is.sig_text);
            // Wrong ctor descriptors miss.
            ctx.check("miss_init_long_nullopt", !get("miss_init_long").resolved);
            ctx.check("miss_init_ret_nullopt",  !get("miss_init_ret").resolved);
            // Lookup-only: no constructor was re-run, so initIntSeen/initStrSeen
            // stay at whatever the probe's own `new MethodExplicitSig()` left
            // (the no-arg ctor writes neither), i.e. zero / empty.
            ctx.check("init_lookup_only_no_int_ctor_dispatch",
                      method_explicit_sig_counters::initIntSeen() == 0);
            ctx.check("init_lookup_only_no_str_ctor_dispatch",
                      method_explicit_sig_counters::initStrSeen().empty());
        }

        // ===================================================================
        //  NAME-ONLY vs EXPLICIT-SIGNATURE contrast.  get_method("process") with
        //  NO signature latches the FIRST overload in _methods-array order; the
        //  explicit-signature probes above proved a SPECIFIC overload (e.g. (J)J,
        //  (II)I) is reachable by descriptor regardless of declaration order.
        // ===================================================================
        {
            const probe_result no{ get("nameonly_process") };
            ctx.check("nameonly_process_resolved", no.resolved);
            // It must be SOME real process descriptor (we don't pin which order
            // the JVM lays the _methods array out — that's JDK/impl dependent —
            // so assert it is one of the six legitimate process descriptors).
            const bool is_a_real_process_overload{
                no.sig_text == SIG_PROC_I  || no.sig_text == SIG_PROC_II ||
                no.sig_text == SIG_PROC_J  || no.sig_text == SIG_PROC_S  ||
                no.sig_text == SIG_PROC_SI || no.sig_text == SIG_PROC_V };
            // PASS-or-[INFO]: the latched descriptor is whichever process() is FIRST
            // in the _methods array, which is JDK/impl-order dependent.  It is normally
            // one of the six known overloads (so PASS), but a JDK that surfaces an
            // unexpected (e.g. synthetic/inherited) descriptor first must NOT red the
            // matrix -> characterize it instead of hard-failing.
            if (is_a_real_process_overload)
                ctx.check("nameonly_process_is_some_real_overload", true);
            else
                ctx.record("[INFO] name-only get_method(\"process\") latched UNEXPECTED descriptor "
                           + no.sig_text + " (JDK _methods-order variant; not one of the six).");
            ctx.record("[INFO] name-only get_method(\"process\") latched descriptor "
                       + no.sig_text + " (first in _methods order); the explicit-"
                       "signature path reaches ANY of the six overloads by descriptor.");

            // Name-only proxy is NOT pinned, so call(int) re-resolves to process(I)I
            // via resolve_compatible_method and dispatches it, returning arg+1.
            const probe_result noc{ get("nameonly_process_call_i") };
            ctx.check("nameonly_process_call_i_resolved", noc.resolved);
            ctx.check("nameonly_process_call_i_dispatched_int_overload",
                      noc.ival == (PROC_I_ARG + 1));
        }

        // ===================================================================
        //  BOUNDARY / DEGENERATE ARGUMENT VALUES.  The exact-descriptor selection
        //  is unchanged; these prove the END-TO-END dispatch of the selected
        //  overload survives the full value range (min/max/zero/negative/special),
        //  with the Java side's transform applied (2's-complement / IEEE wrap as
        //  appropriate).  Each still reasserts the exact signature was selected.
        // ===================================================================
        {
            // process(I)I:  +1 with 2's-complement wrap.
            const probe_result imin{ get("bnd_proc_I_min") };
            ctx.check("bnd_proc_I_min_resolved", imin.resolved);
            ctx.check("bnd_proc_I_min_sig", imin.sig_text == SIG_PROC_I);
            // INT_MIN + 1 == -2147483647
            ctx.check("bnd_proc_I_min_wraps", imin.ival == (-2147483647));
            const probe_result imax{ get("bnd_proc_I_max") };
            ctx.check("bnd_proc_I_max_resolved", imax.resolved);
            ctx.check("bnd_proc_I_max_sig", imax.sig_text == SIG_PROC_I);
            // INT_MAX + 1 wraps to INT_MIN.
            ctx.check("bnd_proc_I_max_wraps", imax.ival == (-2147483647 - 1));
            const probe_result izero{ get("bnd_proc_I_zero") };
            ctx.check("bnd_proc_I_zero_resolved", izero.resolved);
            ctx.check("bnd_proc_I_zero_result", izero.ival == 1);
            const probe_result ineg{ get("bnd_proc_I_neg") };
            ctx.check("bnd_proc_I_neg_resolved", ineg.resolved);
            ctx.check("bnd_proc_I_neg_result", ineg.ival == 0);   // -1 + 1 == 0

            // process(J)J:  +1000 with 2's-complement wrap.
            const probe_result jmin{ get("bnd_proc_J_min") };
            ctx.check("bnd_proc_J_min_resolved", jmin.resolved);
            ctx.check("bnd_proc_J_min_sig", jmin.sig_text == SIG_PROC_J);
            // LONG_MIN + 1000
            ctx.check("bnd_proc_J_min_result",
                      jmin.ival == ((-9223372036854775807LL - 1) + 1000LL));
            const probe_result jmax{ get("bnd_proc_J_max") };
            ctx.check("bnd_proc_J_max_resolved", jmax.resolved);
            ctx.check("bnd_proc_J_max_sig", jmax.sig_text == SIG_PROC_J);
            // LONG_MAX + 1000 wraps around.
            ctx.check("bnd_proc_J_max_wraps",
                      jmax.ival == static_cast<std::int64_t>(
                          static_cast<std::uint64_t>(9223372036854775807LL) + 1000ULL));

            // process(II)I:  a*100 + b with negatives.
            const probe_result iineg{ get("bnd_proc_II_neg") };
            ctx.check("bnd_proc_II_neg_resolved", iineg.resolved);
            ctx.check("bnd_proc_II_neg_sig", iineg.sig_text == SIG_PROC_II);
            ctx.check("bnd_proc_II_neg_result", iineg.ival == ((-7) * 100 + (-3)));

            // process(String)String:  empty + long string round-trip ("S:" prefix).
            const probe_result sempty{ get("bnd_proc_S_empty") };
            ctx.check("bnd_proc_S_empty_resolved", sempty.resolved);
            ctx.check("bnd_proc_S_empty_sig", sempty.sig_text == SIG_PROC_S);
            ctx.check("bnd_proc_S_empty_is_string", sempty.is_string);
            ctx.check("bnd_proc_S_empty_result", sempty.sval == "S:");
            const probe_result slong{ get("bnd_proc_S_long") };
            ctx.check("bnd_proc_S_long_resolved", slong.resolved);
            ctx.check("bnd_proc_S_long_result",
                      slong.sval == "S:abcdefghijklmnopqrstuvwxyz0123456789");

            // shapes(B)B:  *2 with byte wrap.  -128*2 == -256 -> byte 0;
            // 127*2 == 254 -> byte -2.  value_t carries the SIGN-EXTENDED byte.
            const probe_result bmin{ get("bnd_shape_B_min") };
            ctx.check("bnd_shape_B_min_resolved", bmin.resolved);
            ctx.check("bnd_shape_B_min_sig", bmin.sig_text == SIG_SHAPE_B);
            ctx.check("bnd_shape_B_min_wraps",
                      bmin.ival == static_cast<std::int64_t>(static_cast<std::int8_t>(-128 * 2)));
            const probe_result bmax{ get("bnd_shape_B_max") };
            ctx.check("bnd_shape_B_max_resolved", bmax.resolved);
            ctx.check("bnd_shape_B_max_wraps",
                      bmax.ival == static_cast<std::int64_t>(static_cast<std::int8_t>(127 * 2)));

            // shapes(S)S:  *3 with short wrap.
            const probe_result shmin{ get("bnd_shape_S_min") };
            ctx.check("bnd_shape_S_min_resolved", shmin.resolved);
            ctx.check("bnd_shape_S_min_sig", shmin.sig_text == SIG_SHAPE_S);
            ctx.check("bnd_shape_S_min_wraps",
                      shmin.ival == static_cast<std::int64_t>(static_cast<std::int16_t>(-32768 * 3)));
            const probe_result shmax{ get("bnd_shape_S_max") };
            ctx.check("bnd_shape_S_max_resolved", shmax.resolved);
            ctx.check("bnd_shape_S_max_wraps",
                      shmax.ival == static_cast<std::int64_t>(static_cast<std::int16_t>(32767 * 3)));

            // shapes(C)C:  +1 with unsigned-16 wrap.  '\0'+1 == 1; 0xFFFF+1 -> 0.
            const probe_result clo{ get("bnd_shape_C_lo") };
            ctx.check("bnd_shape_C_lo_resolved", clo.resolved);
            ctx.check("bnd_shape_C_lo_sig", clo.sig_text == SIG_SHAPE_C);
            ctx.check("bnd_shape_C_lo_result", clo.ival == 1);
            const probe_result chi{ get("bnd_shape_C_hi") };
            ctx.check("bnd_shape_C_hi_resolved", chi.resolved);
            ctx.check("bnd_shape_C_hi_wraps", chi.ival == 0);   // (char)(0xFFFF+1) == 0

            // shapes(Z)Z:  negation of false is true (1).
            const probe_result zf{ get("bnd_shape_Z_false") };
            ctx.check("bnd_shape_Z_false_resolved", zf.resolved);
            ctx.check("bnd_shape_Z_false_sig", zf.sig_text == SIG_SHAPE_Z);
            ctx.check("bnd_shape_Z_false_returns_true", zf.ival == 1);

            // shapes(F)F:  *2 preserves sign; -3.5*2 == -7.0; 0*2 == 0.
            const probe_result fneg{ get("bnd_shape_F_neg") };
            ctx.check("bnd_shape_F_neg_resolved", fneg.resolved);
            ctx.check("bnd_shape_F_neg_sig", fneg.sig_text == SIG_SHAPE_F);
            ctx.check("bnd_shape_F_neg_result", fneg.dval == static_cast<double>(-3.5f * 2.0f));
            const probe_result fz{ get("bnd_shape_F_zero") };
            ctx.check("bnd_shape_F_zero_resolved", fz.resolved);
            ctx.check("bnd_shape_F_zero_result", fz.dval == 0.0);

            // shapes(D)D:  +3.25 at large magnitude (exact in double).
            const probe_result dbig{ get("bnd_shape_D_big") };
            ctx.check("bnd_shape_D_big_resolved", dbig.resolved);
            ctx.check("bnd_shape_D_big_sig", dbig.sig_text == SIG_SHAPE_D);
            ctx.check("bnd_shape_D_big_result", dbig.dval == (1.0e15 + 3.25));
        }

        // ===================================================================
        //  WIDER MALFORMED / DEGENERATE descriptors -> nullopt.  Strict compare
        //  rejects every non-exact form (degenerate parens, whitespace, wrong-
        //  case return, missing return token, unterminated ref, cross-name).
        // ===================================================================
        ctx.check("miss_only_open_paren_nullopt",   !get("miss_only_open_paren").resolved);
        ctx.check("miss_only_close_paren_nullopt",  !get("miss_only_close_paren").resolved);
        ctx.check("miss_empty_parens_nullopt",      !get("miss_empty_parens").resolved);
        ctx.check("miss_doubled_parens_nullopt",    !get("miss_doubled_parens").resolved);
        ctx.check("miss_internal_space_nullopt",    !get("miss_internal_space").resolved);
        ctx.check("miss_leading_space_nullopt",     !get("miss_leading_space").resolved);
        ctx.check("miss_trailing_space_nullopt",    !get("miss_trailing_space").resolved);
        ctx.check("miss_lowercase_void_nullopt",    !get("miss_lowercase_void").resolved);
        ctx.check("miss_no_return_token_nullopt",   !get("miss_no_return_token").resolved);
        ctx.check("miss_bad_ref_unterminated_nullopt", !get("miss_bad_ref_unterminated").resolved);
        ctx.check("miss_crossname_trigger_I_nullopt", !get("miss_crossname_trigger_I").resolved);
        ctx.check("miss_proc_bool_ret_nullopt",     !get("miss_proc_bool_ret").resolved);
        ctx.check("miss_combo_extra_int_nullopt",   !get("miss_combo_extra_int").resolved);

        // ===================================================================
        //  STATIC-PATH boundary + miss angles.  static_method(name,sig) gates on
        //  JVM_ACC_STATIC and has NO interface-default fallback.
        // ===================================================================
        {
            const probe_result smax{ get("bnd_smap_I_max") };
            ctx.check("bnd_smap_I_max_resolved", smax.resolved);
            ctx.check("bnd_smap_I_max_sig", smax.sig_text == SIG_SMAP_I);
            ctx.check("bnd_smap_I_max_is_static", smax.is_static);
            // INT_MAX * 2 wraps to -2 (32-bit 2's complement).  Compute the wrap
            // in UNSIGNED 32-bit (well-defined modular arithmetic) then reinterpret
            // as int32 — a plain `2147483647 * 2` would be compile-time int overflow.
            ctx.check("bnd_smap_I_max_wraps",
                      smax.ival == static_cast<std::int64_t>(
                          static_cast<std::int32_t>(
                              static_cast<std::uint32_t>(2147483647) * 2u)));
            const probe_result smin{ get("bnd_smap_I_min") };
            ctx.check("bnd_smap_I_min_resolved", smin.resolved);
            ctx.check("bnd_smap_I_min_wraps",
                      smin.ival == static_cast<std::int64_t>(
                          static_cast<std::int32_t>(
                              static_cast<std::uint32_t>(-2147483647 - 1) * 2u)));

            // static return-type twins miss (return char is load-bearing here too).
            ctx.check("miss_smap_I_ret_J_nullopt", !get("miss_smap_I_ret_J").resolved);
            ctx.check("miss_smap_I_ret_V_nullopt", !get("miss_smap_I_ret_V").resolved);
            // static malformed misses.
            ctx.check("miss_smap_malformed_nullopt", !get("miss_smap_malformed").resolved);
            ctx.check("miss_smap_trailing_nullopt",  !get("miss_smap_trailing").resolved);
            // instance-only / inherited-instance / interface-default names rejected
            // on the static path.
            ctx.check("miss_base_via_static_nullopt",     !get("miss_base_via_static").resolved);
            ctx.check("miss_ifacedef_via_static_nullopt", !get("miss_ifacedef_via_static").resolved);
            ctx.check("miss_static_wrong_name_nullopt",   !get("miss_static_wrong_name").resolved);
            ctx.record("[INFO] static_method(name,sig): ACC_STATIC gate + no interface "
                       "fallback — base(I)I (inherited instance) and ifaceDefault(I)I "
                       "(interface default) both correctly MISS on the static path.");
        }

        // ===================================================================
        //  GLOBAL ISOLATION INVARIANTS across the whole run:
        //  every overload's side effect fired EXACTLY the expected number of
        //  times — no wrong-signature miss leaked into a real dispatch, and no
        //  exact selection picked a sibling.
        //
        //  For overloads the boundary block intentionally RE-DISPATCHED with
        //  extreme args (process I/II/J/String, smap(I), shapes F/D/Z/B/S), the
        //  canonical exactly-once value is read from the SNAPSHOT taken after the
        //  family and before the boundary block; the live field afterwards holds
        //  the last boundary value (a separate intentional dispatch, not a leak).
        //  Overloads NOT re-dispatched are read live and must be EXACTLY once.
        // ===================================================================
        // process(II) ran once (proc_II); process(I) ran once (proc_I).  Their
        // recorded canonical args (snapshot) are still the legitimate ones.
        ctx.check("isolation_proc_I_arg_intact", get("snap_proc_I_arg").ival == PROC_I_ARG);
        ctx.check("isolation_proc_II_a_intact",  get("snap_proc_II_a").ival == PROC_II_A);
        ctx.check("isolation_proc_II_b_intact",  get("snap_proc_II_b").ival == PROC_II_B);
        ctx.check("isolation_proc_J_intact",     get("snap_proc_J").ival == PROC_J_ARG);
        // process()V was NEVER re-dispatched, so its live counter must be exactly 1.
        ctx.check("isolation_proc_V_one_hit",    method_explicit_sig::procVoidHits() == 1);
        // combo (FIXED #5): the explicit signature is now PINNED, so each probe
        // dispatched ITS overload exactly once — combo(CharSequence) once and
        // combo(String) once (total two dispatches).  Not re-dispatched: live read.
        ctx.check("isolation_combo_cs_one", method_explicit_sig::comboCsHits() == 1);
        ctx.check("isolation_combo_st_one", method_explicit_sig::comboStHits() == 1);
        ctx.check("isolation_combo_sum_two",
                  method_explicit_sig::comboCsHits() + method_explicit_sig::comboStHits() == 2);
        // static smap(I) canonical hit count from the snapshot (boundary re-ran it);
        // smap(String) was never re-dispatched, so its live counter is exactly 1.
        ctx.check("isolation_smap_i_one",        get("snap_smap_i_hits").ival == 1);
        ctx.check("isolation_smap_s_one",        method_explicit_sig::smapStrHits() == 1);

        // descriptor-shape selectors: each scalar shape's CANONICAL side effect
        // (snapshot) intact; shape(IIII) was not re-dispatched (live read).
        ctx.check("isolation_shape_F", get("snap_shape_F").dval == static_cast<double>(SHAPE_F_ARG));
        ctx.check("isolation_shape_D", get("snap_shape_D").dval == SHAPE_D_ARG);
        ctx.check("isolation_shape_Z", get("snap_shape_Z").ival == 1);
        ctx.check("isolation_shape_B", get("snap_shape_B").ival == SHAPE_B_ARG);
        ctx.check("isolation_shape_S", get("snap_shape_S").ival == SHAPE_S_ARG);
        ctx.check("isolation_shape_4I",
                  method_explicit_sig_counters::shapeFourArgSeen() == (SHAPE_4A + SHAPE_4B + SHAPE_4C + SHAPE_4D));
        // dup* family: instance and static side effects each set by their own kind.
        ctx.check("isolation_dup_inst", method_explicit_sig_counters::dupInstanceSeen() == DUP_INST_ARG);
        ctx.check("isolation_dup_stat", method_explicit_sig_counters::dupStaticSeen() == DUP_STAT_ARG);
        // interface default + abstract-override side effects intact.
        ctx.check("isolation_iface_def_I", method_explicit_sig_counters::ifaceDefaultIntSeen() == IFACE_DEF_I_ARG);
        ctx.check("isolation_iface_def_J", method_explicit_sig_counters::ifaceDefaultLongSeen() == IFACE_DEF_J_ARG);
        ctx.check("isolation_iface_abs",   method_explicit_sig_counters::ifaceAbstractSeen() == IFACE_ABS_ARG);
        // constructors were lookup-only: NO ctor side effect fired.
        ctx.check("isolation_init_no_int_dispatch", method_explicit_sig_counters::initIntSeen() == 0);
        ctx.check("isolation_init_no_str_dispatch", method_explicit_sig_counters::initStrSeen().empty());
        // inherited wide-mixed-arg base(JI)J dispatched once with its packed value
        // (never re-dispatched by the boundary block -> live read is canonical).
        ctx.check("isolation_base_JI", method_explicit_sig_base::baseLongIntSeen() == (BASE_JI_A * 1000 + BASE_JI_B));
        // covariant: the leaf override ran exactly once; the base body never ran.
        ctx.check("isolation_cov_leaf", method_explicit_sig::covStrSeen() == "leaf-cov");
        ctx.check("isolation_cov_base_unran", method_explicit_sig_base::covBaseSeen() == 0);
        // wide family side effects each set by their own overload (not re-dispatched).
        ctx.check("isolation_wide_JI", method_explicit_sig::wideJiSeen() == (WIDE_JI_J_ARG * 1000 + WIDE_JI_I_ARG));
        ctx.check("isolation_wide_DI",
                  method_explicit_sig::wideDiSeen() == (WIDE_DI_D_ARG + static_cast<double>(WIDE_DI_I_ARG)));
        ctx.check("isolation_wide_JJ", method_explicit_sig::wideJjSeen() == (WIDE_JJ_A_ARG - WIDE_JJ_B_ARG));
        // void-with-args sink(II)V dispatched once.
        ctx.check("isolation_sink_IIV", method_explicit_sig::sinkIiSeen() == (SINK_II_A * 1000 + SINK_II_B));
        // object-return makeObj/makeNum were lookup-only: neither dispatched (no
        // observable side effect exists to leak; the proxies carried exact sigs).
    }

    // UNCONDITIONAL teardown: the scoped_hook above is RAII (its destructor at
    // the end of the enclosing block already unhooks trigger()), but if this
    // module's body ever faulted on the no-SEH Windows path the longjmp would
    // skip that destructor and leave the hook armed for the NEXT module.  Per
    // the harness contract, force a full hook reset so a clean slate is
    // guaranteed regardless of how control left the block.
    if (ctx.reset)
    {
        ctx.reset();
    }
}
