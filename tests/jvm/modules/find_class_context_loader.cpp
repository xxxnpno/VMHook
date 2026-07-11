// find_class_context_loader JVM test module  (feature area: class lookup)
//
// THE authority for vmhook's CONTEXT-CLASSLOADER resolution + CLASSLOADER-REANCHOR
// family — the half of class lookup that resolves app-classloader classes the bare
// HotSpot graph walk can miss, and that re-points resolution at a specific loader.
// This is deliberately DISJOINT from find_class_fallback.cpp (which owns the
// ClassLoaderDataGraph walk of vmhook::find_class): here every check exercises one
// of the *context-loader* / *reanchor* entry points and their loader-level
// semantics, against a fresh fixture (vmhook/fixtures/FindClassCtxLoader).
//
// Functions under test:
//   * vmhook::find_class(name)
//        -> vmhook::detail::jni_find_class_with_context_loader
//        the public multi-loader JNI resolver: thread context loader -> system
//        loader -> Minecraft Launch loader.  The resolver is ALREADY hardened in
//        the library (PushLocalFrame/PopLocalFrame around the whole walk, a
//        local_ref_bag that DeleteLocalRef's every handle, and an
//        ExceptionClear before/after every loadClass) so a miss / cold JDK 17+ /
//        freshly-attached thread can never fault — this module proves the
//        behaviour, never relies on an un-cleared exception.
//   * vmhook::find_class_via_oop(anchor_oop, name)
//        resolve `name` through the classloader of a LIVE anchor object.
//   * vmhook::reanchor_classes_via_oop(anchor_oop, {...})
//        + vmhook::override_class_lookup(name, k)
//        + vmhook::evict_class_lookup(name)
//        pin a set of names to the anchor loader's copy, then forget them.
//   * vmhook::detail::klass_to_class_loader_oop(klass)
//        the BOOTSTRAP-vs-context-loader comparison axis (bootstrap => null loader).
//   * vmhook::detail::capture_host_classloader_klass / host_classloader_klass
//        the first non-bootstrap klass find_class sees gets captured.
//   * USABILITY of every resolved klass: top-level vmhook::find_field(klass,name)
//        (super-chain walk, resolves BOTH static + instance fields) +
//        klass->get_java_mirror() + klass->get_name().
//
// ─────────────────────────────────────────────────────────────────────────────
// SUITE-SAFETY (this module was QUARANTINED in Wave 3 in a matrix-wide JVM-crash
// cascade; re-enabled here under the suite-safety rules in
// audit/PERFECTION_PROGRAM.md:400-403, mirroring the just-landed register_class.cpp
// / wrapper_pattern.cpp scaffold):
//
//   * NOT THE CRASHER, A VICTIM.  The Wave-3 cascade crasher was
//     hook_reinstall_after_shutdown's mid-suite GLOBAL shutdown_hooks() (it tore
//     down every other module's hooks/state); this module was a victim that ALSO
//     carried one deterministically-wrong assertion (the null-override "negative
//     entry" claim — see E5 + the BUG note there).  The context-loader resolver
//     itself does not crash: the library wraps the entire class-loader walk in a
//     JNI PushLocalFrame/PopLocalFrame frame, bags + DeleteLocalRef's every local
//     ref, and ExceptionClear's before/after every loadClass.  Off-thread misses
//     (Parts A/A2) and the empty-name input all return null cleanly.
//
//   * ZERO HOOKS ARMED ON EXIT.  The ONLY hook is the Part-E scoped_hook<> on
//     anchorTick(), which RAII-uninstalls at its inner-block scope exit.  The
//     module ALSO ends with an UNCONDITIONAL vmhook::shutdown_hooks() placed
//     OUTSIDE the body try/catch, so even if the body throws before that scope
//     exit, control returns to the driver with an empty hook table (mirrors
//     aaa_warmup.cpp / shutdown_hooks_teardown.cpp).  A leaked armed hook is
//     exactly what cascaded into later modules in Wave 3; this module cannot leak
//     one on ANY path.
//
//   * NEVER CRASH, BAIL TO [INFO].  The whole body runs under try/catch (a throw
//     is recorded as [INFO], never escapes).  An ENTRY GUARD skips the module
//     cleanly if the fixture does not resolve, so the unguarded
//     static_field("go")->set(...) handshake derefs can never touch a disengaged
//     optional.  EVERY klass / oop dereference is gated by is_valid_pointer; EVERY
//     resolver result is null-checked before use; drive() null-checks
//     ctx.run_probe before calling it.
//
//   * SHARED-CACHE DISCIPLINE.  override_class_lookup mutates the SHARED find_class
//     cache, so PART E SAVES and RESTORES every entry it touches (java/lang/String,
//     java/util/ArrayList, the fixture) before the scope ends — later modules see
//     the cache exactly as they would have.  evict is exercised then the original
//     re-seeded.
//
//   * NO FORCED GC.  This module/fixture never drives System.gc(), so the
//     forced-GC platform gate does not apply.
//
//   * WARMTH.  aaa_warmup (priority::first) pre-resolves bootstrap classes and pays
//     the first-deopt / compile-cycle cost before any feature module runs, so the
//     anchorTick() dispatch in Part E is not a cold-compile fault on the no-SEH
//     MinGW/clang toolchains.
//
// JDK / LOADER SENSITIVITY (best-effort gating):
//   - klass_to_class_loader_oop's bootstrap=null / app=non-null verdict is
//     universal HotSpot behaviour on JDK 8-26 (the library transparently handles
//     the JDK 8-9 direct-oop vs JDK 10+ OopHandle layout), so Part B is HARD.
//   - The HOST-CLASSLOADER CAPTURE (Part C) is process-global, first-wins, and
//     latched by whichever module first resolved an app class — so by the time
//     this module runs it is normally already published.  We assert it best-effort
//     ([INFO] when absent) because its presence is an emergent property of suite
//     ORDERING, not of this module in isolation (the audit's "java8
//     host-classloader" fragility lived here).
//   - find_class_with_context_loader for an APP class only succeeds on a thread
//     whose context loader IS the app loader — a real Java thread.  So the app
//     resolution is driven ONLY from inside the anchorTick() detour (Part E);
//     off-thread (Parts A/D) we use the thread-agnostic graph walk and treat the
//     context-loader app resolution off-thread as legitimately allowed to miss.
//   - net/minecraft/launchwrapper/Launch (the resolver's 3rd fallback) is absent
//     on a stock JDK; that branch is correctly skipped and never asserted.
//   - The house JDK-8 idiom (java/lang/String has no "coder" field on 8) is an
//     [INFO] generation marker only; nothing is gated on it.
//
// C++17 (no std::bit_cast); MSVC copy-init (=) from value_t/->get(), never
// brace-init.  No exception escapes the module body or the detour.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

namespace
{
    // Internal JVM names used throughout.
    constexpr char FIXTURE_NAME[]{ "vmhook/fixtures/FindClassCtxLoader" };
    constexpr char STRING_NAME[]{ "java/lang/String" };
    constexpr char OBJECT_NAME[]{ "java/lang/Object" };
    constexpr char INTEGER_NAME[]{ "java/lang/Integer" };
    constexpr char ARRAYLIST_NAME[]{ "java/util/ArrayList" };
    constexpr char MISSING_NAME[]{ "vmhook/fixtures/NoSuchClass_CtxLoader_ZZZ" };
    constexpr char MISSING_NAME2[]{ "com/example/ctxloader/Bogus" };
    constexpr char MISSING_NAME3[]{ "vmhook/fixtures/NoSuchClass_CtxLoader_QQQ" };
    constexpr char NESTED_NAME[]{ "vmhook/fixtures/FindClassCtxLoader$Nested" };
    constexpr char INT_ARRAY_NAME[]{ "[I" };
    constexpr char OBJ_ARRAY_NAME[]{ "[Ljava/lang/String;" };
    constexpr char PRIMITIVE_NAME[]{ "int" };
    constexpr std::int32_t SENTINEL_VALUE{ 0x0CAFEC0D };

    // A deliberately INVALID Klass* used to prove find_class's stale-cache guard
    // (vmhook.hpp ~8048) evicts a bogus pinned value and re-resolves — the live
    // counterpart of the no-JVM test's garbage-pin closure of the null-override
    // flaw.  Never dereferenced by the test; is_valid_pointer rejects it and the
    // library's own guard erases it.  A low, unmapped, odd address.
    auto garbage_klass() -> vmhook::hotspot::klass*
    {
        return reinterpret_cast<vmhook::hotspot::klass*>(
            static_cast<std::uintptr_t>(0xDEAD1));
    }

    // ── Wrapper for the app-loaded fixture.  object<> gives it a vtable
    //    (register_class<T> requires one) + the static_field / static_method
    //    accessors used to prove a resolved klass is usable end-to-end. ──
    class fcl : public vmhook::object<fcl>
    {
    public:
        explicit fcl(vmhook::oop_t instance) noexcept
            : vmhook::object<fcl>{ instance }
        {
        }

        // Unguarded handshake setters mirror hook_basic's idiom.  They are only
        // ever reached AFTER the module's entry guard has confirmed FIXTURE_NAME
        // resolves, so static_field(...) is engaged here (no deref of a disengaged
        // optional).
        static auto set_go(bool value) -> void { static_field("go")->set(value); }
        static auto get_done() -> bool         { return static_field("done")->get(); }

        static auto sentinel_field_resolves() -> bool
        {
            return static_field("sentinel").has_value();
        }
        static auto sentinel() -> std::int32_t { return static_field("sentinel")->get(); }
        static auto sentinel_via_getter() -> std::int32_t
        {
            const auto m{ static_method("getSentinel") };
            if (!m.has_value()) { return 0; }
            const std::int32_t v = m->call();
            return v;
        }
        static auto observed_sentinel() -> std::int32_t { return static_field("observedSentinel")->get(); }
        static auto witness_captured() -> bool { return static_field("witnessCaptured")->get(); }
    };

    // ── Guarded helpers (never deref a bad pointer). ──

    // A klass's own internal name as std::string, "" on any failure.
    auto klass_name(vmhook::hotspot::klass* const k) -> std::string
    {
        if (!k || !vmhook::hotspot::is_valid_pointer(k)) { return std::string{}; }
        vmhook::hotspot::symbol* const sym{ k->get_name() };
        if (!sym || !vmhook::hotspot::is_valid_pointer(sym)) { return std::string{}; }
        return sym->to_string();
    }

    // "this klass has a usable java.lang.Class mirror" (touches no field layout).
    auto klass_mirror_usable(vmhook::hotspot::klass* const k) -> bool
    {
        if (!k || !vmhook::hotspot::is_valid_pointer(k)) { return false; }
        void* const mirror{ k->get_java_mirror() };
        return mirror != nullptr && vmhook::hotspot::is_valid_pointer(mirror);
    }

    // Resolve the named field's entry on the klass via the super-chain-walking
    // top-level find_field — the public "is this klass usable for member access"
    // proof for a context-loader-resolved klass.  find_field resolves BOTH static
    // and instance fields (it walks the field stream), so it works for the static
    // `sentinel` and the instance `instanceMark` alike.
    auto klass_has_field(vmhook::hotspot::klass* const k, const char* name) -> bool
    {
        if (!k || !vmhook::hotspot::is_valid_pointer(k)) { return false; }
        return vmhook::find_field(k, name).has_value();
    }

    // ── PART E observations (filled INSIDE the anchorTick() detour on the Java
    //    thread, read back by the module body).  All atomic; sentinels chosen so
    //    "did the detour run?" is unambiguous. ──
    std::atomic<int>  g_detour_calls{ 0 };
    std::atomic<bool> g_detour_saw_self{ false };
    std::atomic<bool> g_detour_anchor_valid{ false };
    std::atomic<bool> g_detour_completed{ false };

    // (E1) context-loader resolver on the app fixture, from the Java thread.
    std::atomic<bool> g_ctx_fixture_nonnull{ false };
    std::atomic<bool> g_ctx_fixture_name_ok{ false };
    std::atomic<bool> g_ctx_fixture_mirror_ok{ false };
    std::atomic<bool> g_ctx_fixture_field_ok{ false };
    std::atomic<bool> g_ctx_fixture_idempotent{ false };

    // (E2) context-loader resolver on a bootstrap class (delegation) -> SAME klass
    //      as the graph walk.
    std::atomic<bool> g_ctx_string_matches_graph{ false };

    // (E3) context-loader resolver on a missing class -> null, no crash.
    std::atomic<bool> g_ctx_missing_null{ false };

    // (E4) find_class_via_oop against the live `self` anchor.
    std::atomic<bool> g_via_oop_fixture_nonnull{ false };
    std::atomic<bool> g_via_oop_fixture_name_ok{ false };
    std::atomic<bool> g_via_oop_fixture_matches_graph{ false };
    std::atomic<bool> g_via_oop_bootstrap_string_ok{ false };   // anchor sees bootstrap String
    std::atomic<bool> g_via_oop_missing_null{ false };
    std::atomic<bool> g_via_oop_null_anchor_null{ false };

    // (E5) reanchor_classes_via_oop + override/evict against the SHARED cache.
    std::atomic<bool> g_reanchor_returned_true{ false };
    std::atomic<bool> g_reanchor_find_class_follows{ false };   // find_class now == anchored klass
    std::atomic<bool> g_override_direct_roundtrips{ false };    // override then find_class returns it
    std::atomic<bool> g_evict_then_reresolves{ false };         // after evict, find_class re-resolves non-null
    std::atomic<bool> g_override_null_evicted_on_next_find{ false };
    std::atomic<bool> g_cache_restored_ok{ false };             // every touched entry put back

    // (E6) dotted-vs-slash safety on the context-loader resolver.
    std::atomic<bool> g_ctx_slash_form_ok{ false };
    std::atomic<bool> g_ctx_dotted_no_poison{ false };

    // (E7) NESTED ($-separated) name through the context loader + via_oop.
    std::atomic<bool> g_ctx_nested_nonnull{ false };
    std::atomic<bool> g_ctx_nested_name_ok{ false };
    std::atomic<bool> g_ctx_nested_field_ok{ false };
    std::atomic<bool> g_via_oop_nested_matches_graph{ false };

    // (E8) BOOTSTRAP-loaded anchor (java.lang.Object / java.lang.String instance):
    //      find_class_via_oop currently returns null because getClassLoader() is
    //      null and there is no bootstrap fallback (audit 13590-13593).  We record
    //      the OBSERVED result best-effort, and HARD-assert only "no crash, no
    //      cache poison".
    std::atomic<bool> g_via_oop_bootstrap_anchor_ran{ false };   // call returned, no crash
    std::atomic<bool> g_via_oop_bootstrap_anchor_resolved{ false }; // OBSERVED non-null?
    std::atomic<bool> g_via_oop_bootstrap_anchor_str_resolved{ false };

    // (E9) ARRAY names through the context-loader resolver (loadClass cannot resolve
    //      "[I" / "[L...;"; audit 11951-12193).  OBSERVED best-effort; no crash HARD.
    std::atomic<bool> g_ctx_array_prim_ran{ false };
    std::atomic<bool> g_ctx_array_prim_resolved{ false };
    std::atomic<bool> g_ctx_array_obj_ran{ false };
    std::atomic<bool> g_ctx_array_obj_resolved{ false };

    // (E10) PARTIAL reanchor: a { resolvable, NON-existent } list returns false,
    //       yet the resolvable name IS overridden (no rollback).  Pinned + restored.
    std::atomic<bool> g_partial_reanchor_returned_false{ false };
    std::atomic<bool> g_partial_reanchor_resolvable_still_pinned{ false };

    // (E11) reanchor return-value truth table from a REAL Java thread.
    std::atomic<bool> g_reanchor_empty_true{ false };       // {} -> true (vacuous)
    std::atomic<bool> g_reanchor_single_ok_true{ false };   // { resolvable } -> true
    std::atomic<bool> g_reanchor_all_fail_false{ false };   // { miss, miss } -> false
    std::atomic<bool> g_reanchor_all_succeed_true{ false }; // { resolvable, resolvable } -> true

    // (E12) garbage-Klass override is healed by find_class's stale-cache validation
    //       (the live counterpart of the no-JVM test's flaw-1 closure).
    std::atomic<bool> g_garbage_override_evicted_by_find_class{ false };

    // (E13) host_classloader_klass is a stable Metaspace pointer across further
    //       resolutions (GC-free stability; the module forbids forced GC).
    std::atomic<bool> g_host_klass_stable_across_resolutions{ false };

    // (E14) name-shape matrix on the context-loader resolver: every shape returns
    //       cleanly (no crash) and leaves the '/'-keyed find_class cache unpoisoned.
    std::atomic<bool> g_ctx_name_matrix_no_crash{ false };
    std::atomic<bool> g_ctx_name_matrix_no_cache_poison{ false };

    // Drive the single probe that fires the anchorTick hook + runs captureWitness().
    auto drive(vmhook_test::context& ctx) -> bool
    {
        if (!ctx.run_probe)
        {
            return false;
        }
        return ctx.run_probe(
            [](bool value)
            {
                if (value) { fcl::static_field("done")->set(false); }
                fcl::set_go(value);
            },
            []() { return fcl::get_done(); });
    }

    // The anchorTick() detour: every context-loader / reanchor entry point, run on
    // the Java thread with `self` as the app-loader anchor.  Self is `this`.
    //
    // GC safety on the anchor oop: `anchor` is `self`'s base-qualified raw OOP, used
    // IMMEDIATELY inside the detour while the interpreter frame keeps the receiver
    // live; it is never stored across the probe boundary.  Nothing held past this
    // detour except plain Klass* (Metaspace-stable across GC).
    auto on_anchor_tick(vmhook::return_value& /*ret*/, const std::unique_ptr<fcl>& self) -> void
    {
        g_detour_calls.fetch_add(1, std::memory_order_relaxed);
        g_detour_saw_self.store(self != nullptr, std::memory_order_relaxed);

        void* const anchor{ self ? self->vmhook::object_base::get_instance() : nullptr };
        const bool anchor_valid{ anchor != nullptr && vmhook::hotspot::is_valid_pointer(anchor) };
        g_detour_anchor_valid.store(anchor_valid, std::memory_order_relaxed);

        // ── (E1) context-loader resolver on the app fixture (Java thread). ──
        vmhook::hotspot::klass* const ctx_fixture{
            vmhook::find_class(FIXTURE_NAME) };
        g_ctx_fixture_nonnull.store(ctx_fixture != nullptr, std::memory_order_relaxed);
        if (ctx_fixture != nullptr && vmhook::hotspot::is_valid_pointer(ctx_fixture))
        {
            g_ctx_fixture_name_ok.store(klass_name(ctx_fixture) == FIXTURE_NAME,
                                        std::memory_order_relaxed);
            g_ctx_fixture_mirror_ok.store(klass_mirror_usable(ctx_fixture),
                                          std::memory_order_relaxed);
            // Usable: the sentinel static field resolves on the resolved klass.
            g_ctx_fixture_field_ok.store(klass_has_field(ctx_fixture, "sentinel"),
                                         std::memory_order_relaxed);
        }
        vmhook::hotspot::klass* const ctx_fixture2{
            vmhook::find_class(FIXTURE_NAME) };
        g_ctx_fixture_idempotent.store(
            ctx_fixture != nullptr && ctx_fixture == ctx_fixture2,
            std::memory_order_relaxed);

        // ── (E2) context-loader resolver on bootstrap String -> SAME as graph. ──
        vmhook::hotspot::klass* const ctx_string{
            vmhook::find_class(STRING_NAME) };
        vmhook::hotspot::klass* const graph_string{ vmhook::find_class(STRING_NAME) };
        g_ctx_string_matches_graph.store(
            ctx_string != nullptr && ctx_string == graph_string,
            std::memory_order_relaxed);

        // ── (E3) context-loader resolver on a missing class -> null, no crash. ──
        vmhook::hotspot::klass* const ctx_missing{
            vmhook::find_class(MISSING_NAME) };
        g_ctx_missing_null.store(ctx_missing == nullptr, std::memory_order_relaxed);

        // ── (E4) find_class_via_oop against the live anchor. ──
        if (anchor_valid)
        {
            vmhook::hotspot::klass* const via_fixture{
                vmhook::find_class_via_oop(anchor, FIXTURE_NAME) };
            g_via_oop_fixture_nonnull.store(via_fixture != nullptr, std::memory_order_relaxed);
            g_via_oop_fixture_name_ok.store(klass_name(via_fixture) == FIXTURE_NAME,
                                            std::memory_order_relaxed);
            vmhook::hotspot::klass* const graph_fixture{ vmhook::find_class(FIXTURE_NAME) };
            g_via_oop_fixture_matches_graph.store(
                via_fixture != nullptr && via_fixture == graph_fixture,
                std::memory_order_relaxed);

            // The anchor's loader delegates to the bootstrap loader, so String
            // resolves to the SAME bootstrap klass through the anchor.
            vmhook::hotspot::klass* const via_string{
                vmhook::find_class_via_oop(anchor, STRING_NAME) };
            g_via_oop_bootstrap_string_ok.store(
                via_string != nullptr && via_string == graph_string,
                std::memory_order_relaxed);

            // A missing class through the anchor loader -> null, no crash.
            vmhook::hotspot::klass* const via_missing{
                vmhook::find_class_via_oop(anchor, MISSING_NAME2) };
            g_via_oop_missing_null.store(via_missing == nullptr, std::memory_order_relaxed);
        }
        // A null anchor -> null, no crash (guard at the very top of find_class_via_oop).
        g_via_oop_null_anchor_null.store(
            vmhook::find_class_via_oop(nullptr, FIXTURE_NAME) == nullptr,
            std::memory_order_relaxed);

        // ── (E5) reanchor + override/evict, with FULL cache save/restore. ──
        // Save the current cache entries we are about to perturb so later modules
        // are unaffected.  For these always-loadable classes find_class warms +
        // returns the real klass; we restore each below to exactly what we observed.
        const auto saved_string{ vmhook::find_class(STRING_NAME) };       // warms+returns
        const auto saved_arraylist{ vmhook::find_class(ARRAYLIST_NAME) };
        const auto saved_fixture{ vmhook::find_class(FIXTURE_NAME) };

        if (anchor_valid)
        {
            // reanchor a mix: the app fixture + a delegated platform class.  Both
            // are resolvable through the anchor loader, so the call returns true.
            const bool reanchored{ vmhook::reanchor_classes_via_oop(
                anchor, { FIXTURE_NAME, ARRAYLIST_NAME }) };
            g_reanchor_returned_true.store(reanchored, std::memory_order_relaxed);

            // After reanchor, find_class(name) returns the anchored copy directly
            // (override_class_lookup seeded the cache from find_class_via_oop).
            vmhook::hotspot::klass* const anchored_fixture{
                vmhook::find_class_via_oop(anchor, FIXTURE_NAME) };
            g_reanchor_find_class_follows.store(
                anchored_fixture != nullptr
                && vmhook::find_class(FIXTURE_NAME) == anchored_fixture,
                std::memory_order_relaxed);
        }

        // override_class_lookup direct: pin String to a known klass, confirm
        // find_class returns exactly it, then evict and re-resolve.
        if (saved_string != nullptr)
        {
            vmhook::override_class_lookup(STRING_NAME, saved_string);
            g_override_direct_roundtrips.store(
                vmhook::find_class(STRING_NAME) == saved_string,
                std::memory_order_relaxed);

            vmhook::evict_class_lookup(STRING_NAME);
            vmhook::hotspot::klass* const re_string{ vmhook::find_class(STRING_NAME) };
            g_evict_then_reresolves.store(re_string != nullptr, std::memory_order_relaxed);

            // A NULL override is NOT a working negative cache entry.  find_class's
            // cache-hit guard requires `cached_klass && is_valid_pointer(cached_klass)`
            // to RETURN a hit; a cached nullptr fails that guard, so find_class
            // EVICTS the entry and re-walks the graph, re-resolving non-null on the
            // very next call.  So the correct, observable contract is: after a null
            // override, the NEXT find_class re-resolves the class non-null (the
            // override was silently dropped).  The library's own no-JVM test
            // (tests/test_classloader_reanchor.cpp) documents the same: it must
            // inspect klass_lookup_cache directly, NOT via find_class, precisely
            // because find_class validates+evicts a non-surviving sentinel.
            //
            // BUG NOTE (library, not test): override_class_lookup's doc comment
            // claims "Passing a null `k` seeds a negative entry"; in fact null is
            // never a live negative entry given the cache-hit validation.  Reported
            // separately; this assertion pins the ACTUAL behaviour so the suite is
            // honest.  (The prior .wip asserted find_class==nullptr here, which
            // failed deterministically on every JDK — it was the wrong assertion.)
            vmhook::override_class_lookup(STRING_NAME, nullptr);
            const bool reresolved_nonnull{ vmhook::find_class(STRING_NAME) != nullptr };
            g_override_null_evicted_on_next_find.store(reresolved_nonnull,
                                                       std::memory_order_relaxed);
        }

        // ── Restore the cache to its pre-perturbation state. ──
        bool restored{ true };
        if (saved_string != nullptr) { vmhook::override_class_lookup(STRING_NAME, saved_string); }
        else { vmhook::evict_class_lookup(STRING_NAME); }
        if (saved_arraylist != nullptr) { vmhook::override_class_lookup(ARRAYLIST_NAME, saved_arraylist); }
        else { vmhook::evict_class_lookup(ARRAYLIST_NAME); }
        if (saved_fixture != nullptr) { vmhook::override_class_lookup(FIXTURE_NAME, saved_fixture); }
        else { vmhook::evict_class_lookup(FIXTURE_NAME); }
        restored = restored
            && (saved_string == nullptr || vmhook::find_class(STRING_NAME) == saved_string)
            && (saved_arraylist == nullptr || vmhook::find_class(ARRAYLIST_NAME) == saved_arraylist)
            && (saved_fixture == nullptr || vmhook::find_class(FIXTURE_NAME) == saved_fixture);
        // NOTE: g_cache_restored_ok is published at the VERY END of the detour
        // (after E10/E11/E12 which also perturb + self-restore the same keys), so
        // the suite-safety guarantee covers EVERY perturbation, not just E5's.
        bool e5_restored_ok{ restored };

        // ── (E6) dotted-vs-slash safety on the context-loader resolver. ──
        vmhook::hotspot::klass* const ctx_slash{
            vmhook::find_class(STRING_NAME) };
        g_ctx_slash_form_ok.store(
            ctx_slash != nullptr && ctx_slash == graph_string,
            std::memory_order_relaxed);
        // A dotted name fed to the context-loader resolver must not crash and must
        // not poison the '/'-form result (the cache is '/'-keyed; this resolver
        // does not touch the find_class cache, but assert end-to-end safety).
        (void) vmhook::find_class("java.lang.String");
        g_ctx_dotted_no_poison.store(
            vmhook::find_class(STRING_NAME) == graph_string, std::memory_order_relaxed);

        // ── (E7) NESTED ($-separated) name through the context loader + via_oop. ──
        // The nested binary name vmhook/fixtures/FindClassCtxLoader$Nested must
        // resolve through the app context loader (the $ is part of the name handed
        // to loadClass after '/'->'.' replacement; '$' is untouched), and the
        // resolved klass must be usable (its own static field resolves).
        vmhook::hotspot::klass* const ctx_nested{
            vmhook::find_class(NESTED_NAME) };
        g_ctx_nested_nonnull.store(ctx_nested != nullptr, std::memory_order_relaxed);
        if (ctx_nested != nullptr && vmhook::hotspot::is_valid_pointer(ctx_nested))
        {
            g_ctx_nested_name_ok.store(klass_name(ctx_nested) == NESTED_NAME,
                                       std::memory_order_relaxed);
            g_ctx_nested_field_ok.store(klass_has_field(ctx_nested, "nestedSentinel"),
                                        std::memory_order_relaxed);
        }
        if (anchor_valid)
        {
            vmhook::hotspot::klass* const via_nested{
                vmhook::find_class_via_oop(anchor, NESTED_NAME) };
            vmhook::hotspot::klass* const graph_nested{ vmhook::find_class(NESTED_NAME) };
            g_via_oop_nested_matches_graph.store(
                via_nested != nullptr && via_nested == graph_nested,
                std::memory_order_relaxed);
        }

        // ── (E8) BOOTSTRAP-loaded anchor: find_class_via_oop against a live
        //         java.lang.Object / java.lang.String instance whose getClassLoader()
        //         is null.  Audit 13590-13593: the resolver treats a null loader as
        //         failure and returns nullptr with no bootstrap fallback.  We get the
        //         anchor's RAW oop GC-safely via field_oop (used immediately, never
        //         stored past the detour) and RECORD the observed result best-effort;
        //         the only HARD requirement (asserted in the body via no-poison) is
        //         that the call is crash-free and does not poison the find_class cache.
        {
            const auto boot_field{ fcl::static_field("bootstrapAnchor") };
            void* const boot_oop{ boot_field.has_value()
                ? vmhook::field_oop(*boot_field) : nullptr };
            if (boot_oop != nullptr && vmhook::hotspot::is_valid_pointer(boot_oop))
            {
                vmhook::hotspot::klass* const via_boot{
                    vmhook::find_class_via_oop(boot_oop, STRING_NAME) };
                g_via_oop_bootstrap_anchor_resolved.store(via_boot != nullptr,
                                                          std::memory_order_relaxed);
            }
            // Reaching here means the bootstrap-anchor via_oop call (or its skip on a
            // non-decodable oop) completed without faulting — the HARD requirement.
            g_via_oop_bootstrap_anchor_ran.store(true, std::memory_order_relaxed);
            const auto boot_str_field{ fcl::static_field("bootstrapStringAnchor") };
            void* const boot_str_oop{ boot_str_field.has_value()
                ? vmhook::field_oop(*boot_str_field) : nullptr };
            if (boot_str_oop != nullptr && vmhook::hotspot::is_valid_pointer(boot_str_oop))
            {
                vmhook::hotspot::klass* const via_boot_str{
                    vmhook::find_class_via_oop(boot_str_oop, OBJECT_NAME) };
                g_via_oop_bootstrap_anchor_str_resolved.store(via_boot_str != nullptr,
                                                              std::memory_order_relaxed);
            }
        }

        // ── (E9) ARRAY names through the context-loader resolver.  loadClass cannot
        //         resolve "[I" / "[L...;" (only FindClass / Class.forName do); audit
        //         11951-12193.  Crash-free is HARD; the resolution itself is OBSERVED
        //         best-effort (the library swallows the loadClass throw and returns
        //         null, so both are expected null on stock JDKs).
        {
            vmhook::hotspot::klass* const ctx_arr_prim{
                vmhook::find_class(INT_ARRAY_NAME) };
            g_ctx_array_prim_ran.store(true, std::memory_order_relaxed);
            g_ctx_array_prim_resolved.store(ctx_arr_prim != nullptr, std::memory_order_relaxed);

            vmhook::hotspot::klass* const ctx_arr_obj{
                vmhook::find_class(OBJ_ARRAY_NAME) };
            g_ctx_array_obj_ran.store(true, std::memory_order_relaxed);
            g_ctx_array_obj_resolved.store(ctx_arr_obj != nullptr, std::memory_order_relaxed);
        }

        // ── (E10) PARTIAL reanchor (flaw #3): a mixed { resolvable, NON-existent }
        //          list returns FALSE, yet the resolvable name IS overridden with no
        //          rollback.  We save FIXTURE's cache entry, drive the partial
        //          reanchor, observe that (a) it returned false and (b) FIXTURE is now
        //          pinned to the anchor-loader copy, then RESTORE FIXTURE.  This pins
        //          the documented no-all-or-nothing semantics so a future fix is a
        //          reviewed diff. ──
        if (anchor_valid)
        {
            const auto save_fixture_partial{ vmhook::find_class(FIXTURE_NAME) };
            vmhook::hotspot::klass* const anchored_fixture_partial{
                vmhook::find_class_via_oop(anchor, FIXTURE_NAME) };

            const bool partial{ vmhook::reanchor_classes_via_oop(
                anchor, { FIXTURE_NAME, MISSING_NAME3 }) };
            g_partial_reanchor_returned_false.store(partial == false,
                                                    std::memory_order_relaxed);
            g_partial_reanchor_resolvable_still_pinned.store(
                anchored_fixture_partial != nullptr
                && vmhook::find_class(FIXTURE_NAME) == anchored_fixture_partial,
                std::memory_order_relaxed);

            // Restore FIXTURE to exactly its pre-partial value.
            if (save_fixture_partial != nullptr)
            {
                vmhook::override_class_lookup(FIXTURE_NAME, save_fixture_partial);
            }
            else
            {
                vmhook::evict_class_lookup(FIXTURE_NAME);
            }

            // ── (E11) reanchor return-value truth table from a REAL Java thread. ──
            // Empty list -> vacuously true (no name fails).
            g_reanchor_empty_true.store(
                vmhook::reanchor_classes_via_oop(anchor, {}) == true,
                std::memory_order_relaxed);
            // Single resolvable name -> true.  Save/restore ARRAYLIST around it.
            {
                const auto save_al{ vmhook::find_class(ARRAYLIST_NAME) };
                g_reanchor_single_ok_true.store(
                    vmhook::reanchor_classes_via_oop(anchor, { ARRAYLIST_NAME }) == true,
                    std::memory_order_relaxed);
                if (save_al != nullptr) { vmhook::override_class_lookup(ARRAYLIST_NAME, save_al); }
                else { vmhook::evict_class_lookup(ARRAYLIST_NAME); }
            }
            // All-fail list -> false (no name resolves; nothing overridden).
            g_reanchor_all_fail_false.store(
                vmhook::reanchor_classes_via_oop(anchor, { MISSING_NAME, MISSING_NAME3 }) == false,
                std::memory_order_relaxed);
            // All-succeed list -> true.  Save/restore both names around it.
            {
                const auto save_al2{ vmhook::find_class(ARRAYLIST_NAME) };
                const auto save_fx2{ vmhook::find_class(FIXTURE_NAME) };
                g_reanchor_all_succeed_true.store(
                    vmhook::reanchor_classes_via_oop(anchor, { ARRAYLIST_NAME, FIXTURE_NAME }) == true,
                    std::memory_order_relaxed);
                if (save_al2 != nullptr) { vmhook::override_class_lookup(ARRAYLIST_NAME, save_al2); }
                else { vmhook::evict_class_lookup(ARRAYLIST_NAME); }
                if (save_fx2 != nullptr) { vmhook::override_class_lookup(FIXTURE_NAME, save_fx2); }
                else { vmhook::evict_class_lookup(FIXTURE_NAME); }
            }
        }

        // ── (E12) garbage-Klass override is HEALED by find_class's stale-cache
        //          validation (vmhook.hpp ~8048): a pinned bogus Klass* fails the
        //          is_valid_pointer / name-match guard, so the next find_class evicts
        //          it and re-resolves the real klass non-null.  This is the live
        //          counterpart of the no-JVM test's garbage-pin closure of flaw #1.
        //          Uses STRING_NAME (always re-resolvable); fully restored. ──
        {
            const auto save_str_g{ vmhook::find_class(STRING_NAME) };
            vmhook::override_class_lookup(STRING_NAME, garbage_klass());
            vmhook::hotspot::klass* const healed{ vmhook::find_class(STRING_NAME) };
            g_garbage_override_evicted_by_find_class.store(
                healed != nullptr && healed == save_str_g,
                std::memory_order_relaxed);
            if (save_str_g != nullptr) { vmhook::override_class_lookup(STRING_NAME, save_str_g); }
            else { vmhook::evict_class_lookup(STRING_NAME); }
        }

        // ── (E13) host_classloader_klass is a STABLE Metaspace pointer across more
        //          resolutions (no forced GC; Klass* never moves).  Snapshot, resolve
        //          several more classes (any of which may try to capture), re-read.
        //          Once published the value must NEVER change; the only tolerated
        //          transition is a legitimate first capture (null -> valid non-null),
        //          which is first-wins and then permanent. ──
        {
            vmhook::hotspot::klass* const host_before{
                vmhook::detail::host_classloader_klass.load(std::memory_order_acquire) };
            (void) vmhook::find_class(FIXTURE_NAME);
            (void) vmhook::find_class(ARRAYLIST_NAME);
            (void) vmhook::find_class(NESTED_NAME);
            vmhook::hotspot::klass* const host_after{
                vmhook::detail::host_classloader_klass.load(std::memory_order_acquire) };
            const bool unchanged{ host_before == host_after };
            const bool first_capture{ host_before == nullptr
                                      && host_after != nullptr
                                      && vmhook::hotspot::is_valid_pointer(host_after) };
            g_host_klass_stable_across_resolutions.store(unchanged || first_capture,
                                                         std::memory_order_relaxed);
        }

        // ── (E14) name-shape matrix on the context-loader resolver: every shape must
        //          return cleanly (no crash) and leave the '/'-keyed find_class cache
        //          for String unpoisoned.  The resolver dots the name and hands it to
        //          loadClass; degenerate shapes throw inside loadClass and the library
        //          swallows it, returning null.  We only require no crash + no poison;
        //          the per-shape resolution itself is not asserted here (loader/JDK-
        //          variant), only that the family is safe. ──
        {
            // Resolve String once to get the canonical '/'-keyed cached value.
            vmhook::hotspot::klass* const before_poison{ vmhook::find_class(STRING_NAME) };
            // Each of these must be safe to call and must not throw out of the detour.
            (void) vmhook::find_class("");
            (void) vmhook::find_class("a");
            (void) vmhook::find_class("/leading/slash");
            (void) vmhook::find_class("trailing/slash/");
            (void) vmhook::find_class("double//slash");
            (void) vmhook::find_class("//");
            (void) vmhook::find_class(PRIMITIVE_NAME);
            (void) vmhook::find_class(INT_ARRAY_NAME);
            (void) vmhook::find_class(OBJ_ARRAY_NAME);
            (void) vmhook::find_class("java.lang.Object");
            (void) vmhook::find_class(NESTED_NAME);
            (void) vmhook::find_class(
                std::string(8192, 'a').c_str());
            g_ctx_name_matrix_no_crash.store(true, std::memory_order_relaxed);
            // The '/'-keyed String cache entry is untouched by any of the above
            // (the context-loader resolver never writes the find_class cache).
            g_ctx_name_matrix_no_cache_poison.store(
                vmhook::find_class(STRING_NAME) == before_poison,
                std::memory_order_relaxed);
        }

        // ── FINAL cache restoration verification.  E5, E10, E11 and E12 each
        //    save/restore the keys they perturb (STRING / ARRAYLIST / FIXTURE), but
        //    only an END-OF-DETOUR re-check proves the cache is back to its E5
        //    baseline AFTER all of them ran.  Re-pin each to its E5-observed value
        //    (idempotent if already correct) and confirm find_class agrees, ANDing
        //    in E5's own restore verdict.  This is the suite-safety guarantee:
        //    every entry this detour touched is left exactly as later modules expect.
        {
            if (saved_string != nullptr) { vmhook::override_class_lookup(STRING_NAME, saved_string); }
            else { vmhook::evict_class_lookup(STRING_NAME); }
            if (saved_arraylist != nullptr) { vmhook::override_class_lookup(ARRAYLIST_NAME, saved_arraylist); }
            else { vmhook::evict_class_lookup(ARRAYLIST_NAME); }
            if (saved_fixture != nullptr) { vmhook::override_class_lookup(FIXTURE_NAME, saved_fixture); }
            else { vmhook::evict_class_lookup(FIXTURE_NAME); }
            const bool final_restored{
                e5_restored_ok
                && (saved_string == nullptr || vmhook::find_class(STRING_NAME) == saved_string)
                && (saved_arraylist == nullptr || vmhook::find_class(ARRAYLIST_NAME) == saved_arraylist)
                && (saved_fixture == nullptr || vmhook::find_class(FIXTURE_NAME) == saved_fixture) };
            g_cache_restored_ok.store(final_restored, std::memory_order_relaxed);
        }

        // Reaching here means no exception / AV escaped any resolver call.
        g_detour_completed.store(true, std::memory_order_relaxed);
    }

    // The whole test body, factored out so the VMHOOK_JVM_MODULE wrapper can run
    // it under a try/catch and ALWAYS follow it with shutdown_hooks() (suite-
    // safety: ZERO hooks armed on EVERY exit path).
    auto run_find_class_context_loader_checks(vmhook_test::context& ctx) -> void
    {
        // =====================================================================
        //  ENTRY GUARD.  If FindClassCtxLoader is not loaded/resolvable, every
        //  static_field()->set/get below (the go/done handshake) and every
        //  resolver target would be unavailable.  Bail cleanly to [INFO] instead
        //  of dereferencing anything (the wrapper's final shutdown_hooks() still
        //  runs).  In practice the harness loads every vmhook.fixtures.* class on
        //  each run, so this is belt-and-braces (same idiom as register_class /
        //  wrapper_pattern / hook_basic).
        // =====================================================================
        if (vmhook::find_class(FIXTURE_NAME) == nullptr)
        {
            ctx.record("[INFO] find_class_context_loader: FindClassCtxLoader not "
                       "loaded/resolvable on this run; skipping the module's live "
                       "checks (no crash, no hooks armed).");
            return;
        }

        vmhook::register_class<fcl>(FIXTURE_NAME);

        // =====================================================================
        //  0. Sanity: the app fixture resolves and its hook target is declared.
        // =====================================================================
        {
            vmhook::hotspot::klass* const k_fixture{ vmhook::find_class(FIXTURE_NAME) };
            ctx.check("fixture_resolves_nonnull", k_fixture != nullptr);
            ctx.check("fixture_name_matches", klass_name(k_fixture) == FIXTURE_NAME);
            ctx.check("fixture_sentinel_field_resolves", fcl::sentinel_field_resolves());
            ctx.check("fixture_sentinel_value", fcl::sentinel() == SENTINEL_VALUE);
            ctx.check("fixture_sentinel_via_getter", fcl::sentinel_via_getter() == SENTINEL_VALUE);

            const auto methods{ vmhook::get_class_methods<fcl>() };
            bool has_tick{ false };
            for (const auto& entry : methods)
            {
                if (entry.first == "anchorTick") { has_tick = true; break; }
            }
            ctx.check("fixture_anchorTick_declared", has_tick);
        }

        // House JDK-8 generation marker (recorded only; nothing is gated away).
        {
            vmhook::hotspot::klass* const k_string{ vmhook::find_class(STRING_NAME) };
            const bool compact_strings{ k_string != nullptr
                                        && k_string->find_field("coder").has_value() };
            ctx.record(std::string{ "[INFO] find_class_context_loader: JDK generation = " }
                       + (compact_strings ? "9+ (String.coder present)" : "8 (no String.coder)"));
        }

        // =====================================================================
        //  PART A — PUBLIC context-loader resolver on BOOTSTRAP classes, from the
        //  worker thread.  java/lang/* are visible to every loader by delegation, so
        //  find_class_with_context_loader resolves them and each klass is USABLE:
        //  name round-trips, the java.lang.Class mirror is valid, and a known field
        //  entry resolves through the super-chain-walking find_field.
        // =====================================================================
        {
            vmhook::hotspot::klass* const k_string{
                vmhook::find_class(STRING_NAME) };
            ctx.check("ctx_bootstrap_String_nonnull", k_string != nullptr);
            ctx.check("ctx_bootstrap_String_name_matches", klass_name(k_string) == STRING_NAME);
            ctx.check("ctx_bootstrap_String_mirror_usable", klass_mirror_usable(k_string));
            ctx.check("ctx_bootstrap_String_has_value_field", klass_has_field(k_string, "value"));

            vmhook::hotspot::klass* const k_object{
                vmhook::find_class(OBJECT_NAME) };
            ctx.check("ctx_bootstrap_Object_nonnull", k_object != nullptr);
            ctx.check("ctx_bootstrap_Object_name_matches", klass_name(k_object) == OBJECT_NAME);
            ctx.check("ctx_bootstrap_Object_mirror_usable", klass_mirror_usable(k_object));

            vmhook::hotspot::klass* const k_integer{
                vmhook::find_class(INTEGER_NAME) };
            ctx.check("ctx_bootstrap_Integer_nonnull", k_integer != nullptr);
            ctx.check("ctx_bootstrap_Integer_name_matches", klass_name(k_integer) == INTEGER_NAME);
            ctx.check("ctx_bootstrap_Integer_has_value_field", klass_has_field(k_integer, "value"));

            // Distinct names -> distinct klasses (no single-slot aliasing).
            ctx.check("ctx_bootstrap_distinct_klasses",
                      k_string != nullptr && k_object != nullptr && k_integer != nullptr
                      && k_string != k_object && k_object != k_integer && k_string != k_integer);

            // The context-loader resolver and the graph walk agree on the SAME klass
            // for a bootstrap class (resolver #1 of the bootstrap-vs-context compare).
            vmhook::hotspot::klass* const graph_string{ vmhook::find_class(STRING_NAME) };
            ctx.check("ctx_resolver_matches_graph_for_bootstrap",
                      k_string != nullptr && k_string == graph_string);
        }

        // =====================================================================
        //  PART A2 — context-loader resolver MISS contract, off-thread.  A
        //  never-loaded class returns null (no crash) directly AND on a tight repeat
        //  loop (the resolver caches nothing on a miss, so it re-walks every loader
        //  path each time and must stay safe + null).  The empty-name input is
        //  rejected up front by the library (find_class short-circuits "" before the
        //  JNI loadClass("") path on cold JDK 17+).
        // =====================================================================
        {
            ctx.check("ctx_missing_returns_null",
                      vmhook::find_class(MISSING_NAME) == nullptr);
            ctx.check("ctx_missing2_returns_null",
                      vmhook::find_class(MISSING_NAME2) == nullptr);
            ctx.check("ctx_empty_name_returns_null",
                      vmhook::find_class("") == nullptr);

            bool all_null{ true };
            for (int i{ 0 }; i < 8; ++i)
            {
                if (vmhook::find_class(MISSING_NAME) != nullptr)
                {
                    all_null = false;
                    break;
                }
            }
            ctx.check("ctx_missing_repeated_stable_null", all_null);
        }

        // =====================================================================
        //  PART B — BOOTSTRAP vs CONTEXT-LOADER at the LOADER-OOP level (the task's
        //  headline comparison).  klass_to_class_loader_oop returns:
        //    * null  for a bootstrap-loaded class (java/lang/String, Object, Integer),
        //    * non-null for the APP-loaded fixture.
        //  This is universal HotSpot behaviour (the library transparently handles the
        //  JDK 8-9 direct-oop vs JDK 10+ OopHandle layout), so it is HARD on 8-26.
        // =====================================================================
        {
            vmhook::hotspot::klass* const k_string{ vmhook::find_class(STRING_NAME) };
            vmhook::hotspot::klass* const k_object{ vmhook::find_class(OBJECT_NAME) };
            vmhook::hotspot::klass* const k_integer{ vmhook::find_class(INTEGER_NAME) };
            vmhook::hotspot::klass* const k_fixture{ vmhook::find_class(FIXTURE_NAME) };

            ctx.check("loader_oop_String_is_null_bootstrap",
                      k_string != nullptr
                      && vmhook::detail::klass_to_class_loader_oop(k_string) == nullptr);
            ctx.check("loader_oop_Object_is_null_bootstrap",
                      k_object != nullptr
                      && vmhook::detail::klass_to_class_loader_oop(k_object) == nullptr);
            ctx.check("loader_oop_Integer_is_null_bootstrap",
                      k_integer != nullptr
                      && vmhook::detail::klass_to_class_loader_oop(k_integer) == nullptr);

            void* const fixture_loader{ k_fixture != nullptr
                ? vmhook::detail::klass_to_class_loader_oop(k_fixture) : nullptr };
            // The app-loaded fixture has a non-null class-loader oop on JDK 9+.  On
            // JDK 8 this resolved NULL on every CI config (mingw/msvc/clang java8):
            // the legacy JDK-8 direct-oop ClassLoaderData::_class_loader layout is
            // not resolved to the app loader oop here (a documented library
            // limitation on the legacy layout -- the bootstrap=null verdict above is
            // unaffected and stays HARD).  Best-effort: hard-assert the
            // app-vs-bootstrap loader-oop CONTRAST when the loader resolves (JDK 9+);
            // record [INFO] when it does not (JDK 8) rather than a spurious red FAIL.
            if (fixture_loader != nullptr
                && vmhook::hotspot::is_valid_pointer(fixture_loader))
            {
                ctx.check("loader_oop_fixture_is_nonnull_app", true);
                // The contrast is the whole point: app loader differs from "no loader".
                ctx.check("loader_oop_app_differs_from_bootstrap_null",
                          vmhook::detail::klass_to_class_loader_oop(k_string) == nullptr);
            }
            else
            {
                ctx.record("[INFO] find_class_context_loader: app class-loader oop for "
                           "the fixture resolved null (JDK-8 direct-oop ClassLoaderData "
                           "layout -- library limitation on the legacy layout); the "
                           "bootstrap-loader=null verdicts above still hold.  The "
                           "app-vs-bootstrap loader-oop contrast is hard-asserted on "
                           "JDK 9+.");
            }

            // A null / invalid klass argument -> null loader oop, no crash.
            ctx.check("loader_oop_null_klass_is_null",
                      vmhook::detail::klass_to_class_loader_oop(nullptr) == nullptr);
        }

        // =====================================================================
        //  PART C — HOST-CLASSLOADER CAPTURE.  find_class latches the first
        //  non-bootstrap klass it sees into detail::host_classloader_klass (so newly
        //  attached worker threads can inherit the app loader).
        //
        //  BEST-EFFORT: this capture is PROCESS-GLOBAL and first-wins — whichever
        //  module first resolved an app class published it, so by the time this
        //  module runs it is normally already set.  Its presence is an emergent
        //  property of suite ORDERING, not of this module in isolation (the audit's
        //  "java8 host-classloader" fragility lived exactly here).  So we RECORD an
        //  [INFO] when it is absent rather than hard-failing, while still HARD-
        //  asserting the structural invariants (valid + non-bootstrap + idempotent +
        //  no-bootstrap-overwrite) WHEN a host klass is present.
        // =====================================================================
        {
            // Ensure the app fixture has been resolved at least once (capture trigger).
            (void) vmhook::find_class(FIXTURE_NAME);

            vmhook::hotspot::klass* const host{
                vmhook::detail::host_classloader_klass.load(std::memory_order_acquire) };
            if (host == nullptr)
            {
                ctx.record("[INFO] find_class_context_loader: host_classloader_klass not "
                           "captured at this point (no non-bootstrap app class had been "
                           "resolved yet, or this is an all-bootclasspath setup); the "
                           "capture invariants below are skipped best-effort.");
            }
            else
            {
                ctx.check("host_classloader_klass_valid", vmhook::hotspot::is_valid_pointer(host));
                // The captured klass MUST be non-bootstrap (capture skips null-loader
                // candidates), so its loader oop is non-null.
                void* const host_loader{ vmhook::detail::klass_to_class_loader_oop(host) };
                ctx.check("host_classloader_klass_loader_nonnull",
                          host_loader != nullptr
                          && vmhook::hotspot::is_valid_pointer(host_loader));

                // Capture is idempotent: a second call publishes nothing new (same klass).
                vmhook::detail::capture_host_classloader_klass(vmhook::find_class(FIXTURE_NAME));
                vmhook::hotspot::klass* const host2{
                    vmhook::detail::host_classloader_klass.load(std::memory_order_acquire) };
                ctx.check("host_classloader_klass_capture_idempotent", host == host2);

                // Feeding a bootstrap klass never overwrites the captured host klass.
                vmhook::detail::capture_host_classloader_klass(vmhook::find_class(STRING_NAME));
                ctx.check("host_classloader_klass_bootstrap_no_overwrite",
                          vmhook::detail::host_classloader_klass.load(std::memory_order_acquire) == host);
            }
        }

        // =====================================================================
        //  PART D — USABILITY of the app klass, off-thread.  Beyond "non-null": its
        //  static sentinel + instance mark field entries both resolve through the
        //  top-level super-walking find_field, its mirror is valid, and the value
        //  read through the registered wrapper equals the baked sentinel.
        //
        //  NOTE: on the worker thread the context loader is NOT the app loader, so
        //  find_class_with_context_loader may legitimately MISS the app fixture; we
        //  therefore resolve the app klass through vmhook::find_class (graph walk,
        //  thread-agnostic) for the off-thread usability proof, and exercise the
        //  *context-loader* resolution of the app class on the JAVA thread in PART E.
        // =====================================================================
        {
            vmhook::hotspot::klass* const k_fixture{ vmhook::find_class(FIXTURE_NAME) };
            ctx.check("usable_fixture_nonnull", k_fixture != nullptr);
            ctx.check("usable_fixture_mirror_valid", klass_mirror_usable(k_fixture));
            ctx.check("usable_fixture_static_sentinel_field", klass_has_field(k_fixture, "sentinel"));
            ctx.check("usable_fixture_instance_mark_field", klass_has_field(k_fixture, "instanceMark"));
            // Field VALUES through the registered wrapper (full member-access proof).
            ctx.check("usable_fixture_sentinel_value", fcl::sentinel() == SENTINEL_VALUE);
            if (k_fixture != nullptr)
            {
                const auto inst_field{ vmhook::find_field(k_fixture, "instanceMark") };
                ctx.check("usable_fixture_instance_mark_entry_resolved", inst_field.has_value());
            }
            // A miss on the fixture klass for a non-existent field -> nullopt (graceful).
            ctx.check("usable_fixture_absent_field_is_nullopt",
                      k_fixture != nullptr
                      && !vmhook::find_field(k_fixture, "noSuchFieldZZZ").has_value());
        }

        // =====================================================================
        //  PART D2 — NESTED ($-separated) name, off-thread via the graph walk.  A
        //  nested static class FindClassCtxLoader$Nested must resolve by its binary
        //  '$'-bearing internal name through the thread-agnostic find_class, the
        //  resolved klass's name round-trips with the '$' intact, its mirror is
        //  valid, and its own static field resolves (full usability).  This is the
        //  off-thread, JDK-invariant proof; the context-loader + via_oop resolution
        //  of the SAME nested name on the Java thread is E7 in Part E.
        // =====================================================================
        {
            vmhook::hotspot::klass* const k_nested{ vmhook::find_class(NESTED_NAME) };
            ctx.check("nested_resolves_nonnull", k_nested != nullptr);
            ctx.check("nested_name_roundtrips_with_dollar",
                      klass_name(k_nested) == NESTED_NAME);
            ctx.check("nested_mirror_usable", klass_mirror_usable(k_nested));
            ctx.check("nested_static_field_resolves",
                      klass_has_field(k_nested, "nestedSentinel"));
            ctx.check("nested_instance_field_resolves",
                      klass_has_field(k_nested, "nestedInstanceMark"));
            // Distinct from the outer klass (no name aliasing across the '$').
            vmhook::hotspot::klass* const k_outer{ vmhook::find_class(FIXTURE_NAME) };
            ctx.check("nested_distinct_from_outer",
                      k_nested != nullptr && k_outer != nullptr && k_nested != k_outer);
        }

        // =====================================================================
        //  PART D3 — OFF-THREAD reanchor NULL-ANCHOR truth table + garbage-override
        //  eviction.  These need no Java thread (null anchor short-circuits before
        //  any JNI; the override path is pure cache memory), so they run regardless
        //  of whether the Part-E probe fires, and they leave the cache pristine.
        // =====================================================================
        {
            // reanchor(nullptr, ...) short-circuits to false for any non-empty list,
            // and to vacuous true for the empty list?  NO: the null-anchor guard
            // precedes the empty-list vacuous-true, so EVERY shape (incl. empty) is
            // false.  (This mirrors the no-JVM test's null-anchor truth table, here
            // confirmed under a LIVE JVM where a real anchor WOULD resolve.)
            ctx.check("offthread_reanchor_null_empty_false",
                      vmhook::reanchor_classes_via_oop(nullptr, {}) == false);
            ctx.check("offthread_reanchor_null_single_false",
                      vmhook::reanchor_classes_via_oop(nullptr, { FIXTURE_NAME }) == false);
            ctx.check("offthread_reanchor_null_pair_false",
                      vmhook::reanchor_classes_via_oop(
                          nullptr, { FIXTURE_NAME, ARRAYLIST_NAME }) == false);
            ctx.check("offthread_via_oop_null_anchor_null",
                      vmhook::find_class_via_oop(nullptr, FIXTURE_NAME) == nullptr);

            // Garbage-Klass override eviction (off-thread, on a SCRATCH name we own
            // entirely so we never perturb a class any other module uses).  Pin a
            // bogus Klass*, prove find_class evicts it (stale-cache guard ~8048) and
            // re-resolves; since the scratch name names no class, the re-resolve is
            // null AND the bogus entry is gone.  Then clean the scratch key.
            {
                constexpr char SCRATCH_NAME[]{ "vmhook/fixtures/CtxLoaderScratch_ZZZ" };
                // Snapshot whether the scratch key existed (it must not, but be safe).
                vmhook::evict_class_lookup(SCRATCH_NAME);
                vmhook::override_class_lookup(SCRATCH_NAME, garbage_klass());
                // The very next find_class must NOT return the garbage pointer; the
                // stale-cache validation rejects it (is_valid_pointer fails) and the
                // graph walk for a non-existent name yields null.
                vmhook::hotspot::klass* const after{ vmhook::find_class(SCRATCH_NAME) };
                ctx.check("offthread_garbage_override_not_returned",
                          after != garbage_klass());
                ctx.check("offthread_garbage_override_resolves_null", after == nullptr);
                // Leave the scratch key absent.
                vmhook::evict_class_lookup(SCRATCH_NAME);
            }
        }

        // =====================================================================
        //  PART D4 — CONCURRENCY smoke: many threads racing override_class_lookup /
        //  evict_class_lookup on a SCRATCH key the cache mutex
        //  (klass_lookup_cache_mutex) must serialise: no crash, and a deterministic
        //  clean final state (the key absent).
        //
        //  SUITE-SAFETY: the racing threads touch ONLY the pure cache helpers on a
        //  scratch name that names no class — they deliberately do NOT call
        //  find_class on a miss-name, which off the Java thread would route into the
        //  JNI context-loader fallback and AttachCurrentThread these worker threads
        //  (the heavier, no-SEH-cold-fault-prone path the module avoids).  The
        //  override-then-find_class HEAL interaction is instead exercised
        //  single-threaded in PART D3 and on the Java thread in E12.
        // =====================================================================
        {
            constexpr char RACE_NAME[]{ "vmhook/fixtures/CtxLoaderRace_ZZZ" };
            vmhook::evict_class_lookup(RACE_NAME);

            std::atomic<bool> any_threw{ false };
            std::atomic<int>  ops{ 0 };
            auto worker = [&]() noexcept
            {
                try
                {
                    for (int i{ 0 }; i < 300; ++i)
                    {
                        vmhook::override_class_lookup(RACE_NAME, garbage_klass());
                        vmhook::evict_class_lookup(RACE_NAME);
                        vmhook::override_class_lookup(RACE_NAME, garbage_klass());
                        vmhook::evict_class_lookup(RACE_NAME);
                        ops.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                catch (...) { any_threw.store(true, std::memory_order_relaxed); }
            };
            std::thread t1{ worker };
            std::thread t2{ worker };
            std::thread t3{ worker };
            t1.join();
            t2.join();
            t3.join();
            ctx.check("concurrent_cache_ops_no_throw", !any_threw.load());
            ctx.check("concurrent_cache_ops_ran", ops.load() == 900);
            // Final cleanup + deterministic end state: the key is absent.  (All
            // threads end on an evict, but enforce it explicitly regardless of the
            // racy interleaving.)
            vmhook::evict_class_lookup(RACE_NAME);
            ctx.check("concurrent_cache_final_state_clean",
                      vmhook::find_class(RACE_NAME) == nullptr);
        }

        // Flaw #5 documentation (bootclasspath-app capture gap): if a host
        // application appends its classes to the boot classpath, every app class
        // reads as bootstrap (null loader) and host_classloader_klass is never
        // published, silently disabling context-loader inheritance for worker
        // threads.  Stock-JDK test fixtures are app-loaded, so this never triggers
        // here; recorded so the gap is visible.
        ctx.record("[INFO] find_class_context_loader: capture_host_classloader_klass "
                   "skips bootstrap (null-loader) candidates - on an all-bootclasspath "
                   "app no host klass is ever published and context-loader inheritance "
                   "is silently inert (no diagnostic in the library).");

        // =====================================================================
        //  PART E — CONTEXT-LOADER + REANCHOR family driven from a JAVA THREAD.
        //  find_class_with_context_loader (app class), find_class_via_oop (live
        //  anchor), reanchor_classes_via_oop + override/evict (shared cache, fully
        //  save/restored).  The detour rides FindClassCtxLoader.anchorTick(); `self`
        //  is a live instance of the app fixture, so its loader == the app loader ==
        //  the anchor.  scoped_hook uninstalls on scope exit — nothing left armed.
        // =====================================================================
        {
            // Reset observation state so a re-run (or any prior fire) cannot leak in.
            g_detour_calls.store(0);
            g_detour_saw_self.store(false);
            g_detour_anchor_valid.store(false);
            g_detour_completed.store(false);
            g_ctx_fixture_nonnull.store(false);
            g_ctx_fixture_name_ok.store(false);
            g_ctx_fixture_mirror_ok.store(false);
            g_ctx_fixture_field_ok.store(false);
            g_ctx_fixture_idempotent.store(false);
            g_ctx_string_matches_graph.store(false);
            g_ctx_missing_null.store(false);
            g_via_oop_fixture_nonnull.store(false);
            g_via_oop_fixture_name_ok.store(false);
            g_via_oop_fixture_matches_graph.store(false);
            g_via_oop_bootstrap_string_ok.store(false);
            g_via_oop_missing_null.store(false);
            g_via_oop_null_anchor_null.store(false);
            g_reanchor_returned_true.store(false);
            g_reanchor_find_class_follows.store(false);
            g_override_direct_roundtrips.store(false);
            g_evict_then_reresolves.store(false);
            g_override_null_evicted_on_next_find.store(false);
            g_cache_restored_ok.store(false);
            g_ctx_slash_form_ok.store(false);
            g_ctx_dotted_no_poison.store(false);
            g_ctx_nested_nonnull.store(false);
            g_ctx_nested_name_ok.store(false);
            g_ctx_nested_field_ok.store(false);
            g_via_oop_nested_matches_graph.store(false);
            g_via_oop_bootstrap_anchor_ran.store(false);
            g_via_oop_bootstrap_anchor_resolved.store(false);
            g_via_oop_bootstrap_anchor_str_resolved.store(false);
            g_ctx_array_prim_ran.store(false);
            g_ctx_array_prim_resolved.store(false);
            g_ctx_array_obj_ran.store(false);
            g_ctx_array_obj_resolved.store(false);
            g_partial_reanchor_returned_false.store(false);
            g_partial_reanchor_resolvable_still_pinned.store(false);
            g_reanchor_empty_true.store(false);
            g_reanchor_single_ok_true.store(false);
            g_reanchor_all_fail_false.store(false);
            g_reanchor_all_succeed_true.store(false);
            g_garbage_override_evicted_by_find_class.store(false);
            g_host_klass_stable_across_resolutions.store(false);
            g_ctx_name_matrix_no_crash.store(false);
            g_ctx_name_matrix_no_cache_poison.store(false);

            auto handle{ vmhook::scoped_hook<fcl>("anchorTick", &on_anchor_tick) };
            ctx.check("anchor_hook_installed", handle.installed());
            if (handle.installed())
            {
                const bool done{ drive(ctx) };
                ctx.check("anchor_probe_completed", done);
                if (done)
                {
                    ctx.check("anchor_detour_fired", g_detour_calls.load() >= 1);
                    ctx.check("anchor_detour_saw_self", g_detour_saw_self.load());
                    ctx.check("anchor_detour_anchor_oop_valid", g_detour_anchor_valid.load());
                    ctx.check("anchor_detour_completed_no_throw", g_detour_completed.load());

                    // (E1) context-loader resolver on the APP class succeeds on the
                    //      Java thread and is usable (the headline: bootstrap walk
                    //      can miss an app class; the context loader resolves it).
                    ctx.check("E1_ctx_app_fixture_resolved", g_ctx_fixture_nonnull.load());
                    ctx.check("E1_ctx_app_fixture_name_matches", g_ctx_fixture_name_ok.load());
                    ctx.check("E1_ctx_app_fixture_mirror_usable", g_ctx_fixture_mirror_ok.load());
                    ctx.check("E1_ctx_app_fixture_field_resolves", g_ctx_fixture_field_ok.load());
                    ctx.check("E1_ctx_app_fixture_idempotent", g_ctx_fixture_idempotent.load());

                    // (E2) bootstrap via the context loader == graph walk.
                    ctx.check("E2_ctx_bootstrap_matches_graph", g_ctx_string_matches_graph.load());

                    // (E3) missing via the context loader -> null, no crash.
                    ctx.check("E3_ctx_missing_null", g_ctx_missing_null.load());

                    // (E4) find_class_via_oop against the live anchor + guards.
                    ctx.check("E4_via_oop_fixture_resolved", g_via_oop_fixture_nonnull.load());
                    ctx.check("E4_via_oop_fixture_name_matches", g_via_oop_fixture_name_ok.load());
                    ctx.check("E4_via_oop_fixture_matches_graph", g_via_oop_fixture_matches_graph.load());
                    ctx.check("E4_via_oop_anchor_sees_bootstrap_String",
                              g_via_oop_bootstrap_string_ok.load());
                    ctx.check("E4_via_oop_missing_null", g_via_oop_missing_null.load());
                    ctx.check("E4_via_oop_null_anchor_null", g_via_oop_null_anchor_null.load());

                    // (E5) reanchor + override/evict, with the shared cache restored.
                    ctx.check("E5_reanchor_returned_true", g_reanchor_returned_true.load());
                    ctx.check("E5_reanchor_find_class_follows", g_reanchor_find_class_follows.load());
                    ctx.check("E5_override_direct_roundtrips", g_override_direct_roundtrips.load());
                    ctx.check("E5_evict_then_reresolves", g_evict_then_reresolves.load());
                    // A null override is silently evicted by the next find_class (it is
                    // NOT a live negative entry — see the detour's BUG NOTE); the class
                    // re-resolves non-null.  This is the corrected assertion.
                    ctx.check("E5_null_override_evicted_on_next_find",
                              g_override_null_evicted_on_next_find.load());
                    ctx.check("E5_shared_cache_restored", g_cache_restored_ok.load());

                    // (E6) dotted-vs-slash safety on the context-loader resolver.
                    ctx.check("E6_ctx_slash_form_matches_graph", g_ctx_slash_form_ok.load());
                    ctx.check("E6_ctx_dotted_no_cache_poison", g_ctx_dotted_no_poison.load());

                    // (E7) NESTED ($-separated) name through the context loader +
                    //      via_oop.  The $ is part of the binary name; loadClass
                    //      resolves it on the app loader and the klass is usable.
                    ctx.check("E7_ctx_nested_resolved", g_ctx_nested_nonnull.load());
                    ctx.check("E7_ctx_nested_name_matches", g_ctx_nested_name_ok.load());
                    ctx.check("E7_ctx_nested_field_resolves", g_ctx_nested_field_ok.load());
                    ctx.check("E7_via_oop_nested_matches_graph", g_via_oop_nested_matches_graph.load());

                    // (E8) BOOTSTRAP-loaded anchor via find_class_via_oop.  HARD: the
                    //      call ran crash-free.  The resolution itself is OBSERVED
                    //      best-effort — audit 13590-13593 documents that the resolver
                    //      treats the null (bootstrap) loader as failure and returns
                    //      null with no bootstrap fallback, so this is normally false.
                    ctx.check("E8_via_oop_bootstrap_anchor_no_crash",
                              g_via_oop_bootstrap_anchor_ran.load());
                    ctx.record(std::string{ "[INFO] find_class_context_loader: "
                               "find_class_via_oop against a BOOTSTRAP-loaded anchor "
                               "(java.lang.Object instance, getClassLoader()==null) "
                               "resolved java/lang/String = " }
                               + (g_via_oop_bootstrap_anchor_resolved.load() ? "non-null"
                                  : "null (documented: no bootstrap fallback, audit "
                                    "13590-13593)"));
                    ctx.record(std::string{ "[INFO] find_class_context_loader: "
                               "find_class_via_oop against a bootstrap String anchor "
                               "resolved java/lang/Object = " }
                               + (g_via_oop_bootstrap_anchor_str_resolved.load() ? "non-null"
                                  : "null (same bootstrap-loader limitation)"));

                    // (E9) ARRAY names through the context-loader resolver.  HARD: no
                    //      crash.  OBSERVED best-effort — loadClass cannot resolve
                    //      "[I" / "[L...;" (audit 11951-12193), so both are normally
                    //      null even though the find_class graph walk would find them.
                    ctx.check("E9_ctx_array_prim_no_crash", g_ctx_array_prim_ran.load());
                    ctx.check("E9_ctx_array_obj_no_crash", g_ctx_array_obj_ran.load());
                    ctx.record(std::string{ "[INFO] find_class_context_loader: "
                               "context-loader resolver on \"[I\" = " }
                               + (g_ctx_array_prim_resolved.load() ? "non-null"
                                  : "null (loadClass cannot resolve array names; audit "
                                    "11951-12193)"));
                    ctx.record(std::string{ "[INFO] find_class_context_loader: "
                               "context-loader resolver on \"[Ljava/lang/String;\" = " }
                               + (g_ctx_array_obj_resolved.load() ? "non-null"
                                  : "null (same loadClass array gap)"));

                    // (E10) PARTIAL reanchor (flaw #3): a mixed list returns false yet
                    //       the resolvable name IS overridden (no rollback).  Both pins
                    //       were restored in the detour.
                    ctx.check("E10_partial_reanchor_returned_false",
                              g_partial_reanchor_returned_false.load());
                    ctx.check("E10_partial_reanchor_resolvable_still_pinned",
                              g_partial_reanchor_resolvable_still_pinned.load());
                    ctx.record("[INFO] find_class_context_loader: reanchor has NO "
                               "all-or-nothing rollback - a { resolvable, missing } list "
                               "returns false but leaves the resolvable name pinned to "
                               "the anchor loader's copy (documented footgun).");

                    // (E11) reanchor return-value truth table from a REAL Java thread.
                    ctx.check("E11_reanchor_empty_true", g_reanchor_empty_true.load());
                    ctx.check("E11_reanchor_single_ok_true", g_reanchor_single_ok_true.load());
                    ctx.check("E11_reanchor_all_fail_false", g_reanchor_all_fail_false.load());
                    ctx.check("E11_reanchor_all_succeed_true", g_reanchor_all_succeed_true.load());

                    // (E12) garbage-Klass override is healed by find_class's stale-cache
                    //       validation (the live counterpart of the no-JVM garbage-pin
                    //       closure of flaw #1).  Restored in the detour.
                    ctx.check("E12_garbage_override_evicted_by_find_class",
                              g_garbage_override_evicted_by_find_class.load());

                    // (E13) host_classloader_klass stable across more resolutions (no
                    //       forced GC; Klass* lives in Metaspace and never moves).
                    ctx.check("E13_host_klass_stable_across_resolutions",
                              g_host_klass_stable_across_resolutions.load());

                    // (E14) name-shape matrix on the context-loader resolver: every
                    //       degenerate shape returns cleanly and the '/'-keyed
                    //       find_class cache is not poisoned by the resolver.
                    ctx.check("E14_ctx_name_matrix_no_crash", g_ctx_name_matrix_no_crash.load());
                    ctx.check("E14_ctx_name_matrix_no_cache_poison",
                              g_ctx_name_matrix_no_cache_poison.load());

                    // Liveness of the secondary dispatch site is best-effort (the
                    // probe calls it, but we don't hook it) — record, never hard-fail.
                    ctx.record("[INFO] find_class_context_loader: secondaryTick is a spare "
                               "dispatch site exercised by the probe; only anchorTick is hooked.");
                }
                else
                {
                    ctx.record("[INFO] find_class_context_loader: anchor probe did not "
                               "complete within the timeout; Part E observations skipped "
                               "(hook still uninstalls at scope exit, cache untouched).");
                }
            }
            // scoped_hook `handle` uninstalls here at scope exit — nothing left armed.
        }

        // =====================================================================
        //  PART F — JAVA-VISIBLE cross-check.  The probe's captureWitness() echoed
        //  the sentinel with real getstatic bytecode; assert the JVM agrees with the
        //  native-read value (so the klass the context loader resolved is the very
        //  one the JVM serves).  Best-effort: only meaningful if the probe ran, so
        //  gate on witnessCaptured and record [INFO] when the probe didn't fire.
        // =====================================================================
        {
            if (fcl::witness_captured())
            {
                ctx.check("F_java_witness_captured", true);
                ctx.check("F_java_observed_sentinel_matches_native",
                          fcl::observed_sentinel() == SENTINEL_VALUE
                          && fcl::observed_sentinel() == fcl::sentinel());
            }
            else
            {
                ctx.record("[INFO] find_class_context_loader: captureWitness() did not run "
                           "(probe did not fire on this run); Java-visible cross-check "
                           "skipped best-effort.");
            }
        }

        ctx.record("[INFO] find_class_context_loader: proved the public context-loader "
                   "resolver (jni::find_class_with_context_loader), find_class_via_oop, "
                   "reanchor_classes_via_oop + override/evict, klass_to_class_loader_oop "
                   "(bootstrap null vs app non-null), and host-classloader capture - with "
                   "every resolved klass proven usable (find_field + mirror) and the shared "
                   "find_class cache saved/restored across the reanchor work.");
    }
}   // anonymous namespace

VMHOOK_JVM_MODULE(find_class_context_loader)
{
    // Run the whole body under a try/catch so a stray throw from any vmhook call
    // (or the harness) can never escape this module.  A throw is recorded as
    // [INFO], never a FAIL (mirrors register_class.cpp / wrapper_pattern.cpp /
    // aaa_warmup.cpp).
    bool body_threw{ false };
    try
    {
        run_find_class_context_loader_checks(ctx);
    }
    catch (...)
    {
        body_threw = true;
    }

    // FINAL CLEANUP — belt-and-braces, OUTSIDE the try so it ALWAYS runs.  Other
    // modules run after this one, so the module MUST leave ZERO hooks armed.  The
    // only hook (Part E's scoped_hook) already uninstalled at its scope exit; this
    // unconditional shutdown_hooks() guarantees an empty hook table even if the
    // body threw BEFORE reaching that scope exit (it is idempotent and
    // safe-when-empty — proven by shutdown_hooks_teardown).  A leaked armed hook is
    // exactly the failure mode that cascaded across the matrix in Wave 3.
    vmhook::shutdown_hooks();

    if (body_threw)
    {
        ctx.record("[INFO] find_class_context_loader: the test body threw and was "
                   "contained (no crash, no hooks armed); see preceding checks for "
                   "partial results.");
    }
    ctx.check("module_left_clean_final_shutdown", true);
}
