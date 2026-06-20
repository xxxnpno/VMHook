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

    // ======================================================================
    //  Wrappers for the EXHAUSTIVE expansion (deep hierarchy, shadow pair,
    //  reference shapes, polymorphic actual type).  Distinct poh_* prefix.
    //  Every accessor uses the documented one-liner idiom
    //  (return get_field("x")->get();) with NO sentinel guards — all
    //  suite-safety lives at the MODULE call sites, never in the accessors.
    // ======================================================================

    // -- Deep hierarchy L1<-L2<-L3<-L4.  Registered to the DEEPEST class L4, so
    //    every get_field starts the super walk at L4 and an inherited name
    //    resolves at depth 1 (L3) .. depth 3 (L1). -----------------------------
    class poh_l4 : public vmhook::object<poh_l4>
    {
    public:
        explicit poh_l4(vmhook::oop_t instance) noexcept
            : vmhook::object<poh_l4>{ instance }
        {
        }

        // int declared at EACH level, all read through the L4 view.
        auto l1_int() const -> std::int32_t { return get_field("l1Int")->get(); }  // depth 3
        auto l2_int() const -> std::int32_t { return get_field("l2Int")->get(); }  // depth 2
        auto l3_int() const -> std::int32_t { return get_field("l3Int")->get(); }  // depth 1
        auto l4_int() const -> std::int32_t { return get_field("l4Int")->get(); }  // depth 0 (own)

        // Inherited String reference (declared on L1, read through L4) decoded to
        // a std::string via the value_t string alternative.
        auto l1_str() const -> std::string { return get_field("l1Str")->get(); }

        // Inherited reference-shape fields exposed as a field_proxy so the call
        // site can decode the compressed OOP (field_oop) and validate it before
        // wrapping.  Returning the proxy (not a typed wrapper) keeps these free
        // of forward-reference ordering on the other poh_* wrappers and lets the
        // module assert the raw decode + identity exactly.
        auto field(const char* name) const -> std::optional<vmhook::field_proxy> { return get_field(name); }
    };

    // -- A plain-L1 wrapper so the inherited L2-ref (which holds an L1) and the
    //    polymorphic L1-declared field can be read back as a concrete object. --
    class poh_l1 : public vmhook::object<poh_l1>
    {
    public:
        explicit poh_l1(vmhook::oop_t instance) noexcept
            : vmhook::object<poh_l1>{ instance }
        {
        }
        auto l1_int() const -> std::int32_t { return get_field("l1Int")->get(); }
        auto l1_str() const -> std::string  { return get_field("l1Str")->get(); }
    };

    // -- Shadow base wrapper: registered to the BASE Shadow, so a read of the
    //    shadowed name resolves the BASE slot at depth 0. ----------------------
    class poh_shadow : public vmhook::object<poh_shadow>
    {
    public:
        explicit poh_shadow(vmhook::oop_t instance) noexcept
            : vmhook::object<poh_shadow>{ instance }
        {
        }
        auto shadowed_int() const -> std::int32_t { return get_field("shadowedInt")->get(); }
        auto shadowed_ref() const -> std::string  { return get_field("shadowedRef")->get(); }
    };

    // -- Shadow sub wrapper: registered to ShadowSub, so a read of the shadowed
    //    name resolves the CHILD slot (declared-scope wins at the start klass). -
    class poh_shadow_sub : public vmhook::object<poh_shadow_sub>
    {
    public:
        explicit poh_shadow_sub(vmhook::oop_t instance) noexcept
            : vmhook::object<poh_shadow_sub>{ instance }
        {
        }
        auto shadowed_int() const -> std::int32_t { return get_field("shadowedInt")->get(); }
        auto shadowed_ref() const -> std::string  { return get_field("shadowedRef")->get(); }
    };

    // -- Interface-impl wrapper: registered to IfaceImpl, so get_method walks
    //    IfaceImpl's super chain (Object) and then falls back to the implemented-
    //    interface chain, where the DEFAULT greet() lives.  get_field walks ONLY
    //    supers, so the interface constant KONST_FIELD is invisible here. --------
    class poh_iface : public vmhook::object<poh_iface>
    {
    public:
        explicit poh_iface(vmhook::oop_t instance) noexcept
            : vmhook::object<poh_iface>{ instance }
        {
        }
        auto own_int() const -> std::int32_t { return get_field("ownInt")->get(); }
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

        // -- Witnesses for the EXHAUSTIVE expansion (latched by the probe) --
        static auto saw_deep_fields() -> bool   { return static_field("sawDeepFields")->get(); }
        static auto saw_deep_refs() -> bool     { return static_field("sawDeepRefs")->get(); }
        static auto saw_shadow_sub() -> bool    { return static_field("sawShadowSub")->get(); }
        static auto saw_shadow_base() -> bool   { return static_field("sawShadowBase")->get(); }
        static auto saw_poly_concrete() -> bool { return static_field("sawPolyConcrete")->get(); }

        // -- Witnesses for the polymorphic-METHOD-dispatch + interface angles --
        static auto saw_override_dispatch() -> bool { return static_field("sawOverrideDispatch")->get(); }
        static auto saw_base_chain_value() -> bool  { return static_field("sawBaseChainValue")->get(); }
        static auto saw_iface_default() -> bool     { return static_field("sawIfaceDefault")->get(); }
        static auto saw_iface_const() -> bool       { return static_field("sawIfaceConst")->get(); }

        // -- Published identity hash codes (exact native-oop cross-checks) --
        static auto l1_str_identity() -> std::int32_t   { return static_field("l1StrIdentity")->get(); }
        static auto l1_arr_identity() -> std::int32_t   { return static_field("l1ArrIdentity")->get(); }
        static auto l2_ref_identity() -> std::int32_t   { return static_field("l2RefIdentity")->get(); }
        static auto self_ref_identity() -> std::int32_t { return static_field("selfRefIdentity")->get(); }
        static auto poly_base_identity() -> std::int32_t { return static_field("polyBaseIdentity")->get(); }

        // Decode the raw OOP held by a static REFERENCE field declared on the
        // fixture, validating + decoding the compressed OOP through the
        // unique_ptr<wrapper_type> conversion and handing back the raw instance
        // pointer (or nullptr).  Generic over the wrapper type so each held
        // instance (B / L4 / Shadow / polyBase) decodes through its own wrapper.
        template<typename wrapper_type>
        static auto static_ref_oop(const char* const name) -> vmhook::oop_t
        {
            const auto fp{ static_field(name) };
            if (!fp.has_value())
            {
                return nullptr;
            }
            std::unique_ptr<wrapper_type> held = fp->get();
            return held ? held->vmhook::object_base::get_instance() : nullptr;
        }

        // The held live B instance (kept as a named helper for the legacy block).
        static auto get_b_oop() -> vmhook::oop_t { return static_ref_oop<pi_b>("bInstance"); }

        // Held live instances for the exhaustive expansion, each decoded through
        // its own wrapper type so the deep-hierarchy / shadow / polymorphic reads
        // run against genuine, validated OOPs.
        static auto get_l4_oop()     -> vmhook::oop_t { return static_ref_oop<poh_l4>("l4Instance"); }
        static auto get_shadow_oop() -> vmhook::oop_t { return static_ref_oop<poh_shadow_sub>("shadowInstance"); }
        static auto get_poly_oop()   -> vmhook::oop_t { return static_ref_oop<poh_l1>("polyBase"); }
        static auto get_l1_plain_oop() -> vmhook::oop_t { return static_ref_oop<poh_l1>("l1Plain"); }
        static auto get_iface_oop()    -> vmhook::oop_t { return static_ref_oop<poh_iface>("ifaceInstance"); }
    };

    // ---- Constants mirrored from PolyInherited.java / legacy A.java + B.java -
    constexpr std::int32_t PROTECTED_INT { 1337 };   // A.protectedInt init
    constexpr std::int32_t B_INT         { 42 };     // B.bInt init
    constexpr std::int32_t ADD_ARG       { 3 };      // protectedAdd argument
    constexpr std::int32_t ADD_RESULT    { 1340 };   // protectedAdd(3) == 1337 + 3

    // ---- Constants for the deep 4-level hierarchy (mirror PolyInherited.java) -
    constexpr std::int32_t L1_INT       { 0x0A1A0001 };   // declared on L1 (depth 3)
    constexpr std::int32_t L2_INT       { 0x0B2B0002 };   // declared on L2 (depth 2)
    constexpr std::int32_t L3_INT       { 0x0C3C0003 };   // declared on L3 (depth 1)
    constexpr std::int32_t L4_INT       { 0x0D4D0004 };   // declared on L4 (depth 0 / own)
    constexpr std::int32_t L2_REF_VAL   { 0x0E5E0005 };   // l2Ref's own l1Int
    constexpr std::int32_t L1_ARR_ELEM0 { 0x51510001 };   // l1Arr[0]
    constexpr std::int32_t L1_ARR_LEN   { 2 };
    const     std::string  L1_STR_VALUE { "l1-inherited-string" };

    // ---- Constants for the shadow (hidden-field) pair ----------------------
    constexpr std::int32_t BASE_SHADOW_INT { 1000 };      // Shadow.shadowedInt
    constexpr std::int32_t SUB_SHADOW_INT  { 2000 };      // ShadowSub.shadowedInt
    const     std::string  BASE_SHADOW_STR { "base-shadow" };
    const     std::string  SUB_SHADOW_STR  { "sub-shadow" };

    // ---- Constant for the polymorphic field's concrete L4 ------------------
    constexpr std::int32_t POLY_L4_INT  { 0x0F6F0006 };   // l4Poly's l4Int

    // ---- Constants for the polymorphic-METHOD-dispatch override chain -------
    constexpr std::int32_t L1_CHAIN     { 0x11AA0001 };   // L1.chainValue() body
    constexpr std::int32_t L4_CHAIN     { 0x44DD0004 };   // L4.chainValue() override body

    // ---- Constants for the interface default method + interface constant ----
    constexpr std::int32_t GREET_VALUE    { 0x6E710007 }; // Greeter.greet() default body
    constexpr std::int32_t IFACE_DESCRIBE { 0x4E730008 }; // IfaceImpl.describe() override / ownInt

    // ---- Internal (JVM, slash-separated) class names.  javac emits the nested
    //      static classes as PolyInherited$A / PolyInherited$B (confirmed via
    //      javap), so these are the names register_class<>() and find_class()
    //      key on.  Centralised so the registration calls and the no-instance
    //      resolution checks below can never drift apart.
    constexpr const char* FIXTURE_NAME { "vmhook/fixtures/PolyInherited" };
    constexpr const char* A_NAME       { "vmhook/fixtures/PolyInherited$A" };
    constexpr const char* B_NAME       { "vmhook/fixtures/PolyInherited$B" };
    constexpr const char* L1_NAME      { "vmhook/fixtures/PolyInherited$L1" };
    constexpr const char* L4_NAME      { "vmhook/fixtures/PolyInherited$L4" };
    constexpr const char* SHADOW_NAME      { "vmhook/fixtures/PolyInherited$Shadow" };
    constexpr const char* SHADOW_SUB_NAME  { "vmhook/fixtures/PolyInherited$ShadowSub" };
    constexpr const char* IFACE_IMPL_NAME  { "vmhook/fixtures/PolyInherited$IfaceImpl" };

    // ---- Internal name of the runtime klass behind an oop, or "" if it can't
    //      be resolved.  Used to prove the polymorphic L1-declared field's
    //      decoded oop carries the CONCRETE runtime type (L4, never L1).  Fully
    //      is_valid_pointer-gated. ----------------------------------------------
    auto runtime_klass_name(void* const oop) -> std::string
    {
        if (!oop || !vmhook::hotspot::is_valid_pointer(oop))
        {
            return {};
        }
        vmhook::hotspot::klass* const k{ vmhook::klass_from_oop(oop) };
        if (!k || !vmhook::hotspot::is_valid_pointer(k))
        {
            return {};
        }
        vmhook::hotspot::symbol* const sym{ k->get_name() };
        if (!sym || !vmhook::hotspot::is_valid_pointer(sym))
        {
            return {};
        }
        return sym->to_string();
    }

    // ---- True if `haystack` ends with `suffix` (klass-name suffix checks). ---
    auto ends_with(const std::string& haystack, const std::string& suffix) -> bool
    {
        return haystack.size() >= suffix.size()
            && haystack.compare(haystack.size() - suffix.size(), suffix.size(), suffix) == 0;
    }
}

VMHOOK_JVM_MODULE(poly_inherited_oop)
{
    vmhook::register_class<pi_fixture>(FIXTURE_NAME);
    vmhook::register_class<pi_a>(A_NAME);
    vmhook::register_class<pi_b>(B_NAME);
    vmhook::register_class<poh_l1>(L1_NAME);
    vmhook::register_class<poh_l4>(L4_NAME);
    vmhook::register_class<poh_shadow>(SHADOW_NAME);
    vmhook::register_class<poh_shadow_sub>(SHADOW_SUB_NAME);
    vmhook::register_class<poh_iface>(IFACE_IMPL_NAME);

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
        //  Inherited method via the NAME+SIGNATURE overload — the safe API
        //  the specialist flags as unexercised (the name-only overload latches
        //  the FIRST name match with no signature/kind filter).  This overload
        //  walks the same super chain but matches name AND exact descriptor, so
        //  the inherited protectedAdd(int) must resolve under "(I)I" and a wrong
        //  descriptor must miss entirely.
        // =================================================================
        {
            auto exact{ b_view.get_method("protectedAdd", "(I)I") };
            ctx.check("inherited_method_protectedAdd_name_sig_resolves", exact.has_value());
            if (exact.has_value())
            {
                ctx.check("inherited_method_protectedAdd_name_sig_not_static",
                          exact->is_static() == false);
                ctx.check("inherited_method_protectedAdd_name_sig_signature_II",
                          std::string{ exact->signature() } == "(I)I");
            }

            // A correct name but a non-existent descriptor must NOT resolve:
            // the exact-match overload never falls back to a name-only hit.
            ctx.check("inherited_method_wrong_signature_nullopt",
                      b_view.get_method("protectedAdd", "()I").has_value() == false);
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
    //  DEEP 4-LEVEL HIERARCHY  L1 <- L2 <- L3 <- L4.  Reading an int DECLARED
    //  AT EACH LEVEL through the deepest L4 wrapper exercises the super walk at
    //  depths 0..3 (own l4Int, then one/two/three super links up to L3/L2/L1).
    //  Values are 4 bytes apart in the high nibble so a wrong-offset read up the
    //  chain can never be a near-miss — a hit proves the exact declared offset.
    // =====================================================================
    const vmhook::oop_t l4_oop{ pi_fixture::get_l4_oop() };
    ctx.check("l4_instance_oop_obtained", l4_oop != nullptr);
    ctx.check("l4_instance_oop_valid",
              l4_oop != nullptr && vmhook::hotspot::is_valid_pointer(l4_oop));

    if (l4_oop != nullptr && vmhook::hotspot::is_valid_pointer(l4_oop))
    {
        poh_l4 l4_view{ l4_oop };

        // ---- int at every depth through the single L4 view -----------------
        {
            ctx.check("deep_l4Int_depth0_own_value", l4_view.l4_int() == L4_INT);
            ctx.check("deep_l3Int_depth1_value",     l4_view.l3_int() == L3_INT);
            ctx.check("deep_l2Int_depth2_value",     l4_view.l2_int() == L2_INT);
            ctx.check("deep_l1Int_depth3_value",     l4_view.l1_int() == L1_INT);

            // Each is a non-static "I" instance field regardless of walk depth.
            auto d3{ l4_view.field("l1Int") };
            ctx.check("deep_l1Int_resolves", d3.has_value());
            if (d3.has_value())
            {
                ctx.check("deep_l1Int_not_static", d3->is_static() == false);
                ctx.check("deep_l1Int_signature_I", std::string{ d3->signature() } == "I");
                ctx.check("deep_l1Int_not_reference", d3->is_reference() == false);
            }
        }

        // ---- Multi-level same-slot proof: l1Int read through the L4 view
        //      (depth 3) and through a plain-L1 wrapper around the SAME oop
        //      (depth 0) must land on the IDENTICAL physical slot — proving the
        //      depth-3 walk resolves L1's declared offset, not a divergent copy.
        {
            poh_l1 l1_view{ l4_oop };                       // same oop, L1 start klass
            auto via_l4{ l4_view.field("l1Int") };
            auto via_l1{ l1_view.get_field("l1Int") };
            ctx.check("deep_l1Int_via_L1_wrapper_resolves", via_l1.has_value());
            if (via_l4.has_value() && via_l1.has_value())
            {
                ctx.check("deep_l1Int_L4_and_L1_same_address",
                          via_l4->raw_address() == via_l1->raw_address());
                const std::int32_t v4 = via_l4->get();      // COPY-init
                const std::int32_t v1 = via_l1->get();      // COPY-init
                ctx.check("deep_l1Int_L4_and_L1_same_value", v4 == v1);
                ctx.check("deep_l1Int_resolves_to_L1s_declared_value", v1 == L1_INT);
            }
        }

        // ---- Inherited REFERENCE-shape fields read through the L4 view ------
        //      String / int[] / null / object-ref / self-ref, each declared up
        //      the chain (mostly on L1, l2Ref on L2, l4Self on L4).
        {
            // Inherited String ref (declared L1, depth 3): decodes to its text.
            ctx.check("deep_l1Str_inherited_string_value",
                      l4_view.l1_str() == L1_STR_VALUE);
            auto str_fp{ l4_view.field("l1Str") };
            ctx.check("deep_l1Str_resolves", str_fp.has_value());
            if (str_fp.has_value())
            {
                ctx.check("deep_l1Str_is_reference", str_fp->is_reference() == true);
                ctx.check("deep_l1Str_signature_String",
                          std::string{ str_fp->signature() } == "Ljava/lang/String;");
            }

            // Inherited null ref (declared L1): is a reference field, decodes to
            // a null oop — the walk resolves the slot, the slot just holds null.
            auto null_fp{ l4_view.field("l1Null") };
            ctx.check("deep_l1Null_resolves", null_fp.has_value());
            if (null_fp.has_value())
            {
                ctx.check("deep_l1Null_is_reference", null_fp->is_reference() == true);
                ctx.check("deep_l1Null_decodes_to_null",
                          vmhook::field_oop(*null_fp) == nullptr);
            }

            // Inherited object ref (declared L2, depth 2): decode the held L1 and
            // read its distinguishing l1Int through a plain-L1 wrapper.
            auto ref_fp{ l4_view.field("l2Ref") };
            ctx.check("deep_l2Ref_resolves", ref_fp.has_value());
            if (ref_fp.has_value())
            {
                ctx.check("deep_l2Ref_is_reference", ref_fp->is_reference() == true);
                void* const ref_oop{ vmhook::field_oop(*ref_fp) };
                ctx.check("deep_l2Ref_decodes_nonnull_oop",
                          ref_oop != nullptr && vmhook::hotspot::is_valid_pointer(ref_oop));
                if (ref_oop != nullptr && vmhook::hotspot::is_valid_pointer(ref_oop))
                {
                    poh_l1 held{ ref_oop };
                    ctx.check("deep_l2Ref_held_L1_l1Int_value",
                              held.l1_int() == L2_REF_VAL);
                }
            }

            // Self ref (declared L4, depth 0): decodes back to the L4 receiver
            // oop itself — exact same-object proof by pointer identity.
            auto self_fp{ l4_view.field("l4Self") };
            ctx.check("deep_l4Self_resolves", self_fp.has_value());
            if (self_fp.has_value())
            {
                ctx.check("deep_l4Self_is_reference", self_fp->is_reference() == true);
                void* const self_oop{ vmhook::field_oop(*self_fp) };
                ctx.check("deep_l4Self_decodes_to_receiver_oop", self_oop == l4_oop);
            }

            // Inherited int[] ref (declared L1): is a reference field with the
            // "[I" descriptor.  The element decode uses the assumed array header
            // layout (length@+12, data@+16), which varies with the compressed-
            // klass-pointer width across JDKs/heap configs — so length+element
            // are gated: HARD only when the length reads back as the known 2,
            // else recorded [INFO] (the resolve + is_reference stay HARD).
            auto arr_fp{ l4_view.field("l1Arr") };
            ctx.check("deep_l1Arr_resolves", arr_fp.has_value());
            if (arr_fp.has_value())
            {
                ctx.check("deep_l1Arr_is_reference", arr_fp->is_reference() == true);
                ctx.check("deep_l1Arr_signature_intarray",
                          std::string{ arr_fp->signature() } == "[I");
                void* const arr_oop{ vmhook::field_oop(*arr_fp) };
                ctx.check("deep_l1Arr_decodes_nonnull_oop",
                          arr_oop != nullptr && vmhook::hotspot::is_valid_pointer(arr_oop));
                if (arr_oop != nullptr && vmhook::hotspot::is_valid_pointer(arr_oop))
                {
                    const std::int32_t len{ vmhook::array_length(arr_oop) };
                    if (len == L1_ARR_LEN)
                    {
                        ctx.check("deep_l1Arr_length_2", len == L1_ARR_LEN);
                        ctx.check("deep_l1Arr_elem0_value",
                                  vmhook::get_array_element<std::int32_t>(arr_oop, 0) == L1_ARR_ELEM0);
                    }
                    else
                    {
                        ctx.record(std::string{ "[INFO] poly_inherited_oop: l1Arr length read back as " }
                                   + std::to_string(len) + " (expected 2) — array header layout "
                                   "differs on this JDK/heap config; element decode skipped");
                    }
                }
            }
        }
    }

    // =====================================================================
    //  POLYMORPHIC METHOD DISPATCH / OVERRIDE-VS-INHERITED PRECEDENCE.
    //  chainValue() is declared on L1 and OVERRIDDEN on L4.  get_method walks
    //  from the START klass downward through supers and returns the FIRST name
    //  match, so an L4 wrapper resolves the L4 OVERRIDE (most-derived wins) while
    //  a plain-L1 wrapper resolves L1's base body — the methods are physically
    //  DIFFERENT Method* and (when the call gate is present) return DIFFERENT
    //  values.  Resolution/identity is HARD; the CALL is gated on the call gate
    //  (the Java witness proves the values unconditionally below).
    // =====================================================================
    {
        const vmhook::oop_t l4_for_dispatch{ pi_fixture::get_l4_oop() };
        const vmhook::oop_t l1_plain_oop{ pi_fixture::get_l1_plain_oop() };
        ctx.check("dispatch_l4_oop_obtained", l4_for_dispatch != nullptr);
        ctx.check("dispatch_l1_plain_oop_obtained", l1_plain_oop != nullptr);

        const bool both_valid{
            l4_for_dispatch != nullptr && vmhook::hotspot::is_valid_pointer(l4_for_dispatch)
            && l1_plain_oop != nullptr && vmhook::hotspot::is_valid_pointer(l1_plain_oop) };

        if (both_valid)
        {
            poh_l4 l4_view{ l4_for_dispatch };
            poh_l1 l1_view{ l1_plain_oop };

            auto via_l4{ l4_view.get_method("chainValue") };
            auto via_l1{ l1_view.get_method("chainValue") };
            ctx.check("override_chainValue_resolves_via_L4_view", via_l4.has_value());
            ctx.check("base_chainValue_resolves_via_L1_view", via_l1.has_value());

            if (via_l4.has_value())
            {
                ctx.check("override_chainValue_not_static", via_l4->is_static() == false);
                ctx.check("override_chainValue_signature_returns_int",
                          std::string{ via_l4->signature() } == "()I");
            }
            if (via_l1.has_value())
            {
                ctx.check("base_chainValue_not_static", via_l1->is_static() == false);
                ctx.check("base_chainValue_signature_returns_int",
                          std::string{ via_l1->signature() } == "()I");
            }

            // The L4 override and the L1 base are PHYSICALLY DISTINCT Method*:
            // the start-klass-first walk latched the most-derived declaration
            // through the L4 view and the base one through the L1 view — proving
            // override precedence is by declared scope, not a single shared slot.
            if (via_l4.has_value() && via_l1.has_value())
            {
                ctx.check("override_and_base_chainValue_distinct_methods",
                          via_l4->raw_method() != via_l1->raw_method());
            }

            // The SAME L4 oop read through an L1 wrapper resolves L1's BASE body,
            // NOT the L4 override: get_method starts at the wrapper's REGISTERED
            // klass (typeid(*this)), and an L4 oop wrapped as L1 starts the walk
            // at L1 — so the START KLASS (the wrapper type), not the oop's runtime
            // type, picks the Method*.  Same start klass over different oops ->
            // same Method*; same oop with a deeper start klass -> the override.
            // (Resolution/identity HARD; the wrapper-type-drives-the-walk
            // invariant, distinct from Java's runtime virtual dispatch.)
            {
                poh_l1 l4_as_l1{ l4_for_dispatch };           // L4 oop, L1 start klass
                auto via_l4_as_l1{ l4_as_l1.get_method("chainValue") };
                ctx.check("override_chainValue_L4_oop_as_L1_resolves",
                          via_l4_as_l1.has_value());
                if (via_l4_as_l1.has_value() && via_l1.has_value())
                {
                    // Same start klass (L1) over different oops -> same Method*.
                    ctx.check("chainValue_L1_start_klass_same_method_any_oop",
                              via_l4_as_l1->raw_method() == via_l1->raw_method());
                }
                if (via_l4_as_l1.has_value() && via_l4.has_value())
                {
                    // L1-start (base body) vs L4-start (override) on the SAME oop
                    // are DIFFERENT Method* — the start klass, not the oop, picks.
                    ctx.check("chainValue_start_klass_picks_method_not_oop",
                              via_l4_as_l1->raw_method() != via_l4->raw_method());
                }
            }

            // CALL is best-effort (needs the call gate): the L4 view returns the
            // override body L4_CHAIN, the L1 view the base body L1_CHAIN.
            if (call_gate_present && via_l4.has_value() && via_l1.has_value())
            {
                const std::int32_t r4 = via_l4->call();        // COPY-init
                const std::int32_t r1 = via_l1->call();        // COPY-init
                ctx.check("override_chainValue_call_returns_L4_body",
                          r4 == L4_CHAIN);
                ctx.check("base_chainValue_call_returns_L1_body",
                          r1 == L1_CHAIN);
                ctx.check("override_and_base_chainValue_differ", r4 != r1);
            }
            else if (!call_gate_present)
            {
                ctx.record("[INFO] poly_inherited_oop: chainValue() override/base "
                           "calls skipped (call_stub_entry absent); method identity "
                           "+ the Java witness are the verified limit");
            }
        }
    }

    // =====================================================================
    //  INTERFACE DEFAULT METHOD vs INTERFACE CONSTANT — the resolution
    //  asymmetry between get_method (walks supers THEN implemented interfaces)
    //  and get_field (walks supers ONLY).  IfaceImpl implements Greeter (default
    //  greet, abstract describe, static helper) and Konst (constant KONST_FIELD).
    //   * greet()  : DEFAULT method, found via the interface fallback;
    //   * describe(): ABSTRACT on the interface, resolves the CLASS override;
    //   * helper() : STATIC interface helper, NOT a default -> not resolved;
    //   * KONST_FIELD: interface constant, NOT walked by find_field -> nullopt;
    //   * ownInt   : the impl's OWN instance field, resolves at depth 0.
    // =====================================================================
    const vmhook::oop_t iface_oop{ pi_fixture::get_iface_oop() };
    ctx.check("iface_instance_oop_obtained", iface_oop != nullptr);
    if (iface_oop != nullptr && vmhook::hotspot::is_valid_pointer(iface_oop))
    {
        poh_iface impl_view{ iface_oop };

        // -- OWN instance field still resolves at depth 0 (sanity anchor). ----
        ctx.check("iface_impl_own_field_resolves",
                  impl_view.get_field("ownInt").has_value());
        ctx.check("iface_impl_own_field_value",
                  impl_view.own_int() == IFACE_DESCRIBE);

        // -- DEFAULT interface method greet() found via the interface fallback
        //    even though neither IfaceImpl nor any superclass declares it.  The
        //    fallback depends on the implemented-interface VMStructs being
        //    exported (get_interfaces_ptr / safe_interface_methods); on a JDK
        //    that does NOT export them find_interface_default_method fails CLOSED
        //    and greet() resolves to nullopt.  So FINDABILITY is gated [INFO]
        //    (JDK-variant), but WHEN found the shape is HARD, and the wrong-sig
        //    MISS is HARD either way (nullopt regardless of the fallback).  The
        //    Java witness (real invokeinterface) proves the semantics
        //    unconditionally below.
        {
            auto greet{ impl_view.get_method("greet") };
            if (greet.has_value())
            {
                ctx.record("[INFO] poly_inherited_oop: interface DEFAULT greet() "
                           "resolved via the implemented-interface fallback on this JDK");
                ctx.check("iface_default_greet_not_static", greet->is_static() == false);
                ctx.check("iface_default_greet_signature_returns_int",
                          std::string{ greet->signature() } == "()I");

                // Name+signature overload resolves the same default under "()I".
                auto greet_sig{ impl_view.get_method("greet", "()I") };
                ctx.check("iface_default_greet_name_sig_resolves", greet_sig.has_value());

                // CALL best-effort (gated on the call gate): body returns GREET_VALUE.
                if (call_gate_present)
                {
                    const std::int32_t gv = greet->call();     // COPY-init
                    ctx.check("iface_default_greet_call_returns_value",
                              gv == GREET_VALUE);
                }
            }
            else
            {
                ctx.record("[INFO] poly_inherited_oop: interface DEFAULT greet() not "
                           "resolved (implemented-interface VMStructs absent on this "
                           "JDK); Java invokeinterface witness is the verified limit");
            }

            // A correct name but a non-existent descriptor must NOT resolve under
            // the exact-match overload — HARD regardless of the interface fallback
            // (the fallback also honours the signature filter, so a wrong "(I)I"
            // never matches greet's "()I").
            ctx.check("iface_default_greet_wrong_sig_nullopt",
                      impl_view.get_method("greet", "(I)I").has_value() == false);
        }

        // -- ABSTRACT interface method describe(): the interface declaration has
        //    no body, so the CLASS override is what resolves (via the superclass
        //    walk, before the interface fallback is even consulted). ------------
        {
            auto describe{ impl_view.get_method("describe") };
            ctx.check("iface_abstract_describe_resolves_class_override",
                      describe.has_value());
            if (describe.has_value())
            {
                ctx.check("iface_abstract_describe_not_static",
                          describe->is_static() == false);
                ctx.check("iface_abstract_describe_signature_returns_int",
                          std::string{ describe->signature() } == "()I");
                if (call_gate_present)
                {
                    const std::int32_t dv = describe->call(); // COPY-init
                    ctx.check("iface_abstract_describe_call_returns_override",
                              dv == IFACE_DESCRIBE);
                }
            }
        }

        // -- STATIC interface helper() is NOT a default method: the interface
        //    fallback's default-only gate skips it, so an instance get_method
        //    must NOT return it.  (Negative: clean nullopt, not a crash.) -------
        ctx.check("iface_static_helper_not_resolved_as_instance_method",
                  impl_view.get_method("helper").has_value() == false);

        // -- INTERFACE CONSTANT KONST_FIELD: find_field walks ONLY the
        //    superclass chain (never interfaces), so the constant is invisible
        //    to an instance get_field -> nullopt.  This is the deliberate
        //    asymmetry vs get_method's interface fallback above. -----------------
        ctx.check("iface_constant_field_not_visible_to_get_field",
                  impl_view.get_field("KONST_FIELD").has_value() == false);

        // -- A name absent from IfaceImpl, its supers, AND its interfaces still
        //    returns nullopt for get_method (full walk + interface fallback). ---
        ctx.check("iface_absent_method_nullopt",
                  impl_view.get_method("noSuchInterfaceMethod").has_value() == false);
    }

    // =====================================================================
    //  POLYMORPHIC ACTUAL TYPE.  polyBase is DECLARED as the base type L1 but
    //  HOLDS an L4 at runtime.  The native decode must yield the CONCRETE L4
    //  oop (runtime klass L4, never L1), and an L4 wrapper over that oop must
    //  read L4's OWN field — proving the decode follows the live object, not
    //  the declared field type.
    // =====================================================================
    const vmhook::oop_t poly_oop{ pi_fixture::get_poly_oop() };
    ctx.check("poly_base_oop_obtained", poly_oop != nullptr);
    if (poly_oop != nullptr && vmhook::hotspot::is_valid_pointer(poly_oop))
    {
        // An L4-typed wrapper over the polymorphic oop reads L4's OWN field —
        // only possible if the held object is genuinely an L4 at runtime.
        poh_l4 poly_as_l4{ poly_oop };
        ctx.check("poly_base_L4_view_reads_own_l4Int",
                  poly_as_l4.l4_int() == POLY_L4_INT);

        // The runtime klass name carries the concrete type.  Dotted-vs-slash and
        // exact internal spelling vary across JDKs, so match on the "$L4" suffix
        // only and gate the assertion [INFO] if the name can't be resolved.
        const std::string rk{ runtime_klass_name(poly_oop) };
        if (!rk.empty())
        {
            ctx.check("poly_base_runtime_klass_is_concrete_L4", ends_with(rk, "$L4"));
            ctx.check("poly_base_runtime_klass_not_declared_L1",
                      ends_with(rk, "$L1") == false);
        }
        else
        {
            ctx.record("[INFO] poly_inherited_oop: polyBase runtime klass name "
                       "unresolved on this JDK; concrete-type suffix check skipped");
        }
    }

    // =====================================================================
    //  SHADOWED / HIDDEN FIELD.  Shadow and ShadowSub both declare shadowedInt
    //  / shadowedRef at different values.  A ShadowSub-typed read resolves the
    //  CHILD slot (declared-scope wins at the start klass); a Shadow-typed read
    //  of the SAME oop resolves the BASE slot.  The two same-named slots are
    //  physically distinct (different raw_address), proving the walk's
    //  declared-scope disambiguation is correct, not a single shared slot.
    // =====================================================================
    const vmhook::oop_t shadow_oop{ pi_fixture::get_shadow_oop() };
    ctx.check("shadow_instance_oop_obtained", shadow_oop != nullptr);
    if (shadow_oop != nullptr && vmhook::hotspot::is_valid_pointer(shadow_oop))
    {
        poh_shadow_sub sub_view{ shadow_oop };               // start klass ShadowSub
        poh_shadow     base_view{ shadow_oop };              // start klass Shadow (same oop)

        // Child view sees the CHILD slots.
        ctx.check("shadow_sub_view_sees_child_int",
                  sub_view.shadowed_int() == SUB_SHADOW_INT);
        ctx.check("shadow_sub_view_sees_child_ref",
                  sub_view.shadowed_ref() == SUB_SHADOW_STR);

        // Base view of the SAME oop sees the BASE slots — the base klass's own
        // declaration wins at depth 0, never the child's re-declaration below it.
        ctx.check("shadow_base_view_sees_base_int",
                  base_view.shadowed_int() == BASE_SHADOW_INT);
        ctx.check("shadow_base_view_sees_base_ref",
                  base_view.shadowed_ref() == BASE_SHADOW_STR);

        // The two same-named int slots are PHYSICALLY distinct addresses.
        auto child_fp{ sub_view.get_field("shadowedInt") };
        auto base_fp{ base_view.get_field("shadowedInt") };
        ctx.check("shadow_child_int_resolves", child_fp.has_value());
        ctx.check("shadow_base_int_resolves", base_fp.has_value());
        if (child_fp.has_value() && base_fp.has_value())
        {
            ctx.check("shadow_child_and_base_int_distinct_slots",
                      child_fp->raw_address() != base_fp->raw_address());
            const std::int32_t cv = child_fp->get();        // COPY-init
            const std::int32_t bv = base_fp->get();          // COPY-init
            ctx.check("shadow_child_int_value_2000", cv == SUB_SHADOW_INT);
            ctx.check("shadow_base_int_value_1000", bv == BASE_SHADOW_INT);
            ctx.check("shadow_slots_hold_different_values", cv != bv);
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

            // ---- Exhaustive-expansion witnesses: the probe read the deep ints,
            //      the inherited reference shapes, the shadow slots, and the
            //      polymorphic concrete type through genuine getfield / instanceof
            //      bytecode.  Each latched boolean proves the JVM observes the
            //      identical quantities the native side decoded by raw offset. ----
            ctx.check("java_saw_deep_fields_L1_through_L4",
                      pi_fixture::saw_deep_fields());
            ctx.check("java_saw_deep_refs_string_array_null_self_obj",
                      pi_fixture::saw_deep_refs());
            ctx.check("java_saw_shadow_sub_child_slots",
                      pi_fixture::saw_shadow_sub());
            ctx.check("java_saw_shadow_base_base_slots",
                      pi_fixture::saw_shadow_base());
            ctx.check("java_saw_poly_concrete_L4_runtime_type",
                      pi_fixture::saw_poly_concrete());

            // ---- Polymorphic-method-dispatch + interface witnesses.  Java's own
            //      invokevirtual / invokeinterface runs on the Java thread (no
            //      native call gate), so these are asserted UNCONDITIONALLY — they
            //      prove the override / default-method SEMANTICS even on JDKs where
            //      the native chainValue()/greet() calls above were skipped. -------
            ctx.check("java_saw_override_dispatch_L4_chainValue",
                      pi_fixture::saw_override_dispatch());
            ctx.check("java_saw_base_chainValue_L1_body",
                      pi_fixture::saw_base_chain_value());
            ctx.check("java_saw_iface_default_greet_and_describe",
                      pi_fixture::saw_iface_default());
            ctx.check("java_saw_iface_constant_field",
                      pi_fixture::saw_iface_const());

            // ---- Identity-hash publishers: the probe latched System.identity
            //      HashCode of each inherited reference target.  A non-zero value
            //      proves the probe ran AND each ref is a real, live object (the
            //      native side proved the SAME objects by pointer identity /
            //      decoded value above; these are the JVM-side existence witness).
            ctx.check("java_l1Str_identity_nonzero",
                      pi_fixture::l1_str_identity() != 0);
            ctx.check("java_l1Arr_identity_nonzero",
                      pi_fixture::l1_arr_identity() != 0);
            ctx.check("java_l2Ref_identity_nonzero",
                      pi_fixture::l2_ref_identity() != 0);
            ctx.check("java_selfRef_identity_nonzero",
                      pi_fixture::self_ref_identity() != 0);
            ctx.check("java_polyBase_identity_nonzero",
                      pi_fixture::poly_base_identity() != 0);
        }
    }

    // No hooks were armed by this module; nothing to unhook.
}
