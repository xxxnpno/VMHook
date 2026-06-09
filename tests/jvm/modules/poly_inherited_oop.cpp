// poly_inherited_oop JVM test module  (feature area: fields + methods)
//
// The live-JVM counterpart of the legacy example.cpp test_poly_probe
// (vmhook/src/example.cpp:2192).  A B-extends-A object is exercised through
// vmhook to prove, on genuine HotSpot metadata, that:
//
//     PolyInherited$A                 (super — protected int protectedInt = 1337,
//        ^  extends                    protected int protectedAdd(int) = protectedInt + x)
//     PolyInherited$B                 (sub — own int bInt = 42)
//
//   * vmhook::find_field's Klass::get_super() super-chain walk
//     (vmhook.hpp:10756, the `for (k = target_klass; k; k = k->get_super())`
//     loop) resolves an INHERITED INSTANCE field: reading protectedInt THROUGH
//     the B klass must walk one super link UP to A and resolve A's declared
//     field at the correct offset — this is the inherited-INSTANCE-field angle
//     complementing field_inherited's inherited-STATIC focus;
//   * B's OWN field bInt resolves at walk depth 0 through the same B wrapper;
//   * the SAME inherited field read through the B wrapper (start klass B, depth
//     1) and through an A wrapper around the SAME oop (start klass A, depth 0)
//     resolves to the IDENTICAL physical slot — proving the B-klass read lands
//     on A's declared field at the same offset, not a divergent copy;
//   * the inherited protectedAdd(int) is FOUND through the super walk on the B
//     wrapper (get_method walks the same chain), and — best-effort, only when
//     the JDK exports StubRoutines::_call_stub_entry (the interpreter/JNI call
//     gate) — calling protectedAdd(3) returns protectedInt + 3 == 1340.
//
// No hooks are needed and NONE are armed: find_field is driven directly through
// the wrappers, and method_proxy::call() attaches the current thread via
// ensure_current_java_thread() (vmhook.hpp:13133) so it dispatches from this
// native test thread exactly as the legacy test_poly_probe does — gated on the
// call_stub_entry being present.  The go/done probe is driven only to run the
// Java-side witness (Java reads the same three quantities through real bytecode),
// so the module can cross-check that the JVM itself agrees with vmhook's reads.
//
// SAFETY: every oop / klass dereference is gated through is_valid_pointer
// (vmhook.hpp:1768); value_t / call() results are extracted by COPY-init (=),
// never brace-init, to stay MSVC-unambiguous against value_t's templated
// conversion operator.  Read-only ops aren't thread-safe (vmhook.hpp:22-24) but
// this module is the single test thread, matching the documented contract.
#include <vmhook/vmhook.hpp>

#include "../harness.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace
{
    // ---- Wrapper registered to the SUB class PolyInherited$B.  Its
    //      get_field / get_method start the super walk at B's klass, so an
    //      inherited name resolves one link UP at A. ------------------------
    class pi_b : public vmhook::object<pi_b>
    {
    public:
        explicit pi_b(vmhook::oop_t instance) noexcept
            : vmhook::object<pi_b>{ instance }
        {
        }

        // handshake (static fields on the fixture, not on B) — resolved via the
        // fixture wrapper below, so nothing here.

        // B's OWN field (super walk depth 0).
        auto b_int() const -> std::int32_t { return get_field("bInt")->get(); }

        // INHERITED protected field declared on A (super walk depth 1 from B).
        auto protected_int() const -> std::int32_t { return get_field("protectedInt")->get(); }

        // INHERITED protected method declared on A — found via the super walk on
        // B's klass.  call() result COPY-init'd.  Caller gates on the call gate.
        auto protected_add(std::int32_t x) const -> std::int32_t
        {
            const auto mp{ get_method("protectedAdd") };
            if (!mp.has_value())
            {
                return -1;
            }
            const std::int32_t v = mp->call(x);
            return v;
        }
    };

    // ---- Wrapper registered to the SUPER class PolyInherited$A.  Its super
    //      walk starts at A, so it resolves A's OWN protectedInt at depth 0.
    //      Used to prove the B-klass inherited read lands on the SAME slot. ---
    class pi_a : public vmhook::object<pi_a>
    {
    public:
        explicit pi_a(vmhook::oop_t instance) noexcept
            : vmhook::object<pi_a>{ instance }
        {
        }
        auto protected_int() const -> std::int32_t { return get_field("protectedInt")->get(); }
    };

    // ---- Wrapper registered to the FIXTURE class, owning the go/done
    //      handshake, the Java-side witnesses, and the held B instance. -------
    class pi_fixture : public vmhook::object<pi_fixture>
    {
    public:
        explicit pi_fixture(vmhook::oop_t instance) noexcept
            : vmhook::object<pi_fixture>{ instance }
        {
        }

        // -- handshake (portable static accessors) --
        static auto set_go(bool v) -> void   { static_field("go")->set(v); }
        static auto set_done(bool v) -> void { static_field("done")->set(v); }
        static auto get_done() -> bool       { return static_field("done")->get(); }

        // -- Java-side witnesses (set by the probe through real bytecode) --
        static auto saw_own_field() -> bool        { return static_field("sawOwnField")->get(); }
        static auto saw_inherited_field() -> bool  { return static_field("sawInheritedField")->get(); }
        static auto saw_inherited_method() -> bool { return static_field("sawInheritedMethod")->get(); }

        // The held live B instance.  Resolve the static field on the FIXTURE
        // klass (where bInstance is declared), guard the optional, and hand back
        // the raw decoded OOP so the caller can validate it before wrapping as a
        // B.  Returns nullptr on a missing field / null reference.
        static auto get_b_oop() -> vmhook::oop_t
        {
            const auto fp{ static_field("bInstance") };
            if (!fp.has_value())
            {
                return nullptr;
            }
            // unique_ptr<pi_b> conversion validates+decodes the compressed OOP;
            // extract its raw instance pointer (or nullptr) by COPY.
            std::unique_ptr<pi_b> held = fp->get();
            return held ? held->vmhook::object_base::get_instance() : nullptr;
        }
    };

    // ---- Constants mirrored from PolyInherited.java / legacy A.java + B.java -
    constexpr std::int32_t PROTECTED_INT { 1337 };   // A.protectedInt init
    constexpr std::int32_t B_INT         { 42 };     // B.bInt init
    constexpr std::int32_t ADD_ARG       { 3 };      // protectedAdd argument
    constexpr std::int32_t ADD_RESULT    { 1340 };   // protectedAdd(3) == 1337 + 3

    // ---- Internal (JVM, slash-separated) class names.  javac emits the nested
    //      static classes as PolyInherited$A / PolyInherited$B (confirmed via
    //      javap), so these are the names register_class<>() and find_class()
    //      key on.  Centralised so the registration calls and the no-instance
    //      resolution checks below can never drift apart.
    constexpr const char* FIXTURE_NAME { "vmhook/fixtures/PolyInherited" };
    constexpr const char* A_NAME       { "vmhook/fixtures/PolyInherited$A" };
    constexpr const char* B_NAME       { "vmhook/fixtures/PolyInherited$B" };
}

VMHOOK_JVM_MODULE(poly_inherited_oop)
{
    vmhook::register_class<pi_fixture>(FIXTURE_NAME);
    vmhook::register_class<pi_a>(A_NAME);
    vmhook::register_class<pi_b>(B_NAME);

    // Record which dispatch path the live JDK uses for call(), for diagnostics.
    const bool call_gate_present{ vmhook::detail::find_call_stub_entry() != nullptr };
    ctx.record(std::string{ "[INFO] poly_inherited_oop call gate: " }
               + (call_gate_present
                      ? "StubRoutines::_call_stub_entry present (call_stub fast path)"
                      : "call_stub_entry absent (JNI fallback via ensure_current_java_thread)"));

    // =====================================================================
    //  Registration / resolution sanity for both hierarchy levels.
    // =====================================================================
    {
        // The fixture's go/done handshake fields ARE static, so static_field()
        // (the portable type_index-keyed accessor) is the right probe here: it
        // proves the fixture wrapper is registered and its klass is found.
        ctx.check("fixture_class_registered_static_resolves",
                  pi_fixture::static_field("go").has_value());

        // bInt (own on B) and protectedInt (own on A) are INSTANCE fields, so
        // static_field() is the WRONG probe for them: object_base::get_field(
        // type_index, name) deliberately returns nullopt for a non-static entry
        // ("needs an object instance", vmhook.hpp:14049) — it never resolves an
        // instance field regardless of registration.  To prove "the wrapper is
        // registered AND its klass is found AND its OWN field resolves" without
        // a live instance, resolve the registered klass via find_class() and
        // confirm find_field() (the same super-chain walker the instance path
        // uses, which DOES find instance fields) locates the own field.  Each
        // klass is null-checked before use and find_field() is is_valid_pointer-
        // gated internally (vmhook.hpp:10932).
        vmhook::hotspot::klass* const b_klass{ vmhook::find_class(B_NAME) };
        ctx.check("sub_class_b_klass_resolved", b_klass != nullptr);
        ctx.check("sub_class_b_registered_own_field_resolves",
                  b_klass != nullptr
                      && vmhook::find_field(b_klass, "bInt").has_value());

        vmhook::hotspot::klass* const a_klass{ vmhook::find_class(A_NAME) };
        ctx.check("super_class_a_klass_resolved", a_klass != nullptr);
        ctx.check("super_class_a_registered_own_field_resolves",
                  a_klass != nullptr
                      && vmhook::find_field(a_klass, "protectedInt").has_value());
    }

    // =====================================================================
    //  Obtain the live B instance once; validate the decoded OOP before any
    //  dereference, then wrap it as a B (the canonical view).
    // =====================================================================
    const vmhook::oop_t b_oop{ pi_fixture::get_b_oop() };
    ctx.check("b_instance_oop_obtained", b_oop != nullptr);
    ctx.check("b_instance_oop_valid",
              b_oop != nullptr && vmhook::hotspot::is_valid_pointer(b_oop));

    if (b_oop != nullptr && vmhook::hotspot::is_valid_pointer(b_oop))
    {
        // B-typed wrapper: super walk starts at B's klass.
        pi_b b_view{ b_oop };

        // ---- OWN field bInt — super walk depth 0 (declared on B) -----------
        {
            auto fp{ b_view.get_field("bInt") };
            ctx.check("own_field_bInt_resolves", fp.has_value());
            if (fp.has_value())
            {
                const std::int32_t v = fp->get();              // COPY-init
                ctx.check("own_field_bInt_value_42", v == B_INT);
                ctx.check("own_field_bInt_not_static", fp->is_static() == false);
                ctx.check("own_field_bInt_signature_I",
                          std::string{ fp->signature() } == "I");
            }
            ctx.check("own_field_bInt_accessor", b_view.b_int() == B_INT);
        }

        // ---- INHERITED protected field protectedInt — super walk depth 1 ----
        //      Reading THROUGH the B klass must walk one super link up to A and
        //      resolve A's declared field.  This is the inherited-INSTANCE-field
        //      angle (find_field reads by offset, ignoring Java access control).
        {
            auto fp{ b_view.get_field("protectedInt") };
            ctx.check("inherited_protectedInt_resolves_via_super_walk", fp.has_value());
            if (fp.has_value())
            {
                const std::int32_t v = fp->get();              // COPY-init
                ctx.check("inherited_protectedInt_value_1337", v == PROTECTED_INT);
                ctx.check("inherited_protectedInt_not_static", fp->is_static() == false);
                ctx.check("inherited_protectedInt_signature_I",
                          std::string{ fp->signature() } == "I");
            }
            ctx.check("inherited_protectedInt_accessor",
                      b_view.protected_int() == PROTECTED_INT);
        }

        // =================================================================
        //  Offset/declared-field resolution proof: the SAME inherited field
        //  read through the B wrapper (depth 1) and through an A wrapper
        //  around the SAME oop (depth 0 — A's OWN field) must resolve to the
        //  IDENTICAL physical address.  This confirms the B-klass read lands
        //  on A's DECLARED field at the correct offset, not a divergent slot.
        // =================================================================
        {
            pi_a a_view{ b_oop };                               // same oop, A start klass
            auto via_b{ b_view.get_field("protectedInt") };
            auto via_a{ a_view.get_field("protectedInt") };
            ctx.check("inherited_field_via_A_wrapper_resolves", via_a.has_value());
            if (via_b.has_value() && via_a.has_value())
            {
                ctx.check("inherited_field_B_and_A_same_address",
                          via_b->raw_address() == via_a->raw_address());
                const std::int32_t vb = via_b->get();          // COPY-init
                const std::int32_t va = via_a->get();          // COPY-init
                ctx.check("inherited_field_B_and_A_same_value", vb == va);
                ctx.check("inherited_field_resolves_to_As_declared_value",
                          va == PROTECTED_INT);
            }
            ctx.check("inherited_field_A_view_accessor",
                      a_view.protected_int() == PROTECTED_INT);
        }

        // =================================================================
        //  The OWN sub-field bInt is NOT visible from the SUPER (A) wrapper:
        //  the A super walk only goes UP toward Object, never down into B ->
        //  nullopt.  Distinguishes own-vs-inherited resolution direction.
        // =================================================================
        {
            pi_a a_view{ b_oop };
            ctx.check("own_subfield_not_visible_from_super_wrapper",
                      a_view.get_field("bInt").has_value() == false);
        }

        // =================================================================
        //  INHERITED method protectedAdd(int) — FOUND through the super walk
        //  on B's klass (get_method walks the same get_super() chain).
        // =================================================================
        {
            auto mp{ b_view.get_method("protectedAdd") };
            ctx.check("inherited_method_protectedAdd_found_via_super_walk", mp.has_value());
            if (mp.has_value())
            {
                ctx.check("inherited_method_protectedAdd_not_static",
                          mp->is_static() == false);
                ctx.check("inherited_method_protectedAdd_signature_II",
                          std::string{ mp->signature() } == "(I)I");
            }

            // CALL is best-effort: method_proxy::call() needs the call gate
            // (StubRoutines::_call_stub_entry) OR an attachable current thread.
            // Mirror legacy test_poly_probe: only assert the concrete return
            // value when the call gate is exported; otherwise record [INFO] and
            // treat findability as the verified limit (no value assertion).
            if (call_gate_present)
            {
                const std::int32_t result{ b_view.protected_add(ADD_ARG) };
                ctx.check("inherited_method_protectedAdd_call_returns_1340",
                          result == ADD_RESULT);
            }
            else
            {
                ctx.record("[INFO] poly_inherited_oop: inherited protectedAdd(3) "
                           "call skipped (call_stub_entry absent on this JDK); "
                           "method findability via super walk is the verified limit");
            }
        }

        // =================================================================
        //  Cache stability through the walk: a second resolution of the
        //  inherited field returns a proxy at the SAME address + value.
        // =================================================================
        {
            auto a{ b_view.get_field("protectedInt") };
            auto b{ b_view.get_field("protectedInt") };
            if (a.has_value() && b.has_value())
            {
                ctx.check("inherited_field_cache_same_address",
                          a->raw_address() == b->raw_address());
                const std::int32_t va = a->get();              // COPY-init
                const std::int32_t vb = b->get();              // COPY-init
                ctx.check("inherited_field_cache_same_value", va == vb);
            }
        }

        // =================================================================
        //  NEGATIVE path — a name absent from the whole A/B chain walks to
        //  java.lang.Object and returns nullopt.
        // =================================================================
        {
            ctx.check("absent_field_walks_to_object_nullopt",
                      b_view.get_field("noSuchFieldAnywhere").has_value() == false);
            ctx.check("absent_method_walks_to_object_nullopt",
                      b_view.get_method("noSuchMethodAnywhere").has_value() == false);
        }
    }

    // =====================================================================
    //  Java-side WITNESS cross-check.  Drive the fixture's probe so the Java
    //  thread reads the SAME three quantities through real getfield /
    //  invokevirtual bytecode; the module then confirms the JVM itself agrees
    //  with vmhook's offset reads.  No hook is armed — the probe action is
    //  pure Java bytecode.
    // =====================================================================
    {
        pi_fixture::set_done(false);
        const bool probe_done{ ctx.run_probe(
            [](bool v)
            {
                if (v)
                {
                    pi_fixture::set_done(false);
                }
                pi_fixture::set_go(v);
            },
            []() { return pi_fixture::get_done(); }) };

        ctx.check("java_witness_probe_completed", probe_done);
        if (probe_done)
        {
            ctx.check("java_saw_own_field_bInt_42", pi_fixture::saw_own_field());
            ctx.check("java_saw_inherited_protectedInt_1337",
                      pi_fixture::saw_inherited_field());
            // Java's own invokevirtual of protectedAdd(3) always runs on the Java
            // thread (no native call gate involved), so this witness is asserted
            // unconditionally — it proves the inherited method dispatches to A's
            // body and yields 1340 regardless of the native call_stub path.
            ctx.check("java_saw_inherited_protectedAdd_1340",
                      pi_fixture::saw_inherited_method());
        }
    }

    // No hooks were armed by this module; nothing to unhook.
}
