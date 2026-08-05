// repeat_call_stability — exhaustive JVM test for vmhook's JNI LOCAL-REFERENCE
// discipline: it proves that vmhook does NOT leak JNI local references on the
// paths that create them, when those paths run from a long-lived attached detour
// thread in a tight loop FAR past HotSpot's default 16-entry local-ref table.
//
// ── WHY THIS MATTERS (audit:jni_delete_local_ref_table_slot_23.md +
//    method_proxy_call_jni_local_ref_leaks.md) ────────────────────────────────
// vmhook detour threads attach to the JVM and STAY attached — they never push or
// pop a JNI frame, so there is no implicit per-call teardown that would reclaim
// local references. Every operation below allocates one (or two) JNI local refs
// that vmhook MUST release via JNIEnv::DeleteLocalRef (vmhook.hpp
// jni_delete_local_ref, slot 23) or the table — capacity 16 by default — fills
// up. Once full, HotSpot logs "JNI local reference table overflow" and the
// allocating JNI call (NewStringUTF / Call(Static)?ObjectMethodA / FindClass)
// starts returning null, so the OBSERVABLE symptom of a leak is: String returns
// come back "", reference returns become null, and injected String args stop
// reaching the body. We drive each path 100+ times and assert the result stays
// correct on every iteration — the behavioural proof the refs are released.
//
// The local-ref-creating paths exercised here:
//   * call() String return     -> CallObjectMethodA jstring local ref, decoded
//                                  and released (String-RETURN loop).
//   * call() fresh String       -> a brand-new heap String each call (no
//                                  constant-pool reuse to mask a leak).
//   * call(String arg)          -> NewStringUTF local ref (arg) + the returned
//                                  jstring local ref = TWO refs/iter (echo loop).
//   * call() Object/array return -> CallObjectMethodA local ref on the 'L'/'['
//                                  arm, released after decode.
//   * STATIC dispatch           -> FindClass jclass local ref (+ the static
//                                  CallStaticObjectMethodA result ref).
//   * INSTANCE dispatch         -> GetObjectClass jclass local ref.
//   * return_value::set_arg(String) -> NewStringUTF local ref + DeleteLocalRef
//                                  (vmhook.hpp return_value::set_arg, the v0.4.x
//                                  leak fix). Driven by hooking inject(String)
//                                  and letting the probe dispatch it in a loop.
//
// SAFETY: the loops are BOUNDED (a few hundred iterations). If a leak existed it
// would surface as the benign table-overflow warning + degraded return values —
// which THIS module catches as a [FAIL] via the stability assertions — never an
// access violation. We never unbound-spin and never take the JVM down.
//
// call() must run where current_java_thread is set, i.e. inside a hook detour, so
// we hook RepeatCallProbe.trigger() and run all the call()/return loops in that
// detour against the live receiver + the static methods. The set_arg(String)
// loop is driven separately by hooking inject(String): the probe dispatches
// inject(...) RepeatCallProbe.INJECT_ITERATIONS times, the detour injects a fresh
// String each time, and the Java body records what it received.
//
// This module does NOT modify vmhook.hpp. If it ever observed a real unreleased
// ref it would FAIL the stability assertions (characterizing the leak); see the
// [INFO] breadcrumbs for which path degraded.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace
{
    // Wrapper for vmhook.fixtures.RepeatCallProbe. Instance helpers drive the
    // reference-returning call() paths; static helpers exercise the
    // FindClass-based static jclass resolution.
    class jni_local_ref : public vmhook::object<jni_local_ref>
    {
    public:
        explicit jni_local_ref(vmhook::oop_t instance) noexcept
            : vmhook::object<jni_local_ref>{ instance }
        {
        }

        // go / done handshake + side-effect readback.
        static auto set_go(bool v) -> void              { static_field("go")->set(v); }
        static auto get_done() -> bool                  { bool x = static_field("done")->get(); return x; }
        static auto get_trigger_count() -> std::int32_t { std::int32_t x = static_field("triggerCount")->get(); return x; }

        // inject() loop observables (set_arg(String) path).
        static auto get_inject_count() -> std::int32_t       { std::int32_t x = static_field("injectCount")->get(); return x; }
        static auto get_inject_body_ran() -> bool            { bool x = static_field("injectBodyRan")->get(); return x; }
        static auto get_inject_len_seen() -> std::int32_t    { std::int32_t x = static_field("injectLenSeen")->get(); return x; }
        static auto get_inject_seen() -> std::string         { std::string x = static_field("injectSeen")->get(); return x; }
        static auto get_inject_nonempty_count() -> std::int32_t { std::int32_t x = static_field("injectNonEmptyCount")->get(); return x; }
        static auto get_inject_iterations() -> std::int32_t  { std::int32_t x = static_field("INJECT_ITERATIONS")->get(); return x; }

        // injectMixed() loop observables (set_arg union-aliasing path).
        static auto get_inject_mixed_count() -> std::int32_t     { std::int32_t x = static_field("injectMixedCount")->get(); return x; }
        static auto get_inject_mixed_seen() -> std::string       { std::string x = static_field("injectMixedSeen")->get(); return x; }
        static auto get_inject_mixed_int_seen() -> std::int32_t  { std::int32_t x = static_field("injectMixedIntSeen")->get(); return x; }
        static auto get_inject_mixed_ok_count() -> std::int32_t  { std::int32_t x = static_field("injectMixedOkCount")->get(); return x; }
        static auto get_inject_mixed_int() -> std::int32_t       { std::int32_t x = static_field("INJECT_MIXED_INT")->get(); return x; }
    };

    // ── Captured observations: the detour writes, the module body reads. ──────
    std::atomic<int>  g_detour_calls{ 0 };
    std::atomic<bool> g_detour_saw_self{ false };
    std::atomic<bool> g_call_stub_present{ false };
    std::atomic<std::uintptr_t> g_receiver_instance{ 0 };

    // String-RETURN loop (call() -> String, CallObjectMethodA + decode + release).
    std::atomic<int> g_str_loop_iters{ 0 };
    std::atomic<int> g_str_loop_distinct{ -1 };   // distinct values seen; 1 == leak-free
    std::atomic<int> g_str_loop_empties{ -1 };    // # of iterations that came back ""

    // FRESH-String-RETURN loop (a new heap String each call; harshest leak case).
    std::atomic<int> g_fresh_loop_iters{ 0 };
    std::atomic<int> g_fresh_loop_mismatches{ -1 };

    // String-ARG echo loop (NewStringUTF arg ref + returned String ref = 2/iter).
    std::atomic<int> g_echo_loop_iters{ 0 };
    std::atomic<int> g_echo_loop_mismatches{ -1 };

    // Object-RETURN loop (call() -> non-String reference; 'L' arm release).
    std::atomic<int> g_obj_loop_iters{ 0 };
    std::atomic<int> g_obj_loop_nonnull{ -1 };     // # iterations the wrapper decoded non-null
    std::atomic<int> g_obj_loop_identity_ok{ -1 }; // # iterations OOP == receiver (call_stub path)

    // Array-RETURN loop (call() -> '[' reference; 'L'/'[' arm release).
    std::atomic<int> g_arr_loop_iters{ 0 };
    std::atomic<int> g_arr_loop_nonnull{ -1 };

    // FRESH typed-array-RETURN loop (makeBytes '[B', makeChars '[C', makeObjArray
    // '[L...;'): a BRAND-NEW heap array of each element kind every iteration, so
    // no array pooling can mask a leaked CallObjectMethodA ref on the '[' arm.
    // Every iteration must decode all three to a non-null oop.
    std::atomic<int> g_typedarr_loop_iters{ 0 };
    std::atomic<int> g_typedarr_loop_failures{ -1 };  // # iters any of the 3 came back null

    // STATIC String-RETURN loop (FindClass jclass ref + CallStaticObjectMethodA).
    std::atomic<int> g_sstr_loop_iters{ 0 };
    std::atomic<int> g_sstr_loop_distinct{ -1 };

    // STATIC Object-RETURN loop (FindClass ref + CallStaticObjectMethodA result).
    std::atomic<int> g_sobj_loop_iters{ 0 };
    std::atomic<int> g_sobj_loop_nonnull{ -1 };

    // INTERLEAVED loop: one of every path per iteration, so the table sees the
    // worst-case mix of simultaneously-live refs before each release fires.
    std::atomic<int> g_mix_loop_iters{ 0 };
    std::atomic<int> g_mix_loop_failures{ -1 };

    // PRIMITIVE-ARG loop (echoInt): the int's jvalue cell aliases .l as a wild
    // pointer; needs_release stays false so it is NEVER handed to DeleteLocalRef.
    // No local ref is created at all, so a stable echo is the union-aliasing proof.
    std::atomic<int> g_int_loop_iters{ 0 };
    std::atomic<int> g_int_loop_mismatches{ -1 };

    // TWO-WORD primitive-arg loop (echoLong): .j bits alias .l as a wild pointer.
    std::atomic<int> g_long_loop_iters{ 0 };
    std::atomic<int> g_long_loop_mismatches{ -1 };

    // BOOLEAN-arg loop (echoBool): .z == 1 aliases .l as 0x1 (low non-null ptr).
    std::atomic<int> g_bool_loop_iters{ 0 };
    std::atomic<int> g_bool_loop_mismatches{ -1 };

    // MIXED-arg loop (echoMixed: String slot released, int slot NOT): only the
    // String slot's NewStringUTF ref is released; the primitive slot's tag stays
    // false. Both args must arrive intact every iteration.
    std::atomic<int> g_mixed_loop_iters{ 0 };
    std::atomic<int> g_mixed_loop_mismatches{ -1 };

    // TWO-String-arg loop (concat: 2 NewStringUTF refs + 1 result = 3 refs/iter).
    std::atomic<int> g_concat_loop_iters{ 0 };
    std::atomic<int> g_concat_loop_mismatches{ -1 };

    // OBJECT-arg loop (echoObj: synthetic handle arg must NOT be released + the
    // Object-return ref must be). Decoded non-null on every iteration.
    std::atomic<int> g_objarg_loop_iters{ 0 };
    std::atomic<int> g_objarg_loop_nonnull{ -1 };

    // NULL-String-arg loop (nullArgLen: null C string -> Java null, no ref, no
    // release). Every iteration must return -1 (the body saw null).
    std::atomic<int> g_nullarg_loop_iters{ 0 };
    std::atomic<int> g_nullarg_loop_mismatches{ -1 };

    // EMPTY-String-arg loop (echo with ""): a non-null empty arg becomes a real
    // empty Java String (a NewStringUTF ref to release), distinct from Java null.
    std::atomic<int> g_emptyarg_loop_iters{ 0 };
    std::atomic<int> g_emptyarg_loop_mismatches{ -1 };

    // STATIC String-arg echo loop (staticEcho: FindClass ref + NewStringUTF arg
    // ref + result ref = 3 refs/iter on the static path).
    std::atomic<int> g_secho_loop_iters{ 0 };
    std::atomic<int> g_secho_loop_mismatches{ -1 };

    // NATIVE make_java_string loop (vmhook::make_java_string -> the internal
    // jni_new_string_utf16_local NewString local ref + DeleteLocalRef on the JNI
    // path). Driven directly from the detour (no Java call()): each iteration
    // allocates a brand-new String oop and decodes it back. Stable byte-exact
    // round-trip across the loop == the internal local ref is released each time.
    std::atomic<int> g_mkstr_loop_iters{ 0 };
    std::atomic<int> g_mkstr_loop_mismatches{ -1 };   // # iters whose decode != payload
    std::atomic<int> g_mkstr_loop_nonnull{ -1 };      // # iters that decoded a valid oop

    // NATIVE make_java_array loop (vmhook::make_java_array -> the JNI fallback
    // New<Type>Array local ref + DeleteLocalRef on the '[' path). Driven directly
    // from the detour: a fresh primitive array oop each iteration, length-checked.
    std::atomic<int> g_mkarr_loop_iters{ 0 };
    std::atomic<int> g_mkarr_loop_badlen{ -1 };       // # iters whose array_length != requested
    std::atomic<int> g_mkarr_loop_nonnull{ -1 };      // # iters that allocated a valid oop

    // NATIVE find_class MISS loop (vmhook::find_class on an absent class): the
    // miss path is NOT cached, so every iteration re-walks the graph and drives
    // jni_find_class_with_context_loader, which creates + DeleteLocalRefs FindClass
    // / NewStringUTF handles AND clears the pending ClassNotFound exception. A
    // missing release / un-cleared exception would starve the table / poison the
    // next JNI call; every iteration must still return null with the JVM healthy.
    std::atomic<int> g_fcmiss_loop_iters{ 0 };
    std::atomic<int> g_fcmiss_loop_nonnull{ -1 };     // # iters that wrongly returned non-null

    // NATIVE find_class HIT loop (vmhook::find_class on a present class): the
    // first hit resolves + caches; subsequent hits return the SAME cached klass.
    // The FindClass / GetObjectClass local refs of the resolution must be released
    // and the cached pointer must stay stable across the whole loop.
    std::atomic<int> g_fchit_loop_iters{ 0 };
    std::atomic<int> g_fchit_loop_distinct{ -1 };     // distinct klass ptrs seen; 1 == stable
    std::atomic<int> g_fchit_loop_null{ -1 };         // # iters that returned null (must be 0)

    // Post-loop sanity: a single call AFTER every loop still works.
    std::atomic<bool> g_post_loop_str_ok{ false };

    // Post-NATIVE-loop sanity: a fresh make_java_string AFTER all the native
    // ref-churn loops still decodes byte-exact (the table is healthy).
    std::atomic<bool> g_post_native_mkstr_ok{ false };

    // set_arg union-aliasing loop driven by the injectMixed() hook.
    std::atomic<int> g_inject_mixed_hook_calls{ 0 };
    std::atomic<int> g_inject_mixed_str_ok{ 0 };
    std::atomic<int> g_inject_mixed_int_ok{ 0 };

    const std::string k_empty_payload{ "" };
    const std::string k_concat_a{ "concat-A-" };
    const std::string k_concat_b{ "concat-B-end" };
    const std::string k_mixed_payload{ "mixed-arg-str" };
    const std::int32_t k_mixed_n{ 424242 };
    const std::int64_t k_long_payload{ static_cast<std::int64_t>(0x4242424242424242LL) };
    const std::string k_inject_mixed_payload{ "set-arg-mixed-loop" };

    // The exact int the injectMixed hook injects into slot 2. Must match the
    // fixture's RepeatCallProbe.INJECT_MIXED_INT (asserted at runtime via
    // get_inject_mixed_int()). A compile-time constant (not a captured value) so
    // the stateless hook lambda can reference it.
    constexpr std::int32_t JLR_MIXED_INT{ 1337 };

    // set_arg(String) loop driven by the inject() hook.
    std::atomic<int>  g_inject_hook_calls{ 0 };
    std::atomic<int>  g_inject_setarg_ok{ 0 };

    // The fresh String the inject() hook injects each dispatch (built once so the
    // lambda captures a stable reference; the leak we test is the per-call
    // NewStringUTF ref, not the C++ string identity).
    const std::string k_inject_payload{ "set-arg-local-ref-loop" };
    const std::string k_echo_payload{ "echo-local-ref-loop" };

    // Payload for the NATIVE make_java_string loop: pure ASCII (Java-8 fixture
    // discipline) and well under the read_java_string cap so the only thing that
    // can break the byte-exact round-trip is a starved local-ref table.
    const std::string k_mkstr_payload{ "make-java-string-local-ref-loop" };

    // The class names the NATIVE find_class loops resolve. The HIT name is the
    // fixture's own internal name (guaranteed loaded — the detour is running in
    // its trigger() body). The MISS name can never name a loaded class, so its
    // resolution re-walks + re-drives the context-loader JNI path every iteration
    // (uncached), churning FindClass/NewStringUTF local refs and a ClassNotFound
    // exception that must be cleared each time.
    const std::string k_fc_hit_name{ "vmhook/fixtures/RepeatCallProbe" };
    const std::string k_fc_miss_name{ "vmhook/fixtures/NoSuchClass_jlr_hygiene_$$" };

    // Length / element_size for the NATIVE make_java_array loop (a fresh int[]
    // each iteration). Small + bounded; the leak guard is array_length stability,
    // not the contents.
    constexpr std::int32_t k_mkarr_len{ 6 };

    // Run the whole detour-side battery on the live receiver.
    auto run_loops(const std::unique_ptr<jni_local_ref>& self) -> void
    {
        if (!self)
        {
            return;
        }
        jni_local_ref& s = *self;
        g_receiver_instance.store(
            reinterpret_cast<std::uintptr_t>(s.get_instance()),
            std::memory_order_relaxed);

        // Iteration counts chosen WELL past the 16-slot default table so a single
        // un-released ref per call overflows it ~7-15x over the loop.
        constexpr int kStr   = 160;  // String return
        constexpr int kFresh = 160;  // fresh String return
        constexpr int kEcho  = 160;  // String arg + String return (2 refs/iter)
        constexpr int kObj   = 160;  // Object return
        constexpr int kArr   = 160;  // array return
        constexpr int kSStr  = 160;  // static String return (FindClass)
        constexpr int kSObj  = 160;  // static Object return (FindClass)
        constexpr int kMix   = 64;   // interleaved (every path each iter)

        // ── String-RETURN loop: stable single value across all iterations ─────
        // A starved table makes CallObjectMethodA return null -> the decoded
        // String becomes "" -> a SECOND distinct value appears. distinct == 1 and
        // zero empties == leak-free String-return decoding.
        {
            auto proxy{ s.get_method("makeString") };
            std::string first{};
            bool have_first{ false };
            int distinct{ 0 };
            int empties{ 0 };
            for (int i{ 0 }; i < kStr; ++i)
            {
                if (!proxy.has_value())
                {
                    distinct = -1;
                    break;
                }
                const std::string r{ proxy->call().as_string() };
                if (r.empty())
                {
                    ++empties;
                }
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
            g_str_loop_iters.store(kStr);
            g_str_loop_distinct.store(distinct);
            g_str_loop_empties.store(empties);
        }

        // ── FRESH-String-RETURN loop: a new heap String each call ─────────────
        // freshString() always evaluates to "fresh-77" but allocates a new
        // object every iteration (StringBuilder), so no constant-pool reuse can
        // mask a leak. Every iteration must equal "fresh-77".
        {
            auto proxy{ s.get_method("freshString") };
            int mism{ 0 };
            for (int i{ 0 }; i < kFresh; ++i)
            {
                if (!proxy.has_value())
                {
                    mism = kFresh;
                    break;
                }
                if (proxy->call().as_string() != "fresh-77")
                {
                    ++mism;
                }
            }
            g_fresh_loop_iters.store(kFresh);
            g_fresh_loop_mismatches.store(mism);
        }

        // ── String-ARG echo loop: NewStringUTF arg ref + returned String ref ──
        // TWO local refs per iteration -> the table starves twice as fast if
        // EITHER release is missing. Every echo must round-trip to the payload;
        // a starved table yields "" mismatches.
        {
            auto proxy{ s.get_method("echo") };
            int mism{ 0 };
            for (int i{ 0 }; i < kEcho; ++i)
            {
                if (!proxy.has_value())
                {
                    mism = kEcho;
                    break;
                }
                if (proxy->call(k_echo_payload).as_string() != k_echo_payload)
                {
                    ++mism;
                }
            }
            g_echo_loop_iters.store(kEcho);
            g_echo_loop_mismatches.store(mism);
        }

        // ── Object-RETURN loop: non-String reference, 'L' arm release ─────────
        // self() returns the receiver. On the call_stub path the reference
        // decodes to the receiver's real OOP (identity holds); on the call_jni
        // path the 'L' arm decodes+re-encodes the handle, so non-null still
        // holds. The leak guard is "decoded non-null on every iteration" (a
        // starved CallObjectMethodA would return null -> null wrapper).
        {
            auto proxy{ s.get_method("self") };
            int nonnull{ 0 };
            int identity_ok{ 0 };
            const std::uintptr_t recv{ g_receiver_instance.load(std::memory_order_relaxed) };
            for (int i{ 0 }; i < kObj; ++i)
            {
                if (!proxy.has_value())
                {
                    nonnull = -1;
                    break;
                }
                // copy-init (=), NOT brace-init: value_t's templated conversion
                // operator makes unique_ptr<T>{ value_t } ambiguous under MSVC.
                std::unique_ptr<jni_local_ref> w = proxy->call();
                if (w)
                {
                    ++nonnull;
                    if (reinterpret_cast<std::uintptr_t>(w->get_instance()) == recv && recv != 0)
                    {
                        ++identity_ok;
                    }
                }
            }
            g_obj_loop_iters.store(kObj);
            g_obj_loop_nonnull.store(nonnull);
            g_obj_loop_identity_ok.store(identity_ok);
        }

        // ── Array-RETURN loop: '[' reference, 'L'/'[' arm release ─────────────
        // makeArray() returns a fresh int[] each call. Decode to a non-null oop
        // via the value_t void* conversion (without walking it) on every
        // iteration.
        {
            auto proxy{ s.get_method("makeArray") };
            int nonnull{ 0 };
            for (int i{ 0 }; i < kArr; ++i)
            {
                if (!proxy.has_value())
                {
                    nonnull = -1;
                    break;
                }
                const auto v{ proxy->call() };
                void* const arr{ static_cast<void*>(v) };
                if (arr != nullptr)
                {
                    ++nonnull;
                }
            }
            g_arr_loop_iters.store(kArr);
            g_arr_loop_nonnull.store(nonnull);
        }

        // ── FRESH typed-array-RETURN loop: '[B' + '[C' + '[L...;' per iter ─────
        // makeBytes / makeChars / makeObjArray each return a BRAND-NEW heap array
        // every call (no pooling), so a leaked CallObjectMethodA ref on the '['
        // arm cannot hide behind a reused array. All three must decode non-null on
        // every iteration. The object-array ('[Ljava/lang/String;') exercises the
        // reference-array decode shape distinctly from the primitive arrays.
        {
            auto p_b{ s.get_method("makeBytes") };
            auto p_c{ s.get_method("makeChars") };
            auto p_o{ s.get_method("makeObjArray") };
            int failures{ 0 };
            for (int i{ 0 }; i < kArr; ++i)
            {
                bool ok{ p_b.has_value() && p_c.has_value() && p_o.has_value() };
                if (ok)
                {
                    const auto vb{ p_b->call() };
                    if (static_cast<void*>(vb) == nullptr) { ok = false; }
                }
                if (ok)
                {
                    const auto vc{ p_c->call() };
                    if (static_cast<void*>(vc) == nullptr) { ok = false; }
                }
                if (ok)
                {
                    const auto vo{ p_o->call() };
                    if (static_cast<void*>(vo) == nullptr) { ok = false; }
                }
                if (!ok)
                {
                    ++failures;
                }
            }
            g_typedarr_loop_iters.store(kArr);
            g_typedarr_loop_failures.store(failures);
        }

        // ── STATIC String-RETURN loop: FindClass jclass ref each dispatch ─────
        // The static path resolves the declaring jclass via FindClass (a local
        // ref) on top of the CallStaticObjectMethodA result ref. Stable single
        // value across the loop == both refs released.
        {
            auto proxy{ jni_local_ref::static_method("staticMakeString") };
            std::string first{};
            bool have_first{ false };
            int distinct{ 0 };
            for (int i{ 0 }; i < kSStr; ++i)
            {
                if (!proxy.has_value())
                {
                    distinct = -1;
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
            g_sstr_loop_iters.store(kSStr);
            g_sstr_loop_distinct.store(distinct);
        }

        // ── STATIC Object-RETURN loop: FindClass ref + result ref ─────────────
        {
            auto proxy{ jni_local_ref::static_method("staticSelf") };
            int nonnull{ 0 };
            for (int i{ 0 }; i < kSObj; ++i)
            {
                if (!proxy.has_value())
                {
                    nonnull = -1;
                    break;
                }
                std::unique_ptr<jni_local_ref> w = proxy->call();  // copy-init (MSVC C2440)
                if (w)
                {
                    ++nonnull;
                }
            }
            g_sobj_loop_iters.store(kSObj);
            g_sobj_loop_nonnull.store(nonnull);
        }

        // ── INTERLEAVED loop: every path once per iteration ───────────────────
        // The harshest mix: a String return, a String-arg echo (2 refs), an
        // Object return, an array return, a static String return (FindClass),
        // and a static Object return — all within a single iteration, so the
        // table holds several simultaneously-live refs before each release fires.
        // A single missing release anywhere overflows it within a handful of
        // iterations. failures == 0 across the whole loop is the strongest
        // single proof of full local-ref hygiene under realistic mixed pressure.
        {
            auto p_ms{ s.get_method("makeString") };
            auto p_echo{ s.get_method("echo") };
            auto p_self{ s.get_method("self") };
            auto p_arr{ s.get_method("makeArray") };
            auto p_sms{ jni_local_ref::static_method("staticMakeString") };
            auto p_sself{ jni_local_ref::static_method("staticSelf") };
            int failures{ 0 };
            for (int i{ 0 }; i < kMix; ++i)
            {
                bool ok{ p_ms.has_value() && p_echo.has_value() && p_self.has_value()
                         && p_arr.has_value() && p_sms.has_value() && p_sself.has_value() };
                if (ok && p_ms->call().as_string() != "local-ref-stable")              { ok = false; }
                if (ok && p_echo->call(k_echo_payload).as_string() != k_echo_payload)   { ok = false; }
                if (ok)
                {
                    std::unique_ptr<jni_local_ref> w = p_self->call();
                    if (!w) { ok = false; }
                }
                if (ok)
                {
                    const auto v{ p_arr->call() };
                    if (static_cast<void*>(v) == nullptr) { ok = false; }
                }
                if (ok && p_sms->call().as_string() != "static-local-ref-stable")      { ok = false; }
                if (ok)
                {
                    std::unique_ptr<jni_local_ref> w = p_sself->call();
                    if (!w) { ok = false; }
                }
                if (!ok)
                {
                    ++failures;
                }
            }
            g_mix_loop_iters.store(kMix);
            g_mix_loop_failures.store(failures);
        }

        constexpr int kPrim = 160;  // primitive / object / null / empty arg loops

        // ── PRIMITIVE-ARG loop (echoInt): union-aliasing discriminator ────────
        // The int arg's jvalue cell aliases the union's .l as a non-null wild
        // pointer. vmhook's per-slot needs_release tag must stay false so the
        // arg-cleanup RAII never hands it to DeleteLocalRef. No local ref is
        // created on this path, so a stable echo across the loop is the proof
        // that no spurious release fired (a bad release would crash / corrupt,
        // and a phantom leak cannot exist here — the control case).
        {
            auto proxy{ s.get_method("echoInt") };
            int mism{ 0 };
            for (int i{ 0 }; i < kPrim; ++i)
            {
                if (!proxy.has_value())
                {
                    mism = kPrim;
                    break;
                }
                // copy-init via value_t's conversion operator (no as_int()).
                std::int32_t r = proxy->call(i);
                if (r != i)
                {
                    ++mism;
                }
            }
            g_int_loop_iters.store(kPrim);
            g_int_loop_mismatches.store(mism);
        }

        // ── TWO-WORD primitive-arg loop (echoLong): .j aliases .l as a wild ptr ─
        // 0x4242424242424242 as a jlong: if vmhook ever read .l back to classify
        // the slot it would DeleteLocalRef this garbage address. Stable echo of
        // the exact 64-bit value proves the long slot was left untouched.
        {
            auto proxy{ s.get_method("echoLong") };
            int mism{ 0 };
            for (int i{ 0 }; i < kPrim; ++i)
            {
                if (!proxy.has_value())
                {
                    mism = kPrim;
                    break;
                }
                std::int64_t r = proxy->call(k_long_payload);  // copy-init
                if (r != k_long_payload)
                {
                    ++mism;
                }
            }
            g_long_loop_iters.store(kPrim);
            g_long_loop_mismatches.store(mism);
        }

        // ── BOOLEAN-arg loop (echoBool): .z == 1 aliases .l as 0x1 ────────────
        // jboolean true -> .z byte 0x01, which aliases .l as the pointer 0x1: a
        // low, non-null value a naive null-check passes straight to
        // DeleteLocalRef. needs_release must still be false. Echo returns 1.
        {
            auto proxy{ s.get_method("echoBool") };
            int mism{ 0 };
            for (int i{ 0 }; i < kPrim; ++i)
            {
                if (!proxy.has_value())
                {
                    mism = kPrim;
                    break;
                }
                std::int32_t r = proxy->call(true);  // copy-init
                if (r != 1)
                {
                    ++mism;
                }
            }
            g_bool_loop_iters.store(kPrim);
            g_bool_loop_mismatches.store(mism);
        }

        // ── MIXED-arg loop (echoMixed: String slot released, int slot NOT) ────
        // Only slot 1 (the String, a NewStringUTF local ref) is tagged for
        // release; slot 2 (the int) must keep needs_release false. A starved
        // table (missing String release) yields a truncated / empty result; a
        // bad primitive-slot release crashes. Both args must arrive every iter.
        {
            auto proxy{ s.get_method("echoMixed") };
            const std::string want{ k_mixed_payload + ":" + std::to_string(k_mixed_n) };
            int mism{ 0 };
            for (int i{ 0 }; i < kPrim; ++i)
            {
                if (!proxy.has_value())
                {
                    mism = kPrim;
                    break;
                }
                if (proxy->call(k_mixed_payload, k_mixed_n).as_string() != want)
                {
                    ++mism;
                }
            }
            g_mixed_loop_iters.store(kPrim);
            g_mixed_loop_mismatches.store(mism);
        }

        // ── TWO-String-arg loop (concat: 3 refs/iter) ─────────────────────────
        // Two NewStringUTF arg refs (slots 1+2) + the returned String ref. The
        // table starves THREE times as fast if any release is missing; the
        // concatenation must equal a+b every iteration.
        {
            auto proxy{ s.get_method("concat") };
            const std::string want{ k_concat_a + k_concat_b };
            int mism{ 0 };
            for (int i{ 0 }; i < kPrim; ++i)
            {
                if (!proxy.has_value())
                {
                    mism = kPrim;
                    break;
                }
                if (proxy->call(k_concat_a, k_concat_b).as_string() != want)
                {
                    ++mism;
                }
            }
            g_concat_loop_iters.store(kPrim);
            g_concat_loop_mismatches.store(mism);
        }

        // ── OBJECT-arg loop (echoObj): synthetic handle arg NOT released ──────
        // The receiver is passed as an object arg: write_jni_arg_to_slot points
        // value.l at &handle_storage[i] (a self-pointer), leaving needs_release
        // false — the arg-cleanup must NOT DeleteLocalRef it (deleting the stack
        // cell's address is the synthetic-handle footgun). The Object RETURN ref
        // still must be released. Decoded non-null on every iteration.
        {
            auto proxy{ s.get_method("echoObj") };
            int nonnull{ 0 };
            for (int i{ 0 }; i < kPrim; ++i)
            {
                if (!proxy.has_value())
                {
                    nonnull = -1;
                    break;
                }
                std::unique_ptr<jni_local_ref> w = proxy->call(self);  // copy-init (MSVC C2440)
                if (w)
                {
                    ++nonnull;
                }
            }
            g_objarg_loop_iters.store(kPrim);
            g_objarg_loop_nonnull.store(nonnull);
        }

        // ── NULL-String-arg loop (nullArgLen): null C string -> Java null ─────
        // A null const char* maps to Java null with NO NewStringUTF and
        // needs_release false — the arg-cleanup must skip it. The body sees null
        // and returns -1 every iteration (a leak/overflow cannot manifest here
        // since no ref is created, so this pins the no-ref / no-release path).
        {
            auto proxy{ s.get_method("nullArgLen") };
            int mism{ 0 };
            const char* const null_str{ nullptr };
            for (int i{ 0 }; i < kPrim; ++i)
            {
                if (!proxy.has_value())
                {
                    mism = kPrim;
                    break;
                }
                std::int32_t r = proxy->call(null_str);  // copy-init
                if (r != -1)
                {
                    ++mism;
                }
            }
            g_nullarg_loop_iters.store(kPrim);
            g_nullarg_loop_mismatches.store(mism);
        }

        // ── EMPTY-String-arg loop (echo with ""): real empty Java String ──────
        // A non-null empty "" becomes a REAL empty Java String (a NewStringUTF
        // local ref to release), distinct from Java null. The echo round-trips
        // to "" every iteration; the arg ref is released each time.
        {
            auto proxy{ s.get_method("echo") };
            int mism{ 0 };
            for (int i{ 0 }; i < kPrim; ++i)
            {
                if (!proxy.has_value())
                {
                    mism = kPrim;
                    break;
                }
                if (!proxy->call(k_empty_payload).as_string().empty())
                {
                    ++mism;
                }
            }
            g_emptyarg_loop_iters.store(kPrim);
            g_emptyarg_loop_mismatches.store(mism);
        }

        // ── STATIC String-arg echo loop (staticEcho: 3 refs/iter, static path) ─
        // The harshest static case: FindClass jclass ref + NewStringUTF arg ref
        // + CallStaticObjectMethodA result ref, all per dispatch. Every echo
        // must round-trip to the payload.
        {
            auto proxy{ jni_local_ref::static_method("staticEcho") };
            int mism{ 0 };
            for (int i{ 0 }; i < kPrim; ++i)
            {
                if (!proxy.has_value())
                {
                    mism = kPrim;
                    break;
                }
                if (proxy->call(k_echo_payload).as_string() != k_echo_payload)
                {
                    ++mism;
                }
            }
            g_secho_loop_iters.store(kPrim);
            g_secho_loop_mismatches.store(mism);
        }

        // ── NATIVE make_java_string loop: internal NewString local ref release ─
        // vmhook::make_java_string allocates a Java String OOP; its JNI path uses
        // jni_new_string_utf16_local (a NewString LOCAL ref) and DeleteLocalRefs
        // the handle after extracting the OOP (vmhook.hpp comment "Skipping
        // DeleteLocalRef would leak one local per call"). Driven directly from the
        // detour (no Java call()), far past the 16-slot table: a missing release
        // would starve the table and later make_java_string calls would return
        // null / decode "". Every iteration must decode byte-exact to the payload.
        {
            int mism{ 0 };
            int nonnull{ 0 };
            for (int i{ 0 }; i < kPrim; ++i)
            {
                void* const oop{ vmhook::make_java_string(k_mkstr_payload) };
                if (oop != nullptr && vmhook::hotspot::is_valid_pointer(oop))
                {
                    ++nonnull;
                    // copy-init from read_java_string (never brace-init).
                    const std::string decoded = vmhook::read_java_string(oop);
                    if (decoded != k_mkstr_payload)
                    {
                        ++mism;
                    }
                }
                else
                {
                    ++mism;
                }
            }
            g_mkstr_loop_iters.store(kPrim);
            g_mkstr_loop_mismatches.store(mism);
            g_mkstr_loop_nonnull.store(nonnull);
        }

        // ── NATIVE make_java_array loop: internal New<Type>Array local ref ─────
        // vmhook::make_java_array's JNI fallback allocates via New<Type>Array (a
        // LOCAL ref) and DeleteLocalRefs the array_handle after decoding the oop
        // (vmhook.hpp make_java_array). A fresh int[] each iteration; the leak
        // guard is array_length == requested on every iteration (a starved table
        // would return null / a malformed array). Element size is the int width.
        {
            int badlen{ 0 };
            int nonnull{ 0 };
            for (int i{ 0 }; i < kPrim; ++i)
            {
                void* const oop{ vmhook::make_java_array(
                    "[I", k_mkarr_len, sizeof(std::int32_t)) };
                if (oop != nullptr && vmhook::hotspot::is_valid_pointer(oop))
                {
                    ++nonnull;
                    if (vmhook::array_length(oop) != k_mkarr_len)
                    {
                        ++badlen;
                    }
                }
                else
                {
                    ++badlen;
                }
            }
            g_mkarr_loop_iters.store(kPrim);
            g_mkarr_loop_badlen.store(badlen);
            g_mkarr_loop_nonnull.store(nonnull);
        }

        // ── NATIVE find_class HIT loop: cached klass + resolution ref release ──
        // vmhook::find_class on a loaded class resolves via the
        // ClassLoaderDataGraph walk (and on a context-loader fallback releases its
        // FindClass / GetObjectClass local refs), caches the klass, then returns
        // the SAME cached pointer on every subsequent call. distinct == 1 and zero
        // nulls across the loop == stable resolution with its local refs released.
        {
            void* first{ nullptr };
            bool have_first{ false };
            int distinct{ 0 };
            int nulls{ 0 };
            for (int i{ 0 }; i < kPrim; ++i)
            {
                void* const k{ static_cast<void*>(vmhook::find_class(k_fc_hit_name)) };
                if (k == nullptr)
                {
                    ++nulls;
                    continue;
                }
                if (!have_first)
                {
                    first = k;
                    have_first = true;
                    distinct = 1;
                }
                else if (k != first)
                {
                    ++distinct;
                }
            }
            g_fchit_loop_iters.store(kPrim);
            g_fchit_loop_distinct.store(distinct);
            g_fchit_loop_null.store(nulls);
        }

        // ── NATIVE find_class MISS loop: uncached re-walk + exception clear ────
        // A name that can never resolve is NOT cached, so every iteration re-walks
        // the graph and re-drives jni_find_class_with_context_loader, which creates
        // + DeleteLocalRefs FindClass / NewStringUTF handles AND clears the pending
        // ClassNotFound exception. A leaked ref or an un-cleared exception would
        // starve the table / poison the next JNI call; every iteration must still
        // return null with the JVM healthy. Bounded (kStaticMiss) to stay modest:
        // the miss path is heavier than a cache hit. null on every iteration ==
        // the no-resolution path stayed leak-free and exception-clean.
        {
            constexpr int kStaticMiss{ 64 };
            int nonnull{ 0 };
            for (int i{ 0 }; i < kStaticMiss; ++i)
            {
                if (vmhook::find_class(k_fc_miss_name) != nullptr)
                {
                    ++nonnull;
                }
            }
            g_fcmiss_loop_iters.store(kStaticMiss);
            g_fcmiss_loop_nonnull.store(nonnull);
        }

        // ── POST-LOOP sanity: a single String call after all the loops works ──
        // Hundreds of allocate+release cycles later, the table is healthy and a
        // fresh dispatch still decodes its String.
        {
            auto proxy{ s.get_method("makeString") };
            if (proxy.has_value())
            {
                g_post_loop_str_ok.store(proxy->call().as_string() == "local-ref-stable",
                                         std::memory_order_relaxed);
            }
        }

        // ── POST-NATIVE sanity: a fresh make_java_string after all native loops ─
        // After the make_java_string / make_java_array / find_class ref-churn
        // loops, a brand-new native String allocation still decodes byte-exact:
        // the internal local-ref discipline left the table healthy.
        {
            void* const oop{ vmhook::make_java_string(k_mkstr_payload) };
            const bool ok{ oop != nullptr
                           && vmhook::hotspot::is_valid_pointer(oop)
                           && vmhook::read_java_string(oop) == k_mkstr_payload };
            g_post_native_mkstr_ok.store(ok, std::memory_order_relaxed);
        }
    }
}

VMHOOK_JVM_MODULE(repeat_call_stability)
{
    vmhook::register_class<jni_local_ref>("vmhook/fixtures/RepeatCallProbe");

    // Record which call() dispatch path the live JDK uses. Both paths allocate
    // and must release the same JNI local refs for the cases under test; the
    // module is correct (not skipped) on either.
    g_call_stub_present.store(vmhook::detail::find_call_stub_entry() != nullptr,
                              std::memory_order_relaxed);

    {
        // Hook 1: trigger() — establishes current_java_thread; the detour runs
        // every call()/return leak loop here.
        auto h_trigger{ vmhook::scoped_hook<jni_local_ref>(
            "trigger",
            [](vmhook::return_value&,
               const std::unique_ptr<jni_local_ref>& self)
            {
                g_detour_calls.fetch_add(1, std::memory_order_relaxed);
                g_detour_saw_self.store(self != nullptr, std::memory_order_relaxed);
                run_loops(self);
            }) };

        // Hook 2: inject(String) — each dispatch the detour calls set_arg(0, ...)
        // to inject a FRESH Java String into slot 0 (NewStringUTF local ref +
        // DeleteLocalRef). The probe dispatches inject() in a loop, so this hook
        // exercises the set_arg(String) release path far past the 16-slot table.
        auto h_inject{ vmhook::scoped_hook<jni_local_ref>(
            "inject", "(Ljava/lang/String;)V",
            [](vmhook::return_value& ret,
               const std::unique_ptr<jni_local_ref>&,
               const std::string& /*original*/)
            {
                g_inject_hook_calls.fetch_add(1, std::memory_order_relaxed);
                // set_arg(slot, std::string_view) routes through jni_new_string_utf
                // + jni_delete_local_ref (vmhook.hpp return_value::set_arg). On an
                // instance method slot 0 is `this`, so the String arg `value` lives
                // at slot 1 — inject there.
                if (ret.set_arg(1, std::string_view{ k_inject_payload }))
                {
                    g_inject_setarg_ok.fetch_add(1, std::memory_order_relaxed);
                }
            }) };

        // Hook 3: injectMixed(String,int) — each dispatch the detour injects BOTH
        // a fresh String into slot 1 (set_arg routes through NewStringUTF +
        // DeleteLocalRef) AND a primitive int into slot 2 (set_arg's primitive
        // path: NO NewStringUTF, NO DeleteLocalRef — a primitive jvalue cell must
        // never be handed to DeleteLocalRef, the union-aliasing footgun). Driven
        // far past the 16-slot table, this pins both the String-release discipline
        // and the no-release-for-primitives discipline of set_arg simultaneously.
        auto h_inject_mixed{ vmhook::scoped_hook<jni_local_ref>(
            "injectMixed", "(Ljava/lang/String;I)V",
            [](vmhook::return_value& ret,
               const std::unique_ptr<jni_local_ref>&,
               const std::string& /*original*/,
               std::int32_t /*n*/)
            {
                g_inject_mixed_hook_calls.fetch_add(1, std::memory_order_relaxed);
                if (ret.set_arg(1, std::string_view{ k_inject_mixed_payload }))
                {
                    g_inject_mixed_str_ok.fetch_add(1, std::memory_order_relaxed);
                }
                // Primitive set_arg: slot 2 is the int. No local ref is created
                // here, so nothing must be released; a stable readback proves the
                // primitive path never mis-fired DeleteLocalRef on a union cell.
                if (ret.set_arg(2, static_cast<std::int32_t>(JLR_MIXED_INT)))
                {
                    g_inject_mixed_int_ok.fetch_add(1, std::memory_order_relaxed);
                }
            }) };

        ctx.check("jlr_trigger_hook_installed", h_trigger.installed());
        ctx.check("jlr_inject_hook_installed", h_inject.installed());
        ctx.check("jlr_inject_mixed_hook_installed", h_inject_mixed.installed());

        const bool done{ ctx.run_probe(
            [](bool v) { jni_local_ref::set_go(v); },
            []() { return jni_local_ref::get_done(); }) };

        ctx.check("jlr_probe_completed", done);
        ctx.check("jlr_detour_fired", g_detour_calls.load(std::memory_order_relaxed) >= 1);
        ctx.check("jlr_detour_saw_self", g_detour_saw_self.load(std::memory_order_relaxed));
        ctx.check("jlr_trigger_count_advanced", jni_local_ref::get_trigger_count() >= 1);

        const bool stub{ g_call_stub_present.load(std::memory_order_relaxed) };
        ctx.record(std::string{ "[INFO] repeat_call_stability call() dispatch path: " }
                   + (stub ? "call_stub fast path (StubRoutines::_call_stub_entry present)"
                           : "call_jni JNI fallback (Call(Static)?ObjectMethodA — "
                             "the path whose local refs this module stresses)"));

        // ════════════════ String-RETURN local-ref discipline ══════════════════
        // distinct == 1 AND zero empties: every one of the calls decoded the same
        // non-empty String. A leaked CallObjectMethodA ref would starve the table
        // and later calls would return "" (a 2nd distinct value + nonzero empties).
        ctx.check("jlr_string_return_loop_ran", g_str_loop_iters.load() == 160);
        ctx.check("jlr_string_return_no_leak_single_distinct",
                  g_str_loop_distinct.load() == 1);
        ctx.check("jlr_string_return_no_empty_results",
                  g_str_loop_empties.load() == 0);

        // ════════════════ FRESH-String-RETURN discipline ══════════════════════
        // A brand-new heap String each call: zero mismatches proves the fresh
        // jstring local ref is released every iteration.
        ctx.check("jlr_fresh_string_loop_ran", g_fresh_loop_iters.load() == 160);
        ctx.check("jlr_fresh_string_no_leak_zero_mismatches",
                  g_fresh_loop_mismatches.load() == 0);

        // ════════════════ String-ARG echo (2 refs/iter) discipline ════════════
        // Every echo round-trips: both the NewStringUTF arg ref and the returned
        // jstring ref are released each iteration.
        ctx.check("jlr_echo_loop_ran", g_echo_loop_iters.load() == 160);
        ctx.check("jlr_echo_no_leak_zero_mismatches",
                  g_echo_loop_mismatches.load() == 0);

        // ════════════════ Object-RETURN ('L' arm) discipline ══════════════════
        // Non-null on every iteration: the CallObjectMethodA result ref is
        // released each time (a starved table would return null -> null wrapper).
        ctx.check("jlr_object_return_loop_ran", g_obj_loop_iters.load() == 160);
        ctx.check("jlr_object_return_all_iters_non_null",
                  g_obj_loop_nonnull.load() == 160);
        // On the call_stub path the decoded OOP equals the receiver every time
        // (identity preserved across the whole loop); on call_jni the handle is
        // re-encoded so identity is path-dependent — record it either way.
        if (stub)
        {
            ctx.check("jlr_object_return_identity_preserved_call_stub",
                      g_obj_loop_identity_ok.load() == 160);
        }
        else
        {
            ctx.record("[INFO] jlr object-return identity matches receiver on "
                       + std::to_string(g_obj_loop_identity_ok.load()) + "/160 iters "
                       "(call_jni re-encodes the handle; non-null is the leak guard).");
        }

        // ════════════════ Array-RETURN ('[' arm) discipline ═══════════════════
        ctx.check("jlr_array_return_loop_ran", g_arr_loop_iters.load() == 160);
        ctx.check("jlr_array_return_all_iters_non_null",
                  g_arr_loop_nonnull.load() == 160);

        // ════════════════ FRESH typed-array-RETURN ('[B'/'[C'/'[L') discipline ═
        // makeBytes / makeChars / makeObjArray each return a brand-new heap array
        // every call; all three decode non-null on every iteration == the
        // CallObjectMethodA '[' arm ref is released across every element kind,
        // including the reference-array ('[Ljava/lang/String;') decode shape.
        ctx.check("jlr_typed_array_return_loop_ran", g_typedarr_loop_iters.load() == 160);
        ctx.check("jlr_typed_array_return_no_leak_zero_failures",
                  g_typedarr_loop_failures.load() == 0);

        // ════════════════ STATIC String-RETURN (FindClass) discipline ═════════
        // The FindClass jclass local ref AND the CallStaticObjectMethodA result
        // ref are both released each dispatch: stable single value across the loop.
        ctx.check("jlr_static_string_loop_ran", g_sstr_loop_iters.load() == 160);
        ctx.check("jlr_static_string_no_leak_single_distinct",
                  g_sstr_loop_distinct.load() == 1);

        // ════════════════ STATIC Object-RETURN (FindClass) discipline ═════════
        ctx.check("jlr_static_object_loop_ran", g_sobj_loop_iters.load() == 160);
        ctx.check("jlr_static_object_all_iters_non_null",
                  g_sobj_loop_nonnull.load() == 160);

        // ════════════════ INTERLEAVED mixed-pressure discipline ════════════════
        // The strongest single proof: every path once per iteration, several
        // live refs in flight before each release. Zero failures across the loop
        // == full hygiene under realistic mixed local-ref pressure.
        ctx.check("jlr_interleaved_loop_ran", g_mix_loop_iters.load() == 64);
        ctx.check("jlr_interleaved_no_leak_zero_failures",
                  g_mix_loop_failures.load() == 0);

        // ════════════ PRIMITIVE-ARG union-aliasing discipline (echoInt) ════════
        // The int arg's jvalue cell aliases the union's .l as a non-null wild
        // pointer; vmhook's per-slot needs_release tag must stay false so the
        // arg-cleanup never DeleteLocalRef's it. No local ref exists on this path,
        // so a stable echo across all iterations is the union-aliasing proof (a
        // spurious release would have crashed long before the count completed).
        ctx.check("jlr_primitive_int_arg_loop_ran", g_int_loop_iters.load() == 160);
        ctx.check("jlr_primitive_int_arg_no_misrelease_zero_mismatches",
                  g_int_loop_mismatches.load() == 0);

        // ════════════ TWO-WORD primitive-arg discipline (echoLong) ════════════
        // 0x4242424242424242 as a jlong: its .j bits alias .l as a wild pointer —
        // the harshest DeleteLocalRef-discriminator case. Exact 64-bit echo every
        // iteration proves the long slot was never read back as a local ref.
        ctx.check("jlr_primitive_long_arg_loop_ran", g_long_loop_iters.load() == 160);
        ctx.check("jlr_primitive_long_arg_no_misrelease_zero_mismatches",
                  g_long_loop_mismatches.load() == 0);

        // ════════════ BOOLEAN-arg discipline (echoBool, .z==1 aliases .l=0x1) ══
        // jboolean true -> .z 0x01 aliases .l as the pointer 0x1: a low, non-null
        // value a naive null-check would pass to DeleteLocalRef. needs_release
        // stays false; echo returns 1 every iteration.
        ctx.check("jlr_primitive_bool_arg_loop_ran", g_bool_loop_iters.load() == 160);
        ctx.check("jlr_primitive_bool_arg_no_misrelease_zero_mismatches",
                  g_bool_loop_mismatches.load() == 0);

        // ════════════ MIXED-arg per-slot discipline (echoMixed) ═══════════════
        // Slot 1 (String) is the only NewStringUTF ref to release; slot 2 (int)
        // must keep needs_release false. Both args arrive intact every iteration:
        // a missing String release starves the table (truncated/empty result); a
        // bad primitive-slot release crashes. Zero mismatches == correct per-slot
        // discrimination under sustained pressure.
        ctx.check("jlr_mixed_arg_loop_ran", g_mixed_loop_iters.load() == 160);
        ctx.check("jlr_mixed_arg_per_slot_release_zero_mismatches",
                  g_mixed_loop_mismatches.load() == 0);

        // ════════════ TWO-String-arg discipline (concat, 3 refs/iter) ═════════
        // Two NewStringUTF arg refs + the returned String ref starve the table 3x
        // as fast if any release is missing; the concatenation must equal a+b
        // every iteration.
        ctx.check("jlr_concat_two_string_args_loop_ran", g_concat_loop_iters.load() == 160);
        ctx.check("jlr_concat_two_string_args_no_leak_zero_mismatches",
                  g_concat_loop_mismatches.load() == 0);

        // ════════════ OBJECT-arg synthetic-handle discipline (echoObj) ════════
        // The object arg's value.l points at the stack handle_storage cell (a
        // self-pointer), so needs_release stays false — the arg-cleanup must NOT
        // DeleteLocalRef the stack address. The Object RETURN ref still must be
        // released. Decoded non-null on every iteration == both disciplines held.
        ctx.check("jlr_object_arg_loop_ran", g_objarg_loop_iters.load() == 160);
        ctx.check("jlr_object_arg_all_iters_non_null",
                  g_objarg_loop_nonnull.load() == 160);

        // ════════════ NULL-String-arg discipline (nullArgLen) ═════════════════
        // A null const char* -> Java null with NO NewStringUTF and needs_release
        // false (the arg-cleanup must skip the slot). The body sees null and
        // returns -1 every iteration — pins the no-ref / no-release arg path.
        ctx.check("jlr_null_string_arg_loop_ran", g_nullarg_loop_iters.load() == 160);
        ctx.check("jlr_null_string_arg_always_java_null_zero_mismatches",
                  g_nullarg_loop_mismatches.load() == 0);

        // ════════════ EMPTY-String-arg discipline (echo "") ═══════════════════
        // A non-null "" becomes a REAL empty Java String (a NewStringUTF local ref
        // to release), distinct from Java null. The echo round-trips to "" every
        // iteration; the arg ref is released each time.
        ctx.check("jlr_empty_string_arg_loop_ran", g_emptyarg_loop_iters.load() == 160);
        ctx.check("jlr_empty_string_arg_no_leak_zero_mismatches",
                  g_emptyarg_loop_mismatches.load() == 0);

        // ════════════ STATIC String-arg discipline (staticEcho, 3 refs/iter) ══
        // FindClass jclass ref + NewStringUTF arg ref + CallStaticObjectMethodA
        // result ref, all per dispatch on the static path. Every echo round-trips
        // to the payload == all three released each iteration.
        ctx.check("jlr_static_string_arg_loop_ran", g_secho_loop_iters.load() == 160);
        ctx.check("jlr_static_string_arg_no_leak_zero_mismatches",
                  g_secho_loop_mismatches.load() == 0);

        // ════════════════ POST-LOOP non-degradation ═══════════════════════════
        // After hundreds of allocate+release cycles a fresh dispatch still works.
        ctx.check("jlr_post_loop_call_still_works",
                  g_post_loop_str_ok.load(std::memory_order_relaxed));

        // ════════════ NATIVE make_java_string local-ref discipline ════════════
        // vmhook::make_java_string's JNI path allocates a NewString LOCAL ref and
        // DeleteLocalRefs the handle after extracting the OOP. Driven directly
        // from the detour far past the 16-slot table: byte-exact decode on every
        // iteration (zero mismatches, all non-null) == the internal local ref is
        // released each time (a leak would starve the table -> null oop / "").
        ctx.check("jlr_native_make_string_loop_ran", g_mkstr_loop_iters.load() == 160);
        ctx.check("jlr_native_make_string_all_iters_non_null",
                  g_mkstr_loop_nonnull.load() == 160);
        ctx.check("jlr_native_make_string_no_leak_zero_mismatches",
                  g_mkstr_loop_mismatches.load() == 0);

        // ════════════ NATIVE make_java_array local-ref discipline ═════════════
        // vmhook::make_java_array's JNI fallback allocates via New<Type>Array (a
        // LOCAL ref) and DeleteLocalRefs the array_handle after decoding the oop.
        // A fresh int[] each iteration; array_length == requested on every
        // iteration (zero bad lengths, all non-null) == the internal local ref is
        // released each time.
        ctx.check("jlr_native_make_array_loop_ran", g_mkarr_loop_iters.load() == 160);
        ctx.check("jlr_native_make_array_all_iters_non_null",
                  g_mkarr_loop_nonnull.load() == 160);
        ctx.check("jlr_native_make_array_no_leak_correct_length_every_iter",
                  g_mkarr_loop_badlen.load() == 0);

        // ════════════ NATIVE find_class HIT local-ref discipline ══════════════
        // find_class on a loaded class resolves + caches once and returns the SAME
        // klass thereafter; the resolution's FindClass / GetObjectClass local refs
        // are released. distinct == 1 and zero nulls across the loop == stable,
        // leak-free resolution.
        ctx.check("jlr_native_find_class_hit_loop_ran", g_fchit_loop_iters.load() == 160);
        ctx.check("jlr_native_find_class_hit_stable_single_klass",
                  g_fchit_loop_distinct.load() == 1);
        ctx.check("jlr_native_find_class_hit_never_null",
                  g_fchit_loop_null.load() == 0);

        // ════════════ NATIVE find_class MISS local-ref + exception discipline ══
        // An absent class is never cached, so every iteration re-walks the graph
        // and re-drives the context-loader JNI path, which creates + DeleteLocalRefs
        // FindClass / NewStringUTF handles AND clears the pending ClassNotFound
        // exception. Every iteration returns null with the JVM healthy == that
        // uncached path is leak-free and exception-clean (a leaked ref / un-cleared
        // exception would starve the table or poison the next JNI call).
        ctx.check("jlr_native_find_class_miss_loop_ran", g_fcmiss_loop_iters.load() == 64);
        ctx.check("jlr_native_find_class_miss_always_null",
                  g_fcmiss_loop_nonnull.load() == 0);

        // ════════════ POST-NATIVE non-degradation ═════════════════════════════
        // After the native ref-churn loops a fresh make_java_string still decodes
        // byte-exact: the internal local-ref discipline left the table healthy.
        ctx.check("jlr_post_native_make_string_still_byte_exact",
                  g_post_native_mkstr_ok.load(std::memory_order_relaxed));

        // ════════════════ set_arg(String) local-ref discipline ════════════════
        // The probe dispatched inject() RepeatCallProbe.INJECT_ITERATIONS times; each
        // dispatch the detour injected a fresh Java String via set_arg(1, ...),
        // which allocates a NewStringUTF local ref and releases it
        // (jni_delete_local_ref). Far past the 16-slot table, so a missing
        // release would overflow it and later set_arg calls would fail / inject
        // "" — which the body records as a shorter / empty injectSeen.
        const std::int32_t inject_iters{ jni_local_ref::get_inject_iterations() };
        ctx.record("[INFO] jlr set_arg(String) loop: fixture INJECT_ITERATIONS="
                   + std::to_string(inject_iters)
                   + ", inject() hook fired " + std::to_string(g_inject_hook_calls.load())
                   + " time(s), set_arg returned true "
                   + std::to_string(g_inject_setarg_ok.load()) + " time(s); body ran "
                   + std::to_string(jni_local_ref::get_inject_count())
                   + " time(s), observed non-empty injected value "
                   + std::to_string(jni_local_ref::get_inject_nonempty_count()) + " time(s).");

        ctx.check("jlr_setarg_loop_well_past_16_slots", inject_iters >= 100);
        ctx.check("jlr_setarg_inject_hook_fired_each_dispatch",
                  g_inject_hook_calls.load() == inject_iters && inject_iters > 0);
        // Every set_arg call succeeded — no internal NewStringUTF failure / table
        // exhaustion derailed any injection across the whole loop.
        ctx.check("jlr_setarg_all_injections_returned_true",
                  g_inject_setarg_ok.load() == g_inject_hook_calls.load()
                  && g_inject_hook_calls.load() > 0);
        ctx.check("jlr_setarg_body_ran", jni_local_ref::get_inject_body_ran());
        // Every body observed a non-null, non-empty injected String -> the
        // injection reached an unstarved local slot on every one of the 120
        // dispatches (a leaked ref would eventually inject "" or fail).
        ctx.check("jlr_setarg_every_body_saw_nonempty",
                  jni_local_ref::get_inject_nonempty_count() == jni_local_ref::get_inject_count()
                  && jni_local_ref::get_inject_count() == inject_iters);
        // The LAST body observed the exact injected payload (length + content):
        // proof the set_arg(String) path delivered the right bytes after the
        // whole loop, not a truncated / starved value.
        ctx.check("jlr_setarg_last_body_len_exact",
                  jni_local_ref::get_inject_len_seen()
                      == static_cast<std::int32_t>(k_inject_payload.size()));
        ctx.check("jlr_setarg_last_body_content_exact",
                  jni_local_ref::get_inject_seen() == k_inject_payload);

        // ════════════ set_arg UNION-ALIASING discipline (injectMixed) ═════════
        // The probe dispatched injectMixed(String,int) INJECT_ITERATIONS times;
        // each dispatch the detour set BOTH slot 1 (a fresh String via set_arg ->
        // NewStringUTF + DeleteLocalRef) AND slot 2 (a primitive int via set_arg's
        // no-local-ref primitive path — which must NEVER hand the union-aliased
        // cell to DeleteLocalRef). Far past the 16-slot table, this pins the
        // String-release discipline and the no-release-for-primitives discipline
        // of set_arg at once: a String leak starves the table (empty injectMixed
        // String), and a bad primitive release would crash.
        const std::int32_t fixture_mixed_int{ jni_local_ref::get_inject_mixed_int() };
        ctx.record("[INFO] jlr set_arg(union-aliasing) loop: injectMixed hook fired "
                   + std::to_string(g_inject_mixed_hook_calls.load())
                   + " time(s), set_arg(String) true " + std::to_string(g_inject_mixed_str_ok.load())
                   + ", set_arg(int) true " + std::to_string(g_inject_mixed_int_ok.load())
                   + "; body ran " + std::to_string(jni_local_ref::get_inject_mixed_count())
                   + " time(s), saw BOTH slots intact "
                   + std::to_string(jni_local_ref::get_inject_mixed_ok_count()) + " time(s).");

        // The fixture's injected-int constant matches the value the hook injects:
        // a hard invariant (both sides hard-code 1337) guarding accidental drift.
        ctx.check("jlr_setarg_mixed_int_constant_matches", fixture_mixed_int == JLR_MIXED_INT);
        ctx.check("jlr_setarg_mixed_hook_fired_each_dispatch",
                  g_inject_mixed_hook_calls.load() == inject_iters && inject_iters > 0);
        // Every set_arg(String) on slot 1 succeeded — no NewStringUTF failure /
        // table exhaustion across the whole loop.
        ctx.check("jlr_setarg_mixed_all_string_injections_true",
                  g_inject_mixed_str_ok.load() == g_inject_mixed_hook_calls.load()
                  && g_inject_mixed_hook_calls.load() > 0);
        // Every set_arg(int) on slot 2 succeeded — the primitive path stored the
        // value (and, critically, allocated/released NOTHING).
        ctx.check("jlr_setarg_mixed_all_int_injections_true",
                  g_inject_mixed_int_ok.load() == g_inject_mixed_hook_calls.load()
                  && g_inject_mixed_hook_calls.load() > 0);
        // Every body observed BOTH a non-empty String (slot 1, unstarved) AND the
        // exact injected int (slot 2, the primitive arrived unchanged): the
        // simultaneous proof that the String ref was released and the primitive
        // cell was NEVER mistaken for a ref to release.
        ctx.check("jlr_setarg_mixed_every_body_saw_both_slots",
                  jni_local_ref::get_inject_mixed_ok_count() == jni_local_ref::get_inject_mixed_count()
                  && jni_local_ref::get_inject_mixed_count() == inject_iters);
        // The LAST body saw the exact injected primitive int — the union-aliased
        // slot delivered the right bits after the whole loop, not garbage.
        ctx.check("jlr_setarg_mixed_last_body_int_exact",
                  jni_local_ref::get_inject_mixed_int_seen() == JLR_MIXED_INT);
        // The LAST body saw the exact injected String content.
        ctx.check("jlr_setarg_mixed_last_body_string_exact",
                  jni_local_ref::get_inject_mixed_seen() == k_inject_mixed_payload);
    }
}
