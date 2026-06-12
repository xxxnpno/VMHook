// method_call_jni_fallback — exhaustive JVM tests for the JNI INVOCATION
// FALLBACK path of vmhook::method_proxy::call(), i.e. method_proxy::call_jni()
// (vmhook.hpp ~12488-13064).
//
// WHEN THE FALLBACK IS TAKEN: call() probes detail::find_call_stub_entry()
// (StubRoutines::_call_stub_entry).  Present (typ. JDK 8..20) -> interpreter
// call-stub fast path; ABSENT (JDK 21+, and on every JDK where the entry is not
// exported via VMStructs — which is what CI exercises) -> call() short-circuits
// straight into call_jni(), which marshals args into a jvalue[] and dispatches
// via Call(Static)?<Type>MethodA.  This module RECORDS which path is live
// (find_call_stub_entry) and asserts the converted value_t, which must be
// identical on either dispatcher.  So the module is a thorough exercise of
// call() that NATURALLY drives call_jni on the modern JDKs while staying correct
// (not skipped) on the legacy call-stub JDKs.
//
// WHAT IT STRESSES (the audit's JNI-fallback concerns):
//   * every return type   : void Z B C S I J F D String Object  (instance+static)
//   * every arg shape      : no-arg, single primitive, String, Object,
//                            multi-arg incl. long(J) + double(D) two-slot args
//   * local-ref discipline : tight loops of String-RETURN, String-ARG, and
//                            long+double MULTI-ARG primitive calls.  HotSpot's
//                            default local-ref table holds 16 entries and our
//                            long-lived attached detour threads never pop a JNI
//                            frame, so a leak (one un-released NewStringUTF /
//                            GetStringUTFChars ref per call) starves the table
//                            within ~16 iterations and later calls return ""
//                            (or wrong values).  The loops assert the result is
//                            STABLE across all iterations — the observable
//                            characterization of "no local-ref leak".  The
//                            primitive-arg loop also pins the union-aliasing
//                            footgun the audit flagged: a primitive jvalue cell
//                            must never be handed to DeleteLocalRef (would
//                            corrupt / crash) — proven by the loop staying
//                            stable and the post-loop calls remaining correct.
//   * cache warm-up        : repeated calls on the SAME proxy reuse
//                            cached_method_id / cached_class_handle with no
//                            state corruption.
//   * instance vs static   : both Call*MethodA and CallStatic*MethodA, interleaved.
//
// call() must run where current_java_thread is set, i.e. inside a hook detour.
// So we hook MethodCallJni.trigger(int); the probe calls trigger() on a real
// bytecode dispatch, and the detour performs every call() below on the live
// receiver + the static methods, recording observations into file-scope atomics.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

namespace
{
    // Wrapper for vmhook.fixtures.MethodCallJni.  Instance helpers convert each
    // returned value_t into the exact matching C++ type (so the conversion
    // operator is exercised at the right target type); static helpers exercise
    // the CallStatic*MethodA slots / the FindClass-based static jclass path.
    class method_call_jni : public vmhook::object<method_call_jni>
    {
    public:
        explicit method_call_jni(vmhook::oop_t instance) noexcept
            : vmhook::object<method_call_jni>{ instance }
        {
        }

        // -- go/done handshake + side-effect readback --
        static auto set_go(bool v) -> void              { static_field("go")->set(v); }
        static auto get_done() -> bool                  { return static_field("done")->get(); }
        static auto get_trigger_count() -> std::int32_t { return static_field("triggerCount")->get(); }
        static auto void_instance_hits() -> std::int32_t { return static_field("voidInstanceHits")->get(); }
        static auto void_static_hits()   -> std::int32_t { return static_field("voidStaticHits")->get(); }

        static auto multi_prim_called() -> bool        { return static_field("multiPrimCalled")->get(); }
        static auto multi_arg_int()     -> std::int32_t { return static_field("multiArgInt")->get(); }
        static auto multi_arg_long()    -> std::int64_t { return static_field("multiArgLong")->get(); }
        static auto multi_arg_double()  -> double       { return static_field("multiArgDouble")->get(); }

        static auto two_slot_called()     -> bool        { return static_field("twoSlotLoopCalled")->get(); }
        static auto two_slot_last_long()  -> std::int64_t { return static_field("twoSlotLastLong")->get(); }
        static auto two_slot_last_double()-> double       { return static_field("twoSlotLastDouble")->get(); }

        static auto string_arg_called() -> bool        { return static_field("stringArgCalled")->get(); }
        static auto string_arg_value()  -> std::string  { return static_field("stringArgValue")->get(); }
        static auto string_arg_len()    -> std::int32_t { return static_field("stringArgLen")->get(); }

        static auto object_arg_called()   -> bool        { return static_field("objectArgCalled")->get(); }
        static auto object_arg_non_null() -> bool        { return static_field("objectArgNonNull")->get(); }
        static auto object_arg_identity() -> std::int32_t { return static_field("objectArgIdentity")->get(); }
        static auto self_identity()       -> std::int32_t { return static_field("selfIdentity")->get(); }

        static auto last_echo_arg() -> std::int32_t { return static_field("lastEchoArg")->get(); }

        // -- instance primitive returners (convert at the exact target type) --
        auto call_bool(const char* n) -> bool          { return get_method(n)->call(); }
        auto call_byte(const char* n) -> std::int8_t   { return get_method(n)->call(); }
        auto call_char(const char* n) -> std::uint16_t { return get_method(n)->call(); }
        auto call_short(const char* n) -> std::int16_t { return get_method(n)->call(); }
        auto call_int(const char* n) -> std::int32_t   { return get_method(n)->call(); }
        auto call_long(const char* n) -> std::int64_t  { return get_method(n)->call(); }
        auto call_float(const char* n) -> float        { return get_method(n)->call(); }
        auto call_double(const char* n) -> double      { return get_method(n)->call(); }

        // -- static primitive returners --
        static auto scall_bool(const char* n) -> bool          { return static_method(n)->call(); }
        static auto scall_byte(const char* n) -> std::int8_t   { return static_method(n)->call(); }
        static auto scall_char(const char* n) -> std::uint16_t { return static_method(n)->call(); }
        static auto scall_short(const char* n) -> std::int16_t { return static_method(n)->call(); }
        static auto scall_int(const char* n) -> std::int32_t   { return static_method(n)->call(); }
        static auto scall_long(const char* n) -> std::int64_t  { return static_method(n)->call(); }
        static auto scall_float(const char* n) -> float        { return static_method(n)->call(); }
        static auto scall_double(const char* n) -> double      { return static_method(n)->call(); }
    };

    // ---- raw-bit float/double capture so special values survive the atomic ----
    inline auto f2bits(float f) noexcept -> std::uint32_t
    {
        std::uint32_t b{ 0 };
        std::memcpy(&b, &f, sizeof(b));
        return b;
    }
    inline auto d2bits(double d) noexcept -> std::uint64_t
    {
        std::uint64_t b{ 0 };
        std::memcpy(&b, &d, sizeof(b));
        return b;
    }

    constexpr std::int64_t k_uncaptured = static_cast<std::int64_t>(0xDEADBEEFCAFEF00Dull);

    // ------------------------------------------------------------------
    //  Captured observations.  The detour writes; the module body reads.
    // ------------------------------------------------------------------
    std::atomic<int>  g_detour_calls{ 0 };
    std::atomic<bool> g_detour_saw_self{ false };
    std::atomic<bool> g_call_stub_present{ false };

    // instance primitive + void returns
    std::atomic<int>          g_bool_true{ -1 };
    std::atomic<int>          g_bool_false{ -1 };
    std::atomic<std::int64_t> g_byte{ k_uncaptured };
    std::atomic<std::int64_t> g_char{ k_uncaptured };
    std::atomic<std::int64_t> g_char_max{ k_uncaptured };
    std::atomic<std::int64_t> g_char_max_as_int{ k_uncaptured }; // zero-extend proof
    std::atomic<std::int64_t> g_short{ k_uncaptured };
    std::atomic<std::int64_t> g_int{ k_uncaptured };
    std::atomic<std::int64_t> g_long{ k_uncaptured };
    std::atomic<bool>         g_float_captured{ false };
    std::atomic<std::uint32_t> g_float_bits{ 0 };
    std::atomic<bool>         g_double_captured{ false };
    std::atomic<std::uint64_t> g_double_bits{ 0 };
    std::atomic<int>          g_void_is_void{ -1 };

    // static primitive returns
    std::atomic<int>          g_s_bool_true{ -1 };
    std::atomic<std::int64_t> g_s_byte{ k_uncaptured };
    std::atomic<std::int64_t> g_s_char{ k_uncaptured };
    std::atomic<std::int64_t> g_s_short{ k_uncaptured };
    std::atomic<std::int64_t> g_s_int{ k_uncaptured };
    std::atomic<std::int64_t> g_s_long{ k_uncaptured };
    std::atomic<bool>         g_s_float_captured{ false };
    std::atomic<std::uint32_t> g_s_float_bits{ 0 };
    std::atomic<bool>         g_s_double_captured{ false };
    std::atomic<std::uint64_t> g_s_double_bits{ 0 };
    std::atomic<int>          g_s_void_is_void{ -1 };

    // single-arg primitive echo (instance + static)
    std::atomic<std::int64_t> g_echo_int{ k_uncaptured };
    std::atomic<std::int64_t> g_echo_long{ k_uncaptured };
    std::atomic<std::int64_t> g_s_echo_int{ k_uncaptured };

    // multi-arg (int, long, double) returns + side effects (instance + static)
    std::atomic<std::int64_t> g_sum_ild{ k_uncaptured };
    std::atomic<std::int64_t> g_s_sum_ild{ k_uncaptured };

    // String returns (instance + static)
    std::string  g_str_inst{};
    std::string  g_str_static{};
    std::atomic<bool> g_str_inst_is_string{ false };
    std::atomic<bool> g_str_static_is_string{ false };
    std::atomic<bool> g_str_captured{ false };

    // String arg round-trips
    std::string  g_echo_str_ascii{};
    std::string  g_echo_str_empty{};
    std::string  g_echo_str_unicode{};
    std::atomic<bool> g_echo_str_captured{ false };

    // String arg -> void body
    std::atomic<int> g_consume_str_is_void{ -1 };

    // Object arg -> void body
    std::atomic<int> g_consume_obj_is_void{ -1 };

    // Object returns (instance + static): identity + null contract
    std::atomic<bool>           g_self_nonnull{ false };
    std::atomic<std::uintptr_t> g_self_instance{ 0 };
    std::atomic<std::uintptr_t> g_receiver_instance{ 0 };
    std::atomic<int>            g_self_is_void{ -1 };
    std::atomic<bool>           g_null_obj_is_null{ false };
    std::atomic<int>            g_null_obj_is_void{ -1 };
    std::atomic<bool>           g_static_self_nonnull{ false };
    std::atomic<std::uintptr_t> g_static_self_instance{ 0 };
    std::atomic<bool>           g_static_null_is_null{ false };
    std::atomic<bool>           g_array_nonnull{ false };

    // -- TIGHT LOOPS (local-ref-leak characterization) --
    // String-RETURN loop: every call creates+releases a JNI local ref for the
    // returned String.  Distinct==1 over the whole loop proves no leak/starve.
    std::atomic<int> g_ret_loop_iters{ 0 };
    std::atomic<int> g_ret_loop_distinct{ -1 };
    // String-ARG loop: every call creates+releases a NewStringUTF local ref.
    std::atomic<int> g_arg_loop_iters{ 0 };
    std::atomic<int> g_arg_loop_mismatches{ -1 };
    // long+double MULTI-ARG primitive loop: union-aliasing footgun guard.
    std::atomic<int> g_two_loop_iters{ 0 };
    std::atomic<int> g_two_loop_mismatches{ -1 };

    // Non-corruption: a value-returning call AFTER all the loops.
    std::atomic<std::int64_t> g_post_loop_echo{ k_uncaptured };

    // ── STATIC-METHOD-VIA-INSTANCE-PROXY regression (the CI-failure bug) ──
    // A static method resolved through an INSTANCE wrapper's get_method("..")
    // yields a proxy whose this->object is the live receiver but whose Method*
    // is ACC_STATIC.  Before the fix, call()/call_jni keyed static-ness solely
    // on `object == nullptr`, so it mis-classified these as INSTANCE calls:
    //   * call_jni -> GetObjectClass + GetMethodID (which does NOT resolve
    //     static methods) -> null id -> monostate (wrong result), and on JVMs
    //     where it did resolve, the live receiver was bound as the static
    //     method's FIRST declared arg -> corrupted result + poisoned JNI
    //     exception state (which let a sibling module's uncaught exception
    //     escape and kill the JVM).
    //   * call (call_stub) -> prepends the receiver as locals[0], shifting
    //     every real argument down one slot -> wrong arithmetic.
    // The fix treats the call as static when object==nullptr OR is_static().
    // These sentinels are recognizable and INDEPENDENT of the receiver, so a
    // correct value can only come from a true static dispatch.
    std::atomic<std::int64_t> g_svia_ret_int{ k_uncaptured };      // sRetInt() == INT_MIN
    std::atomic<std::int64_t> g_svia_echo_int{ k_uncaptured };     // sEchoInt(v) == v
    std::atomic<std::int64_t> g_svia_sum_ild{ k_uncaptured };      // sSumILD(i,j,d)
    std::atomic<std::int64_t> g_svia_echo_sig{ k_uncaptured };     // via (name,sig) overload
    std::atomic<int>          g_svia_is_static{ -1 };              // proxy.is_static() == true
    std::atomic<int>          g_svia_has_receiver{ -1 };           // proxy receiver OOP != 0
    // Proof the receiver survives a static-via-instance dispatch unharmed: an
    // ordinary INSTANCE call performed immediately AFTER must still be correct.
    // NOTE: we deliberately use echoLong (NOT echoInt) for this follow-up — an
    // instance method that does NOT write the shared `lastEchoArg` field that
    // the unrelated `mcj_echo_int_side_effect` assertion reads.  Using echoInt
    // here would overwrite lastEchoArg after the post-loop echo and make that
    // sibling assertion fail (the regression the first re-land attempt hit was
    // this self-inflicted field clobber, NOT an is_static() defect).
    std::atomic<std::int64_t> g_svia_followup_inst_echo{ k_uncaptured };
    // Sentinels for the static-via-instance regression.
    constexpr std::int64_t k_svia_echo   = static_cast<std::int64_t>(0x6E5D4C3BLL); // 1851877947
    constexpr std::int64_t k_svia_follow = static_cast<std::int64_t>(0x2BCD16E0ABCDEF01LL);

    // Sentinels (mirror the fixture's boundary values).
    constexpr std::int64_t k_int_ret    = 0x0BADF00DLL;            // 195948557
    constexpr std::int64_t k_long_ret   = static_cast<std::int64_t>(0x0123456789ABCDEFLL);
    constexpr std::int32_t k_echo_int   = 0x5A5A5A5A;              // 1515870810
    constexpr std::int64_t k_echo_long  = static_cast<std::int64_t>(0x7FEEDDCCBBAA9988LL);
    constexpr std::int32_t k_sum_i      = 1000;
    constexpr std::int64_t k_sum_j      = 0x0000000100000000LL;    // 4294967296 (high dword set)
    constexpr double       k_sum_d      = 250.0;                   // exact (long)d == 250
    constexpr std::int32_t k_post_echo  = 0x1357ACE0;

    auto run_all(const std::unique_ptr<method_call_jni>& self) -> void
    {
        if (!self)
        {
            return;
        }
        method_call_jni& s = *self;
        g_receiver_instance.store(
            reinterpret_cast<std::uintptr_t>(s.get_instance()),
            std::memory_order_relaxed);

        // ───────── instance primitive returns ─────────
        g_bool_true.store(s.call_bool("retBoolTrue") ? 1 : 0);
        g_bool_false.store(s.call_bool("retBoolFalse") ? 1 : 0);
        g_byte.store(s.call_byte("retByte"));
        g_char.store(s.call_char("retChar"));
        g_char_max.store(s.call_char("retCharMax"));
        {
            const std::int32_t as_int = s.get_method("retCharMax")->call();
            g_char_max_as_int.store(as_int);
        }
        g_short.store(s.call_short("retShort"));
        g_int.store(s.call_int("retInt"));
        g_long.store(s.call_long("retLong"));
        g_float_bits.store(f2bits(s.call_float("retFloat")));
        g_float_captured.store(true);
        g_double_bits.store(d2bits(s.call_double("retDouble")));
        g_double_captured.store(true);
        {
            auto p{ s.get_method("retVoid") };
            if (p.has_value())
            {
                g_void_is_void.store(p->call().is_void() ? 1 : 0);
            }
        }

        // ───────── static primitive returns ─────────
        g_s_bool_true.store(method_call_jni::scall_bool("sRetBoolTrue") ? 1 : 0);
        g_s_byte.store(method_call_jni::scall_byte("sRetByte"));
        g_s_char.store(method_call_jni::scall_char("sRetChar"));
        g_s_short.store(method_call_jni::scall_short("sRetShort"));
        g_s_int.store(method_call_jni::scall_int("sRetInt"));
        g_s_long.store(method_call_jni::scall_long("sRetLong"));
        g_s_float_bits.store(f2bits(method_call_jni::scall_float("sRetFloat")));
        g_s_float_captured.store(true);
        g_s_double_bits.store(d2bits(method_call_jni::scall_double("sRetDouble")));
        g_s_double_captured.store(true);
        {
            auto p{ method_call_jni::static_method("sRetVoid") };
            if (p.has_value())
            {
                g_s_void_is_void.store(p->call().is_void() ? 1 : 0);
            }
        }

        // ───────── single-primitive-arg echoes ─────────
        g_echo_int.store(static_cast<std::int32_t>(s.get_method("echoInt")->call(k_echo_int)));
        g_echo_long.store(static_cast<std::int64_t>(s.get_method("echoLong")->call(k_echo_long)));
        g_s_echo_int.store(static_cast<std::int32_t>(
            method_call_jni::static_method("sEchoInt")->call(k_echo_int)));

        // ───────── multi-arg (int, long, double) — two-slot args ─────────
        // Expected return = i + j + (long)d.
        g_sum_ild.store(static_cast<std::int64_t>(
            s.get_method("sumILD")->call(k_sum_i, k_sum_j, k_sum_d)));
        g_s_sum_ild.store(static_cast<std::int64_t>(
            method_call_jni::static_method("sSumILD")->call(k_sum_i, k_sum_j, k_sum_d)));

        // ───────── String returns ─────────
        {
            auto p{ s.get_method("retString") };
            if (p.has_value())
            {
                const auto v{ p->call() };
                g_str_inst           = v.as_string();
                g_str_inst_is_string.store(v.is_string());
            }
        }
        {
            auto p{ method_call_jni::static_method("sRetString") };
            if (p.has_value())
            {
                const auto v{ p->call() };
                g_str_static           = v.as_string();
                g_str_static_is_string.store(v.is_string());
            }
        }
        g_str_captured.store(true);

        // ───────── String-arg round-trips (echoString) ─────────
        {
            auto p{ s.get_method("echoString") };
            if (p.has_value())
            {
                g_echo_str_ascii   = p->call(std::string{ "round-trip-jni-987" }).as_string();
                g_echo_str_empty   = p->call(std::string{}).as_string();
                // Latin-1 char café: round-trips through NewStringUTF (modified
                // UTF-8 out) and GetStringUTFChars (modified UTF-8 back) on the
                // call_jni path, so the bytes come back identical there.
                g_echo_str_unicode = p->call(std::string{ "caf\xC3\xA9" }).as_string();
            }
        }
        g_echo_str_captured.store(true);

        // ───────── String arg -> void body ─────────
        {
            auto p{ s.get_method("consumeString") };
            if (p.has_value())
            {
                const auto v{ p->call(std::string{ "void-string-arg-jni" }) };
                g_consume_str_is_void.store(v.is_void() ? 1 : 0);
            }
        }

        // ───────── Object arg -> void body (pass the live receiver) ─────────
        {
            auto p{ s.get_method("consumeObject") };
            if (p.has_value())
            {
                const auto v{ p->call(*self) };
                g_consume_obj_is_void.store(v.is_void() ? 1 : 0);
            }
        }

        // ───────── Object returns (identity + null contract) ─────────
        {
            // copy-init (=), NOT brace-init: value_t's templated conversion
            // operator makes std::unique_ptr<T>{ value_t } ambiguous under MSVC
            // (C2440).  Copy-init resolves the user-defined conversion cleanly.
            std::unique_ptr<method_call_jni> sp = s.get_method("retSelf")->call();
            g_self_nonnull.store(sp != nullptr);
            if (sp)
            {
                g_self_instance.store(
                    reinterpret_cast<std::uintptr_t>(sp->get_instance()));
            }
            auto p{ s.get_method("retSelf") };
            if (p.has_value())
            {
                g_self_is_void.store(p->call().is_void() ? 1 : 0);
            }
        }
        {
            std::unique_ptr<method_call_jni> np = s.get_method("retNullObject")->call();  // copy-init (MSVC C2440)
            g_null_obj_is_null.store(np == nullptr);
            auto p{ s.get_method("retNullObject") };
            if (p.has_value())
            {
                g_null_obj_is_void.store(p->call().is_void() ? 1 : 0);
            }
        }
        {
            auto sm{ method_call_jni::static_method("sRetSingleton") };
            if (sm.has_value())
            {
                std::unique_ptr<method_call_jni> sp = sm->call();
                g_static_self_nonnull.store(sp != nullptr);
                if (sp)
                {
                    g_static_self_instance.store(
                        reinterpret_cast<std::uintptr_t>(sp->get_instance()));
                }
            }
            auto sn{ method_call_jni::static_method("sRetNullObject") };
            if (sn.has_value())
            {
                std::unique_ptr<method_call_jni> sp = sn->call();
                g_static_null_is_null.store(sp == nullptr);
            }
        }
        // Array reference return ('[' descriptor): decode to a non-null oop via
        // the value_t void* conversion (decode_oop_pointer), without walking it.
        {
            auto p{ s.get_method("retIntArray") };
            if (p.has_value())
            {
                const auto v{ p->call() };
                void* const arr{ static_cast<void*>(v) };
                g_array_nonnull.store(arr != nullptr);
            }
        }

        // ═════════ TIGHT LOOP 1: String-RETURN local-ref discipline ═════════
        // Every loopString() return is a JNI local ref that call_jni decodes to
        // UTF-8 and releases.  A leak would starve the 16-entry table within
        // ~16 iterations; once starved, later calls return "" -> a 2nd distinct
        // value.  distinct == 1 proves stable, leak-free String-return decoding.
        {
            constexpr int iters{ 256 };
            std::string first{};
            bool have_first{ false };
            int distinct{ 0 };
            auto proxy{ s.get_method("loopString") };
            for (int i{ 0 }; i < iters; ++i)
            {
                if (!proxy.has_value())
                {
                    break;
                }
                const std::string r{ proxy->call().as_string() };
                if (!have_first)
                {
                    first = r;
                    have_first = true;
                    distinct = 1;
                }
                else if (r != first)
                {
                    ++distinct;
                }
            }
            g_ret_loop_iters.store(iters);
            g_ret_loop_distinct.store(distinct);
        }

        // ═════════ TIGHT LOOP 2: String-ARG local-ref discipline ═════════
        // Every echoString(s) marshals a NewStringUTF local ref (released by
        // the arg-cleanup RAII) and returns a String local ref (released by the
        // String-return decode).  Two refs per iteration -> the table starves
        // twice as fast if either release is missing.  We assert every echo
        // round-trips to the exact input; a starved table yields "" mismatches.
        {
            constexpr int iters{ 256 };
            const std::string payload{ "arg-loop-payload-42" };
            int mismatches{ 0 };
            auto proxy{ s.get_method("echoString") };
            for (int i{ 0 }; i < iters; ++i)
            {
                if (!proxy.has_value())
                {
                    mismatches = iters;
                    break;
                }
                const std::string r{ proxy->call(payload).as_string() };
                if (r != payload)
                {
                    ++mismatches;
                }
            }
            g_arg_loop_iters.store(iters);
            g_arg_loop_mismatches.store(mismatches);
        }

        // ═════════ TIGHT LOOP 3: long+double MULTI-ARG primitive calls ═════════
        // twoSlot(long a, double b, long c) -> a + c + (long)b.  This is the
        // union-aliasing footgun: each jvalue cell holds a primitive (jlong /
        // jdouble) whose .l alias is a non-null garbage pointer; the arg-cleanup
        // MUST NOT hand it to DeleteLocalRef.  A regression there corrupts the
        // thread's local-ref state and later iterations diverge (or crash).  We
        // assert every iteration returns the exact arithmetic result and the
        // recorded args match — stable across 256 iterations proves the
        // primitive cells are never released and state never corrupts.
        {
            constexpr int iters{ 256 };
            const std::int64_t a{ static_cast<std::int64_t>(0x4242424242424242LL) };
            const double       b{ 1024.0 };
            const std::int64_t c{ static_cast<std::int64_t>(0x0000111122223333LL) };
            const std::int64_t expected{ a + c + static_cast<std::int64_t>(b) };
            int mismatches{ 0 };
            auto proxy{ s.get_method("twoSlot") };
            for (int i{ 0 }; i < iters; ++i)
            {
                if (!proxy.has_value())
                {
                    mismatches = iters;
                    break;
                }
                const std::int64_t r{ proxy->call(a, b, c) };
                if (r != expected)
                {
                    ++mismatches;
                }
            }
            g_two_loop_iters.store(iters);
            g_two_loop_mismatches.store(mismatches);
        }

        // ═════════ NON-CORRUPTION: value-returning call after all loops ═════════
        {
            auto p{ s.get_method("echoInt") };
            if (p.has_value())
            {
                const std::int32_t r{ p->call(k_post_echo) };
                g_post_loop_echo.store(r);
            }
        }

        // ═════════ STATIC METHOD resolved through the INSTANCE wrapper ═════════
        // get_method("sX") on the instance `s` returns a proxy bound to the live
        // receiver (this->object != null) whose Method* is ACC_STATIC.  These
        // calls reproduce the CI-failure bug exactly; the asserted results below
        // would be wrong (or monostate, and the JVM possibly torn down) before
        // the call()/call_jni static-detection fix.
        {
            // No-arg static via instance proxy: sRetInt() == Integer.MIN_VALUE.
            // Under the bug this took the instance path (GetMethodID can't see a
            // static -> monostate -> 0), so a correct -2147483648 proves static
            // dispatch fired off the Method's declaring class, not the receiver.
            auto p_ret{ s.get_method("sRetInt") };
            if (p_ret.has_value())
            {
                g_svia_is_static.store(p_ret->is_static() ? 1 : 0);
                // The proxy MUST still carry the receiver OOP (that's the whole
                // point — it came from an instance wrapper); the fix must make it
                // dispatch static *despite* a non-null receiver.
                g_svia_has_receiver.store(p_ret->get_compressed_oop() != 0u ? 1 : 0);
                g_svia_ret_int.store(static_cast<std::int32_t>(p_ret->call()));
            }

            // Single-arg static via instance proxy: sEchoInt(v) == v.  This is the
            // sharpest discriminator for the "receiver bound as first arg" mis-
            // dispatch: if the live instance were pinned into slot 0 / arg 0, the
            // echoed value would be the receiver's bits, never our sentinel.
            auto p_echo{ s.get_method("sEchoInt") };
            if (p_echo.has_value())
            {
                g_svia_echo_int.store(static_cast<std::int32_t>(p_echo->call(
                    static_cast<std::int32_t>(k_svia_echo))));
            }

            // Multi-arg (I,J,D) static via instance proxy: a receiver shift would
            // corrupt every two-slot argument, so the exact arithmetic sum is a
            // strong proof the argument block was laid out with NO receiver.
            auto p_sum{ s.get_method("sSumILD") };
            if (p_sum.has_value())
            {
                g_svia_sum_ild.store(static_cast<std::int64_t>(
                    p_sum->call(k_sum_i, k_sum_j, k_sum_d)));
            }

            // The (name, signature) instance overload must behave identically.
            auto p_sig{ s.get_method("sEchoInt", "(I)I") };
            if (p_sig.has_value())
            {
                g_svia_echo_sig.store(static_cast<std::int32_t>(p_sig->call(
                    static_cast<std::int32_t>(k_svia_echo))));
            }

            // Non-corruption: an ordinary INSTANCE call AFTER the static-via-
            // instance dispatches must still deliver its argument intact (proves
            // the receiver/JNI state was not poisoned by the static calls).  We
            // use echoLong here (returns its long arg, NO lastEchoArg side effect)
            // so this follow-up cannot clobber the field that the unrelated
            // mcj_echo_int_side_effect assertion reads.
            auto p_follow{ s.get_method("echoLong") };
            if (p_follow.has_value())
            {
                g_svia_followup_inst_echo.store(
                    static_cast<std::int64_t>(p_follow->call(k_svia_follow)));
            }
        }
    }
}

VMHOOK_JVM_MODULE(method_call_jni_fallback)
{
    vmhook::register_class<method_call_jni>("vmhook/fixtures/MethodCallJni");

    // Record which dispatch path the live JDK uses.  On JDK 21+ (and any JDK
    // that does not export StubRoutines::_call_stub_entry via VMStructs) this is
    // the JNI fallback — the path this module targets.
    g_call_stub_present.store(vmhook::detail::find_call_stub_entry() != nullptr,
                              std::memory_order_relaxed);

    {
        auto handle{ vmhook::scoped_hook<method_call_jni>(
            "trigger",
            [](vmhook::return_value&,
               const std::unique_ptr<method_call_jni>& self,
               std::int32_t /*delta*/)
            {
                g_detour_calls.fetch_add(1, std::memory_order_relaxed);
                g_detour_saw_self.store(self != nullptr, std::memory_order_relaxed);
                run_all(self);
            }) };

        ctx.check("mcj_hook_installed", handle.installed());

        const bool done{ ctx.run_probe(
            [](bool v) { method_call_jni::set_go(v); },
            []() { return method_call_jni::get_done(); }) };

        ctx.check("mcj_probe_completed", done);
        ctx.check("mcj_detour_fired", g_detour_calls.load(std::memory_order_relaxed) >= 1);
        ctx.check("mcj_detour_saw_self", g_detour_saw_self.load(std::memory_order_relaxed));
        ctx.check("mcj_trigger_count_advanced", method_call_jni::get_trigger_count() >= 1);

        const bool stub{ g_call_stub_present.load(std::memory_order_relaxed) };
        ctx.record(std::string{ "[INFO] method_call_jni_fallback dispatch path: " }
                   + (stub ? "call_stub fast path (StubRoutines::_call_stub_entry present) - "
                             "JNI fallback NOT exercised on this JDK; assertions still valid"
                           : "JNI fallback (call_jni: Call(Static)?<Type>MethodA) - "
                             "this module's target path"));

        // ═════════════════════ INSTANCE primitive returns ═════════════════════
        ctx.check("mcj_bool_true_instance",  g_bool_true.load()  == 1);
        ctx.check("mcj_bool_false_instance", g_bool_false.load() == 0);
        ctx.check("mcj_byte_neg7_sign_extends", g_byte.load() == -7);
        ctx.check("mcj_char_Z_90",   g_char.load() == 90);
        ctx.check("mcj_char_max_65535", g_char_max.load() == 65535);
        ctx.check("mcj_char_max_zero_extends_to_int_65535", g_char_max_as_int.load() == 65535);
        ctx.check("mcj_short_neg12345_sign_extends", g_short.load() == -12345);
        ctx.check("mcj_int_badf00d", g_int.load() == k_int_ret);
        ctx.check("mcj_long_pattern", g_long.load() == k_long_ret);
        ctx.check("mcj_float_captured", g_float_captured.load());
        {
            float f{ 0.0f };
            const std::uint32_t b{ g_float_bits.load() };
            std::memcpy(&f, &b, sizeof(f));
            ctx.check("mcj_float_3_5_exact", f == 3.5f);
        }
        ctx.check("mcj_double_captured", g_double_captured.load());
        {
            double d{ 0.0 };
            const std::uint64_t b{ g_double_bits.load() };
            std::memcpy(&d, &b, sizeof(d));
            ctx.check("mcj_double_e_exact", d == 2.718281828459045);
        }
        ctx.check("mcj_void_instance_is_void", g_void_is_void.load() == 1);
        ctx.check("mcj_void_instance_side_effect", method_call_jni::void_instance_hits() == 1);

        // ═════════════════════ STATIC primitive returns ═══════════════════════
        ctx.check("mcj_static_bool_true",  g_s_bool_true.load() == 1);
        ctx.check("mcj_static_byte_99",    g_s_byte.load() == 99);
        ctx.check("mcj_static_char_k_107", g_s_char.load() == 107);
        ctx.check("mcj_static_short_20000", g_s_short.load() == 20000);
        ctx.check("mcj_static_int_min", g_s_int.load() == -2147483648LL);
        ctx.check("mcj_static_long_max", g_s_long.load() == 0x7FFFFFFFFFFFFFFFLL);
        ctx.check("mcj_static_float_captured", g_s_float_captured.load());
        {
            float f{ 0.0f };
            const std::uint32_t b{ g_s_float_bits.load() };
            std::memcpy(&f, &b, sizeof(f));
            ctx.check("mcj_static_float_neg_half", f == -0.5f && std::signbit(f));
        }
        ctx.check("mcj_static_double_captured", g_s_double_captured.load());
        {
            double d{ 0.0 };
            const std::uint64_t b{ g_s_double_bits.load() };
            std::memcpy(&d, &b, sizeof(d));
            ctx.check("mcj_static_double_neg_1_5", d == -1.5);
        }
        ctx.check("mcj_static_void_is_void", g_s_void_is_void.load() == 1);
        ctx.check("mcj_static_void_side_effect", method_call_jni::void_static_hits() == 1);

        // ═════════════════════ single-arg primitive echoes ════════════════════
        ctx.check("mcj_echo_int_passthrough", g_echo_int.load() == k_echo_int);
        ctx.check("mcj_echo_int_side_effect", method_call_jni::last_echo_arg() == k_post_echo); // last echo was the post-loop one
        ctx.check("mcj_echo_long_passthrough", g_echo_long.load() == k_echo_long);
        ctx.check("mcj_static_echo_int_passthrough", g_s_echo_int.load() == k_echo_int);

        // ═════════════════════ multi-arg (I,J,D) two-slot args ════════════════
        // result = i + j + (long)d ; proves long + double both landed correctly.
        const std::int64_t expected_sum{ static_cast<std::int64_t>(k_sum_i)
                                         + k_sum_j + static_cast<std::int64_t>(k_sum_d) };
        ctx.check("mcj_multi_arg_return_correct", g_sum_ild.load() == expected_sum);
        ctx.check("mcj_static_multi_arg_return_correct", g_s_sum_ild.load() == expected_sum);
        // And each argument arrived verbatim at the (instance) body.
        ctx.check("mcj_multi_arg_called", method_call_jni::multi_prim_called());
        ctx.check("mcj_multi_arg_int", method_call_jni::multi_arg_int() == k_sum_i);
        ctx.check("mcj_multi_arg_long", method_call_jni::multi_arg_long() == k_sum_j);
        ctx.check("mcj_multi_arg_double", method_call_jni::multi_arg_double() == k_sum_d);

        // ═════════════════════ String returns ═════════════════════════════════
        ctx.check("mcj_str_captured", g_str_captured.load());
        ctx.check("mcj_str_instance_exact", g_str_inst == "jni-instance-hello");
        ctx.check("mcj_str_instance_is_string", g_str_inst_is_string.load());
        ctx.check("mcj_str_instance_nonempty", !g_str_inst.empty());
        ctx.check("mcj_str_static_exact", g_str_static == "jni-static-hello");
        ctx.check("mcj_str_static_is_string", g_str_static_is_string.load());
        ctx.check("mcj_str_static_nonempty", !g_str_static.empty());

        // ═════════════════════ String-arg round-trips ═════════════════════════
        ctx.check("mcj_echo_str_captured", g_echo_str_captured.load());
        ctx.check("mcj_echo_str_ascii_exact", g_echo_str_ascii == "round-trip-jni-987");
        ctx.check("mcj_echo_str_empty_exact", g_echo_str_empty.empty());
        // Latin-1 café round-trips byte-for-byte on the call_jni path (modified
        // UTF-8 both directions).  On the call_stub path make_java_string copies
        // the raw bytes into LATIN1 and read_java_string maps the two high bytes
        // to '?', so the round trip is "caf??" there.  Assert per path.
        if (stub)
        {
            ctx.check("mcj_echo_str_unicode_call_stub", g_echo_str_unicode == "caf??");
        }
        else
        {
            ctx.check("mcj_echo_str_unicode_call_jni", g_echo_str_unicode == "caf\xC3\xA9");
        }

        // ═════════════════════ String/Object arg -> void body ═════════════════
        ctx.check("mcj_consume_string_is_void", g_consume_str_is_void.load() == 1);
        ctx.check("mcj_consume_string_called", method_call_jni::string_arg_called());
        ctx.check("mcj_consume_string_len_exact",
                  method_call_jni::string_arg_len() == static_cast<std::int32_t>(std::string{ "void-string-arg-jni" }.size()));
        ctx.check("mcj_consume_string_value_exact",
                  method_call_jni::string_arg_value() == "void-string-arg-jni");

        ctx.check("mcj_consume_object_is_void", g_consume_obj_is_void.load() == 1);
        ctx.check("mcj_consume_object_called", method_call_jni::object_arg_called());
        ctx.check("mcj_consume_object_non_null", method_call_jni::object_arg_non_null());
        // The body's identityHashCode of the received object equals the receiver's
        // published identity -> the EXACT receiver object reached the void body.
        ctx.check("mcj_consume_object_identity_matches_receiver",
                  method_call_jni::object_arg_identity() != 0
                  && method_call_jni::object_arg_identity() == method_call_jni::self_identity());

        // ═════════════════════ Object returns: null contract (BOTH paths) ══════
        // The most important reference-return invariant: a null Java return must
        // never fabricate a wrapper, and must be is_void().  Holds on every path.
        ctx.check("mcj_null_object_returns_null_unique_ptr", g_null_obj_is_null.load());
        ctx.check("mcj_null_object_is_void", g_null_obj_is_void.load() == 1);
        ctx.check("mcj_static_null_object_returns_null_unique_ptr", g_static_null_is_null.load());

        // ═════════════════════ Object returns: identity ═══════════════════════
        // The call_jni 'L'/'[' arm decodes the JNI handle to the real heap OOP
        // (jni_decode_object) and re-encodes it (encode_oop_pointer), promoting
        // the live local ref to a JNI global-ref PIN that spans the decode→encode
        // so the OOP is never unrooted in that window (a relocating GC there would
        // otherwise have left it stale).  A non-null reference return therefore
        // round-trips into a usable wrapper on BOTH paths.  retSelf() must yield
        // the RECEIVER's OOP.  (This is an in-detour, same-tick check: no GC is
        // forced between the decode and the compare, so it exercises the decode/
        // re-encode correctness; the pin's value is preventing a GC-window stale
        // read, which this harness cannot deterministically trigger.)
        ctx.check("mcj_retself_non_null_wrapper", g_self_nonnull.load());
        ctx.check("mcj_retself_instance_equals_receiver",
                  g_self_instance.load() != 0
                  && g_self_instance.load() == g_receiver_instance.load());
        // A non-null Object (non-String) return is NOT is_void().
        ctx.check("mcj_retself_not_void", g_self_is_void.load() == 0);
        ctx.check("mcj_static_singleton_non_null_wrapper", g_static_self_nonnull.load());
        ctx.check("mcj_static_singleton_instance_equals_receiver",
                  g_static_self_instance.load() != 0
                  && g_static_self_instance.load() == g_receiver_instance.load());
        // Array reference return decoded to a non-null oop.
        ctx.check("mcj_array_reference_decoded_non_null", g_array_nonnull.load());

        // ═════════════════════ TIGHT LOOP characterization ════════════════════
        // These are the JNI-fallback local-ref-leak guards the audit flagged.

        // String-RETURN loop: stable single value across 256 iterations.
        ctx.check("mcj_string_return_loop_ran", g_ret_loop_iters.load() == 256);
        ctx.check("mcj_string_return_loop_no_leak_single_distinct",
                  g_ret_loop_distinct.load() == 1);

        // String-ARG loop: every echo round-trips exactly (zero mismatches).
        ctx.check("mcj_string_arg_loop_ran", g_arg_loop_iters.load() == 256);
        ctx.check("mcj_string_arg_loop_no_leak_zero_mismatches",
                  g_arg_loop_mismatches.load() == 0);

        // long+double MULTI-ARG primitive loop: zero mismatches => primitive
        // jvalue cells are never handed to DeleteLocalRef (union-aliasing safe)
        // and cached state is reused without corruption.
        ctx.check("mcj_two_slot_loop_ran", g_two_loop_iters.load() == 256);
        ctx.check("mcj_two_slot_loop_no_corruption_zero_mismatches",
                  g_two_loop_mismatches.load() == 0);
        // The loop body's recorded args (from the last iteration) confirm the
        // two-slot long + double survived marshalling.
        ctx.check("mcj_two_slot_loop_called", method_call_jni::two_slot_called());
        ctx.check("mcj_two_slot_last_long",
                  method_call_jni::two_slot_last_long() == static_cast<std::int64_t>(0x4242424242424242LL));
        ctx.check("mcj_two_slot_last_double",
                  method_call_jni::two_slot_last_double() == 1024.0);

        // ═════════════════════ NON-CORRUPTION after the loops ═════════════════
        // A value-returning call after hundreds of JNI dispatches still works.
        ctx.check("mcj_post_loop_echo_value", g_post_loop_echo.load() == k_post_echo);

        // ═════════ STATIC METHOD via INSTANCE PROXY (CI-failure regression) ════
        // The bug: call()/call_jni decided static-vs-instance from
        // `this->object == nullptr` alone, so a static Method resolved through an
        // instance wrapper's get_method("..") (non-null receiver) was dispatched
        // as an instance call — wrong result AND corrupted JNI exception state.
        // The fix dispatches static whenever object==nullptr OR is_static().
        // These assertions FAIL on the pre-fix header (monostate/0 on the JNI
        // path, receiver-shifted arithmetic on the call_stub path) and PASS now,
        // on BOTH dispatch paths.  (The first re-land of this fix regressed only
        // the sibling mcj_echo_int_side_effect check — a self-inflicted field
        // clobber from a now-removed echoInt follow-up — not an is_static defect;
        // the follow-up below uses echoLong so lastEchoArg is left untouched.)

        // The proxy is genuinely a static method that nevertheless carries the
        // receiver (so the fix has to override a non-null `object`).
        ctx.check("mcj_svia_proxy_is_static", g_svia_is_static.load() == 1);
        ctx.check("mcj_svia_proxy_has_receiver", g_svia_has_receiver.load() == 1);

        // No-arg static via instance proxy returns the real static value.
        ctx.check("mcj_svia_no_arg_static_returns_int_min",
                  g_svia_ret_int.load() == -2147483648LL);
        // Single-arg static via instance proxy echoes the sentinel — proves the
        // receiver was NOT bound as the static method's first argument.
        ctx.check("mcj_svia_single_arg_static_echo_passthrough",
                  g_svia_echo_int.load() == k_svia_echo);
        // Multi-arg (I,J,D) static via instance proxy: exact sum proves the arg
        // block had no phantom receiver slot.
        ctx.check("mcj_svia_multi_arg_static_return_correct",
                  g_svia_sum_ild.load() == expected_sum);
        // The (name, signature) instance overload behaves identically.
        ctx.check("mcj_svia_sig_overload_static_echo_passthrough",
                  g_svia_echo_sig.load() == k_svia_echo);
        // And an ordinary INSTANCE call performed right after the static-via-
        // instance dispatches still works — the receiver / JNI state survived.
        // (echoLong returns its argument; no lastEchoArg side effect.)
        ctx.check("mcj_svia_followup_instance_call_intact",
                  g_svia_followup_inst_echo.load() == k_svia_follow);
    }
}
