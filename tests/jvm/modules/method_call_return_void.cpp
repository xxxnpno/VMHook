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
                {
                    auto proxy{ self->get_method("voidBumpInstance") };
                    if (proxy.has_value())
                    {
                        const vmhook::method_proxy::value_t v{ proxy->call() };
                        g_void_inst_is_void.store(v.is_void() ? 1 : 0, std::memory_order_relaxed);
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
        //  CONTRAST: an int return must NOT be reported as void or string
        // =====================================================================
        ctx.check("mcrv_int_return_is_not_void", g_int_is_void.load() == 0);
        ctx.check("mcrv_int_return_is_not_string", g_int_is_string.load() == 0);
        // And it must decode to the real value (proves "not void" isn't a fluke
        // from a failed call that also yields monostate -> would read as 0).
        ctx.check("mcrv_int_return_value_correct", g_int_ret_value.load() == 1337);

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
