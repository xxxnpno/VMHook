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
// Functions under test (REAL header line numbers):
//   * vmhook::jni::find_class_with_context_loader(name)         (vmhook.hpp:10493)
//        -> vmhook::detail::jni_find_class_with_context_loader   (vmhook.hpp:9534)
//        the public multi-loader JNI resolver: thread context loader -> system
//        loader -> Minecraft Launch loader.
//   * vmhook::find_class_via_oop(anchor_oop, name)              (vmhook.hpp:10647)
//        resolve `name` through the classloader of a LIVE anchor object.
//   * vmhook::reanchor_classes_via_oop(anchor_oop, {...})       (vmhook.hpp:10785)
//        + vmhook::override_class_lookup(name, k)               (vmhook.hpp:10742)
//        + vmhook::evict_class_lookup(name)                     (vmhook.hpp:10760)
//        pin a set of names to the anchor loader's copy, then forget them.
//   * vmhook::detail::klass_to_class_loader_oop(klass)          (vmhook.hpp:9713)
//        the BOOTSTRAP-vs-context-loader comparison axis (bootstrap => null loader).
//   * vmhook::detail::capture_host_classloader_klass / host_classloader_klass
//                                                  (vmhook.hpp:9742 / 6282 / 9701)
//        the first non-bootstrap klass find_class sees gets captured.
//   * USABILITY of every resolved klass: top-level vmhook::find_field(klass,name)
//        (vmhook.hpp:10997, super-chain walk) + klass->get_java_mirror()
//        (vmhook.hpp:2712) + klass->get_name() (vmhook.hpp:2592).
//
// HOW THE CHECKS RUN
//   - klass_to_class_loader_oop / capture_host / the bootstrap-vs-app comparison /
//     find_field+mirror usability are pure HotSpot/VMStruct reads, so PARTS A-D
//     call them straight from the module's worker thread (no Java thread needed).
//   - find_class_with_context_loader / find_class_via_oop / reanchor_classes_via_oop
//     resolve through the CALLING thread's context loader (or a live anchor's
//     loader), which is only the APPLICATION loader on a real Java thread.  So
//     PART E runs every one of them from INSIDE a scoped_hook detour on
//     FindClassCtxLoader.anchorTick(), where `self` is a live instance of the
//     app-loaded fixture (its loader == the app loader == the anchor).
//   - PART F cross-checks the Java-visible sentinel witness captured by the probe.
//
// COMPARE BOOTSTRAP vs CONTEXT-LOADER (the task's headline angle), three ways:
//   1. loader OOP:  klass_to_class_loader_oop(String)  == null  (bootstrap)
//                   klass_to_class_loader_oop(fixture)  != null  (app loader).
//   2. resolver:    find_class("java/lang/String") (graph) and
//                   find_class_with_context_loader("java/lang/String") (delegation)
//                   both resolve to the IDENTICAL bootstrap klass.
//   3. via_oop:     find_class_via_oop(app_anchor, "java/lang/String") resolves the
//                   SAME bootstrap String (parent delegation) — proving the anchor
//                   loader sees bootstrap classes — while the app fixture resolves
//                   only through an app-visible loader.
//
// SAFETY (Wave-1 lessons, obeyed):
//   - EVERY klass / oop dereference is gated by vmhook::hotspot::is_valid_pointer;
//     EVERY resolver result is null-checked before use, so a miss never crashes.
//   - The ONLY hook is a scoped_hook<> on anchorTick() that uninstalls on scope
//     exit (RAII) — NOTHING is left armed for later modules (full-suite ordering).
//   - No GC-fragile unrooted-oop hoarding: the anchor oop is `self->get_instance()`,
//     used immediately inside the detour; nothing is held across the probe boundary
//     except plain Klass* (Metaspace-stable across GC).
//   - override_class_lookup mutates the SHARED find_class cache, so PART E SAVES
//     and RESTORES every entry it touches (java/lang/String, java/util/ArrayList,
//     the fixture) before the scope ends — later modules see the cache exactly as
//     they would have.  evict is also exercised and then the original re-seeded.
//   - C++17 (no std::bit_cast); MSVC copy-init (=) from value_t/->get(), never
//     brace-init.  No exception escapes the module body or the detour.
//
// JDK GATING: every invariant here is universal HotSpot behaviour (loader oop null
// for bootstrap, app loader non-null, delegation to bootstrap) and is HARD on all
// JDKs.  The house JDK-8 idiom (java/lang/String has no "coder" field on 8) is
// recorded as an [INFO] generation marker only; no layer is gated away.  The one
// best-effort spot is the secondaryTick() liveness count (a second dispatch site),
// recorded as [INFO], never a hard fail.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

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
    constexpr std::int32_t SENTINEL_VALUE{ 0x0CAFEC0D };

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

        static auto set_go(bool value) -> void { static_field("go")->set(value); }
        static auto get_done() -> bool         { return static_field("done")->get(); }

        static auto sentinel_field_resolves() -> bool
        {
            return static_field("sentinel").has_value();
        }
        static auto sentinel() -> std::int32_t
        {
            const auto proxy{ static_field("sentinel") };
            if (!proxy.has_value()) { return 0; }
            const std::int32_t v = proxy->get();
            return v;
        }
        static auto sentinel_via_getter() -> std::int32_t
        {
            const auto m{ static_method("getSentinel") };
            if (!m.has_value()) { return 0; }
            const std::int32_t v = m->call();
            return v;
        }
        static auto observed_sentinel() -> std::int32_t
        {
            const auto proxy{ static_field("observedSentinel") };
            if (!proxy.has_value()) { return 0; }
            const std::int32_t v = proxy->get();
            return v;
        }
        static auto witness_captured() -> bool
        {
            const auto proxy{ static_field("witnessCaptured") };
            if (!proxy.has_value()) { return false; }
            const bool v = proxy->get();
            return v;
        }
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
    // proof for a context-loader-resolved klass.
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
    std::atomic<bool> g_override_null_seed_then_restore{ false };
    std::atomic<bool> g_cache_restored_ok{ false };             // every touched entry put back

    // (E6) dotted-vs-slash safety on the context-loader resolver.
    std::atomic<bool> g_ctx_slash_form_ok{ false };
    std::atomic<bool> g_ctx_dotted_no_poison{ false };

    // Drive the single probe that fires the anchorTick hook + runs captureWitness().
    auto drive(vmhook_test::context& ctx) -> bool
    {
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
    auto on_anchor_tick(vmhook::return_value& /*ret*/, const std::unique_ptr<fcl>& self) -> void
    {
        g_detour_calls.fetch_add(1, std::memory_order_relaxed);
        g_detour_saw_self.store(self != nullptr, std::memory_order_relaxed);

        void* const anchor{ self ? self->get_instance() : nullptr };
        const bool anchor_valid{ anchor != nullptr && vmhook::hotspot::is_valid_pointer(anchor) };
        g_detour_anchor_valid.store(anchor_valid, std::memory_order_relaxed);

        // ── (E1) context-loader resolver on the app fixture (Java thread). ──
        vmhook::hotspot::klass* const ctx_fixture{
            vmhook::jni::find_class_with_context_loader(FIXTURE_NAME) };
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
            vmhook::jni::find_class_with_context_loader(FIXTURE_NAME) };
        g_ctx_fixture_idempotent.store(
            ctx_fixture != nullptr && ctx_fixture == ctx_fixture2,
            std::memory_order_relaxed);

        // ── (E2) context-loader resolver on bootstrap String -> SAME as graph. ──
        vmhook::hotspot::klass* const ctx_string{
            vmhook::jni::find_class_with_context_loader(STRING_NAME) };
        vmhook::hotspot::klass* const graph_string{ vmhook::find_class(STRING_NAME) };
        g_ctx_string_matches_graph.store(
            ctx_string != nullptr && ctx_string == graph_string,
            std::memory_order_relaxed);

        // ── (E3) context-loader resolver on a missing class -> null, no crash. ──
        vmhook::hotspot::klass* const ctx_missing{
            vmhook::jni::find_class_with_context_loader(MISSING_NAME) };
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
        // are unaffected.  A name absent from the cache reads back as nullptr; we
        // restore that by evicting (not re-seeding null, which would be a negative
        // entry that suppresses a real walk).
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

            // A null override seeds a NEGATIVE entry: find_class returns null until
            // evicted.  Pin null, observe null, then evict + re-resolve non-null.
            vmhook::override_class_lookup(STRING_NAME, nullptr);
            const bool neg_is_null{ vmhook::find_class(STRING_NAME) == nullptr };
            vmhook::evict_class_lookup(STRING_NAME);
            const bool restored_nonnull{ vmhook::find_class(STRING_NAME) != nullptr };
            g_override_null_seed_then_restore.store(neg_is_null && restored_nonnull,
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
        g_cache_restored_ok.store(restored, std::memory_order_relaxed);

        // ── (E6) dotted-vs-slash safety on the context-loader resolver. ──
        vmhook::hotspot::klass* const ctx_slash{
            vmhook::jni::find_class_with_context_loader(STRING_NAME) };
        g_ctx_slash_form_ok.store(
            ctx_slash != nullptr && ctx_slash == graph_string,
            std::memory_order_relaxed);
        // A dotted name fed to the context-loader resolver must not crash and must
        // not poison the '/'-form result (the cache is '/'-keyed; this resolver
        // does not touch the find_class cache, but assert end-to-end safety).
        (void) vmhook::jni::find_class_with_context_loader("java.lang.String");
        g_ctx_dotted_no_poison.store(
            vmhook::find_class(STRING_NAME) == graph_string, std::memory_order_relaxed);

        // Reaching here means no exception / AV escaped any resolver call.
        g_detour_completed.store(true, std::memory_order_relaxed);
    }
}

VMHOOK_JVM_MODULE(find_class_context_loader)
{
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
    //  (Distinct from find_class_fallback: that drives the resolver only from a
    //  detour and only for the app probe class; here we pin the BOOTSTRAP contract
    //  + the usable-klass proof via find_field, off-thread.)
    // =====================================================================
    {
        vmhook::hotspot::klass* const k_string{
            vmhook::jni::find_class_with_context_loader(STRING_NAME) };
        ctx.check("ctx_bootstrap_String_nonnull", k_string != nullptr);
        ctx.check("ctx_bootstrap_String_name_matches", klass_name(k_string) == STRING_NAME);
        ctx.check("ctx_bootstrap_String_mirror_usable", klass_mirror_usable(k_string));
        ctx.check("ctx_bootstrap_String_has_value_field", klass_has_field(k_string, "value"));

        vmhook::hotspot::klass* const k_object{
            vmhook::jni::find_class_with_context_loader(OBJECT_NAME) };
        ctx.check("ctx_bootstrap_Object_nonnull", k_object != nullptr);
        ctx.check("ctx_bootstrap_Object_name_matches", klass_name(k_object) == OBJECT_NAME);
        ctx.check("ctx_bootstrap_Object_mirror_usable", klass_mirror_usable(k_object));

        vmhook::hotspot::klass* const k_integer{
            vmhook::jni::find_class_with_context_loader(INTEGER_NAME) };
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
    //  path each time and must stay safe + null).
    // =====================================================================
    {
        ctx.check("ctx_missing_returns_null",
                  vmhook::jni::find_class_with_context_loader(MISSING_NAME) == nullptr);
        ctx.check("ctx_missing2_returns_null",
                  vmhook::jni::find_class_with_context_loader(MISSING_NAME2) == nullptr);
        ctx.check("ctx_empty_name_returns_null",
                  vmhook::jni::find_class_with_context_loader("") == nullptr);

        bool all_null{ true };
        for (int i{ 0 }; i < 8; ++i)
        {
            if (vmhook::jni::find_class_with_context_loader(MISSING_NAME) != nullptr)
            {
                all_null = false;
                break;
            }
        }
        ctx.check("ctx_missing_repeated_stable_null", all_null);
    }

    // =====================================================================
    //  PART B — BOOTSTRAP vs CONTEXT-LOADER at the LOADER-OOP level (the task's
    //  headline comparison; compare #2).  klass_to_class_loader_oop returns:
    //    * null  for a bootstrap-loaded class (java/lang/String, Object, Integer),
    //    * non-null for the APP-loaded fixture.
    //  This is the structural reason find_class_with_context_loader exists: the
    //  fixture's klass hangs off a NON-bootstrap loader the bare graph walk's
    //  first-by-name resolution can step past.
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
        ctx.check("loader_oop_fixture_is_nonnull_app",
                  fixture_loader != nullptr
                  && vmhook::hotspot::is_valid_pointer(fixture_loader));

        // The contrast is the whole point: app loader differs from "no loader".
        ctx.check("loader_oop_app_differs_from_bootstrap_null",
                  fixture_loader != nullptr
                  && vmhook::detail::klass_to_class_loader_oop(k_string) == nullptr);

        // A null / invalid klass argument -> null loader oop, no crash.
        ctx.check("loader_oop_null_klass_is_null",
                  vmhook::detail::klass_to_class_loader_oop(nullptr) == nullptr);
    }

    // =====================================================================
    //  PART C — HOST-CLASSLOADER CAPTURE.  find_class latches the first
    //  non-bootstrap klass it sees into detail::host_classloader_klass (so newly
    //  attached worker threads can inherit the app loader).  Resolving the app
    //  fixture guarantees a capture candidate existed; assert a host klass is now
    //  recorded AND that its loader oop is non-null (i.e. genuinely non-bootstrap).
    //  (This is the capture half that find_class_fallback never touches.)
    // =====================================================================
    {
        // Ensure the app fixture has been resolved at least once (capture trigger).
        (void) vmhook::find_class(FIXTURE_NAME);

        vmhook::hotspot::klass* const host{
            vmhook::detail::host_classloader_klass.load(std::memory_order_acquire) };
        ctx.check("host_classloader_klass_captured", host != nullptr);
        if (host != nullptr)
        {
            ctx.check("host_classloader_klass_valid", vmhook::hotspot::is_valid_pointer(host));
            // The captured klass MUST be non-bootstrap (capture skips null-loader
            // candidates), so its loader oop is non-null.
            void* const host_loader{ vmhook::detail::klass_to_class_loader_oop(host) };
            ctx.check("host_classloader_klass_loader_nonnull",
                      host_loader != nullptr
                      && vmhook::hotspot::is_valid_pointer(host_loader));
        }
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

    // =====================================================================
    //  PART D — USABILITY of the context-loader-resolved app klass, off-thread.
    //  Beyond "non-null": its static sentinel + instance mark field entries both
    //  resolve through the top-level super-walking find_field, its mirror is
    //  valid, and the value read through the registered wrapper equals the baked
    //  sentinel.  (This is the "returned klass is usable" proof the feature spec
    //  calls for, on the context-loader-resolved klass specifically.)
    //
    //  NOTE: java/lang/String is bootstrap-visible to the context loader, so the
    //  fixture (app) class is the meaningful target here.  On the worker thread
    //  the context loader is NOT the app loader, so find_class_with_context_loader
    //  may legitimately MISS the app fixture; we therefore resolve the app klass
    //  through vmhook::find_class (graph walk, thread-agnostic) for the off-thread
    //  usability proof, and exercise the *context-loader* resolution of the app
    //  class on the JAVA thread in PART E (where it must succeed).
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
    //  PART E — CONTEXT-LOADER + REANCHOR family driven from a JAVA THREAD.
    //  find_class_with_context_loader (app class), find_class_via_oop (live
    //  anchor), reanchor_classes_via_oop + override/evict (shared cache, fully
    //  save/restored).  The detour rides FindClassCtxLoader.anchorTick(); `self`
    //  is a live instance of the app fixture, so its loader == the app loader ==
    //  the anchor.  scoped_hook uninstalls on scope exit — nothing left armed.
    // =====================================================================
    {
        auto handle{ vmhook::scoped_hook<fcl>("anchorTick", &on_anchor_tick) };
        ctx.check("anchor_hook_installed", handle.installed());
        if (handle.installed())
        {
            const bool done{ drive(ctx) };
            ctx.check("anchor_probe_completed", done);
            ctx.check("anchor_detour_fired", g_detour_calls.load() >= 1);
            ctx.check("anchor_detour_saw_self", g_detour_saw_self.load());
            ctx.check("anchor_detour_anchor_oop_valid", g_detour_anchor_valid.load());
            ctx.check("anchor_detour_completed_no_throw", g_detour_completed.load());

            // (E1) context-loader resolver on the APP class succeeds on the Java
            //      thread and is usable (the headline: bootstrap walk can miss an
            //      app class; the context loader resolves it).
            ctx.check("E1_ctx_app_fixture_resolved", g_ctx_fixture_nonnull.load());
            ctx.check("E1_ctx_app_fixture_name_matches", g_ctx_fixture_name_ok.load());
            ctx.check("E1_ctx_app_fixture_mirror_usable", g_ctx_fixture_mirror_ok.load());
            ctx.check("E1_ctx_app_fixture_field_resolves", g_ctx_fixture_field_ok.load());
            ctx.check("E1_ctx_app_fixture_idempotent", g_ctx_fixture_idempotent.load());

            // (E2) bootstrap via the context loader == graph walk (compare #2).
            ctx.check("E2_ctx_bootstrap_matches_graph", g_ctx_string_matches_graph.load());

            // (E3) missing via the context loader -> null, no crash.
            ctx.check("E3_ctx_missing_null", g_ctx_missing_null.load());

            // (E4) find_class_via_oop against the live anchor (compare #3 + guards).
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
            ctx.check("E5_override_null_seed_then_restore",
                      g_override_null_seed_then_restore.load());
            ctx.check("E5_shared_cache_restored", g_cache_restored_ok.load());

            // (E6) dotted-vs-slash safety on the context-loader resolver.
            ctx.check("E6_ctx_slash_form_matches_graph", g_ctx_slash_form_ok.load());
            ctx.check("E6_ctx_dotted_no_cache_poison", g_ctx_dotted_no_poison.load());

            // Liveness of the secondary dispatch site is best-effort (the probe
            // calls it, but we don't hook it) — record, never hard-fail.
            ctx.record("[INFO] find_class_context_loader: secondaryTick is a spare "
                       "dispatch site exercised by the probe; only anchorTick is hooked.");
        }
        // scoped_hook `handle` uninstalls here at scope exit — nothing left armed.
    }

    // =====================================================================
    //  PART F — JAVA-VISIBLE cross-check.  The probe's captureWitness() echoed the
    //  sentinel with real getstatic bytecode; assert the JVM agrees with the
    //  native-read value (so the klass the context loader resolved is the very one
    //  the JVM serves).  A hard floor guarantees the Java path actually ran.
    // =====================================================================
    {
        ctx.check("F_java_witness_captured", fcl::witness_captured());
        ctx.check("F_java_observed_sentinel_matches_native",
                  fcl::observed_sentinel() == SENTINEL_VALUE
                  && fcl::observed_sentinel() == fcl::sentinel());
    }

    ctx.record("[INFO] find_class_context_loader: proved the public context-loader "
               "resolver (jni::find_class_with_context_loader), find_class_via_oop, "
               "reanchor_classes_via_oop + override/evict, klass_to_class_loader_oop "
               "(bootstrap null vs app non-null), and host-classloader capture - with "
               "every resolved klass proven usable (find_field + mirror) and the shared "
               "find_class cache saved/restored across the reanchor work.");
}
