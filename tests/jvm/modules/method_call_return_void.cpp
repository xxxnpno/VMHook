// method_call_return_void JVM test module — area: methods.
//
// FEATURE: vmhook::method_proxy::call() invoking VOID-returning Java methods,
// and the value_t::is_void() introspection path.
//
// The void return path of call() (vmhook.hpp): the result-decode switch's
// case 'V' returns value_t{ std::monostate{} } WITHOUT reading the result
// slot, on BOTH the interpreter call_stub fast path (when the JDK exposes
// StubRoutines::_call_stub_entry) and the JNI fallback (CallVoidMethodA /
// CallStaticVoidMethodA on modern JDKs).  is_void() reports true for that
// monostate, distinguishing "returned void / failed" from a primitive zero.
//
// A void method returns nothing the native side can read, so this module proves
// each call TWO independent ways:
//   (1) the returned value_t.is_void() is true  (the discard contract), AND
//   (2) the Java body actually executed with the right arguments — observed by
//       reading the static fields the fixture records its invocation/args into.
//
// Both halves matter: (1) without (2) would pass even if call() silently no-op'd
// the dispatch; (2) without (1) would miss a value_t that wrongly carried a
// numeric alternative for a 'V' signature.
//
// Scenarios (every angle the audit finding lists):
//   * void INSTANCE method                       -> is_void + side effect,
//   * void STATIC method                         -> is_void + side effect
//                                                   (static slot; no receiver),
//   * void method with PRIMITIVE args (I,J,Z,D)  -> args observed verbatim,
//   * void method with a STRING arg              -> String observed,
//   * void method with an OBJECT arg             -> identity observed,
//   * NON-CORRUPTION: a value-returning call right after a void call still
//     returns the correct value (a void dispatch must not poison the thread),
//   * CONTRAST: is_void() is FALSE for an int-returning method (zero != void).
//
// call() must run where current_java_thread is set, i.e. inside a hook detour.
// So we hook MethodCallVoid.trigger(int); the probe calls trigger() on a real
// bytecode dispatch, and the detour performs every call() below on the live
// receiver + the static methods, recording observations into file-scope atomics.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace
{
    // Wrapper for vmhook.fixtures.MethodCallVoid.  Static helpers read back the
    // side-effect / recorded-argument fields the void bodies write, so the
    // native side can prove each void dispatch reached a real Java body.
    class method_call_void : public vmhook::object<method_call_void>
    {
    public:
        explicit method_call_void(vmhook::oop_t instance) noexcept
            : vmhook::object<method_call_void>{ instance }
        {
        }

        // -- go/done handshake --
        static auto set_go(bool v) -> void { static_field("go")->set(v); }
        static auto get_done() -> bool     { return static_field("done")->get(); }

        // -- side-effect counters --
        static auto void_instance_hits() -> std::int32_t { return static_field("voidInstanceHits")->get(); }
        static auto void_static_hits()   -> std::int32_t { return static_field("voidStaticHits")->get(); }

        // -- recorded primitive args --
        static auto prim_args_called() -> bool        { return static_field("primArgsCalled")->get(); }
        static auto prim_arg_int()     -> std::int32_t { return static_field("primArgInt")->get(); }
        static auto prim_arg_long()    -> std::int64_t { return static_field("primArgLong")->get(); }
        static auto prim_arg_bool()    -> bool         { return static_field("primArgBool")->get(); }
        static auto prim_arg_double()  -> double       { return static_field("primArgDouble")->get(); }

        // -- recorded String arg --
        static auto string_arg_called() -> bool        { return static_field("stringArgCalled")->get(); }
        static auto string_arg()        -> std::string  { return static_field("stringArg")->get(); }
        static auto string_arg_len()    -> std::int32_t { return static_field("stringArgLen")->get(); }

        // -- recorded Object arg --
        static auto object_arg_called()   -> bool        { return static_field("objectArgCalled")->get(); }
        static auto object_arg_non_null() -> bool        { return static_field("objectArgNonNull")->get(); }
        static auto object_arg_identity() -> std::int32_t { return static_field("objectArgIdentity")->get(); }
        static auto self_identity()       -> std::int32_t { return static_field("selfIdentity")->get(); }

        // -- non-corruption breadcrumb --
        static auto last_echo_arg() -> std::int32_t { return static_field("lastEchoArg")->get(); }

        // -- repeat dispatch --
        static auto void_repeat_hits() -> std::int32_t { return static_field("voidRepeatHits")->get(); }

        // -- boundary primitive args (second recorder) --
        static auto edge_prims_called() -> bool         { return static_field("edgePrimsCalled")->get(); }
        static auto edge_prim_int()     -> std::int32_t { return static_field("edgePrimInt")->get(); }
        static auto edge_prim_long()    -> std::int64_t { return static_field("edgePrimLong")->get(); }
        static auto edge_prim_bool()    -> bool         { return static_field("edgePrimBool")->get(); }
        static auto edge_prim_double()  -> double       { return static_field("edgePrimDouble")->get(); }

        // -- narrow / float args --
        static auto narrow_args_called() -> bool         { return static_field("narrowArgsCalled")->get(); }
        static auto narrow_arg_byte()    -> std::int8_t  { return static_field("narrowArgByte")->get(); }
        static auto narrow_arg_short()   -> std::int16_t { return static_field("narrowArgShort")->get(); }
        static auto narrow_arg_char()    -> std::uint16_t { return static_field("narrowArgChar")->get(); }
        static auto narrow_arg_float()   -> float        { return static_field("narrowArgFloat")->get(); }

        // -- degenerate String args --
        static auto empty_string_called() -> bool        { return static_field("emptyStringCalled")->get(); }
        static auto empty_string_len()    -> std::int32_t { return static_field("emptyStringLen")->get(); }
        static auto null_string_called()  -> bool        { return static_field("nullStringCalled")->get(); }
        static auto null_string_was_null() -> bool       { return static_field("nullStringWasNull")->get(); }

        // -- null Object arg --
        static auto null_object_called()   -> bool { return static_field("nullObjectCalled")->get(); }
        static auto null_object_was_null() -> bool { return static_field("nullObjectWasNull")->get(); }

        // -- many-arg void --
        static auto many_args_called() -> bool        { return static_field("manyArgsCalled")->get(); }
        static auto many_args_sum()    -> std::int32_t { return static_field("manyArgsSum")->get(); }
        static auto many_args_last()   -> std::int32_t { return static_field("manyArgsLast")->get(); }

        // -- String arg via const char*/string_view packer --
        static auto cstr_arg_called() -> bool        { return static_field("cstrArgCalled")->get(); }
        static auto cstr_arg()        -> std::string  { return static_field("cstrArg")->get(); }
        static auto cstr_arg_len()    -> std::int32_t { return static_field("cstrArgLen")->get(); }

        // -- static void with a primitive arg --
        static auto static_arg_called() -> bool        { return static_field("staticArgCalled")->get(); }
        static auto static_arg_int()    -> std::int32_t { return static_field("staticArgInt")->get(); }

        // -- throwing void methods --
        static auto void_throw_reached()        -> bool        { return static_field("voidThrowReached")->get(); }
        static auto void_static_throw_reached() -> bool        { return static_field("voidStaticThrowReached")->get(); }
        static auto void_throw_arg_reached()    -> bool        { return static_field("voidThrowArgReached")->get(); }
        static auto void_throw_arg_value()      -> std::int32_t { return static_field("voidThrowArgValue")->get(); }
        static auto void_throw_reached_count()  -> std::int32_t { return static_field("voidThrowReachedCount")->get(); }

        // -- exactly-8-slot instance void --
        static auto eight_slot_called() -> bool        { return static_field("eightSlotCalled")->get(); }
        static auto eight_slot_sum()    -> std::int32_t { return static_field("eightSlotSum")->get(); }
        static auto eight_slot_last()   -> std::int32_t { return static_field("eightSlotLast")->get(); }

        // -- all-wide void (long + double) --
        static auto wide_only_called() -> bool   { return static_field("wideOnlyCalled")->get(); }
        static auto wide_only_long()   -> std::int64_t { return static_field("wideOnlyLong")->get(); }
        static auto wide_only_double() -> double { return static_field("wideOnlyDouble")->get(); }

        // -- mixed ref + primitive + String void --
        static auto mixed_ref_called()       -> bool        { return static_field("mixedRefCalled")->get(); }
        static auto mixed_ref_obj_non_null() -> bool        { return static_field("mixedRefObjNonNull")->get(); }
        static auto mixed_ref_obj_identity() -> std::int32_t { return static_field("mixedRefObjIdentity")->get(); }
        static auto mixed_ref_int()          -> std::int32_t { return static_field("mixedRefInt")->get(); }
        static auto mixed_ref_str_len()      -> std::int32_t { return static_field("mixedRefStrLen")->get(); }

        // -- INHERITED instance / static void (declared on MethodCallVoidBase,
        //    read back through the subclass wrapper via the super-chain walk) --
        static auto void_inherited_hits()        -> std::int32_t { return static_field("voidInheritedHits")->get(); }
        static auto void_inherited_static_hits() -> std::int32_t { return static_field("voidInheritedStaticHits")->get(); }

        // -- INTERFACE-DEFAULT void (recorded on the base for super-chain reach) --
        static auto void_default_hits() -> std::int32_t { return static_field("voidDefaultHits")->get(); }

        // -- interleaved mixed-width void args (int, long, int, double) --
        static auto interleaved_called() -> bool         { return static_field("interleavedCalled")->get(); }
        static auto interleaved_int1()   -> std::int32_t { return static_field("interleavedInt1")->get(); }
        static auto interleaved_long()   -> std::int64_t { return static_field("interleavedLong")->get(); }
        static auto interleaved_int2()   -> std::int32_t { return static_field("interleavedInt2")->get(); }
        static auto interleaved_double() -> double       { return static_field("interleavedDouble")->get(); }

        // -- mixed floating-point void args (float + double) --
        static auto mixed_fp_called() -> bool   { return static_field("mixedFpCalled")->get(); }
        static auto mixed_fp_float()  -> float  { return static_field("mixedFpFloat")->get(); }
        static auto mixed_fp_double() -> double { return static_field("mixedFpDouble")->get(); }
    };

    // ------------------------------------------------------------------
    //  Captured observations.  The detour writes; the module body reads.
    // ------------------------------------------------------------------
    std::atomic<int>  g_detour_calls{ 0 };
    std::atomic<bool> g_detour_saw_self{ false };
    std::atomic<bool> g_call_stub_present{ false };

    // is_void() results for every void call (instance / static / arg variants).
    std::atomic<int> g_void_inst_is_void{ -1 };
    std::atomic<int> g_void_stat_is_void{ -1 };
    std::atomic<int> g_void_prim_is_void{ -1 };
    std::atomic<int> g_void_str_is_void{ -1 };
    std::atomic<int> g_void_obj_is_void{ -1 };

    // is_void() / is_string() on a NON-void int return must both be false, and
    // the int value must decode correctly (proves the contrast call really ran).
    std::atomic<int>          g_int_is_void{ -1 };
    std::atomic<int>          g_int_is_string{ -1 };
    std::atomic<std::int64_t> g_int_ret_value{ 0 };

    // Non-corruption: a value-returning call performed AFTER void calls.  Both
    // the echoed arg (echoIntAfterVoid) and a constant returner (retInt) are
    // captured so we prove the post-void dispatch path is intact.
    std::atomic<std::int64_t> g_post_void_echo{ 0 };
    std::atomic<std::int64_t> g_post_void_retint{ 0 };

    // is_void() for the NEW void scenarios (boundary prims, narrow args, empty/
    // null string, null object, many-arg, const-char* string, static-arg,
    // static-via-instance, repeat, explicit-signature).
    std::atomic<int> g_void_edge_is_void{ -1 };
    std::atomic<int> g_void_narrow_is_void{ -1 };
    std::atomic<int> g_void_empty_str_is_void{ -1 };
    std::atomic<int> g_void_null_str_is_void{ -1 };
    std::atomic<int> g_void_null_obj_is_void{ -1 };
    std::atomic<int> g_void_many_is_void{ -1 };
    std::atomic<int> g_void_cstr_is_void{ -1 };
    std::atomic<int> g_void_static_arg_is_void{ -1 };
    std::atomic<int> g_void_static_via_inst_is_void{ -1 };
    std::atomic<int> g_void_sig_is_void{ -1 };

    // How many times each repeat dispatch reported is_void() (must be all-true).
    constexpr int    k_repeat_calls{ 4 };
    std::atomic<int> g_void_repeat_all_void{ -1 };

    // The OBJECT-arg scenario also exercised through a unique_ptr<wrapper>: the
    // packer's is_unique_ptr branch is distinct from the object_base branch.
    std::atomic<int> g_void_uptr_obj_is_void{ -1 };

    // is_void() on the THROWING void dispatches.  A void method that throws must
    // still report is_void() (the exception is surfaced + cleared by both the
    // call_stub path and the call_jni path before the result decode), so these
    // are HARD-asserted true.
    std::atomic<int> g_void_throw_is_void{ -1 };
    std::atomic<int> g_void_static_throw_is_void{ -1 };
    std::atomic<int> g_void_throw_arg_is_void{ -1 };
    // Both back-to-back throwing dispatches reported void (no poisoning between
    // a throw and the next call on the same detour thread).
    std::atomic<int> g_void_throw_pair_all_void{ -1 };

    // The void-conversion contract: a void value_t static_cast<T>'s to a
    // default-constructed T and as_string()'s to "".  Captured from the very
    // first void instance call.
    std::atomic<int>          g_void_cast_int{ -7 };
    std::atomic<std::int64_t> g_void_cast_double_bits{ -7 };
    std::atomic<int>          g_void_as_string_empty{ -1 };

    // is_void() for the 8-slot-boundary / all-wide / mixed-ref void scenarios.
    std::atomic<int> g_void_eight_is_void{ -1 };
    std::atomic<int> g_void_wide_is_void{ -1 };
    std::atomic<int> g_void_mixed_is_void{ -1 };

    // is_void() for the INHERITED (instance + static, via static_method AND via
    // the instance wrapper) and INTERFACE-DEFAULT void scenarios — a void method
    // NOT declared on the wrapped class itself must still dispatch (super-chain /
    // interface-default resolution) and report void.
    std::atomic<int> g_void_inherited_inst_is_void{ -1 };
    std::atomic<int> g_void_inherited_static_is_void{ -1 };
    std::atomic<int> g_void_inherited_static_via_inst_is_void{ -1 };
    std::atomic<int> g_void_default_is_void{ -1 };

    // is_void() for the interleaved mixed-width and mixed-FP arg void scenarios.
    std::atomic<int> g_void_interleaved_is_void{ -1 };
    std::atomic<int> g_void_mixed_fp_is_void{ -1 };

    // A value-returning call issued IMMEDIATELY after a throwing void call, to
    // prove the surfaced-and-cleared exception left the thread clean enough for
    // the next dispatch to succeed (the strongest non-corruption witness).
    std::atomic<std::int64_t> g_post_throw_echo{ -1 };

    // Sentinel args the native side passes, mirrored by the Java assertions.
    constexpr std::int32_t k_prim_int    = 0x0BADF00D;            // 195948557
    constexpr std::int64_t k_prim_long   = static_cast<std::int64_t>(0x0123456789ABCDEFLL);
    constexpr bool         k_prim_bool   = true;
    constexpr double       k_prim_double = 3.141592653589793;
    // Pure ASCII so length/round-trip is path-independent: the OUTGOING string
    // bytes diverge between call_stub (LATIN1 raw copy) and call_jni (NewStringUTF)
    // only for >= 0x80 bytes, which this string has none of.  Exhaustive unicode
    // String coverage lives in method_call_string; here we only need to prove a
    // String arg reaches a VOID body, so ASCII keeps the assertion exact.
    const std::string      k_string_arg  = "void-string-arg-0123456789";
    constexpr std::int32_t k_echo_arg    = 0x5A5A5A5A;            // 1515870810

    // Boundary primitive sentinels for the SECOND prim recorder (voidEdgePrims):
    // extreme bit patterns so a slot mis-width / sign-extension fault shows up.
    constexpr std::int32_t k_edge_int    = static_cast<std::int32_t>(0x80000000); // INT_MIN
    constexpr std::int64_t k_edge_long   = static_cast<std::int64_t>(0x8000000000000000ULL); // LONG_MIN
    constexpr bool         k_edge_bool   = false;                // the OTHER boolean
    constexpr double       k_edge_double = -0.0;                 // negative zero (sign bit set)

    // Narrow / float sentinels (byte, short, char, float).  Each value sets the
    // high bit of its width so a wrong-width copy would corrupt it.
    constexpr std::int8_t  k_narrow_byte  = static_cast<std::int8_t>(0x80);   // -128
    constexpr std::int16_t k_narrow_short = static_cast<std::int16_t>(0x8001); // -32767
    constexpr std::uint16_t k_narrow_char = static_cast<std::uint16_t>(0xBEEF); // 48879
    constexpr float        k_narrow_float = 2.7182817f;          // e (single precision)

    // const char* string sentinel (distinct content from k_string_arg) routed
    // through the const-char*/string_view packer branch.
    constexpr const char*  k_cstr_arg     = "cstr-void-arg-9876543210";

    // Many-arg sentinels: six ints whose sum and last value are checked.
    constexpr std::int32_t k_many_a       = 11;
    constexpr std::int32_t k_many_b       = 22;
    constexpr std::int32_t k_many_c       = 33;
    constexpr std::int32_t k_many_d       = 44;
    constexpr std::int32_t k_many_e       = 55;
    constexpr std::int32_t k_many_f       = 66;
    constexpr std::int32_t k_many_sum     = k_many_a + k_many_b + k_many_c
                                          + k_many_d + k_many_e + k_many_f; // 231

    // Static-void-with-arg sentinel.
    constexpr std::int32_t k_static_arg   = static_cast<std::int32_t>(0x0FF1CE00); // 267444096

    // Throwing-void-with-arg sentinel (distinct bit pattern, recorded before the
    // body throws — proves the arg crossed the call gate on a throwing dispatch).
    constexpr std::int32_t k_throw_arg    = static_cast<std::int32_t>(0xDEADBEEF); // -559038737

    // Exactly-8-slot sentinels: seven ints (receiver + 7 == params[8] full).
    constexpr std::int32_t k_eight_a      = 1;
    constexpr std::int32_t k_eight_b      = 2;
    constexpr std::int32_t k_eight_c      = 4;
    constexpr std::int32_t k_eight_d      = 8;
    constexpr std::int32_t k_eight_e      = 16;
    constexpr std::int32_t k_eight_f      = 32;
    constexpr std::int32_t k_eight_g      = 64;          // distinct "last" value
    constexpr std::int32_t k_eight_sum    = k_eight_a + k_eight_b + k_eight_c
                                          + k_eight_d + k_eight_e + k_eight_f
                                          + k_eight_g;    // 127

    // All-wide sentinels (long + double, each two interpreter slots).
    constexpr std::int64_t k_wide_long    = static_cast<std::int64_t>(0x7EDCBA9876543210LL);
    constexpr double       k_wide_double  = -2.5e300;     // large negative, sign + exponent set

    // Mixed ref + primitive + String sentinels.
    constexpr std::int32_t k_mixed_int    = static_cast<std::int32_t>(0x13572468); // 324470888
    const std::string      k_mixed_str    = "mixed-ref-void-arg";

    // Interleaved mixed-width sentinels (int, long, int, double): the slot index
    // must advance 1,2,1,2.  Distinct bit patterns so a slot mis-count corrupts a
    // detectable arg.  The SECOND int specifically follows a wide (long) arg.
    constexpr std::int32_t k_inter_int1   = static_cast<std::int32_t>(0x0A0B0C0D); // 168496141
    constexpr std::int64_t k_inter_long   = static_cast<std::int64_t>(0x7766554433221100LL);
    constexpr std::int32_t k_inter_int2   = static_cast<std::int32_t>(0x7EADBEEF); // distinct, +ve high bit clear
    constexpr double       k_inter_double = 1.6180339887498949;  // golden ratio

    // Mixed floating-point sentinels (float + double in one call).
    constexpr float        k_mixed_fp_f   = -1.4142135f;         // -sqrt(2), single
    constexpr double       k_mixed_fp_d   = 2.718281828459045;   // e, double
}

VMHOOK_JVM_MODULE(method_call_return_void)
{
    vmhook::register_class<method_call_void>("vmhook/fixtures/MethodCallVoid");

    g_call_stub_present.store(vmhook::detail::find_call_stub_entry() != nullptr,
                              std::memory_order_relaxed);

    {
        // Hook trigger(); inside the detour current_java_thread is live, so every
        // call() below dispatches a real Java method.  scoped_hook uninstalls
        // when the handle leaves scope.
        auto handle{ vmhook::scoped_hook<method_call_void>(
            "trigger",
            [](vmhook::return_value&,
               const std::unique_ptr<method_call_void>& self,
               std::int32_t /*delta*/)
            {
                g_detour_calls.fetch_add(1, std::memory_order_relaxed);
                g_detour_saw_self.store(self != nullptr, std::memory_order_relaxed);
                if (!self)
                {
                    return;
                }

                // ---- void INSTANCE method: is_void true + side effect ----
                // Also capture the void-conversion contract on this result: a
                // void value_t static_cast<T>'s to a default-constructed T and
                // as_string()'s to "", so a caller that ignores the return gets a
                // well-defined value rather than garbage.
                {
                    auto proxy{ self->get_method("voidBumpInstance") };
                    if (proxy.has_value())
                    {
                        const vmhook::method_proxy::value_t v{ proxy->call() };
                        g_void_inst_is_void.store(v.is_void() ? 1 : 0, std::memory_order_relaxed);
                        g_void_cast_int.store(static_cast<std::int32_t>(v), std::memory_order_relaxed);
                        const double dcast{ static_cast<double>(v) };
                        std::int64_t dbits{ 0 };
                        std::memcpy(&dbits, &dcast, sizeof(dbits));
                        g_void_cast_double_bits.store(dbits, std::memory_order_relaxed);
                        g_void_as_string_empty.store(v.as_string().empty() ? 1 : 0,
                                                     std::memory_order_relaxed);
                    }
                }

                // ---- void STATIC method: is_void true + side effect ----
                {
                    auto proxy{ method_call_void::static_method("voidBumpStatic") };
                    if (proxy.has_value())
                    {
                        const vmhook::method_proxy::value_t v{ proxy->call() };
                        g_void_stat_is_void.store(v.is_void() ? 1 : 0, std::memory_order_relaxed);
                    }
                }

                // ---- void method with PRIMITIVE args (I, J, Z, D) ----
                // is_void must be true AND every arg must arrive verbatim at the
                // Java body (read back from the recorded static fields later).
                {
                    auto proxy{ self->get_method("voidPrimArgs") };
                    if (proxy.has_value())
                    {
                        const vmhook::method_proxy::value_t v{
                            proxy->call(k_prim_int, k_prim_long, k_prim_bool, k_prim_double) };
                        g_void_prim_is_void.store(v.is_void() ? 1 : 0, std::memory_order_relaxed);
                    }
                }

                // ---- void method with a STRING arg ----
                {
                    auto proxy{ self->get_method("voidStringArg") };
                    if (proxy.has_value())
                    {
                        const vmhook::method_proxy::value_t v{ proxy->call(k_string_arg) };
                        g_void_str_is_void.store(v.is_void() ? 1 : 0, std::memory_order_relaxed);
                    }
                }

                // ---- void method with an OBJECT arg ----
                // Pass the live receiver itself (a wrapper -> object_base branch
                // marshals arg.get_instance()).  The body records the object's
                // identityHashCode; we cross-check it against selfIdentity, so
                // this proves the EXACT object reached the void body.
                {
                    auto proxy{ self->get_method("voidObjectArg") };
                    if (proxy.has_value())
                    {
                        const vmhook::method_proxy::value_t v{ proxy->call(*self) };
                        g_void_obj_is_void.store(v.is_void() ? 1 : 0, std::memory_order_relaxed);
                    }
                }

                // ---- void with BOUNDARY primitive args (INT_MIN/LONG_MIN/-0.0) --
                // A second recorder so the existing voidPrimArgs "== 1" stays
                // intact; here the args carry extreme bit patterns to catch a
                // slot-width / sign-extension fault on a no-return dispatch.
                {
                    auto proxy{ self->get_method("voidEdgePrims") };
                    if (proxy.has_value())
                    {
                        const vmhook::method_proxy::value_t v{
                            proxy->call(k_edge_int, k_edge_long, k_edge_bool, k_edge_double) };
                        g_void_edge_is_void.store(v.is_void() ? 1 : 0, std::memory_order_relaxed);
                    }
                }

                // ---- void with NARROW / FLOAT args (byte, short, char, float) --
                // The original prim test only covers I/J/Z/D; these are the 1-slot
                // widths whose extension into an interpreter slot is otherwise
                // unproven for a void dispatch.
                {
                    auto proxy{ self->get_method("voidNarrowArgs") };
                    if (proxy.has_value())
                    {
                        const vmhook::method_proxy::value_t v{
                            proxy->call(k_narrow_byte, k_narrow_short, k_narrow_char, k_narrow_float) };
                        g_void_narrow_is_void.store(v.is_void() ? 1 : 0, std::memory_order_relaxed);
                    }
                }

                // ---- void with an EMPTY String arg ----
                // Degenerate but legal: an empty std::string must marshal to a
                // length-0 java.lang.String (NOT null) and the body must observe 0.
                {
                    auto proxy{ self->get_method("voidEmptyStringArg") };
                    if (proxy.has_value())
                    {
                        const vmhook::method_proxy::value_t v{ proxy->call(std::string{}) };
                        g_void_empty_str_is_void.store(v.is_void() ? 1 : 0, std::memory_order_relaxed);
                    }
                }

                // ---- void with a NULL String arg ----
                // A null const char* routes through the const-char* packer branch
                // and must reach the body as a genuine null reference; the void
                // dispatch must still report is_void() (flaw #3: best-effort arg).
                {
                    auto proxy{ self->get_method("voidNullStringArg") };
                    if (proxy.has_value())
                    {
                        const char* const null_str{ nullptr };
                        const vmhook::method_proxy::value_t v{ proxy->call(null_str) };
                        g_void_null_str_is_void.store(v.is_void() ? 1 : 0, std::memory_order_relaxed);
                    }
                }

                // ---- void with a NULL Object arg ----
                // Passing a null unique_ptr<wrapper> marshals a null reference; the
                // body must see null and the dispatch must still report void.
                {
                    auto proxy{ self->get_method("voidNullObjectArg") };
                    if (proxy.has_value())
                    {
                        std::unique_ptr<method_call_void> null_obj{};
                        const vmhook::method_proxy::value_t v{ proxy->call(std::move(null_obj)) };
                        g_void_null_obj_is_void.store(v.is_void() ? 1 : 0, std::memory_order_relaxed);
                    }
                }

                // ---- void with MANY (six) int args — more of the param block ----
                {
                    auto proxy{ self->get_method("voidManyArgs") };
                    if (proxy.has_value())
                    {
                        const vmhook::method_proxy::value_t v{
                            proxy->call(k_many_a, k_many_b, k_many_c, k_many_d, k_many_e, k_many_f) };
                        g_void_many_is_void.store(v.is_void() ? 1 : 0, std::memory_order_relaxed);
                    }
                }

                // ---- void with a String arg via the const char* packer branch --
                // call(const char*) hits a DIFFERENT pack() branch than
                // call(std::string); prove it also reaches a void body verbatim.
                {
                    auto proxy{ self->get_method("voidStringArgC") };
                    if (proxy.has_value())
                    {
                        const vmhook::method_proxy::value_t v{ proxy->call(k_cstr_arg) };
                        g_void_cstr_is_void.store(v.is_void() ? 1 : 0, std::memory_order_relaxed);
                    }
                }

                // ---- void with an OBJECT arg via unique_ptr<wrapper> ----
                // Distinct packer branch (is_unique_ptr) from the object_base
                // branch the earlier voidObjectArg test exercised; reuses the
                // same recorder fields (objectArg*), so it must again land on the
                // receiver identity.  Wrap the SAME live OOP.
                {
                    auto proxy{ self->get_method("voidObjectArg") };
                    if (proxy.has_value())
                    {
                        auto uptr{ std::make_unique<method_call_void>(self->get_instance()) };
                        const vmhook::method_proxy::value_t v{ proxy->call(std::move(uptr)) };
                        g_void_uptr_obj_is_void.store(v.is_void() ? 1 : 0, std::memory_order_relaxed);
                    }
                }

                // ---- STATIC void WITH an arg (no receiver slot + arg delivered) -
                {
                    auto proxy{ method_call_void::static_method("voidStaticArg") };
                    if (proxy.has_value())
                    {
                        const vmhook::method_proxy::value_t v{ proxy->call(k_static_arg) };
                        g_void_static_arg_is_void.store(v.is_void() ? 1 : 0, std::memory_order_relaxed);
                    }
                }

                // ---- STATIC void resolved THROUGH the instance wrapper ----
                // self->get_method("voidBumpStatic") hands a static Method* back
                // with the receiver still bound in this->object; the library's
                // is_static() clause must omit the receiver slot so the static
                // body runs (and bumps voidStaticHits a SECOND time).
                {
                    auto proxy{ self->get_method("voidBumpStatic") };
                    if (proxy.has_value())
                    {
                        const vmhook::method_proxy::value_t v{ proxy->call() };
                        g_void_static_via_inst_is_void.store(v.is_void() ? 1 : 0, std::memory_order_relaxed);
                    }
                }

                // ---- void INSTANCE via EXPLICIT signature overload ----
                // get_method(name, "()V") + call(): the explicit-descriptor path,
                // and a third bump of voidInstanceHits.
                {
                    auto proxy{ self->get_method("voidBumpInstance", "()V") };
                    if (proxy.has_value())
                    {
                        const vmhook::method_proxy::value_t v{ proxy->call() };
                        g_void_sig_is_void.store(v.is_void() ? 1 : 0, std::memory_order_relaxed);
                    }
                }

                // ---- REPEAT dispatch: call a void method N times, prove each
                // reported void AND the body ran exactly N times (no drop / no
                // double-dispatch, and no poisoning between consecutive voids). --
                {
                    auto proxy{ self->get_method("voidRepeat") };
                    if (proxy.has_value())
                    {
                        int all_void{ 1 };
                        for (int n{ 0 }; n < k_repeat_calls; ++n)
                        {
                            const vmhook::method_proxy::value_t v{ proxy->call() };
                            if (!v.is_void())
                            {
                                all_void = 0;
                            }
                        }
                        g_void_repeat_all_void.store(all_void, std::memory_order_relaxed);
                    }
                }

                // ---- void with EXACTLY 8 SLOTS (receiver + 7 int args) ----
                // The call_stub fast path packs a fixed intptr_t params[8]; an
                // instance call uses slot 0 for the receiver, leaving exactly 7
                // for the args.  Seven ints is the boundary that still fits, so
                // this proves the full param block is delivered with no slot
                // dropped right at the edge of the fixed buffer.
                {
                    auto proxy{ self->get_method("voidEightSlots") };
                    if (proxy.has_value())
                    {
                        const vmhook::method_proxy::value_t v{
                            proxy->call(k_eight_a, k_eight_b, k_eight_c, k_eight_d,
                                        k_eight_e, k_eight_f, k_eight_g) };
                        g_void_eight_is_void.store(v.is_void() ? 1 : 0, std::memory_order_relaxed);
                    }
                }

                // ---- void with ALL-WIDE args (long + double, 2 slots each) ----
                {
                    auto proxy{ self->get_method("voidWideOnly") };
                    if (proxy.has_value())
                    {
                        const vmhook::method_proxy::value_t v{
                            proxy->call(k_wide_long, k_wide_double) };
                        g_void_wide_is_void.store(v.is_void() ? 1 : 0, std::memory_order_relaxed);
                    }
                }

                // ---- void with MIXED ref + primitive + String in one call ----
                // Marshals an object_base receiver, a primitive, and a String
                // together; the body records all three and the module cross-checks
                // each landed (identity == receiver, int verbatim, exact length).
                {
                    auto proxy{ self->get_method("voidMixedRef") };
                    if (proxy.has_value())
                    {
                        const vmhook::method_proxy::value_t v{
                            proxy->call(*self, k_mixed_int, k_mixed_str) };
                        g_void_mixed_is_void.store(v.is_void() ? 1 : 0, std::memory_order_relaxed);
                    }
                }

                // ---- THROWING void (instance): body runs, throws, library
                // surfaces + clears the exception, call() returns void, and the
                // thread is left clean.  Proven THREE ways: is_void() true, the
                // reached-flag set (body ran), and an immediately-following
                // value-returning call still succeeds (no poisoning). ----
                {
                    auto proxy{ self->get_method("voidThrows") };
                    if (proxy.has_value())
                    {
                        const vmhook::method_proxy::value_t v{ proxy->call() };
                        g_void_throw_is_void.store(v.is_void() ? 1 : 0, std::memory_order_relaxed);

                        // Immediately re-dispatch a value returner; if the cleared
                        // exception had poisoned the thread this would misbehave.
                        auto echo{ self->get_method("echoIntAfterVoid") };
                        if (echo.has_value())
                        {
                            const std::int32_t r{ echo->call(k_echo_arg) };
                            g_post_throw_echo.store(r, std::memory_order_relaxed);
                        }
                    }
                }

                // ---- THROWING void (static): static dispatch + thrown exc ----
                {
                    auto proxy{ method_call_void::static_method("voidThrowsStatic") };
                    if (proxy.has_value())
                    {
                        const vmhook::method_proxy::value_t v{ proxy->call() };
                        g_void_static_throw_is_void.store(v.is_void() ? 1 : 0, std::memory_order_relaxed);
                    }
                }

                // ---- THROWING void WITH an arg: the arg must be marshalled into
                // the body (recorded before the throw) even though the dispatch
                // throws.  Proves arg delivery is independent of the throw. ----
                {
                    auto proxy{ self->get_method("voidThrowsArg") };
                    if (proxy.has_value())
                    {
                        const vmhook::method_proxy::value_t v{ proxy->call(k_throw_arg) };
                        g_void_throw_arg_is_void.store(v.is_void() ? 1 : 0, std::memory_order_relaxed);
                    }
                }

                // ---- THROWING void TWICE in a row: a throw on call N must not
                // drop or double call N+1.  Both must report void; the body's
                // reached-count (1 from the single call above + 2 here == 3) is
                // checked in the module body. ----
                {
                    auto proxy{ self->get_method("voidThrows") };
                    if (proxy.has_value())
                    {
                        int all_void{ 1 };
                        for (int n{ 0 }; n < 2; ++n)
                        {
                            const vmhook::method_proxy::value_t v{ proxy->call() };
                            if (!v.is_void())
                            {
                                all_void = 0;
                            }
                        }
                        g_void_throw_pair_all_void.store(all_void, std::memory_order_relaxed);
                    }
                }

                // ---- INHERITED instance void (declared on the base class) ----
                // get_method walks the SUPERCLASS chain, so a void method the
                // wrapped class does NOT itself declare must still resolve and
                // dispatch.  Proves a void call reaches an inherited body.
                {
                    auto proxy{ self->get_method("voidInheritedInstance") };
                    if (proxy.has_value())
                    {
                        const vmhook::method_proxy::value_t v{ proxy->call() };
                        g_void_inherited_inst_is_void.store(v.is_void() ? 1 : 0, std::memory_order_relaxed);
                    }
                }

                // ---- INHERITED static void (declared on the base class) ----
                // static_method() walks the superclass chain (gated on the
                // static-only flag); a static void NOT on the wrapped class must
                // still resolve.  Called both via static_method() (null receiver)
                // and via the instance wrapper (receiver bound but is_static()
                // omits the slot) — both must report void and run the body.
                {
                    auto proxy{ method_call_void::static_method("voidInheritedStatic") };
                    if (proxy.has_value())
                    {
                        const vmhook::method_proxy::value_t v{ proxy->call() };
                        g_void_inherited_static_is_void.store(v.is_void() ? 1 : 0, std::memory_order_relaxed);
                    }
                }
                {
                    auto proxy{ self->get_method("voidInheritedStatic") };
                    if (proxy.has_value())
                    {
                        const vmhook::method_proxy::value_t v{ proxy->call() };
                        g_void_inherited_static_via_inst_is_void.store(v.is_void() ? 1 : 0,
                                                                       std::memory_order_relaxed);
                    }
                }

                // ---- INTERFACE-DEFAULT void (declared on the mixin interface) -
                // get_method's superclass walk misses (the method is on an
                // implemented interface, not the class hierarchy), so the
                // implemented-interface DEFAULT-method fallback must find it; the
                // void `default` body then dispatches through the concrete wrapper.
                {
                    auto proxy{ self->get_method("voidDefaultMethod") };
                    if (proxy.has_value())
                    {
                        const vmhook::method_proxy::value_t v{ proxy->call() };
                        g_void_default_is_void.store(v.is_void() ? 1 : 0, std::memory_order_relaxed);
                    }
                }

                // ---- void with INTERLEAVED mixed-width args (I, J, I, D) ----
                // Slots advance 1,2,1,2: the second int sits AFTER a wide (long)
                // arg and the double AFTER that int, so a running-slot-index fault
                // would corrupt an arg that the adjacent-wide voidWideOnly test
                // cannot catch.  Each arg is read back verbatim later.
                {
                    auto proxy{ self->get_method("voidInterleaved") };
                    if (proxy.has_value())
                    {
                        const vmhook::method_proxy::value_t v{
                            proxy->call(k_inter_int1, k_inter_long, k_inter_int2, k_inter_double) };
                        g_void_interleaved_is_void.store(v.is_void() ? 1 : 0, std::memory_order_relaxed);
                    }
                }

                // ---- void with MIXED floating-point widths (F, D) ----
                // float (1 slot) and double (2 slots) marshalled together; the
                // existing tests cover each FP width only in isolation.
                {
                    auto proxy{ self->get_method("voidMixedFp") };
                    if (proxy.has_value())
                    {
                        const vmhook::method_proxy::value_t v{
                            proxy->call(k_mixed_fp_f, k_mixed_fp_d) };
                        g_void_mixed_fp_is_void.store(v.is_void() ? 1 : 0, std::memory_order_relaxed);
                    }
                }

                // ---- CONTRAST: int returner — is_void() FALSE, value correct ----
                {
                    auto proxy{ self->get_method("retInt") };
                    if (proxy.has_value())
                    {
                        const vmhook::method_proxy::value_t v{ proxy->call() };
                        g_int_is_void.store(v.is_void() ? 1 : 0, std::memory_order_relaxed);
                        g_int_is_string.store(v.is_string() ? 1 : 0, std::memory_order_relaxed);
                        g_int_ret_value.store(static_cast<std::int32_t>(v), std::memory_order_relaxed);
                    }
                }

                // ---- NON-CORRUPTION: value-returning calls AFTER the void calls.
                // A void dispatch must leave the thread / call gate intact, so a
                // subsequent echo and constant return must still be correct.
                {
                    auto echo{ self->get_method("echoIntAfterVoid") };
                    if (echo.has_value())
                    {
                        const std::int32_t r{ echo->call(k_echo_arg) };
                        g_post_void_echo.store(r, std::memory_order_relaxed);
                    }
                    auto ret{ self->get_method("retInt") };
                    if (ret.has_value())
                    {
                        const std::int32_t r{ ret->call() };
                        g_post_void_retint.store(r, std::memory_order_relaxed);
                    }
                }
            }) };

        ctx.check("mcrv_hook_installed", handle.installed());

        const bool done{ ctx.run_probe(
            [](bool v) { method_call_void::set_go(v); },
            []() { return method_call_void::get_done(); }) };

        ctx.check("mcrv_probe_completed", done);
        ctx.check("mcrv_detour_fired", g_detour_calls.load(std::memory_order_relaxed) >= 1);
        ctx.check("mcrv_detour_saw_self", g_detour_saw_self.load(std::memory_order_relaxed));

        ctx.record(std::string{ "[INFO] method_call_return_void dispatch path: " }
                   + (g_call_stub_present.load(std::memory_order_relaxed)
                          ? "call_stub fast path (StubRoutines::_call_stub_entry present)"
                          : "JNI fallback (CallVoidMethodA / CallStaticVoidMethodA)"));

        // =====================================================================
        //  void INSTANCE method
        // =====================================================================
        // The returned value_t must report void...
        ctx.check("mcrv_void_instance_is_void", g_void_inst_is_void.load() == 1);
        // ...and the body must have actually run.  voidInstanceHits is bumped by
        // TWO instance dispatches: the name-only get_method("voidBumpInstance")
        // above and the explicit-signature get_method("voidBumpInstance","()V")
        // below, so the count must be EXACTLY 2 (proves both ran, neither
        // doubled, and the two get_method paths land on the SAME body).
        ctx.check("mcrv_void_instance_side_effect",
                  method_call_void::void_instance_hits() == 2);

        // =====================================================================
        //  void STATIC method (CallStaticVoidMethodA / static call_stub slot)
        // =====================================================================
        ctx.check("mcrv_void_static_is_void", g_void_stat_is_void.load() == 1);
        // voidStaticHits is bumped by TWO static dispatches: static_method()
        // (null receiver) and the static-via-instance get_method() below (the
        // receiver is bound but is_static() must omit it).  Exactly 2.
        ctx.check("mcrv_void_static_side_effect",
                  method_call_void::void_static_hits() == 2);

        // =====================================================================
        //  void method with PRIMITIVE args (I, J, Z, D) — args delivered
        // =====================================================================
        ctx.check("mcrv_void_prim_is_void", g_void_prim_is_void.load() == 1);
        ctx.check("mcrv_void_prim_called", method_call_void::prim_args_called());
        // Each argument must have reached the void body verbatim — this is the
        // ONLY proof that arguments are marshalled for a no-return dispatch.
        ctx.check("mcrv_void_prim_arg_int",
                  method_call_void::prim_arg_int() == k_prim_int);
        ctx.check("mcrv_void_prim_arg_long",
                  method_call_void::prim_arg_long() == k_prim_long);
        ctx.check("mcrv_void_prim_arg_bool",
                  method_call_void::prim_arg_bool() == k_prim_bool);
        ctx.check("mcrv_void_prim_arg_double",
                  method_call_void::prim_arg_double() == k_prim_double);

        // =====================================================================
        //  void method with a STRING arg
        // =====================================================================
        ctx.check("mcrv_void_string_is_void", g_void_str_is_void.load() == 1);
        ctx.check("mcrv_void_string_called", method_call_void::string_arg_called());
        // The String must have been delivered non-null with the exact length.
        // (k_string_arg is pure ASCII, so its String.length() equals its byte
        // count identically on the call_stub and call_jni argument paths.)
        ctx.check("mcrv_void_string_non_null", method_call_void::string_arg_len() >= 0);
        ctx.check("mcrv_void_string_len_exact",
                  static_cast<std::size_t>(method_call_void::string_arg_len())
                      == k_string_arg.size());
        // And the round-tripped String value must match byte-for-byte (ASCII).
        ctx.check("mcrv_void_string_value_exact",
                  method_call_void::string_arg() == k_string_arg);

        // =====================================================================
        //  void method with an OBJECT arg — exact identity delivered
        // =====================================================================
        ctx.check("mcrv_void_object_is_void", g_void_obj_is_void.load() == 1);
        ctx.check("mcrv_void_object_called", method_call_void::object_arg_called());
        ctx.check("mcrv_void_object_non_null", method_call_void::object_arg_non_null());
        // The body's identityHashCode of the received object must equal the
        // receiver's published identity — proves the EXACT object was passed.
        ctx.check("mcrv_void_object_identity_matches_receiver",
                  method_call_void::object_arg_identity() != 0
                  && method_call_void::object_arg_identity()
                         == method_call_void::self_identity());
        // The SAME void Object body was also driven through a unique_ptr<wrapper>
        // (the packer's is_unique_ptr branch, distinct from object_base); it must
        // also report void.  Identity is already pinned above (same OOP wrapped).
        ctx.check("mcrv_void_object_uptr_is_void", g_void_uptr_obj_is_void.load() == 1);

        // =====================================================================
        //  void method with BOUNDARY primitive args (INT_MIN/LONG_MIN/-0.0)
        // =====================================================================
        ctx.check("mcrv_void_edge_is_void", g_void_edge_is_void.load() == 1);
        ctx.check("mcrv_void_edge_called", method_call_void::edge_prims_called());
        ctx.check("mcrv_void_edge_arg_int",
                  method_call_void::edge_prim_int() == k_edge_int);
        ctx.check("mcrv_void_edge_arg_long",
                  method_call_void::edge_prim_long() == k_edge_long);
        ctx.check("mcrv_void_edge_arg_bool",
                  method_call_void::edge_prim_bool() == k_edge_bool);
        // -0.0 must round-trip with its sign bit intact (bit-exact, not == 0.0
        // which would also accept +0.0): compare the raw bit patterns.
        {
            const double got{ method_call_void::edge_prim_double() };
            std::uint64_t got_bits{ 0 };
            std::uint64_t want_bits{ 0 };
            const double want{ k_edge_double };
            std::memcpy(&got_bits, &got, sizeof(got_bits));
            std::memcpy(&want_bits, &want, sizeof(want_bits));
            ctx.check("mcrv_void_edge_arg_double_neg_zero", got_bits == want_bits);
        }

        // =====================================================================
        //  void method with NARROW / FLOAT args (byte, short, char, float)
        // =====================================================================
        ctx.check("mcrv_void_narrow_is_void", g_void_narrow_is_void.load() == 1);
        ctx.check("mcrv_void_narrow_called", method_call_void::narrow_args_called());
        ctx.check("mcrv_void_narrow_arg_byte",
                  method_call_void::narrow_arg_byte() == k_narrow_byte);
        ctx.check("mcrv_void_narrow_arg_short",
                  method_call_void::narrow_arg_short() == k_narrow_short);
        ctx.check("mcrv_void_narrow_arg_char",
                  method_call_void::narrow_arg_char() == k_narrow_char);
        ctx.check("mcrv_void_narrow_arg_float",
                  method_call_void::narrow_arg_float() == k_narrow_float);

        // =====================================================================
        //  void method with an EMPTY String arg
        // =====================================================================
        ctx.check("mcrv_void_empty_string_is_void", g_void_empty_str_is_void.load() == 1);
        ctx.check("mcrv_void_empty_string_called", method_call_void::empty_string_called());
        // An empty std::string must marshal to a non-null length-0 String, NOT
        // a null reference (len == -1 would mean the body saw null).
        ctx.check("mcrv_void_empty_string_len_zero",
                  method_call_void::empty_string_len() == 0);

        // =====================================================================
        //  void method with a NULL const char* String arg
        // =====================================================================
        // is_void() and "body ran" are path-INDEPENDENT and hard-asserted.  Note
        // a null `const char*` arg is the ONE String case where the two dispatch
        // backends genuinely DIVERGE: call_jni (JDK 21+) marshals nullptr -> Java
        // null, while the call_stub path (JDK 8/11/17) routes it through
        // make_java_string(empty) -> a non-null length-0 String.  So whether the
        // body observed null is JDK-variant and is recorded as [INFO], never
        // hard-asserted (the genuinely-null reference test below uses a null
        // unique_ptr<wrapper>, which BOTH paths marshal as a real null).
        ctx.check("mcrv_void_null_string_is_void", g_void_null_str_is_void.load() == 1);
        ctx.check("mcrv_void_null_string_called", method_call_void::null_string_called());
        ctx.record(std::string{ "[INFO] void null-const-char* arg observed as " }
                   + (method_call_void::null_string_was_null()
                          ? "Java null (call_jni nullptr convention)"
                          : "empty String (call_stub make_java_string path)"));

        // =====================================================================
        //  void method with a NULL Object arg
        // =====================================================================
        ctx.check("mcrv_void_null_object_is_void", g_void_null_obj_is_void.load() == 1);
        ctx.check("mcrv_void_null_object_called", method_call_void::null_object_called());
        ctx.check("mcrv_void_null_object_was_null", method_call_void::null_object_was_null());

        // =====================================================================
        //  void method with MANY (six) int args
        // =====================================================================
        ctx.check("mcrv_void_many_is_void", g_void_many_is_void.load() == 1);
        ctx.check("mcrv_void_many_called", method_call_void::many_args_called());
        ctx.check("mcrv_void_many_sum", method_call_void::many_args_sum() == k_many_sum);
        ctx.check("mcrv_void_many_last", method_call_void::many_args_last() == k_many_f);

        // =====================================================================
        //  void method with a String arg via the const char* packer branch
        // =====================================================================
        ctx.check("mcrv_void_cstr_is_void", g_void_cstr_is_void.load() == 1);
        ctx.check("mcrv_void_cstr_called", method_call_void::cstr_arg_called());
        ctx.check("mcrv_void_cstr_len_exact",
                  static_cast<std::size_t>(method_call_void::cstr_arg_len())
                      == std::string_view{ k_cstr_arg }.size());
        ctx.check("mcrv_void_cstr_value_exact",
                  method_call_void::cstr_arg() == std::string{ k_cstr_arg });

        // =====================================================================
        //  STATIC void WITH a primitive arg (no receiver + arg delivered)
        // =====================================================================
        ctx.check("mcrv_void_static_arg_is_void", g_void_static_arg_is_void.load() == 1);
        ctx.check("mcrv_void_static_arg_called", method_call_void::static_arg_called());
        ctx.check("mcrv_void_static_arg_value",
                  method_call_void::static_arg_int() == k_static_arg);

        // =====================================================================
        //  STATIC void resolved THROUGH the instance wrapper, and an
        //  EXPLICIT-signature instance void — both must report void.  (Their
        //  side effects are folded into the == 2 hit-count assertions above.)
        // =====================================================================
        ctx.check("mcrv_void_static_via_instance_is_void",
                  g_void_static_via_inst_is_void.load() == 1);
        ctx.check("mcrv_void_explicit_signature_is_void",
                  g_void_sig_is_void.load() == 1);

        // =====================================================================
        //  REPEAT dispatch — every call reported void AND the body ran exactly
        //  k_repeat_calls times (no drop, no double-dispatch, no poisoning).
        // =====================================================================
        ctx.check("mcrv_void_repeat_all_void", g_void_repeat_all_void.load() == 1);
        ctx.check("mcrv_void_repeat_count_exact",
                  method_call_void::void_repeat_hits() == k_repeat_calls);

        // =====================================================================
        //  INHERITED instance void (declared on MethodCallVoidBase)
        // =====================================================================
        // A void method the wrapped class does NOT itself declare must still
        // dispatch through the subclass wrapper via the superclass-chain walk.
        ctx.check("mcrv_void_inherited_instance_is_void",
                  g_void_inherited_inst_is_void.load() == 1);
        // Side effect: >= 1 (not an exact count — a probe re-entry would bump it
        // again, and BANKED RULE forbids hard-asserting an exact fire count).
        ctx.check("mcrv_void_inherited_instance_side_effect",
                  method_call_void::void_inherited_hits() >= 1);

        // =====================================================================
        //  INHERITED static void (declared on MethodCallVoidBase)
        // =====================================================================
        // Resolved via static_method() (null receiver) AND via the instance
        // wrapper (receiver bound, is_static() omits the slot) — both report void.
        ctx.check("mcrv_void_inherited_static_is_void",
                  g_void_inherited_static_is_void.load() == 1);
        ctx.check("mcrv_void_inherited_static_via_instance_is_void",
                  g_void_inherited_static_via_inst_is_void.load() == 1);
        // Two static dispatches ran, so >= 2 here; assert the weaker >= 1 to stay
        // robust to probe re-entry per the banked exact-count rule.
        ctx.check("mcrv_void_inherited_static_side_effect",
                  method_call_void::void_inherited_static_hits() >= 1);

        // =====================================================================
        //  INTERFACE-DEFAULT void (declared on MethodCallVoidMixin)
        // =====================================================================
        // Reached via get_method's implemented-interface DEFAULT-method fallback
        // (after the superclass chain misses); the void default body dispatches.
        ctx.check("mcrv_void_default_is_void", g_void_default_is_void.load() == 1);
        ctx.check("mcrv_void_default_side_effect",
                  method_call_void::void_default_hits() >= 1);

        // =====================================================================
        //  void with INTERLEAVED mixed-width args (int, long, int, double)
        // =====================================================================
        ctx.check("mcrv_void_interleaved_is_void", g_void_interleaved_is_void.load() == 1);
        ctx.check("mcrv_void_interleaved_called", method_call_void::interleaved_called());
        // Each arg verbatim — the second int (after the wide long) and the double
        // (after that int) are the slots a running-index fault would corrupt.
        ctx.check("mcrv_void_interleaved_int1",
                  method_call_void::interleaved_int1() == k_inter_int1);
        ctx.check("mcrv_void_interleaved_long",
                  method_call_void::interleaved_long() == k_inter_long);
        ctx.check("mcrv_void_interleaved_int2",
                  method_call_void::interleaved_int2() == k_inter_int2);
        ctx.check("mcrv_void_interleaved_double",
                  method_call_void::interleaved_double() == k_inter_double);

        // =====================================================================
        //  void with MIXED floating-point widths (float + double)
        // =====================================================================
        ctx.check("mcrv_void_mixed_fp_is_void", g_void_mixed_fp_is_void.load() == 1);
        ctx.check("mcrv_void_mixed_fp_called", method_call_void::mixed_fp_called());
        ctx.check("mcrv_void_mixed_fp_float",
                  method_call_void::mixed_fp_float() == k_mixed_fp_f);
        ctx.check("mcrv_void_mixed_fp_double",
                  method_call_void::mixed_fp_double() == k_mixed_fp_d);

        // =====================================================================
        //  CONTRAST: an int return must NOT be reported as void or string
        // =====================================================================
        ctx.check("mcrv_int_return_is_not_void", g_int_is_void.load() == 0);
        ctx.check("mcrv_int_return_is_not_string", g_int_is_string.load() == 0);
        // And it must decode to the real value (proves "not void" isn't a fluke
        // from a failed call that also yields monostate -> would read as 0).
        ctx.check("mcrv_int_return_value_correct", g_int_ret_value.load() == 1337);

        // =====================================================================
        //  void RETURN-VALUE CONVERSION CONTRACT (a void result is well-defined)
        // =====================================================================
        // The void value_t must convert to a default-constructed target: an int
        // cast yields 0, a double cast yields +0.0 (bit-exact, so a stray sign
        // or payload bit would fail), and as_string() yields "".  This is what
        // makes "ignore the return of a void call" safe rather than reading
        // garbage out of an unread result slot.
        ctx.check("mcrv_void_cast_int_zero", g_void_cast_int.load() == 0);
        {
            const double pos_zero{ 0.0 };
            std::int64_t want_bits{ 0 };
            std::memcpy(&want_bits, &pos_zero, sizeof(want_bits));
            ctx.check("mcrv_void_cast_double_zero_bits",
                      g_void_cast_double_bits.load() == want_bits);
        }
        ctx.check("mcrv_void_as_string_empty", g_void_as_string_empty.load() == 1);

        // =====================================================================
        //  void with EXACTLY 8 SLOTS (receiver + 7 ints) — full param block
        // =====================================================================
        ctx.check("mcrv_void_eight_is_void", g_void_eight_is_void.load() == 1);
        ctx.check("mcrv_void_eight_called", method_call_void::eight_slot_called());
        // The sum proves NO slot was dropped; the distinct last value proves the
        // 7th (final) argument slot specifically was delivered (the boundary one).
        ctx.check("mcrv_void_eight_sum", method_call_void::eight_slot_sum() == k_eight_sum);
        ctx.check("mcrv_void_eight_last", method_call_void::eight_slot_last() == k_eight_g);

        // =====================================================================
        //  void with ALL-WIDE args (long + double, two 2-slot args)
        // =====================================================================
        ctx.check("mcrv_void_wide_is_void", g_void_wide_is_void.load() == 1);
        ctx.check("mcrv_void_wide_called", method_call_void::wide_only_called());
        ctx.check("mcrv_void_wide_long", method_call_void::wide_only_long() == k_wide_long);
        ctx.check("mcrv_void_wide_double", method_call_void::wide_only_double() == k_wide_double);

        // =====================================================================
        //  void with MIXED ref + primitive + String in one call
        // =====================================================================
        ctx.check("mcrv_void_mixed_is_void", g_void_mixed_is_void.load() == 1);
        ctx.check("mcrv_void_mixed_called", method_call_void::mixed_ref_called());
        // The reference arg must be the EXACT receiver (identity match), the
        // primitive verbatim, and the String the exact length — proving all three
        // distinct marshalling branches landed in a single void dispatch.
        ctx.check("mcrv_void_mixed_obj_identity_matches_receiver",
                  method_call_void::mixed_ref_obj_non_null()
                  && method_call_void::mixed_ref_obj_identity() != 0
                  && method_call_void::mixed_ref_obj_identity()
                         == method_call_void::self_identity());
        ctx.check("mcrv_void_mixed_int", method_call_void::mixed_ref_int() == k_mixed_int);
        ctx.check("mcrv_void_mixed_str_len",
                  static_cast<std::size_t>(method_call_void::mixed_ref_str_len())
                      == k_mixed_str.size());

        // =====================================================================
        //  THROWING void — exception propagation / surfacing across the call
        // =====================================================================
        // A void method that throws still returns a void value_t (the library
        // surfaces + clears the pending exception on BOTH dispatch paths before
        // the result decode), and the Java body actually ran up to the throw.
        ctx.check("mcrv_void_throw_is_void", g_void_throw_is_void.load() == 1);
        ctx.check("mcrv_void_throw_body_ran", method_call_void::void_throw_reached());
        // The strongest non-corruption witness: a value-returning call issued
        // IMMEDIATELY after the throwing void call still delivered correctly, so
        // the cleared exception left the thread clean (no "method seemed to run
        // but nothing happened" poisoning).
        ctx.check("mcrv_post_throw_echo_value", g_post_throw_echo.load() == k_echo_arg);

        // Static throwing void: the static dispatch path also returns void and
        // ran its body.
        ctx.check("mcrv_void_static_throw_is_void", g_void_static_throw_is_void.load() == 1);
        ctx.check("mcrv_void_static_throw_body_ran",
                  method_call_void::void_static_throw_reached());

        // Throwing void WITH an arg: the argument crossed the call gate into the
        // body (recorded before the throw) even though the dispatch threw.
        ctx.check("mcrv_void_throw_arg_is_void", g_void_throw_arg_is_void.load() == 1);
        ctx.check("mcrv_void_throw_arg_body_ran", method_call_void::void_throw_arg_reached());
        ctx.check("mcrv_void_throw_arg_value",
                  method_call_void::void_throw_arg_value() == k_throw_arg);

        // Two throwing dispatches in a row both reported void, and the body ran
        // exactly THREE times total (1 single + 2 pair) — a throw on one call
        // neither dropped nor doubled the next.
        ctx.check("mcrv_void_throw_pair_all_void", g_void_throw_pair_all_void.load() == 1);
        ctx.check("mcrv_void_throw_count_exact",
                  method_call_void::void_throw_reached_count() == 3);

        // =====================================================================
        //  NON-CORRUPTION: value-returning calls after the void calls
        // =====================================================================
        // A value-returning call performed AFTER all the void dispatches must
        // still deliver its argument and return value intact.
        ctx.check("mcrv_post_void_echo_value", g_post_void_echo.load() == k_echo_arg);
        ctx.check("mcrv_post_void_echo_side_effect",
                  method_call_void::last_echo_arg() == k_echo_arg);
        ctx.check("mcrv_post_void_retint_value", g_post_void_retint.load() == 1337);
    }
}
