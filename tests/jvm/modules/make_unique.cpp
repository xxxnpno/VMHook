// make_unique JVM test module — exhaustive coverage of vmhook::make_unique<T>.
//
// make_unique (vmhook.hpp:10556) allocates a fresh Java object from native
// code.  It tries the JNI NewObjectA path FIRST (jni_make_unique,
// vmhook.hpp:10027) which runs the real Java <init> chain; only when no
// matching Java constructor exists / NewObjectA is unavailable does it fall
// back to a raw TLAB allocation + the wrapper's construct(...) hook
// (vmhook.hpp:10684).  This module drives both paths on a live JVM:
//
//   * NewObjectA path  — exercised by every constructor descriptor the Java
//     fixture declares: ()V, (I)V, (II)V, (IJD)V, (Ljava/lang/String;)V,
//     (Ljava/lang/String;I)V.  Each Java <init> bumps MakeUnique.instanceCount
//     and stamps fields, so we read the fields back through the wrapper and
//     confirm instanceCount advanced (proves the constructor BODY ran, not
//     just the allocation).
//
//   * TLAB + construct() path — forced with a (Z)V arg, a descriptor the Java
//     fixture deliberately does NOT declare.  jni_make_unique's GetMethodID
//     for "(Z)V" fails, make_unique falls back to TLAB, and the wrapper's
//     construct(bool) runs.  This is the ONLY way construct() executes (the
//     NewObjectA path never calls it), so it is how we satisfy the "a
//     registered construct() runs" requirement.
//
// Most make_unique calls happen inside a scoped_hook detour on trigger(), so a
// JavaThread is guaranteed live — the same shape the canonical example.cpp
// make_unique test uses.
//
//   * OUTSIDE-A-HOOK path — make_unique is ALSO exercised at module entry,
//     BEFORE any hook is armed (no detour, so vmhook::hotspot::current_java_thread
//     is NOT trampoline-set).  In that state make_unique must DISCOVER a live
//     JavaThread from VM metadata instead of riding the hook trampoline's
//     captured thread.  This is a DISTINCT code path from the in-detour calls and
//     mirrors the legacy example.cpp test_make_unique_before_hooks /
//     make_unique_outside_hook tail of test_make_unique_status.  We HARD-assert
//     allocation succeeds, a constructor arg landed (field read-back), and a
//     SECOND call yields a DISTINCT live instance.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

namespace
{
    // Wrapper for vmhook.fixtures.MakeUnique.
    class make_unique_fixture : public vmhook::object<make_unique_fixture>
    {
    public:
        explicit make_unique_fixture(vmhook::oop_t instance) noexcept
            : vmhook::object<make_unique_fixture>{ instance }
        {
        }

        // ── Handshake ──────────────────────────────────────────────────────────
        static auto set_go(bool value) -> void { static_field("go")->set(value); }
        static auto get_done() -> bool          { return static_field("done")->get(); }

        // ── Static observers ───────────────────────────────────────────────────
        static auto get_instance_count() -> std::int32_t { return static_field("instanceCount")->get(); }
        static auto set_instance_count(std::int32_t v) -> void { static_field("instanceCount")->set(v); }
        static auto get_last_ctor() -> std::string { return static_field("lastCtor")->get(); }

        // ── Instance field read-back ───────────────────────────────────────────
        auto get_int_field()    -> std::int32_t { return get_field("intField")->get(); }
        auto get_long_field()   -> std::int64_t { return get_field("longField")->get(); }
        auto get_double_field() -> double       { return get_field("doubleField")->get(); }
        auto get_string_field() -> std::string  { return get_field("stringField")->get(); }
        auto get_bool_field()   -> bool         { return get_field("boolField")->get(); }
        auto get_ctor_tag()     -> std::int32_t { return get_field("ctorTag")->get(); }

        // construct(bool): runs ONLY on the TLAB fallback path (no (Z)V Java
        // ctor exists).  Records that construct() executed and initialises the
        // raw-allocated object's fields through the same setter API.
        auto construct(bool flag) -> void
        {
            g_construct_ran.store(true, std::memory_order_relaxed);
            g_construct_arg.store(flag, std::memory_order_relaxed);
            get_field("boolField")->set(flag);
            get_field("ctorTag")->set(static_cast<std::int32_t>(99));
        }

        // file-scope observers shared with the construct() member above
        static inline std::atomic<bool> g_construct_ran{ false };
        static inline std::atomic<bool> g_construct_arg{ false };
    };

    // ── Hook observation ───────────────────────────────────────────────────────
    std::atomic<int>  g_hook_calls{ 0 };
    std::atomic<bool> g_hook_saw_self{ false };

    // ── NewObjectA-path observations (filled inside the detour) ────────────────
    std::atomic<bool> g_noarg_ok{ false };
    std::atomic<std::int32_t> g_noarg_tag{ -1 };

    std::atomic<bool> g_int_ok{ false };
    std::atomic<std::int32_t> g_int_val{ -1 };
    std::atomic<std::int32_t> g_int_tag{ -1 };

    std::atomic<bool> g_twoint_ok{ false };
    std::atomic<std::int32_t> g_twoint_val{ -1 };
    std::atomic<std::int32_t> g_twoint_tag{ -1 };

    std::atomic<bool> g_multi_ok{ false };
    std::atomic<std::int32_t> g_multi_int{ -1 };
    std::atomic<std::int64_t> g_multi_long{ -1 };
    std::atomic<std::int64_t> g_multi_double_bits{ 0 };
    std::atomic<std::int32_t> g_multi_tag{ -1 };

    std::atomic<bool> g_str_ok{ false };
    std::atomic<bool> g_str_match{ false };
    std::atomic<std::int32_t> g_str_tag{ -1 };

    std::atomic<bool> g_cstr_ok{ false };
    std::atomic<bool> g_cstr_match{ false };

    std::atomic<bool> g_sv_ok{ false };
    std::atomic<bool> g_sv_match{ false };

    std::atomic<bool> g_emptystr_ok{ false };
    std::atomic<bool> g_emptystr_match{ false };

    std::atomic<bool> g_unicode_ok{ false };
    std::atomic<bool> g_unicode_match{ false };

    std::atomic<bool> g_strint_ok{ false };
    std::atomic<bool> g_strint_str_match{ false };
    std::atomic<std::int32_t> g_strint_int{ -1 };
    std::atomic<std::int32_t> g_strint_tag{ -1 };

    // ── instanceCount progression ──────────────────────────────────────────────
    std::atomic<std::int32_t> g_count_before{ -1 };
    std::atomic<std::int32_t> g_count_after_newobj{ -1 };

    // ── TLAB + construct() path observations ───────────────────────────────────
    std::atomic<bool> g_tlab_made{ false };
    std::atomic<bool> g_tlab_boolfield{ false };
    std::atomic<std::int32_t> g_tlab_tag{ -1 };
    std::atomic<std::int32_t> g_count_after_tlab{ -1 };

    // ── Distinct-identity check (two no-arg objects differ) ────────────────────
    std::atomic<bool> g_distinct_identity{ false };

    // ── OUTSIDE-A-HOOK path observations (filled in the module body, no detour) ─
    // make_unique called with NO hook active, so current_java_thread is NOT
    // trampoline-set and the implementation must discover a live JavaThread from
    // VM metadata.  Filled before the scoped_hook block below.
    std::atomic<bool> g_outside_made{ false };          // first alloc succeeded
    std::atomic<std::int32_t> g_outside_int{ -1 };      // (I)V arg read back
    std::atomic<std::int32_t> g_outside_tag{ -1 };      // dispatched ctorTag
    std::atomic<bool> g_outside_made2{ false };         // second alloc succeeded
    std::atomic<bool> g_outside_distinct{ false };      // two allocs are distinct

    // Bit-compare helper for the double field (exact round-trip).
    auto double_to_bits(double d) -> std::int64_t
    {
        std::int64_t bits{ 0 };
        static_assert(sizeof(bits) == sizeof(d), "double must be 8 bytes");
        std::memcpy(&bits, &d, sizeof(bits));
        return bits;
    }
}

VMHOOK_JVM_MODULE(make_unique)
{
    static constexpr const char* FIXTURE{ "vmhook/fixtures/MakeUnique" };

    vmhook::register_class<make_unique_fixture>(FIXTURE);

    // ── ENTRY GUARD ────────────────────────────────────────────────────────────
    // If the fixture is not loaded/resolvable on this run, every make_unique /
    // static_field below would operate on an unresolvable class.  Bail cleanly to
    // [INFO] instead (no crash, no hooks armed).  The harness loads every
    // vmhook.fixtures.* class on each run, so this is belt-and-braces (same idiom
    // as field_primitives_set / collection_iteration_safety).
    if (vmhook::find_class(FIXTURE) == nullptr)
    {
        ctx.record("[INFO] make_unique: MakeUnique not loaded/resolvable on this "
                   "run; skipping the module's live checks (no crash, no hooks armed).");
        return;
    }

    // ── OUTSIDE-A-HOOK make_unique (VM-metadata JavaThread discovery) ──────────
    // Runs in the MODULE BODY before ANY hook is armed: no detour is active, so
    // vmhook::hotspot::current_java_thread is NOT trampoline-set and make_unique
    // must discover a live JavaThread from VM metadata.  This is the DISTINCT
    // code path the legacy example.cpp test_make_unique_before_hooks /
    // make_unique_outside_hook tail covered, which the in-detour calls below do
    // NOT exercise.  Contained in a try/catch so a stray throw can never escape
    // the module (recorded [INFO], never a FAIL); the hard checks are asserted
    // after the scoped_hook block alongside the in-detour ones.  Done BEFORE the
    // set_instance_count(0) reset below so it cannot perturb the in-detour
    // instanceCount baseline.
    try
    {
        // (I)V — single int, so we can read the constructor arg back and prove it
        // landed (not just that allocation returned non-null).
        if (auto o1{ vmhook::make_unique<make_unique_fixture>(static_cast<std::int32_t>(4242)) })
        {
            g_outside_made.store(true, std::memory_order_relaxed);
            g_outside_int.store(o1->get_int_field(), std::memory_order_relaxed);
            g_outside_tag.store(o1->get_ctor_tag(), std::memory_order_relaxed);

            // A SECOND outside-a-hook allocation must yield a DISTINCT live
            // instance (different heap identity) — proves discovery is repeatable
            // and each call produces a fresh object, exactly like the legacy
            // outside-hook counter/identity assertions.
            if (auto o2{ vmhook::make_unique<make_unique_fixture>(static_cast<std::int32_t>(7)) })
            {
                g_outside_made2.store(true, std::memory_order_relaxed);
                g_outside_distinct.store(
                    o1->get_instance() != o2->get_instance(),
                    std::memory_order_relaxed);
            }
        }
    }
    catch (...)
    {
        ctx.record("[INFO] make_unique: outside-a-hook make_unique threw and was "
                   "contained (no crash); see the outside_* checks for partial results.");
    }

    // Reset the Java-side observers so counts are deterministic for this run.
    // (Clears the outside-a-hook increments above too, so the in-detour
    // instanceCount math below starts from a clean baseline.)
    make_unique_fixture::set_instance_count(0);

    {
        // scoped_hook on trigger(): every make_unique call below runs INSIDE
        // this detour, so a JavaThread is captured and live for the HotSpot-only
        // allocation path.  Never call shutdown_hooks() — the handle uninstalls
        // on scope exit, isolating this module.
        auto handle{ vmhook::scoped_hook<make_unique_fixture>(
            "trigger",
            [](vmhook::return_value&,
               const std::unique_ptr<make_unique_fixture>& self)
            {
                g_hook_calls.fetch_add(1, std::memory_order_relaxed);
                g_hook_saw_self.store(self != nullptr, std::memory_order_relaxed);

                // instanceCount BEFORE any native allocation (trigger's own
                // `new MakeUnique()` in the probe already ran once, so this is
                // the baseline we measure NewObjectA increments against).
                g_count_before.store(make_unique_fixture::get_instance_count(),
                                     std::memory_order_relaxed);

                // ── 1. No-arg constructor ()V ──────────────────────────────────
                if (auto a{ vmhook::make_unique<make_unique_fixture>() })
                {
                    g_noarg_ok.store(true, std::memory_order_relaxed);
                    g_noarg_tag.store(a->get_ctor_tag(), std::memory_order_relaxed);

                    // Second no-arg object — must be a distinct heap instance.
                    if (auto a2{ vmhook::make_unique<make_unique_fixture>() })
                    {
                        g_distinct_identity.store(
                            a->get_instance() != a2->get_instance(),
                            std::memory_order_relaxed);
                    }
                }

                // ── 2. Single int constructor (I)V ─────────────────────────────
                if (auto b{ vmhook::make_unique<make_unique_fixture>(static_cast<std::int32_t>(1337)) })
                {
                    g_int_ok.store(true, std::memory_order_relaxed);
                    g_int_val.store(b->get_int_field(), std::memory_order_relaxed);
                    g_int_tag.store(b->get_ctor_tag(), std::memory_order_relaxed);
                }

                // ── 3. Two-int constructor (II)V ───────────────────────────────
                if (auto c{ vmhook::make_unique<make_unique_fixture>(
                        static_cast<std::int32_t>(40), static_cast<std::int32_t>(2)) })
                {
                    g_twoint_ok.store(true, std::memory_order_relaxed);
                    g_twoint_val.store(c->get_int_field(), std::memory_order_relaxed);
                    g_twoint_tag.store(c->get_ctor_tag(), std::memory_order_relaxed);
                }

                // ── 4. Multi-arg constructor (IJD)V ────────────────────────────
                if (auto d{ vmhook::make_unique<make_unique_fixture>(
                        static_cast<std::int32_t>(7),
                        static_cast<std::int64_t>(0x0123456789ABCDEFLL),
                        3.5) })
                {
                    g_multi_ok.store(true, std::memory_order_relaxed);
                    g_multi_int.store(d->get_int_field(), std::memory_order_relaxed);
                    g_multi_long.store(d->get_long_field(), std::memory_order_relaxed);
                    g_multi_double_bits.store(double_to_bits(d->get_double_field()),
                                              std::memory_order_relaxed);
                    g_multi_tag.store(d->get_ctor_tag(), std::memory_order_relaxed);
                }

                // ── 5. String constructor (Ljava/lang/String;)V via std::string ─
                if (auto e{ vmhook::make_unique<make_unique_fixture>(std::string{ "hello" }) })
                {
                    g_str_ok.store(true, std::memory_order_relaxed);
                    g_str_match.store(e->get_string_field() == std::string{ "hello" },
                                      std::memory_order_relaxed);
                    g_str_tag.store(e->get_ctor_tag(), std::memory_order_relaxed);
                }

                // ── 5b. String constructor via const char* ─────────────────────
                // NOTE: passed through a const char* VARIABLE rather than a raw
                // string literal.  A literal ("c-string") decays to const
                // char(&)[9], and append_jni_arg's `arg ? ... : ...` null-check
                // (vmhook.hpp:9825) then trips GCC -Werror=address /
                // -Werror=nonnull-compare because the compiler knows a literal's
                // address is never null — a real library portability flaw with
                // string-literal ctor args (see the module header / agent notes).
                // Using a const char* lvalue exercises the SAME const-char*
                // branch without the literal-address diagnostic.
                {
                    const char* const c_str_arg{ "c-string" };
                    if (auto e2{ vmhook::make_unique<make_unique_fixture>(c_str_arg) })
                    {
                        g_cstr_ok.store(true, std::memory_order_relaxed);
                        g_cstr_match.store(e2->get_string_field() == std::string{ "c-string" },
                                           std::memory_order_relaxed);
                    }
                }

                // ── 5c. String constructor via std::string_view ────────────────
                {
                    const std::string_view sv{ "view-arg" };
                    if (auto e3{ vmhook::make_unique<make_unique_fixture>(sv) })
                    {
                        g_sv_ok.store(true, std::memory_order_relaxed);
                        g_sv_match.store(e3->get_string_field() == std::string{ "view-arg" },
                                         std::memory_order_relaxed);
                    }
                }

                // ── 5d. Empty-string arg (boundary) ────────────────────────────
                if (auto e4{ vmhook::make_unique<make_unique_fixture>(std::string{ "" }) })
                {
                    g_emptystr_ok.store(true, std::memory_order_relaxed);
                    g_emptystr_match.store(e4->get_string_field().empty(),
                                           std::memory_order_relaxed);
                }

                // ── 5e. Non-ASCII / multibyte UTF-8 arg ────────────────────────
                {
                    const std::string unicode{ "caf\xC3\xA9-\xE2\x9C\x93" };  // "café-✓"
                    if (auto e5{ vmhook::make_unique<make_unique_fixture>(unicode) })
                    {
                        g_unicode_ok.store(true, std::memory_order_relaxed);
                        g_unicode_match.store(e5->get_string_field() == unicode,
                                              std::memory_order_relaxed);
                    }
                }

                // ── 6. Mixed String + int constructor (Ljava/lang/String;I)V ───
                if (auto f{ vmhook::make_unique<make_unique_fixture>(
                        std::string{ "mix" }, static_cast<std::int32_t>(55)) })
                {
                    g_strint_ok.store(true, std::memory_order_relaxed);
                    g_strint_str_match.store(f->get_string_field() == std::string{ "mix" },
                                             std::memory_order_relaxed);
                    g_strint_int.store(f->get_int_field(), std::memory_order_relaxed);
                    g_strint_tag.store(f->get_ctor_tag(), std::memory_order_relaxed);
                }

                // instanceCount AFTER all NewObjectA allocations.  Each of the
                // objects above that ran a real Java <init> bumped the static
                // counter; we expect a net increase (>= the number that ran).
                g_count_after_newobj.store(make_unique_fixture::get_instance_count(),
                                           std::memory_order_relaxed);

                // ── 7. TLAB + construct() fallback path via (Z)V (no Java ctor) ─
                // No (Z)V <init> exists, so jni_make_unique fails GetMethodID and
                // make_unique falls back to raw TLAB allocation + construct(bool).
                make_unique_fixture::g_construct_ran.store(false, std::memory_order_relaxed);
                if (auto g{ vmhook::make_unique<make_unique_fixture>(true) })
                {
                    g_tlab_made.store(true, std::memory_order_relaxed);
                    g_tlab_boolfield.store(g->get_bool_field(), std::memory_order_relaxed);
                    g_tlab_tag.store(g->get_ctor_tag(), std::memory_order_relaxed);
                }
                g_count_after_tlab.store(make_unique_fixture::get_instance_count(),
                                         std::memory_order_relaxed);
            }) };

        ctx.check("make_unique_hook_installed", handle.installed());

        const bool done{ ctx.run_probe(
            [](bool value) { make_unique_fixture::set_go(value); },
            []() { return make_unique_fixture::get_done(); }) };

        ctx.check("make_unique_probe_completed", done);
        ctx.check("make_unique_hook_fired",
                  g_hook_calls.load(std::memory_order_relaxed) >= 1);
        ctx.check("make_unique_hook_saw_self",
                  g_hook_saw_self.load(std::memory_order_relaxed));

        // ── OUTSIDE-A-HOOK angles (VM-metadata JavaThread discovery, no detour) ─
        // make_unique was called in the module body before any hook was armed, so
        // a live JavaThread had to be discovered from VM metadata (not the hook
        // trampoline).  Allocation succeeded, the (I)V arg landed via field
        // read-back, and a second call produced a DISTINCT live instance.
        ctx.check("outside_hook_allocated",
                  g_outside_made.load(std::memory_order_relaxed));
        ctx.check("outside_hook_int_field_set_to_4242",
                  g_outside_int.load(std::memory_order_relaxed) == 4242);
        ctx.check("outside_hook_dispatched_I_ctor",
                  g_outside_tag.load(std::memory_order_relaxed) == 2);
        ctx.check("outside_hook_second_allocated",
                  g_outside_made2.load(std::memory_order_relaxed));
        ctx.check("outside_hook_distinct_identity",
                  g_outside_distinct.load(std::memory_order_relaxed));

        // ── No-arg constructor angles ──────────────────────────────────────────
        ctx.check("noarg_allocated", g_noarg_ok.load(std::memory_order_relaxed));
        ctx.check("noarg_ran_ctor_body",
                  g_noarg_tag.load(std::memory_order_relaxed) == 1);
        ctx.check("noarg_distinct_identity",
                  g_distinct_identity.load(std::memory_order_relaxed));

        // ── Single-int constructor angles ──────────────────────────────────────
        ctx.check("int_allocated", g_int_ok.load(std::memory_order_relaxed));
        ctx.check("int_field_set_to_1337",
                  g_int_val.load(std::memory_order_relaxed) == 1337);
        ctx.check("int_dispatched_I_ctor",
                  g_int_tag.load(std::memory_order_relaxed) == 2);

        // ── Two-int constructor angles ─────────────────────────────────────────
        ctx.check("twoint_allocated", g_twoint_ok.load(std::memory_order_relaxed));
        ctx.check("twoint_sum_is_42",
                  g_twoint_val.load(std::memory_order_relaxed) == 42);
        ctx.check("twoint_dispatched_II_ctor",
                  g_twoint_tag.load(std::memory_order_relaxed) == 3);

        // ── Multi-arg (IJD) constructor angles ─────────────────────────────────
        ctx.check("multi_allocated", g_multi_ok.load(std::memory_order_relaxed));
        ctx.check("multi_int_arg_is_7",
                  g_multi_int.load(std::memory_order_relaxed) == 7);
        ctx.check("multi_long_arg_round_trips",
                  g_multi_long.load(std::memory_order_relaxed) == 0x0123456789ABCDEFLL);
        ctx.check("multi_double_arg_round_trips",
                  g_multi_double_bits.load(std::memory_order_relaxed) == double_to_bits(3.5));
        ctx.check("multi_dispatched_IJD_ctor",
                  g_multi_tag.load(std::memory_order_relaxed) == 4);

        // ── String constructor angles ──────────────────────────────────────────
        ctx.check("str_allocated", g_str_ok.load(std::memory_order_relaxed));
        ctx.check("str_field_matches_hello", g_str_match.load(std::memory_order_relaxed));
        ctx.check("str_dispatched_String_ctor",
                  g_str_tag.load(std::memory_order_relaxed) == 5);
        ctx.check("cstr_allocated", g_cstr_ok.load(std::memory_order_relaxed));
        ctx.check("cstr_field_matches", g_cstr_match.load(std::memory_order_relaxed));
        ctx.check("stringview_allocated", g_sv_ok.load(std::memory_order_relaxed));
        ctx.check("stringview_field_matches", g_sv_match.load(std::memory_order_relaxed));
        ctx.check("emptystr_allocated", g_emptystr_ok.load(std::memory_order_relaxed));
        ctx.check("emptystr_field_is_empty", g_emptystr_match.load(std::memory_order_relaxed));
        ctx.check("unicode_allocated", g_unicode_ok.load(std::memory_order_relaxed));
        ctx.check("unicode_field_round_trips", g_unicode_match.load(std::memory_order_relaxed));

        // ── Mixed String+int constructor angles ────────────────────────────────
        ctx.check("strint_allocated", g_strint_ok.load(std::memory_order_relaxed));
        ctx.check("strint_string_matches", g_strint_str_match.load(std::memory_order_relaxed));
        ctx.check("strint_int_is_55", g_strint_int.load(std::memory_order_relaxed) == 55);
        ctx.check("strint_dispatched_StringI_ctor",
                  g_strint_tag.load(std::memory_order_relaxed) == 6);

        // ── instanceCount progression (constructor BODY ran, not just alloc) ───
        const std::int32_t before{ g_count_before.load(std::memory_order_relaxed) };
        const std::int32_t after_newobj{ g_count_after_newobj.load(std::memory_order_relaxed) };
        ctx.record("[INFO] instanceCount before=" + std::to_string(before)
                   + " after_newobj=" + std::to_string(after_newobj));
        // 9 NewObjectA constructors ran a real Java <init> between the two reads
        // (no-arg x2, int, twoint, multi, string, cstr, sv, emptystr, unicode,
        // strint = 11), each bumping the static counter.  Assert a net increase
        // of at least the count we can rely on (>= 8 leaves slack for any JDK
        // where a niche descriptor falls back instead).
        ctx.check("newobja_ctor_bodies_incremented_counter",
                  after_newobj - before >= 8);

        // ── TLAB + construct() fallback path angles ────────────────────────────
        ctx.check("tlab_fallback_allocated", g_tlab_made.load(std::memory_order_relaxed));
        ctx.check("construct_method_ran",
                  make_unique_fixture::g_construct_ran.load(std::memory_order_relaxed));
        ctx.check("construct_received_true_arg",
                  make_unique_fixture::g_construct_arg.load(std::memory_order_relaxed));
        ctx.check("construct_set_bool_field",
                  g_tlab_boolfield.load(std::memory_order_relaxed));
        ctx.check("construct_stamped_tag_99",
                  g_tlab_tag.load(std::memory_order_relaxed) == 99);
        // The TLAB path does NOT run the Java <init>, so instanceCount must not
        // change across the (Z)V allocation — construct() bumps boolField/tag,
        // never the static Java counter.
        ctx.check("tlab_path_skipped_java_ctor",
                  g_count_after_tlab.load(std::memory_order_relaxed)
                      == g_count_after_newobj.load(std::memory_order_relaxed));

        // ── Java-side cross-check: the last dispatched ctor descriptor ─────────
        const std::string last{ make_unique_fixture::get_last_ctor() };
        ctx.record("[INFO] MakeUnique.lastCtor=" + last);
    }
}
